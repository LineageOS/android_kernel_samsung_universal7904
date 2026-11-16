// SPDX-License-Identifier: GPL-2.0
/*
 * USB enumeration state used by sec_battery (slow charging detection).
 *
 * 4.4 keeps this in the Samsung-patched drivers/usb/gadget/configfs.c
 * (lines 52-93, the !CONFIG_USB_TYPEC_MANAGER_NOTIFIER variant). The USB
 * gadget stack is not ported yet, so the same state and accessors live here;
 * the gadget code must call set_usb_enumeration_state(0x310 | 0x210) on
 * connect and set_usb_enumeration_state(0) on disconnect (4.4 configfs.c:1756-1758,1781).
 * Until then it stays 0, which is also what 4.4 reports on a never-enumerated bus.
 */
#include <linux/export.h>
#include <linux/types.h>

static int usb_enum_state;
static int usb210_count;
static int usb310_count;

void set_usb_enumeration_state(int state)
{
	if (usb_enum_state != state) {
		usb_enum_state = state;
		if (usb_enum_state == 0x310)
			usb310_count++;
		else if (usb_enum_state == 0x210)
			usb210_count++;
	}
}
EXPORT_SYMBOL(set_usb_enumeration_state);

int microusb_get_usb210_count(void)
{
	int ret = usb210_count;

	usb210_count = 0;
	return ret;
}
EXPORT_SYMBOL(microusb_get_usb210_count);

int microusb_get_usb310_count(void)
{
	int ret = usb310_count;

	usb310_count = 0;
	return ret;
}
EXPORT_SYMBOL(microusb_get_usb310_count);

bool get_usb_enumeration_state(void)
{
	return usb_enum_state ? 1 : 0;
}
EXPORT_SYMBOL(get_usb_enumeration_state);
