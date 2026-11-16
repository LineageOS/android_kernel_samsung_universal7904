/* linux/drivers/iommu/exynos_iommu.c
 *
 * Copyright (c) 2011 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
/* Exynos7885/7904 display SYSMMU v7; intentionally separate from v1-v5. */
#include <linux/arm-smccc.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-iommu.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_iommu.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <asm/cacheflush.h>
#include <dt-bindings/sysmmu/sysmmu.h>
#include "exynos-sysmmu-7885.h"

static const struct iommu_ops exynos_iommu_ops;
static struct kmem_cache *lv2table_kmem_cache;

static struct exynos_iommu_domain *to_exynos_domain(struct iommu_domain *dom)
{
	return container_of(dom, struct exynos_iommu_domain, domain);
}
static void pgtable_flush(void *start, void *end)
{
	__dma_flush_area(start, (char *)end - (char *)start);
}

static int sysmmu_parse_tlb_way_dt(struct device *sysmmu,
				struct sysmmu_drvdata *drvdata)
{
	const char *props_name = "sysmmu,tlb_property";
	struct tlb_props *tlb_props = &drvdata->tlb_props;
	struct tlb_priv_id *priv_id_cfg = NULL;
	struct tlb_priv_addr *priv_addr_cfg = NULL;
	int i, cnt, priv_id_cnt = 0, priv_addr_cnt = 0;
	unsigned int priv_id_idx = 0, priv_addr_idx = 0;
	unsigned int prop;
	int ret;

	/* Parsing TLB way properties */
	cnt = of_property_count_u32_elems(sysmmu->of_node, props_name);
	if (cnt > 0 && (cnt & 1))
		return -EINVAL;
	for (i = 0; i < cnt; i+=2) {
		ret = of_property_read_u32_index(sysmmu->of_node,
			props_name, i, &prop);
		if (ret) {
			dev_err(sysmmu, "failed to get property."
				       "cnt = %d, ret = %d\n", i, ret);
			return -EINVAL;
		}

		switch (prop & WAY_TYPE_MASK) {
		case _PRIVATE_WAY_ID:
			priv_id_cnt++;
			tlb_props->flags |= TLB_WAY_PRIVATE_ID;
			break;
		case _PRIVATE_WAY_ADDR:
			priv_addr_cnt++;
			tlb_props->flags |= TLB_WAY_PRIVATE_ADDR;
			break;
		case _PUBLIC_WAY:
			tlb_props->flags |= TLB_WAY_PUBLIC;
			tlb_props->way_props.public_cfg = (prop & ~WAY_TYPE_MASK);
			break;
		default:
			dev_err(sysmmu, "Undefined properties!: %#x\n", prop);
			break;
		}
	}

	if (priv_id_cnt) {
		priv_id_cfg = devm_kzalloc(sysmmu, sizeof(*priv_id_cfg) * priv_id_cnt,
								GFP_KERNEL);
		if (!priv_id_cfg)
			return -ENOMEM;
	}

	if (priv_addr_cnt) {
		priv_addr_cfg = devm_kzalloc(sysmmu, sizeof(*priv_addr_cfg) * priv_addr_cnt,
								GFP_KERNEL);
		if (!priv_addr_cfg) {
			ret = -ENOMEM;
			goto err_priv_id;
		}
	}

	for (i = 0; i < cnt; i+=2) {
		ret = of_property_read_u32_index(sysmmu->of_node,
			props_name, i, &prop);
		if (ret) {
			dev_err(sysmmu, "failed to get property again? "
				       "cnt = %d, ret = %d\n", i, ret);
			ret = -EINVAL;
			goto err_priv_addr;
		}

		switch (prop & WAY_TYPE_MASK) {
		case _PRIVATE_WAY_ID:
			BUG_ON(!priv_id_cfg || priv_id_idx >= priv_id_cnt);

			priv_id_cfg[priv_id_idx].cfg = prop & ~WAY_TYPE_MASK;
			ret = of_property_read_u32_index(sysmmu->of_node,
				props_name, i+1, &priv_id_cfg[priv_id_idx].id);
			if (ret) {
				dev_err(sysmmu, "failed to get id property"
						"cnt = %d, ret = %d\n", i, ret);
				goto err_priv_addr;
			}
			priv_id_idx++;
			break;
		case _PRIVATE_WAY_ADDR:
			BUG_ON(!priv_addr_cfg || priv_addr_idx >= priv_addr_cnt);

			priv_addr_cfg[priv_addr_idx].cfg = prop & ~WAY_TYPE_MASK;
			priv_addr_idx++;
			break;
		case _PUBLIC_WAY:
			break;
		}
	}

	tlb_props->way_props.priv_id_cfg = priv_id_cfg;
	tlb_props->way_props.priv_id_cnt = priv_id_cnt;

	tlb_props->way_props.priv_addr_cfg = priv_addr_cfg;
	tlb_props->way_props.priv_addr_cnt = priv_addr_cnt;

	return 0;

err_priv_addr:
	if (priv_addr_cfg)
		devm_kfree(sysmmu, priv_addr_cfg);
err_priv_id:
	if (priv_id_cfg)
		devm_kfree(sysmmu, priv_id_cfg);

	return ret;
}

static int sysmmu_parse_tlb_port_dt(struct device *sysmmu,
				struct sysmmu_drvdata *drvdata)
{
	const char *props_name = "sysmmu,tlb_property";
	const char *slot_props_name = "sysmmu,slot_property";
	struct tlb_props *tlb_props = &drvdata->tlb_props;
	struct tlb_port_cfg *port_cfg = NULL;
	unsigned int *slot_cfg = NULL;
	int i, cnt, ret;
	int port_id_cnt = 0;

	cnt = of_property_count_u32_elems(sysmmu->of_node, slot_props_name);
	if (cnt > 0) {
		slot_cfg = devm_kzalloc(sysmmu, sizeof(*slot_cfg) * cnt, GFP_KERNEL);
		if (!slot_cfg)
			return -ENOMEM;

		for (i = 0; i < cnt; i++) {
			ret = of_property_read_u32_index(sysmmu->of_node,
					slot_props_name, i, &slot_cfg[i]);
			if (ret) {
				dev_err(sysmmu, "failed to get slot property."
						"cnt = %d, ret = %d\n", i, ret);
				ret = -EINVAL;
				goto err_slot_prop;
			}
		}

		tlb_props->port_props.slot_cnt = cnt;
		tlb_props->port_props.slot_cfg = slot_cfg;
	}

	cnt = of_property_count_u32_elems(sysmmu->of_node, props_name);
	if (cnt > 0 && (cnt & 1))
		return -EINVAL;
	if (!cnt || cnt < 0) {
		dev_info(sysmmu, "No TLB port propeties found.\n");
		return 0;
	}

	port_cfg = devm_kzalloc(sysmmu, sizeof(*port_cfg) * (cnt/2), GFP_KERNEL);
	if (!port_cfg) {
		ret = -ENOMEM;
		goto err_slot_prop;
	}

