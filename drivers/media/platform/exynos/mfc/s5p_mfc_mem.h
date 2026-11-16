/*
 * drivers/media/platform/exynos/mfc/s5p_mfc_mem.h
 *
 * Copyright (c) 2016 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * 5.10 M20 port: ION/vb2-ion replaced with vb2-dma-contig for V4L2
 * buffers and dma_alloc_coherent() for driver-private firmware/context
 * buffers, all in the device's translated IOMMU domain.
 */

#ifndef __S5P_MFC_MEM_H
#define __S5P_MFC_MEM_H __FILE__

#include <linux/dma-mapping.h>
#include <media/videobuf2-dma-contig.h>
#include <media/exynos-vb2-dma.h>

#include "s5p_mfc_common.h"

/* Offset base used to differentiate between CAPTURE and OUTPUT
*  while mmaping */
#define DST_QUEUE_OFF_BASE      (TASK_SIZE / 2)

/* Driver-private coherent allocation; alloc_ctx is the MFC struct device. */
struct s5p_mfc_priv_buf {
	struct device *dev;
	void *vaddr;
	dma_addr_t daddr;
	size_t size;
	/* Low IOMMU alias of the same pages (F/W only), 0 if none. */
	dma_addr_t alias;
};

void *s5p_mfc_mem_alloc(void *alloc_ctx, size_t size);
void s5p_mfc_mem_free(void *cookie);
int s5p_mfc_mem_alias_low(void *cookie);

static inline dma_addr_t s5p_mfc_mem_get_daddr(void *cookie)
{
	struct s5p_mfc_priv_buf *p = cookie;

	return p->alias ? p->alias : p->daddr;
}

static inline dma_addr_t s5p_mfc_mem_get_daddr_vb(
	struct vb2_buffer *v, unsigned int n)
{
	return vb2_dma_contig_plane_dma_addr(v, n);
}

static inline void *s5p_mfc_mem_get_vaddr(void *cookie)
{
	return ((struct s5p_mfc_priv_buf *)cookie)->vaddr;
}

static inline int s5p_mfc_mem_buf_prepare(struct vb2_buffer *vb)
{
	exynos_vb2_sync_for_device(vb);
	return 0;
}

static inline void s5p_mfc_mem_buf_finish(struct vb2_buffer *vb)
{
	exynos_vb2_sync_for_cpu(vb);
}

const struct vb2_mem_ops *s5p_mfc_mem_ops(void);
void s5p_mfc_mem_set_cacheable(void *alloc_ctx, bool cacheable);
void s5p_mfc_mem_clean(void *vb_priv, void *start, off_t offset,
							size_t size);
void s5p_mfc_mem_invalidate(void *vb_priv, void *start, off_t offset,
							size_t size);
int s5p_mfc_mem_clean_vb(struct vb2_buffer *vb, u32 num_planes);
int s5p_mfc_mem_inv_vb(struct vb2_buffer *vb, u32 num_planes);

int s5p_mfc_mem_get_user_shared_handle(struct s5p_mfc_ctx *ctx,
		struct mfc_user_shared_handle *handle);
int s5p_mfc_mem_cleanup_user_shared_handle(struct s5p_mfc_ctx *ctx,
		struct mfc_user_shared_handle *handle);

#endif /* __S5P_MFC_MEM_H */
