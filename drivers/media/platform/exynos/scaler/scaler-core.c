/*
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * Core file for Samsung EXYNOS Scaler driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/version.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/interrupt.h>
#include <linux/clk.h>
#include <linux/slab.h>
#include <linux/pm_runtime.h>
#include <linux/reboot.h>
#include <linux/exynos_iovmm.h>
#include <linux/smc.h>

#include <media/v4l2-ioctl.h>
#include <media/m2m1shot.h>
#include <media/m2m1shot-helper.h>

#include <video/videonode.h>

#include "scaler.h"
#include "scaler-regs.h"

/* Protection IDs of Scaler are 1 and 2. */
#define SC_SMC_PROTECTION_ID(instance)	(1 + (instance))

int sc_log_level;
module_param_named(sc_log_level, sc_log_level, uint, 0644);

int sc_set_blur;
module_param_named(sc_set_blur, sc_set_blur, uint, 0644);

/*
 * If true, writes the latency of H/W operation to v4l2_buffer.reserved2
 * in the unit of nano seconds.  It must not be enabled with real use-case
 * because v4l2_buffer.reserved may be used for other purpose.
 * The latency is written to the destination buffer.
 */
int __measure_hw_latency;
module_param_named(measure_hw_latency, __measure_hw_latency, int, 0644);

struct vb2_sc_buffer {
	struct v4l2_m2m_buffer mb;
	struct sc_ctx *ctx;
	ktime_t ktime;
	struct work_struct work;
	struct dma_fence *acquire_fence;
};

static const struct sc_fmt sc_formats[] = {
	{
		.name		= "RGB565",
		.pixelformat	= V4L2_PIX_FMT_RGB565,
		.cfg_val	= SCALER_CFG_FMT_RGB565,
		.bitperpixel	= { 16 },
		.num_planes	= 1,
		.num_comp	= 1,
		.is_rgb		= 1,
	}, {
		.name		= "RGB1555",
		.pixelformat	= V4L2_PIX_FMT_RGB555X,
		.cfg_val	= SCALER_CFG_FMT_ARGB1555,
		.bitperpixel	= { 16 },
		.num_planes	= 1,
		.num_comp	= 1,
		.is_rgb		= 1,
	}, {
		.name		= "ARGB4444",
		.pixelformat	= V4L2_PIX_FMT_RGB444,
		.cfg_val	= SCALER_CFG_FMT_ARGB4444,
		.bitperpixel	= { 16 },
		.num_planes	= 1,
		.num_comp	= 1,
		.is_rgb		= 1,
	}, {	/* swaps of ARGB32 in bytes in half word, half words in word */
		.name		= "RGBA8888",
		.pixelformat	= V4L2_PIX_FMT_RGB32,
		.cfg_val	= SCALER_CFG_FMT_RGBA8888 |
					SCALER_CFG_BYTE_HWORD_SWAP,
		.bitperpixel	= { 32 },
		.num_planes	= 1,
		.num_comp	= 1,
		.is_rgb		= 1,
	}, {
		.name		= "BGRA8888",
		.pixelformat	= V4L2_PIX_FMT_BGR32,
		.cfg_val	= SCALER_CFG_FMT_ARGB8888,
		.bitperpixel	= { 32 },
		.num_planes	= 1,
		.num_comp	= 1,
		.is_rgb		= 1,
	}, {
		.name		= "YUV 4:2:0 contiguous 2-planar, Y/CbCr",
		.pixelformat	= V4L2_PIX_FMT_NV12,
		.cfg_val	= SCALER_CFG_FMT_YCBCR420_2P,
		.bitperpixel	= { 12 },
		.num_planes	= 1,
		.num_comp	= 2,
		.h_shift	= 1,
		.v_shift	= 1,
	}, {
		.name		= "YUV 4:2:0 contiguous 2-planar, Y/CrCb",
		.pixelformat	= V4L2_PIX_FMT_NV21,
		.cfg_val	= SCALER_CFG_FMT_YCRCB420_2P,
		.bitperpixel	= { 12 },
		.num_planes	= 1,
		.num_comp	= 2,
		.h_shift	= 1,
		.v_shift	= 1,
	}, {
		.name		= "YUV 4:2:0 non-contiguous 2-planar, Y/CbCr",
		.pixelformat	= V4L2_PIX_FMT_NV12M,
		.cfg_val	= SCALER_CFG_FMT_YCBCR420_2P,
		.bitperpixel	= { 8, 4 },
		.num_planes	= 2,
		.num_comp	= 2,
		.h_shift	= 1,
		.v_shift	= 1,
	}, {
		.name		= "YUV 4:2:0 non-contiguous 2-planar, Y/CrCb",
		.pixelformat	= V4L2_PIX_FMT_NV21M,
		.cfg_val	= SCALER_CFG_FMT_YCRCB420_2P,
		.bitperpixel	= { 8, 4 },
		.num_planes	= 2,
		.num_comp	= 2,
		.h_shift	= 1,
		.v_shift	= 1,
	}, {
		.name		= "YUV 4:2:0 non-contiguous 2-planar, Y/CbCr, tiled",
		.pixelformat	= V4L2_PIX_FMT_NV12MT_16X16,
		.cfg_val	= SCALER_CFG_FMT_YCBCR420_2P |
					SCALER_CFG_TILE_EN,
		.bitperpixel	= { 8, 4 },
		.num_planes	= 2,
		.num_comp	= 2,
		.h_shift	= 1,
		.v_shift	= 1,
	}, {
		.name		= "YUV 4:2:0 contiguous 3-planar, Y/Cb/Cr",
		.pixelformat	= V4L2_PIX_FMT_YUV420,	/* I420 */
		.cfg_val	= SCALER_CFG_FMT_YCBCR420_3P,
		.bitperpixel	= { 12 },
		.num_planes	= 1,
		.num_comp	= 3,
		.h_shift	= 1,
		.v_shift	= 1,
	}, {
		.name		= "YVU 4:2:0 contiguous 3-planar, Y/Cr/Cb",
		.pixelformat	= V4L2_PIX_FMT_YVU420,	/* YV12 */
		.cfg_val	= SCALER_CFG_FMT_YCBCR420_3P,
		.bitperpixel	= { 12 },
		.num_planes	= 1,
		.num_comp	= 3,
		.h_shift	= 1,
		.v_shift	= 1,
	}, {
		.name		= "YUV 4:2:0 non-contiguous 3-planar, Y/Cb/Cr",
		.pixelformat	= V4L2_PIX_FMT_YUV420M,
		.cfg_val	= SCALER_CFG_FMT_YCBCR420_3P,
		.bitperpixel	= { 8, 2, 2 },
		.num_planes	= 3,
		.num_comp	= 3,
		.h_shift	= 1,
		.v_shift	= 1,
	}, {
		.name		= "YVU 4:2:0 non-contiguous 3-planar, Y/Cr/Cb",
		.pixelformat	= V4L2_PIX_FMT_YVU420M,
		.cfg_val	= SCALER_CFG_FMT_YCBCR420_3P,
		.bitperpixel	= { 8, 2, 2 },
		.num_planes	= 3,
		.num_comp	= 3,
		.h_shift	= 1,
		.v_shift	= 1,
	}, {
		.name		= "YUV 4:2:2 packed, YCbYCr",
		.pixelformat	= V4L2_PIX_FMT_YUYV,
		.cfg_val	= SCALER_CFG_FMT_YUYV,
		.bitperpixel	= { 16 },
		.num_planes	= 1,
		.num_comp	= 1,
		.h_shift	= 1,
	}, {
		.name		= "YUV 4:2:2 packed, CbYCrY",
		.pixelformat	= V4L2_PIX_FMT_UYVY,
		.cfg_val	= SCALER_CFG_FMT_UYVY,
		.bitperpixel	= { 16 },
		.num_planes	= 1,
		.num_comp	= 1,
		.h_shift	= 1,
	}, {
		.name		= "YUV 4:2:2 packed, YCrYCb",
		.pixelformat	= V4L2_PIX_FMT_YVYU,
		.cfg_val	= SCALER_CFG_FMT_YVYU,
		.bitperpixel	= { 16 },
		.num_planes	= 1,
		.num_comp	= 1,
		.h_shift	= 1,
	}, {
		.name		= "YUV 4:2:2 contiguous 2-planar, Y/CbCr",
		.pixelformat	= V4L2_PIX_FMT_NV16,
		.cfg_val	= SCALER_CFG_FMT_YCBCR422_2P,
		.bitperpixel	= { 16 },
		.num_planes	= 1,
		.num_comp	= 2,
		.h_shift	= 1,
	}, {
		.name		= "YUV 4:2:2 contiguous 2-planar, Y/CrCb",
		.pixelformat	= V4L2_PIX_FMT_NV61,
		.cfg_val	= SCALER_CFG_FMT_YCRCB422_2P,
		.bitperpixel	= { 16 },
		.num_planes	= 1,
		.num_comp	= 2,
		.h_shift	= 1,
	}, {
		.name		= "YUV 4:2:2 contiguous 3-planar, Y/Cb/Cr",
		.pixelformat	= V4L2_PIX_FMT_YUV422P,
		.cfg_val	= SCALER_CFG_FMT_YCBCR422_3P,
		.bitperpixel	= { 16 },
		.num_planes	= 1,
		.num_comp	= 3,
		.h_shift	= 1,
	}, {
		.name		= "YUV 4:2:0 contiguous Y/CbCr",
		.pixelformat	= V4L2_PIX_FMT_NV12N,
		.cfg_val	= SCALER_CFG_FMT_YCBCR420_2P,
		.bitperpixel	= { 12 },
		.num_planes	= 1,
		.num_comp	= 2,
		.h_shift	= 1,
		.v_shift	= 1,
	}, {
		.name		= "YUV 4:2:0 contiguous Y/CbCr 10-bit",
		.pixelformat	= V4L2_PIX_FMT_NV12N_10B,
		.cfg_val	= SCALER_CFG_FMT_YCBCR420_2P,
		.bitperpixel	= { 15 },
		.num_planes	= 1,
		.num_comp	= 2,
		.h_shift	= 1,
		.v_shift	= 1,
	}, {
		.name		= "YUV 4:2:0 contiguous 3-planar Y/Cb/Cr",
		.pixelformat	= V4L2_PIX_FMT_YUV420N,
		.cfg_val	= SCALER_CFG_FMT_YCBCR420_3P,
		.bitperpixel	= { 12 },
		.num_planes	= 1,
		.num_comp	= 3,
		.h_shift	= 1,
		.v_shift	= 1,
	},
};

#define SCALE_RATIO_CONST(x, y) (u32)((1048576ULL * (x)) / (y))

#define SCALE_RATIO(x, y)						\
({									\
		u32 ratio;						\
		if (__builtin_constant_p(x) && __builtin_constant_p(y))	{\
			ratio = SCALE_RATIO_CONST(x, y);		\
		} else if ((x) < 2048) {				\
			ratio = (u32)((1048576UL * (x)) / (y));		\
		} else {						\
			unsigned long long dividend = 1048576ULL;	\
			dividend *= x;					\
			do_div(dividend, y);				\
			ratio = (u32)dividend;				\
		}							\
		ratio;							\
})

#define SCALE_RATIO_FRACT(x, y, z) (u32)(((x << 20) + SCALER_FRACT_VAL(y)) / z)

/* must specify in revers order of SCALER_VERSION(xyz) */
static const u32 sc_version_table[][2] = {
	{ 0x80060007, SCALER_VERSION(4, 2, 0) }, /* SC_BI */
	{ 0x0100000f, SCALER_VERSION(4, 0, 1) }, /* SC_POLY */
	{ 0xA0000013, SCALER_VERSION(4, 0, 1) },
	{ 0xA0000012, SCALER_VERSION(4, 0, 1) },
	{ 0x80044001, SCALER_VERSION(4, 0, 2) }, /* SC_POLY w/ CLK_REQ */
	{ 0x80050007, SCALER_VERSION(4, 0, 0) }, /* SC_POLY */
	{ 0xA000000B, SCALER_VERSION(3, 0, 2) },
	{ 0xA000000A, SCALER_VERSION(3, 0, 2) },
	{ 0x8000006D, SCALER_VERSION(3, 0, 1) },
	{ 0x80000068, SCALER_VERSION(3, 0, 0) },
	{ 0x8004000C, SCALER_VERSION(2, 2, 0) },
	{ 0x80000008, SCALER_VERSION(2, 1, 1) },
	{ 0x80000048, SCALER_VERSION(2, 1, 0) },
	{ 0x80010000, SCALER_VERSION(2, 0, 1) },
	{ 0x80000047, SCALER_VERSION(2, 0, 0) },
};

static const struct sc_variant sc_variant[] = {
	{
		.limit_input = {
			.min_w		= 16,
			.min_h		= 16,
			.max_w		= 8192,
			.max_h		= 8192,
		},
		.limit_output = {
			.min_w		= 4,
			.min_h		= 4,
			.max_w		= 8192,
			.max_h		= 8192,
		},
		.version		= SCALER_VERSION(4, 2, 0),
		.sc_up_max		= SCALE_RATIO_CONST(1, 8),
		.sc_down_min		= SCALE_RATIO_CONST(4, 1),
		.sc_down_swmin		= SCALE_RATIO_CONST(16, 1),
		.blending		= 1,
		.prescale		= 0,
		.ratio_20bit		= 1,
		.initphase		= 1,
	}, {
		.limit_input = {
			.min_w		= 16,
			.min_h		= 16,
			.max_w		= 8192,
			.max_h		= 8192,
		},
		.limit_output = {
			.min_w		= 4,
			.min_h		= 4,
			.max_w		= 8192,
			.max_h		= 8192,
		},
		.version		= SCALER_VERSION(4, 0, 2),
		.sc_up_max		= SCALE_RATIO_CONST(1, 8),
		.sc_down_min		= SCALE_RATIO_CONST(4, 1),
		.sc_up_swmax		= SCALE_RATIO_CONST(1, 16),
		.sc_down_swmin		= SCALE_RATIO_CONST(16, 1),
		.blending		= 0,
		.prescale		= 0,
		.ratio_20bit		= 0,
		.initphase		= 0,
		.q_chan_ctrl		= 1,
	}, {
		.limit_input = {
			.min_w		= 16,
			.min_h		= 16,
			.max_w		= 8192,
			.max_h		= 8192,
		},
		.limit_output = {
			.min_w		= 4,
			.min_h		= 4,
			.max_w		= 8192,
			.max_h		= 8192,
		},
		.version		= SCALER_VERSION(4, 0, 1),
		.sc_up_max		= SCALE_RATIO_CONST(1, 8),
		.sc_down_min		= SCALE_RATIO_CONST(4, 1),
		.sc_up_swmax		= SCALE_RATIO_CONST(1, 16),
		.sc_down_swmin		= SCALE_RATIO_CONST(16, 1),
		.blending		= 0,
		.prescale		= 0,
		.ratio_20bit		= 1,
		.initphase		= 1,
	}, {
		.limit_input = {
			.min_w		= 16,
			.min_h		= 16,
			.max_w		= 8192,
			.max_h		= 8192,
		},
		.limit_output = {
			.min_w		= 4,
			.min_h		= 4,
			.max_w		= 8192,
			.max_h		= 8192,
		},
		.version		= SCALER_VERSION(4, 0, 0),
		.sc_up_max		= SCALE_RATIO_CONST(1, 8),
		.sc_down_min		= SCALE_RATIO_CONST(4, 1),
		.sc_down_swmin		= SCALE_RATIO_CONST(16, 1),
		.blending		= 0,
		.prescale		= 0,
		.ratio_20bit		= 0,
		.initphase		= 0,
	}, {
		.limit_input = {
			.min_w		= 16,
			.min_h		= 16,
			.max_w		= 8192,
			.max_h		= 8192,
		},
		.limit_output = {
			.min_w		= 4,
			.min_h		= 4,
			.max_w		= 8192,
			.max_h		= 8192,
		},
		.version		= SCALER_VERSION(3, 0, 0),
		.sc_up_max		= SCALE_RATIO_CONST(1, 8),
		.sc_down_min		= SCALE_RATIO_CONST(16, 1),
		.sc_down_swmin		= SCALE_RATIO_CONST(16, 1),
		.blending		= 0,
		.prescale		= 1,
		.ratio_20bit		= 1,
		.initphase		= 1,
	}, {
		.limit_input = {
			.min_w		= 16,
			.min_h		= 16,
			.max_w		= 8192,
			.max_h		= 8192,
		},
		.limit_output = {
			.min_w		= 4,
			.min_h		= 4,
			.max_w		= 8192,
			.max_h		= 8192,
		},
		.version		= SCALER_VERSION(2, 2, 0),
		.sc_up_max		= SCALE_RATIO_CONST(1, 8),
		.sc_down_min		= SCALE_RATIO_CONST(4, 1),
		.sc_down_swmin		= SCALE_RATIO_CONST(16, 1),
		.blending		= 1,
		.prescale		= 0,
		.ratio_20bit		= 0,
		.initphase		= 0,
	}, {
		.limit_input = {
			.min_w		= 16,
			.min_h		= 16,
			.max_w		= 8192,
			.max_h		= 8192,
		},
		.limit_output = {
			.min_w		= 4,
			.min_h		= 4,
			.max_w		= 8192,
			.max_h		= 8192,
		},
		.version		= SCALER_VERSION(2, 0, 1),
		.sc_up_max		= SCALE_RATIO_CONST(1, 8),
		.sc_down_min		= SCALE_RATIO_CONST(4, 1),
		.sc_down_swmin		= SCALE_RATIO_CONST(16, 1),
		.blending		= 0,
		.prescale		= 0,
		.ratio_20bit		= 0,
		.initphase		= 0,
	}, {
		.limit_input = {
			.min_w		= 16,
			.min_h		= 16,
			.max_w		= 8192,
			.max_h		= 8192,
		},
		.limit_output = {
			.min_w		= 4,
			.min_h		= 4,
			.max_w		= 4096,
			.max_h		= 4096,
		},
		.version		= SCALER_VERSION(2, 0, 0),
		.sc_up_max		= SCALE_RATIO_CONST(1, 8),
		.sc_down_min		= SCALE_RATIO_CONST(4, 1),
		.sc_down_swmin		= SCALE_RATIO_CONST(16, 1),
		.blending		= 0,
		.prescale		= 0,
		.ratio_20bit		= 0,
		.initphase		= 0,
	},
};