	for (i = 0; i < cnt; i+=2) {
		ret = of_property_read_u32_index(sysmmu->of_node,
			props_name, i, &port_cfg[port_id_cnt].cfg);
		if (ret) {
			dev_err(sysmmu, "failed to get cfg property."
				       "cnt = %d, ret = %d\n", i, ret);
			ret = -EINVAL;
			goto err_port_prop;
		}

		ret = of_property_read_u32_index(sysmmu->of_node,
			props_name, i+1, &port_cfg[port_id_cnt].id);
		if (ret) {
			dev_err(sysmmu, "failed to get id property."
				       "cnt = %d, ret = %d\n", i, ret);
			ret = -EINVAL;
			goto err_port_prop;
		}
		port_id_cnt++;
	}

	tlb_props->port_props.port_id_cnt = port_id_cnt;
	tlb_props->port_props.port_cfg = port_cfg;

	return 0;

err_port_prop:
	devm_kfree(sysmmu, port_cfg);

err_slot_prop:
	devm_kfree(sysmmu, slot_cfg);

	return ret;
}
static void __sysmmu_set_public_way(struct sysmmu_drvdata *drvdata,
						unsigned int public_cfg)
{
	u32 cfg = __raw_readl(drvdata->sfrbase + REG_PUBLIC_WAY_CFG);
	cfg &= ~MMU_PUBLIC_WAY_MASK;
	cfg |= public_cfg;

	writel_relaxed(cfg, drvdata->sfrbase + REG_PUBLIC_WAY_CFG);

	dev_dbg(drvdata->sysmmu, "public_cfg : %#x\n", cfg);
}

static void __sysmmu_set_private_way_id(struct sysmmu_drvdata *drvdata,
						unsigned int way_idx)
{
	struct tlb_priv_id *priv_cfg = drvdata->tlb_props.way_props.priv_id_cfg;
	u32 cfg = __raw_readl(drvdata->sfrbase + REG_PRIVATE_WAY_CFG(way_idx));

	cfg &= ~MMU_PRIVATE_WAY_MASK;
	cfg |= MMU_WAY_CFG_ID_MATCHING | MMU_WAY_CFG_PRIVATE_ENABLE |
						priv_cfg[way_idx].cfg;

	writel_relaxed(cfg, drvdata->sfrbase + REG_PRIVATE_WAY_CFG(way_idx));
	writel_relaxed(priv_cfg[way_idx].id,
			drvdata->sfrbase + REG_PRIVATE_ID(way_idx));

	dev_dbg(drvdata->sysmmu, "priv ID way[%d] cfg : %#x, id : %#x\n",
					way_idx, cfg, priv_cfg[way_idx].id);
}

static void __sysmmu_set_private_way_addr(struct sysmmu_drvdata *drvdata,
						unsigned int priv_addr_idx)
{
	struct tlb_priv_addr *priv_cfg = drvdata->tlb_props.way_props.priv_addr_cfg;
	unsigned int way_idx = drvdata->tlb_props.way_props.priv_id_cnt + priv_addr_idx;
	u32 cfg = __raw_readl(drvdata->sfrbase + REG_PRIVATE_WAY_CFG(way_idx));

	cfg &= ~MMU_PRIVATE_WAY_MASK;
	cfg |= MMU_WAY_CFG_ADDR_MATCHING | MMU_WAY_CFG_PRIVATE_ENABLE |
						priv_cfg[priv_addr_idx].cfg;

	writel_relaxed(cfg, drvdata->sfrbase + REG_PRIVATE_WAY_CFG(way_idx));

	dev_dbg(drvdata->sysmmu, "priv ADDR way[%d] cfg : %#x\n", way_idx, cfg);
}

static void __sysmmu_set_tlb_way_dedication(struct sysmmu_drvdata *drvdata)
{
	u32 cfg = __raw_readl(drvdata->sfrbase + REG_MMU_CAPA0_V7);
	u32 tlb_way_num = MMU_CAPA_NUM_TLB_WAY(cfg);
	u32 set_cnt = 0;
	struct tlb_props *tlb_props = &drvdata->tlb_props;
	unsigned int i;
	int priv_id_cnt = tlb_props->way_props.priv_id_cnt;
	int priv_addr_cnt = tlb_props->way_props.priv_addr_cnt;

	if (tlb_props->flags & TLB_WAY_PUBLIC)
		__sysmmu_set_public_way(drvdata,
				tlb_props->way_props.public_cfg);

	if (tlb_props->flags & TLB_WAY_PRIVATE_ID) {
		for (i = 0; i < priv_id_cnt &&
				set_cnt < tlb_way_num; i++, set_cnt++)
			__sysmmu_set_private_way_id(drvdata, i);
	}

	if (tlb_props->flags & TLB_WAY_PRIVATE_ADDR) {
		for (i = 0; i < priv_addr_cnt &&
				set_cnt < tlb_way_num; i++, set_cnt++)
			__sysmmu_set_private_way_addr(drvdata, i);
	}

	if (priv_id_cnt + priv_addr_cnt > tlb_way_num) {
		dev_warn(drvdata->sysmmu,
				"Too many values than TLB way count %d,"
				" so ignored!\n", tlb_way_num);
		dev_warn(drvdata->sysmmu,
				"Number of private way id/addr = %d/%d\n",
				priv_id_cnt, priv_addr_cnt);
	}
}

static void __sysmmu_set_tlb_port(struct sysmmu_drvdata *drvdata,
						unsigned int port_idx)
{
	struct tlb_port_cfg *port_cfg = drvdata->tlb_props.port_props.port_cfg;

	writel_relaxed(MMU_TLB_CFG_MASK(port_cfg[port_idx].cfg),
			drvdata->sfrbase + REG_MMU_TLB_CFG(port_idx));

	/* port_idx 0 is default port. */
	if (port_idx == 0) {
		dev_dbg(drvdata->sysmmu, "port[%d] cfg : %#x for common\n",
				port_idx,
				MMU_TLB_CFG_MASK(port_cfg[port_idx].cfg));
		return;
	}

	writel_relaxed(MMU_TLB_MATCH_CFG_MASK(port_cfg[port_idx].cfg),
			drvdata->sfrbase + REG_MMU_TLB_MATCH_CFG(port_idx));
	writel_relaxed(port_cfg[port_idx].id,
			drvdata->sfrbase + REG_MMU_TLB_MATCH_ID(port_idx));

	dev_dbg(drvdata->sysmmu, "port[%d] cfg : %#x, match : %#x, id : %#x\n",
				port_idx,
				MMU_TLB_CFG_MASK(port_cfg[port_idx].cfg),
				MMU_TLB_MATCH_CFG_MASK(port_cfg[port_idx].cfg),
				port_cfg[port_idx].id);
}

static void __sysmmu_set_tlb_port_dedication(struct sysmmu_drvdata *drvdata)
{
	u32 cfg = __raw_readl(drvdata->sfrbase + REG_MMU_CAPA1_V7);
	u32 tlb_num = MMU_CAPA1_NUM_TLB(cfg);
	struct tlb_props *tlb_props = &drvdata->tlb_props;
	unsigned int i;
	int port_id_cnt = tlb_props->port_props.port_id_cnt;
	int slot_cnt = tlb_props->port_props.slot_cnt;

	if (port_id_cnt > tlb_num) {
		dev_warn(drvdata->sysmmu,
				"Too many values %d than TLB count %d,"
				" so ignored!\n", port_id_cnt, tlb_num);
		port_id_cnt = tlb_num;
	}

	for (i = 0; i < port_id_cnt; i++)
		__sysmmu_set_tlb_port(drvdata, i);

	for (i = 0; i < slot_cnt; i++)
		writel_relaxed(tlb_props->port_props.slot_cfg[i],
				drvdata->sfrbase + REG_SLOT_RSV(i));
}

