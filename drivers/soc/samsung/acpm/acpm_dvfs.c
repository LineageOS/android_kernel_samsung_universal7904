// SPDX-License-Identifier: GPL-2.0
/*
 * ACPM DVFS requests over the APM mailbox.
 *
 * Port of Samsung 4.4 drivers/soc/samsung/cal-if/acpm_dvfs.c:21-160 (the
 * request/response encoding). Not ported: the TMU cold-temperature notifiers
 * and the MIF-request notification channel (pm_qos), which need the Exynos
 * TMU and devfreq stacks.
 *
 * The id passed to exynos_acpm_set_rate()/exynos_acpm_get_rate() is the index
 * of the domain (ACPM_DVFS_MIF = 0 ... ACPM_DVFS_CP = 9, see
 * dt-bindings/clock/exynos7885.h); callers pass ACPM_DVFS_IDX(ACPM_DVFS_G3D).
 */
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sched/clock.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/thermal.h>
#include <soc/samsung/acpm_dvfs.h>
#include <soc/samsung/acpm_ipc_ctrl.h>

#define ACPM_MAX_LEVELS 64

static unsigned int acpm_dvfs_ch;
static unsigned int acpm_fvp_ch;
static bool acpm_fvp_ready;
static bool acpm_dvfs_ready;
static DEFINE_MUTEX(acpm_dvfs_lock);
/* Last frequency request actually acknowledged by the firmware. */
static unsigned long acpm_ack_rate[10];

static int acpm_dvfs_send(unsigned int id, unsigned int arg, unsigned int op,
			  unsigned int extra, unsigned int *reply)
{
	struct ipc_config config;
	unsigned int cmd[4];
	unsigned long long before, latency;
	int ret;

	if (!acpm_dvfs_ready)
		return -ENODEV;

	config.cmd = cmd;
	config.response = true;
	config.indirection = false;
	cmd[0] = id;
	cmd[1] = arg;
	cmd[2] = op;
	cmd[3] = extra;

	mutex_lock(&acpm_dvfs_lock);
	before = sched_clock();
	ret = acpm_ipc_send_data(acpm_dvfs_ch, &config);
	latency = sched_clock() - before;
	/* Match the stock CAL contract: update the requested state only
	 * after a sequence-matched firmware ACK. The low command word is
	 * not a documented operation-status field (do not invent one).
	 */
	if (!ret && op == ACPM_DVFS_FREQ_REQ && id < ARRAY_SIZE(acpm_ack_rate))
		WRITE_ONCE(acpm_ack_rate[id], arg);
	mutex_unlock(&acpm_dvfs_lock);
	if (ret)
		pr_err("acpm_dvfs: [%u] op %u latency = %llu ret = %d\n",
		       id, op, latency, ret);
	if (reply)
		*reply = cmd[1];

	return ret;
}

int exynos_acpm_set_rate(unsigned int id, unsigned long rate)
{
	return acpm_dvfs_send(id, rate, ACPM_DVFS_FREQ_REQ, 0, NULL);
}
EXPORT_SYMBOL_GPL(exynos_acpm_set_rate);

unsigned long exynos_acpm_get_rate(unsigned int id)
{
	/* This firmware has no working FREQ_GET operation. Report the last
	 * acknowledged SET state, not an invented hardware measurement.
	 * Probe establishes it with a real firmware-OPP request first.
	 */
	if (id >= ARRAY_SIZE(acpm_ack_rate))
		return 0;
	return READ_ONCE(acpm_ack_rate[id]);
}
EXPORT_SYMBOL_GPL(exynos_acpm_get_rate);

/*
 * Minimum-rate votes (stand-in for the 4.4 pm_qos -> exynos-devfreq path).
 * Each voter (display, GPU, ...) asks for a floor in kHz; the domain runs at
 * the lowest FVmap level >= the highest floor, or its boot level when no
 * voter asks for anything.
 */
static unsigned long acpm_votes[10][ACPM_VOTER_MAX];
static bool acpm_votes_frozen;
static DEFINE_MUTEX(acpm_vote_lock);

