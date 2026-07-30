/*
 * dwc-hsotg (dwc2) USB host controller emulation
 *
 * Based on hw/usb/hcd-ehci.c and hw/usb/hcd-ohci.c
 *
 * Note that to use this emulation with the dwc-otg driver in the
 * Raspbian kernel, you must pass the option "dwc_otg.fiq_fsm_enable=0"
 * on the kernel command line.
 *
 * Some useful documentation used to develop this emulation can be
 * found online (as of April 2020) at:
 *
 * http://www.capital-micro.com/PDF/CME-M7_Family_User_Guide_EN.pdf
 * which has a pretty complete description of the controller starting
 * on page 370.
 *
 * https://sourceforge.net/p/wive-ng/wive-ng-mt/ci/master/tree/docs/DataSheets/RT3050_5x_V2.0_081408_0902.pdf
 * which has a description of the controller registers starting on
 * page 130.
 *
 * Copyright (c) 2020 Paul Zimmerman <pauldzim@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/usb/dwc2-regs.h"
#include "hw/usb/hcd-dwc2.h"
#include "migration/vmstate.h"
#include "trace.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"

static void dwc2_update_device_irq(DWC2State *s);
static void dwc2_host_pio_wake_waiters(DWC2State *s);

#define USB_HZ_FS       12000000
#define USB_HZ_HS       96000000
#define USB_FRMINTVL    12000

/* nifty macros from Arnon's EHCI version  */
#define get_field(data, field) \
    (((data) & field##_MASK) >> field##_SHIFT)

#define set_field(data, newval, field) do { \
    uint32_t val = *(data); \
    val &= ~field##_MASK; \
    val |= ((newval) << field##_SHIFT) & field##_MASK; \
    *(data) = val; \
} while (0)

#define get_bit(data, bitmask) \
    (!!((data) & (bitmask)))

/* update irq line */
static inline void dwc2_update_irq(DWC2State *s)
{
    static int oldlevel;
    int level = 0;

    if ((s->gintsts & s->gintmsk) && (s->gahbcfg & GAHBCFG_GLBL_INTR_EN)) {
        level = 1;
    }
    if (level != oldlevel) {
        oldlevel = level;
        trace_usb_dwc2_update_irq(level);
        qemu_set_irq(s->irq, level);
    }
}

/* flag interrupt condition */
static inline void dwc2_raise_global_irq(DWC2State *s, uint32_t intr)
{
    if (!(s->gintsts & intr)) {
        s->gintsts |= intr;
        trace_usb_dwc2_raise_global_irq(intr);
        dwc2_update_irq(s);
    }
}

static inline void dwc2_lower_global_irq(DWC2State *s, uint32_t intr)
{
    if (s->gintsts & intr) {
        s->gintsts &= ~intr;
        trace_usb_dwc2_lower_global_irq(intr);
        dwc2_update_irq(s);
    }
}

static inline void dwc2_raise_host_irq(DWC2State *s, uint32_t host_intr)
{
    if (!(s->haint & host_intr)) {
        s->haint |= host_intr;
        s->haint &= 0xffff;
        trace_usb_dwc2_raise_host_irq(host_intr);
        if (s->haint & s->haintmsk) {
            dwc2_raise_global_irq(s, GINTSTS_HCHINT);
        }
    }
}

static inline void dwc2_lower_host_irq(DWC2State *s, uint32_t host_intr)
{
    if (s->haint & host_intr) {
        s->haint &= ~host_intr;
        trace_usb_dwc2_lower_host_irq(host_intr);
        if (!(s->haint & s->haintmsk)) {
            dwc2_lower_global_irq(s, GINTSTS_HCHINT);
        }
    }
}

static inline void dwc2_update_hc_irq(DWC2State *s, int index)
{
    uint32_t host_intr = 1 << (index >> 3);

    if (s->hreg1[index + 2] & s->hreg1[index + 3]) {
        dwc2_raise_host_irq(s, host_intr);
    } else {
        dwc2_lower_host_irq(s, host_intr);
    }
}

/* set a timer for EOF */
static void dwc2_eof_timer(DWC2State *s)
{
    timer_mod(s->eof_timer, s->sof_time + s->usb_frame_time);
}

/* Set a timer for EOF and generate SOF event */
static void dwc2_sof(DWC2State *s)
{
    s->sof_time += s->usb_frame_time;
    trace_usb_dwc2_sof(s->sof_time);
    dwc2_eof_timer(s);
    dwc2_raise_global_irq(s, GINTSTS_SOF);
}

/* Do frame processing on frame boundary */
static void dwc2_frame_boundary(void *opaque)
{
    DWC2State *s = opaque;
    int64_t now;
    uint16_t frcnt;

    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    /* Frame boundary, so do EOF stuff here */

    /* Increment frame number */
    frcnt = (uint16_t)((now - s->sof_time) / s->fi);
    s->frame_number = (s->frame_number + frcnt) & 0xffff;
    s->hfnum = s->frame_number & HFNUM_MAX_FRNUM;

    /* Do SOF stuff here */
    dwc2_sof(s);
}

/* Start sending SOF tokens on the USB bus */
static void dwc2_bus_start(DWC2State *s)
{
    trace_usb_dwc2_bus_start();
    s->sof_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    dwc2_eof_timer(s);
}

/* Stop sending SOF tokens on the USB bus */
static void dwc2_bus_stop(DWC2State *s)
{
    trace_usb_dwc2_bus_stop();
    timer_del(s->eof_timer);
}

static void dwc2_reset_frame_counter(DWC2State *s)
{
    s->sof_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->frame_number = 0;
    s->hfnum = 0;
    if ((s->gintsts & GINTSTS_CURMODE_HOST) &&
        s->uport.dev && s->uport.dev->attached) {
        dwc2_eof_timer(s);
    }
}

static uint32_t dwc2_device_tx_fifo_words(DWC2State *s, unsigned int ep)
{
    uint32_t size;

    if (ep == 0) {
        size = s->gnptxfsiz;
    } else {
        size = s->dptxfsiz[ep - 1];
    }
    return MIN(FIFOSIZE_DEPTH_GET(size), DWC2_FIFO_WORDS);
}

static bool dwc2_host_channel_periodic(DWC2State *s, unsigned int channel)
{
    uint32_t type = get_field(s->hcchar(channel), HCCHAR_EPTYPE);

    return type == USB_ENDPOINT_XFER_ISOC ||
           type == USB_ENDPOINT_XFER_INT;
}

static uint32_t dwc2_host_tx_fifo_words(DWC2State *s, bool periodic)
{
    uint32_t size = periodic ? s->hptxfsiz : s->gnptxfsiz;

    return MIN(FIFOSIZE_DEPTH_GET(size), DWC2_FIFO_WORDS);
}

static uint32_t dwc2_host_tx_fifo_used(DWC2State *s, bool periodic)
{
    uint32_t used = 0;

    for (unsigned int channel = 0; channel < DWC2_NB_CHAN; channel++) {
        if (dwc2_host_channel_periodic(s, channel) == periodic) {
            used += DIV_ROUND_UP(s->host_tx_fifo_count[channel], 4);
        }
    }
    return used;
}

static void dwc2_device_refresh_fifo_state(DWC2State *s)
{
    if (s->rx_status_count) {
        s->grxstsr = s->rx_status[s->rx_status_head];
        s->gintsts |= GINTSTS_RXFLVL;
    } else {
        s->grxstsr = 0;
        s->gintsts &= ~GINTSTS_RXFLVL;
    }

    if (s->gintsts & GINTSTS_CURMODE_HOST) {
        uint32_t np_words = dwc2_host_tx_fifo_words(s, false);
        uint32_t np_used = dwc2_host_tx_fifo_used(s, false);
        uint32_t p_words = dwc2_host_tx_fifo_words(s, true);
        uint32_t p_used = dwc2_host_tx_fifo_used(s, true);

        s->gnptxsts =
            (s->gnptxsts & ~GNPTXSTS_NP_TXF_SPC_AVAIL_MASK) |
            (np_words > np_used ? np_words - np_used : 0);
        s->hptxsts = (s->hptxsts & ~TXSTS_FSPCAVAIL_MASK) |
                     (p_words > p_used ? p_words - p_used : 0);
    } else {
        for (unsigned int ep = 0; ep < DWC2_DEV_EP_COUNT; ep++) {
            uint32_t words = dwc2_device_tx_fifo_words(s, ep);
            uint32_t used = DIV_ROUND_UP(s->tx_fifo_count[ep], 4);

            s->dtxfsts(ep) = words > used ? words - used : 0;
        }
        s->gnptxsts =
            (s->gnptxsts & ~GNPTXSTS_NP_TXF_SPC_AVAIL_MASK) |
            s->dtxfsts(0);
    }
}

static void dwc2_device_flush_rx_fifo(DWC2State *s)
{
    s->rx_fifo_head = 0;
    s->rx_fifo_count = 0;
    s->rx_status_head = 0;
    s->rx_status_count = 0;
    s->grxstsp = 0;
    dwc2_device_refresh_fifo_state(s);
}

static void dwc2_device_flush_tx_fifo(DWC2State *s, unsigned int ep)
{
    if (ep >= DWC2_DEV_EP_COUNT) {
        return;
    }
    s->tx_fifo_head[ep] = 0;
    s->tx_fifo_count[ep] = 0;
    dwc2_device_refresh_fifo_state(s);
}

static void dwc2_device_flush_all_tx_fifos(DWC2State *s)
{
    memset(s->tx_fifo_head, 0, sizeof(s->tx_fifo_head));
    memset(s->tx_fifo_count, 0, sizeof(s->tx_fifo_count));
    dwc2_device_refresh_fifo_state(s);
}

static void dwc2_device_flush_fifos(DWC2State *s)
{
    dwc2_device_flush_rx_fifo(s);
    dwc2_device_flush_all_tx_fifos(s);
}

static uint32_t dwc2_device_pop_rx_status(DWC2State *s)
{
    uint32_t status;

    if (!s->rx_status_count) {
        return 0;
    }
    status = s->rx_status[s->rx_status_head];
    s->rx_status_head = (s->rx_status_head + 1) %
                        DWC2_RX_STATUS_COUNT;
    s->rx_status_count--;
    s->grxstsp = status;
    dwc2_device_refresh_fifo_state(s);
    dwc2_host_pio_wake_waiters(s);
    dwc2_update_irq(s);
    return status;
}

static bool dwc2_device_queue_rx_status(DWC2State *s, uint32_t status)
{
    uint32_t tail;

    if (s->rx_status_count == DWC2_RX_STATUS_COUNT) {
        return false;
    }
    tail = (s->rx_status_head + s->rx_status_count) %
           DWC2_RX_STATUS_COUNT;
    s->rx_status[tail] = status;
    s->rx_status_count++;
    dwc2_device_refresh_fifo_state(s);
    return true;
}

static void dwc2_device_rx_fifo_write(DWC2State *s, const uint8_t *buf,
                                      size_t len)
{
    uint32_t tail = (s->rx_fifo_head + s->rx_fifo_count) %
                    DWC2_FIFO_BYTES;

    for (size_t i = 0; i < len; i++) {
        s->rx_fifo[tail] = buf ? buf[i] : 0;
        tail = (tail + 1) % DWC2_FIFO_BYTES;
    }
    s->rx_fifo_count += len;
}

static uint32_t dwc2_device_rx_fifo_read(DWC2State *s)
{
    uint32_t val = 0;

    for (unsigned int byte = 0; byte < sizeof(val); byte++) {
        if (!s->rx_fifo_count) {
            break;
        }
        val |= (uint32_t)s->rx_fifo[s->rx_fifo_head] << (byte * 8);
        s->rx_fifo_head = (s->rx_fifo_head + 1) % DWC2_FIFO_BYTES;
        s->rx_fifo_count--;
    }
    dwc2_device_refresh_fifo_state(s);
    dwc2_host_pio_wake_waiters(s);
    return val;
}

static bool dwc2_device_queue_rx_packet(DWC2State *s, unsigned int ep,
                                        const void *buf, size_t len,
                                        bool setup, bool complete)
{
    size_t stored = ROUND_UP(len, sizeof(uint32_t));
    uint32_t depth = MIN(s->grxfsiz & GRXFSIZ_DEPTH_MASK,
                         DWC2_FIFO_WORDS) * sizeof(uint32_t);
    unsigned int statuses = 1 + (setup || complete);
    uint32_t packet_status;

    if (stored > depth || s->rx_fifo_count > depth - stored ||
        s->rx_status_count > DWC2_RX_STATUS_COUNT - statuses) {
        return false;
    }

    dwc2_device_rx_fifo_write(s, buf, len);
    dwc2_device_rx_fifo_write(s, NULL, stored - len);
    packet_status = ep |
                    ((uint32_t)len << GRXSTS_BYTECNT_SHIFT) |
                    ((setup ? GRXSTS_PKTSTS_SETUPRX :
                              GRXSTS_PKTSTS_OUTRX)
                     << GRXSTS_PKTSTS_SHIFT);
    g_assert(dwc2_device_queue_rx_status(s, packet_status));
    if (setup || complete) {
        packet_status = ep |
                        ((setup ? GRXSTS_PKTSTS_SETUPDONE :
                                  GRXSTS_PKTSTS_OUTDONE)
                         << GRXSTS_PKTSTS_SHIFT);
        g_assert(dwc2_device_queue_rx_status(s, packet_status));
    }
    return true;
}

static bool dwc2_device_tx_fifo_write(DWC2State *s, unsigned int ep,
                                      uint32_t val)
{
    uint32_t capacity = dwc2_device_tx_fifo_words(s, ep) *
                        sizeof(uint32_t);
    uint32_t tail;

    if (ep >= DWC2_DEV_EP_COUNT || capacity < sizeof(val) ||
        s->tx_fifo_count[ep] > capacity - sizeof(val)) {
        return false;
    }
    tail = (s->tx_fifo_head[ep] + s->tx_fifo_count[ep]) %
           DWC2_FIFO_BYTES;
    for (unsigned int byte = 0; byte < sizeof(val); byte++) {
        s->tx_fifo[ep][tail] = val >> (byte * 8);
        tail = (tail + 1) % DWC2_FIFO_BYTES;
    }
    s->tx_fifo_count[ep] += sizeof(val);
    dwc2_device_refresh_fifo_state(s);
    return true;
}

static void dwc2_device_tx_fifo_read(DWC2State *s, unsigned int ep,
                                     void *buf, size_t len)
{
    uint8_t *data = buf;

    for (size_t i = 0; i < len; i++) {
        data[i] = s->tx_fifo[ep][s->tx_fifo_head[ep]];
        s->tx_fifo_head[ep] = (s->tx_fifo_head[ep] + 1) %
                              DWC2_FIFO_BYTES;
        s->tx_fifo_count[ep]--;
    }
    dwc2_device_refresh_fifo_state(s);
}

static void dwc2_host_pio_rx_reserved(DWC2State *s, size_t *bytes,
                                      unsigned int *statuses)
{
    *bytes = 0;
    *statuses = 0;
    for (unsigned int channel = 0; channel < DWC2_NB_CHAN; channel++) {
        if (s->host_pio_rx_reserved_active[channel]) {
            *bytes += s->host_pio_rx_reserved_bytes[channel];
            *statuses += 2;
        }
    }
}

static bool dwc2_host_pio_rx_has_space(DWC2State *s, size_t len)
{
    uint32_t depth = MIN(s->grxfsiz & GRXFSIZ_DEPTH_MASK,
                         DWC2_FIFO_WORDS) * sizeof(uint32_t);
    size_t stored = ROUND_UP(len, sizeof(uint32_t));
    size_t reserved;
    unsigned int reserved_statuses;

    dwc2_host_pio_rx_reserved(s, &reserved, &reserved_statuses);
    return reserved <= depth && s->rx_fifo_count <= depth - reserved &&
           stored <= depth - reserved - s->rx_fifo_count &&
           reserved_statuses <= DWC2_RX_STATUS_COUNT &&
           s->rx_status_count <=
               DWC2_RX_STATUS_COUNT - reserved_statuses &&
           s->rx_status_count + reserved_statuses <=
               DWC2_RX_STATUS_COUNT - 2;
}

static void dwc2_host_pio_rx_reserve(DWC2State *s, unsigned int channel,
                                     size_t len)
{
    g_assert(channel < DWC2_NB_CHAN);
    g_assert(!s->host_pio_rx_reserved_active[channel]);
    g_assert(dwc2_host_pio_rx_has_space(s, len));
    s->host_pio_rx_reserved_bytes[channel] =
        ROUND_UP(len, sizeof(uint32_t));
    s->host_pio_rx_reserved_active[channel] = true;
}

static void dwc2_host_pio_rx_release(DWC2State *s, unsigned int channel)
{
    if (channel >= DWC2_NB_CHAN ||
        !s->host_pio_rx_reserved_active[channel]) {
        return;
    }
    s->host_pio_rx_reserved_bytes[channel] = 0;
    s->host_pio_rx_reserved_active[channel] = false;
    dwc2_host_pio_wake_waiters(s);
}

static bool dwc2_host_pio_channel_ready(DWC2State *s, unsigned int channel)
{
    uint32_t hcchar = s->hcchar(channel);
    uint32_t hctsiz = s->hctsiz(channel);
    uint32_t len = get_field(hctsiz, TSIZ_XFERSIZE);
    uint32_t mps = get_field(hcchar, HCCHAR_MPS);
    uint32_t type = get_field(hcchar, HCCHAR_EPTYPE);
    uint32_t pid = get_field(hctsiz, TSIZ_SC_MC_PID);
    bool in = hcchar & HCCHAR_EPDIR;
    size_t packet;

    if (!mps) {
        return true;
    }
    packet = MIN(len, mps);
    if (type == USB_ENDPOINT_XFER_CONTROL &&
        pid == TSIZ_SC_MC_PID_SETUP) {
        in = false;
    }
    if (in) {
        return dwc2_host_pio_rx_has_space(s, packet);
    }
    return s->host_tx_fifo_count[channel] >=
           ROUND_UP(packet, sizeof(uint32_t));
}

static void dwc2_host_pio_wake_waiters(DWC2State *s)
{
    bool wake = false;

    if (!(s->gintsts & GINTSTS_CURMODE_HOST) ||
        (s->gahbcfg & GAHBCFG_DMA_EN)) {
        return;
    }
    for (unsigned int channel = 0; channel < DWC2_NB_CHAN; channel++) {
        if (s->host_pio_waiting[channel] &&
            dwc2_host_pio_channel_ready(s, channel)) {
            s->host_pio_waiting[channel] = false;
            s->packet[channel].needs_service = true;
            wake = true;
        }
    }
    if (wake) {
        qemu_bh_schedule(s->async_bh);
    }
}

static bool dwc2_host_tx_fifo_write(DWC2State *s, unsigned int channel,
                                    uint32_t val)
{
    bool periodic;
    uint32_t capacity;
    uint32_t used;
    uint32_t tail;

    if (channel >= DWC2_NB_CHAN) {
        return false;
    }
    periodic = dwc2_host_channel_periodic(s, channel);
    capacity = dwc2_host_tx_fifo_words(s, periodic) *
               sizeof(uint32_t);
    used = dwc2_host_tx_fifo_used(s, periodic) * sizeof(uint32_t);
    if (capacity < sizeof(val) || used > capacity - sizeof(val)) {
        return false;
    }
    tail = (s->host_tx_fifo_head[channel] +
            s->host_tx_fifo_count[channel]) % DWC2_FIFO_BYTES;
    for (unsigned int byte = 0; byte < sizeof(val); byte++) {
        s->host_tx_fifo[channel][tail] = val >> (byte * 8);
        tail = (tail + 1) % DWC2_FIFO_BYTES;
    }
    s->host_tx_fifo_count[channel] += sizeof(val);
    dwc2_device_refresh_fifo_state(s);
    dwc2_host_pio_wake_waiters(s);
    return true;
}

static void dwc2_host_tx_fifo_peek(DWC2State *s, unsigned int channel,
                                   void *buf, size_t len)
{
    uint8_t *data = buf;
    uint32_t head = s->host_tx_fifo_head[channel];

    for (size_t i = 0; i < len; i++) {
        data[i] = s->host_tx_fifo[channel][head];
        head = (head + 1) % DWC2_FIFO_BYTES;
    }
}

static void dwc2_host_tx_fifo_consume(DWC2State *s, unsigned int channel,
                                      size_t len)
{
    size_t stored = ROUND_UP(len, sizeof(uint32_t));

    for (size_t i = 0; i < stored; i++) {
        s->host_tx_fifo_head[channel] =
            (s->host_tx_fifo_head[channel] + 1) % DWC2_FIFO_BYTES;
        s->host_tx_fifo_count[channel]--;
    }
    dwc2_device_refresh_fifo_state(s);
}

static bool dwc2_host_queue_rx_packet(DWC2State *s, unsigned int channel,
                                      uint32_t pid, const void *buf,
                                      size_t len)
{
    size_t stored = ROUND_UP(len, sizeof(uint32_t));
    uint32_t status;

    if (!dwc2_host_pio_rx_has_space(s, len)) {
        return false;
    }
    dwc2_device_rx_fifo_write(s, buf, len);
    dwc2_device_rx_fifo_write(s, NULL, stored - len);
    status = channel | ((uint32_t)len << GRXSTS_BYTECNT_SHIFT) |
             ((pid & 3) << GRXSTS_DPID_SHIFT) |
             (GRXSTS_PKTSTS_HCHIN << GRXSTS_PKTSTS_SHIFT);
    g_assert(dwc2_device_queue_rx_status(s, status));
    status = channel |
             (GRXSTS_PKTSTS_HCHIN_XFER_COMP << GRXSTS_PKTSTS_SHIFT);
    g_assert(dwc2_device_queue_rx_status(s, status));
    return true;
}

static void dwc2_host_flush_tx_fifos(DWC2State *s, int periodic)
{
    for (unsigned int channel = 0; channel < DWC2_NB_CHAN; channel++) {
        if (periodic < 0 ||
            dwc2_host_channel_periodic(s, channel) == periodic) {
            s->host_tx_fifo_head[channel] = 0;
            s->host_tx_fifo_count[channel] = 0;
            s->host_pio_waiting[channel] =
                (s->hcchar(channel) & HCCHAR_CHENA) &&
                !(s->gahbcfg & GAHBCFG_DMA_EN);
            if (s->host_pio_waiting[channel]) {
                s->packet[channel].needs_service = false;
            }
        }
    }
    dwc2_device_refresh_fifo_state(s);
}

static void dwc2_abort_host_packets(DWC2State *s)
{
    timer_del(s->frame_timer);
    qemu_bh_cancel(s->async_bh);
    s->working = false;
    s->next_chan = 0;

    for (unsigned int channel = 0; channel < DWC2_NB_CHAN; channel++) {
        DWC2Packet *packet = &s->packet[channel];

        if (packet->async == DWC2_ASYNC_INFLIGHT) {
            usb_cancel_packet(&packet->packet);
            usb_packet_cleanup(&packet->packet);
        }
        dwc2_host_pio_rx_release(s, channel);
        packet->async = DWC2_ASYNC_NONE;
        packet->needs_service = false;
    }
}

static void dwc2_host_state_machine_reset(DWC2State *s)
{
    dwc2_abort_host_packets(s);
    memset(s->hreg1, 0, sizeof(s->hreg1));
    dwc2_host_flush_tx_fifos(s, -1);
    dwc2_device_flush_rx_fifo(s);
    s->haint = 0;
    s->gintsts &= ~GINTSTS_HCHINT;
    dwc2_reset_frame_counter(s);
}

static void dwc2_device_state_machine_reset(DWC2State *s)
{
    memset(s->diepreg, 0, sizeof(s->diepreg));
    memset(s->doepreg, 0, sizeof(s->doepreg));
    s->daint = 0;
    s->gintsts &= ~(GINTSTS_IEPINT | GINTSTS_OEPINT |
                    GINTSTS_GOUTNAKEFF | GINTSTS_GINNAKEFF);
    s->dctl &= ~(DCTL_GOUTNAKSTS | DCTL_GNPINNAKSTS);
    dwc2_device_flush_fifos(s);
}

static void dwc2_core_state_machine_reset(DWC2State *s)
{
    uint32_t mode = s->gintsts & GINTSTS_CURMODE_HOST;

    dwc2_host_state_machine_reset(s);
    dwc2_device_state_machine_reset(s);
    s->gotgint = 0;
    s->gintsts = mode | GINTSTS_PTXFEMP | GINTSTS_NPTXFEMP;
    s->grxstsr = 0;
    s->grxstsp = 0;
    dwc2_update_irq(s);
}

static USBDevice *dwc2_find_device(DWC2State *s, uint8_t addr)
{
    USBDevice *dev;

    trace_usb_dwc2_find_device(addr);

    if (!(s->hprt0 & HPRT0_ENA)) {
        trace_usb_dwc2_port_disabled(0);
    } else {
        dev = usb_find_device(&s->uport, addr);
        if (dev != NULL) {
            trace_usb_dwc2_device_found(0);
            return dev;
        }
    }

    trace_usb_dwc2_device_not_found();
    return NULL;
}

static const char *pstatus[] = {
    "USB_RET_SUCCESS", "USB_RET_NODEV", "USB_RET_NAK", "USB_RET_STALL",
    "USB_RET_BABBLE", "USB_RET_IOERROR", "USB_RET_ASYNC",
    "USB_RET_ADD_TO_QUEUE", "USB_RET_REMOVE_FROM_QUEUE"
};

static uint32_t pintr[] = {
    HCINTMSK_XFERCOMPL, HCINTMSK_XACTERR, HCINTMSK_NAK, HCINTMSK_STALL,
    HCINTMSK_BBLERR, HCINTMSK_XACTERR, HCINTMSK_XACTERR, HCINTMSK_XACTERR,
    HCINTMSK_XACTERR
};

static const char *types[] = {
    "Ctrl", "Isoc", "Bulk", "Intr"
};

static const char *dirs[] = {
    "Out", "In"
};

static void dwc2_handle_packet(DWC2State *s, uint32_t devadr, USBDevice *dev,
                               USBEndpoint *ep, uint32_t index, bool send)
{
    DWC2Packet *p;
    uint32_t hcchar = s->hreg1[index];
    uint32_t hctsiz = s->hreg1[index + 4];
    uint32_t hcdma = s->hreg1[index + 5];
    uint32_t chan, epnum, epdir, eptype, mps, pid, pcnt, len, tlen, intr = 0;
    uint32_t tpcnt, stsidx, actual = 0;
    bool do_intr = false, done = false;
    bool dma = s->gahbcfg & GAHBCFG_DMA_EN;

    epnum = get_field(hcchar, HCCHAR_EPNUM);
    epdir = get_bit(hcchar, HCCHAR_EPDIR);
    eptype = get_field(hcchar, HCCHAR_EPTYPE);
    mps = get_field(hcchar, HCCHAR_MPS);
    pid = get_field(hctsiz, TSIZ_SC_MC_PID);
    pcnt = get_field(hctsiz, TSIZ_PKTCNT);
    len = get_field(hctsiz, TSIZ_XFERSIZE);
    if (len > DWC2_MAX_XFER_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: HCTSIZ transfer size too large\n", __func__);
        return;
    }

    chan = index >> 3;
    p = &s->packet[chan];

    trace_usb_dwc2_handle_packet(chan, dev, &p->packet, epnum, types[eptype],
                                 dirs[epdir], mps, len, pcnt);

    if (mps == 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                "%s: Bad HCCHAR_MPS set to zero\n", __func__);
        return;
    }

    if (eptype == USB_ENDPOINT_XFER_CONTROL && pid == TSIZ_SC_MC_PID_SETUP) {
        pid = USB_TOKEN_SETUP;
    } else {
        pid = epdir ? USB_TOKEN_IN : USB_TOKEN_OUT;
    }

    /*
     * A slave-mode channel may stop here until its FIFO has enough data or
     * space.  Preserve the routing metadata before that wait so a later BH
     * resumes the same device and endpoint, including across migration.
     */
    p->devadr = devadr;
    p->epnum = epnum;
    p->epdir = epdir;
    p->mps = mps;
    p->pid = pid;
    p->index = index;
    p->pcnt = pcnt;
    p->len = len;

    if (send) {
        tlen = len;
        if (p->small || !dma) {
            if (tlen > mps) {
                tlen = mps;
            }
        }

        if (pid != USB_TOKEN_IN) {
            if (dma) {
                trace_usb_dwc2_memory_read(hcdma, tlen);
                if (dma_memory_read(&s->dma_as, hcdma, s->usb_buf[chan],
                                    tlen, MEMTXATTRS_UNSPECIFIED) !=
                                    MEMTX_OK) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "%s: dma_memory_read failed\n", __func__);
                }
            } else if (s->host_tx_fifo_count[chan] <
                       ROUND_UP(tlen, sizeof(uint32_t))) {
                s->host_pio_waiting[chan] = true;
                p->needs_service = false;
                return;
            } else {
                dwc2_host_tx_fifo_peek(s, chan, s->usb_buf[chan], tlen);
            }
        } else if (!dma && !dwc2_host_pio_rx_has_space(s, tlen)) {
            s->host_pio_waiting[chan] = true;
            p->needs_service = false;
            return;
        } else if (!dma) {
            dwc2_host_pio_rx_reserve(s, chan, tlen);
        }

        usb_packet_init(&p->packet);
        usb_packet_setup(&p->packet, pid, ep, 0, hcdma,
                         pid != USB_TOKEN_IN, true);
        usb_packet_addbuf(&p->packet, s->usb_buf[chan], tlen);
        p->async = DWC2_ASYNC_NONE;
        usb_handle_packet(dev, &p->packet);
    } else {
        tlen = p->len;
    }

    stsidx = -p->packet.status;
    assert(stsidx < sizeof(pstatus) / sizeof(*pstatus));
    actual = p->packet.actual_length;
    trace_usb_dwc2_packet_status(pstatus[stsidx], actual);

