# SPDX-License-Identifier: GPL-2.0
# m20lte build set of the Samsung 4.4 fimc-legacy driver (ISP v6.20.0, MCD vendor).
# Stages: sensor subdevs, core/CSIS, ischain/hardware/interface, V4L2 nodes.

fimc-is-sensor-y += \
	sensor/csi2/fimc-is-hw-csi-v5_0.o \
	sensor/fimc-is-helper-i2c.o \
	sensor/module_framework/actuator/fimc-is-actuator-dw9808.o \
	sensor/module_framework/actuator/fimc-is-helper-actuator-i2c.o \
	sensor/module_framework/cis/fimc-is-cis-3l6.o \
	sensor/module_framework/cis/fimc-is-cis-4ha.o \
	sensor/module_framework/cis/fimc-is-cis-5e9.o \
	sensor/module_framework/cis/fimc-is-cis.o \
	sensor/module_framework/fimc-is-control-actuator.o \
	sensor/module_framework/fimc-is-control-sensor.o \
	sensor/module_framework/fimc-is-device-sensor-peri.o \
	sensor/module_framework/fimc-is-interface-actuator.o \
	sensor/module_framework/fimc-is-interface-sensor.o \
	sensor/module_framework/flash/fimc-is-flash-gpio.o \
	sensor/module_framework/modules/fimc-is-device-module-3l6.o \
	sensor/module_framework/modules/fimc-is-device-module-4ha.o \
	sensor/module_framework/modules/fimc-is-device-module-5e9.o \
	sensor/module_framework/modules/fimc-is-device-module-base.o \
	vendor/mcd/crc32.o \
	vendor/mcd/fimc-is-device-eeprom.o \
	vendor/mcd/fimc-is-device-from.o \
	vendor/mcd/fimc-is-sec-define.o \
	vendor/mcd/fimc-is-sysfs.o \
	vendor/mcd/fimc-is-vender.o

fimc-is-core-y += \
	fimc-is-binary.o \
	fimc-is-clk-gate.o \
	fimc-is-core.o \
	fimc-is-debug.o \
	fimc-is-device-csi_v3.o \
	fimc-is-devicemgr.o \
	fimc-is-dt.o \
	fimc-is-dvfs.o \
	fimc-is-framemgr.o \
	fimc-is-i2c.o \
	fimc-is-mem.o \
	fimc-is-pipe.o \
	fimc-is-resourcemgr.o \
	fimc-is-spi.o \
	fimc-is-time.o

fimc-is-chain-y += \
	fimc-is-device-ischain.o \
	fimc-is-device-sensor_v2.o \
	fimc-is-groupmgr.o \
	fimc-is-interface-wrap.o \
	fimc-is-subdev-ctrl.o \
	hardware/api/fimc-is-hw-api-common.o \
	hardware/api/fimc-is-hw-api-mcscaler-v420.o \
	hardware/api/fimc-is-hw-api-vra.o \
	hardware/fimc-is-hw-3aa.o \
	hardware/fimc-is-hw-control.o \
	hardware/fimc-is-hw-dm.o \
	hardware/fimc-is-hw-isp.o \
	hardware/fimc-is-hw-mcscaler-v2.o \
	hardware/fimc-is-hw-tdnr-v2.o \
	hardware/fimc-is-hw-tpu.o \
	hardware/fimc-is-hw-vra.o \
	interface/fimc-is-interface-ddk.o \
	interface/fimc-is-interface-ischain.o \
	interface/fimc-is-interface-library.o \
	interface/fimc-is-interface-vra.o \
	ischain/fimc-is-v6_20_0/fimc-is-hw-chain.o \
	ischain/fimc-is-v6_20_0/fimc-is-hw-dvfs.o \
	ischain/fimc-is-v6_20_0/fimc-is-hw-pwr.o \
	ischain/fimc-is-v6_20_0/fimc-is-subdev-3aa.o \
	ischain/fimc-is-v6_20_0/fimc-is-subdev-3ac.o \
	ischain/fimc-is-v6_20_0/fimc-is-subdev-3ap.o \
	ischain/fimc-is-v6_20_0/fimc-is-subdev-isp.o \
	ischain/fimc-is-v6_20_0/fimc-is-subdev-ixc.o \
	ischain/fimc-is-v6_20_0/fimc-is-subdev-ixp.o \
	ischain/fimc-is-v6_20_0/fimc-is-subdev-mcs.o \
	ischain/fimc-is-v6_20_0/fimc-is-subdev-mcsp.o \
	ischain/fimc-is-v6_20_0/fimc-is-subdev-sensor.o \
	ischain/fimc-is-v6_20_0/fimc-is-subdev-sensor_vc0.o \
	ischain/fimc-is-v6_20_0/fimc-is-subdev-sensor_vc1.o \
	ischain/fimc-is-v6_20_0/fimc-is-subdev-sensor_vc2.o \
	ischain/fimc-is-v6_20_0/fimc-is-subdev-sensor_vc3.o \
	ischain/fimc-is-v6_20_0/fimc-is-subdev-vra.o \
	ischain/fimc-is-v6_20_0/setup-fimc-is-sensor.o \
	ischain/fimc-is-v6_20_0/setup-fimc-is.o \
	ischain/setup-fimc-is-module.o

