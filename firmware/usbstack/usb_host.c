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

/* Enumerating the device on the host probe's port, over whichever
 * controller's usb_drv_host_control(): the device descriptor at address 0,
 * SET_ADDRESS, then the device and configuration descriptors from the new
 * address, SET_CONFIGURATION, and the maker and product strings.
 *
 * A hub gets one more step between: its ports are powered, the first with a
 * device on it is reset, and that device is enumerated at address 2. A
 * full-speed device behind a high-speed hub is routed through the hub's
 * transaction translator; a low-speed one is refused. Nothing reads the
 * hub's interrupt endpoint; while a DAC behind it plays,
 * usb_host_hub_device_present() reads its port instead. */

#include <string.h>
#include "system.h"
#include "usb_ch9.h"
#include "usb_drv.h"
#include "usb_log.h"

/* The device on the player's own port, a hub or not */
#define ROOT_ADDR           1

#define USB_DT_HUB          0x29
#define HUB_PORT_RESET      4
#define HUB_PORT_POWER      8
#define HUB_C_PORT_CONNECT  16
#define HUB_C_PORT_RESET    20
/* wPortStatus, and wPortChange shifted up beside it */
#define PS_CONNECT          (1 << 0)
#define PS_ENABLE           (1 << 1)
#define PS_LOW_SPEED        (1 << 9)
#define PS_HIGH_SPEED       (1 << 10)
#define PS_C_CONNECT        (1 << 16)
#define PS_C_RESET          (1 << 20)

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

/* The device and its interface settings and endpoints, into the USB log.
 * A device enumerated twice in a row is logged once; one between them, such
 * as a hub, logs it again. */
static void log_descriptors(const struct usb_drv_host_enum *e)
{
    static uint32_t logged_id;
    static int logged_total = -1;
    const uint8_t *d = e->dev;
    uint32_t id = (uint32_t)(d[8] | d[9] << 8) << 16 | (d[10] | d[11] << 8);

    if (id == logged_id && e->cfg_total == logged_total)
        return;
    logged_id = id;
    logged_total = e->cfg_total;
    usb_log(USB_LOG_HOST_DEVICE, d[4], e->cfg_total, id,
            d[5] | d[6] << 8 | d[17] << 16 |
            (uint32_t)(e->cfg_len < e->cfg_total) << 24);
    for (int i = 0; i + 2 <= e->cfg_len && e->cfg[i] >= 2; i += e->cfg[i])
    {
        const uint8_t *c = &e->cfg[i];

        if (i + c[0] > e->cfg_len)
            break;
        if (c[1] == USB_DT_INTERFACE && c[0] >= 9)
            usb_log(USB_LOG_HOST_IFACE, c[2], c[3] | c[4] << 8,
                    c[5] | c[6] << 8 | c[7] << 16, 0);
        else if (c[1] == USB_DT_ENDPOINT && c[0] >= 7)
            usb_log(USB_LOG_HOST_EP, c[2], c[3], c[4] | c[5] << 8, c[6]);
    }
}


/* Logs one step; a failed one ends the enumeration there */
static bool step(const char *name, bool ok, uint32_t d)
{
    struct usb_drv_host_enum *e = &host_enum;

    e->step = name;
    usb_log(USB_LOG_HOST_ENUM, ok, 0, (uintptr_t)name, d);
    if (!ok)
    {
        e->token = usb_drv_host_last_status();
        e->result = -1;
    }
    return ok;
}

#define STEP(name, cond) \
    do { \
        if (!step(name, (cond), usb_drv_host_last_status())) \
            return false; \
    } while (0)

/* The device at address 0, reached by route r, given addr and
 * configured */
static bool enumerate_device(int addr, struct usb_drv_host_route r)
{
    struct usb_drv_host_enum *e = &host_enum;
    int n;

    r.mps = 64;
    usb_drv_host_set_route(0, &r);
    n = get_descriptor(0, USB_DT_DEVICE << 8, 0, e->dev, 18);
    STEP("device @0", n >= 8);
    STEP("ep0 size", e->dev[7] == 8 || e->dev[7] == 16 || e->dev[7] == 32 ||
                     e->dev[7] == 64);
    r.mps = e->dev[7];
    usb_drv_host_set_route(0, &r);
    usb_drv_host_set_route(addr, &r);
    n = usb_drv_host_control(0, USB_DIR_OUT, USB_REQ_SET_ADDRESS, addr, 0,
                             NULL, 0);
    STEP("set address", n == 0);
    sleep(HZ / 100);
    e->addr = addr;
    e->high_speed = r.high_speed;
    n = get_descriptor(addr, USB_DT_DEVICE << 8, 0, e->dev, 18);
    STEP("device", n == 18);
    n = get_descriptor(addr, USB_DT_CONFIG << 8, 0, host_cfg, 9);
    STEP("config head", n == 9);
    e->cfg_total = host_cfg[2] | (host_cfg[3] << 8);
    n = get_descriptor(addr, USB_DT_CONFIG << 8, 0, host_cfg,
                       MIN(e->cfg_total, (int)sizeof host_cfg));
    STEP("config", n > 0);
    e->cfg = host_cfg;
    e->cfg_len = n;
    log_descriptors(e);
    /* Interfaces can only be selected once the device is configured */
    n = usb_drv_host_control(addr, USB_DIR_OUT, USB_REQ_SET_CONFIGURATION,
                             host_cfg[5], 0, NULL, 0);
    STEP("set config", n == 0);
    return true;
}

