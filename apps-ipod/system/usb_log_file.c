/***************************************************************************
 * GNU General Public License (version 2+)
 * was: apps-ipod/screens/system/debug_menu.c (the USB Log screen's file)
 *
 * See usb_log_file.h. The ring lives in firmware (usb_log.c); this is the
 * side that turns it into text and puts it on disk.
 *
 * Parts: the entry formatter; the file, through debug_log's, whose lock
 * keeps one writer at a time; the background writer thread; the screen's and
 * the setting's hooks.
 ****************************************************************************/
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "thread.h"
#include "file.h"
#include "usb.h"
#include "usb_core.h"
#include "usb_ch9.h"
#include "usb_log.h"
#include "settings/settings.h"
#include "system/debug_log.h"
#include "usb_log_file.h"

/* ---- the entry formatter ------------------------------------------------ */

static const char *usb_log_driver_name(int i)
{
    switch (i)
    {
#ifdef USB_ENABLE_STORAGE
        case USB_DRIVER_MASS_STORAGE: return "storage";
#endif
#ifdef USB_ENABLE_CHARGING_ONLY
        case USB_DRIVER_CHARGING_ONLY: return "charging";
#endif
#ifdef USB_ENABLE_HID
        case USB_DRIVER_HID: return "hid";
#endif
#ifdef USB_ENABLE_AUDIO
        case USB_DRIVER_AUDIO: return "audio";
#endif
#ifdef USB_ENABLE_IAP
        case USB_DRIVER_IAP: return "iap";
#endif
        default: return "?";
    }
}

/* What a setup packet asks for, as far as names go */
static const char *usb_log_request_name(int type, int req, int value)
{
    static const char * const std[] = {
        "GET_STATUS", "CLEAR_FEATURE", "?", "SET_FEATURE", "?",
        "SET_ADDRESS", "GET_DESCRIPTOR", "SET_DESCRIPTOR",
        "GET_CONFIGURATION", "SET_CONFIGURATION", "GET_INTERFACE",
        "SET_INTERFACE", "SYNCH_FRAME" };

    switch ((type >> 5) & 3)
    {
        case 0:
            if (req == USB_REQ_GET_DESCRIPTOR)
            {
                switch (value >> 8)
                {
                    case 1: return "GET_DESCRIPTOR device";
                    case 2: return "GET_DESCRIPTOR config";
                    case 3: return "GET_DESCRIPTOR string";
                    case 6: return "GET_DESCRIPTOR qualifier";
                    case 7: return "GET_DESCRIPTOR other speed";
                    case 0x0f: return "GET_DESCRIPTOR BOS";
                    case 0x22: return "GET_DESCRIPTOR HID report";
                    default: return "GET_DESCRIPTOR ?";
                }
            }
            return req < (int)ARRAYLEN(std) ? std[req] : "?";
        case 1:
            /* audio's names; other classes reuse the numbers */
            switch (req | (type & 0x80) << 1)
            {
                case 0x001: return "class (audio SET_CUR)";
                case 0x181: return "class (audio GET_CUR)";
                case 0x182: return "class (audio GET_MIN)";
                case 0x183: return "class (audio GET_MAX)";
                case 0x184: return "class (audio GET_RES)";
                default: return "class";
            }
        case 2:
            return "vendor";
        default:
            return "reserved";
    }
}

#ifdef USB_ENABLE_IAP
/* "iap > General:StartIDPS #0, 0 bytes": > from the accessory, < from the
 * player. An acknowledgement names the command it answers instead of its
 * payload, since that is what a stalled exchange is read for. */
static void usb_log_format_iap(const struct usb_log_entry *e, char *p,
                               size_t size)
{
    int lingo = e->a & ~(USB_LOG_IAP_FROM_PLAYER | USB_LOG_IAP_SERIAL);
    const char *name = usb_log_iap_command(lingo, e->b);
    unsigned len = e->c & 0xffff, trans = e->c >> 16;
    int n = snprintf(p, size, "iap%s %c %s:%s",
                     e->a & USB_LOG_IAP_SERIAL ? " serial" : "",
                     e->a & USB_LOG_IAP_FROM_PLAYER ? '<' : '>',
                     usb_log_iap_lingo(lingo), name);
    if (n < 0 || (size_t)n >= size)
        return;
    p += n;
    size -= n;
    if (trans != 0xffff)
    {
        n = snprintf(p, size, " #%u", trans);
        if (n < 0 || (size_t)n >= size)
            return;
        p += n;
        size -= n;
    }

    if ((!strcmp(name, "IPodAck") || !strcmp(name, "AccessoryAck"))
        && len >= 2)
    {
        /* status, then the command: one byte, or two in lingo 4 */
        int status = e->d >> 24;
        int cmd = lingo == 4 ? (e->d >> 8) & 0xffff : (e->d >> 16) & 0xff;
        snprintf(p, size, ": %s %s", usb_log_iap_command(lingo, cmd),
                 status ? "FAILED" : "ok");
        if (status)
        {
            n = strlen(p);
            snprintf(p + n, size - n, " (status %02x)", status);
        }
        return;
    }

    n = snprintf(p, size, ", %u byte%s", len, len == 1 ? "" : "s");
    for (unsigned i = 0; i < len && i < 4 && n >= 0 && (size_t)n < size; i++)
        n += snprintf(p + n, size - n, " %02lx",
                      (unsigned long)(e->d >> (24 - 8 * i)) & 0xff);
    if (len > 4 && n >= 0 && (size_t)n < size)
        snprintf(p + n, size - n, " ...");
}
#endif

