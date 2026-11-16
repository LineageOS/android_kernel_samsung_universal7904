/*
 * Copyright (c) 2015 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * The main source file of Samsung Exynos SMFC Driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/interrupt.h>
#include <linux/clk.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/exynos_iovmm.h>
#include <linux/reboot.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/mm.h>

/* Validate the Samsung length-after-offset convention before native import. */
static int smfc_capture_extent(u32 length, u32 offset, size_t dmabuf_size,
			       u32 *extent)
{
	u64 end = (u64)offset + length;

	if (length < PAGE_SIZE || end > U32_MAX || end > dmabuf_size)
		return -EINVAL;
	*extent = end;
	return 0;
}

#include <media/videobuf2-core.h>
#include <media/videobuf2-dma-contig.h>

#include "smfc.h"
#include "smfc-sync.h"

static atomic_t smfc_hwfc_state;
static wait_queue_head_t smfc_hwfc_sync_wq;
static wait_queue_head_t smfc_suspend_wq;

enum {
	SMFC_HWFC_STANDBY = 0,
	SMFC_HWFC_RUN,
	SMFC_HWFC_WAIT,
};

static void smfc_error_buffers(struct smfc_ctx *ctx)
{
	struct vb2_v4l2_buffer *vb;

	vb = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	if (vb)
		v4l2_m2m_buf_done(vb, VB2_BUF_STATE_ERROR);
	vb = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
	if (vb)
		v4l2_m2m_buf_done(vb, VB2_BUF_STATE_ERROR);
}

static irqreturn_t exynos_smfc_irq_handler(int irq, void *priv)
{
	struct smfc_dev *smfc = priv;
	struct smfc_ctx *ctx = v4l2_m2m_get_curr_priv(smfc->m2mdev);
	ktime_t ktime = ktime_get();
	unsigned long flags;
	enum vb2_buffer_state state = VB2_BUF_STATE_DONE;
	u32 streamsize = smfc_get_streamsize(smfc);
	u32 thumb_streamsize = smfc_get_2nd_streamsize(smfc);
	bool suspending = false;

	spin_lock_irqsave(&smfc->flag_lock, flags);

	if (!!(smfc->flags & SMFC_DEV_TIMEDOUT)) {
		/* The tieout handler does the rest */
		dev_err(smfc->dev, "Interrupt occurred after timed-out.\n");
		spin_unlock_irqrestore(&smfc->flag_lock, flags);
		return IRQ_HANDLED;
	}

	if (!(smfc->flags & SMFC_DEV_RUNNING)) {
		spin_unlock_irqrestore(&smfc->flag_lock, flags);
		dev_warn_ratelimited(smfc->dev, "JPEG IRQ without an active job\n");
		return IRQ_NONE;
	}

	suspending = !!(smfc->flags & SMFC_DEV_SUSPENDING);
	del_timer(&smfc->timer);
	smfc->flags &= ~(SMFC_DEV_RUNNING | SMFC_DEV_OTF_EMUMODE);

	spin_unlock_irqrestore(&smfc->flag_lock, flags);

	if (!smfc_hwstatus_okay(smfc, ctx)) {
		smfc_dump_registers(smfc);
		state = VB2_BUF_STATE_ERROR;
		smfc_hwconfigure_reset(smfc);
	}

	if (!IS_ERR(smfc->clk_gate)) {
		clk_disable(smfc->clk_gate);
		if (!IS_ERR(smfc->clk_gate2))
			clk_disable(smfc->clk_gate2);
	}

	pm_runtime_put(smfc->dev);

	atomic_set(&smfc_hwfc_state, SMFC_HWFC_STANDBY);
	wake_up(&smfc_hwfc_sync_wq);
	/* ctx is NULL if streamoff is called before (de)compression finishes */
	if (ctx) {
		struct vb2_v4l2_buffer *vb;

		vb = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (vb) {
			if (!!(ctx->flags & SMFC_CTX_COMPRESS)) {
				if (streamsize > exynos_vb2_plane_dma_size(&vb->vb2_buf, 0) ||
				    ((ctx->flags & SMFC_CTX_B2B_COMPRESS) &&
				     thumb_streamsize > exynos_vb2_plane_dma_size(&vb->vb2_buf, 1))) {
					state = VB2_BUF_STATE_ERROR;
					streamsize = thumb_streamsize = 0;
				}
				vb2_set_plane_payload(&vb->vb2_buf, 0, streamsize);
				if (!!(ctx->flags & SMFC_CTX_B2B_COMPRESS))
					vb2_set_plane_payload(&vb->vb2_buf, 1,
							thumb_streamsize);
			}

			vb->vb2_buf.timestamp = ktime_to_ns(ktime_sub(ktime,
							ctx->ktime_beg));
			v4l2_m2m_buf_done(vb, state);

		}

		vb = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		if (vb)
			v4l2_m2m_buf_done(vb, state);

		if (!suspending) {
			v4l2_m2m_job_finish(smfc->m2mdev, ctx->fh.m2m_ctx);
		} else {
			/*
			 * smfc_resume() is in charge of calling
			 * v4l2_m2m_job_finish() on resuming
			 * from Suspend To RAM
			 */
			spin_lock_irqsave(&smfc->flag_lock, flags);
			smfc->flags &= ~SMFC_DEV_SUSPENDING;
			spin_unlock_irqrestore(&smfc->flag_lock, flags);

			wake_up(&smfc_suspend_wq);
		}
	} else {
		dev_err(smfc->dev, "Spurious interrupt on H/W JPEG occurred\n");
	}

	return IRQ_HANDLED;
}

