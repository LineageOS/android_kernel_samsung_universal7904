// SPDX-License-Identifier: GPL-2.0
/*
 * m20lte (Exynos7904) Mali-G71 frequency scaling through ACPM.
 *
 * 4.4 scaled G3D with the Samsung gpu_dvfs governor and cal_dfs_set_rate(),
 * i.e. ACPM domain 4, which sets frequency and voltage together. The 5.10
 * port drives the G3D clocks through CCF only for gating, so changing the
 * PLL here would leave the voltage behind; request ACPM levels instead.
 *
 * Levels and the MIF floor per level follow 4.4 exynos7904-mali.dtsi
 * (gpu_dvfs_table, gpu_max_clock 1100000). Load comes from the kbase PM
 * busy/idle metrics, fed to the generic simple_ondemand governor.
 */
#include <linux/delay.h>
#include <linux/sort.h>
#include <linux/devfreq.h>
#include <linux/pm_qos.h>
#include <linux/thermal.h>
#include <linux/pm_opp.h>
#include <linux/workqueue.h>
#include <soc/samsung/acpm_dvfs.h>

#include <mali_kbase.h>
#include <backend/gpu/mali_kbase_pm_internal.h>

#define M20_G3D_DOM	4
#define M20_MIF_DOM	0
#define M20_G3D_MIN	343000
#define M20_G3D_MAX	1100000

/* 4.4 gpu_dvfs_table: GPU kHz -> MIF floor kHz */
static const struct { unsigned int gpu, mif; } m20_g3d_mif[] = {
	{ 1001000, 1794000 },
	{  845000, 1352000 },
	{  545000,  676000 },
	{  450000,  546000 },
	{       0,  420000 },
};

static struct kbase_device *m20_kbdev;
/* OPPs in kHz, ascending; cooling state i caps at m20_opp[m20_nopp - 1 - i]. */
static unsigned int m20_opp[16];
static int m20_nopp;
static struct dev_pm_qos_request m20_max_req;
static struct devfreq *m20_df;
static struct thermal_cooling_device *m20_cdev;
static struct kbasep_pm_metrics m20_last;
static unsigned long m20_cur_khz;
static struct devfreq_simple_ondemand_data m20_ondemand = {
	.upthreshold = 70,
	.downdifferential = 15,
};

static unsigned int m20_mif_floor(unsigned long khz)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(m20_g3d_mif); i++)
		if (khz >= m20_g3d_mif[i].gpu)
			return m20_g3d_mif[i].mif;
	return 0;
}

static int m20_gpu_target(struct device *dev, unsigned long *freq, u32 flags)
{
	struct dev_pm_opp *opp;
	unsigned long khz;
	int ret;

	opp = devfreq_recommended_opp(dev, freq, flags);
	if (IS_ERR(opp))
		return PTR_ERR(opp);
	*freq = dev_pm_opp_get_freq(opp);
	dev_pm_opp_put(opp);
	khz = *freq / 1000;
	if (khz == m20_cur_khz)
		return 0;

	/* Raise the memory floor before speeding up, drop it after slowing. */
	if (khz > m20_cur_khz)
		exynos_acpm_vote(M20_MIF_DOM, ACPM_VOTER_GPU, m20_mif_floor(khz));
	ret = exynos_acpm_set_rate(M20_G3D_DOM, khz);
	if (ret) {
		exynos_acpm_vote(M20_MIF_DOM, ACPM_VOTER_GPU,
				 m20_mif_floor(m20_cur_khz));
		return ret;
	}
	if (khz < m20_cur_khz)
		exynos_acpm_vote(M20_MIF_DOM, ACPM_VOTER_GPU, m20_mif_floor(khz));
	m20_cur_khz = khz;
	return 0;
}

static int m20_gpu_status(struct device *dev, struct devfreq_dev_status *stat)
{
	struct kbasep_pm_metrics diff;

	kbase_pm_get_dvfs_metrics(m20_kbdev, &m20_last, &diff);
	stat->busy_time = diff.time_busy;
	stat->total_time = diff.time_busy + diff.time_idle;
	stat->current_frequency = m20_cur_khz * 1000;
	return 0;
}

static int m20_gpu_cur_freq(struct device *dev, unsigned long *freq)
{
	*freq = m20_cur_khz * 1000;
	return 0;
}

static struct devfreq_dev_profile m20_gpu_profile = {
	.polling_ms	= 50,
	.target		= m20_gpu_target,
	.get_dev_status	= m20_gpu_status,
	.get_cur_freq	= m20_gpu_cur_freq,
};

/*
 * Keep the 4.4/acpm cooling device name: the Pixel thermal HAL config and the
 * TMU G3D zone both look for "thermal-gpufreq-0" (thermal-devfreq-N made the
 * HAL abort and stalled boot).
 */
static unsigned long m20_cool_state;

static int m20_cool_max(struct thermal_cooling_device *cdev, unsigned long *state)
{
	*state = m20_nopp - 1;
	return 0;
}

