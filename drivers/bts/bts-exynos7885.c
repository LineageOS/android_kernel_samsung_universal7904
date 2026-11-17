/*
 * Copyright (c) 2016 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/export.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>
#include <linux/uaccess.h>
#include <linux/debugfs.h>
#include <linux/slab.h>
#include <linux/syscore_ops.h>

#include <soc/samsung/bts.h>
#include "cal_bts7885.h"

#define BTS_DBG(x...)		if (exynos_bts_log) pr_info(x)

#define NUM_CHANNEL		4
#define MIF_UTIL		65
#define INT_UTIL		70

static unsigned int exynos_bts_log;
static unsigned int exynos_mif_util = MIF_UTIL;
static unsigned int exynos_int_util = INT_UTIL;

enum bts_index {
	BTS_IDX_G3D,
	BTS_IDX_MFCMSCL,
	BTS_IDX_FSYS,
	BTS_IDX_GNSS,
	BTS_IDX_ISP0,
	BTS_IDX_ISP1,
	BTS_IDX_WLBT,
	BTS_IDX_CP,
	BTS_IDX_CAM,
	BTS_IDX_DPU,
	BTS_IDX_ABOX,
	BTS_IDX_CPU_DMC0,
	BTS_IDX_CPU_DMC1,
};

enum exynos_bts_type {
	BT_TREX,
};

struct bts_table {
	struct bts_status stat;
	struct bts_info *next_bts;
	int prev_scen;
	int next_scen;
};

struct bts_info {
	const char *name;
	unsigned int pa_base;
	void __iomem *va_base;
	bool enable;
	enum exynos_bts_type type;
	struct bts_table table[BS_MAX];
	enum bts_scen_type top_scen;
};

struct bts_scenario {
	const char *name;
	struct bts_info *head;
};

/*
 * 4.4 bts-exynos7885.c:487-488,989-990 used pm_qos_add/update_request
 * with PM_QOS_BUS_THROUGHPUT and PM_QOS_DEVICE_THROUGHPUT. 5.10's
 * dev_pm_qos/freq_qos need a real frequency provider; no Exynos7904
 * MIF/INT devfreq or OPP provider exists here. Record requirements only.
 * This file never called cal_dfs_get_rate or power-domain lookup APIs;
 * those dependencies belong to the display driver, not the BTS core.
 */
static struct bts_bw ip_bw[BTS_BW_MAX];
static unsigned int requested_mif_freq;
static unsigned int requested_int_freq;
static bool bts_hw_ready;
static DEFINE_SPINLOCK(bts_lock);
static DEFINE_MUTEX(media_mutex);
static void __iomem *base_drex0;
static void __iomem *base_drex1;

