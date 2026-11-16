/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ACPM (APM firmware) DVFS requests. Rates are in kHz; the firmware sets the
 * clock and the matching voltage. 4.4 drivers/soc/samsung/cal-if/acpm_dvfs.[ch].
 */
#ifndef __SOC_SAMSUNG_ACPM_DVFS_H
#define __SOC_SAMSUNG_ACPM_DVFS_H

#include <linux/types.h>

/* firmware command ids, 4.4 acpm_dvfs.h:19-26 */
#define ACPM_DVFS_FREQ_REQ	0
#define ACPM_DVFS_FREQ_GET	1
#define ACPM_DVFS_MARGIN_REQ	2
#define ACPM_DVFS_COLDTEMP_REQ	3
#define ACPM_DVFS_POLICY_REQ	4
#define ACPM_DVFS_DATA_INIT	5
#define ACPM_DVFS_SET_FLAG	6
#define ACPM_DVFS_SET_INIT_FREQ	3

/* ids of the dvfs domains are the low bits of the ACPM_DVFS_* values */
#define ACPM_DVFS_IDX(id)	((id) & 0xffff)

/* exynos_acpm_vote() voters */
#define ACPM_VOTER_DISPLAY	0
#define ACPM_VOTER_GPU		1
#define ACPM_VOTER_CPUCL1	2	/* A53, CPUs 0-5 */
#define ACPM_VOTER_CPUCL0	3	/* A73, CPUs 6-7 */
#define ACPM_VOTER_G2D		4
#define ACPM_VOTER_ABOX		5
#define ACPM_VOTER_MAX		6

#if IS_ENABLED(CONFIG_EXYNOS_ACPM_DVFS)
int exynos_acpm_get_rate_table(unsigned int id, unsigned int *rates, unsigned int max);
int exynos_acpm_get_boot_rate(unsigned int id, unsigned long *rate);
int exynos_acpm_set_rate(unsigned int id, unsigned long rate);
int exynos_acpm_set_min_rate(unsigned int id, unsigned long khz);
int exynos_acpm_vote(unsigned int id, unsigned int voter, unsigned long khz);
unsigned long exynos_acpm_get_rate(unsigned int id);
int exynos_acpm_set_init_freq(unsigned int dfs_id, unsigned long freq);
int exynos_acpm_set_volt_margin(unsigned int id, int volt);
int exynos_acpm_set_cold_temp(unsigned int id, bool is_cold_temp);
int exynos_acpm_set_flag(void);
#else
static inline int exynos_acpm_set_rate(unsigned int id, unsigned long rate)
{
	return -ENODEV;
}
static inline int exynos_acpm_set_min_rate(unsigned int id, unsigned long khz)
{
	return -ENODEV;
}
static inline unsigned long exynos_acpm_get_rate(unsigned int id)
{
	return 0;
}
#endif

#endif
