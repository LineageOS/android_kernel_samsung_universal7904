# SPDX-License-Identifier: GPL-2.0

obj-$(CONFIG_EXYNOS_ASV_ARM)	+= exynos5422-asv.o

obj-$(CONFIG_EXYNOS_CHIPID)	+= exynos-chipid.o exynos-asv.o
obj-$(CONFIG_EXYNOS_PMU)	+= exynos-pmu.o

obj-$(CONFIG_EXYNOS_PMU_ARM_DRIVERS)	+= exynos3250-pmu.o exynos4-pmu.o \
					exynos5250-pmu.o exynos5420-pmu.o
obj-$(CONFIG_EXYNOS_PM_DOMAINS) += pm_domains.o
obj-$(CONFIG_EXYNOS7885_PMUCAL) += exynos7885-pmucal.o
obj-$(CONFIG_EXYNOS7885_PM_DOMAINS) += exynos7885-pd.o
obj-$(CONFIG_EXYNOS7885_PMU_LPM) += exynos7885-sleep.o
obj-$(CONFIG_EXYNOS_SECMEM)	+= secmem.o
obj-$(CONFIG_EXYNOS_REGULATOR_COUPLER) += exynos-regulator-coupler.o

obj-$(CONFIG_SAMSUNG_PM_CHECK)	+= s3c-pm-check.o
obj-$(CONFIG_SAMSUNG_PM_DEBUG)	+= s3c-pm-debug.o

# ACPM: Exynos Advanced Power Manager firmware IPC
obj-$(CONFIG_EXYNOS_ACPM)	+= acpm/
obj-y	+= boot_lcdtype.o

obj-$(CONFIG_CP_PMUCAL) += exynos7885-cp.o pmu-cp.o
obj-y += m20lte-motor-off.o
obj-$(CONFIG_ECT)	+= ect_parser.o
