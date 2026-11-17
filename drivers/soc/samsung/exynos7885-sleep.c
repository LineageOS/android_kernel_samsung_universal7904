// SPDX-License-Identifier: GPL-2.0
/*
 * Exynos7885 SoC sleep (suspend-to-RAM), derived from Samsung's 4.4
 * exynos-pm.c, exynos-powermode.c and cal-if/pmucal_system.c.
 *
 * The PMU is programmed for SYS_SLEEP (or SYS_SLEEP_AUD_ON while A-Box is
 * running) from a syscore hook, then the last CPU asks the secure firmware
 * for the vendor PSCI "system sleep" state (CPU_SUSPEND, affinity level 3).
 *
 * Copyright (c) 2015 Samsung Electronics Co., Ltd.
 */
#define pr_fmt(fmt) "exynos7885-sleep: " fmt

#include <linux/arm-smccc.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/cpuhotplug.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/psci.h>
#include <linux/regmap.h>
#include <linux/sizes.h>
#include <linux/suspend.h>
#include <linux/syscore_ops.h>
#include <uapi/linux/psci.h>
#include <asm/arch_timer.h>
#include <asm/cputype.h>
#include <asm/smp.h>
#include <asm/smp_plat.h>
#include <asm/suspend.h>

#include "exynos7885-pmucal.h"
#include "exynos7885-sys-data.h"

/* Per-cycle details are only visible with dynamic debug. */
#define m20_log(fmt, ...) pr_debug(fmt, ##__VA_ARGS__)

/* 4.4 psci_power_state_pack(0, 0, 3) for PSCI_SYSTEM_SLEEP. */
#define EXYNOS_PSCI_SYSTEM_SLEEP	(3 << PSCI_0_2_POWER_STATE_AFFL_SHIFT)

/* 4.4 exynos7885.dtsi: exynos-pm and exynos-powermode nodes. */
#define PMU_CONNECT_REQ_STATUS	0x00d4
#define PMU_EINT_WAKEUP_MASK	0x0648
#define GPIO_ALIVE_BASE		0x11cb0000
#define GICD_ISPENDR_BASE	0x12301200
#define NUM_GICD_ISPENDR	16

static const u32 wakeup_mask_offs[] = { 0x064c, 0x0650, 0x0654, 0x0644 };
static const u32 wakeup_stat_offs[] = { 0x0600, 0x0604, 0x0608, 0x0640 };
static const u32 eint_pend_offs[] = { 0x0a00, 0x0a04, 0x0a08 };
static const struct {
	const char *name;
	u32 sys_pwr, status;
} keep_on_doms[] = {
	{ "MFCMSCL", 0x1408, 0x4044 }, { "ISP", 0x140c, 0x4064 },
	{ "CAM", 0x1410, 0x4084 }, { "G3D", 0x1414, 0x40a4 },
};

/*
 * Sleep policy knobs, applied over the 4.4 SYS_SLEEP_AUD_ON sequence:
 *  keep_fsys:  keep the FSYS block powered (4.4 SYS_SLEEP_FSYS_ON). Off, FSYS
 *              and its memory power down like 4.4 SYS_SLEEP and the firmware
 *              re-initializes the CryptoManager (SSS) on wake; with FSYS
 *              kept up the CryptoManager is lost and the lock screen PIN is
 *              rejected after the first sleep.
 *  keep_media: keep MFCMSCL/ISP/CAM powered; their CMU context is not saved.
 *  mif_keep:   apply the 4.4 SICD values for the MIF/APM/TCXO controls.
 */
