// SPDX-License-Identifier: GPL-2.0-only
/*
 * Exynos7885/7904 common clocks.
 *
 * The vendor 4.4 clk-exynos7885.c uses CAL virtual clocks, not CCF register
 * tables. The register offsets, field widths and parent order below come from
 * drivers/soc/samsung/cal-if/exynos7885/cmucal-{node,sfr,qch}.c. Gate control
 * follows ra_set_gate() and ra_req_enable_qch() in cal-if/ra.c.
 *
 * Unimplemented vendor IDs deliberately return -ENOENT. Shared PLLs, muxes
 * and most dividers are read-only:
 * retain firmware rates until coordinated rate changes and power management
 * have been ported. The optional, powered CMU_G3D provider below also supports
 * its dedicated PLL. No CAL/ACPM firmware interface is assumed here.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/syscore_ops.h>
#include <dt-bindings/clock/exynos7885.h>

#include "clk.h"

#define GATE_MANUAL		BIT(20)
#define GATE_VALUE		BIT(21)
#define GATE_AUTO		BIT(28)
#define QCH_ENABLE		BIT(0)
#define QCH_REQUEST		BIT(1)

/* Read-only shared PLLs: 1417X has the same M/P/S fields as PLL35xx. */
struct exynos7885_pll {
	struct clk_hw hw;
	void __iomem *reg;
	void __iomem *k_reg; /* PLL1431X fractional coefficient, otherwise NULL */
};

static unsigned long exynos7885_pll_recalc_rate(struct clk_hw *hw,
					     unsigned long parent_rate)
{
	struct exynos7885_pll *pll = container_of(hw, struct exynos7885_pll, hw);
	u32 con = readl_relaxed(pll->reg);
	u32 m = FIELD_GET(GENMASK(25, 16), con);
	u32 p = FIELD_GET(GENMASK(13, 8), con);
	u32 s = FIELD_GET(GENMASK(2, 0), con);
	u64 rate = (u64)parent_rate * m;

	/* A PLL left unconfigured by firmware must not divide by zero. */
	if (!p || !(con & BIT(31)))
		return 0;

	if (pll->k_reg) {
		s32 k = (s16)readl_relaxed(pll->k_reg);

		rate = (u64)parent_rate * ((s64)m * 65536 + k);
		do_div(rate, p << s);
		return rate >> 16;
	}
	do_div(rate, p << s);
	return rate;
}

static const struct clk_ops exynos7885_pll_ops = {
	.recalc_rate = exynos7885_pll_recalc_rate,
};

/*
 * PLL_USB (FSYS CMU, PLL_CON0 at 0x01a0, lock time at 0x0000), CAL type
 * PLL_1418X. The USB PHY asks for exactly 50 MHz (phy-exynos-usbdrd3.c); the
 * only entry in the 4.4 pll_usb_rate_table is M=400 P=13 S=4 from the 26 MHz
 * oscillator. Sequence copied from cal-if/ra.c ra_enable_pll()/ra_pll_set_pmsk().
 */
#define PLL_MUX_SEL		BIT(4)
#define PLL_MUX_BUSY		BIT(7)
#define PLL_STABLE		BIT(29)
#define PLL_ENABLE		BIT(31)
#define PLL_LOCK_TIME		150

struct exynos7885_usb_pll {
	struct clk_hw hw;
	void __iomem *con0;
	void __iomem *lock;
	spinlock_t *slock;
};

#define to_usb_pll(_hw)	container_of(_hw, struct exynos7885_usb_pll, hw)

static int exynos7885_pll_wait(void __iomem *reg, u32 bit, bool set)
{
	int usec = 100;

	do {
		if (!!(readl_relaxed(reg) & bit) == set)
			return 0;
		udelay(1);
	} while (--usec > 0);

	return -ETIMEDOUT;
}

static void exynos7885_usb_pll_off(struct exynos7885_usb_pll *pll)
{
	u32 reg = readl_relaxed(pll->con0);

	writel_relaxed(reg & ~PLL_MUX_SEL, pll->con0);
	if (exynos7885_pll_wait(pll->con0, PLL_MUX_BUSY, false))
		pr_err("exynos7885: PLL_USB mux change timed out\n");
	writel_relaxed(readl_relaxed(pll->con0) & ~PLL_ENABLE, pll->con0);
}

#define PLL_USB_MPS_MASK	(GENMASK(25, 16) | GENMASK(13, 8) | GENMASK(2, 0))
#define PLL_USB_MPS	((400 << 16) | (13 << 8) | 4)

/* Program 50 MHz, enable, wait for lock, select the PLL. Lock held. */
static int exynos7885_usb_pll_program(struct exynos7885_usb_pll *pll)
{
	u32 con0;

	exynos7885_usb_pll_off(pll);
	writel_relaxed(13 * PLL_LOCK_TIME, pll->lock);
	con0 = readl_relaxed(pll->con0);
	con0 &= ~PLL_USB_MPS_MASK;
	con0 |= PLL_USB_MPS;
	con0 |= PLL_MUX_SEL | PLL_ENABLE;
	writel_relaxed(con0, pll->con0);
	return exynos7885_pll_wait(pll->con0, PLL_STABLE, true);
}

static unsigned long exynos7885_usb_pll_recalc_rate(struct clk_hw *hw,
						    unsigned long parent_rate)
{
	u32 con = readl_relaxed(to_usb_pll(hw)->con0);
	u32 m = FIELD_GET(GENMASK(25, 16), con);
	u32 p = FIELD_GET(GENMASK(13, 8), con);
	u32 s = FIELD_GET(GENMASK(2, 0), con);
	u64 rate = (u64)parent_rate * m;

	if (!p || !(con & PLL_ENABLE))
		return 0;

	do_div(rate, p << s);
	return rate;
}

static long exynos7885_usb_pll_round_rate(struct clk_hw *hw, unsigned long rate,
					  unsigned long *parent_rate)
{
	return 50000000;
}

static int exynos7885_usb_pll_set_rate(struct clk_hw *hw, unsigned long rate,
				       unsigned long parent_rate)
{
	struct exynos7885_usb_pll *pll = to_usb_pll(hw);
	unsigned long flags;
	int ret;

	if (rate != 50000000 || parent_rate != 26000000)
		return -EINVAL;

	spin_lock_irqsave(pll->slock, flags);
	ret = exynos7885_usb_pll_program(pll);
	spin_unlock_irqrestore(pll->slock, flags);
	if (ret)
		pr_err("exynos7885: PLL_USB did not lock\n");

	return ret;
}

static int exynos7885_usb_pll_enable(struct clk_hw *hw)
{
	struct exynos7885_usb_pll *pll = to_usb_pll(hw);
	unsigned long flags;
	int ret;

	spin_lock_irqsave(pll->slock, flags);
	/*
	 * SoC sleep resets the PLL factors and nothing in the 4.4 save lists
	 * covers PLL_USB (its USB driver reprogrammed it on every enable).
	 */
	if ((readl_relaxed(pll->con0) & PLL_USB_MPS_MASK) != PLL_USB_MPS) {
		ret = exynos7885_usb_pll_program(pll);
		spin_unlock_irqrestore(pll->slock, flags);
		pr_warn("exynos7885: PLL_USB reprogrammed on enable: %d\n", ret);
		return ret;
	}
	writel_relaxed(readl_relaxed(pll->con0) | PLL_ENABLE, pll->con0);
	ret = exynos7885_pll_wait(pll->con0, PLL_STABLE, true);
	/*
	 * ra_enable_pll() switches the output mux to the PLL once it is
	 * stable. recalc_rate already reads 50 MHz from M/P/S, so the clk core
	 * skips set_rate() and this is the only place the mux gets selected;
	 * without it the USB PHY runs from the 26 MHz oscillator.
	 */
	if (!ret) {
		writel_relaxed(readl_relaxed(pll->con0) | PLL_MUX_SEL, pll->con0);
		if (exynos7885_pll_wait(pll->con0, PLL_MUX_BUSY, false))
			pr_err("exynos7885: PLL_USB mux change timed out\n");
	}
	spin_unlock_irqrestore(pll->slock, flags);

	return ret;
}

static void exynos7885_usb_pll_disable(struct clk_hw *hw)
{
	struct exynos7885_usb_pll *pll = to_usb_pll(hw);
	unsigned long flags;

	spin_lock_irqsave(pll->slock, flags);
	exynos7885_usb_pll_off(pll);
	spin_unlock_irqrestore(pll->slock, flags);
}

static int exynos7885_usb_pll_is_enabled(struct clk_hw *hw)
{
	return !!(readl_relaxed(to_usb_pll(hw)->con0) & PLL_ENABLE);
}

static const struct clk_ops exynos7885_usb_pll_ops = {
	.recalc_rate = exynos7885_usb_pll_recalc_rate,
	.round_rate = exynos7885_usb_pll_round_rate,
	.set_rate = exynos7885_usb_pll_set_rate,
	.enable = exynos7885_usb_pll_enable,
	.disable = exynos7885_usb_pll_disable,
	.is_enabled = exynos7885_usb_pll_is_enabled,
};

