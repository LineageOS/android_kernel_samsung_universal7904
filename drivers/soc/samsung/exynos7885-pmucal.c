// SPDX-License-Identifier: GPL-2.0
/* Local power-domain RAE, derived from Samsung's 4.4 pmucal_rae.c.
 * Copyright (c) 2016 Samsung Electronics Co., Ltd.
 */
#include <linux/bitmap.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/list.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>

#include "exynos7885-pmucal.h"

#define EXYNOS7885_PMU_BASE 0x11c80000

/*
 * SYS_PWR reads return all ones on this firmware. In particular, update_bits
 * can then skip a requested all-one field entirely. Share the intended values
 * across local/system CAL users and always issue full writes for this bank.
 * Unknown/reserved bits start at zero, never from the inaccessible readback.
 */
#define SYS_PWR_SHADOW_SIZE (0x400 + 0x20)
static u32 sys_pwr_shadow[SYS_PWR_SHADOW_SIZE];
static DECLARE_BITMAP(sys_pwr_valid, SYS_PWR_SHADOW_SIZE);
static DEFINE_RAW_SPINLOCK(sys_pwr_lock);
static bool stock_rmw = true;

static int sys_pwr_index(u32 offset)
{
	if (!IS_ALIGNED(offset, sizeof(u32)))
		return -1;
	if (offset >= 0x1000 && offset < 0x2000)
		return (offset - 0x1000) / sizeof(u32);
	/* The separate APM SYS_PWR controls used by the system tables. */
	switch (offset) {
	case 0x8084:
	case 0x8088:
	case 0x808c:
	case 0x8094:
	case 0x80a0:
	case 0x80a4:
	case 0x80bc:
	case 0x80c8:
		return 0x400 + (offset - 0x8080) / sizeof(u32);
	default:
		return -1;
	}
}

int exynos7885_pmucal_pmu_read(struct exynos7885_pmucal *cal,
			      u32 offset, u32 *value)
{
	unsigned long flags;
	int index = sys_pwr_index(offset);
	int ret = 0;

	if (index < 0 || stock_rmw)
		return regmap_read(cal->pmu, offset, value);
	*value = 0;
	raw_spin_lock_irqsave(&sys_pwr_lock, flags);
	if (test_bit(index, sys_pwr_valid))
		*value = sys_pwr_shadow[index];
	else
		ret = -ENODATA;
	raw_spin_unlock_irqrestore(&sys_pwr_lock, flags);
	return ret;
}

int exynos7885_pmucal_pmu_write(struct exynos7885_pmucal *cal,
			       u32 offset, u32 mask, u32 value)
{
	unsigned long flags;
	u32 next;
	int index = sys_pwr_index(offset);
	int ret;

	/*
	 * v253: stock 4.4 pmucal_rae_write() semantics for every register:
	 * read, modify, and ALWAYS write (never skip). The v249 shadow wrote
	 * zeros into bits that stock RMW sets from the all-ones readback.
	 */
	if (index < 0 || stock_rmw)
		return regmap_write_bits(cal->pmu, offset, mask, value);
	raw_spin_lock_irqsave(&sys_pwr_lock, flags);
	next = test_bit(index, sys_pwr_valid) ? sys_pwr_shadow[index] : 0;
	next = (next & ~mask) | (value & mask);
	/* Do not omit an unchanged write: firmware may have lost the setting. */
	ret = regmap_write(cal->pmu, offset, next);
	if (!ret) {
		sys_pwr_shadow[index] = next;
		__set_bit(index, sys_pwr_valid);
	}
	raw_spin_unlock_irqrestore(&sys_pwr_lock, flags);
	return ret;
}

struct pmucal_map {
	struct list_head node;
	phys_addr_t pa;
	void __iomem *va;
};

int exynos7885_pmucal_init(struct exynos7885_pmucal *cal,
			struct device *dev, struct regmap *pmu)
{
	cal->dev = dev;
	cal->pmu = pmu;
	cal->maps = devm_kzalloc(dev, sizeof(*cal->maps), GFP_KERNEL);
	if (!cal->maps)
		return -ENOMEM;
	INIT_LIST_HEAD(cal->maps);
	return 0;
}

