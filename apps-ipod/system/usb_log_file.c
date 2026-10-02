/***************************************************************************
 * GNU General Public License (version 2+)
 * was: apps-ipod/screens/system/debug_menu.c (the USB Log screen's file)
 *
 * See usb_log_file.h. The ring lives in firmware (usb_log.c); this is the
 * side that turns it into text and puts it on disk.
 *
 * Parts: the entry formatter; the file (one writer at a time, under
 * file_mutex); the background writer thread; the screen's and the setting's
 * hooks.
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
#include "version.h"
#include "settings/settings.h"
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
    int lingo = e->a & ~USB_LOG_IAP_FROM_PLAYER;
    const char *name = usb_log_iap_command(lingo, e->b);
    unsigned len = e->c & 0xffff, trans = e->c >> 16;
    int n = snprintf(p, size, "iap %c %s:%s", e->a & USB_LOG_IAP_FROM_PLAYER
                     ? '<' : '>', usb_log_iap_lingo(lingo), name);
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
        default:
            snprintf(p, size, "iap2 event %d", e->a);
            break;
    }
}
#endif

void usb_log_file_format(const struct usb_log_entry *e, char *buf,
                         size_t size)
{
    static const char * const eptypes[] = { "ctl", "iso", "bulk", "int" };
    static const char * const audio[] = {
        "init: NO BUFFERS, restart with the setting on", "stream start",
        "stream stop", "first packet", "prebuffered, playing",
        "UNDERFLOW", "OVERFLOW", "playback started on USB thread" };
    int n = snprintf(buf, size, "%4lu.%06lu ",
                     (unsigned long)(e->us / 1000000),
                     (unsigned long)(e->us % 1000000));
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
            if (e->a == USB_LOG_RESP_ACK)
                snprintf(p, size, "< ack %lu", (unsigned long)e->c);
            else if (e->a == USB_LOG_RESP_STALL)
                snprintf(p, size, "< STALL");
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
            snprintf(p, size, "host: @%d %s %s try %d: int %08lx tsiz %08lx",
                     e->a, (e->b & 0xff) == 3 ? "SETUP" :
                     (e->b & 0xff) == 2 ? "DATA1" : "DATA0",
                     e->b & 0x100 ? "in" : "out", e->b >> 12,
                     (unsigned long)e->c, (unsigned long)e->d);
            break;
        case USB_LOG_HOST_ENUM:
            snprintf(p, size, "host: %s%s %s (%08lx)",
                     (const char *)(uintptr_t)e->c,
                     e->b == 1 ? " left" : e->b == 2 ? " right" : "",
                     e->a ? "ok" : "FAILED", (unsigned long)e->d);
            break;
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
#ifdef USB_ENABLE_IAP
        case USB_LOG_IAP:
            usb_log_format_iap(e, p, size);
            break;
        case USB_LOG_IAP_RATE:
            n = snprintf(p, size, "iap: host sets %lu Hz",
                         (unsigned long)e->c);
            if (e->d)
                snprintf(p + n, size - n, ", %lu announced%s",
                         (unsigned long)e->d, e->a ? ": IGNORED" : "");
            break;
        case USB_LOG_IAP_STREAM:
            snprintf(p, size, "iap: stream now %s",
                     e->a == USB_LOG_IAP_STREAM_AUDIO ? "carries audio" :
                     e->a == USB_LOG_IAP_STREAM_NOTHING
                         ? "silent, nothing playing"
                         : "silent, waiting for the host to set the rate");
            break;
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
#endif
        case USB_LOG_SYNC:
            snprintf(p, size, "log on disk after %lu ms%s",
                     (unsigned long)e->c / 1000, e->a ? ", TIMED OUT" : "");
            break;
        default:
            snprintf(p, size, "? type %d", e->type);
            break;
    }
}

/* ---- the file ----------------------------------------------------------- */

/* The screen writes from the UI thread and the writer from its own; the lock
 * keeps one from appending lines the other is about to write again. */
static struct mutex file_mutex;

void usb_log_file_write(void)
{
    unsigned long seq, head;
    struct usb_log_entry e;
    char line[112];
    int fd;

    mutex_lock(&file_mutex);
    seq = usb_log_written();
    head = usb_log_head();
    if (seq == head)
        goto out;
    fd = open(USB_LOG_FILE, O_WRONLY|O_CREAT|O_APPEND, 0666);
    if (fd < 0)
        goto out;
    if (head - seq > USB_LOG_SIZE)
    {
        fdprintf(fd, "-- %lu entries lost --\n", head - seq - USB_LOG_SIZE);
        seq = head - USB_LOG_SIZE;
    }
    for (; seq < head; seq++)
    {
        if (!usb_log_read(seq, &e))
            continue;
        usb_log_file_format(&e, line, sizeof line);
        fdprintf(fd, "%s\n", line);
    }
    close(fd);
    usb_log_set_written(head);
out:
    mutex_unlock(&file_mutex);
}

/* Starts each session in the file: what opened it, and the settings that
 * decide which drivers the host is offered. */
static void write_header(const char *what)
{
    int fd;

    mutex_lock(&file_mutex);
    fd = open(USB_LOG_FILE, O_WRONLY|O_CREAT|O_APPEND, 0666);
    if (fd >= 0)
    {
        fdprintf(fd, "== USB Log %s: %s %s, USB mode %d, USB HID %d",
                 what, MODEL_NAME, rbversion, global_settings.usb_mode,
                 (int)global_settings.usb_hid);
#ifdef USB_ENABLE_AUDIO
        fdprintf(fd, ", USB Sound Card %d%s", global_settings.usb_audio,
                 usb_audio_buffers_ready() ? "" : " (no buffers)");
#endif
#ifdef HAVE_USB_HOST_AUDIO
        fdprintf(fd, ", USB DAC Output %d", global_settings.usb_dac_output);
#endif
        fdprintf(fd, "\n");
        close(fd);
    }
    mutex_unlock(&file_mutex);
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
        write_header("opened");
    screen_open = open;
    update_attach();
}

void usb_log_file_enable(bool on)
{
    /* settings_load() calls this before init, which applies the setting
     * once the sound card's buffers exist for the header to report */
    if (!started)
        return;
    if (on)
        write_header("switched on");
    update_attach();
    if (have_writer)
        queue_post(&writer_queue, Q_WRITER_WAKE, 0);
}

void usb_log_file_init(void)
{
    mutex_init(&file_mutex);
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
    if (global_settings.debug_log_usb)
        write_header("started");
    update_attach();
}
