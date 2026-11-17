/*
 * Copyright (c) 2015 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/firmware.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/debugfs.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/sched/clock.h>
#include <linux/module.h>
#include <linux/sizes.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/pm.h>
#include <linux/input.h>
#include <linux/power_supply.h>
#include <soc/samsung/acpm_dvfs.h>

#include "acpm.h"
#include "acpm_ipc.h"
#include "fw_header/framework.h"

static int ipc_done;
static unsigned long long ipc_time_start;
static unsigned long long ipc_time_end;

static struct acpm_info *exynos_acpm;
static void __iomem *acpm_fvmap_base;

/* Samsung 4.4 fvmap_header is 24 bytes; its rate/voltage entries are
 * pairs of u32 kHz/uV values. Read the firmware table without modifying
 * voltages or importing the old CAL clock-register programming stack.
 */
int exynos_acpm_get_rate_table(unsigned int id, unsigned int *rates, unsigned int max)
{
	void __iomem *base = READ_ONCE(acpm_fvmap_base);
	unsigned int i, count, offset, rate, volt;

	if (!base)
		return -EPROBE_DEFER;
	if (id >= 10 || !rates || !max)
		return -EINVAL;
	count = readb(base + id * 24 + 1);
	offset = readw(base + id * 24 + 20);
	if (!count || count > max || offset < 10 * 24 ||
	    offset > SZ_8K || count * 8 > SZ_8K - offset)
		return -EINVAL;
	for (i = 0; i < count; i++) {
		rate = readl(base + offset + i * 8);
		volt = readl(base + offset + i * 8 + 4);
		/* DISP/AUD levels go below 100 MHz and may carry no voltage. */
		if (rate < 10000 || rate > 3000000 || volt > 1600000 ||
		    (volt && volt < 300000))
			return -ERANGE;
		rates[i] = rate;
	}
	return count;
}
EXPORT_SYMBOL_GPL(exynos_acpm_get_rate_table);

int exynos_acpm_get_boot_rate(unsigned int id, unsigned long *rate)
{
	unsigned int rates[64], level;
	void __iomem *base;
	int count;

	if (!rate)
		return -EINVAL;
	count = exynos_acpm_get_rate_table(id, rates, ARRAY_SIZE(rates));
	if (count < 0)
		return count;
	base = READ_ONCE(acpm_fvmap_base);
	level = readb(base + id * 24 + 8);
	if (level >= (unsigned int)count)
		return -ERANGE;
	*rate = rates[level];
	pr_warn("ACPM domain %u firmware init level %u rate %lu kHz\n",
		id, level, *rate);
	return 0;
}
EXPORT_SYMBOL_GPL(exynos_acpm_get_boot_rate);

static int acpm_send_data(struct device_node *node, unsigned int check_id,
		struct ipc_config *config);

static int handle_dynamic_plugin(struct device_node *node, unsigned int id, unsigned int attach)
{
	struct ipc_config config;
	unsigned int cmd[4] = {0, };
	int ret = 0;

	config.cmd = cmd;
	if (attach)
		config.cmd[0] = (1 << ACPM_IPC_PROTOCOL_DP_A);
	else
		config.cmd[0] = (1 << ACPM_IPC_PROTOCOL_DP_D);

	config.cmd[0] |= id << ACPM_IPC_PROTOCOL_ID;

	config.response = true;
	config.indirection = false;

	ret = acpm_send_data(node, id, &config);

	config.cmd = NULL;

	if (!ret) {
		if (attach)
			pr_info("[ACPM] plugin(id = %d) attach done!\n", id);
		else
			pr_info("[ACPM] plugin(id = %d) detach done!\n", id);
	}

	return ret;
}

static void firmware_load(void __iomem *base, const u8 *firmware, size_t size)
{
	memcpy_toio(base, firmware, size);
	/* Publish the SRAM image before asking the APM to attach it. */
	wmb();
}