/* Vendor bts-exynos7885.c:83-207: all 13 IP tables and scenarios. */
static struct bts_info exynos_bts[] = {
	[BTS_IDX_G3D] = {
		.name ="g3d",
		.pa_base = EXYNOS7885_PA_G3D,
		.type = BT_TREX,
		.enable = true,
		.table[BS_DEFAULT].stat.scen_en = true,
		.table[BS_DEFAULT].stat.priority = 0x4,
	},
	[BTS_IDX_MFCMSCL] = {
		.name = "mfcmscl",
		.pa_base = EXYNOS7885_PA_MFCMSCL,
		.type = BT_TREX,
		.enable = true,
		.table[BS_DEFAULT].stat.scen_en = true,
		.table[BS_DEFAULT].stat.priority = 0x4,
		.table[BS_DEFAULT].stat.rmo = 0x10,
		.table[BS_DEFAULT].stat.wmo = 0x10,
		.table[BS_MFC_UHD].stat.scen_en = true,
		.table[BS_MFC_UHD].stat.priority = 0x4,
		.table[BS_MFC_UHD].stat.rmo = BTS_MAX_MO,
		.table[BS_MFC_UHD].stat.wmo = BTS_MAX_MO,
	},
	[BTS_IDX_FSYS] = {
		.name = "fsys",
		.pa_base = EXYNOS7885_PA_FSYS,
		.type = BT_TREX,
		.enable = true,
		.table[BS_DEFAULT].stat.scen_en = true,
		.table[BS_DEFAULT].stat.priority = 0x4,
	},
	[BTS_IDX_GNSS] = {
		.name = "gnss",
		.pa_base = EXYNOS7885_PA_GNSS,
		.type = BT_TREX,
		.enable = true,
		.table[BS_DEFAULT].stat.scen_en = true,
		.table[BS_DEFAULT].stat.priority = 0x4,
	},
	[BTS_IDX_ISP0] = {
		.name = "isp0",
		.pa_base = EXYNOS7885_PA_ISP0,
		.type = BT_TREX,
		.enable = true,
		.table[BS_DEFAULT].stat.scen_en = true,
		.table[BS_DEFAULT].stat.priority = 0x4,
	},
	[BTS_IDX_ISP1] = {
		.name = "isp1",
		.pa_base = EXYNOS7885_PA_ISP1,
		.type = BT_TREX,
		.enable = true,
		.table[BS_DEFAULT].stat.scen_en = true,
		.table[BS_DEFAULT].stat.priority = 0x4,
	},
	[BTS_IDX_WLBT] = {
		.name = "wlbt",
		.pa_base = EXYNOS7885_PA_WLBT,
		.type = BT_TREX,
		.enable = true,
		.table[BS_DEFAULT].stat.scen_en = true,
		.table[BS_DEFAULT].stat.priority = 0x4,
	},
	[BTS_IDX_CP] = {
		.name = "cp",
		.pa_base = EXYNOS7885_PA_CP,
		.type = BT_TREX,
		.enable = true,
		.table[BS_DEFAULT].stat.scen_en = true,
		.table[BS_DEFAULT].stat.bypass_en = 0x1,
	},
	[BTS_IDX_CAM] = {
		.name = "cam",
		.pa_base = EXYNOS7885_PA_CAM,
		.type = BT_TREX,
		.enable = true,
		.table[BS_DEFAULT].stat.scen_en = true,
		.table[BS_DEFAULT].stat.priority = 0xC,
	},
	[BTS_IDX_DPU] = {
		.name = "dpu",
		.pa_base = EXYNOS7885_PA_DPU,
		.type = BT_TREX,
		.enable = true,
		.table[BS_DEFAULT].stat.scen_en = true,
		.table[BS_DEFAULT].stat.priority = 0xA,
	},
	[BTS_IDX_ABOX] = {
		.name = "abox",
		.pa_base = EXYNOS7885_PA_ABOX,
		.type = BT_TREX,
		.enable = true,
		.table[BS_DEFAULT].stat.scen_en = true,
#ifdef CONFIG_EXYNOS7885_BTS_VM
		.table[BS_DEFAULT].stat.priority = 0x7,
#else
		.table[BS_DEFAULT].stat.priority = 0xA,
#endif
	},
	[BTS_IDX_CPU_DMC0] = {
		.name = "cpu_dmc0",
		.pa_base = EXYNOS7885_PA_CPU_DMC0,
		.type = BT_TREX,
		.enable = true,
		.table[BS_DEFAULT].stat.scen_en = true,
		.table[BS_DEFAULT].stat.priority = 0x4,
	},
	[BTS_IDX_CPU_DMC1] = {
		.name = "cpu_dmc1",
		.pa_base = EXYNOS7885_PA_CPU_DMC1,
		.type = BT_TREX,
		.enable = true,
		.table[BS_DEFAULT].stat.scen_en = true,
		.table[BS_DEFAULT].stat.priority = 0x4,
	},
};

static struct bts_scenario bts_scen[BS_MAX] = {
	[BS_DEFAULT] = {
		.name = "default",
	},
	[BS_MFC_UHD] = {
		.name = "mfc_uhd",
	},
};

static void bts_set_ip_table(struct bts_info *bts)
{
	enum bts_scen_type scen = bts->top_scen;

	/* Scenario state may be recorded before the opt-in DT device probes. */
	if (!bts_hw_ready)
		return;

	BTS_DBG("[BTS] %s bts scen: [%s]->[%s]\n", bts->name,
			bts_scen[scen].name, bts_scen[scen].name);

	switch (bts->type) {
	case BT_TREX:
		bts_setqos(bts->va_base, &bts->table[scen].stat);
		break;
	default:
		break;
	}

	return;
}