static void __sysmmu_init_config(struct sysmmu_drvdata *drvdata)
{
	u32 cfg = 0;

	if (IS_TLB_WAY_TYPE(drvdata))
		__sysmmu_set_tlb_way_dedication(drvdata);
	else
		__sysmmu_set_tlb_port_dedication(drvdata);
	if (drvdata->qos != DEFAULT_QOS_VALUE)
		cfg |= CFG_QOS_OVRRIDE | CFG_QOS(drvdata->qos);
	cfg |= readl_relaxed(drvdata->sfrbase + REG_MMU_CFG) & ~CFG_MASK;
	writel_relaxed(cfg, drvdata->sfrbase + REG_MMU_CFG);
}

static int sysmmu_control(struct sysmmu_drvdata *data, bool enable)
{
	u32 status;
	int ret;

	writel(CTRL_BLOCK, data->sfrbase + REG_MMU_CTRL);
	ret = readl_poll_timeout_atomic(data->sfrbase + REG_MMU_STATUS,
				       status, status & 1, 1, 4096);
	if (ret) {
		dev_err(data->sysmmu, "SYSMMU block timed out\n");
		return ret;
	}
	writel(enable ? CTRL_ENABLE : CTRL_DISABLE,
	       data->sfrbase + REG_MMU_CTRL);
	return 0;
}

void exynos_sysmmu_tlb_invalidate(struct iommu_domain *dom,
				dma_addr_t start, size_t size)
{
	struct exynos_iommu_domain *domain = to_exynos_domain(dom);
	struct exynos_iommu_owner *owner;
	unsigned long flags;

	if (!size)
		return;
	spin_lock_irqsave(&domain->lock, flags);
	list_for_each_entry(owner, &domain->clients_list, client) {
		struct sysmmu_drvdata *data = owner->data;

		spin_lock(&data->lock);
		if (data->active && !data->suspended) {
			writel_relaxed(start, data->sfrbase + REG_FLUSH_RANGE_START);
			writel_relaxed(start + size - 1,
				       data->sfrbase + REG_FLUSH_RANGE_END);
			writel(1, data->sfrbase + REG_MMU_FLUSH_RANGE);
			if (data->is_abox) {
				writel_relaxed(start, data->sfrbase + REG_FLUSH_RANGE_START);
				writel_relaxed(start + size - 1,
					       data->sfrbase + REG_FLUSH_RANGE_END);
				writel(ABOX_PT1_FLUSH, data->sfrbase + REG_MMU_FLUSH_RANGE);
			}
		}
		spin_unlock(&data->lock);
	}
	spin_unlock_irqrestore(&domain->lock, flags);
}

static struct iommu_domain *exynos_iommu_domain_alloc(unsigned int type)
{
	struct exynos_iommu_domain *domain;

	if (type != IOMMU_DOMAIN_UNMANAGED && type != IOMMU_DOMAIN_DMA)
		return NULL;
	domain = kzalloc(sizeof(*domain), GFP_KERNEL);
	if (!domain)
		return NULL;
	domain->pgtable = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, 2);
	if (!domain->pgtable) {
		kfree(domain);
		return NULL;
	}
	pgtable_flush(domain->pgtable, domain->pgtable + NUM_LV1ENTRIES);
	spin_lock_init(&domain->lock);
	spin_lock_init(&domain->pgtablelock);
	INIT_LIST_HEAD(&domain->clients_list);
	domain->domain.geometry.aperture_end = U32_MAX;
	domain->domain.geometry.force_aperture = true;
	if (type == IOMMU_DOMAIN_DMA && iommu_get_dma_cookie(&domain->domain)) {
		free_pages((unsigned long)domain->pgtable, 2);
		kfree(domain);
		return NULL;
	}
	return &domain->domain;
}

static void exynos_iommu_domain_free(struct iommu_domain *dom)
{
	struct exynos_iommu_domain *domain = to_exynos_domain(dom);
	int i;

	if (WARN_ON(!list_empty(&domain->clients_list)))
		return; /* Never free tables still reachable by hardware. */
	iommu_put_dma_cookie(dom);
	for (i = 0; i < NUM_LV1ENTRIES; i++)
		if (domain->lv2table[i])
			kmem_cache_free(lv2table_kmem_cache, domain->lv2table[i]);
	free_pages((unsigned long)domain->pgtable, 2);
	kfree(domain);
}

/* Caller holds data->lock and has enabled the supplier clock/power domain. */
static int sysmmu_enable_locked(struct sysmmu_drvdata *data)
{
	struct exynos_iommu_domain *domain = to_exynos_domain(data->domain);
	int ret;

	/* Splash scan-out is not identity-mapped yet: stay in bypass. */
	if (data->translation_deferred)
		return 0;

	__sysmmu_init_config(data);
	writel_relaxed(virt_to_phys(domain->pgtable) >> SPAGE_ORDER,
		       data->sfrbase + REG_PT_BASE_PPN);
	if (data->is_abox)
		writel_relaxed(virt_to_phys(domain->pgtable) >> SPAGE_ORDER,
			       data->sfrbase + REG_ABOX_PT1_BASE_PPN);
	writel(1, data->sfrbase + REG_MMU_FLUSH);
	if (data->is_abox)
		writel(ABOX_PT1_FLUSH, data->sfrbase + REG_MMU_FLUSH);
	ret = sysmmu_control(data, true);
	if (!ret)
		data->active = true;
	if (data->is_abox)
		dev_warn(data->sysmmu, "ABOX enable: ret %d CTRL %#x CFG %#x VER %#x PT0 %#x PT1 %#x\n",
			 ret, readl(data->sfrbase + REG_MMU_CTRL),
			 readl(data->sfrbase + REG_MMU_CFG),
			 readl(data->sfrbase + REG_MMU_VERSION),
			 readl(data->sfrbase + REG_PT_BASE_PPN),
			 readl(data->sfrbase + REG_ABOX_PT1_BASE_PPN));
	return ret;
}

/* Called by the display IOVMM once the splash buffer is identity-mapped. */
int exynos_sysmmu_allow_translation(struct sysmmu_drvdata *data)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&data->lock, flags);
	if (data->translation_deferred) {
		data->translation_deferred = false;
		if (data->domain && !data->active)
			ret = sysmmu_enable_locked(data);
		if (ret)
			data->translation_deferred = true;
	}
	spin_unlock_irqrestore(&data->lock, flags);
	if (ret)
		dev_err(data->sysmmu, "enabling translation failed: %d\n", ret);
	else
		dev_warn(data->sysmmu, "translation enabled after splash identity map\n");
	return ret;
}

