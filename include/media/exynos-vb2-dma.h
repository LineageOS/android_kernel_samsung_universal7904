/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MEDIA_EXYNOS_VB2_DMA_H
#define _MEDIA_EXYNOS_VB2_DMA_H

#include <linux/dma-mapping.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-dma-contig.h>
#include <media/v4l2-mem2mem.h>

static inline struct vb2_buffer *exynos_m2m_next_src_buf(struct v4l2_m2m_ctx *ctx)
{
	struct vb2_v4l2_buffer *vb = v4l2_m2m_next_src_buf(ctx);

	return vb ? &vb->vb2_buf : NULL;
}

static inline struct vb2_buffer *exynos_m2m_next_dst_buf(struct v4l2_m2m_ctx *ctx)
{
	struct vb2_v4l2_buffer *vb = v4l2_m2m_next_dst_buf(ctx);

	return vb ? &vb->vb2_buf : NULL;
}

/* These accelerators require contiguous IOVAs, not contiguous physical pages.
 * DMA-BUF import ownership is explicit because vb2 core skips its mem_ops
 * cache callbacks for imported buffers. MMAP/USERPTR remain core-managed.
 */
static inline void exynos_vb2_sync_for_device(struct vb2_buffer *vb)
{
	unsigned int i;

	if (vb->memory != VB2_MEMORY_DMABUF)
		return;
	for (i = 0; i < vb->num_planes; i++)
		if (vb->planes[i].mem_priv && vb->vb2_queue->mem_ops->prepare)
			vb->vb2_queue->mem_ops->prepare(vb->planes[i].mem_priv);
}

static inline void exynos_vb2_sync_for_cpu(struct vb2_buffer *vb)
{
	unsigned int i;

	if (vb->memory != VB2_MEMORY_DMABUF)
		return;
	for (i = 0; i < vb->num_planes; i++)
		if (vb->planes[i].mem_priv && vb->vb2_queue->mem_ops->finish)
			vb->vb2_queue->mem_ops->finish(vb->planes[i].mem_priv);
}

static inline size_t exynos_vb2_plane_dma_size(struct vb2_buffer *vb,
					      unsigned int plane)
{
	size_t size = vb2_plane_size(vb, plane);

	return vb->planes[plane].data_offset <= size ?
		size - vb->planes[plane].data_offset : 0;
}

static inline dma_addr_t exynos_vb2_plane_dma_addr(struct vb2_buffer *vb,
						 unsigned int plane)
{
	return vb2_dma_contig_plane_dma_addr(vb, plane) +
		vb->planes[plane].data_offset;
}

#endif