static void bts_add_scen(enum bts_scen_type scen)
{
	struct bts_info *first = bts_scen[scen].head;
	struct bts_info *bts = bts_scen[scen].head;
	int next = 0;
	int prev = 0;

	if (!bts)
		return;

	BTS_DBG("[BTS] scen %s on\n", bts_scen[scen].name);

	do {
		if (bts->enable &&!bts->table[scen].next_scen) {
			if (scen >= bts->top_scen) {
				/* insert at top priority */
				bts->table[scen].prev_scen = bts->top_scen;
				bts->table[bts->top_scen].next_scen = scen;
				bts->top_scen = scen;
				bts->table[scen].next_scen = -1;

				bts_set_ip_table(bts);

			} else {
				/* insert at middle */
				for (prev = bts->top_scen; prev > scen;
				     prev = bts->table[prev].prev_scen)
					next = prev;

				bts->table[scen].prev_scen =
					bts->table[next].prev_scen;
				bts->table[scen].next_scen =
					bts->table[prev].next_scen;
				bts->table[next].prev_scen = scen;
				bts->table[prev].next_scen = scen;
			}
		}

		bts = bts->table[scen].next_bts;
	/* set all bts ip in the current scenario */
	} while (bts && bts != first);
}

static void bts_del_scen(enum bts_scen_type scen)
{
	struct bts_info *first = bts_scen[scen].head;
	struct bts_info *bts = bts_scen[scen].head;
	int next = 0;
	int prev = 0;

	if (!bts)
		return;

	BTS_DBG("[BTS] scen %s off\n", bts_scen[scen].name);

	do {
		if (bts->enable && bts->table[scen].next_scen) {
			if (scen == bts->top_scen) {
				/* revert to prev scenario */
				prev = bts->table[scen].prev_scen;
				bts->top_scen = prev;
				bts->table[prev].next_scen = -1;
				bts->table[scen].next_scen = 0;
				bts->table[scen].prev_scen = 0;

				bts_set_ip_table(bts);
			} else if (scen < bts->top_scen) {
				/* delete mid scenario */
				prev = bts->table[scen].prev_scen;
				next = bts->table[scen].next_scen;

				bts->table[next].prev_scen =
					bts->table[scen].prev_scen;
				bts->table[prev].next_scen =
					bts->table[scen].next_scen;

				bts->table[scen].prev_scen = 0;
				bts->table[scen].next_scen = 0;

			} else {
				BTS_DBG("[BTS]%s scenario couldn't \
					exist above top_scen\n",
					bts_scen[scen].name);
			}
		}

		bts = bts->table[scen].next_bts;
	/* revert all bts ip to prev in the current scenario */
	} while (bts && bts != first);
}

void bts_update_scen(enum bts_scen_type scen, unsigned int val)
{
	unsigned long flags;
	bool on = val ? 1 : 0;

	if (scen <= BS_DEFAULT || scen >= BS_MAX)
		return;

	switch (scen) {
	default:
		spin_lock_irqsave(&bts_lock, flags);
		if (on)
			bts_add_scen(scen);
		else
			bts_del_scen(scen);
		spin_unlock_irqrestore(&bts_lock, flags);
		break;
	}
}

static void scen_chaining(enum bts_scen_type scen)
{
	struct bts_info *prev = NULL;
	struct bts_info *first = NULL;
	struct bts_info *bts;

	for (bts = exynos_bts;
	     bts <= &exynos_bts[ARRAY_SIZE(exynos_bts) - 1]; bts++) {
		if (bts->table[scen].stat.scen_en) {
			if (!first)
				first = bts;
			if (prev)
				prev->table[scen].next_bts = bts;

			prev = bts;
		}
	}

	if (prev)
		prev->table[scen].next_bts = first;

	bts_scen[scen].head = first;
}

#define BIT_PER_BYTE		8

/* Vendor bts-exynos7885.c:361-449: preserve scanline-overlap BW model. */
static unsigned int bts_bw_calc(struct bts_decon_info *decon, int idx)
{
	struct bts_dpp_info *dpp = &decon->dpp[idx];
	unsigned int bw;
	unsigned int dst_h;

	dst_h = dpp->dst.y2 - dpp->dst.y1;
	if (!decon->lcd_w || dpp->dst.x2 <= dpp->dst.x1 ||
	    dpp->dst.y2 <= dpp->dst.y1)
		return 0;
	/* use multifactor for KB/s */
	bw = ((u64)dpp->src_h * dpp->src_w * dpp->bpp * decon->vclk) *
	       (decon->lcd_w*11 + 480) / decon->lcd_w / 10 /
		(BIT_PER_BYTE * dst_h * decon->lcd_w);

	return bw;
}

