/*
 * Raspberry Pi 4 boot attempt sequencing and media selection
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/arm/raspi4b-internal.h"

const char *raspi4_boot_media_name(const Raspi4bMachineState *s)
{
    return s->cm4 ? "emmc" : "sd-card";
}

const char *raspi4_boot_source_name(const Raspi4bMachineState *s,
                                           uint32_t nibble)
{
    static const char * const names[] = {
        [0x0] = "sd-card-detect",
        [0x1] = "sd-card",
        [0x2] = "network",
        [0x3] = "rpiboot",
        [0x4] = "usb-msd",
        [0x5] = "bcm-usb-msd",
        [0x6] = "nvme",
        [0x7] = "http",
        [0xe] = "stop",
        [0xf] = "restart",
    };

    if (nibble == 0x1 && s->cm4) {
        return "emmc";
    }
    return nibble < ARRAY_SIZE(names) && names[nibble] ? names[nibble] : NULL;
}

bool raspi4_try_sd_recovery(Raspi4bMachineState *s)
{
    static const char recovery_name[RASPI_FAT_SHORT_NAME_LEN] =
        "RECOVERYBIN";
    static const char recovery_done_name[RASPI_FAT_SHORT_NAME_LEN] =
        "RECOVERY000";
    static const char update_name[RASPI_FAT_SHORT_NAME_LEN] =
        "PIEEPROMUPD";
    static const char permanent_name[RASPI_FAT_SHORT_NAME_LEN] =
        "PIEEPROMBIN";
    static const char signature_name[RASPI_FAT_SHORT_NAME_LEN] =
        "PIEEPROMSIG";
    static const char config_name[RASPI_FAT_SHORT_NAME_LEN] =
        "CONFIG  TXT";
    RaspiFatVolume volume;
    RaspiFatFile recovery_file;
    RaspiFatFile update_file;
    RaspiFatFile signature_file;
    RaspiFatFile config_file;
    g_autofree uint8_t *recovery = NULL;
    g_autofree uint8_t *update = NULL;
    g_autofree uint8_t *signature = NULL;
    g_autofree uint8_t *config = NULL;
    RaspiFatResult result;
    Raspi4ProgramResult program_result;
    size_t recovery_length;
    size_t update_length;
    size_t signature_length;
    size_t config_length = 0;
    uint8_t provision_key_hash[32];
    Raspi4OtpProvisionResult provision_result = RASPI4_OTP_PROVISION_NONE;
    bool update_timestamp_valid;
    uint32_t update_timestamp;
    int eeprom_wp_request = -1;
    bool rename_after_update = true;
    int64_t eeprom_length;
    const char *recovery_error;

    /*
     * Incoming destinations may inspect shared media but must not start a
     * second recovery transaction while their block nodes are inactive.  The
     * source recovery outcome is restored by behavioral VMState.
     */
    if (runstate_check(RUN_STATE_INMIGRATE)) {
        return false;
    }

    if (!s->sd_recovery_enabled || !s->sd || !blk_is_inserted(s->sd)) {
        return false;
    }
    if (!raspi_fat_open(&volume, s->sd, NULL)) {
        return false;
    }
    result = raspi_fat_find_file(&volume, recovery_name, &recovery_file,
                                 NULL);
    if (result != RASPI_FAT_FOUND) {
        return false;
    }
    recovery = raspi_fat_read_file(&volume, &recovery_file,
                                   RASPI4_RECOVERY_MAX_SIZE,
                                   &recovery_length, NULL);
    if (!recovery || !recovery_length) {
        raspi4_set_recovery_status(s, "recovery-invalid");
        raspi4_set_boot_observation(s, "recovery-invalid", "sd-card");
        trace_raspi4b_boot_event("recovery", "recovery.read", "sd-card",
                                 "failure", 0);
        return true;
    }

    recovery_error = raspi4_validate_recovery(
        s, recovery, recovery_length);
    if (recovery_error) {
        raspi4_set_recovery_status(s, recovery_error);
        raspi4_set_boot_observation(s, recovery_error, "sd-card");
        trace_raspi4b_boot_event("recovery", "rom.recovery-auth", "sd-card",
                                 "failure", recovery_length);
        return true;
    }

    raspi4_set_recovery_status(s, "recovery-discovered");
    trace_raspi4b_boot_event("recovery", "rom.recovery-auth", "sd-card",
                             "success", s->recovery_key_index);
    trace_raspi4b_boot_event("recovery", "recovery.discovered", "sd-card",
                             "success", recovery_length);
    result = raspi_fat_find_file(&volume, update_name, &update_file,
                                 NULL);
    if (result == RASPI_FAT_NOT_FOUND) {
        result = raspi_fat_find_file(&volume, permanent_name, &update_file,
                                     NULL);
        rename_after_update = false;
    }
    if (result != RASPI_FAT_FOUND) {
        raspi4_set_recovery_status(s, "recovery-executed");
        raspi4_set_boot_observation(s, "recovery-executed", "sd-card");
        trace_raspi4b_boot_event("recovery", "recovery.execute", "sd-card",
                                 "success", 0);
        return true;
    }
    update = raspi_fat_read_file(&volume, &update_file,
                                 RASPI4_EEPROM_SIZE, &update_length,
                                 NULL);
    result = raspi_fat_find_file(&volume, signature_name, &signature_file,
                                 NULL);
    if (!update || result != RASPI_FAT_FOUND) {
        goto invalid_signature;
    }
    signature = raspi_fat_read_file(&volume, &signature_file,
                                    RASPI4_SIGNATURE_MAX_SIZE,
                                    &signature_length, NULL);
    if (!signature ||
        !raspi4_recovery_signature_valid(update, update_length, signature,
                                         signature_length,
                                         &update_timestamp_valid,
                                         &update_timestamp)) {
        goto invalid_signature;
    }
    result = raspi_fat_find_file(&volume, config_name, &config_file, NULL);
    if (result == RASPI_FAT_FOUND) {
        config = raspi_fat_read_file(&volume, &config_file, 64 * KiB,
                                     &config_length, NULL);
        if (!config) {
            raspi4_set_recovery_status(s, "recovery-provision-invalid");
            raspi4_set_boot_observation(
                s, "recovery-provision-invalid", "sd-card");
            return true;
        }
        if (!raspi4_recovery_parse_write_protect(
                config, config_length, &eeprom_wp_request)) {
            raspi4_set_recovery_status(
                s, "recovery-write-protect-config-invalid");
            raspi4_set_boot_observation(
                s, "recovery-write-protect-config-invalid", "sd-card");
            return true;
        }
        provision_result = raspi4_secure_provision_prepare(
            s, config, config_length, update, update_length,
            provision_key_hash);
        if (provision_result != RASPI4_OTP_PROVISION_NONE &&
            provision_result != RASPI4_OTP_PROVISION_READY) {
            const char *status =
                provision_result == RASPI4_OTP_PROVISION_SIGNATURE_INVALID ?
                    "recovery-provision-signature-invalid" :
                provision_result == RASPI4_OTP_PROVISION_KEY_MISMATCH ?
                    "recovery-provision-key-mismatch" :
                provision_result ==
                    RASPI4_OTP_PROVISION_PERSISTENCE_REQUIRED ?
                    "recovery-provision-persistence-required" :
                provision_result == RASPI4_OTP_PROVISION_JTAG_UNSUPPORTED ?
                    "recovery-provision-jtag-unsupported" :
                    "recovery-provision-invalid";

            raspi4_set_recovery_status(s, status);
            raspi4_set_boot_observation(s, status, "sd-card");
            return true;
        }
    } else if (result != RASPI_FAT_NOT_FOUND) {
        raspi4_set_recovery_status(s, "recovery-provision-invalid");
        raspi4_set_boot_observation(
            s, "recovery-provision-invalid", "sd-card");
        return true;
    }
    if (!s->eeprom || !blk_is_inserted(s->eeprom)) {
        raspi4_set_recovery_status(s, "recovery-no-eeprom");
        raspi4_set_boot_observation(s, "recovery-no-eeprom", "sd-card");
        trace_raspi4b_boot_event("recovery", "eeprom.no-device", "sd-card",
                                 "failure", update_length);
        return true;
    }
    eeprom_length = blk_getlength(s->eeprom);
    if (eeprom_length < 0 || update_length != eeprom_length) {
        raspi4_set_recovery_status(s, "recovery-size-invalid");
        raspi4_set_boot_observation(s, "recovery-size-invalid", "sd-card");
        trace_raspi4b_boot_event("recovery", "eeprom.invalid-size",
                                 "sd-card", "failure", update_length);
        return true;
    }
    s->eeprom_nwp_sampled = raspi4_eeprom_nwp_live(
        s, &s->eeprom_nwp_bridge_driven);
    if (eeprom_wp_request >= 0 &&
        !!eeprom_wp_request != s->eeprom_write_protect) {
        if (!s->eeprom_nwp_sampled) {
            raspi4_set_recovery_status(
                s, "recovery-write-protect-config-locked");
            raspi4_set_boot_observation(
                s, "recovery-write-protect-config-locked", "sd-card");
            trace_raspi4b_boot_event(
                "recovery", "eeprom.write-protect-config", "sd-card",
                "failure", eeprom_wp_request);
            return true;
        }
        if (!eeprom_wp_request) {
            if (!raspi4_eeprom_status_store(s, false, NULL)) {
                raspi4_set_recovery_status(
                    s, "recovery-write-protect-status-error");
                raspi4_set_boot_observation(
                    s, "recovery-write-protect-status-error", "sd-card");
                return true;
            }
        }
    }
    if (s->eeprom_write_protect) {
        raspi4_set_recovery_status(s, "recovery-write-protected");
        raspi4_set_boot_observation(s, "recovery-write-protected",
                                    "sd-card");
        trace_raspi4b_boot_event("recovery", "eeprom.write-protected",
                                 "sd-card", "failure", update_length);
        return true;
    }

    program_result = raspi4_program_eeprom(s, update, update_length, true);
    if (program_result == RASPI4_PROGRAM_PENDING) {
        raspi4_set_recovery_status(s, "recovery-flash-active");
        raspi4_set_boot_observation(s, "recovery-flash-active", "sd-card");
        trace_raspi4b_boot_event("recovery", "eeprom.flash-active",
                                 "sd-card", "wait",
                                 s->eeprom_flash_stage);
        return true;
    }
    if (program_result == RASPI4_PROGRAM_NOR_VIOLATION) {
        raspi4_set_recovery_status(s, "recovery-nor-violation");
        raspi4_set_boot_observation(s, "recovery-nor-violation",
                                    "sd-card");
        trace_raspi4b_boot_event("recovery", "eeprom.nor-violation",
                                 "sd-card", "failure",
                                 s->eeprom_nor_violation_bits);
        return true;
    }
    if (program_result == RASPI4_PROGRAM_ERROR) {
        raspi4_set_recovery_status(s, "recovery-program-error");
        raspi4_set_boot_observation(s, "recovery-program-error", "sd-card");
        trace_raspi4b_boot_event("recovery", "eeprom.program", "sd-card",
                                 "failure", update_length);
        return true;
    }
    if (program_result != RASPI4_PROGRAM_OK) {
        const char *status;
        const char *event;

        switch (program_result) {
        case RASPI4_ERASE_INTERRUPTED:
            status = "recovery-erase-interrupted";
            event = "eeprom.erase-interrupted";
            break;
        case RASPI4_PROGRAM_INTERRUPTED:
            status = "recovery-interrupted";
            event = "eeprom.program-interrupted";
            break;
        case RASPI4_VERIFY_INTERRUPTED:
            status = "recovery-verify-interrupted";
            event = "eeprom.verify-interrupted";
            break;
        case RASPI4_VERIFY_FAILED:
            status = "recovery-verify-failed";
            event = "eeprom.verify";
            break;
        default:
            g_assert_not_reached();
        }
        raspi4_set_recovery_status(s, status);
        raspi4_set_boot_observation(s, status, "sd-card");
        trace_raspi4b_boot_event("recovery", event, "sd-card", "failure",
                                 s->eeprom_fail_after);
        return true;
    }
    if (!raspi4_eeprom_update_timestamp_store(
            s, update_timestamp_valid, update_timestamp)) {
        raspi4_set_recovery_status(s, "recovery-update-status-error");
        raspi4_set_boot_observation(
            s, "recovery-update-status-error", "sd-card");
        return true;
    }
    if (eeprom_wp_request == 1) {
        if (!raspi4_eeprom_status_store(s, true, NULL)) {
            raspi4_set_recovery_status(
                s, "recovery-write-protect-status-error");
            raspi4_set_boot_observation(
                s, "recovery-write-protect-status-error", "sd-card");
            return true;
        }
        trace_raspi4b_boot_event(
            "recovery", "eeprom.write-protect-config", "sd-card",
            "success", 1);
    }
    if (provision_result == RASPI4_OTP_PROVISION_READY) {
        Raspi4OtpCommitResult commit_result =
            raspi4_secure_provision_commit(s, provision_key_hash);
        const char *status =
            commit_result == RASPI4_OTP_COMMIT_INTERRUPTED ?
                "recovery-provision-power-failed" :
                "recovery-provision-write-failed";

        if (commit_result != RASPI4_OTP_COMMIT_OK) {
            raspi4_set_recovery_status(s, status);
            raspi4_set_boot_observation(s, status, "sd-card");
            return true;
        }
    }
    if (rename_after_update &&
        s->eeprom_fail_stage == RASPI4_EEPROM_FAIL_RENAME) {
        raspi4_set_recovery_status(s, "recovery-rename-interrupted");
        raspi4_set_boot_observation(s, "recovery-rename-interrupted",
                                    "sd-card");
        trace_raspi4b_boot_event("recovery", "recovery.rename", "sd-card",
                                 "failure", 0);
        return true;
    }
    if (rename_after_update &&
        !raspi_fat_rename_file(&volume, &recovery_file, recovery_done_name,
                               NULL)) {
        raspi4_set_recovery_status(s, "recovery-rename-failed");
        raspi4_set_boot_observation(s, "recovery-rename-failed", "sd-card");
        trace_raspi4b_boot_event("recovery", "recovery.rename", "sd-card",
                                 "failure", update_length);
        return true;
    }
    if (rename_after_update) {
        if (s->eeprom_fail_stage == RASPI4_EEPROM_FAIL_REBOOT) {
            raspi4_set_recovery_status(s, "recovery-reboot-interrupted");
            raspi4_set_boot_observation(s, "recovery-reboot-interrupted",
                                        "sd-card");
            trace_raspi4b_boot_event("recovery", "recovery.reboot",
                                     "sd-card", "failure", 0);
            return true;
        }
        raspi4_set_recovery_status(s, "recovery-updated-reboot");
        raspi4_set_boot_observation(s, "recovery-updated-reboot", "sd-card");
        trace_raspi4b_boot_event("recovery", "eeprom.program", "sd-card",
                                 "restart", update_length);
        timer_mod(s->recovery_reboot_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                  RASPI4_RECOVERY_REBOOT_DELAY_MS);
        s->pending_boot_action = RASPI4_PENDING_RECOVERY_REBOOT;
    } else {
        raspi4_set_recovery_status(s, "recovery-updated-stop");
        raspi4_set_boot_observation(s, "recovery-updated-stop", "sd-card");
        trace_raspi4b_boot_event("recovery", "eeprom.program", "sd-card",
                                 "stop", update_length);
    }
    return true;

invalid_signature:
    raspi4_set_recovery_status(s, "recovery-signature-invalid");
    raspi4_set_boot_observation(s, "recovery-signature-invalid", "sd-card");
    trace_raspi4b_boot_event("recovery", "eeprom.signature", "sd-card",
                             "failure", update_file.size);
    return true;
}

void raspi4_boot_config_init(Raspi4bMachineState *s,
                                    Raspi4BootConfig *config)
{
    config->boot_order = RASPI4_DEFAULT_BOOT_ORDER;
    config->signed_boot = false;
    config->boot_uart = false;
    config->vl805 = false;
    config->wake_on_gpio = true;
    config->power_off_on_halt = false;
    config->bootvar0 = 0;
    config->partition = 0;
    config->partition_walk = true;
    config->max_restarts = -1;
    config->reboot_on_fatal_error = true;
    config->sd_boot_max_retries = 0;
    config->sd_overcurrent_check = true;
    config->sd_quirks = 0;
    config->usb_msd_discover_timeout = RASPI4_USB_MSD_DISCOVER_TIMEOUT;
    config->usb_msd_lun_timeout = RASPI4_USB_MSD_LUN_TIMEOUT;
    config->usb_msd_startup_delay = RASPI4_USB_MSD_STARTUP_DELAY;
    config->usb_msd_power_off_time = RASPI4_USB_MSD_PWR_OFF_TIME;
    memset(config->usb_msd_exclude_vid_pid, 0,
           sizeof(config->usb_msd_exclude_vid_pid));
    config->usb_msd_exclude_count = 0;
    config->boot_watchdog_timeout = 0;
    config->boot_watchdog_partition = 0;
    config->net_boot_max_retries = 0;
    config->dhcp_timeout = RASPI4_DHCP_TIMEOUT;
    config->dhcp_req_timeout = RASPI4_DHCP_REQ_TIMEOUT;
    config->dhcp_option97 = RASPI4_DHCP_OPTION97_DEFAULT;
    pstrcpy(config->pxe_option43, sizeof(config->pxe_option43),
            RASPI4_PXE_OPTION43_DEFAULT);
    config->netconsole_enabled = false;
    config->netconsole[0] = 0;
    config->netconsole_source_port = RASPI4_NETCONSOLE_SOURCE_PORT;
    config->netconsole_destination_port =
        RASPI4_NETCONSOLE_DESTINATION_PORT;
    config->netconsole_source_ip = 0;
    config->netconsole_destination_ip = UINT32_MAX;
    memset(config->netconsole_destination_mac, 0,
           sizeof(config->netconsole_destination_mac));
    memset(&config->mac_address, 0, sizeof(config->mac_address));
    config->mac_address_source = RASPI4_MAC_CONFIGURED;
    config->tftp_ip_set = false;
    config->tftp_ip = 0;
    config->tftp_prefix = 0;
    config->tftp_prefix_str[0] = 0;
    config->client_ip_set = false;
    config->client_ip = 0;
    config->subnet_set = false;
    config->subnet = 0;
    config->gateway_set = false;
    config->gateway = 0;
    config->tftp_file_timeout = RASPI4_TFTP_FILE_TIMEOUT;
    config->http_host_set = false;
    config->http_host[0] = 0;
    pstrcpy(config->http_path, sizeof(config->http_path),
            RASPI4_HTTP_DEFAULT_PATH);
    config->http_port = RASPI4_HTTP_DEFAULT_PORT;
    config->http_cacert_hash_set = false;
    config->http_cacert_hash[0] = 0;
    config->disable_hdmi = false;
    config->hdmi_delay = RASPI4_HDMI_DELAY_DEFAULT;
    config->net_install_enabled = !s->cm4;
    config->net_install_at_power_on = false;
    config->net_install_keyboard_wait = 900;
    config->enable_self_update = true;
    config->freeze_version = false;
}

static bool raspi4_boot_filter_value(const char *filter, const char *name,
                                     uint64_t maximum, uint64_t *value)
{
    g_autofree char *prefix = g_strdup_printf("[%s=", name);
    const char *number;
    const char *end;

    if (g_ascii_strncasecmp(filter, prefix, strlen(prefix))) {
        return false;
    }
    number = filter + strlen(prefix);
    if (qemu_strtou64(number, &end, 0, value) < 0 || *value > maximum ||
        strcmp(end, "]")) {
        return false;
    }
    return true;
}

static bool raspi4_boot_parse_u32(const char *text, size_t length,
                                  uint32_t *value)
{
    g_autofree char *number = g_strndup(text, length);
    const char *end;
    uint64_t parsed;

    if (!length || qemu_strtou64(number, &end, 0, &parsed) < 0 ||
        *end || parsed > UINT32_MAX) {
        return false;
    }
    *value = parsed;
    return true;
}

static bool raspi4_boot_variable(Raspi4bMachineState *s,
                                 const char *name, size_t length,
                                 uint32_t *value)
{
    if ((length == strlen("partition") &&
         !memcmp(name, "partition", length)) ||
        (length == strlen("boot_partition") &&
         !memcmp(name, "boot_partition", length))) {
        *value = s->boot_partition;
        return true;
    }

    if (length == strlen("cust_otp0") &&
        !memcmp(name, "cust_otp", strlen("cust_otp")) &&
        name[length - 1] >= '0' && name[length - 1] <= '7') {
        *value = bcm2835_otp_get_row(
            raspi4_otp(s),
            BCM2835_OTP_CUSTOMER_OTP + name[length - 1] - '0');
        return true;
    }

    return false;
}

