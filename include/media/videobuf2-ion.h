/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Samsung 4.4 vb2-ion API as used by the Exynos FIMC-IS driver.
 *
 * TODO(5.10 port): the Android ION/IOVMM allocator is not present in 5.10.
 * This header keeps the 4.4 call surface; the implementation in
 * drivers/media/platform/exynos/fimc-legacy/fimc-is-vb2-ion.c is backed by
 * vb2-dma-contig and the device's IOMMU domain.
 */
#ifndef _MEDIA_VIDEOBUF2_ION_H
#define _MEDIA_VIDEOBUF2_ION_H

#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <media/videobuf2-dma-contig.h>

#define VB2ION_CTX_VMCONTIG		(1 << 0)
#define VB2ION_CTX_IOMMU		(1 << 9)
#define VB2ION_CTX_UNCACHED		(1 << 10)
#define VB2ION_CTX_COHERENT_DMA		(1 << 11)
#define VB2ION_CTX_UNCACHED_READ_DMA	(1 << 12)

struct device;
struct vb2_buffer;
struct vb2_alloc_ctx;

void *vb2_ion_create_context(struct device *dev, size_t alignment, long flags);
struct device *vb2_ion_context_device(void *ctx);
void vb2_ion_destroy_context(void *ctx);
void vb2_ion_set_cached(void *ctx, bool cached);
void vb2_ion_set_noncoherent_dma_read(void *ctx, bool noncoherent);
int vb2_ion_set_alignment(void *ctx, size_t alignment);

/* The cookie of a vb2-dma-contig plane and of a private buffer both start
 * with the DMA address, so one accessor serves both.
 */
int vb2_ion_dma_address(void *cookie, dma_addr_t *dma_addr);

void *vb2_ion_private_vaddr(void *cookie);
void *vb2_ion_private_alloc(void *alloc_ctx, size_t size);
void vb2_ion_private_free(void *cookie);

/* Private coherent allocation cookies only, never vb2_plane_cookie(). */
void vb2_ion_sync_for_device(void *cookie, off_t offset, size_t size,
			     enum dma_data_direction dir);
void vb2_ion_sync_for_cpu(void *cookie, off_t offset, size_t size,
			  enum dma_data_direction dir);

int vb2_ion_attach_iommu(void *alloc_ctx);
void vb2_ion_detach_iommu(void *alloc_ctx);

#define vb2_ion_memops vb2_dma_contig_memops

#endif