/* Find the matches format */
static const struct sc_fmt *sc_find_format(struct sc_dev *sc,
						u32 pixfmt, bool output_buf)
{
	const struct sc_fmt *sc_fmt;
	unsigned long i;

	for (i = 0; i < ARRAY_SIZE(sc_formats); ++i) {
		sc_fmt = &sc_formats[i];
		if (sc_fmt->pixelformat == pixfmt) {
			if (!!(sc_fmt->cfg_val & SCALER_CFG_TILE_EN)) {
				/* tile mode is not supported from v3.0.0 */
				if (sc->version >= SCALER_VERSION(3, 0, 0))
					return NULL;
				if (!output_buf)
					return NULL;
			}
			/* bytes swap is not supported under v2.1.0 */
			if (!!(sc_fmt->cfg_val & SCALER_CFG_SWAP_MASK) &&
					(sc->version < SCALER_VERSION(2, 1, 0)))
				return NULL;
			return &sc_formats[i];
		}
	}

	return NULL;
}

static int sc_v4l2_querycap(struct file *file, void *fh,
			     struct v4l2_capability *cap)
{
	strncpy(cap->driver, MODULE_NAME, sizeof(cap->driver) - 1);
	strncpy(cap->card, MODULE_NAME, sizeof(cap->card) - 1);

	cap->device_caps = video_devdata(file)->device_caps;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;

	return 0;
}

static int sc_v4l2_enum_fmt_mplane(struct file *file, void *fh,
			     struct v4l2_fmtdesc *f)
{
	const struct sc_fmt *sc_fmt;

	if (f->index >= ARRAY_SIZE(sc_formats))
		return -EINVAL;

	sc_fmt = &sc_formats[f->index];
	strncpy(f->description, sc_fmt->name, sizeof(f->description) - 1);
	f->pixelformat = sc_fmt->pixelformat;

	return 0;
}

static int sc_v4l2_g_fmt_mplane(struct file *file, void *fh,
			  struct v4l2_format *f)
{
	struct sc_ctx *ctx = fh_to_sc_ctx(fh);
	const struct sc_fmt *sc_fmt;
	struct sc_frame *frame;
	struct v4l2_pix_format_mplane *pixm = &f->fmt.pix_mp;
	int i;

	frame = ctx_get_frame(ctx, f->type);
	if (IS_ERR(frame))
		return PTR_ERR(frame);

	sc_fmt = frame->sc_fmt;

	pixm->width		= frame->width;
	pixm->height		= frame->height;
	pixm->pixelformat	= frame->pixelformat;
	pixm->field		= V4L2_FIELD_NONE;
	pixm->num_planes	= frame->sc_fmt->num_planes;
	pixm->colorspace	= 0;

	for (i = 0; i < pixm->num_planes; ++i) {
		pixm->plane_fmt[i].bytesperline = (pixm->width *
				sc_fmt->bitperpixel[i]) >> 3;
		if (sc_fmt_is_ayv12(sc_fmt->pixelformat)) {
			unsigned int y_size, c_span;
			y_size = pixm->width * pixm->height;
			c_span = ALIGN(pixm->width >> 1, 16);
			pixm->plane_fmt[i].sizeimage =
				y_size + (c_span * pixm->height >> 1) * 2;
		} else {
			pixm->plane_fmt[i].sizeimage =
				pixm->plane_fmt[i].bytesperline * pixm->height;
		}

		v4l2_dbg(1, sc_log_level, &ctx->sc_dev->m2m.v4l2_dev,
				"[%d] plane: bytesperline %d, sizeimage %d\n",
				i, pixm->plane_fmt[i].bytesperline,
				pixm->plane_fmt[i].sizeimage);
	}

	return 0;
}

static int sc_v4l2_try_fmt_mplane(struct file *file, void *fh,
			    struct v4l2_format *f)
{
	struct sc_ctx *ctx = fh_to_sc_ctx(fh);
	const struct sc_fmt *sc_fmt;
	struct v4l2_pix_format_mplane *pixm = &f->fmt.pix_mp;
	const struct sc_size_limit *limit;
	int i;
	int h_align = 0;
	int w_align = 0;

	if (!V4L2_TYPE_IS_MULTIPLANAR(f->type)) {
		v4l2_err(&ctx->sc_dev->m2m.v4l2_dev,
				"not supported v4l2 type\n");
		return -EINVAL;
	}

	sc_fmt = sc_find_format(ctx->sc_dev, f->fmt.pix_mp.pixelformat, V4L2_TYPE_IS_OUTPUT(f->type));
	if (!sc_fmt) {
		v4l2_err(&ctx->sc_dev->m2m.v4l2_dev,
				"not supported format type\n");
		return -EINVAL;
	}

	if (V4L2_TYPE_IS_OUTPUT(f->type))
		limit = &ctx->sc_dev->variant->limit_input;
	else
		limit = &ctx->sc_dev->variant->limit_output;

	/*
	 * Y_SPAN - should even in interleaved YCbCr422
	 * C_SPAN - should even in YCbCr420 and YCbCr422
	 */
	w_align = sc_fmt->h_shift;
	h_align = sc_fmt->v_shift;

	/* Bound an image to have width and height in limit */
	v4l_bound_align_image(&pixm->width, limit->min_w, limit->max_w,
			w_align, &pixm->height, limit->min_h,
			limit->max_h, h_align, 0);

	pixm->num_planes = sc_fmt->num_planes;
	pixm->colorspace = 0;

	for (i = 0; i < pixm->num_planes; ++i) {
		pixm->plane_fmt[i].bytesperline = (pixm->width *
				sc_fmt->bitperpixel[i]) >> 3;
		if (sc_fmt_is_ayv12(sc_fmt->pixelformat)) {
			unsigned int y_size, c_span;
			y_size = pixm->width * pixm->height;
			c_span = ALIGN(pixm->width >> 1, 16);
			pixm->plane_fmt[i].sizeimage =
				y_size + (c_span * pixm->height >> 1) * 2;
		} else {
			pixm->plane_fmt[i].sizeimage =
				pixm->plane_fmt[i].bytesperline * pixm->height;
		}

		v4l2_dbg(1, sc_log_level, &ctx->sc_dev->m2m.v4l2_dev,
				"[%d] plane: bytesperline %d, sizeimage %d\n",
				i, pixm->plane_fmt[i].bytesperline,
				pixm->plane_fmt[i].sizeimage);
	}

	return 0;
}

static int sc_v4l2_s_fmt_mplane(struct file *file, void *fh,
				 struct v4l2_format *f)

{
	struct sc_ctx *ctx = fh_to_sc_ctx(fh);
	struct vb2_queue *vq = v4l2_m2m_get_vq(ctx->m2m_ctx, f->type);
	struct sc_frame *frame;
	struct v4l2_pix_format_mplane *pixm = &f->fmt.pix_mp;
	const struct sc_size_limit *limitout =
				&ctx->sc_dev->variant->limit_input;
	const struct sc_size_limit *limitcap =
				&ctx->sc_dev->variant->limit_output;
	int i, ret = 0;

	if (vb2_is_streaming(vq)) {
		v4l2_err(&ctx->sc_dev->m2m.v4l2_dev, "device is busy\n");
		return -EBUSY;
	}

	ret = sc_v4l2_try_fmt_mplane(file, fh, f);
	if (ret < 0)
		return ret;

	frame = ctx_get_frame(ctx, f->type);
	if (IS_ERR(frame))
		return PTR_ERR(frame);

	set_bit(CTX_PARAMS, &ctx->flags);

	frame->sc_fmt = sc_find_format(ctx->sc_dev, f->fmt.pix_mp.pixelformat, V4L2_TYPE_IS_OUTPUT(f->type));
	if (!frame->sc_fmt) {
		v4l2_err(&ctx->sc_dev->m2m.v4l2_dev,
				"not supported format values\n");
		return -EINVAL;
	}

	for (i = 0; i < frame->sc_fmt->num_planes; i++)
		frame->bytesused[i] = pixm->plane_fmt[i].sizeimage;

	if (V4L2_TYPE_IS_OUTPUT(f->type) &&
		((pixm->width > limitout->max_w) ||
			 (pixm->height > limitout->max_h))) {
		v4l2_err(&ctx->sc_dev->m2m.v4l2_dev,
			"%dx%d of source image is not supported: too large\n",
			pixm->width, pixm->height);
		return -EINVAL;
	}

	if (!V4L2_TYPE_IS_OUTPUT(f->type) &&
		((pixm->width > limitcap->max_w) ||
			 (pixm->height > limitcap->max_h))) {
		v4l2_err(&ctx->sc_dev->m2m.v4l2_dev,
			"%dx%d of target image is not supported: too large\n",
			pixm->width, pixm->height);
		return -EINVAL;
	}

	if (V4L2_TYPE_IS_OUTPUT(f->type) &&
		((pixm->width < limitout->min_w) ||
			 (pixm->height < limitout->min_h))) {
		v4l2_err(&ctx->sc_dev->m2m.v4l2_dev,
			"%dx%d of source image is not supported: too small\n",
			pixm->width, pixm->height);
		return -EINVAL;
	}

	if (!V4L2_TYPE_IS_OUTPUT(f->type) &&
		((pixm->width < limitcap->min_w) ||
			 (pixm->height < limitcap->min_h))) {
		v4l2_err(&ctx->sc_dev->m2m.v4l2_dev,
			"%dx%d of target image is not supported: too small\n",
			pixm->width, pixm->height);
		return -EINVAL;
	}

	if (pixm->flags == V4L2_PIX_FMT_FLAG_PREMUL_ALPHA &&
			ctx->sc_dev->version != SCALER_VERSION(4, 0, 0))
		frame->pre_multi = true;
	else
		frame->pre_multi = false;

	frame->width = pixm->width;
	frame->height = pixm->height;
	frame->pixelformat = pixm->pixelformat;

	frame->crop.width = pixm->width;
	frame->crop.height = pixm->height;

	return 0;
}

static void sc_queue_ready(struct vb2_sc_buffer *svb, bool failed)
{
	struct vb2_buffer *vb = &svb->mb.vb.vb2_buf;
	struct sc_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	mutex_lock(&ctx->fence_lock);
	if (failed || ctx->stopping) {
		vb2_buffer_done(vb, VB2_BUF_STATE_ERROR);
	} else {
		/* The producer fence must complete before consumer cache ownership. */
		sc_buf_sync_prepare(vb);
		v4l2_m2m_buf_queue(ctx->m2m_ctx, &svb->mb.vb);
		v4l2_m2m_try_schedule(ctx->m2m_ctx);
	}
	mutex_unlock(&ctx->fence_lock);
}

static void sc_fence_work(struct work_struct *work)
{
	struct vb2_sc_buffer *svb = container_of(work, struct vb2_sc_buffer, work);
	struct dma_fence *fence = xchg(&svb->acquire_fence, NULL);
	long ret = -EINVAL;

	if (fence) {
		ret = dma_fence_wait_timeout(fence, true, SC_TIMEOUT);
		if (ret > 0 && dma_fence_get_status(fence) < 0)
			ret = dma_fence_get_status(fence);
		dma_fence_put(fence);
	}
	if (ret <= 0)
		pr_warn_ratelimited("exynos-scaler: acquire fence failed (%ld)\n", ret);
	sc_queue_ready(svb, ret <= 0);
}

static int sc_vb2_buf_init(struct vb2_buffer *vb)
{
	struct vb2_sc_buffer *svb = container_of(to_vb2_v4l2_buffer(vb),
						struct vb2_sc_buffer, mb.vb);

	INIT_WORK(&svb->work, sc_fence_work);
	svb->acquire_fence = NULL;
	return 0;
}


static int sc_v4l2_reqbufs(struct file *file, void *fh,
			 struct v4l2_requestbuffers *reqbufs)
{
	return v4l2_m2m_ioctl_reqbufs(file, fh, reqbufs);
}


static int sc_v4l2_querybuf(struct file *file, void *fh,
			     struct v4l2_buffer *buf)
{
	struct sc_ctx *ctx = fh_to_sc_ctx(fh);
	return v4l2_m2m_querybuf(file, ctx->m2m_ctx, buf);
}

