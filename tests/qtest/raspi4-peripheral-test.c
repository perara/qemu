/*
 * Raspberry Pi 4 BCM2711 peripheral model tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "raspi4-boot-test.h"

void test_bcm2711_rng200_registers(void)
{
    const uint64_t rng_base = 0xfe104000;
    const uint32_t int_total = BIT(0);
    const uint32_t int_nist_fail = BIT(5);
    const uint32_t int_startup = BIT(17);
    const uint32_t int_master_fail = BIT(31);
    const char *rng_path = "/machine/soc/peripherals/rng200";
    QTestState *qts = qtest_init(
        "-global bcm2835-rng.rng200-refill=off "
        "-global bcm2835-rng.rng200-deterministic-seed=1 "
        "-M raspi4b");
    QDict *response = qtest_qmp(
        qts,
        "{ 'execute': 'qom-get', 'arguments': { "
        "'path': %s, 'property': 'sysbus-irq[0]' } }",
        rng_path);

    g_assert_cmpstr(qdict_get_str(response, "return"), ==,
                    "/machine/soc/gic/unnamed-gpio-in[125]");
    qobject_unref(response);
    g_assert_cmphex(qtest_readl(qts, rng_base), ==, 0);
    g_assert_cmphex(qtest_readl(qts, rng_base + 0x0c), ==, 0);
    g_assert_cmphex(qtest_readl(qts, rng_base + 0x18), ==, 0);
    g_assert_cmphex(qtest_readl(qts, rng_base + 0x24), ==, 0);
    qtest_writel(qts, rng_base + 0x1c, int_startup | int_total);
    qtest_writel(qts, rng_base, 1);
    g_assert_cmphex(qtest_readl(qts, rng_base), ==, 1);
    g_assert_cmphex(qtest_readl(qts, rng_base + 0x0c), ==, 544);
    g_assert_cmphex(qtest_readl(qts, rng_base + 0x18), ==, int_startup);
    g_assert_cmphex(qtest_readl(qts, rng_base + 0x24), ==, 16);
    g_assert_true(qtest_readl(qts, 0xff841210) & BIT(29));
    qtest_writel(qts, rng_base + 0x10, 544);
    g_assert_cmphex(qtest_readl(qts, rng_base + 0x18), ==,
                    int_startup | int_total);
    qtest_writel(qts, rng_base + 0x18, int_total);
    qtest_writel(qts, rng_base + 0x10, 545);
    g_assert_cmphex(qtest_readl(qts, rng_base + 0x18), ==, int_startup);
    g_assert_cmphex(qtest_readl(qts, rng_base + 0x20), ==, 0x00042021);
    g_assert_cmphex(qtest_readl(qts, rng_base + 0x24), ==, 15);

    qtest_writel(qts, rng_base + 0x24, 15 << 8);
    g_assert_cmphex(qtest_readl(qts, rng_base + 0x24), ==,
                    (15 << 8) | 15);
    g_assert_cmphex(qtest_readl(qts, rng_base + 0x18), ==,
                    int_startup | int_total);
    qtest_writel(qts, rng_base + 0x18, int_startup);
    g_assert_true(qtest_readl(qts, 0xff841210) & BIT(29));
    qtest_writel(qts, rng_base + 0x18, int_total);
    g_assert_false(qtest_readl(qts, 0xff841210) & BIT(29));

    for (unsigned int word = 0; word < 15; word++) {
        g_assert_cmphex(qtest_readl(qts, rng_base + 0x20), !=, 0);
    }
    g_assert_cmphex(qtest_readl(qts, rng_base + 0x24), ==, 15 << 8);
    g_assert_cmphex(qtest_readl(qts, rng_base + 0x20), ==, 0);

    qtest_writel(qts, rng_base + 0x04, 1);
    g_assert_cmphex(qtest_readl(qts, rng_base), ==, 0);
    g_assert_cmphex(qtest_readl(qts, rng_base + 0x0c), ==, 0);
    g_assert_cmphex(qtest_readl(qts, rng_base + 0x10), ==, 0);
    g_assert_cmphex(qtest_readl(qts, rng_base + 0x18), ==, 0);
    g_assert_cmphex(qtest_readl(qts, rng_base + 0x1c), ==, 0);
    g_assert_cmphex(qtest_readl(qts, rng_base + 0x24), ==, 0);
    qtest_quit(qts);

    qts = qtest_init("-M raspi4b");
    qtest_writel(qts, rng_base, 1);
    g_assert_cmphex(qtest_readl(qts, rng_base + 0x24), ==, 16);
    qtest_readl(qts, rng_base + 0x20);
    g_assert_cmphex(qtest_readl(qts, rng_base + 0x24), ==, 16);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, rng_base), ==, 0);
    g_assert_cmphex(qtest_readl(qts, rng_base + 0x24), ==, 0);
    qtest_quit(qts);

    qts = qtest_init(
        "-global bcm2835-rng.rng200-nist-fail=on -M raspi4b");
    qtest_writel(qts, rng_base, 1);
    g_assert_cmphex(qtest_readl(qts, rng_base + 0x18) &
                    (int_nist_fail | int_master_fail), ==, int_nist_fail);
    g_assert_cmphex(qtest_readl(qts, rng_base + 0x24), ==, 0);
    qtest_writel(qts, rng_base + 0x18, int_nist_fail);
    qtest_writel(qts, rng_base, 0);
    qtest_writel(qts, rng_base, 1);
    g_assert_cmphex(qtest_readl(qts, rng_base + 0x18) &
                    int_nist_fail, ==, int_nist_fail);
    qtest_quit(qts);

    qts = qtest_init(
        "-global bcm2835-rng.rng200-master-fail=on -M raspi4b");
    qtest_writel(qts, rng_base, 1);
    g_assert_cmphex(qtest_readl(qts, rng_base + 0x18) &
                    (int_nist_fail | int_master_fail), ==, int_master_fail);
    g_assert_cmphex(qtest_readl(qts, rng_base + 0x24), ==, 0);
    qtest_quit(qts);
}
void test_bcm2711_rng200_migration(void)
{
    const uint64_t rng_base = 0xfe104000;
    const char *command =
        "-global bcm2835-rng.rng200-refill=off "
        "-global bcm2835-rng.rng200-deterministic-seed=0x12345678 "
        "-M raspi4b";
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    QTestState *source = qtest_init(command);
    QTestState *destination;
    uint32_t expected = 0x12345678;

    qtest_writel(source, rng_base + 0x1c, BIT(0) | BIT(17));
    qtest_writel(source, rng_base + 0x10, 600);
    qtest_writel(source, rng_base + 0x24, 15 << 8);
    qtest_writel(source, rng_base, 1);
    expected = rng200_xorshift32(expected);
    g_assert_cmphex(qtest_readl(source, rng_base + 0x20), ==, expected);
    g_assert_cmphex(qtest_readl(source, rng_base + 0x24), ==,
                    (15 << 8) | 15);

    destination = migrate_to_new_qtest(source, command,
                                       &migration_dir, &migration_socket);
    g_assert_cmphex(qtest_readl(destination, rng_base), ==, 1);
    g_assert_cmphex(qtest_readl(destination, rng_base + 0x0c), ==, 544);
    g_assert_cmphex(qtest_readl(destination, rng_base + 0x10), ==, 600);
    g_assert_cmphex(qtest_readl(destination, rng_base + 0x18), ==,
                    BIT(0) | BIT(17));
    g_assert_cmphex(qtest_readl(destination, rng_base + 0x1c), ==,
                    BIT(0) | BIT(17));
    g_assert_cmphex(qtest_readl(destination, rng_base + 0x24), ==,
                    (15 << 8) | 15);
    expected = rng200_xorshift32(expected);
    g_assert_cmphex(qtest_readl(destination, rng_base + 0x20), ==, expected);

    qtest_quit(source);
    qtest_quit(destination);
    unlink(migration_socket);
    rmdir(migration_dir);
}
void test_bcm2711_genet_registers(void)
{
    QTestState *qts = qtest_init("-M raspi4b");
    uint32_t result;

    g_assert_cmphex(qtest_readl(qts, BCM2711_GENET_BASE +
                                GENET_SYS_REV_CTRL), ==, 0x06000000);
    g_assert_cmphex(qtest_readl(qts, BCM2711_GENET_BASE + GENET_INTRL2_0 +
                                GENET_INTRL2_CPU_MASK_STATUS), ==,
                    0xffffffff);

    qtest_writel(qts, BCM2711_GENET_BASE + GENET_INTRL2_0 +
                 GENET_INTRL2_CPU_MASK_CLEAR,
                 GENET_IRQ_MDIO_DONE | GENET_IRQ_MDIO_ERROR);
    result = bcm2711_genet_mdio_read(qts, 1, 2);
    g_assert_false(result & (GENET_MDIO_START_BUSY | GENET_MDIO_READ_FAIL));
    g_assert_cmphex(result & 0xffff, ==, 0x600d);
    result = bcm2711_genet_mdio_read(qts, 1, 3);
    g_assert_cmphex(result & 0xffff, ==, 0x84a2);
    g_assert_cmphex(qtest_readl(qts, BCM2711_GENET_BASE + GENET_INTRL2_0 +
                                GENET_INTRL2_CPU_STAT) &
                    GENET_IRQ_MDIO_DONE, ==, GENET_IRQ_MDIO_DONE);

    qtest_writel(qts, BCM2711_GENET_BASE + GENET_INTRL2_0 +
                 GENET_INTRL2_CPU_CLEAR, GENET_IRQ_MDIO_DONE);
    result = bcm2711_genet_mdio_read(qts, 2, 2);
    g_assert_true(result & GENET_MDIO_READ_FAIL);
    g_assert_cmphex(qtest_readl(qts, BCM2711_GENET_BASE + GENET_INTRL2_0 +
                                GENET_INTRL2_CPU_STAT) &
                    (GENET_IRQ_MDIO_DONE | GENET_IRQ_MDIO_ERROR), ==,
                    GENET_IRQ_MDIO_DONE | GENET_IRQ_MDIO_ERROR);

    qtest_writel(qts, BCM2711_GENET_BASE + GENET_INTRL2_1 +
                 GENET_INTRL2_CPU_SET, BIT(4));
    g_assert_cmphex(qtest_readl(qts, BCM2711_GENET_BASE + GENET_INTRL2_1 +
                                GENET_INTRL2_CPU_STAT), ==, BIT(4));
    qtest_writel(qts, BCM2711_GENET_BASE + GENET_UMAC_CMD, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, BCM2711_GENET_BASE + GENET_UMAC_CMD),
                    ==, 0x12345678);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, BCM2711_GENET_BASE + GENET_UMAC_CMD),
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, BCM2711_GENET_BASE + GENET_INTRL2_0 +
                                GENET_INTRL2_CPU_STAT), ==, 0);
    g_assert_cmphex(qtest_readl(qts, BCM2711_GENET_BASE + GENET_INTRL2_1 +
                                GENET_INTRL2_CPU_STAT), ==, 0);
    qtest_quit(qts);
}
#ifndef _WIN32
void test_bcm2711_genet_packets(void)
{
    static const uint8_t packet[60] = {
        0x52, 0x54, 0x00, 0x12, 0x34, 0x56,
        0x52, 0x54, 0x00, 0x65, 0x43, 0x21,
        0x08, 0x00, 0x45, 0x00, 0x00, 0x2e,
        0x00, 0x01, 0x00, 0x00, 0x40, 0x11,
        0x00, 0x00, 0xc0, 0x00, 0x02, 0x01,
        0xc0, 0x00, 0x02, 0x02, 0x04, 0xd2,
        0x16, 0x2e, 0x00, 0x1a, 0x00, 0x00,
        'G', 'E', 'N', 'E', 'T', '-', 'D', 'M', 'A', '-', 'T', 'E', 'S', 'T',
        0, 0, 0, 0,
    };
    uint8_t tx_buffer[64 + sizeof(packet)] = { 0 };
    uint8_t received[sizeof(packet)];
    uint8_t rejected[sizeof(packet)];
    uint8_t multicast[sizeof(packet)];
    uint8_t broadcast[sizeof(packet)];
    uint8_t rx_status[4];
    uint8_t rx_packet[sizeof(packet)];
    uint32_t framed_length;
    uint32_t mdio;
    int sockets[2];
    QTestState *qts;

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    qts = qtest_initf(
        "-M raspi4b -netdev socket,fd=%d,id=net0 "
        "-global bcm2711-genet.netdev=net0", sockets[1]);
    close(sockets[1]);

    mdio = bcm2711_genet_mdio_read(qts, 1, 1);
    g_assert_true(mdio & BIT(2));
    g_assert_true(qtest_readl(qts, BCM2711_GENET_BASE +
                              GENET_EXT_RGMII_OOB_CTRL) & GENET_RGMII_LINK);

    qtest_qmp_assert_success(
        qts, "{'execute':'set_link','arguments':"
             "{'name':'bcm2711-genet.0','up':false}}");
    mdio = bcm2711_genet_mdio_read(qts, 1, 1);
    g_assert_false(mdio & BIT(2));
    g_assert_false(qtest_readl(qts, BCM2711_GENET_BASE +
                               GENET_EXT_RGMII_OOB_CTRL) & GENET_RGMII_LINK);
    g_assert_true(qtest_readl(qts, BCM2711_GENET_BASE + GENET_INTRL2_0 +
                              GENET_INTRL2_CPU_STAT) & GENET_IRQ_LINK_DOWN);
    qtest_writel(qts, BCM2711_GENET_BASE + GENET_INTRL2_0 +
                 GENET_INTRL2_CPU_CLEAR,
                 GENET_IRQ_LINK_UP | GENET_IRQ_LINK_DOWN |
                 GENET_IRQ_MDIO_DONE);

    qtest_qmp_assert_success(
        qts, "{'execute':'set_link','arguments':"
             "{'name':'bcm2711-genet.0','up':true}}");
    mdio = bcm2711_genet_mdio_read(qts, 1, 1);
    g_assert_true(mdio & BIT(2));
    g_assert_true(qtest_readl(qts, BCM2711_GENET_BASE +
                              GENET_EXT_RGMII_OOB_CTRL) & GENET_RGMII_LINK);
    g_assert_true(qtest_readl(qts, BCM2711_GENET_BASE + GENET_INTRL2_0 +
                              GENET_INTRL2_CPU_STAT) & GENET_IRQ_LINK_UP);

    qtest_writel(qts, BCM2711_GENET_BASE + GENET_UMAC_CMD, 3);
    qtest_writel(qts, BCM2711_GENET_BASE + GENET_TDMA_REG +
                 GENET_DMA_RING_BUF_SIZE, (4 << 16) | 2048);
    qtest_writel(qts, BCM2711_GENET_BASE + GENET_TDMA_REG +
                 GENET_DMA_RING_START, 0);
    qtest_writel(qts, BCM2711_GENET_BASE + GENET_TDMA_REG +
                 GENET_DMA_COMMON + GENET_DMA_CTRL, 3);
    memcpy(tx_buffer + 64, packet, sizeof(packet));
    qtest_memwrite(qts, 0x100000, tx_buffer, sizeof(tx_buffer));
    qtest_writel(qts, BCM2711_GENET_BASE + GENET_TDMA_DESC,
                 (sizeof(tx_buffer) << 16) | GENET_DMA_SOP | GENET_DMA_EOP);
    qtest_writel(qts, BCM2711_GENET_BASE + GENET_TDMA_DESC + 4, 0x100000);
    qtest_writel(qts, BCM2711_GENET_BASE + GENET_TDMA_DESC + 8, 0);
    qtest_writel(qts, BCM2711_GENET_BASE + GENET_INTRL2_1 +
                 GENET_INTRL2_CPU_MASK_CLEAR, GENET_IRQ1_TX_RING0);
    qtest_writel(qts, BCM2711_GENET_BASE + GENET_TDMA_REG +
                 GENET_DMA_RING_PROD_INDEX, 1);

    genet_socket_read_all(sockets[0], &framed_length,
                          sizeof(framed_length));
    g_assert_cmpuint(ntohl(framed_length), ==, sizeof(packet));
    genet_socket_read_all(sockets[0], received, sizeof(received));
    g_assert_cmpmem(received, sizeof(received), packet, sizeof(packet));
    g_assert_cmpuint(qtest_readl(qts, BCM2711_GENET_BASE + GENET_TDMA_REG +
                                 GENET_DMA_RING_CONS_INDEX), ==, 1);
    g_assert_true(qtest_readl(qts, BCM2711_GENET_BASE + GENET_INTRL2_1 +
                              GENET_INTRL2_CPU_STAT) &
                  GENET_IRQ1_TX_RING0);
    g_assert_cmpuint(qtest_readl(qts, BCM2711_GENET_BASE +
                                 GENET_UMAC_MIB_TX_START), ==, 1);
    g_assert_cmpuint(qtest_readl(qts, BCM2711_GENET_BASE +
                                 GENET_UMAC_MIB_TX_PKT), ==, 1);
    g_assert_cmpuint(qtest_readl(qts, BCM2711_GENET_BASE +
                                 GENET_UMAC_MIB_TX_BYTES), ==,
                     sizeof(packet) + 4);
    g_assert_cmpuint(qtest_readl(qts, BCM2711_GENET_BASE +
                                 GENET_UMAC_MIB_TX_GOOD_PKT), ==, 1);
    g_assert_cmpuint(qtest_readl(qts, BCM2711_GENET_BASE +
                                 GENET_UMAC_MIB_TX_UNICAST), ==, 1);

    qtest_writel(qts, BCM2711_GENET_BASE + GENET_RDMA_REG +
                 GENET_DMA_RING_BUF_SIZE, (4 << 16) | 2048);
    qtest_writel(qts, BCM2711_GENET_BASE + GENET_RDMA_REG +
                 GENET_DMA_RING_START, 0);
    qtest_writel(qts, BCM2711_GENET_BASE + GENET_RDMA_DESC + 4, 0x110000);
    qtest_writel(qts, BCM2711_GENET_BASE + GENET_RDMA_DESC + 8, 0);
    qtest_writel(qts, BCM2711_GENET_BASE + GENET_RDMA_REG +
                 GENET_DMA_COMMON + GENET_DMA_CTRL, 3);
    qtest_writel(qts, BCM2711_GENET_BASE + GENET_INTRL2_1 +
                 GENET_INTRL2_CPU_MASK_CLEAR, GENET_IRQ1_RX_RING0);
    qtest_writel(qts, BCM2711_GENET_BASE + GENET_UMAC_MDF_ADDR, 0x5254);
    qtest_writel(qts, BCM2711_GENET_BASE + GENET_UMAC_MDF_ADDR + 4,
                 0x00123456);
    qtest_writel(qts, BCM2711_GENET_BASE + GENET_UMAC_MDF_CTRL, BIT(16));

    memcpy(rejected, packet, sizeof(rejected));
    rejected[5] ^= 1;
    framed_length = htonl(sizeof(rejected));
    g_assert_cmpint(send(sockets[0], &framed_length, sizeof(framed_length), 0),
                    ==, sizeof(framed_length));
    g_assert_cmpint(send(sockets[0], rejected, sizeof(rejected), 0), ==,
                    sizeof(rejected));
    for (unsigned int attempt = 0; attempt < 1000; attempt++) {
        if (qtest_readl(qts, BCM2711_GENET_BASE +
                        GENET_UMAC_MDF_ERR_CNT) == 1) {
            break;
        }
        qtest_clock_step(qts, 1000);
        g_usleep(1000);
    }
    g_assert_cmpuint(qtest_readl(qts, BCM2711_GENET_BASE +
                                 GENET_UMAC_MDF_ERR_CNT), ==, 1);
    g_assert_cmpuint(qtest_readl(qts, BCM2711_GENET_BASE + GENET_RDMA_REG +
                                 GENET_RDMA_RING_PROD_INDEX), ==, 0);

    framed_length = htonl(sizeof(packet));
    g_assert_cmpint(send(sockets[0], &framed_length, sizeof(framed_length), 0),
                    ==, sizeof(framed_length));
    g_assert_cmpint(send(sockets[0], packet, sizeof(packet), 0), ==,
                    sizeof(packet));
    for (unsigned int attempt = 0; attempt < 1000; attempt++) {
        if (qtest_readl(qts, BCM2711_GENET_BASE + GENET_RDMA_REG +
                        GENET_RDMA_RING_PROD_INDEX) == 1) {
            break;
        }
        qtest_clock_step(qts, 1000);
        g_usleep(1000);
    }
    g_assert_cmpuint(qtest_readl(qts, BCM2711_GENET_BASE + GENET_RDMA_REG +
                                 GENET_RDMA_RING_PROD_INDEX), ==, 1);
    qtest_memread(qts, 0x110000, rx_status, sizeof(rx_status));
    g_assert_cmpuint(ldl_le_p(rx_status) >> 16, ==,
                     sizeof(packet) + GENET_DMA_RX_PREFIX_SIZE);
    g_assert_cmphex(ldl_le_p(rx_status) & (GENET_DMA_SOP | GENET_DMA_EOP),
                    ==, GENET_DMA_SOP | GENET_DMA_EOP);
    qtest_memread(qts, 0x110000 + GENET_DMA_RX_PREFIX_SIZE,
                  rx_packet, sizeof(rx_packet));
    g_assert_cmpmem(rx_packet, sizeof(rx_packet), packet, sizeof(packet));
    g_assert_true(qtest_readl(qts, BCM2711_GENET_BASE + GENET_INTRL2_1 +
                              GENET_INTRL2_CPU_STAT) &
                  GENET_IRQ1_RX_RING0);
    g_assert_cmpuint(qtest_readl(qts, BCM2711_GENET_BASE +
                                 GENET_UMAC_MIB_RX_START), ==, 1);
    g_assert_cmpuint(qtest_readl(qts, BCM2711_GENET_BASE +
                                 GENET_UMAC_MIB_RX_PKT), ==, 1);
    g_assert_cmpuint(qtest_readl(qts, BCM2711_GENET_BASE +
                                 GENET_UMAC_MIB_RX_BYTES), ==,
                     sizeof(packet) + 4);
    g_assert_cmpuint(qtest_readl(qts, BCM2711_GENET_BASE +
                                 GENET_UMAC_MIB_RX_GOOD_PKT), ==, 1);
    g_assert_cmpuint(qtest_readl(qts, BCM2711_GENET_BASE +
                                 GENET_UMAC_MIB_RX_UNICAST), ==, 1);

    qtest_writel(qts, BCM2711_GENET_BASE + GENET_RDMA_REG +
                 GENET_DMA_RING_PROD_INDEX, 1);
    qtest_writel(qts, BCM2711_GENET_BASE + GENET_RDMA_DESC + 12 + 4,
                 0x120000);
    qtest_writel(qts, BCM2711_GENET_BASE + GENET_RDMA_DESC + 12 + 8, 0);
    memcpy(multicast, packet, sizeof(multicast));
    memcpy(multicast, "\x01\x00\x5e\x00\x00\x01", 6);
    qtest_writel(qts, BCM2711_GENET_BASE + GENET_UMAC_MDF_ADDR + 8,
                 0x0100);
    qtest_writel(qts, BCM2711_GENET_BASE + GENET_UMAC_MDF_ADDR + 12,
                 0x5e000001);
    qtest_writel(qts, BCM2711_GENET_BASE + GENET_UMAC_MDF_CTRL,
                 BIT(16) | BIT(15));
    genet_socket_send_packet(sockets[0], multicast, sizeof(multicast));
    genet_wait_register(qts, BCM2711_GENET_BASE + GENET_RDMA_REG +
                        GENET_RDMA_RING_PROD_INDEX, 2);
    g_assert_cmpuint(qtest_readl(qts, BCM2711_GENET_BASE +
                                 GENET_UMAC_MIB_RX_MULTICAST), ==, 1);

    qtest_writel(qts, BCM2711_GENET_BASE + GENET_RDMA_REG +
                 GENET_DMA_RING_PROD_INDEX, 2);
    qtest_writel(qts, BCM2711_GENET_BASE + GENET_RDMA_DESC + 24 + 4,
                 0x130000);
    qtest_writel(qts, BCM2711_GENET_BASE + GENET_RDMA_DESC + 24 + 8, 0);
    memcpy(broadcast, packet, sizeof(broadcast));
    memset(broadcast, 0xff, 6);
    genet_socket_send_packet(sockets[0], broadcast, sizeof(broadcast));
    genet_wait_register(qts, BCM2711_GENET_BASE + GENET_UMAC_MDF_ERR_CNT, 2);
    g_assert_cmpuint(qtest_readl(qts, BCM2711_GENET_BASE + GENET_RDMA_REG +
                                 GENET_RDMA_RING_PROD_INDEX), ==, 2);
    qtest_writel(qts, BCM2711_GENET_BASE + GENET_UMAC_MDF_ADDR + 16,
                 0xffff);
    qtest_writel(qts, BCM2711_GENET_BASE + GENET_UMAC_MDF_ADDR + 20,
                 0xffffffff);
    qtest_writel(qts, BCM2711_GENET_BASE + GENET_UMAC_MDF_CTRL,
                 BIT(16) | BIT(15) | BIT(14));
    genet_socket_send_packet(sockets[0], broadcast, sizeof(broadcast));
    genet_wait_register(qts, BCM2711_GENET_BASE + GENET_RDMA_REG +
                        GENET_RDMA_RING_PROD_INDEX, 3);
    g_assert_cmpuint(qtest_readl(qts, BCM2711_GENET_BASE +
                                 GENET_UMAC_MIB_RX_BROADCAST), ==, 1);

    qtest_writel(qts, BCM2711_GENET_BASE + GENET_RDMA_REG +
                 GENET_DMA_RING_PROD_INDEX, 3);
    qtest_writel(qts, BCM2711_GENET_BASE + GENET_RDMA_DESC + 36 + 4,
                 0x140000);
    qtest_writel(qts, BCM2711_GENET_BASE + GENET_RDMA_DESC + 36 + 8, 0);
    qtest_writel(qts, BCM2711_GENET_BASE + GENET_UMAC_MDF_CTRL, 0);
    qtest_writel(qts, BCM2711_GENET_BASE + GENET_UMAC_CMD,
                 3 | GENET_UMAC_CMD_PROMISC);
    genet_socket_send_packet(sockets[0], rejected, sizeof(rejected));
    genet_wait_register(qts, BCM2711_GENET_BASE + GENET_RDMA_REG +
                        GENET_RDMA_RING_PROD_INDEX, 4);
    g_assert_cmpuint(qtest_readl(qts, BCM2711_GENET_BASE +
                                 GENET_UMAC_MIB_RX_PKT), ==, 4);
    g_assert_cmpuint(qtest_readl(qts, BCM2711_GENET_BASE +
                                 GENET_UMAC_MIB_RX_BYTES), ==,
                     4 * (sizeof(packet) + 4));
    g_assert_cmpuint(qtest_readl(qts, BCM2711_GENET_BASE +
                                 GENET_UMAC_MIB_RX_GOOD_PKT), ==, 4);
    g_assert_cmpuint(qtest_readl(qts, BCM2711_GENET_BASE +
                                 GENET_UMAC_MIB_RX_UNICAST), ==, 2);

    qtest_writel(qts, BCM2711_GENET_BASE + GENET_UMAC_MIB_CTRL,
                 GENET_MIB_RESET_RX | GENET_MIB_RESET_TX);
    g_assert_cmpuint(qtest_readl(qts, BCM2711_GENET_BASE +
                                 GENET_UMAC_MIB_RX_PKT), ==, 0);
    g_assert_cmpuint(qtest_readl(qts, BCM2711_GENET_BASE +
                                 GENET_UMAC_MIB_TX_PKT), ==, 0);
    qtest_writel(qts, BCM2711_GENET_BASE + GENET_UMAC_MIB_CTRL, 0);
    qtest_writel(qts, BCM2711_GENET_BASE + GENET_UMAC_MDF_ERR_CNT, 0);
    g_assert_cmpuint(qtest_readl(qts, BCM2711_GENET_BASE +
                                 GENET_UMAC_MDF_ERR_CNT), ==, 0);

    qtest_quit(qts);
    close(sockets[0]);
}
#endif
#ifndef _WIN32
void test_bcm2711_genet_dma_faults(void)
{
    static const char genet_path[] = "/machine/soc/peripherals/genet";
    static const uint8_t packet[60] = {
        0x52, 0x54, 0x00, 0x12, 0x34, 0x56,
        0x52, 0x54, 0x00, 0x65, 0x43, 0x21,
        0x08, 0x00, 0x45, 0x00, 0x00, 0x2e,
        0x00, 0x01, 0x00, 0x00, 0x40, 0x11,
        0x00, 0x00, 0xc0, 0x00, 0x02, 0x01,
        0xc0, 0x00, 0x02, 0x02, 0x04, 0xd2,
        0x16, 0x2e, 0x00, 0x1a, 0x00, 0x00,
        'G', 'E', 'N', 'E', 'T', '-', 'F', 'A', 'U', 'L', 'T', 0, 0, 0,
        0, 0,
    };
    static const char tx_destination[] =
        "-M raspi4b "
        "-global bcm2711-genet.dma-error=1 "
        "-global bcm2711-genet.dma-error-after=0 "
        "-global bcm2711-genet.dma-error-count=1";
    uint8_t tx_buffer[64 + sizeof(packet)] = { 0 };
    uint8_t received[sizeof(packet)];
    uint8_t rx_packet[sizeof(packet)];
    uint32_t framed_length;
    struct pollfd pfd;
    QTestState *source;
    QTestState *destination;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    int sockets[2];

    memcpy(tx_buffer + 64, packet, sizeof(packet));
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    source = qtest_initf(
        "%s -netdev socket,fd=%d,id=net0 "
        "-global bcm2711-genet.netdev=net0", tx_destination, sockets[1]);
    close(sockets[1]);
    qtest_writel(source, BCM2711_GENET_BASE + GENET_UMAC_CMD, 3);
    qtest_writel(source, BCM2711_GENET_BASE + GENET_TDMA_REG +
                 GENET_DMA_RING_BUF_SIZE, (4 << 16) | 2048);
    qtest_writel(source, BCM2711_GENET_BASE + GENET_TDMA_REG +
                 GENET_DMA_RING_START, 0);
    qtest_writel(source, BCM2711_GENET_BASE + GENET_TDMA_REG +
                 GENET_DMA_COMMON + GENET_DMA_CTRL, 3);
    qtest_memwrite(source, 0x100000, tx_buffer, sizeof(tx_buffer));
    qtest_memwrite(source, 0x101000, tx_buffer, sizeof(tx_buffer));
    for (unsigned int descriptor = 0; descriptor < 2; descriptor++) {
        uint64_t desc = BCM2711_GENET_BASE + GENET_TDMA_DESC +
                        descriptor * 12;

        qtest_writel(source, desc,
                     (sizeof(tx_buffer) << 16) |
                     GENET_DMA_SOP | GENET_DMA_EOP);
        qtest_writel(source, desc + 4, 0x100000 + descriptor * 0x1000);
        qtest_writel(source, desc + 8, 0);
    }
    qtest_writel(source, BCM2711_GENET_BASE + GENET_TDMA_REG +
                 GENET_DMA_RING_PROD_INDEX, 1);
    g_assert_cmpuint(qtest_readl(source, BCM2711_GENET_BASE +
                                 GENET_TDMA_REG +
                                 GENET_DMA_RING_CONS_INDEX), ==, 1);
    g_assert_true(qtest_readl(source, BCM2711_GENET_BASE +
                              GENET_TDMA_DESC) & GENET_DMA_TX_UNDERRUN);
    g_assert_true(qtest_readl(source, BCM2711_GENET_BASE + GENET_INTRL2_0 +
                              GENET_INTRL2_CPU_STAT) &
                  GENET_IRQ_TBUF_UNDERRUN);
    g_assert_cmpuint(qom_path_get_uint64(source, genet_path,
                                         "dma-errors-injected"), ==, 1);
    g_assert_cmpuint(qom_path_get_uint64(source, genet_path,
                                         "dma-bytes-transferred"), ==, 0);
    pfd = (struct pollfd) { .fd = sockets[0], .events = POLLIN };
    g_assert_cmpint(poll(&pfd, 1, 10), ==, 0);

    qtest_writel(source, BCM2711_GENET_BASE + GENET_TDMA_REG +
                 GENET_DMA_RING_PROD_INDEX, 2);
    genet_socket_read_all(sockets[0], &framed_length, sizeof(framed_length));
    g_assert_cmpuint(ntohl(framed_length), ==, sizeof(packet));
    genet_socket_read_all(sockets[0], received, sizeof(received));
    g_assert_cmpmem(received, sizeof(received), packet, sizeof(packet));
    g_assert_cmpuint(qom_path_get_uint64(source, genet_path,
                                         "dma-bytes-transferred"), ==,
                     sizeof(packet));
    qtest_system_reset(source);
    g_assert_cmpuint(qom_path_get_uint64(source, genet_path,
                                         "dma-errors-injected"), ==, 1);
    g_assert_cmpuint(qom_path_get_uint64(source, genet_path,
                                         "dma-bytes-transferred"), ==,
                     sizeof(packet));

    destination = migrate_to_new_qtest(source, tx_destination,
                                       &migration_dir, &migration_socket);
    g_assert_cmpuint(qom_path_get_uint64(destination, genet_path,
                                         "dma-errors-injected"), ==, 1);
    g_assert_cmpuint(qom_path_get_uint64(destination, genet_path,
                                         "dma-bytes-transferred"), ==,
                     sizeof(packet));
    qtest_quit(source);
    qtest_quit(destination);
    close(sockets[0]);
    unlink(migration_socket);
    rmdir(migration_dir);

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    source = qtest_initf(
        "-M raspi4b -netdev socket,fd=%d,id=net0 "
        "-global bcm2711-genet.netdev=net0 "
        "-global bcm2711-genet.dma-error=2 "
        "-global bcm2711-genet.dma-error-after=0 "
        "-global bcm2711-genet.dma-error-count=1", sockets[1]);
    close(sockets[1]);
    qtest_writel(source, BCM2711_GENET_BASE + GENET_UMAC_CMD, 3);
    qtest_writel(source, BCM2711_GENET_BASE + GENET_RDMA_REG +
                 GENET_DMA_RING_BUF_SIZE, (4 << 16) | 2048);
    qtest_writel(source, BCM2711_GENET_BASE + GENET_RDMA_REG +
                 GENET_DMA_RING_START, 0);
    qtest_writel(source, BCM2711_GENET_BASE + GENET_RDMA_DESC + 4, 0x110000);
    qtest_writel(source, BCM2711_GENET_BASE + GENET_RDMA_DESC + 8, 0);
    qtest_writel(source, BCM2711_GENET_BASE + GENET_RDMA_REG +
                 GENET_DMA_COMMON + GENET_DMA_CTRL, 3);
    qtest_writel(source, BCM2711_GENET_BASE + GENET_UMAC_MDF_ADDR, 0x5254);
    qtest_writel(source, BCM2711_GENET_BASE + GENET_UMAC_MDF_ADDR + 4,
                 0x00123456);
    qtest_writel(source, BCM2711_GENET_BASE + GENET_UMAC_MDF_CTRL, BIT(16));
    genet_socket_send_packet(sockets[0], packet, sizeof(packet));
    genet_wait_register(source, BCM2711_GENET_BASE + GENET_RBUF_ERR_CNT, 1);
    g_assert_cmpuint(qtest_readl(source, BCM2711_GENET_BASE +
                                 GENET_RBUF_OVFL_CNT), ==, 1);
    g_assert_true(qtest_readl(source, BCM2711_GENET_BASE + GENET_INTRL2_0 +
                              GENET_INTRL2_CPU_STAT) &
                  GENET_IRQ_RBUF_OVERFLOW);
    g_assert_cmpuint(qtest_readl(source, BCM2711_GENET_BASE + GENET_RDMA_REG +
                                 GENET_RDMA_RING_PROD_INDEX), ==, 0);
    g_assert_cmpuint(qom_path_get_uint64(source, genet_path,
                                         "dma-errors-injected"), ==, 1);
    g_assert_cmpuint(qom_path_get_uint64(source, genet_path,
                                         "dma-bytes-transferred"), ==, 0);

    genet_socket_send_packet(sockets[0], packet, sizeof(packet));
    genet_wait_register(source, BCM2711_GENET_BASE + GENET_RDMA_REG +
                        GENET_RDMA_RING_PROD_INDEX, 1);
    qtest_memread(source, 0x110000 + GENET_DMA_RX_PREFIX_SIZE,
                  rx_packet, sizeof(rx_packet));
    g_assert_cmpmem(rx_packet, sizeof(rx_packet), packet, sizeof(packet));
    g_assert_cmpuint(qom_path_get_uint64(source, genet_path,
                                         "dma-bytes-transferred"), ==,
                     sizeof(packet) + GENET_DMA_RX_PREFIX_SIZE);
    qtest_system_reset(source);
    g_assert_cmpuint(qom_path_get_uint64(source, genet_path,
                                         "dma-errors-injected"), ==, 1);
    qtest_quit(source);
    close(sockets[0]);
}
#endif
void test_bcm2711_thermal_registers(void)
{
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    QTestState *qts = qtest_init("-M raspi4b");
    QTestState *destination;
    uint32_t status = qtest_readl(
        qts, BCM2711_AVS_BASE + BCM2711_AVS_TEMP_STATUS);

    g_assert_cmphex(status & BCM2711_AVS_TEMP_VALID, ==,
                    BCM2711_AVS_TEMP_VALID);
    g_assert_cmpuint(status & 0x3ff, ==, 791);
    g_assert_cmphex(qtest_readl(qts, BCM2711_AVS_BASE + 0x100), ==, 0);
    qtest_writel(qts, BCM2711_AVS_BASE + BCM2711_AVS_TEMP_STATUS,
                 0xffffffff);
    g_assert_cmphex(qtest_readl(
                        qts, BCM2711_AVS_BASE + BCM2711_AVS_TEMP_STATUS),
                    ==, status);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(
                        qts, BCM2711_AVS_BASE + BCM2711_AVS_TEMP_STATUS),
                    ==, status);
    g_assert_cmpint(qom_get_int32(
                        qts, "thermal-temperature-millicelsius"), ==, 25000);
    g_assert_true(qom_get_bool(qts, "thermal-sensor-valid"));
    qom_set_int32(qts, "thermal-temperature-millicelsius", 80000);
    status = qtest_readl(
        qts, BCM2711_AVS_BASE + BCM2711_AVS_TEMP_STATUS);
    g_assert_cmpuint(status & 0x3ff, ==, 678);
    qom_set_bool(qts, "thermal-sensor-valid", false);
    g_assert_cmphex(qtest_readl(
                        qts, BCM2711_AVS_BASE + BCM2711_AVS_TEMP_STATUS),
                    ==, 0);
    qom_set_bool(qts, "thermal-sensor-valid", true);
    qtest_quit(qts);

    qts = qtest_init("-M raspi4b,thermal-sensor-valid=off");
    g_assert_cmphex(qtest_readl(
                        qts, BCM2711_AVS_BASE + BCM2711_AVS_TEMP_STATUS),
                    ==, 0);
    destination = migrate_to_new_qtest(
        qts,
        "-M raspi4b,thermal-temperature-millicelsius=80000",
        &migration_dir, &migration_socket);
    qtest_quit(qts);
    g_assert_cmphex(qtest_readl(
                        destination,
                        BCM2711_AVS_BASE + BCM2711_AVS_TEMP_STATUS),
                    ==, 0);
    g_assert_false(qom_get_bool(destination, "thermal-sensor-valid"));
    qtest_system_reset(destination);
    g_assert_cmphex(qtest_readl(
                        destination,
                        BCM2711_AVS_BASE + BCM2711_AVS_TEMP_STATUS),
                    ==, 0);
    qtest_quit(destination);
    unlink(migration_socket);
    rmdir(migration_dir);
    g_clear_pointer(&migration_socket, g_free);
    g_clear_pointer(&migration_dir, g_free);

    qts = qtest_init(
        "-M raspi4b,thermal-temperature-millicelsius=80000");
    status = qtest_readl(qts,
                         BCM2711_AVS_BASE + BCM2711_AVS_TEMP_STATUS);
    g_assert_cmphex(status & BCM2711_AVS_TEMP_VALID, ==,
                    BCM2711_AVS_TEMP_VALID);
    g_assert_cmpuint(status & 0x3ff, ==, 678);
    g_assert_cmpint(qom_get_int32(
                        qts,
                        "thermal-temperature-millicelsius"), ==, 80000);
    destination = migrate_to_new_qtest(
        qts,
        "-M raspi4b,thermal-sensor-valid=off",
        &migration_dir, &migration_socket);
    qtest_quit(qts);
    status = qtest_readl(
        destination, BCM2711_AVS_BASE + BCM2711_AVS_TEMP_STATUS);
    g_assert_cmphex(status & BCM2711_AVS_TEMP_VALID, ==,
                    BCM2711_AVS_TEMP_VALID);
    g_assert_cmpuint(status & 0x3ff, ==, 678);
    g_assert_cmpint(qom_get_int32(
                        destination,
                        "thermal-temperature-millicelsius"), ==, 80000);
    qtest_system_reset(destination);
    status = qtest_readl(
        destination, BCM2711_AVS_BASE + BCM2711_AVS_TEMP_STATUS);
    g_assert_cmphex(status & BCM2711_AVS_TEMP_VALID, ==,
                    BCM2711_AVS_TEMP_VALID);
    g_assert_cmpuint(status & 0x3ff, ==, 678);
    qtest_quit(destination);
    unlink(migration_socket);
    rmdir(migration_dir);
}
void test_bcm2711_firmware_mailbox_structure(void)
{
    const char *machines[] = { "raspi4b", "raspi-cm4" };

    for (size_t index = 0; index < ARRAY_SIZE(machines); index++) {
        g_autofree char *command = g_strdup_printf(
            "-M %s", machines[index]);
        QTestState *qts = qtest_init(command);
        uint8_t request[1056];

        memset(request, 0xa5, sizeof(request));
        stl_le_p(request, 8);
        stl_le_p(request + 4, 0);
        bcm2711_property_raw_exchange(qts, request, sizeof(request));
        g_assert_cmphex((uint32_t)ldl_le_p(request + 4), ==, 0x80000001);

        memset(request, 0xa5, sizeof(request));
        stl_le_p(request, 24);
        stl_le_p(request + 4, 0);
        stl_le_p(request + 8, RPI_FWREQ_GET_BOARD_REVISION);
        stl_le_p(request + 12, 16);
        stl_le_p(request + 16, 0);
        bcm2711_property_raw_exchange(qts, request, sizeof(request));
        g_assert_cmphex((uint32_t)ldl_le_p(request + 4), ==, 0x80000001);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 16), ==, 0);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 20), ==, 0xa5a5a5a5);

        memset(request, 0xa5, sizeof(request));
        stl_le_p(request, 24);
        stl_le_p(request + 4, 0);
        stl_le_p(request + 8, RPI_FWREQ_GET_BOARD_REVISION);
        stl_le_p(request + 12, 4);
        stl_le_p(request + 16, 0);
        stl_le_p(request + 20, 0x11223344);
        bcm2711_property_raw_exchange(qts, request, sizeof(request));
        g_assert_cmphex((uint32_t)ldl_le_p(request + 4), ==, 0x80000001);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 16), ==, 0);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 20), ==, 0x11223344);

        memset(request, 0xa5, sizeof(request));
        stl_le_p(request, 0xfffffff0);
        stl_le_p(request + 4, 0);
        bcm2711_property_raw_exchange(qts, request, sizeof(request));
        g_assert_cmphex((uint32_t)ldl_le_p(request + 4), ==, 0x80000001);

        memset(request, 0xa5, sizeof(request));
        stl_le_p(request, 32);
        stl_le_p(request + 4, 0);
        stl_le_p(request + 8, RPI_FWREQ_GET_BOARD_MAC_ADDRESS);
        stl_le_p(request + 12, 6);
        stl_le_p(request + 16, 0);
        stl_le_p(request + 28, RPI_FWREQ_PROPERTY_END);
        bcm2711_property_raw_exchange(qts, request, sizeof(request));
        g_assert_cmphex((uint32_t)ldl_le_p(request + 4), ==, 0x80000000);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 16), ==, 0x80000006);
        g_assert_cmphex(request[26], ==, 0xa5);
        g_assert_cmphex(request[27], ==, 0xa5);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 28), ==,
                        RPI_FWREQ_PROPERTY_END);

        memset(request, 0xa5, sizeof(request));
        stl_le_p(request, 80);
        stl_le_p(request + 4, 0);
        stl_le_p(request + 8, RPI_FWREQ_GET_BOARD_REVISION);
        stl_le_p(request + 12, 1);
        stl_le_p(request + 16, 0);
        stl_le_p(request + 24, RPI_FWREQ_GET_BOARD_MAC_ADDRESS);
        stl_le_p(request + 28, 3);
        stl_le_p(request + 32, 0);
        stl_le_p(request + 40, RPI_FWREQ_GET_ARM_MEMORY);
        stl_le_p(request + 44, 6);
        stl_le_p(request + 48, 0);
        stl_le_p(request + 60, RPI_FWREQ_GET_BOARD_SERIAL);
        stl_le_p(request + 64, 4);
        stl_le_p(request + 68, 0);
        stl_le_p(request + 76, RPI_FWREQ_PROPERTY_END);
        bcm2711_property_raw_exchange(qts, request, sizeof(request));
        g_assert_cmphex((uint32_t)ldl_le_p(request + 4), ==, 0x80000000);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 16), ==, 0x80000004);
        g_assert_cmphex(request[20], ==,
                        (index ? CM4_BOARD_REVISION :
                         PI4_BOARD_REVISION) & 0xff);
        g_assert_cmphex(request[21], ==, 0xa5);
        g_assert_cmphex(request[22], ==, 0xa5);
        g_assert_cmphex(request[23], ==, 0xa5);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 24), ==,
                        RPI_FWREQ_GET_BOARD_MAC_ADDRESS);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 32), ==, 0x80000006);
        g_assert_cmphex(request[39], ==, 0xa5);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 40), ==,
                        RPI_FWREQ_GET_ARM_MEMORY);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 48), ==, 0x80000008);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 52), ==, 0);
        g_assert_cmphex(request[58], ==, 0xa5);
        g_assert_cmphex(request[59], ==, 0xa5);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 60), ==,
                        RPI_FWREQ_GET_BOARD_SERIAL);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 68), ==, 0x80000008);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 76), ==,
                        RPI_FWREQ_PROPERTY_END);

        /*
         * Request-size preflight is atomic: a valid side-effecting tag before
         * an undersized tag must not run.
         */
        memset(request, 0xa5, sizeof(request));
        stl_le_p(request, 48);
        stl_le_p(request + 4, 0);
        stl_le_p(request + 8, RPI_FWREQ_SET_REBOOT_FLAGS);
        stl_le_p(request + 12, 4);
        stl_le_p(request + 16, 0);
        stl_le_p(request + 20, 0x11223344);
        stl_le_p(request + 24, RPI_FWREQ_SET_CLOCK_STATE);
        stl_le_p(request + 28, 4);
        stl_le_p(request + 32, 0);
        stl_le_p(request + 36, RPI_FIRMWARE_ARM_CLK_ID);
        stl_le_p(request + 40, RPI_FWREQ_PROPERTY_END);
        bcm2711_property_raw_exchange(qts, request, sizeof(request));
        g_assert_cmphex((uint32_t)ldl_le_p(request + 4), ==, 0x80000001);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 16), ==, 0);
        {
            uint32_t reboot_flags = 0;

            bcm2711_property_call(
                qts, RPI_FWREQ_GET_REBOOT_FLAGS, &reboot_flags, 1);
            g_assert_cmphex(reboot_flags, ==, 0);
        }

        /* Dynamic requests must include every palette entry they declare. */
        memset(request, 0xa5, sizeof(request));
        stl_le_p(request, 36);
        stl_le_p(request + 4, 0);
        stl_le_p(request + 8, RPI_FWREQ_FRAMEBUFFER_SET_PALETTE);
        stl_le_p(request + 12, 12);
        stl_le_p(request + 16, 0);
        stl_le_p(request + 20, 0);
        stl_le_p(request + 24, 2);
        stl_le_p(request + 28, 0x00112233);
        stl_le_p(request + 32, RPI_FWREQ_PROPERTY_END);
        bcm2711_property_raw_exchange(qts, request, sizeof(request));
        g_assert_cmphex((uint32_t)ldl_le_p(request + 4), ==, 0x80000001);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 16), ==, 0);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 28), ==, 0x00112233);

        /*
         * Unsupported tags retain their request code and payload while later
         * supported tags are still processed. The documented board-model tag
         * returns the hardware-observed legacy value zero.
         */
        memset(request, 0xa5, sizeof(request));
        stl_le_p(request, 64);
        stl_le_p(request + 4, 0);
        stl_le_p(request + 8, 0x00abcdef);
        stl_le_p(request + 12, 5);
        stl_le_p(request + 16, 0);
        stl_le_p(request + 28, RPI_FWREQ_GET_BOARD_MODEL);
        stl_le_p(request + 32, 4);
        stl_le_p(request + 36, 0);
        stl_le_p(request + 44, RPI_FWREQ_GET_BOARD_REVISION);
        stl_le_p(request + 48, 4);
        stl_le_p(request + 52, 0);
        stl_le_p(request + 60, RPI_FWREQ_PROPERTY_END);
        bcm2711_property_raw_exchange(qts, request, sizeof(request));
        g_assert_cmphex((uint32_t)ldl_le_p(request + 4), ==, 0x80000000);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 16), ==, 0);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 20), ==, 0xa5a5a5a5);
        g_assert_cmphex(request[24], ==, 0xa5);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 36), ==, 0x80000004);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 40), ==, 0);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 52), ==, 0x80000004);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 56), ==,
                        index ? CM4_BOARD_REVISION : PI4_BOARD_REVISION);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 60), ==,
                        RPI_FWREQ_PROPERTY_END);

        /*
         * A short response is clipped to the two-byte value buffer while its
         * response header still reports the complete four-byte desired size.
         */
        memset(request, 0xa5, sizeof(request));
        stl_le_p(request, 28);
        stl_le_p(request + 4, 0);
        stl_le_p(request + 8, RPI_FWREQ_GET_BOARD_MODEL);
        stl_le_p(request + 12, 2);
        stl_le_p(request + 16, 0);
        stl_le_p(request + 24, RPI_FWREQ_PROPERTY_END);
        bcm2711_property_raw_exchange(qts, request, sizeof(request));
        g_assert_cmphex((uint32_t)ldl_le_p(request + 4), ==, 0x80000000);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 16), ==, 0x80000004);
        g_assert_cmphex(request[20], ==, 0);
        g_assert_cmphex(request[21], ==, 0);
        g_assert_cmphex(request[22], ==, 0xa5);
        g_assert_cmphex(request[23], ==, 0xa5);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 24), ==,
                        RPI_FWREQ_PROPERTY_END);

        {
            uint32_t palette[4] = {
                254, 2, 0x11223344, 0x55667788,
            };

            bcm2711_property_call_response(
                qts, RPI_FWREQ_FRAMEBUFFER_SET_PALETTE,
                palette, G_N_ELEMENTS(palette), 1);
            g_assert_cmphex(palette[0], ==, 0);

            palette[0] = 254;
            palette[1] = 2;
            palette[2] = 0xaabbccdd;
            palette[3] = 0xeeff0011;
            bcm2711_property_call_response(
                qts, RPI_FWREQ_FRAMEBUFFER_TEST_PALETTE,
                palette, G_N_ELEMENTS(palette), 1);
            g_assert_cmphex(palette[0], ==, 0);

            palette[0] = 255;
            palette[1] = 2;
            bcm2711_property_call_response(
                qts, RPI_FWREQ_FRAMEBUFFER_TEST_PALETTE,
                palette, G_N_ELEMENTS(palette), 1);
            g_assert_cmphex(palette[0], ==, 1);
        }

        memset(request, 0xa5, sizeof(request));
        stl_le_p(request, 32);
        stl_le_p(request + 4, 0);
        stl_le_p(request + 8, RPI_FWREQ_FRAMEBUFFER_GET_PALETTE);
        stl_le_p(request + 12, 6);
        stl_le_p(request + 16, 0);
        stl_le_p(request + 28, RPI_FWREQ_PROPERTY_END);
        bcm2711_property_raw_exchange(qts, request, sizeof(request));
        g_assert_cmphex((uint32_t)ldl_le_p(request + 4), ==, 0x80000000);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 16), ==, 0x80000400);
        g_assert_cmphex(request[26], ==, 0xa5);
        g_assert_cmphex(request[27], ==, 0xa5);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 28), ==,
                        RPI_FWREQ_PROPERTY_END);

        memset(request, 0xa5, sizeof(request));
        stl_le_p(request, 1048);
        stl_le_p(request + 4, 0);
        stl_le_p(request + 8, RPI_FWREQ_FRAMEBUFFER_GET_PALETTE);
        stl_le_p(request + 12, 1024);
        stl_le_p(request + 16, 0);
        stl_le_p(request + 1044, RPI_FWREQ_PROPERTY_END);
        bcm2711_property_raw_exchange(qts, request, sizeof(request));
        g_assert_cmphex((uint32_t)ldl_le_p(request + 4), ==, 0x80000000);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 16), ==, 0x80000400);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 1036), ==, 0x11223344);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 1040), ==, 0x55667788);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 1044), ==,
                        RPI_FWREQ_PROPERTY_END);

        qtest_quit(qts);
    }
}
void test_bcm2711_firmware_identity(void)
{
    static const uint8_t expected_mac[6] = {
        0xdc, 0xa6, 0x32, 0x12, 0x34, 0x56,
    };
    static const struct {
        const char *machine;
        uint32_t revision;
    } cases[] = {
        { "raspi4b", PI4_BOARD_REVISION },
        { "raspi-cm4", CM4_BOARD_REVISION },
    };
    const uint32_t serial = 0x12345678;

    for (size_t index = 0; index < ARRAY_SIZE(cases); index++) {
        g_autofree uint8_t *otp = make_otp_image(
            0, 0, cases[index].revision, false);
        g_autofree char *otp_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *migration_dir = NULL;
        g_autofree char *migration_socket = NULL;
        QTestState *source;
        /*
         * Set in phase 1 and read in phase 2; older compilers cannot see
         * that the assignment dominates the use, so start it defined.
         */
        QTestState *destination = NULL;

        stl_le_p(otp + (BCM2711_OTP_SERIAL_ROW - 1) * sizeof(uint32_t),
                 serial);
        stl_le_p(otp + BCM2711_OTP_SERIAL_ROW * sizeof(uint32_t), ~serial);
        write_temp_image("raspi4-identity-otp-XXXXXX", otp, OTP_SIZE,
                         &otp_path);
        command = g_strdup_printf(
            "-M %s,otp-drive=piotp "
            "-drive if=none,id=piotp,format=raw,file=%s,file.locking=off "
            "-global bcm2711-genet.mac=dc:a6:32:12:34:56 -nic none",
            cases[index].machine, otp_path);
        source = qtest_init(command);

        for (unsigned int phase = 0; phase < 3; phase++) {
            QTestState *qts = phase < 2 ? source : destination;
            g_autofree char *observed_mac = qom_get_string(
                qts, "boot-mac-address");
            uint32_t model[1] = { 0xffffffff };
            uint32_t revision[1] = { 0 };
            uint32_t mailbox_serial[2] = { 0 };
            uint32_t mac[2] = { 0 };

            g_assert_cmpstr(observed_mac, ==, "dc:a6:32:12:34:56");
            bcm2711_property_call(
                qts, RPI_FWREQ_GET_BOARD_MODEL, model,
                G_N_ELEMENTS(model));
            g_assert_cmphex(model[0], ==, 0);
            bcm2711_property_call(
                qts, RPI_FWREQ_GET_BOARD_REVISION, revision,
                G_N_ELEMENTS(revision));
            g_assert_cmphex(revision[0], ==, cases[index].revision);
            bcm2711_property_call(
                qts, RPI_FWREQ_GET_BOARD_SERIAL, mailbox_serial,
                G_N_ELEMENTS(mailbox_serial));
            g_assert_cmphex(mailbox_serial[0], ==, serial);
            g_assert_cmphex(mailbox_serial[1], ==, 0);
            bcm2711_property_call_response_bytes(
                qts, RPI_FWREQ_GET_BOARD_MAC_ADDRESS, mac,
                G_N_ELEMENTS(mac), sizeof(expected_mac));
            g_assert_cmpmem(mac, sizeof(expected_mac),
                            expected_mac, sizeof(expected_mac));

            if (!phase) {
                qtest_system_reset(source);
            } else if (phase == 1) {
                destination = migrate_to_new_qtest(
                    source, command, &migration_dir, &migration_socket);
            }
        }
        qtest_quit(source);
        qtest_quit(destination);
        unlink(migration_socket);
        rmdir(migration_dir);
        unlink(otp_path);
    }
}
void test_bcm2711_firmware_edid(void)
{
    static const char * const machines[] = { "raspi4b", "raspi-cm4" };
    uint8_t source_hdmi0[256];
    uint8_t source_hdmi1[128];
    uint8_t destination_hdmi0[256];
    uint8_t destination_hdmi1[128];

    make_edid(source_hdmi0, "DEL", "PRIMARY");
    source_hdmi0[126] = 1;
    finalize_edid_block(source_hdmi0);
    memset(source_hdmi0 + 128, 0, 128);
    source_hdmi0[128] = 0x02;
    source_hdmi0[129] = 0x03;
    source_hdmi0[130] = 0x04;
    source_hdmi0[131] = 0x55;
    finalize_edid_block(source_hdmi0 + 128);
    make_edid(source_hdmi1, "ACR", "SECONDARY");

    memcpy(destination_hdmi0, source_hdmi0, sizeof(destination_hdmi0));
    destination_hdmi0[127]++;
    make_edid(destination_hdmi1, "SAM", "RESET");

    for (size_t index = 0; index < ARRAY_SIZE(machines); index++) {
        g_autofree char *hdmi0_path = NULL;
        g_autofree char *hdmi1_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *migration_dir = NULL;
        g_autofree char *migration_socket = NULL;
        QTestState *source;
        QTestState *destination;
        uint8_t observed[128];

        write_temp_image("raspi4-mailbox-hdmi0-XXXXXX", source_hdmi0,
                         sizeof(source_hdmi0), &hdmi0_path);
        write_temp_image("raspi4-mailbox-hdmi1-XXXXXX", source_hdmi1,
                         sizeof(source_hdmi1), &hdmi1_path);
        command = g_strdup_printf(
            "-M %s,hdmi0-edid-file=%s,hdmi1-edid-file=%s",
            machines[index], hdmi0_path, hdmi1_path);
        source = qtest_init(command);

        g_assert_cmpuint(bcm2711_firmware_edid_call(
                             source, RPI_FWREQ_GET_EDID_BLOCK, 0, 0,
                             observed), ==, 0);
        g_assert_cmpmem(observed, sizeof(observed),
                        source_hdmi0, sizeof(observed));
        g_assert_cmpuint(bcm2711_firmware_edid_call(
                             source, RPI_FWREQ_GET_EDID_BLOCK, 1, 0,
                             observed), ==, 0);
        g_assert_cmpmem(observed, sizeof(observed),
                        source_hdmi0 + 128, sizeof(observed));
        g_assert_cmpuint(bcm2711_firmware_edid_call(
                             source, RPI_FWREQ_GET_EDID_BLOCK, 2, 0,
                             observed), ==, 1);
        g_assert_cmpmem(observed, sizeof(observed),
                        (uint8_t[128]) { 0 }, sizeof(observed));
        g_assert_cmpuint(bcm2711_firmware_edid_call(
                             source, RPI_FWREQ_GET_EDID_BLOCK_DISPLAY, 0, 1,
                             observed), ==, 1);
        g_assert_cmpmem(observed, sizeof(observed),
                        source_hdmi1, sizeof(observed));

        {
            uint8_t request[40];

            memset(request, 0xa5, sizeof(request));
            stl_le_p(request, sizeof(request));
            stl_le_p(request + 4, 0);
            stl_le_p(request + 8, RPI_FWREQ_GET_EDID_BLOCK);
            stl_le_p(request + 12, 16);
            stl_le_p(request + 16, 0);
            stl_le_p(request + 20, 1);
            stl_le_p(request + 36, RPI_FWREQ_PROPERTY_END);
            bcm2711_property_raw_exchange(
                source, request, sizeof(request));
            g_assert_cmphex((uint32_t)ldl_le_p(request + 4), ==,
                            0x80000000);
            g_assert_cmphex((uint32_t)ldl_le_p(request + 16), ==,
                            0x80000088);
            g_assert_cmpmem(request + 28, 8, source_hdmi0 + 128, 8);
            g_assert_cmphex((uint32_t)ldl_le_p(request + 36), ==,
                            RPI_FWREQ_PROPERTY_END);
        }
        {
            uint8_t request[28];

            memset(request, 0xa5, sizeof(request));
            stl_le_p(request, sizeof(request));
            stl_le_p(request + 4, 0);
            stl_le_p(request + 8, RPI_FWREQ_GET_EDID_BLOCK_DISPLAY);
            stl_le_p(request + 12, 4);
            stl_le_p(request + 16, 0);
            stl_le_p(request + 20, 0);
            stl_le_p(request + 24, RPI_FWREQ_PROPERTY_END);
            bcm2711_property_raw_exchange(
                source, request, sizeof(request));
            g_assert_cmphex((uint32_t)ldl_le_p(request + 4), ==,
                            0x80000001);
            g_assert_cmphex((uint32_t)ldl_le_p(request + 16), ==, 0);
            g_assert_cmphex((uint32_t)ldl_le_p(request + 20), ==, 0);
        }

        overwrite_image(hdmi0_path, destination_hdmi0,
                        sizeof(destination_hdmi0));
        overwrite_image(hdmi1_path, destination_hdmi1,
                        sizeof(destination_hdmi1));
        destination = migrate_to_new_qtest(
            source, command, &migration_dir, &migration_socket);

        g_assert_cmpuint(bcm2711_firmware_edid_call(
                             destination, RPI_FWREQ_GET_EDID_BLOCK, 1, 0,
                             observed), ==, 0);
        g_assert_cmpmem(observed, sizeof(observed),
                        source_hdmi0 + 128, sizeof(observed));
        g_assert_cmpuint(bcm2711_firmware_edid_call(
                             destination,
                             RPI_FWREQ_GET_EDID_BLOCK_DISPLAY, 0, 1,
                             observed), ==, 1);
        g_assert_cmpmem(observed, sizeof(observed),
                        source_hdmi1, sizeof(observed));

        qtest_system_reset(destination);
        g_assert_cmpuint(bcm2711_firmware_edid_call(
                             destination, RPI_FWREQ_GET_EDID_BLOCK, 0, 0,
                             observed), ==, 1);
        g_assert_cmpmem(observed, sizeof(observed),
                        (uint8_t[128]) { 0 }, sizeof(observed));
        g_assert_cmpuint(bcm2711_firmware_edid_call(
                             destination,
                             RPI_FWREQ_GET_EDID_BLOCK_DISPLAY, 0, 1,
                             observed), ==, 1);
        g_assert_cmpmem(observed, sizeof(observed),
                        destination_hdmi1, sizeof(observed));

        qtest_quit(source);
        qtest_quit(destination);
        unlink(migration_socket);
        rmdir(migration_dir);
        unlink(hdmi0_path);
        unlink(hdmi1_path);
    }
}
void test_bcm2711_hdmi_i2c_edid(void)
{
    static const char * const machines[] = { "raspi4b", "raspi-cm4" };
    static const uint64_t bsc_bases[] = {
        BCM2711_HDMI0_I2C_BASE,
        BCM2711_HDMI1_I2C_BASE,
    };
    static const uint64_t auto_bases[] = {
        BCM2711_HDMI0_AUTO_I2C_BASE,
        BCM2711_HDMI1_AUTO_I2C_BASE,
    };
    uint8_t source_hdmi0[384];
    uint8_t source_hdmi1[128];
    uint8_t reset_hdmi1[128];

    make_edid(source_hdmi0, "DEL", "DDC-PRIMARY");
    source_hdmi0[126] = 2;
    finalize_edid_block(source_hdmi0);
    for (unsigned int block = 1; block < 3; block++) {
        memset(source_hdmi0 + block * 128, 0, 128);
        source_hdmi0[block * 128] = 0x02;
        source_hdmi0[block * 128 + 1] = 0x03;
        source_hdmi0[block * 128 + 2] = 0x04;
        source_hdmi0[block * 128 + 3] = 0x50 + block;
        finalize_edid_block(source_hdmi0 + block * 128);
    }
    source_hdmi0[128 + 2] = 12;
    source_hdmi0[128 + 4] = (3 << 5) | 7;
    source_hdmi0[128 + 5] = 0xd8;
    source_hdmi0[128 + 6] = 0x5d;
    source_hdmi0[128 + 7] = 0xc4;
    source_hdmi0[128 + 8] = 1;
    source_hdmi0[128 + 9] = 120;
    source_hdmi0[128 + 10] = BIT(7) | BIT(6);
    source_hdmi0[128 + 11] = 0;
    finalize_edid_block(source_hdmi0 + 128);
    make_edid(source_hdmi1, "ACR", "DDC-SECONDARY");
    make_edid(reset_hdmi1, "SAM", "DDC-RESET");

    for (size_t index = 0; index < ARRAY_SIZE(machines); index++) {
        g_autofree char *hdmi0_path = NULL;
        g_autofree char *hdmi1_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *migration_dir = NULL;
        g_autofree char *migration_socket = NULL;
        uint8_t observed[384];
        QTestState *source;
        QTestState *destination;

        write_temp_image("raspi4-ddc-hdmi0-XXXXXX", source_hdmi0,
                         sizeof(source_hdmi0), &hdmi0_path);
        write_temp_image("raspi4-ddc-hdmi1-XXXXXX", source_hdmi1,
                         sizeof(source_hdmi1), &hdmi1_path);
        command = g_strdup_printf(
            "-M %s,hdmi0-edid-file=%s,hdmi1-edid-file=%s",
            machines[index], hdmi0_path, hdmi1_path);
        source = qtest_init(command);

        for (unsigned int port = 0; port < ARRAY_SIZE(bsc_bases); port++) {
            qtest_writel(source, auto_bases[port] + HDMI_I2C_AUTO_CONTROL0,
                         BIT(1));
            g_assert_cmphex(qtest_readl(
                                source,
                                auto_bases[port] + HDMI_I2C_AUTO_CONTROL0),
                            ==, BIT(1));
            g_assert_cmphex(qtest_readl(
                                source,
                                bsc_bases[port] + HDMI_I2C_CONTROL_HIGH),
                            ==, BIT(6));
        }

        bcm2711_hdmi_i2c_read(source, bsc_bases[0], 0, 0,
                              observed, 256);
        g_assert_cmpmem(observed, 256, source_hdmi0, 256);
        bcm2711_hdmi_i2c_read(source, bsc_bases[1], 0, 0,
                              observed, sizeof(source_hdmi1));
        g_assert_cmpmem(observed, sizeof(source_hdmi1),
                        source_hdmi1, sizeof(source_hdmi1));
        {
            uint8_t data[3] = { 0x20, 0xff, 0 };

            g_assert_false(bcm2711_hdmi_i2c_transfer(
                source, bsc_bases[1], 0x54, true, data, 1));
            data[0] = 0x01;
            g_assert_true(bcm2711_hdmi_i2c_transfer(
                source, bsc_bases[0], 0x54, false, data, 1));
            g_assert_true(bcm2711_hdmi_i2c_transfer(
                source, bsc_bases[0], 0x54, true, data, 2));
            g_assert_cmphex(data[0], ==, 1);
            g_assert_cmphex(data[1], ==, 0);

            data[0] = 0x02;
            data[1] = 0xff;
            g_assert_true(bcm2711_hdmi_i2c_transfer(
                source, bsc_bases[0], 0x54, false, data, 2));
            data[0] = 0x20;
            data[1] = 0xff;
            g_assert_true(bcm2711_hdmi_i2c_transfer(
                source, bsc_bases[0], 0x54, false, data, 2));
            data[0] = 0x02;
            g_assert_true(bcm2711_hdmi_i2c_transfer(
                source, bsc_bases[0], 0x54, false, data, 1));
            g_assert_true(bcm2711_hdmi_i2c_transfer(
                source, bsc_bases[0], 0x54, true, data, 1));
            g_assert_cmphex(data[0], ==, 1);
            data[0] = 0x20;
            g_assert_true(bcm2711_hdmi_i2c_transfer(
                source, bsc_bases[0], 0x54, false, data, 1));
            g_assert_true(bcm2711_hdmi_i2c_transfer(
                source, bsc_bases[0], 0x54, true, data, 2));
            g_assert_cmphex(data[0], ==, 3);
            g_assert_cmphex(data[1], ==, 1);

            data[0] = 0x30;
            data[1] = 0xff;
            g_assert_true(bcm2711_hdmi_i2c_transfer(
                source, bsc_bases[0], 0x54, false, data, 2));
            data[0] = 0x30;
            g_assert_true(bcm2711_hdmi_i2c_transfer(
                source, bsc_bases[0], 0x54, false, data, 1));
            g_assert_true(bcm2711_hdmi_i2c_transfer(
                source, bsc_bases[0], 0x54, true, data, 1));
            g_assert_cmphex(data[0], ==, 1);

            data[0] = 0x40;
            g_assert_true(bcm2711_hdmi_i2c_transfer(
                source, bsc_bases[0], 0x54, false, data, 1));
        }

        bcm2711_hdmi_i2c_set_pointer(source, bsc_bases[0], 1, 5);
        overwrite_image(hdmi1_path, reset_hdmi1, sizeof(reset_hdmi1));
        destination = migrate_to_new_qtest(
            source, command, &migration_dir, &migration_socket);
        {
            uint8_t data[2] = { 0 };

            g_assert_true(bcm2711_hdmi_i2c_transfer(
                destination, bsc_bases[0], 0x54, true, data, 1));
            g_assert_cmphex(data[0], ==, 0x0f);
            data[0] = 0x20;
            g_assert_true(bcm2711_hdmi_i2c_transfer(
                destination, bsc_bases[0], 0x54, false, data, 1));
            g_assert_true(bcm2711_hdmi_i2c_transfer(
                destination, bsc_bases[0], 0x54, true, data, 2));
            g_assert_cmphex(data[0], ==, 3);
            g_assert_cmphex(data[1], ==, 1);
            data[0] = 0x30;
            g_assert_true(bcm2711_hdmi_i2c_transfer(
                destination, bsc_bases[0], 0x54, false, data, 1));
            g_assert_true(bcm2711_hdmi_i2c_transfer(
                destination, bsc_bases[0], 0x54, true, data, 1));
            g_assert_cmphex(data[0], ==, 1);
        }
        g_assert_true(bcm2711_hdmi_i2c_transfer(
            destination, bsc_bases[0], 0x50, true, observed, 32));
        g_assert_cmpmem(observed, 32, source_hdmi0 + 256 + 5, 32);
        bcm2711_hdmi_i2c_read(destination, bsc_bases[1], 0, 0,
                              observed, sizeof(source_hdmi1));
        g_assert_cmpmem(observed, sizeof(source_hdmi1),
                        source_hdmi1, sizeof(source_hdmi1));

        qtest_system_reset(destination);
        {
            uint8_t data[2] = { 0x02, 0 };

            g_assert_true(bcm2711_hdmi_i2c_transfer(
                destination, bsc_bases[0], 0x54, false, data, 1));
            g_assert_true(bcm2711_hdmi_i2c_transfer(
                destination, bsc_bases[0], 0x54, true, data, 1));
            g_assert_cmphex(data[0], ==, 0);
            data[0] = 0x20;
            g_assert_true(bcm2711_hdmi_i2c_transfer(
                destination, bsc_bases[0], 0x54, false, data, 1));
            g_assert_true(bcm2711_hdmi_i2c_transfer(
                destination, bsc_bases[0], 0x54, true, data, 2));
            g_assert_cmphex(data[0], ==, 0);
            g_assert_cmphex(data[1], ==, 0);
            data[0] = 0x30;
            g_assert_true(bcm2711_hdmi_i2c_transfer(
                destination, bsc_bases[0], 0x54, false, data, 1));
            g_assert_true(bcm2711_hdmi_i2c_transfer(
                destination, bsc_bases[0], 0x54, true, data, 1));
            g_assert_cmphex(data[0], ==, 0);
        }
        bcm2711_hdmi_i2c_read(destination, bsc_bases[1], 0, 0,
                              observed, sizeof(reset_hdmi1));
        g_assert_cmpmem(observed, sizeof(reset_hdmi1),
                        reset_hdmi1, sizeof(reset_hdmi1));

        qtest_quit(source);
        qtest_quit(destination);
        unlink(migration_socket);
        rmdir(migration_dir);
        unlink(hdmi0_path);
        unlink(hdmi1_path);
    }
}
void test_bcm2711_hdmi_hotplug(void)
{
    static const char * const machines[] = { "raspi4b", "raspi-cm4" };
    static const char hdmi0_path_qom[] =
        "/machine/soc/peripherals/hdmi0";
    static const char hdmi1_path_qom[] =
        "/machine/soc/peripherals/hdmi1";
    uint8_t edid[128];
    uint8_t invalid_edid[128] = { 0 };

    make_edid(edid, "DEL", "HPD-PRIMARY");

    for (size_t index = 0; index < ARRAY_SIZE(machines); index++) {
        g_autofree char *edid_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *migration_dir = NULL;
        g_autofree char *migration_socket = NULL;
        QTestState *source;
        QTestState *destination;
        QDict *response;
        uint8_t byte = 0;

        write_temp_image("raspi4-hpd-edid-XXXXXX", edid, sizeof(edid),
                         &edid_path);
        command = g_strdup_printf("-M %s,hdmi0-edid-file=%s",
                                  machines[index], edid_path);
        source = qtest_init(command);

        response = qtest_qmp(
            source,
            "{ 'execute': 'qom-get', 'arguments': { "
            "'path': '/machine/soc/peripherals/aon-intr', "
            "'property': 'sysbus-irq[0]' } }");
        g_assert_cmpstr(qdict_get_str(response, "return"), ==,
                        "/machine/soc/gic/unnamed-gpio-in[96]");
        qobject_unref(response);
        response = qtest_qmp(
            source,
            "{ 'execute': 'qom-get', 'arguments': { "
            "'path': '/machine/soc/peripherals/hdmi0', "
            "'property': 'sysbus-irq[0]' } }");
        g_assert_cmpstr(
            qdict_get_str(response, "return"), ==,
            "/machine/soc/peripherals/aon-intr/unnamed-gpio-in[4]");
        qobject_unref(response);

        g_assert_true(qtest_qom_get_bool(
            source, hdmi0_path_qom, "connected"));
        g_assert_false(qtest_qom_get_bool(
            source, hdmi1_path_qom, "connected"));
        g_assert_cmphex(qtest_readl(
                            source,
                            BCM2711_HDMI0_BASE + BCM2711_HDMI_HOTPLUG),
                        ==, 1);
        g_assert_cmphex(qtest_readl(
                            source,
                            BCM2711_HDMI1_BASE + BCM2711_HDMI_HOTPLUG),
                        ==, 0);
        g_assert_cmphex(qtest_readl(
                            source,
                            BCM2711_AON_INTR_BASE +
                            BCM2711_AON_MASK_STATUS),
                        ==, UINT32_MAX);
        g_assert_cmphex(qtest_readl(
                            source,
                            BCM2711_AON_INTR_BASE + BCM2711_AON_STATUS),
                        ==, 0);
        g_assert_true(bcm2711_hdmi_i2c_transfer(
            source, BCM2711_HDMI0_I2C_BASE, 0x50, true, &byte, 1));

        qtest_writel(source, 0xff841290, BIT(0));
        qtest_qom_set_bool(source, hdmi0_path_qom, "connected", false);
        g_assert_cmphex(qtest_readl(
                            source,
                            BCM2711_HDMI0_BASE + BCM2711_HDMI_HOTPLUG),
                        ==, 0);
        g_assert_cmphex(qtest_readl(
                            source,
                            BCM2711_AON_INTR_BASE + BCM2711_AON_STATUS),
                        ==, BIT(5));
        g_assert_false(qtest_readl(source, 0xff841210) & BIT(0));
        g_assert_false(bcm2711_hdmi_i2c_transfer(
            source, BCM2711_HDMI0_I2C_BASE, 0x50, true, &byte, 1));

        qtest_writel(source,
                     BCM2711_AON_INTR_BASE + BCM2711_AON_MASK_CLEAR,
                     BIT(5));
        g_assert_true(qtest_readl(source, 0xff841210) & BIT(0));
        qtest_writel(source, BCM2711_AON_INTR_BASE + BCM2711_AON_CLEAR,
                     BIT(5));
        qtest_writel(source, 0xff841290, BIT(0));
        qtest_qom_set_bool(source, hdmi0_path_qom, "connected", true);
        g_assert_cmphex(qtest_readl(
                            source,
                            BCM2711_AON_INTR_BASE + BCM2711_AON_STATUS),
                        ==, BIT(4));
        g_assert_false(qtest_readl(source, 0xff841210) & BIT(0));
        qtest_writel(source,
                     BCM2711_AON_INTR_BASE + BCM2711_AON_MASK_CLEAR,
                     BIT(4));
        g_assert_true(qtest_readl(source, 0xff841210) & BIT(0));
        qtest_writel(source, BCM2711_AON_INTR_BASE + BCM2711_AON_CLEAR,
                     BIT(4));
        qtest_writel(source, 0xff841290, BIT(0));
        qtest_qom_set_bool(source, hdmi0_path_qom, "connected", false);
        g_assert_cmphex(qtest_readl(
                            source,
                            BCM2711_AON_INTR_BASE + BCM2711_AON_STATUS),
                        ==, BIT(5));

        destination = migrate_to_new_qtest(
            source, command, &migration_dir, &migration_socket);
        g_assert_false(qtest_qom_get_bool(
            destination, hdmi0_path_qom, "connected"));
        g_assert_cmphex(qtest_readl(
                            destination,
                            BCM2711_AON_INTR_BASE + BCM2711_AON_STATUS),
                        ==, BIT(5));
        g_assert_cmphex(qtest_readl(
                            destination,
                            BCM2711_AON_INTR_BASE +
                            BCM2711_AON_MASK_STATUS) &
                        (BIT(4) | BIT(5)), ==, 0);
        g_assert_false(bcm2711_hdmi_i2c_transfer(
            destination, BCM2711_HDMI0_I2C_BASE, 0x50, true, &byte, 1));

        qtest_system_reset(destination);
        g_assert_true(qtest_qom_get_bool(
            destination, hdmi0_path_qom, "connected"));
        g_assert_cmphex(qtest_readl(
                            destination,
                            BCM2711_AON_INTR_BASE + BCM2711_AON_STATUS),
                        ==, 0);
        g_assert_cmphex(qtest_readl(
                            destination,
                            BCM2711_AON_INTR_BASE +
                            BCM2711_AON_MASK_STATUS),
                        ==, UINT32_MAX);
        g_assert_true(bcm2711_hdmi_i2c_transfer(
            destination, BCM2711_HDMI0_I2C_BASE, 0x50, true, &byte, 1));

        overwrite_image(edid_path, invalid_edid, sizeof(invalid_edid));
        qtest_system_reset(destination);
        g_assert_false(qtest_qom_get_bool(
            destination, hdmi0_path_qom, "connected"));
        g_assert_cmphex(qtest_readl(
                            destination,
                            BCM2711_HDMI0_BASE + BCM2711_HDMI_HOTPLUG),
                        ==, 0);
        g_assert_false(bcm2711_hdmi_i2c_transfer(
            destination, BCM2711_HDMI0_I2C_BASE, 0x50, true, &byte, 1));

        qtest_quit(source);
        qtest_quit(destination);
        unlink(migration_socket);
        rmdir(migration_dir);
        unlink(edid_path);
    }
}
void test_bcm2711_hdmi_cec(void)
{
    static const char * const machines[] = { "raspi4b", "raspi-cm4" };
    static const char hdmi0_qom[] = "/machine/soc/peripherals/hdmi0";
    static const char hdmi1_qom[] = "/machine/soc/peripherals/hdmi1";
    uint8_t edid[128];

    make_edid(edid, "CEC", "DUAL-CEC");

    for (size_t index = 0; index < ARRAY_SIZE(machines); index++) {
        g_autofree char *edid0_path = NULL;
        g_autofree char *edid1_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *migration_dir = NULL;
        g_autofree char *migration_socket = NULL;
        QTestState *source;
        QTestState *destination;
        uint32_t control;

        write_temp_image("raspi4-cec0-edid-XXXXXX", edid, sizeof(edid),
                         &edid0_path);
        write_temp_image("raspi4-cec1-edid-XXXXXX", edid, sizeof(edid),
                         &edid1_path);
        command = g_strdup_printf(
            "-M %s,hdmi0-edid-file=%s,hdmi1-edid-file=%s",
            machines[index], edid0_path, edid1_path);
        source = qtest_init(command);

        g_assert_cmphex(qtest_readl(source, BCM2711_HDMI0_CEC_BASE +
                                           BCM2711_CEC_CNTRL_1), ==, 0);
        qtest_writel(source, BCM2711_HDMI0_CEC_BASE +
                            BCM2711_CEC_CNTRL_5,
                     BCM2711_CEC_TX_SW_RESET | BCM2711_CEC_RX_SW_RESET);
        qtest_writel(source, BCM2711_HDMI0_CEC_BASE +
                            BCM2711_CEC_CNTRL_5, 0);
        qtest_writel(source, BCM2711_HDMI0_CEC_BASE +
                            BCM2711_CEC_TX_DATA_1, 0x44332210);
        qtest_writel(source, BCM2711_HDMI0_CEC_BASE +
                            BCM2711_CEC_CNTRL_1,
                     (3 << BCM2711_CEC_MESSAGE_LENGTH_SHIFT) |
                     BCM2711_CEC_START_XMIT_BEGIN);
        qtest_clock_step(source, 50 * SCALE_MS);
        g_assert_cmphex(qtest_readl(
                            source, BCM2711_AON_INTR_BASE +
                                    BCM2711_AON_STATUS), ==, 0);
        qtest_clock_step(source, 60 * SCALE_MS);
        control = qtest_readl(source, BCM2711_HDMI0_CEC_BASE +
                                     BCM2711_CEC_CNTRL_1);
        g_assert_true(control & BCM2711_CEC_TX_EOM);
        g_assert_true(control & BCM2711_CEC_TX_STATUS_GOOD);
        g_assert_cmphex(qtest_readl(
                            source, BCM2711_AON_INTR_BASE +
                                    BCM2711_AON_STATUS), ==, BIT(0));

        qtest_writel(source, BCM2711_AON_INTR_BASE + BCM2711_AON_CLEAR,
                     BIT(0));
        qtest_writel(source, BCM2711_HDMI0_CEC_BASE +
                            BCM2711_CEC_CNTRL_1,
                     control & ~BCM2711_CEC_START_XMIT_BEGIN);
        qtest_qom_set_bool(source, hdmi0_qom, "cec-force-nack", true);
        qtest_writel(source, BCM2711_HDMI0_CEC_BASE +
                            BCM2711_CEC_CNTRL_1,
                     BCM2711_CEC_START_XMIT_BEGIN);
        qtest_clock_step(source, 10 * SCALE_MS);

        destination = migrate_to_new_qtest(
            source, command, &migration_dir, &migration_socket);
        qtest_clock_step(destination, 200 * SCALE_MS);
        control = qtest_readl(destination, BCM2711_HDMI0_CEC_BASE +
                                          BCM2711_CEC_CNTRL_1);
        g_assert_true(control & BCM2711_CEC_TX_EOM);
        g_assert_false(control & BCM2711_CEC_TX_STATUS_GOOD);
        g_assert_cmphex(qtest_readl(
                            destination, BCM2711_AON_INTR_BASE +
                                         BCM2711_AON_STATUS), ==, BIT(0));

        qtest_writel(destination,
                     BCM2711_AON_INTR_BASE + BCM2711_AON_CLEAR, BIT(0));
        qom_path_set_uint64(destination, hdmi0_qom, "cec-rx-data-low",
                            0x8877665544332211ULL);
        qom_path_set_uint32(destination, hdmi0_qom, "cec-rx-length", 8);
        qtest_qom_set_bool(destination, hdmi0_qom, "cec-rx-inject", true);
        control = qtest_readl(destination, BCM2711_HDMI0_CEC_BASE +
                                          BCM2711_CEC_CNTRL_1);
        g_assert_true(control & BCM2711_CEC_RX_EOM);
        g_assert_true(control & BCM2711_CEC_RX_STATUS_GOOD);
        g_assert_cmpuint((control >> BCM2711_CEC_REC_WRD_CNT_SHIFT) & 0xf,
                         ==, 7);
        g_assert_cmphex(qtest_readl(destination,
                                   BCM2711_HDMI0_CEC_BASE +
                                   BCM2711_CEC_RX_DATA_1),
                        ==, 0x44332211);
        g_assert_cmphex(qtest_readl(
                            destination, BCM2711_AON_INTR_BASE +
                                         BCM2711_AON_STATUS), ==, BIT(1));
        qtest_writel(destination, BCM2711_HDMI0_CEC_BASE +
                                  BCM2711_CEC_CNTRL_1,
                     control | BCM2711_CEC_CLEAR_RECEIVE_OFF);
        control = qtest_readl(destination, BCM2711_HDMI0_CEC_BASE +
                                          BCM2711_CEC_CNTRL_1);
        g_assert_false(control & BCM2711_CEC_RX_EOM);

        qtest_writel(destination,
                     BCM2711_AON_INTR_BASE + BCM2711_AON_CLEAR, BIT(1));
        qom_path_set_uint64(destination, hdmi1_qom, "cec-rx-data-low",
                            0x000000000000040fULL);
        qom_path_set_uint32(destination, hdmi1_qom, "cec-rx-length", 2);
        qtest_qom_set_bool(destination, hdmi1_qom, "cec-rx-inject", true);
        g_assert_cmphex(qtest_readl(
                            destination, BCM2711_AON_INTR_BASE +
                                         BCM2711_AON_STATUS), ==, BIT(7));
        g_assert_cmphex(qtest_readl(destination,
                                   BCM2711_HDMI1_CEC_BASE +
                                   BCM2711_CEC_RX_DATA_1) & 0xffff,
                        ==, 0x040f);

        qtest_writel(destination, BCM2711_HDMI1_CEC_BASE +
                                  BCM2711_CEC_CNTRL_1,
                     BCM2711_CEC_CLEAR_RECEIVE_OFF);
        qtest_writel(destination, BCM2711_HDMI1_CEC_BASE +
                                  BCM2711_CEC_CNTRL_1,
                     (15 << BCM2711_CEC_MESSAGE_LENGTH_SHIFT) |
                     BCM2711_CEC_START_XMIT_BEGIN);
        qtest_clock_step(destination, 10 * SCALE_MS);
        qtest_system_reset(destination);
        qtest_clock_step(destination, 500 * SCALE_MS);
        g_assert_cmphex(qtest_readl(destination,
                                   BCM2711_HDMI0_CEC_BASE +
                                   BCM2711_CEC_CNTRL_1), ==, 0);
        g_assert_cmphex(qtest_readl(destination,
                                   BCM2711_HDMI1_CEC_BASE +
                                   BCM2711_CEC_CNTRL_1), ==, 0);
        g_assert_cmphex(qtest_readl(
                            destination, BCM2711_AON_INTR_BASE +
                                         BCM2711_AON_STATUS), ==, 0);
        g_assert_cmphex(qtest_readl(
                            destination, BCM2711_AON_INTR_BASE +
                                         BCM2711_AON_MASK_STATUS),
                        ==, UINT32_MAX);

        qtest_quit(source);
        qtest_quit(destination);
        unlink(migration_socket);
        rmdir(migration_dir);
        unlink(edid0_path);
        unlink(edid1_path);
    }
}
void test_bcm2711_firmware_display_timing(void)
{
    static const char * const machines[] = { "raspi4b", "raspi-cm4" };
    uint8_t source_hdmi0[256] = { 0 };
    uint8_t source_hdmi1[128];
    uint8_t destination_hdmi0[256] = { 0 };
    uint8_t destination_hdmi1[128];
    uint8_t source_timing0[36];
    uint8_t source_timing1[36];
    uint8_t destination_timing0[36];
    uint8_t destination_timing1[36];

    make_timed_edid(source_hdmi0, "DEL", 14850, 1920, 280, 88, 44,
                    1080, 45, 4, 5);
    source_hdmi0[126] = 1;
    finalize_edid_block(source_hdmi0);
    source_hdmi0[128] = 0x02;
    source_hdmi0[129] = 0x03;
    source_hdmi0[130] = 8;
    source_hdmi0[132] = (3 << 5) | 3;
    source_hdmi0[133] = 0x03;
    source_hdmi0[134] = 0x0c;
    source_hdmi0[135] = 0x00;
    finalize_edid_block(source_hdmi0 + 128);
    make_timed_edid(source_hdmi1, "ACR", 7425, 1280, 370, 110, 40,
                    720, 30, 5, 5);
    make_timed_edid(destination_hdmi0, "SAM", 6500, 1024, 320, 24, 136,
                    768, 38, 3, 6);
    destination_hdmi0[126] = 1;
    finalize_edid_block(destination_hdmi0);
    destination_hdmi0[128] = 0x02;
    destination_hdmi0[129] = 0x03;
    destination_hdmi0[130] = 4;
    finalize_edid_block(destination_hdmi0 + 128);
    make_timed_edid(destination_hdmi1, "LGD", 4000, 800, 256, 40, 128,
                    600, 28, 1, 4);

    make_firmware_display_timing(source_timing0, 2, 148500,
                                 1920, 88, 44, 2200,
                                 1080, 4, 5, 1125, 0x23);
    make_firmware_display_timing(source_timing1, 7, 74250,
                                 1280, 110, 40, 1650,
                                 720, 5, 5, 750, 0x223);
    make_firmware_display_timing(destination_timing0, 2, 65000,
                                 1024, 24, 136, 1344,
                                 768, 3, 6, 806, 0x213);
    make_firmware_display_timing(destination_timing1, 7, 40000,
                                 800, 40, 128, 1056,
                                 600, 1, 4, 628, 0x213);

    for (size_t index = 0; index < ARRAY_SIZE(machines); index++) {
        g_autofree char *hdmi0_path = NULL;
        g_autofree char *hdmi1_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *migration_dir = NULL;
        g_autofree char *migration_socket = NULL;
        uint8_t current0[36];
        uint8_t current1[36];
        uint8_t observed[36];
        QTestState *source;
        QTestState *destination;

        memcpy(current0, source_timing0, sizeof(current0));
        memcpy(current1, source_timing1, sizeof(current1));
        write_temp_image("raspi4-timing-hdmi0-XXXXXX", source_hdmi0,
                         sizeof(source_hdmi0), &hdmi0_path);
        write_temp_image("raspi4-timing-hdmi1-XXXXXX", source_hdmi1,
                         sizeof(source_hdmi1), &hdmi1_path);
        command = g_strdup_printf(
            "-M %s,hdmi0-edid-file=%s,hdmi1-edid-file=%s",
            machines[index], hdmi0_path, hdmi1_path);
        source = qtest_init(command);

        bcm2711_firmware_display_timing_get(source, 2, observed);
        g_assert_cmpmem(observed, sizeof(observed),
                        current0, sizeof(current0));
        bcm2711_firmware_display_timing_get(source, 7, observed);
        g_assert_cmpmem(observed, sizeof(observed),
                        current1, sizeof(current1));
        bcm2711_firmware_display_timing_get(source, 99, observed);
        g_assert_cmpuint(observed[0], ==, 99);
        g_assert_cmpmem(observed + 1, sizeof(observed) - 1,
                        (uint8_t[35]) { 0 }, 35);

        stl_le_p(current0 + 4, 148000);
        stw_le_p(current0 + 28, 59);
        memcpy(observed, current0, sizeof(observed));
        bcm2711_firmware_display_timing_call(
            source, RPI_FWREQ_SET_TIMING, observed);
        g_assert_cmpmem(observed, sizeof(observed),
                        current0, sizeof(current0));

        memcpy(observed, current0, sizeof(observed));
        stw_le_p(observed + 10, lduw_le_p(observed + 8));
        bcm2711_firmware_display_timing_call(
            source, RPI_FWREQ_SET_TIMING, observed);
        g_assert_cmpmem(observed, sizeof(observed),
                        current0, sizeof(current0));

        stl_le_p(current1 + 4, 74000);
        stw_le_p(current1 + 28, 59);
        {
            uint8_t request[108] = { 0 };

            stl_le_p(request, sizeof(request));
            stl_le_p(request + 8, RPI_FWREQ_GET_DISPLAY_TIMING);
            stl_le_p(request + 12, 36);
            request[20] = 7;
            stl_le_p(request + 56, RPI_FWREQ_SET_TIMING);
            stl_le_p(request + 60, 36);
            memcpy(request + 68, current1, sizeof(current1));
            bcm2711_property_raw_exchange(
                source, request, sizeof(request));
            g_assert_cmphex((uint32_t)ldl_le_p(request + 4), ==,
                            0x80000000);
            g_assert_cmphex((uint32_t)ldl_le_p(request + 16), ==,
                            0x80000024);
            g_assert_cmpmem(request + 20, 36, current1, 36);
            g_assert_cmphex((uint32_t)ldl_le_p(request + 64), ==,
                            0x80000024);
            g_assert_cmpmem(request + 68, 36, current1, 36);
        }
        {
            uint8_t request[56] = { 0 };

            stl_le_p(request, sizeof(request));
            stl_le_p(request + 8, RPI_FWREQ_GET_DISPLAY_TIMING);
            stl_le_p(request + 12, 32);
            request[20] = 2;
            bcm2711_property_raw_exchange(
                source, request, sizeof(request));
            g_assert_cmphex((uint32_t)ldl_le_p(request + 4), ==,
                            0x80000001);
            g_assert_cmphex((uint32_t)ldl_le_p(request + 16), ==, 0);
        }

        overwrite_image(hdmi0_path, destination_hdmi0,
                        sizeof(destination_hdmi0));
        overwrite_image(hdmi1_path, destination_hdmi1,
                        sizeof(destination_hdmi1));
        destination = migrate_to_new_qtest(
            source, command, &migration_dir, &migration_socket);
        bcm2711_firmware_display_timing_get(destination, 2, observed);
        g_assert_cmpmem(observed, sizeof(observed),
                        current0, sizeof(current0));
        bcm2711_firmware_display_timing_get(destination, 7, observed);
        g_assert_cmpmem(observed, sizeof(observed),
                        current1, sizeof(current1));

        qtest_system_reset(destination);
        bcm2711_firmware_display_timing_get(destination, 2, observed);
        g_assert_cmpmem(observed, sizeof(observed),
                        destination_timing0, sizeof(destination_timing0));
        bcm2711_firmware_display_timing_get(destination, 7, observed);
        g_assert_cmpmem(observed, sizeof(observed),
                        destination_timing1, sizeof(destination_timing1));

        qtest_quit(source);
        qtest_quit(destination);
        unlink(migration_socket);
        rmdir(migration_dir);
        unlink(hdmi0_path);
        unlink(hdmi1_path);
    }
}
void test_bcm2711_firmware_power(void)
{
    const char *machines[] = { "raspi4b", "raspi-cm4" };

    for (size_t index = 0; index < ARRAY_SIZE(machines); index++) {
        g_autofree char *command = g_strdup_printf(
            "-M %s", machines[index]);
        g_autofree char *migration_dir = NULL;
        g_autofree char *migration_socket = NULL;
        QTestState *source = qtest_init(command);
        QTestState *destination;

        g_assert_cmpuint(bcm2711_firmware_power_call(
                             source, RPI_FWREQ_GET_POWER_STATE, 3, 0), ==, 1);
        g_assert_cmpuint(bcm2711_firmware_power_call(
                             source, RPI_FWREQ_GET_TIMING, 3, 0), ==, 0);
        g_assert_cmpuint(bcm2711_firmware_power_call(
                             source, RPI_FWREQ_GET_POWER_STATE,
                             BCM2835_PROPERTY_POWER_DEVICE_COUNT - 1,
                             0), ==, 1);
        g_assert_cmpuint(bcm2711_firmware_power_call(
                             source, RPI_FWREQ_SET_POWER_STATE, 3,
                             BIT(0) | BIT(1)), ==, 1);
        g_assert_cmpuint(bcm2711_firmware_power_call(
                             source, RPI_FWREQ_SET_POWER_STATE, 3, 0), ==, 0);
        g_assert_cmpuint(bcm2711_firmware_power_call(
                             source, RPI_FWREQ_GET_POWER_STATE, 3, 0), ==, 0);
        g_assert_cmpuint(bcm2711_firmware_power_call(
                             source, RPI_FWREQ_GET_POWER_STATE,
                             BCM2835_PROPERTY_POWER_DEVICE_COUNT, 0), ==,
                         BIT(1));
        g_assert_cmpuint(bcm2711_firmware_power_call(
                             source, RPI_FWREQ_SET_POWER_STATE,
                             BCM2835_PROPERTY_POWER_DEVICE_COUNT, 1), ==,
                         BIT(1));

        destination = migrate_to_new_qtest(
            source, command, &migration_dir, &migration_socket);
        g_assert_cmpuint(bcm2711_firmware_power_call(
                             destination, RPI_FWREQ_GET_POWER_STATE,
                             3, 0), ==, 0);
        qtest_system_reset(destination);
        g_assert_cmpuint(bcm2711_firmware_power_call(
                             destination, RPI_FWREQ_GET_POWER_STATE,
                             3, 0), ==, 1);

        qtest_quit(source);
        qtest_quit(destination);
        unlink(migration_socket);
        rmdir(migration_dir);
    }
}
void test_bcm2711_firmware_framebuffer_transaction(void)
{
    static const char * const machines[] = { "raspi4b", "raspi-cm4" };

    for (size_t index = 0; index < ARRAY_SIZE(machines); index++) {
        g_autofree char *command = g_strdup_printf(
            "-M %s", machines[index]);
        QTestState *qts = qtest_init(command);
        uint8_t request[80];
        uint32_t dimensions[2] = { 0 };
        uint32_t virtual_dimensions[2] = { 0 };
        uint32_t offsets[2] = { 0 };
        uint32_t depth = 0;

        /*
         * Test tags are evaluated together against a temporary configuration:
         * each normalized response feeds the next Test without changing the
         * live framebuffer.
         */
        memset(request, 0xa5, sizeof(request));
        stl_le_p(request, 72);
        stl_le_p(request + 4, 0);
        stl_le_p(request + 8,
                 RPI_FWREQ_FRAMEBUFFER_TEST_PHYSICAL_WIDTH_HEIGHT);
        stl_le_p(request + 12, 8);
        stl_le_p(request + 16, 0);
        stl_le_p(request + 20, UINT32_MAX);
        stl_le_p(request + 24, 0);
        stl_le_p(request + 28,
                 RPI_FWREQ_FRAMEBUFFER_TEST_VIRTUAL_WIDTH_HEIGHT);
        stl_le_p(request + 32, 8);
        stl_le_p(request + 36, 0);
        stl_le_p(request + 40, 1);
        stl_le_p(request + 44, UINT32_MAX);
        stl_le_p(request + 48,
                 RPI_FWREQ_FRAMEBUFFER_TEST_VIRTUAL_OFFSET);
        stl_le_p(request + 52, 8);
        stl_le_p(request + 56, 0);
        stl_le_p(request + 60, UINT32_MAX);
        stl_le_p(request + 64, UINT32_MAX);
        stl_le_p(request + 68, RPI_FWREQ_PROPERTY_END);
        bcm2711_property_raw_exchange(qts, request, 72);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 4), ==, 0x80000000);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 16), ==, 0x80000008);
        g_assert_cmpuint((uint32_t)ldl_le_p(request + 20), ==, 3840);
        g_assert_cmpuint((uint32_t)ldl_le_p(request + 24), ==, 488);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 36), ==, 0x80000008);
        g_assert_cmpuint((uint32_t)ldl_le_p(request + 40), ==, 3840);
        g_assert_cmpuint((uint32_t)ldl_le_p(request + 44), ==, 2560);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 56), ==, 0x80000008);
        g_assert_cmpuint((uint32_t)ldl_le_p(request + 60), ==, 0);
        g_assert_cmpuint((uint32_t)ldl_le_p(request + 64), ==, 2072);

        bcm2711_property_call(
            qts, RPI_FWREQ_FRAMEBUFFER_GET_PHYSICAL_WIDTH_HEIGHT,
            dimensions, G_N_ELEMENTS(dimensions));
        bcm2711_property_call(
            qts, RPI_FWREQ_FRAMEBUFFER_GET_VIRTUAL_WIDTH_HEIGHT,
            virtual_dimensions, G_N_ELEMENTS(virtual_dimensions));
        bcm2711_property_call(
            qts, RPI_FWREQ_FRAMEBUFFER_GET_VIRTUAL_OFFSET,
            offsets, G_N_ELEMENTS(offsets));
        g_assert_cmpuint(dimensions[0], ==, 640);
        g_assert_cmpuint(dimensions[1], ==, 480);
        g_assert_cmpuint(virtual_dimensions[0], ==, 640);
        g_assert_cmpuint(virtual_dimensions[1], ==, 480);
        g_assert_cmpuint(offsets[0], ==, 0);
        g_assert_cmpuint(offsets[1], ==, 0);

        /*
         * Unsupported scalar candidates return the current temporary values
         * and do not alter live depth, pixel order, or alpha mode.
         */
        memset(request, 0xa5, sizeof(request));
        stl_le_p(request, 60);
        stl_le_p(request + 4, 0);
        stl_le_p(request + 8, RPI_FWREQ_FRAMEBUFFER_TEST_DEPTH);
        stl_le_p(request + 12, 4);
        stl_le_p(request + 16, 0);
        stl_le_p(request + 20, 7);
        stl_le_p(request + 24, RPI_FWREQ_FRAMEBUFFER_TEST_PIXEL_ORDER);
        stl_le_p(request + 28, 4);
        stl_le_p(request + 32, 0);
        stl_le_p(request + 36, 2);
        stl_le_p(request + 40, RPI_FWREQ_FRAMEBUFFER_TEST_ALPHA_MODE);
        stl_le_p(request + 44, 4);
        stl_le_p(request + 48, 0);
        stl_le_p(request + 52, 3);
        stl_le_p(request + 56, RPI_FWREQ_PROPERTY_END);
        bcm2711_property_raw_exchange(qts, request, 60);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 4), ==, 0x80000000);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 16), ==, 0x80000004);
        g_assert_cmpuint((uint32_t)ldl_le_p(request + 20), ==, 16);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 32), ==, 0x80000004);
        g_assert_cmpuint((uint32_t)ldl_le_p(request + 36), ==, 1);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 48), ==, 0x80000004);
        g_assert_cmpuint((uint32_t)ldl_le_p(request + 52), ==, 2);

        /*
         * Duplicate framebuffer tags reject the whole transaction before
         * either candidate can alter live configuration.
         */
        memset(request, 0xa5, sizeof(request));
        stl_le_p(request, 44);
        stl_le_p(request + 4, 0);
        stl_le_p(request + 8, RPI_FWREQ_FRAMEBUFFER_SET_DEPTH);
        stl_le_p(request + 12, 4);
        stl_le_p(request + 16, 0);
        stl_le_p(request + 20, 32);
        stl_le_p(request + 24, RPI_FWREQ_FRAMEBUFFER_SET_DEPTH);
        stl_le_p(request + 28, 4);
        stl_le_p(request + 32, 0);
        stl_le_p(request + 36, 8);
        stl_le_p(request + 40, RPI_FWREQ_PROPERTY_END);
        bcm2711_property_raw_exchange(qts, request, 44);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 4), ==, 0x80000001);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 16), ==, 0);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 32), ==, 0);
        g_assert_cmpuint((uint32_t)ldl_le_p(request + 20), ==, 32);
        g_assert_cmpuint((uint32_t)ldl_le_p(request + 36), ==, 8);
        bcm2711_property_call(
            qts, RPI_FWREQ_FRAMEBUFFER_GET_DEPTH, &depth, 1);
        g_assert_cmpuint(depth, ==, 16);

        /*
         * All Set tags are applied before any Get response, even when the
         * Get appears first in the request.
         */
        memset(request, 0xa5, sizeof(request));
        stl_le_p(request, 52);
        stl_le_p(request + 4, 0);
        stl_le_p(request + 8,
                 RPI_FWREQ_FRAMEBUFFER_GET_PHYSICAL_WIDTH_HEIGHT);
        stl_le_p(request + 12, 8);
        stl_le_p(request + 16, 0);
        stl_le_p(request + 28,
                 RPI_FWREQ_FRAMEBUFFER_SET_PHYSICAL_WIDTH_HEIGHT);
        stl_le_p(request + 32, 8);
        stl_le_p(request + 36, 0);
        stl_le_p(request + 40, 123);
        stl_le_p(request + 44, 77);
        stl_le_p(request + 48, RPI_FWREQ_PROPERTY_END);
        bcm2711_property_raw_exchange(qts, request, 52);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 4), ==, 0x80000000);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 16), ==, 0x80000008);
        g_assert_cmpuint((uint32_t)ldl_le_p(request + 20), ==, 123);
        g_assert_cmpuint((uint32_t)ldl_le_p(request + 24), ==, 77);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 36), ==, 0x80000008);
        g_assert_cmpuint((uint32_t)ldl_le_p(request + 40), ==, 123);
        g_assert_cmpuint((uint32_t)ldl_le_p(request + 44), ==, 77);

        /*
         * Palette writes follow the same transaction ordering and a short
         * earlier Get retains the full desired response length.
         */
        memset(request, 0xa5, sizeof(request));
        stl_le_p(request, 52);
        stl_le_p(request + 4, 0);
        stl_le_p(request + 8, RPI_FWREQ_FRAMEBUFFER_GET_PALETTE);
        stl_le_p(request + 12, 4);
        stl_le_p(request + 16, 0);
        stl_le_p(request + 24, RPI_FWREQ_FRAMEBUFFER_SET_PALETTE);
        stl_le_p(request + 28, 12);
        stl_le_p(request + 32, 0);
        stl_le_p(request + 36, 0);
        stl_le_p(request + 40, 1);
        stl_le_p(request + 44, 0x44332211);
        stl_le_p(request + 48, RPI_FWREQ_PROPERTY_END);
        bcm2711_property_raw_exchange(qts, request, 52);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 4), ==, 0x80000000);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 16), ==, 0x80000400);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 20), ==, 0x44332211);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 32), ==, 0x80000004);
        g_assert_cmpuint((uint32_t)ldl_le_p(request + 36), ==, 0);

        /*
         * Mixing Test with Get/Set rejects the complete request before
         * response headers, configuration, or unrelated side effects.
         */
        memset(request, 0xa5, sizeof(request));
        stl_le_p(request, 44);
        stl_le_p(request + 4, 0);
        stl_le_p(request + 8, RPI_FWREQ_FRAMEBUFFER_TEST_DEPTH);
        stl_le_p(request + 12, 4);
        stl_le_p(request + 16, 0);
        stl_le_p(request + 20, 32);
        stl_le_p(request + 24, RPI_FWREQ_FRAMEBUFFER_SET_DEPTH);
        stl_le_p(request + 28, 4);
        stl_le_p(request + 32, 0);
        stl_le_p(request + 36, 8);
        stl_le_p(request + 40, RPI_FWREQ_PROPERTY_END);
        bcm2711_property_raw_exchange(qts, request, 44);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 4), ==, 0x80000001);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 16), ==, 0);
        g_assert_cmphex((uint32_t)ldl_le_p(request + 32), ==, 0);
        g_assert_cmpuint((uint32_t)ldl_le_p(request + 20), ==, 32);
        g_assert_cmpuint((uint32_t)ldl_le_p(request + 36), ==, 8);

        bcm2711_property_call(
            qts, RPI_FWREQ_FRAMEBUFFER_GET_DEPTH, &depth, 1);
        g_assert_cmpuint(depth, ==, 16);
        bcm2711_property_call(
            qts, RPI_FWREQ_FRAMEBUFFER_GET_PHYSICAL_WIDTH_HEIGHT,
            dimensions, G_N_ELEMENTS(dimensions));
        g_assert_cmpuint(dimensions[0], ==, 123);
        g_assert_cmpuint(dimensions[1], ==, 77);

        qtest_quit(qts);
    }
}
void test_bcm2711_firmware_framebuffer_display_control(void)
{
    static const char * const machines[] = { "raspi4b", "raspi-cm4" };

    for (size_t index = 0; index < ARRAY_SIZE(machines); index++) {
        g_autofree char *command = g_strdup_printf(
            "-M %s", machines[index]);
        g_autofree char *migration_dir = NULL;
        g_autofree char *migration_socket = NULL;
        QTestState *source = qtest_init(command);
        QTestState *destination;
        uint8_t timing[36];
        int64_t remaining;

        g_assert_cmpuint(bcm2711_firmware_framebuffer_scalar(
                             source, RPI_FWREQ_FRAMEBUFFER_GET_LAYER, 0), ==,
                         0);
        g_assert_cmpuint(bcm2711_firmware_framebuffer_scalar(
                             source, RPI_FWREQ_FRAMEBUFFER_GET_TRANSFORM, 0),
                         ==, 0);
        g_assert_cmpuint(bcm2711_firmware_framebuffer_scalar(
                             source, RPI_FWREQ_FRAMEBUFFER_GET_VSYNC, 0), ==,
                         0);

        /* Test responses validate candidates without changing live state. */
        g_assert_cmphex(bcm2711_firmware_framebuffer_scalar(
                            source, RPI_FWREQ_FRAMEBUFFER_TEST_LAYER,
                            UINT32_MAX), ==, UINT32_MAX);
        g_assert_cmpuint(bcm2711_firmware_framebuffer_scalar(
                             source, RPI_FWREQ_FRAMEBUFFER_TEST_TRANSFORM, 7),
                         ==, 7);
        g_assert_cmpuint(bcm2711_firmware_framebuffer_scalar(
                             source, RPI_FWREQ_FRAMEBUFFER_TEST_TRANSFORM, 8),
                         ==, 0);
        g_assert_cmpuint(bcm2711_firmware_framebuffer_scalar(
                             source, RPI_FWREQ_FRAMEBUFFER_TEST_VSYNC, 1), ==,
                         0);
        g_assert_cmpuint(bcm2711_firmware_framebuffer_scalar(
                             source, RPI_FWREQ_FRAMEBUFFER_TEST_VSYNC, 2), ==,
                         0);
        g_assert_cmpuint(bcm2711_firmware_framebuffer_scalar(
                             source, RPI_FWREQ_FRAMEBUFFER_GET_LAYER, 0), ==,
                         0);
        g_assert_cmpuint(bcm2711_firmware_framebuffer_scalar(
                             source, RPI_FWREQ_FRAMEBUFFER_GET_TRANSFORM, 0),
                         ==, 0);
        g_assert_cmpuint(bcm2711_firmware_framebuffer_scalar(
                             source, RPI_FWREQ_FRAMEBUFFER_GET_VSYNC, 0), ==,
                         0);

        g_assert_cmphex(bcm2711_firmware_framebuffer_scalar(
                            source, RPI_FWREQ_FRAMEBUFFER_SET_LAYER,
                            UINT32_MAX), ==, UINT32_MAX);
        g_assert_cmpuint(bcm2711_firmware_framebuffer_scalar(
                             source, RPI_FWREQ_FRAMEBUFFER_SET_TRANSFORM, 6),
                         ==, 6);
        make_firmware_display_timing(timing, 2, 74250,
                                     1280, 110, 40, 1650,
                                     720, 5, 5, 900, 0x23);
        bcm2711_firmware_display_timing_call(
            source, RPI_FWREQ_SET_TIMING, timing);
        g_assert_cmpuint(lduw_le_p(timing + 28), ==, 50);

        remaining = bcm2711_firmware_vsync_submit(source, 50);
        qtest_clock_step(source, remaining - 1);
        g_assert_true(qtest_readl(source, BCM2711_MAILBOX_STATUS) &
                      BCM2711_MAILBOX_EMPTY);
        qtest_clock_step(source, 1);
        bcm2711_firmware_vsync_receive(source);

        /* Invalid Set candidates retain and return the previous value. */
        g_assert_cmpuint(bcm2711_firmware_framebuffer_scalar(
                             source, RPI_FWREQ_FRAMEBUFFER_SET_TRANSFORM, 8),
                         ==, 6);

        remaining = bcm2711_firmware_vsync_submit(source, 50);
        qtest_clock_step(source, remaining / 2);
        destination = migrate_to_new_qtest(
            source, command, &migration_dir, &migration_socket);
        g_assert_true(qtest_readl(destination, BCM2711_MAILBOX_STATUS) &
                      BCM2711_MAILBOX_EMPTY);
        qtest_clock_step_next(destination);
        bcm2711_firmware_vsync_receive(destination);
        g_assert_cmphex(bcm2711_firmware_framebuffer_scalar(
                            destination, RPI_FWREQ_FRAMEBUFFER_GET_LAYER, 0),
                        ==, UINT32_MAX);
        g_assert_cmpuint(bcm2711_firmware_framebuffer_scalar(
                             destination,
                             RPI_FWREQ_FRAMEBUFFER_GET_TRANSFORM, 0), ==, 6);
        g_assert_cmpuint(bcm2711_firmware_framebuffer_scalar(
                             destination, RPI_FWREQ_FRAMEBUFFER_GET_VSYNC, 0),
                         ==, 0);

        bcm2711_firmware_vsync_submit(destination, 50);
        qtest_system_reset(destination);
        g_assert_true(qtest_readl(destination, BCM2711_MAILBOX_STATUS) &
                      BCM2711_MAILBOX_EMPTY);
        remaining = bcm2711_firmware_vsync_submit(destination, 60);
        qtest_clock_step(destination, remaining);
        bcm2711_firmware_vsync_receive(destination);
        g_assert_cmpuint(bcm2711_firmware_framebuffer_scalar(
                             destination, RPI_FWREQ_FRAMEBUFFER_GET_LAYER, 0),
                         ==, 0);
        g_assert_cmpuint(bcm2711_firmware_framebuffer_scalar(
                             destination,
                             RPI_FWREQ_FRAMEBUFFER_GET_TRANSFORM, 0), ==, 0);
        g_assert_cmpuint(bcm2711_firmware_framebuffer_scalar(
                             destination, RPI_FWREQ_FRAMEBUFFER_GET_VSYNC, 0),
                         ==, 0);

        qtest_quit(source);
        qtest_quit(destination);
        unlink(migration_socket);
        rmdir(migration_dir);
    }
}
void test_bcm2711_firmware_framebuffer_displays(void)
{
    static const char * const machines[] = { "raspi4b", "raspi-cm4" };

    for (size_t index = 0; index < ARRAY_SIZE(machines); index++) {
        g_autofree char *command = g_strdup_printf(
            "-M %s", machines[index]);
        g_autofree char *migration_dir = NULL;
        g_autofree char *migration_socket = NULL;
        QTestState *source = qtest_init(command);
        QTestState *destination;

        g_assert_cmpuint(bcm2711_firmware_framebuffer_scalar(
                             source,
                             RPI_FWREQ_FRAMEBUFFER_GET_NUM_DISPLAYS, 0), ==,
                         BCM2835_PROPERTY_DISPLAY_COUNT);
        g_assert_cmpuint(bcm2711_firmware_framebuffer_scalar(
                             source,
                             RPI_FWREQ_FRAMEBUFFER_GET_DISPLAY_ID, 0), ==, 2);
        g_assert_cmpuint(bcm2711_firmware_framebuffer_scalar(
                             source,
                             RPI_FWREQ_FRAMEBUFFER_GET_DISPLAY_ID, 1), ==, 7);
        g_assert_cmphex(bcm2711_firmware_framebuffer_scalar(
                            source,
                            RPI_FWREQ_FRAMEBUFFER_GET_DISPLAY_ID, 2), ==,
                        UINT32_MAX);
        g_assert_cmpuint(qom_path_get_uint32(
                             source, PROPERTY_QOM_PATH,
                             "framebuffer-display-num"), ==, 0);
        g_assert_cmpuint(qom_path_get_uint32(
                             source, PROPERTY_QOM_PATH,
                             "display-power-mask"), ==, 3);

        g_assert_cmpuint(bcm2711_firmware_framebuffer_scalar(
                             source,
                             RPI_FWREQ_FRAMEBUFFER_SET_DISPLAY_NUM, 1), ==, 1);
        g_assert_cmpuint(bcm2711_firmware_framebuffer_scalar(
                             source,
                             RPI_FWREQ_FRAMEBUFFER_SET_DISPLAY_NUM, 2), ==, 1);
        g_assert_cmpuint(qom_path_get_uint32(
                             source, PROPERTY_QOM_PATH,
                             "framebuffer-display-num"), ==, 1);

        g_assert_cmpuint(
            bcm2711_firmware_display_power(source, 7, 0), ==, 0);
        g_assert_cmpuint(qom_path_get_uint32(
                             source, PROPERTY_QOM_PATH,
                             "display-power-mask"), ==, 1);
        g_assert_cmpuint(
            bcm2711_firmware_display_power(source, 2, 2), ==, 1);
        g_assert_cmpuint(
            bcm2711_firmware_display_power(source, 2, 0), ==, 0);
        g_assert_cmpuint(
            bcm2711_firmware_display_power(source, 99, 1), ==, 1);
        g_assert_cmpuint(qom_path_get_uint32(
                             source, PROPERTY_QOM_PATH,
                             "display-power-mask"), ==, 0);

        destination = migrate_to_new_qtest(
            source, command, &migration_dir, &migration_socket);
        g_assert_cmpuint(qom_path_get_uint32(
                             destination, PROPERTY_QOM_PATH,
                             "framebuffer-display-num"), ==, 1);
        g_assert_cmpuint(qom_path_get_uint32(
                             destination, PROPERTY_QOM_PATH,
                             "display-power-mask"), ==, 0);

        qtest_system_reset(destination);
        g_assert_cmpuint(qom_path_get_uint32(
                             destination, PROPERTY_QOM_PATH,
                             "framebuffer-display-num"), ==, 0);
        g_assert_cmpuint(qom_path_get_uint32(
                             destination, PROPERTY_QOM_PATH,
                             "display-power-mask"), ==, 3);

        qtest_quit(source);
        qtest_quit(destination);
        unlink(migration_socket);
        rmdir(migration_dir);
    }
}
void test_bcm2711_firmware_framebuffer_transform(void)
{
    static const char * const machines[] = { "raspi4b", "raspi-cm4" };
    static const uint8_t source_pixels[] = {
        255, 0, 0,   0, 255, 0,
        0, 0, 255,   255, 255, 0,
        255, 0, 255, 0, 255, 255,
    };
    static const uint8_t expected[8][sizeof(source_pixels)] = {
        {
            255, 0, 0,   0, 255, 0,
            0, 0, 255,   255, 255, 0,
            255, 0, 255, 0, 255, 255,
        },
        {
            0, 255, 0,   255, 0, 0,
            255, 255, 0, 0, 0, 255,
            0, 255, 255, 255, 0, 255,
        },
        {
            255, 0, 255, 0, 255, 255,
            0, 0, 255,   255, 255, 0,
            255, 0, 0,   0, 255, 0,
        },
        {
            0, 255, 255, 255, 0, 255,
            255, 255, 0, 0, 0, 255,
            0, 255, 0,   255, 0, 0,
        },
        {
            255, 0, 0, 0, 0, 255, 255, 0, 255,
            0, 255, 0, 255, 255, 0, 0, 255, 255,
        },
        {
            255, 0, 255, 0, 0, 255, 255, 0, 0,
            0, 255, 255, 255, 255, 0, 0, 255, 0,
        },
        {
            0, 255, 0, 255, 255, 0, 0, 255, 255,
            255, 0, 0, 0, 0, 255, 255, 0, 255,
        },
        {
            0, 255, 255, 255, 255, 0, 0, 255, 0,
            255, 0, 255, 0, 0, 255, 255, 0, 0,
        },
    };

    for (size_t index = 0; index < ARRAY_SIZE(machines); index++) {
        g_autofree char *command = g_strdup_printf(
            "-M %s", machines[index]);
        g_autofree char *migration_dir = NULL;
        g_autofree char *migration_socket = NULL;
        g_autofree char *screendump_path = NULL;
        QTestState *source = qtest_init(command);
        QTestState *destination;
        uint8_t dirty_expected[sizeof(source_pixels)];
        uint32_t dimensions[2] = { 2, 3 };
        uint32_t allocation[2] = { 16, 0 };
        uint32_t depth = 24;
        int screendump_fd;

        bcm2711_property_call(
            source, RPI_FWREQ_FRAMEBUFFER_SET_PHYSICAL_WIDTH_HEIGHT,
            dimensions, G_N_ELEMENTS(dimensions));
        bcm2711_property_call(
            source, RPI_FWREQ_FRAMEBUFFER_SET_VIRTUAL_WIDTH_HEIGHT,
            dimensions, G_N_ELEMENTS(dimensions));
        bcm2711_property_call(
            source, RPI_FWREQ_FRAMEBUFFER_SET_DEPTH, &depth, 1);
        bcm2711_property_call(
            source, RPI_FWREQ_FRAMEBUFFER_ALLOCATE,
            allocation, G_N_ELEMENTS(allocation));
        g_assert_cmpuint(allocation[1], ==, sizeof(source_pixels));
        qtest_memwrite(
            source, allocation[0], source_pixels, sizeof(source_pixels));

        screendump_fd = g_file_open_tmp(
            "raspi4-framebuffer-transform-XXXXXX.ppm",
            &screendump_path, NULL);
        g_assert_cmpint(screendump_fd, >=, 0);
        close(screendump_fd);

        for (uint32_t transform = 0; transform < 8; transform++) {
            uint32_t width = transform & BIT(2) ? 3 : 2;
            uint32_t height = transform & BIT(2) ? 2 : 3;

            g_assert_cmpuint(bcm2711_firmware_framebuffer_scalar(
                                 source,
                                 RPI_FWREQ_FRAMEBUFFER_SET_TRANSFORM,
                                 transform), ==, transform);
            assert_bcm2711_framebuffer_surface(
                source, screendump_path, width, height, expected[transform]);
        }

        /*
         * A source write without reconfiguration must follow the transformed
         * dirty-memory path and update the mapped destination pixel.
         */
        memcpy(dirty_expected, expected[7], sizeof(dirty_expected));
        memset(dirty_expected + sizeof(dirty_expected) - 3, 0xff, 3);
        qtest_memset(source, allocation[0], 0xff, 3);
        assert_bcm2711_framebuffer_surface(
            source, screendump_path, 3, 2, dirty_expected);

        destination = migrate_to_new_qtest(
            source, command, &migration_dir, &migration_socket);
        assert_bcm2711_framebuffer_surface(
            destination, screendump_path, 3, 2, dirty_expected);

        qtest_quit(source);
        qtest_quit(destination);
        unlink(screendump_path);
        unlink(migration_socket);
        rmdir(migration_dir);
    }
}
void test_bcm2711_firmware_framebuffer_cursor(void)
{
    static const char * const machines[] = { "raspi4b", "raspi-cm4" };
    enum {
        WIDTH = 20,
        HEIGHT = 20,
        CURSOR_WIDTH = 16,
        CURSOR_HEIGHT = 16,
        CURSOR_ADDRESS = 0x200000,
    };

    for (size_t index = 0; index < ARRAY_SIZE(machines); index++) {
        g_autofree char *command = g_strdup_printf(
            "-M %s", machines[index]);
        g_autofree char *migration_dir = NULL;
        g_autofree char *migration_socket = NULL;
        g_autofree char *screendump_path = NULL;
        g_autofree uint8_t *frame = g_malloc(WIDTH * HEIGHT * 3);
        g_autofree uint8_t *expected = g_malloc(WIDTH * HEIGHT * 3);
        uint8_t cursor_pixels[CURSOR_WIDTH * CURSOR_HEIGHT * 4] = { 0 };
        uint32_t dimensions[2] = { WIDTH, HEIGHT };
        uint32_t allocation[2] = { 16, 0 };
        uint32_t info[6] = {
            CURSOR_WIDTH, CURSOR_HEIGHT, 0, CURSOR_ADDRESS, 1, 2,
        };
        uint32_t state[4] = { 1, 5, 6, 0 };
        uint32_t depth = 24;
        QTestState *source;
        QTestState *destination;
        int screendump_fd;

        for (size_t pixel = 0; pixel < WIDTH * HEIGHT; pixel++) {
            frame[pixel * 3 + 0] = 0;
            frame[pixel * 3 + 1] = 0;
            frame[pixel * 3 + 2] = 255;
        }
        memcpy(expected, frame, WIDTH * HEIGHT * 3);
        stl_le_p(cursor_pixels + (2 * CURSOR_WIDTH + 1) * 4,
                 0xffff0000);

        source = qtest_init(command);
        bcm2711_property_call(
            source, RPI_FWREQ_FRAMEBUFFER_SET_PHYSICAL_WIDTH_HEIGHT,
            dimensions, G_N_ELEMENTS(dimensions));
        bcm2711_property_call(
            source, RPI_FWREQ_FRAMEBUFFER_SET_VIRTUAL_WIDTH_HEIGHT,
            dimensions, G_N_ELEMENTS(dimensions));
        bcm2711_property_call(
            source, RPI_FWREQ_FRAMEBUFFER_SET_DEPTH, &depth, 1);
        bcm2711_property_call(
            source, RPI_FWREQ_FRAMEBUFFER_ALLOCATE,
            allocation, G_N_ELEMENTS(allocation));
        g_assert_cmpuint(allocation[1], ==, sizeof(uint8_t) *
                         WIDTH * HEIGHT * 3);
        qtest_memwrite(source, allocation[0], frame, WIDTH * HEIGHT * 3);
        qtest_memwrite(source, CURSOR_ADDRESS, cursor_pixels,
                       sizeof(cursor_pixels));

        bcm2711_property_call_response(
            source, RPI_FWREQ_SET_CURSOR_INFO, info,
            G_N_ELEMENTS(info), 1);
        g_assert_cmpuint(info[0], ==, 0);
        bcm2711_property_call_response(
            source, RPI_FWREQ_SET_CURSOR_STATE, state,
            G_N_ELEMENTS(state), 1);
        g_assert_cmpuint(state[0], ==, 0);
        g_assert_true(qtest_qom_get_bool(
            source, FRAMEBUFFER_QOM_PATH, "cursor-info-valid"));
        g_assert_true(qtest_qom_get_bool(
            source, FRAMEBUFFER_QOM_PATH, "cursor-enabled"));

        screendump_fd = g_file_open_tmp(
            "raspi4-framebuffer-cursor-XXXXXX.ppm",
            &screendump_path, NULL);
        g_assert_cmpint(screendump_fd, >=, 0);
        close(screendump_fd);
        expected[(6 * WIDTH + 5) * 3 + 0] = 255;
        expected[(6 * WIDTH + 5) * 3 + 2] = 0;
        assert_bcm2711_framebuffer_surface(
            source, screendump_path, WIDTH, HEIGHT, expected);

        info[0] = 15;
        info[1] = CURSOR_HEIGHT;
        info[2] = 0;
        info[3] = CURSOR_ADDRESS;
        info[4] = 1;
        info[5] = 2;
        bcm2711_property_call_response(
            source, RPI_FWREQ_SET_CURSOR_INFO, info,
            G_N_ELEMENTS(info), 1);
        g_assert_cmpuint(info[0], ==, 1);
        state[0] = 1;
        state[1] = 8;
        state[2] = 9;
        state[3] = BIT(1);
        bcm2711_property_call_response(
            source, RPI_FWREQ_SET_CURSOR_STATE, state,
            G_N_ELEMENTS(state), 1);
        g_assert_cmpuint(state[0], ==, 1);
        assert_bcm2711_framebuffer_surface(
            source, screendump_path, WIDTH, HEIGHT, expected);

        state[0] = 1;
        state[1] = 8;
        state[2] = 9;
        state[3] = 0;
        bcm2711_property_call_response(
            source, RPI_FWREQ_SET_CURSOR_STATE, state,
            G_N_ELEMENTS(state), 1);
        g_assert_cmpuint(state[0], ==, 0);
        memcpy(expected, frame, WIDTH * HEIGHT * 3);
        expected[(9 * WIDTH + 8) * 3 + 0] = 255;
        expected[(9 * WIDTH + 8) * 3 + 2] = 0;
        assert_bcm2711_framebuffer_surface(
            source, screendump_path, WIDTH, HEIGHT, expected);

        {
            uint8_t request[44] = { 0 };

            stl_le_p(request, sizeof(request));
            stl_le_p(request + 8, RPI_FWREQ_SET_CURSOR_INFO);
            stl_le_p(request + 12, 20);
            stl_le_p(request + 20, CURSOR_WIDTH);
            bcm2711_property_raw_exchange(
                source, request, sizeof(request));
            g_assert_cmphex((uint32_t)ldl_le_p(request + 4), ==,
                            0x80000001);
            g_assert_cmphex((uint32_t)ldl_le_p(request + 16), ==, 0);
            g_assert_true(qtest_qom_get_bool(
                source, FRAMEBUFFER_QOM_PATH, "cursor-info-valid"));
        }

        destination = migrate_to_new_qtest(
            source, command, &migration_dir, &migration_socket);
        g_assert_true(qtest_qom_get_bool(
            destination, FRAMEBUFFER_QOM_PATH, "cursor-info-valid"));
        g_assert_true(qtest_qom_get_bool(
            destination, FRAMEBUFFER_QOM_PATH, "cursor-enabled"));
        assert_bcm2711_framebuffer_surface(
            destination, screendump_path, WIDTH, HEIGHT, expected);

        stl_le_p(cursor_pixels + (2 * CURSOR_WIDTH + 1) * 4,
                 0xff00ff00);
        qtest_memwrite(destination, CURSOR_ADDRESS, cursor_pixels,
                       sizeof(cursor_pixels));
        memcpy(expected, frame, WIDTH * HEIGHT * 3);
        expected[(9 * WIDTH + 8) * 3 + 1] = 255;
        expected[(9 * WIDTH + 8) * 3 + 2] = 0;
        assert_bcm2711_framebuffer_surface(
            destination, screendump_path, WIDTH, HEIGHT, expected);

        state[0] = 0;
        state[1] = 8;
        state[2] = 9;
        state[3] = 0;
        bcm2711_property_call_response(
            destination, RPI_FWREQ_SET_CURSOR_STATE, state,
            G_N_ELEMENTS(state), 1);
        g_assert_cmpuint(state[0], ==, 0);
        assert_bcm2711_framebuffer_surface(
            destination, screendump_path, WIDTH, HEIGHT, frame);

        state[0] = 1;
        bcm2711_property_call_response(
            destination, RPI_FWREQ_SET_CURSOR_STATE, state,
            G_N_ELEMENTS(state), 1);
        g_assert_true(qtest_qom_get_bool(
            destination, FRAMEBUFFER_QOM_PATH, "cursor-enabled"));
        qtest_system_reset(destination);
        g_assert_false(qtest_qom_get_bool(
            destination, FRAMEBUFFER_QOM_PATH, "cursor-info-valid"));
        g_assert_false(qtest_qom_get_bool(
            destination, FRAMEBUFFER_QOM_PATH, "cursor-enabled"));

        qtest_quit(source);
        qtest_quit(destination);
        unlink(screendump_path);
        unlink(migration_socket);
        rmdir(migration_dir);
    }
}
void test_bcm2711_firmware_overscan(void)
{
    static const uint32_t configured[4] = { 1, 1, 2, 1 };
    static const uint32_t candidate[4] = { 0, 1, 1, 0 };
    static const uint32_t invalid[4] = { 3, 3, 0, 0 };
    static const uint32_t reset_values[4] = { 0, 0, 0, 0 };
    static const uint8_t red[3] = { 255, 0, 0 };
    const char *machines[] = { "raspi4b", "raspi-cm4" };

    for (size_t index = 0; index < ARRAY_SIZE(machines); index++) {
        g_autofree char *command = g_strdup_printf(
            "-M %s", machines[index]);
        g_autofree char *migration_dir = NULL;
        g_autofree char *migration_socket = NULL;
        g_autofree char *screendump_path = NULL;
        uint8_t framebuffer[8 * 6 * 3];
        uint8_t expected[8 * 6 * 3];
        uint32_t dimensions[2] = { 8, 6 };
        uint32_t allocation[2] = { 16, 0 };
        uint32_t depth = 24;
        QTestState *source = qtest_init(command);
        QTestState *destination;
        uint32_t values[4];
        int screendump_fd;

        for (size_t pixel = 0; pixel < 8 * 6; pixel++) {
            memcpy(framebuffer + pixel * 3, red, sizeof(red));
        }
        bcm2711_property_call(
            source, RPI_FWREQ_FRAMEBUFFER_SET_PHYSICAL_WIDTH_HEIGHT,
            dimensions, G_N_ELEMENTS(dimensions));
        bcm2711_property_call(
            source, RPI_FWREQ_FRAMEBUFFER_SET_VIRTUAL_WIDTH_HEIGHT,
            dimensions, G_N_ELEMENTS(dimensions));
        bcm2711_property_call(
            source, RPI_FWREQ_FRAMEBUFFER_SET_DEPTH, &depth, 1);
        bcm2711_property_call(
            source, RPI_FWREQ_FRAMEBUFFER_ALLOCATE,
            allocation, G_N_ELEMENTS(allocation));
        g_assert_cmpuint(allocation[1], ==, sizeof(framebuffer));
        qtest_memwrite(
            source, allocation[0], framebuffer, sizeof(framebuffer));
        screendump_fd = g_file_open_tmp(
            "raspi4-framebuffer-overscan-XXXXXX.ppm",
            &screendump_path, NULL);
        g_assert_cmpint(screendump_fd, >=, 0);
        close(screendump_fd);

        memcpy(values, configured, sizeof(values));
        bcm2711_firmware_overscan_call(
            source, RPI_FWREQ_FRAMEBUFFER_SET_OVERSCAN, values);
        g_assert_cmpmem(values, sizeof(values),
                        configured, sizeof(configured));
        memset(expected, 0, sizeof(expected));
        for (uint32_t y = 1; y < 5; y++) {
            for (uint32_t x = 2; x < 7; x++) {
                memcpy(expected + (y * 8 + x) * 3, red, sizeof(red));
            }
        }
        assert_bcm2711_framebuffer_surface(
            source, screendump_path, 8, 6, expected);

        memcpy(values, candidate, sizeof(values));
        bcm2711_firmware_overscan_call(
            source, RPI_FWREQ_FRAMEBUFFER_TEST_OVERSCAN, values);
        g_assert_cmpmem(values, sizeof(values),
                        candidate, sizeof(candidate));
        assert_bcm2711_framebuffer_surface(
            source, screendump_path, 8, 6, expected);

        memcpy(values, invalid, sizeof(values));
        bcm2711_firmware_overscan_call(
            source, RPI_FWREQ_FRAMEBUFFER_TEST_OVERSCAN, values);
        g_assert_cmpmem(values, sizeof(values),
                        configured, sizeof(configured));
        memcpy(values, invalid, sizeof(values));
        bcm2711_firmware_overscan_call(
            source, RPI_FWREQ_FRAMEBUFFER_SET_OVERSCAN, values);
        g_assert_cmpmem(values, sizeof(values),
                        configured, sizeof(configured));

        memset(values, 0, sizeof(values));
        bcm2711_firmware_overscan_call(
            source, RPI_FWREQ_FRAMEBUFFER_GET_OVERSCAN, values);
        g_assert_cmpmem(values, sizeof(values),
                        configured, sizeof(configured));

        g_assert_cmpuint(bcm2711_firmware_framebuffer_scalar(
                             source,
                             RPI_FWREQ_FRAMEBUFFER_SET_TRANSFORM, 4), ==, 4);
        memset(expected, 0, sizeof(expected));
        for (uint32_t y = 1; y < 7; y++) {
            for (uint32_t x = 2; x < 5; x++) {
                memcpy(expected + (y * 6 + x) * 3, red, sizeof(red));
            }
        }
        assert_bcm2711_framebuffer_surface(
            source, screendump_path, 6, 8, expected);

        destination = migrate_to_new_qtest(
            source, command, &migration_dir, &migration_socket);
        memset(values, 0, sizeof(values));
        bcm2711_firmware_overscan_call(
            destination, RPI_FWREQ_FRAMEBUFFER_GET_OVERSCAN, values);
        g_assert_cmpmem(values, sizeof(values),
                        configured, sizeof(configured));
        assert_bcm2711_framebuffer_surface(
            destination, screendump_path, 6, 8, expected);

        qtest_system_reset(destination);
        memset(values, 0xff, sizeof(values));
        bcm2711_firmware_overscan_call(
            destination, RPI_FWREQ_FRAMEBUFFER_GET_OVERSCAN, values);
        g_assert_cmpmem(values, sizeof(values),
                        reset_values, sizeof(reset_values));

        qtest_quit(source);
        qtest_quit(destination);
        unlink(screendump_path);
        unlink(migration_socket);
        rmdir(migration_dir);
    }
}
void test_bcm2711_firmware_xhci_reset(void)
{
    static const char command[] = "-M raspi4b";
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    QTestState *source = qtest_init(command);
    QTestState *destination;
    uint32_t device_address;

    qtest_writel(source, RASPI4_PCIE_ECAM_BASE + 0x18, 0x00010100);
    qtest_writel(source, RASPI4_PCIE_ECAM_BASE + 0x20, 0xfff0c000);
    qtest_writew(source, RASPI4_PCIE_ECAM_BASE + 4, 2);
    qtest_writel(source, RASPI4_PCIE_EXT_CFG_INDEX, 1 << 20);
    qtest_writel(source, RASPI4_PCIE_EXT_CFG_DATA + 0x10, 0xc0000004);
    qtest_writel(source, RASPI4_PCIE_EXT_CFG_DATA + 0x14, 0);
    qtest_writew(source, RASPI4_PCIE_EXT_CFG_DATA + 4, 2);

    g_assert_cmpuint(qom_path_get_uint32(
                         source, PROPERTY_QOM_PATH,
                         "xhci-reset-count"), ==, 0);
    qtest_writel(source, RASPI4_VL805_USBCMD, 1);
    g_assert_cmphex(qtest_readl(source, RASPI4_VL805_USBCMD) & 1, ==, 1);
    device_address = 0x00100000;
    bcm2711_property_call(
        source, RPI_FWREQ_NOTIFY_XHCI_RESET, &device_address, 1);
    g_assert_cmphex(device_address, ==, 0x00100000);
    g_assert_cmphex(qtest_readl(source, RASPI4_VL805_USBCMD) & 1, ==, 0);
    g_assert_cmpuint(qom_path_get_uint32(
                         source, PROPERTY_QOM_PATH,
                         "xhci-reset-count"), ==, 1);

    qtest_writel(source, RASPI4_VL805_USBCMD, 1);
    device_address = 0;
    bcm2711_property_call(
        source, RPI_FWREQ_NOTIFY_XHCI_RESET, &device_address, 1);
    g_assert_cmphex(qtest_readl(source, RASPI4_VL805_USBCMD) & 1, ==, 1);
    g_assert_cmpuint(qom_path_get_uint32(
                         source, PROPERTY_QOM_PATH,
                         "xhci-reset-count"), ==, 1);

    destination = migrate_to_new_qtest(
        source, command, &migration_dir, &migration_socket);
    g_assert_cmpuint(qom_path_get_uint32(
                         destination, PROPERTY_QOM_PATH,
                         "xhci-reset-count"), ==, 1);
    device_address = 0x00100000;
    bcm2711_property_call(
        destination, RPI_FWREQ_NOTIFY_XHCI_RESET, &device_address, 1);
    g_assert_cmpuint(qom_path_get_uint32(
                         destination, PROPERTY_QOM_PATH,
                         "xhci-reset-count"), ==, 2);
    qtest_system_reset(destination);
    g_assert_cmpuint(qom_path_get_uint32(
                         destination, PROPERTY_QOM_PATH,
                         "xhci-reset-count"), ==, 0);

    qtest_quit(source);
    qtest_quit(destination);
    unlink(migration_socket);
    rmdir(migration_dir);

    source = qtest_init("-M raspi-cm4");
    device_address = 0x00100000;
    bcm2711_property_call(
        source, RPI_FWREQ_NOTIFY_XHCI_RESET, &device_address, 1);
    g_assert_cmpuint(qom_path_get_uint32(
                         source, PROPERTY_QOM_PATH,
                         "xhci-reset-count"), ==, 0);
    qtest_quit(source);
}
void test_bcm2711_firmware_clock(void)
{
    const char *machines[] = { "raspi4b", "raspi-cm4" };

    for (size_t index = 0; index < ARRAY_SIZE(machines); index++) {
        g_autofree char *command = g_strdup_printf(
            "-M %s", machines[index]);
        g_autofree char *migration_dir = NULL;
        g_autofree char *migration_socket = NULL;
        QTestState *source = qtest_init(command);
        QTestState *destination;

        /* 19.2 MHz XOSC / 192 = 100 kHz, visible to both PWM blocks. */
        qtest_writel(source, BCM2711_CPRMAN_BASE + CPRMAN_PWM_DIV,
                     CPRMAN_PASSWORD | (192 << 12));
        qtest_writel(source, BCM2711_CPRMAN_BASE + CPRMAN_PWM_CTL,
                     CPRMAN_PASSWORD | BIT(4) | 1);
        g_assert_cmpuint(qom_path_get_uint32(
                             source, PWM_QOM_PATH, "clock-frequency"), ==,
                         100000);
        g_assert_cmpuint(qom_path_get_uint32(
                             source, PWM1_QOM_PATH, "clock-frequency"), ==,
                         100000);
        g_assert_cmpuint(bcm2711_firmware_clock_call(
                             source, RPI_FWREQ_GET_CLOCK_STATE,
                             RPI_FIRMWARE_PWM_CLK_ID, 0), ==, 1);
        g_assert_cmpuint(bcm2711_firmware_clock_call(
                             source, RPI_FWREQ_GET_CLOCK_RATE,
                             RPI_FIRMWARE_PWM_CLK_ID, 0), ==, 100000);
        g_assert_cmpuint(bcm2711_firmware_clock_call(
                             source, RPI_FWREQ_GET_CLOCK_MEASURED,
                             RPI_FIRMWARE_PWM_CLK_ID, 0), ==, 100000);

        g_assert_cmpuint(bcm2711_firmware_clock_call(
                             source, RPI_FWREQ_SET_CLOCK_STATE,
                             RPI_FIRMWARE_PWM_CLK_ID, 0), ==, 0);
        g_assert_cmpuint(qom_path_get_uint32(
                             source, PWM_QOM_PATH, "clock-frequency"), ==, 0);
        g_assert_cmpuint(qom_path_get_uint32(
                             source, PWM1_QOM_PATH, "clock-frequency"), ==, 0);
        g_assert_cmpuint(bcm2711_firmware_clock_call(
                             source, RPI_FWREQ_GET_CLOCK_RATE,
                             RPI_FIRMWARE_PWM_CLK_ID, 0), ==, 100000);
        g_assert_cmpuint(bcm2711_firmware_clock_call(
                             source, RPI_FWREQ_GET_CLOCK_MEASURED,
                             RPI_FIRMWARE_PWM_CLK_ID, 0), ==, 0);

        g_assert_cmpuint(bcm2711_firmware_clock_call(
                             source, RPI_FWREQ_SET_CLOCK_RATE,
                             RPI_FIRMWARE_PWM_CLK_ID, 200000), ==, 200000);
        g_assert_cmpuint(bcm2711_firmware_clock_call(
                             source, RPI_FWREQ_GET_CLOCK_RATE,
                             RPI_FIRMWARE_PWM_CLK_ID, 0), ==, 200000);
        g_assert_cmpuint(bcm2711_firmware_clock_call(
                             source, RPI_FWREQ_GET_CLOCK_MEASURED,
                             RPI_FIRMWARE_PWM_CLK_ID, 0), ==, 0);
        g_assert_cmpuint(bcm2711_firmware_clock_call(
                             source, RPI_FWREQ_SET_CLOCK_STATE,
                             RPI_FIRMWARE_PWM_CLK_ID, 1), ==, 1);
        g_assert_cmpuint(qom_path_get_uint32(
                             source, PWM_QOM_PATH, "clock-frequency"), ==,
                         200000);
        g_assert_cmpuint(qom_path_get_uint32(
                             source, PWM1_QOM_PATH, "clock-frequency"), ==,
                         200000);

        g_assert_cmpuint(bcm2711_firmware_clock_call(
                             source, RPI_FWREQ_GET_CLOCK_STATE,
                             RPI_FIRMWARE_HEVC_CLK_ID, 0), ==, 2);
        g_assert_cmpuint(bcm2711_firmware_clock_call(
                             source, RPI_FWREQ_GET_CLOCK_RATE,
                             RPI_FIRMWARE_HEVC_CLK_ID, 0), ==, 0);
        g_assert_cmpuint(bcm2711_firmware_clock_call(
                             source, RPI_FWREQ_SET_CLOCK_RATE,
                             RPI_FIRMWARE_HEVC_CLK_ID, 200000), ==, 0);

        destination = migrate_to_new_qtest(
            source, command, &migration_dir, &migration_socket);
        g_assert_cmpuint(bcm2711_firmware_clock_call(
                             destination, RPI_FWREQ_GET_CLOCK_STATE,
                             RPI_FIRMWARE_PWM_CLK_ID, 0), ==, 1);
        g_assert_cmpuint(bcm2711_firmware_clock_call(
                             destination, RPI_FWREQ_GET_CLOCK_RATE,
                             RPI_FIRMWARE_PWM_CLK_ID, 0), ==, 200000);
        g_assert_cmpuint(bcm2711_firmware_clock_call(
                             destination, RPI_FWREQ_GET_CLOCK_MEASURED,
                             RPI_FIRMWARE_PWM_CLK_ID, 0), ==, 200000);
        g_assert_cmpuint(qom_path_get_uint32(
                             destination, PWM_QOM_PATH,
                             "clock-frequency"), ==, 200000);
        g_assert_cmpuint(qom_path_get_uint32(
                             destination, PWM1_QOM_PATH,
                             "clock-frequency"), ==, 200000);

        qtest_system_reset(destination);
        g_assert_cmpuint(bcm2711_firmware_clock_call(
                             destination, RPI_FWREQ_GET_CLOCK_STATE,
                             RPI_FIRMWARE_PWM_CLK_ID, 0), ==, 0);
        g_assert_cmpuint(bcm2711_firmware_clock_call(
                             destination, RPI_FWREQ_GET_CLOCK_MEASURED,
                             RPI_FIRMWARE_PWM_CLK_ID, 0), ==, 0);

        qtest_quit(source);
        qtest_quit(destination);
        unlink(migration_socket);
        rmdir(migration_dir);
    }
}
void test_bcm2711_firmware_temperature(void)
{
    const char *machines[] = { "raspi4b", "raspi-cm4" };

    for (size_t index = 0; index < ARRAY_SIZE(machines); index++) {
        g_autofree char *command = g_strdup_printf(
            "-M %s", machines[index]);
        g_autofree char *migration_dir = NULL;
        g_autofree char *migration_socket = NULL;
        QTestState *source = qtest_init(command);
        QTestState *destination;

        g_assert_cmpuint(bcm2711_get_firmware_temperature(
                             source, RPI_FWREQ_GET_TEMPERATURE), ==, 25000);
        g_assert_cmpuint(bcm2711_get_firmware_temperature(
                             source, RPI_FWREQ_GET_MAX_TEMPERATURE), ==,
                         85000);

        qom_set_int32(source, "thermal-temperature-millicelsius", 80000);
        qom_set_bool(source, "thermal-sensor-valid", false);
        g_assert_cmpuint(bcm2711_get_firmware_temperature(
                             source, RPI_FWREQ_GET_TEMPERATURE), ==, 80000);
        g_assert_false(qom_get_bool(source, "thermal-sensor-valid"));

        qtest_system_reset(source);
        g_assert_cmpuint(bcm2711_get_firmware_temperature(
                             source, RPI_FWREQ_GET_TEMPERATURE), ==, 80000);
        g_assert_false(qom_get_bool(source, "thermal-sensor-valid"));

        destination = migrate_to_new_qtest(
            source, command, &migration_dir, &migration_socket);
        g_assert_cmpint(qom_get_int32(
                            destination,
                            "thermal-temperature-millicelsius"), ==, 80000);
        g_assert_false(qom_get_bool(destination, "thermal-sensor-valid"));
        g_assert_cmpuint(bcm2711_get_firmware_temperature(
                             destination, RPI_FWREQ_GET_TEMPERATURE), ==,
                         80000);
        g_assert_cmpuint(bcm2711_get_firmware_temperature(
                             destination, RPI_FWREQ_GET_MAX_TEMPERATURE), ==,
                         85000);

        qtest_quit(source);
        qtest_quit(destination);
        unlink(migration_socket);
        rmdir(migration_dir);
    }
}
void test_bcm2711_firmware_throttled(void)
{
    const char *machines[] = { "raspi4b", "raspi-cm4" };

    for (size_t index = 0; index < ARRAY_SIZE(machines); index++) {
        g_autofree char *command = g_strdup_printf(
            "-M %s,firmware-throttled-current=5", machines[index]);
        g_autofree char *migration_dir = NULL;
        g_autofree char *migration_socket = NULL;
        QTestState *source = qtest_init(command);
        QTestState *destination;

        g_assert_cmphex(qom_get_uint32(
                            source, "firmware-throttled-current"), ==, 5);
        g_assert_cmphex(qom_get_uint32(
                            source, "firmware-throttled-status"), ==,
                        0x00050005);
        g_assert_cmphex(bcm2711_get_throttled(source), ==, 0x00050005);
        {
            QDict *response = qtest_qmp(
                source, "{ 'execute': 'qom-set', 'arguments': "
                        "{ 'path': '/machine', "
                        "'property': 'firmware-throttled-current', "
                        "'value': 8 } }");

            g_assert(qdict_haskey(response, "error"));
            qobject_unref(response);
        }
        g_assert_cmphex(bcm2711_get_throttled(source), ==, 0x00050005);

        qom_set_uint32(source, "firmware-throttled-current", 2);
        g_assert_cmphex(bcm2711_get_throttled(source), ==, 0x00070002);
        qtest_system_reset(source);
        g_assert_cmphex(qom_get_uint32(
                            source, "firmware-throttled-status"), ==,
                        0x00070002);
        g_assert_cmphex(bcm2711_get_throttled(source), ==, 0x00070002);

        qom_set_uint32(source, "firmware-throttled-current", 0);
        destination = migrate_to_new_qtest(
            source, command, &migration_dir, &migration_socket);
        g_assert_cmphex(qom_get_uint32(
                            destination, "firmware-throttled-current"), ==, 0);
        g_assert_cmphex(qom_get_uint32(
                            destination, "firmware-throttled-status"), ==,
                        0x00070000);
        g_assert_cmphex(bcm2711_get_throttled(destination), ==, 0x00070000);

        qom_set_uint32(destination, "firmware-throttled-current", 1);
        g_assert_cmphex(bcm2711_get_throttled(destination), ==, 0x00070001);

        qtest_quit(source);
        qtest_quit(destination);
        unlink(migration_socket);
        rmdir(migration_dir);
    }
}
void test_bcm2711_firmware_framebuffer(void)
{
    const char *command = "-M raspi4b";
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    g_autofree char *screendump_path = NULL;
    g_autofree char *screendump = NULL;
    QTestState *qts = qtest_init(command);
    QTestState *destination;
    size_t screendump_size;
    int screendump_fd;
    uint32_t small_dimensions[2] = { 2, 1 };
    uint32_t small_virtual_dimensions[2] = { 4, 1 };
    uint32_t allocation[2] = { 16, 0 };
    uint32_t dimensions[2] = { 800, 600 };
    uint32_t virtual_dimensions[2] = { 320, 900 };
    uint32_t offset[2] = { UINT32_MAX, UINT32_MAX };
    uint32_t value[1] = { 24 };
    uint32_t blank[1] = { 1 };
    uint32_t palette[2] = { 255, 2 };
    static const uint8_t pixels[] = {
        0, 0, 0,
        255, 0, 0,
        0, 255, 0,
        0, 0, 255,
    };
    static const char ppm_header[] = "P6\n2 1\n255\n";
    static const uint8_t expected_pixels[] = {
        255, 0, 0,
        0, 255, 0,
    };

    bcm2711_property_call(
        qts, RPI_FWREQ_FRAMEBUFFER_SET_PHYSICAL_WIDTH_HEIGHT,
        small_dimensions, G_N_ELEMENTS(small_dimensions));
    bcm2711_property_call(
        qts, RPI_FWREQ_FRAMEBUFFER_SET_VIRTUAL_WIDTH_HEIGHT,
        small_virtual_dimensions, G_N_ELEMENTS(small_virtual_dimensions));
    bcm2711_property_call(qts, RPI_FWREQ_FRAMEBUFFER_SET_DEPTH,
                          value, G_N_ELEMENTS(value));
    bcm2711_property_call(qts, RPI_FWREQ_FRAMEBUFFER_ALLOCATE,
                          allocation, G_N_ELEMENTS(allocation));
    g_assert_cmpuint(allocation[0], !=, 0);
    g_assert_cmpuint(allocation[1], ==, sizeof(pixels));
    qtest_memwrite(qts, allocation[0], pixels, sizeof(pixels));
    offset[0] = 1;
    offset[1] = 0;
    bcm2711_property_call(
        qts, RPI_FWREQ_FRAMEBUFFER_SET_VIRTUAL_OFFSET,
        offset, G_N_ELEMENTS(offset));
    g_assert_cmpuint(offset[0], ==, 1);
    g_assert_cmpuint(offset[1], ==, 0);

    screendump_fd = g_file_open_tmp(
        "raspi4-framebuffer-XXXXXX.ppm", &screendump_path, NULL);
    g_assert_cmpint(screendump_fd, >=, 0);
    close(screendump_fd);
    qtest_qmp_assert_success(
        qts,
        "{ 'execute': 'screendump',"
        "  'arguments': { 'filename': %s } }",
        screendump_path);
    g_assert_true(g_file_get_contents(
        screendump_path, &screendump, &screendump_size, NULL));
    g_assert_cmpuint(screendump_size, ==,
                     strlen(ppm_header) + 8);
    g_assert_cmpmem(screendump, strlen(ppm_header),
                    ppm_header, strlen(ppm_header));
    g_assert_cmpmem(screendump + strlen(ppm_header),
                    sizeof(expected_pixels),
                    expected_pixels, sizeof(expected_pixels));

    bcm2711_property_call(qts, RPI_FWREQ_FRAMEBUFFER_BLANK,
                          blank, G_N_ELEMENTS(blank));
    g_clear_pointer(&screendump, g_free);
    qtest_qmp_assert_success(
        qts,
        "{ 'execute': 'screendump',"
        "  'arguments': { 'filename': %s } }",
        screendump_path);
    g_assert_true(g_file_get_contents(
        screendump_path, &screendump, &screendump_size, NULL));
    g_assert_cmpuint(screendump_size, ==, strlen(ppm_header) + 8);
    for (size_t i = strlen(ppm_header); i < screendump_size; i++) {
        g_assert_cmpuint((uint8_t)screendump[i], ==, 0);
    }
    blank[0] = 0;
    bcm2711_property_call(qts, RPI_FWREQ_FRAMEBUFFER_BLANK,
                          blank, G_N_ELEMENTS(blank));
    unlink(screendump_path);

    bcm2711_property_call(
        qts, RPI_FWREQ_FRAMEBUFFER_SET_PHYSICAL_WIDTH_HEIGHT,
        dimensions, G_N_ELEMENTS(dimensions));
    g_assert_cmpuint(dimensions[0], ==, 800);
    g_assert_cmpuint(dimensions[1], ==, 600);

    bcm2711_property_call(
        qts, RPI_FWREQ_FRAMEBUFFER_SET_VIRTUAL_WIDTH_HEIGHT,
        virtual_dimensions, G_N_ELEMENTS(virtual_dimensions));
    g_assert_cmpuint(virtual_dimensions[0], ==, 800);
    g_assert_cmpuint(virtual_dimensions[1], ==, 900);

    offset[0] = UINT32_MAX;
    offset[1] = UINT32_MAX;
    bcm2711_property_call(
        qts, RPI_FWREQ_FRAMEBUFFER_SET_VIRTUAL_OFFSET,
        offset, G_N_ELEMENTS(offset));
    g_assert_cmpuint(offset[0], ==, 0);
    g_assert_cmpuint(offset[1], ==, 300);

    bcm2711_property_call(qts, RPI_FWREQ_FRAMEBUFFER_SET_DEPTH,
                          value, G_N_ELEMENTS(value));
    g_assert_cmpuint(value[0], ==, 24);
    value[0] = 0;
    bcm2711_property_call(qts, RPI_FWREQ_FRAMEBUFFER_GET_PITCH,
                          value, G_N_ELEMENTS(value));
    g_assert_cmpuint(value[0], ==, 2400);

    value[0] = 1;
    bcm2711_property_call(qts, RPI_FWREQ_FRAMEBUFFER_BLANK,
                          value, G_N_ELEMENTS(value));
    g_assert_cmpuint(value[0], ==, 1);
    g_assert_true(qtest_qom_get_bool(
        qts, FRAMEBUFFER_QOM_PATH, "blank"));

    bcm2711_property_call_response(
        qts, RPI_FWREQ_FRAMEBUFFER_SET_PALETTE,
        palette, G_N_ELEMENTS(palette), 1);
    g_assert_cmpuint(palette[0], ==, 1);

    destination = migrate_to_new_qtest(
        qts, command, &migration_dir, &migration_socket);
    qtest_quit(qts);
    qts = destination;
    g_assert_true(qtest_qom_get_bool(
        qts, FRAMEBUFFER_QOM_PATH, "blank"));

    memset(virtual_dimensions, 0, sizeof(virtual_dimensions));
    bcm2711_property_call(
        qts, RPI_FWREQ_FRAMEBUFFER_GET_VIRTUAL_WIDTH_HEIGHT,
        virtual_dimensions, G_N_ELEMENTS(virtual_dimensions));
    g_assert_cmpuint(virtual_dimensions[0], ==, 800);
    g_assert_cmpuint(virtual_dimensions[1], ==, 900);
    memset(offset, 0, sizeof(offset));
    bcm2711_property_call(
        qts, RPI_FWREQ_FRAMEBUFFER_GET_VIRTUAL_OFFSET,
        offset, G_N_ELEMENTS(offset));
    g_assert_cmpuint(offset[0], ==, 0);
    g_assert_cmpuint(offset[1], ==, 300);

    value[0] = 0;
    bcm2711_property_call(qts, RPI_FWREQ_FRAMEBUFFER_BLANK,
                          value, G_N_ELEMENTS(value));
    g_assert_false(qtest_qom_get_bool(
        qts, FRAMEBUFFER_QOM_PATH, "blank"));
    qtest_system_reset(qts);
    g_assert_false(qtest_qom_get_bool(
        qts, FRAMEBUFFER_QOM_PATH, "blank"));

    qtest_quit(qts);
    unlink(migration_socket);
    rmdir(migration_dir);
}
void test_bcm2711_firmware_framebuffer_release(void)
{
    static const char * const machines[] = { "raspi4b", "raspi-cm4" };
    static const uint8_t green[3] = { 0, 255, 0 };
    static const uint8_t red[3] = { 255, 0, 0 };
    static const uint8_t black[3] = { 0, 0, 0 };

    for (size_t index = 0; index < ARRAY_SIZE(machines); index++) {
        g_autofree char *command = g_strdup_printf(
            "-M %s", machines[index]);
        g_autofree char *migration_dir = NULL;
        g_autofree char *migration_socket = NULL;
        g_autofree char *screendump_path = NULL;
        QTestState *source = qtest_init(command);
        QTestState *destination;
        uint32_t dimensions[2] = { 1, 1 };
        uint32_t allocation[2] = { 16, 0 };
        uint32_t depth = 24;
        uint32_t allocated_base;
        int screendump_fd;

        bcm2711_property_call(
            source, RPI_FWREQ_FRAMEBUFFER_SET_PHYSICAL_WIDTH_HEIGHT,
            dimensions, G_N_ELEMENTS(dimensions));
        bcm2711_property_call(
            source, RPI_FWREQ_FRAMEBUFFER_SET_VIRTUAL_WIDTH_HEIGHT,
            dimensions, G_N_ELEMENTS(dimensions));
        bcm2711_property_call(
            source, RPI_FWREQ_FRAMEBUFFER_SET_DEPTH, &depth, 1);
        bcm2711_property_call(
            source, RPI_FWREQ_FRAMEBUFFER_ALLOCATE,
            allocation, G_N_ELEMENTS(allocation));
        allocated_base = allocation[0];
        g_assert_cmpuint(allocated_base, !=, 0);
        g_assert_cmpuint(allocation[1], ==, 3);
        g_assert_true(qtest_qom_get_bool(
            source, FRAMEBUFFER_QOM_PATH, "enabled"));
        qtest_memwrite(source, allocated_base, green, sizeof(green));

        screendump_fd = g_file_open_tmp(
            "raspi4-framebuffer-release-XXXXXX.ppm",
            &screendump_path, NULL);
        g_assert_cmpint(screendump_fd, >=, 0);
        close(screendump_fd);
        assert_bcm2711_framebuffer_pixel(
            source, screendump_path, green);

        bcm2711_property_call_response(
            source, RPI_FWREQ_FRAMEBUFFER_RELEASE, NULL, 0, 0);
        g_assert_false(qtest_qom_get_bool(
            source, FRAMEBUFFER_QOM_PATH, "enabled"));
        assert_bcm2711_framebuffer_pixel(
            source, screendump_path, black);

        destination = migrate_to_new_qtest(
            source, command, &migration_dir, &migration_socket);
        g_assert_false(qtest_qom_get_bool(
            destination, FRAMEBUFFER_QOM_PATH, "enabled"));
        assert_bcm2711_framebuffer_pixel(
            destination, screendump_path, black);

        allocation[0] = 16;
        allocation[1] = 0;
        bcm2711_property_call(
            destination, RPI_FWREQ_FRAMEBUFFER_ALLOCATE,
            allocation, G_N_ELEMENTS(allocation));
        g_assert_cmphex(allocation[0], ==, allocated_base);
        g_assert_cmpuint(allocation[1], ==, 3);
        g_assert_true(qtest_qom_get_bool(
            destination, FRAMEBUFFER_QOM_PATH, "enabled"));
        qtest_memwrite(destination, allocation[0], red, sizeof(red));
        assert_bcm2711_framebuffer_pixel(
            destination, screendump_path, red);

        bcm2711_property_call_response(
            destination, RPI_FWREQ_FRAMEBUFFER_RELEASE, NULL, 0, 0);
        qtest_system_reset(destination);
        g_assert_true(qtest_qom_get_bool(
            destination, FRAMEBUFFER_QOM_PATH, "enabled"));
        memset(dimensions, 0, sizeof(dimensions));
        bcm2711_property_call(
            destination,
            RPI_FWREQ_FRAMEBUFFER_GET_PHYSICAL_WIDTH_HEIGHT,
            dimensions, G_N_ELEMENTS(dimensions));
        g_assert_cmpuint(dimensions[0], ==, 640);
        g_assert_cmpuint(dimensions[1], ==, 480);

        qtest_quit(source);
        qtest_quit(destination);
        unlink(screendump_path);
        unlink(migration_socket);
        rmdir(migration_dir);
    }
}
void test_bcm2711_firmware_expander_gpio(void)
{
    const char *command = "-M raspi4b";
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    QTestState *qts = qtest_init(command);
    QTestState *destination;
    uint32_t config[6] = { 128 };
    uint32_t state[2] = { 128 };

    qtest_irq_intercept_out_named(qts, PROPERTY_QOM_PATH, "exp-gpio-out");
    bcm2711_property_call(qts, RPI_FWREQ_GET_GPIO_CONFIG, config, 5);
    for (size_t i = 0; i < 5; i++) {
        g_assert_cmpuint(config[i], ==, 0);
    }
    qtest_set_irq_in(qts, PROPERTY_QOM_PATH, "exp-gpio-in", 0, 1);
    state[0] = 128;
    state[1] = 0;
    bcm2711_property_call(qts, RPI_FWREQ_GET_GPIO_STATE, state, 2);
    g_assert_cmpuint(state[0], ==, 0);
    g_assert_cmpuint(state[1], ==, 1);
    g_assert_true(qtest_get_irq(qts, 0));

    config[0] = 130;
    config[1] = 1;
    config[2] = 1;
    config[3] = 1;
    config[4] = 0;
    config[5] = 1;
    bcm2711_property_call(qts, RPI_FWREQ_SET_GPIO_CONFIG, config, 6);
    g_assert_cmpuint(config[0], ==, 0);
    g_assert_true(qtest_get_irq(qts, 2));

    memset(config, 0, sizeof(config));
    config[0] = 130;
    bcm2711_property_call(qts, RPI_FWREQ_GET_GPIO_CONFIG, config, 5);
    g_assert_cmpuint(config[0], ==, 0);
    g_assert_cmpuint(config[1], ==, 1);
    g_assert_cmpuint(config[2], ==, 1);
    g_assert_cmpuint(config[3], ==, 1);
    g_assert_cmpuint(config[4], ==, 0);

    state[0] = 130;
    state[1] = 0;
    bcm2711_property_call(qts, RPI_FWREQ_GET_GPIO_STATE, state, 2);
    g_assert_cmpuint(state[0], ==, 0);
    g_assert_cmpuint(state[1], ==, 1);

    destination = migrate_to_new_qtest(
        qts, command, &migration_dir, &migration_socket);
    qtest_quit(qts);
    qts = destination;
    memset(config, 0, sizeof(config));
    config[0] = 130;
    bcm2711_property_call(qts, RPI_FWREQ_GET_GPIO_CONFIG, config, 5);
    g_assert_cmpuint(config[0], ==, 0);
    g_assert_cmpuint(config[1], ==, 1);
    g_assert_cmpuint(config[2], ==, 1);
    g_assert_cmpuint(config[3], ==, 1);
    g_assert_cmpuint(config[4], ==, 0);
    state[0] = 130;
    state[1] = 0;
    bcm2711_property_call(qts, RPI_FWREQ_GET_GPIO_STATE, state, 2);
    g_assert_cmpuint(state[0], ==, 0);
    g_assert_cmpuint(state[1], ==, 1);
    qtest_irq_intercept_out_named(qts, PROPERTY_QOM_PATH, "exp-gpio-out");

    state[0] = 130;
    state[1] = 0;
    bcm2711_property_call(qts, RPI_FWREQ_SET_GPIO_STATE, state, 2);
    g_assert_cmpuint(state[0], ==, 0);
    g_assert_cmpuint(state[1], ==, 0);
    g_assert_false(qtest_get_irq(qts, 2));

    state[0] = 127;
    state[1] = 1;
    bcm2711_property_call(qts, RPI_FWREQ_SET_GPIO_STATE, state, 2);
    g_assert_cmpuint(state[0], ==, 127);

    qtest_system_reset(qts);
    memset(config, 0xff, sizeof(config));
    config[0] = 130;
    bcm2711_property_call(qts, RPI_FWREQ_GET_GPIO_CONFIG, config, 5);
    for (size_t i = 0; i < 5; i++) {
        g_assert_cmpuint(config[i], ==, 0);
    }
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 2));
    qtest_quit(qts);
    unlink(migration_socket);
    rmdir(migration_dir);
}
void test_bcm2711_pwm_registers(void)
{
    QTestState *qts = qtest_init("-M raspi4b");
    uint32_t status;

    qtest_irq_intercept_out_named(qts, PWM_QOM_PATH, "channel-enabled");
    g_assert_cmphex(qtest_readl(qts, BCM2711_PWM_BASE + PWM_CTL), ==, 0);
    g_assert_cmphex(qtest_readl(qts, BCM2711_PWM_BASE + PWM_STA), ==,
                    PWM_STA_EMPT1);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));

    qtest_writel(qts, BCM2711_PWM_BASE + PWM_RNG1, 1000);
    qtest_writel(qts, BCM2711_PWM_BASE + PWM_DAT1, 250);
    qtest_writel(qts, BCM2711_PWM_BASE + PWM_RNG2, 2000);
    qtest_writel(qts, BCM2711_PWM_BASE + PWM_DAT2, 1500);
    g_assert_cmpuint(qtest_readl(qts, BCM2711_PWM_BASE + PWM_RNG1), ==,
                     1000);
    g_assert_cmpuint(qtest_readl(qts, BCM2711_PWM_BASE + PWM_DAT1), ==,
                     250);
    g_assert_cmpuint(qtest_readl(qts, BCM2711_PWM_BASE + PWM_RNG2), ==,
                     2000);
    g_assert_cmpuint(qtest_readl(qts, BCM2711_PWM_BASE + PWM_DAT2), ==,
                     1500);

    /* Reserved CTL/DMAC bits read as zero; CLRF is a self-clearing command. */
    qtest_writel(qts, BCM2711_PWM_BASE + PWM_DMAC, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, BCM2711_PWM_BASE + PWM_DMAC), ==,
                    0x8000ffff);
    qtest_writel(qts, BCM2711_PWM_BASE + PWM_CTL,
                 BIT(0) | BIT(4) | BIT(7) | BIT(8));
    g_assert_cmphex(qtest_readl(qts, BCM2711_PWM_BASE + PWM_CTL), ==,
                    BIT(0) | BIT(4) | BIT(7) | BIT(8));
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));
    status = qtest_readl(qts, BCM2711_PWM_BASE + PWM_STA);
    g_assert_cmphex(status & (PWM_STA_STA1 | PWM_STA_STA2), ==,
                    PWM_STA_STA1 | PWM_STA_STA2);

    /* The hardware FIFO holds exactly 16 words and preserves ordering. */
    for (uint32_t i = 0; i < 16; i++) {
        qtest_writel(qts, BCM2711_PWM_BASE + PWM_FIF1, 0x100 + i);
    }
    status = qtest_readl(qts, BCM2711_PWM_BASE + PWM_STA);
    g_assert_true(status & PWM_STA_FULL1);
    g_assert_false(status & PWM_STA_EMPT1);
    qtest_writel(qts, BCM2711_PWM_BASE + PWM_FIF1, 0xdeadbeef);
    g_assert_true(qtest_readl(qts, BCM2711_PWM_BASE + PWM_STA) &
                  PWM_STA_WERR1);
    g_assert_cmphex(qtest_readl(qts, BCM2711_PWM_BASE + PWM_FIF1), ==,
                    0x100);
    qtest_writel(qts, BCM2711_PWM_BASE + PWM_STA, PWM_STA_WERR1);
    g_assert_false(qtest_readl(qts, BCM2711_PWM_BASE + PWM_STA) &
                   PWM_STA_WERR1);

    qtest_writel(qts, BCM2711_PWM_BASE + PWM_CTL,
                 BIT(0) | BIT(6) | BIT(8));
    g_assert_cmphex(qtest_readl(qts, BCM2711_PWM_BASE + PWM_CTL), ==,
                    BIT(0) | BIT(8));
    g_assert_true(qtest_readl(qts, BCM2711_PWM_BASE + PWM_STA) &
                  PWM_STA_EMPT1);
    g_assert_cmphex(qtest_readl(qts, BCM2711_PWM_BASE + PWM_FIF1), ==, 0);
    g_assert_true(qtest_readl(qts, BCM2711_PWM_BASE + PWM_STA) &
                  PWM_STA_RERR1);
    qtest_writel(qts, BCM2711_PWM_BASE + PWM_STA, PWM_STA_RERR1);
    g_assert_false(qtest_readl(qts, BCM2711_PWM_BASE + PWM_STA) &
                   PWM_STA_RERR1);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, BCM2711_PWM_BASE + PWM_CTL), ==, 0);
    g_assert_cmphex(qtest_readl(qts, BCM2711_PWM_BASE + PWM_DMAC), ==, 0);
    g_assert_cmphex(qtest_readl(qts, BCM2711_PWM_BASE + PWM_RNG1), ==, 0);
    g_assert_cmphex(qtest_readl(qts, BCM2711_PWM_BASE + PWM_DAT2), ==, 0);
    g_assert_cmphex(qtest_readl(qts, BCM2711_PWM_BASE + PWM_STA), ==,
                    PWM_STA_EMPT1);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));
    qtest_quit(qts);
}
void test_bcm2711_pwm_clocked_fifo(void)
{
    QTestState *qts = qtest_init("-M raspi4b");

    qtest_irq_intercept_out_named(qts, PWM_QOM_PATH, "dma-threshold");
    configure_pwm_clock(qts);
    qtest_writel(qts, BCM2711_PWM_BASE + PWM_RNG1, 10);
    qtest_writel(qts, BCM2711_PWM_BASE + PWM_FIF1, 0x11111111);
    qtest_writel(qts, BCM2711_PWM_BASE + PWM_FIF1, 0x22222222);
    qtest_writel(qts, BCM2711_PWM_BASE + PWM_FIF1, 0x33333333);
    qtest_writel(qts, BCM2711_PWM_BASE + PWM_DMAC,
                 PWM_DMAC_ENABLE | (2 << 8) | 1);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));
    qtest_writel(qts, BCM2711_PWM_BASE + PWM_CTL,
                 BIT(0) | PWM_CTL_USEF1);

    g_assert_cmphex(qtest_readl(qts, BCM2711_PWM_BASE + PWM_DAT1), ==,
                    0x11111111);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));
    qtest_clock_step(qts, 100000 - 1);
    g_assert_cmphex(qtest_readl(qts, BCM2711_PWM_BASE + PWM_DAT1), ==,
                    0x11111111);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readl(qts, BCM2711_PWM_BASE + PWM_DAT1), ==,
                    0x22222222);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));

    qtest_clock_step(qts, 100000);
    g_assert_cmphex(qtest_readl(qts, BCM2711_PWM_BASE + PWM_DAT1), ==,
                    0x33333333);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));
    qtest_clock_step(qts, 100000);
    g_assert_true(qtest_readl(qts, BCM2711_PWM_BASE + PWM_STA) &
                  PWM_STA_EMPT1);
    g_assert_true(qtest_readl(qts, BCM2711_PWM_BASE + PWM_STA) &
                  PWM_STA_GAPO1);
    g_assert_false(qtest_readl(qts, BCM2711_PWM_BASE + PWM_STA) &
                   PWM_STA_RERR1);
    qtest_writel(qts, BCM2711_PWM_BASE + PWM_STA, PWM_STA_GAPO1);
    g_assert_false(qtest_readl(qts, BCM2711_PWM_BASE + PWM_STA) &
                   PWM_STA_GAPO1);

    /* RPTL repeats the last FIFO sample without reporting a gap. */
    qtest_writel(qts, BCM2711_PWM_BASE + PWM_CTL,
                 BIT(0) | PWM_CTL_USEF1 | PWM_CTL_RPTL1);
    qtest_writel(qts, BCM2711_PWM_BASE + PWM_FIF1, 0x44444444);
    g_assert_cmphex(qtest_readl(qts, BCM2711_PWM_BASE + PWM_DAT1), ==,
                    0x44444444);
    qtest_clock_step(qts, 100000);
    g_assert_cmphex(qtest_readl(qts, BCM2711_PWM_BASE + PWM_DAT1), ==,
                    0x44444444);
    g_assert_false(qtest_readl(qts, BCM2711_PWM_BASE + PWM_STA) &
                   PWM_STA_GAPO1);
    qtest_quit(qts);
}
void test_bcm2711_pwm_clock_migration(void)
{
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    QTestState *source = qtest_init("-M raspi4b");
    QTestState *destination;
    uint32_t pwm_ctl;

    configure_pwm_clock(source);
    pwm_ctl = qtest_readl(
        source, BCM2711_CPRMAN_BASE + CPRMAN_PWM_CTL);
    qtest_writel(source, BCM2711_PWM_BASE + PWM_RNG1, 10);
    qtest_writel(source, BCM2711_PWM_BASE + PWM_FIF1, 0xaaaaaaaa);
    qtest_writel(source, BCM2711_PWM_BASE + PWM_FIF1, 0xbbbbbbbb);
    qtest_writel(source, BCM2711_PWM_BASE + PWM_CTL,
                 BIT(0) | PWM_CTL_USEF1);
    qtest_clock_step(source, 50000);
    qtest_writel(source, BCM2711_CPRMAN_BASE + CPRMAN_PWM_CTL,
                 CPRMAN_PASSWORD | (pwm_ctl & ~BIT(4)));
    g_assert_cmpuint(qom_path_get_uint32(
                         source, PWM_QOM_PATH, "clock-frequency"), ==, 0);

    destination = migrate_to_new_qtest(
        source, "-M raspi4b", &migration_dir, &migration_socket);
    qtest_quit(source);
    g_assert_cmpuint(qom_path_get_uint32(
                         destination, PWM_QOM_PATH,
                         "clock-frequency"), ==, 0);
    g_assert_cmphex(qtest_readl(
                        destination, BCM2711_PWM_BASE + PWM_DAT1), ==,
                    0xaaaaaaaa);

    qtest_writel(destination, BCM2711_CPRMAN_BASE + CPRMAN_PWM_CTL,
                 CPRMAN_PASSWORD | pwm_ctl);
    qtest_clock_step(destination, 50000 - 1);
    g_assert_cmphex(qtest_readl(
                        destination, BCM2711_PWM_BASE + PWM_DAT1), ==,
                    0xaaaaaaaa);
    qtest_clock_step(destination, 1);
    g_assert_cmphex(qtest_readl(
                        destination, BCM2711_PWM_BASE + PWM_DAT1), ==,
                    0xbbbbbbbb);
    qtest_clock_step(destination, 100000);
    g_assert_true(qtest_readl(
                      destination, BCM2711_PWM_BASE + PWM_STA) &
                  PWM_STA_EMPT1);
    g_assert_true(qtest_readl(
                      destination, BCM2711_PWM_BASE + PWM_STA) &
                  PWM_STA_GAPO1);

    qtest_system_reset(destination);
    g_assert_cmphex(qtest_readl(
                        destination, BCM2711_PWM_BASE + PWM_CTL), ==, 0);
    g_assert_cmphex(qtest_readl(
                        destination, BCM2711_PWM_BASE + PWM_STA), ==,
                    PWM_STA_EMPT1);
    qtest_quit(destination);
    unlink(migration_socket);
    rmdir(migration_dir);
}
void test_bcm2711_pwm_waveform_gpio_migration(void)
{
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    QTestState *source = qtest_init("-M raspi4b");
    QTestState *destination;
    uint32_t fsel;
    uint32_t pwm_ctl;

    qtest_irq_intercept_out_named(source, GPIO_QOM_PATH,
                                  "pin-output-enable");
    configure_pwm_clock(source);
    fsel = qtest_readl(source, BCM2711_GPIO_BASE + GPIO_GPFSEL1);
    fsel = deposit32(fsel, (12 - 10) * 3, 3, 4);
    fsel = deposit32(fsel, (13 - 10) * 3, 3, 4);
    fsel = deposit32(fsel, (18 - 10) * 3, 3, 2);
    fsel = deposit32(fsel, (19 - 10) * 3, 3, 2);
    qtest_writel(source, BCM2711_GPIO_BASE + GPIO_GPFSEL1, fsel);
    qtest_writel(source, BCM2711_GPIO_BASE + GPIO_GPFSEL4,
                 4 << ((45 - 40) * 3));
    qtest_writel(source, BCM2711_GPIO_BASE + GPIO_GPREN0, BIT(12));
    qtest_writel(source, BCM2711_GPIO_BASE + GPIO_GPFEN0, BIT(12));
    g_assert_true(qtest_get_irq(source, 12));
    g_assert_true(qtest_get_irq(source, 13));
    g_assert_true(qtest_get_irq(source, 18));
    g_assert_true(qtest_get_irq(source, 19));
    g_assert_true(qtest_get_irq(source, 45));

    /* Mark-space mode emits one exact high and low interval per period. */
    qtest_writel(source, BCM2711_PWM_BASE + PWM_RNG1, 10);
    qtest_writel(source, BCM2711_PWM_BASE + PWM_DAT1, 3);
    qtest_writel(source, BCM2711_PWM_BASE + PWM_CTL,
                 BIT(0) | PWM_CTL_MSEN1);
    g_assert_true(qtest_readl(source,
                             BCM2711_GPIO_BASE + GPIO_GPLEV0) & BIT(12));
    g_assert_true(qtest_readl(source,
                             BCM2711_GPIO_BASE + GPIO_GPLEV0) & BIT(18));
    g_assert_true(qtest_readl(source,
                             BCM2711_GPIO_BASE + GPIO_GPEDS0) & BIT(12));
    qtest_writel(source, BCM2711_GPIO_BASE + GPIO_GPEDS0, BIT(12));
    qtest_clock_step(source, 30000 - 1);
    g_assert_true(qtest_readl(source,
                             BCM2711_GPIO_BASE + GPIO_GPLEV0) & BIT(12));
    qtest_clock_step(source, 1);
    g_assert_false(qtest_readl(source,
                              BCM2711_GPIO_BASE + GPIO_GPLEV0) & BIT(12));
    g_assert_false(qtest_readl(source,
                              BCM2711_GPIO_BASE + GPIO_GPLEV0) & BIT(18));
    g_assert_true(qtest_readl(source,
                             BCM2711_GPIO_BASE + GPIO_GPEDS0) & BIT(12));

    /* Channel 1 fans out through GPIO13, GPIO19, and GPIO45. */
    qtest_writel(source, BCM2711_PWM_BASE + PWM_CTL, 0);
    qtest_writel(source, BCM2711_PWM_BASE + PWM_RNG2, 10);
    qtest_writel(source, BCM2711_PWM_BASE + PWM_DAT2, 7);
    qtest_writel(source, BCM2711_PWM_BASE + PWM_CTL,
                 BIT(8) | PWM_CTL_MSEN2);
    g_assert_true(qtest_readl(source,
                             BCM2711_GPIO_BASE + GPIO_GPLEV0) & BIT(13));
    g_assert_true(qtest_readl(source,
                             BCM2711_GPIO_BASE + GPIO_GPLEV0) & BIT(19));
    g_assert_true(qtest_readl(source,
                             BCM2711_GPIO_BASE + GPIO_GPLEV1) & BIT(13));
    qtest_clock_step(source, 70000);
    g_assert_false(qtest_readl(source,
                              BCM2711_GPIO_BASE + GPIO_GPLEV0) & BIT(13));
    g_assert_false(qtest_readl(source,
                              BCM2711_GPIO_BASE + GPIO_GPLEV0) & BIT(19));
    g_assert_false(qtest_readl(source,
                              BCM2711_GPIO_BASE + GPIO_GPLEV1) & BIT(13));

    /* The default PWM algorithm distributes a 4/8 duty cycle evenly. */
    qtest_writel(source, BCM2711_PWM_BASE + PWM_CTL, 0);
    qtest_writel(source, BCM2711_PWM_BASE + PWM_RNG1, 8);
    qtest_writel(source, BCM2711_PWM_BASE + PWM_DAT1, 4);
    qtest_writel(source, BCM2711_PWM_BASE + PWM_CTL, BIT(0));
    g_assert_false(qtest_readl(source,
                              BCM2711_GPIO_BASE + GPIO_GPLEV0) & BIT(12));
    for (unsigned cycle = 1; cycle < 4; cycle++) {
        qtest_clock_step(source, 10000);
        g_assert_cmpint(
            !!(qtest_readl(source,
                           BCM2711_GPIO_BASE + GPIO_GPLEV0) & BIT(12)),
            ==, cycle & 1);
    }

    /* Serializer mode sends the selected word MSB first. */
    qtest_writel(source, BCM2711_PWM_BASE + PWM_CTL, 0);
    qtest_writel(source, BCM2711_PWM_BASE + PWM_RNG1, 4);
    qtest_writel(source, BCM2711_PWM_BASE + PWM_DAT1, 0xa0000000);
    qtest_writel(source, BCM2711_PWM_BASE + PWM_CTL,
                 BIT(0) | PWM_CTL_MODE1);
    g_assert_true(qtest_readl(source,
                             BCM2711_GPIO_BASE + GPIO_GPLEV0) & BIT(12));
    for (unsigned cycle = 1; cycle < 4; cycle++) {
        qtest_clock_step(source, 10000);
        g_assert_cmpint(
            !!(qtest_readl(source,
                           BCM2711_GPIO_BASE + GPIO_GPLEV0) & BIT(12)),
            ==, !(cycle & 1));
    }

    /* POLA applies after serial/PWM generation; SBIT drives idle state. */
    qtest_writel(source, BCM2711_PWM_BASE + PWM_CTL, 0);
    qtest_writel(source, BCM2711_PWM_BASE + PWM_RNG1, 4);
    qtest_writel(source, BCM2711_PWM_BASE + PWM_DAT1, 0x80000000);
    qtest_writel(source, BCM2711_PWM_BASE + PWM_CTL,
                 BIT(0) | PWM_CTL_MODE1 | PWM_CTL_POLA1);
    g_assert_false(qtest_readl(source,
                              BCM2711_GPIO_BASE + GPIO_GPLEV0) & BIT(12));
    qtest_clock_step(source, 10000);
    g_assert_true(qtest_readl(source,
                             BCM2711_GPIO_BASE + GPIO_GPLEV0) & BIT(12));
    qtest_writel(source, BCM2711_PWM_BASE + PWM_CTL, PWM_CTL_SBIT1);
    g_assert_true(qtest_readl(source,
                             BCM2711_GPIO_BASE + GPIO_GPLEV0) & BIT(12));
    qtest_writel(source, BCM2711_PWM_BASE + PWM_CTL,
                 PWM_CTL_SBIT1 | PWM_CTL_POLA1);
    g_assert_false(qtest_readl(source,
                              BCM2711_GPIO_BASE + GPIO_GPLEV0) & BIT(12));

    /*
     * Migrate halfway through a mark interval with the CPRMAN clock stopped.
     * Both GPIO mux state and the remaining source cycles must survive.
     */
    qtest_writel(source, BCM2711_PWM_BASE + PWM_CTL, 0);
    qtest_writel(source, BCM2711_PWM_BASE + PWM_RNG1, 10);
    qtest_writel(source, BCM2711_PWM_BASE + PWM_DAT1, 3);
    qtest_writel(source, BCM2711_PWM_BASE + PWM_CTL,
                 BIT(0) | PWM_CTL_MSEN1);
    qtest_clock_step(source, 10000);
    pwm_ctl = qtest_readl(
        source, BCM2711_CPRMAN_BASE + CPRMAN_PWM_CTL);
    qtest_writel(source, BCM2711_CPRMAN_BASE + CPRMAN_PWM_CTL,
                 CPRMAN_PASSWORD | (pwm_ctl & ~BIT(4)));

    destination = migrate_to_new_qtest(
        source, "-M raspi4b", &migration_dir, &migration_socket);
    qtest_quit(source);
    g_assert_cmphex(qtest_readl(
                        destination,
                        BCM2711_GPIO_BASE + GPIO_GPFSEL1), ==, fsel);
    g_assert_true(qtest_readl(destination,
                             BCM2711_GPIO_BASE + GPIO_GPLEV0) & BIT(12));
    qtest_writel(destination, BCM2711_GPIO_BASE + GPIO_GPEDS0, BIT(12));
    qtest_writel(destination, BCM2711_CPRMAN_BASE + CPRMAN_PWM_CTL,
                 CPRMAN_PASSWORD | pwm_ctl);
    qtest_clock_step(destination, 20000 - 1);
    g_assert_true(qtest_readl(destination,
                             BCM2711_GPIO_BASE + GPIO_GPLEV0) & BIT(12));
    qtest_clock_step(destination, 1);
    g_assert_false(qtest_readl(destination,
                              BCM2711_GPIO_BASE + GPIO_GPLEV0) & BIT(12));
    g_assert_true(qtest_readl(destination,
                             BCM2711_GPIO_BASE + GPIO_GPEDS0) & BIT(12));
    qtest_writel(destination, BCM2711_GPIO_BASE + GPIO_GPEDS0, BIT(12));
    qtest_clock_step(destination, 70000);
    g_assert_true(qtest_readl(destination,
                             BCM2711_GPIO_BASE + GPIO_GPLEV0) & BIT(12));
    g_assert_true(qtest_readl(destination,
                             BCM2711_GPIO_BASE + GPIO_GPEDS0) & BIT(12));

    qtest_system_reset(destination);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_PWM_BASE + PWM_CTL), ==, 0);
    g_assert_false(qtest_readl(destination,
                              BCM2711_GPIO_BASE + GPIO_GPLEV0) & BIT(12));
    qtest_quit(destination);
    unlink(migration_socket);
    rmdir(migration_dir);
}
void test_bcm2711_pwm_dma_migration(void)
{
    const uint32_t cb_addr = 0x00100000;
    const uint32_t source_addr = 0x00101000;
    const uint32_t samples[] = {
        0x10101010, 0x20202020, 0x30303030, 0x40404040,
        0x50505050, 0x60606060, 0x70707070, 0x80808080,
    };
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    QTestState *source = qtest_init("-M raspi4b");
    QTestState *destination;
    uint32_t pwm_ctl;
    uint32_t dma_cs;

    configure_pwm_clock(source);
    pwm_ctl = qtest_readl(
        source, BCM2711_CPRMAN_BASE + CPRMAN_PWM_CTL);
    qtest_writel(source, BCM2711_PWM_BASE + PWM_RNG1, 10);
    qtest_writel(source, BCM2711_PWM_BASE + PWM_DMAC,
                 PWM_DMAC_ENABLE | (2 << 8) | 4);

    for (size_t i = 0; i < ARRAY_SIZE(samples); i++) {
        uint32_t offset = (i + (i >= 4)) * sizeof(samples[i]);

        qtest_writel(source, source_addr + offset,
                     samples[i]);
    }
    qtest_writel(source, cb_addr,
                 DMA_TI_INT_EN | DMA_TI_TDMODE | DMA_TI_D_DREQ |
                 DMA_TI_S_INC | DMA_TI_PERMAP_PWM);
    qtest_writel(source, cb_addr + 4, source_addr);
    qtest_writel(source, cb_addr + 8, DMA_PWM_FIFO_BUS);
    qtest_writel(source, cb_addr + 12,
                 BIT(16) | 4 * sizeof(samples[0]));
    qtest_writel(source, cb_addr + 16, sizeof(samples[0]));
    qtest_writel(source, cb_addr + 20, 0);
    qtest_writel(source, BCM2711_DMA_BASE + DMA_CB_ADDR, cb_addr);
    qtest_writel(source, BCM2711_DMA_BASE + DMA_CS, DMA_CS_ACTIVE);

    /*
     * Empty PWM asserts DREQ. DMA fills through the threshold, then remains
     * active and held with three source words outstanding.
     */
    dma_cs = qtest_readl(source, BCM2711_DMA_BASE + DMA_CS);
    g_assert_true(dma_cs & DMA_CS_ACTIVE);
    g_assert_true(dma_cs & DMA_CS_ISHELD);
    g_assert_false(dma_cs & DMA_CS_END);
    g_assert_cmpuint(qtest_readl(source, BCM2711_DMA_BASE + 0x0c), ==,
                     source_addr + 6 * sizeof(samples[0]));
    g_assert_cmpuint(qtest_readl(source, BCM2711_DMA_BASE + 0x14), ==,
                     BIT(16) | 3 * sizeof(samples[0]));

    qtest_writel(source, BCM2711_PWM_BASE + PWM_CTL,
                 BIT(0) | PWM_CTL_USEF1);
    g_assert_cmphex(qtest_readl(source,
                               BCM2711_PWM_BASE + PWM_DAT1), ==,
                    samples[0]);
    qtest_clock_step(source, 50000);
    qtest_writel(source, BCM2711_CPRMAN_BASE + CPRMAN_PWM_CTL,
                 CPRMAN_PASSWORD | (pwm_ctl & ~BIT(4)));

    destination = migrate_to_new_qtest(
        source, "-M raspi4b", &migration_dir, &migration_socket);
    qtest_quit(source);
    dma_cs = qtest_readl(destination, BCM2711_DMA_BASE + DMA_CS);
    g_assert_true(dma_cs & DMA_CS_ACTIVE);
    g_assert_true(dma_cs & DMA_CS_ISHELD);
    g_assert_cmpuint(qtest_readl(destination,
                                BCM2711_DMA_BASE + 0x14), ==,
                     BIT(16) | 8);

    qtest_writel(destination, BCM2711_CPRMAN_BASE + CPRMAN_PWM_CTL,
                 CPRMAN_PASSWORD | pwm_ctl);
    qtest_clock_step(destination, 50000 - 1);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_PWM_BASE + PWM_DAT1), ==,
                    samples[0]);

    for (size_t i = 1; i < ARRAY_SIZE(samples); i++) {
        qtest_clock_step(destination, i == 1 ? 1 : 100000);
        g_assert_cmphex(qtest_readl(destination,
                                   BCM2711_PWM_BASE + PWM_DAT1), ==,
                        samples[i]);
        if (i == 1) {
            dma_cs = qtest_readl(destination, BCM2711_DMA_BASE + DMA_CS);
            g_assert_true(dma_cs & DMA_CS_ACTIVE);
            g_assert_true(dma_cs & DMA_CS_ISHELD);
        }
    }

    dma_cs = qtest_readl(destination, BCM2711_DMA_BASE + DMA_CS);
    g_assert_false(dma_cs & DMA_CS_ACTIVE);
    g_assert_false(dma_cs & DMA_CS_ISHELD);
    g_assert_true(dma_cs & DMA_CS_END);
    g_assert_true(dma_cs & DMA_CS_INT);
    g_assert_cmpuint(qtest_readl(destination,
                                BCM2711_DMA_BASE + 0x14), ==, 0);
    g_assert_true(qtest_readl(destination, BCM2711_PWM_BASE + PWM_STA) &
                  PWM_STA_EMPT1);

    qtest_system_reset(destination);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DMA_BASE + DMA_CS), ==, 0);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_PWM_BASE + PWM_DMAC), ==, 0);
    qtest_quit(destination);
    unlink(migration_socket);
    rmdir(migration_dir);
}
void test_bcm2711_pwm1_gpio_dma_migration(void)
{
    const uint32_t dma_base = BCM2711_DMA_BASE + 0x100;
    const uint32_t cb_addr = 0x00100000;
    const uint32_t source_addr = 0x00101000;
    const uint32_t samples[] = { 3, 4, 5, 6, 7, 8, 9, 1 };
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    QTestState *source = qtest_init("-M raspi4b");
    QTestState *destination;
    QTestState *cm4;
    uint32_t fsel;
    uint32_t pwm_ctl;
    uint32_t dma_cs;

    qtest_irq_intercept_out_named(source, GPIO_QOM_PATH,
                                  "pin-output-enable");
    configure_pwm_clock(source);
    g_assert_cmpuint(qom_path_get_uint32(
                         source, PWM1_QOM_PATH, "clock-frequency"), ==,
                     100000);
    g_assert_cmphex(qtest_readl(source, BCM2711_PWM1_BASE + PWM_CTL), ==, 0);
    g_assert_cmphex(qtest_readl(source, BCM2711_PWM1_BASE + PWM_STA), ==,
                    PWM_STA_EMPT1);

    /* The two BCM2711 PWM blocks have independent MMIO and FIFO state. */
    qtest_writel(source, BCM2711_PWM_BASE + PWM_DAT1, 0xfeed0000);
    qtest_writel(source, BCM2711_PWM1_BASE + PWM_DAT1, 4);
    g_assert_cmphex(qtest_readl(source, BCM2711_PWM_BASE + PWM_DAT1), ==,
                    0xfeed0000);
    g_assert_cmphex(qtest_readl(source, BCM2711_PWM1_BASE + PWM_DAT1), ==, 4);

    /* GPIO40/41 ALT0 are PWM1 channels 0/1, not PWM0 aliases. */
    fsel = qtest_readl(source, BCM2711_GPIO_BASE + GPIO_GPFSEL4);
    fsel = deposit32(fsel, 0, 3, 4);
    fsel = deposit32(fsel, 3, 3, 4);
    qtest_writel(source, BCM2711_GPIO_BASE + GPIO_GPFSEL4, fsel);
    qtest_writel(source, BCM2711_GPIO_BASE + GPIO_GPREN1, BIT(8));
    qtest_writel(source, BCM2711_GPIO_BASE + GPIO_GPFEN1, BIT(8));
    g_assert_true(qtest_get_irq(source, 40));
    g_assert_true(qtest_get_irq(source, 41));

    qtest_writel(source, BCM2711_PWM1_BASE + PWM_RNG1, 10);
    qtest_writel(source, BCM2711_PWM1_BASE + PWM_CTL,
                 BIT(0) | PWM_CTL_MSEN1);
    g_assert_true(qtest_readl(source,
                             BCM2711_GPIO_BASE + GPIO_GPLEV1) & BIT(8));
    g_assert_true(qtest_readl(source,
                             BCM2711_GPIO_BASE + GPIO_GPEDS1) & BIT(8));
    qtest_writel(source, BCM2711_GPIO_BASE + GPIO_GPEDS1, BIT(8));
    qtest_clock_step(source, 40000 - 1);
    g_assert_true(qtest_readl(source,
                             BCM2711_GPIO_BASE + GPIO_GPLEV1) & BIT(8));
    qtest_clock_step(source, 1);
    g_assert_false(qtest_readl(source,
                              BCM2711_GPIO_BASE + GPIO_GPLEV1) & BIT(8));
    g_assert_true(qtest_readl(source,
                             BCM2711_GPIO_BASE + GPIO_GPEDS1) & BIT(8));

    /*
     * Peripheral map 1 refills PWM1's FIFO.  Migrate a held DMA control
     * block and PWM1 after the first high source-clock cycle with clk_pwm
     * stopped; resume must retain the exact two remaining high cycles.
     */
    qtest_writel(source, BCM2711_PWM1_BASE + PWM_CTL, BIT(6));
    qtest_writel(source, BCM2711_PWM1_BASE + PWM_RNG1, 10);
    qtest_writel(source, BCM2711_PWM1_BASE + PWM_DMAC,
                 PWM_DMAC_ENABLE | (2 << 8) | 4);
    for (size_t i = 0; i < ARRAY_SIZE(samples); i++) {
        qtest_writel(source, source_addr + i * sizeof(samples[i]),
                     samples[i]);
    }
    qtest_writel(source, cb_addr,
                 DMA_TI_INT_EN | DMA_TI_D_DREQ | DMA_TI_S_INC |
                 DMA_TI_PERMAP_PWM1);
    qtest_writel(source, cb_addr + 4, source_addr);
    qtest_writel(source, cb_addr + 8, DMA_PWM1_FIFO_BUS);
    qtest_writel(source, cb_addr + 12, sizeof(samples));
    qtest_writel(source, cb_addr + 16, 0);
    qtest_writel(source, cb_addr + 20, 0);
    qtest_writel(source, dma_base + DMA_CB_ADDR, cb_addr);
    qtest_writel(source, dma_base + DMA_CS, DMA_CS_ACTIVE);
    dma_cs = qtest_readl(source, dma_base + DMA_CS);
    g_assert_true(dma_cs & DMA_CS_ACTIVE);
    g_assert_true(dma_cs & DMA_CS_ISHELD);

    qtest_writel(source, BCM2711_PWM1_BASE + PWM_CTL,
                 BIT(0) | PWM_CTL_USEF1 | PWM_CTL_MSEN1);
    g_assert_cmphex(qtest_readl(source,
                               BCM2711_PWM1_BASE + PWM_DAT1), ==, samples[0]);
    qtest_clock_step(source, 10000);
    g_assert_true(qtest_readl(source,
                             BCM2711_GPIO_BASE + GPIO_GPLEV1) & BIT(8));
    pwm_ctl = qtest_readl(
        source, BCM2711_CPRMAN_BASE + CPRMAN_PWM_CTL);
    qtest_writel(source, BCM2711_CPRMAN_BASE + CPRMAN_PWM_CTL,
                 CPRMAN_PASSWORD | (pwm_ctl & ~BIT(4)));

    destination = migrate_to_new_qtest(
        source, "-M raspi4b", &migration_dir, &migration_socket);
    qtest_quit(source);
    g_assert_cmpuint(qom_path_get_uint32(
                         destination, PWM1_QOM_PATH,
                         "clock-frequency"), ==, 0);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_PWM1_BASE + PWM_DAT1), ==, samples[0]);
    g_assert_true(qtest_readl(destination,
                             BCM2711_GPIO_BASE + GPIO_GPLEV1) & BIT(8));
    dma_cs = qtest_readl(destination, dma_base + DMA_CS);
    g_assert_true(dma_cs & DMA_CS_ACTIVE);
    g_assert_true(dma_cs & DMA_CS_ISHELD);

    qtest_writel(destination, BCM2711_CPRMAN_BASE + CPRMAN_PWM_CTL,
                 CPRMAN_PASSWORD | pwm_ctl);
    qtest_clock_step(destination, 20000 - 1);
    g_assert_true(qtest_readl(destination,
                             BCM2711_GPIO_BASE + GPIO_GPLEV1) & BIT(8));
    qtest_clock_step(destination, 1);
    g_assert_false(qtest_readl(destination,
                              BCM2711_GPIO_BASE + GPIO_GPLEV1) & BIT(8));
    qtest_clock_step(destination, 70000);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_PWM1_BASE + PWM_DAT1), ==, samples[1]);

    for (size_t i = 2; i < ARRAY_SIZE(samples); i++) {
        qtest_clock_step(destination, 100000);
        g_assert_cmphex(qtest_readl(destination,
                                   BCM2711_PWM1_BASE + PWM_DAT1), ==,
                        samples[i]);
    }
    dma_cs = qtest_readl(destination, dma_base + DMA_CS);
    g_assert_false(dma_cs & DMA_CS_ACTIVE);
    g_assert_false(dma_cs & DMA_CS_ISHELD);
    g_assert_true(dma_cs & DMA_CS_END);
    g_assert_true(dma_cs & DMA_CS_INT);

    qtest_clock_step(destination, 100000);
    g_assert_true(qtest_readl(destination,
                             BCM2711_PWM1_BASE + PWM_STA) & PWM_STA_EMPT1);
    g_assert_true(qtest_readl(destination,
                             BCM2711_PWM1_BASE + PWM_STA) & PWM_STA_GAPO1);

    qtest_system_reset(destination);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_PWM1_BASE + PWM_CTL), ==, 0);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_PWM1_BASE + PWM_DMAC), ==, 0);
    g_assert_false(qtest_readl(destination,
                              BCM2711_GPIO_BASE + GPIO_GPLEV1) & BIT(8));
    qtest_quit(destination);
    unlink(migration_socket);
    rmdir(migration_dir);

    /* CM4 exposes the same BCM2711 PWM1 block and shared CPRMAN clock. */
    cm4 = qtest_init("-M raspi-cm4");
    configure_pwm_clock(cm4);
    g_assert_cmpuint(qom_path_get_uint32(
                         cm4, PWM1_QOM_PATH, "clock-frequency"), ==, 100000);
    qtest_writel(cm4, BCM2711_PWM1_BASE + PWM_DAT2, 0x12345678);
    g_assert_cmphex(qtest_readl(cm4, BCM2711_PWM1_BASE + PWM_DAT2), ==,
                    0x12345678);
    qtest_quit(cm4);
}
void test_bcm2711_pwm_dual_fifo_lockstep_migration(void)
{
    const uint32_t samples[] = {
        1, 2, 3, 4, 5, 6,
    };
    const uint32_t ctl = BIT(0) | PWM_CTL_USEF1 | PWM_CTL_MSEN1 |
                         PWM_CTL_PWEN2 | PWM_CTL_USEF2 | PWM_CTL_MSEN2;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    QTestState *source = qtest_init("-M raspi4b");
    QTestState *destination;
    uint32_t fsel;
    uint32_t pwm_ctl;

    configure_pwm_clock(source);
    fsel = qtest_readl(source, BCM2711_GPIO_BASE + GPIO_GPFSEL1);
    fsel = deposit32(fsel, (12 - 10) * 3, 3, 4);
    fsel = deposit32(fsel, (13 - 10) * 3, 3, 4);
    qtest_writel(source, BCM2711_GPIO_BASE + GPIO_GPFSEL1, fsel);

    qtest_writel(source, BCM2711_PWM_BASE + PWM_RNG1, 5);
    qtest_writel(source, BCM2711_PWM_BASE + PWM_RNG2, 10);
    for (size_t i = 0; i < ARRAY_SIZE(samples); i++) {
        qtest_writel(source, BCM2711_PWM_BASE + PWM_FIF1, samples[i]);
    }
    qtest_writel(source, BCM2711_PWM_BASE + PWM_CTL, ctl);

    /* A/C/E belong to channel 0 and B/D/F to channel 1. */
    g_assert_cmphex(qtest_readl(source,
                               BCM2711_PWM_BASE + PWM_DAT1), ==, samples[0]);
    g_assert_cmphex(qtest_readl(source,
                               BCM2711_PWM_BASE + PWM_DAT2), ==, samples[1]);
    g_assert_true(qtest_readl(source,
                             BCM2711_GPIO_BASE + GPIO_GPLEV0) & BIT(12));
    g_assert_true(qtest_readl(source,
                             BCM2711_GPIO_BASE + GPIO_GPLEV0) & BIT(13));

    /*
     * Channel 0's five-cycle range finishes first.  It must idle for another
     * five cycles instead of consuming C before channel 1 requests D.
     */
    qtest_clock_step(source, 50000);
    g_assert_cmphex(qtest_readl(source,
                               BCM2711_PWM_BASE + PWM_DAT1), ==, samples[0]);
    g_assert_cmphex(qtest_readl(source,
                               BCM2711_PWM_BASE + PWM_DAT2), ==, samples[1]);
    g_assert_false(qtest_readl(source,
                              BCM2711_GPIO_BASE + GPIO_GPLEV0) & BIT(12));
    qtest_clock_step(source, 50000 - 1);
    g_assert_cmphex(qtest_readl(source,
                               BCM2711_PWM_BASE + PWM_DAT1), ==, samples[0]);
    qtest_clock_step(source, 1);
    g_assert_cmphex(qtest_readl(source,
                               BCM2711_PWM_BASE + PWM_DAT1), ==, samples[2]);
    g_assert_cmphex(qtest_readl(source,
                               BCM2711_PWM_BASE + PWM_DAT2), ==, samples[3]);

    /*
     * Migrate the second pair after channel 0 has entered its lock-step wait.
     * PWM1 shares the clock, so stop it before migration and preserve the
     * longer channel's exact five remaining source cycles.
     */
    qtest_clock_step(source, 50000);
    g_assert_cmphex(qtest_readl(source,
                               BCM2711_PWM_BASE + PWM_DAT1), ==, samples[2]);
    g_assert_cmphex(qtest_readl(source,
                               BCM2711_PWM_BASE + PWM_DAT2), ==, samples[3]);
    pwm_ctl = qtest_readl(
        source, BCM2711_CPRMAN_BASE + CPRMAN_PWM_CTL);
    qtest_writel(source, BCM2711_CPRMAN_BASE + CPRMAN_PWM_CTL,
                 CPRMAN_PASSWORD | (pwm_ctl & ~BIT(4)));

    destination = migrate_to_new_qtest(
        source, "-M raspi4b", &migration_dir, &migration_socket);
    qtest_quit(source);
    g_assert_cmpuint(qom_path_get_uint32(
                         destination, PWM_QOM_PATH,
                         "clock-frequency"), ==, 0);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_PWM_BASE + PWM_DAT1), ==, samples[2]);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_PWM_BASE + PWM_DAT2), ==, samples[3]);
    g_assert_false(qtest_readl(destination,
                              BCM2711_GPIO_BASE + GPIO_GPLEV0) & BIT(12));

    qtest_writel(destination, BCM2711_CPRMAN_BASE + CPRMAN_PWM_CTL,
                 CPRMAN_PASSWORD | pwm_ctl);
    qtest_clock_step(destination, 50000 - 1);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_PWM_BASE + PWM_DAT1), ==, samples[2]);
    qtest_clock_step(destination, 1);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_PWM_BASE + PWM_DAT1), ==, samples[4]);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_PWM_BASE + PWM_DAT2), ==, samples[5]);

    /* Both channels report a gap only after the shared FIFO truly empties. */
    qtest_clock_step(destination, 100000);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_PWM_BASE + PWM_STA) &
                    (PWM_STA_GAPO1 | PWM_STA_GAPO2), ==,
                    PWM_STA_GAPO1 | PWM_STA_GAPO2);

    /*
     * Starve with one word at a time.  The next-owner state survives the
     * empty FIFO: G goes to channel 0, H waits for channel 0's boundary and
     * then goes to channel 1.
     */
    qtest_writel(destination, BCM2711_PWM_BASE + PWM_FIF1, 7);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_PWM_BASE + PWM_DAT1), ==, 7);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_PWM_BASE + PWM_DAT2), ==, samples[5]);
    qtest_writel(destination, BCM2711_PWM_BASE + PWM_FIF1, 8);
    qtest_clock_step(destination, 50000 - 1);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_PWM_BASE + PWM_DAT2), ==, samples[5]);
    qtest_clock_step(destination, 1);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_PWM_BASE + PWM_DAT1), ==, 7);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_PWM_BASE + PWM_DAT2), ==, 8);

    qtest_system_reset(destination);
    configure_pwm_clock(destination);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_PWM_BASE + PWM_CTL), ==, 0);

    /* PWM1 is a separate instance of the same paired FIFO state machine. */
    qtest_writel(destination, BCM2711_PWM1_BASE + PWM_RNG1, 4);
    qtest_writel(destination, BCM2711_PWM1_BASE + PWM_RNG2, 4);
    qtest_writel(destination, BCM2711_PWM1_BASE + PWM_FIF1, 0x11);
    qtest_writel(destination, BCM2711_PWM1_BASE + PWM_FIF1, 0x22);
    qtest_writel(destination, BCM2711_PWM1_BASE + PWM_FIF1, 0x33);
    qtest_writel(destination, BCM2711_PWM1_BASE + PWM_FIF1, 0x44);
    qtest_writel(destination, BCM2711_PWM1_BASE + PWM_CTL, ctl);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_PWM1_BASE + PWM_DAT1), ==, 0x11);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_PWM1_BASE + PWM_DAT2), ==, 0x22);
    qtest_clock_step(destination, 40000);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_PWM1_BASE + PWM_DAT1), ==, 0x33);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_PWM1_BASE + PWM_DAT2), ==, 0x44);

    qtest_quit(destination);
    unlink(migration_socket);
    rmdir(migration_dir);
}
void test_bcm2711_pwm_dma_panic_priority_migration(void)
{
    const uint32_t cb0 = 0x00110000;
    const uint32_t cb1 = 0x00110100;
    const uint32_t source0 = 0x00111000;
    const uint32_t source1 = 0x00111100;
    const uint32_t dest = 0x00112000;
    const uint32_t sample0 = 0xaaaaaaaa;
    const uint32_t sample1 = 0xbbbbbbbb;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    QTestState *source = qtest_init("-M raspi4b");
    QTestState *destination;
    uint32_t cs0;
    uint32_t cs1;

    configure_pwm_clock(source);
    qtest_writel(source, BCM2711_PWM_BASE + PWM_RNG1, 10);
    for (unsigned i = 0; i < 5; i++) {
        qtest_writel(source, BCM2711_PWM_BASE + PWM_FIF1, i + 1);
    }
    /*
     * At five words both requests are low.  Starting PWM consumes one word:
     * PANIC (threshold four) rises, while DREQ (threshold two) remains low.
     */
    qtest_writel(source, BCM2711_PWM_BASE + PWM_DMAC,
                 PWM_DMAC_ENABLE | (4 << 8) | 2);
    qtest_writel(source, dest, 0);
    program_dma_paced_word(source, 0, cb0, source0, dest, DMA_PERMAP_PWM,
                           15, 2, sample0);
    program_dma_paced_word(source, 1, cb1, source1, dest, DMA_PERMAP_PWM,
                           1, 14, sample1);
    cs0 = qtest_readl(source, BCM2711_DMA_BASE + DMA_CS);
    cs1 = qtest_readl(source, BCM2711_DMA_BASE + 0x100 + DMA_CS);
    g_assert_true(cs0 & DMA_CS_ACTIVE);
    g_assert_true(cs0 & DMA_CS_ISHELD);
    g_assert_false(cs0 & DMA_CS_DREQ);
    g_assert_true(cs1 & DMA_CS_ACTIVE);
    g_assert_true(cs1 & DMA_CS_ISHELD);
    g_assert_false(cs1 & DMA_CS_DREQ);

    qtest_writel(source, BCM2711_PWM_BASE + PWM_CTL,
                 BIT(0) | PWM_CTL_USEF1);
    g_assert_false(qtest_readl(source,
                              BCM2711_DMA_BASE + DMA_CS) & DMA_CS_DREQ);

    /*
     * Migrate while PWM0 panic is high and both channels are held.  When the
     * FIFO reaches DREQ at the second period, channel 1's panic priority 14
     * wins over channel 0's panic priority 2.  Channel 0 therefore writes
     * last.  Their normal priorities deliberately specify the opposite order.
     */
    destination = migrate_to_new_qtest(
        source, "-M raspi4b", &migration_dir, &migration_socket);
    qtest_quit(source);
    g_assert_cmphex(qtest_readl(destination, dest), ==, 0);
    qtest_clock_step(destination, 100000);
    g_assert_cmphex(qtest_readl(destination, dest), ==, 0);
    g_assert_false(qtest_readl(destination,
                              BCM2711_DMA_BASE + DMA_CS) & DMA_CS_DREQ);
    qtest_clock_step(destination, 100000);
    g_assert_cmphex(qtest_readl(destination, dest), ==, sample0);
    cs0 = qtest_readl(destination, BCM2711_DMA_BASE + DMA_CS);
    cs1 = qtest_readl(destination, BCM2711_DMA_BASE + 0x100 + DMA_CS);
    g_assert_false(cs0 & DMA_CS_ACTIVE);
    g_assert_false(cs0 & DMA_CS_ISHELD);
    g_assert_true(cs0 & DMA_CS_END);
    g_assert_false(cs1 & DMA_CS_ACTIVE);
    g_assert_false(cs1 & DMA_CS_ISHELD);
    g_assert_true(cs1 & DMA_CS_END);

    /*
     * Reset and repeat through PWM1 with PANIC threshold zero.  Its first
     * FIFO pop raises DREQ at level two while panic stays low, selecting
     * normal priority.  Channel 2 wins and channel 3 writes last.
     */
    qtest_system_reset(destination);
    configure_pwm_clock(destination);
    qtest_writel(destination, BCM2711_PWM1_BASE + PWM_RNG1, 10);
    qtest_writel(destination, BCM2711_PWM1_BASE + PWM_FIF1, 1);
    qtest_writel(destination, BCM2711_PWM1_BASE + PWM_FIF1, 2);
    qtest_writel(destination, BCM2711_PWM1_BASE + PWM_FIF1, 3);
    qtest_writel(destination, BCM2711_PWM1_BASE + PWM_DMAC,
                 PWM_DMAC_ENABLE | 2);
    qtest_writel(destination, dest, 0);
    program_dma_paced_word(destination, 2, cb0, source0, dest,
                           DMA_PERMAP_PWM1, 14, 1, sample0);
    program_dma_paced_word(destination, 3, cb1, source1, dest,
                           DMA_PERMAP_PWM1, 2, 15, sample1);
    g_assert_true(qtest_readl(destination,
                             BCM2711_DMA_BASE + 0x200 + DMA_CS) &
                  DMA_CS_ISHELD);
    g_assert_true(qtest_readl(destination,
                             BCM2711_DMA_BASE + 0x300 + DMA_CS) &
                  DMA_CS_ISHELD);
    qtest_writel(destination, BCM2711_PWM1_BASE + PWM_CTL,
                 BIT(0) | PWM_CTL_USEF1);
    g_assert_cmphex(qtest_readl(destination, dest), ==, sample1);

    qtest_system_reset(destination);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DMA_BASE + DMA_CS), ==, 0);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_PWM_BASE + PWM_DMAC), ==, 0);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_PWM1_BASE + PWM_DMAC), ==, 0);
    qtest_quit(destination);
    unlink(migration_socket);
    rmdir(migration_dir);
}
void test_bcm2711_spi_dma_migration(void)
{
    const uint32_t spi_divider = 250;
    const uint32_t tx_cb = 0x00120000;
    const uint32_t tx_source = 0x00121000;
    const uint32_t rx_cb = 0x00122000;
    const uint32_t rx_destination = 0x00123000;
    const uint32_t transfer_size = 8;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    QTestState *source = qtest_init("-M raspi4b");
    QTestState *destination;
    uint32_t cs;
    uint32_t ctl;
    uint32_t dma_cs;
    uint32_t core_hz;
    uint64_t byte_delay;
    uint64_t half_delay;
    uint64_t resume_delay;
    uint64_t serial_hz;

    qtest_irq_intercept_out_named(source, SPI_QOM_PATH, "chip-select");

    /* Active-low CE1 follows TA and its per-line polarity control. */
    qtest_writel(source, BCM2711_SPI_BASE + SPI_CS, SPI_CS_TA | 1);
    g_assert_true(qtest_get_irq(source, 0));
    g_assert_false(qtest_get_irq(source, 1));
    g_assert_true(qtest_get_irq(source, 2));
    qtest_writel(source, BCM2711_SPI_BASE + SPI_CS,
                 SPI_CS_TA | SPI_CS_CSPOL1 | 1);
    g_assert_true(qtest_get_irq(source, 1));
    qtest_writel(source, BCM2711_SPI_BASE + SPI_CS, 0);

    /*
     * CLK paces each byte at eight SCLK cycles.  Stopping VPU halfway through
     * a byte freezes the deadline, and restarting it preserves the remaining
     * serial-clock cycles.
     */
    core_hz = qom_path_get_uint32(
        source, SPI_QOM_PATH, "core-clock-frequency");
    serial_hz = core_hz / spi_divider;
    byte_delay = DIV_ROUND_UP(
        8ULL * NANOSECONDS_PER_SECOND, serial_hz);
    half_delay = byte_delay / 2;
    resume_delay = DIV_ROUND_UP(
        DIV_ROUND_UP((byte_delay - half_delay) * serial_hz,
                     NANOSECONDS_PER_SECOND) * NANOSECONDS_PER_SECOND,
        serial_hz);
    ctl = qtest_readl(source, BCM2711_CPRMAN_BASE + CPRMAN_VPU_CTL);
    qtest_writel(source, BCM2711_SPI_BASE + SPI_CLK, spi_divider);
    qtest_writel(source, BCM2711_SPI_BASE + SPI_CS, SPI_CS_TA | 1);
    qtest_writel(source, BCM2711_SPI_BASE + SPI_FIFO, 0xa5);
    qtest_clock_step(source, half_delay);
    g_assert_false(qtest_readl(source,
                              BCM2711_SPI_BASE + SPI_CS) & SPI_CS_RXD);
    qtest_writel(source, BCM2711_CPRMAN_BASE + CPRMAN_VPU_CTL,
                 CPRMAN_PASSWORD | (ctl & ~BIT(4)));
    qtest_clock_step(source, 10 * NANOSECONDS_PER_SECOND);
    g_assert_false(qtest_readl(source,
                              BCM2711_SPI_BASE + SPI_CS) & SPI_CS_RXD);
    qtest_writel(source, BCM2711_CPRMAN_BASE + CPRMAN_VPU_CTL,
                 CPRMAN_PASSWORD | ctl);
    qtest_clock_step(source, resume_delay - 2);
    g_assert_false(qtest_readl(source,
                              BCM2711_SPI_BASE + SPI_CS) & SPI_CS_RXD);
    qtest_clock_step(source, 4);
    g_assert_true(qtest_readl(source,
                             BCM2711_SPI_BASE + SPI_CS) & SPI_CS_RXD);
    g_assert_cmphex(qtest_readl(source,
                               BCM2711_SPI_BASE + SPI_FIFO), ==, 0);
    qtest_writel(source, BCM2711_SPI_BASE + SPI_CS, 0);

    /*
     * TX DMA map 6 first supplies the documented DLEN/CS framing word and
     * then two little-endian data words.  With no attached peripheral the
     * eight received bytes are deterministic zeroes retained in RX FIFO.
     */
    qtest_writel(source, BCM2711_SPI_BASE + SPI_DC,
                 (1U << 24) | (1U << 8));
    qtest_writel(source, BCM2711_SPI_BASE + SPI_CS,
                 SPI_CS_DMAEN | SPI_CS_ADCS | SPI_CS_INTD);
    qtest_writel(source, tx_source,
                 (transfer_size << 16) | SPI_CS_TA);
    qtest_writel(source, tx_source + 4, 0x44332211);
    qtest_writel(source, tx_source + 8, 0x88776655);
    qtest_writel(source, tx_cb,
                 DMA_TI_INT_EN | DMA_TI_D_DREQ | DMA_TI_S_INC |
                 (DMA_PERMAP_SPI_TX << 16));
    qtest_writel(source, tx_cb + 4, tx_source);
    qtest_writel(source, tx_cb + 8, DMA_SPI_FIFO_BUS);
    qtest_writel(source, tx_cb + 12, 3 * sizeof(uint32_t));
    qtest_writel(source, tx_cb + 16, 0);
    qtest_writel(source, tx_cb + 20, 0);
    qtest_writel(source, BCM2711_DMA_BASE + DMA_CB_ADDR, tx_cb);
    qtest_writel(source, BCM2711_DMA_BASE + DMA_CS, DMA_CS_ACTIVE);

    dma_cs = qtest_readl(source, BCM2711_DMA_BASE + DMA_CS);
    g_assert_true(dma_cs & DMA_CS_ACTIVE);
    g_assert_true(dma_cs & DMA_CS_ISHELD);
    g_assert_false(dma_cs & DMA_CS_END);
    cs = qtest_readl(source, BCM2711_SPI_BASE + SPI_CS);
    g_assert_true(cs & SPI_CS_DMAEN);
    g_assert_true(cs & SPI_CS_TA);
    g_assert_false(cs & SPI_CS_DONE);
    g_assert_false(cs & SPI_CS_RXD);
    g_assert_true(cs & SPI_CS_TXD);
    g_assert_cmpuint(qtest_readl(source,
                                BCM2711_SPI_BASE + SPI_DLEN), ==,
                     transfer_size);
    g_assert_false(qtest_get_irq(source, 0));

    /* Migrate halfway through the first byte with the TX DMA channel held. */
    qtest_clock_step(source, half_delay);
    destination = migrate_to_new_qtest(
        source, "-M raspi4b", &migration_dir, &migration_socket);
    qtest_quit(source);
    cs = qtest_readl(destination, BCM2711_SPI_BASE + SPI_CS);
    g_assert_false(cs & SPI_CS_DONE);
    g_assert_false(cs & SPI_CS_RXD);
    qtest_clock_step_next(destination);
    g_assert_true(qtest_readl(destination,
                             BCM2711_SPI_BASE + SPI_CS) & SPI_CS_RXD);
    for (unsigned int index = 1; index < transfer_size; index++) {
        qtest_clock_step(destination, byte_delay);
    }

    dma_cs = qtest_readl(destination, BCM2711_DMA_BASE + DMA_CS);
    g_assert_false(dma_cs & DMA_CS_ACTIVE);
    g_assert_true(dma_cs & DMA_CS_END);
    g_assert_true(dma_cs & DMA_CS_INT);
    cs = qtest_readl(destination, BCM2711_SPI_BASE + SPI_CS);
    g_assert_true(cs & SPI_CS_DONE);
    g_assert_true(cs & SPI_CS_RXD);

    /*
     * RX DMA map 7 must be restored high after migration.  Two fixed-source
     * FIFO reads drain all eight bytes into adjacent guest words.
     */
    qtest_writel(destination, rx_destination, 0xaaaaaaaa);
    qtest_writel(destination, rx_destination + 4, 0xbbbbbbbb);
    qtest_writel(destination, rx_cb,
                 DMA_TI_INT_EN | DMA_TI_S_DREQ | DMA_TI_D_INC |
                 (DMA_PERMAP_SPI_RX << 16));
    qtest_writel(destination, rx_cb + 4, DMA_SPI_FIFO_BUS);
    qtest_writel(destination, rx_cb + 8, rx_destination);
    qtest_writel(destination, rx_cb + 12, transfer_size);
    qtest_writel(destination, rx_cb + 16, 0);
    qtest_writel(destination, rx_cb + 20, 0);
    qtest_writel(destination, BCM2711_DMA_BASE + 0x100 + DMA_CB_ADDR,
                 rx_cb);
    qtest_writel(destination, BCM2711_DMA_BASE + 0x100 + DMA_CS,
                 DMA_CS_ACTIVE);

    dma_cs = qtest_readl(destination,
                        BCM2711_DMA_BASE + 0x100 + DMA_CS);
    g_assert_false(dma_cs & DMA_CS_ACTIVE);
    g_assert_true(dma_cs & DMA_CS_END);
    g_assert_true(dma_cs & DMA_CS_INT);
    g_assert_cmphex(qtest_readl(destination, rx_destination), ==, 0);
    g_assert_cmphex(qtest_readl(destination,
                               rx_destination + 4), ==, 0);
    cs = qtest_readl(destination, BCM2711_SPI_BASE + SPI_CS);
    g_assert_false(cs & SPI_CS_RXD);
    g_assert_true(cs & SPI_CS_DONE);

    qtest_writel(destination, BCM2711_SPI_BASE + SPI_CS, 0);
    g_assert_false(qtest_readl(destination,
                              BCM2711_SPI_BASE + SPI_CS) & SPI_CS_DONE);
    qtest_system_reset(destination);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_SPI_BASE + SPI_CS), ==,
                    SPI_CS_TXD | BIT(12));
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_SPI_BASE + SPI_DLEN), ==, 0);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_SPI_BASE + SPI_DC), ==,
                    0x30201020);
    qtest_quit(destination);
    unlink(migration_socket);
    rmdir(migration_dir);
}
void test_bcm2711_aux_uart_registers(void)
{
    QTestState *qts = qtest_init("-M raspi4b");

    g_assert_cmphex(qtest_readl(qts, BCM2711_AUX_BASE + AUX_ENABLES), ==, 1);
    g_assert_cmphex(qtest_readl(qts, BCM2711_AUX_BASE + AUX_IRQ), ==, 0);
    g_assert_cmphex(qtest_readl(qts, BCM2711_AUX_BASE + AUX_MU_LCR), ==, 0);
    g_assert_cmphex(qtest_readl(qts, BCM2711_AUX_BASE + AUX_MU_MCR), ==, 0);
    g_assert_cmphex(qtest_readl(qts, BCM2711_AUX_BASE + AUX_MU_LSR), ==,
                    0x60);
    g_assert_cmphex(qtest_readl(qts, BCM2711_AUX_BASE + AUX_MU_MSR), ==,
                    0x10);
    g_assert_cmphex(qtest_readl(qts, BCM2711_AUX_BASE + AUX_MU_CNTL), ==, 3);
    g_assert_cmphex(qtest_readl(qts, BCM2711_AUX_BASE + AUX_MU_STAT), ==,
                    0x30e);

    qtest_writel(qts, BCM2711_AUX_BASE + AUX_MU_SCRATCH, 0xa5);
    g_assert_cmphex(qtest_readl(qts, BCM2711_AUX_BASE + AUX_MU_SCRATCH), ==,
                    0xa5);
    qtest_writel(qts, BCM2711_AUX_BASE + AUX_MU_MCR, 0xff);
    g_assert_cmphex(qtest_readl(qts, BCM2711_AUX_BASE + AUX_MU_MCR), ==, 2);
    qtest_writel(qts, BCM2711_AUX_BASE + AUX_MU_CNTL, 0xa5);
    g_assert_cmphex(qtest_readl(qts, BCM2711_AUX_BASE + AUX_MU_CNTL), ==,
                    0xa5);

    /* 8250 DLAB accesses and the native baud register share one divider. */
    qtest_writel(qts, BCM2711_AUX_BASE + AUX_MU_LCR, 0x83);
    g_assert_cmphex(qtest_readl(qts, BCM2711_AUX_BASE + AUX_MU_LCR), ==,
                    0x83);
    qtest_writel(qts, BCM2711_AUX_BASE + AUX_MU_IO, 0x34);
    qtest_writel(qts, BCM2711_AUX_BASE + AUX_MU_IER, 0x12);
    g_assert_cmphex(qtest_readl(qts, BCM2711_AUX_BASE + AUX_MU_BAUD), ==,
                    0x1234);
    g_assert_cmphex(qtest_readl(qts, BCM2711_AUX_BASE + AUX_MU_IO), ==,
                    0x34);
    g_assert_cmphex(qtest_readl(qts, BCM2711_AUX_BASE + AUX_MU_IER), ==,
                    0x12);
    qtest_writel(qts, BCM2711_AUX_BASE + AUX_MU_BAUD, 0x5678);
    g_assert_cmphex(qtest_readl(qts, BCM2711_AUX_BASE + AUX_MU_BAUD), ==,
                    0x5678);

    qtest_writel(qts, BCM2711_AUX_BASE + AUX_MU_LCR, 3);
    qtest_writel(qts, BCM2711_AUX_BASE + AUX_MU_CNTL, 3);
    qtest_writel(qts, BCM2711_AUX_BASE + AUX_MU_IER, 3);
    g_assert_cmphex(qtest_readl(qts, BCM2711_AUX_BASE + AUX_MU_IER), ==,
                    0xc3);
    g_assert_cmphex(qtest_readl(qts, BCM2711_AUX_BASE + AUX_MU_IIR) & 1,
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, BCM2711_AUX_BASE + AUX_IRQ), ==, 1);
    for (unsigned int i = 0; i < 9; i++) {
        qtest_writel(qts, BCM2711_AUX_BASE + AUX_MU_IO, i);
    }
    g_assert_cmpuint(
        qtest_readl(qts, BCM2711_AUX_BASE + AUX_MU_STAT) >>
        AUX_MU_STAT_TX_LEVEL_SHIFT & 0xf, ==, 8);
    g_assert_true(qtest_readl(
                      qts, BCM2711_AUX_BASE + AUX_MU_STAT) & 0x20);
    qtest_writel(qts, BCM2711_AUX_BASE + AUX_MU_IIR, 0x4);
    g_assert_cmphex(qtest_readl(qts, BCM2711_AUX_BASE + AUX_MU_STAT), ==,
                    0x30e);
    qtest_writel(qts, BCM2711_AUX_BASE + AUX_ENABLES, 0);
    g_assert_cmphex(qtest_readl(qts, BCM2711_AUX_BASE + AUX_IRQ), ==, 0);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, BCM2711_AUX_BASE + AUX_ENABLES), ==, 1);
    g_assert_cmphex(qtest_readl(qts, BCM2711_AUX_BASE + AUX_MU_LCR), ==, 0);
    g_assert_cmphex(qtest_readl(qts, BCM2711_AUX_BASE + AUX_MU_SCRATCH), ==,
                    0);
    g_assert_cmphex(qtest_readl(qts, BCM2711_AUX_BASE + AUX_MU_CNTL), ==, 3);
    g_assert_cmphex(qtest_readl(qts, BCM2711_AUX_BASE + AUX_MU_BAUD), ==, 0);
    qtest_quit(qts);
}
void test_bcm2711_aux_uart_core_clock(void)
{
    const uint32_t baud_divider = 249;
    const uint64_t frame_cycles = 10 * 8 * (baud_divider + 1);
    int sockets[2];
    GPollFD pollfd;
    QTestState *qts;
    uint32_t ctl;
    uint32_t div;
    uint32_t fast_hz;
    uint32_t slow_hz;
    uint64_t fast_delay;
    uint64_t slow_delay;
    uint8_t received;

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    qts = qtest_initf(
        "-M raspi4b -serial null "
        "-chardev socket,id=auxclk,fd=%d -serial chardev:auxclk",
        sockets[1]);
    close(sockets[1]);
    pollfd.fd = sockets[0];
    pollfd.events = G_IO_IN;

    fast_hz = qom_path_get_uint32(
        qts, AUX_QOM_PATH, "core-clock-frequency");
    g_assert_cmpuint(fast_hz, >, 249000000);
    g_assert_cmpuint(fast_hz, <, 251000000);
    fast_delay = DIV_ROUND_UP(
        frame_cycles * NANOSECONDS_PER_SECOND, fast_hz);
    ctl = qtest_readl(qts, BCM2711_CPRMAN_BASE + CPRMAN_VPU_CTL);
    div = qtest_readl(qts, BCM2711_CPRMAN_BASE + CPRMAN_VPU_DIV);
    g_assert_true(ctl & BIT(4));

    qtest_writel(qts, BCM2711_AUX_BASE + AUX_MU_BAUD, baud_divider);
    qtest_writel(qts, BCM2711_AUX_BASE + AUX_MU_IO, 0xa1);
    qtest_clock_step(qts, fast_delay - 2);
    g_assert_cmpint(g_poll(&pollfd, 1, 0), ==, 0);
    qtest_clock_step(qts, 4);
    g_assert_cmpint(g_poll(&pollfd, 1, 1000), ==, 1);
    g_assert_cmpint(read(sockets[0], &received, 1), ==, 1);
    g_assert_cmphex(received, ==, 0xa1);

    /* Doubling the VPU divider halves the UART source frequency. */
    qtest_writel(qts, BCM2711_CPRMAN_BASE + CPRMAN_VPU_DIV,
                 CPRMAN_PASSWORD | (div * 2));
    slow_hz = qom_path_get_uint32(
        qts, AUX_QOM_PATH, "core-clock-frequency");
    g_assert_cmpuint(ABS((int64_t)fast_hz - 2 * (int64_t)slow_hz), <=, 2);
    slow_delay = DIV_ROUND_UP(
        frame_cycles * NANOSECONDS_PER_SECOND, slow_hz);
    qtest_writel(qts, BCM2711_AUX_BASE + AUX_MU_IO, 0xb2);
    qtest_clock_step(qts, slow_delay - 2);
    g_assert_cmpint(g_poll(&pollfd, 1, 0), ==, 0);
    qtest_clock_step(qts, 4);
    g_assert_cmpint(g_poll(&pollfd, 1, 1000), ==, 1);
    g_assert_cmpint(read(sockets[0], &received, 1), ==, 1);
    g_assert_cmphex(received, ==, 0xb2);

    /*
     * A source-rate change in the middle of a frame preserves its remaining
     * source cycles instead of restarting or completing it immediately.
     */
    qtest_writel(qts, BCM2711_AUX_BASE + AUX_MU_IO, 0xc3);
    qtest_clock_step(qts, slow_delay / 2);
    qtest_writel(qts, BCM2711_CPRMAN_BASE + CPRMAN_VPU_DIV,
                 CPRMAN_PASSWORD | div);
    qtest_clock_step(qts, fast_delay / 2 - 2);
    g_assert_cmpint(g_poll(&pollfd, 1, 0), ==, 0);
    qtest_clock_step(qts, 4);
    g_assert_cmpint(g_poll(&pollfd, 1, 1000), ==, 1);
    g_assert_cmpint(read(sockets[0], &received, 1), ==, 1);
    g_assert_cmphex(received, ==, 0xc3);

    /* A stopped VPU clock freezes TX and reset restores the fixed clock. */
    qtest_writel(qts, BCM2711_CPRMAN_BASE + CPRMAN_VPU_CTL,
                 CPRMAN_PASSWORD | (ctl & ~BIT(4)));
    g_assert_cmpuint(qom_path_get_uint32(
                         qts, AUX_QOM_PATH, "core-clock-frequency"), ==, 0);
    qtest_writel(qts, BCM2711_AUX_BASE + AUX_MU_IO, 0xd4);
    qtest_clock_step(qts, 10 * NANOSECONDS_PER_SECOND);
    g_assert_cmpint(g_poll(&pollfd, 1, 0), ==, 0);
    qtest_writel(qts, BCM2711_CPRMAN_BASE + CPRMAN_VPU_CTL,
                 CPRMAN_PASSWORD | ctl);
    qtest_clock_step(qts, fast_delay + 2);
    g_assert_cmpint(g_poll(&pollfd, 1, 1000), ==, 1);
    g_assert_cmpint(read(sockets[0], &received, 1), ==, 1);
    g_assert_cmphex(received, ==, 0xd4);

    qtest_system_reset(qts);
    fast_hz = qom_path_get_uint32(
        qts, AUX_QOM_PATH, "core-clock-frequency");
    g_assert_cmpuint(fast_hz, >, 249000000);
    g_assert_cmpuint(fast_hz, <, 251000000);
    qtest_quit(qts);
    close(sockets[0]);
}
void test_bcm2711_aux_uart_migration(void)
{
    static const uint8_t rx_data[] = { 0x11, 0x22, 0x33, 0x44 };
    static const uint8_t tx_data[] = { 0xa1, 0xb2, 0xc3 };
    g_autofree char *destination_command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    int source_sockets[2];
    int destination_sockets[2];
    QTestState *source;
    QTestState *destination;
    GPollFD pollfd;
    uint32_t stat = 0;
    uint32_t vpu_ctl;
    uint8_t received;

    g_assert_cmpint(
        socketpair(AF_UNIX, SOCK_STREAM, 0, source_sockets), ==, 0);
    g_assert_cmpint(
        socketpair(AF_UNIX, SOCK_STREAM, 0, destination_sockets), ==, 0);
    source = qtest_initf(
        "-M raspi4b -serial null "
        "-chardev socket,id=auxsrc,fd=%d -serial chardev:auxsrc",
        source_sockets[1]);
    close(source_sockets[1]);
    qtest_writel(source, BCM2711_AUX_BASE + AUX_MU_LCR, 3);
    qtest_writel(source, BCM2711_AUX_BASE + AUX_MU_CNTL, 3);
    qtest_writel(source, BCM2711_AUX_BASE + AUX_MU_BAUD, 0xffff);
    qtest_writel(source, BCM2711_AUX_BASE + AUX_MU_SCRATCH, 0x5a);
    qtest_writel(source, BCM2711_AUX_BASE + AUX_MU_MCR, 2);
    qtest_writel(source, BCM2711_AUX_BASE + AUX_MU_IER, 3);

    g_assert_cmpint(
        write(source_sockets[0], rx_data, sizeof(rx_data)), ==,
        sizeof(rx_data));
    for (unsigned int attempt = 0; attempt < 1000; attempt++) {
        stat = qtest_readl(source, BCM2711_AUX_BASE + AUX_MU_STAT);
        if ((stat >> AUX_MU_STAT_RX_LEVEL_SHIFT & 0xf) ==
                sizeof(rx_data)) {
            break;
        }
        g_usleep(1000);
    }
    g_assert_cmpuint(
        stat >> AUX_MU_STAT_RX_LEVEL_SHIFT & 0xf, ==, sizeof(rx_data));
    for (unsigned int i = 0; i < sizeof(tx_data); i++) {
        qtest_writel(source, BCM2711_AUX_BASE + AUX_MU_IO, tx_data[i]);
    }
    vpu_ctl = qtest_readl(
        source, BCM2711_CPRMAN_BASE + CPRMAN_VPU_CTL);
    qtest_clock_step(source, 1000 * 1000);
    qtest_writel(source, BCM2711_CPRMAN_BASE + CPRMAN_VPU_CTL,
                 CPRMAN_PASSWORD | (vpu_ctl & ~BIT(4)));
    g_assert_cmpuint(qom_path_get_uint32(
                         source, AUX_QOM_PATH, "core-clock-frequency"), ==, 0);
    stat = qtest_readl(source, BCM2711_AUX_BASE + AUX_MU_STAT);
    g_assert_cmpuint(
        stat >> AUX_MU_STAT_TX_LEVEL_SHIFT & 0xf, ==, sizeof(tx_data));
    g_assert_cmphex(qtest_readl(
                        source, BCM2711_AUX_BASE + AUX_MU_LSR) & 0x60,
                    ==, 0);

    destination_command = g_strdup_printf(
        "-M raspi4b -serial null "
        "-chardev socket,id=auxdst,fd=%d -serial chardev:auxdst",
        destination_sockets[1]);
    destination = migrate_to_new_qtest(
        source, destination_command,
        &migration_dir, &migration_socket);
    qtest_quit(source);
    close(source_sockets[0]);
    close(destination_sockets[1]);

    g_assert_cmphex(qtest_readl(
                        destination,
                        BCM2711_AUX_BASE + AUX_MU_SCRATCH), ==, 0x5a);
    g_assert_cmphex(qtest_readl(
                        destination,
                        BCM2711_AUX_BASE + AUX_MU_MCR), ==, 2);
    g_assert_cmphex(qtest_readl(
                        destination,
                        BCM2711_AUX_BASE + AUX_MU_BAUD), ==, 0xffff);
    g_assert_cmpuint(qom_path_get_uint32(
                         destination, AUX_QOM_PATH,
                         "core-clock-frequency"), ==, 0);
    stat = qtest_readl(destination, BCM2711_AUX_BASE + AUX_MU_STAT);
    g_assert_cmpuint(
        stat >> AUX_MU_STAT_RX_LEVEL_SHIFT & 0xf, ==, sizeof(rx_data));
    g_assert_cmpuint(
        stat >> AUX_MU_STAT_TX_LEVEL_SHIFT & 0xf, ==, sizeof(tx_data));

    pollfd.fd = destination_sockets[0];
    pollfd.events = G_IO_IN;
    qtest_writel(destination, BCM2711_CPRMAN_BASE + CPRMAN_VPU_CTL,
                 CPRMAN_PASSWORD | vpu_ctl);
    for (unsigned int i = 0; i < sizeof(tx_data); i++) {
        pollfd.revents = 0;
        qtest_clock_step(destination, 21 * 1000 * 1000);
        g_assert_cmpint(g_poll(&pollfd, 1, 1000), ==, 1);
        g_assert_cmpint(
            read(destination_sockets[0], &received, 1), ==, 1);
        g_assert_cmphex(received, ==, tx_data[i]);
    }
    g_assert_cmphex(qtest_readl(
                        destination,
                        BCM2711_AUX_BASE + AUX_MU_LSR) & 0x60,
                    ==, 0x60);
    for (unsigned int i = 0; i < sizeof(rx_data); i++) {
        g_assert_cmphex(qtest_readl(
                            destination,
                            BCM2711_AUX_BASE + AUX_MU_IO) & 0xff,
                        ==, rx_data[i]);
    }
    g_assert_cmphex(qtest_readl(
                        destination, BCM2711_AUX_BASE + AUX_MU_STAT),
                    ==, 0x30e);
    g_assert_cmphex(qtest_readl(
                        destination, BCM2711_AUX_BASE + AUX_IRQ),
                    ==, 1);

    qtest_quit(destination);
    close(destination_sockets[0]);
    unlink(migration_socket);
    rmdir(migration_dir);
}
void test_bcm2711_aux_spi_migration(void)
{
    static const char command[] =
        "-M raspi4b -device w25q80bl,bus=aux-spi1,cs=0";
    const uint32_t speed = 249;
    const uint32_t cntl0 =
        (speed << 20) | (6 << 17) | AUX_SPI_CNTL0_VAR_WIDTH |
        AUX_SPI_CNTL0_ENABLE | AUX_SPI_CNTL0_MSBF_OUT;
    const uint32_t cntl1 = AUX_SPI_CNTL1_TXEMPTY_IRQ |
                           AUX_SPI_CNTL1_IDLE_IRQ |
                           AUX_SPI_CNTL1_MSBF_IN;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    QTestState *source = qtest_init(command);
    QTestState *destination;
    uint32_t core_hz;
    uint32_t ctl;
    uint32_t stat;
    uint64_t delay_8;
    uint64_t delay_24;

    core_hz = qom_path_get_uint32(
        source, AUX_QOM_PATH, "core-clock-frequency");
    delay_8 = DIV_ROUND_UP(
        (8 + 2) * 2ULL * (speed + 1) * NANOSECONDS_PER_SECOND,
        core_hz);
    delay_24 = DIV_ROUND_UP(
        (24 + 2) * 2ULL * (speed + 1) * NANOSECONDS_PER_SECOND,
        core_hz);

    /* Disabled auxiliary modules reject all register accesses. */
    g_assert_cmphex(qtest_readl(
                        source, BCM2711_AUX_BASE + AUX_SPI1_BASE +
                                AUX_SPI_CNTL0), ==, 0);
    qtest_writel(source, BCM2711_AUX_BASE + AUX_SPI1_BASE +
                         AUX_SPI_CNTL0, cntl0);
    g_assert_cmphex(qtest_readl(
                        source, BCM2711_AUX_BASE + AUX_SPI1_BASE +
                                AUX_SPI_CNTL0), ==, 0);

    qtest_writel(source, BCM2711_AUX_BASE + AUX_ENABLES, 7);
    qtest_writel(source, BCM2711_AUX_BASE + AUX_SPI1_BASE +
                         AUX_SPI_CNTL1, cntl1);
    qtest_writel(source, BCM2711_AUX_BASE + AUX_SPI1_BASE +
                         AUX_SPI_CNTL0, cntl0);

    /*
     * Keep CS0 asserted across a 24-bit TXHOLD entry and the final 8-bit IO
     * entry.  The attached flash returns its EF 40 14 JEDEC identity.
     */
    qtest_writel(source, BCM2711_AUX_BASE + AUX_SPI1_BASE +
                         AUX_SPI_TXHOLD,
                 (24U << 24) | (0x9fU << 16));
    qtest_writel(source, BCM2711_AUX_BASE + AUX_SPI1_BASE +
                         AUX_SPI_IO, 8U << 24);
    stat = qtest_readl(source, BCM2711_AUX_BASE + AUX_SPI1_BASE +
                              AUX_SPI_STAT);
    g_assert_true(stat & AUX_SPI_STAT_BUSY);
    g_assert_true(stat & AUX_SPI_STAT_RX_EMPTY);
    qtest_clock_step(source, delay_24 - 2);
    g_assert_true(qtest_readl(
                      source, BCM2711_AUX_BASE + AUX_SPI1_BASE +
                              AUX_SPI_STAT) & AUX_SPI_STAT_RX_EMPTY);
    qtest_clock_step(source, 4);
    g_assert_cmphex(qtest_readl(
                        source, BCM2711_AUX_BASE + AUX_SPI1_BASE +
                                AUX_SPI_PEEK), ==, 0x00ef40);
    g_assert_cmphex(qtest_readl(
                        source, BCM2711_AUX_BASE + AUX_SPI1_BASE +
                                AUX_SPI_IO), ==, 0x00ef40);
    g_assert_cmphex(qtest_readl(source, BCM2711_AUX_BASE + AUX_IRQ) &
                    BIT(1), ==, BIT(1));
    qtest_clock_step(source, delay_8 + 2);
    g_assert_cmphex(qtest_readl(
                        source, BCM2711_AUX_BASE + AUX_SPI1_BASE +
                                AUX_SPI_IO), ==, 0x14);
    stat = qtest_readl(source, BCM2711_AUX_BASE + AUX_SPI1_BASE +
                              AUX_SPI_STAT);
    g_assert_false(stat & AUX_SPI_STAT_BUSY);
    g_assert_true(stat & AUX_SPI_STAT_TX_EMPTY);
    qtest_writel(source, BCM2711_AUX_BASE + AUX_SPI1_BASE +
                         AUX_SPI_CNTL1, AUX_SPI_CNTL1_MSBF_IN);
    g_assert_cmphex(qtest_readl(source, BCM2711_AUX_BASE + AUX_IRQ) &
                    BIT(1), ==, 0);

    /*
     * SPI2 runs independently.  Stop VPU halfway through an eight-bit entry,
     * migrate the active transfer with its remaining core cycles, and resume.
     */
    qtest_writel(source, BCM2711_AUX_BASE + AUX_SPI2_BASE +
                         AUX_SPI_CNTL1, AUX_SPI_CNTL1_IDLE_IRQ |
                                         AUX_SPI_CNTL1_MSBF_IN);
    qtest_writel(source, BCM2711_AUX_BASE + AUX_SPI2_BASE +
                         AUX_SPI_CNTL0, cntl0);
    qtest_writel(source, BCM2711_AUX_BASE + AUX_SPI2_BASE +
                         AUX_SPI_IO, (8U << 24) | (0xa5U << 16));
    qtest_clock_step(source, delay_8 / 2);
    ctl = qtest_readl(source, BCM2711_CPRMAN_BASE + CPRMAN_VPU_CTL);
    qtest_writel(source, BCM2711_CPRMAN_BASE + CPRMAN_VPU_CTL,
                 CPRMAN_PASSWORD | (ctl & ~BIT(4)));
    qtest_clock_step(source, 10 * NANOSECONDS_PER_SECOND);
    g_assert_true(qtest_readl(
                      source, BCM2711_AUX_BASE + AUX_SPI2_BASE +
                              AUX_SPI_STAT) & AUX_SPI_STAT_RX_EMPTY);

    destination = migrate_to_new_qtest(
        source, command, &migration_dir, &migration_socket);
    qtest_quit(source);
    g_assert_cmpuint(qom_path_get_uint32(
                         destination, AUX_QOM_PATH,
                         "core-clock-frequency"), ==, 0);
    stat = qtest_readl(destination, BCM2711_AUX_BASE + AUX_SPI2_BASE +
                                   AUX_SPI_STAT);
    g_assert_true(stat & AUX_SPI_STAT_BUSY);
    g_assert_true(stat & AUX_SPI_STAT_RX_EMPTY);
    qtest_writel(destination, BCM2711_CPRMAN_BASE + CPRMAN_VPU_CTL,
                 CPRMAN_PASSWORD | ctl);
    qtest_clock_step_next(destination);
    g_assert_cmphex(qtest_readl(
                        destination, BCM2711_AUX_BASE + AUX_SPI2_BASE +
                                     AUX_SPI_IO), ==, 0);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_AUX_BASE + AUX_IRQ) & BIT(2),
                    ==, BIT(2));

    qtest_writel(destination, BCM2711_AUX_BASE + AUX_SPI2_BASE +
                              AUX_SPI_CNTL0,
                 AUX_SPI_CNTL0_CLEARFIFO);
    g_assert_true(qtest_readl(
                      destination, BCM2711_AUX_BASE + AUX_SPI2_BASE +
                                   AUX_SPI_STAT) &
                  AUX_SPI_STAT_RX_EMPTY);
    qtest_system_reset(destination);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_AUX_BASE + AUX_ENABLES), ==, 1);
    g_assert_cmphex(qtest_readl(
                        destination, BCM2711_AUX_BASE + AUX_SPI1_BASE +
                                     AUX_SPI_CNTL0), ==, 0);
    qtest_quit(destination);
    unlink(migration_socket);
    rmdir(migration_dir);
}
#ifndef _WIN32
#endif
#ifndef _WIN32
#endif
#ifndef _WIN32
#endif
#ifndef _WIN32
#endif
#ifndef _WIN32
#endif
#ifndef _WIN32
#endif
void test_bcm2711_gpio_external_inputs(void)
{
    static const unsigned int pins[] = { 0, 31, 32, 53, 54, 57 };
    const uint32_t low_mask = BIT(0) | BIT(31);
    const uint32_t high_mask = BIT(0) | BIT(21) | BIT(22) | BIT(25);
    QTestState *qts = qtest_init("-M raspi4b");
    uint32_t value;
    size_t i;

    qtest_irq_intercept_out_named(qts, GPIO_QOM_PATH,
                                  "pin-output-enable");
    for (i = 0; i < ARRAY_SIZE(pins); i++) {
        qtest_set_irq_in(qts, GPIO_QOM_PATH, "pin-input", pins[i], 0);
    }
    g_assert_cmphex(qtest_readl(qts, BCM2711_GPIO_BASE + GPIO_GPLEV0) &
                    low_mask, ==, 0);
    g_assert_cmphex(qtest_readl(qts, BCM2711_GPIO_BASE + GPIO_GPLEV1) &
                    high_mask, ==, 0);

    for (i = 0; i < ARRAY_SIZE(pins); i++) {
        qtest_set_irq_in(qts, GPIO_QOM_PATH, "pin-input", pins[i], 1);
    }
    g_assert_cmphex(qtest_readl(qts, BCM2711_GPIO_BASE + GPIO_GPLEV0) &
                    low_mask, ==, low_mask);
    g_assert_cmphex(qtest_readl(qts, BCM2711_GPIO_BASE + GPIO_GPLEV1) &
                    high_mask, ==, high_mask);

    /* The external input wins while GPIO5 is an input. */
    qtest_set_irq_in(qts, GPIO_QOM_PATH, "pin-input", 5, 1);
    qtest_writel(qts, BCM2711_GPIO_BASE + GPIO_GPCLR0, BIT(5));
    g_assert_cmphex(qtest_readl(qts, BCM2711_GPIO_BASE + GPIO_GPLEV0) &
                    BIT(5), ==, BIT(5));

    /* Entering output mode exposes the previously cleared output latch. */
    value = qtest_readl(qts, BCM2711_GPIO_BASE + GPIO_GPFSEL0);
    value = deposit32(value, 5 * 3, 3, 1);
    qtest_writel(qts, BCM2711_GPIO_BASE + GPIO_GPFSEL0, value);
    g_assert_false(qtest_readl(qts, BCM2711_GPIO_BASE + GPIO_GPLEV0) &
                   BIT(5));
    g_assert_true(qtest_get_irq(qts, 5));
    qtest_writel(qts, BCM2711_GPIO_BASE + GPIO_GPSET0, BIT(5));
    g_assert_cmphex(qtest_readl(qts, BCM2711_GPIO_BASE + GPIO_GPLEV0) &
                    BIT(5), ==, BIT(5));
    qtest_set_irq_in(qts, GPIO_QOM_PATH, "pin-input", 5, 0);
    g_assert_cmphex(qtest_readl(qts, BCM2711_GPIO_BASE + GPIO_GPLEV0) &
                    BIT(5), ==, BIT(5));

    /* Leaving output mode deasserts output-enable and restores input. */
    value = deposit32(value, 5 * 3, 3, 0);
    qtest_writel(qts, BCM2711_GPIO_BASE + GPIO_GPFSEL0, value);
    g_assert_false(qtest_get_irq(qts, 5));
    g_assert_false(qtest_readl(qts, BCM2711_GPIO_BASE + GPIO_GPLEV0) &
                   BIT(5));

    /* A disconnected input resolves through the BCM2711 pull control. */
    qtest_set_irq_in(qts, GPIO_QOM_PATH, "pin-input", 5, -1);
    value = qtest_readl(qts, BCM2711_GPIO_BASE + GPIO_PULL0);
    qtest_writel(qts, BCM2711_GPIO_BASE + GPIO_PULL0,
                 deposit32(value, 5 * 2, 2, 1));
    g_assert_cmphex(qtest_readl(qts, BCM2711_GPIO_BASE + GPIO_GPLEV0) &
                    BIT(5), ==, BIT(5));
    qtest_writel(qts, BCM2711_GPIO_BASE + GPIO_PULL0,
                 deposit32(value, 5 * 2, 2, 2));
    g_assert_false(qtest_readl(qts, BCM2711_GPIO_BASE + GPIO_GPLEV0) &
                   BIT(5));

    /* BCM2711-only GPIO57 uses GPSET1 bit 25 and GPFSEL5 field 7. */
    qtest_set_irq_in(qts, GPIO_QOM_PATH, "pin-input", 57, 0);
    qtest_writel(qts, BCM2711_GPIO_BASE + GPIO_GPSET1, BIT(25));
    g_assert_false(qtest_readl(qts, BCM2711_GPIO_BASE + GPIO_GPLEV1) &
                   BIT(25));
    value = qtest_readl(qts, BCM2711_GPIO_BASE + GPIO_GPFSEL5);
    qtest_writel(qts, BCM2711_GPIO_BASE + GPIO_GPFSEL5,
                 deposit32(value, 7 * 3, 3, 1));
    g_assert_cmphex(qtest_readl(qts, BCM2711_GPIO_BASE + GPIO_GPLEV1) &
                    BIT(25), ==, BIT(25));
    g_assert_true(qtest_get_irq(qts, 57));

    qtest_quit(qts);
}
void test_bcm2711_gpio_events(void)
{
    static const unsigned int pins[] = { 5, 6, 7, 8, 31, 32, 57 };
    QTestState *qts = qtest_init("-M raspi4b");
    size_t i;

    qtest_irq_intercept_out_named(qts, GPIO_QOM_PATH, "sysbus-irq");
    for (i = 0; i < ARRAY_SIZE(pins); i++) {
        qtest_set_irq_in(qts, GPIO_QOM_PATH, "pin-input", pins[i], 0);
    }

    /* Rising-edge events route through all three BCM2711 GPIO groups. */
    qtest_writel(qts, BCM2711_GPIO_BASE + GPIO_GPREN0,
                 BIT(5) | BIT(31));
    qtest_writel(qts, BCM2711_GPIO_BASE + GPIO_GPREN1,
                 BIT(0) | BIT(25));
    qtest_set_irq_in(qts, GPIO_QOM_PATH, "pin-input", 5, 1);
    g_assert_cmphex(qtest_readl(qts, BCM2711_GPIO_BASE + GPIO_GPEDS0) &
                    BIT(5), ==, BIT(5));
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_writel(qts, BCM2711_GPIO_BASE + GPIO_GPEDS0, BIT(5));
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_set_irq_in(qts, GPIO_QOM_PATH, "pin-input", 31, 1);
    qtest_set_irq_in(qts, GPIO_QOM_PATH, "pin-input", 32, 1);
    g_assert_true(qtest_get_irq(qts, 1));
    qtest_writel(qts, BCM2711_GPIO_BASE + GPIO_GPEDS0, BIT(31));
    qtest_writel(qts, BCM2711_GPIO_BASE + GPIO_GPEDS1, BIT(0));
    g_assert_false(qtest_get_irq(qts, 1));

    qtest_set_irq_in(qts, GPIO_QOM_PATH, "pin-input", 57, 1);
    g_assert_true(qtest_get_irq(qts, 2));
    g_assert_false(qtest_get_irq(qts, 3));
    qtest_writel(qts, BCM2711_GPIO_BASE + GPIO_GPEDS1, BIT(25));
    g_assert_false(qtest_get_irq(qts, 2));

    /* Synchronous falling and asynchronous rising use the same latch. */
    qtest_writel(qts, BCM2711_GPIO_BASE + GPIO_GPFEN0, BIT(5));
    qtest_set_irq_in(qts, GPIO_QOM_PATH, "pin-input", 5, 0);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_writel(qts, BCM2711_GPIO_BASE + GPIO_GPEDS0, BIT(5));
    qtest_writel(qts, BCM2711_GPIO_BASE + GPIO_GPAREN0, BIT(6));
    qtest_set_irq_in(qts, GPIO_QOM_PATH, "pin-input", 6, 1);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_writel(qts, BCM2711_GPIO_BASE + GPIO_GPEDS0, BIT(6));

    /* Level detection immediately relatches until its enable is cleared. */
    qtest_writel(qts, BCM2711_GPIO_BASE + GPIO_GPHEN0, BIT(7));
    qtest_set_irq_in(qts, GPIO_QOM_PATH, "pin-input", 7, 1);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_writel(qts, BCM2711_GPIO_BASE + GPIO_GPEDS0, BIT(7));
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_writel(qts, BCM2711_GPIO_BASE + GPIO_GPHEN0, 0);
    qtest_writel(qts, BCM2711_GPIO_BASE + GPIO_GPEDS0, BIT(7));
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_writel(qts, BCM2711_GPIO_BASE + GPIO_GPLEN0, BIT(8));
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_writel(qts, BCM2711_GPIO_BASE + GPIO_GPLEN0, 0);
    qtest_writel(qts, BCM2711_GPIO_BASE + GPIO_GPEDS0, BIT(8));
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_quit(qts);
}
#ifndef _WIN32
void test_bcm2711_gpio_chardev_bridge(void)
{
    g_autofree char *socket_dir = NULL;
    g_autofree char *socket_path = NULL;
    g_autofree char *response = NULL;
    g_autofree char *long_line = NULL;
    QTestState *qts;
    uint32_t value;
    int fd;

    socket_dir = g_dir_make_tmp("qtest-rpi-gpio-XXXXXX", NULL);
    g_assert_nonnull(socket_dir);
    socket_path = g_strdup_printf("%s/gpio.sock", socket_dir);
    qts = qtest_initf("-chardev socket,id=gpio,path=%s,server=on,wait=off "
                      "-M raspi4b,gpio-chardev=gpio", socket_path);
    fd = gpio_bridge_connect(socket_path);
    g_assert_cmpint(fd, >=, 0);

    response = gpio_bridge_read(fd);
    g_assert_nonnull(strstr(response, "RPI-GPIO 1 58\n"));

    gpio_bridge_write(fd, "PI");
    gpio_bridge_write(fd, "NG\n");
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(fd);
    g_assert_cmpstr(response, ==, "PONG 1\n");

    gpio_bridge_write(fd, "SET 5 1\n");
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(fd);
    g_assert_cmpstr(response, ==, "PIN 5 1 0\nOK\n");
    g_assert_cmphex(qtest_readl(qts, BCM2711_GPIO_BASE + GPIO_GPLEV0) &
                    BIT(5), ==, BIT(5));

    /* Direction and output changes are pushed to the host transport. */
    qtest_writel(qts, BCM2711_GPIO_BASE + GPIO_GPCLR0, BIT(5));
    value = qtest_readl(qts, BCM2711_GPIO_BASE + GPIO_GPFSEL0);
    qtest_writel(qts, BCM2711_GPIO_BASE + GPIO_GPFSEL0,
                 deposit32(value, 5 * 3, 3, 1));
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(fd);
    g_assert_nonnull(strstr(response, "PIN 5 0 1\n"));
    qtest_writel(qts, BCM2711_GPIO_BASE + GPIO_GPSET0, BIT(5));
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(fd);
    g_assert_cmpstr(response, ==, "PIN 5 1 1\n");

    /* An external input is retained but does not override an output. */
    gpio_bridge_write(fd, "SET 5 0\nGET 5\n");
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(fd);
    g_assert_nonnull(strstr(response, "PIN 5 1 1\nOK\n"));
    g_assert_nonnull(strstr(response, "PIN 5 1 1\n"));
    qtest_writel(qts, BCM2711_GPIO_BASE + GPIO_GPFSEL0,
                 deposit32(value, 5 * 3, 3, 0));
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(fd);
    g_assert_cmpstr(response, ==, "PIN 5 0 0\n");

    gpio_bridge_write(fd, "SET 58 1\nSET 5 X\nUNKNOWN\n");
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(fd);
    g_assert_cmpstr(response, ==,
                    "ERR pin-range\nERR pin-state\nERR syntax\n");

    long_line = g_strnfill(GPIO_BRIDGE_LINE_SIZE + 8, 'A');
    gpio_bridge_write(fd, long_line);
    gpio_bridge_write(fd, "\nPING\n");
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(fd);
    g_assert_cmpstr(response, ==, "ERR line-too-long\nPONG 1\n");

    gpio_bridge_write(fd, "GET ALL\n");
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(fd);
    g_assert_nonnull(strstr(response, "PIN 0 "));
    g_assert_nonnull(strstr(response, "PIN 57 "));
    g_assert_true(g_str_has_suffix(response, "END\n"));

    /* Peripheral alternate-function edges retain output ownership on wire. */
    configure_pwm_clock(qts);
    value = qtest_readl(qts, BCM2711_GPIO_BASE + GPIO_GPFSEL1);
    qtest_writel(qts, BCM2711_GPIO_BASE + GPIO_GPFSEL1,
                 deposit32(value, (12 - 10) * 3, 3, 4));
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(fd);
    g_assert_nonnull(strstr(response, "PIN 12 0 1\n"));
    qtest_writel(qts, BCM2711_PWM_BASE + PWM_RNG1, 10);
    qtest_writel(qts, BCM2711_PWM_BASE + PWM_DAT1, 3);
    qtest_writel(qts, BCM2711_PWM_BASE + PWM_CTL,
                 BIT(0) | PWM_CTL_MSEN1);
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(fd);
    g_assert_nonnull(strstr(response, "PIN 12 1 1\n"));
    qtest_clock_step(qts, 30000);
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(fd);
    g_assert_nonnull(strstr(response, "PIN 12 0 1\n"));
    qtest_writel(qts, BCM2711_PWM_BASE + PWM_CTL, 0);
    qtest_writel(qts, BCM2711_GPIO_BASE + GPIO_GPFSEL1, value);
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(fd);

    /* Releasing ownership disconnects inputs and restores pull resolution. */
    value = qtest_readl(qts, BCM2711_GPIO_BASE + GPIO_PULL0);
    qtest_writel(qts, BCM2711_GPIO_BASE + GPIO_PULL0,
                 deposit32(value, 5 * 2, 2, 1));
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(fd);
    gpio_bridge_write(fd, "SET 5 0\nRELEASE ALL\n");
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(fd);
    g_assert_nonnull(strstr(response, "PIN 5 0 0\nOK\n"));
    g_assert_nonnull(strstr(response, "PIN 5 1 0\n"));
    g_assert_true(g_str_has_suffix(response, "OK\n"));
    g_assert_cmphex(qtest_readl(qts, BCM2711_GPIO_BASE + GPIO_GPLEV0) &
                    BIT(5), ==, BIT(5));

    qtest_system_reset(qts);
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(fd);
    g_assert_nonnull(strstr(response, "RESET\n"));
    close(fd);
    qtest_quit(qts);
    unlink(socket_path);

    /* The inherited machine property is also accepted by CM4. */
    g_free(socket_path);
    socket_path = g_strdup_printf("%s/cm4-gpio.sock", socket_dir);
    qts = qtest_initf("-chardev socket,id=gpio,path=%s,server=on,wait=off "
                      "-M raspi-cm4,boot-mode=behavioral,nrpiboot=off,"
                      "gpio-chardev=gpio", socket_path);
    fd = gpio_bridge_connect(socket_path);
    g_assert_cmpint(fd, >=, 0);
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(fd);
    g_assert_nonnull(strstr(response, "RPI-GPIO 1 58\n"));

    gpio_bridge_write(fd, "SET 40 0\n");
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(fd);
    g_assert_nonnull(strstr(response, "PIN 40 0 0\nOK\n"));
    qtest_system_reset(qts);
    g_assert_true(qom_get_bool(qts, "nrpiboot-sampled"));
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(fd);
    g_assert_nonnull(strstr(response, "RESET\n"));

    gpio_bridge_write(fd, "SET 40 1\n");
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(fd);
    g_assert_nonnull(strstr(response, "PIN 40 1 0\nOK\n"));
    qtest_system_reset(qts);
    g_assert_false(qom_get_bool(qts, "nrpiboot-sampled"));
    close(fd);
    qtest_quit(qts);
    unlink(socket_path);
    rmdir(socket_dir);
}
#endif
#ifndef _WIN32
void test_bcm2711_gpio_chardev_migration(void)
{
    g_autofree char *directory = NULL;
    g_autofree char *source_socket = NULL;
    g_autofree char *destination_socket = NULL;
    g_autofree char *migration_socket = NULL;
    g_autofree char *migration_uri = NULL;
    g_autofree char *response = NULL;
    struct pollfd destination_poll;
    QTestState *source;
    QTestState *destination;
    uint32_t fsel;
    int source_fd;
    int destination_fd;

    directory = g_dir_make_tmp("qtest-rpi-gpio-migration-XXXXXX", NULL);
    g_assert_nonnull(directory);
    source_socket = g_build_filename(directory, "source.sock", NULL);
    destination_socket = g_build_filename(directory, "destination.sock",
                                          NULL);
    migration_socket = g_build_filename(directory, "migration.sock", NULL);
    migration_uri = g_strdup_printf("unix:%s", migration_socket);

    source = qtest_initf(
        "-chardev socket,id=gpio,path=%s,server=on,wait=off "
        "-M raspi4b,gpio-chardev=gpio", source_socket);
    source_fd = gpio_bridge_connect(source_socket);
    g_assert_cmpint(source_fd, >=, 0);
    response = gpio_bridge_read(source_fd);
    g_assert_cmpstr(response, ==, "RPI-GPIO 1 58\n");

    gpio_bridge_write(source_fd, "SET 5 0\n");
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(source_fd);
    g_assert_nonnull(strstr(response, "PIN 5 0 0\nOK\n"));
    qtest_writel(source, BCM2711_GPIO_BASE + GPIO_GPREN0, BIT(5));
    qtest_writel(source, BCM2711_GPIO_BASE + GPIO_GPFEN0, BIT(5));
    gpio_bridge_write(source_fd, "SET 5 1\n");
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(source_fd);
    g_assert_nonnull(strstr(response, "PIN 5 1 0\nOK\n"));
    g_assert_cmphex(qtest_readl(source, BCM2711_GPIO_BASE + GPIO_GPEDS0) &
                    BIT(5), ==, BIT(5));
    gpio_bridge_write(source_fd, "SET SIGNAL EEPROM_NWP 0\n");
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(source_fd);
    g_assert_cmpstr(response, ==, "SIGNAL EEPROM_NWP 0\nOK\n");
    gpio_bridge_write(source_fd, "SET SIGNAL SD_OVERCURRENT 1\n");
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(source_fd);
    g_assert_cmpstr(response, ==, "SIGNAL SD_OVERCURRENT 1\nOK\n");

    qtest_writel(source, BCM2711_GPIO_BASE + GPIO_GPSET0, BIT(6));
    fsel = qtest_readl(source, BCM2711_GPIO_BASE + GPIO_GPFSEL0);
    qtest_writel(source, BCM2711_GPIO_BASE + GPIO_GPFSEL0,
                 deposit32(fsel, 6 * 3, 3, 1));
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(source_fd);
    g_assert_nonnull(strstr(response, "PIN 6 1 1\n"));
    fsel = qtest_readl(source, BCM2711_GPIO_BASE + GPIO_PULL0);
    qtest_writel(source, BCM2711_GPIO_BASE + GPIO_PULL0,
                 deposit32(fsel, 7 * 2, 2, 1));
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(source_fd);
    g_assert_nonnull(strstr(response, "PIN 7 1 0\n"));

    destination = qtest_initf(
        "-chardev socket,id=gpio,path=%s,server=on,wait=off "
        "-M raspi4b,gpio-chardev=gpio -incoming %s",
        destination_socket, migration_uri);
    qtest_irq_intercept_out_named(destination, GPIO_QOM_PATH, "sysbus-irq");
    destination_fd = gpio_bridge_connect(destination_socket);
    g_assert_cmpint(destination_fd, >=, 0);
    destination_poll = (struct pollfd) {
        .fd = destination_fd,
        .events = POLLIN,
    };
    g_assert_cmpint(poll(&destination_poll, 1, 50), ==, 0);

    qtest_qmp_assert_success(
        source, "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }",
        migration_uri);
    wait_for_migration_complete(source);
    wait_for_migration_complete(destination);

    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(destination_fd);
    g_assert_cmpstr(response, ==, "RPI-GPIO 1 58\n");
    gpio_bridge_write(destination_fd, "GET ALL\n");
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(destination_fd);
    g_assert_nonnull(strstr(response, "PIN 5 1 0\n"));
    g_assert_nonnull(strstr(response, "PIN 6 1 1\n"));
    g_assert_nonnull(strstr(response, "PIN 7 1 0\n"));
    g_assert_true(g_str_has_suffix(response, "END\n"));
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_GPIO_BASE + GPIO_GPEDS0) & BIT(5),
                    ==, BIT(5));
    g_assert_true(qtest_get_irq(destination, 0));
    g_assert_cmphex(extract32(qtest_readl(
                        destination, BCM2711_GPIO_BASE + GPIO_PULL0),
                    7 * 2, 2), ==, 1);
    gpio_bridge_write(destination_fd, "GET SIGNALS\n");
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(destination_fd);
    g_assert_cmpstr(response, ==,
                    "SIGNAL EEPROM_NWP 0\n"
                    "SIGNAL SD_OVERCURRENT 1\nEND\n");
    g_assert_true(qom_get_bool(destination, "sd-overcurrent"));
    {
        g_autofree char *signal_source =
            qom_get_string(destination, "sd-overcurrent-source");

        g_assert_cmpstr(signal_source, ==, "gpio-bridge");
    }

    qtest_writel(destination, BCM2711_GPIO_BASE + GPIO_GPEDS0, BIT(5));
    gpio_bridge_write(destination_fd, "SET 5 0\n");
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(destination_fd);
    g_assert_nonnull(strstr(response, "PIN 5 0 0\nOK\n"));
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_GPIO_BASE + GPIO_GPEDS0) & BIT(5),
                    ==, BIT(5));

    close(source_fd);
    close(destination_fd);
    qtest_quit(source);
    qtest_quit(destination);
    unlink(source_socket);
    unlink(destination_socket);
    unlink(migration_socket);
    rmdir(directory);
}
#endif
#ifndef _WIN32
void test_eeprom_nwp_gpio_chardev(void)
{
    g_autofree char *socket_dir = NULL;
    g_autofree char *socket_path = NULL;
    g_autofree char *extra_drive = NULL;
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree uint8_t *expected = NULL;
    g_autofree char *response = NULL;
    g_autofree char *status = NULL;
    g_autofree char *source = NULL;
    QTestState *qts;
    int fd;

    socket_dir = g_dir_make_tmp("qtest-rpi-eeprom-nwp-XXXXXX", NULL);
    g_assert_nonnull(socket_dir);
    socket_path = g_build_filename(socket_dir, "gpio.sock", NULL);
    extra_drive = g_strdup_printf(
        "-chardev socket,id=gpio,path=%s,server=on,wait=off ",
        socket_path);
    qts = start_recovery_stage_extra(
        false, true, UINT64_MAX, "program", false, false, NULL,
        ",eeprom-nwp=on,sd-recovery-enabled=off,gpio-chardev=gpio",
        "eeprom_write_protect=0\n", extra_drive,
        &eeprom_path, &sd_path, &expected);
    fd = gpio_bridge_connect(socket_path);
    g_assert_cmpint(fd, >=, 0);
    response = gpio_bridge_read(fd);
    g_assert_cmpstr(response, ==, "RPI-GPIO 1 58\n");

    gpio_bridge_write(fd, "GET SIGNALS\n");
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(fd);
    g_assert_cmpstr(response, ==,
                    "SIGNAL EEPROM_NWP Z\n"
                    "SIGNAL SD_OVERCURRENT Z\nEND\n");

    gpio_bridge_write(fd, "SET SIGNAL EEPROM_NWP 0\n");
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(fd);
    g_assert_cmpstr(response, ==, "SIGNAL EEPROM_NWP 0\nOK\n");
    {
        QDict *reply = qtest_qmp(
            qts,
            "{ 'execute': 'qom-set', 'arguments': {"
            " 'path': '/machine',"
            " 'property': 'eeprom-status-write-protect',"
            " 'value': false } }");

        g_assert_nonnull(qdict_get_qdict(reply, "error"));
        qobject_unref(reply);
        g_assert_true(qom_get_bool(
                          qts, "eeprom-status-write-protect"));
    }
    qom_set_bool(qts, "sd-recovery-enabled", true);
    qtest_system_reset(qts);
    status = qom_get_string(qts, "recovery-status");
    source = qom_get_string(qts, "eeprom-nwp-source");
    g_assert_cmpstr(status, ==, "recovery-write-protect-config-locked");
    g_assert_cmpstr(source, ==, "gpio-bridge");
    g_assert_false(qom_get_bool(qts, "eeprom-nwp"));
    g_assert_false(qom_get_bool(qts, "eeprom-nwp-sampled"));

    gpio_bridge_write(fd, "SET SIGNAL EEPROM_NWP 1\n");
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(fd);
    g_assert_nonnull(strstr(response, "SIGNAL EEPROM_NWP 1\nOK\n"));
    qtest_system_reset(qts);
    g_clear_pointer(&status, g_free);
    status = qom_get_string(qts, "recovery-status");
    g_assert_cmpstr(status, ==, "recovery-updated-reboot");
    g_assert_false(qom_get_bool(
                       qts, "eeprom-status-write-protect"));
    g_assert_true(qom_get_bool(qts, "eeprom-nwp-sampled"));

    gpio_bridge_write(fd, "SET SIGNAL EEPROM_NWP Z\n");
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(fd);
    g_assert_nonnull(strstr(response, "SIGNAL EEPROM_NWP Z\nOK\n"));
    g_clear_pointer(&source, g_free);
    source = qom_get_string(qts, "eeprom-nwp-source");
    g_assert_cmpstr(source, ==, "machine-property");
    g_assert_true(qom_get_bool(qts, "eeprom-nwp"));
    gpio_bridge_write(fd, "SET SIGNAL EEPROM_NWP X\n");
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(fd);
    g_assert_cmpstr(response, ==, "ERR signal-state\n");

    close(fd);
    qtest_quit(qts);
    assert_file_equals(eeprom_path, expected, EEPROM_SIZE);
    unlink(eeprom_path);
    unlink(sd_path);
    unlink(socket_path);
    rmdir(socket_dir);
}
#endif
void test_bcm2711_legacy_sdhost_registers(void)
{
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    QTestState *qts = qtest_init("-M raspi4b -nic none");
    QTestState *destination;

    g_assert_cmphex(qtest_readl(qts, BCM2711_SDHOST_BASE + SDHOST_STATUS),
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, BCM2711_SDHOST_BASE + SDHOST_TIMEOUT),
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, BCM2711_SDHOST_BASE + SDHOST_CDIV),
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, BCM2711_SDHOST_BASE + SDHOST_VDD),
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, BCM2711_SDHOST_BASE + SDHOST_EDM),
                    ==, 0xc60f);

    qtest_writel(qts, BCM2711_SDHOST_BASE + SDHOST_TIMEOUT, 0x12345678);
    qtest_writel(qts, BCM2711_SDHOST_BASE + SDHOST_CDIV, UINT32_MAX);
    qtest_writel(qts, BCM2711_SDHOST_BASE + SDHOST_VDD, 3);
    g_assert_cmphex(qtest_readl(qts, BCM2711_SDHOST_BASE + SDHOST_TIMEOUT),
                    ==, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, BCM2711_SDHOST_BASE + SDHOST_CDIV),
                    ==, 0x7ff);
    g_assert_cmphex(qtest_readl(qts, BCM2711_SDHOST_BASE + SDHOST_VDD),
                    ==, 1);

    g_assert_cmphex(qtest_readl(qts, BCM2711_SDHOST_BASE + SDHOST_DATA),
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, BCM2711_SDHOST_BASE + SDHOST_STATUS) &
                    SDHOST_STATUS_FIFO_ERROR, ==,
                    SDHOST_STATUS_FIFO_ERROR);
    qtest_writel(qts, BCM2711_SDHOST_BASE + SDHOST_STATUS,
                 SDHOST_STATUS_FIFO_ERROR);
    for (unsigned int i = 0; i <= SDHOST_FIFO_WORDS; i++) {
        qtest_writel(qts, BCM2711_SDHOST_BASE + SDHOST_DATA, i);
    }
    g_assert_cmphex(qtest_readl(qts, BCM2711_SDHOST_BASE + SDHOST_STATUS) &
                    SDHOST_STATUS_FIFO_ERROR, ==,
                    SDHOST_STATUS_FIFO_ERROR);

    qtest_writel(qts, BCM2711_SDHOST_BASE + SDHOST_STATUS,
                 SDHOST_STATUS_FIFO_ERROR);
    qtest_writel(qts, BCM2711_SDHOST_BASE + SDHOST_CMD,
                 SDHOST_CMD_NEW | 8);
    g_assert_cmphex(qtest_readl(qts, BCM2711_SDHOST_BASE + SDHOST_CMD) &
                    (SDHOST_CMD_NEW | SDHOST_CMD_FAIL), ==,
                    SDHOST_CMD_FAIL);
    g_assert_cmphex(qtest_readl(qts, BCM2711_SDHOST_BASE + SDHOST_STATUS) &
                    SDHOST_STATUS_CMD_TIMEOUT, ==,
                    SDHOST_STATUS_CMD_TIMEOUT);

    destination = migrate_to_new_qtest(
        qts, "-M raspi4b -nic none", &migration_dir, &migration_socket);
    g_assert_cmphex(qtest_readl(
                        destination,
                        BCM2711_SDHOST_BASE + SDHOST_TIMEOUT),
                    ==, 0x12345678);
    g_assert_cmphex(qtest_readl(
                        destination, BCM2711_SDHOST_BASE + SDHOST_CDIV),
                    ==, 0x7ff);
    g_assert_cmphex(qtest_readl(
                        destination, BCM2711_SDHOST_BASE + SDHOST_STATUS) &
                    (SDHOST_STATUS_CMD_TIMEOUT |
                     SDHOST_STATUS_FIFO_ERROR), ==,
                    SDHOST_STATUS_CMD_TIMEOUT);

    qtest_system_reset(destination);
    g_assert_cmphex(qtest_readl(
                        destination, BCM2711_SDHOST_BASE + SDHOST_STATUS),
                    ==, 0);
    g_assert_cmphex(qtest_readl(
                        destination, BCM2711_SDHOST_BASE + SDHOST_TIMEOUT),
                    ==, 0);
    g_assert_cmphex(qtest_readl(
                        destination, BCM2711_SDHOST_BASE + SDHOST_CDIV),
                    ==, 0);
    g_assert_cmphex(qtest_readl(
                        destination, BCM2711_SDHOST_BASE + SDHOST_VDD),
                    ==, 0);
    g_assert_cmphex(qtest_readl(
                        destination, BCM2711_SDHOST_BASE + SDHOST_EDM),
                    ==, 0xc60f);
    qtest_quit(qts);
    qtest_quit(destination);
    unlink(migration_socket);
    rmdir(migration_dir);
}
void test_bcm2711_i2c_controllers(void)
{
    static const uint64_t bases[] = {
        BCM2711_BSC0_BASE,
        BCM2711_BSC1_BASE,
        BCM2711_BSC2_BASE,
    };
    const char *command =
        "-M raspi4b -nic none "
        "-device tmp105,address=0x50,bus=i2c-bus.0 "
        "-device tmp105,address=0x50,bus=i2c-bus.1 "
        "-device tmp105,address=0x50,bus=i2c-bus.2";
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    QTestState *qts = qtest_init(command);
    QTestState *destination;

    qtest_irq_intercept_out_named(qts, BCM2711_BSC0_QOM_PATH,
                                  "sysbus-irq");

    for (unsigned int i = 0; i < ARRAY_SIZE(bases); i++) {
        uint64_t base = bases[i];
        uint32_t status;

        qtest_writel(qts, base + BCM2835_I2C_A, 0xd1);
        qtest_writel(qts, base + BCM2835_I2C_DLEN, 0x10001);
        qtest_writel(qts, base + BCM2835_I2C_DIV, 0x12345678);
        qtest_writel(qts, base + BCM2835_I2C_CLKT, 0x10040);
        g_assert_cmphex(qtest_readl(qts, base + BCM2835_I2C_A), ==, 0x51);
        g_assert_cmphex(qtest_readl(qts, base + BCM2835_I2C_DLEN), ==, 1);
        g_assert_cmphex(qtest_readl(qts, base + BCM2835_I2C_DIV), ==,
                        0x5678);
        g_assert_cmphex(qtest_readl(qts, base + BCM2835_I2C_CLKT), ==,
                        0x40);

        qtest_writel(qts, base + BCM2835_I2C_C,
                     BCM2835_I2C_C_ST | BCM2835_I2C_C_INTD);
        status = qtest_readl(qts, base + BCM2835_I2C_S);
        g_assert_false(status & (BCM2835_I2C_S_TA |
                                 BCM2835_I2C_S_DONE |
                                 BCM2835_I2C_S_ERR));

        qtest_writel(qts, base + BCM2835_I2C_C,
                     BCM2835_I2C_C_I2CEN | BCM2835_I2C_C_INTD);
        status = qtest_readl(qts, base + BCM2835_I2C_S);
        g_assert_false(status & (BCM2835_I2C_S_TA |
                                 BCM2835_I2C_S_DONE |
                                 BCM2835_I2C_S_ERR));

        qtest_writel(qts, base + BCM2835_I2C_C,
                     BCM2835_I2C_C_I2CEN | BCM2835_I2C_C_INTD |
                     BCM2835_I2C_C_ST | BCM2835_I2C_C_CLEAR);
        status = qtest_readl(qts, base + BCM2835_I2C_S);
        g_assert_true(status & BCM2835_I2C_S_DONE);
        g_assert_true(status & BCM2835_I2C_S_ERR);
        g_assert_false(status & BCM2835_I2C_S_TA);
        if (i == 0) {
            g_assert_true(qtest_get_irq(qts, 0));
        }
        qtest_writel(qts, base + BCM2835_I2C_S,
                     BCM2835_I2C_S_DONE | BCM2835_I2C_S_ERR);
        if (i == 0) {
            g_assert_false(qtest_get_irq(qts, 0));
        }

        qtest_writel(qts, base + BCM2835_I2C_A, 0x50);
        qtest_writel(qts, base + BCM2835_I2C_DLEN, 3);
        qtest_writel(qts, base + BCM2835_I2C_C,
                     BCM2835_I2C_C_I2CEN | BCM2835_I2C_C_INTT |
                     BCM2835_I2C_C_INTD | BCM2835_I2C_C_ST);
        qtest_writel(qts, base + BCM2835_I2C_FIFO, 3);
        qtest_writel(qts, base + BCM2835_I2C_FIFO, 0xde);
        qtest_writel(qts, base + BCM2835_I2C_FIFO, 0xad);
        status = qtest_readl(qts, base + BCM2835_I2C_S);
        g_assert_true(status & BCM2835_I2C_S_DONE);
        g_assert_false(status & BCM2835_I2C_S_ERR);
    }

    /*
     * Preload all sixteen TX slots.  A seventeenth write is ignored.  After
     * the engine drains those bytes, one DLEN byte remains and TXW requests
     * the final byte.
     */
    qtest_writel(qts, BCM2711_BSC1_BASE + BCM2835_I2C_S,
                 BCM2835_I2C_S_DONE);
    qtest_writel(qts, BCM2711_BSC1_BASE + BCM2835_I2C_C,
                 BCM2835_I2C_C_I2CEN | BCM2835_I2C_C_CLEAR);
    qtest_writel(qts, BCM2711_BSC1_BASE + BCM2835_I2C_A, 0x50);
    qtest_writel(qts, BCM2711_BSC1_BASE + BCM2835_I2C_DLEN, 17);
    for (unsigned int i = 0; i < BCM2835_I2C_FIFO_LEN + 1; i++) {
        qtest_writel(qts, BCM2711_BSC1_BASE + BCM2835_I2C_FIFO, i);
    }
    g_assert_false(qtest_readl(qts,
                               BCM2711_BSC1_BASE + BCM2835_I2C_S) &
                   BCM2835_I2C_S_TXD);
    g_assert_false(qtest_readl(qts,
                               BCM2711_BSC1_BASE + BCM2835_I2C_S) &
                   BCM2835_I2C_S_TXE);
    qtest_writel(qts, BCM2711_BSC1_BASE + BCM2835_I2C_C,
                 BCM2835_I2C_C_I2CEN | BCM2835_I2C_C_INTT |
                 BCM2835_I2C_C_INTD | BCM2835_I2C_C_ST);
    g_assert_cmphex(qtest_readl(qts,
                                BCM2711_BSC1_BASE + BCM2835_I2C_DLEN),
                    ==, 1);
    g_assert_cmphex(qtest_readl(qts,
                                BCM2711_BSC1_BASE + BCM2835_I2C_S) &
                    (BCM2835_I2C_S_TA | BCM2835_I2C_S_TXW |
                     BCM2835_I2C_S_TXD | BCM2835_I2C_S_TXE), ==,
                    BCM2835_I2C_S_TA | BCM2835_I2C_S_TXW |
                    BCM2835_I2C_S_TXD | BCM2835_I2C_S_TXE);
    qtest_writel(qts, BCM2711_BSC1_BASE + BCM2835_I2C_FIFO, 0xaa);
    g_assert_cmphex(qtest_readl(qts,
                                BCM2711_BSC1_BASE + BCM2835_I2C_DLEN),
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts,
                                BCM2711_BSC1_BASE + BCM2835_I2C_S) &
                    (BCM2835_I2C_S_DONE | BCM2835_I2C_S_TA |
                     BCM2835_I2C_S_TXE), ==,
                    BCM2835_I2C_S_DONE | BCM2835_I2C_S_TXE);

    /* FIFO clear without ST aborts an active transfer and empties the FIFO. */
    qtest_writel(qts, BCM2711_BSC2_BASE + BCM2835_I2C_S,
                 BCM2835_I2C_S_DONE);
    qtest_writel(qts, BCM2711_BSC2_BASE + BCM2835_I2C_C,
                 BCM2835_I2C_C_I2CEN | BCM2835_I2C_C_CLEAR);
    qtest_writel(qts, BCM2711_BSC2_BASE + BCM2835_I2C_A, 0x50);
    qtest_writel(qts, BCM2711_BSC2_BASE + BCM2835_I2C_DLEN, 2);
    qtest_writel(qts, BCM2711_BSC2_BASE + BCM2835_I2C_FIFO, 3);
    qtest_writel(qts, BCM2711_BSC2_BASE + BCM2835_I2C_C,
                 BCM2835_I2C_C_I2CEN | BCM2835_I2C_C_INTT |
                 BCM2835_I2C_C_ST);
    g_assert_true(qtest_readl(qts,
                              BCM2711_BSC2_BASE + BCM2835_I2C_S) &
                  BCM2835_I2C_S_TA);
    qtest_writel(qts, BCM2711_BSC2_BASE + BCM2835_I2C_C,
                 BCM2835_I2C_C_I2CEN | BCM2835_I2C_C_CLEAR);
    g_assert_cmphex(qtest_readl(qts,
                                BCM2711_BSC2_BASE + BCM2835_I2C_S) &
                    (BCM2835_I2C_S_DONE | BCM2835_I2C_S_TA |
                     BCM2835_I2C_S_TXD | BCM2835_I2C_S_TXE), ==,
                    BCM2835_I2C_S_DONE | BCM2835_I2C_S_TXD |
                    BCM2835_I2C_S_TXE);

    qtest_writel(qts, BCM2711_BSC0_BASE + BCM2835_I2C_S,
                 BCM2835_I2C_S_DONE);
    qtest_writel(qts, BCM2711_BSC0_BASE + BCM2835_I2C_DLEN, 1);
    qtest_writel(qts, BCM2711_BSC0_BASE + BCM2835_I2C_C,
                 BCM2835_I2C_C_I2CEN | BCM2835_I2C_C_INTT |
                 BCM2835_I2C_C_ST);
    g_assert_true(qtest_readl(qts,
                              BCM2711_BSC0_BASE + BCM2835_I2C_S) &
                  BCM2835_I2C_S_TA);
    qtest_system_reset(qts);
    g_assert_false(qtest_get_irq(qts, 0));

    for (unsigned int i = 0; i < ARRAY_SIZE(bases); i++) {
        g_assert_cmphex(qtest_readl(qts, bases[i] + BCM2835_I2C_C), ==, 0);
        g_assert_cmphex(qtest_readl(qts, bases[i] + BCM2835_I2C_DLEN), ==,
                        0);
        g_assert_cmphex(qtest_readl(qts, bases[i] + BCM2835_I2C_A), ==, 0);
        g_assert_cmphex(qtest_readl(qts, bases[i] + BCM2835_I2C_S), ==,
                        BCM2835_I2C_S_TXD | BCM2835_I2C_S_TXE);
    }

    /*
     * Leave one write byte outstanding, issue a repeated start into read
     * mode, consume one byte, and migrate with the second byte still active.
     */
    qtest_writel(qts, BCM2711_BSC0_BASE + BCM2835_I2C_A, 0x50);
    qtest_writel(qts, BCM2711_BSC0_BASE + BCM2835_I2C_DLEN, 2);
    qtest_writel(qts, BCM2711_BSC0_BASE + BCM2835_I2C_C,
                 BCM2835_I2C_C_I2CEN | BCM2835_I2C_C_INTT |
                 BCM2835_I2C_C_ST);
    qtest_writel(qts, BCM2711_BSC0_BASE + BCM2835_I2C_FIFO, 3);
    g_assert_cmphex(qtest_readl(qts,
                                BCM2711_BSC0_BASE + BCM2835_I2C_DLEN),
                    ==, 1);
    g_assert_true(qtest_readl(qts,
                              BCM2711_BSC0_BASE + BCM2835_I2C_S) &
                  BCM2835_I2C_S_TA);

    qtest_writel(qts, BCM2711_BSC0_BASE + BCM2835_I2C_DLEN, 17);
    qtest_writel(qts, BCM2711_BSC0_BASE + BCM2835_I2C_C,
                 BCM2835_I2C_C_I2CEN | BCM2835_I2C_C_INTR |
                 BCM2835_I2C_C_INTD | BCM2835_I2C_C_READ |
                 BCM2835_I2C_C_ST);
    g_assert_cmphex(qtest_readl(qts,
                                BCM2711_BSC0_BASE + BCM2835_I2C_DLEN),
                    ==, 1);
    g_assert_cmphex(qtest_readl(qts,
                                BCM2711_BSC0_BASE + BCM2835_I2C_S) &
                    (BCM2835_I2C_S_TA | BCM2835_I2C_S_RXF |
                     BCM2835_I2C_S_RXR | BCM2835_I2C_S_RXD), ==,
                    BCM2835_I2C_S_TA | BCM2835_I2C_S_RXF |
                    BCM2835_I2C_S_RXR | BCM2835_I2C_S_RXD);
    g_assert_true(qtest_get_irq(qts, 0));

    destination = migrate_to_new_qtest(
        qts, command, &migration_dir, &migration_socket);
    qtest_irq_intercept_out_named(destination, BCM2711_BSC0_QOM_PATH,
                                  "sysbus-irq");
    g_assert_true(qtest_readl(destination,
                              BCM2711_BSC0_BASE + BCM2835_I2C_S) &
                  BCM2835_I2C_S_TA);
    g_assert_cmphex(qtest_readl(destination,
                                BCM2711_BSC0_BASE + BCM2835_I2C_DLEN),
                    ==, 1);
    g_assert_cmphex(qtest_readl(destination,
                                BCM2711_BSC0_BASE + BCM2835_I2C_S) &
                    (BCM2835_I2C_S_RXF | BCM2835_I2C_S_RXR |
                     BCM2835_I2C_S_RXD), ==,
                    BCM2835_I2C_S_RXF | BCM2835_I2C_S_RXR |
                    BCM2835_I2C_S_RXD);
    g_assert_cmphex(qtest_readl(destination,
                                BCM2711_BSC0_BASE + BCM2835_I2C_FIFO),
                    ==, 0xde);
    g_assert_true(qtest_readl(destination,
                              BCM2711_BSC0_BASE + BCM2835_I2C_S) &
                  BCM2835_I2C_S_DONE);
    g_assert_false(qtest_readl(destination,
                               BCM2711_BSC0_BASE + BCM2835_I2C_S) &
                   BCM2835_I2C_S_TA);
    g_assert_true(qtest_get_irq(destination, 0));
    for (unsigned int i = 1; i < 17; i++) {
        uint32_t expected = i == 1 ? 0xa0 : 0xff;

        g_assert_cmphex(qtest_readl(
                            destination,
                            BCM2711_BSC0_BASE + BCM2835_I2C_FIFO),
                        ==, expected);
    }
    g_assert_false(qtest_readl(destination,
                               BCM2711_BSC0_BASE + BCM2835_I2C_S) &
                   (BCM2835_I2C_S_RXF | BCM2835_I2C_S_RXR |
                    BCM2835_I2C_S_RXD));
    qtest_writel(destination, BCM2711_BSC0_BASE + BCM2835_I2C_S,
                 BCM2835_I2C_S_DONE);
    g_assert_false(qtest_get_irq(destination, 0));

    qtest_quit(qts);
    qtest_quit(destination);
    unlink(migration_socket);
    rmdir(migration_dir);
}
void test_bcm2711_i2c_ten_bit_migration(void)
{
    const char *command =
        "-M raspi4b -nic none "
        "-device tmp105,address=0x52,ten-bit-address=0x2aa,bus=i2c-bus.0";
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    QTestState *source = qtest_init(command);
    QTestState *destination;
    uint32_t status;

    /*
     * Migrate after selecting 0x2aa and sending two of three TMP105 payload
     * bytes.  This covers the generic bus's active 10-bit target identity as
     * well as the BSC's translated address and in-flight FIFO state.
     */
    qtest_writel(source, BCM2711_BSC0_BASE + BCM2835_I2C_A, 0x7a);
    qtest_writel(source, BCM2711_BSC0_BASE + BCM2835_I2C_DIV, 0x9c4);
    qtest_writel(source, BCM2711_BSC0_BASE + BCM2835_I2C_DEL, 0x00400050);
    qtest_writel(source, BCM2711_BSC0_BASE + BCM2835_I2C_DLEN, 4);
    qtest_writel(source, BCM2711_BSC0_BASE + BCM2835_I2C_C,
                 BCM2835_I2C_C_I2CEN | BCM2835_I2C_C_INTT |
                 BCM2835_I2C_C_INTD | BCM2835_I2C_C_ST);
    qtest_writel(source, BCM2711_BSC0_BASE + BCM2835_I2C_FIFO, 0xaa);
    qtest_writel(source, BCM2711_BSC0_BASE + BCM2835_I2C_FIFO, 3);
    qtest_writel(source, BCM2711_BSC0_BASE + BCM2835_I2C_FIFO, 0xde);
    g_assert_cmphex(qtest_readl(source,
                                BCM2711_BSC0_BASE + BCM2835_I2C_DLEN),
                    ==, 1);
    g_assert_true(qtest_readl(source,
                              BCM2711_BSC0_BASE + BCM2835_I2C_S) &
                  BCM2835_I2C_S_TA);

    destination = migrate_to_new_qtest(
        source, command, &migration_dir, &migration_socket);
    g_assert_cmphex(qtest_readl(destination,
                                BCM2711_BSC0_BASE + BCM2835_I2C_DLEN),
                    ==, 1);
    g_assert_true(qtest_readl(destination,
                              BCM2711_BSC0_BASE + BCM2835_I2C_S) &
                  BCM2835_I2C_S_TA);
    g_assert_cmphex(qtest_readl(destination,
                                BCM2711_BSC0_BASE + BCM2835_I2C_DIV),
                    ==, 0x9c4);
    g_assert_cmphex(qtest_readl(destination,
                                BCM2711_BSC0_BASE + BCM2835_I2C_DEL),
                    ==, 0x00400050);
    qtest_writel(destination, BCM2711_BSC0_BASE + BCM2835_I2C_FIFO, 0xad);
    status = qtest_readl(destination,
                         BCM2711_BSC0_BASE + BCM2835_I2C_S);
    g_assert_true(status & BCM2835_I2C_S_DONE);
    g_assert_false(status & (BCM2835_I2C_S_ERR | BCM2835_I2C_S_TA));

    qtest_writel(destination, BCM2711_BSC0_BASE + BCM2835_I2C_S,
                 BCM2835_I2C_S_DONE);
    qtest_writel(destination, BCM2711_BSC0_BASE + BCM2835_I2C_DLEN, 2);
    qtest_writel(destination, BCM2711_BSC0_BASE + BCM2835_I2C_C,
                 BCM2835_I2C_C_I2CEN | BCM2835_I2C_C_INTT |
                 BCM2835_I2C_C_ST);
    qtest_writel(destination, BCM2711_BSC0_BASE + BCM2835_I2C_FIFO, 0xaa);
    qtest_writel(destination, BCM2711_BSC0_BASE + BCM2835_I2C_FIFO, 3);
    qtest_writel(destination, BCM2711_BSC0_BASE + BCM2835_I2C_S,
                 BCM2835_I2C_S_DONE);
    qtest_writel(destination, BCM2711_BSC0_BASE + BCM2835_I2C_DLEN, 2);
    qtest_writel(destination, BCM2711_BSC0_BASE + BCM2835_I2C_C,
                 BCM2835_I2C_C_I2CEN | BCM2835_I2C_C_INTR |
                 BCM2835_I2C_C_INTD | BCM2835_I2C_C_READ |
                 BCM2835_I2C_C_ST);
    g_assert_cmphex(qtest_readl(destination,
                                BCM2711_BSC0_BASE + BCM2835_I2C_FIFO),
                    ==, 0xde);
    g_assert_cmphex(qtest_readl(destination,
                                BCM2711_BSC0_BASE + BCM2835_I2C_FIFO),
                    ==, 0xa0);

    qtest_quit(source);
    qtest_quit(destination);
    unlink(migration_socket);
    rmdir(migration_dir);
}
void test_bcm2711_i2c_clock_stretch_timeout(void)
{
    const char *command =
        "-M raspi4b -nic none "
        "-device tmp105,address=0x50,bus=i2c-bus.0,"
        "test-clock-stretch-after=0,test-clock-stretch-cycles=128 "
        "-device tmp105,address=0x50,bus=i2c-bus.1,"
        "test-clock-stretch-after=0,test-clock-stretch-cycles=128";
    const uint32_t divider = 250;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    QTestState *source = qtest_init(command);
    QTestState *destination;
    uint64_t serial_hz;
    uint64_t timeout_ns;
    uint64_t half_ns;
    uint32_t status;
    uint32_t vpu_ctl;

    qtest_irq_intercept_out_named(source, BCM2711_BSC0_QOM_PATH,
                                  "sysbus-irq");
    serial_hz = qom_path_get_uint32(
        source, BCM2711_BSC0_QOM_PATH, "core-clock-frequency") / divider;
    g_assert_cmpuint(serial_hz, >, 0);
    timeout_ns = DIV_ROUND_UP(
        64ULL * NANOSECONDS_PER_SECOND, serial_hz);
    half_ns = timeout_ns / 2;

    qtest_writel(source, BCM2711_BSC0_BASE + BCM2835_I2C_DIV, divider);
    qtest_writel(source, BCM2711_BSC0_BASE + BCM2835_I2C_CLKT, 64);
    qtest_writel(source, BCM2711_BSC0_BASE + BCM2835_I2C_A, 0x50);
    qtest_writel(source, BCM2711_BSC0_BASE + BCM2835_I2C_DLEN, 1);
    qtest_writel(source, BCM2711_BSC0_BASE + BCM2835_I2C_FIFO, 3);
    qtest_writel(source, BCM2711_BSC0_BASE + BCM2835_I2C_C,
                 BCM2835_I2C_C_I2CEN | BCM2835_I2C_C_INTD |
                 BCM2835_I2C_C_ST);
    status = qtest_readl(source, BCM2711_BSC0_BASE + BCM2835_I2C_S);
    g_assert_true(status & BCM2835_I2C_S_TA);
    g_assert_false(status & (BCM2835_I2C_S_DONE | BCM2835_I2C_S_CLKT));

    qtest_clock_step(source, half_ns);
    status = qtest_readl(source, BCM2711_BSC0_BASE + BCM2835_I2C_S);
    g_assert_true(status & BCM2835_I2C_S_TA);
    g_assert_false(qtest_get_irq(source, 0));

    destination = migrate_to_new_qtest(
        source, command, &migration_dir, &migration_socket);
    qtest_irq_intercept_out_named(destination, BCM2711_BSC0_QOM_PATH,
                                  "sysbus-irq");
    /*
     * Migration stores the remaining whole SCL-cycle count, so its restored
     * nanosecond deadline may round up by less than one serial period.
     */
    qtest_clock_step(destination, timeout_ns - 1);
    status = qtest_readl(destination,
                         BCM2711_BSC0_BASE + BCM2835_I2C_S);
    g_assert_true(status & BCM2835_I2C_S_TA);
    g_assert_false(status & (BCM2835_I2C_S_DONE | BCM2835_I2C_S_CLKT));
    g_assert_false(qtest_get_irq(destination, 0));

    qtest_clock_step(destination, DIV_ROUND_UP(
        NANOSECONDS_PER_SECOND, serial_hz) + 1);
    status = qtest_readl(destination,
                         BCM2711_BSC0_BASE + BCM2835_I2C_S);
    g_assert_cmphex(status & (BCM2835_I2C_S_TA |
                              BCM2835_I2C_S_DONE |
                              BCM2835_I2C_S_CLKT), ==,
                    BCM2835_I2C_S_DONE | BCM2835_I2C_S_CLKT);
    g_assert_true(qtest_get_irq(destination, 0));
    g_assert_cmphex(qtest_readl(destination,
                                BCM2711_BSC0_BASE + BCM2835_I2C_DLEN),
                    ==, 1);

    qtest_writel(destination, BCM2711_BSC0_BASE + BCM2835_I2C_S,
                 BCM2835_I2C_S_DONE | BCM2835_I2C_S_CLKT);
    g_assert_false(qtest_get_irq(destination, 0));
    g_assert_false(qtest_readl(destination,
                               BCM2711_BSC0_BASE + BCM2835_I2C_S) &
                   (BCM2835_I2C_S_DONE | BCM2835_I2C_S_CLKT));

    /*
     * A stretch shorter than CLKT resumes the same transfer instead of
     * reporting a timeout.
     */
    serial_hz = qom_path_get_uint32(
        destination, BCM2711_BSC1_QOM_PATH,
        "core-clock-frequency") / divider;
    timeout_ns = DIV_ROUND_UP(
        128ULL * NANOSECONDS_PER_SECOND, serial_hz);
    qtest_writel(destination, BCM2711_BSC1_BASE + BCM2835_I2C_DIV, divider);
    qtest_writel(destination, BCM2711_BSC1_BASE + BCM2835_I2C_CLKT, 256);
    qtest_writel(destination, BCM2711_BSC1_BASE + BCM2835_I2C_A, 0x50);
    qtest_writel(destination, BCM2711_BSC1_BASE + BCM2835_I2C_DLEN, 1);
    qtest_writel(destination, BCM2711_BSC1_BASE + BCM2835_I2C_FIFO, 3);
    qtest_writel(destination, BCM2711_BSC1_BASE + BCM2835_I2C_C,
                 BCM2835_I2C_C_I2CEN | BCM2835_I2C_C_INTD |
                 BCM2835_I2C_C_ST);
    vpu_ctl = qtest_readl(destination,
                          BCM2711_CPRMAN_BASE + CPRMAN_VPU_CTL);
    qtest_clock_step(destination, timeout_ns / 2);
    qtest_writel(destination, BCM2711_CPRMAN_BASE + CPRMAN_VPU_CTL,
                 CPRMAN_PASSWORD | (vpu_ctl & ~BIT(4)));
    qtest_clock_step(destination, timeout_ns * 2);
    g_assert_true(qtest_readl(destination,
                              BCM2711_BSC1_BASE + BCM2835_I2C_S) &
                  BCM2835_I2C_S_TA);
    qtest_writel(destination, BCM2711_CPRMAN_BASE + CPRMAN_VPU_CTL,
                 CPRMAN_PASSWORD | vpu_ctl);
    qtest_clock_step(destination, timeout_ns / 2 - 1);
    g_assert_true(qtest_readl(destination,
                              BCM2711_BSC1_BASE + BCM2835_I2C_S) &
                  BCM2835_I2C_S_TA);
    qtest_clock_step(destination, DIV_ROUND_UP(
        NANOSECONDS_PER_SECOND, serial_hz) + 1);
    status = qtest_readl(destination,
                         BCM2711_BSC1_BASE + BCM2835_I2C_S);
    g_assert_cmphex(status & (BCM2835_I2C_S_TA |
                              BCM2835_I2C_S_DONE |
                              BCM2835_I2C_S_CLKT), ==,
                    BCM2835_I2C_S_DONE);

    /* Reset cancels an armed stretch and restores the architectural state. */
    qtest_writel(destination, BCM2711_BSC0_BASE + BCM2835_I2C_S,
                 BCM2835_I2C_S_DONE | BCM2835_I2C_S_CLKT);
    qtest_writel(destination, BCM2711_BSC0_BASE + BCM2835_I2C_A, 0x50);
    qtest_writel(destination, BCM2711_BSC0_BASE + BCM2835_I2C_DLEN, 1);
    qtest_writel(destination, BCM2711_BSC0_BASE + BCM2835_I2C_FIFO, 3);
    qtest_writel(destination, BCM2711_BSC0_BASE + BCM2835_I2C_C,
                 BCM2835_I2C_C_I2CEN | BCM2835_I2C_C_INTD |
                 BCM2835_I2C_C_ST);
    g_assert_true(qtest_readl(destination,
                              BCM2711_BSC0_BASE + BCM2835_I2C_S) &
                  BCM2835_I2C_S_TA);
    qtest_system_reset(destination);
    qtest_clock_step(destination, timeout_ns * 2);
    g_assert_cmphex(qtest_readl(destination,
                                BCM2711_BSC0_BASE + BCM2835_I2C_S), ==,
                    BCM2835_I2C_S_TXD | BCM2835_I2C_S_TXE);

    qtest_quit(source);
    qtest_quit(destination);
    unlink(migration_socket);
    rmdir(migration_dir);
}