#ifdef USB_ENABLE_IAP
static void usb_log_format_iap2_event(const struct usb_log_entry *e, char *p,
                                      size_t size)
{
    const unsigned long c = e->c, d = e->d;
    switch (e->a)
    {
        case USB_LOG_IAP2_LINK_UP:
            snprintf(p, size, "iap2 link up: control session %lu, "
                     "window %lu", c, d);
            break;
        case USB_LOG_IAP2_LINK_LOST:
            snprintf(p, size, "iap2 LINK LOST: seq %lu unacknowledged "
                     "after %lu sends", c, d);
            break;
        case USB_LOG_IAP2_CERTIFICATE:
            snprintf(p, size, "iap2 car's certificate %lu bytes, "
                     "challenge %lu bytes", c, d);
            break;
        case USB_LOG_IAP2_IDENTIFIED:
            snprintf(p, size, "iap2 IDENTIFIED: car sends %lu messages, "
                     "takes %lu", c, d);
            break;
        case USB_LOG_IAP2_RATE:
            snprintf(p, size, "iap2 audio rate %lu Hz announced as %lu",
                     c, d);
            break;
        case USB_LOG_IAP2_NOW_PLAYING:
            snprintf(p, size, "iap2 now playing: track %08lx, state %lu, "
                     "%lu s", c, d >> 24, d & 0xffffff);
            break;
        case USB_LOG_IAP2_SEEK:
            snprintf(p, size, "iap2 car seeks to %lu ms", c);
            break;
        case USB_LOG_IAP2_HID:
            snprintf(p, size, "iap2 car's HID: %lu controls from a "
                     "%lu-byte descriptor", c, d);
            break;
        case USB_LOG_IAP2_BUTTON:
            snprintf(p, size, "iap2 car button: usage %02lx%s", c,
                     d ? "" : ", not mapped");
            break;
        case USB_LOG_IAP2_DROPPED:
            snprintf(p, size, "iap2 NOT SENT: %s (%04lx), %lu bytes",
                     usb_log_iap2_message(c), c, d);
            break;
        case USB_LOG_IAP2_POWER:
            snprintf(p, size, "iap2 power update %s, car asked for %08lx",
                     c ? "with attributes" : "empty", d);
            break;
        case USB_LOG_IAP2_AUDIO_START:
            snprintf(p, size, "audio: car stream starts, mixing at %lu Hz, "
                     "sending at %lu Hz", c, d);
            break;
        case USB_LOG_IAP2_AUDIO_TAKEN:
            snprintf(p, size, "audio: in %u ms the stream took %lu bytes "
                     "in %lu chunks", e->b, c, d);
            break;
        case USB_LOG_IAP2_AUDIO_GAPS:
            snprintf(p, size, "audio: %lu packets of silence, %u chunks "
                     "not whole samples, the track moved %ld ms",
                     c, e->b, (long)d);
            break;
        case USB_LOG_IAP2_AUDIO_USB:
#ifdef USB_BATCH_NON_NATIVE
            snprintf(p, size, "audio: %lu USB packets failed", d);
#else
            snprintf(p, size, "audio: the USB queue ran empty %lu times, "
                     "%lu USB packets failed", c, d);
#endif
            break;
        case USB_LOG_IAP2_RECONNECT:
            snprintf(p, size, "iap2: a car, so connecting again without the "
                     "sound card");
            break;
        case USB_LOG_IAP2_COVER:
        {
            static const char * const from[] =
                { "none", "embedded", "image file", "cache" };
            snprintf(p, size, "iap2 cover: %lu bytes from %s, ready in %lu ms",
                     c & 0xffffff, c >> 24 < 4 ? from[c >> 24] : "?", d);
            break;
        }
        default:
            snprintf(p, size, "iap2 event %d", e->a);
            break;
    }
}
#endif

