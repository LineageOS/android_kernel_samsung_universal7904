// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Exynos Specific Extensions for Synopsys DW Multimedia Card Interface driver
 *
 * Copyright (C) 2012, Samsung Electronics Co., Ltd.
 */

#include <linux/arm-smccc.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>

#include "dw_mmc.h"
#include "dw_mmc-pltfm.h"
#include "dw_mmc-exynos.h"

/* Variations in Exynos specific dw-mshc controller */
enum dw_mci_exynos_type {
	DW_MCI_TYPE_EXYNOS4210,
	DW_MCI_TYPE_EXYNOS4412,
	DW_MCI_TYPE_EXYNOS5250,
	DW_MCI_TYPE_EXYNOS5420,
	DW_MCI_TYPE_EXYNOS5420_SMU,
	DW_MCI_TYPE_EXYNOS7,
	DW_MCI_TYPE_EXYNOS7_SMU,
	DW_MCI_TYPE_EXYNOS7885,
};

/* Exynos implementation specific driver private data */
struct dw_mci_exynos_priv_data {
	enum dw_mci_exynos_type		ctrl_type;
	u8				ciu_div;
	u32				sdr_timing;
	u32				ddr_timing;
	u32				hs400_timing;
	u32				hs200_timing;
	u32				sdr50_timing;
	u32				sdr104_timing;
	u32				hs400_crc_timer;
	bool				fine_tuned;
	u32				tuned_sample;
	u32				cur_speed;
	u32				dqs_delay;
	u32				saved_dqs_en;
	u32				saved_strobe_ctrl;
};

static struct dw_mci_exynos_compatible {
	char				*compatible;
	enum dw_mci_exynos_type		ctrl_type;
} exynos_compat[] = {
	{
		.compatible	= "samsung,exynos4210-dw-mshc",
		.ctrl_type	= DW_MCI_TYPE_EXYNOS4210,
	}, {
		.compatible	= "samsung,exynos4412-dw-mshc",
		.ctrl_type	= DW_MCI_TYPE_EXYNOS4412,
	}, {
		.compatible	= "samsung,exynos5250-dw-mshc",
		.ctrl_type	= DW_MCI_TYPE_EXYNOS5250,
	}, {
		.compatible	= "samsung,exynos5420-dw-mshc",
		.ctrl_type	= DW_MCI_TYPE_EXYNOS5420,
	}, {
		.compatible	= "samsung,exynos5420-dw-mshc-smu",
		.ctrl_type	= DW_MCI_TYPE_EXYNOS5420_SMU,
	}, {
		.compatible	= "samsung,exynos7-dw-mshc",
		.ctrl_type	= DW_MCI_TYPE_EXYNOS7,
	}, {
		.compatible	= "samsung,exynos7-dw-mshc-smu",
		.ctrl_type	= DW_MCI_TYPE_EXYNOS7_SMU,
	}, {
		.compatible	= "samsung,exynos-dw-mshc",
		.ctrl_type	= DW_MCI_TYPE_EXYNOS7885,
	},
};

static bool dw_mci_exynos_clksel64(struct dw_mci_exynos_priv_data *priv)
{
	return priv->ctrl_type == DW_MCI_TYPE_EXYNOS7 ||
		priv->ctrl_type == DW_MCI_TYPE_EXYNOS7_SMU ||
		priv->ctrl_type == DW_MCI_TYPE_EXYNOS7885;
}

static inline u8 dw_mci_exynos_get_ciu_div(struct dw_mci *host)
{
	struct dw_mci_exynos_priv_data *priv = host->priv;

	if (priv->ctrl_type == DW_MCI_TYPE_EXYNOS4412)
		return EXYNOS4412_FIXED_CIU_CLK_DIV;
	else if (priv->ctrl_type == DW_MCI_TYPE_EXYNOS4210)
		return EXYNOS4210_FIXED_CIU_CLK_DIV;
	else if (dw_mci_exynos_clksel64(priv))
		return SDMMC_CLKSEL_GET_DIV(mci_readl(host, CLKSEL64)) + 1;
	else
		return SDMMC_CLKSEL_GET_DIV(mci_readl(host, CLKSEL)) + 1;
}

