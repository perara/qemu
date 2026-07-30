/*
 * Broadcom BCM2711 GENET Ethernet controller
 *
 * This implements the platform discovery, register, interrupt, MDIO/PHY, and
 * descriptor DMA boundaries used by the BCM2711 Linux driver.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/net/bcm2711_genet.h"
#include "migration/vmstate.h"
#include "net/checksum.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "system/dma.h"
#include "trace.h"

#define GENET_SYS_REV_CTRL          0x0000
#define GENET_INTRL2_0              0x0200
#define GENET_INTRL2_1              0x0240
#define GENET_INTRL2_CPU_STAT       0x00
#define GENET_INTRL2_CPU_SET        0x04
#define GENET_INTRL2_CPU_CLEAR      0x08
#define GENET_INTRL2_CPU_MASK_STAT  0x0c
#define GENET_INTRL2_CPU_MASK_SET   0x10
#define GENET_INTRL2_CPU_MASK_CLEAR 0x14
#define GENET_UMAC_MDIO_CMD         0x0e14
#define GENET_UMAC_CMD              0x0808
#define GENET_UMAC_MIB_START        0x0c00
#define GENET_UMAC_MIB_RX_END       0x0c70
#define GENET_UMAC_MIB_TX_START     0x0c80
#define GENET_UMAC_MIB_TX_END       0x0cf0
#define GENET_UMAC_MIB_RUNT_START   0x0d00
#define GENET_UMAC_MIB_RUNT_END     0x0d0c
#define GENET_UMAC_MIB_CTRL         0x0d80
#define GENET_UMAC_MDF_ERR_CNT      0x0e38
#define GENET_UMAC_MDF_CTRL         0x0e50
#define GENET_UMAC_MDF_ADDR         0x0e54
#define GENET_EXT_RGMII_OOB_CTRL    0x008c
#define GENET_RBUF_OVFL_CNT         0x0394
#define GENET_RBUF_ERR_CNT          0x0398

#define GENET_RDMA_DESC             0x2000
#define GENET_TDMA_DESC             0x4000
#define GENET_RDMA_REG              0x2c00
#define GENET_TDMA_REG              0x4c00
#define GENET_DMA_RING_SIZE         0x40
#define GENET_DMA_COMMON            0x440
#define GENET_DMA_CTRL              0x04
#define GENET_DMA_STATUS            0x08
#define GENET_DMA_RING_CONS_INDEX   0x08
#define GENET_DMA_RING_PROD_INDEX   0x0c
#define GENET_DMA_RING_BUF_SIZE     0x10
#define GENET_DMA_RING_START        0x14
#define GENET_DMA_RING_READ_PTR     0x2c

#define GENET_DMA_DESC_SIZE         12
#define GENET_DMA_DESC_COUNT        256
#define GENET_DMA_RING_COUNT        17
#define GENET_DMA_MAX_FRAME         16384
#define GENET_STATUS_BLOCK_SIZE     64
#define GENET_RX_ALIGN_PAD          2
#define GENET_RX_PREFIX_SIZE        (GENET_STATUS_BLOCK_SIZE + \
                                     GENET_RX_ALIGN_PAD)

#define GENET_V5_REVISION           0x06000000

#define MDIO_START_BUSY             BIT(29)
#define MDIO_READ_FAIL              BIT(28)
#define MDIO_RD                     (2U << 26)
#define MDIO_WR                     (1U << 26)
#define MDIO_OP_MASK                (3U << 26)
#define MDIO_PHY_SHIFT              21
#define MDIO_REG_SHIFT              16
#define MDIO_ADDR_MASK              0x1f
#define MDIO_DATA_MASK              0xffff

#define GENET_IRQ_MDIO_DONE         BIT(23)
#define GENET_IRQ_MDIO_ERROR        BIT(24)
#define GENET_IRQ_LINK_UP           BIT(4)
#define GENET_IRQ_LINK_DOWN         BIT(5)
#define GENET_IRQ_TBUF_UNDERRUN     BIT(8)
#define GENET_IRQ_RBUF_OVERFLOW     BIT(9)
#define GENET_IRQ_RXDMA_DONE        BIT(13)
#define GENET_IRQ_TXDMA_DONE        BIT(16)
#define GENET_IRQ1_TX(ring)         BIT(ring)
#define GENET_IRQ1_RX(ring)         BIT(16 + (ring))

#define UMAC_CMD_TX_EN              BIT(0)
#define UMAC_CMD_RX_EN              BIT(1)
#define UMAC_CMD_PROMISC            BIT(4)
#define UMAC_CMD_SW_RESET           BIT(13)
#define RGMII_LINK                  BIT(4)

#define MIB_RESET_RX                BIT(0)
#define MIB_RESET_RUNT              BIT(1)
#define MIB_RESET_TX                BIT(2)

#define GENET_MDF_FILTER_COUNT      17

#define MIB_RX_PKT                  0x28
#define MIB_RX_BYTES                0x2c
#define MIB_RX_MULTICAST            0x30
#define MIB_RX_BROADCAST            0x34
#define MIB_RX_GOOD_PKT             0x64
#define MIB_RX_UNICAST              0x68
#define MIB_TX_PKT                  0x28
#define MIB_TX_MULTICAST            0x2c
#define MIB_TX_BROADCAST            0x30
#define MIB_TX_BYTES                0x68
#define MIB_TX_GOOD_PKT             0x6c
#define MIB_TX_UNICAST              0x70

#define DMA_CTRL_EN                 BIT(0)
#define DMA_CTRL_RING_EN(ring)      BIT(1 + (ring))
#define DMA_BUFLENGTH_SHIFT         16
#define DMA_BUFLENGTH_MASK          0x0fff
#define DMA_OWN                     BIT(15)
#define DMA_EOP                     BIT(14)
#define DMA_SOP                     BIT(13)
#define DMA_TX_DO_CSUM              BIT(4)
#define DMA_RX_BROADCAST            BIT(6)
#define DMA_RX_MULTICAST            BIT(5)
#define DMA_TX_UNDERRUN             BIT(9)

#define GENET_DMA_ERROR_NONE        0
#define GENET_DMA_ERROR_TX          1
#define GENET_DMA_ERROR_RX          2

#define BCM2711_PHY_ADDRESS         1
#define MII_BMCR                    0
#define MII_BMSR                    1
#define MII_PHYSID1                 2
#define MII_PHYSID2                 3
#define MII_ADVERTISE               4
#define MII_LPA                     5
#define MII_CTRL1000                9
#define MII_STAT1000                10
#define MII_ESTATUS                 15
#define BMCR_RESET                  BIT(15)
#define BMCR_PDOWN                  BIT(11)
#define BMCR_ISOLATE                BIT(10)
#define BMCR_ANRESTART              BIT(9)
#define BMSR_LINK_STATUS            BIT(2)

enum {
    GENET_PACKET_DROP_NONE,
    GENET_PACKET_DROP_TX,
    GENET_PACKET_DROP_RX,
    GENET_PACKET_DROP_BOTH,
};

static void bcm2711_genet_mib_packet(BCM2711GenetState *s,
                                     const uint8_t *packet, size_t size,
                                     bool receive);
static bool bcm2711_genet_inject_packet_drop(BCM2711GenetState *s,
                                              uint8_t direction);

static bool bcm2711_genet_link_up(BCM2711GenetState *s)
{
    NetClientState *nc = s->nic ? qemu_get_queue(s->nic) : NULL;

    return nc && nc->peer && !nc->link_down;
}

void bcm2711_genet_set_boot_client(BCM2711GenetState *s,
                                   BCM2711GenetBootReceive receive,
                                   BCM2711GenetBootLinkChanged link_changed,
                                   void *opaque)
{
    s->boot_receive = receive;
    s->boot_link_changed = link_changed;
    s->boot_opaque = opaque;
}

bool bcm2711_genet_boot_link_up(BCM2711GenetState *s)
{
    return bcm2711_genet_link_up(s);
}

void bcm2711_genet_set_boot_client_active(BCM2711GenetState *s, bool active)
{
    NetClientState *nc = s->nic ? qemu_get_queue(s->nic) : NULL;

    s->boot_client_active = active;
    if (active && nc) {
        qemu_flush_queued_packets(nc);
    }
}

ssize_t bcm2711_genet_boot_send(BCM2711GenetState *s, const uint8_t *packet,
                                size_t size)
{
    ssize_t sent;

    if (!bcm2711_genet_link_up(s)) {
        return -ENETDOWN;
    }
    if (bcm2711_genet_inject_packet_drop(s, GENET_PACKET_DROP_TX)) {
        return size;
    }
    sent = qemu_send_packet(qemu_get_queue(s->nic), packet, size);
    if (sent == size) {
        bcm2711_genet_mib_packet(s, packet, size, false);
    }
    return sent;
}

const MACAddr *bcm2711_genet_mac(BCM2711GenetState *s)
{
    return &s->conf.macaddr;
}

void bcm2711_genet_set_mac(BCM2711GenetState *s, const MACAddr *mac)
{
    s->conf.macaddr = *mac;
    if (s->nic) {
        qemu_format_nic_info_str(qemu_get_queue(s->nic),
                                 s->conf.macaddr.a);
    }
}

void bcm2711_genet_restore_mac(BCM2711GenetState *s)
{
    bcm2711_genet_set_mac(s, &s->configured_macaddr);
}

static void bcm2711_genet_update_irq(BCM2711GenetState *s, unsigned int bank)
{
    qemu_set_irq(s->irq[bank], s->intr_status[bank] & ~s->intr_mask[bank]);
}

static void bcm2711_genet_set_phy_link(BCM2711GenetState *s, bool link_up,
                                       bool interrupt)
{
    bool old_link = s->phy_regs[MII_BMSR] & BMSR_LINK_STATUS;

    if (link_up) {
        s->phy_regs[MII_BMSR] |= BMSR_LINK_STATUS;
        s->phy_regs[MII_LPA] = 0xc5e1;
        s->phy_regs[MII_STAT1000] = 0x0800;
        s->regs[GENET_EXT_RGMII_OOB_CTRL / 4] |= RGMII_LINK;
    } else {
        s->phy_regs[MII_BMSR] &= ~BMSR_LINK_STATUS;
        s->phy_regs[MII_LPA] = 0;
        s->phy_regs[MII_STAT1000] = 0;
        s->regs[GENET_EXT_RGMII_OOB_CTRL / 4] &= ~RGMII_LINK;
    }

    if (interrupt && old_link != link_up) {
        s->intr_status[0] |= link_up ? GENET_IRQ_LINK_UP :
                                      GENET_IRQ_LINK_DOWN;
        bcm2711_genet_update_irq(s, 0);
    }
    trace_bcm2711_genet_link(link_up);
}

static void bcm2711_genet_phy_reset(BCM2711GenetState *s)
{
    memset(s->phy_regs, 0, sizeof(s->phy_regs));
    s->phy_regs[MII_BMCR] = 0x1140;
    /* Gigabit capabilities and completed autonegotiation. */
    s->phy_regs[MII_BMSR] = 0x7969;
    /* BCM54213PE uses the BCM54210E model ID with revision 2. */
    s->phy_regs[MII_PHYSID1] = 0x600d;
    s->phy_regs[MII_PHYSID2] = 0x84a2;
    s->phy_regs[MII_ADVERTISE] = 0x01e1;
    s->phy_regs[MII_CTRL1000] = 0x0200;
    s->phy_regs[MII_ESTATUS] = 0x3000;
    bcm2711_genet_set_phy_link(s, bcm2711_genet_link_up(s), false);
}

