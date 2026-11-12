/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2007 by Christian Gmeiner
 * Copyright (C) 2021 by Tomasz Moń
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
#include "string.h"
#include "system.h"
#include "usb_core.h"
#include "usb_drv.h"
#include "kernel.h"
#include "usb_serial.h"
#include "usb_class_driver.h"
#include "usb.h"
/*#define LOGF_ENABLE*/
#include "logf.h"

#define CDC_SUBCLASS_ACM         0x02
#define CDC_PROTOCOL_NONE        0x00

/* Class-Specific Request Codes */
#define SET_LINE_CODING          0x20
#define GET_LINE_CODING          0x21
#define SET_CONTROL_LINE_STATE   0x22

#define SUBTYPE_HEADER           0x00
#define SUBTYPE_CALL_MANAGEMENT  0x01
#define SUBTYPE_ACM              0x02
#define SUBTYPE_UNION            0x06

/* Support SET_LINE_CODING, GET_LINE_CODING, SET_CONTROL_LINE_STATE requests
 * and SERIAL_STATE notification.
 */
#define ACM_CAP_LINE_CODING      0x02

struct cdc_header_descriptor {
    uint8_t  bFunctionLength;
    uint8_t  bDescriptorType;
    uint8_t  bDescriptorSubtype;
    uint16_t bcdCDC;
} __attribute__((packed));

struct cdc_call_management_descriptor {
    uint8_t bFunctionLength;
    uint8_t bDescriptorType;
    uint8_t bDescriptorSubtype;
    uint8_t bmCapabilities;
    uint8_t bDataInterface;
} __attribute__((packed));

struct cdc_acm_descriptor {
    uint8_t bFunctionLength;
    uint8_t bDescriptorType;
    uint8_t bDescriptorSubtype;
    uint8_t bmCapabilities;
} __attribute__((packed));

struct cdc_union_descriptor {
    uint8_t  bFunctionLength;
    uint8_t  bDescriptorType;
    uint8_t  bDescriptorSubtype;
    uint8_t  bControlInterface;
    uint8_t  bSubordinateInterface0;
} __attribute__((packed));

struct cdc_line_coding {
    uint32_t dwDTERate;
    uint8_t bCharFormat;
    uint8_t bParityType;
    uint8_t bDataBits;
} __attribute__((packed));

static struct usb_interface_assoc_descriptor
    association_descriptor =
{
    .bLength            = sizeof(struct usb_interface_assoc_descriptor),
    .bDescriptorType    = USB_DT_INTERFACE_ASSOCIATION,
    .bFirstInterface    = 0,
    .bInterfaceCount    = 2,
    .bFunctionClass     = USB_CLASS_COMM,
    .bFunctionSubClass  = CDC_SUBCLASS_ACM,
    .bFunctionProtocol  = CDC_PROTOCOL_NONE,
    .iFunction          = 0
};

static struct usb_interface_descriptor
    control_interface_descriptor =
{
    .bLength            = sizeof(struct usb_interface_descriptor),
    .bDescriptorType    = USB_DT_INTERFACE,
    .bInterfaceNumber   = 0,
    .bAlternateSetting  = 0,
    .bNumEndpoints      = 1,
    .bInterfaceClass    = USB_CLASS_COMM,
    .bInterfaceSubClass = CDC_SUBCLASS_ACM,
    .bInterfaceProtocol = CDC_PROTOCOL_NONE,
    .iInterface         = 0
};

static struct cdc_header_descriptor
    header_descriptor =
{
    .bFunctionLength    = sizeof(struct cdc_header_descriptor),
    .bDescriptorType    = USB_DT_CS_INTERFACE,
    .bDescriptorSubtype = SUBTYPE_HEADER,
    .bcdCDC             = 0x0110
};

static struct cdc_call_management_descriptor
    call_management_descriptor =
{
    .bFunctionLength    = sizeof(struct cdc_call_management_descriptor),
    .bDescriptorType    = USB_DT_CS_INTERFACE,
    .bDescriptorSubtype = SUBTYPE_CALL_MANAGEMENT,
    .bmCapabilities     = 0,
    .bDataInterface     = 0
};

static struct cdc_acm_descriptor
    acm_descriptor =
{
    .bFunctionLength    = sizeof(struct cdc_acm_descriptor),
    .bDescriptorType    = USB_DT_CS_INTERFACE,
    .bDescriptorSubtype = SUBTYPE_ACM,
    .bmCapabilities     = ACM_CAP_LINE_CODING
};

static struct cdc_union_descriptor
    union_descriptor =
{
    .bFunctionLength        = sizeof(struct cdc_union_descriptor),
    .bDescriptorType        = USB_DT_CS_INTERFACE,
    .bDescriptorSubtype     = SUBTYPE_UNION,
    .bControlInterface      = 0,
    .bSubordinateInterface0 = 0
};