static int m20_cool_cur(struct thermal_cooling_device *cdev, unsigned long *state)
{
	*state = m20_cool_state;
	return 0;
}

static int m20_cool_set(struct thermal_cooling_device *cdev, unsigned long state)
{
	int ret;

	if (state >= m20_nopp)
		return -EINVAL;
	ret = dev_pm_qos_update_request(&m20_max_req,
					m20_opp[m20_nopp - 1 - state]);
	if (ret < 0)
		return ret;
	m20_cool_state = state;
	return 0;
}

static const struct thermal_cooling_device_ops m20_cool_ops = {
	.get_max_state = m20_cool_max,
	.get_cur_state = m20_cool_cur,
	.set_cur_state = m20_cool_set,
};

static int m20_khz_cmp(const void *a, const void *b)
{
	unsigned int x = *(const unsigned int *)a, y = *(const unsigned int *)b;

	return x < y ? -1 : x > y;
}

/*
 * 4.4 gpu_custom_interface sysfs (kHz): dvfs_table, dvfs_min_lock,
 * dvfs_max_lock.  The device init.rc chowns them; "0" releases a lock,
 * reads print "-1" when unlocked.  Implemented as PM QoS requests on the
 * devfreq device so thermal capping (m20_max_req) still wins.
 */
static struct dev_pm_qos_request m20_user_min_req, m20_user_max_req;
static int m20_user_min_khz, m20_user_max_khz;

static ssize_t dvfs_table_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	ssize_t n = 0;
	int i;

	for (i = m20_nopp - 1; i >= 0; i--)
		n += scnprintf(buf + n, PAGE_SIZE - n, " %u", m20_opp[i]);
	n += scnprintf(buf + n, PAGE_SIZE - n, "\n");
	return n;
}

static unsigned int m20_snap_khz(int khz)
{
	int i;

	/* lowest OPP at or above the request, else the highest */
	for (i = 0; i < m20_nopp; i++)
		if (m20_opp[i] >= khz)
			return m20_opp[i];
	return m20_opp[m20_nopp - 1];
}

static ssize_t m20_lock_show(char *buf, int khz)
{
	if (khz > 0)
		return sprintf(buf, "%u / %d\n", m20_snap_khz(khz), khz);
	return sprintf(buf, "-1\n");
}

static ssize_t m20_lock_store(struct dev_pm_qos_request *req, int *khz_out,
			      s32 unlocked, const char *buf, size_t count)
{
	int khz, ret;

	if (!m20_nopp || !dev_pm_qos_request_active(req))
		return -ENODEV;
	if (kstrtoint(buf, 0, &khz) || khz < 0)
		return -EINVAL;
	*khz_out = khz;
	ret = dev_pm_qos_update_request(req, khz ? m20_snap_khz(khz) : unlocked);
	return ret < 0 ? ret : count;
}

static ssize_t dvfs_min_lock_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	return m20_lock_show(buf, m20_user_min_khz);
}

static ssize_t dvfs_min_lock_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	return m20_lock_store(&m20_user_min_req, &m20_user_min_khz,
			      PM_QOS_MIN_FREQUENCY_DEFAULT_VALUE, buf, count);
}

static ssize_t dvfs_max_lock_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	return m20_lock_show(buf, m20_user_max_khz);
}

static ssize_t dvfs_max_lock_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	return m20_lock_store(&m20_user_max_req, &m20_user_max_khz,
			      PM_QOS_MAX_FREQUENCY_DEFAULT_VALUE, buf, count);
}

static DEVICE_ATTR_RO(dvfs_table);
static DEVICE_ATTR_RW(dvfs_min_lock);
static DEVICE_ATTR_RW(dvfs_max_lock);


