/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2009-2014 by Michael Sparmann
 * Copyright © 2010 Amaury Pouly
 * Copyright (C) 2014 by Marcin Bukat
 * Copyright (C) 2016 by Cástor Muñoz
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
#include <inttypes.h>
#include <string.h>

#include "usb-designware.h"

#include "config.h"
#include "cpu.h"
#include "system.h"
#include "kernel.h"
#include "panic.h"
#include "power.h"
#include "usb.h"
#include "usb_drv.h"
#include "usb_ch9.h"
#include "usb_core.h"
#include "timer.h"

/* Define LOGF_ENABLE to enable logf output in this file */
/*#define LOGF_ENABLE*/
#include "logf.h"

/* The ARM940T uses a subset of the ARMv4 functions, not
 * supporting clean/invalidate cache entries using MVA.
 */
#if CONFIG_CPU == S5L8701
#define DISCARD_DCACHE_RANGE(b,s)   commit_discard_dcache()
#define COMMIT_DCACHE_RANGE(b,s)    commit_dcache()
#else
#define DISCARD_DCACHE_RANGE(b,s)   discard_dcache_range(b,s)
#define COMMIT_DCACHE_RANGE(b,s)    commit_dcache_range(b,s)
#endif

/* USB_DW_PHYSADDR(x) converts the address of buffer x to one usable with DMA.
 * For example, converting a virtual address to a physical address.
 *
 * USB_DW_UNCACHEDADDR(x) is used to get an uncached pointer to a buffer.
 * If the platform doesn't support this, define NO_UNCACHED_ADDR instead.
 *
 * Define POST_DMA_FLUSH if the driver should discard DMA RX buffers after a
 * transfer completes. Needed if the CPU can speculatively fetch cache lines
 * in any way, eg. due to speculative execution / prefetching.
 */
#if CONFIG_CPU == X1000
# define USB_DW_PHYSADDR(x)     PHYSADDR(x)
# define USB_DW_UNCACHEDADDR(x) ((typeof(x))UNCACHEDADDR(x))
# define POST_DMA_FLUSH
#elif CONFIG_CPU == AS3525v2
# define USB_DW_PHYSADDR(x)     AS3525_PHYSICAL_ADDR(x)
# define USB_DW_UNCACHEDADDR(x) AS3525_UNCACHED_ADDR(x)
#elif CONFIG_CPU == S5L8701
# define USB_DW_PHYSADDR(x)     x
# define NO_UNCACHED_ADDR       /* Not known how to form uncached addresses */
#elif CONFIG_CPU == S5L8702 || CONFIG_CPU == S5L8720
# define USB_DW_PHYSADDR(x)     S5L8702_PHYSICAL_ADDR(x)
# define USB_DW_UNCACHEDADDR(x) S5L8702_UNCACHED_ADDR(x)
#elif CONFIG_CPU == STM32H743
# define USB_DW_PHYSADDR(x)     x
# define NO_UNCACHED_ADDR       /* TODO: maybe implement this */
# define POST_DMA_FLUSH
#elif !defined(USB_DW_ARCH_SLAVE)
# error "Must define USB_DW_PHYSADDR / USB_DW_UNCACHEDADDR!"
#endif

#ifndef USB_DW_TOUTCAL
#define USB_DW_TOUTCAL 0
#endif

#ifdef USB_DW_FORCE_DEVICE_MODE
#define USB_DW_FORCED_MODE FDMOD
#else
#define USB_DW_FORCED_MODE 0
#endif

#ifndef USB_DW_DCFG_SPEED
#define USB_DW_DCFG_SPEED 0
#endif

#define GET_DTXFNUM(ep) ((DWC_DIEPCTL(ep)>>22) & 0xf)

#define USB_DW_NUM_DIRS 2
#define USB_DW_DIR_OFF(dir) (((dir) == USB_DW_EPDIR_IN) ? 0 : 16)

enum usb_dw_epdir
{
    USB_DW_EPDIR_IN = 0,
    USB_DW_EPDIR_OUT = 1,
};

/* Internal EP state/info */
struct usb_dw_ep
{
    struct semaphore complete;
    void* req_addr;
    uint32_t req_size;
    uint32_t* addr;
    uint32_t sizeleft;
    uint32_t size;
    int8_t status;
    uint8_t active;
    uint8_t busy;
};

static const char* const dw_dir_str[USB_DW_NUM_DIRS] =
{
    [USB_DW_EPDIR_IN]  = "IN",
    [USB_DW_EPDIR_OUT] = "OUT",
};

static struct usb_dw_ep usb_dw_ep_list[USB_NUM_ENDPOINTS][USB_DW_NUM_DIRS];

static uint8_t _ep0_buffer[64] USB_DEVBSS_ATTR __attribute__((aligned(32)));
static uint8_t* ep0_buffer; /* Uncached, unless NO_UNCACHED_ADDR is defined */

static uint32_t usb_endpoints;  /* available EPs mask */

/* For SHARED_FIFO mode this is the number of periodic Tx FIFOs
   (usually 1), otherwise it is the number of dedicated Tx FIFOs
   (not counting NPTX FIFO that is always dedicated for IN0). */
static int n_ptxfifos;

static uint32_t hw_maxbytes;
static uint32_t hw_maxpackets;
#ifdef USB_DW_SHARED_FIFO
static uint8_t hw_nptxqdepth;
static uint32_t epmis_msk;
static uint32_t ep_periodic_msk;
#endif

static struct usb_dw_ep *usb_dw_get_ep(int epnum, enum usb_dw_epdir epdir)
{
    return &usb_dw_ep_list[epnum][epdir];
}

static uint32_t usb_dw_maxpktsize(int epnum, enum usb_dw_epdir epdir)
{
    return epnum ? DWC_EPCTL(epnum, epdir) & 0x7ff : 64;
}

static uint32_t usb_dw_maxxfersize(int epnum, enum usb_dw_epdir epdir)
{
    /* EP0 can only transfer one packet at a time. */
    if(epnum == 0)
        return 64;

    uint32_t maxpktsize = usb_dw_maxpktsize(epnum, epdir);
    return CACHEALIGN_DOWN(MIN(hw_maxbytes, hw_maxpackets * maxpktsize));
}

/* Calculate number of packets (if size == 0 an empty packet will be sent) */
static uint32_t usb_dw_calc_packets(uint32_t size, uint32_t maxpktsize)
{
    return MAX(1, (size + maxpktsize - 1) / maxpktsize);
}

static int usb_dw_get_stall(int epnum, enum usb_dw_epdir epdir)
{
    return !!(DWC_EPCTL(epnum, epdir) & STALL);
}

static void usb_dw_set_stall(int epnum, enum usb_dw_epdir epdir, int stall)
{
    if (stall)
    {
        DWC_EPCTL(epnum, epdir) |= STALL;
    }
    else
    {
        DWC_EPCTL(epnum, epdir) &= ~STALL;
        DWC_EPCTL(epnum, epdir) |= SETD0PIDEF;
    }
}

static void usb_dw_set_address(uint8_t address)
{
    DWC_DCFG = (DWC_DCFG & ~(0x7f0)) | DAD(address);
}

static void usb_dw_wait_for_ahb_idle(void)
{
    while (!(DWC_GRSTCTL & AHBIDL));
}

#ifdef USB_DW_SHARED_FIFO
static unsigned usb_dw_bytes_in_txfifo(int epnum, uint32_t *sentbytes)
{
    uint32_t size = usb_dw_get_ep(epnum, USB_DW_EPDIR_IN)->size;
    if (sentbytes) *sentbytes = size;
    uint32_t dieptsiz = DWC_DIEPTSIZ(epnum);
    uint32_t packetsleft = (dieptsiz >> 19) & 0x3ff;
    if (!packetsleft) return 0;
    uint32_t maxpktsize = usb_dw_maxpktsize(epnum, USB_DW_EPDIR_IN);
    uint32_t packets = usb_dw_calc_packets(size, maxpktsize);
    uint32_t bytesleft = dieptsiz & 0x7ffff;
    uint32_t bytespushed = size - bytesleft;
    uint32_t bytespulled = (packets - packetsleft) * maxpktsize;

    if (sentbytes) *sentbytes = bytespulled;
    return bytespushed - bytespulled;
}
#endif

#ifdef USB_DW_ARCH_SLAVE
/* Read one packet/token from Rx FIFO */
static void usb_dw_handle_rxfifo(void)
{
    uint32_t rxsts = DWC_GRXSTSP;
    uint32_t pktsts = (rxsts >> 17) & 0xf;

    switch (pktsts)
    {
        case PKTSTS_OUTRX:
        case PKTSTS_SETUPRX:
        {
            int ep = rxsts & 0xf;
            uint32_t words = (((rxsts >> 4) & 0x7ff) + 3) >> 2;

            /* Annoyingly, we need to special-case EP0. */
            if(ep == 0)
            {
                uint32_t* addr = (uint32_t*)ep0_buffer;
                while (words--)
                    *addr++ = DWC_DFIFO(0);
            }
            else
            {
                struct usb_dw_ep* dw_ep = usb_dw_get_ep(ep, USB_DW_EPDIR_OUT);
                if (dw_ep->busy)
                {
                    while (words--)
                        *dw_ep->addr++ = DWC_DFIFO(0);
                }
                else
                {
                    /* Discard data */
                    while (words--)
                        (void) DWC_DFIFO(0);
                }
            }

            break;
        }
        case PKTSTS_OUTDONE:
        case PKTSTS_SETUPDONE:
        case PKTSTS_GLOBALOUTNAK:
        default:
            break;
    }
}

