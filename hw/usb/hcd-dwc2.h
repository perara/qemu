/*
 * dwc-hsotg (dwc2) USB host controller state definitions
 *
 * Based on hw/usb/hcd-ehci.h
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

#ifndef HW_USB_HCD_DWC2_H
#define HW_USB_HCD_DWC2_H

#include "qemu/timer.h"
#include "chardev/char-fe.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/usb/usb.h"
#include "hw/usb/dwc2-device-transport.h"
#include "system/dma.h"
#include "qom/object.h"

#define DWC2_MMIO_SIZE      0x11000

#define DWC2_NB_CHAN        8       /* Number of host channels */
#define DWC2_MAX_XFER_SIZE  65536   /* Max transfer size expected in HCTSIZ */

typedef struct DWC2Packet DWC2Packet;
typedef struct DWC2State DWC2State;
typedef struct DWC2Class DWC2Class;
typedef void (*DWC2DevicePacketHandler)(void *opaque, unsigned int ep,
                                        bool in, bool setup,
                                        const uint8_t *data, size_t length,
                                        bool complete);
typedef void (*DWC2DeviceEventHandler)(void *opaque, unsigned int event,
                                       unsigned int value);

enum DWC2DeviceEvent {
    DWC2_DEVICE_EVENT_CONNECT,
    DWC2_DEVICE_EVENT_DISCONNECT,
    DWC2_DEVICE_EVENT_RESET,
    DWC2_DEVICE_EVENT_ENUM_DONE,
    DWC2_DEVICE_EVENT_SUSPEND,
    DWC2_DEVICE_EVENT_RESUME,
};

enum async_state {
    DWC2_ASYNC_NONE = 0,
    DWC2_ASYNC_INITIALIZED,
    DWC2_ASYNC_INFLIGHT,
    DWC2_ASYNC_FINISHED,
};

struct DWC2Packet {
    USBPacket packet;
    uint32_t devadr;
    uint32_t epnum;
    uint32_t epdir;
    uint32_t mps;
    uint32_t pid;
    uint32_t index;
    uint32_t pcnt;
    uint32_t len;
    int32_t async;
    bool small;
    bool needs_service;
};

struct DWC2State {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    USBBus bus;
    qemu_irq irq;
    MemoryRegion *dma_mr;
    AddressSpace dma_as;
    MemoryRegion container;
    MemoryRegion hsotg;
    MemoryRegion fifos;

    union {
#define DWC2_GLBREG_SIZE    0x70
        uint32_t glbreg[DWC2_GLBREG_SIZE / sizeof(uint32_t)];
        struct {
            uint32_t gotgctl;       /* 00 */
            uint32_t gotgint;       /* 04 */
            uint32_t gahbcfg;       /* 08 */
            uint32_t gusbcfg;       /* 0c */
            uint32_t grstctl;       /* 10 */
            uint32_t gintsts;       /* 14 */
            uint32_t gintmsk;       /* 18 */
            uint32_t grxstsr;       /* 1c */
            uint32_t grxstsp;       /* 20 */
            uint32_t grxfsiz;       /* 24 */
            uint32_t gnptxfsiz;     /* 28 */
            uint32_t gnptxsts;      /* 2c */
            uint32_t gi2cctl;       /* 30 */
            uint32_t gpvndctl;      /* 34 */
            uint32_t ggpio;         /* 38 */
            uint32_t guid;          /* 3c */
            uint32_t gsnpsid;       /* 40 */
            uint32_t ghwcfg1;       /* 44 */
            uint32_t ghwcfg2;       /* 48 */
            uint32_t ghwcfg3;       /* 4c */
            uint32_t ghwcfg4;       /* 50 */
            uint32_t glpmcfg;       /* 54 */
            uint32_t gpwrdn;        /* 58 */
            uint32_t gdfifocfg;     /* 5c */
            uint32_t gadpctl;       /* 60 */
            uint32_t grefclk;       /* 64 */
            uint32_t gintmsk2;      /* 68 */
            uint32_t gintsts2;      /* 6c */
        };
    };

    union {
#define DWC2_FSZREG_SIZE    0x04
        uint32_t fszreg[DWC2_FSZREG_SIZE / sizeof(uint32_t)];
        struct {
            uint32_t hptxfsiz;      /* 100 */
        };
    };

    /* Device periodic Tx FIFO sizes at 0x104-0x13c. */
#define DWC2_DPTXFSZ_COUNT  15
    uint32_t dptxfsiz[DWC2_DPTXFSZ_COUNT];

