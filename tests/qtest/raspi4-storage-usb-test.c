/*
 * Raspberry Pi 4 DWC2, eMMC/SD and CYW43455 device tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "raspi4-boot-test.h"

void test_cyw43455_sdio_transport(void)
{
    const char *command =
        "-M raspi-cm4 -nic none "
        "-device cyw43455-sdio,bus=sd-bus,id=wifi";
    const char *path = "/machine/peripheral/wifi";
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    QTestState *source = qtest_init(command);
    QTestState *destination;
    uint32_t response;

    qtest_writeb(source, BCM2711_EMMC_BASE + SDHC_SWRST, SDHC_RESET_ALL);
    qtest_writew(source, BCM2711_EMMC_BASE + SDHC_CLKCON,
                 SDHC_CLOCK_SDCLK_EN | SDHC_CLOCK_INT_EN);
    raspi4_sdhci_command(source, BCM2711_EMMC_BASE, 0, 0, 0, 0, 0);
    raspi4_sdhci_command(source, BCM2711_EMMC_BASE, 0, 0,
                         0x00ff8000, 0,
                         (5 << 8) | SDHC_CMD_RESPONSE);
    response = qtest_readl(source, BCM2711_EMMC_BASE + SDHC_RSPREG0);
    g_assert_true(response & BIT(31));
    g_assert_cmpuint(extract32(response, 28, 3), ==, 2);
    g_assert_cmphex(response & 0x00ffffff, ==, 0x00ff8000);

    raspi4_sdhci_command(source, BCM2711_EMMC_BASE, 0, 0, 0, 0,
                         (3 << 8) | SDHC_CMD_RESPONSE);
    response = qtest_readl(source, BCM2711_EMMC_BASE + SDHC_RSPREG0);
    g_assert_cmpuint(response >> 16, ==, 1);
    raspi4_sdhci_command(source, BCM2711_EMMC_BASE, 0, 0,
                         response & 0xffff0000, 0,
                         (7 << 8) | SDHC_CMD_RESPONSE);

    raspi4_sdhci_command(
        source, BCM2711_EMMC_BASE, 0, 0,
        cyw_sdio_cmd52_argument(false, false, 0, 0x00, 0), 0,
        (52 << 8) | SDHC_CMD_RESPONSE);
    g_assert_cmphex(qtest_readl(source,
                                BCM2711_EMMC_BASE + SDHC_RSPREG0) & 0xff,
                    ==, 0x32);

    raspi4_sdhci_command(
        source, BCM2711_EMMC_BASE, 0, 0,
        cyw_sdio_cmd52_argument(true, true, 0, 0x02, BIT(1) | BIT(2)), 0,
        (52 << 8) | SDHC_CMD_RESPONSE);
    g_assert_cmphex(qtest_readl(source,
                                BCM2711_EMMC_BASE + SDHC_RSPREG0) & 0xff,
                    ==, BIT(1) | BIT(2));
    raspi4_sdhci_command(
        source, BCM2711_EMMC_BASE, 0, 0,
        cyw_sdio_cmd52_argument(false, false, 0, 0x03, 0), 0,
        (52 << 8) | SDHC_CMD_RESPONSE);
    g_assert_cmphex(qtest_readl(source,
                                BCM2711_EMMC_BASE + SDHC_RSPREG0) & 0xff,
                    ==, BIT(1) | BIT(2));

    /* The common CIS reports Broadcom 0x02d0 / CYW43455 0xa9bf. */
    for (unsigned int i = 0; i < 6; i++) {
        static const uint8_t expected[] = {
            0x20, 0x04, 0xd0, 0x02, 0xbf, 0xa9,
        };

        raspi4_sdhci_command(
            source, BCM2711_EMMC_BASE, 0, 0,
            cyw_sdio_cmd52_argument(false, false, 0, 0x1000 + i, 0), 0,
            (52 << 8) | SDHC_CMD_RESPONSE);
        g_assert_cmphex(qtest_readl(
                            source,
                            BCM2711_EMMC_BASE + SDHC_RSPREG0) & 0xff,
                        ==, expected[i]);
    }

    raspi4_sdhci_command(
        source, BCM2711_EMMC_BASE, 0, 0,
        cyw_sdio_cmd52_argument(true, true, 1, 0x1000e, 0x28), 0,
        (52 << 8) | SDHC_CMD_RESPONSE);
    g_assert_cmphex(qtest_readl(source,
                                BCM2711_EMMC_BASE + SDHC_RSPREG0) & 0xc0,
                    ==, 0xc0);

    /*
     * Select the real CYW43455 TCM base. The window registers contain
     * backplane address bits 8..31 and the CMD53 address supplies bits 0..14.
     */
    raspi4_sdhci_command(
        source, BCM2711_EMMC_BASE, 0, 0,
        cyw_sdio_cmd52_argument(true, false, 1, 0x1000a, 0x80), 0,
        (52 << 8) | SDHC_CMD_RESPONSE);
    raspi4_sdhci_command(
        source, BCM2711_EMMC_BASE, 0, 0,
        cyw_sdio_cmd52_argument(true, false, 1, 0x1000b, 0x19), 0,
        (52 << 8) | SDHC_CMD_RESPONSE);
    raspi4_sdhci_command(
        source, BCM2711_EMMC_BASE, 0, 0,
        cyw_sdio_cmd52_argument(true, false, 1, 0x1000c, 0x00), 0,
        (52 << 8) | SDHC_CMD_RESPONSE);
    raspi4_sdhci_command(
        source, BCM2711_EMMC_BASE, 4, 1,
        cyw_sdio_cmd53_argument(true, 1, true, 0x20, 4),
        SDHC_TRNS_BLK_CNT_EN,
        (53 << 8) | SDHC_CMD_DATA_PRESENT | SDHC_CMD_RESPONSE);
    qtest_writel(source, BCM2711_EMMC_BASE + SDHC_BDATA, 0x44332211);

    destination = migrate_to_new_qtest(
        source, command, &migration_dir, &migration_socket);
    raspi4_sdhci_command(
        destination, BCM2711_EMMC_BASE, 4, 1,
        cyw_sdio_cmd53_argument(false, 1, true, 0x20, 4),
        SDHC_TRNS_BLK_CNT_EN | SDHC_TRNS_READ,
        (53 << 8) | SDHC_CMD_DATA_PRESENT | SDHC_CMD_RESPONSE);
    g_assert_cmphex(qtest_readl(destination,
                                BCM2711_EMMC_BASE + SDHC_BDATA),
                    ==, 0x44332211);

    /*
     * The unchanged driver downloads firmware in maximum-size function-1
     * multi-block requests: 511 blocks at the negotiated 64-byte size.
     */
    cyw_sdio_set_backplane_window(destination, 0x001a8000);
    raspi4_sdhci_command(
        destination, BCM2711_EMMC_BASE, 64, 511,
        cyw_sdio_cmd53_argument(true, 1, true, 0x8000, 511) | BIT(27),
        SDHC_TRNS_BLK_CNT_EN | SDHC_TRNS_MULTI,
        (53 << 8) | SDHC_CMD_DATA_PRESENT | SDHC_CMD_RESPONSE);
    for (unsigned int i = 0; i < 511 * 64 / sizeof(uint32_t); i++) {
        qtest_writel(destination, BCM2711_EMMC_BASE + SDHC_BDATA,
                     0xa5000000 | i);
    }
    g_assert_cmpuint(qom_path_get_uint64(
                         destination, path, "firmware-bytes"),
                     ==, 4 + 511 * 64);
    g_assert_cmphex(cyw_sdio_backplane_readl(destination, 0x001a8000),
                    ==, 0xa5000000);
    g_assert_cmphex(cyw_sdio_backplane_readl(
                        destination, 0x001a8000 + 511 * 64 - 4),
                    ==, 0xa5000000 | (511 * 64 / sizeof(uint32_t) - 1));

    /*
     * The production brcmfmac probe reads this exact signature at the
     * chipcommon enumeration base: BCM4345, revision 9, AXI backplane.
     */
    raspi4_sdhci_command(
        destination, BCM2711_EMMC_BASE, 0, 0,
        cyw_sdio_cmd52_argument(true, false, 1, 0x1000a, 0x00), 0,
        (52 << 8) | SDHC_CMD_RESPONSE);
    raspi4_sdhci_command(
        destination, BCM2711_EMMC_BASE, 0, 0,
        cyw_sdio_cmd52_argument(true, false, 1, 0x1000b, 0x00), 0,
        (52 << 8) | SDHC_CMD_RESPONSE);
    raspi4_sdhci_command(
        destination, BCM2711_EMMC_BASE, 0, 0,
        cyw_sdio_cmd52_argument(true, false, 1, 0x1000c, 0x18), 0,
        (52 << 8) | SDHC_CMD_RESPONSE);
    raspi4_sdhci_command(
        destination, BCM2711_EMMC_BASE, 4, 1,
        cyw_sdio_cmd53_argument(false, 1, true, 0x8000, 4),
        SDHC_TRNS_BLK_CNT_EN | SDHC_TRNS_READ,
        (53 << 8) | SDHC_CMD_DATA_PRESENT | SDHC_CMD_RESPONSE);
    g_assert_cmphex(qtest_readl(destination,
                                BCM2711_EMMC_BASE + SDHC_BDATA),
                    ==, 0x15294345);

    /* Function 2 supplies a deterministic transport loopback boundary. */
    raspi4_sdhci_command(
        destination, BCM2711_EMMC_BASE, 4, 1,
        cyw_sdio_cmd53_argument(true, 2, false, 0, 4),
        SDHC_TRNS_BLK_CNT_EN,
        (53 << 8) | SDHC_CMD_DATA_PRESENT | SDHC_CMD_RESPONSE);
    qtest_writel(destination, BCM2711_EMMC_BASE + SDHC_BDATA, 0xddccbbaa);
    raspi4_sdhci_command(
        destination, BCM2711_EMMC_BASE, 4, 1,
        cyw_sdio_cmd53_argument(false, 2, false, 0, 4),
        SDHC_TRNS_BLK_CNT_EN | SDHC_TRNS_READ,
        (53 << 8) | SDHC_CMD_DATA_PRESENT | SDHC_CMD_RESPONSE);
    g_assert_cmphex(qtest_readl(destination,
                                BCM2711_EMMC_BASE + SDHC_BDATA),
                    ==, 0xddccbbaa);

    qtest_quit(source);
    qtest_quit(destination);
    unlink(migration_socket);
    rmdir(migration_dir);
}
void test_cyw43455_sdio_erom(void)
{
    static const struct {
        uint16_t id;
        uint8_t rev;
        uint32_t base;
        uint32_t wrap;
    } expected[] = {
        { 0x800, 54, 0x18000000, 0x18100000 },
        { 0x829, 12, 0x18002000, 0x18102000 },
        { 0x812, 65, 0x18001000, 0x18101000 },
        { 0x83e, 1,  0x18003000, 0x18103000 },
    };
    QTestState *qts = qtest_init(
        "-M raspi-cm4 -nic none -device cyw43455-sdio,bus=sd-bus");
    uint32_t erom;

    cyw_sdio_init_card(qts);
    erom = cyw_sdio_backplane_readl(qts, 0x180000fc);
    g_assert_cmphex(erom, ==, 0x18110000);

    for (unsigned int i = 0; i < ARRAY_SIZE(expected); i++) {
        uint32_t component = cyw_sdio_backplane_readl(qts, erom);
        uint32_t info = cyw_sdio_backplane_readl(qts, erom + 4);
        uint32_t slave = cyw_sdio_backplane_readl(qts, erom + 8);
        uint32_t wrapper = cyw_sdio_backplane_readl(qts, erom + 12);

        g_assert_cmphex(component & 0xf, ==, 0x1);
        g_assert_cmphex(extract32(component, 8, 12), ==, expected[i].id);
        g_assert_cmphex(info & 0xf, ==, 0x1);
        g_assert_cmphex(extract32(info, 24, 8), ==, expected[i].rev);
        g_assert_cmphex(extract32(info, 19, 5), ==, 1);
        g_assert_cmphex(slave & 0xf, ==, 0x5);
        g_assert_cmphex(extract32(slave, 6, 2), ==, 0);
        g_assert_cmphex(slave & 0xfffff000, ==, expected[i].base);
        g_assert_cmphex(wrapper & 0xf, ==, 0x5);
        g_assert_cmphex(extract32(wrapper, 6, 2), ==, 2);
        g_assert_cmphex(wrapper & 0xfffff000, ==, expected[i].wrap);
        erom += 16;
    }
    g_assert_cmphex(cyw_sdio_backplane_readl(qts, erom), ==, 0xf);
    qtest_quit(qts);
}
void test_cyw43455_sdio_core_wrappers(void)
{
    const char *command =
        "-M raspi-cm4 -nic none -device cyw43455-sdio,bus=sd-bus";
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    QTestState *source = qtest_init(command);
    QTestState *destination;

    cyw_sdio_init_card(source);

    /* One 800 KiB ARMCR4 TCM bank: (0x63 + 1) * 8192 bytes. */
    g_assert_cmphex(cyw_sdio_backplane_readl(source, 0x18003004), ==, 1);
    cyw_sdio_backplane_writel(source, 0x18003040, 0);
    g_assert_cmphex(cyw_sdio_backplane_readl(source, 0x18003044),
                    ==, 0x63);

    for (unsigned int i = 0; i < 4; i++) {
        static const uint32_t wrappers[] = {
            0x18100000, 0x18102000, 0x18101000, 0x18103000,
        };

        g_assert_cmphex(cyw_sdio_backplane_readl(
                            source, wrappers[i] + 0x408),
                        ==, BIT(0));
        g_assert_cmphex(cyw_sdio_backplane_readl(
                            source, wrappers[i] + 0x800),
                        ==, 0);
    }

    cyw_sdio_backplane_writel(source, 0x18102000 + 0x408, 0x23);
    cyw_sdio_backplane_writel(source, 0x18102000 + 0x800, 1);
    destination = migrate_to_new_qtest(
        source, command, &migration_dir, &migration_socket);
    g_assert_cmphex(cyw_sdio_backplane_readl(
                        destination, 0x18102000 + 0x408),
                    ==, 0x23);
    g_assert_cmphex(cyw_sdio_backplane_readl(
                        destination, 0x18102000 + 0x800),
                    ==, 1);

    cyw_sdio_backplane_writel(destination, 0x18102000 + 0x800, 0);
    cyw_sdio_backplane_writel(destination, 0x18102000 + 0x408, BIT(0));
    g_assert_cmphex(cyw_sdio_backplane_readl(
                        destination, 0x18102000 + 0x800),
                    ==, 0);
    g_assert_cmphex(cyw_sdio_backplane_readl(
                        destination, 0x18102000 + 0x408),
                    ==, BIT(0));

    qtest_quit(source);
    qtest_quit(destination);
    unlink(migration_socket);
    rmdir(migration_dir);
}
void test_cyw43455_sdio_firmware_start(void)
{
    const char *command =
        "-M raspi-cm4 -nic none "
        "-device cyw43455-sdio,bus=sd-bus,id=wifi";
    const char *path = "/machine/peripheral/wifi";
    const uint32_t ram_base = 0x00198000;
    const uint32_t ram_size = 0x000c8000;
    const uint32_t reset_vector = 0xe1a00000;
    const uint32_t nvram_data[] = {
        0x313d6161, /* "aa=1" */
        0x00000000, /* NUL terminator and four-byte padding */
    };
    const uint16_t nvram_words = sizeof(nvram_data) / sizeof(uint32_t);
    const uint32_t nvram_token =
        ((uint32_t)(uint16_t)~nvram_words << 16) | nvram_words;
    const uint32_t nvram_base =
        ram_base + ram_size - sizeof(nvram_data) - sizeof(nvram_token);
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    QTestState *source = qtest_init(command);
    QTestState *destination;
    uint32_t shared_address;

    cyw_sdio_init_card(source);
    g_assert_false(qtest_qom_get_bool(
                       source, path, "firmware-started"));

    /* Linux writes the unchanged firmware at rambase and its vector at 0. */
    cyw_sdio_backplane_writel(source, ram_base, reset_vector);
    cyw_sdio_backplane_writel(source, 0, reset_vector);

    /* Releasing ARMCR4 without the brcmfmac NVRAM trailer fails closed. */
    cyw_sdio_backplane_writel(source, 0x18103000 + 0x800, 1);
    cyw_sdio_backplane_writel(source, 0x18103000 + 0x800, 0);
    g_assert_false(qtest_qom_get_bool(
                       source, path, "firmware-started"));
    g_assert_cmpuint(qom_path_get_uint64(
                         source, path, "start-failures"), ==, 1);

    /*
     * brcmfmac converts the unchanged text NVRAM to NUL-separated records,
     * pads to four bytes, and appends words/~words at the top of TCM.
     */
    for (unsigned int i = 0; i < ARRAY_SIZE(nvram_data); i++) {
        cyw_sdio_backplane_writel(
            source, nvram_base + i * sizeof(uint32_t), nvram_data[i]);
    }
    cyw_sdio_backplane_writel(
        source, ram_base + ram_size - sizeof(nvram_token), nvram_token);
    cyw_sdio_backplane_writel(source, 0x18103000 + 0x800, 1);
    cyw_sdio_backplane_writel(source, 0x18103000 + 0x800, 0);
    g_assert_true(qtest_qom_get_bool(
                      source, path, "firmware-started"));
    g_assert_cmphex(qom_path_get_uint32(
                        source, path, "reset-vector"), ==, reset_vector);
    g_assert_cmpuint(qom_path_get_uint32(
                         source, path, "nvram-size"),
                     ==, sizeof(nvram_data));
    shared_address = qom_path_get_uint32(source, path, "shared-address");

    g_assert_cmphex(cyw_sdio_backplane_readl(
                        source, ram_base + ram_size - sizeof(uint32_t)),
                    ==, shared_address);
    g_assert_cmphex(cyw_sdio_backplane_readl(source, shared_address),
                    ==, 3);

    /*
     * Function 2 enable publishes the normal brcmfmac firmware-ready
     * mailbox. The host mask and CCCR master/function-1 enables then
     * assert DAT1 and the SDHCI card-interrupt status.
     */
    raspi4_sdhci_command(
        source, BCM2711_EMMC_BASE, 0, 0,
        cyw_sdio_cmd52_argument(true, false, 0, 0x02,
                                BIT(1) | BIT(2)), 0,
        (52 << 8) | SDHC_CMD_RESPONSE);
    cyw_sdio_backplane_writel(source, 0x18002024, 0xf0);
    raspi4_sdhci_command(
        source, BCM2711_EMMC_BASE, 0, 0,
        cyw_sdio_cmd52_argument(true, false, 0, 0x04,
                                BIT(0) | BIT(1)), 0,
        (52 << 8) | SDHC_CMD_RESPONSE);
    /*
     * Linux may enable the controller's card-interrupt status after the
     * device has already pulled DAT1 low.  Enabling it must sample the
     * persistent SDIO IRQ level rather than waiting for another edge.
     */
    g_assert_cmphex(qtest_readw(
                        source, BCM2711_EMMC_BASE + SDHC_NORINTSTS) & 0x100,
                    ==, 0);
    qtest_writew(source, BCM2711_EMMC_BASE + SDHC_NORINTSTSEN, 0x100);
    qtest_writew(source, BCM2711_EMMC_BASE + SDHC_NORINTSIGEN, 0x100);
    g_assert_cmphex(cyw_sdio_backplane_readl(source, 0x18002020),
                    ==, BIT(7));
    g_assert_cmphex(cyw_sdio_backplane_readl(source, 0x1800204c),
                    ==, (4 << 16) | BIT(3));
    g_assert_cmphex(qtest_readw(
                        source, BCM2711_EMMC_BASE + SDHC_NORINTSTS) & 0x100,
                    ==, 0x100);

    destination = migrate_to_new_qtest(
        source, command, &migration_dir, &migration_socket);
    g_assert_true(qtest_qom_get_bool(
                      destination, path, "firmware-started"));
    g_assert_cmphex(qom_path_get_uint32(
                        destination, path, "reset-vector"),
                    ==, reset_vector);
    g_assert_cmpuint(qom_path_get_uint32(
                         destination, path, "nvram-size"),
                     ==, sizeof(nvram_data));
    g_assert_cmphex(qom_path_get_uint32(
                        destination, path, "shared-address"),
                    ==, shared_address);
    g_assert_cmphex(cyw_sdio_backplane_readl(destination, shared_address),
                    ==, 3);
    g_assert_cmpuint(qom_path_get_uint64(
                         destination, path, "start-failures"), ==, 1);
    g_assert_cmphex(cyw_sdio_backplane_readl(destination, 0x18002020),
                    ==, BIT(7));
    g_assert_cmphex(cyw_sdio_backplane_readl(destination, 0x1800204c),
                    ==, (4 << 16) | BIT(3));
    g_assert_cmphex(qtest_readw(
                        destination,
                        BCM2711_EMMC_BASE + SDHC_NORINTSTS) & 0x100,
                    ==, 0x100);

    cyw_sdio_backplane_writel(destination, 0x18002040, BIT(1));
    g_assert_cmphex(cyw_sdio_backplane_readl(destination, 0x18002020),
                    ==, 0);

    /* Any subsequent TCM mutation invalidates the running image. */
    cyw_sdio_backplane_writel(destination, ram_base + 4, 0);
    g_assert_false(qtest_qom_get_bool(
                       destination, path, "firmware-started"));

    qtest_quit(source);
    qtest_quit(destination);
    unlink(migration_socket);
    rmdir(migration_dir);
}
void test_cyw43455_sdio_bcdc_control(void)
{
    const char *command =
        "-M raspi-cm4 -nic none "
        "-device cyw43455-sdio,bus=sd-bus,id=wifi";
    const char *path = "/machine/peripheral/wifi";
    const uint32_t ram_base = 0x00198000;
    const uint32_t ram_size = 0x000c8000;
    const uint32_t reset_vector = 0xe1a00000;
    const uint32_t nvram_base = ram_base + ram_size - 12;
    uint8_t request[32] = { 0 };
    uint8_t response[32] = { 0 };
    uint8_t extended_request[48] = { 0 };
    uint8_t extended_response[40] = { 0 };
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    QTestState *source = qtest_init(command);
    QTestState *destination;

    cyw_sdio_init_card(source);
    cyw_sdio_backplane_writel(source, ram_base, reset_vector);
    cyw_sdio_backplane_writel(source, 0, reset_vector);
    cyw_sdio_backplane_writel(source, nvram_base, 0x313d6161);
    cyw_sdio_backplane_writel(source, nvram_base + 4, 0);
    cyw_sdio_backplane_writel(source, nvram_base + 8, 0xfffd0002);
    cyw_sdio_backplane_writel(source, 0x18103800, 1);
    cyw_sdio_backplane_writel(source, 0x18103800, 0);
    g_assert_true(qtest_qom_get_bool(
                      source, path, "firmware-started"));

    raspi4_sdhci_command(
        source, BCM2711_EMMC_BASE, 0, 0,
        cyw_sdio_cmd52_argument(true, false, 0, 0x02,
                                BIT(1) | BIT(2)), 0,
        (52 << 8) | SDHC_CMD_RESPONSE);
    cyw_sdio_backplane_writel(source, 0x18002024, 0xf0);
    raspi4_sdhci_command(
        source, BCM2711_EMMC_BASE, 0, 0,
        cyw_sdio_cmd52_argument(true, false, 0, 0x04,
                                BIT(0) | BIT(1)), 0,
        (52 << 8) | SDHC_CMD_RESPONSE);
    cyw_sdio_backplane_writel(source, 0x18002020, BIT(7));

    stw_le_p(request, sizeof(request));
    stw_le_p(request + 2, (uint16_t)~sizeof(request));
    stl_le_p(request + 4, 7 | (12U << 24));
    stl_le_p(request + 8, 0);
    stl_le_p(request + 12, 262); /* WLC_GET_VAR */
    stl_le_p(request + 16, 4);
    stl_le_p(request + 20, 0x1234U << 16);
    stl_le_p(request + 24, 0);
    memcpy(request + 28, "ver", 4);

    raspi4_sdhci_command(
        source, BCM2711_EMMC_BASE, sizeof(request), 1,
        cyw_sdio_cmd53_argument(true, 2, false, 0, sizeof(request)),
        SDHC_TRNS_BLK_CNT_EN,
        (53 << 8) | SDHC_CMD_DATA_PRESENT | SDHC_CMD_RESPONSE);
    for (unsigned int i = 0; i < sizeof(request); i += sizeof(uint32_t)) {
        qtest_writel(source, BCM2711_EMMC_BASE + SDHC_BDATA,
                     ldl_le_p(request + i));
    }
    g_assert_cmpuint(qom_path_get_uint64(
                         source, path, "control-requests"), ==, 1);
    g_assert_cmphex(cyw_sdio_backplane_readl(source, 0x18002020),
                    ==, BIT(6));

    destination = migrate_to_new_qtest(
        source, command, &migration_dir, &migration_socket);
    raspi4_sdhci_command(
        destination, BCM2711_EMMC_BASE, sizeof(response), 1,
        cyw_sdio_cmd53_argument(false, 2, false, 0, sizeof(response)),
        SDHC_TRNS_BLK_CNT_EN | SDHC_TRNS_READ,
        (53 << 8) | SDHC_CMD_DATA_PRESENT | SDHC_CMD_RESPONSE);
    for (unsigned int i = 0; i < sizeof(response); i += sizeof(uint32_t)) {
        stl_le_p(response + i, qtest_readl(
                     destination, BCM2711_EMMC_BASE + SDHC_BDATA));
    }
    g_assert_cmphex(lduw_le_p(response), ==, sizeof(response));
    g_assert_cmphex(lduw_le_p(response + 2),
                    ==, (uint16_t)~sizeof(response));
    g_assert_cmphex(extract32(ldl_le_p(response + 4), 8, 4), ==, 0);
    g_assert_cmphex(extract32(ldl_le_p(response + 4), 24, 8), ==, 12);
    g_assert_cmphex(ldl_le_p(response + 12), ==, 262);
    g_assert_cmphex(ldl_le_p(response + 20), ==, 0x1234U << 16);
    g_assert_cmpmem(response + 28, 4, "QEMU", 4);

    cyw_sdio_backplane_writel(destination, 0x18002020, BIT(6));
    request[2] ^= 1;
    raspi4_sdhci_command(
        destination, BCM2711_EMMC_BASE, sizeof(request), 1,
        cyw_sdio_cmd53_argument(true, 2, false, 0, sizeof(request)),
        SDHC_TRNS_BLK_CNT_EN,
        (53 << 8) | SDHC_CMD_DATA_PRESENT | SDHC_CMD_RESPONSE);
    for (unsigned int i = 0; i < sizeof(request); i += sizeof(uint32_t)) {
        qtest_writel(destination, BCM2711_EMMC_BASE + SDHC_BDATA,
                     ldl_le_p(request + i));
    }
    g_assert_cmpuint(qom_path_get_uint64(
                         destination, path, "control-rejections"), ==, 1);
    g_assert_cmphex(cyw_sdio_backplane_readl(destination, 0x18002020),
                    ==, 0);

    /*
     * brcmfmac enables txglom during preinit and then sends control requests
     * with an eight-byte hardware-extension header.  Firmware responses
     * return to the ordinary receive-header form.
     */
    stw_le_p(extended_request, sizeof(extended_request));
    stw_le_p(extended_request + 2,
             (uint16_t)~sizeof(extended_request));
    stl_le_p(extended_request + 4, 44 | BIT(24));
    stl_le_p(extended_request + 12, 8 | (20U << 24));
    stl_le_p(extended_request + 20, 140); /* WLC_GET_BANDLIST */
    stl_le_p(extended_request + 24, 12);
    stl_le_p(extended_request + 28, 0x5678U << 16);
    raspi4_sdhci_command(
        destination, BCM2711_EMMC_BASE, sizeof(extended_request), 1,
        cyw_sdio_cmd53_argument(true, 2, false, 0,
                                sizeof(extended_request)),
        SDHC_TRNS_BLK_CNT_EN,
        (53 << 8) | SDHC_CMD_DATA_PRESENT | SDHC_CMD_RESPONSE);
    for (unsigned int i = 0; i < sizeof(extended_request);
         i += sizeof(uint32_t)) {
        qtest_writel(destination, BCM2711_EMMC_BASE + SDHC_BDATA,
                     ldl_le_p(extended_request + i));
    }
    g_assert_cmpuint(qom_path_get_uint64(
                         destination, path, "control-requests"), ==, 2);
    raspi4_sdhci_command(
        destination, BCM2711_EMMC_BASE, sizeof(extended_response), 1,
        cyw_sdio_cmd53_argument(false, 2, false, 0,
                                sizeof(extended_response)),
        SDHC_TRNS_BLK_CNT_EN | SDHC_TRNS_READ,
        (53 << 8) | SDHC_CMD_DATA_PRESENT | SDHC_CMD_RESPONSE);
    for (unsigned int i = 0; i < sizeof(extended_response);
         i += sizeof(uint32_t)) {
        stl_le_p(extended_response + i, qtest_readl(
                     destination, BCM2711_EMMC_BASE + SDHC_BDATA));
    }
    g_assert_cmphex(lduw_le_p(extended_response),
                    ==, sizeof(extended_response));
    g_assert_cmphex(extract32(
                        ldl_le_p(extended_response + 4), 24, 8),
                    ==, 12);
    g_assert_cmphex(ldl_le_p(extended_response + 12), ==, 140);
    g_assert_cmphex(ldl_le_p(extended_response + 28), ==, 1);
    g_assert_cmphex(ldl_le_p(extended_response + 32), ==, 2);

    qtest_quit(source);
    qtest_quit(destination);
    unlink(migration_socket);
    rmdir(migration_dir);
}
void test_cyw43455_sdio_bcdc_packets(void)
{
    enum { PRESSURE_PACKETS = 32 };
    uint8_t packet[60];
    uint8_t frame[76] = { 0 };
    uint8_t received[76] = { 0 };
    uint32_t wire_size;
    uint32_t net_size;
    g_autofree char *command = NULL;
    struct timeval timeout = { .tv_sec = 2 };
    QTestState *qts;
    int sockets[2];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    g_assert_cmpint(setsockopt(sockets[0], SOL_SOCKET, SO_RCVTIMEO,
                              &timeout, sizeof(timeout)), ==, 0);
    command = g_strdup_printf(
        "-M raspi-cm4 -netdev socket,fd=%d,id=wifi-net "
        "-device cyw43455-sdio,bus=sd-bus,id=wifi,netdev=wifi-net",
        sockets[1]);
    qts = qtest_init(command);
    cyw_sdio_init_card(qts);
    cyw_sdio_backplane_writel(qts, 0x00198000, 0xe1a00000);
    cyw_sdio_backplane_writel(qts, 0, 0xe1a00000);
    cyw_sdio_backplane_writel(qts, 0x0025fff4, 0x313d6161);
    cyw_sdio_backplane_writel(qts, 0x0025fff8, 0);
    cyw_sdio_backplane_writel(qts, 0x0025fffc, 0xfffd0002);
    cyw_sdio_backplane_writel(qts, 0x18103800, 1);
    cyw_sdio_backplane_writel(qts, 0x18103800, 0);
    raspi4_sdhci_command(
        qts, BCM2711_EMMC_BASE, 0, 0,
        cyw_sdio_cmd52_argument(true, false, 0, 0x02,
                                BIT(1) | BIT(2)), 0,
        (52 << 8) | SDHC_CMD_RESPONSE);

    for (unsigned int packet_index = 0;
         packet_index < PRESSURE_PACKETS; packet_index++) {
        for (unsigned int i = 0; i < sizeof(packet); i++) {
            packet[i] = i ^ 0xa5 ^ packet_index;
        }
        stw_le_p(frame, sizeof(frame));
        stw_le_p(frame + 2, (uint16_t)~sizeof(frame));
        stl_le_p(frame + 4,
                 packet_index | (2U << 8) | (12U << 24));
        frame[12] = 2 << 4;
        memcpy(frame + 16, packet, sizeof(packet));
        raspi4_sdhci_command(
            qts, BCM2711_EMMC_BASE, sizeof(frame), 1,
            cyw_sdio_cmd53_argument(true, 2, false, 0, sizeof(frame)),
            SDHC_TRNS_BLK_CNT_EN,
            (53 << 8) | SDHC_CMD_DATA_PRESENT | SDHC_CMD_RESPONSE);
        for (unsigned int i = 0; i < sizeof(frame);
             i += sizeof(uint32_t)) {
            qtest_writel(qts, BCM2711_EMMC_BASE + SDHC_BDATA,
                         ldl_le_p(frame + i));
        }
        g_assert_cmpint(recv(sockets[0], &wire_size, sizeof(wire_size),
                             MSG_WAITALL),
                        ==, sizeof(wire_size));
        g_assert_cmpuint(ntohl(wire_size), ==, sizeof(packet));
        g_assert_cmpint(recv(sockets[0], received, sizeof(packet),
                             MSG_WAITALL), ==, sizeof(packet));
        g_assert_cmpmem(received, sizeof(packet), packet, sizeof(packet));
    }
    g_assert_cmpuint(qom_path_get_uint64(
                         qts, "/machine/peripheral/wifi",
                         "data-tx-packets"), ==, PRESSURE_PACKETS);

    net_size = htonl(sizeof(packet));
    for (unsigned int packet_index = 0;
         packet_index < PRESSURE_PACKETS; packet_index++) {
        for (unsigned int i = 0; i < sizeof(packet); i++) {
            packet[i] = i ^ 0x5a ^ packet_index;
        }
        g_assert_cmpint(send(sockets[0], &net_size, sizeof(net_size), 0),
                        ==, sizeof(net_size));
        g_assert_cmpint(send(sockets[0], packet, sizeof(packet), 0),
                        ==, sizeof(packet));
    }
    for (unsigned int i = 0; i < 100; i++) {
        if (qom_path_get_uint64(qts, "/machine/peripheral/wifi",
                                "data-rx-packets") == 1) {
            break;
        }
        g_usleep(1000);
    }
    g_assert_cmpuint(qom_path_get_uint64(
                         qts, "/machine/peripheral/wifi",
                         "data-rx-packets"), ==, 1);
    for (unsigned int packet_index = 0;
         packet_index < PRESSURE_PACKETS; packet_index++) {
        raspi4_sdhci_command(
            qts, BCM2711_EMMC_BASE, sizeof(received), 1,
            cyw_sdio_cmd53_argument(false, 2, false, 0, sizeof(received)),
            SDHC_TRNS_BLK_CNT_EN | SDHC_TRNS_READ,
            (53 << 8) | SDHC_CMD_DATA_PRESENT | SDHC_CMD_RESPONSE);
        for (unsigned int i = 0; i < sizeof(received);
             i += sizeof(uint32_t)) {
            stl_le_p(received + i, qtest_readl(
                         qts, BCM2711_EMMC_BASE + SDHC_BDATA));
        }
        g_assert_cmphex(lduw_le_p(received), ==, sizeof(received));
        g_assert_cmphex(extract32(ldl_le_p(received + 4), 8, 4), ==, 2);
        g_assert_cmphex(received[12], ==, 2 << 4);
        for (unsigned int i = 0; i < sizeof(packet); i++) {
            packet[i] = i ^ 0x5a ^ packet_index;
        }
        g_assert_cmpmem(received + 16, sizeof(packet),
                        packet, sizeof(packet));
        if (packet_index + 1 < PRESSURE_PACKETS) {
            for (unsigned int i = 0; i < 100; i++) {
                if (qom_path_get_uint64(
                        qts, "/machine/peripheral/wifi",
                        "data-rx-packets") == packet_index + 2) {
                    break;
                }
                g_usleep(1000);
            }
        }
    }
    g_assert_cmpuint(qom_path_get_uint64(
                         qts, "/machine/peripheral/wifi",
                         "data-rx-packets"), ==, PRESSURE_PACKETS);

    qtest_quit(qts);
    close(sockets[0]);
    close(sockets[1]);
}
void test_cyw43455_sdio_packet_migration(void)
{
    static const char path[] = "/machine/peripheral/wifi";
    uint8_t packet[60];
    uint8_t received[60];
    uint32_t net_size = htonl(sizeof(packet));
    struct timeval timeout = { .tv_sec = 2 };
    g_autofree char *source_command = NULL;
    g_autofree char *destination_command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    int source_sockets[2];
    int destination_sockets[2];
    QTestState *source;
    QTestState *destination;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, source_sockets),
                    ==, 0);
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, destination_sockets),
                    ==, 0);
    g_assert_cmpint(setsockopt(destination_sockets[0], SOL_SOCKET,
                              SO_RCVTIMEO, &timeout, sizeof(timeout)), ==, 0);
    source_command = g_strdup_printf(
        "-M raspi-cm4 -netdev socket,fd=%d,id=wifi-net "
        "-device cyw43455-sdio,bus=sd-bus,id=wifi,netdev=wifi-net,"
        "packet-drop-direction=1,packet-drop-after=0,packet-drop-count=1",
        source_sockets[1]);
    destination_command = g_strdup_printf(
        "-M raspi-cm4 -netdev socket,fd=%d,id=wifi-net "
        "-device cyw43455-sdio,bus=sd-bus,id=wifi,netdev=wifi-net,"
        "packet-drop-direction=1,packet-drop-after=0,packet-drop-count=1",
        destination_sockets[1]);
    source = qtest_init(source_command);
    close(source_sockets[1]);
    cyw_sdio_start_firmware(source);

    for (unsigned int i = 0; i < sizeof(packet); i++) {
        packet[i] = i ^ 0x6d;
    }
    cyw_sdio_send_guest_packet(source, packet, sizeof(packet), 0);
    g_assert_cmpuint(qom_path_get_uint64(
                         source, path, "packet-drop-packets-seen"), ==, 1);
    g_assert_cmpuint(qom_path_get_uint64(
                         source, path, "packet-drops-injected"), ==, 1);
    g_assert_cmpint(send(source_sockets[0], &net_size, sizeof(net_size), 0),
                    ==, sizeof(net_size));
    g_assert_cmpint(send(source_sockets[0], packet, sizeof(packet), 0),
                    ==, sizeof(packet));
    for (unsigned int i = 0; i < 100; i++) {
        if (qom_path_get_uint64(source, path, "data-rx-packets") == 1) {
            break;
        }
        g_usleep(1000);
    }
    g_assert_cmpuint(qom_path_get_uint64(source, path, "data-rx-packets"),
                     ==, 1);

    destination = migrate_to_new_qtest_commands(
        source, destination_command, &migration_dir, &migration_socket);
    close(destination_sockets[1]);
    g_assert_cmpuint(qom_path_get_uint64(
                         destination, path, "packet-drop-packets-seen"), ==, 1);
    g_assert_cmpuint(qom_path_get_uint64(
                         destination, path, "packet-drops-injected"), ==, 1);
    cyw_sdio_read_guest_packet(destination, received, sizeof(received));
    g_assert_cmpmem(received, sizeof(received), packet, sizeof(packet));

    for (unsigned int i = 0; i < sizeof(packet); i++) {
        packet[i] = i ^ 0xb2;
    }
    cyw_sdio_send_guest_packet(destination, packet, sizeof(packet), 1);
    for (unsigned int attempt = 0; attempt < 4; attempt++) {
        g_assert_cmpint(recv(destination_sockets[0], &net_size,
                             sizeof(net_size), MSG_WAITALL),
                        ==, sizeof(net_size));
        g_assert_cmpuint(ntohl(net_size), <=, sizeof(received));
        g_assert_cmpint(recv(destination_sockets[0], received,
                             ntohl(net_size), MSG_WAITALL),
                        ==, ntohl(net_size));
        if (ntohl(net_size) == sizeof(packet) &&
            !memcmp(received, packet, sizeof(packet))) {
            break;
        }
    }
    g_assert_cmpmem(received, sizeof(received), packet, sizeof(packet));
    g_assert_cmpuint(qom_path_get_uint64(
                         destination, path, "packet-drop-packets-seen"), ==, 2);
    g_assert_cmpint(send(destination_sockets[0], &net_size,
                         sizeof(net_size), 0), ==, sizeof(net_size));
    g_assert_cmpint(send(destination_sockets[0], packet, sizeof(packet), 0),
                    ==, sizeof(packet));
    for (unsigned int i = 0; i < 100; i++) {
        if (qom_path_get_uint64(destination, path,
                                "data-rx-packets") == 2) {
            break;
        }
        g_usleep(1000);
    }
    g_assert_cmpuint(qom_path_get_uint64(destination, path,
                                         "data-rx-packets"), ==, 2);
    cyw_sdio_read_guest_packet(destination, received, sizeof(received));
    g_assert_cmpmem(received, sizeof(received), packet, sizeof(packet));

    qtest_quit(source);
    qtest_quit(destination);
    close(source_sockets[0]);
    close(destination_sockets[0]);
    unlink(migration_socket);
    rmdir(migration_dir);
}
void test_cyw43455_sdio_packet_loss_recovery(void)
{
    static const char path[] = "/machine/peripheral/wifi";
    uint8_t packet[60];
    uint8_t received[60];
    uint32_t wire_size;
    uint32_t net_size = htonl(sizeof(packet));
    struct pollfd pfd;
    g_autofree char *command = NULL;
    QTestState *qts;
    int sockets[2];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    command = g_strdup_printf(
        "-M raspi-cm4 -netdev socket,fd=%d,id=wifi-net "
        "-device cyw43455-sdio,bus=sd-bus,id=wifi,netdev=wifi-net,"
        "packet-drop-direction=3,packet-drop-after=1,packet-drop-count=2",
        sockets[1]);
    qts = qtest_init(command);
    close(sockets[1]);
    cyw_sdio_start_firmware(qts);
    for (unsigned int i = 0; i < sizeof(packet); i++) {
        packet[i] = i ^ 0x31;
    }

    /* The first selected packet passes. */
    cyw_sdio_send_guest_packet(qts, packet, sizeof(packet), 0);
    g_assert_cmpint(recv(sockets[0], &wire_size, sizeof(wire_size),
                         MSG_WAITALL), ==, sizeof(wire_size));
    g_assert_cmpuint(ntohl(wire_size), ==, sizeof(packet));
    g_assert_cmpint(recv(sockets[0], received, sizeof(received), MSG_WAITALL),
                    ==, sizeof(received));
    g_assert_cmpmem(received, sizeof(received), packet, sizeof(packet));

    /* The configured bounded window drops one RX and one TX packet. */
    g_assert_cmpint(send(sockets[0], &net_size, sizeof(net_size), 0),
                    ==, sizeof(net_size));
    g_assert_cmpint(send(sockets[0], packet, sizeof(packet), 0),
                    ==, sizeof(packet));
    for (unsigned int i = 0; i < 100; i++) {
        if (qom_path_get_uint64(qts, path, "packet-drops-injected") == 1) {
            break;
        }
        g_usleep(1000);
    }
    g_assert_cmpuint(qom_path_get_uint64(qts, path,
                                         "packet-drops-injected"), ==, 1);
    g_assert_cmpuint(qom_path_get_uint64(qts, path, "data-rx-packets"),
                     ==, 0);
    cyw_sdio_send_guest_packet(qts, packet, sizeof(packet), 1);
    g_assert_cmpuint(qom_path_get_uint64(qts, path,
                                         "packet-drops-injected"), ==, 2);
    pfd = (struct pollfd) { .fd = sockets[0], .events = POLLIN };
    g_assert_cmpint(poll(&pfd, 1, 10), ==, 0);

    /* Both directions recover automatically after the bounded loss. */
    cyw_sdio_send_guest_packet(qts, packet, sizeof(packet), 2);
    g_assert_cmpint(recv(sockets[0], &wire_size, sizeof(wire_size),
                         MSG_WAITALL), ==, sizeof(wire_size));
    g_assert_cmpuint(ntohl(wire_size), ==, sizeof(packet));
    g_assert_cmpint(recv(sockets[0], received, sizeof(received), MSG_WAITALL),
                    ==, sizeof(received));
    g_assert_cmpmem(received, sizeof(received), packet, sizeof(packet));
    g_assert_cmpint(send(sockets[0], &net_size, sizeof(net_size), 0),
                    ==, sizeof(net_size));
    g_assert_cmpint(send(sockets[0], packet, sizeof(packet), 0),
                    ==, sizeof(packet));
    for (unsigned int i = 0; i < 100; i++) {
        if (qom_path_get_uint64(qts, path, "data-rx-packets") == 1) {
            break;
        }
        g_usleep(1000);
    }
    cyw_sdio_read_guest_packet(qts, received, sizeof(received));
    g_assert_cmpmem(received, sizeof(received), packet, sizeof(packet));
    g_assert_cmpuint(qom_path_get_uint64(
                         qts, path, "packet-drop-packets-seen"), ==, 5);
    g_assert_cmpuint(qom_path_get_uint64(
                         qts, path, "packet-drops-injected"), ==, 2);

    qtest_quit(qts);
    close(sockets[0]);
}
void test_cyw43455_bluetooth_hci(void)
{
    static const char uart_path[] = "/machine/soc/peripherals/uart0";
    static const char wifi_path[] = "/machine/peripheral/wifi";
    static const uint8_t command_head[] = { 0x01, 0x09 };
    static const uint8_t command_tail[] = { 0x10, 0x00 };
    static const uint8_t expected_event[] = {
        0x04, 0x0e, 0x0a, 0x01, 0x09, 0x10, 0x00,
        0x55, 0x44, 0x33, 0x22, 0x11, 0x02,
    };
    static const uint8_t guest_acl[] = {
        0x02, 0x01, 0x20, 0x04, 0x00, 0xde, 0xad, 0xbe, 0xef,
    };
    static const uint8_t host_acl[] = {
        0x02, 0x02, 0x20, 0x03, 0x00, 0x12, 0x34, 0x56,
    };
    static const uint8_t wifi_packet[60] = { 0x45, 0x00, 0x00, 0x3c };
    uint8_t received[sizeof(expected_event)];
    g_autofree char *source_command = NULL;
    g_autofree char *destination_command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    struct timeval timeout = { .tv_sec = 2 };
    int source_sockets[2];
    int destination_sockets[2];
    QTestState *source;
    QTestState *destination;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, source_sockets),
                    ==, 0);
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, destination_sockets),
                    ==, 0);
    g_assert_cmpint(setsockopt(source_sockets[0], SOL_SOCKET, SO_RCVTIMEO,
                              &timeout, sizeof(timeout)), ==, 0);
    g_assert_cmpint(setsockopt(destination_sockets[0], SOL_SOCKET,
                              SO_RCVTIMEO, &timeout, sizeof(timeout)), ==, 0);
    source_command = g_strdup_printf(
        "-M raspi-cm4,wireless-model=on -nic none "
        "-global cyw43455-sdio.packet-drop-direction=1 "
        "-global cyw43455-sdio.packet-drop-after=0 "
        "-global cyw43455-sdio.packet-drop-count=1 "
        "-chardev socket,id=bt,fd=%d -serial chardev:bt",
        source_sockets[1]);
    destination_command = g_strdup_printf(
        "-M raspi-cm4,wireless-model=on -nic none "
        "-global cyw43455-sdio.packet-drop-direction=1 "
        "-global cyw43455-sdio.packet-drop-after=0 "
        "-global cyw43455-sdio.packet-drop-count=1 "
        "-chardev socket,id=bt,fd=%d -serial chardev:bt",
        destination_sockets[1]);
    source = qtest_init(source_command);
    close(source_sockets[1]);
    raspi4_hci_uart_configure(source);
    cyw_sdio_start_firmware(source);
    cyw_sdio_send_guest_packet(
        source, wifi_packet, sizeof(wifi_packet), 0);
    g_assert_cmpuint(qom_path_get_uint64(
                         source, wifi_path, "packet-drops-injected"), ==, 1);
    raspi4_hci_uart_write(source, command_head, sizeof(command_head));
    g_assert_cmpint(recv(source_sockets[0], received, sizeof(command_head),
                         MSG_WAITALL), ==, sizeof(command_head));
    g_assert_cmpmem(received, sizeof(command_head),
                    command_head, sizeof(command_head));

    destination = migrate_to_new_qtest_commands(
        source, destination_command, &migration_dir, &migration_socket);
    close(destination_sockets[1]);
    g_assert_cmpuint(qom_path_get_uint64(
                         destination, wifi_path,
                         "packet-drops-injected"), ==, 1);
    cyw_sdio_send_guest_packet(
        destination, wifi_packet, sizeof(wifi_packet), 1);
    g_assert_cmpuint(qom_path_get_uint64(
                         destination, wifi_path, "data-tx-packets"), ==, 1);
    raspi4_hci_uart_write(destination, command_tail, sizeof(command_tail));
    g_assert_cmpint(recv(destination_sockets[0], received,
                         sizeof(command_tail), MSG_WAITALL),
                    ==, sizeof(command_tail));
    g_assert_cmpmem(received, sizeof(command_tail),
                    command_tail, sizeof(command_tail));
    raspi4_hci_uart_read(destination, received, sizeof(expected_event));
    g_assert_cmpmem(received, sizeof(expected_event),
                    expected_event, sizeof(expected_event));
    g_assert_cmpuint(qom_path_get_uint64(
                         destination, uart_path, "hci-commands"), ==, 1);
    g_assert_cmpuint(qom_path_get_uint64(
                         destination, uart_path, "hci-events"), ==, 1);

    raspi4_hci_uart_write(destination, guest_acl, sizeof(guest_acl));
    g_assert_cmpint(recv(destination_sockets[0], received,
                         sizeof(guest_acl), MSG_WAITALL),
                    ==, sizeof(guest_acl));
    g_assert_cmpmem(received, sizeof(guest_acl), guest_acl, sizeof(guest_acl));
    g_assert_cmpint(send(destination_sockets[0], host_acl, sizeof(host_acl), 0),
                    ==, sizeof(host_acl));
    raspi4_hci_uart_read(destination, received, sizeof(host_acl));
    g_assert_cmpmem(received, sizeof(host_acl), host_acl, sizeof(host_acl));
    g_assert_cmpuint(qom_path_get_uint64(
                         destination, uart_path, "hci-acl-tx-packets"), ==, 1);
    g_assert_cmpuint(qom_path_get_uint64(
                         destination, uart_path, "hci-acl-rx-packets"), ==, 1);

    qtest_system_reset(destination);
    g_assert_cmpuint(qom_path_get_uint64(
                         destination, uart_path, "hci-commands"), ==, 0);
    g_assert_cmpuint(qom_path_get_uint64(
                         destination, uart_path, "hci-acl-tx-packets"), ==, 0);

    qtest_quit(source);
    qtest_quit(destination);
    close(source_sockets[0]);
    close(destination_sockets[0]);
    unlink(migration_socket);
    rmdir(migration_dir);
}
void test_cyw43455_sdio_onboard_topology(void)
{
    static const char * const machines[] = {
        "raspi4b",
        "raspi-cm4",
    };
    g_autofree char *sd_path = NULL;
    g_autofree char *sd_command = NULL;
    int sd_fd = g_file_open_tmp("raspi4-emmc2-sd-XXXXXX",
                                &sd_path, NULL);
    QTestState *sd_qts;

    g_assert_cmpint(sd_fd, >=, 0);
    g_assert_cmpint(ftruncate(sd_fd, 1 * MiB), ==, 0);
    close(sd_fd);
    sd_command = g_strdup_printf(
        "-M raspi4b -drive if=sd,format=raw,file=%s", sd_path);
    sd_qts = qtest_init(sd_command);
    assert_bus_card_type(
        sd_qts, "/machine/soc/peripherals/emmc2/sd-bus", "sd-card");
    qtest_quit(sd_qts);
    unlink(sd_path);

    for (unsigned int i = 0; i < ARRAY_SIZE(machines); i++) {
        int sockets[2];
        g_autofree char *command = NULL;

        g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
        command = g_strdup_printf(
            "-M %s,wireless-model=on,wireless-netdev=wifi-net "
            "-netdev socket,fd=%d,id=wifi-net -nic none",
            machines[i], sockets[1]);
        QTestState *qts = qtest_init(command);
        g_autofree char *status = qom_get_string(qts, "wireless-status");
        g_autofree char *exclusions = qom_get_string(
            qts, "peripheral-exclusions");

        g_assert_true(qtest_qom_get_bool(
                          qts, "/machine", "wireless-model"));
        g_assert_cmpstr(status, ==, "modeled-sdio-bcdc-uart-hci");
        g_assert_cmpstr(exclusions, ==,
                        "brcm,brcm2711-dvp");
        assert_bus_card_type(
            qts, "/machine/soc/peripherals/emmc2/sd-bus",
            !strcmp(machines[i], "raspi4b") ? "sd-card" : "emmc");
        cyw_sdio_init_card(qts);
        g_assert_cmphex(cyw_sdio_backplane_readl(qts, 0x18000000),
                        ==, 0x15294345);
        qtest_quit(qts);
        close(sockets[0]);
        close(sockets[1]);
    }
}
void test_cyw43455_sdio_final_dtb(void)
{
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    QTestState *qts = start_with_complex_firmware_config_variant(
        NULL, TEST_EXPORT_VALID, NULL, 0, true,
        &eeprom_path, &sd_path, NULL);
    uint64_t address = qom_get_uint64(
        qts, "arm-handoff-device-tree-address");
    uint64_t size = qom_get_uint64(qts, "arm-handoff-device-tree-size");
    g_autofree uint8_t *fdt = g_malloc(size);
    g_autofree char *status = qom_get_string(qts, "wireless-status");
    g_autofree char *source = qom_get_string(qts, "boot-source");
    int node;
    const char *node_status;

    g_assert_cmpstr(status, ==, "modeled-sdio-bcdc-uart-hci");
    g_assert_cmpstr(source, ==, "sd-card");
    qtest_memread(qts, address, fdt, size);
    g_assert_cmpint(fdt_check_header(fdt), ==, 0);
    node = fdt_node_offset_by_compatible(fdt, -1, "brcm,bcm2835-mmc");
    g_assert_cmpint(node, >=, 0);
    node_status = fdt_getprop(fdt, node, "status", NULL);
    g_assert_true(!node_status || strcmp(node_status, "disabled"));
    node = fdt_node_offset_by_compatible(fdt, -1, "brcm,bcm43438-bt");
    g_assert_cmpint(node, >=, 0);
    node_status = fdt_getprop(fdt, node, "status", NULL);
    g_assert_true(!node_status || strcmp(node_status, "disabled"));

    qtest_quit(qts);
    unlink(eeprom_path);
    unlink(sd_path);
}
void test_cyw43455_sdio_exact_firmware_download(void)
{
    const char *firmware_path = g_getenv("QTEST_CYW43455_FIRMWARE");
    const char *expected_sha256 =
        g_getenv("QTEST_CYW43455_FIRMWARE_SHA256");
    g_autofree char *firmware = NULL;
    g_autofree char *readback = NULL;
    g_autofree char *actual_sha256 = NULL;
    gsize firmware_size;
    QTestState *qts;
    uint32_t response;

    if (!firmware_path) {
        g_test_skip("set QTEST_CYW43455_FIRMWARE and its SHA-256");
        return;
    }
    g_assert_nonnull(expected_sha256);
    g_assert_cmpuint(strlen(expected_sha256), ==, 64);
    g_assert_true(g_file_get_contents(firmware_path, &firmware,
                                      &firmware_size, NULL));
    g_assert_cmpuint(firmware_size, >, 0);
    g_assert_cmpuint(firmware_size, <=, 0xc8000);
    actual_sha256 = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, (const guchar *)firmware, firmware_size);
    g_assert_cmpstr(actual_sha256, ==, expected_sha256);
    readback = g_malloc0(firmware_size);

    qts = qtest_init(
        "-M raspi-cm4 -nic none -device cyw43455-sdio,bus=sd-bus");
    qtest_writeb(qts, BCM2711_EMMC_BASE + SDHC_SWRST, SDHC_RESET_ALL);
    qtest_writew(qts, BCM2711_EMMC_BASE + SDHC_CLKCON,
                 SDHC_CLOCK_SDCLK_EN | SDHC_CLOCK_INT_EN);
    raspi4_sdhci_command(qts, BCM2711_EMMC_BASE, 0, 0,
                         0x00ff8000, 0,
                         (5 << 8) | SDHC_CMD_RESPONSE);
    raspi4_sdhci_command(qts, BCM2711_EMMC_BASE, 0, 0, 0, 0,
                         (3 << 8) | SDHC_CMD_RESPONSE);
    response = qtest_readl(qts, BCM2711_EMMC_BASE + SDHC_RSPREG0);
    raspi4_sdhci_command(qts, BCM2711_EMMC_BASE, 0, 0,
                         response & 0xffff0000, 0,
                         (7 << 8) | SDHC_CMD_RESPONSE);
    raspi4_sdhci_command(
        qts, BCM2711_EMMC_BASE, 0, 0,
        cyw_sdio_cmd52_argument(true, false, 0, 0x02, BIT(1)), 0,
        (52 << 8) | SDHC_CMD_RESPONSE);

    for (gsize offset = 0; offset < firmware_size; ) {
        const uint32_t address = 0x198000 + offset;
        const size_t chunk = MIN((gsize)512, firmware_size - offset);
        const size_t wire_size = ROUND_UP(chunk, sizeof(uint32_t));

        cyw_sdio_set_backplane_window(qts, address);
        raspi4_sdhci_command(
            qts, BCM2711_EMMC_BASE, wire_size, 1,
            cyw_sdio_cmd53_argument(true, 1, true,
                                    0x8000 | (address & 0x7fff),
                                    wire_size == 512 ? 0 : wire_size),
            SDHC_TRNS_BLK_CNT_EN,
            (53 << 8) | SDHC_CMD_DATA_PRESENT | SDHC_CMD_RESPONSE);
        for (size_t i = 0; i < wire_size; i += sizeof(uint32_t)) {
            uint32_t word = 0;
            const size_t valid = MIN(sizeof(uint32_t), chunk - i);

            memcpy(&word, firmware + offset + i, valid);
            qtest_writel(qts, BCM2711_EMMC_BASE + SDHC_BDATA, word);
        }
        offset += chunk;
    }

    for (gsize offset = 0; offset < firmware_size; ) {
        const uint32_t address = 0x198000 + offset;
        const size_t chunk = MIN((gsize)512, firmware_size - offset);
        const size_t wire_size = ROUND_UP(chunk, sizeof(uint32_t));

        cyw_sdio_set_backplane_window(qts, address);
        raspi4_sdhci_command(
            qts, BCM2711_EMMC_BASE, wire_size, 1,
            cyw_sdio_cmd53_argument(false, 1, true,
                                    0x8000 | (address & 0x7fff),
                                    wire_size == 512 ? 0 : wire_size),
            SDHC_TRNS_BLK_CNT_EN | SDHC_TRNS_READ,
            (53 << 8) | SDHC_CMD_DATA_PRESENT | SDHC_CMD_RESPONSE);
        for (size_t i = 0; i < wire_size; i += sizeof(uint32_t)) {
            const uint32_t word =
                qtest_readl(qts, BCM2711_EMMC_BASE + SDHC_BDATA);
            const size_t valid = MIN(sizeof(uint32_t), chunk - i);

            memcpy(readback + offset + i, &word, valid);
        }
        offset += chunk;
    }
    g_assert_cmpmem(readback, firmware_size, firmware, firmware_size);
    qtest_quit(qts);
}
void test_cyw43455_sdio_fault_reset(void)
{
    QTestState *qts = qtest_init(
        "-M raspi-cm4 -nic none "
        "-device cyw43455-sdio,bus=sd-bus,fail-command=5,fail-after=1");

    qtest_writeb(qts, BCM2711_EMMC_BASE + SDHC_SWRST, SDHC_RESET_ALL);
    qtest_writew(qts, BCM2711_EMMC_BASE + SDHC_CLKCON,
                 SDHC_CLOCK_SDCLK_EN | SDHC_CLOCK_INT_EN);
    qtest_writew(qts, BCM2711_EMMC_BASE + SDHC_ERRINTSTSEN,
                 SDHC_EISEN_CMDTIMEOUT);

    raspi4_sdhci_command(qts, BCM2711_EMMC_BASE, 0, 0,
                         0x00ff8000, 0,
                         (5 << 8) | SDHC_CMD_RESPONSE);
    g_assert_true(qtest_readl(qts,
                              BCM2711_EMMC_BASE + SDHC_RSPREG0) & BIT(31));
    raspi4_sdhci_command(qts, BCM2711_EMMC_BASE, 0, 0,
                         0x00ff8000, 0,
                         (5 << 8) | SDHC_CMD_RESPONSE);
    g_assert_true(qtest_readw(qts,
                              BCM2711_EMMC_BASE + SDHC_ERRINTSTS) &
                  SDHC_EIS_CMDTIMEOUT);

    /* CMD0 resets the device-side command count and makes CMD5 succeed. */
    qtest_writew(qts, BCM2711_EMMC_BASE + SDHC_ERRINTSTS,
                 SDHC_EIS_CMDTIMEOUT);
    raspi4_sdhci_command(qts, BCM2711_EMMC_BASE, 0, 0, 0, 0, 0);
    raspi4_sdhci_command(qts, BCM2711_EMMC_BASE, 0, 0,
                         0x00ff8000, 0,
                         (5 << 8) | SDHC_CMD_RESPONSE);
    g_assert_false(qtest_readw(qts,
                               BCM2711_EMMC_BASE + SDHC_ERRINTSTS) &
                   SDHC_EIS_CMDTIMEOUT);
    g_assert_true(qtest_readl(qts,
                              BCM2711_EMMC_BASE + SDHC_RSPREG0) & BIT(31));

    qtest_quit(qts);
}
void test_sd_active_write_removal_migration(void)
{
    const uint16_t normal_status_enable =
        SDHC_NISEN_CMDCMP | SDHC_NISEN_TRSCMP | SDHC_NISEN_DMA |
        SDHC_NISEN_WBUFRDY | SDHC_NISEN_RBUFRDY |
        SDHC_NISEN_INSERT | SDHC_NISEN_REMOVE;
    const uint32_t interrupt_signal_enable =
        ((uint32_t)SDHC_EISEN_DATATIMEOUT << 16) |
        SDHC_NORINTSIG_REMOVE;
    g_autofree uint8_t *media = g_malloc(SD_SIZE);
    g_autofree char *sd_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_path = NULL;
    g_autofree char *media_after = NULL;
    gsize media_after_size = 0;
    QTestState *destination;
    QTestState *qts;
    uint32_t present_state;
    uint16_t normal_status;
    uint16_t error_status;

    memset(media, 0xa5, SD_SIZE);
    write_temp_image("raspi4-sd-active-remove-XXXXXX", media, SD_SIZE,
                     &sd_path);
    command = g_strdup_printf(
        "-M raspi4b -drive if=sd,id=sdcard,format=raw,"
        "file=%s,file.locking=off",
        sd_path);
    qts = qtest_init(command);
    raspi4_sdhci_initialize_card(qts);
    qtest_writew(qts, BCM2711_EMMC2_BASE + SDHC_NORINTSTSEN,
                 normal_status_enable);
    qtest_writew(qts, BCM2711_EMMC2_BASE + SDHC_ERRINTSTSEN,
                 SDHC_EISEN_DATATIMEOUT);
    qtest_writel(qts, BCM2711_EMMC2_BASE + SDHC_NORINTSIGEN,
                 interrupt_signal_enable);
    qtest_writel(qts, BCM2711_EMMC2_BASE + SDHC_NORINTSTS,
                 UINT32_MAX);

    raspi4_sdhci_command(qts, BCM2711_EMMC2_BASE, 512, 2, 0,
                         SDHC_TRNS_BLK_CNT_EN | SDHC_TRNS_MULTI,
                         (25 << 8) | SDHC_CMD_DATA_PRESENT |
                         SDHC_CMD_RESPONSE);
    for (unsigned int i = 0; i < 80; i++) {
        qtest_writel(qts, BCM2711_EMMC2_BASE + SDHC_BDATA, 0x3c3c3c3c);
    }
    present_state = qtest_readl(qts, BCM2711_EMMC2_BASE + SDHC_PRNSTS);
    g_assert_true(present_state & SDHC_DATA_INHIBIT);
    g_assert_true(present_state & SDHC_DAT_LINE_ACTIVE);
    g_assert_true(present_state & SDHC_DOING_WRITE);

    destination = migrate_to_new_qtest(qts, command, &migration_dir,
                                       &migration_path);
    qtest_irq_intercept_out(destination, BCM2711_MMC_IRQ_OR_QOM_PATH);
    qtest_qmp_assert_success(
        destination, "{ 'execute': 'eject', 'arguments': {"
                     "'device': 'sdcard', 'force': true } }");

    present_state = qtest_readl(destination,
                               BCM2711_EMMC2_BASE + SDHC_PRNSTS);
    normal_status = qtest_readw(destination,
                               BCM2711_EMMC2_BASE + SDHC_NORINTSTS);
    error_status = qtest_readw(destination,
                              BCM2711_EMMC2_BASE + SDHC_ERRINTSTS);
    g_assert_false(present_state & SDHC_CARD_PRESENT);
    g_assert_false(present_state & (SDHC_DATA_INHIBIT |
                                    SDHC_DAT_LINE_ACTIVE |
                                    SDHC_DOING_READ |
                                    SDHC_DOING_WRITE));
    g_assert_true(normal_status & SDHC_NIS_REMOVE);
    g_assert_true(normal_status & SDHC_NIS_ERR);
    g_assert_false(normal_status & (SDHC_NIS_TRSCMP | SDHC_NIS_DMA |
                                    SDHC_NIS_WBUFRDY |
                                    SDHC_NIS_RBUFRDY));
    g_assert_true(error_status & SDHC_EIS_DATATIMEOUT);
    g_assert_true(qtest_get_irq(destination, 0));

    qtest_writel(destination, BCM2711_EMMC2_BASE + SDHC_NORINTSTS,
                 ((uint32_t)SDHC_EIS_DATATIMEOUT << 16) |
                 SDHC_NIS_REMOVE | SDHC_NIS_ERR);
    g_assert_false(qtest_get_irq(destination, 0));
    qtest_qmp_assert_success(
        destination,
        "{ 'execute': 'blockdev-change-medium', 'arguments': {"
        "'device': 'sdcard', 'filename': %s, 'format': 'raw' } }",
        sd_path);
    present_state = qtest_readl(destination,
                               BCM2711_EMMC2_BASE + SDHC_PRNSTS);
    g_assert_true(present_state & SDHC_CARD_PRESENT);

    raspi4_sdhci_initialize_card(destination);
    raspi4_sdhci_command(destination, BCM2711_EMMC2_BASE, 512, 1, 0,
                         SDHC_TRNS_BLK_CNT_EN | SDHC_TRNS_READ,
                         (17 << 8) | SDHC_CMD_DATA_PRESENT |
                         SDHC_CMD_RESPONSE);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_EMMC2_BASE + SDHC_BDATA),
                    ==, 0xa5a5a5a5);
    qtest_system_reset(destination);
    present_state = qtest_readl(destination,
                               BCM2711_EMMC2_BASE + SDHC_PRNSTS);
    g_assert_true(present_state & SDHC_CARD_PRESENT);
    g_assert_false(present_state & (SDHC_DATA_INHIBIT |
                                    SDHC_DAT_LINE_ACTIVE |
                                    SDHC_DOING_READ |
                                    SDHC_DOING_WRITE));

    qtest_quit(qts);
    qtest_quit(destination);
    g_assert_true(g_file_get_contents(sd_path, &media_after,
                                      &media_after_size, NULL));
    g_assert_cmpuint(media_after_size, ==, SD_SIZE);
    g_assert_cmpmem(media_after, 512, media, 512);
    unlink(sd_path);
    unlink(migration_path);
    rmdir(migration_dir);
}
void test_emmc_power_cut_sector_durability_migration(void)
{
    g_autofree uint8_t *media = g_malloc(SD_SIZE);
    g_autofree uint8_t *expected_sector = g_malloc(512);
    g_autofree char *emmc_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_path = NULL;
    g_autofree char *media_after = NULL;
    gsize media_after_size = 0;
    QTestState *destination;
    QTestState *qts;
    uint32_t present_state;

    memset(media, 0xa5, SD_SIZE);
    memset(expected_sector, 0x5a, 512);
    write_temp_image("raspi4-emmc-power-cut-XXXXXX", media, SD_SIZE,
                     &emmc_path);
    command = g_strdup_printf(
        "-M raspi-cm4,emmc-drive=emmc "
        "-drive if=none,id=emmc,format=raw,file=%s,file.locking=off",
        emmc_path);
    qts = qtest_init(command);
    raspi4_sdhci_initialize_emmc(qts);

    raspi4_sdhci_command(qts, BCM2711_EMMC2_BASE, 512, 2, 0,
                         SDHC_TRNS_BLK_CNT_EN | SDHC_TRNS_MULTI,
                         (25 << 8) | SDHC_CMD_DATA_PRESENT |
                         SDHC_CMD_RESPONSE);
    for (unsigned int i = 0; i < 80; i++) {
        qtest_writel(qts, BCM2711_EMMC2_BASE + SDHC_BDATA, 0x3c3c3c3c);
    }
    present_state = qtest_readl(qts, BCM2711_EMMC2_BASE + SDHC_PRNSTS);
    g_assert_true(present_state & SDHC_DATA_INHIBIT);
    g_assert_true(present_state & SDHC_DAT_LINE_ACTIVE);
    g_assert_true(present_state & SDHC_DOING_WRITE);

    destination = migrate_to_new_qtest(qts, command, &migration_dir,
                                       &migration_path);
    qtest_system_reset(destination);
    present_state = qtest_readl(destination,
                               BCM2711_EMMC2_BASE + SDHC_PRNSTS);
    g_assert_true(present_state & SDHC_CARD_PRESENT);
    g_assert_false(present_state & (SDHC_DATA_INHIBIT |
                                    SDHC_DAT_LINE_ACTIVE |
                                    SDHC_DOING_READ |
                                    SDHC_DOING_WRITE));
    raspi4_sdhci_initialize_emmc(destination);
    raspi4_sdhci_command(destination, BCM2711_EMMC2_BASE, 512, 1, 0,
                         SDHC_TRNS_BLK_CNT_EN | SDHC_TRNS_READ,
                         (17 << 8) | SDHC_CMD_DATA_PRESENT |
                         SDHC_CMD_RESPONSE);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_EMMC2_BASE + SDHC_BDATA),
                    ==, 0xa5a5a5a5);

    qtest_system_reset(destination);
    raspi4_sdhci_initialize_emmc(destination);
    raspi4_sdhci_command(destination, BCM2711_EMMC2_BASE, 512, 1, 0,
                         SDHC_TRNS_BLK_CNT_EN,
                         (24 << 8) | SDHC_CMD_DATA_PRESENT |
                         SDHC_CMD_RESPONSE);
    for (unsigned int i = 0; i < 128; i++) {
        qtest_writel(destination, BCM2711_EMMC2_BASE + SDHC_BDATA,
                     0x5a5a5a5a);
    }
    present_state = qtest_readl(destination,
                               BCM2711_EMMC2_BASE + SDHC_PRNSTS);
    g_assert_false(present_state & (SDHC_DATA_INHIBIT |
                                    SDHC_DAT_LINE_ACTIVE |
                                    SDHC_DOING_WRITE));
    qtest_system_reset(destination);
    raspi4_sdhci_initialize_emmc(destination);
    raspi4_sdhci_command(destination, BCM2711_EMMC2_BASE, 512, 1, 0,
                         SDHC_TRNS_BLK_CNT_EN | SDHC_TRNS_READ,
                         (17 << 8) | SDHC_CMD_DATA_PRESENT |
                         SDHC_CMD_RESPONSE);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_EMMC2_BASE + SDHC_BDATA),
                    ==, 0x5a5a5a5a);

    qtest_quit(qts);
    qtest_quit(destination);
    g_assert_true(g_file_get_contents(emmc_path, &media_after,
                                      &media_after_size, NULL));
    g_assert_cmpuint(media_after_size, ==, SD_SIZE);
    g_assert_cmpmem(media_after, 512, expected_sector, 512);
    g_assert_cmpmem(media_after + 512, 512, media + 512, 512);
    unlink(emmc_path);
    unlink(migration_path);
    rmdir(migration_dir);
}
void test_emmc_volatile_cache_power_cut_flush_migration(void)
{
    g_autofree uint8_t *media = g_malloc(SD_SIZE);
    g_autofree char *emmc_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_path = NULL;
    g_autofree char *contents = NULL;
    gsize contents_size = 0;
    QTestState *destination;
    QTestState *qts;

    memset(media, 0xa5, SD_SIZE);
    write_temp_image("raspi4-emmc-cache-cut-XXXXXX", media, SD_SIZE,
                     &emmc_path);
    command = g_strdup_printf(
        "-M raspi-cm4,emmc-drive=emmc,emmc-cache-size=1K,"
        "emmc-cache-power-loss-on-reset=on "
        "-drive if=none,id=emmc,format=raw,file=%s,file.locking=off",
        emmc_path);
    qts = qtest_init(command);
    raspi4_sdhci_initialize_emmc(qts);
    g_assert_cmpuint(raspi4_emmc_read_cache_size_kib(qts), ==, 1);

    raspi4_emmc_switch(qts, 33, 1);
    raspi4_emmc_write_sector(qts, 0, 0x5a5a5a5a);
    g_assert_cmpuint(raspi4_emmc_cache_dirty_sectors(qts), ==, 1);
    g_assert_cmphex(raspi4_emmc_read_word(qts, 0), ==, 0x5a5a5a5a);
    g_assert_true(g_file_get_contents(emmc_path, &contents,
                                      &contents_size, NULL));
    g_assert_cmpuint(contents_size, ==, SD_SIZE);
    g_assert_cmphex((uint8_t)contents[0], ==, 0xa5);

    destination = migrate_to_new_qtest(qts, command, &migration_dir,
                                       &migration_path);
    g_assert_cmpuint(raspi4_emmc_cache_dirty_sectors(destination), ==, 1);
    g_assert_cmphex(raspi4_emmc_read_word(destination, 0), ==, 0x5a5a5a5a);
    qtest_system_reset(destination);
    g_assert_cmpuint(raspi4_emmc_cache_dirty_sectors(destination), ==, 0);
    raspi4_sdhci_initialize_emmc(destination);
    g_assert_cmphex(raspi4_emmc_read_word(destination, 0), ==, 0xa5a5a5a5);

    raspi4_emmc_switch(destination, 33, 1);
    raspi4_emmc_write_sector(destination, 0, 0x5a5a5a5a);
    raspi4_emmc_switch(destination, 32, 1);
    g_assert_cmpuint(raspi4_emmc_cache_dirty_sectors(destination), ==, 0);
    g_clear_pointer(&contents, g_free);
    g_assert_true(g_file_get_contents(emmc_path, &contents,
                                      &contents_size, NULL));
    g_assert_cmphex((uint8_t)contents[0], ==, 0x5a);
    qtest_system_reset(destination);
    raspi4_sdhci_initialize_emmc(destination);
    g_assert_cmphex(raspi4_emmc_read_word(destination, 0), ==, 0x5a5a5a5a);

    raspi4_emmc_switch(destination, 33, 1);
    raspi4_emmc_write_sector(destination, 512, 0x11111111);
    raspi4_emmc_write_sector(destination, 1024, 0x22222222);
    g_assert_cmpuint(raspi4_emmc_cache_dirty_sectors(destination), ==, 2);
    raspi4_emmc_write_sector(destination, 1536, 0x33333333);
    g_assert_cmpuint(raspi4_emmc_cache_dirty_sectors(destination), ==, 1);
    g_clear_pointer(&contents, g_free);
    g_assert_true(g_file_get_contents(emmc_path, &contents,
                                      &contents_size, NULL));
    g_assert_cmphex((uint8_t)contents[512], ==, 0x11);
    g_assert_cmphex((uint8_t)contents[1024], ==, 0x22);
    g_assert_cmphex((uint8_t)contents[1536], ==, 0xa5);
    qtest_system_reset(destination);
    raspi4_sdhci_initialize_emmc(destination);
    g_assert_cmphex(raspi4_emmc_read_word(destination, 512), ==, 0x11111111);
    g_assert_cmphex(raspi4_emmc_read_word(destination, 1024), ==, 0x22222222);
    g_assert_cmphex(raspi4_emmc_read_word(destination, 1536), ==, 0xa5a5a5a5);

    qtest_quit(qts);
    qtest_quit(destination);
    unlink(emmc_path);
    unlink(migration_path);
    rmdir(migration_dir);
}
void test_emmc_timed_cache_flush_power_cut_migration(void)
{
    g_autofree uint8_t *media = g_malloc(SD_SIZE);
    g_autofree char *emmc_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_path = NULL;
    g_autofree char *contents = NULL;
    gsize contents_size = 0;
    QTestState *destination;
    QTestState *qts;

    memset(media, 0xa5, SD_SIZE);
    write_temp_image("raspi4-emmc-timed-cache-XXXXXX", media, SD_SIZE,
                     &emmc_path);
    command = g_strdup_printf(
        "-M raspi-cm4,emmc-drive=emmc,emmc-cache-size=2K,"
        "emmc-cache-power-loss-on-reset=on,"
        "emmc-cache-flush-sector-delay-us=10 "
        "-drive if=none,id=emmc,format=raw,file=%s,file.locking=off",
        emmc_path);
    qts = qtest_init(command);
    raspi4_sdhci_initialize_emmc(qts);
    raspi4_emmc_switch(qts, 33, 1);
    raspi4_emmc_write_sector(qts, 0, 0x11111111);
    raspi4_emmc_write_sector(qts, 512, 0x22222222);
    raspi4_emmc_write_sector(qts, 1024, 0x33333333);
    raspi4_emmc_write_sector(qts, 1536, 0x44444444);
    g_assert_cmpuint(raspi4_emmc_cache_dirty_sectors(qts), ==, 4);

    raspi4_emmc_switch(qts, 32, 1);
    g_assert_true(raspi4_emmc_cache_flush_active(qts));
    g_assert_cmpuint(raspi4_emmc_cache_flush_completed(qts), ==, 0);
    qtest_clock_step(qts, 9 * 1000);
    g_assert_cmpuint(raspi4_emmc_cache_dirty_sectors(qts), ==, 4);
    g_assert_cmpuint(raspi4_emmc_cache_flush_completed(qts), ==, 0);
    qtest_clock_step(qts, 1000);
    g_assert_cmpuint(raspi4_emmc_cache_dirty_sectors(qts), ==, 3);
    g_assert_cmpuint(raspi4_emmc_cache_flush_completed(qts), ==, 1);
    g_assert_true(raspi4_emmc_cache_flush_active(qts));
    g_assert_true(g_file_get_contents(emmc_path, &contents,
                                      &contents_size, NULL));
    g_assert_cmpuint(contents_size, ==, SD_SIZE);
    g_assert_cmphex((uint8_t)contents[0], ==, 0x11);
    g_assert_cmphex((uint8_t)contents[512], ==, 0xa5);

    destination = migrate_to_new_qtest(qts, command, &migration_dir,
                                       &migration_path);
    g_assert_true(raspi4_emmc_cache_flush_active(destination));
    g_assert_cmpuint(raspi4_emmc_cache_dirty_sectors(destination), ==, 3);
    g_assert_cmpuint(raspi4_emmc_cache_flush_completed(destination), ==, 1);
    qtest_clock_step_next(destination);
    g_assert_cmpuint(raspi4_emmc_cache_dirty_sectors(destination), ==, 2);
    qtest_clock_step_next(destination);
    g_assert_true(raspi4_emmc_cache_flush_active(destination));
    g_assert_cmpuint(raspi4_emmc_cache_dirty_sectors(destination), ==, 1);
    g_assert_cmpuint(raspi4_emmc_cache_flush_completed(destination), ==, 3);
    g_clear_pointer(&contents, g_free);
    g_assert_true(g_file_get_contents(emmc_path, &contents,
                                      &contents_size, NULL));
    g_assert_cmphex((uint8_t)contents[0], ==, 0x11);
    g_assert_cmphex((uint8_t)contents[512], ==, 0x22);
    g_assert_cmphex((uint8_t)contents[1024], ==, 0x33);
    g_assert_cmphex((uint8_t)contents[1536], ==, 0xa5);

    qtest_system_reset(destination);
    g_assert_false(raspi4_emmc_cache_flush_active(destination));
    g_assert_cmpuint(raspi4_emmc_cache_dirty_sectors(destination), ==, 0);
    g_assert_cmpuint(raspi4_emmc_cache_flush_completed(destination), ==, 0);
    raspi4_sdhci_initialize_emmc(destination);
    g_assert_cmphex(raspi4_emmc_read_word(destination, 0), ==, 0x11111111);
    g_assert_cmphex(raspi4_emmc_read_word(destination, 512), ==, 0x22222222);
    g_assert_cmphex(raspi4_emmc_read_word(destination, 1024), ==, 0x33333333);
    g_assert_cmphex(raspi4_emmc_read_word(destination, 1536), ==, 0xa5a5a5a5);

    qtest_quit(qts);
    qtest_quit(destination);
    unlink(emmc_path);
    unlink(migration_path);
    rmdir(migration_dir);
}
void test_emmc_timed_cache_flush_all_cut_points(void)
{
    static const uint32_t values[] = {
        0x11111111, 0x22222222, 0x33333333, 0x44444444,
    };

    for (unsigned int cut = 0; cut <= ARRAY_SIZE(values); cut++) {
        g_autofree uint8_t *media = g_malloc(SD_SIZE);
        g_autofree char *emmc_path = NULL;
        g_autofree char *command = NULL;
        QTestState *qts;

        memset(media, 0xa5, SD_SIZE);
        write_temp_image("raspi4-emmc-cache-campaign-XXXXXX",
                         media, SD_SIZE, &emmc_path);
        command = g_strdup_printf(
            "-M raspi-cm4,emmc-drive=emmc,emmc-cache-size=2K,"
            "emmc-cache-power-loss-on-reset=on,"
            "emmc-cache-flush-sector-delay-us=10 "
            "-drive if=none,id=emmc,format=raw,file=%s,file.locking=off",
            emmc_path);
        qts = qtest_init(command);
        raspi4_sdhci_initialize_emmc(qts);
        raspi4_emmc_switch(qts, 33, 1);
        for (unsigned int sector = 0; sector < ARRAY_SIZE(values); sector++) {
            raspi4_emmc_write_sector(qts, sector * 512, values[sector]);
        }
        raspi4_emmc_switch(qts, 32, 1);
        for (unsigned int completed = 0; completed < cut; completed++) {
            qtest_clock_step(qts, 10 * 1000);
            g_assert_cmpuint(raspi4_emmc_cache_flush_completed(qts),
                             ==, completed + 1);
        }
        g_assert_cmpuint(raspi4_emmc_cache_dirty_sectors(qts),
                         ==, ARRAY_SIZE(values) - cut);
        g_assert_cmpint(raspi4_emmc_cache_flush_active(qts), ==,
                        cut < ARRAY_SIZE(values));

        qtest_system_reset(qts);
        raspi4_sdhci_initialize_emmc(qts);
        for (unsigned int sector = 0; sector < ARRAY_SIZE(values); sector++) {
            uint32_t expected = sector < cut ? values[sector] : 0xa5a5a5a5;

            g_assert_cmphex(raspi4_emmc_read_word(qts, sector * 512),
                            ==, expected);
        }
        qtest_quit(qts);
        unlink(emmc_path);
    }
}
void test_emmc_timed_cache_flush_error_retry(void)
{
    static const char blkdebug_config[] =
        "[inject-error]\n"
        "event = \"flush_to_disk\"\n"
        "errno = \"5\"\n"
        "once = \"on\"\n"
        "immediately = \"on\"\n";
    g_autofree uint8_t *media = g_malloc(SD_SIZE);
    g_autofree char *emmc_path = NULL;
    g_autofree char *debug_path = NULL;
    g_autofree char *command = NULL;
    QTestState *qts;
    uint32_t status;

    memset(media, 0xa5, SD_SIZE);
    write_temp_image("raspi4-emmc-timed-error-XXXXXX", media, SD_SIZE,
                     &emmc_path);
    write_temp_text("raspi4-emmc-timed-debug-XXXXXX", blkdebug_config,
                    &debug_path);
    command = g_strdup_printf(
        "-M raspi-cm4,emmc-drive=emmc,emmc-cache-size=1K,"
        "emmc-cache-power-loss-on-reset=on,"
        "emmc-cache-flush-sector-delay-us=10 "
        "-drive if=none,id=emmc,format=raw,"
        "file=blkdebug:%s:%s",
        debug_path, emmc_path);
    qts = qtest_init(command);
    raspi4_sdhci_initialize_emmc(qts);
    raspi4_emmc_switch(qts, 33, 1);
    raspi4_emmc_write_sector(qts, 0, 0x11111111);
    raspi4_emmc_write_sector(qts, 512, 0x22222222);
    raspi4_emmc_switch(qts, 32, 1);
    qtest_clock_step(qts, 10 * 1000);
    g_assert_false(raspi4_emmc_cache_flush_active(qts));
    g_assert_cmpuint(raspi4_emmc_cache_flush_completed(qts), ==, 0);
    g_assert_cmpuint(raspi4_emmc_cache_dirty_sectors(qts), ==, 2);
    status = raspi4_emmc_status(qts);
    g_assert_true(status & BIT(19));

    raspi4_emmc_switch(qts, 32, 1);
    qtest_clock_step(qts, 10 * 1000);
    g_assert_cmpuint(raspi4_emmc_cache_flush_completed(qts), ==, 1);
    qtest_clock_step(qts, 10 * 1000);
    g_assert_false(raspi4_emmc_cache_flush_active(qts));
    g_assert_cmpuint(raspi4_emmc_cache_flush_completed(qts), ==, 2);
    g_assert_cmpuint(raspi4_emmc_cache_dirty_sectors(qts), ==, 0);
    status = raspi4_emmc_status(qts);
    g_assert_false(status & BIT(19));

    qtest_system_reset(qts);
    raspi4_sdhci_initialize_emmc(qts);
    g_assert_cmphex(raspi4_emmc_read_word(qts, 0), ==, 0x11111111);
    g_assert_cmphex(raspi4_emmc_read_word(qts, 512), ==, 0x22222222);

    qtest_quit(qts);
    unlink(emmc_path);
    unlink(debug_path);
}
void test_emmc_timed_program_all_cut_points(void)
{
    static const uint32_t values[] = {
        0x11111111, 0x22222222, 0x33333333, 0x44444444,
    };

    for (unsigned int mode = 0; mode < 2; mode++) {
        bool reliable = mode != 0;

        for (unsigned int cut = 0; cut <= ARRAY_SIZE(values); cut++) {
            g_autofree uint8_t *media = g_malloc(SD_SIZE);
            g_autofree char *emmc_path = NULL;
            g_autofree char *command = NULL;
            QTestState *qts;

            memset(media, 0xa5, SD_SIZE);
            write_temp_image("raspi4-emmc-program-campaign-XXXXXX",
                             media, SD_SIZE, &emmc_path);
            command = g_strdup_printf(
                "-M raspi-cm4,emmc-drive=emmc,"
                "emmc-program-sector-delay-us=10 "
                "-drive if=none,id=emmc,format=raw,file=%s,"
                "file.locking=off",
                emmc_path);
            qts = qtest_init(command);
            raspi4_sdhci_initialize_emmc(qts);
            raspi4_emmc_write_multiple(qts, 0, values,
                                       ARRAY_SIZE(values), reliable);
            g_assert_true(raspi4_emmc_program_active(qts));
            g_assert_cmpuint(raspi4_emmc_program_pending(qts),
                             ==, ARRAY_SIZE(values));
            g_assert_cmpuint(raspi4_emmc_program_completed(qts), ==, 0);

            for (unsigned int completed = 0; completed < cut; completed++) {
                qtest_clock_step(qts, 10 * 1000);
                g_assert_cmpuint(raspi4_emmc_program_completed(qts),
                                 ==, completed + 1);
            }
            g_assert_cmpuint(raspi4_emmc_program_pending(qts),
                             ==, ARRAY_SIZE(values) - cut);
            g_assert_cmpint(raspi4_emmc_program_active(qts), ==,
                            cut < ARRAY_SIZE(values));

            qtest_system_reset(qts);
            raspi4_sdhci_initialize_emmc(qts);
            for (unsigned int sector = 0;
                 sector < ARRAY_SIZE(values); sector++) {
                uint32_t expected =
                    sector < cut ? values[sector] : 0xa5a5a5a5;

                g_assert_cmphex(
                    raspi4_emmc_read_word(qts, sector * 512),
                    ==, expected);
            }
            qtest_quit(qts);
            unlink(emmc_path);
        }
    }
}
void test_emmc_timed_reliable_program_migration(void)
{
    static const uint32_t values[] = {
        0x11111111, 0x22222222, 0x33333333,
    };
    g_autofree uint8_t *media = g_malloc(SD_SIZE);
    g_autofree char *emmc_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_path = NULL;
    QTestState *destination;
    QTestState *qts;

    memset(media, 0xa5, SD_SIZE);
    write_temp_image("raspi4-emmc-program-migration-XXXXXX",
                     media, SD_SIZE, &emmc_path);
    command = g_strdup_printf(
        "-M raspi-cm4,emmc-drive=emmc,"
        "emmc-program-sector-delay-us=10 "
        "-drive if=none,id=emmc,format=raw,file=%s,file.locking=off",
        emmc_path);
    qts = qtest_init(command);
    raspi4_sdhci_initialize_emmc(qts);
    raspi4_emmc_write_multiple(qts, 0, values, ARRAY_SIZE(values), true);
    qtest_clock_step(qts, 10 * 1000);
    g_assert_cmpuint(raspi4_emmc_program_completed(qts), ==, 1);
    g_assert_cmpuint(raspi4_emmc_program_pending(qts), ==, 2);

    destination = migrate_to_new_qtest(qts, command, &migration_dir,
                                       &migration_path);
    g_assert_true(raspi4_emmc_program_active(destination));
    g_assert_cmpuint(raspi4_emmc_program_completed(destination), ==, 1);
    g_assert_cmpuint(raspi4_emmc_program_pending(destination), ==, 2);
    qtest_clock_step_next(destination);
    g_assert_cmpuint(raspi4_emmc_program_completed(destination), ==, 2);
    g_assert_cmpuint(raspi4_emmc_program_pending(destination), ==, 1);

    qtest_system_reset(destination);
    g_assert_false(raspi4_emmc_program_active(destination));
    g_assert_cmpuint(raspi4_emmc_program_pending(destination), ==, 0);
    raspi4_sdhci_initialize_emmc(destination);
    g_assert_cmphex(raspi4_emmc_read_word(destination, 0),
                    ==, values[0]);
    g_assert_cmphex(raspi4_emmc_read_word(destination, 512),
                    ==, values[1]);
    g_assert_cmphex(raspi4_emmc_read_word(destination, 1024),
                    ==, 0xa5a5a5a5);

    qtest_quit(qts);
    qtest_quit(destination);
    unlink(emmc_path);
    unlink(migration_path);
    rmdir(migration_dir);
}
void test_emmc_timed_program_error_retry(void)
{
    static const char blkdebug_config[] =
        "[inject-error]\n"
        "event = \"flush_to_disk\"\n"
        "errno = \"5\"\n"
        "once = \"on\"\n"
        "immediately = \"on\"\n";
    static const uint32_t values[] = {
        0x11111111, 0x22222222,
    };
    g_autofree uint8_t *media = g_malloc(SD_SIZE);
    g_autofree char *emmc_path = NULL;
    g_autofree char *debug_path = NULL;
    g_autofree char *command = NULL;
    QTestState *qts;
    uint32_t status;

    memset(media, 0xa5, SD_SIZE);
    write_temp_image("raspi4-emmc-program-error-XXXXXX", media, SD_SIZE,
                     &emmc_path);
    write_temp_text("raspi4-emmc-program-debug-XXXXXX", blkdebug_config,
                    &debug_path);
    command = g_strdup_printf(
        "-M raspi-cm4,emmc-drive=emmc,"
        "emmc-program-sector-delay-us=10 "
        "-drive if=none,id=emmc,format=raw,"
        "file=blkdebug:%s:%s",
        debug_path, emmc_path);
    qts = qtest_init(command);
    raspi4_sdhci_initialize_emmc(qts);
    raspi4_emmc_write_multiple(qts, 0, values, ARRAY_SIZE(values), true);
    qtest_clock_step_next(qts);
    g_assert_false(raspi4_emmc_program_active(qts));
    g_assert_cmpuint(raspi4_emmc_program_pending(qts), ==, 2);
    g_assert_cmpuint(raspi4_emmc_program_completed(qts), ==, 0);
    status = raspi4_emmc_status(qts);
    g_assert_true(status & BIT(19));

    raspi4_emmc_write_multiple(qts, 0, values, ARRAY_SIZE(values), true);
    g_assert_true(raspi4_emmc_program_active(qts));
    g_assert_cmpuint(raspi4_emmc_program_pending(qts), ==, 2);
    qtest_clock_step_next(qts);
    g_assert_cmpuint(raspi4_emmc_program_completed(qts), ==, 1);
    qtest_clock_step_next(qts);
    g_assert_false(raspi4_emmc_program_active(qts));
    g_assert_cmpuint(raspi4_emmc_program_pending(qts), ==, 0);
    g_assert_cmpuint(raspi4_emmc_program_completed(qts), ==, 2);
    status = raspi4_emmc_status(qts);
    g_assert_false(status & BIT(19));

    qtest_system_reset(qts);
    raspi4_sdhci_initialize_emmc(qts);
    g_assert_cmphex(raspi4_emmc_read_word(qts, 0), ==, values[0]);
    g_assert_cmphex(raspi4_emmc_read_word(qts, 512), ==, values[1]);

    qtest_quit(qts);
    unlink(emmc_path);
    unlink(debug_path);
}
void test_emmc_timed_erase_all_cut_points(void)
{
    const uint32_t group_size = 512 * KiB;
    const unsigned int group_count = 4;
    const uint32_t end_address =
        group_count * group_size - 512;

    for (unsigned int cut = 0; cut <= group_count; cut++) {
        g_autofree uint8_t *media = g_malloc(SD_SIZE);
        g_autofree char *emmc_path = NULL;
        g_autofree char *command = NULL;
        QTestState *qts;

        memset(media, 0xa5, SD_SIZE);
        write_temp_image("raspi4-emmc-erase-campaign-XXXXXX",
                         media, SD_SIZE, &emmc_path);
        command = g_strdup_printf(
            "-M raspi-cm4,emmc-drive=emmc,"
            "emmc-erase-group-delay-us=10 "
            "-drive if=none,id=emmc,format=raw,file=%s,"
            "file.locking=off",
            emmc_path);
        qts = qtest_init(command);
        raspi4_sdhci_initialize_emmc(qts);
        raspi4_emmc_erase(qts, 0, end_address);
        g_assert_true(raspi4_emmc_erase_active(qts));
        g_assert_cmpuint(raspi4_emmc_erase_pending(qts),
                         ==, group_count);
        g_assert_cmpuint(raspi4_emmc_erase_completed(qts), ==, 0);

        for (unsigned int completed = 0; completed < cut; completed++) {
            qtest_clock_step(qts, 10 * 1000);
            g_assert_cmpuint(raspi4_emmc_erase_completed(qts),
                             ==, completed + 1);
        }
        g_assert_cmpuint(raspi4_emmc_erase_pending(qts),
                         ==, group_count - cut);
        g_assert_cmpint(raspi4_emmc_erase_active(qts), ==,
                        cut < group_count);

        qtest_system_reset(qts);
        raspi4_sdhci_initialize_emmc(qts);
        for (unsigned int group = 0; group < group_count; group++) {
            uint32_t expected = group < cut ? UINT32_MAX : 0xa5a5a5a5;

            g_assert_cmphex(
                raspi4_emmc_read_word(qts, group * group_size),
                ==, expected);
            g_assert_cmphex(
                raspi4_emmc_read_word(
                    qts, (group + 1) * group_size - 512),
                ==, expected);
        }
        g_assert_cmphex(
            raspi4_emmc_read_word(qts, group_count * group_size),
            ==, 0xa5a5a5a5);
        qtest_quit(qts);
        unlink(emmc_path);
    }
}
void test_emmc_timed_erase_cache_migration(void)
{
    const uint32_t group_size = 512 * KiB;
    const unsigned int group_count = 3;
    const uint32_t end_address =
        group_count * group_size - 512;
    g_autofree uint8_t *media = g_malloc(SD_SIZE);
    g_autofree char *emmc_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_path = NULL;
    QTestState *destination;
    QTestState *qts;

    memset(media, 0xa5, SD_SIZE);
    write_temp_image("raspi4-emmc-erase-migration-XXXXXX",
                     media, SD_SIZE, &emmc_path);
    command = g_strdup_printf(
        "-M raspi-cm4,emmc-drive=emmc,emmc-cache-size=1K,"
        "emmc-cache-power-loss-on-reset=on,"
        "emmc-erase-group-delay-us=10 "
        "-drive if=none,id=emmc,format=raw,file=%s,file.locking=off",
        emmc_path);
    qts = qtest_init(command);
    raspi4_sdhci_initialize_emmc(qts);
    raspi4_emmc_switch(qts, 33, 1);
    raspi4_emmc_write_sector(qts, 0, 0x11111111);
    raspi4_emmc_write_sector(qts, group_count * group_size, 0x22222222);
    g_assert_cmpuint(raspi4_emmc_cache_dirty_sectors(qts), ==, 2);

    raspi4_emmc_erase(qts, 0, end_address);
    g_assert_cmpuint(raspi4_emmc_cache_dirty_sectors(qts), ==, 1);
    qtest_clock_step(qts, 10 * 1000);
    g_assert_cmpuint(raspi4_emmc_erase_completed(qts), ==, 1);
    g_assert_cmpuint(raspi4_emmc_erase_pending(qts), ==, 2);

    destination = migrate_to_new_qtest(qts, command, &migration_dir,
                                       &migration_path);
    g_assert_true(raspi4_emmc_erase_active(destination));
    g_assert_cmpuint(raspi4_emmc_erase_completed(destination), ==, 1);
    g_assert_cmpuint(raspi4_emmc_erase_pending(destination), ==, 2);
    g_assert_cmpuint(raspi4_emmc_cache_dirty_sectors(destination), ==, 1);
    qtest_clock_step_next(destination);
    g_assert_cmpuint(raspi4_emmc_erase_completed(destination), ==, 2);
    g_assert_cmpuint(raspi4_emmc_erase_pending(destination), ==, 1);

    qtest_system_reset(destination);
    raspi4_sdhci_initialize_emmc(destination);
    g_assert_cmphex(raspi4_emmc_read_word(destination, 0), ==, UINT32_MAX);
    g_assert_cmphex(raspi4_emmc_read_word(destination, group_size),
                    ==, UINT32_MAX);
    g_assert_cmphex(raspi4_emmc_read_word(destination, 2 * group_size),
                    ==, 0xa5a5a5a5);
    g_assert_cmphex(
        raspi4_emmc_read_word(destination, group_count * group_size),
        ==, 0xa5a5a5a5);

    qtest_quit(qts);
    qtest_quit(destination);
    unlink(emmc_path);
    unlink(migration_path);
    rmdir(migration_dir);
}
void test_emmc_timed_erase_error_retry(void)
{
    static const char blkdebug_config[] =
        "[inject-error]\n"
        "event = \"flush_to_disk\"\n"
        "errno = \"5\"\n"
        "once = \"on\"\n"
        "immediately = \"on\"\n";
    const uint32_t group_size = 512 * KiB;
    const unsigned int group_count = 2;
    const uint32_t end_address =
        group_count * group_size - 512;
    g_autofree uint8_t *media = g_malloc(SD_SIZE);
    g_autofree char *emmc_path = NULL;
    g_autofree char *debug_path = NULL;
    g_autofree char *command = NULL;
    QTestState *qts;
    uint32_t status;

    memset(media, 0xa5, SD_SIZE);
    write_temp_image("raspi4-emmc-erase-error-XXXXXX", media, SD_SIZE,
                     &emmc_path);
    write_temp_text("raspi4-emmc-erase-debug-XXXXXX", blkdebug_config,
                    &debug_path);
    command = g_strdup_printf(
        "-M raspi-cm4,emmc-drive=emmc,"
        "emmc-erase-group-delay-us=10 "
        "-drive if=none,id=emmc,format=raw,"
        "file=blkdebug:%s:%s",
        debug_path, emmc_path);
    qts = qtest_init(command);
    raspi4_sdhci_initialize_emmc(qts);
    raspi4_emmc_erase(qts, 0, end_address);
    qtest_clock_step_next(qts);
    g_assert_false(raspi4_emmc_erase_active(qts));
    g_assert_cmpuint(raspi4_emmc_erase_pending(qts), ==, group_count);
    g_assert_cmpuint(raspi4_emmc_erase_completed(qts), ==, 0);
    status = raspi4_emmc_status(qts);
    g_assert_true(status & BIT(19));

    raspi4_emmc_erase(qts, 0, end_address);
    g_assert_true(raspi4_emmc_erase_active(qts));
    qtest_clock_step_next(qts);
    qtest_clock_step_next(qts);
    g_assert_false(raspi4_emmc_erase_active(qts));
    g_assert_cmpuint(raspi4_emmc_erase_pending(qts), ==, 0);
    g_assert_cmpuint(raspi4_emmc_erase_completed(qts), ==, group_count);
    status = raspi4_emmc_status(qts);
    g_assert_false(status & BIT(19));

    qtest_system_reset(qts);
    raspi4_sdhci_initialize_emmc(qts);
    g_assert_cmphex(raspi4_emmc_read_word(qts, 0), ==, UINT32_MAX);
    g_assert_cmphex(raspi4_emmc_read_word(qts, group_size), ==, UINT32_MAX);
    g_assert_cmphex(raspi4_emmc_read_word(qts, 2 * group_size),
                    ==, 0xa5a5a5a5);

    qtest_quit(qts);
    unlink(emmc_path);
    unlink(debug_path);
}
void test_emmc_erase_sync_cache_invalidation(void)
{
    const uint32_t group_size = 512 * KiB;
    uint8_t ext_csd[512];
    g_autofree uint8_t *media = g_malloc(SD_SIZE);
    g_autofree char *emmc_path = NULL;
    g_autofree char *command = NULL;
    QTestState *qts;

    memset(media, 0xa5, SD_SIZE);
    write_temp_image("raspi4-emmc-erase-sync-XXXXXX", media, SD_SIZE,
                     &emmc_path);
    command = g_strdup_printf(
        "-M raspi-cm4,emmc-drive=emmc,emmc-cache-size=1K,"
        "emmc-cache-power-loss-on-reset=on "
        "-drive if=none,id=emmc,format=raw,file=%s,file.locking=off",
        emmc_path);
    qts = qtest_init(command);
    raspi4_sdhci_initialize_emmc(qts);
    raspi4_emmc_read_ext_csd(qts, ext_csd);
    g_assert_cmphex(ext_csd[181], ==, 1);
    g_assert_cmphex(ext_csd[224], ==, 1);
    raspi4_emmc_switch(qts, 33, 1);
    raspi4_emmc_write_sector(qts, 0, 0x11111111);
    g_assert_cmpuint(raspi4_emmc_cache_dirty_sectors(qts), ==, 1);

    raspi4_emmc_erase(qts, 0, group_size - 512);
    g_assert_cmpuint(raspi4_emmc_cache_dirty_sectors(qts), ==, 0);
    g_assert_false(raspi4_emmc_erase_active(qts));
    g_assert_cmpuint(raspi4_emmc_erase_pending(qts), ==, 0);
    g_assert_cmpuint(raspi4_emmc_erase_completed(qts), ==, 1);
    g_assert_cmphex(raspi4_emmc_read_word(qts, 0), ==, UINT32_MAX);
    g_assert_cmphex(raspi4_emmc_read_word(qts, group_size - 512),
                    ==, UINT32_MAX);
    g_assert_cmphex(raspi4_emmc_read_word(qts, group_size),
                    ==, 0xa5a5a5a5);

    qtest_system_reset(qts);
    raspi4_sdhci_initialize_emmc(qts);
    g_assert_cmphex(raspi4_emmc_read_word(qts, 0), ==, UINT32_MAX);

    qtest_quit(qts);
    unlink(emmc_path);
}
void test_emmc_mixed_timed_reset_migration_campaign(void)
{
    enum {
        CAMPAIGN_CYCLES = 64,
        CAMPAIGN_SECTORS = 4,
    };
    g_autofree uint8_t *media = g_malloc(SD_SIZE);
    g_autofree char *emmc_path = NULL;
    g_autofree char *command = NULL;
    QTestState *qts;

    memset(media, 0xa5, SD_SIZE);
    write_temp_image("raspi4-emmc-mixed-campaign-XXXXXX",
                     media, SD_SIZE, &emmc_path);
    command = g_strdup_printf(
        "-M raspi-cm4,emmc-drive=emmc,emmc-cache-size=2K,"
        "emmc-cache-power-loss-on-reset=on,"
        "emmc-cache-flush-sector-delay-us=10,"
        "emmc-program-sector-delay-us=10 "
        "-drive if=none,id=emmc,format=raw,file=%s,file.locking=off",
        emmc_path);
    qts = qtest_init(command);
    raspi4_sdhci_initialize_emmc(qts);

    for (unsigned int cycle = 0; cycle < CAMPAIGN_CYCLES; cycle++) {
        uint32_t values[CAMPAIGN_SECTORS];
        uint32_t base = cycle * CAMPAIGN_SECTORS * 512;
        unsigned int mode = cycle % 3;
        unsigned int cut = cycle % (CAMPAIGN_SECTORS + 1);

        for (unsigned int sector = 0;
             sector < CAMPAIGN_SECTORS; sector++) {
            values[sector] = 0x10000000 |
                             (cycle << 16) |
                             (sector * 0x1111);
        }

        if (mode == 0) {
            raspi4_emmc_switch(qts, 33, 1);
            for (unsigned int sector = 0;
                 sector < CAMPAIGN_SECTORS; sector++) {
                raspi4_emmc_write_sector(
                    qts, base + sector * 512, values[sector]);
            }
            raspi4_emmc_switch(qts, 32, 1);
        } else {
            raspi4_emmc_write_multiple(
                qts, base, values, CAMPAIGN_SECTORS, mode == 2);
        }

        for (unsigned int completed = 0; completed < cut; completed++) {
            qtest_clock_step(qts, 9 * 1000);
            if (mode == 0) {
                g_assert_cmpuint(raspi4_emmc_cache_flush_completed(qts),
                                 ==, completed);
            } else {
                g_assert_cmpuint(raspi4_emmc_program_completed(qts),
                                 ==, completed);
            }
            qtest_clock_step(qts, 1000);
        }
        if (cut < CAMPAIGN_SECTORS) {
            qtest_clock_step(qts, 9 * 1000);
        }

        if (cycle % 8 == 7 && cut < CAMPAIGN_SECTORS) {
            g_autofree char *migration_dir = NULL;
            g_autofree char *migration_path = NULL;
            QTestState *source = qts;

            qts = migrate_to_new_qtest(source, command, &migration_dir,
                                       &migration_path);
            if (mode == 0) {
                g_assert_true(raspi4_emmc_cache_flush_active(qts));
                g_assert_cmpuint(raspi4_emmc_cache_flush_completed(qts),
                                 ==, cut);
            } else {
                g_assert_true(raspi4_emmc_program_active(qts));
                g_assert_cmpuint(raspi4_emmc_program_completed(qts),
                                 ==, cut);
            }
            qtest_quit(source);
            unlink(migration_path);
            rmdir(migration_dir);
        }

        qtest_system_reset(qts);
        g_assert_false(raspi4_emmc_cache_flush_active(qts));
        g_assert_false(raspi4_emmc_program_active(qts));
        g_assert_cmpuint(raspi4_emmc_cache_dirty_sectors(qts), ==, 0);
        g_assert_cmpuint(raspi4_emmc_program_pending(qts), ==, 0);
        raspi4_sdhci_initialize_emmc(qts);
        for (unsigned int sector = 0;
             sector < CAMPAIGN_SECTORS; sector++) {
            uint32_t expected =
                sector < cut ? values[sector] : 0xa5a5a5a5;

            g_assert_cmphex(
                raspi4_emmc_read_word(qts, base + sector * 512),
                ==, expected);
        }
    }

    qtest_quit(qts);
    unlink(emmc_path);
}
void test_emmc_reliable_write_cache_bypass_migration(void)
{
    uint8_t ext_csd[512];
    g_autofree uint8_t *media = g_malloc(SD_SIZE);
    g_autofree char *emmc_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_path = NULL;
    g_autofree char *contents = NULL;
    gsize contents_size = 0;
    QTestState *destination;
    QTestState *qts;
    uint32_t present_state;

    memset(media, 0xa5, SD_SIZE);
    write_temp_image("raspi4-emmc-reliable-XXXXXX", media, SD_SIZE,
                     &emmc_path);
    command = g_strdup_printf(
        "-M raspi-cm4,emmc-drive=emmc,emmc-cache-size=1K,"
        "emmc-cache-power-loss-on-reset=on "
        "-drive if=none,id=emmc,format=raw,file=%s,file.locking=off",
        emmc_path);
    qts = qtest_init(command);
    raspi4_sdhci_initialize_emmc(qts);
    raspi4_emmc_read_ext_csd(qts, ext_csd);
    g_assert_true(ext_csd[166] & BIT(2));
    raspi4_emmc_switch(qts, 33, 1);

    raspi4_emmc_write_sector(qts, 0, 0x44444444);
    g_assert_cmpuint(raspi4_emmc_cache_dirty_sectors(qts), ==, 1);

    qtest_writel(qts, BCM2711_EMMC2_BASE + SDHC_SYSAD, BIT(31) | 2);
    raspi4_sdhci_command(qts, BCM2711_EMMC2_BASE, 512, 2, 512,
                         SDHC_TRNS_BLK_CNT_EN | SDHC_TRNS_ACMD23 |
                         SDHC_TRNS_MULTI,
                         (25 << 8) | SDHC_CMD_DATA_PRESENT |
                         SDHC_CMD_RESPONSE);
    for (unsigned int i = 0; i < 128; i++) {
        qtest_writel(qts, BCM2711_EMMC2_BASE + SDHC_BDATA, 0x55555555);
    }
    for (unsigned int i = 0; i < 80; i++) {
        qtest_writel(qts, BCM2711_EMMC2_BASE + SDHC_BDATA, 0x66666666);
    }
    present_state = qtest_readl(qts, BCM2711_EMMC2_BASE + SDHC_PRNSTS);
    g_assert_true(present_state & SDHC_DATA_INHIBIT);
    g_assert_true(present_state & SDHC_DOING_WRITE);
    g_assert_true(g_file_get_contents(emmc_path, &contents,
                                      &contents_size, NULL));
    g_assert_cmpuint(contents_size, ==, SD_SIZE);
    g_assert_cmphex((uint8_t)contents[0], ==, 0xa5);
    g_assert_cmphex((uint8_t)contents[512], ==, 0x55);
    g_assert_cmphex((uint8_t)contents[1024], ==, 0xa5);

    destination = migrate_to_new_qtest(qts, command, &migration_dir,
                                       &migration_path);
    g_assert_cmpuint(raspi4_emmc_cache_dirty_sectors(destination), ==, 1);
    present_state = qtest_readl(destination,
                               BCM2711_EMMC2_BASE + SDHC_PRNSTS);
    g_assert_true(present_state & SDHC_DATA_INHIBIT);
    g_assert_true(present_state & SDHC_DOING_WRITE);
    for (unsigned int i = 80; i < 128; i++) {
        qtest_writel(destination, BCM2711_EMMC2_BASE + SDHC_BDATA,
                     0x66666666);
    }
    present_state = qtest_readl(destination,
                               BCM2711_EMMC2_BASE + SDHC_PRNSTS);
    g_assert_false(present_state & (SDHC_DATA_INHIBIT |
                                    SDHC_DAT_LINE_ACTIVE |
                                    SDHC_DOING_WRITE));
    g_assert_cmpuint(raspi4_emmc_cache_dirty_sectors(destination), ==, 1);
    g_clear_pointer(&contents, g_free);
    g_assert_true(g_file_get_contents(emmc_path, &contents,
                                      &contents_size, NULL));
    g_assert_cmphex((uint8_t)contents[0], ==, 0xa5);
    g_assert_cmphex((uint8_t)contents[512], ==, 0x55);
    g_assert_cmphex((uint8_t)contents[1024], ==, 0x66);
    g_assert_cmphex(raspi4_emmc_read_word(destination, 0), ==, 0x44444444);

    qtest_system_reset(destination);
    g_assert_cmpuint(raspi4_emmc_cache_dirty_sectors(destination), ==, 0);
    raspi4_sdhci_initialize_emmc(destination);
    g_assert_cmphex(raspi4_emmc_read_word(destination, 0), ==, 0xa5a5a5a5);
    g_assert_cmphex(raspi4_emmc_read_word(destination, 512), ==, 0x55555555);
    g_assert_cmphex(raspi4_emmc_read_word(destination, 1024), ==, 0x66666666);

    qtest_quit(qts);
    qtest_quit(destination);
    unlink(emmc_path);
    unlink(migration_path);
    rmdir(migration_dir);
}
void test_emmc_reliable_write_flush_error_retry(void)
{
    static const char blkdebug_config[] =
        "[inject-error]\n"
        "event = \"flush_to_disk\"\n"
        "errno = \"5\"\n"
        "once = \"on\"\n"
        "immediately = \"on\"\n";
    g_autofree uint8_t *media = g_malloc(SD_SIZE);
    g_autofree char *emmc_path = NULL;
    g_autofree char *debug_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *contents = NULL;
    gsize contents_size = 0;
    QTestState *qts;
    uint32_t status;

    memset(media, 0xa5, SD_SIZE);
    write_temp_image("raspi4-emmc-reliable-error-XXXXXX", media, SD_SIZE,
                     &emmc_path);
    write_temp_text("raspi4-emmc-reliable-debug-XXXXXX", blkdebug_config,
                    &debug_path);
    command = g_strdup_printf(
        "-M raspi-cm4,emmc-drive=emmc,emmc-cache-size=1K,"
        "emmc-cache-power-loss-on-reset=on "
        "-drive if=none,id=emmc,format=raw,"
        "file=blkdebug:%s:%s",
        debug_path, emmc_path);
    qts = qtest_init(command);
    raspi4_sdhci_initialize_emmc(qts);
    raspi4_emmc_switch(qts, 33, 1);
    raspi4_emmc_write_sector(qts, 0, 0x44444444);
    g_assert_cmpuint(raspi4_emmc_cache_dirty_sectors(qts), ==, 1);

    qtest_writel(qts, BCM2711_EMMC2_BASE + SDHC_SYSAD, BIT(31) | 1);
    raspi4_sdhci_command(qts, BCM2711_EMMC2_BASE, 512, 1, 512,
                         SDHC_TRNS_BLK_CNT_EN | SDHC_TRNS_ACMD23 |
                         SDHC_TRNS_MULTI,
                         (25 << 8) | SDHC_CMD_DATA_PRESENT |
                         SDHC_CMD_RESPONSE);
    for (unsigned int i = 0; i < 128; i++) {
        qtest_writel(qts, BCM2711_EMMC2_BASE + SDHC_BDATA, 0x55555555);
    }
    status = raspi4_emmc_status(qts);
    g_assert_true(status & BIT(19));
    g_assert_cmpuint(raspi4_emmc_cache_dirty_sectors(qts), ==, 1);

    qtest_writel(qts, BCM2711_EMMC2_BASE + SDHC_SYSAD, BIT(31) | 1);
    raspi4_sdhci_command(qts, BCM2711_EMMC2_BASE, 512, 1, 512,
                         SDHC_TRNS_BLK_CNT_EN | SDHC_TRNS_ACMD23 |
                         SDHC_TRNS_MULTI,
                         (25 << 8) | SDHC_CMD_DATA_PRESENT |
                         SDHC_CMD_RESPONSE);
    for (unsigned int i = 0; i < 128; i++) {
        qtest_writel(qts, BCM2711_EMMC2_BASE + SDHC_BDATA, 0x55555555);
    }
    status = raspi4_emmc_status(qts);
    g_assert_false(status & BIT(19));
    g_assert_cmpuint(raspi4_emmc_cache_dirty_sectors(qts), ==, 1);
    g_assert_true(g_file_get_contents(emmc_path, &contents,
                                      &contents_size, NULL));
    g_assert_cmpuint(contents_size, ==, SD_SIZE);
    g_assert_cmphex((uint8_t)contents[0], ==, 0xa5);
    g_assert_cmphex((uint8_t)contents[512], ==, 0x55);

    qtest_system_reset(qts);
    g_assert_cmpuint(raspi4_emmc_cache_dirty_sectors(qts), ==, 0);
    raspi4_sdhci_initialize_emmc(qts);
    g_assert_cmphex(raspi4_emmc_read_word(qts, 0), ==, 0xa5a5a5a5);
    g_assert_cmphex(raspi4_emmc_read_word(qts, 512), ==, 0x55555555);

    qtest_quit(qts);
    unlink(emmc_path);
    unlink(debug_path);
}
void test_bcm2711_dwc2_reset_commands(void)
{
    static const char command[] =
        "-M raspi4b -device usb-kbd,bus=usb-bus.0";
    QTestState *source = qtest_init(command);
    QTestState *destination;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    uint32_t gusbcfg;
    uint32_t reset_commands = GRSTCTL_TXFFLSH | GRSTCTL_RXFFLSH |
                              GRSTCTL_IN_TKNQ_FLSH |
                              GRSTCTL_FRMCNTRRST;

    g_assert_true(qtest_readl(source, BCM2711_DWC2_BASE + HPRT0) &
                  HPRT0_CONNSTS);
    qtest_clock_step(source, 5 * 1000 * 1000);
    g_assert_cmphex(qtest_readl(source, BCM2711_DWC2_BASE + HFNUM) &
                    HFNUM_FRNUM_MASK, !=, 0);

    qtest_writel(source, BCM2711_DWC2_BASE + HCCHAR(3), 64);
    qtest_writel(source, BCM2711_DWC2_BASE + HCTSIZ(3),
                 (1 << TSIZ_PKTCNT_SHIFT) | 64);
    qtest_writel(source, BCM2711_DWC2_BASE + HCDMA(3), DWC2_HOST_DMA);
    qtest_writel(source, BCM2711_DWC2_BASE + GRSTCTL,
                 reset_commands | GRSTCTL_TXFNUM(0x10));
    g_assert_cmphex(qtest_readl(source, BCM2711_DWC2_BASE + GRSTCTL) &
                    reset_commands, ==, 0);
    g_assert_true(qtest_readl(source, BCM2711_DWC2_BASE + GRSTCTL) &
                  GRSTCTL_AHBIDLE);
    g_assert_cmphex(qtest_readl(source, BCM2711_DWC2_BASE + HFNUM) &
                    HFNUM_FRNUM_MASK, ==, 0);
    g_assert_cmphex(qtest_readl(source, BCM2711_DWC2_BASE + HCCHAR(3)),
                    ==, 64);

    qtest_writel(source, BCM2711_DWC2_BASE + GRSTCTL, GRSTCTL_HSFTRST);
    g_assert_false(qtest_readl(source, BCM2711_DWC2_BASE + GRSTCTL) &
                   GRSTCTL_HSFTRST);
    g_assert_cmphex(qtest_readl(source, BCM2711_DWC2_BASE + HCCHAR(3)),
                    ==, 0);
    g_assert_cmphex(qtest_readl(source, BCM2711_DWC2_BASE + HCTSIZ(3)),
                    ==, 0);
    g_assert_cmphex(qtest_readl(source, BCM2711_DWC2_BASE + HCDMA(3)),
                    ==, 0);
    g_assert_true(qtest_readl(source, BCM2711_DWC2_BASE + HPRT0) &
                  HPRT0_CONNSTS);

    gusbcfg = qtest_readl(source, BCM2711_DWC2_BASE + GUSBCFG);
    qtest_writel(source, BCM2711_DWC2_BASE + GUSBCFG,
                 gusbcfg | GUSBCFG_FORCEDEVMODE);
    qtest_writel(source, BCM2711_DWC2_BASE + DCFG,
                 DCFG_DEVADDR(42) | DCFG_DEVSPD_HS);
    qtest_writel(source, BCM2711_DWC2_BASE + DIEPCTL(1),
                 DXEPCTL_EPENA | DXEPCTL_USBACTEP |
                 DXEPCTL_EPTYPE_BULK | 64);
    qtest_writel(source, BCM2711_DWC2_BASE + DOEPCTL(1),
                 DXEPCTL_EPENA | DXEPCTL_USBACTEP |
                 DXEPCTL_EPTYPE_BULK | 64);
    qtest_writel(source, BCM2711_DWC2_BASE + GRSTCTL, GRSTCTL_CSFTRST);
    g_assert_false(qtest_readl(source, BCM2711_DWC2_BASE + GRSTCTL) &
                   GRSTCTL_CSFTRST);
    g_assert_false(qtest_readl(source, BCM2711_DWC2_BASE + GINTSTS) &
                   GINTSTS_CURMODE_HOST);
    g_assert_cmphex(qtest_readl(source, BCM2711_DWC2_BASE + DCFG),
                    ==, DCFG_DEVADDR(42) | DCFG_DEVSPD_HS);
    g_assert_cmphex(qtest_readl(source, BCM2711_DWC2_BASE + DIEPCTL(1)),
                    ==, 0);
    g_assert_cmphex(qtest_readl(source, BCM2711_DWC2_BASE + DOEPCTL(1)),
                    ==, 0);

    destination = migrate_to_new_qtest(source, command, &migration_dir,
                                       &migration_socket);
    g_assert_false(qtest_readl(destination, BCM2711_DWC2_BASE + GINTSTS) &
                   GINTSTS_CURMODE_HOST);
    g_assert_cmphex(qtest_readl(destination, BCM2711_DWC2_BASE + DCFG),
                    ==, DCFG_DEVADDR(42) | DCFG_DEVSPD_HS);
    g_assert_cmphex(qtest_readl(destination, BCM2711_DWC2_BASE + DIEPCTL(1)),
                    ==, 0);
    g_assert_cmphex(qtest_readl(destination, BCM2711_DWC2_BASE + DOEPCTL(1)),
                    ==, 0);
    g_assert_true(qtest_readl(destination, BCM2711_DWC2_BASE + GRSTCTL) &
                  GRSTCTL_AHBIDLE);

    qtest_quit(source);
    qtest_quit(destination);
    unlink(migration_socket);
    rmdir(migration_dir);
}
void test_bcm2711_dwc2_device_registers(void)
{
    static const char command[] = "-M raspi4b";
    static const uint32_t bcm2711_dptxfsiz_reset[] = {
        0x02000406, 0x02000406, 0x02000406, 0x02000406,
        0x02000406, 0x03000406, 0x03000406,
    };
    QTestState *source = qtest_init(command);
    QTestState *destination;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    uint32_t gusbcfg;
    uint32_t dcfg_mask = DCFG_DESCDMA_EN | DCFG_EPMISCNT_MASK |
                         DCFG_IPG_ISOC_SUPPORDED | DCFG_PERFRINT_MASK |
                         DCFG_DEVADDR_MASK | DCFG_NZ_STS_OUT_HSHK |
                         DCFG_DEVSPD_MASK;

    g_assert_cmphex(qtest_readl(source, BCM2711_DWC2_BASE + GSNPSID), ==,
                    0x4f54280a);
    g_assert_cmphex(qtest_readl(source, BCM2711_DWC2_BASE + GHWCFG1), ==, 0);
    g_assert_cmphex(qtest_readl(source, BCM2711_DWC2_BASE + GHWCFG2), ==,
                    0x228ddd50);
    g_assert_cmphex(qtest_readl(source, BCM2711_DWC2_BASE + GHWCFG3), ==,
                    0x0ff000e8);
    g_assert_cmphex(qtest_readl(source, BCM2711_DWC2_BASE + GHWCFG4), ==,
                    0x1ff00020);
    for (unsigned int ep = 1;
         ep <= G_N_ELEMENTS(bcm2711_dptxfsiz_reset); ep++) {
        g_assert_cmphex(qtest_readl(
                            source, BCM2711_DWC2_BASE + DPTXFSIZN(ep)),
                        ==, bcm2711_dptxfsiz_reset[ep - 1]);
    }
    g_assert_true(qtest_readl(source, BCM2711_DWC2_BASE + GINTSTS) &
                  GINTSTS_CURMODE_HOST);
    gusbcfg = qtest_readl(source, BCM2711_DWC2_BASE + GUSBCFG);
    qtest_writel(source, BCM2711_DWC2_BASE + GUSBCFG,
                 gusbcfg | GUSBCFG_FORCEDEVMODE);
    g_assert_false(qtest_readl(source, BCM2711_DWC2_BASE + GINTSTS) &
                   GINTSTS_CURMODE_HOST);
    g_assert_true(qtest_readl(source, BCM2711_DWC2_BASE + PCGCTL) &
                  PCGCTL_IF_DEV_MODE);

    qtest_set_irq_in(source, DWC2_QOM_PATH, "device-connect", 0, 1);
    qtest_writel(source, BCM2711_DWC2_BASE + DCFG,
                 DCFG_DEVADDR(42) | DCFG_DEVSPD_HS);
    qtest_writel(source, BCM2711_DWC2_BASE + DOEPCTL0,
                 DXEPCTL_EPENA | DXEPCTL_USBACTEP | D0EPCTL_MPS_64);
    qtest_set_irq_in(source, DWC2_QOM_PATH, "device-reset", 0, 1);
    qtest_set_irq_in(source, DWC2_QOM_PATH, "device-reset", 0, 0);
    g_assert_true(qtest_readl(source, BCM2711_DWC2_BASE + GINTSTS) &
                  GINTSTS_USBRST);
    g_assert_cmphex(qtest_readl(source, BCM2711_DWC2_BASE + DCFG) &
                    DCFG_DEVADDR_MASK, ==, 0);
    g_assert_cmphex(qtest_readl(source, BCM2711_DWC2_BASE + DOEPCTL0), ==,
                    DXEPCTL_EPENA | DXEPCTL_USBACTEP | D0EPCTL_MPS_64);
    qtest_writel(source, BCM2711_DWC2_BASE + GINTSTS, GINTSTS_USBRST);

    qtest_writel(source, BCM2711_DWC2_BASE + DCFG, UINT32_MAX);
    g_assert_cmphex(qtest_readl(source, BCM2711_DWC2_BASE + DCFG), ==,
                    dcfg_mask);
    qtest_writel(source, BCM2711_DWC2_BASE + DPTXFSIZN(1),
                 0x00800100);
    g_assert_cmphex(qtest_readl(source,
                               BCM2711_DWC2_BASE + DPTXFSIZN(1)), ==,
                    0x00800100);

    qtest_writel(source, BCM2711_DWC2_BASE + DCTL,
                 DCTL_SGOUTNAK | DCTL_SGNPINNAK);
    g_assert_cmphex(qtest_readl(source, BCM2711_DWC2_BASE + DCTL) &
                    (DCTL_GOUTNAKSTS | DCTL_GNPINNAKSTS), ==,
                    DCTL_GOUTNAKSTS | DCTL_GNPINNAKSTS);
    qtest_writel(source, BCM2711_DWC2_BASE + DCTL,
                 DCTL_CGOUTNAK | DCTL_CGNPINNAK);
    g_assert_cmphex(qtest_readl(source, BCM2711_DWC2_BASE + DCTL) &
                    (DCTL_GOUTNAKSTS | DCTL_GNPINNAKSTS), ==, 0);

    qtest_writel(source, BCM2711_DWC2_BASE + DIEPMSK,
                 DIEPMSK_EPDISBLDMSK);
    qtest_writel(source, BCM2711_DWC2_BASE + DAINTMSK, DAINT_INEP(0));
    qtest_writel(source, BCM2711_DWC2_BASE + DIEPCTL0,
                 DXEPCTL_EPENA | DXEPCTL_USBACTEP | D0EPCTL_MPS_64);
    qtest_writel(source, BCM2711_DWC2_BASE + DIEPCTL0, DXEPCTL_EPDIS);
    g_assert_true(qtest_readl(source, BCM2711_DWC2_BASE + DIEPINT(0)) &
                  DXEPINT_EPDISBLD);
    g_assert_true(qtest_readl(source, BCM2711_DWC2_BASE + DAINT) &
                  DAINT_INEP(0));
    g_assert_true(qtest_readl(source, BCM2711_DWC2_BASE + GINTSTS) &
                  GINTSTS_IEPINT);
    qtest_writel(source, BCM2711_DWC2_BASE + DIEPINT(0),
                 DXEPINT_EPDISBLD);
    g_assert_false(qtest_readl(source, BCM2711_DWC2_BASE + DAINT) &
                   DAINT_INEP(0));
    g_assert_false(qtest_readl(source, BCM2711_DWC2_BASE + GINTSTS) &
                   GINTSTS_IEPINT);

    destination = migrate_to_new_qtest(source, command, &migration_dir,
                                       &migration_socket);
    g_assert_false(qtest_readl(destination, BCM2711_DWC2_BASE + GINTSTS) &
                   GINTSTS_CURMODE_HOST);
    g_assert_cmphex(qtest_readl(destination, BCM2711_DWC2_BASE + DCFG), ==,
                    dcfg_mask);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + DPTXFSIZN(1)), ==,
                    0x00800100);

    qtest_writel(destination, BCM2711_DWC2_BASE + DCFG,
                 DCFG_DEVADDR(17) | DCFG_DEVSPD_HS);
    qtest_set_irq_in(destination, DWC2_QOM_PATH, "device-reset", 0, 1);
    qtest_set_irq_in(destination, DWC2_QOM_PATH, "device-reset", 0, 0);
    g_assert_true(qtest_readl(destination, BCM2711_DWC2_BASE + GINTSTS) &
                  GINTSTS_USBRST);
    g_assert_cmphex(qtest_readl(destination, BCM2711_DWC2_BASE + DCFG) &
                    DCFG_DEVADDR_MASK, ==, 0);
    qtest_set_irq_in(destination, DWC2_QOM_PATH, "device-connect", 0, 0);
    g_assert_true(qtest_readl(destination, BCM2711_DWC2_BASE + GINTSTS) &
                  GINTSTS_DISCONNINT);

    gusbcfg = qtest_readl(destination, BCM2711_DWC2_BASE + GUSBCFG);
    qtest_writel(destination, BCM2711_DWC2_BASE + GUSBCFG,
                 (gusbcfg & ~GUSBCFG_FORCEDEVMODE) |
                 GUSBCFG_FORCEHOSTMODE);
    g_assert_true(qtest_readl(destination, BCM2711_DWC2_BASE + GINTSTS) &
                  GINTSTS_CURMODE_HOST);
    qtest_system_reset(destination);
    g_assert_cmphex(qtest_readl(destination, BCM2711_DWC2_BASE + DCFG),
                    ==, 0);
    for (unsigned int ep = 1;
         ep <= G_N_ELEMENTS(bcm2711_dptxfsiz_reset); ep++) {
        g_assert_cmphex(qtest_readl(
                            destination, BCM2711_DWC2_BASE + DPTXFSIZN(ep)),
                        ==, bcm2711_dptxfsiz_reset[ep - 1]);
    }

    qtest_quit(source);
    qtest_quit(destination);
    unlink(migration_socket);
    rmdir(migration_dir);
}
#ifndef _WIN32
void test_bcm2711_dwc2_device_transport(void)
{
    static const uint8_t setup_packet[] = {
        0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x12, 0x00,
    };
    static const uint8_t out_packet[] = {
        0x52, 0x50, 0x49, 0x42, 0x4f, 0x4f, 0x54, 0x00,
    };
    static const uint8_t in_packet[] = {
        0x44, 0x57, 0x43, 0x32, 0x2d, 0x49, 0x4e,
    };
    uint8_t actual[sizeof(setup_packet)];
    g_autofree char *command = NULL;
    uint32_t gusbcfg;
    int sockets[2];
    QTestState *qts;

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    command = g_strdup_printf(
        "-M raspi4b -chardev socket,id=dwc2dev,fd=%d "
        "-global dwc2-usb.device-chardev=dwc2dev", sockets[1]);
    qts = qtest_init(command);
    close(sockets[1]);

    gusbcfg = qtest_readl(qts, BCM2711_DWC2_BASE + GUSBCFG);
    qtest_writel(qts, BCM2711_DWC2_BASE + GUSBCFG,
                 gusbcfg | GUSBCFG_FORCEDEVMODE);
    qtest_writel(qts, BCM2711_DWC2_BASE + GAHBCFG, GAHBCFG_DMA_EN);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_STATE, 0, 0,
                        NULL, 0, NULL, 0), ==,
                    DWC2_DEVICE_TRANSPORT_STATE_PULLUP);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_STATE, 1, 0,
                        NULL, 0, NULL, 0), ==, -EINVAL);

    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_CONNECT, 0, 0,
                        NULL, 0, NULL, 0), ==, 0);
    qtest_writel(qts, BCM2711_DWC2_BASE + DCFG,
                 DCFG_DEVADDR(23) | DCFG_DEVSPD_HS);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_RESET, 0, 0,
                        NULL, 0, NULL, 0), ==, 0);
    g_assert_true(qtest_readl(qts, BCM2711_DWC2_BASE + GINTSTS) &
                  GINTSTS_USBRST);
    g_assert_cmphex(qtest_readl(qts, BCM2711_DWC2_BASE + DCFG) &
                    DCFG_DEVADDR_MASK, ==, 0);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_ENUM_DONE, 0,
                        DSTS_ENUMSPD_HS, NULL, 0, NULL, 0), ==, 0);
    g_assert_true(qtest_readl(qts, BCM2711_DWC2_BASE + GINTSTS) &
                  GINTSTS_ENUMDONE);

    qtest_writel(qts, BCM2711_DWC2_BASE + DOEPMSK,
                 DOEPMSK_XFERCOMPLMSK | DOEPMSK_SETUPMSK);
    qtest_writel(qts, BCM2711_DWC2_BASE + DAINTMSK,
                 DAINT_OUTEP(0) | DAINT_OUTEP(1) | DAINT_INEP(1));

    qtest_writel(qts, BCM2711_DWC2_BASE + DOEPDMA(1), 0x120000);
    qtest_writel(qts, BCM2711_DWC2_BASE + DOEPTSIZ(1),
                 DXEPTSIZ_PKTCNT(1) |
                 DXEPTSIZ_XFERSIZE(sizeof(out_packet)));
    qtest_writel(qts, BCM2711_DWC2_BASE + DOEPCTL(1),
                 DXEPCTL_EPENA | DXEPCTL_USBACTEP |
                 DXEPCTL_EPTYPE_BULK | DXEPCTL_MPS(64));
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_OUT, 1, 0,
                        out_packet, sizeof(out_packet), NULL, 0), ==,
                    sizeof(out_packet));
    qtest_memread(qts, 0x120000, actual, sizeof(out_packet));
    g_assert_cmpmem(actual, sizeof(out_packet),
                    out_packet, sizeof(out_packet));
    g_assert_cmphex(qtest_readl(qts, BCM2711_DWC2_BASE + DOEPDMA(1)), ==,
                    0x120000 + sizeof(out_packet));
    g_assert_true(qtest_readl(qts, BCM2711_DWC2_BASE + DOEPINT(1)) &
                  DXEPINT_XFERCOMPL);
    g_assert_true(qtest_readl(qts, BCM2711_DWC2_BASE + GINTSTS) &
                  GINTSTS_OEPINT);

    qtest_writel(qts, BCM2711_DWC2_BASE + DOEPDMA(0), 0x121000);
    qtest_writel(qts, BCM2711_DWC2_BASE + DOEPTSIZ0,
                 DOEPTSIZ0_PKTCNT | sizeof(setup_packet));
    qtest_writel(qts, BCM2711_DWC2_BASE + DOEPCTL0,
                 DXEPCTL_EPENA | DXEPCTL_USBACTEP | D0EPCTL_MPS_64);
    qtest_writel(qts, BCM2711_DWC2_BASE + GINTSTS,
                 GINTSTS_USBRST | GINTSTS_ENUMDONE);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_STATE, 0, 0,
                        NULL, 0, NULL, 0), ==,
                    DWC2_DEVICE_TRANSPORT_STATE_PULLUP |
                    DWC2_DEVICE_TRANSPORT_STATE_EP0_SETUP);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_SETUP, 0, 0,
                        setup_packet, sizeof(setup_packet), NULL, 0), ==,
                    sizeof(setup_packet));
    qtest_memread(qts, 0x121000, actual, sizeof(setup_packet));
    g_assert_cmpmem(actual, sizeof(setup_packet),
                    setup_packet, sizeof(setup_packet));
    g_assert_true(qtest_readl(qts, BCM2711_DWC2_BASE + DOEPINT(0)) &
                  DXEPINT_SETUP);

    qtest_memwrite(qts, 0x122000, in_packet, sizeof(in_packet));
    qtest_writel(qts, BCM2711_DWC2_BASE + DIEPMSK,
                 DIEPMSK_XFERCOMPLMSK);
    qtest_writel(qts, BCM2711_DWC2_BASE + DIEPDMA(1), 0x122000);
    qtest_writel(qts, BCM2711_DWC2_BASE + DIEPTSIZ(1),
                 DXEPTSIZ_PKTCNT(1) |
                 DXEPTSIZ_XFERSIZE(sizeof(in_packet)));
    qtest_writel(qts, BCM2711_DWC2_BASE + DIEPCTL(1),
                 DXEPCTL_EPENA | DXEPCTL_USBACTEP |
                 DXEPCTL_EPTYPE_BULK | DXEPCTL_MPS(64));
    memset(actual, 0, sizeof(actual));
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_IN, 1,
                        sizeof(actual), NULL, 0, actual, sizeof(actual)), ==,
                    sizeof(in_packet));
    g_assert_cmpmem(actual, sizeof(in_packet),
                    in_packet, sizeof(in_packet));
    g_assert_true(qtest_readl(qts, BCM2711_DWC2_BASE + DIEPINT(1)) &
                  DXEPINT_XFERCOMPL);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_IN, 1,
                        sizeof(actual), NULL, 0, actual, sizeof(actual)), ==,
                    -EAGAIN);

    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_DISCONNECT, 0, 0,
                        NULL, 0, NULL, 0), ==, 0);
    g_assert_true(qtest_readl(qts, BCM2711_DWC2_BASE + GINTSTS) &
                  GINTSTS_DISCONNINT);

    qtest_quit(qts);
    close(sockets[0]);
}
#endif
#ifndef _WIN32
void test_bcm2711_dwc2_device_pio_fifo(void)
{
    static const uint8_t migrated_setup[] = {
        0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x12, 0x00,
    };
    static const uint8_t out_packet[] = {
        0x50, 0x49, 0x4f, 0x2d, 0x4f, 0x55, 0x54,
    };
    static const uint8_t in_packet[] = {
        0x50, 0x49, 0x4f, 0x2d, 0x49, 0x4e,
    };
    static const uint8_t migrated_out[] = {
        0xa1, 0xa2, 0xa3, 0xa4, 0xa5,
    };
    static const uint8_t migrated_in[] = {
        0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    };
    uint8_t full_packet[64];
    uint8_t short_packet[7];
    uint8_t word_bytes[8] = { 0 };
    uint8_t actual[64] = { 0 };
    uint8_t request[DWC2_DEVICE_TRANSPORT_HEADER_SIZE +
                    sizeof(migrated_setup)] = { 0 };
    uint8_t response[DWC2_DEVICE_TRANSPORT_HEADER_SIZE];
    g_autofree char *source_command = NULL;
    g_autofree char *destination_command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    uint32_t status;
    int source_transport[2];
    int destination_transport[2];
    QTestState *source;
    QTestState *destination;

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0,
                               source_transport), ==, 0);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0,
                               destination_transport), ==, 0);
    source_command = g_strdup_printf(
        "-M raspi4b -chardev socket,id=dwc2dev,fd=%d "
        "-global dwc2-usb.device-chardev=dwc2dev", source_transport[1]);
    destination_command = g_strdup_printf(
        "-M raspi4b -chardev socket,id=dwc2dev,fd=%d "
        "-global dwc2-usb.device-chardev=dwc2dev",
        destination_transport[1]);
    source = qtest_init(source_command);
    close(source_transport[1]);

    qtest_writel(source, BCM2711_DWC2_BASE + GUSBCFG,
                 qtest_readl(source, BCM2711_DWC2_BASE + GUSBCFG) |
                 GUSBCFG_FORCEDEVMODE);
    g_assert_cmphex(qtest_readl(source, BCM2711_DWC2_BASE + GAHBCFG) &
                    GAHBCFG_DMA_EN, ==, 0);
    g_assert_cmpint(dwc2_transport_exchange(
                        source_transport[0],
                        DWC2_DEVICE_TRANSPORT_CONNECT, 0, 0,
                        NULL, 0, NULL, 0), ==, 0);

    qtest_writel(source, BCM2711_DWC2_BASE + GRXFSIZ, 16);
    qtest_writel(source, BCM2711_DWC2_BASE + GNPTXFSIZ,
                 4 << FIFOSIZE_DEPTH_SHIFT);
    qtest_writel(source, BCM2711_DWC2_BASE + DPTXFSIZN(1),
                 (4 << FIFOSIZE_DEPTH_SHIFT) | 4);

    qtest_writel(source, BCM2711_DWC2_BASE + DOEPTSIZ(1),
                 DXEPTSIZ_PKTCNT(1) |
                 DXEPTSIZ_XFERSIZE(sizeof(out_packet)));
    qtest_writel(source, BCM2711_DWC2_BASE + DOEPCTL(1),
                 DXEPCTL_EPENA | DXEPCTL_USBACTEP |
                 DXEPCTL_EPTYPE_BULK | DXEPCTL_MPS(64));
    g_assert_cmpint(dwc2_transport_exchange(
                        source_transport[0], DWC2_DEVICE_TRANSPORT_OUT,
                        1, 0, out_packet, sizeof(out_packet), NULL, 0), ==,
                    sizeof(out_packet));
    status = 1 | (sizeof(out_packet) << GRXSTS_BYTECNT_SHIFT) |
             (GRXSTS_PKTSTS_OUTRX << GRXSTS_PKTSTS_SHIFT);
    g_assert_cmphex(qtest_readl(source,
                               BCM2711_DWC2_BASE + GRXSTSR), ==, status);
    g_assert_true(qtest_readl(source, BCM2711_DWC2_BASE + GINTSTS) &
                  GINTSTS_RXFLVL);
    g_assert_cmphex(qtest_readl(source,
                               BCM2711_DWC2_BASE + GRXSTSP), ==, status);
    memcpy(word_bytes, out_packet, sizeof(out_packet));
    g_assert_cmphex(qtest_readl(source,
                               BCM2711_DWC2_BASE + EPFIFO(0)), ==,
                    (uint32_t)ldl_le_p(word_bytes));
    g_assert_cmphex(qtest_readl(source,
                               BCM2711_DWC2_BASE + EPFIFO(0)), ==,
                    (uint32_t)ldl_le_p(word_bytes + 4));
    status = 1 | (GRXSTS_PKTSTS_OUTDONE << GRXSTS_PKTSTS_SHIFT);
    g_assert_cmphex(qtest_readl(source,
                               BCM2711_DWC2_BASE + GRXSTSP), ==, status);
    g_assert_false(qtest_readl(source, BCM2711_DWC2_BASE + GINTSTS) &
                   GINTSTS_RXFLVL);
    g_assert_cmphex(qtest_readl(source, BCM2711_DWC2_BASE + DOEPDMA(1)),
                    ==, 0);

    qtest_writel(source, BCM2711_DWC2_BASE + DIEPMSK,
                 DIEPMSK_XFERCOMPLMSK | DIEPMSK_TXFIFOEMPTY);
    qtest_writel(source, BCM2711_DWC2_BASE + DIEPEMPMSK, BIT(1));
    qtest_writel(source, BCM2711_DWC2_BASE + DAINTMSK, DAINT_INEP(1));
    qtest_writel(source, BCM2711_DWC2_BASE + DIEPTSIZ(1),
                 DXEPTSIZ_PKTCNT(1) |
                 DXEPTSIZ_XFERSIZE(sizeof(in_packet)));
    qtest_writel(source, BCM2711_DWC2_BASE + DIEPCTL(1),
                 DXEPCTL_EPENA | DXEPCTL_USBACTEP |
                 DXEPCTL_EPTYPE_BULK | DXEPCTL_MPS(64));
    memset(word_bytes, 0, sizeof(word_bytes));
    memcpy(word_bytes, in_packet, sizeof(in_packet));
    qtest_writel(source, BCM2711_DWC2_BASE + EPFIFO(1),
                 ldl_le_p(word_bytes));
    qtest_writel(source, BCM2711_DWC2_BASE + EPFIFO(1),
                 ldl_le_p(word_bytes + 4));
    g_assert_cmphex(qtest_readl(source,
                               BCM2711_DWC2_BASE + DTXFSTS(1)), ==, 2);
    g_assert_cmpint(dwc2_transport_exchange(
                        source_transport[0], DWC2_DEVICE_TRANSPORT_IN,
                        1, sizeof(actual), NULL, 0, actual,
                        sizeof(actual)), ==, sizeof(in_packet));
    g_assert_cmpmem(actual, sizeof(in_packet),
                    in_packet, sizeof(in_packet));
    g_assert_cmphex(qtest_readl(source,
                               BCM2711_DWC2_BASE + DTXFSTS(1)), ==, 4);
    g_assert_true(qtest_readl(source,
                             BCM2711_DWC2_BASE + DIEPINT(1)) &
                  DXEPINT_XFERCOMPL);
    g_assert_true(qtest_readl(source,
                             BCM2711_DWC2_BASE + DIEPINT(1)) &
                  DXEPINT_TXFEMP);

    qtest_writel(source, BCM2711_DWC2_BASE + DIEPTSIZ(1),
                 DXEPTSIZ_PKTCNT(1) | DXEPTSIZ_XFERSIZE(4));
    qtest_writel(source, BCM2711_DWC2_BASE + DIEPCTL(1),
                 DXEPCTL_EPENA | DXEPCTL_USBACTEP |
                 DXEPCTL_EPTYPE_BULK | DXEPCTL_MPS(64));
    qtest_writel(source, BCM2711_DWC2_BASE + EPFIFO(1), 0x44332211);
    qtest_writel(source, BCM2711_DWC2_BASE + GRSTCTL,
                 GRSTCTL_TXFFLSH | GRSTCTL_TXFNUM(1));
    g_assert_cmphex(qtest_readl(source,
                               BCM2711_DWC2_BASE + DTXFSTS(1)), ==, 4);
    g_assert_cmpint(dwc2_transport_exchange(
                        source_transport[0], DWC2_DEVICE_TRANSPORT_IN,
                        1, 4, NULL, 0, actual, sizeof(actual)), ==, -EAGAIN);

    qtest_writel(source, BCM2711_DWC2_BASE + GRXFSIZ, 1);
    qtest_writel(source, BCM2711_DWC2_BASE + DOEPTSIZ(1),
                 DXEPTSIZ_PKTCNT(1) | DXEPTSIZ_XFERSIZE(8));
    qtest_writel(source, BCM2711_DWC2_BASE + DOEPCTL(1),
                 DXEPCTL_EPENA | DXEPCTL_USBACTEP |
                 DXEPCTL_EPTYPE_BULK | DXEPCTL_MPS(64));
    g_assert_cmpint(dwc2_transport_exchange(
                        source_transport[0], DWC2_DEVICE_TRANSPORT_OUT,
                        1, 0, word_bytes, sizeof(word_bytes), NULL, 0), ==,
                    -ENOSPC);
    g_assert_true(qtest_readl(source,
                             BCM2711_DWC2_BASE + DOEPCTL(1)) &
                  DXEPCTL_EPENA);
    g_assert_false(qtest_readl(source, BCM2711_DWC2_BASE + GINTSTS) &
                   GINTSTS_RXFLVL);
    qtest_writel(source, BCM2711_DWC2_BASE + GRXFSIZ, 16);

    qtest_writel(source, BCM2711_DWC2_BASE + DOEPTSIZ(1),
                 DXEPTSIZ_PKTCNT(1) |
                 DXEPTSIZ_XFERSIZE(sizeof(migrated_out)));
    qtest_writel(source, BCM2711_DWC2_BASE + DOEPCTL(1),
                 DXEPCTL_EPENA | DXEPCTL_USBACTEP |
                 DXEPCTL_EPTYPE_BULK | DXEPCTL_MPS(64));
    g_assert_cmpint(dwc2_transport_exchange(
                        source_transport[0], DWC2_DEVICE_TRANSPORT_OUT,
                        1, 0, migrated_out, sizeof(migrated_out),
                        NULL, 0), ==, sizeof(migrated_out));
    qtest_writel(source, BCM2711_DWC2_BASE + DIEPTSIZ(1),
                 DXEPTSIZ_PKTCNT(1) |
                 DXEPTSIZ_XFERSIZE(sizeof(migrated_in)));
    qtest_writel(source, BCM2711_DWC2_BASE + DIEPCTL(1),
                 DXEPCTL_EPENA | DXEPCTL_USBACTEP |
                 DXEPCTL_EPTYPE_BULK | DXEPCTL_MPS(64));
    memset(word_bytes, 0, sizeof(word_bytes));
    memcpy(word_bytes, migrated_in, sizeof(migrated_in));
    qtest_writel(source, BCM2711_DWC2_BASE + EPFIFO(1),
                 ldl_le_p(word_bytes));
    qtest_writel(source, BCM2711_DWC2_BASE + EPFIFO(1),
                 ldl_le_p(word_bytes + 4));
    qtest_writel(source, BCM2711_DWC2_BASE + DOEPTSIZ0,
                 DOEPTSIZ0_SUPCNT(3) | DOEPTSIZ0_PKTCNT |
                 sizeof(migrated_setup) * 3);
    qtest_writel(source, BCM2711_DWC2_BASE + DOEPCTL0,
                 DXEPCTL_EPENA | DXEPCTL_USBACTEP | D0EPCTL_MPS_64);
    stl_le_p(request, DWC2_DEVICE_TRANSPORT_MAGIC);
    request[4] = DWC2_DEVICE_TRANSPORT_VERSION;
    request[5] = DWC2_DEVICE_TRANSPORT_SETUP;
    stl_le_p(request + 8, sizeof(migrated_setup));
    memcpy(request + DWC2_DEVICE_TRANSPORT_HEADER_SIZE,
           migrated_setup, sizeof(migrated_setup));
    dwc2_transport_write_all(source_transport[0], request, 9);

    destination = migrate_to_new_qtest_commands(
        source, destination_command, &migration_dir, &migration_socket);
    close(destination_transport[1]);
    dwc2_transport_write_all(
        destination_transport[0], request + 9, sizeof(request) - 9);
    dwc2_transport_read_all(destination_transport[0], response,
                            sizeof(response));
    g_assert_cmphex(ldl_le_p(response), ==,
                    DWC2_DEVICE_TRANSPORT_MAGIC);
    g_assert_cmpuint(response[5], ==,
                     DWC2_DEVICE_TRANSPORT_SETUP |
                     DWC2_DEVICE_TRANSPORT_RESPONSE);
    g_assert_cmpint((int32_t)ldl_le_p(response + 12), ==,
                    sizeof(migrated_setup));

    status = 1 | (sizeof(migrated_out) << GRXSTS_BYTECNT_SHIFT) |
             (GRXSTS_PKTSTS_OUTRX << GRXSTS_PKTSTS_SHIFT);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + GRXSTSP), ==, status);
    memset(word_bytes, 0, sizeof(word_bytes));
    memcpy(word_bytes, migrated_out, sizeof(migrated_out));
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + EPFIFO(0)), ==,
                    (uint32_t)ldl_le_p(word_bytes));
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + EPFIFO(0)), ==,
                    (uint32_t)ldl_le_p(word_bytes + 4));
    status = 1 | (GRXSTS_PKTSTS_OUTDONE << GRXSTS_PKTSTS_SHIFT);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + GRXSTSP), ==, status);
    status = (sizeof(migrated_setup) << GRXSTS_BYTECNT_SHIFT) |
             (GRXSTS_PKTSTS_SETUPRX << GRXSTS_PKTSTS_SHIFT);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + GRXSTSP), ==, status);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + EPFIFO(0)), ==,
                    ldl_le_p(migrated_setup));
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + EPFIFO(0)), ==,
                    ldl_le_p(migrated_setup + 4));
    status = GRXSTS_PKTSTS_SETUPDONE << GRXSTS_PKTSTS_SHIFT;
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + GRXSTSP), ==, status);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + DOEPTSIZ0) &
                    (DOEPTSIZ0_SUPCNT_MASK |
                     DOEPTSIZ0_XFERSIZE_MASK), ==,
                    DOEPTSIZ0_SUPCNT(2) |
                    sizeof(migrated_setup) * 2);
    g_assert_true(qtest_readl(destination,
                             BCM2711_DWC2_BASE + DOEPCTL0) &
                  DXEPCTL_EPENA);
    for (unsigned int setup_index = 1; setup_index < 3; setup_index++) {
        g_assert_cmpint(dwc2_transport_exchange(
                            destination_transport[0],
                            DWC2_DEVICE_TRANSPORT_SETUP, 0, 0,
                            migrated_setup, sizeof(migrated_setup),
                            NULL, 0), ==, sizeof(migrated_setup));
        status = (sizeof(migrated_setup) << GRXSTS_BYTECNT_SHIFT) |
                 (GRXSTS_PKTSTS_SETUPRX << GRXSTS_PKTSTS_SHIFT);
        g_assert_cmphex(qtest_readl(
                            destination, BCM2711_DWC2_BASE + GRXSTSP),
                        ==, status);
        g_assert_cmphex(qtest_readl(
                            destination, BCM2711_DWC2_BASE + EPFIFO(0)),
                        ==, ldl_le_p(migrated_setup));
        g_assert_cmphex(qtest_readl(
                            destination, BCM2711_DWC2_BASE + EPFIFO(0)),
                        ==, ldl_le_p(migrated_setup + 4));
        status = GRXSTS_PKTSTS_SETUPDONE << GRXSTS_PKTSTS_SHIFT;
        g_assert_cmphex(qtest_readl(
                            destination, BCM2711_DWC2_BASE + GRXSTSP),
                        ==, status);
        g_assert_cmphex(qtest_readl(
                            destination, BCM2711_DWC2_BASE + DOEPTSIZ0) &
                        (DOEPTSIZ0_SUPCNT_MASK |
                         DOEPTSIZ0_XFERSIZE_MASK), ==,
                        DOEPTSIZ0_SUPCNT(2 - setup_index) |
                        sizeof(migrated_setup) * (2 - setup_index));
        g_assert_cmpint(!!(qtest_readl(
                              destination,
                              BCM2711_DWC2_BASE + DOEPCTL0) &
                          DXEPCTL_EPENA), ==, setup_index != 2);
    }

    qtest_writel(destination, BCM2711_DWC2_BASE + DIEPINT(0), UINT32_MAX);
    qtest_writel(destination, BCM2711_DWC2_BASE + DIEPTSIZ0,
                 DIEPTSIZ0_PKTCNT(1));
    qtest_writel(destination, BCM2711_DWC2_BASE + DIEPCTL0,
                 DXEPCTL_EPENA | DXEPCTL_USBACTEP | D0EPCTL_MPS_64);
    g_assert_cmpint(dwc2_transport_exchange(
                        destination_transport[0],
                        DWC2_DEVICE_TRANSPORT_IN, 0, 64,
                        NULL, 0, NULL, 0), ==, 0);
    g_assert_false(qtest_readl(destination,
                              BCM2711_DWC2_BASE + DIEPCTL0) &
                   DXEPCTL_EPENA);
    g_assert_true(qtest_readl(destination,
                             BCM2711_DWC2_BASE + DIEPINT(0)) &
                  DXEPINT_XFERCOMPL);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + DIEPTSIZ0) &
                    (DIEPTSIZ0_PKTCNT_MASK |
                     DIEPTSIZ0_XFERSIZE_MASK), ==, 0);

    qtest_writel(destination, BCM2711_DWC2_BASE + DOEPINT(0), UINT32_MAX);
    qtest_writel(destination, BCM2711_DWC2_BASE + DOEPTSIZ0,
                 DOEPTSIZ0_PKTCNT);
    qtest_writel(destination, BCM2711_DWC2_BASE + DOEPCTL0,
                 DXEPCTL_EPENA | DXEPCTL_USBACTEP | D0EPCTL_MPS_64);
    g_assert_cmpint(dwc2_transport_exchange(
                        destination_transport[0],
                        DWC2_DEVICE_TRANSPORT_OUT, 0, 0,
                        NULL, 0, NULL, 0), ==, 0);
    status = GRXSTS_PKTSTS_OUTRX << GRXSTS_PKTSTS_SHIFT;
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + GRXSTSP), ==, status);
    status = GRXSTS_PKTSTS_OUTDONE << GRXSTS_PKTSTS_SHIFT;
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + GRXSTSP), ==, status);
    g_assert_false(qtest_readl(destination,
                              BCM2711_DWC2_BASE + DOEPCTL0) &
                   DXEPCTL_EPENA);
    g_assert_true(qtest_readl(destination,
                             BCM2711_DWC2_BASE + DOEPINT(0)) &
                  DXEPINT_XFERCOMPL);

    for (unsigned int i = 0; i < sizeof(full_packet); i++) {
        full_packet[i] = 0xc0 ^ i;
    }
    for (unsigned int i = 0; i < sizeof(short_packet); i++) {
        short_packet[i] = 0xd0 ^ i;
    }
    qtest_writel(destination, BCM2711_DWC2_BASE + DOEPTSIZ(2),
                 DXEPTSIZ_PKTCNT(2) | DXEPTSIZ_XFERSIZE(128));
    qtest_writel(destination, BCM2711_DWC2_BASE + DOEPCTL(2),
                 DXEPCTL_EPENA | DXEPCTL_USBACTEP |
                 DXEPCTL_EPTYPE_BULK | DXEPCTL_MPS(64));
    g_assert_cmpint(dwc2_transport_exchange(
                        destination_transport[0],
                        DWC2_DEVICE_TRANSPORT_OUT, 2, 0,
                        full_packet, sizeof(full_packet), NULL, 0), ==,
                    sizeof(full_packet));
    status = 2 | (sizeof(full_packet) << GRXSTS_BYTECNT_SHIFT) |
             (GRXSTS_PKTSTS_OUTRX << GRXSTS_PKTSTS_SHIFT);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + GRXSTSP), ==, status);
    for (unsigned int word = 0; word < sizeof(full_packet) / 4; word++) {
        g_assert_cmphex(qtest_readl(
                            destination, BCM2711_DWC2_BASE + EPFIFO(0)),
                        ==, (uint32_t)ldl_le_p(full_packet + word * 4));
    }
    g_assert_true(qtest_readl(destination,
                             BCM2711_DWC2_BASE + DOEPCTL(2)) &
                  DXEPCTL_EPENA);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + DOEPTSIZ(2)) &
                    (DXEPTSIZ_PKTCNT_MASK | DXEPTSIZ_XFERSIZE_MASK), ==,
                    DXEPTSIZ_PKTCNT(1) | DXEPTSIZ_XFERSIZE(64));
    g_assert_cmpint(dwc2_transport_exchange(
                        destination_transport[0],
                        DWC2_DEVICE_TRANSPORT_OUT, 2, 0,
                        short_packet, sizeof(short_packet), NULL, 0), ==,
                    sizeof(short_packet));
    status = 2 | (sizeof(short_packet) << GRXSTS_BYTECNT_SHIFT) |
             (GRXSTS_PKTSTS_OUTRX << GRXSTS_PKTSTS_SHIFT);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + GRXSTSP), ==, status);
    memset(word_bytes, 0, sizeof(word_bytes));
    memcpy(word_bytes, short_packet, sizeof(short_packet));
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + EPFIFO(0)), ==,
                    (uint32_t)ldl_le_p(word_bytes));
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + EPFIFO(0)), ==,
                    (uint32_t)ldl_le_p(word_bytes + 4));
    status = 2 | (GRXSTS_PKTSTS_OUTDONE << GRXSTS_PKTSTS_SHIFT);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + GRXSTSP), ==, status);
    g_assert_false(qtest_readl(destination,
                              BCM2711_DWC2_BASE + DOEPCTL(2)) &
                   DXEPCTL_EPENA);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + DOEPTSIZ(2)) &
                    (DXEPTSIZ_PKTCNT_MASK | DXEPTSIZ_XFERSIZE_MASK), ==,
                    DXEPTSIZ_XFERSIZE(64 - sizeof(short_packet)));

    qtest_writel(destination, BCM2711_DWC2_BASE + DPTXFSIZN(2),
                 (20 << FIFOSIZE_DEPTH_SHIFT) | 8);
    qtest_writel(destination, BCM2711_DWC2_BASE + DIEPTSIZ(2),
                 DXEPTSIZ_PKTCNT(2) |
                 DXEPTSIZ_XFERSIZE(sizeof(full_packet) +
                                   sizeof(short_packet)));
    qtest_writel(destination, BCM2711_DWC2_BASE + DIEPCTL(2),
                 DXEPCTL_EPENA | DXEPCTL_USBACTEP |
                 DXEPCTL_EPTYPE_BULK | DXEPCTL_MPS(64));
    for (unsigned int word = 0; word < sizeof(full_packet) / 4; word++) {
        qtest_writel(destination, BCM2711_DWC2_BASE + EPFIFO(2),
                     ldl_le_p(full_packet + word * 4));
    }
    qtest_writel(destination, BCM2711_DWC2_BASE + EPFIFO(2),
                 ldl_le_p(word_bytes));
    qtest_writel(destination, BCM2711_DWC2_BASE + EPFIFO(2),
                 ldl_le_p(word_bytes + 4));
    memset(actual, 0, sizeof(actual));
    g_assert_cmpint(dwc2_transport_exchange(
                        destination_transport[0],
                        DWC2_DEVICE_TRANSPORT_IN, 2, sizeof(actual),
                        NULL, 0, actual, sizeof(actual)), ==,
                    sizeof(full_packet));
    g_assert_cmpmem(actual, sizeof(full_packet),
                    full_packet, sizeof(full_packet));
    g_assert_true(qtest_readl(destination,
                             BCM2711_DWC2_BASE + DIEPCTL(2)) &
                  DXEPCTL_EPENA);
    memset(actual, 0, sizeof(actual));
    g_assert_cmpint(dwc2_transport_exchange(
                        destination_transport[0],
                        DWC2_DEVICE_TRANSPORT_IN, 2, sizeof(actual),
                        NULL, 0, actual, sizeof(actual)), ==,
                    sizeof(short_packet));
    g_assert_cmpmem(actual, sizeof(short_packet),
                    short_packet, sizeof(short_packet));
    g_assert_false(qtest_readl(destination,
                              BCM2711_DWC2_BASE + DIEPCTL(2)) &
                   DXEPCTL_EPENA);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + DIEPTSIZ(2)) &
                    (DXEPTSIZ_PKTCNT_MASK | DXEPTSIZ_XFERSIZE_MASK), ==, 0);

    memset(actual, 0, sizeof(actual));
    g_assert_cmpint(dwc2_transport_exchange(
                        destination_transport[0],
                        DWC2_DEVICE_TRANSPORT_IN, 1, sizeof(actual),
                        NULL, 0, actual, sizeof(actual)), ==,
                    sizeof(migrated_in));
    g_assert_cmpmem(actual, sizeof(migrated_in),
                    migrated_in, sizeof(migrated_in));

    qtest_writel(destination, BCM2711_DWC2_BASE + DOEPTSIZ(1),
                 DXEPTSIZ_PKTCNT(1) | DXEPTSIZ_XFERSIZE(4));
    qtest_writel(destination, BCM2711_DWC2_BASE + DOEPCTL(1),
                 DXEPCTL_EPENA | DXEPCTL_USBACTEP |
                 DXEPCTL_EPTYPE_BULK | DXEPCTL_MPS(64));
    g_assert_cmpint(dwc2_transport_exchange(
                        destination_transport[0],
                        DWC2_DEVICE_TRANSPORT_OUT, 1, 0,
                        word_bytes, 4, NULL, 0), ==, 4);
    qtest_writel(destination, BCM2711_DWC2_BASE + GRSTCTL,
                 GRSTCTL_RXFFLSH);
    g_assert_false(qtest_readl(destination, BCM2711_DWC2_BASE + GINTSTS) &
                   GINTSTS_RXFLVL);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + GRXSTSP), ==, 0);

    qtest_quit(source);
    qtest_quit(destination);
    close(source_transport[0]);
    close(destination_transport[0]);
    unlink(migration_socket);
    rmdir(migration_dir);
}
#endif
#ifndef _WIN32
void test_bcm2711_dwc2_device_pio_reset_storm(void)
{
    uint8_t setup_packet[8];
    uint8_t out_packet[2][4];
    uint8_t in_packet[2][4];
    uint8_t actual[4];
    g_autofree char *command = NULL;
    uint32_t status;
    int sockets[2];
    QTestState *qts;

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    command = g_strdup_printf(
        "-M raspi4b -chardev socket,id=dwc2dev,fd=%d "
        "-global dwc2-usb.device-chardev=dwc2dev", sockets[1]);
    qts = qtest_init(command);
    close(sockets[1]);

    qtest_writel(qts, BCM2711_DWC2_BASE + GUSBCFG,
                 qtest_readl(qts, BCM2711_DWC2_BASE + GUSBCFG) |
                 GUSBCFG_FORCEDEVMODE);
    qtest_writel(qts, BCM2711_DWC2_BASE + GRXFSIZ, 32);
    qtest_writel(qts, BCM2711_DWC2_BASE + DPTXFSIZN(1),
                 (4 << FIFOSIZE_DEPTH_SHIFT) | 4);
    qtest_writel(qts, BCM2711_DWC2_BASE + DPTXFSIZN(2),
                 (4 << FIFOSIZE_DEPTH_SHIFT) | 8);
    g_assert_cmpint(dwc2_transport_exchange(
                        sockets[0], DWC2_DEVICE_TRANSPORT_CONNECT, 0, 0,
                        NULL, 0, NULL, 0), ==, 0);

    for (unsigned int cycle = 0; cycle < 16; cycle++) {
        for (unsigned int index = 0; index < 2; index++) {
            out_packet[index][0] = 0x40 + cycle;
            out_packet[index][1] = 0x50 + index;
            out_packet[index][2] = cycle;
            out_packet[index][3] = index;
            in_packet[index][0] = 0x80 + cycle;
            in_packet[index][1] = 0x90 + index;
            in_packet[index][2] = cycle;
            in_packet[index][3] = index;

            qtest_writel(qts, BCM2711_DWC2_BASE + DOEPTSIZ(index + 1),
                         DXEPTSIZ_PKTCNT(1) | DXEPTSIZ_XFERSIZE(4));
            qtest_writel(qts, BCM2711_DWC2_BASE + DOEPCTL(index + 1),
                         DXEPCTL_EPENA | DXEPCTL_USBACTEP |
                         DXEPCTL_EPTYPE_BULK | DXEPCTL_MPS(64));
            qtest_writel(qts, BCM2711_DWC2_BASE + DIEPTSIZ(index + 1),
                         DXEPTSIZ_PKTCNT(1) | DXEPTSIZ_XFERSIZE(4));
            qtest_writel(qts, BCM2711_DWC2_BASE + DIEPCTL(index + 1),
                         DXEPCTL_EPENA | DXEPCTL_USBACTEP |
                         DXEPCTL_EPTYPE_BULK | DXEPCTL_MPS(64));
            qtest_writel(qts, BCM2711_DWC2_BASE + EPFIFO(index + 1),
                         ldl_le_p(in_packet[index]));
        }

        qtest_writel(qts, BCM2711_DWC2_BASE + DCFG,
                     DCFG_DEVADDR(cycle + 1) | DCFG_DEVSPD_HS);
        qtest_writel(qts, BCM2711_DWC2_BASE + DCTL,
                     DCTL_SGOUTNAK | DCTL_SGNPINNAK);
        g_assert_cmphex(qtest_readl(qts, BCM2711_DWC2_BASE + DCTL) &
                        (DCTL_GOUTNAKSTS | DCTL_GNPINNAKSTS), ==,
                        DCTL_GOUTNAKSTS | DCTL_GNPINNAKSTS);
        g_assert_cmpint(dwc2_transport_exchange(
                            sockets[0], DWC2_DEVICE_TRANSPORT_OUT, 1, 0,
                            out_packet[0], sizeof(out_packet[0]),
                            NULL, 0), ==, -EAGAIN);
        g_assert_cmpint(dwc2_transport_exchange(
                            sockets[0], DWC2_DEVICE_TRANSPORT_IN, 2,
                            sizeof(actual), NULL, 0, actual,
                            sizeof(actual)), ==, -EAGAIN);

        g_assert_cmpint(dwc2_transport_exchange(
                            sockets[0], DWC2_DEVICE_TRANSPORT_RESET, 0, 0,
                            NULL, 0, NULL, 0), ==, 0);
        g_assert_cmphex(qtest_readl(qts, BCM2711_DWC2_BASE + DCTL) &
                        (DCTL_GOUTNAKSTS | DCTL_GNPINNAKSTS), ==, 0);
        g_assert_cmphex(qtest_readl(qts, BCM2711_DWC2_BASE + DCFG) &
                        DCFG_DEVADDR_MASK, ==, 0);
        g_assert_cmphex(qtest_readl(qts, BCM2711_DWC2_BASE + GINTSTS) &
                        (GINTSTS_RXFLVL | GINTSTS_IEPINT |
                         GINTSTS_OEPINT | GINTSTS_ENUMDONE), ==, 0);
        for (unsigned int ep = 1; ep <= 2; ep++) {
            g_assert_cmphex(qtest_readl(
                                qts, BCM2711_DWC2_BASE + DIEPCTL(ep)),
                            ==, DXEPCTL_EPENA | DXEPCTL_USBACTEP |
                                DXEPCTL_EPTYPE_BULK | DXEPCTL_MPS(64));
            g_assert_cmphex(qtest_readl(
                                qts, BCM2711_DWC2_BASE + DOEPCTL(ep)),
                            ==, DXEPCTL_EPENA | DXEPCTL_USBACTEP |
                                DXEPCTL_EPTYPE_BULK | DXEPCTL_MPS(64));
            g_assert_cmphex(qtest_readl(
                                qts, BCM2711_DWC2_BASE + DIEPTSIZ(ep)),
                            ==, DXEPTSIZ_PKTCNT(1) |
                                DXEPTSIZ_XFERSIZE(4));
            g_assert_cmphex(qtest_readl(
                                qts, BCM2711_DWC2_BASE + DOEPTSIZ(ep)),
                            ==, DXEPTSIZ_PKTCNT(1) |
                                DXEPTSIZ_XFERSIZE(4));
            g_assert_cmphex(qtest_readl(
                                qts, BCM2711_DWC2_BASE + DTXFSTS(ep)),
                            ==, 4);
        }
        g_assert_cmpint(dwc2_transport_exchange(
                            sockets[0], DWC2_DEVICE_TRANSPORT_ENUM_DONE, 0,
                            DSTS_ENUMSPD_HS, NULL, 0, NULL, 0), ==, 0);

        for (unsigned int index = 0; index < 2; index++) {
            qtest_writel(qts, BCM2711_DWC2_BASE + DOEPTSIZ(index + 1),
                         DXEPTSIZ_PKTCNT(1) | DXEPTSIZ_XFERSIZE(4));
            qtest_writel(qts, BCM2711_DWC2_BASE + DOEPCTL(index + 1),
                         DXEPCTL_EPENA | DXEPCTL_USBACTEP |
                         DXEPCTL_EPTYPE_BULK | DXEPCTL_MPS(64));
            qtest_writel(qts, BCM2711_DWC2_BASE + DIEPTSIZ(index + 1),
                         DXEPTSIZ_PKTCNT(1) | DXEPTSIZ_XFERSIZE(4));
            qtest_writel(qts, BCM2711_DWC2_BASE + DIEPCTL(index + 1),
                         DXEPCTL_EPENA | DXEPCTL_USBACTEP |
                         DXEPCTL_EPTYPE_BULK | DXEPCTL_MPS(64));
            qtest_writel(qts, BCM2711_DWC2_BASE + EPFIFO(index + 1),
                         ldl_le_p(in_packet[index]));
        }
        setup_packet[0] = 0x80;
        setup_packet[1] = 0x06;
        setup_packet[2] = cycle;
        setup_packet[3] = 0x01;
        setup_packet[4] = 0;
        setup_packet[5] = 0;
        setup_packet[6] = 0x12;
        setup_packet[7] = 0;
        qtest_writel(qts, BCM2711_DWC2_BASE + DOEPTSIZ0,
                     DOEPTSIZ0_SUPCNT(1) | DOEPTSIZ0_PKTCNT |
                     sizeof(setup_packet));
        qtest_writel(qts, BCM2711_DWC2_BASE + DOEPCTL0,
                     DXEPCTL_EPENA | DXEPCTL_USBACTEP |
                     D0EPCTL_MPS_64);
        g_assert_cmpint(dwc2_transport_exchange(
                            sockets[0], DWC2_DEVICE_TRANSPORT_SETUP, 0, 0,
                            setup_packet, sizeof(setup_packet),
                            NULL, 0), ==, sizeof(setup_packet));
        status = (sizeof(setup_packet) << GRXSTS_BYTECNT_SHIFT) |
                 (GRXSTS_PKTSTS_SETUPRX << GRXSTS_PKTSTS_SHIFT);
        g_assert_cmphex(qtest_readl(
                            qts, BCM2711_DWC2_BASE + GRXSTSP),
                        ==, status);
        g_assert_cmphex(qtest_readl(
                            qts, BCM2711_DWC2_BASE + EPFIFO(0)),
                        ==, ldl_le_p(setup_packet));
        g_assert_cmphex(qtest_readl(
                            qts, BCM2711_DWC2_BASE + EPFIFO(0)),
                        ==, ldl_le_p(setup_packet + 4));
        status = GRXSTS_PKTSTS_SETUPDONE << GRXSTS_PKTSTS_SHIFT;
        g_assert_cmphex(qtest_readl(
                            qts, BCM2711_DWC2_BASE + GRXSTSP),
                        ==, status);
        g_assert_cmphex(qtest_readl(
                            qts, BCM2711_DWC2_BASE + DOEPTSIZ0) &
                        DOEPTSIZ0_SUPCNT_MASK, ==, 0);

        for (int index = 1; index >= 0; index--) {
            unsigned int ep = index + 1;

            g_assert_cmpint(dwc2_transport_exchange(
                                sockets[0], DWC2_DEVICE_TRANSPORT_OUT,
                                ep, 0, out_packet[index],
                                sizeof(out_packet[index]), NULL, 0), ==, 4);
            status = ep | (4 << GRXSTS_BYTECNT_SHIFT) |
                     (GRXSTS_PKTSTS_OUTRX << GRXSTS_PKTSTS_SHIFT);
            g_assert_cmphex(qtest_readl(
                                qts, BCM2711_DWC2_BASE + GRXSTSP),
                            ==, status);
            g_assert_cmphex(qtest_readl(
                                qts, BCM2711_DWC2_BASE + EPFIFO(0)),
                            ==, ldl_le_p(out_packet[index]));
            status = ep |
                     (GRXSTS_PKTSTS_OUTDONE << GRXSTS_PKTSTS_SHIFT);
            g_assert_cmphex(qtest_readl(
                                qts, BCM2711_DWC2_BASE + GRXSTSP),
                            ==, status);
        }
        g_assert_false(qtest_readl(qts, BCM2711_DWC2_BASE + GINTSTS) &
                       GINTSTS_RXFLVL);

        for (unsigned int index = 0; index < 2; index++) {
            memset(actual, 0, sizeof(actual));
            g_assert_cmpint(dwc2_transport_exchange(
                                sockets[0], DWC2_DEVICE_TRANSPORT_IN,
                                index + 1, sizeof(actual), NULL, 0,
                                actual, sizeof(actual)), ==, 4);
            g_assert_cmpmem(actual, sizeof(actual),
                            in_packet[index], sizeof(in_packet[index]));
        }
    }

    qtest_quit(qts);
    close(sockets[0]);
}
#endif
#ifndef _WIN32
void test_bcm2711_dwc2_device_suspend_migration(void)
{
    static const uint8_t out_packet[] = { 0x51, 0x52, 0x53, 0x54 };
    static const uint8_t in_packet[] = { 0xa1, 0xa2, 0xa3, 0xa4 };
    uint8_t actual[sizeof(in_packet)] = { 0 };
    g_autofree char *source_command = NULL;
    g_autofree char *destination_command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    uint32_t status;
    int source_transport[2];
    int destination_transport[2];
    QTestState *source;
    QTestState *destination;

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0,
                               source_transport), ==, 0);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0,
                               destination_transport), ==, 0);
    source_command = g_strdup_printf(
        "-M raspi4b -chardev socket,id=dwc2dev,fd=%d "
        "-global dwc2-usb.device-chardev=dwc2dev", source_transport[1]);
    destination_command = g_strdup_printf(
        "-M raspi4b -chardev socket,id=dwc2dev,fd=%d "
        "-global dwc2-usb.device-chardev=dwc2dev",
        destination_transport[1]);
    source = qtest_init(source_command);
    close(source_transport[1]);

    qtest_writel(source, BCM2711_DWC2_BASE + GUSBCFG,
                 qtest_readl(source, BCM2711_DWC2_BASE + GUSBCFG) |
                 GUSBCFG_FORCEDEVMODE);
    qtest_writel(source, BCM2711_DWC2_BASE + GRXFSIZ, 16);
    qtest_writel(source, BCM2711_DWC2_BASE + DPTXFSIZN(1),
                 (4 << FIFOSIZE_DEPTH_SHIFT) | 4);
    g_assert_cmpint(dwc2_transport_exchange(
                        source_transport[0],
                        DWC2_DEVICE_TRANSPORT_CONNECT, 0, 0,
                        NULL, 0, NULL, 0), ==, 0);
    g_assert_cmpint(dwc2_transport_exchange(
                        source_transport[0],
                        DWC2_DEVICE_TRANSPORT_ENUM_DONE, 0,
                        DSTS_ENUMSPD_HS, NULL, 0, NULL, 0), ==, 0);
    qtest_writel(source, BCM2711_DWC2_BASE + DOEPTSIZ(1),
                 DXEPTSIZ_PKTCNT(1) |
                 DXEPTSIZ_XFERSIZE(sizeof(out_packet)));
    qtest_writel(source, BCM2711_DWC2_BASE + DOEPCTL(1),
                 DXEPCTL_EPENA | DXEPCTL_USBACTEP |
                 DXEPCTL_EPTYPE_BULK | DXEPCTL_MPS(64));
    qtest_writel(source, BCM2711_DWC2_BASE + DIEPTSIZ(1),
                 DXEPTSIZ_PKTCNT(1) |
                 DXEPTSIZ_XFERSIZE(sizeof(in_packet)));
    qtest_writel(source, BCM2711_DWC2_BASE + DIEPCTL(1),
                 DXEPCTL_EPENA | DXEPCTL_USBACTEP |
                 DXEPCTL_EPTYPE_BULK | DXEPCTL_MPS(64));
    qtest_writel(source, BCM2711_DWC2_BASE + EPFIFO(1),
                 ldl_le_p(in_packet));

    g_assert_cmpint(dwc2_transport_exchange(
                        source_transport[0],
                        DWC2_DEVICE_TRANSPORT_SUSPEND, 0, 0,
                        NULL, 0, NULL, 0), ==, 0);
    g_assert_true(qtest_readl(source, BCM2711_DWC2_BASE + DSTS) &
                  DSTS_SUSPSTS);
    g_assert_true(qtest_readl(source, BCM2711_DWC2_BASE + GINTSTS) &
                  GINTSTS_USBSUSP);
    g_assert_cmpint(dwc2_transport_exchange(
                        source_transport[0],
                        DWC2_DEVICE_TRANSPORT_OUT, 1, 0,
                        out_packet, sizeof(out_packet), NULL, 0), ==,
                    -EAGAIN);
    g_assert_cmpint(dwc2_transport_exchange(
                        source_transport[0],
                        DWC2_DEVICE_TRANSPORT_IN, 1, sizeof(actual),
                        NULL, 0, actual, sizeof(actual)), ==, -EAGAIN);

    destination = migrate_to_new_qtest_commands(
        source, destination_command, &migration_dir, &migration_socket);
    close(destination_transport[1]);
    g_assert_true(qtest_readl(destination, BCM2711_DWC2_BASE + DSTS) &
                  DSTS_SUSPSTS);
    g_assert_true(qtest_readl(destination, BCM2711_DWC2_BASE + GINTSTS) &
                  GINTSTS_USBSUSP);
    g_assert_true(qtest_readl(destination,
                             BCM2711_DWC2_BASE + DOEPCTL(1)) &
                  DXEPCTL_EPENA);
    g_assert_true(qtest_readl(destination,
                             BCM2711_DWC2_BASE + DIEPCTL(1)) &
                  DXEPCTL_EPENA);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + DTXFSTS(1)), ==, 3);

    g_assert_cmpint(dwc2_transport_exchange(
                        destination_transport[0],
                        DWC2_DEVICE_TRANSPORT_RESUME, 0, 0,
                        NULL, 0, NULL, 0), ==, 0);
    g_assert_false(qtest_readl(destination, BCM2711_DWC2_BASE + DSTS) &
                   DSTS_SUSPSTS);
    g_assert_cmphex(qtest_readl(destination, BCM2711_DWC2_BASE + GINTSTS) &
                    (GINTSTS_USBSUSP | GINTSTS_WKUPINT), ==,
                    GINTSTS_USBSUSP | GINTSTS_WKUPINT);
    qtest_writel(destination, BCM2711_DWC2_BASE + GINTSTS,
                 GINTSTS_USBSUSP | GINTSTS_WKUPINT);
    g_assert_cmphex(qtest_readl(destination, BCM2711_DWC2_BASE + GINTSTS) &
                    (GINTSTS_USBSUSP | GINTSTS_WKUPINT), ==, 0);

    g_assert_cmpint(dwc2_transport_exchange(
                        destination_transport[0],
                        DWC2_DEVICE_TRANSPORT_OUT, 1, 0,
                        out_packet, sizeof(out_packet), NULL, 0), ==,
                    sizeof(out_packet));
    status = 1 | (sizeof(out_packet) << GRXSTS_BYTECNT_SHIFT) |
             (GRXSTS_PKTSTS_OUTRX << GRXSTS_PKTSTS_SHIFT);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + GRXSTSP), ==, status);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + EPFIFO(0)), ==,
                    ldl_le_p(out_packet));
    status = 1 | (GRXSTS_PKTSTS_OUTDONE << GRXSTS_PKTSTS_SHIFT);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + GRXSTSP), ==, status);
    g_assert_cmpint(dwc2_transport_exchange(
                        destination_transport[0],
                        DWC2_DEVICE_TRANSPORT_IN, 1, sizeof(actual),
                        NULL, 0, actual, sizeof(actual)), ==,
                    sizeof(in_packet));
    g_assert_cmpmem(actual, sizeof(actual),
                    in_packet, sizeof(in_packet));

    g_assert_cmpint(dwc2_transport_exchange(
                        destination_transport[0],
                        DWC2_DEVICE_TRANSPORT_SUSPEND, 0, 0,
                        NULL, 0, NULL, 0), ==, 0);
    g_assert_cmpint(dwc2_transport_exchange(
                        destination_transport[0],
                        DWC2_DEVICE_TRANSPORT_RESET, 0, 0,
                        NULL, 0, NULL, 0), ==, 0);
    g_assert_false(qtest_readl(destination, BCM2711_DWC2_BASE + DSTS) &
                   DSTS_SUSPSTS);
    g_assert_cmphex(qtest_readl(destination, BCM2711_DWC2_BASE + GINTSTS) &
                    (GINTSTS_USBSUSP | GINTSTS_WKUPINT), ==, 0);
    g_assert_true(qtest_readl(destination, BCM2711_DWC2_BASE + GINTSTS) &
                  GINTSTS_USBRST);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + DOEPCTL(1)), ==,
                    DXEPCTL_USBACTEP | DXEPCTL_EPTYPE_BULK |
                    DXEPCTL_MPS(64));
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + DIEPCTL(1)), ==,
                    DXEPCTL_USBACTEP | DXEPCTL_EPTYPE_BULK |
                    DXEPCTL_MPS(64));

    g_assert_cmpint(dwc2_transport_exchange(
                        destination_transport[0],
                        DWC2_DEVICE_TRANSPORT_ENUM_DONE, 0,
                        DSTS_ENUMSPD_HS, NULL, 0, NULL, 0), ==, 0);
    g_assert_cmpint(dwc2_transport_exchange(
                        destination_transport[0],
                        DWC2_DEVICE_TRANSPORT_SUSPEND, 0, 0,
                        NULL, 0, NULL, 0), ==, 0);
    g_assert_cmpint(dwc2_transport_exchange(
                        destination_transport[0],
                        DWC2_DEVICE_TRANSPORT_DISCONNECT, 0, 0,
                        NULL, 0, NULL, 0), ==, 0);
    g_assert_false(qtest_readl(destination, BCM2711_DWC2_BASE + DSTS) &
                   DSTS_SUSPSTS);
    g_assert_cmphex(qtest_readl(destination, BCM2711_DWC2_BASE + GINTSTS) &
                    (GINTSTS_USBSUSP | GINTSTS_WKUPINT), ==, 0);
    g_assert_true(qtest_readl(destination, BCM2711_DWC2_BASE + GINTSTS) &
                  GINTSTS_DISCONNINT);

    qtest_quit(source);
    qtest_quit(destination);
    close(source_transport[0]);
    close(destination_transport[0]);
    unlink(migration_socket);
    rmdir(migration_dir);
}
#endif
#ifndef _WIN32
void test_bcm2711_dwc2_host_pio_fifo(void)
{
    static const uint8_t get_device[] = {
        USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, 0x00, USB_DT_DEVICE,
        0x00, 0x00, 18, 0x00,
    };
    uint8_t descriptor[20] = { 0 };
    g_autofree char *migration_dir1 = NULL;
    g_autofree char *migration_socket1 = NULL;
    g_autofree char *migration_dir2 = NULL;
    g_autofree char *migration_socket2 = NULL;
    const char *command = "-M raspi4b -device usb-kbd,bus=usb-bus.0";
    uint32_t hcchar = HCCHAR_CHENA |
                      (USB_ENDPOINT_XFER_CONTROL << HCCHAR_EPTYPE_SHIFT) |
                      64;
    uint32_t status;
    QTestState *source;
    QTestState *middle;
    QTestState *destination;

    source = qtest_init(command);
    raspi4_dwc2_reset_root_port(source);
    qtest_writel(source, BCM2711_DWC2_BASE + GAHBCFG, 0);
    qtest_writel(source, BCM2711_DWC2_BASE + GRXFSIZ, 16);
    qtest_writel(source, BCM2711_DWC2_BASE + GNPTXFSIZ,
                 4 << FIFOSIZE_DEPTH_SHIFT);

    qtest_writel(source, BCM2711_DWC2_BASE + HCINT(0), UINT32_MAX);
    qtest_writel(source, BCM2711_DWC2_BASE + HCTSIZ(0),
                 (TSIZ_SC_MC_PID_SETUP << TSIZ_SC_MC_PID_SHIFT) |
                 (1 << TSIZ_PKTCNT_SHIFT) | sizeof(get_device));
    qtest_writel(source, BCM2711_DWC2_BASE + HCDMA(0), 0x12340000);
    qtest_writel(source, BCM2711_DWC2_BASE + HCCHAR(0), hcchar);
    qtest_writel(source, BCM2711_DWC2_BASE + EPFIFO(0),
                 ldl_le_p(get_device));
    g_assert_false(qtest_readl(source,
                              BCM2711_DWC2_BASE + HCINT(0)) &
                   HCINTMSK_XFERCOMPL);
    g_assert_cmphex(qtest_readl(source,
                               BCM2711_DWC2_BASE + GNPTXSTS) &
                    GNPTXSTS_NP_TXF_SPC_AVAIL_MASK, ==, 3);

    middle = migrate_to_new_qtest(source, command, &migration_dir1,
                                  &migration_socket1);
    g_assert_cmphex(qtest_readl(middle,
                               BCM2711_DWC2_BASE + GNPTXSTS) &
                    GNPTXSTS_NP_TXF_SPC_AVAIL_MASK, ==, 3);
    qtest_writel(middle, BCM2711_DWC2_BASE + EPFIFO(0),
                 ldl_le_p(get_device + 4));
    status = wait_for_dwc2_channel(middle, 0);
    g_assert_true(status & HCINTMSK_CHHLTD);
    g_assert_cmphex(qtest_readl(middle,
                               BCM2711_DWC2_BASE + HCDMA(0)), ==,
                    0x12340000);
    g_assert_cmphex(qtest_readl(middle,
                               BCM2711_DWC2_BASE + GNPTXSTS) &
                    GNPTXSTS_NP_TXF_SPC_AVAIL_MASK, ==, 4);

    qtest_writel(middle, BCM2711_DWC2_BASE + GRXFSIZ, 1);
    qtest_writel(middle, BCM2711_DWC2_BASE + HCINT(0), UINT32_MAX);
    qtest_writel(middle, BCM2711_DWC2_BASE + HCTSIZ(0),
                 (TSIZ_SC_MC_PID_DATA1 << TSIZ_SC_MC_PID_SHIFT) |
                 (1 << TSIZ_PKTCNT_SHIFT) | 18);
    qtest_writel(middle, BCM2711_DWC2_BASE + HCDMA(0), 0x56780000);
    qtest_writel(middle, BCM2711_DWC2_BASE + HCCHAR(0),
                 hcchar | HCCHAR_EPDIR);
    qtest_clock_step(middle, 250000);
    g_assert_false(qtest_readl(middle,
                              BCM2711_DWC2_BASE + HCINT(0)) &
                   HCINTMSK_XFERCOMPL);
    g_assert_false(qtest_readl(middle,
                              BCM2711_DWC2_BASE + GINTSTS) &
                   GINTSTS_RXFLVL);
    qtest_writel(middle, BCM2711_DWC2_BASE + GRXFSIZ, 16);
    wait_for_dwc2_channel(middle, 0);
    g_assert_true(qtest_readl(middle,
                             BCM2711_DWC2_BASE + GINTSTS) &
                  GINTSTS_RXFLVL);

    destination = migrate_to_new_qtest(middle, command, &migration_dir2,
                                       &migration_socket2);
    status = (18 << GRXSTS_BYTECNT_SHIFT) |
             (TSIZ_SC_MC_PID_DATA1 << GRXSTS_DPID_SHIFT) |
             (GRXSTS_PKTSTS_HCHIN << GRXSTS_PKTSTS_SHIFT);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + GRXSTSR), ==, status);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + GRXSTSP), ==, status);
    for (unsigned int word = 0; word < 5; word++) {
        stl_le_p(descriptor + word * sizeof(uint32_t),
                 qtest_readl(destination,
                             BCM2711_DWC2_BASE + EPFIFO(0)));
    }
    g_assert_cmpuint(descriptor[0], ==, 18);
    g_assert_cmpuint(descriptor[1], ==, USB_DT_DEVICE);
    status = GRXSTS_PKTSTS_HCHIN_XFER_COMP << GRXSTS_PKTSTS_SHIFT;
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + GRXSTSP), ==, status);
    g_assert_false(qtest_readl(destination,
                              BCM2711_DWC2_BASE + GINTSTS) &
                   GINTSTS_RXFLVL);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + HCDMA(0)), ==,
                    0x56780000);

    qtest_writel(destination, BCM2711_DWC2_BASE + HCINT(0), UINT32_MAX);
    qtest_writel(destination, BCM2711_DWC2_BASE + HCTSIZ(0),
                 (TSIZ_SC_MC_PID_DATA1 << TSIZ_SC_MC_PID_SHIFT) |
                 (1 << TSIZ_PKTCNT_SHIFT));
    qtest_writel(destination, BCM2711_DWC2_BASE + HCCHAR(0), hcchar);
    wait_for_dwc2_channel(destination, 0);

    qtest_writel(destination, BCM2711_DWC2_BASE + GNPTXFSIZ,
                 1 << FIFOSIZE_DEPTH_SHIFT);
    qtest_writel(destination, BCM2711_DWC2_BASE + HCINT(0), UINT32_MAX);
    qtest_writel(destination, BCM2711_DWC2_BASE + HCTSIZ(0),
                 (TSIZ_SC_MC_PID_SETUP << TSIZ_SC_MC_PID_SHIFT) |
                 (1 << TSIZ_PKTCNT_SHIFT) | sizeof(get_device));
    qtest_writel(destination, BCM2711_DWC2_BASE + HCCHAR(0), hcchar);
    qtest_writel(destination, BCM2711_DWC2_BASE + EPFIFO(0),
                 ldl_le_p(get_device));
    qtest_writel(destination, BCM2711_DWC2_BASE + EPFIFO(0),
                 ldl_le_p(get_device + 4));
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + GNPTXSTS) &
                    GNPTXSTS_NP_TXF_SPC_AVAIL_MASK, ==, 0);
    g_assert_false(qtest_readl(destination,
                              BCM2711_DWC2_BASE + HCINT(0)) &
                   HCINTMSK_XFERCOMPL);
    qtest_writel(destination, BCM2711_DWC2_BASE + GRSTCTL,
                 GRSTCTL_TXFFLSH | GRSTCTL_TXFNUM(0));
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + GNPTXSTS) &
                    GNPTXSTS_NP_TXF_SPC_AVAIL_MASK, ==, 1);
    qtest_writel(destination, BCM2711_DWC2_BASE + GNPTXFSIZ,
                 4 << FIFOSIZE_DEPTH_SHIFT);
    qtest_writel(destination, BCM2711_DWC2_BASE + EPFIFO(0),
                 ldl_le_p(get_device));
    qtest_writel(destination, BCM2711_DWC2_BASE + EPFIFO(0),
                 ldl_le_p(get_device + 4));
    wait_for_dwc2_channel(destination, 0);

    qtest_writel(destination, BCM2711_DWC2_BASE + HPTXFSIZ,
                 1 << FIFOSIZE_DEPTH_SHIFT);
    qtest_writel(destination, BCM2711_DWC2_BASE + HCINT(1), UINT32_MAX);
    qtest_writel(destination, BCM2711_DWC2_BASE + HCTSIZ(1),
                 (TSIZ_SC_MC_PID_DATA0 << TSIZ_SC_MC_PID_SHIFT) |
                 (1 << TSIZ_PKTCNT_SHIFT) | 8);
    qtest_writel(destination, BCM2711_DWC2_BASE + HCDMA(1), 0x9abc0000);
    qtest_writel(destination, BCM2711_DWC2_BASE + HCCHAR(1),
                 HCCHAR_CHENA | (1 << HCCHAR_EPNUM_SHIFT) |
                 (USB_ENDPOINT_XFER_INT << HCCHAR_EPTYPE_SHIFT) | 8);
    qtest_writel(destination, BCM2711_DWC2_BASE + EPFIFO(1),
                 0x44332211);
    g_assert_false(qtest_readl(destination,
                              BCM2711_DWC2_BASE + HCINT(1)) &
                   HCINTMSK_CHHLTD);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + HPTXSTS) &
                    TXSTS_FSPCAVAIL_MASK, ==, 0);
    qtest_writel(destination, BCM2711_DWC2_BASE + GRSTCTL,
                 GRSTCTL_TXFFLSH | GRSTCTL_TXFNUM(1));
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + HPTXSTS) &
                    TXSTS_FSPCAVAIL_MASK, ==, 1);
    g_assert_cmphex(qtest_readl(destination,
                               BCM2711_DWC2_BASE + HCDMA(1)), ==,
                    0x9abc0000);
    qtest_writel(destination, BCM2711_DWC2_BASE + HCCHAR(1),
                 qtest_readl(destination,
                             BCM2711_DWC2_BASE + HCCHAR(1)) |
                 HCCHAR_CHDIS);

    qtest_quit(source);
    qtest_quit(middle);
    qtest_quit(destination);
    unlink(migration_socket1);
    unlink(migration_socket2);
    rmdir(migration_dir1);
    rmdir(migration_dir2);
}
#endif
#ifndef _WIN32
void test_bcm2711_dwc2_host_pio_async_reset_storm(void)
{
    static const char *command =
        "-M raspi4b "
        "-blockdev driver=null-co,node-name=usbdata,size=1048576,"
        "latency-ns=250000000,read-zeroes=on "
        "-device usb-storage,bus=usb-bus.0,drive=usbdata";
    uint8_t descriptor[18] = { 0 };
    uint8_t hub_descriptor[16] = { 0 };
    uint8_t port_status[4] = { 0 };
    uint8_t cbw[32] = { 0 };
    uint8_t inquiry_cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
    uint8_t inquiry[36] = { 0 };
    uint8_t capacity_cdb[10] = { 0x25 };
    uint8_t capacity[8] = { 0 };
    uint8_t max_lun = 0xff;
    uint32_t bulk_in = HCCHAR_CHENA |
                       (2 << HCCHAR_DEVADDR_SHIFT) |
                       (1 << HCCHAR_EPNUM_SHIFT) |
                       (USB_ENDPOINT_XFER_BULK << HCCHAR_EPTYPE_SHIFT) |
                       HCCHAR_EPDIR | 64;
    QTestState *qts = qtest_init(command);

    raspi4_dwc2_reset_root_port(qts);
    raspi4_dwc2_get_device_descriptor(qts, 0, descriptor);
    g_assert_cmpuint(descriptor[1], ==, USB_DT_DEVICE);
    raspi4_dwc2_set_address(qts, 0, 1);
    g_assert_cmpuint(raspi4_dwc2_control(
                         qts, 1,
                         USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE,
                         USB_REQ_GET_DESCRIPTOR, 0x2900, 0,
                         hub_descriptor, sizeof(hub_descriptor)), >=, 9);
    g_assert_cmpuint(hub_descriptor[2], >=, 1);
    g_assert_cmpuint(raspi4_dwc2_control(
                         qts, 1,
                         USB_TYPE_CLASS | USB_RECIP_OTHER,
                         USB_REQ_SET_FEATURE, 4, 1, NULL, 0), ==, 0);
    g_assert_cmpuint(raspi4_dwc2_control(
                         qts, 1,
                         USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_OTHER,
                         USB_REQ_GET_STATUS, 0, 1,
                         port_status, sizeof(port_status)), ==,
                     sizeof(port_status));
    g_assert_true(lduw_le_p(port_status) & 0x0002);
    memset(descriptor, 0, sizeof(descriptor));
    raspi4_dwc2_get_device_descriptor(qts, 0, descriptor);
    g_assert_cmpuint(descriptor[1], ==, USB_DT_DEVICE);
    g_assert_cmpuint(descriptor[4], ==, 0);
    raspi4_dwc2_set_address(qts, 0, 2);
    g_assert_cmpuint(raspi4_dwc2_control(
                         qts, 2,
                         USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                         USB_REQ_SET_CONFIGURATION, 1, 0, NULL, 0), ==, 0);

    stl_le_p(cbw, 0x43425355);
    stl_le_p(cbw + 4, 0x52504934);
    stl_le_p(cbw + 8, 512);
    cbw[12] = USB_DIR_IN;
    cbw[14] = 10;
    cbw[15] = 0x28;
    cbw[15 + 8] = 1;
    raspi4_dwc2_bot_command(qts, 2, 0, 0x52504930,
                            inquiry_cdb, sizeof(inquiry_cdb),
                            inquiry, sizeof(inquiry), true);
    g_assert_cmpuint(inquiry[0] & 0x1f, ==, 0);
    raspi4_dwc2_bot_command(qts, 2, 0, 0x52504931,
                            capacity_cdb, sizeof(capacity_cdb),
                            capacity, sizeof(capacity), false);
    memset(capacity, 0, sizeof(capacity));
    raspi4_dwc2_bot_command(qts, 2, 0, 0x52504932,
                            capacity_cdb, sizeof(capacity_cdb),
                            capacity, sizeof(capacity), true);
    g_assert_cmpuint(ldl_be_p(capacity + 4), ==, 512);

    for (unsigned int cycle = 0; cycle < 16; cycle++) {
        uint32_t reset = cycle & 1 ? GRSTCTL_CSFTRST : GRSTCTL_HSFTRST;

        g_test_message("DWC2 host PIO async reset cycle %u", cycle);
        stl_le_p(cbw + 4, 0x52504934 + cycle);
        g_assert_cmpuint(raspi4_dwc2_endpoint_transfer(
                             qts, 2, 2, USB_ENDPOINT_XFER_BULK, 64,
                             false, TSIZ_SC_MC_PID_DATA0, cbw, 31), ==, 31);

        qtest_writel(qts, BCM2711_DWC2_BASE + GAHBCFG, 0);
        qtest_writel(qts, BCM2711_DWC2_BASE + GRXFSIZ, 16);
        qtest_writel(qts, BCM2711_DWC2_BASE + HCINT(0), UINT32_MAX);
        g_assert_cmphex(qtest_readl(qts,
                                   BCM2711_DWC2_BASE + HCINT(0)), ==, 0);
        qtest_writel(qts, BCM2711_DWC2_BASE + HCTSIZ(0),
                     (TSIZ_SC_MC_PID_DATA1 << TSIZ_SC_MC_PID_SHIFT) |
                     (1 << TSIZ_PKTCNT_SHIFT) | 64);
        qtest_writel(qts, BCM2711_DWC2_BASE + HCDMA(0), 0x22220000);
        qtest_writel(qts, BCM2711_DWC2_BASE + HCCHAR(0), bulk_in);

        qtest_writel(qts, BCM2711_DWC2_BASE + HCINT(1), UINT32_MAX);
        qtest_writel(qts, BCM2711_DWC2_BASE + HCTSIZ(1),
                     (TSIZ_SC_MC_PID_DATA1 << TSIZ_SC_MC_PID_SHIFT) |
                     (1 << TSIZ_PKTCNT_SHIFT) | 64);
        qtest_writel(qts, BCM2711_DWC2_BASE + HCDMA(1), 0x33330000);
        qtest_writel(qts, BCM2711_DWC2_BASE + HCCHAR(1), bulk_in);

        g_assert_false(qtest_readl(qts,
                                  BCM2711_DWC2_BASE + HCINT(0)) &
                       HCINTMSK_XFERCOMPL);
        g_assert_false(qtest_readl(qts,
                                  BCM2711_DWC2_BASE + HCINT(1)) &
                       HCINTMSK_XFERCOMPL);
        g_assert_false(qtest_readl(qts,
                                  BCM2711_DWC2_BASE + GINTSTS) &
                       GINTSTS_RXFLVL);

        qtest_writel(qts, BCM2711_DWC2_BASE + GRSTCTL, reset);
        g_assert_false(qtest_readl(qts,
                                  BCM2711_DWC2_BASE + GRSTCTL) & reset);
        g_assert_cmphex(qtest_readl(qts,
                                   BCM2711_DWC2_BASE + HCCHAR(0)), ==, 0);
        g_assert_cmphex(qtest_readl(qts,
                                   BCM2711_DWC2_BASE + HCCHAR(1)), ==, 0);
        g_assert_false(qtest_readl(qts,
                                  BCM2711_DWC2_BASE + GINTSTS) &
                       (GINTSTS_RXFLVL | GINTSTS_HCHINT));
        g_assert_cmpuint(raspi4_dwc2_control(
                             qts, 2,
                             USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                             0xff, 0, 0, NULL, 0), ==, 0);
    }

    g_assert_cmpuint(raspi4_dwc2_control(
                         qts, 2,
                         USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                         0xfe, 0, 0, &max_lun, 1), ==, 1);
    g_assert_cmpuint(max_lun, ==, 0);
    qtest_quit(qts);
}
#endif