#ifdef USB_DW_SHARED_FIFO
static void usb_dw_try_push(int epnum)
{
    struct usb_dw_ep* dw_ep = usb_dw_get_ep(epnum, USB_DW_EPDIR_IN);

    if (!dw_ep->busy)
        return;

    if (epmis_msk & (1 << epnum))
        return;

    uint32_t wordsleft = ((DWC_DIEPTSIZ(epnum) & 0x7ffff) + 3) >> 2;
    if (!wordsleft) return;

    /* Get fifo space for NPTXFIFO or PTXFIFO */
    uint32_t fifospace;
    int dtxfnum = GET_DTXFNUM(epnum);
    if (dtxfnum)
    {
        uint32_t fifosize = DWC_DIEPTXF(dtxfnum - 1) >> 16;
        fifospace = fifosize - ((usb_dw_bytes_in_txfifo(epnum, NULL) + 3) >> 2);
    }
    else
    {
        uint32_t gnptxsts = DWC_GNPTXSTS;
        fifospace = ((gnptxsts >> 16) & 0xff) ? (gnptxsts & 0xffff) : 0;
    }

    uint32_t maxpktsize = usb_dw_maxpktsize(epnum, USB_DW_EPDIR_IN);
    uint32_t words = MIN((maxpktsize + 3) >> 2, wordsleft);

    if (fifospace >= words)
    {
        wordsleft -= words;
        while (words--)
            DWC_DFIFO(epnum) = *dw_ep->addr++;
    }

    if (wordsleft)
        DWC_GINTMSK |= (dtxfnum ? PTXFE : NPTXFE);
}

#else /* !USB_DW_SHARED_FIFO */
static void usb_dw_handle_dtxfifo(int epnum)
{
    struct usb_dw_ep* dw_ep = usb_dw_get_ep(epnum, USB_DW_EPDIR_IN);

    if (!dw_ep->busy)
        return;

    uint32_t wordsleft = ((DWC_DIEPTSIZ(epnum) & 0x7ffff) + 3) >> 2;

    while (wordsleft)
    {
        uint32_t words = wordsleft;
        uint32_t fifospace = DWC_DTXFSTS(epnum) & 0xffff;

        if (fifospace < words)
        {
            /* We push whole packets to read consistent info on DIEPTSIZ
               (i.e. when FIFO size is not maxpktsize multiplo). */
            uint32_t maxpktwords = usb_dw_maxpktsize(epnum, USB_DW_EPDIR_IN) >> 2;
            words = (fifospace / maxpktwords) * maxpktwords;
        }

        if (!words)
            break;

        wordsleft -= words;
        while (words--)
            DWC_DFIFO(epnum) = *dw_ep->addr++;
    }

    if (!wordsleft)
        DWC_DIEPEMPMSK &= ~(1 << GET_DTXFNUM(epnum));
}
#endif /* !USB_DW_SHARED_FIFO */
#endif /* USB_DW_ARCH_SLAVE */

static void usb_dw_flush_fifo(uint32_t fflsh, int fnum)
{
#ifdef USB_DW_ARCH_SLAVE
    /* Rx queue must be emptied before flushing Rx FIFO */
    if (fflsh & RXFFLSH)
        while (DWC_GINTSTS & RXFLVL)
            usb_dw_handle_rxfifo();
#else
    /* Wait for any DMA activity to stop */
    usb_dw_wait_for_ahb_idle();
#endif
    DWC_GRSTCTL = TXFNUM(fnum) | fflsh;
    while (DWC_GRSTCTL & fflsh);
    udelay(1);  /* Wait 3 PHY cycles */
}

/* These are the conditions that must be met so that the application can
 * disable an endpoint avoiding race conditions:
 *
 * 1) The endpoint must be enabled when EPDIS is written, otherwise the
 *    core will never raise EPDISD interrupt (thus EPDIS remains enabled).
 *
 * 2) - Periodic (SHARED_FIFO) or dedicated (!SHARED_FIFO) IN endpoints:
 *      IN NAK must be effective, to ensure that the core is not going
 *      to disable the EP just before EPDIS is written.
 *    - Non-periodic (SHARED_FIFO) IN endpoints: use usb_dw_nptx_unqueue().
 *    - OUT endpoints: GONAK must be effective, this also ensures that the
 *      core is not going to disable the EP.
 */
static void usb_dw_disable_ep(int epnum, enum usb_dw_epdir epdir)
{
    if (!epnum && (epdir == USB_DW_EPDIR_OUT))
        return;  /* The application cannot disable OUT0 */

    if (DWC_EPCTL(epnum, epdir) & EPENA)
    {
        int tmo = 50;
        DWC_EPCTL(epnum, epdir) |= EPDIS;
        while (DWC_EPCTL(epnum, epdir) & EPDIS)
        {
            if (!tmo--)
                panicf("%s: %s%d failed!", __func__, dw_dir_str[epdir], epnum);
            udelay(1);
        }
    }
}

static void usb_dw_gonak_effective(bool enable)
{
    if (enable)
    {
        if (!(DWC_DCTL & GONSTS))
            DWC_DCTL |= SGONAK;

        /* Wait for global IN NAK effective */
        int tmo = 50;
        while (~DWC_GINTSTS & GOUTNAKEFF)
        {
            if (!tmo--) panicf("%s: failed!", __func__);
#ifdef USB_DW_ARCH_SLAVE
            /* Pull Rx queue until GLOBALOUTNAK token is received. */
            if (DWC_GINTSTS & RXFLVL)
                usb_dw_handle_rxfifo();
            else
#endif
            udelay(1);
        }
    }
    else
    {
        if (DWC_DCTL & GONSTS)
            DWC_DCTL |= CGONAK;
    }
}

static void usb_dw_set_innak_effective(int epnum)
{
    if (~DWC_DIEPCTL(epnum) & NAKSTS)
    {
        /* Wait for IN NAK effective avoiding race conditions, if the
         * endpoint is disabled by the core (or it was already disabled)
         * then INEPNE is never raised.
         */
        int tmo = 50;
        DWC_DIEPCTL(epnum) |= SNAK;
        while ((DWC_DIEPCTL(epnum) & EPENA) && !(DWC_DIEPINT(epnum) & INEPNE))
        {
            if (!tmo--) panicf("%s: IN%d failed!", __func__, epnum);
            udelay(1);
        }
    }
}

#ifdef USB_DW_SHARED_FIFO
static void usb_dw_ginak_effective(bool enable)
{
    if (enable)
    {
        if (!(DWC_DCTL & GINSTS))
            DWC_DCTL |= SGINAK;

        /* Wait for global IN NAK effective */
        int tmo = 50;
        while (~DWC_GINTSTS & GINAKEFF)
        {
            if (!tmo--) panicf("%s: failed!", __func__);
            udelay(1);
        }
#ifndef USB_DW_ARCH_SLAVE
        /* Wait for any DMA activity to stop. */
        usb_dw_wait_for_ahb_idle();
#endif
    }
    else
    {
        if (DWC_DCTL & GINSTS)
            DWC_DCTL |= CGINAK;
    }
}

static void usb_dw_nptx_unqueue(int epnum)
{
    uint32_t reenable_msk = 0;

    usb_dw_ginak_effective(true);

    /* Disable EPs */
    for (int ep = 0; ep < USB_NUM_ENDPOINTS; ep++)
    {
        if (usb_endpoints & ~ep_periodic_msk & (1 << ep))
        {
            /* Disable */
            if (~DWC_DIEPCTL(ep) & EPENA)
                continue;
            DWC_DIEPCTL(ep) |= EPDIS|SNAK;

            /* Adjust */
            uint32_t packetsleft = (DWC_DIEPTSIZ(ep) >> 19) & 0x3ff;
            if (!packetsleft) continue;

            struct usb_dw_ep* dw_ep = usb_dw_get_ep(ep, USB_DW_EPDIR_IN);
            uint32_t sentbytes;
            uint32_t bytesinfifo = usb_dw_bytes_in_txfifo(ep, &sentbytes);

#ifdef USB_DW_ARCH_SLAVE
            dw_ep->addr -= (bytesinfifo + 3) >> 2;
#else
            (void) bytesinfifo;
            DWC_DIEPDMA(ep) = USB_DW_PHYSADDR((uint32_t)(dw_ep->addr) + sentbytes);
#endif
            DWC_DIEPTSIZ(ep) = PKTCNT(packetsleft) | (dw_ep->size - sentbytes);

            /* Do not re-enable the EP we are going to unqueue */
            if (ep == epnum)
                continue;

            /* Mark EP to be re-enabled later */
            reenable_msk |= (1 << ep);
        }
    }

    /* Flush NPTXFIFO */
    usb_dw_flush_fifo(TXFFLSH, 0);

    /* Re-enable EPs */
    for (int ep = 0; ep < USB_NUM_ENDPOINTS; ep++)
        if (reenable_msk & (1 << ep))
            DWC_DIEPCTL(ep) |= EPENA|CNAK;

#ifdef USB_DW_ARCH_SLAVE
    if (reenable_msk)
        DWC_GINTMSK |= NPTXFE;
#endif

    usb_dw_ginak_effective(false);
}
#endif /* USB_DW_SHARED_FIFO */

