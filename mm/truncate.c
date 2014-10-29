/*
 * mm/truncate.c - code for taking down pages from address_spaces
 *
 * Copyright (C) 2002, Linus Torvalds
 *
 * 10Sep2002	Andrew Morton
 *		Initial version.
 */

#include <linux/kernel.h>
#include <linux/backing-dev.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/export.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/pagevec.h>
#include <linux/task_io_accounting_ops.h>
#include <linux/buffer_head.h>	
#include <linux/cleancache.h>
#include <linux/rmap.h>
#include "internal.h"


void do_invalidatepage(struct page *page, unsigned long offset)
{
	void (*invalidatepage)(struct page *, unsigned long);
	invalidatepage = page->mapping->a_ops->invalidatepage;
#ifdef CONFIG_BLOCK
	if (!invalidatepage)
		invalidatepage = block_invalidatepage;
#endif
	if (invalidatepage)
		(*invalidatepage)(page, offset);
}

static inline void truncate_partial_page(struct page *page, unsigned partial)
{
	zero_user_segment(page, partial, PAGE_CACHE_SIZE);
	cleancache_invalidate_page(page->mapping, page);
	if (page_has_private(page))
		do_invalidatepage(page, partial);
}

void cancel_dirty_page(struct page *page, unsigned int account_size)
{
	if (TestClearPageDirty(page)) {
		struct address_space *mapping = page->mapping;
		if (mapping && mapping_cap_account_dirty(mapping)) {
			dec_zone_page_state(page, NR_FILE_DIRTY);
			dec_bdi_stat(mapping->backing_dev_info,
					BDI_RECLAIMABLE);
			if (account_size)
				task_io_account_cancelled_write(account_size);
		}
	}
}
EXPORT_SYMBOL(cancel_dirty_page);

static int
truncate_complete_page(struct address_space *mapping, struct page *page)
{
	if (page->mapping != mapping)
		return -EIO;

	if (page_has_private(page))
		do_invalidatepage(page, 0);

	cancel_dirty_page(page, PAGE_CACHE_SIZE);

	clear_page_mlock(page);
	ClearPageMappedToDisk(page);
	delete_from_page_cache(page);
	return 0;
}

static int
invalidate_complete_page(struct address_space *mapping, struct page *page)
{
	int ret;

	if (page->mapping != mapping)
		return 0;

	if (page_has_private(page) && !try_to_release_page(page, 0))
		return 0;

	clear_page_mlock(page);
	ret = remove_mapping(mapping, page);

	return ret;
}

int truncate_inode_page(struct address_space *mapping, struct page *page)
{
	if (page_mapped(page)) {
		unmap_mapping_range(mapping,
				   (loff_t)page->index << PAGE_CACHE_SHIFT,
				   PAGE_CACHE_SIZE, 0);
	}
	return truncate_complete_page(mapping, page);
}

int generic_error_remove_page(struct address_space *mapping, struct page *page)
{
	if (!mapping)
		return -EINVAL;
	if (!S_ISREG(mapping->host->i_mode))
		return -EIO;
	return truncate_inode_page(mapping, page);
}
EXPORT_SYMBOL(generic_error_remove_page);

int invalidate_inode_page(struct page *page)
{
	struct address_space *mapping = page_mapping(page);
	if (!mapping)
		return 0;
	if (PageDirty(page) || PageWriteback(page))
		return 0;
	if (page_mapped(page))
		return 0;
	return invalidate_complete_page(mapping, page);
}

void truncate_inode_pages_range(struct address_space *mapping,
				loff_t lstart, loff_t lend)
{
	const pgoff_t start = (lstart + PAGE_CACHE_SIZE-1) >> PAGE_CACHE_SHIFT;
	const unsigned partial = lstart & (PAGE_CACHE_SIZE - 1);
	struct pagevec pvec;
	pgoff_t index;
	pgoff_t end;
	int i;

	cleancache_invalidate_inode(mapping);
	if (mapping->nrpages == 0)
		return;

	BUG_ON((lend & (PAGE_CACHE_SIZE - 1)) != (PAGE_CACHE_SIZE - 1));
	end = (lend >> PAGE_CACHE_SHIFT);

	pagevec_init(&pvec, 0);
	index = start;
	while (index <= end && pagevec_lookup(&pvec, mapping, index,
			min(end - index, (pgoff_t)PAGEVEC_SIZE - 1) + 1)) {
		mem_cgroup_uncharge_start();
		for (i = 0; i < pagevec_count(&pvec); i++) {
			struct page *page = pvec.pages[i];

			
			index = page->index;
			if (index > end)
				break;

			if (!trylock_page(page))
				continue;
			WARN_ON(page->index != index);
			if (PageWriteback(page)) {
				unlock_page(page);
				continue;
			}
			truncate_inode_page(mapping, page);
			unlock_page(page);
		}
		pagevec_release(&pvec);
		mem_cgroup_uncharge_end();
		cond_resched();
		index++;
	}

	if (partial) {
		struct page *page = find_lock_page(mapping, start - 1);
		if (page) {
			wait_on_page_writeback(page);
			truncate_partial_page(page, partial);
			unlock_page(page);
			page_cache_release(page);
		}
	}

	index = start;
	for ( ; ; ) {
		cond_resched();
		if (!pagevec_lookup(&pvec, mapping, index,
			min(end - index, (pgoff_t)PAGEVEC_SIZE - 1) + 1)) {
			if (index == start)
				break;
			index = start;
			continue;
		}
		if (index == start && pvec.pages[0]->index > end) {
			pagevec_release(&pvec);
			break;
		}
		mem_cgroup_uncharge_start();
		for (i = 0; i < pagevec_count(&pvec); i++) {
			struct page *page = pvec.pages[i];

			
			index = page->index;
			if (index > end)
				break;

			lock_page(page);
			WARN_ON(page->index != index);
			wait_on_page_writeback(page);
			truncate_inode_page(mapping, page);
			unlock_page(page);
		}
		pagevec_release(&pvec);
		mem_cgroup_uncharge_end();
		index++;
	}
	cleancache_invalidate_inode(mapping);
}
EXPORT_SYMBOL(truncate_inode_pages_range);