static int sc_v4l2_qbuf(struct file *file, void *fh, struct v4l2_buffer *buf)
{
	struct sc_ctx *ctx = fh_to_sc_ctx(fh);
	struct vb2_queue *vq;
	struct vb2_sc_buffer *svb;
	struct dma_fence *fence = NULL;
	int ret;

	if (buf->type != V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE &&
	    buf->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		return -EINVAL;
	vq = v4l2_m2m_get_vq(ctx->m2m_ctx, buf->type);
	if (!vq || buf->index >= vq->num_buffers || !vq->bufs[buf->index])
		return -EINVAL;
	if (vq->bufs[buf->index]->state != VB2_BUF_STATE_DEQUEUED)
		return -EBUSY;
	svb = container_of(to_vb2_v4l2_buffer(vq->bufs[buf->index]),
				struct vb2_sc_buffer, mb.vb);
	/* Samsung 4.4 puts an acquire sync-file fd in v4l2_buffer.reserved.
	 * It overlaps native request_fd, but USE_SYNC is an input-only hint.
	 */
	if ((buf->flags & 0x8000) && buf->request_fd >= 0) {
		fence = sync_file_get_fence(buf->request_fd);
		if (!fence)
			return -EINVAL;
	}
	if (svb->acquire_fence) {
		dma_fence_put(fence);
		return -EBUSY;
	}
	svb->acquire_fence = fence;
	buf->flags &= ~0x8000;
	buf->request_fd = 0;
	ret = v4l2_m2m_ioctl_qbuf(file, fh, buf);
	if (ret)
		dma_fence_put(xchg(&svb->acquire_fence, NULL));
	return ret;
}


static int sc_v4l2_dqbuf(struct file *file, void *fh,
			  struct v4l2_buffer *buf)
{
	struct sc_ctx *ctx = fh_to_sc_ctx(fh);
	return v4l2_m2m_dqbuf(file, ctx->m2m_ctx, buf);
}

static int sc_v4l2_streamon(struct file *file, void *fh, enum v4l2_buf_type type)
{
	struct sc_ctx *ctx = fh_to_sc_ctx(fh);

	mutex_lock(&ctx->fence_lock);
	ctx->stopping = false;
	mutex_unlock(&ctx->fence_lock);
	return v4l2_m2m_ioctl_streamon(file, fh, type);
}


static int sc_v4l2_streamoff(struct file *file, void *fh,
			      enum v4l2_buf_type type)
{
	struct sc_ctx *ctx = fh_to_sc_ctx(fh);
	return v4l2_m2m_streamoff(file, ctx->m2m_ctx, type);
}

static int sc_v4l2_cropcap(struct file *file, void *fh,
			    struct v4l2_cropcap *cr)
{
	struct sc_ctx *ctx = fh_to_sc_ctx(fh);
	struct sc_frame *frame;

	frame = ctx_get_frame(ctx, cr->type);
	if (IS_ERR(frame))
		return PTR_ERR(frame);

	cr->bounds.left		= 0;
	cr->bounds.top		= 0;
	cr->bounds.width	= frame->width;
	cr->bounds.height	= frame->height;
	cr->defrect		= cr->bounds;

	return 0;
}

static int sc_v4l2_g_crop(struct file *file, void *fh, struct v4l2_crop *cr)
{
	struct sc_ctx *ctx = fh_to_sc_ctx(fh);
	struct sc_frame *frame;

	frame = ctx_get_frame(ctx, cr->type);
	if (IS_ERR(frame))
		return PTR_ERR(frame);

	cr->c.left = SC_CROP_MAKE_FR_VAL(frame->crop.left, ctx->init_phase.yh);
	cr->c.top = SC_CROP_MAKE_FR_VAL(frame->crop.top, ctx->init_phase.yv);
	cr->c.width = SC_CROP_MAKE_FR_VAL(frame->crop.width, ctx->init_phase.w);
	cr->c.height = SC_CROP_MAKE_FR_VAL(frame->crop.height, ctx->init_phase.h);

	return 0;
}

static int sc_get_fract_val(struct v4l2_rect *rect, struct sc_ctx *ctx)
{
	ctx->init_phase.yh = SC_CROP_GET_FR_VAL(rect->left);
	if (ctx->init_phase.yh)
		rect->left &= SC_CROP_INT_MASK;

	ctx->init_phase.yv = SC_CROP_GET_FR_VAL(rect->top);
	if (ctx->init_phase.yv)
		rect->top &= SC_CROP_INT_MASK;

	ctx->init_phase.w = SC_CROP_GET_FR_VAL(rect->width);
	if (ctx->init_phase.w) {
		rect->width &= SC_CROP_INT_MASK;
		rect->width += 1;
	}

	ctx->init_phase.h = SC_CROP_GET_FR_VAL(rect->height);
	if (ctx->init_phase.h) {
		rect->height &= SC_CROP_INT_MASK;
		rect->height += 1;
	}

	if (sc_fmt_is_yuv420(ctx->s_frame.sc_fmt->pixelformat)) {
		ctx->init_phase.ch = ctx->init_phase.yh / 2;
		ctx->init_phase.cv = ctx->init_phase.yv / 2;
	} else {
		ctx->init_phase.ch = ctx->init_phase.yh;
		ctx->init_phase.cv = ctx->init_phase.yv;
	}

	if ((ctx->init_phase.yh || ctx->init_phase.yv || ctx->init_phase.w
			|| ctx->init_phase.h) &&
		(!(sc_fmt_is_yuv420(ctx->s_frame.sc_fmt->pixelformat) ||
		sc_fmt_is_rgb888(ctx->s_frame.sc_fmt->pixelformat)))) {
		v4l2_err(&ctx->sc_dev->m2m.v4l2_dev,
			"%s format on real number is not supported",
			ctx->s_frame.sc_fmt->name);
		return -EINVAL;
	}

	v4l2_dbg(1, sc_log_level, &ctx->sc_dev->m2m.v4l2_dev,
				"src crop position (x,y,w,h) =	\
				(%d.%d, %d.%d, %d.%d, %d.%d) %d, %d\n",
				rect->left, ctx->init_phase.yh,
				rect->top, ctx->init_phase.yv,
				rect->width, ctx->init_phase.w,
				rect->height, ctx->init_phase.h,
				ctx->init_phase.ch, ctx->init_phase.cv);
	return 0;
}

static int sc_v4l2_s_crop(struct file *file, void *fh,
		const struct v4l2_crop *cr)
{
	struct sc_ctx *ctx = fh_to_sc_ctx(fh);
	struct sc_dev *sc = ctx->sc_dev;
	struct sc_frame *frame;
	struct v4l2_rect rect = cr->c;
	struct sc_init_phase saved_phase = ctx->init_phase;
	const struct sc_size_limit *limit = NULL;
	int w_align = 0;
	int h_align = 0;
	int ret = 0;

	frame = ctx_get_frame(ctx, cr->type);
	if (IS_ERR(frame))
		return PTR_ERR(frame);

	if (!test_bit(CTX_PARAMS, &ctx->flags)) {
		v4l2_err(&ctx->sc_dev->m2m.v4l2_dev,
				"color format is not set\n");
		return -EINVAL;
	}

	if (cr->c.left < 0 || cr->c.top < 0) {
		v4l2_err(&ctx->sc_dev->m2m.v4l2_dev,
				"crop position is negative\n");
		return -EINVAL;
	}

	if (V4L2_TYPE_IS_OUTPUT(cr->type)) {
		ret = sc_get_fract_val(&rect, ctx);
		if (ret < 0) {
			ctx->init_phase = saved_phase;
			return ret;
		}
		limit = &sc->variant->limit_input;
	} else {
		limit = &sc->variant->limit_output;
	}

	w_align = frame->sc_fmt->h_shift;
	h_align = frame->sc_fmt->v_shift;

	/* Bound an image to have crop width and height in limit */
	v4l_bound_align_image(&rect.width, limit->min_w, limit->max_w,
			w_align, &rect.height, limit->min_h,
			limit->max_h, h_align, 0);

	if (rect.width > frame->width || rect.height > frame->height) {
		ctx->init_phase = saved_phase;
		return -EINVAL;
	}
	/* Bound an image to have crop position in limit */
	v4l_bound_align_image(&rect.left, 0, frame->width - rect.width,
			w_align, &rect.top, 0, frame->height - rect.height,
			h_align, 0);

	if ((rect.height > frame->height) || (rect.width > frame->width) ||
	    rect.top > frame->height - rect.height ||
	    rect.left > frame->width - rect.width) {
		v4l2_err(&ctx->sc_dev->m2m.v4l2_dev,
			"Out of crop range: (%d,%d,%d,%d) from %dx%d\n",
			rect.left, rect.top, rect.width, rect.height,
			frame->width, frame->height);
		ctx->init_phase = saved_phase;
		return -EINVAL;
	}

	frame->crop.top = rect.top;
	frame->crop.left = rect.left;
	frame->crop.height = rect.height;
	frame->crop.width = rect.width;
	set_bit(V4L2_TYPE_IS_OUTPUT(cr->type) ? CTX_SRC_FMT : CTX_DST_FMT,
		&ctx->flags);

	return 0;
}

static int sc_selection_type(u32 type)
{
	switch (type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		return V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		return V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	default:
		return -EINVAL;
	}
}

static int sc_v4l2_g_selection(struct file *file, void *fh,
				struct v4l2_selection *sel)
{
	int type = sc_selection_type(sel->type);
	bool output;
	struct v4l2_crop crop;
	struct v4l2_cropcap cap;
	int ret;

	if (type < 0)
		return type;
	output = V4L2_TYPE_IS_OUTPUT(type);
	memset(&crop, 0, sizeof(crop));
	memset(&cap, 0, sizeof(cap));
	crop.type = cap.type = type;
	if (sel->target == (output ? V4L2_SEL_TGT_CROP : V4L2_SEL_TGT_COMPOSE)) {
		ret = sc_v4l2_g_crop(file, fh, &crop);
		if (!ret)
			sel->r = crop.c;
		return ret;
	}
	if (sel->target != (output ? V4L2_SEL_TGT_CROP_BOUNDS :
					V4L2_SEL_TGT_COMPOSE_BOUNDS) &&
	    sel->target != (output ? V4L2_SEL_TGT_CROP_DEFAULT :
					V4L2_SEL_TGT_COMPOSE_DEFAULT))
		return -EINVAL;
	ret = sc_v4l2_cropcap(file, fh, &cap);
	if (!ret)
		sel->r = cap.bounds;
	return ret;
}

static int sc_v4l2_s_selection(struct file *file, void *fh,
				struct v4l2_selection *sel)
{
	int type = sc_selection_type(sel->type), ret;
	struct v4l2_crop crop = { .c = sel->r };

	if (type < 0)
		return type;
	if (sel->flags || sel->target != (V4L2_TYPE_IS_OUTPUT(type) ?
				V4L2_SEL_TGT_CROP : V4L2_SEL_TGT_COMPOSE))
		return -EINVAL;
	crop.type = type;
	ret = sc_v4l2_s_crop(file, fh, &crop);
	if (!ret) {
		ret = sc_v4l2_g_crop(file, fh, &crop);
		sel->r = crop.c;
	}
	return ret;
}

static const struct v4l2_ioctl_ops sc_v4l2_ioctl_ops = {
	.vidioc_querycap		= sc_v4l2_querycap,

	.vidioc_enum_fmt_vid_cap	= sc_v4l2_enum_fmt_mplane,
	.vidioc_enum_fmt_vid_out	= sc_v4l2_enum_fmt_mplane,

	.vidioc_g_fmt_vid_cap_mplane	= sc_v4l2_g_fmt_mplane,
	.vidioc_g_fmt_vid_out_mplane	= sc_v4l2_g_fmt_mplane,

	.vidioc_try_fmt_vid_cap_mplane	= sc_v4l2_try_fmt_mplane,
	.vidioc_try_fmt_vid_out_mplane	= sc_v4l2_try_fmt_mplane,

	.vidioc_s_fmt_vid_cap_mplane	= sc_v4l2_s_fmt_mplane,
	.vidioc_s_fmt_vid_out_mplane	= sc_v4l2_s_fmt_mplane,

	.vidioc_reqbufs			= sc_v4l2_reqbufs,
	.vidioc_querybuf		= sc_v4l2_querybuf,

	.vidioc_qbuf			= sc_v4l2_qbuf,
	.vidioc_dqbuf			= sc_v4l2_dqbuf,

	.vidioc_streamon		= sc_v4l2_streamon,
	.vidioc_streamoff		= sc_v4l2_streamoff,

	.vidioc_g_selection		= sc_v4l2_g_selection,
	.vidioc_s_selection		= sc_v4l2_s_selection
};

static int sc_ctx_stop_req(struct sc_ctx *ctx)
{
	struct sc_ctx *curr_ctx;
	struct sc_dev *sc = ctx->sc_dev;
	int ret = 0;

	curr_ctx = v4l2_m2m_get_curr_priv(sc->m2m.m2m_dev);
	if (!test_bit(CTX_RUN, &ctx->flags) || (curr_ctx != ctx))
		return 0;

	set_bit(CTX_ABORT, &ctx->flags);

	ret = wait_event_timeout(sc->wait,
			!test_bit(CTX_RUN, &ctx->flags),
			SC_TIMEOUT * (SC_WDT_CNT + 1));

	/* TODO: How to handle case of timeout event */
	if (ret == 0) {
		dev_err(sc->dev, "device failed to stop request\n");
		ret = -EBUSY;
	}

	return ret;
}

static void sc_calc_intbufsize(struct sc_dev *sc, struct sc_int_frame *int_frame)
{
	struct sc_frame *frame = &int_frame->frame;
	unsigned int pixsize, bytesize;

	pixsize = frame->width * frame->height;
	bytesize = (pixsize * frame->sc_fmt->bitperpixel[0]) >> 3;

	switch (frame->sc_fmt->num_comp) {
	case 1:
		frame->addr.ysize = bytesize;
		break;
	case 2:
		if (frame->sc_fmt->num_planes == 1) {
			frame->addr.ysize = pixsize;
			frame->addr.cbsize = bytesize - pixsize;
		} else if (frame->sc_fmt->num_planes == 2) {
			frame->addr.ysize =
				(pixsize * frame->sc_fmt->bitperpixel[0]) / 8;
			frame->addr.cbsize =
				(pixsize * frame->sc_fmt->bitperpixel[1]) / 8;
		}
		break;
	case 3:
		if (frame->sc_fmt->num_planes == 1) {
			if (sc_fmt_is_ayv12(frame->sc_fmt->pixelformat)) {
				unsigned int c_span;
				c_span = ALIGN(frame->width >> 1, 16);
				frame->addr.ysize = pixsize;
				frame->addr.cbsize = c_span * (frame->height >> 1);
				frame->addr.crsize = frame->addr.cbsize;
			} else {
				frame->addr.ysize = pixsize;
				frame->addr.cbsize = (bytesize - pixsize) / 2;
				frame->addr.crsize = frame->addr.cbsize;
			}
		} else if (frame->sc_fmt->num_planes == 3) {
			frame->addr.ysize =
				(pixsize * frame->sc_fmt->bitperpixel[0]) / 8;
			frame->addr.cbsize =
				(pixsize * frame->sc_fmt->bitperpixel[1]) / 8;
			frame->addr.crsize =
				(pixsize * frame->sc_fmt->bitperpixel[2]) / 8;
		} else {
			dev_err(sc->dev, "Please check the num of comp\n");
		}

		break;
	default:
		break;
	}

	memcpy(&int_frame->src_addr, &frame->addr, sizeof(int_frame->src_addr));
	memcpy(&int_frame->dst_addr, &frame->addr, sizeof(int_frame->dst_addr));
}

static void free_intermediate_frame(struct sc_ctx *ctx)
{
	struct sc_int_frame *f = ctx->i_frame;
	unsigned int i;

	if (!f)
		return;
	for (i = 0; i < 3; i++) {
		if (f->kva[i])
			dma_free_coherent(ctx->sc_dev->dev, f->alloc_size[i],
                     f->kva[i], f->dma[i]);
		f->kva[i] = NULL;
		f->dma[i] = 0;
		f->alloc_size[i] = 0;
	}
	memset(&f->src_addr, 0, sizeof(f->src_addr));
	memset(&f->dst_addr, 0, sizeof(f->dst_addr));
	f->frame.addr.y = f->frame.addr.cb = f->frame.addr.cr = 0;
}


static void destroy_intermediate_frame(struct sc_ctx *ctx)
{
	if (!ctx->i_frame)
		return;
	free_intermediate_frame(ctx);
	kfree(ctx->i_frame);
	ctx->i_frame = NULL;
	clear_bit(CTX_INT_FRAME, &ctx->flags);
}


static bool initialize_initermediate_frame(struct sc_ctx *ctx)
{
	struct sc_int_frame *f = ctx->i_frame;
	struct sc_frame *frame = &f->frame;
	size_t sizes[3];
	unsigned int i;
	bool reusable = true;

	/* Secure heaps and SMC protection are not part of this native port. */
	if (ctx->cp_enabled || test_bit(CTX_INT_FRAME_CP, &ctx->sc_dev->state))
		return false;
	frame->crop.top = frame->crop.left = 0;
	frame->width = frame->crop.width;
	frame->height = frame->crop.height;
	frame->addr.ysize = frame->addr.cbsize = frame->addr.crsize = 0;
	sc_calc_intbufsize(ctx->sc_dev, f);
	sizes[0] = PAGE_ALIGN(frame->addr.ysize);
	sizes[1] = PAGE_ALIGN(frame->addr.cbsize);
	sizes[2] = PAGE_ALIGN(frame->addr.crsize);
	if (!sizes[0])
		return false;
	for (i = 0; i < 3; i++)
		if (f->alloc_size[i] != sizes[i] || (sizes[i] && !f->kva[i]))
			reusable = false;
	if (!reusable) {
		free_intermediate_frame(ctx);
		for (i = 0; i < 3; i++) {
			if (!sizes[i])
				continue;
			f->kva[i] = dma_alloc_coherent(ctx->sc_dev->dev, sizes[i],
                                 &f->dma[i], GFP_KERNEL);
			if (!f->kva[i]) {
				free_intermediate_frame(ctx);
				return false;
			}
			f->alloc_size[i] = sizes[i];
		}
	}
	frame->addr.y = f->dma[0];
	frame->addr.cb = f->dma[1];
	frame->addr.cr = f->dma[2];
	f->src_addr = frame->addr;
	f->dst_addr = frame->addr;
	return true;
}


static bool allocate_intermediate_frame(struct sc_ctx *ctx)
{
	if (!ctx->i_frame)
		ctx->i_frame = kzalloc(sizeof(*ctx->i_frame), GFP_KERNEL);
	return ctx->i_frame != NULL;
}


/* Zoom-out range: (x1/4, x1/16] */
static int sc_prepare_2nd_scaling(struct sc_ctx *ctx,
				  __s32 src_width, __s32 src_height,
				  unsigned int *h_ratio, unsigned int *v_ratio)
{
	struct sc_dev *sc = ctx->sc_dev;
	struct v4l2_rect crop = ctx->d_frame.crop;
	const struct sc_size_limit *limit;
	unsigned int halign = 0, walign = 0;
	const struct sc_fmt *target_fmt = ctx->d_frame.sc_fmt;

	if (!allocate_intermediate_frame(ctx))
		return -ENOMEM;

	limit = &sc->variant->limit_input;
	if (*v_ratio > SCALE_RATIO_CONST(4, 1))
		crop.height = ((src_height + 7) / 8) * 2;

	if (crop.height < limit->min_h)
		crop.height = limit->min_h;

	if (*h_ratio > SCALE_RATIO_CONST(4, 1))
		crop.width = ((src_width + 7) / 8) * 2;

	if (crop.width < limit->min_w)
		crop.width = limit->min_w;

	if (*v_ratio < SCALE_RATIO_CONST(1, 8)) {
		crop.height = src_height * 8;
		if (crop.height > limit->max_h)
			crop.height = limit->max_h;
	}

	if (*h_ratio < SCALE_RATIO_CONST(1, 8)) {
		crop.width = src_width * 8;
		if (crop.width > limit->max_w)
			crop.width = limit->max_w;
	}

	walign = target_fmt->h_shift;
	halign = target_fmt->v_shift;

	limit = &sc->variant->limit_output;
	v4l_bound_align_image(&crop.width, limit->min_w, limit->max_w,
			walign, &crop.height, limit->min_h,
			limit->max_h, halign, 0);

	*h_ratio = SCALE_RATIO(src_width, crop.width);
	*v_ratio = SCALE_RATIO(src_height, crop.height);

	if ((ctx->i_frame->frame.sc_fmt != ctx->d_frame.sc_fmt) ||
		memcmp(&crop, &ctx->i_frame->frame.crop, sizeof(crop)) ||
		(ctx->cp_enabled != test_bit(CTX_INT_FRAME_CP, &sc->state))) {
		if(ctx->cp_enabled)
			set_bit(CTX_INT_FRAME_CP, &sc->state);
		else
			clear_bit(CTX_INT_FRAME_CP, &sc->state);

		memcpy(&ctx->i_frame->frame, &ctx->d_frame,
				sizeof(ctx->d_frame));
		memcpy(&ctx->i_frame->frame.crop, &crop, sizeof(crop));
		free_intermediate_frame(ctx);
		if (!initialize_initermediate_frame(ctx)) {
			free_intermediate_frame(ctx);
			return -ENOMEM;
		}
	}

	return 0;
}

static struct sc_dnoise_filter sc_filter_tab[4] = {
	{SC_FT_240,   426,  240},
	{SC_FT_480,   854,  480},
	{SC_FT_720,  1280,  720},
	{SC_FT_1080, 1920, 1080},
};

static int sc_find_filter_size(struct sc_ctx *ctx)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(sc_filter_tab); i++) {
		if (sc_filter_tab[i].strength == ctx->dnoise_ft.strength) {
			if (ctx->s_frame.width >= ctx->s_frame.height) {
				ctx->dnoise_ft.w = sc_filter_tab[i].w;
				ctx->dnoise_ft.h = sc_filter_tab[i].h;
			} else {
				ctx->dnoise_ft.w = sc_filter_tab[i].h;
				ctx->dnoise_ft.h = sc_filter_tab[i].w;
			}
			break;
		}
	}

	if (i == ARRAY_SIZE(sc_filter_tab)) {
		dev_err(ctx->sc_dev->dev,
			"%s: can't find filter size\n", __func__);
		return -EINVAL;
	}

	if (ctx->s_frame.crop.width < ctx->dnoise_ft.w ||
			ctx->s_frame.crop.height < ctx->dnoise_ft.h) {
		dev_err(ctx->sc_dev->dev,
			"%s: filter is over source size.(%dx%d -> %dx%d)\n",
			__func__, ctx->s_frame.crop.width,
			ctx->s_frame.crop.height, ctx->dnoise_ft.w,
			ctx->dnoise_ft.h);
		return -EINVAL;
	}
	return 0;
}

