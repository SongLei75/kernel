/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _FAT_DEFERRED_CACHE_H
#define _FAT_DEFERRED_CACHE_H

#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/pagemap.h>

#define DC_IOCTL_SET_MODE _IOW('r', 0x30, __u32)

int dc_cont_write_begin(struct file *file, struct address_space *mapping,
			loff_t pos, unsigned int len, struct page **pagep,
			void **fsdata, get_block_t *get_block, loff_t *bytes);
struct buffer_head *dc_sb_bread(struct super_block *sb, sector_t block);
struct buffer_head *dc_sb_getblk(struct super_block *sb, sector_t block);
struct buffer_head *dc_sb_find_get_block(struct super_block *sb, sector_t block);
bool dc_block_dirty_folio(struct address_space *mapping, struct folio *folio);
bool dc_mode_enabled(struct super_block *sb);
int dc_set_mode(struct super_block *sb, bool enabled);

#endif
