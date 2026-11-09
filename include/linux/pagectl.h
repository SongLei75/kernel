/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PAGECTL_H
#define _LINUX_PAGECTL_H

#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/pagemap.h>

#define PAGECTL_IOCTL_SET_MODE	_IOW('p', 0x41, __u32)

/* Both offsets are inclusive. DATA uses file bytes; BUFFER uses block numbers. */
struct pagectl_flush_request {
	__u64 start;
	__u64 end;
	__u32 kind;
	__u32 reason;
};
#define PAGECTL_FLUSH_DATA	0
#define PAGECTL_FLUSH_BUFFER	1
#define PAGECTL_IOCTL_FLUSH	_IOW('p', 0x42, struct pagectl_flush_request)

enum pagectl_flush_reason {
	PAGECTL_FLUSH_USER,
	PAGECTL_FLUSH_POLICY,
	PAGECTL_FLUSH_PRESSURE,
};

/* The controller has no knowledge of the owning filesystem. */
bool pagectl_enabled(struct super_block *sb);
bool pagectl_has_pending(struct super_block *sb);
int pagectl_set_mode(struct super_block *sb, bool enabled);
void pagectl_shutdown(struct super_block *sb);

struct page *pagectl_grab_cache_page_write_begin(struct address_space *mapping,
						  pgoff_t index);
struct folio *pagectl_filemap_get_folio(struct address_space *mapping,
				       pgoff_t index, int fgp_flags,
				       gfp_t gfp);
struct buffer_head *pagectl_sb_bread(struct super_block *sb, sector_t block);
struct buffer_head *pagectl_sb_getblk(struct super_block *sb, sector_t block);
struct buffer_head *pagectl_sb_find_get_block(struct super_block *sb,
					     sector_t block);

void pagectl_defer_folio(struct folio *folio);
void pagectl_mark_buffer_dirty(struct buffer_head *bh);
void pagectl_mark_buffer_dirty_inode(struct buffer_head *bh,
				     struct inode *inode);
void pagectl_invalidate_folio(struct folio *folio, size_t offset, size_t length);

int pagectl_flush_folios(struct super_block *sb, struct address_space *mapping,
			 loff_t start, loff_t end,
			 enum pagectl_flush_reason reason);
int pagectl_flush_buffers(struct super_block *sb, sector_t start,
			  sector_t end, enum pagectl_flush_reason reason);
int pagectl_cont_write_begin(struct file *file, struct address_space *mapping,
			      loff_t pos, unsigned int len, struct page **pagep,
			      void **fsdata, get_block_t *get_block,
			      loff_t *bytes);
int pagectl_generic_write_end(struct file *file, struct address_space *mapping,
			      loff_t pos, unsigned int len, unsigned int copied,
			      struct page *page, void *fsdata);
#endif
