# vfat 延迟写回设计（本地实现草稿）

状态：**本地实现草稿已写入工作区，未编译、未测试、未提交、未推送**。目标基线：Linux 6.1.158 + PREEMPT_RT rt58；参考配置：`configs/company-arm64-linux-6.1.158.config`。本文件不代表功能已经验证。

## 1. 目标与边界

用户态开启 vfat 延迟写回模式，在正常后台写回和 Buffer Head 同步路径中延迟设备写入。根据产品取舍，不拦截内存压力下的 vmscan 回收写盘，因此缓存期间不保证绝对零落盘。关闭时尝试集中同步并报告错误。

沿用 Linux 的 Page Cache（页缓存）、Folio（页缓存单元）、XArray（索引结构）、buffer_head（块缓冲头）及 Dirty（脏页）管理。缓存时间和数据量有限，不引入独立数据副本、通用事务系统或新的 Page/BH 状态位。

本功能不承诺断电保留纯内存数据、跨多个磁盘块的原子提交，以及原始磁盘设备被外部写入者并发访问时的透明性。启用模式时的 `fsync/sync` 成功是“逻辑成功”，不代表介质持久化。

## 2. 核心状态：复用现有 Dirty + OwnerPriv1

### 2.1 普通数据 Page/Folio

- 保留原生 `PG_dirty` 和 `PAGECACHE_TAG_DIRTY`，以维持内核脏页索引、计数和回收保护。
- 复用 `PG_owner_priv_1` 表示“本轮延迟回写”。**不新增 `PG_delayed_dirty`。**
- `PageDirty=1 && PageOwnerPriv1=1`：需要保留内容，但当前不许写盘。
- `PageDirty=1 && PageOwnerPriv1=0`：普通脏页，允许原生回写。
- 对同一 Page 的多个 `buffer_head` 仍保留原有 `BH_Dirty`，不新增 `BH_Deferred`。是否延迟由所在 Page 的 `PG_owner_priv_1` 与目标文件系统的模式共同决定。

`PG_owner_priv_1` 是按拥有者复用的位，并非全局空闲位：必须仅对经过确认的 FAT 文件页和其设备缓存页赋予本语义，判断时同时核验归属与模式。

### 2.2 目标文件系统开关

- 模式状态按 `struct super_block` 实例保存。可用专用的内部超级块状态或现有可用的内部标志机制，**不额外申请 Page Flag 或 BH Flag**。
- 普通文件页通过 `mapping->host->i_sb` 确认归属。
- 元数据 `buffer_head` 的 `bh->b_page->mapping` 往往属于 `bdev->bd_inode->i_mapping`，不能直接使用其 `mapping->host->i_sb` 当作 FAT 的超级块。针对受控块设备需确认 `bh->b_bdev` 与目标已挂载超级块对应。
- 同一个超级块被 bind mount 多次时，以同一个文件系统实例为作用域，而不是以目录字符串为作用域。

## 3. 标记入口（保持原生标脏）

| 数据类别 | 实际基线入口 | 设计处理 |
| --- | --- | --- |
| 普通 `write()` | `fs/fat/inode.c:fat_write_end` → `generic_write_end` → `block_write_end` → `__block_commit_write` → `mark_buffer_dirty` | 原样标 `BH_Dirty/PG_dirty`，在标脏前同步设置 `PG_owner_priv_1` |
| `mmap(MAP_SHARED)` 修改 | `fat_aops.dirty_folio = fat_dirty_folio` | 保持原生 Dirty，并设置 `PG_owner_priv_1` |
| FAT 表更新 | `fs/fat/fatent.c` → `mark_buffer_dirty_inode` | 识别所属受控设备页，原样标 BH/Page Dirty，并设置 OwnerPriv1 |
| 目录项创建、删除、重命名 | `fs/fat/dir.c`、`namei_vfat.c` 等使用 `mark_buffer_dirty_inode`、`mark_buffer_dirty` | 与 FAT 表一致 |
| 文件大小、时间戳、起始簇 | `mark_inode_dirty` → `fat_write_inode` → `mark_buffer_dirty` | 保留 inode Dirty 流程；最终目录项 BH 走相同标记 |
| FSInfo | `mark_fsinfo_dirty` → `fat_write_inode` → `fat_clusters_flush` | 保留 `I_DIRTY_SYNC`，最终 BH 走相同标记 |

