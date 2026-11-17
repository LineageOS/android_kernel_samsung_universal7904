// SPDX-License-Identifier: GPL-2.0
/*
 * Exynos7904 (m20lte) cpufreq on top of ACPM DVFS requests.
 *
 * 4.4 exynos-cpufreq drove two domains through cal-if/ACPM:
 *   CPUs 0-5 (Cortex-A53) -> ACPM_DVFS_CPUCL1 (domain 3), min-freq 449000
 *   CPUs 6-7 (Cortex-A73) -> ACPM_DVFS_CPUCL0 (domain 2), min-freq 936000
 * The OPPs come from the firmware FVmap. The FVmap is shared with Exynos7885
 * and lists frequencies above the 7904 SKU limits, so the table is capped
 * at the 7904 maxima (A53 1586 MHz, A73 1768 MHz). The firmware applies the
 * matching voltage for each request.
 */
#include <linux/cpu.h>
#include <linux/cpu_cooling.h>
#include <linux/cpufreq.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/pm_qos.h>
#include <linux/sort.h>
#include <linux/workqueue.h>
#include <soc/samsung/acpm_dvfs.h>

#define M20_MAX_LEVELS	32

/* 4.4 M20 DT cpufreq/domain@N/dm-constraints/mif-perf: {cpu kHz, MIF kHz}. */
static const unsigned int m20_mif_cl1[][2] = {
	{ 1690000, 1014000 }, { 1586000, 1014000 }, { 1482000, 1014000 },
	{ 1352000, 1014000 }, { 1248000, 1014000 }, { 1144000, 845000 },
	{ 1014000, 845000 }, { 902000, 676000 }, { 839000, 676000 },
	{ 757000, 546000 }, { 676000, 546000 }, { 546000, 420000 },
	{ 449000, 420000 }, { 343000, 420000 }, { 208000, 420000 },
};
static const unsigned int m20_mif_cl0[][2] = {
	{ 2184000, 1794000 }, { 2080000, 1794000 }, { 1976000, 1539000 },
	{ 1872000, 1539000 }, { 1768000, 1352000 }, { 1664000, 1014000 },
	{ 1560000, 845000 }, { 1352000, 845000 }, { 1144000, 676000 },
	{ 936000, 546000 }, { 728000, 420000 },
};
/* 4.4 devfreq_mif freq_info min: never vote MIF below this. */
#define M20_MIF_FLOOR_KHZ	676000

struct m20_cluster {
	unsigned int dom;
	unsigned int voter;
	const unsigned int (*mif)[2];
	unsigned int mif_len;
	unsigned int first_cpu, last_cpu;
	unsigned int min_khz, max_khz;
	struct cpufreq_frequency_table *table;
	/* 4.4 interactive-governor compatibility (powerhint.json, init rc). */
	struct kobject boost_kobj;
	struct freq_qos_request boost_req;
	struct delayed_work boost_end;
	unsigned int hispeed_khz, boost_us, io_is_busy;
	bool boost_ready;
};

static struct m20_cluster m20_clusters[] = {
	{ .dom = 3, .first_cpu = 0, .last_cpu = 5,
	  .min_khz = 449000, .max_khz = 1586000,
	  .voter = ACPM_VOTER_CPUCL1, .mif = m20_mif_cl1,
	  .mif_len = ARRAY_SIZE(m20_mif_cl1) },
	{ .dom = 2, .first_cpu = 6, .last_cpu = 7,
	  .min_khz = 936000, .max_khz = 1768000,
	  .voter = ACPM_VOTER_CPUCL0, .mif = m20_mif_cl0,
	  .mif_len = ARRAY_SIZE(m20_mif_cl0) },
};

static int m20_rate_cmp(const void *a, const void *b)
{
	unsigned int x = *(const unsigned int *)a, y = *(const unsigned int *)b;

	return x < y ? -1 : x > y;
}

static struct m20_cluster *m20_cluster_of(unsigned int cpu)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(m20_clusters); i++)
		if (cpu >= m20_clusters[i].first_cpu &&
		    cpu <= m20_clusters[i].last_cpu)
			return &m20_clusters[i];
	return NULL;
}

