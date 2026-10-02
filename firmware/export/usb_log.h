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
#ifndef USB_LOG_H
#define USB_LOG_H

#include <stdbool.h>
#include <stdint.h>

/* The device side of a USB connection, event by event: every control request
 * and how it was answered, endpoint allocation, the controller's endpoint
 * setup, and the audio stream. Callable from IRQ and FIQ context. The debug
 * menu's USB Log screen shows it; the application appends it to a file while
 * that screen is open or its Write Debug Log setting is on.
 *
 * What a, b, c and d carry, per type: */
enum usb_log_type
{
    USB_LOG_INSERT,      /* a = charge only */
    USB_LOG_BUS_RESET,   /* a = high speed */
    USB_LOG_SETUP,       /* a = bmRequestType, b = bRequest,
                          * c = wValue | wIndex << 16,
                          * d = wLength | USB_LOG_SETUP_* << 16 */
    USB_LOG_RESPONSE,    /* a = USB_LOG_RESP_*, b = bRequest, c = bytes */
    USB_LOG_SET_ADDR,    /* a = address, b = core state before it */
    USB_LOG_SET_CONFIG,  /* a = configuration, b = high speed */
    USB_LOG_DRIVERS,     /* a = active mask, b = errored mask */
    USB_LOG_EP_ALLOC,    /* a = driver, b = endpoint or 0 for none,
                          * c = transfer type, d = optional */
    USB_LOG_DRV_DROPPED, /* a = driver, left out for want of endpoints */
    USB_LOG_INTERFACES,  /* a = driver, b = first, c = one past its last */
    USB_LOG_EP_INIT,     /* a = endpoint, b = type, c = max packet size,
                          * d = the controller register it wrote */
    USB_LOG_ISO_XFER,    /* an isochronous transfer started: a = endpoint,
                          * b = frame, c, d = controller registers */
    USB_LOG_XFER_FAIL,   /* a transfer the controller would not start:
                          * a = endpoint, b = error, c, d = registers */
    USB_LOG_ISO_ERROR,   /* a = endpoint, c = status, d = length */
    USB_LOG_ALT,         /* a = interface, b = alternate, c = result */
    USB_LOG_RATE,        /* c = rate asked for, d = rate set */
    USB_LOG_AUDIO,       /* a = USB_LOG_AUDIO_*, c, d as named there */
    USB_LOG_STREAM,      /* a = frames dropped, b = errored packets,
                          * c = packets, d = shortest | longest << 16 */
    USB_LOG_FEEDBACK,    /* c = value as sent, d = bytes */
    USB_LOG_SYNC,        /* a = timed out, c = microseconds waited */
    /* The player as a USB host */
    USB_LOG_HOST_PORT,   /* port reset: a = attempt, c = HPRT or PORTSC,
                          * d = HFIR, or the ARC's translator port */
    USB_LOG_HOST_STAGE,  /* one try at a control stage: a = address,
                          * b = pid | in << 8 | try << 12,
                          * c = channel interrupts, bit 31 if it never
                          * halted, d = HCTSIZ after */
    USB_LOG_HOST_ENUM,   /* an enumeration step: a = passed,
                          * c = its name (a static string), d = status */
    USB_LOG_HOST_EHCI,   /* an ARC control transfer: a = address,
                          * b = request | translator port << 8,
                          * c = token that failed, d = bytes or -1 */
    USB_LOG_HOST_ISO,    /* the stream: a = 0, one of the first packets:
                          * b = parity, c = channel interrupts, d = HCTSIZ;
                          * a = 1, every 1000th: b = frames in it,
                          * c = packets sent, d = underruns | errors << 16 */
    USB_LOG_HOST_MODE,   /* the port turned host (a = 1) or back (a = 0) */
    USB_LOG_HOST_DAC,    /* the DAC search's decision: a = polls,
                          * b = the last poll's port, bit 0 VBUS,
                          * 1 connected, 2 enabled, USB_LOG_HOST_PORT_READ
                          * if there was one, c = the decision (a static
                          * string), d = HPRT or PORTSC1 */
    USB_LOG_ALLOC,       /* interfaces and endpoints being assigned:
                          * a = enabled drivers, b = core state */
    /* USB iAP, configuration 2 */
    USB_LOG_IAP,         /* a packet: a = lingo | USB_LOG_IAP_FROM_PLAYER,
                          * b = command, c = payload bytes | transaction
                          * << 16 (0xffff for none), d = the payload's
                          * first four bytes, big-endian, zero-padded */
    USB_LOG_IAP_RATE,    /* the host set a rate: a = ignored as not the one
                          * announced, c = rate, d = rate announced or 0 */
    USB_LOG_IAP_STREAM,  /* what the stream now carries: a =
                          * USB_LOG_IAP_STREAM_* */
    USB_LOG_IAP2,        /* an iAP2 report, raw: a = offset into it from
                          * the link-control byte | USB_LOG_IAP_FROM_PLAYER,
                          * b = report bytes, c, d = eight bytes from the
                          * offset, big-endian, zero-padded */
    USB_LOG_IAP2_SENT,   /* the player's iAP2 report left: c = status,
                          * d = bytes */
    USB_LOG_IAP2_PACKET, /* an iAP2 link packet: a = USB_LOG_IAP2_* status
                          * | USB_LOG_IAP_FROM_PLAYER, b = bytes, c =
                          * control, sequence, acknowledgement and session,
                          * a byte each from the top */
    USB_LOG_IAP2_MSG,    /* an iAP2 control message: a =
                          * USB_LOG_IAP_FROM_PLAYER or 0, b = bytes, c = ID */
    USB_LOG_IAP2_PARAMS, /* a received message's parameters, raw: a = bytes
                          * valid, b = offset, c, d = eight bytes from it */
    USB_LOG_IAP2_EVENT,  /* a = USB_LOG_IAP2_* event, c, d as it says */
};