static void smfc_timedout_handler(struct timer_list *timer)
{
	struct smfc_dev *smfc = from_timer(smfc, timer, timer);
	struct smfc_ctx *ctx;
	unsigned long flags;
	bool suspending;

	spin_lock_irqsave(&smfc->flag_lock, flags);
	if (!(smfc->flags & SMFC_DEV_RUNNING)) {
		/* Interrupt is occurred before timer handler is called */
		spin_unlock_irqrestore(&smfc->flag_lock, flags);
		return;
	}

	/* Bound both ordinary DMA and HWFC jobs if the completion IRQ is lost. */
	suspending = !!(smfc->flags & SMFC_DEV_SUSPENDING);
	smfc->flags |= SMFC_DEV_TIMEDOUT; /* indicate the timedout is handled */
	smfc->flags &= ~(SMFC_DEV_RUNNING | SMFC_DEV_OTF_EMUMODE);
	spin_unlock_irqrestore(&smfc->flag_lock, flags);

	dev_err(smfc->dev, "=== TIMED-OUT! (1 sec.) =========================");
	smfc_dump_registers(smfc);
	smfc_hwconfigure_reset(smfc);
	atomic_set(&smfc_hwfc_state, SMFC_HWFC_STANDBY);
	wake_up(&smfc_hwfc_sync_wq);

	if (!IS_ERR(smfc->clk_gate)) {
		clk_disable(smfc->clk_gate);
		if (!IS_ERR(smfc->clk_gate2))
			clk_disable(smfc->clk_gate2);
	}

	pm_runtime_put(smfc->dev);

	spin_lock_irqsave(&smfc->flag_lock, flags);
	/* Drop timeout ownership before job_finish can dispatch a new owner. */
	smfc->flags &= ~SMFC_DEV_TIMEDOUT;
	spin_unlock_irqrestore(&smfc->flag_lock, flags);
	ctx = v4l2_m2m_get_curr_priv(smfc->m2mdev);
	if (ctx) {
		smfc_error_buffers(ctx);
		if (!suspending) {
			v4l2_m2m_job_finish(smfc->m2mdev, ctx->fh.m2m_ctx);
		} else {
			spin_lock_irqsave(&smfc->flag_lock, flags);
			smfc->flags &= ~SMFC_DEV_SUSPENDING;
			spin_unlock_irqrestore(&smfc->flag_lock, flags);

			wake_up(&smfc_suspend_wq);
		}
	}

}


static int smfc_vb2_queue_setup(struct vb2_queue *vq,
		unsigned int *num_buffers,
		unsigned int *num_planes, unsigned int sizes[],
		struct device *alloc_devs[])
{
	struct smfc_ctx *ctx = vb2_get_drv_priv(vq);

	if (!(ctx->flags & SMFC_CTX_COMPRESS))
		return -EOPNOTSUPP;
	if (!ctx->img_fmt || !ctx->img_fmt->num_buffers ||
	    ctx->img_fmt->num_buffers > VIDEO_MAX_PLANES / 2)
		return -EINVAL;
	if (*num_planes) {
		unsigned int required[VIDEO_MAX_PLANES] = { 0 };
		struct device *devices[VIDEO_MAX_PLANES] = { NULL };
		unsigned int planes = 0, i;
		int ret = smfc_vb2_queue_setup(vq, num_buffers, &planes,
					      required, devices);

		if (ret || *num_planes != planes)
			return ret ?: -EINVAL;
		for (i = 0; i < planes; i++)
			if (sizes[i] < required[i])
				return -EINVAL;
		return 0;
	}

	if (!(ctx->flags & SMFC_CTX_COMPRESS) && (*num_buffers > 1)) {
		dev_info(ctx->smfc->dev,
			"Decompression does not allow >1 buffers\n");
		dev_info(ctx->smfc->dev, "forced buffer count to 1\n");
		*num_buffers = 1;
	}

	if (smfc_is_compressed_type(ctx, vq->type)) {
		/*
		 * SMFC is able to stop compression if the target buffer is not
		 * enough. Therefore, it is not required to configure larger
		 * buffer for compression.
		 */
		sizes[0] = PAGE_SIZE;
		*num_planes = 1;
		alloc_devs[0] = ctx->smfc->dev;
		if (!!(ctx->flags & SMFC_CTX_B2B_COMPRESS)) {
			sizes[1] = PAGE_SIZE;
			alloc_devs[1] = ctx->smfc->dev;
			(*num_planes)++;
		}
	} else {
		unsigned int i;
		*num_planes = ctx->img_fmt->num_buffers;
		for (i = 0; i < *num_planes; i++) {
			sizes[i] = ctx->width * ctx->height;
			sizes[i] = (sizes[i] * ctx->img_fmt->bpp_buf[i]) / 8;
			alloc_devs[i] = ctx->smfc->dev;
		}

		if (!!(ctx->flags & SMFC_CTX_B2B_COMPRESS)) {
			unsigned int idx;
			for (i = 0; i < *num_planes; i++) {
				idx = *num_planes + i;
				sizes[idx] = ctx->thumb_width;
				sizes[idx] *= ctx->thumb_height;
				sizes[idx] *= ctx->img_fmt->bpp_buf[i];
				sizes[idx] /= 8;
				alloc_devs[idx] = ctx->smfc->dev;
			}

			*num_planes *= 2;
		}
	}

	return 0;
}

/*
 * The camera HAL calls the fd-less encode(), so libhwjpeg queues the JPEG
 * target as USERPTR inside a gralloc buffer it mmap()ed from a DMA-BUF.
 * 5.10 refuses to pin such VM_PFNMAP ranges, so as 4.4 vb2-ion did, find the
 * DMA-BUF behind the mapping and queue that instead. The capture queue is
 * therefore always set up as DMABUF; userspace keeps seeing USERPTR.
 */
static int smfc_userptr_to_dmabuf_fd(struct smfc_ctx *ctx, unsigned long ptr,
				     u32 length, u32 *offset, struct file **fp)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	struct file *filp = NULL;
	u64 off;
	int fd;

	if (!mm || !ptr || length < PAGE_SIZE)
		return -EINVAL;
	mmap_read_lock(mm);
	vma = find_vma(mm, ptr);
	if (vma && vma->vm_start <= ptr && vma->vm_file &&
	    (u64)ptr + length <= vma->vm_end) {
		off = (u64)(ptr - vma->vm_start) +
		      ((u64)vma->vm_pgoff << PAGE_SHIFT);
		if (off <= U32_MAX) {
			filp = vma->vm_file;
			get_file(filp);
			*offset = off;
		}
	}
	mmap_read_unlock(mm);
	if (!filp) {
		dev_err_ratelimited(ctx->smfc->dev,
			"USERPTR %#lx+%u is not a file mapping\n",
			ptr, length);
		return -EFAULT;
	}
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		fput(filp);
		return fd;
	}
	/* dma_buf_get() in the DMABUF path rejects files that are not DMA-BUFs. */
	/* Only the caller's own fd table is exposed; the file is checked on close. */
	*fp = filp;
	fd_install(fd, filp);
	return fd;
}

