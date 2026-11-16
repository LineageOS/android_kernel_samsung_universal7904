/*
 * drivers/media/m2m1shot2.c
 *
 * Copyright (C) 2015 Samsung Electronics Co., Ltd.
 *
 * Contact: Cho KyongHo <pullip.cho@samsung.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/vmalloc.h>
#include <linux/sched.h>
#include <linux/compat.h>

#include <linux/dma-fence.h>
#include <linux/dma-mapping.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <linux/sync_file.h>
#include <linux/timer.h>

#include <media/m2m1shot2.h>


#define M2M1SHOT2_NEED_CACHEFLUSH_ALL (10 * SZ_1M)
#define M2M1SHOT2_FENCE_MASK (M2M1SHOT2_IMGFLAG_ACQUIRE_FENCE |		\
					M2M1SHOT2_IMGFLAG_RELEASE_FENCE)

static void m2m1shot2_fence_callback(struct dma_fence *fence,
				     struct dma_fence_cb *waiter);

static void m2m1shot2_timeout_handler(struct timer_list *t);

/*
 * Release fences (4.4: sw_sync timeline per context).  One fence per job,
 * signaled when the job finishes or the context is cancelled.
 */
static const char *m2m1shot2_fence_driver_name(struct dma_fence *f)
{
	return "m2m1shot2";
}

static const char *m2m1shot2_fence_timeline_name(struct dma_fence *f)
{
	return "m2m1shot2-ctx";
}

static const struct dma_fence_ops m2m1shot2_fence_ops = {
	.get_driver_name = m2m1shot2_fence_driver_name,
	.get_timeline_name = m2m1shot2_fence_timeline_name,
};

/*
 * A released fence can outlive its context (the sync_file fd stays open
 * after the context is destroyed), so the fence must own the lock it is
 * initialized with: the default release frees the whole object (base is the
 * first member), and the context lock is only used for the context's own
 * pending_release bookkeeping.
 */
struct m2m1shot2_fence {
	struct dma_fence base;
	spinlock_t lock;
};

/*
 * 4.4 ion_iovmm_map()/exynos_iovmm_map_userptr() returned one IOVA range.
 * With the IOMMU DMA ops the mapping is IOVA-contiguous but may still be
 * reported as several DMA segments (max_segment_size); accept that.
 */
/*
 * The 5.10 ION exporter returns its table without updating ->nents to the
 * count dma_map_sg() merged, so trailing entries carry dma_len 0.  Like
 * m2m1shot_sgt_iova(): sum the IOVA-contiguous run from the first entry.
 */
static size_t m2m1shot2_sgt_contig_len(struct sg_table *sgt)
{
	struct scatterlist *sg;
	dma_addr_t next = 0;
	size_t len = 0;
	unsigned int i;

	for_each_sgtable_dma_sg(sgt, sg, i) {
		if (!sg_dma_len(sg) || (i && sg_dma_address(sg) != next))
			break;
		len += sg_dma_len(sg);
		next = sg_dma_address(sg) + sg_dma_len(sg);
	}
	return len;
}

static void m2m1shot2_signal_release(struct m2m1shot2_context *ctx)
{
	struct dma_fence *f;
	unsigned long flags;

	spin_lock_irqsave(&ctx->fence_lock, flags);
	f = ctx->pending_release;
	ctx->pending_release = NULL;
	spin_unlock_irqrestore(&ctx->fence_lock, flags);
	if (f) {
		dma_fence_signal(f);
		dma_fence_put(f);
	}
}

/*
 * STATE TRANSITION of m2m1shot2_context:
 * - PROCESSING:
 *     # set in m2m1shot2_start_processing() after the context is moved to
 *       active_contexts.
 *     # clear in __m2m1shot2_finish_context() inside m2m1shot2_finish_context()
 *       that is called by the client driver when IRQ occurs.
 * - PENDING:
 *     # set in m2m1shot2_start_context() along with PROCESSING.
 *     # clear in m2m1shot2_schedule() before calling .device_run().
 * - WAITING:
 *     # set at the entry of m2m1shot2_wait_process()
 *     # clear in m2m1shot2_ioctl() before returning to user
 * - PROCESSED:
 *     # set in __m2m1shot2_finish_context() along with clearing PROCESSING.
 *     # clear in m2m1shot2_ioctl() before returning to user
 * - ERROR:
 *     # set in __m2m1shot2_finish_context() on an error
 *     # clear in m2m1shot2_ioctl() before returning to user
 */
static int m2m1shot2_open(struct inode *inode, struct file *filp)
{
	struct m2m1shot2_device *m21dev = container_of(filp->private_data,
						struct m2m1shot2_device, misc);
	struct m2m1shot2_context *ctx;
	unsigned long flags;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->fence_context = dma_fence_context_alloc(1);
	spin_lock_init(&ctx->fence_lock);

	INIT_LIST_HEAD(&ctx->node);

	spin_lock_irqsave(&m21dev->lock_ctx, flags);
	list_add_tail(&ctx->node, &m21dev->contexts);
	spin_unlock_irqrestore(&m21dev->lock_ctx, flags);

	ctx->m21dev = m21dev;
	mutex_init(&ctx->mutex);
	init_completion(&ctx->complete);
	complete_all(&ctx->complete); /* prevent to wait for completion */

	filp->private_data = ctx;

	for (ret = 0; ret < M2M1SHOT2_MAX_IMAGES; ret++) {
		ctx->source[ret].img.index = ret;
	}
	ctx->target.index = M2M1SHOT2_MAX_IMAGES;

	ret = m21dev->ops->init_context(ctx);
	if (ret)
		goto err_init;

	spin_lock(&m21dev->lock_priority);
	ctx->priority = M2M1SHOT2_DEFAULT_PRIORITY;
	m21dev->prior_stats[ctx->priority] += 1;
	spin_unlock(&m21dev->lock_priority);

	spin_lock_init(&ctx->fence_timeout_lock);

	timer_setup(&ctx->timer, m2m1shot2_timeout_handler, 0);

	return 0;
err_init:
	spin_lock_irqsave(&m21dev->lock_ctx, flags);
	list_del(&ctx->node);
	spin_unlock_irqrestore(&m21dev->lock_ctx, flags);
	kfree(ctx);
	return ret;

}

/*
 * m2m1shot2_current_context - get the current m2m1shot2_context
 *
 * return the current context pointer.
 * This function should not be called other places than the function that
 * finishes the current context.
 */
struct m2m1shot2_context *m2m1shot2_current_context(
				const struct m2m1shot2_device *m21dev)
{
	return m21dev->current_ctx;
}

static void m2m1shot2_put_userptr(struct device *dev,
				 struct m2m1shot2_dma_buffer *plane)
{
	if (plane->userptr.mapped)
		dma_unmap_sgtable(dev, &plane->userptr.table,
				  DMA_BIDIRECTIONAL, 0);
	if (plane->userptr.table.sgl)
		sg_free_table(&plane->userptr.table);
	if (plane->userptr.pages) {
		unpin_user_pages_dirty_lock(plane->userptr.pages,
					    plane->userptr.nr_pages,
					    plane->userptr.writable);
		kvfree(plane->userptr.pages);
	}
	plane->userptr.mapped = false;
	plane->userptr.pages = NULL;
	plane->userptr.nr_pages = 0;
	plane->userptr.addr = 0;
	plane->dma_addr = 0;
}

static void m2m1shot2_put_dmabuf(struct m2m1shot2_dma_buffer *plane)
{
	if (plane->sgt)
		dma_buf_unmap_attachment(plane->dmabuf.attachment, plane->sgt,
					 DMA_BIDIRECTIONAL);
	plane->sgt = NULL;
	dma_buf_detach(plane->dmabuf.dmabuf, plane->dmabuf.attachment);
	dma_buf_put(plane->dmabuf.dmabuf);
	plane->dmabuf.dmabuf = NULL;
	plane->dmabuf.attachment = NULL;
	plane->dma_addr = 0;
}

static void m2m1shot2_put_buffer(struct m2m1shot2_context *ctx, u32 memory,
				 struct m2m1shot2_dma_buffer plane[],
				 unsigned int num_planes)
{
	unsigned int i;

	if (memory == M2M1SHOT2_BUFTYPE_DMABUF) {
		for (i = 0; i < num_planes; i++)
			m2m1shot2_put_dmabuf(&plane[i]);
	} else if (memory == M2M1SHOT2_BUFTYPE_USERPTR) {
		for (i = 0; i < num_planes; i++)
			m2m1shot2_put_userptr(ctx->m21dev->dev, &plane[i]);
	}
}