static bool keep_fsys;
module_param(keep_fsys, bool, 0644);	/* read per cycle in syscore suspend */
static bool keep_media = true;
module_param(keep_media, bool, 0444);
static bool mif_keep = true;
module_param(mif_keep, bool, 0444);
static const struct { u32 off, mask, val; } mif_keep_regs[] = {
	{ 0x1120, (0x1 << 0), (0x1 << 0) },	/* CLKSTOP_CMU_MIF */
	{ 0x1124, (0x1 << 0), (0x1 << 0) },	/* FORCE_AUTOCLKGATE_CMU_MIF */
	{ 0x112C, (0x3 << 0), (0x3 << 0) },	/* RESET_CMU_MIF */
	{ 0x11B0, (0x3 << 0), (0x3 << 0) },	/* LOGIC_RESET_MIF */
	{ 0x11B8, (0x3 << 0), (0x3 << 0) },	/* SLEEP_RESET_MIF */
	{ 0x11E8, (0x1 << 0), (0x1 << 0) },	/* OSCCLK_SLEEP_MIF */
	{ 0x1204, (0x1 << 0), (0x1 << 0) },	/* PAD_RETENTION_MIF */
	{ 0x12C4, (0x1 << 0), (0x1 << 0) },	/* EXT_REGULATOR_MIF */
	{ 0x1304, (0x1 << 0), (0x1 << 0) },	/* GPIO_MODE_MIF */
	{ 0x8084, (0x1 << 0), (0x1 << 0) },	/* FORCE_AUTOCLKGATE_CMU_APM */
	{ 0x8088, (0x7 << 0), (0x7 << 0) },	/* TOP_BUS_APM */
	{ 0x808C, (0x1 << 0), (0x1 << 0) },	/* CLKSTOP_CMU_APM */
	{ 0x8094, (0x1 << 0), (0x1 << 0) },	/* LPI_RESIDUAL_CMU_APM */
	{ 0x80A0, (0x1 << 0), (0x1 << 0) },	/* OSCCLK_GATE_APM */
	{ 0x80A4, (0x3 << 0), (0x3 << 0) },	/* MEMORY_APM_TOP */
	{ 0x80BC, (0x1 << 0), (0x1 << 0) },	/* TCXO */
	{ 0x80C8, (0x1 << 0), (0x1 << 0) },	/* CPU_QCH_CORTEXM0_APM */
	{ 0x2C88, (0x1 << 1), (0x1 << 1) },	/* TOP_BUS_MIF_OPTION EMULATION_BUS */
	{ 0x2C88, (0x1 << 0), (0x1 << 0) },	/* TOP_BUS_MIF_OPTION EMULATION_BUSMASTER */
};
static u32 mif_keep_saved[ARRAY_SIZE(mif_keep_regs)];

struct exynos7885_sleep_mode {
	int id;
	const char *name;
	struct pmucal_seq *enter, *save, *exit, *early;
	unsigned int num_enter, num_save, num_exit, num_early;
	u32 wakeup_mask[ARRAY_SIZE(wakeup_mask_offs)];
};

