// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung Exynos display IOVMM.
 * Copyright (c) 2011-2012 Samsung Electronics Co., Ltd.
 *
 * Based on the 4.4 exynos-iovmm.c allocator/map/one-to-one interfaces.
 * A single controller, group and page table serve all four display IDMA ports.
 */
#include <linux/bitmap.h>
#include <linux/err.h>
#include <linux/exynos_iovmm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/sizes.h>
#include <linux/slab.h>

#include "exynos-sysmmu-7885.h"

/* Legacy exynos7885.dtsi:1228-1236 display dma-window, exclusive end. */
#define DISPLAY_IOVA_START 0x10000000U
#define DISPLAY_IOVA_SIZE  0xefff0000U

static struct exynos_iovmm *exynos_iovmm_get(struct device *dev)
{
	struct exynos_iommu_owner *owner = dev_iommu_priv_get(dev);
	struct sysmmu_drvdata *data;
	struct exynos_iovmm *vmm;
	u32 window[2] = { DISPLAY_IOVA_START, DISPLAY_IOVA_SIZE };

	if (!owner || !owner->data->group)
		return ERR_PTR(-ENODEV);
	data = owner->data;
	mutex_lock(&data->mutex);
	if (READ_ONCE(data->legacy_reservation_failed)) {
		vmm = ERR_PTR(-ENOMEM);
		goto out;
	}
	vmm = data->vmm;
	if (vmm)
		goto out;
	/* The legacy bus dma-window moves to its sole SYSMMU provider node. */
	if (of_find_property(data->sysmmu->of_node, "dma-window", NULL) &&
	    of_property_read_u32_array(data->sysmmu->of_node, "dma-window", window, 2)) {
		vmm = ERR_PTR(-EINVAL);
		goto out;
	}
	if (window[0] < DISPLAY_IOVA_START || !window[1] ||
	    !IS_ALIGNED(window[0] | window[1], PAGE_SIZE) ||
	    (u64)window[0] + window[1] > (1ULL << 32)) {
		vmm = ERR_PTR(-EINVAL);
		goto out;
	}
	vmm = kzalloc(sizeof(*vmm), GFP_KERNEL);
	if (!vmm) {
		vmm = ERR_PTR(-ENOMEM);
		goto out;
	}
	vmm->iova_start = window[0];
	vmm->iovm_size = window[1];
	vmm->vm_map = bitmap_zalloc(vmm->iovm_size >> PAGE_SHIFT, GFP_KERNEL);
	if (!vmm->vm_map)
		goto err_vmm;
	/* Borrow the controller's default DMA domain; never replace DMA ops' table. */
	vmm->domain = iommu_get_domain_for_dev(dev);
	if (!vmm->domain || vmm->domain->type != IOMMU_DOMAIN_DMA)
		goto err_bitmap;
	vmm->group = iommu_group_ref_get(data->group);
	mutex_init(&vmm->mutex);
	INIT_LIST_HEAD(&vmm->regions_list);
	data->vmm = vmm;
	data->release_iovmm = iovmm_deactivate;
	goto out;
err_bitmap:
	bitmap_free(vmm->vm_map);
err_vmm:
	kfree(vmm);
	vmm = ERR_PTR(-ENOMEM);
out:
	mutex_unlock(&data->mutex);
	return vmm;
}

int iovmm_activate(struct device *dev)
{
	struct exynos_iovmm *vmm = exynos_iovmm_get(dev);
	struct exynos_iommu_owner *owner;
	int ret = 0;

	if (IS_ERR(vmm))
		return PTR_ERR(vmm);
	owner = dev_iommu_priv_get(dev);
	mutex_lock(&vmm->mutex);
	if (owner->activated) {
		ret = -EEXIST;
		goto out;
	}
	/* The core attached the shared default DMA domain before driver probe. */
	if (owner->domain != vmm->domain) {
		ret = -EBUSY;
		goto out;
	}
	vmm->users++;
	owner->activated = true;
out:
	mutex_unlock(&vmm->mutex);
	/*
	 * 4.4 enabled translation here (iommu_attach_device), after the splash
	 * identity map. Never enable it before that map exists, whichever DPP
	 * activates first; DPP1-3 probing earlier simply leave it deferred.
	 */
	if (!ret && READ_ONCE(owner->data->translation_deferred)) {
		if (READ_ONCE(owner->data->handover_mapped))
			exynos_sysmmu_allow_translation(owner->data);
		else
			dev_warn(dev, "splash not identity-mapped yet; translation stays off\n");
	}
	return ret;
}
EXPORT_SYMBOL_GPL(iovmm_activate);

