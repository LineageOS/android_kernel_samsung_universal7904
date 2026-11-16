// SPDX-License-Identifier: GPL-2.0+
/*
 * Samsung S2MPU08 regulators, based on Samsung's 2016 vendor driver.
 * Copyright (c) 2016 Samsung Electronics Co., Ltd.
 *
 * Only register the AP-owned rails described by DT. Leave CP rails, RTC,
 * codec thresholds and bootloader power sequencing alone.
 */

#include <linux/bitops.h>
#include <linux/mfd/samsung/core.h>
#include <linux/mfd/samsung/s2mpu08.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/io.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/slab.h>

struct s2mpu08_info {
	struct sec_pmic_dev *iodev;
	struct mutex lock;
	unsigned int opmode[S2MPU08_REGULATOR_MAX];
};

/* Vendor DT encodes PWREN, PWREN_MIF and normal as 1, 2 and 3. */
static unsigned int s2mpu08_of_map_mode(unsigned int mode)
{
	switch (mode) {
	case 1:
		return REGULATOR_MODE_STANDBY;
	case 2:
		return REGULATOR_MODE_IDLE;
	case 3:
		return REGULATOR_MODE_NORMAL;
	default:
		return REGULATOR_MODE_INVALID;
	}
}

static bool s2mpu08_ldo_erratum(struct regulator_dev *rdev)
{
	struct s2mpu08_info *info = rdev_get_drvdata(rdev);
	int id = rdev_get_id(rdev);

	return info->iodev->pmic_rev == 0 &&
	       (id == S2MPU08_LDO35 || id == S2MPU08_LDO36);
}

/*
 * Revision zero implements LDO35 through SEQ44[7:4] and EXT_CTRL[3],
 * coupled to LDO36. Preserve the vendor transition order, but check errors
 * and serialize the shared registers. Do not run the vendor probe-time
 * sequence that resets their state: retain the bootloader's initial state.
 */
static int s2mpu08_ldo35_36_set(struct regulator_dev *rdev, bool enable)
{
	struct s2mpu08_info *info = rdev_get_drvdata(rdev);
	struct regmap *map = info->iodev->regmap_pmic;
	unsigned int val;
	int ret;

	if (rdev_get_id(rdev) == S2MPU08_LDO35) {
		ret = regmap_read(map, S2MPU08_PMIC_REG_L36CTRL, &val);
		if (ret)
			return ret;
		val = !!(val & S2MPU08_ENABLE_MASK);
		ret = regmap_update_bits(map, 0x75, 0xf0,
					(val == enable) ? 0 : 0xf0);
		if (ret)
			return ret;
		return regmap_update_bits(map, 0xff, 0x08,
					  enable && !val ? 0x08 : 0);
	}

	if (enable) {
		ret = regmap_read(map, 0xff, &val);
		if (ret)
			return ret;
		ret = regmap_update_bits(map, 0x75, 0xf0,
					val & 0x08 ? 0 : 0xf0);
		if (ret)
			return ret;
		ret = regmap_update_bits(map, S2MPU08_PMIC_REG_L36CTRL,
					S2MPU08_ENABLE_MASK, S2MPU08_ENABLE_MASK);
		if (ret)
			return ret;
		return regmap_update_bits(map, 0xff, 0x08, 0);
	}

	ret = regmap_read(map, 0x75, &val);
	if (ret)
		return ret;
	ret = regmap_update_bits(map, 0xff, 0x08,
				(val & 0xf0) == 0 ? 0x08 : 0);
	if (ret)
		return ret;
	ret = regmap_update_bits(map, S2MPU08_PMIC_REG_L36CTRL,
				S2MPU08_ENABLE_MASK, 0);
	if (ret || (val & 0xf0))
		return ret;
	return regmap_update_bits(map, 0x75, 0xf0, 0xf0);
}