babble:
    if (p->packet.status != USB_RET_SUCCESS &&
            p->packet.status != USB_RET_NAK &&
            p->packet.status != USB_RET_STALL &&
            p->packet.status != USB_RET_ASYNC) {
        trace_usb_dwc2_packet_error(pstatus[stsidx]);
    }

    if (!dma && pid != USB_TOKEN_IN &&
        p->packet.status != USB_RET_ASYNC &&
        p->packet.status != USB_RET_NAK) {
        dwc2_host_tx_fifo_consume(s, chan, tlen);
    }

    if (p->packet.status == USB_RET_ASYNC) {
        trace_usb_dwc2_async_packet(&p->packet, chan, dev, epnum,
                                    dirs[epdir], tlen);
        usb_device_flush_ep_queue(dev, ep);
        assert(p->async != DWC2_ASYNC_INFLIGHT);
        p->devadr = devadr;
        p->epnum = epnum;
        p->epdir = epdir;
        p->mps = mps;
        p->pid = pid;
        p->index = index;
        p->pcnt = pcnt;
        p->len = tlen;
        p->async = DWC2_ASYNC_INFLIGHT;
        p->needs_service = false;
        return;
    }

    dwc2_host_pio_rx_release(s, chan);

    if (p->packet.status == USB_RET_SUCCESS) {
        if (actual > tlen) {
            p->packet.status = USB_RET_BABBLE;
            goto babble;
        }

        if (pid == USB_TOKEN_IN) {
            if (dma) {
                trace_usb_dwc2_memory_write(hcdma, actual);
                if (dma_memory_write(&s->dma_as, hcdma, s->usb_buf[chan],
                                     actual, MEMTXATTRS_UNSPECIFIED) !=
                                     MEMTX_OK) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "%s: dma_memory_write failed\n", __func__);
                }
            } else if (!dwc2_host_queue_rx_packet(s, chan,
                                                   get_field(hctsiz,
                                                             TSIZ_SC_MC_PID),
                                                   s->usb_buf[chan],
                                                   actual)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "%s: host Rx FIFO overflow\n", __func__);
                p->packet.status = USB_RET_IOERROR;
                stsidx = -p->packet.status;
                goto babble;
            }
        }

        tpcnt = actual / mps;
        if (actual % mps) {
            tpcnt++;
            if (pid == USB_TOKEN_IN) {
                done = true;
            }
        }

        pcnt -= tpcnt < pcnt ? tpcnt : pcnt;
        set_field(&hctsiz, pcnt, TSIZ_PKTCNT);
        len -= actual < len ? actual : len;
        set_field(&hctsiz, len, TSIZ_XFERSIZE);
        s->hreg1[index + 4] = hctsiz;
        if (dma) {
            hcdma += actual;
            s->hreg1[index + 5] = hcdma;
        }

        if (!pcnt || len == 0 || actual == 0) {
            done = true;
        }
    } else {
        intr |= pintr[stsidx];
        if (p->packet.status == USB_RET_NAK &&
            (eptype == USB_ENDPOINT_XFER_CONTROL ||
             eptype == USB_ENDPOINT_XFER_BULK)) {
            /*
             * for ctrl/bulk, automatically retry on NAK,
             * but send the interrupt anyway
             */
            intr &= ~HCINTMSK_RESERVED14_31;
            s->hreg1[index + 2] |= intr;
            do_intr = true;
        } else {
            intr |= HCINTMSK_CHHLTD;
            done = true;
        }
    }

    usb_packet_cleanup(&p->packet);

    if (done) {
        hcchar &= ~HCCHAR_CHENA;
        s->hreg1[index] = hcchar;
        if (!(intr & HCINTMSK_CHHLTD)) {
            intr |= HCINTMSK_CHHLTD | HCINTMSK_XFERCOMPL;
        }
        intr &= ~HCINTMSK_RESERVED14_31;
        s->hreg1[index + 2] |= intr;
        p->needs_service = false;
        s->host_pio_waiting[chan] = false;
        trace_usb_dwc2_packet_done(pstatus[stsidx], actual, len, pcnt);
        dwc2_update_hc_irq(s, index);
        return;
    }

    p->devadr = devadr;
    p->epnum = epnum;
    p->epdir = epdir;
    p->mps = mps;
    p->pid = pid;
    p->index = index;
    p->pcnt = pcnt;
    p->len = len;
    p->needs_service = true;
    trace_usb_dwc2_packet_next(pstatus[stsidx], len, pcnt);
    if (do_intr) {
        dwc2_update_hc_irq(s, index);
    }
}

