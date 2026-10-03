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

/* iAP2 over the iAP configuration's HID interface, with the player in an
 * iPhone's place: the transport and the link layer. The control session's
 * messages are usb_iap2_control.c's. An accessory asks whether the device
 * speaks iAP2 by sending the probe below, which is also a well-formed iAP1
 * packet that libiap refuses as an opening. With the answer switched on, the
 * probe is echoed as an iPhone does, and every report after it comes here
 * instead of to libiap. Everything here runs on the USB thread.
 *
 * Parts, in file order:
 * - logging: the start of each packet's first report raw, and each packet
 *   decoded;
 * - sending: one packet at a time, split into the smallest input reports
 *   that hold it. The echo and the SYN+ACK are flags, built when the
 *   endpoint is free; the player's data packets wait in a ring of slots
 *   until acknowledged, and each carries the acknowledgement of the last
 *   accessory packet taken. There is no bare acknowledgement: an iPhone
 *   never sends one, and this car stops reading the HID endpoint after one;
 * - the link layer: packets start FF 5A, with a 9-byte header (length,
 *   control bits, sequence, acknowledgement, session, header checksum) and a
 *   checksummed payload. The accessory opens with a SYN carrying its link
 *   parameters; the player answers SYN+ACK with the same parameters, the
 *   packet length capped at what it can take, as the phone side does. Once
 *   the accessory acknowledges that, the link is up. Accessory packets are
 *   acknowledged on the player's next packet; the player's are sent again
 *   only as the accessory's link parameters allow, which for this car is
 *   never;
 * - receiving: reports reassembled into packets, and the control session's
 *   packets into messages.
 *
 * Packet layout from lvalen91/carplayd (public domain), pi/iap2_pi.c;
 * .specifications/carplay-provenance.md gives the source of every fact. */

#include <string.h>
#include "system.h"
#include "kernel.h"
#include "usb.h"
#include "usb_core.h"
#include "usb_drv.h"
#include "usb_log.h"

#include "iap/libiap/spec/hid.h"
#include "iap/macros.h"
#include "usb_iap.h"
#include "usb_iap2.h"

static const uint8_t probe[] = {0xFF, 0x55, 0x02, 0x00, 0xEE, 0x10};

#define SOP1       0xFF
#define SOP2       0x5A
#define CTL_SYN    0x80
#define CTL_ACK    0x40
#define CTL_EAK    0x20
#define CTL_RST    0x10
#define HEADER_LEN 9
#define SYN_FIXED  10   /* link parameters ahead of the session list */

/* The longest packet the player accepts, offered in its SYN+ACK, and the
 * longest it sends. Holds the accessory's certificate, about 950 bytes, in
 * one packet. */
#define MAX_PACKET 1024

#define SLOTS   8       /* the player's packets awaiting acknowledgement */

static int mode = USB_IAP2_MODE_AUTO;
static bool car_seen;       /* this host has sent Apple's 0x53 */
static bool seen;           /* the probe has been answered */

/* ---- link state --------------------------------------------------------- */

static bool synced;         /* the accessory's SYN has been answered */
static bool link_up;        /* ... and the answer acknowledged */
static uint8_t my_seq;      /* the player's last sequence number used */
static uint8_t syn_seq;     /* the SYN+ACK's */
static uint8_t peer_seq;    /* the accessory's, last packet taken in order */
static uint8_t syn_ack[48]; /* the SYN+ACK's payload */
static size_t syn_ack_len;
static uint8_t control_session;
static uint8_t file_session;      /* the file transfer session, or 0 */
static unsigned window;     /* the accessory's outstanding packets, capped */
static long rto;            /* retransmission timeout, ticks; 0 never */
static unsigned max_sends;  /* sends of one packet before giving up */

struct slot
{
    uint8_t packet[MAX_PACKET];
    size_t len;
    bool queued;            /* to go out, first time or again */
    uint8_t tries;
    long sent;
};
static struct slot slots[SLOTS];
static unsigned head, count; /* the ring: oldest unacknowledged, and how many */

/* The control session's bytes, until a whole message is in. */
static uint8_t cs[MAX_PACKET + 256];
static size_t cs_len;

