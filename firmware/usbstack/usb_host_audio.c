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

/* A 1 kHz test tone, 48 kHz, to a USB Audio Class 2 DAC that the host probe
 * has enumerated at address 1: read the class descriptors, choose a 16-bit
 * PCM streaming setting, select it, set the clock, and hand the driver an
 * isochronous stream whose fill is the tone. */

#include <string.h>
#include "system.h"
#include "usb_ch9.h"
#include "usb_drv.h"
#include "usb_host_audio.h"

#define DEV_ADDR            1
#define RATE                48000

#define UAC_CS_INTERFACE    0x24
#define UAC2_CLOCK_SOURCE   0x0a
#define UAC2_AS_GENERAL     0x01
#define UAC2_FORMAT_TYPE    0x02
#define UAC2_CUR            0x01
#define UAC2_SAM_FREQ       (0x01 << 8)

struct as_setting {
    int iface, alt;
    int format_type;
    uint32_t formats;
    int channels, subslot, bits;
    int ep_out, mps_out, interval_out;
    int ep_fb, mps_fb, interval_fb;
};

static struct usb_host_audio_status status = { .state = "idle" };

/* -20 dBFS, one cycle of 1 kHz at 48 kHz */
static const int16_t sine[48] = {
    0, 428, 848, 1254, 1638, 1995, 2317, 2600,
    2838, 3028, 3165, 3249, 3277, 3249, 3165, 3028,
    2838, 2600, 2317, 1995, 1638, 1254, 848, 428,
    0, -428, -848, -1254, -1638, -1995, -2317, -2600,
    -2838, -3028, -3165, -3249, -3277, -3249, -3165, -3028,
    -2838, -2600, -2317, -1995, -1638, -1254, -848, -428,
};
static unsigned int phase;

/* Samples sit at the top of each little-endian subslot. */
static void tone_fill(uint8_t *dst, int frames)
{
    while (frames--)
    {
        uint32_t v = (uint32_t)sine[phase] << 16;
        for (int ch = 0; ch < status.channels; ch++)
            for (int b = 4 - status.subslot; b < 4; b++)
                *dst++ = v >> (8 * b);
        if (++phase == 48)
            phase = 0;
    }
}

/* HS bInterval is 2^(n-1) microframes. */
static int hs_interval(int b)
{
    return (b >= 1 && b <= 16) ? 1 << (b - 1) : 0;
}

static bool usable(const struct as_setting *s)
{
    return s->ep_out && s->format_type == 1 && (s->formats & 1) &&
           s->channels > 0 && s->subslot >= 2 && s->subslot <= 4 &&
           s->interval_out >= 1 && s->interval_out <= 8;
}

/* Walk the configuration for the AC interface's clock source and the first
 * usable PCM streaming setting, a 16-bit one if there is one. */
static bool find_setting(const struct usb_drv_host_enum *e,
                         struct as_setting *best)
{
    struct as_setting cur;
    bool in_ac = false, in_as = false, found = false;

    memset(&cur, 0, sizeof cur);
    memset(best, 0, sizeof *best);
    status.clock = -1;
    for (int i = 0; i + 2 <= e->cfg_len && e->cfg[i] >= 2; i += e->cfg[i])
    {
        const uint8_t *c = &e->cfg[i];
        int len = c[0];

        if (i + len > e->cfg_len)
            break;
        if (c[1] == USB_DT_INTERFACE && len >= 9)
        {
            if (in_as && usable(&cur) &&
                (!found || (best->subslot != 2 && cur.subslot == 2)))
            {
                *best = cur;
                found = true;
            }
            memset(&cur, 0, sizeof cur);
            cur.iface = c[2];
            cur.alt = c[3];
            in_ac = c[5] == 1 && c[6] == 1;
            in_as = c[5] == 1 && c[6] == 2 && c[7] == 0x20;
            if (in_ac)
                status.ac_iface = c[2];
        }
        else if (c[1] == UAC_CS_INTERFACE && len >= 3)
        {
            if (in_ac && c[2] == UAC2_CLOCK_SOURCE && len >= 4 &&
                status.clock < 0)
                status.clock = c[3];
            else if (in_as && c[2] == UAC2_AS_GENERAL && len >= 11)
            {
                cur.format_type = c[5];
                cur.formats = c[6] | (c[7] << 8) | (c[8] << 16) |
                              ((uint32_t)c[9] << 24);
                cur.channels = c[10];
            }
            else if (in_as && c[2] == UAC2_FORMAT_TYPE && len >= 6)
            {
                cur.subslot = c[4];
                cur.bits = c[5];
            }
        }
        else if (c[1] == USB_DT_ENDPOINT && len >= 7 && in_as &&
                 (c[3] & 3) == USB_ENDPOINT_XFER_ISOC)
        {
            int mps = (c[4] | (c[5] << 8)) & 0x7ff;

            if ((c[2] & USB_DIR_IN) && ((c[3] >> 4) & 3) == 1)
            {
                cur.ep_fb = c[2];
                cur.mps_fb = mps;
                cur.interval_fb = hs_interval(c[6]);
            }
            else if (!(c[2] & USB_DIR_IN))
            {
                cur.ep_out = c[2];
                cur.mps_out = mps;
                cur.interval_out = hs_interval(c[6]);
            }
        }
    }
    if (in_as && usable(&cur) &&
        (!found || (best->subslot != 2 && cur.subslot == 2)))
    {
        *best = cur;
        found = true;
    }
    return found;
}

