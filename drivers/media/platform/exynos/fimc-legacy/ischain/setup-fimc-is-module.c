/* linux/arch/arm/mach-exynos/setup-fimc-sensor.c
 *
 * Copyright (c) 2011 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com/
 *
 * FIMC-IS gpio and clock configuration
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/gpio.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/regulator/consumer.h>
#include <linux/delay.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#ifdef CONFIG_OF
#include <linux/of_gpio.h>
#endif

#include <exynos-fimc-is-module.h>

/* Keep one consumer per module/supply across power-on and power-off.
 * Since regulator core tracks enables per consumer, putting an enabled
 * temporary handle and acquiring another one for disable is not balanced.
 */
struct fimc_is_module_supply {
	const char *name;
	struct regulator *regulator;
	unsigned int enables;
};

static DEFINE_MUTEX(fimc_is_module_supply_lock);

static void fimc_is_module_supply_release(struct device *dev, void *res)
{
	struct fimc_is_module_supply *supply = res;
	int ret;

	mutex_lock(&fimc_is_module_supply_lock);
	while (supply->enables) {
		ret = regulator_disable(supply->regulator);
		if (ret) {
			dev_warn(dev, "failed to release %s: %d\n", supply->name, ret);
			break;
		}
		supply->enables--;
	}
	regulator_put(supply->regulator);
	mutex_unlock(&fimc_is_module_supply_lock);
}

static int fimc_is_module_supply_match(struct device *dev, void *res, void *data)
{
	struct fimc_is_module_supply *supply = res;

	return !strcmp(supply->name, data);
}

/* Caller serializes lookup, creation and use with the supply lock. */
static struct fimc_is_module_supply *fimc_is_module_supply_get(
		struct device *dev, const char *name)
{
	struct fimc_is_module_supply *supply;
	struct regulator *regulator;

	supply = devres_find(dev, fimc_is_module_supply_release,
			fimc_is_module_supply_match, (void *)name);
	if (supply)
		return supply;

	supply = devres_alloc(fimc_is_module_supply_release, sizeof(*supply),
			GFP_KERNEL);
	if (!supply)
		return ERR_PTR(-ENOMEM);

	regulator = regulator_get_optional(dev, name);
	if (IS_ERR(regulator)) {
		devres_free(supply);
		return ERR_CAST(regulator);
	}
	supply->name = name;
	supply->regulator = regulator;
	supply->enables = 0;
	devres_add(dev, supply);
	return supply;
}

static int acquire_shared_rsc(struct exynos_sensor_pin *pin_ctrls)
{
	if (pin_ctrls->shared_rsc_count)
		return atomic_inc_return(pin_ctrls->shared_rsc_count);

	return 1;
}

static int release_shared_rsc(struct exynos_sensor_pin *pin_ctrls)
{
	if (pin_ctrls->shared_rsc_count)
		return atomic_dec_return(pin_ctrls->shared_rsc_count);

	return 0;
}

static int exynos_fimc_is_module_pin_control(struct device *dev,
	struct pinctrl *pinctrl, struct exynos_sensor_pin *pin_ctrls)
{
	char* name = pin_ctrls->name;
	ulong pin = pin_ctrls->pin;
	u32 delay = pin_ctrls->delay;
	u32 value = pin_ctrls->value;
	u32 voltage = pin_ctrls->voltage;
	u32 act = pin_ctrls->act;
	int ret = 0;
	int active_count = 0;
	unsigned long flags;

	if (pin_ctrls->shared_rsc_type) {
		spin_lock_irqsave(pin_ctrls->shared_rsc_slock, flags);

		if (pin_ctrls->shared_rsc_type == SRT_ACQUIRE)
			active_count = acquire_shared_rsc(pin_ctrls);
		else if (pin_ctrls->shared_rsc_type == SRT_RELEASE)
			active_count = release_shared_rsc(pin_ctrls);

		pr_info("[@] shared rsc (act(%d), pin(%ld), val(%d), nm(%s), active(cur: %d, target: %d))\n",
			pin_ctrls->act,
			(pin_ctrls->act == PIN_FUNCTION) ? 0 : pin_ctrls->pin,
			pin_ctrls->value,
			pin_ctrls->name,
			active_count,
			pin_ctrls->shared_rsc_active);

		spin_unlock_irqrestore(pin_ctrls->shared_rsc_slock, flags);

		if (active_count != pin_ctrls->shared_rsc_active)
			return 0;
	}

