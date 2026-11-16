// SPDX-License-Identifier: GPL-2.0
/* Samsung Exynos7885/7904 CP sequences; tables from the vendor 4.4 cal-if.
 * No initcall: CP power and reset are exclusively userspace-requested.
 */
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/smc.h>
#include <soc/samsung/cal-if.h>
#include <soc/samsung/exynos-pmu.h>
#include <soc/samsung/pmu-cp.h>

#include "pmucal-cp.h"
#include "exynos7885-cp-data.h"

static int cp_read(u32 offset, u32 *value)
{
	u32 lo, hi, reg;

	if (offset != EXYNOS_PMU_CP_CTRL_NS && offset != EXYNOS_PMU_CP_CTRL_S)
		return exynos_pmu_read(offset, value);
	reg = offset == EXYNOS_PMU_CP_CTRL_S ? CP_CTRL_S : CP_CTRL_NS;
	lo = exynos_smc(SMC_ID, READ_CTRL, 0, reg);
	if (lo & 0xffff)
		return -EIO;
	hi = exynos_smc(SMC_ID, READ_CTRL, 1, reg);
	if (hi & 0xffff)
		return -EIO;
	*value = (hi & 0xffff0000) | (lo >> 16);
	return 0;
}

static int cp_write(struct pmucal_seq *s)
{
	u32 value, reg;
	int ret;

	if (s->offset != EXYNOS_PMU_CP_CTRL_NS && s->offset != EXYNOS_PMU_CP_CTRL_S)
		return exynos_pmu_update(s->offset, s->mask, s->value);
	ret = cp_read(s->offset, &value);
	if (ret)
		return ret;
	value = (value & ~s->mask) | (s->value & s->mask);
	reg = s->offset == EXYNOS_PMU_CP_CTRL_S ? CP_CTRL_S : CP_CTRL_NS;
	return exynos_smc(SMC_ID, WRITE_CTRL, value, reg) ? -EIO : 0;
}

static int cp_run(struct pmucal_seq *seq, unsigned int count)
{
	unsigned int i, retry;
	u32 value;
	int ret;

	for (i = 0; i < count; i++) {
		struct pmucal_seq *s = &seq[i];

		if (s->base_pa != 0x11c80000)
			return -EINVAL;
		switch (s->access_type) {
		case PMUCAL_WRITE:
			ret = cp_write(s);
			break;
		case PMUCAL_READ:
			ret = cp_read(s->offset, &value);
			if (!ret)
				s->value = value & s->mask;
			break;
		case PMUCAL_WAIT:
			/* Vendor RAE polls every microsecond, bounded at 1001us. */
			for (retry = 0; retry <= 1000; retry++) {
				ret = cp_read(s->offset, &value);
				if (ret || (value & s->mask) == s->value)
					break;
				udelay(1);
			}
			if (!ret && retry > 1000)
				ret = -ETIMEDOUT;
			break;
		default:
			return -EINVAL;
		}
		if (ret) {
			pr_err("CP sequence %s failed: %d\n", s->sfr_name, ret);
			return ret;
		}
	}
	return 0;
}

int cal_cp_init(void)
{
	return cp_run(cp_init, ARRAY_SIZE(cp_init));
}

int cal_cp_status(void)
{
	u32 value;
	int ret = cp_read(EXYNOS_PMU_RESET_SEQUENCER_STATUS, &value);

	return ret ? ret : ((value & 7) == 5);
}

int cal_cp_reset_assert(void)
{
	return cp_run(cp_reset_assert, ARRAY_SIZE(cp_reset_assert));
}

int cal_cp_reset_release(void)
{
	return cp_run(cp_reset_release, ARRAY_SIZE(cp_reset_release));
}

int cal_cp_active_clear(void)
{
	return cp_run(cp_cp_active_clear, ARRAY_SIZE(cp_cp_active_clear));
}

int cal_cp_reset_req_clear(void)
{
	return cp_run(cp_cp_reset_req_clear, ARRAY_SIZE(cp_cp_reset_req_clear));
}