/* us is the entry's stamp, widened by the caller where it can be. */
static void format_entry(const struct usb_log_entry *e, uint64_t us,
                         char *buf, size_t size)
{
    static const char * const eptypes[] = { "ctl", "iso", "bulk", "int" };
    static const char * const audio[] = {
        "init: NO BUFFERS, restart with the setting on", "stream start",
        "stream stop", "first packet", "prebuffered, playing",
        "UNDERFLOW", "OVERFLOW", "playback started on USB thread" };
    int n = snprintf(buf, size, "%4lu.%06lu ",
                     (unsigned long)(us / 1000000),
                     (unsigned long)(us % 1000000));
    char *p = buf + n;
    size -= n;

    switch (e->type)
    {
        case USB_LOG_INSERT:
            snprintf(p, size, "== cable in, %s",
                     e->a ? "charge only" : "mass storage");
            break;
        case USB_LOG_BUS_RESET:
            snprintf(p, size, "bus reset");
            break;
        case USB_LOG_SETUP:
            snprintf(p, size, "> %02x %02x %04lx %04lx %04lx %s%s",
                     e->a, e->b, (unsigned long)(e->c & 0xffff),
                     (unsigned long)(e->c >> 16),
                     (unsigned long)(e->d & 0xffff),
                     usb_log_request_name(e->a, e->b, e->c & 0xffff),
                     (e->d >> 16) & USB_LOG_SETUP_DROPPED ? " (dropped)" :
                     (e->d >> 16) & USB_LOG_SETUP_QUEUED ? " (queued)" : "");
            break;
        case USB_LOG_RESPONSE:
            if (e->a == USB_LOG_RESP_ACK && e->d != 0)
                snprintf(p, size, "< ack %lu, product %04lx",
                         (unsigned long)e->c, (unsigned long)e->d);
            else if (e->a == USB_LOG_RESP_ACK)
                snprintf(p, size, "< ack %lu", (unsigned long)e->c);
            else if (e->a == USB_LOG_RESP_STALL)
                snprintf(p, size, "< STALL");
            else if (e->a == USB_LOG_RESP_LATE)
                snprintf(p, size, "< DROPPED: no request waiting for it "
                         "(EP0 state %lu)", (unsigned long)e->c);
            else
                snprintf(p, size, "< STALL, data stage %lu too long",
                         (unsigned long)e->c);
            break;
        case USB_LOG_SET_ADDR:
            snprintf(p, size, "address %d (was %s)", e->a,
                     e->b == 0 ? "default" : e->b == 1 ? "addressed"
                                                      : "configured");
            break;
        case USB_LOG_ALLOC:
            snprintf(p, size, "assigning interfaces, drivers on %02x",
                     e->a);
            break;
        case USB_LOG_SET_CONFIG:
            snprintf(p, size, "configuration %d (%s speed)", e->a,
                     e->b ? "high" : "full");
            break;
        case USB_LOG_DRIVERS:
            snprintf(p, size, "drivers active %02x, failed %02x", e->a, e->b);
            break;
        case USB_LOG_EP_ALLOC:
            if (e->b & 0x7f)
                snprintf(p, size, "%s gets ep %02x %s",
                         usb_log_driver_name(e->a), e->b,
                         eptypes[e->c & 3]);
            else
                snprintf(p, size, "%s gets NO %s %s ep%s",
                         usb_log_driver_name(e->a), eptypes[e->c & 3],
                         e->b ? "in" : "out", e->d ? " (optional)" : "");
            break;
        case USB_LOG_DRV_DROPPED:
            snprintf(p, size, "%s LEFT OUT: not enough endpoints",
                     usb_log_driver_name(e->a));
            break;
        case USB_LOG_INTERFACES:
            snprintf(p, size, "%s interfaces %d-%ld",
                     usb_log_driver_name(e->a), e->b, (long)e->c - 1);
            break;
        case USB_LOG_EP_INIT:
            snprintf(p, size, "ep %02x set up: %s, max %lu, reg %08lx",
                     e->a, eptypes[e->b & 3], (unsigned long)e->c,
                     (unsigned long)e->d);
            break;
        case USB_LOG_ISO_XFER:
            snprintf(p, size, "ep %02x iso start, frame %d: %08lx %08lx",
                     e->a, e->b, (unsigned long)e->c, (unsigned long)e->d);
            break;
        case USB_LOG_XFER_FAIL:
            snprintf(p, size, "ep %02x START FAILED %d: %08lx %08lx",
                     e->a, -(int)e->b, (unsigned long)e->c,
                     (unsigned long)e->d);
            break;
        case USB_LOG_ISO_ERROR:
            snprintf(p, size, "ep %02x iso ERROR %08lx, %lu bytes",
                     e->a, (unsigned long)e->c, (unsigned long)e->d);
            break;
        case USB_LOG_ISO_PARITY:
            if (e->c)
                snprintf(p, size, "ep %02x iso: host took a packet on %s "
                         "microframes, %lu after arming", e->a,
                         e->b ? "odd" : "even", (unsigned long)e->d);
            else
                snprintf(p, size, "ep %02x iso: no packet taken in %lu "
                         "microframes, now sending on %s ones", e->a,
                         (unsigned long)e->d, e->b ? "even" : "odd");
            break;
        case USB_LOG_ALT:
            snprintf(p, size, "audio: interface %d alternate %d",
                     e->a, e->b);
            break;
        case USB_LOG_RATE:
            snprintf(p, size, "audio: rate %lu asked, %lu set",
                     (unsigned long)e->c, (unsigned long)e->d);
            break;
        case USB_LOG_AUDIO:
            n = snprintf(p, size, "audio: %s",
                         (size_t)e->a < ARRAYLEN(audio) ? audio[e->a] : "?");
            if (e->a == USB_LOG_AUDIO_START)
                snprintf(p + n, size - n, " at %lu", (unsigned long)e->c);
            else if (e->a == USB_LOG_AUDIO_FIRST_RX)
                snprintf(p + n, size - n, ", %lu bytes, status %ld",
                         (unsigned long)e->c, (long)(int32_t)e->d);
            break;
        case USB_LOG_STREAM:
            snprintf(p, size, "audio: %lu packets, %lu-%lu bytes, "
                     "%d errors, %d frames dropped",
                     (unsigned long)e->c, (unsigned long)(e->d & 0xffff),
                     (unsigned long)(e->d >> 16), e->b, e->a);
            break;
        case USB_LOG_FEEDBACK:
            snprintf(p, size, "audio: feedback %0*lx",
                     (int)e->d * 2, (unsigned long)e->c);
            break;
        case USB_LOG_HOST_PORT:
            snprintf(p, size, "host: port reset %d: HPRT %08lx HFIR %08lx",
                     e->a, (unsigned long)e->c, (unsigned long)e->d);
            break;
        case USB_LOG_HOST_STAGE:
            snprintf(p, size, "host: @%d %s %s%s try %d: int %08lx "
                     "tsiz %08lx", e->a, (e->b & 0xff) == 3 ? "SETUP" :
                     (e->b & 0xff) == 2 ? "DATA1" : "DATA0",
                     e->b & 0x100 ? "in" : "out",
                     e->b & 0x200 ? " start split" :
                     e->b & 0x400 ? " complete split" : "", e->b >> 12,
                     (unsigned long)e->c, (unsigned long)e->d);
            break;
        case USB_LOG_HOST_ENUM:
            snprintf(p, size, "host: %s%s %s (%08lx)",
                     (const char *)(uintptr_t)e->c,
                     e->b == 1 ? " left" : e->b == 2 ? " right" : "",
                     e->a ? "ok" : "FAILED", (unsigned long)e->d);
            break;
        case USB_LOG_HOST_DEVICE:
            snprintf(p, size, "host: device %04lx:%04lx class "
                     "%02x/%02lx/%02lx, %lu configuration%s, %d bytes%s",
                     (unsigned long)(e->c >> 16),
                     (unsigned long)(e->c & 0xffff), e->a,
                     (unsigned long)(e->d & 0xff),
                     (unsigned long)((e->d >> 8) & 0xff),
                     (unsigned long)((e->d >> 16) & 0xff),
                     ((e->d >> 16) & 0xff) == 1 ? "" : "s", e->b,
                     e->d >> 24 ? " (cut short)" : "");
            break;
        case USB_LOG_HOST_IFACE:
            snprintf(p, size, "host:   IF%d.%d %02lx/%02lx/%02lx, "
                     "%d endpoints", e->a, e->b & 0xff,
                     (unsigned long)(e->c & 0xff),
                     (unsigned long)((e->c >> 8) & 0xff),
                     (unsigned long)((e->c >> 16) & 0xff), e->b >> 8);
            break;
        case USB_LOG_HOST_EP:
        {
            static const char * const sync[] =
                { "", " async", " adaptive", " sync" };
            static const char * const usage[] =
                { "", " feedback", " implicit", " ?" };
            snprintf(p, size, "host:     ep %02x %s%s%s, %lu bytes, "
                     "interval %lu", e->a, eptypes[e->b & 3],
                     (e->b & 3) == 1 ? sync[(e->b >> 2) & 3] : "",
                     usage[(e->b >> 4) & 3],
                     (unsigned long)(e->c & 0x7ff), (unsigned long)e->d);
            break;
        }
        case USB_LOG_HOST_STREAM:
            snprintf(p, size, "host: stream IF%d.%d, %s speed, %lu.%03lu "
                     "frames a %sframe, max %lu bytes every %lu",
                     e->a, e->b, e->d >> 24 ? "high" : "full",
                     (unsigned long)(e->c >> 16),
                     (unsigned long)(((e->c & 0xffff) * 1000) >> 16),
                     e->d >> 24 ? "micro" : "",
                     (unsigned long)(e->d & 0xffff),
                     (unsigned long)((e->d >> 16) & 0xff));
            break;
        case USB_LOG_HOST_SETTING:
        {
            static const char * const why[] = {
                "no OUT endpoint", "not type I", "not PCM", "no channels",
                "sample not 2-4 bytes", "interval too long",
                "no 44.1 or 48 kHz" };
            n = snprintf(p, size, "host: DAC setting IF%d.%d UAC%lu "
                         "%luch %lu/%lu bit:", e->a, e->b,
                         (unsigned long)(e->d >> 24),
                         (unsigned long)(e->d & 0xff),
                         (unsigned long)((e->d >> 16) & 0xff),
                         (unsigned long)((e->d >> 8) & 0xff) * 8);
            if (e->c == 0)
                snprintf(p + n, size - n, " usable");
            for (int i = 0; i < (int)ARRAYLEN(why) && n < (int)size; i++)
                if (e->c & (1u << i))
                    n += snprintf(p + n, size - n, "%s %s",
                                  e->c & ((1u << i) - 1) ? "," : "",
                                  why[i]);
            break;
        }
        case USB_LOG_HOST_EHCI:
            snprintf(p, size, "host: @%d req %02x port %d: %ld bytes, "
                     "token %08lx", e->a, e->b & 0xff, e->b >> 8,
                     (long)(int32_t)e->d, (unsigned long)e->c);
            break;
        case USB_LOG_HOST_ISO:
            if (e->a == 0)
                snprintf(p, size, "host: iso packet (%s): int %08lx "
                         "tsiz %08lx", e->b ? "odd" : "even",
                         (unsigned long)e->c, (unsigned long)e->d);
            else
                snprintf(p, size, "host: iso %lu sent, %lu underruns, "
                         "%lu errors, %d frames last",
                         (unsigned long)e->c, (unsigned long)(e->d & 0xffff),
                         (unsigned long)(e->d >> 16), e->b);
            break;
        case USB_LOG_HOST_MODE:
            snprintf(p, size, "== host mode %s", e->a ? "on" : "off");
            break;
        case USB_LOG_HOST_DAC:
            if (e->b & USB_LOG_HOST_PORT_READ)
                snprintf(p, size, "host: DAC search, poll %d: %s; port: "
                         "VBUS %s, %s, %s (%08lx)", e->a,
                         (const char *)(uintptr_t)e->c,
                         e->b & 1 ? "yes" : "no",
                         e->b & 2 ? "connected" : "nothing connected",
                         e->b & 4 ? "enabled" : "not enabled",
                         (unsigned long)e->d);
            else
                snprintf(p, size, "host: DAC search: %s",
                         (const char *)(uintptr_t)e->c);
            break;
        case USB_LOG_OUTPUT:
            if (e->a == USB_LOG_OUTPUT_SERIAL)
                snprintf(p, size, "output: serial accessory reset, "
                         "departed %d, lingoes %08lx, interface %lu", e->b,
                         (unsigned long)e->c, (unsigned long)e->d);
            else
                snprintf(p, size, "output: %s %s, %s (setting %lu, "
                         "status %lu)",
                         e->b == 0 ? "headphones" : e->b == 1 ? "DAC" :
                         "accessory",
                         e->a == USB_LOG_OUTPUT_GONE ? "gone" :
                         e->a == USB_LOG_OUTPUT_BACK ? "back" : "resume",
                         e->c ? (e->a == USB_LOG_OUTPUT_GONE ? "paused" :
                                 "resumed") : "left alone",
                         (unsigned long)(e->d & 0xff),
                         (unsigned long)(e->d >> 8));
            break;
        case USB_LOG_HOST_HUB:
            snprintf(p, size, "host: hub port %d %s: status %04lx "
                     "change %04lx", e->a, e->b == 0 ? "looking" :
                     e->b == 1 ? "resetting" : "watching",
                     (unsigned long)(e->c & 0xffff),
                     (unsigned long)(e->c >> 16));
            break;
#ifdef USB_ENABLE_IAP
        case USB_LOG_IAP:
            usb_log_format_iap(e, p, size);
            break;
        case USB_LOG_IAP_RATE:
            if (e->b == 2)
            {
                snprintf(p, size, "iap: accessory lists no rates, playing "
                         "the %lu Hz the host set%s", (unsigned long)e->c,
                         e->d ? ", though it is still talking" : "");
                break;
            }
            if (e->b)
            {
                snprintf(p, size, "iap: host sets no rate, playing the "
                         "%lu Hz announced", (unsigned long)e->c);
                break;
            }
            n = snprintf(p, size, "iap: host sets %lu Hz",
                         (unsigned long)e->c);
            if (e->d)
                snprintf(p + n, size - n, ", %lu announced%s",
                         (unsigned long)e->d, e->a ? ": IGNORED" : "");
            break;
        case USB_LOG_IAP_RATES:
            snprintf(p, size, "iap: accessory lists %u rates, of ours%s%s%s"
                     "%s: %s", e->b, e->c & 1 ? " 48000" : "",
                     e->c & 2 ? " 44100" : "", e->c & 4 ? " 32000" : "",
                     e->c & 7 ? "" : " none",
                     e->a ? "accepted" : "REFUSED");
            break;
        case USB_LOG_IAP_STREAM:
            snprintf(p, size, "iap: stream now %s",
                     e->a == USB_LOG_IAP_STREAM_AUDIO ? "carries audio" :
                     e->a == USB_LOG_IAP_STREAM_NOTHING
                         ? "silent, nothing playing"
                         : "silent, waiting for the host to set the rate");
            break;
        case USB_LOG_IAP_ART:
        {
            static const char *const what[] = {
                "playing", "art sent", "art asked for, none sent"
            };
            static const char *const cache[] = {
                "a thumbnail", "Accessory Artwork off", "no track or size",
                "no thumbnail", "an unusable thumbnail"
            };
            const bool has = e->c == USB_LOG_IAP_ART_CACHE_OK || e->d;
            snprintf(p, size, "iap: track %u %s%s; art cache: %s%s", e->b,
                     e->a < 3 ? what[e->a] : "?",
                     e->a == USB_LOG_IAP_ART_REPORTED
                         ? (has ? ", told it has art" : ", told it has none")
                         : "",
                     e->c < 5 ? cache[e->c] : "?",
                     e->d ? ", playback's art used" : "");
            break;
        }
        case USB_LOG_IAP_RUN:
        {
            int lingo = e->a & ~USB_LOG_IAP_FROM_PLAYER;
            char trans[8] = "";

            if (e->c >> 16 != 0xffff)
                snprintf(trans, sizeof trans, " #%lu",
                         (unsigned long)(e->c >> 16));
            snprintf(p, size, "iap < %s:%s%s: %lu more like it, the last "
                     "%02lx %02lx %02lx %02lx", usb_log_iap_lingo(lingo),
                     usb_log_iap_command(lingo, e->b), trans,
                     (unsigned long)(e->c & 0xffff),
                     (unsigned long)(e->d >> 24),
                     (unsigned long)(e->d >> 16 & 0xff),
                     (unsigned long)(e->d >> 8 & 0xff),
                     (unsigned long)(e->d & 0xff));
            break;
        }
        case USB_LOG_IAP_REPORT:
            if (e->a)
                snprintf(p, size, "iap < report read by the host after "
                         "%lu ms", (unsigned long)e->d);
            else
                snprintf(p, size, "iap < report NOT READ by the host for "
                         "%lu ms", (unsigned long)e->d);
            break;
        case USB_LOG_IAP_IDENTIFY:
        {
            static const char *const auth[] = {
                "none", "deferred", "immediate", "reserved"
            };
            snprintf(p, size, "iap: lingoes %08lx, options %08lx, "
                     "authentication %s", (unsigned long)e->c,
                     (unsigned long)e->d, auth[e->d & 3]);
            break;
        }
        case USB_LOG_IAP2:
            snprintf(p, size, "iap2 %c %u bytes +%d: %08lx %08lx",
                     e->a & USB_LOG_IAP_FROM_PLAYER ? '<' : '>', e->b,
                     e->a & ~USB_LOG_IAP_FROM_PLAYER,
                     (unsigned long)e->c, (unsigned long)e->d);
            break;
        case USB_LOG_IAP2_SENT:
            snprintf(p, size, "iap2 < sent, status %ld, %lu bytes",
                     (long)(int32_t)e->c, (unsigned long)e->d);
            break;
        case USB_LOG_IAP2_PACKET:
        {
            static const char *const status[] = {
                "", ", BAD CHECKSUM", ", LONGER THAN OFFERED", ", again",
            };
            const unsigned control = e->c >> 24;
            const int s = e->a & ~USB_LOG_IAP_FROM_PLAYER;
            snprintf(p, size, "iap2 %c %s%s%s%s%sseq %lu ack %lu session %lu"
                     ", %u bytes%s",
                     e->a & USB_LOG_IAP_FROM_PLAYER ? '<' : '>',
                     control & 0x80 ? "SYN " : "", control & 0x40 ? "ACK " : "",
                     control & 0x20 ? "EAK " : "", control & 0x10 ? "RST " : "",
                     control & 0xf0 ? "" : "- ",
                     (unsigned long)(e->c >> 16 & 0xff),
                     (unsigned long)(e->c >> 8 & 0xff),
                     (unsigned long)(e->c & 0xff), e->b,
                     s < (int)ARRAYLEN(status) ? status[s] : "");
            break;
        }
        case USB_LOG_IAP2_MSG:
            snprintf(p, size, "iap2 %c %s (%04lx), %u bytes",
                     e->a & USB_LOG_IAP_FROM_PLAYER ? '<' : '>',
                     usb_log_iap2_message(e->c), (unsigned long)e->c, e->b);
            break;
        case USB_LOG_IAP2_PARAMS:
        {
            /* Hex, then the same bytes as text, to read names in place. */
            char text[9];
            for (int i = 0; i < 8; i++)
            {
                unsigned ch = (i < 4 ? e->c >> (24 - 8 * i)
                                     : e->d >> (56 - 8 * i)) & 0xff;
                text[i] = i >= e->a ? ' ' : ch >= 0x20 && ch < 0x7f ? ch : '.';
            }
            text[8] = '\0';
            snprintf(p, size, "iap2     +%-3u %08lx %08lx  %s", e->b,
                     (unsigned long)e->c, (unsigned long)e->d, text);
            break;
        }
        case USB_LOG_IAP2_EVENT:
            usb_log_format_iap2_event(e, p, size);
            break;
        case USB_LOG_IAP_ERROR:
        {
            const char *fmt = (const char *)(uintptr_t)e->d;

            /* libiap's warn() puts its own location first */
            if (!strncmp(fmt, "%s:%d: ", 7))
                fmt += 7;
            snprintf(p, size, "iap ERROR %s:%u: %s",
                     (const char *)(uintptr_t)e->c, e->b, fmt);
            break;
        }
#endif
        case USB_LOG_SYNC:
            snprintf(p, size, "log on disk after %lu ms%s",
                     (unsigned long)e->c / 1000, e->a ? ", TIMED OUT" : "");
            break;
        case USB_LOG_HANG:
            if (e->a == 0)
            {
                char name[9];

                for (int i = 0; i < 8; i++)
                {
                    unsigned ch = (i < 4 ? e->c >> (24 - 8 * i)
                                         : e->d >> (56 - 8 * i)) & 0xff;
                    name[i] = ch >= 0x20 && ch < 0x7f ? ch : '\0';
                }
                name[8] = '\0';
                snprintf(p, size, "HANG: thread \"%s\" held the core",
                         name);
            }
            else if (e->a == 1)
                snprintf(p, size, "HANG: pc %08lx lr %08lx",
                         (unsigned long)e->c, (unsigned long)e->d);
            else
                snprintf(p, size, "HANG: stack %u: %08lx %08lx", e->b,
                         (unsigned long)e->c, (unsigned long)e->d);
            break;
        default:
            snprintf(p, size, "? type %d", e->type);
            break;
    }
}

