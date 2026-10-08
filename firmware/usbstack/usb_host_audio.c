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

/* Playback to a USB Audio Class 1 or 2 DAC that the host probe has
 * enumerated at address 1, as a PCM sink. Starting reads the class
 * descriptors, chooses a 16-bit PCM streaming setting, selects it and starts
 * an isochronous stream, then switches the mixer to this sink. Class 2 sets
 * its rate on a clock source, class 1 on the endpoint; a class 1 DAC is
 * offered only the rates it lists. The stream's fill runs from the
 * stream's interrupt: it copies the mixer's buffers out, scaled by the
 * volume setting, asks the PCM core for the next buffer when one runs dry,
 * and sends silence while nothing plays. The DAC's clock follows the sink's
 * sample rate.
 *
 * Volume is the player's own, applied to the samples and capped at 0 dB;
 * the DAC's own is unmuted and turned to its maximum. Balance and the tone
 * controls, which the headphone codec does in hardware, do not apply
 * here. */

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
#include "usb_log.h"

#define DEV_ADDR            1

#define UAC_CS_INTERFACE    0x24
#define UAC_CS_ENDPOINT     0x25
#define UAC2_CLOCK_SOURCE   0x0a
#define UAC2_AS_GENERAL     0x01
#define UAC2_FORMAT_TYPE    0x02
#define UAC2_CUR            0x01
#define UAC2_SAM_FREQ       (0x01 << 8)
#define UAC1_EP_GENERAL     0x01
#define UAC1_GET_CUR        0x81
#define UAC1_GET_MAX        0x83
#define UAC2_RANGE          0x02
#define UAC_FEATURE_UNIT    0x06
#define UAC1_FU_MUTE        0x01
#define UAC1_FU_VOLUME      0x02

/* Rates a setting can run at */
#define RATE_48             (1 << 0)
#define RATE_44             (1 << 1)

struct as_setting {
    int uac;
    int iface, alt;
    int format_type;
    uint32_t formats;
    int channels, subslot, bits;
    int rates;
    bool rate_ctl;              /* class 1: the endpoint takes SET_CUR */
    int ep_out, mps_out, interval_out;
    int ep_fb, mps_fb, interval_fb;
};

static struct usb_host_audio_status status = { .state = "off" };

/* Descending, as pcm_set_frequency() rounds against it */
static const unsigned long samprs_both[] = { SAMPR_48, SAMPR_44 };
static const unsigned long samprs_48[] = { SAMPR_48 };
static const unsigned long samprs_44[] = { SAMPR_44 };
extern struct pcm_sink usb_host_pcm_sink;

/* The rate at index freq of those the sink offers */
static unsigned long sink_rate(int freq)
{
    return usb_host_pcm_sink.caps.samprs[freq];
}

static unsigned long sof_per_second = 8000;

/* The DAC's first feature unit, and the controls it lets the host set on
 * the master channel and the first two: bit 0 mute, bit 1 volume */
static int fu_unit;
static uint8_t fu_ctl[3];

static const int16_t *src;          /* the mixer buffer being sent */
static size_t src_frames;           /* stereo frames left in it */
static bool playing;
static int locked;
static int32_t gain;                /* 16.16 */

/* Samples per start-of-(micro)frame, 16.16 */
static uint32_t nominal(unsigned long rate)
{
    return ((uint64_t)rate << 16) / sof_per_second;
}

/* False when the DAC read back a rate other than the one just set */
static bool rate_taken(void)
{
    return status.rate_read == 0 || status.rate_read == status.rate_set;
}

/* Class 1: SET_CUR on the endpoint, then read it back */
static bool set_dac_rate_uac1(unsigned long rate)
{
    uint8_t b[3] = { rate, rate >> 8, rate >> 16 };

    if (status.ep_rate == 0)
        return true;
    if (usb_drv_host_control(DEV_ADDR, USB_DIR_OUT | USB_TYPE_CLASS |
                             USB_RECIP_ENDPOINT, UAC2_CUR, UAC2_SAM_FREQ,
                             status.ep_rate, b, 3) != 3)
        return false;
    status.rate_set = rate;
    status.rate_read = 0;
    if (usb_drv_host_control(DEV_ADDR, USB_DIR_IN | USB_TYPE_CLASS |
                             USB_RECIP_ENDPOINT, UAC1_GET_CUR, UAC2_SAM_FREQ,
                             status.ep_rate, b, 3) == 3)
        status.rate_read = b[0] | (b[1] << 8) | (b[2] << 16);
    return rate_taken();
}