/*
 * config.txt conditional expressions use unsigned 32-bit boot variables:
 * [ARG=VALUE], [ARG&MASK], [ARG&MASK=VALUE], [ARG<VALUE], [ARG>VALUE].
 * Return false only when the filter is not an expression at all.  Malformed
 * expressions and variables unavailable on BCM2711 are recognized but do not
 * match, preserving the firmware's fail-closed conditional behavior.
 */
/*
 * config.txt conditional expressions use unsigned 32-bit boot variables:
 * [ARG=VALUE], [ARG&MASK], [ARG&MASK=VALUE], [ARG<VALUE], [ARG>VALUE].
 * Return false only when the filter is not an expression at all.  Malformed
 * expressions and variables unavailable on BCM2711 are recognized but do not
 * match, preserving the firmware's fail-closed conditional behavior.
 */
static bool raspi4_boot_filter_expression(Raspi4bMachineState *s,
                                          const char *filter, bool *match)
{
    g_autofree char *expression = NULL;
    const char *amp;
    const char *equal;
    const char *less;
    const char *greater;
    const char *operator;
    uint32_t argument;
    uint32_t operand;
    size_t length;

    *match = false;
    length = strlen(filter);
    if (length < 3 || filter[0] != '[' || filter[length - 1] != ']') {
        return false;
    }

    expression = g_strndup(filter + 1, length - 2);
    amp = strchr(expression, '&');
    equal = strchr(expression, '=');
    less = strchr(expression, '<');
    greater = strchr(expression, '>');
    if (!amp && !equal && !less && !greater) {
        return false;
    }

    if (amp) {
        const char *mask = amp + 1;
        uint32_t mask_value;

        if (less || greater || strchr(amp + 1, '&') ||
            (equal && equal < amp) ||
            (equal && strchr(equal + 1, '=')) ||
            !raspi4_boot_variable(s, expression, amp - expression,
                                  &argument)) {
            return true;
        }
        if (equal) {
            if (!raspi4_boot_parse_u32(mask, equal - mask, &mask_value) ||
                !raspi4_boot_parse_u32(equal + 1, strlen(equal + 1),
                                       &operand)) {
                return true;
            }
            *match = (argument & mask_value) == operand;
        } else {
            if (!raspi4_boot_parse_u32(mask, strlen(mask), &mask_value)) {
                return true;
            }
            *match = (argument & mask_value) != 0;
        }
        return true;
    }

    if (!!equal + !!less + !!greater != 1) {
        return true;
    }
    operator = equal ? equal : (less ? less : greater);
    if (strchr(operator + 1, *operator) ||
        !raspi4_boot_variable(s, expression, operator - expression,
                              &argument) ||
        !raspi4_boot_parse_u32(operator + 1, strlen(operator + 1),
                               &operand)) {
        return true;
    }

    if (equal) {
        *match = argument == operand;
    } else if (less) {
        *match = argument < operand;
    } else {
        *match = argument > operand;
    }
    return true;
}


bool raspi4_boot_filters_active(const Raspi4BootFilterState *filters)
{
    return !filters->disabled && filters->model && filters->serial &&
           filters->gpio && filters->expression && filters->other;
}

void raspi4_boot_filters_reset(Raspi4BootFilterState *filters)
{
    *filters = (Raspi4BootFilterState) {
        .model = true,
        .serial = true,
        .gpio = true,
        .expression = true,
        .other = true,
    };
}

void raspi4_boot_filter_apply(Raspi4bMachineState *s,
                                     Raspi4BootFilterState *filters,
                                     const char *filter)
{
    bool expression_match;
    uint64_t value;

    if (!g_ascii_strcasecmp(filter, "[all]")) {
        raspi4_boot_filters_reset(filters);
        return;
    }
    if (!g_ascii_strcasecmp(filter, "[none]")) {
        filters->disabled = true;
        return;
    }
    if (!g_ascii_strcasecmp(filter, "[pi4]")) {
        filters->model = true;
        return;
    }
    if (!g_ascii_strcasecmp(filter, "[cm4]")) {
        filters->model = s->cm4;
        return;
    }
    if (!g_ascii_strncasecmp(filter, "[board-type=", 12)) {
        filters->model =
            raspi4_boot_filter_value(
                filter, "board-type", UINT8_MAX, &value) &&
            ((s->board_revision >> 4) & UINT8_MAX) == value;
        return;
    }
    if (!g_ascii_strncasecmp(filter, "[pi", 3) ||
        !g_ascii_strncasecmp(filter, "[cm", 3)) {
        filters->model = false;
        return;
    }
    if (!g_ascii_strncasecmp(filter, "[0x", 3)) {
        const char *end;
        uint32_t serial = bcm2835_otp_get_row(
            raspi4_otp(s), BCM2711_OTP_SERIAL_ROW);

        filters->serial =
            qemu_strtou64(filter + 1, &end, 0, &value) == 0 &&
            value <= UINT32_MAX && !strcmp(end, "]") && serial == value;
        return;
    }
    if (!g_ascii_strncasecmp(filter, "[gpio", 5)) {
        const char *equals = strchr(filter + 5, '=');
        g_autofree char *pin_filter = equals ?
            g_strndup(filter + 5, equals - filter - 5) : NULL;
        uint64_t pin = BCM2838_GPIO_NUM;
        uint64_t level = 2;
        const char *end = NULL;
        bool pin_valid = false;
        bool level_valid = false;
        int actual = -1;

        if (pin_filter) {
            pin_valid = qemu_strtou64(
                pin_filter, NULL, 10, &pin) == 0;
        }
        if (equals) {
            level_valid = qemu_strtou64(
                equals + 1, &end, 10, &level) == 0;
        }
        if (pin_valid && level_valid && pin < BCM2838_GPIO_NUM &&
            level <= 1 && !strcmp(end, "]")) {
            actual = bcm2838_gpio_get_external_input(
                &s->soc.peripherals.gpio, pin);
        }
        filters->gpio = actual >= 0 && actual == level;
        return;
    }
    if (raspi4_boot_filter_expression(s, filter, &expression_match)) {
        filters->expression = expression_match;
        return;
    }
    filters->other = false;
}

static unsigned int raspi4_boot_order_nibbles(uint32_t boot_order)
{
    return boot_order ? DIV_ROUND_UP(32 - clz32(boot_order), 4) : 1;
}

static void raspi4_boot_uart_write(Raspi4bMachineState *s,
                                   const char *text)
{
    if (s->boot_uart_active) {
        raspi4_uart0_write(s, text, &s->boot_uart_bytes,
                           &s->boot_uart_lines);
    }
}

void raspi4_boot_uart_begin(Raspi4bMachineState *s)
{
    g_autofree char *line = NULL;

    if (!s->boot_uart_enabled || s->boot_uart_started) {
        return;
    }
    s->boot_uart_active = true;
    s->boot_uart_started = true;
    raspi4_uart0_configure(s);
    line = g_strdup_printf(
        "RPI4-BOOT: enabled order=0x%08" PRIx32
        " revision=0x%08" PRIx32 "\r\n",
        s->boot_order, s->board_revision);
    raspi4_boot_uart_write(s, line);
}

void raspi4_boot_uart_observation(Raspi4bMachineState *s,
                                         const char *state,
                                         const char *source)
{
    g_autofree char *line = NULL;

    if (!s->boot_uart_active) {
        return;
    }
    line = g_strdup_printf(
        "RPI4-BOOT: state=%s source=%s attempt=%" PRIu64 "\r\n",
        state, source, s->boot_attempt_count);
    raspi4_boot_uart_write(s, line);
}

void raspi4_boot_uart_end(Raspi4bMachineState *s)
{
    if (!s->boot_uart_active) {
        return;
    }
    raspi4_boot_uart_write(s, "RPI4-BOOT: second-stage\r\n");
    s->boot_uart_active = false;
}

bool raspi4_schedule_bootcode_delay(Raspi4bMachineState *s,
                                           const char *source)
{
    uint64_t delay_ms;

    if (s->bootcode_delay_consumed) {
        return false;
    }
    s->bootcode_delay_seconds = s->firmware_config.bootcode_delay;
    if (!s->bootcode_delay_seconds) {
        return false;
    }

    s->bootcode_delay_consumed = true;
    delay_ms = (uint64_t)s->bootcode_delay_seconds * 1000;
    s->pending_boot_action = RASPI4_PENDING_FIRMWARE_DELAY;
    raspi4_set_firmware_status(s, "bootcode-delay");
    raspi4_set_boot_observation(s, "firmware-delay", source);
    trace_raspi4b_boot_event("firmware", "firmware.bootcode-delay",
                             source, "wait", delay_ms);
    timer_mod(s->boot_timeout_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + delay_ms);
    return true;
}

static Raspi4SelfUpdateResult
raspi4_try_self_update(Raspi4bMachineState *s, RaspiFatVolume *volume,
                       const char *media)
{
    RaspiFatFile update_file;
    RaspiFatFile signature_file;
    g_autofree uint8_t *update = NULL;
    g_autofree uint8_t *signature = NULL;
    RaspiFatResult result;
    size_t update_size = 0;
    size_t signature_size = 0;

    s->self_update_status = RASPI4_SELF_UPDATE_NONE;
    /*
     * An incoming destination may inspect shared media but must not mutate
     * the EEPROM while its block nodes are inactive.  Migrated state restores
     * any source-side pending reboot.
     */
    if (runstate_check(RUN_STATE_INMIGRATE)) {
        return RASPI4_SELF_UPDATE_CONTINUE;
    }
    if (s->cm4) {
        s->self_update_status = RASPI4_SELF_UPDATE_UNSUPPORTED;
        return RASPI4_SELF_UPDATE_CONTINUE;
    }
    if (!s->enable_self_update) {
        s->self_update_status = RASPI4_SELF_UPDATE_DISABLED;
        return RASPI4_SELF_UPDATE_CONTINUE;
    }
    if (s->freeze_version) {
        s->self_update_status = RASPI4_SELF_UPDATE_FROZEN;
        return RASPI4_SELF_UPDATE_CONTINUE;
    }
    result = raspi_fat_find_path(
        volume, "pieeprom.upd", &update_file, NULL);
    if (result == RASPI_FAT_NOT_FOUND) {
        return RASPI4_SELF_UPDATE_CONTINUE;
    }
    if (result != RASPI_FAT_FOUND ||
        raspi_fat_find_path(volume, "pieeprom.sig",
                            &signature_file, NULL) != RASPI_FAT_FOUND) {
        return raspi4_apply_self_update(s, NULL, 0, NULL, 0, media);
    }
    update = raspi_fat_read_file(
        volume, &update_file, RASPI4_EEPROM_SIZE, &update_size, NULL);
    signature = raspi_fat_read_file(
        volume, &signature_file, RASPI4_SIGNATURE_MAX_SIZE,
        &signature_size, NULL);
    return raspi4_apply_self_update(
        s, update, update_size, signature, signature_size, media);
}

static Raspi4BootAttemptResult
raspi4_try_firmware_volume(Raspi4bMachineState *s, RaspiFatVolume *volume,
                           const char *media, const char *config_path)
{
    g_autofree uint8_t *start = NULL;
    g_autofree uint8_t *fixup = NULL;
    g_autofree uint8_t *kernel = NULL;
    g_autofree uint8_t *device_tree = NULL;
    g_autofree uint8_t *cmdline = NULL;
    g_autofree uint8_t *initramfs = NULL;
    RaspiFirmwareResolveResult result;

    if (!raspi_firmware_config_load_named(
            volume, &s->firmware_config, config_path, NULL)) {
        raspi4_set_firmware_status(s, "config-invalid");
        trace_raspi4b_boot_event("firmware", "firmware.config", media,
                                 "failure", 0);
        return RASPI4_BOOT_ATTEMPT_FAILED;
    }
    raspi4_apply_firmware_gpu_mem(s);
    raspi4_firmware_uart_begin(s);
    if (raspi4_schedule_bootcode_delay(s, media)) {
        return RASPI4_BOOT_ATTEMPT_PENDING;
    }
    result = raspi_firmware_resolve(volume, &s->firmware_config,
                                    &s->firmware_manifest, NULL);
    switch (result) {
    case RASPI_FIRMWARE_RESOLVE_ERROR:
        raspi4_set_firmware_status(s, "config-invalid");
        return RASPI4_BOOT_ATTEMPT_FAILED;
    case RASPI_FIRMWARE_MISSING_START:
        raspi4_set_firmware_status(s, "firmware-missing");
        return RASPI4_BOOT_ATTEMPT_FAILED;
    case RASPI_FIRMWARE_MISSING_FIXUP:
        raspi4_set_firmware_status(s, "fixup-missing");
        return RASPI4_BOOT_ATTEMPT_FAILED;
    case RASPI_FIRMWARE_MISSING_KERNEL:
        raspi4_set_firmware_status(s, "kernel-missing");
        return RASPI4_BOOT_ATTEMPT_FAILED;
    case RASPI_FIRMWARE_MISSING_DEVICE_TREE:
        raspi4_set_firmware_status(s, "device-tree-missing");
        return RASPI4_BOOT_ATTEMPT_FAILED;
    case RASPI_FIRMWARE_MISSING_INITRAMFS:
        raspi4_set_firmware_status(s, "initramfs-missing");
        return RASPI4_BOOT_ATTEMPT_FAILED;
    case RASPI_FIRMWARE_READY:
        raspi4_firmware_uart_manifest(s, media);
        break;
    default:
        g_assert_not_reached();
    }

    start = raspi4_read_firmware_artifact(
        volume, &s->firmware_manifest.start,
        RASPI4_FIRMWARE_MAX_SIZE, &s->firmware_size,
        &s->firmware_sha256);
    fixup = raspi4_read_firmware_artifact(
        volume, &s->firmware_manifest.fixup,
        RASPI4_FIXUP_MAX_SIZE, &s->fixup_size, &s->fixup_sha256);
    kernel = raspi4_read_firmware_artifact(
        volume, &s->firmware_manifest.kernel,
        RASPI4_KERNEL_MAX_SIZE, &s->kernel_size, &s->kernel_sha256);
    if (s->firmware_config.device_tree_enabled) {
        device_tree = raspi4_read_firmware_artifact(
            volume, &s->firmware_manifest.device_tree,
            RASPI4_DTB_MAX_SIZE, &s->device_tree_size,
            &s->device_tree_sha256);
    }
    if (s->firmware_manifest.cmdline_present) {
        cmdline = raspi4_read_firmware_artifact(
            volume, &s->firmware_manifest.cmdline,
            RASPI4_CMDLINE_MAX_SIZE, &s->cmdline_size,
            &s->cmdline_sha256);
    }
    if (s->firmware_manifest.initramfs_count) {
        initramfs = raspi4_read_initramfs(
            volume, &s->firmware_manifest, &s->initramfs_size,
            &s->initramfs_sha256);
    }
    if (!start || !fixup || !kernel ||
        (s->firmware_config.device_tree_enabled && !device_tree) ||
        (s->firmware_manifest.cmdline_present && !cmdline) ||
        (s->firmware_manifest.initramfs_count && !initramfs)) {
        raspi4_set_firmware_status(s, "artifact-read-error");
        return RASPI4_BOOT_ATTEMPT_FAILED;
    }
    return raspi4_finish_firmware(
        s, volume, media, kernel, device_tree, cmdline, initramfs,
        NULL, NULL);
}

static Raspi4BootAttemptResult raspi4_try_signed_image(
    Raspi4bMachineState *s, const uint8_t *boot_image, size_t boot_image_size,
    const uint8_t *boot_signature, size_t boot_signature_size,
    const char *media)
{
    return raspi4_try_signed_image_with_key(
        s, s->secure_public_key, boot_image, boot_image_size,
        boot_signature, boot_signature_size, media);
}

Raspi4BootAttemptResult raspi4_try_signed_image_with_key(
    Raspi4bMachineState *s,
    const uint8_t public_key[RASPI_SECURE_PUBLIC_KEY_SIZE],
    const uint8_t *boot_image, size_t boot_image_size,
    const uint8_t *boot_signature, size_t boot_signature_size,
    const char *media)
{
    RaspiFatVolume volume;
    RaspiSecureResult secure_result = raspi_secure_verify(
        public_key, boot_image, boot_image_size,
        boot_signature, boot_signature_size, NULL);

    s->secure_image_size = boot_image_size;
    s->secure_signature_size = boot_signature_size;
    g_free(s->secure_image_sha256);
    s->secure_image_sha256 = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, boot_image, boot_image_size);
    g_free(s->secure_signature_sha256);
    s->secure_signature_sha256 = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, boot_signature, boot_signature_size);
    if (secure_result != RASPI_SECURE_OK) {
        g_autofree char *status = g_strdup_printf(
            "secure-boot-%s", raspi_secure_result_name(secure_result));

        raspi4_set_firmware_status(s, status);
        raspi4_set_secure_boot_status(
            s, raspi_secure_result_name(secure_result));
        trace_raspi4b_boot_event("firmware", "firmware.secure-image",
                                 media, "failure", secure_result);
        return RASPI4_BOOT_ATTEMPT_FAILED;
    }
    if (!raspi_fat_open_memory(&volume, boot_image, boot_image_size, NULL)) {
        raspi4_set_firmware_status(s, "secure-boot-image-invalid");
        raspi4_set_secure_boot_status(s, "image-invalid");
        return RASPI4_BOOT_ATTEMPT_FAILED;
    }
    trace_raspi4b_boot_event("firmware", "firmware.secure-image",
                             media, "success", boot_image_size);
    raspi4_set_secure_boot_status(s, "image-verified");
    /*
     * A signed boot ramdisk has already consumed the outer tryboot switch;
     * the bootloader implicitly applies tryboot_a_b=1 inside the image.
     */
    return raspi4_try_firmware_volume(s, &volume, media, "config.txt");
}


void raspi4_autoboot_parse(RaspiFatVolume *volume, bool tryboot,
                                  Raspi4AutobootConfig *config)
{
    g_autofree uint8_t *contents = NULL;
    g_auto(GStrv) lines = NULL;
    RaspiFatFile file;
    size_t length;
    bool active = true;

    memset(config, 0, sizeof(*config));
    if (raspi_fat_find_path(volume, "autoboot.txt", &file, NULL) !=
        RASPI_FAT_FOUND) {
        return;
    }
    config->present = true;
    if (file.size > 512) {
        return;
    }
    contents = raspi_fat_read_file(volume, &file, 512, &length, NULL);
    if (!contents || length != file.size) {
        return;
    }
    contents = g_realloc(contents, length + 1);
    contents[length] = 0;
    lines = g_strsplit((char *)contents, "\n", -1);

    for (char **linep = lines; *linep; linep++) {
        char *comment = strchr(*linep, '#');
        char *line;
        char *separator;
        uint64_t value;

        if (comment) {
            *comment = 0;
        }
        line = g_strstrip(*linep);
        if (!line[0]) {
            continue;
        }
        if (line[0] == '[') {
            if (!g_ascii_strcasecmp(line, "[all]")) {
                active = true;
            } else if (!g_ascii_strcasecmp(line, "[none]")) {
                active = false;
            } else if (!g_ascii_strcasecmp(line, "[tryboot]")) {
                active = tryboot;
            } else {
                active = false;
            }
            continue;
        }
        if (!active) {
            continue;
        }
        separator = strchr(line, '=');
        if (!separator) {
            continue;
        }
        *separator = 0;
        separator = g_strstrip(separator + 1);
        line = g_strstrip(line);
        if (!g_ascii_strcasecmp(line, "boot_partition")) {
            if (qemu_strtou64(separator, NULL, 0, &value) < 0 ||
                value > 31) {
                return;
            }
            config->partition = value;
            config->partition_set = true;
        } else if (!g_ascii_strcasecmp(line, "tryboot_a_b")) {
            if (qemu_strtou64(separator, NULL, 0, &value) < 0 ||
                value > 1) {
                return;
            }
            config->tryboot_a_b = value;
        }
    }
    config->valid = true;
}