static int exynos_iommu_attach_owner(struct iommu_domain *dom,
		struct exynos_iommu_owner *owner)
{
	struct exynos_iommu_domain *domain = to_exynos_domain(dom);
	struct sysmmu_drvdata *data;
	unsigned long flags;
	int ret = 0;

	if (!owner)
		return -ENODEV;
	data = owner->data;
	mutex_lock(&data->mutex);
	if (owner->domain) {
		ret = owner->domain == dom ? 0 : -EBUSY;
		goto out;
	}
	if (data->domain && data->domain != dom) {
		ret = -EBUSY;
		goto out;
	}
	ret = pm_runtime_resume_and_get(data->sysmmu);
	if (ret < 0)
		goto out;
	spin_lock_irqsave(&domain->lock, flags);
	spin_lock(&data->lock);
	data->activations++;
	data->domain = dom;
	owner->domain = dom;
	list_add_tail(&owner->client, &domain->clients_list);
	if (!data->active)
		ret = sysmmu_enable_locked(data);
	if (ret) {
		list_del_init(&owner->client);
		owner->domain = NULL;
		if (!--data->activations)
			data->domain = NULL;
	}
	spin_unlock(&data->lock);
	spin_unlock_irqrestore(&domain->lock, flags);
	/* Client runtime device links, not domain attachment, keep power on. */
	pm_runtime_put_sync(data->sysmmu);
out:
	mutex_unlock(&data->mutex);
	return ret;
}

static void exynos_iommu_detach_owner(struct iommu_domain *dom,
		struct exynos_iommu_owner *owner)
{
	struct exynos_iommu_domain *domain = to_exynos_domain(dom);
	struct sysmmu_drvdata *data;
	unsigned long flags;

	if (!owner || owner->domain != dom)
		return;
	data = owner->data;
	mutex_lock(&data->mutex);
	spin_lock_irqsave(&domain->lock, flags);
	spin_lock(&data->lock);
	if (data->activations == 1) {
		if (data->active) {
			/* Never free tables while DMA might still reach them. */
			if (sysmmu_control(data, false))
				panic("Unable to disable display SYSMMU");
			writel(0, data->sfrbase + REG_MMU_CFG);
			data->active = false;
		}
		data->domain = NULL;
	}
	data->activations--;
	owner->domain = NULL;
	list_del_init(&owner->client);
	spin_unlock(&data->lock);
	spin_unlock_irqrestore(&domain->lock, flags);
	mutex_unlock(&data->mutex);
}

/* All SYSMMUs on a master must walk the same DMA page tables. */
static int exynos_iommu_attach_device(struct iommu_domain *dom, struct device *dev)
{
	struct exynos_iommu_owner *owner = dev_iommu_priv_get(dev), *it;
	int ret;

	if (!owner)
		return -ENODEV;
	/* MFCMSCL's JPEG encoder lacks a programmed output-length limit.
	 * Reserve, but never map, one tail page for every native DMA mapping.
	 * Other domains (display/audio/camera) retain their existing allocator.
	 */
	if (dom->type == IOMMU_DOMAIN_DMA && dev->of_node &&
	    (of_device_is_compatible(dev->of_node, "samsung,exynos7904-jpeg") ||
	     of_device_is_compatible(dev->of_node, "samsung,exynos5-scaler"))) {
		ret = iommu_dma_enable_guard_page(dom);
		if (ret)
			return ret;
	}
	/* Reject partial attachment; it cannot safely be rolled back as a unit. */
	if (owner->domain) {
		if (owner->domain != dom)
			return -EBUSY;
		list_for_each_entry(it, &owner->siblings, sibling)
			if (it->domain != dom)
				return -EBUSY;
		return 0;
	}
	list_for_each_entry(it, &owner->siblings, sibling)
		if (it->domain)
			return -EBUSY;

	ret = exynos_iommu_attach_owner(dom, owner);
	if (ret)
		return ret;
	list_for_each_entry(it, &owner->siblings, sibling) {
		ret = exynos_iommu_attach_owner(dom, it);
		if (ret)
			goto rollback;
	}
	if (dev->of_node && of_device_is_compatible(dev->of_node,
					"samsung,exynos5-fimc-is")) {
		unsigned int controllers = 1;

		list_for_each_entry(it, &owner->siblings, sibling)
			controllers++;
		dev_warn(dev, "camera shared DMA domain attached to %u SYSMMUs\n",
			 controllers);
	}
	return 0;
rollback:
	list_for_each_entry(it, &owner->siblings, sibling)
		if (it->domain == dom)
			exynos_iommu_detach_owner(dom, it);
	exynos_iommu_detach_owner(dom, owner);
	return ret;
}

static void exynos_iommu_detach_device(struct iommu_domain *dom, struct device *dev)
{
	struct exynos_iommu_owner *owner = dev_iommu_priv_get(dev), *it;

	if (!owner)
		return;
	list_for_each_entry(it, &owner->siblings, sibling)
		exynos_iommu_detach_owner(dom, it);
	exynos_iommu_detach_owner(dom, owner);
}

static int exynos_iommu_map(struct iommu_domain *dom, unsigned long iova,
		phys_addr_t paddr, size_t size, int prot, gfp_t gfp)
{
	struct exynos_iommu_domain *domain = to_exynos_domain(dom);
	sysmmu_pte_t *sent, *pent, *allocated = NULL;
	unsigned long flags;
	unsigned int section = iova >> SECT_ORDER;
	int i, count, ret = 0;

	if (iova > U32_MAX || size > (1ULL << 32) - iova ||
	    paddr >= (1ULL << 36) || size > (1ULL << 36) - paddr ||
	    (size != SECT_SIZE && size != LPAGE_SIZE && size != SPAGE_SIZE) ||
	    !IS_ALIGNED(iova | paddr, size))
		return -EINVAL;
	/* Allocate outside the lock, honouring the IOMMU caller's GFP mask. */
	if (size != SECT_SIZE && !READ_ONCE(domain->lv2table[section])) {
		allocated = kmem_cache_zalloc(lv2table_kmem_cache, gfp);
		if (!allocated)
			return -ENOMEM;
	}
	spin_lock_irqsave(&domain->pgtablelock, flags);
	sent = &domain->pgtable[section];
	if (size == SECT_SIZE) {
		if (*sent) {
			ret = -EADDRINUSE;
			goto out;
		}
		*sent = mk_lv1ent_sect(paddr);
		if (prot & IOMMU_CACHE)
			set_lv1ent_shareable(sent);
		pgtable_flush(sent, sent + 1);
		goto out;
	}
	if (lv1ent_section(sent)) {
		ret = -EADDRINUSE;
		goto out;
	}
	if (!domain->lv2table[section]) {
		domain->lv2table[section] = allocated;
		allocated = NULL;
		pgtable_flush(domain->lv2table[section],
			      domain->lv2table[section] + NUM_LV2ENTRIES);
	}
	pent = domain->lv2table[section] + lv2ent_offset(iova);
	count = size >> SPAGE_ORDER;
	for (i = 0; i < count; i++) {
		if (!lv2ent_fault(pent + i)) {
			ret = -EADDRINUSE;
			goto out;
		}
	}
	for (i = 0; i < count; i++) {
		pent[i] = size == SPAGE_SIZE ? mk_lv2ent_spage(paddr) :
					      mk_lv2ent_lpage(paddr);
		if (prot & IOMMU_CACHE)
			set_lv2ent_shareable(&pent[i]);
	}
	pgtable_flush(pent, pent + count);
	/* Publish L2 contents before its L1 descriptor. */
	if (lv1ent_fault(sent)) {
		*sent = mk_lv1ent_page(virt_to_phys(domain->lv2table[section]));
		pgtable_flush(sent, sent + 1);
	}
out:
	spin_unlock_irqrestore(&domain->pgtablelock, flags);
	if (allocated)
		kmem_cache_free(lv2table_kmem_cache, allocated);
	return ret;
}