/* Class 2: SET_CUR on the clock source, then read it back */
static bool set_dac_rate(unsigned long rate)
{
    int index = (status.clock << 8) | status.ac_iface;
    uint8_t b[4] = { rate, rate >> 8, rate >> 16, rate >> 24 };

    if (status.uac == 1)
        return set_dac_rate_uac1(rate);
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
    return rate_taken();
}

static uint32_t get_le32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Class 2: the rates the clock's RANGE lists, as wNumSubRanges then dMIN,
 * dMAX and dRES each. A clock that will not say is taken to have both. */
static int uac2_rates(void)
{
    static uint8_t b[2 + 12 * 16];
    static const unsigned long want[2] = { SAMPR_48, SAMPR_44 };
    int index = (status.clock << 8) | status.ac_iface;
    int got, n, i, rates = 0;

    if (status.clock < 0)
        return RATE_48 | RATE_44;
    got = usb_drv_host_control(DEV_ADDR, USB_DIR_IN | USB_TYPE_CLASS |
                               USB_RECIP_INTERFACE, UAC2_RANGE,
                               UAC2_SAM_FREQ, index, b, sizeof b);
    if (got < 2)
        return RATE_48 | RATE_44;
    n = b[0] | (b[1] << 8);
    for (i = 0; i < n && 2 + 12 * i + 12 <= got; i++)
    {
        const uint8_t *r = &b[2 + 12 * i];
        uint32_t lo = get_le32(r), hi = get_le32(r + 4);
        uint32_t res = get_le32(r + 8);

        for (int k = 0; k < 2; k++)
            if (lo <= want[k] && want[k] <= hi &&
                (res == 0 || (want[k] - lo) % res == 0))
                rates |= k == 0 ? RATE_48 : RATE_44;
    }
    return i ? rates : RATE_48 | RATE_44;
}

/* The sink's rates from a setting's, 44.1 kHz the default where there is a
 * choice */
static void offer_rates(int rates)
{
    if ((rates & (RATE_48 | RATE_44)) == (RATE_48 | RATE_44))
    {
        usb_host_pcm_sink.caps.samprs = samprs_both;
        usb_host_pcm_sink.caps.num_samprs = ARRAYLEN(samprs_both);
        usb_host_pcm_sink.caps.default_freq = 1;
    }
    else
    {
        usb_host_pcm_sink.caps.samprs =
            rates & RATE_44 ? samprs_44 : samprs_48;
        usb_host_pcm_sink.caps.num_samprs = 1;
        usb_host_pcm_sink.caps.default_freq = 0;
    }
}

/* A feature unit can start muted or low, and the player applies its own
 * volume: unmute it and turn it up to its maximum. Each control is set on
 * the master channel where the unit has it there, and on channels 1 and 2
 * where it does not -- mute on the master and volume per channel is
 * common. Class 1 reads the maximum with GET_MAX; class 2 reads a RANGE,
 * the count of subranges and then the first one's minimum, maximum and
 * resolution. */
