// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Exynos7885/7904 bank data from Samsung's 4.4 pinctrl-exynos.c.
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 * Copyright (c) 2012 Linaro Ltd.
 *
 * Unlike Exynos7/5433, PUD and PUD_PDN have four bits per pin as well as DRV.
 * Preserve vendor controller alias numbering: ALIVE, DISPAUD, FSYS, TOP.
 * Only FSYS/TOP are instantiated by the initial Exynos7904 DT. PMU pad
 * retention and suspend/resume have not been validated on this SoC.
 */

#include <linux/init.h>
#include <linux/kernel.h>

#include "pinctrl-samsung.h"
#include "pinctrl-exynos.h"

static const struct samsung_pin_bank_type exynos7885_bank_type_off = {
	.fld_width = { 4, 1, 4, 4, 2, 4, },
	.reg_offset = { 0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, },
};

static const struct samsung_pin_bank_type exynos7885_bank_type_alive = {
	.fld_width = { 4, 1, 4, 4, },
	.reg_offset = { 0x00, 0x04, 0x08, 0x0c, },
};

/* Vendor etc0/etc1 use the older non-ALIVE layout, with no EINT. */
static const struct samsung_pin_bank_type exynos7885_bank_type_etc = {
	.fld_width = { 4, 1, 2, 2, 2, 2, },
	.reg_offset = { 0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, },
};

#define EXYNOS7885_BANK(kind, pins, reg, id, eint, offs) \
	{ \
		.type = &exynos7885_bank_type_##kind, \
		.pctl_offset = reg, \
		.nr_pins = pins, \
		.name = id, \
		.eint_type = eint, \
		.eint_offset = offs, \
	}

static const struct samsung_pin_bank_data exynos7885_banks0[] __initconst = {
	EXYNOS7885_BANK(etc, 3, 0x000, "etc0", EINT_TYPE_NONE, 0),
	EXYNOS7885_BANK(etc, 3, 0x020, "etc1", EINT_TYPE_NONE, 0),
	EXYNOS7885_BANK(alive, 8, 0x040, "gpa0", EINT_TYPE_WKUP, 0x00),
	EXYNOS7885_BANK(alive, 8, 0x060, "gpa1", EINT_TYPE_WKUP, 0x04),
	EXYNOS7885_BANK(alive, 8, 0x080, "gpa2", EINT_TYPE_WKUP, 0x08),
	EXYNOS7885_BANK(alive, 5, 0x0a0, "gpq0", EINT_TYPE_WKUP, 0x0c),
};

static const struct samsung_pin_bank_data exynos7885_banks1[] __initconst = {
	EXYNOS7885_BANK(off, 5, 0x000, "gpb0", EINT_TYPE_GPIO, 0x00),
	EXYNOS7885_BANK(off, 4, 0x020, "gpb1", EINT_TYPE_GPIO, 0x04),
	EXYNOS7885_BANK(off, 5, 0x040, "gpb2", EINT_TYPE_GPIO, 0x08),
};

static const struct samsung_pin_bank_data exynos7885_banks2[] __initconst = {
	EXYNOS7885_BANK(off, 4, 0x000, "gpf0", EINT_TYPE_GPIO, 0x00),
	EXYNOS7885_BANK(off, 8, 0x020, "gpf2", EINT_TYPE_GPIO, 0x04),
	EXYNOS7885_BANK(off, 6, 0x040, "gpf3", EINT_TYPE_GPIO, 0x08),
	EXYNOS7885_BANK(off, 6, 0x060, "gpf4", EINT_TYPE_GPIO, 0x0c),
};

static const struct samsung_pin_bank_data exynos7885_banks3[] __initconst = {
	EXYNOS7885_BANK(off, 4, 0x000, "gpp0", EINT_TYPE_GPIO, 0x00),
	EXYNOS7885_BANK(off, 3, 0x020, "gpg0", EINT_TYPE_GPIO, 0x04),
	EXYNOS7885_BANK(off, 4, 0x040, "gpp1", EINT_TYPE_GPIO, 0x08),
	EXYNOS7885_BANK(off, 4, 0x060, "gpp2", EINT_TYPE_GPIO, 0x0c),
	EXYNOS7885_BANK(off, 3, 0x080, "gpp3", EINT_TYPE_GPIO, 0x10),
	EXYNOS7885_BANK(off, 6, 0x0a0, "gpp4", EINT_TYPE_GPIO, 0x14),
	EXYNOS7885_BANK(off, 4, 0x0c0, "gpp5", EINT_TYPE_GPIO, 0x18),
	EXYNOS7885_BANK(off, 5, 0x0e0, "gpp6", EINT_TYPE_GPIO, 0x1c),
	EXYNOS7885_BANK(off, 2, 0x100, "gpp7", EINT_TYPE_GPIO, 0x20),
	EXYNOS7885_BANK(off, 2, 0x120, "gpp8", EINT_TYPE_GPIO, 0x24),
	EXYNOS7885_BANK(off, 8, 0x140, "gpg1", EINT_TYPE_GPIO, 0x28),
	EXYNOS7885_BANK(off, 8, 0x160, "gpg2", EINT_TYPE_GPIO, 0x2c),
	EXYNOS7885_BANK(off, 8, 0x180, "gpg3", EINT_TYPE_GPIO, 0x30),
	EXYNOS7885_BANK(off, 2, 0x1a0, "gpg4", EINT_TYPE_GPIO, 0x34),
	EXYNOS7885_BANK(off, 4, 0x1c0, "gpc0", EINT_TYPE_GPIO, 0x38),
	EXYNOS7885_BANK(off, 8, 0x1e0, "gpc1", EINT_TYPE_GPIO, 0x3c),
	EXYNOS7885_BANK(off, 8, 0x200, "gpc2", EINT_TYPE_GPIO, 0x40),
};

static const struct samsung_pin_ctrl exynos7885_ctrl[] __initconst = {
	{
		.pin_banks = exynos7885_banks0,
		.nr_banks = ARRAY_SIZE(exynos7885_banks0),
		.eint_wkup_init = exynos_eint_wkup_init,
	}, {
		.pin_banks = exynos7885_banks1,
		.nr_banks = ARRAY_SIZE(exynos7885_banks1),
	}, {
		.pin_banks = exynos7885_banks2,
		.nr_banks = ARRAY_SIZE(exynos7885_banks2),
		.eint_gpio_init = exynos_eint_gpio_init,
	}, {
		.pin_banks = exynos7885_banks3,
		.nr_banks = ARRAY_SIZE(exynos7885_banks3),
		.eint_gpio_init = exynos_eint_gpio_init,
	},
};

const struct samsung_pinctrl_of_match_data exynos7885_of_data __initconst = {
	.ctrl = exynos7885_ctrl,
	.num_ctrl = ARRAY_SIZE(exynos7885_ctrl),
};