void truncate_inode_pages(struct address_space *mapping, loff_t lstart)
{
	truncate_inode_pages_range(mapping, lstart, (loff_t)-1);
}
EXPORT_SYMBOL(truncate_inode_pages);

unsigned long invalidate_mapping_pages(struct address_space *mapping,
		pgoff_t start, pgoff_t end)
{
	struct pagevec pvec;
	pgoff_t index = start;
	unsigned long ret;
	unsigned long count = 0;
	int i;


	pagevec_init(&pvec, 0);
	while (index <= end && pagevec_lookup(&pvec, mapping, index,
			min(end - index, (pgoff_t)PAGEVEC_SIZE - 1) + 1)) {
		mem_cgroup_uncharge_start();
		for (i = 0; i < pagevec_count(&pvec); i++) {
			struct page *page = pvec.pages[i];

			
			index = page->index;
			if (index > end)
				break;

			if (!trylock_page(page))
				continue;
			WARN_ON(page->index != index);
			ret = invalidate_inode_page(page);
			unlock_page(page);
			if (!ret)
				deactivate_page(page);
			count += ret;
		}
		pagevec_release(&pvec);
		mem_cgroup_uncharge_end();
		cond_resched();
		index++;
	}
	return count;
}
EXPORT_SYMBOL(invalidate_mapping_pages);

static int
invalidate_complete_page2(struct address_space *mapping, struct page *page)
{
	if (page->mapping != mapping)
		return 0;

	if (page_has_private(page) && !try_to_release_page(page, GFP_KERNEL))
		return 0;

	clear_page_mlock(page);

	spin_lock_irq(&mapping->tree_lock);
	if (PageDirty(page))
		goto failed;

	BUG_ON(page_has_private(page));
	__delete_from_page_cache(page);
	spin_unlock_irq(&mapping->tree_lock);
	mem_cgroup_uncharge_cache_page(page);

	if (mapping->a_ops->freepage)
		mapping->a_ops->freepage(page);

	page_cache_release(page);	
	return 1;
failed:
	spin_unlock_irq(&mapping->tree_lock);
	return 0;
}

static int do_launder_page(struct address_space *mapping, struct page *page)
{
	if (!PageDirty(page))
		return 0;
	if (page->mapping != mapping || mapping->a_ops->launder_page == NULL)
		return 0;
	return mapping->a_ops->launder_page(page);
}

int invalidate_inode_pages2_range(struct address_space *mapping,
				  pgoff_t start, pgoff_t end)
{
	struct pagevec pvec;
	pgoff_t index;
	int i;
	int ret = 0;
	int ret2 = 0;
	int did_range_unmap = 0;

	cleancache_invalidate_inode(mapping);
	pagevec_init(&pvec, 0);
	index = start;
	while (index <= end && pagevec_lookup(&pvec, mapping, index,
			min(end - index, (pgoff_t)PAGEVEC_SIZE - 1) + 1)) {
		mem_cgroup_uncharge_start();
		for (i = 0; i < pagevec_count(&pvec); i++) {
			struct page *page = pvec.pages[i];

			
			index = page->index;
			if (index > end)
				break;

			lock_page(page);
			WARN_ON(page->index != index);
			if (page->mapping != mapping) {
				unlock_page(page);
				continue;
			}
			wait_on_page_writeback(page);
			if (page_mapped(page)) {
				if (!did_range_unmap) {
					unmap_mapping_range(mapping,
					   (loff_t)index << PAGE_CACHE_SHIFT,
					   (loff_t)(1 + end - index)
							 << PAGE_CACHE_SHIFT,
					    0);
					did_range_unmap = 1;
				} else {
					unmap_mapping_range(mapping,
					   (loff_t)index << PAGE_CACHE_SHIFT,
					   PAGE_CACHE_SIZE, 0);
				}
			}
			BUG_ON(page_mapped(page));
			ret2 = do_launder_page(mapping, page);
			if (ret2 == 0) {
				if (!invalidate_complete_page2(mapping, page))
					ret2 = -EBUSY;
			}
			if (ret2 < 0)
				ret = ret2;
			unlock_page(page);
		}
		pagevec_release(&pvec);
		mem_cgroup_uncharge_end();
		cond_resched();
		index++;
	}
	cleancache_invalidate_inode(mapping);
	return ret;
}
EXPORT_SYMBOL_GPL(invalidate_inode_pages2_range);