static int s2mpu08_set_enable(struct regulator_dev *rdev, bool enable)
{
	struct s2mpu08_info *info = rdev_get_drvdata(rdev);
	int id = rdev_get_id(rdev);
	int ret;

	/* BUCK5 is controlled externally; the vendor driver never toggles it. */
	if (id == S2MPU08_BUCK5)
		return 0;

	mutex_lock(&info->lock);
	if (s2mpu08_ldo_erratum(rdev))
		ret = s2mpu08_ldo35_36_set(rdev, enable);
	else
		ret = regmap_update_bits(info->iodev->regmap_pmic,
					 rdev->desc->enable_reg,
					 rdev->desc->enable_mask,
					 enable ? info->opmode[id] : 0);
	mutex_unlock(&info->lock);
	return ret;
}

static int s2mpu08_enable(struct regulator_dev *rdev)
{
	return s2mpu08_set_enable(rdev, true);
}

static int s2mpu08_disable(struct regulator_dev *rdev)
{
	return s2mpu08_set_enable(rdev, false);
}

static int s2mpu08_is_enabled(struct regulator_dev *rdev)
{
	struct s2mpu08_info *info = rdev_get_drvdata(rdev);
	struct regmap *map = info->iodev->regmap_pmic;
	unsigned int ext, l36, seq;
	int ret;

	/* Preserve the vendor BUCK5 externally-controlled status semantics. */
	if (rdev_get_id(rdev) == S2MPU08_BUCK5)
		return 0;
	if (!s2mpu08_ldo_erratum(rdev) ||
	    rdev_get_id(rdev) != S2MPU08_LDO35)
		return regulator_is_enabled_regmap(rdev);

	mutex_lock(&info->lock);
	ret = regmap_read(map, 0xff, &ext);
	if (ret)
		goto out;
	if (ext & 0x08) {
		ret = 1;
		goto out;
	}
	ret = regmap_read(map, S2MPU08_PMIC_REG_L36CTRL, &l36);
	if (ret)
		goto out;
	ret = regmap_read(map, 0x75, &seq);
	if (!ret)
		ret = !!(l36 & S2MPU08_ENABLE_MASK) && !(seq & 0xf0);
out:
	mutex_unlock(&info->lock);
	return ret;
}

static int s2mpu08_set_mode(struct regulator_dev *rdev, unsigned int mode)
{
	struct s2mpu08_info *info = rdev_get_drvdata(rdev);
	unsigned int value, old;
	int ret;

	switch (mode) {
	case REGULATOR_MODE_STANDBY:
		value = 1;
		break;
	case REGULATOR_MODE_IDLE:
		value = 2;
		break;
	case REGULATOR_MODE_NORMAL:
		value = 3;
		break;
	default:
		return -EINVAL;
	}
	value <<= S2MPU08_ENABLE_SHIFT;

	mutex_lock(&info->lock);
	ret = regmap_read(info->iodev->regmap_pmic,
			  rdev->desc->enable_reg, &old);
	/* Selecting a future mode must not turn a disabled rail on. */
	if (!ret && (old & rdev->desc->enable_mask))
		ret = regmap_update_bits(info->iodev->regmap_pmic,
					 rdev->desc->enable_reg,
					 rdev->desc->enable_mask, value);
	if (!ret)
		info->opmode[rdev_get_id(rdev)] = value;
	mutex_unlock(&info->lock);
	return ret;
}

static int s2mpu08_set_ramp_delay(struct regulator_dev *rdev, int ramp_delay)
{
	struct s2mpu08_info *info = rdev_get_drvdata(rdev);
	unsigned int shift, value;

	if (ramp_delay <= 0)
		return -EINVAL;
	value = ramp_delay / 6000;
	value = value ? fls(value) - 1 : 0;
	if (value > 3)
		return -EINVAL;

	switch (rdev_get_id(rdev)) {
	case S2MPU08_BUCK1:
	case S2MPU08_BUCK6:
	case S2MPU08_BUCK7:
		shift = 6;
		break;
	case S2MPU08_BUCK2:
	case S2MPU08_BUCK3:
		shift = 4;
		break;
	case S2MPU08_BUCK4:
	case S2MPU08_BUCK5:
		shift = 2;
		break;
	case S2MPU08_BUCK8:
		shift = 0;
		break;
	default:
		return -EINVAL;
	}

	return regmap_update_bits(info->iodev->regmap_pmic,
				  S2MPU08_PMIC_REG_BUCKRAMP,
				  0x03 << shift, value << shift);
}

