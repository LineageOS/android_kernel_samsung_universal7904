/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ABOX_PORT_H
#define __ABOX_PORT_H

#define ABOX_IS_ERR_VALUE(v) ((long)(v) < 0)

#include <sound/soc.h>

#define SND_SOC_DAPM_DEMUX_E(wname, wreg, wshift, winvert, wcontrols, \
	wevent, wflags) \
{ .id = snd_soc_dapm_demux, .name = wname, \
  SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), \
  .kcontrol_news = wcontrols, .num_kcontrols = 1, \
  .event = wevent, .event_flags = wflags }

#include <linux/device.h>
#include <linux/arm-smccc.h>
#include <linux/clk.h>
#include <linux/iommu.h>
#include <linux/mfd/syscon.h>
#include <linux/pm_wakeup.h>
#include <linux/regmap.h>
#include <linux/sched/clock.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/dma-iommu.h>
#include <asm/barrier.h>

#include <dt-bindings/clock/exynos7885.h>
#include <soc/samsung/acpm_dvfs.h>

/* Keep the legacy wakeup lifetime, using the upstream wakeup-source API. */
struct abox_wake_lock {
	struct wakeup_source *ws;
};
static inline void abox_wake_lock_init(struct abox_wake_lock *lock,
		const char *name)
{
	lock->ws = wakeup_source_register(NULL, name);
}
static inline void abox_wake_lock_timeout(struct abox_wake_lock *lock,
		unsigned long ms)
{
	__pm_wakeup_event(lock->ws, ms);
}
static inline void abox_wake_unlock(struct abox_wake_lock *lock)
{
	__pm_relax(lock->ws);
}

/*
 * Throughput requests for the firmware ABI. Domains with an ACPM DVFS
 * counterpart (AUD, INT, MIF) are applied as ACPM minimum-rate votes, like
 * the 4.4 pm_qos -> exynos-devfreq path; without them the DSP stays at the
 * boot AUD clock while the firmware is told it runs at the requested gear.
 * The CPU classes (lit/big) are only tracked here.
 * The update may sleep: call it from process context only.
 */
struct abox_port_qos_request {
	s32 value;
	bool acpm;
	unsigned int domain;
};
#define ABOX_PORT_QOS_ACPM(dom)	{ .acpm = true, .domain = (dom) }
static inline void abox_port_qos_update(struct abox_port_qos_request *req,
		s32 value)
{
	req->value = value;
	if (req->acpm)
		exynos_acpm_vote(req->domain, ACPM_VOTER_ABOX,
				value > 0 ? value : 0);
}

static inline int abox_smc(unsigned long cmd, unsigned long a1,
		unsigned long a2, unsigned long a3)
{
	struct arm_smccc_res res;

	dsb(sy);
	arm_smccc_smc(cmd, a1, a2, a3, 0, 0, 0, 0, &res);
	return (int)res.a0;
}

static inline struct regmap *abox_pmu(void)
{
	return syscon_regmap_lookup_by_compatible("samsung,exynos7885-pmu");
}
static inline int abox_pmu_read(unsigned int reg, unsigned int *value)
{
	struct regmap *map = abox_pmu();

	return IS_ERR(map) ? PTR_ERR(map) : regmap_read(map, reg, value);
}
static inline int abox_pmu_write(unsigned int reg, unsigned int value)
{
	struct regmap *map = abox_pmu();

	return IS_ERR(map) ? PTR_ERR(map) : regmap_write(map, reg, value);
}
static inline int abox_pmu_update(unsigned int reg, unsigned int mask,
		unsigned int value)
{
	struct regmap *map = abox_pmu();

	return IS_ERR(map) ? PTR_ERR(map) : regmap_update_bits(map, reg, mask, value);
}

static inline int abox_pcm_ioctl(struct snd_soc_component *component,
		struct snd_pcm_substream *substream, unsigned int cmd, void *arg)
{
	return snd_pcm_lib_ioctl(substream, cmd, arg);
}

struct device *abox_dma_device(void);

#endif
