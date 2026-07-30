/*
 * Raspberry Pi 4B emulation
 *
 * Copyright (C) 2022 Ovchinnikov Vitalii <vitalii.ovchinnikov@auriga.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/arm/raspi4b-internal.h"


BCM2835PowerMgtState *raspi4_powermgt(Raspi4bMachineState *s)
{
    return &s->soc.peripherals.parent_obj.powermgt;
}

BCM2835PropertyState *raspi4_property(Raspi4bMachineState *s)
{
    return &s->soc.peripherals.parent_obj.property;
}

const char *raspi4_config_path(const Raspi4bMachineState *s)
{
    return s->tryboot && !s->tryboot_a_b ? "tryboot.txt" : "config.txt";
}

DWC2State *raspi4_dwc2(Raspi4bMachineState *s)
{
    return &s->soc.peripherals.parent_obj.dwc2;
}

void raspi4_hdmi_diagnostics_show(Raspi4bMachineState *s)
{
    timer_del(s->hdmi_diagnostics_timer);
    s->hdmi_diagnostics_pending = false;
    s->hdmi_diagnostics_remaining_ns = 0;
    if (!s->boot_disable_hdmi) {
        s->hdmi_diagnostics_visible = true;
    }
}

static void raspi4_hdmi_diagnostics_expired(void *opaque)
{
    raspi4_hdmi_diagnostics_show(opaque);
}

static void raspi4_hdmi_diagnostics_arm(Raspi4bMachineState *s)
{
    uint64_t delay_ns =
        (uint64_t)s->hdmi_delay * NANOSECONDS_PER_SECOND;

    timer_del(s->hdmi_diagnostics_timer);
    s->hdmi_diagnostics_pending = false;
    s->hdmi_diagnostics_visible = false;
    s->hdmi_diagnostics_remaining_ns = 0;
    if (s->boot_disable_hdmi) {
        return;
    }
    if (!delay_ns) {
        raspi4_hdmi_diagnostics_show(s);
        return;
    }
    s->hdmi_diagnostics_pending = true;
    timer_mod_ns(s->hdmi_diagnostics_timer,
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay_ns);
}

void raspi4_set_boot_observation(Raspi4bMachineState *s,
                                        const char *state,
                                        const char *source)
{
    const char *effective_source = source ? source : "none";
    bool changed = !s->boot_state || !s->boot_source ||
                   strcmp(s->boot_state, state) ||
                   strcmp(s->boot_source, effective_source);

    g_free(s->boot_state);
    g_free(s->boot_source);
    s->boot_state = g_strdup(state);
    s->boot_source = g_strdup(effective_source);
    if (!strcmp(state, "arm-handoff-ready") && s->boot_watchdog_armed) {
        timer_del(s->boot_watchdog_timer);
        s->boot_watchdog_armed = false;
        s->boot_watchdog_remaining_ns = 0;
    }
    if (!strcmp(state, "arm-handoff-ready") &&
        s->hdmi_diagnostics_pending) {
        timer_del(s->hdmi_diagnostics_timer);
        s->hdmi_diagnostics_pending = false;
        s->hdmi_diagnostics_remaining_ns = 0;
    }
    if (s->cm4 && s->provision_state_file &&
        !strcmp(state, "rpiboot-wait")) {
        s->provision_rpiboot_owned =
            raspi4_provision_write(s, "qemu-rpiboot-wait");
    }
    if (!strcmp(state, "rpiboot-wait")) {
        raspi4_rpiboot_dwc2_start(s);
    }
    if (changed && !runstate_check(RUN_STATE_INMIGRATE)) {
        raspi4_boot_uart_observation(s, state, effective_source);
        raspi4_netconsole_observation(s, state, effective_source);
    }
}



void raspi4_set_secure_boot_status(Raspi4bMachineState *s,
                                          const char *status)
{
    g_free(s->secure_boot_status);
    s->secure_boot_status = g_strdup(status);
}

static uint8_t raspi4_decode_boot_partition(uint32_t reset_status)
{
    uint8_t partition = 0;

    for (unsigned int bit = 0; bit < 6; bit++) {
        partition |= ((reset_status >> (bit * 2)) & 1) << bit;
    }
    return partition;
}

static const char *raspi4_reset_cause_name(uint32_t reset_status)
{
    if (reset_status & RASPI4_PM_RSTS_HADWRF) {
        return "watchdog";
    }
    if (reset_status & RASPI4_PM_RSTS_HADSRF) {
        return "software";
    }
    if (reset_status & RASPI4_PM_RSTS_HADDRF) {
        return "debug";
    }
    if (reset_status & RASPI4_PM_RSTS_HADPOR) {
        return "power-on";
    }
    return "unknown";
}

static bool raspi4_read_otp_policy(Raspi4bMachineState *s)
{
    BCM2835OTPState *otp = raspi4_otp(s);
    uint32_t bootmode_copy;

    s->otp_bootmode = bcm2835_otp_get_row(
        otp, BCM2711_OTP_BOOTMODE_ROW);
    bootmode_copy = bcm2835_otp_get_row(
        otp, BCM2711_OTP_BOOTMODE_COPY_ROW);
    s->otp_board_revision = bcm2835_otp_get_row(
        otp, BCM2711_OTP_BOARD_REVISION_ROW);
    s->otp_secure_boot_flags = bcm2835_otp_get_row(
        otp, BCM2711_OTP_SECURE_BOOT_FLAGS_ROW);
    s->otp_secure_boot = s->otp_bootmode &
                         BCM2711_OTP_BOOTMODE_SECURE_BOOT;

    trace_raspi4b_boot_event("rom", "rom.otp-read", "none", "success",
                             s->otp_bootmode);
    if (s->otp_bootmode != bootmode_copy) {
        raspi4_set_boot_observation(s, "otp-bootmode-invalid", "none");
        trace_raspi4b_boot_event("rom", "rom.otp-copy", "none", "failure",
                                 bootmode_copy);
        return false;
    }
    if (s->otp_board_revision != s->board_revision) {
        raspi4_set_boot_observation(s, "otp-board-mismatch", "none");
        trace_raspi4b_boot_event("rom", "rom.otp-board", "none", "failure",
                                 s->otp_board_revision);
        return false;
    }
    return true;
}

const char *raspi4_validate_recovery(Raspi4bMachineState *s,
                                            const uint8_t *image,
                                            size_t image_length)
{
    g_autofree char *digest = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, image, image_length);
    const uint8_t *rsa;
    const uint8_t *hmac;
    size_t payload_length;
    uint32_t embedded_length;
    uint32_t key_index;
    bool rsa_nonzero = false;
    bool hmac_nonzero = false;

    pstrcpy((char *)s->recovery_sha256, sizeof(s->recovery_sha256), digest);
    s->recovery_key_index = UINT32_MAX;
    if (image_length <= RASPI4_RECOVERY_TRAILER_SIZE ||
        image_length > RASPI4_RECOVERY_SIGNED_MAX_SIZE) {
        return "recovery-format-invalid";
    }

    payload_length = image_length - RASPI4_RECOVERY_TRAILER_SIZE;
    embedded_length = ldl_le_p(image + payload_length);
    key_index = ldl_le_p(image + payload_length + sizeof(uint32_t));
    rsa = image + payload_length + sizeof(uint32_t) * 2;
    hmac = rsa + RASPI4_RECOVERY_RSA_SIZE;
    if (embedded_length != payload_length || key_index > 4) {
        return "recovery-format-invalid";
    }
    for (size_t i = 0; i < RASPI4_RECOVERY_RSA_SIZE; i++) {
        rsa_nonzero |= rsa[i] != 0;
    }
    for (size_t i = 0; i < RASPI4_RECOVERY_HMAC_SIZE; i++) {
        hmac_nonzero |= hmac[i] != 0;
    }
    if (!rsa_nonzero || !hmac_nonzero) {
        return "recovery-format-invalid";
    }

    s->recovery_key_index = key_index;
    if (!s->recovery_trusted_sha256) {
        return "recovery-trust-required";
    }
    if (strcmp(digest, s->recovery_trusted_sha256)) {
        return "recovery-trust-mismatch";
    }
    return NULL;
}

Raspi4ProgramResult raspi4_program_eeprom(Raspi4bMachineState *s,
                                                 const uint8_t *image,
                                                 size_t image_length,
                                                 bool allow_timing)
{
    g_autofree uint8_t *erase = g_malloc(4096);
    g_autofree uint8_t *current = g_malloc(256);
    g_autofree uint8_t *page = g_malloc(256);
    g_autofree uint8_t *verify = g_malloc(4096);
    size_t erased = 0;
    size_t programmed = 0;
    size_t verified = 0;
    size_t erase_limit = s->eeprom_fail_stage == RASPI4_EEPROM_FAIL_ERASE ?
                         MIN((uint64_t)image_length, s->eeprom_fail_after) :
                         image_length;
    size_t program_limit =
        s->eeprom_fail_stage == RASPI4_EEPROM_FAIL_PROGRAM ?
        MIN((uint64_t)image_length, s->eeprom_fail_after) : image_length;
    size_t verify_limit = s->eeprom_fail_stage == RASPI4_EEPROM_FAIL_VERIFY ?
                          MIN((uint64_t)image_length, s->eeprom_fail_after) :
                          image_length;

    if (allow_timing && s->eeprom_flash_timing_complete) {
        bool matches = s->eeprom_flash_image_size == image_length &&
                       !memcmp(s->eeprom_flash_image, image, image_length);

        s->eeprom_flash_timing_complete = false;
        s->eeprom_flash_image_size = 0;
        g_clear_pointer(&s->eeprom_flash_image, g_free);
        return matches ? RASPI4_PROGRAM_OK : RASPI4_PROGRAM_ERROR;
    }
    if (allow_timing &&
        (s->eeprom_erase_delay_us || s->eeprom_program_delay_us ||
         s->eeprom_verify_delay_us)) {
        if (s->pending_boot_action == RASPI4_PENDING_EEPROM_FLASH) {
            return RASPI4_PROGRAM_PENDING;
        }
        g_clear_pointer(&s->eeprom_flash_image, g_free);
        s->eeprom_flash_image = g_memdup2(image, image_length);
        s->eeprom_flash_image_size = image_length;
        s->eeprom_flash_timing_complete = false;
        s->eeprom_erased_bytes = 0;
        s->eeprom_programmed_bytes = 0;
        s->eeprom_verified_bytes = 0;
        s->eeprom_dirty_sector_count = 0;
        s->eeprom_program_page_count = 0;
        s->eeprom_nor_violation_bits = 0;
        s->eeprom_flash_elapsed_us = 0;
        s->eeprom_flash_deadline_us = 0;
        s->eeprom_flash_stage = RASPI4_EEPROM_FLASH_ERASE;
        s->pending_boot_action = RASPI4_PENDING_EEPROM_FLASH;
        raspi4_eeprom_flash_schedule(s, false);
        return RASPI4_PROGRAM_PENDING;
    }

    s->eeprom_erased_bytes = 0;
    s->eeprom_programmed_bytes = 0;
    s->eeprom_verified_bytes = 0;
    s->eeprom_dirty_sector_count = 0;
    s->eeprom_program_page_count = 0;
    s->eeprom_nor_violation_bits = 0;
    s->eeprom_flash_elapsed_us = 0;
    s->eeprom_flash_deadline_us = 0;
    s->eeprom_flash_stage = RASPI4_EEPROM_FLASH_ERASE;
    while (erased < erase_limit) {
        size_t chunk = MIN((size_t)4096, erase_limit - erased);

        memset(erase, 0xff, chunk);
        if (s->eeprom_stuck_zero_offset >= erased &&
            s->eeprom_stuck_zero_offset < erased + chunk) {
            erase[s->eeprom_stuck_zero_offset - erased] &=
                ~s->eeprom_stuck_zero_mask;
        }
        if (blk_pwrite(s->eeprom, erased, chunk, erase, 0) < 0) {
            return RASPI4_PROGRAM_ERROR;
        }
        erased += chunk;
        s->eeprom_erased_bytes = erased;
        s->eeprom_dirty_sector_count = DIV_ROUND_UP(erased, 4096);
    }
    if (blk_flush(s->eeprom) < 0) {
        return RASPI4_PROGRAM_ERROR;
    }
    if (erased != image_length) {
        return RASPI4_ERASE_INTERRUPTED;
    }

    s->eeprom_flash_stage = RASPI4_EEPROM_FLASH_PROGRAM;
    while (programmed < program_limit) {
        size_t chunk = MIN((size_t)256, program_limit - programmed);
        uint32_t violations = 0;

        if (blk_pread(s->eeprom, programmed, chunk, current, 0) < 0) {
            return RASPI4_PROGRAM_ERROR;
        }
        for (size_t i = 0; i < chunk; i++) {
            uint8_t violation = image[programmed + i] & ~current[i];

            violations += ctpop8(violation);
            page[i] = current[i] & image[programmed + i];
        }
        if (blk_pwrite(s->eeprom, programmed, chunk, page, 0) < 0) {
            return RASPI4_PROGRAM_ERROR;
        }
        programmed += chunk;
        s->eeprom_programmed_bytes = programmed;
        s->eeprom_program_page_count++;
        s->eeprom_nor_violation_bits += violations;
        s->eeprom_dirty_sector_count = MAX(
            s->eeprom_dirty_sector_count,
            (uint32_t)DIV_ROUND_UP(programmed, 4096));
        if (violations) {
            if (blk_flush(s->eeprom) < 0) {
                return RASPI4_PROGRAM_ERROR;
            }
            return RASPI4_PROGRAM_NOR_VIOLATION;
        }
    }
    if (blk_flush(s->eeprom) < 0) {
        return RASPI4_PROGRAM_ERROR;
    }
    if (programmed != image_length) {
        return RASPI4_PROGRAM_INTERRUPTED;
    }

    s->eeprom_flash_stage = RASPI4_EEPROM_FLASH_VERIFY;
    while (verified < verify_limit) {
        size_t chunk = MIN((size_t)4096, verify_limit - verified);
        size_t matched = 0;

        if (blk_pread(s->eeprom, verified, chunk, verify, 0) < 0) {
            return RASPI4_PROGRAM_ERROR;
        }
        if (s->eeprom_fail_stage ==
                RASPI4_EEPROM_FAIL_VERIFY_MISMATCH &&
            s->eeprom_fail_after >= verified &&
            s->eeprom_fail_after < verified + chunk) {
            verify[s->eeprom_fail_after - verified] ^= 1;
        }
        if (memcmp(verify, image + verified, chunk)) {
            while (matched < chunk &&
                   verify[matched] == image[verified + matched]) {
                matched++;
            }
            s->eeprom_verified_bytes = verified + matched;
            return RASPI4_VERIFY_FAILED;
        }
        verified += chunk;
        s->eeprom_verified_bytes = verified;
    }
    if (verified != image_length) {
        return RASPI4_VERIFY_INTERRUPTED;
    }
    s->eeprom_flash_stage = RASPI4_EEPROM_FLASH_COMPLETE;
    return RASPI4_PROGRAM_OK;
}

bool raspi4_sd_overcurrent_live(Raspi4bMachineState *s,
                                        bool *bridge_driven)
{
    int level = bcm2838_gpio_get_sd_overcurrent(
        &s->soc.peripherals.gpio);

    *bridge_driven = level >= 0;
    return level >= 0 ? level : s->sd_overcurrent;
}

bool raspi4_mac_address_valid(const MACAddr *mac)
{
    static const uint8_t zero[6];
    static const uint8_t broadcast[6] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff
    };

    return !(mac->a[0] & 1) &&
           memcmp(mac->a, zero, sizeof(mac->a)) &&
           memcmp(mac->a, broadcast, sizeof(mac->a));
}

static void raspi4_apply_mac_address(Raspi4bMachineState *s,
                                     const Raspi4BootConfig *config)
{
    BCM2711GenetState *genet = raspi4_genet(s);

    bcm2711_genet_restore_mac(genet);
    if (config->mac_address_source != RASPI4_MAC_CONFIGURED) {
        bcm2711_genet_set_mac(genet, &config->mac_address);
    }
    s->boot_mac_address = *bcm2711_genet_mac(genet);
    s->boot_mac_address_source = config->mac_address_source;
    bcm2835_property_set_mac(raspi4_property(s), &s->boot_mac_address);
}

static void raspi4_set_tftp_prefix(Raspi4bMachineState *s,
                                   const Raspi4BootConfig *config)
{
    const MACAddr *mac = bcm2711_genet_mac(raspi4_genet(s));
    uint32_t serial = bcm2835_otp_get_row(
        raspi4_otp(s), BCM2711_OTP_SERIAL_ROW);

    s->tftp_prefix_mode = config->tftp_prefix;
    s->tftp_prefix[0] = 0;
    switch (config->tftp_prefix) {
    case 0:
        if (serial) {
            snprintf((char *)s->tftp_prefix, sizeof(s->tftp_prefix),
                     "%08" PRIx32 "/", serial);
        }
        break;
    case 1:
        pstrcpy((char *)s->tftp_prefix, sizeof(s->tftp_prefix),
                config->tftp_prefix_str);
        break;
    case 2:
        snprintf((char *)s->tftp_prefix, sizeof(s->tftp_prefix),
                 "%02x-%02x-%02x-%02x-%02x-%02x/",
                 mac->a[0], mac->a[1], mac->a[2],
                 mac->a[3], mac->a[4], mac->a[5]);
        break;
    default:
        g_assert_not_reached();
    }
}

bool raspi4_ipv4_is_unicast(uint32_t address)
{
    return address && address != UINT32_MAX && !IN_MULTICAST(address);
}

static bool raspi4_parse_timeout(const char *text, uint32_t minimum,
                                 uint32_t *value)
{
    uint64_t parsed;

    if (qemu_strtou64(text, NULL, 0, &parsed) < 0 || parsed < minimum ||
        parsed > UINT32_MAX) {
        return false;
    }
    *value = parsed;
    return true;
}

static bool raspi4_parse_bounded_timeout(const char *text, uint32_t minimum,
                                         uint32_t maximum, uint32_t *value)
{
    uint64_t parsed;

    if (qemu_strtou64(text, NULL, 0, &parsed) < 0 || parsed < minimum ||
        parsed > maximum) {
        return false;
    }
    *value = parsed;
    return true;
}

static bool raspi4_parse_usb_msd_exclude_vid_pid(
    const char *text,
    uint32_t values[RASPI4_USB_MSD_EXCLUDE_MAX],
    uint8_t *count)
{
    const char *entry = text;
    uint8_t parsed_count = 0;

    memset(values, 0, sizeof(uint32_t) * RASPI4_USB_MSD_EXCLUDE_MAX);
    *count = 0;
    if (!text[0]) {
        return true;
    }
    while (entry[0]) {
        size_t entry_length = strcspn(entry, ",");
        uint32_t value = 0;

        if (parsed_count == RASPI4_USB_MSD_EXCLUDE_MAX ||
            entry_length != 8) {
            return false;
        }
        for (unsigned int digit = 0; digit < 8; digit++) {
            int nibble = g_ascii_xdigit_value(entry[digit]);

            if (nibble < 0) {
                return false;
            }
            value = (value << 4) | nibble;
        }
        values[parsed_count++] = value;
        if (!entry[8]) {
            break;
        }
        entry += 9;
        if (!entry[0]) {
            return false;
        }
    }
    *count = parsed_count;
    return true;
}

static bool raspi4_parse_http_host(const char *text, char *host,
                                   size_t host_size)
{
    size_t length = strlen(text);
    size_t label_length = 0;

    if (!length || length >= host_size || text[0] == '.' ||
        text[length - 1] == '.') {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        unsigned char c = text[i];

        if (c == '.') {
            if (!label_length || text[i - 1] == '-') {
                return false;
            }
            label_length = 0;
        } else if (g_ascii_islower(c) || g_ascii_isdigit(c) || c == '-') {
            if ((!label_length && c == '-') || ++label_length > 63) {
                return false;
            }
        } else {
            return false;
        }
    }
    if (!label_length || text[length - 1] == '-') {
        return false;
    }
    pstrcpy(host, host_size, text);
    return true;
}

static bool raspi4_parse_http_path(const char *text, char *path,
                                   size_t path_size)
{
    const char *start = text;
    size_t length;

    while (*start == '/') {
        start++;
    }
    length = strlen(start);
    while (length && start[length - 1] == '/') {
        length--;
    }
    if (!length || length >= path_size) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        unsigned char c = start[i];

        if (c <= 0x20 || c >= 0x7f || c == '?' || c == '#' || c == '\\') {
            return false;
        }
    }
    memcpy(path, start, length);
    path[length] = 0;
    return true;
}

bool raspi4_parse_sha256(const char *text, char digest[65])
{
    if (strlen(text) != 64) {
        return false;
    }
    for (unsigned int i = 0; i < 64; i++) {
        if (!g_ascii_isxdigit(text[i])) {
            return false;
        }
        digest[i] = g_ascii_tolower(text[i]);
    }
    digest[64] = 0;
    return true;
}

static bool raspi4_parse_nonnegative_or_infinite(const char *text,
                                                 int32_t *value)
{
    int64_t parsed;

    if (qemu_strtoi64(text, NULL, 0, &parsed) < 0 || parsed < -1 ||
        parsed > INT32_MAX) {
        return false;
    }
    *value = parsed;
    return true;
}

static bool raspi4_parse_boot_config(Raspi4bMachineState *s,
                                     const uint8_t *data, size_t length,
                                     Raspi4BootConfig *config)
{
    g_autofree char *text = g_strndup((const char *)data, length);
    g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
    Raspi4BootFilterState filters;

    raspi4_boot_filters_reset(&filters);

    for (char **linep = lines; *linep; linep++) {
        char *comment = strchr(*linep, '#');
        char *line;
        char *separator;
        uint64_t value;
        bool value_has_horizontal_space;

        if (comment) {
            *comment = '\0';
        }
        line = g_strstrip(*linep);
        if (!line[0]) {
            continue;
        }
        if (line[0] == '[') {
            raspi4_boot_filter_apply(s, &filters, line);
            continue;
        }
        if (!raspi4_boot_filters_active(&filters)) {
            continue;
        }
        separator = strchr(line, '=');
        if (!separator) {
            continue;
        }
        *separator = '\0';
        line = g_strstrip(line);
        value_has_horizontal_space =
            strpbrk(separator + 1, " \t") != NULL;
        separator = g_strstrip(separator + 1);
        if (!g_ascii_strcasecmp(line, "BOOT_ORDER")) {
            if (qemu_strtou64(separator, NULL, 0, &value) < 0 ||
                value > UINT32_MAX) {
                return false;
            }
            config->boot_order = value;
        } else if (!g_ascii_strcasecmp(line, "SIGNED_BOOT")) {
            if (qemu_strtou64(separator, NULL, 0, &value) < 0 ||
                value > 1) {
                return false;
            }
            config->signed_boot = value;
        } else if (!g_ascii_strcasecmp(line, "BOOT_UART")) {
            if (qemu_strtou64(separator, NULL, 0, &value) < 0 ||
                value > 1) {
                return false;
            }
            config->boot_uart = value;
        } else if (!g_ascii_strcasecmp(line, "WAKE_ON_GPIO") ||
                   !g_ascii_strcasecmp(line, "POWER_OFF_ON_HALT")) {
            bool *field = !g_ascii_strcasecmp(line, "WAKE_ON_GPIO") ?
                &config->wake_on_gpio : &config->power_off_on_halt;

            if (qemu_strtou64(separator, NULL, 0, &value) < 0 ||
                value > 1) {
                return false;
            }
            *field = value;
        } else if (!g_ascii_strcasecmp(line, "VL805")) {
            if (qemu_strtou64(separator, NULL, 0, &value) < 0 ||
                value > 1) {
                return false;
            }
            config->vl805 = value;
        } else if (!g_ascii_strcasecmp(line, "BOOTVAR0")) {
            if (qemu_strtou64(separator, NULL, 0, &value) < 0 ||
                value > UINT32_MAX) {
                return false;
            }
            config->bootvar0 = value;
        } else if (!g_ascii_strcasecmp(line, "PARTITION")) {
            if (qemu_strtou64(separator, NULL, 0, &value) < 0 ||
                value > 31) {
                return false;
            }
            config->partition = value;
        } else if (!g_ascii_strcasecmp(line, "PARTITION_WALK")) {
            if (qemu_strtou64(separator, NULL, 0, &value) < 0 ||
                value > 1) {
                return false;
            }
            config->partition_walk = value;
        } else if (!g_ascii_strcasecmp(line, "MAX_RESTARTS")) {
            if (!raspi4_parse_nonnegative_or_infinite(
                    separator, &config->max_restarts)) {
                return false;
            }
        } else if (!g_ascii_strcasecmp(line, "REBOOT_ON_FATAL_ERROR")) {
            if (qemu_strtou64(separator, NULL, 0, &value) < 0 ||
                value > 1) {
                return false;
            }
            config->reboot_on_fatal_error = value;
        } else if (!g_ascii_strcasecmp(line, "SD_BOOT_MAX_RETRIES")) {
            if (!raspi4_parse_nonnegative_or_infinite(
                    separator, &config->sd_boot_max_retries)) {
                return false;
            }
        } else if (!g_ascii_strcasecmp(line, "SD_OVERCURRENT_CHECK")) {
            if (qemu_strtou64(separator, NULL, 0, &value) < 0 ||
                value > 1) {
                return false;
            }
            config->sd_overcurrent_check = value;
        } else if (!g_ascii_strcasecmp(line, "SD_QUIRKS")) {
            /*
             * BCM2711 currently defines bit zero only.  Reserved bits must
             * remain zero so a newer firmware policy cannot be mistaken for
             * behavior this model does not implement.
             */
            if (qemu_strtou64(separator, NULL, 0, &value) < 0 ||
                value > RASPI4_SD_QUIRK_DISABLE_HIGH_SPEED) {
                return false;
            }
            config->sd_quirks = value;
        } else if (!g_ascii_strcasecmp(line,
                                       "USB_MSD_DISCOVER_TIMEOUT")) {
            if (!raspi4_parse_timeout(
                    separator, RASPI4_USB_MSD_DISCOVER_TIMEOUT_MIN,
                    &config->usb_msd_discover_timeout)) {
                return false;
            }
        } else if (!g_ascii_strcasecmp(line, "USB_MSD_LUN_TIMEOUT")) {
            if (!raspi4_parse_timeout(
                    separator, RASPI4_USB_MSD_LUN_TIMEOUT_MIN,
                    &config->usb_msd_lun_timeout)) {
                return false;
            }
        } else if (!g_ascii_strcasecmp(line, "USB_MSD_STARTUP_DELAY")) {
            if (!raspi4_parse_bounded_timeout(
                    separator, 0, RASPI4_USB_MSD_STARTUP_DELAY_MAX,
                    &config->usb_msd_startup_delay)) {
                return false;
            }
        } else if (!g_ascii_strcasecmp(line, "USB_MSD_PWR_OFF_TIME")) {
            if (!raspi4_parse_bounded_timeout(
                    separator, 0, RASPI4_USB_MSD_PWR_OFF_TIME_MAX,
                    &config->usb_msd_power_off_time)) {
                return false;
            }
        } else if (!g_ascii_strcasecmp(
                       line, "USB_MSD_EXCLUDE_VID_PID")) {
            if (value_has_horizontal_space ||
                !raspi4_parse_usb_msd_exclude_vid_pid(
                    separator, config->usb_msd_exclude_vid_pid,
                    &config->usb_msd_exclude_count)) {
                return false;
            }
        } else if (!g_ascii_strcasecmp(line, "BOOT_WATCHDOG_TIMEOUT")) {
            if (!raspi4_parse_bounded_timeout(
                    separator, 0, UINT32_MAX,
                    &config->boot_watchdog_timeout)) {
                return false;
            }
        } else if (!g_ascii_strcasecmp(line, "BOOT_WATCHDOG_PARTITION")) {
            if (qemu_strtou64(separator, NULL, 0, &value) < 0 ||
                value > RASPI4_BOOT_WATCHDOG_PARTITION_MAX) {
                return false;
            }
            config->boot_watchdog_partition = value;
        } else if (!g_ascii_strcasecmp(line, "NET_BOOT_MAX_RETRIES")) {
            if (!raspi4_parse_nonnegative_or_infinite(
                    separator, &config->net_boot_max_retries)) {
                return false;
            }
        } else if (!g_ascii_strcasecmp(line, "DHCP_TIMEOUT")) {
            if (!raspi4_parse_timeout(separator, RASPI4_DHCP_TIMEOUT_MIN,
                                      &config->dhcp_timeout)) {
                return false;
            }
        } else if (!g_ascii_strcasecmp(line, "DHCP_REQ_TIMEOUT")) {
            if (!raspi4_parse_timeout(separator,
                                      RASPI4_DHCP_REQ_TIMEOUT_MIN,
                                      &config->dhcp_req_timeout)) {
                return false;
            }
        } else if (!g_ascii_strcasecmp(line, "DHCP_OPTION97")) {
            if (qemu_strtou64(separator, NULL, 0, &value) < 0 ||
                value > UINT32_MAX) {
                return false;
            }
            config->dhcp_option97 = value;
        } else if (!g_ascii_strcasecmp(line, "PXE_OPTION43")) {
            size_t option_length = strlen(separator);

            if (!option_length ||
                option_length > RASPI4_PXE_OPTION43_MAX) {
                return false;
            }
            for (size_t i = 0; i < option_length; i++) {
                if (separator[i] < 0x20 || separator[i] > 0x7e) {
                    return false;
                }
            }
            pstrcpy(config->pxe_option43,
                    sizeof(config->pxe_option43), separator);
        } else if (!g_ascii_strcasecmp(line, "NETCONSOLE")) {
            if (!raspi4_parse_netconsole(separator, config)) {
                return false;
            }
        } else if (!g_ascii_strcasecmp(line, "MAC_ADDRESS")) {
            if (!separator[0]) {
                config->mac_address_source = RASPI4_MAC_CONFIGURED;
            } else if (!raspi4_parse_mac_address(
                           separator, &config->mac_address)) {
                return false;
            } else {
                config->mac_address_source = RASPI4_MAC_EXPLICIT;
            }
        } else if (!g_ascii_strcasecmp(line, "MAC_ADDRESS_OTP")) {
            if (!separator[0]) {
                config->mac_address_source = RASPI4_MAC_CONFIGURED;
            } else if (!raspi4_parse_mac_address_otp(
                           s, separator, &config->mac_address)) {
                return false;
            } else {
                config->mac_address_source = RASPI4_MAC_CUSTOMER_OTP;
            }
        } else if (!g_ascii_strcasecmp(line, "TFTP_IP")) {
            if (!separator[0]) {
                config->tftp_ip_set = false;
                config->tftp_ip = 0;
            } else if (!raspi4_parse_ipv4(separator, &config->tftp_ip)) {
                return false;
            } else {
                config->tftp_ip_set = true;
            }
        } else if (!g_ascii_strcasecmp(line, "TFTP_PREFIX")) {
            if (qemu_strtou64(separator, NULL, 0, &value) < 0 ||
                value > 2) {
                return false;
            }
            config->tftp_prefix = value;
        } else if (!g_ascii_strcasecmp(line, "TFTP_PREFIX_STR")) {
            size_t prefix_length = strlen(separator);

            if (prefix_length > RASPI4_TFTP_PREFIX_STR_MAX) {
                return false;
            }
            for (size_t i = 0; i < prefix_length; i++) {
                unsigned char c = separator[i];

                if (c < 0x20 || c >= 0x7f) {
                    return false;
                }
            }
            pstrcpy(config->tftp_prefix_str,
                    sizeof(config->tftp_prefix_str), separator);
        } else if (!g_ascii_strcasecmp(line, "CLIENT_IP")) {
            if (!separator[0]) {
                config->client_ip_set = false;
                config->client_ip = 0;
            } else if (!raspi4_parse_ipv4(separator, &config->client_ip)) {
                return false;
            } else {
                config->client_ip_set = true;
            }
        } else if (!g_ascii_strcasecmp(line, "SUBNET")) {
            if (!separator[0]) {
                config->subnet_set = false;
                config->subnet = 0;
            } else if (!raspi4_parse_netmask(separator, &config->subnet)) {
                return false;
            } else {
                config->subnet_set = true;
            }
        } else if (!g_ascii_strcasecmp(line, "GATEWAY")) {
            if (!separator[0]) {
                config->gateway_set = false;
                config->gateway = 0;
            } else if (!raspi4_parse_ipv4(separator, &config->gateway)) {
                return false;
            } else {
                config->gateway_set = true;
            }
        } else if (!g_ascii_strcasecmp(line, "TFTP_FILE_TIMEOUT")) {
            if (!raspi4_parse_timeout(separator,
                                      RASPI4_TFTP_FILE_TIMEOUT_MIN,
                                      &config->tftp_file_timeout)) {
                return false;
            }
        } else if (!g_ascii_strcasecmp(line, "HTTP_HOST")) {
            if (!separator[0]) {
                config->http_host_set = false;
                config->http_host[0] = 0;
            } else if (!raspi4_parse_http_host(
                           separator, config->http_host,
                           sizeof(config->http_host))) {
                /*
                 * The Pi bootloader ignores an invalid HTTP_HOST and uses
                 * the default host policy.  Do not reject the complete
                 * EEPROM configuration for this field.
                 */
                config->http_host_set = false;
                config->http_host[0] = 0;
            } else {
                config->http_host_set = true;
            }
        } else if (!g_ascii_strcasecmp(line, "HTTP_PORT")) {
            if (qemu_strtou64(separator, NULL, 0, &value) < 0 || !value ||
                value > UINT16_MAX) {
                return false;
            }
            config->http_port = value;
        } else if (!g_ascii_strcasecmp(line, "HTTP_PATH")) {
            if (!raspi4_parse_http_path(separator, config->http_path,
                                        sizeof(config->http_path))) {
                return false;
            }
        } else if (!g_ascii_strcasecmp(line, "HTTP_CACERT_HASH")) {
            if (!separator[0]) {
                config->http_cacert_hash_set = false;
                config->http_cacert_hash[0] = 0;
            } else if (!raspi4_parse_sha256(
                           separator, config->http_cacert_hash)) {
                return false;
            } else {
                config->http_cacert_hash_set = true;
            }
        } else if (!g_ascii_strcasecmp(line, "DISABLE_HDMI")) {
            if (qemu_strtou64(separator, NULL, 0, &value) < 0 ||
                value > UINT32_MAX) {
                return false;
            }
            config->disable_hdmi = value == 1;
        } else if (!g_ascii_strcasecmp(line, "HDMI_DELAY")) {
            if (qemu_strtou64(separator, NULL, 0, &value) < 0 ||
                value > UINT32_MAX) {
                return false;
            }
            config->hdmi_delay = value;
        } else if (!g_ascii_strcasecmp(line, "NET_INSTALL_ENABLED") ||
                   !g_ascii_strcasecmp(
                       line, "NET_INSTALL_AT_POWER_ON")) {
            bool *setting =
                !g_ascii_strcasecmp(line, "NET_INSTALL_ENABLED") ?
                    &config->net_install_enabled :
                    &config->net_install_at_power_on;

            if (qemu_strtou64(separator, NULL, 0, &value) < 0 ||
                value > 1) {
                return false;
            }
            *setting = value;
        } else if (!g_ascii_strcasecmp(
                       line, "NET_INSTALL_KEYBOARD_WAIT")) {
            if (qemu_strtou64(separator, NULL, 0, &value) < 0 ||
                value > UINT32_MAX) {
                return false;
            }
            config->net_install_keyboard_wait = value;
        } else if (!g_ascii_strcasecmp(line, "ENABLE_SELF_UPDATE") ||
                   !g_ascii_strcasecmp(line, "FREEZE_VERSION")) {
            bool *setting =
                !g_ascii_strcasecmp(line, "ENABLE_SELF_UPDATE") ?
                    &config->enable_self_update :
                    &config->freeze_version;

            if (qemu_strtou64(separator, NULL, 0, &value) < 0 ||
                value > 1) {
                return false;
            }
            *setting = value;
        }
    }

    return true;
}