/* Attach or detach a device on root hub */

static const char *speeds[] = {
    "low", "full", "high"
};

static void dwc2_attach(USBPort *port)
{
    DWC2State *s = port->opaque;
    int hispd = 0;

    trace_usb_dwc2_attach(port);
    assert(port->index == 0);

    if (!port->dev || !port->dev->attached) {
        return;
    }

    assert(port->dev->speed <= USB_SPEED_HIGH);
    trace_usb_dwc2_attach_speed(speeds[port->dev->speed]);
    s->hprt0 &= ~HPRT0_SPD_MASK;

    switch (port->dev->speed) {
    case USB_SPEED_LOW:
        s->hprt0 |= HPRT0_SPD_LOW_SPEED << HPRT0_SPD_SHIFT;
        break;
    case USB_SPEED_FULL:
        s->hprt0 |= HPRT0_SPD_FULL_SPEED << HPRT0_SPD_SHIFT;
        break;
    case USB_SPEED_HIGH:
        s->hprt0 |= HPRT0_SPD_HIGH_SPEED << HPRT0_SPD_SHIFT;
        hispd = 1;
        break;
    }

    if (hispd) {
        s->usb_frame_time = NANOSECONDS_PER_SECOND / 8000;        /* 125000 */
        if (NANOSECONDS_PER_SECOND >= USB_HZ_HS) {
            s->usb_bit_time = NANOSECONDS_PER_SECOND / USB_HZ_HS; /* 10.4 */
        } else {
            s->usb_bit_time = 1;
        }
    } else {
        s->usb_frame_time = NANOSECONDS_PER_SECOND / 1000;        /* 1000000 */
        if (NANOSECONDS_PER_SECOND >= USB_HZ_FS) {
            s->usb_bit_time = NANOSECONDS_PER_SECOND / USB_HZ_FS; /* 83.3 */
        } else {
            s->usb_bit_time = 1;
        }
    }

    s->fi = USB_FRMINTVL - 1;
    s->hprt0 |= HPRT0_CONNDET | HPRT0_CONNSTS;

    dwc2_bus_start(s);
    dwc2_raise_global_irq(s, GINTSTS_PRTINT);
}

static void dwc2_detach(USBPort *port)
{
    DWC2State *s = port->opaque;

    trace_usb_dwc2_detach(port);
    assert(port->index == 0);

    dwc2_bus_stop(s);

    s->hprt0 &= ~(HPRT0_SPD_MASK | HPRT0_SUSP | HPRT0_ENA | HPRT0_CONNSTS);
    s->hprt0 |= HPRT0_CONNDET | HPRT0_ENACHG;

    dwc2_raise_global_irq(s, GINTSTS_PRTINT);
}

static void dwc2_child_detach(USBPort *port, USBDevice *child)
{
    trace_usb_dwc2_child_detach(port, child);
    assert(port->index == 0);
}

static void dwc2_wakeup(USBPort *port)
{
    DWC2State *s = port->opaque;

    trace_usb_dwc2_wakeup(port);
    assert(port->index == 0);

    if (s->hprt0 & HPRT0_SUSP) {
        s->hprt0 |= HPRT0_RES;
        dwc2_raise_global_irq(s, GINTSTS_PRTINT);
    }

    qemu_bh_schedule(s->async_bh);
}

static void dwc2_async_packet_complete(USBPort *port, USBPacket *packet)
{
    DWC2State *s = port->opaque;
    DWC2Packet *p;
    USBDevice *dev;
    USBEndpoint *ep;

    assert(port->index == 0);
    p = container_of(packet, DWC2Packet, packet);
    assert(p->async == DWC2_ASYNC_INFLIGHT);
    if (packet->status == USB_RET_REMOVE_FROM_QUEUE) {
        usb_cancel_packet(packet);
        usb_packet_cleanup(packet);
        dwc2_host_pio_rx_release(s, p->index >> 3);
        return;
    }

    dev = dwc2_find_device(s, p->devadr);
    if (!dev) {
        packet->status = USB_RET_NODEV;
        dwc2_host_pio_rx_release(s, p->index >> 3);
        p->async = DWC2_ASYNC_FINISHED;
        qemu_bh_schedule(s->async_bh);
        return;
    }
    ep = usb_ep_get(dev, p->pid, p->epnum);
    trace_usb_dwc2_async_packet_complete(port, packet, p->index >> 3, dev,
                                         p->epnum, dirs[p->epdir], p->len);

    dwc2_handle_packet(s, p->devadr, dev, ep, p->index, false);

    p->async = DWC2_ASYNC_FINISHED;
    qemu_bh_schedule(s->async_bh);
}

static USBPortOps dwc2_port_ops = {
    .attach = dwc2_attach,
    .detach = dwc2_detach,
    .child_detach = dwc2_child_detach,
    .wakeup = dwc2_wakeup,
    .complete = dwc2_async_packet_complete,
};

static uint32_t dwc2_get_frame_remaining(DWC2State *s)
{
    uint32_t fr = 0;
    int64_t tks;

    tks = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->sof_time;
    if (tks < 0) {
        tks = 0;
    }

    /* avoid muldiv if possible */
    if (tks >= s->usb_frame_time) {
        goto out;
    }
    if (tks < s->usb_bit_time) {
        fr = s->fi;
        goto out;
    }

    /* tks = number of ns since SOF, divided by 83 (fs) or 10 (hs) */
    tks = tks / s->usb_bit_time;
    if (tks >= (int64_t)s->fi) {
        goto out;
    }

    /* remaining = frame interval minus tks */
    fr = (uint32_t)((int64_t)s->fi - tks);

out:
    return fr;
}

static void dwc2_work_bh(void *opaque)
{
    DWC2State *s = opaque;
    DWC2Packet *p;
    USBDevice *dev;
    USBEndpoint *ep;
    int64_t t_now, expire_time;
    int chan;
    bool found = false;

    trace_usb_dwc2_work_bh();
    if (s->working) {
        return;
    }
    s->working = true;

    t_now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    chan = s->next_chan;

    do {
        p = &s->packet[chan];
        if (p->needs_service) {
            dev = dwc2_find_device(s, p->devadr);
            if (!dev) {
                uint32_t index = chan << 3;

                s->hcchar(chan) &= ~HCCHAR_CHENA;
                s->hcint(chan) |= HCINTMSK_CHHLTD | HCINTMSK_XACTERR;
                p->needs_service = false;
                s->host_pio_waiting[chan] = false;
                dwc2_host_pio_rx_release(s, chan);
                dwc2_update_hc_irq(s, index);
                found = true;
            } else {
                ep = usb_ep_get(dev, p->pid, p->epnum);
                trace_usb_dwc2_work_bh_service(s->next_chan, chan, dev,
                                               p->epnum);
                dwc2_handle_packet(s, p->devadr, dev, ep, p->index, true);
                found = true;
            }
        }
        if (++chan == DWC2_NB_CHAN) {
            chan = 0;
        }
        if (found) {
            s->next_chan = chan;
            trace_usb_dwc2_work_bh_next(chan);
        }
    } while (chan != s->next_chan);

    if (found) {
        expire_time = t_now + NANOSECONDS_PER_SECOND / 4000;
        timer_mod(s->frame_timer, expire_time);
    }
    s->working = false;
}

static void dwc2_enable_chan(DWC2State *s,  uint32_t index)
{
    USBDevice *dev;
    USBEndpoint *ep;
    uint32_t hcchar;
    uint32_t hctsiz;
    uint32_t devadr, epnum, epdir, eptype, pid, len;
    DWC2Packet *p;

    assert((index >> 3) < DWC2_NB_CHAN);
    p = &s->packet[index >> 3];
    hcchar = s->hreg1[index];
    hctsiz = s->hreg1[index + 4];
    devadr = get_field(hcchar, HCCHAR_DEVADDR);
    epnum = get_field(hcchar, HCCHAR_EPNUM);
    epdir = get_bit(hcchar, HCCHAR_EPDIR);
    eptype = get_field(hcchar, HCCHAR_EPTYPE);
    pid = get_field(hctsiz, TSIZ_SC_MC_PID);
    len = get_field(hctsiz, TSIZ_XFERSIZE);

    dev = dwc2_find_device(s, devadr);

    trace_usb_dwc2_enable_chan(index >> 3, dev, &p->packet, epnum);
    if (dev == NULL) {
        return;
    }

    if (eptype == USB_ENDPOINT_XFER_CONTROL && pid == TSIZ_SC_MC_PID_SETUP) {
        pid = USB_TOKEN_SETUP;
    } else {
        pid = epdir ? USB_TOKEN_IN : USB_TOKEN_OUT;
    }

    ep = usb_ep_get(dev, pid, epnum);

    /*
     * Hack: Networking doesn't like us delivering large transfers, it kind
     * of works but the latency is horrible. So if the transfer is <= the mtu
     * size, we take that as a hint that this might be a network transfer,
     * and do the transfer packet-by-packet.
     */
    if (len > 1536) {
        p->small = false;
    } else {
        p->small = true;
    }

    dwc2_handle_packet(s, devadr, dev, ep, index, true);
    qemu_bh_schedule(s->async_bh);
}

static const char *glbregnm[] = {
    "GOTGCTL  ", "GOTGINT  ", "GAHBCFG  ", "GUSBCFG  ", "GRSTCTL  ",
    "GINTSTS  ", "GINTMSK  ", "GRXSTSR  ", "GRXSTSP  ", "GRXFSIZ  ",
    "GNPTXFSIZ", "GNPTXSTS ", "GI2CCTL  ", "GPVNDCTL ", "GGPIO    ",
    "GUID     ", "GSNPSID  ", "GHWCFG1  ", "GHWCFG2  ", "GHWCFG3  ",
    "GHWCFG4  ", "GLPMCFG  ", "GPWRDN   ", "GDFIFOCFG", "GADPCTL  ",
    "GREFCLK  ", "GINTMSK2 ", "GINTSTS2 "
};

static uint64_t dwc2_glbreg_read(void *ptr, hwaddr addr, int index,
                                 unsigned size)
{
    DWC2State *s = ptr;
    uint32_t val;

    if (addr > GINTSTS2) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return 0;
    }

    val = s->glbreg[index];

    switch (addr) {
    case GRSTCTL:
        /* clear any self-clearing bits that were set */
        val &= ~(GRSTCTL_TXFFLSH | GRSTCTL_RXFFLSH | GRSTCTL_IN_TKNQ_FLSH |
                 GRSTCTL_FRMCNTRRST | GRSTCTL_HSFTRST | GRSTCTL_CSFTRST);
        s->glbreg[index] = val;
        break;
    case GRXSTSR:
        dwc2_device_refresh_fifo_state(s);
        val = s->grxstsr;
        break;
    case GRXSTSP:
        val = dwc2_device_pop_rx_status(s);
        break;
    default:
        break;
    }

    trace_usb_dwc2_glbreg_read(addr, glbregnm[index], val);
    return val;
}

