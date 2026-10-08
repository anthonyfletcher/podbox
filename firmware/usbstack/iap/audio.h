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
#include <stdbool.h>
#include <stdint.h>

bool iap_audio_init(void);
bool iap_audio_deinit(void);
bool iap_audio_enable(void);
bool iap_audio_disable(void);
bool iap_audio_set_sampr(uint32_t sampr);
/* TrackNewAudioAttributes once the accessory's rates are accepted, if
 * nothing has sent it yet on this connection. */
void iap_audio_connected(void);
/* USB thread: TrackNewAudioAttributes again if a track has started since. */
void iap_audio_tick(void);
/* The rate the iAP sink is configured for. */
unsigned long iap_audio_sampr(void);

/* What the stream has done since the last call, for the USB log: bytes
 * taken from playback, in how many chunks; packets of silence sent for want
 * of any; chunks that were not a whole number of stereo samples. */
struct iap_audio_counts
{
    uint32_t bytes, chunks, silent, odd;
};
void iap_audio_take_counts(struct iap_audio_counts *c);