void usb_log_file_format(const struct usb_log_entry *e, char *buf,
                         size_t size)
{
    format_entry(e, e->us, buf, size);
}

/* ---- the file ----------------------------------------------------------- */

/* The entries' 32-bit stamps wrap every 71.6 minutes; the file carries the
 * wraps so its times keep rising through a long car session. A gap of more
 * than 71 minutes between two written entries hides a wrap. */
static uint32_t last_us;
static uint32_t us_wraps;

static uint64_t widen_us(uint32_t us)
{
    if (us < last_us)
        us_wraps++;
    last_us = us;
    return (uint64_t)us_wraps << 32 | us;
}

void usb_log_file_write(void)
{
    unsigned long seq, head;
    struct usb_log_entry e;
    char line[112];
    int fd;

    /* The log's lock is held from open to close: the screen writes from the
     * UI thread and the writer from its own, and neither may append lines
     * the other is about to write again. Read again under it. */
    if (usb_log_written() == usb_log_head())
        return;
    fd = debug_log_open(DEBUG_LOG_USB);
    if (fd < 0)
        return;
    seq = usb_log_written();
    head = usb_log_head();
    if (head - seq > USB_LOG_SIZE)
    {
        debug_log_lost(DEBUG_LOG_USB, fd, head - seq - USB_LOG_SIZE, NULL);
        seq = head - USB_LOG_SIZE;
    }
    for (; seq < head; seq++)
    {
        if (!usb_log_read(seq, &e))
            continue;
        format_entry(&e, widen_us(e.us), line, sizeof line);
        fdprintf(fd, "%s\n", line);
    }
    usb_log_set_written(head);
    debug_log_close(DEBUG_LOG_USB, fd);
}