void iovmm_deactivate(struct device *dev)
{
	struct exynos_iovmm *vmm = exynos_get_iovmm(dev);
	struct exynos_iommu_owner *owner = dev_iommu_priv_get(dev);
	unsigned long flags;

	if (!vmm || !owner)
		return;
	mutex_lock(&vmm->mutex);
	spin_lock_irqsave(&owner->data->lock, flags);
	owner->fault_handler = NULL;
	owner->token = NULL;
	spin_unlock_irqrestore(&owner->data->lock, flags);
	if (owner->activated) {
		owner->activated = false;
		vmm->users--;
		/* DMA mappings may outlive this legacy activation reference. */
	}
	mutex_unlock(&vmm->mutex);
}
EXPORT_SYMBOL_GPL(iovmm_deactivate);

struct iommu_domain *get_domain_from_dev(struct device *dev)
{
	struct exynos_iovmm *vmm = exynos_iovmm_get(dev);

	return IS_ERR(vmm) ? NULL : vmm->domain;
}
EXPORT_SYMBOL_GPL(get_domain_from_dev);

/* Caller holds vmm->mutex for bitmap, region and page-table transaction. */
static struct exynos_vm_region *alloc_iovm_region(struct exynos_iovmm *vmm,
				size_t size, size_t section_off, off_t page_off)
{
	struct exynos_vm_region *region;
	unsigned long index, pages, total = vmm->iovm_size >> PAGE_SHIFT;
	size_t reserved;

	/* Keep the vendor 128 KiB prefetch guard and 1 MiB alignment. */
	if (size > U32_MAX - SZ_256K - section_off)
		return ERR_PTR(-EOVERFLOW);
	reserved = ALIGN(size + SZ_128K, SZ_128K) + section_off;
	pages = reserved >> PAGE_SHIFT;
	index = bitmap_find_next_zero_area_off(vmm->vm_map, total, 0, pages,
					     (SZ_1M >> PAGE_SHIFT) - 1,
					     vmm->iova_start >> PAGE_SHIFT);
	if (index >= total || pages > total - index)
		return ERR_PTR(-ENOMEM);
	region = kzalloc(sizeof(*region), GFP_KERNEL);
	if (!region)
		return ERR_PTR(-ENOMEM);
	bitmap_set(vmm->vm_map, index, pages);
	region->start = vmm->iova_start + (index << PAGE_SHIFT) + page_off;
	region->section_off = section_off;
	region->size = reserved;
	region->dummy_size = reserved - size;
	list_add_tail(&region->node, &vmm->regions_list);
	return region;
}

static void free_iovm_region(struct exynos_iovmm *vmm,
			     struct exynos_vm_region *region)
{
	bitmap_clear(vmm->vm_map,
		     (region->start - vmm->iova_start) >> PAGE_SHIFT,
		     region->size >> PAGE_SHIFT);
	list_del(&region->node);
	kfree(region);
}

static int mapping_prot(enum dma_data_direction direction, int prot)
{
	/* 4.4 ignored permissions, but preserve correct generic API direction. */
	switch (direction) {
	case DMA_TO_DEVICE:
		return prot | IOMMU_READ;
	case DMA_FROM_DEVICE:
		return prot | IOMMU_WRITE;
	case DMA_BIDIRECTIONAL:
		return prot | IOMMU_READ | IOMMU_WRITE;
	default:
		return -EINVAL;
	}
}

dma_addr_t iovmm_map(struct device *dev, struct scatterlist *sg, off_t offset,
		     size_t size, enum dma_data_direction direction, int prot)
{
	struct exynos_iovmm *vmm = exynos_iovmm_get(dev);
	struct exynos_vm_region *region;
	struct scatterlist *entry;
	dma_addr_t start;
	size_t available = 0;
	size_t mapped = 0, page_off, section_off = 0;
	int ret;

	if (IS_ERR(vmm))
		return PTR_ERR(vmm);
	if (!size || size > U32_MAX - PAGE_SIZE || offset < 0)
		return -EINVAL;
	prot = mapping_prot(direction, prot);
	if (prot < 0)
		return prot;
	while (sg && offset >= sg->length) {
		offset -= sg->length;
		sg = sg_next(sg);
	}
	if (!sg)
		return -EINVAL;
	/* Validate bytes before rounding the last physical page up. */
	for (entry = sg; entry && available < size + offset; entry = sg_next(entry)) {
		if (entry->length > SIZE_MAX - available)
			return -EOVERFLOW;
		available += entry->length;
	}
	if (available < size + offset)
		return -EINVAL;
	page_off = offset_in_page(sg_phys(sg) + offset);
	size = PAGE_ALIGN(size + page_off);
	if (!sg_next(sg))
		section_off = round_down(sg_phys(sg) + offset, PAGE_SIZE) & ~SECT_MASK;
	mutex_lock(&vmm->mutex);
	region = alloc_iovm_region(vmm, size, section_off, page_off);
	if (IS_ERR(region)) {
		ret = PTR_ERR(region);
		goto out_unlock;
	}
	start = region->start + section_off;
	while (sg && mapped < size) {
		phys_addr_t phys = sg_phys(sg) + offset;
		size_t len = sg->length - offset;
		size_t pad = offset_in_page(phys);

		/* Only the first chunk may have a subpage offset. */
		if ((mapped && pad) || (!sg_is_last(sg) &&
					!IS_ALIGNED(phys + len, PAGE_SIZE))) {
			ret = -EINVAL;
			goto err_map;
		}
		phys -= pad;
		len = min(PAGE_ALIGN(len + pad), size - mapped);
		ret = iommu_map(vmm->domain, start - page_off + mapped,
				phys, len, prot);
		if (ret)
			goto err_map;
		mapped += len;
		offset = 0;
		sg = sg_next(sg);
	}
	if (mapped != size) {
		ret = -EINVAL;
		goto err_map;
	}
	mutex_unlock(&vmm->mutex);
	return start;
err_map:
	if (mapped && iommu_unmap(vmm->domain, start - page_off, mapped) != mapped)
		panic("Unable to roll back display IOVMM mapping");
	free_iovm_region(vmm, region);
out_unlock:
	mutex_unlock(&vmm->mutex);
	return (dma_addr_t)ret;
}
EXPORT_SYMBOL_GPL(iovmm_map);