static void usb_dw_flush_endpoint(int epnum, enum usb_dw_epdir epdir)
{
    struct usb_dw_ep* dw_ep = usb_dw_get_ep(epnum, epdir);
    dw_ep->busy = false;
    dw_ep->status = -1;
    semaphore_release(&dw_ep->complete);

    if (DWC_EPCTL(epnum, epdir) & EPENA)
    {
        if (epdir == USB_DW_EPDIR_IN)
        {
            /* We are shutting down an endpoint that might still have IN
             * packets in the FIFO. Disable the endpoint, wait for things
             * to settle, and flush the relevant FIFO.
             */
            int dtxfnum = GET_DTXFNUM(epnum);

#ifdef USB_DW_SHARED_FIFO
            if (!dtxfnum)
            {
                usb_dw_nptx_unqueue(epnum);
            }
            else
#endif
            {
                /* Wait for IN NAK effective to avoid race conditions
                   while shutting down the endpoint. */
                usb_dw_set_innak_effective(epnum);

                /* Disable the EP we are going to flush */
                usb_dw_disable_ep(epnum, epdir);

                /* Flush it all the way down! */
                usb_dw_flush_fifo(TXFFLSH, dtxfnum);

#if !defined(USB_DW_SHARED_FIFO) && defined(USB_DW_ARCH_SLAVE)
                DWC_DIEPEMPMSK &= ~(1 << dtxfnum);
#endif
            }
        }
        else
        {
            /* We are waiting for an OUT packet on this endpoint, which
             * might arrive any moment. Assert a global output NAK to
             * avoid race conditions while shutting down the endpoint.
             * Global output NAK also flushes the Rx FIFO.
             */
            usb_dw_gonak_effective(true);
            usb_dw_disable_ep(epnum, epdir);
            usb_dw_gonak_effective(false);
        }
    }

    /* At this point the endpoint is disabled, SNAK it (in case it is not
     * already done), it is needed for Tx shared FIFOs (to not to raise
     * unwanted EPMIS interrupts) and recomended for dedicated FIFOs.
     */
    DWC_EPCTL(epnum, epdir) |= SNAK;

#ifdef USB_DW_SHARED_FIFO
    if (epdir == USB_DW_EPDIR_IN)
    {
        epmis_msk &= ~(1 << epnum);
        if (!epmis_msk)
            DWC_DIEPMSK &= ~ITTXFE;
    }
#endif

    /* Clear all channel interrupts to avoid to process
       pending tokens for the flushed EP. */
    DWC_EPINT(epnum, epdir) = DWC_EPINT(epnum, epdir);
}

static void usb_dw_unconfigure_ep(int epnum, enum usb_dw_epdir epdir)
{
    uint32_t epctl = 0;

    if (epdir == USB_DW_EPDIR_IN)
    {
#ifdef USB_DW_SHARED_FIFO
#ifndef USB_DW_ARCH_SLAVE
        int next;
        for (next = epnum + 1; next < USB_NUM_ENDPOINTS; next++)
            if (usb_endpoints & (1 << next))
                break;
        epctl = NEXTEP(next % USB_NUM_ENDPOINTS);
#endif
        ep_periodic_msk &= ~(1 << epnum);
#endif
    }

    usb_dw_flush_endpoint(epnum, epdir);
    DWC_EPCTL(epnum, epdir) = epctl;
}

static void usb_dw_configure_ep(const struct usb_drv_ep_alloc_ctx* ctx, int epnum, enum usb_dw_epdir epdir, int type, int maxpktsize)
{
    uint32_t epctl = SETD0PIDEF|EPTYP(type)|USBAEP|maxpktsize;

    if (epdir == USB_DW_EPDIR_IN && ctx->assigned_txfifos[epnum] > 0)
    {
#ifdef USB_DW_SHARED_FIFO
        ep_periodic_msk |= (1 << epnum);
#ifndef USB_DW_ARCH_SLAVE
        epctl |= DWC_DIEPCTL(epnum) & NEXTEP(0xf);
#endif
#endif
        epctl |= DTXFNUM(ctx->assigned_txfifos[epnum]);
    }

    DWC_EPCTL(epnum, epdir) = epctl;
}

static void usb_dw_reset_endpoints(void)
{
    /* Initial state for all endpoints, setting OUT EPs as not busy
     * will discard all pending data (if any) on the flush stage.
     */
    for (int ep = 0; ep < USB_NUM_ENDPOINTS; ep++)
    {
        for (int dir = 0; dir < USB_DW_NUM_DIRS; dir++)
        {
            struct usb_dw_ep* dw_ep = usb_dw_get_ep(ep, dir);
            dw_ep->active = !ep;
            dw_ep->busy = false;
            dw_ep->status = -1;
            semaphore_release(&dw_ep->complete);
        }
    }

#if CONFIG_CPU == S5L8701
    /*
     * Workaround for spurious -EPROTO when receiving bulk data on Nano2G.
     *
     * The Rx FIFO and Rx queue are currupted by the received (corrupted)
     * data, must be flushed, otherwise the core can not set GONAK effective.
     */
    usb_dw_flush_fifo(RXFFLSH, 0);
#endif

    /* Flush and initialize EPs, includes disabling USBAEP on all EPs
     * except EP0 (USB HW core keeps EP0 active on all configurations).
     */
    for (int ep = 0; ep < USB_NUM_ENDPOINTS; ep++)
    {
        if (usb_endpoints & (1 << (ep + 16)))
            usb_dw_unconfigure_ep(ep, USB_DW_EPDIR_OUT);
        if (usb_endpoints & (1 << ep))
            usb_dw_unconfigure_ep(ep, USB_DW_EPDIR_IN);
    }

#ifdef USB_DW_SHARED_FIFO
    ep_periodic_msk = 0;
#endif
}

static void usb_dw_epstart(int epnum, enum usb_dw_epdir epdir,
                           void* buf, uint32_t size)
{
    if ((uint32_t)buf & ((epdir == USB_DW_EPDIR_IN) ? 3 : CACHEALIGN_SIZE-1))
        logf("%s: %s%d %p unaligned", __func__, dw_dir_str[epdir], epnum, buf);

    struct usb_dw_ep* dw_ep = usb_dw_get_ep(epnum, epdir);
    uint32_t xfersize = MIN(size, usb_dw_maxxfersize(epnum, epdir));

    dw_ep->addr = (uint32_t*)buf;
    dw_ep->size = xfersize;
    dw_ep->sizeleft = size;
    dw_ep->status = -1;
    dw_ep->busy = true;

    uint32_t maxpktsize = usb_dw_maxpktsize(epnum, epdir);
    uint32_t packets = usb_dw_calc_packets(xfersize, maxpktsize);
    uint32_t eptsiz = PKTCNT(packets) | xfersize;
    uint32_t nak = CNAK;

    if (epdir == USB_DW_EPDIR_IN)
    {
#ifndef USB_DW_ARCH_SLAVE
        COMMIT_DCACHE_RANGE(buf, xfersize);
#endif
#ifdef USB_DW_SHARED_FIFO
        eptsiz |= MCCNT((ep_periodic_msk >> epnum) & 1);
#else
        /* Dedicated FIFO mode still requires a non-zero multi-count for
         * periodic IN endpoints. MCCNT(0) transmits no packets. */
        uint32_t eptype = (DWC_DIEPCTL(epnum) >> 18) & 0x3;
        if (eptype == EPTYP_ISOCHRONOUS || eptype == EPTYP_INTERRUPT)
            eptsiz |= MCCNT(packets);  /* rarely, if ever, not 1 */
#endif

    }
    else
    {
#ifndef USB_DW_ARCH_SLAVE
        DISCARD_DCACHE_RANGE(buf, xfersize);
#endif
    }

#ifndef USB_DW_ARCH_SLAVE
    DWC_EPDMA(epnum, epdir) = USB_DW_PHYSADDR((uint32_t)buf);
#endif
    DWC_EPTSIZ(epnum, epdir) = eptsiz;
    if (((DWC_EPCTL(epnum, epdir) >> 18) & 0x3) == EPTYP_ISOCHRONOUS)
    {
        /*
         * Handle frame scheduling for isochronous endpoints.
         *
         * TODO: this needs to take into account the endpoint's bInterval,
         * and currently will not work for USB 2.0 endpoints that require
         * one packet per microframe. For slower USB 2.0 endpoints data is
         * only transferred on *even* frames; SETD1PIDOF is not used.
         * This also breaks for USB 1.0 endpoints with bInterval > 1 for
         * the same reason.
         */
        if (usb_drv_port_speed() || (usb_drv_get_frame_number() & 1))
            DWC_EPCTL(epnum, epdir) |= EPENA | nak | SETD0PIDEF;
        else
            DWC_EPCTL(epnum, epdir) |= EPENA | nak | SETD1PIDOF;
    }
    else
    {
        DWC_EPCTL(epnum, epdir) |= EPENA | nak;
    }

#ifdef USB_DW_ARCH_SLAVE
    /* Enable interrupts to start pushing data into the FIFO */
    if ((epdir == USB_DW_EPDIR_IN) && dw_ep->size > 0)
#ifdef USB_DW_SHARED_FIFO
        DWC_GINTMSK |= ((ep_periodic_msk & (1 << epnum)) ? PTXFE : NPTXFE);
#else
        DWC_DIEPEMPMSK |= (1 << GET_DTXFNUM(epnum));
#endif
#endif
}