void usb_log_file_state(int fd, const char *mark)
{
    fdprintf(fd, "%s USB mode %d, USB HID %d", mark, global_settings.usb_mode,
             (int)global_settings.usb_hid);
#ifdef USB_ENABLE_AUDIO
    fdprintf(fd, ", USB Sound Card %d%s", global_settings.usb_audio,
             usb_audio_buffers_ready() ? "" : " (no buffers)");
#endif
#ifdef HAVE_USB_HOST_AUDIO
    fdprintf(fd, ", USB DAC Output %d", global_settings.usb_dac_output);
#endif
    /* What a dock or a car is offered */
    fdprintf(fd, "\n%s Accessory Protocol %d", mark,
             (int)global_settings.iap_enabled);
#ifdef USB_ENABLE_IAP
    fdprintf(fd, ", Support iPhone Accessories %d, Identify As %d",
             global_settings.iap2_mode, global_settings.iap_identity);
#endif
    fdprintf(fd, ", Accessory Artwork %d\n", global_settings.car_artwork);
}

/* ---- the background writer ---------------------------------------------- */

#define WRITER_STACK_SIZE (DEFAULT_STACK_SIZE + 0x800)
static long writer_stack[WRITER_STACK_SIZE / sizeof(long)];
static const char writer_name[] = "usb log";
static struct event_queue writer_queue;

