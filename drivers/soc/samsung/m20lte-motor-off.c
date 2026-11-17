// SPDX-License-Identifier: GPL-2.0
/*
 * M20 GPIO vibrator, with the timed_output ABI used by the vendor HAL.
 * The external motor LDO is enabled by GPP0-3, active high. Keep the
 * early bootloader-state fix until the GPIO driver can take ownership.
 */
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/kthread.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/slab.h>

#define M20_GPP0_BASE 0x139b0000
#define GPP0_CON 0x00
#define GPP0_DAT 0x04
#define MOT_PIN 3
#define M20_MAX_TIMEOUT_MS 10000

static int __init m20lte_motor_off(void)
{
	void __iomem *b;
	u32 con, dat;

	if (!of_machine_is_compatible("samsung,m20lte"))
		return 0;
	b = ioremap(M20_GPP0_BASE, 0x100);
	if (!b)
		return -ENOMEM;
	con = readl(b + GPP0_CON);
	dat = readl(b + GPP0_DAT);
	writel(dat & ~BIT(MOT_PIN), b + GPP0_DAT);
	writel((con & ~(0xf << (MOT_PIN * 4))) | (0x1 << (MOT_PIN * 4)),
	       b + GPP0_CON);
	writel(dat & ~BIT(MOT_PIN), b + GPP0_DAT);
	iounmap(b);
	return 0;
}
arch_initcall(m20lte_motor_off);

struct m20_vibrator {
	struct gpio_desc *enable_gpio;
	struct hrtimer timer;
	struct mutex lock;
	struct device *sysfs_dev;
	struct device *dev;
	bool suspended;
};

static struct class *m20_timed_output_class;
static bool m20_motor_owned;

/* Failsafe for deferred/failed probe: do not let the bootloader or an old
 * UART pinmux leave the motor running. The GPIO driver owns the pin once
 * probe succeeds; after that it must be free to perform timed pulses.
 */
static int m20_motor_guard(void *unused)
{
	void __iomem *b = ioremap(M20_GPP0_BASE, 0x100);

	if (!b)
		return -ENOMEM;
	while (!kthread_should_stop()) {
		u32 con, dat;

		if (smp_load_acquire(&m20_motor_owned))
			break;
		con = readl(b + GPP0_CON);
		dat = readl(b + GPP0_DAT);
		if ((dat & BIT(MOT_PIN)) ||
		    ((con >> (MOT_PIN * 4)) & 0xf) != 1) {
			writel(dat & ~BIT(MOT_PIN), b + GPP0_DAT);
			writel((con & ~(0xf << (MOT_PIN * 4))) |
			       (1 << (MOT_PIN * 4)), b + GPP0_CON);
		}
		msleep(100);
	}
	iounmap(b);
	return 0;
}

static int __init m20_motor_guard_init(void)
{
	struct task_struct *task;

	if (!of_machine_is_compatible("samsung,m20lte"))
		return 0;
	task = kthread_run(m20_motor_guard, NULL, "m20motor");
	return IS_ERR(task) ? PTR_ERR(task) : 0;
}
late_initcall(m20_motor_guard_init);

static enum hrtimer_restart m20_vibrator_expire(struct hrtimer *timer)
{
	struct m20_vibrator *vib = container_of(timer, struct m20_vibrator, timer);

	/* Probe rejects sleeping GPIOs: the Exynos bank is MMIO-backed. */
	gpiod_set_value(vib->enable_gpio, 0);
	return HRTIMER_NORESTART;
}

static ssize_t enable_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	struct m20_vibrator *vib = dev_get_drvdata(dev);
	s64 remaining = 0;

	mutex_lock(&vib->lock);
	if (hrtimer_active(&vib->timer))
		remaining = max_t(s64, 0,
				  ktime_to_ms(hrtimer_get_remaining(&vib->timer)));
	mutex_unlock(&vib->lock);
	return sysfs_emit(buf, "%lld\n", remaining);
}

static ssize_t enable_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct m20_vibrator *vib = dev_get_drvdata(dev);
	int timeout, ret;

	ret = kstrtoint(buf, 0, &timeout);
	if (ret)
		return ret;
	timeout = clamp(timeout, 0, M20_MAX_TIMEOUT_MS);

	mutex_lock(&vib->lock);
	if (vib->suspended) {
		mutex_unlock(&vib->lock);
		return -EBUSY;
	}
	/* Drain the old callback before turning the motor on again. */
	hrtimer_cancel(&vib->timer);
	gpiod_set_value(vib->enable_gpio, timeout != 0);
	if (timeout) {
		pm_wakeup_event(vib->dev, timeout + 20);
		hrtimer_start(&vib->timer, ms_to_ktime(timeout), HRTIMER_MODE_REL);
	}
	mutex_unlock(&vib->lock);
	return count;
}
static DEVICE_ATTR_RW(enable);

