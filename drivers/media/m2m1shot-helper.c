// SPDX-License-Identifier: GPL-2.0
/*
 * drivers/media/m2m1shot-helper.c
 *
 * Copyright (C) 2014 Samsung Electronics Co., Ltd.
 *
 * 5.10 M20 port: native DMA-BUF attachments and pinned user pages mapped
 * through the device's IOMMU DMA domain replace ION/IOVMM.
 */
#include <linux/kernel.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/scatterlist.h>

#include <media/m2m1shot-helper.h>

int m2m1shot_map_dma_buf(struct device *dev,
			struct m2m1shot_buffer_plane_dma *plane,
			enum dma_data_direction dir)
{
	if (plane->dmabuf) {
		plane->sgt = dma_buf_map_attachment(plane->attachment, dir);
		if (IS_ERR(plane->sgt)) {
			dev_err(dev, "%s: failed to map attacment of dma_buf\n",
					__func__);
			return PTR_ERR(plane->sgt);
		}
	}

	return 0;
}
EXPORT_SYMBOL(m2m1shot_map_dma_buf);

void m2m1shot_unmap_dma_buf(struct device *dev,
			struct m2m1shot_buffer_plane_dma *plane,
			enum dma_data_direction dir)
{
	if (plane->dmabuf)
		dma_buf_unmap_attachment(plane->attachment, plane->sgt, dir);
}
EXPORT_SYMBOL(m2m1shot_unmap_dma_buf);

/* An IOMMU DMA mapping of a scatterlist is one contiguous IOVA range. */
static int m2m1shot_sgt_iova(struct device *dev, struct sg_table *sgt,
			     size_t need, dma_addr_t *iova)
{
	struct scatterlist *sg;
	dma_addr_t expect;
	size_t len = 0;
	int i;

	if (!sgt->nents)
		return -EINVAL;
	expect = sg_dma_address(sgt->sgl);
	for_each_sgtable_dma_sg(sgt, sg, i) {
		if (sg_dma_address(sg) != expect)
			break;
		len += sg_dma_len(sg);
		expect += sg_dma_len(sg);
	}
	if (len < need) {
		dev_err(dev, "%s: %zu contiguous DMA bytes, need %zu\n",
			__func__, len, need);
		return -EFAULT;
	}
	*iova = sg_dma_address(sgt->sgl);
	return 0;
}

static int m2m1shot_pin_userptr(struct device *dev,
				struct m2m1shot_buffer_plane_dma *plane,
				unsigned long uaddr, enum dma_data_direction dir)
{
	unsigned long first = uaddr & PAGE_MASK;
	unsigned long last = PAGE_ALIGN(uaddr + plane->bytes_used);
	unsigned int n = (last - first) >> PAGE_SHIFT;
	int pinned, ret;

	if (!plane->bytes_used || last <= first)
		return -EINVAL;
	plane->upages = kvcalloc(n, sizeof(*plane->upages), GFP_KERNEL);
	if (!plane->upages)
		return -ENOMEM;
	pinned = pin_user_pages_fast(first, n,
			dir == DMA_TO_DEVICE ? 0 : FOLL_WRITE, plane->upages);
	if (pinned != n) {
		if (pinned > 0)
			unpin_user_pages(plane->upages, pinned);
		ret = pinned < 0 ? pinned : -EFAULT;
		goto err_free;
	}
	plane->nr_upages = n;
	ret = sg_alloc_table_from_pages(&plane->usgt, plane->upages, n,
					uaddr & ~PAGE_MASK, plane->bytes_used,
					GFP_KERNEL);
	if (ret)
		goto err_unpin;
	ret = dma_map_sgtable(dev, &plane->usgt, dir, 0);
	if (ret)
		goto err_sgt;
	return 0;
err_sgt:
	sg_free_table(&plane->usgt);
err_unpin:
	unpin_user_pages(plane->upages, n);
	plane->nr_upages = 0;
err_free:
	kvfree(plane->upages);
	plane->upages = NULL;
	return ret;
}

static void m2m1shot_unpin_userptr(struct device *dev,
				   struct m2m1shot_buffer_plane_dma *plane,
				   enum dma_data_direction dir)
{
	if (!plane->upages)
		return;
	dma_unmap_sgtable(dev, &plane->usgt, dir, 0);
	sg_free_table(&plane->usgt);
	unpin_user_pages_dirty_lock(plane->upages, plane->nr_upages,
				    dir != DMA_TO_DEVICE);
	kvfree(plane->upages);
	plane->upages = NULL;
	plane->nr_upages = 0;
}

int m2m1shot_dma_addr_map(struct device *dev,
			struct m2m1shot_buffer_dma *buf,
			int plane_idx, enum dma_data_direction dir)
{
	struct m2m1shot_buffer_plane_dma *plane = &buf->plane[plane_idx];
	dma_addr_t iova;
	int ret;

	if (plane->dmabuf) {
		ret = m2m1shot_sgt_iova(dev, plane->sgt,
					plane->offset + plane->bytes_used, &iova);
		if (ret)
			return ret;
		plane->dma_addr = iova + plane->offset;
		return 0;
	}

	ret = m2m1shot_pin_userptr(dev, plane,
				   buf->buffer->plane[plane_idx].userptr, dir);
	if (ret)
		return ret;
	/* sg_alloc_table_from_pages() already carries the in-page offset. */
	ret = m2m1shot_sgt_iova(dev, &plane->usgt, plane->bytes_used, &iova);
	if (ret) {
		m2m1shot_unpin_userptr(dev, plane, dir);
		return ret;
	}
	plane->dma_addr = iova;
	plane->offset = 0;
	plane->priv = (void *)(unsigned long)dir;
	return 0;
}

void m2m1shot_dma_addr_unmap(struct device *dev,
			struct m2m1shot_buffer_dma *buf, int plane_idx)
{
	struct m2m1shot_buffer_plane_dma *plane = &buf->plane[plane_idx];

	if (!plane->dmabuf)
		m2m1shot_unpin_userptr(dev, plane,
			(enum dma_data_direction)(unsigned long)plane->priv);
	plane->dma_addr = 0;
}

void m2m1shot_sync_for_device(struct device *dev,
			      struct m2m1shot_buffer_plane_dma *plane,
			      enum dma_data_direction dir)
{
	/* map_attachment/dma_map_sgtable already handed ownership to the
	 * device; re-sync explicitly to cover reused attachments. */
	if (plane->dmabuf && !IS_ERR_OR_NULL(plane->sgt))
		dma_sync_sgtable_for_device(dev, plane->sgt, dir);
	else if (plane->upages)
		dma_sync_sgtable_for_device(dev, &plane->usgt, dir);
}
EXPORT_SYMBOL(m2m1shot_sync_for_device);

void m2m1shot_sync_for_cpu(struct device *dev,
			   struct m2m1shot_buffer_plane_dma *plane,
			   enum dma_data_direction dir)
{
	if (plane->dmabuf && !IS_ERR_OR_NULL(plane->sgt))
		dma_sync_sgtable_for_cpu(dev, plane->sgt, dir);
	else if (plane->upages)
		dma_sync_sgtable_for_cpu(dev, &plane->usgt, dir);
}
EXPORT_SYMBOL(m2m1shot_sync_for_cpu);