	switch (act) {
	case PIN_NONE:
		usleep_range(delay, delay);
		break;
	case PIN_OUTPUT:
		if (gpio_is_valid(pin)) {
			if (value) {
				ret = gpio_request_one(pin, GPIOF_OUT_INIT_HIGH, "CAM_GPIO_OUTPUT_HIGH");
				if(ret < 0)
					pr_err("pinctrl_out_init_high(%s) is fail(%d)\n", name, ret);
			} else {
				ret = gpio_request_one(pin, GPIOF_OUT_INIT_LOW, "CAM_GPIO_OUTPUT_LOW");
				if(ret < 0)
					pr_err("pinctrl_out_init_low(%s) is fail(%d)\n", name, ret);
			}
			usleep_range(delay, delay);
			gpio_free(pin);
		} else {
			pr_err("gpio_is_valid fail(%s)-(%ld)\n", name, pin);
		}		
		break;
	case PIN_INPUT:
		if (gpio_is_valid(pin)) {
			gpio_request_one(pin, GPIOF_IN, "CAM_GPIO_INPUT");
			gpio_free(pin);
		}
		break;
	case PIN_RESET:
		if (gpio_is_valid(pin)) {
			gpio_request_one(pin, GPIOF_OUT_INIT_HIGH, "CAM_GPIO_RESET");
			usleep_range(1000, 1000);
			__gpio_set_value(pin, 0);
			usleep_range(1000, 1000);
			__gpio_set_value(pin, 1);
			usleep_range(1000, 1000);
			gpio_free(pin);
		}
		break;
	case PIN_FUNCTION:
		{
			struct pinctrl_state *s = (struct pinctrl_state *)pin;

			ret = pinctrl_select_state(pinctrl, s);
			if (ret < 0) {
				pr_err("pinctrl_select_state(%s) is fail(%d)\n", name, ret);
				return ret;
			}
			usleep_range(delay, delay);
		}
		break;
	case PIN_REGULATOR:
		{
			struct fimc_is_module_supply *supply;
			struct regulator *regulator;

			mutex_lock(&fimc_is_module_supply_lock);
			supply = fimc_is_module_supply_get(dev, name);
			if (IS_ERR(supply)) {
				ret = PTR_ERR(supply);
				dev_err(dev, "regulator_get(%s) failed: %d\n", name, ret);
				goto supply_unlock;
			}
			regulator = supply->regulator;

			if (value) {
				if (voltage > 0) {
					ret = regulator_set_voltage(regulator, voltage, voltage);
					if (ret)
						goto supply_unlock;
				}
				ret = regulator_enable(regulator);
				if (!ret)
					supply->enables++;
			} else if (supply->enables) {
				ret = regulator_disable(regulator);
				if (!ret)
					supply->enables--;
			}
			/* An enabled rail may belong to another camera consumer.
			 * Never disable it without this module's own enable vote.
			 */
			if (!ret)
				usleep_range(delay, delay);

supply_unlock:
			mutex_unlock(&fimc_is_module_supply_lock);
			if (ret)
				return ret;
		}
		break;
	default:
		pr_err("unknown act for pin\n");
		break;
	}

	return ret;
}

int exynos_fimc_is_module_pins_cfg(struct fimc_is_module_enum *module,
	u32 scenario,
	u32 gpio_scenario)
{
	int ret = 0;
	u32 idx_max, idx;
	struct pinctrl *pinctrl;
	struct exynos_sensor_pin (*pin_ctrls)[GPIO_SCENARIO_MAX][GPIO_CTRL_MAX];
	struct exynos_platform_fimc_is_module *pdata;

	BUG_ON(!module);
	BUG_ON(!module->pdata);
	BUG_ON(gpio_scenario >= GPIO_SCENARIO_MAX);
	BUG_ON(scenario > SENSOR_SCENARIO_MAX);

	pdata = module->pdata;
	pinctrl = pdata->pinctrl;
	pin_ctrls = pdata->pin_ctrls;
	idx_max = pdata->pinctrl_index[scenario][gpio_scenario];

	/* print configs */
	for (idx = 0; idx < idx_max; ++idx) {
		printk(KERN_DEBUG "[@] pin_ctrl(act(%d), pin(%ld), val(%d), nm(%s)\n",
			pin_ctrls[scenario][gpio_scenario][idx].act,
			(pin_ctrls[scenario][gpio_scenario][idx].act == PIN_FUNCTION) ? 0 : pin_ctrls[scenario][gpio_scenario][idx].pin,
			pin_ctrls[scenario][gpio_scenario][idx].value,
			pin_ctrls[scenario][gpio_scenario][idx].name);
	}

	/* do configs */
	for (idx = 0; idx < idx_max; ++idx) {
		ret = exynos_fimc_is_module_pin_control(module->dev, pinctrl, &pin_ctrls[scenario][gpio_scenario][idx]);
		if (ret) {
			pr_err("[@] exynos_fimc_is_sensor_gpio(%d) is fail(%d)", idx, ret);
			goto p_err;
		}
	}

p_err:
	return ret;
}

