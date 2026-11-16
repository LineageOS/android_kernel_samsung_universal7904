
/*
 * rtc-s2mp.c
 *
 * Copyright (c) 2016 Samsung Electronics Co., Ltd
 *              http://www.samsung.com
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 *  2013-12-11  Performance improvements and code clean up by
 *		Minsung Kim <ms925.kim@samsung.com>
 *
 */
#include <linux/module.h>
#include <linux/gpio.h>
#include <linux/rtc.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/regmap.h>
#include <linux/platform_device.h>
#include <linux/mfd/samsung/rtc-s2mp.h>
#include <linux/mfd/samsung/s2mpu08.h>
#include <linux/mfd/samsung/s2mpu08-private.h>
#include <linux/mfd/samsung/core.h>
#include <linux/mfd/samsung/irq.h>
#include <linux/mfd/samsung/s2mpu08.h>
#include <linux/regmap.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/ktime.h>
#include <linux/rtc.h>
#include <linux/mfd/samsung/rtc-s2mp.h>

/*#define CONFIG_WEEKDAY_ALARM_ENABLE*/

struct s2m_rtc_info {
	struct device		*dev;
	struct regmap		*regmap;	/* RTC address space */
	struct regmap		*regmap_pmic;	/* PMIC address space */
	struct sec_pmic_dev	*sec;
	struct s2m_rtc_pdata	*pdata;
	struct rtc_device	*rtc_dev;
	struct mutex		lock;
	struct work_struct	irq_work;
	int			irq;
	bool			use_irq;
	bool			wtsr_en;
	bool			smpl_en;
	bool			alarm_enabled;
	u8			update_reg;
	bool			use_alarm_workaround;
	bool			alarm_check;
	u8			wudr_mask;
	u8			audr_mask;
#ifdef CONFIG_SEC_PM
	struct delayed_work	rtc_check_work;
#endif
};

/* Board settings for the RTC, read from the PMIC DT node (4.4 s2mpu08 pdata). */
struct s2m_rtc_wtsr {
	bool wtsr_en;
	bool smpl_en;
	bool check_jigon;
	u32 wtsr_timer_val;
	u32 smpl_timer_val;
};

struct s2m_rtc_pdata {
	int osc_bias_up;
	int cap_sel;
	int osc_xin;
	int osc_xout;
	struct s2m_rtc_wtsr *wtsr_smpl;
	struct rtc_time *init_time;	/* first-boot time from DT: not ported yet */
};

static struct s2m_rtc_pdata *s2m_rtc_parse_dt(struct device *parent)
{
	struct device_node *np = parent->of_node;
	struct s2m_rtc_pdata *pdata;
	const char *status;
	u32 val;

