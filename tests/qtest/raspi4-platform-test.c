/*
 * Raspberry Pi 4 platform integration tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "raspi4-boot-test.h"

void test_pi4_memory_models(void)
{
    static const struct {
        const char *machine;
        unsigned int gib;
        uint32_t board_revision;
    } cases[] = {
        { "raspi4b", 1, 0x00a03115 },
        { "raspi4b", 2, 0x00b03115 },
        { "raspi4b", 4, 0x00c03115 },
        { "raspi4b", 8, 0x00d03115 },
        { "raspi-cm4", 1, 0x00a03140 },
        { "raspi-cm4", 2, 0x00b03140 },
        { "raspi-cm4", 4, 0x00c03140 },
        { "raspi-cm4", 8, 0x00d03140 },
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree char *command = g_strdup_printf(
            "-M %s,boot-mode=behavioral -m %uG",
            cases[i].machine, cases[i].gib);
        g_autofree char *model = NULL;
        g_autofree char *expected_model = g_strdup_printf(
            "%uGiB", cases[i].gib);
        uint64_t ram_size = (uint64_t)cases[i].gib * GiB;
        uint64_t probe = ram_size - sizeof(uint64_t);
        QTestState *qts = qtest_init(command);

        if (ram_size > RASPI4_LOW_RAM_END) {
            probe = RASPI4_HIGH_RAM_BASE +
                    ram_size - RASPI4_LOW_RAM_END - sizeof(uint64_t);
        }
        g_assert_cmphex(qom_get_uint32(qts, "board-revision"), ==,
                        cases[i].board_revision);
        g_assert_cmphex(qom_get_uint32(qts, "otp-board-revision"), ==,
                        cases[i].board_revision);
        model = qom_get_string(qts, "memory-model");
        g_assert_cmpstr(model, ==, expected_model);
        qtest_writeq(qts, probe, 0x0123456789abcdefULL);
        g_assert_cmphex(qtest_readq(qts, probe), ==,
                        0x0123456789abcdefULL);
        qtest_quit(qts);
    }

    {
        QTestState *qts = qtest_init("-M raspi4b");
        g_autofree char *mode = qom_get_string(
            qts, "videocore-execution-mode");
        g_autofree char *policy = qom_get_string(
            qts, "videocore-artifact-policy");

        g_assert_cmpstr(mode, ==, "inactive-direct-loader");
        g_assert_cmpstr(policy, ==, "not-applicable");
        g_assert_cmpuint(qom_get_uint32(
                             qts, "videocore-boundary-version"), ==, 0);
        qtest_quit(qts);
    }
}
void test_pi4_memory_model_device_tree(void)
{
    static const uint8_t firmware[] = "QEMU Pi 4 firmware fixture";
    static const unsigned int sizes[] = { 1, 2, 4, 8 };

    for (size_t i = 0; i < ARRAY_SIZE(sizes); i++) {
        g_autofree char *eeprom_path = NULL;
        g_autofree char *sd_path = NULL;
        uint64_t dt_address;
        uint64_t dt_size;
        g_autofree uint8_t *dt = NULL;
        const uint8_t *reg;
        int reg_length;
        int node;
        g_autofree char *state = NULL;
        QTestState *qts = start_with_eeprom_and_sd_ram(
            "BOOT_ORDER=0xf1\n", "START   ELF", firmware,
            sizeof(firmware), NULL, false, sizes[i],
            sizes[i] == 8 ? 1 : 2, &eeprom_path, &sd_path);

        state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(state, ==, "arm-handoff-ready");
        dt_address = qom_get_uint64(qts, "arm-handoff-device-tree-address");
        dt_size = qom_get_uint64(qts, "arm-handoff-device-tree-size");
        dt = g_malloc(dt_size);
        qtest_memread(qts, dt_address, dt, dt_size);
        g_assert_cmpint(fdt_check_header(dt), ==, 0);
        node = fdt_path_offset(dt, "/memory@40000000");
        if (sizes[i] == 1) {
            g_assert_cmpint(node, ==, -FDT_ERR_NOTFOUND);
        } else {
            uint64_t total = (uint64_t)sizes[i] * GiB;
            uint64_t middle_end = MIN(total, RASPI4_LOW_RAM_END);

            g_assert_cmpint(node, >=, 0);
            reg = fdt_getprop(dt, node, "reg", &reg_length);
            g_assert_nonnull(reg);
            g_assert_cmpint(reg_length, ==,
                            (sizes[i] == 8 ? 3 : 4) * sizeof(uint32_t));
            g_assert_cmphex(ldq_be_p(reg), ==, 1 * GiB);
            if (sizes[i] == 8) {
                g_assert_cmphex(
                    (uint64_t)(uint32_t)ldl_be_p(
                        reg + sizeof(uint64_t)), ==,
                    middle_end - 1 * GiB);
            } else {
                g_assert_cmphex(ldq_be_p(reg + sizeof(uint64_t)), ==,
                                middle_end - 1 * GiB);
            }
        }
        node = fdt_path_offset(dt, "/memory@100000000");
        if ((uint64_t)sizes[i] * GiB <= RASPI4_LOW_RAM_END) {
            g_assert_cmpint(node, ==, -FDT_ERR_NOTFOUND);
        } else {
            g_assert_cmpint(node, >=, 0);
            reg = fdt_getprop(dt, node, "reg", &reg_length);
            g_assert_nonnull(reg);
            g_assert_cmpint(reg_length, ==,
                            (sizes[i] == 8 ? 3 : 4) * sizeof(uint32_t));
            g_assert_cmphex(ldq_be_p(reg), ==, RASPI4_HIGH_RAM_BASE);
            if (sizes[i] == 8) {
                g_assert_cmphex(
                    (uint64_t)(uint32_t)ldl_be_p(
                        reg + sizeof(uint64_t)), ==, 2 * GiB);
                node = fdt_path_offset(dt, "/memory@180000000");
                g_assert_cmpint(node, >=, 0);
                reg = fdt_getprop(dt, node, "reg", &reg_length);
                g_assert_nonnull(reg);
                g_assert_cmpint(reg_length, ==, 3 * sizeof(uint32_t));
                g_assert_cmphex(ldq_be_p(reg), ==, 6 * GiB);
                g_assert_cmphex(
                    (uint64_t)(uint32_t)ldl_be_p(
                        reg + sizeof(uint64_t)), ==, 2 * GiB);
                node = fdt_path_offset(dt, "/memory@200000000");
                g_assert_cmpint(node, >=, 0);
                reg = fdt_getprop(dt, node, "reg", &reg_length);
                g_assert_nonnull(reg);
                g_assert_cmpint(reg_length, ==, 3 * sizeof(uint32_t));
                g_assert_cmphex(ldq_be_p(reg), ==, 8 * GiB);
                g_assert_cmphex(
                    (uint64_t)(uint32_t)ldl_be_p(
                        reg + sizeof(uint64_t)), ==, 64 * MiB);
            } else {
                g_assert_cmphex(ldq_be_p(reg + sizeof(uint64_t)), ==,
                                (uint64_t)sizes[i] * GiB -
                                RASPI4_LOW_RAM_END);
            }
        }
        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(sd_path);
    }
}
void test_imager_repo_url_handoff(void)
{
    static const char bootconf[] =
        "BOOT_ORDER=0xf1\n"
        "IMAGER_REPO_URL=https://repo.example/os-list.json?model=pi4\n";
    static const char * const machines[] = {
        "raspi4b",
        "raspi-cm4",
    };

    for (size_t i = 0; i < ARRAY_SIZE(machines); i++) {
        const bool cm4 = !strcmp(machines[i], "raspi-cm4");
        g_autofree char *eeprom_path = NULL;
        g_autofree char *media_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *migration_dir = NULL;
        g_autofree char *migration_socket = NULL;
        QTestState *source = start_arm32_memory_model_with_otp(
            machines[i], 2, bootconf, NULL, NULL, 0, NULL, 0, NULL,
            &eeprom_path, &media_path);
        QTestState *destination;

        assert_bootloader_reserved_blob(
            source, "raspberrypi,bootloader-config",
            (const uint8_t *)bootconf, strlen(bootconf));
        qtest_system_reset(source);
        assert_bootloader_reserved_blob(
            source, "raspberrypi,bootloader-config",
            (const uint8_t *)bootconf, strlen(bootconf));

        if (cm4) {
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
        assert_bootloader_reserved_blob(
            destination, "raspberrypi,bootloader-config",
            (const uint8_t *)bootconf, strlen(bootconf));

        qtest_quit(source);
        qtest_quit(destination);
        unlink(eeprom_path);
        unlink(media_path);
        unlink(migration_socket);
        rmdir(migration_dir);
    }
}
void test_firmware_total_mem_models(void)
{
    static const uint8_t firmware[] = "QEMU total_mem network fixture";
    static const struct {
        const char *machine;
        unsigned int installed_gib;
        const char *config;
        uint32_t effective_mb;
        uint32_t lower_mb;
        uint32_t upper_mb;
    } cases[] = {
        { "raspi4b", 4, "total_mem=1024\n", 1024, 948, 0 },
        { "raspi4b", 4, "total_mem=1536\n", 1536, 948, 512 },
        { "raspi4b", 4, "total_mem=64\n", 128, 52, 0 },
        { "raspi4b", 2, "total_mem=99999\n", 2048, 948, 1024 },
        { "raspi-cm4", 8, "total_mem=2048\n", 2048, 948, 1024 },
        { "raspi-cm4", 1, "total_mem=0\n", 128, 52, 0 },
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree char *eeprom_path = NULL;
        g_autofree char *media_path = NULL;
        uint8_t public_key[264];
        QTestState *qts = start_arm32_memory_model(
            cases[i].machine, cases[i].installed_gib, cases[i].config,
            NULL, 0,
            &eeprom_path, &media_path);
        g_autofree char *state = qom_get_string(qts, "boot-state");

        g_assert_cmpstr(state, ==, "arm-handoff-ready");
        g_assert_cmpuint(qom_get_uint32(
                             qts, "firmware-total-mem-mb"), ==,
                         cases[i].effective_mb);
        g_assert_cmpuint(firmware_sdram_size_gbit(qts), ==,
                         cases[i].installed_gib * 8);
        assert_firmware_memory_nodes(
            qts, (uint64_t)cases[i].lower_mb * MiB,
            (uint64_t)cases[i].upper_mb * MiB);
        assert_no_firmware_initramfs(qts);
        make_test_public_key(public_key);
        assert_bootloader_reserved_blob(
            qts, "raspberrypi,bootloader-config",
            (const uint8_t *)"BOOT_ORDER=0xf1\n",
            strlen("BOOT_ORDER=0xf1\n"));
        assert_bootloader_reserved_blob(
            qts, "raspberrypi,bootloader-public-key",
            public_key, sizeof(public_key));
        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(media_path);
    }

    {
        g_autofree char *eeprom_path = NULL;
        g_autofree char *network_path = NULL;
        QTestState *qts = start_with_eeprom_and_network_config(
            "BOOT_ORDER=0x2\n", "START4  ELF", firmware,
            sizeof(firmware), "total_mem=1024\n",
            &eeprom_path, &network_path);
        g_autofree char *state = qom_get_string(qts, "boot-state");
        g_autofree char *source = qom_get_string(qts, "boot-source");

        g_assert_cmpstr(state, ==, "arm-handoff-ready");
        g_assert_cmpstr(source, ==, "network");
        g_assert_cmpuint(qom_get_uint32(
                             qts, "firmware-total-mem-mb"), ==, 1024);
        g_assert_cmpuint(firmware_sdram_size_gbit(qts), ==, 16);
        assert_firmware_memory_nodes(qts, 948 * MiB, 0);
        assert_no_firmware_initramfs(qts);
        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(network_path);
    }
}
void test_firmware_total_mem_include_boundary(void)
{
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    QTestState *qts = start_with_complex_firmware_config(
        "[pi4]\ntotal_mem=128\n", &eeprom_path, &sd_path);
    g_autofree char *state = qom_get_string(qts, "boot-state");

    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpuint(qom_get_uint32(
                         qts, "firmware-total-mem-mb"), ==, 2048);
    g_assert_cmpuint(qom_get_uint32(
                         qts, "firmware-config-ignored"), ==, 2);
    assert_firmware_memory_nodes(qts, 948 * MiB, 1 * GiB);
    qtest_quit(qts);
    unlink(eeprom_path);
    unlink(sd_path);
}
void test_firmware_gpu_mem_models(void)
{
    static const uint8_t firmware[] = "QEMU gpu_mem network fixture";
    static const struct {
        const char *machine;
        unsigned int installed_gib;
    } models[] = {
        { "raspi4b", 1 },
        { "raspi4b", 2 },
        { "raspi4b", 4 },
        { "raspi4b", 8 },
        { "raspi-cm4", 1 },
        { "raspi-cm4", 2 },
        { "raspi-cm4", 4 },
        { "raspi-cm4", 8 },
    };
    static const char config[] =
        "gpu_mem=128\n"
        "gpu_mem_256=16\n"
        "gpu_mem_512=32\n"
        "gpu_mem_1024=96\n";

    for (size_t i = 0; i < ARRAY_SIZE(models); i++) {
        g_autofree char *eeprom_path = NULL;
        g_autofree char *media_path = NULL;
        g_autofree char *source = NULL;
        uint32_t arm_memory[2] = { 0 };
        uint32_t vc_memory[2] = { 0 };
        QTestState *qts = start_arm32_memory_model(
            models[i].machine, models[i].installed_gib, config,
            NULL, 0,
            &eeprom_path, &media_path);

        g_assert_cmpuint(qom_get_uint32(
                             qts, "firmware-gpu-mem-mb"), ==, 96);
        source = qom_get_string(qts, "firmware-gpu-mem-source");
        g_assert_cmpstr(source, ==, "gpu_mem_1024");
        assert_firmware_memory_nodes(
            qts, 928 * MiB,
            models[i].installed_gib > 1 ?
                ((uint64_t)models[i].installed_gib - 1) * GiB : 0);
        bcm2711_property_call(
            qts, RPI_FWREQ_GET_ARM_MEMORY, arm_memory, 2);
        bcm2711_property_call(
            qts, RPI_FWREQ_GET_VC_MEMORY, vc_memory, 2);
        g_assert_cmphex(arm_memory[0], ==, 0);
        g_assert_cmphex(arm_memory[1], ==, 928 * MiB);
        g_assert_cmphex(vc_memory[0], ==, 928 * MiB);
        g_assert_cmphex(vc_memory[1], ==, 96 * MiB);
        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(media_path);
    }

    {
        g_autofree char *eeprom_path = NULL;
        g_autofree char *network_path = NULL;
        g_autofree char *source = NULL;
        QTestState *qts = start_with_eeprom_and_network_config(
            "BOOT_ORDER=0x2\n", "START4  ELF", firmware,
            sizeof(firmware), "gpu_mem=112\ngpu_mem_1024=80\n",
            &eeprom_path, &network_path);

        g_assert_cmpuint(qom_get_uint32(
                             qts, "firmware-gpu-mem-mb"), ==, 80);
        source = qom_get_string(qts, "firmware-gpu-mem-source");
        g_assert_cmpstr(source, ==, "gpu_mem_1024");
        assert_firmware_memory_nodes(qts, 944 * MiB, 1 * GiB);
        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(network_path);
    }

    {
        g_autofree char *eeprom_path = NULL;
        g_autofree char *sd_path = NULL;
        g_autofree char *source = NULL;
        QTestState *qts = start_with_complex_firmware_config(
            "[pi4]\n"
            "gpu_mem=16\n"
            "gpu_mem_256=16\n"
            "gpu_mem_512=16\n"
            "gpu_mem_1024=16\n",
            &eeprom_path, &sd_path);

        g_assert_cmpuint(qom_get_uint32(
                             qts, "firmware-gpu-mem-mb"), ==, 76);
        source = qom_get_string(qts, "firmware-gpu-mem-source");
        g_assert_cmpstr(source, ==, "default");
        g_assert_cmpuint(qom_get_uint32(
                             qts, "firmware-config-ignored"), ==, 5);
        assert_firmware_memory_nodes(qts, 948 * MiB, 1 * GiB);
        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(sd_path);
    }

    {
        g_autofree char *eeprom_path = NULL;
        g_autofree char *sd_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *migration_dir = NULL;
        g_autofree char *migration_socket = NULL;
        g_autofree char *source_name = NULL;
        g_autofree char *destination_name = NULL;
        uint32_t vc_memory[2] = { 0 };
        uint8_t public_key[264];
        uint64_t source_kaslr_seed;
        QTestState *source = start_arm32_memory_model(
            "raspi4b", 4, config, NULL, 0,
            &eeprom_path, &sd_path);
        QTestState *destination;

        qtest_system_reset(source);
        source_name = qom_get_string(
            source, "firmware-gpu-mem-source");
        g_assert_cmpstr(source_name, ==, "gpu_mem_1024");
        g_assert_cmpuint(qom_get_uint32(
                             source, "firmware-gpu-mem-mb"), ==, 96);
        assert_firmware_memory_nodes(source, 928 * MiB, 3 * GiB);
        bcm2711_property_call(
            source, RPI_FWREQ_GET_VC_MEMORY, vc_memory, 2);
        g_assert_cmphex(vc_memory[0], ==, 928 * MiB);
        g_assert_cmphex(vc_memory[1], ==, 96 * MiB);
        make_test_public_key(public_key);
        assert_bootloader_reserved_blob(
            source, "raspberrypi,bootloader-config",
            (const uint8_t *)"BOOT_ORDER=0xf1\n",
            strlen("BOOT_ORDER=0xf1\n"));
        assert_bootloader_reserved_blob(
            source, "raspberrypi,bootloader-public-key",
            public_key, sizeof(public_key));
        assert_firmware_system_identity(source, 0x00c03115, 0);
        g_assert_cmpuint(firmware_sdram_size_gbit(source), ==, 32);
        assert_firmware_chosen_string(source, "os_prefix", "");
        assert_firmware_chosen_string(source, "overlay_prefix", "overlays/");
        source_kaslr_seed = firmware_kaslr_seed(source);

        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
            "-m 4G "
            "-drive if=none,id=pieeprom,format=raw,file=%s,"
            "file.locking=off "
            "-drive if=sd,format=raw,file=%s,file.locking=off "
            "-nic none",
            eeprom_path, sd_path);
        destination = migrate_to_new_qtest(
            source, command, &migration_dir, &migration_socket);
        destination_name = qom_get_string(
            destination, "firmware-gpu-mem-source");
        g_assert_cmpstr(destination_name, ==, "gpu_mem_1024");
        g_assert_cmpuint(qom_get_uint32(
                             destination,
                             "firmware-gpu-mem-mb"), ==, 96);
        assert_firmware_memory_nodes(destination, 928 * MiB, 3 * GiB);
        memset(vc_memory, 0, sizeof(vc_memory));
        bcm2711_property_call(
            destination, RPI_FWREQ_GET_VC_MEMORY, vc_memory, 2);
        g_assert_cmphex(vc_memory[0], ==, 928 * MiB);
        g_assert_cmphex(vc_memory[1], ==, 96 * MiB);
        assert_bootloader_reserved_blob(
            destination, "raspberrypi,bootloader-config",
            (const uint8_t *)"BOOT_ORDER=0xf1\n",
            strlen("BOOT_ORDER=0xf1\n"));
        assert_bootloader_reserved_blob(
            destination, "raspberrypi,bootloader-public-key",
            public_key, sizeof(public_key));
        assert_firmware_system_identity(destination, 0x00c03115, 0);
        g_assert_cmpuint(firmware_sdram_size_gbit(destination), ==, 32);
        assert_firmware_chosen_string(destination, "os_prefix", "");
        assert_firmware_chosen_string(destination, "overlay_prefix",
                                      "overlays/");
        g_assert_cmphex(firmware_kaslr_seed(destination), ==,
                        source_kaslr_seed);

        qtest_quit(source);
        qtest_quit(destination);
        unlink(eeprom_path);
        unlink(sd_path);
        unlink(migration_socket);
        rmdir(migration_dir);
    }
}
void test_firmware_bootcode_delay_sd_reset(void)
{
    static const uint8_t firmware[] = "QEMU bootcode_delay SD fixture";
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    QTestState *qts = start_with_eeprom_and_sd(
        "BOOT_ORDER=0x1\n", "START4  ELF", firmware, sizeof(firmware),
        "bootcode_delay=2\n", &eeprom_path, &sd_path);
    g_autofree char *state = qom_get_string(qts, "boot-state");

    g_assert_cmpstr(state, ==, "firmware-delay");
    g_assert_cmpuint(qom_get_uint32(
                         qts, "firmware-bootcode-delay-seconds"), ==, 2);
    g_assert_cmpuint(qom_get_uint64(qts, "boot-elapsed-ms"), ==, 0);
    qtest_clock_step(qts, INT64_C(1999) * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "firmware-delay");
    qtest_clock_step(qts, INT64_C(1) * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpuint(qom_get_uint64(qts, "boot-elapsed-ms"), ==, 2000);

    qtest_system_reset(qts);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "firmware-delay");
    qtest_quit(qts);
    unlink(eeprom_path);
    unlink(sd_path);
}
void test_firmware_bootcode_delay_network(void)
{
    static const uint8_t firmware[] = "QEMU bootcode_delay network fixture";
    g_autofree char *eeprom_path = NULL;
    g_autofree char *network_path = NULL;
    QTestState *qts = start_with_eeprom_and_network_config(
        "BOOT_ORDER=0x2\n", "START4  ELF", firmware, sizeof(firmware),
        "bootcode_delay=1\n", &eeprom_path, &network_path);
    g_autofree char *state = qom_get_string(qts, "boot-state");
    g_autofree char *source = qom_get_string(qts, "boot-source");

    g_assert_cmpstr(state, ==, "firmware-delay");
    g_assert_cmpstr(source, ==, "network");
    qtest_clock_step(qts, INT64_C(1000) * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpuint(qom_get_uint64(qts, "boot-elapsed-ms"), ==, 1000);
    qtest_quit(qts);
    unlink(eeprom_path);
    unlink(network_path);
}
void test_firmware_bootcode_delay_include_boundary(void)
{
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    QTestState *qts = start_with_complex_firmware_config(
        "[pi4]\nbootcode_delay=2\n", &eeprom_path, &sd_path);
    g_autofree char *state = qom_get_string(qts, "boot-state");

    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpuint(qom_get_uint32(
                         qts, "firmware-bootcode-delay-seconds"), ==, 0);
    g_assert_cmpuint(qom_get_uint32(
                         qts, "firmware-config-ignored"), ==, 2);
    qtest_quit(qts);
    unlink(eeprom_path);
    unlink(sd_path);
}
void test_firmware_bootcode_delay_invalid(void)
{
    static const uint8_t firmware[] = "QEMU invalid bootcode_delay fixture";
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    QTestState *qts = start_with_eeprom_and_sd(
        "BOOT_ORDER=0x0\n", "START4  ELF", firmware, sizeof(firmware),
        "bootcode_delay=-1\n", &eeprom_path, &sd_path);
    g_autofree char *status = qom_get_string(qts, "firmware-status");

    g_assert_cmpstr(status, ==, "config-invalid");
    g_assert_cmpuint(qom_get_uint32(
                         qts, "firmware-bootcode-delay-seconds"), ==, 0);
    qtest_quit(qts);
    unlink(eeprom_path);
    unlink(sd_path);
}
void test_firmware_bootcode_delay_migration(void)
{
    static const uint8_t firmware[] = "QEMU migrated bootcode_delay fixture";
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    QTestState *source = start_with_eeprom_and_sd(
        "BOOT_ORDER=0x1\n", "START4  ELF", firmware, sizeof(firmware),
        "bootcode_delay=2\n", &eeprom_path, &sd_path);
    QTestState *destination;
    g_autofree char *state = NULL;

    qtest_clock_step(source, INT64_C(750) * 1000 * 1000);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=sd,format=raw,file=%s,file.locking=off -nic none",
        eeprom_path, sd_path);
    destination = migrate_to_new_qtest(
        source, command, &migration_dir, &migration_socket);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "firmware-delay");
    g_assert_cmpuint(qom_get_uint32(
                         destination,
                         "firmware-bootcode-delay-seconds"), ==, 2);
    qtest_clock_step(destination, INT64_C(1249) * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "firmware-delay");
    qtest_clock_step(destination, INT64_C(1) * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpuint(qom_get_uint64(
                         destination, "boot-elapsed-ms"), ==, 2000);

    qtest_quit(source);
    qtest_quit(destination);
    unlink(eeprom_path);
    unlink(sd_path);
    unlink(migration_socket);
    rmdir(migration_dir);
}
void test_eeprom_config_txt_append(void)
{
    static const uint8_t firmware[] =
        "QEMU EEPROM config.txt append fixture";
    static const char append[] =
        "gpu_mem=16\n"
        "bootcode_delay=2\n";
    static const char boot_config[] =
        "BOOT_ORDER=0x1\n"
        "[config.txt]\n"
        "gpu_mem=16\n"
        "bootcode_delay=2\n";
    g_autofree char *expected_sha256 = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, (const uint8_t *)append, strlen(append));
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    g_autofree char *state = NULL;
    g_autofree char *value = NULL;
    QTestState *source = start_with_eeprom_and_sd(
        boot_config, "START4CDELF", firmware, sizeof(firmware),
        NULL, &eeprom_path, &sd_path);
    QTestState *destination;

    state = qom_get_string(source, "boot-state");
    g_assert_cmpstr(state, ==, "firmware-delay");
    g_assert_cmpuint(qom_get_uint32(
                         source, "boot-eeprom-config-append-size"),
                     ==, strlen(append));
    value = qom_get_string(source, "boot-eeprom-config-append-sha256");
    g_assert_cmpstr(value, ==, expected_sha256);
    g_assert_cmpuint(qom_get_uint32(
                         source, "firmware-gpu-mem-mb"), ==, 16);

    qtest_clock_step(source, INT64_C(750) * 1000 * 1000);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=sd,format=raw,file=%s,file.locking=off -nic none",
        eeprom_path, sd_path);
    destination = migrate_to_new_qtest(
        source, command, &migration_dir, &migration_socket);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "firmware-delay");
    g_assert_cmpuint(qom_get_uint32(
                         destination, "boot-eeprom-config-append-size"),
                     ==, strlen(append));
    g_clear_pointer(&value, g_free);
    value = qom_get_string(
        destination, "boot-eeprom-config-append-sha256");
    g_assert_cmpstr(value, ==, expected_sha256);

    qtest_clock_step(destination, INT64_C(1249) * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "firmware-delay");
    qtest_clock_step(destination, INT64_C(1) * 1000 * 1000);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(destination, "boot-state");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_clear_pointer(&value, g_free);
    value = qom_get_string(destination, "firmware-file");
    g_assert_cmpstr(value, ==, "start4cd.elf");

    qtest_quit(source);
    qtest_quit(destination);
    unlink(eeprom_path);
    unlink(sd_path);
    unlink(migration_socket);
    rmdir(migration_dir);

    g_clear_pointer(&eeprom_path, g_free);
    g_clear_pointer(&sd_path, g_free);
    source = start_with_eeprom_and_network_config(
        "BOOT_ORDER=0x2\n"
        "[config.txt]\n"
        "gpu_mem=16\n",
        "START4CDELF", firmware, sizeof(firmware), NULL,
        &eeprom_path, &sd_path);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(source, "boot-state");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_clear_pointer(&value, g_free);
    value = qom_get_string(source, "firmware-file");
    g_assert_cmpstr(value, ==, "start4cd.elf");
    qtest_quit(source);
    unlink(eeprom_path);
    unlink(sd_path);
}
void test_firmware_sdram_frequency_models(void)
{
    static const struct {
        const char *machine;
        unsigned int gib;
    } cases[] = {
        { "raspi4b", 1 },
        { "raspi4b", 2 },
        { "raspi4b", 4 },
        { "raspi4b", 8 },
        { "raspi-cm4", 1 },
        { "raspi-cm4", 2 },
        { "raspi-cm4", 4 },
        { "raspi-cm4", 8 },
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        bool explicit_zero = i == ARRAY_SIZE(cases) - 1;
        g_autofree char *eeprom_path = NULL;
        g_autofree char *media_path = NULL;
        QTestState *qts = start_arm32_memory_model(
            cases[i].machine, cases[i].gib,
            explicit_zero ? "sdram_freq=0\n": "sdram_freq=1600\n",
            NULL, 0,
            &eeprom_path, &media_path);
        g_autofree char *state = qom_get_string(qts, "boot-state");

        g_assert_cmpstr(state, ==, "arm-handoff-ready");
        g_assert_cmpuint(qom_get_uint32(
                             qts,
                             "firmware-sdram-frequency-requested-mhz"),
                         ==, explicit_zero ? 0 : 1600);
        g_assert_true(qom_get_bool(
            qts, "firmware-sdram-frequency-requested"));
        g_assert_cmpuint(qom_get_uint32(
                             qts, "firmware-sdram-frequency-mhz"),
                         ==, 3200);
        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(media_path);
    }
}
void test_firmware_sdram_frequency_network(void)
{
    static const uint8_t firmware[] = "QEMU sdram_freq network fixture";
    g_autofree char *eeprom_path = NULL;
    g_autofree char *network_path = NULL;
    QTestState *qts = start_with_eeprom_and_network_config(
        "BOOT_ORDER=0x2\n", "START4  ELF", firmware, sizeof(firmware),
        "sdram_freq=4267\n", &eeprom_path, &network_path);
    g_autofree char *state = qom_get_string(qts, "boot-state");

    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpuint(qom_get_uint32(
                         qts,
                         "firmware-sdram-frequency-requested-mhz"),
                     ==, 4267);
    g_assert_true(qom_get_bool(
        qts, "firmware-sdram-frequency-requested"));
    g_assert_cmpuint(qom_get_uint32(
                         qts, "firmware-sdram-frequency-mhz"),
                     ==, 3200);
    qtest_quit(qts);
    unlink(eeprom_path);
    unlink(network_path);
}
void test_firmware_sdram_frequency_include_boundary(void)
{
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    QTestState *qts = start_with_complex_firmware_config(
        "[pi4]\nsdram_freq=1600\n", &eeprom_path, &sd_path);
    g_autofree char *state = qom_get_string(qts, "boot-state");

    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_cmpuint(qom_get_uint32(
                         qts,
                         "firmware-sdram-frequency-requested-mhz"),
                     ==, 0);
    g_assert_false(qom_get_bool(
        qts, "firmware-sdram-frequency-requested"));
    g_assert_cmpuint(qom_get_uint32(
                         qts, "firmware-sdram-frequency-mhz"),
                     ==, 3200);
    g_assert_cmpuint(qom_get_uint32(
                         qts, "firmware-config-ignored"), ==, 2);
    qtest_quit(qts);
    unlink(eeprom_path);
    unlink(sd_path);
}
void test_firmware_sdram_frequency_invalid(void)
{
    static const uint8_t firmware[] = "QEMU invalid sdram_freq fixture";
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    QTestState *qts = start_with_eeprom_and_sd(
        "BOOT_ORDER=0x0\n", "START4  ELF", firmware, sizeof(firmware),
        "sdram_freq=-1\n", &eeprom_path, &sd_path);
    g_autofree char *status = qom_get_string(qts, "firmware-status");

    g_assert_cmpstr(status, ==, "config-invalid");
    qtest_quit(qts);
    unlink(eeprom_path);
    unlink(sd_path);
}
void test_boot_uart_sd_migration_boundaries(void)
{
    static const uint8_t firmware[] = "QEMU BOOT_UART fixture";
    g_autofree uint8_t *eeprom = make_eeprom_image(
        "BOOT_ORDER=0x1\nBOOT_UART=1\n", false);
    g_autofree uint8_t *media = make_usb_boot_image(
        "START4  ELF", firmware, sizeof(firmware), NULL);
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree char *serial_path = NULL;
    g_autofree char *serial_data = NULL;
    g_autofree char *command = NULL;
    g_autofree char *state = NULL;
    g_autofree char *format = NULL;
    gsize serial_size;
    int serial_fd = g_file_open_tmp(
        "raspi4-boot-uart-XXXXXX", &serial_path, NULL);
    QTestState *qts;

    g_assert_cmpint(serial_fd, >=, 0);
    close(serial_fd);
    write_temp_image("raspi4-boot-uart-eeprom-XXXXXX",
                     eeprom, EEPROM_SIZE, &eeprom_path);
    write_temp_image("raspi4-boot-uart-sd-XXXXXX",
                     media, SD_SIZE, &sd_path);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
        "-drive if=none,id=pieeprom,format=raw,file=%s "
        "-drive if=sd,format=raw,file=%s "
        "-serial file:%s -nic none",
        eeprom_path, sd_path, serial_path);
    qts = qtest_init(command);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "arm-handoff-ready");
    g_assert_true(qom_get_bool(qts, "boot-uart-enabled"));
    g_assert_false(qom_get_bool(qts, "boot-uart-active"));
    format = qom_get_string(qts, "boot-uart-format");
    g_assert_cmpstr(format, ==,
                    "primary-uart0-115200-8n1-cleanroom-v1");
    g_assert_true(g_file_get_contents(
        serial_path, &serial_data, &serial_size, NULL));
    g_assert_nonnull(strstr(serial_data,
                            "RPI4-BOOT: enabled order=0x00000001 "));
    g_assert_nonnull(strstr(serial_data,
                            "RPI4-BOOT: state=boot-source-attempt "
                            "source=sd-card "));
    g_assert_true(g_str_has_suffix(
        serial_data, "RPI4-BOOT: second-stage\r\n"));
    g_assert_cmpuint(qom_get_uint64(qts, "boot-uart-bytes"),
                     ==, serial_size);
    g_assert_cmpuint(qom_get_uint32(qts, "boot-uart-lines"), >=, 3);
    qtest_quit(qts);
    unlink(eeprom_path);
    unlink(sd_path);
    unlink(serial_path);

    g_clear_pointer(&eeprom, g_free);
    g_clear_pointer(&eeprom_path, g_free);
    g_clear_pointer(&command, g_free);
    g_clear_pointer(&state, g_free);
    eeprom = make_eeprom_image(
        "BOOT_ORDER=0x4\n"
        "BOOT_UART=1\n"
        "USB_MSD_PWR_OFF_TIME=0\n"
        "USB_MSD_DISCOVER_TIMEOUT=5000\n",
        false);
    write_temp_image("raspi4-boot-uart-migrate-XXXXXX",
                     eeprom, EEPROM_SIZE, &eeprom_path);
    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-serial null -nic none",
        eeprom_path);
    qts = qtest_init(command);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "usb-discovery-wait");
    {
        g_autofree char *migration_dir = NULL;
        g_autofree char *migration_socket = NULL;
        uint64_t source_bytes = qom_get_uint64(qts, "boot-uart-bytes");
        uint32_t source_lines = qom_get_uint32(qts, "boot-uart-lines");
        QTestState *destination;

        qtest_clock_step(qts, INT64_C(1000) * 1000 * 1000);
        destination = migrate_to_new_qtest(
            qts, command, &migration_dir, &migration_socket);
        g_assert_true(qom_get_bool(destination, "boot-uart-enabled"));
        g_assert_true(qom_get_bool(destination, "boot-uart-active"));
        g_assert_cmpuint(qom_get_uint64(
                             destination, "boot-uart-bytes"),
                         ==, source_bytes);
        g_assert_cmpuint(qom_get_uint32(
                             destination, "boot-uart-lines"),
                         ==, source_lines);
        qtest_clock_step(destination, INT64_C(3999) * 1000 * 1000);
        g_clear_pointer(&state, g_free);
        state = qom_get_string(destination, "boot-state");
        g_assert_cmpstr(state, ==, "usb-discovery-wait");
        qtest_clock_step(destination, INT64_C(1) * 1000 * 1000);
        g_clear_pointer(&state, g_free);
        state = qom_get_string(destination, "boot-state");
        g_assert_cmpstr(state, ==, "fatal-error-reboot-wait");
        g_assert_cmpuint(qom_get_uint64(
                             destination, "boot-uart-bytes"),
                         >, source_bytes);
        g_assert_cmpuint(qom_get_uint32(
                             destination, "boot-uart-lines"),
                         >, source_lines);
        qtest_quit(qts);
        qtest_quit(destination);
        unlink(migration_socket);
        rmdir(migration_dir);
    }
    unlink(eeprom_path);

    g_clear_pointer(&eeprom_path, g_free);
    qts = start_with_eeprom(
        "BOOT_ORDER=0x1\nBOOT_UART=2\n", false, &eeprom_path);
    g_clear_pointer(&state, g_free);
    state = qom_get_string(qts, "boot-state");
    g_assert_cmpstr(state, ==, "eeprom-invalid");
    g_assert_false(qom_get_bool(qts, "boot-uart-enabled"));
    g_assert_cmpuint(qom_get_uint64(qts, "boot-uart-bytes"), ==, 0);
    qtest_quit(qts);
    unlink(eeprom_path);
}
void test_cm4_vl805_eeprom_policy(void)
{
    static const uint8_t firmware[] = "QEMU CM4 VL805 EEPROM fixture";
    static const char media_config[] =
        "device_tree=bcm2711-rpi-4-b.dtb\n";
    g_autofree uint8_t *media = make_usb_boot_image(
        "START4  ELF", firmware, sizeof(firmware), media_config);
    g_autofree char *usb_path = NULL;
    g_autofree char *eeprom_path = NULL;
    g_autofree char *command = NULL;
    g_autofree char *state = NULL;
    g_autofree char *status = NULL;
    QTestState *qts;

    write_temp_image("raspi4-cm4-vl805-usb-XXXXXX",
                     media, SD_SIZE, &usb_path);

    {
        g_autofree uint8_t *eeprom = make_eeprom_image(
            "BOOT_ORDER=0x4\nVL805=1\n", false);

        write_temp_image("raspi4-cm4-vl805-enabled-XXXXXX",
                         eeprom, EEPROM_SIZE, &eeprom_path);
        command = g_strdup_printf(
            "-M raspi-cm4,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "usb-boot-drive=usbboot,usb-boot-controller=xhci "
            "-drive if=none,id=pieeprom,format=raw,file=%s "
            "-drive if=none,id=usbboot,format=raw,file=%s",
            eeprom_path, usb_path);
        qts = qtest_init(command);
        state = qom_get_string(qts, "boot-state");
        status = qom_get_string(qts, "boot-vl805-status");
        g_assert_cmpstr(state, ==, "arm-handoff-ready");
        g_assert_true(qom_get_bool(qts, "boot-vl805-enabled"));
        g_assert_true(qom_get_bool(qts, "boot-vl805-initialized"));
        g_assert_cmpstr(status, ==, "initialized");
        g_assert_cmpuint(qom_get_uint64(
                             qts, "usb-boot-controller-read-commands"),
                         >, 0);

        qtest_system_reset(qts);
        g_clear_pointer(&state, g_free);
        state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(state, ==, "arm-handoff-ready");
        g_assert_true(qom_get_bool(qts, "boot-vl805-initialized"));
        qtest_quit(qts);
    }
    unlink(eeprom_path);

    g_clear_pointer(&eeprom_path, g_free);
    g_clear_pointer(&command, g_free);
    g_clear_pointer(&state, g_free);
    g_clear_pointer(&status, g_free);
    {
        g_autofree uint8_t *eeprom = make_eeprom_image(
            "BOOT_ORDER=0xe4\n", false);

        write_temp_image("raspi4-cm4-vl805-disabled-XXXXXX",
                         eeprom, EEPROM_SIZE, &eeprom_path);
        command = g_strdup_printf(
            "-M raspi-cm4,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "usb-boot-drive=usbboot,usb-boot-controller=xhci "
            "-drive if=none,id=pieeprom,format=raw,file=%s "
            "-drive if=none,id=usbboot,format=raw,file=%s",
            eeprom_path, usb_path);
        qts = qtest_init(command);
        state = qom_get_string(qts, "boot-state");
        status = qom_get_string(qts, "firmware-status");
        g_assert_cmpstr(state, ==, "stopped");
        g_assert_false(qom_get_bool(qts, "boot-vl805-enabled"));
        g_assert_false(qom_get_bool(qts, "boot-vl805-initialized"));
        g_assert_cmpstr(status, ==, "vl805-disabled");
        g_assert_cmpuint(qom_get_uint64(
                             qts, "usb-boot-controller-read-commands"),
                         ==, 0);
        qtest_quit(qts);
    }
    unlink(eeprom_path);

    g_clear_pointer(&eeprom_path, g_free);
    g_clear_pointer(&command, g_free);
    g_clear_pointer(&state, g_free);
    g_clear_pointer(&status, g_free);
    {
        g_autofree uint8_t *eeprom = make_eeprom_image(
            "BOOT_ORDER=0x4\n"
            "VL805=1\n"
            "USB_MSD_STARTUP_DELAY=5000\n",
            false);
        g_autofree char *migration_dir = NULL;
        g_autofree char *migration_socket = NULL;
        QTestState *destination;

        write_temp_image("raspi4-cm4-vl805-migration-XXXXXX",
                         eeprom, EEPROM_SIZE, &eeprom_path);
        command = g_strdup_printf(
            "-M raspi-cm4,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "usb-boot-drive=usbboot,usb-boot-controller=xhci "
            "-drive if=none,id=pieeprom,format=raw,file=%s,"
            "file.locking=off "
            "-drive if=none,id=usbboot,format=raw,file=%s,"
            "file.locking=off",
            eeprom_path, usb_path);
        qts = qtest_init(command);
        state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(state, ==, "usb-startup-delay");
        qtest_clock_step(qts, INT64_C(1000) * 1000 * 1000);
        destination = migrate_to_new_qtest(
            qts, command, &migration_dir, &migration_socket);
        g_assert_true(qom_get_bool(
            destination, "boot-vl805-enabled"));
        g_assert_true(qom_get_bool(
            destination, "boot-vl805-initialized"));
        status = qom_get_string(destination, "boot-vl805-status");
        g_assert_cmpstr(status, ==, "initialized");
        qtest_clock_step(destination, INT64_C(3999) * 1000 * 1000);
        g_clear_pointer(&state, g_free);
        state = qom_get_string(destination, "boot-state");
        g_assert_cmpstr(state, ==, "usb-startup-delay");
        qtest_clock_step(destination, INT64_C(1) * 1000 * 1000);
        g_clear_pointer(&state, g_free);
        state = qom_get_string(destination, "boot-state");
        g_assert_cmpstr(state, ==, "arm-handoff-ready");
        qtest_quit(qts);
        qtest_quit(destination);
        unlink(migration_socket);
        rmdir(migration_dir);
    }
    unlink(eeprom_path);

    g_clear_pointer(&eeprom_path, g_free);
    g_clear_pointer(&state, g_free);
    {
        g_autofree uint8_t *eeprom = make_eeprom_image(
            "BOOT_ORDER=0x4\nVL805=2\n", false);

        write_temp_image("raspi4-cm4-vl805-invalid-XXXXXX",
                         eeprom, EEPROM_SIZE, &eeprom_path);
        command = g_strdup_printf(
            "-M raspi-cm4,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "usb-boot-drive=usbboot,usb-boot-controller=xhci "
            "-drive if=none,id=pieeprom,format=raw,file=%s "
            "-drive if=none,id=usbboot,format=raw,file=%s",
            eeprom_path, usb_path);
        qts = qtest_init(command);
        state = qom_get_string(qts, "boot-state");
        g_assert_cmpstr(state, ==, "eeprom-invalid");
        g_assert_false(qom_get_bool(qts, "boot-vl805-enabled"));
        g_assert_false(qom_get_bool(qts, "boot-vl805-initialized"));
        qtest_quit(qts);
    }
    unlink(eeprom_path);
    unlink(usb_path);
}
void test_firmware_uart_2ndstage_sd_reset_migration(void)
{
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    g_autofree char *serial_path = NULL;
    g_autofree char *serial_data = NULL;
    g_autofree char *start = NULL;
    g_autofree char *fixup = NULL;
    g_autofree char *kernel = NULL;
    g_autofree char *expected = NULL;
    g_autofree char *command = NULL;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;
    gsize serial_size;
    int serial_fd = g_file_open_tmp(
        "raspi4-uart-XXXXXX", &serial_path, NULL);
    QTestState *source;
    QTestState *destination;

    g_assert_cmpint(serial_fd, >=, 0);
    close(serial_fd);
    source = start_with_eeprom_and_media_serial(
        false, "uart_2ndstage=1\n", serial_path,
        &eeprom_path, &sd_path);
    start = qom_get_string(source, "firmware-file");
    fixup = qom_get_string(source, "firmware-fixup-file");
    kernel = qom_get_string(source, "firmware-kernel-file");
    expected = g_strdup_printf(
        "RPI4: uart_2ndstage enabled\r\n"
        "RPI4: source=sd-card start=%s fixup=%s kernel=%s\r\n"
        "RPI4: handoff=arm64 source=sd-card entry=0x%" PRIx64 "\r\n",
        start, fixup, kernel,
        qom_get_uint64(source, "arm-handoff-entry-address"));
    g_assert_true(g_file_get_contents(
        serial_path, &serial_data, &serial_size, NULL));
    g_assert_cmpmem(serial_data, serial_size,
                    expected, strlen(expected));
    g_assert_true(qom_get_bool(source, "firmware-uart-2ndstage"));
    g_assert_cmpuint(qom_get_uint64(
                         source, "firmware-uart-2ndstage-bytes"),
                     ==, strlen(expected));
    g_assert_cmpuint(qom_get_uint32(
                         source, "firmware-uart-2ndstage-lines"), ==, 3);
    qtest_system_reset(source);
    g_clear_pointer(&serial_data, g_free);
    g_assert_true(g_file_get_contents(
        serial_path, &serial_data, &serial_size, NULL));
    g_assert_cmpuint(serial_size, ==, strlen(expected) * 2);
    g_assert_cmpmem(serial_data, strlen(expected),
                    expected, strlen(expected));
    g_assert_cmpmem(serial_data + strlen(expected), strlen(expected),
                    expected, strlen(expected));
    g_assert_cmpuint(qom_get_uint64(
                         source, "firmware-uart-2ndstage-bytes"),
                     ==, strlen(expected));

    command = g_strdup_printf(
        "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom "
        "-drive if=none,id=pieeprom,format=raw,file=%s,file.locking=off "
        "-drive if=sd,format=raw,file=%s,file.locking=off "
        "-serial null -nic none",
        eeprom_path, sd_path);
    destination = migrate_to_new_qtest(
        source, command, &migration_dir, &migration_socket);
    g_assert_true(qom_get_bool(destination, "firmware-uart-2ndstage"));
    g_assert_cmpuint(qom_get_uint64(
                         destination, "firmware-uart-2ndstage-bytes"),
                     ==, strlen(expected));
    g_assert_cmpuint(qom_get_uint32(
                         destination, "firmware-uart-2ndstage-lines"),
                     ==, 3);
    qtest_quit(source);
    qtest_quit(destination);
    unlink(eeprom_path);
    unlink(sd_path);
    unlink(serial_path);
    unlink(migration_socket);
    rmdir(migration_dir);
}
void test_firmware_uart_2ndstage_network(void)
{
    g_autofree char *eeprom_path = NULL;
    g_autofree char *network_path = NULL;
    g_autofree char *serial_path = NULL;
    g_autofree char *serial_data = NULL;
    gsize serial_size;
    int serial_fd = g_file_open_tmp(
        "raspi4-uart-network-XXXXXX", &serial_path, NULL);
    QTestState *qts;

    g_assert_cmpint(serial_fd, >=, 0);
    close(serial_fd);
    qts = start_with_eeprom_and_media_serial(
        true, "uart_2ndstage=1\n", serial_path,
        &eeprom_path, &network_path);
    g_assert_true(qom_get_bool(qts, "firmware-uart-2ndstage"));
    g_assert_cmpuint(qom_get_uint32(
                         qts, "firmware-uart-2ndstage-lines"), ==, 3);
    g_assert_true(g_file_get_contents(
        serial_path, &serial_data, &serial_size, NULL));
    g_assert_nonnull(strstr(serial_data, "RPI4: source=network "));
    g_assert_nonnull(strstr(serial_data,
                            "RPI4: handoff=arm64 source=network "));
    g_assert_cmpuint(qom_get_uint64(
                         qts, "firmware-uart-2ndstage-bytes"),
                     ==, serial_size);
    qtest_quit(qts);
    unlink(eeprom_path);
    unlink(network_path);
    unlink(serial_path);
}
void test_firmware_uart_2ndstage_boundaries(void)
{
    static const uint8_t firmware[] = "QEMU invalid uart fixture";
    g_autofree char *eeprom_path = NULL;
    g_autofree char *sd_path = NULL;
    QTestState *qts = start_with_complex_firmware_config(
        "[pi4]\nuart_2ndstage=1\n", &eeprom_path, &sd_path);

    g_assert_false(qom_get_bool(qts, "firmware-uart-2ndstage"));
    g_assert_cmpuint(qom_get_uint64(
                         qts, "firmware-uart-2ndstage-bytes"), ==, 0);
    g_assert_cmpuint(qom_get_uint32(
                         qts, "firmware-config-ignored"), ==, 2);
    qtest_quit(qts);
    unlink(eeprom_path);
    unlink(sd_path);

    g_clear_pointer(&eeprom_path, g_free);
    g_clear_pointer(&sd_path, g_free);
    qts = start_with_eeprom_and_sd(
        "BOOT_ORDER=0x0\n", "START4  ELF", firmware, sizeof(firmware),
        "uart_2ndstage=2\n", &eeprom_path, &sd_path);
    g_autofree char *status = qom_get_string(qts, "firmware-status");

    g_assert_cmpstr(status, ==, "config-invalid");
    g_assert_false(qom_get_bool(qts, "firmware-uart-2ndstage"));
    qtest_quit(qts);
    unlink(eeprom_path);
    unlink(sd_path);
}
void test_arm32_memory_model_device_tree(void)
{
    static const struct {
        const char *machine;
        const char *source;
        const char *device_tree;
        unsigned int gib;
        uint32_t board_revision;
    } cases[] = {
        { "raspi4b", "sd-card", "bcm2711-rpi-4-b.dtb",
          1, 0x00a03115 },
        { "raspi4b", "sd-card", "bcm2711-rpi-4-b.dtb",
          2, 0x00b03115 },
        { "raspi4b", "sd-card", "bcm2711-rpi-4-b.dtb",
          4, 0x00c03115 },
        { "raspi4b", "sd-card", "bcm2711-rpi-4-b.dtb",
          8, 0x00d03115 },
        { "raspi-cm4", "emmc", "bcm2711-rpi-cm4.dtb",
          1, 0x00a03140 },
        { "raspi-cm4", "emmc", "bcm2711-rpi-cm4.dtb",
          2, 0x00b03140 },
        { "raspi-cm4", "emmc", "bcm2711-rpi-cm4.dtb",
          4, 0x00c03140 },
        { "raspi-cm4", "emmc", "bcm2711-rpi-cm4.dtb",
          8, 0x00d03140 },
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree char *eeprom_path = NULL;
        g_autofree char *media_path = NULL;
        g_autofree char *state = NULL;
        g_autofree char *source = NULL;
        g_autofree char *architecture = NULL;
        g_autofree char *device_tree_file = NULL;
        g_autofree char *memory_model = NULL;
        g_autofree char *expected_model = g_strdup_printf(
            "%uGiB", cases[i].gib);
        uint64_t dt_address;
        uint64_t dt_size;
        g_autofree uint8_t *dt = NULL;
        const uint8_t *reg;
        int reg_length;
        int node;
        QTestState *qts = start_arm32_memory_model(
            cases[i].machine, cases[i].gib, NULL,
            NULL, 0,
            &eeprom_path, &media_path);

        state = qom_get_string(qts, "boot-state");
        source = qom_get_string(qts, "boot-source");
        architecture = qom_get_string(qts, "arm-handoff-architecture");
        device_tree_file = qom_get_string(
            qts, "firmware-device-tree-file");
        memory_model = qom_get_string(qts, "memory-model");
        g_assert_cmpstr(state, ==, "arm-handoff-ready");
        g_assert_cmpstr(source, ==, cases[i].source);
        g_assert_cmpstr(architecture, ==, "aarch32");
        g_assert_cmpstr(device_tree_file, ==, cases[i].device_tree);
        g_assert_cmpstr(memory_model, ==, expected_model);
        g_assert_false(qom_get_bool(qts, "firmware-arm-64bit"));
        g_assert_cmphex(qom_get_uint32(qts, "board-revision"), ==,
                        cases[i].board_revision);
        g_assert_cmphex(qom_get_uint32(qts, "otp-board-revision"), ==,
                        cases[i].board_revision);
        assert_firmware_system_identity(
            qts, cases[i].board_revision, 0);
        g_assert_cmphex(firmware_kaslr_seed(qts), !=,
                        UINT64_C(0xfeedfacecafebeef));
        g_assert_cmpuint(firmware_min_boot_version(qts), ==, 0);
        g_assert_cmpuint(firmware_sdram_size_gbit(qts), ==,
                         cases[i].gib * 8);
        g_assert_cmphex(firmware_board_revision_ext(qts), ==, 0);
        assert_firmware_chosen_string(qts, "os_prefix", "");
        assert_firmware_chosen_string(qts, "overlay_prefix", "overlays/");
        assert_bootloader_build_identity(
            qts, true, 1779045198, "224877da");
        assert_bootloader_update_timestamp(qts, false, 0);
        assert_bootloader_usb_identity(qts, false, 0, 0, 0, 0);
        assert_bootloader_device_tree(qts, 1, 1, 0);
        g_assert_cmpuint(qom_get_uint32(qts, "bootloader-signed"), ==, 0);
        g_assert_cmpuint(firmware_bootloader_u32(qts, "signed"), ==, 0);
        g_assert_cmphex(qom_get_uint64(
                            qts, "arm-handoff-kernel-address"), ==,
                        0x8000);
        g_assert_cmphex(qom_get_uint64(
                            qts, "arm-handoff-entry-address"), ==, 0);
        g_assert_cmphex(qom_get_uint32(
                            qts, "arm-handoff-core-mask"), ==, 0xf);

        dt_address = qom_get_uint64(
            qts, "arm-handoff-device-tree-address");
        dt_size = qom_get_uint64(qts, "arm-handoff-device-tree-size");
        dt = g_malloc(dt_size);
        qtest_memread(qts, dt_address, dt, dt_size);
        g_assert_cmpint(fdt_check_header(dt), ==, 0);
        node = fdt_path_offset(dt, "/memory@0");
        g_assert_cmpint(node, >=, 0);
        reg = fdt_getprop(dt, node, "reg", &reg_length);
        g_assert_nonnull(reg);
        g_assert_cmpint(reg_length, ==, 4 * sizeof(uint32_t));
        g_assert_cmphex(ldq_be_p(reg), ==, 0);
        g_assert_cmphex(ldq_be_p(reg + sizeof(uint64_t)), ==,
                        948 * MiB);

        node = fdt_path_offset(dt, "/memory@40000000");
        if (cases[i].gib == 1) {
            g_assert_cmpint(node, ==, -FDT_ERR_NOTFOUND);
        } else {
            uint64_t total = (uint64_t)cases[i].gib * GiB;
            uint64_t middle_end = MIN(total, RASPI4_LOW_RAM_END);

            g_assert_cmpint(node, >=, 0);
            reg = fdt_getprop(dt, node, "reg", &reg_length);
            g_assert_nonnull(reg);
            g_assert_cmpint(reg_length, ==, 4 * sizeof(uint32_t));
            g_assert_cmphex(ldq_be_p(reg), ==, 1 * GiB);
            g_assert_cmphex(ldq_be_p(reg + sizeof(uint64_t)), ==,
                            middle_end - 1 * GiB);
        }

        node = fdt_path_offset(dt, "/memory@100000000");
        if ((uint64_t)cases[i].gib * GiB <= RASPI4_LOW_RAM_END) {
            g_assert_cmpint(node, ==, -FDT_ERR_NOTFOUND);
        } else {
            g_assert_cmpint(node, >=, 0);
            reg = fdt_getprop(dt, node, "reg", &reg_length);
            g_assert_nonnull(reg);
            g_assert_cmpint(reg_length, ==, 4 * sizeof(uint32_t));
            g_assert_cmphex(ldq_be_p(reg), ==, RASPI4_HIGH_RAM_BASE);
            g_assert_cmphex(ldq_be_p(reg + sizeof(uint64_t)), ==,
                            (uint64_t)cases[i].gib * GiB -
                            RASPI4_LOW_RAM_END);
        }

        qtest_quit(qts);
        unlink(eeprom_path);
        unlink(media_path);
    }

    {
        const uint32_t revision = 0x00b03115;
        const uint32_t revision_ext = 0x89abcdef;
        const uint32_t serial = 0x12345678;
        g_autofree uint8_t *otp = make_otp_image(
            0, 0, revision, false);
        g_autofree char *otp_path = NULL;
        g_autofree char *eeprom_path = NULL;
        g_autofree char *media_path = NULL;
        g_autofree char *command = NULL;
        g_autofree char *migration_dir = NULL;
        g_autofree char *migration_socket = NULL;
        uint32_t mailbox_serial[2] = { 0 };
        uint64_t reset_seed;
        QTestState *qts;
        QTestState *destination;

        stl_le_p(otp + (BCM2711_OTP_SERIAL_ROW - 1) * sizeof(uint32_t),
                 serial);
        stl_le_p(otp + BCM2711_OTP_SERIAL_ROW * sizeof(uint32_t), ~serial);
        stl_le_p(otp + (BCM2711_OTP_BOARD_REVISION_EXT_ROW - 1) *
                 sizeof(uint32_t), revision_ext);
        write_temp_image("raspi4-arm32-identity-otp-XXXXXX",
                         otp, OTP_SIZE, &otp_path);
        qts = start_arm32_memory_model_with_otp(
            "raspi4b", 2, NULL, NULL, NULL, 0, otp_path, 1,
            "-seed 0x12345678",
            &eeprom_path, &media_path);

        uint64_t first_seed = firmware_kaslr_seed(qts);

        assert_firmware_system_identity(qts, revision, serial);
        assert_bootloader_build_identity(
            qts, true, 1779045198, "224877da");
        assert_bootloader_update_timestamp(qts, false, 0);
        assert_bootloader_usb_identity(qts, false, 0, 0, 0, 0);
        g_assert_cmpuint(qom_get_uint32(qts, "min-boot-version"), ==, 1);
        g_assert_cmpuint(firmware_min_boot_version(qts), ==, 1);
        g_assert_cmpuint(firmware_sdram_size_gbit(qts), ==, 16);
        g_assert_cmphex(firmware_board_revision_ext(qts), ==, revision_ext);
        {
            QDict *response = qtest_qmp(
                qts, "{ 'execute': 'qom-set', 'arguments': "
                     "{ 'path': '/machine', "
                     "'property': 'min-boot-version', 'value': 2 } }");

            g_assert(qdict_haskey(response, "error"));
            qobject_unref(response);
        }
        g_assert_cmphex(first_seed, !=,
                        UINT64_C(0xfeedfacecafebeef));
        bcm2711_property_call(
            qts, RPI_FWREQ_GET_BOARD_SERIAL, mailbox_serial,
            G_N_ELEMENTS(mailbox_serial));
        g_assert_cmphex(mailbox_serial[0], ==, serial);
        g_assert_cmphex(mailbox_serial[1], ==, 0);
        qtest_system_reset(qts);
        assert_firmware_system_identity(qts, revision, serial);
        assert_bootloader_build_identity(
            qts, true, 1779045198, "224877da");
        assert_bootloader_update_timestamp(qts, false, 0);
        assert_bootloader_usb_identity(qts, false, 0, 0, 0, 0);
        g_assert_cmpuint(firmware_min_boot_version(qts), ==, 1);
        g_assert_cmpuint(firmware_sdram_size_gbit(qts), ==, 16);
        g_assert_cmphex(firmware_board_revision_ext(qts), ==, revision_ext);
        reset_seed = firmware_kaslr_seed(qts);
        g_assert_cmphex(reset_seed, !=, first_seed);

        command = g_strdup_printf(
            "-M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
            "otp-drive=piotp,min-boot-version=9 -m 2G "
            "-drive if=none,id=pieeprom,format=raw,file=%s,"
            "file.locking=off "
            "-drive if=sd,format=raw,file=%s,file.locking=off "
            "-drive if=none,id=piotp,format=raw,file=%s,"
            "file.locking=off -nic none -seed 0x12345678",
            eeprom_path, media_path, otp_path);
        destination = migrate_to_new_qtest(
            qts, command, &migration_dir, &migration_socket);
        g_assert_cmpuint(qom_get_uint32(
                             destination, "min-boot-version"), ==, 1);
        g_assert_cmpuint(firmware_min_boot_version(destination), ==, 1);
        g_assert_cmpuint(firmware_sdram_size_gbit(destination), ==, 16);
        g_assert_cmphex(firmware_board_revision_ext(destination), ==,
                        revision_ext);
        g_assert_cmphex(firmware_kaslr_seed(destination), ==, reset_seed);
        assert_bootloader_build_identity(
            destination, true, 1779045198, "224877da");
        assert_bootloader_update_timestamp(destination, false, 0);
        assert_bootloader_usb_identity(destination, false, 0, 0, 0, 0);
        g_assert_cmpuint(qom_get_uint32(
                             destination, "bootloader-signed"), ==, 0);
        g_assert_cmpuint(firmware_bootloader_u32(
                             destination, "signed"), ==, 0);

        qtest_quit(qts);
        qtest_quit(destination);
        unlink(migration_socket);
        rmdir(migration_dir);
        unlink(eeprom_path);
        unlink(media_path);

        g_clear_pointer(&eeprom_path, g_free);
        g_clear_pointer(&media_path, g_free);
        qts = start_arm32_memory_model_with_otp(
            "raspi4b", 2, NULL, NULL, NULL, 0, otp_path, 1,
            "-seed 0x12345678",
            &eeprom_path, &media_path);
        g_assert_cmphex(firmware_kaslr_seed(qts), ==, first_seed);
        g_assert_cmpuint(firmware_min_boot_version(qts), ==, 1);
        g_assert_cmpuint(firmware_sdram_size_gbit(qts), ==, 16);
        g_assert_cmphex(firmware_board_revision_ext(qts), ==, revision_ext);
        assert_bootloader_build_identity(
            qts, true, 1779045198, "224877da");
        assert_bootloader_update_timestamp(qts, false, 0);
        assert_bootloader_usb_identity(qts, false, 0, 0, 0, 0);

        qtest_quit(qts);
        unlink(otp_path);
        unlink(eeprom_path);
        unlink(media_path);
    }
}
void test_bcm2711_pcie_platform(void)
{
    QTestState *qts = qtest_init("-M raspi4b");
    uint8_t pcie_cap;
    uint32_t id;
    uint32_t class_revision;

    g_assert_cmphex(qtest_readl(qts, RASPI4_PCIE_ECAM_BASE), ==,
                    0x271114e4);
    g_assert_cmphex(qtest_readl(qts, RASPI4_PCIE_ECAM_BASE + 8), ==,
                    0x06040020);
    pcie_cap = qtest_readb(qts, RASPI4_PCIE_ECAM_BASE + PCI_CAPABILITY_LIST);
    while (pcie_cap &&
           qtest_readb(qts, RASPI4_PCIE_ECAM_BASE + pcie_cap) !=
           PCI_CAP_ID_EXP) {
        pcie_cap = qtest_readb(qts, RASPI4_PCIE_ECAM_BASE + pcie_cap + 1);
    }
    g_assert_cmphex(pcie_cap, !=, 0);
    g_assert_cmphex(
        qtest_readl(qts, RASPI4_PCIE_ECAM_BASE + pcie_cap + PCI_EXP_LNKCAP) &
            (PCI_EXP_LNKCAP_SLS | PCI_EXP_LNKCAP_MLW),
        ==, PCI_EXP_LNKCAP_SLS_5_0GB | PCI_EXP_LNKSTA_NLW_X1);
    g_assert_cmphex(qtest_readl(qts, RASPI4_PCIE_MSI_INTR_STATUS), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RASPI4_PCIE_MSI_INTR_MASK_SET), ==,
                    UINT32_MAX);
    qtest_writel(qts, RASPI4_PCIE_MSI_BAR_CONFIG_LO,
                 (uint32_t)RASPI4_PCIE_MSI_TARGET_LOW | 1);
    qtest_writel(qts, RASPI4_PCIE_MSI_BAR_CONFIG_HI,
                 RASPI4_PCIE_MSI_TARGET_LOW >> 32);
    qtest_writel(qts, RASPI4_PCIE_MSI_DATA_CONFIG,
                 RASPI4_PCIE_MSI_DATA_CONFIG_VALUE);
    qtest_writel(qts, RASPI4_PCIE_MSI_INTR_MASK_CLR, BIT(5));
    qtest_writel(qts, RASPI4_PCIE_MSI_TARGET_LOW,
                 RASPI4_PCIE_MSI_DATA_CONFIG_VALUE | 5);
    g_assert_cmphex(qtest_readl(qts, RASPI4_PCIE_MSI_INTR_STATUS), ==,
                    BIT(5));
    g_assert_true(qtest_readl(qts, BCM2711_GICD_ISPENDR5) &
                  BCM2711_GICD_PCI_MSI_BIT);
    qtest_writel(qts, RASPI4_PCIE_MSI_INTR_MASK_SET, BIT(5));
    g_assert_false(qtest_readl(qts, BCM2711_GICD_ISPENDR5) &
                   BCM2711_GICD_PCI_MSI_BIT);
    qtest_writel(qts, RASPI4_PCIE_MSI_INTR_MASK_CLR, BIT(5));
    g_assert_true(qtest_readl(qts, BCM2711_GICD_ISPENDR5) &
                  BCM2711_GICD_PCI_MSI_BIT);
    qtest_writel(qts, RASPI4_PCIE_MSI_INTR_CLR, BIT(5));
    g_assert_cmphex(qtest_readl(qts, RASPI4_PCIE_MSI_INTR_STATUS), ==, 0);
    g_assert_false(qtest_readl(qts, BCM2711_GICD_ISPENDR5) &
                   BCM2711_GICD_PCI_MSI_BIT);
    qtest_writel(qts, RASPI4_PCIE_MSI_BAR_CONFIG_LO,
                 (uint32_t)RASPI4_PCIE_MSI_TARGET_HIGH | 1);
    qtest_writel(qts, RASPI4_PCIE_MSI_BAR_CONFIG_HI,
                 RASPI4_PCIE_MSI_TARGET_HIGH >> 32);
    qtest_writel(qts, RASPI4_PCIE_MSI_INTR_MASK_CLR, BIT(6));
    qtest_writel(qts, RASPI4_PCIE_MSI_TARGET_HIGH,
                 RASPI4_PCIE_MSI_DATA_CONFIG_VALUE | 6);
    g_assert_cmphex(qtest_readl(qts, RASPI4_PCIE_MSI_INTR_STATUS), ==,
                    BIT(6));
    g_assert_true(qtest_readl(qts, BCM2711_GICD_ISPENDR5) &
                  BCM2711_GICD_PCI_MSI_BIT);
    qtest_writel(qts, RASPI4_PCIE_MSI_INTR_CLR, BIT(6));
    qtest_writel(qts, RASPI4_PCIE_ECAM_BASE + 0x18, 0x00010100);
    qtest_writel(qts, RASPI4_PCIE_ECAM_BASE + 0x20, 0xfff0c000);
    qtest_writew(qts, RASPI4_PCIE_ECAM_BASE + 4, 2);
    qtest_writel(qts, RASPI4_PCIE_EXT_CFG_INDEX, 1 << 20);
    id = qtest_readl(qts, RASPI4_PCIE_EXT_CFG_DATA);
    class_revision = qtest_readl(qts, RASPI4_PCIE_EXT_CFG_DATA + 8);
    g_assert_cmphex(id, ==, 0x34831106);
    g_assert_cmphex(class_revision >> 8, ==, 0x0c0330);

    qtest_writel(qts, RASPI4_PCIE_EXT_CFG_DATA + 0x10, 0xc0000004);
    qtest_writel(qts, RASPI4_PCIE_EXT_CFG_DATA + 0x14, 0);
    qtest_writew(qts, RASPI4_PCIE_EXT_CFG_DATA + 4, 2);
    g_assert_cmphex(qtest_readl(qts, RASPI4_PCIE_MMIO_BASE), ==,
                    0x01000040);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, RASPI4_PCIE_ECAM_BASE), ==,
                    0x271114e4);
    g_assert_cmphex(qtest_readl(qts, RASPI4_PCIE_ECAM_BASE + 8), ==,
                    0x06040020);
    g_assert_cmphex(qtest_readl(qts, RASPI4_PCIE_MSI_INTR_STATUS), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RASPI4_PCIE_MSI_INTR_MASK_SET), ==,
                    UINT32_MAX);
    g_assert_false(qtest_readl(qts, BCM2711_GICD_ISPENDR5) &
                   BCM2711_GICD_PCI_MSI_BIT);
    qtest_quit(qts);

    qts = qtest_init(
        "-M raspi-cm4 "
        "-device nvme,bus=pcie-root,serial=QEMU-RPI-NVME");
    g_assert_cmphex(qtest_readl(qts, RASPI4_PCIE_ECAM_BASE), ==,
                    0x271114e4);
    g_assert_cmphex(qtest_readl(qts, RASPI4_PCIE_ECAM_BASE + 8), ==,
                    0x06040020);
    qtest_writel(qts, RASPI4_PCIE_ECAM_BASE + 0x18, 0x00010100);
    qtest_writel(qts, RASPI4_PCIE_EXT_CFG_INDEX, 1 << 20);
    g_assert_cmphex(qtest_readl(qts, RASPI4_PCIE_EXT_CFG_DATA), ==,
                    0x00101b36);
    qtest_quit(qts);
}
void test_bcm2711_pcie_migration(void)
{
    static const char command[] = "-M raspi4b";
    QTestState *source = qtest_init(command);
    QTestState *destination;
    g_autofree char *migration_dir = NULL;
    g_autofree char *migration_socket = NULL;

    qtest_writel(source, RASPI4_PCIE_ECAM_BASE + 0x18, 0x00010100);
    qtest_writel(source, RASPI4_PCIE_ECAM_BASE + 0x20, 0xfff0c000);
    qtest_writew(source, RASPI4_PCIE_ECAM_BASE + 4, 2);
    qtest_writel(source, RASPI4_PCIE_ECAM_BASE + 0x4008, 0x5a5aa5a5);
    qtest_writel(source, RASPI4_PCIE_EXT_CFG_INDEX, 1 << 20);
    qtest_writel(source, RASPI4_PCIE_EXT_CFG_DATA + 0x10, 0xc0000004);
    qtest_writel(source, RASPI4_PCIE_EXT_CFG_DATA + 0x14, 0);
    qtest_writew(source, RASPI4_PCIE_EXT_CFG_DATA + 4, 2);
    qtest_writel(source, RASPI4_PCIE_MSI_BAR_CONFIG_LO,
                 (uint32_t)RASPI4_PCIE_MSI_TARGET_HIGH | 1);
    qtest_writel(source, RASPI4_PCIE_MSI_BAR_CONFIG_HI,
                 RASPI4_PCIE_MSI_TARGET_HIGH >> 32);
    qtest_writel(source, RASPI4_PCIE_MSI_DATA_CONFIG,
                 RASPI4_PCIE_MSI_DATA_CONFIG_VALUE);
    qtest_writel(source, RASPI4_PCIE_MSI_INTR_MASK_CLR, BIT(7));
    qtest_writel(source, RASPI4_PCIE_MSI_TARGET_HIGH,
                 RASPI4_PCIE_MSI_DATA_CONFIG_VALUE | 7);
    g_assert_true(qtest_readl(source, BCM2711_GICD_ISPENDR5) &
                  BCM2711_GICD_PCI_MSI_BIT);

    destination = migrate_to_new_qtest(source, command, &migration_dir,
                                       &migration_socket);
    g_assert_cmphex(qtest_readl(destination,
                               RASPI4_PCIE_ECAM_BASE + 0x4008), ==,
                    0x5a5aa5a5);
    g_assert_cmphex(qtest_readl(destination, RASPI4_PCIE_ECAM_BASE), ==,
                    0x271114e4);
    g_assert_cmphex(qtest_readl(destination,
                               RASPI4_PCIE_ECAM_BASE + 8), ==,
                    0x06040020);
    g_assert_cmphex(qtest_readl(destination,
                               RASPI4_PCIE_MSI_INTR_STATUS), ==, BIT(7));
    g_assert_cmphex(qtest_readl(destination,
                               RASPI4_PCIE_MSI_INTR_MASK_SET), ==,
                    UINT32_MAX & ~BIT(7));
    g_assert_true(qtest_readl(destination, BCM2711_GICD_ISPENDR5) &
                  BCM2711_GICD_PCI_MSI_BIT);
    g_assert_cmphex(qtest_readl(destination, RASPI4_PCIE_EXT_CFG_INDEX), ==,
                    1 << 20);
    g_assert_cmphex(qtest_readl(destination, RASPI4_PCIE_EXT_CFG_DATA), ==,
                    0x34831106);
    g_assert_cmphex(qtest_readl(destination, RASPI4_PCIE_MMIO_BASE), ==,
                    0x01000040);
    qtest_writel(destination, RASPI4_PCIE_MSI_INTR_CLR, BIT(7));
    g_assert_false(qtest_readl(destination, BCM2711_GICD_ISPENDR5) &
                   BCM2711_GICD_PCI_MSI_BIT);

    qtest_quit(source);
    qtest_quit(destination);
    unlink(migration_socket);
    rmdir(migration_dir);
}