static void usb_dw_transfer(int epnum, enum usb_dw_epdir epdir,
                            void* buf, uint32_t size)
{
    struct usb_dw_ep* dw_ep = usb_dw_get_ep(epnum, epdir);

    if (!dw_ep->active)
        logf("%s: %s%d inactive", __func__, dw_dir_str[epdir], epnum);
    if (dw_ep->busy)
        logf("%s: %s%d busy", __func__, dw_dir_str[epdir], epnum);

    dw_ep->req_addr = buf;
    dw_ep->req_size = size;
    usb_dw_epstart(epnum, epdir, buf, size);
}

static void usb_dw_ep0_recv(void)
{
#ifndef USB_DW_ARCH_SLAVE
#ifdef NO_UNCACHED_ADDR
    DISCARD_DCACHE_RANGE(&_ep0_buffer[0], 64);
#endif
    DWC_DOEPDMA(0) = USB_DW_PHYSADDR((uint32_t)&_ep0_buffer[0]);
#endif
    DWC_DOEPTSIZ(0) = STUPCNT(1) | PKTCNT(1) | 64;
    DWC_DOEPCTL(0) |= EPENA | SNAK;
}

static void usb_dw_abort_endpoint(int epnum, enum usb_dw_epdir epdir)
{
    struct usb_dw_ep* dw_ep = usb_dw_get_ep(epnum, epdir);
    if (dw_ep->busy)
    {
        usb_dw_flush_endpoint(epnum, epdir);
        usb_core_transfer_complete(epnum, (epdir == USB_DW_EPDIR_OUT) ?
                                        USB_DIR_OUT : USB_DIR_IN, -1, 0);
    }
}

static void usb_dw_handle_xfer_complete(int epnum, enum usb_dw_epdir epdir)
{
    struct usb_dw_ep* dw_ep = usb_dw_get_ep(epnum, epdir);
    bool is_ep0out = (epnum == 0 && epdir == USB_DW_EPDIR_OUT);

    if (!dw_ep->busy)
    {
        if(is_ep0out)
            usb_dw_ep0_recv();
        return;
    }

    uint32_t bytes_left = DWC_EPTSIZ(epnum, epdir) & 0x7ffff;
    uint32_t transferred = dw_ep->size - bytes_left;

    if(transferred > dw_ep->sizeleft)
    {
        /* Host sent more data than expected.
         * Shouldn't happen for IN endpoints. */
        dw_ep->status = -2;
        goto complete;
    }

    if(is_ep0out)
    {
#if !defined(USB_DW_ARCH_SLAVE) && defined(NO_UNCACHED_ADDR) && defined(POST_DMA_FLUSH)
        DISCARD_DCACHE_RANGE(ep0_buffer, 64);
#endif
#ifdef USB_DW_ARCH_SLAVE
        memcpy(dw_ep->addr, ep0_buffer, transferred);
#endif
        usb_dw_ep0_recv();
    }

    dw_ep->sizeleft -= transferred;

    /* Start a new transfer if there is still more to go */
    if(bytes_left == 0 && dw_ep->sizeleft > 0)
    {
#ifndef USB_DW_ARCH_SLAVE
        dw_ep->addr += (dw_ep->size >> 2); /* offset in words */
#endif
        usb_dw_epstart(epnum, epdir, dw_ep->addr, dw_ep->sizeleft);
        return;
    }

    if(epdir == USB_DW_EPDIR_IN)
    {
        /* SNAK the disabled EP, otherwise IN tokens for this
           EP could raise unwanted EPMIS interrupts. Useful for
           usbserial when there is no data to send. */
        DWC_DIEPCTL(epnum) |= SNAK;

#ifdef USB_DW_SHARED_FIFO
        /* See usb-s5l8701.c */
        if (usb_dw_config.use_ptxfifo_as_plain_buffer)
        {
            int dtxfnum = GET_DTXFNUM(epnum);
            if (dtxfnum)
                usb_dw_flush_fifo(TXFFLSH, dtxfnum);
        }
#endif
    }
    else
    {
#if !defined(USB_DW_ARCH_SLAVE) && defined(POST_DMA_FLUSH)
        /* On EP0 OUT we do not DMA into the request buffer,
         * so do not discard the cache in this case. */
        if(!is_ep0out)
            DISCARD_DCACHE_RANGE(dw_ep->req_addr, dw_ep->req_size);
#endif
    }

    dw_ep->status = 0;

  complete:
    dw_ep->busy = false;
    semaphore_release(&dw_ep->complete);

    int total_bytes = dw_ep->req_size - dw_ep->sizeleft;
    usb_core_transfer_complete(epnum, (epdir == USB_DW_EPDIR_OUT) ?
                USB_DIR_OUT : USB_DIR_IN, dw_ep->status, total_bytes);
}

