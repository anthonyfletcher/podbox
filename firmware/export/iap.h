/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Alan Korr
 *
 * All files in this archive are subject to the GNU General Public License.
 * See the file COPYING in the source tree root for full license agreement.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#ifndef __IAP_H__
#define __IAP_H__

#include <stdbool.h>

/* This is just the payload size, without sync, length and checksum */
#define RX_BUFLEN (64*1024)
/* This is the entire frame length, sync, length, payload and checksum */
#define TX_BUFLEN 128

#ifdef HAVE_IAP_MULTIPORT
#define IF_IAP_MP(x...) x
#define IF_IAP_MP_NONVOID(x...) x
#else
#define IF_IAP_MP(x...)
#define IF_IAP_MP_NONVOID(x...) void
#endif

extern bool iap_getc(IF_IAP_MP(int port,) unsigned char x);
extern void iap_setup(int ratenum);
extern void iap_bitrate_set(int ratenum);
extern void iap_enable(bool enable);
/* An accessory is talking on the dock connector serial line */
extern bool iap_accessory_present(void);
extern void iap_periodic(void);
extern void iap_handlepkt(void);
extern void iap_send_pkt(const unsigned char * data, int len);
const unsigned char *iap_get_serbuf(void);
#ifdef HAVE_LINE_REC
extern bool iap_record(bool onoff);
#endif
void iap_reset_state(IF_IAP_MP_NONVOID(int port) ); /* 0 is dock, 1 is headphone */
/* USB power went: the session resets, as for an accessory that has gone,
 * but playback carries on */
void iap_power_reset(void);
/* The player set the codec's volume, which ends an accessory's mute */
void iap_volume_set(void);
#ifdef HAVE_IAP_ACCESSORY_POLL
/* Ten times a second from the iAP thread, on a target whose accessory
 * detect can only be read from a thread: resets the session when the
 * accessory goes. */
void iap_accessory_poll(void);
#endif
bool dbg_iap(void);
#endif