static int m20_build_table(struct m20_cluster *cl)
{
	unsigned int rates[M20_MAX_LEVELS];
	int levels, i, n = 0;

	if (cl->table)
		return 0;
	levels = exynos_acpm_get_rate_table(cl->dom, rates, ARRAY_SIZE(rates));
	if (levels <= 0)
		return levels ? levels : -ENODATA;
	sort(rates, levels, sizeof(rates[0]), m20_rate_cmp, NULL);

	cl->table = kcalloc(levels + 1, sizeof(*cl->table), GFP_KERNEL);
	if (!cl->table)
		return -ENOMEM;
	for (i = 0; i < levels; i++) {
		if (rates[i] < cl->min_khz || rates[i] > cl->max_khz)
			continue;
		if (n && cl->table[n - 1].frequency == rates[i])
			continue;
		cl->table[n].driver_data = n;
		cl->table[n++].frequency = rates[i];
	}
	cl->table[n].frequency = CPUFREQ_TABLE_END;
	if (!n) {
		kfree(cl->table);
		cl->table = NULL;
		return -EINVAL;
	}
	pr_info("m20-cpufreq: domain %u cpus %u-%u: %d OPPs %u..%u kHz\n",
		cl->dom, cl->first_cpu, cl->last_cpu, n,
		cl->table[0].frequency, cl->table[n - 1].frequency);
	return 0;
}

static unsigned int m20_suspend_khz(struct m20_cluster *cl)
{
	unsigned long boot = 0;
	unsigned int best = 0;
	int i;

	exynos_acpm_get_boot_rate(cl->dom, &boot);
	for (i = 0; cl->table[i].frequency != CPUFREQ_TABLE_END; i++)
		if (cl->table[i].frequency <= boot || !best)
			best = cl->table[i].frequency;
	return best;
}

/*
 * cpufreq/interactive/{boostpulse,boostpulse_duration,hispeed_freq,io_is_busy}
 * as the 4.4 interactive governor exposed them.  The power HAL writes
 * boostpulse on every touch interaction: hold a minimum-frequency QoS
 * request at hispeed_freq for boostpulse_duration (default 80 ms, the
 * interactive DEFAULT_MIN_SAMPLE_TIME).  schedutil keeps governing.
 */
struct m20_boost_attr {
	struct attribute attr;
	ssize_t (*show)(struct m20_cluster *cl, char *buf);
	ssize_t (*store)(struct m20_cluster *cl, const char *buf, size_t n);
};

static void m20_boost_end(struct work_struct *work)
{
	struct m20_cluster *cl = container_of(to_delayed_work(work),
					      struct m20_cluster, boost_end);

	freq_qos_update_request(&cl->boost_req, FREQ_QOS_MIN_DEFAULT_VALUE);
}

/*
 * libperfmgr writes "1" when an INTERACTION hint starts and the default "0"
 * when it ends (powerhint.json CPUBoostPulse only targets cpu0).  4.4 shared
 * interactive tunables across policies, so boost both clusters and hold the
 * boost until "0", bounded by boostpulse_duration (default 3 s here).
 */
static ssize_t m20_boostpulse_store(struct m20_cluster *cl, const char *buf,
				    size_t n)
{
	unsigned int v;
	int i;

	if (kstrtouint(buf, 0, &v))
		return -EINVAL;
	for (i = 0; i < 2; i++) {
		struct m20_cluster *c = &m20_clusters[i];

		if (!c->boost_ready)
			continue;
		if (!v) {
			mod_delayed_work(system_highpri_wq, &c->boost_end, 0);
			continue;
		}
		freq_qos_update_request(&c->boost_req, c->hispeed_khz);
		mod_delayed_work(system_highpri_wq, &c->boost_end,
				 usecs_to_jiffies(cl->boost_us));
	}
	return n;
}

