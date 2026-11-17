/* SPDX-License-Identifier: GPL-2.0 */
/* 5.10 compatibility for the Exynos 4.4 MFC driver (M20 port). */
#ifndef __S5P_MFC_COMPAT_H
#define __S5P_MFC_COMPAT_H

#include <linux/sched/clock.h>
#include <linux/videodev2.h>
#include <linux/version.h>
#include <linux/ktime.h>
#include <media/videobuf2-v4l2.h>

/*
 * 5.10 removed struct timeval. The driver only uses it for V4L2 buffer
 * timestamps, whose UAPI type keeps the same tv_sec/tv_usec pair.
 */
#define timeval __kernel_v4l2_timeval

static inline int mfc_timeval_compare(const struct __kernel_v4l2_timeval *a,
				      const struct __kernel_v4l2_timeval *b)
{
	if (a->tv_sec != b->tv_sec)
		return a->tv_sec < b->tv_sec ? -1 : 1;
	if (a->tv_usec != b->tv_usec)
		return a->tv_usec < b->tv_usec ? -1 : 1;
	return 0;
}
#define timeval_compare mfc_timeval_compare

static inline struct __kernel_v4l2_timeval mfc_ktime_to_timeval(ktime_t k)
{
	struct timespec64 ts = ktime_to_timespec64(k);
	struct __kernel_v4l2_timeval tv = {
		.tv_sec = ts.tv_sec,
		.tv_usec = ts.tv_nsec / NSEC_PER_USEC,
	};

	return tv;
}
#define ktime_to_timeval mfc_ktime_to_timeval

/* vb2 keeps the V4L2 timestamp in nanoseconds since 4.x. */
static inline struct __kernel_v4l2_timeval mfc_vb_timeval(struct vb2_v4l2_buffer *b)
{
	return mfc_ktime_to_timeval(ns_to_ktime(b->vb2_buf.timestamp));
}

/* Secure (DRM) MFC instances are not supported by this port. */
#ifndef DRMDRV_OK
#define DRMDRV_OK 0
#endif

/* Minimum plane size vb2 recorded for a buffer at REQBUFS time. */
static inline unsigned int mfc_q_plane_size(struct vb2_queue *q,
					    unsigned int index, unsigned int plane)
{
	if (index >= q->num_buffers || !q->bufs[index] ||
	    plane >= q->bufs[index]->num_planes)
		return 0;
	return q->bufs[index]->planes[plane].min_length;
}

/* Samsung's vb2 copied v4l2_buffer.reserved2 (codec flags/SEI bits); carry it here. */
#define mfc_vb2_buf_r2(q, i) \
	(container_of(to_vb2_v4l2_buffer((q)->bufs[i]), struct s5p_mfc_buf, vb)->reserved2)

#define mfc_vb2_qbuf(q, b) ({						\
	if ((b)->index < (q)->num_buffers && (q)->bufs[(b)->index])	\
		mfc_vb2_buf_r2(q, (b)->index) = (b)->reserved2;		\
	vb2_qbuf(q, NULL, b);						\
})

#define mfc_vb2_dqbuf(q, b, nb) ({					\
	int __r = vb2_dqbuf(q, b, nb);					\
	if (!__r && (b)->index < (q)->num_buffers && (q)->bufs[(b)->index]) \
		(b)->reserved2 = mfc_vb2_buf_r2(q, (b)->index);		\
	__r;								\
})

#endif
