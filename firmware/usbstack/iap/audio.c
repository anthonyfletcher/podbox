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
#include "core_alloc.h"
#include "events.h"
#include "pcm-internal.h"
#include "pcm_sampr.h"
#include "pcm_mixer.h"
#include "pcm_sink.h"
#include "system.h"
#include "usb_drv.h"
#include "usb_log.h"

#include "../usb_iap.h"
#include "buffer.h"
#include "libiap/constants.h"
#include "libiap/iap.h"
#include "macros.h"
#include "platform.h"
#include "appevents.h"
#include "../usb_iap2.h"
#include "audio.h"

static const unsigned long samprs[] = {
    SAMPR_48,
    SAMPR_44,
    SAMPR_32,
};

/* The sink's rates: those of samprs[] the accessory lists, so the mixer
 * resamples to one of them and nothing else is announced. Every freq below
 * indexes this list, not samprs[]. */
static unsigned long acc_samprs[ARRAYLEN(samprs)];
#define RATE(freq) (iap_pcm_sink.caps.samprs[(freq)])

struct StagingBuffer {
    struct IAPAllocResult buf;
    uint16_t              cursor;
};
static struct StagingBuffer staging_buffers[USB_BATCH_SLOTS + 1]; /* +1 for silent buffer */
static int                  staging_buffer_index;

#define zero_buffer (staging_buffers[USB_BATCH_SLOTS])

static const uint8_t* pulled_buf;
static size_t         pulled_buf_size;
static size_t         pulled_buf_cursor;
static int8_t         set_freq; /* requested freq from rockbox */
static int8_t         cur_freq; /* requested freq from accessory */
static uint8_t        packet_count;
static uint8_t        logged_stream; /* USB_LOG_IAP_STREAM_*, or 0xff */

static bool enabled;
static bool exhausted;
static bool track_attrs_sent;
static volatile bool track_changed; /* since TrackNewAudioAttributes */
static volatile unsigned int pcm_lock_count;

/* A new rate plays only once the accessory acks its TrackNewAudioAttributes,
 * or after a second with no ack (Gerrit 7900). A track start re-announcing
 * the same rate is not held, nor is iAP2, which announces rates its own way.
 * Options if a dock objects: drop the acked_freq test from batch_get_more(),
 * or the reset keep from usb_iap_disconnect(). 7900 as written takes the
 * sink at configuration and holds every rate for the ack, which silences an
 * iAP1 accessory playing the line out and an iAP2 car. */
static bool   accessory_ready; /* its rates accepted, so it can be told ours */
static int8_t acked_freq; /* the rate the accessory has acked, or -1 */
static int8_t asked_freq; /* a rate announced and not yet acked, or -1 */
static long   asked_tick;
/* An accessory that sets no rate on the stream within half a second of an
 * announcement plays the announced one: some never set one */
static bool   host_set_rate = true; /* a SET_CUR since the announcement */
static long   announced_tick;
/* An accessory that never answers GetAccessorySampleRateCaps but sets a rate
 * on the stream plays that rate, once it has sent nothing for two seconds,
 * neither a SET_CUR nor a report: one still identifying lists its rates
 * after. One that keeps talking, polling the play status, plays it four
 * seconds after the SET_CUR or the end of authentication, whichever is
 * later. */
static int8_t host_rate = -1; /* index into samprs[], or -1 */
static long   host_rate_tick;     /* the last SET_CUR or report */
static long   host_rate_set_tick; /* the last SET_CUR, or authentication */
/* libiap sends no notification while a TrackNewAudioAttributes ack is owed,
 * so one owed for a second is written off: an accessory that never acks
 * still hears of track changes */
static bool   attrs_owed;
static long   attrs_owed_tick;

extern bool iap_initialized;

/* See iap_audio_take_counts() */
static volatile uint32_t count_bytes, count_chunks, count_silent, count_odd;

extern struct pcm_sink iap_pcm_sink;

/* The sink offers the rates of samprs[] whose indexes are set in listed,
 * 48 kHz the default where it is one. The list does not change under a sink
 * in use, which gives way to the built-in one while it does. */
