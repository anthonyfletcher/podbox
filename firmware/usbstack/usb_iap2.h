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
#ifndef USB_IAP2_H
#define USB_IAP2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The iAP driver's hooks into iAP2, on the USB thread unless they say. */

/* At each SET_CONFIGURATION of the iAP configuration. */
void usb_iap2_connect(void);
/* An output report from SET_REPORT. True when it was iAP2's: the caller
 * acknowledges it and libiap never sees it. */
bool usb_iap2_report(const uint8_t *report, size_t len);
/* The HID IN endpoint finished. True when the report was iAP2's. */
bool usb_iap2_sent(int status, int length);
/* Every tenth of a second. True once the probe has been answered, when
 * the connection is iAP2's and libiap stays quiet. */
bool usb_iap2_tick(void);
/* At disconnect: true when the connection was iAP2's and playback never
 * left the built-in sink, so it carries on. */
bool usb_iap2_keeps_playing(void);
/* The iAP sink's new rate, from the audio thread. True when iAP2 announces
 * it, and libiap is not asked to. */
bool usb_iap2_audio_rate(unsigned long rate);

/* Between the link layer, usb_iap2.c, and the control session,
 * usb_iap2_control.c. */

/* A message is built in place in the next free packet: start returns where
 * it goes and how much fits, or NULL when the link is down or full; send
 * queues the n bytes built. */
uint8_t *usb_iap2_message_start(size_t *room);
void usb_iap2_message_send(size_t n);
/* As send, on the file transfer session; false when the accessory's link
 * has none, and the packet is not sent. */
bool usb_iap2_file_send(size_t n);
/* Packets that can be queued now. */
size_t usb_iap2_room(void);

void usb_iap2_control_link_up(void);
void usb_iap2_control_reset(void);
void usb_iap2_control_receive(const uint8_t *msg, size_t len);
/* A packet's payload on the file transfer session. */
void usb_iap2_control_file(const uint8_t *data, size_t len);
/* A packet has gone and there is room to queue another. */
void usb_iap2_control_room(void);
void usb_iap2_control_tick(void);
/* From usb_iap2_audio_rate(), on the audio thread. */
void usb_iap2_control_rate(unsigned long rate);
/* Whether the car has playback on the iAP sink. */
bool usb_iap2_control_audio(void);

#endif
