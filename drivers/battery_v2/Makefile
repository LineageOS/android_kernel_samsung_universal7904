obj-$(CONFIG_BATTERY_SAMSUNG) += sec_battery.o sec_adc.o sec_battery_ttf.o sec_batt_boot.o
obj-$(CONFIG_BATTERY_CISD) += sec_cisd.o
obj-$(CONFIG_BATTERY_NOTIFIER) += battery_notifier.o
obj-$(CONFIG_CHARGER_S2MU004) += s2mu004_charger.o
obj-$(CONFIG_FUELGAUGE_S2MU004) += s2mu004_fuelgauge.o
ifndef CONFIG_USB_TYPEC_MANAGER_NOTIFIER
obj-$(CONFIG_BATTERY_SAMSUNG) += sec_usb_enum.o
endif
