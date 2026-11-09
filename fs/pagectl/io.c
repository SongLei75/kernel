// SPDX-License-Identifier: GPL-2.0
/*
 * Buffered-write helpers which defer the normal buffer/page dirtying step.
 * Native mapping, allocation, read and writeback remain unchanged.
 */
#include <linux/buffer_head.h>
#include <linux/highmem.h>
#include <linux/module.h>
#include <linux/pagectl.h>
#include <linux/sched/signal.h>
#include <linux/writeback.h>

static void pagectl_zero_new_buffers(struct page *page,
				     unsigned int from, unsigned int to)
{
	struct buffer_head *bh, *head;
	unsigned int start = 0, end;

	if (!page_has_buffers(page))
		return;
	head = page_buffers(page);
	bh = head;
	do {
		end = start + bh->b_size;
		if (buffer_new(bh) && end > from && start < to) {
			if (!PageUptodate(page)) {
				unsigned int first = max(from, start);
				unsigned int last = min(to, end);

				zero_user(page, first, last - first);
				set_buffer_uptodate(bh);
			}
			clear_buffer_new(bh);
			pagectl_defer_folio(page_folio(page));
		}
		start = end;
		bh = bh->b_this_page;
	} while (bh != head);
}

static int pagectl_block_write_begin(struct address_space *mapping, loff_t pos,
				     unsigned int len, struct page **pagep,
				     get_block_t *get_block)
{
	struct inode *inode = mapping->host;
	unsigned int from = pos & (PAGE_SIZE - 1), to = from + len;
	unsigned int blocksize = i_blocksize(inode);
	unsigned int start, end;
	struct buffer_head *bh, *head, *wait[2], **wait_bh = wait;
	sector_t block;
	struct folio *folio;
	struct page *page;
	int err = 0;

	page = pagectl_grab_cache_page_write_begin(mapping, pos >> PAGE_SHIFT);
	if (!page)
		return -ENOMEM;
	folio = page_folio(page);

	if (!page_has_buffers(page))
		create_empty_buffers(page, blocksize, 0);
	head = page_buffers(page);
	bh = head;
	block = (sector_t)page->index << (PAGE_SHIFT - inode->i_blkbits);

	for (start = 0; ; start = end, block++, bh = bh->b_this_page) {
		end = start + blocksize;
		if (end <= from || start >= to) {
			if (PageUptodate(page))
				set_buffer_uptodate(bh);
			goto next;
		}
		if (buffer_new(bh))
			clear_buffer_new(bh);
		if (!buffer_mapped(bh)) {
			err = get_block(inode, block, bh, 1);
			if (err)
				break;
			if (buffer_new(bh)) {
				clean_bdev_bh_alias(bh);
				if (PageUptodate(page)) {
					clear_buffer_new(bh);
					set_buffer_uptodate(bh);
					pagectl_defer_folio(folio);
					goto next;
				}
				if (end > to || start < from)
					folio_zero_segments(folio, to, end, start, from);
				goto next;
			}
		}
		if (PageUptodate(page)) {
			set_buffer_uptodate(bh);
			goto next;
		}
		if (!buffer_uptodate(bh) && !buffer_delay(bh) &&
		    !buffer_unwritten(bh) && (start < from || end > to)) {
			bh_read_nowait(bh, 0);
			*wait_bh++ = bh;
		}
next:
		if (bh->b_this_page == head)
			break;
	}
	while (wait_bh > wait) {
		wait_on_buffer(*--wait_bh);
		if (!buffer_uptodate(*wait_bh))
			err = -EIO;
	}
	if (err)
		pagectl_zero_new_buffers(page, from, to);
	if (err) {
		unlock_page(page);
		put_page(page);
		page = NULL;
	}
	*pagep = page;
	return err;
}

