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

/* Playback to a USB Audio Class 2 DAC that the host probe has enumerated at
 * address 1, as a PCM sink. Starting reads the class descriptors, chooses a
 * 16-bit PCM streaming setting, selects it and starts an isochronous stream,
 * then switches the mixer to this sink. The stream's fill runs from the tick
 * interrupt: it copies the mixer's buffers out, scaled by the volume setting,
 * asks the PCM core for the next buffer when one runs dry, and sends silence
 * while nothing plays. The DAC's clock follows the sink's sample rate.
 *
 * Volume is the player's own, applied to the samples and capped at 0 dB;
 * the DAC's own volume is left where it is. Balance and the tone controls,
 * which the headphone codec does in hardware, do not apply here. */

#include <string.h>
#include "system.h"
#include "fixedpoint.h"
#include "kernel.h"
#include "pcm-internal.h"
#include "pcm_mixer.h"
#include "pcm_sampr.h"
#include "pcm_sink.h"
#include "sound.h"
#include "usb.h"
#include "usb_ch9.h"
#include "usb_drv.h"
#include "usb_host_audio.h"

#define DEV_ADDR            1

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

static struct usb_host_audio_status status = { .state = "off" };

/* Descending, as pcm_set_frequency() rounds against it */
static const unsigned long samprs[] = { SAMPR_48, SAMPR_44 };
#define DEFAULT_FREQ 1

static const int16_t *src;          /* the mixer buffer being sent */
static size_t src_frames;           /* stereo frames left in it */
static bool playing;
static int locked;
static int32_t gain;                /* 16.16 */

static uint32_t nominal(unsigned long rate)
{
    return ((uint64_t)rate << 16) / 8000;
}

/* SET_CUR on the clock source, then read it back for the screen */
static bool set_dac_rate(unsigned long rate)
{
    int index = (status.clock << 8) | status.ac_iface;
    uint8_t b[4] = { rate, rate >> 8, rate >> 16, rate >> 24 };

    if (status.clock < 0)
        return true;
    if (usb_drv_host_control(DEV_ADDR, USB_DIR_OUT | USB_TYPE_CLASS |
                             USB_RECIP_INTERFACE, UAC2_CUR, UAC2_SAM_FREQ,
                             index, b, 4) != 4)
        return false;
    status.rate_set = rate;
    status.rate_read = 0;
    if (usb_drv_host_control(DEV_ADDR, USB_DIR_IN | USB_TYPE_CLASS |
                             USB_RECIP_INTERFACE, UAC2_CUR, UAC2_SAM_FREQ,
                             index, b, 4) == 4)
        status.rate_read = b[0] | (b[1] << 8) | (b[2] << 16) |
                           ((uint32_t)b[3] << 24);
    return true;
}

/* A volume setting value in centibels */
static int volume_cb(int value)
{
    int shift = 1 - sound_numdecimals(SOUND_VOLUME);

    for (; shift < 0; shift++)
        value /= 10;
    for (; shift > 0; shift--)
        value *= 10;
    return value;
}

/* Tick interrupt: skipped while the PCM core holds the lock, and the one
 * place the volume is read. */
static bool sink_begin(void)
{
    int cb, min_cb;

    if (locked)
        return false;
    cb = volume_cb(sound_current(SOUND_VOLUME));
    if (cb != status.gain_cb || gain == 0)
    {
        min_cb = volume_cb(sound_min(SOUND_VOLUME));
        status.gain_cb = cb;
        gain = cb <= min_cb ? 0 :
               cb >= 0 ? 1 << 16 : fp_factor(((long)cb << 16) / 10, 16);
    }
    return true;
}

/* Tick interrupt: stereo 16-bit from the mixer into the DAC's subslots,
 * sample at the top of each little-endian subslot. */
static void sink_fill(uint8_t *dst, int frames)
{
    while (frames > 0)
    {
        if (src_frames == 0)
        {
            const void *addr;
            size_t size;

            if (!playing ||
                !pcm_play_dma_complete_callback(PCM_DMAST_OK, &addr, &size))
            {
                playing = false;
                memset(dst, 0, frames * status.channels * status.subslot);
                return;
            }
            pcm_play_dma_status_callback(PCM_DMAST_STARTED);
            src = addr;
            src_frames = size / 4;
            continue;
        }

        for (int ch = 0; ch < status.channels; ch++)
        {
            uint32_t v = ch < 2 ? (uint32_t)((src[ch] * gain) >> 16) << 16
                                : 0;
            for (int b = 4 - status.subslot; b < 4; b++)
                *dst++ = v >> (8 * b);
        }
        src += 2;
        src_frames--;
        frames--;
    }
}