static struct exynos7885_usb_pll *exynos7885_usb_pll_dev;

/* PLL_USB left enabled across SoC sleep: bring it back before USB resumes. */
static void exynos7885_usb_pll_syscore_resume(void)
{
	struct exynos7885_usb_pll *pll = exynos7885_usb_pll_dev;
	u32 con0;
	int ret;

	if (!pll || !__clk_get_enable_count(pll->hw.clk))
		return;
	con0 = readl_relaxed(pll->con0);
	if ((con0 & PLL_USB_MPS_MASK) == PLL_USB_MPS &&
	    (con0 & (PLL_ENABLE | PLL_STABLE | PLL_MUX_SEL)) ==
	    (PLL_ENABLE | PLL_STABLE | PLL_MUX_SEL))
		return;
	ret = exynos7885_usb_pll_program(pll);
	pr_warn("exynos7885: PLL_USB con0 %#x after sleep, reprogrammed: %d\n",
		con0, ret);
}

static struct syscore_ops exynos7885_usb_pll_syscore_ops = {
	.resume = exynos7885_usb_pll_syscore_resume,
};

static void __init exynos7885_register_usb_pll(struct samsung_clk_provider *ctx,
					       void __iomem *fsys)
{
	struct exynos7885_usb_pll *pll;
	const char *parent = "fin_pll";
	struct clk_init_data init = {
		.name = "pll_usb",
		.ops = &exynos7885_usb_pll_ops,
		.parent_names = &parent,
		.num_parents = 1,
		.flags = CLK_GET_RATE_NOCACHE | CLK_IGNORE_UNUSED,
	};
	int ret;

	pll = kzalloc(sizeof(*pll), GFP_KERNEL);
	if (!pll)
		panic("exynos7885: cannot allocate PLL_USB\n");
	pll->con0 = fsys + 0x01a0;
	pll->lock = fsys + 0x0000;
	pll->slock = &ctx->lock;
	pll->hw.init = &init;
	ret = clk_hw_register(NULL, &pll->hw);
	if (ret)
		panic("exynos7885: cannot register PLL_USB: %d\n", ret);
	samsung_clk_add_lookup(ctx, &pll->hw, PLL_OUT_USB);
	exynos7885_usb_pll_dev = pll;
	register_syscore_ops(&exynos7885_usb_pll_syscore_ops);
}

static void __init exynos7885_register_pll(struct samsung_clk_provider *ctx,
		void __iomem *base, const char *name, unsigned int offset,
		unsigned int k_offset, unsigned int id)
{
	struct exynos7885_pll *pll;
	const char *parent = "fin_pll";
	struct clk_init_data init = {
		.name = name,
		.ops = &exynos7885_pll_ops,
		.parent_names = &parent,
		.num_parents = 1,
		.flags = CLK_GET_RATE_NOCACHE,
	};
	int ret;

	pll = kzalloc(sizeof(*pll), GFP_KERNEL);
	if (!pll)
		panic("exynos7885: cannot allocate PLL %s\n", name);
	pll->reg = base + offset;
	if (k_offset)
		pll->k_reg = base + k_offset;
	pll->hw.init = &init;
	ret = clk_hw_register(NULL, &pll->hw);
	if (ret)
		panic("exynos7885: cannot register PLL %s: %d\n", name, ret);
	if (id != NONE)
		samsung_clk_add_lookup(ctx, &pll->hw, id);
}

/* A CAL Q-channel can cover multiple physical clock gates (UART PCLK/UCLK). */
struct exynos7885_gate {
	struct clk_hw hw;
	void __iomem *reg;
	void __iomem *companion;
	void __iomem *qch;
	spinlock_t *lock;
};

struct exynos7885_gate_clock {
	unsigned int id;
	const char *name;
	const char *parent;
	u16 offset;
	u16 companion;
	u16 qch;
	unsigned long flags;
};

static void exynos7885_gate_set(void __iomem *reg, bool enable)
{
	u32 val;

	/* QCH-only gates (HWACG vclk in 4.4) have no manual gate register. */
	if (!reg)
		return;

	val = readl_relaxed(reg);

	/* Preserve firmware's manual/automatic policy, exactly as CAL does. */
	if (val & GATE_MANUAL) {
		val &= ~GATE_VALUE;
		if (enable)
			val |= GATE_VALUE;
	} else {
		val &= ~GATE_AUTO;
		if (!enable)
			val |= GATE_AUTO;
	}
	writel_relaxed(val, reg);
}

static void exynos7885_gate_request(struct exynos7885_gate *gate, bool enable)
{
	u32 val;

	if (!gate->qch)
		return;
	val = readl_relaxed(gate->qch);
	/* When hardware Q-channel control is enabled, CAL leaves REQ alone. */
	if (val & QCH_ENABLE)
		return;
	val &= ~QCH_REQUEST;
	if (enable)
		val |= QCH_REQUEST;
	writel_relaxed(val, gate->qch);
}

static int exynos7885_gate_enable(struct clk_hw *hw)
{
	struct exynos7885_gate *gate = container_of(hw, struct exynos7885_gate, hw);
	unsigned long flags;

	spin_lock_irqsave(gate->lock, flags);
	exynos7885_gate_set(gate->reg, true);
	if (gate->companion)
		exynos7885_gate_set(gate->companion, true);
	exynos7885_gate_request(gate, true);
	spin_unlock_irqrestore(gate->lock, flags);
	return 0;
}

static void exynos7885_gate_disable(struct clk_hw *hw)
{
	struct exynos7885_gate *gate = container_of(hw, struct exynos7885_gate, hw);
	unsigned long flags;

	spin_lock_irqsave(gate->lock, flags);
	exynos7885_gate_request(gate, false);
	if (gate->companion)
		exynos7885_gate_set(gate->companion, false);
	exynos7885_gate_set(gate->reg, false);
	spin_unlock_irqrestore(gate->lock, flags);
}

static int exynos7885_gate_is_enabled(struct clk_hw *hw)
{
	struct exynos7885_gate *gate = container_of(hw, struct exynos7885_gate, hw);
	u32 val;

	if (!gate->reg)
		return !!(readl_relaxed(gate->qch) & QCH_REQUEST);

	val = readl_relaxed(gate->reg);

	return val & GATE_MANUAL ? !!(val & GATE_VALUE) : !(val & GATE_AUTO);
}

static const struct clk_ops exynos7885_gate_ops = {
	.enable = exynos7885_gate_enable,
	.disable = exynos7885_gate_disable,
	.is_enabled = exynos7885_gate_is_enabled,
};

static void __init exynos7885_register_gates(struct samsung_clk_provider *ctx,
			const struct exynos7885_gate_clock *clks, unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		const struct exynos7885_gate_clock *clk = &clks[i];
		struct clk_init_data init = {
			.name = clk->name,
			.ops = &exynos7885_gate_ops,
			.parent_names = &clk->parent,
			.num_parents = 1,
			/* Do not gate firmware-owned clocks during late init. */
			.flags = clk->flags | CLK_IGNORE_UNUSED,
		};
		struct exynos7885_gate *gate;
		int ret;

		gate = kzalloc(sizeof(*gate), GFP_KERNEL);
		if (!gate)
			panic("exynos7885: cannot allocate gate %s\n", clk->name);
		if (clk->offset)
			gate->reg = ctx->reg_base + clk->offset;
		if (clk->companion)
			gate->companion = ctx->reg_base + clk->companion;
		if (clk->qch)
			gate->qch = ctx->reg_base + clk->qch;
		gate->lock = &ctx->lock;
		gate->hw.init = &init;
		ret = clk_hw_register(NULL, &gate->hw);
		if (ret)
			panic("exynos7885: cannot register gate %s: %d\n",
			      clk->name, ret);
		samsung_clk_add_lookup(ctx, &gate->hw, clk->id);
	}
}

/*
 * Q-channel-only gate: 4.4 registers GATE_SPEEDY_APM as HWACG_VCLK with
 * SPEEDY_QCH and no manual gate. The APM bus parent is firmware-owned and is
 * not modelled here, so the clock has no parent.
 */
static void __init exynos7885_register_qch_gate(struct samsung_clk_provider *ctx,
			void __iomem *qch, unsigned int id, const char *name,
			const char *parent)
{
	struct clk_init_data init = {
		.name = name,
		.ops = &exynos7885_gate_ops,
		.parent_names = parent ? &parent : NULL,
		.num_parents = parent ? 1 : 0,
		.flags = CLK_IGNORE_UNUSED,
	};
	struct exynos7885_gate *gate;
	int ret;

	gate = kzalloc(sizeof(*gate), GFP_KERNEL);
	if (!gate)
		panic("exynos7885: cannot allocate gate %s\n", name);
	gate->qch = qch;
	gate->lock = &ctx->lock;
	gate->hw.init = &init;
	ret = clk_hw_register(NULL, &gate->hw);
	if (ret)
		panic("exynos7885: cannot register gate %s: %d\n", name, ret);
	samsung_clk_add_lookup(ctx, &gate->hw, id);
}

static const struct samsung_fixed_rate_clock exynos7885_fixed_clks[] __initconst = {
	FRATE(OSCCLK, "fin_pll", NULL, 0, 26000000),
};

