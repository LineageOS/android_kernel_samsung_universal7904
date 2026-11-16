// SPDX-License-Identifier: GPL-2.0-only
/*
 * Exynos7885/7904 TMU monitoring for the 5.10 bring-up.
 * Register layout and calibration follow Samsung's 4.4 exynos78XX TMU.
 * BIG and LITTLE share the CPU sensor on this SoC; G3D has a second TMU.
 * Real temperatures feed passive ACPM cooling at 91 C and independent
 * software/hardware critical trips at 115 C.
 */
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/cpufreq.h>
#include <linux/thermal.h>
#include <soc/samsung/ect_parser.h>

#define TMU_TRIMINFO 0x00
#define TMU_TRIMINFO1 0x04
#define TMU_CONTROL 0x20
#define TMU_CONTROL1 0x24
#define TMU_CURRENT_TEMP 0x40
#define TMU_RISE7_6 0x50
#define TMU_CODE_MASK 0x1ff
#define TMU_CORE_ENABLE BIT(0)
#define TMU_HW_TRIP_ENABLE BIT(12)
#define TMU_CAL_TWO_POINT BIT(23)
#define TMU_FIRST_TEMP 25000
#define TMU_SECOND_TEMP 85000
#define TMU_PASSIVE_TEMP 91000
#define TMU_CRIT_TEMP 115000

struct m20_tmu_sensor {
	void __iomem *base;
	u32 trim;
	int error1;
	int error2;
	bool two_point;
};

#define M20_TMU_MAX_TRIPS 12

/*
 * Per-zone trips.  Without ECT: passive 91 C + critical 115 C (bring-up).
 * With the bootloader ECT "THERMAL" block (4.4 of-thermal.c:552-620): one
 * passive trip per range at its lower bound, each pinning the cluster's
 * cpufreq cooling state for that range's max_frequency, plus 115 C.
 */
struct m20_tmu_zone {
	struct m20_tmu_sensor *sensor;
	int ntrips;
	int temp[M20_TMU_MAX_TRIPS];
	unsigned long state[M20_TMU_MAX_TRIPS];
	unsigned int max_khz[M20_TMU_MAX_TRIPS];
	unsigned int cpu;
	bool ect;
};

struct m20_tmu {
	struct m20_tmu_sensor sensors[2];
	struct m20_tmu_zone zdata[3];
	struct thermal_zone_device *zones[3];
};

static int m20_tmu_get_temp(struct thermal_zone_device *tz, int *temp)
{
	struct m20_tmu_zone *zone = tz->devdata;
	struct m20_tmu_sensor *sensor = zone->sensor;
	int code;

	if (!(readl(sensor->base + TMU_CONTROL) & TMU_CORE_ENABLE))
		return -EAGAIN;
	code = readl(sensor->base + TMU_CURRENT_TEMP) & TMU_CODE_MASK;
	if (!code || code == TMU_CODE_MASK)
		return -EAGAIN;
	if (sensor->two_point)
		*temp = div_s64((s64)(code - sensor->error1) *
				 (TMU_SECOND_TEMP - TMU_FIRST_TEMP),
				 sensor->error2 - sensor->error1) + TMU_FIRST_TEMP;
	else
		*temp = (code - sensor->error1) * 1000 + TMU_FIRST_TEMP;
	if (*temp < -40000 || *temp > 150000)
		return -ERANGE;
	return 0;
}

static int m20_tmu_trip_type(struct thermal_zone_device *tz, int trip,
			     enum thermal_trip_type *type)
{
	struct m20_tmu_zone *zone = tz->devdata;

	if (trip < 0 || trip >= zone->ntrips)
		return -EINVAL;
	*type = trip == zone->ntrips - 1 ? THERMAL_TRIP_CRITICAL :
		THERMAL_TRIP_PASSIVE;
	return 0;
}

static int m20_tmu_trip_temp(struct thermal_zone_device *tz, int trip, int *temp)
{
	struct m20_tmu_zone *zone = tz->devdata;

	if (trip < 0 || trip >= zone->ntrips)
		return -EINVAL;
	*temp = zone->temp[trip];
	return 0;
}