static void dwc2_glbreg_write(void *ptr, hwaddr addr, int index, uint64_t val,
                              unsigned size)
{
    DWC2State *s = ptr;
    uint64_t orig = val;
    uint32_t *mmio;
    uint32_t old;
    int iflg = 0;
    int mode = -1;

    if (addr > GINTSTS2) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return;
    }

    mmio = &s->glbreg[index];
    old = *mmio;

    switch (addr) {
    case GOTGCTL:
        /* don't allow setting of read-only bits */
        val &= ~(GOTGCTL_MULT_VALID_BC_MASK | GOTGCTL_BSESVLD |
                 GOTGCTL_ASESVLD | GOTGCTL_DBNC_SHORT | GOTGCTL_CONID_B |
                 GOTGCTL_HSTNEGSCS | GOTGCTL_SESREQSCS);
        /* don't allow clearing of read-only bits */
        val |= old & (GOTGCTL_MULT_VALID_BC_MASK | GOTGCTL_BSESVLD |
                      GOTGCTL_ASESVLD | GOTGCTL_DBNC_SHORT | GOTGCTL_CONID_B |
                      GOTGCTL_HSTNEGSCS | GOTGCTL_SESREQSCS);
        break;
    case GAHBCFG:
        if ((val & GAHBCFG_GLBL_INTR_EN) && !(old & GAHBCFG_GLBL_INTR_EN)) {
            iflg = 1;
        }
        break;
    case GUSBCFG:
        if ((val & GUSBCFG_FORCEDEVMODE) &&
            (val & GUSBCFG_FORCEHOSTMODE)) {
            val &= ~(GUSBCFG_FORCEDEVMODE | GUSBCFG_FORCEHOSTMODE);
            val |= old & (GUSBCFG_FORCEDEVMODE |
                          GUSBCFG_FORCEHOSTMODE);
        } else if (val & GUSBCFG_FORCEDEVMODE) {
            mode = 0;
        } else {
            mode = 1;
        }
        break;
    case GRSTCTL:
        val |= GRSTCTL_AHBIDLE;
        val &= ~GRSTCTL_DMAREQ;
        if (!(old & GRSTCTL_TXFFLSH) && (val & GRSTCTL_TXFFLSH)) {
            uint32_t fifo = (val & GRSTCTL_TXFNUM_MASK) >>
                            GRSTCTL_TXFNUM_SHIFT;

            if (s->gintsts & GINTSTS_CURMODE_HOST) {
                if (fifo == 0x10) {
                    dwc2_host_flush_tx_fifos(s, -1);
                } else {
                    dwc2_host_flush_tx_fifos(s, fifo != 0);
                }
            } else if (fifo == 0x10) {
                dwc2_device_flush_all_tx_fifos(s);
            } else {
                dwc2_device_flush_tx_fifo(s, fifo);
            }
            dwc2_device_refresh_fifo_state(s);
            if (s->gintsts & GINTSTS_CURMODE_HOST) {
                dwc2_update_irq(s);
            } else {
                dwc2_update_device_irq(s);
            }
        }
        if (!(old & GRSTCTL_RXFFLSH) && (val & GRSTCTL_RXFFLSH)) {
            dwc2_device_flush_rx_fifo(s);
            dwc2_host_pio_wake_waiters(s);
            if (s->gintsts & GINTSTS_CURMODE_HOST) {
                dwc2_update_irq(s);
            } else {
                dwc2_update_device_irq(s);
            }
        }
        if (!(old & GRSTCTL_IN_TKNQ_FLSH) && (val & GRSTCTL_IN_TKNQ_FLSH)) {
            s->dtknqr1 = 0;
            s->dtknqr2 = 0;
            s->dtknqr3 = 0;
        }
        if (!(old & GRSTCTL_FRMCNTRRST) && (val & GRSTCTL_FRMCNTRRST)) {
            dwc2_reset_frame_counter(s);
        }
        if (!(old & GRSTCTL_HSFTRST) && (val & GRSTCTL_HSFTRST)) {
            dwc2_host_state_machine_reset(s);
        }
        if (!(old & GRSTCTL_CSFTRST) && (val & GRSTCTL_CSFTRST)) {
            dwc2_core_state_machine_reset(s);
        }
        val &= ~(GRSTCTL_TXFFLSH | GRSTCTL_RXFFLSH |
                 GRSTCTL_IN_TKNQ_FLSH | GRSTCTL_FRMCNTRRST |
                 GRSTCTL_HSFTRST | GRSTCTL_CSFTRST);
        break;
    case GINTSTS:
        /* clear the write-1-to-clear bits */
        val |= ~old;
        val = ~val;
        /* don't allow clearing of read-only bits */
        val |= old & (GINTSTS_PTXFEMP | GINTSTS_HCHINT | GINTSTS_PRTINT |
                      GINTSTS_OEPINT | GINTSTS_IEPINT | GINTSTS_GOUTNAKEFF |
                      GINTSTS_GINNAKEFF | GINTSTS_NPTXFEMP | GINTSTS_RXFLVL |
                      GINTSTS_OTGINT | GINTSTS_CURMODE_HOST);
        iflg = 1;
        break;
    case GINTMSK:
        iflg = 1;
        break;
    case GRXSTSR:
    case GRXSTSP:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write to read-only register\n", __func__);
        return;
    default:
        break;
    }

    trace_usb_dwc2_glbreg_write(addr, glbregnm[index], orig, old, val);
    *mmio = val;
    if (addr == GRXFSIZ || addr == GNPTXFSIZ) {
        dwc2_device_refresh_fifo_state(s);
        dwc2_host_pio_wake_waiters(s);
    }

    if (mode >= 0 && mode != !!(s->gintsts & GINTSTS_CURMODE_HOST)) {
        if (mode) {
            s->gintsts |= GINTSTS_CURMODE_HOST;
            s->pcgctl &= ~PCGCTL_IF_DEV_MODE;
            dwc2_bus_start(s);
        } else {
            dwc2_bus_stop(s);
            s->gintsts &= ~GINTSTS_CURMODE_HOST;
            s->pcgctl |= PCGCTL_IF_DEV_MODE;
        }
        s->gintsts |= GINTSTS_CONIDSTSCHNG;
        dwc2_device_refresh_fifo_state(s);
        iflg = 1;
    }

    if (iflg) {
        dwc2_update_irq(s);
    }
}

static uint64_t dwc2_fszreg_read(void *ptr, hwaddr addr, int index,
                                 unsigned size)
{
    DWC2State *s = ptr;
    uint32_t val;

    if (addr != HPTXFSIZ) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return 0;
    }

    val = s->fszreg[index];

    trace_usb_dwc2_fszreg_read(addr, val);
    return val;
}

static void dwc2_fszreg_write(void *ptr, hwaddr addr, int index, uint64_t val,
                              unsigned size)
{
    DWC2State *s = ptr;
    uint64_t orig = val;
    uint32_t *mmio;
    uint32_t old;

    if (addr != HPTXFSIZ) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return;
    }

    mmio = &s->fszreg[index];
    old = *mmio;

    trace_usb_dwc2_fszreg_write(addr, orig, old, val);
    *mmio = val;
    dwc2_device_refresh_fifo_state(s);
    dwc2_host_pio_wake_waiters(s);
}

static uint64_t dwc2_dptxfsiz_read(void *ptr, hwaddr addr, int index,
                                   unsigned size)
{
    DWC2State *s = ptr;

    return s->dptxfsiz[index];
}

static void dwc2_dptxfsiz_write(void *ptr, hwaddr addr, int index,
                                uint64_t val, unsigned size)
{
    DWC2State *s = ptr;

    s->dptxfsiz[index] = val;
    dwc2_device_refresh_fifo_state(s);
}

static const char *hreg0nm[] = {
    "HCFG     ", "HFIR     ", "HFNUM    ", "<rsvd>   ", "HPTXSTS  ",
    "HAINT    ", "HAINTMSK ", "HFLBADDR ", "<rsvd>   ", "<rsvd>   ",
    "<rsvd>   ", "<rsvd>   ", "<rsvd>   ", "<rsvd>   ", "<rsvd>   ",
    "<rsvd>   ", "HPRT0    "
};

static uint64_t dwc2_hreg0_read(void *ptr, hwaddr addr, int index,
                                unsigned size)
{
    DWC2State *s = ptr;
    uint32_t val;

    if (addr < HCFG || addr > HPRT0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return 0;
    }

    val = s->hreg0[index];

    switch (addr) {
    case HFNUM:
        val = (dwc2_get_frame_remaining(s) << HFNUM_FRREM_SHIFT) |
              (s->hfnum << HFNUM_FRNUM_SHIFT);
        break;
    default:
        break;
    }

    trace_usb_dwc2_hreg0_read(addr, hreg0nm[index], val);
    return val;
}

static void dwc2_hreg0_write(void *ptr, hwaddr addr, int index, uint64_t val,
                             unsigned size)
{
    DWC2State *s = ptr;
    USBDevice *dev = s->uport.dev;
    uint64_t orig = val;
    uint32_t *mmio;
    uint32_t tval, told, old;
    int prst = 0;
    int iflg = 0;

    if (addr < HCFG || addr > HPRT0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return;
    }

    mmio = &s->hreg0[index];
    old = *mmio;

    switch (addr) {
    case HFIR:
        break;
    case HFNUM:
    case HPTXSTS:
    case HAINT:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write to read-only register\n",
                      __func__);
        return;
    case HAINTMSK:
        val &= 0xffff;
        break;
    case HPRT0:
        /* don't allow clearing of read-only bits */
        val |= old & (HPRT0_SPD_MASK | HPRT0_LNSTS_MASK | HPRT0_OVRCURRACT |
                      HPRT0_CONNSTS);
        /* don't allow clearing of self-clearing bits */
        val |= old & (HPRT0_SUSP | HPRT0_RES);
        /* don't allow setting of self-setting bits */
        if (!(old & HPRT0_ENA) && (val & HPRT0_ENA)) {
            val &= ~HPRT0_ENA;
        }
        /* clear the write-1-to-clear bits */
        tval = val & (HPRT0_OVRCURRCHG | HPRT0_ENACHG | HPRT0_ENA |
                      HPRT0_CONNDET);
        told = old & (HPRT0_OVRCURRCHG | HPRT0_ENACHG | HPRT0_ENA |
                      HPRT0_CONNDET);
        tval |= ~told;
        tval = ~tval;
        tval &= (HPRT0_OVRCURRCHG | HPRT0_ENACHG | HPRT0_ENA |
                 HPRT0_CONNDET);
        val &= ~(HPRT0_OVRCURRCHG | HPRT0_ENACHG | HPRT0_ENA |
                 HPRT0_CONNDET);
        val |= tval;
        if (!(val & HPRT0_RST) && (old & HPRT0_RST)) {
            if (dev && dev->attached) {
                val |= HPRT0_ENA | HPRT0_ENACHG;
                prst = 1;
            }
        }
        if (val & (HPRT0_OVRCURRCHG | HPRT0_ENACHG | HPRT0_CONNDET)) {
            iflg = 1;
        } else {
            iflg = -1;
        }
        break;
    default:
        break;
    }

    if (prst) {
        trace_usb_dwc2_hreg0_write(addr, hreg0nm[index], orig, old,
                                   val & ~HPRT0_CONNDET);
        trace_usb_dwc2_hreg0_action("call usb_port_reset");
        usb_port_reset(&s->uport);
        val &= ~HPRT0_CONNDET;
    } else {
        trace_usb_dwc2_hreg0_write(addr, hreg0nm[index], orig, old, val);
    }

    *mmio = val;

    if (iflg > 0) {
        trace_usb_dwc2_hreg0_action("enable PRTINT");
        dwc2_raise_global_irq(s, GINTSTS_PRTINT);
    } else if (iflg < 0) {
        trace_usb_dwc2_hreg0_action("disable PRTINT");
        dwc2_lower_global_irq(s, GINTSTS_PRTINT);
    }
}

bool dwc2_host_firmware_reset_port(DWC2State *s, Error **errp)
{
    uint32_t writable;

    if (!s->uport.dev || !s->uport.dev->attached ||
        s->uport.dev->state == USB_STATE_NOTATTACHED ||
        !(s->hprt0 & HPRT0_CONNSTS)) {
        error_setg(errp, "DWC2 firmware host has no attached root device");
        return false;
    }
    writable = s->hprt0 &
        ~(HPRT0_CONNDET | HPRT0_ENACHG | HPRT0_OVRCURRCHG);
    dwc2_hreg0_write(s, HPRT0, (HPRT0 - HCFG) >> 2,
                     writable | HPRT0_PWR | HPRT0_RST, 4);
    dwc2_hreg0_write(s, HPRT0, (HPRT0 - HCFG) >> 2,
                     writable | HPRT0_PWR, 4);
    if (!(s->hprt0 & HPRT0_ENA)) {
        error_setg(errp, "DWC2 firmware host root-port reset failed");
        return false;
    }
    return true;
}

