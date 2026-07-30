/*
 * Raspberry Pi 4 boot tests for SD, USB, NVMe and CM4 media
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "raspi4-boot-test.h"

void test_cm4_nrpiboot_is_dedicated(void)
{
    g_autofree char *lifecycle_path = NULL;
    g_autofree char *lifecycle_lock = NULL;
    g_autofree char *lifecycle = NULL;
    g_autofree char *command = NULL;
    int fd = g_file_open_tmp("raspi4-provision-XXXXXX", &lifecycle_path,
                             NULL);
    QTestState *qts;

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    lifecycle_lock = g_strdup_printf("%s.lock", lifecycle_path);
    command = g_strdup_printf(
        "-M raspi-cm4,boot-mode=behavioral,nrpiboot=on,"
        "provision-state-file=%s", lifecycle_path);
    qts = qtest_init(command);
    g_autofree char *state = qom_get_string(qts, "boot-state");
    g_autofree char *source = qom_get_string(qts, "boot-source");
    g_autofree char *provision = qom_get_string(qts, "provision-state");

    g_assert_cmpstr(state, ==, "rpiboot-wait");
    g_assert_cmpstr(source, ==, "rpiboot");
    g_assert_cmpstr(provision, ==, "qemu-rpiboot-wait");
    g_assert_cmphex(qom_get_uint32(qts, "otp-board-revision"), ==,
                    CM4_BOARD_REVISION);
    g_assert_false(qom_get_bool(qts, "sd-recovery-enabled"));
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(lifecycle_path, &lifecycle, NULL, NULL));
    g_strchomp(lifecycle);
    g_assert_cmpstr(lifecycle, ==, "rpiboot-host-ready");
    unlink(lifecycle_path);
    unlink(lifecycle_lock);
}
void test_cm4_continued_guest_flash_completion(void)
{
    g_autofree char *lifecycle_path = NULL;
    g_autofree char *lifecycle_lock = NULL;
    g_autofree char *lifecycle = NULL;
    g_autofree char *command = NULL;
    int fd = g_file_open_tmp("raspi4-provision-XXXXXX", &lifecycle_path,
                             NULL);
    QTestState *qts;

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    lifecycle_lock = g_strdup_printf("%s.lock", lifecycle_path);
    command = g_strdup_printf(
        "-M raspi-cm4,boot-mode=behavioral,nrpiboot=on,"
        "provision-state-file=%s", lifecycle_path);
    qts = qtest_init(command);

    g_assert_true(g_file_set_contents(
        lifecycle_path, "rpiboot-complete\n", -1, NULL));
    qtest_qom_set_bool(qts, "/machine", "provision-flash-complete", true);
    g_assert_true(qtest_qom_get_bool(
        qts, "/machine", "provision-flash-complete"));
    g_clear_pointer(&lifecycle, g_free);
    g_assert_true(g_file_get_contents(
        lifecycle_path, &lifecycle, NULL, NULL));
    g_strchomp(lifecycle);
    g_assert_cmpstr(lifecycle, ==, "boot-ready");

    qtest_quit(qts);
    g_clear_pointer(&lifecycle, g_free);
    g_assert_true(g_file_get_contents(
        lifecycle_path, &lifecycle, NULL, NULL));
    g_strchomp(lifecycle);
    g_assert_cmpstr(lifecycle, ==, "boot-ready");
    unlink(lifecycle_path);
    unlink(lifecycle_lock);
}
void test_cm4_nrpiboot_gpio40_sampling(void)
{
    QTestState *qts = qtest_init(
        "-M raspi-cm4,boot-mode=behavioral,nrpiboot=on");
    g_autofree char *state = qom_get_string(qts, "boot-state");
    g_autofree char *source = qom_get_string(qts, "nrpiboot-source");

    g_assert_cmpstr(state, ==, "rpiboot-wait");
    g_assert_cmpstr(source, ==, "machine-property");
    g_assert_true(qom_get_bool(qts, "nrpiboot-sampled"));

    /* A driven GPIO40 is the physical active-low CM4 boot-mode input. */
    qtest_set_irq_in(qts, GPIO_QOM_PATH, "pin-input", 40, 1);
    g_assert_cmphex(qtest_readl(qts, BCM2711_GPIO_BASE + GPIO_GPLEV1) &
                    BIT(8), ==, BIT(8));
    qtest_system_reset(qts);
    g_clear_pointer(&state, g_free);
    g_clear_pointer(&source, g_free);
    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "nrpiboot-source");
    g_assert_cmpstr(source, ==, "gpio40");
    g_assert_false(qom_get_bool(qts, "nrpiboot-sampled"));

    qtest_set_irq_in(qts, GPIO_QOM_PATH, "pin-input", 40, 0);
    qtest_system_reset(qts);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "rpiboot-wait");
    g_assert_true(qom_get_bool(qts, "nrpiboot-sampled"));

    /* High impedance falls back to the backwards-compatible property. */
    qtest_set_irq_in(qts, GPIO_QOM_PATH, "pin-input", 40, -1);
    qtest_system_reset(qts);
    g_clear_pointer(&source, g_free);
    source = qom_get_string(qts, "nrpiboot-source");
    g_assert_cmpstr(source, ==, "machine-property");
    g_assert_true(qom_get_bool(qts, "nrpiboot-sampled"));
    qtest_quit(qts);
}
void test_cm4_persistent_emmc_boot(void)
{
    static const char config[] =
        "[pi4]\n"
        "kernel=missing.img\n"
        "[cm4]\n"
        "kernel=kernel8.img\n";
    g_autofree char *eeprom_path = NULL;
    g_autofree char *emmc_path = NULL;
    g_autofree char *state = NULL;
    g_autofree char *source = NULL;
    g_autofree char *device_tree_file = NULL;
    g_autofree char *emmc_drive = NULL;
    g_autofree char *recovery_status = NULL;
    g_autofree char *provision = NULL;
    g_autofree char *lifecycle_path = NULL;
    g_autofree char *lifecycle_lock = NULL;
    g_autofree char *lifecycle = NULL;
    g_autofree char *command = NULL;
    static const char boot_ready[] = "boot-ready\n";
    int fd;
    QTestState *qts = start_cm4_with_eeprom_and_emmc(
        config, &eeprom_path, &emmc_path);

    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "boot-source");
    device_tree_file = qom_get_string(qts, "firmware-device-tree-file");
    emmc_drive = qom_get_string(qts, "emmc-drive");
    recovery_status = qom_get_string(qts, "recovery-status");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(source, ==, "emmc");
    g_assert_cmpstr(device_tree_file, ==, "bcm2711-rpi-cm4.dtb");
    g_assert_cmpstr(emmc_drive, ==, "emmc");
    assert_bus_card_type(qts, "/machine/soc/peripherals/emmc2/sd-bus",
                         "emmc");
    g_assert_cmpstr(recovery_status, ==, "none");
    g_assert_cmphex(qom_get_uint32(qts, "otp-board-revision"), ==,
                    CM4_BOARD_REVISION);
    qtest_quit(qts);

    fd = g_file_open_tmp("raspi4-provision-XXXXXX", &lifecycle_path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(qemu_write_full(fd, boot_ready,
                                    sizeof(boot_ready) - 1), ==,
                    sizeof(boot_ready) - 1);
    close(fd);
    lifecycle_lock = g_strdup_printf("%s.lock", lifecycle_path);
    command = g_strdup_printf(
        "-M raspi-cm4,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "emmc-drive=emmc,provision-state-file=%s "
        "-drive if=none,id=pieeprom,format=raw,file=%s "
        "-drive if=none,id=emmc,format=raw,file=%s",
        lifecycle_path, eeprom_path, emmc_path);
    qts = qtest_init(command);
    g_clear_pointer(&state, g_free);
    g_clear_pointer(&source, g_free);
    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "boot-source");
    provision = qom_get_string(qts, "provision-state");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(source, ==, "emmc");
    g_assert_cmpstr(provision, ==, "qemu-owned");
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(lifecycle_path, &lifecycle, NULL, NULL));
    g_strchomp(lifecycle);
    g_assert_cmpstr(lifecycle, ==, "qemu-stopped");
    unlink(eeprom_path);
    unlink(emmc_path);
    unlink(lifecycle_path);
    unlink(lifecycle_lock);
}
void test_cm4_separate_emmc_partitions(void)
{
    static const char config[] = "kernel=kernel8.img\n";
    static const char card_path[] =
        "/machine/soc/peripherals/emmc2/sd-bus/child[0]";
    const size_t boot_size = 512 * KiB;
    const size_t rpmb_size = 128 * KiB;
    g_autofree uint8_t *boot = g_malloc(boot_size);
    g_autofree uint8_t *rpmb = g_malloc(rpmb_size);
    g_autofree char *eeprom_path = NULL;
    g_autofree char *emmc_path = NULL;
    g_autofree char *boot_path = NULL;
    g_autofree char *rpmb_path = NULL;
    g_autofree char *emmc_before = NULL;
    g_autofree char *emmc_after = NULL;
    g_autofree char *boot_after = NULL;
    g_autofree char *rpmb_after = NULL;
    g_autofree char *command = NULL;
    g_autofree char *destination_command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_path = NULL;
    g_autofree char *migration_uri = NULL;
    g_autofree char *state = NULL;
    g_autofree char *source = NULL;
    g_autofree char *boot_drive = NULL;
    g_autofree char *rpmb_drive = NULL;
    g_autofree char *emmc_cid = NULL;
    gsize emmc_before_size;
    gsize emmc_after_size;
    gsize boot_after_size;
    gsize rpmb_after_size;
    QTestState *destination;
    QTestState *qts = start_cm4_with_eeprom_and_emmc(
        config, &eeprom_path, &emmc_path);

    qtest_quit(qts);
    g_assert_true(g_file_get_contents(emmc_path, &emmc_before,
                                     &emmc_before_size, NULL));
    memset(boot, 0xa5, boot_size / 2);
    memset(boot + boot_size / 2, 0x5a, boot_size / 2);
    memset(rpmb, 0x3c, rpmb_size);
    write_temp_image("raspi4-emmc-boot-XXXXXX", boot, boot_size,
                     &boot_path);
    write_temp_image("raspi4-emmc-rpmb-XXXXXX", rpmb, rpmb_size,
                     &rpmb_path);

    command = g_strdup_printf(
        "-M raspi-cm4,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "emmc-drive=emmc,emmc-boot-drive=emmcboot,"
        "emmc-rpmb-drive=emmcrpmb,emmc-cid=" CM4_TEST_EMMC_CID ","
        "emmc-data-error=crc,emmc-data-error-after=4096,"
        "emmc-data-error-count=3 "
        "-m 1G "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=none,id=emmc,format=raw,file=%s,file.locking=off "
        "-drive if=none,id=emmcboot,format=raw,file=%s,file.locking=off "
        "-drive if=none,id=emmcrpmb,format=raw,file=%s,file.locking=off",
        eeprom_path, emmc_path, boot_path, rpmb_path);
    qts = qtest_init(command);
    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "boot-source");
    boot_drive = qom_get_string(qts, "emmc-boot-drive");
    rpmb_drive = qom_get_string(qts, "emmc-rpmb-drive");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(source, ==, "emmc");
    g_assert_cmpstr(boot_drive, ==, "emmcboot");
    g_assert_cmpstr(rpmb_drive, ==, "emmcrpmb");
    emmc_cid = qom_get_string(qts, "emmc-cid");
    g_assert_cmpstr(emmc_cid, ==, CM4_TEST_EMMC_CID);
    g_autofree char *data_error = qom_get_string(qts, "emmc-data-error");
    g_assert_cmpstr(data_error, ==, "crc");
    g_assert_cmpuint(qom_get_uint64(qts, "emmc-data-error-after"), ==, 4096);
    g_assert_cmpuint(qom_get_uint32(qts, "emmc-data-error-count"), ==, 3);
    g_assert_cmpuint(qom_path_get_uint64(qts,
                                        "/machine/soc/peripherals/emmc2",
                                        "data-error"), ==,
                     2);
    assert_bus_card_type(qts, "/machine/soc/peripherals/emmc2/sd-bus",
                         "emmc");
    g_assert_cmpuint(qom_path_get_uint64(qts, card_path,
                                        "boot-partition-size"), ==,
                     boot_size / 2);
    g_assert_cmpuint(qom_path_get_uint64(qts, card_path,
                                        "rpmb-partition-size"), ==,
                     rpmb_size);
    QDict *response = qtest_qmp_assert_failure_ref(
        qts, "{ 'execute': 'eject', 'arguments': {"
             "'device': 'emmc', 'force': true } }");
    g_assert_nonnull(strstr(qdict_get_str(response, "desc"),
                            "not removable"));
    qobject_unref(response);

    qtest_system_reset(qts);
    g_clear_pointer(&state, g_free);
    g_clear_pointer(&source, g_free);
    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "boot-source");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(source, ==, "emmc");
    g_assert_cmpuint(qom_path_get_uint64(qts, card_path,
                                        "boot-partition-size"), ==,
                     boot_size / 2);
    g_assert_cmpuint(qom_path_get_uint64(qts, card_path,
                                        "rpmb-partition-size"), ==,
                     rpmb_size);

    migration_dir = g_dir_make_tmp("raspi4-emmc-migration-XXXXXX", NULL);
    g_assert_nonnull(migration_dir);
    migration_path = g_build_filename(migration_dir, "stream.sock", NULL);
    migration_uri = g_strdup_printf("unix:%s", migration_path);
    destination_command = g_strdup_printf("%s -incoming %s",
                                          command, migration_uri);
    destination = qtest_init(destination_command);
    qtest_qmp_assert_success(
        qts, "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }",
        migration_uri);
    wait_for_migration_complete(qts);
    wait_for_migration_complete(destination);
    g_assert_cmpuint(qom_path_get_uint64(destination, card_path,
                                        "boot-partition-size"), ==,
                     boot_size / 2);
    g_assert_cmpuint(qom_path_get_uint64(destination, card_path,
                                        "rpmb-partition-size"), ==,
                     rpmb_size);
    g_clear_pointer(&state, g_free);
    g_clear_pointer(&source, g_free);
    state = qom_get_string(destination, "boot-state");
    source = qom_get_string(destination, "boot-source");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(source, ==, "emmc");
    g_clear_pointer(&emmc_cid, g_free);
    emmc_cid = qom_get_string(destination, "emmc-cid");
    g_assert_cmpstr(emmc_cid, ==, CM4_TEST_EMMC_CID);
    qtest_quit(qts);
    qtest_quit(destination);

    g_assert_true(g_file_get_contents(emmc_path, &emmc_after,
                                     &emmc_after_size, NULL));
    g_assert_cmpuint(emmc_after_size, ==, emmc_before_size);
    g_assert_cmpmem(emmc_after, emmc_after_size,
                    emmc_before, emmc_before_size);
    g_assert_true(g_file_get_contents(boot_path, &boot_after,
                                     &boot_after_size, NULL));
    g_assert_cmpuint(boot_after_size, ==, boot_size);
    g_assert_cmpmem(boot_after, boot_after_size, boot, boot_size);
    g_assert_true(g_file_get_contents(rpmb_path, &rpmb_after,
                                     &rpmb_after_size, NULL));
    g_assert_cmpuint(rpmb_after_size, ==, rpmb_size);
    g_assert_cmpmem(rpmb_after, rpmb_after_size, rpmb, rpmb_size);
    unlink(eeprom_path);
    unlink(emmc_path);
    unlink(boot_path);
    unlink(rpmb_path);
    unlink(migration_path);
    rmdir(migration_dir);
}
void test_hat_eeprom_overlays(void)
{
    g_autofree uint8_t *overlay = NULL;
    g_autofree uint8_t *embedded_hat = NULL;
    g_autofree uint8_t *named_hat = NULL;
    size_t overlay_size;
    size_t embedded_hat_size;
    size_t named_hat_size;

    overlay = make_hat_overlay(&overlay_size);
    embedded_hat = make_hat_eeprom(
        overlay, overlay_size, &embedded_hat_size);
    named_hat = make_hat_eeprom(
        (const uint8_t *)"vc4-kms-v3d\n",
        strlen("vc4-kms-v3d\n"), &named_hat_size);

    for (unsigned int mode = 0; mode < 4; mode++) {
        const uint8_t *hat = mode == 1 ? named_hat : embedded_hat;
        size_t hat_size = mode == 1 ? named_hat_size : embedded_hat_size;
        const char *config =
            mode == 0 ? "[pi4]\ndtparam=rate=42\ndtoverlay=\n" :
            mode == 1 ? "[pi4]\ndtparam=value=77\ndtoverlay=\n" :
            mode == 2 ? "[pi4]\ndtoverlay=\n" :
                        "[pi4]\nforce_eeprom_read=0\n";
        g_autofree char *eeprom_path = NULL;
        g_autofree char *sd_path = NULL;
        g_autofree char *hat_path = NULL;
        g_autofree uint8_t *dtb = NULL;
        size_t hat_backend_size = ROUND_UP(hat_size, 512);
        g_autofree uint8_t *hat_backend = g_malloc0(hat_backend_size);
        g_autofree char *hat_content_digest = g_compute_checksum_for_data(
            G_CHECKSUM_SHA256, hat, hat_size);
        g_autofree char *hat_backend_digest = NULL;
        QTestState *qts = start_with_complex_firmware_config_variant(
            config, TEST_EXPORT_VALID, hat, hat_size,
            false,
            &eeprom_path, &sd_path, &hat_path);
        g_autofree char *status = qom_get_string(
            qts, "arm-handoff-status");
        int node;

        memcpy(hat_backend, hat, hat_size);
        hat_backend_digest = g_compute_checksum_for_data(
            G_CHECKSUM_SHA256, hat_backend, hat_backend_size);
        g_assert_cmpstr(status, ==, "ready");
        {
            g_autofree char *hat_status = qom_get_string(
                qts, "hat-eeprom-status");
            g_autofree char *hat_sha256 = qom_get_string(
                qts, "hat-eeprom-sha256");
            g_autofree char *hat_content_sha256 = qom_get_string(
                qts, "hat-content-sha256");
            g_autofree char *hat_vendor = qom_get_string(
                qts, "hat-vendor");
            g_autofree char *hat_product = qom_get_string(
                qts, "hat-product");
            g_autofree char *hat_uuid = qom_get_string(
                qts, "hat-uuid");
            g_autofree char *hat_overlay = qom_get_string(
                qts, "hat-overlay");

            if (mode == 3) {
                g_assert_cmpstr(hat_status, ==, "not-read");
                g_assert_cmpstr(hat_sha256, ==, "none");
                g_assert_cmpstr(hat_content_sha256, ==, "none");
                g_assert_cmpstr(hat_vendor, ==, "none");
                g_assert_cmpstr(hat_product, ==, "none");
                g_assert_cmpstr(hat_uuid, ==, "none");
                g_assert_cmpstr(hat_overlay, ==, "none");
                g_assert_cmpuint(qom_get_uint64(
                                     qts, "hat-eeprom-size"), ==, 0);
                g_assert_cmpuint(qom_get_uint32(
                                     qts, "hat-eeprom-declared-size"), ==, 0);
                g_assert_cmpuint(qom_get_uint32(
                                     qts, "hat-custom-count"), ==, 0);
            } else {
                g_assert_cmpstr(hat_status, ==, "valid");
                g_assert_cmpstr(hat_sha256, ==, hat_backend_digest);
                g_assert_cmpstr(
                    hat_content_sha256, ==, hat_content_digest);
                g_assert_cmpstr(hat_vendor, ==, "QEMU Labs");
                g_assert_cmpstr(hat_product, ==, "Virtual HAT");
                g_assert_cmpstr(
                    hat_uuid, ==,
                    "100f0e0d-0c0b-0a09-0807-060504030201");
                g_assert_cmpstr(
                    hat_overlay, ==,
                    mode == 1 ? "vc4-kms-v3d" : "embedded");
                g_assert_cmpuint(qom_get_uint64(
                                     qts, "hat-eeprom-size"),
                                 ==, hat_backend_size);
                g_assert_cmpuint(qom_get_uint32(
                                     qts, "hat-eeprom-declared-size"),
                                 ==, hat_size);
                g_assert_cmpuint(qom_get_uint32(
                                     qts, "hat-product-id"), ==, 0x1234);
                g_assert_cmpuint(qom_get_uint32(
                                     qts, "hat-product-version"), ==, 0x5678);
                g_assert_cmpuint(qom_get_uint32(
                                     qts, "hat-custom-count"), ==, 1);
            }
        }
        dtb = test_read_handoff_dtb(qts);
        node = fdt_path_offset(dtb, "/hat");
        if (mode == 3) {
            g_assert_cmpint(node, ==, -FDT_ERR_NOTFOUND);
        } else {
            const uint8_t expected_custom[] = {
                0xde, 0xad, 0xbe, 0xef,
            };
            const void *custom;
            int length;

            g_assert_cmpint(node, >=, 0);
            g_assert_cmpstr((const char *)fdt_getprop(
                                dtb, node, "vendor", NULL),
                            ==, "QEMU Labs");
            g_assert_cmpstr((const char *)fdt_getprop(
                                dtb, node, "product", NULL),
                            ==, "Virtual HAT");
            g_assert_cmpuint(fdt_get_u32(dtb, node, "product_id"),
                             ==, 0x1234);
            g_assert_cmpuint(fdt_get_u32(dtb, node, "product_ver"),
                             ==, 0x5678);
            custom = fdt_getprop(dtb, node, "custom_0", &length);
            g_assert_cmpmem(custom, length, expected_custom,
                            sizeof(expected_custom));
        }
        node = fdt_path_offset(
            dtb, mode == 1 ? "/overlay-device" : "/hat-device");
        if (mode >= 2) {
            g_assert_cmpint(node, ==, -FDT_ERR_NOTFOUND);
            g_assert_cmpuint(qom_get_uint32(
                                 qts, "firmware-overlay-applied"), ==, 0);
        } else {
            g_assert_cmpint(node, >=, 0);
            g_assert_cmpuint(fdt_get_u32(
                                 dtb, node,
                                 mode == 1 ? "test-value" : "rate"),
                             ==, mode == 1 ? 77 : 42);
            g_assert_cmpuint(qom_get_uint32(
                                 qts, "firmware-overlay-applied"), ==, 1);
        }
        {
            g_autofree char *gpio_status = qom_get_string(
                qts, "hat-gpio-map-status");

            if (mode == 3) {
                g_assert_cmpstr(gpio_status, ==, "none");
                g_assert_cmphex(qom_get_uint32(
                                    qts, "hat-gpio-used-mask"), ==, 0);
            } else {
                uint32_t fsel0 = qtest_readl(
                    qts, BCM2711_GPIO_BASE + GPIO_GPFSEL0);
                uint32_t fsel1 = qtest_readl(
                    qts, BCM2711_GPIO_BASE + GPIO_GPFSEL1);
                uint32_t pull0 = qtest_readl(
                    qts, BCM2711_GPIO_BASE + GPIO_PULL0);
                uint32_t pull1 = qtest_readl(
                    qts, BCM2711_GPIO_BASE + GPIO_PULL1);

                g_assert_cmpstr(
                    gpio_status, ==,
                    "digital-applied-electrical-policy-recorded");
                g_assert_cmphex(qom_get_uint32(
                                    qts, "hat-gpio-used-mask"), ==,
                                BIT(4) | BIT(17) | BIT(27));
                g_assert_cmpuint(qom_get_uint32(
                                     qts, "hat-gpio-drive"), ==, 4);
                g_assert_cmpuint(qom_get_uint32(
                                     qts, "hat-gpio-slew"), ==, 1);
                g_assert_cmpuint(qom_get_uint32(
                                     qts, "hat-gpio-hysteresis"), ==, 2);
                g_assert_cmpuint(qom_get_uint32(
                                     qts, "hat-gpio-back-power"), ==, 2);
                g_assert_cmphex(extract32(fsel0, 4 * 3, 3), ==, 4);
                g_assert_cmphex(extract32(fsel1, 7 * 3, 3), ==, 1);
                g_assert_cmphex(extract32(pull0, 4 * 2, 2), ==, 1);
                g_assert_cmphex(extract32(pull1, 1 * 2, 2), ==, 2);
                g_assert_cmphex(extract32(pull1, 11 * 2, 2), ==, 0);
            }
        }
        if (mode == 0) {
            g_autofree char *migration_dir = NULL;
            g_autofree char *migration_socket = NULL;
            g_autofree char *command = NULL;
            QTestState *source;

            qtest_writel(qts, BCM2711_GPIO_BASE + GPIO_GPFSEL0, 0);
            g_assert_cmphex(extract32(qtest_readl(
                                qts, BCM2711_GPIO_BASE + GPIO_GPFSEL0),
                                4 * 3, 3), ==, 0);
            qtest_system_reset(qts);
            g_assert_cmphex(extract32(qtest_readl(
                                qts, BCM2711_GPIO_BASE + GPIO_GPFSEL0),
                                4 * 3, 3), ==, 4);
            g_assert_cmphex(qom_get_uint32(
                                qts, "hat-gpio-used-mask"), ==,
                            BIT(4) | BIT(17) | BIT(27));
            command = g_strdup_printf(
                "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
                "hat-eeprom-drive=hat "
                "-drive if=none,id=pieeprom,format=raw,file=%s,"
                "file.locking=off "
                "-drive if=none,id=hat,format=raw,readonly=on,file=%s,"
                "file.locking=off "
                "-drive if=sd,format=raw,file=%s,file.locking=off",
                eeprom_path, hat_path, sd_path);
            source = qts;
            qts = migrate_to_new_qtest(
                source, command, &migration_dir, &migration_socket);
            g_assert_cmphex(qom_get_uint32(
                                qts, "hat-gpio-used-mask"), ==,
                            BIT(4) | BIT(17) | BIT(27));
            g_assert_cmphex(extract32(qtest_readl(
                                qts, BCM2711_GPIO_BASE + GPIO_GPFSEL0),
                                4 * 3, 3), ==, 4);
            g_assert_cmpuint(qom_get_uint32(
                                 qts, "hat-gpio-back-power"), ==, 2);
            {
                g_autofree char *migrated_hat_sha256 = qom_get_string(
                    qts, "hat-content-sha256");

                g_assert_cmpstr(
                    migrated_hat_sha256, ==, hat_content_digest);
            }
            qtest_quit(source);
            unlink(migration_socket);
            rmdir(migration_dir);
        }
        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(sd_path);
        unlink(hat_path);
    }

    {
        g_autofree uint8_t *invalid_gpio =
            g_memdup2(embedded_hat, embedded_hat_size);
        const uint8_t *invalid_images[2] = {
            embedded_hat, invalid_gpio,
        };
        size_t gpio_atom = 12 + 8 + ldl_le_p(embedded_hat + 12 + 4);
        uint32_t gpio_dlen = ldl_le_p(invalid_gpio + gpio_atom + 4);

        embedded_hat[embedded_hat_size - 1] ^= 1;
        invalid_gpio[gpio_atom + 8] =
            (invalid_gpio[gpio_atom + 8] & 0xf0) | 9;
        stw_le_p(invalid_gpio + gpio_atom + 8 + gpio_dlen - 2,
                 test_hat_crc16(invalid_gpio + gpio_atom,
                                8 + gpio_dlen - 2));
        for (unsigned int invalid = 0;
             invalid < ARRAY_SIZE(invalid_images); invalid++) {
            g_autofree char *eeprom_path = NULL;
            g_autofree char *sd_path = NULL;
            g_autofree char *hat_path = NULL;
            QTestState *qts = start_with_complex_firmware_config_variant(
                "[pi4]\n", TEST_EXPORT_VALID,
                invalid_images[invalid], embedded_hat_size,
                false,
                &eeprom_path, &sd_path, &hat_path);
            g_autofree char *status = qom_get_string(
                qts, "arm-handoff-status");

            g_assert_cmpstr(status, ==, "overlay-invalid");
            qtest_quit(qts);
            unlink(eeprom_path);
            unlink(sd_path);
            unlink(hat_path);
        }
    }
}
void test_usb_mass_storage_boot(void)
{
    static const uint8_t firmware[] = "byte-identical USB boot firmware";
    static const struct {
        const char *config;
        const char *source;
        uint8_t usb_version;
        uint8_t route_string;
    } cases[] = {
        { "BOOT_ORDER=0x4\n", "usb-msd", 3, 0 },
        { "BOOT_ORDER=0x5\n", "bcm-usb-msd", 2, 1 },
    };
    g_autofree char *expected_hash = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, firmware, sizeof(firmware));

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree char *eeprom_path = NULL;
        g_autofree char *usb_path = NULL;
        g_autofree char *state = NULL;
        g_autofree char *source = NULL;
        g_autofree char *firmware_hash = NULL;
        g_autofree char *usb_drive = NULL;
        g_autofree char *usb_transport = NULL;
        g_autofree char *qtree = NULL;
        QTestState *qts = start_with_eeprom_and_usb(
            cases[i].config, "START4  ELF", firmware, sizeof(firmware),
            &eeprom_path, &usb_path);

        state = qom_get_string(qts, "boot-state");
        source = qom_get_string(qts, "boot-source");
        firmware_hash = qom_get_string(qts, "firmware-sha256");
        usb_drive = qom_get_string(qts, "usb-boot-drive");
        usb_transport = qom_get_string(qts, "usb-boot-transport");
        qtree = qtest_hmp(qts, "info qtree");
        g_assert_cmpstr(state, ==, "arm-handoff-ready");
        g_assert_cmpstr(source, ==, cases[i].source);
        g_assert_cmpstr(firmware_hash, ==, expected_hash);
        g_assert_cmpstr(usb_drive, ==, "usbboot");
        g_assert_cmpstr(
            usb_transport, ==,
            i ? "dwc2-host-bot-scsi-read10-v1" :
                "vl805-xhci-host-bot-scsi-read10-v1");
        g_assert_cmpuint(qom_get_uint64(
                             qts, "usb-boot-controller-read-commands"), >, 0);
        g_assert_cmpuint(qom_get_uint64(
                             qts, "usb-boot-controller-read-bytes"), >=, 512);
        g_assert_nonnull(strstr(qtree, "usb-storage"));
        g_assert_cmpuint(qom_get_uint32(qts, "firmware-size"), ==,
                         sizeof(firmware));
        g_assert_cmpuint(qom_get_uint64(qts, "boot-attempt-count"), ==, 1);
        g_assert_cmpuint(qom_get_uint32(qts, "boot-order-index"), ==, 0);
        g_assert_cmpuint(qom_get_uint32(
                             qts, "boot-usb-discover-timeout-ms"), ==,
                         20000);
        g_assert_cmpuint(qom_get_uint32(qts, "boot-usb-lun-timeout-ms"),
                         ==, 2000);
        g_assert_cmpuint(qom_get_uint64(qts, "boot-elapsed-ms"), ==, 0);
        assert_bootloader_boot_mode(qts, i ? 5 : 4);
        assert_bootloader_usb_identity(
            qts, true, cases[i].usb_version, cases[i].route_string, 1, 0);

        qtest_system_reset(qts);
        g_clear_pointer(&state, g_free);
        g_clear_pointer(&source, g_free);
        state = qom_get_string(qts, "boot-state");
        source = qom_get_string(qts, "boot-source");
        g_assert_cmpstr(state, ==, "arm-handoff-ready");
        g_assert_cmpstr(source, ==, cases[i].source);
        assert_bootloader_boot_mode(qts, i ? 5 : 4);
        assert_bootloader_usb_identity(
            qts, true, cases[i].usb_version, cases[i].route_string, 1, 0);

        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(usb_path);
    }
}
void test_usb_msd_exclude_vid_pid(void)
{
    static const uint8_t firmware[] =
        "USB firmware behind a descriptor exclusion";
    static const struct {
        const char *config;
        const char *controller;
        const char *policy;
    } excluded_cases[] = {
        {
            "BOOT_ORDER=0xe4\n"
            "USB_MSD_DISCOVER_TIMEOUT=5000\n"
            "USB_MSD_EXCLUDE_VID_PID=00000000,46f40001,06270001,"
            "ffffffff\n",
            "xhci",
            "00000000,46f40001,06270001,ffffffff",
        },
        {
            "BOOT_ORDER=0xe5\n"
            "USB_MSD_DISCOVER_TIMEOUT=5000\n"
            "USB_MSD_EXCLUDE_VID_PID=46f40001\n",
            "dwc2",
            "46f40001",
        },
    };
    g_autofree uint8_t *usb = make_usb_boot_image(
        "START4  ELF", firmware, sizeof(firmware), NULL);

    for (unsigned int i = 0; i < ARRAY_SIZE(excluded_cases); i++) {
        g_autofree uint8_t *eeprom =
            make_eeprom_image(excluded_cases[i].config, false);
        g_autofree char *eeprom_path = NULL;
        g_autofree char *usb_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *state = NULL;
        g_autofree char *policy = NULL;
        g_autofree char *migration_dir = NULL;
        g_autofree char *migration_socket = NULL;
        QTestState *qts;

        write_temp_image("raspi4-eeprom-usb-exclude-XXXXXX", eeprom,
                         EEPROM_SIZE, &eeprom_path);
        write_temp_image("raspi4-usb-excluded-XXXXXX", usb, SD_SIZE,
                         &usb_path);
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "usb-boot-drive=usbboot,usb-boot-controller=%s "
            "-drive if=none,id=pieeprom,format=raw,file=%s,"
            "file.locking=off "
            "-drive if=none,id=usbboot,format=raw,file=%s,"
            "file.locking=off",
            excluded_cases[i].controller, eeprom_path, usb_path);
        qts = qtest_init(command);

        state = qom_get_string(qts, "boot-state");
        policy = qom_get_string(qts, "usb-msd-exclude-vid-pid");
        g_assert_cmpstr(state, ==, "usb-discovery-wait");
        g_assert_cmpstr(policy, ==, excluded_cases[i].policy);
        g_assert_cmpuint(qom_get_uint32(
                             qts, "usb-boot-excluded-device-count"),
                         ==, 1);
        g_assert_cmpuint(qom_get_uint32(
                             qts, "usb-boot-eligible-device-count"),
                         ==, 0);
        g_assert_cmphex(qom_get_uint32(
                            qts, "usb-boot-last-excluded-vid-pid"),
                        ==, 0x46f40001);
        g_assert_cmpuint(qom_get_uint64(
                             qts, "usb-boot-controller-read-commands"),
                         ==, 0);

        qtest_clock_step(qts, INT64_C(1234) * 1000 * 1000);
        if (i == 0) {
            QTestState *destination = migrate_to_new_qtest(
                qts, command, &migration_dir, &migration_socket);

            qtest_quit(qts);
            qts = destination;
            g_clear_pointer(&state, g_free);
            state = qom_get_string(qts, "boot-state");
            g_assert_cmpstr(state, ==, "usb-discovery-wait");
            g_assert_cmpuint(qom_get_uint32(
                                 qts, "usb-boot-excluded-device-count"),
                             ==, 1);
            g_assert_cmphex(qom_get_uint32(
                                qts,
                                "usb-boot-last-excluded-vid-pid"),
                            ==, 0x46f40001);
            unlink(migration_socket);
            rmdir(migration_dir);
        }
        qtest_clock_step(qts, INT64_C(3766) * 1000 * 1000);
        g_clear_pointer(&state, g_free);
        state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(state, ==, "stopped");

        qtest_system_reset(qts);
        g_clear_pointer(&state, g_free);
        state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(state, ==, "usb-discovery-wait");
        g_assert_cmpuint(qom_get_uint32(
                             qts, "usb-boot-excluded-device-count"),
                         ==, 1);

        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(usb_path);
    }

    {
        g_autofree uint8_t *eeprom = make_eeprom_image(
            "BOOT_ORDER=0x4\n"
            "USB_MSD_EXCLUDE_VID_PID=06270001\n", false);
        g_autofree char *eeprom_path = NULL;
        g_autofree char *usb_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *state = NULL;
        g_autofree char *hash = g_compute_checksum_for_data(
            G_CHECKSUM_SHA256, firmware, sizeof(firmware));
        g_autofree char *actual_hash = NULL;
        QTestState *qts;

        write_temp_image("raspi4-eeprom-usb-fallback-XXXXXX", eeprom,
                         EEPROM_SIZE, &eeprom_path);
        write_temp_image("raspi4-usb-fallback-XXXXXX", usb, SD_SIZE,
                         &usb_path);
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "usb-boot-external=on,usb-boot-controller=xhci "
            "-drive if=none,id=pieeprom,format=raw,file=%s,"
            "file.locking=off "
            "-drive if=none,id=usbboot,format=raw,file=%s,"
            "file.locking=off "
            "-device usb-kbd,bus=vl805.0,port=1 "
            "-device usb-storage,bus=vl805.0,port=2,drive=usbboot,"
            "removable=on",
            eeprom_path, usb_path);
        qts = qtest_init(command);

        state = qom_get_string(qts, "boot-state");
        actual_hash = qom_get_string(qts, "firmware-sha256");
        g_assert_cmpstr(state, ==, "arm-handoff-ready");
        g_assert_cmpstr(actual_hash, ==, hash);
        g_assert_cmpuint(qom_get_uint32(
                             qts, "usb-boot-excluded-device-count"),
                         ==, 1);
        g_assert_cmpuint(qom_get_uint32(
                             qts, "usb-boot-eligible-device-count"),
                         ==, 1);
        g_assert_cmpuint(qom_get_uint32(
                             qts, "usb-boot-selected-index"), ==, 0);
        g_assert_cmpuint(qom_get_uint32(
                             qts, "usb-boot-selected-device"), ==, 1);
        g_assert_cmphex(qom_get_uint32(
                            qts, "usb-boot-last-excluded-vid-pid"),
                        ==, 0x06270001);

        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(usb_path);
    }

    {
        g_autofree uint8_t *eeprom = make_eeprom_image(
            "BOOT_ORDER=0xe4\n"
            "USB_MSD_DISCOVER_TIMEOUT=5000\n"
            "USB_MSD_EXCLUDE_VID_PID=040955aa\n", false);
        g_autofree char *eeprom_path = NULL;
        g_autofree char *usb_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *state = NULL;
        QTestState *qts;

        write_temp_image("raspi4-eeprom-usb-hub-exclude-XXXXXX", eeprom,
                         EEPROM_SIZE, &eeprom_path);
        write_temp_image("raspi4-usb-behind-hub-XXXXXX", usb, SD_SIZE,
                         &usb_path);
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "usb-boot-external=on,usb-boot-controller=xhci "
            "-drive if=none,id=pieeprom,format=raw,file=%s,"
            "file.locking=off "
            "-drive if=none,id=usbboot,format=raw,file=%s,"
            "file.locking=off "
            "-device usb-hub,id=excludedhub,bus=vl805.0,port=1 "
            "-device usb-storage,bus=vl805.0,port=1.1,"
            "drive=usbboot,removable=on",
            eeprom_path, usb_path);
        qts = qtest_init(command);

        state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(state, ==, "usb-discovery-wait");
        g_assert_cmpuint(qom_get_uint32(
                             qts, "usb-boot-excluded-device-count"),
                         ==, 1);
        g_assert_cmpuint(qom_get_uint32(
                             qts, "usb-boot-eligible-device-count"),
                         ==, 0);
        g_assert_cmpuint(qom_get_uint64(
                             qts, "usb-boot-controller-read-commands"),
                         ==, 0);

        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(usb_path);
    }
}
void test_usb_bot_reset_recovery(void)
{
    static const uint8_t firmware[] =
        "unchanged firmware after BOT reset recovery";
    static const struct {
        const char *config;
        const char *controller;
        const char *source;
        const char *fault;
        uint64_t recoveries;
        uint64_t phase_errors;
        uint64_t cases_tested;
        bool migrate;
        uint8_t usb_version;
        uint8_t route_string;
    } cases[] = {
        {
            "BOOT_ORDER=0x4\n", "auto", "usb-msd",
            "usb-boot-bot-stall-count=4", 4, 0, 0, true, 3, 0,
        },
        {
            "BOOT_ORDER=0x5\n", "dwc2", "bcm-usb-msd",
            "usb-boot-bot-stall-count=4", 4, 0, 0, false, 2, 1,
        },
        {
            "BOOT_ORDER=0x4\n", "auto", "usb-msd",
            "usb-boot-bot-phase-count=6", 6, 6, 0, true, 3, 0,
        },
        {
            "BOOT_ORDER=0x5\n", "dwc2", "bcm-usb-msd",
            "usb-boot-bot-phase-count=6", 6, 6, 0, false, 2, 1,
        },
        {
            "BOOT_ORDER=0x4\n", "auto", "usb-msd",
            "usb-boot-bot-case-count=13", 6, 6, 13, true, 3, 0,
        },
        {
            "BOOT_ORDER=0x5\n", "dwc2", "bcm-usb-msd",
            "usb-boot-bot-case-count=13", 6, 6, 13, false, 2, 1,
        },
    };
    g_autofree char *expected_hash = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, firmware, sizeof(firmware));

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree uint8_t *eeprom =
            make_eeprom_image(cases[i].config, false);
        g_autofree uint8_t *usb = make_usb_boot_image(
            "START4  ELF", firmware, sizeof(firmware), NULL);
        g_autofree char *eeprom_path = NULL;
        g_autofree char *usb_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *state = NULL;
        g_autofree char *source = NULL;
        g_autofree char *hash = NULL;
        g_autofree char *migration_dir = NULL;
        g_autofree char *migration_socket = NULL;
        g_autofree char *usb_after = NULL;
        gsize usb_after_size = 0;
        QTestState *qts;

        write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                         &eeprom_path);
        write_temp_image("raspi4-usb-bot-recovery-XXXXXX", usb, SD_SIZE,
                         &usb_path);
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "usb-boot-drive=usbboot,usb-boot-controller=%s,"
            "%s "
            "-drive if=none,id=pieeprom,format=raw,file=%s,"
            "file.locking=off "
            "-drive if=none,id=usbboot,format=raw,file=%s,"
            "file.locking=off",
            cases[i].controller, cases[i].fault, eeprom_path, usb_path);
        qts = qtest_init(command);

        state = qom_get_string(qts, "boot-state");
        source = qom_get_string(qts, "boot-source");
        hash = qom_get_string(qts, "firmware-sha256");
        g_assert_cmpuint(qom_get_uint64(qts, "usb-boot-bot-recoveries"),
                         ==, cases[i].recoveries);
        g_assert_cmpuint(qom_get_uint64(
                             qts, "usb-boot-bot-phase-errors"),
                         ==, cases[i].phase_errors);
        g_assert_cmpuint(qom_get_uint64(
                             qts, "usb-boot-bot-cases-tested"),
                         ==, cases[i].cases_tested);
        g_assert_cmpuint(qom_get_uint64(
                             qts, "usb-boot-controller-read-commands"),
                         >, 0);
        assert_bootloader_usb_identity(
            qts, true, cases[i].usb_version, cases[i].route_string, 1, 0);
        g_assert_cmpstr(state, ==, "arm-handoff-ready");
        g_assert_cmpstr(source, ==, cases[i].source);
        g_assert_cmpstr(hash, ==, expected_hash);

        if (cases[i].migrate) {
            QTestState *destination = migrate_to_new_qtest(
                qts, command, &migration_dir, &migration_socket);

            qtest_quit(qts);
            qts = destination;
            g_assert_cmpuint(qom_get_uint64(
                                 qts, "usb-boot-bot-recoveries"),
                             ==, cases[i].recoveries);
            g_assert_cmpuint(qom_get_uint64(
                                 qts, "usb-boot-bot-phase-errors"),
                             ==, cases[i].phase_errors);
            g_assert_cmpuint(qom_get_uint64(
                                 qts, "usb-boot-bot-cases-tested"),
                             ==, cases[i].cases_tested);
            g_clear_pointer(&state, g_free);
            g_clear_pointer(&source, g_free);
            g_clear_pointer(&hash, g_free);
            state = qom_get_string(qts, "boot-state");
            source = qom_get_string(qts, "boot-source");
            hash = qom_get_string(qts, "firmware-sha256");
            g_assert_cmpstr(state, ==, "arm-handoff-ready");
            g_assert_cmpstr(source, ==, cases[i].source);
            g_assert_cmpstr(hash, ==, expected_hash);
            unlink(migration_socket);
            rmdir(migration_dir);
        }

        qtest_quit(qts);
        g_assert_true(g_file_get_contents(
            usb_path, &usb_after, &usb_after_size, NULL));
        g_assert_cmpuint(usb_after_size, ==, SD_SIZE);
        g_assert_cmpmem(usb_after, usb_after_size, usb, SD_SIZE);
        unlink(eeprom_path);
        unlink(usb_path);
    }
}
void test_usb_boot_controller_mode_split(void)
{
    static const uint8_t firmware[] = "must not cross USB controllers";
    static const struct {
        const char *config;
        const char *controller;
        const char *source;
        const char *transport;
    } cases[] = {
        {
            "BOOT_ORDER=0x5\nUSB_MSD_LUN_TIMEOUT=100\n",
            "xhci", "bcm-usb-msd",
            "vl805-xhci-host-bot-scsi-read10-v1",
        },
        {
            "BOOT_ORDER=0x4\nUSB_MSD_LUN_TIMEOUT=100\n",
            "dwc2", "usb-msd", "dwc2-host-bot-scsi-read10-v1",
        },
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree char *eeprom_path = NULL;
        g_autofree char *usb_path = NULL;
        g_autofree char *state = NULL;
        g_autofree char *source = NULL;
        g_autofree char *status = NULL;
        g_autofree char *transport = NULL;
        QTestState *qts = start_with_eeprom_and_usb_controller(
            cases[i].config, "START4  ELF", firmware, sizeof(firmware),
            cases[i].controller, &eeprom_path, &usb_path);

        state = qom_get_string(qts, "boot-state");
        source = qom_get_string(qts, "boot-source");
        status = qom_get_string(qts, "firmware-status");
        transport = qom_get_string(qts, "usb-boot-transport");
        g_assert_cmpstr(state, ==, "usb-lun-wait");
        g_assert_cmpstr(source, ==, cases[i].source);
        g_assert_cmpstr(status, ==, "usb-controller-unavailable");
        g_assert_cmpstr(transport, ==, cases[i].transport);
        g_assert_cmpuint(qom_get_uint64(
                             qts, "usb-boot-controller-read-commands"),
                         ==, 0);

        qtest_clock_step(qts, INT64_C(100) * 1000 * 1000);
        g_clear_pointer(&state, g_free);
        state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(state, ==, "fatal-error-reboot-wait");
        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(usb_path);
    }
}
void test_usb_external_mass_storage_boot(void)
{
    static const uint8_t firmware[] =
        "externally attached unchanged USB boot firmware";
    static const struct {
        const char *config;
        const char *controller;
        const char *bus;
        const char *source;
        const char *transport;
        uint8_t usb_version;
        uint8_t route_string;
    } cases[] = {
        {
            "BOOT_ORDER=0x4\n", "xhci", "vl805.0", "usb-msd",
            "vl805-xhci-host-bot-scsi-read10-v1", 3, 0,
        },
        {
            "BOOT_ORDER=0x5\n", "dwc2", "usb-bus.0", "bcm-usb-msd",
            "dwc2-host-bot-scsi-read10-v1", 2, 1,
        },
    };
    g_autofree char *expected_hash = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, firmware, sizeof(firmware));

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree uint8_t *eeprom =
            make_eeprom_image(cases[i].config, false);
        g_autofree uint8_t *usb = make_usb_boot_image(
            "START4  ELF", firmware, sizeof(firmware), NULL);
        g_autofree char *eeprom_path = NULL;
        g_autofree char *usb_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *state = NULL;
        g_autofree char *source = NULL;
        g_autofree char *hash = NULL;
        g_autofree char *transport = NULL;
        g_autofree char *migration_dir = NULL;
        g_autofree char *migration_socket = NULL;
        QTestState *qts;

        write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                         &eeprom_path);
        write_temp_image("raspi4-usb-external-XXXXXX", usb, SD_SIZE,
                         &usb_path);
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "usb-boot-external=on,usb-boot-controller=%s "
            "-drive if=none,id=pieeprom,format=raw,file=%s,"
            "file.locking=off "
            "-drive if=none,id=usbboot,format=raw,file=%s,"
            "file.locking=off "
            "-device usb-storage,bus=%s,drive=usbboot,removable=on",
            cases[i].controller, eeprom_path, usb_path, cases[i].bus);
        qts = qtest_init(command);

        state = qom_get_string(qts, "boot-state");
        source = qom_get_string(qts, "boot-source");
        hash = qom_get_string(qts, "firmware-sha256");
        transport = qom_get_string(qts, "usb-boot-transport");
        g_assert_cmpstr(state, ==, "arm-handoff-ready");
        g_assert_cmpstr(source, ==, cases[i].source);
        g_assert_cmpstr(hash, ==, expected_hash);
        g_assert_cmpstr(transport, ==, cases[i].transport);
        g_assert_cmpuint(qom_get_uint32(
                             qts, "usb-boot-selected-index"), ==, 0);
        g_assert_cmpuint(qom_get_uint32(
                             qts, "usb-boot-selected-device"), ==, 0);
        g_assert_cmpuint(qom_get_uint32(
                             qts, "usb-boot-selected-lun"), ==, 0);
        g_assert_cmpuint(qom_get_uint64(
                             qts, "usb-boot-controller-read-commands"),
                         >, 0);
        assert_bootloader_usb_identity(
            qts, true, cases[i].usb_version, cases[i].route_string, 1, 0);
        if (i == 0) {
            QTestState *destination = migrate_to_new_qtest(
                qts, command, &migration_dir, &migration_socket);

            qtest_quit(qts);
            qts = destination;
            g_assert_cmpuint(qom_get_uint32(
                                 qts, "usb-boot-selected-index"), ==, 0);
            g_assert_cmpuint(qom_get_uint32(
                                 qts, "usb-boot-selected-device"), ==, 0);
            g_assert_cmpuint(qom_get_uint32(
                                 qts, "usb-boot-selected-lun"), ==, 0);
            assert_bootloader_usb_identity(qts, true, 3, 0, 1, 0);
            g_clear_pointer(&hash, g_free);
            hash = qom_get_string(qts, "firmware-sha256");
            g_assert_cmpstr(hash, ==, expected_hash);
            unlink(migration_socket);
            rmdir(migration_dir);
        }
        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(usb_path);
    }
}
void test_usb_mass_storage_multiple_devices(void)
{
    static const uint8_t first_firmware[] = "first USB-MSD firmware";
    static const uint8_t second_firmware[] = "second USB-MSD firmware";
    static const struct {
        bool first_bootable;
        uint8_t selected;
        uint8_t usb_version;
        uint8_t route_string;
        uint8_t root_hub_port;
    } cases[] = {
        { true, 0, 3, 0, 1 },
        { false, 1, 2, 1, 2 },
    };
    g_autofree uint8_t *eeprom = make_eeprom_image(
        "BOOT_ORDER=0x4\n", false);
    g_autofree uint8_t *first_valid = make_usb_boot_image(
        "START4  ELF", first_firmware, sizeof(first_firmware), NULL);
    g_autofree uint8_t *first_invalid = make_usb_boot_image(
        "START4  ELF", NULL, 0, NULL);
    g_autofree uint8_t *second = make_usb_boot_image(
        "START4  ELF", second_firmware, sizeof(second_firmware), NULL);
    g_autofree char *first_hash = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, first_firmware, sizeof(first_firmware));
    g_autofree char *second_hash = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, second_firmware, sizeof(second_firmware));

    for (unsigned int i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree char *eeprom_path = NULL;
        g_autofree char *first_path = NULL;
        g_autofree char *second_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *hash = NULL;
        g_autofree char *drives = NULL;
        g_autofree char *qtree = NULL;
        g_autofree char *migration_dir = NULL;
        g_autofree char *migration_socket = NULL;
        const char *cursor;
        unsigned int device_count = 0;
        uint64_t controller_reads;
        uint64_t controller_bytes;
        QTestState *qts;

        write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                         &eeprom_path);
        write_temp_image(
            "raspi4-usb-first-XXXXXX",
            cases[i].first_bootable ? first_valid
                                    : first_invalid,
            SD_SIZE, &first_path);
        write_temp_image("raspi4-usb-second-XXXXXX", second, SD_SIZE,
                         &second_path);
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "usb-boot-drives=usb0:usb1 "
            "-drive if=none,id=pieeprom,format=raw,file=%s,"
            "file.locking=off "
            "-drive if=none,id=usb0,format=raw,file=%s,file.locking=off "
            "-drive if=none,id=usb1,format=raw,file=%s,file.locking=off",
            eeprom_path, first_path, second_path);
        qts = qtest_init(command);
        hash = qom_get_string(qts, "firmware-sha256");
        drives = qom_get_string(qts, "usb-boot-drives");
        qtree = qtest_hmp(qts, "info qtree");
        g_assert_cmpstr(hash, ==,
                        cases[i].selected ? second_hash
                                          : first_hash);
        g_assert_cmpstr(drives, ==, "usb0:usb1");
        g_assert_cmpuint(qom_get_uint32(
                             qts, "usb-boot-selected-index"), ==,
                         cases[i].selected);
        g_assert_cmpuint(qom_get_uint32(
                             qts, "usb-boot-selected-device"), ==,
                         cases[i].selected);
        g_assert_cmpuint(qom_get_uint32(
                             qts, "usb-boot-selected-lun"), ==, 0);
        assert_bootloader_usb_identity(
            qts, true, cases[i].usb_version, cases[i].route_string,
            cases[i].root_hub_port, 0);
        controller_reads = qom_get_uint64(
            qts, "usb-boot-controller-read-commands");
        controller_bytes = qom_get_uint64(
            qts, "usb-boot-controller-read-bytes");
        g_assert_cmpuint(controller_reads, >, 0);
        g_assert_cmpuint(controller_bytes, >=, 512);
        cursor = qtree;
        while ((cursor = strstr(cursor, "usb-storage"))) {
            device_count++;
            cursor += strlen("usb-storage");
        }
        g_assert_cmpuint(device_count, ==, 2);

        if (cases[i].selected == 1) {
            QTestState *destination = migrate_to_new_qtest(
                qts, command, &migration_dir, &migration_socket);

            qtest_quit(qts);
            qts = destination;
            g_assert_cmpuint(qom_get_uint32(
                                 qts, "usb-boot-selected-index"), ==, 1);
            g_assert_cmpuint(qom_get_uint32(
                                 qts, "usb-boot-selected-device"), ==, 1);
            g_assert_cmpuint(qom_get_uint32(
                                 qts, "usb-boot-selected-lun"), ==, 0);
            assert_bootloader_usb_identity(qts, true, 2, 1, 2, 0);
            g_assert_cmpuint(qom_get_uint64(
                                 qts,
                                 "usb-boot-controller-read-commands"),
                             ==, controller_reads);
            g_assert_cmpuint(qom_get_uint64(
                                 qts, "usb-boot-controller-read-bytes"),
                             ==, controller_bytes);
            unlink(migration_socket);
            rmdir(migration_dir);
        }
        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(first_path);
        unlink(second_path);
    }

    {
        g_autofree char *eeprom_path = NULL;
        g_autofree char *second_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *state = NULL;
        g_autofree char *hash = NULL;
        QTestState *qts;

        write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                         &eeprom_path);
        write_temp_image("raspi4-usb-second-XXXXXX", second, SD_SIZE,
                         &second_path);
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "usb-boot-drives=usb0:usb1 "
            "-drive if=none,id=pieeprom,format=raw,file=%s "
            "-drive if=none,id=usb0 -drive if=none,id=usb1",
            eeprom_path);
        qts = qtest_init(command);
        state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(state, ==, "usb-discovery-wait");
        qtest_qmp_assert_success(
            qts, "{ 'execute': 'blockdev-change-medium', 'arguments': {"
                 "'device': 'usb1', 'filename': %s, 'format': 'raw' } }",
            second_path);
        g_clear_pointer(&state, g_free);
        state = qom_get_string(qts, "boot-state");
        hash = qom_get_string(qts, "firmware-sha256");
        g_assert_cmpstr(state, ==, "arm-handoff-ready");
        g_assert_cmpstr(hash, ==, second_hash);
        g_assert_cmpuint(qom_get_uint32(
                             qts, "usb-boot-selected-index"), ==, 1);
        g_assert_cmpuint(qom_get_uint32(
                             qts, "usb-boot-selected-device"), ==, 1);
        g_assert_cmpuint(qom_get_uint32(
                             qts, "usb-boot-selected-lun"), ==, 0);
        assert_bootloader_usb_identity(qts, true, 2, 1, 2, 0);
        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(second_path);
    }

    {
        g_autofree char *eeprom_path = NULL;
        g_autofree char *first_path = NULL;
        g_autofree char *second_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *hash = NULL;
        g_autofree char *qtree = NULL;
        g_autofree char *migration_dir = NULL;
        g_autofree char *migration_socket = NULL;
        QTestState *qts;

        write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                         &eeprom_path);
        write_temp_image("raspi4-usb-lun0-XXXXXX", first_invalid, SD_SIZE,
                         &first_path);
        write_temp_image("raspi4-usb-lun1-XXXXXX", second, SD_SIZE,
                         &second_path);
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "usb-boot-drives=usb0+usb1 "
            "-drive if=none,id=pieeprom,format=raw,file=%s,"
            "file.locking=off "
            "-drive if=none,id=usb0,format=raw,file=%s,file.locking=off "
            "-drive if=none,id=usb1,format=raw,file=%s,file.locking=off",
            eeprom_path, first_path, second_path);
        qts = qtest_init(command);
        hash = qom_get_string(qts, "firmware-sha256");
        qtree = qtest_hmp(qts, "info qtree");
        g_assert_cmpstr(hash, ==, second_hash);
        g_assert_nonnull(strstr(qtree, "usb-bot"));
        g_assert_nonnull(strstr(qtree, "scsi-hd"));
        g_assert_cmpuint(qom_get_uint32(
                             qts, "usb-boot-selected-index"), ==, 1);
        g_assert_cmpuint(qom_get_uint32(
                             qts, "usb-boot-selected-device"), ==, 0);
        g_assert_cmpuint(qom_get_uint32(
                             qts, "usb-boot-selected-lun"), ==, 1);
        assert_bootloader_usb_identity(qts, true, 3, 0, 1, 1);

        {
            QTestState *destination = migrate_to_new_qtest(
                qts, command, &migration_dir, &migration_socket);

            qtest_quit(qts);
            qts = destination;
            g_assert_cmpuint(qom_get_uint32(
                                 qts, "usb-boot-selected-index"), ==, 1);
            g_assert_cmpuint(qom_get_uint32(
                                 qts, "usb-boot-selected-device"), ==, 0);
            g_assert_cmpuint(qom_get_uint32(
                                 qts, "usb-boot-selected-lun"), ==, 1);
            assert_bootloader_usb_identity(qts, true, 3, 0, 1, 1);
            unlink(migration_socket);
            rmdir(migration_dir);
        }
        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(first_path);
        unlink(second_path);
    }
}
void test_usb_controller_topology_identity(void)
{
    static const uint8_t firmware[] =
        "controller-visible USB topology firmware";
    g_autofree uint8_t *eeprom =
        make_eeprom_image("BOOT_ORDER=0x5\n", false);
    g_autofree uint8_t *invalid =
        make_usb_boot_image("START4  ELF", NULL, 0, NULL);
    g_autofree uint8_t *valid = make_usb_boot_image(
        "START4  ELF", firmware, sizeof(firmware), NULL);
    g_autofree char *eeprom_path = NULL;
    g_autofree char *lun0_path = NULL;
    g_autofree char *lun1_path = NULL;
    g_autofree char *device1_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *qtree = NULL;
    uint8_t descriptor[18] = { 0 };
    uint8_t hub_descriptor[16] = { 0 };
    unsigned int storage_devices = 0;
    QTestState *qts;

    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    write_temp_image("raspi4-usb-lun0-XXXXXX", invalid, SD_SIZE,
                     &lun0_path);
    write_temp_image("raspi4-usb-lun1-XXXXXX", invalid, SD_SIZE,
                     &lun1_path);
    write_temp_image("raspi4-usb-device1-XXXXXX", valid, SD_SIZE,
                     &device1_path);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "usb-boot-drives=usb0+usb1:usb2,usb-boot-controller=dwc2 "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=none,id=usb0,format=raw,file=%s,file.locking=off "
        "-drive if=none,id=usb1,format=raw,file=%s,file.locking=off "
        "-drive if=none,id=usb2,format=raw,file=%s,file.locking=off",
        eeprom_path, lun0_path, lun1_path, device1_path);
    qts = qtest_init(command);

    g_assert_cmpuint(qom_get_uint32(
                         qts, "usb-boot-selected-device"), ==, 1);
    g_assert_cmpuint(qom_get_uint32(
                         qts, "usb-boot-selected-lun"), ==, 0);
    assert_bootloader_usb_identity(qts, true, 2, 2, 1, 0);
    qtree = qtest_hmp(qts, "info qtree");
    g_assert_nonnull(strstr(qtree, "raspi4-usb-boot-0"));
    g_assert_nonnull(strstr(qtree, "raspi4-usb-boot-0-lun-0"));
    g_assert_nonnull(strstr(qtree, "raspi4-usb-boot-0-lun-1"));
    g_assert_nonnull(strstr(qtree, "raspi4-usb-boot-1"));
    g_assert_nonnull(strstr(qtree, "QEMU-RPI-BOOT-D0"));
    g_assert_nonnull(strstr(qtree, "QEMU-RPI-BOOT-D1"));

    raspi4_dwc2_reset_root_port(qts);
    raspi4_dwc2_get_device_descriptor(qts, 0, descriptor);
    g_assert_cmpuint(descriptor[0], ==, sizeof(descriptor));
    g_assert_cmpuint(descriptor[1], ==, USB_DT_DEVICE);
    g_assert_cmpuint(descriptor[4], ==, USB_CLASS_HUB);

    raspi4_dwc2_set_address(qts, 0, 1);
    g_assert_cmpuint(raspi4_dwc2_control(
                         qts, 1,
                         USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE,
                         USB_REQ_GET_DESCRIPTOR, 0x2900, 0,
                         hub_descriptor, sizeof(hub_descriptor)), >=, 9);
    g_assert_cmphex(hub_descriptor[1], ==, 0x29);
    g_assert_cmpuint(hub_descriptor[2], >, 1);
    g_assert_cmpuint(hub_descriptor[2], <=, 8);

    for (unsigned int port = 1; port <= hub_descriptor[2]; port++) {
        uint8_t port_status[4] = { 0 };
        uint8_t config[32] = { 0 };
        uint8_t max_lun = 0;
        uint16_t status;
        bool mass_storage = false;
        size_t config_length;

        g_assert_cmpuint(raspi4_dwc2_control(
                             qts, 1,
                             USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_OTHER,
                             USB_REQ_GET_STATUS, 0, port,
                             port_status, sizeof(port_status)),
                         ==, sizeof(port_status));
        status = lduw_le_p(port_status);
        if (!(status & 0x0001)) {
            continue;
        }

        g_assert_cmpuint(raspi4_dwc2_control(
                             qts, 1,
                             USB_TYPE_CLASS | USB_RECIP_OTHER,
                             USB_REQ_SET_FEATURE, 4, port, NULL, 0), ==, 0);
        memset(port_status, 0, sizeof(port_status));
        g_assert_cmpuint(raspi4_dwc2_control(
                             qts, 1,
                             USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_OTHER,
                             USB_REQ_GET_STATUS, 0, port,
                             port_status, sizeof(port_status)),
                         ==, sizeof(port_status));
        g_assert_true(lduw_le_p(port_status) & 0x0002);

        memset(descriptor, 0, sizeof(descriptor));
        raspi4_dwc2_get_device_descriptor(qts, 0, descriptor);
        g_assert_cmpuint(descriptor[0], ==, sizeof(descriptor));
        g_assert_cmpuint(descriptor[1], ==, USB_DT_DEVICE);
        g_assert_cmpuint(descriptor[4], ==, 0);
        raspi4_dwc2_set_address(qts, 0, port + 1);

        config_length = raspi4_dwc2_control(
            qts, port + 1,
            USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
            USB_REQ_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0,
            config, sizeof(config));
        for (size_t offset = 0; offset + 2 <= config_length &&
             config[offset] != 0; offset += config[offset]) {
            if (config[offset + 1] == USB_DT_INTERFACE &&
                offset + 9 <= config_length &&
                config[offset + 5] == USB_CLASS_MASS_STORAGE &&
                config[offset + 6] == 0x06 &&
                config[offset + 7] == 0x50) {
                mass_storage = true;
                break;
            }
        }
        g_assert_true(mass_storage);

        g_assert_cmpuint(raspi4_dwc2_control(
                             qts, port + 1,
                             USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                             USB_REQ_SET_CONFIGURATION, 1, 0, NULL, 0), ==, 0);
        g_assert_cmpuint(raspi4_dwc2_control(
                             qts, port + 1,
                             USB_DIR_IN | USB_TYPE_CLASS |
                             USB_RECIP_INTERFACE,
                             0xfe, 0, 0, &max_lun, 1), ==, 1);
        g_assert_cmpuint(max_lun, ==, port == 1 ? 1 : 0);

        for (unsigned int lun = 0; lun <= max_lun; lun++) {
            const uint8_t *expected = port == 1 ? invalid : valid;
            uint8_t inquiry_cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
            uint8_t inquiry[36] = { 0 };
            uint8_t capacity_cdb[10] = { 0x25 };
            uint8_t capacity[8] = { 0 };
            uint8_t read_cdb[10] = { 0x28 };
            uint8_t sector[512] = { 0 };
            uint32_t tag = 0x52504900 | (port << 8) | (lun << 4);

            read_cdb[8] = 1;
            raspi4_dwc2_bot_command(qts, port + 1, lun, tag,
                                    inquiry_cdb, sizeof(inquiry_cdb),
                                    inquiry, sizeof(inquiry), true);
            g_assert_cmpuint(inquiry[0] & 0x1f, ==, 0);
            g_assert_cmpuint(inquiry[4], >=, 31);
            raspi4_dwc2_bot_command(qts, port + 1, lun, tag + 1,
                                    capacity_cdb, sizeof(capacity_cdb),
                                    capacity, sizeof(capacity), false);
            memset(capacity, 0, sizeof(capacity));
            raspi4_dwc2_bot_command(qts, port + 1, lun, tag + 2,
                                    capacity_cdb, sizeof(capacity_cdb),
                                    capacity, sizeof(capacity), true);
            g_assert_cmpuint(ldl_be_p(capacity), ==, SD_SIZE / 512 - 1);
            g_assert_cmpuint(ldl_be_p(capacity + 4), ==, 512);
            raspi4_dwc2_bot_command(qts, port + 1, lun, tag + 3,
                                    read_cdb, sizeof(read_cdb),
                                    sector, sizeof(sector), true);
            g_assert_cmpmem(sector, sizeof(sector), expected, sizeof(sector));
        }
        storage_devices++;
    }
    g_assert_cmpuint(storage_devices, ==, 2);

    qtest_quit(qts);
    unlink(eeprom_path);
    unlink(lun0_path);
    unlink(lun1_path);
    unlink(device1_path);
}
void test_usb_controller_read_error_fallback(void)
{
    static const char blkdebug_config[] =
        "[inject-error]\n"
        "event = \"read_aio\"\n"
        "errno = \"5\"\n"
        "once = \"off\"\n";
    static const uint8_t usb_firmware[] =
        "unreachable USB firmware behind read failure";
    static const uint8_t sd_firmware[] =
        "SD firmware after controller-visible USB failure";
    g_autofree uint8_t *eeprom = make_eeprom_image(
        "BOOT_ORDER=0x14\nUSB_MSD_LUN_TIMEOUT=100\n", false);
    g_autofree uint8_t *usb = make_usb_boot_image(
        "START4  ELF", usb_firmware, sizeof(usb_firmware), NULL);
    g_autofree uint8_t *sd = make_usb_boot_image(
        "START4  ELF", sd_firmware, sizeof(sd_firmware), NULL);
    g_autofree char *expected_hash = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, sd_firmware, sizeof(sd_firmware));
    g_autofree char *eeprom_path = NULL;
    g_autofree char *usb_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree char *debug_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *state = NULL;
    g_autofree char *source = NULL;
    g_autofree char *hash = NULL;
    QTestState *qts;

    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    write_temp_image("raspi4-usb-XXXXXX", usb, SD_SIZE, &usb_path);
    write_temp_image("raspi4-sd-XXXXXX", sd, SD_SIZE, &sd_path);
    write_temp_text("raspi4-blkdebug-XXXXXX", blkdebug_config, &debug_path);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "usb-boot-drive=usbboot "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=none,id=usbboot,format=raw,"
        "file=blkdebug:%s:%s "
        "-drive if=sd,format=raw,file=%s,file.locking=off -nic none",
        eeprom_path, debug_path, usb_path, sd_path);
    qts = qtest_init(command);

    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "usb-lun-wait");
    g_assert_cmpuint(qom_get_uint64(
                         qts, "usb-boot-controller-command-failures"), >, 0);
    qtest_clock_step(qts, INT64_C(100) * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "boot-source");
    hash = qom_get_string(qts, "firmware-sha256");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(source, ==, "sd-card");
    g_assert_cmpstr(hash, ==, expected_hash);
    g_assert_cmpuint(qom_get_uint64(qts, "boot-attempt-count"), ==, 2);
    g_assert_cmpuint(qom_get_uint32(qts, "boot-order-index"), ==, 1);
    g_assert_cmpuint(qom_get_uint64(
                         qts, "usb-boot-controller-command-failures"), >, 0);

    qtest_quit(qts);
    unlink(eeprom_path);
    unlink(usb_path);
    unlink(sd_path);
    unlink(debug_path);
}
void test_nvme_boot(void)
{
    static const uint8_t firmware[] = "byte-identical NVMe boot firmware";
    g_autofree uint8_t *eeprom = make_eeprom_image(
        "BOOT_ORDER=0xf6\n", false);
    g_autofree uint8_t *nvme = make_usb_boot_image(
        "START4  ELF", firmware, sizeof(firmware), NULL);
    g_autofree char *expected_hash = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, firmware, sizeof(firmware));
    g_autofree char *eeprom_path = NULL;
    g_autofree char *nvme_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *state = NULL;
    g_autofree char *source = NULL;
    g_autofree char *firmware_hash = NULL;
    g_autofree char *drive = NULL;
    g_autofree char *qtree = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    QTestState *qts;
    QTestState *destination;

    write_temp_image("raspi4-nvme-XXXXXX", nvme, SD_SIZE, &nvme_path);
    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "nvme-drive=nvmeboot "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=none,id=nvmeboot,format=raw,file=%s,file.locking=off",
        eeprom_path, nvme_path);
    qts = qtest_init(command);

    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "boot-source");
    firmware_hash = qom_get_string(qts, "firmware-sha256");
    drive = qom_get_string(qts, "nvme-drive");
    qtree = qtest_hmp(qts, "info qtree");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(source, ==, "nvme");
    g_assert_cmpstr(firmware_hash, ==, expected_hash);
    g_assert_cmpstr(drive, ==, "nvmeboot");
    g_assert_nonnull(strstr(qtree, "dev: nvme"));
    g_assert_cmpuint(qom_get_uint64(qts, "boot-attempt-count"), ==, 1);
    g_assert_cmpuint(qom_get_uint32(qts, "boot-order-index"), ==, 0);
    assert_bootloader_boot_mode(qts, 6);

    destination = migrate_to_new_qtest(qts, command, &migration_dir,
                                       &migration_socket);
    g_clear_pointer(&state, g_free);
    g_clear_pointer(&source, g_free);
    state = qom_get_string(destination, "boot-state");
    source = qom_get_string(destination, "boot-source");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(source, ==, "nvme");
    g_assert_cmpuint(qom_get_uint64(destination, "boot-attempt-count"), ==, 1);
    assert_bootloader_boot_mode(destination, 6);
    qtest_quit(destination);
    qtest_quit(qts);
    unlink(migration_socket);
    rmdir(migration_dir);
    unlink(eeprom_path);
    unlink(nvme_path);

    {
        g_autofree char *fallback_eeprom_path = NULL;
        g_autofree char *sd_path = NULL;
        g_autofree char *fallback_state = NULL;
        g_autofree char *fallback_source = NULL;

        qts = start_with_eeprom_and_sd(
            "BOOT_ORDER=0xf16\n", "START4  ELF", firmware,
            sizeof(firmware), NULL, &fallback_eeprom_path, &sd_path);
        fallback_state = qom_get_string(qts, "boot-state");
        fallback_source = qom_get_string(qts, "boot-source");
        g_assert_cmpstr(fallback_state, ==, "arm-handoff-ready");
        g_assert_cmpstr(fallback_source, ==, "sd-card");
        g_assert_cmpuint(qom_get_uint64(qts, "boot-attempt-count"), ==, 2);
        g_assert_cmpuint(qom_get_uint32(qts, "boot-order-index"), ==, 1);
        qtest_quit(qts);
        unlink(fallback_eeprom_path);
        unlink(sd_path);
    }
}
void test_nvme_present_failure_fallback(void)
{
    static const char blkdebug_config[] =
        "[inject-error]\n"
        "event = \"read_aio\"\n"
        "errno = \"5\"\n"
        "once = \"off\"\n";
    static const uint8_t firmware[] =
        "SD firmware after present but unbootable NVMe";
    g_autofree uint8_t *eeprom = make_eeprom_image(
        "BOOT_ORDER=0x16\n", false);
    g_autofree uint8_t *nvme = g_malloc0(SD_SIZE);
    g_autofree uint8_t *sd = make_usb_boot_image(
        "START4  ELF", firmware, sizeof(firmware), NULL);
    g_autofree char *expected_hash = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, firmware, sizeof(firmware));
    g_autofree char *eeprom_path = NULL;
    g_autofree char *nvme_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree char *debug_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *state = NULL;
    g_autofree char *source = NULL;
    g_autofree char *firmware_hash = NULL;
    g_autofree char *qtree = NULL;
    QTestState *qts;

    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    write_temp_image("raspi4-nvme-XXXXXX", nvme, SD_SIZE, &nvme_path);
    write_temp_image("raspi4-sd-XXXXXX", sd, SD_SIZE, &sd_path);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "nvme-drive=nvmeboot "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=none,id=nvmeboot,format=raw,file=%s,file.locking=off "
        "-drive if=sd,format=raw,file=%s,file.locking=off -nic none",
        eeprom_path, nvme_path, sd_path);
    qts = qtest_init(command);

    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "boot-source");
    firmware_hash = qom_get_string(qts, "firmware-sha256");
    qtree = qtest_hmp(qts, "info qtree");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(source, ==, "sd-card");
    g_assert_cmpstr(firmware_hash, ==, expected_hash);
    g_assert_nonnull(strstr(qtree, "dev: nvme"));
    g_assert_cmpuint(qom_get_uint64(qts, "boot-attempt-count"), ==, 2);
    g_assert_cmpuint(qom_get_uint32(qts, "boot-order-index"), ==, 1);

    qtest_quit(qts);

    /*
     * Keep the namespace and valid boot bytes present, but make every backend
     * read fail.  BOOT_ORDER must still advance to SD rather than confusing
     * an I/O failure with successful NVMe firmware discovery.
     */
    overwrite_image(nvme_path, sd, SD_SIZE);
    write_temp_text("raspi4-blkdebug-XXXXXX", blkdebug_config, &debug_path);
    g_clear_pointer(&command, g_free);
    g_clear_pointer(&state, g_free);
    g_clear_pointer(&source, g_free);
    g_clear_pointer(&firmware_hash, g_free);
    g_clear_pointer(&qtree, g_free);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "nvme-drive=nvmeboot "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=none,id=nvmeboot,format=raw,"
        "file=blkdebug:%s:%s "
        "-drive if=sd,format=raw,file=%s,file.locking=off -nic none",
        eeprom_path, debug_path, nvme_path, sd_path);
    qts = qtest_init(command);
    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "boot-source");
    firmware_hash = qom_get_string(qts, "firmware-sha256");
    qtree = qtest_hmp(qts, "info qtree");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(source, ==, "sd-card");
    g_assert_cmpstr(firmware_hash, ==, expected_hash);
    g_assert_nonnull(strstr(qtree, "dev: nvme"));
    g_assert_cmpuint(qom_get_uint64(qts, "boot-attempt-count"), ==, 2);
    g_assert_cmpuint(qom_get_uint32(qts, "boot-order-index"), ==, 1);
    raspi4_assert_nvme_rw_statuses(
        qts, NVME_CMD_READ, NULL, NULL, NULL,
        NVME_UNRECOVERED_READ, NVME_UNRECOVERED_READ);
    qtest_quit(qts);

    unlink(eeprom_path);
    unlink(nvme_path);
    unlink(sd_path);
    unlink(debug_path);
}
void test_nvme_command_status_fault_count(void)
{
    static const char blkdebug_config[] =
        "[inject-error]\n"
        "event = \"read_aio\"\n"
        "errno = \"5\"\n"
        "once = \"on\"\n";
    static const uint8_t firmware[] =
        "SD firmware beside bounded NVMe status fault";
    g_autofree uint8_t *eeprom =
        make_eeprom_image("BOOT_ORDER=0x1\n", false);
    g_autofree uint8_t *media = make_usb_boot_image(
        "START4  ELF", firmware, sizeof(firmware), NULL);
    g_autofree char *eeprom_path = NULL;
    g_autofree char *nvme_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree char *debug_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *state = NULL;
    g_autofree char *source = NULL;
    QTestState *qts;

    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    write_temp_image("raspi4-nvme-XXXXXX", media, SD_SIZE, &nvme_path);
    write_temp_image("raspi4-sd-XXXXXX", media, SD_SIZE, &sd_path);
    write_temp_text("raspi4-blkdebug-XXXXXX", blkdebug_config, &debug_path);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "nvme-drive=nvmeboot "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=none,id=nvmeboot,format=raw,"
        "file=blkdebug:%s:%s "
        "-drive if=sd,format=raw,file=%s,file.locking=off -nic none",
        eeprom_path, debug_path, nvme_path, sd_path);
    qts = qtest_init(command);
    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "boot-source");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(source, ==, "sd-card");
    raspi4_assert_nvme_rw_statuses(
        qts, NVME_CMD_READ, NULL, NULL, NULL,
        NVME_UNRECOVERED_READ, NVME_SUCCESS);
    qtest_quit(qts);

    unlink(eeprom_path);
    unlink(nvme_path);
    unlink(sd_path);
    unlink(debug_path);
}
void test_nvme_write_status_and_durability(void)
{
    static const struct {
        const char *once;
        uint16_t second_status;
        bool second_write_durable;
    } cases[] = {
        { "off", NVME_WRITE_FAULT, false },
        { "on", NVME_SUCCESS, true },
    };
    static const uint8_t firmware[] =
        "SD firmware beside bounded NVMe write fault";
    g_autofree uint8_t *eeprom =
        make_eeprom_image("BOOT_ORDER=0x1\n", false);
    g_autofree uint8_t *sd = make_usb_boot_image(
        "START4  ELF", firmware, sizeof(firmware), NULL);
    uint8_t payload[512];
    uint8_t initial[512];
    uint8_t persisted[512];

    memset(initial, 0x3c, sizeof(initial));
    for (unsigned int i = 0; i < sizeof(payload); i++) {
        payload[i] = i ^ 0xa5;
    }

    for (unsigned int i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree uint8_t *nvme = g_malloc0(SD_SIZE);
        g_autofree char *blkdebug_config = g_strdup_printf(
            "[inject-error]\n"
            "event = \"write_aio\"\n"
            "errno = \"5\"\n"
            "once = \"%s\"\n", cases[i].once);
        g_autofree char *eeprom_path = NULL;
        g_autofree char *nvme_path = NULL;
        g_autofree char *sd_path = NULL;
        g_autofree char *debug_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *state = NULL;
        g_autofree char *source = NULL;
        const uint8_t *expected = initial;
        QTestState *qts;
        int fd;

        if (cases[i].second_write_durable) {
            expected = payload;
        }
        memcpy(nvme, initial, sizeof(initial));
        write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                         &eeprom_path);
        write_temp_image("raspi4-nvme-XXXXXX", nvme, SD_SIZE, &nvme_path);
        write_temp_image("raspi4-sd-XXXXXX", sd, SD_SIZE, &sd_path);
        write_temp_text("raspi4-blkdebug-XXXXXX", blkdebug_config,
                        &debug_path);
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "nvme-drive=nvmeboot "
            "-drive if=none,id=pieeprom,format=raw,file=%s,"
            "file.locking=off "
            "-drive if=none,id=nvmeboot,format=raw,"
            "file=blkdebug:%s:%s "
            "-drive if=sd,format=raw,file=%s,file.locking=off -nic none",
            eeprom_path, debug_path, nvme_path, sd_path);
        qts = qtest_init(command);
        state = qom_get_string(qts, "boot-state");
        source = qom_get_string(qts, "boot-source");
        g_assert_cmpstr(state, ==, "arm-handoff-ready");
        g_assert_cmpstr(source, ==, "sd-card");
        raspi4_assert_nvme_rw_statuses(
            qts, NVME_CMD_WRITE, NULL, payload, payload,
            NVME_WRITE_FAULT, cases[i].second_status);
        qtest_quit(qts);

        fd = open(nvme_path, O_RDONLY);
        g_assert_cmpint(fd, >=, 0);
        g_assert_cmpint(pread(fd, persisted, sizeof(persisted), 0), ==,
                        sizeof(persisted));
        close(fd);
        g_assert_cmpmem(persisted, sizeof(persisted),
                        expected, sizeof(payload));

        unlink(eeprom_path);
        unlink(nvme_path);
        unlink(sd_path);
        unlink(debug_path);
    }
}
void test_nvme_flush_status_fault_count(void)
{
    static const char blkdebug_config[] =
        "[inject-error]\n"
        "event = \"none\"\n"
        "iotype = \"flush\"\n"
        "errno = \"5\"\n"
        "once = \"on\"\n";
    static const uint8_t firmware[] =
        "SD firmware beside bounded NVMe flush fault";
    g_autofree uint8_t *eeprom =
        make_eeprom_image("BOOT_ORDER=0x1\n", false);
    g_autofree uint8_t *media = make_usb_boot_image(
        "START4  ELF", firmware, sizeof(firmware), NULL);
    g_autofree char *eeprom_path = NULL;
    g_autofree char *nvme_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree char *debug_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *state = NULL;
    g_autofree char *source = NULL;
    QTestState *qts;

    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    write_temp_image("raspi4-nvme-XXXXXX", media, SD_SIZE, &nvme_path);
    write_temp_image("raspi4-sd-XXXXXX", media, SD_SIZE, &sd_path);
    write_temp_text("raspi4-blkdebug-XXXXXX", blkdebug_config, &debug_path);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "nvme-drive=nvmeboot "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=none,id=nvmeboot,format=raw,cache=writeback,"
        "file=blkdebug:%s:%s "
        "-drive if=sd,format=raw,file=%s,file.locking=off -nic none",
        eeprom_path, debug_path, nvme_path, sd_path);
    qts = qtest_init(command);
    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "boot-source");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(source, ==, "sd-card");
    raspi4_assert_nvme_rw_statuses(
        qts, NVME_CMD_FLUSH, media, NULL, NULL,
        NVME_WRITE_FAULT, NVME_SUCCESS);
    qtest_quit(qts);

    unlink(eeprom_path);
    unlink(nvme_path);
    unlink(sd_path);
    unlink(debug_path);
}
void test_nvme_compare_status(void)
{
    static const uint8_t firmware[] =
        "SD firmware beside NVMe compare status";
    g_autofree uint8_t *eeprom =
        make_eeprom_image("BOOT_ORDER=0x1\n", false);
    g_autofree uint8_t *sd = make_usb_boot_image(
        "START4  ELF", firmware, sizeof(firmware), NULL);
    g_autofree uint8_t *nvme = g_malloc0(SD_SIZE);
    g_autofree char *eeprom_path = NULL;
    g_autofree char *nvme_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *state = NULL;
    g_autofree char *source = NULL;
    uint8_t expected[512];
    uint8_t mismatch[512];
    uint8_t persisted[512];
    QTestState *qts;
    int fd;

    for (unsigned int i = 0; i < sizeof(expected); i++) {
        expected[i] = i ^ 0x69;
        mismatch[i] = expected[i] ^ 0xff;
    }
    memcpy(nvme, expected, sizeof(expected));
    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    write_temp_image("raspi4-nvme-XXXXXX", nvme, SD_SIZE, &nvme_path);
    write_temp_image("raspi4-sd-XXXXXX", sd, SD_SIZE, &sd_path);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "nvme-drive=nvmeboot "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=none,id=nvmeboot,format=raw,file=%s,file.locking=off "
        "-drive if=sd,format=raw,file=%s,file.locking=off -nic none",
        eeprom_path, nvme_path, sd_path);
    qts = qtest_init(command);
    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "boot-source");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(source, ==, "sd-card");
    raspi4_assert_nvme_rw_statuses(
        qts, NVME_CMD_COMPARE, NULL, mismatch, expected,
        NVME_CMP_FAILURE | NVME_DNR, NVME_SUCCESS);
    qtest_quit(qts);

    fd = open(nvme_path, O_RDONLY);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(pread(fd, persisted, sizeof(persisted), 0), ==,
                    sizeof(persisted));
    close(fd);
    g_assert_cmpmem(persisted, sizeof(persisted),
                    expected, sizeof(expected));

    unlink(eeprom_path);
    unlink(nvme_path);
    unlink(sd_path);
}
void test_boot_filesystem_self_update(void)
{
    static const uint8_t firmware[] = "firmware after EEPROM self-update";
    const uint32_t update_timestamp = 1700000123;
    g_autofree uint8_t *initial = make_eeprom_image(
        "BOOT_ORDER=0x1\nENABLE_SELF_UPDATE=1\n", false);
    g_autofree uint8_t *update = make_eeprom_image(
        "BOOT_ORDER=0x1\nENABLE_SELF_UPDATE=1\nBOOTVAR0=0x1234\n", false);
    g_autofree char *digest = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, update, EEPROM_SIZE);
    g_autofree char *signature = g_strdup_printf(
        "%s\nts: %u\n", digest, update_timestamp);
    g_autofree uint8_t *sd = make_usb_boot_image_with_update(
        "START4  ELF", firmware, sizeof(firmware), NULL,
        update, signature);
    g_autofree char *eeprom_path = NULL;
    g_autofree char *eeprom_status_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *state = NULL;
    g_autofree char *status = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    uint8_t persisted[EEPROM_SIZE];
    uint8_t persisted_status[512] = { 0 };
    QTestState *destination;
    QTestState *source;
    int fd;

    write_temp_image("raspi4-eeprom-self-update-XXXXXX",
                     initial, EEPROM_SIZE, &eeprom_path);
    write_temp_image("raspi4-eeprom-self-status-XXXXXX",
                     persisted_status, sizeof(persisted_status),
                     &eeprom_status_path);
    write_temp_image("raspi4-sd-self-update-XXXXXX",
                     sd, SD_SIZE, &sd_path);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "eeprom-status-drive=eepromstatus "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=none,id=eepromstatus,format=raw,file=%s,"
        "file.locking=off "
        "-drive if=sd,format=raw,file=%s,file.locking=off -nic none",
        eeprom_path, eeprom_status_path, sd_path);
    source = qtest_init(command);
    state = qom_get_string(source, "boot-state");
    status = qom_get_string(source, "boot-self-update-status");
    g_assert_cmpstr(state, ==, "self-update-updated-reboot");
    g_assert_cmpstr(status, ==, "updated-reboot");
    g_assert_true(qom_get_bool(source, "boot-enable-self-update"));
    g_assert_false(qom_get_bool(source, "boot-freeze-version"));
    g_assert_true(qom_get_bool(
                      source, "boot-eeprom-update-timestamp-valid"));
    g_assert_cmpuint(qom_get_uint32(
                         source, "boot-eeprom-update-timestamp"), ==,
                     update_timestamp);

    destination = migrate_to_new_qtest(
        source, command, &migration_dir, &migration_socket);
    g_clear_pointer(&state, g_free);
    g_clear_pointer(&status, g_free);
    state = qom_get_string(destination, "boot-state");
    status = qom_get_string(destination, "boot-self-update-status");
    g_assert_cmpstr(state, ==, "self-update-updated-reboot");
    g_assert_cmpstr(status, ==, "updated-reboot");
    qtest_clock_step(destination, INT64_C(10) * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    g_clear_pointer(&status, g_free);
    state = qom_get_string(destination, "boot-state");
    status = qom_get_string(destination, "boot-self-update-status");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(status, ==, "up-to-date");
    assert_bootloader_update_timestamp(
        destination, true, update_timestamp);

    qtest_system_reset(destination);
    assert_bootloader_update_timestamp(
        destination, true, update_timestamp);

    qtest_quit(source);
    qtest_quit(destination);
    fd = open(eeprom_path, O_RDONLY);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(pread(fd, persisted, sizeof(persisted), 0), ==,
                    sizeof(persisted));
    close(fd);
    g_assert_cmpmem(persisted, sizeof(persisted), update, EEPROM_SIZE);
    persisted_status[1] = 1;
    stl_le_p(persisted_status + 2, update_timestamp);
    assert_file_equals(eeprom_status_path, persisted_status,
                       sizeof(persisted_status));
    source = qtest_init(command);
    assert_bootloader_update_timestamp(source, true, update_timestamp);
    qtest_quit(source);
    unlink(migration_socket);
    rmdir(migration_dir);
    unlink(eeprom_path);
    unlink(eeprom_status_path);
    unlink(sd_path);
}
void test_boot_filesystem_self_update_policy(void)
{
    static const uint8_t firmware[] = "firmware with self-update blocked";
    static const struct {
        const char *config;
        const char *status;
    } cases[] = {
        {
            "BOOT_ORDER=0x1\nENABLE_SELF_UPDATE=0\n",
            "disabled",
        },
        {
            "BOOT_ORDER=0x1\nENABLE_SELF_UPDATE=1\nFREEZE_VERSION=1\n",
            "frozen",
        },
    };
    g_autofree uint8_t *update = make_eeprom_image(
        "BOOT_ORDER=0x1\nBOOTVAR0=0x5678\n", false);
    g_autofree char *digest = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, update, EEPROM_SIZE);
    g_autofree char *signature = g_strdup_printf("%s\n", digest);
    g_autofree uint8_t *sd = make_usb_boot_image_with_update(
        "START4  ELF", firmware, sizeof(firmware), NULL,
        update, signature);

    for (unsigned int i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree uint8_t *initial = make_eeprom_image(
            cases[i].config, false);
        g_autofree char *eeprom_path = NULL;
        g_autofree char *sd_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *state = NULL;
        g_autofree char *status = NULL;
        uint8_t persisted[EEPROM_SIZE];
        QTestState *qts;
        int fd;

        write_temp_image("raspi4-eeprom-self-policy-XXXXXX",
                         initial, EEPROM_SIZE, &eeprom_path);
        write_temp_image("raspi4-sd-self-policy-XXXXXX",
                         sd, SD_SIZE, &sd_path);
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
            "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
            "-drive if=sd,format=raw,file=%s,file.locking=off -nic none",
            eeprom_path, sd_path);
        qts = qtest_init(command);
        state = qom_get_string(qts, "boot-state");
        status = qom_get_string(qts, "boot-self-update-status");
        g_assert_cmpstr(state, ==, "arm-handoff-ready");
        g_assert_cmpstr(status, ==, cases[i].status);
        qtest_quit(qts);

        fd = open(eeprom_path, O_RDONLY);
        g_assert_cmpint(fd, >=, 0);
        g_assert_cmpint(pread(fd, persisted, sizeof(persisted), 0), ==,
                        sizeof(persisted));
        close(fd);
        g_assert_cmpmem(persisted, sizeof(persisted),
                        initial, EEPROM_SIZE);
        unlink(eeprom_path);
        unlink(sd_path);
    }
}
void test_boot_filesystem_self_update_failures(void)
{
    static const uint8_t firmware[] = "unreached self-update firmware";
    static const struct {
        bool bad_signature;
        bool malformed_timestamp;
        bool write_protect;
        bool stale;
        const char *state;
        const char *status;
    } cases[] = {
        { true, false, false, false,
          "fatal-error-reboot-wait", "invalid" },
        { false, true, false, false,
          "fatal-error-reboot-wait", "invalid" },
        { false, false, true, false,
          "fatal-error-reboot-wait", "write-protected" },
        { false, false, false, true,
          "arm-handoff-ready", "stale" },
    };
    g_autofree uint8_t *initial = make_eeprom_image(
        "BOOT_ORDER=0x1\nENABLE_SELF_UPDATE=1\n", false);
    g_autofree uint8_t *update = make_eeprom_image(
        "BOOT_ORDER=0x1\nBOOTVAR0=0x9abc\n", false);
    g_autofree char *digest = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, update, EEPROM_SIZE);

    for (unsigned int i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree char *signature = NULL;
        g_autofree uint8_t *sd = NULL;
        g_autofree char *eeprom_path = NULL;
        g_autofree char *eeprom_status_path = NULL;
        g_autofree char *sd_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *state = NULL;
        g_autofree char *status = NULL;
        uint8_t persisted[EEPROM_SIZE];
        uint8_t persisted_status[512] = { 0 };
        QTestState *qts;
        const char *write_protect;
        int fd;

        if (cases[i].bad_signature) {
            signature = g_strdup_printf("%064d\n", 0);
        } else if (cases[i].malformed_timestamp) {
            signature = g_strdup_printf("%s\nts: invalid\n", digest);
        } else {
            signature = g_strdup_printf(
                "%s\nts: %u\n", digest, cases[i].stale ? 41: 100U);
        }
        sd = make_usb_boot_image_with_update(
            "START4  ELF", firmware, sizeof(firmware), NULL,
            update, signature);
        if (cases[i].write_protect) {
            write_protect = "on";
        } else {
            write_protect = "off";
        }
        write_temp_image("raspi4-eeprom-self-fail-XXXXXX",
                         initial, EEPROM_SIZE, &eeprom_path);
        persisted_status[0] = cases[i].write_protect;
        persisted_status[1] = 1;
        stl_le_p(persisted_status + 2, 42);
        write_temp_image("raspi4-eeprom-status-fail-XXXXXX",
                         persisted_status, sizeof(persisted_status),
                         &eeprom_status_path);
        write_temp_image("raspi4-sd-self-fail-XXXXXX",
                         sd, SD_SIZE, &sd_path);
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "eeprom-write-protect=%s,eeprom-status-drive=eepromstatus "
            "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
            "-drive if=none,id=eepromstatus,format=raw,file=%s,"
            "file.locking=off "
            "-drive if=sd,format=raw,file=%s,file.locking=off -nic none",
            write_protect, eeprom_path, eeprom_status_path, sd_path);
        qts = qtest_init(command);
        state = qom_get_string(qts, "boot-state");
        status = qom_get_string(qts, "boot-self-update-status");
        g_assert_cmpstr(state, ==, cases[i].state);
        g_assert_cmpstr(status, ==, cases[i].status);
        g_assert_true(qom_get_bool(
                          qts, "boot-eeprom-update-timestamp-valid"));
        g_assert_cmpuint(qom_get_uint32(
                             qts, "boot-eeprom-update-timestamp"), ==, 42);
        qtest_quit(qts);

        fd = open(eeprom_path, O_RDONLY);
        g_assert_cmpint(fd, >=, 0);
        g_assert_cmpint(pread(fd, persisted, sizeof(persisted), 0), ==,
                        sizeof(persisted));
        close(fd);
        g_assert_cmpmem(persisted, sizeof(persisted),
                        initial, EEPROM_SIZE);
        assert_file_equals(eeprom_status_path, persisted_status,
                           sizeof(persisted_status));
        unlink(eeprom_path);
        unlink(eeprom_status_path);
        unlink(sd_path);
    }
}
void test_boot_filesystem_self_update_usb_nvme(void)
{
    static const uint8_t firmware[] = "alternate-media self-update";
    static const struct {
        const char *boot_order;
        const char *machine_option;
        const char *drive_id;
        const char *source;
    } cases[] = {
        { "0x4", "usb-boot-drive=bootmedia", "bootmedia", "usb-msd" },
        { "0x6", "nvme-drive=bootmedia", "bootmedia", "nvme" },
    };

    for (unsigned int i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree char *initial_config = g_strdup_printf(
            "BOOT_ORDER=%s\nENABLE_SELF_UPDATE=1\n", cases[i].boot_order);
        g_autofree char *update_config = g_strdup_printf(
            "BOOT_ORDER=%s\nENABLE_SELF_UPDATE=1\nBOOTVAR0=0x%x\n",
            cases[i].boot_order, 0x2000 + i);
        g_autofree uint8_t *initial = make_eeprom_image(
            initial_config, false);
        g_autofree uint8_t *update = make_eeprom_image(
            update_config, false);
        g_autofree char *digest = g_compute_checksum_for_data(
            G_CHECKSUM_SHA256, update, EEPROM_SIZE);
        g_autofree char *signature = g_strdup_printf("%s\n", digest);
        g_autofree uint8_t *media = make_usb_boot_image_with_update(
            "START4  ELF", firmware, sizeof(firmware), NULL,
            update, signature);
        g_autofree char *eeprom_path = NULL;
        g_autofree char *media_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *state = NULL;
        g_autofree char *status = NULL;
        g_autofree char *source = NULL;
        QTestState *qts;

        write_temp_image("raspi4-eeprom-self-alt-XXXXXX",
                         initial, EEPROM_SIZE, &eeprom_path);
        write_temp_image("raspi4-media-self-alt-XXXXXX",
                         media, SD_SIZE, &media_path);
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,%s "
            "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
            "-drive if=none,id=%s,format=raw,file=%s,file.locking=off "
            "-nic none",
            cases[i].machine_option, eeprom_path,
            cases[i].drive_id, media_path);
        qts = qtest_init(command);
        state = qom_get_string(qts, "boot-state");
        status = qom_get_string(qts, "boot-self-update-status");
        g_assert_cmpstr(state, ==, "self-update-updated-reboot");
        g_assert_cmpstr(status, ==, "updated-reboot");
        qtest_clock_step(qts, INT64_C(10) * 1000 * 1000);
        g_clear_pointer(&state, g_free);
        g_clear_pointer(&status, g_free);
        state = qom_get_string(qts, "boot-state");
        status = qom_get_string(qts, "boot-self-update-status");
        source = qom_get_string(qts, "boot-source");
        g_assert_cmpstr(state, ==, "arm-handoff-ready");
        g_assert_cmpstr(status, ==, "up-to-date");
        g_assert_cmpstr(source, ==, cases[i].source);
        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(media_path);
    }
}
void test_usb_mass_storage_timeouts(void)
{
    {
        static const uint8_t firmware[] =
            "USB media waits for the configured startup delay";
        g_autofree char *eeprom_path = NULL;
        g_autofree char *usb_path = NULL;
        g_autofree char *state = NULL;
        g_autofree char *source = NULL;
        QTestState *qts = start_with_eeprom_and_usb(
            "BOOT_ORDER=0xe4\nUSB_MSD_STARTUP_DELAY=1234\n"
            "USB_MSD_DISCOVER_TIMEOUT=7000\n",
            "START4  ELF", firmware, sizeof(firmware),
            &eeprom_path, &usb_path);

        state = qom_get_string(qts, "boot-state");
        source = qom_get_string(qts, "boot-source");
        g_assert_cmpstr(state, ==, "usb-startup-delay");
        g_assert_cmpstr(source, ==, "usb-msd");
        g_assert_cmpuint(qom_get_uint32(
                             qts, "boot-usb-startup-delay-ms"), ==, 1234);
        g_assert_cmpuint(qom_get_uint64(qts, "firmware-size"), ==, 0);
        g_assert_cmpuint(qom_get_uint64(qts, "boot-elapsed-ms"), ==, 0);
        qtest_clock_step(qts, INT64_C(1233) * 1000 * 1000);
        g_clear_pointer(&state, g_free);
        state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(state, ==, "usb-startup-delay");
        qtest_clock_step(qts, 1 * 1000 * 1000);
        g_clear_pointer(&state, g_free);
        state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(state, ==, "arm-handoff-ready");
        g_assert_cmpuint(qom_get_uint64(qts, "firmware-size"), ==,
                         sizeof(firmware));
        g_assert_cmpuint(qom_get_uint64(qts, "boot-elapsed-ms"), ==, 1234);
        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(usb_path);
    }

    {
        g_autofree char *eeprom_path = NULL;
        g_autofree char *state = NULL;
        g_autofree char *source = NULL;
        QTestState *qts = start_with_eeprom(
            "BOOT_ORDER=0xe4\nUSB_MSD_DISCOVER_TIMEOUT=7000\n"
            "USB_MSD_LUN_TIMEOUT=321\n",
            false, &eeprom_path);

        state = qom_get_string(qts, "boot-state");
        source = qom_get_string(qts, "boot-source");
        g_assert_cmpstr(state, ==, "usb-discovery-wait");
        g_assert_cmpstr(source, ==, "usb-msd");
        g_assert_cmpuint(qom_get_uint64(qts, "boot-elapsed-ms"), ==, 0);
        g_assert_cmpuint(qom_get_uint32(
                             qts, "boot-usb-discover-timeout-ms"), ==, 7000);
        g_assert_cmpuint(qom_get_uint32(qts, "boot-usb-lun-timeout-ms"),
                         ==, 321);
        g_assert_cmpuint(qom_get_uint64(qts, "boot-attempt-count"), ==, 1);
        g_assert_cmpuint(qom_get_uint32(qts, "boot-order-index"), ==, 0);
        qtest_clock_step(qts, INT64_C(6999) * 1000 * 1000);
        g_clear_pointer(&state, g_free);
        state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(state, ==, "usb-discovery-wait");
        qtest_clock_step(qts, 1 * 1000 * 1000);
        g_clear_pointer(&state, g_free);
        g_clear_pointer(&source, g_free);
        state = qom_get_string(qts, "boot-state");
        source = qom_get_string(qts, "boot-source");
        g_assert_cmpstr(state, ==, "stopped");
        g_assert_cmpstr(source, ==, "stop");
        g_assert_cmpuint(qom_get_uint64(qts, "boot-elapsed-ms"), ==, 7000);
        g_assert_cmpuint(qom_get_uint32(qts, "boot-order-index"), ==, 1);
        qtest_quit(qts);
        unlink(eeprom_path);
    }

    {
        g_autofree char *eeprom_path = NULL;
        g_autofree char *usb_path = NULL;
        g_autofree char *state = NULL;
        g_autofree char *status = NULL;
        QTestState *qts = start_with_eeprom_and_usb(
            "BOOT_ORDER=0xe4\nUSB_MSD_DISCOVER_TIMEOUT=7000\n"
            "USB_MSD_LUN_TIMEOUT=321\n",
            "START4  ELF", NULL, 0, &eeprom_path, &usb_path);

        state = qom_get_string(qts, "boot-state");
        status = qom_get_string(qts, "firmware-status");
        g_assert_cmpstr(state, ==, "usb-lun-wait");
        g_assert_cmpstr(status, ==, "firmware-missing");
        g_assert_cmpuint(qom_get_uint64(qts, "boot-elapsed-ms"), ==, 0);
        qtest_clock_step(qts, 321 * 1000 * 1000);
        g_clear_pointer(&state, g_free);
        state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(state, ==, "stopped");
        g_assert_cmpuint(qom_get_uint64(qts, "boot-elapsed-ms"), ==, 321);
        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(usb_path);
    }

    {
        static const uint8_t firmware[] =
            "USB media became bootable during the LUN wait";
        g_autofree uint8_t *usb = make_usb_boot_image(
            "START4  ELF", firmware, sizeof(firmware), NULL);
        g_autofree char *expected_hash = g_compute_checksum_for_data(
            G_CHECKSUM_SHA256, firmware, sizeof(firmware));
        g_autofree char *eeprom_path = NULL;
        g_autofree char *usb_path = NULL;
        g_autofree char *state = NULL;
        g_autofree char *source = NULL;
        g_autofree char *firmware_hash = NULL;
        QTestState *qts = start_with_eeprom_and_usb(
            "BOOT_ORDER=0xe4\nUSB_MSD_LUN_TIMEOUT=321\n",
            "START4  ELF", NULL, 0, &eeprom_path, &usb_path);

        state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(state, ==, "usb-lun-wait");
        overwrite_image(usb_path, usb, SD_SIZE);
        qtest_clock_step(qts, 321 * 1000 * 1000);
        g_clear_pointer(&state, g_free);
        state = qom_get_string(qts, "boot-state");
        source = qom_get_string(qts, "boot-source");
        firmware_hash = qom_get_string(qts, "firmware-sha256");
        g_assert_cmpstr(state, ==, "arm-handoff-ready");
        g_assert_cmpstr(source, ==, "usb-msd");
        g_assert_cmpstr(firmware_hash, ==, expected_hash);
        g_assert_cmpuint(qom_get_uint64(qts, "boot-elapsed-ms"), ==, 321);
        g_assert_cmpuint(qom_get_uint64(qts, "boot-attempt-count"), ==, 1);
        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(usb_path);
    }

    {
        static const char * const invalid[] = {
            "BOOT_ORDER=0x4\nUSB_MSD_DISCOVER_TIMEOUT=4999\n",
            "BOOT_ORDER=0x4\nUSB_MSD_LUN_TIMEOUT=99\n",
            "BOOT_ORDER=0x4\nUSB_MSD_STARTUP_DELAY=30001\n",
            "BOOT_ORDER=0x4\nUSB_MSD_PWR_OFF_TIME=5001\n",
        };

        for (size_t i = 0; i < ARRAY_SIZE(invalid); i++) {
            g_autofree char *eeprom_path = NULL;
            g_autofree char *state = NULL;
            QTestState *qts = start_with_eeprom(
                invalid[i], false, &eeprom_path);

            state = qom_get_string(qts, "boot-state");
            g_assert_cmpstr(state, ==, "eeprom-invalid");
            qtest_quit(qts);
            unlink(eeprom_path);
        }
    }
}
void test_usb_power_off_timing_and_board_revisions(void)
{
    static const uint8_t firmware[] =
        "USB power-cycle board-revision corpus";
    g_autofree uint8_t *usb = make_usb_boot_image(
        "START4  ELF", firmware, sizeof(firmware), NULL);

    {
        g_autofree uint8_t *eeprom = make_eeprom_image(
            "BOOT_ORDER=0xe4\nUSB_MSD_PWR_OFF_TIME=5000\n", false);
        g_autofree char *eeprom_path = NULL;
        g_autofree char *usb_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *state = NULL;
        g_autofree char *mode = NULL;
        QTestState *qts;

        write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                         &eeprom_path);
        write_temp_image("raspi4-usb-XXXXXX", usb, SD_SIZE, &usb_path);
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "usb-boot-drive=usbboot "
            "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
            "-drive if=none,id=usbboot,format=raw,file=%s,file.locking=off "
            "-nic none",
            eeprom_path, usb_path);
        qts = qtest_init(command);
        state = qom_get_string(qts, "boot-state");
        mode = qom_get_string(qts, "boot-usb-power-cycle-mode");
        g_assert_cmpstr(state, ==, "usb-power-off-wait");
        g_assert_cmpstr(mode, ==,
                        "reset-held-with-memory-init-overlap");
        g_assert_true(qom_get_bool(qts,
                                   "boot-usb-power-off-applicable"));
        g_assert_false(qom_get_bool(qts, "boot-usb-power-enabled"));
        g_assert_cmpuint(qom_get_uint32(
                             qts, "boot-usb-power-off-time-ms"), ==, 5000);
        g_assert_cmpuint(qom_get_uint64(
                             qts, "boot-usb-power-off-elapsed-ms"), ==,
                         2000);
        g_assert_cmpuint(qom_get_uint64(
                             qts, "boot-usb-power-off-remaining-ns"), ==,
                         INT64_C(3000) * 1000 * 1000);
        qtest_clock_step(qts, INT64_C(2999) * 1000 * 1000);
        g_clear_pointer(&state, g_free);
        state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(state, ==, "usb-power-off-wait");
        qtest_clock_step(qts, 1 * 1000 * 1000);
        g_clear_pointer(&state, g_free);
        state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(state, ==, "arm-handoff-ready");
        g_assert_true(qom_get_bool(qts, "boot-usb-power-enabled"));
        g_assert_cmpuint(qom_get_uint64(
                             qts, "boot-usb-power-off-elapsed-ms"), ==,
                         5000);
        g_assert_cmpuint(qom_get_uint64(qts, "boot-elapsed-ms"), ==, 3000);
        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(usb_path);
    }

    {
        g_autofree uint8_t *eeprom = make_eeprom_image(
            "BOOT_ORDER=0xe4\nUSB_MSD_PWR_OFF_TIME=1000\n", false);
        g_autofree char *eeprom_path = NULL;
        g_autofree char *usb_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *state = NULL;
        g_autofree char *mode = NULL;
        QTestState *qts;

        write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                         &eeprom_path);
        write_temp_image("raspi4-usb-XXXXXX", usb, SD_SIZE, &usb_path);
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "usb-boot-drive=usbboot,board-revision=0xa03111 -m 1G "
            "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
            "-drive if=none,id=usbboot,format=raw,file=%s,file.locking=off "
            "-nic none",
            eeprom_path, usb_path);
        qts = qtest_init(command);
        state = qom_get_string(qts, "boot-state");
        mode = qom_get_string(qts, "boot-usb-power-cycle-mode");
        g_assert_cmphex(qom_get_uint32(qts, "board-revision"), ==,
                        0xa03111);
        g_assert_cmpstr(state, ==, "usb-power-off-wait");
        g_assert_cmpstr(mode, ==, "legacy-short-then-configurable");
        g_assert_false(qom_get_bool(qts, "boot-usb-power-enabled"));
        g_assert_cmpuint(qom_get_uint64(
                             qts, "boot-usb-power-off-elapsed-ms"), ==, 0);
        qtest_clock_step(qts, INT64_C(999) * 1000 * 1000);
        g_clear_pointer(&state, g_free);
        state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(state, ==, "usb-power-off-wait");
        qtest_clock_step(qts, 1 * 1000 * 1000);
        g_clear_pointer(&state, g_free);
        state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(state, ==, "arm-handoff-ready");
        g_assert_cmpuint(qom_get_uint64(
                             qts, "boot-usb-power-off-elapsed-ms"), ==,
                         1000);
        g_assert_cmpuint(qom_get_uint64(qts, "boot-elapsed-ms"), ==, 1000);

        qtest_system_reset(qts);
        g_clear_pointer(&state, g_free);
        state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(state, ==, "usb-power-off-wait");
        g_assert_false(qom_get_bool(qts, "boot-usb-power-enabled"));
        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(usb_path);
    }

    {
        g_autofree uint8_t *eeprom = make_eeprom_image(
            "BOOT_ORDER=0xe4\nUSB_MSD_PWR_OFF_TIME=0\n", false);
        g_autofree char *eeprom_path = NULL;
        g_autofree char *usb_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *state = NULL;
        QTestState *qts;

        write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                         &eeprom_path);
        write_temp_image("raspi4-usb-XXXXXX", usb, SD_SIZE, &usb_path);
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "usb-boot-drive=usbboot,board-revision=0xa03111 -m 1G "
            "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
            "-drive if=none,id=usbboot,format=raw,file=%s,file.locking=off "
            "-nic none",
            eeprom_path, usb_path);
        qts = qtest_init(command);
        state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(state, ==, "arm-handoff-ready");
        g_assert_true(qom_get_bool(qts, "boot-usb-power-enabled"));
        g_assert_cmpuint(qom_get_uint64(
                             qts, "boot-usb-power-off-elapsed-ms"), ==, 0);
        g_assert_cmpuint(qom_get_uint64(qts, "boot-elapsed-ms"), ==, 0);
        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(usb_path);
    }

    {
        g_autofree uint8_t *eeprom = make_eeprom_image(
            "BOOT_ORDER=0xe4\nUSB_MSD_PWR_OFF_TIME=5000\n", false);
        g_autofree char *eeprom_path = NULL;
        g_autofree char *usb_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *state = NULL;
        g_autofree char *mode = NULL;
        QTestState *qts;

        write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                         &eeprom_path);
        write_temp_image("raspi4-usb-XXXXXX", usb, SD_SIZE, &usb_path);
        command = g_strdup_printf(
            "-M raspi-cm4,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "usb-boot-drive=usbboot "
            "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
            "-drive if=none,id=usbboot,format=raw,file=%s,file.locking=off "
            "-nic none",
            eeprom_path, usb_path);
        qts = qtest_init(command);
        state = qom_get_string(qts, "boot-state");
        mode = qom_get_string(qts, "boot-usb-power-cycle-mode");
        g_assert_cmpstr(state, ==, "usb-lun-wait");
        g_assert_cmpstr(mode, ==, "not-applicable");
        g_assert_false(qom_get_bool(qts,
                                    "boot-usb-power-off-applicable"));
        g_assert_true(qom_get_bool(qts, "boot-usb-power-enabled"));
        g_assert_cmpuint(qom_get_uint64(
                             qts, "boot-usb-power-off-elapsed-ms"), ==, 0);
        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(usb_path);
    }
}
void test_usb_mass_storage_hotplug(void)
{
    static const uint8_t firmware[] =
        "USB medium inserted while behavioral ROM is waiting";
    g_autofree uint8_t *eeprom = make_eeprom_image(
        "BOOT_ORDER=0xe4\nUSB_MSD_DISCOVER_TIMEOUT=7000\n"
        "USB_MSD_LUN_TIMEOUT=321\n", false);
    g_autofree uint8_t *invalid_usb = make_usb_boot_image(
        "START4  ELF", NULL, 0, NULL);
    g_autofree uint8_t *bootable_usb = make_usb_boot_image(
        "START4  ELF", firmware, sizeof(firmware), NULL);
    g_autofree char *expected_hash = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, firmware, sizeof(firmware));
    g_autofree char *eeprom_path = NULL;
    g_autofree char *invalid_usb_path = NULL;
    g_autofree char *bootable_usb_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *state = NULL;
    g_autofree char *source = NULL;
    g_autofree char *firmware_hash = NULL;
    g_autofree char *firmware_status = NULL;
    g_autofree char *qtree = NULL;
    QTestState *qts;

    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    write_temp_image("raspi4-usb-invalid-XXXXXX", invalid_usb, SD_SIZE,
                     &invalid_usb_path);
    write_temp_image("raspi4-usb-bootable-XXXXXX", bootable_usb, SD_SIZE,
                     &bootable_usb_path);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "usb-boot-drive=usbboot "
        "-drive if=none,id=pieeprom,format=raw,file=%s "
        "-drive if=none,id=usbboot",
        eeprom_path);
    qts = qtest_init(command);
    state = qom_get_string(qts, "boot-state");
    qtree = qtest_hmp(qts, "info qtree");
    g_assert_cmpstr(state, ==, "usb-discovery-wait");
    g_assert_nonnull(strstr(qtree, "usb-storage"));
    qtest_clock_step(qts, INT64_C(1234) * 1000 * 1000);

    qtest_qmp_assert_success(
        qts, "{ 'execute': 'blockdev-change-medium', 'arguments': {"
             "'device': 'usbboot', 'filename': %s, 'format': 'raw' } }",
        invalid_usb_path);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "usb-lun-wait");
    g_assert_cmpuint(qom_get_uint64(qts, "boot-elapsed-ms"), ==, 0);
    qtest_clock_step(qts, INT64_C(100) * 1000 * 1000);

    qtest_qmp_assert_success(
        qts, "{ 'execute': 'eject', 'arguments': {"
             "'device': 'usbboot', 'force': true } }");
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "usb-discovery-wait");

    qtest_qmp_assert_success(
        qts, "{ 'execute': 'blockdev-change-medium', 'arguments': {"
             "'device': 'usbboot', 'filename': %s, 'format': 'raw' } }",
        bootable_usb_path);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "boot-source");
    firmware_hash = qom_get_string(qts, "firmware-sha256");
    firmware_status = qom_get_string(qts, "firmware-status");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(source, ==, "usb-msd");
    g_assert_cmpstr(firmware_status, ==, "config-ready");
    g_assert_cmpstr(firmware_hash, ==, expected_hash);
    g_assert_cmpuint(qom_get_uint64(qts, "boot-attempt-count"), ==, 1);
    g_assert_cmpuint(qom_get_uint64(qts, "boot-elapsed-ms"), ==, 0);
    qtest_clock_step(qts, INT64_C(8000) * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");

    qtest_quit(qts);
    unlink(eeprom_path);
    unlink(invalid_usb_path);
    unlink(bootable_usb_path);
}
void test_sd_card_hotplug(void)
{
    static const uint8_t firmware[] =
        "SD medium inserted while behavioral ROM is waiting";
    g_autofree char *expected_hash = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, firmware, sizeof(firmware));
    g_autofree char *eeprom_path = NULL;
    g_autofree char *bootable_sd_path = NULL;
    g_autofree char *invalid_sd_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *state = NULL;
    g_autofree char *source = NULL;
    g_autofree char *firmware_hash = NULL;
    Fat16Builder invalid_builder;
    QTestState *qts = start_with_eeprom_and_sd(
        "BOOT_ORDER=0x0\n", "START4  ELF", firmware, sizeof(firmware),
        NULL, &eeprom_path, &bootable_sd_path);

    qtest_quit(qts);
    fat16_init(&invalid_builder);
    write_temp_image("raspi4-sd-invalid-XXXXXX", invalid_builder.image,
                     SD_SIZE, &invalid_sd_path);
    g_free(invalid_builder.image);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
        "-drive if=none,id=pieeprom,format=raw,file=%s "
        "-drive if=sd,id=sdcard",
        eeprom_path);
    qts = qtest_init(command);
    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "boot-source");
    g_assert_cmpstr(state, ==, "sd-card-detect-wait");
    g_assert_cmpstr(source, ==, "sd-card-detect");
    g_assert_cmpuint(qom_get_uint64(qts, "boot-attempt-count"), ==, 1);

    qtest_qmp_assert_success(
        qts, "{ 'execute': 'blockdev-change-medium', 'arguments': {"
             "'device': 'sdcard', 'filename': %s, 'format': 'raw' } }",
        invalid_sd_path);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "sd-card-detect-wait");
    qtest_qmp_assert_success(
        qts, "{ 'execute': 'eject', 'arguments': {"
             "'device': 'sdcard', 'force': true } }");
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "sd-card-detect-wait");

    qtest_qmp_assert_success(
        qts, "{ 'execute': 'blockdev-change-medium', 'arguments': {"
             "'device': 'sdcard', 'filename': %s, 'format': 'raw' } }",
        bootable_sd_path);
    g_clear_pointer(&state, g_free);
    g_clear_pointer(&source, g_free);
    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "boot-source");
    firmware_hash = qom_get_string(qts, "firmware-sha256");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpstr(source, ==, "sd-card-detect");
    g_assert_cmpstr(firmware_hash, ==, expected_hash);
    g_assert_cmpuint(qom_get_uint64(qts, "boot-attempt-count"), ==, 1);

    qtest_quit(qts);
    unlink(eeprom_path);
    unlink(bootable_sd_path);
    unlink(invalid_sd_path);
}
void test_sd_retry_hotplug(void)
{
    static const uint8_t firmware[] =
        "SD medium inserted while infinite retry is active";
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *state = NULL;
    QTestState *qts = start_with_eeprom_and_sd(
        "BOOT_ORDER=0x1\nSD_BOOT_MAX_RETRIES=-1\n", "START4  ELF",
        firmware, sizeof(firmware), NULL, &eeprom_path, &sd_path);

    qtest_quit(qts);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
        "-drive if=none,id=pieeprom,format=raw,file=%s "
        "-drive if=sd,id=sdcard",
        eeprom_path);
    qts = qtest_init(command);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "sd-retry-loop");
    g_assert_cmpuint(qom_get_uint64(qts, "boot-attempt-count"), ==, 1);
    g_assert_cmpuint(qom_get_uint64(qts, "boot-retry-count"), ==, 0);

    qtest_qmp_assert_success(
        qts, "{ 'execute': 'blockdev-change-medium', 'arguments': {"
             "'device': 'sdcard', 'filename': %s, 'format': 'raw' } }",
        sd_path);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpuint(qom_get_uint64(qts, "boot-attempt-count"), ==, 2);
    g_assert_cmpuint(qom_get_uint64(qts, "boot-retry-count"), ==, 1);

    qtest_quit(qts);
    unlink(eeprom_path);
    unlink(sd_path);
}
void test_sd_detect_migration(void)
{
    static const uint8_t firmware[] =
        "SD medium inserted after card-detect migration";
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_path = NULL;
    g_autofree char *state = NULL;
    g_autofree char *source = NULL;
    QTestState *destination;
    QTestState *qts = start_with_eeprom_and_sd(
        "BOOT_ORDER=0x0\n", "START4  ELF", firmware, sizeof(firmware),
        NULL, &eeprom_path, &sd_path);

    qtest_quit(qts);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=sd,id=sdcard",
        eeprom_path);
    qts = qtest_init(command);
    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "boot-source");
    g_assert_cmpstr(state, ==, "sd-card-detect-wait");
    g_assert_cmpstr(source, ==, "sd-card-detect");
    g_assert_cmpuint(qom_get_uint32(qts, "boot-order-index"), ==, 0);
    g_assert_cmpuint(qom_get_uint64(qts, "boot-attempt-count"), ==, 1);

    destination = migrate_to_new_qtest(qts, command, &migration_dir,
                                       &migration_path);
    g_clear_pointer(&state, g_free);
    g_clear_pointer(&source, g_free);
    state = qom_get_string(destination, "boot-state");
    source = qom_get_string(destination, "boot-source");
    g_assert_cmpstr(state, ==, "sd-card-detect-wait");
    g_assert_cmpstr(source, ==, "sd-card-detect");
    g_assert_cmpuint(qom_get_uint32(destination, "boot-order-index"), ==, 0);
    g_assert_cmpuint(qom_get_uint64(destination, "boot-attempt-count"), ==,
                     1);

    qtest_qmp_assert_success(
        destination,
        "{ 'execute': 'blockdev-change-medium', 'arguments': {"
        "'device': 'sdcard', 'filename': %s, 'format': 'raw' } }",
        sd_path);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpuint(qom_get_uint64(destination, "boot-attempt-count"), ==,
                     1);

    qtest_quit(qts);
    qtest_quit(destination);
    unlink(eeprom_path);
    unlink(sd_path);
    unlink(migration_path);
    rmdir(migration_dir);
}
void test_usb_timeout_migration(void)
{
    g_autofree uint8_t *eeprom = make_eeprom_image(
        "BOOT_ORDER=0xe4\nUSB_MSD_STARTUP_DELAY=2000\n"
        "USB_MSD_DISCOVER_TIMEOUT=7000\n", false);
    g_autofree char *eeprom_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *destination_command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_path = NULL;
    g_autofree char *migration_uri = NULL;
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
    source = qom_get_string(qts, "boot-source");
    g_assert_cmpstr(state, ==, "usb-startup-delay");
    g_assert_cmpstr(source, ==, "usb-msd");
    g_assert_cmpuint(qom_get_uint64(qts, "boot-attempt-count"), ==, 1);
    g_assert_cmpuint(qom_get_uint32(qts, "boot-order-index"), ==, 0);

    qtest_clock_step(qts, INT64_C(1000) * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "usb-startup-delay");

    migration_dir = g_dir_make_tmp("raspi4-boot-migration-XXXXXX", NULL);
    g_assert_nonnull(migration_dir);
    migration_path = g_build_filename(migration_dir, "stream.sock", NULL);
    migration_uri = g_strdup_printf("unix:%s", migration_path);
    destination_command = g_strdup_printf("%s -incoming %s", command,
                                          migration_uri);
    destination = qtest_init(destination_command);
    qtest_qmp_assert_success(
        qts, "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }",
        migration_uri);
    wait_for_migration_complete(qts);
    wait_for_migration_complete(destination);

    g_clear_pointer(&state, g_free);
    g_clear_pointer(&source, g_free);
    state = qom_get_string(destination, "boot-state");
    source = qom_get_string(destination, "boot-source");
    g_assert_cmpstr(state, ==, "usb-startup-delay");
    g_assert_cmpstr(source, ==, "usb-msd");
    g_assert_cmpuint(qom_get_uint64(destination, "boot-attempt-count"), ==,
                     1);
    g_assert_cmpuint(qom_get_uint64(destination, "boot-elapsed-ms"), ==, 0);

    qtest_clock_step(destination, INT64_C(999) * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "usb-startup-delay");
    qtest_clock_step(destination, 1 * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "usb-discovery-wait");
    g_assert_cmpuint(qom_get_uint64(destination, "boot-elapsed-ms"), ==,
                     2000);
    qtest_clock_step(destination, INT64_C(6999) * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "usb-discovery-wait");
    qtest_clock_step(destination, 1 * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    g_clear_pointer(&source, g_free);
    state = qom_get_string(destination, "boot-state");
    source = qom_get_string(destination, "boot-source");
    g_assert_cmpstr(state, ==, "stopped");
    g_assert_cmpstr(source, ==, "stop");
    g_assert_cmpuint(qom_get_uint64(destination, "boot-elapsed-ms"), ==,
                     9000);
    g_assert_cmpuint(qom_get_uint32(destination, "boot-order-index"), ==, 1);

    qtest_quit(qts);
    qtest_quit(destination);
    unlink(eeprom_path);
    unlink(migration_path);
    rmdir(migration_dir);
}
void test_usb_power_off_migration(void)
{
    static const uint8_t firmware[] =
        "USB power-off migration corpus";
    g_autofree uint8_t *eeprom = make_eeprom_image(
        "BOOT_ORDER=0xe4\nUSB_MSD_PWR_OFF_TIME=5000\n", false);
    g_autofree uint8_t *usb = make_usb_boot_image(
        "START4  ELF", firmware, sizeof(firmware), NULL);
    g_autofree char *eeprom_path = NULL;
    g_autofree char *usb_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_path = NULL;
    g_autofree char *state = NULL;
    QTestState *destination;
    QTestState *qts;

    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    write_temp_image("raspi4-usb-XXXXXX", usb, SD_SIZE, &usb_path);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "usb-boot-drive=usbboot "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=none,id=usbboot,format=raw,file=%s,file.locking=off "
        "-nic none",
        eeprom_path, usb_path);
    qts = qtest_init(command);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "usb-power-off-wait");
    qtest_clock_step(qts, INT64_C(1000) * 1000 * 1000);
    g_assert_cmpuint(qom_get_uint64(
                         qts, "boot-usb-power-off-remaining-ns"), ==,
                     INT64_C(2000) * 1000 * 1000);

    destination = migrate_to_new_qtest(
        qts, command, &migration_dir, &migration_path);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "usb-power-off-wait");
    g_assert_false(qom_get_bool(destination, "boot-usb-power-enabled"));
    g_assert_cmpuint(qom_get_uint64(
                         destination,
                         "boot-usb-power-off-elapsed-ms"), ==, 2000);
    g_assert_cmpuint(qom_get_uint64(
                         destination,
                         "boot-usb-power-off-remaining-ns"), ==,
                     INT64_C(2000) * 1000 * 1000);
    qtest_clock_step(destination, INT64_C(1999) * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "usb-power-off-wait");
    qtest_clock_step(destination, 1 * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_true(qom_get_bool(destination, "boot-usb-power-enabled"));
    g_assert_cmpuint(qom_get_uint64(
                         destination,
                         "boot-usb-power-off-elapsed-ms"), ==, 5000);
    g_assert_cmpuint(qom_get_uint64(
                         destination, "boot-elapsed-ms"), ==, 3000);

    qtest_quit(qts);
    qtest_quit(destination);
    unlink(eeprom_path);
    unlink(usb_path);
    unlink(migration_path);
    rmdir(migration_dir);
}
void test_usb_lun_timeout_migration(void)
{
    g_autofree uint8_t *eeprom = make_eeprom_image(
        "BOOT_ORDER=0xe4\nUSB_MSD_LUN_TIMEOUT=321\n", false);
    g_autofree uint8_t *usb = make_usb_boot_image(
        "START4  ELF", NULL, 0, NULL);
    g_autofree char *eeprom_path = NULL;
    g_autofree char *usb_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_path = NULL;
    g_autofree char *state = NULL;
    QTestState *destination;
    QTestState *qts;

    write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE,
                     &eeprom_path);
    write_temp_image("raspi4-usb-XXXXXX", usb, SD_SIZE, &usb_path);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "usb-boot-drive=usbboot "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=none,id=usbboot,format=raw,file=%s,file.locking=off",
        eeprom_path, usb_path);
    qts = qtest_init(command);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "usb-lun-wait");
    g_assert_cmpuint(qom_get_uint64(qts, "boot-attempt-count"), ==, 1);
    qtest_clock_step(qts, INT64_C(100) * 1000 * 1000);

    destination = migrate_to_new_qtest(qts, command, &migration_dir,
                                       &migration_path);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "usb-lun-wait");
    g_assert_cmpuint(qom_get_uint64(destination, "boot-elapsed-ms"), ==, 0);
    qtest_clock_step(destination, INT64_C(220999999));
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "usb-lun-wait");
    qtest_clock_step(destination, 1);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "stopped");
    g_assert_cmpuint(qom_get_uint64(destination, "boot-elapsed-ms"), ==, 321);

    qtest_quit(qts);
    qtest_quit(destination);
    unlink(eeprom_path);
    unlink(usb_path);
    unlink(migration_path);
    rmdir(migration_dir);
}
void test_sd_overcurrent_recovery_and_policy(void)
{
    static const uint8_t firmware[] = "SD over-current recovery";

    {
        g_autofree char *eeprom_path = NULL;
        g_autofree char *sd_path = NULL;
        g_autofree char *state = NULL;
        QTestState *qts = start_with_eeprom_and_sd(
            "BOOT_ORDER=0x1\n", "START4  ELF",
            firmware, sizeof(firmware), NULL, &eeprom_path, &sd_path);

        qom_set_bool(qts, "sd-overcurrent", true);
        qtest_system_reset(qts);
        g_clear_pointer(&state, g_free);
        state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(state, ==, "sd-overcurrent-wait");
        g_assert_true(qom_get_bool(qts, "boot-sd-overcurrent-check"));
        g_assert_false(qom_get_bool(qts, "boot-sd-overcurrent-warning"));
        g_assert_false(qom_get_bool(qts, "boot-sd-power-enabled"));
        g_assert_cmpuint(qom_get_uint64(
                             qts, "boot-sd-overcurrent-retries"), ==, 1);
        g_assert_cmpuint(qom_get_uint64(qts, "boot-attempt-count"), ==, 0);

        qtest_clock_step(qts, INT64_C(5) * 1000 * 1000 * 1000 - 1);
        g_clear_pointer(&state, g_free);
        state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(state, ==, "sd-overcurrent-wait");
        qtest_clock_step(qts, 1);
        g_assert_cmpuint(qom_get_uint64(
                             qts, "boot-sd-overcurrent-retries"), ==, 2);
        g_assert_cmpuint(qom_get_uint64(qts, "boot-elapsed-ms"), ==, 5000);

        qom_set_bool(qts, "sd-overcurrent", false);
        qtest_clock_step(qts, INT64_C(5) * 1000 * 1000 * 1000 - 1);
        g_clear_pointer(&state, g_free);
        state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(state, ==, "sd-overcurrent-wait");
        qtest_clock_step(qts, 1);
        g_clear_pointer(&state, g_free);
        state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(state, ==, "arm-handoff-ready");
        g_assert_true(qom_get_bool(qts, "boot-sd-power-enabled"));
        g_assert_cmpuint(qom_get_uint64(qts, "boot-attempt-count"), ==, 1);
        g_assert_cmpuint(qom_get_uint64(qts, "boot-elapsed-ms"), ==, 10000);

        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(sd_path);
    }

    {
        g_autofree char *eeprom_path = NULL;
        g_autofree char *sd_path = NULL;
        g_autofree char *state = NULL;
        QTestState *qts = start_with_eeprom_and_sd(
            "BOOT_ORDER=0x1\nSD_OVERCURRENT_CHECK=0\nSD_QUIRKS=1\n",
            "START4  ELF", firmware, sizeof(firmware), NULL,
            &eeprom_path, &sd_path);

        qom_set_bool(qts, "sd-overcurrent", true);
        qtest_system_reset(qts);
        state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(state, ==, "arm-handoff-ready");
        g_assert_false(qom_get_bool(qts, "boot-sd-overcurrent-check"));
        g_assert_cmpuint(qom_get_uint32(qts, "boot-sd-quirks"), ==, 1);
        g_assert_false(qom_get_bool(qts, "boot-sd-high-speed-enabled"));
        g_assert_cmpuint(
            qom_get_uint32(qts, "boot-sd-clock-limit-hz"), ==, 12500000);
        g_assert_cmpuint(
            qom_get_uint32(qts, "boot-sd-controller-clock-hz"), ==,
            8666666);
        g_assert_false(
            qom_get_bool(qts, "boot-sd-controller-high-speed"));
        g_assert_cmphex(
            qtest_readb(qts, BCM2711_EMMC_BASE + SDHC_HOSTCTL) &
                SDHC_CTRL_HIGH_SPEED, ==, 0);
        g_assert_cmphex(
            qtest_readw(qts, BCM2711_EMMC_BASE + SDHC_CLKCON), ==, 0x0307);
        g_assert_true(qom_get_bool(qts, "boot-sd-overcurrent-warning"));
        g_assert_true(qom_get_bool(qts, "boot-sd-power-enabled"));
        g_assert_cmpuint(qom_get_uint64(
                             qts, "boot-sd-overcurrent-retries"), ==, 0);

        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(sd_path);
    }

    {
        g_autofree char *path = NULL;
        g_autofree char *state = NULL;
        QTestState *qts = start_with_eeprom(
            "BOOT_ORDER=0x1\nSD_OVERCURRENT_CHECK=2\n", false, &path);

        state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(state, ==, "eeprom-invalid");
        qtest_quit(qts);
        unlink(path);
    }

    {
        g_autofree char *path = NULL;
        g_autofree char *state = NULL;
        QTestState *qts = start_with_eeprom(
            "BOOT_ORDER=0x1\nSD_QUIRKS=2\n", false, &path);

        state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(state, ==, "eeprom-invalid");
        qtest_quit(qts);
        unlink(path);
    }

    {
        g_autofree uint8_t *eeprom = make_eeprom_image(
            "BOOT_ORDER=0xe1\nSD_QUIRKS=1\n", false);
        g_autofree char *path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *state = NULL;
        QTestState *qts;

        write_temp_image("raspi4-eeprom-XXXXXX", eeprom, EEPROM_SIZE, &path);
        command = g_strdup_printf(
            "-M raspi-cm4,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "sd-overcurrent=on "
            "-drive if=none,id=pieeprom,format=raw,file=%s",
            path);
        qts = qtest_init(command);
        state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(state, !=, "sd-overcurrent-wait");
        g_assert_cmpuint(qom_get_uint64(
                             qts, "boot-sd-overcurrent-retries"), ==, 0);
        g_assert_true(qom_get_bool(qts, "boot-sd-power-enabled"));
        g_assert_cmpuint(qom_get_uint32(qts, "boot-sd-quirks"), ==, 0);
        g_assert_true(qom_get_bool(qts, "boot-sd-high-speed-enabled"));
        g_assert_cmpuint(
            qom_get_uint32(qts, "boot-sd-clock-limit-hz"), ==, 0);
        g_assert_cmpuint(
            qom_get_uint32(qts, "boot-sd-controller-clock-hz"), ==, 0);
        g_assert_false(
            qom_get_bool(qts, "boot-sd-controller-high-speed"));
        qtest_quit(qts);
        unlink(path);
    }
}
void test_sd_overcurrent_migration(void)
{
    static const uint8_t firmware[] = "migrated SD over-current recovery";
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_path = NULL;
    g_autofree char *state = NULL;
    QTestState *destination;
    QTestState *qts;
    QTestState *seed;

    seed = start_with_eeprom_and_sd(
        "BOOT_ORDER=0x1\nSD_QUIRKS=1\n", "START4  ELF",
        firmware, sizeof(firmware), NULL, &eeprom_path, &sd_path);
    qtest_quit(seed);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom -m 2G "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=sd,format=raw,file=%s,file.locking=off -nic none",
        eeprom_path, sd_path);
    qts = qtest_init(command);
    qom_set_bool(qts, "sd-overcurrent", true);
    qtest_system_reset(qts);
    qtest_clock_step(qts, INT64_C(2) * 1000 * 1000 * 1000);

    destination = migrate_to_new_qtest(qts, command, &migration_dir,
                                       &migration_path);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "sd-overcurrent-wait");
    g_assert_cmpuint(
        qom_get_uint32(destination, "boot-sd-quirks"), ==, 1);
    g_assert_false(
        qom_get_bool(destination, "boot-sd-high-speed-enabled"));
    g_assert_cmpuint(
        qom_get_uint32(destination, "boot-sd-clock-limit-hz"), ==, 12500000);
    g_assert_cmpuint(
        qom_get_uint32(destination, "boot-sd-controller-clock-hz"), ==,
        8666666);
    g_assert_false(
        qom_get_bool(destination, "boot-sd-controller-high-speed"));
    g_assert_cmphex(
        qtest_readw(destination, BCM2711_EMMC_BASE + SDHC_CLKCON), ==,
        0x0307);
    g_assert_true(qom_get_bool(destination, "sd-overcurrent"));
    g_assert_false(qom_get_bool(destination, "boot-sd-power-enabled"));
    g_assert_cmpuint(qom_get_uint64(
                         destination,
                         "boot-sd-overcurrent-remaining-ns"),
                     ==, INT64_C(3) * 1000 * 1000 * 1000);
    qom_set_bool(destination, "sd-overcurrent", false);
    g_assert_false(qom_get_bool(destination, "sd-overcurrent"));
    qtest_clock_step(destination,
                     INT64_C(3) * 1000 * 1000 * 1000 - 1);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "sd-overcurrent-wait");
    qtest_clock_step(destination, 1);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_true(qom_get_bool(destination, "boot-sd-power-enabled"));
    g_assert_cmpuint(qom_get_uint64(
                         destination, "boot-sd-overcurrent-retries"), ==, 1);
    g_assert_cmpuint(qom_get_uint64(destination, "boot-elapsed-ms"), ==, 5000);

    qtest_system_reset(destination);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpuint(
        qom_get_uint32(destination, "boot-sd-controller-clock-hz"), ==,
        8666666);
    g_assert_false(
        qom_get_bool(destination, "boot-sd-controller-high-speed"));
    g_assert_cmphex(
        qtest_readb(destination, BCM2711_EMMC_BASE + SDHC_HOSTCTL) &
            SDHC_CTRL_HIGH_SPEED, ==, 0);
    g_assert_cmphex(
        qtest_readw(destination, BCM2711_EMMC_BASE + SDHC_CLKCON), ==,
        0x0307);

    qtest_quit(qts);
    qtest_quit(destination);
    unlink(eeprom_path);
    unlink(sd_path);
    unlink(migration_path);
    rmdir(migration_dir);
}
void test_sd_recovery_update(void)
{
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree uint8_t *expected = NULL;
    g_autofree char *state = NULL;
    g_autofree char *status = NULL;
    g_autofree char *source = NULL;
    g_autofree char *sd_contents = NULL;
    size_t sd_size;
    QTestState *qts = start_recovery(false, false, UINT64_MAX, false,
                                     &eeprom_path, &sd_path, &expected);

    state = qom_get_string(qts, "boot-state");
    status = qom_get_string(qts, "recovery-status");
    g_assert_cmpstr(state, ==, "recovery-updated-reboot");
    g_assert_cmpstr(status, ==, "recovery-updated-reboot");

    qtest_clock_step(qts, 10 * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "usb-discovery-wait");
    qtest_clock_step(qts, INT64_C(20) * 1000 * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "boot-source");
    g_assert_cmpstr(state, ==, "restart-loop");
    g_assert_cmpstr(source, ==, "restart");
    qtest_quit(qts);

    assert_file_equals(eeprom_path, expected, EEPROM_SIZE);
    g_assert_true(g_file_get_contents(sd_path, &sd_contents, &sd_size, NULL));
    g_assert_cmpuint(sd_size, ==, SD_SIZE);
    g_assert_cmpmem(sd_contents + SD_PARTITION_LBA * 512 +
                    (1 + FAT_SECTORS) * 512, 11, "RECOVERY000", 11);
    unlink(eeprom_path);
    unlink(sd_path);
}
void test_sd_recovery_executable_validation(void)
{
    enum {
        RECOVERY_TRUST_MISSING,
        RECOVERY_TRUST_WRONG,
        RECOVERY_LENGTH_INVALID,
        RECOVERY_KEY_INVALID,
        RECOVERY_RSA_INVALID,
        RECOVERY_HMAC_INVALID,
        RECOVERY_TRUNCATED,
    };
    static const struct {
        unsigned int fault;
        const char *status;
        bool envelope_valid;
    } cases[] = {
        { RECOVERY_TRUST_MISSING, "recovery-trust-required", true },
        { RECOVERY_TRUST_WRONG, "recovery-trust-mismatch", true },
        { RECOVERY_LENGTH_INVALID, "recovery-format-invalid", false },
        { RECOVERY_KEY_INVALID, "recovery-format-invalid", false },
        { RECOVERY_RSA_INVALID, "recovery-format-invalid", false },
        { RECOVERY_HMAC_INVALID, "recovery-format-invalid", false },
        { RECOVERY_TRUNCATED, "recovery-format-invalid", false },
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree uint8_t *recovery = make_test_recovery();
        g_autofree uint8_t *initial =
            make_eeprom_image("BOOT_ORDER=0xf41\n", false);
        g_autofree uint8_t *update =
            make_eeprom_image("BOOT_ORDER=0xf14\n", false);
        g_autofree char *update_digest = g_compute_checksum_for_data(
            G_CHECKSUM_SHA256, update, EEPROM_SIZE);
        g_autofree char *signature = g_strdup_printf(
            "%s\nts: 1\n", update_digest);
        g_autofree char *recovery_digest = NULL;
        g_autofree char *trust_option = NULL;
        g_autofree char *eeprom_path = NULL;
        g_autofree char *sd_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *status = NULL;
        g_autofree char *observed_digest = NULL;
        uint32_t expected_key_index;
        size_t recovery_size = TEST_RECOVERY_SIZE;
        size_t rsa_offset =
            TEST_RECOVERY_PAYLOAD_SIZE + sizeof(uint32_t) * 2;
        Fat16Builder builder;
        QTestState *qts;

        switch (cases[i].fault) {
        case RECOVERY_TRUST_MISSING:
        case RECOVERY_TRUST_WRONG:
            break;
        case RECOVERY_LENGTH_INVALID:
            stl_le_p(recovery + TEST_RECOVERY_PAYLOAD_SIZE,
                     TEST_RECOVERY_PAYLOAD_SIZE + 1);
            break;
        case RECOVERY_KEY_INVALID:
            stl_le_p(recovery + TEST_RECOVERY_PAYLOAD_SIZE +
                     sizeof(uint32_t), 5);
            break;
        case RECOVERY_RSA_INVALID:
            memset(recovery + rsa_offset, 0, TEST_RECOVERY_RSA_SIZE);
            break;
        case RECOVERY_HMAC_INVALID:
            memset(recovery + rsa_offset + TEST_RECOVERY_RSA_SIZE, 0,
                   TEST_RECOVERY_HMAC_SIZE);
            break;
        case RECOVERY_TRUNCATED:
            recovery_size = TEST_RECOVERY_SIZE - 6;
            break;
        default:
            g_assert_not_reached();
        }

        recovery_digest = g_compute_checksum_for_data(
            G_CHECKSUM_SHA256, recovery, recovery_size);
        if (cases[i].fault == RECOVERY_TRUST_MISSING) {
            trust_option = g_strdup("");
        } else if (cases[i].fault == RECOVERY_TRUST_WRONG) {
            trust_option = g_strdup(
                ",recovery-trusted-sha256="
                "0000000000000000000000000000000000000000000000000000000000000000");
        } else {
            trust_option = g_strdup_printf(
                ",recovery-trusted-sha256=%s", recovery_digest);
        }

        fat16_init(&builder);
        fat16_add_file(&builder, "RECOVERYBIN", recovery, recovery_size);
        fat16_add_file(&builder, "PIEEPROMUPD", update, EEPROM_SIZE);
        fat16_add_file(&builder, "PIEEPROMSIG",
                       (const uint8_t *)signature, strlen(signature));
        write_temp_image("raspi4-recovery-validation-sd-XXXXXX",
                         builder.image, SD_SIZE, &sd_path);
        g_free(builder.image);
        write_temp_image("raspi4-recovery-validation-eeprom-XXXXXX",
                         initial, EEPROM_SIZE, &eeprom_path);
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom%s "
            "-drive if=none,id=pieeprom,format=raw,file=%s,"
            "file.locking=off "
            "-drive if=sd,format=raw,file=%s,file.locking=off",
            trust_option, eeprom_path, sd_path);
        qts = qtest_init(command);
        status = qom_get_string(qts, "recovery-status");
        observed_digest = qom_get_string(qts, "recovery-sha256");
        g_assert_cmpstr(status, ==, cases[i].status);
        g_assert_cmpstr(observed_digest, ==, recovery_digest);
        expected_key_index = UINT32_MAX;
        if (cases[i].envelope_valid) {
            expected_key_index = 1;
        }
        g_assert_cmpuint(qom_get_uint32(qts, "recovery-key-index"), ==,
                         expected_key_index);
        qtest_quit(qts);
        assert_file_equals(eeprom_path, initial, EEPROM_SIZE);
        unlink(eeprom_path);
        unlink(sd_path);
    }
}
void test_sd_recovery_safeguards(void)
{
    static const struct {
        bool bad_signature;
        bool write_protect;
        const char *state;
    } cases[] = {
        { true, false, "recovery-signature-invalid" },
        { false, true, "recovery-write-protected" },
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree char *eeprom_path = NULL;
        g_autofree char *sd_path = NULL;
        g_autofree uint8_t *expected = NULL;
        g_autofree uint8_t *initial =
            make_eeprom_image("BOOT_ORDER=0xf41\n", false);
        QTestState *qts = start_recovery(
            cases[i].bad_signature, cases[i].write_protect, UINT64_MAX,
            false, &eeprom_path, &sd_path, &expected);
        g_autofree char *state = qom_get_string(qts, "boot-state");

        g_assert_cmpstr(state, ==, cases[i].state);
        qtest_quit(qts);
        assert_file_equals(eeprom_path, initial, EEPROM_SIZE);
        unlink(eeprom_path);
        unlink(sd_path);
    }
}
void test_sd_recovery_write_protect_contract(void)
{
    static const struct {
        bool status_protected;
        bool nwp;
        const char *config;
        const char *recovery_status;
        bool updated;
        bool final_status_protected;
        bool sampled;
    } cases[] = {
        { false, false, "eeprom_write_protect=-1\n",
          "recovery-updated-reboot", true, false, false },
        { true, true, "eeprom_write_protect=0\n",
          "recovery-updated-reboot", true, false, true },
        { true, false, "eeprom_write_protect=0\n",
          "recovery-write-protect-config-locked", false, true, false },
        { false, false, "eeprom_write_protect=1\n",
          "recovery-write-protect-config-locked", false, false, false },
        { false, true, "eeprom_write_protect=2\n",
          "recovery-write-protect-config-invalid", false, false, false },
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree uint8_t *initial =
            make_eeprom_image("BOOT_ORDER=0xf41\n", false);
        g_autofree uint8_t *expected = NULL;
        g_autofree char *eeprom_path = NULL;
        g_autofree char *sd_path = NULL;
        g_autofree char *machine_extra = NULL;
        g_autofree char *status = NULL;
        const char *nwp_value = "off";
        const uint8_t *expected_media = initial;
        QTestState *qts;

        if (cases[i].nwp) {
            nwp_value = "on";
        }
        machine_extra = g_strdup_printf(",eeprom-nwp=%s", nwp_value);
        qts = start_recovery_stage_extra(
            false, cases[i].status_protected, UINT64_MAX, "program",
            false, false, NULL, machine_extra, cases[i].config, NULL,
            &eeprom_path, &sd_path, &expected);

        if (cases[i].updated) {
            expected_media = expected;
        }
        status = qom_get_string(qts, "recovery-status");
        g_assert_cmpstr(status, ==, cases[i].recovery_status);
        g_assert_cmpint(qtest_qom_get_bool(
                            qts, "/machine",
                            "eeprom-status-write-protect"),
                        ==, cases[i].final_status_protected);
        if (strcmp(cases[i].recovery_status,
                   "recovery-write-protect-config-invalid")) {
            g_assert_cmpint(qtest_qom_get_bool(
                                qts, "/machine", "eeprom-nwp-sampled"),
                            ==, cases[i].sampled);
        }
        qtest_quit(qts);
        assert_file_equals(eeprom_path, expected_media, EEPROM_SIZE);
        unlink(eeprom_path);
        unlink(sd_path);
    }

    {
        g_autofree uint8_t *expected = NULL;
        g_autofree char *eeprom_path = NULL;
        g_autofree char *eeprom_status_path = NULL;
        g_autofree char *sd_path = NULL;
        g_autofree char *status = NULL;
        g_autofree char *recovery_digest = NULL;
        g_autofree char *command = NULL;
        g_autofree char *extra_drive = NULL;
        g_autofree char *migration_dir = NULL;
        g_autofree char *migration_path = NULL;
        uint8_t persisted_status[512] = { 0 };
        QTestState *destination;
        QTestState *relaunch;
        QTestState *qts;

        write_temp_image("raspi4-eeprom-status-XXXXXX", persisted_status,
                         sizeof(persisted_status), &eeprom_status_path);
        extra_drive = g_strdup_printf(
            "-drive if=none,id=wpstatus,format=raw,file=%s,"
            "file.locking=off ",
            eeprom_status_path);
        qts = start_recovery_stage_extra(
            false, false, UINT64_MAX, "program", true, false, NULL,
            ",eeprom-nwp=on,eeprom-status-drive=wpstatus",
            "eeprom_write_protect=1\n", extra_drive,
            &eeprom_path, &sd_path, &expected);

        status = qom_get_string(qts, "recovery-status");
        g_assert_cmpstr(status, ==, "recovery-updated-stop");
        g_assert_true(qtest_qom_get_bool(
                          qts, "/machine",
                          "eeprom-status-write-protect"));
        g_assert_true(qom_get_bool(
                          qts, "boot-eeprom-update-timestamp-valid"));
        g_assert_cmpuint(qom_get_uint32(
                             qts, "boot-eeprom-update-timestamp"), ==, 1);
        qom_set_bool(qts, "eeprom-nwp", false);
        recovery_digest = qom_get_string(qts, "recovery-sha256");
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "recovery-trusted-sha256=%s,eeprom-write-protect=off,"
            "eeprom-nwp=on,eeprom-status-drive=wpstatus "
            "-drive if=none,id=pieeprom,format=raw,file=%s,"
            "file.locking=off "
            "-drive if=none,id=wpstatus,format=raw,file=%s,"
            "file.locking=off "
            "-drive if=sd,format=raw,file=%s,file.locking=off",
            recovery_digest, eeprom_path, eeprom_status_path, sd_path);
        destination = migrate_to_new_qtest(
            qts, command, &migration_dir, &migration_path);
        g_assert_true(qtest_qom_get_bool(
                          destination, "/machine",
                          "eeprom-status-write-protect"));
        g_assert_false(qtest_qom_get_bool(
                           destination, "/machine", "eeprom-nwp"));
        g_assert_true(qtest_qom_get_bool(
                          destination, "/machine",
                          "eeprom-nwp-sampled"));

        qtest_system_reset(destination);
        g_clear_pointer(&status, g_free);
        status = qom_get_string(destination, "recovery-status");
        g_assert_cmpstr(status, ==, "recovery-write-protected");
        g_assert_true(qtest_qom_get_bool(
                          destination, "/machine",
                          "eeprom-status-write-protect"));
        qtest_quit(qts);
        qtest_quit(destination);
        assert_file_equals(eeprom_path, expected, EEPROM_SIZE);

        relaunch = qtest_init(command);
        g_assert_true(qtest_qom_get_bool(
                          relaunch, "/machine",
                          "eeprom-status-write-protect"));
        g_clear_pointer(&status, g_free);
        status = qom_get_string(relaunch, "recovery-status");
        g_assert_cmpstr(status, ==, "recovery-write-protected");
        qom_set_bool(relaunch, "eeprom-status-write-protect", false);
        qtest_quit(relaunch);
        persisted_status[1] = 1;
        stl_le_p(persisted_status + 2, 1);
        assert_file_equals(eeprom_status_path, persisted_status,
                           sizeof(persisted_status));

        unlink(eeprom_path);
        unlink(eeprom_status_path);
        unlink(sd_path);
        unlink(migration_path);
        rmdir(migration_dir);
    }
}
void test_sd_recovery_nor_semantics(void)
{
    g_autofree uint8_t *expected = NULL;
    g_autofree uint8_t *faulted = g_malloc(EEPROM_SIZE);
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree char *status = NULL;
    g_autofree char *stage = NULL;
    QTestState *qts = start_recovery_stage_extra(
        false, false, UINT64_MAX, "program", false, false, NULL,
        ",eeprom-stuck-zero-offset=0,eeprom-stuck-zero-mask=5",
        NULL, NULL, &eeprom_path, &sd_path, &expected);

    status = qom_get_string(qts, "recovery-status");
    stage = qom_get_string(qts, "eeprom-flash-stage");
    g_assert_cmpstr(status, ==, "recovery-nor-violation");
    g_assert_cmpstr(stage, ==, "program");
    g_assert_cmpuint(qom_get_uint64(qts, "eeprom-erased-bytes"),
                     ==, EEPROM_SIZE);
    g_assert_cmpuint(qom_get_uint64(qts, "eeprom-programmed-bytes"),
                     ==, 256);
    g_assert_cmpuint(qom_get_uint32(qts, "eeprom-program-page-count"),
                     ==, 1);
    g_assert_cmpuint(qom_get_uint32(qts, "eeprom-nor-violation-bits"),
                     ==, 2);
    g_assert_cmpuint(qom_get_uint64(qts, "eeprom-verified-bytes"), ==, 0);

    memset(faulted, 0xff, EEPROM_SIZE);
    memcpy(faulted, expected, 256);
    faulted[0] &= ~5;
    assert_file_equals(eeprom_path, faulted, EEPROM_SIZE);

    qom_set_uint64(qts, "eeprom-stuck-zero-offset", UINT64_MAX);
    qtest_system_reset(qts);
    g_clear_pointer(&status, g_free);
    status = qom_get_string(qts, "recovery-status");
    g_assert_cmpstr(status, ==, "recovery-updated-reboot");
    g_assert_cmpuint(qom_get_uint32(qts, "eeprom-program-page-count"),
                     ==, EEPROM_SIZE / 256);
    g_assert_cmpuint(qom_get_uint32(qts, "eeprom-nor-violation-bits"),
                     ==, 0);

    qtest_quit(qts);
    assert_file_equals(eeprom_path, expected, EEPROM_SIZE);
    unlink(eeprom_path);
    unlink(sd_path);
}
void test_sd_recovery_fragmented_and_cyclic_files(void)
{
    g_autofree uint8_t *recovery = make_test_recovery();
    g_autofree char *recovery_digest = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, recovery, TEST_RECOVERY_SIZE);
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree uint8_t *expected = NULL;
    g_autofree char *state = NULL;
    QTestState *qts = start_recovery_stage(
        false, false, UINT64_MAX, "program", false, true, NULL,
        &eeprom_path, &sd_path, &expected);

    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "recovery-updated-reboot");
    qtest_quit(qts);
    assert_file_equals(eeprom_path, expected, EEPROM_SIZE);
    unlink(eeprom_path);
    unlink(sd_path);
    g_clear_pointer(&eeprom_path, g_free);
    g_clear_pointer(&sd_path, g_free);

    {
        g_autofree uint8_t *initial =
            make_eeprom_image("BOOT_ORDER=0xf41\n", false);
        g_autofree uint8_t *repeated =
            make_eeprom_image("BOOT_ORDER=0xf14\n", false);
        g_autofree char *digest = NULL;
        g_autofree char *signature = NULL;
        g_autofree char *command = NULL;
        uint16_t first_cluster;
        uint8_t *partition;
        uint8_t *fat;
        Fat16Builder builder;

        for (size_t offset = 512; offset < EEPROM_SIZE; offset += 512) {
            memcpy(repeated + offset, repeated, 512);
        }
        /*
         * Sign exactly what an unchecked self-loop would synthesize.  The
         * negative result therefore proves cycle rejection, not a digest
         * mismatch caused by unrelated payload bytes.
         */
        digest = g_compute_checksum_for_data(
            G_CHECKSUM_SHA256, repeated, EEPROM_SIZE);
        signature = g_strdup_printf("%s\nts: 1\n", digest);

        fat16_init(&builder);
        fat16_add_file(&builder, "RECOVERYBIN", recovery,
                       TEST_RECOVERY_SIZE);
        first_cluster = fat16_add_fragmented_file(
            &builder, "PIEEPROMUPD", repeated, EEPROM_SIZE);
        fat16_add_file(&builder, "PIEEPROMSIG", (uint8_t *)signature,
                       strlen(signature));
        partition = builder.image + SD_PARTITION_LBA * 512;
        fat = partition + 512;
        stw_le_p(fat + first_cluster * 2, first_cluster);

        write_temp_image("raspi4-sd-cycle-XXXXXX", builder.image, SD_SIZE,
                         &sd_path);
        g_free(builder.image);
        write_temp_image("raspi4-eeprom-XXXXXX", initial, EEPROM_SIZE,
                         &eeprom_path);
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "recovery-trusted-sha256=%s "
            "-drive if=none,id=pieeprom,format=raw,file=%s,"
            "file.locking=off "
            "-drive if=sd,format=raw,file=%s,file.locking=off",
            recovery_digest, eeprom_path, sd_path);
        qts = qtest_init(command);
        g_clear_pointer(&state, g_free);
        state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(state, ==, "recovery-signature-invalid");
        qtest_quit(qts);
        assert_file_equals(eeprom_path, initial, EEPROM_SIZE);
        unlink(eeprom_path);
        unlink(sd_path);
    }
}
void test_sd_recovery_fat12_fat32_fragmented_and_malformed(void)
{
    static const unsigned int fat_types[] = { 12, 32 };

    for (size_t type_index = 0;
         type_index < ARRAY_SIZE(fat_types); type_index++) {
        unsigned int fat_type = fat_types[type_index];
        g_autofree uint8_t *initial =
            make_eeprom_image("BOOT_ORDER=0xf41\n", false);
        g_autofree uint8_t *update =
            make_eeprom_image("BOOT_ORDER=0xf14\n", false);
        g_autofree char *update_digest = g_compute_checksum_for_data(
            G_CHECKSUM_SHA256, update, EEPROM_SIZE);
        g_autofree char *signature = g_strdup_printf(
            "%s\nts: 1\n", update_digest);
        g_autofree char *eeprom_path = NULL;
        g_autofree char *sd_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *state = NULL;
        g_autofree char *status = NULL;
        g_autofree char *recovery_digest = NULL;
        size_t recovery_size;
        g_autofree uint8_t *recovery =
            make_test_recovery_sized(8192, &recovery_size);
        FatVariantBuilder builder;
        QTestState *qts;

        recovery_digest = g_compute_checksum_for_data(
            G_CHECKSUM_SHA256, recovery, recovery_size);
        fat_variant_init(&builder, fat_type);
        fat_variant_add_file(&builder, "RECOVERYBIN", recovery,
                             recovery_size, true);
        fat_variant_add_file(&builder, "PIEEPROMUPD", update,
                             EEPROM_SIZE, true);
        fat_variant_add_file(&builder, "PIEEPROMSIG",
                             (uint8_t *)signature, strlen(signature), true);
        write_temp_image("raspi4-recovery-fat-XXXXXX", builder.image,
                         builder.image_size, &sd_path);
        g_free(builder.image);
        write_temp_image("raspi4-recovery-eeprom-XXXXXX", initial,
                         EEPROM_SIZE, &eeprom_path);
        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "recovery-trusted-sha256=%s "
            "-drive if=none,id=pieeprom,format=raw,file=%s,"
            "file.locking=off "
            "-drive if=sd,format=raw,file=%s,file.locking=off",
            recovery_digest, eeprom_path, sd_path);
        qts = qtest_init(command);
        state = qom_get_string(qts, "boot-state");
        status = qom_get_string(qts, "recovery-status");
        g_assert_cmpstr(state, ==, "recovery-updated-reboot");
        g_assert_cmpstr(status, ==, "recovery-updated-reboot");
        qtest_quit(qts);
        assert_file_equals(eeprom_path, update, EEPROM_SIZE);
        unlink(eeprom_path);
        unlink(sd_path);
    }

    for (size_t type_index = 0;
         type_index < ARRAY_SIZE(fat_types); type_index++) {
        unsigned int fat_type = fat_types[type_index];

        for (unsigned int fault = 0; fault < 2; fault++) {
            g_autofree uint8_t *initial =
                make_eeprom_image("BOOT_ORDER=0xf41\n", false);
            g_autofree uint8_t *repeated =
                make_eeprom_image("BOOT_ORDER=0xf14\n", false);
            g_autofree char *signature = NULL;
            g_autofree char *eeprom_path = NULL;
            g_autofree char *sd_path = NULL;
            g_autofree char *command = NULL;
            g_autofree char *state = NULL;
            g_autofree char *recovery_digest = NULL;
            size_t recovery_size;
            g_autofree uint8_t *recovery =
                make_test_recovery_sized(8192, &recovery_size);
            size_t cluster_bytes;
            uint32_t first_cluster;
            FatVariantBuilder builder;
            QTestState *qts;

            fat_variant_init(&builder, fat_type);
            cluster_bytes = builder.sectors_per_cluster * 512;
            for (size_t offset = cluster_bytes;
                 offset < EEPROM_SIZE; offset += cluster_bytes) {
                memcpy(repeated + offset, repeated,
                       MIN(cluster_bytes, EEPROM_SIZE - offset));
            }
            {
                g_autofree char *digest = g_compute_checksum_for_data(
                    G_CHECKSUM_SHA256, repeated, EEPROM_SIZE);

                signature = g_strdup_printf("%s\nts: 1\n", digest);
            }
            recovery_digest = g_compute_checksum_for_data(
                G_CHECKSUM_SHA256, recovery, recovery_size);
            fat_variant_add_file(&builder, "RECOVERYBIN", recovery,
                                 recovery_size, true);
            first_cluster = fat_variant_add_file(
                &builder, "PIEEPROMUPD", repeated, EEPROM_SIZE, true);
            fat_variant_add_file(&builder, "PIEEPROMSIG",
                                 (uint8_t *)signature, strlen(signature),
                                 false);
            if (fault == 0) {
                fat_variant_set_entry(&builder, first_cluster,
                                      first_cluster);
            } else {
                fat_variant_set_entry(
                    &builder, first_cluster,
                    fat_type == 12 ? 0x0ff7 : 0x0ffffff7);
            }
            write_temp_image("raspi4-recovery-fat-malformed-XXXXXX",
                             builder.image, builder.image_size, &sd_path);
            g_free(builder.image);
            write_temp_image("raspi4-recovery-eeprom-XXXXXX", initial,
                             EEPROM_SIZE, &eeprom_path);
            command = g_strdup_printf(
                "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
                "recovery-trusted-sha256=%s "
                "-drive if=none,id=pieeprom,format=raw,file=%s,"
                "file.locking=off "
                "-drive if=sd,format=raw,file=%s,file.locking=off",
                recovery_digest, eeprom_path, sd_path);
            qts = qtest_init(command);
            state = qom_get_string(qts, "boot-state");
            g_assert_cmpstr(state, ==, "recovery-signature-invalid");
            qtest_quit(qts);
            assert_file_equals(eeprom_path, initial, EEPROM_SIZE);
            unlink(eeprom_path);
            unlink(sd_path);
        }
    }
}
void test_sd_recovery_interrupted_resume(void)
{
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree uint8_t *expected = NULL;
    g_autofree char *state = NULL;
    g_autofree char *source = NULL;
    QTestState *qts = start_recovery(false, false, 1024, false,
                                     &eeprom_path, &sd_path, &expected);

    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "recovery-interrupted");

    qom_set_uint64(qts, "eeprom-fail-after", EEPROM_SIZE);
    qtest_system_reset(qts);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "recovery-updated-reboot");

    qtest_system_reset(qts);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "usb-discovery-wait");
    qtest_clock_step(qts, INT64_C(20) * 1000 * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    source = qom_get_string(qts, "boot-source");
    g_assert_cmpstr(state, ==, "restart-loop");
    g_assert_cmpstr(source, ==, "restart");
    qtest_quit(qts);
    assert_file_equals(eeprom_path, expected, EEPROM_SIZE);
    unlink(eeprom_path);
    unlink(sd_path);
}
void test_sd_recovery_stage_faults(void)
{
    static const struct {
        const char *stage;
        uint64_t fail_after;
        const char *status;
        bool renamed;
        uint64_t erased;
        uint64_t programmed;
        uint64_t verified;
        uint32_t dirty_sectors;
        const char *flash_stage;
    } cases[] = {
        { "erase", 4096, "recovery-erase-interrupted", false,
          4096, 0, 0, 1, "erase" },
        { "program", 1024, "recovery-interrupted", false,
          EEPROM_SIZE, 1024, 0, EEPROM_SIZE / 4096, "program" },
        { "verify", 1024, "recovery-verify-interrupted", false,
          EEPROM_SIZE, EEPROM_SIZE, 1024, EEPROM_SIZE / 4096, "verify" },
        { "verify-mismatch", 1024, "recovery-verify-failed", false,
          EEPROM_SIZE, EEPROM_SIZE, 1024, EEPROM_SIZE / 4096, "verify" },
        { "rename", UINT64_MAX, "recovery-rename-interrupted", false,
          EEPROM_SIZE, EEPROM_SIZE, EEPROM_SIZE,
          EEPROM_SIZE / 4096, "complete" },
        { "reboot", UINT64_MAX, "recovery-reboot-interrupted", true,
          EEPROM_SIZE, EEPROM_SIZE, EEPROM_SIZE,
          EEPROM_SIZE / 4096, "complete" },
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree char *eeprom_path = NULL;
        g_autofree char *sd_path = NULL;
        g_autofree uint8_t *expected = NULL;
        g_autofree uint8_t *initial =
            make_eeprom_image("BOOT_ORDER=0xf41\n", false);
        g_autofree uint8_t *media_expected = NULL;
        g_autofree char *contents = NULL;
        g_autofree char *sd_contents = NULL;
        g_autofree char *status = NULL;
        g_autofree char *flash_stage = NULL;
        const char *recovery_name;
        size_t size;
        size_t sd_size;
        QTestState *qts = start_recovery_stage(
            false, false, cases[i].fail_after, cases[i].stage, false, false,
            NULL, &eeprom_path, &sd_path, &expected);

        status = qom_get_string(qts, "recovery-status");
        g_assert_cmpstr(status, ==, cases[i].status);
        flash_stage = qom_get_string(qts, "eeprom-flash-stage");
        g_assert_cmpstr(flash_stage, ==, cases[i].flash_stage);
        g_assert_cmpuint(qom_get_uint64(qts, "eeprom-erased-bytes"),
                         ==, cases[i].erased);
        g_assert_cmpuint(qom_get_uint64(qts, "eeprom-programmed-bytes"),
                         ==, cases[i].programmed);
        g_assert_cmpuint(qom_get_uint64(qts, "eeprom-verified-bytes"),
                         ==, cases[i].verified);
        g_assert_cmpuint(qom_get_uint32(qts, "eeprom-dirty-sector-count"),
                         ==, cases[i].dirty_sectors);
        if (cases[i].renamed) {
            recovery_name = "RECOVERY000";
            qtest_system_reset(qts);
            g_clear_pointer(&status, g_free);
            status = qom_get_string(qts, "recovery-status");
            g_assert_cmpstr(status, ==, "none");
        } else {
            recovery_name = "RECOVERYBIN";
        }
        qtest_quit(qts);

        media_expected = g_memdup2(initial, EEPROM_SIZE);
        if (!strcmp(cases[i].stage, "erase")) {
            memset(media_expected, 0xff, cases[i].fail_after);
        } else if (!strcmp(cases[i].stage, "program")) {
            memset(media_expected, 0xff, EEPROM_SIZE);
            memcpy(media_expected, expected, cases[i].fail_after);
        } else {
            memcpy(media_expected, expected, EEPROM_SIZE);
        }
        g_assert_true(g_file_get_contents(eeprom_path, &contents, &size,
                                          NULL));
        g_assert_cmpuint(size, ==, EEPROM_SIZE);
        g_assert_cmpmem(contents, size, media_expected, EEPROM_SIZE);

        g_assert_true(g_file_get_contents(sd_path, &sd_contents, &sd_size,
                                          NULL));
        g_assert_cmpuint(sd_size, ==, SD_SIZE);
        g_assert_cmpmem(sd_contents + SD_PARTITION_LBA * 512 +
                        (1 + FAT_SECTORS) * 512, 11,
                        recovery_name, 11);
        unlink(eeprom_path);
        unlink(sd_path);
    }
}
void test_sd_recovery_timed_flash_power_cut_migration(void)
{
    static const char machine_extra[] =
        ",eeprom-erase-sector-delay-us=10,"
        "eeprom-program-page-delay-us=5,"
        "eeprom-verify-sector-delay-us=2";
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree uint8_t *expected = NULL;
    g_autofree uint8_t *initial =
        make_eeprom_image("BOOT_ORDER=0xf41\n", false);
    g_autofree char *recovery_sha256 = NULL;
    g_autofree char *command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_path = NULL;
    g_autofree char *status = NULL;
    g_autofree char *contents = NULL;
    gsize contents_size = 0;
    QTestState *destination;
    QTestState *qts = start_recovery_stage_extra(
        false, false, 512, "program", false, false, NULL, machine_extra,
        NULL, NULL, &eeprom_path, &sd_path, &expected);

    status = qom_get_string(qts, "recovery-status");
    g_assert_cmpstr(status, ==, "recovery-flash-active");
    g_assert_cmpuint(qom_get_uint64(qts, "eeprom-erased-bytes"), ==, 0);
    g_assert_cmpuint(qom_get_uint64(qts, "eeprom-flash-elapsed-us"), ==, 0);
    qtest_clock_step(qts, 9 * 1000);
    g_assert_cmpuint(qom_get_uint64(qts, "eeprom-erased-bytes"), ==, 0);
    qtest_clock_step(qts, 1000);
    g_assert_cmpuint(qom_get_uint64(qts, "eeprom-erased-bytes"), ==, 4096);
    g_assert_cmpuint(qom_get_uint64(qts, "eeprom-flash-elapsed-us"), ==, 10);
    g_assert_true(g_file_get_contents(eeprom_path, &contents,
                                      &contents_size, NULL));
    g_assert_cmpuint(contents_size, ==, EEPROM_SIZE);
    for (size_t i = 0; i < 4096; i++) {
        g_assert_cmphex((uint8_t)contents[i], ==, 0xff);
    }
    g_assert_cmpmem(contents + 4096, EEPROM_SIZE - 4096,
                    initial + 4096, EEPROM_SIZE - 4096);

    qtest_system_reset(qts);
    g_clear_pointer(&status, g_free);
    status = qom_get_string(qts, "recovery-status");
    g_assert_cmpstr(status, ==, "recovery-flash-active");
    g_assert_cmpuint(qom_get_uint64(qts, "eeprom-erased-bytes"), ==, 0);
    g_assert_cmpuint(qom_get_uint64(qts, "eeprom-flash-elapsed-us"), ==, 0);
    for (unsigned int i = 0; i < 3; i++) {
        qtest_clock_step_next(qts);
    }
    g_assert_cmpuint(qom_get_uint64(qts, "eeprom-erased-bytes"),
                     ==, 3 * 4096);
    g_assert_cmpuint(qom_get_uint64(qts, "eeprom-flash-elapsed-us"), ==, 30);

    QDict *failure = qtest_qmp_assert_failure_ref(
        qts, "{ 'execute': 'qom-set', 'arguments': {"
             "'path': '/machine',"
             "'property': 'eeprom-program-page-delay-us',"
             "'value': 99 } }");
    g_assert_nonnull(strstr(qdict_get_str(failure, "desc"),
                            "during active flash"));
    qobject_unref(failure);

    recovery_sha256 = qom_get_string(qts, "recovery-sha256");
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "recovery-trusted-sha256=%s,eeprom-write-protect=off,"
        "eeprom-fail-after=512,eeprom-fail-stage=program%s "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=sd,format=raw,file=%s,file.locking=off",
        recovery_sha256, machine_extra, eeprom_path, sd_path);
    destination = migrate_to_new_qtest(qts, command, &migration_dir,
                                       &migration_path);
    g_assert_cmpuint(qom_get_uint64(destination, "eeprom-erased-bytes"),
                     ==, 3 * 4096);
    g_assert_cmpuint(qom_get_uint64(destination,
                                   "eeprom-flash-elapsed-us"), ==, 30);

    qtest_clock_step(destination, 1260 * 1000);
    g_clear_pointer(&status, g_free);
    status = qom_get_string(destination, "recovery-status");
    g_assert_cmpstr(status, ==, "recovery-interrupted");
    g_assert_cmpuint(qom_get_uint64(destination, "eeprom-erased-bytes"),
                     ==, EEPROM_SIZE);
    g_assert_cmpuint(qom_get_uint64(destination, "eeprom-programmed-bytes"),
                     ==, 512);
    g_assert_cmpuint(qom_get_uint64(destination,
                                   "eeprom-flash-elapsed-us"), ==, 1290);

    qom_set_uint64(destination, "eeprom-fail-after", EEPROM_SIZE);
    qtest_system_reset(destination);
    qtest_clock_step(destination, 11776 * 1000);
    g_clear_pointer(&status, g_free);
    status = qom_get_string(destination, "recovery-status");
    g_assert_cmpstr(status, ==, "recovery-updated-reboot");
    g_assert_cmpuint(qom_get_uint64(destination, "eeprom-erased-bytes"),
                     ==, EEPROM_SIZE);
    g_assert_cmpuint(qom_get_uint64(destination, "eeprom-programmed-bytes"),
                     ==, EEPROM_SIZE);
    g_assert_cmpuint(qom_get_uint64(destination, "eeprom-verified-bytes"),
                     ==, EEPROM_SIZE);
    g_assert_cmpuint(qom_get_uint64(destination,
                                   "eeprom-flash-elapsed-us"), ==, 11776);

    qtest_quit(qts);
    qtest_quit(destination);
    assert_file_equals(eeprom_path, expected, EEPROM_SIZE);
    unlink(eeprom_path);
    unlink(sd_path);
    unlink(migration_path);
    rmdir(migration_dir);
}
void test_sd_recovery_verify_mismatch_retry_migration(void)
{
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree uint8_t *expected = NULL;
    g_autofree char *command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_path = NULL;
    g_autofree char *recovery_sha256 = NULL;
    g_autofree char *status = NULL;
    g_autofree char *sd_contents = NULL;
    size_t sd_size;
    QTestState *destination;
    QTestState *qts = start_recovery_stage(
        false, false, 1024, "verify-mismatch", false, false, NULL,
        &eeprom_path, &sd_path, &expected);

    status = qom_get_string(qts, "recovery-status");
    g_assert_cmpstr(status, ==, "recovery-verify-failed");
    g_assert_cmpuint(qom_get_uint64(qts, "eeprom-erased-bytes"),
                     ==, EEPROM_SIZE);
    g_assert_cmpuint(qom_get_uint64(qts, "eeprom-programmed-bytes"),
                     ==, EEPROM_SIZE);
    g_assert_cmpuint(qom_get_uint64(qts, "eeprom-verified-bytes"), ==, 1024);
    g_assert_cmpuint(qom_get_uint32(qts, "eeprom-dirty-sector-count"),
                     ==, EEPROM_SIZE / 4096);
    g_assert_cmpuint(qom_get_uint32(qts, "eeprom-program-page-count"),
                     ==, EEPROM_SIZE / 256);
    g_assert_cmpuint(qom_get_uint32(qts, "eeprom-nor-violation-bits"),
                     ==, 0);
    assert_file_equals(eeprom_path, expected, EEPROM_SIZE);
    recovery_sha256 = qom_get_string(qts, "recovery-sha256");

    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "recovery-trusted-sha256=%s,"
        "eeprom-write-protect=off,eeprom-fail-after=1024,"
        "eeprom-fail-stage=verify-mismatch "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=sd,format=raw,file=%s,file.locking=off",
        recovery_sha256, eeprom_path, sd_path);
    destination = migrate_to_new_qtest(
        qts, command, &migration_dir, &migration_path);
    g_clear_pointer(&status, g_free);
    status = qom_get_string(destination, "recovery-status");
    g_assert_cmpstr(status, ==, "recovery-verify-failed");
    g_assert_cmpuint(qom_get_uint64(destination, "eeprom-erased-bytes"),
                     ==, EEPROM_SIZE);
    g_assert_cmpuint(qom_get_uint64(destination, "eeprom-programmed-bytes"),
                     ==, EEPROM_SIZE);
    g_assert_cmpuint(qom_get_uint64(destination, "eeprom-verified-bytes"),
                     ==, 1024);
    g_assert_cmpuint(qom_get_uint32(destination, "eeprom-dirty-sector-count"),
                     ==, EEPROM_SIZE / 4096);
    g_assert_cmpuint(
        qom_get_uint32(destination, "eeprom-program-page-count"),
        ==, EEPROM_SIZE / 256);
    g_assert_cmpuint(
        qom_get_uint32(destination, "eeprom-nor-violation-bits"), ==, 0);

    qom_set_uint64(destination, "eeprom-fail-after", EEPROM_SIZE);
    qtest_system_reset(destination);
    g_clear_pointer(&status, g_free);
    status = qom_get_string(destination, "recovery-status");
    g_assert_cmpstr(status, ==, "recovery-updated-reboot");
    g_assert_cmpuint(qom_get_uint64(destination, "eeprom-verified-bytes"),
                     ==, EEPROM_SIZE);

    qtest_quit(qts);
    qtest_quit(destination);
    assert_file_equals(eeprom_path, expected, EEPROM_SIZE);
    g_assert_true(g_file_get_contents(sd_path, &sd_contents, &sd_size,
                                      NULL));
    g_assert_cmpuint(sd_size, ==, SD_SIZE);
    g_assert_cmpmem(sd_contents + SD_PARTITION_LBA * 512 +
                    (1 + FAT_SECTORS) * 512, 11,
                    "RECOVERY000", 11);
    unlink(eeprom_path);
    unlink(sd_path);
    unlink(migration_path);
    rmdir(migration_dir);
}
void test_sd_recovery_backend_io_error_retry(void)
{
    static const struct {
        const char *event;
        bool erase_completed;
    } cases[] = {
        { "write_aio", false },
        { "read_aio", true },
    };

    for (unsigned int i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree char *eeprom_path = NULL;
        g_autofree char *sd_path = NULL;
        g_autofree char *debug_path = NULL;
        g_autofree char *debug_config = g_strdup_printf(
            "[inject-error]\n"
            "event = \"%s\"\n"
            "errno = \"5\"\n"
            "once = \"on\"\n"
            "immediately = \"on\"\n",
            cases[i].event);
        g_autofree uint8_t *expected = NULL;
        g_autofree uint8_t *initial =
            make_eeprom_image("BOOT_ORDER=0xf41\n", false);
        g_autofree uint8_t *erased = g_malloc(EEPROM_SIZE);
        g_autofree char *status = NULL;
        const uint8_t *expected_after_failure = initial;
        QTestState *qts;

        memset(erased, 0xff, EEPROM_SIZE);
        if (cases[i].erase_completed) {
            expected_after_failure = erased;
        }
        write_temp_text("raspi4-blkdebug-XXXXXX", debug_config,
                        &debug_path);
        qts = start_recovery_stage(
            false, false, UINT64_MAX, "program", false, false, debug_path,
            &eeprom_path, &sd_path, &expected);
        status = qom_get_string(qts, "recovery-status");
        g_assert_cmpstr(status, ==, "recovery-program-error");
        assert_file_equals(eeprom_path, expected_after_failure,
                           EEPROM_SIZE);

        qtest_system_reset(qts);
        g_clear_pointer(&status, g_free);
        status = qom_get_string(qts, "recovery-status");
        g_assert_cmpstr(status, ==, "recovery-updated-reboot");
        qtest_quit(qts);
        assert_file_equals(eeprom_path, expected, EEPROM_SIZE);
        unlink(eeprom_path);
        unlink(sd_path);
        unlink(debug_path);
    }
}
void test_sd_recovery_permanent_file(void)
{
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree uint8_t *expected = NULL;
    g_autofree char *state = NULL;
    QTestState *qts = start_recovery(false, false, UINT64_MAX, true,
                                     &eeprom_path, &sd_path, &expected);

    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "recovery-updated-stop");
    qtest_system_reset(qts);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "recovery-updated-stop");
    qtest_quit(qts);
    assert_file_equals(eeprom_path, expected, EEPROM_SIZE);
    unlink(eeprom_path);
    unlink(sd_path);
}
void test_sd_secure_otp_provisioning(void)
{
    static const char key_hash_hex[] =
        "dea09d2a3225f6e195b2aba975d991c19acef3534a55c0c7e7c88589957d3152";
    uint8_t expected_hash[32];
    g_autofree uint8_t *initial_otp =
        make_otp_image(0, 0, PI4_BOARD_REVISION, false);
    g_autofree uint8_t *signed_eeprom = NULL;
    g_autofree char *eeprom_path = NULL;
    g_autofree char *otp_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree char *state = NULL;
    g_autofree char *otp_contents = NULL;
    g_autofree char *command = NULL;
    size_t otp_size;
    QTestState *qts;

    test_decode_hex(key_hash_hex, expected_hash, sizeof(expected_hash));
    qts = start_secure_provision_recovery(
        "program_pubkey=1\nrevoke_devkey=0\n", initial_otp, true, true,
        UINT8_MAX, &eeprom_path, &otp_path, &sd_path, &signed_eeprom);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "recovery-updated-reboot");
    qtest_quit(qts);

    assert_file_equals(eeprom_path, signed_eeprom, EEPROM_SIZE);
    g_assert_true(g_file_get_contents(
        otp_path, &otp_contents, &otp_size, NULL));
    g_assert_cmpuint(otp_size, ==, OTP_SIZE);
    g_assert_cmphex(ldl_le_p(
                        otp_contents + (17 - 1) * sizeof(uint32_t)),
                    ==, BIT(15));
    g_assert_cmphex(ldl_le_p(
                        otp_contents + (18 - 1) * sizeof(uint32_t)),
                    ==, BIT(15));
    g_assert_cmpmem(otp_contents + (47 - 1) * sizeof(uint32_t),
                    sizeof(expected_hash), expected_hash,
                    sizeof(expected_hash));
    g_assert_cmphex(ldl_le_p(
                        otp_contents + (55 - 1) * sizeof(uint32_t)),
                    ==, 0x81);

    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "otp-drive=piotp" TEST_BOOTSYS_MACHINE_OPTION " "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=none,id=piotp,format=raw,file=%s,file.locking=off "
        "-nic none",
        eeprom_path, otp_path);
    qts = qtest_init(command);
    g_assert_true(qtest_qom_get_bool(qts, "/machine", "otp-secure-boot"));
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "secure-boot-status");
    g_assert_cmpstr(state, ==, "config-verified");
    qtest_quit(qts);

    unlink(eeprom_path);
    unlink(otp_path);
    unlink(sd_path);
}
void test_sd_secure_otp_provisioning_fail_closed(void)
{
    static const struct {
        const char *config;
        bool attach_otp;
        bool trust_bootsys;
        bool mismatched_key;
        const char *state;
    } cases[] = {
        {
            "program_pubkey=1\nprogram_pubkey=1\n", true, true, false,
            "recovery-provision-invalid",
        },
        {
            "program_pubkey=1\nprogram_jtag_lock=1\n", true, true, false,
            "recovery-provision-jtag-unsupported",
        },
        {
            "program_pubkey=1\n", false, true, false,
            "recovery-provision-persistence-required",
        },
        {
            "program_pubkey=1\n", true, true, true,
            "recovery-provision-key-mismatch",
        },
        {
            "program_pubkey=1\n", true, false, false,
            "recovery-provision-signature-invalid",
        },
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree uint8_t *initial_otp =
            make_otp_image(0, 0, PI4_BOARD_REVISION,
                           cases[i].mismatched_key);
        g_autofree uint8_t *signed_eeprom = NULL;
        g_autofree uint8_t *initial_eeprom =
            make_eeprom_image("BOOT_ORDER=0xf41\n", false);
        g_autofree char *eeprom_path = NULL;
        g_autofree char *otp_path = NULL;
        g_autofree char *sd_path = NULL;
        g_autofree char *state = NULL;
        QTestState *qts = start_secure_provision_recovery(
            cases[i].config, initial_otp, cases[i].attach_otp,
            cases[i].trust_bootsys, UINT8_MAX,
            &eeprom_path, &otp_path, &sd_path, &signed_eeprom);

        state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(state, ==, cases[i].state);
        qtest_quit(qts);
        assert_file_equals(eeprom_path, initial_eeprom, EEPROM_SIZE);
        if (cases[i].attach_otp) {
            assert_file_equals(otp_path, initial_otp, OTP_SIZE);
            unlink(otp_path);
        }
        unlink(eeprom_path);
        unlink(sd_path);
    }
}
void test_sd_secure_otp_provisioning_power_loss(void)
{
    static const char key_hash_hex[] =
        "dea09d2a3225f6e195b2aba975d991c19acef3534a55c0c7e7c88589957d3152";
    uint8_t key_hash[32];

    test_decode_hex(key_hash_hex, key_hash, sizeof(key_hash));
    for (uint8_t cutoff = 0;
         cutoff < 3 + BCM2711_OTP_CUSTOMER_KEY_HASH_LEN; cutoff++) {
        g_autofree uint8_t *initial_otp =
            make_otp_image(0, 0, PI4_BOARD_REVISION, false);
        g_autofree uint8_t *expected_otp =
            g_memdup2(initial_otp, OTP_SIZE);
        g_autofree uint8_t *signed_eeprom = NULL;
        g_autofree char *eeprom_path = NULL;
        g_autofree char *otp_path = NULL;
        g_autofree char *sd_path = NULL;
        g_autofree char *state = NULL;
        g_autofree char *command = NULL;
        g_autofree char *recovery_digest = NULL;
        QTestState *qts = start_secure_provision_recovery(
            "program_pubkey=1\n", initial_otp, true, true, cutoff,
            &eeprom_path, &otp_path, &sd_path, &signed_eeprom);

        state = qom_get_string(qts, "boot-state");
        recovery_digest = qom_get_string(qts, "recovery-sha256");
        g_assert_cmpstr(recovery_digest, !=, "none");
        g_assert_cmpstr(state, ==, "recovery-provision-power-failed");
        g_assert_cmpuint(
            qom_get_uint32(qts, "otp-provision-rows-programmed"), ==,
            cutoff);
        if (cutoff == 5) {
            g_autofree char *migration_command = g_strdup_printf(
                "-M raspi4b,boot-mode=behavioral,"
                "eeprom-drive=pieeprom,otp-drive=piotp,"
                "recovery-trusted-sha256=%s,"
                "otp-provision-fail-after=%u" TEST_BOOTSYS_MACHINE_OPTION " "
                "-drive if=none,id=pieeprom,format=raw,file=%s,"
                "file.locking=off "
                "-drive if=none,id=piotp,format=raw,file=%s,"
                "file.locking=off "
                "-drive if=sd,format=raw,file=%s,file.locking=off "
                "-nic none",
                recovery_digest, cutoff, eeprom_path, otp_path, sd_path);
            g_autofree char *migration_dir = NULL;
            g_autofree char *migration_socket = NULL;
            QTestState *destination = migrate_to_new_qtest(
                qts, migration_command, &migration_dir,
                &migration_socket);

            g_clear_pointer(&state, g_free);
            state = qom_get_string(destination, "boot-state");
            g_assert_cmpstr(
                state, ==, "recovery-provision-power-failed");
            g_assert_cmpuint(
                qom_get_uint32(
                    destination, "otp-provision-rows-programmed"),
                ==, cutoff);
            qtest_quit(qts);
            qtest_quit(destination);
            unlink(migration_socket);
            rmdir(migration_dir);
        } else {
            qtest_quit(qts);
        }
        assert_file_equals(eeprom_path, signed_eeprom, EEPROM_SIZE);

        if (cutoff >= 1) {
            stl_le_p(expected_otp + (17 - 1) * sizeof(uint32_t), BIT(15));
        }
        if (cutoff >= 2) {
            stl_le_p(expected_otp + (18 - 1) * sizeof(uint32_t), BIT(15));
        }
        if (cutoff >= 3) {
            stl_le_p(expected_otp + (55 - 1) * sizeof(uint32_t), 0x81);
        }
        for (unsigned int row = 0;
             row < BCM2711_OTP_CUSTOMER_KEY_HASH_LEN; row++) {
            if (cutoff >= 4 + row) {
                stl_le_p(expected_otp +
                         (BCM2711_OTP_CUSTOMER_KEY_HASH_ROW + row - 1) *
                         sizeof(uint32_t),
                         ldl_le_p(key_hash + row * sizeof(uint32_t)));
            }
        }
        assert_file_equals(otp_path, expected_otp, OTP_SIZE);

        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "otp-drive=piotp,recovery-trusted-sha256=%s"
            TEST_BOOTSYS_MACHINE_OPTION " "
            "-drive if=none,id=pieeprom,format=raw,file=%s,"
            "file.locking=off "
            "-drive if=none,id=piotp,format=raw,file=%s,"
            "file.locking=off "
            "-drive if=sd,format=raw,file=%s,file.locking=off -nic none",
            recovery_digest, eeprom_path, otp_path, sd_path);
        qts = qtest_init(command);
        g_clear_pointer(&state, g_free);
        state = qom_get_string(qts, "boot-state");
        if (cutoff == 0) {
            g_assert_cmpstr(state, ==, "recovery-updated-reboot");
            g_assert_cmpuint(
                qom_get_uint32(qts, "otp-provision-rows-programmed"), ==,
                3 + BCM2711_OTP_CUSTOMER_KEY_HASH_LEN);
        } else if (cutoff == 1) {
            g_assert_cmpstr(state, ==, "otp-bootmode-invalid");
        } else if (cutoff <= 3) {
            g_assert_cmpstr(state, ==, "secure-boot-key-missing");
        } else {
            g_assert_cmpstr(state, ==, "secure-boot-key-hash-mismatch");
        }
        qtest_quit(qts);

        unlink(eeprom_path);
        unlink(otp_path);
        unlink(sd_path);
    }
}
#ifndef _WIN32
void test_cm4_rpiboot_dwc2_enumeration(void)
{
    run_cm4_rpiboot_dwc2_enumeration(false, true);
}
#endif
#ifndef _WIN32
void test_cm4_rpiboot_untrusted_bootcode(void)
{
    run_cm4_rpiboot_dwc2_enumeration(false, false);
}
#endif
#ifndef _WIN32
void test_cm4_rpiboot_secure_provisioning(void)
{
    run_cm4_rpiboot_dwc2_enumeration(true, true);
}
#endif
#ifndef _WIN32
void test_cm4_rpiboot_active_migration(void)
{
    static const uint8_t get_configuration[] = {
        USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        USB_REQ_GET_CONFIGURATION, 0x00, 0x00,
        0x00, 0x00, 0x01, 0x00,
    };
    uint8_t boot_message[24] = { 0 };
    uint8_t bootcode[1024];
    uint8_t status[4];
    uint8_t setup[8] = { 0x40, 0 };
    g_autofree char *source_command = NULL;
    g_autofree char *destination_command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    g_autofree char *expected_hash = NULL;
    g_autofree char *actual_hash = NULL;
    g_autofree char *state = NULL;
    int source_sockets[2];
    int destination_sockets[2];
    QTestState *source;
    QTestState *destination;

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, source_sockets),
                    ==, 0);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0,
                               destination_sockets), ==, 0);
    for (size_t i = 0; i < sizeof(bootcode); i++) {
        bootcode[i] = i * 17 + 11;
    }
    expected_hash = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, bootcode, sizeof(bootcode));
    source_command = g_strdup_printf(
        "-M raspi-cm4,boot-mode=behavioral,nrpiboot=on,"
        "rpiboot-bootcode-trusted-sha256=%s "
        "-chardev socket,id=dwc2dev,fd=%d "
        "-global dwc2-usb.device-chardev=dwc2dev",
        expected_hash, source_sockets[1]);
    destination_command = g_strdup_printf(
        "-M raspi-cm4,boot-mode=behavioral,nrpiboot=on,"
        "rpiboot-bootcode-trusted-sha256=%s "
        "-chardev socket,id=dwc2dev,fd=%d "
        "-global dwc2-usb.device-chardev=dwc2dev",
        expected_hash, destination_sockets[1]);
    source = qtest_init(source_command);
    close(source_sockets[1]);

    g_assert_cmpint(dwc2_transport_exchange(
                        source_sockets[0], DWC2_DEVICE_TRANSPORT_CONNECT,
                        0, 0, NULL, 0, NULL, 0), ==, 0);
    g_assert_cmpint(dwc2_transport_exchange(
                        source_sockets[0], DWC2_DEVICE_TRANSPORT_RESET,
                        0, 0, NULL, 0, NULL, 0), ==, 0);
    g_assert_cmpint(dwc2_transport_exchange(
                        source_sockets[0], DWC2_DEVICE_TRANSPORT_ENUM_DONE,
                        0, DSTS_ENUMSPD_HS, NULL, 0, NULL, 0), ==, 0);
    dwc2_rpiboot_configure(source_sockets[0], 23);
    g_assert_cmpuint(qom_get_uint32(source, "rpiboot-configuration"), ==, 1);

    stl_le_p(boot_message, sizeof(bootcode));
    stw_le_p(setup + 2, sizeof(boot_message));
    g_assert_cmpint(dwc2_transport_exchange(
                        source_sockets[0], DWC2_DEVICE_TRANSPORT_SETUP,
                        0, 0, setup, sizeof(setup), NULL, 0), ==,
                    sizeof(setup));
    g_assert_cmpint(dwc2_transport_exchange(
                        source_sockets[0], DWC2_DEVICE_TRANSPORT_IN,
                        0, 64, NULL, 0, status, sizeof(status)), ==, 0);
    g_assert_cmpint(dwc2_transport_exchange(
                        source_sockets[0], DWC2_DEVICE_TRANSPORT_OUT,
                        1, 0, boot_message, sizeof(boot_message), NULL, 0),
                    ==, sizeof(boot_message));
    stw_le_p(setup + 2, sizeof(bootcode));
    g_assert_cmpint(dwc2_transport_exchange(
                        source_sockets[0], DWC2_DEVICE_TRANSPORT_SETUP,
                        0, 0, setup, sizeof(setup), NULL, 0), ==,
                    sizeof(setup));
    g_assert_cmpint(dwc2_transport_exchange(
                        source_sockets[0], DWC2_DEVICE_TRANSPORT_IN,
                        0, 64, NULL, 0, status, sizeof(status)), ==, 0);
    g_assert_cmpint(dwc2_transport_exchange(
                        source_sockets[0], DWC2_DEVICE_TRANSPORT_OUT,
                        1, 0, bootcode, 512, NULL, 0), ==, 512);
    g_assert_cmpuint(qom_get_uint32(source, "rpiboot-transfer-received"),
                     ==, 512);

    destination = migrate_to_new_qtest_commands(
        source, destination_command, &migration_dir, &migration_socket);
    close(destination_sockets[1]);
    g_assert_cmpuint(qom_get_uint32(destination,
                                   "rpiboot-transfer-received"),
                     ==, 512);
    memset(status, 0xff, sizeof(status));
    g_assert_cmpuint(dwc2_rpiboot_standard_control(
                         destination_sockets[0], get_configuration,
                         status, sizeof(status)), ==, 1);
    g_assert_cmpuint(status[0], ==, 1);
    g_assert_cmpuint(qom_get_uint32(destination,
                                   "rpiboot-configuration"), ==, 1);
    g_assert_cmpint(dwc2_transport_exchange(
                        destination_sockets[0],
                        DWC2_DEVICE_TRANSPORT_OUT, 1, 0,
                        bootcode + 512, 512, NULL, 0), ==, 512);
    g_assert_cmpuint(qom_get_uint32(destination, "rpiboot-bootcode-size"),
                     ==, sizeof(bootcode));
    actual_hash = qom_get_string(destination, "rpiboot-bootcode-sha256");
    g_assert_cmpstr(actual_hash, ==, expected_hash);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "rpiboot-bootcode-trust");
    g_assert_cmpstr(state, ==, "trusted");

    memset(setup, 0, sizeof(setup));
    setup[0] = USB_DIR_IN | USB_TYPE_VENDOR;
    stw_le_p(setup + 6, sizeof(status));
    g_assert_cmpint(dwc2_transport_exchange(
                        destination_sockets[0],
                        DWC2_DEVICE_TRANSPORT_SETUP, 0, 0,
                        setup, sizeof(setup), NULL, 0), ==, sizeof(setup));
    g_assert_cmpint(dwc2_transport_exchange(
                        destination_sockets[0], DWC2_DEVICE_TRANSPORT_IN,
                        0, sizeof(status), NULL, 0,
                        status, sizeof(status)), ==, sizeof(status));
    g_assert_cmpint(dwc2_transport_exchange(
                        destination_sockets[0], DWC2_DEVICE_TRANSPORT_OUT,
                        0, 0, NULL, 0, NULL, 0), ==, 0);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "rpiboot-bootcode-ready");

    qtest_quit(source);
    qtest_quit(destination);
    close(source_sockets[0]);
    close(destination_sockets[0]);
    unlink(migration_socket);
    rmdir(migration_dir);
}
#endif