PNAME(peri_bus_p) = { "pll_shared0_div4", "pll_shared1_div4" };
PNAME(fsys_p) = { "pll_shared0_div2", "pll_shared1_div2" };
PNAME(uart0_p) = { "fin_pll", "pll_shared0_div4" };
PNAME(spi1_p) = { "fin_pll", "pll_shared0_div4" };
PNAME(spi1_user_p) = { "fin_pll", "dout_peri_spi1" };
PNAME(peri_bus_user_p) = { "fin_pll", "dout_peri_bus" };
PNAME(uart0_user_p) = { "fin_pll", "dout_peri_uart0" };
PNAME(fsys_bus_user_p) = { "fin_pll", "dout_fsys_bus" };
PNAME(mmc_user_p) = { "fin_pll", "dout_fsys_mmc_embd" };
PNAME(mmc_card_user_p) = { "fin_pll", "dout_fsys_mmc_card" };
PNAME(camera_bus_p) = { "pll_shared0_div3", "pll_shared1_div3",
			"pll_shared0_div4", "pll_shared0_div5" };
PNAME(isp_bus_p) = { "pll_shared0_div3", "pll_shared1_div3",
		     "pll_shared0_div4", "mout_isp_lowfreq" };
PNAME(isp_func_p) = { "pll_shared0_div3", "pll_shared1_div3",
		      "pll_shared0_div4", "pll_shared1_div4" };
PNAME(isp_lowfreq_p) = { "pll_shared1_div4", "pll_shared0_div5" };
PNAME(mfcmscl_p) = { "pll_shared0_div3", "pll_shared1_div4" };
PNAME(dispaud_p) = { "pll_shared0_div2", "pll_shared1_div2" };
PNAME(dispaud_bus_user_p) = { "fin_pll", "dout_dispaud_bus" };
PNAME(aud_cpu_user_p) = { "fin_pll", "dout_dispaud_cpu" };
PNAME(aud_cpu_p) = { "dout_aud_pll", "mout_aud_cpu_user" };
PNAME(aud_cpu_hch_p) = { "mout_aud_cpu", "fin_pll" };
PNAME(aud_uaif0_p) = { "dout_aud_uaif0", "ioclk_audiocdclk0" };
PNAME(aud_uaif2_p) = { "dout_aud_uaif2", "ioclk_audiocdclk2" };
PNAME(aud_uaif3_p) = { "dout_aud_uaif3", "ioclk_audiocdclk3" };

#define RO_MUX(id, name, parents, offset, shift) \
	MUX_F(id, name, parents, offset, shift, 1, CLK_GET_RATE_NOCACHE, \
	      CLK_MUX_READ_ONLY)
#define RO_DIV(id, name, parent, offset, width) \
	DIV_F(id, name, parent, offset, 0, width, CLK_GET_RATE_NOCACHE, \
	      CLK_DIVIDER_READ_ONLY)

/* CMU at 0x12060000: CMUCAL parent order and DIVRATIO widths unchanged. */
static const struct samsung_mux_clock exynos7885_top_muxes[] __initconst = {
	MUX_F(NONE, "mout_cam_bus", camera_bus_p, 0x1004, 0, 2,
	      CLK_GET_RATE_NOCACHE, CLK_MUX_READ_ONLY),
	MUX_F(NONE, "mout_isp_bus", isp_bus_p, 0x103c, 0, 2,
	      CLK_GET_RATE_NOCACHE, CLK_MUX_READ_ONLY),
	MUX_F(NONE, "mout_isp_gdc", isp_func_p, 0x1040, 0, 2,
	      CLK_GET_RATE_NOCACHE, CLK_MUX_READ_ONLY),
	MUX_F(NONE, "mout_isp_vra", isp_func_p, 0x1044, 0, 2,
	      CLK_GET_RATE_NOCACHE, CLK_MUX_READ_ONLY),
	RO_MUX(NONE, "mout_isp_lowfreq", isp_lowfreq_p, 0x1080, 0),
	RO_MUX(NONE, "mout_mfcmscl_mscl", mfcmscl_p, 0x104c, 0),
	RO_MUX(NONE, "mout_mfcmscl_mfc", mfcmscl_p, 0x1048, 0),
	RO_MUX(NONE, "mout_cis0", uart0_p, 0x1008, 0),
	RO_MUX(NONE, "mout_cis1", uart0_p, 0x100c, 0),
	RO_MUX(NONE, "mout_cis2", uart0_p, 0x1010, 0),
	RO_MUX(NONE, "mout_dispaud_bus", dispaud_p, 0x1020, 0),
	RO_MUX(NONE, "mout_dispaud_cpu", dispaud_p, 0x1024, 0),
	RO_MUX(NONE, "mout_fsys_bus", fsys_p, 0x1028, 0),
	RO_MUX(NONE, "mout_fsys_mmc_card", fsys_p, 0x102c, 0),
	RO_MUX(NONE, "mout_fsys_mmc_embd", fsys_p, 0x1030, 0),
	RO_MUX(NONE, "mout_peri_bus", peri_bus_p, 0x1058, 0),
	RO_MUX(NONE, "mout_peri_uart0", uart0_p, 0x1064, 0),
	RO_MUX(NONE, "mout_peri_spi1", spi1_p, 0x1060, 0),
};

static const struct samsung_div_clock exynos7885_top_divs[] __initconst = {
	/* CMUCAL CLKCMU_G3D_SWITCH: no BUSD divider exists in CMU_G3D. */
	RO_DIV(DOUT_G3D_SWITCH, "dout_g3d_switch", "gate_g3d_switch", 0x1858, 3),
	RO_DIV(NONE, "pll_shared0_div3", "pll_shared0", 0x18a0, 2),
	RO_DIV(NONE, "pll_shared0_div5", "pll_shared0", 0x18a8, 3),
	RO_DIV(NONE, "pll_shared1_div3", "pll_shared1", 0x18b0, 2),
	RO_DIV(NONE, "dout_cam_bus", "gate_cam_bus", 0x180c, 3),
	RO_DIV(NONE, "dout_isp_bus", "gate_isp_bus", 0x185c, 3),
	RO_DIV(NONE, "dout_isp_gdc", "gate_isp_gdc", 0x1860, 3),
	RO_DIV(NONE, "dout_isp_vra", "gate_isp_vra", 0x1864, 3),
	RO_DIV(NONE, "dout_mfcmscl_mscl", "gate_mfcmscl_mscl", 0x186c, 3),
	RO_DIV(NONE, "dout_mfcmscl_mfc", "gate_mfcmscl_mfc", 0x1868, 3),
	/* TODO(5.10 port): coordinated CIS rate changes; keep firmware rates. */
	RO_DIV(CIS_CLK0, "dout_cis0", "gate_cis0", 0x1810, 5),
	RO_DIV(CIS_CLK1, "dout_cis1", "gate_cis1", 0x1814, 5),
	RO_DIV(CIS_CLK2, "dout_cis2", "gate_cis2", 0x1818, 5),
	RO_DIV(NONE, "pll_shared0_div2", "pll_shared0", 0x189c, 1),
	RO_DIV(NONE, "pll_shared0_div4", "pll_shared0_div2", 0x18a4, 1),
	RO_DIV(NONE, "pll_shared1_div2", "pll_shared1", 0x18ac, 1),
	RO_DIV(NONE, "pll_shared1_div4", "pll_shared1_div2", 0x18b4, 1),
	RO_DIV(NONE, "dout_dispaud_bus", "gate_dispaud_bus", 0x183c, 4),
	RO_DIV(NONE, "dout_dispaud_cpu", "gate_dispaud_cpu", 0x1840, 3),
	RO_DIV(NONE, "dout_fsys_bus", "gate_fsys_bus", 0x1844, 4),
	/* Writable: microSD SDR50/SDR104 need 400/800 MHz CIU input. */
	DIV_F(MMC_CARD, "dout_fsys_mmc_card", "gate_fsys_mmc_card", 0x1848, 0, 9,
	      CLK_GET_RATE_NOCACHE, 0),
	/* Writable: the eMMC host selects 100/200/400/800 MHz per timing. */
	DIV_F(MMC_EMBD, "dout_fsys_mmc_embd", "gate_fsys_mmc_embd", 0x184c, 0, 9,
	      CLK_GET_RATE_NOCACHE, 0),
	RO_DIV(NONE, "dout_peri_bus", "gate_peri_bus", 0x1874, 4),
	RO_DIV(UART_0, "dout_peri_uart0", "gate_peri_uart0", 0x1880, 4),
	/* Dedicated SPI1 divider; its shared PLL parents stay read-only. */
	DIV_F(SPI_1, "dout_peri_spi1", "gate_peri_spi1", 0x187c, 0, 6,
	      CLK_GET_RATE_NOCACHE, 0),
};