	pdata = devm_kzalloc(parent, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return ERR_PTR(-ENOMEM);

	pdata->wtsr_smpl = devm_kzalloc(parent, sizeof(*pdata->wtsr_smpl), GFP_KERNEL);
	if (!pdata->wtsr_smpl)
		return ERR_PTR(-ENOMEM);

	pdata->osc_bias_up = -1;
	pdata->cap_sel = -1;
	pdata->osc_xin = -1;
	pdata->osc_xout = -1;
	if (!of_property_read_u32(np, "osc_bias_up", &val))
		pdata->osc_bias_up = val;
	if (!of_property_read_u32(np, "cap_sel", &val))
		pdata->cap_sel = val;
	if (!of_property_read_u32(np, "osc_xin", &val))
		pdata->osc_xin = val;
	if (!of_property_read_u32(np, "osc_xout", &val))
		pdata->osc_xout = val;

	status = of_get_property(np, "wtsr_en", NULL);
	pdata->wtsr_smpl->wtsr_en = status && (!strcmp(status, "enabled") || !strcmp(status, "okay"));
	status = of_get_property(np, "smpl_en", NULL);
	pdata->wtsr_smpl->smpl_en = status && (!strcmp(status, "enabled") || !strcmp(status, "okay"));
	if (!of_property_read_u32(np, "wtsr_timer_val", &val))
		pdata->wtsr_smpl->wtsr_timer_val = val;
	if (!of_property_read_u32(np, "smpl_timer_val", &val))
		pdata->wtsr_smpl->smpl_timer_val = val;
	if (!of_property_read_u32(np, "check_jigon", &val))
		pdata->wtsr_smpl->check_jigon = !!val;

	return pdata;
}

static struct wakeup_source *rtc_ws;

#ifdef CONFIG_SEC_PM
static int rtc_status = 0;

static int rtc_check;
static u8 rtc_check_data[NR_RTC_CNT_REGS];
static bool rtc_check_first = true;
#endif

enum S2M_RTC_OP {
	S2M_RTC_READ,
	S2M_RTC_WRITE_TIME,
	S2M_RTC_WRITE_ALARM,
};

static void s2m_data_to_tm(u8 *data, struct rtc_time *tm)
{
//	tm->tm_msec = (data[RTC_MSEC] & 0x0f) + (data[RTC_MSEC] & 0xf0) * 10;
	tm->tm_sec = data[RTC_SEC] & 0x7f;
	tm->tm_min = data[RTC_MIN] & 0x7f;
	tm->tm_hour = data[RTC_HOUR] & 0x1f;
	tm->tm_wday = __fls(data[RTC_WEEKDAY] & 0x7f);
	tm->tm_mday = data[RTC_DATE] & 0x1f;
	tm->tm_mon = (data[RTC_MONTH] & 0x0f) - 1;
	tm->tm_year = (data[RTC_YEAR] & 0x7f) + 100;
	tm->tm_yday = 0;
	tm->tm_isdst = 0;
}

#ifdef CONFIG_RTC_HIGH_RES
static void s2m_data_to_hrtm(u8 *data, struct rtc_hrtime *tm)
{
	tm->tm_msec = (data[RTC_HR_MSEC] & 0x7f) * 10;
	tm->tm_sec = data[RTC_HR_SEC] & 0x7f;
	tm->tm_min = data[RTC_HR_MIN] & 0x7f;
	tm->tm_hour = data[RTC_HR_HOUR] & 0x1f;
	tm->tm_wday = __fls(data[RTC_HR_WEEKDAY] & 0x7f);
	tm->tm_mday = data[RTC_HR_DATE] & 0x1f;
	tm->tm_mon = (data[RTC_HR_MONTH] & 0x0f) - 1;
	tm->tm_year = (data[RTC_HR_YEAR] & 0x7f) + 100;
	tm->tm_yday = 0;
	tm->tm_isdst = 0;
}
#endif /* CONFIG_RTC_HIGH_RES */

static int s2m_tm_to_data(struct rtc_time *tm, u8 *data)
{
//	data[RTC_MSEC] = ((tm->tm_msec / 10) << 4) | (tm->tm_msec % 10);
	data[RTC_SEC] = tm->tm_sec;
	data[RTC_MIN] = tm->tm_min;

	if (tm->tm_hour >= 12)
		data[RTC_HOUR] = tm->tm_hour | HOUR_PM_MASK;
	else
		data[RTC_HOUR] = tm->tm_hour;

	data[RTC_WEEKDAY] = BIT(tm->tm_wday);
	data[RTC_DATE] = tm->tm_mday;
	data[RTC_MONTH] = tm->tm_mon + 1;
	data[RTC_YEAR] = tm->tm_year > 100 ? (tm->tm_year - 100) : 0 ;

	if (tm->tm_year < 100) {
		pr_warn("%s: SEC RTC cannot handle the year %d\n", __func__,
				1900 + tm->tm_year);
		return -EINVAL;
	}
	return 0;
}

/* Register access: the RTC address space and the PMIC address space. */
static int s2m_read(struct s2m_rtc_info *info, unsigned int reg, u8 *dest)
{
	unsigned int v;
	int ret;

	ret = regmap_read(info->regmap, reg, &v);
	if (!ret)
		*dest = v & 0xff;
	return ret;
}

static int s2m_read_pmic(struct s2m_rtc_info *info, unsigned int reg, u8 *dest)
{
	unsigned int v;
	int ret;

	ret = regmap_read(info->regmap_pmic, reg, &v);
	if (!ret)
		*dest = v & 0xff;
	return ret;
}

static int s2m_write(struct s2m_rtc_info *info, unsigned int reg, u8 value)
{
	return regmap_write(info->regmap, reg, value);
}

static int s2m_bulk_read(struct s2m_rtc_info *info, unsigned int reg, int count, u8 *buf)
{
	return regmap_bulk_read(info->regmap, reg, buf, count);
}

static int s2m_bulk_write(struct s2m_rtc_info *info, unsigned int reg, int count, u8 *buf)
{
	return regmap_bulk_write(info->regmap, reg, buf, count);
}

static int s2m_update(struct s2m_rtc_info *info, unsigned int reg, u8 val, u8 mask)
{
	return regmap_update_bits(info->regmap, reg, mask, val);
}

static int s2m_rtc_update(struct s2m_rtc_info *info,
				 enum S2M_RTC_OP op)
{
	u8 data;
	int ret;