static int m20_tmu_crit_temp(struct thermal_zone_device *tz, int *temp)
{
	*temp = TMU_CRIT_TEMP;
	return 0;
}

static int m20_tmu_trip_hyst(struct thermal_zone_device *tz, int trip, int *hyst)
{
	struct m20_tmu_zone *zone = tz->devdata;

	if (trip < 0 || trip >= zone->ntrips)
		return -EINVAL;
	/* 4.4 M20 DT: 5 C hysteresis on every alert/hot trip. */
	*hyst = trip == zone->ntrips - 1 ? 0 : 5000;
	return 0;
}

static bool m20_tmu_cooling_matches(struct thermal_zone_device *tz,
				    struct thermal_cooling_device *cdev)
{
	const char *name;

	if (!strcmp(tz->type, "BIG"))
		name = "thermal-cpufreq-0";
	else if (!strcmp(tz->type, "LITTLE"))
		name = "thermal-cpufreq-1";
	else
		name = "thermal-gpufreq-0";	/* m20 GPU devfreq max-freq cooling */
	return !strcmp(cdev->type, name);
}

/* cpufreq cooling state for a frequency cap: index in the descending table. */
static unsigned long m20_tmu_cpu_state(unsigned int cpu, unsigned int khz)
{
	struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);
	struct cpufreq_frequency_table *pos;
	unsigned long state = 0, higher = 0;

	if (!policy)
		return 0;
	cpufreq_for_each_valid_entry(pos, policy->freq_table)
		if (pos->frequency > khz)
			higher++;
	state = higher;
	cpufreq_cpu_put(policy);
	return state;
}

static int m20_tmu_bind(struct thermal_zone_device *tz,
			struct thermal_cooling_device *cdev)
{
	struct m20_tmu_zone *zone = tz->devdata;
	unsigned long max, st;
	int ret, i;

	if (!m20_tmu_cooling_matches(tz, cdev))
		return 0;
	ret = cdev->ops->get_max_state(cdev, &max);
	if (ret)
		return ret;
	if (!zone->ect)
		return thermal_zone_bind_cooling_device(tz, 0, cdev, max, 0,
							THERMAL_WEIGHT_DEFAULT);
	for (i = 0; i < zone->ntrips - 1; i++) {
		/* cpufreq is registered by now (it owns this cooling device). */
		zone->state[i] = m20_tmu_cpu_state(zone->cpu, zone->max_khz[i]);
		st = min(zone->state[i], max);
		dev_dbg(&tz->device, "%s trip %d %d C cap %u kHz state %lu\n",
			 tz->type, i, zone->temp[i] / 1000, zone->max_khz[i], st);
		ret = thermal_zone_bind_cooling_device(tz, i, cdev, st, st,
						       THERMAL_WEIGHT_DEFAULT);
		if (ret)
			return ret;
	}
	return 0;
}

static int m20_tmu_unbind(struct thermal_zone_device *tz,
			  struct thermal_cooling_device *cdev)
{
	struct m20_tmu_zone *zone = tz->devdata;
	int i;

	if (!m20_tmu_cooling_matches(tz, cdev))
		return 0;
	for (i = 0; i < (zone->ect ? zone->ntrips - 1 : 1); i++)
		thermal_zone_unbind_cooling_device(tz, i, cdev);
	return 0;
}

