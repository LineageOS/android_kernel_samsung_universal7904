// SPDX-License-Identifier: GPL-2.0
/*
 * DECON sync timeline on top of dma_fence / sync_file.
 *
 * 4.4 used the staging sw_sync timeline (sw_sync_timeline_create/inc,
 * sw_sync_pt_create) and sync_fence_*; none of that kernel API exists in 5.10
 * (sw_sync only has the debugfs ioctl front end now). This keeps the same
 * semantics: a point with value N signals once timeline->value >= N, fences
 * are exported to user space as sync_file fds, acquire fences come in through
 * sync_file_get_fence().
 */
#include <linux/dma-fence.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/sync_file.h>

#include "decon.h"

static const struct dma_fence_ops decon_pt_ops;

static inline struct decon_pt *to_decon_pt(struct dma_fence *fence)
{
	return container_of(fence, struct decon_pt, base);
}

static void decon_timeline_free(struct kref *kref)
{
	struct decon_timeline *tl = container_of(kref, struct decon_timeline, kref);

	kfree(tl);
}

struct decon_timeline *decon_timeline_create(const char *name)
{
	struct decon_timeline *tl = kzalloc(sizeof(*tl), GFP_KERNEL);

	if (!tl)
		return NULL;

	kref_init(&tl->kref);
	strlcpy(tl->name, name, sizeof(tl->name));
	tl->context = dma_fence_context_alloc(1);
	spin_lock_init(&tl->lock);
	INIT_LIST_HEAD(&tl->pts);

	return tl;
}

/* sw_sync_timeline_inc(): advance the timeline and signal the passed points */
void decon_timeline_inc(struct decon_timeline *tl, u32 inc)
{
	struct decon_pt *pt, *next;
	LIST_HEAD(signaled);
	unsigned long flags;

	spin_lock_irqsave(&tl->lock, flags);
	tl->value += inc;
	list_for_each_entry_safe(pt, next, &tl->pts, link) {
		/* the point is already being released: it unlinks itself */
		if ((s32)(tl->value - pt->value) < 0 ||
		    !dma_fence_get_rcu(&pt->base))
			continue;
		dma_fence_is_signaled_locked(&pt->base);
		list_move_tail(&pt->link, &signaled);
	}
	spin_unlock_irqrestore(&tl->lock, flags);

	/*
	 * Drop the references outside tl->lock: the last put runs
	 * decon_pt_release(), which takes the same lock.
	 */
	list_for_each_entry_safe(pt, next, &signaled, link) {
		list_del_init(&pt->link);
		dma_fence_put(&pt->base);
	}
}

static const char *decon_pt_get_driver_name(struct dma_fence *fence)
{
	return "decon";
}

static const char *decon_pt_get_timeline_name(struct dma_fence *fence)
{
	return to_decon_pt(fence)->tl->name;
}

static bool decon_pt_signaled(struct dma_fence *fence)
{
	struct decon_pt *pt = to_decon_pt(fence);

	/* same wrap-safe compare as sw_sync */
	return (s32)(pt->tl->value - pt->value) >= 0;
}

static void decon_pt_release(struct dma_fence *fence)
{
	struct decon_pt *pt = to_decon_pt(fence);
	unsigned long flags;

	spin_lock_irqsave(fence->lock, flags);
	if (!list_empty(&pt->link))
		list_del(&pt->link);
	spin_unlock_irqrestore(fence->lock, flags);

	kref_put(&pt->tl->kref, decon_timeline_free);
	dma_fence_free(fence);
}

static const struct dma_fence_ops decon_pt_ops = {
	.get_driver_name = decon_pt_get_driver_name,
	.get_timeline_name = decon_pt_get_timeline_name,
	.signaled = decon_pt_signaled,
	.release = decon_pt_release,
};

/* sw_sync_pt_create() */
struct decon_pt *decon_pt_create(struct decon_timeline *tl, u32 value)
{
	struct decon_pt *pt = kzalloc(sizeof(*pt), GFP_KERNEL);
	unsigned long flags;

	if (!pt)
		return NULL;

	kref_get(&tl->kref);
	pt->tl = tl;
	pt->value = value;
	INIT_LIST_HEAD(&pt->link);
	dma_fence_init(&pt->base, &decon_pt_ops, &tl->lock, tl->context, value);

	spin_lock_irqsave(&tl->lock, flags);
	if (!dma_fence_is_signaled_locked(&pt->base))
		list_add_tail(&pt->link, &tl->pts);
	spin_unlock_irqrestore(&tl->lock, flags);

	return pt;
}

/* sync_fence_create("display", pt): the fence handed to user space */
struct sync_file *decon_sync_file_create(struct decon_pt *pt)
{
	struct sync_file *sf = sync_file_create(&pt->base);

	/* the sync_file holds its own reference to the point */
	dma_fence_put(&pt->base);

	return sf;
}

/* sync_fence_wait(fence, timeout_ms): 0 when signaled, -ETIME on timeout */
int decon_fence_wait(struct dma_fence *fence, unsigned int timeout_ms)
{
	long ret = dma_fence_wait_timeout(fence, true, msecs_to_jiffies(timeout_ms));

	if (ret < 0)
		return ret;
	if (ret == 0)
		return -ETIME;
	return 0;
}