static void m2m1shot2_put_image(struct m2m1shot2_context *ctx,
				struct m2m1shot2_context_image *img)
{
	if (!!(img->flags & M2M1SHOT2_IMGFLAG_ACQUIRE_FENCE)) {
		/*
		 * confirm the fence callback is not called after img->fence is
		 * cleared.
		 */
		dma_fence_remove_callback(img->fence, &img->waiter);
		dma_fence_put(img->fence);
		img->fence = NULL;
		img->flags &= ~M2M1SHOT2_IMGFLAG_ACQUIRE_FENCE;
	}

	m2m1shot2_put_buffer(ctx, img->memory, img->plane, img->num_planes);

	img->memory = M2M1SHOT2_BUFTYPE_NONE;
}

static void m2m1shot2_put_source_images(struct m2m1shot2_context *ctx)
{
	unsigned int i;

	for (i = 0; i < ctx->num_sources; i++)
		m2m1shot2_put_image(ctx, &ctx->source[i].img);

	ctx->num_sources = 0;
}

static void m2m1shot2_put_images(struct m2m1shot2_context *ctx)
{
	m2m1shot2_put_source_images(ctx);
	m2m1shot2_put_image(ctx, &ctx->target);
}

static void __m2m1shot2_finish_context(struct m2m1shot2_context *ctx,
					bool success)
{
	struct m2m1shot2_device *m21dev = ctx->m21dev;
	unsigned long flags;

	m2m1shot2_signal_release(ctx);

	spin_lock_irqsave(&m21dev->lock_ctx, flags);

	/* effective only to the current processing context */
	if (WARN_ON(ctx != m21dev->current_ctx)) {
		spin_unlock_irqrestore(&m21dev->lock_ctx, flags);
		return;
	}

	m21dev->current_ctx = NULL;
	list_add_tail(&ctx->node, &m21dev->contexts);

	spin_unlock_irqrestore(&m21dev->lock_ctx, flags);

	if (!success)
		set_bit(M2M1S2_CTXSTATE_ERROR, &ctx->state);

	set_bit(M2M1S2_CTXSTATE_PROCESSED, &ctx->state);
	clear_bit(M2M1S2_CTXSTATE_PROCESSING, &ctx->state);

	complete_all(&ctx->complete);
}

/**
 * __m2m1shot2_schedule() - pick a context to process
 *
 * pick a context that is at the front of m2m1shot2_device.active_contexts
 * and provide it to the client to process that context.
 * If the client returns error then pick the next context.
 */
static void __m2m1shot2_schedule(struct m2m1shot2_device *m21dev)
{
	struct m2m1shot2_context *ctx;
	unsigned long flags;

retry:
	spin_lock_irqsave(&m21dev->lock_ctx, flags);

	if (m21dev->current_ctx != NULL) {
		/* the client is currently processing a context */
		spin_unlock_irqrestore(&m21dev->lock_ctx, flags);
		return;
	}

	if (list_empty(&m21dev->active_contexts)) {
		/* no context to process */
		spin_unlock_irqrestore(&m21dev->lock_ctx, flags);
		return;
	}

	ctx = list_first_entry(&m21dev->active_contexts,
				struct m2m1shot2_context, node);
	m21dev->current_ctx = ctx;
	list_del_init(&ctx->node);

	spin_unlock_irqrestore(&m21dev->lock_ctx, flags);

	clear_bit(M2M1S2_CTXSTATE_PENDING, &ctx->state);

	if (m21dev->ops->device_run(ctx) < 0) {
		__m2m1shot2_finish_context(ctx, false);
		goto retry;
	}
}


/* After H/W: make device writes visible to the CPU (4.4 sync_for_cpu). */
static void m2m1shot2_unmap_image(struct m2m1shot2_device *m21dev,
				struct m2m1shot2_context_image *img,
				enum dma_data_direction dir,
				bool skip_inv)
{
	bool inv = !skip_inv && !(img->flags & M2M1SHOT2_IMGFLAG_NO_CACHEINV) &&
			!(m21dev->attr & M2M1SHOT2_DEVATTR_COHERENT);
	unsigned int i;

	if (!inv || dir == DMA_TO_DEVICE)
		return;
	for (i = 0; i < img->num_planes; i++) {
		struct m2m1shot2_dma_buffer *pl = &img->plane[i];

		if (img->memory == M2M1SHOT2_BUFTYPE_DMABUF && pl->sgt)
			dma_sync_sgtable_for_cpu(m21dev->dev, pl->sgt, dir);
		else if (img->memory == M2M1SHOT2_BUFTYPE_USERPTR &&
			 pl->userptr.mapped)
			dma_sync_sgtable_for_cpu(m21dev->dev,
						 &pl->userptr.table, dir);
	}
}

static void m2m1shot2_unmap_images(struct m2m1shot2_context *ctx)
{
	unsigned int i;

	for (i = 0; i < ctx->num_sources; i++)
		m2m1shot2_unmap_image(ctx->m21dev,
				&ctx->source[i].img, DMA_TO_DEVICE, false);

	m2m1shot2_unmap_image(ctx->m21dev,
				&ctx->target, DMA_FROM_DEVICE, false);
}

/* Buffers are mapped once in m2m1shot2_get_{dmabuf,userptr}(). */
static int m2m1shot2_map_image(struct m2m1shot2_device *m21dev,
				struct m2m1shot2_context_image *img,
				enum dma_data_direction dir)
{
	return 0;
}



static void m2m1shot2_cachesync_image(struct m2m1shot2_context *ctx,
				      struct m2m1shot2_context_image *img,
				      enum dma_data_direction dir)
{
	struct device *dev = ctx->m21dev->dev;
	int i;

	if (!!(img->flags & M2M1SHOT2_IMGFLAG_NO_CACHECLEAN))
		return;

	for (i = 0; i < img->num_planes; i++) {
		struct m2m1shot2_dma_buffer *pl = &img->plane[i];

		if (img->memory == M2M1SHOT2_BUFTYPE_DMABUF && pl->sgt)
			dma_sync_sgtable_for_device(dev, pl->sgt, dir);
		else if (img->memory == M2M1SHOT2_BUFTYPE_USERPTR &&
			 pl->userptr.mapped)
			dma_sync_sgtable_for_device(dev, &pl->userptr.table,
						    dir);
	}
}

static void m2m1shot2_cachesync_images(struct m2m1shot2_context *ctx)
{
	int i;

	if (!!(ctx->m21dev->attr & M2M1SHOT2_DEVATTR_COHERENT) &&
			!test_bit(M2M1S2_CTXSTATE_CACHEFLUSH, &ctx->state))
		return;

	/* 5.10 has no flush_all_cpu_caches(): always sync per buffer. */
	for (i = 0; i < ctx->num_sources; i++)
		m2m1shot2_cachesync_image(ctx,
					  &ctx->source[i].img, DMA_TO_DEVICE);

	m2m1shot2_cachesync_image(ctx, &ctx->target, DMA_FROM_DEVICE);
}

static int m2m1shot2_map_images(struct m2m1shot2_context *ctx)
{
	unsigned int i;
	int ret;

	for (i = 0; i < ctx->num_sources; i++) {
		ret = m2m1shot2_map_image(ctx->m21dev,
					  &ctx->source[i].img, DMA_TO_DEVICE);
		if (ret) {
			while (i-- > 0)
				m2m1shot2_unmap_image(ctx->m21dev,
					&ctx->source[i].img, DMA_TO_DEVICE,
					true);
			return ret;
		}
	}

	ret = m2m1shot2_map_image(ctx->m21dev, &ctx->target, DMA_FROM_DEVICE);
	if (ret) {
		for (i = 0; i < ctx->num_sources; i++)
			m2m1shot2_unmap_image(ctx->m21dev,
				&ctx->source[i].img, DMA_TO_DEVICE, true);
	}

	return ret;
}

/*
 * m2m1shot2_wait_process() - wait until processing the context is finished
 *
 * This function should be called under ctx->mutex held.
 */
static bool m2m1shot2_wait_process(struct m2m1shot2_device *m21dev,
				   struct m2m1shot2_context *ctx)
{
	bool success = true;

	set_bit(M2M1S2_CTXSTATE_WAITING, &ctx->state);

	wait_for_completion(&ctx->complete);

	clear_bit(M2M1S2_CTXSTATE_WAITING, &ctx->state);

	if ((ctx->state & ((1 << M2M1S2_CTXSTATE_PENDING) |
				 (1 << M2M1S2_CTXSTATE_PROCESSING)))) {
		pr_err("m2m1shot finish state : %lx\n", ctx->state);
		BUG();
	}

	clear_bit(M2M1S2_CTXSTATE_PROCESSED, &ctx->state);

	if (test_bit(M2M1S2_CTXSTATE_ERROR, &ctx->state)) {
		success = false;
		clear_bit(M2M1S2_CTXSTATE_ERROR, &ctx->state);
	}

	m2m1shot2_unmap_images(ctx);

	return success;
}