static void dw_mci_exynos_config_smu(struct dw_mci *host)
{
	struct dw_mci_exynos_priv_data *priv = host->priv;

	/*
	 * If Exynos is provided the Security management,
	 * set for non-ecryption mode at this time.
	 */
	if (priv->ctrl_type == DW_MCI_TYPE_EXYNOS5420_SMU ||
		priv->ctrl_type == DW_MCI_TYPE_EXYNOS7_SMU) {
		mci_writel(host, MPSBEGIN0, 0);
		mci_writel(host, MPSEND0, SDMMC_ENDING_SEC_NR_MAX);
		mci_writel(host, MPSCTRL0, SDMMC_MPSCTRL_SECURE_WRITE_BIT |
			   SDMMC_MPSCTRL_NON_SECURE_READ_BIT |
			   SDMMC_MPSCTRL_VALID |
			   SDMMC_MPSCTRL_NON_SECURE_WRITE_BIT);
	}
}

/*
 * 4.4 drivers/crypto/fmp/smu_dev.c:29-89 and dw_mmc.c:4513-4530,
 * 4855-4864: configure non-FMP descriptors, then initialize/resume the SMU.
 * Do not access protected SMU registers directly or retain firmware's FMP
 * descriptor format: 5.10 uses standard DesignWare descriptors, type zero.
 */
#define EXYNOS7885_FORCE_CLK_STOP	0x0b0
#define EXYNOS7885_MMC_HWACG_CONTROL	BIT(4)

static int dw_mci_exynos7885_smu(struct dw_mci *host, bool resume)
{
	struct dw_mci_exynos_priv_data *priv = host->priv;
	struct arm_smccc_res res;
	u32 command;

	if (priv->ctrl_type != DW_MCI_TYPE_EXYNOS7885)
		return 0;

	arm_smccc_smc(EXYNOS7885_SMC_FMP_SECURITY, 0,
		      EXYNOS7885_SMU_EMBEDDED, EXYNOS7885_FMP_DESC_BYPASS,
		      0, 0, 0, 0, &res);
	if ((int)res.a0)
		goto err;

	command = resume ? EXYNOS7885_SMC_SMU_RESUME : EXYNOS7885_SMC_SMU_INIT;
	arm_smccc_smc(command, EXYNOS7885_SMU_INIT, EXYNOS7885_SMU_EMBEDDED,
		      0, 0, 0, 0, 0, &res);
	if (!(int)res.a0)
		return 0;
err:
	dev_err(host->dev, "SMU firmware configuration failed: %d\n", (int)res.a0);
	return -EIO;
}

static int dw_mci_exynos_priv_init(struct dw_mci *host)
{
	struct dw_mci_exynos_priv_data *priv = host->priv;
	int ret;

	ret = dw_mci_exynos7885_smu(host, false);
	if (ret)
		return ret;
	dw_mci_exynos_config_smu(host);

	/* 4.4 card-init-hwacg-ctrl: firmware leaves FORCE_CLK_STOP[4]
	 * (MMC_HWACG_CONTROL) set, so the CMU Q-channel gates the controller
	 * clock once it idles and the next command times out. Keep it off.
	 */
	if (priv->ctrl_type == DW_MCI_TYPE_EXYNOS7885) {
		u32 fcs = readl(host->regs + EXYNOS7885_FORCE_CLK_STOP);

		writel(fcs & ~EXYNOS7885_MMC_HWACG_CONTROL,
		       host->regs + EXYNOS7885_FORCE_CLK_STOP);
		dev_warn(host->dev, "HWACG off (FORCE_CLK_STOP %#x -> %#x)\n",
			 fcs, readl(host->regs + EXYNOS7885_FORCE_CLK_STOP));
	}

	if (priv->ctrl_type >= DW_MCI_TYPE_EXYNOS5420) {
		priv->saved_strobe_ctrl = mci_readl(host, HS400_DLINE_CTRL);
		priv->saved_dqs_en = mci_readl(host, HS400_DQS_EN);
		priv->saved_dqs_en |= AXI_NON_BLOCKING_WR;
		mci_writel(host, HS400_DQS_EN, priv->saved_dqs_en);
		if (!priv->dqs_delay)
			priv->dqs_delay =
				DQS_CTRL_GET_RD_DELAY(priv->saved_strobe_ctrl);
	}

	host->bus_hz /= (priv->ciu_div + 1);

	return 0;
}