ssize_t dwc2_host_firmware_transfer(
    DWC2State *s, uint8_t address, uint8_t endpoint, uint8_t type,
    uint16_t max_packet, bool in, uint32_t pid, uint32_t dma,
    void *buffer, size_t length, Error **errp)
{
    const unsigned int channel = 0;
    const unsigned int index = channel << 3;
    uint32_t hcchar;
    uint32_t hctsiz;

    if (address > 127 || endpoint > 15 ||
        type > USB_ENDPOINT_XFER_INT || !max_packet ||
        max_packet > HCCHAR_MPS_MASK || length > DWC2_MAX_XFER_SIZE ||
        pid > TSIZ_SC_MC_PID_SETUP) {
        error_setg(errp, "invalid DWC2 firmware host transfer");
        return -1;
    }
    if (!(s->hprt0 & HPRT0_ENA)) {
        error_setg(errp, "DWC2 firmware host root port is disabled");
        return -1;
    }
    if (s->packet[channel].async == DWC2_ASYNC_INFLIGHT) {
        error_setg(errp, "DWC2 firmware host channel is busy");
        return -1;
    }
    s->gahbcfg |= GAHBCFG_DMA_EN;
    if (!in && length &&
        dma_memory_write(&s->dma_as, dma, buffer, length,
                         MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        error_setg(errp, "DWC2 firmware host DMA write failed");
        return -1;
    }

    hcchar = HCCHAR_CHENA |
             ((uint32_t)address << HCCHAR_DEVADDR_SHIFT) |
             ((uint32_t)endpoint << HCCHAR_EPNUM_SHIFT) |
             ((uint32_t)type << HCCHAR_EPTYPE_SHIFT) |
             (in ? HCCHAR_EPDIR : 0) | max_packet;
    hctsiz = (pid << TSIZ_SC_MC_PID_SHIFT) |
             (MAX(1, DIV_ROUND_UP(length, max_packet)) <<
              TSIZ_PKTCNT_SHIFT) | length;
    s->hcint(channel) = 0;
    s->hctsiz(channel) = hctsiz;
    s->hcdma(channel) = dma;
    s->hcchar(channel) = hcchar;
    dwc2_enable_chan(s, index);

    for (unsigned int attempt = 0; attempt < 10000; attempt++) {
        uint32_t status = s->hcint(channel);

        if (status & HCINTMSK_XFERCOMPL) {
            size_t remaining =
                s->hctsiz(channel) & TSIZ_XFERSIZE_MASK;
            size_t actual = length - MIN(length, remaining);

            if (in && actual &&
                dma_memory_read(&s->dma_as, dma, buffer, actual,
                                MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
                error_setg(errp, "DWC2 firmware host DMA read failed");
                return -1;
            }
            return actual;
        }
        if (status & (HCINTMSK_STALL | HCINTMSK_BBLERR |
                      HCINTMSK_XACTERR | HCINTMSK_AHBERR)) {
            error_setg(errp,
                       "DWC2 firmware host transfer failed (HCINT=0x%08x)",
                       status);
            return -1;
        }
        aio_poll(qemu_get_aio_context(), false);
        dwc2_work_bh(s);
        if (s->packet[channel].async == DWC2_ASYNC_INFLIGHT) {
            g_usleep(100);
        }
    }
    error_setg(errp, "DWC2 firmware host transfer timed out");
    return -1;
}

static const char *hreg1nm[] = {
    "HCCHAR  ", "HCSPLT  ", "HCINT   ", "HCINTMSK", "HCTSIZ  ", "HCDMA   ",
    "<rsvd>  ", "HCDMAB  "
};

static uint64_t dwc2_hreg1_read(void *ptr, hwaddr addr, int index,
                                unsigned size)
{
    DWC2State *s = ptr;
    uint32_t val;

    if (addr < HCCHAR(0) || addr > HCDMAB(DWC2_NB_CHAN - 1)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return 0;
    }

    val = s->hreg1[index];

    trace_usb_dwc2_hreg1_read(addr, hreg1nm[index & 7], addr >> 5, val);
    return val;
}

static void dwc2_hreg1_write(void *ptr, hwaddr addr, int index, uint64_t val,
                             unsigned size)
{
    DWC2State *s = ptr;
    uint64_t orig = val;
    uint32_t *mmio;
    uint32_t old;
    int iflg = 0;
    int enflg = 0;
    int disflg = 0;

    if (addr < HCCHAR(0) || addr > HCDMAB(DWC2_NB_CHAN - 1)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return;
    }

    mmio = &s->hreg1[index];
    old = *mmio;

    switch (HSOTG_REG(0x500) + (addr & 0x1c)) {
    case HCCHAR(0):
        if ((val & HCCHAR_CHDIS) && !(old & HCCHAR_CHDIS)) {
            val &= ~(HCCHAR_CHENA | HCCHAR_CHDIS);
            disflg = 1;
        } else {
            val |= old & HCCHAR_CHDIS;
            if ((val & HCCHAR_CHENA) && !(old & HCCHAR_CHENA)) {
                val &= ~HCCHAR_CHDIS;
                enflg = 1;
            } else {
                val |= old & HCCHAR_CHENA;
            }
        }
        break;
    case HCINT(0):
        /* clear the write-1-to-clear bits */
        val |= ~old;
        val = ~val;
        val &= ~HCINTMSK_RESERVED14_31;
        iflg = 1;
        break;
    case HCINTMSK(0):
        val &= ~HCINTMSK_RESERVED14_31;
        iflg = 1;
        break;
    case HCDMAB(0):
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write to read-only register\n",
                      __func__);
        return;
    default:
        break;
    }

    trace_usb_dwc2_hreg1_write(addr, hreg1nm[index & 7], index >> 3, orig,
                               old, val);
    *mmio = val;

    if (disflg) {
        unsigned int channel = index >> 3;
        DWC2Packet *packet = &s->packet[channel];

        if (packet->async == DWC2_ASYNC_INFLIGHT) {
            usb_cancel_packet(&packet->packet);
            usb_packet_cleanup(&packet->packet);
            packet->async = DWC2_ASYNC_NONE;
            packet->needs_service = false;
        }
        dwc2_host_pio_rx_release(s, channel);
        s->host_tx_fifo_head[channel] = 0;
        s->host_tx_fifo_count[channel] = 0;
        s->host_pio_waiting[channel] = false;
        dwc2_device_refresh_fifo_state(s);
        /* set ChHltd in HCINT */
        s->hreg1[(index & ~7) + 2] |= HCINTMSK_CHHLTD;
        iflg = 1;
    }

    if (enflg) {
        dwc2_enable_chan(s, index & ~7);
    }

    if (iflg) {
        dwc2_update_hc_irq(s, index & ~7);
    }
}

bool dwc2_host_firmware_abort_transfer(DWC2State *s, Error **errp)
{
    const unsigned int channel = 0;
    const unsigned int index = channel << 3;

    if (s->packet[channel].async != DWC2_ASYNC_INFLIGHT) {
        return true;
    }
    dwc2_hreg1_write(s, HCCHAR(channel), index,
                     s->hcchar(channel) | HCCHAR_CHENA | HCCHAR_CHDIS, 4);
    if (s->packet[channel].async == DWC2_ASYNC_INFLIGHT) {
        error_setg(errp, "DWC2 firmware host channel abort failed");
        return false;
    }
    return true;
}

static void dwc2_update_device_irq(DWC2State *s)
{
    uint32_t daint = 0;

    dwc2_device_refresh_fifo_state(s);
    for (unsigned int ep = 0; ep < DWC2_DEV_EP_COUNT; ep++) {
        if (!s->tx_fifo_count[ep] && (s->diepepmsk & BIT(ep))) {
            s->diepint(ep) |= DXEPINT_TXFEMP;
        } else {
            s->diepint(ep) &= ~DXEPINT_TXFEMP;
        }
        if (s->diepint(ep) & s->diepmsk) {
            daint |= DAINT_INEP(ep);
        }
        if (s->doepint(ep) & s->doepmsk) {
            daint |= DAINT_OUTEP(ep);
        }
    }
    s->daint = daint;
    if (daint & s->daintmsk & 0xffff) {
        s->gintsts |= GINTSTS_IEPINT;
    } else {
        s->gintsts &= ~GINTSTS_IEPINT;
    }
    if (daint & s->daintmsk & 0xffff0000) {
        s->gintsts |= GINTSTS_OEPINT;
    } else {
        s->gintsts &= ~GINTSTS_OEPINT;
    }
    dwc2_update_irq(s);
}

static bool dwc2_device_available(DWC2State *s)
{
    return !(s->gintsts & GINTSTS_CURMODE_HOST) &&
           s->device_connected && !(s->dctl & DCTL_SFTDISCON);
}

static uint32_t dwc2_device_ep_mps(uint32_t ctl, unsigned int ep)
{
    static const uint8_t ep0_mps[] = { 64, 32, 16, 8 };

    if (ep == 0) {
        return ep0_mps[ctl & D0EPCTL_MPS_MASK];
    }
    return ctl & DXEPCTL_MPS_MASK;
}

static void dwc2_device_complete(DWC2State *s, unsigned int ep, bool in,
                                 size_t actual, bool setup, bool advance_dma)
{
    uint32_t *ctl = in ? &s->diepctl(ep) : &s->doepctl(ep);
    uint32_t *intr = in ? &s->diepint(ep) : &s->doepint(ep);
    uint32_t *size = in ? &s->dieptsiz(ep) : &s->doeptsiz(ep);
    uint32_t *dma = in ? &s->diepdma(ep) : &s->doepdma(ep);
    uint32_t remaining = DXEPTSIZ_XFERSIZE_GET(*size);
    uint32_t packets = DXEPTSIZ_PKTCNT_GET(*size);
    uint32_t mps = dwc2_device_ep_mps(*ctl, ep);
    bool setup_pending = false;

    remaining -= MIN(remaining, actual);
    packets -= packets != 0;
    set_field(size, remaining, DXEPTSIZ_XFERSIZE);
    set_field(size, packets, DXEPTSIZ_PKTCNT);
    if (setup && ep == 0 && !in) {
        uint32_t setup_packets = get_field(*size, DOEPTSIZ0_SUPCNT);

        setup_packets -= setup_packets != 0;
        set_field(size, setup_packets, DOEPTSIZ0_SUPCNT);
        setup_pending = setup_packets != 0 && remaining >= 8;
    }
    if (advance_dma) {
        *dma += actual;
    }

    if (setup || remaining == 0 || actual < mps || packets == 0) {
        if (!setup_pending) {
            *ctl &= ~DXEPCTL_EPENA;
        }
        *intr |= DXEPINT_XFERCOMPL;
    }
    if (setup) {
        *intr |= DXEPINT_SETUP | DXEPINT_SETUP_RCVD;
    }
    trace_usb_dwc2_device_packet(ep, in, actual, remaining, packets, setup);
    dwc2_update_device_irq(s);
}

static void dwc2_device_dma_error(DWC2State *s, unsigned int ep, bool in)
{
    uint32_t *ctl = in ? &s->diepctl(ep) : &s->doepctl(ep);
    uint32_t *intr = in ? &s->diepint(ep) : &s->doepint(ep);

    *ctl &= ~DXEPCTL_EPENA;
    *intr |= DXEPINT_AHBERR;
    dwc2_update_device_irq(s);
}

void dwc2_device_host_connect(DWC2State *s, bool connected)
{
    if (s->device_connected == connected) {
        return;
    }
    s->device_connected = connected;
    trace_usb_dwc2_device_connect(connected);
    if (!connected) {
        s->dsts &= ~DSTS_SUSPSTS;
        s->gintsts &= ~(GINTSTS_USBSUSP | GINTSTS_WKUPINT);
        s->gintsts |= GINTSTS_DISCONNINT;
        dwc2_update_irq(s);
    }
    if (s->device_event_handler) {
        s->device_event_handler(
            s->device_handler_opaque,
            connected ? DWC2_DEVICE_EVENT_CONNECT :
                        DWC2_DEVICE_EVENT_DISCONNECT, 0);
    }
}

void dwc2_device_host_reset(DWC2State *s)
{
    if (!dwc2_device_available(s)) {
        return;
    }

    s->dcfg &= ~DCFG_DEVADDR_MASK;
    s->dctl &= ~(DCTL_GOUTNAKSTS | DCTL_GNPINNAKSTS);
    s->dsts &= ~DSTS_SUSPSTS;
    /*
     * A USB bus reset does not reset the DWC2 programming interface.  Keep
     * endpoint activation, transfer size, and DMA addresses intact so the
     * EP0 receive request armed before first enumeration remains usable.
     * Linux explicitly tears down and reprograms endpoints after a reset of
     * an already-addressed device.
     */
    for (unsigned int ep = 0; ep < DWC2_DEV_EP_COUNT; ep++) {
        s->diepint(ep) = 0;
        s->doepint(ep) = 0;
    }
    dwc2_device_flush_fifos(s);
    s->daint = 0;
    s->gintsts &= ~(GINTSTS_IEPINT | GINTSTS_OEPINT | GINTSTS_ENUMDONE |
                    GINTSTS_USBSUSP | GINTSTS_WKUPINT);
    s->gintsts |= GINTSTS_USBRST;
    trace_usb_dwc2_device_reset();
    dwc2_update_irq(s);
    if (s->device_event_handler) {
        s->device_event_handler(s->device_handler_opaque,
                                DWC2_DEVICE_EVENT_RESET, 0);
    }
}

void dwc2_device_host_enum_done(DWC2State *s, unsigned int speed)
{
    if (!dwc2_device_available(s) || speed > DSTS_ENUMSPD_FS48) {
        return;
    }

    s->dsts &= ~(DSTS_ENUMSPD_MASK | DSTS_SUSPSTS);
    s->dsts |= speed << DSTS_ENUMSPD_SHIFT;
    s->gintsts |= GINTSTS_ENUMDONE;
    trace_usb_dwc2_device_enum_done(speed);
    dwc2_update_irq(s);
    if (s->device_event_handler) {
        s->device_event_handler(s->device_handler_opaque,
                                DWC2_DEVICE_EVENT_ENUM_DONE, speed);
    }
}

void dwc2_device_host_suspend(DWC2State *s, bool suspended)
{
    unsigned int event;

    if (!dwc2_device_available(s) ||
        suspended == !!(s->dsts & DSTS_SUSPSTS)) {
        return;
    }

    if (suspended) {
        s->dsts |= DSTS_SUSPSTS;
        s->gintsts |= GINTSTS_USBSUSP;
        event = DWC2_DEVICE_EVENT_SUSPEND;
    } else {
        s->dsts &= ~DSTS_SUSPSTS;
        s->gintsts |= GINTSTS_WKUPINT;
        event = DWC2_DEVICE_EVENT_RESUME;
    }
    dwc2_update_irq(s);
    if (s->device_event_handler) {
        s->device_event_handler(s->device_handler_opaque, event, 0);
    }
}

ssize_t dwc2_device_host_send(DWC2State *s, unsigned int ep,
                              const void *buf, size_t len, bool setup)
{
    uint32_t ctl, size, mps, remaining;
    uint32_t packets;
    bool dma, complete;

    if (!dwc2_device_available(s) || ep >= DWC2_DEV_EP_COUNT) {
        return -ENODEV;
    }
    if (s->dsts & DSTS_SUSPSTS) {
        return -EAGAIN;
    }
    ctl = s->doepctl(ep);
    if (ctl & DXEPCTL_STALL) {
        return -EPIPE;
    }
    if (!(ctl & DXEPCTL_EPENA) || !(ctl & DXEPCTL_USBACTEP) ||
        (ctl & DXEPCTL_NAKSTS) || (s->dctl & DCTL_GOUTNAKSTS)) {
        return -EAGAIN;
    }
    if (setup && (ep != 0 || len != 8)) {
        return -EINVAL;
    }

    size = s->doeptsiz(ep);
    remaining = DXEPTSIZ_XFERSIZE_GET(size);
    packets = DXEPTSIZ_PKTCNT_GET(size);
    mps = dwc2_device_ep_mps(ctl, ep);
    if (mps == 0 || len > mps || len > remaining) {
        return -EMSGSIZE;
    }
    dma = s->gahbcfg & GAHBCFG_DMA_EN;
    complete = remaining == len || len < mps || packets <= 1;
    if (dma) {
        if (len && dma_memory_write(&s->dma_as, s->doepdma(ep), buf, len,
                                    MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            dwc2_device_dma_error(s, ep, false);
            return -EIO;
        }
    } else if (!dwc2_device_queue_rx_packet(s, ep, buf, len, setup,
                                             complete)) {
        return -ENOSPC;
    }

    dwc2_device_complete(s, ep, false, len, setup, dma);
    if (s->device_packet_handler) {
        s->device_packet_handler(s->device_handler_opaque, ep, false, setup,
                                 buf, len,
                                 !(s->doepctl(ep) & DXEPCTL_EPENA));
    }
    return len;
}

ssize_t dwc2_device_host_receive(DWC2State *s, unsigned int ep,
                                 void *buf, size_t len)
{
    uint32_t ctl, size, mps, remaining;
    size_t actual;
    bool dma;

    if (!dwc2_device_available(s) || ep >= DWC2_DEV_EP_COUNT) {
        return -ENODEV;
    }
    if (s->dsts & DSTS_SUSPSTS) {
        return -EAGAIN;
    }
    ctl = s->diepctl(ep);
    if (ctl & DXEPCTL_STALL) {
        return -EPIPE;
    }
    if (!(ctl & DXEPCTL_EPENA) || !(ctl & DXEPCTL_USBACTEP) ||
        (ctl & DXEPCTL_NAKSTS) || (s->dctl & DCTL_GNPINNAKSTS)) {
        return -EAGAIN;
    }

    size = s->dieptsiz(ep);
    remaining = DXEPTSIZ_XFERSIZE_GET(size);
    mps = dwc2_device_ep_mps(ctl, ep);
    if (mps == 0) {
        return -EINVAL;
    }
    dma = s->gahbcfg & GAHBCFG_DMA_EN;
    actual = MIN(MIN((size_t)remaining, (size_t)mps), len);
    if (dma) {
        if (actual && dma_memory_read(&s->dma_as, s->diepdma(ep), buf,
                                     actual, MEMTXATTRS_UNSPECIFIED) !=
                                     MEMTX_OK) {
            dwc2_device_dma_error(s, ep, true);
            return -EIO;
        }
    } else {
        if (actual && s->tx_fifo_count[ep] < actual) {
            return -EAGAIN;
        }
        dwc2_device_tx_fifo_read(s, ep, buf, actual);
    }

    dwc2_device_complete(s, ep, true, actual, false, dma);
    if (!dma && !(s->diepctl(ep) & DXEPCTL_EPENA)) {
        dwc2_device_flush_tx_fifo(s, ep);
        dwc2_update_device_irq(s);
    }
    if (s->device_packet_handler) {
        s->device_packet_handler(s->device_handler_opaque, ep, true, false,
                                 buf, actual,
                                 !(s->diepctl(ep) & DXEPCTL_EPENA));
    }
    return actual;
}

void dwc2_device_set_firmware_handlers(DWC2State *s,
                                       DWC2DevicePacketHandler packet,
                                       DWC2DeviceEventHandler event,
                                       void *opaque)
{
    s->device_packet_handler = packet;
    s->device_event_handler = event;
    s->device_handler_opaque = opaque;
}

void dwc2_device_firmware_start(DWC2State *s)
{
    dwc2_bus_stop(s);
    s->gusbcfg &= ~GUSBCFG_FORCEHOSTMODE;
    s->gusbcfg |= GUSBCFG_FORCEDEVMODE;
    s->gintsts &= ~GINTSTS_CURMODE_HOST;
    s->pcgctl |= PCGCTL_IF_DEV_MODE;
    s->gahbcfg |= GAHBCFG_DMA_EN;
    s->dcfg = DCFG_DEVSPD_HS;
    s->diepmsk = DIEPMSK_XFERCOMPLMSK | DIEPMSK_AHBERRMSK |
                 DIEPMSK_EPDISBLDMSK;
    s->doepmsk = DOEPMSK_XFERCOMPLMSK | DOEPMSK_SETUPMSK |
                 DOEPMSK_AHBERRMSK | DOEPMSK_EPDISBLDMSK;
    s->daintmsk = UINT32_MAX;
    dwc2_update_device_irq(s);
}

static int dwc2_device_firmware_ctl(uint32_t *ctl, unsigned int ep,
                                    uint32_t mps, uint32_t type)
{
    if (ep == 0) {
        switch (mps) {
        case 64:
            mps = D0EPCTL_MPS_64;
            break;
        case 32:
            mps = D0EPCTL_MPS_32;
            break;
        case 16:
            mps = D0EPCTL_MPS_16;
            break;
        case 8:
            mps = D0EPCTL_MPS_8;
            break;
        default:
            return -EINVAL;
        }
        type = DXEPCTL_EPTYPE_CONTROL;
    } else if (!mps || mps > DXEPCTL_MPS_LIMIT ||
               (type & ~DXEPCTL_EPTYPE_MASK)) {
        return -EINVAL;
    }

    *ctl = DXEPCTL_EPENA | DXEPCTL_USBACTEP | type | mps;
    return 0;
}

int dwc2_device_firmware_arm_out(DWC2State *s, unsigned int ep,
                                 uint32_t dma, size_t length, uint32_t mps,
                                 uint32_t type, bool setup)
{
    uint32_t packets;
    int error;

    if (ep >= DWC2_DEV_EP_COUNT || length > DXEPTSIZ_XFERSIZE_LIMIT ||
        (setup && (ep != 0 || length != 8))) {
        return -EINVAL;
    }
    error = dwc2_device_firmware_ctl(&s->doepctl(ep), ep, mps, type);
    if (error) {
        return error;
    }
    packets = MAX(1U, DIV_ROUND_UP(length, mps));
    if (packets > DXEPTSIZ_PKTCNT_LIMIT) {
        s->doepctl(ep) = 0;
        return -E2BIG;
    }
    s->doepdma(ep) = dma;
    s->doeptsiz(ep) = DXEPTSIZ_PKTCNT(packets) |
                       DXEPTSIZ_XFERSIZE(length);
    if (setup) {
        s->doeptsiz(ep) |= DOEPTSIZ0_SUPCNT(1);
    }
    return 0;
}

int dwc2_device_firmware_arm_in(DWC2State *s, unsigned int ep,
                                uint32_t dma, const void *data, size_t length,
                                uint32_t mps, uint32_t type)
{
    uint32_t packets;
    int error;

    if (ep >= DWC2_DEV_EP_COUNT || length > DXEPTSIZ_XFERSIZE_LIMIT) {
        return -EINVAL;
    }
    error = dwc2_device_firmware_ctl(&s->diepctl(ep), ep, mps, type);
    if (error) {
        return error;
    }
    packets = MAX(1U, DIV_ROUND_UP(length, mps));
    if (packets > DXEPTSIZ_PKTCNT_LIMIT) {
        s->diepctl(ep) = 0;
        return -E2BIG;
    }
    if (length && dma_memory_write(&s->dma_as, dma, data, length,
                                   MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        s->diepctl(ep) = 0;
        return -EIO;
    }
    s->diepdma(ep) = dma;
    s->dieptsiz(ep) = DXEPTSIZ_PKTCNT(packets) |
                      DXEPTSIZ_XFERSIZE(length);
    return 0;
}

void dwc2_device_firmware_disable_endpoint(DWC2State *s, unsigned int ep)
{
    if (ep >= DWC2_DEV_EP_COUNT) {
        return;
    }

    s->diepctl(ep) = 0;
    s->diepint(ep) = 0;
    s->dieptsiz(ep) = 0;
    s->diepdma(ep) = 0;
    s->doepctl(ep) = 0;
    s->doepint(ep) = 0;
    s->doeptsiz(ep) = 0;
    s->doepdma(ep) = 0;
    dwc2_device_flush_tx_fifo(s, ep);
    dwc2_update_device_irq(s);
}

static void dwc2_device_transport_reply(DWC2State *s, uint8_t opcode,
                                        uint8_t ep, int32_t result,
                                        const void *payload, uint32_t length)
{
    uint8_t header[DWC2_DEVICE_TRANSPORT_HEADER_SIZE] = { 0 };

    stl_le_p(header, DWC2_DEVICE_TRANSPORT_MAGIC);
    header[4] = DWC2_DEVICE_TRANSPORT_VERSION;
    header[5] = opcode | DWC2_DEVICE_TRANSPORT_RESPONSE;
    header[6] = ep;
    stl_le_p(header + 8, length);
    stl_le_p(header + 12, result);
    qemu_chr_fe_write_all(&s->device_chr, header, sizeof(header));
    if (length) {
        qemu_chr_fe_write_all(&s->device_chr, payload, length);
    }
}

static void dwc2_device_transport_request(DWC2State *s,
                                          const uint8_t *header,
                                          const uint8_t *payload)
{
    uint8_t opcode = header[5];
    uint8_t ep = header[6];
    uint32_t length = ldl_le_p(header + 8);
    uint32_t value = ldl_le_p(header + 12);
    ssize_t result = 0;
    uint32_t reply_length = 0;

    if (header[7] != 0 || (opcode & DWC2_DEVICE_TRANSPORT_RESPONSE)) {
        result = -EPROTO;
        goto reply;
    }

    switch (opcode) {
    case DWC2_DEVICE_TRANSPORT_CONNECT:
        if (length) {
            result = -EINVAL;
            break;
        }
        dwc2_device_host_connect(s, true);
        break;
    case DWC2_DEVICE_TRANSPORT_DISCONNECT:
        if (length) {
            result = -EINVAL;
            break;
        }
        dwc2_device_host_connect(s, false);
        break;
    case DWC2_DEVICE_TRANSPORT_RESET:
        if (length) {
            result = -EINVAL;
            break;
        }
        if (!dwc2_device_available(s)) {
            result = -ENODEV;
            break;
        }
        dwc2_device_host_reset(s);
        break;
    case DWC2_DEVICE_TRANSPORT_ENUM_DONE:
        if (length || value > DSTS_ENUMSPD_FS48) {
            result = -EINVAL;
            break;
        }
        if (!dwc2_device_available(s)) {
            result = -ENODEV;
            break;
        }
        dwc2_device_host_enum_done(s, value);
        break;
    case DWC2_DEVICE_TRANSPORT_SUSPEND:
    case DWC2_DEVICE_TRANSPORT_RESUME:
        if (length) {
            result = -EINVAL;
            break;
        }
        if (!dwc2_device_available(s)) {
            result = -ENODEV;
            break;
        }
        dwc2_device_host_suspend(
            s, opcode == DWC2_DEVICE_TRANSPORT_SUSPEND);
        break;
    case DWC2_DEVICE_TRANSPORT_STATE:
        if (length || ep || value) {
            result = -EINVAL;
            break;
        }
        result = 0;
        if (!(s->gintsts & GINTSTS_CURMODE_HOST) &&
            !(s->dctl & DCTL_SFTDISCON)) {
            result |= DWC2_DEVICE_TRANSPORT_STATE_PULLUP;
        }
        if ((s->doepctl(0) &
             (DXEPCTL_EPENA | DXEPCTL_USBACTEP)) ==
                (DXEPCTL_EPENA | DXEPCTL_USBACTEP) &&
            DXEPTSIZ_XFERSIZE_GET(s->doeptsiz(0)) >= 8 &&
            DXEPTSIZ_PKTCNT_GET(s->doeptsiz(0)) &&
            !(s->gintsts & (GINTSTS_USBRST | GINTSTS_ENUMDONE))) {
            result |= DWC2_DEVICE_TRANSPORT_STATE_EP0_SETUP;
        }
        break;
    case DWC2_DEVICE_TRANSPORT_OUT:
    case DWC2_DEVICE_TRANSPORT_SETUP:
        result = dwc2_device_host_send(
            s, ep, payload, length,
            opcode == DWC2_DEVICE_TRANSPORT_SETUP);
        break;
    case DWC2_DEVICE_TRANSPORT_IN:
        if (length || value > DWC2_DEVICE_TRANSPORT_MAX_PAYLOAD) {
            result = -EINVAL;
            break;
        }
        result = dwc2_device_host_receive(s, ep, s->device_tx_buf, value);
        if (result >= 0) {
            reply_length = result;
        }
        break;
    default:
        result = -ENOTSUP;
        break;
    }

reply:
    dwc2_device_transport_reply(s, opcode, ep, result, s->device_tx_buf,
                                reply_length);
}

static int dwc2_device_transport_can_read(void *opaque)
{
    DWC2State *s = opaque;

    return sizeof(s->device_rx_buf) - s->device_rx_used;
}

static void dwc2_device_transport_read(void *opaque, const uint8_t *buf,
                                       int size)
{
    DWC2State *s = opaque;

    memcpy(s->device_rx_buf + s->device_rx_used, buf, size);
    s->device_rx_used += size;

    while (s->device_rx_used >= DWC2_DEVICE_TRANSPORT_HEADER_SIZE) {
        uint8_t *header = s->device_rx_buf;
        uint32_t length;
        size_t frame_size;

        if (ldl_le_p(header) != DWC2_DEVICE_TRANSPORT_MAGIC ||
            header[4] != DWC2_DEVICE_TRANSPORT_VERSION) {
            dwc2_device_transport_reply(s, header[5], header[6], -EPROTO,
                                        NULL, 0);
            s->device_rx_used = 0;
            return;
        }
        length = ldl_le_p(header + 8);
        if (length > DWC2_DEVICE_TRANSPORT_MAX_PAYLOAD) {
            dwc2_device_transport_reply(s, header[5], header[6], -EMSGSIZE,
                                        NULL, 0);
            s->device_rx_used = 0;
            return;
        }
        frame_size = DWC2_DEVICE_TRANSPORT_HEADER_SIZE + length;
        if (s->device_rx_used < frame_size) {
            return;
        }

        dwc2_device_transport_request(
            s, header, header + DWC2_DEVICE_TRANSPORT_HEADER_SIZE);
        s->device_rx_used -= frame_size;
        memmove(s->device_rx_buf, s->device_rx_buf + frame_size,
                s->device_rx_used);
    }
}

static void dwc2_device_transport_event(void *opaque, QEMUChrEvent event)
{
    DWC2State *s = opaque;

    if (event == CHR_EVENT_CLOSED) {
        s->device_rx_used = 0;
        dwc2_device_host_connect(s, false);
    }
}

static void dwc2_device_connect_gpio(void *opaque, int n, int level)
{
    dwc2_device_host_connect(opaque, level);
}

static void dwc2_device_reset_gpio(void *opaque, int n, int level)
{
    if (level) {
        dwc2_device_host_reset(opaque);
    }
}

static uint64_t dwc2_dreg_read(void *ptr, hwaddr addr, int index,
                               unsigned size)
{
    DWC2State *s = ptr;

    return s->dreg[index];
}

static void dwc2_dreg_write(void *ptr, hwaddr addr, int index, uint64_t val,
                            unsigned size)
{
    DWC2State *s = ptr;
    uint32_t old = s->dreg[index];

    switch (addr) {
    case DCFG:
        val &= DCFG_DESCDMA_EN | DCFG_EPMISCNT_MASK |
               DCFG_IPG_ISOC_SUPPORDED | DCFG_PERFRINT_MASK |
               DCFG_DEVADDR_MASK | DCFG_NZ_STS_OUT_HSHK |
               DCFG_DEVSPD_MASK;
        break;
    case DCTL:
        val &= DCTL_PWRONPRGDONE | DCTL_TSTCTL_MASK |
               DCTL_SFTDISCON | DCTL_RMTWKUPSIG |
               DCTL_SGOUTNAK | DCTL_CGOUTNAK |
               DCTL_SGNPINNAK | DCTL_CGNPINNAK;
        if (val & DCTL_SGOUTNAK) {
            val |= DCTL_GOUTNAKSTS;
        } else if (val & DCTL_CGOUTNAK) {
            val &= ~DCTL_GOUTNAKSTS;
        } else {
            val |= old & DCTL_GOUTNAKSTS;
        }
        if (val & DCTL_SGNPINNAK) {
            val |= DCTL_GNPINNAKSTS;
        } else if (val & DCTL_CGNPINNAK) {
            val &= ~DCTL_GNPINNAKSTS;
        } else {
            val |= old & DCTL_GNPINNAKSTS;
        }
        val &= ~(DCTL_SGOUTNAK | DCTL_CGOUTNAK |
                 DCTL_SGNPINNAK | DCTL_CGNPINNAK);
        break;
    case DSTS:
    case DAINT:
    case DTKNQR1:
    case DTKNQR2:
    case DTKNQR3:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write to read-only register\n", __func__);
        return;
    case DIEPMSK:
        val &= DIEPMSK_NAKMSK | DIEPMSK_BNAININTRMSK |
               DIEPMSK_TXFIFOUNDRNMSK | DIEPMSK_TXFIFOEMPTY |
               DIEPMSK_INEPNAKEFFMSK | DIEPMSK_INTKNEPMISMSK |
               DIEPMSK_INTKNTXFEMPMSK | DIEPMSK_TIMEOUTMSK |
               DIEPMSK_AHBERRMSK | DIEPMSK_EPDISBLDMSK |
               DIEPMSK_XFERCOMPLMSK;
        break;
    case DOEPMSK:
        val &= DOEPMSK_BNAMSK | DOEPMSK_BACK2BACKSETUP |
               DOEPMSK_STSPHSERCVDMSK | DOEPMSK_OUTTKNEPDISMSK |
               DOEPMSK_SETUPMSK | DOEPMSK_AHBERRMSK |
               DOEPMSK_EPDISBLDMSK | DOEPMSK_XFERCOMPLMSK;
        break;
    default:
        break;
    }
    s->dreg[index] = val;
    if (addr == DIEPMSK || addr == DOEPMSK || addr == DAINTMSK ||
        addr == DIEPEMPMSK) {
        dwc2_update_device_irq(s);
    }
}

static uint64_t dwc2_depreg_read(void *ptr, hwaddr addr, int index,
                                 unsigned size, bool in)
{
    DWC2State *s = ptr;
    uint32_t *regs = in ? s->diepreg : s->doepreg;

    if (in && (addr & 0x1c) == 0x18) {
        dwc2_device_refresh_fifo_state(s);
    }
    return regs[index];
}

static void dwc2_depreg_write(void *ptr, hwaddr addr, int index, uint64_t val,
                              unsigned size, bool in)
{
    DWC2State *s = ptr;
    uint32_t *regs = in ? s->diepreg : s->doepreg;
    uint32_t reg_offset = addr & 0x1c;
    uint32_t old = regs[index];

    switch (reg_offset) {
    case 0x00:
        if ((val & DXEPCTL_EPDIS) && (old & DXEPCTL_EPENA)) {
            val &= ~(DXEPCTL_EPDIS | DXEPCTL_EPENA);
            regs[(index & ~7) + 2] |= DXEPINT_EPDISBLD;
        }
        break;
    case 0x08:
        val = old & ~val;
        break;
    case 0x18:
        if (!in) {
            return;
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write to read-only register\n", __func__);
        return;
    default:
        break;
    }
    regs[index] = val;
    trace_usb_dwc2_depreg_write(in, index >> 3, addr, old, val);
    if (reg_offset == 0x00 || reg_offset == 0x08) {
        dwc2_update_device_irq(s);
    }
}

static const char *pcgregnm[] = {
        "PCGCTL   ", "PCGCCTL1 "
};

static uint64_t dwc2_pcgreg_read(void *ptr, hwaddr addr, int index,
                                 unsigned size)
{
    DWC2State *s = ptr;
    uint32_t val;

    if (addr < PCGCTL || addr > PCGCCTL1) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return 0;
    }

    val = s->pcgreg[index];

    trace_usb_dwc2_pcgreg_read(addr, pcgregnm[index], val);
    return val;
}

static void dwc2_pcgreg_write(void *ptr, hwaddr addr, int index,
                              uint64_t val, unsigned size)
{
    DWC2State *s = ptr;
    uint64_t orig = val;
    uint32_t *mmio;
    uint32_t old;

    if (addr < PCGCTL || addr > PCGCCTL1) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        return;
    }

    mmio = &s->pcgreg[index];
    old = *mmio;

    trace_usb_dwc2_pcgreg_write(addr, pcgregnm[index], orig, old, val);
    *mmio = val;
}

static uint64_t dwc2_hsotg_read(void *ptr, hwaddr addr, unsigned size)
{
    uint64_t val;

    switch (addr) {
    case HSOTG_REG(0x000) ... HSOTG_REG(0x0fc):
        val = dwc2_glbreg_read(ptr, addr, (addr - HSOTG_REG(0x000)) >> 2, size);
        break;
    case HSOTG_REG(0x100):
        val = dwc2_fszreg_read(ptr, addr, (addr - HSOTG_REG(0x100)) >> 2, size);
        break;
    case HSOTG_REG(0x104) ... HSOTG_REG(0x13c):
        val = dwc2_dptxfsiz_read(ptr, addr,
                                 (addr - HSOTG_REG(0x104)) >> 2, size);
        break;
    case HSOTG_REG(0x140) ... HSOTG_REG(0x3fc):
        val = 0;
        break;
    case HSOTG_REG(0x400) ... HSOTG_REG(0x4fc):
        val = dwc2_hreg0_read(ptr, addr, (addr - HSOTG_REG(0x400)) >> 2, size);
        break;
    case HSOTG_REG(0x500) ... HSOTG_REG(0x7fc):
        val = dwc2_hreg1_read(ptr, addr, (addr - HSOTG_REG(0x500)) >> 2, size);
        break;
    case HSOTG_REG(0x800) ... HSOTG_REG(0x83c):
        val = dwc2_dreg_read(ptr, addr,
                             (addr - HSOTG_REG(0x800)) >> 2, size);
        break;
    case HSOTG_REG(0x840) ... HSOTG_REG(0x8fc):
        val = 0;
        break;
    case HSOTG_REG(0x900) ... HSOTG_REG(0xafc):
        val = dwc2_depreg_read(ptr, addr,
                               (addr - HSOTG_REG(0x900)) >> 2, size, true);
        break;
    case HSOTG_REG(0xb00) ... HSOTG_REG(0xcfc):
        val = dwc2_depreg_read(ptr, addr,
                               (addr - HSOTG_REG(0xb00)) >> 2, size, false);
        break;
    case HSOTG_REG(0xd00) ... HSOTG_REG(0xdfc):
        val = 0;
        break;
    case HSOTG_REG(0xe00) ... HSOTG_REG(0xffc):
        val = dwc2_pcgreg_read(ptr, addr, (addr - HSOTG_REG(0xe00)) >> 2, size);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        val = 0;
        break;
    }

    return val;
}

static void dwc2_hsotg_write(void *ptr, hwaddr addr, uint64_t val,
                             unsigned size)
{
    switch (addr) {
    case HSOTG_REG(0x000) ... HSOTG_REG(0x0fc):
        dwc2_glbreg_write(ptr, addr, (addr - HSOTG_REG(0x000)) >> 2, val, size);
        break;
    case HSOTG_REG(0x100):
        dwc2_fszreg_write(ptr, addr, (addr - HSOTG_REG(0x100)) >> 2, val, size);
        break;
    case HSOTG_REG(0x104) ... HSOTG_REG(0x13c):
        dwc2_dptxfsiz_write(ptr, addr,
                            (addr - HSOTG_REG(0x104)) >> 2, val, size);
        break;
    case HSOTG_REG(0x140) ... HSOTG_REG(0x3fc):
        break;
    case HSOTG_REG(0x400) ... HSOTG_REG(0x4fc):
        dwc2_hreg0_write(ptr, addr, (addr - HSOTG_REG(0x400)) >> 2, val, size);
        break;
    case HSOTG_REG(0x500) ... HSOTG_REG(0x7fc):
        dwc2_hreg1_write(ptr, addr, (addr - HSOTG_REG(0x500)) >> 2, val, size);
        break;
    case HSOTG_REG(0x800) ... HSOTG_REG(0x83c):
        dwc2_dreg_write(ptr, addr, (addr - HSOTG_REG(0x800)) >> 2,
                        val, size);
        break;
    case HSOTG_REG(0x840) ... HSOTG_REG(0x8fc):
        break;
    case HSOTG_REG(0x900) ... HSOTG_REG(0xafc):
        dwc2_depreg_write(ptr, addr,
                          (addr - HSOTG_REG(0x900)) >> 2, val, size, true);
        break;
    case HSOTG_REG(0xb00) ... HSOTG_REG(0xcfc):
        dwc2_depreg_write(ptr, addr,
                          (addr - HSOTG_REG(0xb00)) >> 2, val, size, false);
        break;
    case HSOTG_REG(0xd00) ... HSOTG_REG(0xdfc):
        break;
    case HSOTG_REG(0xe00) ... HSOTG_REG(0xffc):
        dwc2_pcgreg_write(ptr, addr, (addr - HSOTG_REG(0xe00)) >> 2, val, size);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, addr);
        break;
    }
}

static const MemoryRegionOps dwc2_mmio_hsotg_ops = {
    .read = dwc2_hsotg_read,
    .write = dwc2_hsotg_write,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static uint64_t dwc2_hreg2_read(void *ptr, hwaddr addr, unsigned size)
{
    DWC2State *s = ptr;
    unsigned int ep = addr >> 12;
    uint32_t val = 0;

    if (ep == 0 && !(s->gahbcfg & GAHBCFG_DMA_EN)) {
        val = dwc2_device_rx_fifo_read(s);
    } else {
        qemu_log_mask(LOG_UNIMP,
                      "%s: DMA or nonzero receive FIFO read not implemented\n",
                      __func__);
    }
    trace_usb_dwc2_hreg2_read(addr, ep, val);
    return val;
}

static void dwc2_hreg2_write(void *ptr, hwaddr addr, uint64_t val,
                             unsigned size)
{
    DWC2State *s = ptr;
    uint64_t orig = val;
    unsigned int ep = addr >> 12;
    uint32_t old = 0;

    if (!(s->gahbcfg & GAHBCFG_DMA_EN) &&
        (((s->gintsts & GINTSTS_CURMODE_HOST) &&
          dwc2_host_tx_fifo_write(s, ep, val)) ||
         (!(s->gintsts & GINTSTS_CURMODE_HOST) &&
          dwc2_device_tx_fifo_write(s, ep, val)))) {
        old = val;
        if (!(s->gintsts & GINTSTS_CURMODE_HOST)) {
            dwc2_update_device_irq(s);
        }
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: FIFO %u is unavailable or full\n", __func__, ep);
    }
    trace_usb_dwc2_hreg2_write(addr, ep, orig, old, val);
}

static const MemoryRegionOps dwc2_mmio_hreg2_ops = {
    .read = dwc2_hreg2_read,
    .write = dwc2_hreg2_write,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void dwc2_wakeup_endpoint(USBBus *bus, USBEndpoint *ep,
                                 unsigned int stream)
{
    DWC2State *s = container_of(bus, DWC2State, bus);

    trace_usb_dwc2_wakeup_endpoint(ep, stream);

    /* TODO - do something here? */
    qemu_bh_schedule(s->async_bh);
}

static USBBusOps dwc2_bus_ops = {
    .wakeup_endpoint = dwc2_wakeup_endpoint,
};

static void dwc2_work_timer(void *opaque)
{
    DWC2State *s = opaque;

    trace_usb_dwc2_work_timer();
    qemu_bh_schedule(s->async_bh);
}

static void dwc2_reset_enter(Object *obj, ResetType type)
{
    static const uint16_t bcm2711_dev_tx_fifo_depths[] = {
        512, 512, 512, 512, 512, 768, 768,
    };
    DWC2Class *c = DWC2_USB_GET_CLASS(obj);
    DWC2State *s = DWC2_USB(obj);
    int i;

    trace_usb_dwc2_reset_enter();

    if (c->parent_phases.enter) {
        c->parent_phases.enter(obj, type);
    }

    timer_del(s->frame_timer);
    qemu_bh_cancel(s->async_bh);

    if (s->uport.dev && s->uport.dev->attached) {
        usb_detach(&s->uport);
    }

    dwc2_bus_stop(s);

    s->gotgctl = GOTGCTL_BSESVLD | GOTGCTL_ASESVLD | GOTGCTL_CONID_B;
    s->gotgint = 0;
    s->gahbcfg = 0;
    s->gusbcfg = 5 << GUSBCFG_USBTRDTIM_SHIFT;
    s->grstctl = GRSTCTL_AHBIDLE;
    s->gintsts = GINTSTS_CONIDSTSCHNG | GINTSTS_PTXFEMP | GINTSTS_NPTXFEMP |
                 GINTSTS_CURMODE_HOST;
    s->gintmsk = 0;
    s->grxstsr = 0;
    s->grxstsp = 0;
    s->grxfsiz = 1024;
    s->gnptxfsiz = 1024 << FIFOSIZE_DEPTH_SHIFT;
    s->gnptxsts = (4 << FIFOSIZE_DEPTH_SHIFT) | 1024;
    s->gi2cctl = GI2CCTL_I2CDATSE0 | GI2CCTL_ACK;
    s->gpvndctl = 0;
    s->ggpio = 0;
    s->guid = 0;
    /*
     * BCM2711 DWC2 synthesis values.  Operation mode zero in GHWCFG2
     * advertises the dual-role core used by gadget overlays; a host-only
     * value makes Linux override an unchanged dr_mode=peripheral DT.
     */
    s->gsnpsid = 0x4f54280a;
    s->ghwcfg1 = 0;
    s->ghwcfg2 = 0x228ddd50;
    s->ghwcfg3 = 0x0ff000e8;
    s->ghwcfg4 = 0x1ff00020;
    s->glpmcfg = 0;
    s->gpwrdn = GPWRDN_PWRDNRSTN;
    s->gdfifocfg = 0;
    s->gadpctl = 0;
    s->grefclk = 0;
    s->gintmsk2 = 0;
    s->gintsts2 = 0;

    s->hptxfsiz = 500 << FIFOSIZE_DEPTH_SHIFT;
    memset(s->dptxfsiz, 0, sizeof(s->dptxfsiz));
    /*
     * BCM2711 exposes seven dedicated device-mode Tx FIFOs.  Their reset
     * depths are hardware capability inputs to Linux's DWC2 gadget driver;
     * zero makes every g-tx-fifo-size value invalid and leaves all non-EP0
     * IN endpoints unusable.  The reset start addresses overlap by design
     * and Linux assigns non-overlapping addresses when it configures the
     * dynamic FIFO layout.
     */
    for (i = 0; i < ARRAY_SIZE(bcm2711_dev_tx_fifo_depths); i++) {
        s->dptxfsiz[i] =
            bcm2711_dev_tx_fifo_depths[i] << FIFOSIZE_DEPTH_SHIFT |
            0x406;
    }

    s->hcfg = 2 << HCFG_RESVALID_SHIFT;
    s->hfir = 60000;
    s->hfnum = 0x3fff;
    s->hptxsts = (16 << TXSTS_QSPCAVAIL_SHIFT) | 32768;
    s->haint = 0;
    s->haintmsk = 0;
    s->hprt0 = 0;

    memset(s->hreg1, 0, sizeof(s->hreg1));
    memset(s->host_tx_fifo_head, 0, sizeof(s->host_tx_fifo_head));
    memset(s->host_tx_fifo_count, 0, sizeof(s->host_tx_fifo_count));
    memset(s->host_pio_waiting, 0, sizeof(s->host_pio_waiting));
    memset(s->host_pio_rx_reserved_bytes, 0,
           sizeof(s->host_pio_rx_reserved_bytes));
    memset(s->host_pio_rx_reserved_active, 0,
           sizeof(s->host_pio_rx_reserved_active));
    memset(s->dreg, 0, sizeof(s->dreg));
    memset(s->diepreg, 0, sizeof(s->diepreg));
    memset(s->doepreg, 0, sizeof(s->doepreg));
    s->dsts = DSTS_ENUMSPD_HS << DSTS_ENUMSPD_SHIFT;
    s->dvbusdis = 0x17d7;
    s->dvbuspulse = 0x5b8;
    memset(s->pcgreg, 0, sizeof(s->pcgreg));

    s->sof_time = 0;
    s->frame_number = 0;
    s->fi = USB_FRMINTVL - 1;
    s->next_chan = 0;
    s->working = false;
    s->device_connected = false;
    s->device_rx_used = 0;
    dwc2_device_flush_fifos(s);

    for (i = 0; i < DWC2_NB_CHAN; i++) {
        s->packet[i].needs_service = false;
    }
}

static void dwc2_reset_hold(Object *obj, ResetType type)
{
    DWC2Class *c = DWC2_USB_GET_CLASS(obj);
    DWC2State *s = DWC2_USB(obj);

    trace_usb_dwc2_reset_hold();

    if (c->parent_phases.hold) {
        c->parent_phases.hold(obj, type);
    }

    dwc2_update_irq(s);
}

static void dwc2_reset_exit(Object *obj, ResetType type)
{
    DWC2Class *c = DWC2_USB_GET_CLASS(obj);
    DWC2State *s = DWC2_USB(obj);

    trace_usb_dwc2_reset_exit();

    if (c->parent_phases.exit) {
        c->parent_phases.exit(obj, type);
    }

    s->hprt0 = HPRT0_PWR;
    if (s->uport.dev && s->uport.dev->attached) {
        usb_attach(&s->uport);
        usb_device_reset(s->uport.dev);
    }
}

static void dwc2_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    DWC2State *s = DWC2_USB(dev);
    Object *obj;

    obj = object_property_get_link(OBJECT(dev), "dma-mr", &error_abort);

    s->dma_mr = MEMORY_REGION(obj);
    address_space_init(&s->dma_as, s->dma_mr, "dwc2");

    usb_bus_new(&s->bus, sizeof(s->bus), &dwc2_bus_ops, dev);
    usb_register_port(&s->bus, &s->uport, s, 0, &dwc2_port_ops,
                      USB_SPEED_MASK_LOW | USB_SPEED_MASK_FULL |
                      (s->usb_version == 2 ? USB_SPEED_MASK_HIGH : 0));
    s->uport.dev = 0;

    s->usb_frame_time = NANOSECONDS_PER_SECOND / 1000;          /* 1000000 */
    if (NANOSECONDS_PER_SECOND >= USB_HZ_FS) {
        s->usb_bit_time = NANOSECONDS_PER_SECOND / USB_HZ_FS;   /* 83.3 */
    } else {
        s->usb_bit_time = 1;
    }

    s->fi = USB_FRMINTVL - 1;
    s->eof_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dwc2_frame_boundary, s);
    s->frame_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dwc2_work_timer, s);
    s->async_bh = qemu_bh_new_guarded(dwc2_work_bh, s,
                                      &dev->mem_reentrancy_guard);

    sysbus_init_irq(sbd, &s->irq);

    if (qemu_chr_fe_backend_connected(&s->device_chr)) {
        qemu_chr_fe_set_handlers(&s->device_chr,
                                 dwc2_device_transport_can_read,
                                 dwc2_device_transport_read,
                                 dwc2_device_transport_event,
                                 NULL, s, NULL, true);
    }
}

