# Makefile of modem_v1

EXTRA_CFLAGS += -Idrivers/misc/modem_v1 \
		-DCONFIG_OPTION_REGION=\"$(PROJECT_REGION)\"

obj-y += modem_main.o modem_variation.o modem_io_device.o modem_utils.o modem_dump.o

obj-$(CONFIG_LINK_DEVICE_SHMEM) += link_device_shmem.o \
	link_device_memory_snapshot.o link_device_memory_flow_control.o \
	link_device_memory_debug.o link_device_memory_sbd.o modem_notifier.o
obj-$(CONFIG_UMTS_MODEM_SS310AP) += modem_ctrl_ss310ap.o
ccflags-y += -DCONFIG_SOC_EXYNOS7885 -include $(srctree)/drivers/misc/modem_v1/modem_port.h
