/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2025 by Sho Tanimoto
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
#pragma once
#include "usb_class_driver.h"

/* [2] P.32 Table 2-8 USB Device Vendor Request to set available current from accessory (USB Device Mode only) */
#define USB_REQ_APPLE_SET_AVAIL_CURRENT (0x40)
/* Sent by a Mazda CX-30 before it selects the iAP configuration, a 4-byte
 * read; nothing public says what it asks. */
#define USB_REQ_APPLE_0x53 (0x53)

extern struct usb_class_driver_ep_allocation usb_iap_ep_allocs[2];

extern struct usb_class_driver usb_cdrv_iap;

/* iAP2, by the setting: Off, the probe is refused, as an iPod refuses it.
 * On, it is answered, and the disk is not offered. Auto, it is answered
 * once the host has sent Apple's vendor request 0x53, as a car looking for
 * an iPhone does, and the disk handover waits a second after the host
 * picks a configuration that needs it: a car moves on to the iAP one within
 * that, and is never handed the disk; a computer stays, and is. With iAP2
 * not Off, the iAP configuration is an iPhone's: its nine rates and its
 * name. */
enum
{
    USB_IAP2_MODE_OFF,
    USB_IAP2_MODE_AUTO,
    USB_IAP2_MODE_ON,
};
void usb_iap_set_iap2_mode(int mode);
bool usb_iap2_offered(void);
/* The host has sent 0x53; and a new host, which has not. */
void usb_iap2_host_is_car(void);
void usb_iap2_host_new(void);

/* Whether the probe is answered now, and as what. Debug > Answer iAP2 probe
 * forces it until restart, an iPhone also enumerating with an iPhone's
 * product ID and Apple's manufacturer and product strings. */
enum
{
    USB_IAP2_OFF,
    USB_IAP2_IPOD,
    USB_IAP2_IPHONE,
};
#define USB_PRODUCT_ID_IPHONE 0x12A8
void usb_iap_set_answer_iap2(int as);
int usb_iap_answer_iap2(void);
