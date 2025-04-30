// SPDX-License-Identifier: GPL-2.0-only
#include <linux/rpal.h>
#include <linux/sched/mm.h>
#include <linux/mman.h>
#include <linux/hugetlb.h>
#include <linux/security.h>
#include <linux/sched/task.h>

#include <asm/pgalloc.h>
#include <asm/tlbflush.h>

#include "internal.h"

void rpal_munmap(struct vm_area_struct *area);
const struct vm_operations_struct rpal_vm_ops = { .close = rpal_munmap };

static inline int rpal_balloon_mapping(unsigned long base, unsigned long size)
{
	struct vm_area_struct *vma;
	unsigned long addr, populate;
	int is_fail = 0;

	if (size == 0)
		return 0;

	addr = do_mmap(NULL, base, size, PROT_NONE,
		       MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE, 0, &populate,
		       NULL);

	is_fail = base != addr;

	if (is_fail) {
		pr_info("rpal: Balloon mapping 0x%016lx - 0x%016lx, %s, addr: 0x%016lx\n",
			base, base + size, is_fail ? "Fail" : "Success", addr);
	}
	vma = find_vma(current->mm, addr);
	if (vma->vm_start != addr || vma->vm_end != addr + size) {
		is_fail = 1;
		rpal_err("rpal: find vma 0x%016lx - 0x%016lx fail\n", addr,
			 addr + size);
	} else {
		vma->vm_flags |= VM_DONTEXPAND | VM_PFNMAP | VM_DONTDUMP;
	}

	return is_fail;
}

#define RPAL_USER_TOP TASK_SIZE

int rpal_balloon_init(unsigned long base)
{
	unsigned long top;
	struct mm_struct *mm = current->mm;
	int ret;

	top = base + RPAL_ADDR_SPACE_SIZE;

	mmap_write_lock(mm);

	if (base > mmap_min_addr) {
		ret = rpal_balloon_mapping(mmap_min_addr, base - mmap_min_addr);
		if (ret)
			goto out;
	}

	ret = rpal_balloon_mapping(top, RPAL_USER_TOP - top);
	if (ret && base > mmap_min_addr)
		do_munmap(mm, mmap_min_addr, base - mmap_min_addr, NULL);

out:
	mmap_write_unlock(mm);

	return ret;
}

void rpal_munmap(struct vm_area_struct *area)
{
	struct mm_struct *mm = area->vm_mm;
	struct rpal_service *rs = mm->rpal_rs;
	struct rpal_shared_page *rsp = area->vm_private_data;
	unsigned long flags;
	int refcnt = atomic_read(&rsp->refcnt);

	if (mm->rpal_rs == NULL) {
		pr_debug(
			"free shared page after exit_mmap or fork a child process\n");
		return;
	}

	/* TODO: implement a better design of shared memory free */
	if (unlikely(area->vm_start != rsp->user_start ||
		     area->vm_end != rsp->user_end)) {
		rpal_err("free partial of shared pages\n");
		return;
	}

	if (unlikely(refcnt != 0)) {
		rpal_err("refcnt(%d) of shared page is not 0\n", refcnt);
		return;
	}

	spin_lock_irqsave(&rs->lock, flags);
	list_del(&rsp->list);
	spin_unlock_irqrestore(&rs->lock, flags);

	atomic_sub(rsp->npage, &rs->nr_shared_pages);
	__free_pages(rsp->page, get_order(rsp->npage));
	kfree(rsp);

	pr_debug("rpal: [%d] free page user addr: 0x%016lx - 0x%016lx\n",
		 current->pid, rsp->user_start, rsp->user_end);
}

int rpal_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct rpal_service *cur = rpal_current_service();
	struct rpal_shared_page *rsp;
	struct page *page = NULL;
	unsigned long size = (unsigned long)(vma->vm_end - vma->vm_start);
	unsigned long flags;
	int nr_pages, npage;
	int order = -1;
	int ret = 0;

	if (!cur) {
		ret = -EINVAL;
		goto out;
	}

	pr_debug("rpal debug: vma->vm_start: 0x%016lx, size: 0x%016lx\n",
		 vma->vm_start, size);

	if (!IS_ALIGNED(size, PAGE_SIZE) ||
	    !IS_ALIGNED(vma->vm_start, PAGE_SIZE)) {
		ret = -EINVAL;
		goto out;
	}

	npage = size >> PAGE_SHIFT;
	if (!is_power_of_2(npage)) {
		ret = -EINVAL;
		goto out;
	}

	order = get_order(size);

retry:
	nr_pages = atomic_read(&cur->nr_shared_pages);
	if (nr_pages + npage <= RPAL_MAX_SHARED_PAGES) {
		if (atomic_cmpxchg(&cur->nr_shared_pages, nr_pages,
				   nr_pages + npage) != nr_pages) {
			goto retry;
		}
	} else {
		ret = -ENOMEM;
		goto out;
	}

	rsp = kmalloc(sizeof(*rsp), GFP_KERNEL);
	if (!rsp) {
		ret = -EAGAIN;
		goto dec;
	}

	page = alloc_pages(GFP_KERNEL | __GFP_ZERO, order);
	if (!page) {
		ret = -ENOMEM;
		goto free_rsp;
	}

	rsp->user_start = vma->vm_start;
	rsp->user_end = vma->vm_end;
	rsp->kernel_start = (unsigned long)page_address(page);
	rsp->npage = npage;
	rsp->page = page;
	atomic_set(&rsp->refcnt, 0);
	INIT_LIST_HEAD(&rsp->list);

	ret = remap_pfn_range(vma, vma->vm_start, page_to_pfn(page), size,
			      vma->vm_page_prot);
	if (ret)
		goto free_page;

	spin_lock_irqsave(&cur->lock, flags);
	list_add(&rsp->list, &cur->shared_pages);
	spin_unlock_irqrestore(&cur->lock, flags);

	vma->vm_ops = &rpal_vm_ops;
	vma->vm_private_data = rsp;

	return 0;

free_page:
	__free_pages(page, order);
free_rsp:
	kfree(rsp);
dec:
	atomic_sub(npage, &cur->nr_shared_pages);
out:
	return ret;
}

void rpal_exit_mmap(struct mm_struct *mm)
{
	struct rpal_service *rs = mm->rpal_rs;

	if (rs) {
		int nr_pages;

		mm->rpal_rs = NULL;
		nr_pages = atomic_read(&rs->nr_shared_pages);
		if (unlikely(nr_pages != 0))
			rpal_err("shared page is not zero: %d\n", nr_pages);
		rpal_put_service(rs);
	}
}
