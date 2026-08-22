/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * CheckWPS shadow of firmware/export/usb.h.
 *
 * usb.h declares usb_set_hid() only under USB_ENABLE_HID, which a __PCTOOL__
 * build does not reach; settings_list.c names it as a setting callback
 * regardless.
 *
 * See ../README.
 ****************************************************************************/
#ifndef PODBOX_CHECKWPS_USB_H
#define PODBOX_CHECKWPS_USB_H

#include_next "usb.h"

#include <stdbool.h>
void usb_set_hid(bool enable);

#endif /* PODBOX_CHECKWPS_USB_H */