static hwaddr bcm2711_genet_desc_address(BCM2711GenetState *s,
                                         hwaddr desc_base,
                                         unsigned int index)
{
    hwaddr offset = desc_base + index * GENET_DMA_DESC_SIZE;
    uint64_t low = s->regs[(offset + 4) / 4];
    uint64_t high = s->regs[(offset + 8) / 4] & 0xff;

    return low | (high << 32);
}

static bool bcm2711_genet_dma_enabled(BCM2711GenetState *s,
                                      hwaddr reg_base, unsigned int ring)
{
    uint32_t ctrl = s->regs[(reg_base + GENET_DMA_COMMON +
                             GENET_DMA_CTRL) / 4];

    return (ctrl & (DMA_CTRL_EN | DMA_CTRL_RING_EN(ring))) ==
           (DMA_CTRL_EN | DMA_CTRL_RING_EN(ring));
}

static hwaddr bcm2711_genet_mib_bucket(size_t frame_length)
{
    static const size_t limits[] = {
        64, 127, 255, 511, 1023, 1518, 1522, 2047, 4095, 9216,
    };

    for (unsigned int bucket = 0; bucket < ARRAY_SIZE(limits); bucket++) {
        if (frame_length <= limits[bucket]) {
            return bucket * sizeof(uint32_t);
        }
    }
    return (ARRAY_SIZE(limits) - 1) * sizeof(uint32_t);
}

