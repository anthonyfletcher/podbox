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
#include "iap-usb.h"
#include "libiap/iap.h"
#include "platform.h"

#include "macros.h"

extern bool iap_initialized;

void iap_on_track_time_position(uint32_t pos_ms) {
    if(!iap_initialized) {
        return;
    }
    struct IAPContext* ctx = _iap_acquire_ctx(false);
    iap_notify_track_time_position(ctx, pos_ms);
}

void iap_on_track_playback_index(uint32_t index, bool track_ready) {
    if(!iap_initialized) {
        return;
    }

    struct IAPContext* ctx = _iap_acquire_ctx(false);

    /* before the accessory hears of the track and asks about it */
    iap_platform_track_changed(index);
    iap_platform_track_id(ctx);

    /* Sent on the change itself, whatever the art: a track that was already
     * buffered gets no second call. A track that was not gets one from
     * audio_finish_load_track() (track_ready), which says it again once the
     * playback engine's art, the fallback to the art cache, is loaded. */
    (void)track_ready;
    iap_notify_track_playback_index(ctx, index);
}

void iap_on_tracks_count(uint32_t count) {
    if(!iap_initialized) {
        return;
    }

    struct IAPContext* ctx = _iap_acquire_ctx(false);
    iap_notify_tracks_count(ctx, count);
}

void iap_on_play_status(int status) {
    if(!iap_initialized) {
        return;
    }

    struct IAPContext* ctx = _iap_acquire_ctx(false);
    iap_notify_play_status(ctx, _iap_convert_play_status(status));
}

void iap_on_volume(int volume) {
    if(!iap_initialized) {
        return;
    }

    struct IAPContext* ctx = _iap_acquire_ctx(false);
    iap_notify_volume(ctx, _iap_convert_volume(volume), iap_false);
}

void iap_on_shuffle_state(bool state) {
    if(!iap_initialized) {
        return;
    }

    struct IAPContext* ctx = _iap_acquire_ctx(false);
    iap_notify_shuffle_state(ctx, _iap_convert_shuffle_state(state));
}

void iap_on_repeat_state(int state) {
    if(!iap_initialized) {
        return;
    }

    struct IAPContext* ctx = _iap_acquire_ctx(false);
    iap_notify_repeat_state(ctx, _iap_convert_repeat_state(state));
}

void iap_on_power_off(void) {
    if(!iap_initialized) {
        return;
    }

    struct IAPContext* ctx = _iap_acquire_ctx(false);
    iap_notify_power_off(ctx);
}