bool raspi4_boot_volume_is_bootable(Raspi4bMachineState *s,
                                           RaspiFatVolume *volume)
{
    RaspiFatFile file;
    const char *path;

    if (s->otp_secure_boot) {
        path = s->tryboot && !s->tryboot_a_b ?
            "tryboot.img" : "boot.img";
        return raspi_fat_find_path(volume, path, &file, NULL) ==
               RASPI_FAT_FOUND;
    }
    return raspi_fat_find_path(volume, "start4.elf", &file, NULL) ==
               RASPI_FAT_FOUND ||
           raspi_fat_find_path(volume, "start.elf", &file, NULL) ==
               RASPI_FAT_FOUND;
}


bool raspi4_boot_media_open(
    const Raspi4BootMediaReader *reader, RaspiFatVolume *volume,
    unsigned int partition, Error **errp)
{
    if (reader->read) {
        return raspi_fat_open_reader_partition(
            volume, reader->read, reader->opaque, reader->size,
            partition, errp);
    }
    return raspi_fat_open_partition(volume, reader->blk, partition, errp);
}

static Raspi4BootAttemptResult
raspi4_try_firmware_reader(Raspi4bMachineState *s,
                           const Raspi4BootMediaReader *reader,
                           const char *media)
{
    g_autofree uint8_t *boot_image = NULL;
    g_autofree uint8_t *boot_signature = NULL;
    RaspiFatVolume outer_volume;
    RaspiFatFile boot_image_file;
    RaspiFatFile boot_signature_file;
    const char *image_path;
    const char *signature_path;
    size_t boot_image_size;
    size_t boot_signature_size;
    Error *local_err = NULL;
    Raspi4SelfUpdateResult self_update;

    if ((!reader->read &&
         (!reader->blk || !blk_is_inserted(reader->blk))) ||
        !raspi4_open_boot_volume(
            s, reader, &outer_volume, reader->read ? &local_err : NULL)) {
        error_free(local_err);
        raspi4_set_firmware_status(s, "media-unavailable");
        return RASPI4_BOOT_ATTEMPT_FAILED;
    }
    self_update = raspi4_try_self_update(s, &outer_volume, media);
    if (self_update == RASPI4_SELF_UPDATE_PENDING) {
        return RASPI4_BOOT_ATTEMPT_PENDING;
    }
    if (self_update == RASPI4_SELF_UPDATE_FAILED) {
        return RASPI4_BOOT_ATTEMPT_FAILED;
    }
    image_path = s->tryboot && !s->tryboot_a_b ?
        "tryboot.img" : "boot.img";
    signature_path = s->tryboot && !s->tryboot_a_b ?
        "tryboot.sig" : "boot.sig";
    if (!s->otp_secure_boot) {
        return raspi4_try_firmware_volume(
            s, &outer_volume, media, raspi4_config_path(s));
    }
    if (!s->secure_public_key_valid ||
        raspi_fat_find_path(&outer_volume, image_path,
                            &boot_image_file, NULL) != RASPI_FAT_FOUND ||
        raspi_fat_find_path(&outer_volume, signature_path,
                            &boot_signature_file, NULL) != RASPI_FAT_FOUND) {
        raspi4_set_firmware_status(s, "secure-boot-image-missing");
        raspi4_set_secure_boot_status(s, "image-missing");
        return RASPI4_BOOT_ATTEMPT_FAILED;
    }
    boot_image = raspi_fat_read_file(
        &outer_volume, &boot_image_file, RASPI4_INITRAMFS_MAX_SIZE,
        &boot_image_size, NULL);
    boot_signature = raspi_fat_read_file(
        &outer_volume, &boot_signature_file, RASPI4_SIGNATURE_MAX_SIZE,
        &boot_signature_size, NULL);
    if (!boot_image || !boot_signature) {
        raspi4_set_firmware_status(s, "secure-boot-image-read-error");
        raspi4_set_secure_boot_status(s, "image-read-error");
        return RASPI4_BOOT_ATTEMPT_FAILED;
    }
    return raspi4_try_signed_image(
        s, boot_image, boot_image_size, boot_signature,
        boot_signature_size, media);
}

Raspi4BootAttemptResult
raspi4_try_firmware(Raspi4bMachineState *s, BlockBackend *blk,
                    const char *media)
{
    Raspi4BootMediaReader reader = {
        .blk = blk,
    };

    return raspi4_try_firmware_reader(s, &reader, media);
}

Raspi4BootAttemptResult
raspi4_try_rpiboot_firmware(Raspi4bMachineState *s)
{
    Raspi4BootMediaReader reader = {
        .read = raspi4_rpiboot_media_read,
        .opaque = s,
        .size = s->rpiboot_boot_img_size,
    };
    Raspi4BootAttemptResult result;

    if (!s->rpiboot_boot_img || !s->rpiboot_boot_img_size) {
        raspi4_set_firmware_status(s, "media-unavailable");
        return RASPI4_BOOT_ATTEMPT_FAILED;
    }
    result = raspi4_try_firmware_reader(s, &reader, "rpiboot");
    if (result == RASPI4_BOOT_ATTEMPT_READY) {
        /*
         * The exact received boot.img is transient firmware media only.
         * Release the modeled ROM callback so the handed-off kernel owns
         * DWC2, while the configured eMMC backend remains its flash target.
         */
        s->rpiboot_dwc2_active = false;
        dwc2_device_set_firmware_handlers(raspi4_dwc2(s), NULL, NULL, NULL);
        trace_raspi4b_boot_event(
            "boot-source", "boot-source.rpiboot", "rpiboot",
            "success", s->rpiboot_boot_img_size);
    }
    return result;
}

Raspi4BootAttemptResult
raspi4_try_usb_firmware(Raspi4bMachineState *s, const char *source)
{
    g_autofree uint8_t *bounce = g_malloc(127 * 512);
    Raspi4UsbBootReader usb_reader = {
        .machine = s,
        .tag = 0x52504900,
        .bounce = bounce,
    };
    Raspi4BootAttemptResult result = RASPI4_BOOT_ATTEMPT_FAILED;
    Error *local_err = NULL;

    s->usb_boot_selected_index = UINT8_MAX;
    s->usb_boot_selected_device = UINT8_MAX;
    s->usb_boot_selected_lun = UINT8_MAX;
    s->usb_boot_identity_valid = false;
    s->usb_boot_version = 0;
    s->usb_boot_route_string = 0;
    s->usb_boot_root_hub_port = 0;
    s->usb_boot_last_excluded_vid_pid = UINT32_MAX;
    s->usb_boot_excluded_device_count = 0;
    s->usb_boot_eligible_device_count = 0;
    s->usb_boot_files_missing = false;
    if (!strcmp(source, "usb-msd") && s->usb_boot_on_xhci) {
        usb_reader.xhci = s->vl805_xhci;
    }
    if ((strcmp(source, "usb-msd") == 0) != s->usb_boot_on_xhci) {
        raspi4_set_firmware_status(s, "usb-controller-unavailable");
        return result;
    }
    if (!usb_reader.xhci &&
        !(raspi4_dwc2(s)->hprt0 & HPRT0_CONNSTS)) {
        s->usb_boot_hotplug_pending = true;
        qemu_bh_schedule(s->usb_boot_hotplug_bh);
        return result;
    }
    if (!raspi4_usb_enumerate(&usb_reader, &local_err)) {
        raspi4_set_firmware_status(s, "usb-enumeration-failed");
        error_report_err(local_err);
        return result;
    }
    if (s->usb_boot_external) {
        unsigned int candidate = 0;

        for (unsigned int group = 0;
             group < usb_reader.group_count; group++) {
            if (!usb_reader.group_present[group]) {
                continue;
            }
            for (unsigned int lun = 0;
                 lun <= usb_reader.max_luns[group]; lun++, candidate++) {
                Raspi4BootMediaReader reader;

                if (!raspi4_usb_prepare_reader(
                        &usb_reader, group, lun, &local_err)) {
                    error_report_err(local_err);
                    local_err = NULL;
                    continue;
                }
                reader = (Raspi4BootMediaReader) {
                    .read = raspi4_usb_media_read,
                    .opaque = &usb_reader,
                    .size = usb_reader.size,
                };
                s->usb_boot_identity_valid = true;
                s->usb_boot_version = usb_reader.versions[group];
                s->usb_boot_route_string =
                    usb_reader.route_strings[group];
                s->usb_boot_root_hub_port =
                    usb_reader.root_hub_ports[group];
                s->usb_boot_selected_lun = lun;
                result = raspi4_try_firmware_reader(s, &reader, source);
                if (result == RASPI4_BOOT_ATTEMPT_READY) {
                    s->usb_boot_selected_index =
                        MIN(candidate, UINT8_MAX - 1);
                    s->usb_boot_selected_device = group;
                    return result;
                }
                s->usb_boot_identity_valid = false;
                s->usb_boot_selected_lun = UINT8_MAX;
                if (result == RASPI4_BOOT_ATTEMPT_PENDING) {
                    return result;
                }
                s->usb_boot_files_missing |=
                    raspi4_firmware_status_is_missing_boot_files(s);
            }
        }
        return result;
    }
    for (unsigned int i = 0; i < s->usb_boot_device_count; i++) {
        BlockBackend *blk = s->usb_boot_devices[i];
        Raspi4BootMediaReader reader;
        unsigned int group = s->usb_boot_device_numbers[i];

        if (!blk || !blk_is_inserted(blk) ||
            !usb_reader.group_present[group]) {
            continue;
        }
        if (!raspi4_usb_prepare_reader(
                &usb_reader, group,
                s->usb_boot_luns[i], &local_err)) {
            error_report_err(local_err);
            local_err = NULL;
            continue;
        }
        reader = (Raspi4BootMediaReader) {
            .read = raspi4_usb_media_read,
            .opaque = &usb_reader,
            .size = usb_reader.size,
        };
        s->usb_boot_identity_valid = true;
        s->usb_boot_version = usb_reader.versions[group];
        s->usb_boot_route_string = usb_reader.route_strings[group];
        s->usb_boot_root_hub_port =
            usb_reader.root_hub_ports[group];
        s->usb_boot_selected_lun = s->usb_boot_luns[i];
        result = raspi4_try_firmware_reader(s, &reader, source);
        if (result == RASPI4_BOOT_ATTEMPT_READY) {
            s->usb_boot_selected_index = i;
            s->usb_boot_selected_device =
                s->usb_boot_device_numbers[i];
            return result;
        }
        s->usb_boot_identity_valid = false;
        s->usb_boot_selected_lun = UINT8_MAX;
        if (result == RASPI4_BOOT_ATTEMPT_PENDING) {
            return result;
        }
        s->usb_boot_files_missing |=
            raspi4_firmware_status_is_missing_boot_files(s);
    }
    return result;
}

Raspi4BootAttemptResult
raspi4_try_nvme_firmware(Raspi4bMachineState *s)
{
    return raspi4_try_firmware(s, s->nvme, "nvme");
}

void raspi4_schedule_usb_wait(Raspi4bMachineState *s,
                                     const char *source, bool discovery)
{
    uint32_t timeout = discovery ? s->usb_msd_discover_timeout :
                                   s->usb_msd_lun_timeout;

    s->boot_usb_discovery_wait = discovery;
    s->pending_boot_action = discovery ? RASPI4_PENDING_USB_DISCOVERY :
                                         RASPI4_PENDING_USB_LUN;
    raspi4_set_boot_observation(s, discovery ? "usb-discovery-wait" :
                                              "usb-lun-wait",
                                source);
    trace_raspi4b_boot_event("boot-source", "boot-source.attempt", source,
                             discovery ? "discover-wait" : "lun-wait",
                             timeout);
    timer_mod(s->boot_timeout_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + timeout);
}

void raspi4_schedule_usb_startup(Raspi4bMachineState *s,
                                        const char *source)
{
    s->pending_boot_action = RASPI4_PENDING_USB_STARTUP;
    raspi4_set_boot_observation(s, "usb-startup-delay", source);
    trace_raspi4b_boot_event("boot-source", "boot-source.attempt", source,
                             "startup-delay", s->usb_msd_startup_delay);
    timer_mod(s->boot_timeout_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
              s->usb_msd_startup_delay);
}

void raspi4_schedule_usb_power_off(Raspi4bMachineState *s,
                                           const char *source,
                                           uint32_t delay_ms)
{
    s->usb_power_enabled = false;
    s->pending_boot_action = RASPI4_PENDING_USB_POWER_OFF;
    raspi4_set_boot_observation(s, "usb-power-off-wait", source);
    trace_raspi4b_boot_event("boot-source", "boot-source.usb-power",
                             source, "power-off", delay_ms);
    timer_mod(s->boot_timeout_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + delay_ms);
}

void raspi4_schedule_network_wait(Raspi4bMachineState *s,
                                         bool dhcp)
{
    uint32_t timeout = dhcp ? s->dhcp_timeout : s->tftp_file_timeout;

    s->pending_boot_action = dhcp ? RASPI4_PENDING_NETWORK_DHCP :
                                    RASPI4_PENDING_NETWORK_TFTP;
    if (dhcp && s->network_boot_wire) {
        raspi4_network_dhcp_arm_retransmit(s);
    } else if (!dhcp) {
        raspi4_network_tftp_cancel_retransmit(s);
        s->network_dhcp_phase = RASPI4_DHCP_PHASE_NONE;
        bcm2711_genet_set_boot_client_active(raspi4_genet(s), false);
    }
    raspi4_set_boot_observation(s, dhcp ? "network-dhcp-wait" :
                                         "network-tftp-wait",
                                "network");
    trace_raspi4b_boot_event("boot-source", "boot-source.attempt",
                             "network", dhcp ? "dhcp-wait" : "tftp-wait",
                             timeout);
    timer_mod(s->boot_timeout_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + timeout);
}

bool raspi4_try_network_firmware(Raspi4bMachineState *s)
{
    Raspi4BootAttemptResult result;

    s->network_attempt++;
    s->boot_attempt_count++;
    if (s->network_http_mode &&
        (s->http_cacert_hash_set ||
         (s->http_host_set &&
          (!s->otp_secure_boot || !s->secure_public_key_valid)) ||
         (!s->http_host_set && s->otp_secure_boot))) {
        raspi4_set_firmware_status(
            s, s->http_cacert_hash_set ? "http-https-unsupported" :
                                         "http-policy-invalid");
        raspi4_set_boot_observation(
            s, s->http_cacert_hash_set ? "http-https-unsupported" :
                                         "http-policy-invalid",
            "http");
        return false;
    }
    if (s->network_boot_wire) {
        raspi4_network_dhcp_cancel_retransmit(s);
        s->network_dhcp_retransmit_count = 0;
        s->network_dhcp_retransmit_remaining_ns = 0;
        s->network_dns_retransmit_count = 0;
        raspi4_network_tftp_cancel_retransmit(s);
        s->network_tftp_retransmit_count = 0;
        s->network_tftp_retransmit_remaining_ns = 0;
        s->network_tftp_dally_server_port = 0;
        s->network_tftp_dally_block = 0;
        s->network_dhcp_phase = RASPI4_DHCP_PHASE_DISCOVER;
        s->network_dhcp_xid = RASPI4_DHCP_XID;
        s->network_offered_ip = 0;
        s->network_dhcp_server_ip = 0;
        s->network_proxy_tftp_ip = 0;
        s->network_server_ip = 0;
        memset(s->network_server_mac, 0, sizeof(s->network_server_mac));
        s->network_tftp_server_port = 0;
        s->network_tftp_next_block = 0;
        s->network_tftp_block_number = 0;
        s->network_tftp_size = 0;
        s->network_tftp_expected_size = 0;
        g_clear_pointer(&s->network_tftp_data, g_free);
        g_clear_pointer(&s->network_tftp_filename, g_free);
        g_clear_pointer(&s->network_tftp_expected_sha256, g_free);
        g_clear_pointer(&s->network_http_response, g_free);
        g_clear_pointer(&s->network_http_ooo_data, g_free);
        g_clear_pointer(&s->network_http_ooo_valid, g_free);
        s->network_http_state = RASPI4_HTTP_IDLE;
        s->network_http_response_size = 0;
        s->network_http_content_length = 0;
        s->network_http_header_parsed = false;
        s->network_http_ooo_sequence = 0;
        s->network_http_ooo_size = 0;
        s->network_http_ooo_fin = false;
        s->network_http_ooo_fin_sequence = 0;
        raspi4_network_artifacts_clear(s);
        bcm2711_genet_set_boot_client_active(raspi4_genet(s), true);
        if (raspi4_network_static_configured(s)) {
            s->network_dhcp_phase = RASPI4_DHCP_PHASE_NONE;
            s->network_offered_ip = s->client_ip;
            s->network_server_ip = s->tftp_ip;
            s->network_subnet = s->subnet;
            s->network_gateway = s->gateway_set ? s->gateway : 0;
            if ((s->network_http_mode && raspi4_network_begin_http(s)) ||
                (!s->network_http_mode && raspi4_network_begin_tftp(s))) {
                trace_raspi4b_boot_event(
                    "boot-source", "boot-source.static-ip",
                    s->network_http_mode ? "http" : "network",
                    "arp", s->network_next_hop_ip);
                return true;
            }
            raspi4_schedule_network_wait(s, false);
            return true;
        }
        raspi4_schedule_network_wait(s, true);
        if (runstate_is_running()) {
            raspi4_network_boot_tx(s);
        }
        return true;
    }
    if (!s->network_boot || !blk_is_inserted(s->network_boot)) {
        raspi4_schedule_network_wait(s, true);
        return true;
    }
    result = raspi4_try_firmware(s, s->network_boot, "network");
    if (result == RASPI4_BOOT_ATTEMPT_READY) {
        raspi4_set_boot_observation(s, "arm-handoff-ready", "network");
        trace_raspi4b_boot_event("boot-source", "boot-source.attempt",
                                 "network", "success", s->firmware_size);
        return true;
    }
    if (result == RASPI4_BOOT_ATTEMPT_PENDING) {
        return true;
    }
    raspi4_schedule_network_wait(s, false);
    return true;
}

static void raspi4_schedule_sd_overcurrent(Raspi4bMachineState *s,
                                            const char *source)
{
    s->pending_boot_action = RASPI4_PENDING_SD_OVERCURRENT;
    s->sd_power_enabled = false;
    s->sd_overcurrent_retry_count++;
    raspi4_set_boot_observation(s, "sd-overcurrent-wait", source);
    trace_raspi4b_boot_event("boot-source", "boot-source.sd-overcurrent",
                             source, "power-off",
                             s->sd_overcurrent_retry_count);
    timer_mod(s->boot_timeout_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
              RASPI4_SD_OVERCURRENT_RETRY_MS);
}

static void raspi4_schedule_fatal_reboot(Raspi4bMachineState *s,
                                         const char *source)
{
    raspi4_hdmi_diagnostics_show(s);
    if (!s->reboot_on_fatal_error) {
        return;
    }

    s->pending_boot_action = RASPI4_PENDING_FATAL_REBOOT;
    raspi4_set_boot_observation(s, "fatal-error-reboot-wait", source);
    trace_raspi4b_boot_event("boot-source", "boot-source.fatal-error",
                             source, "reboot",
                             RASPI4_FATAL_ERROR_PATTERN_COUNT);
    timer_mod(s->boot_timeout_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
              RASPI4_FATAL_ERROR_REBOOT_MS);
}