static void dac_unmute(void)
{
    int index = (fu_unit << 8) | status.ac_iface;

    if (fu_unit == 0)
        return;
    for (int bit = 0; bit < 2; bit++)
    {
        for (int ch = 0; ch < 3; ch++)
        {
            uint8_t b[8] = { 0 };
            int n;

            if (!(fu_ctl[ch] & (1 << bit)))
                continue;
            if (bit == 0)
            {
                n = usb_drv_host_control(DEV_ADDR, USB_DIR_OUT |
                                         USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                                         UAC2_CUR, (UAC1_FU_MUTE << 8) | ch,
                                         index, b, 1);
                usb_log(USB_LOG_HOST_ENUM, n == 1, ch,
                        (uintptr_t)"dac unmute", usb_drv_host_last_status());
            }
            else if (status.uac == 1)
            {
                n = usb_drv_host_control(DEV_ADDR, USB_DIR_IN |
                                         USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                                         UAC1_GET_MAX,
                                         (UAC1_FU_VOLUME << 8) | ch,
                                         index, b, 2);
                if (n == 2)
                    n = usb_drv_host_control(DEV_ADDR, USB_DIR_OUT |
                                             USB_TYPE_CLASS |
                                             USB_RECIP_INTERFACE, UAC2_CUR,
                                             (UAC1_FU_VOLUME << 8) | ch,
                                             index, b, 2);
                usb_log(USB_LOG_HOST_ENUM, n == 2, ch,
                        (uintptr_t)"dac volume", b[0] | b[1] << 8);
            }
            else
            {
                n = usb_drv_host_control(DEV_ADDR, USB_DIR_IN |
                                         USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                                         UAC2_RANGE,
                                         (UAC1_FU_VOLUME << 8) | ch,
                                         index, b, 8);
                if (n == 8)
                    n = usb_drv_host_control(DEV_ADDR, USB_DIR_OUT |
                                             USB_TYPE_CLASS |
                                             USB_RECIP_INTERFACE, UAC2_CUR,
                                             (UAC1_FU_VOLUME << 8) | ch,
                                             index, &b[4], 2);
                usb_log(USB_LOG_HOST_ENUM, n == 2, ch,
                        (uintptr_t)"dac volume", b[4] | b[5] << 8);
            }
            if (ch == 0)
                break;
        }
    }
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

/* Stream interrupt: skipped while the PCM core holds the lock, and the one
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

/* Stream interrupt: stereo 16-bit from the mixer into the DAC's subslots,
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

static void sink_lost(void);

static bool set_interface(int alt)
{
    return usb_drv_host_control(DEV_ADDR, USB_DIR_OUT | USB_RECIP_INTERFACE,
                                USB_REQ_SET_INTERFACE, alt, status.iface,
                                NULL, 0) == 0;
}

/* Some DACs take a new rate only across an alternate setting change */
static bool set_dac_rate_idle(unsigned long rate)
{
    bool ok;

    if (!set_interface(0))
        return false;
    ok = set_dac_rate(rate);
    return set_interface(status.alt) && ok;
}

/* A rate the DAC refuses twice would have it play the mixer's samples at
 * its own rate, so playback goes back to the headphones as for an
 * unplugged DAC; the debug screen keeps the rate set and read. */
static void sink_set_freq(uint16_t freq)
{
    if (set_dac_rate(sink_rate(freq)) || set_dac_rate_idle(sink_rate(freq)))
        usb_drv_host_iso_set_nominal(nominal(sink_rate(freq)));
    else
        sink_lost();
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

/* The DAC unplugged (stream interrupt) or refusing a rate (thread): the USB
 * thread shuts the probe down, which stops this sink and returns playback to
 * the headphone socket. */
static void sink_lost(void)
{
    usb_set_host_probe(false);
}

static void sink_nop(void)
{
}

/* The rates offered are the DAC's, set by usb_host_audio_start() */
struct pcm_sink usb_host_pcm_sink = {
    .caps = {
        .samprs       = samprs_both,
        .num_samprs   = ARRAYLEN(samprs_both),
        .default_freq = 1,
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

/* An isochronous bInterval is 2^(n-1) microframes at high speed, frames
 * at full. */
static int hs_interval(int b)
{
    return (b >= 1 && b <= 16) ? 1 << (b - 1) : 0;
}

static bool usable(const struct as_setting *s)
{
    return s->ep_out && s->format_type == 1 && (s->formats & 1) &&
           s->channels > 0 && s->subslot >= 2 && s->subslot <= 4 &&
           s->interval_out >= 1 && s->interval_out <= 8 && s->rates;
}

/* Whether a 48 kHz packet at the top of the feedback window fits the
 * endpoint and the controller. One that does not is cut short every
 * packet, and the DAC plays slow. */
static bool fits(const struct as_setting *s)
{
    bool hs = usb_drv_host_high_speed();
    int sof = hs ? 8000 : 1000;
    int frames = (SAMPR_48 * 9 / 8 * (hs ? s->interval_out : 1) + sof - 1) /
                 sof;
    int limit = MIN(s->mps_out,
                    usb_drv_host_iso_max_packet(s->interval_out));

    return frames * s->channels * s->subslot <= limit;
}

/* Class 2: a 16-bit setting where there is one. Class 1: the widest
 * samples, the format a desktop host streams in and so the one a class 1
 * DAC's firmware is tested with -- a Fosi DAC-Q4's 16-bit setting plays
 * noise on its left channel. The player's samples go in the top of the
 * slot. Any setting that fits() comes before one that does not. */
static bool better(const struct as_setting *cur,
                   const struct as_setting *best, bool found)
{
    if (!found)
        return true;
    if (fits(cur) != fits(best))
        return fits(cur);
    if (cur->uac == 1)
        return cur->subslot > best->subslot;
    return best->subslot != 2 && cur->subslot == 2;
}

/* Class 1 type I rates: a list, or a range when the count is 0 */
static int uac1_rates(const uint8_t *c, int len)
{
    int n = c[7], rates = 0;

    if (n == 0 && len >= 14)
    {
        unsigned long lo = c[8] | (c[9] << 8) | (c[10] << 16);
        unsigned long hi = c[11] | (c[12] << 8) | (c[13] << 16);
        if (lo <= SAMPR_48 && hi >= SAMPR_48)
            rates |= RATE_48;
        if (lo <= SAMPR_44 && hi >= SAMPR_44)
            rates |= RATE_44;
    }
    for (int i = 0; i < n && 8 + 3 * i + 3 <= len; i++)
    {
        const uint8_t *r = &c[8 + 3 * i];
        unsigned long rate = r[0] | (r[1] << 8) | (r[2] << 16);
        if (rate == SAMPR_48)
            rates |= RATE_48;
        else if (rate == SAMPR_44)
            rates |= RATE_44;
    }
    return rates;
}

/* Walk the configuration for the AC interface's clock source or feature
 * unit, and the usable PCM streaming setting better() prefers. */
static bool find_setting(const struct usb_drv_host_enum *e,
                         struct as_setting *best)
{
    struct as_setting cur;
    bool in_ac = false, in_as = false, found = false, ac_uac1 = false;

    memset(&cur, 0, sizeof cur);
    memset(best, 0, sizeof *best);
    status.clock = -1;
    fu_unit = 0;
    memset(fu_ctl, 0, sizeof fu_ctl);
    for (int i = 0; i + 2 <= e->cfg_len && e->cfg[i] >= 2; i += e->cfg[i])
    {
        const uint8_t *c = &e->cfg[i];
        int len = c[0];

        if (i + len > e->cfg_len)
            break;
        if (c[1] == USB_DT_INTERFACE && len >= 9)
        {
            if (in_as && usable(&cur) && better(&cur, best, found))
            {
                *best = cur;
                found = true;
            }
            memset(&cur, 0, sizeof cur);
            cur.iface = c[2];
            cur.alt = c[3];
            cur.uac = c[7] == 0x20 ? 2 : 1;
            /* class 2 rates are the clock's, asked for at start */
            cur.rates = cur.uac == 2 ? RATE_48 | RATE_44 : 0;
            in_ac = c[5] == 1 && c[6] == 1;
            in_as = c[5] == 1 && c[6] == 2 && (c[7] == 0x20 || c[7] == 0);
            ac_uac1 = in_ac ? c[7] == 0 : ac_uac1;
            if (in_ac)
                status.ac_iface = c[2];
        }
        else if (c[1] == UAC_CS_INTERFACE && len >= 3 && in_as &&
                 cur.uac == 1)
        {
            if (c[2] == UAC2_AS_GENERAL && len >= 7)
            {
                /* wFormatTag 1 is PCM */
                cur.formats = (c[5] | (c[6] << 8)) == 1;
            }
            else if (c[2] == UAC2_FORMAT_TYPE && len >= 8)
            {
                cur.format_type = c[3];
                cur.channels = c[4];
                cur.subslot = c[5];
                cur.bits = c[6];
                cur.rates = uac1_rates(c, len);
            }
        }
        else if (c[1] == UAC_CS_ENDPOINT && len >= 4 && in_as &&
                 cur.uac == 1 && c[2] == UAC1_EP_GENERAL)
        {
            cur.rate_ctl = c[3] & 1;
        }
        else if (c[1] == UAC_CS_INTERFACE && len >= 3)
        {
            if (in_ac && ac_uac1 && c[2] == UAC_FEATURE_UNIT && len >= 7 &&
                fu_unit == 0 && c[5] >= 1)
            {
                /* bControlSize, then one control bitmap per channel from
                 * the master; the low byte holds mute and volume */
                fu_unit = c[3];
                for (int ch = 0; ch < 3 && 6 + ch * c[5] < len; ch++)
                    fu_ctl[ch] = c[6 + ch * c[5]];
            }
            else if (in_ac && !ac_uac1 && c[2] == UAC_FEATURE_UNIT &&
                     len >= 10 && fu_unit == 0)
            {
                /* four bytes a channel from the master, two bits a
                 * control: mute in bits 0-1, volume in 2-3, and 3 is
                 * settable by the host */
                fu_unit = c[3];
                for (int ch = 0; ch < 3 && 5 + ch * 4 < len - 1; ch++)
                    fu_ctl[ch] = ((c[5 + ch * 4] & 3) == 3) |
                                 (((c[5 + ch * 4] >> 2) & 3) == 3) << 1;
            }
            else if (in_ac && c[2] == UAC2_CLOCK_SOURCE && len >= 4 &&
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

            if ((c[2] & USB_DIR_IN) && ((c[3] >> 4) & 3) == 1 &&
                cur.uac == 2)
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
    if (in_as && usable(&cur) && better(&cur, best, found))
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
    do { \
        if (!(cond)) { \
            status.state = name; \
            status.ctrl_status = usb_drv_host_last_status(); \
            return false; \
        } \
    } while (0)

    usb_host_audio_stop();
    STEP("no device", e->result == 1);
    STEP("no PCM setting", find_setting(e, &as));
    status.uac = as.uac;
    status.iface = as.iface;
    status.alt = as.alt;
    status.channels = as.channels;
    status.subslot = as.subslot;
    status.bits = as.bits;
    status.ep_rate = as.uac == 1 && as.rate_ctl ? as.ep_out : 0;
    STEP("too many channels", fits(&as));
    sof_per_second = usb_drv_host_high_speed() ? 8000 : 1000;

    if (as.uac == 2)
        as.rates = uac2_rates();
    STEP("no 44.1 or 48 kHz", as.rates & (RATE_48 | RATE_44));
    offer_rates(as.rates);
#define DEFAULT_FREQ (usb_host_pcm_sink.caps.default_freq)

    STEP("set interface",
         usb_drv_host_control(DEV_ADDR, USB_DIR_OUT | USB_RECIP_INTERFACE,
                              USB_REQ_SET_INTERFACE, as.alt, as.iface,
                              NULL, 0) == 0);

    /* A clock can list 44.1 kHz and still refuse it: then 48 alone */
    if (!set_dac_rate(sink_rate(DEFAULT_FREQ)))
    {
        STEP("set rate", as.rates == (RATE_48 | RATE_44));
        offer_rates(RATE_48);
        STEP("set rate", set_dac_rate(sink_rate(DEFAULT_FREQ)));
    }
    /* Trap: left at the last session's rate, the switch below finds nothing
     * to change and never sets the DAC to the mixer's rate. It also indexes
     * this session's rate table, which can be shorter. */
    usb_host_pcm_sink.configured_freq = DEFAULT_FREQ;
    dac_unmute();

    memset(&iso, 0, sizeof iso);
    iso.addr = DEV_ADDR;
    iso.ep_out = as.ep_out & 0xf;
    iso.mps_out = as.mps_out;
    iso.interval_out = as.interval_out;
    iso.ep_fb = as.ep_fb;
    iso.mps_fb = as.mps_fb;
    iso.interval_fb = as.interval_fb;
    iso.frame_bytes = as.channels * as.subslot;
    iso.nominal = nominal(sink_rate(DEFAULT_FREQ));
    iso.begin = sink_begin;
    iso.fill = sink_fill;
    iso.lost = sink_lost;
    locked = 0;
    gain = 0;
    sink_stop();
    STEP("start stream", usb_drv_host_iso_start(&iso));
    STEP("switch sink", mixer_switch_sink(PCM_SINK_USB_HOST));
#undef DEFAULT_FREQ
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