static size_t exynos_iommu_unmap(struct iommu_domain *dom, unsigned long iova,
			 size_t size, struct iommu_iotlb_gather *gather)
{
	struct exynos_iommu_domain *domain = to_exynos_domain(dom);
	sysmmu_pte_t *sent, *pent;
	unsigned long flags;
	size_t unmapped = 0;

	if (iova > U32_MAX)
		return 0;
	spin_lock_irqsave(&domain->pgtablelock, flags);
	sent = domain->pgtable + lv1ent_offset(iova);
	if (lv1ent_section(sent)) {
		if (size < SECT_SIZE || !IS_ALIGNED(iova, SECT_SIZE))
			goto out;
		*sent = 0;
		pgtable_flush(sent, sent + 1);
		unmapped = SECT_SIZE;
	} else if (lv1ent_page(sent)) {
		pent = domain->lv2table[lv1ent_offset(iova)] + lv2ent_offset(iova);
		if (lv2ent_fault(pent))
			goto out;
		unmapped = lv2ent_small(pent) ? SPAGE_SIZE : LPAGE_SIZE;
		if (size < unmapped || !IS_ALIGNED(iova, unmapped)) {
			unmapped = 0;
			goto out;
		}
		memset(pent, 0, sizeof(*pent) * (unmapped >> SPAGE_ORDER));
		pgtable_flush(pent, pent + (unmapped >> SPAGE_ORDER));
		/* Retain storage, but allow later remapping as a 1 MiB section. */
		if (!memchr_inv(domain->lv2table[lv1ent_offset(iova)], 0,
				LV2TABLE_SIZE)) {
			*sent = 0;
			pgtable_flush(sent, sent + 1);
		}
	}
out:
	spin_unlock_irqrestore(&domain->pgtablelock, flags);
	if (unmapped)
		iommu_iotlb_gather_add_page(dom, gather, iova, unmapped);
	return unmapped;
}

static phys_addr_t exynos_iommu_iova_to_phys(struct iommu_domain *dom, dma_addr_t iova)
{
	struct exynos_iommu_domain *domain = to_exynos_domain(dom);
	sysmmu_pte_t *ent;
	phys_addr_t phys = 0;
	unsigned long flags;

	if (iova > U32_MAX)
		return 0;
	spin_lock_irqsave(&domain->pgtablelock, flags);
	ent = domain->pgtable + lv1ent_offset(iova);
	if (lv1ent_section(ent)) {
		phys = section_phys(ent) + section_offs(iova);
	} else if (lv1ent_page(ent)) {
		ent = domain->lv2table[lv1ent_offset(iova)] + lv2ent_offset(iova);
		if (lv2ent_small(ent))
			phys = spage_phys(ent) + spage_offs(iova);
		else if (lv2ent_large(ent))
			phys = lpage_phys(ent) + lpage_offs(iova);
	}
	spin_unlock_irqrestore(&domain->pgtablelock, flags);
	return phys;
}

/* Vendor SMC ABI: include/linux/smc.h:65,143-147 (READ == 0). */
static u32 secure_read(struct sysmmu_drvdata *data, unsigned int offset)
{
	struct arm_smccc_res res;

	arm_smccc_smc(0x820020d0, data->securebase + offset, 0, 0,
		      0, 0, 0, 0, &res);
	if ((res.a0 >> 16 & 0xffff) == 0xdead)
		panic("Display SYSMMU secure register read failed");
	return res.a0;
}

static irqreturn_t sysmmu_fault(struct sysmmu_drvdata *data, bool secure)
{
	struct exynos_iommu_owner *owner;
	u32 status, addr, info;
	unsigned int type;
	int flags;

	status = secure ? secure_read(data, REG_INT_STATUS) :
			  readl(data->sfrbase + REG_INT_STATUS);
	if (!status)
		return IRQ_NONE;
	type = __ffs(status);
	addr = secure ? secure_read(data, REG_FAULT_ADDR) :
			readl(data->sfrbase + REG_FAULT_ADDR);
	info = secure ? secure_read(data, REG_FAULT_TRANS_INFO) :
			readl(data->sfrbase + REG_FAULT_TRANS_INFO);
	/* The vendor secure SFR interface retains separate write status bits. */
	if (secure) {
		flags = type >= 16 ? IOMMU_FAULT_WRITE : IOMMU_FAULT_READ;
		if (type >= 16)
			type -= 16;
	} else {
		flags = MMU_IS_READ_FAULT(info) ? IOMMU_FAULT_READ : IOMMU_FAULT_WRITE;
	}
	if (type >= SYSMMU_FAULT_UNKNOWN)
		type = SYSMMU_FAULT_UNKNOWN;
	flags |= SYSMMU_FAULT_FLAG(type);
	dev_emerg(data->sysmmu,
		  "%s %s fault: status %#x, IOVA %#x, info %#x, flags %#x\n",
		  data->port_name, secure ? "secure" : "nonsecure",
		  status, addr, info, flags);
	/* Retain the vendor's first three nonsecure PTW-access retries. */
	spin_lock(&data->lock);
	if (strncmp(data->port_name, "DPU", 3) && !secure) {
		struct exynos_iommu_domain *dom = data->domain ?
			to_exynos_domain(data->domain) : NULL;
		sysmmu_pte_t *ent = dom ? dom->pgtable + lv1ent_offset(addr) : NULL;

		dev_emerg(data->sysmmu,
			  "CTRL %#x CFG %#x STATUS %#x VER %#x PT0 %#x PT1 %#x pgtable %pa lv1 %#x lv2 %#x\n",
			  readl(data->sfrbase + REG_MMU_CTRL),
			  readl(data->sfrbase + REG_MMU_CFG),
			  readl(data->sfrbase + REG_MMU_STATUS),
			  readl(data->sfrbase + REG_MMU_VERSION),
			  readl(data->sfrbase + REG_PT_BASE_PPN),
			  data->is_abox ? readl(data->sfrbase + REG_ABOX_PT1_BASE_PPN) : 0,
			  dom ? &(phys_addr_t){ virt_to_phys(dom->pgtable) } : NULL,
			  ent ? *ent : 0,
			  ent && lv1ent_page(ent) ?
				*(dom->lv2table[lv1ent_offset(addr)] + lv2ent_offset(addr)) : 0);
		writel(~0U, data->sfrbase + REG_INT_CLEAR);
		if (++data->other_faults >= 8 && data->irq > 0) {
			dev_emerg(data->sysmmu, "too many faults, masking IRQ\n");
			disable_irq_nosync(data->irq);
		}
		spin_unlock(&data->lock);
		return IRQ_HANDLED;
	}
	if (!secure && type == SYSMMU_FAULT_PTW_ACCESS && ++data->ptw_faults <= 3) {
		writel(1, data->sfrbase + REG_INT_CLEAR);
		spin_unlock(&data->lock);
		return IRQ_HANDLED;
	}
	/* Owner entries and handlers are protected against concurrent release. */
	list_for_each_entry(owner, &data->owners, node)
		if (owner->fault_handler && owner->domain)
			owner->fault_handler(owner->domain, owner->master,
					     addr, flags, owner->token);
	/* Only a DPU fault can corrupt scan-out; other masters just stall. */
	if (strncmp(data->port_name, "DPU", 3)) {
		writel(1 << type, data->sfrbase + REG_INT_CLEAR);
		spin_unlock(&data->lock);
		return IRQ_HANDLED;
	}
	spin_unlock(&data->lock);
	panic("Unrecoverable display SYSMMU fault");
}