bool usb_host_audio_start(void)
{
    const struct usb_drv_host_enum *e = usb_drv_host_get_enum();
    struct as_setting as;
    struct usb_drv_host_iso iso;
    uint8_t rate[4];

#define STEP(name, cond) \
    do { if (!(cond)) { status.state = name; return false; } } while (0)

    usb_host_audio_stop();
    STEP("no device", e->result == 1);
    STEP("no UAC2 PCM setting", find_setting(e, &as));
    status.iface = as.iface;
    status.alt = as.alt;
    status.channels = as.channels;
    status.subslot = as.subslot;
    status.bits = as.bits;

    STEP("set interface",
         usb_drv_host_control(DEV_ADDR, USB_DIR_OUT | USB_RECIP_INTERFACE,
                              USB_REQ_SET_INTERFACE, as.alt, as.iface,
                              NULL, 0) == 0);

    if (status.clock >= 0)
    {
        int index = (status.clock << 8) | status.ac_iface;

        rate[0] = RATE & 0xff;
        rate[1] = (RATE >> 8) & 0xff;
        rate[2] = (RATE >> 16) & 0xff;
        rate[3] = 0;
        STEP("set rate",
             usb_drv_host_control(DEV_ADDR, USB_DIR_OUT | USB_TYPE_CLASS |
                                  USB_RECIP_INTERFACE, UAC2_CUR,
                                  UAC2_SAM_FREQ, index, rate, 4) == 4);
        status.rate_set = RATE;
        if (usb_drv_host_control(DEV_ADDR, USB_DIR_IN | USB_TYPE_CLASS |
                                 USB_RECIP_INTERFACE, UAC2_CUR,
                                 UAC2_SAM_FREQ, index, rate, 4) == 4)
            status.rate_read = rate[0] | (rate[1] << 8) | (rate[2] << 16) |
                               ((uint32_t)rate[3] << 24);
    }

    memset(&iso, 0, sizeof iso);
    iso.addr = DEV_ADDR;
    iso.ep_out = as.ep_out & 0xf;
    iso.mps_out = as.mps_out;
    iso.interval_out = as.interval_out;
    iso.ep_fb = as.ep_fb;
    iso.mps_fb = as.mps_fb;
    iso.interval_fb = as.interval_fb;
    iso.frame_bytes = as.channels * as.subslot;
    iso.nominal = ((uint64_t)RATE << 16) / 8000;
    iso.fill = tone_fill;
    phase = 0;
    STEP("start stream", usb_drv_host_iso_start(&iso));
#undef STEP

    status.state = "playing";
    return true;
}

void usb_host_audio_stop(void)
{
    struct usb_drv_host_iso_stats st;

    usb_drv_host_iso_get_stats(&st);
    if (!st.running)
        return;
    usb_drv_host_iso_stop();
    usb_drv_host_control(DEV_ADDR, USB_DIR_OUT | USB_RECIP_INTERFACE,
                         USB_REQ_SET_INTERFACE, 0, status.iface, NULL, 0);
    status.state = "idle";
}

const struct usb_host_audio_status *usb_host_audio_get_status(void)
{
    return &status;
}