/* Close a temporary fd only if it still refers to the file we installed. */
static void smfc_close_temp_fd(int fd, struct file *filp)
{
	struct file *f = fget(fd);

	if (f == filp)
		__close_fd(current->files, fd);
	if (f)
		fput(f);
}

static int smfc_capture_qbuf_dmabuf(struct smfc_ctx *ctx, struct file *file,
				    void *fh, struct v4l2_buffer *b);

static int smfc_capture_qbuf_userptr(struct smfc_ctx *ctx, struct file *file,
				     void *fh, struct v4l2_buffer *b)
{
	unsigned long ptrs[2];
	u32 lengths[2], offsets[2];
	int fds[2] = { -1, -1 };
	struct file *tmpf[2] = { NULL, NULL };
	struct vb2_queue *q;
	struct vb2_buffer *vb;
	struct smfc_buffer *buf;
	unsigned int i, planes;
	int ret;

	q = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, b->type);
	if (!q || q->memory != VB2_MEMORY_DMABUF || !b->m.planes ||
	    !b->length || b->length > ARRAY_SIZE(ptrs))
		return -EINVAL;
	vb = vb2_get_buffer(q, b->index);
	if (!vb)
		return -EINVAL;
	planes = b->length;
	for (i = 0; i < planes; i++) {
		ptrs[i] = b->m.planes[i].m.userptr;
		lengths[i] = b->m.planes[i].length;
		ret = smfc_userptr_to_dmabuf_fd(ctx, ptrs[i], lengths[i],
						&offsets[i], &tmpf[i]);
		if (ret < 0)
			goto out;
		fds[i] = ret;
	}
	b->memory = V4L2_MEMORY_DMABUF;
	for (i = 0; i < planes; i++) {
		b->m.planes[i].m.fd = fds[i];
		b->m.planes[i].data_offset = offsets[i];
	}
	ret = smfc_capture_qbuf_dmabuf(ctx, file, fh, b);
	if (!ret) {
		buf = container_of(to_vb2_v4l2_buffer(vb), struct smfc_buffer, m2m.vb);
		for (i = 0; i < planes; i++) {
			buf->user_ptr[i] = ptrs[i];
			buf->user_len[i] = lengths[i];
		}
	} else {
		dev_err_ratelimited(ctx->smfc->dev,
			"USERPTR->DMABUF QBUF failed: %d ptr=%#lx off=%u len=%u\n",
			ret, ptrs[0], offsets[0], lengths[0]);
	}
	b->memory = V4L2_MEMORY_USERPTR;
	for (i = 0; i < planes; i++) {
		b->m.planes[i].m.userptr = ptrs[i];
		b->m.planes[i].length = lengths[i];
		b->m.planes[i].data_offset = 0;
	}
out:
	/* vb2 took its own DMA-BUF reference; the temporary fds go away. */
	for (i = 0; i < ARRAY_SIZE(fds); i++)
		if (fds[i] >= 0)
			smfc_close_temp_fd(fds[i], tmpf[i]);
	return ret;
}

int smfc_capture_reqbufs(struct file *file, void *fh,
			 struct v4l2_requestbuffers *rb)
{
	int ret;

	if (rb->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ||
	    rb->memory != V4L2_MEMORY_USERPTR)
		return v4l2_m2m_ioctl_reqbufs(file, fh, rb);
	rb->memory = V4L2_MEMORY_DMABUF;
	ret = v4l2_m2m_ioctl_reqbufs(file, fh, rb);
	rb->memory = V4L2_MEMORY_USERPTR;
	return ret;
}

/* Native VB2 deliberately clears CAPTURE data_offset. Keep Samsung's APP/EXIF
 * prefix in driver-owned per-buffer state, never in a global queue variable.
 */
int smfc_capture_qbuf(struct file *file, void *fh, struct v4l2_buffer *b)
{
	struct smfc_ctx *ctx = v4l2_fh_to_smfc_ctx(fh);

	if (b->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE &&
	    b->memory == V4L2_MEMORY_USERPTR)
		return smfc_capture_qbuf_userptr(ctx, file, fh, b);
	return smfc_capture_qbuf_dmabuf(ctx, file, fh, b);
}

