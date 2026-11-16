/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal 4.4 exynos-pd.h shim for the Mali glue. The 5.10 port drives G3D
 * through exynos7885-pd (genpd); the 4.4 cal_pd_* interface does not exist.
 */
#ifndef __SOC_SAMSUNG_EXYNOS_PD_H
#define __SOC_SAMSUNG_EXYNOS_PD_H

#include <linux/mutex.h>
#include <linux/pm_domain.h>

struct exynos_pm_domain {
	struct generic_pm_domain genpd;
	struct mutex access_lock;
	unsigned int cal_pdid;
};

/* Only used by the inter-frame PM path (CONFIG_MALI_RT_PM, off). */
static inline int cal_pd_status(unsigned int id) { return 0; }
static inline int cal_pd_control(unsigned int id, int on) { return -ENOSYS; }

#endif