static struct usb_interface_descriptor
    data_interface_descriptor =
{
    .bLength            = sizeof(struct usb_interface_descriptor),
    .bDescriptorType    = USB_DT_INTERFACE,
    .bInterfaceNumber   = 0,
    .bAlternateSetting  = 0,
    .bNumEndpoints      = 2,
    .bInterfaceClass    = USB_CLASS_CDC_DATA,
    .bInterfaceSubClass = 0,
    .bInterfaceProtocol = 0,
    .iInterface         = 0
};

static struct usb_endpoint_descriptor
    endpoint_descriptor =
{
    .bLength          = sizeof(struct usb_endpoint_descriptor),
    .bDescriptorType  = USB_DT_ENDPOINT,
    .bEndpointAddress = 0,
    .bmAttributes     = USB_ENDPOINT_XFER_BULK,
    .wMaxPacketSize   = 0,
    .bInterval        = 0
};

union line_coding_buffer
{
    struct cdc_line_coding data;
    unsigned char raw[64];
};

static union line_coding_buffer line_coding;

/* send_buffer: local ring buffer.
 * transit_buffer: used to store aligned data that will be sent by the USB
 * driver. PP502x needs boost for high speed USB, but still works up to
 * around 100 bytes without boost, we play safe and limit packet size to 32
 * bytes, it doesn't hurt because data can be sent over several transfers.
 */
#ifdef HAVE_USB_PODLINK
#define BUFFER_SIZE 16384
#else
#define BUFFER_SIZE 512
#endif
#ifdef HAVE_USB_PODLINK
#define TRANSIT_BUFFER_SIZE 4096
#else
#define TRANSIT_BUFFER_SIZE 32
#endif
#ifdef HAVE_USB_PODLINK
#define RECV_BUFFER_SIZE 512
#else
#define RECV_BUFFER_SIZE TRANSIT_BUFFER_SIZE
#endif
static unsigned char send_buffer[BUFFER_SIZE];
static unsigned char transit_buffer[TRANSIT_BUFFER_SIZE]
    USB_DEVBSS_ATTR __attribute__((aligned(4)));
static unsigned char receive_buffer[RECV_BUFFER_SIZE]
    USB_DEVBSS_ATTR __attribute__((aligned(32)));

/* Both rings are protected by this mutex. Only the USB thread submits DMA.
 * Leave room for a full OUT packet before arming receive: NAK is backpressure.
 * Pending IN bytes remain in the ring until successful completion. */
static struct mutex serial_mtx;
#ifdef HAVE_USB_PODLINK
static struct semaphore serial_wake;
static void wake_screen(void)
{
    semaphore_release(&serial_wake);
}
#else
static void wake_screen(void)
{
}
#endif
static unsigned char rx_buffer[BUFFER_SIZE];
static int tx_start, tx_count, tx_inflight;
static int rx_start, rx_count;
static bool rx_armed, active, kick_pending;
static int rx_packet_size = RECV_BUFFER_SIZE;

static struct usb_class_driver_ep_allocation ep_allocs[3] = {
    {.type = USB_ENDPOINT_XFER_BULK, .dir = DIR_IN, .optional = false, .mps = -1},
    {.type = USB_ENDPOINT_XFER_BULK, .dir = DIR_OUT, .optional = false, .mps = -1},
    {.type = USB_ENDPOINT_XFER_INT, .dir = DIR_IN,
     .optional = false, .mps = -1},
};

#define EP_IN (ep_allocs[0].ep)
#define EP_OUT (ep_allocs[1].ep)
#define EP_INT (ep_allocs[2].ep)

static int control_interface, data_interface;

static int usb_serial_set_first_interface(int interface)
{
    control_interface = interface;
    data_interface = interface + 1;
    return interface + 2;
}

static int usb_serial_get_config_descriptor(unsigned char *dest, int max_packet_size)
{
    unsigned char *orig_dest = dest;

    association_descriptor.bFirstInterface         = control_interface;
    control_interface_descriptor.bInterfaceNumber  = control_interface;
    call_management_descriptor.bDataInterface      = data_interface;
    union_descriptor.bControlInterface             = control_interface;
    union_descriptor.bSubordinateInterface0        = data_interface;
    data_interface_descriptor.bInterfaceNumber     = data_interface;

    if (EP_INT > 0)
    {
        PACK_DATA(&dest, association_descriptor);
        PACK_DATA(&dest, control_interface_descriptor);
        PACK_DATA(&dest, header_descriptor);
        PACK_DATA(&dest, call_management_descriptor);
        PACK_DATA(&dest, acm_descriptor);
        PACK_DATA(&dest, union_descriptor);

        /* Notification endpoint. Set wMaxPacketSize to 64 as it is valid
         * both on Full and High speed. Note that max_packet_size is for bulk.
         * Maximum bInterval for High Speed is 16 and for Full Speed is 255.
         */
        endpoint_descriptor.bEndpointAddress = EP_INT;
        endpoint_descriptor.bmAttributes     = USB_ENDPOINT_XFER_INT;
        endpoint_descriptor.wMaxPacketSize   = 64;
        endpoint_descriptor.bInterval        = 16;
        PACK_DATA(&dest, endpoint_descriptor);
    }

    PACK_DATA(&dest, data_interface_descriptor);
    endpoint_descriptor.bEndpointAddress = EP_IN;
    endpoint_descriptor.bmAttributes     = USB_ENDPOINT_XFER_BULK;
    endpoint_descriptor.wMaxPacketSize   = max_packet_size;
    endpoint_descriptor.bInterval        = 0;
    PACK_DATA(&dest, endpoint_descriptor);

    endpoint_descriptor.bEndpointAddress = EP_OUT;
    PACK_DATA(&dest, endpoint_descriptor);

    return (dest - orig_dest);
}

