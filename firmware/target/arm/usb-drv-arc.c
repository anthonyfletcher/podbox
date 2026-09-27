/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Driver for ARC USBOTG Device Controller
 *
 * Copyright (C) 2007 by Björn Stenberg
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

#include "system.h"
#include "config.h"
#include "string.h"
#include "usb_ch9.h"
#include "usb_core.h"
#include "kernel.h"
#include "panic.h"
#include "usb_drv.h"
#include "usb_log.h"

/*#define LOGF_ENABLE*/
#include "logf.h"

/* USB device mode registers (Little Endian) */

#define REG_ID               (*(volatile unsigned int *)(USB_BASE+0x000))
#define REG_HWGENERAL        (*(volatile unsigned int *)(USB_BASE+0x004))
#define REG_HWHOST           (*(volatile unsigned int *)(USB_BASE+0x008))
#define REG_HWDEVICE         (*(volatile unsigned int *)(USB_BASE+0x00c))
#define REG_TXBUF            (*(volatile unsigned int *)(USB_BASE+0x010))
#define REG_RXBUF            (*(volatile unsigned int *)(USB_BASE+0x014))
#define REG_CAPLENGTH        (*(volatile unsigned char*)(USB_BASE+0x100))
#define REG_DCIVERSION       (*(volatile unsigned int *)(USB_BASE+0x120))
#define REG_DCCPARAMS        (*(volatile unsigned int *)(USB_BASE+0x124))
#define REG_USBCMD           (*(volatile unsigned int *)(USB_BASE+0x140))
#define REG_USBSTS           (*(volatile unsigned int *)(USB_BASE+0x144))
#define REG_USBINTR          (*(volatile unsigned int *)(USB_BASE+0x148))
#define REG_FRINDEX          (*(volatile unsigned int *)(USB_BASE+0x14c))
#define REG_DEVICEADDR       (*(volatile unsigned int *)(USB_BASE+0x154))
#define REG_ENDPOINTLISTADDR (*(volatile unsigned int *)(USB_BASE+0x158))
#define REG_BURSTSIZE        (*(volatile unsigned int *)(USB_BASE+0x160))
#define REG_ULPI             (*(volatile unsigned int *)(USB_BASE+0x170))
#define REG_CONFIGFLAG       (*(volatile unsigned int *)(USB_BASE+0x180))
#define REG_PORTSC1          (*(volatile unsigned int *)(USB_BASE+0x184))
#define REG_OTGSC            (*(volatile unsigned int *)(USB_BASE+0x1a4))
#define REG_USBMODE          (*(volatile unsigned int *)(USB_BASE+0x1a8))
#define REG_ENDPTSETUPSTAT   (*(volatile unsigned int *)(USB_BASE+0x1ac))
#define REG_ENDPTPRIME       (*(volatile unsigned int *)(USB_BASE+0x1b0))
#define REG_ENDPTFLUSH       (*(volatile unsigned int *)(USB_BASE+0x1b4))
#define REG_ENDPTSTATUS      (*(volatile unsigned int *)(USB_BASE+0x1b8))
#define REG_ENDPTCOMPLETE    (*(volatile unsigned int *)(USB_BASE+0x1bc))
#define REG_ENDPTCTRL0       (*(volatile unsigned int *)(USB_BASE+0x1c0))
#define REG_ENDPTCTRL1       (*(volatile unsigned int *)(USB_BASE+0x1c4))
#define REG_ENDPTCTRL2       (*(volatile unsigned int *)(USB_BASE+0x1c8))
#define REG_ENDPTCTRL(_x_)   (*(volatile unsigned int *)(USB_BASE+0x1c0+4*(_x_)))

/* Frame Index Register Bit Masks */
#define USB_FRINDEX_MASKS                      (0x3fff)

/* USB CMD  Register Bit Masks */
#define USBCMD_RUN                            (0x00000001)
#define USBCMD_CTRL_RESET                     (0x00000002)
#define USBCMD_PERIODIC_SCHEDULE_EN           (0x00000010)
#define USBCMD_ASYNC_SCHEDULE_EN              (0x00000020)
#define USBCMD_INT_AA_DOORBELL                (0x00000040)
#define USBCMD_ASP                            (0x00000300)
#define USBCMD_ASYNC_SCH_PARK_EN              (0x00000800)
#define USBCMD_SUTW                           (0x00002000)
#define USBCMD_ATDTW                          (0x00004000)
#define USBCMD_ITC                            (0x00FF0000)

/* bit 15,3,2 are frame list size */
#define USBCMD_FRAME_SIZE_1024                (0x00000000)
#define USBCMD_FRAME_SIZE_512                 (0x00000004)
#define USBCMD_FRAME_SIZE_256                 (0x00000008)
#define USBCMD_FRAME_SIZE_128                 (0x0000000C)
#define USBCMD_FRAME_SIZE_64                  (0x00008000)
#define USBCMD_FRAME_SIZE_32                  (0x00008004)
#define USBCMD_FRAME_SIZE_16                  (0x00008008)
#define USBCMD_FRAME_SIZE_8                   (0x0000800C)

/* bit 9-8 are async schedule park mode count */
#define USBCMD_ASP_00                         (0x00000000)
#define USBCMD_ASP_01                         (0x00000100)
#define USBCMD_ASP_10                         (0x00000200)
#define USBCMD_ASP_11                         (0x00000300)
#define USBCMD_ASP_BIT_POS                    (8)

/* bit 23-16 are interrupt threshold control */
#define USBCMD_ITC_NO_THRESHOLD               (0x00000000)
#define USBCMD_ITC_1_MICRO_FRM                (0x00010000)
#define USBCMD_ITC_2_MICRO_FRM                (0x00020000)
#define USBCMD_ITC_4_MICRO_FRM                (0x00040000)
#define USBCMD_ITC_8_MICRO_FRM                (0x00080000)
#define USBCMD_ITC_16_MICRO_FRM               (0x00100000)
#define USBCMD_ITC_32_MICRO_FRM               (0x00200000)
#define USBCMD_ITC_64_MICRO_FRM               (0x00400000)
#define USBCMD_ITC_BIT_POS                    (16)

/* USB STS Register Bit Masks */
#define USBSTS_INT                            (0x00000001)
#define USBSTS_ERR                            (0x00000002)
#define USBSTS_PORT_CHANGE                    (0x00000004)
#define USBSTS_FRM_LST_ROLL                   (0x00000008)
#define USBSTS_SYS_ERR                        (0x00000010) /* not used */
#define USBSTS_IAA                            (0x00000020)
#define USBSTS_RESET                          (0x00000040)
#define USBSTS_SOF                            (0x00000080)
#define USBSTS_SUSPEND                        (0x00000100)
#define USBSTS_HC_HALTED                      (0x00001000)
#define USBSTS_RCL                            (0x00002000)
#define USBSTS_PERIODIC_SCHEDULE              (0x00004000)
#define USBSTS_ASYNC_SCHEDULE                 (0x00008000)

/* USB INTR Register Bit Masks */
#define USBINTR_INT_EN                        (0x00000001)
#define USBINTR_ERR_INT_EN                    (0x00000002)
#define USBINTR_PTC_DETECT_EN                 (0x00000004)
#define USBINTR_FRM_LST_ROLL_EN               (0x00000008)
#define USBINTR_SYS_ERR_EN                    (0x00000010)
#define USBINTR_ASYN_ADV_EN                   (0x00000020)
#define USBINTR_RESET_EN                      (0x00000040)
#define USBINTR_SOF_EN                        (0x00000080)
#define USBINTR_DEVICE_SUSPEND                (0x00000100)

/* ULPI Register Bit Masks */
#define ULPI_ULPIWU                           (0x80000000)
#define ULPI_ULPIRUN                          (0x40000000)
#define ULPI_ULPIRW                           (0x20000000)
#define ULPI_ULPISS                           (0x08000000)
#define ULPI_ULPIPORT                         (0x07000000)
#define ULPI_ULPIADDR                         (0x00FF0000)
#define ULPI_ULPIDATRD                        (0x0000FF00)
#define ULPI_ULPIDATWR                        (0x000000FF)

/* Device Address bit masks */
#define USBDEVICEADDRESS_MASK                 (0xFE000000)
#define USBDEVICEADDRESS_BIT_POS              (25)

/* endpoint list address bit masks */
#define USB_EP_LIST_ADDRESS_MASK               (0xfffff800)

