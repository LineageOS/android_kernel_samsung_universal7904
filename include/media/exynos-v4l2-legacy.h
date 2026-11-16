/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MEDIA_EXYNOS_V4L2_LEGACY_H
#define _MEDIA_EXYNOS_V4L2_LEGACY_H

#include <linux/build_bug.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>

/* Samsung's native 64-bit 4.4 format ABI used 17 plane descriptors. Keep
 * that wire layout local to these vendor nodes, never the global vb2 ABI.
 */
#define EXYNOS_LEGACY_PLANES 17
struct exynos_legacy_pix_format {
	u32 width, height, pixelformat, field, colorspace;
	struct v4l2_plane_pix_format plane_fmt[EXYNOS_LEGACY_PLANES];
	u8 num_planes, flags, ycbcr_enc, quantization, xfer_func, reserved[7];
} __packed;

struct exynos_legacy_format {
	u32 type, padding;
	union {
		struct exynos_legacy_pix_format pix_mp;
		u8 raw_data[376];
	} fmt;
};

#define EXYNOS_LEGACY_G_FMT _IOWR('V', 4, struct exynos_legacy_format)
#define EXYNOS_LEGACY_S_FMT _IOWR('V', 5, struct exynos_legacy_format)
#define EXYNOS_LEGACY_TRY_FMT _IOWR('V', 64, struct exynos_legacy_format)

static inline bool exynos_legacy_valid_type(u32 type)
{
	return type == V4L2_BUF_TYPE_VIDEO_CAPTURE ||
		type == V4L2_BUF_TYPE_VIDEO_OUTPUT ||
		type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ||
		type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
}

static inline int exynos_format_from_legacy(struct v4l2_format *dst,
				 const struct exynos_legacy_format *src,
				 bool input)
{
	const struct exynos_legacy_pix_format *old = &src->fmt.pix_mp;
	struct v4l2_pix_format_mplane *pix = &dst->fmt.pix_mp;

	BUILD_BUG_ON(sizeof(struct exynos_legacy_format) != 384);
	BUILD_BUG_ON(offsetof(struct exynos_legacy_format,
			     fmt.pix_mp.num_planes) != 368);
	if (!exynos_legacy_valid_type(src->type))
		return -EINVAL;
	memset(dst, 0, sizeof(*dst));
	dst->type = src->type;
	if (!input)
		return 0;
	if (!V4L2_TYPE_IS_MULTIPLANAR(src->type)) {
		memcpy(dst->fmt.raw_data, src->fmt.raw_data,
		       sizeof(dst->fmt.raw_data));
		return 0;
	}
	if (old->num_planes > VIDEO_MAX_PLANES)
		return -EINVAL;
	pix->width = old->width;
	pix->height = old->height;
	pix->pixelformat = old->pixelformat;
	pix->field = old->field;
	pix->colorspace = old->colorspace;
	memcpy(pix->plane_fmt, old->plane_fmt, sizeof(pix->plane_fmt));
	pix->num_planes = old->num_planes;
	pix->flags = old->flags;
	pix->ycbcr_enc = old->ycbcr_enc;
	pix->quantization = old->quantization;
	pix->xfer_func = old->xfer_func;
	return 0;
}

static inline int exynos_format_to_legacy(struct exynos_legacy_format *dst,
				 const struct v4l2_format *src)
{
	struct exynos_legacy_pix_format *old = &dst->fmt.pix_mp;
	const struct v4l2_pix_format_mplane *pix = &src->fmt.pix_mp;

	memset(dst, 0, sizeof(*dst));
	dst->type = src->type;
	if (!V4L2_TYPE_IS_MULTIPLANAR(src->type)) {
		memcpy(dst->fmt.raw_data, src->fmt.raw_data,
		       sizeof(src->fmt.raw_data));
		return 0;
	}
	if (pix->num_planes > VIDEO_MAX_PLANES)
		return -EINVAL;
	old->width = pix->width;
	old->height = pix->height;
	old->pixelformat = pix->pixelformat;
	old->field = pix->field;
	old->colorspace = pix->colorspace;
	memcpy(old->plane_fmt, pix->plane_fmt, sizeof(pix->plane_fmt));
	old->num_planes = pix->num_planes;
	old->flags = pix->flags;
	old->ycbcr_enc = pix->ycbcr_enc;
	old->quantization = pix->quantization;
	old->xfer_func = pix->xfer_func;
	return 0;
}