    union {
#define DWC2_HREG0_SIZE     0x44
        uint32_t hreg0[DWC2_HREG0_SIZE / sizeof(uint32_t)];
        struct {
            uint32_t hcfg;          /* 400 */
            uint32_t hfir;          /* 404 */
            uint32_t hfnum;         /* 408 */
            uint32_t rsvd0;         /* 40c */
            uint32_t hptxsts;       /* 410 */
            uint32_t haint;         /* 414 */
            uint32_t haintmsk;      /* 418 */
            uint32_t hflbaddr;      /* 41c */
            uint32_t rsvd1[8];      /* 420-43c */
            uint32_t hprt0;         /* 440 */
        };
    };

#define DWC2_HREG1_SIZE     (0x20 * DWC2_NB_CHAN)
    uint32_t hreg1[DWC2_HREG1_SIZE / sizeof(uint32_t)];

#define hcchar(_ch)     hreg1[((_ch) << 3) + 0] /* 500, 520, ... */
#define hcsplt(_ch)     hreg1[((_ch) << 3) + 1] /* 504, 524, ... */
#define hcint(_ch)      hreg1[((_ch) << 3) + 2] /* 508, 528, ... */
#define hcintmsk(_ch)   hreg1[((_ch) << 3) + 3] /* 50c, 52c, ... */
#define hctsiz(_ch)     hreg1[((_ch) << 3) + 4] /* 510, 530, ... */
#define hcdma(_ch)      hreg1[((_ch) << 3) + 5] /* 514, 534, ... */
#define hcdmab(_ch)     hreg1[((_ch) << 3) + 7] /* 51c, 53c, ... */

    union {
#define DWC2_DREG_SIZE      0x40
        uint32_t dreg[DWC2_DREG_SIZE / sizeof(uint32_t)];
        struct {
            uint32_t dcfg;          /* 800 */
            uint32_t dctl;          /* 804 */
            uint32_t dsts;          /* 808 */
            uint32_t rsvd_d0;       /* 80c */
            uint32_t diepmsk;       /* 810 */
            uint32_t doepmsk;       /* 814 */
            uint32_t daint;         /* 818 */
            uint32_t daintmsk;      /* 81c */
            uint32_t dtknqr1;       /* 820 */
            uint32_t dtknqr2;       /* 824 */
            uint32_t dvbusdis;      /* 828 */
            uint32_t dvbuspulse;    /* 82c */
            uint32_t dtknqr3;       /* 830 */
            uint32_t diepepmsk;     /* 834 */
            uint32_t rsvd_d1[2];    /* 838-83c */
        };
    };

#define DWC2_DEV_EP_COUNT    16
#define DWC2_DEPREG_SIZE     (0x20 * DWC2_DEV_EP_COUNT)
    uint32_t diepreg[DWC2_DEPREG_SIZE / sizeof(uint32_t)];
    uint32_t doepreg[DWC2_DEPREG_SIZE / sizeof(uint32_t)];

#define diepctl(_ep) diepreg[((_ep) << 3) + 0]
#define diepint(_ep) diepreg[((_ep) << 3) + 2]
#define dieptsiz(_ep) diepreg[((_ep) << 3) + 4]
#define diepdma(_ep) diepreg[((_ep) << 3) + 5]
#define dtxfsts(_ep) diepreg[((_ep) << 3) + 6]

#define doepctl(_ep) doepreg[((_ep) << 3) + 0]
#define doepint(_ep) doepreg[((_ep) << 3) + 2]
#define doeptsiz(_ep) doepreg[((_ep) << 3) + 4]
#define doepdma(_ep) doepreg[((_ep) << 3) + 5]

    union {
#define DWC2_PCGREG_SIZE    0x08
        uint32_t pcgreg[DWC2_PCGREG_SIZE / sizeof(uint32_t)];
        struct {
            uint32_t pcgctl;        /* e00 */
            uint32_t pcgcctl1;      /* e04 */
        };
    };

    /*
     * The BCM2711 integration advertises 4096 32-bit FIFO words.  Keep a
     * bounded backing store for every device Tx FIFO so configured FIFO
     * depths, PIO payloads, flushes, and migration are observable.
     */
#define DWC2_FIFO_WORDS       4096
#define DWC2_FIFO_BYTES       (DWC2_FIFO_WORDS * sizeof(uint32_t))
#define DWC2_RX_STATUS_COUNT  64
#define DWC2_HFIFO_SIZE       (0x1000 * DWC2_DEV_EP_COUNT)