/* PORTSCX  Register Bit Masks */
#define PORTSCX_CURRENT_CONNECT_STATUS         (0x00000001)
#define PORTSCX_CONNECT_STATUS_CHANGE          (0x00000002)
#define PORTSCX_PORT_ENABLE                    (0x00000004)
#define PORTSCX_PORT_EN_DIS_CHANGE             (0x00000008)
#define PORTSCX_OVER_CURRENT_ACT               (0x00000010)
#define PORTSCX_OVER_CURRENT_CHG               (0x00000020)
#define PORTSCX_PORT_FORCE_RESUME              (0x00000040)
#define PORTSCX_PORT_SUSPEND                   (0x00000080)
#define PORTSCX_PORT_RESET                     (0x00000100)
#define PORTSCX_LINE_STATUS_BITS               (0x00000C00)
#define PORTSCX_PORT_POWER                     (0x00001000)
#define PORTSCX_PORT_INDICTOR_CTRL             (0x0000C000)
#define PORTSCX_PORT_TEST_CTRL                 (0x000F0000)
#define PORTSCX_WAKE_ON_CONNECT_EN             (0x00100000)
#define PORTSCX_WAKE_ON_CONNECT_DIS            (0x00200000)
#define PORTSCX_WAKE_ON_OVER_CURRENT           (0x00400000)
#define PORTSCX_PHY_LOW_POWER_SPD              (0x00800000)
#define PORTSCX_PORT_FORCE_FULL_SPEED          (0x01000000)
#define PORTSCX_PORT_SPEED_MASK                (0x0C000000)
#define PORTSCX_PORT_WIDTH                     (0x10000000)
#define PORTSCX_PHY_TYPE_SEL                   (0xC0000000)

/* bit 11-10 are line status */
#define PORTSCX_LINE_STATUS_SE0                (0x00000000)
#define PORTSCX_LINE_STATUS_JSTATE             (0x00000400)
#define PORTSCX_LINE_STATUS_KSTATE             (0x00000800)
#define PORTSCX_LINE_STATUS_UNDEF              (0x00000C00)
#define PORTSCX_LINE_STATUS_BIT_POS            (10)

/* bit 15-14 are port indicator control */
#define PORTSCX_PIC_OFF                        (0x00000000)
#define PORTSCX_PIC_AMBER                      (0x00004000)
#define PORTSCX_PIC_GREEN                      (0x00008000)
#define PORTSCX_PIC_UNDEF                      (0x0000C000)
#define PORTSCX_PIC_BIT_POS                    (14)

/* bit 19-16 are port test control */
#define PORTSCX_PTC_DISABLE                    (0x00000000)
#define PORTSCX_PTC_JSTATE                     (0x00010000)
#define PORTSCX_PTC_KSTATE                     (0x00020000)
#define PORTSCX_PTC_SE0NAK                     (0x00030000)
#define PORTSCX_PTC_PACKET                     (0x00040000)
#define PORTSCX_PTC_FORCE_EN                   (0x00050000)
#define PORTSCX_PTC_BIT_POS                    (16)

/* bit 27-26 are port speed */
#define PORTSCX_PORT_SPEED_FULL                (0x00000000)
#define PORTSCX_PORT_SPEED_LOW                 (0x04000000)
#define PORTSCX_PORT_SPEED_HIGH                (0x08000000)
#define PORTSCX_PORT_SPEED_UNDEF               (0x0C000000)
#define PORTSCX_SPEED_BIT_POS                  (26)

/* bit 28 is parallel transceiver width for UTMI interface */
#define PORTSCX_PTW                            (0x10000000)
#define PORTSCX_PTW_8BIT                       (0x00000000)
#define PORTSCX_PTW_16BIT                      (0x10000000)

/* bit 31-30 are port transceiver select */
#define PORTSCX_PTS_UTMI                       (0x00000000)
#define PORTSCX_PTS_CLASSIC                    (0x40000000)
#define PORTSCX_PTS_ULPI                       (0x80000000)
#define PORTSCX_PTS_FSLS                       (0xC0000000)
#define PORTSCX_PTS_BIT_POS                    (30)

/* USB MODE Register Bit Masks */
#define USBMODE_CTRL_MODE_IDLE                (0x00000000)
#define USBMODE_CTRL_MODE_DEVICE              (0x00000002)
#define USBMODE_CTRL_MODE_HOST                (0x00000003)
#define USBMODE_CTRL_MODE_RSV                 (0x00000001)
#define USBMODE_SETUP_LOCK_OFF                (0x00000008)
#define USBMODE_STREAM_DISABLE                (0x00000010)

/* Endpoint Flush Register */
#define EPFLUSH_TX_OFFSET                      (0x00010000)
#define EPFLUSH_RX_OFFSET                      (0x00000000)

/* Endpoint Setup Status bit masks */
#define EPSETUP_STATUS_MASK                   (0x0000003F)
#define EPSETUP_STATUS_EP0                    (0x00000001)

/* ENDPOINTCTRLx  Register Bit Masks */
#define EPCTRL_TX_ENABLE                       (0x00800000)
#define EPCTRL_TX_DATA_TOGGLE_RST              (0x00400000)    /* Not EP0 */
#define EPCTRL_TX_DATA_TOGGLE_INH              (0x00200000)    /* Not EP0 */
#define EPCTRL_TX_TYPE                         (0x000C0000)
#define EPCTRL_TX_DATA_SOURCE                  (0x00020000)    /* Not EP0 */
#define EPCTRL_TX_EP_STALL                     (0x00010000)
#define EPCTRL_RX_ENABLE                       (0x00000080)
#define EPCTRL_RX_DATA_TOGGLE_RST              (0x00000040)    /* Not EP0 */
#define EPCTRL_RX_DATA_TOGGLE_INH              (0x00000020)    /* Not EP0 */
#define EPCTRL_RX_TYPE                         (0x0000000C)
#define EPCTRL_RX_DATA_SINK                    (0x00000002)    /* Not EP0 */
#define EPCTRL_RX_EP_STALL                     (0x00000001)

/* bit 19-18 and 3-2 are endpoint type */
#define EPCTRL_TX_EP_TYPE_SHIFT                (18)
#define EPCTRL_RX_EP_TYPE_SHIFT                (2)

/* pri_ctrl Register Bit Masks */
#define PRI_CTRL_PRI_LVL1                      (0x0000000C)
#define PRI_CTRL_PRI_LVL0                      (0x00000003)

/* si_ctrl Register Bit Masks */
#define SI_CTRL_ERR_DISABLE                    (0x00000010)
#define SI_CTRL_IDRC_DISABLE                   (0x00000008)
#define SI_CTRL_RD_SAFE_EN                     (0x00000004)
#define SI_CTRL_RD_PREFETCH_DISABLE            (0x00000002)
#define SI_CTRL_RD_PREFEFETCH_VAL              (0x00000001)

/* control Register Bit Masks */
#define USB_CTRL_IOENB                         (0x00000004)
#define USB_CTRL_ULPI_INT0EN                   (0x00000001)

/* OTGSC Register Bit Masks */
#define OTGSC_B_SESSION_VALID                  (0x00000800)
#define OTGSC_A_VBUS_VALID                     (0x00000200)

#define QH_MULT_POS                            (30)
#define QH_ZLT_SEL                             (0x20000000)
#define QH_MAX_PKT_LEN_POS                     (16)
#define QH_IOS                                 (0x00008000)
#define QH_NEXT_TERMINATE                      (0x00000001)
#define QH_IOC                                 (0x00008000)
#define QH_MULTO                               (0x00000C00)
#define QH_STATUS_HALT                         (0x00000040)
#define QH_STATUS_ACTIVE                       (0x00000080)
#define EP_QUEUE_CURRENT_OFFSET_MASK         (0x00000FFF)
#define EP_QUEUE_HEAD_NEXT_POINTER_MASK      (0xFFFFFFE0)
#define EP_QUEUE_FRINDEX_MASK                (0x000007FF)
#define EP_MAX_LENGTH_TRANSFER               (0x4000)

#define DTD_NEXT_TERMINATE                   (0x00000001)
#define DTD_IOC                              (0x00008000)
#define DTD_STATUS_ACTIVE                    (0x00000080)
#define DTD_STATUS_HALTED                    (0x00000040)
#define DTD_STATUS_DATA_BUFF_ERR             (0x00000020)
#define DTD_STATUS_TRANSACTION_ERR           (0x00000008)
#define DTD_RESERVED_FIELDS                  (0x80007300)
#define DTD_ADDR_MASK                        (0xFFFFFFE0)
#define DTD_PACKET_SIZE                      (0x7FFF0000)
#define DTD_LENGTH_BIT_POS                   (16)
#define DTD_ERROR_MASK                       (DTD_STATUS_HALTED | \
                                               DTD_STATUS_DATA_BUFF_ERR | \
                                               DTD_STATUS_TRANSACTION_ERR)

#define DTD_RESERVED_LENGTH_MASK             0x0001ffff
#define DTD_RESERVED_IN_USE                  0x80000000
#define DTD_RESERVED_PIPE_MASK               0x0ff00000
#define DTD_RESERVED_PIPE_OFFSET             20
/*-------------------------------------------------------------------------*/

/* 4 transfer descriptors per endpoint allow 64k transfers, which is the usual MSC
   transfer size, so it seems like a good size */
#define NUM_TDS_PER_EP 4

struct usb_drv_ep_spec usb_drv_ep_specs[USB_NUM_ENDPOINTS]; /* filled in usb_drv_startup */
uint8_t usb_drv_ep_specs_flags = USB_ENDPOINT_SPEC_FORCE_IO_TYPE_MATCH;

