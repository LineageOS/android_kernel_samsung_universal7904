/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2015 Samsung Electronics Co., Ltd.
 * EXYNOS - Secure memory support
 */

#ifndef __ASM_ARCH_SECMEM_H
#define __ASM_ARCH_SECMEM_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define MAX_NAME_LEN	20

/* Keep the native ARM64 layouts used by the 4.4 vendor interface. */
struct secchunk_info {
	int		index;
	char		name[MAX_NAME_LEN];
	phys_addr_t	base;
	size_t		size;
};

struct secfd_info {
	int		fd;
	unsigned long	phys;
};

struct protect_info {
	__u32 dev;
	__u32 enable;
};

struct secmem_crypto_driver_ftn {
	int (*lock)(void);
	int (*release)(void);
};

struct secmem_region {
	char		*virt_addr;
	unsigned long	phys_addr;
	unsigned long	len;
};

#if defined(CONFIG_EXYNOS_CONTENT_PATH_PROTECTION)
void secmem_crypto_register(struct secmem_crypto_driver_ftn *ftn);
void secmem_crypto_deregister(void);
#else
#define secmem_crypto_register(ftn)
#define secmem_crypto_deregister()
#endif

int drm_gsc_enable_locked(bool enable);

#define SECMEM_IOC_CHUNKINFO		_IOWR('S', 1, struct secchunk_info)
#define SECMEM_IOC_SET_DRM_ONOFF		_IOWR('S', 2, int)
#define SECMEM_IOC_GET_DRM_ONOFF		_IOWR('S', 3, int)
#define SECMEM_IOC_GET_CRYPTO_LOCK		_IOR('S', 4, int)
#define SECMEM_IOC_RELEASE_CRYPTO_LOCK	_IOR('S', 5, int)
#define SECMEM_IOC_GET_FD_PHYS_ADDR	_IOWR('S', 8, struct secfd_info)
#define SECMEM_IOC_GET_CHUNK_NUM		_IOWR('S', 9, int)
/* Absent from the supplied 4.4 header; vendor request is 0xc004530a. */
#define SECMEM_IOC_MIF_LOCK		_IOWR('S', 10, int)
#define SECMEM_IOC_SET_TZPC		_IOWR('S', 11, struct protect_info)
#define SECMEM_IOC_SET_PROTECT		_IOWR('S', 12, int)
#define SECMEM_IOC_SET_VIDEO_EXT_PROC	_IOWR('S', 13, int)
#define SECMEM_IOC_GET_DRM_PROT_VER		_IOWR('S', 14, int)

#endif /* __ASM_ARCH_SECMEM_H */