    uint8_t rx_fifo[DWC2_FIFO_BYTES];
    uint32_t rx_fifo_head;
    uint32_t rx_fifo_count;
    uint32_t rx_status[DWC2_RX_STATUS_COUNT];
    uint32_t rx_status_head;
    uint32_t rx_status_count;
    uint8_t tx_fifo[DWC2_DEV_EP_COUNT][DWC2_FIFO_BYTES];
    uint32_t tx_fifo_head[DWC2_DEV_EP_COUNT];
    uint32_t tx_fifo_count[DWC2_DEV_EP_COUNT];
    uint8_t host_tx_fifo[DWC2_NB_CHAN][DWC2_FIFO_BYTES];
    uint32_t host_tx_fifo_head[DWC2_NB_CHAN];
    uint32_t host_tx_fifo_count[DWC2_NB_CHAN];
    bool host_pio_waiting[DWC2_NB_CHAN];
    uint32_t host_pio_rx_reserved_bytes[DWC2_NB_CHAN];
    bool host_pio_rx_reserved_active[DWC2_NB_CHAN];

    /*
     *  Internal state
     */
    QEMUTimer *eof_timer;
    QEMUTimer *frame_timer;
    QEMUBH *async_bh;
    int64_t sof_time;
    int64_t usb_frame_time;
    int64_t usb_bit_time;
    uint32_t usb_version;
    uint16_t frame_number;
    uint16_t fi;
    uint16_t next_chan;
    bool working;
    bool device_connected;
    CharFrontend device_chr;
    uint32_t device_rx_used;
    uint8_t device_rx_buf[DWC2_DEVICE_TRANSPORT_HEADER_SIZE +
                          DWC2_DEVICE_TRANSPORT_MAX_PAYLOAD];
    uint8_t device_tx_buf[DWC2_DEVICE_TRANSPORT_MAX_PAYLOAD];
    DWC2DevicePacketHandler device_packet_handler;
    DWC2DeviceEventHandler device_event_handler;
    void *device_handler_opaque;
    USBPort uport;
    DWC2Packet packet[DWC2_NB_CHAN];                   /* one packet per chan */
    uint8_t usb_buf[DWC2_NB_CHAN][DWC2_MAX_XFER_SIZE]; /* one buffer per chan */
};

struct DWC2Class {
    /*< private >*/
    SysBusDeviceClass parent_class;
    ResettablePhases parent_phases;

    /*< public >*/
};

#define TYPE_DWC2_USB   "dwc2-usb"
OBJECT_DECLARE_TYPE(DWC2State, DWC2Class, DWC2_USB)

/*
 * Behavioral firmware host boundary.  These helpers execute the same host
 * channel engine, USB packets, DMA, hub routing, and endpoint models as MMIO
 * programming while the VideoCore replacement owns the controller pre-ARM.
 */
bool dwc2_host_firmware_reset_port(DWC2State *s, Error **errp);
bool dwc2_host_firmware_abort_transfer(DWC2State *s, Error **errp);
ssize_t dwc2_host_firmware_transfer(
    DWC2State *s, uint8_t address, uint8_t endpoint, uint8_t type,
    uint16_t max_packet, bool in, uint32_t pid, uint32_t dma,
    void *buffer, size_t length, Error **errp);

/*
 * Host-facing device-mode boundary.  A transport such as USB/IP or Raw Gadget
 * feeds host tokens through these functions; the DWC2 model owns endpoint
 * readiness, DMA, transfer-size accounting, and interrupts.
 */
void dwc2_device_host_connect(DWC2State *s, bool connected);
void dwc2_device_host_reset(DWC2State *s);
void dwc2_device_host_enum_done(DWC2State *s, unsigned int speed);
void dwc2_device_host_suspend(DWC2State *s, bool suspended);
ssize_t dwc2_device_host_send(DWC2State *s, unsigned int ep,
                              const void *buf, size_t len, bool setup);
ssize_t dwc2_device_host_receive(DWC2State *s, unsigned int ep,
                                 void *buf, size_t len);
void dwc2_device_set_firmware_handlers(DWC2State *s,
                                       DWC2DevicePacketHandler packet,
                                       DWC2DeviceEventHandler event,
                                       void *opaque);
void dwc2_device_firmware_start(DWC2State *s);
int dwc2_device_firmware_arm_out(DWC2State *s, unsigned int ep,
                                 uint32_t dma, size_t length, uint32_t mps,
                                 uint32_t type, bool setup);
int dwc2_device_firmware_arm_in(DWC2State *s, unsigned int ep,
                                uint32_t dma, const void *data, size_t length,
                                uint32_t mps, uint32_t type);
void dwc2_device_firmware_disable_endpoint(DWC2State *s, unsigned int ep);

#endif