static void unmap_region(struct device *dev, dma_addr_t iova, bool one_to_one)
{
	struct exynos_iovmm *vmm = exynos_get_iovmm(dev);
	struct exynos_vm_region *region;

	if (!vmm)
		return;
	mutex_lock(&vmm->mutex);
	list_for_each_entry(region, &vmm->regions_list, node) {
		if (region->start + region->section_off != iova ||
		    region->one_to_one != one_to_one)
			continue;
		if (iommu_unmap(vmm->domain, round_down(iova, PAGE_SIZE),
				region->size - region->dummy_size) !=
				region->size - region->dummy_size)
			panic("Unable to unmap display IOVMM region");
		free_iovm_region(vmm, region);
		mutex_unlock(&vmm->mutex);
		return;
	}
	dev_warn(dev, "No %s IOVMM mapping at %pad\n",
		 one_to_one ? "identity" : "allocated", &iova);
	mutex_unlock(&vmm->mutex);
}

void iovmm_unmap(struct device *dev, dma_addr_t iova)
{
	unmap_region(dev, iova, false);
}
EXPORT_SYMBOL_GPL(iovmm_unmap);

int iovmm_map_oto(struct device *dev, phys_addr_t phys, size_t size)
{
	struct exynos_iovmm *vmm = exynos_iovmm_get(dev);
	struct exynos_vm_region *region;
	unsigned long index, pages;
	int ret = 0;

	if (IS_ERR(vmm))
		return PTR_ERR(vmm);
	if (!size || !IS_ALIGNED(phys | size, PAGE_SIZE) ||
	    phys < vmm->iova_start || phys >= (u64)vmm->iova_start + vmm->iovm_size ||
	    size > (u64)vmm->iova_start + vmm->iovm_size - phys)
		return -EINVAL;
	region = kzalloc(sizeof(*region), GFP_KERNEL);
	if (!region)
		return -ENOMEM;
	index = (phys - vmm->iova_start) >> PAGE_SHIFT;
	pages = size >> PAGE_SHIFT;
	mutex_lock(&vmm->mutex);
	if (find_next_bit(vmm->vm_map, index + pages, index) < index + pages) {
		ret = -EADDRINUSE;
		goto err;
	}
	ret = iommu_map(vmm->domain, phys, phys, size, IOMMU_READ | IOMMU_WRITE);
	if (ret)
		goto err;
	bitmap_set(vmm->vm_map, index, pages);
	region->start = phys;
	region->size = size;
	region->one_to_one = true;
	list_add_tail(&region->node, &vmm->regions_list);
	{
		struct sysmmu_drvdata *data =
			((struct exynos_iommu_owner *)dev_iommu_priv_get(dev))->data;

		if (data->handover_size && phys <= data->handover_base &&
		    (u64)phys + size >= (u64)data->handover_base + data->handover_size)
			WRITE_ONCE(data->handover_mapped, true);
	}
	mutex_unlock(&vmm->mutex);
	return 0;
err:
	mutex_unlock(&vmm->mutex);
	kfree(region);
	return ret;
}
EXPORT_SYMBOL_GPL(iovmm_map_oto);

void iovmm_unmap_oto(struct device *dev, phys_addr_t phys)
{
	unmap_region(dev, phys, true);
}
EXPORT_SYMBOL_GPL(iovmm_unmap_oto);

void iovmm_set_fault_handler(struct device *dev,
			     iommu_fault_handler_t handler, void *token)
{
	struct exynos_iommu_owner *owner = dev_iommu_priv_get(dev);
	unsigned long flags;

	if (!owner) {
		dev_err(dev, "No display SYSMMU for fault handler\n");
		return;
	}
	spin_lock_irqsave(&owner->data->lock, flags);
	owner->fault_handler = handler;
	owner->token = token;
	spin_unlock_irqrestore(&owner->data->lock, flags);
}
EXPORT_SYMBOL_GPL(iovmm_set_fault_handler);
MODULE_LICENSE("GPL");