static void dw_mci_exynos_set_clksel_timing(struct dw_mci *host, u32 timing)
{
	struct dw_mci_exynos_priv_data *priv = host->priv;
	u32 clksel;

	if (dw_mci_exynos_clksel64(priv))
		clksel = mci_readl(host, CLKSEL64);
	else
		clksel = mci_readl(host, CLKSEL);

	if (priv->ctrl_type == DW_MCI_TYPE_EXYNOS7885)
		clksel &= ~(EXYNOS7885_CLKSEL_FINE_DRIVE(3) |
			    EXYNOS7885_CLKSEL_ULP);
	clksel = (clksel & ~SDMMC_CLKSEL_TIMING_MASK) | timing;

	if (dw_mci_exynos_clksel64(priv))
		mci_writel(host, CLKSEL64, clksel);
	else
		mci_writel(host, CLKSEL, clksel);

	/*
	 * Exynos4412 and Exynos5250 extends the use of CMD register with the
	 * use of bit 29 (which is reserved on standard MSHC controllers) for
	 * optionally bypassing the HOLD register for command and data. The
	 * HOLD register should be bypassed in case there is no phase shift
	 * applied on CMD/DATA that is sent to the card.
	 */
	if (!SDMMC_CLKSEL_GET_DRV_WD3(clksel) && host->slot)
		set_bit(DW_MMC_CARD_NO_USE_HOLD, &host->slot->flags);
}

#ifdef CONFIG_PM
static int dw_mci_exynos_runtime_resume(struct device *dev)
{
	struct dw_mci *host = dev_get_drvdata(dev);
	int ret;

	ret = dw_mci_runtime_resume(dev);
	if (ret)
		return ret;

	dw_mci_exynos_config_smu(host);
	ret = dw_mci_exynos7885_smu(host, true);
	if (ret)
		dw_mci_runtime_suspend(dev);

	return ret;
}
#endif /* CONFIG_PM */

#ifdef CONFIG_PM_SLEEP
/**
 * dw_mci_exynos_suspend_noirq - Exynos-specific suspend code
 * @dev: Device to suspend (this device)
 *
 * This ensures that device will be in runtime active state in
 * dw_mci_exynos_resume_noirq after calling pm_runtime_force_resume()
 */
static int dw_mci_exynos_suspend_noirq(struct device *dev)
{
	pm_runtime_get_noresume(dev);
	return pm_runtime_force_suspend(dev);
}

/**
 * dw_mci_exynos_resume_noirq - Exynos-specific resume code
 * @dev: Device to resume (this device)
 *
 * On exynos5420 there is a silicon errata that will sometimes leave the
 * WAKEUP_INT bit in the CLKSEL register asserted.  This bit is 1 to indicate
 * that it fired and we can clear it by writing a 1 back.  Clear it to prevent
 * interrupts from going off constantly.
 *
 * We run this code on all exynos variants because it doesn't hurt.
 */
static int dw_mci_exynos_resume_noirq(struct device *dev)
{
	struct dw_mci *host = dev_get_drvdata(dev);
	struct dw_mci_exynos_priv_data *priv = host->priv;
	u32 clksel;
	int ret;

	ret = pm_runtime_force_resume(dev);
	if (ret)
		return ret;

	if (dw_mci_exynos_clksel64(priv))
		clksel = mci_readl(host, CLKSEL64);
	else
		clksel = mci_readl(host, CLKSEL);

	if (clksel & SDMMC_CLKSEL_WAKEUP_INT) {
		if (dw_mci_exynos_clksel64(priv))
			mci_writel(host, CLKSEL64, clksel);
		else
			mci_writel(host, CLKSEL, clksel);
	}

	pm_runtime_put(dev);

	return 0;
}
#endif /* CONFIG_PM_SLEEP */

static void dw_mci_exynos_config_hs400(struct dw_mci *host, u32 timing)
{
	struct dw_mci_exynos_priv_data *priv = host->priv;
	u32 dqs, strobe;

	/*
	 * Not supported to configure register
	 * related to HS400
	 */
	if (priv->ctrl_type < DW_MCI_TYPE_EXYNOS5420) {
		if (timing == MMC_TIMING_MMC_HS400)
			dev_warn(host->dev,
				 "cannot configure HS400, unsupported chipset\n");
		return;
	}

	dqs = priv->saved_dqs_en;
	strobe = priv->saved_strobe_ctrl;

	/* 4.4 dw_mmc-exynos.c:408-451, non-ULP branch. */
	if (priv->ctrl_type == DW_MCI_TYPE_EXYNOS7885) {
		if (timing == MMC_TIMING_MMC_HS400) {
			dqs &= ~EXYNOS7885_TXDT_CRC_TIMER(0xff, 0xff);
			dqs |= priv->hs400_crc_timer | EXYNOS7885_RDDQS_EN |
			       AXI_NON_BLOCKING_WR | DATA_STROBE_EN;
			strobe = EXYNOS7885_FIFO_CLK_DELAY(2) |
				 (priv->dqs_delay & 0x3ff);
		} else {
			dqs &= ~DATA_STROBE_EN;
		}
		mci_writel(host, HS400_DQS_EN, dqs);
		mci_writel(host, HS400_DLINE_CTRL, strobe);
		return;
	}

	if (timing == MMC_TIMING_MMC_HS400) {
		dqs |= DATA_STROBE_EN;
		strobe = DQS_CTRL_RD_DELAY(strobe, priv->dqs_delay);
	} else if (timing == MMC_TIMING_UHS_SDR104) {
		dqs &= 0xffffff00;
	} else {
		dqs &= ~DATA_STROBE_EN;
	}

	mci_writel(host, HS400_DQS_EN, dqs);
	mci_writel(host, HS400_DLINE_CTRL, strobe);
}

