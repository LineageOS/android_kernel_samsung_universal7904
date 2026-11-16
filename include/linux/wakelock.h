/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Android wake_lock API on top of wakeup sources, for the Samsung 4.4 drivers.
 *
 * 4.4 include/linux/wakelock.h embedded a struct wakeup_source and used
 * wakeup_source_init()/wakeup_source_trash(). In 5.10 wakeup sources are
 * allocated (wakeup_source_register()), so the lock holds a pointer. Behaviour
 * is the same: wake_lock() stays awake until wake_unlock(),
 * wake_lock_timeout() takes a jiffies timeout.
 */
#ifndef _LINUX_WAKELOCK_H
#define _LINUX_WAKELOCK_H

#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/pm_wakeup.h>

enum {
	WAKE_LOCK_SUSPEND, /* Prevent suspend */
	WAKE_LOCK_TYPE_COUNT
};

struct wake_lock {
	struct wakeup_source *ws;
};

static inline void wake_lock_init(struct wake_lock *lock, int type,
				  const char *name)
{
	lock->ws = wakeup_source_register(NULL, name);
}

static inline void wake_lock_destroy(struct wake_lock *lock)
{
	wakeup_source_unregister(lock->ws);
	lock->ws = NULL;
}

static inline void wake_lock(struct wake_lock *lock)
{
	__pm_stay_awake(lock->ws);
}

static inline void wake_lock_timeout(struct wake_lock *lock, long timeout)
{
	__pm_wakeup_event(lock->ws, jiffies_to_msecs(timeout));
}

static inline void wake_unlock(struct wake_lock *lock)
{
	__pm_relax(lock->ws);
}

static inline int wake_lock_active(struct wake_lock *lock)
{
	return lock->ws && lock->ws->active;
}

#endif