fimc-is-video-y += \
	fimc-is-video-3aa.o \
	fimc-is-video-3aac.o \
	fimc-is-video-3aap.o \
	fimc-is-video-dis.o \
	fimc-is-video-disc.o \
	fimc-is-video-isp.o \
	fimc-is-video-ispc.o \
	fimc-is-video-ispp.o \
	fimc-is-video-mcs.o \
	fimc-is-video-mcsp.o \
	fimc-is-video-sensor.o \
	fimc-is-video-ssvc0.o \
	fimc-is-video-ssvc1.o \
	fimc-is-video-ssvc2.o \
	fimc-is-video-ssvc3.o \
	fimc-is-video-vra.o \
	fimc-is-video.o

fimc-is-objs := $(fimc-is-sensor-y) $(fimc-is-core-y) $(fimc-is-chain-y) $(fimc-is-video-y)
obj-$(CONFIG_VIDEO_EXYNOS_FIMC_LEGACY) += fimc-is-vb2-ion.o
obj-$(CONFIG_VIDEO_EXYNOS_FIMC_LEGACY) += fimc-is.o

FIMC_LEGACY := drivers/media/platform/exynos/fimc-legacy
subdir-ccflags-y += -I$(srctree)/$(FIMC_LEGACY) -I$(srctree)/$(FIMC_LEGACY)/include
subdir-ccflags-y += -I$(srctree)/$(FIMC_LEGACY)/interface -I$(srctree)/$(FIMC_LEGACY)/hardware
subdir-ccflags-y += -I$(srctree)/$(FIMC_LEGACY)/hardware/api -I$(srctree)/$(FIMC_LEGACY)/vendor
subdir-ccflags-y += -I$(srctree)/$(FIMC_LEGACY)/sensor/csi2
subdir-ccflags-y += -I$(srctree)/$(FIMC_LEGACY)/sensor/module_framework
subdir-ccflags-y += -I$(srctree)/$(FIMC_LEGACY)/vendor/mcd -I$(srctree)/$(FIMC_LEGACY)/vendor/mcd/preprocessor
subdir-ccflags-y += -I$(srctree)/$(FIMC_LEGACY)/ischain/fimc-is-v6_20_0
ccflags-y += -include $(srctree)/$(FIMC_LEGACY)/include/fimc-is-compat.h $(subdir-ccflags-y) -Wno-unused-variable -Wno-missing-braces -Wno-enum-conversion -Wno-sizeof-array-div -Wno-pointer-bool-conversion -ferror-limit=0