static void m2m1shot2_schedule_context(struct m2m1shot2_context *ctx)
{
	struct m2m1shot2_device *m21dev = ctx->m21dev;
	unsigned long flags;

	del_timer(&ctx->timer);

	m2m1shot2_cachesync_images(ctx);

	spin_lock_irqsave(&ctx->m21dev->lock_ctx, flags);

	BUG_ON(list_empty(&ctx->node));

	/* move ctx from m21dev->contexts to m21dev->active_contexts */
	list_move_tail(&ctx->node, &ctx->m21dev->active_contexts);

	set_bit(M2M1S2_CTXSTATE_PROCESSING, &ctx->state);

	spin_unlock_irqrestore(&ctx->m21dev->lock_ctx, flags);

	__m2m1shot2_schedule(m21dev);
}

static void m2m1shot2_context_schedule_work(struct work_struct *work)
{
	m2m1shot2_schedule_context(
			container_of(work, struct m2m1shot2_context, work));
}

static void m2m1shot2_context_schedule_start(struct kref *kref)
{
	m2m1shot2_schedule_context(
			container_of(kref, struct m2m1shot2_context, starter));
}

static void m2m1shot2_schedule_queuework(struct kref *kref)
{
	struct m2m1shot2_context *ctx =
			container_of(kref, struct m2m1shot2_context, starter);
	bool failed;

	failed = !queue_work(ctx->m21dev->schedule_workqueue, &ctx->work);

	BUG_ON(failed);
}

/* NOTE that this function is called under irq disabled context */
/* NOTE that this function is called under irq disabled context */
static void m2m1shot2_fence_callback(struct dma_fence *fence,
				     struct dma_fence_cb *waiter)
{
	struct m2m1shot2_context_image *img =
		container_of(waiter, struct m2m1shot2_context_image, waiter);
	unsigned long ptr = (unsigned long)img;
	struct m2m1shot2_context *ctx;
	unsigned long flags;

	BUG_ON(img->index > M2M1SHOT2_MAX_IMAGES);

	ptr -= sizeof(struct m2m1shot2_source_image) * img->index;
	ptr -= offsetof(struct m2m1shot2_context, source);
	ctx = (struct m2m1shot2_context *)ptr;

	spin_lock_irqsave(&ctx->fence_timeout_lock, flags);

	if (!(img->flags & M2M1SHOT2_IMGFLAG_ACQUIRE_FENCE)) {
		spin_unlock_irqrestore(&ctx->fence_timeout_lock, flags);
		return;
	}

	img->flags &= ~M2M1SHOT2_IMGFLAG_ACQUIRE_FENCE;
	dma_fence_put(img->fence);
	img->fence = NULL;

	kref_put(&ctx->starter, m2m1shot2_schedule_queuework);

	spin_unlock_irqrestore(&ctx->fence_timeout_lock, flags);
}

static void m2m1shot2_timeout_handler(struct timer_list *t)
{
	struct m2m1shot2_context *ctx = from_timer(ctx, t, timer);
	struct m2m1shot2_source_image *image = ctx->source;
	struct m2m1shot2_context_image *timeout_image[M2M1SHOT2_MAX_IMAGES + 1];
	unsigned long flags;
	int i, j = 0;

	pr_err("%s: %d Fence(s) timed out after %d msec.\n", __func__,
		kref_read(&ctx->starter), M2M1S2_TIMEOUT_INTERVAL);

	spin_lock_irqsave(&ctx->fence_timeout_lock, flags);

	if (kref_read(&ctx->starter) == 0) {
		spin_unlock_irqrestore(&ctx->fence_timeout_lock, flags);
		pr_err("All fences have been signaled. (work_busy? %d)\n",
			work_busy(&ctx->work));
		return;
	}

	for (i = 0; i < ctx->num_sources; i++) {
		if (image[i].img.flags & M2M1SHOT2_IMGFLAG_ACQUIRE_FENCE) {
			image[i].img.flags &= ~M2M1SHOT2_IMGFLAG_ACQUIRE_FENCE;
			timeout_image[j++] = &image[i].img;
		}
	}

	if (!!(ctx->target.flags & M2M1SHOT2_IMGFLAG_ACQUIRE_FENCE)) {
		ctx->target.flags &= ~M2M1SHOT2_IMGFLAG_ACQUIRE_FENCE;
		timeout_image[j++] = &ctx->target;
	}

	/* Increase reference to prevent running the workqueue in callback */
	kref_get(&ctx->starter);

	spin_unlock_irqrestore(&ctx->fence_timeout_lock, flags);

	while (j-- > 0) {
		dma_fence_remove_callback(timeout_image[j]->fence,
					  &timeout_image[j]->waiter);
		dma_fence_put(timeout_image[j]->fence);
		timeout_image[j]->fence = NULL;
	}

	m2m1shot2_schedule_queuework(&ctx->starter);
};

/**
 * m2m1shot2_start_context() - add a context to the queue
 *
 * Given a valid context ready for processing, add it to
 * m2m1shot2_device.active_list that is the queue of ready contexts.
 * The state of the given context becomes PROCESSING|PENDING.
 * Then pick the next context to process from the queue. If there was no context
 * in the queue before this function call, the given context will be pick for
 * the context to process.
 */
static void m2m1shot2_start_context(struct m2m1shot2_device *m21dev,
				    struct m2m1shot2_context *ctx)
{
	int refcount;

	m2m1shot2_map_images(ctx);

	INIT_WORK(&ctx->work, m2m1shot2_context_schedule_work);

	reinit_completion(&ctx->complete);

	clear_bit(M2M1S2_CTXSTATE_PROCESSED, &ctx->state);
	set_bit(M2M1S2_CTXSTATE_PENDING, &ctx->state);
	/*
	 * if there is no fence to wait for, workqueue is not used to push a
	 * task
	 */
	refcount = kref_read(&ctx->starter);
	if (refcount > 1)
		mod_timer(&ctx->timer,
			jiffies + msecs_to_jiffies(M2M1S2_TIMEOUT_INTERVAL));

	kref_put(&ctx->starter, m2m1shot2_schedule_queuework);
}

void m2m1shot2_finish_context(struct m2m1shot2_context *ctx, bool success)
{
	__m2m1shot2_finish_context(ctx, success);
}

void m2m1shot2_schedule(struct m2m1shot2_device *m21dev)
{
	__m2m1shot2_schedule(m21dev);
}

static void m2m1shot2_cancel_context(struct m2m1shot2_context *ctx)
{
	unsigned long flags;

	/*
	 * wait until the H/W finishes the processing about the context if
	 * the context is being serviced by H/W. This should be the only waiter
	 * for the context. If nothing is to be waited for,
	 * m2m1shot2_wait_process() returns immediately.
	 */
	m2m1shot2_wait_process(ctx->m21dev, ctx);

	/*
	 * this context is added to m2m1shot2_device.contexts by
	 * m2m1shot2_finish_context() called by the client device driver
	 */
	spin_lock_irqsave(&ctx->m21dev->lock_ctx, flags);
	BUG_ON(list_empty(&ctx->node));
	list_del_init(&ctx->node);
	spin_unlock_irqrestore(&ctx->m21dev->lock_ctx, flags);

	/* signal all possible release fences */
	m2m1shot2_signal_release(ctx);

	/* to confirm if no reference to a resource exists */
	if (!!(ctx->flags & M2M1SHOT2_FLAG_NONBLOCK))
		m2m1shot2_put_images(ctx);

	WARN(!M2M1S2_CTXSTATE_IDLE(ctx),
		"state should be IDLE but %#lx\n", ctx->state);
}

static void m2m1shot2_destroy_context(struct work_struct *work)
{
	struct m2m1shot2_context *ctx =
		container_of(work, struct m2m1shot2_context, dwork);

	mutex_lock(&ctx->mutex);

	m2m1shot2_cancel_context(ctx);

	spin_lock(&ctx->m21dev->lock_priority);
	ctx->m21dev->prior_stats[ctx->priority] -= 1;
	spin_unlock(&ctx->m21dev->lock_priority);

	if (ctx->m21dev->ops->prepare_perf)
		ctx->m21dev->ops->prepare_perf(ctx, NULL);

	ctx->m21dev->ops->free_context(ctx);

	mutex_unlock(&ctx->mutex);

	kfree(ctx);
}

/*
 * m2m1shot2_release() may be called during the context to be destroyed is ready
 * for processing or currently waiting for completion of processing due to the
 * non-blocking interface. Therefore it should remove the context from
 * m2m1shot2_device.active_contexts if the context is in the list. If it is
 * currently being processed by H/W, this function should wait until the H/W
 * finishes.
 */
static int m2m1shot2_release(struct inode *inode, struct file *filp)
{
	struct m2m1shot2_context *ctx = filp->private_data;

	INIT_WORK(&ctx->dwork, m2m1shot2_destroy_context);
	/* always success */
	queue_work(ctx->m21dev->destroy_workqueue, &ctx->dwork);

	return 0;
}