static int smfc_capture_qbuf_dmabuf(struct smfc_ctx *ctx, struct file *file,
				    void *fh, struct v4l2_buffer *b)
{
	struct vb2_queue *q;
	struct vb2_buffer *vb;
	struct smfc_buffer *buf;
	u32 lengths[2], offsets[2], extents[2];
	unsigned int i;
	int ret;

	if (b->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ||
	    b->memory != V4L2_MEMORY_DMABUF) {
		ret = v4l2_m2m_ioctl_qbuf(file, fh, b);
		if (ret)
			dev_err_ratelimited(ctx->smfc->dev,
				"QBUF type=%u memory=%u index=%u len=%u failed: %d\n",
				b->type, b->memory, b->index, b->length, ret);
		return ret;
	}
	q = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, b->type);
	if (!q || q->type != b->type || q->memory != b->memory) {
		dev_err_ratelimited(ctx->smfc->dev,
			"QBUF queue mismatch: q=%d qmem=%u bmem=%u\n",
			!!q, q ? q->memory : 0, b->memory);
		return -EINVAL;
	}
	vb = vb2_get_buffer(q, b->index);
	if (!vb || vb->state != VB2_BUF_STATE_DEQUEUED || vb->prepared ||
	    !vb->num_planes || vb->num_planes > ARRAY_SIZE(lengths) ||
	    !b->m.planes || b->length < vb->num_planes ||
	    b->length > VIDEO_MAX_PLANES) {
		dev_err_ratelimited(ctx->smfc->dev,
			"QBUF buffer state: index=%u vb=%d state=%d prepared=%d vbplanes=%u blen=%u\n",
			b->index, !!vb, vb ? vb->state : -1, vb ? vb->prepared : -1,
			vb ? vb->num_planes : 0, b->length);
		return -EINVAL;
	}
	buf = container_of(to_vb2_v4l2_buffer(vb), struct smfc_buffer, m2m.vb);
	for (i = 0; i < vb->num_planes; i++) {
		struct dma_buf *dbuf = dma_buf_get(b->m.planes[i].m.fd);

		if (IS_ERR(dbuf))
			return PTR_ERR(dbuf);
		lengths[i] = b->m.planes[i].length;
		offsets[i] = b->m.planes[i].data_offset;
		ret = smfc_capture_extent(lengths[i], offsets[i], dbuf->size,
					 &extents[i]);
		if (ret)
			dev_err_ratelimited(ctx->smfc->dev,
				"QBUF extent plane=%u length=%u offset=%u dmabuf=%zu: %d\n",
				i, lengths[i], offsets[i], dbuf->size, ret);
		dma_buf_put(dbuf);
		if (ret)
			return ret;
	}
	for (i = 0; i < vb->num_planes; i++) {
		b->m.planes[i].length = extents[i];
		buf->capture_offset[i] = offsets[i];
	}
	ret = v4l2_m2m_ioctl_qbuf(file, fh, b);
	for (i = 0; i < vb->num_planes; i++) {
		b->m.planes[i].length = lengths[i];
		b->m.planes[i].data_offset = offsets[i];
	}
	if (ret)
		dev_err_ratelimited(ctx->smfc->dev,
			"JPEG capture QBUF failed: %d planes=%u offset=%u remaining=%u\n",
			ret, vb->num_planes, offsets[0], lengths[0]);
	return ret;
}

int smfc_capture_dqbuf(struct file *file, void *fh, struct v4l2_buffer *b)
{
	struct smfc_ctx *ctx = v4l2_fh_to_smfc_ctx(fh);
	bool userptr = b->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE &&
		       b->memory == V4L2_MEMORY_USERPTR;
	struct vb2_queue *q;
	unsigned int i;
	int ret;

	if (userptr) {
		q = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, b->type);
		if (q && q->memory == VB2_MEMORY_DMABUF)
			b->memory = V4L2_MEMORY_DMABUF;
		else
			userptr = false;
	}
	ret = v4l2_m2m_ioctl_dqbuf(file, fh, b);
	if (userptr) {
		b->memory = V4L2_MEMORY_USERPTR;
		if (!ret) {
			struct vb2_buffer *vb = vb2_get_buffer(q, b->index);
			struct smfc_buffer *buf = vb ? container_of(
				to_vb2_v4l2_buffer(vb), struct smfc_buffer,
				m2m.vb) : NULL;

			for (i = 0; buf && i < b->length && i < 2; i++) {
				b->m.planes[i].m.userptr = buf->user_ptr[i];
				b->m.planes[i].length = buf->user_len[i];
				b->m.planes[i].data_offset = 0;
			}
		}
		return ret;
	}
	if (ret || b->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ||
	    b->memory != V4L2_MEMORY_DMABUF)
		return ret;
	for (i = 0; i < b->length; i++)
		b->m.planes[i].length -= b->m.planes[i].data_offset;
	return 0;
}

static int smfc_vb2_buf_prepare(struct vb2_buffer *vb)
{
	struct smfc_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	bool output = V4L2_TYPE_IS_OUTPUT(vb->type);
	unsigned int i, buffers, planes;

	if (!(ctx->flags & SMFC_CTX_COMPRESS) || !ctx->img_fmt)
		return -EOPNOTSUPP;
	buffers = ctx->img_fmt->num_buffers;
	planes = output ? buffers : 1;
	if (ctx->flags & SMFC_CTX_B2B_COMPRESS)
		planes *= 2;
	if (!buffers || buffers > VIDEO_MAX_PLANES / 2 || vb->num_planes != planes)
		return -EINVAL;
	if (!output && vb->memory == VB2_MEMORY_DMABUF) {
		struct smfc_buffer *buf = container_of(vbuf, struct smfc_buffer, m2m.vb);

		for (i = 0; i < planes; i++)
			vb->planes[i].data_offset = buf->capture_offset[i];
	}
	for (i = 0; i < planes; i++) {
		size_t size = vb2_plane_size(vb, i);
		u32 offset = vb->planes[i].data_offset;

		if (offset >= size)
			return -EINVAL;
		if (output) {
			u32 width = i < buffers ? ctx->width : ctx->thumb_width;
			u32 height = i < buffers ? ctx->height : ctx->thumb_height;
			u64 bytes = (u64)width * height * ctx->img_fmt->bpp_buf[i % buffers] / 8;
			size_t payload = vb2_get_plane_payload(vb, i);

			if (!bytes || bytes > size - offset || payload < offset ||
			    bytes > payload - offset)
				return -EINVAL;
		}
	}
	/* HWFC reads while MCSC is writing: do not clean that producer image.
	 * The JPEG output still gets BIDI ownership for CPU APP/EXIF headers.
	 */
	if (!(vbuf->flags & V4L2_BUF_FLAG_NO_CACHE_CLEAN) &&
	    !(output && ctx->enable_hwfc))
		exynos_vb2_sync_for_device(vb);
	return 0;
}


static void smfc_vb2_buf_finish(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *v4l2_buf = to_vb2_v4l2_buffer(vb);
	if (!(v4l2_buf->flags & V4L2_BUF_FLAG_NO_CACHE_INVALIDATE))

		exynos_vb2_sync_for_cpu(vb);
}

static void smfc_vb2_buf_cleanup(struct vb2_buffer *vb)
{
	struct smfc_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	if (!V4L2_TYPE_IS_OUTPUT(vb->vb2_queue->type) &&
			!!(ctx->flags & SMFC_CTX_COMPRESS)) {
		/*
		 * clean the APP segments written by CPU in front
		 * of the start of the JPEG stream to be written by H/W
		 * for the later use of this buffer.
		 */
		exynos_vb2_sync_for_device(vb);
	}
}