static int firmware_update(struct device *dev, void __iomem *fw_base,
			   const char *fw_name, unsigned int slot_size)
{
	const struct firmware *fw_entry = NULL;
	int err;

	dev_info(dev, "Loading %s firmware ... ", fw_name);
	err = request_firmware_direct(&fw_entry, fw_name, dev);
	if (err || !fw_entry) {
		dev_err(dev, "firmware request FAIL \n");
		return err;
	}

	if (!slot_size || fw_entry->size > slot_size) {
		dev_err(dev, "firmware %s is too large (%zu, slot %u)\n",
			fw_name, fw_entry->size, slot_size);
		release_firmware(fw_entry);
		return -EFBIG;
	}
	firmware_load(fw_base, fw_entry->data, fw_entry->size);

	dev_warn(dev, "loaded firmware %s (%zu bytes, slot %u)\n",
		 fw_name, fw_entry->size, slot_size);

	release_firmware(fw_entry);

	return 0;
}

static int plugins_init(void)
{
	struct plugin *plugins;
	int i, len, ret = 0;
	unsigned int plugin_id;
	char name[50];
	const char *fw_name = NULL;
	void __iomem *fw_base_addr = 0, *dvfs_base_addr = 0;
	struct device_node *node, *child;
	const __be32 *prop;
	unsigned int offset;

	plugins = (struct plugin *)(acpm_srambase + acpm_initdata->plugins);

	for (i = 0; i < acpm_initdata->num_plugins; i++) {
		if ((plugins[i].is_attached == 0 && plugins[i].stay_attached == 1) ||
			(plugins[i].is_attached == 1 && plugins[i].stay_attached == 0)) {

			if (plugins[i].is_attached == 0 && plugins[i].stay_attached == 1) {
				fw_name = (const char *)(acpm_srambase + plugins[i].fw_name);
				if (!plugins[i].fw_name) {
					dev_err(exynos_acpm->dev, "fw_name missing on plugins %d.\n", plugins[i].id);
					return -ENOENT;
				}

				fw_base_addr = acpm_srambase + (plugins[i].base_addr & ~0x1);
				if (!plugins[i].base_addr) {
					dev_err(exynos_acpm->dev, "base_addr missing on plugins %d.\n", plugins[i].id);
					return -ENOENT;
				}

				/* Override fw_name from acpm firmware if it is described on DT. */
				node = of_get_child_by_name(exynos_acpm->dev->of_node, "dynamic-plugins");
				for_each_child_of_node(node, child) {
					prop = of_get_property(child, "plugin-id", &len);
					if (!prop)
						continue;
					plugin_id = be32_to_cpup(prop);

					if (plugin_id != i)
						continue;

					if (of_property_read_string(child, "fw-name", &fw_name)) {
						dev_err(exynos_acpm->dev, "fw name of plugin %d is invalid in DT.\n", i);
						return -EINVAL;
					}
				}

				strncpy(name, fw_name, 50);
				name[49]= 0;

				ret = firmware_update(exynos_acpm->dev, fw_base_addr, name,
						      plugins[i].size);
				if (ret) {
					dev_err(exynos_acpm->dev, "firmware_update failed for %s\n", name);
					continue;
				}
			}

			ret = handle_dynamic_plugin(exynos_acpm->dev->of_node, plugins[i].id, plugins[i].stay_attached);
			if (ret < 0)
				dev_err(exynos_acpm->dev, "plugin attach/detach:%u fail! plugin_id:%d, ret:%d",
						plugins[i].stay_attached, plugins[i].id, ret);
			if (!ret && plugins[i].stay_attached && fw_name &&
			    (strstr(fw_name, "DVFS") || strstr(fw_name, "dvfs")))
				dvfs_base_addr = acpm_srambase +
					(plugins[i].base_addr & ~0x1);

		} else if (plugins[i].is_attached == 1 && plugins[i].stay_attached == 1) {
			fw_name = (const char *)(acpm_srambase + plugins[i].fw_name);
			if (plugins[i].fw_name && fw_name &&
					(strstr(fw_name, "DVFS") || strstr(fw_name, "dvfs")))
				dvfs_base_addr = acpm_srambase + (plugins[i].base_addr & ~0x1);
		}
	}

	prop = of_get_property(exynos_acpm->dev->of_node, "fvmap_offset", &len);
	if (prop && dvfs_base_addr) {
		struct device_node *ipc_node;
		struct resource sram;
		unsigned long start;

		offset = be32_to_cpup(prop);
		start = (unsigned long)(dvfs_base_addr - acpm_srambase);
		ipc_node = of_find_compatible_node(NULL, NULL,
						"samsung,exynos-acpm-ipc");
		if (ipc_node && !of_address_to_resource(ipc_node, 1, &sram) &&
		    start <= resource_size(&sram) &&
		    offset <= resource_size(&sram) - start &&
		    SZ_8K <= resource_size(&sram) - start - offset) {
			WRITE_ONCE(acpm_fvmap_base, dvfs_base_addr + offset);
			pr_warn("[ACPM] read-only fvmap: SRAM + %#lx (%#x bytes)\n",
				start + offset, SZ_8K);
		} else {
			dev_err(exynos_acpm->dev, "FVmap outside mapped SRAM\n");
		}
		of_node_put(ipc_node);
	}

	return ret;
}

