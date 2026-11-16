// SPDX-License-Identifier: GPL-2.0
/*
 * ION Memory Allocator - dmabuf interface
 *
 * Copyright (c) 2019, Google, Inc.
 */

#include <linux/device.h>
#if IS_ENABLED(CONFIG_EXYNOS_IOVMM)
#include <linux/exynos_iovmm.h>
#endif
#include <linux/mm.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "ion_private.h"

#if IS_ENABLED(CONFIG_EXYNOS_IOVMM)
struct ion_iovmm_mapping {
	struct list_head node;
	struct dma_buf_attachment *attachment;
	dma_addr_t iova;
	off_t offset;
	size_t size;
	enum dma_data_direction direction;
	int prot;
	unsigned int users;
};
static LIST_HEAD(ion_iovmm_mappings);
static DEFINE_MUTEX(ion_iovmm_lock);

static void ion_iovmm_detach(struct dma_buf_attachment *attachment)
{
	struct ion_iovmm_mapping *map, *tmp;

	mutex_lock(&ion_iovmm_lock);
	list_for_each_entry_safe(map, tmp, &ion_iovmm_mappings, node) {
		if (map->attachment != attachment)
			continue;
		WARN_ON(map->users);
		iovmm_unmap(attachment->dev, map->iova);
		list_del(&map->node);
		kfree(map);
	}
	mutex_unlock(&ion_iovmm_lock);
}
#endif

static struct sg_table *dup_sg_table(struct sg_table *table)
{
	struct sg_table *new_table;
	int ret, i;
	struct scatterlist *sg, *new_sg;

	new_table = kzalloc(sizeof(*new_table), GFP_KERNEL);
	if (!new_table)
		return ERR_PTR(-ENOMEM);

	ret = sg_alloc_table(new_table, table->nents, GFP_KERNEL);
	if (ret) {
		kfree(new_table);
		return ERR_PTR(-ENOMEM);
	}

	new_sg = new_table->sgl;
	for_each_sg(table->sgl, sg, table->nents, i) {
		memcpy(new_sg, sg, sizeof(*sg));
		new_sg->dma_address = 0;
		new_sg = sg_next(new_sg);
	}

	return new_table;
}

static void free_duped_table(struct sg_table *table)
{
	sg_free_table(table);
	kfree(table);
}

static int ion_dma_buf_attach(struct dma_buf *dmabuf,
			      struct dma_buf_attachment *attachment)
{
	struct ion_dma_buf_attachment *a;
	struct sg_table *table;
	struct ion_buffer *buffer = dmabuf->priv;
	struct ion_heap *heap = buffer->heap;

	if (heap->buf_ops.attach)
		return heap->buf_ops.attach(dmabuf, attachment);

	a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;

	table = dup_sg_table(buffer->sg_table);
	if (IS_ERR(table)) {
		kfree(a);
		return -ENOMEM;
	}

	a->table = table;
	a->dev = attachment->dev;
	INIT_LIST_HEAD(&a->list);
	a->mapped = false;

	attachment->priv = a;

	mutex_lock(&buffer->lock);
	list_add(&a->list, &buffer->attachments);
	mutex_unlock(&buffer->lock);

	return 0;
}

static void ion_dma_buf_detatch(struct dma_buf *dmabuf,
				struct dma_buf_attachment *attachment)
{
	struct ion_dma_buf_attachment *a = attachment->priv;
	struct ion_buffer *buffer = dmabuf->priv;
	struct ion_heap *heap = buffer->heap;

#if IS_ENABLED(CONFIG_EXYNOS_IOVMM)
	ion_iovmm_detach(attachment);
#endif
	if (heap->buf_ops.detach)
		return heap->buf_ops.detach(dmabuf, attachment);

	mutex_lock(&buffer->lock);
	list_del(&a->list);
	mutex_unlock(&buffer->lock);
	free_duped_table(a->table);

	kfree(a);
}