/*
 * 4.4 exynos_iovmm_map_userptr() mapped the user range into the SysMMU.
 * 5.10: a USERPTR inside a DMA-BUF mmap is not handled here (callers use
 * dmabuf fds); plain memory is pinned and mapped with dma_map_sgtable(),
 * which allocates one contiguous IOVA range behind the IOMMU DMA ops.
 */
static int m2m1shot2_get_userptr(struct m2m1shot2_device *m21dev,
				 struct m2m1shot2_dma_buffer *plane,
				 unsigned long addr, u32 length,
				 enum dma_data_direction dir)
{
	unsigned long first = addr & PAGE_MASK;
	unsigned long last = (addr + length - 1) & PAGE_MASK;
	unsigned int nr = ((last - first) >> PAGE_SHIFT) + 1;
	unsigned int gup = FOLL_LONGTERM;
	int pinned, ret;

	if (plane->userptr.pages)
		m2m1shot2_put_userptr(m21dev->dev, plane);
	if (!length)
		return -EINVAL;

	plane->userptr.pages = kvcalloc(nr, sizeof(struct page *), GFP_KERNEL);
	if (!plane->userptr.pages)
		return -ENOMEM;
	plane->userptr.writable = dir != DMA_TO_DEVICE;
	if (plane->userptr.writable)
		gup |= FOLL_WRITE;
	pinned = pin_user_pages_fast(first, nr, gup, plane->userptr.pages);
	if (pinned != nr) {
		dev_err(m21dev->dev, "%s: pinned %d/%u pages at %#lx\n",
			__func__, pinned, nr, addr);
		if (pinned > 0)
			unpin_user_pages(plane->userptr.pages, pinned);
		kvfree(plane->userptr.pages);
		plane->userptr.pages = NULL;
		return pinned < 0 ? pinned : -EFAULT;
	}
	plane->userptr.nr_pages = nr;

	ret = sg_alloc_table_from_pages(&plane->userptr.table,
					plane->userptr.pages, nr,
					offset_in_page(addr), length, GFP_KERNEL);
	if (ret)
		goto err;
	ret = dma_map_sgtable(m21dev->dev, &plane->userptr.table,
			      DMA_BIDIRECTIONAL, 0);
	if (ret)
		goto err;
	/* the SG table already starts at the in-page offset */
	if (m2m1shot2_sgt_contig_len(&plane->userptr.table) < length) {
		dev_err(m21dev->dev, "%s: %u DMA segments, not IOVA-contiguous\n",
			__func__, plane->userptr.table.nents);
		dma_unmap_sgtable(m21dev->dev, &plane->userptr.table,
				  DMA_BIDIRECTIONAL, 0);
		ret = -EINVAL;
		goto err;
	}
	plane->userptr.mapped = true;
	plane->dma_addr = sg_dma_address(plane->userptr.table.sgl);
	plane->userptr.addr = addr;
	plane->userptr.length = length;
	return 0;
err:
	m2m1shot2_put_userptr(m21dev->dev, plane);
	return ret;
}

static int m2m1shot2_get_dmabuf(struct m2m1shot2_device *m21dev,
				struct m2m1shot2_dma_buffer *plane,
				int fd, u32 off, size_t payload,
				enum dma_data_direction dir)
{
	struct dma_buf *dmabuf = dma_buf_get(fd);
	struct sg_table *sgt;
	int ret = -EINVAL;

	if (IS_ERR(dmabuf)) {
		dev_err(m21dev->dev, "%s: failed to get dmabuf from fd %d\n",
			__func__, fd);
		return PTR_ERR(dmabuf);
	}

	if (dmabuf->size < off) {
		dev_err(m21dev->dev,
			"%s: too large offset %u for dmabuf of %zu\n",
			__func__, off, dmabuf->size);
		goto err;
	}

	if ((dmabuf->size - off) < payload) {
		dev_err(m21dev->dev,
			"%s: too small dmabuf %zu/%u but reqiured %zu\n",
			__func__, dmabuf->size, off, payload);
		goto err;
	}

	if ((dmabuf == plane->dmabuf.dmabuf) &&
			(dmabuf->size == plane->dmabuf.dmabuf->size) &&
			(dmabuf->file == plane->dmabuf.dmabuf->file) &&
			plane->sgt) {
		/* same buffer: keep the existing attachment and IOVA */
		plane->dma_addr = sg_dma_address(plane->sgt->sgl) + off;
		plane->dmabuf.offset = off;
		dma_buf_put(dmabuf);
		return 0;
	}

	/* release the previous buffer */
	if (plane->dmabuf.dmabuf != NULL)
		m2m1shot2_put_dmabuf(plane);

	plane->dmabuf.attachment = dma_buf_attach(dmabuf, m21dev->dev);
	if (IS_ERR(plane->dmabuf.attachment)) {
		dev_err(m21dev->dev,
			"%s: failed to attach to dmabuf\n", __func__);
		ret = PTR_ERR(plane->dmabuf.attachment);
		goto err;
	}

	/* 4.4 ion_iovmm_map(): one IOVA range for the whole buffer. */
	sgt = dma_buf_map_attachment(plane->dmabuf.attachment,
				     DMA_BIDIRECTIONAL);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		goto err_map;
	}
	if (m2m1shot2_sgt_contig_len(sgt) < off + payload) {
		struct scatterlist *sg;
		unsigned int k;

		dev_err(m21dev->dev, "%s: %u DMA segments, not IOVA-contiguous (exp %s size %zu)\n",
			__func__, sgt->nents, dmabuf->exp_name, dmabuf->size);
		for_each_sgtable_dma_sg(sgt, sg, k) {
			if (k >= 4)
				break;
			dev_err(m21dev->dev, "M20G2D seg%u dma %pad len %u phys %pa\n",
				k, &sg_dma_address(sg), sg_dma_len(sg),
				&(phys_addr_t){ sg_phys(sg) });
		}
		dma_buf_unmap_attachment(plane->dmabuf.attachment, sgt,
					 DMA_BIDIRECTIONAL);
		ret = -EINVAL;
		goto err_map;
	}

	plane->sgt = sgt;
	plane->dmabuf.dmabuf = dmabuf;
	plane->dmabuf.offset = off;
	plane->dma_addr = sg_dma_address(sgt->sgl) + off;

	return 0;
err_map:
	dma_buf_detach(dmabuf, plane->dmabuf.attachment);
	plane->dma_addr = 0;
	plane->dmabuf.attachment = NULL;
err:
	dma_buf_put(dmabuf);
	return ret;
}

static int m2m1shot2_get_buffer(struct m2m1shot2_context *ctx,
				struct m2m1shot2_context_image *img,
				struct m2m1shot2_image *src,
				size_t payload[],
				enum dma_data_direction dir)
{
	unsigned int num_planes = src->num_planes;
	int ret = 0;
	unsigned int i;

	if (src->memory != img->memory)
		m2m1shot2_put_buffer(ctx, img->memory,
					img->plane, img->num_planes);

	img->memory = M2M1SHOT2_BUFTYPE_NONE;

	if (src->memory == M2M1SHOT2_BUFTYPE_DMABUF) {
		for (i = 0; i < src->num_planes; i++) {
			ret = m2m1shot2_get_dmabuf(ctx->m21dev,
					&img->plane[i], src->plane[i].fd,
					src->plane[i].offset, payload[i], dir);
			if (ret) {
				while (i-- > 0)
					m2m1shot2_put_dmabuf(&img->plane[i]);
				return ret;
			}

			img->plane[i].payload = payload[i];

			/* 4.4 ION cached-buffer accounting: assume cached. */
			if (!(img->flags & M2M1SHOT2_IMGFLAG_NO_CACHECLEAN))
				ctx->ctx_private += img->plane[i].payload;
		}
	} else if (src->memory == M2M1SHOT2_BUFTYPE_USERPTR) {
		for (i = 0; i < src->num_planes; i++) {
			if (src->plane[i].offset != 0) {
				dev_err(ctx->m21dev->dev,
					"%s: offset should be 0 with userptr\n",
					__func__);
				ret = -EINVAL;
			} else {
				ret = m2m1shot2_get_userptr(ctx->m21dev,
					&img->plane[i], src->plane[i].userptr,
					src->plane[i].length, dir);
			}

			if (ret) {
				while (i-- > 0)
					m2m1shot2_put_userptr(ctx->m21dev->dev,
								&img->plane[i]);
				return ret;
			}

			img->plane[i].payload = payload[i];

			if (!(img->flags & M2M1SHOT2_IMGFLAG_NO_CACHECLEAN))
				ctx->ctx_private += img->plane[i].payload;
		}
	} else {
		dev_err(ctx->m21dev->dev,
			"%s: invalid memory type %d\n", __func__, src->memory);
		return -EINVAL;
	}

	img->num_planes = num_planes;
	img->memory = src->memory;