uint32_t raspi4_xxh32_short(const uint8_t *data, size_t size)
{
    const uint32_t prime1 = UINT32_C(0x9e3779b1);
    const uint32_t prime2 = UINT32_C(0x85ebca77);
    const uint32_t prime3 = UINT32_C(0xc2b2ae3d);
    const uint32_t prime4 = UINT32_C(0x27d4eb2f);
    const uint32_t prime5 = UINT32_C(0x165667b1);
    uint32_t hash = prime5 + size;

    g_assert(size < 16);
    while (size >= sizeof(uint32_t)) {
        hash += ldl_le_p(data) * prime3;
        hash = (hash << 17 | hash >> 15) * prime4;
        data += sizeof(uint32_t);
        size -= sizeof(uint32_t);
    }
    while (size) {
        hash += *data++ * prime5;
        hash = (hash << 11 | hash >> 21) * prime1;
        size--;
    }
    hash ^= hash >> 15;
    hash *= prime2;
    hash ^= hash >> 13;
    hash *= prime3;
    return hash ^ hash >> 16;
}

bool raspi4_buffer_contains(const uint8_t *haystack,
                                   size_t haystack_size,
                                   const uint8_t *needle,
                                   size_t needle_size)
{
    if (needle_size > haystack_size) {
        return false;
    }
    for (size_t i = 0; i <= haystack_size - needle_size; i++) {
        if (!memcmp(haystack + i, needle, needle_size)) {
            return true;
        }
    }
    return false;
}

static void raspi4_read_eeprom_build_identity(Raspi4bMachineState *s,
                                              const uint8_t *image,
                                              size_t image_size)
{
    static const char timestamp_prefix[] = "BUILD_TIMESTAMP=";
    static const char version_prefix[] = "VERSION:";
    const uint8_t *value;

    value = raspi4_eeprom_find_string(image, image_size, timestamp_prefix);
    if (value) {
        const uint8_t *end = memchr(value, 0, image + image_size - value);

        if (end && end > value && end - value <= 10) {
            g_autofree char *text =
                g_strndup((const char *)value, end - value);
            uint64_t timestamp;

            if (qemu_strtou64(text, NULL, 10, &timestamp) == 0 &&
                timestamp <= UINT32_MAX) {
                s->eeprom_build_timestamp = timestamp;
                s->eeprom_build_timestamp_valid = true;
            }
        }
    }

    value = raspi4_eeprom_find_string(image, image_size, version_prefix);
    if (value) {
        const uint8_t *end = memchr(value, 0, image + image_size - value);
        size_t length = end ? end - value : 0;
        bool valid = length >= 8 &&
                     length <= RASPI_FIRMWARE_BOOTLOADER_VERSION_MAX;

        for (size_t index = 0; valid && index < length; index++) {
            valid = g_ascii_isxdigit(value[index]);
        }
        if (valid) {
            memcpy(s->eeprom_version, value, length);
            s->eeprom_version[length] = 0;
        }
    }
    /*
     * Public hardware observations bound these release families exactly:
     * 2020-12-11 reports 0x1f and 2021-07-06 through the current BCM2711
     * release report 0x7f.  Unknown earlier/intermediate images remain
     * absent rather than receiving a capability value inferred from QEMU.
     */
    if (s->eeprom_build_timestamp_valid) {
        if (s->eeprom_build_timestamp >=
            RASPI4_CAPABILITIES_2021_TIMESTAMP) {
            s->eeprom_capabilities = RASPI4_CAPABILITIES_2021;
            s->eeprom_capabilities_valid = true;
        } else if (s->eeprom_build_timestamp ==
                   RASPI4_CAPABILITIES_2020_TIMESTAMP) {
            s->eeprom_capabilities = RASPI4_CAPABILITIES_2020;
            s->eeprom_capabilities_valid = true;
        }
    }
}

static bool raspi4_read_eeprom_boot_config(Raspi4bMachineState *s,
                                           Raspi4BootConfig *config,
                                           RaspiSecureResult *secure_result)
{
    g_autofree uint8_t *image = NULL;
    int64_t image_size;
    Raspi4EepromFiles files;

    if (s->otp_secure_boot) {
        *secure_result = RASPI_SECURE_SIGNATURE_FORMAT;
    }

    if (!s->eeprom || !blk_is_inserted(s->eeprom)) {
        return false;
    }
    image_size = blk_getlength(s->eeprom);
    if (image_size != RASPI4_EEPROM_SIZE) {
        return false;
    }
    image = g_malloc(image_size);
    if (blk_pread(s->eeprom, 0, image_size, image, 0) < 0) {
        return false;
    }

    if (!raspi4_eeprom_files_parse(image, image_size, &files)) {
        return false;
    }
    raspi4_read_eeprom_build_identity(s, image, image_size);
    if (s->otp_secure_boot) {
        *secure_result = raspi4_secure_eeprom_verify(s, &files);
        if (*secure_result != RASPI_SECURE_OK) {
            return false;
        }
    }
    if (files.bootconf &&
        !raspi4_parse_boot_config(
            s, files.bootconf, files.bootconf_size, config)) {
        return false;
    }
    if (files.bootconf) {
        s->eeprom_bootconf = g_memdup2(
            files.bootconf, files.bootconf_size);
        s->eeprom_bootconf_size = files.bootconf_size;
    }
    if (files.public_key_size == sizeof(s->eeprom_public_key)) {
        memcpy(s->eeprom_public_key, files.public_key,
               sizeof(s->eeprom_public_key));
        s->eeprom_public_key_size = files.public_key_size;
    }
    if (files.bootconf) {
        const uint8_t *cursor = files.bootconf;
        const uint8_t *end = files.bootconf + files.bootconf_size;

        while (cursor < end) {
            const uint8_t *newline = memchr(cursor, '\n', end - cursor);
            const uint8_t *line_end = newline ? newline : end;
            g_autofree char *line =
                g_strndup((const char *)cursor, line_end - cursor);
            char *comment = strchr(line, '#');

            if (comment) {
                *comment = 0;
            }
            if (!g_ascii_strcasecmp(g_strstrip(line), "[config.txt]")) {
                const uint8_t *append = newline ? newline + 1 : end;
                size_t append_size = end - append;

                memcpy(s->eeprom_config_append, append, append_size);
                s->eeprom_config_append_size = append_size;
                break;
            }
            cursor = newline ? newline + 1 : end;
        }
    }
    return true;
}

void raspi4_set_firmware_status(Raspi4bMachineState *s,
                                       const char *status)
{
    g_free(s->firmware_status);
    s->firmware_status = g_strdup(status);
}

static void raspi4_set_handoff_status(Raspi4bMachineState *s,
                                      const char *status)
{
    g_free(s->handoff_status);
    s->handoff_status = g_strdup(status);
}

static bool raspi4_edid_name(const uint8_t *edid, size_t length,
                             uint8_t name[RASPI_FIRMWARE_EDID_NAME_MAX])
{
    static const uint8_t header[8] = {
        0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00,
    };
    uint16_t vendor;
    char manufacturer[4];
    const uint8_t *product = NULL;
    size_t product_length = 13;
    unsigned int blocks;

    if (length < 128 || length % 128 ||
        memcmp(edid, header, sizeof(header))) {
        return false;
    }
    blocks = 1 + edid[126];
    if (length != blocks * 128) {
        return false;
    }
    for (unsigned int block = 0; block < blocks; block++) {
        uint8_t checksum = 0;

        for (unsigned int byte = 0; byte < 128; byte++) {
            checksum += edid[block * 128 + byte];
        }
        if (checksum) {
            return false;
        }
    }

    vendor = lduw_be_p(edid + 8);
    for (unsigned int index = 0; index < 3; index++) {
        unsigned int shift = 10 - index * 5;
        unsigned int letter = (vendor >> shift) & 0x1f;

        if (!letter || letter > 26) {
            return false;
        }
        manufacturer[index] = 'A' + letter - 1;
    }
    manufacturer[3] = 0;

    for (unsigned int offset = 54; offset <= 108; offset += 18) {
        if (!edid[offset] && !edid[offset + 1] && !edid[offset + 2] &&
            edid[offset + 3] == 0xfc && !edid[offset + 4]) {
            product = edid + offset + 5;
            break;
        }
    }
    if (!product) {
        return false;
    }
    while (product_length &&
           (product[product_length - 1] == 0 ||
            g_ascii_isspace(product[product_length - 1]))) {
        product_length--;
    }
    if (!product_length ||
        3 + 1 + product_length >= RASPI_FIRMWARE_EDID_NAME_MAX) {
        return false;
    }
    memcpy(name, manufacturer, 3);
    name[3] = '-';
    for (unsigned int index = 0; index < product_length; index++) {
        if (product[index] < 0x20 || product[index] > 0x7e) {
            return false;
        }
        name[4 + index] = g_ascii_isspace(product[index]) ?
                          '_' : product[index];
    }
    name[4 + product_length] = 0;
    return true;
}

void raspi4_sample_edids(Raspi4bMachineState *s)
{
    for (unsigned int port = 0; port < RASPI_FIRMWARE_MAX_EDIDS; port++) {
        g_autofree char *contents = NULL;
        gsize length = 0;

        s->hdmi_edid_name[port][0] = 0;
        s->hdmi_edid_status[port] = RASPI4_EDID_DISCONNECTED;
        bcm2835_property_set_edid(raspi4_property(s), port, NULL, 0);
        if (!s->hdmi_edid_file[port]) {
            continue;
        }
        if (!g_file_get_contents(s->hdmi_edid_file[port], &contents,
                                 &length, NULL)) {
            s->hdmi_edid_status[port] = RASPI4_EDID_INVALID;
            continue;
        }
        if (!length) {
            continue;
        }
        if (raspi4_edid_name((const uint8_t *)contents, length,
                             s->hdmi_edid_name[port])) {
            s->hdmi_edid_status[port] = RASPI4_EDID_VALID;
            bcm2835_property_set_edid(
                raspi4_property(s), port, (const uint8_t *)contents, length);
        } else {
            s->hdmi_edid_status[port] = RASPI4_EDID_INVALID;
        }
    }
}

void raspi4_set_firmware_filter_inputs(
    Raspi4bMachineState *s, RaspiFirmwareConfig *config)
{
    config->installed_mem_mb = MACHINE(s)->ram_size / MiB;
    config->bootvar0 = s->bootvar0;
    config->boot_mode = s->selected_boot_mode;
    config->bootloader_build_timestamp_valid =
        s->eeprom_build_timestamp_valid;
    config->bootloader_build_timestamp = s->eeprom_build_timestamp;
    config->bootloader_update_timestamp_valid =
        s->eeprom_update_timestamp_valid;
    config->bootloader_update_timestamp = s->eeprom_update_timestamp;
    config->bootloader_capabilities_valid = s->eeprom_capabilities_valid;
    config->bootloader_capabilities = s->eeprom_capabilities;
    config->bootloader_usb_valid = s->usb_boot_identity_valid;
    config->bootloader_usb_version = s->usb_boot_version;
    config->bootloader_usb_route_string = s->usb_boot_route_string;
    config->bootloader_usb_root_hub_port =
        s->usb_boot_root_hub_port;
    config->bootloader_usb_lun = s->usb_boot_selected_lun;
    pstrcpy(config->bootloader_version,
            sizeof(config->bootloader_version),
            (const char *)s->eeprom_version);
    config->bootloader_signed = s->bootloader_signed;
    config->partition = s->boot_partition;
    config->boot_partition = s->selected_boot_partition;
    config->reset_status = s->reset_status;
    config->board_revision = s->board_revision;
    config->board_revision_ext = bcm2835_otp_get_row(
        raspi4_otp(s), BCM2711_OTP_BOARD_REVISION_EXT_ROW);
    config->min_boot_version = s->min_boot_version;
    config->tryboot = s->tryboot;
    config->bootloader_config_append = s->eeprom_config_append;
    config->bootloader_config_append_size =
        s->eeprom_config_append_size;
    config->board_type = (s->board_revision >> 4) & UINT8_MAX;
    config->serial = bcm2835_otp_get_row(
        raspi4_otp(s), BCM2711_OTP_SERIAL_ROW);
    for (unsigned int index = 0; index < 8; index++) {
        config->customer_otp[index] = bcm2835_otp_get_row(
            raspi4_otp(s), BCM2835_OTP_CUSTOMER_OTP + index);
    }
    for (unsigned int port = 0; port < RASPI_FIRMWARE_MAX_EDIDS; port++) {
        if (s->hdmi_edid_status[port] == RASPI4_EDID_VALID) {
            pstrcpy(config->edid_names[port],
                    sizeof(config->edid_names[port]),
                    (const char *)s->hdmi_edid_name[port]);
            config->edid_valid_mask |= BIT(port);
        }
    }
    for (unsigned int pin = 0; pin < BCM2838_GPIO_NUM; pin++) {
        int level = bcm2838_gpio_get_external_input(
            &s->soc.peripherals.gpio, pin);

        if (level >= 0) {
            config->gpio_known_mask |= BIT_ULL(pin);
            if (level) {
                config->gpio_level_mask |= BIT_ULL(pin);
            }
        }
    }
}

void raspi4_clear_firmware_observation(Raspi4bMachineState *s)
{
    if (s->firmware_config_initialized) {
        raspi_firmware_config_clear(&s->firmware_config);
    }
    raspi_firmware_config_init(&s->firmware_config, s->cm4);
    s->firmware_config.bootvar0 = s->bootvar0;
    s->firmware_config.partition = s->boot_partition;
    s->firmware_config.boot_partition = s->selected_boot_partition;
    s->firmware_config.reset_status = s->reset_status;
    s->firmware_config.tryboot = s->tryboot;
    raspi4_set_firmware_filter_inputs(s, &s->firmware_config);
    s->firmware_config_initialized = true;
    memset(&s->firmware_manifest, 0, sizeof(s->firmware_manifest));
    s->firmware_size = 0;
    s->secure_image_size = 0;
    s->secure_signature_size = 0;
    s->fixup_size = 0;
    s->kernel_size = 0;
    s->device_tree_size = 0;
    s->cmdline_size = 0;
    s->initramfs_size = 0;
    g_clear_pointer(&s->firmware_file, g_free);
    g_clear_pointer(&s->secure_image_sha256, g_free);
    g_clear_pointer(&s->secure_signature_sha256, g_free);
    g_clear_pointer(&s->firmware_sha256, g_free);
    g_clear_pointer(&s->fixup_sha256, g_free);
    g_clear_pointer(&s->kernel_sha256, g_free);
    g_clear_pointer(&s->device_tree_sha256, g_free);
    g_clear_pointer(&s->cmdline_sha256, g_free);
    g_clear_pointer(&s->initramfs_sha256, g_free);
    raspi_arm_handoff_clear(&s->handoff);
    raspi_overlay_state_clear(&s->overlay_state);
    s->handoff_core_mask = 0;
    raspi4_set_firmware_status(s, "none");
    raspi4_set_handoff_status(s, "none");
}

void raspi4_uart0_write(Raspi4bMachineState *s, const char *text,
                               uint64_t *bytes, uint32_t *lines)
{
    MemoryRegion *uart = &s->soc.peripherals.parent_obj.uart0.iomem;

    for (const uint8_t *cursor = (const uint8_t *)text;
         *cursor; cursor++) {
        memory_region_dispatch_write(uart, RASPI4_UART0_DR, *cursor,
                                     MO_32 | MO_LE,
                                     MEMTXATTRS_UNSPECIFIED);
        (*bytes)++;
        if (*cursor == '\n') {
            (*lines)++;
        }
    }
}

void raspi4_uart0_configure(Raspi4bMachineState *s)
{
    MemoryRegion *gpio = &s->soc.peripherals.gpio.iomem;
    MemoryRegion *uart = &s->soc.peripherals.parent_obj.uart0.iomem;
    uint64_t value;
    uint32_t gpfsel1;

    memory_region_dispatch_read(gpio, 4, &value, MO_32 | MO_LE,
                                MEMTXATTRS_UNSPECIFIED);
    gpfsel1 = value;
    gpfsel1 &= ~((7U << 12) | (7U << 15));
    gpfsel1 |= (4U << 12) | (4U << 15);
    memory_region_dispatch_write(gpio, 4, gpfsel1, MO_32 | MO_LE,
                                 MEMTXATTRS_UNSPECIFIED);
    memory_region_dispatch_write(uart, RASPI4_UART0_IBRD, 26,
                                 MO_32 | MO_LE, MEMTXATTRS_UNSPECIFIED);
    memory_region_dispatch_write(uart, RASPI4_UART0_FBRD, 3,
                                 MO_32 | MO_LE, MEMTXATTRS_UNSPECIFIED);
    memory_region_dispatch_write(uart, RASPI4_UART0_LCRH, 0x70,
                                 MO_32 | MO_LE, MEMTXATTRS_UNSPECIFIED);
    memory_region_dispatch_write(uart, RASPI4_UART0_CR, 0x301,
                                 MO_32 | MO_LE, MEMTXATTRS_UNSPECIFIED);
}

static void raspi4_firmware_uart_write(Raspi4bMachineState *s,
                                       const char *text)
{
    if (s->firmware_uart_enabled) {
        raspi4_uart0_write(s, text, &s->firmware_uart_bytes,
                           &s->firmware_uart_lines);
    }
}

void raspi4_firmware_uart_begin(Raspi4bMachineState *s)
{
    raspi4_boot_uart_end(s);
    s->firmware_uart_enabled = s->firmware_config.uart_2ndstage;
    if (!s->firmware_uart_enabled || s->firmware_uart_started) {
        return;
    }

    raspi4_uart0_configure(s);
    s->firmware_uart_started = true;
    raspi4_firmware_uart_write(
        s, "RPI4: uart_2ndstage enabled\r\n");
}

void raspi4_firmware_uart_manifest(Raspi4bMachineState *s,
                                          const char *source)
{
    g_autofree char *line = NULL;

    if (!s->firmware_uart_enabled) {
        return;
    }
    line = g_strdup_printf(
        "RPI4: source=%s start=%s fixup=%s kernel=%s\r\n",
        source, s->firmware_config.start_file,
        s->firmware_config.fixup_file, s->firmware_config.kernel_file);
    raspi4_firmware_uart_write(s, line);
}

void raspi4_apply_firmware_gpu_mem(Raspi4bMachineState *s)
{
    BCM2835FBState *fb = &s->soc.peripherals.parent_obj.fb;
    uint32_t old_initial_base = fb->initial_config.base;
    uint32_t framebuffer_offset = old_initial_base - fb->vcram_base;
    bool config_at_initial = fb->config.base == old_initial_base;

    fb->vcram_size = s->firmware_config.gpu_mem_effective_mb * MiB;
    fb->vcram_base = 1 * GiB - fb->vcram_size;
    fb->initial_config.base = fb->vcram_base + framebuffer_offset;
    if (config_at_initial) {
        fb->config.base = fb->initial_config.base;
    }
}

uint8_t *raspi4_read_firmware_artifact(RaspiFatVolume *volume,
                                              const RaspiFatFile *file,
                                              size_t maximum, uint32_t *size,
                                              char **sha256)
{
    uint8_t *data;
    size_t length;

    data = raspi_fat_read_file(volume, file, maximum, &length, NULL);
    if (!data || length != file->size) {
        g_free(data);
        return NULL;
    }
    *size = length;
    *sha256 = g_compute_checksum_for_data(G_CHECKSUM_SHA256, data, length);
    return data;
}

uint8_t *raspi4_read_hat_eeprom(Raspi4bMachineState *s,
                                       size_t *size)
{
    int64_t length;
    uint8_t *data;

    if (!s->hat_eeprom) {
        *size = 0;
        return NULL;
    }
    length = blk_getlength(s->hat_eeprom);
    if (length < 12 || length > RASPI_HAT_EEPROM_MAX_SIZE) {
        return NULL;
    }
    data = g_malloc(length);
    if (blk_pread(s->hat_eeprom, 0, length, data, 0) < 0) {
        g_free(data);
        return NULL;
    }
    *size = length;
    return data;
}

uint8_t *raspi4_read_initramfs(
    RaspiFatVolume *volume, const RaspiFirmwareManifest *manifest,
    uint32_t *size, char **sha256)
{
    g_autofree uint8_t *data = NULL;
    size_t total = 0;

    for (unsigned int i = 0; i < manifest->initramfs_count; i++) {
        const RaspiFatFile *file = &manifest->initramfs[i];
        g_autofree uint8_t *part = NULL;
        size_t length;

        if (file->size > RASPI4_INITRAMFS_MAX_SIZE - total) {
            return NULL;
        }
        part = raspi_fat_read_file(volume, file,
                                   RASPI4_INITRAMFS_MAX_SIZE - total,
                                   &length, NULL);
        if (!part || length != file->size) {
            return NULL;
        }
        data = g_realloc(data, total + length);
        memcpy(data + total, part, length);
        total += length;
    }
    if (!total) {
        return NULL;
    }
    *size = total;
    *sha256 = g_compute_checksum_for_data(G_CHECKSUM_SHA256, data, total);
    return g_steal_pointer(&data);
}

Raspi4BootAttemptResult raspi4_finish_firmware(
    Raspi4bMachineState *s, RaspiFatVolume *volume, const char *media,
    const uint8_t *kernel, const uint8_t *device_tree,
    const uint8_t *cmdline, const uint8_t *initramfs,
    RaspiFirmwareReadPath *read_path, void *read_opaque)
{
    RaspiBaseMachineState *s_base = RASPI_BASE_MACHINE(s);
    g_autofree uint8_t *effective_tree = NULL;
    g_autofree uint8_t *hat_eeprom = NULL;
    RaspiHandoffResult handoff_result;
    RaspiOverlayResult overlay_result;
    Error *handoff_error = NULL;
    Error *overlay_error = NULL;
    size_t effective_tree_size;
    size_t hat_eeprom_size = 0;
    AddressSpace *as;

    trace_raspi4b_boot_event("firmware", "firmware.start", media,
                             "success", s->firmware_size);
    trace_raspi4b_boot_event("firmware", "firmware.fixup", media,
                             "success", s->fixup_size);
    trace_raspi4b_boot_event("firmware", "firmware.kernel", media,
                             "success", s->kernel_size);
    trace_raspi4b_boot_event("firmware", "firmware.device-tree", media,
                             "success", s->device_tree_size);
    if (s->initramfs_size) {
        trace_raspi4b_boot_event("firmware", "firmware.initramfs", media,
                                 "success", s->initramfs_size);
    }
    g_free(s->firmware_file);
    s->firmware_file = g_strdup(s->firmware_config.start_file);
    raspi4_set_firmware_status(s, "config-ready");
    trace_raspi4b_boot_event("firmware", "firmware.config", media,
                             "success", s->firmware_config.parsed_lines);
    if (!device_tree) {
        raspi4_set_handoff_status(s, "device-tree-required");
        return RASPI4_BOOT_ATTEMPT_FAILED;
    }
    hat_eeprom = raspi4_read_hat_eeprom(s, &hat_eeprom_size);
    if (s->hat_eeprom && !hat_eeprom) {
        raspi4_set_handoff_status(s, "overlay-invalid");
        return RASPI4_BOOT_ATTEMPT_FAILED;
    }
    if (s->firmware_config.dt_commands->len || hat_eeprom ||
        (!fdt_check_header(device_tree) &&
         fdt_path_offset(device_tree, "/hat") >= 0)) {
        overlay_result = read_path ?
            raspi_overlay_apply_config_with_reader(
                &s->firmware_config, hat_eeprom, hat_eeprom_size,
                device_tree, s->device_tree_size,
                read_path, read_opaque, &effective_tree,
                &effective_tree_size, &s->overlay_state, &overlay_error) :
            raspi_overlay_apply_config(
                volume, &s->firmware_config, hat_eeprom, hat_eeprom_size,
                device_tree,
                s->device_tree_size, &effective_tree,
                &effective_tree_size, &s->overlay_state, &overlay_error);
    } else {
        effective_tree = g_memdup2(device_tree, s->device_tree_size);
        effective_tree_size = s->device_tree_size;
        overlay_result = RASPI_OVERLAY_READY;
    }
    if (overlay_result != RASPI_OVERLAY_READY) {
        if (overlay_error) {
            error_report_err(overlay_error);
        }
        switch (overlay_result) {
        case RASPI_OVERLAY_ERROR_LIMIT:
            raspi4_set_handoff_status(s, "overlay-limit");
            break;
        case RASPI_OVERLAY_ERROR_MISSING:
            raspi4_set_handoff_status(s, "overlay-missing");
            break;
        case RASPI_OVERLAY_ERROR_INVALID:
            raspi4_set_handoff_status(s, "overlay-invalid");
            break;
        case RASPI_OVERLAY_ERROR_PARAMETER:
            raspi4_set_handoff_status(s, "overlay-parameter-invalid");
            break;
        case RASPI_OVERLAY_ERROR_APPLY:
            raspi4_set_handoff_status(s, "overlay-apply-error");
            break;
        default:
            g_assert_not_reached();
        }
        return RASPI4_BOOT_ATTEMPT_FAILED;
    }
    bcm2838_gpio_apply_hat_map(
        &s->soc.peripherals.gpio,
        s->overlay_state.hat_gpio_valid ?
            s->overlay_state.hat_gpio_map : NULL);
    trace_raspi4b_boot_event("firmware", "firmware.overlays", media,
                             "success", s->overlay_state.overlays_applied);
    s->firmware_config.ethernet_mac_set = true;
    memcpy(s->firmware_config.ethernet_mac, s->boot_mac_address.a,
           sizeof(s->firmware_config.ethernet_mac));
    s->firmware_config.bootloader_config = s->eeprom_bootconf;
    s->firmware_config.bootloader_config_size = s->eeprom_bootconf_size;
    s->firmware_config.bootloader_public_key = s->eeprom_public_key;
    s->firmware_config.bootloader_public_key_size =
        s->eeprom_public_key_size;
    /*
     * The firmware configuration may have been parsed before BOOT_ORDER chose
     * the source that reached this handoff.  Publish the successful source,
     * rather than the pre-selection reset value, in /chosen/bootloader.
     */
    s->firmware_config.boot_mode = s->selected_boot_mode;
    s->firmware_config.bootloader_build_timestamp_valid =
        s->eeprom_build_timestamp_valid;
    s->firmware_config.bootloader_build_timestamp =
        s->eeprom_build_timestamp;
    s->firmware_config.bootloader_update_timestamp_valid =
        s->eeprom_update_timestamp_valid;
    s->firmware_config.bootloader_update_timestamp =
        s->eeprom_update_timestamp;
    s->firmware_config.bootloader_capabilities_valid =
        s->eeprom_capabilities_valid;
    s->firmware_config.bootloader_capabilities =
        s->eeprom_capabilities;
    s->firmware_config.bootloader_usb_valid =
        s->usb_boot_identity_valid;
    s->firmware_config.bootloader_usb_version =
        s->usb_boot_version;
    s->firmware_config.bootloader_usb_route_string =
        s->usb_boot_route_string;
    s->firmware_config.bootloader_usb_root_hub_port =
        s->usb_boot_root_hub_port;
    s->firmware_config.bootloader_usb_lun =
        s->usb_boot_selected_lun;
    pstrcpy(s->firmware_config.bootloader_version,
            sizeof(s->firmware_config.bootloader_version),
            (const char *)s->eeprom_version);
    s->firmware_config.bootloader_signed = s->bootloader_signed;
    handoff_result = raspi_arm_prepare_handoff(
        &s->handoff, &s->firmware_config, &s_base->binfo,
        kernel, s->kernel_size, effective_tree, effective_tree_size,
        cmdline, s->cmdline_size, initramfs, s->initramfs_size,
        &handoff_error);
    if (handoff_result != RASPI_HANDOFF_READY) {
        static const char * const errors[] = {
            [-RASPI_HANDOFF_ERROR_LAYOUT] = "layout-invalid",
            [-RASPI_HANDOFF_ERROR_DEVICE_TREE] = "device-tree-invalid",
            [-RASPI_HANDOFF_ERROR_KERNEL] = "kernel-invalid",
        };

        if (handoff_error) {
            error_report_err(handoff_error);
        }
        raspi4_set_handoff_status(s, errors[-handoff_result]);
        return RASPI4_BOOT_ATTEMPT_FAILED;
    }
    g_free(s->overlay_state.final_dtb_sha256);
    s->overlay_state.final_dtb_sha256 = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, s->handoff.device_tree,
        s->handoff.device_tree_size);
    as = arm_boot_address_space(&s->soc.parent_obj.cpu[0].core,
                                &s_base->binfo);
    if (!raspi_arm_install_handoff(&s->handoff, initramfs, as, NULL)) {
        raspi4_set_handoff_status(s, "memory-write-error");
        return RASPI4_BOOT_ATTEMPT_FAILED;
    }
    for (unsigned int core = 1; core < BCM283X_NCPUS; core++) {
        if (arm_set_cpu_on_with_endianness(
                core, 0x300, 0, 2, s->handoff.arm_64bit,
                s->handoff.big_endian) !=
            QEMU_ARM_POWERCTL_RET_SUCCESS) {
            raspi4_set_handoff_status(s, "secondary-power-error");
            return RASPI4_BOOT_ATTEMPT_FAILED;
        }
        s->handoff_core_mask |= 1U << core;
    }
    if (arm_set_cpu_on_with_endianness(
            0, s->handoff.entry_address,
            s->handoff.device_tree_address, 2, s->handoff.arm_64bit,
            s->handoff.big_endian) !=
        QEMU_ARM_POWERCTL_RET_SUCCESS) {
        raspi4_set_handoff_status(s, "primary-power-error");
        return RASPI4_BOOT_ATTEMPT_FAILED;
    }
    s->handoff_core_mask |= 1;
    raspi4_set_handoff_status(s, "ready");
    trace_raspi4b_boot_event(
        "handoff",
        s->handoff.arm_64bit ? "handoff.arm64" : "handoff.arm32",
        media, "success", s->handoff.entry_address);
    if (s->firmware_uart_enabled) {
        g_autofree char *line = g_strdup_printf(
            "RPI4: handoff=%s source=%s entry=0x%" PRIx64 "\r\n",
            s->handoff.arm_64bit ? "arm64" : "arm32",
            media, s->handoff.entry_address);

        raspi4_firmware_uart_write(s, line);
    }
    return RASPI4_BOOT_ATTEMPT_READY;
}