static void dw_mci_exynos_adjust_clock(struct dw_mci *host, unsigned int wanted)
{
	struct dw_mci_exynos_priv_data *priv = host->priv;
	unsigned long actual;
	u8 div;
	int ret;
	/*
	 * Don't care if wanted clock is zero or
	 * ciu clock is unavailable
	 */
	if (!wanted || IS_ERR(host->ciu_clk))
		return;

	/* Guaranteed minimum frequency for cclkin */
	if (wanted < EXYNOS_CCLKIN_MIN)
		wanted = EXYNOS_CCLKIN_MIN;

	if (wanted == priv->cur_speed)
		return;

	div = dw_mci_exynos_get_ciu_div(host);
	ret = clk_set_rate(host->ciu_clk, wanted * div);
	if (ret)
		dev_warn(host->dev,
			"failed to set clk-rate %u error: %d\n",
			wanted * div, ret);
	actual = clk_get_rate(host->ciu_clk);
	host->bus_hz = actual / div;
	priv->cur_speed = wanted;
	host->current_speed = 0;
	if (priv->ctrl_type == DW_MCI_TYPE_EXYNOS7885)
		dev_dbg(host->dev, "cclkin %u div %u ciu %lu bus %u\n",
			 wanted, div, actual, host->bus_hz);
}

/* 4.4 exynos7904-m20lte_common.dtsi ciu_clkin, indexed by MMC_TIMING_*. */
static const u16 exynos7885_cclkin_mhz[] = {
	25, 50, 50, 25, 50, 100, 200, 50, 50, 200, 200, 200,
};

/* 4.4 dw_mmc-exynos.c:891-914: CLKSEL bit 6 adds half a sample phase. */
static void dw_mci_exynos7885_set_fine(struct dw_mci *host, bool fine)
{
	u32 clksel = mci_readl(host, CLKSEL64);

	if (fine)
		clksel |= EXYNOS7885_CLKSEL_FINE_TUNE;
	else
		clksel &= ~EXYNOS7885_CLKSEL_FINE_TUNE;
	mci_writel(host, CLKSEL64, clksel);
}

static void dw_mci_exynos_set_ios(struct dw_mci *host, struct mmc_ios *ios)
{
	struct dw_mci_exynos_priv_data *priv = host->priv;
	unsigned int wanted = ios->clock;
	u32 timing = ios->timing, clksel;

	/*
	 * 4.4 dw_mmc-exynos.c:489-576: the input clock follows the per-timing
	 * reference table (ciu_clkin) rather than the requested card clock.
	 */
	if (priv->ctrl_type == DW_MCI_TYPE_EXYNOS7885 && ios->clock) {
		if (timing < ARRAY_SIZE(exynos7885_cclkin_mhz))
			wanted = exynos7885_cclkin_mhz[timing] * 1000000;
		if (timing != MMC_TIMING_MMC_HS200)
			priv->fine_tuned = false;
	}

	switch (timing) {
	case MMC_TIMING_MMC_HS200:
		if (priv->ctrl_type != DW_MCI_TYPE_EXYNOS7885) {
			clksel = priv->sdr_timing;
			break;
		}
		clksel = SDMMC_CLKSEL_UP_SAMPLE(priv->hs200_timing,
						priv->tuned_sample);
		break;
	case MMC_TIMING_MMC_HS400:
		/* Update tuned sample timing */
		clksel = SDMMC_CLKSEL_UP_SAMPLE(
				priv->hs400_timing, priv->tuned_sample);
		wanted <<= 1;
		break;
	case MMC_TIMING_MMC_DDR52:
		clksel = priv->ddr_timing;
		/* Should be double rate for DDR mode */
		if (ios->bus_width == MMC_BUS_WIDTH_8)
			wanted <<= 1;
		break;
	case MMC_TIMING_UHS_SDR104:
	case MMC_TIMING_UHS_SDR50:
		/* 4.4 dw_mmc-exynos.c:527-546: per-mode timing plus tuned sample. */
		if (priv->ctrl_type == DW_MCI_TYPE_EXYNOS7885) {
			u32 t = timing == MMC_TIMING_UHS_SDR104 ?
				priv->sdr104_timing : priv->sdr50_timing;

			clksel = SDMMC_CLKSEL_UP_SAMPLE(t ? t : priv->sdr_timing,
							priv->tuned_sample);
			break;
		}
		clksel = (priv->sdr_timing & 0xfff8ffff) |
			(priv->ciu_div << 16);
		break;
	case MMC_TIMING_UHS_DDR50:
		clksel = (priv->ddr_timing & 0xfff8ffff) |
			(priv->ciu_div << 16);
		break;
	default:
		clksel = priv->sdr_timing;
	}

	/* 4.4 dw_mmc-exynos.c:560-561: disable phase-7 mux at identification. */
	if (priv->ctrl_type == DW_MCI_TYPE_EXYNOS7885 &&
	    ios->clock && ios->clock <= 400000)
		mci_writel(host, AXI_BURST_LEN,
			   mci_readl(host, AXI_BURST_LEN) & ~EXYNOS7885_PHASE7_ENABLE);

	/* Set clock timing for the requested speed mode*/
	dw_mci_exynos_set_clksel_timing(host, clksel);
	if (priv->ctrl_type == DW_MCI_TYPE_EXYNOS7885)
		dw_mci_exynos7885_set_fine(host, priv->fine_tuned);

	/* Configure setting for HS400 */
	dw_mci_exynos_config_hs400(host, timing);

	/* Configure clock rate */
	dw_mci_exynos_adjust_clock(host, wanted);
}

