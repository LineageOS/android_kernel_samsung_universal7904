// SPDX-License-Identifier: GPL-2.0-only
#include <linux/build_bug.h>
#include <soc/samsung/bts.h>

static_assert(BUS_WIDTH == 16 && DISP_UTIL == 75);
static_assert(BTS_BW_DECON0 == 0 && BTS_BW_MFC == 7 && BTS_BW_MAX == 8);
static_assert(BTS_DPP_MAX == 4 && BTS_DPU_MAX == 3 && BS_MAX == 5);
static_assert(sizeof(struct bts_dpp_info) == 40);
static_assert(sizeof(struct bts_decon_info) == 172);
static_assert(sizeof(struct bts_bw) == 12);

typedef unsigned int (*bts_calc_bw_fn)(enum bts_bw_type, void *);
typedef void (*bts_update_bw_fn)(enum bts_bw_type, struct bts_bw);
typedef void (*bts_update_scen_fn)(enum bts_scen_type, unsigned int);
static_assert(__same_type(&bts_calc_bw, (bts_calc_bw_fn)0));
static_assert(__same_type(&bts_update_bw, (bts_update_bw_fn)0));
static_assert(__same_type(&bts_update_scen, (bts_update_scen_fn)0));
