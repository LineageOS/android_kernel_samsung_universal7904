// SPDX-License-Identifier: GPL-2.0
/* Exynos7885 DISPAUD domain, based on Samsung's 4.4 exynos-pd.c.
 * Copyright (c) 2016 Samsung Electronics Co., Ltd.
 */
#include <soc/samsung/exynos7885-pd.h>
#include <linux/arm-smccc.h>
#include <linux/mfd/syscon.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <asm/barrier.h>

#include "exynos7885-pmucal.h"
#include "exynos7885-pd-data.h"
#include "exynos7885-mfcmscl-pd-data.h"

#define SMC_CMD_PREPARE_PD_ONOFF 0x82000410
#define DISPAUD_SECURE_REG 0x148f0000

struct exynos7885_pd_data {
	const char *name;
	phys_addr_t base;
	u32 secure_reg;
	const struct pmucal_seq *on, *off, *save, *status;
	unsigned int on_count, off_count, save_count, status_count;
};

#define PD_DATA(_name, _base, _smc) { \
	.name = "pd-" #_name, .base = (_base), .secure_reg = (_smc), \
	.on = _name##_on, .on_count = ARRAY_SIZE(_name##_on), \
	.off = _name##_off, .off_count = ARRAY_SIZE(_name##_off), \
	.save = _name##_save, .save_count = ARRAY_SIZE(_name##_save), \
	.status = _name##_status, .status_count = ARRAY_SIZE(_name##_status) }

static const struct exynos7885_pd_data dispaud_data =
	PD_DATA(dispaud, 0x11c84020, DISPAUD_SECURE_REG);
static const struct exynos7885_pd_data g3d_data =
	PD_DATA(g3d, 0x11c840a0, 0);
static const struct exynos7885_pd_data mfcmscl_data =
	PD_DATA(mfcmscl, 0x11c84040, 0);
static const struct exynos7885_pd_data isp_data =
	PD_DATA(isp, 0x11c84060, 0);
static const struct exynos7885_pd_data cam_data =
	PD_DATA(cam, 0x11c84080, 0);

struct exynos7885_pd {
	struct generic_pm_domain genpd;
	const struct exynos7885_pd_data *data;
	struct exynos7885_pmucal cal;
	struct pmucal_seq *on, *off, *save, *status;
	u32 secure_reg;
};

static struct exynos7885_pd *dispaud_pd;

static struct exynos7885_pd *to_exynos7885_pd(struct generic_pm_domain *genpd)
{
	return container_of(genpd, struct exynos7885_pd, genpd);
}

static int exynos7885_pd_secure(struct exynos7885_pd *pd, bool restore)
{
	struct arm_smccc_res res;

	if (!pd->secure_reg)
		return 0;

	/* exynos-el3_mon.c:302-312; exynos-smc.S:39 supplies the barrier. */
	dsb(sy);
	arm_smccc_smc(SMC_CMD_PREPARE_PD_ONOFF, restore, pd->secure_reg, 0,
		      0, 0, 0, 0, &res);
	if ((int)res.a0) {
		dev_err(pd->cal.dev, "secure %s failed: %#lx\n",
			restore ? "restore" : "save", res.a0);
		return -EIO;
	}
	return 0;
}

static int exynos7885_pd_on(struct generic_pm_domain *genpd)
{
	struct exynos7885_pd *pd = to_exynos7885_pd(genpd);
	int ret;

	/* genpd serializes transitions; do not hold a spinlock while polling. */
	ret = exynos7885_pmucal_run(&pd->cal, pd->on, pd->data->on_count);
	if (ret)
		return ret;
	ret = exynos7885_pmucal_restore(&pd->cal, pd->save,
				       pd->data->save_count);
	if (ret)
		return ret;
	/* Secure peripheral access must be restored after the local CAL. */
	return exynos7885_pd_secure(pd, true);
}

static int exynos7885_pd_off(struct generic_pm_domain *genpd)
{
	struct exynos7885_pd *pd = to_exynos7885_pd(genpd);
	int ret, recover;

	/* Late unused-domain cleanup must not cut the bootloader splash. */
	if (!genpd->device_count)
		return -EBUSY;

	/* exynos-pd.c:103-107 precedes pmucal_local_disable's register save. */
	ret = exynos7885_pd_secure(pd, false);
	if (ret)
		return ret;
	ret = exynos7885_pmucal_save(&pd->cal, pd->save,
				    pd->data->save_count);
	if (ret) {
		recover = exynos7885_pd_secure(pd, true);
		if (recover)
			dev_err(pd->cal.dev, "secure save rollback failed\n");
		return ret;
	}
	ret = exynos7885_pmucal_run(&pd->cal, pd->off, pd->data->off_count);
	if (!ret)
		return 0;

	/* An off timeout may have changed hardware. Recover before reporting
	 * failure, since genpd keeps its software state ON on this path.
	 */
	recover = exynos7885_pd_on(genpd);
	if (recover)
		dev_crit(pd->cal.dev, "power-off rollback failed: %d\n", recover);
	return ret;
}

static int exynos7885_pd_copy_seq(struct exynos7885_pd *pd,
				const struct pmucal_seq *src, size_t count,
				struct pmucal_seq **dst)
{
	*dst = devm_kmemdup(pd->cal.dev, src, count * sizeof(*src), GFP_KERNEL);
	if (!*dst)
		return -ENOMEM;
	return exynos7885_pmucal_map(&pd->cal, *dst, count);
}

/*
 * 4.4 exynos_pd_status(): read the real PMU status, not the genpd bookkeeping.
 * DECON uses it to find out whether other consumers keep the domain on after
 * its own runtime suspend.
 */