static int exynos_fimc_is_module_pin_debug(struct device *dev,
	struct pinctrl *pinctrl, struct exynos_sensor_pin *pin_ctrls)
{
	int ret = 0;
	ulong pin = pin_ctrls->pin;
	char* name = pin_ctrls->name;
	u32 act = pin_ctrls->act;

	switch (act) {
	case PIN_NONE:
		break;
	case PIN_OUTPUT:
	case PIN_INPUT:
	case PIN_RESET:
		if (gpio_is_valid(pin))
			pr_info("[@] pin %s : %d\n", name, gpio_get_value(pin));
		break;
	case PIN_FUNCTION:
#if defined(CONFIG_SOC_EXYNOS7420) || defined(CONFIG_SOC_EXYNOS7890)
		{
			/* there is no way to get pin by name after probe */
			ulong base, pin;
			u32 index;

			base = (ulong)ioremap_nocache(0x13470000, 0x10000);

			index = 0x60; /* GPC2 */
			pin = base + index;
			pr_info("[@] CON[0x%X] : 0x%X\n", index, readl((void *)pin));
			pr_info("[@] DAT[0x%X] : 0x%X\n", index, readl((void *)(pin + 4)));

			index = 0x160; /* GPD7 */
			pin = base + index;
			pr_info("[@] CON[0x%X] : 0x%X\n", index, readl((void *)pin));
			pr_info("[@] DAT[0x%X] : 0x%X\n", index, readl((void *)(pin + 4)));

			iounmap((void *)base);
		}
#endif
		break;
	case PIN_REGULATOR:
		{
			struct regulator *regulator;
			int voltage;

			regulator = regulator_get_optional(dev, name);
			if (IS_ERR(regulator)) {
				pr_err("%s : regulator_get(%s) fail\n", __func__, name);
				return PTR_ERR(regulator);
			}

			if (regulator_is_enabled(regulator))
				voltage = regulator_get_voltage(regulator);
			else
				voltage = 0;

			regulator_put(regulator);

			pr_info("[@] %s LDO : %d\n", name, voltage);
		}
		break;
	default:
		pr_err("unknown act for pin\n");
		break;
	}

	return ret;
}

int exynos_fimc_is_module_pins_dbg(struct fimc_is_module_enum *module,
	u32 scenario,
	u32 gpio_scenario)
{
	int ret = 0;
	u32 idx_max, idx;
	struct pinctrl *pinctrl;
	struct exynos_sensor_pin (*pin_ctrls)[GPIO_SCENARIO_MAX][GPIO_CTRL_MAX];
	struct exynos_platform_fimc_is_module *pdata;

	BUG_ON(!module);
	BUG_ON(!module->pdata);
	BUG_ON(gpio_scenario > 1);
	BUG_ON(scenario > SENSOR_SCENARIO_MAX);

	pdata = module->pdata;
	pinctrl = pdata->pinctrl;
	pin_ctrls = pdata->pin_ctrls;
	idx_max = pdata->pinctrl_index[scenario][gpio_scenario];

	/* print configs */
	for (idx = 0; idx < idx_max; ++idx) {
		printk(KERN_DEBUG "[@] pin_ctrl(act(%d), pin(%ld), val(%d), nm(%s)\n",
			pin_ctrls[scenario][gpio_scenario][idx].act,
			(pin_ctrls[scenario][gpio_scenario][idx].act == PIN_FUNCTION) ? 0 : pin_ctrls[scenario][gpio_scenario][idx].pin,
			pin_ctrls[scenario][gpio_scenario][idx].value,
			pin_ctrls[scenario][gpio_scenario][idx].name);
	}

	/* do configs */
	for (idx = 0; idx < idx_max; ++idx) {
		ret = exynos_fimc_is_module_pin_debug(module->dev, pinctrl, &pin_ctrls[scenario][gpio_scenario][idx]);
		if (ret) {
			pr_err("[@] exynos_fimc_is_sensor_gpio(%d) is fail(%d)", idx, ret);
			goto p_err;
		}
	}

p_err:
	return ret;
}