static irqreturn_t exynos_sysmmu_irq(int irq, void *arg)
{
	return sysmmu_fault(arg, false);
}
static irqreturn_t exynos_sysmmu_secure_irq(int irq, void *arg)
{
	return sysmmu_fault(arg, true);
}

static int exynos_iommu_of_xlate(struct device *dev, struct of_phandle_args *args)
{
	struct exynos_iommu_owner *owner = dev_iommu_priv_get(dev), *it, *tmp;
	struct of_phandle_args spec;
	struct platform_device *pdev;
	struct sysmmu_drvdata *data;
	unsigned long flags;
	int count, i, ret = 0;

	if (args->args_count) {
		if (!owner)
			iommu_fwspec_free(dev);
		return -EINVAL;
	}
	if (owner) {
		if (owner->data->sysmmu->of_node == args->np)
			return 0;
		list_for_each_entry(it, &owner->siblings, sibling)
			if (it->data->sysmmu->of_node == args->np)
				return 0;
		return -EINVAL;
	}
	count = of_count_phandle_with_args(dev->of_node, "iommus", "#iommu-cells");
	if (count < 1) {
		ret = count < 0 ? count : -EINVAL;
		goto fail_fwspec;
	}
	/* Only the camera uses multiple controllers; retain the display ABI. */
	if (count > 1 && !of_device_is_compatible(dev->of_node, "samsung,exynos5-fimc-is")) {
		ret = -EINVAL;
		goto fail_fwspec;
	}
	for (i = 0; i < count; i++) {
		ret = of_parse_phandle_with_args(dev->of_node, "iommus", "#iommu-cells", i, &spec);
		if (ret)
			goto fail_fwspec;
		if (spec.args_count || !of_device_is_compatible(spec.np,
					"samsung,exynos7885-sysmmu")) {
			of_node_put(spec.np);
			ret = -EINVAL;
			goto fail_fwspec;
		}
		pdev = of_find_device_by_node(spec.np);
		of_node_put(spec.np);
		if (!pdev) {
			ret = -EPROBE_DEFER;
			goto fail_fwspec;
		}
		data = platform_get_drvdata(pdev);
		put_device(&pdev->dev);
		if (!data) {
			ret = -EPROBE_DEFER;
			goto fail_fwspec;
		}
	}

	owner = NULL;
	for (i = 0; i < count; i++) {
		ret = of_parse_phandle_with_args(dev->of_node, "iommus", "#iommu-cells", i, &spec);
		if (ret)
			goto fail;
		pdev = of_find_device_by_node(spec.np);
		of_node_put(spec.np);
		if (!pdev) { ret = -EPROBE_DEFER; goto fail; }
		data = platform_get_drvdata(pdev);
		put_device(&pdev->dev);
		if (!data) { ret = -EPROBE_DEFER; goto fail; }
		if (owner) {
			if (owner->data == data) { ret = -EINVAL; goto fail; }
			list_for_each_entry(it, &owner->siblings, sibling)
				if (it->data == data) { ret = -EINVAL; goto fail; }
		}
		it = kzalloc(sizeof(*it), GFP_KERNEL);
		if (!it) { ret = -ENOMEM; goto fail; }
		it->master = dev;
		it->data = data;
		INIT_LIST_HEAD(&it->client);
		INIT_LIST_HEAD(&it->siblings);
		INIT_LIST_HEAD(&it->sibling);
		if (!owner)
			owner = it;
		else
			list_add_tail(&it->sibling, &owner->siblings);
		spin_lock_irqsave(&data->lock, flags);
		list_add_tail(&it->node, &data->owners);
		spin_unlock_irqrestore(&data->lock, flags);
	}
	dev_iommu_priv_set(dev, owner);
	return 0;
fail:
	if (!owner)
		goto fail_fwspec;
	list_for_each_entry_safe(it, tmp, &owner->siblings, sibling) {
		spin_lock_irqsave(&it->data->lock, flags);
		list_del(&it->node);
		spin_unlock_irqrestore(&it->data->lock, flags);
		list_del(&it->sibling);
		kfree(it);
	}
	spin_lock_irqsave(&owner->data->lock, flags);
	list_del(&owner->node);
	spin_unlock_irqrestore(&owner->data->lock, flags);
	kfree(owner);
fail_fwspec:
	/* An ops-bearing fwspec otherwise makes OF skip xlate on retry. */
	iommu_fwspec_free(dev);
	return ret;
}

static void exynos_iommu_release_device(struct device *dev);

static struct iommu_device *exynos_iommu_probe_device(struct device *dev)
{
	struct exynos_iommu_owner *owner = dev_iommu_priv_get(dev), *it;

	if (!owner)
		return ERR_PTR(-ENODEV);
	owner->link = device_link_add(dev, owner->data->sysmmu,
				      DL_FLAG_STATELESS | DL_FLAG_PM_RUNTIME);
	if (!owner->link) {
		exynos_iommu_release_device(dev);
		return ERR_PTR(-ENOMEM);
	}
	list_for_each_entry(it, &owner->siblings, sibling) {
		it->link = device_link_add(dev, it->data->sysmmu,
					 DL_FLAG_STATELESS | DL_FLAG_PM_RUNTIME);
		if (!it->link)
			goto fail;
	}
	return &owner->data->iommu;
fail:
	list_for_each_entry(it, &owner->siblings, sibling) {
		if (it->link) {
			device_link_del(it->link);
			it->link = NULL;
		}
	}
	device_link_del(owner->link);
	owner->link = NULL;
	exynos_iommu_release_device(dev);
	return ERR_PTR(-ENOMEM);
}
static struct iommu_group *exynos_iommu_device_group(struct device *dev)
{
	struct exynos_iommu_owner *owner = dev_iommu_priv_get(dev);
	struct sysmmu_drvdata *data = owner->data;
	struct iommu_group *group;