bool exynos7885_pd_dispaud_is_on(void)
{
	struct exynos7885_pd *pd = dispaud_pd;

	if (!pd || exynos7885_pmucal_run(&pd->cal, pd->status,
					 pd->data->status_count))
		return false;

	return pd->status[0].value == 0xf;
}
EXPORT_SYMBOL_GPL(exynos7885_pd_dispaud_is_on);

static int exynos7885_pd_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	const struct exynos7885_pd_data *data = of_device_get_match_data(dev);
	struct of_phandle_args parent, child = { .np = np };
	struct exynos7885_pd *pd;
	struct regmap *pmu;
	struct resource res;
	int ret;
	u32 state;

	/* Only explicit SoC matches may select the local PMUCAL sequences. */
	ret = of_address_to_resource(np, 0, &res);
	if (ret)
		return ret;
	if (res.start != data->base || resource_size(&res) != 0x20)
		return -EINVAL;
	pmu = syscon_regmap_lookup_by_phandle(np, "samsung,pmu");
	if (IS_ERR(pmu))
		return PTR_ERR(pmu);

	pd = devm_kzalloc(dev, sizeof(*pd), GFP_KERNEL);
	if (!pd)
		return -ENOMEM;
	pd->data = data;
	pd->secure_reg = data->secure_reg;
	if (pd->secure_reg) {
		u32 secure_reg;

		ret = of_property_read_u32(np, "need_smc", &secure_reg);
		if (ret)
			return ret;
		if (secure_reg != pd->secure_reg)
			return -EINVAL;
	}
	ret = exynos7885_pmucal_init(&pd->cal, dev, pmu);
	if (ret)
		return ret;

	ret = exynos7885_pd_copy_seq(pd, data->on, pd->data->on_count, &pd->on);
	if (ret)
		return ret;
	ret = exynos7885_pd_copy_seq(pd, data->off, pd->data->off_count, &pd->off);
	if (ret)
		return ret;
	ret = exynos7885_pd_copy_seq(pd, data->save, pd->data->save_count, &pd->save);
	if (ret)
		return ret;
	ret = exynos7885_pd_copy_seq(pd, data->status,
				   pd->data->status_count, &pd->status);
	if (ret)
		return ret;

	/* Read, never write, the actual PMU status before genpd registration. */
	ret = exynos7885_pmucal_run(&pd->cal, pd->status, pd->data->status_count);
	if (ret)
		return ret;
	state = pd->status[0].value;
	if (state && state != 0xf)
		return dev_err_probe(dev, -EIO, "partial %s state: %#x\n", data->name, state);

	pd->genpd.name = data->name;
	pd->genpd.power_on = exynos7885_pd_on;
	pd->genpd.power_off = exynos7885_pd_off;
	/* The splash/simplefb scans out of DISPAUD and no DPU driver owns it
	 * yet; an off attempt stalls with the display active and hangs the bus.
	 */
	if (data == &dispaud_data && state)
		pd->genpd.flags |= GENPD_FLAG_ALWAYS_ON;
	/* MFCMSCL, CAM and ISP already stay on at runtime (their CMU providers
	 * hold them). Only system sleep tried their off sequences, which hang
	 * suspend (MFCMSCL_STATUS timeout, then the ISP CMU noirq step).
	 */
	if (data == &mfcmscl_data || data == &cam_data || data == &isp_data) {
		if (!state) {
			ret = exynos7885_pd_on(&pd->genpd);
			if (ret)
				return dev_err_probe(dev, ret, "%s power-on failed\n",
						     data->name);
			state = 0xf;
		}
		pd->genpd.flags |= GENPD_FLAG_ALWAYS_ON;
	}
	ret = pm_genpd_init(&pd->genpd, NULL, !state);
	if (ret)
		return ret;
	ret = of_genpd_add_provider_simple(np, &pd->genpd);
	if (ret) {
		pm_genpd_remove(&pd->genpd);
		return ret;
	}
	/* CAM is a child of ISP in the vendor power-domain topology. */
	if (of_find_property(np, "power-domains", NULL)) {
		ret = of_parse_phandle_with_args(np, "power-domains",
				"#power-domain-cells", 0, &parent);
		if (!ret) {
			ret = of_genpd_add_subdomain(&parent, &child);
			of_node_put(parent.np);
		}
		if (ret) {
			of_genpd_del_provider(np);
			pm_genpd_remove(&pd->genpd);
			return ret;
		}
	}
	platform_set_drvdata(pdev, pd);
	if (data == &dispaud_data)
		dispaud_pd = pd;
	dev_warn(dev, "%s initially %s; retain until a consumer suspends\n",
		 data->name, state ? "on" : "off");
	return 0;
}

static const struct of_device_id exynos7885_pd_match[] = {
	{ .compatible = "samsung,exynos7885-dispaud-pd", .data = &dispaud_data },
	{ .compatible = "samsung,exynos7885-g3d-pd", .data = &g3d_data },
	{ .compatible = "samsung,exynos7885-mfcmscl-pd", .data = &mfcmscl_data },
	{ .compatible = "samsung,exynos7885-isp-pd", .data = &isp_data },
	{ .compatible = "samsung,exynos7885-cam-pd", .data = &cam_data },
	{ }
};

/* Built-in and non-removable: genpd and its consumers outlive probe. */
static struct platform_driver exynos7885_pd_driver = {
	.probe = exynos7885_pd_probe,
	.driver = {
		.name = "exynos7885-pd",
		.of_match_table = exynos7885_pd_match,
		.suppress_bind_attrs = true,
	},
};

static int __init exynos7885_pd_init(void)
{
	return platform_driver_register(&exynos7885_pd_driver);
}
subsys_initcall(exynos7885_pd_init);