Raspi4SelfUpdateResult
raspi4_apply_self_update(Raspi4bMachineState *s,
                         const uint8_t *update, size_t update_size,
                         const uint8_t *signature, size_t signature_size,
                         const char *media)
{
    g_autofree uint8_t *current = NULL;
    Raspi4ProgramResult program_result;
    bool update_timestamp_valid;
    uint32_t update_timestamp;
    int64_t eeprom_size;

    if (!update || !signature ||
        !raspi4_recovery_signature_valid(
            update, update_size, signature, signature_size,
            &update_timestamp_valid, &update_timestamp)) {
        goto invalid;
    }
    if (!s->eeprom || !blk_is_inserted(s->eeprom)) {
        goto invalid;
    }
    eeprom_size = blk_getlength(s->eeprom);
    if (eeprom_size != RASPI4_EEPROM_SIZE ||
        update_size != RASPI4_EEPROM_SIZE) {
        goto invalid;
    }
    current = g_malloc(update_size);
    if (blk_pread(s->eeprom, 0, update_size, current, 0) < 0) {
        goto invalid;
    }
    if (!memcmp(current, update, update_size)) {
        s->self_update_status = RASPI4_SELF_UPDATE_UP_TO_DATE;
        trace_raspi4b_boot_event(
            "eeprom", "eeprom.self-update", media, "up-to-date",
            update_size);
        return RASPI4_SELF_UPDATE_CONTINUE;
    }
    if (s->eeprom_update_timestamp_valid &&
        (!update_timestamp_valid ||
         update_timestamp <= s->eeprom_update_timestamp)) {
        s->self_update_status = RASPI4_SELF_UPDATE_STALE;
        trace_raspi4b_boot_event(
            "eeprom", "eeprom.self-update", media, "stale",
            update_timestamp);
        return RASPI4_SELF_UPDATE_CONTINUE;
    }
    s->eeprom_nwp_sampled = raspi4_eeprom_nwp_live(
        s, &s->eeprom_nwp_bridge_driven);
    if (s->eeprom_write_protect) {
        s->self_update_status = RASPI4_SELF_UPDATE_WRITE_PROTECTED;
        raspi4_set_boot_observation(
            s, "self-update-write-protected", media);
        trace_raspi4b_boot_event(
            "eeprom", "eeprom.self-update", media, "write-protected",
            update_size);
        return RASPI4_SELF_UPDATE_FAILED;
    }
    program_result = raspi4_program_eeprom(s, update, update_size, false);
    if (program_result != RASPI4_PROGRAM_OK) {
        s->self_update_status = RASPI4_SELF_UPDATE_PROGRAM_FAILED;
        raspi4_set_boot_observation(s, "self-update-program-failed", media);
        trace_raspi4b_boot_event(
            "eeprom", "eeprom.self-update", media, "failure",
            program_result);
        return RASPI4_SELF_UPDATE_FAILED;
    }
    if (!raspi4_eeprom_update_timestamp_store(
            s, update_timestamp_valid, update_timestamp)) {
        s->self_update_status = RASPI4_SELF_UPDATE_PROGRAM_FAILED;
        raspi4_set_boot_observation(s, "self-update-status-error", media);
        return RASPI4_SELF_UPDATE_FAILED;
    }
    s->self_update_status = RASPI4_SELF_UPDATE_UPDATED_REBOOT;
    raspi4_set_boot_observation(s, "self-update-updated-reboot", media);
    trace_raspi4b_boot_event(
        "eeprom", "eeprom.self-update", media, "restart", update_size);
    timer_mod(s->recovery_reboot_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
              RASPI4_RECOVERY_REBOOT_DELAY_MS);
    s->pending_boot_action = RASPI4_PENDING_RECOVERY_REBOOT;
    return RASPI4_SELF_UPDATE_PENDING;

invalid:
    s->self_update_status = RASPI4_SELF_UPDATE_INVALID;
    raspi4_set_boot_observation(s, "self-update-invalid", media);
    trace_raspi4b_boot_event(
        "eeprom", "eeprom.self-update", media, "invalid", 0);
    return RASPI4_SELF_UPDATE_FAILED;
}

bool raspi4_open_boot_volume(Raspi4bMachineState *s,
                                    const Raspi4BootMediaReader *reader,
                                    RaspiFatVolume *volume, Error **errp)
{
    Raspi4AutobootConfig autoboot;
    RaspiFatVolume default_volume;
    RaspiFatVolume selected_volume;
    uint8_t requested = s->boot_partition;
    bool explicit_partition = requested >= 1 && requested <= 31;
    bool selected_opened;
    unsigned selected;

    s->tryboot_a_b = false;
    if (!raspi4_boot_media_open(reader, &default_volume, 0, errp)) {
        return false;
    }

    selected = explicit_partition ? requested : s->eeprom_partition;
    raspi4_autoboot_parse(&default_volume, s->tryboot, &autoboot);
    if (!explicit_partition && autoboot.valid && autoboot.partition_set) {
        selected = autoboot.partition;
    }
    if (autoboot.valid) {
        s->tryboot_a_b = autoboot.tryboot_a_b;
    }

    if (!selected) {
        *volume = default_volume;
    } else if (!raspi4_boot_media_open(reader, volume, selected, NULL)) {
        memset(volume, 0, sizeof(*volume));
    }
    selected_opened = volume->blk || volume->read || volume->memory;
    selected_volume = *volume;
    if ((volume->blk || volume->read || volume->memory) &&
        raspi4_boot_volume_is_bootable(s, volume)) {
        s->selected_boot_partition = volume->partition_number;
        s->firmware_config.partition = s->boot_partition;
        s->firmware_config.boot_partition = s->selected_boot_partition;
        return true;
    }
    if (!s->partition_walk || (autoboot.present && autoboot.valid)) {
        goto selected_unbootable;
    }

    /*
     * Current production EEPROM limits a partition walk to eight entries.
     * Do not parse autoboot.txt while walking: that would permit cycles.
     */
    selected = volume->partition_number ?
        volume->partition_number : selected;
    for (unsigned int step = 1; step <= 8; step++) {
        unsigned candidate = ((selected + step - 1) % 8) + 1;

        if (!raspi4_boot_media_open(reader, volume, candidate, NULL)) {
            continue;
        }
        if (raspi4_boot_volume_is_bootable(s, volume)) {
            s->selected_boot_partition = volume->partition_number;
            s->firmware_config.partition = s->boot_partition;
            s->firmware_config.boot_partition =
                s->selected_boot_partition;
            return true;
        }
    }

selected_unbootable:
    if (!selected_opened) {
        return false;
    }
    *volume = selected_volume;
    s->selected_boot_partition = volume->partition_number;
    s->firmware_config.partition = s->boot_partition;
    s->firmware_config.boot_partition = s->selected_boot_partition;
    return true;
}

void raspi4_observe_network_artifact(
    const Raspi4NetworkArtifact *artifact, uint32_t *size, char **sha256)
{
    *size = artifact->size;
    g_free(*sha256);
    *sha256 = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, artifact->data, artifact->size);
}

unsigned int raspi4_initramfs_file_count(const char *files)
{
    unsigned int count = files ? 1 : 0;

    for (const char *p = files; p && *p; p++) {
        count += *p == ',';
    }
    return count;
}

uint8_t *raspi4_concat_network_initramfs(
    const Raspi4bMachineState *s, unsigned int expected,
    uint32_t *size, char **sha256)
{
    g_autofree uint8_t *data = NULL;
    size_t total = 0;
    unsigned int count = 0;

    for (unsigned int i = 0; i < s->network_artifact_count; i++) {
        const Raspi4NetworkArtifact *artifact = &s->network_artifacts[i];

        if (artifact->kind != RASPI4_NETWORK_ARTIFACT_INITRAMFS) {
            continue;
        }
        count++;
        if (artifact->missing || !artifact->data ||
            artifact->size > RASPI4_INITRAMFS_MAX_SIZE - total) {
            return NULL;
        }
        data = g_realloc(data, total + artifact->size);
        memcpy(data + total, artifact->data, artifact->size);
        total += artifact->size;
    }
    if (!expected || count != expected || !total) {
        return NULL;
    }
    *size = total;
    g_free(*sha256);
    *sha256 = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, data, total);
    return g_steal_pointer(&data);
}

bool raspi4_firmware_status_is_missing_boot_files(
    const Raspi4bMachineState *s)
{
    return s->firmware_status &&
           (!strcmp(s->firmware_status, "firmware-missing") ||
            !strcmp(s->firmware_status, "secure-boot-image-missing"));
}

static bool raspi4_sd_wait_pending(const Raspi4bMachineState *s)
{
    return s->pending_boot_action == RASPI4_PENDING_SD_DETECT ||
           s->pending_boot_action == RASPI4_PENDING_SD_RETRY;
}

void raspi4_sd_hotplug_bh(void *opaque)
{
    Raspi4bMachineState *s = opaque;
    uint8_t action = s->pending_boot_action;
    uint32_t nibble;
    const char *source;
    Raspi4BootAttemptResult result;

    s->sd_hotplug_pending = false;
    if (!raspi4_sd_wait_pending(s) || !blk_is_inserted(s->sd)) {
        return;
    }
    nibble = (s->boot_order >> (s->boot_order_index * 4)) & 0xf;
    source = raspi4_boot_source_name(s, nibble);
    if (action == RASPI4_PENDING_SD_RETRY) {
        s->boot_attempt_count++;
        s->boot_retry_count++;
    }
    s->pending_boot_action = RASPI4_PENDING_NONE;
    result = raspi4_try_sd_firmware(s);

    if (result == RASPI4_BOOT_ATTEMPT_READY) {
        raspi4_set_boot_observation(s, "arm-handoff-ready", source);
        trace_raspi4b_boot_event("boot-source", "boot-source.hotplug",
                                 source, "success", s->firmware_size);
        return;
    }
    if (result == RASPI4_BOOT_ATTEMPT_PENDING) {
        return;
    }
    s->pending_boot_action = action;
    raspi4_set_boot_observation(
        s, action == RASPI4_PENDING_SD_DETECT ? "sd-card-detect-wait" :
                                               "sd-retry-loop",
        source);
    trace_raspi4b_boot_event("boot-source", "boot-source.hotplug", source,
                             "retry", s->boot_retry_count);
}

static void raspi4_sd_media_inserted(Notifier *notifier, void *data)
{
    Raspi4bMachineState *s = container_of(
        notifier, Raspi4bMachineState, sd_insert_notifier);

    if (data == s->sd) {
        s->sd_hotplug_pending = true;
        qemu_bh_schedule(s->sd_hotplug_bh);
    }
}

static void raspi4_sd_media_removed(Notifier *notifier, void *data)
{
    Raspi4bMachineState *s = container_of(
        notifier, Raspi4bMachineState, sd_remove_notifier);

    if (data == s->sd) {
        s->sd_hotplug_pending = true;
        qemu_bh_schedule(s->sd_hotplug_bh);
    }
}

BCM2711GenetState *raspi4_genet(Raspi4bMachineState *s)
{
    return &s->soc.peripherals.genet;
}

static void raspi4_apply_sd_controller_policy(Raspi4bMachineState *s)
{
    if (s->cm4) {
        return;
    }
    sdhci_configure_boot_clock(
        &s->soc.peripherals.parent_obj.sdhci, false,
        (s->sd_quirks & RASPI4_SD_QUIRK_DISABLE_HIGH_SPEED) ?
            RASPI4_SD_QUIRK_CLOCK_LIMIT_HZ : 0);
}

bool raspi4_pxe_option43_matches(
    const Raspi4bMachineState *s, const Raspi4DhcpOptions *options)
{
    size_t match_length = strlen((const char *)s->pxe_option43);

    if (!options->pxe_option43_seen ||
        match_length > options->pxe_option43_length) {
        return false;
    }
    for (size_t offset = 0;
         offset + match_length <= options->pxe_option43_length;
         offset++) {
        if (!memcmp(options->pxe_option43 + offset,
                    s->pxe_option43, match_length)) {
            return true;
        }
    }
    return false;
}

void raspi4_resume_after_bootcode_delay(Raspi4bMachineState *s)
{
    uint8_t index = s->boot_order_index;
    uint32_t nibble = (s->boot_order >> (index * 4)) & 0xf;
    const char *source = raspi4_boot_source_name(s, nibble);
    Raspi4BootAttemptResult result = RASPI4_BOOT_ATTEMPT_FAILED;

    raspi4_sample_edids(s);
    raspi4_clear_firmware_observation(s);

    switch (nibble) {
    case 0x0:
    case 0x1:
        result = raspi4_try_sd_firmware(s);
        break;
    case 0x2:
    case 0x7:
        result = s->network_boot_wire ?
            raspi4_try_network_artifacts(s) :
            raspi4_try_firmware(s, s->network_boot, "network");
        break;
    case 0x4:
    case 0x5:
        result = raspi4_try_usb_firmware(s, source);
        break;
    case 0x6:
        result = raspi4_try_nvme_firmware(s);
        break;
    case 0x3:
        result = raspi4_try_rpiboot_firmware(s);
        break;
    default:
        g_assert_not_reached();
    }

    if (result == RASPI4_BOOT_ATTEMPT_READY) {
        raspi4_set_boot_observation(s, "arm-handoff-ready", source);
        trace_raspi4b_boot_event("firmware", "firmware.bootcode-delay",
                                 source, "complete", s->firmware_size);
        return;
    }
    if (result == RASPI4_BOOT_ATTEMPT_PENDING) {
        return;
    }

    switch (nibble) {
    case 0x0:
        s->pending_boot_action = RASPI4_PENDING_SD_DETECT;
        raspi4_set_boot_observation(s, "sd-card-detect-wait", source);
        return;
    case 0x1:
        if (s->sd_boot_max_retries < 0) {
            s->pending_boot_action = RASPI4_PENDING_SD_RETRY;
            raspi4_set_boot_observation(s, s->cm4 ? "emmc-retry-loop" :
                                                       "sd-retry-loop",
                                        source);
            return;
        }
        s->boot_attempt_count += s->sd_boot_max_retries;
        s->boot_retry_count += s->sd_boot_max_retries;
        break;
    case 0x4:
    case 0x5:
        raspi4_schedule_usb_wait(
            s, source, raspi4_usb_only_excluded(s));
        return;
    case 0x2:
    case 0x7:
        if (s->net_boot_max_retries < 0 ||
            s->network_attempt <= s->net_boot_max_retries) {
            s->boot_retry_count++;
            s->bootcode_delay_consumed = false;
            raspi4_try_network_firmware(s);
            return;
        }
        bcm2711_genet_set_boot_client_active(raspi4_genet(s), false);
        break;
    case 0x6:
        break;
    default:
        g_assert_not_reached();
    }
    raspi4_execute_after_source_failure(s, index);
}

void raspi4_continue_after_eeprom_config(Raspi4bMachineState *s)
{
    raspi4_boot_uart_begin(s);
    raspi4_boot_watchdog_arm(s);
    if (s->otp_secure_boot) {
        raspi4_set_secure_boot_status(s, "config-verified");
        raspi4_set_boot_observation(s, "secure-boot-config-verified",
                                    "eeprom");
        trace_raspi4b_boot_event("eeprom", "eeprom.secure-config",
                                 "eeprom", "success", s->boot_order);
    }
    if (s->net_install_requested && s->net_install_enabled) {
        raspi4_net_install_request(s);
        return;
    }
    if (s->net_install_enabled && s->net_install_keyboard_wait &&
        s->net_install_keyboard_present) {
        s->pending_boot_action = RASPI4_PENDING_NET_INSTALL_KEYBOARD;
        raspi4_set_boot_observation(
            s, "net-install-keyboard-wait", "keyboard");
        if (s->net_install_shift_held) {
            raspi4_net_install_request(s);
        } else {
            timer_mod(s->boot_timeout_timer,
                      qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                      s->net_install_keyboard_wait);
        }
        return;
    }
    raspi4_execute_boot_order(s);
}

static void raspi4_restart_cycle(void *opaque)
{
    Raspi4bMachineState *s = opaque;

    s->pending_boot_action = RASPI4_PENDING_NONE;
    raspi4_execute_boot_order_from(s, 0);
}

uint32_t raspi4_encode_boot_partition(uint8_t partition)
{
    uint32_t encoded = 0;

    for (unsigned int bit = 0; bit < 6; bit++) {
        encoded |= ((partition >> bit) & 1) << (bit * 2);
    }
    return encoded;
}

QEMUTimer *raspi4_pending_boot_timer(Raspi4bMachineState *s)
{
    switch (s->pending_boot_action) {
    case RASPI4_PENDING_USB_DISCOVERY:
    case RASPI4_PENDING_USB_LUN:
    case RASPI4_PENDING_NETWORK_DHCP:
    case RASPI4_PENDING_NETWORK_TFTP:
    case RASPI4_PENDING_NETWORK_ARP:
    case RASPI4_PENDING_NETWORK_DNS:
    case RASPI4_PENDING_NETWORK_HTTP:
    case RASPI4_PENDING_NETWORK_TFTP_DALLY:
    case RASPI4_PENDING_FIRMWARE_DELAY:
    case RASPI4_PENDING_USB_STARTUP:
    case RASPI4_PENDING_SD_OVERCURRENT:
    case RASPI4_PENDING_USB_POWER_OFF:
    case RASPI4_PENDING_FATAL_REBOOT:
    case RASPI4_PENDING_NET_INSTALL_KEYBOARD:
    case RASPI4_PENDING_NETCONSOLE_LINK:
        return s->boot_timeout_timer;
    case RASPI4_PENDING_RESTART:
        return s->boot_restart_timer;
    case RASPI4_PENDING_RECOVERY_REBOOT:
        return s->recovery_reboot_timer;
    case RASPI4_PENDING_EEPROM_FLASH:
        return s->eeprom_flash_timer;
    default:
        return NULL;
    }
}

static void raspi4b_boot_reset(void *opaque)
{
    Raspi4bMachineState *s = opaque;
    Raspi4BootConfig config;
    RaspiSecureResult secure_result = RASPI_SECURE_OK;
    bool nrpiboot;

    s->pcie_ext_cfg_index = 0;
    memset(s->pcie_regs, 0, sizeof(s->pcie_regs));
    s->pcie_regs[
        RASPI4_PCIE_MSI_INTR_MASK_SET / sizeof(uint32_t)] = UINT32_MAX;
    raspi4_pcie_msi_update(s);
    s->rpiboot_dwc2_active = false;
    s->rpiboot_control_in = false;
    s->rpiboot_control_pending = RASPI4_RPIBOOT_CONTROL_NONE;
    s->rpiboot_control_value = 0;
    s->rpiboot_configuration = 0;
    g_clear_pointer(&s->rpiboot_bootcode, g_free);
    g_clear_pointer(&s->rpiboot_config, g_free);
    g_clear_pointer(&s->rpiboot_boot_img, g_free);
    s->rpiboot_bootcode_size = 0;
    s->rpiboot_bootcode_alloc = 0;
    s->rpiboot_bootcode_trusted = false;
    s->rpiboot_config_size = 0;
    s->rpiboot_config_alloc = 0;
    s->rpiboot_boot_img_size = 0;
    s->rpiboot_boot_img_alloc = 0;
    bcm2711_genet_restore_mac(raspi4_genet(s));
    s->boot_mac_address = *bcm2711_genet_mac(raspi4_genet(s));
    s->boot_mac_address_source = RASPI4_MAC_CONFIGURED;
    bcm2835_property_set_mac(raspi4_property(s), &s->boot_mac_address);
    if (!s->behavioral_boot) {
        raspi4_sample_edids(s);
        raspi4_set_boot_observation(s, "direct", "none");
        return;
    }

    timer_del(s->recovery_reboot_timer);
    timer_del(s->eeprom_flash_timer);
    timer_del(s->boot_timeout_timer);
    timer_del(s->boot_restart_timer);
    timer_del(s->network_dhcp_retransmit_timer);
    timer_del(s->network_tftp_retransmit_timer);
    timer_del(s->boot_watchdog_timer);
    timer_del(s->hdmi_diagnostics_timer);
    qemu_bh_cancel(s->usb_boot_hotplug_bh);
    s->usb_boot_hotplug_pending = false;
    qemu_bh_cancel(s->sd_hotplug_bh);
    s->sd_hotplug_pending = false;
    qemu_bh_cancel(s->network_boot_hotplug_bh);
    s->network_boot_hotplug_pending = false;
    bcm2711_genet_set_boot_client_active(raspi4_genet(s), false);
    s->pending_boot_action = RASPI4_PENDING_NONE;
    s->eeprom_flash_timing_complete = false;
    s->eeprom_flash_deadline_us = 0;
    s->eeprom_flash_image_size = 0;
    g_clear_pointer(&s->eeprom_flash_image, g_free);
    s->boot_health = RASPI4_HEALTH_NONE;

    trace_raspi4b_boot_event("reset", "reset.released", "none", "success",
                             0);
    s->reset_status = raspi4_powermgt(s)->rsts;
    s->boot_partition = raspi4_decode_boot_partition(s->reset_status);
    s->selected_boot_mode = 0;
    s->eeprom_build_timestamp_valid = false;
    s->eeprom_build_timestamp = 0;
    s->eeprom_capabilities_valid = false;
    s->eeprom_capabilities = 0;
    s->eeprom_version[0] = 0;
    s->bootloader_signed = 0;
    s->selected_boot_partition = 0;
    s->tryboot = !!(raspi4_property(s)->reboot_flags & 1);
    s->tryboot_a_b = false;
    raspi4_property(s)->reboot_flags = 0;
    g_free(s->reset_cause);
    s->reset_cause = g_strdup(raspi4_reset_cause_name(s->reset_status));
    trace_raspi4b_boot_event("reset", "reset.cause", "none", "success",
                             s->reset_status);
    raspi4_boot_config_init(s, &config);
    bcm2835_powermgt_set_halt_policy(raspi4_powermgt(s), true, false);
    s->boot_disable_hdmi = false;
    s->hdmi_delay = RASPI4_HDMI_DELAY_DEFAULT;
    s->hdmi_diagnostics_pending = false;
    s->hdmi_diagnostics_visible = false;
    s->hdmi_diagnostics_remaining_ns = 0;
    s->netconsole_enabled = false;
    s->netconsole[0] = 0;
    s->netconsole_source_port = RASPI4_NETCONSOLE_SOURCE_PORT;
    s->netconsole_destination_port =
        RASPI4_NETCONSOLE_DESTINATION_PORT;
    s->netconsole_source_ip = 0;
    s->netconsole_destination_ip = UINT32_MAX;
    memset(s->netconsole_destination_mac, 0,
           sizeof(s->netconsole_destination_mac));
    s->netconsole_packets = 0;
    s->netconsole_bytes = 0;
    s->net_install_enabled = !s->cm4;
    s->net_install_at_power_on = false;
    s->net_install_override = false;
    s->net_install_usb_fallback_consumed = false;
    s->usb_boot_files_missing = false;
    s->net_install_keyboard_wait = 900;
    s->enable_self_update = true;
    s->freeze_version = false;
    s->self_update_status = RASPI4_SELF_UPDATE_NONE;
    s->bootvar0 = 0;
    s->eeprom_config_append_size = 0;
    g_clear_pointer(&s->eeprom_bootconf, g_free);
    s->eeprom_bootconf_size = 0;
    s->eeprom_public_key_size = 0;
    memset(s->eeprom_public_key, 0, sizeof(s->eeprom_public_key));
    s->eeprom_partition = 0;
    s->partition_walk = true;
    s->boot_attempt_count = 0;
    s->boot_restart_count = 0;
    s->boot_retry_count = 0;
    s->boot_elapsed_ms = 0;
    s->boot_order_index = 0;
    s->boot_usb_discovery_wait = false;
    s->reboot_on_fatal_error = true;
    s->sd_overcurrent_check = true;
    s->sd_quirks = 0;
    raspi4_apply_sd_controller_policy(s);
    s->sd_overcurrent_warning = false;
    s->sd_power_enabled = true;
    s->sd_overcurrent_retry_count = 0;
    s->usb_msd_power_off_time = RASPI4_USB_MSD_PWR_OFF_TIME;
    s->usb_power_cycle_legacy = raspi4_usb_legacy_power_cycle(s);
    s->usb_power_off_consumed = s->cm4;
    s->usb_power_off_elapsed_ms =
        (!s->cm4 && !s->usb_power_cycle_legacy) ?
            RASPI4_USB_MSD_NEW_BOARD_MIN_OFF_MS : 0;
    s->usb_power_enabled =
        s->cm4 || s->usb_power_cycle_legacy ||
        s->usb_msd_power_off_time <= s->usb_power_off_elapsed_ms;
    s->boot_watchdog_timeout = 0;
    s->boot_watchdog_partition = 0;
    s->boot_watchdog_armed = false;
    s->boot_watchdog_remaining_ns = 0;
    s->bootcode_delay_consumed = false;
    s->bootcode_delay_seconds = 0;
    s->boot_uart_enabled = false;
    s->boot_uart_active = false;
    s->boot_uart_started = false;
    s->boot_uart_bytes = 0;
    s->boot_uart_lines = 0;
    s->vl805_enabled = false;
    s->vl805_initialized = false;
    s->firmware_uart_enabled = false;
    s->firmware_uart_started = false;
    s->firmware_uart_bytes = 0;
    s->firmware_uart_lines = 0;
    s->usb_boot_selected_index = UINT8_MAX;
    s->usb_boot_selected_device = UINT8_MAX;
    s->usb_boot_selected_lun = UINT8_MAX;
    s->usb_boot_identity_valid = false;
    s->usb_boot_version = 0;
    s->usb_boot_route_string = 0;
    s->usb_boot_root_hub_port = 0;
    memset(s->usb_msd_exclude_vid_pid, 0,
           sizeof(s->usb_msd_exclude_vid_pid));
    s->usb_msd_exclude_count = 0;
    s->usb_boot_excluded_device_count = 0;
    s->usb_boot_eligible_device_count = 0;
    s->usb_boot_last_excluded_vid_pid = UINT32_MAX;
    s->usb_boot_controller_reads = 0;
    s->usb_boot_controller_bytes = 0;
    s->usb_boot_controller_failures = 0;
    s->network_attempt = 0;
    pstrcpy((char *)s->pxe_option43, sizeof(s->pxe_option43),
            RASPI4_PXE_OPTION43_DEFAULT);
    s->tftp_prefix_mode = 0;
    s->tftp_prefix[0] = 0;
    s->network_tftp_prefix_fallback = false;
    s->network_dhcp_phase = RASPI4_DHCP_PHASE_NONE;
    s->network_dhcp_xid = RASPI4_DHCP_XID;
    s->network_dhcp_retransmit_count = 0;
    s->network_dhcp_retransmit_remaining_ns = 0;
    s->network_offered_ip = 0;
    s->network_dhcp_server_ip = 0;
    s->network_proxy_tftp_ip = 0;
    s->network_server_ip = 0;
    s->network_subnet = 0;
    s->network_gateway = 0;
    s->network_next_hop_ip = 0;
    s->network_dns_server_ip = 0;
    s->network_dns_query_id = 0;
    s->network_dns_retransmit_count = 0;
    s->network_arp_for_dns = false;
    s->network_tftp_hostname[0] = 0;
    memset(s->network_server_mac, 0, sizeof(s->network_server_mac));
    s->network_tftp_server_port = 0;
    s->network_tftp_next_block = 0;
    s->network_tftp_block_number = 0;
    s->network_tftp_size = 0;
    s->network_tftp_expected_size = 0;
    s->network_tftp_file_retransmits = 0;
    s->network_tftp_retransmit_count = 0;
    s->network_tftp_retransmit_remaining_ns = 0;
    s->network_tftp_dally_server_port = 0;
    s->network_tftp_dally_block = 0;
    g_clear_pointer(&s->network_tftp_data, g_free);
    g_clear_pointer(&s->network_tftp_filename, g_free);
    g_clear_pointer(&s->network_tftp_expected_sha256, g_free);
    g_clear_pointer(&s->network_http_response, g_free);
    g_clear_pointer(&s->network_http_ooo_data, g_free);
    g_clear_pointer(&s->network_http_ooo_valid, g_free);
    raspi4_network_tls_clear(s);
    s->network_http_mode = false;
    s->network_http_default_host = false;
    s->network_http_tls = false;
    s->network_http_state = RASPI4_HTTP_IDLE;
    s->network_http_client_port = 0;
    s->network_http_client_seq = 0;
    s->network_http_request_seq = 0;
    s->network_http_server_seq = 0;
    s->network_http_response_size = 0;
    s->network_http_content_length = 0;
    s->network_http_header_parsed = false;
    s->network_http_ooo_sequence = 0;
    s->network_http_ooo_size = 0;
    s->network_http_ooo_fin = false;
    s->network_http_ooo_fin_sequence = 0;
    raspi4_network_artifacts_clear(s);
    raspi4_sample_edids(s);
    raspi4_clear_firmware_observation(s);
    raspi4_set_recovery_status(s, "none");
    s->recovery_sha256[0] = 0;
    s->recovery_key_index = UINT32_MAX;
    raspi4_set_secure_boot_status(s, "disabled");
    s->bootsys_sha256[0] = 0;
    s->bootsys_dependencies_sha256[0] = 0;
    s->bootsys_dependency_count = 0;
    s->bootsys_key_index = UINT32_MAX;
    s->secure_public_key_valid = false;
    memset(s->secure_public_key, 0, sizeof(s->secure_public_key));
    if (!raspi4_read_otp_policy(s)) {
        return;
    }
    nrpiboot = s->nrpiboot;
    s->nrpiboot_gpio40_driven = false;
    if (s->cm4) {
        int level = bcm2838_gpio_get_external_input(
            &s->soc.peripherals.gpio, 40);

        if (level >= 0) {
            /* CM4 EMMC_DISABLE / nRPIBOOT is active low. */
            nrpiboot = !level;
            s->nrpiboot_gpio40_driven = true;
        }
    }
    s->nrpiboot_sampled = nrpiboot;
    if (nrpiboot && (s->cm4 || s->otp_rpiboot_gpio)) {
        s->boot_order = 0;
        raspi4_set_boot_observation(s, "rpiboot-wait", "rpiboot");
        trace_raspi4b_boot_event("rom", "rom.nrpiboot-sampled", "rpiboot",
                                 "success",
                                 s->nrpiboot_gpio40_driven ? 40 : 0);
        return;
    }
    if (nrpiboot) {
        trace_raspi4b_boot_event("rom", "rom.nrpiboot-unconfigured",
                                 "rpiboot", "unsupported", 0);
    }
    if (s->otp_secure_boot &&
        !raspi4_otp_customer_key_present(raspi4_otp(s))) {
        raspi4_set_secure_boot_status(s, "key-missing");
        raspi4_set_boot_observation(s, "secure-boot-key-missing", "none");
        trace_raspi4b_boot_event("rom", "rom.secure-boot", "none",
                                 "failure", s->otp_bootmode);
        return;
    }

    /* BCM2711 ROM checks recovery.bin on the primary SD before SPI EEPROM. */
    if (!s->otp_secure_boot && !s->cm4 && raspi4_try_sd_recovery(s)) {
        return;
    }

    s->boot_order = config.boot_order;
    if (!s->eeprom || !blk_is_inserted(s->eeprom)) {
        if (s->otp_secure_boot) {
            raspi4_set_secure_boot_status(s, "eeprom-missing");
            raspi4_set_boot_observation(s, "secure-boot-eeprom-missing",
                                        "none");
            trace_raspi4b_boot_event("rom", "rom.secure-boot", "none",
                                     "failure", s->boot_order);
        } else if (s->cm4) {
            raspi4_set_boot_observation(s, "rpiboot-wait", "rpiboot");
            trace_raspi4b_boot_event("rom", "rom.rpiboot-fallback",
                                     "rpiboot", "wait", s->boot_order);
        } else {
            raspi4_set_boot_observation(s, "recovery-required", "sd-card");
            trace_raspi4b_boot_event("rom", "rom.recovery-selected",
                                     "sd-card", "start", s->boot_order);
        }
        return;
    }
    if (!raspi4_read_eeprom_boot_config(s, &config, &secure_result)) {
        g_autofree char *state = s->otp_secure_boot ?
            g_strdup_printf("secure-boot-%s",
                            raspi_secure_result_name(secure_result)) :
            g_strdup("eeprom-invalid");

        raspi4_set_boot_observation(s, state, "none");
        if (s->otp_secure_boot) {
            raspi4_set_secure_boot_status(
                s, raspi_secure_result_name(secure_result));
        }
        trace_raspi4b_boot_event("eeprom", "eeprom.read", "none", "failure",
                                 s->boot_order);
        raspi4_hdmi_diagnostics_show(s);
        return;
    }
    s->boot_order = config.boot_order;
    s->boot_uart_enabled = config.boot_uart;
    bcm2835_powermgt_set_halt_policy(
        raspi4_powermgt(s), config.wake_on_gpio,
        config.power_off_on_halt);
    s->bootloader_signed =
        (config.signed_boot ? RASPI4_BOOTLOADER_SIGNED_CONFIG : 0) |
        ((s->otp_secure_boot_flags &
          BCM2711_OTP_SECURE_BOOT_FLAGS_REVOKE_DEVKEY) ?
         RASPI4_BOOTLOADER_SIGNED_DEVKEY_REVOKED : 0) |
        (raspi4_otp_customer_key_present(raspi4_otp(s)) ?
         RASPI4_BOOTLOADER_SIGNED_CUSTOMER_KEY : 0);
    s->vl805_enabled = config.vl805;
    s->vl805_initialized =
        s->cm4 && s->vl805_enabled && s->vl805_xhci;
    s->bootvar0 = config.bootvar0;
    s->firmware_config.bootloader_config_append =
        s->eeprom_config_append;
    s->firmware_config.bootloader_config_append_size =
        s->eeprom_config_append_size;
    s->eeprom_partition = config.partition;
    s->partition_walk = config.partition_walk;
    s->firmware_config.bootvar0 = s->bootvar0;
    s->max_restarts = config.max_restarts;
    s->reboot_on_fatal_error = config.reboot_on_fatal_error;
    s->sd_boot_max_retries = config.sd_boot_max_retries;
    s->sd_overcurrent_check = config.sd_overcurrent_check;
    s->sd_quirks = s->cm4 ? 0 : config.sd_quirks;
    raspi4_apply_sd_controller_policy(s);
    s->usb_msd_discover_timeout = config.usb_msd_discover_timeout;
    s->usb_msd_lun_timeout = config.usb_msd_lun_timeout;
    s->usb_msd_startup_delay = config.usb_msd_startup_delay;
    s->usb_msd_power_off_time = config.usb_msd_power_off_time;
    memcpy(s->usb_msd_exclude_vid_pid,
           config.usb_msd_exclude_vid_pid,
           sizeof(s->usb_msd_exclude_vid_pid));
    s->usb_msd_exclude_count = config.usb_msd_exclude_count;
    s->usb_power_enabled =
        s->cm4 || s->usb_power_cycle_legacy ||
        s->usb_msd_power_off_time <= s->usb_power_off_elapsed_ms;
    s->boot_watchdog_timeout = config.boot_watchdog_timeout;
    s->boot_watchdog_partition = config.boot_watchdog_partition;
    s->net_boot_max_retries = config.net_boot_max_retries;
    s->dhcp_timeout = config.dhcp_timeout;
    s->dhcp_req_timeout = config.dhcp_req_timeout;
    s->dhcp_option97 = config.dhcp_option97;
    pstrcpy((char *)s->pxe_option43, sizeof(s->pxe_option43),
            config.pxe_option43);
    s->netconsole_enabled = config.netconsole_enabled;
    pstrcpy((char *)s->netconsole, sizeof(s->netconsole),
            config.netconsole);
    s->netconsole_source_port = config.netconsole_source_port;
    s->netconsole_destination_port =
        config.netconsole_destination_port;
    s->netconsole_source_ip = config.netconsole_source_ip;
    s->netconsole_destination_ip =
        config.netconsole_destination_ip;
    memcpy(s->netconsole_destination_mac,
           config.netconsole_destination_mac,
           sizeof(s->netconsole_destination_mac));
    s->tftp_ip_set = config.tftp_ip_set;
    s->tftp_ip = config.tftp_ip;
    raspi4_apply_mac_address(s, &config);
    raspi4_set_tftp_prefix(s, &config);
    s->client_ip_set = config.client_ip_set;
    s->client_ip = config.client_ip;
    s->subnet_set = config.subnet_set;
    s->subnet = config.subnet;
    s->gateway_set = config.gateway_set;
    s->gateway = config.gateway;
    s->tftp_file_timeout = config.tftp_file_timeout;
    s->http_host_set = config.http_host_set;
    pstrcpy((char *)s->http_host, sizeof(s->http_host), config.http_host);
    pstrcpy((char *)s->http_path, sizeof(s->http_path), config.http_path);
    s->http_port = config.http_port;
    s->http_cacert_hash_set = config.http_cacert_hash_set;
    pstrcpy((char *)s->http_cacert_hash, sizeof(s->http_cacert_hash),
            config.http_cacert_hash);
    s->boot_disable_hdmi = config.disable_hdmi;
    s->hdmi_delay = config.hdmi_delay;
    s->net_install_enabled =
        !s->boot_disable_hdmi &&
        (config.net_install_enabled || config.net_install_at_power_on);
    s->net_install_at_power_on =
        !s->boot_disable_hdmi && config.net_install_at_power_on;
    s->net_install_keyboard_wait = config.net_install_keyboard_wait;
    s->enable_self_update = config.enable_self_update;
    s->freeze_version = config.freeze_version;
    raspi4_hdmi_diagnostics_arm(s);
    if (s->netconsole_enabled &&
        !bcm2711_genet_boot_link_up(raspi4_genet(s))) {
        s->pending_boot_action = RASPI4_PENDING_NETCONSOLE_LINK;
        raspi4_set_boot_observation(s, "netconsole-link-wait", "network");
        timer_mod(s->boot_timeout_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                  s->dhcp_timeout);
        return;
    }
    raspi4_netconsole_begin(s);
    raspi4_continue_after_eeprom_config(s);
}