static void bcm2711_genet_mib_packet(BCM2711GenetState *s,
                                     const uint8_t *packet, size_t length,
                                     bool receive)
{
    hwaddr base = receive ? GENET_UMAC_MIB_START :
                            GENET_UMAC_MIB_TX_START;
    size_t wire_length = length + 4;
    /*
     * Runt frames reach the counters through the boot consumer, which does
     * not apply the receive filter's length guard.  Classify them without
     * reading address bytes the frame does not carry.
     */
    bool multicast = length >= 1 && (packet[0] & 1);
    bool broadcast = length >= 6 && multicast &&
                     !memcmp(packet, "\xff\xff\xff\xff\xff\xff", 6);

    s->regs[(base + bcm2711_genet_mib_bucket(wire_length)) / 4]++;
    if (receive) {
        s->regs[(base + MIB_RX_PKT) / 4]++;
        s->regs[(base + MIB_RX_BYTES) / 4] += wire_length;
        s->regs[(base + MIB_RX_GOOD_PKT) / 4]++;
        s->regs[(base + (broadcast ? MIB_RX_BROADCAST :
                         multicast ? MIB_RX_MULTICAST :
                                     MIB_RX_UNICAST)) / 4]++;
    } else {
        s->regs[(base + MIB_TX_PKT) / 4]++;
        s->regs[(base + MIB_TX_BYTES) / 4] += wire_length;
        s->regs[(base + MIB_TX_GOOD_PKT) / 4]++;
        s->regs[(base + (broadcast ? MIB_TX_BROADCAST :
                         multicast ? MIB_TX_MULTICAST :
                                     MIB_TX_UNICAST)) / 4]++;
    }
}

static bool bcm2711_genet_filter_match(BCM2711GenetState *s,
                                       const uint8_t *packet, size_t length)
{
    uint32_t control = s->regs[GENET_UMAC_MDF_CTRL / 4];

    if (length < 6) {
        return false;
    }
    if (s->regs[GENET_UMAC_CMD / 4] & UMAC_CMD_PROMISC) {
        return true;
    }

    for (unsigned int slot = 0; slot < GENET_MDF_FILTER_COUNT; slot++) {
        hwaddr offset = GENET_UMAC_MDF_ADDR + slot * 2 * sizeof(uint32_t);
        uint32_t ms = s->regs[offset / 4];
        uint32_t ls = s->regs[(offset + sizeof(uint32_t)) / 4];
        uint8_t address[6] = {
            ms >> 8, ms, ls >> 24, ls >> 16, ls >> 8, ls,
        };

        if ((control & BIT(GENET_MDF_FILTER_COUNT - 1 - slot)) &&
            !memcmp(packet, address, sizeof(address))) {
            return true;
        }
    }
    return false;
}