static int pmucal_map_base(struct exynos7885_pmucal *cal, phys_addr_t pa,
			  void __iomem **va)
{
	struct pmucal_map *map;

	if (!pa || pa == EXYNOS7885_PMU_BASE)
		return 0;
	list_for_each_entry(map, cal->maps, node) {
		if (map->pa == pa) {
			*va = map->va;
			return 0;
		}
	}
	map = devm_kzalloc(cal->dev, sizeof(*map), GFP_KERNEL);
	if (!map)
		return -ENOMEM;
	/* CMU/DPP/DMA are also mapped by their functional drivers. */
	map->va = devm_ioremap(cal->dev, pa, SZ_64K);
	if (!map->va)
		return -ENOMEM;
	map->pa = pa;
	list_add_tail(&map->node, cal->maps);
	*va = map->va;
	return 0;
}

int exynos7885_pmucal_map(struct exynos7885_pmucal *cal,
			struct pmucal_seq *seq, unsigned int count)
{
	unsigned int i;
	int ret;

	for (i = 0; i < count; i++) {
		if (!seq[i].base_pa && seq[i].access_type != PMUCAL_DELAY)
			return -EINVAL;
		if (seq[i].offset > SZ_64K - sizeof(u32) ||
		    seq[i].cond_offset > SZ_64K - sizeof(u32) ||
		    !IS_ALIGNED(seq[i].offset, sizeof(u32)) ||
		    !IS_ALIGNED(seq[i].cond_offset, sizeof(u32)))
			return -EINVAL;
		ret = pmucal_map_base(cal, seq[i].base_pa, &seq[i].base_va);
		if (ret)
			return ret;
		ret = pmucal_map_base(cal, seq[i].cond_base_pa,
				      &seq[i].cond_base_va);
		if (ret)
			return ret;
	}
	return 0;
}

static int pmucal_read(struct exynos7885_pmucal *cal, struct pmucal_seq *s,
		       bool condition, u32 *value)
{
	phys_addr_t pa = condition ? s->cond_base_pa : s->base_pa;
	void __iomem *va = condition ? s->cond_base_va : s->base_va;
	u32 offset = condition ? s->cond_offset : s->offset;

	if (pa == EXYNOS7885_PMU_BASE)
		return exynos7885_pmucal_pmu_read(cal, offset, value);
	if (!va)
		return -EINVAL;
	*value = readl(va + offset);
	return 0;
}

static int pmucal_check(struct exynos7885_pmucal *cal, struct pmucal_seq *s,
			bool condition)
{
	u32 value;
	int ret = pmucal_read(cal, s, condition, &value);

	if (ret)
		return ret;
	if (condition)
		return (value & s->cond_mask) == s->cond_value;
	return (value & s->mask) == s->value;
}

static int pmucal_capture(struct exynos7885_pmucal *cal, struct pmucal_seq *s)
{
	u32 value;
	int ret = pmucal_read(cal, s, false, &value);

	if (!ret)
		s->value = value & s->mask;
	return ret;
}

static int pmucal_write(struct exynos7885_pmucal *cal, struct pmucal_seq *s)
{
	u32 value;
	int ret;

	if (s->base_pa == EXYNOS7885_PMU_BASE)
		return exynos7885_pmucal_pmu_write(cal, s->offset, s->mask,
						  s->value);
	if (!s->base_va)
		return -EINVAL;
	if (s->mask == U32_MAX) {
		writel(s->value, s->base_va + s->offset);
		return 0;
	}
	ret = pmucal_read(cal, s, false, &value);
	if (ret)
		return ret;
	writel((value & ~s->mask) | s->value, s->base_va + s->offset);
	return 0;
}

static int pmucal_wait(struct exynos7885_pmucal *cal, struct pmucal_seq *s)
{
	unsigned int timeout;
	int ret;

	/* Match the vendor's 1us polling, with a bounded 1001us timeout. */
	for (timeout = 0; timeout <= 1000; timeout++) {
		ret = pmucal_check(cal, s, false);
		if (ret < 0)
			return ret;
		if (ret)
			return 0;
		udelay(1);
	}
	dev_err(cal->dev, "timeout waiting for %s (%#x, mask %#x)\n",
		s->sfr_name, s->value, s->mask);
	return -ETIMEDOUT;
}