static int debug_log_level_get(void *data, unsigned long long *val)
{

	return 0;
}

static int debug_log_level_set(void *data, unsigned long long val)
{
	acpm_fw_log_level((unsigned int)val);

	return 0;
}

static int debug_ipc_loopback_test_get(void *data, unsigned long long *val)
{
	struct ipc_config config;
	int ret = 0;
	unsigned int cmd[4] = {0, };

	config.cmd = cmd;
	config.cmd[0] = (1 << ACPM_IPC_PROTOCOL_TEST);
	config.cmd[0] |= 0x3 << ACPM_IPC_PROTOCOL_ID;

	config.response = true;
	config.indirection = false;

	ret = acpm_send_data(exynos_acpm->dev->of_node, 3, &config);

	if (!ret)
		*val = ipc_time_end - ipc_time_start;
	else
		*val = 0;

	config.cmd = NULL;

	return 0;
}

static int debug_ipc_loopback_test_set(void *data, unsigned long long val)
{

	return 0;
}

static int debug_mifdn_count_get(void *data, unsigned long long *val)
{
	*val = acpm_get_mifdn_count();

	return 0;
}

static int debug_mifdn_count_set(void *data, unsigned long long val)
{
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(debug_log_level_fops,
		debug_log_level_get, debug_log_level_set, "%llu\n");
DEFINE_SIMPLE_ATTRIBUTE(debug_ipc_loopback_test_fops,
		debug_ipc_loopback_test_get, debug_ipc_loopback_test_set, "%llu\n");
DEFINE_SIMPLE_ATTRIBUTE(debug_mifdn_count_fops,
		debug_mifdn_count_get, debug_mifdn_count_set, "%llu\n");

static void acpm_debugfs_init(struct acpm_info *acpm)
{
	struct dentry *den;

	den = debugfs_create_dir("acpm_framework", NULL);
	debugfs_create_file("ipc_loopback_test", 0644, den, acpm, &debug_ipc_loopback_test_fops);
	debugfs_create_file("log_level", 0644, den, NULL, &debug_log_level_fops);
	debugfs_create_file("mifdn_count", 0644, den, NULL, &debug_mifdn_count_fops);
}

void *memcpy_align_4(void *dest, const void *src, unsigned int n)
{
	unsigned int *dp = dest;
	const unsigned int *sp = src;
	int i;

	if ((n % 4))
		BUG();

	n = n >> 2;

	for (i = 0; i < n; i++)
		*dp++ = *sp++;

	return dest;
}

void exynos_apm_power_down(void)
{
	unsigned int reg;

	reg = __raw_readl(exynos_acpm->mcore_base + EXYNOS_PMU_CORTEX_APM_CONFIGURATION);
	reg &= APM_LOCAL_PWR_CFG_RESET;
	__raw_writel(reg, exynos_acpm->mcore_base + EXYNOS_PMU_CORTEX_APM_CONFIGURATION);
}

void acpm_enter_wfi(void)
{
	struct ipc_config config;
	int ret = 0;
	unsigned int cmd[4] = {0, };

	if (exynos_acpm->enter_wfi)
		return;

	config.cmd = cmd;
	config.response = true;
	config.indirection = false;
	config.cmd[0] = 1 << ACPM_IPC_PROTOCOL_STOP;

	ret = acpm_send_data(exynos_acpm->dev->of_node, ACPM_IPC_PROTOCOL_STOP, &config);

	config.cmd = NULL;

	if (ret) {
		pr_err("[ACPM] acpm enter wfi fail!!\n");
	} else {
		pr_err("[ACPM] wfi done\n");
		exynos_acpm->enter_wfi++;
	}
}

void exynos_acpm_reboot(void)
{
	acpm_ipc_set_waiting_mode(BUSY_WAIT);
	acpm_enter_wfi();
	exynos_apm_power_down();
}

static int acpm_send_data(struct device_node *node, unsigned int check_id,
		struct ipc_config *config)
{
	unsigned int channel_num, size;
	int ret = 0;
	int timeout_flag;
	unsigned int id = 0;

	if (!acpm_ipc_request_channel(node, NULL,
				&channel_num, &size)) {
		ipc_done = -1;

		ipc_time_start = sched_clock();
		ret = acpm_ipc_send_data(channel_num, config);

		if (config->cmd[0] & ACPM_IPC_PROTOCOL_DP_CMD) {
			id = config->cmd[0] & ACPM_IPC_PROTOCOL_IDX;
			id = id	>> ACPM_IPC_PROTOCOL_ID;
			ipc_done = id;
		} else if (config->cmd[0] & (1 << ACPM_IPC_PROTOCOL_STOP)) {
			ipc_done = ACPM_IPC_PROTOCOL_STOP;
		} else if (config->cmd[0] & (1 << ACPM_IPC_PROTOCOL_TEST)) {
			id = config->cmd[0] & ACPM_IPC_PROTOCOL_IDX;
			id = id	>> ACPM_IPC_PROTOCOL_ID;
			ipc_done = id;
		} else {
			id = config->cmd[0] & ACPM_IPC_PROTOCOL_IDX;
			id = id	>> ACPM_IPC_PROTOCOL_ID;
			ipc_done = id;
		}

		/* Response interrupt waiting */
		UNTIL_EQUAL(ipc_done, check_id, timeout_flag);

		if (timeout_flag)
			ret = -ETIMEDOUT;

		acpm_ipc_release_channel(node, channel_num);
	} else {
		pr_err("%s ipc request_channel fail, id:%u, size:%u\n",
				__func__, channel_num, size);
		ret = -EBUSY;
	}

	return ret;
}

/*
 * Power off as the stock 4.4 sec_reboot.c does: wait for the power key to be
 * released, stop the APM through ACPM, then drop PS_HOLD. PSCI SYSTEM_OFF
 * alone skips the APM/PMIC preparation, and a held key at that point keeps
 * the PMIC from powering on again with a long key press.
 */
#define M20_PMU_PS_HOLD_CONTROL	0x330c
#define M20_PS_HOLD_DATA	BIT(8)
#define M20_PMU_INFORM2		0x808
#define M20_PMU_SWRESET		0x400
#define M20_SEC_POWER_OFF	0

static struct regmap *m20_pmu;
static struct device_node *m20_powerkey_np;
static void (*m20_orig_power_off)(void);

/* A charger/USB supply is online: stock reboots into LP charging instead. */
static bool m20_charger_online(void)
{
	static const char * const names[] = { "ac", "usb", "wireless" };
	union power_supply_propval val;
	struct power_supply *psy;
	bool online = false;
	int i;

	for (i = 0; i < ARRAY_SIZE(names) && !online; i++) {
		psy = power_supply_get_by_name(names[i]);
		if (!psy)
			continue;
		if (!power_supply_get_property(psy, POWER_SUPPLY_PROP_ONLINE, &val))
			online = val.intval;
		power_supply_put(psy);
	}
	return online;
}

static void exynos7885_acpm_power_off(void)
{
	unsigned int val, i;
	int key = m20_powerkey_np ? of_get_gpio(m20_powerkey_np, 0) : -1;
	bool charger = m20_charger_online() &&
		       !strstr(saved_command_line, "androidboot.mode=charger");

	local_irq_disable();

	if (charger) {
		/*
		 * sec_reboot.c: with a charger connected a PS_HOLD drop would
		 * restart the PMIC by itself. INFORM2=SEC_POWER_OFF and a soft
		 * reset make the bootloader enter LP charging.
		 */
		regmap_write(m20_pmu, M20_PMU_INFORM2, M20_SEC_POWER_OFF);
		exynos_acpm_reboot();
		regmap_update_bits(m20_pmu, M20_PMU_SWRESET, BIT(0), BIT(0));
		mdelay(1000);
	}

	/* the key reads high (raw) when released; wait up to 10 s */
	for (i = 0; gpio_is_valid(key) && i < 100 && !gpio_get_value(key); i++)
		mdelay(100);

	exynos_acpm_reboot();

	if (!regmap_read(m20_pmu, M20_PMU_PS_HOLD_CONTROL, &val))
		regmap_write(m20_pmu, M20_PMU_PS_HOLD_CONTROL,
			     val & ~M20_PS_HOLD_DATA);
	mdelay(1000);

	/* PS_HOLD did not take effect: fall back to the firmware call */
	if (m20_orig_power_off)
		m20_orig_power_off();
	while (1)
		cpu_relax();
}

static void acpm_poweroff_init(struct device *dev)
{
	struct device_node *np;

	m20_pmu = syscon_regmap_lookup_by_compatible("samsung,exynos7885-pmu");
	if (IS_ERR(m20_pmu)) {
		dev_warn(dev, "no PMU regmap, power off stays on PSCI\n");
		return;
	}
	for_each_compatible_node(np, NULL, "gpio-keys") {
		struct device_node *key;

		for_each_child_of_node(np, key) {
			u32 code;

			if (!of_property_read_u32(key, "linux,code", &code) &&
			    code == KEY_POWER)
				m20_powerkey_np = of_node_get(key);
		}
	}
	m20_orig_power_off = pm_power_off;
	pm_power_off = exynos7885_acpm_power_off;
}

static int acpm_probe(struct platform_device *pdev)
{
	struct acpm_info *acpm;
	struct device_node *node = pdev->dev.of_node;
	struct resource *res;
	int ret = 0;

	dev_info(&pdev->dev, "acpm probe\n");

	if (!node) {
		dev_err(&pdev->dev, "driver doesnt support"
				"non-dt devices\n");
		return -ENODEV;
	}

	acpm = devm_kzalloc(&pdev->dev,
			sizeof(struct acpm_info), GFP_KERNEL);
	if (IS_ERR(acpm))
		return PTR_ERR(acpm);
	if (!acpm)
		return -ENOMEM;

	acpm->dev = &pdev->dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	acpm->mcore_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(acpm->mcore_base))
		return PTR_ERR(acpm->mcore_base);

	exynos_acpm = acpm;
	dev_warn(&pdev->dev, "APM power config %#x status %#x\n",
		 readl(acpm->mcore_base + EXYNOS_PMU_CORTEX_APM_CONFIGURATION),
		 readl(acpm->mcore_base + EXYNOS_PMU_CORTEX_APM_STATUS));

	acpm_debugfs_init(acpm);
	acpm_poweroff_init(&pdev->dev);

	return ret;
}

static int acpm_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id acpm_match[] = {
	{ .compatible = "samsung,exynos-acpm" },
	{},
};

static struct platform_driver samsung_acpm_driver = {
	.probe	= acpm_probe,
	.remove	= acpm_remove,
	.driver	= {
		.name = "exynos-acpm",
		.owner	= THIS_MODULE,
		.of_match_table	= acpm_match,
	},
};

static int __init exynos_acpm_init(void)
{
	return platform_driver_register(&samsung_acpm_driver);
}
arch_initcall_sync(exynos_acpm_init);

static int __init exynos_acpm_binary_update(void)
{
	int ret;

	acpm_ipc_set_waiting_mode(BUSY_WAIT);

	ret = plugins_init();

	return ret;
}
fs_initcall_sync(exynos_acpm_binary_update);