/* called by usb_core_control_request() */
static bool usb_serial_control_request(struct usb_ctrlrequest* req, uint8_t* reqdata, size_t reqdata_size)
{
    (void)reqdata_size; /* should check this? */

    bool handled = false;

    if (req->wIndex != control_interface)
    {
        return false;
    }

    if (req->bRequestType == (USB_DIR_OUT|USB_TYPE_CLASS|USB_RECIP_INTERFACE))
    {
        if (req->bRequest == SET_LINE_CODING)
        {
            if (req->wLength == sizeof(struct cdc_line_coding))
            {
                /* Receive line coding into local copy */
                memcpy(line_coding.raw, reqdata, sizeof(struct cdc_line_coding));
                usb_core_control_response(USB_CONTROL_ACK, NULL, 0);
                handled = true;
            }
        }
        else if (req->bRequest == SET_CONTROL_LINE_STATE)
        {
            if (req->wLength == 0)
            {
                /* wValue holds Control Signal Bitmap that is simply ignored here */
                usb_core_control_response(USB_CONTROL_ACK, NULL, 0);
                handled = true;
            }
        }
    }
    else if (req->bRequestType == (USB_DIR_IN|USB_TYPE_CLASS|USB_RECIP_INTERFACE))
    {
        if (req->bRequest == GET_LINE_CODING)
        {
            if (req->wLength == sizeof(struct cdc_line_coding))
            {
                /* Send back line coding so host is happy */
                usb_core_control_response(USB_CONTROL_ACK, line_coding.raw,
                                         sizeof(struct cdc_line_coding));
                handled = true;
            }
        }
    }

    return handled;
}

static uint32_t stream_generation;

/* Called with serial_mtx held, only in the USB thread. */
static void serial_service(void)
{
    if (!active)
        return;
    if (!rx_armed && BUFFER_SIZE - rx_count >= rx_packet_size)
    {
        if (usb_drv_recv_nonblocking(EP_OUT, receive_buffer,
                                     rx_packet_size) == 0)
        {
            rx_armed = true;
        }
        else
        {
            active = false;
            ++stream_generation;
        }
    }

    if (!tx_inflight && tx_count)
    {
        int n = MIN(tx_count, TRANSIT_BUFFER_SIZE);
        int first = MIN(n, BUFFER_SIZE - tx_start);
        memcpy(transit_buffer, send_buffer + tx_start, first);
        memcpy(transit_buffer + first, send_buffer, n - first);
        if (usb_drv_send_nonblocking(EP_IN, transit_buffer, n) == 0)
        {
            tx_inflight = n;
        }
        else
        {
            active = false;
            ++stream_generation;
        }
    }
}

static void reset_buffers(void)
{
    ++stream_generation;
    tx_start = tx_count = tx_inflight = rx_start = rx_count = 0;
    rx_armed = kick_pending = false;
}

void usb_serial_init(void)
{
    static bool initialized;
    if (!initialized)
    {
        mutex_init(&serial_mtx);
#ifdef HAVE_USB_PODLINK
        semaphore_init(&serial_wake, 1, 0);
#endif
        initialized = true;
    }
    mutex_lock(&serial_mtx);
    active = false;
    reset_buffers();
    line_coding.data.dwDTERate = 115200;
    line_coding.data.bDataBits = 8;
    mutex_unlock(&serial_mtx);
}

static int usb_serial_init_connection(void)
{
    mutex_lock(&serial_mtx);
    reset_buffers();
#ifdef HAVE_USB_PODLINK
    /* A stream write ending on a full packet need not carry a ZLP. One
     * packet per OUT request therefore completes every host write size. */
    rx_packet_size = usb_drv_port_speed() ? 512 : 64;
#endif
    active = true;
    serial_service();
    mutex_unlock(&serial_mtx);
    wake_screen();
    return active ? 0 : -1;
}