	mutex_lock(&data->mutex);
	if (!data->group) {
		group = iommu_group_alloc();
		if (IS_ERR(group)) {
			mutex_unlock(&data->mutex);
			return group;
		}
		data->group = group;
	}
	group = iommu_group_ref_get(data->group);
	mutex_unlock(&data->mutex);
	return group;
}
static void exynos_iommu_release_device(struct device *dev)
{
	struct exynos_iommu_owner *owner = dev_iommu_priv_get(dev), *it, *tmp;
	unsigned long flags;

	if (!owner)
		return;
	if (owner->data->release_iovmm)
		owner->data->release_iovmm(dev);
	if (owner->domain)
		exynos_iommu_detach_device(owner->domain, dev);
	list_for_each_entry_safe(it, tmp, &owner->siblings, sibling) {
		if (it->domain)
			exynos_iommu_detach_owner(it->domain, it);
		spin_lock_irqsave(&it->data->lock, flags);
		list_del(&it->node);
		spin_unlock_irqrestore(&it->data->lock, flags);
		if (it->link)
			device_link_del(it->link);
		list_del(&it->sibling);
		kfree(it);
	}
	spin_lock_irqsave(&owner->data->lock, flags);
	list_del(&owner->node);
	spin_unlock_irqrestore(&owner->data->lock, flags);
	if (owner->link)
		device_link_del(owner->link);
	dev_iommu_priv_set(dev, NULL);
	kfree(owner);
}
static void exynos_iommu_sync_map(struct iommu_domain *dom,
				 unsigned long iova, size_t size)
{
	exynos_sysmmu_tlb_invalidate(dom, iova, size);
}

static void exynos_iommu_iotlb_sync(struct iommu_domain *dom,
				  struct iommu_iotlb_gather *gather)
{
	if (!gather->pgsize)
		return;
	exynos_sysmmu_tlb_invalidate(dom, gather->start,
				    gather->end - gather->start + 1);
	/* PTW must retire before iommu_unmap returns backing memory to callers. */
	udelay(60); /* 4.4 exynos-iovmm.c:488. */
}

static void exynos_iommu_flush_all(struct iommu_domain *dom)
{
	exynos_sysmmu_tlb_invalidate(dom, 0, 1ULL << 32);
	udelay(60);
}

static int exynos_iommu_default_domain_type(struct device *dev)
{
	/* Display dma-buf importers must never fall back to identity DMA. */
	return IOMMU_DOMAIN_DMA;
}

static void exynos_iommu_get_resv_regions(struct device *dev, struct list_head *head)
{
	struct exynos_iommu_owner *owner = dev_iommu_priv_get(dev);
	struct iommu_resv_region *region;

	iommu_dma_get_resv_regions(dev, head);
	/* Native camera/media importers have no legacy IOVMM bitmap. They
	 * need the full DMA aperture for multiple full-resolution buffers.
	 */
	if (dev->of_node &&
	    (of_device_is_compatible(dev->of_node, "samsung,exynos7904-jpeg") ||
	     of_device_is_compatible(dev->of_node, "samsung,exynos5-scaler") ||
	     of_device_is_compatible(dev->of_node, "samsung,mfc-v6"))) {
		/*
		 * MFCMSCL domain: the MFC RISC rejects buffers below its F/W
		 * base, and dma-iommu allocates top-down. Keep a low window out
		 * of the allocator so MFC can alias its F/W there, below every
		 * other MFCMSCL IOVA. Any media device may initialise the domain.
		 */
		region = iommu_alloc_resv_region(EXYNOS_MFC_FW_IOVA,
						 EXYNOS_MFC_FW_IOVA_SIZE, 0,
						 IOMMU_RESV_RESERVED);
		if (region)
			list_add_tail(&region->list, head);
		return;
	}
	if (dev->of_node &&
	    of_device_is_compatible(dev->of_node, "samsung,exynos5-fimc-is"))
		return;
	if (!IS_ENABLED(CONFIG_EXYNOS_IOVMM))
		return;
	/*
	 * Keep DMA-API IOVAs below 256 MiB. The legacy bitmap and boot identity
	 * mappings own the high window; reserve its final 64 KiB guard as well.
	 */
	region = iommu_alloc_resv_region(0x10000000, 0xf0000000, 0,
					IOMMU_RESV_RESERVED);
	if (!region) {
		/* Without a reservation only generic DMA is safe, never legacy maps. */
		WRITE_ONCE(owner->data->legacy_reservation_failed, true);
		dev_err(dev, "Unable to reserve legacy display IOVAs\n");
		return;
	}
	list_add_tail(&region->list, head);
}

static const struct iommu_ops exynos_iommu_ops = {
	.domain_alloc = exynos_iommu_domain_alloc,
	.domain_free = exynos_iommu_domain_free,
	.attach_dev = exynos_iommu_attach_device,
	.detach_dev = exynos_iommu_detach_device,
	.map = exynos_iommu_map,
	.unmap = exynos_iommu_unmap,
	.iova_to_phys = exynos_iommu_iova_to_phys,
	.iotlb_sync_map = exynos_iommu_sync_map,
	.iotlb_sync = exynos_iommu_iotlb_sync,
	.flush_iotlb_all = exynos_iommu_flush_all,
	.def_domain_type = exynos_iommu_default_domain_type,
	.get_resv_regions = exynos_iommu_get_resv_regions,
	.put_resv_regions = generic_iommu_put_resv_regions,
	.probe_device = exynos_iommu_probe_device,
	.release_device = exynos_iommu_release_device,
	.device_group = exynos_iommu_device_group,
	.of_xlate = exynos_iommu_of_xlate,
	.pgsize_bitmap = SECT_SIZE | LPAGE_SIZE | SPAGE_SIZE,
};

static int sysmmu_runtime_resume(struct device *dev)
{
	struct sysmmu_drvdata *data = dev_get_drvdata(dev);
	unsigned long flags;
	int ret;

	if (data->rpm_held) {
		data->rpm_held = false;
		return 0;
	}
	ret = clk_prepare_enable(data->clk);
	if (ret)
		return ret;
	spin_lock_irqsave(&data->lock, flags);
	if (data->domain && !data->active)
		ret = sysmmu_enable_locked(data);
	spin_unlock_irqrestore(&data->lock, flags);
	if (ret)
		clk_disable_unprepare(data->clk);
	return ret;
}
static int sysmmu_runtime_suspend(struct device *dev)
{
	struct sysmmu_drvdata *data = dev_get_drvdata(dev);
	unsigned long flags;
	int ret = 0;

	/*
	 * The A-Box DSP keeps running while its driver is runtime suspended
	 * (4.4 only stops it when DISPAUD powers off, which is always-on
	 * here). Dropping translation under it makes Calliope fault on every
	 * access, so keep the SYSMMU and its clock on, as 4.4 did.
	 */
	if (data->is_abox && data->active) {
		data->rpm_held = true;
		return 0;
	}

	spin_lock_irqsave(&data->lock, flags);
	if (data->active) {
		ret = sysmmu_control(data, false);
		if (!ret) {
			writel(0, data->sfrbase + REG_MMU_CFG);
			data->active = false;
		}
	}
	spin_unlock_irqrestore(&data->lock, flags);
	if (!ret)
		clk_disable_unprepare(data->clk);
	return ret;
}
static int __maybe_unused sysmmu_suspend(struct device *dev)
{
	struct sysmmu_drvdata *data = dev_get_drvdata(dev);
	unsigned long flags;
	int ret = 0;

	/*
	 * Calliope can still run here (DISPAUD stays on), and its firmware
	 * IOVA 0x80000000 equals the DRAM base. With translation off those
	 * accesses bypass to physical memory and overwrite the kernel image.
	 * 4.4 never disabled the A-Box SYSMMU for system sleep either.
	 */
	if (data->is_abox)
		return 0;

	spin_lock_irqsave(&data->lock, flags);
	if (data->active) {
		ret = sysmmu_control(data, false);
		if (!ret) {
			data->suspended = true;
			data->active = false;
		}
	}
	spin_unlock_irqrestore(&data->lock, flags);
	return ret;
}