static void offer_rates(unsigned listed) {
    struct pcm_sink_caps* caps = &iap_pcm_sink.caps;
    unsigned long         list[ARRAYLEN(samprs)];
    uint8_t               n   = 0;
    uint8_t               def = 0;

    for(size_t i = 0; i < ARRAYLEN(samprs); i += 1) {
        if(listed & (1u << i)) {
            if(samprs[i] == SAMPR_48) {
                def = n;
            }
            list[n++] = samprs[i];
        }
    }
    if(n == caps->num_samprs && def == caps->default_freq && !memcmp(list, caps->samprs, n * sizeof(list[0]))) {
        return;
    }

    const bool in_use = pcm_current_sink() == PCM_SINK_IAP;
    if(in_use) {
        mixer_switch_sink(PCM_SINK_BUILTIN);
    }
    /* the stream's packets are sized from it in the USB interrupt */
    const int oldlevel = disable_irq_save();
    memcpy(acc_samprs, list, sizeof(list));
    caps->samprs       = acc_samprs;
    caps->num_samprs   = n;
    caps->default_freq = def;
    set_freq           = -1;
    cur_freq           = -2;
    acked_freq         = -1;
    asked_freq         = -1;
    /* so iap_audio_connected() announces a rate from the new list */
    track_attrs_sent   = false;
    restore_irq(oldlevel);
    if(in_use) {
        mixer_switch_sink(PCM_SINK_IAP);
    }
}

static void sink_set_freq(uint16_t freq) {
    LOG("freq=%d", freq);

    set_freq = freq;

    if(usb_iap2_audio_rate(RATE(freq))) {
        host_set_rate    = true; /* iAP2 sets its rate its own way */
        track_attrs_sent = true;
        track_changed    = false;
        acked_freq       = freq;
        return;
    }
    if(!accessory_ready) {
        /* announced once the rates arrive, by iap_audio_connected(): a sink
         * kept through a bus reset hears rates before the dock is back */
        return;
    }
    track_attrs_sent = true;
    track_changed    = false;
    host_set_rate    = false;
    announced_tick   = current_tick;
    if(freq != acked_freq) {
        asked_freq = freq;
        asked_tick = current_tick;
    } else {
        /* back to the acked rate: an ack still owed is for another */
        asked_freq = -1;
    }

    struct IAPContext* ctx = _iap_acquire_ctx(true);
    check_act(iap_select_sampr(ctx, RATE(freq)), );
    _iap_release_ctx();
}

static size_t calc_packet_size(unsigned long cur_sampr, uint8_t packet_count) {
    /* packet size calculation
     * (4 = sizeof(int16_t) * channels)
     * (1000 = usb frames per second)
     * 48000Hz:
     *   48000 * 4 / 1000 = 192.0
     *   => 192 + ...
     * 44100Hz:
     *   44100 * 4 / 1000 = 176.4
     *   => (9 * 176 + 180) + (...
     */
    if(cur_sampr == SAMPR_48) {
        return 192;
    } else if(cur_sampr == SAMPR_32) {
        return 128;
    } else {
        /*44.1k*/
        return packet_count % 10 == 0 ? 180 : 176;
    }
}