	if (!info || !info->regmap) {
		pr_err("%s: Invalid argument\n", __func__);
		return -EINVAL;
	}

	switch (op) {
	case S2M_RTC_READ:
		data = RTC_RUDR_MASK;
		break;
	case S2M_RTC_WRITE_TIME:
		data = RTC_WUDR_MASK_REV;
		break;
	case S2M_RTC_WRITE_ALARM:
		data = RTC_AUDR_MASK_REV;
		break;
	default:
		dev_err(info->dev, "%s: invalid op(%d)\n", __func__, op);
		return -EINVAL;
	}

	data |= info->update_reg;

	ret = s2m_write(info, S2MP_RTC_REG_UPDATE, data);
	if (ret < 0)
		dev_err(info->dev, "%s: fail to write update reg(%d,%u)\n",
				__func__, ret, data);
	else
		usleep_range(1000, 1000);

	return ret;
}

static int s2m_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct s2m_rtc_info *info = dev_get_drvdata(dev);
	u8 data[NR_RTC_CNT_REGS];
	int ret;

	mutex_lock(&info->lock);
	ret = s2m_rtc_update(info, S2M_RTC_READ);
	if (ret < 0)
		goto out;

	ret = s2m_bulk_read(info, S2MP_RTC_REG_SEC, NR_RTC_CNT_REGS,
			data);
	if (ret < 0) {
		dev_err(info->dev, "%s: fail to read time reg(%d)\n", __func__,
			ret);
		goto out;
	}

	dev_info(info->dev, "%s: %d-%02d-%02d %02d:%02d:%02d(0x%02x)%s\n",
			__func__, data[RTC_YEAR] + 2000, data[RTC_MONTH],
			data[RTC_DATE], data[RTC_HOUR] & 0x1f, data[RTC_MIN],
			data[RTC_SEC], data[RTC_WEEKDAY],
			data[RTC_HOUR] & HOUR_PM_MASK ? "PM" : "AM");

	s2m_data_to_tm(data, tm);
	ret = rtc_valid_tm(tm);
out:
	mutex_unlock(&info->lock);
	return ret;
}

#ifdef CONFIG_RTC_HIGH_RES
static int s2m_rtc_read_hrtime(struct device *dev, struct rtc_hrtime *tm)
{
	struct s2m_rtc_info *info = dev_get_drvdata(dev);
	u8 data[NR_RTC_HR_CNT_REGS];
	int ret;

	mutex_lock(&info->lock);
	ret = s2m_rtc_update(info, S2M_RTC_READ);
	if (ret < 0)
		goto out;

	ret = s2m_bulk_read(info, S2MP_RTC_REG_MSEC,
			NR_RTC_HR_CNT_REGS, data);
	if (ret < 0) {
		dev_err(info->dev, "%s: fail to read hrtime reg(%d)\n",
				__func__, ret);
		goto out;
	}

	pr_info("%s: RTC_MSEC: 0x%02X\n", __func__, data[RTC_HR_MSEC]);

	dev_info(info->dev, "%s: %d-%02d-%02d %02d:%02d:%02d.%03d(0x%02x)%s\n",
			__func__, data[RTC_HR_YEAR] + 2000, data[RTC_HR_MONTH],
			data[RTC_HR_DATE], data[RTC_HR_HOUR] & 0x1f,
			data[RTC_HR_MIN], data[RTC_HR_SEC],
			(data[RTC_HR_MSEC] & 0x7f) * 10, data[RTC_HR_WEEKDAY],
			data[RTC_HR_HOUR] & HOUR_PM_MASK ? "PM" : "AM");

	s2m_data_to_hrtm(data, tm);
	ret = rtc_valid_hrtm(tm);
out:
	mutex_unlock(&info->lock);
	return ret;
}
#endif /* CONFIG_RTC_HIGH_RES */