static int __maybe_unused sysmmu_resume(struct device *dev)
{
	struct sysmmu_drvdata *data = dev_get_drvdata(dev);
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&data->lock, flags);
	if (data->suspended) {
		ret = sysmmu_enable_locked(data);
		if (!ret)
			data->suspended = false;
	}
	spin_unlock_irqrestore(&data->lock, flags);
	return ret;
}

static const struct dev_pm_ops sysmmu_pm_ops = {
	SET_RUNTIME_PM_OPS(sysmmu_runtime_suspend, sysmmu_runtime_resume, NULL)
	SET_LATE_SYSTEM_SLEEP_PM_OPS(sysmmu_suspend, sysmmu_resume)
};

static int exynos_sysmmu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sysmmu_drvdata *data;
	struct device_node *np;
	u32 qos = DEFAULT_QOS_VALUE;
	int irq, ret;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->sysmmu = dev;
	data->sfrbase = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(data->sfrbase))
		return PTR_ERR(data->sfrbase);
	data->clk = devm_clk_get(dev, "aclk");
	if (IS_ERR(data->clk))
		return PTR_ERR(data->clk);
	/* The vendor samsung,power-domain provider is not a 5.10 genpd. */
	if (!dev->pm_domain)
		return dev_err_probe(dev, -ENODEV, "display genpd required\n");
	mutex_init(&data->mutex);
	spin_lock_init(&data->lock);
	INIT_LIST_HEAD(&data->owners);
	platform_set_drvdata(pdev, data);
	pm_runtime_enable(dev);
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		goto err_pm;
	data->version = MMU_RAW_VER(readl(data->sfrbase + REG_MMU_VERSION));
	if (MMU_MAJ_VER(data->version) != 7) {
		ret = -ENODEV;
		goto err_put;
	}
	if (!(readl(data->sfrbase + REG_MMU_CAPA0_V7) & BIT(11))) {
		data->tlb_props.flags = TLB_TYPE_WAY;
		ret = sysmmu_parse_tlb_way_dt(dev, data);
	} else if (!(readl(data->sfrbase + REG_MMU_CAPA1_V7) & GENMASK(31, 28))) {
		data->tlb_props.flags = TLB_TYPE_PORT;
		ret = sysmmu_parse_tlb_port_dt(dev, data);
	} else {
		/* Unknown TLB configuration: never silently use a v5 sequence. */
		ret = -ENODEV;
	}
	if (ret)
		goto err_put;
	of_property_read_u32(dev->of_node, "qos", &qos);
	data->qos = qos <= 15 ? qos : DEFAULT_QOS_VALUE;
	data->port_name = dev_name(dev);
	of_property_read_string(dev->of_node, "port-name", &data->port_name);
	data->is_abox = !strcmp(data->port_name, "ABOX");
	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = irq;
		goto err_put;
	}
	data->irq = irq;
	ret = devm_request_irq(dev, irq, exynos_sysmmu_irq, 0, dev_name(dev), data);
	if (ret)
		goto err_put;
	if (of_property_read_bool(dev->of_node, "sysmmu,secure-irq")) {
		ret = of_property_read_u32(dev->of_node, "sysmmu,secure_base",
					   &data->securebase);
		if (ret)
			goto err_put;
		irq = platform_get_irq(pdev, 1);
		if (irq < 0) {
			ret = irq;
			goto err_put;
		}
		ret = devm_request_irq(dev, irq, exynos_sysmmu_secure_irq,
				      0, dev_name(dev), data);
		if (ret)
			goto err_put;
	}
	/* Only the DPU SYSMMU carries the splash scan-out (not ABOX etc.). */
	np = strncmp(data->port_name, "DPU", 3) ? NULL :
		of_find_compatible_node(NULL, NULL, "exynos,dpu_fb_handover");
	if (np) {
		struct resource res;

		if (!of_address_to_resource(np, 0, &res) && resource_size(&res)) {
			/* Keep the splash DMA path clocked across runtime PM. */
			ret = clk_prepare_enable(data->clk);
			if (ret) {
				of_node_put(np);
				goto err_put;
			}
			data->boot_clk_held = true;
			data->handover_base = res.start;
			data->handover_size = resource_size(&res);
			data->translation_deferred = true;
			dev_warn(dev, "splash %pa+%#zx: translation deferred until identity map\n",
				 &data->handover_base, data->handover_size);
		}
		of_node_put(np);
	}
	ret = iommu_device_sysfs_add(&data->iommu, dev, NULL, "%s", dev_name(dev));
	if (ret)
		goto err_put;
	iommu_device_set_ops(&data->iommu, &exynos_iommu_ops);
	iommu_device_set_fwnode(&data->iommu, &dev->of_node->fwnode);
	ret = iommu_device_register(&data->iommu);
	if (ret) {
		iommu_device_sysfs_remove(&data->iommu);
		goto err_put;
	}
	pm_runtime_put_sync(dev);
	return 0;
err_put:
	if (data->boot_clk_held) {
		clk_disable_unprepare(data->clk);
		data->boot_clk_held = false;
	}
	pm_runtime_put_sync(dev);
err_pm:
	pm_runtime_disable(dev);
	platform_set_drvdata(pdev, NULL);
	return ret;
}

static const struct of_device_id sysmmu_of_match[] = {
	{ .compatible = "samsung,exynos7885-sysmmu" },
	{ }
};
static struct platform_driver exynos_sysmmu_driver = {
	.probe = exynos_sysmmu_probe,
	.driver = {
		.name = "exynos-sysmmu-7885",
		.of_match_table = sysmmu_of_match,
		.pm = &sysmmu_pm_ops,
		.suppress_bind_attrs = true,
	},
};
static int __init exynos_iommu_init(void)
{
	struct device_node *np;
	int ret;

	np = of_find_matching_node(NULL, sysmmu_of_match);
	if (!np)
		return 0;
	of_node_put(np);
	lv2table_kmem_cache = kmem_cache_create("exynos7885-lv2table",
					LV2TABLE_SIZE, LV2TABLE_SIZE, 0, NULL);
	if (!lv2table_kmem_cache)
		return -ENOMEM;
	ret = platform_driver_register(&exynos_sysmmu_driver);
	if (ret)
		goto err_cache;
	ret = bus_set_iommu(&platform_bus_type, &exynos_iommu_ops);
	if (!ret)
		return 0;
	platform_driver_unregister(&exynos_sysmmu_driver);
err_cache:
	kmem_cache_destroy(lv2table_kmem_cache);
	return ret;
}
subsys_initcall(exynos_iommu_init);
MODULE_LICENSE("GPL");
