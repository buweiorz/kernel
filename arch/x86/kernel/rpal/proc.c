// SPDX-License-Identifier: GPL-2.0-only
#include <linux/sched/signal.h>
#include <linux/poll.h>
#include <linux/mm.h>
#include <linux/proc_fs.h>
#include <linux/rpal.h>
#include <linux/seq_file.h>
#include <linux/pkeys.h>
#include <asm/page.h>

#include "internal.h"

char *proc_rpal_enabled_page;

int rpal_open(struct inode *inode,
			     struct file *file)
{
	return 0;
}

ssize_t rpal_read(struct file *file,
			     char __user *buf, size_t count, loff_t *pos)
{
	struct rpal_service *cur = rpal_current_service();
	struct rpal_poll_data *rpd;
	u64 released_keys[MAX_REQUEST_SERVICE];
	unsigned long flags;
	int nr_key = 0;
	int nr_byte = 0;
	int idx;

	if (!cur)
		return -EINVAL;

	rpd = &cur->rpd;

	spin_lock_irqsave(&rpd->poll_lock, flags);
	idx = find_first_bit(rpd->dead_key_bitmap, RPAL_NR_ID);
	while (idx < RPAL_NR_ID) {
		released_keys[nr_key++] = rpd->dead_keys[idx];
		idx = find_next_bit(rpd->dead_key_bitmap, RPAL_NR_ID, idx + 1);
	}
	spin_unlock_irqrestore(&rpd->poll_lock, flags);
	nr_byte = nr_key * sizeof(u64);

	if (copy_to_user(buf, released_keys, nr_byte)) {
		nr_byte = -EAGAIN;
		goto out;
	}
out:
	return nr_byte;
}

ssize_t rpal_write(struct file *file,
			     const char __user *buf, size_t count, loff_t *ppos)
{
	struct rpal_service *cur = rpal_current_service();
	struct rpal_ctl_args args;
	unsigned long res;
	int ret;

	if (!cur)
		return -EINVAL;

	if (count != sizeof(args))
		return -EINVAL;

	ret = copy_from_user(&args, buf, sizeof(args));
	if (ret) {
		pr_debug("rpal: copy arguments from userspace failed: %d\n",
			ret);
		goto out;
	}

	res = rpal_ctl(args.cmd, args.arg0, args.arg1);

	ret = put_user(res, args.ret);
	if (ret) {
		pr_debug("copy result to userspace failed\n");
		force_sig_fault(SIGSEGV, SEGV_MAPERR, (void __user *)args.ret);
		goto out;
	}

out:
	return ret ? -EINVAL : count;
}

__poll_t rpal_poll(struct file *filep,
			     struct poll_table_struct *wait)
{
	struct rpal_service *cur = rpal_current_service();
	struct rpal_poll_data *rpd;
	unsigned long flags;
	__poll_t mask = 0;

	if (unlikely(!cur)) {
		rpal_err("Not a rpal service\n");
		goto out;
	}

	rpd = &cur->rpd;

	poll_wait(filep, &rpd->rpal_waitqueue, wait);

	spin_lock_irqsave(&rpd->poll_lock, flags);
	if (find_first_bit(rpd->dead_key_bitmap, RPAL_NR_ID) < RPAL_NR_ID)
		mask |= EPOLLIN | EPOLLRDNORM;
	spin_unlock_irqrestore(&rpd->poll_lock, flags);

out:
	return mask;
}

const struct proc_ops proc_rpal_operations = {
	.proc_open = rpal_open,
	.proc_read = rpal_read,
	.proc_write = rpal_write,
	.proc_mmap = rpal_mmap,
	.proc_poll = rpal_poll,
};

int rpal_enabled_show(struct seq_file *m, void *p)
{
	seq_printf(m, "pku: %d\n", rpal_read_proc_pku_enabled());

	return 0;
}

int rpal_enabled_open(struct inode *inode,
			     struct file *file)
{
	return single_open(file, rpal_enabled_show, NULL);
}

#define RPAL_BUF_SIZE 0x20
ssize_t rpal_enabled_write(struct file *file, const char __user *buf, size_t count,
			     loff_t *ppos)
{
	char kbuf[RPAL_BUF_SIZE];
	int value = -1;
	int ret;

	memset(kbuf, 0, sizeof(kbuf));

	if (count >= RPAL_BUF_SIZE)
		return -EINVAL;

	ret = copy_from_user(kbuf, buf, count);

	if (ret)
		return -EAGAIN;

	ret = kstrtoint(kbuf, 0, &value);
	if (!ret && value >= 0 && value <= RPAL_ENABLED_MASK) {
		rpal_write_proc_pku_enabled(((value & RPAL_PKU_ENABLED)
			 && rpal_pku_enabled()) ? 1 : 0);
		pr_info("rpal_enabled: %d\n", value);
	}

	return count;
}

int rpal_enabled_mmap(struct file *filp,
			     struct vm_area_struct *vma)
{
	unsigned long size = (unsigned long)(vma->vm_end - vma->vm_start);
	int ret;

	if (size != PAGE_SIZE)
		return -EINVAL;

	ret = remap_pfn_range(vma, vma->vm_start,
			      page_to_pfn(virt_to_page(proc_rpal_enabled_page)),
			      size, vma->vm_page_prot);

	return ret;
}

const struct proc_ops proc_rpal_enabled_operations = {
	.proc_open = rpal_enabled_open,
	.proc_read = seq_read,
	.proc_write = rpal_enabled_write,
	.proc_mmap = rpal_enabled_mmap,
};

static void rpal_enabled_init(void)
{
	/* We must allocate a new page to avoid exposing other kernel data */
	proc_rpal_enabled_page = (char *)get_zeroed_page(GFP_KERNEL);
	if (!proc_rpal_enabled_page) {
		rpal_err("alloc proc_rpal_enabled_page fail\n");
		return;
	}

	rpal_write_proc_pku_enabled(arch_pkeys_enabled() ? 1 : 0);
	proc_create("rpal_enabled", 0644, NULL, &proc_rpal_enabled_operations);
}

static int __init proc_rpal_init(void)
{
	if (boot_cpu_has(X86_FEATURE_NORPAL))
		return 0;

	rpal_enabled_init();
	proc_create("rpal", 0644, NULL, &proc_rpal_operations);
	return 0;
}
fs_initcall(proc_rpal_init);