/* manual: 32.13.2 Endpoint Transfer Descriptor (dTD) */
struct transfer_descriptor {
    unsigned int next_td_ptr;           /* Next TD pointer(31-5), T(0) set
                                           indicate invalid */
    unsigned int size_ioc_sts;          /* Total bytes (30-16), IOC (15),
                                           MultO(11-10), STS (7-0)  */
    unsigned int buff_ptr0;             /* Buffer pointer Page 0 */
    unsigned int buff_ptr1;             /* Buffer pointer Page 1 */
    unsigned int buff_ptr2;             /* Buffer pointer Page 2 */
    unsigned int buff_ptr3;             /* Buffer pointer Page 3 */
    unsigned int buff_ptr4;             /* Buffer pointer Page 4 */
    unsigned int reserved;
} __attribute__ ((packed));

static struct transfer_descriptor td_array[USB_NUM_ENDPOINTS*2*NUM_TDS_PER_EP]
    USB_DEVBSS_ATTR __attribute__((aligned(32)));

/* manual: 32.13.1 Endpoint Queue Head (dQH) */
struct queue_head {
    unsigned int max_pkt_length;    /* Mult(31-30) , Zlt(29) , Max Pkt len
                                       and IOS(15) */
    unsigned int curr_dtd_ptr;      /* Current dTD Pointer(31-5) */
    struct transfer_descriptor dtd; /* dTD overlay */
    unsigned int setup_buffer[2];   /* Setup data 8 bytes */
    unsigned int reserved;          /* for software use, pointer to the first TD */
    unsigned int status;            /* for software use, status of chain in progress */
    unsigned int length;            /* for software use, transfered bytes of chain in progress */
    unsigned int wait;              /* for softwate use, indicates if the transfer is blocking */
} __attribute__((packed));

static struct queue_head qh_array[USB_NUM_ENDPOINTS*2]
    USB_QHARRAY_ATTR;

static int pending_device_address = -1;

static struct semaphore transfer_completion_signal[USB_NUM_ENDPOINTS*2]
    SHAREDBSS_ATTR;

static const unsigned int pipe2mask[USB_NUM_ENDPOINTS*2] = {
    0x01, 0x010000,
    0x02, 0x020000,
    0x04, 0x040000,
#if USB_NUM_ENDPOINTS > 3
    0x08, 0x080000,
#endif
#if USB_NUM_ENDPOINTS > 4
    0x10, 0x100000,
#endif
};

/*-------------------------------------------------------------------------*/
static void transfer_completed(void);
static void control_received(void);
static void sof_received(void);
static int prime_transfer(int ep_num, void* ptr, int len, bool send, bool wait);
static void prepare_td(struct transfer_descriptor* td,
        struct transfer_descriptor* previous_td, void *ptr, int len,int pipe);
static void bus_reset(void);
static void init_control_queue_heads(void);
/*-------------------------------------------------------------------------*/
static void usb_drv_stop(void)
{
    /* disable interrupts */
    REG_USBINTR = 0;
    /* stop usb controller (disconnect) */
    REG_USBCMD &= ~USBCMD_RUN;
}

static void usb_drv_reset(void)
{
    int oldlevel = disable_irq_save();
    REG_USBCMD &= ~USBCMD_RUN;
    restore_irq(oldlevel);

#ifdef USB_PORTSCX_PHY_TYPE
    /* If a PHY type is specified, set it now */
    REG_PORTSC1 = (REG_PORTSC1 & ~PORTSCX_PHY_TYPE_SEL) | USB_PORTSCX_PHY_TYPE;
#endif
    sleep(HZ/20);
    REG_USBCMD |= USBCMD_CTRL_RESET;
    while (REG_USBCMD & USBCMD_CTRL_RESET);

#if CONFIG_CPU == PP5022 || CONFIG_CPU == PP5024
    /* On a CPU which identifies as a PP5022, this
       initialization must be done after USB is reset.
     */
    outl(inl(0x70000060) | 0xF, 0x70000060);
    outl(inl(0x70000028) | 0x10000, 0x70000028);
    outl(inl(0x70000028) & ~0x10000, 0x70000028);
    outl(inl(0x70000060) & ~0x20, 0x70000060);
    udelay(10);
    outl(inl(0x70000060) | 0x20, 0x70000060);
    udelay(10);
    outl((inl(0x70000060) & ~0xF) | 4, 0x70000060);
    udelay(10);
    outl(inl(0x70000060) & ~0x20, 0x70000060);
    udelay(10);
    outl(inl(0x70000060) & ~0xF, 0x70000060);
    udelay(10);
    outl(inl(0x70000060) | 0x20, 0x70000060);
    udelay(10);
    outl(inl(0x70000028) | 0x800, 0x70000028);
    outl(inl(0x70000028) & ~0x800, 0x70000028);
    while ((inl(0x70000028) & 0x80) == 0);
#endif
}

static void td_set_buf_ptr(struct transfer_descriptor* td, const void* ptr){
    td->buff_ptr0 = (unsigned int)ptr;
    td->buff_ptr1 = ((unsigned int)ptr & 0xfffff000) + 0x1000;
    td->buff_ptr2 = ((unsigned int)ptr & 0xfffff000) + 0x2000;
    td->buff_ptr3 = ((unsigned int)ptr & 0xfffff000) + 0x3000;
    td->buff_ptr4 = ((unsigned int)ptr & 0xfffff000) + 0x4000;
}

/* One-time driver startup init */
void usb_drv_startup(void)
{
    /* Initialize all the signal objects once */
    int i;
    for(i=0;i<USB_NUM_ENDPOINTS*2;i++) {
        semaphore_init(&transfer_completion_signal[i], 1, 0);
    }

    /* Fill the endpoint spec table */
    usb_drv_ep_specs[0].type[DIR_OUT] = USB_ENDPOINT_XFER_CONTROL;
    usb_drv_ep_specs[0].type[DIR_IN] = USB_ENDPOINT_XFER_CONTROL;
    for(int i = 1; i < USB_NUM_ENDPOINTS; i += 1) {
        usb_drv_ep_specs[i].type[DIR_OUT] = USB_ENDPOINT_TYPE_ANY;
        usb_drv_ep_specs[i].type[DIR_IN] = USB_ENDPOINT_TYPE_ANY;
    }
}

#ifdef LOGF_ENABLE
#define XFER_DIR_STR(dir) ((dir) ? "IN" : "OUT")
#define XFER_TYPE_STR(type) \
    ((type) == USB_ENDPOINT_XFER_CONTROL ? "CTRL" : \
     ((type) == USB_ENDPOINT_XFER_ISOC ? "ISOC" : \
      ((type) == USB_ENDPOINT_XFER_BULK ? "BULK" : \
       ((type) == USB_ENDPOINT_XFER_INT ? "INTR" : "INVL"))))
#endif

/* Isochronous transfers logged per pipe since its endpoint was set up: the
 * first few starts, and errors, each up to ISO_LOG_MAX. */
#define ISO_LOG_MAX 8
static uint8_t iso_starts_logged[USB_NUM_ENDPOINTS*2];
static uint8_t iso_errors_logged[USB_NUM_ENDPOINTS*2];

static bool pipe_is_iso(int ep_num, bool send)
{
    unsigned int ctrl = REG_ENDPTCTRL(ep_num);
    int type = send ? (ctrl & EPCTRL_TX_TYPE) >> EPCTRL_TX_EP_TYPE_SHIFT
                    : (ctrl & EPCTRL_RX_TYPE) >> EPCTRL_RX_EP_TYPE_SHIFT;
    return ep_num != EP_CONTROL && type == USB_ENDPOINT_XFER_ISOC;
}

static void init_endpoint(int ep, int type, int mps) {
    const int ep_num = EP_NUM(ep);
    const int ep_dir = EP_DIR(ep);

    logf("ep init: %d %s %s", ep_num, XFER_DIR_STR(ep_dir), XFER_TYPE_STR(type));

    struct queue_head* qh;
    unsigned int ctrl = REG_ENDPTCTRL(ep_num);
    if(ep_dir == DIR_IN) {
        ctrl &= ~EPCTRL_TX_TYPE;
        ctrl |= EPCTRL_TX_DATA_TOGGLE_RST | EPCTRL_TX_ENABLE | type << EPCTRL_TX_EP_TYPE_SHIFT;
        qh = &qh_array[ep_num * 2 + 1];
    } else {
        ctrl &= ~EPCTRL_RX_TYPE;
        ctrl |= EPCTRL_RX_DATA_TOGGLE_RST | EPCTRL_RX_ENABLE | type << EPCTRL_RX_EP_TYPE_SHIFT;
        qh = &qh_array[ep_num * 2];
    }
    REG_ENDPTCTRL(ep_num) = ctrl;

    if(mps == -1) {
        if(type == USB_ENDPOINT_XFER_ISOC) {
            mps = 1024;
        } else {
            mps = usb_drv_port_speed() ? 512 : 64;
        }
    }
    if(type == USB_ENDPOINT_XFER_ISOC)
        /* FIXME: we can adjust the number of packets per frame, currently use one */
        qh->max_pkt_length = mps << QH_MAX_PKT_LEN_POS | QH_ZLT_SEL | 1 << QH_MULT_POS;
    else
        qh->max_pkt_length = mps << QH_MAX_PKT_LEN_POS | QH_ZLT_SEL;

    qh->dtd.next_td_ptr = QH_NEXT_TERMINATE;

    int pipe = ep_num * 2 + (ep_dir == DIR_IN ? 1 : 0);
    iso_starts_logged[pipe] = 0;
    iso_errors_logged[pipe] = 0;
    usb_log(USB_LOG_EP_INIT, ep, type, mps, qh->max_pkt_length);
}