static int m20_gpu_devfreq_register(struct kbase_device *kbdev)
{
	struct device *dev = kbdev->dev;
	unsigned int rates[32], max_khz;
	int n, i, opps = 0, ret;

	/* Stock caps G3D at this chip's ECT MINMAX maximum (gpu_max_clock_limit). */
	max_khz = exynos_acpm_get_max_khz("dvfs_g3d");
	if (!max_khz)
		max_khz = M20_G3D_MAX;
	n = exynos_acpm_get_rate_table(M20_G3D_DOM, rates, ARRAY_SIZE(rates));
	if (n <= 0)
		return n ? n : -ENODATA;
	for (i = 0; i < n; i++) {
		if (rates[i] < M20_G3D_MIN || rates[i] > max_khz)
			continue;
		if (opps < ARRAY_SIZE(m20_opp) &&
		    !dev_pm_opp_add(dev, rates[i] * 1000UL, 0))
			m20_opp[opps++] = rates[i];
	}
	if (!opps)
		return -EINVAL;
	sort(m20_opp, opps, sizeof(m20_opp[0]), m20_khz_cmp, NULL);
	m20_nopp = opps;

	m20_cur_khz = exynos_acpm_get_rate(M20_G3D_DOM);
	if (!m20_cur_khz)
		m20_cur_khz = M20_G3D_MIN;
	exynos_acpm_vote(M20_MIF_DOM, ACPM_VOTER_GPU, m20_mif_floor(m20_cur_khz));
	m20_gpu_profile.initial_freq = m20_cur_khz * 1000;
	kbase_pm_get_dvfs_metrics(kbdev, &m20_last, &(struct kbasep_pm_metrics){});

	m20_df = devfreq_add_device(dev, &m20_gpu_profile,
				    DEVFREQ_GOV_SIMPLE_ONDEMAND, &m20_ondemand);
	if (IS_ERR(m20_df)) {
		ret = PTR_ERR(m20_df);
		m20_df = NULL;
		dev_pm_opp_remove_all_dynamic(dev);
		return ret;
	}
	ret = dev_pm_qos_add_request(dev, &m20_max_req, DEV_PM_QOS_MAX_FREQUENCY,
				     PM_QOS_MAX_FREQUENCY_DEFAULT_VALUE);
	if (ret < 0) {
		dev_err(dev, "m20 gpu devfreq: cooling registration failed\n");
	} else {
		m20_cdev = thermal_cooling_device_register("thermal-gpufreq-0",
							   NULL, &m20_cool_ops);
		if (IS_ERR(m20_cdev)) {
			dev_err(dev, "m20 gpu devfreq: cooling registration failed\n");
			m20_cdev = NULL;
		}
	}
	{
		int r1 = dev_pm_qos_add_request(dev, &m20_user_min_req,
						DEV_PM_QOS_MIN_FREQUENCY,
						PM_QOS_MIN_FREQUENCY_DEFAULT_VALUE);
		int r2 = dev_pm_qos_add_request(dev, &m20_user_max_req,
						DEV_PM_QOS_MAX_FREQUENCY,
						PM_QOS_MAX_FREQUENCY_DEFAULT_VALUE);
		int r3 = device_create_file(dev, &dev_attr_dvfs_table);
		int r4 = device_create_file(dev, &dev_attr_dvfs_min_lock);
		int r5 = device_create_file(dev, &dev_attr_dvfs_max_lock);

		if (r1 < 0 || r2 < 0 || r3 || r4 || r5)
			dev_err(dev, "m20 gpu devfreq: dvfs lock sysfs qos %d/%d files %d/%d/%d\n",
				r1, r2, r3, r4, r5);
	}
	dev_warn(dev, "m20 gpu devfreq: %d levels %u..%u kHz via ACPM, now %lu kHz\n",
		 opps, M20_G3D_MIN, max_khz, m20_cur_khz);
	return 0;
}

static void m20_gpu_devfreq_work(struct work_struct *work);
static DECLARE_DELAYED_WORK(m20_gpu_work, m20_gpu_devfreq_work);
static int m20_gpu_tries;

static void m20_gpu_devfreq_work(struct work_struct *work)
{
	int ret;

	/* kbase sets drvdata at the very end of a successful probe. */
	if (dev_get_drvdata(m20_kbdev->dev) != m20_kbdev) {
		if (++m20_gpu_tries < 30)
			schedule_delayed_work(&m20_gpu_work, HZ);
		return;
	}
	ret = m20_gpu_devfreq_register(m20_kbdev);
	if (ret)
		dev_err(m20_kbdev->dev, "m20 gpu devfreq: disabled (%d)\n", ret);
}

void m20_gpu_devfreq_start(struct kbase_device *kbdev)
{
	m20_kbdev = kbdev;
	schedule_delayed_work(&m20_gpu_work, 2 * HZ);
}

/*
 * kbase probe failure or unbind: the delayed work, the devfreq device, the
 * QoS requests and the sysfs files reference kbdev, which is freed next.
 */
void m20_gpu_devfreq_stop(struct kbase_device *kbdev)
{
	struct device *dev;

	if (m20_kbdev != kbdev)
		return;
	cancel_delayed_work_sync(&m20_gpu_work);
	dev = kbdev->dev;
	if (m20_df) {
		device_remove_file(dev, &dev_attr_dvfs_table);
		device_remove_file(dev, &dev_attr_dvfs_min_lock);
		device_remove_file(dev, &dev_attr_dvfs_max_lock);
		if (m20_cdev)
			thermal_cooling_device_unregister(m20_cdev);
		m20_cdev = NULL;
		if (dev_pm_qos_request_active(&m20_user_min_req))
			dev_pm_qos_remove_request(&m20_user_min_req);
		if (dev_pm_qos_request_active(&m20_user_max_req))
			dev_pm_qos_remove_request(&m20_user_max_req);
		if (dev_pm_qos_request_active(&m20_max_req))
			dev_pm_qos_remove_request(&m20_max_req);
		devfreq_remove_device(m20_df);
		m20_df = NULL;
		dev_pm_opp_remove_all_dynamic(dev);
	}
	m20_kbdev = NULL;
}
