/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __EXYNOS7885_PMUCAL_H
#define __EXYNOS7885_PMUCAL_H

#include <linux/io.h>
#include <linux/types.h>

struct device;
struct regmap;

/* Samsung 4.4 pmucal_common.h:17-65, restricted to the local RAE. */
enum pmucal_seq_acctype {
	PMUCAL_READ,
	PMUCAL_WRITE,
	PMUCAL_COND_READ,
	PMUCAL_COND_WRITE,
	PMUCAL_SAVE_RESTORE,
	PMUCAL_COND_SAVE_RESTORE,
	PMUCAL_WAIT,
	PMUCAL_CHECK_SKIP,
	PMUCAL_COND_CHECK_SKIP,
	PMUCAL_DELAY,
};

struct pmucal_seq {
	u32 access_type;
	const char *sfr_name;
	phys_addr_t base_pa;
	void __iomem *base_va;
	u32 offset, mask, value;
	phys_addr_t cond_base_pa;
	void __iomem *cond_base_va;
	u32 cond_offset, cond_mask, cond_value;
	bool need_restore, need_skip;
};

#define PMUCAL_SEQ_DESC(type, name, base, off, m, v, cb, co, cm, cv) \
	{ .access_type = (type), .sfr_name = (name), .base_pa = (base), \
	  .offset = (off), .mask = (m), .value = (v), .cond_base_pa = (cb), \
	  .cond_offset = (co), .cond_mask = (cm), .cond_value = (cv) }

struct exynos7885_pmucal {
	struct device *dev;
	struct regmap *pmu;
	struct list_head *maps;
};

/* SYS_PWR reads are software intentions, not hardware status (-ENODATA if new). */
int exynos7885_pmucal_pmu_read(struct exynos7885_pmucal *cal,
			      u32 offset, u32 *value);
int exynos7885_pmucal_pmu_write(struct exynos7885_pmucal *cal,
			       u32 offset, u32 mask, u32 value);
int exynos7885_pmucal_init(struct exynos7885_pmucal *cal,
			struct device *dev, struct regmap *pmu);
int exynos7885_pmucal_map(struct exynos7885_pmucal *cal,
			struct pmucal_seq *seq, unsigned int count);
int exynos7885_pmucal_run(struct exynos7885_pmucal *cal,
			struct pmucal_seq *seq, unsigned int count);
int exynos7885_pmucal_save(struct exynos7885_pmucal *cal,
			 struct pmucal_seq *seq, unsigned int count);
int exynos7885_pmucal_restore(struct exynos7885_pmucal *cal,
			    struct pmucal_seq *seq, unsigned int count);
#endif