bool raspi4_execute_boot_source(Raspi4bMachineState *s,
                                       uint32_t nibble, uint8_t index)
{
    const char *source = raspi4_boot_source_name(s, nibble);
    Raspi4BootAttemptResult result;
    uint64_t attempts;
    bool bridge_driven;

    s->boot_order_index = index;
    s->selected_boot_mode = nibble;
    if (!source) {
        g_autofree char *reserved = g_strdup_printf("reserved-0x%x", nibble);

        raspi4_set_boot_observation(s, "boot-source-unsupported", reserved);
        trace_raspi4b_boot_event("boot-source", "boot-source.attempt",
                                 reserved, "unsupported", index);
        raspi4_schedule_fatal_reboot(s, reserved);
        return true;
    }
    if (!s->cm4 && (nibble == 0x0 || nibble == 0x1)) {
        if (raspi4_sd_overcurrent_live(s, &bridge_driven)) {
            if (s->sd_overcurrent_check) {
                raspi4_schedule_sd_overcurrent(s, source);
                return true;
            }
            s->sd_overcurrent_warning = true;
            trace_raspi4b_boot_event("boot-source",
                                     "boot-source.sd-overcurrent",
                                     source, "warning", index);
        }
    }
    raspi4_set_boot_observation(s, "boot-source-attempt", source);

    switch (nibble) {
    case 0x0:
        s->boot_attempt_count++;
        result = raspi4_try_sd_firmware(s);
        if (result == RASPI4_BOOT_ATTEMPT_READY) {
            raspi4_set_boot_observation(s, "arm-handoff-ready", source);
            trace_raspi4b_boot_event("boot-source", "boot-source.attempt",
                                     source, "success", s->firmware_size);
        } else if (result != RASPI4_BOOT_ATTEMPT_PENDING) {
            s->pending_boot_action = RASPI4_PENDING_SD_DETECT;
            raspi4_set_boot_observation(s, "sd-card-detect-wait", source);
            trace_raspi4b_boot_event("boot-source", "boot-source.attempt",
                                     source, "wait", index);
        }
        return true;
    case 0x1:
        result = raspi4_try_sd_firmware(s);
        if (result == RASPI4_BOOT_ATTEMPT_READY) {
            s->boot_attempt_count++;
            raspi4_set_boot_observation(s, "arm-handoff-ready", source);
            trace_raspi4b_boot_event("boot-source", "boot-source.attempt",
                                     source, "success", s->firmware_size);
            return true;
        }
        if (result == RASPI4_BOOT_ATTEMPT_PENDING) {
            s->boot_attempt_count++;
            return true;
        }
        if (s->sd_boot_max_retries < 0) {
            s->boot_attempt_count++;
            s->pending_boot_action = RASPI4_PENDING_SD_RETRY;
            raspi4_set_boot_observation(s, s->cm4 ? "emmc-retry-loop" :
                                                       "sd-retry-loop",
                                        source);
            trace_raspi4b_boot_event("boot-source", "boot-source.attempt",
                                     source, "retry", UINT64_MAX);
            return true;
        }
        attempts = (uint64_t)s->sd_boot_max_retries + 1;
        s->boot_attempt_count += attempts;
        s->boot_retry_count += s->sd_boot_max_retries;
        trace_raspi4b_boot_event("boot-source", "boot-source.attempt",
                                 source, "failure", attempts);
        return false;
    case 0x4:
    case 0x5:
        s->boot_attempt_count++;
        if (!raspi4_usb_controller_bootable(s, source)) {
            return false;
        }
        if (!raspi4_usb_power_prepare(s, source)) {
            return true;
        }
        raspi4_usb_continue_after_power(s, source);
        return true;
    case 0x2:
        s->network_http_mode = false;
        s->network_attempt = 0;
        return raspi4_try_network_firmware(s);
    case 0x6:
        s->boot_attempt_count++;
        result = raspi4_try_nvme_firmware(s);
        if (result == RASPI4_BOOT_ATTEMPT_READY) {
            raspi4_set_boot_observation(s, "arm-handoff-ready", source);
            trace_raspi4b_boot_event("boot-source", "boot-source.attempt",
                                     source, "success", s->firmware_size);
            return true;
        }
        if (result == RASPI4_BOOT_ATTEMPT_PENDING) {
            return true;
        }
        trace_raspi4b_boot_event("boot-source", "boot-source.attempt",
                                 source, "failure", index);
        return false;
    case 0x7:
        s->network_http_mode = true;
        s->network_attempt = 0;
        return raspi4_try_network_firmware(s);
    case 0x3:
        s->boot_attempt_count++;
        raspi4_set_boot_observation(s, "rpiboot-wait", source);
        trace_raspi4b_boot_event("boot-source", "boot-source.attempt",
                                 source, "wait", index);
        return true;
    case 0xe:
        raspi4_set_boot_observation(s, "stopped", source);
        trace_raspi4b_boot_event("boot-source", "boot-source.stop", source,
                                 "stop", index);
        return true;
    case 0xf:
        s->boot_restart_count++;
        if (s->max_restarts < 0) {
            raspi4_set_boot_observation(s, "restart-loop", source);
            trace_raspi4b_boot_event("boot-source", "boot-source.restart",
                                     source, "restart", UINT64_MAX);
            return true;
        }
        if (s->boot_restart_count > s->max_restarts) {
            raspi4_set_boot_observation(s, "restart-watchdog-pending",
                                        source);
            trace_raspi4b_boot_event("boot-source", "boot-source.restart",
                                     source, "watchdog",
                                     s->boot_restart_count);
            bcm2835_powermgt_watchdog_start(
                raspi4_powermgt(s), RASPI4_RESTART_WATCHDOG_DELAY_MS);
            s->pending_boot_action = RASPI4_PENDING_RESTART_WATCHDOG;
            return true;
        }
        raspi4_set_boot_observation(s, "restart-cycle-wait", source);
        trace_raspi4b_boot_event("boot-source", "boot-source.restart",
                                 source, "restart", s->boot_restart_count);
        timer_mod(s->boot_restart_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1);
        s->pending_boot_action = RASPI4_PENDING_RESTART;
        return true;
    default:
        g_assert_not_reached();
    }
}

void raspi4_execute_boot_order_from(Raspi4bMachineState *s,
                                           uint8_t start)
{
    unsigned int nibbles = raspi4_boot_order_nibbles(s->boot_order);

    for (uint8_t index = start; index < nibbles; index++) {
        uint32_t nibble = (s->boot_order >> (index * 4)) & 0xf;

        s->bootcode_delay_consumed = false;
        s->bootcode_delay_seconds = 0;
        if (raspi4_execute_boot_source(s, nibble, index)) {
            return;
        }
    }

    s->boot_order_index = nibbles;
    raspi4_set_boot_observation(s, "boot-order-exhausted", "none");
    trace_raspi4b_boot_event("boot-source", "boot-source.exhausted", "none",
                             "failure", s->boot_attempt_count);
    raspi4_schedule_fatal_reboot(s, "none");
}

void raspi4_execute_after_source_failure(Raspi4bMachineState *s,
                                                uint8_t index)
{
    if (s->net_install_override) {
        s->net_install_override = false;
        raspi4_execute_boot_order_from(s, 0);
    } else {
        raspi4_execute_boot_order_from(s, index + 1);
    }
}

void raspi4_execute_boot_order(Raspi4bMachineState *s)
{
    raspi4_execute_boot_order_from(s, 0);
}

void raspi4_boot_timeout(void *opaque)
{
    Raspi4bMachineState *s = opaque;
    uint8_t action = s->pending_boot_action;
    bool discovery = s->boot_usb_discovery_wait;
    uint8_t index = s->boot_order_index;
    uint32_t nibble = (s->boot_order >> (index * 4)) & 0xf;
    const char *source = raspi4_boot_source_name(s, nibble);
    uint32_t timeout = discovery ? s->usb_msd_discover_timeout :
                                   s->usb_msd_lun_timeout;
    Raspi4BootAttemptResult result;

    if (action == RASPI4_PENDING_NETCONSOLE_LINK) {
        s->pending_boot_action = RASPI4_PENDING_NONE;
        s->boot_elapsed_ms += s->dhcp_timeout;
        raspi4_set_boot_observation(
            s, "netconsole-link-timeout", "network");
        raspi4_continue_after_eeprom_config(s);
        return;
    }

    if (action == RASPI4_PENDING_NET_INSTALL_KEYBOARD) {
        s->pending_boot_action = RASPI4_PENDING_NONE;
        s->boot_elapsed_ms += s->net_install_keyboard_wait;
        raspi4_execute_boot_order(s);
        return;
    }

    if (action == RASPI4_PENDING_FATAL_REBOOT) {
        s->pending_boot_action = RASPI4_PENDING_NONE;
        s->boot_elapsed_ms += RASPI4_FATAL_ERROR_REBOOT_MS;
        s->fatal_error_reboot_count++;
        trace_raspi4b_boot_event("reset", "reset.fatal-error",
                                 source ? source : "none", "watchdog",
                                 s->fatal_error_reboot_count);
        bcm2835_powermgt_watchdog_trigger(raspi4_powermgt(s));
        return;
    }

    if (action == RASPI4_PENDING_SD_OVERCURRENT) {
        bool bridge_driven;

        s->pending_boot_action = RASPI4_PENDING_NONE;
        s->boot_elapsed_ms += RASPI4_SD_OVERCURRENT_RETRY_MS;
        s->sd_power_enabled = true;
        if (raspi4_sd_overcurrent_live(s, &bridge_driven) &&
            s->sd_overcurrent_check && !s->cm4) {
            raspi4_schedule_sd_overcurrent(s, source);
            return;
        }
        trace_raspi4b_boot_event("boot-source",
                                 "boot-source.sd-overcurrent",
                                 source, "power-on",
                                 s->sd_overcurrent_retry_count);
        raspi4_execute_boot_source(s, nibble, index);
        return;
    }

    if (action == RASPI4_PENDING_USB_POWER_OFF) {
        uint64_t remaining_ms = s->usb_power_cycle_legacy ?
            s->usb_msd_power_off_time :
            s->usb_msd_power_off_time -
                MIN((uint64_t)s->usb_msd_power_off_time,
                    s->usb_power_off_elapsed_ms);

        s->pending_boot_action = RASPI4_PENDING_NONE;
        s->usb_power_off_elapsed_ms += remaining_ms;
        s->boot_elapsed_ms += remaining_ms;
        s->usb_power_enabled = true;
        trace_raspi4b_boot_event("boot-source",
                                 "boot-source.usb-power",
                                 source, "power-on",
                                 s->usb_power_off_elapsed_ms);
        raspi4_usb_continue_after_power(s, source);
        return;
    }

    if (action == RASPI4_PENDING_USB_STARTUP) {
        s->pending_boot_action = RASPI4_PENDING_NONE;
        s->boot_elapsed_ms += s->usb_msd_startup_delay;
        if (!raspi4_usb_media_present(s)) {
            raspi4_schedule_usb_wait(s, source, true);
            return;
        }
        result = raspi4_try_usb_firmware(s, source);
        if (result == RASPI4_BOOT_ATTEMPT_READY) {
            raspi4_set_boot_observation(s, "arm-handoff-ready", source);
            trace_raspi4b_boot_event("boot-source", "boot-source.attempt",
                                     source, "success", s->firmware_size);
            return;
        }
        if (result == RASPI4_BOOT_ATTEMPT_PENDING) {
            return;
        }
        raspi4_schedule_usb_wait(
            s, source, raspi4_usb_only_excluded(s));
        return;
    }

    if (action == RASPI4_PENDING_FIRMWARE_DELAY) {
        uint64_t delay_ms = (uint64_t)s->bootcode_delay_seconds * 1000;

        s->pending_boot_action = RASPI4_PENDING_NONE;
        s->boot_elapsed_ms += delay_ms;
        raspi4_resume_after_bootcode_delay(s);
        return;
    }

    if (action == RASPI4_PENDING_NETWORK_TFTP_DALLY) {
        s->boot_elapsed_ms += RASPI4_TFTP_DALLY_MS;
        raspi4_network_complete_artifacts(s);
        return;
    }

    if (action == RASPI4_PENDING_NETWORK_DHCP ||
        action == RASPI4_PENDING_NETWORK_TFTP ||
        action == RASPI4_PENDING_NETWORK_ARP ||
        action == RASPI4_PENDING_NETWORK_DNS ||
        action == RASPI4_PENDING_NETWORK_HTTP) {
        bool dhcp = action == RASPI4_PENDING_NETWORK_DHCP;
        bool arp = action == RASPI4_PENDING_NETWORK_ARP;
        bool http = action == RASPI4_PENDING_NETWORK_HTTP;
        bool dns = action == RASPI4_PENDING_NETWORK_DNS ||
                   (arp && s->network_arp_for_dns);

        raspi4_network_dhcp_cancel_retransmit(s);
        raspi4_network_tftp_cancel_retransmit(s);
        timeout = dhcp || dns ? s->dhcp_timeout : s->tftp_file_timeout;
        s->pending_boot_action = RASPI4_PENDING_NONE;
        s->boot_elapsed_ms += timeout;
        if (!s->network_boot_wire && s->network_boot &&
            blk_is_inserted(s->network_boot)) {
            result = raspi4_try_firmware(s, s->network_boot, "network");
            if (result == RASPI4_BOOT_ATTEMPT_READY) {
                raspi4_set_boot_observation(s, "arm-handoff-ready",
                                            "network");
                trace_raspi4b_boot_event(
                    "boot-source", "boot-source.attempt", "network",
                    "success", s->firmware_size);
                return;
            }
            if (result == RASPI4_BOOT_ATTEMPT_PENDING) {
                return;
            }
        }
        trace_raspi4b_boot_event("boot-source", "boot-source.attempt",
                                 "network", dhcp ? "dhcp-timeout" :
                                            dns ? "dns-timeout" :
                                            http ? "http-timeout" :
                                            arp ? "arp-timeout" :
                                                  "tftp-timeout",
                                 timeout);
        if (s->net_boot_max_retries >= 0 &&
            s->network_attempt > s->net_boot_max_retries) {
            s->network_dhcp_phase = RASPI4_DHCP_PHASE_NONE;
            bcm2711_genet_set_boot_client_active(raspi4_genet(s), false);
            raspi4_execute_after_source_failure(s, index);
            return;
        }
        s->boot_retry_count++;
        raspi4_try_network_firmware(s);
        return;
    }

    s->pending_boot_action = RASPI4_PENDING_NONE;
    s->boot_elapsed_ms += timeout;
    if (raspi4_usb_media_present(s)) {
        result = raspi4_try_usb_firmware(s, source);
        if (result == RASPI4_BOOT_ATTEMPT_READY) {
            raspi4_set_boot_observation(s, "arm-handoff-ready", source);
            trace_raspi4b_boot_event("boot-source", "boot-source.attempt",
                                     source, "success", s->firmware_size);
            return;
        }
        if (result == RASPI4_BOOT_ATTEMPT_PENDING) {
            return;
        }
        if (discovery && s->usb_boot_eligible_device_count) {
            raspi4_schedule_usb_wait(s, source, false);
            return;
        }
    }
    trace_raspi4b_boot_event("boot-source", "boot-source.attempt", source,
                             discovery ? "discover-timeout" : "lun-failure",
                             timeout);
    if (raspi4_usb_net_install_fallback(s, nibble)) {
        return;
    }
    raspi4_execute_boot_order_from(s, index + 1);
}

void raspi4_boot_watchdog_expired(void *opaque)
{
    Raspi4bMachineState *s = opaque;
    BCM2835PowerMgtState *power = raspi4_powermgt(s);

    s->boot_watchdog_armed = false;
    s->boot_watchdog_remaining_ns = 0;
    power->rsts = (power->rsts & ~0x555) |
                  raspi4_encode_boot_partition(
                      s->boot_watchdog_partition);
    bcm2835_powermgt_watchdog_trigger(power);
}

void raspi4_boot_watchdog_arm(Raspi4bMachineState *s)
{
    uint64_t delay;

    timer_del(s->boot_watchdog_timer);
    s->boot_watchdog_armed = false;
    s->boot_watchdog_remaining_ns = 0;
    if (!s->boot_watchdog_timeout) {
        return;
    }
    delay = (uint64_t)s->boot_watchdog_timeout * NANOSECONDS_PER_SECOND;
    s->boot_watchdog_armed = true;
    s->boot_watchdog_remaining_ns = delay;
    timer_mod_ns(s->boot_watchdog_timer,
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay);
}