#define SLEEP_MODE(_id, _name, _sfx, m0, m1, m2, m3)			\
	{								\
		.id = _id, .name = _name,				\
		.enter = exynos7885_enter_##_sfx,			\
		.save = exynos7885_save_##_sfx,				\
		.exit = exynos7885_exit_##_sfx,				\
		.early = exynos7885_earlywkup_##_sfx,			\
		.num_enter = ARRAY_SIZE(exynos7885_enter_##_sfx),	\
		.num_save = ARRAY_SIZE(exynos7885_save_##_sfx),		\
		.num_exit = ARRAY_SIZE(exynos7885_exit_##_sfx),		\
		.num_early = ARRAY_SIZE(exynos7885_earlywkup_##_sfx),	\
		.wakeup_mask = { m0, m1, m2, m3 },			\
	}

/* sys_powerdown indices and wakeup-masks columns from 4.4; v262 masks
 * USB_REWA (WAKEUP_MASK bit 15): 5.10 gets a spurious REWA wake on every
 * sleep, which multiplies cycles and the rare firmware-entry hang. */
static struct exynos7885_sleep_mode sleep_modes[] = {
	SLEEP_MODE(8, "SLEEP", sleep,
		   0x500ffe7e, 0xffff02ff, 0xffff00ff, 0x0),
	SLEEP_MODE(10, "SLEEP_AUD_ON", sleep_aud_on,
		   0x500ffe7e, 0xffff02ff, 0xffff00ff, 0x0),
	SLEEP_MODE(12, "SLEEP_FSYS_ON", sleep_fsys_on,
		   0x500ffe7e, 0xffff00ff, 0xffff00ff, 0x0),
};

/* 0 = automatic (4.4 policy), otherwise a sys_powerdown index above. */
static int force_mode;
module_param(force_mode, int, 0644);
MODULE_PARM_DESC(force_mode, "0=auto, 8=SLEEP, 10=SLEEP_AUD_ON, 12=SLEEP_FSYS_ON");

struct exynos7885_sleep {
	struct device *dev;
	struct regmap *pmu;
	struct exynos7885_pmucal cal;
	void __iomem *eint_base;
	void __iomem *gic_pend;
	void __iomem *fsys_cmu;
	struct exynos7885_sleep_mode *mode;
	bool prepared;
	bool early_wakeup;
	u32 eint_mask;
	u32 fsys_mem_cfg, ppc_fsys;
};

static struct exynos7885_sleep *sleep_data;
u32 exynos_get_eint_wake_mask(void);
#if IS_ENABLED(CONFIG_SND_SOC_SAMSUNG_ABOX)
bool abox_is_on(void);
#else
static inline bool abox_is_on(void) { return false; }
#endif

static struct exynos7885_sleep_mode *find_mode(int id)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(sleep_modes); i++)
		if (sleep_modes[i].id == id)
			return &sleep_modes[i];
	return NULL;
}

static u32 pmu_read(u32 off)
{
	u32 val = 0;

	exynos7885_pmucal_pmu_read(&sleep_data->cal, off, &val);
	return val;
}

static struct exynos7885_sleep_mode *pick_mode(void)
{
	struct exynos7885_sleep_mode *mode = NULL;

	if (force_mode)
		mode = find_mode(force_mode);
	if (mode)
		return mode;
	/* Match 4.4 policy: domain power alone is not an active A-Box vote. */
	/*
	 * 4.4: a running A-Box selects the cp_call (SLEEP_AUD_ON) mode, which
	 * keeps DISPAUD powered. 5.10 has no DISPAUD context restore either
	 * (plain SLEEP with DISPAUD still on never came back), so use it
	 * whenever the domain is powered.
	 */
	if (abox_is_on() || (pmu_read(0x4024) & 0xf) == 0xf)
		return find_mode(10);
	return find_mode(8);
}


static void show_wakeup_reason(bool early)
{
	struct exynos7885_sleep *s = sleep_data;
	unsigned int i;

	if (early) {
		m20_log("early wakeup, pending registers:\n");
		for (i = 0; i < ARRAY_SIZE(eint_pend_offs); i++)
			m20_log("EINT_PEND[%u] = 0x%02x\n", i,
				readl(s->eint_base + eint_pend_offs[i]));
		for (i = 0; i < NUM_GICD_ISPENDR; i++) {
			u32 v = readl(s->gic_pend + i * 4);

			if (v)
				m20_log("GICD_ISPENDR[%u] = 0x%08x\n",
					i, v);
		}
		return;
	}

	for (i = 0; i < ARRAY_SIZE(wakeup_stat_offs); i++)
		m20_log("WAKEUP_STAT 0x%03x = 0x%08x\n",
			wakeup_stat_offs[i], pmu_read(wakeup_stat_offs[i]));
	for (i = 0; i < ARRAY_SIZE(eint_pend_offs); i++)
		m20_log("EINT_PEND[%u] = 0x%02x\n", i,
			readl(s->eint_base + eint_pend_offs[i]));
}

/*
 * 4.4 registers the FSYS IP gates (MMC_CARD/EMBD/SDIO, USB30DRD_*) as HWACG
 * clocks with no Q-channel control (clk-exynos7885.c, composite.c): it never
 * clears CLOCK_REQ in QCH_CON, so at sleep entry these keep the boot value
 * (ENABLE=0, REQ=1). The 5.10 clock driver clears REQ when a gate is
 * disabled. Raise REQ again after the PMUCAL save, so that the FSYS can
 * power down; the save list restores the 5.10 values on wake.
 */
static bool fsys_qch_req = true;
module_param(fsys_qch_req, bool, 0644);
MODULE_PARM_DESC(fsys_qch_req, "Before sleep, set CLOCK_REQ on the FSYS MMC/USB Q-channels as 4.4 leaves them");

static const u16 fsys_qch_44_offs[] = {
	0x3018, 0x301c, 0x3020,			/* MMC_CARD, MMC_EMBD, MMC_SDIO */
	0x3034, 0x3038, 0x303c, 0x3040,		/* USB30DRD HSDRD/USB20/USB30_0/1 */
};

static void exynos7885_fsys_qch_44(struct exynos7885_sleep *s)
{
	u32 before[ARRAY_SIZE(fsys_qch_44_offs)];
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(fsys_qch_44_offs); i++)
		before[i] = readl(s->fsys_cmu + fsys_qch_44_offs[i]);
	if (!fsys_qch_req)
		return;
	for (i = 0; i < ARRAY_SIZE(fsys_qch_44_offs); i++) {
		/* Same rule as ra_req_enable_qch(): REQ only matters if ENABLE=0. */
		if (!(before[i] & BIT(0)))
			writel(before[i] | BIT(1), s->fsys_cmu + fsys_qch_44_offs[i]);
	}
}

static int exynos7885_sleep_syscore_suspend(void)
{
	struct exynos7885_sleep *s = sleep_data;
	struct exynos7885_sleep_mode *mode = pick_mode();
	unsigned int i;
	int ret;

	s->prepared = false;
	s->early_wakeup = false;
	s->mode = mode;

	/* exynos_set_wakeupmask() */
	s->eint_mask = exynos_get_eint_wake_mask();
	regmap_write(s->pmu, PMU_EINT_WAKEUP_MASK, s->eint_mask);
	for (i = 0; i < ARRAY_SIZE(wakeup_mask_offs); i++)
		regmap_write(s->pmu, wakeup_mask_offs[i],
			     mode->wakeup_mask[i]);

	/* pmucal_system_enter() */
	ret = exynos7885_pmucal_save(&s->cal, mode->save, mode->num_save);
	if (ret) {
		m20_log("%s save failed: %d\n", mode->name, ret);
		return ret;
	}
	ret = exynos7885_pmucal_run(&s->cal, mode->enter, mode->num_enter);
	if (ret) {
		m20_log("%s enter sequence failed: %d\n",
		       mode->name, ret);
		exynos7885_pmucal_run(&s->cal, mode->early, mode->num_early);
		exynos7885_pmucal_restore(&s->cal, mode->save, mode->num_save);
		return ret;
	}

	/*
	 * 5.10 keeps MFCMSCL/ISP/CAM/G3D powered (genpd ALWAYS_ON or bound CMU
	 * providers) and has no context restore for them: keep each powered
	 * domain on through sleep (SYS_PWR = 0xF), as AUD_ON does for DISPAUD.
	 */
	for (i = 0; i < ARRAY_SIZE(keep_on_doms); i++) {
		if ((pmu_read(keep_on_doms[i].status) & 0xf) != 0xf)
			continue;
		/*
		 * v224: like 4.4, let the PMU power MFCMSCL/ISP/CAM down in
		 * sleep (their CMU context is now a conditional save entry);
		 * powered media domains hanging off a MIF that goes down are
		 * the leading suspect for the MIF-down wake hang.
		 */
		if (!keep_media && i < 3) {
			m20_log("let PMU power %s down in sleep\n",
				keep_on_doms[i].name);
			continue;
		}
		regmap_update_bits(s->pmu, keep_on_doms[i].sys_pwr, 0xf, 0xf);
		m20_log("keep %s on (SYS_PWR 0x%04x)\n", keep_on_doms[i].name,
			keep_on_doms[i].sys_pwr);
	}

	/*
	 * 4.4 SYS_SLEEP_FSYS_ON: keep FSYS (USB, MMC) powered. 5.10 dwc3 does
	 * not survive FSYS power loss (USB never re-enumerates after resume).
	 */
	s->fsys_mem_cfg = pmu_read(0x4ac0);
	s->ppc_fsys = pmu_read(0x0dcc);
	if (keep_fsys) {
		/* v259: exact v243 semantics (update_bits may skip SYS_PWR). */
		regmap_update_bits(s->pmu, 0x1418, 0xf, 0xf);
		regmap_update_bits(s->pmu, 0x4ac0, 0x3, 0x0);
		regmap_update_bits(s->pmu, 0x0dcc, BIT(20), BIT(20));
		m20_log("keep FSYS on (SLEEP_FSYS_ON settings)\n");
	}
	exynos7885_fsys_qch_44(s);
	if (mif_keep) {
		for (i = 0; i < ARRAY_SIZE(mif_keep_regs); i++) {
			mif_keep_saved[i] = pmu_read(mif_keep_regs[i].off);
			regmap_update_bits(s->pmu, mif_keep_regs[i].off,
					   mif_keep_regs[i].mask,
					   mif_keep_regs[i].val);
		}
		m20_log("keep MIF/APM/TCXO up (SICD values)\n");
	}

	s->prepared = true;
	m20_log("enter %s (mode %d), eint wake mask 0x%08x\n",
		mode->name, mode->id, s->eint_mask);
	return 0;
}

static void exynos7885_sleep_syscore_resume(void)
{
	struct exynos7885_sleep *s = sleep_data;
	struct exynos7885_sleep_mode *mode = s->mode;
	int ret;

	if (!s->prepared)
		return;
	s->prepared = false;

	/* pmucal_system_exit() / pmucal_system_earlywakeup() */
	if (s->early_wakeup)
		ret = exynos7885_pmucal_run(&s->cal, mode->early,
					    mode->num_early);
	else
		ret = exynos7885_pmucal_run(&s->cal, mode->exit,
					    mode->num_exit);
	if (ret)
		m20_log("%s %s sequence failed: %d\n", mode->name,
		       s->early_wakeup ? "early-wakeup" : "exit", ret);
	ret = exynos7885_pmucal_restore(&s->cal, mode->save, mode->num_save);
	if (ret)
		m20_log("%s restore failed: %d\n", mode->name, ret);

	if (mif_keep) {
		unsigned int k;

		for (k = 0; k < ARRAY_SIZE(mif_keep_regs); k++)
			regmap_update_bits(s->pmu, mif_keep_regs[k].off,
					   mif_keep_regs[k].mask,
					   mif_keep_saved[k]);
	}

	/*
	 * MEMORY_FSYS_CONFIGURATION and PPC_FSYS act while running too, and the
	 * early-wakeup sequence does not reset them: leaving the FSYS_ON values
	 * broke eMMC (bus width switch failures, garbage reads) after resume.
	 */
	regmap_write(s->pmu, 0x4ac0, s->fsys_mem_cfg);
	regmap_write(s->pmu, 0x0dcc, s->ppc_fsys);
	m20_log("restored MEMORY_FSYS_CONFIG 0x%x PPC_FSYS 0x%x\n",
		pmu_read(0x4ac0), pmu_read(0x0dcc));
	show_wakeup_reason(s->early_wakeup);
}

static struct syscore_ops exynos7885_sleep_syscore_ops = {
	.suspend = exynos7885_sleep_syscore_suspend,
	.resume = exynos7885_sleep_syscore_resume,
};

static int exynos7885_sleep_finisher(unsigned long arg)
{
	return psci_ops.cpu_suspend(EXYNOS_PSCI_SYSTEM_SLEEP,
				    __pa_symbol(cpu_resume));
}

static int exynos7885_sleep_enter(suspend_state_t state)
{
	struct exynos7885_sleep *s = sleep_data;
	int ret;

	if (!s->prepared) {
		m20_log("PMU not prepared, not entering sleep\n");
		return -EIO;
	}

	/* Returns here on wakeup, or early with an error from firmware. */
	ret = cpu_suspend(0, exynos7885_sleep_finisher);
	s->early_wakeup = ret != 0;
	if (s->early_wakeup)
		m20_log("return to originator (%d)\n", ret);

	return 0;
}

static const struct platform_suspend_ops exynos7885_sleep_ops = {
	.valid = suspend_valid_only_mem,
	.enter = exynos7885_sleep_enter,
};

static int exynos7885_sleep_map(struct exynos7885_sleep *s,
				struct pmucal_seq *seq, unsigned int n,
				const char *what)
{
	int ret = exynos7885_pmucal_map(&s->cal, seq, n);

	if (ret)
		dev_err(s->dev, "cannot map %s sequence: %d\n", what, ret);
	return ret;
}

/*
 * 4.4 exynos-powermode.c hotcpu notifier: the PMU only powers a core down
 * at WFI when its CONFIGURATION is 0, and the central sleep sequencer waits
 * for every core. PSCI CPU_OFF alone leaves offline cores powered.
 */
struct exynos7885_pmucal_pair {
	struct pmucal_seq *on, *off;
	unsigned int num_on, num_off;
};

#define PMU_PAIR(n) { exynos7885_##n##_on, exynos7885_##n##_off, \
		      ARRAY_SIZE(exynos7885_##n##_on), \
		      ARRAY_SIZE(exynos7885_##n##_off) }

/* Index: 4.4 linear_phycpu() = cluster0 (2), cluster1 (4), cluster2 (2). */
static struct exynos7885_pmucal_pair cpu_seqs[] = {
	PMU_PAIR(core00), PMU_PAIR(core01), PMU_PAIR(core10), PMU_PAIR(core11),
	PMU_PAIR(core12), PMU_PAIR(core13), PMU_PAIR(core20), PMU_PAIR(core21),
};

static struct exynos7885_pmucal_pair cluster_seqs[] = {
	PMU_PAIR(cluster0), PMU_PAIR(cluster1), PMU_PAIR(cluster2),
};

static const unsigned int cluster_first_phycpu[] = { 0, 2, 6 };

static int cpu_phys(unsigned int cpu, unsigned int *cluster)
{
	u64 mpidr = cpu_logical_map(cpu);
	unsigned int cl = MPIDR_AFFINITY_LEVEL(mpidr, 1);
	unsigned int core = MPIDR_AFFINITY_LEVEL(mpidr, 0);

	if (cl >= ARRAY_SIZE(cluster_seqs))
		return -EINVAL;
	*cluster = cl;
	return cluster_first_phycpu[cl] + core;
}

static bool cluster_has_online_cpu(unsigned int cluster, unsigned int self)
{
	unsigned int cpu, cl;

	for_each_online_cpu(cpu)
		if (cpu != self && cpu_phys(cpu, &cl) >= 0 && cl == cluster)
			return true;
	return false;
}

static int exynos7885_cpu_starting(unsigned int cpu)
{
	struct exynos7885_sleep *s = sleep_data;
	struct exynos7885_pmucal_pair *p;
	unsigned int cluster;
	int phy = cpu_phys(cpu, &cluster);

	if (phy < 0 || phy >= ARRAY_SIZE(cpu_seqs))
		return 0;
	if (!cluster_has_online_cpu(cluster, cpu)) {
		p = &cluster_seqs[cluster];
		exynos7885_pmucal_run(&s->cal, p->on, p->num_on);
	}
	p = &cpu_seqs[phy];
	exynos7885_pmucal_run(&s->cal, p->on, p->num_on);
	return 0;
}

static int exynos7885_cpu_dying(unsigned int cpu)
{
	struct exynos7885_sleep *s = sleep_data;
	struct exynos7885_pmucal_pair *p;
	unsigned int cluster;
	int phy = cpu_phys(cpu, &cluster);

	if (phy < 0 || phy >= ARRAY_SIZE(cpu_seqs))
		return 0;
	p = &cpu_seqs[phy];
	exynos7885_pmucal_run(&s->cal, p->off, p->num_off);
	if (!cluster_has_online_cpu(cluster, cpu)) {
		p = &cluster_seqs[cluster];
		exynos7885_pmucal_run(&s->cal, p->off, p->num_off);
	}
	return 0;
}

bool exynos7885_lpm_ready;
EXPORT_SYMBOL_GPL(exynos7885_lpm_ready);

static int exynos7885_sleep_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct exynos7885_sleep *s;
	unsigned int i;
	int ret;

	if (!psci_ops.cpu_suspend) {
		dev_err(dev, "PSCI CPU_SUSPEND not available\n");
		return -ENODEV;
	}

	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->dev = dev;
	s->pmu = syscon_regmap_lookup_by_compatible("samsung,exynos7885-pmu");
	if (IS_ERR(s->pmu))
		return dev_err_probe(dev, PTR_ERR(s->pmu), "no PMU regmap\n");
	s->eint_base = devm_ioremap(dev, GPIO_ALIVE_BASE, SZ_4K);
	s->gic_pend = devm_ioremap(dev, GICD_ISPENDR_BASE,
				   NUM_GICD_ISPENDR * 4);
	if (!s->eint_base || !s->gic_pend)
		return -ENOMEM;
	/* FSYS CMU, for the Q-channel requests before sleep. */
	s->fsys_cmu = devm_ioremap(dev, 0x13400000, SZ_16K);
	if (!s->fsys_cmu)
		return -ENOMEM;

	ret = exynos7885_pmucal_init(&s->cal, dev, s->pmu);
	if (ret)
		return ret;

	ret = exynos7885_sleep_map(s, exynos7885_lpm_init,
				   ARRAY_SIZE(exynos7885_lpm_init), "lpm_init");
	for (i = 0; !ret && i < ARRAY_SIZE(cpu_seqs); i++)
		ret = exynos7885_sleep_map(s, cpu_seqs[i].on, cpu_seqs[i].num_on,
					   "cpu on") ?:
		      exynos7885_sleep_map(s, cpu_seqs[i].off,
					   cpu_seqs[i].num_off, "cpu off");
	for (i = 0; !ret && i < ARRAY_SIZE(cluster_seqs); i++)
		ret = exynos7885_sleep_map(s, cluster_seqs[i].on,
					   cluster_seqs[i].num_on, "cluster on") ?:
		      exynos7885_sleep_map(s, cluster_seqs[i].off,
					   cluster_seqs[i].num_off, "cluster off");
	for (i = 0; !ret && i < ARRAY_SIZE(sleep_modes); i++) {
		struct exynos7885_sleep_mode *m = &sleep_modes[i];

		ret = exynos7885_sleep_map(s, m->enter, m->num_enter, m->name) ?:
		      exynos7885_sleep_map(s, m->save, m->num_save, m->name) ?:
		      exynos7885_sleep_map(s, m->exit, m->num_exit, m->name) ?:
		      exynos7885_sleep_map(s, m->early, m->num_early, m->name);
	}
	if (ret)
		return ret;

	/* pmucal_system_init(): run the common low-power init once. */
	ret = exynos7885_pmucal_run(&s->cal, exynos7885_lpm_init,
				    ARRAY_SIZE(exynos7885_lpm_init));
	if (ret) {
		dev_err(dev, "lpm_init sequence failed: %d\n", ret);
		return ret;
	}

	sleep_data = s;
	ret = cpuhp_setup_state_nocalls(CPUHP_AP_EXYNOS7885_CPU_STARTING,
					"soc/exynos7885-sleep:starting",
					exynos7885_cpu_starting,
					exynos7885_cpu_dying);
	if (ret) {
		dev_err(dev, "cpuhp setup failed: %d\n", ret);
		sleep_data = NULL;
		return ret;
	}
	/* Published after lpm_init and the CPU PMU hooks: gates cpuidle C2. */
	smp_store_release(&exynos7885_lpm_ready, true);
	if (!IS_ENABLED(CONFIG_EXYNOS7885_SLEEP)) {
		dev_info(dev, "lpm_init done, suspend stays s2idle\n");
		return 0;
	}
	register_syscore_ops(&exynos7885_sleep_syscore_ops);
	suspend_set_ops(&exynos7885_sleep_ops);
	dev_info(dev, "SoC sleep enabled (PSCI state 0x%08x)\n",
		 EXYNOS_PSCI_SYSTEM_SLEEP);
	return 0;
}

static struct platform_driver exynos7885_sleep_driver = {
	.probe = exynos7885_sleep_probe,
	.driver = {
		.name = "exynos7885-sleep",
		.suppress_bind_attrs = true,
	},
};

static int __init exynos7885_sleep_init(void)
{
	struct platform_device *pdev;
	int ret;

	if (!of_machine_is_compatible("samsung,exynos7904") &&
	    !of_machine_is_compatible("samsung,exynos7885"))
		return 0;

	ret = platform_driver_register(&exynos7885_sleep_driver);
	if (ret)
		return ret;
	pdev = platform_device_register_simple("exynos7885-sleep", -1,
					       NULL, 0);
	return PTR_ERR_OR_ZERO(pdev);
}
late_initcall(exynos7885_sleep_init);