static void dwc2_unrealize(DeviceState *dev)
{
    DWC2State *s = DWC2_USB(dev);

    qemu_chr_fe_deinit(&s->device_chr, false);
}

static void dwc2_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    DWC2State *s = DWC2_USB(obj);

    memory_region_init(&s->container, obj, "dwc2", DWC2_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->container);

    memory_region_init_io(&s->hsotg, obj, &dwc2_mmio_hsotg_ops, s,
                          "dwc2-io", 4 * KiB);
    memory_region_add_subregion(&s->container, 0x0000, &s->hsotg);

    memory_region_init_io(&s->fifos, obj, &dwc2_mmio_hreg2_ops, s,
                          "dwc2-fifo", DWC2_HFIFO_SIZE);
    memory_region_add_subregion(&s->container, 0x1000, &s->fifos);

    qdev_init_gpio_in_named(DEVICE(obj), dwc2_device_connect_gpio,
                            "device-connect", 1);
    qdev_init_gpio_in_named(DEVICE(obj), dwc2_device_reset_gpio,
                            "device-reset", 1);
}

static const VMStateDescription vmstate_dwc2_state_packet = {
    .name = "dwc2/packet",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(devadr, DWC2Packet),
        VMSTATE_UINT32(epnum, DWC2Packet),
        VMSTATE_UINT32(epdir, DWC2Packet),
        VMSTATE_UINT32(mps, DWC2Packet),
        VMSTATE_UINT32(pid, DWC2Packet),
        VMSTATE_UINT32(index, DWC2Packet),
        VMSTATE_UINT32(pcnt, DWC2Packet),
        VMSTATE_UINT32(len, DWC2Packet),
        VMSTATE_INT32(async, DWC2Packet),
        VMSTATE_BOOL(small, DWC2Packet),
        VMSTATE_BOOL(needs_service, DWC2Packet),
        VMSTATE_END_OF_LIST()
    },
};