static unsigned int bts_find_max_bw(struct bts_decon_info *decon,
			 const struct bts_layer_position *input, int idx)
{
	struct bts_layer_position output;
	struct bts_dpp_info *dpp;
	unsigned int max = 0;
	int i;

	for (i = idx; i < BTS_DPP_MAX; i++) {
		dpp = &decon->dpp[i];
		if (!dpp->used)
			continue;
		output.y1 = input->y1 < dpp->dst.y1 ? dpp->dst.y1 : input->y1;
		output.y2 = input->y2 > dpp->dst.y2 ? dpp->dst.y2 : input->y2;
		output.x1 = input->x1 < dpp->dst.x1 ? dpp->dst.x1 : input->x1;
		output.x2 = input->x2 > dpp->dst.x2 ? dpp->dst.x2 : input->x2;
		if (output.y1 < output.y2) {
			unsigned int bw;
			bw = dpp->bw + bts_find_max_bw(decon, &output, i + 1);
			if (bw > max)
				max = bw;
		}
	}
	return max;

}

static unsigned int bts_update_decon_bw(struct bts_decon_info *decon)
{
	unsigned int max = 0;
	struct bts_dpp_info *dpp;
	int i;

	for (i = 0; i < BTS_DPP_MAX; i++) {
		dpp = &decon->dpp[i];
		if (!dpp->used)
			continue;
		dpp->bw = bts_bw_calc(decon, i);
	}
	for (i = 0; i < BTS_DPP_MAX; i++) {
		unsigned int bw;
		dpp = &decon->dpp[i];
		if (!dpp->used)
			continue;
		bw = dpp->bw + bts_find_max_bw(decon, &dpp->dst, i + 1);
		if (bw > max)
			max = bw;
	}

	return max;
}

unsigned int bts_calc_bw(enum bts_bw_type type, void *data)
{
	unsigned int bw;

	if (!data)
		return 0;

	switch (type) {
	case BTS_BW_DECON0:
	case BTS_BW_DECON1:
	case BTS_BW_DECON2:
		bw = bts_update_decon_bw(data);
		break;
	default:
		bw = 0;
		break;
	}

	return bw;
}

void bts_update_bw(enum bts_bw_type type, struct bts_bw bw)
{
	unsigned int mif_freq;
	unsigned int int_freq;
	unsigned int total_bw = 0;
	unsigned int bw_r = 0;
	unsigned int bw_w = 0;
	unsigned int int_bw = 0;
	int i;

	if ((unsigned int)type >= BTS_BW_MAX)
		return;
	mutex_lock(&media_mutex);

	ip_bw[type] = bw;
	for (i = 0; i < BTS_BW_MAX; i++) {
		if (int_bw < ip_bw[i].peak)
			int_bw = ip_bw[i].peak;
		bw_r += ip_bw[i].read;
		bw_w += ip_bw[i].write;
	}
	total_bw = bw_r + bw_w;
	if (int_bw < (bw_w / NUM_CHANNEL))
		int_bw = bw_w / NUM_CHANNEL;
	if (int_bw < (bw_r / NUM_CHANNEL))
		int_bw = bw_r / NUM_CHANNEL;

	/* MIF minimum frequency calculation as per BTS guide */
	mif_freq = (u64)total_bw * 100 / BUS_WIDTH /
		max_t(unsigned int, READ_ONCE(exynos_mif_util), 1);
	int_freq = (u64)int_bw * 100 / BUS_WIDTH /
		max_t(unsigned int, READ_ONCE(exynos_int_util), 1);

	requested_mif_freq = mif_freq;
	requested_int_freq = int_freq;

	BTS_DBG("[BTS] BW(KB/s): type%i bw %up %ur %uw, "
		"int %u total %u, read %u, write %u, "
		"freq(Khz): mif %u, int %u\n",
		type, bw.peak, bw.read, bw.write, int_bw, total_bw,
		bw_r, bw_w, mif_freq, int_freq);

	mutex_unlock(&media_mutex);
}

/* Vendor bts-exynos7885.c:499-520: timeout values unchanged. */
static void bts_drex_init(void __iomem *base)
{
	__raw_writel(0x00000000, base + QOS_TIMEOUT_F);
	__raw_writel(0x00000004, base + QOS_TIMEOUT_E);
	__raw_writel(0x00000008, base + QOS_TIMEOUT_D);
	__raw_writel(0x00000010, base + QOS_TIMEOUT_C);
	__raw_writel(0x00000010, base + QOS_TIMEOUT_B);
	__raw_writel(0x00000010, base + QOS_TIMEOUT_A);
	__raw_writel(0x00000020, base + QOS_TIMEOUT_9);
	__raw_writel(0x00000040, base + QOS_TIMEOUT_8);
#ifdef CONFIG_EXYNOS7885_BTS_VM
	__raw_writel(0x00000080, base + QOS_TIMEOUT_7);
#else
	__raw_writel(0x00000100, base + QOS_TIMEOUT_7);
#endif
	__raw_writel(0x00000100, base + QOS_TIMEOUT_6);
	__raw_writel(0x00000100, base + QOS_TIMEOUT_5);
	__raw_writel(0x00000100, base + QOS_TIMEOUT_4);
	__raw_writel(0x00000100, base + QOS_TIMEOUT_3);
	__raw_writel(0x00000100, base + QOS_TIMEOUT_2);
	__raw_writel(0x00000100, base + QOS_TIMEOUT_1);
}

