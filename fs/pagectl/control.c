// SPDX-License-Identifier: GPL-2.0
/*
 * Native page-cache deferred dirtying, deliberately independent of any
 * filesystem.  A deferred page is retained but is not marked PG_dirty.
 */
#include <linux/capability.h>
#include <linux/hash.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/pagectl.h>

#define PAGECTL_HASH_BITS 8
#define PAGECTL_HASH_SIZE (1U << PAGECTL_HASH_BITS)

enum pagectl_kind {
	PAGECTL_FOLIO,
	PAGECTL_BUFFER,
};

struct pagectl_entry {
	struct list_head list;
	struct hlist_node hash;
	enum pagectl_kind kind;
	struct folio *folio;
	struct buffer_head *bh;
	struct address_space *mapping;
	pgoff_t index;
	sector_t block;
};

struct pagectl_context {
	struct list_head list;
	struct list_head pending;
	struct hlist_head index[PAGECTL_HASH_SIZE];
	struct super_block *sb;
	bool enabled;
};

/* Protects contexts, pending entries, and mode transitions. */
static DEFINE_SPINLOCK(pagectl_lock);
static DEFINE_MUTEX(pagectl_mode_mutex);
static LIST_HEAD(pagectl_contexts);

static unsigned int pagectl_bucket(struct folio *folio, struct buffer_head *bh)
{
	return hash_ptr(bh ? (void *)bh : (void *)folio, PAGECTL_HASH_BITS);
}

static void pagectl_take_entry(struct pagectl_entry *entry,
			       struct list_head *released)
{
	hlist_del_init(&entry->hash);
	list_move_tail(&entry->list, released);
}

static void pagectl_take_all(struct pagectl_context *ctx,
			     struct list_head *released)
{
	struct pagectl_entry *entry, *next;

	list_for_each_entry_safe(entry, next, &ctx->pending, list)
		pagectl_take_entry(entry, released);
}

static struct pagectl_context *pagectl_find(struct super_block *sb)
{
	struct pagectl_context *ctx;

	list_for_each_entry(ctx, &pagectl_contexts, list)
		if (ctx->sb == sb)
			return ctx;
	return NULL;
}

bool pagectl_enabled(struct super_block *sb)
{
	struct pagectl_context *ctx;
	unsigned long flags;
	bool enabled;

	spin_lock_irqsave(&pagectl_lock, flags);
	ctx = pagectl_find(sb);
	enabled = ctx && ctx->enabled;
	spin_unlock_irqrestore(&pagectl_lock, flags);
	return enabled;
}
EXPORT_SYMBOL_GPL(pagectl_enabled);

bool pagectl_has_pending(struct super_block *sb)
{
	struct pagectl_context *ctx;
	unsigned long flags;
	bool pending;

	spin_lock_irqsave(&pagectl_lock, flags);
	ctx = pagectl_find(sb);
	pending = ctx && !list_empty(&ctx->pending);
	spin_unlock_irqrestore(&pagectl_lock, flags);
	return pending;
}
EXPORT_SYMBOL_GPL(pagectl_has_pending);

static void pagectl_release_entry(struct pagectl_entry *entry, bool discard)
{
	if (entry->kind == PAGECTL_FOLIO) {
		struct folio *folio = entry->folio;

		if (!discard) {
			folio_lock(folio);
			if (folio_mapping(folio) == entry->mapping)
				folio_mark_dirty(folio);
			folio_unlock(folio);
		}
		folio_put(folio);
	} else {
		if (!discard)
			mark_buffer_dirty(entry->bh);
		brelse(entry->bh);
	}
	kfree(entry);
}

static void pagectl_release_list(struct list_head *list, bool discard)
{
	struct pagectl_entry *entry, *next;

	list_for_each_entry_safe(entry, next, list, list) {
		list_del(&entry->list);
		pagectl_release_entry(entry, discard);
	}
}

/*
 * Disabling is a handoff to normal dirty accounting, not an immediate sync.
 * No free/truncate/fsync path may discard outstanding changes silently.
 */