static bool raspi4b_get_nrpiboot(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->nrpiboot;
}

static void raspi4b_set_nrpiboot(Object *obj, bool value, Error **errp)
{
    RASPI4B_MACHINE(obj)->nrpiboot = value;
}

static bool raspi4b_get_nrpiboot_sampled(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->nrpiboot_sampled;
}

static char *raspi4b_get_nrpiboot_source(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->nrpiboot_gpio40_driven ? "gpio40" :
                                                "machine-property");
}

static bool raspi4b_get_sd_recovery_enabled(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->sd_recovery_enabled;
}

static void raspi4b_set_sd_recovery_enabled(Object *obj, bool value,
                                            Error **errp)
{
    RASPI4B_MACHINE(obj)->sd_recovery_enabled = value;
}

static void raspi4b_set_otp_provision_fail_after(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint8_t value;

    if (visit_type_uint8(v, name, &value, errp)) {
        RASPI4B_MACHINE(obj)->otp_provision_fail_after = value;
    }
}

static char *raspi4b_get_hat_eeprom_drive(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->hat_eeprom_drive ? s->hat_eeprom_drive : "");
}

static char *raspi4b_get_hdmi0_edid_file(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->hdmi_edid_file[0] ? s->hdmi_edid_file[0] : "");
}

static void raspi4b_set_hdmi0_edid_file(Object *obj, const char *value,
                                        Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    g_free(s->hdmi_edid_file[0]);
    s->hdmi_edid_file[0] = value[0] ? g_strdup(value) : NULL;
}

static char *raspi4b_get_hdmi1_edid_file(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->hdmi_edid_file[1] ? s->hdmi_edid_file[1] : "");
}

static void raspi4b_set_hdmi1_edid_file(Object *obj, const char *value,
                                        Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    g_free(s->hdmi_edid_file[1]);
    s->hdmi_edid_file[1] = value[0] ? g_strdup(value) : NULL;
}

static char *raspi4b_get_hdmi_edid_name(Object *obj, unsigned int port)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup((const char *)s->hdmi_edid_name[port]);
}

static char *raspi4b_get_hdmi0_edid_name(Object *obj, Error **errp)
{
    return raspi4b_get_hdmi_edid_name(obj, 0);
}

static char *raspi4b_get_hdmi1_edid_name(Object *obj, Error **errp)
{
    return raspi4b_get_hdmi_edid_name(obj, 1);
}

static char *raspi4b_get_hdmi_edid_status(Object *obj, unsigned int port)
{
    static const char *const names[] = {
        [RASPI4_EDID_DISCONNECTED] = "disconnected",
        [RASPI4_EDID_VALID] = "valid",
        [RASPI4_EDID_INVALID] = "invalid",
    };
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    uint8_t status = s->hdmi_edid_status[port];

    return g_strdup(status < ARRAY_SIZE(names) ? names[status] : "invalid");
}

static char *raspi4b_get_hdmi0_edid_status(Object *obj, Error **errp)
{
    return raspi4b_get_hdmi_edid_status(obj, 0);
}

static char *raspi4b_get_hdmi1_edid_status(Object *obj, Error **errp)
{
    return raspi4b_get_hdmi_edid_status(obj, 1);
}

static char *raspi4b_get_network_boot_drive(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->network_boot_drive ? s->network_boot_drive : "");
}

static void raspi4b_set_network_boot_drive(Object *obj, const char *value,
                                           Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    g_free(s->network_boot_drive);
    g_clear_pointer(&s->network_tftp_data, g_free);
    g_clear_pointer(&s->network_tftp_filename, g_free);
    g_clear_pointer(&s->network_tftp_expected_sha256, g_free);
    raspi4_network_artifacts_clear(s);
    s->network_boot_drive = value[0] ? g_strdup(value) : NULL;
}

static char *raspi4b_get_nvme_drive(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->nvme_drive ? s->nvme_drive : "");
}

static void raspi4b_set_nvme_drive(Object *obj, const char *value,
                                   Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    g_free(s->nvme_drive);
    s->nvme_drive = value[0] ? g_strdup(value) : NULL;
}

static char *raspi4b_get_http_tls_creds(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->http_tls_creds ? s->http_tls_creds : "");
}

static void raspi4b_set_http_tls_creds(Object *obj, const char *value,
                                       Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    g_free(s->http_tls_creds);
    s->http_tls_creds = value[0] ? g_strdup(value) : NULL;
}

static void raspi4b_set_rpiboot_bootcode_trusted_sha256(Object *obj,
                                                         const char *value,
                                                         Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    char digest[65];

    if (!value[0]) {
        g_clear_pointer(&s->rpiboot_bootcode_trusted_sha256, g_free);
        return;
    }
    if (!raspi4_parse_sha256(value, digest)) {
        error_setg(errp, "rpiboot-bootcode-trusted-sha256 must be exactly "
                   "64 hexadecimal characters");
        return;
    }
    g_free(s->rpiboot_bootcode_trusted_sha256);
    s->rpiboot_bootcode_trusted_sha256 = g_strdup(digest);
}

static void raspi4b_set_recovery_trusted_sha256(Object *obj,
                                                const char *value,
                                                Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    char digest[65];

    if (!value[0]) {
        g_clear_pointer(&s->recovery_trusted_sha256, g_free);
        return;
    }
    if (!raspi4_parse_sha256(value, digest)) {
        error_setg(errp, "recovery-trusted-sha256 must be exactly 64 "
                   "hexadecimal characters");
        return;
    }
    g_free(s->recovery_trusted_sha256);
    s->recovery_trusted_sha256 = g_strdup(digest);
}

static bool raspi4b_get_network_boot_wire(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->network_boot_wire;
}

static bool raspi4b_get_wireless_model(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->wireless_model;
}

static char *raspi4b_get_wireless_netdev(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->wireless_netdev ? s->wireless_netdev : "");
}

static bool raspi4b_get_net_install_requested(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->net_install_requested;
}

static void raspi4b_get_hdmi_delay(Object *obj, Visitor *v,
                                   const char *name, void *opaque,
                                   Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->hdmi_delay;

    visit_type_uint32(v, name, &value, errp);
}

static bool raspi4b_get_hdmi_diagnostics_pending(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->hdmi_diagnostics_pending;
}

static bool raspi4b_get_hdmi_diagnostics_visible(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->hdmi_diagnostics_visible;
}

static void raspi4b_get_hdmi_diagnostics_remaining(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    uint64_t value = 0;

    if (s->hdmi_diagnostics_pending &&
        timer_pending(s->hdmi_diagnostics_timer)) {
        uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        uint64_t expiry = timer_expire_time_ns(s->hdmi_diagnostics_timer);

        if (expiry > now) {
            value = expiry - now;
        }
    }
    visit_type_uint64(v, name, &value, errp);
}

static bool raspi4b_get_netconsole_enabled(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->netconsole_enabled;
}

static char *raspi4b_get_netconsole_config(Object *obj, Error **errp)
{
    return g_strdup((const char *)RASPI4B_MACHINE(obj)->netconsole);
}

static bool raspi4b_get_netconsole_link_waiting(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->pending_boot_action ==
           RASPI4_PENDING_NETCONSOLE_LINK;
}

static void raspi4b_get_netconsole_remaining(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    uint64_t value = 0;

    if (s->pending_boot_action == RASPI4_PENDING_NETCONSOLE_LINK &&
        timer_pending(s->boot_timeout_timer)) {
        uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        uint64_t expiry = timer_expire_time_ns(s->boot_timeout_timer);

        if (expiry > now) {
            value = expiry - now;
        }
    }
    visit_type_uint64(v, name, &value, errp);
}

static void raspi4b_get_netconsole_packets(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint64_t value = RASPI4B_MACHINE(obj)->netconsole_packets;

    visit_type_uint64(v, name, &value, errp);
}

static void raspi4b_get_netconsole_bytes(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint64_t value = RASPI4B_MACHINE(obj)->netconsole_bytes;

    visit_type_uint64(v, name, &value, errp);
}

static void raspi4b_set_net_install_requested(Object *obj, bool value,
                                              Error **errp)
{
    RASPI4B_MACHINE(obj)->net_install_requested = value;
}

static bool raspi4b_get_net_install_enabled(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->net_install_enabled;
}

static bool raspi4b_get_net_install_at_power_on(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->net_install_at_power_on;
}

static bool raspi4b_get_net_install_usb_fallback_consumed(
    Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->net_install_usb_fallback_consumed;
}

static void raspi4b_get_net_install_keyboard_wait(Object *obj, Visitor *v,
                                                   const char *name,
                                                   void *opaque,
                                                   Error **errp)
{
    uint32_t value =
        RASPI4B_MACHINE(obj)->net_install_keyboard_wait;

    visit_type_uint32(v, name, &value, errp);
}

static bool raspi4b_get_net_install_keyboard_present(Object *obj,
                                                      Error **errp)
{
    return RASPI4B_MACHINE(obj)->net_install_keyboard_present;
}

static void raspi4b_set_net_install_keyboard_present(Object *obj, bool value,
                                                      Error **errp)
{
    RASPI4B_MACHINE(obj)->net_install_keyboard_present = value;
}

static bool raspi4b_get_net_install_shift_held(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->net_install_shift_held;
}

static void raspi4b_set_net_install_shift_held(Object *obj, bool value,
                                               Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    s->net_install_shift_held = value;
    if (value &&
        s->pending_boot_action == RASPI4_PENDING_NET_INSTALL_KEYBOARD) {
        raspi4_net_install_request(s);
    }
}

static bool raspi4b_get_net_install_keyboard_waiting(Object *obj,
                                                     Error **errp)
{
    return RASPI4B_MACHINE(obj)->pending_boot_action ==
        RASPI4_PENDING_NET_INSTALL_KEYBOARD;
}

static void raspi4b_get_net_install_keyboard_remaining(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    uint64_t value = 0;

    if (s->pending_boot_action == RASPI4_PENDING_NET_INSTALL_KEYBOARD &&
        timer_pending(s->boot_timeout_timer)) {
        uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        uint64_t expiry = timer_expire_time_ns(s->boot_timeout_timer);

        if (expiry > now) {
            value = expiry - now;
        }
    }
    visit_type_uint64(v, name, &value, errp);
}

static bool raspi4b_get_enable_self_update(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->enable_self_update;
}

static bool raspi4b_get_freeze_version(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->freeze_version;
}

static char *raspi4b_get_self_update_status(Object *obj, Error **errp)
{
    static const char * const names[RASPI4_SELF_UPDATE_STATUS__MAX] = {
        [RASPI4_SELF_UPDATE_NONE] = "none",
        [RASPI4_SELF_UPDATE_DISABLED] = "disabled",
        [RASPI4_SELF_UPDATE_FROZEN] = "frozen",
        [RASPI4_SELF_UPDATE_UNSUPPORTED] = "unsupported",
        [RASPI4_SELF_UPDATE_UP_TO_DATE] = "up-to-date",
        [RASPI4_SELF_UPDATE_STALE] = "stale",
        [RASPI4_SELF_UPDATE_INVALID] = "invalid",
        [RASPI4_SELF_UPDATE_WRITE_PROTECTED] = "write-protected",
        [RASPI4_SELF_UPDATE_PROGRAM_FAILED] = "program-failed",
        [RASPI4_SELF_UPDATE_UPDATED_REBOOT] = "updated-reboot",
    };
    uint8_t status = RASPI4B_MACHINE(obj)->self_update_status;

    return g_strdup(status < RASPI4_SELF_UPDATE_STATUS__MAX ?
                    names[status] : "invalid-state");
}

static void raspi4b_set_network_boot_wire(Object *obj, bool value,
                                          Error **errp)
{
    RASPI4B_MACHINE(obj)->network_boot_wire = value;
}

static void raspi4b_set_wireless_model(Object *obj, bool value,
                                       Error **errp)
{
    RASPI4B_MACHINE(obj)->wireless_model = value;
}

static void raspi4b_set_wireless_netdev(Object *obj, const char *value,
                                        Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    g_free(s->wireless_netdev);
    s->wireless_netdev = value[0] ? g_strdup(value) : NULL;
}

static char *raspi_cm4_get_emmc_drive(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->emmc_drive ? s->emmc_drive : "");
}

static void raspi_cm4_set_emmc_drive(Object *obj, const char *value,
                                     Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    g_free(s->emmc_drive);
    s->emmc_drive = value[0] ? g_strdup(value) : NULL;
}

static char *raspi_cm4_get_emmc_boot_drive(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->emmc_boot_drive ? s->emmc_boot_drive : "");
}

static void raspi_cm4_set_emmc_boot_drive(Object *obj, const char *value,
                                          Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    g_free(s->emmc_boot_drive);
    s->emmc_boot_drive = value[0] ? g_strdup(value) : NULL;
}

static char *raspi_cm4_get_emmc_rpmb_drive(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->emmc_rpmb_drive ? s->emmc_rpmb_drive : "");
}

static void raspi_cm4_set_emmc_rpmb_drive(Object *obj, const char *value,
                                          Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    g_free(s->emmc_rpmb_drive);
    s->emmc_rpmb_drive = value[0] ? g_strdup(value) : NULL;
}

static char *raspi_cm4_get_emmc_cid(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->emmc_cid ? s->emmc_cid : "");
}

static void raspi_cm4_set_emmc_cid(Object *obj, const char *value,
                                   Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    g_free(s->emmc_cid);
    s->emmc_cid = value[0] ? g_strdup(value) : NULL;
}

static const char *raspi_cm4_emmc_data_error_name(uint8_t error)
{
    static const char *const names[] = {
        [SDHCI_DATA_ERROR_NONE] = "none",
        [SDHCI_DATA_ERROR_TIMEOUT] = "timeout",
        [SDHCI_DATA_ERROR_CRC] = "crc",
    };

    return names[error];
}

static char *raspi_cm4_get_emmc_data_error(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(raspi_cm4_emmc_data_error_name(s->emmc_data_error));
}

static void raspi_cm4_set_emmc_data_error(Object *obj, const char *value,
                                          Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    for (uint8_t error = SDHCI_DATA_ERROR_NONE;
         error <= SDHCI_DATA_ERROR_CRC; error++) {
        if (!strcmp(value, raspi_cm4_emmc_data_error_name(error))) {
            s->emmc_data_error = error;
            return;
        }
    }
    error_setg(errp, "emmc-data-error must be none, timeout, or crc");
}

static void raspi_cm4_get_emmc_data_error_after(Object *obj, Visitor *v,
                                                const char *name,
                                                void *opaque, Error **errp)
{
    uint64_t value = RASPI4B_MACHINE(obj)->emmc_data_error_after;

    visit_type_uint64(v, name, &value, errp);
}

static void raspi_cm4_set_emmc_data_error_after(Object *obj, Visitor *v,
                                                const char *name,
                                                void *opaque, Error **errp)
{
    uint64_t value;

    if (visit_type_uint64(v, name, &value, errp)) {
        RASPI4B_MACHINE(obj)->emmc_data_error_after = value;
    }
}

static void raspi_cm4_get_emmc_data_error_count(Object *obj, Visitor *v,
                                                const char *name,
                                                void *opaque, Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->emmc_data_error_count;

    visit_type_uint32(v, name, &value, errp);
}

static void raspi_cm4_set_emmc_data_error_count(Object *obj, Visitor *v,
                                                const char *name,
                                                void *opaque, Error **errp)
{
    uint32_t value;

    if (visit_type_uint32(v, name, &value, errp)) {
        RASPI4B_MACHINE(obj)->emmc_data_error_count = value;
    }
}

static void raspi_cm4_get_emmc_cache_size(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    uint64_t value = RASPI4B_MACHINE(obj)->emmc_cache_size;

    visit_type_size(v, name, &value, errp);
}

static void raspi_cm4_set_emmc_cache_size(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    uint64_t value;

    if (visit_type_size(v, name, &value, errp)) {
        RASPI4B_MACHINE(obj)->emmc_cache_size = value;
    }
}

static bool raspi_cm4_get_emmc_cache_power_loss(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->emmc_cache_power_loss_on_reset;
}

static void raspi_cm4_set_emmc_cache_power_loss(Object *obj, bool value,
                                                Error **errp)
{
    RASPI4B_MACHINE(obj)->emmc_cache_power_loss_on_reset = value;
}

static void raspi_cm4_get_emmc_cache_flush_delay(Object *obj, Visitor *v,
                                                 const char *name,
                                                 void *opaque, Error **errp)
{
    uint64_t value =
        RASPI4B_MACHINE(obj)->emmc_cache_flush_sector_delay_us;

    visit_type_uint64(v, name, &value, errp);
}

static void raspi_cm4_set_emmc_cache_flush_delay(Object *obj, Visitor *v,
                                                 const char *name,
                                                 void *opaque, Error **errp)
{
    uint64_t value;

    if (visit_type_uint64(v, name, &value, errp)) {
        RASPI4B_MACHINE(obj)->emmc_cache_flush_sector_delay_us = value;
    }
}

static void raspi_cm4_get_emmc_program_delay(Object *obj, Visitor *v,
                                             const char *name,
                                             void *opaque, Error **errp)
{
    uint64_t value =
        RASPI4B_MACHINE(obj)->emmc_program_sector_delay_us;

    visit_type_uint64(v, name, &value, errp);
}

static void raspi_cm4_set_emmc_program_delay(Object *obj, Visitor *v,
                                             const char *name,
                                             void *opaque, Error **errp)
{
    uint64_t value;

    if (visit_type_uint64(v, name, &value, errp)) {
        RASPI4B_MACHINE(obj)->emmc_program_sector_delay_us = value;
    }
}

static void raspi_cm4_get_emmc_erase_delay(Object *obj, Visitor *v,
                                           const char *name,
                                           void *opaque, Error **errp)
{
    uint64_t value =
        RASPI4B_MACHINE(obj)->emmc_erase_group_delay_us;

    visit_type_uint64(v, name, &value, errp);
}

static void raspi_cm4_set_emmc_erase_delay(Object *obj, Visitor *v,
                                           const char *name,
                                           void *opaque, Error **errp)
{
    uint64_t value;

    if (visit_type_uint64(v, name, &value, errp)) {
        RASPI4B_MACHINE(obj)->emmc_erase_group_delay_us = value;
    }
}

static char *raspi_cm4_get_provision_state_file(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->provision_state_file ? s->provision_state_file : "");
}

static void raspi_cm4_set_provision_state_file(Object *obj,
                                                const char *value,
                                                Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    g_free(s->provision_state_file);
    s->provision_state_file = value[0] ? g_strdup(value) : NULL;
}

static char *raspi_cm4_get_provision_state(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    char *state = raspi4_provision_read(s);

    return state ? state : g_strdup("disabled");
}

static bool raspi_cm4_get_provision_flash_complete(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    g_autofree char *state = raspi4_provision_read(s);

    return state && !strcmp(state, "boot-ready");
}

static void raspi_cm4_set_provision_flash_complete(Object *obj, bool value,
                                                   Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    g_autofree char *state = NULL;

    if (!value) {
        return;
    }
    if (!s->provision_state_file || s->provision_lock_fd < 0) {
        error_setg(errp, "CM4 provisioning lifecycle is not owned by QEMU");
        return;
    }
    state = raspi4_provision_read(s);
    if (!state || strcmp(state, "rpiboot-complete")) {
        error_setg(errp,
                   "CM4 flash completion requires rpiboot-complete "
                   "(found '%s')", state ? state : "unreadable");
        return;
    }
    if (!raspi4_provision_write(s, "boot-ready")) {
        error_setg(errp, "cannot publish CM4 boot-ready lifecycle state");
        return;
    }
    s->provision_rpiboot_owned = false;
}

static bool raspi_cm4_get_provision_recover_stale(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->provision_recover_stale;
}

static void raspi_cm4_set_provision_recover_stale(Object *obj, bool value,
                                                  Error **errp)
{
    RASPI4B_MACHINE(obj)->provision_recover_stale = value;
}

static char *raspi_cm4_get_provision_recovery(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->provision_stale_recovered ? "stale-qemu-owned" :
                                                   "none");
}

static char *raspi4b_get_gpio_chardev(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->gpio_chardev ? s->gpio_chardev : "");
}

static void raspi4b_set_gpio_chardev(Object *obj, const char *value,
                                     Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    g_free(s->gpio_chardev);
    s->gpio_chardev = value[0] ? g_strdup(value) : NULL;
}

static void raspi4b_set_otp_drive(Object *obj, const char *value, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    g_free(s->otp_drive);
    s->otp_drive = value[0] ? g_strdup(value) : NULL;
}

static void raspi4b_set_otp_rpiboot_gpio(Object *obj, Visitor *v,
                                         const char *name, void *opaque,
                                         Error **errp)
{
    uint8_t value;

    if (!visit_type_uint8(v, name, &value, errp)) {
        return;
    }
    if (value != 0 && value != 2 && value != 4 && value != 5 &&
        value != 6 && value != 7 && value != 8) {
        error_setg(errp, "otp-rpiboot-gpio must be 0, 2, 4, 5, 6, 7, or 8");
        return;
    }
    RASPI4B_MACHINE(obj)->otp_rpiboot_gpio = value;
}

static void raspi4b_set_hat_eeprom_drive(Object *obj, const char *value,
                                         Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    g_free(s->hat_eeprom_drive);
    s->hat_eeprom_drive = value[0] ? g_strdup(value) : NULL;
}

static char *raspi4b_get_secure_boot_status(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->secure_boot_status ? s->secure_boot_status : "none");
}

static char *raspi4b_get_secure_image_sha256(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->secure_image_sha256 ?
                    s->secure_image_sha256 : "none");
}

static char *raspi4b_get_secure_signature_sha256(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->secure_signature_sha256 ?
                    s->secure_signature_sha256 : "none");
}

static void raspi4b_get_secure_image_size(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->secure_image_size;

    visit_type_uint32(v, name, &value, errp);
}

static void raspi4b_get_secure_signature_size(Object *obj, Visitor *v,
                                              const char *name, void *opaque,
                                              Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->secure_signature_size;

    visit_type_uint32(v, name, &value, errp);
}

static char *raspi4b_get_tftp_ip(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    if (!s->tftp_ip_set) {
        return g_strdup("");
    }
    return g_strdup_printf("%u.%u.%u.%u",
                           (s->tftp_ip >> 24) & 0xff,
                           (s->tftp_ip >> 16) & 0xff,
                           (s->tftp_ip >> 8) & 0xff,
                           s->tftp_ip & 0xff);
}

static void raspi4b_get_tftp_prefix_mode(Object *obj, Visitor *v,
                                         const char *name, void *opaque,
                                         Error **errp)
{
    uint8_t value = RASPI4B_MACHINE(obj)->tftp_prefix_mode;

    visit_type_uint8(v, name, &value, errp);
}

static char *raspi4b_get_tftp_prefix(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup((const char *)s->tftp_prefix);
}

static bool raspi4b_get_tftp_prefix_fallback(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->network_tftp_prefix_fallback;
}

static char *raspi4b_get_network_server_ip(Object *obj, Error **errp)
{
    uint32_t address = RASPI4B_MACHINE(obj)->network_server_ip;

    if (!raspi4_ipv4_is_unicast(address)) {
        return g_strdup("");
    }
    return g_strdup_printf("%u.%u.%u.%u",
                           (address >> 24) & 0xff,
                           (address >> 16) & 0xff,
                           (address >> 8) & 0xff,
                           address & 0xff);
}

static char *raspi4b_get_dns_server_ip(Object *obj, Error **errp)
{
    uint32_t address = RASPI4B_MACHINE(obj)->network_dns_server_ip;

    if (!raspi4_ipv4_is_unicast(address)) {
        return g_strdup("");
    }
    return g_strdup_printf("%u.%u.%u.%u",
                           (address >> 24) & 0xff,
                           (address >> 16) & 0xff,
                           (address >> 8) & 0xff,
                           address & 0xff);
}

static char *raspi4b_get_tftp_hostname(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup((const char *)s->network_tftp_hostname);
}

static char *raspi4b_get_http_host(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup((s->http_host_set || s->network_http_default_host) ?
                    (const char *)s->http_host : "");
}

static char *raspi4b_get_http_path(Object *obj, Error **errp)
{
    return g_strdup((const char *)RASPI4B_MACHINE(obj)->http_path);
}

static char *raspi4b_get_http_cacert_hash(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->http_cacert_hash_set ?
                    (const char *)s->http_cacert_hash : "");
}

static char *raspi4_format_ipv4(bool configured, uint32_t address)
{
    if (!configured) {
        return g_strdup("");
    }
    return g_strdup_printf("%u.%u.%u.%u",
                           (address >> 24) & 0xff,
                           (address >> 16) & 0xff,
                           (address >> 8) & 0xff,
                           address & 0xff);
}

static char *raspi4b_get_client_ip(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return raspi4_format_ipv4(s->client_ip_set, s->client_ip);
}

static char *raspi4b_get_subnet(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return raspi4_format_ipv4(s->subnet_set, s->subnet);
}

static char *raspi4b_get_gateway(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return raspi4_format_ipv4(s->gateway_set, s->gateway);
}

static void raspi4b_get_max_restarts(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    int32_t value = RASPI4B_MACHINE(obj)->max_restarts;

    visit_type_int32(v, name, &value, errp);
}

static void raspi4b_get_sd_boot_max_retries(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    int32_t value = RASPI4B_MACHINE(obj)->sd_boot_max_retries;

    visit_type_int32(v, name, &value, errp);
}

static bool raspi4b_get_sd_overcurrent(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    bool bridge_driven;

    return raspi4_sd_overcurrent_live(s, &bridge_driven);
}

static void raspi4b_get_thermal_temperature(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    int32_t value = s->soc_initialized ?
        s->soc.peripherals.thermal2711.temperature_millicelsius :
        s->thermal_temperature_millicelsius;

    visit_type_int32(v, name, &value, errp);
}

static void raspi4b_set_thermal_temperature(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    int32_t value;

    if (!visit_type_int32(v, name, &value, errp)) {
        return;
    }
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    s->thermal_temperature_millicelsius = value;
    if (s->soc_initialized) {
        s->soc.peripherals.thermal2711.temperature_millicelsius = value;
    }
}

static bool raspi4b_get_thermal_sensor_valid(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return s->soc_initialized ?
        s->soc.peripherals.thermal2711.sensor_valid :
        s->thermal_sensor_valid;
}

static void raspi4b_set_thermal_sensor_valid(Object *obj, bool value,
                                             Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    s->thermal_sensor_valid = value;
    if (s->soc_initialized) {
        s->soc.peripherals.thermal2711.sensor_valid = value;
    }
}

static void raspi4b_get_firmware_throttled_current(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    uint32_t value = s->soc_initialized ?
        raspi4_property(s)->throttled_current :
        s->firmware_throttled_initial;

    visit_type_uint32(v, name, &value, errp);
}

static void raspi4b_set_firmware_throttled_current(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (value & ~BCM2835_PROPERTY_THROTTLED_CURRENT_MASK) {
        error_setg(errp,
                   "firmware-throttled-current accepts only bits 0 through 2");
        return;
    }
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    s->firmware_throttled_initial = value;
    if (s->soc_initialized) {
        bcm2835_property_set_throttled_current(raspi4_property(s), value);
    }
}

static void raspi4b_get_firmware_throttled_status(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    uint32_t value = s->soc_initialized ?
        bcm2835_property_get_throttled(raspi4_property(s)) :
        s->firmware_throttled_initial |
        (s->firmware_throttled_initial <<
         BCM2835_PROPERTY_THROTTLED_HISTORY_SHIFT);

    visit_type_uint32(v, name, &value, errp);
}

static void raspi4b_set_sd_overcurrent(Object *obj, bool value, Error **errp)
{
    RASPI4B_MACHINE(obj)->sd_overcurrent = value;
}

static char *raspi4b_get_sd_overcurrent_source(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    bool bridge_driven;

    raspi4_sd_overcurrent_live(s, &bridge_driven);
    return g_strdup(bridge_driven ? "gpio-bridge" : "machine-property");
}

static bool raspi4b_get_sd_overcurrent_check(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->sd_overcurrent_check;
}

static void raspi4b_get_sd_quirks(Object *obj, Visitor *v,
                                  const char *name, void *opaque,
                                  Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->sd_quirks;

    visit_type_uint32(v, name, &value, errp);
}

static bool raspi4b_get_sd_high_speed_enabled(Object *obj, Error **errp)
{
    return !(RASPI4B_MACHINE(obj)->sd_quirks &
             RASPI4_SD_QUIRK_DISABLE_HIGH_SPEED);
}

static void raspi4b_get_sd_clock_limit(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    uint32_t value =
        (RASPI4B_MACHINE(obj)->sd_quirks &
         RASPI4_SD_QUIRK_DISABLE_HIGH_SPEED) ?
            RASPI4_SD_QUIRK_CLOCK_LIMIT_HZ : 0;

    visit_type_uint32(v, name, &value, errp);
}

static void raspi4b_get_sd_controller_clock(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    uint32_t value = (!s->behavioral_boot || s->cm4) ? 0 :
        sdhci_get_clock_hz(&s->soc.peripherals.parent_obj.sdhci);

    visit_type_uint32(v, name, &value, errp);
}

static bool raspi4b_get_sd_controller_high_speed(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return s->behavioral_boot && !s->cm4 &&
        sdhci_get_high_speed(&s->soc.peripherals.parent_obj.sdhci);
}

