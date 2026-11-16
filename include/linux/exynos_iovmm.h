/* SPDX-License-Identifier: GPL-2.0-only */
/* Samsung 4.4 display IOVMM interface, adapted for the Exynos7885 v7 driver. */
#ifndef _LINUX_EXYNOS_IOVMM_H
#define _LINUX_EXYNOS_IOVMM_H

#include <linux/dma-direction.h>
#include <linux/iommu.h>
#include <linux/types.h>

struct scatterlist;
struct device;

/* Preserve the Samsung fault encoding consumed by the DPP callback. */
#define SYSMMU_FAULT_BITS	4
#define SYSMMU_FAULT_SHIFT	16
#define SYSMMU_FAULT_MASK	((1 << SYSMMU_FAULT_BITS) - 1)
#define SYSMMU_FAULT_FLAG(id) (((id) & SYSMMU_FAULT_MASK) << SYSMMU_FAULT_SHIFT)
#define SYSMMU_FAULT_ID(fg) (((fg) >> SYSMMU_FAULT_SHIFT) & SYSMMU_FAULT_MASK)
#define SYSMMU_FAULT_PTW_ACCESS	0
#define SYSMMU_FAULT_PAGE_FAULT	1
#define SYSMMU_FAULT_TLB_MULTIHIT	2
#define SYSMMU_FAULT_ACCESS	3
#define SYSMMU_FAULT_SECURITY	4
#define SYSMMU_FAULT_UNKNOWN	5
#define IOMMU_FAULT_EXYNOS_PTW_ACCESS SYSMMU_FAULT_FLAG(SYSMMU_FAULT_PTW_ACCESS)
#define IOMMU_FAULT_EXYNOS_PAGE_FAULT SYSMMU_FAULT_FLAG(SYSMMU_FAULT_PAGE_FAULT)
#define IOMMU_FAULT_EXYNOS_TLB_MULTIHIT SYSMMU_FAULT_FLAG(SYSMMU_FAULT_TLB_MULTIHIT)
#define IOMMU_FAULT_EXYNOS_ACCESS SYSMMU_FAULT_FLAG(SYSMMU_FAULT_ACCESS)
#define IOMMU_FAULT_EXYNOS_SECURITY SYSMMU_FAULT_FLAG(SYSMMU_FAULT_SECURITY)
#define IOMMU_FAULT_EXYNOS_UNKNOWN SYSMMU_FAULT_FLAG(SYSMMU_FAULT_UNKNOWN)

int iovmm_activate(struct device *dev);
void iovmm_deactivate(struct device *dev);
struct iommu_domain *get_domain_from_dev(struct device *dev);
dma_addr_t iovmm_map(struct device *dev, struct scatterlist *sg, off_t offset,
		     size_t size, enum dma_data_direction direction, int prot);
void iovmm_unmap(struct device *dev, dma_addr_t iova);
int iovmm_map_oto(struct device *dev, phys_addr_t phys, size_t size);
void iovmm_unmap_oto(struct device *dev, phys_addr_t phys);
void iovmm_set_fault_handler(struct device *dev,
			     iommu_fault_handler_t handler, void *token);

/* Deliberately no disabled-config success stubs or unused userptr/PPC API. */
#endif