static int raspi4_boot_pre_save(void *opaque)
{
    Raspi4bMachineState *s = opaque;
    QEMUTimer *timer;
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t expiry;
    bool last_excluded_matches = false;

    /*
     * TLS traffic keys are deliberately not serialized.  Migrating before
     * TCP/TLS starts is safe; an established HTTPS connection is rejected
     * instead of resuming with invalid cryptographic state.
     */
    if (s->network_tls_session) {
        return -ENOTSUP;
    }
    if (s->eeprom_config_append_size > RASPI4_EEPROM_SIZE) {
        return -EINVAL;
    }
    if ((s->boot_uart_active &&
         (!s->boot_uart_enabled || !s->boot_uart_started)) ||
        (s->boot_uart_started && !s->boot_uart_enabled) ||
        s->boot_uart_lines > s->boot_uart_bytes) {
        return -EINVAL;
    }
    if (s->vl805_initialized &&
        (!s->cm4 || !s->vl805_enabled || !s->vl805_xhci)) {
        return -EINVAL;
    }
    if (s->sd_quirks & ~RASPI4_SD_QUIRK_DISABLE_HIGH_SPEED) {
        return -EINVAL;
    }
    if (s->usb_msd_startup_delay > RASPI4_USB_MSD_STARTUP_DELAY_MAX) {
        return -EINVAL;
    }
    if (s->usb_msd_power_off_time > RASPI4_USB_MSD_PWR_OFF_TIME_MAX ||
        s->usb_power_cycle_legacy != raspi4_usb_legacy_power_cycle(s) ||
        (s->pending_boot_action == RASPI4_PENDING_USB_POWER_OFF &&
         (s->cm4 || s->usb_power_enabled ||
          !s->usb_power_off_consumed ||
          s->usb_power_off_elapsed_ms >=
              s->usb_msd_power_off_time))) {
        return -EINVAL;
    }
    if (s->usb_msd_exclude_count > RASPI4_USB_MSD_EXCLUDE_MAX ||
        s->usb_boot_eligible_device_count >
            RASPI4_USB_BOOT_MAX_DEVICES) {
        return -EINVAL;
    }
    if (s->self_update_status >= RASPI4_SELF_UPDATE_STATUS__MAX ||
        (s->behavioral_boot &&
         (s->boot_mac_address_source >= RASPI4_MAC__MAX ||
          !raspi4_mac_address_valid(&s->boot_mac_address))) ||
        s->tftp_prefix_mode > 2 ||
        !memchr(s->tftp_prefix, 0, sizeof(s->tftp_prefix))) {
        return -EINVAL;
    }
    if (s->behavioral_boot &&
        (!s->pxe_option43[0] ||
         !memchr(s->pxe_option43, 0, sizeof(s->pxe_option43)))) {
        return -EINVAL;
    }
    if ((s->netconsole_enabled &&
         (!memchr(s->netconsole, 0, sizeof(s->netconsole)) ||
          !s->netconsole[0] ||
          !s->netconsole_source_port ||
          !s->netconsole_destination_port ||
          !raspi4_ipv4_is_unicast(s->netconsole_source_ip))) ||
        (!s->netconsole_enabled && s->netconsole[0]) ||
        (s->pending_boot_action == RASPI4_PENDING_NETCONSOLE_LINK &&
         !s->netconsole_enabled)) {
        return -EINVAL;
    }
    if (s->pending_boot_action == RASPI4_PENDING_NET_INSTALL_KEYBOARD &&
        (!s->net_install_enabled || !s->net_install_keyboard_wait ||
         !s->net_install_keyboard_present ||
         s->net_install_override)) {
        return -EINVAL;
    }
    if (s->boot_disable_hdmi &&
        (s->net_install_enabled || s->net_install_at_power_on ||
         s->net_install_override)) {
        return -EINVAL;
    }
    if ((s->boot_disable_hdmi &&
         (s->hdmi_diagnostics_pending ||
          s->hdmi_diagnostics_visible)) ||
        (s->hdmi_diagnostics_pending &&
         s->hdmi_diagnostics_visible)) {
        return -EINVAL;
    }
    for (unsigned int i = 0; i < s->usb_msd_exclude_count; i++) {
        if (s->usb_msd_exclude_vid_pid[i] ==
            s->usb_boot_last_excluded_vid_pid) {
            last_excluded_matches = true;
        }
    }
    if ((!s->usb_boot_excluded_device_count &&
         s->usb_boot_last_excluded_vid_pid != UINT32_MAX) ||
        (s->usb_boot_excluded_device_count &&
         !last_excluded_matches)) {
        return -EINVAL;
    }
    if ((!s->sd_power_enabled) !=
            (s->pending_boot_action == RASPI4_PENDING_SD_OVERCURRENT) ||
        (s->pending_boot_action == RASPI4_PENDING_SD_OVERCURRENT &&
         !s->sd_overcurrent_retry_count) ||
        (s->sd_overcurrent_warning && s->sd_overcurrent_check)) {
        return -EINVAL;
    }
    if (s->rpiboot_phase >= RASPI4_RPIBOOT__MAX ||
        s->rpiboot_control_pending >= RASPI4_RPIBOOT_CONTROL__MAX ||
        (s->rpiboot_control_pending == RASPI4_RPIBOOT_CONTROL_NONE &&
         s->rpiboot_control_value) ||
        (s->rpiboot_control_pending ==
             RASPI4_RPIBOOT_CONTROL_SET_CONFIGURATION &&
         s->rpiboot_control_value > 1) ||
        (s->rpiboot_control_pending ==
             RASPI4_RPIBOOT_CONTROL_SET_ADDRESS &&
         s->rpiboot_control_value > 127) ||
        ((s->rpiboot_control_pending ==
              RASPI4_RPIBOOT_CONTROL_SET_ENDPOINT_HALT ||
          s->rpiboot_control_pending ==
              RASPI4_RPIBOOT_CONTROL_CLEAR_ENDPOINT_HALT) &&
         s->rpiboot_control_value != 1) ||
        s->rpiboot_configuration > 1 ||
        s->rpiboot_file_index > 2 ||
        s->rpiboot_bulk_received > s->rpiboot_bulk_expected ||
        s->rpiboot_bootcode_alloc > RASPI4_RPIBOOT_MAX_BOOTCODE_SIZE ||
        s->rpiboot_config_alloc > RASPI4_RPIBOOT_MAX_FILE_SIZE ||
        s->rpiboot_boot_img_alloc > RASPI4_RPIBOOT_MAX_FILE_SIZE ||
        s->rpiboot_bootcode_size > s->rpiboot_bootcode_alloc ||
        s->rpiboot_config_size > s->rpiboot_config_alloc ||
        s->rpiboot_boot_img_size > s->rpiboot_boot_img_alloc ||
        (!!s->rpiboot_bootcode != !!s->rpiboot_bootcode_alloc) ||
        (!!s->rpiboot_config != !!s->rpiboot_config_alloc) ||
        (!!s->rpiboot_boot_img != !!s->rpiboot_boot_img_alloc)) {
        return -EINVAL;
    }
    if (s->rpiboot_bootcode_trusted &&
        s->rpiboot_phase < RASPI4_RPIBOOT_EXPECT_STATUS) {
        return -EINVAL;
    }
    if (s->pending_boot_action == RASPI4_PENDING_EEPROM_FLASH &&
        (!s->eeprom_flash_image ||
         s->eeprom_flash_image_size != RASPI4_EEPROM_SIZE ||
         s->eeprom_flash_stage < RASPI4_EEPROM_FLASH_ERASE ||
         s->eeprom_flash_stage > RASPI4_EEPROM_FLASH_VERIFY)) {
        return -EINVAL;
    }
    if (s->usb_boot_hotplug_pending) {
        qemu_bh_cancel(s->usb_boot_hotplug_bh);
        raspi4_usb_hotplug_bh(s);
    }
    if (s->sd_hotplug_pending) {
        qemu_bh_cancel(s->sd_hotplug_bh);
        raspi4_sd_hotplug_bh(s);
    }
    if (s->network_boot_hotplug_pending) {
        qemu_bh_cancel(s->network_boot_hotplug_bh);
        raspi4_network_hotplug_bh(s);
    }
    s->pending_boot_remaining_ns = 0;
    s->boot_watchdog_remaining_ns = 0;
    s->hdmi_diagnostics_remaining_ns = 0;
    s->network_dhcp_retransmit_remaining_ns = 0;
    s->network_tftp_retransmit_remaining_ns = 0;
    if (s->network_boot_wire &&
        (s->pending_boot_action == RASPI4_PENDING_NETWORK_DHCP ||
         s->pending_boot_action == RASPI4_PENDING_NETWORK_DNS)) {
        if (!timer_pending(s->network_dhcp_retransmit_timer)) {
            return -EINVAL;
        }
        expiry = timer_expire_time_ns(s->network_dhcp_retransmit_timer);
        if (expiry > now) {
            s->network_dhcp_retransmit_remaining_ns = expiry - now;
        }
    }
    if (s->network_boot_wire &&
        (s->pending_boot_action == RASPI4_PENDING_NETWORK_TFTP ||
         s->pending_boot_action == RASPI4_PENDING_NETWORK_HTTP)) {
        if (!timer_pending(s->network_tftp_retransmit_timer)) {
            return -EINVAL;
        }
        expiry = timer_expire_time_ns(s->network_tftp_retransmit_timer);
        if (expiry > now) {
            s->network_tftp_retransmit_remaining_ns = expiry - now;
        }
    }
    if (s->boot_watchdog_armed) {
        if (!timer_pending(s->boot_watchdog_timer)) {
            return -EINVAL;
        }
        expiry = timer_expire_time_ns(s->boot_watchdog_timer);
        if (expiry > now) {
            s->boot_watchdog_remaining_ns = expiry - now;
        }
    }
    if (s->hdmi_diagnostics_pending) {
        if (!timer_pending(s->hdmi_diagnostics_timer)) {
            return -EINVAL;
        }
        expiry = timer_expire_time_ns(s->hdmi_diagnostics_timer);
        if (expiry > now) {
            s->hdmi_diagnostics_remaining_ns = expiry - now;
        }
    } else if (timer_pending(s->hdmi_diagnostics_timer)) {
        return -EINVAL;
    }
    timer = raspi4_pending_boot_timer(s);
    if (!timer) {
        return 0;
    }
    if (!timer_pending(timer)) {
        return -EINVAL;
    }
    expiry = timer_expire_time_ns(timer);
    if (expiry > now) {
        s->pending_boot_remaining_ns = expiry - now;
    }
    return 0;
}