static bool raspi4b_get_sd_overcurrent_warning(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->sd_overcurrent_warning;
}

static bool raspi4b_get_sd_power_enabled(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->sd_power_enabled;
}

static void raspi4b_get_sd_overcurrent_retries(Object *obj, Visitor *v,
                                                const char *name,
                                                void *opaque, Error **errp)
{
    uint64_t value = RASPI4B_MACHINE(obj)->sd_overcurrent_retry_count;

    visit_type_uint64(v, name, &value, errp);
}

static void raspi4b_get_sd_overcurrent_remaining(Object *obj, Visitor *v,
                                                  const char *name,
                                                  void *opaque, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    uint64_t value = 0;

    if (s->pending_boot_action == RASPI4_PENDING_SD_OVERCURRENT &&
        timer_pending(s->boot_timeout_timer)) {
        uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        uint64_t expiry = timer_expire_time_ns(s->boot_timeout_timer);

        if (expiry > now) {
            value = expiry - now;
        }
    }
    visit_type_uint64(v, name, &value, errp);
}

static bool raspi4b_get_reboot_on_fatal_error(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->reboot_on_fatal_error;
}

static void raspi4b_get_fatal_error_reboot_count(Object *obj, Visitor *v,
                                                  const char *name,
                                                  void *opaque, Error **errp)
{
    uint64_t value = RASPI4B_MACHINE(obj)->fatal_error_reboot_count;

    visit_type_uint64(v, name, &value, errp);
}

static void raspi4b_get_fatal_error_remaining(Object *obj, Visitor *v,
                                               const char *name,
                                               void *opaque, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    uint64_t value = 0;

    if (s->pending_boot_action == RASPI4_PENDING_FATAL_REBOOT &&
        timer_pending(s->boot_timeout_timer)) {
        uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        uint64_t expiry = timer_expire_time_ns(s->boot_timeout_timer);

        if (expiry > now) {
            value = expiry - now;
        }
    }
    visit_type_uint64(v, name, &value, errp);
}

static void raspi4b_get_net_boot_max_retries(Object *obj, Visitor *v,
                                             const char *name, void *opaque,
                                             Error **errp)
{
    int32_t value = RASPI4B_MACHINE(obj)->net_boot_max_retries;

    visit_type_int32(v, name, &value, errp);
}

static void raspi4b_get_dhcp_timeout(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->dhcp_timeout;

    visit_type_uint32(v, name, &value, errp);
}

static void raspi4b_get_tftp_file_timeout(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->tftp_file_timeout;

    visit_type_uint32(v, name, &value, errp);
}

static void raspi4b_get_http_port(Object *obj, Visitor *v,
                                  const char *name, void *opaque,
                                  Error **errp)
{
    uint16_t value = RASPI4B_MACHINE(obj)->http_port;

    visit_type_uint16(v, name, &value, errp);
}

static void raspi4b_get_dhcp_req_timeout(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->dhcp_req_timeout;

    visit_type_uint32(v, name, &value, errp);
}

static void raspi4b_get_dhcp_option97(Object *obj, Visitor *v,
                                      const char *name, void *opaque,
                                      Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->dhcp_option97;

    visit_type_uint32(v, name, &value, errp);
}

static char *raspi4b_get_pxe_option43(Object *obj, Error **errp)
{
    return g_strdup(
        (const char *)RASPI4B_MACHINE(obj)->pxe_option43);
}

static void raspi4b_get_tftp_retransmit_count(Object *obj, Visitor *v,
                                               const char *name,
                                               void *opaque, Error **errp)
{
    uint64_t value =
        RASPI4B_MACHINE(obj)->network_tftp_retransmit_count;

    visit_type_uint64(v, name, &value, errp);
}

static void raspi4b_get_dhcp_retransmit_count(Object *obj, Visitor *v,
                                               const char *name,
                                               void *opaque, Error **errp)
{
    uint64_t value =
        RASPI4B_MACHINE(obj)->network_dhcp_retransmit_count;

    visit_type_uint64(v, name, &value, errp);
}

static void raspi4b_get_dns_retransmit_count(Object *obj, Visitor *v,
                                              const char *name,
                                              void *opaque, Error **errp)
{
    uint64_t value = RASPI4B_MACHINE(obj)->network_dns_retransmit_count;

    visit_type_uint64(v, name, &value, errp);
}

static char *raspi4b_get_firmware_file(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->firmware_file ? s->firmware_file : "none");
}

static char *raspi4b_get_firmware_status(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->firmware_status ? s->firmware_status : "none");
}

static char *raspi4b_get_wireless_status(Object *obj, Error **errp)
{
    return g_strdup(RASPI4B_MACHINE(obj)->wireless_model ?
                    "modeled-sdio-bcdc-uart-hci" : "excluded-unmodeled");
}

static char *raspi4b_get_peripheral_policy(Object *obj, Error **errp)
{
    return g_strdup(RASPI4_PERIPHERAL_POLICY);
}

static char *raspi4b_get_peripheral_exclusions(Object *obj, Error **errp)
{
    return g_strdup(RASPI4B_MACHINE(obj)->wireless_model ?
                    RASPI4_EXCLUDED_DVP :
                    RASPI4_PERIPHERAL_EXCLUSIONS);
}

static char *raspi4b_get_videocore_execution_mode(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->behavioral_boot ? "behavioral-replacement-v1" :
                                        "inactive-direct-loader");
}

static char *raspi4b_get_videocore_artifact_policy(Object *obj,
                                                    Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->behavioral_boot ?
                    "exact-input-bytes-not-instruction-executed" :
                    "not-applicable");
}

static void raspi4b_get_videocore_boundary_version(Object *obj, Visitor *v,
                                                    const char *name,
                                                    void *opaque,
                                                    Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    uint32_t value = s->behavioral_boot ?
                     RASPI4_VIDEOCORE_BOUNDARY_VERSION : 0;

    visit_type_uint32(v, name, &value, errp);
}

static const char *raspi4b_observed_network_filename(
    Raspi4bMachineState *s, Raspi4NetworkArtifactKind kind)
{
    const Raspi4NetworkArtifact *artifact;

    if (!s->boot_source || strcmp(s->boot_source, "network")) {
        return NULL;
    }
    artifact = raspi4_network_find_artifact(s, kind);
    return artifact && !artifact->missing && artifact->data ?
           raspi4_network_artifact_filename(artifact) : NULL;
}

static char *raspi4b_get_fixup_file(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    const char *observed = raspi4b_observed_network_filename(
        s, RASPI4_NETWORK_ARTIFACT_FIXUP);

    return g_strdup(observed ? observed :
                    (s->firmware_config_initialized ?
                     s->firmware_config.fixup_file : "none"));
}

static char *raspi4b_get_kernel_file(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    const char *observed = raspi4b_observed_network_filename(
        s, RASPI4_NETWORK_ARTIFACT_KERNEL);

    return g_strdup(observed ? observed :
                    (s->firmware_config_initialized ?
                     s->firmware_config.kernel_file : "none"));
}

static char *raspi4b_get_device_tree_file(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    const char *observed = raspi4b_observed_network_filename(
        s, RASPI4_NETWORK_ARTIFACT_DTB);

    return g_strdup(observed ? observed :
                    (s->firmware_config_initialized &&
                     s->firmware_config.device_tree_enabled ?
                     s->firmware_config.device_tree_file : "none"));
}

static char *raspi4b_get_cmdline_file(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    const char *observed = raspi4b_observed_network_filename(
        s, RASPI4_NETWORK_ARTIFACT_CMDLINE);

    return g_strdup(observed ? observed :
                    (s->firmware_config_initialized ?
                     s->firmware_config.cmdline_file : "none"));
}

static char *raspi4b_get_initramfs_file(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    const char *observed = raspi4b_observed_network_filename(
        s, RASPI4_NETWORK_ARTIFACT_INITRAMFS);

    return g_strdup(observed ? observed :
                    (s->firmware_config_initialized &&
                     s->firmware_config.initramfs_file ?
                     s->firmware_config.initramfs_file : "none"));
}

static char *raspi4b_get_os_prefix(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->firmware_config_initialized ?
                    s->firmware_config.os_prefix : "");
}

static char *raspi4b_get_firmware_sha256(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->firmware_sha256 ? s->firmware_sha256 : "none");
}

static char *raspi4b_get_fixup_sha256(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->fixup_sha256 ? s->fixup_sha256 : "none");
}

static char *raspi4b_get_kernel_sha256(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->kernel_sha256 ? s->kernel_sha256 : "none");
}

static char *raspi4b_get_device_tree_sha256(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->device_tree_sha256 ?
                    s->device_tree_sha256 : "none");
}

static char *raspi4b_get_cmdline_sha256(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->cmdline_sha256 ? s->cmdline_sha256 : "none");
}

static char *raspi4b_get_initramfs_sha256(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->initramfs_sha256 ? s->initramfs_sha256 : "none");
}

static void raspi4b_get_firmware_size(Object *obj, Visitor *v,
                                      const char *name, void *opaque,
                                      Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->firmware_size;

    visit_type_uint32(v, name, &value, errp);
}

static void raspi4b_get_fixup_size(Object *obj, Visitor *v,
                                   const char *name, void *opaque,
                                   Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->fixup_size;

    visit_type_uint32(v, name, &value, errp);
}

static void raspi4b_get_kernel_size(Object *obj, Visitor *v,
                                    const char *name, void *opaque,
                                    Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->kernel_size;

    visit_type_uint32(v, name, &value, errp);
}

static void raspi4b_get_device_tree_size(Object *obj, Visitor *v,
                                         const char *name, void *opaque,
                                         Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->device_tree_size;

    visit_type_uint32(v, name, &value, errp);
}

static void raspi4b_get_cmdline_size(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->cmdline_size;

    visit_type_uint32(v, name, &value, errp);
}

static void raspi4b_get_initramfs_size(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->initramfs_size;

    visit_type_uint32(v, name, &value, errp);
}

static void raspi4b_get_config_line_count(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    uint32_t value = s->firmware_config_initialized ?
                     s->firmware_config.parsed_lines : 0;

    visit_type_uint32(v, name, &value, errp);
}

static void raspi4b_get_config_include_count(Object *obj, Visitor *v,
                                             const char *name, void *opaque,
                                             Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    uint32_t value = s->firmware_config_initialized ?
                     s->firmware_config.include_count : 0;

    visit_type_uint32(v, name, &value, errp);
}

static void raspi4b_get_config_ignored_count(Object *obj, Visitor *v,
                                             const char *name, void *opaque,
                                             Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    uint32_t value = s->firmware_config_initialized ?
                     s->firmware_config.ignored_properties : 0;

    visit_type_uint32(v, name, &value, errp);
}

static void raspi4b_get_firmware_total_mem_mb(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    uint32_t value = 0;

    if (s->firmware_config_initialized) {
        value = s->firmware_config.total_mem_set ?
                s->firmware_config.total_mem_mb :
                s->firmware_config.installed_mem_mb;
    }
    visit_type_uint32(v, name, &value, errp);
}

static void raspi4b_get_firmware_gpu_mem_mb(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    uint32_t value = s->firmware_config_initialized ?
                     s->firmware_config.gpu_mem_effective_mb : 0;

    visit_type_uint32(v, name, &value, errp);
}

static char *raspi4b_get_firmware_gpu_mem_source(Object *obj, Error **errp)
{
    RaspiFirmwareConfig *config =
        &RASPI4B_MACHINE(obj)->firmware_config;

    if (config->installed_mem_mb == 256 && config->gpu_mem_256_set) {
        return g_strdup("gpu_mem_256");
    }
    if (config->installed_mem_mb == 512 && config->gpu_mem_512_set) {
        return g_strdup("gpu_mem_512");
    }
    if (config->installed_mem_mb >= 1024 && config->gpu_mem_1024_set) {
        return g_strdup("gpu_mem_1024");
    }
    return g_strdup(config->gpu_mem_set ? "gpu_mem" : "default");
}

static void raspi4b_get_firmware_bootcode_delay(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    uint32_t value = s->bootcode_delay_seconds;

    visit_type_uint32(v, name, &value, errp);
}

static void raspi4b_get_firmware_sdram_frequency(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    bool requested = GPOINTER_TO_UINT(opaque);
    uint32_t value = 0;

    if (s->firmware_config_initialized) {
        value = requested ?
                (s->firmware_config.sdram_freq_set ?
                    s->firmware_config.sdram_freq_mhz : 0) :
                RASPI_FIRMWARE_PI4_SDRAM_FREQ_MHZ;
    }
    visit_type_uint32(v, name, &value, errp);
}

static bool raspi4b_get_firmware_sdram_frequency_requested(
    Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return s->firmware_config_initialized &&
           s->firmware_config.sdram_freq_set;
}

static bool raspi4b_get_firmware_uart_enabled(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->firmware_uart_enabled;
}

static bool raspi4b_get_vl805_enabled(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->vl805_enabled;
}

static bool raspi4b_get_vl805_initialized(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->vl805_initialized;
}

static char *raspi4b_get_vl805_boot_status(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    if (!s->cm4) {
        return g_strdup("onboard");
    }
    if (!s->vl805_enabled) {
        return g_strdup("disabled");
    }
    return g_strdup(s->vl805_initialized ? "initialized" :
                                             "controller-missing");
}

static void raspi4b_get_firmware_uart_bytes(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint64_t value = RASPI4B_MACHINE(obj)->firmware_uart_bytes;

    visit_type_uint64(v, name, &value, errp);
}

static void raspi4b_get_firmware_uart_lines(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->firmware_uart_lines;

    visit_type_uint32(v, name, &value, errp);
}

static void raspi4b_get_overlay_count(Object *obj, Visitor *v,
                                      const char *name, void *opaque,
                                      Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    uint32_t value = s->firmware_config_initialized ?
                     s->firmware_config.overlays->len : 0;

    visit_type_uint32(v, name, &value, errp);
}

static void raspi4b_get_dtparam_count(Object *obj, Visitor *v,
                                      const char *name, void *opaque,
                                      Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    uint32_t value = s->firmware_config_initialized ?
                     s->firmware_config.dtparams->len : 0;

    visit_type_uint32(v, name, &value, errp);
}

static void raspi4b_get_overlay_applied_count(Object *obj, Visitor *v,
                                              const char *name, void *opaque,
                                              Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->overlay_state.overlays_applied;

    visit_type_uint32(v, name, &value, errp);
}

static void raspi4b_get_dtparam_applied_count(Object *obj, Visitor *v,
                                              const char *name, void *opaque,
                                              Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->overlay_state.parameters_applied;

    visit_type_uint32(v, name, &value, errp);
}

static char *raspi4b_get_last_overlay_file(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->overlay_state.last_overlay_file ?
                    s->overlay_state.last_overlay_file : "none");
}

static char *raspi4b_get_final_device_tree_sha256(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->overlay_state.final_dtb_sha256 ?
                    s->overlay_state.final_dtb_sha256 : "none");
}

static char *raspi4b_get_hat_gpio_status(Object *obj, Error **errp)
{
    BCM2838GpioState *gpio =
        &RASPI4B_MACHINE(obj)->soc.peripherals.gpio;

    return g_strdup(bcm2838_gpio_hat_map_applied(gpio) ?
                    "digital-applied-electrical-policy-recorded" : "none");
}

static char *raspi4b_get_hat_eeprom_status(Object *obj, Error **errp)
{
    return g_strdup(RASPI4B_MACHINE(obj)->overlay_state.hat_present ?
                    "valid" : "not-read");
}

static char *raspi4b_get_hat_eeprom_sha256(Object *obj, Error **errp)
{
    RaspiOverlayState *state = &RASPI4B_MACHINE(obj)->overlay_state;

    return g_strdup(state->hat_eeprom_sha256 ?
                    state->hat_eeprom_sha256 : "none");
}

static char *raspi4b_get_hat_content_sha256(Object *obj, Error **errp)
{
    RaspiOverlayState *state = &RASPI4B_MACHINE(obj)->overlay_state;

    return g_strdup(state->hat_content_sha256 ?
                    state->hat_content_sha256 : "none");
}

static char *raspi4b_get_hat_vendor(Object *obj, Error **errp)
{
    RaspiOverlayState *state = &RASPI4B_MACHINE(obj)->overlay_state;

    return g_strdup(state->hat_vendor ? state->hat_vendor : "none");
}

static char *raspi4b_get_hat_product(Object *obj, Error **errp)
{
    RaspiOverlayState *state = &RASPI4B_MACHINE(obj)->overlay_state;

    return g_strdup(state->hat_product ? state->hat_product : "none");
}

static char *raspi4b_get_hat_uuid(Object *obj, Error **errp)
{
    RaspiOverlayState *state = &RASPI4B_MACHINE(obj)->overlay_state;

    return g_strdup(state->hat_uuid ? state->hat_uuid : "none");
}

static char *raspi4b_get_hat_overlay(Object *obj, Error **errp)
{
    RaspiOverlayState *state = &RASPI4B_MACHINE(obj)->overlay_state;

    return g_strdup(state->hat_overlay ? state->hat_overlay : "none");
}

static void raspi4b_get_hat_eeprom_size(Object *obj, Visitor *v,
                                        const char *name, void *opaque,
                                        Error **errp)
{
    uint64_t value =
        RASPI4B_MACHINE(obj)->overlay_state.hat_eeprom_size;

    visit_type_uint64(v, name, &value, errp);
}

static void raspi4b_get_hat_identity_number(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    RaspiOverlayState *state = &RASPI4B_MACHINE(obj)->overlay_state;
    uint32_t value;

    switch (GPOINTER_TO_UINT(opaque)) {
    case 0:
        value = state->hat_eeprom_declared_size;
        break;
    case 1:
        value = state->hat_product_id;
        break;
    case 2:
        value = state->hat_product_version;
        break;
    default:
        value = state->hat_custom_count;
        break;
    }
    visit_type_uint32(v, name, &value, errp);
}

static void raspi4b_get_hat_gpio_used_mask(Object *obj, Visitor *v,
                                           const char *name, void *opaque,
                                           Error **errp)
{
    uint32_t value = bcm2838_gpio_hat_used_mask(
        &RASPI4B_MACHINE(obj)->soc.peripherals.gpio);

    visit_type_uint32(v, name, &value, errp);
}

static void raspi4b_get_hat_gpio_policy(Object *obj, Visitor *v,
                                        const char *name, void *opaque,
                                        Error **errp)
{
    BCM2838GpioState *gpio =
        &RASPI4B_MACHINE(obj)->soc.peripherals.gpio;
    uint8_t value;

    switch (GPOINTER_TO_UINT(opaque)) {
    case 0:
        value = bcm2838_gpio_hat_drive(gpio);
        break;
    case 1:
        value = bcm2838_gpio_hat_slew(gpio);
        break;
    case 2:
        value = bcm2838_gpio_hat_hysteresis(gpio);
        break;
    default:
        value = bcm2838_gpio_hat_back_power(gpio);
        break;
    }
    visit_type_uint8(v, name, &value, errp);
}

static bool raspi4b_get_config_present(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return s->firmware_config_initialized &&
           s->firmware_config.config_present;
}

static bool raspi4b_get_arm_64bit(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return s->firmware_config_initialized && s->firmware_config.arm_64bit;
}

static char *raspi4b_get_handoff_status(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->handoff_status ? s->handoff_status : "none");
}

static char *raspi4b_get_handoff_architecture(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    if (!s->handoff.kernel) {
        return g_strdup("none");
    }
    return g_strdup(s->handoff.arm_64bit ? "aarch64" : "aarch32");
}

static char *raspi4b_get_handoff_endianness(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    if (!s->handoff.kernel) {
        return g_strdup("none");
    }
    return g_strdup(s->handoff.big_endian ? "big" : "little");
}

const char *raspi4b_boot_health_name(uint8_t health)
{
    static const char * const names[] = {
        [RASPI4_HEALTH_NONE] = "none",
        [RASPI4_HEALTH_KERNEL_STARTED] = "kernel-started",
        [RASPI4_HEALTH_USERSPACE_READY] = "userspace-ready",
        [RASPI4_HEALTH_FAILED] = "failed",
    };

    return health < ARRAY_SIZE(names) ? names[health] : "invalid";
}

static void raspi4b_get_handoff_kernel_address(Object *obj, Visitor *v,
                                               const char *name, void *opaque,
                                               Error **errp)
{
    uint64_t value = RASPI4B_MACHINE(obj)->handoff.kernel_address;

    visit_type_uint64(v, name, &value, errp);
}

static void raspi4b_get_handoff_kernel_size(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    uint64_t value = RASPI4B_MACHINE(obj)->handoff.kernel_size;

    visit_type_uint64(v, name, &value, errp);
}

static void raspi4b_get_handoff_entry_address(Object *obj, Visitor *v,
                                              const char *name, void *opaque,
                                              Error **errp)
{
    uint64_t value = RASPI4B_MACHINE(obj)->handoff.entry_address;

    visit_type_uint64(v, name, &value, errp);
}

static void raspi4b_get_handoff_device_tree_address(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint64_t value = RASPI4B_MACHINE(obj)->handoff.device_tree_address;

    visit_type_uint64(v, name, &value, errp);
}

static void raspi4b_get_handoff_device_tree_size(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint64_t value = RASPI4B_MACHINE(obj)->handoff.device_tree_size;

    visit_type_uint64(v, name, &value, errp);
}

static void raspi4b_get_handoff_initramfs_address(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint64_t value = RASPI4B_MACHINE(obj)->handoff.initramfs_address;

    visit_type_uint64(v, name, &value, errp);
}

static void raspi4b_get_handoff_core_mask(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    uint8_t value = RASPI4B_MACHINE(obj)->handoff_core_mask;

    visit_type_uint8(v, name, &value, errp);
}

static void raspi4b_get_reset_status(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->reset_status;

    visit_type_uint32(v, name, &value, errp);
}

static void raspi4b_get_selected_boot_partition(Object *obj, Visitor *v,
                                                const char *name,
                                                void *opaque, Error **errp)
{
    uint8_t value = RASPI4B_MACHINE(obj)->selected_boot_partition;

    visit_type_uint8(v, name, &value, errp);
}

static bool raspi4b_get_partition_walk(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->partition_walk;
}

static void raspi4b_get_selected_boot_mode(Object *obj, Visitor *v,
                                           const char *name, void *opaque,
                                           Error **errp)
{
    uint8_t value = RASPI4B_MACHINE(obj)->selected_boot_mode;

    visit_type_uint8(v, name, &value, errp);
}

static char *raspi4b_get_reset_cause(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->reset_cause ? s->reset_cause : "uninitialized");
}

static bool raspi4b_get_wake_on_gpio(Object *obj, Error **errp)
{
    return raspi4_powermgt(RASPI4B_MACHINE(obj))->wake_on_gpio;
}

static bool raspi4b_get_power_off_on_halt(Object *obj, Error **errp)
{
    return raspi4_powermgt(RASPI4B_MACHINE(obj))->power_off_on_halt;
}

static char *raspi4b_get_halt_state(Object *obj, Error **errp)
{
    return g_strdup(bcm2835_powermgt_halt_state(
        raspi4_powermgt(RASPI4B_MACHINE(obj))));
}

static bool raspi4b_get_global_en(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return s->soc_initialized ? raspi4_powermgt(s)->global_en :
                                s->global_en;
}

static void raspi4b_set_global_en(Object *obj, bool value, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    s->global_en = value;
    if (s->soc_initialized) {
        bcm2835_powermgt_global_en_input(raspi4_powermgt(s), value);
    }
}

static void raspi4b_get_board_revision(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->board_revision;

    visit_type_uint32(v, name, &value, errp);
}

static void raspi4b_set_board_revision(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (s->board_revision) {
        error_setg(errp, "board-revision cannot change after realization");
        return;
    }
    s->configured_board_revision = value;
}

static void raspi4b_get_min_boot_version(Object *obj, Visitor *v,
                                         const char *name, void *opaque,
                                         Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->min_boot_version;

    visit_type_uint32(v, name, &value, errp);
}

static void raspi4b_set_min_boot_version(Object *obj, Visitor *v,
                                         const char *name, void *opaque,
                                         Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (s->soc_initialized) {
        error_setg(errp,
                   "min-boot-version cannot change after realization");
        return;
    }
    s->min_boot_version = value;
}

static char *raspi4b_get_memory_model(Object *obj, Error **errp)
{
    MachineState *machine = MACHINE(obj);

    return g_strdup_printf("%" PRIu64 "GiB", machine->ram_size / GiB);
}

static bool raspi4_board_revision_for_ram(uint32_t base_revision,
                                          uint64_t ram_size,
                                          uint32_t *board_revision)
{
    uint32_t memory_code;

    switch (ram_size) {
    case 1 * GiB:
        memory_code = 2;
        break;
    case 2 * GiB:
        memory_code = 3;
        break;
    case 4 * GiB:
        memory_code = 4;
        break;
    case 8 * GiB:
        memory_code = 5;
        break;
    default:
        return false;
    }
#if HOST_LONG_BITS == 32
    if (ram_size > 1 * GiB) {
        return false;
    }
#endif
    *board_revision = (base_revision & ~RASPI4_REV_MEMORY_MASK) |
                      (memory_code << RASPI4_REV_MEMORY_SHIFT);
    return true;
}

/*
 * Add second memory region if board RAM amount exceeds VC base address
 * (see https://datasheets.raspberrypi.com/bcm2711/bcm2711-peripherals.pdf
 * 1.2 Address Map)
 */
static void raspi_add_memory_node(void *fdt, hwaddr mem_base, hwaddr mem_len)
{
    uint32_t acells, scells;
    char *nodename = g_strdup_printf("/memory@%" PRIx64, mem_base);

    acells = qemu_fdt_getprop_cell(fdt, "/", "#address-cells",
                                   NULL, &error_fatal);
    scells = qemu_fdt_getprop_cell(fdt, "/", "#size-cells",
                                   NULL, &error_fatal);
    /* validated by arm_load_dtb */
    g_assert(acells && scells);

    qemu_fdt_add_subnode(fdt, nodename);
    qemu_fdt_setprop_string(fdt, nodename, "device_type", "memory");
    qemu_fdt_setprop_sized_cells(fdt, nodename, "reg",
                                        acells, mem_base,
                                        scells, mem_len);

    g_free(nodename);
}

static void raspi4_enable_pcie_dtb(void *fdt)
{
    g_autoptr(GPtrArray) paths = g_ptr_array_new_with_free_func(g_free);
    int offset = -1;

    while ((offset = fdt_node_offset_by_compatible(
                fdt, offset, "brcm,bcm2711-pcie")) >= 0) {
        char path[256];

        if (!fdt_get_path(fdt, offset, path, sizeof(path))) {
            g_ptr_array_add(paths, g_strdup(path));
        }
    }

    for (size_t i = 0; i < paths->len; i++) {
        static const char * const props[] = {
            "brcm,enable-ssc",
        };
        const char *path = g_ptr_array_index(paths, i);

        for (size_t j = 0; j < ARRAY_SIZE(props); j++) {
            offset = fdt_path_offset(fdt, path);
            if (offset >= 0) {
                fdt_delprop(fdt, offset, props[j]);
            }
        }
    }
}

static void raspi4_disable_dtb_compatible(void *fdt, const char *compatible)
{
    g_autoptr(GPtrArray) paths = g_ptr_array_new_with_free_func(g_free);
    int offset = -1;

    while ((offset = fdt_node_offset_by_compatible(
                fdt, offset, compatible)) >= 0) {
        char path[256];

        if (!fdt_get_path(fdt, offset, path, sizeof(path))) {
            g_ptr_array_add(paths, g_strdup(path));
        }
    }
    for (size_t i = 0; i < paths->len; i++) {
        const char *path = g_ptr_array_index(paths, i);

        offset = fdt_path_offset(fdt, path);
        if (offset >= 0 &&
            fdt_setprop_string(fdt, offset, "status", "disabled") < 0) {
            warn_report("bcm2711 dtb: cannot disable unmodeled %s at %s",
                        compatible, path);
        }
    }
}

static void raspi4_modify_dtb(const struct arm_boot_info *info, void *fdt)
{
    static const char * const excluded_compatibles[] = {
        RASPI4_EXCLUDED_DVP,
        RASPI4_EXCLUDED_WIFI,
        RASPI4_EXCLUDED_BLUETOOTH,
    };
    Raspi4bMachineState *s = RASPI4B_MACHINE(qdev_get_machine());
    uint64_t ram_size;

    raspi4_enable_pcie_dtb(fdt);
    /*
     * Preserve the production nodes so software can distinguish an explicitly
     * excluded device from a firmware-generated tree that never described it.
     * This pass runs after overlays, so an overlay cannot silently advertise
     * an endpoint for which this machine has no behavioral model.
     */
    for (int i = 0; i < ARRAY_SIZE(excluded_compatibles); i++) {
        if (s->wireless_model &&
            (!strcmp(excluded_compatibles[i], RASPI4_EXCLUDED_WIFI) ||
             !strcmp(excluded_compatibles[i],
                     RASPI4_EXCLUDED_BLUETOOTH))) {
            continue;
        }
        raspi4_disable_dtb_compatible(fdt, excluded_compatibles[i]);
    }

    /*
     * The CM4 eMMC supplies are controlled through firmware GPIOs which are
     * not yet modelled.  Leaving the phandles in a production DTB makes Linux
     * defer the otherwise implemented EMMC2 controller forever.
     */
    {
        int offset = fdt_node_offset_by_compatible(
            fdt, -1, "brcm,bcm2711-emmc2");

        while (offset >= 0) {
            fdt_delprop(fdt, offset, "vqmmc-supply");
            fdt_delprop(fdt, offset, "vmmc-supply");
            offset = fdt_node_offset_by_compatible(
                fdt, offset, "brcm,bcm2711-emmc2");
        }
    }

    ram_size = board_ram_size(info->board_id);

    if (ram_size > UPPER_RAM_BASE) {
        uint64_t middle_end = MIN(ram_size,
                                  (uint64_t)BCM2838_PERI_LOW_BASE);

        raspi_add_memory_node(fdt, UPPER_RAM_BASE,
                              middle_end - UPPER_RAM_BASE);
    }
    if (ram_size > BCM2838_PERI_LOW_BASE) {
        raspi_add_memory_node(fdt, 4 * GiB,
                              ram_size - BCM2838_PERI_LOW_BASE);
    }
}

static void raspi4_vl805_class_init(ObjectClass *oc, const void *data)
{
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);

    pc->vendor_id = PCI_VENDOR_ID_VIA;
    pc->device_id = 0x3483;
    pc->revision = 0x01;
    pc->subsystem_vendor_id = PCI_VENDOR_ID_VIA;
    pc->subsystem_id = 0x3483;
}

static const TypeInfo raspi4_vl805_type = {
    .name = TYPE_RASPI4_VL805,
    .parent = TYPE_QEMU_XHCI,
    .class_init = raspi4_vl805_class_init,
};

static void raspi4_pcie_root_port_class_init(ObjectClass *oc,
                                              const void *data)
{
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);

    pc->vendor_id = RASPI4_PCIE_VENDOR_ID;
    pc->device_id = RASPI4_PCIE_DEVICE_ID;
    pc->revision = RASPI4_PCIE_REVISION;
    dc->desc = "BCM2711 PCIe Root Port";
}

static const TypeInfo raspi4_pcie_root_port_type = {
    .name = TYPE_RASPI4_PCIE_ROOT_PORT,
    .parent = "pcie-root-port",
    .class_init = raspi4_pcie_root_port_class_init,
};

void raspi4_pcie_msi_update(Raspi4bMachineState *s)
{
    uint32_t status =
        s->pcie_regs[RASPI4_PCIE_MSI_INTR_STATUS / sizeof(uint32_t)];
    uint32_t mask =
        s->pcie_regs[RASPI4_PCIE_MSI_INTR_MASK_SET / sizeof(uint32_t)];

    qemu_set_irq(s->pcie_msi_irq, !!(status & ~mask));
}

static uint64_t raspi4_pcie_msi_doorbell_read(void *opaque, hwaddr addr,
                                              unsigned int size)
{
    return 0;
}

static void raspi4_pcie_msi_doorbell_write(Raspi4bMachineState *s,
                                           hwaddr addr, uint64_t value,
                                           unsigned int size,
                                           uint64_t doorbell_target)
{
    uint32_t bar_lo =
        s->pcie_regs[RASPI4_PCIE_MSI_BAR_CONFIG_LO / sizeof(uint32_t)];
    uint32_t bar_hi =
        s->pcie_regs[RASPI4_PCIE_MSI_BAR_CONFIG_HI / sizeof(uint32_t)];
    uint32_t data_config =
        s->pcie_regs[RASPI4_PCIE_MSI_DATA_CONFIG / sizeof(uint32_t)];
    uint64_t target = ((uint64_t)bar_hi << 32) | (bar_lo & ~1U);
    uint32_t data = value;
    uint32_t vector;

    if (addr || size != sizeof(uint32_t) || !(bar_lo & 1) ||
        target != doorbell_target ||
        (data & (UINT16_MAX & ~RASPI4_PCIE_MSI_DATA_MASK)) !=
        (data_config & (UINT16_MAX & ~RASPI4_PCIE_MSI_DATA_MASK))) {
        return;
    }

    vector = data & RASPI4_PCIE_MSI_DATA_MASK;
    s->pcie_regs[RASPI4_PCIE_MSI_INTR_STATUS / sizeof(uint32_t)] |=
        BIT(vector);
    raspi4_pcie_msi_update(s);
}