#define USB_LOG_SETUP_DROPPED  1 /* arrived during a bus reset; ignored */
#define USB_LOG_SETUP_QUEUED   2 /* arrived mid-request; handled after it */

#define USB_LOG_RESP_ACK       0
#define USB_LOG_RESP_STALL     1
#define USB_LOG_RESP_TOO_LONG  2 /* data stage larger than the buffer */

#define USB_LOG_AUDIO_NO_BUFFERS  0
#define USB_LOG_AUDIO_START       1 /* c = rate */
#define USB_LOG_AUDIO_STOP        2
#define USB_LOG_AUDIO_FIRST_RX    3 /* c = length, d = status */
#define USB_LOG_AUDIO_PREBUFFERED 4 /* playback started */
#define USB_LOG_AUDIO_UNDERFLOW   5
#define USB_LOG_AUDIO_OVERFLOW    6
#define USB_LOG_AUDIO_THREAD_START 7 /* playback started on the USB thread */

#define USB_LOG_HOST_PORT_READ 0x80

#define USB_LOG_IAP_FROM_PLAYER 0x80

#define USB_LOG_IAP2_OK       0
#define USB_LOG_IAP2_CHECKSUM 1 /* the payload's checksum is wrong */
#define USB_LOG_IAP2_TOO_LONG 2 /* longer than the player offered */
#define USB_LOG_IAP2_AGAIN    3 /* the player's, sent again */

#define USB_LOG_IAP2_LINK_UP     0 /* c = control session, d = window */
#define USB_LOG_IAP2_LINK_LOST   1 /* c = sequence never acknowledged,
                                    * d = sends */
#define USB_LOG_IAP2_CERTIFICATE 2 /* c = its bytes, d = challenge bytes */
#define USB_LOG_IAP2_IDENTIFIED  3 /* c = messages the car sends, d = takes */
#define USB_LOG_IAP2_RATE        4 /* c = rate announced, d = index sent */
#define USB_LOG_IAP2_NOW_PLAYING 5 /* c = track key, d = state << 24 | s */
#define USB_LOG_IAP2_SEEK        6 /* c = ms */
#define USB_LOG_IAP2_HID         7 /* c = controls found, d = descriptor
                                    * bytes */
#define USB_LOG_IAP2_BUTTON      8 /* c = usage, d = 1 when acted on */
#define USB_LOG_IAP2_DROPPED     9 /* c = message ID not sent, d = bytes */
#define USB_LOG_IAP2_POWER      10 /* c = 0 empty, 1 attributes; d = mask
                                    * the car asked for */

#define USB_LOG_IAP_STREAM_AUDIO     0 /* the playing audio */
#define USB_LOG_IAP_STREAM_NOTHING   1 /* silence: nothing to play */
#define USB_LOG_IAP_STREAM_RATE_WAIT 2 /* silence: the host has not set the
                                        * rate the player announced */

struct usb_log_entry
{
    uint32_t us;         /* USEC_TIMER, wrapping */
    uint8_t type, a;
    uint16_t b;
    uint32_t c, d;
};

#ifdef BOOTLOADER
#define usb_log(type, a, b, c, d) do { } while (0)
#define usb_log_sync() do { } while (0)
#else
void usb_log(int type, int a, int b, uint32_t c, uint32_t d);

/* Thread context only. While something is writing the file, wait up to three
 * seconds for everything logged so far to reach it, so that a step which
 * hangs the player is on disk before it is taken. Returns at once when
 * nothing is writing or the host has the disk. */
void usb_log_sync(void);

/* For the screen and the file. Sequence numbers count every entry since boot;
 * the ring keeps the most recent USB_LOG_SIZE. */
#define USB_LOG_SIZE 512
unsigned long usb_log_head(void);
bool usb_log_read(unsigned long seq, struct usb_log_entry *e);
void usb_log_attach(bool on);
void usb_log_set_written(unsigned long seq);
unsigned long usb_log_written(void);

/* Names for a USB_LOG_IAP entry, "?" for one the table does not know.
 * Defined only where USB_ENABLE_IAP is. */
const char *usb_log_iap_lingo(int lingo);
const char *usb_log_iap_command(int lingo, int command);
const char *usb_log_iap2_message(int id);
#endif

#endif /* USB_LOG_H */