static const struct exynos7885_gate_clock exynos7885_top_gates[] __initconst = {
	{ NONE, "gate_g3d_switch", "pll_shared0_div2", 0x2058 },
	{ NONE, "gate_cam_bus", "mout_cam_bus", 0x200c },
	{ NONE, "gate_isp_bus", "mout_isp_bus", 0x205c },
	{ NONE, "gate_isp_gdc", "mout_isp_gdc", 0x2060 },
	{ NONE, "gate_isp_vra", "mout_isp_vra", 0x2064 },
	{ NONE, "gate_mfcmscl_mscl", "mout_mfcmscl_mscl", 0x206c },
	{ NONE, "gate_mfcmscl_mfc", "mout_mfcmscl_mfc", 0x2068 },
	{ NONE, "gate_cis0", "mout_cis0", 0x2010 },
	{ NONE, "gate_cis1", "mout_cis1", 0x2014 },
	{ NONE, "gate_cis2", "mout_cis2", 0x2018 },
	{ NONE, "gate_dispaud_bus", "mout_dispaud_bus", 0x203c },
	{ NONE, "gate_dispaud_cpu", "mout_dispaud_cpu", 0x2040 },
	{ NONE, "gate_fsys_bus", "mout_fsys_bus", 0x2044 },
	{ NONE, "gate_fsys_mmc_card", "mout_fsys_mmc_card", 0x2048 },
	{ NONE, "gate_fsys_mmc_embd", "mout_fsys_mmc_embd", 0x204c },
	{ NONE, "gate_peri_bus", "mout_peri_bus", 0x207c },
	{ NONE, "gate_peri_uart0", "mout_peri_uart0", 0x2088 },
	{ NONE, "gate_peri_spi1", "mout_peri_spi1", 0x2084 },
};

/* PERI at 0x10010000: user mux select is bit 4, not bit 0. */
static const struct samsung_mux_clock exynos7885_peri_muxes[] __initconst = {
	RO_MUX(UMUX_CLKCMU_PERI_BUS, "mout_peri_bus_user", peri_bus_user_p, 0x0100, 4),
	RO_MUX(UMUX_CLKCMU_PERI_UART_0, "mout_peri_uart0_user", uart0_user_p, 0x0160, 4),
	/* Allow selecting the dedicated divider instead of a fixed oscillator. */
	{ .id = UMUX_CLKCMU_PERI_SPI1, .name = "mout_peri_spi1_user",
	  .parent_names = spi1_user_p, .num_parents = ARRAY_SIZE(spi1_user_p),
	  .flags = CLK_GET_RATE_NOCACHE | CLK_SET_RATE_PARENT,
	  .offset = 0x0140, .shift = 4, .width = 1 },
};

static const struct exynos7885_gate_clock exynos7885_peri_gates[] __initconst = {
	/* 4.4 cal-if/exynos7885/cmucal-sfr.c:918-919,974-975. */
	/* MCT PCLK + QCH: 4.4 cmucal-sfr.c:910 (0x20a0), :1000 (0x306c). */
	{ GATE_MCT, "gate_mct", "mout_peri_bus_user", 0x20a0, 0, 0x306c,
	  CLK_IS_CRITICAL },
	{ GATE_I2C0, "gate_i2c0", "mout_peri_bus_user", 0x2038, 0, 0x3018 },
	{ GATE_I2C1, "gate_i2c1", "mout_peri_bus_user", 0x203c, 0, 0x301c },
	/* M20 TFA9896 bus: cmucal-sfr.c I2C3 PCLK/QCH. */
	{ GATE_I2C3, "gate_i2c3", "mout_peri_bus_user", 0x2044, 0, 0x3024 },
	/* I2C4 (backlight IC bus): cmucal-sfr.c:922,978. */
	{ GATE_I2C4, "gate_i2c4", "mout_peri_bus_user", 0x2048, 0, 0x3028 },
	/* I2C5/I2C6 (sensors): cmucal-sfr.c:917,923,979,980. */
	{ GATE_I2C5, "gate_i2c5", "mout_peri_bus_user", 0x204c, 0, 0x302c },
	{ GATE_I2C6, "gate_i2c6", "mout_peri_bus_user", 0x2050, 0, 0x3030 },
	/* Cluster watchdogs: 4.4 cmucal-sfr.c:908-909 (PCLK), :1005-1006 (QCH). */
	{ GATE_WDT_CLUSTER0, "gate_wdt_cl0", "mout_peri_bus_user", 0x20b4, 0, 0x3080 },
	{ GATE_WDT_CLUSTER1, "gate_wdt_cl1", "mout_peri_bus_user", 0x20b8, 0, 0x3084 },
	/* GATE_UART0 in CAL covers PCLK and EXT_UCLK under the same Q-channel. */
	{ GATE_UART0, "gate_uart0", "mout_peri_bus_user", 0x2070, 0x206c, 0x3044 },
	/* SPI1 PCLK, external clock and shared Q-channel, 4.4 CAL. */
	{ GATE_SPI1, "gate_spi1", "mout_peri_spi1_user", 0x2064, 0x2068, 0x3040 },
	/*
	 * USI0 CH0/CH1 (I2C4 camera PMIC / I2C5 touch) share HSI2C0.
	 * 4.4 cmucal-sfr.c:935 (PCLK 0x2028), :987 (QCH 0x3008).
	 */
	{ GATE_HSI2C0, "gate_hsi2c0", "mout_peri_bus_user", 0x2028, 0, 0x3008 },
	/* Dedicated camera HSI2C channels: 4.4 cmucal-sfr.c:933-938 (PCLK), :988-990 (QCH). */
	{ GATE_HSI2C1, "gate_hsi2c1", "mout_peri_bus_user", 0x202c, 0, 0x300c },
	{ GATE_HSI2C2, "gate_hsi2c2", "mout_peri_bus_user", 0x2030, 0, 0x3010 },
	{ GATE_HSI2C3, "gate_hsi2c3", "mout_peri_bus_user", 0x2034, 0, 0x3014 },
	/* No clock hook in samsung-pinctrl; keep the GPIO register bus live. */
	{ NONE, "gate_gpio_top", "mout_peri_bus_user", 0x2024, 0, 0x3004,
	  CLK_IS_CRITICAL },
};

/* FSYS at 0x13400000. Both MMC clocks use the single MMC_EMBD Q-channel. */
static const struct samsung_mux_clock exynos7885_fsys_muxes[] __initconst = {
	RO_MUX(UMUX_CLKCMU_FSYS_BUS, "mout_fsys_bus_user", fsys_bus_user_p, 0x0100, 4),
	MUX_F(UMUX_CLKCMU_FSYS_MMC_CARD, "mout_fsys_mmc_card_user", mmc_card_user_p,
	      0x0120, 4, 1, CLK_GET_RATE_NOCACHE | CLK_SET_RATE_PARENT,
	      CLK_MUX_READ_ONLY),
	MUX_F(UMUX_CLKCMU_FSYS_MMC_EMBD, "mout_fsys_mmc_embd_user", mmc_user_p,
	      0x0140, 4, 1, CLK_GET_RATE_NOCACHE | CLK_SET_RATE_PARENT,
	      CLK_MUX_READ_ONLY),
};

static const struct exynos7885_gate_clock exynos7885_fsys_gates[] __initconst = {
	/* Keep the vendor compound gate: I_ACLK and SDCLKIN must run together. */
	/* microSD slot: 4.4 cmucal-sfr.c:661-662 (ACLK/SDCLKIN), :699 (QCH). */
	{ GATE_MMC_CARD, "gate_mmc_card", "mout_fsys_mmc_card_user",
	  0x2034, 0x2030, 0x3018 },
	{ GATE_MMC_EMBD, "gate_mmc_embd", "mout_fsys_mmc_embd_user",
	  0x203c, 0x2038, 0x301c },
	{ NONE, "gate_gpio_fsys", "mout_fsys_bus_user", 0x2024, 0, 0x300c,
	  CLK_IS_CRITICAL },
};

/* DISPAUD at 0x14800000: user mux select is bit 4 (4.4 PLL_CON0_MUX_CLKCMU_DISPAUD_BUS_USER). */
static const struct samsung_mux_clock exynos7885_dispaud_muxes[] __initconst = {
	RO_MUX(UMUX_CLKCMU_DISPAUD_BUS, "mout_dispaud_bus_user", dispaud_bus_user_p,
	       0x0120, 4),
	RO_MUX(NONE, "mout_aud_cpu_user", aud_cpu_user_p, 0x0100, 4),
	RO_MUX(NONE, "mout_aud_cpu", aud_cpu_p, 0x1000, 0),
	RO_MUX(NONE, "mout_aud_cpu_hch", aud_cpu_hch_p, 0x1004, 0),
	RO_MUX(NONE, "mout_aud_uaif0", aud_uaif0_p, 0x100c, 0),
	RO_MUX(NONE, "mout_aud_uaif2", aud_uaif2_p, 0x1010, 0),
	RO_MUX(NONE, "mout_aud_uaif3", aud_uaif3_p, 0x1014, 0),
};

/* TODO(5.10 port): AUD PLL/mux/divider rate changes and external BCLK
 * selection need coordinated CAL/ACPM policy. Read firmware rates accurately;
 * do not pretend a fixed-rate oscillator is an operational audio PLL.
 */