关键点：`write_begin` 只是获取/准备页，标脏发生于后续提交路径。不要在 `fat_write_end` 返回之后“反向清 Dirty”；在真正标脏入口完成联合设置，避免写回与标记之间的竞态。还要处理 `mark_buffer_dirty` 对“已 Dirty BH”直接返回的快速路径，确保延迟标记不会因此漏置。

FAT 的 `.dirty_folio` 回调设置文件页标记，通用 `mark_buffer_dirty()` 设置 Buffer Head 所在页标记，仍保留原生 Dirty 语义。

## 4. 拦截真实回写（仅跳过，不能把页伪装成 Clean）

| 文件与函数 | 改动方向 |
| --- | --- |
| `mm/page-writeback.c:write_cache_pages` | 在 `clear_page_dirty_for_io()` **之前**检查受控延迟页。命中时解锁并跳过，不消耗脏状态、不下发 `writepage` |
| `fs/buffer.c:write_dirty_buffer` | 在清 `BH_Dirty` 前检查受控延迟 BH 所在 Page；命中时返回但不提交 I/O |
| `fs/buffer.c:__sync_dirty_buffer` | 同上，返回逻辑成功，保留 `BH_Dirty` |
| `block/fops.c:blkdev_writepage/writepages` | 应经由通用 Page 回写/回收闸门；验证不存在绕过路径 |
| `fs/fat/inode.c:fat_writepage/writepages` | 应经由通用闸门；尽量不复制回写代码 |

这几处是可复用的通用层改动，不打算在 `fat_sync_bhs`、`fat_mirror_bhs` 等每个调用点分别插入缓存判断。现有 `fat_sync_bhs()` 直接调用 `write_dirty_buffer()`，所以只拦 `sync_dirty_buffer()` 不够；`fsync_buffers_list()` 也会使用 `write_dirty_buffer()`。

缓存保护范围限于已拦截的正常写回与 Buffer Head 路径；不拦截 `vmscan`，也不额外处理产品不会触发的 FITRIM、discard、remount 与卸载过程。

## 5. 用户态开关与提交

### 开启

1. 通过 vfat 的目录 ioctl 指定文件系统实例；校验权限和模式状态。
2. 产品保证开启前没有在途写入，开启时直接置位，不调用 `freeze_super()`。
3. 原子切换到缓存模式；后续标脏保持 `PG_dirty` 并设置 `PG_owner_priv_1`。普通 `fsync/sync/syncfs` 无实际持久化写入。

### 关闭

1. 关闭时机不确定、可能存在并发写入，因此暂时保留 `freeze_super()` 隔离；该函数会先在延迟模式仍开启时执行一次同步。
2. 收敛需要序列化的 inode 元数据，解除超级块的延迟模式开关，让原生写回逻辑允许回写全部仍标脏的 Page/BH。目前未逐页清除旧 `PG_owner_priv_1` 位；回写判断同时要求超级块处于延迟模式，因此关闭后不会被该位阻止落盘。
3. 解除延迟拦截后走原生 `sync_filesystem(sb)`，等待设备回写并检查错误。调用须符合 `s_umount` 等锁条件，不能直接在普通文件 ioctl 中随意调用。
4. 成功后恢复普通模式并放行写入；出错时保留尚未成功写入的脏状态并向用户态返回错误，不谎称已提交。

注：必须保证 `PG_owner_priv_1` 最终在被释放、失效或复用前清理。`truncate/unlink` 是用户主动删除的语义，不能通过“永远不允许截断脏页”简单屏蔽；提交前的失效与 inode 回收应有针对性检查。

### Direct I/O / 同步语义

