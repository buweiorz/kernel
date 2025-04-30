// SPDX-License-Identifier: GPL-2.0-only
#include <linux/rpal.h>
#include <linux/file.h>
#include <linux/net.h>
#include <linux/pkeys.h>
#include <net/af_unix.h>
#include <asm/paravirt_types.h>
#include <linux/mmu_context.h>

#include "internal.h"

bool rpal_inited;
unsigned long rpal_cap;


struct task_struct *rpal_find_next_task(unsigned long fsbase)
{
	struct rpal_service *cur = rpal_current_service();
	struct rpal_service *tgt;
	struct task_struct *tsk = NULL;
	int i;

	tgt = rpal_get_mapped_service_by_addr(cur, fsbase);
	if (unlikely(!tgt)) {
		pr_debug("rpal debug: cannot find legal rs, fsbase: 0x%016lx\n",
			 fsbase);
		return NULL;
	}
	for (i = 0; i < RPAL_MAX_RECEIVER_NUM; ++i) {
		if (tgt->fs_tsk_map[i].fsbase == fsbase) {
			tsk = tgt->fs_tsk_map[i].tsk;
			break;
		}
	}
	rpal_put_service(tgt);

	return tsk;
}

static long rpal_cmd_get_api_version_and_cap(void __user *p)
{
	struct rpal_version_info rvi;
	int ret;

	rvi.compat_version = RPAL_COMPAT_VERSION;
	rvi.api_version = RPAL_API_VERSION;
	rvi.cap = rpal_cap;

	ret = copy_to_user(p, &rvi, sizeof(rvi));
	if (ret)
		goto fail;

	return 0;

fail:
	return -1;
}

static long rpal_cmd_register_thread(unsigned long arg0, unsigned long arg1)
{
	long ret;

	switch (arg1) {
	case RPAL_REGISTER_SENDER_THREAD:
		ret = rpal_register_sender(arg0);
		break;
	case RPAL_REGISTER_RECEIVER_THREAD:
		ret = rpal_register_receiver(arg0);
		break;
	case RPAL_UNREGISTER_SENDER_THREAD:
		ret = rpal_unregister_sender();
		break;
	case RPAL_UNREGISTER_RECEIVER_THREAD:
		ret = rpal_unregister_receiver();
		break;
	default:
		ret = -RPAL_ERR_BAD_ARG;
		break;
	}

	return ret;
}

long rpal_ctl(unsigned long cmd, unsigned long arg0, unsigned long arg1)
{
	struct rpal_service *cur = rpal_current_service();
	long ret;

	switch (cmd) {
	case RPAL_CMD_GET_API_VERSION_AND_CAP:
		ret = rpal_cmd_get_api_version_and_cap((void __user *)arg0);
		break;
	case RPAL_CMD_GET_SERVICE_KEY:
		ret = (long)cur->key;
		break;
	case RPAL_CMD_REQUEST_SERVICE:
		ret = rpal_request_service((u64)arg0, (void __user *)arg1);
		break;
	case RPAL_CMD_RELEASE_SERVICE:
		ret = rpal_release_service((u64)arg0);
		break;
	case RPAL_CMD_ENABLE_SERVICE:
		ret = rpal_enable_service((void __user *)arg0,
					  (void __user *)arg1, false);
		break;
	case RPAL_CMD_DISABLE_SERVICE:
		ret = rpal_disable_service();
		break;
	case RPAL_CMD_REGISTER_THREAD:
		ret = rpal_cmd_register_thread(arg0, arg1);
		break;
	default:
		ret = -RPAL_ERR_BAD_ARG;
		break;
	}
	return ret;
}

static bool check_hardware_features(void)
{
	if (!boot_cpu_has(X86_FEATURE_FSGSBASE)) {
		rpal_err("no fsgsbase feature\n");
		return false;
	}

	return true;
}

int __init rpal_init(void)
{
	int ret = 0;

	rpal_cap = 0;

	if (!check_hardware_features())
		goto fail;

	ret = rpal_service_init();
	if (ret)
		goto fail;

	ret = rpal_thread_init();
	if (ret)
		goto thread_init_fail;

	rpal_inited = true;
	return 0;

thread_init_fail:
	rpal_service_exit();
fail:
	rpal_err("rpal init fail\n");
	return -1;
}

subsys_initcall(rpal_init);