static struct usb_drv_hw_info hw_info;

const struct usb_drv_hw_info *usb_drv_get_hw_info(void)
{
    return &hw_info;
}

/* HWHOST bit 0 is host capability; bits 3:1 are the root port count - 1. */
static void read_hw_info(void)
{
    hw_info.regs[0].name = "ID";
    hw_info.regs[0].val = REG_ID;
    hw_info.regs[1].name = "HWGENERAL";
    hw_info.regs[1].val = REG_HWGENERAL;
    hw_info.regs[2].name = "HWHOST";
    hw_info.regs[2].val = REG_HWHOST;
    hw_info.regs[3].name = "HWDEVICE";
    hw_info.regs[3].val = REG_HWDEVICE;
    hw_info.host_capable = hw_info.regs[2].val & 1;
    hw_info.host_units = ((hw_info.regs[2].val >> 1) & 7) + 1;
    hw_info.nregs = 4;
}

/* Host probe. The controller is an EHCI host with one root port and no
 * transaction translator, so only high-speed devices are expected to
 * enable. PORTSC1's change bits clear when written as 1, and writing PE as
 * 1 does nothing, so every write masks all four. */
#define PORTSCX_WRITE_MASK (PORTSCX_CONNECT_STATUS_CHANGE | \
                            PORTSCX_PORT_ENABLE | \
                            PORTSCX_PORT_EN_DIS_CHANGE | \
                            PORTSCX_OVER_CURRENT_CHG)

static bool host_active;
static bool host_reset_done;
static int host_resets;

/* EHCI 1.0 section 3.5: queue element transfer descriptor */
struct ehci_qtd {
    uint32_t next;
    uint32_t alt_next;
    uint32_t token;
    uint32_t buf[5];
};

/* EHCI 1.0 section 3.6: queue head, padded to 64 bytes */
struct ehci_qh {
    uint32_t link;
    uint32_t chars;
    uint32_t caps;
    uint32_t current;
    struct ehci_qtd overlay;
    uint32_t pad[4];
};

#define EHCI_T          (1 << 0)
#define EHCI_TYP_QH     (1 << 1)
#define QTD_HALTED      (1 << 6)
#define QTD_ACTIVE      (1 << 7)
#define QTD_PID_OUT     (0 << 8)
#define QTD_PID_IN      (1 << 8)
#define QTD_PID_SETUP   (2 << 8)
#define QTD_CERR3       (3 << 10)
#define QTD_IOC         (1 << 15)
#define QTD_BYTES(n)    ((n) << 16)
#define QTD_TOGGLE      (1u << 31)
#define QH_EPS_HIGH     (2 << 12)
#define QH_DTC          (1 << 14)
#define QH_HEAD         (1 << 15)
#define QH_MPS(n)       ((n) << 16)
#define QH_MULT1        (1u << 30)

/* Everything the controller reads or writes. It is only ever touched
 * through its uncached alias, which is also the address the controller
 * takes; the type's alignment pads it to whole cache lines, so no
 * neighbour's write-back can land on it. */
struct host_dma {
    struct ehci_qh qh;
    struct ehci_qtd qtd[3];
    struct usb_ctrlrequest setup;
    uint8_t data[1024];
} __attribute__((aligned(32)));

static struct host_dma host_dma_mem;
static struct host_dma *hd;
static uint32_t host_last_status;

uint32_t usb_drv_host_last_status(void)
{
    return host_last_status;
}

static void qtd_fill(struct ehci_qtd *qtd, void *next, void *alt,
                     uint32_t token, void *buf)
{
    uint32_t a = (uint32_t)buf;

    qtd->next = next ? (uint32_t)next : EHCI_T;
    qtd->alt_next = alt ? (uint32_t)alt : EHCI_T;
    qtd->token = token;
    qtd->buf[0] = a;
    for (int i = 1; i < 5; i++)
        qtd->buf[i] = (a & ~0xfff) + i * 0x1000;
}

/* One control transfer to endpoint 0 of a high-speed device, on an async
 * schedule holding one queue head. Returns the data-stage byte count, or
 * -1 with host_last_status set from the qTD that halted or never
 * finished. */
static int host_control(int addr, int reqtype, int req, int value,
                        int index, int len)
{
    struct ehci_qtd *setup = &hd->qtd[0];
    struct ehci_qtd *data = &hd->qtd[1];
    struct ehci_qtd *status = &hd->qtd[2];
    bool in = reqtype & USB_DIR_IN;
    uint32_t token = QTD_ACTIVE | QTD_CERR3;
    int n = -1;

    hd->setup.bRequestType = reqtype;
    hd->setup.bRequest = req;
    hd->setup.wValue = value;
    hd->setup.wIndex = index;
    hd->setup.wLength = len;

    /* The status stage runs opposite to the data stage, and IN if there is
     * none. A short IN packet skips to it through alt_next. */
    qtd_fill(status, NULL, NULL, token | QTD_IOC | QTD_TOGGLE |
             ((len && in) ? QTD_PID_OUT : QTD_PID_IN), NULL);
    qtd_fill(data, status, status, token | QTD_TOGGLE | QTD_BYTES(len) |
             (in ? QTD_PID_IN : QTD_PID_OUT), hd->data);
    qtd_fill(setup, len ? data : status, NULL,
             token | QTD_PID_SETUP | QTD_BYTES(8), &hd->setup);

    hd->qh.link = (uint32_t)&hd->qh | EHCI_TYP_QH;
    hd->qh.chars = addr | QH_EPS_HIGH | QH_DTC | QH_HEAD | QH_MPS(64);
    hd->qh.caps = QH_MULT1;
    hd->qh.current = 0;
    hd->qh.overlay.next = (uint32_t)setup;
    hd->qh.overlay.alt_next = EHCI_T;
    hd->qh.overlay.token = 0;

    REG_ENDPOINTLISTADDR = (uint32_t)&hd->qh;
    REG_USBCMD |= USBCMD_ASYNC_SCHEDULE_EN;

    for (int t = 0; t < 5000; t++)
    {
        udelay(100);
        if ((setup->token | data->token | status->token) & QTD_HALTED)
            break;
        if (!(status->token & QTD_ACTIVE))
        {
            n = len - ((data->token >> 16) & 0x7fff);
            break;
        }
    }

    if (n < 0)
        host_last_status = (setup->token & (QTD_ACTIVE | QTD_HALTED)) ?
                          setup->token :
                          (len && (data->token & (QTD_ACTIVE | QTD_HALTED))) ?
                          data->token : status->token;

    REG_USBCMD &= ~USBCMD_ASYNC_SCHEDULE_EN;
    for (int t = 0; t < 100 && (REG_USBSTS & USBSTS_ASYNC_SCHEDULE); t++)
        udelay(100);
    return n;
}

int usb_drv_host_control(int addr, int reqtype, int req, int value,
                         int index, void *data, int len)
{
    int n;

    if (!host_active || len > (int)sizeof hd->data)
        return -1;
    if (!(reqtype & USB_DIR_IN))
        memcpy(hd->data, data, len);
    n = host_control(addr, reqtype, req, value, index, len);
    if (n > 0 && (reqtype & USB_DIR_IN))
        memcpy(data, hd->data, n);
    return n;
}

/* EHCI 1.0 section 3.3: isochronous transfer descriptor, one 1 ms frame of
 * one endpoint, one transaction slot per microframe. */
struct ehci_itd {
    uint32_t next;
    uint32_t trans[8];
    uint32_t page[7];
};

#define ITD_ACTIVE      (1u << 31)
#define ITD_ERRORS      (7u << 28)
#define ITD_LEN(n)      ((n) << 16)
#define ITD_DIR_IN      (1 << 11)

/* The periodic schedule. Every frame-list entry for frame f leads to the
 * feedback iTD then the OUT iTD of slot f % ISO_SLOTS. The refill keeps
 * between 2 and ISO_AHEAD frames queued, so it never rewrites the slot the
 * controller is on. Each slot's buffer lies inside one 4 KB page, so every
 * transaction uses page pointer 0. */
#define ISO_SLOTS       32
#define ISO_AHEAD       24
#define ISO_SLOT_BYTES  512

struct host_iso_dma {
    uint32_t framelist[1024];
    struct ehci_itd out[ISO_SLOTS];
    struct ehci_itd fb[ISO_SLOTS];
    uint8_t buf[ISO_SLOTS][ISO_SLOT_BYTES];
    uint32_t fbbuf[ISO_SLOTS];
} __attribute__((aligned(4096)));