	return 0;
}

/*
 * check fence_count fences configured in the previous images in the same
 * context. If it does not find the same fence with the given, register the
 * fence waiter.
 * If registering the fence waiter for an unique fence fails, it releases it
 * immediately. The caller should handle the case.
 *
 */
static bool m2m1shot2_check_fence_wait(struct m2m1shot2_context *ctx,
					unsigned int fence_count,
					struct dma_fence *fence,
					struct dma_fence_cb *waiter)
{
	unsigned int i;
	int ret;

	for (i = 0; i < fence_count; i++) {
		if (ctx->source[i].img.fence == fence)
			return false;
	}

	/*
	 * Always increase ctx->starter before adding the callback.  If the
	 * fence is already signaled (-ENOENT) or failed, just drop it.
	 */
	kref_get(&ctx->starter);
	ret = dma_fence_add_callback(fence, waiter, m2m1shot2_fence_callback);
	if (ret) {
		if (ret != -ENOENT)
			dev_err(ctx->m21dev->dev,
				"Error occurred in an acquire fence\n");
		kref_put(&ctx->starter, m2m1shot2_context_schedule_start);
		dma_fence_put(fence);
		return false;
	}

	return true;
}

static int m2m1shot2_get_source(struct m2m1shot2_context *ctx,
				unsigned int index,
				struct m2m1shot2_source_image *img,
				struct m2m1shot2_image *src)
{
	struct device *dev = ctx->m21dev->dev;
	size_t payload[M2M1SHOT2_MAX_PLANES];
	unsigned int i, num_planes;
	int ret;

	if (!M2M1SHOT2_BUFTYPE_VALID(src->memory)) {
		dev_err(dev,
			"%s: invalid memory type %u specified for image %u\n",
			__func__, src->memory, index);
		return -EINVAL;
	}

	img->img.fmt.fmt = src->fmt;
	img->img.flags = src->flags;
	img->ext = src->ext;
	img->img.fmt.colorspace = src->colorspace;

	ret = ctx->m21dev->ops->prepare_format(&img->img.fmt, index,
				DMA_TO_DEVICE, payload, &num_planes);
	if (ret) {
		dev_err(dev, "%s: invalid format specified for image %u\n",
			__func__, index);
		return ret;
	}

	if (src->memory == M2M1SHOT2_BUFTYPE_EMPTY) {
		m2m1shot2_put_image(ctx, &img->img);
		img->img.memory = src->memory;
		img->img.num_planes = 0;
		/* no buffer, no fence */
		img->img.flags &= ~(M2M1SHOT2_IMGFLAG_ACQUIRE_FENCE |
					M2M1SHOT2_IMGFLAG_RELEASE_FENCE);
		return 0;
	}

	BUG_ON((num_planes < 1) || (num_planes > M2M1SHOT2_MAX_PLANES));

	if (num_planes != src->num_planes) {
		dev_err(dev, "%s: wrong number of planes %u of image %u.\n",
			__func__, src->num_planes, index);
		return -EINVAL;
	}

	for (i = 0; i < num_planes; i++) {
		if (src->plane[i].length < payload[i]) {
			dev_err(dev,
				"%s: too small size %u (plane %u / image %u)\n",
				__func__, src->plane[i].length, i, index);
			return -EINVAL;
		}
	}

	return m2m1shot2_get_buffer(ctx, &img->img,
				    src, payload, DMA_TO_DEVICE);
}

static int m2m1shot2_get_sources(struct m2m1shot2_context *ctx,
				 struct m2m1shot2_image __user *usersrc,
				 bool blocking)
{
	struct device *dev = ctx->m21dev->dev;
	struct m2m1shot2_source_image *image = ctx->source;
	unsigned int i;
	int ret;

	for (i = 0; i < ctx->num_sources; i++, usersrc++) {
		struct m2m1shot2_image source;

		if (copy_from_user(&source, usersrc, sizeof(source))) {
			dev_err(dev,
				"%s: Failed to read source[%u] image data\n",
				__func__, i);
			ret = -EFAULT;
			goto err;
		}

		/* blocking mode does not allow fences */
		if (blocking && !!(source.flags & M2M1SHOT2_FENCE_MASK)) {
			dev_err(dev, "%s: fence set for blocking mode\n",
				__func__);
			ret = -EINVAL;
			goto err;
		}

		ret = m2m1shot2_get_source(ctx, i, &image[i], &source);
		if (ret)
			goto err;

		ret = ctx->m21dev->ops->prepare_source(ctx, i, &image[i]);
		if (ret) {
			m2m1shot2_put_image(ctx, &image[i].img);
			goto err;
		}

		image[i].img.fence = NULL;

		if (!!(image[i].img.flags & M2M1SHOT2_IMGFLAG_ACQUIRE_FENCE)) {
			image[i].img.fence = sync_file_get_fence(source.fence);
			if (image[i].img.fence == NULL) {
				dev_err(dev, "%s: invalid acquire fence %d\n",
					__func__, source.fence);
				ret = -EINVAL;
				image[i].img.flags &=
					~M2M1SHOT2_IMGFLAG_ACQUIRE_FENCE;
				m2m1shot2_put_image(ctx, &image[i].img);
				goto err;
			}

			if (!m2m1shot2_check_fence_wait(ctx, i,
						image[i].img.fence,
						&image[i].img.waiter))
				image[i].img.flags &=
					~M2M1SHOT2_IMGFLAG_ACQUIRE_FENCE;
		}
	}

	return 0;
err:
	while (i-- > 0)
		m2m1shot2_put_image(ctx, &ctx->source[i].img);

	ctx->num_sources = 0;

	return ret;

}

static struct dma_fence *m2m1shot2_create_fence(struct m2m1shot2_context *ctx)
{
	struct m2m1shot2_fence *mf = kzalloc(sizeof(*mf), GFP_KERNEL);

	if (!mf)
		return NULL;
	spin_lock_init(&mf->lock);
	dma_fence_init(&mf->base, &m2m1shot2_fence_ops, &mf->lock,
		       ctx->fence_context, ++ctx->timeline_max);
	return &mf->base;
}

static int m2m1shot2_get_target(struct m2m1shot2_context *ctx,
				struct m2m1shot2_image *dst)
{
	struct device *dev = ctx->m21dev->dev;
	struct m2m1shot2_context_image *img = &ctx->target;
	size_t payload[M2M1SHOT2_MAX_PLANES];
	unsigned int i, num_planes;
	int ret;

	if (!M2M1SHOT2_BUFTYPE_VALID(dst->memory)) {
		dev_err(dev,
			"%s: invalid memory type %u specified for target\n",
			__func__, dst->memory);
		return -EINVAL;
	}

	if (dst->memory == M2M1SHOT2_BUFTYPE_EMPTY) {
		dev_err(dev,
			"%s: M2M1SHOT2_BUFTYPE_EMPTY is not valid for target\n",
			__func__);
		return -EINVAL;
	}

	img->fmt.fmt = dst->fmt;
	img->fmt.colorspace = dst->colorspace;

	/*
	 * The client driver may configure 0 to payload if it is not able to
	 * determine the payload before image processing especially when the
	 * result is a compressed data
	 */
	ret = ctx->m21dev->ops->prepare_format(&img->fmt, 0,
				DMA_FROM_DEVICE, payload, &num_planes);
	if (ret) {
		dev_err(dev, "%s: invalid format specified for target\n",
			__func__);
		return ret;
	}

	BUG_ON((num_planes < 1) || (num_planes > M2M1SHOT2_MAX_PLANES));

	if (num_planes != dst->num_planes) {
		dev_err(dev, "%s: wrong number of planes %u for target\n",
			__func__, dst->num_planes);
		return -EINVAL;
	}

	for (i = 0; i < num_planes; i++) {
		if ((payload[i] != 0) && (dst->plane[i].length < payload[i])) {
			dev_err(dev,
				"%s: too small size %u (plane %u)\n",
				__func__, dst->plane[i].length, i);
			return -EINVAL;
		}
	}

	ret = m2m1shot2_get_buffer(ctx, img, dst, payload, DMA_FROM_DEVICE);
	if (ret)
		return ret;

	img->flags = dst->flags;
	img->fence = NULL;

	if (!!(img->flags & M2M1SHOT2_IMGFLAG_ACQUIRE_FENCE)) {
		img->fence = sync_file_get_fence(dst->fence);
		if (img->fence == NULL) {
			dev_err(dev, "%s: invalid acquire fence %d\n",
				__func__, dst->fence);
			img->flags &= ~M2M1SHOT2_IMGFLAG_ACQUIRE_FENCE;
			m2m1shot2_put_buffer(ctx, dst->memory,
						img->plane, img->num_planes);
			return -EINVAL;
		}

		if (!m2m1shot2_check_fence_wait(ctx, ctx->num_sources,
					   img->fence, &img->waiter))
			img->flags &= ~M2M1SHOT2_IMGFLAG_ACQUIRE_FENCE;
	}