static void smfc_vb2_buf_queue(struct vb2_buffer *vb)
{
	struct smfc_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = container_of(vb,
				struct vb2_v4l2_buffer, vb2_buf);
	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}

static void smfc_vb2_stop_streaming(struct vb2_queue *vq)
{
	struct vb2_v4l2_buffer *vb;
	struct smfc_ctx *ctx = vb2_get_drv_priv(vq);

	if (V4L2_TYPE_IS_OUTPUT(vq->type)) {
		while ((vb = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx)))
			v4l2_m2m_buf_done(vb, VB2_BUF_STATE_ERROR);
	} else {
		while ((vb = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx)))
			v4l2_m2m_buf_done(vb, VB2_BUF_STATE_ERROR);
	}

	vb2_wait_for_all_buffers(vq);
}

static struct vb2_ops smfc_vb2_ops = {
	.queue_setup	= smfc_vb2_queue_setup,
	.buf_prepare	= smfc_vb2_buf_prepare,
	.buf_finish	= smfc_vb2_buf_finish,
	.buf_cleanup	= smfc_vb2_buf_cleanup,
	.buf_queue	= smfc_vb2_buf_queue,
	.wait_finish	= vb2_ops_wait_finish,
	.wait_prepare	= vb2_ops_wait_prepare,
	.stop_streaming	= smfc_vb2_stop_streaming,
};

static int smfc_queue_init(void *priv, struct vb2_queue *src_vq,
		      struct vb2_queue *dst_vq)
{
	struct smfc_ctx *ctx = priv;
	int ret;

	memset(src_vq, 0, sizeof(*src_vq));
	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	src_vq->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF;
	src_vq->ops = &smfc_vb2_ops;
	src_vq->mem_ops = &vb2_dma_contig_memops;
	src_vq->dev = ctx->smfc->dev;
	src_vq->bidirectional = true;
	src_vq->drv_priv = ctx;
	src_vq->buf_struct_size = sizeof(struct smfc_buffer);
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock = &ctx->smfc->video_device_mutex;

	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	memset(dst_vq, 0, sizeof(*dst_vq));
	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	dst_vq->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF;
	dst_vq->ops = &smfc_vb2_ops;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->dev = ctx->smfc->dev;
	/* APP/EXIF header bytes are CPU-written in the JPEG output buffer. */
	dst_vq->bidirectional = true;
	dst_vq->drv_priv = ctx;
	dst_vq->buf_struct_size = sizeof(struct smfc_buffer);
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &ctx->smfc->video_device_mutex;

	return vb2_queue_init(dst_vq);
}

int exynos_smfc_wait_done(bool enable_hwfc)
{
	int prev, new;
	int ret = 0;

	ret = wait_event_interruptible_timeout(smfc_hwfc_sync_wq,
			atomic_read(&smfc_hwfc_state) < SMFC_HWFC_WAIT, 2 * HZ);
	if (ret <= 0)
		return ret ?: -ETIMEDOUT;
	ret = 0;

	prev = atomic_read(&smfc_hwfc_state);
	while (enable_hwfc && prev == SMFC_HWFC_RUN
			&& (new = atomic_cmpxchg((&smfc_hwfc_state),
					prev, SMFC_HWFC_WAIT)) != prev)
		prev = new;

	return ret;
}

static int exynos_smfc_open(struct file *filp)
{
	struct smfc_dev *smfc = video_drvdata(filp);
	struct smfc_ctx *ctx;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		dev_err(smfc->dev, "Failed to allocate smfc_ctx");
		return -ENOMEM;
	}

	ctx->smfc = smfc;

	v4l2_fh_init(&ctx->fh, smfc->videodev);

	ret = smfc_init_controls(smfc, &ctx->v4l2_ctrlhdlr);
	if (ret)
		goto err_control;

	ctx->fh.ctrl_handler = &ctx->v4l2_ctrlhdlr;

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(smfc->m2mdev, ctx, smfc_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		dev_err(smfc->dev, "Failed(%d) to init m2m_ctx\n", ret);
		goto err_m2m_ctx_init;
	}

	v4l2_fh_add(&ctx->fh);

	filp->private_data = &ctx->fh;

	if (!IS_ERR(smfc->clk_gate)) {
		ret = clk_prepare(smfc->clk_gate);
		if (ret)
			goto err_clk;
		if (!IS_ERR(smfc->clk_gate2)) {
			ret = clk_prepare(smfc->clk_gate2);
			if (ret) {
				clk_unprepare(smfc->clk_gate);
				goto err_clk;
			}
		}
	}

	/*
	 * default mode: compression
	 * default image format: YUYV
	 * default size: 16x8
	 * default thumbnail size: 0x0 (back2back comp. not enabled)
	 * default chroma subsampling for JPEG: YUV422
	 * default quality factor for compression: 96
	 * default quality factor of thumbnail for compression: 50
	 */
	ctx->img_fmt = SMFC_DEFAULT_OUTPUT_FORMAT;
	ctx->width = SMFC_MIN_WIDTH << ctx->img_fmt->chroma_hfactor;
	ctx->height = SMFC_MIN_HEIGHT << ctx->img_fmt->chroma_vfactor;
	ctx->chroma_hfactor = ctx->img_fmt->chroma_hfactor;
	ctx->chroma_vfactor = ctx->img_fmt->chroma_vfactor;
	ctx->flags |= SMFC_CTX_COMPRESS;
	ctx->quality_factor = 96;
	ctx->restart_interval = 0;
	ctx->thumb_quality_factor = 50;
	ctx->enable_hwfc = 0;

	return 0;
err_clk:
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
err_m2m_ctx_init:
	v4l2_ctrl_handler_free(&ctx->v4l2_ctrlhdlr);
err_control:
	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	return ret;
}

static int exynos_smfc_release(struct file *filp)
{
	struct smfc_ctx *ctx = v4l2_fh_to_smfc_ctx(filp->private_data);

	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_ctrl_handler_free(&ctx->v4l2_ctrlhdlr);
	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);

	if (!IS_ERR(ctx->smfc->clk_gate)) {
		clk_unprepare(ctx->smfc->clk_gate);
		if (!IS_ERR(ctx->smfc->clk_gate2))
			clk_unprepare(ctx->smfc->clk_gate2);
	}

	kfree(ctx->quantizer_tables);
	kfree(ctx->huffman_tables);

	kfree(ctx);

	return 0;
}