enum
{
    SEND_ECHO    = 1 << 0,
    SEND_SYN_ACK = 1 << 1,
};
static unsigned pending;
static uint8_t ctl[HEADER_LEN + sizeof(syn_ack) + 1];
static const uint8_t *tx;
static size_t tx_len, tx_off;
static bool tx_busy;
static struct slot *tx_slot;      /* the data packet going out, or NULL */
static long tx_started;           /* when the report in flight was queued */
static bool tx_stuck_logged;

/* Logged as a report's status when it has not completed after a second: the
 * host has stopped collecting the interrupt endpoint, or the controller has
 * lost the transfer. No driver returns it. */
#define TX_STUCK (-1000)
/* Sent by DMA: the controller drops an IN buffer's low address bits, so an
 * unaligned one goes out shifted. */
static uint8_t report[96] USB_DEVBSS_ATTR __attribute__((aligned(32)));

static uint8_t rx[MAX_PACKET + 256];
static size_t rx_len;

bool usb_iap_answer_iap2(void)
{
    return mode == USB_IAP2_MODE_ON ||
           (mode == USB_IAP2_MODE_AUTO && car_seen);
}

void usb_iap_set_iap2_mode(int m)
{
    mode = m;
}

/* Not with Accessory Protocol off: the probe would hold the disk back from a
 * car that is then offered no iAP configuration to use instead */
bool usb_iap2_offered(void)
{
    return usb_core_driver_enabled(USB_DRIVER_IAP) &&
           mode != USB_IAP2_MODE_OFF;
}

void usb_iap2_host_is_car(void)
{
    car_seen = true;
}

void usb_iap2_host_new(void)
{
    car_seen = false;
}

/* ---- logging ------------------------------------------------------------ */

/* A report from its link-control byte on, eight bytes an entry, up to 16. */
static void log_report(const uint8_t *r, size_t len, bool from_player)
{
    const uint8_t *data = r + 1;
    const size_t avail = len - 1;
    for (size_t off = 0; off < MIN(avail, 16); off += 8)
    {
        uint32_t c = 0, d = 0;
        for (size_t i = 0; i < 4; i++)
        {
            c = c << 8 | (off + i < avail ? data[off + i] : 0);
            d = d << 8 | (off + 4 + i < avail ? data[off + 4 + i] : 0);
        }
        usb_log(USB_LOG_IAP2,
                off | (from_player ? USB_LOG_IAP_FROM_PLAYER : 0), len, c, d);
    }
}

static void log_packet(const uint8_t *p, size_t len, int status,
                       bool from_player)
{
    usb_log(USB_LOG_IAP2_PACKET,
            status | (from_player ? USB_LOG_IAP_FROM_PLAYER : 0), len,
            (uint32_t)p[4] << 24 | p[5] << 16 | p[6] << 8 | p[7], 0);
}

/* ---- sending ------------------------------------------------------------ */

struct report_size
{
    uint8_t id;
    uint16_t size; /* from the link-control byte */
};

/* As usb_iap.c's report descriptors declare the input reports. */
static const struct report_size in_hs[] = {
    {0x01, 0x05}, {0x02, 0x09}, {0x03, 0x0D}, {0x04, 0x11},
    {0x05, 0x19}, {0x06, 0x31}, {0x07, 0x5F},
};
static const struct report_size in_fs[] = {
    {0x01, 0x0C}, {0x02, 0x0E}, {0x03, 0x14}, {0x04, 0x3F},
};

/* The smallest report holding n bytes, else the largest. Both tables stop
 * at the largest report that fits the buffer. */
static const struct report_size *report_for(size_t n)
{
    const bool hs = usb_drv_port_speed();
    const struct report_size *t = hs ? in_hs : in_fs;
    const size_t count = hs ? ARRAYLEN(in_hs) : ARRAYLEN(in_fs);
    for (size_t i = 0; i < count; i++)
        if (t[i].size >= n + 1)
            return &t[i];
    return &t[count - 1];
}

static uint8_t checksum(const uint8_t *b, size_t n)
{
    uint8_t sum = 0;
    while (n--)
        sum += *b++;
    return -sum;
}

static void put_header(uint8_t *p, size_t len, uint8_t control, uint8_t seq,
                       uint8_t session)
{
    p[0] = SOP1;
    p[1] = SOP2;
    p[2] = len >> 8;
    p[3] = len;
    p[4] = control;
    p[5] = seq;
    p[6] = peer_seq;
    p[7] = session;
    p[8] = checksum(p, 8);
}