static void raspi4_pcie_msi_doorbell_low_write(void *opaque, hwaddr addr,
                                               uint64_t value,
                                               unsigned int size)
{
    raspi4_pcie_msi_doorbell_write(opaque, addr, value, size,
                                   RASPI4_PCIE_MSI_TARGET_LOW);
}

static void raspi4_pcie_msi_doorbell_high_write(void *opaque, hwaddr addr,
                                                uint64_t value,
                                                unsigned int size)
{
    raspi4_pcie_msi_doorbell_write(opaque, addr, value, size,
                                   RASPI4_PCIE_MSI_TARGET_HIGH);
}

static const MemoryRegionOps raspi4_pcie_msi_doorbell_low_ops = {
    .read = raspi4_pcie_msi_doorbell_read,
    .write = raspi4_pcie_msi_doorbell_low_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = sizeof(uint32_t),
        .max_access_size = sizeof(uint32_t),
    },
};

static const MemoryRegionOps raspi4_pcie_msi_doorbell_high_ops = {
    .read = raspi4_pcie_msi_doorbell_read,
    .write = raspi4_pcie_msi_doorbell_high_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = sizeof(uint32_t),
        .max_access_size = sizeof(uint32_t),
    },
};

static uint64_t raspi4_pcie_register_read(void *opaque, hwaddr addr,
                                          unsigned int size)
{
    Raspi4bMachineState *s = opaque;
    uint64_t value = 0;

    if (addr < 0x1000) {
        memory_region_dispatch_read(s->pcie_ecam, addr, &value,
                                    size_memop(size) | MO_LE,
                                    MEMTXATTRS_UNSPECIFIED);
        return value;
    }
    if (addr >= 0x8000 && addr + size <= 0x9000) {
        memory_region_dispatch_read(
            s->pcie_ecam, s->pcie_ext_cfg_index + addr - 0x8000,
            &value, size_memop(size) | MO_LE, MEMTXATTRS_UNSPECIFIED);
        return value;
    }
    switch (addr) {
    case 0x4068:
        return 0xb0;
    case 0x406c:
        return 0x0303;
    case RASPI4_PCIE_MSI_INTR_STATUS:
        return s->pcie_regs[
            RASPI4_PCIE_MSI_INTR_STATUS / sizeof(uint32_t)];
    case RASPI4_PCIE_MSI_INTR_MASK_SET:
    case RASPI4_PCIE_MSI_INTR_MASK_CLR:
        return s->pcie_regs[
            RASPI4_PCIE_MSI_INTR_MASK_SET / sizeof(uint32_t)];
    case 0x9000:
        return s->pcie_ext_cfg_index;
    default:
        if (!(addr & 3) && addr + sizeof(uint32_t) <=
            RASPI4_PCIE_REG_SIZE) {
            return s->pcie_regs[addr / sizeof(uint32_t)];
        }
        return 0;
    }
}

static void raspi4_pcie_register_write(void *opaque, hwaddr addr,
                                       uint64_t value, unsigned int size)
{
    Raspi4bMachineState *s = opaque;

    if (addr < 0x1000) {
        memory_region_dispatch_write(s->pcie_ecam, addr, value,
                                     size_memop(size) | MO_LE,
                                     MEMTXATTRS_UNSPECIFIED);
        return;
    }
    if (addr >= 0x8000 && addr + size <= 0x9000) {
        memory_region_dispatch_write(
            s->pcie_ecam, s->pcie_ext_cfg_index + addr - 0x8000,
            value, size_memop(size) | MO_LE, MEMTXATTRS_UNSPECIFIED);
        return;
    }
    if (addr == 0x9000 && size == sizeof(uint32_t)) {
        s->pcie_ext_cfg_index = value;
        return;
    }
    if (size == sizeof(uint32_t)) {
        switch (addr) {
        case RASPI4_PCIE_MSI_INTR_CLR:
            s->pcie_regs[
                RASPI4_PCIE_MSI_INTR_STATUS / sizeof(uint32_t)] &= ~value;
            raspi4_pcie_msi_update(s);
            return;
        case RASPI4_PCIE_MSI_INTR_MASK_SET:
            s->pcie_regs[
                RASPI4_PCIE_MSI_INTR_MASK_SET / sizeof(uint32_t)] |= value;
            raspi4_pcie_msi_update(s);
            return;
        case RASPI4_PCIE_MSI_INTR_MASK_CLR:
            s->pcie_regs[
                RASPI4_PCIE_MSI_INTR_MASK_SET / sizeof(uint32_t)] &= ~value;
            raspi4_pcie_msi_update(s);
            return;
        case RASPI4_PCIE_MSI_INTR_STATUS:
            return;
        default:
            break;
        }
    }
    if (!(addr & 3) && size == sizeof(uint32_t) &&
        addr + size <= RASPI4_PCIE_REG_SIZE) {
        s->pcie_regs[addr / sizeof(uint32_t)] = value;
    }
}

static const MemoryRegionOps raspi4_pcie_register_ops = {
    .read = raspi4_pcie_register_read,
    .write = raspi4_pcie_register_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void raspi4_create_pcie(Raspi4bMachineState *s)
{
    DeviceState *host = qdev_new(TYPE_GPEX_HOST);
    SysBusDevice *sbd = SYS_BUS_DEVICE(host);
    MemoryRegion *mmio;
    DeviceState *root_port;

    qdev_prop_set_int32(DEVICE(&GPEX_HOST(host)->gpex_root), "addr",
                        PCI_DEVFN(31, 0));
    sysbus_realize_and_unref(sbd, &error_fatal);
    s->pcie_ecam = sysbus_mmio_get_region(sbd, 0);
    memory_region_init_io(&s->pcie_registers, OBJECT(s),
                          &raspi4_pcie_register_ops, s,
                          "raspi4-pcie-registers",
                          RASPI4_PCIE_REG_SIZE);
    memory_region_add_subregion(get_system_memory(), RASPI4_PCIE_ECAM_BASE,
                                &s->pcie_registers);

    mmio = sysbus_mmio_get_region(sbd, 1);
    memory_region_init_alias(&s->pcie_mmio_alias, OBJECT(s),
                             "raspi4-pcie-mmio", mmio,
                             RASPI4_PCIE_MMIO_PCI_BASE,
                             RASPI4_PCIE_MMIO_SIZE);
    memory_region_add_subregion(get_system_memory(), RASPI4_PCIE_MMIO_BASE,
                                &s->pcie_mmio_alias);
    memory_region_init_io(&s->pcie_msi_doorbell_low, OBJECT(s),
                          &raspi4_pcie_msi_doorbell_low_ops, s,
                          "raspi4-pcie-msi-doorbell-low",
                          sizeof(uint32_t));
    memory_region_add_subregion(get_system_memory(),
                                RASPI4_PCIE_MSI_TARGET_LOW,
                                &s->pcie_msi_doorbell_low);
    memory_region_init_io(&s->pcie_msi_doorbell_high, OBJECT(s),
                          &raspi4_pcie_msi_doorbell_high_ops, s,
                          "raspi4-pcie-msi-doorbell-high",
                          sizeof(uint32_t));
    memory_region_add_subregion(get_system_memory(),
                                RASPI4_PCIE_MSI_TARGET_HIGH,
                                &s->pcie_msi_doorbell_high);
    s->pcie_msi_irq = qdev_get_gpio_in(
        DEVICE(&s->soc), GIC_SPI_INTERRUPT_PCI_MSI);

    for (int i = 0; i < PCI_NUM_PINS; i++) {
        int irq = GIC_SPI_INTERRUPT_PCI_INT_A + i;

        sysbus_connect_irq(sbd, i,
                           qdev_get_gpio_in(DEVICE(&s->soc), irq));
        gpex_set_irq_num(GPEX_HOST(host), i, irq);
    }

    root_port = qdev_new(TYPE_RASPI4_PCIE_ROOT_PORT);
    root_port->id = g_strdup("pcie-root");
    qdev_prop_set_int32(root_port, "addr", PCI_DEVFN(0, 0));
    qdev_prop_set_uint8(root_port, "chassis", 1);
    qdev_prop_set_uint16(root_port, "port", 0);
    qdev_prop_set_enum(root_port, "x-speed", PCIE_LINK_SPEED_5);
    qdev_prop_set_enum(root_port, "x-width", PCIE_LINK_WIDTH_1);
    qdev_realize_and_unref(root_port, BUS(PCI_HOST_BRIDGE(host)->bus),
                           &error_fatal);
    pci_default_write_config(
        PCI_DEVICE(root_port), PCI_COMMAND,
        PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER,
        sizeof(uint16_t));
    s->pcie_bus = pci_bridge_get_sec_bus(PCI_BRIDGE(root_port));
    if (s->usb_boot_on_xhci &&
        (s->usb_boot_device_count || s->usb_boot_external) &&
        s->nvme) {
        error_report("VL805 USB boot and NVMe cannot share the single "
                     "BCM2711 PCIe root port");
        exit(1);
    }
    if ((!s->cm4 && !s->nvme) ||
        (s->usb_boot_on_xhci &&
         (s->usb_boot_device_count || s->usb_boot_external))) {
        DeviceState *vl805 = qdev_new(TYPE_RASPI4_VL805);

        vl805->id = g_strdup("vl805");
        s->vl805_xhci = &XHCI_PCI(vl805)->xhci;
        qdev_prop_set_uint32(vl805, "p2", 2);
        qdev_prop_set_uint32(vl805, "p3", 2);
        object_property_set_str(OBJECT(vl805), "msi", "on", &error_fatal);
        object_property_set_str(OBJECT(vl805), "msix", "off", &error_fatal);
        qdev_realize_and_unref(vl805, BUS(s->pcie_bus), &error_fatal);
        bcm2835_property_set_xhci(
            raspi4_property(s), DEVICE(s->vl805_xhci));
        /*
         * BCM2711's inbound PCIe window exposes system RAM to VL805.  The
         * generic GPEX bus-master container is populated only after machine
         * initialization, but EEPROM boot runs before that notifier.
         */
        s->vl805_xhci->as = &address_space_memory;
        pci_default_write_config(
            PCI_DEVICE(vl805), PCI_COMMAND,
            PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER,
            sizeof(uint16_t));
    }
    if (s->nvme) {
        DeviceState *nvme = qdev_new("nvme");

        qdev_prop_set_drive(nvme, "drive", s->nvme);
        qdev_prop_set_string(nvme, "serial", "QEMU-RPI-NVME");
        qdev_realize_and_unref(nvme, BUS(s->pcie_bus), &error_fatal);
    }
}

static bool raspi4_resolve_usb_boot_devices(Raspi4bMachineState *s)
{
    g_auto(GStrv) groups = NULL;

    if (s->usb_boot_external &&
        (s->usb_boot_drive || s->usb_boot_drives)) {
        error_report("usb-boot-external cannot be combined with "
                     "usb-boot-drive or usb-boot-drives");
        return false;
    }
    if (s->usb_boot_drive && s->usb_boot_drives) {
        error_report("usb-boot-drive and usb-boot-drives are mutually "
                     "exclusive");
        return false;
    }
    if (s->usb_boot_drive) {
        s->usb_boot = blk_by_name(s->usb_boot_drive);
        if (!s->usb_boot) {
            error_report("USB boot block backend '%s' was not found",
                         s->usb_boot_drive);
            return false;
        }
        s->usb_boot_devices[0] = s->usb_boot;
        s->usb_boot_device_count = 1;
        s->usb_boot_group_count = 1;
        return true;
    }
    if (!s->usb_boot_drives) {
        return true;
    }

    groups = g_strsplit(s->usb_boot_drives, ":", -1);
    for (char **groupp = groups; *groupp; groupp++) {
        g_auto(GStrv) luns = NULL;
        char *group = g_strstrip(*groupp);
        unsigned int lun = 0;

        if (!group[0]) {
            error_report("usb-boot-drives contains an empty device");
            return false;
        }
        if (s->usb_boot_group_count == RASPI4_USB_BOOT_MAX_DEVICES) {
            error_report("usb-boot-drives supports at most %u USB devices",
                         RASPI4_USB_BOOT_MAX_DEVICES);
            return false;
        }
        luns = g_strsplit(group, "+", -1);
        for (char **lunp = luns; *lunp; lunp++, lun++) {
            const char *name = g_strstrip(*lunp);
            BlockBackend *blk;
            unsigned int index = s->usb_boot_device_count;

            if (!name[0]) {
                error_report("usb-boot-drives contains an empty LUN");
                return false;
            }
            if (lun > 15) {
                error_report("usb-boot-drives supports LUNs 0 through 15");
                return false;
            }
            if (index == RASPI4_USB_BOOT_MAX_DEVICES) {
                error_report("usb-boot-drives supports at most %u LUNs",
                             RASPI4_USB_BOOT_MAX_DEVICES);
                return false;
            }
            blk = blk_by_name(name);
            if (!blk) {
                error_report("USB boot block backend '%s' was not found",
                             name);
                return false;
            }
            for (unsigned int i = 0; i < index; i++) {
                if (s->usb_boot_devices[i] == blk) {
                    error_report("USB boot block backend '%s' is duplicated",
                                 name);
                    return false;
                }
            }
            s->usb_boot_devices[index] = blk;
            s->usb_boot_device_numbers[index] = s->usb_boot_group_count;
            s->usb_boot_luns[index] = lun;
            s->usb_boot_device_count++;
        }
        s->usb_boot_group_count++;
    }
    s->usb_boot = s->usb_boot_devices[0];
    return true;
}

static void raspi4b_machine_init(MachineState *machine)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(machine);
    RaspiBaseMachineState *s_base = RASPI_BASE_MACHINE(machine);
    RaspiBaseMachineClass *mc = RASPI_BASE_MACHINE_GET_CLASS(machine);
    BCM2838State *soc = &s->soc;
    uint32_t derived_revision;

    if (!raspi4_board_revision_for_ram(mc->board_rev, machine->ram_size,
                                       &derived_revision)) {
        g_autofree char *size = size_to_str(machine->ram_size);

        error_report("unsupported %s RAM size %s; use -m 1G, 2G, 4G, or 8G%s",
                     s->cm4 ? "CM4" : "Pi 4B", size,
#if HOST_LONG_BITS == 32
                     " (32-bit hosts support only 1G)"
#else
                     ""
#endif
        );
        exit(1);
    }
    if (s->configured_board_revision) {
        uint32_t ram_revision;
        uint32_t expected_type = s->cm4 ? 0x14 : 0x11;

        if (!(s->configured_board_revision & BIT(23)) ||
            ((s->configured_board_revision >> 12) & 0xf) != 3 ||
            ((s->configured_board_revision >> 4) & 0xff) != expected_type ||
            !raspi4_board_revision_for_ram(
                s->configured_board_revision, machine->ram_size,
                &ram_revision) ||
            ram_revision != s->configured_board_revision) {
            error_report(
                "board-revision 0x%08x is not a valid %s revision for "
                "the selected RAM size",
                s->configured_board_revision, s->cm4 ? "CM4" : "Pi 4B");
            exit(1);
        }
        s->board_revision = s->configured_board_revision;
    } else {
        s->board_revision = derived_revision;
    }
    s_base->binfo.modify_dtb = raspi4_modify_dtb;
    s_base->binfo.board_id = s->board_revision;

    if (s->behavioral_boot &&
        (machine->kernel_filename || machine->dtb || machine->initrd_filename ||
         machine->firmware)) {
        error_report("behavioral boot does not accept -kernel, -dtb, -initrd, "
                     "or -bios; boot artifacts must come from modeled media");
        exit(1);
    }
    if (s->provision_state_file && (!s->cm4 || !s->behavioral_boot)) {
        error_report("provision-state-file requires raspi-cm4 behavioral boot");
        exit(1);
    }
    if (s->provision_recover_stale && !s->provision_state_file) {
        error_report("provision-recover-stale requires provision-state-file");
        exit(1);
    }
    if (s->provision_state_file) {
        if (!raspi4_provision_lock(s)) {
            exit(1);
        }
        s->provision_exit_notifier.notify = raspi4_provision_exit_notify;
        qemu_add_exit_notifier(&s->provision_exit_notifier);
        s->provision_exit_registered = true;
    }

    object_initialize_child(OBJECT(machine), "soc", soc,
                            board_soc_type(s->board_revision));
    object_property_set_int(
        OBJECT(&soc->peripherals.thermal2711),
        "temperature-millicelsius",
        s->thermal_temperature_millicelsius, &error_abort);
    object_property_set_bool(
        OBJECT(&soc->peripherals.thermal2711), "sensor-valid",
        s->thermal_sensor_valid, &error_abort);
    bcm2835_property_set_throttled_current(
        &soc->peripherals.parent_obj.property,
        s->firmware_throttled_initial);
    s->soc_initialized = true;
    bcm2835_powermgt_global_en_input(raspi4_powermgt(s), s->global_en);
    if (s->gpio_chardev) {
        object_property_set_str(OBJECT(&soc->peripherals.gpio), "chardev",
                                s->gpio_chardev, &error_fatal);
    }
    object_property_set_uint(OBJECT(raspi4_otp(s)), "board-rev",
                             s->board_revision, &error_abort);

    if (s->otp_drive) {
        s->otp = blk_by_name(s->otp_drive);
        if (!s->otp) {
            error_report("OTP block backend '%s' was not found",
                         s->otp_drive);
            exit(1);
        }
        qdev_prop_set_drive(DEVICE(raspi4_otp(s)), "drive", s->otp);
    }

    if (s->emmc_drive) {
        if (!s->cm4) {
            error_report("emmc-drive is only valid for raspi-cm4");
            exit(1);
        }
        if (drive_get(IF_SD, 0, 0)) {
            error_report("emmc-drive and an if=sd drive are mutually "
                         "exclusive");
            exit(1);
        }
        s->sd = blk_by_name(s->emmc_drive);
        if (!s->sd) {
            error_report("eMMC block backend '%s' was not found",
                         s->emmc_drive);
            exit(1);
        }
        if (s->provision_state_file && !s->nrpiboot) {
            g_autofree char *state = raspi4_provision_read(s);

            if (state && !strcmp(state, "qemu-owned") &&
                s->provision_recover_stale) {
                if (!raspi4_provision_write(s, "qemu-stopped")) {
                    exit(1);
                }
                s->provision_stale_recovered = true;
                g_clear_pointer(&state, g_free);
                state = g_strdup("qemu-stopped");
            }
            if (!state || (strcmp(state, "boot-ready") &&
                           strcmp(state, "qemu-stopped"))) {
                error_report("CM4 eMMC ownership state must be boot-ready or "
                             "qemu-stopped before boot (found '%s')",
                             state ? state : "unreadable");
                exit(1);
            }
            if (!raspi4_provision_write(s, "qemu-owned")) {
                exit(1);
            }
            s->provision_emmc_owned = true;
        }
    } else {
        DriveInfo *storage_di = drive_get(IF_SD, 0, 0);

        s->sd = storage_di ? blk_by_legacy_dinfo(storage_di) : NULL;
    }

    if (s->emmc_boot_drive || s->emmc_rpmb_drive) {
        if (!s->cm4 || !s->emmc_drive) {
            error_report("eMMC hidden partition drives require raspi-cm4 "
                         "with emmc-drive");
            exit(1);
        }
        if (s->emmc_boot_drive) {
            s->emmc_boot = blk_by_name(s->emmc_boot_drive);
            if (!s->emmc_boot) {
                error_report("eMMC boot partition backend '%s' was not found",
                             s->emmc_boot_drive);
                exit(1);
            }
        }
        if (s->emmc_rpmb_drive) {
            s->emmc_rpmb = blk_by_name(s->emmc_rpmb_drive);
            if (!s->emmc_rpmb) {
                error_report("eMMC RPMB backend '%s' was not found",
                             s->emmc_rpmb_drive);
                exit(1);
            }
        }
    }

    if (s->eeprom_status_drive && !s->behavioral_boot) {
        error_report("eeprom-status-drive requires behavioral boot");
        exit(1);
    }
    if (s->hat_eeprom_drive && !s->behavioral_boot) {
        error_report("hat-eeprom-drive requires behavioral boot");
        exit(1);
    }
    if (s->behavioral_boot) {
        uint8_t eeprom_status[RASPI4_EEPROM_STATUS_SIZE];

        /* The VideoCore first stage owns reset; hold every ARM core for now. */
        object_property_set_uint(OBJECT(soc), "enabled-cpus", 0, &error_abort);
        if (s->eeprom_drive) {
            s->eeprom = blk_by_name(s->eeprom_drive);
            if (!s->eeprom) {
                error_report("EEPROM block backend '%s' was not found",
                             s->eeprom_drive);
                exit(1);
            }
            if (blk_set_perm(s->eeprom,
                             BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                             BLK_PERM_ALL, &error_fatal) < 0) {
                return;
            }
        }
        if (s->hat_eeprom_drive) {
            int64_t hat_length;

            s->hat_eeprom = blk_by_name(s->hat_eeprom_drive);
            if (!s->hat_eeprom) {
                error_report("HAT EEPROM block backend '%s' was not found",
                             s->hat_eeprom_drive);
                exit(1);
            }
            if (blk_set_perm(s->hat_eeprom, BLK_PERM_CONSISTENT_READ,
                             BLK_PERM_ALL, &error_fatal) < 0) {
                return;
            }
            hat_length = blk_getlength(s->hat_eeprom);
            if (hat_length < 12 ||
                hat_length > RASPI_HAT_EEPROM_MAX_SIZE) {
                error_report("HAT EEPROM backend must be 12 bytes to 1 MiB");
                exit(1);
            }
        }
        if (s->eeprom_status_drive) {
            int64_t status_length;

            if (!s->eeprom) {
                error_report("eeprom-status-drive requires eeprom-drive");
                exit(1);
            }
            s->eeprom_status = blk_by_name(s->eeprom_status_drive);
            if (!s->eeprom_status) {
                error_report("EEPROM status block backend '%s' was not found",
                             s->eeprom_status_drive);
                exit(1);
            }
            if (blk_set_perm(s->eeprom_status,
                             BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                             BLK_PERM_ALL, &error_fatal) < 0) {
                return;
            }
            status_length = blk_getlength(s->eeprom_status);
            if (status_length != sizeof(eeprom_status) ||
                blk_pread(s->eeprom_status, 0, sizeof(eeprom_status),
                          eeprom_status, 0) < 0 ||
                eeprom_status[RASPI4_EEPROM_STATUS_WP_OFFSET] > 1 ||
                eeprom_status[
                    RASPI4_EEPROM_STATUS_UPDATE_VALID_OFFSET] > 1 ||
                (!eeprom_status[
                     RASPI4_EEPROM_STATUS_UPDATE_VALID_OFFSET] &&
                 ldl_le_p(eeprom_status +
                          RASPI4_EEPROM_STATUS_UPDATE_TIMESTAMP_OFFSET)) ||
                !buffer_is_zero(
                    eeprom_status + RASPI4_EEPROM_STATUS_METADATA_SIZE,
                    sizeof(eeprom_status) -
                    RASPI4_EEPROM_STATUS_METADATA_SIZE)) {
                error_report("EEPROM status backend must be one 512-byte "
                             "sector containing write-protect and optional "
                             "update-timestamp metadata followed by zeros");
                exit(1);
            }
            s->eeprom_write_protect =
                eeprom_status[RASPI4_EEPROM_STATUS_WP_OFFSET];
            s->eeprom_update_timestamp_valid =
                eeprom_status[RASPI4_EEPROM_STATUS_UPDATE_VALID_OFFSET];
            s->eeprom_update_timestamp = ldl_le_p(
                eeprom_status +
                RASPI4_EEPROM_STATUS_UPDATE_TIMESTAMP_OFFSET);
            s->eeprom_status_loaded = true;
        }
    }

    if (!raspi4_resolve_usb_boot_devices(s)) {
        exit(1);
    }
    if ((s->usb_boot_bot_stall_count ?
         s->usb_boot_bot_stall_count : s->usb_boot_bot_stall_once) +
        s->usb_boot_bot_phase_count +
        raspi4_usb_bot_case_recovery_count(s->usb_boot_bot_case_count) >
        RASPI4_USB_BOT_MAX_RECOVERIES) {
        error_report("combined USB BOT fault count exceeds the recovery "
                     "limit of %u", RASPI4_USB_BOT_MAX_RECOVERIES);
        exit(1);
    }

    if (s->network_boot_drive) {
        s->network_boot = blk_by_name(s->network_boot_drive);
        if (!s->network_boot) {
            error_report("network boot block backend '%s' was not found",
                         s->network_boot_drive);
            exit(1);
        }
    }

    if (s->nvme_drive) {
        s->nvme = blk_by_name(s->nvme_drive);
        if (!s->nvme) {
            error_report("NVMe block backend '%s' was not found",
                         s->nvme_drive);
            exit(1);
        }
    }

    if (s->cm4) {
        Object *emmc2 = OBJECT(&soc->peripherals.emmc2);

        object_property_set_uint(emmc2, "data-error", s->emmc_data_error,
                                 &error_abort);
        object_property_set_uint(emmc2, "data-error-after",
                                 s->emmc_data_error_after, &error_abort);
        object_property_set_uint(emmc2, "data-error-count",
                                 s->emmc_data_error_count, &error_abort);
    }

    soc->peripherals.parent_obj.uart0.hci_controller = s->wireless_model;
    raspi_base_machine_init(machine, &soc->parent_obj, s->sd,
                            DEVICE(&soc->peripherals.emmc2),
                            s->cm4, s->emmc_boot, s->emmc_rpmb,
                            s->emmc_cid, s->emmc_cache_size,
                            s->emmc_cache_power_loss_on_reset,
                            s->emmc_cache_flush_sector_delay_us,
                            s->emmc_program_sector_delay_us,
                            s->emmc_erase_group_delay_us,
                            s->board_revision);
    if (s->wireless_model) {
        BusState *bus = qdev_get_child_bus(DEVICE(soc), "sd-bus");
        DeviceState *wifi = qdev_new(TYPE_CYW43455_SDIO);

        if (!bus) {
            error_report("wireless model requires the BCM2711 mmcnr SD bus");
            exit(1);
        }
        qdev_set_id(wifi, g_strdup("wifi"), &error_fatal);
        if (s->wireless_netdev) {
            object_property_set_str(OBJECT(wifi), "netdev",
                                    s->wireless_netdev, &error_fatal);
        }
        qdev_realize_and_unref(wifi, bus, &error_fatal);
    }
    s->usb_boot_on_xhci = raspi4_usb_boot_uses_xhci(s);
    raspi4_create_pcie(s);
    bcm2711_genet_set_boot_client(&soc->peripherals.genet,
                                  raspi4_network_dhcp_receive,
                                  raspi4_netconsole_link_changed, s);
    for (unsigned int group = 0; group < s->usb_boot_group_count; group++) {
        USBBus *boot_bus = s->usb_boot_on_xhci ?
            &s->vl805_xhci->bus :
            &soc->peripherals.parent_obj.dwc2.bus;
        unsigned int first = 0;
        unsigned int count = 0;

        for (unsigned int i = 0; i < s->usb_boot_device_count; i++) {
            if (s->usb_boot_device_numbers[i] == group) {
                if (!count) {
                    first = i;
                }
                count++;
            }
        }
        if (count == 1) {
            g_autofree char *serial =
                g_strdup_printf("QEMU-RPI-BOOT-D%u", group);
            USBDevice *usb = USB_DEVICE(qdev_new("usb-storage"));

            DEVICE(usb)->id = g_strdup_printf(
                "raspi4-usb-boot-%u", group);
            qdev_prop_set_drive(
                DEVICE(usb), "drive", s->usb_boot_devices[first]);
            qdev_prop_set_bit(DEVICE(usb), "removable", true);
            qdev_prop_set_string(DEVICE(usb), "serial", serial);
            usb_realize_and_unref(usb, boot_bus, &error_fatal);
        } else {
            g_autofree char *serial =
                g_strdup_printf("QEMU-RPI-BOOT-D%u", group);
            USBDevice *usb = USB_DEVICE(qdev_new("usb-bot"));
            MSDState *bot = USB_STORAGE_DEV(usb);

            DEVICE(usb)->id = g_strdup_printf(
                "raspi4-usb-boot-%u", group);
            qdev_prop_set_string(DEVICE(usb), "serial", serial);
            usb_realize_and_unref(usb, boot_bus, &error_fatal);
            for (unsigned int i = first; i < first + count; i++) {
                DeviceState *disk = qdev_new("scsi-hd");

                disk->id = g_strdup_printf(
                    "raspi4-usb-boot-%u-lun-%u", group,
                    s->usb_boot_luns[i]);
                qdev_prop_set_drive(disk, "drive", s->usb_boot_devices[i]);
                qdev_prop_set_uint32(disk, "lun", s->usb_boot_luns[i]);
                qdev_realize_and_unref(disk, BUS(&bot->bus), &error_fatal);
            }
        }
    }

    s->recovery_reboot_timer = timer_new_ms(
        QEMU_CLOCK_VIRTUAL, raspi4_recovery_reboot, s);
    s->eeprom_flash_timer = timer_new_us(
        QEMU_CLOCK_VIRTUAL, raspi4_eeprom_flash_step, s);
    s->boot_timeout_timer = timer_new_ms(
        QEMU_CLOCK_VIRTUAL, raspi4_boot_timeout, s);
    s->network_dhcp_retransmit_timer = timer_new_ms(
        QEMU_CLOCK_VIRTUAL, raspi4_network_dhcp_retransmit, s);
    s->network_tftp_retransmit_timer = timer_new_ms(
        QEMU_CLOCK_VIRTUAL, raspi4_network_tftp_retransmit, s);
    s->boot_restart_timer = timer_new_ms(
        QEMU_CLOCK_VIRTUAL, raspi4_restart_cycle, s);
    s->boot_watchdog_timer = timer_new_ns(
        QEMU_CLOCK_VIRTUAL, raspi4_boot_watchdog_expired, s);
    s->hdmi_diagnostics_timer = timer_new_ns(
        QEMU_CLOCK_VIRTUAL, raspi4_hdmi_diagnostics_expired, s);
    s->usb_boot_hotplug_bh = qemu_bh_new(raspi4_usb_hotplug_bh, s);
    s->sd_hotplug_bh = qemu_bh_new(raspi4_sd_hotplug_bh, s);
    s->network_boot_hotplug_bh = qemu_bh_new(raspi4_network_hotplug_bh, s);
    s->network_boot_vmstate = qemu_add_vm_change_state_handler(
        raspi4_network_vm_state_change, s);
    for (unsigned int i = 0; i < s->usb_boot_device_count; i++) {
        Raspi4UsbBootNotifier *insert = &s->usb_boot_insert_notifiers[i];
        Raspi4UsbBootNotifier *remove = &s->usb_boot_remove_notifiers[i];

        insert->machine = s;
        insert->notifier.notify = raspi4_usb_media_inserted;
        remove->machine = s;
        remove->notifier.notify = raspi4_usb_media_removed;
        blk_add_insert_bs_notifier(s->usb_boot_devices[i],
                                   &insert->notifier);
        blk_add_remove_bs_notifier(s->usb_boot_devices[i],
                                   &remove->notifier);
        s->usb_boot_notifiers_registered = true;
    }
    if (s->network_boot) {
        s->network_boot_insert_notifier.notify =
            raspi4_network_media_inserted;
        s->network_boot_remove_notifier.notify =
            raspi4_network_media_removed;
        blk_add_insert_bs_notifier(s->network_boot,
                                   &s->network_boot_insert_notifier);
        blk_add_remove_bs_notifier(s->network_boot,
                                   &s->network_boot_remove_notifier);
        s->network_boot_notifiers_registered = true;
    }
    if (s->sd && !s->cm4) {
        s->sd_insert_notifier.notify = raspi4_sd_media_inserted;
        s->sd_remove_notifier.notify = raspi4_sd_media_removed;
        blk_add_insert_bs_notifier(s->sd, &s->sd_insert_notifier);
        blk_add_remove_bs_notifier(s->sd, &s->sd_remove_notifier);
        s->sd_notifiers_registered = true;
    }
    if (vmstate_register(NULL, 0, &vmstate_raspi4_boot, s) < 0) {
        error_report("failed to register Raspberry Pi behavioral boot "
                     "migration state");
        exit(1);
    }
    s->boot_vmstate_registered = true;
    qemu_register_reset(raspi4b_boot_reset, s);
}

static void raspi4b_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    RaspiBaseMachineClass *rmc = RASPI_BASE_MACHINE_CLASS(oc);
    ObjectProperty *prop;

#if HOST_LONG_BITS == 32
    rmc->board_rev = 0xa03111; /* Revision 1.1, 1 Gb RAM */
#else
    rmc->board_rev = 0xb03115; /* Revision 1.5, 2 Gb RAM */
