// SPDX-License-Identifier: GPL-2.0
/*
 * Exynos7885/7904 (m20lte) CPU idle: WFI and per-core power down (C2).
 *
 * 4.4 cpuidle-exynos64.c + exynos-powermode.c exynos_cpu_pm_enter():
 * clear the core's PMU CLUSTERn_CPUm_CONFIGURATION, then PSCI CPU_SUSPEND
 * with the power-down state 0x10000.  The secure firmware does not program
 * the PMU itself (see the v183 hotplug hang), so the kernel must.
 * The core configuration is restored after every exit so that a later
 * plain WFI cannot power the core down without a saved context.
 * Cluster power down (CPD) and SICD are not implemented.
 */
#include <linux/completion.h>
#include <linux/cpu_pm.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/cpuidle.h>
#include <linux/init.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/psci.h>
#include <linux/regmap.h>
#include <asm/cpuidle.h>
#include <asm/cputype.h>
#include <asm/smp_plat.h>

#define EXYNOS7885_PSCI_CPU_DOWN	0x10000

static bool c2 = true;
module_param(c2, bool, 0444);
MODULE_PARM_DESC(c2, "enable per-core power down idle state");
/*
 * Per cluster (index: 0 boot A53 cpus 0-3, 1 A53 cpus 4-5, 2 A73 cpus 6-7).
 * v204 hung ~10 ms after C2 went live on cpus 0-5 (device SPIs default to
 * cpu0; the A73 canary only proved IPI wake-up).  A73 only until SPI
 * wake-up from C2 is verified.
 */
static bool c2_cluster[3] = { false, false, false };	/* C2 stays off until validated with system sleep */
module_param_array(c2_cluster, bool, NULL, 0444);

extern bool exynos7885_lpm_ready;
static struct regmap *pmu;
static unsigned long c2_count[NR_CPUS], c2_fail[NR_CPUS];
module_param_array_named(c2_fail, c2_fail, ulong, NULL, 0444);
module_param_array_named(c2_count, c2_count, ulong, NULL, 0444);

/* CLUSTERn_CPUm_CONFIGURATION, indexed by 4.4 linear_phycpu(). */
static const u32 cpu_cfg_offs[] = { 0x2000, 0x2080, 0x2200, 0x2280,
				    0x2300, 0x2380, 0x2100, 0x2180 };
static const unsigned int cluster_first_phycpu[] = { 0, 2, 6 };
static int cpu_cfg[NR_CPUS];

static int exynos7885_enter_c2(struct cpuidle_device *dev,
			       struct cpuidle_driver *drv, int index)
{
	u32 off = cpu_cfg[dev->cpu];
	int ret;

	ret = cpu_pm_enter();
	if (ret)
		return -EBUSY;
	regmap_write_bits(pmu, off, 0xf, 0x0);
	ret = psci_cpu_suspend_enter(EXYNOS7885_PSCI_CPU_DOWN);
	regmap_write_bits(pmu, off, 0xf, 0xf);
	cpu_pm_exit();

	if (ret) {
		c2_fail[dev->cpu]++;
		return -EBUSY;
	}
	c2_count[dev->cpu]++;
	return index;
}

static int exynos7885_enter_wfi(struct cpuidle_device *dev,
				struct cpuidle_driver *drv, int index)
{
	cpu_do_idle();
	return index;
}

#define EXYNOS7885_IDLE_DRIVER(_name, _exit, _target)			\
{									\
	.name = _name,							\
	.owner = THIS_MODULE,						\
	.states = {							\
		{							\
			.enter = exynos7885_enter_wfi,			\
			.exit_latency = 1,				\
			.target_residency = 1,				\
			.name = "WFI",					\
			.desc = "ARM WFI",				\
		},							\
		{							\
			.enter = exynos7885_enter_c2,			\
			.exit_latency = _exit,				\
			.target_residency = _target,			\
			.name = "C2",					\
			.desc = "core power down",			\
			.flags = CPUIDLE_FLAG_TIMER_STOP,		\
		},							\
	},								\
	.state_count = 2,						\
}

/* 4.4 DT idle-states: bootcl-cpu-sleep (A53, MPIDR Aff1 = 1) and
 * nobootcl-cpu-sleep (the A73 cluster and the second A53 cluster). */
static struct cpuidle_driver exynos7885_idle_drivers[] = {
	EXYNOS7885_IDLE_DRIVER("exynos7885_bootcl", 35 + 90, 750),
	EXYNOS7885_IDLE_DRIVER("exynos7885_cl2", 30 + 75, 3000),
	EXYNOS7885_IDLE_DRIVER("exynos7885_cl0", 30 + 75, 3000),
};