static void m20_tmu_zone_setup(struct device *dev, struct m20_tmu_zone *zone,
			       const char *name)
{
	struct ect_ap_thermal_function *fn = NULL;
	void *block;
	char ectname[24];
	int i, n = 0;

	zone->ntrips = 2;
	zone->temp[0] = TMU_PASSIVE_TEMP;
	zone->temp[1] = TMU_CRIT_TEMP;
	zone->cpu = strcmp(name, "BIG") ? 0 : 6;

	block = ect_get_block(BLOCK_AP_THERMAL);
	if (!block || !strcmp(name, "G3D"))
		goto out;
	fn = ect_ap_thermal_get_function(block, (char *)name);
	if (!fn) {
		snprintf(ectname, sizeof(ectname), "%s_THERMAL", name);
		fn = ect_ap_thermal_get_function(block, ectname);
	}
	if (!fn || fn->num_of_range <= 0)
		goto out;
	for (i = 0; i < fn->num_of_range && n < M20_TMU_MAX_TRIPS - 1; i++) {
		struct ect_ap_thermal_range *r = &fn->range_list[i];
		int t = r->lower_bound_temperature * 1000;

		/* Never place a throttling trip at/above the 115 C cutoff. */
		if (t >= TMU_CRIT_TEMP || !r->max_frequency)
			continue;
		zone->temp[n] = t;
		zone->max_khz[n] = r->max_frequency;
		dev_dbg(dev, "%s ECT trip %d: %d C -> %u kHz\n",
			 name, n, r->lower_bound_temperature, r->max_frequency);
		n++;
	}
	if (!n)
		goto out;
	zone->temp[n] = TMU_CRIT_TEMP;
	zone->ntrips = n + 1;
	zone->ect = true;
	return;
out:
	dev_dbg(dev, "%s: %s, single passive trip at %d C\n", name,
		 block ? "no ECT function" : "no ECT", TMU_PASSIVE_TEMP / 1000);
}

static struct thermal_zone_device_ops m20_tmu_ops = {
	.bind = m20_tmu_bind,
	.unbind = m20_tmu_unbind,
	.get_trip_hyst = m20_tmu_trip_hyst,
	.get_temp = m20_tmu_get_temp,
	.get_trip_type = m20_tmu_trip_type,
	.get_trip_temp = m20_tmu_trip_temp,
	.get_crit_temp = m20_tmu_crit_temp,
};

static int m20_tmu_init_sensor(struct platform_device *pdev,
			       struct m20_tmu_sensor *sensor, int index)
{
	u32 control, control1, trim1, value;
	int critical_code;

	sensor->base = devm_platform_ioremap_resource(pdev, index);
	if (IS_ERR(sensor->base))
		return PTR_ERR(sensor->base);
	sensor->trim = readl(sensor->base + TMU_TRIMINFO);
	if (sensor->trim == ~0U)
		return -ENODEV;
	sensor->two_point = !!(sensor->trim & TMU_CAL_TWO_POINT);
	sensor->error1 = sensor->trim & TMU_CODE_MASK;
	sensor->error2 = (sensor->trim >> 9) & TMU_CODE_MASK;
	/* Samsung's board DT supplies 0x13a as the untrimmed fallback. */
	if (!sensor->error1)
		sensor->error1 = 0x13a;
	if (sensor->two_point && sensor->error2 <= sensor->error1)
		return -EINVAL;

	control = readl(sensor->base + TMU_CONTROL);
	if (!(control & TMU_CORE_ENABLE)) {
		/* Same reference voltage (16), gain (8), noise mode (4),
		 * TEM1455X mux (2), PTAT/BUF fuse fields as the 4.4 M20 DT.
		 * Do not cycle an already running firmware TMU off/on.
		 */
		control &= ~((0x1fU << 24) | (0xfU << 8) |
			     (0x7U << 13) | (0x7U << 20));
		control |= (16U << 24) | (8U << 8) | (4U << 13) | (2U << 20);
		trim1 = readl(sensor->base + TMU_TRIMINFO1);
		control1 = readl(sensor->base + TMU_CONTROL1);
		control1 &= ~((0x7U << 20) | (0xfU << 12));
		control1 |= ((sensor->trim >> 18) & 0x7U) << 20;
		control1 |= ((trim1 >> 18) & 0xfU) << 12;
		writel(control1, sensor->base + TMU_CONTROL1);
	}