static void bcm2711_genet_mib_reset(BCM2711GenetState *s, uint32_t control)
{
    if (control & MIB_RESET_RX) {
        memset(&s->regs[GENET_UMAC_MIB_START / 4], 0,
               GENET_UMAC_MIB_RX_END - GENET_UMAC_MIB_START + 4);
    }
    if (control & MIB_RESET_TX) {
        memset(&s->regs[GENET_UMAC_MIB_TX_START / 4], 0,
               GENET_UMAC_MIB_TX_END - GENET_UMAC_MIB_TX_START + 4);
    }
    if (control & MIB_RESET_RUNT) {
        memset(&s->regs[GENET_UMAC_MIB_RUNT_START / 4], 0,
               GENET_UMAC_MIB_RUNT_END - GENET_UMAC_MIB_RUNT_START + 4);
    }
}

static bool bcm2711_genet_inject_dma_error(BCM2711GenetState *s,
                                            uint8_t error)
{
    if (s->dma_error != error ||
        s->dma_errors_injected >= s->dma_error_count ||
        s->dma_bytes_transferred < s->dma_error_after) {
        return false;
    }

    s->dma_errors_injected++;
    if (error == GENET_DMA_ERROR_TX) {
        s->intr_status[0] |= GENET_IRQ_TBUF_UNDERRUN;
    } else {
        s->intr_status[0] |= GENET_IRQ_RBUF_OVERFLOW;
        s->regs[GENET_RBUF_OVFL_CNT / 4]++;
        s->regs[GENET_RBUF_ERR_CNT / 4]++;
    }
    bcm2711_genet_update_irq(s, 0);
    trace_bcm2711_genet_dma_error(error, s->dma_bytes_transferred,
                                  s->dma_errors_injected,
                                  s->dma_error_count);
    return true;
}

static bool bcm2711_genet_inject_packet_drop(BCM2711GenetState *s,
                                              uint8_t direction)
{
    bool selected = s->packet_drop_direction == direction ||
                    s->packet_drop_direction == GENET_PACKET_DROP_BOTH;
    bool drop;

    if (!selected) {
        return false;
    }
    drop = s->packet_drop_packets_seen >= s->packet_drop_after &&
           s->packet_drops_injected < s->packet_drop_count;
    s->packet_drop_packets_seen++;
    if (drop) {
        s->packet_drops_injected++;
        trace_bcm2711_genet_packet_drop(
            direction, s->packet_drop_packets_seen,
            s->packet_drops_injected, s->packet_drop_count);
    }
    return drop;
}

static void bcm2711_genet_dma_complete(BCM2711GenetState *s,
                                       unsigned int ring, bool receive)
{
    unsigned int bank;
    uint32_t mask;

    if (ring == 16) {
        bank = 0;
        mask = receive ? GENET_IRQ_RXDMA_DONE : GENET_IRQ_TXDMA_DONE;
    } else {
        bank = 1;
        mask = receive ? GENET_IRQ1_RX(ring) : GENET_IRQ1_TX(ring);
    }
    s->intr_status[bank] |= mask;
    bcm2711_genet_update_irq(s, bank);
}