static int raspi4_boot_post_load(void *opaque, int version_id)
{
    Raspi4bMachineState *s = opaque;
    QEMUTimer *timer;
    uint32_t nibble;
    uint32_t migrated_expected_size;
    const char *recovery_status;
    const char *source;
    bool last_excluded_matches = false;

    if (s->pending_boot_action >= RASPI4_PENDING__MAX) {
        return -EINVAL;
    }

    /*
     * These are migrated as raw buffers but consumed as C strings, so a
     * stream that fills one completely would otherwise be read past its
     * end.  Terminate them rather than rejecting the stream: a truncated
     * name simply fails its own validation further on.
     */
    s->http_host[sizeof(s->http_host) - 1] = 0;
    s->network_tftp_hostname[sizeof(s->network_tftp_hostname) - 1] = 0;
    raspi4_pcie_msi_update(s);
    if (version_id >= 34 && version_id < 38 &&
        s->usb_boot_selected_index != UINT8_MAX &&
        s->usb_boot_selected_index >= s->usb_boot_device_count) {
        return -EINVAL;
    }
    if (version_id < 38) {
        s->usb_boot_selected_device = UINT8_MAX;
        s->usb_boot_selected_lun = UINT8_MAX;
        if (s->usb_boot_selected_index < s->usb_boot_device_count) {
            s->usb_boot_selected_device =
                s->usb_boot_device_numbers[s->usb_boot_selected_index];
            s->usb_boot_selected_lun =
                s->usb_boot_luns[s->usb_boot_selected_index];
        }
    }
    if (version_id < 55) {
        memset(s->usb_msd_exclude_vid_pid, 0,
               sizeof(s->usb_msd_exclude_vid_pid));
        s->usb_msd_exclude_count = 0;
        s->usb_boot_excluded_device_count = 0;
        s->usb_boot_eligible_device_count = 0;
        s->usb_boot_last_excluded_vid_pid = UINT32_MAX;
    }
    if (version_id < 56) {
        s->tftp_prefix_mode = 0;
        s->tftp_prefix[0] = 0;
        s->network_tftp_prefix_fallback = false;
    }
    if (version_id < 57 && s->behavioral_boot) {
        s->boot_mac_address = *bcm2711_genet_mac(raspi4_genet(s));
        s->boot_mac_address_source = RASPI4_MAC_CONFIGURED;
    }
    if (version_id < 58) {
        s->enable_self_update = true;
        s->freeze_version = false;
        s->self_update_status = RASPI4_SELF_UPDATE_NONE;
    }
    if (version_id < 70) {
        s->eeprom_update_timestamp_valid = false;
        s->eeprom_update_timestamp = 0;
    }
    if (version_id < 71) {
        s->eeprom_capabilities_valid = false;
        s->eeprom_capabilities = 0;
    }
    if (version_id < 72) {
        s->usb_boot_identity_valid = false;
        s->usb_boot_version = 0;
        s->usb_boot_route_string = 0;
        s->usb_boot_root_hub_port = 0;
    }
    if (s->self_update_status >= RASPI4_SELF_UPDATE_STATUS__MAX ||
        (s->behavioral_boot &&
         (s->boot_mac_address_source >= RASPI4_MAC__MAX ||
          !raspi4_mac_address_valid(&s->boot_mac_address))) ||
        s->tftp_prefix_mode > 2 ||
        !memchr(s->tftp_prefix, 0, sizeof(s->tftp_prefix))) {
        return -EINVAL;
    }
    if (s->behavioral_boot) {
        bcm2711_genet_set_mac(raspi4_genet(s), &s->boot_mac_address);
        bcm2835_property_set_mac(raspi4_property(s), &s->boot_mac_address);
    }
    if (s->usb_msd_exclude_count > RASPI4_USB_MSD_EXCLUDE_MAX ||
        s->usb_boot_eligible_device_count >
            RASPI4_USB_BOOT_MAX_DEVICES) {
        return -EINVAL;
    }
    for (unsigned int i = 0; i < s->usb_msd_exclude_count; i++) {
        if (s->usb_msd_exclude_vid_pid[i] ==
            s->usb_boot_last_excluded_vid_pid) {
            last_excluded_matches = true;
        }
    }
    if ((!s->usb_boot_excluded_device_count &&
         s->usb_boot_last_excluded_vid_pid != UINT32_MAX) ||
        (s->usb_boot_excluded_device_count &&
         !last_excluded_matches)) {
        return -EINVAL;
    }
    if ((s->usb_boot_selected_device == UINT8_MAX) !=
        (s->usb_boot_selected_lun == UINT8_MAX) ||
        (s->usb_boot_selected_device != UINT8_MAX &&
         (s->usb_boot_selected_device >= RASPI4_USB_BOOT_MAX_DEVICES ||
          s->usb_boot_selected_lun > 15))) {
        return -EINVAL;
    }
    if (s->usb_boot_identity_valid &&
        ((s->usb_boot_version != 2 && s->usb_boot_version != 3) ||
         !s->usb_boot_root_hub_port ||
         s->usb_boot_route_string > 0xfffff ||
         s->usb_boot_selected_lun == UINT8_MAX)) {
        return -EINVAL;
    }
    if (version_id < 35) {
        s->recovery_status_id = 0;
    }
    if (version_id < 36) {
        s->eeprom_erased_bytes = 0;
        s->eeprom_programmed_bytes = 0;
        s->eeprom_verified_bytes = 0;
        s->eeprom_dirty_sector_count = 0;
        s->eeprom_flash_stage = RASPI4_EEPROM_FLASH_IDLE;
    }
    if (version_id < 41) {
        s->otp_provision_rows_programmed = 0;
    }
    if (version_id < 42) {
        s->recovery_sha256[0] = 0;
        s->recovery_key_index = UINT32_MAX;
    }
    if (version_id < 43) {
        s->eeprom_program_page_count = 0;
        s->eeprom_nor_violation_bits = 0;
    }
    if (version_id < 44) {
        s->eeprom_nwp_sampled = s->eeprom_nwp;
    }
    if (version_id < 45) {
        s->eeprom_nwp_bridge_driven = false;
    }
    if (version_id < 46) {
        memset(s->hdmi_edid_name, 0, sizeof(s->hdmi_edid_name));
        memset(s->hdmi_edid_status, 0, sizeof(s->hdmi_edid_status));
    }
    if (version_id < 47) {
        s->bootcode_delay_consumed = false;
        s->bootcode_delay_seconds = 0;
    }
    if (version_id < 48) {
        s->firmware_uart_enabled = false;
        s->firmware_uart_started = false;
        s->firmware_uart_bytes = 0;
        s->firmware_uart_lines = 0;
    }
    if (version_id < 49) {
        s->eeprom_erase_delay_us = 0;
        s->eeprom_program_delay_us = 0;
        s->eeprom_verify_delay_us = 0;
        s->eeprom_flash_elapsed_us = 0;
        s->eeprom_flash_deadline_us = 0;
        s->eeprom_flash_image_size = 0;
        s->eeprom_flash_image = NULL;
        s->eeprom_flash_timing_complete = false;
    }
    for (unsigned int port = 0; port < RASPI_FIRMWARE_MAX_EDIDS; port++) {
        if (s->hdmi_edid_status[port] > RASPI4_EDID_INVALID ||
            !memchr(s->hdmi_edid_name[port], 0,
                    sizeof(s->hdmi_edid_name[port])) ||
            (s->hdmi_edid_status[port] == RASPI4_EDID_VALID) !=
                !!s->hdmi_edid_name[port][0]) {
            return -EINVAL;
        }
    }
    if ((s->firmware_uart_enabled && !s->firmware_uart_started) ||
        s->firmware_uart_lines > s->firmware_uart_bytes) {
        return -EINVAL;
    }
    if (s->recovery_key_index != UINT32_MAX &&
        s->recovery_key_index > 4) {
        return -EINVAL;
    }
    if (s->otp_provision_rows_programmed >
        3 + BCM2711_OTP_CUSTOMER_KEY_HASH_LEN) {
        return -EINVAL;
    }
    if (s->eeprom_flash_stage >= RASPI4_EEPROM_FLASH__MAX) {
        return -EINVAL;
    }
    if (s->eeprom_program_page_count >
            DIV_ROUND_UP(RASPI4_EEPROM_SIZE, 256) ||
        s->eeprom_nor_violation_bits > RASPI4_EEPROM_SIZE * 8) {
        return -EINVAL;
    }
    if (s->pending_boot_action == RASPI4_PENDING_EEPROM_FLASH &&
        (!s->eeprom_flash_image ||
         s->eeprom_flash_image_size != RASPI4_EEPROM_SIZE ||
         s->eeprom_flash_stage < RASPI4_EEPROM_FLASH_ERASE ||
         s->eeprom_flash_stage > RASPI4_EEPROM_FLASH_VERIFY ||
         s->eeprom_erased_bytes > RASPI4_EEPROM_SIZE ||
         s->eeprom_programmed_bytes > RASPI4_EEPROM_SIZE ||
         s->eeprom_verified_bytes > RASPI4_EEPROM_SIZE ||
         (s->eeprom_flash_stage >= RASPI4_EEPROM_FLASH_PROGRAM &&
          s->eeprom_erased_bytes != RASPI4_EEPROM_SIZE) ||
         (s->eeprom_flash_stage >= RASPI4_EEPROM_FLASH_VERIFY &&
          s->eeprom_programmed_bytes != RASPI4_EEPROM_SIZE))) {
        return -EINVAL;
    }
    recovery_status = raspi4_recovery_status_from_id(
        s->recovery_status_id);
    if (!recovery_status) {
        return -EINVAL;
    }
    raspi4_set_recovery_status(s, recovery_status);
    if (s->recovery_status_id) {
        raspi4_set_boot_observation(s, recovery_status, "sd-card");
    }
    if (version_id < 30) {
        s->rpiboot_dwc2_active = false;
        s->rpiboot_control_in = false;
        s->rpiboot_phase = RASPI4_RPIBOOT_EXPECT_MESSAGE;
        s->rpiboot_bulk_expected = 0;
        s->rpiboot_bulk_received = 0;
        s->rpiboot_expected_bootcode = 0;
        s->rpiboot_bootcode_size = 0;
        s->rpiboot_bootcode_alloc = 0;
        s->rpiboot_file_index = 0;
        s->rpiboot_file_expected = 0;
        s->rpiboot_config_size = 0;
        s->rpiboot_config_alloc = 0;
        s->rpiboot_boot_img_size = 0;
        s->rpiboot_boot_img_alloc = 0;
        s->rpiboot_second_stage = false;
        memset(s->rpiboot_boot_message, 0,
               sizeof(s->rpiboot_boot_message));
        g_clear_pointer(&s->rpiboot_bootcode, g_free);
        g_clear_pointer(&s->rpiboot_config, g_free);
        g_clear_pointer(&s->rpiboot_boot_img, g_free);
    }
    if (version_id < 50) {
        s->rpiboot_bootcode_trusted = false;
    }
    if (version_id < 59) {
        s->rpiboot_control_pending = RASPI4_RPIBOOT_CONTROL_NONE;
        s->rpiboot_control_value = 0;
        s->rpiboot_configuration = 0;
    }
    if (version_id < 60) {
        s->reboot_on_fatal_error = true;
        s->fatal_error_reboot_count = 0;
    }
    if (version_id < 61) {
        s->eeprom_config_append_size = 0;
    }
    if (version_id < 62) {
        s->boot_uart_enabled = false;
        s->boot_uart_active = false;
        s->boot_uart_started = false;
        s->boot_uart_bytes = 0;
        s->boot_uart_lines = 0;
    }
    if (version_id < 63) {
        s->vl805_enabled = false;
        s->vl805_initialized = false;
    }
    if (version_id < 64) {
        s->sd_quirks = 0;
    }
    if (version_id < 65) {
        s->eeprom_bootconf_size = 0;
        s->eeprom_bootconf = NULL;
        s->eeprom_public_key_size = 0;
        memset(s->eeprom_public_key, 0, sizeof(s->eeprom_public_key));
    }
    if (s->eeprom_config_append_size > RASPI4_EEPROM_SIZE) {
        return -EINVAL;
    }
    if (s->eeprom_bootconf_size > RASPI4_EEPROM_SIZE ||
        (s->eeprom_bootconf_size && !s->eeprom_bootconf) ||
        (s->eeprom_public_key_size &&
         s->eeprom_public_key_size != RASPI_SECURE_PUBLIC_KEY_SIZE)) {
        return -EINVAL;
    }
    if ((s->boot_uart_active &&
         (!s->boot_uart_enabled || !s->boot_uart_started)) ||
        (s->boot_uart_started && !s->boot_uart_enabled) ||
        s->boot_uart_lines > s->boot_uart_bytes) {
        return -EINVAL;
    }
    if (s->vl805_initialized &&
        (!s->cm4 || !s->vl805_enabled || !s->vl805_xhci)) {
        return -EINVAL;
    }
    if (s->sd_quirks & ~RASPI4_SD_QUIRK_DISABLE_HIGH_SPEED) {
        return -EINVAL;
    }
    s->firmware_config.bootloader_config_append =
        s->eeprom_config_append;
    s->firmware_config.bootloader_config_append_size =
        s->eeprom_config_append_size;
    s->firmware_config.bootloader_config = s->eeprom_bootconf;
    s->firmware_config.bootloader_config_size = s->eeprom_bootconf_size;
    s->firmware_config.bootloader_public_key = s->eeprom_public_key;
    s->firmware_config.bootloader_public_key_size =
        s->eeprom_public_key_size;
    if (version_id < 51) {
        s->usb_msd_startup_delay = RASPI4_USB_MSD_STARTUP_DELAY;
    }
    if (version_id < 52) {
        s->boot_watchdog_timeout = 0;
        s->boot_watchdog_partition = 0;
        s->boot_watchdog_armed = false;
        s->boot_watchdog_remaining_ns = 0;
    }
    if (version_id < 53) {
        s->sd_overcurrent = false;
        s->sd_overcurrent_check = true;
        s->sd_overcurrent_warning = false;
        s->sd_power_enabled = true;
        s->sd_overcurrent_retry_count = 0;
    }
    if (version_id < 54) {
        s->usb_msd_power_off_time = RASPI4_USB_MSD_PWR_OFF_TIME;
        s->usb_power_cycle_legacy = raspi4_usb_legacy_power_cycle(s);
        s->usb_power_enabled = true;
        s->usb_power_off_consumed = true;
        s->usb_power_off_elapsed_ms = 0;
    }
    if (s->usb_msd_startup_delay > RASPI4_USB_MSD_STARTUP_DELAY_MAX) {
        return -EINVAL;
    }
    if (s->usb_msd_power_off_time > RASPI4_USB_MSD_PWR_OFF_TIME_MAX ||
        s->usb_power_cycle_legacy != raspi4_usb_legacy_power_cycle(s) ||
        (s->pending_boot_action == RASPI4_PENDING_USB_POWER_OFF &&
         (s->cm4 || s->usb_power_enabled ||
          !s->usb_power_off_consumed ||
          s->usb_power_off_elapsed_ms >=
              s->usb_msd_power_off_time))) {
        return -EINVAL;
    }
    if ((!s->sd_power_enabled) !=
            (s->pending_boot_action == RASPI4_PENDING_SD_OVERCURRENT) ||
        (s->pending_boot_action == RASPI4_PENDING_SD_OVERCURRENT &&
         !s->sd_overcurrent_retry_count) ||
        (s->sd_overcurrent_warning && s->sd_overcurrent_check)) {
        return -EINVAL;
    }
    if (s->rpiboot_phase >= RASPI4_RPIBOOT__MAX ||
        s->rpiboot_file_index > 2 ||
        s->rpiboot_bulk_received > s->rpiboot_bulk_expected ||
        s->rpiboot_bootcode_alloc > RASPI4_RPIBOOT_MAX_BOOTCODE_SIZE ||
        s->rpiboot_config_alloc > RASPI4_RPIBOOT_MAX_FILE_SIZE ||
        s->rpiboot_boot_img_alloc > RASPI4_RPIBOOT_MAX_FILE_SIZE ||
        s->rpiboot_bootcode_size > s->rpiboot_bootcode_alloc ||
        s->rpiboot_config_size > s->rpiboot_config_alloc ||
        s->rpiboot_boot_img_size > s->rpiboot_boot_img_alloc) {
        return -EINVAL;
    }
    if (s->pending_boot_action == RASPI4_PENDING_NETWORK_TFTP_DALLY &&
        !s->network_tftp_dally_server_port) {
        return -EINVAL;
    }
    if (s->boot_health >= RASPI4_HEALTH__MAX) {
        return -EINVAL;
    }
    if (s->tftp_ip_set && !raspi4_ipv4_is_unicast(s->tftp_ip)) {
        return -EINVAL;
    }
    if ((s->client_ip_set && !raspi4_ipv4_is_unicast(s->client_ip)) ||
        (s->gateway_set && !raspi4_ipv4_is_unicast(s->gateway)) ||
        (s->subnet_set &&
         (!s->subnet || (~s->subnet & ((~s->subnet) + 1))))) {
        return -EINVAL;
    }
    if (version_id < 8) {
        s->network_tftp_block_number = s->network_tftp_next_block;
    }
    if (version_id < 11) {
        s->network_tftp_file_retransmits = 0;
        s->network_tftp_retransmit_count = 0;
        s->network_tftp_retransmit_remaining_ns =
            RASPI4_TFTP_RETRANSMIT_INITIAL_MS * SCALE_MS;
    }
    if (version_id < 12) {
        s->dhcp_req_timeout = RASPI4_DHCP_REQ_TIMEOUT;
    }
    if (version_id < 13) {
        s->network_dhcp_retransmit_count = 0;
        s->network_dhcp_retransmit_remaining_ns = 0;
    }
    if (version_id < 14) {
        s->tftp_ip_set = false;
        s->tftp_ip = 0;
    }
    if (version_id < 15) {
        s->dhcp_option97 = RASPI4_DHCP_OPTION97_DEFAULT;
    }
    if (version_id < 16) {
        s->network_tftp_dally_server_port = 0;
        s->network_tftp_dally_block = 0;
    }
    if (version_id < 17) {
        s->client_ip_set = false;
        s->client_ip = 0;
        s->subnet_set = false;
        s->subnet = 0;
        s->gateway_set = false;
        s->gateway = 0;
        s->network_subnet = 0;
        s->network_gateway = 0;
        s->network_next_hop_ip = s->network_server_ip;
    }
    if (version_id < 18) {
        s->network_tftp_block_size = RASPI4_TFTP_CLASSIC_BLOCK_SIZE;
        s->network_tftp_options_disabled = true;
        s->network_tftp_oack_accepted = false;
        s->network_tftp_tsize_valid = false;
        s->network_tftp_tsize = 0;
    }
    if (version_id < 19) {
        s->network_dhcp_server_ip = s->network_server_ip;
    }
    if (version_id < 20) {
        s->network_proxy_tftp_ip = 0;
    }
    if (version_id < 21) {
        s->network_dns_server_ip = 0;
        s->network_dns_query_id = 0;
        s->network_dns_retransmit_count = 0;
        s->network_arp_for_dns = false;
        s->network_tftp_hostname[0] = 0;
    }
    if (version_id < 22) {
        s->secure_public_key_valid = false;
        memset(s->secure_public_key, 0, sizeof(s->secure_public_key));
    }
    if (version_id < 23) {
        s->http_host_set = false;
        s->http_host[0] = 0;
        pstrcpy((char *)s->http_path, sizeof(s->http_path),
                RASPI4_HTTP_DEFAULT_PATH);
        s->http_port = RASPI4_HTTP_DEFAULT_PORT;
        s->http_cacert_hash_set = false;
        s->http_cacert_hash[0] = 0;
        s->network_http_mode = false;
        s->network_http_state = RASPI4_HTTP_IDLE;
        s->network_http_client_port = 0;
        s->network_http_client_seq = 0;
        s->network_http_request_seq = 0;
        s->network_http_server_seq = 0;
        s->network_http_response_size = 0;
        s->network_http_content_length = 0;
        s->network_http_header_parsed = false;
        s->network_http_response = NULL;
    }
    if (version_id < 24) {
        s->network_http_ooo_sequence = 0;
        s->network_http_ooo_size = 0;
        s->network_http_ooo_fin = false;
        s->network_http_ooo_fin_sequence = 0;
        s->network_http_ooo_data = NULL;
        s->network_http_ooo_valid = NULL;
    }
    if (version_id < 25) {
        s->network_http_ooo_fin = false;
    }
    if (version_id < 26) {
        s->network_http_ooo_fin_sequence =
            s->network_http_ooo_fin ?
                s->network_http_ooo_sequence +
                    s->network_http_ooo_size : 0;
        if (s->network_http_ooo_size) {
            s->network_http_ooo_valid =
                g_malloc(s->network_http_ooo_size);
            memset(s->network_http_ooo_valid, 1,
                   s->network_http_ooo_size);
        } else {
            s->network_http_ooo_valid = NULL;
        }
    }
    if (version_id < 27) {
        s->network_http_default_host = false;
        s->network_http_tls = false;
    }
    if (version_id < 28) {
        s->net_install_enabled = true;
        s->net_install_at_power_on = false;
        s->net_install_override = false;
    }
    if (version_id < 73) {
        s->net_install_keyboard_wait = 900;
        s->net_install_keyboard_present = false;
        s->net_install_shift_held = false;
    }
    if (version_id < 74) {
        pstrcpy((char *)s->pxe_option43, sizeof(s->pxe_option43),
                RASPI4_PXE_OPTION43_DEFAULT);
    }
    if (version_id < 75) {
        s->boot_disable_hdmi = false;
    }
    if (version_id < 76) {
        s->net_install_usb_fallback_consumed = false;
        s->usb_boot_files_missing = false;
    }
    if (version_id < 77) {
        s->hdmi_delay = RASPI4_HDMI_DELAY_DEFAULT;
        s->hdmi_diagnostics_pending = false;
        s->hdmi_diagnostics_visible = false;
        s->hdmi_diagnostics_remaining_ns = 0;
    }
    if (version_id < 78) {
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
    }
    if ((s->netconsole_enabled &&
         (!memchr(s->netconsole, 0, sizeof(s->netconsole)) ||
          !s->netconsole[0] ||
          !s->netconsole_source_port ||
          !s->netconsole_destination_port ||
          !raspi4_ipv4_is_unicast(s->netconsole_source_ip))) ||
        (!s->netconsole_enabled && s->netconsole[0]) ||
        (s->pending_boot_action == RASPI4_PENDING_NETCONSOLE_LINK &&
         !s->netconsole_enabled)) {
        return -EINVAL;
    }
    if ((s->boot_disable_hdmi &&
         (s->hdmi_diagnostics_pending ||
          s->hdmi_diagnostics_visible)) ||
        (s->hdmi_diagnostics_pending &&
         s->hdmi_diagnostics_visible)) {
        return -EINVAL;
    }
    if (version_id < 31) {
        s->tryboot = false;
    }
    if (version_id < 32) {
        s->bootvar0 = 0;
    }
    if (s->secure_public_key_valid) {
        raspi4_set_secure_boot_status(s, "config-verified");
    }
    if (s->network_tftp_hostname[0]) {
        const uint8_t *terminator = memchr(
            s->network_tftp_hostname, 0,
            sizeof(s->network_tftp_hostname));
        char hostname[RASPI4_DNS_NAME_MAX + 1];

        if (!terminator ||
            !raspi4_dns_hostname(
                s->network_tftp_hostname,
                terminator - s->network_tftp_hostname, hostname) ||
            strcmp((const char *)s->network_tftp_hostname, hostname)) {
            return -EINVAL;
        }
    }
    if (s->network_proxy_tftp_ip &&
        !raspi4_ipv4_is_unicast(s->network_proxy_tftp_ip)) {
        return -EINVAL;
    }
    if (s->pending_boot_action == RASPI4_PENDING_NETWORK_DHCP &&
        s->network_dhcp_phase == RASPI4_DHCP_PHASE_REQUEST &&
        !raspi4_ipv4_is_unicast(s->network_dhcp_server_ip)) {
        return -EINVAL;
    }
    if ((s->pending_boot_action == RASPI4_PENDING_NETWORK_DNS ||
         (s->pending_boot_action == RASPI4_PENDING_NETWORK_ARP &&
          s->network_arp_for_dns)) &&
        (!raspi4_ipv4_is_unicast(s->network_dns_server_ip) ||
         !s->network_tftp_hostname[0])) {
        return -EINVAL;
    }
    if ((s->pending_boot_action == RASPI4_PENDING_NETWORK_TFTP ||
         s->pending_boot_action == RASPI4_PENDING_NETWORK_TFTP_DALLY) &&
        (s->network_tftp_block_size < 8 ||
         s->network_tftp_block_size > RASPI4_TFTP_REQUESTED_BLOCK_SIZE ||
         (s->network_tftp_options_disabled &&
          s->network_tftp_block_size != RASPI4_TFTP_CLASSIC_BLOCK_SIZE) ||
         (s->network_tftp_oack_accepted &&
          (!s->network_tftp_server_port ||
           s->network_tftp_block_number != 1 || s->network_tftp_size)) ||
         (s->network_tftp_tsize_valid &&
          s->network_tftp_tsize > s->network_tftp_expected_size))) {
        return -EINVAL;
    }
    if ((s->pending_boot_action == RASPI4_PENDING_NETWORK_ARP ||
         s->pending_boot_action == RASPI4_PENDING_NETWORK_DNS ||
         s->pending_boot_action == RASPI4_PENDING_NETWORK_TFTP ||
         s->pending_boot_action == RASPI4_PENDING_NETWORK_HTTP ||
         s->pending_boot_action == RASPI4_PENDING_NETWORK_TFTP_DALLY) &&
        !raspi4_ipv4_is_unicast(s->network_next_hop_ip)) {
        return -EINVAL;
    }
    if (s->pending_boot_action == RASPI4_PENDING_FIRMWARE_DELAY &&
        (!s->bootcode_delay_consumed || !s->bootcode_delay_seconds ||
         s->boot_order_index >=
             raspi4_boot_order_nibbles(s->boot_order))) {
        return -EINVAL;
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
    timer = raspi4_pending_boot_timer(s);
    if (timer) {
        if (s->pending_boot_action == RASPI4_PENDING_EEPROM_FLASH) {
            s->eeprom_flash_deadline_us =
                (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                 s->pending_boot_remaining_ns) / 1000;
        }
        timer_mod_ns(timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                            s->pending_boot_remaining_ns);
    }
    if (s->boot_watchdog_armed) {
        timer_mod_ns(s->boot_watchdog_timer,
                     qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                     s->boot_watchdog_remaining_ns);
    }
    if (s->hdmi_diagnostics_pending) {
        timer_mod_ns(s->hdmi_diagnostics_timer,
                     qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                     s->hdmi_diagnostics_remaining_ns);
    }

    switch (s->pending_boot_action) {
    case RASPI4_PENDING_NONE:
        return 0;
    case RASPI4_PENDING_NETCONSOLE_LINK:
        if (!s->netconsole_enabled) {
            return -EINVAL;
        }
        raspi4_set_boot_observation(s, "netconsole-link-wait", "network");
        return 0;
    case RASPI4_PENDING_USB_DISCOVERY:
    case RASPI4_PENDING_USB_LUN:
    case RASPI4_PENDING_USB_STARTUP:
    case RASPI4_PENDING_USB_POWER_OFF:
        if (s->boot_order_index >= raspi4_boot_order_nibbles(s->boot_order)) {
            return -EINVAL;
        }
        nibble = (s->boot_order >> (s->boot_order_index * 4)) & 0xf;
        if (nibble != 0x4 && nibble != 0x5) {
            return -EINVAL;
        }
        s->boot_usb_discovery_wait =
            s->pending_boot_action == RASPI4_PENDING_USB_DISCOVERY;
        source = raspi4_boot_source_name(s, nibble);
        raspi4_set_boot_observation(
            s, s->pending_boot_action == RASPI4_PENDING_USB_POWER_OFF ?
                "usb-power-off-wait" :
            s->pending_boot_action == RASPI4_PENDING_USB_STARTUP ?
                "usb-startup-delay" :
            s->boot_usb_discovery_wait ? "usb-discovery-wait" :
                                         "usb-lun-wait",
            source);
        return 0;
    case RASPI4_PENDING_NETWORK_DHCP:
    case RASPI4_PENDING_NETWORK_TFTP:
    case RASPI4_PENDING_NETWORK_ARP:
    case RASPI4_PENDING_NETWORK_DNS:
    case RASPI4_PENDING_NETWORK_HTTP:
    case RASPI4_PENDING_NETWORK_TFTP_DALLY:
        if (s->boot_order_index >= raspi4_boot_order_nibbles(s->boot_order)) {
            return -EINVAL;
        }
        nibble = (s->boot_order >> (s->boot_order_index * 4)) & 0xf;
        if ((nibble != 0x2 && nibble != 0x7) || !s->network_attempt ||
            (nibble == 0x7) != s->network_http_mode) {
            return -EINVAL;
        }
        if (s->pending_boot_action == RASPI4_PENDING_NETWORK_DHCP &&
            s->network_boot_wire &&
            s->network_dhcp_phase != RASPI4_DHCP_PHASE_DISCOVER &&
            s->network_dhcp_phase != RASPI4_DHCP_PHASE_REQUEST) {
            return -EINVAL;
        }
        if (s->network_boot_wire &&
            s->pending_boot_action != RASPI4_PENDING_NETWORK_DHCP &&
            s->pending_boot_action != RASPI4_PENDING_NETWORK_DNS &&
            !(s->pending_boot_action == RASPI4_PENDING_NETWORK_ARP &&
              s->network_arp_for_dns) &&
            s->pending_boot_action != RASPI4_PENDING_NETWORK_TFTP_DALLY) {
            migrated_expected_size = s->network_tftp_expected_size;
            if (s->network_tftp_size > RASPI4_INITRAMFS_MAX_SIZE ||
                s->network_tftp_expected_size >
                    RASPI4_INITRAMFS_MAX_SIZE ||
                s->network_tftp_size > s->network_tftp_expected_size ||
                !raspi4_network_prepare_tftp(s, true) ||
                s->network_tftp_expected_size != migrated_expected_size) {
                return -EINVAL;
            }
        }
        if (s->network_boot_wire &&
            (s->pending_boot_action == RASPI4_PENDING_NETWORK_TFTP ||
             s->pending_boot_action == RASPI4_PENDING_NETWORK_HTTP)) {
            if (s->network_tftp_retransmit_remaining_ns >
                s->pending_boot_remaining_ns) {
                return -EINVAL;
            }
            timer_mod_ns(s->network_tftp_retransmit_timer,
                         qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                         s->network_tftp_retransmit_remaining_ns);
        }
        if (s->pending_boot_action == RASPI4_PENDING_NETWORK_HTTP &&
            (s->network_http_state <= RASPI4_HTTP_IDLE ||
             s->network_http_state >= RASPI4_HTTP__MAX ||
             s->network_http_response_size >
                 s->network_tftp_expected_size + RASPI4_HTTP_HEADER_MAX ||
             (s->network_http_header_parsed &&
              (!s->network_http_content_length ||
               s->network_http_response_size >
                   s->network_http_content_length)) ||
             s->network_http_ooo_size > RASPI4_HTTP_OOO_MAX ||
             (s->network_http_ooo_size &&
              (int32_t)(s->network_http_ooo_sequence -
                        s->network_http_server_seq) <= 0) ||
             (!s->network_http_ooo_size &&
              s->network_http_ooo_sequence) ||
             (s->network_http_ooo_fin &&
              ((int32_t)(s->network_http_ooo_fin_sequence -
                         s->network_http_server_seq) <= 0 ||
               (uint32_t)(s->network_http_ooo_fin_sequence -
                          s->network_http_server_seq) >
                   RASPI4_HTTP_OOO_MAX)) ||
             (!s->network_http_ooo_fin &&
              s->network_http_ooo_fin_sequence))) {
            return -EINVAL;
        }
        for (uint32_t i = 0; i < s->network_http_ooo_size; i++) {
            if (s->network_http_ooo_valid[i] > 1) {
                return -EINVAL;
            }
        }
        if (s->network_boot_wire &&
            (s->pending_boot_action == RASPI4_PENDING_NETWORK_DHCP ||
             s->pending_boot_action == RASPI4_PENDING_NETWORK_DNS)) {
            if (version_id < 13) {
                s->network_dhcp_retransmit_remaining_ns = MIN(
                    (uint64_t)s->dhcp_req_timeout * SCALE_MS,
                    s->pending_boot_remaining_ns);
            }
            if (s->network_dhcp_retransmit_remaining_ns >
                s->pending_boot_remaining_ns) {
                return -EINVAL;
            }
            timer_mod_ns(s->network_dhcp_retransmit_timer,
                         qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                         s->network_dhcp_retransmit_remaining_ns);
        }
        bcm2711_genet_set_boot_client_active(
            raspi4_genet(s), s->network_boot_wire);
        raspi4_set_boot_observation(s,
            s->pending_boot_action == RASPI4_PENDING_NETWORK_TFTP_DALLY ?
                "network-tftp-dally" :
            s->pending_boot_action == RASPI4_PENDING_NETWORK_ARP ?
                (s->network_arp_for_dns ? "network-dns-arp-wait" :
                                          "network-arp-wait") :
            s->pending_boot_action == RASPI4_PENDING_NETWORK_DNS ?
                "network-dns-wait" :
            s->pending_boot_action == RASPI4_PENDING_NETWORK_HTTP ?
                (s->network_http_state == RASPI4_HTTP_SYN_SENT ?
                    "network-http-connect" : "network-http-response") :
            s->pending_boot_action == RASPI4_PENDING_NETWORK_TFTP ?
                (s->network_boot_wire ? "network-tftp-transfer" :
                                        "network-tftp-wait") :
            s->network_dhcp_phase == RASPI4_DHCP_PHASE_REQUEST ?
                "network-dhcp-request-wait" : "network-dhcp-wait",
            s->pending_boot_action == RASPI4_PENDING_NETWORK_DHCP ?
                "network" :
                (s->network_http_mode ? "http" : "network"));
        return 0;
    case RASPI4_PENDING_RESTART:
        raspi4_set_boot_observation(s, "restart-cycle-wait", "restart");
        return 0;
    case RASPI4_PENDING_RECOVERY_REBOOT:
        if (s->self_update_status ==
                RASPI4_SELF_UPDATE_UPDATED_REBOOT) {
            raspi4_set_boot_observation(
                s, "self-update-updated-reboot", "boot-filesystem");
        } else {
            raspi4_set_recovery_status(s, "recovery-updated-reboot");
            raspi4_set_boot_observation(
                s, "recovery-updated-reboot", "sd-card");
        }
        return 0;
    case RASPI4_PENDING_EEPROM_FLASH:
        if (!s->eeprom_flash_image ||
            s->eeprom_flash_image_size != RASPI4_EEPROM_SIZE ||
            s->eeprom_flash_stage < RASPI4_EEPROM_FLASH_ERASE ||
            s->eeprom_flash_stage > RASPI4_EEPROM_FLASH_VERIFY) {
            return -EINVAL;
        }
        raspi4_set_recovery_status(s, "recovery-flash-active");
        raspi4_set_boot_observation(s, "recovery-flash-active", "sd-card");
        return 0;
    case RASPI4_PENDING_RESTART_WATCHDOG:
        raspi4_set_boot_observation(s, "restart-watchdog-pending",
                                    "restart");
        return 0;
    case RASPI4_PENDING_FATAL_REBOOT: {
        g_autofree char *reserved = NULL;

        if (!s->reboot_on_fatal_error) {
            return -EINVAL;
        }
        source = "none";
        if (s->boot_order_index <
                raspi4_boot_order_nibbles(s->boot_order)) {
            nibble = (s->boot_order >>
                      (s->boot_order_index * 4)) & 0xf;
            source = raspi4_boot_source_name(s, nibble);
            if (!source) {
                reserved = g_strdup_printf("reserved-0x%x", nibble);
                source = reserved;
            }
        }
        raspi4_set_boot_observation(s, "fatal-error-reboot-wait", source);
        return 0;
    }
    case RASPI4_PENDING_NET_INSTALL_KEYBOARD:
        if (!s->net_install_enabled || !s->net_install_keyboard_wait ||
            !s->net_install_keyboard_present ||
            s->net_install_override) {
            return -EINVAL;
        }
        raspi4_set_boot_observation(
            s, "net-install-keyboard-wait", "keyboard");
        return 0;
    case RASPI4_PENDING_SD_OVERCURRENT:
        if (s->cm4 || !s->sd_overcurrent_check || s->sd_power_enabled ||
            s->boot_order_index >=
                raspi4_boot_order_nibbles(s->boot_order)) {
            return -EINVAL;
        }
        nibble = (s->boot_order >> (s->boot_order_index * 4)) & 0xf;
        if (nibble != 0x0 && nibble != 0x1) {
            return -EINVAL;
        }
        source = raspi4_boot_source_name(s, nibble);
        raspi4_set_boot_observation(s, "sd-overcurrent-wait", source);
        return 0;
    case RASPI4_PENDING_SD_DETECT:
    case RASPI4_PENDING_SD_RETRY:
        if (s->boot_order_index >= raspi4_boot_order_nibbles(s->boot_order)) {
            return -EINVAL;
        }
        nibble = (s->boot_order >> (s->boot_order_index * 4)) & 0xf;
        if ((s->pending_boot_action == RASPI4_PENDING_SD_DETECT &&
             nibble != 0x0) ||
            (s->pending_boot_action == RASPI4_PENDING_SD_RETRY &&
             nibble != 0x1)) {
            return -EINVAL;
        }
        source = raspi4_boot_source_name(s, nibble);
        raspi4_set_boot_observation(
            s, s->pending_boot_action == RASPI4_PENDING_SD_DETECT ?
               "sd-card-detect-wait" : s->cm4 ? "emmc-retry-loop" :
                                                "sd-retry-loop",
            source);
        return 0;
    case RASPI4_PENDING_FIRMWARE_DELAY:
        nibble = (s->boot_order >> (s->boot_order_index * 4)) & 0xf;
        source = raspi4_boot_source_name(s, nibble);
        if (!source || (nibble != 0x0 && nibble != 0x1 &&
                        nibble != 0x2 && nibble != 0x4 &&
                        nibble != 0x5 && nibble != 0x6 &&
                        nibble != 0x7 && nibble != 0x3)) {
            return -EINVAL;
        }
        raspi4_set_firmware_status(s, "bootcode-delay");
        raspi4_set_boot_observation(s, "firmware-delay", source);
        return 0;
    default:
        g_assert_not_reached();
    }
}

static const VMStateDescription vmstate_raspi4_network_artifact = {
    .name = "raspi4/network-artifact",
    .version_id = 9,
    .minimum_version_id = 7,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(size, Raspi4NetworkArtifact),
        VMSTATE_VBUFFER_ALLOC_UINT32(data, Raspi4NetworkArtifact,
                                     7, NULL, size),
        VMSTATE_UINT32_V(maximum_size, Raspi4NetworkArtifact, 9),
        VMSTATE_UINT8_V(kind, Raspi4NetworkArtifact, 9),
        VMSTATE_BOOL_V(required, Raspi4NetworkArtifact, 9),
        VMSTATE_BOOL_V(missing, Raspi4NetworkArtifact, 9),
        VMSTATE_BUFFER_V(wire_filename, Raspi4NetworkArtifact, 9),
        VMSTATE_END_OF_LIST()
    },
};

