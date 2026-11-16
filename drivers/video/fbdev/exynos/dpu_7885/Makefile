#
# Copyright (c) 2013 Samsung Electronics Co., Ltd.
#		http://www.samsung.com
#
# Licensed under GPLv2
#

# 4.4 defined CONFIG_SOC_EXYNOS7885 for the SoC; the display code selects its per-SoC paths with it
subdir-ccflags-y	+= -DCONFIG_EXYNOS_DPU20 -DCONFIG_SOC_EXYNOS7885

obj-$(CONFIG_EXYNOS_DPP) += dpp.o
dpp-y := dpp_drv.o
obj-$(CONFIG_EXYNOS_MIPI_DSIM) += dsim.o
dsim-y += dsim_drv.o
obj-$(CONFIG_EXYNOS_DECON_7885) += dsim_reg.o dpp_reg.o decon_reg.o
obj-$(CONFIG_EXYNOS_DECON_7885) += decon.o
obj-$(CONFIG_DECON_EVENT_LOG) += event_log.o
decon-y += decon_core.o decon_dsi.o helper.o win_update.o bts.o fence.o m20_dpu_defer.o
obj-y += panels/