static int dwc2_post_load(void *opaque, int version_id)
{
    DWC2State *s = opaque;
    size_t reserved_bytes;
    unsigned int reserved_statuses;

    if (s->device_rx_used > sizeof(s->device_rx_buf) ||
        s->rx_fifo_head >= DWC2_FIFO_BYTES ||
        s->rx_fifo_count > DWC2_FIFO_BYTES ||
        s->rx_status_head >= DWC2_RX_STATUS_COUNT ||
        s->rx_status_count > DWC2_RX_STATUS_COUNT) {
        return -EINVAL;
    }
    /*
     * The resumed bottom half indexes packet[next_chan] and the frame
     * accounting divides by usb_bit_time, so both have to be proven sane
     * before any timer or bottom half is allowed to run again.
     */
    if (s->next_chan >= DWC2_NB_CHAN || s->usb_bit_time <= 0 ||
        s->usb_frame_time <= 0) {
        return -EINVAL;
    }
    for (unsigned int channel = 0; channel < DWC2_NB_CHAN; channel++) {
        const DWC2Packet *p = &s->packet[channel];

        if (p->async < DWC2_ASYNC_NONE || p->async > DWC2_ASYNC_FINISHED ||
            p->epnum > USB_MAX_ENDPOINTS ||
            p->index + 5 >= DWC2_HREG1_SIZE / sizeof(uint32_t) ||
            (p->index >> 3) >= DWC2_NB_CHAN ||
            p->len > DWC2_FIFO_BYTES) {
            return -EINVAL;
        }
    }
    for (unsigned int ep = 0; ep < DWC2_DEV_EP_COUNT; ep++) {
        if (s->tx_fifo_head[ep] >= DWC2_FIFO_BYTES ||
            s->tx_fifo_count[ep] > DWC2_FIFO_BYTES) {
            return -EINVAL;
        }
    }
    for (unsigned int channel = 0; channel < DWC2_NB_CHAN; channel++) {
        if (s->host_tx_fifo_head[channel] >= DWC2_FIFO_BYTES ||
            s->host_tx_fifo_count[channel] > DWC2_FIFO_BYTES ||
            s->host_pio_rx_reserved_bytes[channel] > DWC2_FIFO_BYTES ||
            (!s->host_pio_rx_reserved_active[channel] &&
             s->host_pio_rx_reserved_bytes[channel])) {
            return -EINVAL;
        }
    }
    dwc2_host_pio_rx_reserved(s, &reserved_bytes, &reserved_statuses);
    if (reserved_bytes > DWC2_FIFO_BYTES ||
        reserved_statuses > DWC2_RX_STATUS_COUNT) {
        return -EINVAL;
    }
    if (!(s->gintsts & GINTSTS_CURMODE_HOST)) {
        dwc2_bus_stop(s);
        dwc2_update_device_irq(s);
    } else {
        dwc2_device_refresh_fifo_state(s);
        dwc2_host_pio_wake_waiters(s);
        dwc2_update_irq(s);
    }
    return 0;
}