static void bcm2711_genet_transmit(BCM2711GenetState *s, unsigned int ring)
{
    hwaddr ring_base = GENET_TDMA_REG + ring * GENET_DMA_RING_SIZE;
    uint32_t cons = s->regs[(ring_base + GENET_DMA_RING_CONS_INDEX) / 4] &
                    0xffff;
    uint32_t prod = s->regs[(ring_base + GENET_DMA_RING_PROD_INDEX) / 4] &
                    0xffff;
    uint32_t ring_size = s->regs[(ring_base + GENET_DMA_RING_BUF_SIZE) / 4] >>
                         16;
    uint32_t start = s->regs[(ring_base + GENET_DMA_RING_START) / 4] / 3;
    g_autoptr(GByteArray) packet = g_byte_array_sized_new(2048);
    bool checksum = false;
    bool packet_started = false;
    unsigned int processed = 0;

    if (!(s->regs[GENET_UMAC_CMD / 4] & UMAC_CMD_TX_EN) ||
        !bcm2711_genet_dma_enabled(s, GENET_TDMA_REG, ring) ||
        ring_size == 0 || ring_size > GENET_DMA_DESC_COUNT) {
        return;
    }

    while (cons != prod && processed++ < ring_size) {
        unsigned int index = start + cons % ring_size;
        hwaddr desc_offset;
        hwaddr dma_addr;
        uint32_t len_stat;
        size_t length;
        size_t skip = 0;

        if (index >= GENET_DMA_DESC_COUNT) {
            break;
        }
        desc_offset = GENET_TDMA_DESC + index * GENET_DMA_DESC_SIZE;
        len_stat = s->regs[desc_offset / 4];
        length = (len_stat >> DMA_BUFLENGTH_SHIFT) & DMA_BUFLENGTH_MASK;
        dma_addr = bcm2711_genet_desc_address(s, GENET_TDMA_DESC, index);

        if (len_stat & DMA_SOP) {
            g_byte_array_set_size(packet, 0);
            packet_started = true;
            checksum = len_stat & DMA_TX_DO_CSUM;
            skip = MIN(length, (size_t)GENET_STATUS_BLOCK_SIZE);
        }
        if (!packet_started || length < skip ||
            packet->len + length - skip > GENET_DMA_MAX_FRAME) {
            packet_started = false;
        } else if (length > skip) {
            size_t old_length = packet->len;

            if (bcm2711_genet_inject_dma_error(s, GENET_DMA_ERROR_TX)) {
                s->regs[desc_offset / 4] = len_stat | DMA_TX_UNDERRUN;
                packet_started = false;
            } else {
                g_byte_array_set_size(packet, old_length + length - skip);
                if (dma_memory_read(&s->dma_as, dma_addr + skip,
                                    packet->data + old_length, length - skip,
                                    MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
                    packet_started = false;
                } else {
                    s->dma_bytes_transferred += length - skip;
                }
            }
        }

        cons = (cons + 1) & 0xffff;
        if ((len_stat & DMA_EOP) && packet_started && packet->len) {
            if (checksum) {
                net_checksum_calculate(packet->data, packet->len, CSUM_ALL);
            }
            if (!bcm2711_genet_inject_packet_drop(
                    s, GENET_PACKET_DROP_TX)) {
                qemu_send_packet(qemu_get_queue(s->nic), packet->data,
                                 packet->len);
                bcm2711_genet_mib_packet(
                    s, packet->data, packet->len, false);
            }
            packet_started = false;
        }
    }

    s->regs[(ring_base + GENET_DMA_RING_CONS_INDEX) / 4] = cons;
    s->regs[(ring_base + GENET_DMA_RING_READ_PTR) / 4] =
        (start + cons % ring_size) * 3;
    bcm2711_genet_dma_complete(s, ring, false);
    trace_bcm2711_genet_tx(ring, processed, packet->len);
}

static bool bcm2711_genet_rx_ring_ready(BCM2711GenetState *s,
                                        unsigned int ring)
{
    hwaddr ring_base = GENET_RDMA_REG + ring * GENET_DMA_RING_SIZE;
    /* The first two index registers swap meaning between RDMA and TDMA. */
    uint32_t prod = s->regs[(ring_base + GENET_DMA_RING_CONS_INDEX) / 4] &
                    0xffff;
    uint32_t cons = s->regs[(ring_base + GENET_DMA_RING_PROD_INDEX) / 4] &
                    0xffff;
    uint32_t ring_size = s->regs[(ring_base + GENET_DMA_RING_BUF_SIZE) / 4] >>
                         16;

    return bcm2711_genet_dma_enabled(s, GENET_RDMA_REG, ring) &&
           ring_size > 0 && ring_size <= GENET_DMA_DESC_COUNT &&
           ((prod - cons) & 0xffff) < ring_size;
}

static int bcm2711_genet_rx_ring(BCM2711GenetState *s)
{
    /* Legacy Linux drivers use ring 16 as the unclassified default queue. */
    if (bcm2711_genet_rx_ring_ready(s, 16)) {
        return 16;
    }
    for (unsigned int ring = 0; ring < 16; ring++) {
        if (bcm2711_genet_rx_ring_ready(s, ring)) {
            return ring;
        }
    }
    return -1;
}

static bool bcm2711_genet_can_receive(NetClientState *nc)
{
    BCM2711GenetState *s = qemu_get_nic_opaque(nc);

    return (s->boot_client_active && s->boot_receive) ||
           ((s->regs[GENET_UMAC_CMD / 4] & UMAC_CMD_RX_EN) &&
            bcm2711_genet_rx_ring(s) >= 0);
}

static ssize_t bcm2711_genet_receive(NetClientState *nc, const uint8_t *buf,
                                     size_t size)
{
    BCM2711GenetState *s = qemu_get_nic_opaque(nc);
    int ring = bcm2711_genet_rx_ring(s);
    hwaddr ring_base;
    uint32_t prod;
    uint32_t ring_size;
    uint32_t buffer_size;
    uint32_t start;
    unsigned int index;
    hwaddr dma_addr;
    uint8_t status[GENET_STATUS_BLOCK_SIZE] = { 0 };
    uint32_t flags = DMA_SOP | DMA_EOP;
    uint32_t length_status;

    if (bcm2711_genet_inject_packet_drop(s, GENET_PACKET_DROP_RX)) {
        return size;
    }

    if (s->boot_client_active && s->boot_receive &&
        s->boot_receive(s->boot_opaque, buf, size)) {
        bcm2711_genet_mib_packet(s, buf, size, true);
        return size;
    }

    if (!(s->regs[GENET_UMAC_CMD / 4] & UMAC_CMD_RX_EN) || ring < 0) {
        return 0;
    }
    if (!bcm2711_genet_filter_match(s, buf, size)) {
        s->regs[GENET_UMAC_MDF_ERR_CNT / 4]++;
        return size;
    }
    ring_base = GENET_RDMA_REG + ring * GENET_DMA_RING_SIZE;
    prod = s->regs[(ring_base + GENET_DMA_RING_CONS_INDEX) / 4] &
                    0xffff;
    ring_size = s->regs[(ring_base + GENET_DMA_RING_BUF_SIZE) / 4] >> 16;
    buffer_size = s->regs[(ring_base + GENET_DMA_RING_BUF_SIZE) / 4] & 0xffff;
    start = s->regs[(ring_base + GENET_DMA_RING_START) / 4] / 3;
    index = start + prod % ring_size;

    if (index >= GENET_DMA_DESC_COUNT ||
        size + GENET_RX_PREFIX_SIZE > buffer_size) {
        return size;
    }
    if (bcm2711_genet_inject_dma_error(s, GENET_DMA_ERROR_RX)) {
        return size;
    }

    dma_addr = bcm2711_genet_desc_address(s, GENET_RDMA_DESC, index);
    if (buf[0] & 1) {
        flags |= !memcmp(buf, "\xff\xff\xff\xff\xff\xff", 6) ?
                 DMA_RX_BROADCAST : DMA_RX_MULTICAST;
    }
    length_status = ((size + GENET_RX_PREFIX_SIZE) << DMA_BUFLENGTH_SHIFT) |
                    flags;
    stl_le_p(status, length_status);
    if (dma_memory_write(&s->dma_as, dma_addr, status, sizeof(status),
                         MEMTXATTRS_UNSPECIFIED) != MEMTX_OK ||
        dma_memory_write(&s->dma_as, dma_addr + GENET_RX_PREFIX_SIZE, buf,
                         size, MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        return size;
    }
    s->dma_bytes_transferred += size + GENET_RX_PREFIX_SIZE;

    prod = (prod + 1) & 0xffff;
    s->regs[(ring_base + GENET_DMA_RING_CONS_INDEX) / 4] = prod;
    bcm2711_genet_mib_packet(s, buf, size, true);
    bcm2711_genet_dma_complete(s, ring, true);
    trace_bcm2711_genet_rx(ring, index, size);
    return size;
}

static void bcm2711_genet_link_status_changed(NetClientState *nc)
{
    BCM2711GenetState *s = qemu_get_nic_opaque(nc);
    bool link_up = bcm2711_genet_link_up(s);

    bcm2711_genet_set_phy_link(
        s, link_up &&
        !(s->phy_regs[MII_BMCR] & (BMCR_PDOWN | BMCR_ISOLATE)), true);
    if (s->boot_link_changed) {
        s->boot_link_changed(s->boot_opaque, link_up);
    }
}

static NetClientInfo bcm2711_genet_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = bcm2711_genet_can_receive,
    .receive = bcm2711_genet_receive,
    .link_status_changed = bcm2711_genet_link_status_changed,
};

static void bcm2711_genet_mdio_transaction(BCM2711GenetState *s,
                                           uint32_t command)
{
    unsigned int phy = (command >> MDIO_PHY_SHIFT) & MDIO_ADDR_MASK;
    unsigned int reg = (command >> MDIO_REG_SHIFT) & MDIO_ADDR_MASK;
    uint32_t result = command & ~(MDIO_START_BUSY | MDIO_READ_FAIL);
    bool failed = phy != BCM2711_PHY_ADDRESS;

    if (!failed && (command & MDIO_OP_MASK) == MDIO_RD) {
        result = (result & ~MDIO_DATA_MASK) | s->phy_regs[reg];
    } else if (!failed && (command & MDIO_OP_MASK) == MDIO_WR) {
        if (reg == MII_BMCR && (command & BMCR_RESET)) {
            bcm2711_genet_phy_reset(s);
            result = (result & ~MDIO_DATA_MASK) | s->phy_regs[MII_BMCR];
        } else if (reg == MII_BMCR) {
            /* Reset and autonegotiation restart are self-clearing commands. */
            s->phy_regs[reg] = command & MDIO_DATA_MASK & ~BMCR_ANRESTART;
            bcm2711_genet_set_phy_link(
                s, bcm2711_genet_link_up(s) &&
                !(s->phy_regs[reg] & (BMCR_PDOWN | BMCR_ISOLATE)), true);
            result = (result & ~MDIO_DATA_MASK) | s->phy_regs[reg];
        } else if (reg == MII_ADVERTISE || reg == MII_CTRL1000) {
            s->phy_regs[reg] = command & MDIO_DATA_MASK;
        }
    } else if (!failed) {
        failed = true;
    }

    if (failed) {
        result |= MDIO_READ_FAIL;
        s->intr_status[0] |= GENET_IRQ_MDIO_ERROR;
    }
    s->intr_status[0] |= GENET_IRQ_MDIO_DONE;
    s->regs[GENET_UMAC_MDIO_CMD / 4] = result;
    bcm2711_genet_update_irq(s, 0);
    trace_bcm2711_genet_mdio(phy, reg,
                            (command & MDIO_OP_MASK) == MDIO_WR,
                            result & MDIO_DATA_MASK);
}

static bool bcm2711_genet_intrl2_decode(hwaddr offset, unsigned int *bank,
                                        hwaddr *reg)
{
    if (offset >= GENET_INTRL2_0 && offset <= GENET_INTRL2_0 + 0x14) {
        *bank = 0;
        *reg = offset - GENET_INTRL2_0;
        return true;
    }
    if (offset >= GENET_INTRL2_1 && offset <= GENET_INTRL2_1 + 0x14) {
        *bank = 1;
        *reg = offset - GENET_INTRL2_1;
        return true;
    }
    return false;
}

static uint64_t bcm2711_genet_read(void *opaque, hwaddr offset, unsigned size)
{
    BCM2711GenetState *s = opaque;
    unsigned int bank;
    hwaddr reg;

    if (bcm2711_genet_intrl2_decode(offset, &bank, &reg)) {
        switch (reg) {
        case GENET_INTRL2_CPU_STAT:
            return s->intr_status[bank];
        case GENET_INTRL2_CPU_MASK_STAT:
            return s->intr_mask[bank];
        default:
            return 0;
        }
    }
    if (offset == GENET_RDMA_REG + GENET_DMA_COMMON + GENET_DMA_STATUS ||
        offset == GENET_TDMA_REG + GENET_DMA_COMMON + GENET_DMA_STATUS) {
        hwaddr ctrl_offset = offset - GENET_DMA_STATUS + GENET_DMA_CTRL;

        return ~s->regs[ctrl_offset / 4] & 0x3ffff;
    }
    return s->regs[offset / 4];
}

static void bcm2711_genet_write(void *opaque, hwaddr offset, uint64_t value,
                                unsigned size)
{
    BCM2711GenetState *s = opaque;
    unsigned int bank;
    hwaddr reg;
    uint32_t val = value;

    if (bcm2711_genet_intrl2_decode(offset, &bank, &reg)) {
        switch (reg) {
        case GENET_INTRL2_CPU_SET:
            s->intr_status[bank] |= val;
            break;
        case GENET_INTRL2_CPU_CLEAR:
            s->intr_status[bank] &= ~val;
            break;
        case GENET_INTRL2_CPU_MASK_SET:
            s->intr_mask[bank] |= val;
            break;
        case GENET_INTRL2_CPU_MASK_CLEAR:
            s->intr_mask[bank] &= ~val;
            break;
        default:
            break;
        }
        bcm2711_genet_update_irq(s, bank);
        return;
    }

    if (offset == GENET_UMAC_MIB_CTRL) {
        bcm2711_genet_mib_reset(s, val);
        s->regs[offset / 4] = val &
                             (MIB_RESET_RX | MIB_RESET_RUNT | MIB_RESET_TX);
        return;
    }
    if ((offset >= GENET_UMAC_MIB_START &&
         offset <= GENET_UMAC_MIB_RX_END) ||
        (offset >= GENET_UMAC_MIB_TX_START &&
         offset <= GENET_UMAC_MIB_TX_END) ||
        (offset >= GENET_UMAC_MIB_RUNT_START &&
         offset <= GENET_UMAC_MIB_RUNT_END)) {
        return;
    }
    if (offset == GENET_UMAC_MDF_ERR_CNT && val != 0) {
        return;
    }

    if (offset == GENET_SYS_REV_CTRL) {
        return;
    }

    if (offset == GENET_UMAC_CMD) {
        /* UniMAC software reset is self-clearing. */
        val &= ~UMAC_CMD_SW_RESET;
    }
    s->regs[offset / 4] = val;
    if (offset == GENET_UMAC_MDIO_CMD && (val & MDIO_START_BUSY)) {
        bcm2711_genet_mdio_transaction(s, val);
    }
    if (offset >= GENET_TDMA_REG &&
        offset < GENET_TDMA_REG + GENET_DMA_RING_COUNT *
                                  GENET_DMA_RING_SIZE &&
        (offset - GENET_TDMA_REG) % GENET_DMA_RING_SIZE ==
        GENET_DMA_RING_PROD_INDEX) {
        bcm2711_genet_transmit(
            s, (offset - GENET_TDMA_REG) / GENET_DMA_RING_SIZE);
    }
    if ((offset == GENET_UMAC_CMD ||
         offset == GENET_RDMA_REG + GENET_DMA_COMMON + GENET_DMA_CTRL ||
         (offset >= GENET_RDMA_REG &&
          offset < GENET_RDMA_REG + GENET_DMA_RING_COUNT *
                                    GENET_DMA_RING_SIZE &&
          (offset - GENET_RDMA_REG) % GENET_DMA_RING_SIZE ==
          GENET_DMA_RING_PROD_INDEX)) &&
        bcm2711_genet_can_receive(qemu_get_queue(s->nic))) {
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
    }
}

static const MemoryRegionOps bcm2711_genet_ops = {
    .read = bcm2711_genet_read,
    .write = bcm2711_genet_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void bcm2711_genet_reset(DeviceState *dev)
{
    BCM2711GenetState *s = BCM2711_GENET(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->intr_status, 0, sizeof(s->intr_status));
    memset(s->intr_mask, 0xff, sizeof(s->intr_mask));
    s->regs[GENET_SYS_REV_CTRL / 4] = GENET_V5_REVISION;
    bcm2711_genet_phy_reset(s);
    for (unsigned int bank = 0; bank < BCM2711_GENET_NUM_IRQS; bank++) {
        bcm2711_genet_update_irq(s, bank);
    }
}

static int bcm2711_genet_post_load(void *opaque, int version_id)
{
    BCM2711GenetState *s = opaque;

    for (unsigned int bank = 0; bank < BCM2711_GENET_NUM_IRQS; bank++) {
        bcm2711_genet_update_irq(s, bank);
    }
    bcm2711_genet_set_phy_link(
        s, bcm2711_genet_link_up(s) &&
        !(s->phy_regs[MII_BMCR] & (BMCR_PDOWN | BMCR_ISOLATE)), false);
    return 0;
}

static const VMStateDescription vmstate_bcm2711_genet = {
    .name = TYPE_BCM2711_GENET,
    .version_id = 3,
    .minimum_version_id = 1,
    .post_load = bcm2711_genet_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, BCM2711GenetState,
                             BCM2711_GENET_NUM_REGS),
        VMSTATE_UINT32_ARRAY(intr_status, BCM2711GenetState,
                             BCM2711_GENET_NUM_IRQS),
        VMSTATE_UINT32_ARRAY(intr_mask, BCM2711GenetState,
                             BCM2711_GENET_NUM_IRQS),
        VMSTATE_UINT16_ARRAY(phy_regs, BCM2711GenetState,
                             BCM2711_GENET_NUM_PHY_REGS),
        VMSTATE_UINT64_V(dma_bytes_transferred, BCM2711GenetState, 2),
        VMSTATE_UINT32_V(dma_errors_injected, BCM2711GenetState, 2),
        VMSTATE_UINT64_V(packet_drop_packets_seen, BCM2711GenetState, 3),
        VMSTATE_UINT32_V(packet_drops_injected, BCM2711GenetState, 3),
        VMSTATE_END_OF_LIST()
    },
};