int exynos_acpm_vote(unsigned int id, unsigned int voter, unsigned long khz)
{
	unsigned int rates[ACPM_MAX_LEVELS];
	unsigned long want = 0, best = 0;
	int n, i, ret = 0;

	if (id >= ARRAY_SIZE(acpm_votes) || voter >= ACPM_VOTER_MAX)
		return -EINVAL;
	n = exynos_acpm_get_rate_table(id, rates, ARRAY_SIZE(rates));
	if (n <= 0)
		return n ? n : -ENODATA;

	mutex_lock(&acpm_vote_lock);
	acpm_votes[id][voter] = khz;
	/* Domains are parked for SoC sleep between suspend_late and
	 * resume_early; a late vote must not raise MIF/INT/DISP again
	 * (v196: MIF above the 4.4 suspend_freq broke long sleep). */
	if (acpm_votes_frozen) {
		mutex_unlock(&acpm_vote_lock);
		return 0;
	}
	for (i = 0; i < ACPM_VOTER_MAX; i++)
		want = max(want, acpm_votes[id][i]);
	if (!want) {
		if (exynos_acpm_get_boot_rate(id, &best))
			ret = -ENODATA;
	} else {
		for (i = 0; i < n; i++)
			if (rates[i] >= want && (!best || rates[i] < best))
				best = rates[i];
		for (i = 0; !best && i < n; i++)
			best = max_t(unsigned long, best, rates[i]);
		if (!best)
			for (i = 0; i < n; i++)
				best = max_t(unsigned long, best, rates[i]);
	}
	if (!ret && exynos_acpm_get_rate(id) != best) {
		pr_debug("acpm dvfs: domain %u vote %u:%lu -> %lu kHz\n",
			 id, voter, khz, best);
		ret = exynos_acpm_set_rate(id, best);
	}
	mutex_unlock(&acpm_vote_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(exynos_acpm_vote);

int exynos_acpm_set_min_rate(unsigned int id, unsigned long khz)
{
	return exynos_acpm_vote(id, ACPM_VOTER_DISPLAY, khz);
}
EXPORT_SYMBOL_GPL(exynos_acpm_set_min_rate);

int exynos_acpm_set_init_freq(unsigned int dfs_id, unsigned long freq)
{
	return acpm_dvfs_send(ACPM_DVFS_IDX(dfs_id), freq, ACPM_DVFS_DATA_INIT,
			      ACPM_DVFS_SET_INIT_FREQ, NULL);
}
EXPORT_SYMBOL_GPL(exynos_acpm_set_init_freq);

int exynos_acpm_set_volt_margin(unsigned int id, int volt)
{
	return acpm_dvfs_send(id, volt, ACPM_DVFS_MARGIN_REQ, 0, NULL);
}
EXPORT_SYMBOL_GPL(exynos_acpm_set_volt_margin);

int exynos_acpm_set_cold_temp(unsigned int id, bool is_cold_temp)
{
	return acpm_dvfs_send(id, is_cold_temp, ACPM_DVFS_COLDTEMP_REQ, 0, NULL);
}
EXPORT_SYMBOL_GPL(exynos_acpm_set_cold_temp);

int exynos_acpm_set_flag(void)
{
	return acpm_dvfs_send(7, 1, ACPM_DVFS_SET_FLAG, 0, NULL);
}
EXPORT_SYMBOL_GPL(exynos_acpm_set_flag);


struct acpm_cooling {
	unsigned int id;
	unsigned int rates[ACPM_MAX_LEVELS];
	unsigned int count;
	struct mutex lock;
};

static int acpm_cooling_max(struct thermal_cooling_device *cdev,
			    unsigned long *state)
{
	struct acpm_cooling *cooling = cdev->devdata;

	*state = cooling->count - 1;
	return 0;
}

static int acpm_cooling_current(struct thermal_cooling_device *cdev,
				unsigned long *state)
{
	struct acpm_cooling *cooling = cdev->devdata;
	unsigned long rate;
	int i, ret = -EIO;

	mutex_lock(&cooling->lock);
	rate = exynos_acpm_get_rate(cooling->id);
	for (i = 0; rate && i < cooling->count; i++) {
		if (rate == cooling->rates[i]) {
			*state = i;
			ret = 0;
			break;
		}
	}
	mutex_unlock(&cooling->lock);
	return ret;
}

static int acpm_cooling_set(struct thermal_cooling_device *cdev,
			    unsigned long state)
{
	struct acpm_cooling *cooling = cdev->devdata;
	unsigned long current_rate;
	int ret = 0;

	if (state >= cooling->count)
		return -EINVAL;
	mutex_lock(&cooling->lock);
	current_rate = exynos_acpm_get_rate(cooling->id);
	if (!current_rate) {
		ret = -EIO;
		goto out;
	}
	if (current_rate == cooling->rates[state])
		goto out;
	/* The firmware owns the matching voltage/clock sequence. All
	 * selectable rates are firmware OPPs at or below the boot rate.
	 */
	ret = exynos_acpm_set_rate(cooling->id, cooling->rates[state]);
	if (!ret && exynos_acpm_get_rate(cooling->id) != cooling->rates[state])
		ret = -EIO;
	if (ret)
		pr_err("acpm cooling: domain %u state %lu failed: %d\n",
		       cooling->id, state, ret);
out:
	mutex_unlock(&cooling->lock);
	return ret;
}

static struct thermal_cooling_device_ops acpm_cooling_ops = {
	.get_max_state = acpm_cooling_max,
	.get_cur_state = acpm_cooling_current,
	.set_cur_state = acpm_cooling_set,
};

static int acpm_rate_descending(const void *a, const void *b)
{
	unsigned int left = *(const unsigned int *)a;
	unsigned int right = *(const unsigned int *)b;

	return (left < right) - (left > right);
}

static void acpm_cooling_unregister(void *data)
{
	thermal_cooling_device_unregister(data);
}

static int acpm_register_cooling(struct platform_device *pdev,
				 unsigned int id, const char *name, bool expose)
{
	struct acpm_cooling *cooling;
	struct thermal_cooling_device *cdev;
	unsigned int all[ACPM_MAX_LEVELS];
	unsigned long boot_rate;
	int i, levels, ret;
	bool found = false;

	levels = exynos_acpm_get_rate_table(id, all, ARRAY_SIZE(all));
	if (levels < 0)
		return levels;
	ret = exynos_acpm_get_boot_rate(id, &boot_rate);
	if (ret)
		return ret;
	for (i = 0; i < levels; i++)
		if (all[i] == boot_rate)
			found = true;
	if (!found) {
		dev_err(&pdev->dev, "domain %u boot rate %lu absent from FVmap\n",
			id, boot_rate);
		return -ERANGE;
	}
	cooling = devm_kzalloc(&pdev->dev, sizeof(*cooling), GFP_KERNEL);
	if (!cooling)
		return -ENOMEM;
	cooling->id = id;
	mutex_init(&cooling->lock);
	sort(all, levels, sizeof(all[0]), acpm_rate_descending, NULL);
	for (i = 0; i < levels; i++) {
		if (all[i] > boot_rate ||
		    (cooling->count &&
		     all[i] == cooling->rates[cooling->count - 1]))
			continue;
		cooling->rates[cooling->count++] = all[i];
	}
	if (!cooling->count)
		return -EINVAL;
	/* Establish a real, acknowledged state using the firmware's own
	 * initial OPP. Never expose a cooling device after a failed SET.
	 */
	ret = exynos_acpm_set_rate(id, boot_rate);
	if (ret)
		return ret;
	/* CPU domains: m20-acpm-cpufreq scales them and registers cpufreq cooling. */
	if (!expose) {
		dev_info(&pdev->dev, "domain %u boot %lu kHz set (cooling via cpufreq)\n",
			 id, boot_rate);
		return 0;
	}
	cdev = thermal_cooling_device_register((char *)name, cooling,
					       &acpm_cooling_ops);
	if (IS_ERR(cdev))
		return PTR_ERR(cdev);
	ret = devm_add_action_or_reset(&pdev->dev, acpm_cooling_unregister, cdev);
	if (ret)
		return ret;
	dev_info(&pdev->dev, "%s: domain %u boot %lu kHz, %u states, min %u kHz\n",
		 name, id, boot_rate, cooling->count,
		 cooling->rates[cooling->count - 1]);
	return 0;
}

/* Separate FVP channel, not the CAL/DVFS channel 0. */
/* 4.4 devfreq suspend/resume: MIF (type 0) and INT (type 1) both send
 * {type, running, DATA_INIT, RELEASE} on ACPM channel 1. */
static int acpm_fvp_running(bool running)
{
	unsigned int type;
	int ret = 0;

	if (!acpm_fvp_ready)
		return 0;
	mutex_lock(&acpm_dvfs_lock);
	for (type = 0; type <= 1 && !ret; type++) {
		unsigned int cmd[4] = { type, running, ACPM_DVFS_DATA_INIT, 2 };
		struct ipc_config config = {
			.cmd = cmd, .response = true, .indirection = false,
		};

		ret = acpm_ipc_send_data(acpm_fvp_ch, &config);
		pr_debug("acpm_dvfs: FVP type %u running=%d ret %d resp %u\n",
			 type, running, ret, cmd[1]);
	}
	mutex_unlock(&acpm_dvfs_lock);
	return ret;
}

static int acpm_dvfs_probe(struct platform_device *pdev)
{
	struct device_node *fvp;
	unsigned int size, rates[ACPM_MAX_LEVELS];
	int ret;

	/* Wait for the ACPM plugin to publish a validated FVmap first. */
	ret = exynos_acpm_get_rate_table(2, rates, ARRAY_SIZE(rates));
	if (ret < 0)
		return ret;
	if (acpm_ipc_request_channel(pdev->dev.of_node, NULL, &acpm_dvfs_ch, &size))
		return -EPROBE_DEFER;
	if (size != 4 * sizeof(unsigned int))
		return -EINVAL;
	/* This dedicated DVFS channel uses polled request/reply messages. */
	ret = acpm_ipc_set_ch_mode(pdev->dev.of_node, true);
	if (ret)
		return ret;
	/* Optional: a missing FVP channel must not take DVFS down. */
	fvp = of_get_child_by_name(pdev->dev.of_node, "fvp-channel");
	if (fvp) {
		unsigned int fsize = 0;
		int r = acpm_ipc_request_channel(fvp, NULL, &acpm_fvp_ch, &fsize);

		if (!r && fsize != 4 * sizeof(unsigned int))
			r = -EINVAL;
		if (!r)
			r = acpm_ipc_set_ch_mode(fvp, true);
		of_node_put(fvp);
		acpm_fvp_ready = !r;
		dev_info(&pdev->dev, "ACPM FVP channel %u: %d\n", acpm_fvp_ch, r);
	}
	acpm_dvfs_ready = true;
	dev_info(&pdev->dev, "ACPM DVFS channel %u\n", acpm_dvfs_ch);
	ret = acpm_register_cooling(pdev, 2, "thermal-cpufreq-0", false);
	if (!ret)
		ret = acpm_register_cooling(pdev, 3, "thermal-cpufreq-1", false);
	if (!ret)
		ret = acpm_register_cooling(pdev, 4, "thermal-gpufreq-0", false);
	if (ret) {
		acpm_dvfs_ready = false;
		dev_err(&pdev->dev, "cooling registration failed: %d\n", ret);
	}
	return ret;
}

/*
 * 4.4 exynos-devfreq put MIF/INT/DISP on their suspend_freq before SoC sleep
 * (exynos7885.dtsi freq_info): the firmware leaves and re-enters DRAM
 * self-refresh at that MIF level. CPU domains are parked by cpufreq.
 */
static const struct {
	unsigned int id;
	unsigned long khz;
} acpm_suspend_freqs[] = {
	{ 0, 420000 },	/* MIF */
	{ 1, 107000 },	/* INT */
	{ 7, 167000 },	/* DISP */
};

static unsigned long acpm_suspend_saved[ARRAY_SIZE(acpm_suspend_freqs)];


static int acpm_dvfs_suspend_late(struct device *dev)
{
	unsigned long cur, boot;
	unsigned int i, id;
	int ret;

	mutex_lock(&acpm_vote_lock);
	acpm_votes_frozen = true;
	mutex_unlock(&acpm_vote_lock);
	/* FVP RELEASE=false announces AP sleep, as in 4.4 devfreq. */
	ret = acpm_fvp_running(false);
	if (ret) {
		/* One of MIF/INT may already be announced asleep: undo it. */
		acpm_fvp_running(true);
		mutex_lock(&acpm_vote_lock);
		acpm_votes_frozen = false;
		mutex_unlock(&acpm_vote_lock);
		return ret;
	}
	for (i = 0; i < ARRAY_SIZE(acpm_suspend_freqs); i++) {
		id = acpm_suspend_freqs[i].id;
		cur = exynos_acpm_get_rate(id);
		if (!cur && !exynos_acpm_get_boot_rate(id, &boot))
			cur = boot;
		acpm_suspend_saved[i] = cur;
		if (exynos_acpm_set_rate(id, acpm_suspend_freqs[i].khz))
			acpm_suspend_saved[i] = 0;
		dev_dbg(dev, "domain %u %lu -> %lu kHz for sleep\n",
			id, cur, exynos_acpm_get_rate(id));
	}
	return 0;
}

static int acpm_dvfs_resume_early(struct device *dev)
{
	unsigned int i;
	int ret;

	ret = acpm_fvp_running(true);
	if (ret)
		dev_err(dev, "FVP RELEASE=true failed: %d\n", ret);
	for (i = 0; i < ARRAY_SIZE(acpm_suspend_freqs); i++)
		if (acpm_suspend_saved[i])
			exynos_acpm_set_rate(acpm_suspend_freqs[i].id,
					     acpm_suspend_saved[i]);
	mutex_lock(&acpm_vote_lock);
	acpm_votes_frozen = false;
	mutex_unlock(&acpm_vote_lock);
	return 0;
}

static const struct dev_pm_ops acpm_dvfs_pm_ops = {
	SET_LATE_SYSTEM_SLEEP_PM_OPS(acpm_dvfs_suspend_late,
				     acpm_dvfs_resume_early)
};

static const struct of_device_id acpm_dvfs_match[] = {
	{ .compatible = "samsung,exynos-acpm-dvfs" },
	{ }
};

static struct platform_driver acpm_dvfs_driver = {
	.probe = acpm_dvfs_probe,
	.driver = {
		.name = "exynos-acpm-dvfs",
		.pm = IS_ENABLED(CONFIG_EXYNOS7885_SLEEP) ? &acpm_dvfs_pm_ops : NULL,
		.of_match_table = acpm_dvfs_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(acpm_dvfs_driver);