static int sc_prepare_denoise_filter(struct sc_ctx *ctx)
{
	unsigned int sc_down_min = ctx->sc_dev->variant->sc_down_min;

	if (ctx->dnoise_ft.strength <= SC_FT_BLUR)
		return 0;

	if (sc_find_filter_size(ctx))
		return -EINVAL;

	if (!allocate_intermediate_frame(ctx))
		return -ENOMEM;

	memcpy(&ctx->i_frame->frame, &ctx->d_frame, sizeof(ctx->d_frame));
	ctx->i_frame->frame.crop.width = ctx->dnoise_ft.w;
	ctx->i_frame->frame.crop.height = ctx->dnoise_ft.h;

	free_intermediate_frame(ctx);
	if (!initialize_initermediate_frame(ctx)) {
		free_intermediate_frame(ctx);
		dev_err(ctx->sc_dev->dev,
			"%s: failed to initialize int_frame\n", __func__);
		return -ENOMEM;
	}

	ctx->h_ratio = SCALE_RATIO(ctx->s_frame.crop.width, ctx->dnoise_ft.w);
	ctx->v_ratio = SCALE_RATIO(ctx->s_frame.crop.height, ctx->dnoise_ft.h);

	if ((ctx->h_ratio > sc_down_min) ||
			(ctx->h_ratio < ctx->sc_dev->variant->sc_up_max)) {
		dev_err(ctx->sc_dev->dev,
			"filter can't support width scaling(%d -> %d)\n",
			ctx->s_frame.crop.width, ctx->dnoise_ft.w);
		goto err_ft;
	}

	if ((ctx->v_ratio > sc_down_min) ||
			(ctx->v_ratio < ctx->sc_dev->variant->sc_up_max)) {
		dev_err(ctx->sc_dev->dev,
			"filter can't support height scaling(%d -> %d)\n",
			ctx->s_frame.crop.height, ctx->dnoise_ft.h);
		goto err_ft;
	}

	if (ctx->sc_dev->variant->prescale) {
		BUG_ON(sc_down_min != SCALE_RATIO_CONST(16, 1));

		if (ctx->h_ratio > SCALE_RATIO_CONST(8, 1))
			ctx->pre_h_ratio = 2;
		else if (ctx->h_ratio > SCALE_RATIO_CONST(4, 1))
			ctx->pre_h_ratio = 1;
		else
			ctx->pre_h_ratio = 0;

		if (ctx->v_ratio > SCALE_RATIO_CONST(8, 1))
			ctx->pre_v_ratio = 2;
		else if (ctx->v_ratio > SCALE_RATIO_CONST(4, 1))
			ctx->pre_v_ratio = 1;
		else
			ctx->pre_v_ratio = 0;

		if (ctx->pre_h_ratio || ctx->pre_v_ratio) {
			if (!IS_ALIGNED(ctx->s_frame.crop.width,
					1 << (ctx->pre_h_ratio +
					ctx->s_frame.sc_fmt->h_shift))) {
				dev_err(ctx->sc_dev->dev,
			"filter can't support not-aligned source(%d -> %d)\n",
			ctx->s_frame.crop.width, ctx->dnoise_ft.w);
				goto err_ft;
			} else if (!IS_ALIGNED(ctx->s_frame.crop.height,
					1 << (ctx->pre_v_ratio +
					ctx->s_frame.sc_fmt->v_shift))) {
				dev_err(ctx->sc_dev->dev,
			"filter can't support not-aligned source(%d -> %d)\n",
			ctx->s_frame.crop.height, ctx->dnoise_ft.h);
				goto err_ft;
			} else {
				ctx->h_ratio >>= ctx->pre_h_ratio;
				ctx->v_ratio >>= ctx->pre_v_ratio;
			}
		}
	}

	return 0;

err_ft:
	free_intermediate_frame(ctx);
	return -EINVAL;
}

static int sc_find_scaling_ratio(struct sc_ctx *ctx)
{
	__s32 src_width, src_height;
	unsigned int h_ratio, v_ratio;
	struct sc_dev *sc = ctx->sc_dev;
	unsigned int sc_down_min = sc->variant->sc_down_min;
	unsigned int sc_up_max = sc->variant->sc_up_max;

	if ((ctx->s_frame.crop.width == 0) ||
			(ctx->d_frame.crop.width == 0))
		return 0; /* s_fmt is not complete */

	src_width = ctx->s_frame.crop.width;
	src_height = ctx->s_frame.crop.height;
	if (!!(ctx->flip_rot_cfg & SCALER_ROT_90))
		swap(src_width, src_height);

	h_ratio = SCALE_RATIO(src_width, ctx->d_frame.crop.width);
	v_ratio = SCALE_RATIO(src_height, ctx->d_frame.crop.height);

	/*
	 * If the source crop width or height is fractional value
	 * calculate scaling ratio including it and calculate with original
	 * crop.width and crop.height value because they were rounded up.
	 */
	if (ctx->init_phase.w)
		h_ratio = SCALE_RATIO_FRACT((src_width - 1), ctx->init_phase.w,
				ctx->d_frame.crop.width);
	if (ctx->init_phase.h)
		v_ratio = SCALE_RATIO_FRACT((src_height - 1), ctx->init_phase.h,
				ctx->d_frame.crop.height);
	sc_dbg("Scaling ratio h_ratio %d, v_ratio %d\n", h_ratio, v_ratio);

	if ((h_ratio > sc->variant->sc_down_swmin) ||
			(h_ratio < sc->variant->sc_up_swmax)) {
		dev_err(sc->dev, "Width scaling is out of range(%d -> %d)\n",
			src_width, ctx->d_frame.crop.width);
		return -EINVAL;
	}

	if ((v_ratio > sc->variant->sc_down_swmin) ||
			(v_ratio < sc->variant->sc_up_swmax)) {
		dev_err(sc->dev, "Height scaling is out of range(%d -> %d)\n",
			src_height, ctx->d_frame.crop.height);
		return -EINVAL;
	}

	if (sc->variant->prescale) {
		BUG_ON(sc_down_min != SCALE_RATIO_CONST(16, 1));

		if (h_ratio > SCALE_RATIO_CONST(8, 1)) {
			ctx->pre_h_ratio = 2;
		} else if (h_ratio > SCALE_RATIO_CONST(4, 1)) {
			ctx->pre_h_ratio = 1;
		} else {
			ctx->pre_h_ratio = 0;
		}

		if (v_ratio > SCALE_RATIO_CONST(8, 1)) {
			ctx->pre_v_ratio = 2;
		} else if (v_ratio > SCALE_RATIO_CONST(4, 1)) {
			ctx->pre_v_ratio = 1;
		} else {
			ctx->pre_v_ratio = 0;
		}

		/*
		 * If the source image resolution violates the constraints of
		 * pre-scaler, then performs poly-phase scaling twice
		 */
		if (ctx->pre_h_ratio || ctx->pre_v_ratio) {
			if (!IS_ALIGNED(src_width, 1 << (ctx->pre_h_ratio +
					ctx->s_frame.sc_fmt->h_shift)) ||
				!IS_ALIGNED(src_height, 1 << (ctx->pre_v_ratio +
					ctx->s_frame.sc_fmt->v_shift))) {
				sc_down_min = SCALE_RATIO_CONST(4, 1);
				ctx->pre_h_ratio = 0;
				ctx->pre_v_ratio = 0;
			} else {
				h_ratio >>= ctx->pre_h_ratio;
				v_ratio >>= ctx->pre_v_ratio;
			}
		}

		if (sc_down_min == SCALE_RATIO_CONST(4, 1)) {
			dev_info(sc->dev,
			"%s: Prepared 2nd polyphase scaler (%dx%d->%dx%d)\n",
			__func__,
			ctx->s_frame.crop.width, ctx->s_frame.crop.height,
			ctx->d_frame.crop.width, ctx->d_frame.crop.height);
		}
	}

	if ((h_ratio > sc_down_min) || (v_ratio > sc_down_min)
			|| (h_ratio < sc_up_max) || (v_ratio < sc_up_max)) {
		int ret;

		ret = sc_prepare_2nd_scaling(ctx, src_width, src_height,
						&h_ratio, &v_ratio);
		if (ret)
			return ret;
	} else {
		destroy_intermediate_frame(ctx);
	}

	ctx->h_ratio = h_ratio;
	ctx->v_ratio = v_ratio;

	return 0;
}

static int sc_vb2_queue_setup(struct vb2_queue *vq, unsigned int *num_buffers,
		unsigned int *num_planes, unsigned int sizes[],
		struct device *allocators[])
{
	struct sc_ctx *ctx = vb2_get_drv_priv(vq);
	struct sc_frame *frame = ctx_get_frame(ctx, vq->type);
	unsigned int i, planes;
	int ret;

	if (IS_ERR(frame))
		return PTR_ERR(frame);
	if (!frame->sc_fmt)
		return -EINVAL;
	planes = frame->sc_fmt->num_planes;
	if (!planes || planes > SC_MAX_PLANES)
		return -EINVAL;
	if (*num_planes && *num_planes != planes)
		return -EINVAL;
	for (i = 0; i < planes; i++) {
		if (!frame->bytesused[i] ||
		    (*num_planes && sizes[i] < frame->bytesused[i]))
			return -EINVAL;
	}
	*num_planes = planes;
	for (i = 0; i < planes; i++) {
		sizes[i] = frame->bytesused[i];
		allocators[i] = ctx->sc_dev->dev;
	}
	ret = sc_find_scaling_ratio(ctx);
	if (ret)
		return ret;
	/* Never reinitialize a live vb2 queue from its queue_setup callback. */
	return sc_prepare_denoise_filter(ctx);
}


static int sc_vb2_buf_prepare(struct vb2_buffer *vb)
{
	struct sc_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct sc_frame *frame = ctx_get_frame(ctx, vb->type);
	unsigned int i;

	if (IS_ERR(frame))
		return PTR_ERR(frame);
	if (!frame->sc_fmt || vb->num_planes != frame->sc_fmt->num_planes)
		return -EINVAL;
	for (i = 0; i < vb->num_planes; i++) {
		size_t size = vb2_plane_size(vb, i);
		u32 offset = vb->planes[i].data_offset;

		if (offset > size || frame->bytesused[i] > size - offset)
			return -EINVAL;
		if (V4L2_TYPE_IS_OUTPUT(vb->type)) {
			if (vb2_get_plane_payload(vb, i) < offset ||
			    frame->bytesused[i] > vb2_get_plane_payload(vb, i) - offset)
				return -EINVAL;
		} else {
			vb2_set_plane_payload(vb, i, offset + frame->bytesused[i]);
		}
	}
	/* Imported cache ownership is transferred after the acquire fence. */
	return 0;
}


static void sc_vb2_buf_finish(struct vb2_buffer *vb)
{
	sc_buf_sync_finish(vb);
}

static void sc_vb2_buf_queue(struct vb2_buffer *vb)
{
	struct sc_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_sc_buffer *svb = container_of(to_vb2_v4l2_buffer(vb),
						struct vb2_sc_buffer, mb.vb);

	if (svb->acquire_fence)
		queue_work(ctx->sc_dev->fence_wq, &svb->work);
	else
		sc_queue_ready(svb, false);
}


static void sc_vb2_buf_cleanup(struct vb2_buffer *vb)
{
	struct vb2_sc_buffer *svb = container_of(to_vb2_v4l2_buffer(vb),
						struct vb2_sc_buffer, mb.vb);

	cancel_work_sync(&svb->work);
	dma_fence_put(xchg(&svb->acquire_fence, NULL));
}


static void sc_vb2_lock(struct vb2_queue *vq)
{
	struct sc_ctx *ctx = vb2_get_drv_priv(vq);
	mutex_lock(&ctx->sc_dev->lock);
}

static void sc_vb2_unlock(struct vb2_queue *vq)
{
	struct sc_ctx *ctx = vb2_get_drv_priv(vq);
	mutex_unlock(&ctx->sc_dev->lock);
}

static int sc_vb2_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct sc_ctx *ctx = vb2_get_drv_priv(vq);
	set_bit(CTX_STREAMING, &ctx->flags);

	return 0;
}

static void sc_vb2_stop_streaming(struct vb2_queue *vq)
{
	struct sc_ctx *ctx = vb2_get_drv_priv(vq);
	struct vb2_v4l2_buffer *vb;
	unsigned int i;
	int ret;

	mutex_lock(&ctx->fence_lock);
	ctx->stopping = true;
	mutex_unlock(&ctx->fence_lock);
	for (i = 0; i < vq->num_buffers; i++)
		sc_vb2_buf_cleanup(vq->bufs[i]);
	ret = sc_ctx_stop_req(ctx);
	if (ret < 0)
		dev_err(ctx->sc_dev->dev, "scaler stop timeout (%d)\n", ret);
	if (V4L2_TYPE_IS_OUTPUT(vq->type)) {
		while ((vb = v4l2_m2m_src_buf_remove(ctx->m2m_ctx)))
			v4l2_m2m_buf_done(vb, VB2_BUF_STATE_ERROR);
	} else {
		while ((vb = v4l2_m2m_dst_buf_remove(ctx->m2m_ctx)))
			v4l2_m2m_buf_done(vb, VB2_BUF_STATE_ERROR);
	}
	/* Canceled fence work may own an ACTIVE buffer not on the m2m list. */
	for (i = 0; i < vq->num_buffers; i++)
		if (vq->bufs[i]->state == VB2_BUF_STATE_ACTIVE)
			vb2_buffer_done(vq->bufs[i], VB2_BUF_STATE_ERROR);
	clear_bit(CTX_STREAMING, &ctx->flags);
}


static struct vb2_ops sc_vb2_ops = {
	.queue_setup		= sc_vb2_queue_setup,
	.buf_init		= sc_vb2_buf_init,
	.buf_prepare		= sc_vb2_buf_prepare,
	.buf_finish		= sc_vb2_buf_finish,
	.buf_queue		= sc_vb2_buf_queue,
	.buf_cleanup		= sc_vb2_buf_cleanup,
	.wait_finish		= sc_vb2_lock,
	.wait_prepare		= sc_vb2_unlock,
	.start_streaming	= sc_vb2_start_streaming,
	.stop_streaming		= sc_vb2_stop_streaming,
};

static int queue_init(void *priv, struct vb2_queue *src_vq,
		      struct vb2_queue *dst_vq)
{
	struct sc_ctx *ctx = priv;
	int ret;

	memset(src_vq, 0, sizeof(*src_vq));
	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	src_vq->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF;
	src_vq->ops = &sc_vb2_ops;
	src_vq->mem_ops = &vb2_dma_contig_memops;
	src_vq->dev = ctx->sc_dev->dev;
	src_vq->lock = &ctx->sc_dev->lock;
	src_vq->drv_priv = ctx;
	src_vq->buf_struct_size = sizeof(struct vb2_sc_buffer);
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;

	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	memset(dst_vq, 0, sizeof(*dst_vq));
	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	dst_vq->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF;
	dst_vq->ops = &sc_vb2_ops;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->dev = ctx->sc_dev->dev;
	dst_vq->lock = &ctx->sc_dev->lock;
	dst_vq->drv_priv = ctx;
	dst_vq->buf_struct_size = sizeof(struct vb2_sc_buffer);
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;

	return vb2_queue_init(dst_vq);
}

static bool sc_configure_rotation_degree(struct sc_ctx *ctx, int degree)
{
	ctx->flip_rot_cfg &= ~SCALER_ROT_MASK;

	/*
	 * we expect that the direction of rotation is clockwise
	 * but the Scaler does in counter clockwise.
	 * Since the GScaler doest that in clockwise,
	 * the following makes the direction of rotation by the Scaler
	 * clockwise.
	 */
	if (degree == 270) {
		ctx->flip_rot_cfg |= SCALER_ROT_90;
	} else if (degree == 180) {
		ctx->flip_rot_cfg |= SCALER_ROT_180;
	} else if (degree == 90) {
		ctx->flip_rot_cfg |= SCALER_ROT_270;
	} else if (degree != 0) {
		dev_err(ctx->sc_dev->dev,
			"%s: Rotation of %d is not supported\n",
			__func__, degree);
		return false;
	}

	return true;
}