static const struct regulator_ops s2mpu08_ldo_ops = {
	.list_voltage = regulator_list_voltage_linear,
	.map_voltage = regulator_map_voltage_linear,
	.is_enabled = s2mpu08_is_enabled,
	.enable = s2mpu08_enable,
	.disable = s2mpu08_disable,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.set_voltage_time_sel = regulator_set_voltage_time_sel,
	.set_mode = s2mpu08_set_mode,
};

static const struct regulator_ops s2mpu08_buck_ops = {
	.list_voltage = regulator_list_voltage_linear,
	.map_voltage = regulator_map_voltage_linear,
	.is_enabled = s2mpu08_is_enabled,
	.enable = s2mpu08_enable,
	.disable = s2mpu08_disable,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.set_voltage_time_sel = regulator_set_voltage_time_sel,
	.set_mode = s2mpu08_set_mode,
	.set_ramp_delay = s2mpu08_set_ramp_delay,
};

#define S2MPU08_DESC(_name, _id, _ops, _min, _step, _vsel, _en, _time, _mask) { \
	.name = _name, \
	.id = _id, \
	.ops = _ops, \
	.type = REGULATOR_VOLTAGE, \
	.owner = THIS_MODULE, \
	.min_uV = _min, \
	.uV_step = _step, \
	.n_voltages = (_mask) + 1, \
	.vsel_reg = _vsel, \
	.vsel_mask = _mask, \
	.enable_reg = _en, \
	.enable_mask = S2MPU08_ENABLE_MASK, \
	.enable_time = _time, \
	.ramp_delay = S2MPU08_RAMP_DELAY, \
	.of_map_mode = s2mpu08_of_map_mode, \
}

#define LDO_DESC(_name, _id, _ops, _min, _step, _vsel, _en, _time) \
	S2MPU08_DESC(_name, _id, _ops, _min, _step, _vsel, _en, _time, \
		     S2MPU08_LDO_VSEL_MASK)
#define BUCK_DESC(_name, _id, _ops, _min, _step, _vsel, _en, _time) \
	S2MPU08_DESC(_name, _id, _ops, _min, _step, _vsel, _en, _time, \
		     S2MPU08_BUCK_VSEL_MASK)

#define _BUCK(macro) S2MPU08_BUCK##macro
#define _buck_ops(num) s2mpu08_buck_ops##num
#define _LDO(macro) S2MPU08_LDO##macro
#define _REG(ctrl) S2MPU08_PMIC_REG##ctrl
#define _ldo_ops(num) s2mpu08_ldo_ops##num
#define _TIME(macro) S2MPU08_ENABLE_TIME##macro