static struct attribute *m20_vibrator_attrs[] = {
	&dev_attr_enable.attr,
	NULL,
};
ATTRIBUTE_GROUPS(m20_vibrator);

static void m20_vibrator_stop(struct m20_vibrator *vib)
{
	mutex_lock(&vib->lock);
	vib->suspended = true;
	hrtimer_cancel(&vib->timer);
	gpiod_set_value(vib->enable_gpio, 0);
	mutex_unlock(&vib->lock);
}

static int m20_vibrator_probe(struct platform_device *pdev)
{
	struct m20_vibrator *vib;
	int ret;

	vib = devm_kzalloc(&pdev->dev, sizeof(*vib), GFP_KERNEL);
	if (!vib)
		return -ENOMEM;
	vib->dev = &pdev->dev;
	vib->enable_gpio = devm_gpiod_get(&pdev->dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(vib->enable_gpio))
		return PTR_ERR(vib->enable_gpio);
	if (gpiod_cansleep(vib->enable_gpio))
		return -EINVAL;

	mutex_init(&vib->lock);
	hrtimer_init(&vib->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	vib->timer.function = m20_vibrator_expire;
	platform_set_drvdata(pdev, vib);
	ret = device_init_wakeup(&pdev->dev, true);
	if (ret)
		return ret;

	/* No parent: keep the stock /devices/virtual/timed_output/vibrator
	 * path, including its existing SELinux sysfs_vibrator label.
	 */
	vib->sysfs_dev = device_create_with_groups(m20_timed_output_class,
						  NULL, MKDEV(0, 0), vib,
						  m20_vibrator_groups, "vibrator");
	if (IS_ERR(vib->sysfs_dev)) {
		device_init_wakeup(&pdev->dev, false);
		return PTR_ERR(vib->sysfs_dev);
	}
	smp_store_release(&m20_motor_owned, true);
	dev_warn(&pdev->dev, "GPIO vibrator ready (timed_output ABI, max %d ms)\n",
		 M20_MAX_TIMEOUT_MS);
	return 0;
}

static int m20_vibrator_remove(struct platform_device *pdev)
{
	struct m20_vibrator *vib = platform_get_drvdata(pdev);

	device_unregister(vib->sysfs_dev);
	m20_vibrator_stop(vib);
	device_init_wakeup(&pdev->dev, false);
	return 0;
}

static void m20_vibrator_shutdown(struct platform_device *pdev)
{
	m20_vibrator_stop(platform_get_drvdata(pdev));
}

static int __maybe_unused m20_vibrator_suspend(struct device *dev)
{
	m20_vibrator_stop(dev_get_drvdata(dev));
	return 0;
}

static int __maybe_unused m20_vibrator_resume(struct device *dev)
{
	struct m20_vibrator *vib = dev_get_drvdata(dev);

	mutex_lock(&vib->lock);
	vib->suspended = false;
	mutex_unlock(&vib->lock);
	return 0;
}
static SIMPLE_DEV_PM_OPS(m20_vibrator_pm, m20_vibrator_suspend,
			m20_vibrator_resume);

static const struct of_device_id m20_vibrator_match[] = {
	{ .compatible = "samsung,m20lte-vibrator" },
	{ }
};
MODULE_DEVICE_TABLE(of, m20_vibrator_match);

static struct platform_driver m20_vibrator_driver = {
	.probe = m20_vibrator_probe,
	.remove = m20_vibrator_remove,
	.shutdown = m20_vibrator_shutdown,
	.driver = {
		.name = "m20lte-vibrator",
		.of_match_table = m20_vibrator_match,
		.pm = &m20_vibrator_pm,
	},
};

static int __init m20_vibrator_init(void)
{
	int ret;

	if (!of_machine_is_compatible("samsung,m20lte"))
		return 0;
	m20_timed_output_class = class_create(THIS_MODULE, "timed_output");
	if (IS_ERR(m20_timed_output_class))
		return PTR_ERR(m20_timed_output_class);
	ret = platform_driver_register(&m20_vibrator_driver);
	if (ret)
		class_destroy(m20_timed_output_class);
	return ret;
}
device_initcall(m20_vibrator_init);
