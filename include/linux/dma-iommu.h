/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2014-2015 ARM Ltd.
 */
#ifndef __DMA_IOMMU_H
#define __DMA_IOMMU_H

/* Exynos MFCMSCL: reserved low IOVA window aliasing the MFC firmware. */
#define EXYNOS_MFC_FW_IOVA		0x10000000UL
#define EXYNOS_MFC_FW_IOVA_SIZE		0x00400000UL

#include <linux/errno.h>
#include <linux/types.h>

#ifdef CONFIG_IOMMU_DMA
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/msi.h>

/* Domain management interface for IOMMU drivers */
int iommu_get_dma_cookie(struct iommu_domain *domain);
/* Enable before DMA-domain initialization; mappings retain an unmapped tail. */
int iommu_dma_enable_guard_page(struct iommu_domain *domain);
int iommu_get_msi_cookie(struct iommu_domain *domain, dma_addr_t base);
void iommu_put_dma_cookie(struct iommu_domain *domain);

/* Setup call for arch DMA mapping code */
void iommu_setup_dma_ops(struct device *dev, u64 dma_base, u64 size);

/* The DMA API isn't _quite_ the whole story, though... */
/*
 * iommu_dma_prepare_msi() - Map the MSI page in the IOMMU device
 *
 * The MSI page will be stored in @desc.
 *
 * Return: 0 on success otherwise an error describing the failure.
 */
int iommu_dma_prepare_msi(struct msi_desc *desc, phys_addr_t msi_addr);

/* Update the MSI message if required. */
void iommu_dma_compose_msi_msg(struct msi_desc *desc,
			       struct msi_msg *msg);

void iommu_dma_get_resv_regions(struct device *dev, struct list_head *list);

int iommu_dma_reserve_iova(struct device *dev, dma_addr_t base,
			   u64 size);

int iommu_dma_enable_best_fit_algo(struct device *dev);

#else /* CONFIG_IOMMU_DMA */

struct iommu_domain;
struct msi_desc;
struct msi_msg;
struct device;

static inline void iommu_setup_dma_ops(struct device *dev, u64 dma_base,
		u64 size)
{
}

static inline int iommu_get_dma_cookie(struct iommu_domain *domain)
{
	return -ENODEV;
}

static inline int iommu_dma_enable_guard_page(struct iommu_domain *domain)
{
	return -ENODEV;
}

static inline int iommu_get_msi_cookie(struct iommu_domain *domain, dma_addr_t base)
{
	return -ENODEV;
}

static inline void iommu_put_dma_cookie(struct iommu_domain *domain)
{
}

static inline int iommu_dma_prepare_msi(struct msi_desc *desc,
					phys_addr_t msi_addr)
{
	return 0;
}

static inline void iommu_dma_compose_msi_msg(struct msi_desc *desc,
					     struct msi_msg *msg)
{
}

static inline void iommu_dma_get_resv_regions(struct device *dev, struct list_head *list)
{
}

static inline int iommu_dma_reserve_iova(struct device *dev, dma_addr_t base,
					 u64 size)
{
	return -ENODEV;
}

static inline int iommu_dma_enable_best_fit_algo(struct device *dev)
{
	return -ENODEV;
}

#endif	/* CONFIG_IOMMU_DMA */
#endif	/* __DMA_IOMMU_H */
