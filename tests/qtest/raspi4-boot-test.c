/*
 * Raspberry Pi 4 behavioral boot boundary tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "raspi4-boot-test.h"


























#ifndef _WIN32










#endif











































































































static void test_recovery_without_eeprom(void)
{
    QTestState *qts = qtest_init("-M raspi4b,boot-mode=behavioral");
    g_autofree char *state = qom_get_string(qts, "boot-state");
    g_autofree char *source = qom_get_string(qts, "boot-source");

    g_assert_cmpstr(state, ==, "recovery-required");
    g_assert_cmpstr(source, ==, "sd-card");
    g_assert_true(qtest_qom_get_bool(qts, "/machine/soc/cpu[0]",
                                     "start-powered-off"));
    qtest_quit(qts);
}

static void test_nrpiboot_precedes_eeprom(void)
{
    QTestState *qts =
        qtest_init("-M raspi4b,boot-mode=behavioral,nrpiboot=on,"
                   "otp-rpiboot-gpio=8");
    g_autofree char *state = qom_get_string(qts, "boot-state");
    g_autofree char *source = qom_get_string(qts, "boot-source");

    g_assert_cmpstr(state, ==, "rpiboot-wait");
    g_assert_cmpstr(source, ==, "rpiboot");
    qtest_quit(qts);
}

static void test_nrpiboot_requires_otp_configuration(void)
{
    QTestState *qts =
        qtest_init("-M raspi4b,boot-mode=behavioral,nrpiboot=on");
    g_autofree char *state = qom_get_string(qts, "boot-state");

    g_assert_cmpstr(state, ==, "recovery-required");
    qtest_quit(qts);
}






static void test_eeprom_boot_order(void)
{
    static const uint8_t firmware[] = "QEMU Pi 4 firmware fixture";
    const char config[] = "[pi4]\nBOOT_ORDER=0xf41\n";
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    QTestState *qts;
    g_autofree char *state = NULL;
    g_autofree char *source = NULL;
    g_autofree char *firmware_file = NULL;
    g_autofree char *firmware_status = NULL;
    g_autofree char *device_tree_file = NULL;
    g_autofree char *firmware_hash = NULL;
    g_autofree char *handoff_status = NULL;
    uint64_t device_tree_address;
    uint8_t header[64];
    uint8_t fdt_magic[4];

    qts = start_with_eeprom_and_sd(
        config, "START   ELF", firmware, sizeof(firmware),
        NULL, &eeprom_path, &sd_path);
    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "boot-source");
    firmware_file = qom_get_string(qts, "firmware-file");
    firmware_status = qom_get_string(qts, "firmware-status");
    device_tree_file = qom_get_string(qts, "firmware-device-tree-file");
    firmware_hash = qom_get_string(qts, "firmware-sha256");
    handoff_status = qom_get_string(qts, "arm-handoff-status");

    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(source, ==, "sd-card");
    g_assert_cmpstr(firmware_file, ==, "start.elf");
    g_assert_cmpstr(firmware_status, ==, "config-ready");
    g_assert_cmpstr(device_tree_file, ==, "bcm2711-rpi-4-b.dtb");
    g_assert_cmpuint(strlen(firmware_hash), ==, 64);
    g_assert_cmpuint(qom_get_uint32(qts, "firmware-size"), ==,
                     sizeof(firmware));
    g_assert_cmpuint(qom_get_uint32(qts, "firmware-kernel-size"), >, 0);
    g_assert_cmpuint(qom_get_uint32(qts, "firmware-device-tree-size"), >, 0);
    g_assert_false(qom_get_bool(qts, "firmware-config-present"));
    g_assert_true(qom_get_bool(qts, "firmware-arm-64bit"));
    g_assert_cmpuint(qom_get_uint32(qts, "boot-order"), ==, 0xf41);
    g_assert_cmpuint(qom_get_uint64(qts, "boot-attempt-count"), ==, 1);
    g_assert_cmpuint(qom_get_uint32(qts, "boot-order-index"), ==, 0);
    g_assert_cmpstr(handoff_status, ==, "ready");
    g_assert_cmpuint(qom_get_uint64(qts, "arm-handoff-kernel-address"),
                     ==, 0x200000);
    g_assert_cmpuint(qom_get_uint64(qts, "arm-handoff-kernel-size"),
                     ==, TEST_KERNEL_SIZE);
    g_assert_cmpuint(qom_get_uint32(qts, "arm-handoff-core-mask"), ==, 0xf);
    qtest_memread(qts, 0x200000, header, sizeof(header));
    g_assert_cmpmem(header + 56, 4, "ARM\x64", 4);
    device_tree_address = qom_get_uint64(
        qts, "arm-handoff-device-tree-address");
    g_assert_cmpuint(device_tree_address, >, 0);
    qtest_memread(qts, device_tree_address, fdt_magic, sizeof(fdt_magic));
    g_assert_cmpmem(fdt_magic, sizeof(fdt_magic), "\xd0\x0d\xfe\xed", 4);
    g_assert_cmphex(qtest_readl(qts, 0x300), ==, 0xd2801b05);
    g_assert_cmphex(qtest_readq(qts, 0xd8), ==, 0);

    qtest_system_reset(qts);
    g_clear_pointer(&state, g_free);
    g_clear_pointer(&source, g_free);
    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "boot-source");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(source, ==, "sd-card");
    qtest_quit(qts);
    unlink(eeprom_path);
    unlink(sd_path);
}

static void test_arm32_firmware_handoff(void)
{
    static const uint8_t firmware[] = "QEMU Pi 4 firmware fixture";
    static const uint8_t fixup[] = "QEMU fixup fixture";
    static const uint8_t cmdline[] =
        "console=ttyAMA0 root=/dev/mmcblk0p2\n";
    static const char eeprom_config[] = "BOOT_ORDER=0xf1\n";
    static const char media_config[] = "arm_64bit=0\n";
    static const uint32_t entry_code[] = {
        0xe1a02000,
        0xe3a00000,
        0xe3e01000,
        0xe51ff004,
        0x00008000,
    };
    g_autofree uint8_t *eeprom =
        make_eeprom_image(eeprom_config, false);
    g_autofree uint8_t *kernel = make_arm32_kernel();
    g_autofree uint8_t *device_tree = NULL;
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *state = NULL;
    g_autofree char *kernel_file = NULL;
    g_autofree char *architecture = NULL;
    g_autofree char *registers = NULL;
    size_t device_tree_size;
    uint64_t device_tree_address;
    uint8_t loaded_kernel[64];
    uint8_t loaded_entry[sizeof(entry_code)];
    uint8_t fdt_magic[4];
    Fat16Builder builder;
    QTestState *qts;

    device_tree = make_device_tree(&device_tree_size);
    fat16_init(&builder);
    fat16_add_file(&builder, "START4  ELF", firmware, sizeof(firmware));
    fat16_add_file(&builder, "FIXUP4  DAT", fixup, sizeof(fixup));
    fat16_add_file(&builder, "KERNEL7LIMG", kernel, TEST_KERNEL_SIZE);
    fat16_add_long_file(&builder, "bcm2711-rpi-4-b.dtb", "BCM271~1DTB",
                        device_tree, device_tree_size);
    fat16_add_file(&builder, "CMDLINE TXT", cmdline, sizeof(cmdline));
    fat16_add_file(&builder, "CONFIG  TXT",
                   (const uint8_t *)media_config, strlen(media_config));
    write_temp_image("raspi4-arm32-sd-XXXXXX", builder.image, SD_SIZE,
                     &sd_path);
    g_free(builder.image);
    write_temp_image("raspi4-arm32-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
        "-m 2G "
        "-drive if=none,id=pieeprom,format=raw,file=%s "
        "-drive if=sd,format=raw,file=%s "
        "-nic none",
        eeprom_path, sd_path);
    qts = qtest_init(command);

    state = qom_get_string(qts, "boot-state");
    kernel_file = qom_get_string(qts, "firmware-kernel-file");
    architecture = qom_get_string(qts, "arm-handoff-architecture");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(kernel_file, ==, "kernel7l.img");
    g_assert_cmpstr(architecture, ==, "aarch32");
    g_assert_false(qom_get_bool(qts, "firmware-arm-64bit"));
    g_assert_cmpuint(qom_get_uint64(qts, "arm-handoff-kernel-address"),
                     ==, 0x8000);
    g_assert_cmpuint(qom_get_uint64(qts, "arm-handoff-entry-address"),
                     ==, 0);
    g_assert_cmpuint(qom_get_uint64(qts, "arm-handoff-kernel-size"),
                     ==, TEST_KERNEL_SIZE);
    g_assert_cmpuint(qom_get_uint32(qts, "arm-handoff-core-mask"), ==, 0xf);

    qtest_memread(qts, 0x8000, loaded_kernel, sizeof(loaded_kernel));
    g_assert_cmpmem(loaded_kernel, sizeof(loaded_kernel),
                    kernel, sizeof(loaded_kernel));
    qtest_memread(qts, 0, loaded_entry, sizeof(loaded_entry));
    g_assert_cmpmem(loaded_entry, sizeof(loaded_entry),
                    entry_code, sizeof(entry_code));
    g_assert_cmphex(qtest_readl(qts, 0x300), ==, 0xee100fb0);
    g_assert_cmphex(qtest_readl(qts, 0x324), ==, 0xff8000cc);
    g_assert_cmphex(qtest_readq(qts, 0xe0), ==, 0);

    device_tree_address = qom_get_uint64(
        qts, "arm-handoff-device-tree-address");
    g_assert_cmpuint(device_tree_address, >, 0);
    qtest_memread(qts, device_tree_address, fdt_magic, sizeof(fdt_magic));
    g_assert_cmpmem(fdt_magic, sizeof(fdt_magic), "\xd0\x0d\xfe\xed", 4);

    registers = qtest_hmp(qts, "info registers");
    g_assert_nonnull(strstr(registers, "R00="));
    g_assert_nonnull(strstr(registers, "R02="));
    g_assert_nonnull(strstr(registers, "PSR="));
    g_assert_null(strstr(registers, "X00="));

    qtest_quit(qts);
    unlink(eeprom_path);
    unlink(sd_path);
}

static void test_eeprom_boot_order_platform_filters(void)
{
    static const char config[] =
        "BOOT_ORDER=0x8\n"
        "[pi4]\n"
        "BOOT_ORDER=0xe\n"
        "[cm4]\n"
        "BOOT_ORDER=0xf\n";
    static const struct {
        const char *machine;
        uint32_t order;
        const char *state;
        const char *source;
    } cases[] = {
        { "raspi4b", 0xe, "stopped", "stop" },
        { "raspi-cm4", 0xf, "restart-loop", "restart" },
    };
    g_autofree uint8_t *eeprom = make_eeprom_image(config, false);
    g_autofree char *eeprom_path = NULL;

    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree char *command = g_strdup_printf(
            "-M %s,boot-mode=behavioral,eeprom-drive=pieeprom "
            "-drive if=none,id=pieeprom,format=raw,file=%s,"
            "file.locking=off -nic none",
            cases[i].machine, eeprom_path);
        QTestState *qts = qtest_init(command);
        g_autofree char *state = qom_get_string(qts, "boot-state");
        g_autofree char *source = qom_get_string(qts, "boot-source");

        g_assert_cmpuint(qom_get_uint32(qts, "boot-order"), ==,
                         cases[i].order);
        g_assert_cmpstr(state, ==, cases[i].state);
        g_assert_cmpstr(source, ==, cases[i].source);
        qtest_quit(qts);
    }
    unlink(eeprom_path);
}

static void test_eeprom_boot_order_runtime_filters(void)
{
    const uint64_t pm_base = 0xfe100000;
    g_autofree uint8_t *eeprom = make_eeprom_image(
        "BOOT_ORDER=0x8\n"
        "[partition=5]\n"
        "BOOT_ORDER=0xe\n",
        false);
    g_autofree char *eeprom_path = NULL;
    g_autofree char *command = NULL;
    QTestState *qts;

    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
        "-drive if=none,id=pieeprom,format=raw,file=%s,"
        "file.locking=off -nic none",
        eeprom_path);
    qts = qtest_init(command);
    g_assert_cmpuint(qom_get_uint32(qts, "boot-order"), ==, 0x8);

    /* Partition 5 is encoded in alternating PM_RSTS bits 0 and 4. */
    qtest_writel(qts, pm_base + 0x20, 0x5a000011);
    qtest_system_reset(qts);
    g_assert_cmpuint(qom_get_uint32(qts, "boot-partition"), ==, 5);
    g_assert_cmpuint(qom_get_uint32(qts, "boot-order"), ==, 0xe);
    qtest_quit(qts);
    unlink(eeprom_path);

    g_clear_pointer(&eeprom, g_free);
    g_clear_pointer(&eeprom_path, g_free);
    g_clear_pointer(&command, g_free);
    eeprom = make_eeprom_image(
        "BOOT_ORDER=0x8\n"
        "[gpio7=1]\n"
        "BOOT_ORDER=0xe\n"
        "[gpio7=0]\n"
        "BOOT_ORDER=0xf\n",
        false);
    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
        "-drive if=none,id=pieeprom,format=raw,file=%s,"
        "file.locking=off -nic none",
        eeprom_path);
    qts = qtest_init(command);
    g_assert_cmpuint(qom_get_uint32(qts, "boot-order"), ==, 0x8);

    qtest_set_irq_in(qts, GPIO_QOM_PATH, "pin-input", 7, 1);
    qtest_system_reset(qts);
    g_assert_cmpuint(qom_get_uint32(qts, "boot-order"), ==, 0xe);
    qtest_set_irq_in(qts, GPIO_QOM_PATH, "pin-input", 7, 0);
    qtest_system_reset(qts);
    g_assert_cmpuint(qom_get_uint32(qts, "boot-order"), ==, 0xf);
    qtest_quit(qts);
    unlink(eeprom_path);
}

static void test_eeprom_boot_variable_expressions(void)
{
    const uint64_t pm_base = 0xfe100000;
    g_autofree uint8_t *eeprom = make_eeprom_image(
        "BOOT_ORDER=0x8\n"
        "[cust_otp0&0x0f=0x05]\n"
        "BOOT_ORDER=0xe\n"
        "[cust_otp0=0xa4]\n"
        "BOOT_ORDER=0xf\n"
        "[cust_otp0>0xa4]\n"
        "MAX_RESTARTS=3\n"
        "[cust_otp0<0xa6]\n"
        "SD_BOOT_MAX_RETRIES=2\n"
        "[cust_otp0&0x80]\n"
        "NET_BOOT_MAX_RETRIES=4\n"
        "[boot_partition=0]\n"
        "USB_MSD_DISCOVER_TIMEOUT=6000\n"
        "[boot_count=0]\n"
        "BOOT_ORDER=0xf\n"
        "[partition=5]\n"
        "BOOT_ORDER=0xf\n",
        false);
    g_autofree uint8_t *otp = make_otp_image(
        0, 0, PI4_BOARD_REVISION, false);
    g_autofree char *eeprom_path = NULL;
    g_autofree char *otp_path = NULL;
    g_autofree char *command = NULL;
    QTestState *qts;

    stl_le_p(otp + (36 - 1) * 4, 0xa5);
    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    write_temp_image("raspi4-otp-XXXXXX", otp, OTP_SIZE, &otp_path);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "otp-drive=piotp" TEST_BOOTSYS_MACHINE_OPTION " "
        "-drive if=none,id=pieeprom,format=raw,file=%s,"
        "file.locking=off "
        "-drive if=none,id=piotp,format=raw,file=%s,"
        "file.locking=off -nic none",
        eeprom_path, otp_path);
    qts = qtest_init(command);

    g_assert_cmpuint(qom_get_uint32(qts, "boot-order"), ==, 0xe);
    g_assert_cmpint(qom_get_int32(qts, "boot-max-restarts"), ==, 3);
    g_assert_cmpint(qom_get_int32(qts, "boot-sd-max-retries"), ==, 2);
    g_assert_cmpint(qom_get_int32(qts, "boot-net-max-retries"), ==, 4);
    g_assert_cmpuint(qom_get_uint32(
                         qts, "boot-usb-discover-timeout-ms"), ==, 6000);

    /* Partition 5 is encoded in alternating PM_RSTS bits 0 and 4. */
    qtest_writel(qts, pm_base + 0x20, 0x5a000011);
    qtest_system_reset(qts);
    g_assert_cmpuint(qom_get_uint32(qts, "boot-partition"), ==, 5);
    g_assert_cmpuint(qom_get_uint32(qts, "boot-order"), ==, 0xf);
    qtest_quit(qts);
    unlink(eeprom_path);
    unlink(otp_path);
}

static void test_eeprom_boot_order_combined_identity_filters(void)
{
    const uint32_t serial = 0x12345678;
    g_autofree uint8_t *eeprom = make_eeprom_image(
        "BOOT_ORDER=0x8\n"
        "[0x87654321]\n"
        "BOOT_ORDER=0xf\n"
        "[0x12345678]\n"
        "BOOT_ORDER=0xe\n",
        false);
    g_autofree uint8_t *otp = make_otp_image(
        0, 0, PI4_BOARD_REVISION, false);
    g_autofree char *eeprom_path = NULL;
    g_autofree char *otp_path = NULL;
    g_autofree char *command = NULL;
    QTestState *qts;

    stl_le_p(otp + (28 - 1) * 4, serial);
    stl_le_p(otp + (29 - 1) * 4, ~serial);
    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    write_temp_image("raspi4-otp-XXXXXX", otp, OTP_SIZE, &otp_path);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "otp-drive=piotp" TEST_BOOTSYS_MACHINE_OPTION " "
        "-drive if=none,id=pieeprom,format=raw,file=%s,"
        "file.locking=off "
        "-drive if=none,id=piotp,format=raw,file=%s,"
        "file.locking=off -nic none",
        eeprom_path, otp_path);
    qts = qtest_init(command);
    g_assert_cmpuint(qom_get_uint32(qts, "boot-order"), ==, 0xe);
    qtest_quit(qts);
    unlink(eeprom_path);
    unlink(otp_path);

    g_clear_pointer(&eeprom, g_free);
    g_clear_pointer(&eeprom_path, g_free);
    g_clear_pointer(&command, g_free);
    eeprom = make_eeprom_image(
        "BOOT_ORDER=0x8\n"
        "[cm4]\n"
        "[gpio7=1]\n"
        "BOOT_ORDER=0xe\n",
        false);
    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    for (unsigned int cm4 = 0; cm4 <= 1; cm4++) {
        command = g_strdup_printf(
            "-M %s,boot-mode=behavioral,eeprom-drive=pieeprom "
            "-drive if=none,id=pieeprom,format=raw,file=%s,"
            "file.locking=off -nic none",
            cm4 ? "raspi-cm4" : "raspi4b", eeprom_path);
        qts = qtest_init(command);
        qtest_set_irq_in(qts, GPIO_QOM_PATH, "pin-input", 7, 1);
        qtest_system_reset(qts);
        g_assert_cmpuint(qom_get_uint32(qts, "boot-order"), ==,
                         cm4 ? 0xe : 0x8);
        qtest_quit(qts);
        g_clear_pointer(&command, g_free);
    }
    unlink(eeprom_path);

    g_clear_pointer(&eeprom, g_free);
    g_clear_pointer(&eeprom_path, g_free);
    eeprom = make_eeprom_image(
        "BOOT_ORDER=0x8\n"
        "[board-type=0x11]\n"
        "BOOT_ORDER=0xe\n"
        "[board-type=0x14]\n"
        "BOOT_ORDER=0xf\n",
        false);
    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    for (unsigned int cm4 = 0; cm4 <= 1; cm4++) {
        command = g_strdup_printf(
            "-M %s,boot-mode=behavioral,eeprom-drive=pieeprom "
            "-drive if=none,id=pieeprom,format=raw,file=%s,"
            "file.locking=off -nic none",
            cm4 ? "raspi-cm4" : "raspi4b", eeprom_path);
        qts = qtest_init(command);
        g_assert_cmpuint(qom_get_uint32(qts, "boot-order"), ==,
                         cm4 ? 0xf : 0xe);
        qtest_quit(qts);
        g_clear_pointer(&command, g_free);
    }
    unlink(eeprom_path);

    {
        static const char * const inactive_configs[] = {
            "BOOT_ORDER=0x8\n"
            "[none]\n"
            "[pi4]\n"
            "BOOT_ORDER=0xe\n",
            "BOOT_ORDER=0x8\n"
            "[unsupported-filter]\n"
            "[pi4]\n"
            "BOOT_ORDER=0xe\n",
        };

        for (size_t i = 0; i < ARRAY_SIZE(inactive_configs); i++) {
            g_clear_pointer(&eeprom, g_free);
            g_clear_pointer(&eeprom_path, g_free);
            eeprom = make_eeprom_image(inactive_configs[i], false);
            write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                             &eeprom_path);
            command = g_strdup_printf(
                "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
                "-drive if=none,id=pieeprom,format=raw,file=%s,"
                "file.locking=off -nic none",
                eeprom_path);
            qts = qtest_init(command);
            g_assert_cmpuint(qom_get_uint32(qts, "boot-order"), ==, 0x8);
            qtest_quit(qts);
            g_clear_pointer(&command, g_free);
            unlink(eeprom_path);
        }
    }
}

static void test_gpt_sd_boot(void)
{
    static const uint8_t firmware[] = "QEMU GPT firmware fixture";
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *state = NULL;
    g_autofree char *source = NULL;
    QTestState *qts = start_with_eeprom_and_sd_ram(
        "BOOT_ORDER=0x1\n", "START4  ELF", firmware, sizeof(firmware),
        NULL, true, 2, 2, &eeprom_path, &sd_path);
    uint8_t crc_byte;
    int fd;

    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "boot-source");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(source, ==, "sd-card");
    qtest_quit(qts);

    fd = open(sd_path, O_RDWR);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(pread(fd, &crc_byte, 1, 512 + 16), ==, 1);
    crc_byte ^= 1;
    g_assert_cmpint(pwrite(fd, &crc_byte, 1, 512 + 16), ==, 1);
    g_assert_cmpint(fsync(fd), ==, 0);
    close(fd);

    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=sd,format=raw,file=%s,file.locking=off -nic none",
        eeprom_path, sd_path);
    qts = qtest_init(command);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "fatal-error-reboot-wait");
    qtest_quit(qts);
    unlink(eeprom_path);
    unlink(sd_path);
}

static void test_logical_mbr_sd_boot(void)
{
    static const uint8_t firmware5[] = "QEMU logical partition five";
    static const uint8_t firmware6[] = "QEMU logical partition six";
    static const struct {
        const char *config;
        unsigned int partition;
    } cases[] = {
        { "BOOT_ORDER=0x1\n", 5 },
        { "BOOT_ORDER=0x1\nPARTITION=6\n", 6 },
    };
    g_autofree uint8_t *partition5 = make_usb_boot_image(
        "START4  ELF", firmware5, sizeof(firmware5), NULL);
    g_autofree uint8_t *partition6 = make_usb_boot_image(
        "START4  ELF", firmware6, sizeof(firmware6), NULL);
    g_autofree uint8_t *sd =
        make_logical_boot_image(partition5, partition6);
    g_autofree char *sd_path = NULL;

    write_temp_image("raspi4-logical-sd-XXXXXX", sd, AB_SD_SIZE, &sd_path);
    for (unsigned int i = 0; i < ARRAY_SIZE(cases); i++) {
        const uint8_t *firmware = firmware5;
        size_t firmware_size = sizeof(firmware5);
        g_autofree uint8_t *eeprom =
            make_eeprom_image(cases[i].config, false);
        g_autofree char *expected_hash = NULL;
        g_autofree char *eeprom_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *state = NULL;
        g_autofree char *hash = NULL;
        QTestState *qts;

        if (cases[i].partition == 6) {
            firmware = firmware6;
            firmware_size = sizeof(firmware6);
        }
        expected_hash = g_compute_checksum_for_data(
            G_CHECKSUM_SHA256, firmware, firmware_size);
        write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                         &eeprom_path);
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
            "-drive if=none,id=pieeprom,format=raw,file=%s,"
            "file.locking=off "
            "-drive if=sd,format=raw,file=%s,file.locking=off -nic none",
            eeprom_path, sd_path);
        qts = qtest_init(command);
        state = qom_get_string(qts, "boot-state");
        hash = qom_get_string(qts, "firmware-sha256");
        g_assert_cmpstr(state, ==, "arm-handoff-ready");
        g_assert_cmpstr(hash, ==, expected_hash);
        g_assert_cmpuint(qom_get_uint32(qts, "selected-boot-partition"),
                         ==, cases[i].partition);
        assert_bootloader_device_tree(qts, 1, cases[i].partition, 0);
        qtest_quit(qts);
        unlink(eeprom_path);
    }
    unlink(sd_path);
}


static void test_logical_mbr_rejects_malformed_chains(void)
{
    static const uint8_t firmware5[] = "QEMU logical malformed five";
    static const uint8_t firmware6[] = "QEMU logical malformed six";
    g_autofree uint8_t *partition5 = make_usb_boot_image(
        "START4  ELF", firmware5, sizeof(firmware5), NULL);
    g_autofree uint8_t *partition6 = make_usb_boot_image(
        "START4  ELF", firmware6, sizeof(firmware6), NULL);
    uint8_t *ebr5_link;
    uint8_t *ebr6;
    g_autofree uint8_t *sd =
        make_logical_boot_image(partition5, partition6);

    ebr6 = sd + (uint64_t)(SD_PARTITION_LBA + LOGICAL_EBR_GAP) * 512;
    ebr6[446 + 16 + 4] = 0x0f;
    stl_le_p(ebr6 + 446 + 16 + 8, LOGICAL_EBR_GAP);
    stl_le_p(ebr6 + 446 + 16 + 12,
             AB_SD_SIZE / 512 - SD_PARTITION_LBA - LOGICAL_EBR_GAP);
    assert_logical_mbr_rejected(
        sd, "BOOT_ORDER=0x1\nPARTITION=7\nPARTITION_WALK=0\n");

    g_free(g_steal_pointer(&sd));
    sd = make_logical_boot_image(partition5, partition6);
    ebr6 = sd + (uint64_t)(SD_PARTITION_LBA + LOGICAL_EBR_GAP) * 512;
    stl_le_p(ebr6 + 446 + 8, AB_SD_SIZE / 512);
    assert_logical_mbr_rejected(
        sd, "BOOT_ORDER=0x1\nPARTITION=6\nPARTITION_WALK=0\n");

    g_free(g_steal_pointer(&sd));
    sd = make_logical_boot_image(partition5, partition6);
    ebr5_link = sd + (uint64_t)SD_PARTITION_LBA * 512 + 446 + 16;
    ebr5_link[4] = 0x0e;
    assert_logical_mbr_rejected(
        sd, "BOOT_ORDER=0x1\nPARTITION=6\nPARTITION_WALK=0\n");
}

static void test_firmware_config_include_and_prefix(void)
{
    static const uint8_t expected_initramfs[] =
        "QEMU initramfs first\0QEMU initramfs second";
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    QTestState *qts = start_with_complex_firmware_config(
        NULL, &eeprom_path, &sd_path);
    g_autofree char *state = qom_get_string(qts, "boot-state");
    g_autofree char *status = qom_get_string(qts, "firmware-status");
    g_autofree char *handoff_status = qom_get_string(
        qts, "arm-handoff-status");
    g_autofree char *prefix = qom_get_string(qts, "firmware-os-prefix");
    g_autofree char *start = qom_get_string(qts, "firmware-file");
    g_autofree char *initramfs = qom_get_string(
        qts, "firmware-initramfs-file");
    uint64_t initramfs_address = qom_get_uint64(
        qts, "arm-handoff-initramfs-address");
    uint64_t device_tree_address = qom_get_uint64(
        qts, "arm-handoff-device-tree-address");
    uint64_t device_tree_size = qom_get_uint64(
        qts, "arm-handoff-device-tree-size");
    uint8_t actual_initramfs[sizeof(expected_initramfs)];
    g_autofree uint8_t *final_dtb = g_malloc(device_tree_size);
    g_autofree char *overlay_file = qom_get_string(
        qts, "firmware-overlay-file");
    g_autofree char *final_dtb_hash = qom_get_string(
        qts, "firmware-final-device-tree-sha256");
    g_autofree char *wireless_status = qom_get_string(
        qts, "wireless-status");
    g_autofree char *peripheral_policy = qom_get_string(
        qts, "peripheral-model-policy");
    g_autofree char *peripheral_exclusions = qom_get_string(
        qts, "peripheral-exclusions");
    g_autofree char *videocore_mode = qom_get_string(
        qts, "videocore-execution-mode");
    g_autofree char *artifact_policy = qom_get_string(
        qts, "videocore-artifact-policy");
    int node;
    int bootargs_length;
    const char *bootargs;

    g_assert_cmpstr(status, ==, "config-ready");
    g_assert_cmpstr(handoff_status, ==, "ready");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(prefix, ==, "alt/");
    assert_firmware_chosen_string(qts, "os_prefix", "alt/");
    assert_firmware_chosen_string(qts, "overlay_prefix", "");
    g_assert_cmpstr(start, ==, "start4.elf");
    g_assert_cmpstr(wireless_status, ==, "excluded-unmodeled");
    g_assert_cmpstr(peripheral_policy, ==,
                    "preserve-disabled-unmodeled-v1");
    g_assert_cmpstr(
        peripheral_exclusions, ==,
        "brcm,brcm2711-dvp;brcm,bcm2835-mmc;brcm,bcm43438-bt");
    g_assert_cmpstr(videocore_mode, ==, "behavioral-replacement-v1");
    g_assert_cmpstr(artifact_policy, ==,
                    "exact-input-bytes-not-instruction-executed");
    g_assert_cmpuint(qom_get_uint32(
                         qts, "videocore-boundary-version"), ==, 1);
    g_assert_cmpstr(initramfs, ==, "initr-a,initr-b");
    g_assert_cmpuint(qom_get_uint32(qts, "firmware-config-includes"), ==, 1);
    g_assert_cmpuint(qom_get_uint32(qts, "firmware-overlay-count"), ==, 2);
    g_assert_cmpuint(qom_get_uint32(qts, "firmware-dtparam-count"), ==, 7);
    g_assert_cmpuint(qom_get_uint32(qts, "firmware-overlay-applied"), ==, 2);
    g_assert_cmpuint(qom_get_uint32(qts, "firmware-dtparam-applied"), ==,
                     23);
    g_assert_cmpstr(overlay_file, ==, "alt/consumer.dtbo");
    g_assert_cmpuint(strlen(final_dtb_hash), ==, 64);
    g_assert_cmpuint(qom_get_uint32(qts, "firmware-config-ignored"), ==, 3);
    g_assert_cmpuint(qom_get_uint32(qts, "firmware-initramfs-size"), >, 0);
    g_assert_cmpuint(initramfs_address, ==, 0x201000);
    g_assert_cmpuint(device_tree_address, >, initramfs_address);
    qtest_memread(qts, initramfs_address, actual_initramfs,
                  sizeof(actual_initramfs));
    g_assert_cmpmem(actual_initramfs, sizeof(actual_initramfs),
                    expected_initramfs, sizeof(expected_initramfs));
    qtest_memread(qts, device_tree_address, final_dtb, device_tree_size);
    g_assert_cmpint(fdt_check_header(final_dtb), ==, 0);
    node = fdt_node_offset_by_compatible(
        final_dtb, -1, "brcm,bcm2711-thermal");
    g_assert_cmpint(node, >=, 0);
    node = fdt_node_offset_by_compatible(
        final_dtb, -1, "brcm,bcm2711-genet-v5");
    g_assert_cmpint(node, >=, 0);
    {
        int length;
        const uint8_t *mac = fdt_getprop(
            final_dtb, node, "local-mac-address", &length);

        g_assert_nonnull(mac);
        g_assert_cmpint(length, ==, 6);
        g_assert_cmpmem(mac, length,
                        ((const uint8_t[]) { 0xdc, 0xa6, 0x32,
                                            0x01, 0x36, 0xc2 }), 6);
    }
    node = fdt_node_offset_by_compatible(
        final_dtb, -1, "brcm,bcm2835-mmc");
    g_assert_cmpint(node, >=, 0);
    g_assert_cmpstr((const char *)fdt_getprop(
                        final_dtb, node, "status", NULL), ==, "disabled");
    node = fdt_node_offset_by_compatible(
        final_dtb, -1, "brcm,bcm43438-bt");
    g_assert_cmpint(node, >=, 0);
    g_assert_cmpstr((const char *)fdt_getprop(
                        final_dtb, node, "status", NULL), ==, "disabled");
    {
        static const char * const excluded[] = {
            "brcm,brcm2711-dvp",
        };

        for (size_t index = 0; index < ARRAY_SIZE(excluded); index++) {
            node = fdt_node_offset_by_compatible(
                final_dtb, -1, excluded[index]);
            g_assert_cmpint(node, >=, 0);
            g_assert_cmpstr((const char *)fdt_getprop(
                                final_dtb, node, "status", NULL), ==,
                            "disabled");
        }
    }
    node = fdt_node_offset_by_compatible(
        final_dtb, -1, "brcm,bcm2711-l2-intc");
    g_assert_cmpint(node, >=, 0);
    g_assert_cmpstr((const char *)fdt_getprop(
                        final_dtb, node, "status", NULL), ==, "okay");
    node = fdt_node_offset_by_compatible(
        final_dtb, -1, "brcm,bcm2711-hdmi-i2c");
    g_assert_cmpint(node, >=, 0);
    g_assert_cmpstr((const char *)fdt_getprop(
                        final_dtb, node, "status", NULL), ==, "okay");
    node = fdt_path_offset(final_dtb, "/test-peripheral");
    g_assert_cmpint(node, >=, 0);
    g_assert_cmpstr((const char *)fdt_getprop(
                        final_dtb, node, "status", NULL), ==, "okay");
    g_assert_cmpuint(fdt_get_u32(final_dtb, node, "test-value"), ==, 99);
    node = fdt_path_offset(final_dtb, "/chosen");
    g_assert_cmpint(node, >=, 0);
    bootargs = fdt_getprop(final_dtb, node, "bootargs", &bootargs_length);
    g_assert_cmpstr(bootargs, ==,
                    "coherent_pool=1M 8250.nr_uarts=1 "
                    "symbol-flag=1 direct-flag=1 "
                    "overlay-flag=1 "
                    "console=ttyS0 root=PARTUUID=test");
    node = fdt_path_offset(final_dtb, "/overlay-device");
    g_assert_cmpint(node, >=, 0);
    g_assert_cmpstr((const char *)fdt_getprop(
                        final_dtb, node, "status", NULL), ==, "okay");
    g_assert_cmpuint(fdt_get_u32(final_dtb, node, "test-value"), ==, 42);
    {
        static const uint8_t expected_bytes[2] = { 0, 171 };
        static const uint8_t expected_u16[4] = { 0, 0, 0x12, 0x34 };
        static const uint8_t expected_mac[6] = {
            0xb8, 0x27, 0xeb, 0x01, 0x23, 0x45,
        };
        const uint8_t *property;
        int length;

        property = fdt_getprop(final_dtb, node, "test-bytes", &length);
        g_assert_cmpmem(property, length, expected_bytes,
                        sizeof(expected_bytes));
        property = fdt_getprop(final_dtb, node, "test-u16", &length);
        g_assert_cmpmem(property, length, expected_u16,
                        sizeof(expected_u16));
        property = fdt_getprop(final_dtb, node, "test-u64", &length);
        g_assert_cmpuint(ldq_be_p(property), ==,
                         UINT64_C(0x1122334455667788));
        property = fdt_getprop(final_dtb, node, "test-mac", &length);
        g_assert_cmpmem(property, length, expected_mac, sizeof(expected_mac));
        g_assert_nonnull(fdt_getprop(final_dtb, node, "test-bool", NULL));
        g_assert_nonnull(fdt_getprop(final_dtb, node, "test-inverted", NULL));
        g_assert_cmpstr((const char *)fdt_getprop(
                            final_dtb, node, "lookup-name", NULL),
                        ==, "bravo charlie");
        g_assert_cmpstr((const char *)fdt_getprop(
                            final_dtb, node, "lookup-default", NULL),
                        ==, "fallback");
        g_assert_cmpstr((const char *)fdt_getprop(
                            final_dtb, node, "lookup-pass", NULL),
                        ==, "verbatim");
        g_assert_cmpuint(fdt_get_u32(final_dtb, node, "multi-a"), ==, 123);
        g_assert_cmpuint(fdt_get_u32(final_dtb, node, "multi-b"), ==, 123);
        g_assert_nonnull(fdt_getprop(final_dtb, node, "multi-bool-a", NULL));
        g_assert_nonnull(fdt_getprop(final_dtb, node, "multi-bool-b", NULL));
    }
    {
        int linked = fdt_path_offset(final_dtb, "/linked-device");
        int chosen = fdt_path_offset(final_dtb, "/chosen");

        g_assert_cmpint(linked, >=, 0);
        g_assert_cmpint(chosen, >=, 0);
        g_assert_cmpuint(fdt_get_u32(final_dtb, node, "test-link"), ==,
                         fdt_get_phandle(final_dtb, linked));
        g_assert_cmpuint(fdt_get_u32(final_dtb, node, "external-link"), ==,
                         fdt_get_phandle(final_dtb, chosen));
        g_assert_cmpuint(fdt_get_u32(final_dtb, node, "lookup-local"), ==,
                         fdt_get_phandle(final_dtb, linked));
        g_assert_cmpuint(fdt_get_u32(final_dtb, node, "lookup-external"), ==,
                         fdt_get_phandle(final_dtb, chosen));
    }
    node = fdt_path_offset(final_dtb, "/forced-fragment");
    g_assert_cmpint(node, >=, 0);
    node = fdt_path_offset(final_dtb, "/reg-device@2a");
    g_assert_cmpint(node, >=, 0);
    g_assert_cmpuint(fdt_get_u32(final_dtb, node, "reg"), ==, 42);
    node = fdt_path_offset(final_dtb, "/renamed-device");
    g_assert_cmpint(node, >=, 0);
    g_assert_null(fdt_getprop(final_dtb, node, "name", NULL));
    node = fdt_path_offset(final_dtb, "/intra-parent");
    g_assert_cmpint(node, >=, 0);
    g_assert_cmpstr((const char *)fdt_getprop(
                        final_dtb, node, "first-pass", NULL), ==, "parent");
    g_assert_cmpstr((const char *)fdt_getprop(
                        final_dtb, node, "second-pass", NULL), ==, "patched");
    g_assert_cmpstr((const char *)fdt_getprop(
                        final_dtb, node, "export-consumer", NULL),
                    ==, "applied");
    g_assert_cmpint(fdt_subnode_offset(final_dtb, node, "intra-child"),
                    >=, 0);
    node = fdt_path_offset(final_dtb, "/intra-parent/intra-child");
    g_assert_cmpint(node, >=, 0);
    g_assert_cmpstr((const char *)fdt_getprop(
                        final_dtb, node, "third-pass", NULL),
                    ==, "deep-patched");
    g_assert_cmpint(fdt_subnode_offset(final_dtb, node, "deep-child"),
                    >=, 0);
    node = fdt_path_offset(final_dtb, "/__symbols__");
    g_assert_cmpint(node, >=, 0);
    g_assert_cmpstr((const char *)fdt_getprop(
                        final_dtb, node, "public_label", NULL),
                    ==, "/intra-parent");
    g_assert_null(fdt_getprop(final_dtb, node, "private_label", NULL));
    g_assert_cmpstr((const char *)fdt_getprop(
                        final_dtb, node, "chosen_label", NULL),
                    ==, "/chosen");
    qtest_quit(qts);
    unlink(eeprom_path);
    unlink(sd_path);
}

static void test_firmware_bootloader_owned_selection(void)
{
    static const uint8_t firmware[] =
        "QEMU bootloader-owned firmware selection fixture";
    static const struct {
        const char *config;
        const char *firmware_short_name;
        const char *firmware_file;
        bool network;
    } cases[] = {
        { "start_x=1\n", "START4X ELF", "start4x.elf", false },
        { "start_x=1\n", "START_X ELF", "start_x.elf", false },
        { "start_debug=1\n", "START_DBELF", "start_db.elf", false },
        { "gpu_mem=16\n", "START4CDELF", "start4cd.elf", false },
        { "gpu_mem=64\ngpu_mem_1024=16\n",
          "START4CDELF", "start4cd.elf", false },
        { "gpu_mem=16\ngpu_mem_1024=64\n",
          "START4  ELF", "start4.elf", false },
        { "gpu_mem=64\ngpu_mem_256=16\ngpu_mem_512=16\n",
          "START4  ELF", "start4.elf", false },
        { "start_x=1\nstart_file=start4.elf\n"
          "fixup_file=fixup4.dat\n",
          "START4  ELF", "start4.elf", false },
        { "start_x=1\n", "START_X ELF", "start_x.elf", true },
    };
    g_autofree char *expected_hash = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, firmware, sizeof(firmware));

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        const char *boot_order = "BOOT_ORDER=0x1\n";
        const char *expected_source = "sd-card";
        g_autofree uint8_t *eeprom = NULL;
        g_autofree uint8_t *media = make_usb_boot_image(
            cases[i].firmware_short_name, firmware, sizeof(firmware),
            cases[i].config);
        g_autofree char *eeprom_path = NULL;
        g_autofree char *media_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *state = NULL;
        g_autofree char *status = NULL;
        g_autofree char *file = NULL;
        g_autofree char *hash = NULL;
        g_autofree char *source = NULL;
        QTestState *qts;

        if (cases[i].network) {
            boot_order = "BOOT_ORDER=0x2\n";
            expected_source = "network";
        }
        eeprom = make_eeprom_image(boot_order, false);
        write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                         &eeprom_path);
        write_temp_image("raspi4-firmware-pair-XXXXXX", media, SD_SIZE,
                         &media_path);
        if (cases[i].network) {
            command = g_strdup_printf(
                "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
                "network-boot-drive=netboot,network-boot-wire=off "
                "-drive if=none,id=pieeprom,format=raw,file=%s "
                "-drive if=none,id=netboot,format=raw,file=%s",
                eeprom_path, media_path);
        } else {
            command = g_strdup_printf(
                "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
                "-drive if=none,id=pieeprom,format=raw,file=%s "
                "-drive if=sd,format=raw,file=%s -nic none",
                eeprom_path, media_path);
        }
        qts = qtest_init(command);
        state = qom_get_string(qts, "boot-state");
        status = qom_get_string(qts, "firmware-status");
        file = qom_get_string(qts, "firmware-file");
        hash = qom_get_string(qts, "firmware-sha256");
        source = qom_get_string(qts, "boot-source");
        g_assert_cmpstr(state, ==, "arm-handoff-ready");
        g_assert_cmpstr(status, ==, "config-ready");
        g_assert_cmpstr(file, ==, cases[i].firmware_file);
        g_assert_cmpstr(hash, ==, expected_hash);
        g_assert_cmpstr(source, ==, expected_source);
        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(media_path);
    }

}

static void test_firmware_bootloader_owned_include_boundary(void)
{
    static const char included_config[] =
        "[pi4]\n"
        "start_x=1\n"
        "start_debug=1\n"
        "gpu_mem=16\n";
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    QTestState *qts = start_with_complex_firmware_config(
        included_config, &eeprom_path, &sd_path);
    g_autofree char *state = qom_get_string(qts, "boot-state");
    g_autofree char *status = qom_get_string(qts, "firmware-status");
    g_autofree char *file = qom_get_string(qts, "firmware-file");

    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(status, ==, "config-ready");
    g_assert_cmpstr(file, ==, "start4.elf");
    g_assert_cmpuint(qom_get_uint32(qts, "firmware-config-ignored"), ==, 4);
    qtest_quit(qts);
    unlink(eeprom_path);
    unlink(sd_path);
}

static void test_firmware_config_combined_filters_and_include_state(void)
{
    static const char config[] =
        "os_prefix=wrong/\n"
        "[pi4]\n"
        "[board-type=0x11]\n"
        "[0x1]\n"
        "[0x0]\n"
        "[bootvar0=1]\n"
        "[cust_otp0=0]\n"
        "os_prefix=alt/\n"
        "[gpio4=1]\n"
        "os_prefix=wrong/\n"
        "[all]\n"
        "os_prefix=alt/\n"
        "[none]\n"
        "ignored_inside=1\n";
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    QTestState *qts = start_with_complex_firmware_config(
        config, &eeprom_path, &sd_path);
    g_autofree char *state = qom_get_string(qts, "boot-state");
    g_autofree char *prefix = qom_get_string(qts, "firmware-os-prefix");

    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(prefix, ==, "alt/");
    /*
     * [none] in the included file remains active when parsing resumes in
     * config.txt, so neither ignored_inside nor caller_probe is counted.
     */
    g_assert_cmpuint(qom_get_uint32(
                         qts, "firmware-config-ignored"), ==, 0);
    qtest_quit(qts);
    unlink(eeprom_path);
    unlink(sd_path);
}

static void test_firmware_config_gpio_filter_reset(void)
{
    static const char config[] =
        "[all]\n"
        "[gpio4=1]\n"
        "dtoverlay=vc4-kms-v3d\n";
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree char *state = NULL;
    QTestState *qts = start_with_complex_firmware_config(
        config, &eeprom_path, &sd_path);

    g_assert_cmpuint(qom_get_uint32(
                         qts, "firmware-overlay-applied"), ==, 0);

    qtest_set_irq_in(qts, GPIO_QOM_PATH, "pin-input", 4, 1);
    qtest_system_reset(qts);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    assert_firmware_chosen_string(qts, "os_prefix", "alt/");
    assert_firmware_chosen_string(qts, "overlay_prefix", "");
    g_assert_cmpuint(qom_get_uint32(
                         qts, "firmware-overlay-applied"), ==, 1);

    qtest_set_irq_in(qts, GPIO_QOM_PATH, "pin-input", 4, 0);
    qtest_system_reset(qts);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpuint(qom_get_uint32(
                         qts, "firmware-overlay-applied"), ==, 0);

    qtest_quit(qts);
    unlink(eeprom_path);
    unlink(sd_path);
}

static void test_firmware_config_edid_filters_and_migration(void)
{
    static const char config[] =
        "[EDID=DEL-DELL_U2422H]\n"
        "edid0_match=1\n"
        "[pi4]\n"
        "combined_match=1\n"
        "[EDID=ACR-SECOND_PANEL]\n"
        "edid1_match=1\n"
        "[all]\n";
    uint8_t hdmi0[128];
    uint8_t hdmi1[128];
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree char *hdmi0_path = NULL;
    g_autofree char *hdmi1_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    g_autofree char *status0 = NULL;
    g_autofree char *status1 = NULL;
    g_autofree char *name0 = NULL;
    g_autofree char *name1 = NULL;
    QTestState *source;
    QTestState *destination;

    make_edid(hdmi0, "DEL", "DELL U2422H");
    make_edid(hdmi1, "ACR", "SECOND PANEL");
    write_temp_image("raspi4-hdmi0-edid-XXXXXX", hdmi0, sizeof(hdmi0),
                     &hdmi0_path);
    write_temp_image("raspi4-hdmi1-edid-XXXXXX", hdmi1, sizeof(hdmi1),
                     &hdmi1_path);
    source = start_with_complex_firmware_config(
        config, &eeprom_path, &sd_path);
    qom_set_string(source, "hdmi0-edid-file", hdmi0_path);
    qom_set_string(source, "hdmi1-edid-file", hdmi1_path);
    qtest_system_reset(source);

    status0 = qom_get_string(source, "hdmi0-edid-status");
    status1 = qom_get_string(source, "hdmi1-edid-status");
    name0 = qom_get_string(source, "hdmi0-edid-name");
    name1 = qom_get_string(source, "hdmi1-edid-name");
    g_assert_cmpstr(status0, ==, "valid");
    g_assert_cmpstr(status1, ==, "valid");
    g_assert_cmpstr(name0, ==, "DEL-DELL_U2422H");
    g_assert_cmpstr(name1, ==, "ACR-SECOND_PANEL");
    g_assert_cmpuint(qom_get_uint32(
                         source, "firmware-config-ignored"), ==, 4);

    /*
     * Make the destination's reset-time inputs disagree before incoming
     * migration.  VMState must restore the source's already sampled names.
     */
    make_edid(hdmi0, "ACR", "OTHER");
    hdmi1[127]++;
    overwrite_image(hdmi0_path, hdmi0, sizeof(hdmi0));
    overwrite_image(hdmi1_path, hdmi1, sizeof(hdmi1));
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "hdmi0-edid-file=%s,hdmi1-edid-file=%s "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=sd,format=raw,file=%s,file.locking=off",
        hdmi0_path, hdmi1_path, eeprom_path, sd_path);
    destination = migrate_to_new_qtest(
        source, command, &migration_dir, &migration_socket);
    qtest_quit(source);

    g_clear_pointer(&status0, g_free);
    g_clear_pointer(&status1, g_free);
    g_clear_pointer(&name0, g_free);
    g_clear_pointer(&name1, g_free);
    status0 = qom_get_string(destination, "hdmi0-edid-status");
    status1 = qom_get_string(destination, "hdmi1-edid-status");
    name0 = qom_get_string(destination, "hdmi0-edid-name");
    name1 = qom_get_string(destination, "hdmi1-edid-name");
    g_assert_cmpstr(status0, ==, "valid");
    g_assert_cmpstr(status1, ==, "valid");
    g_assert_cmpstr(name0, ==, "DEL-DELL_U2422H");
    g_assert_cmpstr(name1, ==, "ACR-SECOND_PANEL");

    qtest_system_reset(destination);
    g_clear_pointer(&status0, g_free);
    g_clear_pointer(&status1, g_free);
    g_clear_pointer(&name0, g_free);
    status0 = qom_get_string(destination, "hdmi0-edid-status");
    status1 = qom_get_string(destination, "hdmi1-edid-status");
    name0 = qom_get_string(destination, "hdmi0-edid-name");
    g_assert_cmpstr(status0, ==, "valid");
    g_assert_cmpstr(status1, ==, "invalid");
    g_assert_cmpstr(name0, ==, "ACR-OTHER");
    g_assert_cmpuint(qom_get_uint32(
                         destination, "firmware-config-ignored"), ==, 1);

    qtest_quit(destination);
    unlink(migration_socket);
    rmdir(migration_dir);
    unlink(hdmi0_path);
    unlink(hdmi1_path);
    unlink(eeprom_path);
    unlink(sd_path);
}



static void test_firmware_manifest_rejects_missing_files(void)
{
    static const uint8_t firmware[] = "QEMU firmware fixture";
    static const struct {
        const char *config;
        const char *status;
    } cases[] = {
        { "kernel=missing.img\n", "kernel-missing" },
        { "initramfs missing-a,missing-b followkernel\n",
          "initramfs-missing" },
        { "initramfs first,,third followkernel\n", "config-invalid" },
        { "initramfs a,b,c,d,e,f,g,h,i followkernel\n",
          "config-invalid" },
        { "start_file=start4.elf\n", "config-invalid" },
        { "start_x=2\n", "config-invalid" },
        { "start_debug=-1\n", "config-invalid" },
        { "gpu_mem=invalid\n", "config-invalid" },
        { "gpu_mem_256=invalid\n", "config-invalid" },
        { "gpu_mem_512=invalid\n", "config-invalid" },
        { "gpu_mem_1024=1024\n", "config-invalid" },
        { "total_mem=128\ngpu_mem=128\n", "config-invalid" },
        { "total_mem=invalid\n", "config-invalid" },
        { "device_tree_address=invalid\n", "config-invalid" },
        { "device_tree_end=-1\n", "config-invalid" },
        { "start_file=start4cd.elf\nfixup_file=fixup4cd.dat\n",
          "config-invalid" },
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree char *eeprom_path = NULL;
        g_autofree char *sd_path = NULL;
        QTestState *qts = start_with_eeprom_and_sd(
            "BOOT_ORDER=0xe1\n", "START4  ELF", firmware,
            sizeof(firmware), cases[i].config, &eeprom_path, &sd_path);
        g_autofree char *state = qom_get_string(qts, "boot-state");
        g_autofree char *status = qom_get_string(qts, "firmware-status");

        g_assert_cmpstr(state, ==, "stopped");
        g_assert_cmpstr(status, ==, cases[i].status);
        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(sd_path);
    }

}

static void test_arm_handoff_rejects_invalid_artifacts(void)
{
    static const uint8_t firmware[] = "QEMU firmware fixture";
    static const struct {
        const char *config;
        const char *status;
    } cases[] = {
        { "kernel=fixup4.dat\n", "kernel-invalid" },
        { "device_tree=fixup4.dat\n", "device-tree-invalid" },
        { "initramfs fixup4.dat 0x200000\n", "layout-invalid" },
        { "dtparam=unknown=1\n", "overlay-parameter-invalid" },
        { "dtoverlay=missing\n", "overlay-missing" },
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree char *eeprom_path = NULL;
        g_autofree char *sd_path = NULL;
        QTestState *qts = start_with_eeprom_and_sd(
            "BOOT_ORDER=0xe1\n", "START4  ELF", firmware,
            sizeof(firmware), cases[i].config, &eeprom_path, &sd_path);
        g_autofree char *state = qom_get_string(qts, "boot-state");
        g_autofree char *status = qom_get_string(
            qts, "arm-handoff-status");

        g_assert_cmpstr(state, ==, "stopped");
        g_assert_cmpstr(status, ==, cases[i].status);
        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(sd_path);
    }
    {
        static const char * const malformed_parameters[] = {
            "malformed_cell=on",
            "lookup_nomatch=z",
            "lookup_bad_tail=a",
            "lookup_truncated_cell=local",
        };

        for (size_t i = 0; i < ARRAY_SIZE(malformed_parameters); i++) {
            g_autofree char *malformed_extra = g_strdup_printf(
                "[pi4]\ndtoverlay=vc4-kms-v3d,%s\n",
                malformed_parameters[i]);
            g_autofree char *eeprom_path = NULL;
            g_autofree char *sd_path = NULL;
            QTestState *qts = start_with_complex_firmware_config(
                malformed_extra, &eeprom_path, &sd_path);
            g_autofree char *state = qom_get_string(qts, "boot-state");
            g_autofree char *status = qom_get_string(
                qts, "arm-handoff-status");

            g_assert_cmpstr(state, ==, "restart-loop");
            g_assert_cmpstr(status, ==, "overlay-parameter-invalid");
            qtest_quit(qts);
            unlink(eeprom_path);
            unlink(sd_path);
        }
    }
    {
        static const TestOverlayExportVariant malformed_exports[] = {
            TEST_EXPORT_NONEMPTY,
            TEST_EXPORT_MISSING_SYMBOL,
            TEST_EXPORT_COLLISION,
            TEST_EXPORT_INTRA_CYCLE,
        };

        for (size_t i = 0; i < ARRAY_SIZE(malformed_exports); i++) {
            g_autofree char *eeprom_path = NULL;
            g_autofree char *sd_path = NULL;
            QTestState *qts = start_with_complex_firmware_config_variant(
                "[pi4]\ndtoverlay=vc4-kms-v3d\n",
                malformed_exports[i], NULL, 0,
                false,
                &eeprom_path, &sd_path, NULL);
            g_autofree char *state = qom_get_string(qts, "boot-state");
            g_autofree char *status = qom_get_string(
                qts, "arm-handoff-status");

            g_assert_cmpstr(state, ==, "restart-loop");
            g_assert_cmpstr(status, ==, "overlay-apply-error");
            qtest_quit(qts);
            unlink(eeprom_path);
            unlink(sd_path);
        }
    }
}

static void test_arm64_image_header_layout(void)
{
    static const char * const machines[] = {
        "raspi4b",
        "raspi-cm4",
    };

    for (size_t i = 0; i < ARRAY_SIZE(machines); i++) {
        g_autofree uint8_t *kernel = make_arm64_kernel();
        g_autofree char *eeprom_path = NULL;
        g_autofree char *media_path = NULL;
        g_autofree char *migration_dir = NULL;
        g_autofree char *migration_socket = NULL;
        g_autofree char *command = NULL;
        QTestState *source;
        QTestState *destination;

        stq_le_p(kernel + 8, 0x100);
        command = make_custom_kernel_boot_command(
            machines[i], kernel, TEST_KERNEL_SIZE,
            NULL, NULL, 0, NULL, 0,
            &eeprom_path, &media_path);
        source = qtest_init(command);
        {
            g_autofree char *state = qom_get_string(source, "boot-state");
            g_autofree char *status = qom_get_string(
                source, "arm-handoff-status");
            g_autofree char *endianness = qom_get_string(
                source, "arm-handoff-endianness");

            g_assert_cmpstr(state, ==, "arm-handoff-ready");
            g_assert_cmpstr(status, ==, "ready");
            g_assert_cmpstr(endianness, ==, "little");
        }
        g_assert_cmphex(qom_get_uint64(
                            source, "arm-handoff-kernel-address"), ==,
                        0x200100);
        g_assert_cmphex(qom_get_uint64(
                            source, "arm-handoff-entry-address"), ==,
                        0x200100);

        destination = migrate_to_new_qtest(
            source, command, &migration_dir, &migration_socket);
        g_assert_cmphex(qom_get_uint64(
                            destination, "arm-handoff-kernel-address"), ==,
                        0x200100);
        g_assert_cmphex(qom_get_uint64(
                            destination, "arm-handoff-entry-address"), ==,
                        0x200100);
        qtest_system_reset(destination);
        {
            g_autofree char *state = qom_get_string(
                destination, "boot-state");

            g_assert_cmpstr(state, ==, "arm-handoff-ready");
        }
        g_assert_cmphex(qom_get_uint64(
                            destination, "arm-handoff-kernel-address"), ==,
                        0x200100);
        g_assert_cmphex(qom_get_uint64(
                            destination, "arm-handoff-entry-address"), ==,
                        0x200100);

        qtest_quit(source);
        qtest_quit(destination);
        unlink(migration_socket);
        rmdir(migration_dir);
        unlink(eeprom_path);
        unlink(media_path);
    }

    for (size_t i = 0; i < ARRAY_SIZE(machines); i++) {
        static const uint8_t initramfs[64] = {
            0x51, 0x45, 0x4d, 0x55, 0x2d, 0x49, 0x4e, 0x49,
            0x54, 0x52, 0x41, 0x4d, 0x46, 0x53,
        };
        static const struct {
            const char *address;
            bool valid;
            const char *state;
            const char *status;
        } cases[] = {
            { "0x300", false, "stopped", "layout-invalid" },
            { "0x32c", true, "arm-handoff-ready", "ready" },
        };

        for (size_t c = 0; c < ARRAY_SIZE(cases); c++) {
            g_autofree uint8_t *kernel = make_arm64_kernel();
            g_autofree char *config = g_strdup_printf(
                "initramfs initrd.img %s\n", cases[c].address);
            g_autofree char *eeprom_path = NULL;
            g_autofree char *media_path = NULL;
            g_autofree char *migration_dir = NULL;
            g_autofree char *migration_socket = NULL;
            g_autofree char *command = NULL;
            QTestState *source;

            command = make_custom_kernel_boot_command(
                machines[i], kernel, TEST_KERNEL_SIZE, config,
                NULL, 0, initramfs, sizeof(initramfs),
                &eeprom_path, &media_path);
            source = qtest_init(command);
            {
                g_autofree char *state = qom_get_string(
                    source, "boot-state");
                g_autofree char *status = qom_get_string(
                    source, "arm-handoff-status");

                g_assert_cmpstr(state, ==, cases[c].state);
                g_assert_cmpstr(status, ==, cases[c].status);
            }
            if (cases[c].valid) {
                uint8_t loaded[sizeof(initramfs)];
                QTestState *destination;

                g_assert_cmphex(qom_get_uint64(
                                    source,
                                    "arm-handoff-initramfs-address"), ==,
                                0x32c);
                qtest_memread(source, 0x32c, loaded, sizeof(loaded));
                g_assert_cmpmem(loaded, sizeof(loaded),
                                initramfs, sizeof(initramfs));
                destination = migrate_to_new_qtest(
                    source, command, &migration_dir, &migration_socket);
                g_assert_cmphex(qom_get_uint64(
                                    destination,
                                    "arm-handoff-initramfs-address"), ==,
                                0x32c);
                qtest_system_reset(destination);
                qtest_memread(destination, 0x32c, loaded, sizeof(loaded));
                g_assert_cmpmem(loaded, sizeof(loaded),
                                initramfs, sizeof(initramfs));
                qtest_quit(destination);
                unlink(migration_socket);
                rmdir(migration_dir);
            }

            qtest_quit(source);
            unlink(eeprom_path);
            unlink(media_path);
        }
    }

    for (size_t i = 0; i < ARRAY_SIZE(machines); i++) {
        g_autofree uint8_t *kernel = make_arm64_kernel();
        g_autofree char *eeprom_path = NULL;
        g_autofree char *media_path = NULL;
        g_autofree char *migration_dir = NULL;
        g_autofree char *migration_socket = NULL;
        g_autofree char *command = NULL;
        QTestState *source;
        QTestState *destination;

        stq_le_p(kernel + 24, BIT_ULL(0));
        command = make_custom_kernel_boot_command(
            machines[i], kernel, TEST_KERNEL_SIZE,
            NULL, NULL, 0, NULL, 0,
            &eeprom_path, &media_path);
        source = qtest_init(command);
        {
            g_autofree char *state = qom_get_string(source, "boot-state");
            g_autofree char *endianness = qom_get_string(
                source, "arm-handoff-endianness");

            g_assert_cmpstr(state, ==, "arm-handoff-ready");
            g_assert_cmpstr(endianness, ==, "big");
        }
        destination = migrate_to_new_qtest(
            source, command, &migration_dir, &migration_socket);
        {
            g_autofree char *endianness = qom_get_string(
                destination, "arm-handoff-endianness");

            g_assert_cmpstr(endianness, ==, "big");
        }
        qtest_system_reset(destination);
        {
            g_autofree char *state = qom_get_string(
                destination, "boot-state");
            g_autofree char *endianness = qom_get_string(
                destination, "arm-handoff-endianness");

            g_assert_cmpstr(state, ==, "arm-handoff-ready");
            g_assert_cmpstr(endianness, ==, "big");
        }

        qtest_quit(source);
        qtest_quit(destination);
        unlink(migration_socket);
        rmdir(migration_dir);
        unlink(eeprom_path);
        unlink(media_path);
    }

    for (size_t i = 0; i < ARRAY_SIZE(machines); i++) {
        uint8_t truncated_header[63] = { 0 };
        g_autofree char *eeprom_path = NULL;
        g_autofree char *media_path = NULL;
        g_autofree char *command = NULL;
        QTestState *qts;

        stl_le_p(truncated_header, 0x14000000);
        stq_le_p(truncated_header + 16, sizeof(truncated_header));
        memcpy(truncated_header + 56, "ARM\x64", 4);
        command = make_custom_kernel_boot_command(
            machines[i], truncated_header, sizeof(truncated_header),
            NULL, NULL, 0, NULL, 0,
            &eeprom_path, &media_path);
        qts = qtest_init(command);
        {
            g_autofree char *state = qom_get_string(qts, "boot-state");
            g_autofree char *status = qom_get_string(
                qts, "arm-handoff-status");

            g_assert_cmpstr(state, ==, "stopped");
            g_assert_cmpstr(status, ==, "kernel-invalid");
        }
        g_assert_cmphex(qom_get_uint64(
                            qts, "arm-handoff-kernel-address"), ==, 0);
        g_assert_cmphex(qom_get_uint64(
                            qts, "arm-handoff-entry-address"), ==, 0);

        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(media_path);
    }
}

static void test_device_tree_explicit_layout(void)
{
    static const char * const machines[] = {
        "raspi4b",
        "raspi-cm4",
    };

    for (size_t i = 0; i < ARRAY_SIZE(machines); i++) {
        g_autofree uint8_t *kernel = make_arm64_kernel();
        g_autofree char *eeprom_path = NULL;
        g_autofree char *media_path = NULL;
        g_autofree char *migration_dir = NULL;
        g_autofree char *migration_socket = NULL;
        g_autofree char *command = NULL;
        static const char config[] =
            "device_tree_address=0x400000\n"
            "device_tree_end=0x600000\n";
        uint8_t magic[4];
        QTestState *source;
        QTestState *destination;

        command = make_custom_kernel_boot_command(
            machines[i], kernel, TEST_KERNEL_SIZE, config,
            NULL, 0, NULL, 0, &eeprom_path, &media_path);
        source = qtest_init(command);
        {
            g_autofree char *state = qom_get_string(source, "boot-state");
            g_autofree char *status = qom_get_string(
                source, "arm-handoff-status");

            g_assert_cmpstr(state, ==, "arm-handoff-ready");
            g_assert_cmpstr(status, ==, "ready");
        }
        g_assert_cmphex(qom_get_uint64(
                            source, "arm-handoff-device-tree-address"), ==,
                        0x400000);
        qtest_memread(source, 0x400000, magic, sizeof(magic));
        g_assert_cmpmem(magic, sizeof(magic), "\xd0\x0d\xfe\xed", 4);

        destination = migrate_to_new_qtest(
            source, command, &migration_dir, &migration_socket);
        g_assert_cmphex(qom_get_uint64(
                            destination,
                            "arm-handoff-device-tree-address"), ==,
                        0x400000);
        qtest_system_reset(destination);
        g_assert_cmphex(qom_get_uint64(
                            destination,
                            "arm-handoff-device-tree-address"), ==,
                        0x400000);
        qtest_memread(destination, 0x400000, magic, sizeof(magic));
        g_assert_cmpmem(magic, sizeof(magic), "\xd0\x0d\xfe\xed", 4);

        qtest_quit(source);
        qtest_quit(destination);
        unlink(migration_socket);
        rmdir(migration_dir);
        unlink(eeprom_path);
        unlink(media_path);
    }

    for (size_t i = 0; i < ARRAY_SIZE(machines); i++) {
        g_autofree uint8_t *kernel = make_arm64_kernel();
        g_autofree char *eeprom_path = NULL;
        g_autofree char *media_path = NULL;
        g_autofree char *command = make_custom_kernel_boot_command(
            machines[i], kernel, TEST_KERNEL_SIZE,
            "device_tree_end=0x500000\n",
            NULL, 0, NULL, 0, &eeprom_path, &media_path);
        g_autofree char *state = NULL;
        uint64_t expected;
        uint64_t size;
        QTestState *qts;

        qts = qtest_init(command);
        size = qom_get_uint64(qts, "arm-handoff-device-tree-size");
        expected = QEMU_ALIGN_DOWN(0x500000 - size, 0x100);
        state = qom_get_string(qts, "boot-state");

        g_assert_cmpstr(state, ==, "arm-handoff-ready");
        g_assert_cmphex(qom_get_uint64(
                            qts, "arm-handoff-device-tree-address"), ==,
                        expected);
        g_assert_cmpuint(expected + size, <=, 0x500000);
        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(media_path);
    }

    for (size_t i = 0; i < ARRAY_SIZE(machines); i++) {
        static const char * const invalid_layouts[] = {
            "device_tree_address=0x200000\n"
            "device_tree_end=0x600000\n",
            "device_tree_address=0x400000\n"
            "device_tree_end=0x400001\n",
            "device_tree_end=0x100\n",
        };

        for (size_t c = 0; c < ARRAY_SIZE(invalid_layouts); c++) {
            g_autofree uint8_t *kernel = make_arm64_kernel();
            g_autofree char *eeprom_path = NULL;
            g_autofree char *media_path = NULL;
            g_autofree char *command = make_custom_kernel_boot_command(
                machines[i], kernel, TEST_KERNEL_SIZE, invalid_layouts[c],
                NULL, 0, NULL, 0, &eeprom_path, &media_path);
            QTestState *qts = qtest_init(command);
            g_autofree char *status = qom_get_string(
                qts, "arm-handoff-status");

            g_assert_cmpstr(status, ==, "layout-invalid");
            g_assert_cmphex(qom_get_uint64(
                                qts,
                                "arm-handoff-device-tree-address"), ==, 0);
            qtest_quit(qts);
            unlink(eeprom_path);
            unlink(media_path);
        }
    }
}

static void test_arm32_firmware_state_layout(void)
{
    static const char * const machines[] = {
        "raspi4b",
        "raspi-cm4",
    };
    static const uint8_t initramfs[64] = {
        0x51, 0x45, 0x4d, 0x55, 0x2d, 0x41, 0x52, 0x4d,
        0x33, 0x32, 0x2d, 0x49, 0x4e, 0x49, 0x54, 0x52,
        0x41, 0x4d, 0x46, 0x53,
    };
    static const struct {
        const char *address;
        bool valid;
        const char *state;
        const char *status;
    } cases[] = {
        { "0x4", false, "restart-loop", "layout-invalid" },
        { "0xd8", false, "restart-loop", "layout-invalid" },
        { "0x300", false, "restart-loop", "layout-invalid" },
        { "0x328", true, "arm-handoff-ready", "ready" },
    };

    for (size_t i = 0; i < ARRAY_SIZE(machines); i++) {
        for (size_t c = 0; c < ARRAY_SIZE(cases); c++) {
            g_autofree char *config = g_strdup_printf(
                "initramfs initrd.img %s\n", cases[c].address);
            g_autofree char *eeprom_path = NULL;
            g_autofree char *media_path = NULL;
            QTestState *source = start_arm32_memory_model(
                machines[i], 2, config, initramfs, sizeof(initramfs),
                &eeprom_path, &media_path);

            {
                g_autofree char *state = qom_get_string(
                    source, "boot-state");
                g_autofree char *status = qom_get_string(
                    source, "arm-handoff-status");

                g_assert_cmpstr(state, ==, cases[c].state);
                g_assert_cmpstr(status, ==, cases[c].status);
            }
            if (cases[c].valid) {
                uint8_t loaded[sizeof(initramfs)];
                g_autofree char *architecture = qom_get_string(
                    source, "arm-handoff-architecture");
                g_autofree char *migration_dir = NULL;
                g_autofree char *migration_socket = NULL;
                g_autofree char *command = NULL;
                QTestState *destination;

                g_assert_cmpstr(architecture, ==, "aarch32");
                g_assert_cmphex(qom_get_uint32(
                                    source,
                                    "arm-handoff-core-mask"), ==, 0xf);
                g_assert_cmphex(qom_get_uint64(
                                    source,
                                    "arm-handoff-initramfs-address"), ==,
                                0x328);
                qtest_memread(source, 0x328, loaded, sizeof(loaded));
                g_assert_cmpmem(loaded, sizeof(loaded),
                                initramfs, sizeof(initramfs));
                qtest_system_reset(source);
                qtest_memread(source, 0x328, loaded, sizeof(loaded));
                g_assert_cmpmem(loaded, sizeof(loaded),
                                initramfs, sizeof(initramfs));

                if (!strcmp(machines[i], "raspi-cm4")) {
                    command = g_strdup_printf(
                        "-M raspi-cm4,boot-mode=behavioral,"
                        "eeprom-drive=pieeprom,emmc-drive=emmc -m 2G "
                        "-drive if=none,id=pieeprom,format=raw,file=%s,"
                        "file.locking=off "
                        "-drive if=none,id=emmc,format=raw,file=%s,"
                        "file.locking=off -nic none",
                        eeprom_path, media_path);
                } else {
                    command = g_strdup_printf(
                        "-M raspi4b,boot-mode=behavioral,"
                        "eeprom-drive=pieeprom -m 2G "
                        "-drive if=none,id=pieeprom,format=raw,file=%s,"
                        "file.locking=off "
                        "-drive if=sd,format=raw,file=%s,"
                        "file.locking=off -nic none",
                        eeprom_path, media_path);
                }
                destination = migrate_to_new_qtest(
                    source, command, &migration_dir, &migration_socket);
                g_assert_cmphex(qom_get_uint64(
                                    destination,
                                    "arm-handoff-initramfs-address"), ==,
                                0x328);
                g_assert_cmphex(qom_get_uint32(
                                    destination,
                                    "arm-handoff-core-mask"), ==, 0xf);
                qtest_memread(destination, 0x328, loaded, sizeof(loaded));
                g_assert_cmpmem(loaded, sizeof(loaded),
                                initramfs, sizeof(initramfs));
                qtest_quit(destination);
                unlink(migration_socket);
                rmdir(migration_dir);
            }

            qtest_quit(source);
            unlink(eeprom_path);
            unlink(media_path);
        }
    }
}

static void test_cmdline_first_line_semantics(void)
{
    static const char * const machines[] = {
        "raspi4b",
        "raspi-cm4",
    };
    static const uint8_t lf_cmdline[] =
        "console=serial0,115200 root=first\nroot=second";
    static const uint8_t crlf_cmdline[] =
        "console=serial0,115200 root=first\r\nroot=second";
    static const uint8_t trailing_nul_cmdline[] = "root=first";
    static const uint8_t embedded_nul_cmdline[] =
        "root=first\0root=second";
    static const struct {
        const uint8_t *contents;
        size_t size;
        const char *state;
        const char *status;
        const char *bootargs;
    } cases[] = {
        {
            lf_cmdline, sizeof(lf_cmdline) - 1,
            "arm-handoff-ready", "ready",
            "coherent_pool=1M 8250.nr_uarts=1 "
            "console=ttyS0,115200 root=first",
        },
        {
            crlf_cmdline, sizeof(crlf_cmdline) - 1,
            "arm-handoff-ready", "ready",
            "coherent_pool=1M 8250.nr_uarts=1 "
            "console=ttyS0,115200 root=first",
        },
        {
            trailing_nul_cmdline, sizeof(trailing_nul_cmdline),
            "arm-handoff-ready", "ready",
            "coherent_pool=1M 8250.nr_uarts=1 root=first",
        },
        {
            embedded_nul_cmdline, sizeof(embedded_nul_cmdline) - 1,
            "stopped", "device-tree-invalid", NULL,
        },
    };

    for (size_t i = 0; i < ARRAY_SIZE(machines); i++) {
        for (size_t c = 0; c < ARRAY_SIZE(cases); c++) {
            g_autofree uint8_t *kernel = make_arm64_kernel();
            g_autofree char *eeprom_path = NULL;
            g_autofree char *media_path = NULL;
            g_autofree char *migration_dir = NULL;
            g_autofree char *migration_socket = NULL;
            g_autofree char *command = NULL;
            g_autofree char *expected_sha256 =
                g_compute_checksum_for_data(
                    G_CHECKSUM_SHA256,
                    cases[c].contents, cases[c].size);
            QTestState *source;

            command = make_custom_kernel_boot_command(
                machines[i], kernel, TEST_KERNEL_SIZE,
                NULL, cases[c].contents, cases[c].size, NULL, 0,
                &eeprom_path, &media_path);
            source = qtest_init(command);
            {
                g_autofree char *state = qom_get_string(
                    source, "boot-state");
                g_autofree char *status = qom_get_string(
                    source, "arm-handoff-status");
                g_autofree char *sha256 = qom_get_string(
                    source, "firmware-cmdline-sha256");

                g_assert_cmpstr(state, ==, cases[c].state);
                g_assert_cmpstr(status, ==, cases[c].status);
                g_assert_cmpstr(sha256, ==, expected_sha256);
                g_assert_cmpuint(qom_get_uint32(
                                     source,
                                     "firmware-cmdline-size"), ==,
                                 cases[c].size);
            }
            if (cases[c].bootargs) {
                g_autofree uint8_t *fdt = test_read_handoff_dtb(source);
                QTestState *destination;
                int chosen = fdt_path_offset(fdt, "/chosen");

                g_assert_cmpint(chosen, >=, 0);
                g_assert_cmpstr((const char *)fdt_getprop(
                                    fdt, chosen, "bootargs", NULL), ==,
                                cases[c].bootargs);
                g_assert_null(strstr(
                    (const char *)fdt_getprop(
                        fdt, chosen, "bootargs", NULL),
                    "root=second"));
                qtest_system_reset(source);
                g_clear_pointer(&fdt, g_free);
                fdt = test_read_handoff_dtb(source);
                chosen = fdt_path_offset(fdt, "/chosen");
                g_assert_cmpstr((const char *)fdt_getprop(
                                    fdt, chosen, "bootargs", NULL), ==,
                                cases[c].bootargs);

                destination = migrate_to_new_qtest(
                    source, command, &migration_dir, &migration_socket);
                g_clear_pointer(&fdt, g_free);
                fdt = test_read_handoff_dtb(destination);
                chosen = fdt_path_offset(fdt, "/chosen");
                g_assert_cmpstr((const char *)fdt_getprop(
                                    fdt, chosen, "bootargs", NULL), ==,
                                cases[c].bootargs);
                {
                    g_autofree char *sha256 = qom_get_string(
                        destination, "firmware-cmdline-sha256");

                    g_assert_cmpstr(sha256, ==, expected_sha256);
                }
                qtest_quit(destination);
                unlink(migration_socket);
                rmdir(migration_dir);
            }

            qtest_quit(source);
            unlink(eeprom_path);
            unlink(media_path);
        }
    }
}

static void test_special_boot_order_states(void)
{
    static const struct {
        const char *config;
        const char *state;
        const char *source;
    } cases[] = {
        { "BOOT_ORDER=0xe\n", "stopped", "stop" },
        { "BOOT_ORDER=0xf\n", "restart-loop", "restart" },
        { "BOOT_ORDER=0x8\n", "fatal-error-reboot-wait", "reserved-0x8" },
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree char *path = NULL;
        QTestState *qts = start_with_eeprom(cases[i].config, false, &path);
        g_autofree char *state = qom_get_string(qts, "boot-state");
        g_autofree char *source = qom_get_string(qts, "boot-source");

        g_assert_cmpstr(state, ==, cases[i].state);
        g_assert_cmpstr(source, ==, cases[i].source);
        qtest_quit(qts);
        unlink(path);
    }
}

static void test_network_install_request_policy(void)
{
    static const struct {
        const char *config;
        bool requested;
        bool enabled;
        const char *state;
        const char *source;
        uint64_t attempts;
    } cases[] = {
        {
            "BOOT_ORDER=0xe\n"
            "NET_INSTALL_ENABLED=0\n"
            "NET_INSTALL_AT_POWER_ON=1\n",
            true, true, "network-dhcp-wait", "network", 1,
        },
        {
            "BOOT_ORDER=0xe\n"
            "NET_INSTALL_ENABLED=1\n"
            "NET_INSTALL_AT_POWER_ON=1\n",
            false, true, "stopped", "stop", 0,
        },
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree uint8_t *eeprom =
            make_eeprom_image(cases[i].config, false);
        g_autofree char *eeprom_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *state = NULL;
        g_autofree char *source = NULL;
        const char *requested;
        QTestState *qts;
        QDict *response;

        if (cases[i].requested) {
            requested = "on";
        } else {
            requested = "off";
        }
        write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                         &eeprom_path);
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "net-install-requested=%s "
            "-drive if=none,id=pieeprom,format=raw,file=%s "
            "-nic none",
            requested, eeprom_path);
        qts = qtest_init(command);
        state = qom_get_string(qts, "boot-state");
        source = qom_get_string(qts, "boot-source");

        g_assert_cmpstr(state, ==, cases[i].state);
        g_assert_cmpstr(source, ==, cases[i].source);
        g_assert_cmpuint(qom_get_uint32(qts, "boot-order"), ==, 0xe);
        g_assert_cmpuint(qom_get_uint64(qts, "boot-attempt-count"), ==,
                         cases[i].attempts);
        g_assert_cmpint(qom_get_bool(qts, "net-install-requested"), ==,
                        cases[i].requested);
        g_assert_cmpint(qom_get_bool(qts, "boot-net-install-enabled"), ==,
                        cases[i].enabled);
        g_assert_true(qom_get_bool(qts, "boot-net-install-at-power-on"));

        response = qtest_qmp_assert_failure_ref(
            qts, "{ 'execute': 'qom-set', 'arguments': {"
                 "'path': '/machine', "
                 "'property': 'boot-net-install-enabled', "
                 "'value': true } }");
        qobject_unref(response);
        response = qtest_qmp_assert_failure_ref(
            qts, "{ 'execute': 'qom-set', 'arguments': {"
                 "'path': '/machine', "
                 "'property': 'boot-net-install-at-power-on', "
                 "'value': false } }");
        qobject_unref(response);

        qtest_quit(qts);
        unlink(eeprom_path);
    }

    {
        g_autofree uint8_t *eeprom =
            make_eeprom_image("BOOT_ORDER=0xe\n", false);
        g_autofree char *eeprom_path = NULL;
        g_autofree char *command = NULL;
        QTestState *qts;

        write_temp_image("raspi4-cm4-net-install-XXXXXX", eeprom,
                         EEPROM_SIZE, &eeprom_path);
        command = g_strdup_printf(
            "-M raspi-cm4,boot-mode=behavioral,eeprom-drive=pieeprom "
            "-drive if=none,id=pieeprom,format=raw,file=%s -nic none",
            eeprom_path);
        qts = qtest_init(command);
        g_assert_false(qom_get_bool(qts, "boot-net-install-enabled"));
        g_assert_cmpuint(
            qom_get_uint32(qts, "boot-net-install-keyboard-wait-ms"),
            ==, 900);
        qtest_quit(qts);
        unlink(eeprom_path);
    }
}

static void test_network_install_keyboard_wait(void)
{
    g_autofree uint8_t *eeprom = make_eeprom_image(
        "BOOT_ORDER=0xe\n"
        "NET_INSTALL_ENABLED=1\n"
        "NET_INSTALL_KEYBOARD_WAIT=1000\n", false);
    g_autofree char *eeprom_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *state = NULL;
    g_autofree char *source = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    QTestState *qts;
    QTestState *destination;

    write_temp_image("raspi4-eeprom-net-install-XXXXXX", eeprom,
                     EEPROM_SIZE, &eeprom_path);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "net-install-keyboard-present=on "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-nic none",
        eeprom_path);
    qts = qtest_init(command);

    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "boot-source");
    g_assert_cmpstr(state, ==, "net-install-keyboard-wait");
    g_assert_cmpstr(source, ==, "keyboard");
    g_assert_true(qom_get_bool(qts, "net-install-keyboard-waiting"));
    g_assert_true(qom_get_bool(qts, "net-install-keyboard-present"));
    g_assert_false(qom_get_bool(qts, "net-install-shift-held"));
    g_assert_cmpuint(
        qom_get_uint32(qts, "boot-net-install-keyboard-wait-ms"),
        ==, 1000);
    g_assert_cmpuint(
        qom_get_uint64(qts, "net-install-keyboard-remaining-ns"),
        ==, INT64_C(1000) * 1000 * 1000);

    qtest_clock_step(qts, INT64_C(400) * 1000 * 1000);
    destination = migrate_to_new_qtest(
        qts, command, &migration_dir, &migration_socket);
    qtest_quit(qts);
    qts = destination;
    g_assert_true(qom_get_bool(qts, "net-install-keyboard-waiting"));
    g_assert_cmpuint(
        qom_get_uint64(qts, "net-install-keyboard-remaining-ns"),
        ==, INT64_C(600) * 1000 * 1000);

    qtest_clock_step(qts, INT64_C(599) * 1000 * 1000);
    g_assert_true(qom_get_bool(qts, "net-install-keyboard-waiting"));
    qtest_clock_step(qts, INT64_C(1) * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    g_clear_pointer(&source, g_free);
    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "boot-source");
    g_assert_cmpstr(state, ==, "stopped");
    g_assert_cmpstr(source, ==, "stop");
    g_assert_false(qom_get_bool(qts, "net-install-keyboard-waiting"));
    g_assert_cmpuint(qom_get_uint64(qts, "boot-elapsed-ms"), ==, 1000);

    qtest_system_reset(qts);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "net-install-keyboard-wait");
    qom_set_bool(qts, "net-install-shift-held", true);
    g_clear_pointer(&state, g_free);
    g_clear_pointer(&source, g_free);
    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "boot-source");
    g_assert_false(qom_get_bool(qts, "net-install-keyboard-waiting"));
    g_assert_cmpstr(source, ==, "network");
    g_assert_cmpstr(state, !=, "stopped");

    qtest_quit(qts);
    unlink(migration_socket);
    rmdir(migration_dir);
    unlink(eeprom_path);

    {
        static const struct {
            const char *config;
            const char *state;
        } cases[] = {
            {
                "BOOT_ORDER=0xe\n"
                "NET_INSTALL_ENABLED=1\n"
                "NET_INSTALL_KEYBOARD_WAIT=0\n",
                "stopped",
            },
            {
                "BOOT_ORDER=0xe\n"
                "NET_INSTALL_ENABLED=0\n"
                "NET_INSTALL_KEYBOARD_WAIT=1000\n",
                "stopped",
            },
            {
                "BOOT_ORDER=0xe\n"
                "NET_INSTALL_KEYBOARD_WAIT=4294967296\n",
                "eeprom-invalid",
            },
            {
                "BOOT_ORDER=0xe\n"
                "NET_INSTALL_KEYBOARD_WAIT=-1\n",
                "eeprom-invalid",
            },
        };

        for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
            g_autofree uint8_t *case_eeprom =
                make_eeprom_image(cases[i].config, false);
            g_autofree char *case_path = NULL;
            g_autofree char *case_command = NULL;
            g_autofree char *case_state = NULL;

            write_temp_image("raspi4-eeprom-net-install-case-XXXXXX",
                             case_eeprom, EEPROM_SIZE, &case_path);
            case_command = g_strdup_printf(
                "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
                "net-install-keyboard-present=on,"
                "net-install-shift-held=on "
                "-drive if=none,id=pieeprom,format=raw,file=%s -nic none",
                case_path);
            qts = qtest_init(case_command);
            case_state = qom_get_string(qts, "boot-state");
            g_assert_cmpstr(case_state, ==, cases[i].state);
            qtest_quit(qts);
            unlink(case_path);
        }
    }
}

static void test_network_install_disable_hdmi(void)
{
    static const char config[] =
        "BOOT_ORDER=0xe\n"
        "DISABLE_HDMI=1\n"
        "NET_INSTALL_ENABLED=1\n"
        "NET_INSTALL_AT_POWER_ON=1\n"
        "NET_INSTALL_KEYBOARD_WAIT=1000\n";
    static const char * const machines[] = {
        "raspi4b",
        "raspi-cm4",
    };

    for (size_t i = 0; i < ARRAY_SIZE(machines); i++) {
        g_autofree uint8_t *eeprom = make_eeprom_image(config, false);
        g_autofree char *eeprom_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *state = NULL;
        g_autofree char *migration_dir = NULL;
        g_autofree char *migration_socket = NULL;
        QTestState *source;
        QTestState *destination;

        write_temp_image("raspi4-disable-hdmi-XXXXXX", eeprom,
                         EEPROM_SIZE, &eeprom_path);
        command = g_strdup_printf(
            "-M %s,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "net-install-requested=on,"
            "net-install-keyboard-present=on,"
            "net-install-shift-held=on "
            "-drive if=none,id=pieeprom,format=raw,file=%s,"
            "file.locking=off -nic none",
            machines[i], eeprom_path);
        source = qtest_init(command);

        state = qom_get_string(source, "boot-state");
        g_assert_cmpstr(state, ==, "stopped");
        g_assert_true(qom_get_bool(source, "boot-disable-hdmi"));
        g_assert_false(qom_get_bool(source, "boot-net-install-enabled"));
        g_assert_false(qom_get_bool(
                           source, "boot-net-install-at-power-on"));
        g_assert_false(qom_get_bool(
                           source, "net-install-keyboard-waiting"));
        g_assert_cmpuint(qom_get_uint64(
                             source, "boot-attempt-count"), ==, 0);

        qtest_system_reset(source);
        g_clear_pointer(&state, g_free);
        state = qom_get_string(source, "boot-state");
        g_assert_cmpstr(state, ==, "stopped");
        g_assert_true(qom_get_bool(source, "boot-disable-hdmi"));
        g_assert_false(qom_get_bool(source, "boot-net-install-enabled"));

        destination = migrate_to_new_qtest(
            source, command, &migration_dir, &migration_socket);
        g_assert_true(qom_get_bool(destination, "boot-disable-hdmi"));
        g_assert_false(qom_get_bool(
                           destination, "boot-net-install-enabled"));
        g_assert_false(qom_get_bool(
                           destination, "boot-net-install-at-power-on"));

        qtest_quit(source);
        qtest_quit(destination);
        unlink(eeprom_path);
        unlink(migration_socket);
        rmdir(migration_dir);
    }

    {
        g_autofree uint8_t *eeprom = make_eeprom_image(
            "BOOT_ORDER=0x7\n"
            "DISABLE_HDMI=1\n"
            "HTTP_HOST=boot.example\n", false);
        g_autofree char *eeprom_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *state = NULL;
        g_autofree char *source = NULL;
        QTestState *qts;

        write_temp_image("raspi4-disable-hdmi-http-XXXXXX", eeprom,
                         EEPROM_SIZE, &eeprom_path);
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
            "-drive if=none,id=pieeprom,format=raw,file=%s -nic none",
            eeprom_path);
        qts = qtest_init(command);
        state = qom_get_string(qts, "boot-state");
        source = qom_get_string(qts, "boot-source");
        g_assert_cmpstr(state, ==, "fatal-error-reboot-wait");
        g_assert_cmpstr(source, ==, "none");
        g_assert_cmpuint(qom_get_uint64(
                             qts, "boot-attempt-count"), ==, 1);
        g_assert_true(qom_get_bool(qts, "boot-disable-hdmi"));
        g_assert_false(qom_get_bool(qts, "boot-net-install-enabled"));
        qtest_quit(qts);
        unlink(eeprom_path);
    }

    {
        static const struct {
            const char *value;
            const char *state;
            bool disabled;
        } cases[] = {
            { "0", "network-dhcp-wait", false },
            { "2", "network-dhcp-wait", false },
            { "4294967296", "eeprom-invalid", false },
            { "-1", "eeprom-invalid", false },
        };

        for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
            g_autofree char *case_config = g_strdup_printf(
                "BOOT_ORDER=0xe\n"
                "DISABLE_HDMI=%s\n"
                "NET_INSTALL_ENABLED=1\n",
                cases[i].value);
            g_autofree uint8_t *eeprom =
                make_eeprom_image(case_config, false);
            g_autofree char *eeprom_path = NULL;
            g_autofree char *command = NULL;
            g_autofree char *state = NULL;
            QTestState *qts;

            write_temp_image("raspi4-disable-hdmi-value-XXXXXX", eeprom,
                             EEPROM_SIZE, &eeprom_path);
            command = g_strdup_printf(
                "-M raspi4b,boot-mode=behavioral,"
                "eeprom-drive=pieeprom,"
                "net-install-keyboard-present=on,"
                "net-install-shift-held=on "
                "-drive if=none,id=pieeprom,format=raw,file=%s -nic none",
                eeprom_path);
            qts = qtest_init(command);
            state = qom_get_string(qts, "boot-state");
            g_assert_cmpstr(state, ==, cases[i].state);
            g_assert_cmpint(
                qom_get_bool(qts, "boot-disable-hdmi"), ==,
                cases[i].disabled);
            qtest_quit(qts);
            unlink(eeprom_path);
        }
    }
}

static void test_hdmi_diagnostics_delay(void)
{
    static const char * const machines[] = {
        "raspi4b",
        "raspi-cm4",
    };
    const uint64_t second = INT64_C(1000) * 1000 * 1000;

    for (size_t i = 0; i < ARRAY_SIZE(machines); i++) {
        g_autofree uint8_t *eeprom = make_eeprom_image(
            "BOOT_ORDER=0xe\nREBOOT_ON_FATAL_ERROR=0\n", false);
        g_autofree char *eeprom_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *state = NULL;
        g_autofree char *migration_dir = NULL;
        g_autofree char *migration_socket = NULL;
        QTestState *source;
        QTestState *destination;

        write_temp_image("raspi4-hdmi-delay-eeprom-XXXXXX", eeprom,
                         EEPROM_SIZE, &eeprom_path);
        command = g_strdup_printf(
            "-M %s,boot-mode=behavioral,eeprom-drive=pieeprom "
            "-drive if=none,id=pieeprom,format=raw,file=%s,"
            "file.locking=off -nic none",
            machines[i], eeprom_path);
        source = qtest_init(command);

        state = qom_get_string(source, "boot-state");
        g_assert_cmpstr(state, ==, "stopped");
        g_assert_cmpuint(qom_get_uint32(
                             source, "boot-hdmi-delay"), ==, 5);
        g_assert_true(qom_get_bool(
                          source, "boot-hdmi-diagnostics-pending"));
        g_assert_false(qom_get_bool(
                           source, "boot-hdmi-diagnostics-visible"));
        g_assert_cmpuint(qom_get_uint64(
                             source,
                             "boot-hdmi-diagnostics-remaining-ns"),
                         ==, 5 * second);

        qtest_clock_step(source, 2 * second);
        destination = migrate_to_new_qtest(
            source, command, &migration_dir, &migration_socket);
        g_assert_true(qom_get_bool(
                          destination, "boot-hdmi-diagnostics-pending"));
        g_assert_false(qom_get_bool(
                           destination, "boot-hdmi-diagnostics-visible"));
        g_assert_cmpuint(qom_get_uint64(
                             destination,
                             "boot-hdmi-diagnostics-remaining-ns"),
                         ==, 3 * second);

        qtest_clock_step(destination, 3 * second - 1);
        g_assert_false(qom_get_bool(
                           destination, "boot-hdmi-diagnostics-visible"));
        qtest_clock_step(destination, 1);
        g_assert_false(qom_get_bool(
                           destination, "boot-hdmi-diagnostics-pending"));
        g_assert_true(qom_get_bool(
                          destination, "boot-hdmi-diagnostics-visible"));

        qtest_system_reset(destination);
        g_assert_true(qom_get_bool(
                          destination, "boot-hdmi-diagnostics-pending"));
        g_assert_false(qom_get_bool(
                           destination, "boot-hdmi-diagnostics-visible"));
        g_assert_cmpuint(qom_get_uint64(
                             destination,
                             "boot-hdmi-diagnostics-remaining-ns"),
                         ==, 5 * second);

        qtest_quit(source);
        qtest_quit(destination);
        unlink(migration_socket);
        rmdir(migration_dir);
        unlink(eeprom_path);
    }

    {
        static const struct {
            const char *config;
            const char *state;
            uint32_t delay;
            bool visible;
        } cases[] = {
            {
                "BOOT_ORDER=0xe\nREBOOT_ON_FATAL_ERROR=0\n"
                "HDMI_DELAY=0\n",
                "stopped", 0, true,
            },
            {
                "BOOT_ORDER=0xe\nREBOOT_ON_FATAL_ERROR=0\n"
                "HDMI_DELAY=0\nDISABLE_HDMI=1\n",
                "stopped", 0, false,
            },
            {
                "BOOT_ORDER=0xe\nHDMI_DELAY=4294967296\n",
                "eeprom-invalid", 5, true,
            },
            {
                "BOOT_ORDER=0xe\nHDMI_DELAY=-1\n",
                "eeprom-invalid", 5, true,
            },
            {
                "BOOT_ORDER=0x8\nHDMI_DELAY=100\n",
                "fatal-error-reboot-wait", 100, true,
            },
        };

        for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
            g_autofree uint8_t *eeprom =
                make_eeprom_image(cases[i].config, false);
            g_autofree char *eeprom_path = NULL;
            g_autofree char *command = NULL;
            g_autofree char *state = NULL;
            QTestState *qts;

            write_temp_image("raspi4-hdmi-delay-case-XXXXXX", eeprom,
                             EEPROM_SIZE, &eeprom_path);
            command = g_strdup_printf(
                "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
                "-drive if=none,id=pieeprom,format=raw,file=%s -nic none",
                eeprom_path);
            qts = qtest_init(command);
            state = qom_get_string(qts, "boot-state");
            g_assert_cmpstr(state, ==, cases[i].state);
            g_assert_cmpuint(qom_get_uint32(
                                 qts, "boot-hdmi-delay"), ==,
                             cases[i].delay);
            g_assert_cmpint(qom_get_bool(
                                qts,
                                "boot-hdmi-diagnostics-visible"), ==,
                            cases[i].visible);
            g_assert_false(qom_get_bool(
                               qts,
                               "boot-hdmi-diagnostics-pending"));
            qtest_quit(qts);
            unlink(eeprom_path);
        }
    }

    {
        static const uint8_t firmware[] =
            "normal boot before HDMI diagnostics delay";
        g_autofree char *eeprom_path = NULL;
        g_autofree char *sd_path = NULL;
        QTestState *qts = start_with_eeprom_and_sd(
            "BOOT_ORDER=0x1\n", "START4  ELF",
            firmware, sizeof(firmware), NULL,
            &eeprom_path, &sd_path);

        g_assert_false(qom_get_bool(
                           qts, "boot-hdmi-diagnostics-pending"));
        g_assert_false(qom_get_bool(
                           qts, "boot-hdmi-diagnostics-visible"));
        g_assert_cmpuint(qom_get_uint64(
                             qts,
                             "boot-hdmi-diagnostics-remaining-ns"), ==, 0);
        qtest_clock_step(qts, 6 * second);
        g_assert_false(qom_get_bool(
                           qts, "boot-hdmi-diagnostics-visible"));
        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(sd_path);
    }
}

static void test_network_install_usb_no_boot_files(void)
{
    static const uint8_t firmware[] =
        "QEMU Network Install USB fallback fixture";
    static const char config[] =
        "BOOT_ORDER=0xe4\n"
        "NET_INSTALL_ENABLED=1\n"
        "NET_INSTALL_KEYBOARD_WAIT=0\n"
        "USB_MSD_LUN_TIMEOUT=100\n";
    g_autofree uint8_t *eeprom = make_eeprom_image(config, false);
    g_autofree uint8_t *usb = make_usb_boot_image(
        "MISSING ELF", firmware, sizeof(firmware), NULL);
    g_autofree uint8_t *network = make_usb_boot_image(
        "START4  ELF", firmware, sizeof(firmware), TEST_NETWORK_CONFIG);
    g_autofree char *expected_hash = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, firmware, sizeof(firmware));
    g_autofree char *eeprom_path = NULL;
    g_autofree char *usb_path = NULL;
    g_autofree char *network_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *state = NULL;
    g_autofree char *source = NULL;
    g_autofree char *actual_hash = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    QTestState *qts;
    QTestState *destination;

    write_temp_image("raspi4-net-install-usb-eeprom-XXXXXX", eeprom,
                     EEPROM_SIZE, &eeprom_path);
    write_temp_image("raspi4-net-install-usb-empty-XXXXXX", usb,
                     SD_SIZE, &usb_path);
    write_temp_image("raspi4-net-install-network-XXXXXX", network,
                     SD_SIZE, &network_path);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "usb-boot-drive=usbboot,network-boot-drive=netboot,"
        "network-boot-wire=off,net-install-keyboard-present=on,"
        "net-install-shift-held=on "
        "-drive if=none,id=pieeprom,format=raw,file=%s,"
        "file.locking=off "
        "-drive if=none,id=usbboot,format=raw,file=%s,"
        "file.locking=off "
        "-drive if=none,id=netboot,format=raw,file=%s,"
        "file.locking=off",
        eeprom_path, usb_path, network_path);
    qts = qtest_init(command);

    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "usb-lun-wait");
    g_assert_true(qom_get_bool(qts, "usb-boot-files-missing"));
    g_assert_false(qom_get_bool(
                       qts, "net-install-usb-fallback-consumed"));
    qtest_clock_step(qts, INT64_C(40) * 1000 * 1000);
    destination = migrate_to_new_qtest(
        qts, command, &migration_dir, &migration_socket);
    qtest_quit(qts);
    qts = destination;
    g_assert_true(qom_get_bool(qts, "usb-boot-files-missing"));
    g_assert_false(qom_get_bool(
                       qts, "net-install-usb-fallback-consumed"));
    qtest_clock_step(qts, INT64_C(59) * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "usb-lun-wait");
    qtest_clock_step(qts, INT64_C(1) * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    g_clear_pointer(&source, g_free);
    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "boot-source");
    actual_hash = qom_get_string(qts, "firmware-sha256");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(source, ==, "network");
    g_assert_cmpstr(actual_hash, ==, expected_hash);
    g_assert_true(qom_get_bool(
                      qts, "net-install-usb-fallback-consumed"));

    qtest_system_reset(qts);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "usb-lun-wait");
    g_assert_false(qom_get_bool(
                       qts, "net-install-usb-fallback-consumed"));
    qtest_clock_step(qts, INT64_C(100) * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_true(qom_get_bool(
                      qts, "net-install-usb-fallback-consumed"));

    qtest_quit(qts);
    unlink(migration_socket);
    rmdir(migration_dir);
    unlink(eeprom_path);
    unlink(usb_path);
    unlink(network_path);

    {
        static const struct {
            const char *config;
            const char *controller;
            const char *inputs;
        } cases[] = {
            {
                "BOOT_ORDER=0xe4\n"
                "NET_INSTALL_ENABLED=0\n"
                "NET_INSTALL_KEYBOARD_WAIT=0\n"
                "USB_MSD_LUN_TIMEOUT=100\n",
                "xhci", "net-install-keyboard-present=on,"
                        "net-install-shift-held=on",
            },
            {
                "BOOT_ORDER=0xe4\n"
                "DISABLE_HDMI=1\n"
                "NET_INSTALL_ENABLED=1\n"
                "NET_INSTALL_KEYBOARD_WAIT=0\n"
                "USB_MSD_LUN_TIMEOUT=100\n",
                "xhci", "net-install-keyboard-present=on,"
                        "net-install-shift-held=on",
            },
            {
                "BOOT_ORDER=0xe5\n"
                "NET_INSTALL_ENABLED=1\n"
                "NET_INSTALL_KEYBOARD_WAIT=0\n"
                "USB_MSD_LUN_TIMEOUT=100\n",
                "dwc2", "net-install-keyboard-present=on,"
                        "net-install-shift-held=on",
            },
            {
                "BOOT_ORDER=0xe4\n"
                "NET_INSTALL_ENABLED=1\n"
                "NET_INSTALL_KEYBOARD_WAIT=0\n"
                "USB_MSD_LUN_TIMEOUT=100\n",
                "xhci", "net-install-keyboard-present=on",
            },
        };

        for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
            g_autofree uint8_t *case_eeprom =
                make_eeprom_image(cases[i].config, false);
            g_autofree char *case_eeprom_path = NULL;
            g_autofree char *case_usb_path = NULL;
            g_autofree char *case_network_path = NULL;
            g_autofree char *case_command = NULL;
            g_autofree char *case_state = NULL;

            write_temp_image("raspi4-net-install-case-eeprom-XXXXXX",
                             case_eeprom, EEPROM_SIZE, &case_eeprom_path);
            write_temp_image("raspi4-net-install-case-usb-XXXXXX",
                             usb, SD_SIZE, &case_usb_path);
            write_temp_image("raspi4-net-install-case-network-XXXXXX",
                             network, SD_SIZE, &case_network_path);
            case_command = g_strdup_printf(
                "-M raspi4b,boot-mode=behavioral,"
                "eeprom-drive=pieeprom,usb-boot-drive=usbboot,"
                "usb-boot-controller=%s,"
                "network-boot-drive=netboot,network-boot-wire=off,%s "
                "-drive if=none,id=pieeprom,format=raw,file=%s "
                "-drive if=none,id=usbboot,format=raw,file=%s "
                "-drive if=none,id=netboot,format=raw,file=%s",
                cases[i].controller, cases[i].inputs, case_eeprom_path,
                case_usb_path, case_network_path);
            qts = qtest_init(case_command);
            case_state = qom_get_string(qts, "boot-state");
            g_assert_cmpstr(case_state, ==, "usb-lun-wait");
            qtest_clock_step(qts, INT64_C(100) * 1000 * 1000);
            g_clear_pointer(&case_state, g_free);
            case_state = qom_get_string(qts, "boot-state");
            g_assert_cmpstr(case_state, ==, "stopped");
            g_assert_false(qom_get_bool(
                               qts,
                               "net-install-usb-fallback-consumed"));
            qtest_quit(qts);
            unlink(case_eeprom_path);
            unlink(case_usb_path);
            unlink(case_network_path);
        }
    }

    {
        g_autofree uint8_t *case_eeprom = make_eeprom_image(
            "BOOT_ORDER=0xe4\n"
            "NET_INSTALL_ENABLED=1\n"
            "NET_INSTALL_KEYBOARD_WAIT=0\n"
            "USB_MSD_DISCOVER_TIMEOUT=5000\n", false);
        g_autofree char *case_eeprom_path = NULL;
        g_autofree char *case_network_path = NULL;
        g_autofree char *case_command = NULL;
        g_autofree char *case_state = NULL;

        write_temp_image("raspi4-net-install-absent-eeprom-XXXXXX",
                         case_eeprom, EEPROM_SIZE, &case_eeprom_path);
        write_temp_image("raspi4-net-install-absent-network-XXXXXX",
                         network, SD_SIZE, &case_network_path);
        case_command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "network-boot-drive=netboot,network-boot-wire=off,"
            "net-install-keyboard-present=on,"
            "net-install-shift-held=on "
            "-drive if=none,id=pieeprom,format=raw,file=%s "
            "-drive if=none,id=netboot,format=raw,file=%s",
            case_eeprom_path, case_network_path);
        qts = qtest_init(case_command);
        case_state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(case_state, ==, "usb-discovery-wait");
        qtest_clock_step(qts, INT64_C(5000) * 1000 * 1000);
        g_clear_pointer(&case_state, g_free);
        case_state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(case_state, ==, "stopped");
        g_assert_false(qom_get_bool(qts, "usb-boot-files-missing"));
        g_assert_false(qom_get_bool(
                           qts, "net-install-usb-fallback-consumed"));
        qtest_quit(qts);
        unlink(case_eeprom_path);
        unlink(case_network_path);
    }
}

static void test_boot_order_fallback_to_sd(void)
{
    static const uint8_t firmware[] = "bootable after source fallback";
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    QTestState *qts = start_with_eeprom_and_sd(
        "BOOT_ORDER=0x124\n", "START4  ELF", firmware, sizeof(firmware),
        NULL, &eeprom_path, &sd_path);
    g_autofree char *state = qom_get_string(qts, "boot-state");
    g_autofree char *source = qom_get_string(qts, "boot-source");

    g_assert_true(qom_get_bool(qts, "network-boot-wire"));
    g_assert_cmpstr(state, ==, "usb-discovery-wait");
    g_assert_cmpstr(source, ==, "usb-msd");
    g_assert_cmpuint(qom_get_uint64(qts, "boot-elapsed-ms"), ==, 0);
    qtest_clock_step(qts, INT64_C(19999) * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    g_clear_pointer(&source, g_free);
    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "boot-source");
    g_assert_cmpstr(state, ==, "usb-discovery-wait");
    g_assert_cmpstr(source, ==, "usb-msd");
    qtest_clock_step(qts, INT64_C(1) * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    g_clear_pointer(&source, g_free);
    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "boot-source");
    g_assert_cmpuint(qom_get_uint64(qts, "boot-elapsed-ms"), ==, 20000);
    g_assert_cmpstr(state, ==, "network-dhcp-wait");
    g_assert_cmpstr(source, ==, "network");
    qtest_clock_step(qts, INT64_C(45) * 1000 * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    g_clear_pointer(&source, g_free);
    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "boot-source");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(source, ==, "sd-card");
    g_assert_cmpuint(qom_get_uint64(qts, "boot-attempt-count"), ==, 3);
    g_assert_cmpuint(qom_get_uint64(qts, "boot-elapsed-ms"), ==, 65000);
    g_assert_cmpuint(qom_get_uint32(qts, "boot-order-index"), ==, 2);
    qtest_quit(qts);
    unlink(eeprom_path);
    unlink(sd_path);
}















static void test_network_boot(void)
{
    static const uint8_t firmware[] =
        "byte-identical TFTP boot firmware";
    g_autofree char *expected_hash = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, firmware, sizeof(firmware));
    g_autofree char *eeprom_path = NULL;
    g_autofree char *network_path = NULL;
    g_autofree char *state = NULL;
    g_autofree char *source = NULL;
    g_autofree char *actual_hash = NULL;
    g_autofree char *network_drive = NULL;
    g_autofree char *tftp_prefix = NULL;
    QTestState *qts = start_with_eeprom_and_network(
        "BOOT_ORDER=0x2\nNET_BOOT_MAX_RETRIES=3\n"
        "DHCP_TIMEOUT=7000\nDHCP_REQ_TIMEOUT=600\n"
        "TFTP_FILE_TIMEOUT=9000\n"
        "TFTP_PREFIX=1\nTFTP_PREFIX_STR=fleet/pi4/\n",
        "START4  ELF", firmware, sizeof(firmware),
        &eeprom_path, &network_path);

    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "boot-source");
    actual_hash = qom_get_string(qts, "firmware-sha256");
    network_drive = qom_get_string(qts, "network-boot-drive");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(source, ==, "network");
    g_assert_cmpstr(actual_hash, ==, expected_hash);
    g_assert_cmpstr(network_drive, ==, "netboot");
    g_assert_cmpint(qom_get_int32(qts, "boot-net-max-retries"), ==, 3);
    g_assert_cmpuint(qom_get_uint32(qts, "boot-dhcp-timeout-ms"), ==,
                     7000);
    g_assert_cmpuint(qom_get_uint32(qts, "boot-dhcp-req-timeout-ms"), ==,
                     600);
    g_assert_cmpuint(qom_get_uint32(qts, "boot-tftp-file-timeout-ms"), ==,
                     9000);
    g_assert_cmpuint(qom_get_uint32(qts, "boot-tftp-prefix-mode"), ==, 1);
    tftp_prefix = qom_get_string(qts, "boot-tftp-prefix");
    g_assert_cmpstr(tftp_prefix, ==, "fleet/pi4/");
    g_assert_cmpuint(qom_get_uint64(qts, "boot-attempt-count"), ==, 1);
    g_assert_cmpuint(qom_get_uint32(qts, "boot-order-index"), ==, 0);
    assert_bootloader_boot_mode(qts, 2);
    qtest_quit(qts);
    unlink(eeprom_path);
    unlink(network_path);
}

static void test_tftp_prefix_modes(void)
{
    const uint32_t serial = 0x9ffefdef;
    g_autofree uint8_t *eeprom = make_eeprom_image(
        "BOOT_ORDER=0xe2\n", false);
    g_autofree uint8_t *otp = make_otp_image(
        0, 0, PI4_BOARD_REVISION, false);
    g_autofree char *eeprom_path = NULL;
    g_autofree char *otp_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *prefix = NULL;
    QTestState *qts;

    stl_le_p(otp + (28 - 1) * 4, serial);
    stl_le_p(otp + (29 - 1) * 4, ~serial);
    write_temp_image("raspi4-eeprom-prefix-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    write_temp_image("raspi4-otp-prefix-XXXXXX", otp, OTP_SIZE, &otp_path);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "otp-drive=piotp "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=none,id=piotp,format=raw,file=%s,file.locking=off "
        "-nic none",
        eeprom_path, otp_path);
    qts = qtest_init(command);
    prefix = qom_get_string(qts, "boot-tftp-prefix");
    g_assert_cmpuint(qom_get_uint32(qts, "boot-tftp-prefix-mode"), ==, 0);
    g_assert_cmpstr(prefix, ==, "9ffefdef/");
    qtest_system_reset(qts);
    g_clear_pointer(&prefix, g_free);
    prefix = qom_get_string(qts, "boot-tftp-prefix");
    g_assert_cmpstr(prefix, ==, "9ffefdef/");
    g_assert_false(qom_get_bool(qts, "boot-tftp-prefix-fallback"));
    qtest_quit(qts);

    unlink(eeprom_path);
    g_clear_pointer(&eeprom, g_free);
    g_clear_pointer(&eeprom_path, g_free);
    g_clear_pointer(&command, g_free);
    g_clear_pointer(&prefix, g_free);
    eeprom = make_eeprom_image(
        "BOOT_ORDER=0xe2\nTFTP_PREFIX=2\n", false);
    write_temp_image("raspi4-eeprom-prefix-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-global bcm2711-genet.mac=52:54:00:12:34:56",
        eeprom_path);
    qts = qtest_init(command);
    prefix = qom_get_string(qts, "boot-tftp-prefix");
    g_assert_cmpuint(qom_get_uint32(qts, "boot-tftp-prefix-mode"), ==, 2);
    g_assert_cmpstr(prefix, ==, "52-54-00-12-34-56/");
    qtest_quit(qts);

    unlink(eeprom_path);
    unlink(otp_path);
}

static void test_mac_address_policy(void)
{
    g_autofree uint8_t *eeprom = make_eeprom_image(
        "BOOT_ORDER=0xe2\nMAC_ADDRESS=dc:a6:32:01:36:c2\n"
        "TFTP_PREFIX=2\n", false);
    g_autofree uint8_t *otp = make_otp_image(
        0, 0, PI4_BOARD_REVISION, false);
    g_autofree char *eeprom_path = NULL;
    g_autofree char *otp_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *mac = NULL;
    g_autofree char *source = NULL;
    g_autofree char *prefix = NULL;
    QTestState *qts;

    stl_le_p(otp + (36 - 1) * 4, 0x247e0000);
    stl_le_p(otp + (37 - 1) * 4, 0xe45f0120);
    write_temp_image("raspi4-eeprom-mac-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    write_temp_image("raspi4-otp-mac-XXXXXX", otp, OTP_SIZE, &otp_path);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "otp-drive=piotp "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=none,id=piotp,format=raw,file=%s,file.locking=off "
        "-global bcm2711-genet.mac=52:54:00:12:34:56 -nic none",
        eeprom_path, otp_path);
    qts = qtest_init(command);
    mac = qom_get_string(qts, "boot-mac-address");
    source = qom_get_string(qts, "boot-mac-address-source");
    prefix = qom_get_string(qts, "boot-tftp-prefix");
    g_assert_cmpstr(mac, ==, "dc:a6:32:01:36:c2");
    g_assert_cmpstr(source, ==, "mac-address");
    g_assert_cmpstr(prefix, ==, "dc-a6-32-01-36-c2/");
    qtest_system_reset(qts);
    g_clear_pointer(&mac, g_free);
    mac = qom_get_string(qts, "boot-mac-address");
    g_assert_cmpstr(mac, ==, "dc:a6:32:01:36:c2");
    qtest_quit(qts);

    unlink(eeprom_path);
    g_clear_pointer(&eeprom, g_free);
    g_clear_pointer(&eeprom_path, g_free);
    g_clear_pointer(&command, g_free);
    g_clear_pointer(&mac, g_free);
    g_clear_pointer(&source, g_free);
    g_clear_pointer(&prefix, g_free);
    eeprom = make_eeprom_image(
        "BOOT_ORDER=0xe2\nMAC_ADDRESS=dc:a6:32:01:36:c2\n"
        "MAC_ADDRESS_OTP=0,1\nTFTP_PREFIX=2\n", false);
    write_temp_image("raspi4-eeprom-mac-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "otp-drive=piotp "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=none,id=piotp,format=raw,file=%s,file.locking=off "
        "-global bcm2711-genet.mac=52:54:00:12:34:56 -nic none",
        eeprom_path, otp_path);
    qts = qtest_init(command);
    mac = qom_get_string(qts, "boot-mac-address");
    source = qom_get_string(qts, "boot-mac-address-source");
    prefix = qom_get_string(qts, "boot-tftp-prefix");
    g_assert_cmpstr(mac, ==, "e4:5f:01:20:24:7e");
    g_assert_cmpstr(source, ==, "customer-otp");
    g_assert_cmpstr(prefix, ==, "e4-5f-01-20-24-7e/");
    qtest_quit(qts);

    unlink(eeprom_path);
    unlink(otp_path);
}





static void test_network_boot_timeout_fallback(void)
{
    static const uint8_t firmware[] = "SD firmware after DHCP retries";
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree char *state = NULL;
    g_autofree char *source = NULL;
    QTestState *qts = start_with_eeprom_and_sd(
        "BOOT_ORDER=0x12\nNET_BOOT_MAX_RETRIES=1\nDHCP_TIMEOUT=5000\n",
        "START4  ELF", firmware, sizeof(firmware), NULL,
        &eeprom_path, &sd_path);

    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "boot-source");
    g_assert_cmpstr(state, ==, "network-dhcp-wait");
    g_assert_cmpstr(source, ==, "network");
    g_assert_cmpuint(qom_get_uint64(qts, "boot-attempt-count"), ==, 1);

    qtest_clock_step(qts, INT64_C(5000) * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "network-dhcp-wait");
    g_assert_cmpuint(qom_get_uint64(qts, "boot-retry-count"), ==, 1);
    g_assert_cmpuint(qom_get_uint64(qts, "boot-attempt-count"), ==, 2);

    qtest_clock_step(qts, INT64_C(5000) * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    g_clear_pointer(&source, g_free);
    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "boot-source");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(source, ==, "sd-card");
    g_assert_cmpuint(qom_get_uint64(qts, "boot-attempt-count"), ==, 3);
    g_assert_cmpuint(qom_get_uint64(qts, "boot-elapsed-ms"), ==, 10000);
    g_assert_cmpuint(qom_get_uint32(qts, "boot-order-index"), ==, 1);
    qtest_quit(qts);
    unlink(eeprom_path);
    unlink(sd_path);
}

static void test_network_tftp_timeout_fallback(void)
{
    static const uint8_t firmware[] = "SD firmware after TFTP failure";
    g_autofree uint8_t *eeprom = make_eeprom_image(
        "BOOT_ORDER=0x12\nTFTP_FILE_TIMEOUT=5000\n", false);
    g_autofree uint8_t *network = make_usb_boot_image(
        "START4  ELF", NULL, 0, NULL);
    g_autofree uint8_t *sd = make_usb_boot_image(
        "START4  ELF", firmware, sizeof(firmware), NULL);
    g_autofree char *eeprom_path = NULL;
    g_autofree char *network_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *state = NULL;
    g_autofree char *source = NULL;
    QTestState *qts;

    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    write_temp_image("raspi4-network-XXXXXX", network, SD_SIZE,
                     &network_path);
    write_temp_image("raspi4-sd-XXXXXX", sd, SD_SIZE, &sd_path);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "network-boot-drive=netboot,network-boot-wire=off "
        "-drive if=none,id=pieeprom,format=raw,file=%s "
        "-drive if=none,id=netboot,format=raw,file=%s "
        "-drive if=sd,format=raw,file=%s",
        eeprom_path, network_path, sd_path);
    qts = qtest_init(command);
    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "boot-source");
    g_assert_cmpstr(state, ==, "network-tftp-wait");
    g_assert_cmpstr(source, ==, "network");

    qtest_clock_step(qts, INT64_C(5000) * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    g_clear_pointer(&source, g_free);
    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "boot-source");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(source, ==, "sd-card");
    g_assert_cmpuint(qom_get_uint64(qts, "boot-attempt-count"), ==, 2);
    g_assert_cmpuint(qom_get_uint64(qts, "boot-elapsed-ms"), ==, 5000);
    qtest_quit(qts);
    unlink(eeprom_path);
    unlink(network_path);
    unlink(sd_path);
}

static void test_network_boot_hotplug(void)
{
    static const uint8_t firmware[] = "TFTP corpus inserted during DHCP";
    g_autofree char *eeprom_path = NULL;
    g_autofree char *network_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *state = NULL;
    QTestState *qts = start_with_eeprom_and_network(
        "BOOT_ORDER=0xe2\n", "START4  ELF", firmware, sizeof(firmware),
        &eeprom_path, &network_path);

    qtest_quit(qts);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "network-boot-drive=netboot,network-boot-wire=off "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=none,id=netboot",
        eeprom_path);
    qts = qtest_init(command);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "network-dhcp-wait");
    qtest_qmp_assert_success(
        qts, "{ 'execute': 'blockdev-change-medium', 'arguments': {"
             "'device': 'netboot', 'filename': %s, 'format': 'raw' } }",
        network_path);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpuint(qom_get_uint64(qts, "boot-attempt-count"), ==, 1);
    g_assert_cmpuint(qom_get_uint64(qts, "boot-elapsed-ms"), ==, 0);
    qtest_quit(qts);
    unlink(eeprom_path);
    unlink(network_path);
}




































































static void test_boot_health_migration(void)
{
    static const uint8_t firmware[] = "health observation fixture";
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_path = NULL;
    g_autofree char *health = NULL;
    QTestState *destination;
    QTestState *qts = start_with_eeprom_and_sd(
        "BOOT_ORDER=0x1\n", "START4  ELF", firmware, sizeof(firmware),
        NULL, &eeprom_path, &sd_path);

    qtest_quit(qts);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=sd,format=raw,file=%s,file.locking=off",
        eeprom_path, sd_path);
    qts = qtest_init(command);
    health = qom_get_string(qts, "boot-health");
    g_assert_cmpstr(health, ==, "none");

    QDict *response = qtest_qmp_assert_failure_ref(
        qts, "{ 'execute': 'qom-set', 'arguments': {"
             "'path': '/machine', 'property': 'boot-health', "
             "'value': 'userspace-ready' } }");
    g_assert_nonnull(strstr(qdict_get_str(response, "desc"),
                            "invalid boot-health transition"));
    qobject_unref(response);
    qtest_qmp_assert_success(
        qts, "{ 'execute': 'qom-set', 'arguments': {"
             "'path': '/machine', 'property': 'boot-health', "
             "'value': 'kernel-started' } }");
    qtest_qmp_assert_success(
        qts, "{ 'execute': 'qom-set', 'arguments': {"
             "'path': '/machine', 'property': 'boot-health', "
             "'value': 'userspace-ready' } }");

    destination = migrate_to_new_qtest(qts, command, &migration_dir,
                                       &migration_path);
    g_clear_pointer(&health, g_free);
    health = qom_get_string(destination, "boot-health");
    g_assert_cmpstr(health, ==, "userspace-ready");

    qtest_system_reset(destination);
    g_clear_pointer(&health, g_free);
    health = qom_get_string(destination, "boot-health");
    g_assert_cmpstr(health, ==, "none");

    qtest_quit(qts);
    qtest_quit(destination);
    unlink(eeprom_path);
    unlink(sd_path);
    unlink(migration_path);
    rmdir(migration_dir);
}




static void test_network_timeout_migration(void)
{
    g_autofree uint8_t *eeprom = make_eeprom_image(
        "BOOT_ORDER=0xe2\nDHCP_TIMEOUT=7000\n", false);
    g_autofree char *eeprom_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_path = NULL;
    g_autofree char *state = NULL;
    g_autofree char *source = NULL;
    QTestState *destination;
    QTestState *qts;

    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-nic none",
        eeprom_path);
    qts = qtest_init(command);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "network-dhcp-wait");
    qtest_clock_step(qts, INT64_C(3000) * 1000 * 1000);

    destination = migrate_to_new_qtest(qts, command, &migration_dir,
                                       &migration_path);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    source = qom_get_string(destination, "boot-source");
    g_assert_cmpstr(state, ==, "network-dhcp-wait");
    g_assert_cmpstr(source, ==, "network");
    g_assert_cmpuint(qom_get_uint64(destination, "boot-elapsed-ms"), ==, 0);

    qtest_clock_step(destination, INT64_C(3999) * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "network-dhcp-wait");
    qtest_clock_step(destination, INT64_C(1) * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    g_clear_pointer(&source, g_free);
    state = qom_get_string(destination, "boot-state");
    source = qom_get_string(destination, "boot-source");
    g_assert_cmpstr(state, ==, "stopped");
    g_assert_cmpstr(source, ==, "stop");
    g_assert_cmpuint(qom_get_uint64(destination, "boot-elapsed-ms"), ==,
                     7000);
    g_assert_cmpuint(qom_get_uint32(destination, "boot-order-index"), ==, 1);

    qtest_quit(qts);
    qtest_quit(destination);
    unlink(eeprom_path);
    unlink(migration_path);
    rmdir(migration_dir);
}

static void test_restart_timer_migration(void)
{
    g_autofree uint8_t *eeprom = make_eeprom_image(
        "BOOT_ORDER=0xef\nMAX_RESTARTS=2\n", false);
    g_autofree char *eeprom_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_path = NULL;
    g_autofree char *state = NULL;
    QTestState *destination;
    QTestState *qts;

    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off",
        eeprom_path);
    qts = qtest_init(command);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "restart-cycle-wait");
    g_assert_cmpuint(qom_get_uint64(qts, "boot-restart-count"), ==, 1);
    qtest_clock_step(qts, 400 * 1000);

    destination = migrate_to_new_qtest(qts, command, &migration_dir,
                                       &migration_path);
    qtest_clock_step(destination, 599999);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "restart-cycle-wait");
    g_assert_cmpuint(qom_get_uint64(destination, "boot-restart-count"), ==,
                     1);
    qtest_clock_step(destination, 1);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "restart-cycle-wait");
    g_assert_cmpuint(qom_get_uint64(destination, "boot-restart-count"), ==,
                     2);

    qtest_quit(qts);
    qtest_quit(destination);
    unlink(eeprom_path);
    unlink(migration_path);
    rmdir(migration_dir);
}

static void test_recovery_reboot_migration(void)
{
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree uint8_t *expected = NULL;
    g_autofree char *command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_path = NULL;
    g_autofree char *state = NULL;
    g_autofree char *status = NULL;
    g_autofree char *recovery_digest = NULL;
    QTestState *destination;
    QTestState *qts = start_recovery(
        false, false, UINT64_MAX, false, &eeprom_path, &sd_path, &expected);

    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "recovery-updated-reboot");
    recovery_digest = qom_get_string(qts, "recovery-sha256");
    g_assert_cmpstr(recovery_digest, !=, "none");
    g_assert_cmpuint(qom_get_uint32(qts, "recovery-key-index"), ==, 1);
    qtest_clock_step(qts, 4 * 1000 * 1000);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "recovery-trusted-sha256=%s,"
        "eeprom-write-protect=off,eeprom-fail-after=%" PRIu64 ","
        "eeprom-fail-stage=program "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=sd,format=raw,file=%s,file.locking=off",
        recovery_digest, UINT64_MAX, eeprom_path, sd_path);
    destination = migrate_to_new_qtest(qts, command, &migration_dir,
                                       &migration_path);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    status = qom_get_string(destination, "recovery-status");
    g_assert_cmpstr(state, ==, "recovery-updated-reboot");
    g_assert_cmpstr(status, ==, "recovery-updated-reboot");
    {
        g_autofree char *migrated_digest =
            qom_get_string(destination, "recovery-sha256");

        g_assert_cmpstr(migrated_digest, ==, recovery_digest);
    }
    g_assert_cmpuint(qom_get_uint32(destination, "recovery-key-index"),
                     ==, 1);
    qtest_clock_step(destination, 5999999);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "recovery-updated-reboot");
    qtest_clock_step(destination, 1);
    qtest_qmp_eventwait(destination, "RESET");
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "usb-discovery-wait");

    qtest_quit(qts);
    qtest_quit(destination);
    assert_file_equals(eeprom_path, expected, EEPROM_SIZE);
    unlink(eeprom_path);
    unlink(sd_path);
    unlink(migration_path);
    rmdir(migration_dir);
}

static void test_restart_watchdog_migration(void)
{
    g_autofree uint8_t *eeprom = make_eeprom_image(
        "BOOT_ORDER=0xef\nMAX_RESTARTS=2\n", false);
    g_autofree char *eeprom_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_path = NULL;
    g_autofree char *state = NULL;
    g_autofree char *cause = NULL;
    QTestState *destination;
    QTestState *qts;

    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off",
        eeprom_path);
    qts = qtest_init(command);
    qtest_clock_step(qts, 2 * 1000 * 1000);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "restart-watchdog-pending");
    g_assert_cmpuint(qom_get_uint64(qts, "boot-restart-count"), ==, 3);
    qtest_clock_step(qts, 4 * 1000 * 1000);

    destination = migrate_to_new_qtest(qts, command, &migration_dir,
                                       &migration_path);
    qtest_clock_step(destination,
                     TEST_RESTART_WATCHDOG_NS - 4 * 1000 * 1000 - 1);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "restart-watchdog-pending");
    qtest_clock_step(destination, 1);
    qtest_qmp_eventwait(destination, "RESET");
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    cause = qom_get_string(destination, "reset-cause");
    g_assert_cmpstr(state, ==, "restart-cycle-wait");
    g_assert_cmpstr(cause, ==, "watchdog");
    g_assert_cmpuint(qom_get_uint64(destination, "boot-restart-count"), ==,
                     1);

    qtest_quit(qts);
    qtest_quit(destination);
    unlink(eeprom_path);
    unlink(migration_path);
    rmdir(migration_dir);
}

static void test_boot_watchdog_deadline_partition_and_validation(void)
{
    {
        g_autofree char *path = NULL;
        g_autofree char *state = NULL;
        g_autofree char *cause = NULL;
        QTestState *qts = start_with_eeprom(
            "BOOT_ORDER=0x3\n"
            "BOOT_WATCHDOG_TIMEOUT=2\n"
            "BOOT_WATCHDOG_PARTITION=5\n"
            "[partition=5]\n"
            "BOOT_ORDER=0xe\n",
            false, &path);

        state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(state, ==, "rpiboot-wait");
        g_assert_cmpuint(qom_get_uint32(qts, "boot-watchdog-timeout-s"),
                         ==, 2);
        g_assert_cmpuint(qom_get_uint32(qts, "boot-watchdog-partition"),
                         ==, 5);
        g_assert_true(qom_get_bool(qts, "boot-watchdog-armed"));
        g_assert_cmpuint(qom_get_uint64(
                             qts, "boot-watchdog-remaining-ns"),
                         ==, INT64_C(2) * 1000 * 1000 * 1000);

        qtest_clock_step(qts, INT64_C(2) * 1000 * 1000 * 1000 - 1);
        g_assert_true(qom_get_bool(qts, "boot-watchdog-armed"));
        qtest_clock_step(qts, 1);
        qtest_qmp_eventwait(qts, "RESET");

        g_clear_pointer(&state, g_free);
        state = qom_get_string(qts, "boot-state");
        cause = qom_get_string(qts, "reset-cause");
        g_assert_cmpstr(state, ==, "stopped");
        g_assert_cmpstr(cause, ==, "watchdog");
        g_assert_cmpuint(qom_get_uint32(qts, "boot-partition"), ==, 5);
        qtest_quit(qts);
        unlink(path);
    }

    {
        static const char * const invalid[] = {
            "BOOT_ORDER=0x3\nBOOT_WATCHDOG_TIMEOUT=4294967296\n",
            "BOOT_ORDER=0x3\nBOOT_WATCHDOG_PARTITION=64\n",
            "BOOT_ORDER=0x3\nBOOT_WATCHDOG_TIMEOUT=-1\n",
        };

        for (size_t i = 0; i < ARRAY_SIZE(invalid); i++) {
            g_autofree char *path = NULL;
            g_autofree char *state = NULL;
            QTestState *qts = start_with_eeprom(
                invalid[i], false, &path);

            state = qom_get_string(qts, "boot-state");
            g_assert_cmpstr(state, ==, "eeprom-invalid");
            qtest_quit(qts);
            unlink(path);
        }
    }
}

static void test_boot_watchdog_handoff_cancels(void)
{
    static const uint8_t firmware[] = "watchdog handoff cancellation";
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree char *state = NULL;
    g_autofree char *cause = NULL;
    QTestState *qts = start_with_eeprom_and_sd(
        "BOOT_ORDER=0x1\nBOOT_WATCHDOG_TIMEOUT=1\n"
        "BOOT_WATCHDOG_PARTITION=5\n",
        "START4  ELF", firmware, sizeof(firmware), NULL,
        &eeprom_path, &sd_path);

    state = qom_get_string(qts, "boot-state");
    cause = qom_get_string(qts, "reset-cause");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(cause, ==, "power-on");
    g_assert_false(qom_get_bool(qts, "boot-watchdog-armed"));
    g_assert_cmpuint(qom_get_uint64(qts, "boot-watchdog-remaining-ns"),
                     ==, 0);
    qtest_clock_step(qts, INT64_C(2) * 1000 * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    g_clear_pointer(&cause, g_free);
    state = qom_get_string(qts, "boot-state");
    cause = qom_get_string(qts, "reset-cause");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(cause, ==, "power-on");

    qtest_quit(qts);
    unlink(eeprom_path);
    unlink(sd_path);
}

static void test_boot_watchdog_migration(void)
{
    g_autofree uint8_t *eeprom = make_eeprom_image(
        "BOOT_ORDER=0x3\nBOOT_WATCHDOG_TIMEOUT=3\n"
        "BOOT_WATCHDOG_PARTITION=5\n"
        "[partition=5]\nBOOT_ORDER=0xe\n",
        false);
    g_autofree char *eeprom_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_path = NULL;
    g_autofree char *state = NULL;
    g_autofree char *cause = NULL;
    QTestState *destination;
    QTestState *qts;

    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off",
        eeprom_path);
    qts = qtest_init(command);
    qtest_clock_step(qts, INT64_C(1) * 1000 * 1000 * 1000);
    destination = migrate_to_new_qtest(qts, command, &migration_dir,
                                       &migration_path);

    g_assert_true(qom_get_bool(destination, "boot-watchdog-armed"));
    g_assert_cmpuint(qom_get_uint64(
                         destination, "boot-watchdog-remaining-ns"),
                     ==, INT64_C(2) * 1000 * 1000 * 1000);
    qtest_clock_step(destination,
                     INT64_C(2) * 1000 * 1000 * 1000 - 1);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "rpiboot-wait");
    qtest_clock_step(destination, 1);
    qtest_qmp_eventwait(destination, "RESET");
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    cause = qom_get_string(destination, "reset-cause");
    g_assert_cmpstr(state, ==, "stopped");
    g_assert_cmpstr(cause, ==, "watchdog");
    g_assert_cmpuint(qom_get_uint32(destination, "boot-partition"), ==, 5);

    qtest_quit(qts);
    qtest_quit(destination);
    unlink(eeprom_path);
    unlink(migration_path);
    rmdir(migration_dir);
}



static void test_boot_order_wait_and_exhaustion(void)
{
    static const struct {
        const char *config;
        const char *state;
        const char *source;
        uint64_t attempts;
        uint8_t index;
    } cases[] = {
        { "BOOT_ORDER=0x0\n", "sd-card-detect-wait",
          "sd-card-detect", 1, 0 },
        { "BOOT_ORDER=0x31\n", "rpiboot-wait", "rpiboot", 2, 1 },
        { "BOOT_ORDER=0x41\nREBOOT_ON_FATAL_ERROR=0\n",
          "boot-order-exhausted", "none", 2, 2 },
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree char *path = NULL;
        QTestState *qts = start_with_eeprom(cases[i].config, false, &path);
        g_autofree char *state = NULL;
        g_autofree char *source = NULL;

        if (i == 2) {
            state = qom_get_string(qts, "boot-state");
            g_assert_cmpstr(state, ==, "usb-discovery-wait");
            g_clear_pointer(&state, g_free);
            qtest_clock_step(qts, INT64_C(20) * 1000 * 1000 * 1000);
        }
        state = qom_get_string(qts, "boot-state");
        source = qom_get_string(qts, "boot-source");

        g_assert_cmpstr(state, ==, cases[i].state);
        g_assert_cmpstr(source, ==, cases[i].source);
        g_assert_cmpuint(qom_get_uint64(qts, "boot-attempt-count"), ==,
                         cases[i].attempts);
        g_assert_cmpuint(qom_get_uint32(qts, "boot-order-index"), ==,
                         cases[i].index);
        qtest_quit(qts);
        unlink(path);
    }
}

static void test_boot_order_fatal_error_reboot(void)
{
    const uint64_t fatal_delay_ns = INT64_C(3) * 1000 * 1000 * 1000;
    g_autofree char *disabled_path = NULL;
    g_autofree char *eeprom_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_path = NULL;
    g_autofree char *state = NULL;
    g_autofree char *source = NULL;
    g_autofree char *cause = NULL;
    QTestState *destination;
    QTestState *qts = start_with_eeprom(
        "BOOT_ORDER=0x8\nREBOOT_ON_FATAL_ERROR=0\n",
        false, &disabled_path);

    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "boot-source");
    g_assert_cmpstr(state, ==, "boot-source-unsupported");
    g_assert_cmpstr(source, ==, "reserved-0x8");
    g_assert_false(qom_get_bool(qts, "boot-reboot-on-fatal-error"));
    g_assert_cmpuint(qom_get_uint64(
                         qts, "boot-fatal-error-remaining-ns"), ==, 0);
    qtest_clock_step(qts, fatal_delay_ns);
    g_assert_cmpuint(qom_get_uint64(
                         qts, "boot-fatal-error-reboot-count"), ==, 0);
    qtest_quit(qts);
    unlink(disabled_path);

    qts = start_with_eeprom("BOOT_ORDER=0x8\n", false, &eeprom_path);
    qtest_quit(qts);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
        "-drive if=none,id=pieeprom,format=raw,"
        "file=%s,file.locking=off -nic none",
        eeprom_path);
    qts = qtest_init(command);
    g_clear_pointer(&state, g_free);
    g_clear_pointer(&source, g_free);
    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "boot-source");
    g_assert_cmpstr(state, ==, "fatal-error-reboot-wait");
    g_assert_cmpstr(source, ==, "reserved-0x8");
    g_assert_true(qom_get_bool(qts, "boot-reboot-on-fatal-error"));
    g_assert_cmpuint(qom_get_uint64(
                         qts, "boot-fatal-error-remaining-ns"),
                     ==, fatal_delay_ns);
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND);

    destination = migrate_to_new_qtest(
        qts, command, &migration_dir, &migration_path);
    g_clear_pointer(&state, g_free);
    g_clear_pointer(&source, g_free);
    state = qom_get_string(destination, "boot-state");
    source = qom_get_string(destination, "boot-source");
    g_assert_cmpstr(state, ==, "fatal-error-reboot-wait");
    g_assert_cmpstr(source, ==, "reserved-0x8");
    g_assert_cmpuint(qom_get_uint64(
                         destination, "boot-fatal-error-remaining-ns"),
                     ==, INT64_C(2) * NANOSECONDS_PER_SECOND);
    qtest_clock_step(destination,
                     INT64_C(2) * NANOSECONDS_PER_SECOND);
    qtest_qmp_eventwait(destination, "RESET");
    g_clear_pointer(&state, g_free);
    g_clear_pointer(&cause, g_free);
    state = qom_get_string(destination, "boot-state");
    cause = qom_get_string(destination, "reset-cause");
    g_assert_cmpstr(state, ==, "fatal-error-reboot-wait");
    g_assert_cmpstr(cause, ==, "watchdog");
    g_assert_cmpuint(qom_get_uint64(
                         destination,
                         "boot-fatal-error-reboot-count"), ==, 1);
    g_assert_cmpuint(qom_get_uint64(
                         destination, "boot-fatal-error-remaining-ns"),
                     ==, fatal_delay_ns);

    qtest_quit(qts);
    qtest_quit(destination);
    unlink(eeprom_path);
    unlink(migration_path);
    rmdir(migration_dir);
}

static void test_boot_order_retry_and_restart_limits(void)
{
    static const struct {
        const char *config;
        const char *state;
        const char *source;
        uint64_t attempts;
        uint64_t retries;
        uint64_t restarts;
        uint64_t elapsed_ms;
        int32_t max_restarts;
        int32_t sd_max_retries;
    } cases[] = {
        { "BOOT_ORDER=0xe1\nSD_BOOT_MAX_RETRIES=2\n",
          "stopped", "stop", 3, 2, 0, 0, -1, 2 },
        { "BOOT_ORDER=0xe1\nSD_BOOT_MAX_RETRIES=-1\n",
          "sd-retry-loop", "sd-card", 1, 0, 0, 0, -1, -1 },
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree char *path = NULL;
        QTestState *qts = start_with_eeprom(cases[i].config, false, &path);
        g_autofree char *state = NULL;
        g_autofree char *source = NULL;

        state = qom_get_string(qts, "boot-state");
        source = qom_get_string(qts, "boot-source");

        g_assert_cmpstr(state, ==, cases[i].state);
        g_assert_cmpstr(source, ==, cases[i].source);
        g_assert_cmpuint(qom_get_uint64(qts, "boot-attempt-count"), ==,
                         cases[i].attempts);
        g_assert_cmpuint(qom_get_uint64(qts, "boot-retry-count"), ==,
                         cases[i].retries);
        g_assert_cmpuint(qom_get_uint64(qts, "boot-restart-count"), ==,
                         cases[i].restarts);
        g_assert_cmpuint(qom_get_uint64(qts, "boot-elapsed-ms"), ==,
                         cases[i].elapsed_ms);
        g_assert_cmpint(qom_get_int32(qts, "boot-max-restarts"), ==,
                        cases[i].max_restarts);
        g_assert_cmpint(qom_get_int32(qts, "boot-sd-max-retries"), ==,
                        cases[i].sd_max_retries);
        qtest_quit(qts);
        unlink(path);
    }

    {
        g_autofree char *path = NULL;
        g_autofree char *state = NULL;
        g_autofree char *source = NULL;
        g_autofree char *cause = NULL;
        QTestState *qts = start_with_eeprom(
            "BOOT_ORDER=0xf41\nMAX_RESTARTS=2\n", false, &path);

        state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(state, ==, "usb-discovery-wait");
        for (uint64_t restart = 1; restart <= 2; restart++) {
            qtest_clock_step(qts, INT64_C(20) * 1000 * 1000 * 1000);
            g_clear_pointer(&state, g_free);
            state = qom_get_string(qts, "boot-state");
            g_assert_cmpstr(state, ==, "restart-cycle-wait");
            g_assert_cmpuint(qom_get_uint64(
                                 qts, "boot-restart-count"), ==, restart);
            qtest_clock_step(qts, 1 * 1000 * 1000);
            g_clear_pointer(&state, g_free);
            state = qom_get_string(qts, "boot-state");
            g_assert_cmpstr(state, ==, "usb-discovery-wait");
        }

        qtest_clock_step(qts, INT64_C(20) * 1000 * 1000 * 1000);
        g_clear_pointer(&state, g_free);
        state = qom_get_string(qts, "boot-state");
        source = qom_get_string(qts, "boot-source");
        g_assert_cmpstr(state, ==, "restart-watchdog-pending");
        g_assert_cmpstr(source, ==, "restart");
        g_assert_cmpuint(qom_get_uint64(qts, "boot-attempt-count"), ==, 6);
        g_assert_cmpuint(qom_get_uint64(qts, "boot-restart-count"), ==, 3);
        g_assert_cmpuint(qom_get_uint64(qts, "boot-elapsed-ms"), ==, 60000);

        qtest_clock_step(qts, 11 * 1000 * 1000);
        qtest_qmp_eventwait(qts, "RESET");
        g_clear_pointer(&state, g_free);
        g_clear_pointer(&source, g_free);
        state = qom_get_string(qts, "boot-state");
        source = qom_get_string(qts, "boot-source");
        cause = qom_get_string(qts, "reset-cause");
        g_assert_cmpstr(state, ==, "usb-discovery-wait");
        g_assert_cmpstr(source, ==, "usb-msd");
        g_assert_cmpstr(cause, ==, "watchdog");
        g_assert_cmpuint(qom_get_uint64(qts, "boot-attempt-count"), ==, 2);
        g_assert_cmpuint(qom_get_uint64(qts, "boot-restart-count"), ==, 0);
        g_assert_cmpuint(qom_get_uint64(qts, "boot-elapsed-ms"), ==, 0);
        qtest_quit(qts);
        unlink(path);
    }
}

static void test_corrupt_eeprom_is_rejected(void)
{
    static const struct {
        const char *config;
        bool corrupt;
    } cases[] = {
        { "BOOT_ORDER=0xf41\n", true },
        { "BOOT_ORDER=0xf41\nSD_BOOT_MAX_RETRIES=-2\n", false },
        {
            "BOOT_ORDER=0xf4\n"
            "USB_MSD_EXCLUDE_VID_PID=46f4000\n",
            false,
        },
        {
            "BOOT_ORDER=0xf4\n"
            "USB_MSD_EXCLUDE_VID_PID=46f4000g\n",
            false,
        },
        {
            "BOOT_ORDER=0xf4\n"
            "USB_MSD_EXCLUDE_VID_PID=46f40001, 06270001\n",
            false,
        },
        {
            "BOOT_ORDER=0xf4\n"
            "USB_MSD_EXCLUDE_VID_PID= 46f40001\n",
            false,
        },
        {
            "BOOT_ORDER=0xf4\n"
            "USB_MSD_EXCLUDE_VID_PID=46f40001,\n",
            false,
        },
        {
            "BOOT_ORDER=0xf4\n"
            "USB_MSD_EXCLUDE_VID_PID=00000001,00000002,00000003,"
            "00000004,00000005\n",
            false,
        },
        { "BOOT_ORDER=0xf2\nNET_BOOT_MAX_RETRIES=-2\n", false },
        { "BOOT_ORDER=0xf2\nDHCP_TIMEOUT=4999\n", false },
        { "BOOT_ORDER=0xf2\nDHCP_REQ_TIMEOUT=499\n", false },
        { "BOOT_ORDER=0xf2\nDHCP_OPTION97=0x100000000\n", false },
        { "BOOT_ORDER=0xf2\nMAC_ADDRESS=dc:a6:32:01:36\n", false },
        { "BOOT_ORDER=0xf2\nMAC_ADDRESS=01:00:00:00:00:01\n", false },
        { "BOOT_ORDER=0xf2\nMAC_ADDRESS=00:00:00:00:00:00\n", false },
        { "BOOT_ORDER=0xf2\nMAC_ADDRESS_OTP=0,8\n", false },
        { "BOOT_ORDER=0xf2\nMAC_ADDRESS_OTP=0,0\n", false },
        { "BOOT_ORDER=0xf2\nMAC_ADDRESS_OTP=0, 1\n", false },
        { "BOOT_ORDER=0xf2\nTFTP_IP=10.0.2.999\n", false },
        { "BOOT_ORDER=0xf2\nTFTP_IP=224.0.0.1\n", false },
        { "BOOT_ORDER=0xf2\nTFTP_IP=255.255.255.255\n", false },
        { "BOOT_ORDER=0xf2\nTFTP_PREFIX=3\n", false },
        {
            "BOOT_ORDER=0xf2\n"
            "TFTP_PREFIX_STR=123456789012345678901234567890123\n",
            false,
        },
        { "BOOT_ORDER=0xf2\nCLIENT_IP=224.0.0.1\n", false },
        { "BOOT_ORDER=0xf2\nCLIENT_IP=255.255.255.255\n", false },
        { "BOOT_ORDER=0xf2\nSUBNET=255.0.255.0\n", false },
        { "BOOT_ORDER=0xf2\nSUBNET=0.0.0.0\n", false },
        { "BOOT_ORDER=0xf2\nGATEWAY=255.255.255.255\n", false },
        { "BOOT_ORDER=0xf2\nTFTP_FILE_TIMEOUT=4999\n", false },
        { "BOOT_ORDER=0xf7\nHTTP_PORT=0\n", false },
        { "BOOT_ORDER=0xf7\nHTTP_PORT=65536\n", false },
        { "BOOT_ORDER=0xf7\nHTTP_PATH=///\n", false },
        { "BOOT_ORDER=0xf7\nHTTP_PATH=net?install\n", false },
        { "BOOT_ORDER=0xf7\nHTTP_CACERT_HASH=1234\n", false },
        { "BOOT_ORDER=0xf7\nNET_INSTALL_ENABLED=2\n", false },
        { "BOOT_ORDER=0xf7\nNET_INSTALL_AT_POWER_ON=-1\n", false },
        { "BOOT_ORDER=0xf1\nENABLE_SELF_UPDATE=2\n", false },
        { "BOOT_ORDER=0xf1\nFREEZE_VERSION=-1\n", false },
        { "BOOT_ORDER=0xf1\nSIGNED_BOOT=2\n", false },
        { "BOOT_ORDER=0xf1\nPARTITION_WALK=2\n", false },
        { "BOOT_ORDER=0xe\nWAKE_ON_GPIO=2\n", false },
        { "BOOT_ORDER=0xe\nPOWER_OFF_ON_HALT=-1\n", false },
        { "BOOT_ORDER=0xe\nREBOOT_ON_FATAL_ERROR=2\n", false },
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree char *path = NULL;
        QTestState *qts = start_with_eeprom(
            cases[i].config, cases[i].corrupt, &path);
        g_autofree char *state = qom_get_string(qts, "boot-state");
        g_autofree char *source = qom_get_string(qts, "boot-source");

        g_assert_cmpstr(state, ==, "eeprom-invalid");
        g_assert_cmpstr(source, ==, "none");
        qtest_quit(qts);
        unlink(path);
    }
}

static void test_invalid_http_host_is_ignored(void)
{
    static const char * const machines[] = {
        "raspi4b",
        "raspi-cm4",
    };
    static const char * const invalid_hosts[] = {
        "Example.com",
        "-example.com",
        "example..com",
    };

    for (size_t machine = 0; machine < ARRAY_SIZE(machines); machine++) {
        for (size_t host = 0; host < ARRAY_SIZE(invalid_hosts); host++) {
            g_autofree char *config = g_strdup_printf(
                "BOOT_ORDER=0xe7\n"
                "HTTP_HOST=%s\n",
                invalid_hosts[host]);
            g_autofree uint8_t *eeprom =
                make_eeprom_image(config, false);
            g_autofree char *eeprom_path = NULL;
            g_autofree char *command = NULL;
            g_autofree char *state = NULL;
            g_autofree char *source = NULL;
            g_autofree char *http_host = NULL;
            g_autofree char *migration_dir = NULL;
            g_autofree char *migration_socket = NULL;
            QTestState *qts;

            write_temp_image("raspi4-invalid-http-host-XXXXXX", eeprom,
                             EEPROM_SIZE, &eeprom_path);
            command = g_strdup_printf(
                "-M %s,boot-mode=behavioral,eeprom-drive=pieeprom "
                "-drive if=none,id=pieeprom,format=raw,file=%s,"
                "file.locking=off -nic none",
                machines[machine], eeprom_path);
            qts = qtest_init(command);

            state = qom_get_string(qts, "boot-state");
            source = qom_get_string(qts, "boot-source");
            http_host = qom_get_string(qts, "boot-http-host");
            g_assert_cmpstr(state, ==, "network-dhcp-wait");
            g_assert_cmpstr(source, ==, "network");
            g_assert_cmpstr(http_host, ==, "");
            g_assert_cmpuint(qom_get_uint64(
                                 qts, "boot-attempt-count"), ==, 1);

            qtest_system_reset(qts);
            g_clear_pointer(&state, g_free);
            g_clear_pointer(&source, g_free);
            state = qom_get_string(qts, "boot-state");
            source = qom_get_string(qts, "boot-source");
            g_assert_cmpstr(state, ==, "network-dhcp-wait");
            g_assert_cmpstr(source, ==, "network");
            g_assert_cmpuint(qom_get_uint64(
                                 qts, "boot-attempt-count"), ==, 1);

            if (host == 0) {
                QTestState *destination = migrate_to_new_qtest(
                    qts, command, &migration_dir, &migration_socket);

                g_clear_pointer(&state, g_free);
                g_clear_pointer(&source, g_free);
                state = qom_get_string(destination, "boot-state");
                source = qom_get_string(destination, "boot-source");
                g_assert_cmpstr(state, ==, "network-dhcp-wait");
                g_assert_cmpstr(source, ==, "network");
                g_assert_cmpuint(qom_get_uint64(
                                     destination,
                                     "boot-attempt-count"), ==, 1);
                qtest_quit(destination);
                unlink(migration_socket);
                rmdir(migration_dir);
            }

            qtest_quit(qts);
            unlink(eeprom_path);
        }
    }
}

static void test_eeprom_geometry(void)
{
    static const size_t invalid_sizes[] = {
        EEPROM_SIZE - 512,
        4 * EEPROM_SIZE,
    };
    g_autofree uint8_t *valid =
        make_eeprom_image("BOOT_ORDER=0xe\n", false);
    g_autofree uint8_t *invalid = g_malloc(4 * EEPROM_SIZE);
    g_autofree char *exact_path = NULL;
    g_autofree char *state = NULL;
    QTestState *qts;

    qts = start_with_eeprom("BOOT_ORDER=0xe\n", false, &exact_path);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "stopped");
    g_assert_cmpuint(qom_get_uint32(qts, "boot-order"), ==, 0xe);
    qtest_quit(qts);

    memset(invalid, 0xff, 4 * EEPROM_SIZE);
    memcpy(invalid, valid, EEPROM_SIZE);
    for (size_t i = 0; i < ARRAY_SIZE(invalid_sizes); i++) {
        g_autofree char *invalid_path = NULL;
        g_autofree char *command = NULL;

        write_temp_image("raspi4-eeprom-size-XXXXXX", invalid,
                         invalid_sizes[i], &invalid_path);
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
            "-drive if=none,id=pieeprom,format=raw,file=%s",
            invalid_path);
        qts = qtest_init(command);
        g_clear_pointer(&state, g_free);
        state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(state, ==, "eeprom-invalid");
        qtest_quit(qts);
        unlink(invalid_path);
    }

    unlink(exact_path);
}














static void test_otp_backing_and_rpiboot_policy(void)
{
    g_autofree char *path = NULL;
    g_autofree uint8_t *expected = make_otp_image(
        0, 0, PI4_BOARD_REVISION, false);
    g_autofree char *state = NULL;
    QTestState *qts = start_with_otp(
        0, 0, PI4_BOARD_REVISION, false, 8, true, &path);

    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "rpiboot-wait");
    g_assert_cmphex(qom_get_uint32(qts, "otp-bootmode"), ==, 0);
    g_assert_cmphex(qom_get_uint32(qts, "otp-board-revision"), ==,
                    PI4_BOARD_REVISION);
    g_assert_false(qtest_qom_get_bool(qts, "/machine", "otp-secure-boot"));

    qtest_system_reset(qts);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "rpiboot-wait");
    qtest_quit(qts);
    assert_file_equals(path, expected, OTP_SIZE);
    unlink(path);
}

static void test_otp_identity_validation(void)
{
    static const struct {
        uint32_t bootmode;
        uint32_t copy;
        uint32_t board_revision;
        const char *state;
    } cases[] = {
        { 1, 0, PI4_BOARD_REVISION, "otp-bootmode-invalid" },
        { 0, 0, 0x00c03115, "otp-board-mismatch" },
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree char *path = NULL;
        QTestState *qts = start_with_otp(
            cases[i].bootmode, cases[i].copy, cases[i].board_revision,
            false, 0, false, &path);
        g_autofree char *state = qom_get_string(qts, "boot-state");

        g_assert_cmpstr(state, ==, cases[i].state);
        qtest_quit(qts);
        unlink(path);
    }
}

static void test_otp_secure_boot_fails_closed(void)
{
    static const struct {
        bool customer_key;
        const char *state;
    } cases[] = {
        { false, "secure-boot-key-missing" },
        { true, "secure-boot-eeprom-missing" },
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree char *path = NULL;
        QTestState *qts = start_with_otp(
            1U << 15, 1U << 15, PI4_BOARD_REVISION,
            cases[i].customer_key,
            0, false, &path);
        g_autofree char *state = qom_get_string(qts, "boot-state");

        g_assert_cmpstr(state, ==, cases[i].state);
        g_assert_true(qtest_qom_get_bool(qts, "/machine",
                                        "otp-secure-boot"));
        qtest_quit(qts);
        unlink(path);
    }
}










static void test_secure_eeprom_verification(void)
{
    static const char key_hash_hex[] =
        "dea09d2a3225f6e195b2aba975d991c19acef3534a55c0c7e7c88589957d3152";
    static const struct {
        bool omit_signature;
        bool tamper_config;
        bool tamper_signature;
        bool tamper_otp;
        TestDuplicateEepromFile duplicate_file;
        TestBootsysFault bootsys_fault;
        const char *status;
    } cases[] = {
        {
            false, false, false, false, TEST_EEPROM_DUPLICATE_NONE,
            TEST_BOOTSYS_VALID, "config-verified",
        },
        { true, false, false, false, TEST_EEPROM_DUPLICATE_NONE,
          TEST_BOOTSYS_VALID,
          "signature-format-invalid" },
        { false, true, false, false, TEST_EEPROM_DUPLICATE_NONE,
          TEST_BOOTSYS_VALID,
          "image-hash-mismatch" },
        { false, false, true, false, TEST_EEPROM_DUPLICATE_NONE,
          TEST_BOOTSYS_VALID,
          "rsa-signature-invalid" },
        { false, false, false, true, TEST_EEPROM_DUPLICATE_NONE,
          TEST_BOOTSYS_VALID,
          "key-hash-mismatch" },
        {
            false, false, false, false, TEST_EEPROM_DUPLICATE_BOOTCONF,
            TEST_BOOTSYS_VALID, "signature-format-invalid",
        },
        {
            false, false, false, false, TEST_EEPROM_DUPLICATE_SIGNATURE,
            TEST_BOOTSYS_VALID, "signature-format-invalid",
        },
        {
            false, false, false, false, TEST_EEPROM_DUPLICATE_PUBLIC_KEY,
            TEST_BOOTSYS_VALID, "signature-format-invalid",
        },
        {
            false, false, false, false, TEST_EEPROM_DUPLICATE_NONE,
            TEST_BOOTSYS_TRUST_MISSING, "bootsys-trust-missing",
        },
        {
            false, false, false, false, TEST_EEPROM_DUPLICATE_NONE,
            TEST_BOOTSYS_WRONG_TRUST, "bootsys-hash-mismatch",
        },
        {
            false, false, false, false, TEST_EEPROM_DUPLICATE_NONE,
            TEST_BOOTSYS_MISSING, "bootsys-missing",
        },
        {
            false, false, false, false, TEST_EEPROM_DUPLICATE_NONE,
            TEST_BOOTSYS_FORMAT, "bootsys-format-invalid",
        },
        {
            false, false, false, false, TEST_EEPROM_DUPLICATE_NONE,
            TEST_DEPENDENCY_MISSING, "dependency-missing",
        },
        {
            false, false, false, false, TEST_EEPROM_DUPLICATE_NONE,
            TEST_DEPENDENCY_FORMAT, "dependency-format-invalid",
        },
        {
            false, false, false, false, TEST_EEPROM_DUPLICATE_NONE,
            TEST_DEPENDENCY_HEADER_CHECKSUM, "dependency-format-invalid",
        },
        {
            false, false, false, false, TEST_EEPROM_DUPLICATE_NONE,
            TEST_DEPENDENCY_HASH, "dependency-hash-mismatch",
        },
    };
    uint8_t key_hash[32];

    test_decode_hex(key_hash_hex, key_hash, sizeof(key_hash));
    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree uint8_t *eeprom = make_signed_eeprom(
            cases[i].omit_signature, cases[i].tamper_config,
            cases[i].tamper_signature, false, cases[i].duplicate_file);
        g_autofree uint8_t *otp = make_secure_otp_image(key_hash);
        g_autofree char *eeprom_path = NULL;
        g_autofree char *otp_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *state = NULL;
        const char *trust_option = TEST_BOOTSYS_MACHINE_OPTION;
        QTestState *qts;

        if (cases[i].tamper_otp) {
            otp[(47 - 1) * 4] ^= 1;
        }
        switch (cases[i].bootsys_fault) {
        case TEST_BOOTSYS_VALID:
            break;
        case TEST_BOOTSYS_TRUST_MISSING:
            trust_option = "";
            break;
        case TEST_BOOTSYS_WRONG_TRUST:
            trust_option = TEST_BOOTSYS_WRONG_MACHINE_OPTION;
            break;
        case TEST_BOOTSYS_MISSING:
            stl_be_p(eeprom, 0x55aafeef);
            break;
        case TEST_BOOTSYS_FORMAT:
            stl_le_p(eeprom + 8 + TEST_BOOTSYS_PAYLOAD_SIZE,
                     TEST_BOOTSYS_PAYLOAD_SIZE - 1);
            break;
        case TEST_DEPENDENCY_MISSING:
            stl_be_p(eeprom + TEST_BOOTSYS_SECTION_SIZE, 0x55aafeef);
            break;
        case TEST_DEPENDENCY_FORMAT:
            eeprom[TEST_BOOTSYS_SECTION_SIZE + 8 +
                   TEST_DEPENDENCY_NAME_SIZE] ^= 1;
            break;
        case TEST_DEPENDENCY_HEADER_CHECKSUM:
            eeprom[TEST_BOOTSYS_SECTION_SIZE + 8 +
                   TEST_DEPENDENCY_NAME_SIZE + 14] ^= 1;
            break;
        case TEST_DEPENDENCY_HASH:
            eeprom[TEST_BOOTSYS_SECTION_SIZE + 8 +
                   TEST_DEPENDENCY_NAME_SIZE + 15 + sizeof(uint32_t)] ^= 1;
            break;
        default:
            g_assert_not_reached();
        }
        write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                         &eeprom_path);
        write_temp_image("raspi4-otp-XXXXXX", otp, OTP_SIZE, &otp_path);
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "otp-drive=piotp%s "
            "-drive if=none,id=pieeprom,format=raw,file=%s "
            "-drive if=none,id=piotp,format=raw,file=%s",
            trust_option, eeprom_path, otp_path);
        qts = qtest_init(command);
        state = qom_get_string(qts, "secure-boot-status");
        g_assert_cmpstr(state, ==, cases[i].status);
        if (!i) {
            g_autofree char *bootsys_hash =
                qom_get_string(qts, "bootsys-sha256");

            g_assert_cmpstr(bootsys_hash, ==, TEST_BOOTSYS_SHA256);
            g_assert_cmpuint(
                qom_get_uint32(qts, "bootsys-dependency-count"), ==, 3);
            g_autofree char *dependencies_hash =
                qom_get_string(qts, "bootsys-dependencies-sha256");

            g_assert_cmpstr(dependencies_hash, ==,
                            TEST_DEPENDENCIES_SHA256);
            qtest_system_reset(qts);
            g_clear_pointer(&state, g_free);
            state = qom_get_string(qts, "secure-boot-status");
            g_assert_cmpstr(state, ==, cases[i].status);
        }
        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(otp_path);
    }
}

static void test_secure_bootsys_development_key_revocation(void)
{
    static const char key_hash_hex[] =
        "dea09d2a3225f6e195b2aba975d991c19acef3534a55c0c7e7c88589957d3152";
    static const struct {
        uint32_t key_index;
        uint32_t otp_flags;
        const char *status;
    } cases[] = {
        { 0, 0, "config-verified" },
        {
            0, BCM2711_OTP_SECURE_BOOT_FLAGS_REVOKE_DEVKEY,
            "bootsys-development-key-revoked",
        },
        { 1, BCM2711_OTP_SECURE_BOOT_FLAGS_PRODUCTION, "config-verified" },
        { 2, BCM2711_OTP_SECURE_BOOT_FLAGS_PRODUCTION, "config-verified" },
        { 3, BCM2711_OTP_SECURE_BOOT_FLAGS_PRODUCTION, "config-verified" },
        { 4, BCM2711_OTP_SECURE_BOOT_FLAGS_PRODUCTION, "config-verified" },
        {
            5, BCM2711_OTP_SECURE_BOOT_FLAGS_PRODUCTION,
            "bootsys-format-invalid",
        },
    };
    uint8_t key_hash[32];

    test_decode_hex(key_hash_hex, key_hash, sizeof(key_hash));
    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree uint8_t *eeprom = make_signed_eeprom(
            false, false, false, 0, TEST_EEPROM_DUPLICATE_NONE);
        g_autofree uint8_t *otp = make_secure_otp_image(key_hash);
        g_autofree char *eeprom_path = NULL;
        g_autofree char *otp_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *status = NULL;
        g_autofree char *bootsys_hash = NULL;
        QTestState *qts;

        stl_le_p(eeprom + 8 + TEST_BOOTSYS_PAYLOAD_SIZE +
                 sizeof(uint32_t), cases[i].key_index);
        stl_le_p(otp + (BCM2711_OTP_SECURE_BOOT_FLAGS_ROW - 1) *
                 sizeof(uint32_t), cases[i].otp_flags);
        bootsys_hash = g_compute_checksum_for_data(
            G_CHECKSUM_SHA256, eeprom + 8, ldl_be_p(eeprom + 4));
        write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                         &eeprom_path);
        write_temp_image("raspi4-otp-XXXXXX", otp, OTP_SIZE, &otp_path);
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "otp-drive=piotp,bootsys-trusted-sha256=%s "
            "-drive if=none,id=pieeprom,format=raw,file=%s "
            "-drive if=none,id=piotp,format=raw,file=%s -nic none",
            bootsys_hash, eeprom_path, otp_path);
        qts = qtest_init(command);
        status = qom_get_string(qts, "secure-boot-status");
        g_assert_cmpstr(status, ==, cases[i].status);
        g_assert_cmpuint(qom_get_uint32(qts, "bootsys-key-index"), ==,
                         cases[i].key_index);
        g_assert_cmpuint(qom_get_uint32(qts, "otp-secure-boot-flags"), ==,
                         cases[i].otp_flags);

        qtest_system_reset(qts);
        g_clear_pointer(&status, g_free);
        status = qom_get_string(qts, "secure-boot-status");
        g_assert_cmpstr(status, ==, cases[i].status);
        g_assert_cmpuint(qom_get_uint32(qts, "bootsys-key-index"), ==,
                         cases[i].key_index);
        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(otp_path);
    }
}

static void test_secure_boot_image_boundary(void)
{
    static const char key_hash_hex[] =
        "dea09d2a3225f6e195b2aba975d991c19acef3534a55c0c7e7c88589957d3152";
    static const struct {
        bool omit_image;
        bool omit_signature;
        bool tamper_image;
        bool tamper_signature;
        const char *status;
    } cases[] = {
        { false, false, false, false, "image-verified" },
        { true, false, false, false, "image-missing" },
        { false, true, false, false, "image-missing" },
        { false, false, true, false, "image-hash-mismatch" },
        { false, false, false, true, "rsa-signature-invalid" },
    };
    uint8_t key_hash[32];
    size_t inner_size;
    g_autofree uint8_t *inner = make_secure_inner_boot_image(&inner_size);
    g_autofree char *signed_inner = make_secure_boot_signature();

    test_decode_hex(key_hash_hex, key_hash, sizeof(key_hash));
    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree uint8_t *eeprom = make_signed_eeprom(
            false, false, false, false, TEST_EEPROM_DUPLICATE_NONE);
        g_autofree uint8_t *otp = make_secure_otp_image(key_hash);
        g_autofree uint8_t *boot_image = NULL;
        g_autofree uint8_t *boot_signature = NULL;
        g_autofree char *eeprom_path = NULL;
        g_autofree char *otp_path = NULL;
        g_autofree char *sd_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *status = NULL;
        size_t signature_size = strlen(signed_inner);
        Fat16Builder builder;
        QTestState *qts;

        boot_image = g_memdup2(inner, inner_size);
        boot_signature = g_memdup2(signed_inner, signature_size);
        if (cases[i].tamper_image) {
            boot_image[0] ^= 1;
        }
        if (cases[i].tamper_signature) {
            boot_signature[signature_size - 2] =
                boot_signature[signature_size - 2] == '0' ? '1' : '0';
        }
        fat16_init(&builder);
        if (!cases[i].omit_image) {
            fat16_add_file(&builder, "BOOT    IMG", boot_image, inner_size);
        }
        if (!cases[i].omit_signature) {
            fat16_add_file(&builder, "BOOT    SIG", boot_signature,
                           signature_size);
        }
        write_temp_image("raspi4-sd-XXXXXX", builder.image, SD_SIZE,
                         &sd_path);
        g_free(builder.image);
        write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                         &eeprom_path);
        write_temp_image("raspi4-otp-XXXXXX", otp, OTP_SIZE, &otp_path);
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "otp-drive=piotp" TEST_BOOTSYS_MACHINE_OPTION " "
            "-drive if=none,id=pieeprom,format=raw,file=%s "
            "-drive if=none,id=piotp,format=raw,file=%s "
            "-drive if=sd,format=raw,file=%s -nic none",
            eeprom_path, otp_path, sd_path);
        qts = qtest_init(command);
        status = qom_get_string(qts, "secure-boot-status");
        g_assert_cmpstr(status, ==, cases[i].status);
        if (!i) {
            g_autofree char *state = qom_get_string(qts, "boot-state");
            g_autofree char *source = qom_get_string(qts, "boot-source");
            g_autofree char *image_sha256 = g_compute_checksum_for_data(
                G_CHECKSUM_SHA256, boot_image, inner_size);
            g_autofree char *signature_sha256 = g_compute_checksum_for_data(
                G_CHECKSUM_SHA256, boot_signature, signature_size);
            g_autofree char *observed_image_sha256 =
                qom_get_string(qts, "secure-boot-image-sha256");
            g_autofree char *observed_signature_sha256 =
                qom_get_string(qts, "secure-boot-signature-sha256");

            g_assert_cmpstr(state, ==, "arm-handoff-ready");
            g_assert_cmpstr(source, ==, "sd-card");
            g_assert_cmpuint(qom_get_uint32(
                                 qts, "secure-boot-image-size"),
                             ==, inner_size);
            g_assert_cmpuint(qom_get_uint32(
                                 qts, "secure-boot-signature-size"),
                             ==, signature_size);
            g_assert_cmpstr(observed_image_sha256, ==, image_sha256);
            g_assert_cmpstr(observed_signature_sha256, ==,
                            signature_sha256);
            g_assert_cmpuint(qom_get_uint32(
                                 qts, "bootloader-signed"), ==, 9);
            g_assert_cmpuint(firmware_bootloader_u32(
                                 qts, "signed"), ==, 9);
            qtest_system_reset(qts);
            g_clear_pointer(&status, g_free);
            status = qom_get_string(qts, "secure-boot-status");
            g_assert_cmpstr(status, ==, cases[i].status);
            g_assert_cmpuint(qom_get_uint32(
                                 qts, "bootloader-signed"), ==, 9);
            g_assert_cmpuint(firmware_bootloader_u32(
                                 qts, "signed"), ==, 9);
        }
        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(otp_path);
        unlink(sd_path);
    }
}

static void test_secure_nvme_boot(void)
{
    static const char key_hash_hex[] =
        "42a9474e81932752067e2ec4b2c08ace08b9f0348014c24cd854fbdcef19b4f8";
    static const struct {
        bool tamper_image;
        const char *state;
        const char *status;
    } cases[] = {
        { false, "arm-handoff-ready", "image-verified" },
        { true, "restart-loop", "image-hash-mismatch" },
    };
    size_t inner_size;
    g_autofree uint8_t *inner = make_secure_inner_boot_image(&inner_size);
    g_autofree char *signed_inner = make_secure_nvme_boot_signature();
    uint8_t key_hash[32];

    test_decode_hex(key_hash_hex, key_hash, sizeof(key_hash));
    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree uint8_t *eeprom = make_signed_nvme_eeprom();
        g_autofree uint8_t *otp = make_secure_otp_image(key_hash);
        g_autofree uint8_t *boot_image = g_memdup2(inner, inner_size);
        g_autofree char *eeprom_path = NULL;
        g_autofree char *otp_path = NULL;
        g_autofree char *nvme_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *state = NULL;
        g_autofree char *status = NULL;
        g_autofree char *source = NULL;
        Fat16Builder builder;
        QTestState *qts;

        if (cases[i].tamper_image) {
            boot_image[0] ^= 1;
        }
        fat16_init(&builder);
        fat16_add_file(&builder, "BOOT    IMG", boot_image, inner_size);
        fat16_add_file(&builder, "BOOT    SIG",
                       (const uint8_t *)signed_inner,
                       strlen(signed_inner));
        write_temp_image("raspi4-nvme-XXXXXX", builder.image, SD_SIZE,
                         &nvme_path);
        g_free(builder.image);
        write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                         &eeprom_path);
        write_temp_image("raspi4-otp-XXXXXX", otp, OTP_SIZE, &otp_path);
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "otp-drive=piotp" TEST_BOOTSYS_MACHINE_OPTION
            ",nvme-drive=nvmeboot "
            "-drive if=none,id=pieeprom,format=raw,file=%s "
            "-drive if=none,id=piotp,format=raw,file=%s "
            "-drive if=none,id=nvmeboot,format=raw,file=%s",
            eeprom_path, otp_path, nvme_path);
        qts = qtest_init(command);
        state = qom_get_string(qts, "boot-state");
        status = qom_get_string(qts, "secure-boot-status");
        source = qom_get_string(qts, "boot-source");
        g_assert_cmpstr(state, ==, cases[i].state);
        g_assert_cmpstr(status, ==, cases[i].status);
        if (!cases[i].tamper_image) {
            g_assert_cmpstr(source, ==, "nvme");
            g_assert_cmpuint(qom_get_uint32(
                                 qts, "secure-boot-image-size"),
                             ==, inner_size);
            g_assert_cmpuint(qom_get_uint64(
                                 qts, "boot-attempt-count"), ==, 1);
        }
        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(otp_path);
        unlink(nvme_path);
    }
}

static void test_reset_status_survives_warm_reset(void)
{
    const uint64_t pm_base = 0xfe100000;
    QTestState *qts = qtest_init("-M raspi4b,boot-mode=behavioral");
    g_autofree char *cause = qom_get_string(qts, "reset-cause");

    g_assert_cmpstr(cause, ==, "power-on");
    g_assert_cmphex(qom_get_uint32(qts, "reset-status"), ==, 0x1000);
    g_assert_cmpuint(qom_get_uint32(qts, "boot-partition"), ==, 0);

    /* Partition 5 is encoded in alternating PM_RSTS bits 0 and 4. */
    qtest_writel(qts, pm_base + 0x20, 0x5a000011);
    qtest_writel(qts, pm_base + 0x24, 0x5a00000a);
    qtest_writel(qts, pm_base + 0x1c, 0x5a000020);
    g_clear_pointer(&cause, g_free);
    cause = qom_get_string(qts, "reset-cause");
    g_assert_cmpstr(cause, ==, "power-on");
    qtest_clock_step(qts, 1000000000LL);
    qtest_qmp_eventwait(qts, "RESET");

    g_clear_pointer(&cause, g_free);
    cause = qom_get_string(qts, "reset-cause");
    g_assert_cmpstr(cause, ==, "watchdog");
    g_assert_cmphex(qom_get_uint32(qts, "reset-status"), ==, 0x31);
    g_assert_cmpuint(qom_get_uint32(qts, "boot-partition"), ==, 5);
    g_assert_cmphex(qtest_readl(qts, pm_base + 0x20), ==, 0x31);

    qtest_writel(qts, pm_base + 0x20, 0x5a000200);
    qtest_system_reset(qts);
    g_clear_pointer(&cause, g_free);
    cause = qom_get_string(qts, "reset-cause");
    g_assert_cmpstr(cause, ==, "software");
    g_assert_cmphex(qom_get_uint32(qts, "reset-status"), ==, 0x200);
    qtest_quit(qts);
}

static void test_watchdog_can_be_cancelled(void)
{
    const uint64_t pm_base = 0xfe100000;
    QTestState *qts = qtest_init("-M raspi4b,boot-mode=behavioral");
    g_autofree char *cause = NULL;

    qtest_writel(qts, pm_base + 0x24, 0x5a00000a);
    qtest_writel(qts, pm_base + 0x1c, 0x5a000020);
    qtest_writel(qts, pm_base + 0x1c, 0x5a000102);
    qtest_clock_step(qts, 1000000000LL);

    cause = qom_get_string(qts, "reset-cause");
    g_assert_cmpstr(cause, ==, "power-on");
    g_assert_cmphex(qtest_readl(qts, pm_base + 0x1c), ==, 0x102);
    qtest_quit(qts);
}


static void test_halt_power_policy(void)
{
    static const struct {
        const char *machine;
        const char *config;
    } gpio_cases[] = {
        { "raspi4b", "BOOT_ORDER=0xe\n" },
        {
            "raspi-cm4",
            "BOOT_ORDER=0xe\nWAKE_ON_GPIO=1\nPOWER_OFF_ON_HALT=1\n",
        },
    };

    for (size_t i = 0; i < ARRAY_SIZE(gpio_cases); i++) {
        g_autofree uint8_t *eeprom =
            make_eeprom_image(gpio_cases[i].config, false);
        g_autofree char *eeprom_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *halt_state = NULL;
        QTestState *qts;

        write_temp_image("raspi4-halt-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                         &eeprom_path);
        command = g_strdup_printf(
            "-M %s,boot-mode=behavioral,eeprom-drive=pieeprom "
            "-drive if=none,id=pieeprom,format=raw,file=%s,"
            "file.locking=off -nic none",
            gpio_cases[i].machine, eeprom_path);
        qts = qtest_init(command);
        qtest_set_irq_in(qts, GPIO_QOM_PATH, "pin-input", 3, 1);
        g_assert_true(qom_get_bool(qts, "boot-wake-on-gpio"));
        raspi4_trigger_guest_halt(qts);
        qtest_qmp_eventwait(qts, "SUSPEND");
        halt_state = qom_get_string(qts, "halt-state");
        g_assert_cmpstr(halt_state, ==, "halted-gpio-wake");
        qtest_set_irq_in(qts, GPIO_QOM_PATH, "pin-input", 3, 0);
        qtest_qmp_eventwait(qts, "RESET");
        g_clear_pointer(&halt_state, g_free);
        halt_state = qom_get_string(qts, "halt-state");
        g_assert_cmpstr(halt_state, ==, "running");
        qtest_quit(qts);
        unlink(eeprom_path);
    }

    {
        g_autofree char *eeprom_path = NULL;
        g_autofree char *halt_state = NULL;
        QTestState *qts = start_with_eeprom(
            "BOOT_ORDER=0xe\nWAKE_ON_GPIO=0\nPOWER_OFF_ON_HALT=0\n",
            false, &eeprom_path);

        qtest_set_irq_in(qts, GPIO_QOM_PATH, "pin-input", 3, 1);
        g_assert_false(qom_get_bool(qts, "boot-wake-on-gpio"));
        g_assert_false(qom_get_bool(qts, "boot-power-off-on-halt"));
        raspi4_trigger_guest_halt(qts);
        qtest_qmp_eventwait(qts, "SUSPEND");
        halt_state = qom_get_string(qts, "halt-state");
        g_assert_cmpstr(halt_state, ==, "halted-global-en");
        qtest_set_irq_in(qts, GPIO_QOM_PATH, "pin-input", 3, 0);
        g_clear_pointer(&halt_state, g_free);
        halt_state = qom_get_string(qts, "halt-state");
        g_assert_cmpstr(halt_state, ==, "halted-global-en");
        qom_set_bool(qts, "global-en", false);
        qtest_qmp_eventwait(qts, "RESET");
        g_clear_pointer(&halt_state, g_free);
        halt_state = qom_get_string(qts, "halt-state");
        g_assert_cmpstr(halt_state, ==, "running");
        qtest_quit(qts);
        unlink(eeprom_path);
    }

    {
        g_autofree uint8_t *eeprom = make_eeprom_image(
            "BOOT_ORDER=0xe\nWAKE_ON_GPIO=0\nPOWER_OFF_ON_HALT=1\n",
            false);
        g_autofree char *eeprom_path = NULL;
        g_autofree char *halt_state = NULL;
        QTestState *qts;

        write_temp_image("raspi4-poweroff-eeprom-XXXXXX", eeprom,
                         EEPROM_SIZE, &eeprom_path);
        qts = qtest_initf(
            "-no-shutdown "
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
            "-drive if=none,id=pieeprom,format=raw,file=%s,"
            "file.locking=off -nic none", eeprom_path);
        g_assert_true(qom_get_bool(qts, "boot-power-off-on-halt"));
        raspi4_trigger_guest_halt(qts);
        qtest_qmp_eventwait(qts, "SHUTDOWN");
        halt_state = qom_get_string(qts, "halt-state");
        g_assert_cmpstr(halt_state, ==, "powered-off");
        qtest_quit(qts);
        unlink(eeprom_path);
    }
}

static void test_halt_power_policy_migration(void)
{
    g_autofree uint8_t *eeprom = make_eeprom_image(
        "BOOT_ORDER=0xe\nWAKE_ON_GPIO=0\nPOWER_OFF_ON_HALT=0\n", false);
    g_autofree char *eeprom_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    g_autofree char *halt_state = NULL;
    QTestState *source;
    QTestState *destination;

    write_temp_image("raspi4-halt-migration-eeprom-XXXXXX", eeprom,
                     EEPROM_SIZE, &eeprom_path);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
        "-drive if=none,id=pieeprom,format=raw,file=%s,"
        "file.locking=off -nic none", eeprom_path);
    source = qtest_init(command);
    destination = migrate_to_new_qtest(
        source, command, &migration_dir, &migration_socket);
    qtest_quit(source);

    g_assert_false(qom_get_bool(destination, "boot-wake-on-gpio"));
    g_assert_false(qom_get_bool(destination, "boot-power-off-on-halt"));
    raspi4_trigger_guest_halt(destination);
    qtest_qmp_eventwait(destination, "SUSPEND");
    halt_state = qom_get_string(destination, "halt-state");
    g_assert_cmpstr(halt_state, ==, "halted-global-en");
    qom_set_bool(destination, "global-en", false);
    qtest_qmp_eventwait(destination, "RESET");
    g_clear_pointer(&halt_state, g_free);
    halt_state = qom_get_string(destination, "halt-state");
    g_assert_cmpstr(halt_state, ==, "running");

    qtest_quit(destination);
    unlink(migration_socket);
    rmdir(migration_dir);
    unlink(eeprom_path);
}

#ifndef _WIN32
static void test_halt_gpio_bridge_wake(void)
{
    g_autofree uint8_t *eeprom =
        make_eeprom_image("BOOT_ORDER=0xe\nWAKE_ON_GPIO=1\n", false);
    g_autofree char *eeprom_path = NULL;
    g_autofree char *socket_dir = NULL;
    g_autofree char *socket_path = NULL;
    g_autofree char *response = NULL;
    g_autofree char *halt_state = NULL;
    QTestState *qts;
    int fd;

    socket_dir = g_dir_make_tmp("qtest-rpi-halt-gpio-XXXXXX", NULL);
    g_assert_nonnull(socket_dir);
    socket_path = g_strdup_printf("%s/gpio.sock", socket_dir);
    write_temp_image("raspi4-halt-gpio-eeprom-XXXXXX", eeprom,
                     EEPROM_SIZE, &eeprom_path);
    qts = qtest_initf(
        "-chardev socket,id=gpio,path=%s,server=on,wait=off "
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "gpio-chardev=gpio "
        "-drive if=none,id=pieeprom,format=raw,file=%s,"
        "file.locking=off -nic none",
        socket_path, eeprom_path);
    fd = gpio_bridge_connect(socket_path);
    g_assert_cmpint(fd, >=, 0);
    response = gpio_bridge_read(fd);
    g_assert_nonnull(strstr(response, "RPI-GPIO 1 58\n"));

    gpio_bridge_write(fd, "SET 3 1\n");
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(fd);
    g_assert_nonnull(strstr(response, "PIN 3 1 0\nOK\n"));
    raspi4_trigger_guest_halt(qts);
    qtest_qmp_eventwait(qts, "SUSPEND");
    halt_state = qom_get_string(qts, "halt-state");
    g_assert_cmpstr(halt_state, ==, "halted-gpio-wake");

    gpio_bridge_write(fd, "SET 3 0\n");
    g_clear_pointer(&response, g_free);
    response = gpio_bridge_read(fd);
    g_assert_nonnull(strstr(response, "PIN 3 0 0\nOK\n"));
    qtest_qmp_eventwait(qts, "RESET");
    g_clear_pointer(&halt_state, g_free);
    halt_state = qom_get_string(qts, "halt-state");
    g_assert_cmpstr(halt_state, ==, "running");

    close(fd);
    qtest_quit(qts);
    unlink(socket_path);
    rmdir(socket_dir);
    unlink(eeprom_path);
}
#endif






#ifndef _WIN32




static void test_eeprom_netconsole(void)
{
    static const char * const machines[] = {
        "raspi4b",
        "raspi-cm4",
    };
    static const char config[] =
        "BOOT_ORDER=0xe\n"
        "REBOOT_ON_FATAL_ERROR=0\n"
        "DHCP_TIMEOUT=5000\n"
        "NETCONSOLE=6665@169.254.1.1/,6666@/\n";
    const uint64_t second = INT64_C(1000) * 1000 * 1000;

    for (size_t i = 0; i < ARRAY_SIZE(machines); i++) {
        g_autofree uint8_t *eeprom = make_eeprom_image(config, false);
        g_autofree char *eeprom_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *state = NULL;
        g_autofree char *netconsole = NULL;
        g_autofree char *migration_dir = NULL;
        g_autofree char *migration_socket = NULL;
        QTestState *source;
        QTestState *destination;

        write_temp_image("raspi4-netconsole-eeprom-XXXXXX", eeprom,
                         EEPROM_SIZE, &eeprom_path);
        command = g_strdup_printf(
            "-M %s,boot-mode=behavioral,eeprom-drive=pieeprom "
            "-drive if=none,id=pieeprom,format=raw,file=%s,"
            "file.locking=off -nic none",
            machines[i], eeprom_path);
        source = qtest_init(command);
        state = qom_get_string(source, "boot-state");
        netconsole = qom_get_string(source, "boot-netconsole-config");
        g_assert_cmpstr(state, ==, "netconsole-link-wait");
        g_assert_cmpstr(netconsole, ==,
                        "6665@169.254.1.1/,6666@/");
        g_assert_true(qom_get_bool(
                          source, "boot-netconsole-enabled"));
        g_assert_true(qom_get_bool(
                          source, "boot-netconsole-link-waiting"));
        g_assert_cmpuint(qom_get_uint64(
                             source, "boot-netconsole-remaining-ns"),
                         ==, 5 * second);

        qtest_clock_step(source, 2 * second);
        destination = migrate_to_new_qtest(
            source, command, &migration_dir, &migration_socket);
        g_assert_true(qom_get_bool(
                          destination, "boot-netconsole-link-waiting"));
        g_assert_cmpuint(qom_get_uint64(
                             destination,
                             "boot-netconsole-remaining-ns"),
                         ==, 3 * second);
        qtest_clock_step(destination, 3 * second - 1);
        state = qom_get_string(destination, "boot-state");
        g_assert_cmpstr(state, ==, "netconsole-link-wait");
        g_clear_pointer(&state, g_free);
        qtest_clock_step(destination, 1);
        state = qom_get_string(destination, "boot-state");
        g_assert_cmpstr(state, ==, "stopped");
        g_assert_false(qom_get_bool(
                           destination, "boot-netconsole-link-waiting"));
        g_assert_cmpuint(qom_get_uint64(
                             destination, "boot-elapsed-ms"), ==, 5000);

        qtest_system_reset(destination);
        g_assert_true(qom_get_bool(
                          destination, "boot-netconsole-link-waiting"));
        g_assert_cmpuint(qom_get_uint64(
                             destination,
                             "boot-netconsole-remaining-ns"),
                         ==, 5 * second);

        qtest_quit(source);
        qtest_quit(destination);
        unlink(migration_socket);
        rmdir(migration_dir);
        unlink(eeprom_path);
    }

    {
        static const char * const invalid[] = {
            "6665@169.254.1.1/6666@/",
            "6665@/dev,6666@/",
            "0@169.254.1.1/,6666@/",
            "65536@169.254.1.1/,6666@/",
            "6665@0.0.0.0/,6666@/",
            "6665@169.254.1.1/,6666@bad/",
            "6665@169.254.1.1/,6666@/bad",
            "6665@169.254.1.1/,6666@/00:11:22:33:44",
            "6665@169.254.1.1/dev,6666@/012345678901234",
        };

        for (size_t i = 0; i < ARRAY_SIZE(invalid); i++) {
            g_autofree char *case_config = g_strdup_printf(
                "BOOT_ORDER=0xe\nNETCONSOLE=%s\n", invalid[i]);
            g_autofree char *eeprom_path = NULL;
            g_autofree char *state = NULL;
            QTestState *qts = start_with_eeprom(
                case_config, false, &eeprom_path);

            state = qom_get_string(qts, "boot-state");
            g_assert_cmpstr(state, ==, "eeprom-invalid");
            g_assert_false(qom_get_bool(
                               qts, "boot-netconsole-enabled"));
            qtest_quit(qts);
            unlink(eeprom_path);
        }
    }

    {
        g_autofree uint8_t *eeprom = make_eeprom_image(config, false);
        g_autofree char *eeprom_path = NULL;
        g_autofree char *command = NULL;
        uint8_t packet[256];
        const uint8_t zero_mac[6] = { 0 };
        size_t packet_size;
        const uint8_t *ip;
        const uint8_t *udp;
        const uint8_t *payload;
        int sockets[2];
        QTestState *qts;

        write_temp_image("raspi4-netconsole-wire-XXXXXX", eeprom,
                         EEPROM_SIZE, &eeprom_path);
        g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets),
                        ==, 0);
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
            "-drive if=none,id=pieeprom,format=raw,file=%s "
            "-netdev socket,fd=%d,id=net0 "
            "-global bcm2711-genet.netdev=net0 "
            "-global bcm2711-genet.mac=52:54:00:12:34:56",
            eeprom_path, sockets[1]);
        qts = qtest_init(command);
        close(sockets[1]);

        packet_size = genet_socket_read_packet(
            sockets[0], packet, sizeof(packet));
        g_assert_cmpuint(packet_size, >, 42);
        g_assert_cmpmem(packet, 6, zero_mac, sizeof(zero_mac));
        g_assert_cmphex(lduw_be_p(packet + 12), ==, 0x0800);
        ip = packet + 14;
        udp = ip + 20;
        payload = udp + 8;
        g_assert_cmphex(ip[9], ==, 17);
        g_assert_cmphex((uint32_t)ldl_be_p(ip + 12), ==, 0xa9fe0101U);
        g_assert_cmphex((uint32_t)ldl_be_p(ip + 16), ==, UINT32_MAX);
        g_assert_cmpuint(lduw_be_p(udp), ==, 6665);
        g_assert_cmpuint(lduw_be_p(udp + 2), ==, 6666);
        g_assert_nonnull(g_strstr_len(
            (const char *)payload, packet + packet_size - payload,
            "RPI4-BOOT: netconsole enabled"));
        g_assert_cmpuint(qom_get_uint64(
                             qts, "boot-netconsole-packets"), >=, 2);
        g_assert_cmpuint(qom_get_uint64(
                             qts, "boot-netconsole-bytes"), >, 0);
        g_assert_false(qom_get_bool(
                           qts, "boot-netconsole-link-waiting"));

        qtest_quit(qts);
        close(sockets[0]);
        unlink(eeprom_path);
    }
}
























static void test_secure_network_boot_wire(void)
{
    static const char key_hash_hex[] =
        "dea09d2a3225f6e195b2aba975d991c19acef3534a55c0c7e7c88589957d3152";
    static const char * const filenames[] = { "boot.sig", "boot.img" };
    uint8_t key_hash[32];
    size_t image_size;
    g_autofree uint8_t *image = make_secure_inner_boot_image(&image_size);
    g_autofree char *signature = make_secure_boot_signature();
    const size_t sizes[] = { strlen(signature), image_size };

    test_decode_hex(key_hash_hex, key_hash, sizeof(key_hash));
    for (unsigned int tamper = 0; tamper < 2; tamper++) {
        g_autofree uint8_t *eeprom = make_signed_eeprom(
            false, false, false, true, TEST_EEPROM_DUPLICATE_NONE);
        g_autofree uint8_t *otp = make_secure_otp_image(key_hash);
        g_autofree uint8_t *test_signature = g_memdup2(
            signature, sizes[0]);
        g_autofree char *eeprom_path = NULL;
        g_autofree char *otp_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *state = NULL;
        g_autofree char *secure_status = NULL;
        const uint8_t *test_files[] = { test_signature, image };
        uint8_t client[576];
        uint8_t server[576];
        size_t server_size;
        int sockets[2];
        QTestState *qts;

        if (tamper) {
            test_signature[sizes[0] - 2] =
                test_signature[sizes[0] - 2] == '0' ? '1' : '0';
        }
        write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                         &eeprom_path);
        write_temp_image("raspi4-otp-XXXXXX", otp, OTP_SIZE, &otp_path);
        g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "otp-drive=piotp" TEST_BOOTSYS_MACHINE_OPTION
            ",network-boot-wire=on "
            "-drive if=none,id=pieeprom,format=raw,file=%s "
            "-drive if=none,id=piotp,format=raw,file=%s "
            "-netdev socket,fd=%d,id=net0 "
            "-global bcm2711-genet.netdev=net0 "
            "-global bcm2711-genet.mac=52:54:00:12:34:56",
            eeprom_path, otp_path, sockets[1]);
        qts = qtest_init(command);
        close(sockets[1]);

        genet_socket_read_packet(sockets[0], client, sizeof(client));
        g_assert_cmphex(lduw_be_p(client + 12), ==, 0x0806);
        g_assert_cmphex(ldl_be_p(client + 14 + 24), ==, 0x0a000263);
        server_size = test_make_arp_reply(server, client);
        genet_socket_send_packet(sockets[0], server, server_size);
        test_reject_optional_self_update(
            sockets[0], client, sizeof(client));
        test_transfer_tftp_files(
            sockets[0], client, sizeof(client), filenames, test_files,
            sizes, ARRAY_SIZE(filenames));
        qtest_clock_step(qts, INT64_C(500) * 1000 * 1000);

        state = qom_get_string(qts, "boot-state");
        secure_status = qom_get_string(qts, "secure-boot-status");
        g_assert_cmpstr(state, ==,
                        tamper ? "network-tftp-dally" :
                                 "arm-handoff-ready");
        g_assert_cmpstr(secure_status, ==,
                        tamper ? "rsa-signature-invalid" :
                                 "image-verified");
        qtest_quit(qts);
        close(sockets[0]);
        unlink(eeprom_path);
        unlink(otp_path);
    }
}

static void test_secure_http_boot_wire(void)
{
    static const char key_hash_hex[] =
        "dea09d2a3225f6e195b2aba975d991c19acef3534a55c0c7e7c88589957d3152";
    uint8_t key_hash[32];
    size_t image_size;
    g_autofree uint8_t *image = make_secure_inner_boot_image(&image_size);
    g_autofree char *signature = make_secure_boot_signature();
    g_autofree uint8_t *eeprom = make_signed_eeprom(
        false, false, false, 2, TEST_EEPROM_DUPLICATE_NONE);
    g_autofree uint8_t *otp = NULL;
    g_autofree char *eeprom_path = NULL;
    g_autofree char *otp_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *state = NULL;
    g_autofree char *secure_status = NULL;
    g_autofree char *http_host = NULL;
    g_autofree char *http_path = NULL;
    uint8_t client[1600];
    uint8_t server[1600];
    size_t client_size;
    size_t server_size;
    int sockets[2];
    QTestState *qts;

    test_decode_hex(key_hash_hex, key_hash, sizeof(key_hash));
    otp = make_secure_otp_image(key_hash);
    stl_le_p(otp + (BCM2711_OTP_SECURE_BOOT_FLAGS_ROW - 1) *
             sizeof(uint32_t),
             BCM2711_OTP_SECURE_BOOT_FLAGS_PRODUCTION);
    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    write_temp_image("raspi4-otp-XXXXXX", otp, OTP_SIZE, &otp_path);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "otp-drive=piotp" TEST_BOOTSYS_MACHINE_OPTION
        ",network-boot-wire=on "
        "-drive if=none,id=pieeprom,format=raw,file=%s "
        "-drive if=none,id=piotp,format=raw,file=%s "
        "-netdev socket,fd=%d,id=net0 "
        "-global bcm2711-genet.netdev=net0 "
        "-global bcm2711-genet.mac=52:54:00:12:34:56",
        eeprom_path, otp_path, sockets[1]);
    qts = qtest_init(command);
    close(sockets[1]);

    client_size = genet_socket_read_packet(
        sockets[0], client, sizeof(client));
    g_assert_cmpuint(test_dhcp_message_type(client, client_size), ==, 1);
    server_size = test_make_dhcp_reply(
        server, sizeof(server), client, 2);
    genet_socket_send_packet(sockets[0], server, server_size);
    client_size = genet_socket_read_packet(
        sockets[0], client, sizeof(client));
    g_assert_cmpuint(test_dhcp_message_type(client, client_size), ==, 3);
    server_size = test_make_dhcp_reply(
        server, sizeof(server), client, 5);
    genet_socket_send_packet(sockets[0], server, server_size);

    genet_socket_read_packet(sockets[0], client, sizeof(client));
    g_assert_cmphex(lduw_be_p(client + 12), ==, 0x0806);
    g_assert_cmphex(ldl_be_p(client + 14 + 24), ==, 0x0a000263);
    server_size = test_make_arp_reply(server, client);
    genet_socket_send_packet(sockets[0], server, server_size);
    test_transfer_http_file(
        qts, sockets[0], client, sizeof(client), "net_install/boot.sig",
        "10.0.2.99",
        (const uint8_t *)signature, strlen(signature), 0x10203040,
        true, true, false, false);
    test_transfer_http_file(
        qts, sockets[0], client, sizeof(client), "net_install/boot.img",
        "10.0.2.99",
        image, image_size, 0x50607080, false, false, true, true);

    state = qom_get_string(qts, "boot-state");
    secure_status = qom_get_string(qts, "secure-boot-status");
    http_host = qom_get_string(qts, "boot-http-host");
    http_path = qom_get_string(qts, "boot-http-path");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(secure_status, ==, "image-verified");
    g_assert_cmpuint(qom_get_uint32(
                         qts, "bootloader-signed"), ==, 13);
    g_assert_cmpuint(firmware_bootloader_u32(
                         qts, "signed"), ==, 13);
    g_assert_cmpstr(http_host, ==, "10.0.2.99");
    g_assert_cmpstr(http_path, ==, "net_install");
    g_assert_cmpuint(qom_get_uint32(qts, "boot-http-port"), ==, 80);
    assert_bootloader_boot_mode(qts, 7);
    qtest_quit(qts);
    close(sockets[0]);
    unlink(eeprom_path);
    unlink(otp_path);
}

static void test_secure_http_dns_boot_wire(void)
{
    static const char key_hash_hex[] =
        "bf8d278a4e92747095db55636efb4cd4c67b01e6796c269ea641469d063beb55";
    uint8_t key_hash[32];
    size_t image_size;
    g_autofree uint8_t *image = make_secure_inner_boot_image(&image_size);
    g_autofree char *signature = make_dns_secure_boot_signature();
    g_autofree uint8_t *eeprom = make_dns_signed_http_eeprom();
    g_autofree uint8_t *otp = NULL;
    g_autofree char *eeprom_path = NULL;
    g_autofree char *otp_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *state = NULL;
    g_autofree char *secure_status = NULL;
    g_autofree char *http_host = NULL;
    g_autofree char *dns_server = NULL;
    uint8_t client[1600];
    uint8_t server[1600];
    const uint8_t *ip;
    const uint8_t *udp;
    const uint8_t *dns;
    size_t client_size;
    size_t server_size;
    int sockets[2];
    QTestState *qts;

    test_decode_hex(key_hash_hex, key_hash, sizeof(key_hash));
    otp = make_secure_otp_image(key_hash);
    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    write_temp_image("raspi4-otp-XXXXXX", otp, OTP_SIZE, &otp_path);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "otp-drive=piotp" TEST_BOOTSYS_MACHINE_OPTION
        ",network-boot-wire=on "
        "-drive if=none,id=pieeprom,format=raw,file=%s "
        "-drive if=none,id=piotp,format=raw,file=%s "
        "-netdev socket,fd=%d,id=net0 "
        "-global bcm2711-genet.netdev=net0 "
        "-global bcm2711-genet.mac=52:54:00:12:34:56",
        eeprom_path, otp_path, sockets[1]);
    qts = qtest_init(command);
    close(sockets[1]);

    client_size = genet_socket_read_packet(sockets[0], client,
                                            sizeof(client));
    g_assert_cmpuint(test_dhcp_message_type(client, client_size), ==, 1);
    server_size = test_make_dhcp_reply(server, sizeof(server), client, 2);
    server_size = test_add_dhcp_ipv4_option(
        server, sizeof(server), 6, 0x0a000235);
    genet_socket_send_packet(sockets[0], server, server_size);
    client_size = genet_socket_read_packet(sockets[0], client,
                                            sizeof(client));
    g_assert_cmpuint(test_dhcp_message_type(client, client_size), ==, 3);
    server_size = test_make_dhcp_reply(server, sizeof(server), client, 5);
    server_size = test_add_dhcp_ipv4_option(
        server, sizeof(server), 6, 0x0a000235);
    genet_socket_send_packet(sockets[0], server, server_size);

    client_size = genet_socket_read_packet(sockets[0], client,
                                            sizeof(client));
    g_assert_cmphex(lduw_be_p(client + 12), ==, 0x0806);
    g_assert_cmphex(ldl_be_p(client + 14 + 24), ==, 0x0a000235);
    server_size = test_make_arp_reply(server, client);
    genet_socket_send_packet(sockets[0], server, server_size);
    client_size = genet_socket_read_packet(sockets[0], client,
                                            sizeof(client));
    ip = client + 14;
    udp = ip + (ip[0] & 0xf) * 4;
    dns = udp + 8;
    g_assert_cmphex(lduw_be_p(udp + 2), ==, 53);
    g_assert_cmpuint(dns[12], ==, 4);
    g_assert_cmpmem(dns + 13, 4, "boot", 4);
    g_assert_cmpuint(dns[17], ==, 4);
    g_assert_cmpmem(dns + 18, 4, "test", 4);
    g_assert_cmpuint(dns[22], ==, 0);
    server_size = test_make_dns_reply(
        server, sizeof(server), client, client_size, 0x0a000263);
    genet_socket_send_packet(sockets[0], server, server_size);

    genet_socket_read_packet(sockets[0], client, sizeof(client));
    g_assert_cmphex(lduw_be_p(client + 12), ==, 0x0806);
    g_assert_cmphex(ldl_be_p(client + 14 + 24), ==, 0x0a000263);
    server_size = test_make_arp_reply(server, client);
    genet_socket_send_packet(sockets[0], server, server_size);
    test_transfer_http_file(
        qts, sockets[0], client, sizeof(client), "net_install/boot.sig",
        "boot.test", (const uint8_t *)signature, strlen(signature),
        0x11223344, false, false, false, false);
    test_transfer_http_file(
        qts, sockets[0], client, sizeof(client), "net_install/boot.img",
        "boot.test", image, image_size, 0x55667788,
        false, false, false, true);

    state = qom_get_string(qts, "boot-state");
    secure_status = qom_get_string(qts, "secure-boot-status");
    http_host = qom_get_string(qts, "boot-http-host");
    dns_server = qom_get_string(qts, "boot-dns-server-ip");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(secure_status, ==, "image-verified");
    g_assert_cmpstr(http_host, ==, "boot.test");
    g_assert_cmpstr(dns_server, ==, "10.0.2.53");

    qtest_quit(qts);
    close(sockets[0]);
    unlink(eeprom_path);
    unlink(otp_path);
}

static void test_default_host_builtin_ca(void)
{
    static const char config[] =
        "BOOT_ORDER=0xf21\n"
        "NET_INSTALL_ENABLED=1\n";
    g_autofree uint8_t *eeprom = make_eeprom_image(config, false);
    g_autofree char *eeprom_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *tls_creds = NULL;
    uint8_t client[1600];
    uint8_t server[1600];
    const uint8_t *ip;
    const uint8_t *tcp;
    const uint8_t *payload;
    size_t client_size;
    size_t server_size;
    uint32_t client_sequence;
    int sockets[2];
    QTestState *qts;

    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "network-boot-wire=on,net-install-requested=on "
        "-drive if=none,id=pieeprom,format=raw,file=%s "
        "-netdev socket,fd=%d,id=net0 "
        "-global bcm2711-genet.netdev=net0 "
        "-global bcm2711-genet.mac=52:54:00:12:34:56",
        eeprom_path, sockets[1]);
    qts = qtest_init(command);
    close(sockets[1]);

    tls_creds = qom_get_string(qts, "http-tls-creds");
    g_assert_cmpstr(tls_creds, ==, "");
    client_size = genet_socket_read_packet(
        sockets[0], client, sizeof(client));
    g_assert_cmpuint(test_dhcp_message_type(client, client_size), ==, 1);
    server_size = test_make_dhcp_reply(
        server, sizeof(server), client, 2);
    server_size = test_add_dhcp_ipv4_option(
        server, sizeof(server), 6, 0x0a000235);
    genet_socket_send_packet(sockets[0], server, server_size);
    client_size = genet_socket_read_packet(
        sockets[0], client, sizeof(client));
    g_assert_cmpuint(test_dhcp_message_type(client, client_size), ==, 3);
    server_size = test_make_dhcp_reply(
        server, sizeof(server), client, 5);
    server_size = test_add_dhcp_ipv4_option(
        server, sizeof(server), 6, 0x0a000235);
    genet_socket_send_packet(sockets[0], server, server_size);

    genet_socket_read_packet(sockets[0], client, sizeof(client));
    server_size = test_make_arp_reply(server, client);
    genet_socket_send_packet(sockets[0], server, server_size);
    client_size = genet_socket_read_packet(
        sockets[0], client, sizeof(client));
    server_size = test_make_dns_reply(
        server, sizeof(server), client, client_size, 0x0a000263);
    genet_socket_send_packet(sockets[0], server, server_size);

    genet_socket_read_packet(sockets[0], client, sizeof(client));
    server_size = test_make_arp_reply(server, client);
    genet_socket_send_packet(sockets[0], server, server_size);
    client_size = genet_socket_read_packet(
        sockets[0], client, sizeof(client));
    ip = client + 14;
    tcp = ip + (ip[0] & 0xf) * 4;
    g_assert_cmphex(lduw_be_p(tcp + 2), ==, 443);
    g_assert_true(tcp[13] & 0x02);
    client_sequence = ldl_be_p(tcp + 4);
    server_size = test_make_tcp_reply(
        server, sizeof(server), client, 0x10203040,
        client_sequence + 1, 0x12, NULL, 0);
    genet_socket_send_packet(sockets[0], server, server_size);

    client_size = genet_socket_read_packet(
        sockets[0], client, sizeof(client));
    ip = client + 14;
    tcp = ip + (ip[0] & 0xf) * 4;
    payload = tcp + (tcp[12] >> 4) * 4;
    g_assert_cmpuint(client + client_size - payload, >, 5);
    g_assert_cmphex(payload[0], ==, 0x16);
    g_assert_cmphex(payload[1], ==, 0x03);

    qtest_quit(qts);
    close(sockets[0]);
    unlink(eeprom_path);
}

#ifdef CONFIG_TASN1






static void test_default_host_https_handshake(void)
{
    static const char config[] =
        "BOOT_ORDER=0xf21\n"
        "NET_INSTALL_ENABLED=1\n"
        "NET_INSTALL_AT_POWER_ON=1\n";
    g_autofree uint8_t *eeprom = make_eeprom_image(config, false);
    g_autofree char *eeprom_path = NULL;
    g_autofree char *tls_dir = g_dir_make_tmp(
        "raspi4-https-XXXXXX", NULL);
    g_autofree char *key_path = g_strdup_printf("%s/key.pem", tls_dir);
    g_autofree char *ca_path = g_strdup_printf(
        "%s/ca-cert.pem", tls_dir);
    g_autofree char *server_cert_path = g_strdup_printf(
        "%s/server-cert.pem", tls_dir);
    g_autofree char *server_key_path = g_strdup_printf(
        "%s/server-key.pem", tls_dir);
    g_autofree char *command = NULL;
    g_autofree char *http_host = NULL;
    g_autofree uint8_t *first_flight = NULL;
    g_autoptr(GByteArray) tls_rx = g_byte_array_new();
    g_autoptr(GByteArray) tls_tx = g_byte_array_new();
    TestTlsTransport transport = {
        .rx = tls_rx,
        .tx = tls_tx,
    };
    gnutls_certificate_credentials_t server_creds;
    gnutls_session_t server_session;
    uint8_t client[1600];
    uint8_t server[1600];
    const uint8_t *ip;
    const uint8_t *tcp;
    const uint8_t *payload;
    size_t client_size;
    size_t first_flight_size;
    size_t replay_size;
    size_t server_size;
    uint32_t client_sequence;
    uint32_t server_sequence = 0x10203041;
    bool server_handshake_complete = false;
    bool request_received = false;
    int sockets[2];
    QTestState *qts;

    test_tls_init(key_path);
    TLS_ROOT_REQ_SIMPLE(cacertreq, ca_path);
    g_assert_cmpint(link(key_path, server_key_path), ==, 0);
    TLS_CERT_REQ_SIMPLE_SERVER(
        servercertreq, cacertreq, server_cert_path,
        "fw-download-alias1.raspberrypi.com", NULL);
    test_tls_deinit_cert(&servercertreq);
    test_tls_deinit_cert(&cacertreq);
    g_assert_cmpint(
        gnutls_certificate_allocate_credentials(&server_creds), ==, 0);
    g_assert_cmpint(
        gnutls_certificate_set_x509_key_file(
            server_creds, server_cert_path, server_key_path,
            GNUTLS_X509_FMT_PEM), ==, 0);
    g_assert_cmpint(
        gnutls_init(&server_session, GNUTLS_SERVER | GNUTLS_NONBLOCK), ==, 0);
    g_assert_cmpint(gnutls_set_default_priority(server_session), ==, 0);
    g_assert_cmpint(
        gnutls_credentials_set(
            server_session, GNUTLS_CRD_CERTIFICATE, server_creds), ==, 0);
    gnutls_transport_set_ptr(server_session, &transport);
    gnutls_transport_set_pull_function(server_session, test_tls_pull);
    gnutls_transport_set_push_function(server_session, test_tls_push);
    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    command = g_strdup_printf(
        "-object tls-creds-x509,id=pihttps,endpoint=client,"
        "dir=%s,verify-peer=on "
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "network-boot-wire=on,http-tls-creds=pihttps,"
        "net-install-requested=on "
        "-drive if=none,id=pieeprom,format=raw,file=%s "
        "-netdev socket,fd=%d,id=net0 "
        "-global bcm2711-genet.netdev=net0 "
        "-global bcm2711-genet.mac=52:54:00:12:34:56",
        tls_dir, eeprom_path, sockets[1]);
    qts = qtest_init(command);
    close(sockets[1]);

    client_size = genet_socket_read_packet(
        sockets[0], client, sizeof(client));
    g_assert_cmpuint(test_dhcp_message_type(client, client_size), ==, 1);
    server_size = test_make_dhcp_reply(
        server, sizeof(server), client, 2);
    server_size = test_add_dhcp_ipv4_option(
        server, sizeof(server), 6, 0x0a000235);
    genet_socket_send_packet(sockets[0], server, server_size);
    client_size = genet_socket_read_packet(
        sockets[0], client, sizeof(client));
    g_assert_cmpuint(test_dhcp_message_type(client, client_size), ==, 3);
    server_size = test_make_dhcp_reply(
        server, sizeof(server), client, 5);
    server_size = test_add_dhcp_ipv4_option(
        server, sizeof(server), 6, 0x0a000235);
    genet_socket_send_packet(sockets[0], server, server_size);

    genet_socket_read_packet(sockets[0], client, sizeof(client));
    server_size = test_make_arp_reply(server, client);
    genet_socket_send_packet(sockets[0], server, server_size);
    client_size = genet_socket_read_packet(
        sockets[0], client, sizeof(client));
    server_size = test_make_dns_reply(
        server, sizeof(server), client, client_size, 0x0a000263);
    genet_socket_send_packet(sockets[0], server, server_size);

    genet_socket_read_packet(sockets[0], client, sizeof(client));
    server_size = test_make_arp_reply(server, client);
    genet_socket_send_packet(sockets[0], server, server_size);
    client_size = genet_socket_read_packet(
        sockets[0], client, sizeof(client));
    ip = client + 14;
    tcp = ip + (ip[0] & 0xf) * 4;
    g_assert_cmphex(lduw_be_p(tcp + 2), ==, 443);
    g_assert_true(tcp[13] & 0x02);
    client_sequence = ldl_be_p(tcp + 4);
    server_size = test_make_tcp_reply(
        server, sizeof(server), client, 0x10203040,
        client_sequence + 1, 0x12, NULL, 0);
    genet_socket_send_packet(sockets[0], server, server_size);

    client_size = genet_socket_read_packet(
        sockets[0], client, sizeof(client));
    ip = client + 14;
    tcp = ip + (ip[0] & 0xf) * 4;
    payload = tcp + (tcp[12] >> 4) * 4;
    g_assert_cmpuint(client + client_size - payload, >, 5);
    first_flight_size = client + client_size - payload;
    first_flight = g_memdup2(payload, first_flight_size);
    g_assert_cmphex(payload[0], ==, 0x16);
    g_assert_cmphex(payload[1], ==, 0x03);
    client_sequence = ldl_be_p(tcp + 4);

    qtest_clock_step(qts, INT64_C(500) * 1000 * 1000);
    client_size = genet_socket_read_packet(
        sockets[0], client, sizeof(client));
    ip = client + 14;
    tcp = ip + (ip[0] & 0xf) * 4;
    payload = tcp + (tcp[12] >> 4) * 4;
    replay_size = client + client_size - payload;
    g_assert_cmphex(ldl_be_p(tcp + 4), ==, client_sequence);
    g_assert_cmpmem(payload, replay_size,
                    first_flight, first_flight_size);
    g_assert_cmpuint(
        qom_get_uint64(qts, "boot-tftp-retransmit-count"), ==, 1);

    test_tls_feed_client_packet(
        sockets[0], &transport, client, client_size,
        server, sizeof(server), &server_sequence, &client_sequence);
    for (unsigned int attempt = 0;
         attempt < 20 && !request_received; attempt++) {
        int ret;

        if (!server_handshake_complete) {
            ret = gnutls_handshake(server_session);
            g_assert_true(ret == 0 || ret == GNUTLS_E_AGAIN ||
                          ret == GNUTLS_E_INTERRUPTED);
            server_handshake_complete = ret == 0;
        }
        if (transport.tx->len) {
            test_tls_send_server_flight(
                sockets[0], &transport, client, sizeof(client),
                server, sizeof(server), &server_sequence, &client_sequence);
        }
        test_tls_drain_client(
            sockets[0], &transport, client, sizeof(client),
            server, sizeof(server), &server_sequence, &client_sequence);
        if (server_handshake_complete) {
            uint8_t request[1024];
            ssize_t received = gnutls_record_recv(
                server_session, request, sizeof(request));

            g_assert_true(received > 0 || received == GNUTLS_E_AGAIN ||
                          received == GNUTLS_E_INTERRUPTED);
            if (received > 0) {
                g_assert_nonnull(g_strstr_len(
                    (const char *)request, received,
                    "GET /net_install/boot.sig HTTP/1.1\r\n"));
                g_assert_nonnull(g_strstr_len(
                    (const char *)request, received,
                    "Host: fw-download-alias1.raspberrypi.com\r\n"));
                request_received = true;
            }
        }
    }
    g_assert_true(server_handshake_complete);
    g_assert_true(request_received);
    {
        static const char body[] = "invalid-signature\n";
        g_autofree char *response = g_strdup_printf(
            "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n"
            "Connection: close\r\n\r\n%s",
            strlen(body), body);
        size_t response_size = strlen(response);

        g_assert_cmpint(
            gnutls_record_send(
                server_session, response, response_size), ==,
            response_size);
        test_tls_send_server_flight(
            sockets[0], &transport, client, sizeof(client),
            server, sizeof(server), &server_sequence, &client_sequence);
        test_tls_drain_client(
            sockets[0], &transport, client, sizeof(client),
            server, sizeof(server), &server_sequence, &client_sequence);
        for (unsigned int attempt = 0;
             attempt < 5 && !transport.saw_syn; attempt++) {
            test_tls_drain_client(
                sockets[0], &transport, client, sizeof(client),
                server, sizeof(server), &server_sequence,
                &client_sequence);
        }
        g_assert_true(transport.saw_fin);
        g_assert_true(transport.saw_syn);
    }
    http_host = qom_get_string(qts, "boot-http-host");
    g_assert_cmpstr(http_host, ==,
                    "fw-download-alias1.raspberrypi.com");
    g_assert_cmpuint(qom_get_uint32(qts, "boot-http-port"), ==, 443);
    g_assert_true(qom_get_bool(qts, "boot-net-install-enabled"));
    g_assert_true(qom_get_bool(qts, "boot-net-install-at-power-on"));

    qtest_quit(qts);
    gnutls_deinit(server_session);
    gnutls_certificate_free_credentials(server_creds);
    close(sockets[0]);
    unlink(eeprom_path);
    unlink(server_cert_path);
    unlink(server_key_path);
    unlink(ca_path);
    unlink(key_path);
    test_tls_cleanup(key_path);
    rmdir(tls_dir);
}
#endif

static void test_secure_http_boot_wire_migration(void)
{
    static const char key_hash_hex[] =
        "dea09d2a3225f6e195b2aba975d991c19acef3534a55c0c7e7c88589957d3152";
    enum {
        first_body_size = 32,
        first_gap_size = 32,
        middle_size = 32,
        second_gap_size = 32,
    };
    uint8_t key_hash[32];
    size_t image_size;
    g_autofree uint8_t *image = make_secure_inner_boot_image(&image_size);
    g_autofree char *signature = make_secure_boot_signature();
    g_autofree uint8_t *eeprom = make_signed_eeprom(
        false, false, false, 2, TEST_EEPROM_DUPLICATE_NONE);
    g_autofree uint8_t *otp = NULL;
    g_autofree char *eeprom_path = NULL;
    g_autofree char *otp_path = NULL;
    g_autofree char *source_command = NULL;
    g_autofree char *destination_command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_path = NULL;
    g_autofree char *migration_uri = NULL;
    g_autofree char *header = NULL;
    g_autofree char *state = NULL;
    g_autofree char *secure_status = NULL;
    uint8_t client[1600];
    uint8_t request[1600];
    uint8_t server[1600];
    uint8_t first_payload[1024];
    const uint8_t *ip;
    const uint8_t *tcp;
    const uint8_t *request_payload;
    size_t client_size;
    size_t request_size;
    size_t server_size;
    size_t header_size;
    size_t signature_size = strlen(signature);
    size_t tail_size = signature_size - first_body_size -
                       first_gap_size - middle_size - second_gap_size;
    uint32_t client_sequence;
    uint32_t server_sequence = 0x10203040;
    uint32_t server_next;
    int source_sockets[2];
    int destination_sockets[2];
    QTestState *source;
    QTestState *destination;

    g_assert_cmpuint(signature_size, >,
                     first_body_size + first_gap_size + middle_size +
                     second_gap_size);
    test_decode_hex(key_hash_hex, key_hash, sizeof(key_hash));
    otp = make_secure_otp_image(key_hash);
    stl_le_p(otp + (BCM2711_OTP_SECURE_BOOT_FLAGS_ROW - 1) *
             sizeof(uint32_t),
             BCM2711_OTP_SECURE_BOOT_FLAGS_PRODUCTION);
    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    write_temp_image("raspi4-otp-XXXXXX", otp, OTP_SIZE, &otp_path);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, source_sockets), ==,
                    0);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0,
                               destination_sockets), ==, 0);
    source_command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "otp-drive=piotp" TEST_BOOTSYS_MACHINE_OPTION
        ",network-boot-wire=on "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=none,id=piotp,format=raw,file=%s,file.locking=off "
        "-netdev socket,fd=%d,id=net0 "
        "-global bcm2711-genet.netdev=net0 "
        "-global bcm2711-genet.mac=52:54:00:12:34:56",
        eeprom_path, otp_path, source_sockets[1]);
    source = qtest_init(source_command);
    close(source_sockets[1]);

    client_size = genet_socket_read_packet(
        source_sockets[0], client, sizeof(client));
    g_assert_cmpuint(test_dhcp_message_type(client, client_size), ==, 1);
    server_size = test_make_dhcp_reply(
        server, sizeof(server), client, 2);
    genet_socket_send_packet(source_sockets[0], server, server_size);
    client_size = genet_socket_read_packet(
        source_sockets[0], client, sizeof(client));
    g_assert_cmpuint(test_dhcp_message_type(client, client_size), ==, 3);
    server_size = test_make_dhcp_reply(
        server, sizeof(server), client, 5);
    genet_socket_send_packet(source_sockets[0], server, server_size);
    genet_socket_read_packet(source_sockets[0], client, sizeof(client));
    server_size = test_make_arp_reply(server, client);
    genet_socket_send_packet(source_sockets[0], server, server_size);

    client_size = genet_socket_read_packet(
        source_sockets[0], client, sizeof(client));
    ip = client + 14;
    tcp = ip + (ip[0] & 0xf) * 4;
    g_assert_true(tcp[13] & 0x02);
    client_sequence = ldl_be_p(tcp + 4);
    server_size = test_make_tcp_reply(
        server, sizeof(server), client, server_sequence,
        client_sequence + 1, 0x12, NULL, 0);
    genet_socket_send_packet(source_sockets[0], server, server_size);

    client_size = genet_socket_read_packet(
        source_sockets[0], request, sizeof(request));
    ip = request + 14;
    tcp = ip + (ip[0] & 0xf) * 4;
    request_payload = tcp + (tcp[12] >> 4) * 4;
    request_size = (request + client_size) - request_payload;
    g_assert_nonnull(g_strstr_len(
        (const char *)request_payload, request_size,
        "net_install/boot.sig"));
    client_sequence = ldl_be_p(tcp + 4) + request_size;

    header = g_strdup_printf(
        "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Connection: close\r\n\r\n", signature_size);
    header_size = strlen(header);
    g_assert_cmpuint(header_size + first_body_size, <=,
                     sizeof(first_payload));
    memcpy(first_payload, header, header_size);
    memcpy(first_payload + header_size, signature, first_body_size);
    server_size = test_make_tcp_reply(
        server, sizeof(server), request, server_sequence + 1,
        client_sequence, 0x18, first_payload,
        header_size + first_body_size);
    genet_socket_send_packet(source_sockets[0], server, server_size);
    genet_socket_read_packet(source_sockets[0], client, sizeof(client));
    server_next = server_sequence + 1 + header_size + first_body_size;

    server_size = test_make_tcp_reply(
        server, sizeof(server), request,
        server_next + first_gap_size + middle_size + second_gap_size,
        client_sequence, 0x19,
        (const uint8_t *)signature + first_body_size + first_gap_size +
            middle_size + second_gap_size,
        tail_size);
    genet_socket_send_packet(source_sockets[0], server, server_size);
    genet_socket_read_packet(source_sockets[0], client, sizeof(client));
    ip = client + 14;
    tcp = ip + (ip[0] & 0xf) * 4;
    g_assert_cmphex(ldl_be_p(tcp + 8), ==, server_next);

    server_size = test_make_tcp_reply(
        server, sizeof(server), request, server_next + first_gap_size,
        client_sequence, 0x18,
        (const uint8_t *)signature + first_body_size + first_gap_size,
        middle_size);
    genet_socket_send_packet(source_sockets[0], server, server_size);
    genet_socket_read_packet(source_sockets[0], client, sizeof(client));
    ip = client + 14;
    tcp = ip + (ip[0] & 0xf) * 4;
    g_assert_cmphex(ldl_be_p(tcp + 8), ==, server_next);
    qtest_clock_step(source, INT64_C(300) * 1000 * 1000);

    migration_dir = g_dir_make_tmp("raspi4-http-migration-XXXXXX", NULL);
    g_assert_nonnull(migration_dir);
    migration_path = g_build_filename(migration_dir, "stream.sock", NULL);
    migration_uri = g_strdup_printf("unix:%s", migration_path);
    destination_command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "otp-drive=piotp" TEST_BOOTSYS_MACHINE_OPTION
        ",network-boot-wire=on "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=none,id=piotp,format=raw,file=%s,file.locking=off "
        "-netdev socket,fd=%d,id=net0 "
        "-global bcm2711-genet.netdev=net0 "
        "-global bcm2711-genet.mac=52:54:00:12:34:56 -incoming %s",
        eeprom_path, otp_path, destination_sockets[1], migration_uri);
    destination = qtest_init(destination_command);
    close(destination_sockets[1]);
    qtest_qmp_assert_success(
        source, "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }",
        migration_uri);
    wait_for_migration_complete(source);
    wait_for_migration_complete(destination);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "network-http-response");
    {
        g_autofree char *bootsys_hash =
            qom_get_string(destination, "bootsys-sha256");

        g_assert_cmpstr(bootsys_hash, ==, TEST_BOOTSYS_SHA256);
        g_assert_cmpuint(
            qom_get_uint32(destination, "bootsys-key-index"), ==, 1);
        g_assert_cmpuint(
            qom_get_uint32(destination, "otp-secure-boot-flags"), ==,
            BCM2711_OTP_SECURE_BOOT_FLAGS_PRODUCTION);
    }
    g_assert_cmpuint(qom_get_uint64(
                         destination, "boot-tftp-retransmit-count"),
                     ==, 0);

    qtest_clock_step(destination, INT64_C(200) * 1000 * 1000);
    for (unsigned int attempt = 0; attempt < 3; attempt++) {
        client_size = genet_socket_read_packet(
            destination_sockets[0], client, sizeof(client));
        if (lduw_be_p(client + 12) == 0x0800 &&
            client[14 + 9] == 6) {
            break;
        }
    }
    ip = client + 14;
    tcp = ip + (ip[0] & 0xf) * 4;
    g_assert_cmphex(tcp[13], ==, 0x10);
    g_assert_cmphex(ldl_be_p(tcp + 8), ==, server_next);
    g_assert_cmpuint(qom_get_uint64(
                         destination, "boot-tftp-retransmit-count"),
                     ==, 1);

    server_size = test_make_tcp_reply(
        server, sizeof(server), request, server_next, client_sequence,
        0x18, (const uint8_t *)signature + first_body_size,
        first_gap_size);
    genet_socket_send_packet(destination_sockets[0], server, server_size);
    genet_socket_read_packet(destination_sockets[0], client, sizeof(client));
    ip = client + 14;
    tcp = ip + (ip[0] & 0xf) * 4;
    g_assert_cmphex(ldl_be_p(tcp + 8), ==,
                    server_next + first_gap_size);
    genet_socket_read_packet(destination_sockets[0], client, sizeof(client));
    ip = client + 14;
    tcp = ip + (ip[0] & 0xf) * 4;
    server_next += first_gap_size + middle_size;
    g_assert_cmphex(ldl_be_p(tcp + 8), ==, server_next);

    server_size = test_make_tcp_reply(
        server, sizeof(server), request, server_next, client_sequence,
        0x18,
        (const uint8_t *)signature + first_body_size + first_gap_size +
            middle_size,
        second_gap_size);
    genet_socket_send_packet(destination_sockets[0], server, server_size);
    genet_socket_read_packet(destination_sockets[0], client, sizeof(client));
    ip = client + 14;
    tcp = ip + (ip[0] & 0xf) * 4;
    g_assert_cmphex(ldl_be_p(tcp + 8), ==,
                    server_next + second_gap_size);
    genet_socket_read_packet(destination_sockets[0], client, sizeof(client));
    ip = client + 14;
    tcp = ip + (ip[0] & 0xf) * 4;
    server_next += second_gap_size + tail_size;
    g_assert_cmphex(ldl_be_p(tcp + 8), ==, server_next);
    genet_socket_read_packet(destination_sockets[0], client, sizeof(client));
    ip = client + 14;
    tcp = ip + (ip[0] & 0xf) * 4;
    g_assert_cmphex(ldl_be_p(tcp + 8), ==, server_next + 1);
    genet_socket_read_packet(destination_sockets[0], client, sizeof(client));
    ip = client + 14;
    tcp = ip + (ip[0] & 0xf) * 4;
    g_assert_cmphex(tcp[13], ==, 0x11);
    g_assert_cmphex(ldl_be_p(tcp + 8), ==, server_next + 1);
    test_transfer_http_file(
        destination, destination_sockets[0], client, sizeof(client),
        "net_install/boot.img", "10.0.2.99", image, image_size, 0x50607080,
        false, false, false, false);

    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    secure_status = qom_get_string(destination, "secure-boot-status");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(secure_status, ==, "image-verified");
    g_assert_cmpuint(qom_get_uint32(
                         destination, "bootloader-signed"), ==, 13);
    g_assert_cmpuint(firmware_bootloader_u32(
                         destination, "signed"), ==, 13);
    {
        g_autofree char *dependencies_hash =
            qom_get_string(destination, "bootsys-dependencies-sha256");

        g_assert_cmpstr(dependencies_hash, ==,
                        TEST_DEPENDENCIES_SHA256);
        g_assert_cmpuint(
            qom_get_uint32(destination, "bootsys-dependency-count"), ==, 3);
    }

    qtest_quit(source);
    qtest_quit(destination);
    close(source_sockets[0]);
    close(destination_sockets[0]);
    unlink(eeprom_path);
    unlink(otp_path);
    unlink(migration_path);
    rmdir(migration_dir);
}


static void test_secure_http_error_fallback(void)
{
    static const uint8_t not_found[] =
        "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
    static const uint8_t redirect[] =
        "HTTP/1.1 302 Found\r\n"
        "Location: http://other.test/net_install/boot.sig\r\n"
        "Content-Length: 0\r\n\r\n";
    static const uint8_t truncated[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 512\r\n\r\nshort";

    test_secure_http_error_case(
        not_found, sizeof(not_found) - 1, 0x18,
        "network-http-header-invalid");
    test_secure_http_error_case(
        redirect, sizeof(redirect) - 1, 0x18,
        "network-http-redirect-rejected");
    test_secure_http_error_case(
        truncated, sizeof(truncated) - 1, 0x19,
        "network-http-truncated");
}




static void test_network_boot_dhcp_wire(void)
{
    enum { firmware_size = 1025 };
    g_autofree uint8_t *firmware = g_malloc(firmware_size);
    g_autofree char *expected_hash = NULL;
    g_autofree char *eeprom_path = NULL;
    g_autofree char *network_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *state = NULL;
    g_autofree char *hash = NULL;
    g_autofree char *firmware_file = NULL;
    g_autofree char *os_prefix = NULL;
    g_autofree char *tftp_ip = NULL;
    uint8_t client[576];
    uint8_t server[576];
    uint8_t expected_machine_id[17] = { 0 };
    uint8_t option_length;
    const uint8_t *option;
    size_t client_size;
    size_t server_size;
    unsigned int tftp_blocks;
    uint32_t request_xid;
    int sockets[2];
    QTestState *qts;

    for (size_t i = 0; i < firmware_size; i++) {
        firmware[i] = i * 37 + 11;
    }
    expected_hash = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, firmware, firmware_size);
    qts = start_with_eeprom_and_network(
        "BOOT_ORDER=0xe2\nDHCP_TIMEOUT=15000\nDHCP_REQ_TIMEOUT=1000\n"
        "TFTP_IP=10.0.2.99\n",
        "START4  ELF",
        firmware, firmware_size, &eeprom_path, &network_path);

    qtest_quit(qts);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-netdev socket,fd=%d,id=net0 "
        "-global bcm2711-genet.netdev=net0 "
        "-global bcm2711-genet.mac=52:54:00:12:34:56",
        eeprom_path, sockets[1]);
    qts = qtest_init(command);
    close(sockets[1]);
    g_assert_true(qom_get_bool(qts, "network-boot-wire"));

    client_size = genet_socket_read_packet(sockets[0], client,
                                            sizeof(client));
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "network-dhcp-wait");
    g_assert_cmpuint(test_dhcp_message_type(client, client_size), ==, 1);
    g_assert_cmphex(lduw_be_p(client + 12), ==, 0x0800);
    g_assert_cmphex(lduw_be_p(client + 34), ==, 68);
    g_assert_cmphex(lduw_be_p(client + 36), ==, 67);
    g_assert_cmphex(test_ip_checksum(client + 14, 20), ==, 0);
    stl_le_p(expected_machine_id + 1, 0x34695052);
    stl_le_p(expected_machine_id + 5,
             qom_get_uint32(qts, "board-revision"));
    memcpy(expected_machine_id + 9,
           (const uint8_t[]) { 0x00, 0x12, 0x34, 0x56 }, 4);
    option = test_dhcp_option(client, client_size, 97, &option_length);
    g_assert_nonnull(option);
    g_assert_cmpuint(option_length, ==, sizeof(expected_machine_id));
    g_assert_cmpmem(option, option_length, expected_machine_id,
                    sizeof(expected_machine_id));
    g_assert_cmphex(qom_get_uint32(qts, "boot-dhcp-option97"), ==,
                    0x34695052);
    qtest_clock_step(qts, INT64_C(1000) * 1000 * 1000);
    client_size = genet_socket_read_packet(sockets[0], client,
                                            sizeof(client));
    g_assert_cmpuint(test_dhcp_message_type(client, client_size), ==, 1);
    server_size = test_make_dhcp_reply(server, sizeof(server), client, 2);
    server_size = test_add_conflicting_overloaded_server(
        server, sizeof(server), 0x0a000203);
    genet_socket_send_packet(sockets[0], server, server_size);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "network-dhcp-wait");
    server_size = test_make_dhcp_reply(server, sizeof(server), client, 2);
    server_size = test_add_overloaded_string_option(
        server, sizeof(server), 67, "different");
    genet_socket_send_packet(sockets[0], server, server_size);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "network-dhcp-wait");
    server_size = test_make_dhcp_reply(server, sizeof(server), client, 2);
    genet_socket_send_packet(sockets[0], server, server_size);

    client_size = genet_socket_read_packet(sockets[0], client,
                                            sizeof(client));
    g_assert_cmpuint(test_dhcp_message_type(client, client_size), ==, 3);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "network-dhcp-request-wait");
    request_xid = ldl_be_p(client + 14 + 20 + 8 + 4);
    qtest_clock_step(qts, INT64_C(1000) * 1000 * 1000);
    client_size = genet_socket_read_packet(sockets[0], client,
                                            sizeof(client));
    g_assert_cmpuint(test_dhcp_message_type(client, client_size), ==, 3);
    server_size = test_make_dhcp_reply(server, sizeof(server), client, 6);
    genet_socket_send_packet(sockets[0], server, server_size);

    client_size = genet_socket_read_packet(sockets[0], client,
                                            sizeof(client));
    g_assert_cmpuint(test_dhcp_message_type(client, client_size), ==, 1);
    g_assert_cmphex(ldl_be_p(client + 14 + 20 + 8 + 4), ==,
                    request_xid + 1);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "network-dhcp-nak-restart");
    server_size = test_make_dhcp_reply(server, sizeof(server), client, 2);
    genet_socket_send_packet(sockets[0], server, server_size);
    client_size = genet_socket_read_packet(sockets[0], client,
                                            sizeof(client));
    g_assert_cmpuint(test_dhcp_message_type(client, client_size), ==, 3);
    server_size = test_make_dhcp_reply(server, sizeof(server), client, 5);
    test_set_dhcp_server_addresses(
        server, server_size, 0x0a000203, 0x0a000202);
    genet_socket_send_packet(sockets[0], server, server_size);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "network-dhcp-request-wait");
    server_size = test_make_dhcp_reply(server, sizeof(server), client, 5);
    genet_socket_send_packet(sockets[0], server, server_size);
    tftp_blocks = test_complete_response_tftp(
        qts, sockets[0], client, sizeof(client), firmware, firmware_size,
        0x0a000263, true, false);

    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    hash = qom_get_string(qts, "firmware-sha256");
    firmware_file = qom_get_string(qts, "firmware-file");
    os_prefix = qom_get_string(qts, "firmware-os-prefix");
    tftp_ip = qom_get_string(qts, "boot-tftp-ip");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(hash, ==, expected_hash);
    g_assert_cmpstr(firmware_file, ==, "start.elf");
    g_assert_cmpstr(os_prefix, ==, "");
    g_assert_cmpstr(tftp_ip, ==, "10.0.2.99");
    g_assert_true(qom_get_bool(qts, "firmware-config-present"));
    g_assert_cmpuint(qom_get_uint32(qts, "firmware-config-lines"), ==, 5);
    g_assert_cmpuint(qom_get_uint32(qts, "firmware-config-includes"), ==, 1);
    g_assert_cmpuint(qom_get_uint32(qts, "firmware-overlay-applied"), ==, 1);
    g_assert_cmpuint(qom_get_uint32(qts, "firmware-initramfs-size"), ==,
                     strlen(TEST_NETWORK_INITRAMFS_A) + 1 +
                     strlen(TEST_NETWORK_INITRAMFS_B) + 1);
    g_assert_cmpuint(qom_get_uint64(qts, "boot-elapsed-ms"), ==, 500);
    g_assert_cmpuint(qom_get_uint64(qts, "boot-tftp-retransmit-count"),
                     ==, 2);
    g_assert_cmpuint(qom_get_uint64(qts, "boot-dhcp-retransmit-count"),
                     ==, 2);
    g_assert_cmpuint(qtest_readl(qts, BCM2711_GENET_BASE +
                                 GENET_UMAC_MIB_TX_PKT), ==,
                     6 + 1 + 12 + tftp_blocks + 5 + 1);
    g_assert_cmpuint(qtest_readl(qts, BCM2711_GENET_BASE +
                                 GENET_UMAC_MIB_RX_PKT), ==,
                     5 + 1 + 3 + tftp_blocks + 2 + 2 + 1);
    qtest_quit(qts);
    close(sockets[0]);
    unlink(eeprom_path);
    unlink(network_path);
}

static void test_network_boot_dns_wire(void)
{
    static const uint8_t firmware[] = "QEMU DNS-resolved TFTP firmware";
    g_autofree char *eeprom_path = NULL;
    g_autofree char *network_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *state = NULL;
    g_autofree char *dns_server = NULL;
    g_autofree char *tftp_hostname = NULL;
    g_autofree char *network_server = NULL;
    uint8_t client[576];
    uint8_t server[576];
    const uint8_t *ip;
    const uint8_t *udp;
    const uint8_t *dns;
    size_t client_size;
    size_t server_size;
    int sockets[2];
    QTestState *qts = start_with_eeprom_and_network(
        "BOOT_ORDER=0xe2\nDHCP_TIMEOUT=15000\nDHCP_REQ_TIMEOUT=1000\n",
        "START4  ELF", firmware, sizeof(firmware),
        &eeprom_path, &network_path);

    qtest_quit(qts);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "network-boot-wire=on "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-netdev socket,fd=%d,id=net0 "
        "-global bcm2711-genet.netdev=net0 "
        "-global bcm2711-genet.mac=52:54:00:12:34:56",
        eeprom_path, sockets[1]);
    qts = qtest_init(command);
    close(sockets[1]);

    client_size = genet_socket_read_packet(sockets[0], client,
                                            sizeof(client));
    g_assert_cmpuint(test_dhcp_message_type(client, client_size), ==, 1);
    server_size = test_make_dhcp_reply(server, sizeof(server), client, 2);
    test_set_dhcp_server_addresses(server, server_size, 0x0a000202, 0);
    server_size = test_add_dhcp_ipv4_option(
        server, sizeof(server), 6, 0x0a000235);
    server_size = test_add_overloaded_tftp_server_name(
        server, sizeof(server), "bad_name.example");
    genet_socket_send_packet(sockets[0], server, server_size);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "network-dhcp-wait");
    g_clear_pointer(&state, g_free);
    server_size = test_make_dhcp_reply(server, sizeof(server), client, 2);
    test_set_dhcp_server_addresses(server, server_size, 0x0a000202, 0);
    server_size = test_add_dhcp_ipv4_option(
        server, sizeof(server), 6, 0x0a000235);
    server_size = test_add_overloaded_tftp_server_name(
        server, sizeof(server), "tftp.example");
    genet_socket_send_packet(sockets[0], server, server_size);

    client_size = genet_socket_read_packet(sockets[0], client,
                                            sizeof(client));
    g_assert_cmpuint(test_dhcp_message_type(client, client_size), ==, 3);
    server_size = test_make_dhcp_reply(server, sizeof(server), client, 5);
    test_set_dhcp_server_addresses(server, server_size, 0x0a000202, 0);
    server_size = test_add_dhcp_ipv4_option(
        server, sizeof(server), 6, 0x0a000235);
    server_size = test_add_overloaded_tftp_server_name(
        server, sizeof(server), "tftp.example");
    genet_socket_send_packet(sockets[0], server, server_size);

    client_size = genet_socket_read_packet(sockets[0], client,
                                            sizeof(client));
    g_assert_cmphex(lduw_be_p(client + 12), ==, 0x0806);
    g_assert_cmphex(ldl_be_p(client + 14 + 24), ==, 0x0a000235);
    server_size = test_make_arp_reply(server, client);
    genet_socket_send_packet(sockets[0], server, server_size);

    client_size = genet_socket_read_packet(sockets[0], client,
                                            sizeof(client));
    ip = client + 14;
    udp = ip + 20;
    dns = udp + 8;
    g_assert_cmphex(lduw_be_p(client + 12), ==, 0x0800);
    g_assert_cmphex(ldl_be_p(ip + 16), ==, 0x0a000235);
    g_assert_cmphex(lduw_be_p(udp), ==, 49153);
    g_assert_cmphex(lduw_be_p(udp + 2), ==, 53);
    g_assert_cmpuint(dns[12], ==, 4);
    g_assert_cmpmem(dns + 13, 4, "tftp", 4);
    g_assert_cmpuint(dns[17], ==, 7);
    g_assert_cmpmem(dns + 18, 7, "example", 7);
    g_assert_cmpuint(dns[25], ==, 0);

    server_size = test_make_dns_reply(
        server, sizeof(server), client, client_size, 0x0a000263);
    stw_be_p(server + 14 + 20 + 8, lduw_be_p(dns) + 1);
    genet_socket_send_packet(sockets[0], server, server_size);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "network-dns-wait");
    g_clear_pointer(&state, g_free);

    qtest_clock_step(qts, INT64_C(1000) * 1000 * 1000);
    client_size = genet_socket_read_packet(sockets[0], client,
                                            sizeof(client));
    server_size = test_make_dns_reply(
        server, sizeof(server), client, client_size, 0x0a000263);
    memcpy(server + 6, "\x52\x55\x0a\x00\x02\x02", 6);
    genet_socket_send_packet(sockets[0], server, server_size);

    test_complete_response_tftp(
        qts, sockets[0], client, sizeof(client), firmware,
        sizeof(firmware), 0x0a000263, true, false);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    dns_server = qom_get_string(qts, "boot-dns-server-ip");
    tftp_hostname = qom_get_string(qts, "boot-tftp-hostname");
    network_server = qom_get_string(qts, "boot-network-server-ip");
    g_assert_cmpstr(dns_server, ==, "10.0.2.53");
    g_assert_cmpstr(tftp_hostname, ==, "tftp.example");
    g_assert_cmpstr(network_server, ==, "10.0.2.99");
    g_assert_cmpuint(qom_get_uint64(qts, "boot-dns-retransmit-count"),
                     ==, 1);
    qtest_quit(qts);
    close(sockets[0]);
    unlink(eeprom_path);
    unlink(network_path);
}

static void test_network_boot_option67_ignored(void)
{
    g_autofree char *eeprom_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *state = NULL;
    uint8_t client[576];
    uint8_t server[576];
    size_t client_size;
    size_t server_size;
    int sockets[2];
    QTestState *qts = start_with_eeprom(
        "BOOT_ORDER=0xe2\nDHCP_TIMEOUT=5000\n", false, &eeprom_path);

    qtest_quit(qts);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "network-boot-wire=on "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-netdev socket,fd=%d,id=net0 "
        "-global bcm2711-genet.netdev=net0 "
        "-global bcm2711-genet.mac=52:54:00:12:34:56",
        eeprom_path, sockets[1]);
    qts = qtest_init(command);
    close(sockets[1]);

    client_size = genet_socket_read_packet(sockets[0], client,
                                            sizeof(client));
    server_size = test_make_dhcp_reply(server, sizeof(server), client, 2);
    test_set_dhcp_bootfile(server, server_size, "bootme");
    genet_socket_send_packet(sockets[0], server, server_size);
    client_size = genet_socket_read_packet(sockets[0], client,
                                            sizeof(client));
    g_assert_cmpuint(test_dhcp_message_type(client, client_size), ==, 3);
    server_size = test_make_dhcp_reply(server, sizeof(server), client, 5);
    test_set_dhcp_bootfile(server, server_size, "bootme");
    genet_socket_send_packet(sockets[0], server, server_size);

    client_size = genet_socket_read_packet(sockets[0], client,
                                            sizeof(client));
    g_assert_cmphex(lduw_be_p(client + 12), ==, 0x0806);
    server_size = test_make_arp_reply(server, client);
    genet_socket_send_packet(sockets[0], server, server_size);
    test_reject_optional_self_update(
        sockets[0], client, sizeof(client));
    client_size = genet_socket_read_packet(sockets[0], client,
                                            sizeof(client));
    test_assert_tftp_rrq(client, "config.txt", true);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "network-tftp-transfer");

    qtest_quit(qts);
    close(sockets[0]);
    unlink(eeprom_path);
}

static void test_network_boot_packet_drop_migration(void)
{
    static const char genet_path[] = "/machine/soc/peripherals/genet";
    g_autofree char *eeprom_path = NULL;
    g_autofree char *source_command = NULL;
    g_autofree char *destination_command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_path = NULL;
    g_autofree char *migration_uri = NULL;
    uint8_t packet[576];
    size_t packet_size = 0;
    struct pollfd pfd;
    int source_sockets[2];
    int destination_sockets[2];
    QTestState *destination;
    QTestState *source = start_with_eeprom(
        "BOOT_ORDER=0xe2\nDHCP_TIMEOUT=5000\nDHCP_REQ_TIMEOUT=1000\n",
        false, &eeprom_path);

    qtest_quit(source);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, source_sockets), ==,
                    0);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0,
                               destination_sockets), ==, 0);
    source_command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "network-boot-wire=on "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-netdev socket,fd=%d,id=net0 "
        "-global bcm2711-genet.netdev=net0 "
        "-global bcm2711-genet.mac=52:54:00:12:34:56 "
        "-global bcm2711-genet.packet-drop-direction=1 "
        "-global bcm2711-genet.packet-drop-after=0 "
        "-global bcm2711-genet.packet-drop-count=1",
        eeprom_path, source_sockets[1]);
    source = qtest_init(source_command);
    close(source_sockets[1]);
    g_assert_cmpuint(qom_path_get_uint64(
                         source, genet_path, "packet-drop-packets-seen"),
                     ==, 1);
    g_assert_cmpuint(qom_path_get_uint64(
                         source, genet_path, "packet-drops-injected"),
                     ==, 1);
    pfd = (struct pollfd) { .fd = source_sockets[0], .events = POLLIN };
    g_assert_cmpint(poll(&pfd, 1, 10), ==, 0);
    qtest_clock_step(source, INT64_C(400) * 1000 * 1000);

    migration_dir = g_dir_make_tmp("raspi4-packet-drop-XXXXXX", NULL);
    g_assert_nonnull(migration_dir);
    migration_path = g_build_filename(migration_dir, "stream.sock", NULL);
    migration_uri = g_strdup_printf("unix:%s", migration_path);
    destination_command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "network-boot-wire=on "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-netdev socket,fd=%d,id=net0 "
        "-global bcm2711-genet.netdev=net0 "
        "-global bcm2711-genet.mac=52:54:00:12:34:56 "
        "-global bcm2711-genet.packet-drop-direction=1 "
        "-global bcm2711-genet.packet-drop-after=0 "
        "-global bcm2711-genet.packet-drop-count=1 -incoming %s",
        eeprom_path, destination_sockets[1], migration_uri);
    destination = qtest_init(destination_command);
    close(destination_sockets[1]);
    qtest_qmp_assert_success(
        source, "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }",
        migration_uri);
    wait_for_migration_complete(source);
    wait_for_migration_complete(destination);
    g_assert_cmpuint(qom_path_get_uint64(
                         destination, genet_path,
                         "packet-drop-packets-seen"), ==, 2);
    g_assert_cmpuint(qom_path_get_uint64(
                         destination, genet_path, "packet-drops-injected"),
                     ==, 1);

    for (unsigned int attempt = 0; attempt < 8; attempt++) {
        packet_size = genet_socket_read_packet(
            destination_sockets[0], packet, sizeof(packet));
        if (packet_size >= 14 + 20 + 8 + 240 &&
            test_dhcp_message_type(packet, packet_size) == 1) {
            break;
        }
    }
    g_assert_cmpuint(test_dhcp_message_type(packet, packet_size), ==, 1);
    g_assert_cmpuint(qom_path_get_uint64(
                         destination, genet_path,
                         "packet-drop-packets-seen"), ==, 2);
    qtest_clock_step(destination, INT64_C(999) * 1000 * 1000);
    pfd = (struct pollfd) {
        .fd = destination_sockets[0], .events = POLLIN,
    };
    g_assert_cmpint(poll(&pfd, 1, 10), ==, 0);
    qtest_clock_step(destination, INT64_C(1) * 1000 * 1000);
    packet_size = genet_socket_read_packet(
        destination_sockets[0], packet, sizeof(packet));
    g_assert_cmpuint(test_dhcp_message_type(packet, packet_size), ==, 1);
    g_assert_cmpuint(qom_get_uint64(
                         destination, "boot-dhcp-retransmit-count"), ==, 1);
    g_assert_cmpuint(qom_path_get_uint64(
                         destination, genet_path,
                         "packet-drop-packets-seen"), ==, 3);

    qtest_quit(source);
    qtest_quit(destination);
    close(source_sockets[0]);
    close(destination_sockets[0]);
    unlink(eeprom_path);
    unlink(migration_path);
    rmdir(migration_dir);
}

static void test_network_boot_packet_drop_rx(void)
{
    static const char genet_path[] = "/machine/soc/peripherals/genet";
    g_autofree char *eeprom_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *state = NULL;
    uint8_t client[576];
    uint8_t server[576];
    size_t client_size;
    size_t server_size;
    struct pollfd pfd;
    int sockets[2];
    QTestState *qts = start_with_eeprom(
        "BOOT_ORDER=0xe2\nDHCP_TIMEOUT=5000\n", false, &eeprom_path);

    qtest_quit(qts);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "network-boot-wire=on "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-netdev socket,fd=%d,id=net0 "
        "-global bcm2711-genet.netdev=net0 "
        "-global bcm2711-genet.mac=52:54:00:12:34:56 "
        "-global bcm2711-genet.packet-drop-direction=2 "
        "-global bcm2711-genet.packet-drop-after=1 "
        "-global bcm2711-genet.packet-drop-count=1",
        eeprom_path, sockets[1]);
    qts = qtest_init(command);
    close(sockets[1]);

    client_size = genet_socket_read_packet(sockets[0], client,
                                            sizeof(client));
    server_size = test_make_dhcp_reply(server, sizeof(server), client, 2);
    genet_socket_send_packet(sockets[0], server, server_size);
    client_size = genet_socket_read_packet(sockets[0], client,
                                            sizeof(client));
    g_assert_cmpuint(test_dhcp_message_type(client, client_size), ==, 3);
    g_assert_cmpuint(qom_path_get_uint64(
                         qts, genet_path, "packet-drop-packets-seen"), ==, 1);

    server_size = test_make_dhcp_reply(server, sizeof(server), client, 5);
    genet_socket_send_packet(sockets[0], server, server_size);
    pfd = (struct pollfd) { .fd = sockets[0], .events = POLLIN };
    g_assert_cmpint(poll(&pfd, 1, 10), ==, 0);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "network-dhcp-request-wait");
    g_assert_cmpuint(qom_path_get_uint64(
                         qts, genet_path, "packet-drops-injected"), ==, 1);

    genet_socket_send_packet(sockets[0], server, server_size);
    client_size = genet_socket_read_packet(sockets[0], client,
                                            sizeof(client));
    g_assert_cmphex(lduw_be_p(client + 12), ==, 0x0806);
    g_assert_cmpuint(qom_path_get_uint64(
                         qts, genet_path, "packet-drop-packets-seen"), ==, 3);

    qtest_quit(qts);
    close(sockets[0]);
    unlink(eeprom_path);
}

static void test_network_boot_dns_migration(void)
{
    g_autofree char *eeprom_path = NULL;
    g_autofree char *source_command = NULL;
    g_autofree char *destination_command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_path = NULL;
    g_autofree char *migration_uri = NULL;
    g_autofree char *state = NULL;
    uint8_t client[576];
    uint8_t server[576];
    const uint8_t *ip;
    const uint8_t *udp;
    size_t client_size;
    size_t server_size;
    int source_sockets[2];
    int destination_sockets[2];
    QTestState *destination;
    QTestState *source = start_with_eeprom(
        "BOOT_ORDER=0xe2\nENABLE_SELF_UPDATE=0\n"
        "DHCP_TIMEOUT=15000\nDHCP_REQ_TIMEOUT=1000\n",
        false, &eeprom_path);

    qtest_quit(source);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, source_sockets), ==,
                    0);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0,
                               destination_sockets), ==, 0);
    source_command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "network-boot-wire=on "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-netdev socket,fd=%d,id=net0 "
        "-global bcm2711-genet.netdev=net0 "
        "-global bcm2711-genet.mac=52:54:00:12:34:56",
        eeprom_path, source_sockets[1]);
    source = qtest_init(source_command);
    close(source_sockets[1]);

    client_size = genet_socket_read_packet(source_sockets[0], client,
                                            sizeof(client));
    server_size = test_make_dhcp_reply(server, sizeof(server), client, 2);
    test_set_dhcp_server_addresses(server, server_size, 0x0a000202, 0);
    server_size = test_add_dhcp_ipv4_option(
        server, sizeof(server), 6, 0x0a000235);
    server_size = test_add_overloaded_tftp_server_name(
        server, sizeof(server), "tftp.example");
    genet_socket_send_packet(source_sockets[0], server, server_size);
    client_size = genet_socket_read_packet(source_sockets[0], client,
                                            sizeof(client));
    server_size = test_make_dhcp_reply(server, sizeof(server), client, 5);
    test_set_dhcp_server_addresses(server, server_size, 0x0a000202, 0);
    server_size = test_add_dhcp_ipv4_option(
        server, sizeof(server), 6, 0x0a000235);
    server_size = test_add_overloaded_tftp_server_name(
        server, sizeof(server), "tftp.example");
    genet_socket_send_packet(source_sockets[0], server, server_size);
    genet_socket_read_packet(source_sockets[0], client, sizeof(client));
    server_size = test_make_arp_reply(server, client);
    genet_socket_send_packet(source_sockets[0], server, server_size);
    client_size = genet_socket_read_packet(source_sockets[0], client,
                                            sizeof(client));
    qtest_clock_step(source, INT64_C(400) * 1000 * 1000);

    migration_dir = g_dir_make_tmp("raspi4-dns-XXXXXX", NULL);
    g_assert_nonnull(migration_dir);
    migration_path = g_build_filename(migration_dir, "stream.sock", NULL);
    migration_uri = g_strdup_printf("unix:%s", migration_path);
    destination_command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "network-boot-wire=on "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-netdev socket,fd=%d,id=net0 "
        "-global bcm2711-genet.netdev=net0 "
        "-global bcm2711-genet.mac=52:54:00:12:34:56 -incoming %s",
        eeprom_path, destination_sockets[1], migration_uri);
    destination = qtest_init(destination_command);
    close(destination_sockets[1]);
    qtest_qmp_assert_success(
        source, "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }",
        migration_uri);
    wait_for_migration_complete(source);
    wait_for_migration_complete(destination);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "network-dns-wait");

    qtest_clock_step(destination, INT64_C(600) * 1000 * 1000);
    for (unsigned int attempt = 0; attempt < 8; attempt++) {
        client_size = genet_socket_read_packet(
            destination_sockets[0], client, sizeof(client));
        if (lduw_be_p(client + 12) == 0x0800) {
            break;
        }
    }
    g_assert_cmphex(lduw_be_p(client + 12), ==, 0x0800);
    ip = client + 14;
    udp = ip + 20;
    g_assert_cmphex(ldl_be_p(ip + 16), ==, 0x0a000235);
    g_assert_cmphex(lduw_be_p(udp), ==, 49153);
    g_assert_cmphex(lduw_be_p(udp + 2), ==, 53);
    server_size = test_make_dns_reply(
        server, sizeof(server), client, client_size, 0x0a000263);
    genet_socket_send_packet(destination_sockets[0], server, server_size);

    genet_socket_read_packet(destination_sockets[0], client, sizeof(client));
    g_assert_cmphex(lduw_be_p(client + 12), ==, 0x0806);
    g_assert_cmphex(ldl_be_p(client + 14 + 24), ==, 0x0a000263);
    server_size = test_make_arp_reply(server, client);
    genet_socket_send_packet(destination_sockets[0], server, server_size);
    genet_socket_read_packet(destination_sockets[0], client, sizeof(client));
    ip = client + 14;
    udp = ip + 20;
    g_assert_cmphex(lduw_be_p(udp + 2), ==, 69);
    g_assert_cmpstr((const char *)udp + 10, ==, "config.txt");

    qtest_quit(source);
    qtest_quit(destination);
    close(source_sockets[0]);
    close(destination_sockets[0]);
    unlink(eeprom_path);
    unlink(migration_path);
    rmdir(migration_dir);
}

static void test_network_boot_dns_timeout(void)
{
    g_autofree char *eeprom_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *state = NULL;
    uint8_t client[576];
    uint8_t server[576];
    size_t server_size;
    int sockets[2];
    QTestState *qts = start_with_eeprom(
        "BOOT_ORDER=0xe2\nDHCP_TIMEOUT=5000\nDHCP_REQ_TIMEOUT=500\n",
        false, &eeprom_path);

    qtest_quit(qts);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "network-boot-wire=on "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-netdev socket,fd=%d,id=net0 "
        "-global bcm2711-genet.netdev=net0 "
        "-global bcm2711-genet.mac=52:54:00:12:34:56",
        eeprom_path, sockets[1]);
    qts = qtest_init(command);
    close(sockets[1]);

    genet_socket_read_packet(sockets[0], client, sizeof(client));
    server_size = test_make_dhcp_reply(server, sizeof(server), client, 2);
    test_set_dhcp_server_addresses(server, server_size, 0x0a000202, 0);
    server_size = test_add_dhcp_ipv4_option(
        server, sizeof(server), 6, 0x0a000235);
    server_size = test_add_overloaded_tftp_server_name(
        server, sizeof(server), "tftp.example");
    genet_socket_send_packet(sockets[0], server, server_size);
    genet_socket_read_packet(sockets[0], client, sizeof(client));
    server_size = test_make_dhcp_reply(server, sizeof(server), client, 5);
    test_set_dhcp_server_addresses(server, server_size, 0x0a000202, 0);
    server_size = test_add_dhcp_ipv4_option(
        server, sizeof(server), 6, 0x0a000235);
    server_size = test_add_overloaded_tftp_server_name(
        server, sizeof(server), "tftp.example");
    genet_socket_send_packet(sockets[0], server, server_size);
    genet_socket_read_packet(sockets[0], client, sizeof(client));
    server_size = test_make_arp_reply(server, client);
    genet_socket_send_packet(sockets[0], server, server_size);
    genet_socket_read_packet(sockets[0], client, sizeof(client));

    qtest_clock_step(qts, INT64_C(4999) * 1000 * 1000);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "network-dns-wait");
    g_clear_pointer(&state, g_free);
    qtest_clock_step(qts, INT64_C(1) * 1000 * 1000);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "stopped");
    g_assert_cmpuint(qom_get_uint64(qts, "boot-elapsed-ms"), ==, 5000);

    qtest_quit(qts);
    close(sockets[0]);
    unlink(eeprom_path);
}

static void test_network_boot_tftp_error_fallback(void)
{
    static const uint8_t firmware[] = "SD firmware after TFTP ERROR";
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *state = NULL;
    g_autofree char *source = NULL;
    uint8_t client[576];
    uint8_t server[576];
    size_t server_size;
    int sockets[2];
    QTestState *qts = start_with_eeprom_and_sd(
        "BOOT_ORDER=0x12\nENABLE_SELF_UPDATE=0\n"
        "NET_BOOT_MAX_RETRIES=0\n",
        "START4  ELF", firmware, sizeof(firmware), NULL,
        &eeprom_path, &sd_path);

    qtest_quit(qts);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "network-boot-wire=on "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=sd,format=raw,file=%s,file.locking=off "
        "-netdev socket,fd=%d,id=net0 "
        "-global bcm2711-genet.netdev=net0 "
        "-global bcm2711-genet.mac=52:54:00:12:34:56",
        eeprom_path, sd_path, sockets[1]);
    qts = qtest_init(command);
    close(sockets[1]);

    genet_socket_read_packet(sockets[0], client, sizeof(client));
    server_size = test_make_dhcp_reply(server, sizeof(server), client, 2);
    genet_socket_send_packet(sockets[0], server, server_size);
    genet_socket_read_packet(sockets[0], client, sizeof(client));
    server_size = test_make_dhcp_reply(server, sizeof(server), client, 5);
    genet_socket_send_packet(sockets[0], server, server_size);
    genet_socket_read_packet(sockets[0], client, sizeof(client));
    server_size = test_make_arp_reply(server, client);
    genet_socket_send_packet(sockets[0], server, server_size);
    genet_socket_read_packet(sockets[0], client, sizeof(client));
    test_assert_tftp_rrq(client, "config.txt", true);
    server_size = test_make_tftp_error(
        server, sizeof(server), client, 8, "Option negotiation failed");
    genet_socket_send_packet(sockets[0], server, server_size);
    genet_socket_read_packet(sockets[0], client, sizeof(client));
    test_assert_tftp_rrq(client, "config.txt", false);
    server_size = test_make_tftp_error(
        server, sizeof(server), client, 2, "Access violation");
    genet_socket_send_packet(sockets[0], server, server_size);

    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "boot-source");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(source, ==, "sd-card");
    g_assert_cmpuint(qom_get_uint64(qts, "boot-elapsed-ms"), ==, 0);
    g_assert_cmpuint(qom_get_uint64(qts, "boot-retry-count"), ==, 0);

    qtest_quit(qts);
    close(sockets[0]);
    unlink(eeprom_path);
    unlink(sd_path);
}

static void test_network_boot_tftp_device_prefix_fallback(void)
{
    g_autofree char *eeprom_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *state = NULL;
    uint8_t client[576];
    uint8_t server[576];
    size_t server_size;
    int sockets[2];
    QTestState *qts = start_with_eeprom(
        "BOOT_ORDER=0xe2\nENABLE_SELF_UPDATE=0\n"
        "TFTP_IP=10.0.2.99\n"
        "CLIENT_IP=10.0.2.15\nSUBNET=255.255.255.0\n"
        "TFTP_PREFIX=1\nTFTP_PREFIX_STR=fleet/pi4/\n",
        false, &eeprom_path);

    qtest_quit(qts);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "network-boot-wire=on "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-netdev socket,fd=%d,id=net0 "
        "-global bcm2711-genet.netdev=net0 "
        "-global bcm2711-genet.mac=52:54:00:12:34:56",
        eeprom_path, sockets[1]);
    qts = qtest_init(command);
    close(sockets[1]);

    genet_socket_read_packet(sockets[0], client, sizeof(client));
    server_size = test_make_arp_reply(server, client);
    genet_socket_send_packet(sockets[0], server, server_size);

    genet_socket_read_packet(sockets[0], client, sizeof(client));
    test_assert_tftp_rrq(client, "fleet/pi4/config.txt", true);
    server_size = test_make_tftp_not_found(
        server, sizeof(server), client);
    genet_socket_send_packet(sockets[0], server, server_size);

    genet_socket_read_packet(sockets[0], client, sizeof(client));
    test_assert_tftp_rrq(client, "fleet/pi4/start4.elf", true);
    server_size = test_make_tftp_not_found(
        server, sizeof(server), client);
    genet_socket_send_packet(sockets[0], server, server_size);

    genet_socket_read_packet(sockets[0], client, sizeof(client));
    test_assert_tftp_rrq(client, "fleet/pi4/start.elf", true);
    server_size = test_make_tftp_not_found(
        server, sizeof(server), client);
    genet_socket_send_packet(sockets[0], server, server_size);

    genet_socket_read_packet(sockets[0], client, sizeof(client));
    test_assert_tftp_rrq(client, "start.elf", true);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "network-tftp-device-prefix-fallback");
    g_assert_true(qom_get_bool(qts, "boot-tftp-prefix-fallback"));

    qtest_quit(qts);
    close(sockets[0]);
    unlink(eeprom_path);
}

static void test_network_boot_tftp_dally_migration(void)
{
    static const uint8_t firmware[] = "firmware before migrated TFTP dally";
    g_autofree char *eeprom_path = NULL;
    g_autofree char *network_path = NULL;
    g_autofree char *source_command = NULL;
    g_autofree char *destination_command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_path = NULL;
    g_autofree char *migration_uri = NULL;
    g_autofree char *state = NULL;
    uint8_t client[576];
    uint8_t server[576];
    size_t server_size;
    int source_sockets[2];
    int destination_sockets[2];
    QTestState *destination;
    QTestState *source = start_with_eeprom_and_network(
        "BOOT_ORDER=0xe2\n", "START4  ELF", firmware,
        sizeof(firmware), &eeprom_path, &network_path);

    qtest_quit(source);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, source_sockets), ==,
                    0);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0,
                               destination_sockets), ==, 0);
    source_command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "network-boot-wire=on "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-netdev socket,fd=%d,id=net0 "
        "-global bcm2711-genet.netdev=net0 "
        "-global bcm2711-genet.mac=52:54:00:12:34:56",
        eeprom_path, source_sockets[1]);
    source = qtest_init(source_command);
    close(source_sockets[1]);

    genet_socket_read_packet(source_sockets[0], client, sizeof(client));
    server_size = test_make_dhcp_reply(server, sizeof(server), client, 2);
    test_set_dhcp_server_addresses(
        server, server_size, 0x0a000203, 0);
    server_size = test_add_overloaded_tftp_server_name(
        server, sizeof(server), "10.0.2.2");
    genet_socket_send_packet(source_sockets[0], server, server_size);
    genet_socket_read_packet(source_sockets[0], client, sizeof(client));
    {
        uint8_t length;
        const uint8_t *identifier = test_dhcp_option(
            client, sizeof(client), 54, &length);

        g_assert_nonnull(identifier);
        g_assert_cmpuint(length, ==, 4);
        g_assert_cmphex(ldl_be_p(identifier), ==, 0x0a000203);
    }
    server_size = test_make_dhcp_reply(server, sizeof(server), client, 2);
    test_set_dhcp_server_addresses(
        server, server_size, 0x0a000204, 0x0a000263);
    stl_be_p(server + 14 + 20 + 8 + 16, 0);
    server_size = test_add_dhcp_bytes_option(
        server, sizeof(server), 43,
        (const uint8_t *)"Raspberry Pi Boot",
        strlen("Raspberry Pi Boot"));
    genet_socket_send_packet(source_sockets[0], server, server_size);
    server_size = test_make_dhcp_reply(server, sizeof(server), client, 5);
    test_set_dhcp_server_addresses(
        server, server_size, 0x0a000203, 0);
    server_size = test_add_overloaded_tftp_server_name(
        server, sizeof(server), "10.0.2.2");
    genet_socket_send_packet(source_sockets[0], server, server_size);
    test_complete_response_tftp(
        source, source_sockets[0], client, sizeof(client), firmware,
        sizeof(firmware), 0x0a000263, false, false);
    qtest_clock_step(source, INT64_C(200) * 1000 * 1000);

    migration_dir = g_dir_make_tmp("raspi4-tftp-dally-XXXXXX", NULL);
    g_assert_nonnull(migration_dir);
    migration_path = g_build_filename(migration_dir, "stream.sock", NULL);
    migration_uri = g_strdup_printf("unix:%s", migration_path);
    destination_command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "network-boot-wire=on "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-netdev socket,fd=%d,id=net0 "
        "-global bcm2711-genet.netdev=net0 "
        "-global bcm2711-genet.mac=52:54:00:12:34:56 -incoming %s",
        eeprom_path, destination_sockets[1], migration_uri);
    destination = qtest_init(destination_command);
    close(destination_sockets[1]);
    qtest_qmp_assert_success(
        source, "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }",
        migration_uri);
    wait_for_migration_complete(source);
    wait_for_migration_complete(destination);

    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "network-tftp-dally");
    qtest_clock_step(destination, INT64_C(299) * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "network-tftp-dally");

    server_size = test_make_tftp_data(
        server, sizeof(server), client,
        lduw_be_p(client + 14 + 20 + 8 + 2), (const uint8_t *)"", 0);
    genet_socket_send_packet(destination_sockets[0], server, server_size);
    for (unsigned int attempt = 0; attempt < 2; attempt++) {
        genet_socket_read_packet(destination_sockets[0], client,
                                 sizeof(client));
        if (lduw_be_p(client + 12) == 0x0800) {
            break;
        }
    }
    g_assert_cmphex(lduw_be_p(client + 12), ==, 0x0800);
    g_assert_cmphex(lduw_be_p(client + 14 + 20 + 8), ==, 4);
    qtest_clock_step(destination, INT64_C(499) * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "network-tftp-dally");
    qtest_clock_step(destination, INT64_C(1) * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpuint(qom_get_uint64(destination, "boot-elapsed-ms"), ==,
                     500);

    qtest_quit(source);
    qtest_quit(destination);
    close(source_sockets[0]);
    close(destination_sockets[0]);
    unlink(eeprom_path);
    unlink(network_path);
    unlink(migration_path);
    rmdir(migration_dir);
}

static void test_network_boot_pxe_option43(void)
{
    static const char custom_match[] = "Lab PXE";
    static const char default_match[] = "Raspberry Pi Boot";
    g_autofree uint8_t *eeprom = make_eeprom_image(
        "BOOT_ORDER=0xe2\n"
        "PXE_OPTION43=Lab PXE\n", false);
    g_autofree char *eeprom_path = NULL;
    g_autofree char *source_command = NULL;
    g_autofree char *destination_command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    g_autofree char *policy = NULL;
    g_autofree char *server_ip = NULL;
    uint8_t client[576];
    uint8_t server[576];
    size_t client_size;
    size_t server_size;
    int source_sockets[2];
    int destination_sockets[2];
    QTestState *source;
    QTestState *destination;

    write_temp_image("raspi4-eeprom-pxe-option43-XXXXXX", eeprom,
                     EEPROM_SIZE, &eeprom_path);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, source_sockets), ==,
                    0);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0,
                               destination_sockets), ==, 0);
    source_command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "network-boot-wire=on "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-netdev socket,fd=%d,id=net0 "
        "-global bcm2711-genet.netdev=net0 "
        "-global bcm2711-genet.mac=52:54:00:12:34:56",
        eeprom_path, source_sockets[1]);
    source = qtest_init(source_command);
    close(source_sockets[1]);
    policy = qom_get_string(source, "boot-pxe-option43");
    g_assert_cmpstr(policy, ==, custom_match);

    client_size = genet_socket_read_packet(
        source_sockets[0], client, sizeof(client));
    g_assert_cmpuint(test_dhcp_message_type(client, client_size), ==, 1);

    server_size = test_make_dhcp_reply(
        server, sizeof(server), client, 2);
    test_set_dhcp_server_addresses(
        server, server_size, 0x0a000204, 0x0a000263);
    stl_be_p(server + 14 + 20 + 8 + 16, 0);
    server_size = test_add_dhcp_bytes_option(
        server, sizeof(server), 43,
        (const uint8_t *)default_match, strlen(default_match));
    genet_socket_send_packet(source_sockets[0], server, server_size);
    server_ip = qom_get_string(source, "boot-network-server-ip");
    g_assert_cmpstr(server_ip, ==, "");

    server_size = test_make_dhcp_reply(
        server, sizeof(server), client, 2);
    genet_socket_send_packet(source_sockets[0], server, server_size);
    client_size = genet_socket_read_packet(
        source_sockets[0], client, sizeof(client));
    g_assert_cmpuint(test_dhcp_message_type(client, client_size), ==, 3);

    server_size = test_make_dhcp_reply(
        server, sizeof(server), client, 2);
    test_set_dhcp_server_addresses(
        server, server_size, 0x0a000204, 0x0a000263);
    stl_be_p(server + 14 + 20 + 8 + 16, 0);
    server_size = test_add_dhcp_bytes_option(
        server, sizeof(server), 43,
        (const uint8_t *)custom_match, strlen(custom_match));
    genet_socket_send_packet(source_sockets[0], server, server_size);
    g_clear_pointer(&server_ip, g_free);
    server_ip = qom_get_string(source, "boot-network-server-ip");
    g_assert_cmpstr(server_ip, ==, "10.0.2.99");

    destination_command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "network-boot-wire=on "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-netdev socket,fd=%d,id=net0 "
        "-global bcm2711-genet.netdev=net0 "
        "-global bcm2711-genet.mac=52:54:00:12:34:56",
        eeprom_path, destination_sockets[1]);
    destination = migrate_to_new_qtest_commands(
        source, destination_command, &migration_dir, &migration_socket);
    close(destination_sockets[1]);
    g_clear_pointer(&policy, g_free);
    policy = qom_get_string(destination, "boot-pxe-option43");
    g_assert_cmpstr(policy, ==, custom_match);
    g_clear_pointer(&server_ip, g_free);
    server_ip = qom_get_string(destination, "boot-network-server-ip");
    g_assert_cmpstr(server_ip, ==, "10.0.2.99");

    server_size = test_make_dhcp_reply(
        server, sizeof(server), client, 5);
    test_set_dhcp_server_addresses(
        server, server_size, 0x0a000202, 0);
    genet_socket_send_packet(destination_sockets[0], server, server_size);
    for (unsigned int packet = 0; packet < 3; packet++) {
        client_size = genet_socket_read_packet(
            destination_sockets[0], client, sizeof(client));
        if (client_size >= 42 && lduw_be_p(client + 12) == 0x0806) {
            break;
        }
    }
    g_assert_cmphex(lduw_be_p(client + 12), ==, 0x0806);
    g_assert_cmphex(ldl_be_p(client + 38), ==, 0x0a000263);

    qtest_system_reset(destination);
    g_clear_pointer(&policy, g_free);
    policy = qom_get_string(destination, "boot-pxe-option43");
    g_assert_cmpstr(policy, ==, custom_match);

    qtest_quit(source);
    qtest_quit(destination);
    close(source_sockets[0]);
    close(destination_sockets[0]);
    unlink(migration_socket);
    rmdir(migration_dir);
    unlink(eeprom_path);

    {
        static const char * const invalid[] = {
            "BOOT_ORDER=0xe\nPXE_OPTION43=\n",
        };

        for (size_t i = 0; i < ARRAY_SIZE(invalid); i++) {
            g_autofree char *path = NULL;
            g_autofree char *state = NULL;
            QTestState *qts = start_with_eeprom(
                invalid[i], false, &path);

            state = qom_get_string(qts, "boot-state");
            g_assert_cmpstr(state, ==, "eeprom-invalid");
            qtest_quit(qts);
            unlink(path);
        }

        {
            g_autofree char *value = g_strnfill(UINT8_MAX + 1, 'x');
            g_autofree char *config = g_strdup_printf(
                "BOOT_ORDER=0xe\nPXE_OPTION43=%s\n", value);
            g_autofree char *path = NULL;
            g_autofree char *state = NULL;
            QTestState *qts = start_with_eeprom(
                config, false, &path);

            state = qom_get_string(qts, "boot-state");
            g_assert_cmpstr(state, ==, "eeprom-invalid");
            qtest_quit(qts);
            unlink(path);
        }
    }
}

static void test_network_boot_static_ip_wire(void)
{
    static const uint8_t firmware[] = "firmware from static TFTP client";
    g_autofree char *eeprom_path = NULL;
    g_autofree char *network_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *state = NULL;
    g_autofree char *client_ip = NULL;
    g_autofree char *subnet = NULL;
    g_autofree char *gateway = NULL;
    uint8_t client[576];
    int sockets[2];
    QTestState *qts = start_with_eeprom_and_network(
        "BOOT_ORDER=0xe2\nTFTP_IP=10.0.2.99\n"
        "CLIENT_IP=10.0.2.15\nSUBNET=255.255.255.0\n",
        "START4  ELF", firmware, sizeof(firmware),
        &eeprom_path, &network_path);

    qtest_quit(qts);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "network-boot-wire=on "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-netdev socket,fd=%d,id=net0 "
        "-global bcm2711-genet.netdev=net0 "
        "-global bcm2711-genet.mac=52:54:00:12:34:56",
        eeprom_path, sockets[1]);
    qts = qtest_init(command);
    close(sockets[1]);

    test_complete_response_tftp(
        qts, sockets[0], client, sizeof(client), firmware,
        sizeof(firmware), 0x0a000263, true, false);
    state = qom_get_string(qts, "boot-state");
    client_ip = qom_get_string(qts, "boot-client-ip");
    subnet = qom_get_string(qts, "boot-subnet");
    gateway = qom_get_string(qts, "boot-gateway");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(client_ip, ==, "10.0.2.15");
    g_assert_cmpstr(subnet, ==, "255.255.255.0");
    g_assert_cmpstr(gateway, ==, "");
    g_assert_cmpuint(qom_get_uint64(qts, "boot-dhcp-retransmit-count"),
                     ==, 0);

    qtest_quit(qts);
    close(sockets[0]);
    unlink(eeprom_path);
    unlink(network_path);
}

static void test_network_tftp_self_update_wire(void)
{
    static const uint8_t firmware[] =
        "firmware after wire EEPROM self-update";
    g_autofree uint8_t *initial = make_eeprom_image(
        "BOOT_ORDER=0xe2\nENABLE_SELF_UPDATE=1\n"
        "TFTP_IP=10.0.2.99\nCLIENT_IP=10.0.2.15\n"
        "SUBNET=255.255.255.0\n", false);
    g_autofree uint8_t *update = make_eeprom_image(
        "BOOT_ORDER=0xe2\nENABLE_SELF_UPDATE=1\n"
        "TFTP_IP=10.0.2.99\nCLIENT_IP=10.0.2.15\n"
        "SUBNET=255.255.255.0\nBOOTVAR0=0x7788\n", false);
    g_autofree char *digest = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, update, EEPROM_SIZE);
    g_autofree char *signature = g_strdup_printf("%s\n", digest);
    g_autofree char *eeprom_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *state = NULL;
    g_autofree char *status = NULL;
    const char * const filenames[] = {
        "pieeprom.upd", "pieeprom.sig",
    };
    const uint8_t *files[] = {
        update, (const uint8_t *)signature,
    };
    const size_t sizes[] = {
        EEPROM_SIZE, strlen(signature),
    };
    uint8_t persisted[EEPROM_SIZE];
    uint8_t client[576];
    uint8_t server[576];
    size_t server_size;
    int sockets[2];
    int fd;
    QTestState *qts;

    write_temp_image("raspi4-eeprom-wire-update-XXXXXX",
                     initial, EEPROM_SIZE, &eeprom_path);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "network-boot-wire=on "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-netdev socket,fd=%d,id=net0 "
        "-global bcm2711-genet.netdev=net0 "
        "-global bcm2711-genet.mac=52:54:00:12:34:56",
        eeprom_path, sockets[1]);
    qts = qtest_init(command);
    close(sockets[1]);

    genet_socket_read_packet(sockets[0], client, sizeof(client));
    g_assert_cmphex(lduw_be_p(client + 12), ==, 0x0806);
    server_size = test_make_arp_reply(server, client);
    genet_socket_send_packet(sockets[0], server, server_size);
    test_transfer_tftp_files(
        sockets[0], client, sizeof(client), filenames, files, sizes,
        ARRAY_SIZE(filenames));

    state = qom_get_string(qts, "boot-state");
    status = qom_get_string(qts, "boot-self-update-status");
    g_assert_cmpstr(state, ==, "self-update-updated-reboot");
    g_assert_cmpstr(status, ==, "updated-reboot");
    fd = open(eeprom_path, O_RDONLY);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(pread(fd, persisted, sizeof(persisted), 0), ==,
                    sizeof(persisted));
    close(fd);
    g_assert_cmpmem(persisted, sizeof(persisted), update, EEPROM_SIZE);

    qtest_clock_step(qts, INT64_C(10) * 1000 * 1000);
    genet_socket_read_packet(sockets[0], client, sizeof(client));
    g_assert_cmphex(lduw_be_p(client + 12), ==, 0x0806);
    server_size = test_make_arp_reply(server, client);
    genet_socket_send_packet(sockets[0], server, server_size);
    test_transfer_tftp_files(
        sockets[0], client, sizeof(client), filenames, files, sizes,
        ARRAY_SIZE(filenames));
    g_clear_pointer(&state, g_free);
    g_clear_pointer(&status, g_free);
    state = qom_get_string(qts, "boot-state");
    status = qom_get_string(qts, "boot-self-update-status");
    g_assert_cmpstr(state, ==, "network-tftp-transfer");
    g_assert_cmpstr(status, ==, "up-to-date");

    test_complete_response_tftp(
        qts, sockets[0], client, sizeof(client), firmware,
        sizeof(firmware), 0x0a000263, true, true);
    g_clear_pointer(&state, g_free);
    g_clear_pointer(&status, g_free);
    state = qom_get_string(qts, "boot-state");
    status = qom_get_string(qts, "boot-self-update-status");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(status, ==, "up-to-date");

    qtest_quit(qts);
    close(sockets[0]);
    unlink(eeprom_path);
}

static void test_network_tftp_self_update_failure_wire(void)
{
    static const struct {
        bool bad_signature;
        bool write_protect;
        const char *status;
    } cases[] = {
        { true, false, "invalid" },
        { false, true, "write-protected" },
    };
    g_autofree uint8_t *initial = make_eeprom_image(
        "BOOT_ORDER=0xe2\nENABLE_SELF_UPDATE=1\n"
        "NET_BOOT_MAX_RETRIES=0\nTFTP_IP=10.0.2.99\n"
        "CLIENT_IP=10.0.2.15\nSUBNET=255.255.255.0\n", false);
    g_autofree uint8_t *update = make_eeprom_image(
        "BOOT_ORDER=0xe2\nENABLE_SELF_UPDATE=1\n"
        "NET_BOOT_MAX_RETRIES=0\nTFTP_IP=10.0.2.99\n"
        "CLIENT_IP=10.0.2.15\nSUBNET=255.255.255.0\n"
        "BOOTVAR0=0xbad0\n", false);
    g_autofree char *digest = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, update, EEPROM_SIZE);

    for (unsigned int i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree char *signature = NULL;
        g_autofree char *eeprom_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *state = NULL;
        g_autofree char *status = NULL;
        const char * const filenames[] = {
            "pieeprom.upd", "pieeprom.sig",
        };
        const uint8_t *files[2];
        size_t sizes[2];
        uint8_t persisted[EEPROM_SIZE];
        uint8_t client[576];
        uint8_t server[576];
        size_t server_size;
        int sockets[2];
        int fd;
        QTestState *qts;
        const char *write_protect;

        if (cases[i].bad_signature) {
            signature = g_strdup_printf("%064d\n", 0);
        } else {
            signature = g_strdup_printf("%s\n", digest);
        }
        files[0] = update;
        files[1] = (const uint8_t *)signature;
        sizes[0] = EEPROM_SIZE;
        sizes[1] = strlen(signature);
        if (cases[i].write_protect) {
            write_protect = "on";
        } else {
            write_protect = "off";
        }
        write_temp_image("raspi4-eeprom-wire-failure-XXXXXX",
                         initial, EEPROM_SIZE, &eeprom_path);
        g_assert_cmpint(
            socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "network-boot-wire=on,eeprom-write-protect=%s "
            "-drive if=none,id=pieeprom,format=raw,file=%s,"
            "file.locking=off "
            "-netdev socket,fd=%d,id=net0 "
            "-global bcm2711-genet.netdev=net0 "
            "-global bcm2711-genet.mac=52:54:00:12:34:56",
            write_protect, eeprom_path, sockets[1]);
        qts = qtest_init(command);
        close(sockets[1]);

        genet_socket_read_packet(sockets[0], client, sizeof(client));
        server_size = test_make_arp_reply(server, client);
        genet_socket_send_packet(sockets[0], server, server_size);
        test_transfer_tftp_files(
            sockets[0], client, sizeof(client), filenames, files, sizes,
            ARRAY_SIZE(filenames));

        state = qom_get_string(qts, "boot-state");
        status = qom_get_string(qts, "boot-self-update-status");
        g_assert_cmpstr(state, ==, "stopped");
        g_assert_cmpstr(status, ==, cases[i].status);
        fd = open(eeprom_path, O_RDONLY);
        g_assert_cmpint(fd, >=, 0);
        g_assert_cmpint(pread(fd, persisted, sizeof(persisted), 0), ==,
                        sizeof(persisted));
        close(fd);
        g_assert_cmpmem(persisted, sizeof(persisted),
                        initial, EEPROM_SIZE);

        qtest_quit(qts);
        close(sockets[0]);
        unlink(eeprom_path);
    }
}

static void test_network_boot_static_gateway_migration(void)
{
    g_autofree char *eeprom_path = NULL;
    g_autofree char *source_command = NULL;
    g_autofree char *destination_command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_path = NULL;
    g_autofree char *migration_uri = NULL;
    g_autofree char *state = NULL;
    g_autofree char *client_ip = NULL;
    g_autofree char *subnet = NULL;
    g_autofree char *gateway = NULL;
    g_autofree char *tftp_prefix = NULL;
    uint8_t client[576];
    uint8_t server[576];
    const uint8_t *arp;
    const uint8_t *ip;
    const uint8_t *udp;
    const uint8_t *tftp;
    size_t server_size;
    int source_sockets[2];
    int destination_sockets[2];
    QTestState *destination;
    QTestState *source = start_with_eeprom(
        "BOOT_ORDER=0xe2\nENABLE_SELF_UPDATE=0\n"
        "TFTP_IP=10.1.0.99\n"
        "CLIENT_IP=10.0.2.15\nSUBNET=255.255.255.0\n"
        "GATEWAY=10.0.2.1\n"
        "TFTP_PREFIX=1\nTFTP_PREFIX_STR=fleet/pi4/\n",
        false, &eeprom_path);

    qtest_quit(source);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, source_sockets), ==,
                    0);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0,
                               destination_sockets), ==, 0);
    source_command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "network-boot-wire=on "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-netdev socket,fd=%d,id=net0 "
        "-global bcm2711-genet.netdev=net0 "
        "-global bcm2711-genet.mac=52:54:00:12:34:56",
        eeprom_path, source_sockets[1]);
    source = qtest_init(source_command);
    close(source_sockets[1]);
    genet_socket_read_packet(source_sockets[0], client, sizeof(client));
    g_assert_cmphex(lduw_be_p(client + 12), ==, 0x0806);
    arp = client + 14;
    g_assert_cmphex(ldl_be_p(arp + 14), ==, 0x0a00020f);
    g_assert_cmphex(ldl_be_p(arp + 24), ==, 0x0a000201);
    qtest_clock_step(source, INT64_C(3000) * 1000 * 1000);

    migration_dir = g_dir_make_tmp("raspi4-static-ip-XXXXXX", NULL);
    g_assert_nonnull(migration_dir);
    migration_path = g_build_filename(migration_dir, "stream.sock", NULL);
    migration_uri = g_strdup_printf("unix:%s", migration_path);
    destination_command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "network-boot-wire=on "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-netdev socket,fd=%d,id=net0 "
        "-global bcm2711-genet.netdev=net0 "
        "-global bcm2711-genet.mac=52:54:00:12:34:56 -incoming %s",
        eeprom_path, destination_sockets[1], migration_uri);
    destination = qtest_init(destination_command);
    close(destination_sockets[1]);
    qtest_qmp_assert_success(
        source, "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }",
        migration_uri);
    wait_for_migration_complete(source);
    wait_for_migration_complete(destination);

    state = qom_get_string(destination, "boot-state");
    client_ip = qom_get_string(destination, "boot-client-ip");
    subnet = qom_get_string(destination, "boot-subnet");
    gateway = qom_get_string(destination, "boot-gateway");
    g_assert_cmpstr(state, ==, "network-arp-wait");
    g_assert_cmpstr(client_ip, ==, "10.0.2.15");
    g_assert_cmpstr(subnet, ==, "255.255.255.0");
    g_assert_cmpstr(gateway, ==, "10.0.2.1");
    qtest_clock_step(destination, INT64_C(26999) * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "network-arp-wait");

    server_size = test_make_arp_reply(server, client);
    genet_socket_send_packet(destination_sockets[0], server, server_size);
    for (unsigned int attempt = 0; attempt < 8; attempt++) {
        genet_socket_read_packet(destination_sockets[0], client,
                                 sizeof(client));
        if (lduw_be_p(client + 12) == 0x0800) {
            break;
        }
    }
    g_assert_cmphex(lduw_be_p(client + 12), ==, 0x0800);
    ip = client + 14;
    udp = ip + (ip[0] & 0xf) * 4;
    tftp = udp + 8;
    g_assert_cmphex(ldl_be_p(ip + 12), ==, 0x0a00020f);
    g_assert_cmphex(ldl_be_p(ip + 16), ==, 0x0a010063);
    g_assert_cmphex(lduw_be_p(udp + 2), ==, 69);
    g_assert_cmphex(lduw_be_p(tftp), ==, 1);
    g_assert_cmpstr((const char *)tftp + 2, ==,
                    "fleet/pi4/config.txt");
    server_size = test_make_tftp_not_found(
        server, sizeof(server), client);
    genet_socket_send_packet(destination_sockets[0], server, server_size);
    genet_socket_read_packet(destination_sockets[0], client, sizeof(client));
    test_assert_tftp_rrq(client, "fleet/pi4/start4.elf", true);
    g_assert_cmpuint(qom_get_uint32(
                         destination, "boot-tftp-prefix-mode"), ==, 1);
    tftp_prefix = qom_get_string(destination, "boot-tftp-prefix");
    g_assert_cmpstr(tftp_prefix, ==, "fleet/pi4/");

    qtest_quit(source);
    qtest_quit(destination);
    close(source_sockets[0]);
    close(destination_sockets[0]);
    unlink(eeprom_path);
    unlink(migration_path);
    rmdir(migration_dir);
}

static void test_network_boot_dhcp_gateway_wire(void)
{
    g_autofree char *eeprom_path = NULL;
    g_autofree char *command = NULL;
    uint8_t client[576];
    uint8_t server[576];
    const uint8_t *arp;
    const uint8_t *ip;
    const uint8_t *udp;
    const uint8_t *tftp;
    size_t server_size;
    int sockets[2];
    QTestState *qts = start_with_eeprom(
        "BOOT_ORDER=0xe2\nTFTP_IP=10.1.0.99\n", false, &eeprom_path);

    qtest_quit(qts);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "network-boot-wire=on "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-netdev socket,fd=%d,id=net0 "
        "-global bcm2711-genet.netdev=net0 "
        "-global bcm2711-genet.mac=52:54:00:12:34:56",
        eeprom_path, sockets[1]);
    qts = qtest_init(command);
    close(sockets[1]);

    genet_socket_read_packet(sockets[0], client, sizeof(client));
    server_size = test_make_dhcp_reply(server, sizeof(server), client, 2);
    genet_socket_send_packet(sockets[0], server, server_size);
    genet_socket_read_packet(sockets[0], client, sizeof(client));
    server_size = test_make_dhcp_reply(server, sizeof(server), client, 5);
    genet_socket_send_packet(sockets[0], server, server_size);
    for (unsigned int attempt = 0; attempt < 4; attempt++) {
        genet_socket_read_packet(sockets[0], client, sizeof(client));
        if (lduw_be_p(client + 12) == 0x0806) {
            break;
        }
    }
    g_assert_cmphex(lduw_be_p(client + 12), ==, 0x0806);
    arp = client + 14;
    g_assert_cmphex(ldl_be_p(arp + 14), ==, 0x0a00020f);
    g_assert_cmphex(ldl_be_p(arp + 24), ==, 0x0a000201);

    server_size = test_make_arp_reply(server, client);
    genet_socket_send_packet(sockets[0], server, server_size);
    genet_socket_read_packet(sockets[0], client, sizeof(client));
    ip = client + 14;
    udp = ip + (ip[0] & 0xf) * 4;
    tftp = udp + 8;
    g_assert_cmphex(ldl_be_p(ip + 12), ==, 0x0a00020f);
    g_assert_cmphex(ldl_be_p(ip + 16), ==, 0x0a010063);
    g_assert_cmphex(lduw_be_p(udp + 2), ==, 69);
    g_assert_cmphex(lduw_be_p(tftp), ==, 1);

    qtest_quit(qts);
    close(sockets[0]);
    unlink(eeprom_path);
}

static void test_network_boot_dhcp_wire_migration(void)
{
    static const uint8_t firmware[] = "firmware after migrated DHCP ACK";
    g_autofree char *eeprom_path = NULL;
    g_autofree char *network_path = NULL;
    g_autofree char *source_command = NULL;
    g_autofree char *destination_command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_path = NULL;
    g_autofree char *migration_uri = NULL;
    g_autofree char *state = NULL;
    g_autofree char *prefix = NULL;
    g_autofree char *configured_tftp_ip = NULL;
    uint8_t client[576];
    uint8_t server[576];
    size_t client_size;
    size_t server_size;
    int source_sockets[2];
    int destination_sockets[2];
    QTestState *destination;
    QTestState *source = start_with_eeprom_and_network(
        "BOOT_ORDER=0xe2\nDHCP_TIMEOUT=7000\nDHCP_REQ_TIMEOUT=4000\n"
        "TFTP_IP=10.0.2.99\nDHCP_OPTION97=0x41424344\n"
        "MAC_ADDRESS=dc:a6:32:01:36:c2\n",
        "START4  ELF",
        firmware, sizeof(firmware), &eeprom_path, &network_path);

    qtest_quit(source);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, source_sockets), ==,
                    0);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0,
                               destination_sockets), ==, 0);
    source_command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "network-boot-wire=on "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-netdev socket,fd=%d,id=net0 "
        "-global bcm2711-genet.netdev=net0 "
        "-global bcm2711-genet.mac=52:54:00:12:34:56",
        eeprom_path, source_sockets[1]);
    source = qtest_init(source_command);
    close(source_sockets[1]);
    client_size = genet_socket_read_packet(source_sockets[0], client,
                                            sizeof(client));
    g_assert_cmpmem(client + 6, 6,
                    ((const uint8_t[]) { 0xdc, 0xa6, 0x32,
                                        0x01, 0x36, 0xc2 }), 6);
    g_assert_cmpmem(client + 14 + 20 + 8 + 28, 6,
                    ((const uint8_t[]) { 0xdc, 0xa6, 0x32,
                                        0x01, 0x36, 0xc2 }), 6);
    {
        uint8_t length;
        const uint8_t *identifier = test_dhcp_option(
            client, client_size, 97, &length);

        g_assert_nonnull(identifier);
        g_assert_cmpuint(length, ==, 17);
        g_assert_cmpmem(identifier + 1, 4,
                        ((const uint8_t[]) { 0x44, 0x43, 0x42, 0x41 }), 4);
        g_assert_cmpmem(identifier + 9, 4,
                        ((const uint8_t[]) { 0x32, 0x01, 0x36, 0xc2 }), 4);
    }
    server_size = test_make_dhcp_reply(server, sizeof(server), client, 2);
    test_set_dhcp_server_addresses(
        server, server_size, 0x0a000203, 0x0a000202);
    genet_socket_send_packet(source_sockets[0], server, server_size);
    client_size = genet_socket_read_packet(source_sockets[0], client,
                                            sizeof(client));
    g_assert_cmpuint(test_dhcp_message_type(client, client_size), ==, 3);
    {
        uint8_t length;
        const uint8_t *identifier = test_dhcp_option(
            client, client_size, 54, &length);

        g_assert_nonnull(identifier);
        g_assert_cmpuint(length, ==, 4);
        g_assert_cmphex(ldl_be_p(identifier), ==, 0x0a000203);
    }
    qtest_clock_step(source, INT64_C(3000) * 1000 * 1000);

    migration_dir = g_dir_make_tmp("raspi4-dhcp-migration-XXXXXX", NULL);
    g_assert_nonnull(migration_dir);
    migration_path = g_build_filename(migration_dir, "stream.sock", NULL);
    migration_uri = g_strdup_printf("unix:%s", migration_path);
    destination_command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "network-boot-wire=on "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-netdev socket,fd=%d,id=net0 "
        "-global bcm2711-genet.netdev=net0 "
        "-global bcm2711-genet.mac=52:54:00:12:34:56 -incoming %s",
        eeprom_path, destination_sockets[1], migration_uri);
    destination = qtest_init(destination_command);
    close(destination_sockets[1]);
    qtest_qmp_assert_success(
        source, "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }",
        migration_uri);
    wait_for_migration_complete(source);
    wait_for_migration_complete(destination);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "network-dhcp-request-wait");
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-mac-address");
    g_assert_cmpstr(state, ==, "dc:a6:32:01:36:c2");
    g_assert_cmpuint(qom_get_uint64(destination, "boot-elapsed-ms"), ==, 0);
    g_assert_cmpuint(qom_get_uint32(
                         destination, "boot-dhcp-req-timeout-ms"),
                     ==, 4000);
    configured_tftp_ip = qom_get_string(destination, "boot-tftp-ip");
    g_assert_cmpstr(configured_tftp_ip, ==, "10.0.2.99");
    g_assert_cmphex(qom_get_uint32(destination, "boot-dhcp-option97"), ==,
                    0x41424344);
    qtest_clock_step(destination, INT64_C(999) * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "network-dhcp-request-wait");
    qtest_clock_step(destination, INT64_C(1) * 1000 * 1000);
    for (unsigned int attempt = 0; attempt < 2; attempt++) {
        client_size = genet_socket_read_packet(
            destination_sockets[0], client, sizeof(client));
        if (lduw_be_p(client + 12) != 0x8035) {
            break;
        }
    }
    g_assert_cmpuint(test_dhcp_message_type(client, client_size), ==, 3);
    {
        uint8_t length;
        const uint8_t *identifier = test_dhcp_option(
            client, client_size, 54, &length);

        g_assert_nonnull(identifier);
        g_assert_cmpuint(length, ==, 4);
        g_assert_cmphex(ldl_be_p(identifier), ==, 0x0a000203);
    }
    g_assert_cmpuint(qom_get_uint64(
                         destination, "boot-dhcp-retransmit-count"),
                     ==, 1);

    server_size = test_make_dhcp_reply(server, sizeof(server), client, 5);
    test_set_dhcp_server_addresses(
        server, server_size, 0x0a000203, 0x0a000202);
    genet_socket_send_packet(destination_sockets[0], server, server_size);
    test_complete_response_tftp(destination, destination_sockets[0], client,
                                sizeof(client), firmware,
                                sizeof(firmware), 0x0a000263, true, false);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    prefix = qom_get_string(destination, "firmware-os-prefix");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(prefix, ==, "");
    g_assert_cmpuint(qom_get_uint64(
                         destination, "boot-tftp-retransmit-count"),
                     ==, 1);

    qtest_quit(source);
    qtest_quit(destination);
    close(source_sockets[0]);
    close(destination_sockets[0]);
    unlink(eeprom_path);
    unlink(network_path);
    unlink(migration_path);
    rmdir(migration_dir);
}

static void test_network_boot_tftp_wire_migration(void)
{
    enum { firmware_size = 700 };
    static const uint8_t media_config[] =
        TEST_NETWORK_CONFIG "os_prefix=os/\n";
    static const uint8_t media_include[] = TEST_NETWORK_INCLUDE;
    static const uint8_t fixup[] = "QEMU USB fixup fixture";
    static const char * const initial_filenames[] = {
        "config.txt", "extra.txt", "start4.elf", "fixup4.dat",
    };
    g_autofree uint8_t *firmware = g_malloc(firmware_size);
    g_autofree uint8_t *kernel = make_arm64_kernel();
    g_autofree char *eeprom_path = NULL;
    g_autofree char *network_path = NULL;
    g_autofree char *source_command = NULL;
    g_autofree char *destination_command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_path = NULL;
    g_autofree char *migration_uri = NULL;
    g_autofree char *state = NULL;
    g_autofree char *prefix = NULL;
    uint8_t client[576];
    uint8_t request[576];
    uint8_t server[576];
    const uint8_t *initial_files[] = {
        media_config, media_include, firmware, fixup,
    };
    const size_t initial_sizes[] = {
        sizeof(media_config) - 1, sizeof(media_include) - 1,
        firmware_size, sizeof(fixup),
    };
    size_t server_size;
    int source_sockets[2];
    int destination_sockets[2];
    QTestState *destination;
    QTestState *source;

    for (size_t i = 0; i < firmware_size; i++) {
        firmware[i] = i * 19 + 7;
    }
    source = start_with_eeprom_and_network(
        "BOOT_ORDER=0xe2\nENABLE_SELF_UPDATE=0\n"
        "TFTP_FILE_TIMEOUT=7000\n", "START4  ELF",
        firmware, firmware_size, &eeprom_path, &network_path);
    qtest_quit(source);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, source_sockets), ==,
                    0);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0,
                               destination_sockets), ==, 0);
    source_command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "network-boot-wire=on "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-netdev socket,fd=%d,id=net0 "
        "-global bcm2711-genet.netdev=net0 "
        "-global bcm2711-genet.mac=52:54:00:12:34:56",
        eeprom_path, source_sockets[1]);
    source = qtest_init(source_command);
    close(source_sockets[1]);

    genet_socket_read_packet(source_sockets[0], client, sizeof(client));
    server_size = test_make_dhcp_reply(server, sizeof(server), client, 2);
    genet_socket_send_packet(source_sockets[0], server, server_size);
    genet_socket_read_packet(source_sockets[0], client, sizeof(client));
    server_size = test_make_dhcp_reply(server, sizeof(server), client, 5);
    genet_socket_send_packet(source_sockets[0], server, server_size);
    genet_socket_read_packet(source_sockets[0], client, sizeof(client));
    server_size = test_make_arp_reply(server, client);
    genet_socket_send_packet(source_sockets[0], server, server_size);
    test_transfer_tftp_files(
        source_sockets[0], client, sizeof(client), initial_filenames,
        initial_files, initial_sizes, ARRAY_SIZE(initial_files));
    genet_socket_read_packet(source_sockets[0], request, sizeof(request));
    g_assert_cmpstr((const char *)request + 14 + 20 + 8 + 2, ==,
                    "os/kernel8.img");
    server_size = test_make_tftp_not_found(
        server, sizeof(server), request);
    genet_socket_send_packet(source_sockets[0], server, server_size);
    genet_socket_read_packet(source_sockets[0], request, sizeof(request));
    g_assert_cmpstr((const char *)request + 14 + 20 + 8 + 2, ==,
                    "kernel8.img");
    server_size = test_make_tftp_data(server, sizeof(server), request, 1,
                                      kernel, 512);
    genet_socket_send_packet(source_sockets[0], server, server_size);
    genet_socket_read_packet(source_sockets[0], client, sizeof(client));
    state = qom_get_string(source, "boot-state");
    g_assert_cmpstr(state, ==, "network-tftp-prefix-fallback");
    qtest_clock_step(source, INT64_C(3000) * 1000 * 1000);

    migration_dir = g_dir_make_tmp("raspi4-tftp-migration-XXXXXX", NULL);
    g_assert_nonnull(migration_dir);
    migration_path = g_build_filename(migration_dir, "stream.sock", NULL);
    migration_uri = g_strdup_printf("unix:%s", migration_path);
    destination_command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "network-boot-wire=on "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-netdev socket,fd=%d,id=net0 "
        "-global bcm2711-genet.netdev=net0 "
        "-global bcm2711-genet.mac=52:54:00:12:34:56 -incoming %s",
        eeprom_path, destination_sockets[1], migration_uri);
    destination = qtest_init(destination_command);
    close(destination_sockets[1]);
    qtest_qmp_assert_success(
        source, "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }",
        migration_uri);
    wait_for_migration_complete(source);
    wait_for_migration_complete(destination);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "network-tftp-transfer");

    qtest_clock_step(destination, INT64_C(500) * 1000 * 1000);
    for (unsigned int attempt = 0; attempt < 2; attempt++) {
        genet_socket_read_packet(destination_sockets[0], client,
                                 sizeof(client));
        if (lduw_be_p(client + 12) != 0x8035) {
            break;
        }
    }
    g_assert_cmphex(lduw_be_p(client + 12), ==, 0x0800);
    g_assert_cmphex(lduw_be_p(client + 14 + 20 + 8), ==, 4);
    g_assert_cmphex(lduw_be_p(client + 14 + 20 + 8 + 2), ==, 1);
    g_assert_cmpuint(qom_get_uint64(
                         destination, "boot-tftp-retransmit-count"),
                     ==, 3);

    for (uint16_t block = 2; block <= 9; block++) {
        size_t offset = (block - 1) * 512;
        size_t chunk = MIN((size_t)TEST_KERNEL_SIZE - offset, (size_t)512);

        server_size = test_make_tftp_data(
            server, sizeof(server), request, block, kernel + offset, chunk);
        genet_socket_send_packet(destination_sockets[0], server, server_size);
        for (unsigned int attempt = 0; attempt < 2; attempt++) {
            genet_socket_read_packet(destination_sockets[0], client,
                                     sizeof(client));
            if (lduw_be_p(client + 12) != 0x8035) {
                break;
            }
        }
        g_assert_cmphex(lduw_be_p(client + 12), ==, 0x0800);
    }
    test_transfer_response_tftp_tail(
        destination_sockets[0], client, sizeof(client));
    qtest_clock_step(destination, INT64_C(500) * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    prefix = qom_get_string(destination, "firmware-os-prefix");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(prefix, ==, "");

    qtest_quit(source);
    qtest_quit(destination);
    close(source_sockets[0]);
    close(destination_sockets[0]);
    unlink(eeprom_path);
    unlink(network_path);
    unlink(migration_path);
    rmdir(migration_dir);
}



#endif












































static void test_tryboot_reboot_flags(void)
{
    static const uint8_t normal_start[] = "normal config.txt firmware";
    static const uint8_t try_start[] = "one-shot tryboot.txt firmware";
    g_autofree uint8_t *eeprom = make_eeprom_image(
        "BOOT_ORDER=0x1\n", false);
    g_autofree uint8_t *sd = make_tryboot_boot_image(
        normal_start, sizeof(normal_start), try_start, sizeof(try_start),
        NULL);
    g_autofree char *normal_hash = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, normal_start, sizeof(normal_start));
    g_autofree char *try_hash = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, try_start, sizeof(try_start));
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *firmware_hash = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    uint32_t flags[1] = { 1 };
    QTestState *qts;
    QTestState *destination;

    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    write_temp_image("raspi4-sd-XXXXXX", sd, SD_SIZE, &sd_path);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=sd,format=raw,file=%s,file.locking=off -nic none",
        eeprom_path, sd_path);
    qts = qtest_init(command);
    firmware_hash = qom_get_string(qts, "firmware-sha256");
    g_assert_cmpstr(firmware_hash, ==, normal_hash);

    bcm2711_property_call(qts, RPI_FWREQ_SET_REBOOT_FLAGS, flags, 1);
    flags[0] = 0;
    bcm2711_property_call(qts, RPI_FWREQ_GET_REBOOT_FLAGS, flags, 1);
    g_assert_cmpuint(flags[0], ==, 1);
    bcm2711_property_call(qts, RPI_FWREQ_NOTIFY_REBOOT, NULL, 0);
    destination = migrate_to_new_qtest(
        qts, command, &migration_dir, &migration_socket);
    qtest_quit(qts);
    qts = destination;
    qtest_system_reset(qts);

    g_clear_pointer(&firmware_hash, g_free);
    firmware_hash = qom_get_string(qts, "firmware-sha256");
    g_assert_cmpstr(firmware_hash, ==, try_hash);
    flags[0] = UINT32_MAX;
    bcm2711_property_call(qts, RPI_FWREQ_GET_REBOOT_FLAGS, flags, 1);
    g_assert_cmpuint(flags[0], ==, 0);

    qtest_system_reset(qts);
    g_clear_pointer(&firmware_hash, g_free);
    firmware_hash = qom_get_string(qts, "firmware-sha256");
    g_assert_cmpstr(firmware_hash, ==, normal_hash);

    qtest_quit(qts);
    unlink(migration_socket);
    rmdir(migration_dir);
    unlink(eeprom_path);
    unlink(sd_path);
}

static void test_bootvar0_config_filter(void)
{
    static const uint8_t normal_start[] = "normal BOOTVAR0 firmware";
    static const uint8_t selected_start[] = "BOOTVAR0-selected firmware";
    static const struct {
        const char *filter;
        uint32_t bootvar0;
        bool selected;
    } cases[] = {
        { "[bootvar0=0xa5]", 0xa5, true },
        { "[bootvar0&0x80]", 0xa5, true },
        { "[bootvar0&0x0f=0x05]", 0xa5, true },
        { "[bootvar0<0xa6]", 0xa5, true },
        { "[bootvar0>0xa4]", 0xa5, true },
        { "[bootvar0=0xa5]", 0xa4, false },
        { "[bootvar0&0x08]", 0xa5, false },
        { "[cust_otp0=0]", 0, true },
        { "[cust_otp0=1]", 0, false },
        { "[board-type=0x11]", 0, true },
        { "[board-type=0x14]", 0, false },
        { "[0x0]", 0, true },
        { "[0x1]", 0, false },
        { "[pi4]", 0, true },
        { "[cm4]", 0, false },
        { "[tryboot]", 0, false },
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree char *media_config = g_strdup_printf(
            "%s\nstart_file=try-start.elf\n"
            "fixup_file=try-fixup.dat\n", cases[i].filter);
        g_autofree uint8_t *sd = make_tryboot_boot_image(
            normal_start, sizeof(normal_start),
            selected_start, sizeof(selected_start), media_config);
        g_autofree char *config = g_strdup_printf(
            "BOOT_ORDER=0x1\nBOOTVAR0=0x%x\n", cases[i].bootvar0);
        g_autofree uint8_t *eeprom = make_eeprom_image(config, false);
        g_autofree char *expected_hash = g_compute_checksum_for_data(
            G_CHECKSUM_SHA256,
            cases[i].selected ? selected_start
                              : normal_start,
            cases[i].selected ? sizeof(selected_start)
                              : sizeof(normal_start));
        g_autofree char *eeprom_path = NULL;
        g_autofree char *sd_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *firmware_hash = NULL;
        QTestState *qts;

        write_temp_image("raspi4-sd-XXXXXX", sd, SD_SIZE, &sd_path);
        write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                         &eeprom_path);
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
            "-drive if=none,id=pieeprom,format=raw,file=%s,"
            "file.locking=off "
            "-drive if=sd,format=raw,file=%s,file.locking=off -nic none",
            eeprom_path, sd_path);
        qts = qtest_init(command);
        firmware_hash = qom_get_string(qts, "firmware-sha256");
        g_assert_cmpstr(firmware_hash, ==, expected_hash);
        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(sd_path);
    }

    {
        const uint32_t serial = 0x12345678;
        g_autofree uint8_t *sd = make_tryboot_boot_image(
            normal_start, sizeof(normal_start),
            selected_start, sizeof(selected_start),
            "[0x12345678]\n"
            "[cust_otp0=0xa5]\n"
            "start_file=try-start.elf\n"
            "fixup_file=try-fixup.dat\n");
        g_autofree uint8_t *eeprom = make_eeprom_image(
            "BOOT_ORDER=0x1\n", false);
        g_autofree uint8_t *otp = make_otp_image(
            0, 0, PI4_BOARD_REVISION, false);
        g_autofree char *expected_hash = g_compute_checksum_for_data(
            G_CHECKSUM_SHA256, selected_start, sizeof(selected_start));
        g_autofree char *eeprom_path = NULL;
        g_autofree char *sd_path = NULL;
        g_autofree char *otp_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *firmware_hash = NULL;
        QTestState *qts;

        stl_le_p(otp + (28 - 1) * 4, serial);
        stl_le_p(otp + (29 - 1) * 4, ~serial);
        stl_le_p(otp + (36 - 1) * 4, 0xa5);
        write_temp_image("raspi4-sd-XXXXXX", sd, SD_SIZE, &sd_path);
        write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                         &eeprom_path);
        write_temp_image("raspi4-otp-XXXXXX", otp, OTP_SIZE, &otp_path);
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "otp-drive=piotp" TEST_BOOTSYS_MACHINE_OPTION " "
            "-drive if=none,id=pieeprom,format=raw,file=%s,"
            "file.locking=off "
            "-drive if=none,id=piotp,format=raw,file=%s,"
            "file.locking=off "
            "-drive if=sd,format=raw,file=%s,file.locking=off -nic none",
            eeprom_path, otp_path, sd_path);
        qts = qtest_init(command);
        firmware_hash = qom_get_string(qts, "firmware-sha256");
        g_assert_cmpstr(firmware_hash, ==, expected_hash);
        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(sd_path);
        unlink(otp_path);
    }
}

static void test_autoboot_ab_partitions(void)
{
    static const uint8_t normal2[] = "A/B partition two firmware";
    static const uint8_t try2[] = "partition two tryboot.txt firmware";
    static const uint8_t normal3[] = "A/B partition three firmware";
    static const uint8_t try3[] = "partition three tryboot.txt firmware";
    const uint64_t pm_base = 0xfe100000;
    uint32_t flags[1] = { 1 };
    g_autofree uint8_t *partition2 = make_tryboot_boot_image(
        normal2, sizeof(normal2), try2, sizeof(try2),
        "[boot_partition=2]\n"
        "start_file=try-start.elf\n"
        "fixup_file=try-fixup.dat\n");
    g_autofree uint8_t *partition3 = make_tryboot_boot_image(
        normal3, sizeof(normal3), try3, sizeof(try3), NULL);
    g_autofree uint8_t *sd = make_ab_boot_image(partition2, partition3);
    g_autofree uint8_t *eeprom = make_eeprom_image(
        "BOOT_ORDER=0x1\nPARTITION_WALK=1\n", false);
    g_autofree char *partition2_hash = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, try2, sizeof(try2));
    g_autofree char *normal3_hash = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, normal3, sizeof(normal3));
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *hash = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    QTestState *qts;
    QTestState *destination;

    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    write_temp_image("raspi4-ab-sd-XXXXXX", sd, AB_SD_SIZE, &sd_path);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=sd,format=raw,file=%s,file.locking=off -nic none",
        eeprom_path, sd_path);
    qts = qtest_init(command);

    hash = qom_get_string(qts, "firmware-sha256");
    g_assert_cmpstr(hash, ==, partition2_hash);
    g_assert_cmpuint(qom_get_uint32(qts, "selected-boot-partition"), ==, 2);
    assert_bootloader_device_tree(qts, 1, 2, 0);

    destination = migrate_to_new_qtest(
        qts, command, &migration_dir, &migration_socket);
    qtest_quit(qts);
    qts = destination;
    g_assert_cmpuint(qom_get_uint32(qts, "selected-boot-partition"), ==, 2);
    assert_bootloader_device_tree(qts, 1, 2, 0);

    bcm2711_property_call(qts, RPI_FWREQ_SET_REBOOT_FLAGS, flags, 1);
    qtest_system_reset(qts);
    g_clear_pointer(&hash, g_free);
    hash = qom_get_string(qts, "firmware-sha256");
    g_assert_cmpstr(hash, ==, normal3_hash);
    g_assert_cmpuint(qom_get_uint32(qts, "selected-boot-partition"), ==, 3);
    assert_bootloader_device_tree(qts, 1, 3, 1);

    qtest_system_reset(qts);
    g_clear_pointer(&hash, g_free);
    hash = qom_get_string(qts, "firmware-sha256");
    g_assert_cmpstr(hash, ==, partition2_hash);
    g_assert_cmpuint(qom_get_uint32(qts, "selected-boot-partition"), ==, 2);
    assert_bootloader_device_tree(qts, 1, 2, 0);

    /* An explicit reboot partition wins over autoboot.txt. */
    qtest_writel(qts, pm_base + 0x20, 0x5a000005);
    qtest_system_reset(qts);
    g_clear_pointer(&hash, g_free);
    hash = qom_get_string(qts, "firmware-sha256");
    g_assert_cmpstr(hash, ==, normal3_hash);
    g_assert_cmpuint(qom_get_uint32(qts, "boot-partition"), ==, 3);
    g_assert_cmpuint(qom_get_uint32(qts, "selected-boot-partition"), ==, 3);
    assert_bootloader_device_tree(qts, 1, 3, 0);

    qtest_quit(qts);
    unlink(migration_socket);
    rmdir(migration_dir);
    unlink(eeprom_path);
    unlink(sd_path);
}

static void test_partition_walk_malformed_autoboot(void)
{
    static const uint8_t normal2[] = "partition walk firmware";
    static const uint8_t try2[] = "partition walk selected firmware";
    static const uint8_t normal3[] = "partition walk fallback three";
    static const uint8_t try3[] = "partition walk try three";
    g_autofree uint8_t *partition2 = make_tryboot_boot_image(
        normal2, sizeof(normal2), try2, sizeof(try2),
        "[boot_partition=2]\n"
        "start_file=try-start.elf\n"
        "fixup_file=try-fixup.dat\n");
    g_autofree uint8_t *partition3 = make_tryboot_boot_image(
        normal3, sizeof(normal3), try3, sizeof(try3), NULL);
    g_autofree uint8_t *sd = make_ab_boot_image(partition2, partition3);
    g_autofree uint8_t *eeprom = make_eeprom_image(
        "BOOT_ORDER=0x1\nPARTITION=1\nPARTITION_WALK=1\n", false);
    g_autofree char *expected_hash = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, try2, sizeof(try2));
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *hash = NULL;
    uint8_t *autoboot_entry =
        sd + (uint64_t)SD_PARTITION_LBA * 512 +
        (1 + FAT_SECTORS) * 512;
    QTestState *qts;

    /* The 512-byte limit makes autoboot invalid, so walking remains legal. */
    stl_le_p(autoboot_entry + 28, 513);
    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    write_temp_image("raspi4-walk-sd-XXXXXX", sd, AB_SD_SIZE, &sd_path);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=sd,format=raw,file=%s,file.locking=off -nic none",
        eeprom_path, sd_path);
    qts = qtest_init(command);
    hash = qom_get_string(qts, "firmware-sha256");
    g_assert_cmpstr(hash, ==, expected_hash);
    g_assert_cmpuint(qom_get_uint32(qts, "boot-partition"), ==, 0);
    g_assert_cmpuint(qom_get_uint32(qts, "selected-boot-partition"), ==, 2);
    assert_bootloader_device_tree(qts, 1, 2, 0);

    qtest_quit(qts);
    unlink(eeprom_path);
    unlink(sd_path);
}

static void test_partition_walk_default_enabled(void)
{
    static const uint8_t firmware2[] =
        "partition walk default selected firmware";
    static const uint8_t firmware3[] =
        "partition walk default fallback firmware";
    static const char * const machines[] = {
        "raspi4b",
        "raspi-cm4",
    };
    g_autofree uint8_t *partition2 = make_usb_boot_image(
        "START4  ELF", firmware2, sizeof(firmware2), NULL);
    g_autofree uint8_t *partition3 = make_usb_boot_image(
        "START4  ELF", firmware3, sizeof(firmware3), NULL);
    g_autofree uint8_t *sd = make_ab_boot_image(partition2, partition3);
    g_autofree char *expected_hash = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, firmware2, sizeof(firmware2));
    uint8_t *autoboot_entry =
        sd + (uint64_t)SD_PARTITION_LBA * 512 +
        (1 + FAT_SECTORS) * 512;
    g_autofree char *sd_path = NULL;

    /*
     * Make partition 1's autoboot.txt invalid. It remains a readable FAT
     * partition with no boot files, while partition 2 is bootable.
     */
    stl_le_p(autoboot_entry + 28, 513);
    write_temp_image("raspi4-walk-default-media-XXXXXX", sd, AB_SD_SIZE,
                     &sd_path);

    for (size_t i = 0; i < ARRAY_SIZE(machines); i++) {
        g_autofree uint8_t *eeprom = make_eeprom_image(
            "BOOT_ORDER=0xe1\nPARTITION=1\n", false);
        g_autofree char *eeprom_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *state = NULL;
        g_autofree char *migration_dir = NULL;
        g_autofree char *migration_socket = NULL;
        g_autofree char *hash = NULL;
        QTestState *source;
        QTestState *destination;

        write_temp_image("raspi4-walk-default-eeprom-XXXXXX", eeprom,
                         EEPROM_SIZE, &eeprom_path);
        if (!strcmp(machines[i], "raspi-cm4")) {
            command = g_strdup_printf(
                "-M raspi-cm4,boot-mode=behavioral,"
                "eeprom-drive=pieeprom,emmc-drive=emmc "
                "-drive if=none,id=pieeprom,format=raw,file=%s,"
                "file.locking=off "
                "-drive if=none,id=emmc,format=raw,file=%s,"
                "file.locking=off -nic none",
                eeprom_path, sd_path);
        } else {
            command = g_strdup_printf(
                "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
                "-drive if=none,id=pieeprom,format=raw,file=%s,"
                "file.locking=off "
                "-drive if=sd,format=raw,file=%s,file.locking=off "
                "-nic none",
                eeprom_path, sd_path);
        }
        source = qtest_init(command);

        state = qom_get_string(source, "boot-state");
        g_assert_cmpstr(state, ==, "arm-handoff-ready");
        g_assert_true(qom_get_bool(source, "boot-partition-walk"));
        g_assert_cmpuint(qom_get_uint32(
                             source, "selected-boot-partition"), ==, 2);
        hash = qom_get_string(source, "firmware-sha256");
        g_assert_cmpstr(hash, ==, expected_hash);

        qtest_system_reset(source);
        g_clear_pointer(&state, g_free);
        state = qom_get_string(source, "boot-state");
        g_assert_cmpstr(state, ==, "arm-handoff-ready");
        g_assert_true(qom_get_bool(source, "boot-partition-walk"));
        g_assert_cmpuint(qom_get_uint32(
                             source, "selected-boot-partition"), ==, 2);

        destination = migrate_to_new_qtest(
            source, command, &migration_dir, &migration_socket);
        g_assert_true(qom_get_bool(destination, "boot-partition-walk"));
        g_assert_cmpuint(qom_get_uint32(
                             destination,
                             "selected-boot-partition"), ==, 2);

        qtest_quit(source);
        qtest_quit(destination);
        unlink(migration_socket);
        rmdir(migration_dir);
        unlink(eeprom_path);
    }

    {
        g_autofree uint8_t *eeprom = make_eeprom_image(
            "BOOT_ORDER=0xe1\nPARTITION=1\nPARTITION_WALK=0\n", false);
        g_autofree char *eeprom_path = NULL;
        g_autofree char *command = NULL;
        QTestState *qts;

        write_temp_image("raspi4-walk-disabled-eeprom-XXXXXX", eeprom,
                         EEPROM_SIZE, &eeprom_path);
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
            "-drive if=none,id=pieeprom,format=raw,file=%s,"
            "file.locking=off "
            "-drive if=sd,format=raw,file=%s,file.locking=off "
            "-nic none",
            eeprom_path, sd_path);
        qts = qtest_init(command);
        g_assert_false(qom_get_bool(qts, "boot-partition-walk"));
        g_assert_cmpuint(qom_get_uint32(
                             qts, "selected-boot-partition"), ==, 1);
        qtest_quit(qts);
        unlink(eeprom_path);
    }

    unlink(sd_path);
}

static void test_secure_tryboot_image(void)
{
    static const char key_hash_hex[] =
        "dea09d2a3225f6e195b2aba975d991c19acef3534a55c0c7e7c88589957d3152";
    uint8_t key_hash[32];
    size_t inner_size;
    g_autofree uint8_t *eeprom = make_signed_eeprom(
        false, false, false, false, TEST_EEPROM_DUPLICATE_NONE);
    g_autofree uint8_t *otp = NULL;
    g_autofree uint8_t *inner = make_secure_inner_boot_image(&inner_size);
    size_t bad_boot_size = 32;
    g_autofree uint8_t *bad_boot = g_memdup2(inner, bad_boot_size);
    g_autofree char *signature = make_secure_boot_signature();
    g_autofree char *eeprom_path = NULL;
    g_autofree char *otp_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *status = NULL;
    g_autofree char *state = NULL;
    size_t signature_size = strlen(signature);
    uint32_t flags[1] = { 1 };
    Fat16Builder builder;
    QTestState *qts;

    test_decode_hex(key_hash_hex, key_hash, sizeof(key_hash));
    otp = make_secure_otp_image(key_hash);
    bad_boot[0] ^= 1;
    fat16_init(&builder);
    fat16_add_file(&builder, "BOOT    IMG", bad_boot, bad_boot_size);
    fat16_add_file(&builder, "BOOT    SIG",
                   (const uint8_t *)signature, signature_size);
    fat16_add_file(&builder, "TRYBOOT IMG", inner, inner_size);
    fat16_add_file(&builder, "TRYBOOT SIG",
                   (const uint8_t *)signature, signature_size);
    write_temp_image("raspi4-sd-XXXXXX", builder.image, SD_SIZE, &sd_path);
    g_free(builder.image);
    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    write_temp_image("raspi4-otp-XXXXXX", otp, OTP_SIZE, &otp_path);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "otp-drive=piotp" TEST_BOOTSYS_MACHINE_OPTION " "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=none,id=piotp,format=raw,file=%s,file.locking=off "
        "-drive if=sd,format=raw,file=%s,file.locking=off -nic none",
        eeprom_path, otp_path, sd_path);
    qts = qtest_init(command);
    status = qom_get_string(qts, "secure-boot-status");
    g_assert_cmpstr(status, ==, "image-hash-mismatch");

    bcm2711_property_call(qts, RPI_FWREQ_SET_REBOOT_FLAGS, flags, 1);
    qtest_system_reset(qts);
    g_clear_pointer(&status, g_free);
    status = qom_get_string(qts, "secure-boot-status");
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(status, ==, "image-verified");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    flags[0] = UINT32_MAX;
    bcm2711_property_call(qts, RPI_FWREQ_GET_REBOOT_FLAGS, flags, 1);
    g_assert_cmpuint(flags[0], ==, 0);

    qtest_system_reset(qts);
    g_clear_pointer(&status, g_free);
    status = qom_get_string(qts, "secure-boot-status");
    g_assert_cmpstr(status, ==, "image-hash-mismatch");

    qtest_quit(qts);

    {
        g_autofree uint8_t *network_eeprom = make_signed_eeprom(
            false, false, false, 1, TEST_EEPROM_DUPLICATE_NONE);
        g_autofree uint8_t *network_otp = make_secure_otp_image(key_hash);
        g_autofree char *network_eeprom_path = NULL;
        g_autofree char *network_otp_path = NULL;
        g_autofree char *network_command = NULL;
        g_autofree char *network_status = NULL;
        g_autofree char *network_state = NULL;

        write_temp_image("raspi4-eeprom-XXXXXX", network_eeprom,
                         EEPROM_SIZE, &network_eeprom_path);
        write_temp_image("raspi4-otp-XXXXXX", network_otp,
                         OTP_SIZE, &network_otp_path);
        network_command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "otp-drive=piotp" TEST_BOOTSYS_MACHINE_OPTION
            ",network-boot-drive=netboot,"
            "network-boot-wire=off "
            "-drive if=none,id=pieeprom,format=raw,file=%s,"
            "file.locking=off "
            "-drive if=none,id=piotp,format=raw,file=%s,"
            "file.locking=off "
            "-drive if=none,id=netboot,format=raw,file=%s,"
            "file.locking=off -nic none",
            network_eeprom_path, network_otp_path, sd_path);
        qts = qtest_init(network_command);
        network_status = qom_get_string(qts, "secure-boot-status");
        g_assert_cmpstr(network_status, ==, "image-hash-mismatch");

        flags[0] = 1;
        bcm2711_property_call(
            qts, RPI_FWREQ_SET_REBOOT_FLAGS, flags, 1);
        qtest_system_reset(qts);
        g_clear_pointer(&network_status, g_free);
        network_status = qom_get_string(qts, "secure-boot-status");
        network_state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(network_status, ==, "image-verified");
        g_assert_cmpstr(network_state, ==, "arm-handoff-ready");
        qtest_quit(qts);
        unlink(network_eeprom_path);
        unlink(network_otp_path);
    }

    unlink(eeprom_path);
    unlink(otp_path);
    unlink(sd_path);
}



















#ifndef _WIN32











#endif



#ifndef _WIN32


#endif































int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/raspi4/boot/recovery-no-eeprom",
                   test_recovery_without_eeprom);
    qtest_add_func("/raspi4/boot/nrpiboot", test_nrpiboot_precedes_eeprom);
    qtest_add_func("/raspi4/boot/nrpiboot-unconfigured",
                   test_nrpiboot_requires_otp_configuration);
    qtest_add_func("/raspi4/boot/cm4-nrpiboot",
                   test_cm4_nrpiboot_is_dedicated);
    qtest_add_func("/raspi4/boot/cm4-continued-guest-flash-completion",
                   test_cm4_continued_guest_flash_completion);
    qtest_add_func("/raspi4/boot/cm4-nrpiboot-gpio40",
                   test_cm4_nrpiboot_gpio40_sampling);
    qtest_add_func("/raspi4/boot/cm4-emmc",
                   test_cm4_persistent_emmc_boot);
    qtest_add_func("/raspi4/boot/cm4-emmc-hidden-partitions",
                   test_cm4_separate_emmc_partitions);
    qtest_add_func("/raspi4/boot/eeprom-order", test_eeprom_boot_order);
    qtest_add_func("/raspi4/boot/arm32-firmware-handoff",
                   test_arm32_firmware_handoff);
    qtest_add_func("/raspi4/boot/eeprom-order-platform-filters",
                   test_eeprom_boot_order_platform_filters);
    qtest_add_func("/raspi4/boot/eeprom-order-runtime-filters",
                   test_eeprom_boot_order_runtime_filters);
    qtest_add_func("/raspi4/boot/eeprom-boot-variable-expressions",
                   test_eeprom_boot_variable_expressions);
    qtest_add_func("/raspi4/boot/eeprom-order-combined-identity-filters",
                   test_eeprom_boot_order_combined_identity_filters);
    qtest_add_func("/raspi4/boot/gpt-sd", test_gpt_sd_boot);
    qtest_add_func("/raspi4/boot/logical-mbr-sd",
                   test_logical_mbr_sd_boot);
    qtest_add_func("/raspi4/boot/logical-mbr-malformed",
                   test_logical_mbr_rejects_malformed_chains);
    qtest_add_func("/raspi4/boot/firmware-config-prefix",
                   test_firmware_config_include_and_prefix);
    qtest_add_func("/raspi4/boot/firmware-pair-selection",
                   test_firmware_bootloader_owned_selection);
    qtest_add_func("/raspi4/boot/firmware-pair-include-boundary",
                   test_firmware_bootloader_owned_include_boundary);
    qtest_add_func("/raspi4/boot/firmware-config-filters",
                   test_firmware_config_combined_filters_and_include_state);
    qtest_add_func("/raspi4/boot/firmware-config-gpio-filter",
                   test_firmware_config_gpio_filter_reset);
    qtest_add_func("/raspi4/boot/firmware-config-edid-filter",
                   test_firmware_config_edid_filters_and_migration);
    qtest_add_func("/raspi4/boot/hat-eeprom-overlays",
                   test_hat_eeprom_overlays);
    qtest_add_func("/raspi4/boot/firmware-missing-files",
                   test_firmware_manifest_rejects_missing_files);
    qtest_add_func("/raspi4/boot/handoff-invalid-artifacts",
                   test_arm_handoff_rejects_invalid_artifacts);
    qtest_add_func("/raspi4/boot/arm64-image-header-layout",
                   test_arm64_image_header_layout);
    qtest_add_func("/raspi4/boot/device-tree-explicit-layout",
                   test_device_tree_explicit_layout);
    qtest_add_func("/raspi4/boot/arm32-firmware-state-layout",
                   test_arm32_firmware_state_layout);
    qtest_add_func("/raspi4/boot/cmdline-first-line",
                   test_cmdline_first_line_semantics);
    qtest_add_func("/raspi4/boot/special-orders",
                   test_special_boot_order_states);
    qtest_add_func("/raspi4/boot/network-install-request-policy",
                   test_network_install_request_policy);
    qtest_add_func("/raspi4/boot/network-install-keyboard-wait",
                   test_network_install_keyboard_wait);
    qtest_add_func("/raspi4/boot/network-install-disable-hdmi",
                   test_network_install_disable_hdmi);
    qtest_add_func("/raspi4/boot/hdmi-diagnostics-delay",
                   test_hdmi_diagnostics_delay);
    qtest_add_func("/raspi4/boot/network-install-usb-no-boot-files",
                   test_network_install_usb_no_boot_files);
    qtest_add_func("/raspi4/boot/order-fallback-sd",
                   test_boot_order_fallback_to_sd);
    qtest_add_func("/raspi4/boot/usb-mass-storage",
                   test_usb_mass_storage_boot);
    qtest_add_func("/raspi4/boot/usb-msd-exclude-vid-pid",
                   test_usb_msd_exclude_vid_pid);
    qtest_add_func("/raspi4/boot/usb-bot-reset-recovery",
                   test_usb_bot_reset_recovery);
    qtest_add_func("/raspi4/boot/usb-controller-mode-split",
                   test_usb_boot_controller_mode_split);
    qtest_add_func("/raspi4/boot/usb-external-mass-storage",
                   test_usb_external_mass_storage_boot);
    qtest_add_func("/raspi4/boot/usb-mass-storage-multiple-devices",
                   test_usb_mass_storage_multiple_devices);
    qtest_add_func("/raspi4/boot/usb-controller-topology-identity",
                   test_usb_controller_topology_identity);
    qtest_add_func("/raspi4/boot/usb-controller-read-error-fallback",
                   test_usb_controller_read_error_fallback);
    qtest_add_func("/raspi4/boot/nvme", test_nvme_boot);
    qtest_add_func("/raspi4/boot/nvme-present-failure-fallback",
                   test_nvme_present_failure_fallback);
    qtest_add_func("/raspi4/boot/nvme-command-status-fault-count",
                   test_nvme_command_status_fault_count);
    qtest_add_func("/raspi4/boot/nvme-write-status-durability",
                   test_nvme_write_status_and_durability);
    qtest_add_func("/raspi4/boot/nvme-flush-status-fault-count",
                   test_nvme_flush_status_fault_count);
    qtest_add_func("/raspi4/boot/nvme-compare-status",
                   test_nvme_compare_status);
    qtest_add_func("/raspi4/boot/network", test_network_boot);
    qtest_add_func("/raspi4/boot/tftp-prefix-modes",
                   test_tftp_prefix_modes);
    qtest_add_func("/raspi4/boot/mac-address-policy",
                   test_mac_address_policy);
    qtest_add_func("/raspi4/boot/filesystem-self-update",
                   test_boot_filesystem_self_update);
    qtest_add_func("/raspi4/boot/filesystem-self-update-policy",
                   test_boot_filesystem_self_update_policy);
    qtest_add_func("/raspi4/boot/filesystem-self-update-failures",
                   test_boot_filesystem_self_update_failures);
    qtest_add_func("/raspi4/boot/filesystem-self-update-usb-nvme",
                   test_boot_filesystem_self_update_usb_nvme);
    qtest_add_func("/raspi4/boot/network-timeout-fallback",
                   test_network_boot_timeout_fallback);
    qtest_add_func("/raspi4/boot/network-tftp-timeout-fallback",
                   test_network_tftp_timeout_fallback);
    qtest_add_func("/raspi4/boot/network-hotplug",
                   test_network_boot_hotplug);
    qtest_add_func("/raspi4/boot/usb-mass-storage-timeouts",
                   test_usb_mass_storage_timeouts);
    qtest_add_func("/raspi4/boot/usb-power-off-board-revisions",
                   test_usb_power_off_timing_and_board_revisions);
    qtest_add_func("/raspi4/boot/usb-mass-storage-hotplug",
                   test_usb_mass_storage_hotplug);
    qtest_add_func("/raspi4/boot/sd-card-hotplug",
                   test_sd_card_hotplug);
    qtest_add_func("/raspi4/boot/sd-retry-hotplug",
                   test_sd_retry_hotplug);
    qtest_add_func("/raspi4/boot/sd-detect-migration",
                   test_sd_detect_migration);
    qtest_add_func("/raspi4/peripherals/sd-active-write-removal-migration",
                   test_sd_active_write_removal_migration);
    qtest_add_func("/raspi4/peripherals/emmc-power-cut-sector-durability",
                   test_emmc_power_cut_sector_durability_migration);
    qtest_add_func("/raspi4/peripherals/emmc-volatile-cache-power-cut-flush",
                   test_emmc_volatile_cache_power_cut_flush_migration);
    qtest_add_func("/raspi4/peripherals/emmc-timed-cache-flush-power-cut",
                   test_emmc_timed_cache_flush_power_cut_migration);
    qtest_add_func("/raspi4/peripherals/emmc-timed-cache-flush-all-cuts",
                   test_emmc_timed_cache_flush_all_cut_points);
    qtest_add_func("/raspi4/peripherals/emmc-timed-cache-flush-error",
                   test_emmc_timed_cache_flush_error_retry);
    qtest_add_func("/raspi4/peripherals/emmc-timed-program-all-cuts",
                   test_emmc_timed_program_all_cut_points);
    qtest_add_func("/raspi4/peripherals/emmc-timed-reliable-migration",
                   test_emmc_timed_reliable_program_migration);
    qtest_add_func("/raspi4/peripherals/emmc-timed-program-error",
                   test_emmc_timed_program_error_retry);
    qtest_add_func("/raspi4/peripherals/emmc-timed-erase-all-cuts",
                   test_emmc_timed_erase_all_cut_points);
    qtest_add_func("/raspi4/peripherals/emmc-timed-erase-migration",
                   test_emmc_timed_erase_cache_migration);
    qtest_add_func("/raspi4/peripherals/emmc-timed-erase-error",
                   test_emmc_timed_erase_error_retry);
    qtest_add_func("/raspi4/peripherals/emmc-erase-sync-cache",
                   test_emmc_erase_sync_cache_invalidation);
    qtest_add_func("/raspi4/peripherals/emmc-mixed-reset-campaign",
                   test_emmc_mixed_timed_reset_migration_campaign);
    qtest_add_func("/raspi4/peripherals/emmc-reliable-write-cache-bypass",
                   test_emmc_reliable_write_cache_bypass_migration);
    qtest_add_func("/raspi4/peripherals/emmc-reliable-write-flush-error",
                   test_emmc_reliable_write_flush_error_retry);
    qtest_add_func("/raspi4/boot/health-migration",
                   test_boot_health_migration);
    qtest_add_func("/raspi4/boot/usb-timeout-migration",
                   test_usb_timeout_migration);
    qtest_add_func("/raspi4/boot/usb-power-off-migration",
                   test_usb_power_off_migration);
    qtest_add_func("/raspi4/boot/usb-lun-timeout-migration",
                   test_usb_lun_timeout_migration);
    qtest_add_func("/raspi4/boot/network-timeout-migration",
                   test_network_timeout_migration);
    qtest_add_func("/raspi4/boot/restart-timer-migration",
                   test_restart_timer_migration);
    qtest_add_func("/raspi4/boot/recovery-reboot-migration",
                   test_recovery_reboot_migration);
    qtest_add_func("/raspi4/boot/restart-watchdog-migration",
                   test_restart_watchdog_migration);
    qtest_add_func("/raspi4/boot/watchdog-deadline-partition-validation",
                   test_boot_watchdog_deadline_partition_and_validation);
    qtest_add_func("/raspi4/boot/watchdog-handoff-cancel",
                   test_boot_watchdog_handoff_cancels);
    qtest_add_func("/raspi4/boot/watchdog-migration",
                   test_boot_watchdog_migration);
    qtest_add_func("/raspi4/boot/sd-overcurrent-recovery-policy",
                   test_sd_overcurrent_recovery_and_policy);
    qtest_add_func("/raspi4/boot/sd-overcurrent-migration",
                   test_sd_overcurrent_migration);
    qtest_add_func("/raspi4/boot/order-wait-exhaust",
                   test_boot_order_wait_and_exhaustion);
    qtest_add_func("/raspi4/boot/fatal-error-reboot",
                   test_boot_order_fatal_error_reboot);
    qtest_add_func("/raspi4/boot/order-retry-restart",
                   test_boot_order_retry_and_restart_limits);
    qtest_add_func("/raspi4/boot/corrupt-eeprom",
                   test_corrupt_eeprom_is_rejected);
    qtest_add_func("/raspi4/boot/invalid-http-host-ignored",
                   test_invalid_http_host_is_ignored);
    qtest_add_func("/raspi4/boot/eeprom-geometry",
                   test_eeprom_geometry);
    qtest_add_func("/raspi4/boot/sd-recovery-update",
                   test_sd_recovery_update);
    qtest_add_func("/raspi4/boot/sd-recovery-executable-validation",
                   test_sd_recovery_executable_validation);
    qtest_add_func("/raspi4/boot/sd-recovery-safeguards",
                   test_sd_recovery_safeguards);
    qtest_add_func("/raspi4/boot/sd-recovery-write-protect-contract",
                   test_sd_recovery_write_protect_contract);
    qtest_add_func("/raspi4/boot/sd-recovery-nor-semantics",
                   test_sd_recovery_nor_semantics);
    qtest_add_func("/raspi4/boot/sd-recovery-fragmented-cyclic",
                   test_sd_recovery_fragmented_and_cyclic_files);
    qtest_add_func("/raspi4/boot/sd-recovery-fat12-fat32",
                   test_sd_recovery_fat12_fat32_fragmented_and_malformed);
    qtest_add_func("/raspi4/boot/sd-recovery-interrupted-resume",
                   test_sd_recovery_interrupted_resume);
    qtest_add_func("/raspi4/boot/sd-recovery-stage-faults",
                   test_sd_recovery_stage_faults);
    qtest_add_func("/raspi4/boot/sd-recovery-timed-power-cut-migration",
                   test_sd_recovery_timed_flash_power_cut_migration);
    qtest_add_func("/raspi4/boot/sd-recovery-verify-mismatch-retry-migration",
                   test_sd_recovery_verify_mismatch_retry_migration);
    qtest_add_func("/raspi4/boot/sd-recovery-backend-io-error-retry",
                   test_sd_recovery_backend_io_error_retry);
    qtest_add_func("/raspi4/boot/sd-recovery-permanent",
                   test_sd_recovery_permanent_file);
    qtest_add_func("/raspi4/boot/sd-secure-otp-provisioning",
                   test_sd_secure_otp_provisioning);
    qtest_add_func("/raspi4/boot/sd-secure-otp-provisioning-fail-closed",
                   test_sd_secure_otp_provisioning_fail_closed);
    qtest_add_func("/raspi4/boot/sd-secure-otp-provisioning-power-loss",
                   test_sd_secure_otp_provisioning_power_loss);
    qtest_add_func("/raspi4/boot/otp-backing-rpiboot",
                   test_otp_backing_and_rpiboot_policy);
    qtest_add_func("/raspi4/boot/otp-identity",
                   test_otp_identity_validation);
    qtest_add_func("/raspi4/boot/otp-secure-fail-closed",
                   test_otp_secure_boot_fails_closed);
    qtest_add_func("/raspi4/boot/secure-eeprom",
                   test_secure_eeprom_verification);
    qtest_add_func("/raspi4/boot/secure-bootsys-development-key-revocation",
                   test_secure_bootsys_development_key_revocation);
    qtest_add_func("/raspi4/boot/secure-image",
                   test_secure_boot_image_boundary);
    qtest_add_func("/raspi4/boot/secure-nvme",
                   test_secure_nvme_boot);
    qtest_add_func("/raspi4/boot/reset-status",
                   test_reset_status_survives_warm_reset);
    qtest_add_func("/raspi4/boot/watchdog-cancel",
                   test_watchdog_can_be_cancelled);
    qtest_add_func("/raspi4/boot/halt-power-policy",
                   test_halt_power_policy);
    qtest_add_func("/raspi4/boot/halt-power-policy-migration",
                   test_halt_power_policy_migration);
#ifndef _WIN32
    qtest_add_func("/raspi4/boot/halt-gpio-bridge-wake",
                   test_halt_gpio_bridge_wake);
#endif
    qtest_add_func("/raspi4/boot/tryboot-reboot-flags",
                   test_tryboot_reboot_flags);
    qtest_add_func("/raspi4/boot/bootvar0-config-filter",
                   test_bootvar0_config_filter);
    qtest_add_func("/raspi4/boot/autoboot-ab-partitions",
                   test_autoboot_ab_partitions);
    qtest_add_func("/raspi4/boot/partition-walk-malformed-autoboot",
                   test_partition_walk_malformed_autoboot);
    qtest_add_func("/raspi4/boot/partition-walk-default-enabled",
                   test_partition_walk_default_enabled);
    qtest_add_func("/raspi4/boot/secure-tryboot-image",
                   test_secure_tryboot_image);
    qtest_add_func("/raspi4/peripherals/rng200",
                   test_bcm2711_rng200_registers);
    qtest_add_func("/raspi4/peripherals/rng200-migration",
                   test_bcm2711_rng200_migration);
    qtest_add_func("/raspi4/peripherals/genet",
                   test_bcm2711_genet_registers);
#ifndef _WIN32
    qtest_add_func("/raspi4/boot/eeprom-netconsole",
                   test_eeprom_netconsole);
    qtest_add_func("/raspi4/boot/secure-network-wire",
                   test_secure_network_boot_wire);
    qtest_add_func("/raspi4/boot/secure-http-wire",
                   test_secure_http_boot_wire);
    qtest_add_func("/raspi4/boot/secure-http-dns-wire",
                   test_secure_http_dns_boot_wire);
    qtest_add_func("/raspi4/boot/default-host-builtin-ca",
                   test_default_host_builtin_ca);
#ifdef CONFIG_TASN1
    qtest_add_func("/raspi4/boot/default-host-https-handshake",
                   test_default_host_https_handshake);
#endif
    qtest_add_func("/raspi4/boot/secure-http-wire-migration",
                   test_secure_http_boot_wire_migration);
    qtest_add_func("/raspi4/boot/secure-http-error-fallback",
                   test_secure_http_error_fallback);
    qtest_add_func("/raspi4/boot/network-dhcp-wire",
                   test_network_boot_dhcp_wire);
    qtest_add_func("/raspi4/boot/network-tftp-self-update-wire",
                   test_network_tftp_self_update_wire);
    qtest_add_func("/raspi4/boot/network-tftp-self-update-failure-wire",
                   test_network_tftp_self_update_failure_wire);
    qtest_add_func("/raspi4/boot/network-dns-wire",
                   test_network_boot_dns_wire);
    qtest_add_func("/raspi4/boot/network-option67-ignored",
                   test_network_boot_option67_ignored);
    qtest_add_func("/raspi4/boot/network-packet-drop-migration",
                   test_network_boot_packet_drop_migration);
    qtest_add_func("/raspi4/boot/network-packet-drop-rx",
                   test_network_boot_packet_drop_rx);
    qtest_add_func("/raspi4/boot/network-dns-migration",
                   test_network_boot_dns_migration);
    qtest_add_func("/raspi4/boot/network-dns-timeout",
                   test_network_boot_dns_timeout);
    qtest_add_func("/raspi4/boot/network-tftp-error-fallback",
                   test_network_boot_tftp_error_fallback);
    qtest_add_func("/raspi4/boot/network-tftp-device-prefix-fallback",
                   test_network_boot_tftp_device_prefix_fallback);
    qtest_add_func("/raspi4/boot/network-tftp-dally-migration",
                   test_network_boot_tftp_dally_migration);
    qtest_add_func("/raspi4/boot/network-pxe-option43",
                   test_network_boot_pxe_option43);
    qtest_add_func("/raspi4/boot/network-static-ip-wire",
                   test_network_boot_static_ip_wire);
    qtest_add_func("/raspi4/boot/network-static-gateway-migration",
                   test_network_boot_static_gateway_migration);
    qtest_add_func("/raspi4/boot/network-dhcp-gateway-wire",
                   test_network_boot_dhcp_gateway_wire);
    qtest_add_func("/raspi4/boot/network-dhcp-wire-migration",
                   test_network_boot_dhcp_wire_migration);
    qtest_add_func("/raspi4/boot/network-tftp-wire-migration",
                   test_network_boot_tftp_wire_migration);
    qtest_add_func("/raspi4/peripherals/genet-packets",
                   test_bcm2711_genet_packets);
    qtest_add_func("/raspi4/peripherals/genet-dma-faults",
                   test_bcm2711_genet_dma_faults);
#endif
    qtest_add_func("/raspi4/peripherals/thermal",
                   test_bcm2711_thermal_registers);
    qtest_add_func("/raspi4/peripherals/firmware-expander-gpio",
                   test_bcm2711_firmware_expander_gpio);
    qtest_add_func("/raspi4/peripherals/firmware-identity",
                   test_bcm2711_firmware_identity);
    qtest_add_func("/raspi4/peripherals/firmware-edid",
                   test_bcm2711_firmware_edid);
    qtest_add_func("/raspi4/peripherals/hdmi-i2c-edid",
                   test_bcm2711_hdmi_i2c_edid);
    qtest_add_func("/raspi4/peripherals/hdmi-hotplug",
                   test_bcm2711_hdmi_hotplug);
    qtest_add_func("/raspi4/peripherals/hdmi-cec",
                   test_bcm2711_hdmi_cec);
    qtest_add_func("/raspi4/peripherals/firmware-display-timing",
                   test_bcm2711_firmware_display_timing);
    qtest_add_func("/raspi4/peripherals/firmware-mailbox-structure",
                   test_bcm2711_firmware_mailbox_structure);
    qtest_add_func("/raspi4/peripherals/firmware-framebuffer-transaction",
                   test_bcm2711_firmware_framebuffer_transaction);
    qtest_add_func("/raspi4/peripherals/firmware-framebuffer-display-control",
                   test_bcm2711_firmware_framebuffer_display_control);
    qtest_add_func("/raspi4/peripherals/firmware-framebuffer-displays",
                   test_bcm2711_firmware_framebuffer_displays);
    qtest_add_func("/raspi4/peripherals/firmware-framebuffer-transform",
                   test_bcm2711_firmware_framebuffer_transform);
    qtest_add_func("/raspi4/peripherals/firmware-framebuffer-cursor",
                   test_bcm2711_firmware_framebuffer_cursor);
    qtest_add_func("/raspi4/peripherals/firmware-power",
                   test_bcm2711_firmware_power);
    qtest_add_func("/raspi4/peripherals/firmware-overscan",
                   test_bcm2711_firmware_overscan);
    qtest_add_func("/raspi4/peripherals/firmware-xhci-reset",
                   test_bcm2711_firmware_xhci_reset);
    qtest_add_func("/raspi4/peripherals/firmware-clock",
                   test_bcm2711_firmware_clock);
    qtest_add_func("/raspi4/peripherals/firmware-temperature",
                   test_bcm2711_firmware_temperature);
    qtest_add_func("/raspi4/peripherals/firmware-throttled",
                   test_bcm2711_firmware_throttled);
    qtest_add_func("/raspi4/peripherals/firmware-framebuffer",
                   test_bcm2711_firmware_framebuffer);
    qtest_add_func("/raspi4/peripherals/firmware-framebuffer-release",
                   test_bcm2711_firmware_framebuffer_release);
    qtest_add_func("/raspi4/peripherals/pwm",
                   test_bcm2711_pwm_registers);
    qtest_add_func("/raspi4/peripherals/pwm-clocked-fifo",
                   test_bcm2711_pwm_clocked_fifo);
    qtest_add_func("/raspi4/peripherals/pwm-clock-migration",
                   test_bcm2711_pwm_clock_migration);
    qtest_add_func("/raspi4/peripherals/pwm-waveform-gpio-migration",
                   test_bcm2711_pwm_waveform_gpio_migration);
    qtest_add_func("/raspi4/peripherals/pwm-dma-migration",
                   test_bcm2711_pwm_dma_migration);
    qtest_add_func("/raspi4/peripherals/pwm1-gpio-dma-migration",
                   test_bcm2711_pwm1_gpio_dma_migration);
    qtest_add_func("/raspi4/peripherals/pwm-dual-fifo-lockstep-migration",
                   test_bcm2711_pwm_dual_fifo_lockstep_migration);
    qtest_add_func("/raspi4/peripherals/pwm-dma-panic-priority-migration",
                   test_bcm2711_pwm_dma_panic_priority_migration);
    qtest_add_func("/raspi4/peripherals/spi-dma-migration",
                   test_bcm2711_spi_dma_migration);
    qtest_add_func("/raspi4/peripherals/legacy-sdhost-registers",
                   test_bcm2711_legacy_sdhost_registers);
    qtest_add_func("/raspi4/peripherals/i2c-controllers",
                   test_bcm2711_i2c_controllers);
    qtest_add_func("/raspi4/peripherals/i2c-ten-bit-migration",
                   test_bcm2711_i2c_ten_bit_migration);
    qtest_add_func("/raspi4/peripherals/i2c-clock-stretch-timeout",
                   test_bcm2711_i2c_clock_stretch_timeout);
    qtest_add_func("/raspi4/peripherals/cyw43455-sdio-transport",
                   test_cyw43455_sdio_transport);
    qtest_add_func("/raspi4/peripherals/cyw43455-sdio-erom",
                   test_cyw43455_sdio_erom);
    qtest_add_func("/raspi4/peripherals/cyw43455-sdio-core-wrappers",
                   test_cyw43455_sdio_core_wrappers);
    qtest_add_func("/raspi4/peripherals/cyw43455-sdio-firmware-start",
                   test_cyw43455_sdio_firmware_start);
    qtest_add_func("/raspi4/peripherals/cyw43455-sdio-bcdc-control",
                   test_cyw43455_sdio_bcdc_control);
    qtest_add_func("/raspi4/peripherals/cyw43455-sdio-bcdc-packets",
                   test_cyw43455_sdio_bcdc_packets);
    qtest_add_func("/raspi4/peripherals/cyw43455-sdio-packet-migration",
                   test_cyw43455_sdio_packet_migration);
    qtest_add_func("/raspi4/peripherals/cyw43455-sdio-packet-loss-recovery",
                   test_cyw43455_sdio_packet_loss_recovery);
    qtest_add_func("/raspi4/peripherals/cyw43455-bluetooth-hci",
                   test_cyw43455_bluetooth_hci);
    qtest_add_func("/raspi4/peripherals/cyw43455-sdio-onboard-topology",
                   test_cyw43455_sdio_onboard_topology);
    qtest_add_func("/raspi4/peripherals/cyw43455-sdio-final-dtb",
                   test_cyw43455_sdio_final_dtb);
    qtest_add_func("/raspi4/peripherals/cyw43455-sdio-exact-firmware-download",
                   test_cyw43455_sdio_exact_firmware_download);
    qtest_add_func("/raspi4/peripherals/cyw43455-sdio-fault-reset",
                   test_cyw43455_sdio_fault_reset);
    qtest_add_func("/raspi4/peripherals/aux-uart",
                   test_bcm2711_aux_uart_registers);
    qtest_add_func("/raspi4/peripherals/aux-uart-core-clock",
                   test_bcm2711_aux_uart_core_clock);
    qtest_add_func("/raspi4/peripherals/aux-uart-migration",
                   test_bcm2711_aux_uart_migration);
    qtest_add_func("/raspi4/peripherals/aux-spi-migration",
                   test_bcm2711_aux_spi_migration);
    qtest_add_func("/raspi4/peripherals/dwc2-reset-commands",
                   test_bcm2711_dwc2_reset_commands);
    qtest_add_func("/raspi4/peripherals/dwc2-device-registers",
                   test_bcm2711_dwc2_device_registers);
#ifndef _WIN32
    qtest_add_func("/raspi4/peripherals/dwc2-device-transport",
                   test_bcm2711_dwc2_device_transport);
    qtest_add_func("/raspi4/peripherals/dwc2-device-pio-fifo",
                   test_bcm2711_dwc2_device_pio_fifo);
    qtest_add_func("/raspi4/peripherals/dwc2-device-pio-reset-storm",
                   test_bcm2711_dwc2_device_pio_reset_storm);
    qtest_add_func("/raspi4/peripherals/dwc2-device-suspend-migration",
                   test_bcm2711_dwc2_device_suspend_migration);
    qtest_add_func("/raspi4/peripherals/dwc2-host-pio-fifo",
                   test_bcm2711_dwc2_host_pio_fifo);
    qtest_add_func("/raspi4/peripherals/dwc2-host-pio-async-reset-storm",
                   test_bcm2711_dwc2_host_pio_async_reset_storm);
    qtest_add_func("/raspi4/boot/cm4-rpiboot-dwc2-enumeration",
                   test_cm4_rpiboot_dwc2_enumeration);
    qtest_add_func("/raspi4/boot/cm4-rpiboot-untrusted-bootcode",
                   test_cm4_rpiboot_untrusted_bootcode);
    qtest_add_func("/raspi4/boot/cm4-rpiboot-secure-provisioning",
                   test_cm4_rpiboot_secure_provisioning);
    qtest_add_func("/raspi4/boot/cm4-rpiboot-active-migration",
                   test_cm4_rpiboot_active_migration);
#endif
    qtest_add_func("/raspi4/peripherals/gpio-external-inputs",
                   test_bcm2711_gpio_external_inputs);
    qtest_add_func("/raspi4/peripherals/gpio-events",
                   test_bcm2711_gpio_events);
#ifndef _WIN32
    qtest_add_func("/raspi4/peripherals/gpio-chardev-bridge",
                   test_bcm2711_gpio_chardev_bridge);
    qtest_add_func("/raspi4/peripherals/gpio-chardev-migration",
                   test_bcm2711_gpio_chardev_migration);
    qtest_add_func("/raspi4/peripherals/eeprom-nwp-gpio-chardev",
                   test_eeprom_nwp_gpio_chardev);
#endif
    qtest_add_func("/raspi4/platform/memory-models",
                   test_pi4_memory_models);
    qtest_add_func("/raspi4/platform/memory-model-dt",
                   test_pi4_memory_model_device_tree);
    qtest_add_func("/raspi4/platform/imager-repo-url-handoff",
                   test_imager_repo_url_handoff);
    qtest_add_func("/raspi4/platform/firmware-total-mem",
                   test_firmware_total_mem_models);
    qtest_add_func("/raspi4/platform/firmware-total-mem-include",
                   test_firmware_total_mem_include_boundary);
    qtest_add_func("/raspi4/platform/firmware-gpu-mem",
                   test_firmware_gpu_mem_models);
    qtest_add_func("/raspi4/platform/firmware-bootcode-delay-sd-reset",
                   test_firmware_bootcode_delay_sd_reset);
    qtest_add_func("/raspi4/platform/firmware-bootcode-delay-network",
                   test_firmware_bootcode_delay_network);
    qtest_add_func("/raspi4/platform/firmware-bootcode-delay-include",
                   test_firmware_bootcode_delay_include_boundary);
    qtest_add_func("/raspi4/platform/firmware-bootcode-delay-invalid",
                   test_firmware_bootcode_delay_invalid);
    qtest_add_func("/raspi4/platform/firmware-bootcode-delay-migration",
                   test_firmware_bootcode_delay_migration);
    qtest_add_func("/raspi4/platform/eeprom-config-txt-append",
                   test_eeprom_config_txt_append);
    qtest_add_func("/raspi4/platform/firmware-sdram-frequency-models",
                   test_firmware_sdram_frequency_models);
    qtest_add_func("/raspi4/platform/firmware-sdram-frequency-network",
                   test_firmware_sdram_frequency_network);
    qtest_add_func("/raspi4/platform/firmware-sdram-frequency-include",
                   test_firmware_sdram_frequency_include_boundary);
    qtest_add_func("/raspi4/platform/firmware-sdram-frequency-invalid",
                   test_firmware_sdram_frequency_invalid);
    qtest_add_func("/raspi4/platform/boot-uart",
                   test_boot_uart_sd_migration_boundaries);
    qtest_add_func("/raspi4/platform/cm4-vl805-eeprom-policy",
                   test_cm4_vl805_eeprom_policy);
    qtest_add_func("/raspi4/platform/firmware-uart-2ndstage-sd",
                   test_firmware_uart_2ndstage_sd_reset_migration);
    qtest_add_func("/raspi4/platform/firmware-uart-2ndstage-network",
                   test_firmware_uart_2ndstage_network);
    qtest_add_func("/raspi4/platform/firmware-uart-2ndstage-boundaries",
                   test_firmware_uart_2ndstage_boundaries);
    qtest_add_func("/raspi4/platform/arm32-memory-model-dt",
                   test_arm32_memory_model_device_tree);
    qtest_add_func("/raspi4/platform/pcie",
                   test_bcm2711_pcie_platform);
    qtest_add_func("/raspi4/platform/pcie-migration",
                   test_bcm2711_pcie_migration);
    return g_test_run();
}