#endif
    raspi_machine_class_common_init(mc, rmc->board_rev);
    mc->default_nic = TYPE_BCM2711_GENET;
    mc->auto_create_sdcard = true;
    mc->init = raspi4b_machine_init;

    prop = object_class_property_add_str(oc, "boot-mode",
                                         raspi4b_get_boot_mode,
                                         raspi4b_set_boot_mode);
    object_property_set_default_str(prop, "direct");
    object_class_property_set_description(
        oc, "boot-mode",
        "Boot path: 'direct' preserves legacy loading; 'behavioral' starts "
        "at the modeled BCM2711 first-stage boundary");

    object_class_property_add_bool(oc, "nrpiboot", raspi4b_get_nrpiboot,
                                   raspi4b_set_nrpiboot);
    object_class_property_set_description(
        oc, "nrpiboot", "Sample the active nRPIBOOT input during ROM reset");
    object_class_property_add_bool(oc, "nrpiboot-sampled",
                                   raspi4b_get_nrpiboot_sampled, NULL);
    object_class_property_set_description(
        oc, "nrpiboot-sampled",
        "Active nRPIBOOT level sampled by the most recent behavioral reset");
    object_class_property_add_str(oc, "nrpiboot-source",
                                  raspi4b_get_nrpiboot_source, NULL);
    object_class_property_set_description(
        oc, "nrpiboot-source",
        "Source sampled at reset: machine-property or driven CM4 gpio40");
    object_class_property_add(oc, "rpiboot-bootcode-size", "uint32",
                              raspi4b_get_rpiboot_bootcode_size,
                              NULL, NULL, NULL);
    object_class_property_set_description(
        oc, "rpiboot-bootcode-size",
        "Bytes of bootcode4.bin accepted by the in-process DWC2 ROM");
    object_class_property_add(oc, "rpiboot-transfer-received", "uint32",
                              raspi4b_get_rpiboot_transfer_received,
                              NULL, NULL, NULL);
    object_class_property_set_description(
        oc, "rpiboot-transfer-received",
        "Bytes accepted in the active in-process RPIBOOT bulk transfer");
    object_class_property_add(oc, "rpiboot-configuration", "uint32",
                              raspi4b_get_rpiboot_configuration,
                              NULL, NULL, NULL);
    object_class_property_set_description(
        oc, "rpiboot-configuration",
        "Current USB configuration selected through the RPIBOOT ROM EP0");
    object_class_property_add_str(oc, "rpiboot-bootcode-sha256",
                                  raspi4b_get_rpiboot_bootcode_sha256, NULL);
    object_class_property_set_description(
        oc, "rpiboot-bootcode-sha256",
        "SHA-256 of bootcode4.bin accepted by the in-process DWC2 ROM");
    object_class_property_add_str(
        oc, "rpiboot-bootcode-trusted-sha256",
        raspi4b_get_rpiboot_bootcode_trusted_sha256,
        raspi4b_set_rpiboot_bootcode_trusted_sha256);
    object_class_property_set_description(
        oc, "rpiboot-bootcode-trusted-sha256",
        "Pinned SHA-256 trust digest for the exact bootcode4.bin; the "
        "in-process behavioral ROM fails closed when absent or mismatched");
    object_class_property_add_str(oc, "rpiboot-bootcode-trust",
                                  raspi4b_get_rpiboot_bootcode_trust, NULL);
    object_class_property_set_description(
        oc, "rpiboot-bootcode-trust",
        "First-stage trust result: not-checked, missing, mismatch, or trusted");
    object_class_property_add(oc, "rpiboot-config-size", "uint32",
                              raspi4b_get_rpiboot_config_size,
                              NULL, NULL, NULL);
    object_class_property_add_str(oc, "rpiboot-config-sha256",
                                  raspi4b_get_rpiboot_config_sha256, NULL);
    object_class_property_add(oc, "rpiboot-boot-img-size", "uint32",
                              raspi4b_get_rpiboot_boot_img_size,
                              NULL, NULL, NULL);
    object_class_property_add_str(oc, "rpiboot-boot-img-sha256",
                                  raspi4b_get_rpiboot_boot_img_sha256, NULL);

    object_class_property_add_bool(oc, "sd-recovery-enabled",
                                   raspi4b_get_sd_recovery_enabled,
                                   raspi4b_set_sd_recovery_enabled);
    object_class_property_set_description(
        oc, "sd-recovery-enabled",
        "Search the primary SD FAT partition for recovery.bin before EEPROM");

    object_class_property_add(
        oc, "thermal-temperature-millicelsius", "int32",
        raspi4b_get_thermal_temperature,
        raspi4b_set_thermal_temperature, NULL, NULL);
    object_class_property_set_description(
        oc, "thermal-temperature-millicelsius",
        "Logical BCM2711 AVS temperature input in millidegrees Celsius");
    object_class_property_add_bool(
        oc, "thermal-sensor-valid",
        raspi4b_get_thermal_sensor_valid,
        raspi4b_set_thermal_sensor_valid);
    object_class_property_set_description(
        oc, "thermal-sensor-valid",
        "Expose or clear the BCM2711 AVS temperature-valid status");
    object_class_property_add(
        oc, "firmware-throttled-current", "uint32",
        raspi4b_get_firmware_throttled_current,
        raspi4b_set_firmware_throttled_current, NULL, NULL);
    object_class_property_set_description(
        oc, "firmware-throttled-current",
        "Logical current under-voltage, frequency-cap, and throttle flags "
        "(bits 0 through 2)");
    object_class_property_add(
        oc, "firmware-throttled-status", "uint32",
        raspi4b_get_firmware_throttled_status, NULL, NULL, NULL);
    object_class_property_set_description(
        oc, "firmware-throttled-status",
        "Firmware GET_THROTTLED current flags and sticky history");

    object_class_property_add_bool(oc, "eeprom-write-protect",
                                   raspi4b_get_eeprom_write_protect,
                                   raspi4b_set_eeprom_write_protect);
    object_class_property_set_description(
        oc, "eeprom-write-protect",
        "Compatibility alias for the persistent EEPROM block-protect status");
    object_class_property_add_bool(
        oc, "eeprom-status-write-protect",
        raspi4b_get_eeprom_write_protect,
        raspi4b_set_eeprom_write_protect);
    object_class_property_set_description(
        oc, "eeprom-status-write-protect",
        "Persistent EEPROM status-register block protection; this, not the "
        "nWP pin alone, rejects array erase/program");
    object_class_property_add_bool(oc, "eeprom-nwp",
                                   raspi4b_get_eeprom_nwp,
                                   raspi4b_set_eeprom_nwp);
    object_class_property_set_description(
        oc, "eeprom-nwp",
        "Active-high logical level of the physical EEPROM_nWP pin; low locks "
        "changes to status-register block protection");
    object_class_property_add_bool(oc, "eeprom-nwp-sampled",
                                   raspi4b_get_eeprom_nwp_sampled, NULL);
    object_class_property_set_description(
        oc, "eeprom-nwp-sampled",
        "EEPROM_nWP level sampled for the latest recovery transaction");
    object_class_property_add_str(oc, "eeprom-nwp-source",
                                  raspi4b_get_eeprom_nwp_source, NULL);
    object_class_property_set_description(
        oc, "eeprom-nwp-source",
        "Live EEPROM_nWP source: machine-property or gpio-bridge");

    object_class_property_add(
        oc, "eeprom-stuck-zero-offset", "uint64",
        raspi4b_get_eeprom_stuck_zero_offset,
        raspi4b_set_eeprom_stuck_zero_offset, NULL, NULL);
    object_class_property_set_description(
        oc, "eeprom-stuck-zero-offset",
        "EEPROM byte whose selected bits remain zero after erase; UINT64_MAX "
        "disables the deterministic NOR-cell fault");
    object_class_property_add(
        oc, "eeprom-stuck-zero-mask", "uint8",
        raspi4b_get_eeprom_stuck_zero_mask,
        raspi4b_set_eeprom_stuck_zero_mask, NULL, NULL);
    object_class_property_set_description(
        oc, "eeprom-stuck-zero-mask",
        "Bit mask forced to zero at eeprom-stuck-zero-offset");

    object_class_property_add(oc, "eeprom-fail-after", "uint64",
                              raspi4b_get_eeprom_fail_after,
                              raspi4b_set_eeprom_fail_after, NULL, NULL);
    object_class_property_set_description(
        oc, "eeprom-fail-after",
        "Interrupt EEPROM recovery after this many bytes in the selected "
        "erase, program, or verify stage");
    object_class_property_add(
        oc, "eeprom-erase-sector-delay-us", "uint32",
        raspi4b_get_eeprom_erase_delay,
        raspi4b_set_eeprom_erase_delay, NULL, NULL);
    object_class_property_set_description(
        oc, "eeprom-erase-sector-delay-us",
        "Virtual delay before each durable 4 KiB EEPROM sector erase; zero "
        "retains synchronous compatibility behavior");
    object_class_property_add(
        oc, "eeprom-program-page-delay-us", "uint32",
        raspi4b_get_eeprom_program_delay,
        raspi4b_set_eeprom_program_delay, NULL, NULL);
    object_class_property_set_description(
        oc, "eeprom-program-page-delay-us",
        "Virtual delay before each durable 256-byte EEPROM page program; "
        "zero retains synchronous compatibility behavior");
    object_class_property_add(
        oc, "eeprom-verify-sector-delay-us", "uint32",
        raspi4b_get_eeprom_verify_delay,
        raspi4b_set_eeprom_verify_delay, NULL, NULL);
    object_class_property_set_description(
        oc, "eeprom-verify-sector-delay-us",
        "Virtual delay before each 4 KiB EEPROM verification read; zero "
        "retains synchronous compatibility behavior");
    object_class_property_add(
        oc, "eeprom-flash-elapsed-us", "uint64",
        raspi4b_get_eeprom_flash_elapsed, NULL, NULL, NULL);
    object_class_property_set_description(
        oc, "eeprom-flash-elapsed-us",
        "Accumulated configured virtual latency for the active or latest "
        "EEPROM recovery transaction");

    object_class_property_add(
        oc, "otp-provision-fail-after", "uint8",
        raspi4b_get_otp_provision_fail_after,
        raspi4b_set_otp_provision_fail_after, NULL, NULL);
    object_class_property_set_description(
        oc, "otp-provision-fail-after",
        "Interrupt irreversible secure-boot OTP provisioning after this "
        "many complete row writes; 255 disables the fault");

    object_class_property_add(oc, "eeprom-erased-bytes", "uint64",
                              raspi4b_get_eeprom_erased_bytes,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "eeprom-programmed-bytes", "uint64",
                              raspi4b_get_eeprom_programmed_bytes,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "eeprom-verified-bytes", "uint64",
                              raspi4b_get_eeprom_verified_bytes,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "eeprom-dirty-sector-count", "uint32",
                              raspi4b_get_eeprom_dirty_sector_count,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "eeprom-program-page-count", "uint32",
                              raspi4b_get_eeprom_program_page_count,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "eeprom-nor-violation-bits", "uint32",
                              raspi4b_get_eeprom_nor_violation_bits,
                              NULL, NULL, NULL);
    object_class_property_set_description(
        oc, "eeprom-dirty-sector-count",
        "4 KiB EEPROM sectors durably touched by the latest recovery");
    object_class_property_add_str(oc, "eeprom-flash-stage",
                                  raspi4b_get_eeprom_flash_stage, NULL);
    object_class_property_set_description(
        oc, "eeprom-flash-stage",
        "Latest EEPROM recovery stage: idle, erase, program, verify, or "
        "complete");

    prop = object_class_property_add_str(oc, "eeprom-fail-stage",
                                         raspi4b_get_eeprom_fail_stage,
                                         raspi4b_set_eeprom_fail_stage);
    object_property_set_default_str(prop, "program");
    object_class_property_set_description(
        oc, "eeprom-fail-stage",
        "EEPROM recovery fault stage: erase, program, verify, "
        "verify-mismatch, rename, or reboot");

    object_class_property_add_str(oc, "eeprom-drive",
                                  raspi4b_get_eeprom_drive,
                                  raspi4b_set_eeprom_drive);
    object_class_property_set_description(
        oc, "eeprom-drive",
        "Named if=none block backend containing an exact 512 KiB BCM2711 "
        "SPI EEPROM image");
    object_class_property_add_str(oc, "hat-eeprom-drive",
                                  raspi4b_get_hat_eeprom_drive,
                                  raspi4b_set_hat_eeprom_drive);
    object_class_property_set_description(
        oc, "hat-eeprom-drive",
        "Named read-only if=none backend containing an unchanged Raspberry "
        "Pi HAT ID EEPROM image");
    object_class_property_add_str(oc, "eeprom-status-drive",
                                  raspi4b_get_eeprom_status_drive,
                                  raspi4b_set_eeprom_status_drive);
    object_class_property_set_description(
        oc, "eeprom-status-drive",
        "Named 512-byte if=none backend persisting EEPROM status-register "
        "block protection (first byte 0=off or 1=on; reserved bytes zero)");
    object_class_property_add_str(oc, "hdmi0-edid-file",
                                  raspi4b_get_hdmi0_edid_file,
                                  raspi4b_set_hdmi0_edid_file);
    object_class_property_set_description(
        oc, "hdmi0-edid-file",
        "Raw EDID bytes sampled for HDMI0 at each behavioral reset");
    object_class_property_add_str(oc, "hdmi1-edid-file",
                                  raspi4b_get_hdmi1_edid_file,
                                  raspi4b_set_hdmi1_edid_file);
    object_class_property_set_description(
        oc, "hdmi1-edid-file",
        "Raw EDID bytes sampled for HDMI1 at each behavioral reset");
    object_class_property_add_str(oc, "hdmi0-edid-name",
                                  raspi4b_get_hdmi0_edid_name, NULL);
    object_class_property_add_str(oc, "hdmi1-edid-name",
                                  raspi4b_get_hdmi1_edid_name, NULL);
    object_class_property_add_str(oc, "hdmi0-edid-status",
                                  raspi4b_get_hdmi0_edid_status, NULL);
    object_class_property_add_str(oc, "hdmi1-edid-status",
                                  raspi4b_get_hdmi1_edid_status, NULL);

    object_class_property_add_str(oc, "usb-boot-drive",
                                  raspi4b_get_usb_boot_drive,
                                  raspi4b_set_usb_boot_drive);
    object_class_property_set_description(
        oc, "usb-boot-drive",
        "Named if=none raw USB mass-storage backend used by behavioral "
        "BOOT_ORDER and attached to the emulated DWC2 host");
    object_class_property_add_str(oc, "usb-boot-drives",
                                  raspi4b_get_usb_boot_drives,
                                  raspi4b_set_usb_boot_drives);
    object_class_property_set_description(
        oc, "usb-boot-drives",
        "Ordered USB-MSD topology of if=none backends: ':' separates USB "
        "devices and '+' separates consecutive LUNs on one device");
    prop = object_class_property_add_str(
        oc, "usb-boot-controller", raspi4b_get_usb_boot_controller,
        raspi4b_set_usb_boot_controller);
    object_property_set_default_str(prop, "auto");
    object_class_property_set_description(
        oc, "usb-boot-controller",
        "USB-MSD attachment: auto selects VL805/xHCI on Pi 4B and "
        "BCM2711 DWC2 on CM4; xhci and dwc2 select explicitly");
    object_class_property_add_bool(
        oc, "usb-boot-external", raspi4b_get_usb_boot_external,
        raspi4b_set_usb_boot_external);
    object_class_property_set_description(
        oc, "usb-boot-external",
        "Discover bounded USB-MSD devices attached separately to the "
        "selected controller instead of realizing named machine backends");
    object_class_property_add_bool(
        oc, "usb-boot-bot-stall-once",
        raspi4b_get_usb_boot_bot_stall_once,
        raspi4b_set_usb_boot_bot_stall_once);
    object_class_property_set_description(
        oc, "usb-boot-bot-stall-once",
        "Inject one malformed BOT CBW and require Mass Storage Reset plus "
        "endpoint-halt recovery");
    object_class_property_add(
        oc, "usb-boot-bot-stall-count", "uint8",
        raspi4b_get_usb_boot_bot_stall_count,
        raspi4b_set_usb_boot_bot_stall_count, NULL, NULL);
    object_class_property_set_description(
        oc, "usb-boot-bot-stall-count",
        "Inject up to eight malformed BOT CBWs and require bounded reset "
        "recovery before retrying the unchanged command");
    object_class_property_add(
        oc, "usb-boot-bot-phase-count", "uint8",
        raspi4b_get_usb_boot_bot_phase_count,
        raspi4b_set_usb_boot_bot_phase_count, NULL, NULL);
    object_class_property_set_description(
        oc, "usb-boot-bot-phase-count",
        "Cycle through the six USB-IF BOT phase-error relations and require "
        "bounded reset recovery before retrying the unchanged command");
    object_class_property_add(
        oc, "usb-boot-bot-case-count", "uint8",
        raspi4b_get_usb_boot_bot_case_count,
        raspi4b_set_usb_boot_bot_case_count, NULL, NULL);
    object_class_property_set_description(
        oc, "usb-boot-bot-case-count",
        "Exercise a prefix of the complete thirteen-case USB-IF BOT "
        "direction and length matrix before unchanged boot traffic");

    object_class_property_add_str(oc, "network-boot-drive",
                                  raspi4b_get_network_boot_drive,
                                  raspi4b_set_network_boot_drive);
    object_class_property_set_description(
        oc, "network-boot-drive",
        "Named if=none raw FAT backend representing the unchanged file "
        "corpus served by behavioral DHCP/TFTP network boot");
    object_class_property_add_str(oc, "nvme-drive",
                                  raspi4b_get_nvme_drive,
                                  raspi4b_set_nvme_drive);
    object_class_property_set_description(
        oc, "nvme-drive",
        "Named if=none raw NVMe namespace used by behavioral BOOT_ORDER "
        "mode 6 and the PCIe NVMe controller");
    object_class_property_add_bool(oc, "network-boot-wire",
                                   raspi4b_get_network_boot_wire,
                                   raspi4b_set_network_boot_wire);
    object_class_property_set_description(
        oc, "network-boot-wire",
        "Use real DHCP/TFTP through GENET for network BOOT_ORDER; disabling "
        "retains the corpus-only compatibility path");
    object_class_property_add_bool(oc, "wireless-model",
                                   raspi4b_get_wireless_model,
                                   raspi4b_set_wireless_model);
    object_class_property_set_description(
        oc, "wireless-model",
        "Instantiate the onboard CYW43455 on mmcnr; Pi 4B boot storage "
        "always uses EMMC2 and RF behavior remains a Pass 2 HIL gate");
    object_class_property_add_str(oc, "wireless-netdev",
                                  raspi4b_get_wireless_netdev,
                                  raspi4b_set_wireless_netdev);
    object_class_property_set_description(
        oc, "wireless-netdev",
        "Attach the onboard CYW43455 data path to the named QEMU netdev "
        "backend; requires wireless-model=on");
    object_class_property_add_bool(
        oc, "net-install-requested",
        raspi4b_get_net_install_requested,
        raspi4b_set_net_install_requested);
    object_class_property_set_description(
        oc, "net-install-requested",
        "Model the boot-time user request for Raspberry Pi Network Install");
    object_class_property_add_bool(
        oc, "boot-disable-hdmi",
        raspi4b_get_boot_disable_hdmi, NULL);
    object_class_property_add(
        oc, "boot-hdmi-delay", "uint32",
        raspi4b_get_hdmi_delay, NULL, NULL, NULL);
    object_class_property_add_bool(
        oc, "boot-hdmi-diagnostics-pending",
        raspi4b_get_hdmi_diagnostics_pending, NULL);
    object_class_property_add_bool(
        oc, "boot-hdmi-diagnostics-visible",
        raspi4b_get_hdmi_diagnostics_visible, NULL);
    object_class_property_add(
        oc, "boot-hdmi-diagnostics-remaining-ns", "uint64",
        raspi4b_get_hdmi_diagnostics_remaining, NULL, NULL, NULL);
    object_class_property_add_bool(
        oc, "boot-netconsole-enabled",
        raspi4b_get_netconsole_enabled, NULL);
    object_class_property_add_str(
        oc, "boot-netconsole-config",
        raspi4b_get_netconsole_config, NULL);
    object_class_property_add_bool(
        oc, "boot-netconsole-link-waiting",
        raspi4b_get_netconsole_link_waiting, NULL);
    object_class_property_add(
        oc, "boot-netconsole-remaining-ns", "uint64",
        raspi4b_get_netconsole_remaining, NULL, NULL, NULL);
    object_class_property_add(
        oc, "boot-netconsole-packets", "uint64",
        raspi4b_get_netconsole_packets, NULL, NULL, NULL);
    object_class_property_add(
        oc, "boot-netconsole-bytes", "uint64",
        raspi4b_get_netconsole_bytes, NULL, NULL, NULL);
    object_class_property_add_bool(
        oc, "boot-net-install-enabled",
        raspi4b_get_net_install_enabled, NULL);
    object_class_property_add_bool(
        oc, "boot-net-install-at-power-on",
        raspi4b_get_net_install_at_power_on, NULL);
    object_class_property_add_bool(
        oc, "net-install-usb-fallback-consumed",
        raspi4b_get_net_install_usb_fallback_consumed, NULL);
    object_class_property_add_bool(
        oc, "usb-boot-files-missing",
        raspi4b_get_usb_boot_files_missing, NULL);
    object_class_property_add(
        oc, "boot-net-install-keyboard-wait-ms", "uint32",
        raspi4b_get_net_install_keyboard_wait, NULL, NULL, NULL);
    object_class_property_add_bool(
        oc, "net-install-keyboard-present",
        raspi4b_get_net_install_keyboard_present,
        raspi4b_set_net_install_keyboard_present);
    object_class_property_set_description(
        oc, "net-install-keyboard-present",
        "Expose a host keyboard to the EEPROM Network Install scan");
    object_class_property_add_bool(
        oc, "net-install-shift-held",
        raspi4b_get_net_install_shift_held,
        raspi4b_set_net_install_shift_held);
    object_class_property_set_description(
        oc, "net-install-shift-held",
        "Expose the host Shift key to the active Network Install scan");
    object_class_property_add_bool(
        oc, "net-install-keyboard-waiting",
        raspi4b_get_net_install_keyboard_waiting, NULL);
    object_class_property_add(
        oc, "net-install-keyboard-remaining-ns", "uint64",
        raspi4b_get_net_install_keyboard_remaining, NULL, NULL, NULL);
    object_class_property_add_bool(
        oc, "boot-enable-self-update",
        raspi4b_get_enable_self_update, NULL);
    object_class_property_add_bool(
        oc, "boot-freeze-version",
        raspi4b_get_freeze_version, NULL);
    object_class_property_add_str(
        oc, "boot-self-update-status",
        raspi4b_get_self_update_status, NULL);
    object_class_property_add_str(oc, "http-tls-creds",
                                  raspi4b_get_http_tls_creds,
                                  raspi4b_set_http_tls_creds);
    object_class_property_set_description(
        oc, "http-tls-creds",
        "Client tls-creds object used for the Pi bootloader default-host "
        "HTTPS transport");
    object_class_property_add_str(
        oc, "bootsys-trusted-sha256",
        raspi4b_get_bootsys_trusted_sha256,
        raspi4b_set_bootsys_trusted_sha256);
    object_class_property_set_description(
        oc, "bootsys-trusted-sha256",
        "Pinned SHA-256 trust digest for the exact BCM2711 bootsys section; "
        "secure boot fails closed when this oracle is absent or mismatched");
    object_class_property_add_str(
        oc, "recovery-trusted-sha256",
        raspi4b_get_recovery_trusted_sha256,
        raspi4b_set_recovery_trusted_sha256);
    object_class_property_set_description(
        oc, "recovery-trusted-sha256",
        "Pinned SHA-256 trust digest for the exact BCM2711 recovery.bin; "
        "behavioral ROM execution fails closed when absent or mismatched");

    object_class_property_add_str(oc, "otp-drive", raspi4b_get_otp_drive,
                                  raspi4b_set_otp_drive);
    object_class_property_set_description(
        oc, "otp-drive",
        "Named if=none 512-byte backend with 66 little-endian BCM2711 OTP "
        "rows");

    object_class_property_add_str(oc, "gpio-chardev",
                                  raspi4b_get_gpio_chardev,
                                  raspi4b_set_gpio_chardev);
    object_class_property_set_description(
        oc, "gpio-chardev",
        "Optional versioned host bridge for all 58 BCM2711 GPIO pins");

    object_class_property_add(oc, "otp-rpiboot-gpio", "uint8",
                              raspi4b_get_otp_rpiboot_gpio,
                              raspi4b_set_otp_rpiboot_gpio, NULL, NULL);
    object_class_property_set_description(
        oc, "otp-rpiboot-gpio",
        "Behavioral Pi 4B OTP selection for the active-low nRPIBOOT GPIO");

    object_class_property_add_str(oc, "boot-state", raspi4b_get_boot_state,
                                  NULL);
    object_class_property_add_str(oc, "boot-source", raspi4b_get_boot_source,
                                  NULL);
    object_class_property_add_str(oc, "recovery-status",
                                  raspi4b_get_recovery_status, NULL);
    object_class_property_add_str(oc, "recovery-sha256",
                                  raspi4b_get_recovery_sha256, NULL);
    object_class_property_add(oc, "recovery-key-index", "uint32",
                              raspi4b_get_recovery_key_index,
                              NULL, NULL, NULL);
    object_class_property_add_str(oc, "secure-boot-status",
                                  raspi4b_get_secure_boot_status, NULL);
    object_class_property_add_str(oc, "bootsys-sha256",
                                  raspi4b_get_bootsys_sha256, NULL);
    object_class_property_add_str(oc, "bootsys-dependencies-sha256",
                                  raspi4b_get_bootsys_dependencies_sha256,
                                  NULL);
    object_class_property_add(oc, "bootsys-dependency-count", "uint32",
                              raspi4b_get_bootsys_dependency_count,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "bootsys-key-index", "uint32",
                              raspi4b_get_bootsys_key_index,
                              NULL, NULL, NULL);
    object_class_property_add_str(oc, "secure-boot-image-sha256",
                                  raspi4b_get_secure_image_sha256, NULL);
    object_class_property_add_str(oc, "secure-boot-signature-sha256",
                                  raspi4b_get_secure_signature_sha256, NULL);
    object_class_property_add(oc, "secure-boot-image-size", "uint32",
                              raspi4b_get_secure_image_size,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "secure-boot-signature-size", "uint32",
                              raspi4b_get_secure_signature_size,
                              NULL, NULL, NULL);
    object_class_property_add_str(oc, "boot-tftp-ip",
                                  raspi4b_get_tftp_ip, NULL);
    object_class_property_add_str(oc, "boot-mac-address",
                                  raspi4b_get_boot_mac_address, NULL);
    object_class_property_add_str(oc, "boot-mac-address-source",
                                  raspi4b_get_boot_mac_address_source, NULL);
    object_class_property_add(oc, "boot-tftp-prefix-mode", "uint8",
                              raspi4b_get_tftp_prefix_mode,
                              NULL, NULL, NULL);
    object_class_property_add_str(oc, "boot-tftp-prefix",
                                  raspi4b_get_tftp_prefix, NULL);
    object_class_property_add_bool(oc, "boot-tftp-prefix-fallback",
                                   raspi4b_get_tftp_prefix_fallback, NULL);
    object_class_property_add_str(oc, "boot-network-server-ip",
                                  raspi4b_get_network_server_ip, NULL);
    object_class_property_add_str(oc, "boot-dns-server-ip",
                                  raspi4b_get_dns_server_ip, NULL);
    object_class_property_add_str(oc, "boot-tftp-hostname",
                                  raspi4b_get_tftp_hostname, NULL);
    object_class_property_add_str(oc, "boot-http-host",
                                  raspi4b_get_http_host, NULL);
    object_class_property_add_str(oc, "boot-http-path",
                                  raspi4b_get_http_path, NULL);
    object_class_property_add_str(oc, "boot-http-cacert-hash",
                                  raspi4b_get_http_cacert_hash, NULL);
    object_class_property_add_str(oc, "boot-client-ip",
                                  raspi4b_get_client_ip, NULL);
    object_class_property_add_str(oc, "boot-subnet",
                                  raspi4b_get_subnet, NULL);
    object_class_property_add_str(oc, "boot-gateway",
                                  raspi4b_get_gateway, NULL);
    object_class_property_add(oc, "boot-order", "uint32",
                              raspi4b_get_boot_order, NULL, NULL, NULL);
    object_class_property_add(oc, "boot-max-restarts", "int32",
                              raspi4b_get_max_restarts, NULL, NULL, NULL);
    object_class_property_add(oc, "boot-sd-max-retries", "int32",
                              raspi4b_get_sd_boot_max_retries,
                              NULL, NULL, NULL);
    object_class_property_add_bool(oc, "sd-overcurrent",
                                   raspi4b_get_sd_overcurrent,
                                   raspi4b_set_sd_overcurrent);
    object_class_property_set_description(
        oc, "sd-overcurrent",
        "Host-injected Pi 4 SD power-switch over-current signal");
    object_class_property_add_str(oc, "sd-overcurrent-source",
                                  raspi4b_get_sd_overcurrent_source, NULL);
    object_class_property_add_bool(oc, "boot-sd-overcurrent-check",
                                   raspi4b_get_sd_overcurrent_check, NULL);
    object_class_property_add(oc, "boot-sd-quirks", "uint32",
                              raspi4b_get_sd_quirks, NULL, NULL, NULL);
    object_class_property_add_bool(oc, "boot-sd-high-speed-enabled",
                                   raspi4b_get_sd_high_speed_enabled, NULL);
    object_class_property_add(oc, "boot-sd-clock-limit-hz", "uint32",
                              raspi4b_get_sd_clock_limit,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "boot-sd-controller-clock-hz", "uint32",
                              raspi4b_get_sd_controller_clock,
                              NULL, NULL, NULL);
    object_class_property_add_bool(
        oc, "boot-sd-controller-high-speed",
        raspi4b_get_sd_controller_high_speed, NULL);
    object_class_property_add_bool(oc, "boot-sd-overcurrent-warning",
                                   raspi4b_get_sd_overcurrent_warning, NULL);
    object_class_property_add_bool(oc, "boot-sd-power-enabled",
                                   raspi4b_get_sd_power_enabled, NULL);
    object_class_property_add(
        oc, "boot-sd-overcurrent-retries", "uint64",
        raspi4b_get_sd_overcurrent_retries, NULL, NULL, NULL);
    object_class_property_add(
        oc, "boot-sd-overcurrent-remaining-ns", "uint64",
        raspi4b_get_sd_overcurrent_remaining, NULL, NULL, NULL);
    object_class_property_add(oc, "boot-usb-discover-timeout-ms", "uint32",
                              raspi4b_get_usb_msd_discover_timeout,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "boot-usb-lun-timeout-ms", "uint32",
                              raspi4b_get_usb_msd_lun_timeout,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "boot-usb-startup-delay-ms", "uint32",
                              raspi4b_get_usb_msd_startup_delay,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "boot-usb-power-off-time-ms", "uint32",
                              raspi4b_get_usb_msd_power_off_time,
                              NULL, NULL, NULL);
    object_class_property_add_bool(oc, "boot-usb-power-enabled",
                                   raspi4b_get_usb_power_enabled, NULL);
    object_class_property_add_bool(oc, "boot-usb-power-off-applicable",
                                   raspi4b_get_usb_power_off_applicable,
                                   NULL);
    object_class_property_add_str(oc, "boot-usb-power-cycle-mode",
                                  raspi4b_get_usb_power_cycle_mode, NULL);
    object_class_property_add(oc, "boot-usb-power-off-elapsed-ms", "uint64",
                              raspi4b_get_usb_power_off_elapsed,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "boot-usb-power-off-remaining-ns",
                              "uint64",
                              raspi4b_get_usb_power_off_remaining,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "boot-watchdog-timeout-s", "uint32",
                              raspi4b_get_boot_watchdog_timeout,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "boot-watchdog-partition", "uint8",
                              raspi4b_get_boot_watchdog_partition,
                              NULL, NULL, NULL);
    object_class_property_add_bool(oc, "boot-watchdog-armed",
                                   raspi4b_get_boot_watchdog_armed, NULL);
    object_class_property_add(oc, "boot-watchdog-remaining-ns", "uint64",
                              raspi4b_get_boot_watchdog_remaining,
                              NULL, NULL, NULL);
    object_class_property_add_bool(oc, "boot-reboot-on-fatal-error",
                                   raspi4b_get_reboot_on_fatal_error, NULL);
    object_class_property_add(oc, "boot-fatal-error-reboot-count", "uint64",
                              raspi4b_get_fatal_error_reboot_count,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "boot-fatal-error-remaining-ns", "uint64",
                              raspi4b_get_fatal_error_remaining,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "boot-net-max-retries", "int32",
                              raspi4b_get_net_boot_max_retries,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "boot-dhcp-timeout-ms", "uint32",
                              raspi4b_get_dhcp_timeout,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "boot-dhcp-req-timeout-ms", "uint32",
                              raspi4b_get_dhcp_req_timeout,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "boot-dhcp-option97", "uint32",
                              raspi4b_get_dhcp_option97,
                              NULL, NULL, NULL);
    object_class_property_add_str(oc, "boot-pxe-option43",
                                  raspi4b_get_pxe_option43, NULL);
    object_class_property_add(oc, "boot-tftp-file-timeout-ms", "uint32",
                              raspi4b_get_tftp_file_timeout,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "boot-http-port", "uint16",
                              raspi4b_get_http_port, NULL, NULL, NULL);
    object_class_property_add(oc, "boot-tftp-retransmit-count", "uint64",
                              raspi4b_get_tftp_retransmit_count,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "boot-dhcp-retransmit-count", "uint64",
                              raspi4b_get_dhcp_retransmit_count,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "boot-dns-retransmit-count", "uint64",
                              raspi4b_get_dns_retransmit_count,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "boot-attempt-count", "uint64",
                              raspi4b_get_boot_attempt_count,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "boot-restart-count", "uint64",
                              raspi4b_get_boot_restart_count,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "boot-retry-count", "uint64",
                              raspi4b_get_boot_retry_count,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "boot-elapsed-ms", "uint64",
                              raspi4b_get_boot_elapsed_ms,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "boot-order-index", "uint8",
                              raspi4b_get_boot_order_index,
                              NULL, NULL, NULL);
    object_class_property_add_str(oc, "firmware-file",
                                  raspi4b_get_firmware_file, NULL);
    object_class_property_add_str(oc, "firmware-status",
                                  raspi4b_get_firmware_status, NULL);
    object_class_property_add_str(oc, "wireless-status",
                                  raspi4b_get_wireless_status, NULL);
    object_class_property_set_description(
        oc, "wireless-status",
        "Onboard Wi-Fi/Bluetooth model status; RF remains hardware-only");
    object_class_property_add_str(oc, "peripheral-model-policy",
                                  raspi4b_get_peripheral_policy, NULL);
    object_class_property_set_description(
        oc, "peripheral-model-policy",
        "Versioned final-DT policy for peripherals without a behavior model");
    object_class_property_add_str(oc, "peripheral-exclusions",
                                  raspi4b_get_peripheral_exclusions, NULL);
    object_class_property_set_description(
        oc, "peripheral-exclusions",
        "Semicolon-separated compatible strings forced disabled in final DT");
    object_class_property_add_str(oc, "videocore-execution-mode",
                                  raspi4b_get_videocore_execution_mode,
                                  NULL);
    object_class_property_set_description(
        oc, "videocore-execution-mode",
        "VideoCore boundary: versioned clean-room behavior or inactive");
    object_class_property_add_str(oc, "videocore-artifact-policy",
                                  raspi4b_get_videocore_artifact_policy,
                                  NULL);
    object_class_property_set_description(
        oc, "videocore-artifact-policy",
        "Whether exact firmware bytes are behavioral inputs or executable");
    object_class_property_add(oc, "videocore-boundary-version", "uint32",
                              raspi4b_get_videocore_boundary_version,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "firmware-size", "uint32",
                              raspi4b_get_firmware_size, NULL, NULL, NULL);
    object_class_property_add_str(oc, "firmware-fixup-file",
                                  raspi4b_get_fixup_file, NULL);
    object_class_property_add(oc, "firmware-fixup-size", "uint32",
                              raspi4b_get_fixup_size, NULL, NULL, NULL);
    object_class_property_add_str(oc, "firmware-kernel-file",
                                  raspi4b_get_kernel_file, NULL);
    object_class_property_add(oc, "firmware-kernel-size", "uint32",
                              raspi4b_get_kernel_size, NULL, NULL, NULL);
    object_class_property_add_str(oc, "firmware-device-tree-file",
                                  raspi4b_get_device_tree_file, NULL);
    object_class_property_add(oc, "firmware-device-tree-size", "uint32",
                              raspi4b_get_device_tree_size,
                              NULL, NULL, NULL);
    object_class_property_add_str(oc, "firmware-cmdline-file",
                                  raspi4b_get_cmdline_file, NULL);
    object_class_property_add(oc, "firmware-cmdline-size", "uint32",
                              raspi4b_get_cmdline_size, NULL, NULL, NULL);
    object_class_property_add_str(oc, "firmware-initramfs-file",
                                  raspi4b_get_initramfs_file, NULL);
    object_class_property_add(oc, "firmware-initramfs-size", "uint32",
                              raspi4b_get_initramfs_size,
                              NULL, NULL, NULL);
    object_class_property_add_str(oc, "firmware-os-prefix",
                                  raspi4b_get_os_prefix, NULL);
    object_class_property_add_bool(oc, "firmware-config-present",
                                   raspi4b_get_config_present, NULL);
    object_class_property_add_bool(oc, "firmware-arm-64bit",
                                   raspi4b_get_arm_64bit, NULL);
    object_class_property_add_str(oc, "arm-handoff-status",
                                  raspi4b_get_handoff_status, NULL);
    object_class_property_add_str(oc, "arm-handoff-architecture",
                                  raspi4b_get_handoff_architecture, NULL);
    object_class_property_add_str(oc, "arm-handoff-endianness",
                                  raspi4b_get_handoff_endianness, NULL);
    object_class_property_add_str(oc, "boot-health",
                                  raspi4b_get_boot_health,
                                  raspi4b_set_boot_health);
    object_class_property_set_description(
        oc, "boot-health",
        "Host-observed behavioral health milestone: kernel-started, "
        "userspace-ready, or failed");
    object_class_property_add(oc, "arm-handoff-kernel-address", "uint64",
                              raspi4b_get_handoff_kernel_address,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "arm-handoff-kernel-size", "uint64",
                              raspi4b_get_handoff_kernel_size,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "arm-handoff-entry-address", "uint64",
                              raspi4b_get_handoff_entry_address,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "arm-handoff-device-tree-address",
                              "uint64",
                              raspi4b_get_handoff_device_tree_address,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "arm-handoff-device-tree-size", "uint64",
                              raspi4b_get_handoff_device_tree_size,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "arm-handoff-initramfs-address", "uint64",
                              raspi4b_get_handoff_initramfs_address,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "arm-handoff-core-mask", "uint8",
                              raspi4b_get_handoff_core_mask,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "firmware-config-lines", "uint32",
                              raspi4b_get_config_line_count,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "firmware-config-includes", "uint32",
                              raspi4b_get_config_include_count,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "firmware-config-ignored", "uint32",
                              raspi4b_get_config_ignored_count,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "firmware-total-mem-mb", "uint32",
                              raspi4b_get_firmware_total_mem_mb,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "firmware-gpu-mem-mb", "uint32",
                              raspi4b_get_firmware_gpu_mem_mb,
                              NULL, NULL, NULL);
    object_class_property_add_str(oc, "firmware-gpu-mem-source",
                                  raspi4b_get_firmware_gpu_mem_source,
                                  NULL);
    object_class_property_add(
        oc, "firmware-bootcode-delay-seconds", "uint32",
        raspi4b_get_firmware_bootcode_delay, NULL, NULL, NULL);
    object_class_property_add(
        oc, "firmware-sdram-frequency-requested-mhz", "uint32",
        raspi4b_get_firmware_sdram_frequency, NULL, NULL,
        GUINT_TO_POINTER(1));
    object_class_property_add_bool(
        oc, "firmware-sdram-frequency-requested",
        raspi4b_get_firmware_sdram_frequency_requested, NULL);
    object_class_property_add(
        oc, "firmware-sdram-frequency-mhz", "uint32",
        raspi4b_get_firmware_sdram_frequency, NULL, NULL, NULL);
    object_class_property_add_bool(
        oc, "boot-uart-enabled", raspi4b_get_boot_uart_enabled, NULL);
    object_class_property_add_bool(
        oc, "boot-uart-active", raspi4b_get_boot_uart_active, NULL);
    object_class_property_add(
        oc, "boot-uart-bytes", "uint64",
        raspi4b_get_boot_uart_bytes, NULL, NULL, NULL);
    object_class_property_add(
        oc, "boot-uart-lines", "uint32",
        raspi4b_get_boot_uart_lines, NULL, NULL, NULL);
    object_class_property_add_str(
        oc, "boot-uart-format", raspi4b_get_boot_uart_format, NULL);
    object_class_property_add_bool(
        oc, "boot-vl805-enabled", raspi4b_get_vl805_enabled, NULL);
    object_class_property_add_bool(
        oc, "boot-vl805-initialized",
        raspi4b_get_vl805_initialized, NULL);
    object_class_property_add_str(
        oc, "boot-vl805-status", raspi4b_get_vl805_boot_status, NULL);
    object_class_property_add_bool(
        oc, "firmware-uart-2ndstage",
        raspi4b_get_firmware_uart_enabled, NULL);
    object_class_property_add(
        oc, "firmware-uart-2ndstage-bytes", "uint64",
        raspi4b_get_firmware_uart_bytes, NULL, NULL, NULL);
    object_class_property_add(
        oc, "firmware-uart-2ndstage-lines", "uint32",
        raspi4b_get_firmware_uart_lines, NULL, NULL, NULL);
    object_class_property_add(oc, "firmware-overlay-count", "uint32",
                              raspi4b_get_overlay_count, NULL, NULL, NULL);
    object_class_property_add(oc, "firmware-dtparam-count", "uint32",
                              raspi4b_get_dtparam_count, NULL, NULL, NULL);
    object_class_property_add(oc, "firmware-overlay-applied", "uint32",
                              raspi4b_get_overlay_applied_count,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "firmware-dtparam-applied", "uint32",
                              raspi4b_get_dtparam_applied_count,
                              NULL, NULL, NULL);
    object_class_property_add_str(oc, "firmware-overlay-file",
                                  raspi4b_get_last_overlay_file, NULL);
    object_class_property_add_str(oc, "hat-eeprom-status",
                                  raspi4b_get_hat_eeprom_status, NULL);
    object_class_property_add_str(oc, "hat-eeprom-sha256",
                                  raspi4b_get_hat_eeprom_sha256, NULL);
    object_class_property_add_str(oc, "hat-content-sha256",
                                  raspi4b_get_hat_content_sha256, NULL);
    object_class_property_add(oc, "hat-eeprom-size", "uint64",
                              raspi4b_get_hat_eeprom_size,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "hat-eeprom-declared-size", "uint32",
                              raspi4b_get_hat_identity_number,
                              NULL, NULL, GUINT_TO_POINTER(0));
    object_class_property_add_str(oc, "hat-vendor",
                                  raspi4b_get_hat_vendor, NULL);
    object_class_property_add_str(oc, "hat-product",
                                  raspi4b_get_hat_product, NULL);
    object_class_property_add_str(oc, "hat-uuid",
                                  raspi4b_get_hat_uuid, NULL);
    object_class_property_add(oc, "hat-product-id", "uint32",
                              raspi4b_get_hat_identity_number,
                              NULL, NULL, GUINT_TO_POINTER(1));
    object_class_property_add(oc, "hat-product-version", "uint32",
                              raspi4b_get_hat_identity_number,
                              NULL, NULL, GUINT_TO_POINTER(2));
    object_class_property_add(oc, "hat-custom-count", "uint32",
                              raspi4b_get_hat_identity_number,
                              NULL, NULL, GUINT_TO_POINTER(3));
    object_class_property_add_str(oc, "hat-overlay",
                                  raspi4b_get_hat_overlay, NULL);
    object_class_property_add_str(oc, "hat-gpio-map-status",
                                  raspi4b_get_hat_gpio_status, NULL);
    object_class_property_add(oc, "hat-gpio-used-mask", "uint32",
                              raspi4b_get_hat_gpio_used_mask,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "hat-gpio-drive", "uint8",
                              raspi4b_get_hat_gpio_policy,
                              NULL, NULL, GUINT_TO_POINTER(0));
    object_class_property_add(oc, "hat-gpio-slew", "uint8",
                              raspi4b_get_hat_gpio_policy,
                              NULL, NULL, GUINT_TO_POINTER(1));
    object_class_property_add(oc, "hat-gpio-hysteresis", "uint8",
                              raspi4b_get_hat_gpio_policy,
                              NULL, NULL, GUINT_TO_POINTER(2));
    object_class_property_add(oc, "hat-gpio-back-power", "uint8",
                              raspi4b_get_hat_gpio_policy,
                              NULL, NULL, GUINT_TO_POINTER(3));
    object_class_property_add_str(oc, "firmware-sha256",
                                  raspi4b_get_firmware_sha256, NULL);
    object_class_property_add_str(oc, "firmware-fixup-sha256",
                                  raspi4b_get_fixup_sha256, NULL);
    object_class_property_add_str(oc, "firmware-kernel-sha256",
                                  raspi4b_get_kernel_sha256, NULL);
    object_class_property_add_str(oc, "firmware-device-tree-sha256",
                                  raspi4b_get_device_tree_sha256, NULL);
    object_class_property_add_str(oc, "firmware-cmdline-sha256",
                                  raspi4b_get_cmdline_sha256, NULL);
    object_class_property_add_str(oc, "firmware-initramfs-sha256",
                                  raspi4b_get_initramfs_sha256, NULL);
    object_class_property_add_str(oc, "firmware-final-device-tree-sha256",
                                  raspi4b_get_final_device_tree_sha256, NULL);
    object_class_property_add(oc, "otp-bootmode", "uint32",
                              raspi4b_get_otp_bootmode, NULL, NULL, NULL);
    object_class_property_add(oc, "otp-secure-boot-flags", "uint32",
                              raspi4b_get_otp_secure_boot_flags,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "otp-board-revision", "uint32",
                              raspi4b_get_otp_board_revision,
                              NULL, NULL, NULL);
    object_class_property_add_bool(oc, "otp-secure-boot",
                                   raspi4b_get_otp_secure_boot, NULL);
    object_class_property_add(
        oc, "otp-provision-rows-programmed", "uint8",
        raspi4b_get_otp_provision_rows_programmed, NULL, NULL, NULL);
    object_class_property_add(oc, "reset-status", "uint32",
                              raspi4b_get_reset_status, NULL, NULL, NULL);
    object_class_property_add_bool(oc, "boot-wake-on-gpio",
                                   raspi4b_get_wake_on_gpio, NULL);
    object_class_property_add_bool(oc, "boot-power-off-on-halt",
                                   raspi4b_get_power_off_on_halt, NULL);
    object_class_property_add_str(oc, "halt-state",
                                  raspi4b_get_halt_state, NULL);
    object_class_property_add_bool(oc, "global-en",
                                   raspi4b_get_global_en,
                                   raspi4b_set_global_en);
    object_class_property_add(oc, "board-revision", "uint32",
                              raspi4b_get_board_revision,
                              raspi4b_set_board_revision, NULL, NULL);
    object_class_property_set_description(
        oc, "board-revision",
        "Optional exact new-style Pi board revision; model and RAM bits "
        "must match the selected machine and memory size");
    object_class_property_add(oc, "min-boot-version", "uint32",
                              raspi4b_get_min_boot_version,
                              raspi4b_set_min_boot_version, NULL, NULL);
    object_class_property_set_description(
        oc, "min-boot-version",
        "Board manufacturing minimum EEPROM MFG_VER exposed to firmware; "
        "the undocumented physical OTP bit encoding is not modeled");
    object_class_property_add_str(oc, "memory-model",
                                  raspi4b_get_memory_model, NULL);
    object_class_property_add(oc, "boot-partition", "uint8",
                              raspi4b_get_boot_partition, NULL, NULL, NULL);
    object_class_property_add(oc, "selected-boot-partition", "uint8",
                              raspi4b_get_selected_boot_partition,
                              NULL, NULL, NULL);
    object_class_property_add_bool(oc, "boot-partition-walk",
                                   raspi4b_get_partition_walk, NULL);
    object_class_property_add(oc, "selected-boot-mode", "uint8",
                              raspi4b_get_selected_boot_mode,
                              NULL, NULL, NULL);
    object_class_property_add_bool(
        oc, "boot-eeprom-build-timestamp-valid",
        raspi4b_get_eeprom_build_timestamp_valid, NULL);
    object_class_property_add(
        oc, "boot-eeprom-build-timestamp", "uint32",
        raspi4b_get_eeprom_build_timestamp, NULL, NULL, NULL);
    object_class_property_add_bool(
        oc, "boot-eeprom-update-timestamp-valid",
        raspi4b_get_eeprom_update_timestamp_valid, NULL);
    object_class_property_add(
        oc, "boot-eeprom-update-timestamp", "uint32",
        raspi4b_get_eeprom_update_timestamp, NULL, NULL, NULL);
    object_class_property_add_bool(
        oc, "boot-eeprom-capabilities-valid",
        raspi4b_get_eeprom_capabilities_valid, NULL);
    object_class_property_add(
        oc, "boot-eeprom-capabilities", "uint32",
        raspi4b_get_eeprom_capabilities, NULL, NULL, NULL);
    object_class_property_add_str(oc, "boot-eeprom-version",
                                  raspi4b_get_eeprom_version, NULL);
    object_class_property_add(oc, "bootloader-signed", "uint32",
                              raspi4b_get_bootloader_signed,
                              NULL, NULL, NULL);
    object_class_property_add(
        oc, "boot-eeprom-config-append-size", "uint32",
        raspi4b_get_eeprom_config_append_size, NULL, NULL, NULL);
    object_class_property_add_str(
        oc, "boot-eeprom-config-append-sha256",
        raspi4b_get_eeprom_config_append_sha256, NULL);
    object_class_property_add(oc, "usb-boot-selected-index", "uint8",
                              raspi4b_get_usb_boot_selected_index,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "usb-boot-selected-device", "uint8",
                              raspi4b_get_usb_boot_selected_device,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "usb-boot-selected-lun", "uint8",
                              raspi4b_get_usb_boot_selected_lun,
                              NULL, NULL, NULL);
    object_class_property_add_bool(
        oc, "usb-boot-identity-valid",
        raspi4b_get_usb_boot_identity_valid, NULL);
    object_class_property_add(oc, "usb-boot-version", "uint8",
                              raspi4b_get_usb_boot_version,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "usb-boot-route-string", "uint32",
                              raspi4b_get_usb_boot_route_string,
                              NULL, NULL, NULL);
    object_class_property_add(oc, "usb-boot-root-hub-port", "uint32",
                              raspi4b_get_usb_boot_root_hub_port,
                              NULL, NULL, NULL);
    object_class_property_add_str(
        oc, "usb-msd-exclude-vid-pid",
        raspi4b_get_usb_msd_exclude_vid_pid, NULL);
    object_class_property_add(
        oc, "usb-boot-excluded-device-count", "uint8",
        raspi4b_get_usb_boot_excluded_device_count, NULL, NULL, NULL);
    object_class_property_add(
        oc, "usb-boot-eligible-device-count", "uint8",
        raspi4b_get_usb_boot_eligible_device_count, NULL, NULL, NULL);
    object_class_property_add(
        oc, "usb-boot-last-excluded-vid-pid", "uint32",
        raspi4b_get_usb_boot_last_excluded_vid_pid, NULL, NULL, NULL);
    object_class_property_add_str(oc, "usb-boot-transport",
                                  raspi4b_get_usb_boot_transport, NULL);
    object_class_property_add(
        oc, "usb-boot-controller-read-commands", "uint64",
        raspi4b_get_usb_boot_controller_reads, NULL, NULL, NULL);
    object_class_property_add(
        oc, "usb-boot-controller-read-bytes", "uint64",
        raspi4b_get_usb_boot_controller_bytes, NULL, NULL, NULL);
    object_class_property_add(
        oc, "usb-boot-controller-command-failures", "uint64",
        raspi4b_get_usb_boot_controller_failures, NULL, NULL, NULL);
    object_class_property_add(
        oc, "usb-boot-bot-recoveries", "uint64",
        raspi4b_get_usb_boot_bot_recoveries, NULL, NULL, NULL);
    object_class_property_add(
        oc, "usb-boot-bot-phase-errors", "uint64",
        raspi4b_get_usb_boot_bot_phase_errors, NULL, NULL, NULL);
    object_class_property_add(
        oc, "usb-boot-bot-cases-tested", "uint64",
        raspi4b_get_usb_boot_bot_cases_tested, NULL, NULL, NULL);
    object_class_property_add_str(oc, "reset-cause",
                                  raspi4b_get_reset_cause, NULL);
}