#define M20_UINT_ATTR(_name, _field, _min, _max)				\
static ssize_t m20_##_name##_show(struct m20_cluster *cl, char *buf)	\
{									\
	return sprintf(buf, "%u\n", cl->_field);			\
}									\
static ssize_t m20_##_name##_store(struct m20_cluster *cl,		\
				   const char *buf, size_t n)		\
{									\
	unsigned int v;							\
									\
	if (kstrtouint(buf, 0, &v) || v < (_min) || v > (_max))		\
		return -EINVAL;						\
	cl->_field = v;							\
	return n;							\
}									\
static struct m20_boost_attr m20_attr_##_name =				\
	__ATTR(_name, 0644, m20_##_name##_show, m20_##_name##_store)

M20_UINT_ATTR(boostpulse_duration, boost_us, 1000, 5000000);
M20_UINT_ATTR(io_is_busy, io_is_busy, 0, 1);

static ssize_t m20_hispeed_freq_show(struct m20_cluster *cl, char *buf)
{
	return sprintf(buf, "%u\n", cl->hispeed_khz);
}

static ssize_t m20_hispeed_freq_store(struct m20_cluster *cl,
				      const char *buf, size_t n)
{
	unsigned int v;

	if (kstrtouint(buf, 0, &v) || v < cl->min_khz || v > cl->max_khz)
		return -EINVAL;
	cl->hispeed_khz = v;
	return n;
}

static struct m20_boost_attr m20_attr_hispeed_freq =
	__ATTR(hispeed_freq, 0644, m20_hispeed_freq_show,
	       m20_hispeed_freq_store);
static struct m20_boost_attr m20_attr_boostpulse =
	__ATTR(boostpulse, 0200, NULL, m20_boostpulse_store);

static struct attribute *m20_boost_attrs[] = {
	&m20_attr_boostpulse.attr,
	&m20_attr_boostpulse_duration.attr,
	&m20_attr_hispeed_freq.attr,
	&m20_attr_io_is_busy.attr,
	NULL
};
ATTRIBUTE_GROUPS(m20_boost);

static ssize_t m20_boost_show(struct kobject *k, struct attribute *a, char *b)
{
	struct m20_boost_attr *at = container_of(a, struct m20_boost_attr, attr);

	return at->show ? at->show(container_of(k, struct m20_cluster,
						boost_kobj), b) : -EIO;
}

static ssize_t m20_boost_store(struct kobject *k, struct attribute *a,
			       const char *b, size_t n)
{
	struct m20_boost_attr *at = container_of(a, struct m20_boost_attr, attr);

	return at->store ? at->store(container_of(k, struct m20_cluster,
						  boost_kobj), b, n) : -EIO;
}

static const struct sysfs_ops m20_boost_ops = {
	.show = m20_boost_show,
	.store = m20_boost_store,
};

static struct kobj_type m20_boost_ktype = {
	.sysfs_ops = &m20_boost_ops,
	.default_groups = m20_boost_groups,
};

static void m20_boost_init(struct cpufreq_policy *policy, struct m20_cluster *cl)
{
	int ret;

	if (cl->boost_ready)
		return;
	cl->hispeed_khz = cl->table[0].frequency;
	{
		int i;

		for (i = 0; cl->table[i].frequency != CPUFREQ_TABLE_END; i++)
			cl->hispeed_khz = cl->table[i].frequency;
	}
	cl->boost_us = 3000000;
	INIT_DELAYED_WORK(&cl->boost_end, m20_boost_end);
	ret = freq_qos_add_request(&policy->constraints, &cl->boost_req,
				   FREQ_QOS_MIN, FREQ_QOS_MIN_DEFAULT_VALUE);
	if (ret < 0) {
		pr_err("m20-cpufreq: boost QoS failed: %d\n", ret);
		return;
	}
	ret = kobject_init_and_add(&cl->boost_kobj, &m20_boost_ktype,
				   &policy->kobj, "interactive");
	if (ret) {
		pr_err("m20-cpufreq: interactive sysfs failed: %d\n", ret);
		kobject_put(&cl->boost_kobj);
		freq_qos_remove_request(&cl->boost_req);
		return;
	}
	cl->boost_ready = true;
}

static int m20_cpufreq_init(struct cpufreq_policy *policy)
{
	struct m20_cluster *cl = m20_cluster_of(policy->cpu);
	unsigned int cpu;
	int ret;

	if (!cl)
		return -ENODEV;
	ret = m20_build_table(cl);
	if (ret)
		return ret;
	for (cpu = cl->first_cpu; cpu <= cl->last_cpu; cpu++)
		cpumask_set_cpu(cpu, policy->cpus);
	policy->driver_data = cl;
	policy->freq_table = cl->table;
	/* ACPM requests are polled IPC round trips well under 1 ms. */
	policy->cpuinfo.transition_latency = 500 * NSEC_PER_USEC;
	policy->transition_delay_us = 4000;
	policy->cur = exynos_acpm_get_rate(cl->dom);
	/*
	 * 4.4 exynos-acme pinned each domain to its resume frequency before
	 * SoC sleep: the firmware wake-up path sets the rails for that OPP.
	 * Use the firmware's initial level, capped to this SKU's table.
	 */
	policy->suspend_freq = m20_suspend_khz(cl);
	pr_warn("m20-cpufreq: domain %u suspend freq %u kHz\n", cl->dom,
		policy->suspend_freq);
	return 0;
}

static int m20_cpufreq_target_index(struct cpufreq_policy *policy,
				    unsigned int index)
{
	struct m20_cluster *cl = policy->driver_data;
	unsigned int khz = cl->table[index].frequency, mif = 0, i;
	int ret;

	ret = exynos_acpm_set_rate(cl->dom, khz);
	if (ret)
		return ret;
	/* 4.4 exynos-dm CONSTRAINT_MIN: first row whose CPU rate <= target. */
	for (i = 0; i < cl->mif_len; i++)
		if (cl->mif[i][0] <= khz) {
			mif = cl->mif[i][1];
			break;
		}
	if (!mif && cl->mif_len)
		mif = cl->mif[cl->mif_len - 1][1];
	exynos_acpm_vote(0, cl->voter, max_t(unsigned int, mif,
					      M20_MIF_FLOOR_KHZ));
	return 0;
}

static unsigned int m20_cpufreq_get(unsigned int cpu)
{
	struct m20_cluster *cl = m20_cluster_of(cpu);

	/* Last rate the firmware acknowledged; FREQ_GET returns nothing usable. */
	return cl ? exynos_acpm_get_rate(cl->dom) : 0;
}

static struct cpufreq_driver m20_cpufreq_driver = {
	.name		= "m20-acpm",
	.flags		= CPUFREQ_HAVE_GOVERNOR_PER_POLICY |
			  CPUFREQ_NEED_INITIAL_FREQ_CHECK,
	.init		= m20_cpufreq_init,
	.verify		= cpufreq_generic_frequency_table_verify,
	.target_index	= m20_cpufreq_target_index,
	.get		= m20_cpufreq_get,
#ifdef CONFIG_EXYNOS7885_SLEEP
	.suspend	= cpufreq_generic_suspend,
#endif
	.attr		= cpufreq_generic_attr,
};

static void m20_register_cooling(unsigned int cpu)
{
	struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);
	struct thermal_cooling_device *cdev;

	if (!policy)
		return;
	cdev = cpufreq_cooling_register(policy);
	if (IS_ERR(cdev))
		pr_err("m20-cpufreq: cooling for cpu%u failed: %ld\n",
		       cpu, PTR_ERR(cdev));
	cpufreq_cpu_put(policy);
}

static int __init m20_cpufreq_register(void)
{
	int ret;

	if (!of_machine_is_compatible("samsung,exynos7904"))
		return 0;
	/* acpm_dvfs set and acknowledged both boot OPPs if it is ready. */
	if (!exynos_acpm_get_rate(2) || !exynos_acpm_get_rate(3)) {
		pr_err("m20-cpufreq: ACPM DVFS not ready, no cpufreq\n");
		return -ENODEV;
	}
	ret = cpufreq_register_driver(&m20_cpufreq_driver);
	if (ret) {
		pr_err("m20-cpufreq: register failed: %d\n", ret);
		return ret;
	}
	{
		unsigned int cpus[] = { 0, 6 };
		int i;

		for (i = 0; i < ARRAY_SIZE(cpus); i++) {
			struct cpufreq_policy *policy = cpufreq_cpu_get(cpus[i]);

			if (!policy)
				continue;
			m20_boost_init(policy, policy->driver_data);
			cpufreq_cpu_put(policy);
		}
	}
	/* Same names as before for the TMU zones: BIG first, then LITTLE. */
	m20_register_cooling(6);
	m20_register_cooling(0);
	return 0;
}
late_initcall_sync(m20_cpufreq_register);