- 缓存开启期间，同一 FAT 实例的 `O_DIRECT` **读和写**都要走 Buffered I/O，确保读取最新缓存内容，不让 Direct I/O 删除脏缓存。只在 `fat_direct_IO` 返回 `0` 不足以覆盖上层 `generic_file_direct_write` 的失效流程。
- 普通 `fsync/fdatasync/sync/syncfs/msync/O_SYNC` 依赖通用写回闸门阻止真实写入；必要时仅在 FAT 文件接口做小范围配合，不能改变其他文件系统。
- `discard/FITRIM`、直接块设备访问、卸载/重新挂载与异常电源事件须明确处理策略；不引入独立的持久事务系统。

## 6. 预计修改点（实施前）

- **通用状态/接口**：`include/linux/page-flags.h`（仅复用已有 OwnerPriv1 API）、`include/linux/fs.h`（文件系统实例开关/接口，具体方式待实现）；不新增 Page/BH 位。
- **通用标脏与 BH**：`fs/buffer.c`、必要时 `include/linux/buffer_head.h`。
- **通用回写**：`mm/page-writeback.c`；不改 `mm/vmscan.c`。
- **FAT 适配及 ioctl**：`fs/fat/file.c`、`fs/fat/inode.c` 和私有 `fs/fat/fat.h`。

本地工作区已按上述主要改动点编写实现；尚未执行编译或测试。任何额外的内核插桩必须对应到已确认的绕过路径。

## 7. 最小验证矩阵（后续实施时）

1. `write/read`、覆盖写、扩容、截断、删除后重建：缓存期间可读最新内容。
2. `mkdir/rename/unlink`、长文件名、FAT 链和 FSInfo：缓存期间不写物理介质。
3. `mmap(MAP_SHARED)`、`msync`：内容可见且不提前落盘。
4. `O_DIRECT` 读写与 `O_SYNC`、`fsync/fdatasync/sync/syncfs`：不发生旁路写入。
5. 后台 Flusher 跳过受控延迟页；内存压力下 `vmscan` 可能提前写盘。
6. 禁用缓存后：数据和元数据落盘，重新挂载验证结构完整。
7. 用独立测试镜像和块 I/O trace 检查真实写入；禁止直接在公司产品分区上做破坏性测试。

当前选择：**保留 `PG_dirty` + 复用 `PG_owner_priv_1`，保留 `BH_Dirty`，不新增 Page/BH 标志位**。先沿这条路径验证，不采用“只置特殊标志、不置 Dirty”的方案。

## 8. 当前本地草稿与已知限制

仅本地工作区修改，**未提交、未推送、未编译、未测试**。公开 GitHub 的 `main` 仍是未实现功能的基线提交。

- 内部 ioctl：`FAT_IOCTL_SET_DEFERRED_WRITEBACK` 定义于 `fs/fat/fat.h`，在 vfat 文件或目录 FD 上以指向 `__u32` 的参数调用，`1` 开启、`0` 关闭；要求 `CAP_SYS_ADMIN`。用户态需自行使用一致的命令编号。
- 超级块状态：`SB_I_DEFERRED_WRITEBACK`；开启前保证无写入并直接置位；关闭时使用 `freeze_super/thaw_super` 和 `sync_filesystem`。
- 文件数据与设备 BH：保留 `PG_dirty/BH_Dirty`，用 `PG_owner_priv_1` 标注延迟修改。正常通用回写和 BH 同步跳过写盘，但 `vmscan` 允许内存压力下写盘。
- vfat 适配：`O_DIRECT` 读写尽量回退到缓存路径；`fsync` 返回逻辑成功。不再额外处理 FITRIM、discard、remount 或卸载。
- 已知限制：无法保证掉电保存纯内存数据；不能保证多扇区原子提交；原始块设备被直接访问时不在文件系统透明性承诺范围内；实际回写路径及锁序还需在后续获得测试授权时验证；页脏比例过高会触发普通 Dirty 限速。
- 当前复用的 `PG_owner_priv_1` 在关闭模式后不会额外扫描逐页清理，必须注意其仍存在于当前内存页直至生命周期结束（回写判断需同时检查 SB 开关）。