#define Q_WRITER_WAKE 1

static bool started;            /* usb_log_file_init() has run */
static bool screen_open;
static bool writer_parked;
static bool have_writer;

/* usb_log_sync() waits only while something is draining the ring. The writer
 * stops draining while a host has the disk, and a sync then would stall the
 * USB thread for its full timeout. */
static void update_attach(void)
{
    usb_log_attach(screen_open
                   || (global_settings.debug_log_usb && have_writer
                       && !writer_parked));
}

static void writer_thread(void)
{
    struct queue_event ev;

    while (1)
    {
        /* Polled rather than signalled: usb_log() runs in interrupt context
         * and cannot post. The tenth of a second bounds how long a sync
         * waits on it. */
        queue_wait_w_tmo(&writer_queue, &ev, global_settings.debug_log_usb
                                             ? HZ/10 : TIMEOUT_BLOCK);

        if (ev.id == SYS_USB_CONNECTED)
        {
            /* Everything up to the handover goes on disk first. */
            if (global_settings.debug_log_usb)
                usb_log_file_write();
            writer_parked = true;
            update_attach();
            usb_acknowledge(SYS_USB_CONNECTED_ACK, ev.data);
            usb_wait_for_disconnect(&writer_queue);
            writer_parked = false;
            update_attach();
        }
        else if (global_settings.debug_log_usb)
            usb_log_file_write();
    }
}

/* ---- the screen's and the setting's hooks ------------------------------- */

void usb_log_file_screen(bool open)
{
    if (open)
        debug_log_header(DEBUG_LOG_USB, "opened");
    screen_open = open;
    update_attach();
}

void usb_log_file_enable(bool on)
{
    (void)on;
    if (!started)
        return;
    update_attach();
    if (have_writer)
        queue_post(&writer_queue, Q_WRITER_WAKE, 0);
}

void usb_log_file_init(void)
{
    queue_init(&writer_queue, true);

    /* Above the background workers, so a sync is not left waiting behind a
     * database pass. Without a thread the queue leaves the broadcast list,
     * or the disk handover waits for an ack nobody sends. */
    if (!create_thread(writer_thread, writer_stack, sizeof(writer_stack), 0,
                       writer_name IF_PRIO(, PRIORITY_SYSTEM)
                       IF_COP(, CPU)))
        queue_delete(&writer_queue);
    else
        have_writer = true;
    started = true;
    /* Here rather than in debug_log_init(): the sound card's buffers exist
     * now, for the header to report */
    if (global_settings.debug_log_usb)
        debug_log_header(DEBUG_LOG_USB, "started");
    update_attach();
}
