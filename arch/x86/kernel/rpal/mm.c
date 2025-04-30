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