static struct host_iso_dma host_iso_mem;
static struct host_iso_dma *iso_dma;
static struct usb_drv_host_iso iso_cfg;
static struct usb_drv_host_iso_stats iso_stats;
static unsigned int iso_next;          /* next frame to fill */
static uint32_t iso_acc;               /* fractional samples, 16.16 */
static bool iso_fb_armed[ISO_SLOTS];

static void itd_init(struct ehci_itd *itd, void *buf, int ep, int dir,
                     int mps)
{
    memset(itd, 0, sizeof *itd);
    itd->next = EHCI_T;
    itd->page[0] = ((uint32_t)buf & ~0xfff) | (ep << 8) | iso_cfg.addr;
    itd->page[1] = dir | mps;
    itd->page[2] = 1;       /* one transaction per microframe */
}

/* A feedback value is 16.16 samples per microframe. One more than an
 * eighth away from nominal is taken as noise, not a rate. */
static void iso_take_feedback(int slot)
{
    uint32_t t = iso_dma->fb[slot].trans[0];
    uint32_t v, nom = iso_cfg.nominal;

    iso_fb_armed[slot] = false;
    if (t & (ITD_ACTIVE | ITD_ERRORS))
    {
        iso_stats.errors += !!(t & ITD_ERRORS);
        return;
    }
    if (((t >> 16) & 0xfff) < 3)
        return;
    v = iso_dma->fbbuf[slot];
    iso_stats.fb_raw = v;
    if (v > nom - nom / 8 && v < nom + nom / 8)
    {
        iso_stats.feedback = v;
        iso_stats.fb_ok++;
    }
    else
        iso_stats.fb_bad++;
}

static void iso_fill_slot(int slot, unsigned int frame)
{
    struct ehci_itd *o = &iso_dma->out[slot];
    struct ehci_itd *f = &iso_dma->fb[slot];
    uint8_t *buf = iso_dma->buf[slot];
    uint32_t base = (uint32_t)buf & 0xfff;
    int step = iso_cfg.interval_out;
    int max = MIN(iso_cfg.mps_out, ISO_SLOT_BYTES / (8 / step)) /
              iso_cfg.frame_bytes;
    int off = 0;

    for (int m = 0; m < 8; m++)
    {
        if (o->trans[m] & ITD_ERRORS)
            iso_stats.errors++;
        o->trans[m] = 0;
    }
    if (iso_fb_armed[slot])
        iso_take_feedback(slot);

    for (int m = 0; m < 8; m += step)
    {
        int n, bytes;

        iso_acc += iso_stats.feedback * step;
        n = MIN((int)(iso_acc >> 16), max);
        iso_acc &= 0xffff;
        bytes = n * iso_cfg.frame_bytes;
        iso_cfg.fill(buf + off, n);
        o->trans[m] = ITD_ACTIVE | ITD_LEN(bytes) | (base + off);
        off += bytes;
    }

    if (iso_cfg.ep_fb &&
        frame % MAX(1, iso_cfg.interval_fb / 8) == 0)
    {
        f->trans[0] = ITD_ACTIVE | ITD_LEN(iso_cfg.mps_fb) |
                      ((uint32_t)&iso_dma->fbbuf[slot] & 0xfff);
        iso_fb_armed[slot] = true;
    }
}

static void iso_tick(void)
{
    unsigned int cur = (REG_FRINDEX >> 3) & 1023;
    unsigned int ahead = (iso_next - cur) & 1023;

    if (!(REG_PORTSC1 & PORTSCX_CURRENT_CONNECT_STATUS))
    {
        if (!iso_stats.lost)
        {
            iso_stats.lost = true;
            if (iso_cfg.lost)
                iso_cfg.lost();
        }
        return;
    }
    if (iso_cfg.begin && !iso_cfg.begin())
        return;

    if (ahead < 2 || ahead > ISO_AHEAD)
    {
        if (ahead < 2 || ahead > 512)
        {
            iso_stats.underruns++;
            iso_next = (cur + 4) & 1023;
        }
        else
            return;
    }
    while (((iso_next - cur) & 1023) < ISO_AHEAD)
    {
        iso_fill_slot(iso_next % ISO_SLOTS, iso_next);
        iso_next = (iso_next + 1) & 1023;
        iso_stats.frames++;
    }
}

bool usb_drv_host_iso_start(const struct usb_drv_host_iso *iso)
{
    if (!host_active || iso_stats.running || iso->frame_bytes <= 0 ||
        iso->interval_out < 1 || iso->interval_out > 8)
        return false;

    iso_dma = UNCACHED_ADDR(&host_iso_mem);
    iso_cfg = *iso;
    memset(&iso_stats, 0, sizeof iso_stats);
    memset(iso_fb_armed, 0, sizeof iso_fb_armed);
    iso_stats.feedback = iso->nominal;
    iso_acc = 0;

    for (int s = 0; s < ISO_SLOTS; s++)
    {
        itd_init(&iso_dma->out[s], iso_dma->buf[s], iso->ep_out, 0,
                 iso->mps_out);
        itd_init(&iso_dma->fb[s], &iso_dma->fbbuf[s], iso->ep_fb & 0xf,
                 ITD_DIR_IN, iso->mps_fb);
        iso_dma->fb[s].next = (uint32_t)&iso_dma->out[s];
    }
    for (int i = 0; i < 1024; i++)
        iso_dma->framelist[i] = (uint32_t)&iso_dma->fb[i % ISO_SLOTS];

    REG_DEVICEADDR = (uint32_t)iso_dma->framelist;    /* PERIODICLISTBASE */
    iso_next = (((REG_FRINDEX >> 3) + 4) & 1023);
    iso_tick();
    REG_USBCMD |= USBCMD_PERIODIC_SCHEDULE_EN;
    for (int t = 0; t < 100 && !(REG_USBSTS & USBSTS_PERIODIC_SCHEDULE); t++)
        udelay(100);
    iso_stats.running = true;
    tick_add_task(iso_tick);
    return true;
}

void usb_drv_host_iso_stop(void)
{
    if (!iso_stats.running)
        return;
    tick_remove_task(iso_tick);
    iso_stats.running = false;
    REG_USBCMD &= ~USBCMD_PERIODIC_SCHEDULE_EN;
    for (int t = 0; t < 100 && (REG_USBSTS & USBSTS_PERIODIC_SCHEDULE); t++)
        udelay(100);
}

void usb_drv_host_iso_set_nominal(uint32_t nominal)
{
    int oldlevel = disable_irq_save();
    iso_cfg.nominal = nominal;
    iso_stats.feedback = nominal;
    restore_irq(oldlevel);
}

void usb_drv_host_iso_get_stats(struct usb_drv_host_iso_stats *st)
{
    *st = iso_stats;
}

void usb_drv_host_start(void)
{
    commit_discard_dcache();
    hd = UNCACHED_ADDR(&host_dma_mem);
#ifdef HAVE_USB_HOST
    usb_host_enum_clear();
#endif

    usb_drv_int_enable(false);
    usb_drv_reset();
    REG_USBMODE = USBMODE_CTRL_MODE_HOST;
    REG_USBINTR = 0;
    REG_PORTSC1 = (REG_PORTSC1 & ~PORTSCX_WRITE_MASK) | PORTSCX_PORT_POWER;
    REG_USBCMD |= USBCMD_RUN;
    host_reset_done = false;
    host_resets = 0;
    host_active = true;
}

void usb_drv_host_stop(void)
{
    usb_drv_host_iso_stop();
    host_active = false;
    REG_PORTSC1 &= ~(PORTSCX_WRITE_MASK | PORTSCX_PORT_POWER);
    REG_USBCMD &= ~USBCMD_RUN;
    REG_USBCMD |= USBCMD_CTRL_RESET;
    while (REG_USBCMD & USBCMD_CTRL_RESET);
}

void usb_drv_host_poll(struct usb_drv_host_status *st)
{
    static const char * const speeds[] = { "full", "low", "high", "?" };
    unsigned int portsc;

    st->active = host_active;
    if (!host_active)
        return;

    portsc = REG_PORTSC1;
    if (!(portsc & PORTSCX_CURRENT_CONNECT_STATUS))
    {
        usb_drv_host_iso_stop();
        host_reset_done = false;
#ifdef HAVE_USB_HOST
        usb_host_enum_clear();
#endif
    }
    else if (!host_reset_done)
    {
        /* 100 ms connect debounce, then a 60 ms root-port reset. The
         * controller may time the reset itself; if not, end it here. */
        host_reset_done = true;
        host_resets++;
        udelay(100000);
        REG_PORTSC1 = (REG_PORTSC1 & ~PORTSCX_WRITE_MASK) | PORTSCX_PORT_RESET;
        udelay(60000);
        if (REG_PORTSC1 & PORTSCX_PORT_RESET)
            REG_PORTSC1 &= ~(PORTSCX_WRITE_MASK | PORTSCX_PORT_RESET);
        udelay(10000);
        portsc = REG_PORTSC1;
#ifdef HAVE_USB_HOST
        if (portsc & PORTSCX_PORT_ENABLE)
        {
            usb_host_enumerate();
            portsc = REG_PORTSC1;
        }
#endif
    }

    st->host_mode = (REG_USBMODE & 3) == USBMODE_CTRL_MODE_HOST;
    st->vbus = REG_OTGSC & (OTGSC_A_VBUS_VALID | OTGSC_B_SESSION_VALID);
    st->connected = portsc & PORTSCX_CURRENT_CONNECT_STATUS;
    st->enabled = portsc & PORTSCX_PORT_ENABLE;
    st->line = (portsc >> 10) & 3;
    st->speed = st->enabled ? speeds[(portsc >> 26) & 3] : "-";
    st->resets = host_resets;
    st->regs[0].name = "PORTSC1";
    st->regs[0].val = portsc;
    st->regs[1].name = "OTGSC";
    st->regs[1].val = REG_OTGSC;
    st->regs[2].name = "USBSTS";
    st->regs[2].val = REG_USBSTS;
    st->regs[3].name = "USBMODE";
    st->regs[3].val = REG_USBMODE;
    st->nregs = 4;
}