/* Mirrors cont_expand_zero without invoking the native eager dirtying path. */
static int pagectl_cont_expand_zero(struct file *file,
				    struct address_space *mapping,
				    loff_t pos, loff_t *bytes)
{
	struct inode *inode = mapping->host;
	const struct address_space_operations *aops = mapping->a_ops;
	unsigned int blocksize = i_blocksize(inode);
	pgoff_t index = pos >> PAGE_SHIFT, curidx;
	loff_t curpos;
	unsigned int zerofrom, offset = pos & ~PAGE_MASK, len;
	struct page *page;
	void *fsdata = NULL;
	int err;

	while (index > (curidx = (curpos = *bytes) >> PAGE_SHIFT)) {
		zerofrom = curpos & ~PAGE_MASK;
		if (zerofrom & (blocksize - 1))
			*bytes = (*bytes | (blocksize - 1)) + 1;
		len = PAGE_SIZE - zerofrom;
		err = aops->write_begin(file, mapping, curpos, len,
					&page, &fsdata);
		if (err)
			return err;
		zero_user(page, zerofrom, len);
		err = aops->write_end(file, mapping, curpos, len, len,
				      page, fsdata);
		if (err < 0)
			return err;
		if (err != len)
			return -EIO;
		balance_dirty_pages_ratelimited(mapping);
		if (fatal_signal_pending(current))
			return -EINTR;
	}

	if (index == curidx) {
		zerofrom = curpos & ~PAGE_MASK;
		if (offset > zerofrom) {
			if (zerofrom & (blocksize - 1))
				*bytes = (*bytes | (blocksize - 1)) + 1;
			len = offset - zerofrom;
			err = aops->write_begin(file, mapping, curpos, len,
						&page, &fsdata);
			if (err)
				return err;
			zero_user(page, zerofrom, len);
			err = aops->write_end(file, mapping, curpos, len, len,
					      page, fsdata);
			if (err < 0)
				return err;
			if (err != len)
				return -EIO;
		}
	}
	return 0;
}

int pagectl_cont_write_begin(struct file *file, struct address_space *mapping,
			      loff_t pos, unsigned int len, struct page **pagep,
			      void **fsdata, get_block_t *get_block,
			      loff_t *bytes)
{
	struct inode *inode = mapping->host;
	unsigned int blocksize = i_blocksize(inode);
	unsigned int zerofrom;
	int ret;

	if (!pagectl_enabled(inode->i_sb))
		return cont_write_begin(file, mapping, pos, len, pagep, fsdata,
					get_block, bytes);

	ret = pagectl_cont_expand_zero(file, mapping, pos, bytes);
	if (ret)
		return ret;
	zerofrom = *bytes & ~PAGE_MASK;
	if (pos + len > *bytes && (zerofrom & (blocksize - 1)))
		*bytes = (*bytes | (blocksize - 1)) + 1;
	return pagectl_block_write_begin(mapping, pos, len, pagep, get_block);
}
EXPORT_SYMBOL_GPL(pagectl_cont_write_begin);

int pagectl_generic_write_end(struct file *file, struct address_space *mapping,
			      loff_t pos, unsigned int len, unsigned int copied,
			      struct page *page, void *fsdata)
{
	struct inode *inode = mapping->host;
	loff_t old_size = inode->i_size;
	unsigned int from = pos & (PAGE_SIZE - 1), to;
	unsigned int blockstart = 0, blockend;
	struct buffer_head *bh, *head;
	bool partial = false, changed = false, grew = false;

	if (!pagectl_enabled(inode->i_sb))
		return generic_write_end(file, mapping, pos, len, copied,
					 page, fsdata);

	if (copied < len) {
		if (!PageUptodate(page))
			copied = 0;
		pagectl_zero_new_buffers(page, from + copied, from + len);
	}
	flush_dcache_page(page);
	to = from + copied;

	head = page_buffers(page);
	bh = head;
	do {
		blockend = blockstart + bh->b_size;
		if (blockend <= from || blockstart >= to) {
			if (!buffer_uptodate(bh))
				partial = true;
		} else {
			set_buffer_uptodate(bh);
			changed = true;
		}
		if (buffer_new(bh))
			clear_buffer_new(bh);
		blockstart = blockend;
		bh = bh->b_this_page;
	} while (bh != head);
	if (!partial)
		SetPageUptodate(page);
	if (changed)
		pagectl_defer_folio(page_folio(page));

	if (pos + copied > inode->i_size) {
		i_size_write(inode, pos + copied);
		grew = true;
	}
	unlock_page(page);
	put_page(page);
	if (old_size < pos)
		pagecache_isize_extended(inode, old_size, pos);
	if (grew)
		mark_inode_dirty(inode);
	return copied;
}
EXPORT_SYMBOL_GPL(pagectl_generic_write_end);
