/*
 * Copyright (C) 2012 Samsung Electronics, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/sec_batt.h>

#if defined(CONFIG_BATTERY_SAMSUNG)

unsigned int lpcharge;
EXPORT_SYMBOL(lpcharge);

int charging_night_mode;
EXPORT_SYMBOL(charging_night_mode);

static int sec_bat_is_lpm_check(char *str)
{
	if (strncmp(str, "charger", 7) == 0)
		lpcharge = 1;

	pr_info("%s: Low power charging mode: %d\n", __func__, lpcharge);

	return lpcharge;
}
__setup("androidboot.mode=", sec_bat_is_lpm_check);

static int __init charging_mode(char *str)
{
	int mode;

	/*
	 * Only update loglevel value when a correct setting was passed,
	 * to prevent blind crashes (when loglevel being set to 0) that
	 * are quite hard to debug
	 */
	if (get_option(&str, &mode)) {
		charging_night_mode = mode & 0x000000FF;

		printk(KERN_ERR "charging_mode() : 0x%x(%d)\n", charging_night_mode, charging_night_mode);

		return 0;
	}

	printk(KERN_ERR "charging_mode() : %d\n", -EINVAL);

	return -EINVAL;
}
early_param("charging_mode", charging_mode);

int fg_reset;
EXPORT_SYMBOL(fg_reset);

static int sec_bat_get_fg_reset(char *val)
{
	fg_reset = strncmp(val, "1", 1) ? 0 : 1;
	pr_info("%s, fg_reset:%d\n", __func__, fg_reset);
	return 1;
}
__setup("fg_reset=", sec_bat_get_fg_reset);

int factory_mode;
EXPORT_SYMBOL(factory_mode);

static int sec_bat_get_factory_mode(char *val)
{
        factory_mode = strncmp(val, "1", 1) ? 0 : 1;
        pr_info("%s, factory_mode : %d\n", __func__, factory_mode);
        return 1;
}
__setup("factory_mode=", sec_bat_get_factory_mode);
#endif

/* Boot policy only; MUIC hardware/notifier is not enabled.
 * universal7904 drivers/muic/muic-core.c:189-245. */
#include <linux/muic/muic.h>
#if !IS_ENABLED(CONFIG_USE_MUIC)
static int battery_switch_sel;
/* With the MUIC driver these two live in drivers/muic/muic_core.c (same code). */
static int set_switch_sel(char *str)
{
	get_option(&str, &battery_switch_sel);
	battery_switch_sel = (battery_switch_sel) & 0xfff;
	pr_info("%s: switch_sel: 0x%03x\n", __func__,
			battery_switch_sel);

	return battery_switch_sel;
}
__setup("pmic_info=", set_switch_sel);

int get_switch_sel(void)
{
	return battery_switch_sel;
}

/* afc_mode:
 *   0x31 : Disabled
 *   0x30 : Enabled
 */
static int afc_mode = 0;
static int __init set_afc_mode(char *str)
{
	int mode;
	get_option(&str, &mode);
	afc_mode = (mode & 0x0000FF00) >> 8;
	pr_info("%s: afc_mode is 0x%02x\n", __func__, afc_mode);

	return 0;
}
early_param("charging_mode", set_afc_mode);

int get_afc_mode(void)
{
	return afc_mode;
}
#endif /* !CONFIG_USE_MUIC */

/* universal7904 drivers/staging/samsung/sec_param.c:197-217. */
static char *salescode;
bool sales_code_is(char* str)
{
	bool status = 0;

	if(!salescode)
		goto out;

	if (!strncmp((char *)salescode, str, 3))
		status = 1;

out:
	return status;
}
EXPORT_SYMBOL(sales_code_is);

static int __init sales_code_setup(char *str)
{
	salescode = str;
	return 1;
}
early_param("sales_code", sales_code_setup);