static int hub_feature(int req, int feature, int port)
{
    return usb_drv_host_control(ROOT_ADDR, USB_DIR_OUT | USB_TYPE_CLASS |
                                USB_RECIP_OTHER, req, feature, port,
                                NULL, 0);
}

/* wPortStatus | wPortChange << 16, or 0 if it cannot be read */
static uint32_t hub_port_status(int port, int why)
{
    uint8_t b[4];
    uint32_t ps = 0;

    if (usb_drv_host_control(ROOT_ADDR, USB_DIR_IN | USB_TYPE_CLASS |
                             USB_RECIP_OTHER, USB_REQ_GET_STATUS, 0, port,
                             b, 4) == 4)
        ps = b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
    usb_log(USB_LOG_HOST_HUB, port, why, ps, 0);
    return ps;
}

/* The hub, configured at ROOT_ADDR: power its ports, wait up to a second
 * for a device on one, reset that port, and enumerate the device. */
static bool enumerate_hub(void)
{
    struct usb_drv_host_enum *e = &host_enum;
    struct usb_drv_host_route r = { 0 };
    uint32_t ps = 0;
    int n, port = 0;

    e->hub_vid = e->dev[8] | (e->dev[9] << 8);
    e->hub_pid = e->dev[10] | (e->dev[11] << 8);
    e->hub_tt = e->dev[6];
    n = usb_drv_host_control(ROOT_ADDR, USB_DIR_IN | USB_TYPE_CLASS |
                             USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR,
                             USB_DT_HUB << 8, 0, buf, 9);
    STEP("hub descriptor", n >= 7 && buf[2] > 0);
    e->hub_ports = buf[2];
    for (int p = 1; p <= e->hub_ports; p++)
        STEP("hub port power",
             hub_feature(USB_REQ_SET_FEATURE, HUB_PORT_POWER, p) == 0);
    /* bPwrOn2PwrGood counts 2 ms */
    sleep(HZ * (buf[5] * 2 + 20) / 1000 + 1);

    for (int t = 0; t < 20 && !port; t++)
    {
        for (int p = 1; p <= e->hub_ports && !port; p++)
        {
            ps = hub_port_status(p, 0);
            if (ps & PS_CONNECT)
                port = p;
        }
        if (!port)
            sleep(HZ / 20);
    }
    if (!step("hub: device on a port", port != 0, e->hub_ports))
        return false;
    e->hub_port = port;

    /* 100 ms connect debounce, then the hub times the reset itself */
    sleep(HZ / 10);
    STEP("hub port reset",
         hub_feature(USB_REQ_SET_FEATURE, HUB_PORT_RESET, port) == 0);
    for (int t = 0; t < 50; t++)
    {
        sleep(HZ / 100);
        ps = hub_port_status(port, 1);
        if (ps & PS_C_RESET)
            break;
    }
    if (!step("hub port enabled", (ps & PS_C_RESET) && (ps & PS_ENABLE),
              ps))
        return false;
    hub_feature(USB_REQ_CLEAR_FEATURE, HUB_C_PORT_RESET, port);
    hub_feature(USB_REQ_CLEAR_FEATURE, HUB_C_PORT_CONNECT, port);
    sleep(HZ / 100);                /* reset recovery */

    if (!step("hub: full or high speed", !(ps & PS_LOW_SPEED), ps))
        return false;
    r.high_speed = ps & PS_HIGH_SPEED;
    if (!r.high_speed && usb_drv_host_high_speed())
    {
        r.tt_hub = ROOT_ADDR;
        r.tt_port = port;
    }
    return enumerate_device(ROOT_ADDR + 1, r);
}
#undef STEP

void usb_host_enumerate(void)
{
    struct usb_drv_host_enum *e = &host_enum;
    struct usb_drv_host_route root = {
        .high_speed = usb_drv_host_high_speed(),
    };
    int n, langid;

    if (!enumerate_device(ROOT_ADDR, root))
        return;
    if (e->dev[4] == USB_CLASS_HUB && !enumerate_hub())
        return;

    n = get_descriptor(e->addr, USB_DT_STRING << 8, 0, buf, 4);
    langid = n >= 4 ? buf[2] | (buf[3] << 8) : 0x0409;
    get_string(e->addr, e->dev[14], langid, e->manufacturer,
               sizeof e->manufacturer);
    get_string(e->addr, e->dev[15], langid, e->product,
               sizeof e->product);
    e->result = 1;
}

bool usb_host_hub_device_present(void)
{
    const struct usb_drv_host_enum *e = &host_enum;
    uint32_t ps;

    if (e->result != 1 || e->hub_ports == 0)
        return true;
    /* A status that cannot be read is the hub itself gone, which the root
     * port's disconnect reports. A device that left and came back between
     * two reads is connected again but on a disabled port at address 0,
     * with the connect change that enumeration cleared set once more. */
    ps = hub_port_status(e->hub_port, 2);
    return ps == 0 || ((ps & PS_CONNECT) && (ps & PS_ENABLE) &&
                       !(ps & PS_C_CONNECT));
}