static void batch_get_more(const void** ptr, size_t* len) {
    const size_t packet_size = calc_packet_size(cur_freq >= 0 ? RATE(cur_freq) : 0, packet_count);
#if 0
    const int cur_frame = usb_drv_get_frame_number();
    LOG("ex=%d set=%d cur=%d", exhausted, set_freq, cur_freq);
#endif

start:
    {
        const uint8_t stream = exhausted ? USB_LOG_IAP_STREAM_NOTHING
                             : cur_freq != set_freq || acked_freq != set_freq ? USB_LOG_IAP_STREAM_RATE_WAIT
                                                    : USB_LOG_IAP_STREAM_AUDIO;
        if(stream != logged_stream) {
            logged_stream = stream;
            usb_log(USB_LOG_IAP_STREAM, stream, 0, 0, 0);
        }
    }
    if(pcm_lock_count || exhausted || cur_freq != set_freq || acked_freq != set_freq) {
        *ptr = zero_buffer.buf.ptr;
        *len = packet_size;
        packet_count = (packet_count + 1) % 10;
        count_silent++;
        return;
    }

    if(pulled_buf_cursor == pulled_buf_size) {
        /* run out of previously pulled data.
         * let's start filling them, by requesting new data from upstream */
        if(!pcm_play_dma_complete_callback(PCM_DMAST_OK, (const void**)&pulled_buf, &pulled_buf_size)) {
            /* no more data, but we have to keep sending something as long as the audio stream interface is enabled */
            exhausted = true;
            goto start;
        }

        /* pushing_{buf,buf_size} are filled. reset cursor and continue filling */
        pcm_play_dma_status_callback(PCM_DMAST_STARTED);
        pulled_buf_cursor = 0;
        count_bytes += pulled_buf_size;
        count_chunks++;
        if(pulled_buf_size & 3)
            count_odd++;
    }

    /* fill this single packet */
    struct StagingBuffer* stage = &staging_buffers[staging_buffer_index];
    const size_t          copy  = MIN(packet_size - stage->cursor, pulled_buf_size - pulled_buf_cursor);
    memcpy(stage->buf.ptr + stage->cursor, pulled_buf + pulled_buf_cursor, copy);
    pulled_buf_cursor += copy;
    stage->cursor += copy;
#define AUDIO_STAT 0
#if AUDIO_STAT == 1
    static int last_hz;
    static int sample;
#endif
    if(stage->cursor == packet_size) {
        *ptr                 = stage->buf.ptr;
        *len                 = stage->cursor;
        stage->cursor        = 0;
        staging_buffer_index = (staging_buffer_index + 1) % USB_BATCH_SLOTS;
        packet_count = (packet_count + 1) % 10;
#if AUDIO_STAT == 1
        sample += packet_size / 4;
        if(current_tick >= last_hz + HZ) {
            logf("pushed %d %d", packet_size, sample);
            sample  = 0;
            last_hz = current_tick;
        }
#endif
    } else { /* pushing_buf_cursor == pushing_buf_size */
        goto start;
    }
}

static void sink_play(const void* addr, size_t size) {
    LOG("play");

    int oldlevel = disable_irq_save();
    pulled_buf        = addr;
    pulled_buf_size   = size;
    pulled_buf_cursor = 0;
    exhausted         = false;
    staging_buffers[staging_buffer_index].cursor = 0;
    restore_irq(oldlevel);

    if(!iap_initialized) {
        return;
    }

    /* resolve pending play request before sending TrackNewAudioAttributes (by sink_set_freq).
     * some accessories will get confused when receiving it while waiting for an ack */
    struct IAPContext* ctx = _iap_acquire_ctx(true);
    struct Platform*   plt = ctx->platform;
    if(plt->control_pending) {
        check_act(iap_control_response(ctx, plt->pending_control, true), );
        plt->control_pending = false;
    }
    _iap_release_ctx();

    /* rockbox only calls set_freq when changes occur, but we must call
     * set_freq(and iap_select_sampr) at least once per connection. */
    if(!track_attrs_sent) {
        sink_set_freq(iap_pcm_sink.configured_freq);
    }
}

/* Digital Audio 1.01 and later announce every track, not only a new rate,
 * and libiap reports the lingo as 1.03. */
static void on_track_change(unsigned short id, void* data) {
    (void)id;
    (void)data;
    track_changed = true;
}

void iap_audio_connected(void) {
    accessory_ready = true;
    /* The sink keeps its rate between connections, so switching to it sets
     * no rate after the first, and a dock that waits for the announcement
     * before it sends any button would wait for ever. */
    if(!track_attrs_sent) {
        sink_set_freq(iap_pcm_sink.configured_freq);
    }
}

void iap_audio_attrs_ack(bool success) {
    LOG("attrs ack=%d asked=%d set=%d", success, asked_freq, set_freq);
    if(asked_freq < 0) {
        /* a track start's re-announcement */
        return;
    }
    /* a refused rate stays silent: the accessory cannot play it */
    if(success && asked_freq == set_freq) {
        acked_freq = set_freq;
    }
    asked_freq = -1;
}

