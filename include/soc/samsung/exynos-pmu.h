/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SAMSUNG_LEGACY_PMU_H
#define __SAMSUNG_LEGACY_PMU_H
#include <linux/err.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/soc/samsung/exynos-pmu.h>
/* Upstream exynos-pmu does not list exynos7885-pmu, so its lookup returns
 * -ENODEV on m20lte; fall back to the syscon by compatible.
 */
static inline struct regmap *exynos_legacy_pmu_regmap(void)
{
 struct regmap *pmu = exynos_get_pmu_regmap();
 if (IS_ERR(pmu))
  pmu = syscon_regmap_lookup_by_compatible("samsung,exynos7885-pmu");
 return pmu;
}
static inline int exynos_pmu_read(unsigned int reg, unsigned int *val)
{
 struct regmap *pmu = exynos_legacy_pmu_regmap();
 return IS_ERR(pmu) ? PTR_ERR(pmu) : regmap_read(pmu, reg, val);
}
static inline int exynos_pmu_write(unsigned int reg, unsigned int val)
{
 struct regmap *pmu = exynos_legacy_pmu_regmap();
 return IS_ERR(pmu) ? PTR_ERR(pmu) : regmap_write(pmu, reg, val);
}
static inline int exynos_pmu_update(unsigned int reg, unsigned int mask, unsigned int val)
{
 struct regmap *pmu = exynos_legacy_pmu_regmap();
 return IS_ERR(pmu) ? PTR_ERR(pmu) : regmap_update_bits(pmu, reg, mask, val);
}
#endif