/* 4.4 dw_mmc-exynos.h:162-180: divider, fine drive, drive, sample. */
static int dw_mci_exynos7885_parse_timing(struct device_node *np,
					const char *name, u32 div, u32 *value)
{
	u32 timing[4];
	int ret;

	if (of_property_count_u32_elems(np, name) != ARRAY_SIZE(timing))
		return -EINVAL;
	ret = of_property_read_u32_array(np, name, timing, ARRAY_SIZE(timing));
	if (ret)
		return ret;
	if (timing[0] != div || timing[0] > 7 || timing[1] > 3 ||
	    timing[2] > 7 || timing[3] > 7)
		return -EINVAL;

	*value = SDMMC_CLKSEL_TIMING(timing[3], timing[2], timing[0]) |
		EXYNOS7885_CLKSEL_FINE_DRIVE(timing[1]);
	return 0;
}

static int dw_mci_exynos_parse_dt(struct dw_mci *host)
{
	struct dw_mci_exynos_priv_data *priv;
	struct device_node *np = host->dev->of_node;
	u32 timing[2];
	u32 div = 0;
	int idx;
	int ret;

	priv = devm_kzalloc(host->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	for (idx = 0; idx < ARRAY_SIZE(exynos_compat); idx++) {
		if (of_device_is_compatible(np, exynos_compat[idx].compatible))
			priv->ctrl_type = exynos_compat[idx].ctrl_type;
	}

	if (priv->ctrl_type == DW_MCI_TYPE_EXYNOS4412)
		priv->ciu_div = EXYNOS4412_FIXED_CIU_CLK_DIV - 1;
	else if (priv->ctrl_type == DW_MCI_TYPE_EXYNOS4210)
		priv->ciu_div = EXYNOS4210_FIXED_CIU_CLK_DIV - 1;
	else {
		of_property_read_u32(np, "samsung,dw-mshc-ciu-div", &div);
		priv->ciu_div = div;
	}

	if (priv->ctrl_type == DW_MCI_TYPE_EXYNOS7885) {
		struct device_node *smu;
		bool valid;

		if (of_property_read_u32(np, "samsung,dw-mshc-ciu-div", &div) ||
		    div > 7)
			return -EINVAL;

		/* Retain the vendor reference, but no separate SMU driver is needed. */
		smu = of_parse_phandle(np, "mmc-exynos-smu", 0);
		valid = smu && of_device_is_available(smu) &&
			of_device_is_compatible(smu, "samsung,exynos-smu");
		of_node_put(smu);
		if (!valid)
			return -EINVAL;

		ret = dw_mci_exynos7885_parse_timing(np,
				"samsung,dw-mshc-sdr-timing", div, &priv->sdr_timing);
		if (ret)
			return ret;
		ret = dw_mci_exynos7885_parse_timing(np,
				"samsung,dw-mshc-ddr-timing", div, &priv->ddr_timing);
		if (ret)
			return ret;
		if (of_find_property(np, "samsung,dw-mshc-sdr50-timing", NULL)) {
			ret = dw_mci_exynos7885_parse_timing(np,
					"samsung,dw-mshc-sdr50-timing", div,
					&priv->sdr50_timing);
			if (ret)
				return ret;
		}
		if (of_find_property(np, "samsung,dw-mshc-sdr104-timing", NULL)) {
			ret = dw_mci_exynos7885_parse_timing(np,
					"samsung,dw-mshc-sdr104-timing", div,
					&priv->sdr104_timing);
			if (ret)
				return ret;
		}
		if (of_find_property(np, "samsung,dw-mshc-hs400-timing", NULL)) {
			u32 crc[2] = { 0x13, 0x15 };

			/* HS400 uses its own fixed divider (4.4 timing <1 0 2 0>). */
			ret = dw_mci_exynos7885_parse_timing(np,
					"samsung,dw-mshc-hs400-timing",
					HS400_FIXED_CIU_CLK_DIV,
					&priv->hs400_timing);
			if (ret)
				return ret;
			of_property_read_u32(np, "samsung,dw-mshc-hs400-delay-line",
					     &priv->dqs_delay);
			of_property_read_u32(np,
				"samsung,dw-mshc-txdt-crc-timer-fastlimit", &crc[0]);
			of_property_read_u32(np,
				"samsung,dw-mshc-txdt-crc-timer-initval", &crc[1]);
			priv->hs400_crc_timer = EXYNOS7885_TXDT_CRC_TIMER(crc[0],
									  crc[1]);
		}
		if (of_find_property(np, "samsung,dw-mshc-hs200-timing", NULL)) {
			ret = dw_mci_exynos7885_parse_timing(np,
					"samsung,dw-mshc-hs200-timing", div,
					&priv->hs200_timing);
			if (ret)
				return ret;
		}
		host->priv = priv;
		return 0;
	}

	ret = of_property_read_u32_array(np,
			"samsung,dw-mshc-sdr-timing", timing, 2);
	if (ret)
		return ret;

	priv->sdr_timing = SDMMC_CLKSEL_TIMING(timing[0], timing[1], div);

	ret = of_property_read_u32_array(np,
			"samsung,dw-mshc-ddr-timing", timing, 2);
	if (ret)
		return ret;

	priv->ddr_timing = SDMMC_CLKSEL_TIMING(timing[0], timing[1], div);

	ret = of_property_read_u32_array(np,
			"samsung,dw-mshc-hs400-timing", timing, 2);
	if (!ret && of_property_read_u32(np,
				"samsung,read-strobe-delay", &priv->dqs_delay))
		dev_dbg(host->dev,
			"read-strobe-delay is not found, assuming usage of default value\n");

	priv->hs400_timing = SDMMC_CLKSEL_TIMING(timing[0], timing[1],
						HS400_FIXED_CIU_CLK_DIV);
	host->priv = priv;
	return 0;
}

static inline u8 dw_mci_exynos_get_clksmpl(struct dw_mci *host)
{
	struct dw_mci_exynos_priv_data *priv = host->priv;

	if (dw_mci_exynos_clksel64(priv))
		return SDMMC_CLKSEL_CCLK_SAMPLE(mci_readl(host, CLKSEL64));
	else
		return SDMMC_CLKSEL_CCLK_SAMPLE(mci_readl(host, CLKSEL));
}

static inline void dw_mci_exynos_set_clksmpl(struct dw_mci *host, u8 sample)
{
	u32 clksel;
	struct dw_mci_exynos_priv_data *priv = host->priv;

	if (dw_mci_exynos_clksel64(priv))
		clksel = mci_readl(host, CLKSEL64);
	else
		clksel = mci_readl(host, CLKSEL);
	clksel = SDMMC_CLKSEL_UP_SAMPLE(clksel, sample);
	if (dw_mci_exynos_clksel64(priv))
		mci_writel(host, CLKSEL64, clksel);
	else
		mci_writel(host, CLKSEL, clksel);
}

static inline u8 dw_mci_exynos_move_next_clksmpl(struct dw_mci *host)
{
	struct dw_mci_exynos_priv_data *priv = host->priv;
	u32 clksel;
	u8 sample;

	if (dw_mci_exynos_clksel64(priv))
		clksel = mci_readl(host, CLKSEL64);
	else
		clksel = mci_readl(host, CLKSEL);

	sample = (clksel + 1) & 0x7;
	clksel = SDMMC_CLKSEL_UP_SAMPLE(clksel, sample);

	if (dw_mci_exynos_clksel64(priv))
		mci_writel(host, CLKSEL64, clksel);
	else
		mci_writel(host, CLKSEL, clksel);

	return sample;
}

static s8 dw_mci_exynos_get_best_clksmpl(u8 candiates)
{
	const u8 iter = 8;
	u8 __c;
	s8 i, loc = -1;

	for (i = 0; i < iter; i++) {
		__c = ror8(candiates, i);
		if ((__c & 0xc7) == 0xc7) {
			loc = i;
			goto out;
		}
	}

	for (i = 0; i < iter; i++) {
		__c = ror8(candiates, i);
		if ((__c & 0x83) == 0x83) {
			loc = i;
			goto out;
		}
	}

	/*
	 * If there is no cadiates value, then it needs to return -EIO.
	 * If there are candiates values and don't find bset clk sample value,
	 * then use a first candiates clock sample value.
	 */
	for (i = 0; i < iter; i++) {
		__c = ror8(candiates, i);
		if ((__c & 0x1) == 0x1) {
			loc = i;
			goto out;
		}
	}
out:
	return loc;
}

static int dw_mci_exynos_execute_tuning(struct dw_mci_slot *slot, u32 opcode)
{
	struct dw_mci *host = slot->host;
	struct dw_mci_exynos_priv_data *priv = host->priv;
	struct mmc_host *mmc = slot->mmc;
	u8 start_smpl, smpl, candiates = 0;
	s8 found;
	int ret = 0;

	start_smpl = dw_mci_exynos_get_clksmpl(host);

	do {
		mci_writel(host, TMOUT, ~0);
		smpl = dw_mci_exynos_move_next_clksmpl(host);

		if (!mmc_send_tuning(mmc, opcode, NULL))
			candiates |= (1 << smpl);

	} while (start_smpl != smpl);

	found = dw_mci_exynos_get_best_clksmpl(candiates);
	if (found >= 0) {
		dw_mci_exynos_set_clksmpl(host, found);
		priv->tuned_sample = found;
	} else {
		ret = -EIO;
		dev_warn(&mmc->class_dev,
			"There is no candiates value about clksmpl!\n");
	}

	return ret;
}

/*
 * 4.4 dw_mmc-exynos.c:985-1220: sixteen sample points (eight phases, each
 * with and without the fine-tuning half step).  Use the middle of the
 * longest passing run instead of the vendor's pattern table.
 */
static int dw_mci_exynos7885_execute_tuning(struct dw_mci_slot *slot,
					    u32 opcode)
{
	struct dw_mci *host = slot->host;
	struct dw_mci_exynos_priv_data *priv = host->priv;
	struct mmc_host *mmc = slot->mmc;
	u32 map = 0, burst;
	int i, len, best = -1, best_len = 0, start;

	for (i = 0; i < 16; i++) {
		mci_writel(host, TMOUT, ~0);
		dw_mci_exynos_set_clksmpl(host, i >> 1);
		dw_mci_exynos7885_set_fine(host, i & 1);
		burst = mci_readl(host, AXI_BURST_LEN);
		if ((i >> 1) == 7)
			burst |= EXYNOS7885_PHASE7_ENABLE;
		else
			burst &= ~EXYNOS7885_PHASE7_ENABLE;
		mci_writel(host, AXI_BURST_LEN, burst);
		if (!mmc_send_tuning(mmc, opcode, NULL))
			map |= BIT(i);
	}

	if (map == 0xffff) {
		best = 8;
		best_len = 16;
	} else {
		for (start = 0; start < 16; start++) {
			if (!(map & BIT(start)) ||
			    (map & BIT((start + 15) & 15)))
				continue;
			for (len = 0; len < 16 &&
			     (map & BIT((start + len) & 15)); len++)
				;
			if (len > best_len) {
				best_len = len;
				best = (start + (len - 1) / 2) & 15;
			}
		}
	}

	dev_dbg(host->dev, "tuning map 0x%04x best %d len %d\n",
		 map, best, best_len);
	if (best < 0 || best_len < 3)
		return -EIO;

	priv->tuned_sample = best >> 1;
	priv->fine_tuned = best & 1;
	dw_mci_exynos_set_clksmpl(host, priv->tuned_sample);
	dw_mci_exynos7885_set_fine(host, priv->fine_tuned);
	burst = mci_readl(host, AXI_BURST_LEN);
	if (priv->tuned_sample == 7)
		burst |= EXYNOS7885_PHASE7_ENABLE;
	else
		burst &= ~EXYNOS7885_PHASE7_ENABLE;
	mci_writel(host, AXI_BURST_LEN, burst);
	return 0;
}

static int dw_mci_exynos_prepare_hs400_tuning(struct dw_mci *host,
					struct mmc_ios *ios)
{
	struct dw_mci_exynos_priv_data *priv = host->priv;

	dw_mci_exynos_set_clksel_timing(host, priv->hs400_timing);
	dw_mci_exynos_adjust_clock(host, (ios->clock) << 1);

	return 0;
}

/* Common capabilities of Exynos4/Exynos5 SoC */
static unsigned long exynos_dwmmc_caps[4] = {
	MMC_CAP_1_8V_DDR | MMC_CAP_8_BIT_DATA | MMC_CAP_CMD23,
	MMC_CAP_CMD23,
	MMC_CAP_CMD23,
	MMC_CAP_CMD23,
};

static const struct dw_mci_drv_data exynos_drv_data = {
	.caps			= exynos_dwmmc_caps,
	.num_caps		= ARRAY_SIZE(exynos_dwmmc_caps),
	.init			= dw_mci_exynos_priv_init,
	.set_ios		= dw_mci_exynos_set_ios,
	.parse_dt		= dw_mci_exynos_parse_dt,
	.execute_tuning		= dw_mci_exynos_execute_tuning,
	.prepare_hs400_tuning	= dw_mci_exynos_prepare_hs400_tuning,
};

/* Start with SDR only; do not inherit Exynos4/5's unconditional DDR cap. */
static unsigned long exynos7885_dwmmc_caps[] = { MMC_CAP_CMD23 };

static const struct dw_mci_drv_data exynos7885_drv_data = {
	.caps			= exynos7885_dwmmc_caps,
	.num_caps		= ARRAY_SIZE(exynos7885_dwmmc_caps),
	.init			= dw_mci_exynos_priv_init,
	.set_ios		= dw_mci_exynos_set_ios,
	.parse_dt		= dw_mci_exynos_parse_dt,
	.execute_tuning		= dw_mci_exynos7885_execute_tuning,
	.prepare_hs400_tuning	= dw_mci_exynos_prepare_hs400_tuning,
};

static const struct of_device_id dw_mci_exynos_match[] = {
	{ .compatible = "samsung,exynos-dw-mshc",
			.data = &exynos7885_drv_data, },
	{ .compatible = "samsung,exynos4412-dw-mshc",
			.data = &exynos_drv_data, },
	{ .compatible = "samsung,exynos5250-dw-mshc",
			.data = &exynos_drv_data, },
	{ .compatible = "samsung,exynos5420-dw-mshc",
			.data = &exynos_drv_data, },
	{ .compatible = "samsung,exynos5420-dw-mshc-smu",
			.data = &exynos_drv_data, },
	{ .compatible = "samsung,exynos7-dw-mshc",
			.data = &exynos_drv_data, },
	{ .compatible = "samsung,exynos7-dw-mshc-smu",
			.data = &exynos_drv_data, },
	{},
};
MODULE_DEVICE_TABLE(of, dw_mci_exynos_match);

static int dw_mci_exynos_probe(struct platform_device *pdev)
{
	const struct dw_mci_drv_data *drv_data;
	const struct of_device_id *match;
	int ret;

	match = of_match_node(dw_mci_exynos_match, pdev->dev.of_node);
	drv_data = match->data;

	pm_runtime_get_noresume(&pdev->dev);
	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);

	ret = dw_mci_pltfm_register(pdev, drv_data);
	if (ret) {
		pm_runtime_disable(&pdev->dev);
		pm_runtime_set_suspended(&pdev->dev);
		pm_runtime_put_noidle(&pdev->dev);

		return ret;
	}

	return 0;
}