static int sc_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct sc_ctx *ctx;
	int ret = 0;

	sc_dbg("ctrl ID:%d, value:%d\n", ctrl->id, ctrl->val);
	ctx = container_of(ctrl->handler, struct sc_ctx, ctrl_handler);

	switch (ctrl->id) {
	case V4L2_CID_VFLIP:
		if (ctrl->val)
			ctx->flip_rot_cfg |= SCALER_FLIP_X_EN;
		else
			ctx->flip_rot_cfg &= ~SCALER_FLIP_X_EN;
		break;
	case V4L2_CID_HFLIP:
		if (ctrl->val)
			ctx->flip_rot_cfg |= SCALER_FLIP_Y_EN;
		else
			ctx->flip_rot_cfg &= ~SCALER_FLIP_Y_EN;
		break;
	case V4L2_CID_ROTATE:
		if (!sc_configure_rotation_degree(ctx, ctrl->val))
			return -EINVAL;
		break;
	case V4L2_CID_GLOBAL_ALPHA:
		ctx->g_alpha = ctrl->val;
		break;
	case V4L2_CID_2D_BLEND_OP:
		if (!ctx->sc_dev->variant->blending && (ctrl->val > 0)) {
			dev_err(ctx->sc_dev->dev,
				"%s: blending is not supported from v2.2.0\n",
				__func__);
			return -EINVAL;
		}
		ctx->bl_op = ctrl->val;
		break;
	case V4L2_CID_2D_FMT_PREMULTI:
		ctx->pre_multi = ctrl->val;
		break;
	case V4L2_CID_2D_DITH:
		ctx->dith = ctrl->val;
		break;
	case V4L2_CID_CSC_EQ:
		ctx->csc.csc_eq = ctrl->val;
		break;
	case V4L2_CID_CSC_RANGE:
		ctx->csc.csc_range = ctrl->val;
		break;
	case V4L2_CID_CONTENT_PROTECTION:
		if (ctrl->val)
			return -EOPNOTSUPP;
		ctx->cp_enabled = false;
		break;
	case SC_CID_DNOISE_FT:
		ctx->dnoise_ft.strength = ctrl->val;
		break;
	}

	return ret;
}

static const struct v4l2_ctrl_ops sc_ctrl_ops = {
	.s_ctrl = sc_s_ctrl,
};

static const struct v4l2_ctrl_config sc_custom_ctrl[] = {
	{
		.ops = &sc_ctrl_ops,
		.id = V4L2_CID_GLOBAL_ALPHA,
		.name = "Set constant src alpha",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.flags = V4L2_CTRL_FLAG_SLIDER,
		.step = 1,
		.min = 0,
		.max = 255,
		.def = 255,
	}, {
		.ops = &sc_ctrl_ops,
		.id = V4L2_CID_2D_BLEND_OP,
		.name = "set blend op",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.flags = V4L2_CTRL_FLAG_SLIDER,
		.step = 1,
		.min = 0,
		.max = BL_OP_ADD,
		.def = 0,
	}, {
		.ops = &sc_ctrl_ops,
		.id = V4L2_CID_2D_DITH,
		.name = "set dithering",
		.type = V4L2_CTRL_TYPE_BOOLEAN,
		.flags = V4L2_CTRL_FLAG_SLIDER,
		.step = 1,
		.min = false,
		.max = true,
		.def = false,
	}, {
		.ops = &sc_ctrl_ops,
		.id = V4L2_CID_2D_FMT_PREMULTI,
		.name = "set pre-multiplied format",
		.type = V4L2_CTRL_TYPE_BOOLEAN,
		.flags = V4L2_CTRL_FLAG_SLIDER,
		.step = 1,
		.min = false,
		.max = true,
		.def = false,
	}, {
		.ops = &sc_ctrl_ops,
		.id = V4L2_CID_CSC_EQ,
		.name = "Set CSC equation",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.flags = V4L2_CTRL_FLAG_SLIDER,
		.step = 1,
		.min = V4L2_COLORSPACE_DEFAULT,
		.max = V4L2_COLORSPACE_DCI_P3,
		.def = V4L2_COLORSPACE_DEFAULT,
	}, {
		.ops = &sc_ctrl_ops,
		.id = V4L2_CID_CSC_RANGE,
		.name = "Set CSC range",
		.type = V4L2_CTRL_TYPE_BOOLEAN,
		.flags = V4L2_CTRL_FLAG_SLIDER,
		.step = 1,
		.min = SC_CSC_NARROW,
		.max = SC_CSC_WIDE,
		.def = SC_CSC_NARROW,
	}, {
		.ops = &sc_ctrl_ops,
		.id = V4L2_CID_CONTENT_PROTECTION,
		.name = "Enable contents protection",
		.type = V4L2_CTRL_TYPE_BOOLEAN,
		.flags = V4L2_CTRL_FLAG_SLIDER,
		.step = 1,
		.min = 0,
		.max = 1,
		.def = 0,
	}, {
		.ops = &sc_ctrl_ops,
		.id = SC_CID_DNOISE_FT,
		.name = "Enable denoising filter",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.step = 1,
		.min = 0,
		.max = SC_FT_MAX,
		.def = 0,
	}
};

static int sc_add_ctrls(struct sc_ctx *ctx)
{
	unsigned long i;

	v4l2_ctrl_handler_init(&ctx->ctrl_handler, SC_MAX_CTRL_NUM);
	v4l2_ctrl_new_std(&ctx->ctrl_handler, &sc_ctrl_ops,
			V4L2_CID_VFLIP, 0, 1, 1, 0);
	v4l2_ctrl_new_std(&ctx->ctrl_handler, &sc_ctrl_ops,
			V4L2_CID_HFLIP, 0, 1, 1, 0);
	v4l2_ctrl_new_std(&ctx->ctrl_handler, &sc_ctrl_ops,
			V4L2_CID_ROTATE, 0, 270, 90, 0);

	for (i = 0; i < ARRAY_SIZE(sc_custom_ctrl); i++)
		v4l2_ctrl_new_custom(&ctx->ctrl_handler,
				&sc_custom_ctrl[i], NULL);
	if (ctx->ctrl_handler.error) {
		int err = ctx->ctrl_handler.error;
		v4l2_err(&ctx->sc_dev->m2m.v4l2_dev,
				"v4l2_ctrl_handler_init failed %d\n", err);
		v4l2_ctrl_handler_free(&ctx->ctrl_handler);
		return err;
	}

	v4l2_ctrl_handler_setup(&ctx->ctrl_handler);

	return 0;
}

static int sc_power_clk_enable(struct sc_dev *sc)
{
	int ret;

	if (in_interrupt())
		ret = pm_runtime_get(sc->dev);
	else
		ret = pm_runtime_get_sync(sc->dev);

	if (ret < 0) {
		pm_runtime_put_noidle(sc->dev);
		dev_err(sc->dev,
			"%s=%d: Failed to enable local power\n", __func__, ret);
		return ret;
	}

	if (!IS_ERR(sc->pclk)) {
		ret = clk_enable(sc->pclk);
		if (ret) {
			dev_err(sc->dev, "%s: Failed to enable PCLK (err %d)\n",
				__func__, ret);
			goto err_pclk;
		}
	}

	if (!IS_ERR(sc->aclk)) {
		ret = clk_enable(sc->aclk);
		if (ret) {
			dev_err(sc->dev, "%s: Failed to enable ACLK (err %d)\n",
				__func__, ret);
			goto err_aclk;
		}
	}

	return 0;
err_aclk:
	if (!IS_ERR(sc->pclk))
		clk_disable(sc->pclk);
err_pclk:
	pm_runtime_put(sc->dev);
	return ret;
}

static void sc_clk_power_disable(struct sc_dev *sc)
{
	sc_clear_aux_power_cfg(sc);

	if (!IS_ERR(sc->aclk))
		clk_disable(sc->aclk);

	if (!IS_ERR(sc->pclk))
		clk_disable(sc->pclk);

	pm_runtime_put(sc->dev);
}

#ifdef CONFIG_EXYNOS_CONTENT_PATH_PROTECTION
static int sc_ctrl_protection(struct sc_dev *sc, struct sc_ctx *ctx, bool en)
{
	int ret;

	ret = exynos_smc(SMC_PROTECTION_SET, 0,
			SC_SMC_PROTECTION_ID(sc->dev_id),
			(en ? SMC_PROTECTION_ENABLE : SMC_PROTECTION_DISABLE));
	if (ret != 0) {
		dev_err(sc->dev,
			"fail to protection enable (%d)\n", ret);
	}
	return ret;
}
#endif

static int sc_open(struct file *file)
{
	struct sc_dev *sc = video_drvdata(file);
	struct sc_ctx *ctx;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		dev_err(sc->dev, "no memory for open context\n");
		return -ENOMEM;
	}

	atomic_inc(&sc->m2m.in_use);

	ctx->context_type = SC_CTX_V4L2_TYPE;
	mutex_init(&ctx->fence_lock);
	INIT_LIST_HEAD(&ctx->node);
	ctx->sc_dev = sc;

	v4l2_fh_init(&ctx->fh, sc->m2m.vfd);
	ret = sc_add_ctrls(ctx);
	if (ret)
		goto err_fh;

	ctx->fh.ctrl_handler = &ctx->ctrl_handler;
	file->private_data = &ctx->fh;
	v4l2_fh_add(&ctx->fh);

	/* Default color format */
	ctx->s_frame.sc_fmt = &sc_formats[0];
	ctx->d_frame.sc_fmt = &sc_formats[0];

	if (!IS_ERR(sc->pclk)) {
		ret = clk_prepare(sc->pclk);
		if (ret) {
			dev_err(sc->dev, "%s: failed to prepare PCLK(err %d)\n",
					__func__, ret);
			goto err_pclk_prepare;
		}
	}

	if (!IS_ERR(sc->aclk)) {
		ret = clk_prepare(sc->aclk);
		if (ret) {
			dev_err(sc->dev, "%s: failed to prepare ACLK(err %d)\n",
					__func__, ret);
			goto err_aclk_prepare;
		}
	}

	/* Setup the device context for mem2mem mode. */
	ctx->m2m_ctx = v4l2_m2m_ctx_init(sc->m2m.m2m_dev, ctx, queue_init);
	if (IS_ERR(ctx->m2m_ctx)) {
		ret = PTR_ERR(ctx->m2m_ctx);
		goto err_ctx;
	}
	ctx->fh.m2m_ctx = ctx->m2m_ctx;

	return 0;

err_ctx:
	if (!IS_ERR(sc->aclk))
		clk_unprepare(sc->aclk);
err_aclk_prepare:
	if (!IS_ERR(sc->pclk))
		clk_unprepare(sc->pclk);
err_pclk_prepare:
	v4l2_fh_del(&ctx->fh);
err_fh:
	v4l2_ctrl_handler_free(&ctx->ctrl_handler);
	v4l2_fh_exit(&ctx->fh);
	atomic_dec(&sc->m2m.in_use);
	kfree(ctx);

	return ret;
}

static int sc_release(struct file *file)
{
	struct sc_ctx *ctx = fh_to_sc_ctx(file->private_data);
	struct sc_dev *sc = ctx->sc_dev;

	sc_dbg("refcnt= %d", atomic_read(&sc->m2m.in_use));

	atomic_dec(&sc->m2m.in_use);

	v4l2_m2m_ctx_release(ctx->m2m_ctx);

	destroy_intermediate_frame(ctx);

	if (!IS_ERR(sc->aclk))
		clk_unprepare(sc->aclk);
	if (!IS_ERR(sc->pclk))
		clk_unprepare(sc->pclk);
	v4l2_ctrl_handler_free(&ctx->ctrl_handler);
	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);

	return 0;
}

static unsigned int sc_poll(struct file *file,
			     struct poll_table_struct *wait)
{
	struct sc_ctx *ctx = fh_to_sc_ctx(file->private_data);

	return v4l2_m2m_poll(file, ctx->m2m_ctx, wait);
}

static int sc_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct sc_ctx *ctx = fh_to_sc_ctx(file->private_data);

	return v4l2_m2m_mmap(file, ctx->m2m_ctx, vma);
}

static const struct v4l2_file_operations sc_v4l2_fops = {
	.owner		= THIS_MODULE,
	.open		= sc_open,
	.release	= sc_release,
	.poll		= sc_poll,
	.unlocked_ioctl	= exynos_vendor_video_ioctl,
	.mmap		= sc_mmap,
};

static int sc_run_next_job(struct sc_dev *sc);

static void sc_job_finish(struct sc_dev *sc, struct sc_ctx *ctx)
{
	unsigned long flags;
	struct vb2_v4l2_buffer *src_vb, *dst_vb;

	spin_lock_irqsave(&sc->slock, flags);

	if (ctx->context_type == SC_CTX_V4L2_TYPE) {
		ctx = v4l2_m2m_get_curr_priv(sc->m2m.m2m_dev);
		if (!ctx || !ctx->m2m_ctx) {
			dev_err(sc->dev, "current ctx is NULL\n");
			spin_unlock_irqrestore(&sc->slock, flags);
			return;

		}
		clear_bit(CTX_RUN, &ctx->flags);

#ifdef CONFIG_EXYNOS_CONTENT_PATH_PROTECTION
		if (test_bit(DEV_CP, &sc->state)) {
			sc_ctrl_protection(sc, ctx, false);
			clear_bit(DEV_CP, &sc->state);
		}
#endif
		src_vb = v4l2_m2m_src_buf_remove(ctx->m2m_ctx);
		dst_vb = v4l2_m2m_dst_buf_remove(ctx->m2m_ctx);

		if (src_vb)
			v4l2_m2m_buf_done(src_vb, VB2_BUF_STATE_ERROR);
		if (dst_vb)
			v4l2_m2m_buf_done(dst_vb, VB2_BUF_STATE_ERROR);
		clear_bit(CTX_ABORT, &ctx->flags);
		wake_up(&sc->wait);
		v4l2_m2m_job_finish(sc->m2m.m2m_dev, ctx->m2m_ctx);
	} else if (ctx->context_type == SC_CTX_M2M1SHOT_TYPE && ctx->m21_ctx) {
		struct m2m1shot_device *m21dev = ctx->m21_ctx->m21dev;

		m2m1shot_task_finish(m21dev, m2m1shot_get_current_task(m21dev),
				     true);
	} else {
		dev_err(sc->dev, "invalid scaler context type\n");
	}

	spin_unlock_irqrestore(&sc->slock, flags);
}

static void sc_watchdog(struct timer_list *timer)
{
	struct sc_dev *sc = from_timer(sc, timer, wdt.timer);
	struct sc_ctx *ctx;
	unsigned long flags;

	sc_dbg("timeout watchdog\n");
	if (atomic_read(&sc->wdt.cnt) >= SC_WDT_CNT) {
		sc_hwset_soft_reset(sc);

		atomic_set(&sc->wdt.cnt, 0);
		clear_bit(DEV_RUN, &sc->state);

		spin_lock_irqsave(&sc->ctxlist_lock, flags);
		ctx = sc->current_ctx;
		sc->current_ctx = NULL;
		spin_unlock_irqrestore(&sc->ctxlist_lock, flags);

		/* the completion IRQ finished the job meanwhile */
		if (!ctx)
			return;
		sc_job_finish(sc, ctx);
		sc_clk_power_disable(sc);
		return;
	}

	if (test_bit(DEV_RUN, &sc->state)) {
		sc_hwregs_dump(sc);
		/* Native SYSMMU fault IRQs report translation state separately. */
		atomic_inc(&sc->wdt.cnt);
		dev_err(sc->dev, "scaler is still running\n");
		mod_timer(&sc->wdt.timer, jiffies + SC_TIMEOUT);
	} else {
		sc_dbg("scaler finished job\n");
	}

}

static void sc_set_csc_coef(struct sc_ctx *ctx)
{
	struct sc_frame *s_frame, *d_frame;
	struct sc_dev *sc;
	enum sc_csc_idx idx;

	sc = ctx->sc_dev;
	s_frame = &ctx->s_frame;
	d_frame = &ctx->d_frame;

	if (s_frame->sc_fmt->is_rgb == d_frame->sc_fmt->is_rgb)
		idx = NO_CSC;
	else if (s_frame->sc_fmt->is_rgb)
		idx = CSC_R2Y;
	else
		idx = CSC_Y2R;

	sc_hwset_csc_coef(sc, idx, &ctx->csc);
}

static bool sc_process_2nd_stage(struct sc_dev *sc, struct sc_ctx *ctx)
{
	struct sc_frame *s_frame, *d_frame;
	const struct sc_size_limit *limit;
	unsigned int halign = 0, walign = 0;
	unsigned int pre_h_ratio = 0;
	unsigned int pre_v_ratio = 0;
	unsigned int h_ratio = SCALE_RATIO(1, 1);
	unsigned int v_ratio = SCALE_RATIO(1, 1);

	if (!test_bit(CTX_INT_FRAME, &ctx->flags))
		return false;

	s_frame = &ctx->i_frame->frame;
	d_frame = &ctx->d_frame;

	s_frame->addr.y = ctx->i_frame->src_addr.y;
	s_frame->addr.cb = ctx->i_frame->src_addr.cb;
	s_frame->addr.cr = ctx->i_frame->src_addr.cr;

	walign = d_frame->sc_fmt->h_shift;
	halign = d_frame->sc_fmt->v_shift;

	limit = &sc->variant->limit_input;
	v4l_bound_align_image(&s_frame->crop.width, limit->min_w, limit->max_w,
			walign, &s_frame->crop.height, limit->min_h,
			limit->max_h, halign, 0);

	sc_hwset_src_image_format(sc, s_frame->sc_fmt);
	sc_hwset_dst_image_format(sc, d_frame->sc_fmt);
	sc_hwset_src_imgsize(sc, s_frame);
	sc_hwset_dst_imgsize(sc, d_frame);

	if ((ctx->flip_rot_cfg & SCALER_ROT_90) &&
		(ctx->dnoise_ft.strength > SC_FT_BLUR)) {
		h_ratio = SCALE_RATIO(s_frame->crop.height, d_frame->crop.width);
		v_ratio = SCALE_RATIO(s_frame->crop.width, d_frame->crop.height);
	} else {
		h_ratio = SCALE_RATIO(s_frame->crop.width, d_frame->crop.width);
		v_ratio = SCALE_RATIO(s_frame->crop.height, d_frame->crop.height);
	}

	pre_h_ratio = 0;
	pre_v_ratio = 0;

	sc_hwset_polyphase_hcoef(sc, h_ratio, h_ratio, 0);
	sc_hwset_polyphase_vcoef(sc, v_ratio, v_ratio, 0);

	if (!sc->variant->ratio_20bit) {
		/* No prescaler, 1/4 precision */
		BUG_ON(h_ratio > SCALE_RATIO(4, 1));
		BUG_ON(v_ratio > SCALE_RATIO(4, 1));

		h_ratio >>= 4;
		v_ratio >>= 4;
	}

	/* no rotation */

	sc_hwset_hratio(sc, h_ratio, pre_h_ratio);
	sc_hwset_vratio(sc, v_ratio, pre_v_ratio);

	sc_hwset_src_pos(sc, s_frame->crop.left, s_frame->crop.top,
			s_frame->sc_fmt->h_shift, s_frame->sc_fmt->v_shift);
	sc_hwset_src_wh(sc, s_frame->crop.width, s_frame->crop.height,
			pre_h_ratio, pre_v_ratio,
			s_frame->sc_fmt->h_shift, s_frame->sc_fmt->v_shift);

	sc_hwset_dst_pos(sc, d_frame->crop.left, d_frame->crop.top);
	sc_hwset_dst_wh(sc, d_frame->crop.width, d_frame->crop.height);

	sc_hwset_src_addr(sc, &s_frame->addr);
	sc_hwset_dst_addr(sc, &d_frame->addr);

	if ((ctx->flip_rot_cfg & SCALER_ROT_MASK) &&
		(ctx->dnoise_ft.strength > SC_FT_BLUR))
		sc_hwset_flip_rotation(sc, ctx->flip_rot_cfg);
	else
		sc_hwset_flip_rotation(sc, 0);

	if (ctx->sc_dev->variant->q_chan_ctrl != 0)
		sc_hwset_clk_req(sc, true);

	sc_hwset_start(sc);

	clear_bit(CTX_INT_FRAME, &ctx->flags);

	return true;
}