static struct sg_table *ion_map_dma_buf(struct dma_buf_attachment *attachment,
					enum dma_data_direction direction)
{
	struct ion_buffer *buffer = attachment->dmabuf->priv;
	struct ion_heap *heap = buffer->heap;
	struct ion_dma_buf_attachment *a;
	struct sg_table *table;
	unsigned long attrs = attachment->dma_map_attrs;

	if (heap->buf_ops.map_dma_buf)
		return heap->buf_ops.map_dma_buf(attachment, direction);

	a = attachment->priv;
	table = a->table;

	if (!(buffer->flags & ION_FLAG_CACHED))
		attrs |= DMA_ATTR_SKIP_CPU_SYNC;

	if (!dma_map_sg_attrs(attachment->dev, table->sgl, table->nents,
			      direction, attrs))
		return ERR_PTR(-ENOMEM);

	a->mapped = true;

	return table;
}

static void ion_unmap_dma_buf(struct dma_buf_attachment *attachment,
			      struct sg_table *table,
			      enum dma_data_direction direction)
{
	struct ion_buffer *buffer = attachment->dmabuf->priv;
	struct ion_heap *heap = buffer->heap;
	struct ion_dma_buf_attachment *a = attachment->priv;
	unsigned long attrs = attachment->dma_map_attrs;

	if (heap->buf_ops.unmap_dma_buf)
		return heap->buf_ops.unmap_dma_buf(attachment, table,
						   direction);

	a->mapped = false;

	if (!(buffer->flags & ION_FLAG_CACHED))
		attrs |= DMA_ATTR_SKIP_CPU_SYNC;

	dma_unmap_sg_attrs(attachment->dev, table->sgl, table->nents,
			   direction, attrs);
}

static void ion_dma_buf_release(struct dma_buf *dmabuf)
{
	struct ion_buffer *buffer = dmabuf->priv;
	struct ion_heap *heap = buffer->heap;

	if (heap->buf_ops.release)
		return heap->buf_ops.release(dmabuf);

	ion_free(buffer);
}

static int ion_dma_buf_begin_cpu_access(struct dma_buf *dmabuf,
					enum dma_data_direction direction)
{
	struct ion_buffer *buffer = dmabuf->priv;
	struct ion_heap *heap = buffer->heap;
	struct ion_dma_buf_attachment *a;

	if (heap->buf_ops.begin_cpu_access)
		return heap->buf_ops.begin_cpu_access(dmabuf, direction);

	mutex_lock(&buffer->lock);
	if (!(buffer->flags & ION_FLAG_CACHED))
		goto unlock;

	list_for_each_entry(a, &buffer->attachments, list) {
		if (!a->mapped)
			continue;
		dma_sync_sg_for_cpu(a->dev, a->table->sgl, a->table->nents,
				    direction);
	}

unlock:
	mutex_unlock(&buffer->lock);
	return 0;
}

static int
ion_dma_buf_begin_cpu_access_partial(struct dma_buf *dmabuf,
				     enum dma_data_direction direction,
				     unsigned int offset, unsigned int len)
{
	struct ion_buffer *buffer = dmabuf->priv;
	struct ion_heap *heap = buffer->heap;

	/* This is done to make sure partial buffer cache flush / invalidate is
	 * allowed. The implementation may be vendor specific in this case, so
	 * ion core does not provide a default implementation
	 */
	if (!heap->buf_ops.begin_cpu_access_partial)
		return -EOPNOTSUPP;

	return heap->buf_ops.begin_cpu_access_partial(dmabuf, direction, offset,
						      len);
}

static int ion_dma_buf_end_cpu_access(struct dma_buf *dmabuf,
				      enum dma_data_direction direction)
{
	struct ion_buffer *buffer = dmabuf->priv;
	struct ion_heap *heap = buffer->heap;
	struct ion_dma_buf_attachment *a;

	if (heap->buf_ops.end_cpu_access)
		return heap->buf_ops.end_cpu_access(dmabuf, direction);

	mutex_lock(&buffer->lock);
	if (!(buffer->flags & ION_FLAG_CACHED))
		goto unlock;

