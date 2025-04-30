/* SPDX-License-Identifier: GPL-2.0 */

#define RPAL_COMPAT_VERSION 1
#define RPAL_API_VERSION 1

/* Error Code */
#define RPAL_ERR_BAD_ARG 1
#define RPAL_ERR_NO_SERVICE 2
#define RPAL_ERR_MAPPED 3
#define RPAL_ERR_RETRY 4
#define RPAL_ERR_BAD_SERVICE_STATUS 5
#define RPAL_ERR_BAD_THREAD_STATUS 6
#define RPAL_ERR_REACH_LIMIT 7
#define RPAL_ERR_NOMEM 8
#define RPAL_ERR_NOMAPPING 9
#define RPAL_ERR_INVAL 10

/* rpal_ctl command */
enum rpal_command_type {
	RPAL_CMD_GET_API_VERSION_AND_CAP,
	RPAL_CMD_GET_SERVICE_KEY,
	RPAL_NR_CMD,
};

extern bool rpal_inited;

/* service.c */
int __init rpal_service_init(void);
void rpal_service_exit(void);
long rpal_ctl(unsigned long cmd, unsigned long arg0, unsigned long arg1);

/* mm.c */
int rpal_mmap(struct file *filp, struct vm_area_struct *vma);