static struct work_struct canary_work[NR_CPUS];
static struct completion canary_done[NR_CPUS];

static void exynos7885_canary_fn(struct work_struct *w)
{
	complete(&canary_done[smp_processor_id()]);
}

/*
 * Canary: after C2 is live on one cluster, every CPU of it must still run
 * work and must have completed C2 entries.  Otherwise keep C2 off for the
 * remaining clusters (a lost core must not take the whole phone down).
 */
static bool __init exynos7885_c2_canary(const struct cpumask *mask)
{
	unsigned int cpu;
	bool ok = true;

	msleep(2000);
	for_each_cpu_and(cpu, mask, cpu_online_mask) {
		init_completion(&canary_done[cpu]);
		INIT_WORK(&canary_work[cpu], exynos7885_canary_fn);
		schedule_work_on(cpu, &canary_work[cpu]);
		if (!wait_for_completion_timeout(&canary_done[cpu], HZ)) {
			pr_err("m20-cpuidle: CANARY cpu%u did not respond after C2 (count %lu fail %lu)\n",
			       cpu, c2_count[cpu], c2_fail[cpu]);
			ok = false;
			continue;
		}
		pr_warn("m20-cpuidle: canary cpu%u ok, C2 entries %lu fail %lu\n",
			cpu, c2_count[cpu], c2_fail[cpu]);
	}
	return ok;
}

static int __init exynos7885_cpuidle_init(void)
{
	static struct cpumask masks[ARRAY_SIZE(exynos7885_idle_drivers)];
	static const int order[] = { 2, 1, 0 };	/* A73 first as the canary */
	unsigned int cpu, k;
	int ret, i;

	if (!of_machine_is_compatible("samsung,exynos7904") &&
	    !of_machine_is_compatible("samsung,exynos7885"))
		return 0;
	/*
	 * C2 needs the lpm_init CPUn_OPTION wake-up enables; without them a
	 * powered-down core never comes back (v202 boot hang).
	 */
	if (c2 && !smp_load_acquire(&exynos7885_lpm_ready)) {
		pr_err("m20-cpuidle: PMU lpm_init not done, C2 disabled\n");
		c2 = false;
	}
	pmu = syscon_regmap_lookup_by_compatible("samsung,exynos7885-pmu");
	if (IS_ERR(pmu)) {
		pr_err("m20-cpuidle: no PMU regmap: %ld\n", PTR_ERR(pmu));
		return PTR_ERR(pmu);
	}

	for_each_possible_cpu(cpu) {
		u64 mpidr = cpu_logical_map(cpu);
		unsigned int cl = MPIDR_AFFINITY_LEVEL(mpidr, 1);
		unsigned int core = MPIDR_AFFINITY_LEVEL(mpidr, 0);
		unsigned int drv;

		if (cl >= ARRAY_SIZE(cluster_first_phycpu) ||
		    cluster_first_phycpu[cl] + core >= ARRAY_SIZE(cpu_cfg_offs))
			return -EINVAL;
		cpu_cfg[cpu] = cpu_cfg_offs[cluster_first_phycpu[cl] + core];
		drv = cl == 1 ? 0 : cl == 2 ? 1 : 2;
		cpumask_set_cpu(cpu, &masks[drv]);
	}

	for (k = 0; k < ARRAY_SIZE(order); k++) {
		struct cpuidle_driver *drv;

		i = order[k];
		drv = &exynos7885_idle_drivers[i];

		if (cpumask_empty(&masks[i]))
			continue;
		drv->cpumask = &masks[i];
		if (!c2 || !c2_cluster[i])
			drv->state_count = 1;
		ret = cpuidle_register(drv, NULL);
		if (ret) {
			pr_err("m20-cpuidle: %s register failed: %d\n",
			       drv->name, ret);
			return ret;
		}
		pr_warn("m20-cpuidle: %s cpus %*pbl, C2 %s\n", drv->name,
			cpumask_pr_args(&masks[i]),
			drv->state_count > 1 ? "on" : "off");
		if (k == 0 && drv->state_count > 1 &&
		    !exynos7885_c2_canary(&masks[i])) {
			pr_err("m20-cpuidle: C2 canary failed, C2 off elsewhere\n");
			c2 = false;
		}
	}
	return 0;
}
/* After exynos7885-sleep (late_initcall) has run lpm_init. */
late_initcall_sync(exynos7885_cpuidle_init);