static void bts_initialize_domains(void)
{
	unsigned long flags;
	struct bts_info *bts;

	spin_lock_irqsave(&bts_lock, flags);
	for (bts = exynos_bts;
	     bts <= &exynos_bts[ARRAY_SIZE(exynos_bts) - 1]; bts++) {
		if (!bts->enable)
			continue;
		bts_set_ip_table(bts);
	}
	spin_unlock_irqrestore(&bts_lock, flags);
	return;
}

static void exynos_bts_syscore_resume(void)
{
	bts_drex_init(base_drex0);
	bts_drex_init(base_drex1);
	bts_initialize_domains();
}

static struct syscore_ops exynos_bts_syscore_ops = {
	.resume		= exynos_bts_syscore_resume,
};

#if defined(CONFIG_DEBUG_FS)
static int exynos_qos_status_open_show(struct seq_file *buf, void *d)
{
	unsigned long flags;
	struct bts_info *bts;

	spin_lock_irqsave(&bts_lock, flags);

	for (bts = exynos_bts;
	     bts <= &exynos_bts[ARRAY_SIZE(exynos_bts) - 1]; bts++) {
		if (!bts->enable) {
			seq_printf(buf, "%5s(disabled): ", bts->name);
			continue;
		} else {
			seq_printf(buf, "%5s(%s): ",
					bts->name, bts_scen[bts->top_scen].name);
		}
		switch (bts->type) {
		case BT_TREX:
			bts_showqos(bts->va_base, buf);
			break;
		default:
			seq_puts(buf, "none\n");
		}
	}

	spin_unlock_irqrestore(&bts_lock, flags);

	return 0;
}

static int exynos_timeout_status_open_show(struct seq_file *buf, void *d)
{
	int i;

	seq_puts(buf, "\tqos/timeout\nex)echo 1 100 > timeout\n");
	seq_puts(buf, "DREX0\n");
	for (i = 0; i < 0xf; i++) {
		seq_printf(buf, "[0x%x]: %d\n", i + 1, __raw_readl(base_drex0 +
					QOS_TIMEOUT_1 + 4 * i));
	}

	seq_puts(buf, "DREX1\n");
	for (i = 0; i < 0xf; i++) {
		seq_printf(buf, "[0x%x]: %d\n", i + 1, __raw_readl(base_drex1 +
					QOS_TIMEOUT_1 + 4 * i));
	}

	return 0;
}

static ssize_t exynos_timeout_write(struct file *file, const char __user *user_buf,
					size_t count, loff_t *ppos)
{
	char buf[32];
	int qos, timeout, ret;
	ssize_t len;

	len = simple_write_to_buffer(buf, sizeof(buf) - 1, ppos, user_buf, count);
	if (len < 0)
		return len;

	buf[len] = '\0';

	ret = sscanf(buf, "%d %d\n", &qos, &timeout);
	if (ret != 2) {
		pr_err("%s, Failed at sscanf function: %d\n", __func__, ret);
		return -EINVAL;
	}

	if (qos < 1 || qos > 0xf || timeout < 0) {
		pr_info("Invalid variable\n");
		return len;
	}
	__raw_writel(timeout, base_drex0 + QOS_TIMEOUT_1 + 4 * (qos - 1));
	__raw_writel(timeout, base_drex1 + QOS_TIMEOUT_1 + 4 * (qos - 1));

	return len;
}