	return 0;
}

static int m2m1shot2_get_userdata(struct m2m1shot2_context *ctx,
				  struct m2m1shot2 *data)
{
	struct device *dev = ctx->m21dev->dev;
	struct m2m1shot2_image __user *usersrc = data->sources;
	unsigned int len_src, len_dst;
	bool blocking;
	int ret;

	if ((data->num_sources < 1) ||
			(data->num_sources > M2M1SHOT2_MAX_IMAGES)) {
		dev_err(dev, "%s: Invalid number of source images %u\n",
			__func__, data->num_sources);
		return -EINVAL;
	}

	blocking = !(data->flags & M2M1SHOT2_FLAG_NONBLOCK);

	while (ctx->num_sources > data->num_sources) {
		ctx->num_sources--;
		m2m1shot2_put_image(ctx, &ctx->source[ctx->num_sources].img);
	}

	ctx->ctx_private = 0;
	ctx->ctx_private2 = 0;
	ctx->num_sources = data->num_sources;
	ctx->flags = data->flags;

	ret = m2m1shot2_get_sources(ctx, usersrc, blocking);
	if (ret)
		return ret;

	if (blocking && !!(data->target.flags & M2M1SHOT2_FENCE_MASK)) {
		dev_err(dev, "%s: fence set for blocking mode\n", __func__);
		ret = -EINVAL;
		goto err;
	}

	len_src = ctx->ctx_private;

	ret = m2m1shot2_get_target(ctx, &data->target);
	if (ret)
		goto err;

	ret = ctx->m21dev->ops->prepare_target(ctx, &ctx->target);
	if (ret)
		goto err;

	len_dst = ctx->ctx_private - len_src;

	if (ctx->ctx_private < M2M1SHOT2_NEED_CACHEFLUSH_ALL)
		clear_bit(M2M1S2_CTXSTATE_CACHECLEANALL, &ctx->state);
	else
		set_bit(M2M1S2_CTXSTATE_CACHECLEANALL, &ctx->state);

	if (ctx->ctx_private2 > 0)
		set_bit(M2M1S2_CTXSTATE_CACHEFLUSH, &ctx->state);
	else
		clear_bit(M2M1S2_CTXSTATE_CACHEFLUSH, &ctx->state);

	if (ctx->ctx_private2 < M2M1SHOT2_NEED_CACHEFLUSH_ALL)
		clear_bit(M2M1S2_CTXSTATE_CACHEFLUSHALL, &ctx->state);
	else
		set_bit(M2M1S2_CTXSTATE_CACHEFLUSHALL, &ctx->state);

	if (!(ctx->target.flags & M2M1SHOT2_IMGFLAG_NO_CACHEINV) &&
				(len_dst < M2M1SHOT2_NEED_CACHEFLUSH_ALL))
		clear_bit(M2M1S2_CTXSTATE_CACHEINVALALL, &ctx->state);
	else
		set_bit(M2M1S2_CTXSTATE_CACHEINVALALL, &ctx->state);

	return 0;
err:
	m2m1shot2_put_source_images(ctx);
	return ret;
}

static int m2m1shot2_install_fence(struct m2m1shot2_context *ctx,
				   struct dma_fence *fence,
				   s32 __user *pfd)
{
	struct sync_file *sf;
	int fd = get_unused_fd_flags(O_CLOEXEC);

	if (fd < 0) {
		dev_err(ctx->m21dev->dev, "%s: failed to allocated unused fd\n",
			__func__);
		return fd;
	}

	sf = sync_file_create(fence);
	if (!sf) {
		put_unused_fd(fd);
		return -ENOMEM;
	}

	if (put_user(fd, pfd)) {
		dev_err(ctx->m21dev->dev,
			"%s: failed to put release fence to user\n", __func__);
		fput(sf->file);
		put_unused_fd(fd);
		return -EFAULT;
	}

	fd_install(fd, sf->file);

	return fd;
}

static int m2m1shot2_create_release_fence(struct m2m1shot2_context *ctx,
					struct m2m1shot2_image __user *usertgt,
					struct m2m1shot2_image __user usersrc[],
					unsigned int num_sources)
{
	struct dma_fence *fence = NULL;
	struct m2m1shot2_context_image *img;
	unsigned int i;
	unsigned int num_fences = 0;
	unsigned long flags;
	int ret = 0;

	if (!!(ctx->target.flags & M2M1SHOT2_IMGFLAG_RELEASE_FENCE))
		num_fences++;

	for (i = 0; i < ctx->num_sources; i++) {
		if (!!(ctx->source[i].img.flags &
					M2M1SHOT2_IMGFLAG_RELEASE_FENCE))
			num_fences++;
	}

	if (num_fences == 0)
		return 0;

	fence = m2m1shot2_create_fence(ctx);
	if (!fence)
		return -ENOMEM;

	for (i = 0; i < ctx->num_sources; i++) {
		img = &ctx->source[i].img;
		if (!!(img->flags & M2M1SHOT2_IMGFLAG_RELEASE_FENCE)) {
			ret = m2m1shot2_install_fence(ctx,
					fence, &usersrc[i].fence);
			if (ret < 0)
				goto err;
		}
	}

	img = &ctx->target;
	if (!!(ctx->target.flags & M2M1SHOT2_IMGFLAG_RELEASE_FENCE)) {
		ret = m2m1shot2_install_fence(ctx, fence,
						&usertgt->fence);
		if (ret < 0)
			goto err;
	}

	/* keep the creation reference until the job is finished */
	spin_lock_irqsave(&ctx->fence_lock, flags);
	WARN_ON(ctx->pending_release);
	ctx->pending_release = fence;
	spin_unlock_irqrestore(&ctx->fence_lock, flags);
	ctx->release_fence = fence;

	return 0;
err:
	/* fds already installed must not wait forever */
	dma_fence_signal(fence);
	dma_fence_put(fence);

	return ret;
}

static int m2m1shot2_wait_put_user(struct m2m1shot2_context *ctx,
				   struct m2m1shot2 __user *uptr, u32 userflag)
{
	int ret = 0;

	if (!m2m1shot2_wait_process(ctx->m21dev, ctx)) {
		userflag |= M2M1SHOT2_FLAG_ERROR;
		ret = put_user(userflag, &uptr->flags);
	} else {
		unsigned int i;

		for (i = 0; i < ctx->target.num_planes; i++)
			ret = put_user(ctx->target.plane[i].payload,
					&uptr->target.plane[i].payload);
		put_user(ctx->work_delay_in_nsec, &uptr->work_delay_in_nsec);
	}

	return ret;
}

/*
 * find the previous requested context which has lower priority.
 * if it find that context, we have to return error to avoid
 * for the higher context to wait finishing the lower context job
 */
static int m2m1shot2_prepare_priority_context(struct m2m1shot2_context *ctx)
{
	struct m2m1shot2_device *m21dev = ctx->m21dev;
	struct m2m1shot2_context *pctx;
	unsigned long flags;
	int cond = ctx->priority;

	spin_lock_irqsave(&m21dev->lock_ctx, flags);
	list_for_each_entry(pctx, &m21dev->active_contexts, node) {
		if (pctx->priority < cond)
			goto err_busy;
	}

	list_for_each_entry(pctx, &m21dev->contexts, node) {
		if ((pctx->priority < cond) && M2M1S2_CTXSTATE_BUSY(pctx))
			goto err_busy;
	}

	pctx = m2m1shot2_current_context(m21dev);
	if (pctx && (pctx->priority < cond))
		goto err_busy;

	spin_unlock_irqrestore(&m21dev->lock_ctx, flags);

	return 0;

err_busy:
	spin_unlock_irqrestore(&m21dev->lock_ctx, flags);

	return -EBUSY;
}