static int s2m_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct s2m_rtc_info *info = dev_get_drvdata(dev);
	u8 data[NR_RTC_CNT_REGS];
	int ret;

	ret = s2m_tm_to_data(tm, data);
	if (ret < 0)
		return ret;

	dev_info(info->dev, "%s: %d-%02d-%02d %02d:%02d:%02d(0x%02x)%s\n",
			__func__, data[RTC_YEAR] + 2000, data[RTC_MONTH],
			data[RTC_DATE], data[RTC_HOUR] & 0x1f, data[RTC_MIN],
			data[RTC_SEC], data[RTC_WEEKDAY],
			data[RTC_HOUR] & HOUR_PM_MASK ? "PM" : "AM");

	mutex_lock(&info->lock);
	ret = s2m_bulk_write(info, S2MP_RTC_REG_SEC, NR_RTC_CNT_REGS,
			data);
	if (ret < 0) {
		dev_err(info->dev, "%s: fail to write time reg(%d)\n", __func__,
			ret);
		goto out;
	}

	ret = s2m_rtc_update(info, S2M_RTC_WRITE_TIME);
out:
	mutex_unlock(&info->lock);
	return ret;
}

/* This is a workaround for the problem that RTC TIME is overwirted by write
 * buffer when setting RTC ALARM. It is quite rare but it does happen. The root
 * cuase is that clear signal of RUDR & WUDR is 1 clock delay while it should be
 * 2 clock delay.
 */
static int s2m_rtc_check_rtc_time(struct s2m_rtc_info *info)
{
	u8 data[NR_RTC_CNT_REGS];
	struct rtc_time tm;
	struct timespec64 sys_time;
	time64_t rtc_time;
	int ret;

	/* Read RTC TIME */
	ret = s2m_rtc_update(info, S2M_RTC_READ);
	if (ret < 0)
		goto out;

	ret = s2m_bulk_read(info, S2MP_RTC_REG_SEC, NR_RTC_CNT_REGS,
			data);
	if (ret < 0) {
		dev_err(info->dev, "%s: fail to read time reg(%d)\n", __func__,
			ret);
		goto out;
	}

	/* Get system time */
	ktime_get_real_ts64(&sys_time);

	/* Convert RTC TIME to seconds since 01-01-1970 00:00:00. */
	s2m_data_to_tm(data, &tm);
	rtc_time = rtc_tm_to_time64(&tm);

	if (abs(rtc_time - sys_time.tv_sec) > 2) {
		/* Set RTC TIME */
		rtc_time64_to_tm(sys_time.tv_sec, &tm);
		ret = s2m_tm_to_data(&tm, data);
		if (ret < 0) {
			dev_err(info->dev, "%s: fail to tm_to_data(%d)\n",
					__func__, ret);
			goto out;
		}

		ret = s2m_bulk_write(info, S2MP_RTC_REG_SEC,
				NR_RTC_CNT_REGS, data);
		if (ret < 0) {
			dev_err(info->dev, "%s: fail to write time reg(%d)\n",
					__func__, ret);
			goto out;
		}

		ret = s2m_rtc_update(info, S2M_RTC_WRITE_TIME);

		dev_warn(info->dev, "%s: adjust RTC TIME: sys_time: %lu, "
				"rtc_time: %lu\n", __func__, sys_time.tv_sec,
				rtc_time);

		dev_info(info->dev, "%s: %d-%02d-%02d %02d:%02d:%02d(0x%02x)%s\n",
				__func__, data[RTC_YEAR] + 2000, data[RTC_MONTH],
				data[RTC_DATE], data[RTC_HOUR] & 0x1f, data[RTC_MIN],
				data[RTC_SEC], data[RTC_WEEKDAY],
				data[RTC_HOUR] & HOUR_PM_MASK ? "PM" : "AM");
	}
out:
	return ret;
}

static int s2m_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct s2m_rtc_info *info = dev_get_drvdata(dev);
	u8 data[NR_RTC_CNT_REGS];
	u8 reg, val;
	int ret;

	mutex_lock(&info->lock);
	ret = s2m_rtc_update(info, S2M_RTC_READ);
	if (ret < 0)
		goto out;

	ret = s2m_bulk_read(info, S2MP_RTC_REG_A0SEC, NR_RTC_CNT_REGS,
			data);
	if (ret < 0) {
		dev_err(info->dev, "%s:%d fail to read alarm reg(%d)\n",
			__func__, __LINE__, ret);
		goto out;
	}

	s2m_data_to_tm(data, &alrm->time);

	dev_info(info->dev, "%s: %d-%02d-%02d %02d:%02d:%02d(%d)\n", __func__,
			alrm->time.tm_year + 1900, alrm->time.tm_mon + 1,
			alrm->time.tm_mday, alrm->time.tm_hour,
			alrm->time.tm_min, alrm->time.tm_sec,
			alrm->time.tm_wday);

	alrm->enabled = info->alarm_enabled;
	alrm->pending = 0;

	reg = S2MPU08_PMIC_REG_STATUS2;

	ret = s2m_read_pmic(info, reg, &val); /* i2c for PM */
	if (ret < 0) {
		dev_err(info->dev, "%s:%d fail to read STATUS2 reg(%d)\n",
			__func__, __LINE__, ret);
		goto out;
	}

	if (val & RTCA0E)
		alrm->pending = 1;