static int exynos_mo_status_open_show(struct seq_file *buf, void *d)
{
	unsigned long flags;
	struct bts_info *bts;
	int i, nr_ip = 0;

	seq_puts(buf, "\tIP/Scen/RW/MO\nex)echo 0 0 0 16 > mo\n");
	spin_lock_irqsave(&bts_lock, flags);

	for (bts = exynos_bts;
			bts <= &exynos_bts[ARRAY_SIZE(exynos_bts) - 1]; bts++) {
		if (!bts->enable) {
			seq_printf(buf, "[%2d]IP: %s is disabled\n", nr_ip++, bts->name);
			continue;
		} else {
			seq_printf(buf, "[%2d]IP: %s\n", nr_ip++, bts->name);
		}
		for (i = 0; i < ARRAY_SIZE(bts_scen); i++) {
			if (!bts_scen[i].name)
				continue;
			seq_printf(buf, "%6s: rmo:0x%x wmo:0x%x\n",
					bts_scen[i].name,
					bts->table[i].stat.rmo,
					bts->table[i].stat.wmo);
		}
	}

	spin_unlock_irqrestore(&bts_lock, flags);

	return 0;
}

static ssize_t exynos_mo_write(struct file *file, const char __user *user_buf,
					size_t count, loff_t *ppos)
{
	unsigned long flags;
	struct bts_info *bts = NULL;
	char buf[32];
	int ip, scen, rw, mo, ret;
	ssize_t len;
	int nr_ip = ARRAY_SIZE(exynos_bts) - 1;
	int nr_scen = ARRAY_SIZE(bts_scen) - 1;

	len = simple_write_to_buffer(buf, sizeof(buf) - 1, ppos, user_buf, count);
	if (len < 0)
		return len;

	buf[len] = '\0';

	ret = sscanf(buf, "%d %d %d %d\n", &ip, &scen, &rw, &mo);
	if (ret != 4) {
		pr_err("%s, Failed at sscanf function: %d\n", __func__, ret);
		return -EINVAL;
	}

	if (ip < 0 || ip > nr_ip || scen < 0 ||
			scen > nr_scen || rw < 0 || rw > 1 || mo < 0 || mo > BTS_MAX_MO) {
		pr_info("Invalid variable\n");
		return len;
	}

	bts = &exynos_bts[ip];

	spin_lock_irqsave(&bts_lock, flags);
	if (!rw)
		bts->table[scen].stat.rmo = mo;
	else
		bts->table[scen].stat.wmo = mo;

	if (!bts->table[scen].stat.scen_en) {
		bts->table[scen].stat.scen_en = true;
		scen_chaining(scen);
	}

	if (scen == bts->top_scen)
		bts_setqos(bts->va_base, &bts->table[scen].stat);
	spin_unlock_irqrestore(&bts_lock, flags);

	return len;
}

static int exynos_prio_status_open_show(struct seq_file *buf, void *d)
{
	unsigned long flags;
	struct bts_info *bts;
	int i, nr_ip = 0;

	seq_puts(buf, "\tqos IP/Scen/Prio\nex)echo 0 0 8 > priority\n");
	spin_lock_irqsave(&bts_lock, flags);

	for (bts = exynos_bts;
			bts <= &exynos_bts[ARRAY_SIZE(exynos_bts) - 1]; bts++) {
		if (!bts->enable) {
			seq_printf(buf, "[%2d]IP: %s is disabled\n", nr_ip++, bts->name);
			continue;
		} else {
			seq_printf(buf, "[%2d]IP: %s\n", nr_ip++, bts->name);
		}
		for (i = 0; i < ARRAY_SIZE(bts_scen); i++) {
			if (!bts_scen[i].name)
				continue;
			seq_printf(buf, "%6s: %d\n",
					bts_scen[i].name, bts->table[i].stat.priority);
		}
	}

	spin_unlock_irqrestore(&bts_lock, flags);

	return 0;
}

static ssize_t exynos_prio_write(struct file *file, const char __user *user_buf,
					size_t count, loff_t *ppos)
{
	unsigned long flags;
	struct bts_info *bts = NULL;
	char buf[32];
	int ip, scen, prio, ret;
	ssize_t len;
	int nr_ip = ARRAY_SIZE(exynos_bts) - 1;
	int nr_scen = ARRAY_SIZE(bts_scen) - 1;

	len = simple_write_to_buffer(buf, sizeof(buf) - 1, ppos, user_buf, count);
	if (len < 0)
		return len;

	buf[len] = '\0';

	ret = sscanf(buf, "%d %d %d\n", &ip, &scen, &prio);
	if (ret != 3) {
		pr_err("%s, Failed at sscanf function: %d\n", __func__, ret);
		return -EINVAL;
	}

	if (ip < 0 || ip > nr_ip || scen < 0 ||
			scen > nr_scen || prio < 0 || prio > 0xf) {
		pr_info("Invalid variable\n");
		return len;
	}

	bts = &exynos_bts[ip];

	spin_lock_irqsave(&bts_lock, flags);
	bts->table[scen].stat.priority = prio;

	if (!bts->table[scen].stat.scen_en) {
		bts->table[scen].stat.scen_en = true;
		scen_chaining(scen);
	}

	if (scen == bts->top_scen)
		bts_setqos(bts->va_base, &bts->table[scen].stat);
	spin_unlock_irqrestore(&bts_lock, flags);

	return len;
}