static void build_ctl(uint8_t control, const uint8_t *payload, size_t n)
{
    tx_len = HEADER_LEN + (n ? n + 1 : 0);
    put_header(ctl, tx_len, control, my_seq, 0);
    if (n)
    {
        memcpy(ctl + HEADER_LEN, payload, n);
        ctl[HEADER_LEN + n] = checksum(payload, n);
    }
    tx = ctl;
    log_packet(ctl, tx_len, USB_LOG_IAP2_OK, true);
}

/* The first queued packet inside the accessory's window. With no
 * retransmission there is nothing to keep a packet for: it leaves the ring
 * once sent, and the window does not hold the next back -- an iPhone has had
 * 148 packets unacknowledged by this car at once. */
static struct slot *next_queued(void)
{
    const unsigned limit = rto ? MIN(count, window) : count;
    for (unsigned i = 0; i < limit; i++)
    {
        struct slot *s = &slots[(head + i) % SLOTS];
        if (s->queued)
            return s;
    }
    return NULL;
}

static void send_next(void)
{
    struct slot *s;
    if (tx_busy)
        return;
    if (tx_off >= tx_len)
    {
        tx_off = 0;
        tx_len = 0;
        if (pending & SEND_ECHO)
        {
            pending &= ~SEND_ECHO;
            tx = probe;
            tx_len = sizeof(probe);
        }
        else if (pending & SEND_SYN_ACK)
        {
            pending &= ~SEND_SYN_ACK;
            build_ctl(CTL_SYN | CTL_ACK, syn_ack, syn_ack_len);
        }
        else if (link_up && (s = next_queued()) != NULL)
        {
            s->packet[6] = peer_seq;
            s->packet[8] = checksum(s->packet, 8);
            s->queued = false;
            s->tries++;
            s->sent = current_tick;
            tx = s->packet;
            tx_len = s->len;
            tx_slot = s;
            log_packet(tx, tx_len, s->tries > 1 ? USB_LOG_IAP2_AGAIN
                                                : USB_LOG_IAP2_OK, true);
        }
        else
            return;
    }

    const struct report_size *rs = report_for(tx_len - tx_off);
    const size_t take = MIN(rs->size - 1u, tx_len - tx_off);
    struct IAPHIDReport *r = (struct IAPHIDReport *)report;
    memset(report, 0, 1 + rs->size);
    r->report_id = rs->id;
    r->link_control =
        (tx_off ? IAPHIDReportLinkControlBits_Continue : 0) |
        (tx_off + take < tx_len ? IAPHIDReportLinkControlBits_MoreToFollow : 0);
    memcpy(r->data, tx + tx_off, take);
    if (!tx_off)
        log_report(report, 1 + rs->size, true);
    tx_off += take;
    int ret = usb_drv_send_nonblocking(HID_EP_IN, report, 1 + rs->size);
    if (ret == 0)
    {
        tx_busy = true;
        tx_started = current_tick;
    }
    else
    {
        usb_log(USB_LOG_IAP2_SENT, 0, 0, ret, 0);
        tx_len = tx_off = 0;
        if (tx_slot)
            tx_slot->queued = true;
        tx_slot = NULL;
    }
}

bool usb_iap2_sent(int status, int length)
{
    if (!tx_busy)
        return false;
    tx_busy = false;
    tx_stuck_logged = false;
    if (status != 0)
        usb_log(USB_LOG_IAP2_SENT, 0, 0, status, length);
    if (tx_slot && tx_off >= tx_len)
    {
        /* Never sent again, so done with once sent. */
        if (!rto && count && tx_slot == &slots[head])
        {
            head = (head + 1) % SLOTS;
            count--;
        }
        tx_slot = NULL;
    }
    send_next();
    if (link_up && count < SLOTS)
        usb_iap2_control_room();
    return true;
}

size_t usb_iap2_room(void)
{
    return link_up ? SLOTS - count : 0;
}

uint8_t *usb_iap2_message_start(size_t *room)
{
    if (!link_up || count == SLOTS)
        return NULL;
    *room = MAX_PACKET - HEADER_LEN - 1;
    return slots[(head + count) % SLOTS].packet + HEADER_LEN;
}