static int dw_mci_exynos_remove(struct platform_device *pdev)
{
	pm_runtime_disable(&pdev->dev);
	pm_runtime_set_suspended(&pdev->dev);
	pm_runtime_put_noidle(&pdev->dev);

	return dw_mci_pltfm_remove(pdev);
}

static const struct dev_pm_ops dw_mci_exynos_pmops = {
	SET_NOIRQ_SYSTEM_SLEEP_PM_OPS(dw_mci_exynos_suspend_noirq,
				      dw_mci_exynos_resume_noirq)
	SET_RUNTIME_PM_OPS(dw_mci_runtime_suspend,
			   dw_mci_exynos_runtime_resume,
			   NULL)
};

static struct platform_driver dw_mci_exynos_pltfm_driver = {
	.probe		= dw_mci_exynos_probe,
	.remove		= dw_mci_exynos_remove,
	.driver		= {
		.name		= "dwmmc_exynos",
		.probe_type	= PROBE_PREFER_ASYNCHRONOUS,
		.of_match_table	= dw_mci_exynos_match,
		.pm		= &dw_mci_exynos_pmops,
	},
};

module_platform_driver(dw_mci_exynos_pltfm_driver);

MODULE_DESCRIPTION("Samsung Specific DW-MSHC Driver Extension");
MODULE_AUTHOR("Thomas Abraham <thomas.ab@samsung.com");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:dwmmc_exynos");