	list_for_each_entry(a, &buffer->attachments, list) {
		if (!a->mapped)
			continue;
		dma_sync_sg_for_device(a->dev, a->table->sgl, a->table->nents,
				       direction);
	}
unlock:
	mutex_unlock(&buffer->lock);

	return 0;
}

static int ion_dma_buf_end_cpu_access_partial(struct dma_buf *dmabuf,
					      enum dma_data_direction direction,
					      unsigned int offset,
					      unsigned int len)
{
	struct ion_buffer *buffer = dmabuf->priv;
	struct ion_heap *heap = buffer->heap;

	/* This is done to make sure partial buffer cache flush / invalidate is
	 * allowed. The implementation may be vendor specific in this case, so
	 * ion core does not provide a default implementation
	 */
	if (!heap->buf_ops.end_cpu_access_partial)
		return -EOPNOTSUPP;

	return heap->buf_ops.end_cpu_access_partial(dmabuf, direction, offset,
						    len);
}

static int ion_dma_buf_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct ion_buffer *buffer = dmabuf->priv;
	struct ion_heap *heap = buffer->heap;
	int ret;

	/* now map it to userspace */
	if (heap->buf_ops.mmap) {
		ret = heap->buf_ops.mmap(dmabuf, vma);
	} else {
		mutex_lock(&buffer->lock);
		if (!(buffer->flags & ION_FLAG_CACHED))
			vma->vm_page_prot =
				pgprot_writecombine(vma->vm_page_prot);

		ret = ion_heap_map_user(heap, buffer, vma);
		mutex_unlock(&buffer->lock);
	}

	if (ret)
		pr_err("%s: failure mapping buffer to userspace\n", __func__);

	return ret;
}

static void *ion_dma_buf_vmap(struct dma_buf *dmabuf)
{
	struct ion_buffer *buffer = dmabuf->priv;
	struct ion_heap *heap = buffer->heap;
	void *vaddr;

	if (heap->buf_ops.vmap)
		return heap->buf_ops.vmap(dmabuf);

	mutex_lock(&buffer->lock);
	vaddr = ion_buffer_kmap_get(buffer);
	mutex_unlock(&buffer->lock);

	return vaddr;
}

static void ion_dma_buf_vunmap(struct dma_buf *dmabuf, void *vaddr)
{
	struct ion_buffer *buffer = dmabuf->priv;
	struct ion_heap *heap = buffer->heap;

	if (heap->buf_ops.vunmap) {
		heap->buf_ops.vunmap(dmabuf, vaddr);
		return;
	}

	mutex_lock(&buffer->lock);
	ion_buffer_kmap_put(buffer);
	mutex_unlock(&buffer->lock);
}

static int ion_dma_buf_get_flags(struct dma_buf *dmabuf, unsigned long *flags)
{
	struct ion_buffer *buffer = dmabuf->priv;
	struct ion_heap *heap = buffer->heap;

	if (!heap->buf_ops.get_flags)
		return -EOPNOTSUPP;

	return heap->buf_ops.get_flags(dmabuf, flags);
}

static const struct dma_buf_ops dma_buf_ops = {
	.attach = ion_dma_buf_attach,
	.detach = ion_dma_buf_detatch,
	.map_dma_buf = ion_map_dma_buf,
	.unmap_dma_buf = ion_unmap_dma_buf,
	.release = ion_dma_buf_release,
	.begin_cpu_access = ion_dma_buf_begin_cpu_access,
	.begin_cpu_access_partial = ion_dma_buf_begin_cpu_access_partial,
	.end_cpu_access = ion_dma_buf_end_cpu_access,
	.end_cpu_access_partial = ion_dma_buf_end_cpu_access_partial,
	.mmap = ion_dma_buf_mmap,
	.vmap = ion_dma_buf_vmap,
	.vunmap = ion_dma_buf_vunmap,
	.get_flags = ion_dma_buf_get_flags,
};