static long m2m1shot2_ioctl(struct file *filp,
			    unsigned int cmd, unsigned long arg)
{
	struct m2m1shot2_context *ctx = filp->private_data;
	struct m2m1shot2_device *m21dev = ctx->m21dev;
	int ret = 0;

	mutex_lock(&ctx->mutex);

	switch (cmd) {
	case M2M1SHOT2_IOC_REQUEST_PERF:
	{
		struct m2m1shot2_performance_data data;
		int i;

		if (!ctx->m21dev->ops->prepare_perf) {
			dev_err(ctx->m21dev->dev,
				"%s: it doesn't exist the prepare performance\n", __func__);
			ret = -ENOTTY;
			break;
		}

		if (copy_from_user(&data, (void __user *)arg, sizeof(data))) {
			dev_err(ctx->m21dev->dev,
				"%s: Failed to performance data\n", __func__);
			ret = -EFAULT;
			break;
		}

		if (data.num_frames >= M2M1SHOT2_PERF_MAX_FRAMES) {
			ret = -EINVAL;
			break;
		}

		for (i = 0; i < data.num_frames; i++) {
			if (data.frame[i].num_layers >= M2M1SHOT2_MAX_IMAGES) {
				ret = -EINVAL;
				break;
			}
		}
		if (!ret)
			ret = ctx->m21dev->ops->prepare_perf(ctx, &data);

		break;
	}
	case M2M1SHOT2_IOC_SET_PRIORITY:
	{
		enum m2m1shot2_priority data;

		get_user(data, (enum m2m1shot2_priority __user *)arg);

		if ((data < M2M1SHOT2_LOW_PRIORITY) ||
					(data >= M2M1SHOT2_PRIORITY_END)) {
			dev_err(m21dev->dev,
				"%s: m2m1shot2 does not allow %d priority\n",
				__func__, data);
			ret = -EINVAL;
			break;
		}

		/* update current priority */
		if (data != ctx->priority) {
			spin_lock(&m21dev->lock_priority);
			m21dev->prior_stats[ctx->priority] -= 1;
			m21dev->prior_stats[data] += 1;
			ctx->priority = data;
			spin_unlock(&m21dev->lock_priority);
		}

		ret = m2m1shot2_prepare_priority_context(ctx);
		if (ret < 0)
			break;

		break;
	}
	case M2M1SHOT2_IOC_PROCESS:
	{
		struct m2m1shot2 __user *uptr = (struct m2m1shot2 __user *)arg;
		struct m2m1shot2 data;
		int i;

		if (copy_from_user(&data, uptr, sizeof(data))) {
			dev_err(ctx->m21dev->dev,
				"%s: Failed to read userdata\n", __func__);
			ret = -EFAULT;
			break;
		}

		/* wait for completion of the previous task */
		if (!M2M1S2_CTXSTATE_IDLE(ctx))
			m2m1shot2_wait_process(m21dev, ctx);

		/*
		 * A new process request with the lower priority than the
		 * heighest priority currently configured is not allowed
		 * to be executed and m2m1shot2 simply returns -EBUSY
		 */
		spin_lock(&m21dev->lock_priority);
		for (i = ctx->priority + 1; i < M2M1SHOT2_PRIORITY_END; i++) {
			if (m21dev->prior_stats[i] > 0)
				break;
		}
		spin_unlock(&m21dev->lock_priority);

		if (i < M2M1SHOT2_PRIORITY_END) {
			ret = -EBUSY;
			break;
		}

		kref_init(&ctx->starter);

		ret = m2m1shot2_get_userdata(ctx, &data);
		if (ret < 0)
			break;

		ctx->release_fence = NULL;
		if (!!(data.flags & M2M1SHOT2_FLAG_NONBLOCK)) {
			ret = m2m1shot2_create_release_fence(ctx, &uptr->target,
						data.sources, data.num_sources);
			if (ret < 0) {
				m2m1shot2_put_images(ctx);
				break;
			}
		}

		m2m1shot2_start_context(ctx->m21dev, ctx);

		if (!(data.flags & M2M1SHOT2_FLAG_NONBLOCK)) {
			ret = m2m1shot2_wait_put_user(ctx, uptr, data.flags);
			m2m1shot2_put_images(ctx);
		}

		break;
	}
	case M2M1SHOT2_IOC_WAIT_PROCESS:
	{
		struct m2m1shot2 __user *uptr = (void __user *)arg;
		struct m2m1shot2 data;

		if (M2M1S2_CTXSTATE_IDLE(ctx)) {
			ret = -EAGAIN;
			break;
		}

		if (copy_from_user(&data, uptr, sizeof(data))) {
			dev_err(ctx->m21dev->dev,
				"%s: Failed to read userdata\n", __func__);
			ret = -EFAULT;
			break;
		}

		ret = m2m1shot2_wait_put_user(ctx, uptr, data.flags);

		break;
	}
	case M2M1SHOT2_IOC_CUSTOM:
	{
		struct m2m1shot2_custom_data data;

		if (!ctx->m21dev->ops->custom_ioctl) {
			dev_err(ctx->m21dev->dev,
				"%s: it doesn't exist the custom ioctl\n", __func__);
			ret = -ENOTTY;
			break;
		}
		if (copy_from_user(&data, (void __user *)arg, sizeof(data))) {
			dev_err(ctx->m21dev->dev,
				"%s: Failed to read userdata\n", __func__);
			ret = -EFAULT;
			break;
		}
		ret = ctx->m21dev->ops->custom_ioctl(ctx, data.cmd, data.arg);
		break;
	}
	default:
	{
		dev_err(ctx->m21dev->dev,
			"%s: unknown ioctl command %#x\n", __func__, cmd);
		ret = -EINVAL;
		break;
	}
	} /* switch */

	mutex_unlock(&ctx->mutex);

	return ret;
}

#ifdef CONFIG_COMPAT
struct compat_m2m1shot2_buffer {
	union {
		compat_ulong_t	userptr;
		__s32		fd;
	};
	__u32		offset;
	__u32		length;
	__u32		payload;
	compat_ulong_t	reserved;
};

struct compat_m2m1shot2_image {
	__u32				flags;
	__s32				fence;
	__u8				memory;
	__u8				num_planes;
	struct compat_m2m1shot2_buffer	plane[M2M1SHOT2_MAX_PLANES];
	struct m2m1shot2_format		fmt;
	struct m2m1shot2_extra		ext;
	__u32				reserved[4];
};

struct compat_m2m1shot2 {
	compat_uptr_t			sources;
	struct compat_m2m1shot2_image	target;
	__u8				num_sources;
	__u32				flags;
	__u32				reserved1;
	__u32				reserved2;
	compat_ulong_t			work_delay_in_nsec;
	compat_ulong_t			reserved4;
};

#define COMPAT_M2M1SHOT2_IOC_PROCESS	  _IOWR('M', 4, struct compat_m2m1shot2)
#define COMPAT_M2M1SHOT2_IOC_WAIT_PROCESS _IOR('M', 5, struct compat_m2m1shot2)

static int m2m1shot2_compat_get_imagedata(struct device *dev,
				struct m2m1shot2_image __user *img,
				struct compat_m2m1shot2_image __user *cimg)
{
	__u32 uw;
	__s32 sw;
	__u8 b;
	compat_ulong_t l;
	unsigned int i;
	int ret;

	ret =  get_user(uw, &cimg->flags);
	ret |= put_user(uw, &img->flags);
	ret |= get_user(sw, &cimg->fence);
	ret |= put_user(sw, &img->fence);
	ret |= get_user(b, &cimg->memory);
	ret |= put_user(b, &img->memory);
	ret |= get_user(b, &cimg->num_planes);
	ret |= put_user(b, &img->num_planes);
	if (b > M2M1SHOT2_MAX_PLANES) {
		dev_err(dev, "%s: too many plane count %u\n", __func__, b);
		return -EINVAL;
	}
	for (i = 0; i < b; i++) { /* b contains num_planes */
		/* userptr is not smaller than fd */
		ret |= get_user(l, &cimg->plane[i].userptr);
		ret |= put_user(l, &img->plane[i].userptr);
		ret |= get_user(uw, &cimg->plane[i].offset);
		ret |= put_user(uw, &img->plane[i].offset);
		ret |= get_user(uw, &cimg->plane[i].length);
		ret |= put_user(uw, &img->plane[i].length);
		ret |= get_user(uw, &cimg->plane[i].payload);
		ret |= put_user(uw, &img->plane[i].payload);
		ret |= get_user(l, &cimg->plane[i].reserved);
		ret |= put_user(l, &img->plane[i].reserved);
	}

	ret |= copy_in_user(&img->fmt, &cimg->fmt,
			sizeof(struct m2m1shot2_format) +
			sizeof(struct m2m1shot2_extra) + sizeof(__u32) * 4);

	return ret ? -EFAULT : 0;
}

static int m2m1shot2_compat_put_imagedata(struct m2m1shot2_image __user *img,
				struct compat_m2m1shot2_image __user *cimg)
{
	__u32 uw;
	__s32 sw;
	__u8 b;
	compat_ulong_t l;
	unsigned int i;
	int ret;

	ret  = get_user(uw, &img->flags);
	ret |= put_user(uw, &cimg->flags);
	if (!!(uw & M2M1SHOT2_IMGFLAG_RELEASE_FENCE)) {
		ret |= get_user(sw, &img->fence);
		ret |= put_user(sw, &cimg->fence);
	}
	ret |= get_user(b, &img->num_planes);
	for (i = 0; i < b; i++) { /* b contains num_planes */
		ret |= get_user(uw, &img->plane[i].payload);
		ret |= put_user(uw, &cimg->plane[i].payload);
		ret |= get_user(l, &img->plane[i].reserved);
		ret |= put_user(l, &cimg->plane[i].reserved);
	}

	return ret;
}