/* manual: 32.14.1 Device Controller Initialization */
void usb_drv_init(void)
{
    read_hw_info();

    /* USB core decides */
    usb_drv_reset();

    REG_USBMODE = USBMODE_CTRL_MODE_DEVICE;

#ifdef USB_NO_HIGH_SPEED
    /* Force device to full speed */
    /* See 32.9.5.9.2 */
    REG_PORTSC1 |= PORTSCX_PORT_FORCE_FULL_SPEED;
#endif

    init_control_queue_heads();
    memset(td_array, 0, sizeof td_array);

    REG_ENDPOINTLISTADDR = (unsigned int)qh_array;
    REG_DEVICEADDR = 0;

    /* enable USB interrupts */
    REG_USBINTR =
        USBINTR_INT_EN |
        USBINTR_ERR_INT_EN |
        USBINTR_PTC_DETECT_EN |
        USBINTR_RESET_EN;

    usb_drv_int_enable(true);

    /* go go go */
    REG_USBCMD |= USBCMD_RUN;

    logf("usb_drv_init() finished");
    logf("usb id %x", REG_ID);
    logf("usb dciversion %x", REG_DCIVERSION);
    logf("usb dccparams %x", REG_DCCPARAMS);

    /* now a bus reset will occur. see bus_reset() */

    /* manual: 32.9.5.18 (Caution): Leaving an unconfigured endpoint control
     * will cause undefined behavior for the data pid tracking on the active
     * endpoint/direction. */
    for(int ep_num=1;ep_num<USB_NUM_ENDPOINTS;ep_num++) {
        init_endpoint(ep_num | USB_DIR_IN, USB_ENDPOINT_XFER_BULK, -1);
        init_endpoint(ep_num | USB_DIR_OUT, USB_ENDPOINT_XFER_BULK, -1);
    }
}

void usb_drv_exit(void)
{
    usb_drv_stop();

    /* TODO : is one of these needed to save power ?
    REG_PORTSC1 |= PORTSCX_PHY_LOW_POWER_SPD;
    REG_USBCMD |= USBCMD_CTRL_RESET;
    */

    usb_drv_int_enable(false);
}

void usb_drv_int(void)
{
    unsigned int usbintr = REG_USBINTR; /* Only watch enabled ints */
    unsigned int status = REG_USBSTS & usbintr;

#if 0
    if (status & USBSTS_INT) logf("int: usb ioc");
    if (status & USBSTS_ERR) logf("int: usb err");
    if (status & USBSTS_PORT_CHANGE) logf("int: portchange");
    if (status & USBSTS_RESET) logf("int: reset");
#endif

    /* usb transaction interrupt */
    if (status & USBSTS_INT) {
        REG_USBSTS = USBSTS_INT;

        /* a control packet? */
        if (REG_ENDPTSETUPSTAT & EPSETUP_STATUS_EP0) {
            control_received();
        }

        if (REG_ENDPTCOMPLETE)
            transfer_completed();
    }

    /* error interrupt */
    if (status & USBSTS_ERR) {
        REG_USBSTS = USBSTS_ERR;
        logf("usb error int");
    }

    /* reset interrupt */
    if (status & USBSTS_RESET) {
        REG_USBSTS = USBSTS_RESET;
        bus_reset();
        usb_core_bus_reset(); /* tell mom */
    }

    /* port change */
    if (status & USBSTS_PORT_CHANGE) {
        REG_USBSTS = USBSTS_PORT_CHANGE;
    }

    /* sof */
    if (status & USBSTS_SOF) {
        REG_USBSTS = USBSTS_SOF;
        sof_received();
    }
}

bool usb_drv_stalled(int endpoint,bool in)
{
    if(in) {
        return ((REG_ENDPTCTRL(EP_NUM(endpoint)) & EPCTRL_TX_EP_STALL)!=0);
    }
    else {
        return ((REG_ENDPTCTRL(EP_NUM(endpoint)) & EPCTRL_RX_EP_STALL)!=0);
    }

}
void usb_drv_stall(int endpoint, bool stall, bool in)
{
    int ep_num = EP_NUM(endpoint);

    logf("%sstall %d", stall ? "" : "un", ep_num);

    if(in) {
        if (stall) {
            REG_ENDPTCTRL(ep_num) |= EPCTRL_TX_EP_STALL;
        }
        else {
            REG_ENDPTCTRL(ep_num) &= ~EPCTRL_TX_EP_STALL;
        }
    }
    else {
        if (stall) {
            REG_ENDPTCTRL(ep_num) |= EPCTRL_RX_EP_STALL;
        }
        else {
            REG_ENDPTCTRL(ep_num) &= ~EPCTRL_RX_EP_STALL;
        }
    }
}

int usb_drv_send_nonblocking(int endpoint, void* ptr, int length)
{
    return prime_transfer(EP_NUM(endpoint), ptr, length, true, false);
}

int usb_drv_send(int endpoint, void* ptr, int length)
{
    return prime_transfer(EP_NUM(endpoint), ptr, length, true, true);
}

int usb_drv_recv_nonblocking(int endpoint, void* ptr, int length)
{
    //logf("usbrecv(%x, %d)", ptr, length);
    return prime_transfer(EP_NUM(endpoint), ptr, length, false, false);
}

int usb_drv_recv_blocking(int endpoint, void* ptr, int length)
{
    return prime_transfer(EP_NUM(endpoint), ptr, length, false, true);
}

int usb_drv_port_speed(void)
{
    return (REG_PORTSC1 & 0x08000000) ? 1 : 0;
}

int usb_drv_get_frame_number(void)
{
    /* the lower 3 bits store the microframe (in HS mode), discard them */
    return (REG_FRINDEX & USB_FRINDEX_MASKS) >> 3;
}

bool usb_drv_connected(void)
{
    return (REG_PORTSC1 &
        (PORTSCX_PORT_SUSPEND | PORTSCX_CURRENT_CONNECT_STATUS))
            == PORTSCX_CURRENT_CONNECT_STATUS;
}

bool usb_drv_powered(void)
{
    /* true = bus 4V4 ok */
    return (REG_OTGSC & OTGSC_A_VBUS_VALID) ? true : false;
}

void usb_drv_set_address(int address)
{
    /* SET_ADDRESS is captured when the setup packet arrives and applied
     * after the EP0 IN status stage completes. */
    (void)address;
}

void usb_drv_reset_endpoint(int endpoint, bool send)
{
    int pipe = EP_NUM(endpoint) * 2 + (send ? 1 : 0);
    unsigned int mask = pipe2mask[pipe];
    REG_ENDPTFLUSH = mask;
    while (REG_ENDPTFLUSH & mask);
}

void usb_drv_set_test_mode(int mode)
{
    switch(mode){
        case 0:
            REG_PORTSC1 &= ~PORTSCX_PORT_TEST_CTRL;
            break;
        case 1:
            REG_PORTSC1 |= PORTSCX_PTC_JSTATE;
            break;
        case 2:
            REG_PORTSC1 |= PORTSCX_PTC_KSTATE;
            break;
        case 3:
            REG_PORTSC1 |= PORTSCX_PTC_SE0NAK;
            break;
        case 4:
            REG_PORTSC1 |= PORTSCX_PTC_PACKET;
            break;
        case 5:
            REG_PORTSC1 |= PORTSCX_PTC_FORCE_EN;
            break;
    }
    usb_drv_reset();
    REG_USBCMD |= USBCMD_RUN;
}

/* batched request api  */
static struct transfer_descriptor batch_td_array[USB_BATCH_SLOTS] USB_DEVBSS_ATTR __attribute__((aligned(32)));

static uint8_t batch_ep = 0;
static uint8_t batch_write_cursor;
static bool batch_stopped;
static usb_drv_batch_get_more batch_get_more;

static int ep_to_pipe_index(uint8_t ep) {
    const int ep_num = EP_NUM(ep);
    const int ep_dir = EP_DIR(ep);
    const int pipe = ep_num * 2 + (ep_dir == DIR_IN ? 1 : 0);
    return pipe;
}

