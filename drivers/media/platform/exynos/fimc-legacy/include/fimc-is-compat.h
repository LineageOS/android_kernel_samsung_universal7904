/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Force-included 4.4 -> 5.10 compatibility for the m20lte FIMC-IS port.
 * Keep this file small; every shim must be a faithful 4.4 semantic.
 */
#ifndef FIMC_IS_COMPAT_H
#define FIMC_IS_COMPAT_H

#include <linux/version.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/ktime.h>
#include <linux/timekeeping.h>
#include <linux/time64.h>

/* 4.4 struct timeval is the 5.10 uapi legacy __kernel_old_timeval. */
#define timeval __kernel_old_timeval

static inline void do_gettimeofday(struct timeval *tv)
{
	struct timespec64 ts;

	ktime_get_real_ts64(&ts);
	tv->tv_sec = ts.tv_sec;
	tv->tv_usec = ts.tv_nsec / NSEC_PER_USEC;
}

#include <linux/printk.h>
#include <linux/timer.h>
#include <linux/kthread.h>
#include <linux/sched/clock.h>
#include <linux/sched/types.h>
#include <uapi/linux/sched/types.h>
#include <linux/pm_qos.h>
#include <linux/io.h>
#include <asm/neon.h>
#include <asm/fpsimd.h>
#include <asm/simd.h>

#define CONFIG_VIDEOBUF2_ION 1
#define pr_warning pr_warn

/* struct timespec no longer exists in the kernel; 4.4 users only format it. */
#define timespec timespec64
#define ktime_get_ts ktime_get_ts64
#define ktime_to_timespec ktime_to_timespec64

#define cpu_clock(cpu) local_clock()
#define ioremap_nocache ioremap
#define devm_ioremap_nocache devm_ioremap
#define IRQF_GIC_MULTI_TARGET 0

/* kthread worker API was renamed in 4.9. */
#define init_kthread_worker kthread_init_worker
#define init_kthread_work kthread_init_work
#define queue_kthread_work kthread_queue_work
#define flush_kthread_work kthread_flush_work
#define flush_kthread_worker kthread_flush_worker

/* Firmware runs with NEON ownership, but its C OS/sensor callbacks may
 * sleep. Snapshot every FP/SIMD register before ending NEON ownership, then
 * reacquire it and restore the firmware state when the callback returns.
 * A stack snapshot follows the task even when a sleeping callback migrates.
 * Only release the exact process-context BH disable owned by NEON: never
 * drop a caller's real spinlock, hard-IRQ or softirq protection.
 */
struct fimc_is_fpsimd_sleep {
	struct user_fpsimd_state state;
	bool paused;
};

static inline void fimc_is_fpsimd_suspend(struct fimc_is_fpsimd_sleep *save)
{
	save->paused = false;
	if (preempt_count() != SOFTIRQ_DISABLE_OFFSET || irqs_disabled() ||
	    !this_cpu_read(fpsimd_context_busy))
		return;

	fpsimd_save_state(&save->state);
	save->paused = true;
	kernel_neon_end();
}

static inline void fimc_is_fpsimd_resume(struct fimc_is_fpsimd_sleep *save)
{
	if (!save->paused)
		return;

	kernel_neon_begin();
	fpsimd_load_state(&save->state);
	save->paused = false;
}

#define FIMC_IS_SLEEPABLE(call) ({ \
	struct fimc_is_fpsimd_sleep __fp_save; \
	typeof(call) __fp_ret; \
	fimc_is_fpsimd_suspend(&__fp_save); \
	if (in_atomic() || irqs_disabled()) \
		__fp_ret = -EWOULDBLOCK; \
	else \
		__fp_ret = (call); \
	fimc_is_fpsimd_resume(&__fp_save); \
	__fp_ret; \
})

/* Nested firmware calls borrow an existing NEON scope on this task.
 * A hard interrupt must never borrow registers from the interrupted task.
 * IRQ entry points which execute firmware therefore run in IRQ threads.
 */
struct fimc_is_simd_guard {
	bool acquired;
};

static inline int fimc_is_simd_guard_enter(struct fimc_is_simd_guard *guard)
{
	guard->acquired = false;
	if (in_irq() || in_nmi())
		return -EWOULDBLOCK;
	if (this_cpu_read(fpsimd_context_busy))
		return 0;
	if (!may_use_simd())
		return -EWOULDBLOCK;
	kernel_neon_begin();
	guard->acquired = true;
	return 0;
}

static inline void fimc_is_simd_guard_exit(struct fimc_is_simd_guard *guard)
{
	if (guard->acquired) {
		kernel_neon_end();
		guard->acquired = false;
	}
}