const VMStateDescription vmstate_raspi4_boot = {
    .name = "raspi4/behavioral-boot",
    .version_id = 78,
    .minimum_version_id = 1,
    .pre_save = raspi4_boot_pre_save,
    .post_load = raspi4_boot_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(boot_order, Raspi4bMachineState),
        VMSTATE_BOOL_V(boot_uart_enabled,
                       Raspi4bMachineState, 62),
        VMSTATE_BOOL_V(boot_uart_active,
                       Raspi4bMachineState, 62),
        VMSTATE_BOOL_V(boot_uart_started,
                       Raspi4bMachineState, 62),
        VMSTATE_UINT64_V(boot_uart_bytes,
                         Raspi4bMachineState, 62),
        VMSTATE_UINT32_V(boot_uart_lines,
                         Raspi4bMachineState, 62),
        VMSTATE_BOOL_V(vl805_enabled,
                       Raspi4bMachineState, 63),
        VMSTATE_BOOL_V(vl805_initialized,
                       Raspi4bMachineState, 63),
        VMSTATE_UINT32_V(sd_quirks,
                         Raspi4bMachineState, 64),
        VMSTATE_INT32(max_restarts, Raspi4bMachineState),
        VMSTATE_INT32(sd_boot_max_retries, Raspi4bMachineState),
        VMSTATE_UINT32(usb_msd_discover_timeout, Raspi4bMachineState),
        VMSTATE_UINT32(usb_msd_lun_timeout, Raspi4bMachineState),
        VMSTATE_UINT32_V(usb_msd_startup_delay,
                         Raspi4bMachineState, 51),
        VMSTATE_UINT32_V(usb_msd_power_off_time,
                         Raspi4bMachineState, 54),
        VMSTATE_BOOL_V(usb_power_enabled,
                       Raspi4bMachineState, 54),
        VMSTATE_BOOL_V(usb_power_off_consumed,
                       Raspi4bMachineState, 54),
        VMSTATE_BOOL_V(usb_power_cycle_legacy,
                       Raspi4bMachineState, 54),
        VMSTATE_UINT64_V(usb_power_off_elapsed_ms,
                         Raspi4bMachineState, 54),
        VMSTATE_UINT32_ARRAY_V(usb_msd_exclude_vid_pid,
                               Raspi4bMachineState,
                               RASPI4_USB_MSD_EXCLUDE_MAX, 55),
        VMSTATE_UINT32_V(usb_boot_last_excluded_vid_pid,
                         Raspi4bMachineState, 55),
        VMSTATE_UINT8_V(usb_msd_exclude_count,
                        Raspi4bMachineState, 55),
        VMSTATE_UINT8_V(usb_boot_excluded_device_count,
                        Raspi4bMachineState, 55),
        VMSTATE_UINT8_V(usb_boot_eligible_device_count,
                        Raspi4bMachineState, 55),
        VMSTATE_UINT32_V(boot_watchdog_timeout,
                         Raspi4bMachineState, 52),
        VMSTATE_UINT8_V(boot_watchdog_partition,
                        Raspi4bMachineState, 52),
        VMSTATE_BOOL_V(boot_watchdog_armed,
                       Raspi4bMachineState, 52),
        VMSTATE_UINT64_V(boot_watchdog_remaining_ns,
                         Raspi4bMachineState, 52),
        VMSTATE_BOOL_V(reboot_on_fatal_error,
                       Raspi4bMachineState, 60),
        VMSTATE_UINT64_V(fatal_error_reboot_count,
                         Raspi4bMachineState, 60),
        VMSTATE_BOOL_V(sd_overcurrent,
                       Raspi4bMachineState, 53),
        VMSTATE_BOOL_V(sd_overcurrent_check,
                       Raspi4bMachineState, 53),
        VMSTATE_BOOL_V(sd_overcurrent_warning,
                       Raspi4bMachineState, 53),
        VMSTATE_BOOL_V(sd_power_enabled,
                       Raspi4bMachineState, 53),
        VMSTATE_UINT64_V(sd_overcurrent_retry_count,
                         Raspi4bMachineState, 53),
        VMSTATE_UINT64(boot_attempt_count, Raspi4bMachineState),
        VMSTATE_UINT64(boot_restart_count, Raspi4bMachineState),
        VMSTATE_UINT64(boot_retry_count, Raspi4bMachineState),
        VMSTATE_UINT64(boot_elapsed_ms, Raspi4bMachineState),
        VMSTATE_UINT8(boot_order_index, Raspi4bMachineState),
        VMSTATE_BOOL(boot_usb_discovery_wait, Raspi4bMachineState),
        VMSTATE_BOOL_V(bootcode_delay_consumed,
                       Raspi4bMachineState, 47),
        VMSTATE_UINT32_V(bootcode_delay_seconds,
                         Raspi4bMachineState, 47),
        VMSTATE_BOOL_V(firmware_uart_enabled,
                       Raspi4bMachineState, 48),
        VMSTATE_BOOL_V(firmware_uart_started,
                       Raspi4bMachineState, 48),
        VMSTATE_UINT64_V(firmware_uart_bytes,
                         Raspi4bMachineState, 48),
        VMSTATE_UINT32_V(firmware_uart_lines,
                         Raspi4bMachineState, 48),
        VMSTATE_BOOL_V(tryboot, Raspi4bMachineState, 31),
        VMSTATE_UINT32_V(bootvar0, Raspi4bMachineState, 32),
        VMSTATE_UINT32_V(eeprom_config_append_size,
                         Raspi4bMachineState, 61),
        VMSTATE_BUFFER_V(eeprom_config_append,
                         Raspi4bMachineState, 61),
        VMSTATE_UINT32_V(eeprom_bootconf_size,
                         Raspi4bMachineState, 65),
        VMSTATE_VBUFFER_ALLOC_UINT32(eeprom_bootconf,
                                     Raspi4bMachineState, 65, NULL,
                                     eeprom_bootconf_size),
        VMSTATE_UINT32_V(eeprom_public_key_size,
                         Raspi4bMachineState, 65),
        VMSTATE_UINT8_ARRAY_V(eeprom_public_key,
                              Raspi4bMachineState,
                              RASPI_SECURE_PUBLIC_KEY_SIZE, 65),
        VMSTATE_UINT32_V(min_boot_version,
                         Raspi4bMachineState, 66),
        VMSTATE_UINT8_V(selected_boot_mode,
                        Raspi4bMachineState, 67),
        VMSTATE_BOOL_V(eeprom_build_timestamp_valid,
                       Raspi4bMachineState, 68),
        VMSTATE_UINT32_V(eeprom_build_timestamp,
                         Raspi4bMachineState, 68),
        VMSTATE_BUFFER_V(eeprom_version, Raspi4bMachineState, 68),
        VMSTATE_UINT32_V(bootloader_signed,
                         Raspi4bMachineState, 69),
        VMSTATE_BOOL_V(eeprom_update_timestamp_valid,
                       Raspi4bMachineState, 70),
        VMSTATE_UINT32_V(eeprom_update_timestamp,
                         Raspi4bMachineState, 70),
        VMSTATE_BOOL_V(eeprom_capabilities_valid,
                       Raspi4bMachineState, 71),
        VMSTATE_UINT32_V(eeprom_capabilities,
                         Raspi4bMachineState, 71),
        VMSTATE_BOOL_V(usb_boot_identity_valid,
                       Raspi4bMachineState, 72),
        VMSTATE_UINT8_V(usb_boot_version,
                        Raspi4bMachineState, 72),
        VMSTATE_UINT32_V(usb_boot_route_string,
                         Raspi4bMachineState, 72),
        VMSTATE_UINT32_V(usb_boot_root_hub_port,
                         Raspi4bMachineState, 72),
        VMSTATE_UINT8_V(selected_boot_partition,
                       Raspi4bMachineState, 33),
        VMSTATE_UINT8_V(eeprom_partition, Raspi4bMachineState, 33),
        VMSTATE_BOOL_V(partition_walk, Raspi4bMachineState, 33),
        VMSTATE_BOOL_V(tryboot_a_b, Raspi4bMachineState, 33),
        VMSTATE_UINT8_V(usb_boot_selected_index,
                       Raspi4bMachineState, 34),
        VMSTATE_UINT8_V(usb_boot_selected_device,
                        Raspi4bMachineState, 38),
        VMSTATE_UINT8_V(usb_boot_selected_lun,
                        Raspi4bMachineState, 38),
        VMSTATE_UINT64_V(usb_boot_controller_reads,
                         Raspi4bMachineState, 37),
        VMSTATE_UINT64_V(usb_boot_controller_bytes,
                         Raspi4bMachineState, 37),
        VMSTATE_UINT64_V(usb_boot_controller_failures,
                         Raspi4bMachineState, 37),
        VMSTATE_UINT8_V(usb_boot_bot_stalls_consumed,
                        Raspi4bMachineState, 39),
        VMSTATE_UINT8_V(usb_boot_bot_phases_consumed,
                        Raspi4bMachineState, 39),
        VMSTATE_UINT64_V(usb_boot_bot_recoveries,
                         Raspi4bMachineState, 39),
        VMSTATE_UINT64_V(usb_boot_bot_phase_errors,
                         Raspi4bMachineState, 39),
        VMSTATE_UINT8_V(usb_boot_bot_cases_consumed,
                        Raspi4bMachineState, 40),
        VMSTATE_UINT64_V(usb_boot_bot_cases_tested,
                         Raspi4bMachineState, 40),
        VMSTATE_UINT8_V(recovery_status_id, Raspi4bMachineState, 35),
        VMSTATE_BUFFER_V(recovery_sha256, Raspi4bMachineState, 42),
        VMSTATE_UINT32_V(recovery_key_index, Raspi4bMachineState, 42),
        VMSTATE_UINT64_V(eeprom_erased_bytes, Raspi4bMachineState, 36),
        VMSTATE_UINT64_V(eeprom_programmed_bytes, Raspi4bMachineState, 36),
        VMSTATE_UINT64_V(eeprom_verified_bytes, Raspi4bMachineState, 36),
        VMSTATE_UINT32_V(eeprom_dirty_sector_count,
                         Raspi4bMachineState, 36),
        VMSTATE_UINT32_V(eeprom_program_page_count,
                         Raspi4bMachineState, 43),
        VMSTATE_UINT32_V(eeprom_nor_violation_bits,
                         Raspi4bMachineState, 43),
        VMSTATE_BOOL_V(eeprom_write_protect,
                       Raspi4bMachineState, 44),
        VMSTATE_BOOL_V(eeprom_nwp, Raspi4bMachineState, 44),
        VMSTATE_BOOL_V(eeprom_nwp_sampled,
                       Raspi4bMachineState, 44),
        VMSTATE_BOOL_V(eeprom_nwp_bridge_driven,
                       Raspi4bMachineState, 45),
        VMSTATE_UINT8_2DARRAY_V(hdmi_edid_name, Raspi4bMachineState,
                                RASPI_FIRMWARE_MAX_EDIDS,
                                RASPI_FIRMWARE_EDID_NAME_MAX, 46),
        VMSTATE_UINT8_ARRAY_V(hdmi_edid_status, Raspi4bMachineState,
                              RASPI_FIRMWARE_MAX_EDIDS, 46),
        VMSTATE_UINT8_V(eeprom_flash_stage, Raspi4bMachineState, 36),
        VMSTATE_UINT32_V(eeprom_erase_delay_us,
                         Raspi4bMachineState, 49),
        VMSTATE_UINT32_V(eeprom_program_delay_us,
                         Raspi4bMachineState, 49),
        VMSTATE_UINT32_V(eeprom_verify_delay_us,
                         Raspi4bMachineState, 49),
        VMSTATE_UINT64_V(eeprom_flash_elapsed_us,
                         Raspi4bMachineState, 49),
        VMSTATE_UINT64_V(eeprom_flash_deadline_us,
                         Raspi4bMachineState, 49),
        VMSTATE_UINT32_V(eeprom_flash_image_size,
                         Raspi4bMachineState, 49),
        VMSTATE_VBUFFER_ALLOC_UINT32(eeprom_flash_image,
                                     Raspi4bMachineState, 49, NULL,
                                     eeprom_flash_image_size),
        VMSTATE_BOOL_V(eeprom_flash_timing_complete,
                       Raspi4bMachineState, 49),
        VMSTATE_UINT8_V(otp_provision_rows_programmed,
                        Raspi4bMachineState, 41),
        VMSTATE_UINT8(pending_boot_action, Raspi4bMachineState),
        VMSTATE_UINT64(pending_boot_remaining_ns, Raspi4bMachineState),
        VMSTATE_UINT8_V(boot_health, Raspi4bMachineState, 3),
        VMSTATE_INT32_V(net_boot_max_retries, Raspi4bMachineState, 4),
        VMSTATE_UINT32_V(dhcp_timeout, Raspi4bMachineState, 4),
        VMSTATE_UINT32_V(tftp_file_timeout, Raspi4bMachineState, 4),
        VMSTATE_UINT32_V(network_attempt, Raspi4bMachineState, 4),
        VMSTATE_BOOL_V(network_boot_wire, Raspi4bMachineState, 5),
        VMSTATE_UINT8_V(network_dhcp_phase, Raspi4bMachineState, 5),
        VMSTATE_UINT32_V(network_dhcp_xid, Raspi4bMachineState, 5),
        VMSTATE_UINT32_V(network_offered_ip, Raspi4bMachineState, 5),
        VMSTATE_UINT32_V(network_server_ip, Raspi4bMachineState, 5),
        VMSTATE_UINT8_ARRAY_V(network_server_mac, Raspi4bMachineState, 6, 6),
        VMSTATE_UINT16_V(network_tftp_server_port, Raspi4bMachineState, 6),
        VMSTATE_UINT16_V(network_tftp_next_block, Raspi4bMachineState, 6),
        VMSTATE_UINT32_V(network_tftp_block_number,
                         Raspi4bMachineState, 8),
        VMSTATE_UINT32_V(network_tftp_size, Raspi4bMachineState, 6),
        VMSTATE_UINT32_V(network_tftp_expected_size, Raspi4bMachineState, 6),
        VMSTATE_VBUFFER_ALLOC_UINT32(network_tftp_data, Raspi4bMachineState,
                                     6, NULL, network_tftp_size),
        VMSTATE_UINT8_V(network_artifact_count, Raspi4bMachineState, 7),
        VMSTATE_UINT8_V(network_artifact_index, Raspi4bMachineState, 7),
        VMSTATE_UINT8_V(network_base_artifact_index,
                        Raspi4bMachineState, 7),
        VMSTATE_UINT8_V(network_overlay_artifact_index,
                        Raspi4bMachineState, 7),
        VMSTATE_STRUCT_ARRAY(network_artifacts, Raspi4bMachineState,
                             RASPI4_NETWORK_ARTIFACTS_MAX, 7,
                             vmstate_raspi4_network_artifact,
                             Raspi4NetworkArtifact),
        VMSTATE_BOOL_V(network_response_discovery,
                       Raspi4bMachineState, 9),
        VMSTATE_UINT8_V(network_discovery_phase,
                        Raspi4bMachineState, 9),
        VMSTATE_BOOL_V(network_prefix_fallback,
                       Raspi4bMachineState, 10),
        VMSTATE_UINT32_V(network_tftp_file_retransmits,
                         Raspi4bMachineState, 11),
        VMSTATE_UINT64_V(network_tftp_retransmit_count,
                         Raspi4bMachineState, 11),
        VMSTATE_UINT64_V(network_tftp_retransmit_remaining_ns,
                         Raspi4bMachineState, 11),
        VMSTATE_UINT32_V(dhcp_req_timeout, Raspi4bMachineState, 12),
        VMSTATE_UINT64_V(network_dhcp_retransmit_count,
                         Raspi4bMachineState, 13),
        VMSTATE_UINT64_V(network_dhcp_retransmit_remaining_ns,
                         Raspi4bMachineState, 13),
        VMSTATE_BOOL_V(tftp_ip_set, Raspi4bMachineState, 14),
        VMSTATE_UINT32_V(tftp_ip, Raspi4bMachineState, 14),
        VMSTATE_UINT8_V(tftp_prefix_mode, Raspi4bMachineState, 56),
        VMSTATE_UINT8_ARRAY_V(tftp_prefix, Raspi4bMachineState,
                              RASPI4_TFTP_PREFIX_MAX + 1, 56),
        VMSTATE_BOOL_V(network_tftp_prefix_fallback,
                       Raspi4bMachineState, 56),
        VMSTATE_UINT32_V(dhcp_option97, Raspi4bMachineState, 15),
        VMSTATE_UINT8_ARRAY_V(pxe_option43, Raspi4bMachineState,
                              RASPI4_PXE_OPTION43_MAX + 1, 74),
        VMSTATE_BOOL_V(boot_disable_hdmi,
                       Raspi4bMachineState, 75),
        VMSTATE_UINT32_V(hdmi_delay,
                         Raspi4bMachineState, 77),
        VMSTATE_BOOL_V(hdmi_diagnostics_pending,
                       Raspi4bMachineState, 77),
        VMSTATE_BOOL_V(hdmi_diagnostics_visible,
                       Raspi4bMachineState, 77),
        VMSTATE_UINT64_V(hdmi_diagnostics_remaining_ns,
                         Raspi4bMachineState, 77),
        VMSTATE_BOOL_V(netconsole_enabled,
                       Raspi4bMachineState, 78),
        VMSTATE_UINT8_ARRAY_V(netconsole,
                              Raspi4bMachineState,
                              RASPI4_NETCONSOLE_MAX + 1, 78),
        VMSTATE_UINT16_V(netconsole_source_port,
                         Raspi4bMachineState, 78),
        VMSTATE_UINT16_V(netconsole_destination_port,
                         Raspi4bMachineState, 78),
        VMSTATE_UINT32_V(netconsole_source_ip,
                         Raspi4bMachineState, 78),
        VMSTATE_UINT32_V(netconsole_destination_ip,
                         Raspi4bMachineState, 78),
        VMSTATE_UINT8_ARRAY_V(netconsole_destination_mac,
                              Raspi4bMachineState, 6, 78),
        VMSTATE_UINT64_V(netconsole_packets,
                         Raspi4bMachineState, 78),
        VMSTATE_UINT64_V(netconsole_bytes,
                         Raspi4bMachineState, 78),
        VMSTATE_BOOL_V(net_install_usb_fallback_consumed,
                       Raspi4bMachineState, 76),
        VMSTATE_BOOL_V(usb_boot_files_missing,
                       Raspi4bMachineState, 76),
        VMSTATE_UINT8_ARRAY_V(boot_mac_address.a,
                              Raspi4bMachineState, 6, 57),
        VMSTATE_UINT8_V(boot_mac_address_source,
                        Raspi4bMachineState, 57),
        VMSTATE_UINT16_V(network_tftp_dally_server_port,
                         Raspi4bMachineState, 16),
        VMSTATE_UINT16_V(network_tftp_dally_block,
                         Raspi4bMachineState, 16),
        VMSTATE_BOOL_V(client_ip_set, Raspi4bMachineState, 17),
        VMSTATE_UINT32_V(client_ip, Raspi4bMachineState, 17),
        VMSTATE_BOOL_V(subnet_set, Raspi4bMachineState, 17),
        VMSTATE_UINT32_V(subnet, Raspi4bMachineState, 17),
        VMSTATE_BOOL_V(gateway_set, Raspi4bMachineState, 17),
        VMSTATE_UINT32_V(gateway, Raspi4bMachineState, 17),
        VMSTATE_UINT32_V(network_subnet, Raspi4bMachineState, 17),
        VMSTATE_UINT32_V(network_gateway, Raspi4bMachineState, 17),
        VMSTATE_UINT32_V(network_next_hop_ip, Raspi4bMachineState, 17),
        VMSTATE_UINT16_V(network_tftp_block_size,
                         Raspi4bMachineState, 18),
        VMSTATE_BOOL_V(network_tftp_options_disabled,
                       Raspi4bMachineState, 18),
        VMSTATE_BOOL_V(network_tftp_oack_accepted,
                       Raspi4bMachineState, 18),
        VMSTATE_BOOL_V(network_tftp_tsize_valid,
                       Raspi4bMachineState, 18),
        VMSTATE_UINT32_V(network_tftp_tsize,
                         Raspi4bMachineState, 18),
        VMSTATE_UINT32_V(network_dhcp_server_ip,
                         Raspi4bMachineState, 19),
        VMSTATE_UINT32_V(network_proxy_tftp_ip,
                         Raspi4bMachineState, 20),
        VMSTATE_UINT32_V(network_dns_server_ip,
                         Raspi4bMachineState, 21),
        VMSTATE_UINT16_V(network_dns_query_id,
                         Raspi4bMachineState, 21),
        VMSTATE_UINT64_V(network_dns_retransmit_count,
                         Raspi4bMachineState, 21),
        VMSTATE_BOOL_V(network_arp_for_dns,
                       Raspi4bMachineState, 21),
        VMSTATE_BUFFER_V(network_tftp_hostname,
                         Raspi4bMachineState, 21),
        VMSTATE_BOOL_V(secure_public_key_valid,
                       Raspi4bMachineState, 22),
        VMSTATE_UINT8_ARRAY_V(secure_public_key,
                              Raspi4bMachineState,
                              RASPI_SECURE_PUBLIC_KEY_SIZE, 22),
        VMSTATE_BOOL_V(http_host_set, Raspi4bMachineState, 23),
        VMSTATE_BUFFER_V(http_host, Raspi4bMachineState, 23),
        VMSTATE_BUFFER_V(http_path, Raspi4bMachineState, 23),
        VMSTATE_UINT16_V(http_port, Raspi4bMachineState, 23),
        VMSTATE_BOOL_V(http_cacert_hash_set, Raspi4bMachineState, 23),
        VMSTATE_BUFFER_V(http_cacert_hash, Raspi4bMachineState, 23),
        VMSTATE_BOOL_V(network_http_mode, Raspi4bMachineState, 23),
        VMSTATE_UINT8_V(network_http_state, Raspi4bMachineState, 23),
        VMSTATE_UINT16_V(network_http_client_port,
                         Raspi4bMachineState, 23),
        VMSTATE_UINT32_V(network_http_client_seq,
                         Raspi4bMachineState, 23),
        VMSTATE_UINT32_V(network_http_request_seq,
                         Raspi4bMachineState, 23),
        VMSTATE_UINT32_V(network_http_server_seq,
                         Raspi4bMachineState, 23),
        VMSTATE_UINT32_V(network_http_response_size,
                         Raspi4bMachineState, 23),
        VMSTATE_UINT32_V(network_http_content_length,
                         Raspi4bMachineState, 23),
        VMSTATE_BOOL_V(network_http_header_parsed,
                       Raspi4bMachineState, 23),
        VMSTATE_VBUFFER_ALLOC_UINT32(network_http_response,
                                     Raspi4bMachineState, 23, NULL,
                                     network_http_response_size),
        VMSTATE_UINT32_V(network_http_ooo_sequence,
                         Raspi4bMachineState, 24),
        VMSTATE_UINT32_V(network_http_ooo_size,
                         Raspi4bMachineState, 24),
        VMSTATE_BOOL_V(network_http_ooo_fin,
                       Raspi4bMachineState, 25),
        VMSTATE_UINT32_V(network_http_ooo_fin_sequence,
                         Raspi4bMachineState, 26),
        VMSTATE_VBUFFER_ALLOC_UINT32(network_http_ooo_data,
                                     Raspi4bMachineState, 24, NULL,
                                     network_http_ooo_size),
        VMSTATE_VBUFFER_ALLOC_UINT32(network_http_ooo_valid,
                                     Raspi4bMachineState, 26, NULL,
                                     network_http_ooo_size),
        VMSTATE_BOOL_V(network_http_default_host,
                       Raspi4bMachineState, 27),
        VMSTATE_BOOL_V(network_http_tls, Raspi4bMachineState, 27),
        VMSTATE_BOOL_V(net_install_enabled, Raspi4bMachineState, 28),
        VMSTATE_BOOL_V(net_install_at_power_on,
                       Raspi4bMachineState, 28),
        VMSTATE_BOOL_V(net_install_override,
                       Raspi4bMachineState, 28),
        VMSTATE_UINT32_V(net_install_keyboard_wait,
                         Raspi4bMachineState, 73),
        VMSTATE_BOOL_V(net_install_keyboard_present,
                       Raspi4bMachineState, 73),
        VMSTATE_BOOL_V(net_install_shift_held,
                       Raspi4bMachineState, 73),
        VMSTATE_BOOL_V(enable_self_update, Raspi4bMachineState, 58),
        VMSTATE_BOOL_V(freeze_version, Raspi4bMachineState, 58),
        VMSTATE_UINT8_V(self_update_status, Raspi4bMachineState, 58),
        VMSTATE_UINT32_V(pcie_ext_cfg_index,
                         Raspi4bMachineState, 29),
        VMSTATE_UINT32_ARRAY_V(pcie_regs, Raspi4bMachineState,
                               RASPI4_PCIE_REG_SIZE / sizeof(uint32_t), 29),
        VMSTATE_BOOL_V(rpiboot_dwc2_active,
                       Raspi4bMachineState, 30),
        VMSTATE_BOOL_V(rpiboot_control_in,
                       Raspi4bMachineState, 30),
        VMSTATE_UINT8_V(rpiboot_control_pending,
                        Raspi4bMachineState, 59),
        VMSTATE_UINT8_V(rpiboot_control_value,
                        Raspi4bMachineState, 59),
        VMSTATE_UINT8_V(rpiboot_configuration,
                        Raspi4bMachineState, 59),
        VMSTATE_UINT8_V(rpiboot_phase,
                        Raspi4bMachineState, 30),
        VMSTATE_UINT32_V(rpiboot_bulk_expected,
                         Raspi4bMachineState, 30),
        VMSTATE_UINT32_V(rpiboot_bulk_received,
                         Raspi4bMachineState, 30),
        VMSTATE_UINT32_V(rpiboot_expected_bootcode,
                         Raspi4bMachineState, 30),
        VMSTATE_UINT32_V(rpiboot_bootcode_size,
                         Raspi4bMachineState, 30),
        VMSTATE_UINT32_V(rpiboot_bootcode_alloc,
                         Raspi4bMachineState, 30),
        VMSTATE_UINT8_V(rpiboot_file_index,
                        Raspi4bMachineState, 30),
        VMSTATE_UINT32_V(rpiboot_file_expected,
                         Raspi4bMachineState, 30),
        VMSTATE_UINT32_V(rpiboot_config_size,
                         Raspi4bMachineState, 30),
        VMSTATE_UINT32_V(rpiboot_config_alloc,
                         Raspi4bMachineState, 30),
        VMSTATE_UINT32_V(rpiboot_boot_img_size,
                         Raspi4bMachineState, 30),
        VMSTATE_UINT32_V(rpiboot_boot_img_alloc,
                         Raspi4bMachineState, 30),
        VMSTATE_BOOL_V(rpiboot_second_stage,
                       Raspi4bMachineState, 30),
        VMSTATE_BOOL_V(rpiboot_bootcode_trusted,
                       Raspi4bMachineState, 50),
        VMSTATE_BUFFER_V(rpiboot_boot_message,
                         Raspi4bMachineState, 30),
        VMSTATE_VBUFFER_ALLOC_UINT32(rpiboot_bootcode,
                                     Raspi4bMachineState, 30, NULL,
                                     rpiboot_bootcode_alloc),
        VMSTATE_VBUFFER_ALLOC_UINT32(rpiboot_config,
                                     Raspi4bMachineState, 30, NULL,
                                     rpiboot_config_alloc),
        VMSTATE_VBUFFER_ALLOC_UINT32(rpiboot_boot_img,
                                     Raspi4bMachineState, 30, NULL,
                                     rpiboot_boot_img_alloc),
        VMSTATE_END_OF_LIST()
    },
};

