// SPDX-License-Identifier: GPL-2.0-only
/* Exynos7885/7904 camera Q-channels, from Samsung 4.4 CMUCAL.
 * Copyright (c) 2017 Samsung Electronics Co., Ltd.
 *
 * Unlike the always-on TOP CMU, these registers must not be touched while
 * their power domain is off. clk_prepare() pins the domain until unprepare.
 * No is_enabled/get_parent callback reads a powered-off local register.
 */
#include <linux/clk-provider.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <dt-bindings/clock/exynos7885.h>

struct camera_gate_desc {
	unsigned int id;
	const char *name;
	const char *parent;
	u16 qch;
	u16 mux;
};

struct camera_gate {
	struct clk_hw hw;
	struct device *dev;
	void __iomem *base;
	const struct camera_gate_desc *desc;
	spinlock_t *lock;
};

#define CAM_GATE(_id, _qch) \
	{ _id, #_id, "dout_cam_bus", _qch, 0x0100 }
#define ISP_GATE(_id, _parent, _qch, _mux) \
	{ _id, #_id, _parent, _qch, _mux }

/* cmucal-sfr.c:130-138; vendor HWACG_VCLKs have only a Q-channel. */
static const struct camera_gate_desc cam_gates[] = {
	CAM_GATE(GATE_IS6P20P0_CAM_S_CAM_3AA1, 0x300c),
	CAM_GATE(GATE_IS6P20P0_CAM_S_CAM_3AA0, 0x3008),
	CAM_GATE(GATE_IS6P20P0_CAM_S_CAM_CSIS_DMA, 0x3020),
	CAM_GATE(GATE_IS6P20P0_CAM_S_CAM_CSIS0, 0x3010),
	CAM_GATE(GATE_IS6P20P0_CAM_S_CAM_CSIS1, 0x3014),
	CAM_GATE(GATE_IS6P20P0_CAM_S_CAM_CSIS2, 0x3018),
	CAM_GATE(GATE_IS6P20P0_CAM_S_CAM_CSIS3, 0x301c),
	CAM_GATE(GATE_IS6P20P0_CAM_S_CAM_SMMU, 0x3028),
};

/* cmucal-sfr.c:796-803, user muxes at 0x100/0x120/0x140. */
static const struct camera_gate_desc isp_gates[] = {
	ISP_GATE(GATE_IS5P20P0_ISP_ISP, "dout_isp_bus", 0x3004, 0x0100),
	ISP_GATE(GATE_IS5P20P0_ISP_MCSC, "dout_isp_bus", 0x3008, 0x0100),
	ISP_GATE(GATE_IS5P20P0_ISP_VRA, "dout_isp_vra", 0x301c, 0x0140),
	ISP_GATE(GATE_IS5P20P0_ISP_GDC, "dout_isp_gdc", 0x3000, 0x0120),
	ISP_GATE(GATE_SMMU_IS0, "dout_isp_bus", 0x3014, 0x0100),
	ISP_GATE(GATE_SMMU_IS1, "dout_isp_bus", 0x3018, 0x0100),
};

struct camera_cmu_desc {
	const struct camera_gate_desc *gates;
	unsigned int count;
};

static const struct camera_cmu_desc cam_desc = {
	.gates = cam_gates, .count = ARRAY_SIZE(cam_gates),
};
static const struct camera_cmu_desc isp_desc = {
	.gates = isp_gates, .count = ARRAY_SIZE(isp_gates),
};

static int camera_gate_prepare(struct clk_hw *hw)
{
	struct camera_gate *gate = container_of(hw, struct camera_gate, hw);
	void __iomem *mux = gate->base + gate->desc->mux;
	unsigned long flags;
	u32 val;
	int ret;

	ret = pm_runtime_resume_and_get(gate->dev);
	if (ret < 0)
		return ret;
	/* Select the prepared TOP parent, never access CMU before genpd resume. */
	spin_lock_irqsave(gate->lock, flags);
	val = readl(mux);
	writel(val | BIT(4), mux);
	spin_unlock_irqrestore(gate->lock, flags);
	ret = readl_poll_timeout(mux, val, !(val & BIT(7)), 1, 1000);
	if (ret)
		pm_runtime_put_sync(gate->dev);
	return ret;
}

static void camera_gate_unprepare(struct clk_hw *hw)
{
	struct camera_gate *gate = container_of(hw, struct camera_gate, hw);

	pm_runtime_put_sync(gate->dev);
}

static void camera_gate_request(struct clk_hw *hw, bool enable)
{
	struct camera_gate *gate = container_of(hw, struct camera_gate, hw);
	void __iomem *qch = gate->base + gate->desc->qch;
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(gate->lock, flags);
	val = readl(qch);
	/* CAL ra_req_enable_qch(): leave REQ alone under hardware control. */
	if (!(val & BIT(0))) {
		val = enable ? val | BIT(1) : val & ~BIT(1);
		writel(val, qch);
	}
	spin_unlock_irqrestore(gate->lock, flags);
}

static int camera_gate_enable(struct clk_hw *hw)
{
	camera_gate_request(hw, true);
	return 0;
}

static void camera_gate_disable(struct clk_hw *hw)
{
	camera_gate_request(hw, false);
}

static const struct clk_ops camera_gate_ops = {
	.prepare = camera_gate_prepare,
	.unprepare = camera_gate_unprepare,
	.enable = camera_gate_enable,
	.disable = camera_gate_disable,
};

static int exynos7885_camera_clk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct camera_cmu_desc *desc = of_device_get_match_data(dev);
	struct clk_hw_onecell_data *data;
	spinlock_t *lock;
	void __iomem *base;
	unsigned int i;
	int ret;

	if (!dev->pm_domain)
		return dev_err_probe(dev, -EINVAL, "camera CMU needs a power domain\n");
	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);
	data = devm_kzalloc(dev, struct_size(data, hws, CLK_NR_CLKS), GFP_KERNEL);
	lock = devm_kzalloc(dev, sizeof(*lock), GFP_KERNEL);
	if (!data || !lock)
		return -ENOMEM;
	spin_lock_init(lock);
	data->num = CLK_NR_CLKS;
	for (i = 0; i < data->num; i++)
		data->hws[i] = ERR_PTR(-ENOENT);

	pm_runtime_enable(dev);
	for (i = 0; i < desc->count; i++) {
		const struct camera_gate_desc *d = &desc->gates[i];
		struct clk_init_data init = {
			.name = d->name, .ops = &camera_gate_ops,
			.parent_names = &d->parent, .num_parents = 1,
			.flags = CLK_IGNORE_UNUSED,
		};
		struct camera_gate *gate;

		gate = devm_kzalloc(dev, sizeof(*gate), GFP_KERNEL);
		if (!gate) {
			ret = -ENOMEM;
			goto err_pm;
		}
		gate->dev = dev;
		gate->base = base;
		gate->desc = d;
		gate->lock = lock;
		gate->hw.init = &init;
		ret = devm_clk_hw_register(dev, &gate->hw);
		if (ret)
			goto err_pm;
		data->hws[d->id] = &gate->hw;
	}
	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, data);
	if (!ret)
		return 0;
err_pm:
	pm_runtime_disable(dev);
	return ret;
}

static const struct of_device_id exynos7885_camera_clk_match[] = {
	{ .compatible = "samsung,exynos7885-cam-clock", .data = &cam_desc },
	{ .compatible = "samsung,exynos7885-isp-clock", .data = &isp_desc },
	{ }
};

/* Built-in provider: no unbind while camera consumers hold clock handles. */
static struct platform_driver exynos7885_camera_clk_driver = {
	.probe = exynos7885_camera_clk_probe,
	.driver = {
		.name = "exynos7885-camera-clock",
		.of_match_table = exynos7885_camera_clk_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(exynos7885_camera_clk_driver);
