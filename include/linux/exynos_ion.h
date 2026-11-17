/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Exynos extensions used by the 4.4 display stack, adapted to the 5.10 ION.
 *
 * 4.4 reference: include/linux/exynos_ion.h:27-34 (heap ids/masks) and
 * drivers/staging/android/ion/exynos/exynos_ion_sync.c:104-137.
 *
 * 5.10 ION has no ion_client/ion_handle layer. Kernel users hold dma-bufs
 * directly, so only the heap masks and the cache-sync helper that DECON
 * calls are kept here.
 */
#ifndef _LINUX_EXYNOS_ION_H
#define _LINUX_EXYNOS_ION_H

#include <linux/dma-buf.h>
#include <linux/dma-direction.h>
#include <linux/ion.h>

#define EXYNOS_ION_HEAP_SYSTEM_ID	0
#define EXYNOS_ION_HEAP_SYSTEM_MASK	(1 << EXYNOS_ION_HEAP_SYSTEM_ID)

/*
 * 4.4 flushed the CPU cache of cached ION buffers for the device and did
 * nothing for uncached ones. ion's end_cpu_access() does exactly that for
 * every attached device.
 */
static inline void exynos_ion_sync_dmabuf_for_device(struct device *dev,
						     struct dma_buf *dmabuf,
						     size_t size,
						     enum dma_data_direction dir)
{
	dma_buf_end_cpu_access(dmabuf, dir);
}

#endif /* _LINUX_EXYNOS_ION_H */