#if IS_ENABLED(CONFIG_EXYNOS_IOVMM)
/*
 * 4.4 ion.c:2180-2318 used ion_buffer::sg_table, not DMA-remapped addresses.
 * Preserve that distinction: decon already maps the attachment for cache
 * maintenance, and its separate IOVMM address is what DPP actually programs.
 * Track mappings per attachment rather than borrowing 4.4's ion_buffer layout.
 */
dma_addr_t ion_iovmm_map(struct dma_buf_attachment *attachment,
		       off_t offset, size_t size,
		       enum dma_data_direction direction, int prot)
{
	struct ion_iovmm_mapping *map;
	struct ion_buffer *buffer;
	dma_addr_t iova;

	if (!attachment || attachment->dmabuf->ops != &dma_buf_ops ||
	    offset < 0 || !size)
		return -EINVAL;
	buffer = attachment->dmabuf->priv;
	/* Old ION_FLAG_PROTECTED == 16: never map secure memory nonsecurely. */
	if ((buffer->flags & 16) || buffer->heap->buf_ops.attach ||
	    buffer->heap->buf_ops.map_dma_buf ||
	    (size_t)offset > buffer->size || size > buffer->size - offset)
		return -EINVAL;
	if (!(buffer->flags & ION_FLAG_CACHED))
		prot &= ~IOMMU_CACHE;
	mutex_lock(&ion_iovmm_lock);
	list_for_each_entry(map, &ion_iovmm_mappings, node) {
		if (map->attachment == attachment && map->offset == offset &&
		    map->size == size && map->direction == direction && map->prot == prot) {
			if (map->users == UINT_MAX) {
				iova = -EOVERFLOW;
				goto out;
			}
			map->users++;
			iova = map->iova;
			goto out;
		}
	}
	map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (!map) {
		iova = -ENOMEM;
		goto out;
	}
	iova = iovmm_map(attachment->dev, buffer->sg_table->sgl,
			  offset, size, direction, prot);
	if (IS_ERR_VALUE(iova)) {
		kfree(map);
		goto out;
	}
	map->attachment = attachment;
	map->iova = iova;
	map->offset = offset;
	map->size = size;
	map->direction = direction;
	map->prot = prot;
	map->users = 1;
	list_add_tail(&map->node, &ion_iovmm_mappings);
out:
	mutex_unlock(&ion_iovmm_lock);
	return iova;
}
EXPORT_SYMBOL_GPL(ion_iovmm_map);

void ion_iovmm_unmap(struct dma_buf_attachment *attachment, dma_addr_t iova)
{
	struct ion_iovmm_mapping *map;

	if (!attachment || attachment->dmabuf->ops != &dma_buf_ops)
		return;
	mutex_lock(&ion_iovmm_lock);
	list_for_each_entry(map, &ion_iovmm_mappings, node) {
		if (map->attachment != attachment || map->iova != iova)
			continue;
		if (!--map->users) {
			iovmm_unmap(attachment->dev, iova);
			list_del(&map->node);
			kfree(map);
		}
		mutex_unlock(&ion_iovmm_lock);
		return;
	}
	mutex_unlock(&ion_iovmm_lock);
	dev_warn(attachment->dev, "Unknown ION IOVMM address %pad\n", &iova);
}
EXPORT_SYMBOL_GPL(ion_iovmm_unmap);
#endif

struct dma_buf *ion_dmabuf_alloc(struct ion_device *dev, size_t len,
				 unsigned int heap_id_mask,
				 unsigned int flags)
{
	struct ion_buffer *buffer;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct dma_buf *dmabuf;

	pr_debug("%s: len %zu heap_id_mask %u flags %x\n", __func__,
		 len, heap_id_mask, flags);

	buffer = ion_buffer_alloc(dev, len, heap_id_mask, flags);
	if (IS_ERR(buffer))
		return ERR_CAST(buffer);

	exp_info.ops = &dma_buf_ops;
	exp_info.size = buffer->size;
	exp_info.flags = O_RDWR;
	exp_info.priv = buffer;

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf))
		ion_buffer_destroy(dev, buffer);

	return dmabuf;
}
