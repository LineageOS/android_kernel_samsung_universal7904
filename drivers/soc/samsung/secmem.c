// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015 Samsung Electronics Co., Ltd.
 * Samsung secure-memory userspace interface.
 */

#include <linux/arm-smccc.h>
#include <linux/compat.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <asm/barrier.h>

#include <soc/samsung/secmem.h>

#define SECMEM_DEV_NAME	"s5p-smem"
#define DRM_PROT_VER_BUFFER_BASED_PROT	1
/* The vendor SMC command from the 4.4 Exynos monitor interface. */
#define SMC_DRM_VIDEO_PROC	0x81000007

static bool drm_onoff;
static DEFINE_MUTEX(drm_lock);
static DEFINE_MUTEX(smc_lock);

struct secmem_info {
	bool drm_enabled;
};

/* The caller holds drm_lock. This is bookkeeping, not buffer protection. */
static int drm_enable_locked(struct secmem_info *info, bool enable)
{
	if (drm_onoff == enable) {
		pr_err("secmem: DRM is already %s\n", enable ? "on" : "off");
		return -EINVAL;
	}

	drm_onoff = enable;
	/* Only the instance that enabled DRM may turn it off. */
	info->drm_enabled = enable;

	return 0;
}

static int secmem_open(struct inode *inode, struct file *file)
{
	struct secmem_info *info;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	file->private_data = info;
	return 0;
}

static int secmem_release(struct inode *inode, struct file *file)
{
	struct secmem_info *info = file->private_data;

	mutex_lock(&drm_lock);
	/* Drop ownership even when another instance is still open. */
	if (info->drm_enabled)
		drm_enable_locked(info, false);
	mutex_unlock(&drm_lock);

	kfree(info);
	return 0;
}

static long secmem_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct secmem_info *info = filp->private_data;
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case SECMEM_IOC_CHUNKINFO:
	case SECMEM_IOC_GET_CHUNK_NUM:
		/* TODO(5.10 port): Exynos secure ION region enumeration is absent. */
		return -EOPNOTSUPP;
	case SECMEM_IOC_GET_FD_PHYS_ADDR:
		/* TODO(5.10 port): no Exynos secure heap or ion_phys replacement. */
		return -EOPNOTSUPP;
	case SECMEM_IOC_MIF_LOCK:
		/* TODO(5.10 port): the Exynos MIF frequency QoS backend is absent. */
		return -EOPNOTSUPP;
	case SECMEM_IOC_GET_DRM_ONOFF:
	{
		int val;

		mutex_lock(&drm_lock);
		val = drm_onoff;
		mutex_unlock(&drm_lock);
		/* The ABI is int, not the one-byte internal bool. */
		if (copy_to_user(argp, &val, sizeof(val)))
			return -EFAULT;
		break;
	}
	case SECMEM_IOC_SET_DRM_ONOFF:
	{
		int ret = 0, val;

		if (copy_from_user(&val, argp, sizeof(val)))
			return -EFAULT;

		mutex_lock(&drm_lock);
		if ((info->drm_enabled && !val) ||
		    (!info->drm_enabled && val))
			ret = drm_enable_locked(info, !!val);
		mutex_unlock(&drm_lock);
		return ret;
	}
	case SECMEM_IOC_GET_CRYPTO_LOCK:
	case SECMEM_IOC_RELEASE_CRYPTO_LOCK:
		/* TODO(5.10 port): the vendor crypto locking backend is absent. */
		return -EOPNOTSUPP;
	case SECMEM_IOC_SET_TZPC:
		/* TODO(5.10 port): the legacy TZPC case had no implementation. */
		return -EOPNOTSUPP;
	case SECMEM_IOC_SET_PROTECT:
		/* TODO(5.10 port): the legacy buffer protection handler is absent. */
		return -EOPNOTSUPP;
	case SECMEM_IOC_SET_VIDEO_EXT_PROC:
	{
		struct arm_smccc_res res;
		int val;

		if (copy_from_user(&val, argp, sizeof(val)))
			return -EFAULT;

		mutex_lock(&smc_lock);
		/* Preserve the barrier and SMC arguments used by exynos_smc(). */
		dsb(sy);
		arm_smccc_smc(SMC_DRM_VIDEO_PROC, val, 0, 0, 0, 0, 0, 0, &res);
		mutex_unlock(&smc_lock);
		if ((int)res.a0) {
			pr_err("secmem: VIDEO EXT protection failed: %d\n", val);
			return -ENOMEM;
		}
		break;
	}
	case SECMEM_IOC_GET_DRM_PROT_VER:
	{
		int val = DRM_PROT_VER_BUFFER_BASED_PROT;

		if (copy_to_user(argp, &val, sizeof(val)))
			return -EFAULT;
		break;
	}
	default:
		return -ENOTTY;
	}

	return 0;
}

#ifdef CONFIG_COMPAT
static long secmem_compat_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	/* As in 4.4, accept only the native ioctl layouts, without translation. */
	return secmem_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
}
#endif

static const struct file_operations secmem_fops = {
	.owner		= THIS_MODULE,
	.open		= secmem_open,
	.release	= secmem_release,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= secmem_compat_ioctl,
#endif
	.unlocked_ioctl	= secmem_ioctl,
	.llseek		= no_llseek,
};

static struct miscdevice secmem = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= SECMEM_DEV_NAME,
	.fops	= &secmem_fops,
};

static int __init secmem_init(void)
{
	return misc_register(&secmem);
}

static void __exit secmem_exit(void)
{
	misc_deregister(&secmem);
}

module_init(secmem_init);
module_exit(secmem_exit);

MODULE_DESCRIPTION("Samsung secure-memory userspace interface");
MODULE_LICENSE("GPL");