static void message_send(size_t n, uint8_t session)
{
    struct slot *s = &slots[(head + count) % SLOTS];
    s->len = HEADER_LEN + n + 1;
    put_header(s->packet, s->len, CTL_ACK, ++my_seq, session);
    s->packet[HEADER_LEN + n] = checksum(s->packet + HEADER_LEN, n);
    s->queued = true;
    s->tries = 0;
    count++;
    send_next();
}

void usb_iap2_message_send(size_t n)
{
    message_send(n, control_session);
}

bool usb_iap2_file_send(size_t n)
{
    if (!file_session)
        return false;
    message_send(n, file_session);
    return true;
}

/* ---- the link layer ----------------------------------------------------- */

static void reset_link(void)
{
    synced = false;
    link_up = false;
    count = 0;
    tx_slot = NULL;
    cs_len = 0;
    pending &= ~SEND_SYN_ACK;
    usb_iap2_control_reset();
}

/* Retransmitted SYNs get the same answer. */
static void take_syn(uint8_t seq, const uint8_t *payload, size_t n)
{
    if (n < SYN_FIXED || n > sizeof(syn_ack) || payload[0] != 1)
        return;
    memcpy(syn_ack, payload, n);
    syn_ack_len = n;
    if ((syn_ack[2] << 8 | syn_ack[3]) > MAX_PACKET)
    {
        syn_ack[2] = MAX_PACKET >> 8;
        syn_ack[3] = MAX_PACKET & 0xff;
    }
    window = MAX(1, MIN(payload[1], SLOTS));
    /* The accessory's retransmission timeout and retransmissions, as it
     * gives them. Trap: this car gives 0 for both and leaves packets
     * unacknowledged for seconds; an iPhone never sends one again, and a
     * packet sent again stops the car reading. */
    max_sends = payload[8] + 1u;
    rto = payload[8] ? (payload[4] << 8 | payload[5]) * HZ / 1000 : 0;
    if (rto && rto < HZ / 10)
        rto = HZ / 10;
    control_session = 0;
    file_session = 0;
    for (size_t i = SYN_FIXED; i + 3 <= n; i += 3)
    {
        if (payload[i + 1] == 0)
            control_session = payload[i];
        else if (payload[i + 1] == 1)
            file_session = payload[i];
    }
    syn_seq = my_seq;
    peer_seq = seq;
    synced = true;
    pending |= SEND_SYN_ACK;
}

/* Everything the accessory has acknowledged leaves the ring. */
static void take_ack(uint8_t ack)
{
    if (!link_up && ack == syn_seq)
    {
        link_up = true;
        usb_log(USB_LOG_IAP2_EVENT, USB_LOG_IAP2_LINK_UP, 0,
                control_session, window);
        usb_iap2_control_link_up();
    }
    while (count)
    {
        struct slot *s = &slots[head];
        if (!s->tries || (uint8_t)(ack - s->packet[5]) >= 128)
            break;
        head = (head + 1) % SLOTS;
        count--;
    }
}

/* Whole messages off the front of the control session's bytes. */
static void take_control(const uint8_t *data, size_t n)
{
    if (cs_len + n > sizeof(cs))
        cs_len = 0;
    memcpy(cs + cs_len, data, n);
    cs_len += n;
    size_t off = 0;
    while (cs_len - off >= 6)
    {
        const uint8_t *m = cs + off;
        const size_t len = m[2] << 8 | m[3];
        if (m[0] != 0x40 || m[1] != 0x40 || len < 6 || len > sizeof(cs))
        {
            off = cs_len;
            break;
        }
        if (cs_len - off < len)
            break;
        usb_iap2_control_receive(m, len);
        off += len;
    }
    memmove(cs, cs + off, cs_len - off);
    cs_len -= off;
}

