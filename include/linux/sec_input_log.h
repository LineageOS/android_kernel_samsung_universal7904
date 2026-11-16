/* Samsung touch logging macros (4.4 uapi/linux/input.h), kernel-side only.
 * Only the CONFIG_SEC_DEBUG_TSP_LOG=n branch is ported; the TSP log buffer is not. */
#ifndef __LINUX_SEC_INPUT_LOG_H
#define __LINUX_SEC_INPUT_LOG_H

#include <linux/device.h>
#include <linux/kernel.h>

#define SECLOG			"[sec_input]"
#define INPUT_LOG_BUF_SIZE	512

#define input_dbg(mode, dev, fmt, ...)						\
({										\
	static char input_log_buf[INPUT_LOG_BUF_SIZE];				\
	snprintf(input_log_buf, sizeof(input_log_buf), "%s %s", SECLOG, fmt);	\
	dev_dbg(dev, input_log_buf, ## __VA_ARGS__);				\
})
#define input_info(mode, dev, fmt, ...)						\
({										\
	static char input_log_buf[INPUT_LOG_BUF_SIZE];				\
	snprintf(input_log_buf, sizeof(input_log_buf), "%s %s", SECLOG, fmt);	\
	dev_info(dev, input_log_buf, ## __VA_ARGS__);				\
})
#define input_err(mode, dev, fmt, ...)						\
({										\
	static char input_log_buf[INPUT_LOG_BUF_SIZE];				\
	snprintf(input_log_buf, sizeof(input_log_buf), "%s %s", SECLOG, fmt);	\
	dev_err(dev, input_log_buf, ## __VA_ARGS__);				\
})
#define input_raw_info(mode, dev, fmt, ...) input_info(mode, dev, fmt, ...)
#define input_log_fix() {}
#define input_raw_data_clear() {}
/* TSP command history is a vendor debug feature (CONFIG_SEC_DEBUG_TSP_LOG); off here. */
#define sec_debug_tsp_command_history(msg) do { } while (0)

#endif