static void raspi4b_machine_instance_init(Object *obj)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    s->sd_recovery_enabled = true;
    s->network_boot_wire = true;
    s->global_en = true;
    s->provision_lock_fd = -1;
    s->eeprom_nwp = true;
    s->thermal_temperature_millicelsius = 25000;
    s->thermal_sensor_valid = true;
    s->eeprom_stuck_zero_offset = UINT64_MAX;
    s->eeprom_stuck_zero_mask = 1;
    s->eeprom_fail_after = UINT64_MAX;
    s->eeprom_fail_stage = RASPI4_EEPROM_FAIL_PROGRAM;
    s->otp_provision_fail_after = UINT8_MAX;
    s->emmc_data_error_after = UINT64_MAX;
    s->emmc_data_error_count = 1;
    s->max_restarts = -1;
    s->sd_overcurrent_check = true;
    s->sd_quirks = 0;
    s->sd_power_enabled = true;
    s->usb_msd_discover_timeout = RASPI4_USB_MSD_DISCOVER_TIMEOUT;
    s->usb_msd_lun_timeout = RASPI4_USB_MSD_LUN_TIMEOUT;
    s->usb_msd_startup_delay = RASPI4_USB_MSD_STARTUP_DELAY;
    s->usb_msd_power_off_time = RASPI4_USB_MSD_PWR_OFF_TIME;
    s->usb_power_enabled = true;
    s->boot_watchdog_timeout = 0;
    s->boot_watchdog_partition = 0;
    s->usb_boot_selected_index = UINT8_MAX;
    s->usb_boot_selected_device = UINT8_MAX;
    s->usb_boot_selected_lun = UINT8_MAX;
    s->usb_boot_last_excluded_vid_pid = UINT32_MAX;
    s->net_boot_max_retries = 0;
    s->dhcp_timeout = RASPI4_DHCP_TIMEOUT;
    s->dhcp_req_timeout = RASPI4_DHCP_REQ_TIMEOUT;
    s->dhcp_option97 = RASPI4_DHCP_OPTION97_DEFAULT;
    s->netconsole_source_port = RASPI4_NETCONSOLE_SOURCE_PORT;
    s->netconsole_destination_port =
        RASPI4_NETCONSOLE_DESTINATION_PORT;
    s->netconsole_destination_ip = UINT32_MAX;
    s->tftp_ip_set = false;
    s->tftp_ip = 0;
    s->client_ip_set = false;
    s->client_ip = 0;
    s->subnet_set = false;
    s->subnet = 0;
    s->gateway_set = false;
    s->gateway = 0;
    s->tftp_file_timeout = RASPI4_TFTP_FILE_TIMEOUT;
}

static void raspi_cm4_machine_instance_init(Object *obj)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    s->cm4 = true;
    s->sd_recovery_enabled = false;
}

static void raspi4b_machine_finalize(Object *obj)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    if (s->provision_exit_registered) {
        qemu_remove_exit_notifier(&s->provision_exit_notifier);
        s->provision_exit_registered = false;
        raspi4_provision_release(s);
    }
    if (s->provision_lock_fd >= 0) {
        qemu_unlock_fd(s->provision_lock_fd, 0, 0);
        close(s->provision_lock_fd);
        s->provision_lock_fd = -1;
    }

    qemu_unregister_reset(raspi4b_boot_reset, s);
    if (s->usb_boot_notifiers_registered) {
        for (unsigned int i = 0; i < s->usb_boot_device_count; i++) {
            notifier_remove(&s->usb_boot_insert_notifiers[i].notifier);
            notifier_remove(&s->usb_boot_remove_notifiers[i].notifier);
        }
        s->usb_boot_notifiers_registered = false;
    }
    if (s->sd_notifiers_registered) {
        notifier_remove(&s->sd_insert_notifier);
        notifier_remove(&s->sd_remove_notifier);
        s->sd_notifiers_registered = false;
    }
    if (s->network_boot_notifiers_registered) {
        notifier_remove(&s->network_boot_insert_notifier);
        notifier_remove(&s->network_boot_remove_notifier);
        s->network_boot_notifiers_registered = false;
    }
    if (s->boot_vmstate_registered) {
        vmstate_unregister(NULL, &vmstate_raspi4_boot, s);
        s->boot_vmstate_registered = false;
    }
    timer_free(s->recovery_reboot_timer);
    timer_free(s->eeprom_flash_timer);
    timer_free(s->boot_timeout_timer);
    timer_free(s->network_dhcp_retransmit_timer);
    timer_free(s->network_tftp_retransmit_timer);
    timer_free(s->boot_restart_timer);
    timer_free(s->boot_watchdog_timer);
    timer_free(s->hdmi_diagnostics_timer);
    if (s->usb_boot_hotplug_bh) {
        qemu_bh_delete(s->usb_boot_hotplug_bh);
    }
    if (s->sd_hotplug_bh) {
        qemu_bh_delete(s->sd_hotplug_bh);
    }
    if (s->network_boot_hotplug_bh) {
        qemu_bh_delete(s->network_boot_hotplug_bh);
    }
    if (s->network_boot_vmstate) {
        qemu_del_vm_change_state_handler(s->network_boot_vmstate);
    }
    g_free(s->eeprom_drive);
    g_free(s->eeprom_flash_image);
    g_free(s->eeprom_bootconf);
    g_free(s->hat_eeprom_drive);
    g_free(s->eeprom_status_drive);
    g_free(s->hdmi_edid_file[0]);
    g_free(s->hdmi_edid_file[1]);
    g_free(s->usb_boot_drive);
    g_free(s->usb_boot_drives);
    g_free(s->usb_boot_controller);
    g_free(s->network_boot_drive);
    g_free(s->nvme_drive);
    g_free(s->wireless_netdev);
    g_free(s->network_tftp_data);
    g_free(s->network_http_response);
    g_free(s->network_http_ooo_data);
    g_free(s->network_http_ooo_valid);
    raspi4_network_tls_clear(s);
    g_free(s->http_tls_creds);
    g_free(s->bootsys_trusted_sha256);
    g_free(s->recovery_trusted_sha256);
    g_free(s->rpiboot_bootcode_trusted_sha256);
    g_free(s->network_tftp_filename);
    g_free(s->network_tftp_expected_sha256);
    raspi4_network_artifacts_clear(s);
    g_free(s->emmc_drive);
    g_free(s->emmc_boot_drive);
    g_free(s->emmc_rpmb_drive);
    g_free(s->emmc_cid);
    g_free(s->otp_drive);
    g_free(s->gpio_chardev);
    g_free(s->provision_state_file);
    g_free(s->rpiboot_bootcode);
    g_free(s->rpiboot_config);
    g_free(s->rpiboot_boot_img);
    g_free(s->boot_state);
    g_free(s->boot_source);
    g_free(s->secure_boot_status);
    g_free(s->secure_image_sha256);
    g_free(s->secure_signature_sha256);
    g_free(s->firmware_file);
    g_free(s->firmware_status);
    g_free(s->handoff_status);
    g_free(s->firmware_sha256);
    g_free(s->fixup_sha256);
    g_free(s->kernel_sha256);
    g_free(s->device_tree_sha256);
    g_free(s->cmdline_sha256);
    g_free(s->initramfs_sha256);
    if (s->firmware_config_initialized) {
        raspi_firmware_config_clear(&s->firmware_config);
    }
    raspi_arm_handoff_clear(&s->handoff);
    raspi_overlay_state_clear(&s->overlay_state);
    g_free(s->recovery_status);
    g_free(s->reset_cause);
}

static const TypeInfo raspi4b_machine_type = {
    .name           = TYPE_RASPI4B_MACHINE,
    .parent         = TYPE_RASPI_BASE_MACHINE,
    .instance_size  = sizeof(Raspi4bMachineState),
    .instance_init  = raspi4b_machine_instance_init,
    .instance_finalize = raspi4b_machine_finalize,
    .class_init     = raspi4b_machine_class_init,
    .interfaces     = aarch64_machine_interfaces,
};

static void raspi_cm4_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    RaspiBaseMachineClass *rmc = RASPI_BASE_MACHINE_CLASS(oc);

    rmc->board_rev = RASPI_CM4_BOARD_REVISION;
    raspi_machine_class_common_init(mc, rmc->board_rev);
    mc->default_nic = TYPE_BCM2711_GENET;
    mc->auto_create_sdcard = false;
    mc->init = raspi4b_machine_init;
    object_class_property_add_str(oc, "emmc-drive",
                                  raspi_cm4_get_emmc_drive,
                                  raspi_cm4_set_emmc_drive);
    object_class_property_set_description(
        oc, "emmc-drive",
        "Named if=none block backend containing persistent CM4 eMMC bytes");
    object_class_property_add_str(oc, "emmc-boot-drive",
                                  raspi_cm4_get_emmc_boot_drive,
                                  raspi_cm4_set_emmc_boot_drive);
    object_class_property_set_description(
        oc, "emmc-boot-drive",
        "Optional backend containing boot0 followed by equal-sized boot1");
    object_class_property_add_str(oc, "emmc-rpmb-drive",
                                  raspi_cm4_get_emmc_rpmb_drive,
                                  raspi_cm4_set_emmc_rpmb_drive);
    object_class_property_set_description(
        oc, "emmc-rpmb-drive",
        "Optional backend containing the separate persistent RPMB area");
    object_class_property_add_str(oc, "emmc-cid",
                                  raspi_cm4_get_emmc_cid,
                                  raspi_cm4_set_emmc_cid);
    object_class_property_set_description(
        oc, "emmc-cid",
        "Optional eMMC CID as 30 payload hex characters or a 32-character "
        "raw/Linux-sysfs value");
    object_class_property_add_str(oc, "emmc-data-error",
                                  raspi_cm4_get_emmc_data_error,
                                  raspi_cm4_set_emmc_data_error);
    object_class_property_set_description(
        oc, "emmc-data-error",
        "Deterministic eMMC controller data fault: none, timeout, or crc");
    object_class_property_add(oc, "emmc-data-error-after", "uint64",
                              raspi_cm4_get_emmc_data_error_after,
                              raspi_cm4_set_emmc_data_error_after, NULL, NULL);
    object_class_property_set_description(
        oc, "emmc-data-error-after",
        "Inject after this many successful controller data bytes (512-byte "
        "aligned)");
    object_class_property_add(oc, "emmc-data-error-count", "uint32",
                              raspi_cm4_get_emmc_data_error_count,
                              raspi_cm4_set_emmc_data_error_count, NULL, NULL);
    object_class_property_set_description(
        oc, "emmc-data-error-count",
        "Number of deterministic eMMC controller data faults to inject");
    object_class_property_add(oc, "emmc-cache-size", "size",
                              raspi_cm4_get_emmc_cache_size,
                              raspi_cm4_set_emmc_cache_size, NULL, NULL);
    object_class_property_set_description(
        oc, "emmc-cache-size",
        "Advertised and modeled eMMC volatile write-cache size");
    object_class_property_add_bool(
        oc, "emmc-cache-power-loss-on-reset",
        raspi_cm4_get_emmc_cache_power_loss,
        raspi_cm4_set_emmc_cache_power_loss);
    object_class_property_set_description(
        oc, "emmc-cache-power-loss-on-reset",
        "Discard unflushed eMMC cache entries on system reset");
    object_class_property_add(
        oc, "emmc-cache-flush-sector-delay-us", "uint64",
        raspi_cm4_get_emmc_cache_flush_delay,
        raspi_cm4_set_emmc_cache_flush_delay, NULL, NULL);
    object_class_property_set_description(
        oc, "emmc-cache-flush-sector-delay-us",
        "Virtual delay before each durable eMMC cache sector writeback");
    object_class_property_add(
        oc, "emmc-program-sector-delay-us", "uint64",
        raspi_cm4_get_emmc_program_delay,
        raspi_cm4_set_emmc_program_delay, NULL, NULL);
    object_class_property_set_description(
        oc, "emmc-program-sector-delay-us",
        "Virtual delay before each durable uncached eMMC sector program");
    object_class_property_add(
        oc, "emmc-erase-group-delay-us", "uint64",
        raspi_cm4_get_emmc_erase_delay,
        raspi_cm4_set_emmc_erase_delay, NULL, NULL);
    object_class_property_set_description(
        oc, "emmc-erase-group-delay-us",
        "Virtual delay before each durable 512 KiB eMMC erase group");
    object_class_property_add_str(oc, "provision-state-file",
                                  raspi_cm4_get_provision_state_file,
                                  raspi_cm4_set_provision_state_file);
    object_class_property_set_description(
        oc, "provision-state-file",
        "Opt-in fail-closed QEMU/RPIBOOT/mass-storage ownership state file");
    object_class_property_add_str(oc, "provision-state",
                                  raspi_cm4_get_provision_state, NULL);
    object_class_property_add_bool(
        oc, "provision-flash-complete",
        raspi_cm4_get_provision_flash_complete,
        raspi_cm4_set_provision_flash_complete);
    object_class_property_set_description(
        oc, "provision-flash-complete",
        "Publish boot-ready after the continued guest eMMC is verified "
        "and flushed");
    object_class_property_add_bool(oc, "provision-recover-stale",
                                   raspi_cm4_get_provision_recover_stale,
                                   raspi_cm4_set_provision_recover_stale);
    object_class_property_set_description(
        oc, "provision-recover-stale",
        "Opt in to recovering an unlocked stale qemu-owned lifecycle state");
    object_class_property_add_str(oc, "provision-recovery",
                                  raspi_cm4_get_provision_recovery, NULL);
    object_class_property_set_description(
        oc, "provision-recovery",
        "Stale lifecycle recovery performed during machine initialization");
}

static const TypeInfo raspi_cm4_machine_type = {
    .name           = TYPE_RASPI_CM4_MACHINE,
    .parent         = TYPE_RASPI4B_MACHINE,
    .instance_init  = raspi_cm4_machine_instance_init,
    .class_init     = raspi_cm4_machine_class_init,
};

static void raspi4b_machine_register_type(void)
{
    type_register_static(&raspi4_vl805_type);
    type_register_static(&raspi4_pcie_root_port_type);
    type_register_static(&raspi4b_machine_type);
    type_register_static(&raspi_cm4_machine_type);
}

type_init(raspi4b_machine_register_type)