int exynos7885_pmucal_run(struct exynos7885_pmucal *cal,
			struct pmucal_seq *seq, unsigned int count)
{
	unsigned int i;
	int ret;

	for (i = 0; i < count; i++) {
		struct pmucal_seq *s = &seq[i];

		switch (s->access_type) {
		case PMUCAL_COND_READ:
		case PMUCAL_COND_WRITE:
			ret = pmucal_check(cal, s, true);
			if (ret < 0)
				return ret;
			if (!ret)
				continue;
			ret = s->access_type == PMUCAL_COND_READ ?
				pmucal_capture(cal, s) : pmucal_write(cal, s);
			break;
		case PMUCAL_READ:
			ret = pmucal_capture(cal, s);
			break;
		case PMUCAL_WRITE:
			ret = pmucal_write(cal, s);
			break;
		case PMUCAL_WAIT:
			ret = pmucal_wait(cal, s);
			break;
		case PMUCAL_DELAY:
			udelay(s->value);
			ret = 0;
			break;
		default:
			return -EINVAL;
		}
		if (ret)
			return ret;
	}
	return 0;
}

int exynos7885_pmucal_save(struct exynos7885_pmucal *cal,
			 struct pmucal_seq *seq, unsigned int count)
{
	unsigned int i;
	int ret;

	for (i = 0; i < count; i++) {
		struct pmucal_seq *s = &seq[i];

		s->need_restore = false;
		s->need_skip = false;
		switch (s->access_type) {
		case PMUCAL_COND_SAVE_RESTORE:
			ret = pmucal_check(cal, s, true);
			if (ret < 0)
				return ret;
			if (!ret)
				continue;
			fallthrough;
		case PMUCAL_SAVE_RESTORE:
		case PMUCAL_READ:
			/* Plain SAVE_RESTORE/READ ignore condition columns in 4.4. */
			ret = pmucal_capture(cal, s);
			if (ret)
				return ret;
			s->need_restore = s->access_type != PMUCAL_READ;
			break;
		case PMUCAL_WRITE:
		case PMUCAL_CHECK_SKIP:
		case PMUCAL_COND_CHECK_SKIP:
		case PMUCAL_WAIT:
			/* These are restore-only operations, not save-time writes. */
			break;
		default:
			return -EINVAL;
		}
	}
	return 0;
}

int exynos7885_pmucal_restore(struct exynos7885_pmucal *cal,
			    struct pmucal_seq *seq, unsigned int count)
{
	unsigned int i;
	int ret;

	for (i = 0; i < count; i++) {
		struct pmucal_seq *s = &seq[i];

		if (s->need_skip) {
			s->need_skip = false;
			continue;
		}
		switch (s->access_type) {
		case PMUCAL_COND_SAVE_RESTORE:
			ret = pmucal_check(cal, s, true);
			if (ret < 0)
				return ret;
			if (!ret)
				continue;
			fallthrough;
		case PMUCAL_SAVE_RESTORE:
			if (!s->need_restore)
				continue;
			ret = pmucal_write(cal, s);
			if (!ret)
				s->need_restore = false;
			break;
		case PMUCAL_COND_CHECK_SKIP:
			ret = pmucal_check(cal, s, true);
			if (ret < 0)
				return ret;
			if (!ret) {
				if (i + 1 < count)
					seq[i + 1].need_skip = true;
				continue;
			}
			fallthrough;
		case PMUCAL_CHECK_SKIP:
			ret = pmucal_check(cal, s, false);
			if (ret < 0)
				return ret;
			if (i + 1 < count)
				seq[i + 1].need_skip = ret;
			ret = 0;
			break;
		case PMUCAL_WAIT:
			ret = pmucal_wait(cal, s);
			break;
		case PMUCAL_WRITE:
			ret = pmucal_write(cal, s);
			break;
		case PMUCAL_READ:
			/* Diagnostic reads were captured by save; never restore them. */
			ret = 0;
			break;
		default:
			return -EINVAL;
		}
		if (ret)
			return ret;
	}
	return 0;
}