/*
 * 32-bit Samsung userspace (e.g. the Exynos OMX service) uses the same
 * 17-plane layout, but its union starts at offset 4: 376 bytes in total.
 * 5.10 compat passes these unknown-size commands through unchanged, so
 * the driver sees them in vidioc_default.
 */
struct exynos_legacy_format32 {
	u32 type;
	union {
		struct exynos_legacy_pix_format pix_mp;
		u8 raw_data[372];
	} fmt;
} __packed;

static inline long exynos_legacy_fmt_default(struct file *file, void *fh,
					     bool valid_prio, unsigned int cmd,
					     void *arg,
					     const struct v4l2_ioctl_ops *ops)
{
	struct exynos_legacy_format lf;
	struct v4l2_format f;
	unsigned int nr = _IOC_NR(cmd), size = _IOC_SIZE(cmd);
	bool out, mp;
	int ret;

	BUILD_BUG_ON(sizeof(struct exynos_legacy_format32) != 376);
	if (_IOC_TYPE(cmd) != 'V' || (_IOC_DIR(cmd) != (_IOC_READ | _IOC_WRITE)) ||
	    (nr != 4 && nr != 5 && nr != 64))
		return -ENOTTY;
	memset(&lf, 0, sizeof(lf));
	if (size == sizeof(struct exynos_legacy_format)) {
		memcpy(&lf, arg, sizeof(lf));
	} else if (size == sizeof(struct exynos_legacy_format32)) {
		const struct exynos_legacy_format32 *l32 = arg;

		lf.type = l32->type;
		memcpy(&lf.fmt, &l32->fmt, sizeof(l32->fmt));
	} else {
		return -ENOTTY;
	}
	ret = exynos_format_from_legacy(&f, &lf, nr != 4);
	if (ret)
		return ret;
	if (nr == 5 && !valid_prio)
		return -EBUSY;
	out = V4L2_TYPE_IS_OUTPUT(f.type);
	mp = V4L2_TYPE_IS_MULTIPLANAR(f.type);
#define EXYNOS_LEGACY_CALL(op)						\
	(ops->op ? ops->op(file, fh, &f) : -ENOTTY)
	switch (nr) {
	case 4:
		ret = mp ? (out ? EXYNOS_LEGACY_CALL(vidioc_g_fmt_vid_out_mplane) :
				  EXYNOS_LEGACY_CALL(vidioc_g_fmt_vid_cap_mplane)) :
			   (out ? EXYNOS_LEGACY_CALL(vidioc_g_fmt_vid_out) :
				  EXYNOS_LEGACY_CALL(vidioc_g_fmt_vid_cap));
		break;
	case 5:
		ret = mp ? (out ? EXYNOS_LEGACY_CALL(vidioc_s_fmt_vid_out_mplane) :
				  EXYNOS_LEGACY_CALL(vidioc_s_fmt_vid_cap_mplane)) :
			   (out ? EXYNOS_LEGACY_CALL(vidioc_s_fmt_vid_out) :
				  EXYNOS_LEGACY_CALL(vidioc_s_fmt_vid_cap));
		break;
	default:
		ret = mp ? (out ? EXYNOS_LEGACY_CALL(vidioc_try_fmt_vid_out_mplane) :
				  EXYNOS_LEGACY_CALL(vidioc_try_fmt_vid_cap_mplane)) :
			   (out ? EXYNOS_LEGACY_CALL(vidioc_try_fmt_vid_out) :
				  EXYNOS_LEGACY_CALL(vidioc_try_fmt_vid_cap));
		break;
	}
#undef EXYNOS_LEGACY_CALL
	if (ret)
		return ret;
	ret = exynos_format_to_legacy(&lf, &f);
	if (ret)
		return ret;
	if (size == sizeof(struct exynos_legacy_format)) {
		memcpy(arg, &lf, sizeof(lf));
	} else {
		struct exynos_legacy_format32 *l32 = arg;

		l32->type = lf.type;
		memcpy(&l32->fmt, &lf.fmt, sizeof(l32->fmt));
	}
	return 0;
}

