/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SAMSUNG_MODEM_510_PORT_H
#define __SAMSUNG_MODEM_510_PORT_H

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/ktime.h>
#include <linux/timekeeping.h>
#include <linux/sizes.h>
#include <linux/seq_file.h>
#include <linux/sched/signal.h>
#include <linux/pm_qos.h>

/* TODO(5.10 port): vendor ECT/ARGOS and cluster/MIF/INT QoS providers are
 * unavailable. Keep mailbox protocol and sysfs but make no frequency votes.
 * These identifiers are local selectors, not 5.10 PM QoS classes.
 */
enum mif_port_qos_class {
	PM_QOS_CLUSTER0_FREQ_MIN,
	PM_QOS_CLUSTER1_FREQ_MIN,
	PM_QOS_BUS_THROUGHPUT,
	PM_QOS_DEVICE_THROUGHPUT,
};

static inline void mif_port_qos_add_request(struct pm_qos_request *req,
					 int qos_class, s32 value)
{
}

static inline void mif_port_qos_update_request(struct pm_qos_request *req,
					    s32 value)
{
}

#endif