static void sc_set_dithering(struct sc_ctx *ctx)
{
	struct sc_dev *sc = ctx->sc_dev;
	unsigned int val = 0;

	if (ctx->dith)
		val = sc_dith_val(1, 1, 1);

	sc_dbg("dither value is 0x%x\n", val);
	sc_hwset_dith(sc, val);
}

/*
 * 'Prefetch' is not required by Scaler
 * because fetch larger region is more beneficial for rotation
 */
#define SC_SRC_PBCONFIG	(SYSMMU_PBUFCFG_TLB_UPDATE |		\
			SYSMMU_PBUFCFG_ASCENDING | SYSMMU_PBUFCFG_READ)
#define SC_DST_PBCONFIG	(SYSMMU_PBUFCFG_TLB_UPDATE |		\
			SYSMMU_PBUFCFG_ASCENDING | SYSMMU_PBUFCFG_WRITE)

static void sc_set_initial_phase(struct sc_ctx *ctx)
{
	struct sc_dev *sc = ctx->sc_dev;

	/* TODO: need to check scaling, csc, rot according to H/W Goude  */
	sc_hwset_src_init_phase(sc, &ctx->init_phase);
}

static int sc_run_next_job_now(struct sc_dev *sc)
{
	unsigned long flags;
	struct sc_ctx *ctx;
	struct sc_frame *d_frame, *s_frame;
	unsigned int pre_h_ratio = 0;
	unsigned int pre_v_ratio = 0;
	unsigned int h_ratio = SCALE_RATIO(1, 1);
	unsigned int v_ratio = SCALE_RATIO(1, 1);
	unsigned int ch_ratio = SCALE_RATIO(1, 1);
	unsigned int cv_ratio = SCALE_RATIO(1, 1);
	unsigned int h_shift, v_shift;
	int ret;

	spin_lock_irqsave(&sc->ctxlist_lock, flags);

	if (sc->current_ctx || list_empty(&sc->context_list)) {
		/* a job is currently being processed or no job is to run */
		spin_unlock_irqrestore(&sc->ctxlist_lock, flags);
		return 0;
	}

	/*
	 * sc_run_next_job() must not reenter while sc->state is DEV_RUN.
	 * DEV_RUN is cleared when an operation is finished.
	 */
	BUG_ON(test_bit(DEV_RUN, &sc->state));

	set_bit(DEV_RUN, &sc->state);

	if (test_bit(DEV_SUSPEND, &sc->state)) {
		clear_bit(DEV_RUN, &sc->state);
		spin_unlock_irqrestore(&sc->ctxlist_lock, flags);
		return 0;
	}

	ctx = list_first_entry(&sc->context_list, struct sc_ctx, node);

	set_bit(CTX_RUN, &ctx->flags);
	list_del_init(&ctx->node);

	sc->current_ctx = ctx;

	spin_unlock_irqrestore(&sc->ctxlist_lock, flags);

	ret = sc_power_clk_enable(sc);
	if (ret) {
		/* Return failed buffers instead of parking an un-timed job forever. */
		spin_lock_irqsave(&sc->ctxlist_lock, flags);
		sc->current_ctx = NULL;
		clear_bit(CTX_RUN, &ctx->flags);
		clear_bit(DEV_RUN, &sc->state);
		spin_unlock_irqrestore(&sc->ctxlist_lock, flags);
		sc_job_finish(sc, ctx);
		wake_up(&sc->wait);
		return 0;
	}

	s_frame = &ctx->s_frame;
	d_frame = &ctx->d_frame;

	sc_hwset_init(sc);
	if (ctx->sc_dev->variant->q_chan_ctrl != 0)
		sc_hwset_clk_req(sc, true);


	if (ctx->i_frame) {
		set_bit(CTX_INT_FRAME, &ctx->flags);
		d_frame = &ctx->i_frame->frame;
	}

	sc_set_csc_coef(ctx);

	sc_hwset_src_image_format(sc, s_frame->sc_fmt);
	sc_hwset_dst_image_format(sc, d_frame->sc_fmt);

	sc_hwset_pre_multi_format(sc, s_frame->pre_multi, d_frame->pre_multi);

	sc_hwset_src_imgsize(sc, s_frame);
	sc_hwset_dst_imgsize(sc, d_frame);

	h_ratio = ctx->h_ratio;
	v_ratio = ctx->v_ratio;
	pre_h_ratio = ctx->pre_h_ratio;
	pre_v_ratio = ctx->pre_v_ratio;


	h_shift = s_frame->sc_fmt->h_shift;
	v_shift = s_frame->sc_fmt->v_shift;

	if (!!(ctx->flip_rot_cfg & SCALER_ROT_90)) {
		swap(pre_h_ratio, pre_v_ratio);
		swap(h_shift, v_shift);
	}

	if (h_shift < d_frame->sc_fmt->h_shift)
		ch_ratio = h_ratio * 2; /* chroma scaling down */
	else if (h_shift > d_frame->sc_fmt->h_shift)
		ch_ratio = h_ratio / 2; /* chroma scaling up */
	else
		ch_ratio = h_ratio;

	if (v_shift < d_frame->sc_fmt->v_shift)
		cv_ratio = v_ratio * 2; /* chroma scaling down */
	else if (v_shift > d_frame->sc_fmt->v_shift)
		cv_ratio = v_ratio / 2; /* chroma scaling up */
	else
		cv_ratio = v_ratio;

	sc_hwset_polyphase_hcoef(sc, h_ratio, ch_ratio,
			ctx->dnoise_ft.strength);
	sc_hwset_polyphase_vcoef(sc, v_ratio, cv_ratio,
			ctx->dnoise_ft.strength);

	if (!sc->variant->ratio_20bit) {

		/* No prescaler, 1/4 precision */
		BUG_ON(h_ratio > SCALE_RATIO(4, 1));
		BUG_ON(v_ratio > SCALE_RATIO(4, 1));

		h_ratio >>= 4;
		v_ratio >>= 4;
	}

	sc_hwset_hratio(sc, h_ratio, pre_h_ratio);
	sc_hwset_vratio(sc, v_ratio, pre_v_ratio);

	sc_hwset_src_pos(sc, s_frame->crop.left, s_frame->crop.top,
			s_frame->sc_fmt->h_shift, s_frame->sc_fmt->v_shift);
	sc_hwset_src_wh(sc, s_frame->crop.width, s_frame->crop.height,
			pre_h_ratio, pre_v_ratio,
			s_frame->sc_fmt->h_shift, s_frame->sc_fmt->v_shift);

	sc_hwset_dst_pos(sc, d_frame->crop.left, d_frame->crop.top);
	sc_hwset_dst_wh(sc, d_frame->crop.width, d_frame->crop.height);

	if (sc->variant->initphase)
		sc_set_initial_phase(ctx);

	sc_hwset_src_addr(sc, &s_frame->addr);
	sc_hwset_dst_addr(sc, &d_frame->addr);

	sc_set_dithering(ctx);

	if (ctx->bl_op)
		sc_hwset_blend(sc, ctx->bl_op, ctx->pre_multi, ctx->g_alpha);

	if (ctx->dnoise_ft.strength > SC_FT_BLUR)
		sc_hwset_flip_rotation(sc, 0);
	else
		sc_hwset_flip_rotation(sc, ctx->flip_rot_cfg);

	sc_hwset_int_en(sc);

	/* Prefetch/TLB policy belongs to the native SYSMMU provider. */

	mod_timer(&sc->wdt.timer, jiffies + SC_TIMEOUT);

	if (__measure_hw_latency) {
		if (ctx->context_type == SC_CTX_V4L2_TYPE) {
			struct vb2_v4l2_buffer *vb =
					v4l2_m2m_next_dst_buf(ctx->m2m_ctx);
			struct v4l2_m2m_buffer *mb =
					container_of(vb, typeof(*mb), vb);
			struct vb2_sc_buffer *svb =
					container_of(mb, typeof(*svb), mb);

			svb->ktime = ktime_get();
		} else {
			ctx->ktime_m2m1shot = ktime_get();
		}
	}
#ifdef CONFIG_EXYNOS_CONTENT_PATH_PROTECTION
	if (ctx->cp_enabled) {
		ret = sc_ctrl_protection(sc, ctx, true);
		if (!ret)
			set_bit(DEV_CP, &sc->state);
	}
#endif
	if (ctx->sc_dev->variant->q_chan_ctrl != 0)
		sc_hwset_clk_req(sc, true);
	sc_hwset_start(sc);

	return 0;
}


static void sc_run_work(struct work_struct *work)
{
	struct sc_dev *sc = container_of(work, struct sc_dev, run_work);

	sc_run_next_job_now(sc);
}

static int sc_run_next_job(struct sc_dev *sc)
{
	if (in_atomic() || irqs_disabled()) {
		schedule_work(&sc->run_work);
		return 0;
	}
	return sc_run_next_job_now(sc);
}

static int sc_add_context_and_run(struct sc_dev *sc, struct sc_ctx *ctx)
{
	unsigned long flags;

	if (test_bit(CTX_ABORT, &ctx->flags)) {
		dev_err(sc->dev, "aborted scaler device run\n");
		return -EAGAIN;
	}

	spin_lock_irqsave(&sc->ctxlist_lock, flags);
	list_add_tail(&ctx->node, &sc->context_list);
	spin_unlock_irqrestore(&sc->ctxlist_lock, flags);

	return sc_run_next_job(sc);
}

static irqreturn_t sc_irq_handler(int irq, void *priv)
{
	struct sc_dev *sc = priv;
	struct sc_ctx *ctx;
	struct vb2_v4l2_buffer *src_vb, *dst_vb;
	u32 irq_status;

	spin_lock(&sc->slock);

	/*
	 * ok to access sc->current_ctx withot ctxlist_lock held
	 * because it is not modified until sc_run_next_job() is called.
	 */
	ctx = sc->current_ctx;

	if (!ctx) {
		spin_unlock(&sc->slock);
		dev_warn_ratelimited(sc->dev, "scaler IRQ without an active job\n");
		return IRQ_NONE;
	}

	irq_status = sc_hwget_and_clear_irq_status(sc);
	if (ctx->sc_dev->variant->q_chan_ctrl != 0)
		sc_hwset_clk_req(sc, false);

	if (SCALER_INT_OK(irq_status) && sc_process_2nd_stage(sc, ctx))
		goto isr_unlock;

	del_timer(&sc->wdt.timer);
	/* a slow but finished job must not count against the next one */
	atomic_set(&sc->wdt.cnt, 0);

#ifdef CONFIG_EXYNOS_CONTENT_PATH_PROTECTION
	if (test_bit(DEV_CP, &sc->state)) {
		sc_ctrl_protection(sc, ctx, false);
		clear_bit(DEV_CP, &sc->state);
	}
#endif
	if (!SCALER_INT_OK(irq_status))
		sc_hwset_soft_reset(sc);

	clear_bit(DEV_RUN, &sc->state);
	clear_bit(CTX_RUN, &ctx->flags);

	if (ctx->context_type == SC_CTX_V4L2_TYPE) {
		BUG_ON(ctx != v4l2_m2m_get_curr_priv(sc->m2m.m2m_dev));

		src_vb = v4l2_m2m_src_buf_remove(ctx->m2m_ctx);
		dst_vb = v4l2_m2m_dst_buf_remove(ctx->m2m_ctx);

		BUG_ON(!src_vb || !dst_vb);

		if (__measure_hw_latency) {
			struct v4l2_m2m_buffer *mb =
					container_of(dst_vb, typeof(*mb), vb);
			struct vb2_sc_buffer *svb =
					container_of(mb, typeof(*svb), mb);

			dst_vb->vb2_buf.timestamp =
				ktime_to_ns(ktime_sub(ktime_get(), svb->ktime));
		}

		v4l2_m2m_buf_done(src_vb,
			SCALER_INT_OK(irq_status) ?
				VB2_BUF_STATE_DONE : VB2_BUF_STATE_ERROR);
		v4l2_m2m_buf_done(dst_vb,
			SCALER_INT_OK(irq_status) ?
				VB2_BUF_STATE_DONE : VB2_BUF_STATE_ERROR);

		v4l2_m2m_job_finish(sc->m2m.m2m_dev, ctx->m2m_ctx);

		/* Wake up from CTX_ABORT state */
		clear_bit(CTX_ABORT, &ctx->flags);
	} else if (ctx->context_type == SC_CTX_M2M1SHOT_TYPE && ctx->m21_ctx) {
		struct m2m1shot_device *m21dev = ctx->m21_ctx->m21dev;
		struct m2m1shot_task *task = m2m1shot_get_current_task(m21dev);

		if (__measure_hw_latency && task)
			task->task.reserved[1] = (unsigned long)ktime_us_delta(
					ktime_get(), ctx->ktime_m2m1shot);
		m2m1shot_task_finish(m21dev, task, SCALER_INT_OK(irq_status));
	} else {
		dev_err(sc->dev, "invalid scaler context type\n");
	}

	spin_lock(&sc->ctxlist_lock);
	sc->current_ctx = NULL;
	spin_unlock(&sc->ctxlist_lock);

	wake_up(&sc->wait);

	sc_run_next_job(sc);

	sc_clk_power_disable(sc);
isr_unlock:
	spin_unlock(&sc->slock);

	return IRQ_HANDLED;
}

static int sc_get_bufaddr(struct sc_dev *sc, struct vb2_buffer *vb2buf,
		struct sc_frame *frame)
{
	int ret;
	unsigned int pixsize, bytesize;
	void *cookie;

	pixsize = frame->width * frame->height;
	bytesize = (pixsize * frame->sc_fmt->bitperpixel[0]) >> 3;

	cookie = vb2_plane_cookie(vb2buf, 0);
	if (!cookie)
		return -EINVAL;

	ret = sc_get_dma_address(cookie, &frame->addr.y);
	if (!ret)
		frame->addr.y += vb2buf->planes[0].data_offset;
	if (ret != 0)
		return ret;

	frame->addr.cb = 0;
	frame->addr.cr = 0;
	frame->addr.cbsize = 0;
	frame->addr.crsize = 0;

