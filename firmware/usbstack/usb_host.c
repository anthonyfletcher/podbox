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

/* Enumerating the one device on the host probe's port, over whichever
 * controller's usb_drv_host_control(): the device descriptor at address 0,
 * SET_ADDRESS 1, then the device and configuration descriptors from the new
 * address, SET_CONFIGURATION, and the maker and product strings. */

#include <string.h>
#include "system.h"
#include "usb_ch9.h"
#include "usb_drv.h"
#include "usb_log.h"

static struct usb_drv_host_enum host_enum;
static uint8_t host_cfg[1024];
static uint8_t buf[64];

const struct usb_drv_host_enum *usb_host_get_enum(void)
{
    return &host_enum;
}

void usb_host_enum_clear(void)
{
    memset(&host_enum, 0, sizeof host_enum);
}

static int get_descriptor(int addr, int value, int index, void *data,
                          int len)
{
    return usb_drv_host_control(addr, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                                value, index, data, len);
}

static void get_string(int addr, int index, int langid, char *out,
                       size_t size)
{
    int n;
    size_t j = 0;

    if (index == 0)
        return;
    n = get_descriptor(addr, (USB_DT_STRING << 8) | index, langid, buf,
                       sizeof buf);
    /* UTF-16LE after a two-byte header; anything outside ASCII is '?' */
    for (int i = 2; i + 1 < n && j + 1 < size; i += 2)
        out[j++] = buf[i + 1] || buf[i] >= 0x80 ? '?' : buf[i];
    out[j] = '\0';
}

void usb_host_enumerate(void)
{
    struct usb_drv_host_enum *e = &host_enum;
    int n, langid;

#define STEP(name, cond) \
    do { \
        bool ok = (cond); \
        e->step = name; \
        usb_log(USB_LOG_HOST_ENUM, ok, 0, (uintptr_t)name, \
                usb_drv_host_last_status()); \
        if (!ok) { \
            e->token = usb_drv_host_last_status(); \
            e->result = -1; \
            return; \
        } \
    } while (0)

    usb_drv_host_set_ep0_mps(64);
    n = get_descriptor(0, USB_DT_DEVICE << 8, 0, e->dev, 18);
    STEP("device @0", n >= 8);
    STEP("ep0 size", e->dev[7] == 8 || e->dev[7] == 16 || e->dev[7] == 32 ||
                     e->dev[7] == 64);
    usb_drv_host_set_ep0_mps(e->dev[7]);
    n = usb_drv_host_control(0, USB_DIR_OUT, USB_REQ_SET_ADDRESS, 1, 0,
                             NULL, 0);
    STEP("set address", n == 0);
    sleep(HZ / 100);
    n = get_descriptor(1, USB_DT_DEVICE << 8, 0, e->dev, 18);
    STEP("device @1", n == 18);
    n = get_descriptor(1, USB_DT_CONFIG << 8, 0, host_cfg, 9);
    STEP("config head", n == 9);
    e->cfg_total = host_cfg[2] | (host_cfg[3] << 8);
    n = get_descriptor(1, USB_DT_CONFIG << 8, 0, host_cfg,
                       MIN(e->cfg_total, (int)sizeof host_cfg));
    STEP("config", n > 0);
    e->cfg = host_cfg;
    e->cfg_len = n;
    /* Interfaces can only be selected once the device is configured */
    n = usb_drv_host_control(1, USB_DIR_OUT, USB_REQ_SET_CONFIGURATION,
                             host_cfg[5], 0, NULL, 0);
    STEP("set config", n == 0);
#undef STEP

    n = get_descriptor(1, USB_DT_STRING << 8, 0, buf, 4);
    langid = n >= 4 ? buf[2] | (buf[3] << 8) : 0x0409;
    get_string(1, e->dev[14], langid, e->manufacturer,
               sizeof e->manufacturer);
    get_string(1, e->dev[15], langid, e->product, sizeof e->product);
    e->result = 1;
}
