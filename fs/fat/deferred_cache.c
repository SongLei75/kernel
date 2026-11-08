// SPDX-License-Identifier: GPL-2.0
/*
 * Wrappers retain the original page cache. Only the management bit is added.
 */
#include <linux/capability.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/page-flags.h>

#include "deferred_cache.h"

static DEFINE_MUTEX(dc_mode_lock);

static void dc_track_page(struct page *page)
{
	if (page)
		SetPageOwnerPriv1(page);
}

int dc_cont_write_begin(struct file *file, struct address_space *mapping,
			loff_t pos, unsigned int len, struct page **pagep,
			void **fsdata, get_block_t *get_block, loff_t *bytes)
{
	int ret = cont_write_begin(file, mapping, pos, len, pagep, fsdata,
			       get_block, bytes);

	if (!ret)
		dc_track_page(*pagep);
	return ret;
}
EXPORT_SYMBOL_GPL(dc_cont_write_begin);

struct buffer_head *dc_sb_bread(struct super_block *sb, sector_t block)
{
	struct buffer_head *bh = sb_bread(sb, block);

	if (bh)
		dc_track_page(bh->b_page);
	return bh;
}
EXPORT_SYMBOL_GPL(dc_sb_bread);

struct buffer_head *dc_sb_getblk(struct super_block *sb, sector_t block)
{
	struct buffer_head *bh = sb_getblk(sb, block);

	if (bh)
		dc_track_page(bh->b_page);
	return bh;
}
EXPORT_SYMBOL_GPL(dc_sb_getblk);

struct buffer_head *dc_sb_find_get_block(struct super_block *sb, sector_t block)
{
	struct buffer_head *bh = sb_find_get_block(sb, block);

	if (bh)
		dc_track_page(bh->b_page);
	return bh;
}
EXPORT_SYMBOL_GPL(dc_sb_find_get_block);

/* mmap may bypass write_begin; retain the ordinary dirty_folio behavior. */
bool dc_block_dirty_folio(struct address_space *mapping, struct folio *folio)
{
	dc_track_page(&folio->page);
	return block_dirty_folio(mapping, folio);
}
EXPORT_SYMBOL_GPL(dc_block_dirty_folio);

bool dc_mode_enabled(struct super_block *sb)
{
	return READ_ONCE(sb->s_iflags) & SB_I_DEFERRED_CACHE;
}
EXPORT_SYMBOL_GPL(dc_mode_enabled);

int dc_set_mode(struct super_block *sb, bool enabled)
{
	int ret, thaw_ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (sb_rdonly(sb))
		return -EROFS;

	mutex_lock(&dc_mode_lock);
	if (enabled) {
		/* The caller enables the cache before any writes are in flight. */
		set_bit(SB_I_DEFERRED_CACHE_BIT, &sb->s_iflags);
		ret = 0;
	} else {
		/*
		 * Release the writeback gate first. freeze_super() then drains
		 * active writers and syncs the filesystem using its native paths.
		 * Repeated disable requests can retry a failed sync.
		 */
		clear_bit(SB_I_DEFERRED_CACHE_BIT, &sb->s_iflags);
		ret = freeze_super(sb);
		if (!ret) {
			thaw_ret = thaw_super(sb);
			if (!ret)
				ret = thaw_ret;
		}
	}
	mutex_unlock(&dc_mode_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(dc_set_mode);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Page acquisition wrappers and deferred writeback control");