static const struct samsung_div_clock exynos7885_aud_divs[] __initconst = {
	RO_DIV(NONE, "dout_aud_pll", "pll_aud", 0x1818, 4),
	RO_DIV(NONE, "dout_aud_bus", "pll_aud", 0x1804, 3),
	/*
	 * ABOX programs AUDIF and the UAIF BCLK dividers per stream, as 4.4
	 * did through cal_clk_setrate(); left read-only they stay at /1 and
	 * the speaker amp sees a 1.18 GHz "BCLK" (no clock).
	 */
	DIV_F(DOUT_CLK_ABOX_AUDIF, "dout_aud_audif", "pll_aud", 0x1800, 0, 9,
	      CLK_GET_RATE_NOCACHE, CLK_DIVIDER_ROUND_CLOSEST),
	DIV_F(DOUT_CLK_ABOX_UAIF0, "dout_aud_uaif0", "dout_aud_audif", 0x181c, 0, 5,
	      CLK_GET_RATE_NOCACHE, CLK_DIVIDER_ROUND_CLOSEST),
	DIV_F(DOUT_CLK_ABOX_UAIF2, "dout_aud_uaif2", "dout_aud_audif", 0x1820, 0, 5,
	      CLK_GET_RATE_NOCACHE, CLK_DIVIDER_ROUND_CLOSEST),
	DIV_F(DOUT_CLK_ABOX_UAIF3, "dout_aud_uaif3", "dout_aud_audif", 0x1824, 0, 5,
	      CLK_GET_RATE_NOCACHE, CLK_DIVIDER_ROUND_CLOSEST),
};

static const struct exynos7885_gate_clock exynos7885_aud_gates[] __initconst = {
	/* Same QCH-only HWACG gates as Samsung's 4.4 clock provider. */
	{ GATE_ABOX_ABOX, "gate_abox_abox", "mout_aud_cpu_hch", 0, 0, 0x3000 },
	{ GATE_ABOX_CPU, "gate_abox_cpu", "mout_aud_cpu_hch", 0, 0, 0x3004 },
	{ GATE_ABOX_BUS, "gate_abox_bus", "dout_aud_bus", 0, 0, 0x300c },
	{ GATE_UAIF0, "gate_uaif0", "mout_aud_uaif0", 0, 0, 0x3010 },
	{ GATE_UAIF2, "gate_uaif2", "mout_aud_uaif2", 0, 0, 0x3014 },
	{ GATE_UAIF3, "gate_uaif3", "mout_aud_uaif3", 0, 0, 0x3018 },
	{ GATE_SMMU_ABOX, "gate_smmu_abox", "mout_dispaud_bus_user", 0, 0, 0x3044 },
	{ NONE, "gate_gpio_dispaud", "mout_dispaud_bus_user", 0x2080, 0, 0x302c,
	  CLK_IS_CRITICAL },
};

/* Legacy clock names requested by the Trustonic ET520 ABI. Route SCLK
 * through the user mux so its rate request selects the dedicated divider.
 */
static const struct samsung_clock_alias exynos7885_fp_aliases[] __initconst = {
	ALIAS(GATE_SPI1, NULL, "fp-spi-pclk"),
	ALIAS(UMUX_CLKCMU_PERI_SPI1, NULL, "fp-spi-sclk"),
};

static void __init exynos7885_clk_init(struct device_node *np)
{
	struct samsung_clk_provider *ctx;
	void __iomem *top, *peri, *fsys, *apm, *dispaud, *core;

	/* Resource bases/sizes are the CMUCAL SFR_BLOCK entries, not guesses. */
	top = of_iomap(np, 0);
	peri = of_iomap(np, 1);
	fsys = of_iomap(np, 2);
	apm = of_iomap(np, 3);
	dispaud = of_iomap(np, 4);
	core = of_iomap(np, 5);
	if (!top || !peri || !fsys || !apm || !dispaud || !core)
		panic("exynos7885: cannot map CMU/PERI/FSYS/APM/DISPAUD/CORE registers\n");

	ctx = samsung_clk_init(np, top, CLK_NR_CLKS);
	samsung_clk_register_fixed_rate(ctx, exynos7885_fixed_clks,
				       ARRAY_SIZE(exynos7885_fixed_clks));
	exynos7885_register_pll(ctx, top, "pll_shared0", 0x0100, 0, NONE);
	exynos7885_register_pll(ctx, top, "pll_shared1", 0x0120, 0, NONE);
	samsung_clk_register_mux(ctx, exynos7885_top_muxes,
				 ARRAY_SIZE(exynos7885_top_muxes));
	samsung_clk_register_div(ctx, exynos7885_top_divs,
				 ARRAY_SIZE(exynos7885_top_divs));
	exynos7885_register_gates(ctx, exynos7885_top_gates,
				 ARRAY_SIZE(exynos7885_top_gates));

	/* Registration captures each base; all runtime clocks share ctx->lock. */
	ctx->reg_base = peri;
	samsung_clk_register_mux(ctx, exynos7885_peri_muxes,
				 ARRAY_SIZE(exynos7885_peri_muxes));
	exynos7885_register_gates(ctx, exynos7885_peri_gates,
				 ARRAY_SIZE(exynos7885_peri_gates));
	ctx->reg_base = fsys;
	samsung_clk_register_mux(ctx, exynos7885_fsys_muxes,
				 ARRAY_SIZE(exynos7885_fsys_muxes));
	exynos7885_register_gates(ctx, exynos7885_fsys_gates,
				 ARRAY_SIZE(exynos7885_fsys_gates));
	/*
	 * USB30DRD Q-channel gates (HWACG_VCLK, parent UMUX_CLKCMU_FSYS_BUS in
	 * 4.4 clk-exynos7885.c:99-102); QCH offsets from cmucal-sfr.c:707-710.
	 */
	exynos7885_register_qch_gate(ctx, fsys + 0x3034, GATE_USB30DRD_HSDRD,
				     "gate_usb30drd_hsdrd", "mout_fsys_bus_user");
	exynos7885_register_qch_gate(ctx, fsys + 0x3038, GATE_USB30DRD_USB20,
				     "gate_usb30drd_usb20", "mout_fsys_bus_user");
	exynos7885_register_qch_gate(ctx, fsys + 0x303c, GATE_USB30DRD_USB30_0,
				     "gate_usb30drd_usb30_0", "mout_fsys_bus_user");
	exynos7885_register_qch_gate(ctx, fsys + 0x3040, GATE_USB30DRD_USB30_1,
				     "gate_usb30drd_usb30_1", "mout_fsys_bus_user");
	exynos7885_register_usb_pll(ctx, fsys);
	ctx->reg_base = top;

	/*
	 * DISPAUD block: DPU Q-channel gates (HWACG_VCLK, no manual gate) with
	 * parent UMUX_CLKCMU_DISPAUD_BUS. QCH offsets from 4.4 cmucal-sfr.c:
	 * DPP 0x3028, DMA 0x3024, DECON0 0x3020, SMMU_DPU 0x3048.
	 */
	ctx->reg_base = dispaud;
	exynos7885_register_pll(ctx, dispaud, "pll_aud", 0x0160, 0x016c, PLL_OUT_AUD);
	samsung_clk_register_div(ctx, exynos7885_aud_divs,
				 ARRAY_SIZE(exynos7885_aud_divs));
	samsung_clk_register_mux(ctx, exynos7885_dispaud_muxes,
				 ARRAY_SIZE(exynos7885_dispaud_muxes));
	exynos7885_register_gates(ctx, exynos7885_aud_gates,
				 ARRAY_SIZE(exynos7885_aud_gates));
	exynos7885_register_qch_gate(ctx, dispaud + 0x3028, GATE_DPU_DPP,
				     "gate_dpu_dpp", "mout_dispaud_bus_user");
	exynos7885_register_qch_gate(ctx, dispaud + 0x3024, GATE_DPU_DMA,
				     "gate_dpu_dma", "mout_dispaud_bus_user");
	exynos7885_register_qch_gate(ctx, dispaud + 0x3020, GATE_DPU_DECON0,
				     "gate_dpu_decon0", "mout_dispaud_bus_user");
	exynos7885_register_qch_gate(ctx, dispaud + 0x3048, GATE_SMMU_DPU,
				     "gate_smmu_dpu", "mout_dispaud_bus_user");
	ctx->reg_base = top;

	/* APM block (0x11C00000): SPEEDY_QCH at 0x304c, as CAL cmucal-sfr.c. */
	exynos7885_register_qch_gate(ctx, apm + 0x304c, GATE_SPEEDY_APM,
				     "gate_speedy_apm", NULL);

	/* 4.4 clk-exynos7885.c:134-135 uses Q-channel-only ADC wrapper gates.
	 * CORE base and QCH offsets: cmucal-sfr.c:11,429-430. */
	if (core) {
		exynos7885_register_qch_gate(ctx, core + 0x30f0,
			GATE_WRAP_ADC_IF_0, "gate_adcif0", NULL);
		exynos7885_register_qch_gate(ctx, core + 0x30f4,
			GATE_WRAP_ADC_IF_1, "gate_adcif1", NULL);
	}

	samsung_clk_register_alias(ctx, exynos7885_fp_aliases,
				   ARRAY_SIZE(exynos7885_fp_aliases));
	samsung_clk_of_add_provider(np, ctx);
	pr_info("Exynos7885: UART0/MMC clocks, firmware rates retained\n");
}
CLK_OF_DECLARE(exynos7885_clk, "samsung,exynos7885-clock", exynos7885_clk_init);