static int exynos_scen_status_open_show(struct seq_file *buf, void *d)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(bts_scen); i++) {
		if (!bts_scen[i].name)
			continue;
		seq_printf(buf, "[%2d]%9s\n", i, bts_scen[i].name);
	}
	return 0;
}

static ssize_t exynos_scen_write(struct file *file,
		const char __user *user_buf, size_t count, loff_t *ppos)
{
	char *buf;
	int ret;
	u32 scen, on;

	if (!count || count > 32)
		return -EINVAL;
	buf = memdup_user_nul(user_buf, count);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	ret = sscanf(buf, "%u %u", &scen, &on);
	if (ret != 2) {
		pr_err("%s, Failed at sscanf function: %d\n", __func__, ret);
		ret = -EINVAL;
		goto out;
	}

	if (scen >= BS_MAX) {
		pr_err("Invalid	variable\n");
		ret = -EINVAL;
		goto out;
	}

	bts_update_scen((enum bts_scen_type)scen, on);
	ret = count;

out:
	kfree(buf);

	return ret;
}

static int exynos_addr_status_open_show(struct seq_file *buf, void *d)
{
	unsigned long flags;
	struct bts_info *bts;

	spin_lock_irqsave(&bts_lock, flags);

	for (bts = exynos_bts;
			bts <= &exynos_bts[ARRAY_SIZE(exynos_bts) - 1]; bts++) {
		if (!bts->enable)
			continue;
		seq_printf(buf, "[IP: %9s]:0x%x \n", bts->name, bts->pa_base);
	}

	spin_unlock_irqrestore(&bts_lock, flags);

	return 0;
}

static int exynos_qos_open(struct inode *inode, struct file *file)
{
	return single_open(file, exynos_qos_status_open_show, inode->i_private);
}

static int exynos_timeout_open(struct inode *inode, struct file *file)
{
	return single_open(file, exynos_timeout_status_open_show, inode->i_private);
}

static int exynos_mo_open(struct inode *inode, struct file *file)
{
	return single_open(file, exynos_mo_status_open_show, inode->i_private);
}

static int exynos_prio_open(struct inode *inode, struct file *file)
{
	return single_open(file, exynos_prio_status_open_show, inode->i_private);
}

static int exynos_scen_open(struct inode *inode, struct file *file)
{
	return single_open(file, exynos_scen_status_open_show, inode->i_private);
}

static int exynos_addr_open(struct inode *inode, struct file *file)
{
	return single_open(file, exynos_addr_status_open_show, inode->i_private);
}