/* The active vendor descriptors are copied below without voltage changes. */
static const struct regulator_desc regulators[S2MPU08_REGULATOR_MAX] = {

	LDO_DESC("LDO1", _LDO(1), &_ldo_ops(), _LDO(_MIN1),
	_LDO(_STEP1), _REG(_L1CTRL), _REG(_L1CTRL), _TIME(_LDO)),
	LDO_DESC("LDO2", _LDO(2), &_ldo_ops(), _LDO(_MIN3),
	_LDO(_STEP2), _REG(_L2CTRL1), _REG(_L2CTRL1), _TIME(_LDO)),
	LDO_DESC("LDO3", _LDO(3), &_ldo_ops(), _LDO(_MIN1),
	_LDO(_STEP2), _REG(_L3CTRL), _REG(_L3CTRL), _TIME(_LDO)),
	LDO_DESC("LDO4", _LDO(4), &_ldo_ops(), _LDO(_MIN2),
	_LDO(_STEP1), _REG(_L4CTRL), _REG(_L4CTRL), _TIME(_LDO)),
	LDO_DESC("LDO5", _LDO(5), &_ldo_ops(), _LDO(_MIN1),
	_LDO(_STEP1), _REG(_L5CTRL), _REG(_L5CTRL), _TIME(_LDO)),
	LDO_DESC("LDO6", _LDO(6), &_ldo_ops(), _LDO(_MIN1),
	_LDO(_STEP1), _REG(_L6CTRL), _REG(_L6CTRL), _TIME(_LDO)),
	LDO_DESC("LDO7", _LDO(7), &_ldo_ops(), _LDO(_MIN1),
	_LDO(_STEP2), _REG(_L7CTRL), _REG(_L7CTRL), _TIME(_LDO)),
	LDO_DESC("LDO8", _LDO(8), &_ldo_ops(), _LDO(_MIN4),
	_LDO(_STEP2), _REG(_L8CTRL), _REG(_L8CTRL), _TIME(_LDO)),
	LDO_DESC("LDO9", _LDO(9), &_ldo_ops(), _LDO(_MIN4),
	_LDO(_STEP2), _REG(_L9CTRL), _REG(_L9CTRL), _TIME(_LDO)),
	LDO_DESC("LDO10", _LDO(10), &_ldo_ops(), _LDO(_MIN4),
	_LDO(_STEP2), _REG(_L10CTRL), _REG(_L10CTRL), _TIME(_LDO)),
	LDO_DESC("LDO11", _LDO(11), &_ldo_ops(), _LDO(_MIN4),
	_LDO(_STEP2), _REG(_L11CTRL), _REG(_L11CTRL), _TIME(_LDO)),
	LDO_DESC("LDO12", _LDO(12), &_ldo_ops(), _LDO(_MIN1),
	_LDO(_STEP1), _REG(_L12CTRL), _REG(_L12CTRL), _TIME(_LDO)),
	LDO_DESC("LDO13", _LDO(13), &_ldo_ops(), _LDO(_MIN1),
	_LDO(_STEP2), _REG(_L13CTRL), _REG(_L13CTRL), _TIME(_LDO)),
	LDO_DESC("LDO14", _LDO(14), &_ldo_ops(), _LDO(_MIN3),
	_LDO(_STEP2), _REG(_L14CTRL), _REG(_L14CTRL), _TIME(_LDO)),
	LDO_DESC("LDO33", _LDO(33), &_ldo_ops(), _LDO(_MIN1),
	_LDO(_STEP2), _REG(_L33CTRL), _REG(_L33CTRL), _TIME(_LDO)),
	LDO_DESC("LDO34", _LDO(34), &_ldo_ops(), _LDO(_MIN3),
	_LDO(_STEP2), _REG(_L34CTRL), _REG(_L34CTRL), _TIME(_LDO)),
	LDO_DESC("LDO35", _LDO(35), &_ldo_ops(), _LDO(_MIN3),
	_LDO(_STEP2), _REG(_L35CTRL), _REG(_L35CTRL), _TIME(_LDO)),
	LDO_DESC("LDO36", _LDO(36), &_ldo_ops(), _LDO(_MIN1),
	_LDO(_STEP2), _REG(_L36CTRL), _REG(_L36CTRL), _TIME(_LDO)),
	LDO_DESC("LDO37", _LDO(37), &_ldo_ops(), _LDO(_MIN3),
	_LDO(_STEP2), _REG(_L37CTRL), _REG(_L37CTRL), _TIME(_LDO)),
	BUCK_DESC("BUCK1", _BUCK(1), &_buck_ops(), _BUCK(_MIN1),
	_BUCK(_STEP1), _REG(_B1CTRL2), _REG(_B1CTRL1), _TIME(_BUCK1)),
	BUCK_DESC("BUCK2", _BUCK(2), &_buck_ops(), _BUCK(_MIN1),
	_BUCK(_STEP1), _REG(_B2CTRL2), _REG(_B2CTRL1), _TIME(_BUCK2)),
	BUCK_DESC("BUCK3", _BUCK(3), &_buck_ops(), _BUCK(_MIN1),
	_BUCK(_STEP1), _REG(_B3CTRL2), _REG(_B3CTRL1), _TIME(_BUCK3)),
	BUCK_DESC("BUCK4", _BUCK(4), &_buck_ops(), _BUCK(_MIN1),
	_BUCK(_STEP1), _REG(_B4CTRL2), _REG(_B4CTRL1), _TIME(_BUCK4)),
	BUCK_DESC("BUCK5", _BUCK(5), &_buck_ops(), _BUCK(_MIN1),
	_BUCK(_STEP1), _REG(_B5CTRL2), _REG(_B5CTRL1), _TIME(_BUCK5)),
	BUCK_DESC("BUCK6", _BUCK(6), &_buck_ops(), _BUCK(_MIN1),
	_BUCK(_STEP1), _REG(_B6CTRL2), _REG(_B6CTRL1), _TIME(_BUCK6)),
	BUCK_DESC("BUCK7", _BUCK(7), &_buck_ops(), _BUCK(_MIN2),
	_BUCK(_STEP2), _REG(_B7CTRL2), _REG(_B7CTRL1), _TIME(_BUCK7)),
	BUCK_DESC("BUCK8", _BUCK(8), &_buck_ops(), _BUCK(_MIN2),
	_BUCK(_STEP2), _REG(_B8CTRL2), _REG(_B8CTRL1), _TIME(_BUCK8)),

};