/*
 * CMU_G3D is in a switchable power domain. Register it as a platform driver,
 * after genpd has powered the block, not from CLK_OF_DECLARE before genpd.
 * With runtime PM disabled the bound provider keeps the domain active.
 * TODO(5.10 port): coordinated CMU context save/restore and runtime PM.
 *
 * All offsets/fields below are from exynos7885/cmucal-sfr.c:711-736,
 * 3008-3109. Parent order and PLL table: cmucal-node.c. PLL1417X lock factor
 * and programming sequence: cal-if/pll_spec.c and ra.c, not PLL35xx.
 */
#define G3D_PLL_CON0		0x0120
#define G3D_PLL_LOCK		0x0000
#define G3D_SWITCH_CON0		0x0100
#define G3D_BUSD_MUX		0x1004
#define G3D_BUSP_DIV		0x1800
#define G3D_PLL_PMS		(GENMASK(25, 16) | GENMASK(13, 8) | GENMASK(2, 0))

struct exynos7885_g3d_pll_rate {
	unsigned long rate;
	u16 m;
	u8 p, s;
};

static const struct exynos7885_g3d_pll_rate g3d_pll_rates[] = {
	{ 1196000000, 138, 3, 0 },
	{ 949000000, 146, 4, 0 },
	{ 747500000, 115, 4, 0 },
	/* Vendor table labels this 550333313; integer M/P/S gives 550333333. */
	{ 550333333, 127, 3, 1 },
	{ 301000000, 301, 13, 1 },
};

struct exynos7885_g3d_pll {
	struct clk_hw hw;
	void __iomem *base;
	spinlock_t *lock;
	bool mux_selected;
};

#define to_g3d_pll(_hw) container_of(_hw, struct exynos7885_g3d_pll, hw)

static unsigned long exynos7885_g3d_pll_recalc(struct clk_hw *hw,
					    unsigned long parent_rate)
{
	u32 con = readl_relaxed(to_g3d_pll(hw)->base + G3D_PLL_CON0);
	u32 p = FIELD_GET(GENMASK(13, 8), con);
	u64 rate = (u64)parent_rate * FIELD_GET(GENMASK(25, 16), con);

	if (!(con & PLL_MUX_SEL))
		return parent_rate;
	if (!p || !(con & PLL_ENABLE) || !(con & PLL_STABLE))
		return 0;
	do_div(rate, p << FIELD_GET(GENMASK(2, 0), con));
	return rate;
}

static long exynos7885_g3d_pll_round(struct clk_hw *hw, unsigned long rate,
				    unsigned long *parent_rate)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(g3d_pll_rates); i++)
		if (rate >= g3d_pll_rates[i].rate)
			return g3d_pll_rates[i].rate;
	return g3d_pll_rates[ARRAY_SIZE(g3d_pll_rates) - 1].rate;
}

/* The consumer must first move BUSD to the switch-user clock via CCF. */
static int exynos7885_g3d_pll_set(struct clk_hw *hw, unsigned long rate,
				  unsigned long parent_rate)
{
	struct exynos7885_g3d_pll *pll = to_g3d_pll(hw);
	const struct exynos7885_g3d_pll_rate *entry = NULL;
	void __iomem *con = pll->base + G3D_PLL_CON0;
	unsigned long flags;
	u32 old, val;
	unsigned int i;
	int ret;

	if (parent_rate != 26000000)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(g3d_pll_rates); i++)
		if (rate == g3d_pll_rates[i].rate)
			entry = &g3d_pll_rates[i];
	if (!entry)
		return -EINVAL;

	spin_lock_irqsave(pll->lock, flags);
	if (!(readl_relaxed(pll->base + G3D_BUSD_MUX) & BIT(0))) {
		ret = -EBUSY;
		goto out;
	}
	old = readl_relaxed(con);
	writel_relaxed(old & ~PLL_MUX_SEL, con);
	ret = exynos7885_pll_wait(con, PLL_MUX_BUSY, false);
	if (ret)
		goto restore;
	writel_relaxed(readl_relaxed(con) & ~PLL_ENABLE, con);
	writel_relaxed(entry->p * PLL_LOCK_TIME, pll->base + G3D_PLL_LOCK);
	val = old & ~(G3D_PLL_PMS | PLL_STABLE);
	val |= (entry->m << 16) | (entry->p << 8) | entry->s;
	val |= PLL_MUX_SEL;
	writel_relaxed(val, con);
	ret = old & PLL_ENABLE ?
		exynos7885_pll_wait(con, PLL_STABLE, true) : 0;
	if (!ret)
		goto out;
restore:
	/* Leave BUSD on the switch path if the old PLL cannot be recovered. */
	writel_relaxed(readl_relaxed(con) & ~PLL_ENABLE, con);
	writel_relaxed(FIELD_GET(GENMASK(13, 8), old) * PLL_LOCK_TIME,
		       pll->base + G3D_PLL_LOCK);
	writel_relaxed(old & ~PLL_STABLE, con);
	if ((old & PLL_ENABLE) &&
	    exynos7885_pll_wait(con, PLL_STABLE, true)) {
		writel_relaxed(readl_relaxed(con) & ~PLL_ENABLE, con);
		pr_err("exynos7885: PLL_G3D restore failed\n");
	}
out:
	spin_unlock_irqrestore(pll->lock, flags);
	return ret;
}

static int exynos7885_g3d_pll_enable(struct clk_hw *hw)
{
	struct exynos7885_g3d_pll *pll = to_g3d_pll(hw);
	void __iomem *con = pll->base + G3D_PLL_CON0;
	unsigned long flags;
	u32 old;
	int ret;

	spin_lock_irqsave(pll->lock, flags);
	old = readl_relaxed(con);
	/* Do not undo a firmware bypass selection merely by taking a ref. */
	if (!(old & PLL_ENABLE))
		writel_relaxed(old | PLL_ENABLE |
			       (pll->mux_selected ? PLL_MUX_SEL : 0), con);
	ret = exynos7885_pll_wait(con, PLL_STABLE, true);
	if (ret)
		writel_relaxed(old, con);
	spin_unlock_irqrestore(pll->lock, flags);
	return ret;
}

static void exynos7885_g3d_pll_disable(struct clk_hw *hw)
{
	struct exynos7885_g3d_pll *pll = to_g3d_pll(hw);
	void __iomem *con = pll->base + G3D_PLL_CON0;
	unsigned long flags;

	spin_lock_irqsave(pll->lock, flags);
	pll->mux_selected = !!(readl_relaxed(con) & PLL_MUX_SEL);
	writel_relaxed(readl_relaxed(con) & ~PLL_MUX_SEL, con);
	if (!exynos7885_pll_wait(con, PLL_MUX_BUSY, false))
		writel_relaxed(readl_relaxed(con) & ~PLL_ENABLE, con);
	else
		pr_err("exynos7885: PLL_G3D bypass timed out\n");
	spin_unlock_irqrestore(pll->lock, flags);
}

static int exynos7885_g3d_pll_is_enabled(struct clk_hw *hw)
{
	return !!(readl_relaxed(to_g3d_pll(hw)->base + G3D_PLL_CON0) & PLL_ENABLE);
}

static const struct clk_ops exynos7885_g3d_pll_ops = {
	.recalc_rate = exynos7885_g3d_pll_recalc,
	.round_rate = exynos7885_g3d_pll_round,
	.set_rate = exynos7885_g3d_pll_set,
	.enable = exynos7885_g3d_pll_enable,
	.disable = exynos7885_g3d_pll_disable,
	.is_enabled = exynos7885_g3d_pll_is_enabled,
};

struct exynos7885_g3d_mux {
	struct clk_mux mux;
	u32 busy;
};

static int exynos7885_g3d_mux_set_parent(struct clk_hw *hw, u8 index)
{
	struct exynos7885_g3d_mux *mux = container_of(to_clk_mux(hw),
						struct exynos7885_g3d_mux, mux);
	u8 old = clk_mux_ops.get_parent(hw);
	int ret;

	ret = clk_mux_ops.set_parent(hw, index);
	if (ret)
		return ret;
	ret = exynos7885_pll_wait(mux->mux.reg, mux->busy, false);
	if (ret) {
		clk_mux_ops.set_parent(hw, old);
		if (exynos7885_pll_wait(mux->mux.reg, mux->busy, false))
			pr_err("exynos7885: G3D mux restore timed out\n");
	}
	return ret;
}

static u8 exynos7885_g3d_mux_get_parent(struct clk_hw *hw)
{
	return clk_mux_ops.get_parent(hw);
}

static const struct clk_ops exynos7885_g3d_mux_ops = {
	.get_parent = exynos7885_g3d_mux_get_parent,
	.set_parent = exynos7885_g3d_mux_set_parent,
};

