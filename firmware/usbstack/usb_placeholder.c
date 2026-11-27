/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
/* The interface an otherwise empty configuration is given. Answering iAP2
 * leaves the disk out of configuration 1, and Windows stops with
 * WDF_VIOLATION on selecting a configuration with no interfaces. No driver
 * claims this one. */
#include "string.h"
#include "system.h"
#include "usb_ch9.h"
#include "usb_class_driver.h"
#include "usb_placeholder.h"

static struct usb_interface_descriptor __attribute__((aligned(2)))
                                interface_descriptor =
{
    .bLength            = sizeof(struct usb_interface_descriptor),
    .bDescriptorType    = USB_DT_INTERFACE,
    .bInterfaceNumber   = 0,
    .bAlternateSetting  = 0,
    .bNumEndpoints      = 0,
    /* Not vendor-specific: Linux passes over a configuration whose first
     * interface is, and would choose the iAP one */
    .bInterfaceClass    = USB_CLASS_APP_SPEC,
    .bInterfaceSubClass = 0,
    .bInterfaceProtocol = 0,
    .iInterface         = 0
};

int usb_placeholder_descriptor(unsigned char *dest)
{
    unsigned char *orig_dest = dest;

    PACK_DATA(&dest, interface_descriptor);
    return dest - orig_dest;
}
