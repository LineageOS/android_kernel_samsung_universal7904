// SPDX-License-Identifier: GPL-2.0
/*
 * Panel id from the bootloader command line: lcdtype=<value>.
 * Same parser as 4.4 drivers/video/fbdev/exynos/dpu_7885/panels/dsim_panel.c.
 * Shared by the touch driver and the panel code.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/string.h>

unsigned int lcdtype;
EXPORT_SYMBOL(lcdtype);

static int __init get_lcd_type(char *arg)
{
	get_option(&arg, &lcdtype);

	pr_info("lcdtype: %6X\n", lcdtype);

	return 0;
}
early_param("lcdtype", get_lcd_type);

/* CMDLINE_FORCE keeps our debug console but discards Samsung's dynamic
 * lcdtype argument. The original /chosen property is still available;
 * recover only that hardware ID before the panel/touch drivers probe.
 */
static int __init lcdtype_from_chosen(void)
{
	const char *args, *entry;
	char *value;
	int panel_id;

	if (lcdtype || !of_machine_is_compatible("samsung,m20lte"))
		return 0;
	if (of_property_read_string(of_chosen, "bootargs", &args))
		return 0;
	entry = strstr(args, "lcdtype=");
	if (!entry || (entry != args && entry[-1] != ' '))
		return 0;
	value = (char *)entry + strlen("lcdtype=");
	if (get_option(&value, &panel_id) && panel_id > 0) {
		lcdtype = panel_id;
		pr_warn("m20: recovered lcdtype=%#x from /chosen/bootargs\n", lcdtype);
	}
	return 0;
}
arch_initcall(lcdtype_from_chosen);
