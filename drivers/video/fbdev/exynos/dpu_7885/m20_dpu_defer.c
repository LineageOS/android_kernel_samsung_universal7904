// SPDX-License-Identifier: GPL-2.0
/*
 * m20lte bring-up aid: m20_dpu_delay_ms=<ms> keeps the bootloader splash
 * untouched and registers the display drivers from a delayed work, so the
 * USB console is up before the first DPP/DECON access.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include "decon.h"

static unsigned int m20_dpu_delay_ms;

static int __init m20_dpu_delay_setup(char *str)
{
	return !kstrtouint(str, 0, &m20_dpu_delay_ms);
}
__setup("m20_dpu_delay_ms=", m20_dpu_delay_setup);

bool m20_dpu_deferred(void)
{
	return m20_dpu_delay_ms != 0;
}

/* Stage marker: printed and given time to drain over the USB console. */
void m20_dpu_mark(const char *fn, int line)
{
	if (!m20_dpu_delay_ms)
		return;
	pr_err("m20-dpu: mark %s:%d\n", fn, line);
	msleep(400);
}

static void m20_dpu_register_work(struct work_struct *work)
{
	int ret;

	pr_err("m20-dpu: delayed registration start\n");
	msleep(500);
	ret = m20_dpp_register();
	pr_err("m20-dpu: dpp registered ret=%d\n", ret);
	ret = m20_dsim_register();
	pr_err("m20-dpu: dsim registered ret=%d\n", ret);
	ret = m20_decon_register();
	pr_err("m20-dpu: decon registered ret=%d\n", ret);
}
static DECLARE_DELAYED_WORK(m20_dpu_work, m20_dpu_register_work);

static int __init m20_dpu_defer_init(void)
{
	if (!m20_dpu_delay_ms)
		return 0;
	pr_err("m20-dpu: display registration deferred by %u ms\n",
	       m20_dpu_delay_ms);
	schedule_delayed_work(&m20_dpu_work, msecs_to_jiffies(m20_dpu_delay_ms));
	return 0;
}
late_initcall_sync(m20_dpu_defer_init);