static void usb_dw_handle_setup_received(void)
{
#if !defined(USB_DW_ARCH_SLAVE) && defined(NO_UNCACHED_ADDR) && defined(POST_DMA_FLUSH)
    DISCARD_DCACHE_RANGE(ep0_buffer, 64);
#endif
    struct usb_ctrlrequest req;
    memcpy(&req, ep0_buffer, sizeof(struct usb_ctrlrequest));
    usb_dw_flush_endpoint(0, USB_DW_EPDIR_IN);
    usb_dw_ep0_recv();

    if ((req.bRequestType & USB_RECIP_MASK) == USB_RECIP_DEVICE &&
        (req.bRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD &&
        (req.bRequest == USB_REQ_SET_ADDRESS))
        usb_dw_set_address(req.wValue);

    usb_core_setup_received(&req);
}

#ifdef USB_DW_SHARED_FIFO
static int usb_dw_get_epmis(void)
{
    unsigned epmis;
    uint32_t gnptxsts = DWC_GNPTXSTS;

    if (((gnptxsts >> 16) & 0xff) >= hw_nptxqdepth)
        return -1;  /* empty queue */

    /* Get the EP on the top of the queue, 0 < idx < number of available
       IN endpoints */
    uint32_t idx = (gnptxsts >> 27) & 0xf;
    for (epmis = 0; epmis < USB_NUM_ENDPOINTS; epmis++)
        if ((usb_endpoints & (1 << epmis)) && !idx--)
            break;

    /* The maximum EP mismatch counter is configured, so we verify all NPTX
       queue entries, 4 bits per entry, first entry at DTKQNR1[11:8] */
    uint32_t volatile *dtknqr = &DWC_DTKNQR1;
    for (int i = 2; i < hw_nptxqdepth + 2; i++)
        if (((*(dtknqr+(i>>3)) >> ((i & 0x7)*4)) & 0xf) == epmis)
            return -1;

    return epmis;
}

static void usb_dw_handle_token_mismatch(void)
{
    usb_dw_ginak_effective(true);
    int epmis = usb_dw_get_epmis();
    if (epmis >= 0)
    {
        /* The EP is disabled, unqueued, and reconfigured to re-reenable it
           later when a token is received, (or it will be cancelled by
           timeout if it was a blocking request). */
        usb_dw_nptx_unqueue(epmis);

        epmis_msk |= (1 << epmis);
        if (epmis_msk)
            DWC_DIEPMSK |= ITTXFE;

        /* Be sure the status is clear */
        DWC_DIEPINT(epmis) = ITTXFE;

        /* Must disable NAK to allow to get ITTXFE interrupts for this EP */
        DWC_DIEPCTL(epmis) |= CNAK;
    }
    usb_dw_ginak_effective(false);
}
#endif /* USB_DW_SHARED_FIFO */

static void usb_dw_iepint(int ep)
{
    uint32_t epints = DWC_DIEPINT(ep);
    DWC_DIEPINT(ep) = epints;

    if (epints & TOC)
    {
        usb_dw_abort_endpoint(ep, USB_DW_EPDIR_IN);
    }

#ifdef USB_DW_SHARED_FIFO
    if (epints & ITTXFE)
    {
        if (epmis_msk & (1 << ep))
        {
            DWC_DIEPCTL(ep) |= EPENA;
            epmis_msk &= ~(1 << ep);
            if (!epmis_msk)
                DWC_DIEPMSK &= ~ITTXFE;
        }
    }

#elif defined(USB_DW_ARCH_SLAVE)
    if (epints & TXFE)
    {
        usb_dw_handle_dtxfifo(ep);
    }
#endif

    if (epints & XFRC)
    {
        usb_dw_handle_xfer_complete(ep, USB_DW_EPDIR_IN);
    }
}

static void usb_dw_oepint(int ep)
{
    uint32_t epints = DWC_DOEPINT(ep);
    DWC_DOEPINT(ep) = epints;

    if (!ep)
    {
        if (epints & STUP)
        {
            usb_dw_handle_setup_received();
        }

        if (epints & XFRC)
        {
            if(epints & STATUSRECVD)
            {
                /* At the end of a control write's data phase, the
                 * controller writes a spurious OUTDONE token to the
                 * FIFO and raises StatusRecvd | XferCompl.
                 *
                 * We do not need or want this -- we've already handled
                 * the data phase by this point -- but EP0 is stopped
                 * as a side effect of XferCompl, so we need to restart
                 * it to keep receiving packets. */
                usb_dw_ep0_recv();
            }
            else if(!(epints & SETUPRECVD))
            {
                /* Only call this for normal data packets. Setup
                 * packets use the STUP interrupt handler instead. */
                usb_dw_handle_xfer_complete(0, USB_DW_EPDIR_OUT);
            }
        }
    }
    else
    {
        if (epints & XFRC)
        {
            usb_dw_handle_xfer_complete(ep, USB_DW_EPDIR_OUT);
        }
    }
}

static void usb_dw_irq(void)
{
    int ep;
    uint32_t gintsts = DWC_GINTSTS & DWC_GINTMSK;

#ifdef USB_DW_ARCH_SLAVE
    /* Handle one packet at a time, the IRQ will re-trigger if there's
       something left. */
    if (gintsts & RXFLVL)
    {
        usb_dw_handle_rxfifo();
    }
#endif

#ifdef USB_DW_SHARED_FIFO
    if (gintsts & EPMIS)
    {
        usb_dw_handle_token_mismatch();
        DWC_GINTSTS = EPMIS;
    }

#ifdef USB_DW_ARCH_SLAVE
    if (gintsts & PTXFE)
    {
        /* First disable the IRQ, it will be re-enabled later if there
           is anything left to be done. */
        DWC_GINTMSK &= ~PTXFE;
        /* Check all periodic endpoints for anything to be transmitted */
        for (ep = 1; ep < USB_NUM_ENDPOINTS; ep++)
            if (usb_endpoints & ep_periodic_msk & (1 << ep))
                usb_dw_try_push(ep);
    }

    if (gintsts & NPTXFE)
    {
        /* First disable the IRQ, it will be re-enabled later if there
           is anything left to be done. */
        DWC_GINTMSK &= ~NPTXFE;
        /* Check all non-periodic endpoints for anything to be transmitted */
        for (ep = 0; ep < USB_NUM_ENDPOINTS; ep++)
            if (usb_endpoints & ~ep_periodic_msk & (1 << ep))
                usb_dw_try_push(ep);
    }
#endif /* USB_DW_ARCH_SLAVE */
#endif /* USB_DW_SHARED_FIFO */

    if (gintsts & (OEPINT | IEPINT))
    {
        uint32_t daint = DWC_DAINT & DWC_DAINTMSK;
        uint32_t daint_out = daint >> 16;
        uint32_t daint_in = daint & 0xffff;
        for (ep = 0; ep < USB_NUM_ENDPOINTS && daint_in; ep++, daint_in >>= 1)
        {
            if (daint_in & 1)
            {
                usb_dw_iepint(ep);
            }
        }

        for (ep = 0; ep < USB_NUM_ENDPOINTS && daint_out; ep++, daint_out >>= 1)
        {
            if (daint_out & 1)
            {
                usb_dw_oepint(ep);
            }
        }
    }

    if (gintsts & USBRST)
    {
        DWC_GINTSTS = USBRST;
        usb_dw_set_address(0);
        usb_dw_reset_endpoints();
        usb_core_bus_reset();
    }

    if (gintsts & ENUMDNE)
    {
        DWC_GINTSTS = ENUMDNE;
        usb_dw_ep0_recv();
    }
}

static void usb_dw_check_hw(void)
{
    uint32_t ghwcfg2 = DWC_GHWCFG2;
    uint32_t ghwcfg3 = DWC_GHWCFG3;
    uint32_t ghwcfg4 = DWC_GHWCFG4;
    const struct usb_dw_config *c = &usb_dw_config;
    int hw_numeps;
    int hw_maxtxfifos;  /* periodic or dedicated */
    char *err;

    hw_numeps = ((ghwcfg2 >> 10) & 0xf) + 1;

    if (hw_numeps < USB_NUM_ENDPOINTS)
    {
        err = "USB_NUM_ENDPOINTS too big";
        goto panic;
    }
    /* HWCFG registers are not checked to detect the PHY, if an option
       is not supported then the related bits should be Read-Only. */
    DWC_GUSBCFG = c->phytype;
    if (DWC_GUSBCFG != c->phytype)
    {
        err = "PHY type not supported";
        goto panic;
    }
#ifndef USB_DW_ARCH_SLAVE
    if (((ghwcfg2 >> 3) & 3) != 2)
    {
        err = "internal DMA not supported";
        goto panic;
    }
#endif
#ifdef USB_DW_SHARED_FIFO
    if ((ghwcfg4 >> 25) & 1)
    {
        err = "shared TxFIFO not supported";
        goto panic;
    }
    hw_maxtxfifos = ghwcfg4 & 0xf;
    hw_nptxqdepth = (1 << (((ghwcfg2 >> 22) & 3) + 1));
#else
    if (!((ghwcfg4 >> 25) & 1))
    {
        err = "dedicated TxFIFO not supported";
        goto panic;
    }
    hw_maxtxfifos = (ghwcfg4 >> 26) & 0xf;
#endif
    hw_maxbytes = (1 << (((ghwcfg3 >> 0) & 0xf) + 11)) - 1;
    hw_maxpackets = (1 << (((ghwcfg3 >> 4) & 0x7) + 4)) - 1;
    uint16_t hw_fifomem = ghwcfg3 >> 16;

    /* Configure FIFOs, sizes are 32-bit words, we will need at least
       one periodic or dedicated Tx FIFO (really the periodic Tx FIFO
       is not needed if !USB_ENABLE_HID). */
    if (c->rx_fifosz + c->nptx_fifosz + c->ptx_fifosz > hw_fifomem)
    {
        err = "insufficient FIFO memory";
        goto panic;
    }
    n_ptxfifos = (hw_fifomem - c->rx_fifosz - c->nptx_fifosz) / c->ptx_fifosz;
    if (n_ptxfifos > hw_maxtxfifos) n_ptxfifos = hw_maxtxfifos;

    logf("%s():", __func__);
    logf(" HW version: %4lx, num EPs: %d", DWC_GSNPSID & 0xffff, hw_numeps);
    logf(" FIFO mem=%d rx=%d nptx=%d ptx=%dx%d", hw_fifomem,
                    c->rx_fifosz, c->nptx_fifosz, n_ptxfifos, c->ptx_fifosz);

    return;

panic:
    panicf("%s: %s", __func__, err);
}

static void usb_dw_init(void)
{
    static bool initialized = false;
    const struct usb_dw_config *c = &usb_dw_config;

    if (!initialized)
    {
#if !defined(USB_DW_ARCH_SLAVE) && !defined(NO_UNCACHED_ADDR)
        ep0_buffer = USB_DW_UNCACHEDADDR(&_ep0_buffer[0]);
#else
        /* DMA is not used so we can operate on cached addresses */
        ep0_buffer = &_ep0_buffer[0];
#endif

        for (int ep = 0; ep < USB_NUM_ENDPOINTS; ep++)
            for (int dir = 0; dir < USB_DW_NUM_DIRS; dir++)
                semaphore_init(&usb_dw_get_ep(ep, dir)->complete, 1, 0);

        initialized = true;
    }

    /* Disable IRQ during setup */
    usb_dw_target_disable_irq();

    /* Enable OTG clocks */
    usb_dw_target_enable_clocks();

    /* Enable PHY clocks */
    DWC_PCGCCTL = 0;

    usb_dw_check_hw();

    /* Configure PHY type (must be done before reset) */
#ifndef USB_DW_TURNAROUND
    /*
     * Turnaround time (in PHY clocks) = 4*AHB clocks + 1*PHY clock,
     * worst cases are:
     *  16-bit UTMI+: PHY=30MHz, AHB=30Mhz -> 5
     *  8-bit UTMI+:  PHY=60MHz, AHB=30MHz -> 9
     */
    int USB_DW_TURNAROUND = (c->phytype == DWC_PHYTYPE_UTMI_16) ? 5 : 9;
#endif
    uint32_t gusbcfg = c->phytype|TRDT(USB_DW_TURNAROUND)|USB_DW_TOUTCAL|USB_DW_FORCED_MODE;
    DWC_GUSBCFG = gusbcfg;

    /* Reset the whole USB core */
    udelay(100);
    usb_dw_wait_for_ahb_idle();
    DWC_GRSTCTL = CSRST;
    while (DWC_GRSTCTL & CSRST);
    usb_dw_wait_for_ahb_idle();

    /* Configure FIFOs */
    DWC_GRXFSIZ = c->rx_fifosz;
#ifdef USB_DW_SHARED_FIFO
    DWC_GNPTXFSIZ = (c->nptx_fifosz << 16) | c->rx_fifosz;
#else
    DWC_TX0FSIZ = (c->nptx_fifosz << 16) | c->rx_fifosz;
#endif
    for (int i = 0; i < n_ptxfifos; i++)
        DWC_DIEPTXF(i) = (c->ptx_fifosz << 16) |
                        (c->nptx_fifosz + c->rx_fifosz + c->ptx_fifosz*i);
    /*
     * According to p428 of the design guide, we need to ensure that
     * fifos are flushed before continuing.
     */
    usb_dw_flush_fifo(TXFFLSH|RXFFLSH, 0x10);

    /* Configure the core */
    DWC_GUSBCFG = gusbcfg;

    uint32_t gahbcfg = GINT;
#ifdef USB_DW_ARCH_SLAVE
#ifdef USB_DW_SHARED_FIFO
    if (c->use_ptxfifo_as_plain_buffer)
        gahbcfg |= PTXFELVL;
#endif
    if (c->disable_double_buffering)
        gahbcfg |= TXFELVL;
#else
    gahbcfg |= HBSTLEN(c->ahb_burst_len)|DMAEN;
#endif
    DWC_GAHBCFG = gahbcfg;

    DWC_DCFG = NZLSOHSK | USB_DW_DCFG_SPEED;
#ifdef USB_DW_SHARED_FIFO
    /* Set EP mismatch counter to the maximum */
    DWC_DCFG |= EPMISCNT(0x1f);
#endif

#if !defined(USB_DW_ARCH_SLAVE) && !defined(USB_DW_SHARED_FIFO)
    if (c->ahb_threshold)
        DWC_DTHRCTL = ARPEN|RXTHRLEN(c->ahb_threshold)|RXTHREN;
#endif

    /* Set up interrupts */
    DWC_DOEPMSK = STUP|XFRC;
    DWC_DIEPMSK = TOC|XFRC;

    /* Unmask all available endpoints */
    DWC_DAINTMSK = 0xffffffff;
    usb_endpoints = DWC_DAINTMSK;

    uint32_t gintmsk = USBRST|ENUMDNE|IEPINT|OEPINT;
#ifdef USB_DW_ARCH_SLAVE
    gintmsk |= RXFLVL;
#endif
#ifdef USB_DW_SHARED_FIFO
    gintmsk |= EPMIS;
#endif
    DWC_GINTMSK = gintmsk;

    usb_dw_reset_endpoints();

    /* Soft disconnect */
    DWC_DCTL = SDIS;

    usb_dw_target_clear_irq();
    usb_dw_target_enable_irq();

    /* Soft reconnect */
    udelay(3000);
    DWC_DCTL &= ~SDIS;
}

static void usb_dw_exit(void)
{
    /* Soft disconnect */
    DWC_DCTL = SDIS;
    udelay(10);

    DWC_PCGCCTL = 1; /* Stop Phy clock */

    /* Disable IRQs */
    usb_dw_target_disable_irq();

    /* Disable clocks */
    usb_dw_target_disable_clocks();
}


/*
 * API functions
 */

/* Cancel transfers on configured EPs */
void usb_drv_cancel_all_transfers()
{
    usb_dw_target_disable_irq();
    for (int ep = 1; ep < USB_NUM_ENDPOINTS; ep++)
        for (int dir = 0; dir < USB_DW_NUM_DIRS; dir++)
            if (usb_endpoints & (1 << (ep + USB_DW_DIR_OFF(dir))))
                if (usb_dw_get_ep(ep, dir)->active)
                {
                    //usb_dw_flush_endpoint(ep, dir);
                    usb_dw_abort_endpoint(ep, dir);
                    DWC_EPCTL(ep, dir) |= SETD0PIDEF;
                }
    usb_dw_target_enable_irq();
}

bool usb_drv_stalled(int endpoint, bool in)
{
    return usb_dw_get_stall(EP_NUM(endpoint),
                    in ? USB_DW_EPDIR_IN : USB_DW_EPDIR_OUT);
}

void usb_drv_stall(int endpoint, bool stall, bool in)
{
    usb_dw_target_disable_irq();
    usb_dw_set_stall(EP_NUM(endpoint),
                    in ? USB_DW_EPDIR_IN : USB_DW_EPDIR_OUT, stall);
    usb_dw_target_enable_irq();
}

void usb_drv_set_address(int address)
{
#if 1
    /* Ignored intentionally, because the controller requires us to set the
       new address before sending the response for some reason. So we'll
       already set it when the control request arrives, before passing that
       into the USB core, which will then call this dummy function. */
    (void)address;
#else
    usb_dw_target_disable_irq();
    usb_dw_set_address(address);
    usb_dw_target_enable_irq();
#endif
}

int usb_drv_port_speed(void)
{
    return ((DWC_DSTS & 0x6) == 0);
}

void usb_drv_set_test_mode(int mode)
{
    (void)mode;
    /* Ignore this for now */
}

void usb_attach(void)
{
}

static struct usb_drv_hw_info hw_info;

const struct usb_drv_hw_info *usb_drv_get_hw_info(void)
{
    return &hw_info;
}

/* GHWCFG2 bits 2:0 are the core's OTG mode: 0-2 are OTG (either role),
 * 3-4 device only, 5-6 host only. Bits 17:14 are host channels - 1. */
static void usb_dw_read_hw_info(void)
{
    uint32_t mode;

    hw_info.regs[0].name = "GSNPSID";
    hw_info.regs[0].val = DWC_GSNPSID;
    hw_info.regs[1].name = "GHWCFG2";
    hw_info.regs[1].val = DWC_GHWCFG2;
    hw_info.regs[2].name = "GHWCFG3";
    hw_info.regs[2].val = DWC_GHWCFG3;
    hw_info.regs[3].name = "GHWCFG4";
    hw_info.regs[3].val = DWC_GHWCFG4;
    mode = hw_info.regs[1].val & 7;
    hw_info.host_capable = mode <= 2 || mode == 5 || mode == 6;
    hw_info.host_units = ((hw_info.regs[1].val >> 14) & 0xf) + 1;
    hw_info.nregs = 4;
}

void usb_drv_init(void)
{
    usb_dw_init();
    usb_dw_read_hw_info();
}

/* Host probe. HPRT's change bits clear when written as 1, and so does its
 * enable bit, so every write masks all four. Interrupts stay off: the
 * device-mode handler must never see a host-mode core. */
#define HPRT_CONN_STS   (1<<0)
#define HPRT_ENA        (1<<2)
#define HPRT_RST        (1<<8)
#define HPRT_PWR        (1<<12)
#define HPRT_WRITE_MASK ((1<<1)|(1<<2)|(1<<3)|(1<<5))
#define GOTGCTL_SESVLD  ((1<<18)|(1<<19))

static bool host_active;
static bool host_reset_done;
static int host_resets;

/* Host channel registers */
#define HCCHAR_MPS(x)       (x)
#define HCCHAR_EPNUM(x)     ((x) << 11)
#define HCCHAR_EPDIR_IN     (1 << 15)
#define HCCHAR_EPTYPE(x)    ((x) << 18)     /* 0 control, 1 iso */
#define HCCHAR_MC(x)        ((x) << 20)
#define HCCHAR_DEVADDR(x)   ((x) << 22)
#define HCCHAR_ODDFRM       (1 << 29)
#define HCCHAR_CHDIS        (1 << 30)
#define HCCHAR_CHENA        (1u << 31)
#define HCTSIZ_PKTCNT(x)    ((x) << 19)
#define HCTSIZ_PID(x)       ((x) << 29)
#define PID_DATA0           0
#define PID_DATA1           2
#define PID_SETUP           3
#define HCINT_XFERC         (1 << 0)
#define HCINT_CHH           (1 << 1)
#define HCINT_ALL           0x7ff

/* Host-mode FIFOs, in words, from the core's 0x820 */
#define HOST_RXFIFO         0x300
#define HOST_NPTXFIFO       0x100
#define HOST_PTXFIFO        0x200

/* Everything the core's DMA touches. The CPU reaches it only through its
 * uncached alias; the core is given the plain address, which is physical.
 * The type's alignment pads it to whole cache lines. */
struct host_dw_dma {
    struct usb_ctrlrequest setup;
    uint8_t pad[24];
    uint8_t data[1024];
} __attribute__((aligned(32)));

static struct host_dw_dma host_dw_mem;
static struct host_dw_dma *hdw;
static uint32_t host_last_status;

#define HOST_PHYS(p) \
    USB_DW_PHYSADDR((uint32_t)&host_dw_mem + \
                    ((uint32_t)(p) - (uint32_t)hdw))

uint32_t usb_drv_host_last_status(void)
{
    return host_last_status;
}

/* One stage of a control transfer on channel 0. In DMA mode the core
 * retries NAKs itself and halts the channel when the stage ends, one way or
 * the other; XFERC says it ended well. An IN stage is sized in whole
 * packets and a short one ends it, so buf must hold len rounded up to 64.
 * Returns the bytes moved, or -1 with host_last_status the channel's
 * interrupt bits (bit 31 set if it never halted). */
static int host_stage(int addr, int pid, bool in, void *buf, int len)
{
    const int mps = 64;
    int pkts = len ? (len + mps - 1) / mps : 1;
    int size = in ? pkts * mps : len;
    uint32_t hcint = 0;

    DWC_HCINT(0) = HCINT_ALL;
    DWC_HCTSIZ(0) = size | HCTSIZ_PKTCNT(pkts) | HCTSIZ_PID(pid);
    DWC_HCDMA(0) = HOST_PHYS(buf);
    DWC_HCCHAR(0) = HCCHAR_MPS(mps) | HCCHAR_EPNUM(0) |
                    (in ? HCCHAR_EPDIR_IN : 0) | HCCHAR_EPTYPE(0) |
                    HCCHAR_MC(1) | HCCHAR_DEVADDR(addr) | HCCHAR_CHENA;

    for (int t = 0; t < 5000; t++)
    {
        hcint = DWC_HCINT(0);
        if (hcint & HCINT_CHH)
            break;
        udelay(100);
    }
    if (!(hcint & HCINT_CHH))
    {
        DWC_HCCHAR(0) |= HCCHAR_CHDIS | HCCHAR_CHENA;
        for (int t = 0; t < 100 && !(DWC_HCINT(0) & HCINT_CHH); t++)
            udelay(100);
        host_last_status = hcint | (1u << 31);
        return -1;
    }
    if (!(hcint & HCINT_XFERC))
    {
        host_last_status = hcint;
        return -1;
    }
    return in ? size - (int)(DWC_HCTSIZ(0) & 0x7ffff) : len;
}

int usb_drv_host_control(int addr, int reqtype, int req, int value,
                         int index, void *data, int len)
{
    bool in = reqtype & USB_DIR_IN;
    int n = 0;

    if (!host_active || len > (int)sizeof hdw->data)
        return -1;
    hdw->setup.bRequestType = reqtype;
    hdw->setup.bRequest = req;
    hdw->setup.wValue = value;
    hdw->setup.wIndex = index;
    hdw->setup.wLength = len;
    if (!in && len)
        memcpy(hdw->data, data, len);

    if (host_stage(addr, PID_SETUP, false, &hdw->setup, 8) != 8)
        return -1;
    if (len)
    {
        n = host_stage(addr, PID_DATA1, in, hdw->data, len);
        if (n < 0)
            return -1;
    }
    /* The status stage runs opposite to the data stage, and IN if none */
    if (host_stage(addr, PID_DATA1, !(len && in), hdw->data, 0) < 0)
        return -1;
    if (in && n > 0)
        memcpy(data, hdw->data, MIN(n, len));
    return MIN(n, len);
}

/* The isochronous stream. This core in buffer DMA mode has no schedule of
 * its own: a periodic channel carries one packet, in the (micro)frame its
 * ODDFRM bit names, and must be set up again for the next. So a 2 kHz timer
 * interrupt does that. Each OUT packet carries the samples owed since the
 * last one, counted from the core's own microframe counter, so the rate
 * follows USB time however the timer jitters. Feedback is read on a second
 * channel at its endpoint's interval. */
#define ISO_RATE        2000            /* packet setups per second */
#define ISO_CH_OUT      1
#define ISO_CH_FB       2
#define ISO_BUF_BYTES   512
#define HCINT_FRMOR     (1 << 9)
#define HFNUM_UFRAMES   0x3fff

struct host_dw_iso_dma {
    uint8_t buf[2][ISO_BUF_BYTES];
    uint32_t fb[8];
} __attribute__((aligned(32)));

static struct host_dw_iso_dma host_dw_iso_mem;
static struct host_dw_iso_dma *iso_dma;
static struct usb_drv_host_iso iso_cfg;
static struct usb_drv_host_iso_stats iso_stats;
static uint32_t iso_acc;                /* samples owed, 16.16 */
static unsigned int iso_last_uframe;
static int iso_buf, iso_bytes;          /* the packet on the OUT channel */
static bool iso_out_busy, iso_fb_busy;
static int iso_fb_every, iso_fb_count;

#define ISO_PHYS(p) \
    USB_DW_PHYSADDR((uint32_t)&host_dw_iso_mem + \
                    ((uint32_t)(p) - (uint32_t)iso_dma))

static void iso_arm(int ch, bool in, int ep, int mps, void *buf, int bytes)
{
    DWC_HCINT(ch) = HCINT_ALL;
    DWC_HCTSIZ(ch) = bytes | HCTSIZ_PKTCNT(1) | HCTSIZ_PID(PID_DATA0);
    DWC_HCDMA(ch) = ISO_PHYS(buf);
    /* the next microframe: ODDFRM names the parity it must have */
    DWC_HCCHAR(ch) = HCCHAR_MPS(mps) | HCCHAR_EPNUM(ep) |
                     (in ? HCCHAR_EPDIR_IN : 0) | HCCHAR_EPTYPE(1) |
                     HCCHAR_MC(1) | HCCHAR_DEVADDR(iso_cfg.addr) |
                     ((DWC_HFNUM & 1) ? 0 : HCCHAR_ODDFRM) | HCCHAR_CHENA;
}

static void iso_halt(int ch)
{
    if (!(DWC_HCCHAR(ch) & HCCHAR_CHENA))
        return;
    DWC_HCCHAR(ch) |= HCCHAR_CHDIS | HCCHAR_CHENA;
    for (int t = 0; t < 20 && !(DWC_HCINT(ch) & HCINT_CHH); t++)
        udelay(100);
}

/* A feedback value is 16.16 samples per microframe. One more than an
 * eighth away from nominal is taken as noise, not a rate. */
static void iso_take_feedback(uint32_t v)
{
    uint32_t nom = iso_cfg.nominal;

    iso_stats.fb_raw = v;
    if (v > nom - nom / 8 && v < nom + nom / 8)
    {
        iso_stats.feedback = v;
        iso_stats.fb_ok++;
    }
    else
        iso_stats.fb_bad++;
}

static void iso_feedback(void)
{
    uint32_t hcint;

    if (iso_fb_busy)
    {
        hcint = DWC_HCINT(ISO_CH_FB);
        if (!(hcint & HCINT_CHH))
            return;
        iso_fb_busy = false;
        if (hcint & HCINT_XFERC)
        {
            int got = iso_cfg.mps_fb - (int)(DWC_HCTSIZ(ISO_CH_FB) & 0x7ffff);
            if (got >= 3)
                iso_take_feedback(iso_dma->fb[0]);
        }
        else if (!(hcint & HCINT_FRMOR))
            iso_stats.errors++;
    }
    if (++iso_fb_count >= iso_fb_every)
    {
        iso_fb_count = 0;
        iso_dma->fb[0] = 0;
        iso_arm(ISO_CH_FB, true, iso_cfg.ep_fb & 0xf, iso_cfg.mps_fb,
                iso_dma->fb, iso_cfg.mps_fb);
        iso_fb_busy = true;
    }
}

/* Timer interrupt */
static void iso_timer(void)
{
    uint32_t hcint;
    unsigned int now, delta;
    int max, n;

    if (!(DWC_HPRT & HPRT_CONN_STS))
    {
        if (!iso_stats.lost)
        {
            iso_stats.lost = true;
            if (iso_cfg.lost)
                iso_cfg.lost();
        }
        return;
    }
    if (iso_cfg.ep_fb)
        iso_feedback();

    if (iso_out_busy)
    {
        hcint = DWC_HCINT(ISO_CH_OUT);
        if (!(hcint & HCINT_CHH))
            return;                     /* not sent yet */
        iso_out_busy = false;
        if (hcint & HCINT_FRMOR)
        {
            /* set up too late for its microframe: send it again */
            iso_arm(ISO_CH_OUT, false, iso_cfg.ep_out, iso_cfg.mps_out,
                    iso_dma->buf[iso_buf], iso_bytes);
            iso_out_busy = true;
            return;
        }
        if (!(hcint & HCINT_XFERC))
            iso_stats.errors++;
    }

    if (iso_cfg.begin && !iso_cfg.begin())
        return;

    now = DWC_HFNUM & HFNUM_UFRAMES;
    delta = (now - iso_last_uframe) & HFNUM_UFRAMES;
    iso_last_uframe = now;
    iso_acc += iso_stats.feedback * MIN(delta, 64u);

    max = MIN(iso_cfg.mps_out, ISO_BUF_BYTES) / iso_cfg.frame_bytes;
    n = iso_acc >> 16;
    if (n > max)
    {
        n = max;
        iso_stats.underruns++;
        /* never owe more than a few packets' worth */
        if (iso_acc >> 16 > (uint32_t)max * 4)
            iso_acc = (uint32_t)max * 4 << 16;
    }
    iso_acc -= (uint32_t)n << 16;

    iso_buf ^= 1;
    iso_bytes = n * iso_cfg.frame_bytes;
    iso_cfg.fill(iso_dma->buf[iso_buf], n);
    iso_arm(ISO_CH_OUT, false, iso_cfg.ep_out, iso_cfg.mps_out,
            iso_dma->buf[iso_buf], iso_bytes);
    iso_out_busy = true;
    iso_stats.frames++;
}

bool usb_drv_host_iso_start(const struct usb_drv_host_iso *iso)
{
    if (!host_active || iso_stats.running || iso->frame_bytes <= 0)
        return false;

    commit_discard_dcache_range(&host_dw_iso_mem, sizeof host_dw_iso_mem);
    iso_dma = USB_DW_UNCACHEDADDR(&host_dw_iso_mem);
    iso_cfg = *iso;
    memset(&iso_stats, 0, sizeof iso_stats);
    iso_stats.feedback = iso->nominal;
    iso_acc = 0;
    iso_out_busy = iso_fb_busy = false;
    iso_fb_every = MAX(1, iso->interval_fb * ISO_RATE / 8000);
    iso_fb_count = iso_fb_every;
    iso_last_uframe = DWC_HFNUM & HFNUM_UFRAMES;

    iso_stats.running = true;
    if (!timer_register(1, NULL, TIMER_FREQ / ISO_RATE, iso_timer))
    {
        iso_stats.running = false;
        return false;
    }
    return true;
}

void usb_drv_host_iso_stop(void)
{
    if (!iso_stats.running)
        return;
    timer_unregister();
    iso_stats.running = false;
    iso_halt(ISO_CH_OUT);
    iso_halt(ISO_CH_FB);
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
#ifndef USB_DW_TURNAROUND
    int USB_DW_TURNAROUND =
        (usb_dw_config.phytype == DWC_PHYTYPE_UTMI_16) ? 5 : 9;
#endif
    uint32_t gusbcfg = usb_dw_config.phytype | TRDT(USB_DW_TURNAROUND) |
                       USB_DW_TOUTCAL;

    usb_dw_target_disable_irq();
    usb_dw_target_enable_clocks();
    DWC_PCGCCTL = 0;

    /* PHY type before the core reset, force-host after it */
    DWC_GUSBCFG = gusbcfg;
    udelay(100);
    usb_dw_wait_for_ahb_idle();
    DWC_GRSTCTL = CSRST;
    while (DWC_GRSTCTL & CSRST);
    usb_dw_wait_for_ahb_idle();
    DWC_GUSBCFG = gusbcfg | FHMOD;
    for (int i = 0; i < 100 && !(DWC_GINTSTS & CMOD); i++)
        udelay(1000);

    commit_discard_dcache_range(&host_dw_mem, sizeof host_dw_mem);
    hdw = USB_DW_UNCACHEDADDR(&host_dw_mem);
#ifdef HAVE_USB_HOST
    usb_host_enum_clear();
#endif

    /* DMA on, interrupts off: everything is polled */
    DWC_GAHBCFG = HBSTLEN(usb_dw_config.ahb_burst_len) | DMAEN;
    DWC_GINTMSK = 0;
    DWC_GRXFSIZ = HOST_RXFIFO;
    DWC_TX0FSIZ = (HOST_NPTXFIFO << 16) | HOST_RXFIFO;
    DWC_HPTXFSIZ = (HOST_PTXFIFO << 16) | (HOST_RXFIFO + HOST_NPTXFIFO);
    usb_dw_flush_fifo(TXFFLSH | RXFFLSH, 0x10);
    DWC_HCFG = 0;       /* 30/60 MHz PHY clock, high speed allowed */
    DWC_HPRT = HPRT_PWR;

    host_reset_done = false;
    host_resets = 0;
    host_active = true;
}

void usb_drv_host_stop(void)
{
    usb_drv_host_iso_stop();
    host_active = false;
    DWC_HPRT = 0;
    DWC_GUSBCFG &= ~FHMOD;
    DWC_PCGCCTL = 1;
    usb_dw_target_disable_clocks();
}

void usb_drv_host_poll(struct usb_drv_host_status *st)
{
    static const char * const speeds[] = { "high", "full", "low", "?" };
    uint32_t hprt;

    st->active = host_active;
    if (!host_active)
        return;

    hprt = DWC_HPRT;
    if (!(hprt & HPRT_CONN_STS))
    {
        usb_drv_host_iso_stop();
        host_reset_done = false;
#ifdef HAVE_USB_HOST
        usb_host_enum_clear();
#endif
    }
    else if (!host_reset_done)
    {
        /* 100 ms connect debounce, then a 60 ms root-port reset, which
         * this core leaves to software to end. */
        host_reset_done = true;
        host_resets++;
        udelay(100000);
        DWC_HPRT = (DWC_HPRT & ~HPRT_WRITE_MASK) | HPRT_RST;
        udelay(60000);
        DWC_HPRT = DWC_HPRT & ~(HPRT_WRITE_MASK | HPRT_RST);
        udelay(10000);
        hprt = DWC_HPRT;
#ifdef HAVE_USB_HOST
        if (hprt & HPRT_ENA)
        {
            usb_host_enumerate();
            hprt = DWC_HPRT;
        }
#endif
    }

    st->host_mode = DWC_GINTSTS & CMOD;
    st->vbus = DWC_GOTGCTL & GOTGCTL_SESVLD;
    st->connected = hprt & HPRT_CONN_STS;
    st->enabled = hprt & HPRT_ENA;
    st->line = (hprt >> 10) & 3;
    st->speed = st->enabled ? speeds[(hprt >> 17) & 3] : "-";
    st->resets = host_resets;
    st->regs[0].name = "HPRT";
    st->regs[0].val = hprt;
    st->regs[1].name = "GOTGCTL";
    st->regs[1].val = DWC_GOTGCTL;
    st->regs[2].name = "GINTSTS";
    st->regs[2].val = DWC_GINTSTS;
    st->regs[3].name = "GUSBCFG";
    st->regs[3].val = DWC_GUSBCFG;
    st->nregs = 4;
}

void usb_drv_exit(void)
{
    usb_dw_exit();
}

void INT_USB_FUNC(void)
{
    usb_dw_irq();
}

void usb_drv_ep_reset_alloc_ctx(struct usb_drv_ep_alloc_ctx* ctx)
{
    memset(ctx, 0, sizeof(*ctx));
}

bool usb_drv_ep_allocate(struct usb_drv_ep_alloc_ctx* ctx, int ep, int type, int max_packet_size)
{
    const uint8_t epnum = EP_NUM(ep);
    const uint8_t epdir = EP_DIR(ep);

    if(ep == EP_CONTROL)
    {
        return false;
    }

    enum usb_dw_epdir dwdir = epdir == DIR_IN ? USB_DW_EPDIR_IN : USB_DW_EPDIR_OUT;
    if(!(usb_endpoints & (1 << (epnum + USB_DW_DIR_OFF(dwdir)))))
    {
        return false;
    }

    bool need_fifo = epdir == DIR_IN;
#ifdef USB_DW_SHARED_FIFO
    /* in shared fifo mode, only periodic endpoints need dedicated fifo */
    need_fifo &= type == USB_ENDPOINT_XFER_ISOC || type == USB_ENDPOINT_XFER_INT;
#endif
    if(!need_fifo)
    {
        goto ok;
    }

    for (int fnum = 1; fnum <= n_ptxfifos; fnum++)
    {
        if (~ctx->txfifo_usage & (1 << fnum))
        {
            ctx->txfifo_usage |= 1 << fnum;
            ctx->assigned_txfifos[epnum] = fnum;
            goto ok;
        }
    }

    return false;

ok:
    ctx->type[epnum][epdir] = type;
    ctx->max_packet_size[epnum][epdir] = max_packet_size;
    return true;
}

void usb_drv_ep_init(const struct usb_drv_ep_alloc_ctx* ctx, int ep)
{
    const int epnum = EP_NUM(ep);
    const int epdir_ = EP_DIR(ep);
    const int type = ctx->type[epnum][epdir_];

    enum usb_dw_epdir epdir = (epdir_ == DIR_IN) ? USB_DW_EPDIR_IN : USB_DW_EPDIR_OUT;
    struct usb_dw_ep* dw_ep = usb_dw_get_ep(EP_NUM(ep), epdir);

    int mps = ctx->max_packet_size[epnum][epdir_];
    if(mps == -1)
    {
        if(type == EPTYP_ISOCHRONOUS)
        {
            mps = usb_drv_port_speed() ? 1024 : 1023;
        }
        else
        {
            mps = usb_drv_port_speed() ? 512 : 64;
        }
    }

    usb_dw_target_disable_irq();
    usb_dw_configure_ep(ctx, epnum, epdir, type, mps);
    usb_dw_target_enable_irq();

    dw_ep->active = true;
}

void usb_drv_ep_deinit(const struct usb_drv_ep_alloc_ctx* ctx, int ep)
{
    (void)ctx;
    enum usb_dw_epdir epdir = (EP_DIR(ep) == DIR_IN) ? USB_DW_EPDIR_IN : USB_DW_EPDIR_OUT;
    struct usb_dw_ep* dw_ep = usb_dw_get_ep(EP_NUM(ep), epdir);

    usb_dw_target_disable_irq();
    usb_dw_unconfigure_ep(EP_NUM(ep), epdir);
    usb_dw_target_enable_irq();

    dw_ep->active = false;
}

int usb_drv_recv_nonblocking(int endpoint, void* ptr, int length)
{
    usb_dw_target_disable_irq();
    usb_dw_transfer(EP_NUM(endpoint), USB_DW_EPDIR_OUT, ptr, length);
    usb_dw_target_enable_irq();
    return 0;
}

int usb_drv_send_nonblocking(int endpoint, void *ptr, int length)
{
    usb_dw_target_disable_irq();
    usb_dw_transfer(EP_NUM(endpoint), USB_DW_EPDIR_IN, ptr, length);
    usb_dw_target_enable_irq();
    return 0;
}

int usb_drv_send(int endpoint, void *ptr, int length)
{
    int epnum = EP_NUM(endpoint);
    struct usb_dw_ep* dw_ep = usb_dw_get_ep(epnum, USB_DW_EPDIR_IN);

    semaphore_wait(&dw_ep->complete, 0);

    usb_drv_send_nonblocking(endpoint, ptr, length);

    if (semaphore_wait(&dw_ep->complete, HZ) == OBJ_WAIT_TIMEDOUT)
    {
        usb_dw_target_disable_irq();
        usb_dw_abort_endpoint(epnum, USB_DW_EPDIR_IN);
        usb_dw_target_enable_irq();
    }

    return dw_ep->status;
}

int usb_drv_get_frame_number(void)
{
    /*
     * SOFFN is a 14-bit microframe number for high-speed hosts and
     * a plain frame number for full-speed hosts.
     */
    if (usb_drv_port_speed())
        return (DWC_DSTS >> 11) & 0x7FF;
    else
        return (DWC_DSTS >> 8) & 0x3FFF;
}
