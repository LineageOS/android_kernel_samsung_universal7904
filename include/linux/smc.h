/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Secure monitor calls used by the display stack.
 *
 * 4.4 reference: include/linux/smc.h:53,78-79,130-136,222 and
 * arch/arm64/kernel/exynos-smc.S (__exynos_smc: dsb sy; smc #0, returns x0).
 */
#ifndef __LINUX_EXYNOS_SMC_H
#define __LINUX_EXYNOS_SMC_H

#include <linux/arm-smccc.h>
#include <asm/barrier.h>

#define SMC_PROTECTION_SET		(0x82002010)
#define SMC_PROTECTION_ENABLE		(1)
#define SMC_PROTECTION_DISABLE		(0)

#define PROT_G0				(3)
#define PROT_G1				(4)
#define PROT_VG0			(5)
#define PROT_VGR0			(9)

static inline int exynos_smc(unsigned long cmd, unsigned long arg1,
			     unsigned long arg2, unsigned long arg3)
{
	struct arm_smccc_res res;

	dsb(sy);
	arm_smccc_smc(cmd, arg1, arg2, arg3, 0, 0, 0, 0, &res);

	return (int)res.a0;
}

#endif /* __LINUX_EXYNOS_SMC_H */