static int exynos7885_g3d_register_mux(struct device *dev,
		struct samsung_clk_provider *ctx, unsigned int id,
		const char *name, const char * const *parents,
		u32 offset, u8 shift, u32 busy)
{
	struct exynos7885_g3d_mux *mux;
	struct clk_init_data init = {
		.name = name,
		.ops = &exynos7885_g3d_mux_ops,
		.parent_names = parents,
		.num_parents = 2,
		.flags = CLK_GET_RATE_NOCACHE | CLK_SET_RATE_NO_REPARENT,
	};
	int ret;

	mux = devm_kzalloc(dev, sizeof(*mux), GFP_KERNEL);
	if (!mux)
		return -ENOMEM;
	mux->mux.hw.init = &init;
	mux->mux.reg = ctx->reg_base + offset;
	mux->mux.shift = shift;
	mux->mux.mask = 1;
	mux->mux.lock = &ctx->lock;
	mux->busy = busy;
	ret = devm_clk_hw_register(dev, &mux->mux.hw);
	if (!ret)
		samsung_clk_add_lookup(ctx, &mux->mux.hw, id);
	return ret;
}

static const char * const g3d_switch_p[] = { "fin_pll", "dout_g3d_switch" };
static const char * const g3d_busd_p[] = { "pll_g3d", "mout_g3d_switch_user" };

static const struct exynos7885_gate_clock exynos7885_g3d_gates[] = {
	/* HWACG QCHs as cmucal-qch.c; AGPU has no manual gate register.
	 * TODO(5.10 port): validate AGPU's inferred BUSD parent on hardware;
	 * the remaining AXI/SYSREG/CMU parents are from cmucal-node.c.
	 */
	{ GATE_AGPU_G3D, "gate_agpu_g3d", "mout_g3d_busd", 0, 0, 0x3000 },
	{ GATE_G3D_CMU, "gate_g3d_cmu", "dout_g3d_busp", 0, 0, 0x3004 },
	{ GATE_G3D_LHM_SFR, "gate_g3d_lhm_sfr", "mout_g3d_busd", 0, 0, 0x3008 },
	{ GATE_G3D_LHM_P, "gate_g3d_lhm_p", "dout_g3d_busp", 0, 0, 0x300c },
	{ GATE_G3D_LHS_D, "gate_g3d_lhs_d", "mout_g3d_busd", 0, 0, 0x3010 },
	{ GATE_G3D_LHS_SFR, "gate_g3d_lhs_sfr", "dout_g3d_busp", 0, 0, 0x3014 },
	{ GATE_G3D_SYSREG, "gate_g3d_sysreg", "dout_g3d_busp", 0, 0, 0x3018 },
};

static void exynos7885_g3d_clk_idle(void *data)
{
	pm_runtime_set_suspended(data);
}

static int exynos7885_g3d_clk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct samsung_clk_provider *ctx;
	struct exynos7885_g3d_pll *pll;
	struct clk_divider *div;
	struct clk *input;
	const char *parent = "fin_pll";
	struct clk_init_data init = {
		.name = "pll_g3d",
		.ops = &exynos7885_g3d_pll_ops,
		.parent_names = &parent,
		.num_parents = 1,
		.flags = CLK_GET_RATE_NOCACHE | CLK_IGNORE_UNUSED,
	};
	unsigned int i;
	int ret;

	if (!dev->pm_domain)
		return dev_err_probe(dev, -ENODEV, "G3D power domain required\n");
	input = devm_clk_get(dev, "osc");
	if (IS_ERR(input))
		return PTR_ERR(input);
	if (clk_get_rate(input) != 26000000)
		return -EINVAL;
	input = devm_clk_get(dev, "switch");
	if (IS_ERR(input))
		return PTR_ERR(input);

	/* genpd powered the block before probe. Keep it active and runtime PM
	 * disabled until CMU context management is ported; otherwise genpd's
	 * unused-domain sweep may cut power beneath registered CCF clocks.
	 */
	ret = pm_runtime_set_active(dev);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, exynos7885_g3d_clk_idle, dev);
	if (ret)
		return ret;

	ctx = devm_kzalloc(dev, struct_size(ctx, clk_data.hws, CLK_NR_CLKS),
			   GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->reg_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ctx->reg_base))
		return PTR_ERR(ctx->reg_base);
	spin_lock_init(&ctx->lock);
	ctx->clk_data.num = CLK_NR_CLKS;
	for (i = 0; i < CLK_NR_CLKS; i++)
		ctx->clk_data.hws[i] = ERR_PTR(-ENOENT);

	pll = devm_kzalloc(dev, sizeof(*pll), GFP_KERNEL);
	if (!pll)
		return -ENOMEM;
	pll->base = ctx->reg_base;
	pll->lock = &ctx->lock;
	pll->mux_selected = !!(readl_relaxed(pll->base + G3D_PLL_CON0) & PLL_MUX_SEL);
	pll->hw.init = &init;
	ret = devm_clk_hw_register(dev, &pll->hw);
	if (ret)
		return ret;
	samsung_clk_add_lookup(ctx, &pll->hw, PLL_OUT_G3D);
	ret = exynos7885_g3d_register_mux(dev, ctx, MOUT_G3D_SWITCH_USER,
		"mout_g3d_switch_user", g3d_switch_p, G3D_SWITCH_CON0, 4, BIT(7));
	if (ret)
		return ret;
	ret = exynos7885_g3d_register_mux(dev, ctx, MOUT_G3D_BUSD,
		"mout_g3d_busd", g3d_busd_p, G3D_BUSD_MUX, 0, BIT(16));
	if (ret)
		return ret;

	/* Retain firmware's BUSP ratio until a coordinated DVFS policy exists. */
	div = devm_kzalloc(dev, sizeof(*div), GFP_KERNEL);
	if (!div)
		return -ENOMEM;
	parent = "mout_g3d_busd";
	init.name = "dout_g3d_busp";
	init.ops = &clk_divider_ro_ops;
	init.flags = CLK_GET_RATE_NOCACHE;
	div->hw.init = &init;
	div->reg = ctx->reg_base + G3D_BUSP_DIV;
	div->width = 3;
	div->lock = &ctx->lock;
	ret = devm_clk_hw_register(dev, &div->hw);
	if (ret)
		return ret;
	samsung_clk_add_lookup(ctx, &div->hw, DOUT_G3D_BUSP);

	for (i = 0; i < ARRAY_SIZE(exynos7885_g3d_gates); i++) {
		const struct exynos7885_gate_clock *desc = &exynos7885_g3d_gates[i];
		struct exynos7885_gate *gate;

		gate = devm_kzalloc(dev, sizeof(*gate), GFP_KERNEL);
		if (!gate)
			return -ENOMEM;
		init.name = desc->name;
		init.ops = &exynos7885_gate_ops;
		init.parent_names = &desc->parent;
		init.flags = CLK_GET_RATE_NOCACHE | CLK_IGNORE_UNUSED;
		/* The provider itself must always be able to reach CMU registers. */
		if (desc->id == GATE_G3D_CMU)
			init.flags |= CLK_IS_CRITICAL;
		gate->hw.init = &init;
		gate->qch = ctx->reg_base + desc->qch;
		gate->lock = &ctx->lock;
		ret = devm_clk_hw_register(dev, &gate->hw);
		if (ret)
			return ret;
		samsung_clk_add_lookup(ctx, &gate->hw, desc->id);
	}
	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get,
					 &ctx->clk_data);
}

static const struct of_device_id exynos7885_g3d_clk_match[] = {
	{ .compatible = "samsung,exynos7885-cmu-g3d" },
	{ }
};