static const struct v4l2_file_operations smfc_v4l2_fops = {
	.owner		= THIS_MODULE,
	.open		= exynos_smfc_open,
	.release	= exynos_smfc_release,
	.poll		= v4l2_m2m_fop_poll,
	.unlocked_ioctl	= exynos_vendor_video_ioctl,
	.mmap		= v4l2_m2m_fop_mmap,
};

static bool smfc_check_hwfc_configuration(struct smfc_ctx *ctx, bool hwfc_en)
{
	const struct smfc_image_format *fmt = ctx->img_fmt;
	if (!(ctx->flags & SMFC_CTX_COMPRESS) || !hwfc_en)
		return true;

	if (fmt->chroma_hfactor == 2) {
		/* YUV422 1-plane */
		if ((fmt->chroma_vfactor == 1) && (fmt->num_planes == 1))
			return true;
		/* YUV420 2-plane */
		if ((fmt->chroma_vfactor == 2) && (fmt->num_planes == 2))
			return true;
	}

	dev_err(ctx->smfc->dev,
		"HWFC is only available with YUV420-2p and YUV422-1p\n");

	return false;
}

static void smfc_m2m_device_run(void *priv)
{
	struct smfc_ctx *ctx = priv;
	unsigned long flags;
	int ret;
	unsigned char chroma_hfactor = ctx->chroma_hfactor;
	unsigned char chroma_vfactor = ctx->chroma_vfactor;
	unsigned char restart_interval = ctx->restart_interval;
	unsigned char quality_factor = ctx->quality_factor;
	unsigned char thumb_quality_factor = ctx->thumb_quality_factor;
	unsigned char enable_hwfc = ctx->enable_hwfc;

	if (!exynos_m2m_next_src_buf(ctx->fh.m2m_ctx) ||
	    !exynos_m2m_next_dst_buf(ctx->fh.m2m_ctx) ||
	    !(ctx->flags & SMFC_CTX_COMPRESS))
		goto err_pm;
	ret = pm_runtime_get_sync(ctx->smfc->dev);
	if (ret < 0) {
		pm_runtime_put_noidle(ctx->smfc->dev);
		pr_err("Failed to enable power\n");
		goto err_pm;
	}

	if (!IS_ERR(ctx->smfc->clk_gate)) {
		ret = clk_enable(ctx->smfc->clk_gate);
		if (!ret && !IS_ERR(ctx->smfc->clk_gate2)) {
			ret = clk_enable(ctx->smfc->clk_gate2);
			if (ret)
				clk_disable(ctx->smfc->clk_gate);
		}
	}

	if (ret < 0) {
		dev_err(ctx->smfc->dev, "Failed to enable clocks\n");
		goto err_clk;
	}

	if (!smfc_check_hwfc_configuration(ctx, !!enable_hwfc))
		goto err_hwfc;

	smfc_hwconfigure_reset(ctx->smfc);

	if (!!(ctx->flags & SMFC_CTX_COMPRESS)) {
		smfc_hwconfigure_tables(ctx, quality_factor,
					ctx->ctrl_qtbl2->p_cur.p_u8);
		smfc_hwconfigure_image(ctx, chroma_hfactor, chroma_vfactor);
		if (!!(ctx->flags & SMFC_CTX_B2B_COMPRESS)) {
			smfc_hwconfigure_2nd_tables(ctx, thumb_quality_factor);
			smfc_hwconfigure_2nd_image(ctx, !!enable_hwfc);
		}
	} else {
		if ((ctx->stream_width != ctx->width) ||
				(ctx->stream_height != ctx->height)) {
			dev_err(ctx->smfc->dev,
				"Downscaling on decompression not allowed\n");
			/* It is okay to abort after reset */
			goto err_invalid_size;
		}

		smfc_hwconfigure_image(ctx,
				ctx->stream_hfactor, ctx->stream_vfactor);
		smfc_hwconfigure_tables_for_decompression(ctx);
	}

	spin_lock_irqsave(&ctx->smfc->flag_lock, flags);
	ctx->smfc->flags |= SMFC_DEV_RUNNING;
	if (!!enable_hwfc)
		ctx->smfc->flags |= SMFC_DEV_OTF_EMUMODE;
	spin_unlock_irqrestore(&ctx->smfc->flag_lock, flags);

	/*
	 * SMFC internal timer is unavailable if HWFC is enabled
	 * Therefore, S/W timer object is used to detect unexpected delay.
	 */
	if (!!enable_hwfc)
		atomic_set(&smfc_hwfc_state, SMFC_HWFC_RUN);
	mod_timer(&ctx->smfc->timer, jiffies + HZ);

	ctx->ktime_beg = ktime_get();

	smfc_hwconfigure_start(ctx, restart_interval, !!enable_hwfc);

	return;
err_invalid_size:
err_hwfc:
	if (!IS_ERR(ctx->smfc->clk_gate)) {
		clk_disable(ctx->smfc->clk_gate);
		if (!IS_ERR(ctx->smfc->clk_gate2))
			clk_disable(ctx->smfc->clk_gate2);
	}
err_clk:
	pm_runtime_put(ctx->smfc->dev);
err_pm:
	atomic_set(&smfc_hwfc_state, SMFC_HWFC_STANDBY);
	wake_up(&smfc_hwfc_sync_wq);
	smfc_error_buffers(ctx);
	/*
	 * It is safe to call v4l2_m2m_job_finish() here because .device_run()
	 * is called without any lock held
	 */
	v4l2_m2m_job_finish(ctx->smfc->m2mdev, ctx->fh.m2m_ctx);
}

static void smfc_m2m_job_abort(void *priv)
{
	/* nothing to do */
}

static struct v4l2_m2m_ops smfc_m2m_ops = {
	.device_run	= smfc_m2m_device_run,
	.job_abort	= smfc_m2m_job_abort,
};