static void sink_set_freq(uint16_t freq)
{
    if (set_dac_rate(samprs[freq]))
        usb_drv_host_iso_set_nominal(nominal(samprs[freq]));
}

static void sink_lock(void)
{
    int oldlevel = disable_irq_save();
    locked++;
    restore_irq(oldlevel);
}

static void sink_unlock(void)
{
    int oldlevel = disable_irq_save();
    locked--;
    restore_irq(oldlevel);
}

static void sink_play(const void *addr, size_t size)
{
    int oldlevel = disable_irq_save();
    src = addr;
    src_frames = size / 4;
    playing = true;
    restore_irq(oldlevel);
}

static void sink_stop(void)
{
    int oldlevel = disable_irq_save();
    playing = false;
    src_frames = 0;
    restore_irq(oldlevel);
}

/* Tick interrupt, the DAC unplugged: the USB thread shuts the probe down,
 * which stops this sink and returns playback to the headphone socket. */
static void sink_lost(void)
{
    usb_set_host_probe(false);
}

static void sink_nop(void)
{
}

struct pcm_sink usb_host_pcm_sink = {
    .caps = {
        .samprs       = samprs,
        .num_samprs   = ARRAYLEN(samprs),
        .default_freq = DEFAULT_FREQ,
        .volume_type  = PCM_SINK_HWVOL,
    },
    .ops = {
        .init     = sink_nop,
        .postinit = sink_nop,
        .set_freq = sink_set_freq,
        .lock     = sink_lock,
        .unlock   = sink_unlock,
        .play     = sink_play,
        .stop     = sink_stop,
    },
};

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
    const struct usb_drv_host_enum *e = usb_host_get_enum();
    struct as_setting as;
    struct usb_drv_host_iso iso;

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

    STEP("set rate", set_dac_rate(samprs[DEFAULT_FREQ]));

    memset(&iso, 0, sizeof iso);
    iso.addr = DEV_ADDR;
    iso.ep_out = as.ep_out & 0xf;
    iso.mps_out = as.mps_out;
    iso.interval_out = as.interval_out;
    iso.ep_fb = as.ep_fb;
    iso.mps_fb = as.mps_fb;
    iso.interval_fb = as.interval_fb;
    iso.frame_bytes = as.channels * as.subslot;
    iso.nominal = nominal(samprs[DEFAULT_FREQ]);
    iso.begin = sink_begin;
    iso.fill = sink_fill;
    iso.lost = sink_lost;
    locked = 0;
    gain = 0;
    sink_stop();
    STEP("start stream", usb_drv_host_iso_start(&iso));
    STEP("switch sink", mixer_switch_sink(PCM_SINK_USB_HOST));
#undef STEP

    status.state = "on";
    queue_broadcast(SYS_USB_DAC_ON, 0);
    return true;
}

/* Playback goes back to the headphone socket first, so the mixer never
 * feeds a stream that has stopped. The interface is only released while
 * the DAC is still there to answer. */
void usb_host_audio_stop(void)
{
    struct usb_drv_host_iso_stats st;
    bool was_on = !strcmp(status.state, "on");

    if (pcm_current_sink() == PCM_SINK_USB_HOST)
        mixer_switch_sink(PCM_SINK_BUILTIN);
    usb_drv_host_iso_get_stats(&st);
    if (st.running)
    {
        usb_drv_host_iso_stop();
        if (!st.lost)
            usb_drv_host_control(DEV_ADDR, USB_DIR_OUT | USB_RECIP_INTERFACE,
                                 USB_REQ_SET_INTERFACE, 0, status.iface,
                                 NULL, 0);
    }
    status.state = "off";
    if (was_on)
        queue_broadcast(SYS_USB_DAC_OFF, 0);
}

const struct usb_host_audio_status *usb_host_audio_get_status(void)
{
    return &status;
}