static void usb_serial_disconnect(void)
{
    mutex_lock(&serial_mtx);
    active = false;
    reset_buffers();
    mutex_unlock(&serial_mtx);
    wake_screen();
}

/* Called with serial_mtx held. Coalesce notifications to bound queue usage. */
static void kick(void)
{
    if (active && !kick_pending)
    {
        kick_pending = true;
        usb_signal_class_notify(USB_DRIVER_SERIAL, 0);
    }
}

static void serial_notify(intptr_t data)
{
    (void)data;
    mutex_lock(&serial_mtx);
    kick_pending = false;
    serial_service();
    mutex_unlock(&serial_mtx);
}

bool usb_serial_get_state(uint32_t *generation)
{
    mutex_lock(&serial_mtx);
    bool connected = active;
    *generation = stream_generation;
    mutex_unlock(&serial_mtx);
    return connected;
}

bool usb_serial_connected(void)
{
    uint32_t generation;
    return usb_serial_get_state(&generation);
}

int usb_serial_write(const unsigned char *data, int length)
{
    if (!data || length <= 0)
        return 0;
    mutex_lock(&serial_mtx);
    int n = active ? MIN(length, BUFFER_SIZE - tx_count) : 0;
    int start = (tx_start + tx_count) % BUFFER_SIZE;
    int first = MIN(n, BUFFER_SIZE - start);
    memcpy(send_buffer + start, data, first);
    memcpy(send_buffer, data + first, n - first);
    tx_count += n;
    if (n)
        kick();
    mutex_unlock(&serial_mtx);
    return n;
}

int usb_serial_read(unsigned char *data, int length)
{
    if (!data || length <= 0)
        return 0;
    mutex_lock(&serial_mtx);
    int n = active ? MIN(length, rx_count) : 0;
    int first = MIN(n, BUFFER_SIZE - rx_start);
    memcpy(data, rx_buffer + rx_start, first);
    memcpy(data + first, rx_buffer, n - first);
    rx_start = (rx_start + n) % BUFFER_SIZE;
    rx_count -= n;
    if (n)
        kick();
    mutex_unlock(&serial_mtx);
    return n;
}

void usb_serial_send(const unsigned char *data, int length)
{
    /* logf can run before init and in interrupts. Never wait in those paths. */
    if (!active)
        return;
#ifdef is_thread_context
    if (!is_thread_context())
        return;
#endif
#ifdef HAVE_USB_PODLINK
    if (usb_podlink_active())
        return; /* logf must never enter the application byte stream */
#endif
    (void)usb_serial_write(data, length);
}

static void usb_serial_transfer_complete(int ep, int dir, int status,
                                         int length)
{
    (void)ep;
    mutex_lock(&serial_mtx);
    if (!active)
        goto out;
    if (status != 0 || length < 0)
    {
        active = false; /* fail the session, never silently lose stream bytes */
        reset_buffers();
        goto out;
    }
    if (dir == USB_DIR_OUT)
    {
        rx_armed = false;
        if (length > rx_packet_size || length > BUFFER_SIZE - rx_count)
        {
            active = false;
            reset_buffers();
            goto out;
        }
        int start = (rx_start + rx_count) % BUFFER_SIZE;
        int first = MIN(length, BUFFER_SIZE - start);
        memcpy(rx_buffer + start, receive_buffer, first);
        memcpy(rx_buffer, receive_buffer + first, length - first);
        rx_count += length;
    }
    else if (dir == USB_DIR_IN)
    {
        if (length != tx_inflight)
        {
            active = false;
            reset_buffers();
            goto out;
        }
        tx_start = (tx_start + length) % BUFFER_SIZE;
        tx_count -= length;
        tx_inflight = 0;
    }
    serial_service();
out:
    mutex_unlock(&serial_mtx);
    wake_screen();
}

struct usb_class_driver usb_cdrv_serial = {
    .needs_exclusive_storage = false,
    .needs_cpu_boost = false,
    .config = 1,
    .ep_allocs_size = ARRAYLEN(ep_allocs),
    .ep_allocs = ep_allocs,
    .set_first_interface = usb_serial_set_first_interface,
    .get_config_descriptor = usb_serial_get_config_descriptor,
    .init_connection = usb_serial_init_connection,
    .init = usb_serial_init,
    .disconnect = usb_serial_disconnect,
    .transfer_complete = usb_serial_transfer_complete,
    .control_request = usb_serial_control_request,
    .notify_event = serial_notify,
};

void usb_serial_wait(int ticks)
{
#ifdef HAVE_USB_PODLINK
    mutex_lock(&serial_mtx);
    bool ready = rx_count != 0;
    mutex_unlock(&serial_mtx);
    if (!ready) semaphore_wait(&serial_wake, ticks);
#else
    (void)ticks;
#endif
}
