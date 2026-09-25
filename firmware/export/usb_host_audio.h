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
#ifndef USB_HOST_AUDIO_H
#define USB_HOST_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

/* A 1 kHz test tone to a USB Audio Class 2 DAC on the host probe's port. */
struct usb_host_audio_status {
    const char *state;      /* "idle", "playing", or the step that failed */
    int iface, alt;         /* the streaming interface setting in use */
    int ac_iface, clock;    /* where the sample rate is set */
    int channels, subslot, bits;
    uint32_t rate_set, rate_read;
};

bool usb_host_audio_start(void);
void usb_host_audio_stop(void);
const struct usb_host_audio_status *usb_host_audio_get_status(void);

#endif /* USB_HOST_AUDIO_H */
