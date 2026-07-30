/*
 * Broadcom BCM2711 GENET Ethernet controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_NET_BCM2711_GENET_H
#define HW_NET_BCM2711_GENET_H

#include "hw/core/sysbus.h"
#include "net/net.h"

#define TYPE_BCM2711_GENET "bcm2711-genet"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2711GenetState, BCM2711_GENET)

#define BCM2711_GENET_MMIO_SIZE 0x10000
#define BCM2711_GENET_NUM_REGS (BCM2711_GENET_MMIO_SIZE / sizeof(uint32_t))
#define BCM2711_GENET_NUM_IRQS 2
#define BCM2711_GENET_NUM_PHY_REGS 32

typedef bool (*BCM2711GenetBootReceive)(void *opaque, const uint8_t *packet,
                                       size_t size);
typedef void (*BCM2711GenetBootLinkChanged)(void *opaque, bool link_up);

struct BCM2711GenetState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    AddressSpace dma_as;
    NICState *nic;
    NICConf conf;
    MACAddr configured_macaddr;
    qemu_irq irq[BCM2711_GENET_NUM_IRQS];
    uint32_t regs[BCM2711_GENET_NUM_REGS];
    uint32_t intr_status[BCM2711_GENET_NUM_IRQS];
    uint32_t intr_mask[BCM2711_GENET_NUM_IRQS];
    uint16_t phy_regs[BCM2711_GENET_NUM_PHY_REGS];
    uint8_t dma_error;
    uint64_t dma_error_after;
    uint32_t dma_error_count;
    uint64_t dma_bytes_transferred;
    uint32_t dma_errors_injected;
    uint8_t packet_drop_direction;
    uint64_t packet_drop_after;
    uint32_t packet_drop_count;
    uint64_t packet_drop_packets_seen;
    uint32_t packet_drops_injected;
    BCM2711GenetBootReceive boot_receive;
    BCM2711GenetBootLinkChanged boot_link_changed;
    void *boot_opaque;
    bool boot_client_active;
};

void bcm2711_genet_set_boot_client(BCM2711GenetState *s,
                                   BCM2711GenetBootReceive receive,
                                   BCM2711GenetBootLinkChanged link_changed,
                                   void *opaque);
void bcm2711_genet_set_boot_client_active(BCM2711GenetState *s, bool active);
bool bcm2711_genet_boot_link_up(BCM2711GenetState *s);
ssize_t bcm2711_genet_boot_send(BCM2711GenetState *s, const uint8_t *packet,
                                size_t size);
const MACAddr *bcm2711_genet_mac(BCM2711GenetState *s);
void bcm2711_genet_set_mac(BCM2711GenetState *s, const MACAddr *mac);
void bcm2711_genet_restore_mac(BCM2711GenetState *s);

#endif