	switch (frame->sc_fmt->num_comp) {
	case 1: /* rgb, yuyv */
		frame->addr.ysize = bytesize;
		break;
	case 2:
		if (frame->sc_fmt->num_planes == 1) {
			if (frame->sc_fmt->pixelformat == V4L2_PIX_FMT_NV12N) {
				unsigned int w = frame->width;
				unsigned int h = frame->height;
				frame->addr.cb =
					NV12N_CBCR_BASE(frame->addr.y, w, h);
				frame->addr.ysize = NV12N_Y_SIZE(w, h);
				frame->addr.cbsize = NV12N_CBCR_SIZE(w, h);
			} else if (frame->sc_fmt->pixelformat == V4L2_PIX_FMT_NV12N_10B) {
				unsigned int w = frame->width;
				unsigned int h = frame->height;
				frame->addr.cb =
					NV12N_10B_CBCR_BASE(frame->addr.y, w, h);
				frame->addr.ysize = NV12N_Y_SIZE(w, h);
				frame->addr.cbsize = NV12N_CBCR_SIZE(w, h);
			} else {
				frame->addr.cb = frame->addr.y + pixsize;
				frame->addr.ysize = pixsize;
				frame->addr.cbsize = bytesize - pixsize;
			}
		} else if (frame->sc_fmt->num_planes == 2) {
			cookie = vb2_plane_cookie(vb2buf, 1);
			if (!cookie)
				return -EINVAL;

			ret = sc_get_dma_address(cookie, &frame->addr.cb);
			if (!ret)
				frame->addr.cb += vb2buf->planes[1].data_offset;
			if (ret != 0)
				return ret;
			frame->addr.ysize =
				pixsize * frame->sc_fmt->bitperpixel[0] >> 3;
			frame->addr.cbsize =
				pixsize * frame->sc_fmt->bitperpixel[1] >> 3;
		}
		break;
	case 3:
		if (frame->sc_fmt->num_planes == 1) {
			if (sc_fmt_is_ayv12(frame->sc_fmt->pixelformat)) {
				unsigned int c_span;
				c_span = ALIGN(frame->width >> 1, 16);
				frame->addr.ysize = pixsize;
				frame->addr.cbsize = c_span * (frame->height >> 1);
				frame->addr.crsize = frame->addr.cbsize;
				frame->addr.cb = frame->addr.y + pixsize;
				frame->addr.cr = frame->addr.cb + frame->addr.cbsize;
			} else if (frame->sc_fmt->pixelformat ==
					V4L2_PIX_FMT_YUV420N) {
				unsigned int w = frame->width;
				unsigned int h = frame->height;
				frame->addr.ysize = YUV420N_Y_SIZE(w, h);
				frame->addr.cbsize = YUV420N_CB_SIZE(w, h);
				frame->addr.crsize = YUV420N_CR_SIZE(w, h);
				frame->addr.cb =
					YUV420N_CB_BASE(frame->addr.y, w, h);
				frame->addr.cr =
					YUV420N_CR_BASE(frame->addr.y, w, h);
			} else {
				frame->addr.ysize = pixsize;
				frame->addr.cbsize = (bytesize - pixsize) / 2;
				frame->addr.crsize = frame->addr.cbsize;
				frame->addr.cb = frame->addr.y + pixsize;
				frame->addr.cr = frame->addr.cb + frame->addr.cbsize;
			}
		} else if (frame->sc_fmt->num_planes == 3) {
			cookie = vb2_plane_cookie(vb2buf, 1);
			if (!cookie)
				return -EINVAL;
			ret = sc_get_dma_address(cookie, &frame->addr.cb);
			if (!ret)
				frame->addr.cb += vb2buf->planes[1].data_offset;
			if (ret != 0)
				return ret;
			cookie = vb2_plane_cookie(vb2buf, 2);
			if (!cookie)
				return -EINVAL;
			ret = sc_get_dma_address(cookie, &frame->addr.cr);
			if (!ret)
				frame->addr.cr += vb2buf->planes[2].data_offset;
			if (ret != 0)
				return ret;
			frame->addr.ysize =
				pixsize * frame->sc_fmt->bitperpixel[0] >> 3;
			frame->addr.cbsize =
				pixsize * frame->sc_fmt->bitperpixel[1] >> 3;
			frame->addr.crsize =
				pixsize * frame->sc_fmt->bitperpixel[2] >> 3;
		} else {
			dev_err(sc->dev, "Please check the num of comp\n");
		}
		break;
	default:
		break;
	}

	if (frame->sc_fmt->pixelformat == V4L2_PIX_FMT_YVU420 ||
			frame->sc_fmt->pixelformat == V4L2_PIX_FMT_YVU420M) {
		u32 t_cb = frame->addr.cb;
		frame->addr.cb = frame->addr.cr;
		frame->addr.cr = t_cb;
	}

	sc_dbg("y addr %pa y size %#x\n", &frame->addr.y, frame->addr.ysize);
	sc_dbg("cb addr %pa cb size %#x\n", &frame->addr.cb, frame->addr.cbsize);
	sc_dbg("cr addr %pa cr size %#x\n", &frame->addr.cr, frame->addr.crsize);

	return 0;
}

static void sc_m2m_device_run(void *priv)
{
	struct sc_ctx *ctx = priv;
	struct sc_dev *sc = ctx->sc_dev;
	struct vb2_buffer *src = exynos_m2m_next_src_buf(ctx->m2m_ctx);
	struct vb2_buffer *dst = exynos_m2m_next_dst_buf(ctx->m2m_ctx);

	if (!src || !dst || ctx->cp_enabled ||
	    sc_get_bufaddr(sc, src, &ctx->s_frame) ||
	    sc_get_bufaddr(sc, dst, &ctx->d_frame)) {
		sc_job_finish(sc, ctx);
		return;
	}
	v4l2_m2m_buf_copy_metadata(to_vb2_v4l2_buffer(src),
				  to_vb2_v4l2_buffer(dst), true);
	if (sc_add_context_and_run(sc, ctx))
		sc_job_finish(sc, ctx);
}

static void sc_m2m_job_abort(void *priv)
{
	struct sc_ctx *ctx = priv;
	int ret;

	ret = sc_ctx_stop_req(ctx);
	if (ret < 0)
		dev_err(ctx->sc_dev->dev, "wait timeout\n");
}

static struct v4l2_m2m_ops sc_m2m_ops = {
	.device_run	= sc_m2m_device_run,
	.job_abort	= sc_m2m_job_abort,
};

static void sc_unregister_m2m_device(struct sc_dev *sc)
{
	video_unregister_device(sc->m2m.vfd);
	v4l2_m2m_release(sc->m2m.m2m_dev);
	v4l2_device_unregister(&sc->m2m.v4l2_dev);
}

static int sc_register_m2m_device(struct sc_dev *sc, int dev_id)
{
	struct v4l2_device *v4l2_dev;
	struct device *dev;
	struct video_device *vfd;
	int ret = 0;

	dev = sc->dev;
	v4l2_dev = &sc->m2m.v4l2_dev;

	scnprintf(v4l2_dev->name, sizeof(v4l2_dev->name), "%s.m2m",
			MODULE_NAME);

	ret = v4l2_device_register(dev, v4l2_dev);
	if (ret) {
		dev_err(sc->dev, "failed to register v4l2 device\n");
		return ret;
	}

	vfd = video_device_alloc();
	if (!vfd) {
		dev_err(sc->dev, "failed to allocate video device\n");
		ret = -ENOMEM;
		goto err_v4l2_dev;
	}

	vfd->fops	= &sc_v4l2_fops;
	vfd->ioctl_ops	= &sc_v4l2_ioctl_ops;
	vfd->release	= video_device_release;
	vfd->lock	= &sc->lock;
	vfd->vfl_dir	= VFL_DIR_M2M;
	vfd->v4l2_dev	= v4l2_dev;
	vfd->device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING | 0x0100;
	set_bit(V4L2_FL_QUIRK_INVERTED_CROP, &vfd->flags);
	scnprintf(vfd->name, sizeof(vfd->name), "%s:m2m", MODULE_NAME);

	video_set_drvdata(vfd, sc);

	sc->m2m.vfd = vfd;
	sc->m2m.m2m_dev = v4l2_m2m_init(&sc_m2m_ops);
	if (IS_ERR(sc->m2m.m2m_dev)) {
		dev_err(sc->dev, "failed to initialize v4l2-m2m device\n");
		ret = PTR_ERR(sc->m2m.m2m_dev);
		goto err_dev_alloc;
	}

	ret = video_register_device(vfd, VFL_TYPE_VIDEO,
				EXYNOS_VIDEONODE_SCALER(dev_id));
	if (ret) {
		dev_err(sc->dev, "failed to register video device\n");
		goto err_m2m_dev;
	}

	return 0;

err_m2m_dev:
	v4l2_m2m_release(sc->m2m.m2m_dev);
err_dev_alloc:
	video_device_release(sc->m2m.vfd);
err_v4l2_dev:
	v4l2_device_unregister(v4l2_dev);

	return ret;
}

/*
 * Legacy Samsung m2m1shot interface (/dev/m2m1shot_scaler*), restored for
 * the Exynos libscaler/libcsc userspace (OMX decoder colour conversion).
 * Jobs share the native V4L2 path's context queue and hardware setup.
 */
static int sc_m2m1shot_init_context(struct m2m1shot_context *m21ctx)
{
	struct sc_dev *sc = dev_get_drvdata(m21ctx->m21dev->dev);
	struct sc_ctx *ctx;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	atomic_inc(&sc->m2m.in_use);

	if (!IS_ERR(sc->pclk)) {
		ret = clk_prepare(sc->pclk);
		if (ret) {
			dev_err(sc->dev, "%s: failed to prepare PCLK(err %d)\n",
					__func__, ret);
			goto err_pclk;
		}
	}

	if (!IS_ERR(sc->aclk)) {
		ret = clk_prepare(sc->aclk);
		if (ret) {
			dev_err(sc->dev, "%s: failed to prepare ACLK(err %d)\n",
					__func__, ret);
			goto err_aclk;
		}
	}

	ctx->context_type = SC_CTX_M2M1SHOT_TYPE;
	mutex_init(&ctx->fence_lock);
	INIT_LIST_HEAD(&ctx->node);
	ctx->sc_dev = sc;

	ctx->s_frame.sc_fmt = &sc_formats[0];
	ctx->d_frame.sc_fmt = &sc_formats[0];

	m21ctx->priv = ctx;
	ctx->m21_ctx = m21ctx;

	return 0;
err_aclk:
	if (!IS_ERR(sc->pclk))
		clk_unprepare(sc->pclk);
err_pclk:
	kfree(ctx);
	return ret;
}

static int sc_m2m1shot_free_context(struct m2m1shot_context *m21ctx)
{
	struct sc_ctx *ctx = m21ctx->priv;

	atomic_dec(&ctx->sc_dev->m2m.in_use);
	if (!IS_ERR(ctx->sc_dev->aclk))
		clk_unprepare(ctx->sc_dev->aclk);
	if (!IS_ERR(ctx->sc_dev->pclk))
		clk_unprepare(ctx->sc_dev->pclk);
	WARN_ON(!list_empty(&ctx->node));
	destroy_intermediate_frame(ctx);
	kfree(ctx);
	return 0;
}

static int sc_m2m1shot_prepare_format(struct m2m1shot_context *m21ctx,
			struct m2m1shot_pix_format *fmt,
			enum dma_data_direction dir,
			size_t bytes_used[])
{
	struct sc_ctx *ctx = m21ctx->priv;
	struct sc_frame *frame = (dir == DMA_TO_DEVICE) ?
					&ctx->s_frame : &ctx->d_frame;
	s32 size_min = (dir == DMA_TO_DEVICE) ? 16 : 4;
	bool crop_modified = false;
	int i;

	frame->sc_fmt = sc_find_format(ctx->sc_dev, fmt->fmt,
						(dir == DMA_TO_DEVICE));
	if (!frame->sc_fmt) {
		dev_err(ctx->sc_dev->dev,
			"%s: Pixel format %#x is not supported for %s\n",
			__func__, fmt->fmt,
			(dir == DMA_TO_DEVICE) ? "output" : "capture");
		return -EINVAL;
	}

	if ((fmt->width & frame->sc_fmt->h_shift) ||
			(fmt->height & frame->sc_fmt->v_shift)) {
		dev_err(ctx->sc_dev->dev,
			"%s: Invalid %s image size %dx%d for %s\n", __func__,
			(dir == DMA_TO_DEVICE) ? "src" : "dst",
			fmt->width, fmt->height, frame->sc_fmt->name);
	}

	if (!fmt->crop.width)
		fmt->crop.width = fmt->width;
	if (!fmt->crop.height)
		fmt->crop.height = fmt->height;

	frame->width = fmt->width;
	frame->height = fmt->height;
	frame->crop = fmt->crop;

	/* just checking for an informative message */
	if (((fmt->crop.left | fmt->crop.width) & frame->sc_fmt->h_shift) ||
		((fmt->crop.top | fmt->crop.height) & frame->sc_fmt->v_shift)) {
		dev_info(ctx->sc_dev->dev,
			"%s: shrinking %s image of %dx%d@(%d,%d) for %s\n",
			__func__, (dir == DMA_TO_DEVICE) ? "src" : "dst",
			fmt->crop.width, fmt->crop.height, fmt->crop.left,
			fmt->crop.top, frame->sc_fmt->name);
		crop_modified = true;
	}

	if (frame->crop.left & frame->sc_fmt->h_shift) {
		frame->crop.left++;
		frame->crop.width--;
	}

	if (frame->crop.top & frame->sc_fmt->v_shift) {
		frame->crop.top++;
		frame->crop.height--;
	}

	if (frame->crop.width & frame->sc_fmt->h_shift)
		frame->crop.width--;

	if (frame->crop.height & frame->sc_fmt->v_shift)
		frame->crop.height--;

	if (crop_modified)
		dev_info(ctx->sc_dev->dev,
			"%s: into %dx%d@(%d,%d)\n", __func__, frame->crop.width,
			frame->crop.height, frame->crop.left, frame->crop.top);

	if (!frame->width || !frame->height ||
				!frame->crop.width || !frame->crop.height) {
		dev_err(ctx->sc_dev->dev,
			"%s: neither width nor height can be zero\n",
			__func__);
		return -EINVAL;
	}

	if ((frame->width > 8192) || (frame->height > 8192)) {
		dev_err(ctx->sc_dev->dev,
			"%s: requested image size %dx%d exceed 8192x8192\n",
			__func__, frame->width, frame->height);
		return -EINVAL;
	}

	if ((frame->crop.width < size_min) || (frame->crop.height < size_min)) {
		dev_err(ctx->sc_dev->dev,
			"%s: image size %dx%d must not less than %dx%d\n",
			__func__, frame->width, frame->height,
			size_min, size_min);
		return -EINVAL;
	}

	if ((frame->crop.left < 0) || (frame->crop.top < 0)) {
		dev_err(ctx->sc_dev->dev,
			"%s: negative crop offset(%d, %d) is not supported\n",
			__func__, frame->crop.left, frame->crop.top);
		return -EINVAL;
	}

	if ((frame->width < (frame->crop.width + frame->crop.left)) ||
		(frame->height < (frame->crop.height + frame->crop.top))) {
		dev_err(ctx->sc_dev->dev,
			"%s: crop region(%d,%d,%d,%d) is larger than image\n",
			__func__, frame->crop.left, frame->crop.top,
			frame->crop.width, frame->crop.height);
		return -EINVAL;
	}

	for (i = 0; i < frame->sc_fmt->num_planes; i++) {
		if (sc_fmt_is_ayv12(fmt->fmt)) {
			unsigned int y_size, c_span;

			y_size = frame->width * frame->height;
			c_span = ALIGN(frame->width / 2, 16);
			bytes_used[i] = y_size;
			bytes_used[i] += (c_span * frame->height / 2) * 2;
		} else if (frame->sc_fmt->num_planes == 1 &&
			   fmt->fmt == V4L2_PIX_FMT_NV12N) {
			/* padded layout: the programmed addresses use it too */
			bytes_used[i] = NV12N_Y_SIZE(frame->width, frame->height) +
				NV12N_CBCR_SIZE(frame->width, frame->height);
		} else if (frame->sc_fmt->num_planes == 1 &&
			   fmt->fmt == V4L2_PIX_FMT_NV12N_10B) {
			bytes_used[i] = NV12N_Y_SIZE(frame->width, frame->height) +
				NV12N_10B_Y_2B_SIZE(frame->width, frame->height) +
				NV12N_CBCR_SIZE(frame->width, frame->height) +
				NV12N_10B_CBCR_2B_SIZE(frame->width, frame->height);
		} else if (frame->sc_fmt->num_planes == 1 &&
			   fmt->fmt == V4L2_PIX_FMT_YUV420N) {
			bytes_used[i] = YUV420N_Y_SIZE(frame->width, frame->height) +
				YUV420N_CB_SIZE(frame->width, frame->height) +
				YUV420N_CR_SIZE(frame->width, frame->height);
		} else {
			bytes_used[i] = frame->width * frame->height;
			bytes_used[i] *= frame->sc_fmt->bitperpixel[i];
			bytes_used[i] /= 8;
		}
	}

	return frame->sc_fmt->num_planes;
}

static int sc_m2m1shot_prepare_operation(struct m2m1shot_context *m21ctx,
						struct m2m1shot_task *task)
{
	struct sc_ctx *ctx = m21ctx->priv;
	struct m2m1shot *shot = &task->task;
	int ret;

	if (!sc_configure_rotation_degree(ctx, shot->op.rotate))
		return -EINVAL;

	ctx->flip_rot_cfg &= ~(SCALER_FLIP_X_EN | SCALER_FLIP_Y_EN);

	if (shot->op.op & M2M1SHOT_OP_FLIP_VIRT)
		ctx->flip_rot_cfg |= SCALER_FLIP_X_EN;

	if (shot->op.op & M2M1SHOT_OP_FLIP_HORI)
		ctx->flip_rot_cfg |= SCALER_FLIP_Y_EN;

	ctx->csc.csc_eq = !(shot->op.op & M2M1SHOT_OP_CSC_709) ?
			V4L2_COLORSPACE_SMPTE170M : V4L2_COLORSPACE_REC709;
	ctx->csc.csc_range = !(shot->op.op & M2M1SHOT_OP_CSC_WIDE) ?
						SC_CSC_NARROW : SC_CSC_WIDE;
	ctx->pre_multi = !!(shot->op.op & M2M1SHOT_OP_PREMULTIPLIED_ALPHA);

	ctx->dnoise_ft.strength = (shot->op.op & SC_M2M1SHOT_OP_FILTER_MASK) >>
					SC_M2M1SHOT_OP_FILTER_SHIFT;

	ret = sc_find_scaling_ratio(m21ctx->priv);
	if (ret)
		return ret;

	return sc_prepare_denoise_filter(m21ctx->priv);
}

static int sc_m2m1shot_prepare_buffer(struct m2m1shot_context *m21ctx,
			struct m2m1shot_buffer_dma *buf_dma,
			int plane,
			enum dma_data_direction dir)
{
	int ret;

	ret = m2m1shot_map_dma_buf(m21ctx->m21dev->dev,
				&buf_dma->plane[plane], dir);
	if (ret)
		return ret;

	ret = m2m1shot_dma_addr_map(m21ctx->m21dev->dev, buf_dma, plane, dir);
	if (ret) {
		m2m1shot_unmap_dma_buf(m21ctx->m21dev->dev,
					&buf_dma->plane[plane], dir);
		return ret;
	}

