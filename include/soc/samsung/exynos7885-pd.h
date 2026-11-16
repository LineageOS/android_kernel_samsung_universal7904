/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SOC_SAMSUNG_EXYNOS7885_PD_H
#define __SOC_SAMSUNG_EXYNOS7885_PD_H

#include <linux/types.h>

#if IS_ENABLED(CONFIG_EXYNOS7885_PM_DOMAINS)
bool exynos7885_pd_dispaud_is_on(void);
#else
static inline bool exynos7885_pd_dispaud_is_on(void)
{
	return false;
}
#endif

#endif
