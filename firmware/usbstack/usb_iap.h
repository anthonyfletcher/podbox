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

/* Whether an accessory's iAP2 probe is answered, and as what. Off at every
 * start: an accessory that probes before falling back to iAP1 would
 * otherwise be taken into iAP2, which the player cannot yet finish. As an
 * iPhone, the player also enumerates with an iPhone's product ID and
 * Apple's manufacturer and product strings. */
enum
{
    USB_IAP2_OFF,
    USB_IAP2_IPOD,
    USB_IAP2_IPHONE,
};
#define USB_PRODUCT_ID_IPHONE 0x12A8
void usb_iap_set_answer_iap2(int as);
int usb_iap_answer_iap2(void);