out:
	mutex_unlock(&info->lock);
	return ret;
}

static int s2m_rtc_set_alarm_enable(struct s2m_rtc_info *info, bool enabled)
{
	if (!info->use_irq)
		return -EPERM;

	if (enabled && !info->alarm_enabled) {
		info->alarm_enabled = true;
		enable_irq(info->irq);
	} else if (!enabled && info->alarm_enabled) {
		info->alarm_enabled = false;
		disable_irq(info->irq);
	}
	return 0;
}

static int s2m_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct s2m_rtc_info *info = dev_get_drvdata(dev);
	u8 data[NR_RTC_CNT_REGS];
	int ret, i;

	mutex_lock(&info->lock);
	ret = s2m_tm_to_data(&alrm->time, data);
	if (ret < 0)
		goto out;

	dev_info(info->dev, "%s: %d-%02d-%02d %02d:%02d:%02d(0x%02x)%s\n",
			__func__, data[RTC_YEAR] + 2000, data[RTC_MONTH],
			data[RTC_DATE], data[RTC_HOUR] & 0x1f, data[RTC_MIN],
			data[RTC_SEC], data[RTC_WEEKDAY],
			data[RTC_HOUR] & HOUR_PM_MASK ? "PM" : "AM");

	if (info->alarm_check) {
		for (i = 0; i < NR_RTC_CNT_REGS; i++)
			data[i] &= ~ALARM_ENABLE_MASK;

		ret = s2m_bulk_write(info, S2MP_RTC_REG_A0SEC, NR_RTC_CNT_REGS,
				data);
		if (ret < 0) {
			dev_err(info->dev, "%s: fail to disable alarm reg(%d)\n",
				__func__, ret);
			goto out;
		}

		ret = s2m_rtc_update(info, S2M_RTC_WRITE_ALARM);
		if (ret < 0)
			goto out;
	}

	for (i = 0; i < NR_RTC_CNT_REGS; i++)
		data[i] |= ALARM_ENABLE_MASK;

	ret = s2m_bulk_write(info, S2MP_RTC_REG_A0SEC, NR_RTC_CNT_REGS,
			data);
	if (ret < 0) {
		dev_err(info->dev, "%s: fail to write alarm reg(%d)\n",
			__func__, ret);
		goto out;
	}

	ret = s2m_rtc_update(info, S2M_RTC_WRITE_ALARM);
	if (ret < 0)
		goto out;

	if (info->use_alarm_workaround) {
		ret = s2m_rtc_check_rtc_time(info);
		if (ret < 0)
			goto out;
	}

	ret = s2m_rtc_set_alarm_enable(info, alrm->enabled);
out:
	mutex_unlock(&info->lock);
	return ret;
}

static int s2m_rtc_alarm_irq_enable(struct device *dev,
					unsigned int enabled)
{
	struct s2m_rtc_info *info = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&info->lock);
	ret = s2m_rtc_set_alarm_enable(info, enabled);
	mutex_unlock(&info->lock);
	return ret;
}

static irqreturn_t s2m_rtc_alarm_irq(int irq, void *data)
{
	struct s2m_rtc_info *info = data;

	if (!info->rtc_dev)
		return IRQ_HANDLED;

	dev_info(info->dev, "%s:irq(%d)\n", __func__, irq);

	rtc_update_irq(info->rtc_dev, 1, RTC_IRQF | RTC_AF);
	__pm_wakeup_event(rtc_ws, 500);
	return IRQ_HANDLED;
}

static const struct rtc_class_ops s2m_rtc_ops = {
	.read_time = s2m_rtc_read_time,
#ifdef CONFIG_RTC_HIGH_RES
	.read_hrtime = s2m_rtc_read_hrtime,
#endif
	.set_time = s2m_rtc_set_time,
	.read_alarm = s2m_rtc_read_alarm,
	.set_alarm = s2m_rtc_set_alarm,
	.alarm_irq_enable = s2m_rtc_alarm_irq_enable,
};

static void s2m_rtc_optimize_osc(struct s2m_rtc_info *info,
						struct s2m_rtc_pdata *pdata)
{
	int ret = 0;