int pagectl_set_mode(struct super_block *sb, bool enabled)
{
	struct pagectl_context *ctx, *new_ctx = NULL;
	unsigned long flags;
	LIST_HEAD(released);

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (sb_rdonly(sb))
		return -EROFS;

	if (enabled) {
		new_ctx = kzalloc(sizeof(*new_ctx), GFP_KERNEL);
		if (!new_ctx)
			return -ENOMEM;
		INIT_LIST_HEAD(&new_ctx->pending);
		{
			unsigned int i;

			for (i = 0; i < PAGECTL_HASH_SIZE; i++)
				INIT_HLIST_HEAD(&new_ctx->index[i]);
		}
		new_ctx->sb = sb;
	}

	mutex_lock(&pagectl_mode_mutex);
	spin_lock_irqsave(&pagectl_lock, flags);
	ctx = pagectl_find(sb);
	if (enabled && !ctx) {
		ctx = new_ctx;
		new_ctx = NULL;
		list_add(&ctx->list, &pagectl_contexts);
	}
	if (ctx) {
		ctx->enabled = enabled;
		if (!enabled)
			pagectl_take_all(ctx, &released);
	}
	spin_unlock_irqrestore(&pagectl_lock, flags);

	kfree(new_ctx);
	pagectl_release_list(&released, false);
	mutex_unlock(&pagectl_mode_mutex);
	return 0;
}
EXPORT_SYMBOL_GPL(pagectl_set_mode);

void pagectl_shutdown(struct super_block *sb)
{
	struct pagectl_context *ctx;
	unsigned long flags;
	LIST_HEAD(released);

	/* Called by the filesystem while its superblock is still alive. */
	mutex_lock(&pagectl_mode_mutex);
	spin_lock_irqsave(&pagectl_lock, flags);
	ctx = pagectl_find(sb);
	if (ctx) {
		ctx->enabled = false;
		pagectl_take_all(ctx, &released);
		list_del(&ctx->list);
	}
	spin_unlock_irqrestore(&pagectl_lock, flags);

	pagectl_release_list(&released, false);
	kfree(ctx);
	mutex_unlock(&pagectl_mode_mutex);
}
EXPORT_SYMBOL_GPL(pagectl_shutdown);

static bool pagectl_defer(struct super_block *sb, struct folio *folio,
			  struct buffer_head *bh)
{
	struct pagectl_context *ctx;
	struct pagectl_entry *entry, *item;
	unsigned int bucket;
	unsigned long flags;
	bool deferred = false;

	/* These hooks can run under a buffer or page lock. */
	entry = kzalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry)
		return false;

	entry->folio = folio;
	entry->bh = bh;
	entry->kind = bh ? PAGECTL_BUFFER : PAGECTL_FOLIO;
	entry->mapping = folio_mapping(folio);
	entry->index = folio->index;
	entry->block = bh ? bh->b_blocknr : 0;
	bucket = pagectl_bucket(folio, bh);

	spin_lock_irqsave(&pagectl_lock, flags);
	ctx = pagectl_find(sb);
	if (ctx && ctx->enabled) {
		hlist_for_each_entry(item, &ctx->index[bucket], hash) {
			if (item->kind == entry->kind &&
			    (bh ? item->bh == bh : item->folio == folio)) {
				deferred = true;
				goto out;
			}
		}
		if (bh)
			get_bh(bh);
		else
			folio_get(folio);
		hlist_add_head(&entry->hash, &ctx->index[bucket]);
		list_add_tail(&entry->list, &ctx->pending);
		entry = NULL;
		deferred = true;
	}
out:
	spin_unlock_irqrestore(&pagectl_lock, flags);
	kfree(entry);
	return deferred;
}

void pagectl_defer_folio(struct folio *folio)
{
	struct address_space *mapping = folio_mapping(folio);

	if (!mapping || !mapping->host ||
	    !pagectl_defer(mapping->host->i_sb, folio, NULL))
		folio_mark_dirty(folio);
}
EXPORT_SYMBOL_GPL(pagectl_defer_folio);

void pagectl_mark_buffer_dirty(struct buffer_head *bh)
{
	struct super_block *sb = bh->b_bdev ? bh->b_bdev->bd_super : NULL;

	if (!sb || !pagectl_defer(sb, page_folio(bh->b_page), bh))
		mark_buffer_dirty(bh);
}
EXPORT_SYMBOL_GPL(pagectl_mark_buffer_dirty);

/*
 * Keep the native inode<->buffer association even while deferring BH_Dirty.
 * This allows normal sync_mapping_buffers() to work after handoff.
 */
void pagectl_mark_buffer_dirty_inode(struct buffer_head *bh,
				     struct inode *inode)
{
	struct address_space *mapping = inode->i_mapping;
	struct address_space *buffer_mapping = bh->b_page->mapping;