static void bcm2711_genet_realize(DeviceState *dev, Error **errp)
{
    BCM2711GenetState *s = BCM2711_GENET(dev);

    if (s->dma_error > GENET_DMA_ERROR_RX) {
        error_setg(errp, "dma-error must be 0 (none), 1 (TX), or 2 (RX)");
        return;
    }
    if (s->dma_error != GENET_DMA_ERROR_NONE) {
        if (s->dma_error_after == UINT64_MAX) {
            error_setg(errp, "dma-error requires dma-error-after");
            return;
        }
        if (!s->dma_error_count) {
            error_setg(errp, "dma-error-count must be nonzero");
            return;
        }
    }
    if (s->packet_drop_direction > GENET_PACKET_DROP_BOTH) {
        error_setg(errp,
                   "packet-drop-direction must be 0 (none), 1 (TX), "
                   "2 (RX), or 3 (both)");
        return;
    }
    if (s->packet_drop_direction != GENET_PACKET_DROP_NONE) {
        if (s->packet_drop_after == UINT64_MAX) {
            error_setg(errp,
                       "packet-drop-direction requires packet-drop-after");
            return;
        }
        if (!s->packet_drop_count) {
            error_setg(errp, "packet-drop-count must be nonzero");
            return;
        }
    }
    address_space_init(&s->dma_as, get_system_memory(), "bcm2711-genet-dma");
    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->configured_macaddr = s->conf.macaddr;
    s->nic = qemu_new_nic(&bcm2711_genet_net_info, &s->conf,
                          object_get_typename(OBJECT(dev)), dev->id,
                          &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}

static void bcm2711_genet_unrealize(DeviceState *dev)
{
    BCM2711GenetState *s = BCM2711_GENET(dev);

    qemu_del_nic(s->nic);
    address_space_destroy(&s->dma_as);
}

static void bcm2711_genet_init(Object *obj)
{
    BCM2711GenetState *s = BCM2711_GENET(obj);

    memory_region_init_io(&s->mmio, obj, &bcm2711_genet_ops, s,
                          TYPE_BCM2711_GENET, BCM2711_GENET_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    for (unsigned int i = 0; i < BCM2711_GENET_NUM_IRQS; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[i]);
    }
    object_property_add_uint64_ptr(obj, "dma-bytes-transferred",
                                   &s->dma_bytes_transferred,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "dma-errors-injected",
                                   &s->dma_errors_injected,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "packet-drop-packets-seen",
                                   &s->packet_drop_packets_seen,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "packet-drops-injected",
                                   &s->packet_drops_injected,
                                   OBJ_PROP_FLAG_READ);
}

static const Property bcm2711_genet_properties[] = {
    DEFINE_NIC_PROPERTIES(BCM2711GenetState, conf),
    DEFINE_PROP_UINT8("dma-error", BCM2711GenetState, dma_error,
                      GENET_DMA_ERROR_NONE),
    DEFINE_PROP_UINT64("dma-error-after", BCM2711GenetState,
                       dma_error_after, UINT64_MAX),
    DEFINE_PROP_UINT32("dma-error-count", BCM2711GenetState,
                       dma_error_count, 1),
    DEFINE_PROP_UINT8("packet-drop-direction", BCM2711GenetState,
                      packet_drop_direction, GENET_PACKET_DROP_NONE),
    DEFINE_PROP_UINT64("packet-drop-after", BCM2711GenetState,
                       packet_drop_after, UINT64_MAX),
    DEFINE_PROP_UINT32("packet-drop-count", BCM2711GenetState,
                       packet_drop_count, 1),
};

static void bcm2711_genet_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, bcm2711_genet_reset);
    dc->realize = bcm2711_genet_realize;
    dc->unrealize = bcm2711_genet_unrealize;
    dc->vmsd = &vmstate_bcm2711_genet;
    device_class_set_props(dc, bcm2711_genet_properties);
}

static const TypeInfo bcm2711_genet_info = {
    .name = TYPE_BCM2711_GENET,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2711GenetState),
    .instance_init = bcm2711_genet_init,
    .class_init = bcm2711_genet_class_init,
};

static void bcm2711_genet_register_types(void)
{
    type_register_static(&bcm2711_genet_info);
}

type_init(bcm2711_genet_register_types)
