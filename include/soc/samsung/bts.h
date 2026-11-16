/*
 * Copyright (c) 2014 Samsung Electronics Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef __EXYNOS_BTS_H_
#define __EXYNOS_BTS_H_

#include <linux/types.h>

/* Exynos7885 ABI: vendor include/soc/samsung/bts.h, lines 15-93. */
#define BUS_WIDTH		16
#define DISP_UTIL		75

enum bts_scen_type {
	BS_DEFAULT,
	BS_MIF_CHANGE,
	BS_MFC_UHD,
	BS_G3D_PEFORMANCE,
	BS_CAMERA_DEFAULT,
	BS_MAX,
};

enum bts_bw_type {
	/* RT */
	BTS_BW_DECON0,
	BTS_BW_DECON1,
	BTS_BW_DECON2,
	BTS_BW_CAMERA,
	BTS_BW_AUDIO,
	BTS_BW_CP,
	/* NRT */
	BTS_BW_G2D,
	BTS_BW_MFC,
	BTS_BW_MAX,
	BTS_BW_RT = BTS_BW_G2D,
};

enum bts_dpp_type {
	BTS_DPP0,
	BTS_DPP1,
	BTS_DPP2,
	BTS_DPP3,
	BTS_DPP_MAX,
};

enum bts_dpu_type {
	BTS_DPU0,
	BTS_DPU1,
	BTS_DPU2,
	BTS_DPU_MAX,
};

struct bts_layer_position {
	unsigned int x1;
	/* x2 = x1 + width */
	unsigned int x2;
	unsigned int y1;
	/* y2 = y1 + height */
	unsigned int y2;
};

struct bts_dpp_info {
	bool used;
	unsigned int bpp;
	unsigned int src_h;
	unsigned int src_w;
	struct bts_layer_position dst;
	unsigned int bw;
	unsigned int idma_type;
};

struct bts_decon_info {
	struct bts_dpp_info dpp[BTS_DPP_MAX];
	/* KHz */
	unsigned int vclk;
	unsigned int lcd_w;
	unsigned int lcd_h;
};

struct bts_bw {
	unsigned int peak;
	unsigned int read;
	unsigned int write;
};

/* No disabled-config no-ops: callers must depend on EXYNOS7885_BTS. */
void bts_update_scen(enum bts_scen_type type, unsigned int val);
/* Bandwidth in KB/s. Frequency requirements are recorded, not DVFS votes. */
void bts_update_bw(enum bts_bw_type type, struct bts_bw bw);
unsigned int bts_calc_bw(enum bts_bw_type type, void *data);

/*
 * bts_init/deinit/release_bw/update_qos_{disp,mif,int} are DECON operation
 * members, implemented by dpu_7885/bts.c, not functions in this public ABI.
 */
#endif