int usb_drv_batch_init(int ep, usb_drv_batch_get_more get_more) {
    logf("batch init");
    if(batch_ep != 0) {
        logf("batch function not available");
        return -1;
    }
    batch_ep = ep;
    batch_get_more = get_more;
    return 0;
}

int usb_drv_batch_deinit() {
    logf("batch deinit");
    usb_drv_batch_stop();
    batch_ep = 0;
    return 0;
}

/* returns whether priming is needed */
static bool batch_fill_tds(void) {
    while(!(batch_td_array[batch_write_cursor].size_ioc_sts & DTD_STATUS_ACTIVE)) {
        /* batch_get_more may call batch_stop() through:
        *  - batch_get_more()
        *   - pcm_play_dma_complete_callback()
        *    - sink_stop()
        *     - usb_drv_batch_stop() */
        const void* ptr;
        size_t len;
        batch_get_more(&ptr, &len);
        if(len == 0 || batch_stopped) {
            return false;
        }

        struct transfer_descriptor* td = &batch_td_array[batch_write_cursor];
        td_set_buf_ptr(td, ptr);
        td->size_ioc_sts = (len << DTD_LENGTH_BIT_POS) | DTD_STATUS_ACTIVE;
        batch_write_cursor = (batch_write_cursor + 1)  % USB_BATCH_SLOTS;
    }
    return true;
}

int usb_drv_batch_start(void) {
    logf("batch start");

    /* reset variables */
    batch_write_cursor = 0;
    batch_stopped = false;

    /* chain tds */
    memset(batch_td_array, 0, sizeof(struct transfer_descriptor) * USB_BATCH_SLOTS);
    for(int i = 0; i < USB_BATCH_SLOTS; i += 1) {
        struct transfer_descriptor* td = &batch_td_array[i];
        td->next_td_ptr = (unsigned int)&batch_td_array[(i + 1) % USB_BATCH_SLOTS];
    }

    /* configure queue head */
    const int pipe = ep_to_pipe_index(batch_ep);
    struct queue_head* const qh = &qh_array[pipe];

    qh->curr_dtd_ptr = (unsigned int)&batch_td_array[0]; /* or error check in sof_received may read random location */
    qh->dtd.next_td_ptr = (unsigned int)&batch_td_array[0];
    qh->dtd.size_ioc_sts = 0;

    /* pull initial buffers */
    batch_fill_tds();

    /* monitor sof */
    REG_USBINTR |= USBINTR_SOF_EN;
    return 0;
}

int usb_drv_batch_stop(void) {
    batch_stopped = true;

    /* disable sof interrupt */
    REG_USBINTR &= ~USBINTR_SOF_EN;

    /* break the chain */
    /* terminating qh->dtd.next_td_ptr is not reliable */
    for(int i = 0; i < USB_BATCH_SLOTS; i += 1) {
        struct transfer_descriptor* td = &batch_td_array[i];
        td->next_td_ptr = DTD_NEXT_TERMINATE;
    }

    /* flush endpoint */
    const int pipe = ep_to_pipe_index(batch_ep);
    const unsigned int mask = pipe2mask[pipe];

    REG_ENDPTFLUSH = mask;
    while (REG_ENDPTFLUSH & mask);

    return 0;
}

#if defined(LOGF_ENABLE) && defined(ROCKBOX_HAS_LOGF)
void usb_drv_dump_regs(void) {
    logf("==== register dump %ld ====", current_tick);
    logf("USBSTS         0x%08X", REG_USBSTS);
    logf("ENDPTSETUPSTAT 0x%08X", REG_ENDPTSETUPSTAT);
    logf("ENDPTPRIME     0x%08X", REG_ENDPTPRIME);
    logf("ENDPTFLUSH     0x%08X", REG_ENDPTFLUSH);
    logf("ENDPTSTATUS    0x%08X", REG_ENDPTSTATUS);
    logf("ENDPTCOMPLETE  0x%08X", REG_ENDPTCOMPLETE);
    logf("ENDPTCTRL0     0x%08X", REG_ENDPTCTRL0);
    logf("ENDPTCTRL1     0x%08X", REG_ENDPTCTRL1);
    logf("ENDPTCTRL2     0x%08X", REG_ENDPTCTRL2);
}

static void dump_td_array(struct queue_head* qh, struct transfer_descriptor* tds, size_t size) {
    void* current = (void*)qh->curr_dtd_ptr;
    void* next = (void*)qh->dtd.next_td_ptr;

    logf("==== td dump %ld n=%p c=%p ====", current_tick, next, current);
    for(int i = 0; i < size; i += 1) {
        int len = (tds[i].size_ioc_sts & DTD_PACKET_SIZE) >> DTD_LENGTH_BIT_POS;
        char sts[] = {
            &tds[i] == current ? 'c' : ' ',
            &tds[i] == next ? 'n' : ' ',
            i == batch_write_cursor ? 'w' : ' ',
            '\0',
        };
        logf("%s td[%02d] status=0x%08X len=%d", sts, i, tds[i].size_ioc_sts, len);
    }
}

void usb_drv_dump_tds(int ep) {
    const int pipe = ep_to_pipe_index(ep);
    struct queue_head* const qh = &qh_array[pipe];
    dump_td_array(qh, &td_array[pipe * NUM_TDS_PER_EP], NUM_TDS_PER_EP);
}

void usb_drv_batch_dump_tds(void) {
    const int pipe = ep_to_pipe_index(batch_ep);
    struct queue_head* const qh = &qh_array[pipe];
    dump_td_array(qh, batch_td_array, USB_BATCH_SLOTS);
}
#endif

static void sof_received(void) {
    if(batch_ep == 0) {
        /* should not happen */
        return;
    }

    /* error recovery */
    const int pipe = ep_to_pipe_index(batch_ep);
    const unsigned int mask = pipe2mask[pipe];
    struct queue_head* const qh = &qh_array[pipe];
    struct transfer_descriptor* const td = (void*)qh->curr_dtd_ptr;
    if(td->size_ioc_sts & DTD_ERROR_MASK) {
        logf("td error status=0x%08X", td->size_ioc_sts);
        REG_ENDPTPRIME |= mask;
    }

    /* do refill */
    if(batch_fill_tds()) {
        const int pipe = ep_to_pipe_index(batch_ep);
        const unsigned int mask = pipe2mask[pipe];
        REG_ENDPTPRIME |= mask;
    }
}

/*-------------------------------------------------------------------------*/

/* manual: 32.14.5.2 */
static int prime_transfer(int ep_num, void* ptr, int len, bool send, bool wait)
{
    int rc = 0;
    int pipe = ep_num * 2 + (send ? 1 : 0);
    unsigned int mask = pipe2mask[pipe];
    struct queue_head* qh = &qh_array[pipe];
    static long last_tick;
    struct transfer_descriptor *new_td, *cur_td, *prev_td;

    int oldlevel = disable_irq_save();
/*
    if (send && ep_num > EP_CONTROL) {
        logf("usb: sent %d bytes", len);
    }
*/
    qh->status = 0;
    qh->wait = wait;

    new_td=&td_array[pipe*NUM_TDS_PER_EP];
    cur_td=new_td;
    prev_td=0;
    int tdlen;

    do
    {
        tdlen=MIN(len,16384);
        prepare_td(cur_td, prev_td, ptr, tdlen,pipe);
        ptr+=tdlen;
        prev_td=cur_td;
        cur_td++;
        len-=tdlen;
    }
    while(len>0);
    //logf("starting ep %d %s",ep_num,send?"send":"receive");

    qh->dtd.next_td_ptr = (unsigned int)new_td;
    qh->dtd.size_ioc_sts &= ~(QH_STATUS_HALT | QH_STATUS_ACTIVE);

    REG_ENDPTPRIME |= mask;

    if(ep_num == EP_CONTROL && (REG_ENDPTSETUPSTAT & EPSETUP_STATUS_EP0)) {
        /* 32.14.3.2.2 */
        logf("new setup arrived");
        rc = -4;
        goto pt_error;
    }

    last_tick = current_tick;
    while ((REG_ENDPTPRIME & mask)) {
        if (REG_USBSTS & USBSTS_RESET) {
            rc = -1;
            goto pt_error;
        }

        if (TIME_AFTER(current_tick, last_tick + HZ/4)) {
            logf("prime timeout");
            rc = -2;
            goto pt_error;
        }
    }

    if (!(REG_ENDPTSTATUS & mask)) {
        if(REG_ENDPTCOMPLETE & mask)
        {
            logf("endpoint completed fast! %d %d %x", ep_num, pipe, qh->dtd.size_ioc_sts & 0xff);
        }
        else
        {
            logf("no prime! %d %d %x", ep_num, pipe, qh->dtd.size_ioc_sts & 0xff);
            rc = -3;
            goto pt_error;
        }
    }
    if(ep_num == EP_CONTROL && (REG_ENDPTSETUPSTAT & EPSETUP_STATUS_EP0)) {
        /* 32.14.3.2.2 */
        logf("new setup arrived");
        rc = -4;
        goto pt_error;
    }

    if (pipe_is_iso(ep_num, send) && iso_starts_logged[pipe] < ISO_LOG_MAX) {
        iso_starts_logged[pipe]++;
        usb_log(USB_LOG_ISO_XFER, ep_num | (send ? USB_DIR_IN : 0),
                usb_drv_get_frame_number(), REG_ENDPTSTATUS,
                REG_ENDPTCTRL(ep_num));
    }

    restore_irq(oldlevel);

    if (wait) {
        /* wait for transfer to finish */
        semaphore_wait(&transfer_completion_signal[pipe], TIMEOUT_BLOCK);
        if(qh->status!=0) {
            /* No need to cancel wait here since it was done and the signal
             * came. */
            return -5;
        }
        //logf("all tds done");
    }

pt_error:
    if (rc < 0 && ep_num != EP_CONTROL)
        usb_log(USB_LOG_XFER_FAIL, ep_num | (send ? USB_DIR_IN : 0), -rc,
                REG_ENDPTPRIME, REG_ENDPTCTRL(ep_num));
    if(rc<0)
        restore_irq(oldlevel);

    /* Error status must make sure an abandoned wakeup signal isn't left */
    if (rc < 0 && wait) {
        /* Cancel wait */
        qh->wait = 0;
        /* Make sure to remove any signal if interrupt fired before we zeroed
         * qh->wait. Could happen during a bus reset for example. */
        semaphore_wait(&transfer_completion_signal[pipe], TIMEOUT_NOBLOCK);
    }

    return rc;
}