	/* edit option for OSC_BIAS_UP */
	if (pdata->osc_bias_up >= 0) {
		ret = s2m_update(info, S2MP_RTC_REG_CAPSEL,
			pdata->osc_bias_up << OSC_BIAS_UP_SHIFT,
			OSC_BIAS_UP_MASK);
		if (ret < 0) {
			dev_err(info->dev, "%s: fail to write OSC_BIAS_UP(%d)\n",
				__func__, pdata->osc_bias_up);
			return;
		}
	}

	/* edit option for CAP_SEL */
	if (pdata->cap_sel >= 0) {
		s2m_update(info, S2MP_RTC_REG_CAPSEL,
			pdata->cap_sel << CAP_SEL_SHIFT, CAP_SEL_MASK);
		if (ret < 0) {
			dev_err(info->dev, "%s: fail to write CAP_SEL(%d)\n",
			__func__, pdata->cap_sel);
			return;
		}
	}

	/* edit option for OSC_CTRL */
	if (pdata->osc_xin >= 0) {
		s2m_update(info, S2MP_RTC_REG_OSCCTRL,
			pdata->osc_xin << OSC_XIN_SHIFT, OSC_XIN_MASK);
		if (ret < 0) {
			dev_err(info->dev, "%s: fail to write OSC_CTRL(%d)\n",
			__func__,  pdata->osc_xin);
			return;
		}
	}
	if (pdata->osc_xout >= 0) {
		s2m_update(info, S2MP_RTC_REG_OSCCTRL,
			pdata->osc_xout << OSC_XOUT_SHIFT, OSC_XOUT_MASK);
		if (ret < 0) {
			dev_err(info->dev, "%s: fail to write OSC_CTRL(%d)\n",
			__func__, pdata->osc_xout);
			return;
		}
	}
}

static bool s2m_is_jigonb_low(struct s2m_rtc_info *info)
{
	int ret, reg;
	u8 val, mask;

	reg = S2MPU08_PMIC_REG_STATUS1;
	mask = BIT(1);

	ret = s2m_read(info, reg, &val);
	if (ret < 0) {
		dev_err(info->dev, "%s: fail to read status1 reg(%d)\n",
			__func__, ret);
		return false;
	}

	return !(val & mask);
}

static void s2m_rtc_enable_wtsr_smpl(struct s2m_rtc_info *info,
						struct s2m_rtc_pdata *pdata)
{
	u8 val;
	int ret;

	if (pdata->wtsr_smpl->check_jigon && s2m_is_jigonb_low(info))
		pdata->wtsr_smpl->smpl_en = false;

	val = (pdata->wtsr_smpl->wtsr_en << WTSR_EN_SHIFT)
		| (pdata->wtsr_smpl->smpl_en << SMPL_EN_SHIFT)
		| WTSR_TIMER_BITS(pdata->wtsr_smpl->wtsr_timer_val)
		| SMPL_TIMER_BITS(pdata->wtsr_smpl->smpl_timer_val);

	dev_info(info->dev, "%s: WTSR: %s, SMPL: %s\n", __func__,
			pdata->wtsr_smpl->wtsr_en ? "enable" : "disable",
			pdata->wtsr_smpl->smpl_en ? "enable" : "disable");

	ret = s2m_write(info, S2MP_RTC_REG_WTSR_SMPL, val);
	if (ret < 0) {
		dev_err(info->dev, "%s: fail to write WTSR/SMPL reg(%d)\n",
				__func__, ret);
		return;
	}
	info->wtsr_en = pdata->wtsr_smpl->wtsr_en;
	info->smpl_en = pdata->wtsr_smpl->smpl_en;
}

static void s2m_rtc_disable_wtsr_smpl(struct s2m_rtc_info *info,
					struct s2m_rtc_pdata *pdata)
{
	int ret;

	dev_info(info->dev, "%s: disable WTSR\n", __func__);
	ret = s2m_update(info, S2MP_RTC_REG_WTSR_SMPL, 0,
			WTSR_EN_MASK | SMPL_EN_MASK);
	if (ret < 0)
		dev_err(info->dev, "%s: fail to update WTSR reg(%d)\n",
				__func__, ret);
}

static int s2m_rtc_init_reg(struct s2m_rtc_info *info,
				struct s2m_rtc_pdata *pdata)
{
	u8 data, update_val, ctrl_val, capsel_val;
	int ret;

