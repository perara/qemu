/*
 * BCM2711 HDMI controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_BCM2711_HDMI_H
#define HW_DISPLAY_BCM2711_HDMI_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

typedef struct BCM2835PropertyState BCM2835PropertyState;

#define TYPE_BCM2711_HDMI "bcm2711-hdmi"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2711HDMIState, BCM2711_HDMI)

#define BCM2711_HDMI_CORE_SIZE 0x300
#define BCM2711_HDMI_CEC_SIZE 0x100

struct BCM2711HDMIState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    MemoryRegion cec_iomem;
    BCM2835PropertyState *property;
    qemu_irq hpd_connected;
    qemu_irq hpd_removed;
    qemu_irq cec_tx;
    qemu_irq cec_rx;
    QEMUTimer *cec_tx_timer;
    uint32_t cec_control[5];
    uint32_t cec_tx_data[4];
    uint32_t cec_rx_data[4];
    uint64_t cec_rx_inject_data[2];
    uint32_t cec_tx_count;
    uint32_t port;
    uint16_t cec_peer_address_mask;
    uint8_t cec_rx_inject_length;
    bool connected;
    bool cec_tx_active;
    bool cec_force_nack;
};

void bcm2711_hdmi_set_property(BCM2711HDMIState *s,
                               BCM2835PropertyState *property);
bool bcm2711_hdmi_is_connected(const BCM2711HDMIState *s);

#endif