#define FIMC_IS_SIMD_CALL(call) ({ \
	struct fimc_is_simd_guard __simd_guard; \
	int __simd_ret = fimc_is_simd_guard_enter(&__simd_guard); \
	if (!__simd_ret) { \
		__simd_ret = (call); \
		fimc_is_simd_guard_exit(&__simd_guard); \
	} \
	__simd_ret; \
})

#define fpsimd_get() kernel_neon_begin()
#define fpsimd_put() kernel_neon_end()
#define fpsimd_set_task_using(t) ((void)(t))

/*
 * TODO(5.10 port): the 4.4 Samsung pm_qos classes (INT/MIF/CAM/HPG throughput)
 * have no 5.10 provider. Requests are accepted and dropped.
 */
#define PM_QOS_DEVICE_THROUGHPUT	1000
#define PM_QOS_BUS_THROUGHPUT		1001
#define PM_QOS_INTCAM_THROUGHPUT	1002
#define PM_QOS_CAM_THROUGHPUT		1003
#define PM_QOS_CPU_ONLINE_MIN		1004
#undef pm_qos_add_request
#undef pm_qos_update_request
#undef pm_qos_remove_request
#undef pm_qos_request_active
#define pm_qos_add_request(req, cls, val) ((void)(req), (void)(cls), (void)(val))
#define pm_qos_update_request(req, val) ((void)(req), (void)(val))
#define pm_qos_remove_request(req) ((void)(req))
#define pm_qos_request_active(req) ((void)(req), false)

/*
 * TODO(5.10 port): Samsung SoC services the 4.4 driver calls that have no
 * 5.10 provider yet (TMU/ISP cooling, BCM, PCIe control). Inert stubs.
 */
struct notifier_block;
static inline int exynos_tmu_isp_add_notifier(struct notifier_block *n)
{
	return 0;
}
static inline unsigned int isp_cooling_get_fps(unsigned int id, unsigned long freq)
{
	return 0;
}
static inline void bcm_stop(void *data) { }

/* Private core.ioctl commands standing in for removed 4.4 subdev ops. */
#define FIMC_IS_SD_S_CTRL	0x46530001
#define FIMC_IS_SD_G_CTRL	0x46530002
#define FIMC_IS_SD_S_EXT_CTRLS	0x46530003
#define FIMC_IS_SD_G_EXT_CTRLS	0x46530004
#define FIMC_IS_SD_S_PARM	0x46530005

#include <linux/pinctrl/consumer.h>
#define devm_pinctrl_get_select pinctrl_get_select
#define devm_pinctrl_put pinctrl_put

/* Samsung CPU cluster pm_qos classes, see the pm_qos note above. */
#define PM_QOS_CLUSTER0_FREQ_MIN	1010
#define PM_QOS_CLUSTER0_FREQ_MAX	1011
#define PM_QOS_CLUSTER1_FREQ_MIN	1012
#define PM_QOS_CLUSTER1_FREQ_MAX	1013

/* From the 4.4 isp_cooling/tmu headers. */
enum isp_noti_state {
	ISP_NORMAL = 0,
	ISP_COLD,
	ISP_THROTTLING,
	ISP_TRIPPING,
};

/* Samsung 4.4 videodev2.h buffer flag, dropped from the 5.10 uapi. */
#define V4L2_BUF_FLAG_USE_SYNC	0x00008000

/*
 * 4.4 in-kernel users of the file syscalls (calibration/dump files, always
 * under set_fs(KERNEL_DS)). TODO(5.10 port): sys_rmdir is not provided; only
 * used to clear a directory squatting on a dump file name.
 */
#include <linux/syscalls.h>
#include <linux/fdtable.h>
#include <linux/fs_struct.h>
#include <linux/namei.h>
#include <linux/fcntl.h>
#include <linux/uaccess.h>

#define sys_open(name, flags, mode) \
	do_sys_open(AT_FDCWD, (const char __force __user *)(name), (flags), (mode))
#define sys_close(fd) __close_fd(current->files, (fd))

static inline int fimc_is_sys_umask(int mask)
{
	return xchg(&current->fs->umask, mask & S_IRWXUGO);
}
#define sys_umask(mask) fimc_is_sys_umask(mask)

static inline int fimc_is_sys_access(const char *name, int mode)
{
	struct path path;
	int ret = kern_path(name, LOOKUP_FOLLOW, &path);

	if (!ret)
		path_put(&path);
	return ret;
}
#define sys_access(name, mode) fimc_is_sys_access((name), (mode))
#define sys_rmdir(name) (-EPERM)

#endif