	ret = s2m_read(info, S2MP_RTC_REG_UPDATE, &update_val);
	if (ret < 0) {
		dev_err(info->dev, "%s: fail to read update reg(%d)\n",
			__func__, ret);
		return ret;
	}

	info->update_reg =
		update_val & ~(info->wudr_mask | RTC_FREEZE_MASK | RTC_RUDR_MASK | info->audr_mask);

	ret = s2m_write(info, S2MP_RTC_REG_UPDATE, info->update_reg);
	if (ret < 0) {
		dev_err(info->dev, "%s: fail to write update reg(%d)\n",
			__func__, ret);
		return ret;
	}

	s2m_rtc_update(info, S2M_RTC_READ);

	ret = s2m_read(info, S2MP_RTC_REG_CTRL, &ctrl_val);
	if (ret < 0) {
		dev_err(info->dev, "%s: fail to read control reg(%d)\n",
			__func__, ret);
		return ret;
	}

	ret = s2m_read(info, S2MP_RTC_REG_CAPSEL, &capsel_val);
	if (ret < 0) {
		dev_err(info->dev, "%s: fail to read cap_sel reg(%d)\n",
			__func__, ret);
		return ret;
	}

	/* If the value of RTC_CTRL register is 0, RTC registers were reset */
	if ((ctrl_val & MODEL24_MASK) && ((capsel_val & 0xf0) == 0xf0))
		return 0;

#ifdef CONFIG_SEC_PM
	/* If RTC registers were reset, set 1 to rtc_status */
	rtc_status = 1;
#endif

	/* Set RTC control register : Binary mode, 24hour mode */
	data = MODEL24_MASK;
	ret = s2m_write(info, S2MP_RTC_REG_CTRL, data);
	if (ret < 0) {
		dev_err(info->dev, "%s: fail to write CTRL reg(%d)\n",
			__func__, ret);
		return ret;
	}

	ret = s2m_rtc_update(info, S2M_RTC_WRITE_ALARM);
	if (ret < 0)
		return ret;

	capsel_val |= 0xf0;
	ret = s2m_write(info, S2MP_RTC_REG_CAPSEL, capsel_val);
	if (ret < 0) {
		dev_err(info->dev, "%s: fail to write CAP_SEL reg(%d)\n",
				__func__, ret);
		return ret;
	}

	if (pdata->init_time) {
		dev_info(info->dev, "%s: initialize RTC time\n", __func__);
		ret = s2m_rtc_set_time(info->dev, pdata->init_time);
	}
	else {
		dev_info(info->dev, "%s: RTC initialize is not operated: "
				"This causes a weekday problem\n", __func__);
	}
	return ret;
}

#ifdef CONFIG_SEC_PM
static ssize_t show_rtc_status(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	int count = 0;

	/* Return rtc status
	 * 0: Keep rtc registers, 1: Reset rtc registers
	 */
	count += sprintf(&buf[count], "%d\n", rtc_status);
	rtc_status = 0;

	return count;
}

static DEVICE_ATTR(rtc_status, 0444, show_rtc_status, NULL);

static void rtc_check_thread(struct work_struct *work)
{
	struct s2m_rtc_info *info = container_of(work, struct s2m_rtc_info,
			rtc_check_work.work);
	int i;
	u8 data[NR_RTC_CNT_REGS];
	int ret;

	mutex_lock(&info->lock);
	ret = s2m_rtc_update(info, S2M_RTC_READ);
	if (ret < 0)
		goto out;

	ret = s2m_bulk_read(info, S2MP_RTC_REG_SEC,
			NR_RTC_CNT_REGS, data);

	if (ret < 0) {
		dev_err(info->dev, "%s: fail to read time reg(%d)\n", __func__,
				ret);
		goto out;
	}

	if (rtc_check_first) {
		for (i = 0; i < NR_RTC_CNT_REGS; i++)
			rtc_check_data[i] = data[i];

		rtc_check_first = false;

		schedule_delayed_work(&info->rtc_check_work,
				msecs_to_jiffies(15000));
	} else {
		for (i = 0; i < NR_RTC_CNT_REGS; i++) {
			if (rtc_check_data[i] != data[i]) {
				rtc_check = 1;
				goto out;
			}
		}

		rtc_check = -1;
	}

out:
	mutex_unlock(&info->lock);
}

static ssize_t show_rtc_check(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", rtc_check);
}

static DEVICE_ATTR(rtc_check, 0444, show_rtc_check, NULL);
#endif