	if (!pagectl_defer(inode->i_sb, page_folio(bh->b_page), bh)) {
		mark_buffer_dirty_inode(bh, inode);
		return;
	}
	if (!mapping->private_data)
		mapping->private_data = buffer_mapping;
	else
		BUG_ON(mapping->private_data != buffer_mapping);
	if (!bh->b_assoc_map) {
		spin_lock(&buffer_mapping->private_lock);
		list_move_tail(&bh->b_assoc_buffers, &mapping->private_list);
		bh->b_assoc_map = mapping;
		spin_unlock(&buffer_mapping->private_lock);
	}
}
EXPORT_SYMBOL_GPL(pagectl_mark_buffer_dirty_inode);

static int pagectl_collect(struct super_block *sb, bool data,
			   struct address_space *mapping,
			   unsigned long long start, unsigned long long end,
			   struct list_head *released)
{
	struct pagectl_context *ctx;
	struct pagectl_entry *entry, *next;
	unsigned long flags;
	int nr = 0;

	spin_lock_irqsave(&pagectl_lock, flags);
	ctx = pagectl_find(sb);
	if (ctx) {
		list_for_each_entry_safe(entry, next, &ctx->pending, list) {
			if (data != (entry->kind == PAGECTL_FOLIO))
				continue;
			if (data && entry->mapping != mapping)
				continue;
			if ((data ? entry->index : entry->block) < start ||
			    (data ? entry->index : entry->block) > end)
				continue;
			pagectl_take_entry(entry, released);
			nr++;
		}
	}
	spin_unlock_irqrestore(&pagectl_lock, flags);
	return nr;
}

int pagectl_flush_folios(struct super_block *sb, struct address_space *mapping,
			 loff_t start, loff_t end,
			 enum pagectl_flush_reason reason)
{
	LIST_HEAD(released);
	int nr;

	if (start < 0 || end < start)
		return -EINVAL;
	nr = pagectl_collect(sb, true, mapping, start >> PAGE_SHIFT,
			     end >> PAGE_SHIFT, &released);
	pagectl_release_list(&released, false);
	return nr;
}
EXPORT_SYMBOL_GPL(pagectl_flush_folios);

int pagectl_flush_buffers(struct super_block *sb, sector_t start,
			  sector_t end, enum pagectl_flush_reason reason)
{
	LIST_HEAD(released);
	int nr;

	if (end < start)
		return -EINVAL;
	nr = pagectl_collect(sb, false, NULL, start, end, &released);
	pagectl_release_list(&released, false);
	return nr;
}
EXPORT_SYMBOL_GPL(pagectl_flush_buffers);

void pagectl_invalidate_folio(struct folio *folio, size_t offset, size_t length)
{
	struct pagectl_context *ctx;
	struct pagectl_entry *entry, *next;
	unsigned long flags;
	LIST_HEAD(released);
	bool discard = offset == 0 && length >= folio_size(folio);

	spin_lock_irqsave(&pagectl_lock, flags);
	list_for_each_entry(ctx, &pagectl_contexts, list) {
		list_for_each_entry_safe(entry, next, &ctx->pending, list) {
			if (entry->folio == folio &&
			    entry->kind == PAGECTL_FOLIO)
				pagectl_take_entry(entry, &released);
		}
	}
	spin_unlock_irqrestore(&pagectl_lock, flags);

	/* Invalidation runs with folio locked. Avoid locking it again. */
	if (!discard && !list_empty(&released))
		folio_mark_dirty(folio);
	pagectl_release_list(&released, true);
}
EXPORT_SYMBOL_GPL(pagectl_invalidate_folio);

struct page *pagectl_grab_cache_page_write_begin(struct address_space *mapping,
						  pgoff_t index)
{
	return grab_cache_page_write_begin(mapping, index);
}
EXPORT_SYMBOL_GPL(pagectl_grab_cache_page_write_begin);

struct folio *pagectl_filemap_get_folio(struct address_space *mapping,
				       pgoff_t index, int fgp_flags,
				       gfp_t gfp)
{
	return __filemap_get_folio(mapping, index, fgp_flags, gfp);
}
EXPORT_SYMBOL_GPL(pagectl_filemap_get_folio);

struct buffer_head *pagectl_sb_bread(struct super_block *sb, sector_t block)
{
	return sb_bread(sb, block);
}
EXPORT_SYMBOL_GPL(pagectl_sb_bread);

struct buffer_head *pagectl_sb_getblk(struct super_block *sb, sector_t block)
{
	return sb_getblk(sb, block);
}
EXPORT_SYMBOL_GPL(pagectl_sb_getblk);

struct buffer_head *pagectl_sb_find_get_block(struct super_block *sb,
					     sector_t block)
{
	return sb_find_get_block(sb, block);
}
EXPORT_SYMBOL_GPL(pagectl_sb_find_get_block);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Filesystem-neutral deferred page dirtying and flush control");