static void take_packet(const uint8_t *p, size_t len)
{
    const uint8_t control = p[4], seq = p[5], session = p[7];
    const uint8_t *payload = p + HEADER_LEN;
    const size_t n = len > HEADER_LEN ? len - HEADER_LEN - 1 : 0;
    if (n && checksum(payload, n + 1) != 0)
    {
        log_packet(p, len, USB_LOG_IAP2_CHECKSUM, false);
        return;
    }
    log_packet(p, len, USB_LOG_IAP2_OK, false);

    if (control & CTL_RST)
    {
        reset_link();
        return;
    }
    if (control & CTL_SYN)
    {
        if (link_up)
            reset_link();
        take_syn(seq, payload, n);
        return;
    }
    if (!synced)
        return;
    if (control & CTL_ACK)
        take_ack(p[6]);
    if (control & CTL_EAK)
    {
        for (size_t i = 0; i < n; i++)
            for (unsigned j = 0; j < count; j++)
            {
                struct slot *s = &slots[(head + j) % SLOTS];
                if (s->tries && s->packet[5] == payload[i])
                    s->queued = true;
            }
    }
    else if (n)
    {
        /* Out of order or repeated, the packet is dropped; the player's
         * next packet names the last one taken. */
        if (seq == (uint8_t)(peer_seq + 1))
        {
            peer_seq = seq;
            if (link_up && session == control_session)
                take_control(payload, n);
            else if (link_up && file_session && session == file_session)
                usb_iap2_control_file(payload, n);
        }
    }
}

/* ---- receiving ---------------------------------------------------------- */

/* Whole packets off the front of rx. What is left of the last report of a
 * transfer is padding, or a packet cut short, and goes. */
static void take_packets(bool last)
{
    size_t off = 0;
    while (rx_len - off >= HEADER_LEN)
    {
        const uint8_t *p = rx + off;
        if (p[0] != SOP1 || p[1] != SOP2 || checksum(p, HEADER_LEN) != 0)
        {
            off++;
            continue;
        }
        const size_t len = p[2] << 8 | p[3];
        if (len < HEADER_LEN)
        {
            off++;
            continue;
        }
        if (len > MAX_PACKET)
        {
            log_packet(p, len, USB_LOG_IAP2_TOO_LONG, false);
            off = rx_len;
            break;
        }
        if (rx_len - off < len)
            break;
        take_packet(p, len);
        off += len;
    }
    if (last)
        rx_len = 0;
    else
    {
        memmove(rx, rx + off, rx_len - off);
        rx_len -= off;
    }
}

bool usb_iap2_report(const uint8_t *r, size_t len)
{
    const struct IAPHIDReport *hr = (const struct IAPHIDReport *)r;
    if (!usb_iap_answer_iap2() || len < sizeof(*hr))
        return false;
    const uint8_t *data = hr->data;
    size_t n = len - sizeof(*hr);
    const bool first =
        !(hr->link_control & IAPHIDReportLinkControlBits_Continue);
    const bool is_probe = first && n >= sizeof(probe) &&
                          !memcmp(data, probe, sizeof(probe));
    if (!is_probe && !seen)
        return false;
    if (first)
        log_report(r, len, false);

    if (is_probe)
    {
        seen = true;
        reset_link();
        rx_len = 0;
        pending |= SEND_ECHO;
    }
    else
    {
        if (first)
            rx_len = 0;
        n = MIN(n, sizeof(rx) - rx_len);
        memcpy(rx + rx_len, data, n);
        rx_len += n;
        take_packets(!(hr->link_control &
                       IAPHIDReportLinkControlBits_MoreToFollow));
    }
    send_next();
    return true;
}

bool usb_iap2_audio_rate(unsigned long rate)
{
    if (!seen)
        return false;
    usb_iap2_control_rate(rate);
    return true;
}

bool usb_iap2_tick(void)
{
    if (!seen)
        return false;
    if (link_up && count && rto)
    {
        struct slot *s = &slots[head];
        if (s->tries && !s->queued && TIME_AFTER(current_tick, s->sent + rto))
        {
            if (s->tries >= max_sends)
            {
                usb_log(USB_LOG_IAP2_EVENT, USB_LOG_IAP2_LINK_LOST, 0,
                        s->packet[5], s->tries);
                reset_link();
            }
            else
                s->queued = true;
        }
    }
    if (tx_busy && !tx_stuck_logged &&
        TIME_AFTER(current_tick, tx_started + HZ))
    {
        usb_log(USB_LOG_IAP2_SENT, 0, 0, TX_STUCK, tx_off);
        tx_stuck_logged = true;
    }
    if (link_up)
        usb_iap2_control_tick();
    send_next();
    return true;
}

void usb_iap2_connect(void)
{
    seen = false;
    my_seq = 1;
    peer_seq = 0;
    head = 0;
    pending = 0;
    tx_len = tx_off = 0;
    tx_busy = false;
    tx_stuck_logged = false;
    rx_len = 0;
    reset_link();
}