static const struct file_operations debug_qos_status_fops = {
	.open		= exynos_qos_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static const struct file_operations debug_timeout_status_fops = {
	.open		= exynos_timeout_open,
	.read		= seq_read,
	.write		= exynos_timeout_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static const struct file_operations debug_mo_status_fops = {
	.open		= exynos_mo_open,
	.read		= seq_read,
	.write		= exynos_mo_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static const struct file_operations debug_prio_status_fops = {
	.open		= exynos_prio_open,
	.read		= seq_read,
	.write		= exynos_prio_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static const struct file_operations debug_scen_status_fops = {
	.open		= exynos_scen_open,
	.read		= seq_read,
	.write		= exynos_scen_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static const struct file_operations debug_addr_status_fops = {
	.open		= exynos_addr_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static struct dentry *bts_debugfs_root;

static int bts_bandwidth_show(struct seq_file *buf, void *data)
{
	int i;

	mutex_lock(&media_mutex);
	seq_puts(buf, "client peak read write (KB/s)\n");
	for (i = 0; i < BTS_BW_MAX; i++)
		seq_printf(buf, "%d %u %u %u\n", i, ip_bw[i].peak,
			   ip_bw[i].read, ip_bw[i].write);
	seq_printf(buf, "required mif=%u int=%u KHz; DVFS votes unavailable\n",
		   requested_mif_freq, requested_int_freq);
	mutex_unlock(&media_mutex);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(bts_bandwidth);

static void bts_debugfs_hw(void)
{
	struct dentry *den = bts_debugfs_root;

	if (IS_ERR_OR_NULL(den))
		return;
	debugfs_create_file("qos", 0440, den, NULL, &debug_qos_status_fops);
	debugfs_create_file("mo", 0644, den, NULL, &debug_mo_status_fops);
	debugfs_create_file("timeout", 0644, den, NULL, &debug_timeout_status_fops);
	debugfs_create_file("priority", 0644, den, NULL, &debug_prio_status_fops);
	debugfs_create_file("scenario", 0644, den, NULL, &debug_scen_status_fops);
	debugfs_create_file("address", 0440, den, NULL, &debug_addr_status_fops);
}

static void bts_debugfs(void)
{
	struct dentry *den;

	den = debugfs_create_dir("bts", NULL);
	bts_debugfs_root = den;
	if (IS_ERR_OR_NULL(den))
		return;
	debugfs_create_file("bandwidth", 0440, den, NULL, &bts_bandwidth_fops);
	debugfs_create_u32("log", 0644, den, &exynos_bts_log);
	debugfs_create_u32("mif_util", 0644, den, &exynos_mif_util);
	debugfs_create_u32("int_util", 0644, den, &exynos_int_util);
}
#endif

/*
 * The vendor arch_initcall mapped fixed SFRs unconditionally (966-998).
 * Bind only to an explicitly enabled device on 5.10: the board keeps it
 * disabled until the missing frequency/power-management paths are ready.
 * DT resource names match the complete vendor IP table above, plus DREX.
 */
static void __iomem *bts_map_resource(struct platform_device *pdev,
		const char *name, resource_size_t address, resource_size_t size)
{
	struct resource *res;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (!res || res->start != address || resource_size(res) != size)
		return ERR_PTR(-EINVAL);
	return devm_ioremap_resource(&pdev->dev, res);
}

static int exynos_bts_probe(struct platform_device *pdev)
{
	void __iomem *bases[ARRAY_SIZE(exynos_bts)];
	void __iomem *drex0, *drex1;
	unsigned long flags;
	unsigned int i;

	if (bts_hw_ready)
		return -EBUSY;
	for (i = 0; i < ARRAY_SIZE(exynos_bts); i++) {
		bases[i] = bts_map_resource(pdev, exynos_bts[i].name,
				exynos_bts[i].pa_base, SZ_2K);
		if (IS_ERR(bases[i]))
			return PTR_ERR(bases[i]);
	}
	drex0 = bts_map_resource(pdev, "drex0", EXYNOS7885_PA_DREX0, SZ_4K);
	if (IS_ERR(drex0))
		return PTR_ERR(drex0);
	drex1 = bts_map_resource(pdev, "drex1", EXYNOS7885_PA_DREX1, SZ_4K);
	if (IS_ERR(drex1))
		return PTR_ERR(drex1);

	/* Publish mappings atomically with respect to scenario callers. */
	spin_lock_irqsave(&bts_lock, flags);
	for (i = 0; i < ARRAY_SIZE(exynos_bts); i++)
		exynos_bts[i].va_base = bases[i];
	base_drex0 = drex0;
	base_drex1 = drex1;
	bts_hw_ready = true;
	bts_drex_init(base_drex0);
	bts_drex_init(base_drex1);
	spin_unlock_irqrestore(&bts_lock, flags);
	bts_initialize_domains();
	register_syscore_ops(&exynos_bts_syscore_ops);
#ifdef CONFIG_DEBUG_FS
	bts_debugfs_hw();
#endif
	dev_info(&pdev->dev, "BTS register policy active; no MIF/INT DVFS votes\n");
	return 0;
}

static const struct of_device_id exynos_bts_match[] = {
	{ .compatible = "samsung,exynos7885-bts" },
	{ }
};

static struct platform_driver exynos_bts_driver = {
	.probe = exynos_bts_probe,
	.driver = {
		.name = "exynos7885-bts",
		.of_match_table = exynos_bts_match,
		/* Built-in syscore callbacks retain the managed mappings. */
		.suppress_bind_attrs = true,
	},
};

static int __init exynos_bts_init(void)
{
	unsigned int i;

	for (i = BS_DEFAULT + 1; i < BS_MAX; i++)
		scen_chaining(i);
#ifdef CONFIG_DEBUG_FS
	bts_debugfs();
#endif
	return platform_driver_register(&exynos_bts_driver);
}
arch_initcall(exynos_bts_init);

EXPORT_SYMBOL_GPL(bts_update_scen);
EXPORT_SYMBOL_GPL(bts_calc_bw);
EXPORT_SYMBOL_GPL(bts_update_bw);