	/* Level 7 is the hardware thermal trip on the stock board. Keep
	 * the same 115 C cutoff, independently of userspace HAL startup.
	 * Never raise an existing, lower hardware cutoff.
	 */
	if (sensor->two_point)
		critical_code = (TMU_CRIT_TEMP - TMU_FIRST_TEMP) *
			(sensor->error2 - sensor->error1) /
			(TMU_SECOND_TEMP - TMU_FIRST_TEMP) + sensor->error1;
	else
		critical_code = (TMU_CRIT_TEMP - TMU_FIRST_TEMP) / 1000 +
			sensor->error1;
	if (critical_code > TMU_CODE_MASK)
		return -EINVAL;
	value = readl(sensor->base + TMU_RISE7_6);
	if (!(control & TMU_HW_TRIP_ENABLE) ||
	    !((value >> 16) & TMU_CODE_MASK) ||
	    ((value >> 16) & TMU_CODE_MASK) > critical_code) {
		value &= ~(TMU_CODE_MASK << 16);
		value |= critical_code << 16;
		writel(value, sensor->base + TMU_RISE7_6);
	}
	writel(control | TMU_CORE_ENABLE | TMU_HW_TRIP_ENABLE,
	       sensor->base + TMU_CONTROL);
	dev_warn(&pdev->dev, "TMU%d trim=%#x err=%d/%d cal=%s control=%#x raw=%#x\n",
		 index, sensor->trim, sensor->error1, sensor->error2,
		 sensor->two_point ? "two-point" : "one-point",
		 readl(sensor->base + TMU_CONTROL),
		 readl(sensor->base + TMU_CURRENT_TEMP));
	return 0;
}

static int m20_tmu_probe(struct platform_device *pdev)
{
	static const char * const names[] = { "BIG", "LITTLE", "G3D" };
	struct m20_tmu *tmu;
	int i, ret;

	tmu = devm_kzalloc(&pdev->dev, sizeof(*tmu), GFP_KERNEL);
	if (!tmu)
		return -ENOMEM;
	for (i = 0; i < ARRAY_SIZE(tmu->sensors); i++) {
		ret = m20_tmu_init_sensor(pdev, &tmu->sensors[i], i);
		if (ret)
			return ret;
	}
	for (i = 0; i < ARRAY_SIZE(tmu->zones); i++) {
		tmu->zdata[i].sensor = &tmu->sensors[i == 2];
		m20_tmu_zone_setup(&pdev->dev, &tmu->zdata[i], names[i]);
		tmu->zones[i] = thermal_zone_device_register(names[i],
			tmu->zdata[i].ntrips, 0, &tmu->zdata[i], &m20_tmu_ops,
			NULL, 1000, 1000);
		if (IS_ERR(tmu->zones[i])) {
			ret = PTR_ERR(tmu->zones[i]);
			goto unregister;
		}
		thermal_zone_device_enable(tmu->zones[i]);
	}
	platform_set_drvdata(pdev, tmu);
	return 0;
unregister:
	while (i--)
		thermal_zone_device_unregister(tmu->zones[i]);
	return ret;
}

static int m20_tmu_remove(struct platform_device *pdev)
{
	struct m20_tmu *tmu = platform_get_drvdata(pdev);
	int i;

	for (i = 0; i < ARRAY_SIZE(tmu->zones); i++)
		thermal_zone_device_unregister(tmu->zones[i]);
	/* Leave hardware overtemperature protection running. */
	return 0;
}

static const struct of_device_id m20_tmu_match[] = {
	{ .compatible = "samsung,exynos7904-tmu" },
	{ }
};
MODULE_DEVICE_TABLE(of, m20_tmu_match);

static struct platform_driver m20_tmu_driver = {
	.probe = m20_tmu_probe,
	.remove = m20_tmu_remove,
	.driver = {
		.name = "exynos7904-tmu",
		.of_match_table = m20_tmu_match,
	},
};
module_platform_driver(m20_tmu_driver);

MODULE_DESCRIPTION("Exynos7904 TMU monitoring and critical trips");
MODULE_LICENSE("GPL");