char *raspi4b_get_boot_mode(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->behavioral_boot ? "behavioral" : "direct");
}

void raspi4b_set_boot_mode(Object *obj, const char *value, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    if (!g_ascii_strcasecmp(value, "direct")) {
        s->behavioral_boot = false;
    } else if (!g_ascii_strcasecmp(value, "behavioral")) {
        s->behavioral_boot = true;
    } else {
        error_setg(errp, "boot-mode must be 'direct' or 'behavioral'");
    }
}

char *raspi4b_get_bootsys_trusted_sha256(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->bootsys_trusted_sha256 ?
                    s->bootsys_trusted_sha256 : "");
}

void raspi4b_set_bootsys_trusted_sha256(Object *obj,
                                               const char *value,
                                               Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    char digest[65];

    if (!value[0]) {
        g_clear_pointer(&s->bootsys_trusted_sha256, g_free);
        return;
    }
    if (!raspi4_parse_sha256(value, digest)) {
        error_setg(errp, "bootsys-trusted-sha256 must be exactly 64 "
                   "hexadecimal characters");
        return;
    }
    g_free(s->bootsys_trusted_sha256);
    s->bootsys_trusted_sha256 = g_strdup(digest);
}

bool raspi4b_get_boot_disable_hdmi(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->boot_disable_hdmi;
}

char *raspi4b_get_boot_state(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->boot_state ? s->boot_state : "uninitialized");
}

char *raspi4b_get_boot_source(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->boot_source ? s->boot_source : "none");
}

char *raspi4b_get_bootsys_sha256(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->bootsys_sha256[0] ? s->bootsys_sha256 : "none");
}

char *raspi4b_get_bootsys_dependencies_sha256(Object *obj,
                                                     Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->bootsys_dependencies_sha256[0] ?
                    s->bootsys_dependencies_sha256 : "none");
}

void raspi4b_get_bootsys_dependency_count(Object *obj, Visitor *v,
                                                 const char *name,
                                                 void *opaque, Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->bootsys_dependency_count;

    visit_type_uint32(v, name, &value, errp);
}

void raspi4b_get_bootsys_key_index(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->bootsys_key_index;

    visit_type_uint32(v, name, &value, errp);
}

char *raspi4b_get_boot_mac_address(Object *obj, Error **errp)
{
    const MACAddr *mac = &RASPI4B_MACHINE(obj)->boot_mac_address;

    return g_strdup_printf("%02x:%02x:%02x:%02x:%02x:%02x",
                           mac->a[0], mac->a[1], mac->a[2],
                           mac->a[3], mac->a[4], mac->a[5]);
}

char *raspi4b_get_boot_mac_address_source(Object *obj, Error **errp)
{
    static const char * const names[RASPI4_MAC__MAX] = {
        [RASPI4_MAC_CONFIGURED] = "configured",
        [RASPI4_MAC_EXPLICIT] = "mac-address",
        [RASPI4_MAC_CUSTOMER_OTP] = "customer-otp",
    };
    uint8_t source = RASPI4B_MACHINE(obj)->boot_mac_address_source;

    return g_strdup(source < RASPI4_MAC__MAX ? names[source] : "invalid");
}

void raspi4b_get_boot_order(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->boot_order;

    visit_type_uint32(v, name, &value, errp);
}

void raspi4b_get_boot_watchdog_timeout(Object *obj, Visitor *v,
                                               const char *name, void *opaque,
                                               Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->boot_watchdog_timeout;

    visit_type_uint32(v, name, &value, errp);
}

void raspi4b_get_boot_watchdog_partition(Object *obj, Visitor *v,
                                                 const char *name,
                                                 void *opaque, Error **errp)
{
    uint8_t value = RASPI4B_MACHINE(obj)->boot_watchdog_partition;

    visit_type_uint8(v, name, &value, errp);
}

bool raspi4b_get_boot_watchdog_armed(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->boot_watchdog_armed;
}

void raspi4b_get_boot_watchdog_remaining(Object *obj, Visitor *v,
                                                 const char *name,
                                                 void *opaque, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    uint64_t value = 0;

    if (s->boot_watchdog_armed && timer_pending(s->boot_watchdog_timer)) {
        uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        uint64_t expiry = timer_expire_time_ns(s->boot_watchdog_timer);

        if (expiry > now) {
            value = expiry - now;
        }
    }
    visit_type_uint64(v, name, &value, errp);
}

void raspi4b_get_boot_attempt_count(Object *obj, Visitor *v,
                                           const char *name, void *opaque,
                                           Error **errp)
{
    uint64_t value = RASPI4B_MACHINE(obj)->boot_attempt_count;

    visit_type_uint64(v, name, &value, errp);
}

void raspi4b_get_boot_restart_count(Object *obj, Visitor *v,
                                           const char *name, void *opaque,
                                           Error **errp)
{
    uint64_t value = RASPI4B_MACHINE(obj)->boot_restart_count;

    visit_type_uint64(v, name, &value, errp);
}

void raspi4b_get_boot_retry_count(Object *obj, Visitor *v,
                                         const char *name, void *opaque,
                                         Error **errp)
{
    uint64_t value = RASPI4B_MACHINE(obj)->boot_retry_count;

    visit_type_uint64(v, name, &value, errp);
}

void raspi4b_get_boot_elapsed_ms(Object *obj, Visitor *v,
                                        const char *name, void *opaque,
                                        Error **errp)
{
    uint64_t value = RASPI4B_MACHINE(obj)->boot_elapsed_ms;

    visit_type_uint64(v, name, &value, errp);
}

void raspi4b_get_boot_order_index(Object *obj, Visitor *v,
                                         const char *name, void *opaque,
                                         Error **errp)
{
    uint8_t value = RASPI4B_MACHINE(obj)->boot_order_index;

    visit_type_uint8(v, name, &value, errp);
}

bool raspi4b_get_boot_uart_enabled(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->boot_uart_enabled;
}

bool raspi4b_get_boot_uart_active(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->boot_uart_active;
}

void raspi4b_get_boot_uart_bytes(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint64_t value = RASPI4B_MACHINE(obj)->boot_uart_bytes;

    visit_type_uint64(v, name, &value, errp);
}

void raspi4b_get_boot_uart_lines(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->boot_uart_lines;

    visit_type_uint32(v, name, &value, errp);
}

char *raspi4b_get_boot_uart_format(Object *obj, Error **errp)
{
    return g_strdup("primary-uart0-115200-8n1-cleanroom-v1");
}

char *raspi4b_get_boot_health(Object *obj, Error **errp)
{
    return g_strdup(raspi4b_boot_health_name(
        RASPI4B_MACHINE(obj)->boot_health));
}

void raspi4b_set_boot_health(Object *obj, const char *value,
                                    Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    Raspi4BootHealth next;
    const char *event;
    const char *outcome = "success";

    if (!strcmp(value, "kernel-started")) {
        next = RASPI4_HEALTH_KERNEL_STARTED;
        event = "health.kernel-started";
    } else if (!strcmp(value, "userspace-ready")) {
        next = RASPI4_HEALTH_USERSPACE_READY;
        event = "health.userspace-ready";
    } else if (!strcmp(value, "failed")) {
        next = RASPI4_HEALTH_FAILED;
        event = "health.failed";
        outcome = "failure";
    } else {
        error_setg(errp, "boot-health must be kernel-started, "
                   "userspace-ready, or failed");
        return;
    }
    if (!s->behavioral_boot || !s->handoff_status ||
        strcmp(s->handoff_status, "ready")) {
        error_setg(errp, "boot-health requires a completed behavioral ARM "
                   "handoff");
        return;
    }
    if (next == s->boot_health) {
        return;
    }
    if ((next == RASPI4_HEALTH_KERNEL_STARTED &&
         s->boot_health != RASPI4_HEALTH_NONE) ||
        (next == RASPI4_HEALTH_USERSPACE_READY &&
         s->boot_health != RASPI4_HEALTH_KERNEL_STARTED) ||
        (s->boot_health == RASPI4_HEALTH_FAILED)) {
        error_setg(errp, "invalid boot-health transition from %s to %s",
                   raspi4b_boot_health_name(s->boot_health), value);
        return;
    }
    s->boot_health = next;
    trace_raspi4b_boot_event("health", event,
                             s->boot_source ? s->boot_source : "none",
                             outcome, next);
}

void raspi4b_get_boot_partition(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    uint8_t value = RASPI4B_MACHINE(obj)->boot_partition;

    visit_type_uint8(v, name, &value, errp);
}

void raspi4b_get_bootloader_signed(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->bootloader_signed;

    visit_type_uint32(v, name, &value, errp);
}