static inline long exynos_legacy_format_ioctl(struct file *file,
					     unsigned int cmd,
					     unsigned long arg,
					     bool *handled)
{
	struct video_device *vdev = video_devdata(file);
	const struct v4l2_ioctl_ops *ops = vdev->ioctl_ops;
	struct exynos_legacy_format old;
	struct v4l2_format fmt;
	int (*callback)(struct file *, void *, struct v4l2_format *) = NULL;
	bool output, multi;
	int ret;

	*handled = cmd == EXYNOS_LEGACY_G_FMT || cmd == EXYNOS_LEGACY_S_FMT ||
		cmd == EXYNOS_LEGACY_TRY_FMT;
	if (!*handled)
		return -ENOIOCTLCMD;
	if (!video_is_registered(vdev))
		return -ENODEV;
	if (copy_from_user(&old, (void __user *)arg, sizeof(old)))
		return -EFAULT;
	ret = exynos_format_from_legacy(&fmt, &old, cmd != EXYNOS_LEGACY_G_FMT);
	if (ret)
		return ret;
	output = V4L2_TYPE_IS_OUTPUT(fmt.type);
	multi = V4L2_TYPE_IS_MULTIPLANAR(fmt.type);
	if ((output && vdev->vfl_dir == VFL_DIR_RX) ||
	    (!output && vdev->vfl_dir == VFL_DIR_TX))
		return -EINVAL;
	if (cmd == EXYNOS_LEGACY_G_FMT)
		callback = multi ? (output ? ops->vidioc_g_fmt_vid_out_mplane :
			ops->vidioc_g_fmt_vid_cap_mplane) :
			(output ? ops->vidioc_g_fmt_vid_out : ops->vidioc_g_fmt_vid_cap);
	else if (cmd == EXYNOS_LEGACY_S_FMT)
		callback = multi ? (output ? ops->vidioc_s_fmt_vid_out_mplane :
			ops->vidioc_s_fmt_vid_cap_mplane) :
			(output ? ops->vidioc_s_fmt_vid_out : ops->vidioc_s_fmt_vid_cap);
	else
		callback = multi ? (output ? ops->vidioc_try_fmt_vid_out_mplane :
			ops->vidioc_try_fmt_vid_cap_mplane) :
			(output ? ops->vidioc_try_fmt_vid_out : ops->vidioc_try_fmt_vid_cap);
	if (!callback)
		return -ENOTTY;
	if (vdev->lock && mutex_lock_interruptible(vdev->lock))
		return -ERESTARTSYS;
	if (cmd == EXYNOS_LEGACY_S_FMT && vdev->prio) {
		struct v4l2_fh *fh = file->private_data;

		ret = v4l2_prio_check(vdev->prio, fh->prio);
		if (ret)
			goto unlock;
	}
	ret = callback(file, file->private_data, &fmt);
unlock:
	if (vdev->lock)
		mutex_unlock(vdev->lock);
	if (ret)
		return ret;
	ret = exynos_format_to_legacy(&old, &fmt);
	if (ret)
		return ret;
	return copy_to_user((void __user *)arg, &old, sizeof(old)) ? -EFAULT : 0;
}

static inline long exynos_vendor_video_ioctl(struct file *file,
					     unsigned int cmd,
					     unsigned long arg)
{
	bool handled;
	long ret = exynos_legacy_format_ioctl(file, cmd, arg, &handled);

	return handled ? ret : video_ioctl2(file, cmd, arg);
}

#endif