	m2m1shot_sync_for_device(m21ctx->m21dev->dev,
				&buf_dma->plane[plane], dir);

	return 0;
}

static void sc_m2m1shot_finish_buffer(struct m2m1shot_context *m21ctx,
			struct m2m1shot_buffer_dma *buf_dma,
			int plane,
			enum dma_data_direction dir)
{
	m2m1shot_sync_for_cpu(m21ctx->m21dev->dev,
			      &buf_dma->plane[plane], dir);
	m2m1shot_dma_addr_unmap(m21ctx->m21dev->dev, buf_dma, plane);
	m2m1shot_unmap_dma_buf(m21ctx->m21dev->dev,
				&buf_dma->plane[plane], dir);
}

static void sc_m2m1shot_get_bufaddr(struct sc_dev *sc,
			struct m2m1shot_buffer_dma *buf, struct sc_frame *frame)
{
	unsigned int pixsize, bytesize;

	pixsize = frame->width * frame->height;
	bytesize = (pixsize * frame->sc_fmt->bitperpixel[0]) >> 3;

	frame->addr.y = buf->plane[0].dma_addr;

	frame->addr.cb = 0;
	frame->addr.cr = 0;
	frame->addr.cbsize = 0;
	frame->addr.crsize = 0;

	switch (frame->sc_fmt->num_comp) {
	case 1: /* rgb, yuyv */
		frame->addr.ysize = bytesize;
		break;
	case 2:
		if (frame->sc_fmt->num_planes == 1) {
			if (frame->sc_fmt->pixelformat == V4L2_PIX_FMT_NV12N) {
				unsigned int w = frame->width;
				unsigned int h = frame->height;
				frame->addr.cb =
					NV12N_CBCR_BASE(frame->addr.y, w, h);
				frame->addr.ysize = NV12N_Y_SIZE(w, h);
				frame->addr.cbsize = NV12N_CBCR_SIZE(w, h);
			} else if (frame->sc_fmt->pixelformat == V4L2_PIX_FMT_NV12N_10B) {
				unsigned int w = frame->width;
				unsigned int h = frame->height;
				frame->addr.cb =
					NV12N_10B_CBCR_BASE(frame->addr.y, w, h);
				frame->addr.ysize = NV12N_Y_SIZE(w, h);
				frame->addr.cbsize = NV12N_CBCR_SIZE(w, h);
			} else {
				frame->addr.cb = frame->addr.y + pixsize;
				frame->addr.ysize = pixsize;
				frame->addr.cbsize = bytesize - pixsize;
			}
		} else if (frame->sc_fmt->num_planes == 2) {
			frame->addr.cb = buf->plane[1].dma_addr;

			frame->addr.ysize =
				pixsize * frame->sc_fmt->bitperpixel[0] >> 3;
			frame->addr.cbsize =
				pixsize * frame->sc_fmt->bitperpixel[1] >> 3;
		}
		break;
	case 3:
		if (frame->sc_fmt->num_planes == 1) {
			if (sc_fmt_is_ayv12(frame->sc_fmt->pixelformat)) {
				unsigned int c_span;
				c_span = ALIGN(frame->width >> 1, 16);
				frame->addr.ysize = pixsize;
				frame->addr.cbsize =
					c_span * (frame->height >> 1);
				frame->addr.crsize = frame->addr.cbsize;
				frame->addr.cb = frame->addr.y + pixsize;
				frame->addr.cr =
					frame->addr.cb + frame->addr.cbsize;
			} else if (frame->sc_fmt->pixelformat ==
					V4L2_PIX_FMT_YUV420N) {
				unsigned int w = frame->width;
				unsigned int h = frame->height;
				frame->addr.ysize = YUV420N_Y_SIZE(w, h);
				frame->addr.cbsize = YUV420N_CB_SIZE(w, h);
				frame->addr.crsize = YUV420N_CR_SIZE(w, h);
				frame->addr.cb =
					YUV420N_CB_BASE(frame->addr.y, w, h);
				frame->addr.cr =
					YUV420N_CR_BASE(frame->addr.y, w, h);
			} else {
				frame->addr.ysize = pixsize;
				frame->addr.cbsize = (bytesize - pixsize) / 2;
				frame->addr.crsize = frame->addr.cbsize;
				frame->addr.cb = frame->addr.y + pixsize;
				frame->addr.cr =
					frame->addr.cb + frame->addr.cbsize;
			}
		} else if (frame->sc_fmt->num_planes == 3) {
			frame->addr.cb = buf->plane[1].dma_addr;
			frame->addr.cr = buf->plane[2].dma_addr;

			frame->addr.ysize =
				pixsize * frame->sc_fmt->bitperpixel[0] >> 3;
			frame->addr.cbsize =
				pixsize * frame->sc_fmt->bitperpixel[1] >> 3;
			frame->addr.crsize =
				pixsize * frame->sc_fmt->bitperpixel[2] >> 3;
		} else {
			dev_err(sc->dev, "Please check the num of comp\n");
		}
		break;
	default:
		break;
	}

	if (frame->sc_fmt->pixelformat == V4L2_PIX_FMT_YVU420 ||
			frame->sc_fmt->pixelformat == V4L2_PIX_FMT_YVU420M) {
		u32 t_cb = frame->addr.cb;
		frame->addr.cb = frame->addr.cr;
		frame->addr.cr = t_cb;
	}
}

static int sc_m2m1shot_device_run(struct m2m1shot_context *m21ctx,
				struct m2m1shot_task *task)
{
	struct sc_ctx *ctx = m21ctx->priv;
	struct sc_dev *sc = ctx->sc_dev;
	struct sc_frame *s_frame, *d_frame;

	s_frame = &ctx->s_frame;
	d_frame = &ctx->d_frame;

	sc_m2m1shot_get_bufaddr(sc, &task->dma_buf_out, s_frame);
	sc_m2m1shot_get_bufaddr(sc, &task->dma_buf_cap, d_frame);

	return sc_add_context_and_run(sc, ctx);
}

static void sc_m2m1shot_timeout_task(struct m2m1shot_context *m21ctx,
		struct m2m1shot_task *task)
{
	struct sc_ctx *ctx = m21ctx->priv;
	struct sc_dev *sc = ctx->sc_dev;
	unsigned long flags;

	sc_hwregs_dump(sc);

	sc_hwset_soft_reset(sc);

	sc_clk_power_disable(sc);

	spin_lock_irqsave(&sc->ctxlist_lock, flags);
	sc->current_ctx = NULL;
	spin_unlock_irqrestore(&sc->ctxlist_lock, flags);

	clear_bit(DEV_RUN, &sc->state);
	clear_bit(CTX_RUN, &ctx->flags);

	sc_run_next_job(sc);
}

static const struct m2m1shot_devops sc_m2m1shot_ops = {
	.init_context = sc_m2m1shot_init_context,
	.free_context = sc_m2m1shot_free_context,
	.prepare_format = sc_m2m1shot_prepare_format,
	.prepare_operation = sc_m2m1shot_prepare_operation,
	.prepare_buffer = sc_m2m1shot_prepare_buffer,
	.finish_buffer = sc_m2m1shot_finish_buffer,
	.device_run = sc_m2m1shot_device_run,
	.timeout_task = sc_m2m1shot_timeout_task,
};


static int __attribute__((unused)) sc_sysmmu_fault_handler(struct iommu_domain *domain,
	struct device *dev, unsigned long iova, int flags, void *token)
{
	struct sc_dev *sc = dev_get_drvdata(dev);

	if (test_bit(DEV_RUN, &sc->state)) {
		dev_info(dev, "System MMU fault called for IOVA %#lx\n", iova);
		sc_hwregs_dump(sc);
	}

	return 0;
}

static int sc_clk_get(struct sc_dev *sc)
{
	sc->aclk = devm_clk_get(sc->dev, "gate");
	if (IS_ERR(sc->aclk)) {
		if (PTR_ERR(sc->aclk) != -ENOENT) {
			dev_err(sc->dev, "Failed to get 'gate' clock: %ld",
				PTR_ERR(sc->aclk));
			return PTR_ERR(sc->aclk);
		}
		dev_info(sc->dev, "'gate' clock is not present\n");
	}

	sc->pclk = devm_clk_get(sc->dev, "gate2");
	if (IS_ERR(sc->pclk)) {
		if (PTR_ERR(sc->pclk) != -ENOENT) {
			dev_err(sc->dev, "Failed to get 'gate2' clock: %ld",
				PTR_ERR(sc->pclk));
			return PTR_ERR(sc->pclk);
		}
		dev_info(sc->dev, "'gate2' clock is not present\n");
	}

	sc->clk_chld = devm_clk_get(sc->dev, "mux_user");
	if (IS_ERR(sc->clk_chld)) {
		if (PTR_ERR(sc->clk_chld) != -ENOENT) {
			dev_err(sc->dev, "Failed to get 'mux_user' clock: %ld",
				PTR_ERR(sc->clk_chld));
			return PTR_ERR(sc->clk_chld);
		}
		dev_info(sc->dev, "'mux_user' clock is not present\n");
	}

	if (!IS_ERR(sc->clk_chld)) {
		sc->clk_parn = devm_clk_get(sc->dev, "mux_src");
		if (IS_ERR(sc->clk_parn)) {
			dev_err(sc->dev, "Failed to get 'mux_src' clock: %ld",
				PTR_ERR(sc->clk_parn));
			return PTR_ERR(sc->clk_parn);
		}
	} else {
		sc->clk_parn = ERR_PTR(-ENOENT);
	}

	return 0;
}




#ifdef CONFIG_PM_SLEEP
static int sc_suspend(struct device *dev)
{
	struct sc_dev *sc = dev_get_drvdata(dev);

	v4l2_m2m_suspend(sc->m2m.m2m_dev);
	set_bit(DEV_SUSPEND, &sc->state);
	cancel_work_sync(&sc->run_work);
	return 0;
}

static int sc_resume(struct device *dev)
{
	struct sc_dev *sc = dev_get_drvdata(dev);

	clear_bit(DEV_SUSPEND, &sc->state);
	v4l2_m2m_resume(sc->m2m.m2m_dev);
	return 0;
}
#endif

#ifdef CONFIG_PM
static int sc_runtime_resume(struct device *dev)
{
	struct sc_dev *sc = dev_get_drvdata(dev);

	if (!IS_ERR(sc->clk_chld) && !IS_ERR(sc->clk_parn)) {
		int ret = clk_set_parent(sc->clk_chld, sc->clk_parn);
		if (ret) {
			dev_err(sc->dev, "%s: Failed to setup MUX: %d\n",
				__func__, ret);
			return ret;
		}
	}


	return 0;
}

static int sc_runtime_suspend(struct device *dev)
{
	return 0;
}
#endif

static const struct dev_pm_ops sc_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(sc_suspend, sc_resume)
	SET_RUNTIME_PM_OPS(NULL, sc_runtime_resume, sc_runtime_suspend)
};

static int sc_reboot_notifier(struct notifier_block *nb, unsigned long l, void *p)
{
	struct sc_dev *sc = container_of(nb, struct sc_dev, reboot_notifier);

	v4l2_m2m_suspend(sc->m2m.m2m_dev);
	set_bit(DEV_SUSPEND, &sc->state);
	cancel_work_sync(&sc->run_work);
	pm_runtime_barrier(sc->dev);
	return NOTIFY_DONE;
}


static int sc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);
	struct sc_dev *sc;
	struct resource *res;
	size_t i;
	u32 hwver;
	int irq, ret;
	bool found = false;

	if (!domain || domain->type != IOMMU_DOMAIN_DMA)
		return dev_err_probe(dev, -EPROBE_DEFER,
				"scaler requires a translated DMA domain\n");
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	sc = devm_kzalloc(dev, sizeof(*sc), GFP_KERNEL);
	if (!sc)
		return -ENOMEM;
	sc->dev = dev;
	sc->dev_id = of_alias_get_id(dev->of_node, "scaler");
	if (sc->dev_id != 0)
		return dev_err_probe(dev, -EINVAL, "scaler0 alias is required\n");
	spin_lock_init(&sc->ctxlist_lock);
	INIT_LIST_HEAD(&sc->context_list);
	spin_lock_init(&sc->slock);
	mutex_init(&sc->lock);
	init_waitqueue_head(&sc->wait);
	INIT_WORK(&sc->run_work, sc_run_work);
	atomic_set(&sc->wdt.cnt, 0);
	timer_setup(&sc->wdt.timer, sc_watchdog, 0);
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	sc->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(sc->regs))
		return PTR_ERR(sc->regs);
	ret = sc_clk_get(sc);
	if (ret)
		return dev_err_probe(dev, ret, "cannot get scaler clocks\n");
	if (IS_ERR(sc->aclk))
		return dev_err_probe(dev, PTR_ERR(sc->aclk), "scaler gate is required\n");
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	platform_set_drvdata(pdev, sc);
	pm_runtime_enable(dev);
	ret = pm_runtime_get_sync(dev);
	if (ret < 0) {
		pm_runtime_put_noidle(dev);
		goto disable_pm;
	}
	if (!IS_ERR(sc->pclk)) {
		ret = clk_prepare_enable(sc->pclk);
		if (ret)
			goto put_pm;
	}
	ret = clk_prepare_enable(sc->aclk);
	if (ret)
		goto put_pclk;
	hwver = readl(sc->regs + SCALER_VER);
	for (i = 0; i < ARRAY_SIZE(sc_version_table); i++) {
		if (hwver == sc_version_table[i][0]) {
			sc->version = sc_version_table[i][1];
			found = true;
			break;
		}
	}
	if (found)
		for (i = 0; i < ARRAY_SIZE(sc_variant); i++)
			if (sc->version >= sc_variant[i].version) {
				sc->variant = &sc_variant[i];
				break;
			}
	if (sc->variant)
		sc_hwset_soft_reset(sc);
	clk_disable_unprepare(sc->aclk);
	ret = sc->variant ? 0 : -ENODEV;
	if (ret)
		dev_err(dev, "unsupported scaler version %#x\n", hwver);
put_pclk:
	if (!IS_ERR(sc->pclk))
		clk_disable_unprepare(sc->pclk);
put_pm:
	pm_runtime_put(dev);
	if (ret)
		goto disable_pm;
	sc->fence_wq = alloc_ordered_workqueue("scaler-fences", WQ_MEM_RECLAIM);
	if (!sc->fence_wq) {
		ret = -ENOMEM;
		goto disable_pm;
	}
	ret = devm_request_irq(dev, irq, sc_irq_handler, 0, dev_name(dev), sc);
	if (ret)
		goto destroy_wq;
	iovmm_set_fault_handler(dev, sc_sysmmu_fault_handler, sc);
	ret = sc_register_m2m_device(sc, sc->dev_id);
	if (ret)
		goto clear_fault;
	/* /dev/m2m1shot_scaler0, as in 4.4 (the only node ueventd/SELinux allow). */
	for (i = 0; i < ARRAY_SIZE(sc->m21dev); i++) {
		sc->m21dev[i] = m2m1shot_create_device(dev, &sc_m2m1shot_ops,
						       "scaler", sc->dev_id + i, -1);
		if (IS_ERR(sc->m21dev[i])) {
			ret = PTR_ERR(sc->m21dev[i]);
			sc->m21dev[i] = NULL;
			goto destroy_m21;
		}
	}
	sc->reboot_notifier.notifier_call = sc_reboot_notifier;
	ret = register_reboot_notifier(&sc->reboot_notifier);
	if (ret)
		goto unregister_video;
	dev_info(dev, "scaler version %#x (%#x), native DMA, video50\n",
		 hwver, sc->version);
	return 0;
unregister_video:
destroy_m21:
	for (i = 0; i < ARRAY_SIZE(sc->m21dev); i++)
		if (sc->m21dev[i])
			m2m1shot_destroy_device(sc->m21dev[i]);
	sc_unregister_m2m_device(sc);
clear_fault:
	iovmm_set_fault_handler(dev, NULL, NULL);
destroy_wq:
	destroy_workqueue(sc->fence_wq);
disable_pm:
	pm_runtime_disable(dev);
	return ret;
}


static int sc_remove(struct platform_device *pdev)
{
	struct sc_dev *sc = platform_get_drvdata(pdev);

	unregister_reboot_notifier(&sc->reboot_notifier);
	v4l2_m2m_suspend(sc->m2m.m2m_dev);
	cancel_work_sync(&sc->run_work);
	del_timer_sync(&sc->wdt.timer);
	sc_unregister_m2m_device(sc);
	destroy_workqueue(sc->fence_wq);
	iovmm_set_fault_handler(sc->dev, NULL, NULL);
	pm_runtime_disable(sc->dev);
	return 0;
}


static void sc_shutdown(struct platform_device *pdev)
{
	struct sc_dev *sc = platform_get_drvdata(pdev);

	dev_info(sc->dev, "shutdown with state: %#lx\n", sc->state);
}

static const struct of_device_id exynos_sc_match[] = {
	{
		.compatible = "samsung,exynos5-scaler",
	},
	{},
};
MODULE_DEVICE_TABLE(of, exynos_sc_match);

static struct platform_driver sc_driver = {
	.probe		= sc_probe,
	.remove		= sc_remove,
	.shutdown	= sc_shutdown,
	.driver = {
		.name	= MODULE_NAME,
		.owner	= THIS_MODULE,
		.pm	= &sc_pm_ops,
		.of_match_table = of_match_ptr(exynos_sc_match),
		.suppress_bind_attrs = true,
	}
};

module_platform_driver(sc_driver);

MODULE_AUTHOR("Sunyoung, Kang <sy0816.kang@samsung.com>");
MODULE_DESCRIPTION("EXYNOS m2m scaler driver");
MODULE_LICENSE("GPL");