int invalidate_inode_pages2(struct address_space *mapping)
{
	return invalidate_inode_pages2_range(mapping, 0, -1);
}
EXPORT_SYMBOL_GPL(invalidate_inode_pages2);

/**
 * truncate_pagecache - unmap and remove pagecache that has been truncated
 * @inode: inode
 * @oldsize: old file size
 * @newsize: new file size
 *
 * inode's new i_size must already be written before truncate_pagecache
 * is called.
 *
 * This function should typically be called before the filesystem
 * releases resources associated with the freed range (eg. deallocates
 * blocks). This way, pagecache will always stay logically coherent
 * with on-disk format, and the filesystem would not have to deal with
 * situations such as writepage being called for a page that has already
 * had its underlying blocks deallocated.
 */
void truncate_pagecache(struct inode *inode, loff_t oldsize, loff_t newsize)
{
	struct address_space *mapping = inode->i_mapping;
	loff_t holebegin = round_up(newsize, PAGE_SIZE);

	unmap_mapping_range(mapping, holebegin, 0, 1);
	truncate_inode_pages(mapping, newsize);
	unmap_mapping_range(mapping, holebegin, 0, 1);
}
EXPORT_SYMBOL(truncate_pagecache);

void truncate_setsize(struct inode *inode, loff_t newsize)
{
	loff_t oldsize = inode->i_size;
	i_size_write(inode, newsize);

	if (newsize > oldsize)
		pagecache_isize_extended(inode, oldsize, newsize);
	truncate_pagecache(inode, oldsize, newsize);
}
EXPORT_SYMBOL(truncate_setsize);

/**
 * pagecache_isize_extended - update pagecache after extension of i_size
 * @inode:	inode for which i_size was extended
 * @from:	original inode size
 * @to:		new inode size
 *
 * Handle extension of inode size either caused by extending truncate or by
 * write starting after current i_size. We mark the page straddling current
 * i_size RO so that page_mkwrite() is called on the nearest write access to
 * the page.  This way filesystem can be sure that page_mkwrite() is called on
 * the page before user writes to the page via mmap after the i_size has been
 * changed.
 *
 * The function must be called after i_size is updated so that page fault
 * coming after we unlock the page will already see the new i_size.
 * The function must be called while we still hold i_mutex - this not only
 * makes sure i_size is stable but also that userspace cannot observe new
 * i_size value before we are prepared to store mmap writes at new inode size.
 */
void pagecache_isize_extended(struct inode *inode, loff_t from, loff_t to)
{
	int bsize = 1 << inode->i_blkbits;
	loff_t rounded_from;
	struct page *page;
	pgoff_t index;

	WARN_ON(to > inode->i_size);

	if (from >= to || bsize == PAGE_CACHE_SIZE)
		return;
	/* Page straddling @from will not have any hole block created? */
	rounded_from = round_up(from, bsize);
	if (to <= rounded_from || !(rounded_from & (PAGE_CACHE_SIZE - 1)))
		return;

	index = from >> PAGE_CACHE_SHIFT;
	page = find_lock_page(inode->i_mapping, index);
	/* Page not cached? Nothing to do */
	if (!page)
		return;
	/*
	 * See clear_page_dirty_for_io() for details why set_page_dirty()
	 * is needed.
	 */
	if (page_mkclean(page))
		set_page_dirty(page);
	unlock_page(page);
	page_cache_release(page);
}
EXPORT_SYMBOL(pagecache_isize_extended);

int vmtruncate(struct inode *inode, loff_t newsize)
{
	int error;

	error = inode_newsize_ok(inode, newsize);
	if (error)
		return error;

	truncate_setsize(inode, newsize);
	if (inode->i_op->truncate)
		inode->i_op->truncate(inode);
	return 0;
}
EXPORT_SYMBOL(vmtruncate);

void truncate_pagecache_range(struct inode *inode, loff_t lstart, loff_t lend)
{
	struct address_space *mapping = inode->i_mapping;
	loff_t unmap_start = round_up(lstart, PAGE_SIZE);
	loff_t unmap_end = round_down(1 + lend, PAGE_SIZE) - 1;

	if ((u64)unmap_end > (u64)unmap_start)
		unmap_mapping_range(mapping, unmap_start,
				    1 + unmap_end - unmap_start, 0);
	truncate_inode_pages_range(mapping, lstart, lend);
}
EXPORT_SYMBOL(truncate_pagecache_range);