static int m2m1shot2_compat_put_fence(struct m2m1shot2_image __user *img,
				struct compat_m2m1shot2_image __user *cimg)
{
	__u32 uw = 0;
	__s32 sw;
	int ret;

	ret =  get_user(uw, &img->flags);
	if (!!(uw & M2M1SHOT2_IMGFLAG_RELEASE_FENCE)) {
		ret |= get_user(sw, &img->fence);
		ret |= put_user(sw, &cimg->fence);
	}

	return ret;
}

static long m2m1shot2_compat_ioctl32(struct file *filp,
				unsigned int cmd, unsigned long arg)
{
	struct m2m1shot2_context *ctx = filp->private_data;
	struct device *dev = ctx->m21dev->dev;
	struct compat_m2m1shot2 __user *cdata = compat_ptr(arg);
	struct m2m1shot2 __user *data;
	struct m2m1shot2_image __user *src;
	int ret;
	compat_ulong_t l;
	compat_uptr_t sources;
	__u32 w;
	__u8 num_sources;

	switch (cmd) {
	case COMPAT_M2M1SHOT2_IOC_PROCESS:
		cmd = M2M1SHOT2_IOC_PROCESS;
		break;
	case COMPAT_M2M1SHOT2_IOC_WAIT_PROCESS:
		cmd = M2M1SHOT2_IOC_WAIT_PROCESS;
		break;
	case M2M1SHOT2_IOC_CUSTOM:
	case M2M1SHOT2_IOC_SET_PRIORITY:
	case M2M1SHOT2_IOC_REQUEST_PERF:
		if (!filp->f_op->unlocked_ioctl)
			return -ENOTTY;

		return filp->f_op->unlocked_ioctl(filp, cmd,
						(unsigned long)compat_ptr(arg));
	default:
		dev_err(dev, "%s: unknown ioctl command %#x\n", __func__, cmd);
		return -EINVAL;
	}

	data = compat_alloc_user_space(sizeof(*data));

	ret  = get_user(num_sources, &cdata->num_sources);
	ret |= put_user(num_sources, &data->num_sources);
	ret |= get_user(w, &cdata->flags);
	ret |= put_user(w, &data->flags);
	ret |= get_user(w, &cdata->reserved1);
	ret |= put_user(w, &data->reserved1);
	ret |= get_user(w, &cdata->reserved2);
	ret |= put_user(w, &data->reserved2);
	ret |= get_user(l, &cdata->work_delay_in_nsec);
	ret |= put_user(l, &data->work_delay_in_nsec);
	ret |= get_user(l, &cdata->reserved4);
	ret |= put_user(l, &data->reserved4);
	ret |= get_user(sources, &cdata->sources);
	if (ret) {
		dev_err(dev, "%s: failed to read m2m1shot2 data\n", __func__);
		return -EFAULT;
	}

	ret = m2m1shot2_compat_get_imagedata(
				dev, &data->target, &cdata->target);
	if (ret) {
		dev_err(dev, "%s: failed to read the target image data\n",
			__func__);
		return ret;
	}

	if (cmd == M2M1SHOT2_IOC_PROCESS) {
		struct compat_m2m1shot2_image __user *csc = compat_ptr(sources);

		src = compat_alloc_user_space(
				sizeof(*data) + sizeof(*src) * num_sources);
		if (w > M2M1SHOT2_MAX_IMAGES) {
			dev_err(dev, "%s: too many source images %u\n",
				__func__, w);
			return -EINVAL;
		}

		for (w = 0; w < num_sources; w++) {
			ret = m2m1shot2_compat_get_imagedata(
						dev, &src[w], &csc[w]);
			if (ret) {
				dev_err(dev,
				"%s: failed to read %dth source image data\n",
					__func__, w);
				return ret;
			}
		}


		if (put_user(src, &data->sources)) {
			dev_err(dev, "%s: failed to handle source image data\n",
				__func__);
			return -EFAULT;
		}
	}

	ret = m2m1shot2_ioctl(filp, cmd, (unsigned long)data);
	if (ret)
		return ret;

	ret  = get_user(w, &data->flags);
	ret |= put_user(w, &cdata->flags);
	ret |= get_user(w, &data->reserved1);
	ret |= put_user(w, &cdata->reserved1);
	ret |= get_user(w, &data->reserved2);
	ret |= put_user(w, &cdata->reserved2);
	ret |= get_user(l, &data->work_delay_in_nsec);
	ret |= put_user(l, &cdata->work_delay_in_nsec);
	ret |= get_user(l, &data->reserved4);
	ret |= put_user(l, &cdata->reserved4);
	ret |= m2m1shot2_compat_put_imagedata(&data->target, &cdata->target);

	if (cmd == M2M1SHOT2_IOC_PROCESS) {
		struct compat_m2m1shot2_image __user *csc = compat_ptr(sources);

		for (w = 0; w < num_sources; w++)
			ret |= m2m1shot2_compat_put_fence(&src[w], &csc[w]);
	}

	if (ret)
		dev_err(dev, "%s: failed to write userdata\n", __func__);

	return ret;
}
#endif

static const struct file_operations m2m1shot2_fops = {
	.owner          = THIS_MODULE,
	.open           = m2m1shot2_open,
	.release        = m2m1shot2_release,
	.unlocked_ioctl	= m2m1shot2_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= m2m1shot2_compat_ioctl32,
#endif
};

struct m2m1shot2_device *m2m1shot2_create_device(struct device *dev,
					const struct m2m1shot2_devops *ops,
					const char *nodename, int id,
					unsigned long attr)
{
	struct m2m1shot2_device *m21dev;
	char *name;
	size_t name_size;
	int ret = -ENOMEM;

	/* one DMA segment per IOVA range: no 64 KiB max_segment_size split */
	if (dev->dma_parms)
		dma_set_max_seg_size(dev, UINT_MAX);

	if (!ops || !ops->init_context || !ops->free_context ||
			!ops->prepare_format || !ops->device_run) {
		dev_err(dev,
			"%s: m2m1shot2_devops is insufficient\n", __func__);
		return ERR_PTR(-EINVAL);
	}

	if (!nodename) {
		dev_err(dev, "%s: node name is not specified\n", __func__);
		return ERR_PTR(-EINVAL);
	}

	name_size = strlen(nodename) + 1;

	if (id >= 0)
		name_size += 3; /* instance number: maximum 3 digits */

	name = kmalloc(name_size, GFP_KERNEL);
	if (!name)
		return ERR_PTR(-ENOMEM);

	if (id < 0)
		strncpy(name, nodename, name_size);
	else
		scnprintf(name, name_size, "%s%d", nodename, id);

	m21dev = kzalloc(sizeof(*m21dev), GFP_KERNEL);
	if (!m21dev)
		goto err_m21dev;

	m21dev->schedule_workqueue =
		create_singlethread_workqueue("m2m1shot2-scheduler");
	if (!m21dev->schedule_workqueue) {
		dev_err(dev, "failed to create workqueue for scheduling\n");
		ret = -ENOMEM;
		goto err_workqueue;
	}

	m21dev->destroy_workqueue =
		create_singlethread_workqueue("m2m1shot2-destructor");
	if (!m21dev->destroy_workqueue) {
		dev_err(dev, "failed to create workqueue for context destruction\n");
		ret = -ENOMEM;
		goto err_workqueue2;
	}

	spin_lock_init(&m21dev->lock_ctx);
	spin_lock_init(&m21dev->lock_priority);
	INIT_LIST_HEAD(&m21dev->contexts);
	INIT_LIST_HEAD(&m21dev->active_contexts);

	m21dev->dev = dev;
	m21dev->ops = ops;
	m21dev->attr = attr;

	/* Everything the open path uses is initialized before the node appears. */
	m21dev->misc.minor = MISC_DYNAMIC_MINOR;
	m21dev->misc.name = name;
	m21dev->misc.fops = &m2m1shot2_fops;
	ret = misc_register(&m21dev->misc);
	if (ret)
		goto err_misc;

	dev_info(dev, "registered as m2m1shot2 device");

	return m21dev;
err_misc:
	destroy_workqueue(m21dev->destroy_workqueue);
err_workqueue2:
	destroy_workqueue(m21dev->schedule_workqueue);
err_workqueue:
	kfree(m21dev);
err_m21dev:
	kfree(name);

	return ERR_PTR(ret);
}
EXPORT_SYMBOL(m2m1shot2_create_device);

void m2m1shot2_destroy_device(struct m2m1shot2_device *m21dev)
{
	destroy_workqueue(m21dev->destroy_workqueue);
	destroy_workqueue(m21dev->schedule_workqueue);
	misc_deregister(&m21dev->misc);
	kfree(m21dev->misc.name);
	kfree(m21dev);
	/* TODO: something forgot to release? */
}
EXPORT_SYMBOL(m2m1shot2_destroy_device);