const VMStateDescription vmstate_dwc2_state = {
    .name = "dwc2",
    .version_id = 7,
    .minimum_version_id = 1,
    .post_load = dwc2_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(glbreg, DWC2State,
                             DWC2_GLBREG_SIZE / sizeof(uint32_t)),
        VMSTATE_UINT32_ARRAY(fszreg, DWC2State,
                             DWC2_FSZREG_SIZE / sizeof(uint32_t)),
        VMSTATE_UINT32_ARRAY(hreg0, DWC2State,
                             DWC2_HREG0_SIZE / sizeof(uint32_t)),
        VMSTATE_UINT32_ARRAY(hreg1, DWC2State,
                             DWC2_HREG1_SIZE / sizeof(uint32_t)),
        VMSTATE_UINT32_ARRAY(pcgreg, DWC2State,
                             DWC2_PCGREG_SIZE / sizeof(uint32_t)),
        VMSTATE_UINT32_ARRAY_V(dptxfsiz, DWC2State,
                               DWC2_DPTXFSZ_COUNT, 2),
        VMSTATE_UINT32_ARRAY_V(dreg, DWC2State,
                               DWC2_DREG_SIZE / sizeof(uint32_t), 2),
        VMSTATE_UINT32_ARRAY_V(diepreg, DWC2State,
                               DWC2_DEPREG_SIZE / sizeof(uint32_t), 2),
        VMSTATE_UINT32_ARRAY_V(doepreg, DWC2State,
                               DWC2_DEPREG_SIZE / sizeof(uint32_t), 2),

        VMSTATE_TIMER_PTR(eof_timer, DWC2State),
        VMSTATE_TIMER_PTR(frame_timer, DWC2State),
        VMSTATE_INT64(sof_time, DWC2State),
        VMSTATE_INT64(usb_frame_time, DWC2State),
        VMSTATE_INT64(usb_bit_time, DWC2State),
        VMSTATE_UINT32(usb_version, DWC2State),
        VMSTATE_UINT16(frame_number, DWC2State),
        VMSTATE_UINT16(fi, DWC2State),
        VMSTATE_UINT16(next_chan, DWC2State),
        VMSTATE_BOOL(working, DWC2State),
        VMSTATE_BOOL_V(device_connected, DWC2State, 3),
        VMSTATE_UINT32_V(device_rx_used, DWC2State, 7),
        VMSTATE_UINT8_ARRAY_V(device_rx_buf, DWC2State,
                              DWC2_DEVICE_TRANSPORT_HEADER_SIZE +
                              DWC2_DEVICE_TRANSPORT_MAX_PAYLOAD, 7),
        VMSTATE_UINT8_ARRAY_V(rx_fifo, DWC2State, DWC2_FIFO_BYTES, 4),
        VMSTATE_UINT32_V(rx_fifo_head, DWC2State, 4),
        VMSTATE_UINT32_V(rx_fifo_count, DWC2State, 4),
        VMSTATE_UINT32_ARRAY_V(rx_status, DWC2State,
                               DWC2_RX_STATUS_COUNT, 4),
        VMSTATE_UINT32_V(rx_status_head, DWC2State, 4),
        VMSTATE_UINT32_V(rx_status_count, DWC2State, 4),
        VMSTATE_UINT8_2DARRAY_V(tx_fifo, DWC2State, DWC2_DEV_EP_COUNT,
                                DWC2_FIFO_BYTES, 4),
        VMSTATE_UINT32_ARRAY_V(tx_fifo_head, DWC2State,
                               DWC2_DEV_EP_COUNT, 4),
        VMSTATE_UINT32_ARRAY_V(tx_fifo_count, DWC2State,
                               DWC2_DEV_EP_COUNT, 4),
        VMSTATE_UINT8_2DARRAY_V(host_tx_fifo, DWC2State, DWC2_NB_CHAN,
                                DWC2_FIFO_BYTES, 5),
        VMSTATE_UINT32_ARRAY_V(host_tx_fifo_head, DWC2State,
                               DWC2_NB_CHAN, 5),
        VMSTATE_UINT32_ARRAY_V(host_tx_fifo_count, DWC2State,
                               DWC2_NB_CHAN, 5),
        VMSTATE_BOOL_ARRAY_V(host_pio_waiting, DWC2State,
                             DWC2_NB_CHAN, 5),
        VMSTATE_UINT32_ARRAY_V(host_pio_rx_reserved_bytes, DWC2State,
                               DWC2_NB_CHAN, 6),
        VMSTATE_BOOL_ARRAY_V(host_pio_rx_reserved_active, DWC2State,
                             DWC2_NB_CHAN, 6),

        VMSTATE_STRUCT_ARRAY(packet, DWC2State, DWC2_NB_CHAN, 1,
                             vmstate_dwc2_state_packet, DWC2Packet),
        VMSTATE_UINT8_2DARRAY(usb_buf, DWC2State, DWC2_NB_CHAN,
                              DWC2_MAX_XFER_SIZE),

        VMSTATE_END_OF_LIST()
    }
};

static const Property dwc2_usb_properties[] = {
    DEFINE_PROP_UINT32("usb_version", DWC2State, usb_version, 2),
    DEFINE_PROP_CHR("device-chardev", DWC2State, device_chr),
};

static void dwc2_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    DWC2Class *c = DWC2_USB_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = dwc2_realize;
    dc->unrealize = dwc2_unrealize;
    dc->vmsd = &vmstate_dwc2_state;
    set_bit(DEVICE_CATEGORY_USB, dc->categories);
    device_class_set_props(dc, dwc2_usb_properties);
    resettable_class_set_parent_phases(rc, dwc2_reset_enter, dwc2_reset_hold,
                                       dwc2_reset_exit, &c->parent_phases);
}

static const TypeInfo dwc2_usb_type_info = {
    .name          = TYPE_DWC2_USB,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DWC2State),
    .instance_init = dwc2_init,
    .class_size    = sizeof(DWC2Class),
    .class_init    = dwc2_class_init,
};

static void dwc2_usb_register_types(void)
{
    type_register_static(&dwc2_usb_type_info);
}

type_init(dwc2_usb_register_types)