static int s2mpu08_probe(struct platform_device *pdev)
{
	struct sec_pmic_dev *iodev = dev_get_drvdata(pdev->dev.parent);
	struct regulator_config config = { };
	struct of_regulator_match *matches;
	struct device_node *np;
	struct s2mpu08_info *info;
	unsigned int value;
	int i, ret;

	if (!iodev || iodev->device_type != S2MPU08)
		return -ENODEV;

	np = of_get_child_by_name(iodev->dev->of_node, "regulators");
	if (!np)
		return -ENODEV;

	info = devm_kzalloc(&pdev->dev, sizeof(*info), GFP_KERNEL);
	matches = devm_kcalloc(&pdev->dev, ARRAY_SIZE(regulators),
			       sizeof(*matches), GFP_KERNEL);
	if (!info || !matches) {
		of_node_put(np);
		return -ENOMEM;
	}
	info->iodev = iodev;
	mutex_init(&info->lock);

	for (i = 0; i < ARRAY_SIZE(regulators); i++) {
		matches[i].name = regulators[i].name;
		matches[i].desc = &regulators[i];
	}
	ret = of_regulator_match(&pdev->dev, np, matches,
				 ARRAY_SIZE(regulators));
	of_node_put(np);
	if (ret < 0)
		return ret;
	if (!ret)
		return -ENODEV;

	config.dev = &pdev->dev;
	config.driver_data = info;
	config.regmap = iodev->regmap_pmic;
	for (i = 0; i < ARRAY_SIZE(regulators); i++) {
		struct regulator_dev *rdev;

		if (!matches[i].of_node ||
		    !of_device_is_available(matches[i].of_node))
			continue;

		/* Retain each rail's bootloader mode until DT requests another. */
		ret = regmap_read(iodev->regmap_pmic,
				  regulators[i].enable_reg, &value);
		if (ret)
			return ret;
		value &= S2MPU08_ENABLE_MASK;
		info->opmode[i] = value ? value : S2MPU08_ENABLE_MASK;

		config.init_data = matches[i].init_data;
		config.of_node = matches[i].of_node;
		rdev = devm_regulator_register(&pdev->dev, &regulators[i], &config);
		if (IS_ERR(rdev))
			return dev_err_probe(&pdev->dev, PTR_ERR(rdev),
					     "failed to register %s\n",
					     regulators[i].name);
	}
	return 0;
}

static const struct platform_device_id s2mpu08_id[] = {
	{ "s2mpu08-regulator", 0 },
	{ }
};
MODULE_DEVICE_TABLE(platform, s2mpu08_id);

static struct platform_driver s2mpu08_driver = {
	.driver = {
		.name = "s2mpu08-regulator",
	},
	.probe = s2mpu08_probe,
	.id_table = s2mpu08_id,
};
module_platform_driver(s2mpu08_driver);

MODULE_DESCRIPTION("Samsung S2MPU08 regulator driver");
MODULE_LICENSE("GPL");
