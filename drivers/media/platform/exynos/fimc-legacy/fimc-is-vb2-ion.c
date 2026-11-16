// SPDX-License-Identifier: GPL-2.0-only
/*
 * vb2-ion call surface for FIMC-IS on top of dma-contig/IOMMU.
 * TODO(5.10 port): not equivalent to the 4.4 ION allocator (no heap flags,
 * no cache-attribute control, one backing device); untested on hardware.
 */
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <media/videobuf2-ion.h>

struct fimc_is_vb2_ctx {
	struct device *dev;
	size_t align;
	long flags;
};

struct fimc_is_vb2_priv {
	dma_addr_t dma;		/* must stay first, see vb2_ion_dma_address() */
	void *vaddr;
	size_t size;
	struct device *dev;
};

void *vb2_ion_create_context(struct device *dev, size_t alignment, long flags)
{
	struct fimc_is_vb2_ctx *ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);

	if (!ctx)
		return ERR_PTR(-ENOMEM);
	ctx->dev = dev;
	ctx->align = alignment;
	ctx->flags = flags;
	return ctx;
}

struct device *vb2_ion_context_device(void *ctx)
{
	struct fimc_is_vb2_ctx *c = ctx;

	return IS_ERR_OR_NULL(c) ? NULL : c->dev;
}

void vb2_ion_destroy_context(void *ctx)
{
	kfree(ctx);
}

void vb2_ion_set_cached(void *ctx, bool cached) { }
void vb2_ion_set_noncoherent_dma_read(void *ctx, bool noncoherent) { }

int vb2_ion_set_alignment(void *ctx, size_t alignment)
{
	struct fimc_is_vb2_ctx *c = ctx;

	if (!c)
		return -EINVAL;
	c->align = alignment;
	return 0;
}

int vb2_ion_dma_address(void *cookie, dma_addr_t *dma_addr)
{
	if (WARN_ON(!dma_addr || IS_ERR_OR_NULL(cookie)))
		return -EINVAL;
	*dma_addr = *(dma_addr_t *)cookie;
	return 0;
}

void *vb2_ion_private_vaddr(void *cookie)
{
	struct fimc_is_vb2_priv *p = cookie;

	return IS_ERR_OR_NULL(p) ? ERR_PTR(-EINVAL) : p->vaddr;
}

void *vb2_ion_private_alloc(void *alloc_ctx, size_t size)
{
	struct fimc_is_vb2_ctx *ctx = alloc_ctx;
	struct fimc_is_vb2_priv *p;

	if (!ctx)
		return ERR_PTR(-EINVAL);
	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return ERR_PTR(-ENOMEM);
	p->dev = ctx->dev;
	p->size = PAGE_ALIGN(size);
	p->vaddr = dma_alloc_coherent(p->dev, p->size, &p->dma, GFP_KERNEL);
	if (!p->vaddr) {
		kfree(p);
		return ERR_PTR(-ENOMEM);
	}
	return p;
}

void vb2_ion_private_free(void *cookie)
{
	struct fimc_is_vb2_priv *p = cookie;

	if (IS_ERR_OR_NULL(p))
		return;
	dma_free_coherent(p->dev, p->size, p->vaddr, p->dma);
	kfree(p);
}

/* These cookies belong only to private dma_alloc_coherent() buffers.
 * Coherent allocations need ordering, not streaming-map cache maintenance.
 * Imported image planes use the vb2 allocator's SG-aware mem_ops instead.
 */
static bool fimc_is_vb2_private_range_valid(void *cookie, off_t offset,
					 size_t size)
{
	struct fimc_is_vb2_priv *p = cookie;

	if (IS_ERR_OR_NULL(p))
		return false;
	if (WARN_ON(offset < 0 || (size_t)offset > p->size ||
		    size > p->size - (size_t)offset))
		return false;
	return true;
}

void vb2_ion_sync_for_device(void *cookie, off_t offset, size_t size,
			     enum dma_data_direction dir)
{
	if (fimc_is_vb2_private_range_valid(cookie, offset, size))
		dma_wmb();
}

void vb2_ion_sync_for_cpu(void *cookie, off_t offset, size_t size,
			  enum dma_data_direction dir)
{
	if (fimc_is_vb2_private_range_valid(cookie, offset, size))
		dma_rmb();
}

/* The IOMMU domain is attached by the iommu core / runtime PM. */
int vb2_ion_attach_iommu(void *alloc_ctx) { return 0; }
void vb2_ion_detach_iommu(void *alloc_ctx) { }