void usb_drv_cancel_all_transfers(void)
{
    int i;
    REG_ENDPTFLUSH = ~0;
    while (REG_ENDPTFLUSH);

    memset(td_array, 0, sizeof td_array);
    for(i=0;i<USB_NUM_ENDPOINTS*2;i++) {
        if(qh_array[i].wait) {
            qh_array[i].wait=0;
            qh_array[i].status=DTD_STATUS_HALTED;
            semaphore_release(&transfer_completion_signal[i]);
        }
    }
}

void usb_drv_ep_init(const struct usb_drv_ep_alloc_ctx* ctx, int ep) {
    const int ep_num = EP_NUM(ep);
    const int ep_dir = EP_DIR(ep);
    init_endpoint(ep, ctx->type[ep_num][ep_dir], ctx->max_packet_size[ep_num][ep_dir]);
}

void usb_drv_ep_deinit(const struct usb_drv_ep_alloc_ctx* ctx, int ep) {
    (void)ctx;
    int ep_num = EP_NUM(ep);
    int ep_dir = EP_DIR(ep);

    logf("ep deinit: %d %s", ep_num, XFER_DIR_STR(ep_dir));

    if(ep_dir == DIR_IN) {
        REG_ENDPTCTRL(ep_num) &= ~EPCTRL_TX_ENABLE & ~EPCTRL_TX_TYPE;
    } else {
        REG_ENDPTCTRL(ep_num) &= ~EPCTRL_RX_ENABLE & ~EPCTRL_RX_TYPE;
    }
}

static void prepare_td(struct transfer_descriptor* td,
                       struct transfer_descriptor* previous_td,
                       void *ptr, int len,int pipe)
{
    //logf("adding a td : %d",len);
    /* FIXME td allow iso packets per frame override but we don't use it here */
    memset(td, 0, sizeof(struct transfer_descriptor));
    td->next_td_ptr = DTD_NEXT_TERMINATE;
    td->size_ioc_sts = (len<< DTD_LENGTH_BIT_POS) |
        DTD_STATUS_ACTIVE | DTD_IOC;
    td_set_buf_ptr(td, ptr);
    td->reserved |= DTD_RESERVED_LENGTH_MASK & len;
    td->reserved |= DTD_RESERVED_IN_USE;
    td->reserved |= (pipe << DTD_RESERVED_PIPE_OFFSET);

    if (previous_td != 0) {
        previous_td->next_td_ptr=(unsigned int)td;
        previous_td->size_ioc_sts&=~DTD_IOC;
    }
}

static void control_received(void)
{
    int i;
    /* copy setup data from packet */
    static unsigned int tmp[2];
    tmp[0] = qh_array[0].setup_buffer[0];
    tmp[1] = qh_array[0].setup_buffer[1];

    /* acknowledge packet recieved */
    REG_ENDPTSETUPSTAT = EPSETUP_STATUS_EP0;

    /* Stop pending control transfers */
    for(i=0;i<2;i++) {
        if(qh_array[i].wait) {
            qh_array[i].wait=0;
            qh_array[i].status=DTD_STATUS_HALTED;
            semaphore_release(&transfer_completion_signal[i]);
        }
    }

    struct usb_ctrlrequest *req = (struct usb_ctrlrequest*)tmp;

    /* A new setup packet supersedes any unfinished SET_ADDRESS request. */
    pending_device_address = -1;

    if ((req->bRequestType & (USB_TYPE_MASK | USB_RECIP_MASK)) ==
            (USB_TYPE_STANDARD | USB_RECIP_DEVICE) &&
        req->bRequest == USB_REQ_SET_ADDRESS)
        pending_device_address = req->wValue;

    usb_core_setup_received(req);
}

static void transfer_completed(void)
{
    int ep;
    unsigned int mask = REG_ENDPTCOMPLETE;
    REG_ENDPTCOMPLETE = mask;

    for (ep=0; ep<USB_NUM_ENDPOINTS; ep++) {
        int dir;
        for (dir=0; dir<2; dir++) {
            int pipe = ep * 2 + dir;
            if (mask & pipe2mask[pipe]) {
                struct queue_head* qh = &qh_array[pipe];

                int length=0;
                struct transfer_descriptor* td=&td_array[pipe*NUM_TDS_PER_EP];
                if ((td->size_ioc_sts & DTD_ERROR_MASK) &&
                    pipe_is_iso(ep, dir) &&
                    iso_errors_logged[pipe] < ISO_LOG_MAX) {
                    iso_errors_logged[pipe]++;
                    usb_log(USB_LOG_ISO_ERROR, ep | (dir ? USB_DIR_IN : 0), 0,
                            td->size_ioc_sts, 0);
                }
                while(td!=(struct transfer_descriptor*)DTD_NEXT_TERMINATE && td!=0)
                {
                    /* It seems that the controller sets the pipe bit to one even if the TD
                     * dosn't have the IOC bit set. So we have the rely the active status bit
                     * to check that all the TDs of the transfer are really finished and let
                     * the transfer continue if it's no the case */
                    if(td->size_ioc_sts & DTD_STATUS_ACTIVE)
                    {
                        logf("skip half finished transfer");
                        goto Lskip;
                    }
                    length += ((td->reserved & DTD_RESERVED_LENGTH_MASK) -
                        ((td->size_ioc_sts & DTD_PACKET_SIZE) >> DTD_LENGTH_BIT_POS));
                    td=(struct transfer_descriptor*) td->next_td_ptr;
                }
                if (ep == EP_CONTROL && dir == DIR_IN &&
                    pending_device_address >= 0 &&
                    qh->status == 0 && length == 0) {
                    REG_DEVICEADDR =
                        pending_device_address << USBDEVICEADDRESS_BIT_POS;
                    pending_device_address = -1;
                }

                if(qh->wait) {
                    qh->wait=0;
                    semaphore_release(&transfer_completion_signal[pipe]);
                }

                usb_core_transfer_complete(ep, dir?USB_DIR_IN:USB_DIR_OUT,
                        qh->status, length);
                Lskip:
                continue;
            }
        }
    }
}

/* manual: 32.14.2.1 Bus Reset */
static void bus_reset(void)
{
    int i;
    logf("usb bus_reset");

    REG_DEVICEADDR = 0;
    pending_device_address = -1;
    REG_ENDPTSETUPSTAT = REG_ENDPTSETUPSTAT;
    REG_ENDPTCOMPLETE  = REG_ENDPTCOMPLETE;

    for (i=0; i<100; i++) {
        if (!REG_ENDPTPRIME)
            break;

        if (REG_USBSTS & USBSTS_RESET) {
            logf("usb: double reset");
            return;
        }

        udelay(100);
    }
    if (REG_ENDPTPRIME) {
        logf("usb: short reset timeout");
    }

    usb_drv_cancel_all_transfers();

    if (!(REG_PORTSC1 & PORTSCX_PORT_RESET)) {
        logf("usb: slow reset!");
    }
}

/* manual: 32.14.4.1 Queue Head Initialization */
static void init_control_queue_heads(void)
{
    memset(qh_array, 0, sizeof qh_array);

    /*** control ***/
    qh_array[EP_CONTROL].max_pkt_length = 64 << QH_MAX_PKT_LEN_POS | QH_IOS | QH_ZLT_SEL;
    qh_array[EP_CONTROL].dtd.next_td_ptr = QH_NEXT_TERMINATE;
    qh_array[EP_CONTROL+1].max_pkt_length = 64 << QH_MAX_PKT_LEN_POS | QH_ZLT_SEL;
    qh_array[EP_CONTROL+1].dtd.next_td_ptr = QH_NEXT_TERMINATE;
}
