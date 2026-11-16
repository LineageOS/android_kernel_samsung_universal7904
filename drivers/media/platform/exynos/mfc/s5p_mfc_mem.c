/*
 * drivers/media/platform/exynos/mfc/s5p_mfc_mem.c
 *
 * Copyright (c) 2016 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/dma-buf.h>
#include <linux/dma-iommu.h>
#include <linux/iommu.h>
#include <linux/slab.h>

#include "s5p_mfc_mem.h"

void *s5p_mfc_mem_alloc(void *alloc_ctx, size_t size)
{
	struct s5p_mfc_priv_buf *p;

	if (!alloc_ctx || !size)
		return ERR_PTR(-EINVAL);
	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return ERR_PTR(-ENOMEM);
	p->dev = alloc_ctx;
	p->size = PAGE_ALIGN(size);
	p->vaddr = dma_alloc_coherent(p->dev, p->size, &p->daddr, GFP_KERNEL);
	if (!p->vaddr) {
		kfree(p);
		return ERR_PTR(-ENOMEM);
	}
	return p;
}

/*
 * Map the pages of an existing coherent buffer a second time at the reserved
 * EXYNOS_MFC_FW_IOVA window. The MFC RISC rejects context/codec buffers below
 * its base, and 5.10 dma-iommu hands out IOVAs top-down, so the F/W must sit
 * lower than anything the allocator can return. CPU access keeps using the
 * coherent mapping; only the device address changes.
 */
int s5p_mfc_mem_alias_low(void *cookie)
{
	struct s5p_mfc_priv_buf *p = cookie;
	struct iommu_domain *domain;
	size_t off;
	int ret;

	if (IS_ERR_OR_NULL(p) || p->alias)
		return -EINVAL;
	if (p->size > EXYNOS_MFC_FW_IOVA_SIZE)
		return -E2BIG;
	domain = iommu_get_domain_for_dev(p->dev);
	if (!domain)
		return -ENODEV;
	for (off = 0; off < p->size; off += PAGE_SIZE) {
		phys_addr_t phys = iommu_iova_to_phys(domain, p->daddr + off);

		if (!phys) {
			ret = -EFAULT;
			goto unmap;
		}
		ret = iommu_map(domain, EXYNOS_MFC_FW_IOVA + off, phys, PAGE_SIZE,
				IOMMU_READ | IOMMU_WRITE);
		if (ret)
			goto unmap;
	}
	p->alias = EXYNOS_MFC_FW_IOVA;
	return 0;
unmap:
	if (off)
		iommu_unmap(domain, EXYNOS_MFC_FW_IOVA, off);
	return ret;
}

void s5p_mfc_mem_free(void *cookie)
{
	struct s5p_mfc_priv_buf *p = cookie;

	if (IS_ERR_OR_NULL(p))
		return;
	if (p->alias) {
		struct iommu_domain *domain = iommu_get_domain_for_dev(p->dev);

		if (domain)
			iommu_unmap(domain, p->alias, p->size);
		p->alias = 0;
	}
	dma_free_coherent(p->dev, p->size, p->vaddr, p->daddr);
	kfree(p);
}

const struct vb2_mem_ops *s5p_mfc_mem_ops(void)
{
	return &vb2_dma_contig_memops;
}

/* Imported DMA-BUFs carry their own attributes; nothing to switch here. */
void s5p_mfc_mem_set_cacheable(void *alloc_ctx, bool cacheable)
{
}

/* Private buffers are coherent: only order CPU writes/reads vs. MFC. */
void s5p_mfc_mem_clean(void *vb_priv, void *start, off_t offset,
							size_t size)
{
	dma_wmb();
}

void s5p_mfc_mem_invalidate(void *vb_priv, void *start, off_t offset,
							size_t size)
{
	dma_rmb();
}

int s5p_mfc_mem_clean_vb(struct vb2_buffer *vb, u32 num_planes)
{
	exynos_vb2_sync_for_device(vb);
	return 0;
}

int s5p_mfc_mem_inv_vb(struct vb2_buffer *vb, u32 num_planes)
{
	exynos_vb2_sync_for_cpu(vb);
	return 0;
}

int s5p_mfc_mem_get_user_shared_handle(struct s5p_mfc_ctx *ctx,
	struct mfc_user_shared_handle *handle)
{
	struct dma_buf *dma_buf;
	void *vaddr;

	dma_buf = dma_buf_get(handle->fd);
	if (IS_ERR(dma_buf)) {
		mfc_err_ctx("Failed to dma_buf_get (err %ld)\n", PTR_ERR(dma_buf));
		return -EINVAL;
	}
	if (dma_buf->size < handle->data_size) {
		mfc_err_ctx("User-provided dma_buf size(%zu) is smaller than required size(%zu)\n",
				dma_buf->size, handle->data_size);
		dma_buf_put(dma_buf);
		return -EINVAL;
	}
	vaddr = dma_buf_vmap(dma_buf);
	if (!vaddr) {
		mfc_err_ctx("Failed to get kernel virtual address\n");
		dma_buf_put(dma_buf);
		return -EINVAL;
	}
	handle->dma_buf = dma_buf;
	handle->vaddr = vaddr;
	mfc_debug(2, "User Handle: fd = %d, virtual addr = 0x%p\n",
				handle->fd, handle->vaddr);
	return 0;
}

int s5p_mfc_mem_cleanup_user_shared_handle(struct s5p_mfc_ctx *ctx,
		struct mfc_user_shared_handle *handle)
{
	if (handle->fd == -1 || !handle->dma_buf)
		return 0;
	if (handle->vaddr)
		dma_buf_vunmap(handle->dma_buf, handle->vaddr);
	dma_buf_put(handle->dma_buf);
	handle->dma_buf = NULL;
	handle->vaddr = NULL;
	return 0;
}