static int smfc_init_v4l2(struct device *dev, struct smfc_dev *smfc)
{
	int ret;
	size_t str_len;

	strncpy(smfc->v4l2_dev.name, "exynos-hwjpeg",
		sizeof(smfc->v4l2_dev.name) - 1);
	smfc->v4l2_dev.name[sizeof(smfc->v4l2_dev.name) - 1] = '\0';

	ret = v4l2_device_register(dev, &smfc->v4l2_dev);
	if (ret) {
		dev_err(dev, "Failed to register v4l2 device\n");
		return ret;
	}

	smfc->videodev = video_device_alloc();
	if (!smfc->videodev) {
		dev_err(dev, "Failed to allocate video_device");
		ret = -ENOMEM;
		goto err_videodev_alloc;
	}

	str_len = sizeof(smfc->videodev->name);
	if (smfc->device_id < 0) {
		strncpy(smfc->videodev->name, MODULE_NAME, str_len);
		smfc->videodev->name[str_len - 1] = '\0';
	} else {
		scnprintf(smfc->videodev->name, str_len,
			  "%s.%d", MODULE_NAME, smfc->device_id);
	}

	mutex_init(&smfc->video_device_mutex);

	smfc->videodev->fops		= &smfc_v4l2_fops;
	smfc->videodev->ioctl_ops	= &smfc_v4l2_ioctl_ops;
	smfc->videodev->release		= video_device_release;
	smfc->videodev->lock		= &smfc->video_device_mutex;
	smfc->videodev->vfl_dir		= VFL_DIR_M2M;
	smfc->videodev->v4l2_dev	= &smfc->v4l2_dev;
	smfc->videodev->device_caps = V4L2_CAP_VIDEO_M2M |
		V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING |
		smfc->devdata->device_caps;
	/* Samsung S_CROP describes input cropping on the OUTPUT queue. */
	set_bit(V4L2_FL_QUIRK_INVERTED_CROP, &smfc->videodev->flags);

	video_set_drvdata(smfc->videodev, smfc);

	smfc->m2mdev = v4l2_m2m_init(&smfc_m2m_ops);
	if (IS_ERR(smfc->m2mdev)) {
		ret = PTR_ERR(smfc->m2mdev);
		dev_err(dev, "Failed(%d) to create v4l2_m2m_device\n", ret);
		goto err_m2m_init;
	}

	/* TODO: promote the magic number 12 in public */
	ret = video_register_device(smfc->videodev, VFL_TYPE_VIDEO, 12);
	if (ret < 0) {
		dev_err(dev, "Failed(%d) to register video_device[%d]\n",
			ret, 12);
		goto err_register;
	}

	return 0;

err_register:
	v4l2_m2m_release(smfc->m2mdev);
err_m2m_init:
	video_device_release(smfc->videodev);
err_videodev_alloc:
	v4l2_device_unregister(&smfc->v4l2_dev);
	return ret;
}

static void smfc_deinit_v4l2(struct device *dev, struct smfc_dev *smfc)
{
	video_unregister_device(smfc->videodev);
	v4l2_m2m_release(smfc->m2mdev);
	v4l2_device_unregister(&smfc->v4l2_dev);
}

static int smfc_init_clock(struct device *dev, struct smfc_dev *smfc)
{
	smfc->clk_gate = devm_clk_get(dev, "gate");
	if (IS_ERR(smfc->clk_gate)) {
		if (PTR_ERR(smfc->clk_gate) != -ENOENT) {
			dev_err(dev, "Failed(%ld) to get 'gate' clock",
				PTR_ERR(smfc->clk_gate));
			return PTR_ERR(smfc->clk_gate);
		}

		dev_info(dev, "'gate' clock is note present\n");
		smfc->clk_gate2 = ERR_PTR(-ENOENT);
		return 0;
	}

	smfc->clk_gate2 = devm_clk_get(dev, "gate2");
	if (IS_ERR(smfc->clk_gate2) && (PTR_ERR(smfc->clk_gate2) != -ENOENT)) {
		dev_err(dev, "Failed(%ld) to get 'gate2' clock\n",
				PTR_ERR(smfc->clk_gate2));
		return PTR_ERR(smfc->clk_gate2);
	}

	return 0;
}

static int smfc_find_hw_version(struct device *dev, struct smfc_dev *smfc)
{
	int ret = pm_runtime_get_sync(dev);

	if (ret < 0) {
		pm_runtime_put_noidle(dev);
		return ret;
	}
	ret = clk_prepare_enable(smfc->clk_gate);
	if (ret)
		goto put_pm;
	if (!IS_ERR(smfc->clk_gate2)) {
		ret = clk_prepare_enable(smfc->clk_gate2);
		if (ret)
			goto put_gate;
	}
	smfc->hwver = readl(smfc->reg + REG_IP_VERSION_NUMBER);
	ret = (smfc->hwver && smfc->hwver != U32_MAX) ? 0 : -ENODEV;
	if (!ret)
		smfc_hwconfigure_reset(smfc);
	if (ret)
		dev_err(dev, "invalid JPEG version %#x\n", smfc->hwver);
	if (!IS_ERR(smfc->clk_gate2))
		clk_disable_unprepare(smfc->clk_gate2);
put_gate:
	clk_disable_unprepare(smfc->clk_gate);
put_pm:
	pm_runtime_put(dev);
	return ret;
}

static int __attribute__((unused)) smfc_iommu_fault_handler(
		struct iommu_domain *domain, struct device *dev,
		unsigned long fault_addr, int fault_flags, void *token)
{
	struct smfc_dev *smfc = token;

	if (smfc->flags & SMFC_DEV_RUNNING)
		smfc_dump_registers(smfc);

	return 0;
}

/* The M20 uses the 8890-style encoder register layout. Decompression is
 * deliberately not advertised: its old USERPTR parser is not yet ported or
 * audited for the native DMA-BUF interface. Camera JPEG encoding needs no
 * stream parser, and failed decoder format requests must not change mode.
 */