void iap_audio_tick(void) {
    if(asked_freq >= 0 && asked_freq == set_freq && TIME_AFTER(current_tick, asked_tick + HZ)) {
        ERROR("no ack for rate %d, playing anyway", asked_freq);
        acked_freq = asked_freq;
        asked_freq = -1;
    }
    {
        struct IAPContext* ctx = _iap_acquire_ctx(true);
        if(!ctx->waiting_for_audio_attrs_ack) {
            attrs_owed = false;
        } else if(!attrs_owed) {
            attrs_owed      = true;
            attrs_owed_tick = current_tick;
        } else if(TIME_AFTER(current_tick, attrs_owed_tick + HZ)) {
            ERROR("no ack for audio attributes, notifying anyway");
            ctx->waiting_for_audio_attrs_ack = iap_false;
            attrs_owed                       = false;
        }
        _iap_release_ctx();
    }
    if(track_changed && track_attrs_sent) {
        sink_set_freq(set_freq);
    }
    if(set_freq >= 0 && cur_freq != set_freq && !host_set_rate &&
       TIME_AFTER(current_tick, announced_tick + HZ / 2)) {
        usb_log(USB_LOG_IAP_RATE, 0, 1, RATE(set_freq), 0);
        cur_freq = set_freq;
    }
    if(!accessory_ready && host_rate >= 0) {
        /* not while it authenticates: the rates are asked for after that,
         * so the limit counts from its end */
        struct IAPContext* ctx     = _iap_acquire_ctx(true);
        const bool         authing = ctx->phase == IAPPhase_Auth;
        _iap_release_ctx();
        if(authing) {
            host_rate_set_tick = current_tick;
        } else if(TIME_AFTER(current_tick, host_rate_tick + 2 * HZ) ||
                  TIME_AFTER(current_tick, host_rate_set_tick + 4 * HZ)) {
            usb_log(USB_LOG_IAP_RATE, 0, 2, samprs[host_rate],
                    !TIME_AFTER(current_tick, host_rate_tick + 2 * HZ));
            offer_rates(1u << host_rate);
            host_rate = -1;
            iap_platform_take_sink();
        }
    }
}

static void sink_stop(void) {
    LOG("stop");
    /* we don't call usb_drv_batch_stop() here,
     * because we need to send something, even not playing. */
    int oldlevel = disable_irq_save();
    exhausted = true;
    pulled_buf = NULL;
    pulled_buf_size = pulled_buf_cursor = 0;
    staging_buffers[staging_buffer_index].cursor = 0;
    restore_irq(oldlevel);
}

/* The USB IRQ sends silence while a thread changes the mixer, rather than
 * entering it. IRQs stay on: a rate change can take the iAP mutex. */
static void sink_lock(void) {
    int oldlevel = disable_irq_save();
    pcm_lock_count++;
    restore_irq(oldlevel);
}

static void sink_unlock(void) {
    int oldlevel = disable_irq_save();
    pcm_lock_count--;
    restore_irq(oldlevel);
}

static void sink_nop(void) {
}