static int s2m_rtc_probe(struct platform_device *pdev)
{
	struct sec_pmic_dev *sec = dev_get_drvdata(pdev->dev.parent);
	struct s2m_rtc_info *info;
	struct s2m_rtc_pdata *pdata;
	int ret = 0;

	info = devm_kzalloc(&pdev->dev, sizeof(struct s2m_rtc_info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	pdata = s2m_rtc_parse_dt(sec->dev);
	if (IS_ERR(pdata))
		return PTR_ERR(pdata);

	mutex_init(&info->lock);
	info->dev = &pdev->dev;
	info->sec = sec;
	info->regmap = sec->regmap_rtc;
	info->regmap_pmic = sec->regmap_pmic;
	info->pdata = pdata;
	info->alarm_check = true;
	info->use_alarm_workaround = false;

	info->wudr_mask = RTC_WUDR_MASK_REV;
	info->audr_mask = RTC_AUDR_MASK_REV;

	/* RTC alarm is S2MPU08_PMIC_IRQ_RTCA0_INT2 in the PMIC IRQ domain. */
	info->irq = regmap_irq_get_virq(sec->irq_data, S2MPU08_PMIC_IRQ_RTCA0_INT2);
	if (info->irq <= 0) {
		dev_err(&pdev->dev, "no RTC alarm virq\n");
		ret = -ENODEV;
		goto err_rtc_irq;
	}

	platform_set_drvdata(pdev, info);

	ret = s2m_rtc_init_reg(info, pdata);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to initialize RTC reg:%d\n", ret);
		goto err_rtc_init_reg;
	}

	/* enable wtsrm smpl */
	if (pdata->wtsr_smpl)
		s2m_rtc_enable_wtsr_smpl(info, pdata);

	s2m_rtc_optimize_osc(info, pdata);

	device_init_wakeup(&pdev->dev, true);
	rtc_ws = wakeup_source_register(&pdev->dev, "rtc-s2mp");

	ret = devm_request_threaded_irq(&pdev->dev, info->irq, NULL,
			s2m_rtc_alarm_irq, 0, "rtc-alarm0", info);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to request alarm IRQ: %d: %d\n",
			info->irq, ret);
		goto err_rtc_irq;
	}
	disable_irq(info->irq);
	info->use_irq = true;

	info->rtc_dev = devm_rtc_device_register(&pdev->dev, "s2mp-rtc",
			&s2m_rtc_ops, THIS_MODULE);
	if (IS_ERR(info->rtc_dev)) {
		ret = PTR_ERR(info->rtc_dev);
		dev_err(&pdev->dev, "Failed to register RTC device: %d\n", ret);
		goto err_rtc_dev_register;
	}
	/*
	 * The alarm IRQ stays disabled until an alarm is set:
	 * s2m_rtc_set_alarm_enable() pairs enable/disable with alarm_enabled.
	 */

	return 0;

err_rtc_dev_register:
	devm_free_irq(&pdev->dev, info->irq, info);
err_rtc_irq:
	wakeup_source_unregister(rtc_ws);
err_rtc_init_reg:
	platform_set_drvdata(pdev, NULL);
	mutex_destroy(&info->lock);

	return ret;
}

static int s2m_rtc_remove(struct platform_device *pdev)
{
	struct s2m_rtc_info *info = platform_get_drvdata(pdev);

	if (!info->alarm_enabled)
		enable_irq(info->irq);

	wakeup_source_unregister(rtc_ws);

	return 0;
}

static void s2m_rtc_shutdown(struct platform_device *pdev)
{
	/*disable wtsr, smpl */
	struct s2m_rtc_info *info = platform_get_drvdata(pdev);
	struct s2m_rtc_pdata *pdata = info->pdata;

	if (info->wtsr_en || info->smpl_en)
		s2m_rtc_disable_wtsr_smpl(info, pdata);
}

static const struct platform_device_id s2m_rtc_id[] = {
	{ "s2mpu08-rtc", 0 },
};

static struct platform_driver s2m_rtc_driver = {
	.driver		= {
		.name	= "s2mpu08-rtc",
		.owner	= THIS_MODULE,
	},
	.probe		= s2m_rtc_probe,
	.remove		= s2m_rtc_remove,
	.shutdown	= s2m_rtc_shutdown,
	.id_table	= s2m_rtc_id,
};

module_platform_driver(s2m_rtc_driver);

/* Module information */
MODULE_DESCRIPTION("Samsung RTC driver");
MODULE_AUTHOR("Samsung Electronics");
MODULE_LICENSE("GPL");