static struct platform_driver exynos7885_g3d_clk_driver = {
	.probe = exynos7885_g3d_clk_probe,
	.driver = {
		.name = "exynos7885-cmu-g3d",
		.of_match_table = exynos7885_g3d_clk_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(exynos7885_g3d_clk_driver);

/*
 * CMU_CAM (0x14500000) and CMU_ISP (0x14700000) sit in switchable power
 * domains, like CMU_G3D, so they are platform drivers probed after genpd.
 * Offsets: 4.4 cmucal-sfr.c:81-82,125-138 (CAM) and :737-744,790-803 (ISP);
 * parents and the HWACG vclk list: clk-exynos7885.c:52-61,110-119 (4.4).
 * TODO(5.10 port): runtime PM / context save; the domains stay powered.
 */
static const char * const cam_bus_user_p[] = { "fin_pll", "dout_cam_bus" };
static const char * const isp_bus_user_p[] = { "fin_pll", "dout_isp_bus" };
static const char * const isp_vra_user_p[] = { "fin_pll", "dout_isp_vra" };
static const char * const isp_gdc_user_p[] = { "fin_pll", "dout_isp_gdc" };

struct exynos7885_camisp_mux {
	unsigned int id;
	const char *name;
	const char * const *parents;
	u32 offset;
};

struct exynos7885_camisp_data {
	const struct exynos7885_camisp_mux *muxes;
	unsigned int nmuxes;
	const struct exynos7885_gate_clock *gates;
	unsigned int ngates;
};

static const struct exynos7885_camisp_mux cam_muxes[] = {
	{ UMUX_CLKCMU_CAM_BUS, "mout_cam_bus_user", cam_bus_user_p, 0x0100 },
};

static const struct exynos7885_gate_clock cam_gates[] = {
	{ GATE_IS6P20P0_CAM_S_CAM_3AA1, "gate_cam_3aa1", "mout_cam_bus_user", 0, 0, 0x300c },
	{ GATE_IS6P20P0_CAM_S_CAM_3AA0, "gate_cam_3aa0", "mout_cam_bus_user", 0, 0, 0x3008 },
	{ GATE_IS6P20P0_CAM_S_CAM_CSIS_DMA, "gate_cam_csis_dma", "mout_cam_bus_user", 0, 0, 0x3020 },
	{ GATE_IS6P20P0_CAM_S_CAM_CSIS0, "gate_cam_csis0", "mout_cam_bus_user", 0, 0, 0x3010 },
	{ GATE_IS6P20P0_CAM_S_CAM_CSIS1, "gate_cam_csis1", "mout_cam_bus_user", 0, 0, 0x3014 },
	{ GATE_IS6P20P0_CAM_S_CAM_CSIS2, "gate_cam_csis2", "mout_cam_bus_user", 0, 0, 0x3018 },
	{ GATE_IS6P20P0_CAM_S_CAM_CSIS3, "gate_cam_csis3", "mout_cam_bus_user", 0, 0, 0x301c },
	{ GATE_IS6P20P0_CAM_S_CAM_SMMU, "gate_cam_smmu", "mout_cam_bus_user", 0, 0, 0x3028 },
};

static const struct exynos7885_camisp_mux isp_muxes[] = {
	{ UMUX_CLKCMU_ISP_BUS, "mout_isp_bus_user", isp_bus_user_p, 0x0100 },
	{ UMUX_CLKCMU_ISP_GDC, "mout_isp_gdc_user", isp_gdc_user_p, 0x0120 },
	{ UMUX_CLKCMU_ISP_VRA, "mout_isp_vra_user", isp_vra_user_p, 0x0140 },
};

static const struct exynos7885_gate_clock isp_gates[] = {
	{ GATE_IS5P20P0_ISP_ISP, "gate_ispq_isp", "mout_isp_bus_user", 0, 0, 0x3004 },
	{ GATE_IS5P20P0_ISP_MCSC, "gate_ispq_mcsc", "mout_isp_bus_user", 0, 0, 0x3008 },
	{ GATE_IS5P20P0_ISP_VRA, "gate_ispq_vra", "mout_isp_vra_user", 0, 0, 0x301c },
	{ GATE_IS5P20P0_ISP_GDC, "gate_ispq_gdc", "mout_isp_gdc_user", 0, 0, 0x3000 },
	{ GATE_SMMU_IS0, "gate_smmu_is0", "mout_isp_bus_user", 0, 0, 0x3014 },
	{ GATE_SMMU_IS1, "gate_smmu_is1", "mout_isp_bus_user", 0, 0, 0x3018 },
};

/* MFCMSCL clock IDs and QCHs: Samsung 4.4 clk-exynos7885.c:120-130;
 * CMU register offsets and parent order: exynos7885/cmucal-{sfr,node}.c.
 */
static const char * const mfcmscl_mscl_user_p[] = { "fin_pll", "dout_mfcmscl_mscl" };
static const char * const mfcmscl_mfc_user_p[] = { "fin_pll", "dout_mfcmscl_mfc" };
static const struct exynos7885_camisp_mux mfcmscl_muxes[] = {
	{ UMUX_CLKCMU_MFCMSCL_MSCL, "mout_mfcmscl_mscl_user", mfcmscl_mscl_user_p, 0x0120 },
	{ UMUX_CLKCMU_MFCMSCL_MFC, "mout_mfcmscl_mfc_user", mfcmscl_mfc_user_p, 0x0100 },
};
static const struct exynos7885_gate_clock mfcmscl_gates[] = {
	{ GATE_G2D, "gate_mfcmscl_g2d", "mout_mfcmscl_mscl_user", 0, 0, 0x3000 },
	{ GATE_JPEG, "gate_mfcmscl_jpeg", "mout_mfcmscl_mscl_user", 0, 0, 0x3004 },
	{ GATE_MFC, "gate_mfcmscl_mfc_ip", "mout_mfcmscl_mfc_user", 0, 0, 0x3014 },
	{ GATE_MSCL, "gate_mfcmscl_scaler", "mout_mfcmscl_mscl_user", 0, 0, 0x3018 },
	{ GATE_SMMU_MFCMSCL, "gate_mfcmscl_smmu", "mout_mfcmscl_mscl_user", 0, 0, 0x3020 },
};
static const struct exynos7885_camisp_data mfcmscl_data = {
	mfcmscl_muxes, ARRAY_SIZE(mfcmscl_muxes),
	mfcmscl_gates, ARRAY_SIZE(mfcmscl_gates),
};

static const struct exynos7885_camisp_data cam_data = {
	cam_muxes, ARRAY_SIZE(cam_muxes), cam_gates, ARRAY_SIZE(cam_gates),
};
static const struct exynos7885_camisp_data isp_data = {
	isp_muxes, ARRAY_SIZE(isp_muxes), isp_gates, ARRAY_SIZE(isp_gates),
};

static int exynos7885_camisp_clk_probe(struct platform_device *pdev)
{
	const struct exynos7885_camisp_data *data;
	struct device *dev = &pdev->dev;
	struct samsung_clk_provider *ctx;
	unsigned int i;
	int ret;

	data = of_device_get_match_data(dev);
	if (!dev->pm_domain)
		return dev_err_probe(dev, -ENODEV, "power domain required\n");

	/* Keep the domain powered; see the G3D comment above. */
	ret = pm_runtime_set_active(dev);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, exynos7885_g3d_clk_idle, dev);
	if (ret)
		return ret;

	ctx = devm_kzalloc(dev, struct_size(ctx, clk_data.hws, CLK_NR_CLKS),
			   GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->reg_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ctx->reg_base))
		return PTR_ERR(ctx->reg_base);
	spin_lock_init(&ctx->lock);
	ctx->clk_data.num = CLK_NR_CLKS;
	for (i = 0; i < CLK_NR_CLKS; i++)
		ctx->clk_data.hws[i] = ERR_PTR(-ENOENT);

	for (i = 0; i < data->nmuxes; i++) {
		ret = exynos7885_g3d_register_mux(dev, ctx, data->muxes[i].id,
				data->muxes[i].name, data->muxes[i].parents,
				data->muxes[i].offset, 4, BIT(7));
		if (ret)
			return ret;
		/* Both MFCMSCL USER muxes leave reset on FIN_PLL; select the
		 * TOP bus clock as 4.4 CAL does before MSCL/JPEG/MFC run.
		 */
		if (data == &mfcmscl_data &&
		    (data->muxes[i].id == UMUX_CLKCMU_MFCMSCL_MSCL ||
		     data->muxes[i].id == UMUX_CLKCMU_MFCMSCL_MFC)) {
			struct clk_hw *hw = ctx->clk_data.hws[data->muxes[i].id];
			struct clk_hw *parent = clk_hw_get_parent_by_index(hw, 1);

			if (!parent)
				return -EPROBE_DEFER;
			/* Enable the source during the BUSY-handshaked mux switch. */
			ret = clk_prepare_enable(parent->clk);
			if (ret)
				return ret;
			ret = clk_hw_set_parent(hw, parent);
			clk_disable_unprepare(parent->clk);
			if (ret)
				return ret;
		}
	}

	for (i = 0; i < data->ngates; i++) {
		const struct exynos7885_gate_clock *desc = &data->gates[i];
		struct exynos7885_gate *gate;
		struct clk_init_data init = {
			.name = desc->name,
			.ops = &exynos7885_gate_ops,
			.parent_names = &desc->parent,
			.num_parents = 1,
			.flags = CLK_GET_RATE_NOCACHE | CLK_IGNORE_UNUSED,
		};

		gate = devm_kzalloc(dev, sizeof(*gate), GFP_KERNEL);
		if (!gate)
			return -ENOMEM;
		gate->hw.init = &init;
		gate->qch = ctx->reg_base + desc->qch;
		gate->lock = &ctx->lock;
		ret = devm_clk_hw_register(dev, &gate->hw);
		if (ret)
			return ret;
		samsung_clk_add_lookup(ctx, &gate->hw, desc->id);
	}
	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get,
					 &ctx->clk_data);
}

static const struct of_device_id exynos7885_camisp_clk_match[] = {
	{ .compatible = "samsung,exynos7885-cmu-mfcmscl", .data = &mfcmscl_data },
	{ .compatible = "samsung,exynos7885-cmu-cam", .data = &cam_data },
	{ .compatible = "samsung,exynos7885-cmu-isp", .data = &isp_data },
	{ }
};

static struct platform_driver exynos7885_camisp_clk_driver = {
	.probe = exynos7885_camisp_clk_probe,
	.driver = {
		.name = "exynos7885-cmu-camisp",
		.of_match_table = exynos7885_camisp_clk_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(exynos7885_camisp_clk_driver);