struct pcm_sink iap_pcm_sink = {
    .caps = {
        .samprs       = samprs,
        .num_samprs   = ARRAYLEN(samprs),
        .default_freq = 0,
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

bool iap_audio_init(void) {
    check_act(usb_drv_batch_init(AS_EP_IN, batch_get_more) == 0, return false);

    /* every rate for a new connection; a sink kept through a bus reset keeps
     * the dock's until they come again, and its stream carries on from where
     * it stopped once the dock has a rate */
    if(pcm_current_sink() != PCM_SINK_IAP) {
        offer_rates((1u << ARRAYLEN(samprs)) - 1);
        exhausted = true;
    }

    for(size_t i = 0; i < ARRAYLEN(staging_buffers); i += 1) {
        check_act(iap_alloc_usb_send_buffer(AS_PACKET_SIZE, &staging_buffers[i].buf), goto error);
        staging_buffers[i].cursor = 0;
    }
    staging_buffer_index = 0;

    memset(zero_buffer.buf.ptr, 0, AS_PACKET_SIZE);

    set_freq         = -1;
    cur_freq         = -2; /* something <0 && !=set_freq */
    enabled          = false;
    track_attrs_sent = false;
    track_changed    = false;
    accessory_ready  = false;
    acked_freq       = -1;
    asked_freq       = -1;
    host_rate        = -1;
    attrs_owed       = false;
    packet_count     = 0;
    logged_stream    = 0xff;

    add_event(PLAYBACK_EVENT_TRACK_CHANGE, on_track_change);
    return true;

error:
    usb_drv_batch_deinit();
    for(size_t i = 0; i < ARRAYLEN(staging_buffers); i += 1) {
        if(staging_buffers[i].buf.ptr != NULL) {
            core_free(staging_buffers[i].buf.handle);
            staging_buffers[i].buf.ptr = NULL;
        }
    }
    return false;
}

bool iap_audio_deinit(void) {
    accessory_ready = false;
    remove_event(PLAYBACK_EVENT_TRACK_CHANGE, on_track_change);
    check_act(usb_drv_batch_deinit() == 0, );
    for(size_t i = 0; i < ARRAYLEN(staging_buffers); i += 1) {
        core_free(staging_buffers[i].buf.handle);
        staging_buffers[i].buf.ptr = NULL;
    }
    return true;
}

bool iap_audio_enable(void) {
    LOG("enabled=%d", enabled);
    check_act(enabled || usb_drv_batch_start() == 0, return false);
    enabled = true;
    return true;
}

bool iap_audio_disable(void) {
    LOG("enabled=%d", enabled);
    check_act(!enabled || usb_drv_batch_stop() == 0, return false);
    enabled = false;
    return true;
}

unsigned long iap_audio_sampr(void) {
    return RATE(iap_pcm_sink.configured_freq);
}

bool iap_audio_accessory_rates(struct IAPSpan* list) {
    unsigned listed = 0;
    unsigned count  = 0;
    uint32_t rate;

    while(iap_span_read_32(list, &rate)) {
        count += 1;
        for(size_t i = 0; i < ARRAYLEN(samprs); i += 1) {
            if(samprs[i] == rate) {
                listed |= 1u << i;
            }
        }
    }
    /* one rate the player can send is enough */
    const bool ok = listed != 0;
    usb_log(USB_LOG_IAP_RATES, ok, count, listed, 0);
    check_act(ok, return false, "accessory lists none of our rates");
    offer_rates(listed);
    return true;
}

void iap_audio_take_counts(struct iap_audio_counts *c) {
    const int irq = disable_irq_save();
    c->bytes  = count_bytes;
    c->chunks = count_chunks;
    c->silent = count_silent;
    c->odd    = count_odd;
    count_bytes = count_chunks = count_silent = count_odd = 0;
    restore_irq(irq);
}

void iap_audio_accessory_heard(void) {
    host_rate_tick = current_tick;
}

bool iap_audio_set_sampr(uint32_t sampr) {
    if(!accessory_ready) {
        for(size_t i = 0; i < ARRAYLEN(samprs); i += 1) {
            if(samprs[i] == sampr) {
                host_rate          = i;
                host_rate_tick     = current_tick;
                host_rate_set_tick = current_tick;
            }
        }
    }

    uint16_t freq = 0;
    for(; freq < iap_pcm_sink.caps.num_samprs; freq += 1) {
        if(RATE(freq) == sampr) {
            break;
        }
    }
    check_act(freq < iap_pcm_sink.caps.num_samprs, return false);

    usb_log(USB_LOG_IAP_RATE, set_freq >= 0 && freq != set_freq, 0, sampr,
            set_freq >= 0 ? RATE(set_freq) : 0);
    host_set_rate = true;

    if(set_freq >= 0 && freq != set_freq) {
        /* Accessories should only set the frequency we requested via
         * TrackNewAudioAttributes, but in some cases, USB control requests may arrive late,
         * breaking established cur_freq == set_freq.
         * Ignoring sets of frequencies that were not requested can work around this. */
        ERROR("wrong frequency set: expected=%d got=%d", set_freq, freq);
        return true;
    }

    cur_freq = freq;

    LOG("sampr=%lu, set_freq=%d cur_freq=%d", sampr, set_freq, cur_freq);

    return true;
}