static const struct smfc_device_data smfc_7904_data = {
	.device_caps = V4L2_CAP_EXYNOS_JPEG_B2B_COMPRESSION |
		V4L2_CAP_EXYNOS_JPEG_HWFC |
		V4L2_CAP_EXYNOS_JPEG_NO_STREAMBASE_ALIGN |
		V4L2_CAP_EXYNOS_JPEG_NO_IMAGEBASE_ALIGN |
		V4L2_CAP_EXYNOS_JPEG_DMABUF_OFFSET,
	.burstlenth_bits = 4,
};

static const struct of_device_id exynos_smfc_match[] = {
	{ .compatible = "samsung,exynos7904-jpeg", .data = &smfc_7904_data },
	{ },
};
MODULE_DEVICE_TABLE(of, exynos_smfc_match);

static int exynos_smfc_reboot_notifier(struct notifier_block *nb,
				unsigned long l, void *p)
{
	struct smfc_dev *smfc = container_of(nb, struct smfc_dev, reboot_notifier);

	/* The per-job timer bounds a lost completion IRQ; do not detach the
	 * DMA-core domain or drop a PM reference which this callback never took.
	 */
	v4l2_m2m_suspend(smfc->m2mdev);
	pm_runtime_barrier(smfc->dev);
	return NOTIFY_DONE;
}

static int exynos_smfc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);
	struct smfc_dev *smfc;
	struct resource *res;
	int ret, irq;

	/* No physical-address fallback for system-heap DMA-BUFs. */
	if (!domain || domain->type != IOMMU_DOMAIN_DMA)
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "JPEG requires a translated DMA domain\n");
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	atomic_set(&smfc_hwfc_state, SMFC_HWFC_STANDBY);
	init_waitqueue_head(&smfc_hwfc_sync_wq);
	init_waitqueue_head(&smfc_suspend_wq);
	smfc = devm_kzalloc(dev, sizeof(*smfc), GFP_KERNEL);
	if (!smfc)
		return -ENOMEM;
	smfc->dev = dev;
	smfc->devdata = of_device_get_match_data(dev);
	if (!smfc->devdata)
		return -EINVAL;
	spin_lock_init(&smfc->flag_lock);
	timer_setup(&smfc->timer, smfc_timedout_handler, 0);
	platform_set_drvdata(pdev, smfc);
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	smfc->reg = devm_ioremap_resource(dev, res);
	if (IS_ERR(smfc->reg))
		return PTR_ERR(smfc->reg);
	ret = smfc_init_clock(dev, smfc);
	if (ret)
		return dev_err_probe(dev, ret, "cannot get JPEG clocks\n");
	if (IS_ERR(smfc->clk_gate))
		return dev_err_probe(dev, PTR_ERR(smfc->clk_gate),
				     "JPEG gate clock is required\n");
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	smfc->device_id = of_alias_get_id(dev->of_node, "jpeg");
	pm_runtime_enable(dev);
	ret = smfc_find_hw_version(dev, smfc);
	if (ret < 0)
		goto err_pm;
	ret = devm_request_threaded_irq(dev, irq, NULL,
			exynos_smfc_irq_handler, IRQF_ONESHOT, dev_name(dev), smfc);
	if (ret)
		goto err_pm;
	iovmm_set_fault_handler(dev, smfc_iommu_fault_handler, smfc);
	ret = smfc_init_v4l2(dev, smfc);
	if (ret)
		goto err_fault;
	smfc->reboot_notifier.notifier_call = exynos_smfc_reboot_notifier;
	ret = register_reboot_notifier(&smfc->reboot_notifier);
	if (ret)
		goto err_video;
	dev_info(dev, "Probed H/W Version: %02x.%02x.%04x with DMA API\n",
		 (smfc->hwver >> 24) & 0xff, (smfc->hwver >> 16) & 0xff,
		 smfc->hwver & 0xffff);
	return 0;

err_video:
	smfc_deinit_v4l2(dev, smfc);
err_fault:
	iovmm_set_fault_handler(dev, NULL, NULL);
err_pm:
	pm_runtime_disable(dev);
	return ret;
}

static int exynos_smfc_remove(struct platform_device *pdev)
{
	struct smfc_dev *smfc = platform_get_drvdata(pdev);

	unregister_reboot_notifier(&smfc->reboot_notifier);
	del_timer_sync(&smfc->timer);
	smfc_deinit_v4l2(&pdev->dev, smfc);
	iovmm_set_fault_handler(&pdev->dev, NULL, NULL);
	pm_runtime_disable(&pdev->dev);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int smfc_suspend(struct device *dev)
{
	struct smfc_dev *smfc = dev_get_drvdata(dev);

	v4l2_m2m_suspend(smfc->m2mdev);
	return 0;
}

static int smfc_resume(struct device *dev)
{
	struct smfc_dev *smfc = dev_get_drvdata(dev);

	v4l2_m2m_resume(smfc->m2mdev);
	return 0;
}
#endif

#ifdef CONFIG_PM
static int smfc_runtime_resume(struct device *dev)
{
	/* The genpd and DMA supplier links perform the power transition. */
	return 0;
}

static int smfc_runtime_suspend(struct device *dev)
{
	return 0;
}
#endif

static void exynos_smfc_shutdown(struct platform_device *pdev)
{
	struct smfc_dev *smfc = platform_get_drvdata(pdev);

	dev_info(smfc->dev, "shutdown with state %u\n",
					smfc->flags);
}

static const struct dev_pm_ops exynos_smfc_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(smfc_suspend, smfc_resume)
	SET_RUNTIME_PM_OPS(NULL, smfc_runtime_resume, smfc_runtime_suspend)
};

static struct platform_driver exynos_smfc_driver = {
	.probe		= exynos_smfc_probe,
	.remove		= exynos_smfc_remove,
	.shutdown	= exynos_smfc_shutdown,
	.driver = {
		.name	= MODULE_NAME,
		.owner	= THIS_MODULE,
		.pm	= &exynos_smfc_pm_ops,
		.of_match_table = of_match_ptr(exynos_smfc_match),
		.suppress_bind_attrs = true,
	}
};

module_platform_driver(exynos_smfc_driver);

MODULE_AUTHOR("Cho KyongHo <pullip.cho@samsung.com>");
MODULE_DESCRIPTION("Exynos Still MFC(JPEG) V4L2 Driver");
MODULE_LICENSE("GPL");
