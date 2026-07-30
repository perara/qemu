/*
 * Raspberry Pi 4 EEPROM, recovery and OTP policy
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/arm/raspi4b-internal.h"

static const char * const raspi4_recovery_statuses[] = {
    "none",
    "recovery-invalid",
    "recovery-discovered",
    "recovery-executed",
    "recovery-no-eeprom",
    "recovery-size-invalid",
    "recovery-write-protected",
    "recovery-program-error",
    "recovery-erase-interrupted",
    "recovery-interrupted",
    "recovery-verify-interrupted",
    "recovery-verify-failed",
    "recovery-rename-interrupted",
    "recovery-rename-failed",
    "recovery-reboot-interrupted",
    "recovery-updated-reboot",
    "recovery-updated-stop",
    "recovery-signature-invalid",
    "recovery-provision-invalid",
    "recovery-provision-signature-invalid",
    "recovery-provision-key-mismatch",
    "recovery-provision-persistence-required",
    "recovery-provision-write-failed",
    "recovery-provision-jtag-unsupported",
    "recovery-provision-power-failed",
    "recovery-format-invalid",
    "recovery-trust-required",
    "recovery-trust-mismatch",
    "recovery-nor-violation",
    "recovery-write-protect-config-invalid",
    "recovery-write-protect-config-locked",
    "recovery-write-protect-status-error",
    "recovery-flash-active",
    "recovery-source-lost",
};

void raspi4_set_recovery_status(Raspi4bMachineState *s,
                                       const char *status)
{
    unsigned int id;

    for (id = 0; id < ARRAY_SIZE(raspi4_recovery_statuses); id++) {
        if (!strcmp(status, raspi4_recovery_statuses[id])) {
            break;
        }
    }
    g_assert(id < ARRAY_SIZE(raspi4_recovery_statuses));
    g_free(s->recovery_status);
    s->recovery_status = g_strdup(status);
    s->recovery_status_id = id;
}

BCM2835OTPState *raspi4_otp(Raspi4bMachineState *s)
{
    return &s->soc.peripherals.parent_obj.otp;
}

const char *raspi4_recovery_status_from_id(uint8_t id)
{
    return id < ARRAY_SIZE(raspi4_recovery_statuses) ?
           raspi4_recovery_statuses[id] : NULL;
}

void raspi4_recovery_reboot(void *opaque)
{
    Raspi4bMachineState *s = opaque;
    bool self_update =
        s->self_update_status == RASPI4_SELF_UPDATE_UPDATED_REBOOT;

    s->pending_boot_action = RASPI4_PENDING_NONE;
    trace_raspi4b_boot_event(
        self_update ? "eeprom" : "recovery",
        self_update ? "eeprom.self-update-reboot" : "recovery.reboot",
        self_update ? "boot-filesystem" : "sd-card", "success", 0);
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
}

bool raspi4_otp_customer_key_present(BCM2835OTPState *otp)
{
    for (unsigned int row = 0;
         row < BCM2711_OTP_CUSTOMER_KEY_HASH_LEN; row++) {
        if (bcm2835_otp_get_row(
                otp, BCM2711_OTP_CUSTOMER_KEY_HASH_ROW + row)) {
            return true;
        }
    }
    return false;
}

bool raspi4_recovery_signature_valid(const uint8_t *image,
                                            size_t image_length,
                                            const uint8_t *signature,
                                            size_t signature_length,
                                            bool *timestamp_valid,
                                            uint32_t *timestamp)
{
    g_autofree char *digest = NULL;
    size_t offset = 64;

    *timestamp_valid = false;
    *timestamp = 0;

    if (signature_length < 64) {
        return false;
    }
    for (unsigned i = 0; i < 64; i++) {
        if (!g_ascii_isxdigit(signature[i])) {
            return false;
        }
    }
    digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256, image,
                                         image_length);
    if (g_ascii_strncasecmp(digest, (const char *)signature, 64)) {
        return false;
    }
    if (offset == signature_length) {
        return true;
    }
    if (signature[offset++] != '\n') {
        return false;
    }
    if (offset == signature_length) {
        return true;
    }
    if (signature_length - offset < 5 ||
        memcmp(signature + offset, "ts: ", 4)) {
        return false;
    }
    offset += 4;
    {
        const uint8_t *end = memchr(signature + offset, '\n',
                                    signature_length - offset);
        size_t length = end ? end - (signature + offset) :
                              signature_length - offset;
        g_autofree char *text = NULL;
        const char *parse_end = NULL;
        uint64_t value;

        if (!length || length > 10 ||
            (end && end + 1 != signature + signature_length)) {
            return false;
        }
        text = g_strndup((const char *)signature + offset, length);
        if (qemu_strtou64(text, &parse_end, 10, &value) < 0 ||
            *parse_end || value > UINT32_MAX) {
            return false;
        }
        *timestamp_valid = true;
        *timestamp = value;
    }
    return true;
}

/*
 * The BCM2711 signing tool emits:
 *
 *   payload | payload length | ROM key index | RSA-2048 | HMAC-SHA1
 *
 * QEMU cannot reproduce the silicon HMAC check without Raspberry Pi's
 * non-public key.  Structural validation plus an independently pinned digest
 * is the fail-closed behavioral replacement at this boundary.
 */
static uint32_t raspi4_eeprom_stage_delay_us(
    const Raspi4bMachineState *s)
{
    switch (s->eeprom_flash_stage) {
    case RASPI4_EEPROM_FLASH_ERASE:
        return s->eeprom_erase_delay_us;
    case RASPI4_EEPROM_FLASH_PROGRAM:
        return s->eeprom_program_delay_us;
    case RASPI4_EEPROM_FLASH_VERIFY:
        return s->eeprom_verify_delay_us;
    default:
        return 0;
    }
}

void raspi4_eeprom_flash_schedule(Raspi4bMachineState *s,
                                         bool continue_cadence)
{
    uint32_t delay = raspi4_eeprom_stage_delay_us(s);

    if (continue_cadence) {
        s->eeprom_flash_deadline_us += delay;
    } else {
        s->eeprom_flash_deadline_us =
            qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) + delay;
    }
    timer_mod(s->eeprom_flash_timer, s->eeprom_flash_deadline_us);
}

bool raspi4_eeprom_status_store(Raspi4bMachineState *s, bool value,
                                       Error **errp)
{
    uint8_t persisted[RASPI4_EEPROM_STATUS_SIZE] = { 0 };

    if (!s->eeprom_status_loaded) {
        s->eeprom_write_protect = value;
        return true;
    }
    persisted[RASPI4_EEPROM_STATUS_WP_OFFSET] = value;
    persisted[RASPI4_EEPROM_STATUS_UPDATE_VALID_OFFSET] =
        s->eeprom_update_timestamp_valid;
    stl_le_p(persisted + RASPI4_EEPROM_STATUS_UPDATE_TIMESTAMP_OFFSET,
             s->eeprom_update_timestamp);
    if (blk_pwrite(s->eeprom_status, 0, sizeof(persisted), persisted, 0) < 0 ||
        blk_flush(s->eeprom_status) < 0) {
        error_setg(errp, "could not persist EEPROM write-protect status");
        return false;
    }
    s->eeprom_write_protect = value;
    return true;
}

bool raspi4_eeprom_update_timestamp_store(Raspi4bMachineState *s,
                                                 bool valid,
                                                 uint32_t timestamp)
{
    bool previous_valid = s->eeprom_update_timestamp_valid;
    uint32_t previous_timestamp = s->eeprom_update_timestamp;

    s->eeprom_update_timestamp_valid = valid;
    s->eeprom_update_timestamp = timestamp;
    if (!raspi4_eeprom_status_store(s, s->eeprom_write_protect, NULL)) {
        s->eeprom_update_timestamp_valid = previous_valid;
        s->eeprom_update_timestamp = previous_timestamp;
        return false;
    }
    return true;
}

bool raspi4_eeprom_nwp_live(Raspi4bMachineState *s,
                                   bool *bridge_driven)
{
    int level = bcm2838_gpio_get_eeprom_nwp(&s->soc.peripherals.gpio);

    if (level >= 0) {
        *bridge_driven = true;
        return level;
    }
    *bridge_driven = false;
    return s->eeprom_nwp;
}

static size_t raspi4_eeprom_async_limit(const Raspi4bMachineState *s)
{
    bool selected =
        (s->eeprom_flash_stage == RASPI4_EEPROM_FLASH_ERASE &&
         s->eeprom_fail_stage == RASPI4_EEPROM_FAIL_ERASE) ||
        (s->eeprom_flash_stage == RASPI4_EEPROM_FLASH_PROGRAM &&
         s->eeprom_fail_stage == RASPI4_EEPROM_FAIL_PROGRAM) ||
        (s->eeprom_flash_stage == RASPI4_EEPROM_FLASH_VERIFY &&
         s->eeprom_fail_stage == RASPI4_EEPROM_FAIL_VERIFY);

    return selected ? MIN((uint64_t)s->eeprom_flash_image_size,
                          s->eeprom_fail_after) :
                      s->eeprom_flash_image_size;
}

static void raspi4_eeprom_async_finish(Raspi4bMachineState *s,
                                       Raspi4ProgramResult result)
{
    const char *status;
    const char *event;

    s->pending_boot_action = RASPI4_PENDING_NONE;
    s->eeprom_flash_timing_complete = false;
    s->eeprom_flash_image_size = 0;
    g_clear_pointer(&s->eeprom_flash_image, g_free);
    switch (result) {
    case RASPI4_PROGRAM_ERROR:
        status = "recovery-program-error";
        event = "eeprom.program";
        break;
    case RASPI4_PROGRAM_NOR_VIOLATION:
        status = "recovery-nor-violation";
        event = "eeprom.nor-violation";
        break;
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
}

void raspi4_eeprom_flash_step(void *opaque)
{
    Raspi4bMachineState *s = opaque;
    uint8_t erase[4096];
    uint8_t current[256];
    uint8_t page[256];
    uint8_t verify[4096];
    size_t offset;
    size_t limit;
    size_t chunk;

    if (s->pending_boot_action != RASPI4_PENDING_EEPROM_FLASH ||
        !s->eeprom_flash_image ||
        s->eeprom_flash_image_size != RASPI4_EEPROM_SIZE ||
        !s->eeprom || !blk_is_inserted(s->eeprom)) {
        raspi4_eeprom_async_finish(s, RASPI4_PROGRAM_ERROR);
        return;
    }
    s->eeprom_flash_elapsed_us += raspi4_eeprom_stage_delay_us(s);
    limit = raspi4_eeprom_async_limit(s);

    switch (s->eeprom_flash_stage) {
    case RASPI4_EEPROM_FLASH_ERASE:
        offset = s->eeprom_erased_bytes;
        if (offset >= limit && offset < s->eeprom_flash_image_size) {
            raspi4_eeprom_async_finish(s, RASPI4_ERASE_INTERRUPTED);
            return;
        }
        chunk = MIN((size_t)4096, limit - offset);
        memset(erase, 0xff, chunk);
        if (s->eeprom_stuck_zero_offset >= offset &&
            s->eeprom_stuck_zero_offset < offset + chunk) {
            erase[s->eeprom_stuck_zero_offset - offset] &=
                ~s->eeprom_stuck_zero_mask;
        }
        if (blk_pwrite(s->eeprom, offset, chunk, erase, 0) < 0 ||
            blk_flush(s->eeprom) < 0) {
            raspi4_eeprom_async_finish(s, RASPI4_PROGRAM_ERROR);
            return;
        }
        s->eeprom_erased_bytes += chunk;
        s->eeprom_dirty_sector_count =
            DIV_ROUND_UP(s->eeprom_erased_bytes, 4096);
        if (s->eeprom_erased_bytes == s->eeprom_flash_image_size) {
            s->eeprom_flash_stage = RASPI4_EEPROM_FLASH_PROGRAM;
        } else if (s->eeprom_erased_bytes == limit) {
            raspi4_eeprom_async_finish(s, RASPI4_ERASE_INTERRUPTED);
            return;
        }
        break;

    case RASPI4_EEPROM_FLASH_PROGRAM: {
        uint32_t violations = 0;

        offset = s->eeprom_programmed_bytes;
        if (offset >= limit && offset < s->eeprom_flash_image_size) {
            raspi4_eeprom_async_finish(s, RASPI4_PROGRAM_INTERRUPTED);
            return;
        }
        chunk = MIN((size_t)256, limit - offset);
        if (blk_pread(s->eeprom, offset, chunk, current, 0) < 0) {
            raspi4_eeprom_async_finish(s, RASPI4_PROGRAM_ERROR);
            return;
        }
        for (size_t i = 0; i < chunk; i++) {
            uint8_t violation =
                s->eeprom_flash_image[offset + i] & ~current[i];

            violations += ctpop8(violation);
            page[i] = current[i] & s->eeprom_flash_image[offset + i];
        }
        if (blk_pwrite(s->eeprom, offset, chunk, page, 0) < 0) {
            raspi4_eeprom_async_finish(s, RASPI4_PROGRAM_ERROR);
            return;
        }
        s->eeprom_programmed_bytes += chunk;
        s->eeprom_program_page_count++;
        s->eeprom_nor_violation_bits += violations;
        if ((QEMU_IS_ALIGNED(s->eeprom_programmed_bytes, 4096) ||
             s->eeprom_programmed_bytes == limit || violations) &&
            blk_flush(s->eeprom) < 0) {
            raspi4_eeprom_async_finish(s, RASPI4_PROGRAM_ERROR);
            return;
        }
        s->eeprom_dirty_sector_count = MAX(
            s->eeprom_dirty_sector_count,
            (uint32_t)DIV_ROUND_UP(s->eeprom_programmed_bytes, 4096));
        if (violations) {
            raspi4_eeprom_async_finish(s,
                                       RASPI4_PROGRAM_NOR_VIOLATION);
            return;
        }
        if (s->eeprom_programmed_bytes == s->eeprom_flash_image_size) {
            s->eeprom_flash_stage = RASPI4_EEPROM_FLASH_VERIFY;
        } else if (s->eeprom_programmed_bytes == limit) {
            raspi4_eeprom_async_finish(s, RASPI4_PROGRAM_INTERRUPTED);
            return;
        }
        break;
    }

    case RASPI4_EEPROM_FLASH_VERIFY:
        offset = s->eeprom_verified_bytes;
        if (offset >= limit && offset < s->eeprom_flash_image_size) {
            raspi4_eeprom_async_finish(s, RASPI4_VERIFY_INTERRUPTED);
            return;
        }
        chunk = MIN((size_t)4096, limit - offset);
        if (blk_pread(s->eeprom, offset, chunk, verify, 0) < 0) {
            raspi4_eeprom_async_finish(s, RASPI4_PROGRAM_ERROR);
            return;
        }
        if (s->eeprom_fail_stage ==
                RASPI4_EEPROM_FAIL_VERIFY_MISMATCH &&
            s->eeprom_fail_after >= offset &&
            s->eeprom_fail_after < offset + chunk) {
            verify[s->eeprom_fail_after - offset] ^= 1;
        }
        if (memcmp(verify, s->eeprom_flash_image + offset, chunk)) {
            size_t matched = 0;

            while (matched < chunk &&
                   verify[matched] ==
                       s->eeprom_flash_image[offset + matched]) {
                matched++;
            }
            s->eeprom_verified_bytes += matched;
            raspi4_eeprom_async_finish(s, RASPI4_VERIFY_FAILED);
            return;
        }
        s->eeprom_verified_bytes += chunk;
        if (s->eeprom_verified_bytes == s->eeprom_flash_image_size) {
            s->eeprom_flash_stage = RASPI4_EEPROM_FLASH_COMPLETE;
            s->pending_boot_action = RASPI4_PENDING_NONE;
            s->eeprom_flash_timing_complete = true;
            if (!raspi4_try_sd_recovery(s)) {
                s->eeprom_flash_timing_complete = false;
                s->eeprom_flash_image_size = 0;
                g_clear_pointer(&s->eeprom_flash_image, g_free);
                raspi4_set_recovery_status(s, "recovery-source-lost");
                raspi4_set_boot_observation(
                    s, "recovery-source-lost", "sd-card");
            }
            return;
        }
        if (s->eeprom_verified_bytes == limit) {
            raspi4_eeprom_async_finish(s, RASPI4_VERIFY_INTERRUPTED);
            return;
        }
        break;

    default:
        raspi4_eeprom_async_finish(s, RASPI4_PROGRAM_ERROR);
        return;
    }
    raspi4_set_recovery_status(s, "recovery-flash-active");
    raspi4_set_boot_observation(s, "recovery-flash-active", "sd-card");
    raspi4_eeprom_flash_schedule(s, true);
}

bool raspi4_eeprom_files_parse(const uint8_t *image,
                                      size_t image_size,
                                      Raspi4EepromFiles *files)
{
    size_t offset = 0;

    memset(files, 0, sizeof(*files));
    while (offset + 8 <= image_size) {
        uint32_t magic = ldl_be_p(image + offset);
        uint32_t length = ldl_be_p(image + offset + 4);
        size_t next;

        if (magic == 0 || magic == UINT32_MAX) {
            return true;
        }
        if ((magic & RASPI4_EEPROM_MAGIC_MASK) != RASPI4_EEPROM_MAGIC ||
            length > image_size - offset - 8) {
            return false;
        }
        next = QEMU_ALIGN_UP(offset + 8 + length, 8);
        if (next <= offset || next > image_size) {
            return false;
        }
        if (!offset && magic == RASPI4_EEPROM_MAGIC) {
            files->bootsys = image + 8;
            files->bootsys_size = length;
        } else if (magic == RASPI4_EEPROM_DEPENDENCY_MAGIC) {
            const uint8_t *contents = image + offset + 8;
            const uint8_t *terminator;

            if (length <= RASPI4_EEPROM_DEPENDENCY_NAME_LEN +
                          RASPI4_EEPROM_DEPENDENCY_HASH_SIZE ||
                files->dependency_count ==
                    RASPI4_EEPROM_DEPENDENCY_MAX) {
                return false;
            }
            terminator = memchr(contents, 0,
                                RASPI4_EEPROM_DEPENDENCY_NAME_LEN);
            if (!terminator || terminator == contents ||
                !buffer_is_zero(terminator,
                                contents +
                                RASPI4_EEPROM_DEPENDENCY_NAME_LEN -
                                terminator)) {
                return false;
            }
            for (const uint8_t *p = contents; p < terminator; p++) {
                if (!g_ascii_isprint(*p)) {
                    return false;
                }
            }
            for (unsigned int i = 0; i < files->dependency_count; i++) {
                if (!memcmp(files->dependencies[i].name, contents,
                            RASPI4_EEPROM_DEPENDENCY_NAME_LEN)) {
                    return false;
                }
            }
            memcpy(files->dependencies[files->dependency_count].name,
                   contents, terminator - contents);
            files->dependencies[files->dependency_count].contents =
                contents + RASPI4_EEPROM_DEPENDENCY_NAME_LEN;
            files->dependencies[files->dependency_count].contents_size =
                length - RASPI4_EEPROM_DEPENDENCY_NAME_LEN;
            files->dependency_count++;
        } else if (magic == RASPI4_EEPROM_FILE_MAGIC &&
            length >= RASPI4_EEPROM_NAME_LEN + 4) {
            const uint8_t *name = image + offset + 8;
            const uint8_t *contents =
                image + offset + RASPI4_EEPROM_FILE_HDR;
            size_t contents_size = length - RASPI4_EEPROM_NAME_LEN - 4;

            if (!memcmp(name, "bootconf.txt", RASPI4_EEPROM_NAME_LEN)) {
                if (files->bootconf) {
                    return false;
                }
                files->bootconf = contents;
                files->bootconf_size = contents_size;
            } else if (!memcmp(name, "bootconf.sig",
                               RASPI4_EEPROM_NAME_LEN)) {
                if (files->bootconf_signature) {
                    return false;
                }
                files->bootconf_signature = contents;
                files->bootconf_signature_size = contents_size;
            } else if (!memcmp(name, "pubkey.bin\0\0",
                               RASPI4_EEPROM_NAME_LEN)) {
                if (files->public_key) {
                    return false;
                }
                files->public_key = contents;
                files->public_key_size = contents_size;
            }
        }
        offset = next;
    }
    return offset == image_size;
}

static RaspiSecureResult raspi4_secure_dependencies_verify(
    Raspi4bMachineState *s, const Raspi4EepromFiles *files,
    size_t bootsys_payload_size)
{
    g_autoptr(GChecksum) set_checksum = g_checksum_new(G_CHECKSUM_SHA256);
    bool bootmain = false;
    bool mcb = false;
    bool memsys = false;

    for (unsigned int i = 0; i < files->dependency_count; i++) {
        const uint8_t *contents = files->dependencies[i].contents;
        size_t contents_size = files->dependencies[i].contents_size;
        const uint8_t *expected_hash =
            contents + contents_size - RASPI4_EEPROM_DEPENDENCY_HASH_SIZE;
        uint8_t padded_name[RASPI4_EEPROM_DEPENDENCY_NAME_LEN] = {};

        memcpy(padded_name, files->dependencies[i].name,
               strlen(files->dependencies[i].name));
        g_checksum_update(set_checksum, padded_name, sizeof(padded_name));
        g_checksum_update(set_checksum, expected_hash,
                          RASPI4_EEPROM_DEPENDENCY_HASH_SIZE);
        bootmain |= !strcmp(files->dependencies[i].name, "bootmain");
        mcb |= !strcmp(files->dependencies[i].name, "mcb.bin");
        memsys |= g_str_has_prefix(files->dependencies[i].name, "memsys") &&
                  g_str_has_suffix(files->dependencies[i].name, ".bin");
    }
    pstrcpy(s->bootsys_dependencies_sha256,
            sizeof(s->bootsys_dependencies_sha256),
            g_checksum_get_string(set_checksum));
    s->bootsys_dependency_count = files->dependency_count;
    if (!bootmain || !mcb || !memsys) {
        return RASPI_SECURE_DEPENDENCY_MISSING;
    }

    for (unsigned int i = 0; i < files->dependency_count; i++) {
        g_autoptr(GByteArray) decoded = NULL;
        g_autofree char *actual_hash = NULL;
        char expected_hash_hex[RASPI4_EEPROM_DEPENDENCY_HASH_SIZE * 2 + 1];
        const uint8_t *contents = files->dependencies[i].contents;
        size_t contents_size = files->dependencies[i].contents_size;
        size_t frame_size =
            contents_size - RASPI4_EEPROM_DEPENDENCY_HASH_SIZE;
        const uint8_t *expected_hash = contents + frame_size;

        if (!raspi4_buffer_contains(files->bootsys, bootsys_payload_size,
                                    expected_hash,
                                    RASPI4_EEPROM_DEPENDENCY_HASH_SIZE)) {
            return RASPI_SECURE_DEPENDENCY_HASH_MISMATCH;
        }
        decoded = raspi4_lz4_frame_decode(contents, frame_size);
        if (!decoded) {
            return RASPI_SECURE_DEPENDENCY_FORMAT;
        }
        actual_hash = g_compute_checksum_for_data(
            G_CHECKSUM_SHA256, decoded->data, decoded->len);
        for (unsigned int j = 0;
             j < RASPI4_EEPROM_DEPENDENCY_HASH_SIZE; j++) {
            snprintf(expected_hash_hex + j * 2, 3, "%02x", expected_hash[j]);
        }
        if (strcmp(actual_hash, expected_hash_hex)) {
            return RASPI_SECURE_DEPENDENCY_HASH_MISMATCH;
        }
    }
    return RASPI_SECURE_OK;
}

static RaspiSecureResult raspi4_secure_bootsys_verify(
    Raspi4bMachineState *s, const Raspi4EepromFiles *files);

bool raspi4_recovery_parse_write_protect(
    const uint8_t *config, size_t config_size, int *value)
{
    g_autofree char *text = g_strndup((const char *)config, config_size);
    g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
    bool present = false;

    if (memchr(config, '\0', config_size)) {
        return false;
    }
    *value = -1;
    for (unsigned int i = 0; lines[i]; i++) {
        char *line = g_strstrip(lines[i]);
        char *separator;
        int64_t parsed;

        if (!*line || *line == '#') {
            continue;
        }
        separator = strchr(line, '=');
        if (!separator) {
            continue;
        }
        *separator++ = '\0';
        line = g_strstrip(line);
        separator = g_strstrip(separator);
        if (strcmp(line, "eeprom_write_protect")) {
            continue;
        }
        if (present || qemu_strtoi64(separator, NULL, 0, &parsed) < 0 ||
            parsed < -1 || parsed > 1) {
            return false;
        }
        present = true;
        *value = parsed;
    }
    return true;
}

Raspi4OtpProvisionResult raspi4_secure_provision_prepare(
    Raspi4bMachineState *s, const uint8_t *config, size_t config_size,
    const uint8_t *eeprom, size_t eeprom_size, uint8_t key_hash[32])
{
    bool program_present, program_pubkey;
    bool revoke_present, revoke_devkey;
    bool jtag_present, program_jtag_lock;
    Raspi4EepromFiles files;
    g_autofree char *hash = NULL;
    uint8_t existing_hash[32];

    if (!raspi4_provision_parse_bool(
            config, config_size, "program_pubkey",
            &program_present, &program_pubkey) ||
        !raspi4_provision_parse_bool(
            config, config_size, "revoke_devkey",
            &revoke_present, &revoke_devkey) ||
        !raspi4_provision_parse_bool(
            config, config_size, "program_jtag_lock",
            &jtag_present, &program_jtag_lock)) {
        return RASPI4_OTP_PROVISION_CONFIG_INVALID;
    }
    if ((!program_present || !program_pubkey) &&
        (!revoke_present || !revoke_devkey) &&
        (!jtag_present || !program_jtag_lock)) {
        return RASPI4_OTP_PROVISION_NONE;
    }
    if (!program_pubkey) {
        return RASPI4_OTP_PROVISION_CONFIG_INVALID;
    }
    if (program_jtag_lock) {
        /*
         * The public Pi 4 contract does not disclose its OTP bit encoding.
         * Reject instead of pretending to burn an irreversible fuse.
         */
        return RASPI4_OTP_PROVISION_JTAG_UNSUPPORTED;
    }
    if (!s->otp) {
        return RASPI4_OTP_PROVISION_PERSISTENCE_REQUIRED;
    }
    if (!raspi4_eeprom_files_parse(eeprom, eeprom_size, &files) ||
        !files.bootconf || !files.bootconf_signature || !files.public_key ||
        files.public_key_size != RASPI_SECURE_PUBLIC_KEY_SIZE) {
        return RASPI4_OTP_PROVISION_SIGNATURE_INVALID;
    }
    if (raspi4_secure_bootsys_verify(s, &files) != RASPI_SECURE_OK) {
        return RASPI4_OTP_PROVISION_SIGNATURE_INVALID;
    }
    if (raspi_secure_verify(
            files.public_key, files.bootconf, files.bootconf_size,
            files.bootconf_signature, files.bootconf_signature_size,
            NULL) != RASPI_SECURE_OK) {
        return RASPI4_OTP_PROVISION_SIGNATURE_INVALID;
    }
    hash = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, files.public_key, files.public_key_size);
    for (unsigned int i = 0; i < sizeof(existing_hash); i++) {
        existing_hash[i] = bcm2835_otp_get_row(
            raspi4_otp(s), BCM2711_OTP_CUSTOMER_KEY_HASH_ROW + i / 4) >>
            ((i % 4) * 8);
    }
    for (unsigned int i = 0; i < sizeof(existing_hash); i++) {
        key_hash[i] = g_ascii_xdigit_value(hash[i * 2]) << 4 |
                      g_ascii_xdigit_value(hash[i * 2 + 1]);
    }
    if (raspi4_otp_customer_key_present(raspi4_otp(s)) &&
        memcmp(existing_hash, key_hash, sizeof(existing_hash))) {
        return RASPI4_OTP_PROVISION_KEY_MISMATCH;
    }
    return RASPI4_OTP_PROVISION_READY;
}

static Raspi4OtpCommitResult raspi4_secure_provision_program_row(
    Raspi4bMachineState *s, unsigned int row, uint32_t value)
{
    BCM2835OTPState *otp = raspi4_otp(s);

    if (s->otp_provision_rows_programmed ==
        s->otp_provision_fail_after) {
        return RASPI4_OTP_COMMIT_INTERRUPTED;
    }
    bcm2835_otp_set_row(otp, row, value);
    s->otp_provision_rows_programmed++;
    return (bcm2835_otp_get_row(otp, row) & value) == value ?
           RASPI4_OTP_COMMIT_OK : RASPI4_OTP_COMMIT_WRITE_FAILED;
}

Raspi4OtpCommitResult raspi4_secure_provision_commit(
    Raspi4bMachineState *s, const uint8_t key_hash[32])
{
    BCM2835OTPState *otp = raspi4_otp(s);
    Raspi4OtpCommitResult result;

    s->otp_provision_rows_programmed = 0;
    result = raspi4_secure_provision_program_row(
        s, BCM2711_OTP_BOOTMODE_ROW,
        BCM2711_OTP_BOOTMODE_SECURE_BOOT);
    if (result != RASPI4_OTP_COMMIT_OK) {
        return result;
    }
    result = raspi4_secure_provision_program_row(
        s, BCM2711_OTP_BOOTMODE_COPY_ROW,
        BCM2711_OTP_BOOTMODE_SECURE_BOOT);
    if (result != RASPI4_OTP_COMMIT_OK) {
        return result;
    }
    result = raspi4_secure_provision_program_row(
        s, BCM2711_OTP_SECURE_BOOT_FLAGS_ROW,
        BCM2711_OTP_SECURE_BOOT_FLAGS_PRODUCTION);
    if (result != RASPI4_OTP_COMMIT_OK) {
        return result;
    }
    for (unsigned int i = 0; i < BCM2711_OTP_CUSTOMER_KEY_HASH_LEN; i++) {
        result = raspi4_secure_provision_program_row(
            s, BCM2711_OTP_CUSTOMER_KEY_HASH_ROW + i,
            ldl_le_p(key_hash + i * sizeof(uint32_t)));
        if (result != RASPI4_OTP_COMMIT_OK) {
            return result;
        }
    }

    for (unsigned int i = 0; i < BCM2711_OTP_CUSTOMER_KEY_HASH_LEN; i++) {
        if (bcm2835_otp_get_row(
                otp, BCM2711_OTP_CUSTOMER_KEY_HASH_ROW + i) !=
            ldl_le_p(key_hash + i * sizeof(uint32_t))) {
            return RASPI4_OTP_COMMIT_WRITE_FAILED;
        }
    }
    return RASPI4_OTP_COMMIT_OK;
}

static RaspiSecureResult raspi4_secure_bootsys_verify(
    Raspi4bMachineState *s, const Raspi4EepromFiles *files)
{
    g_autofree char *bootsys_hash = NULL;
    const uint8_t *trailer;
    uint32_t payload_size;
    uint32_t key_index;
    RaspiSecureResult result;
    bool signature_erased = true;
    bool hmac_erased = true;

    if (!files->bootsys) {
        return RASPI_SECURE_BOOTSYS_MISSING;
    }
    if (!s->bootsys_trusted_sha256) {
        return RASPI_SECURE_BOOTSYS_TRUST_MISSING;
    }
    if (files->bootsys_size < RASPI4_BOOTSYS_TRAILER_SIZE) {
        return RASPI_SECURE_BOOTSYS_FORMAT;
    }
    trailer = files->bootsys + files->bootsys_size -
              RASPI4_BOOTSYS_TRAILER_SIZE;
    payload_size = ldl_le_p(trailer);
    key_index = ldl_le_p(trailer + sizeof(uint32_t));
    s->bootsys_key_index = key_index;
    for (unsigned int i = 0; i < RASPI4_BOOTSYS_RSA_SIZE; i++) {
        signature_erased &= trailer[sizeof(uint32_t) * 2 + i] == UINT8_MAX;
    }
    for (unsigned int i = 0; i < RASPI4_BOOTSYS_HMAC_SIZE; i++) {
        hmac_erased &= trailer[sizeof(uint32_t) * 2 +
                               RASPI4_BOOTSYS_RSA_SIZE + i] == UINT8_MAX;
    }
    if (payload_size != files->bootsys_size - RASPI4_BOOTSYS_TRAILER_SIZE ||
        key_index > 4 ||
        buffer_is_zero(trailer + sizeof(uint32_t) * 2,
                       RASPI4_BOOTSYS_RSA_SIZE) ||
        signature_erased ||
        buffer_is_zero(trailer + sizeof(uint32_t) * 2 +
                       RASPI4_BOOTSYS_RSA_SIZE,
                       RASPI4_BOOTSYS_HMAC_SIZE) ||
        hmac_erased) {
        return RASPI_SECURE_BOOTSYS_FORMAT;
    }
    /*
     * Public BCM2711 images before secure boot used ROM key index zero.
     * Current production images use key index one.  Recovery's
     * revoke_devkey policy burns row 55 bit 7 specifically to prevent those
     * old key-zero second stages from bypassing secure boot.
     */
    if (key_index == 0 &&
        (s->otp_secure_boot_flags &
         BCM2711_OTP_SECURE_BOOT_FLAGS_REVOKE_DEVKEY)) {
        return RASPI_SECURE_BOOTSYS_DEVKEY_REVOKED;
    }
    bootsys_hash = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, files->bootsys, files->bootsys_size);
    pstrcpy(s->bootsys_sha256, sizeof(s->bootsys_sha256), bootsys_hash);
    if (strcmp(bootsys_hash, s->bootsys_trusted_sha256)) {
        return RASPI_SECURE_BOOTSYS_HASH_MISMATCH;
    }
    result = raspi4_secure_dependencies_verify(s, files, payload_size);
    return result;
}

RaspiSecureResult raspi4_secure_eeprom_verify(
    Raspi4bMachineState *s, const Raspi4EepromFiles *files)
{
    uint8_t expected_hash[RASPI_SECURE_KEY_HASH_SIZE];
    RaspiSecureResult result;

    result = raspi4_secure_bootsys_verify(s, files);
    if (result != RASPI_SECURE_OK) {
        return result;
    }
    if (!files->bootconf || !files->bootconf_signature ||
        !files->public_key ||
        files->public_key_size != RASPI_SECURE_PUBLIC_KEY_SIZE) {
        return RASPI_SECURE_SIGNATURE_FORMAT;
    }
    for (unsigned int i = 0; i < BCM2711_OTP_CUSTOMER_KEY_HASH_LEN; i++) {
        stl_le_p(expected_hash + i * sizeof(uint32_t),
                 bcm2835_otp_get_row(
                     raspi4_otp(s),
                     BCM2711_OTP_CUSTOMER_KEY_HASH_ROW + i));
    }
    result = raspi_secure_check_public_key(files->public_key,
                                           expected_hash, NULL);
    if (result != RASPI_SECURE_OK) {
        return result;
    }
    result = raspi_secure_verify(
        files->public_key, files->bootconf, files->bootconf_size,
        files->bootconf_signature, files->bootconf_signature_size, NULL);
    if (result == RASPI_SECURE_OK) {
        memcpy(s->secure_public_key, files->public_key,
               sizeof(s->secure_public_key));
        s->secure_public_key_valid = true;
    }
    return result;
}

const uint8_t *raspi4_eeprom_find_string(const uint8_t *image,
                                                size_t image_size,
                                                const char *prefix)
{
    size_t prefix_size = strlen(prefix);

    for (size_t offset = 0; offset + prefix_size < image_size; offset++) {
        if ((offset == 0 || !g_ascii_isprint(image[offset - 1])) &&
            !memcmp(image + offset, prefix, prefix_size)) {
            return image + offset + prefix_size;
        }
    }
    return NULL;
}

bool raspi4b_get_eeprom_write_protect(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->eeprom_write_protect;
}

void raspi4b_set_eeprom_write_protect(Object *obj, bool value,
                                             Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    bool bridge_driven;

    if (s->boot_vmstate_registered &&
        !raspi4_eeprom_nwp_live(s, &bridge_driven) &&
        value != s->eeprom_write_protect) {
        error_setg(errp, "EEPROM_nWP is low; write-protect status is locked");
        return;
    }
    raspi4_eeprom_status_store(s, value, errp);
}

bool raspi4b_get_eeprom_nwp(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    bool bridge_driven;

    return raspi4_eeprom_nwp_live(s, &bridge_driven);
}

void raspi4b_set_eeprom_nwp(Object *obj, bool value, Error **errp)
{
    RASPI4B_MACHINE(obj)->eeprom_nwp = value;
}

bool raspi4b_get_eeprom_nwp_sampled(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->eeprom_nwp_sampled;
}

char *raspi4b_get_eeprom_nwp_source(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    bool bridge_driven;

    raspi4_eeprom_nwp_live(s, &bridge_driven);
    return g_strdup(bridge_driven ? "gpio-bridge" : "machine-property");
}

void raspi4b_get_eeprom_stuck_zero_offset(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint64_t value = RASPI4B_MACHINE(obj)->eeprom_stuck_zero_offset;

    visit_type_uint64(v, name, &value, errp);
}

void raspi4b_set_eeprom_stuck_zero_offset(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint64_t value;

    if (!visit_type_uint64(v, name, &value, errp)) {
        return;
    }
    if (value != UINT64_MAX && value >= RASPI4_EEPROM_SIZE) {
        error_setg(errp, "eeprom-stuck-zero-offset must be below 524288 or "
                   "UINT64_MAX");
        return;
    }
    RASPI4B_MACHINE(obj)->eeprom_stuck_zero_offset = value;
}

void raspi4b_get_eeprom_stuck_zero_mask(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint8_t value = RASPI4B_MACHINE(obj)->eeprom_stuck_zero_mask;

    visit_type_uint8(v, name, &value, errp);
}

void raspi4b_set_eeprom_stuck_zero_mask(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint8_t value;

    if (visit_type_uint8(v, name, &value, errp)) {
        RASPI4B_MACHINE(obj)->eeprom_stuck_zero_mask = value;
    }
}

void raspi4b_get_eeprom_fail_after(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    uint64_t value = RASPI4B_MACHINE(obj)->eeprom_fail_after;

    visit_type_uint64(v, name, &value, errp);
}

void raspi4b_get_otp_provision_fail_after(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint8_t value = RASPI4B_MACHINE(obj)->otp_provision_fail_after;

    visit_type_uint8(v, name, &value, errp);
}

void raspi4b_get_otp_provision_rows_programmed(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint8_t value = RASPI4B_MACHINE(obj)->otp_provision_rows_programmed;

    visit_type_uint8(v, name, &value, errp);
}

void raspi4b_get_eeprom_erased_bytes(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    uint64_t value = RASPI4B_MACHINE(obj)->eeprom_erased_bytes;

    visit_type_uint64(v, name, &value, errp);
}

void raspi4b_get_eeprom_programmed_bytes(Object *obj, Visitor *v,
                                                const char *name,
                                                void *opaque, Error **errp)
{
    uint64_t value = RASPI4B_MACHINE(obj)->eeprom_programmed_bytes;

    visit_type_uint64(v, name, &value, errp);
}

void raspi4b_get_eeprom_verified_bytes(Object *obj, Visitor *v,
                                              const char *name, void *opaque,
                                              Error **errp)
{
    uint64_t value = RASPI4B_MACHINE(obj)->eeprom_verified_bytes;

    visit_type_uint64(v, name, &value, errp);
}

void raspi4b_get_eeprom_dirty_sector_count(Object *obj, Visitor *v,
                                                  const char *name,
                                                  void *opaque, Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->eeprom_dirty_sector_count;

    visit_type_uint32(v, name, &value, errp);
}

void raspi4b_get_eeprom_program_page_count(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->eeprom_program_page_count;

    visit_type_uint32(v, name, &value, errp);
}

void raspi4b_get_eeprom_nor_violation_bits(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->eeprom_nor_violation_bits;

    visit_type_uint32(v, name, &value, errp);
}

char *raspi4b_get_eeprom_flash_stage(Object *obj, Error **errp)
{
    static const char *const names[] = {
        [RASPI4_EEPROM_FLASH_IDLE] = "idle",
        [RASPI4_EEPROM_FLASH_ERASE] = "erase",
        [RASPI4_EEPROM_FLASH_PROGRAM] = "program",
        [RASPI4_EEPROM_FLASH_VERIFY] = "verify",
        [RASPI4_EEPROM_FLASH_COMPLETE] = "complete",
    };
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->eeprom_flash_stage < ARRAY_SIZE(names) ?
                    names[s->eeprom_flash_stage] : "invalid");
}

void raspi4b_set_eeprom_fail_after(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    uint64_t value;

    if (visit_type_uint64(v, name, &value, errp)) {
        RASPI4B_MACHINE(obj)->eeprom_fail_after = value;
    }
}

void raspi4b_get_eeprom_erase_delay(Object *obj, Visitor *v,
                                           const char *name, void *opaque,
                                           Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->eeprom_erase_delay_us;

    visit_type_uint32(v, name, &value, errp);
}

void raspi4b_set_eeprom_erase_delay(Object *obj, Visitor *v,
                                           const char *name, void *opaque,
                                           Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (s->pending_boot_action == RASPI4_PENDING_EEPROM_FLASH) {
        error_setg(errp, "cannot change EEPROM timing during active flash");
        return;
    }
    s->eeprom_erase_delay_us = value;
}

void raspi4b_get_eeprom_program_delay(Object *obj, Visitor *v,
                                             const char *name, void *opaque,
                                             Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->eeprom_program_delay_us;

    visit_type_uint32(v, name, &value, errp);
}

void raspi4b_set_eeprom_program_delay(Object *obj, Visitor *v,
                                             const char *name, void *opaque,
                                             Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (s->pending_boot_action == RASPI4_PENDING_EEPROM_FLASH) {
        error_setg(errp, "cannot change EEPROM timing during active flash");
        return;
    }
    s->eeprom_program_delay_us = value;
}

void raspi4b_get_eeprom_verify_delay(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->eeprom_verify_delay_us;

    visit_type_uint32(v, name, &value, errp);
}

void raspi4b_set_eeprom_verify_delay(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (s->pending_boot_action == RASPI4_PENDING_EEPROM_FLASH) {
        error_setg(errp, "cannot change EEPROM timing during active flash");
        return;
    }
    s->eeprom_verify_delay_us = value;
}

void raspi4b_get_eeprom_flash_elapsed(Object *obj, Visitor *v,
                                             const char *name, void *opaque,
                                             Error **errp)
{
    uint64_t value = RASPI4B_MACHINE(obj)->eeprom_flash_elapsed_us;

    visit_type_uint64(v, name, &value, errp);
}

static const char *raspi4_eeprom_fail_stage_name(Raspi4EepromFailStage stage)
{
    static const char *const names[] = {
        [RASPI4_EEPROM_FAIL_ERASE] = "erase",
        [RASPI4_EEPROM_FAIL_PROGRAM] = "program",
        [RASPI4_EEPROM_FAIL_VERIFY] = "verify",
        [RASPI4_EEPROM_FAIL_VERIFY_MISMATCH] = "verify-mismatch",
        [RASPI4_EEPROM_FAIL_RENAME] = "rename",
        [RASPI4_EEPROM_FAIL_REBOOT] = "reboot",
    };

    return names[stage];
}

char *raspi4b_get_eeprom_fail_stage(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(raspi4_eeprom_fail_stage_name(s->eeprom_fail_stage));
}

void raspi4b_set_eeprom_fail_stage(Object *obj, const char *value,
                                          Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    for (Raspi4EepromFailStage stage = RASPI4_EEPROM_FAIL_ERASE;
         stage <= RASPI4_EEPROM_FAIL_REBOOT; stage++) {
        if (!strcmp(value, raspi4_eeprom_fail_stage_name(stage))) {
            s->eeprom_fail_stage = stage;
            return;
        }
    }
    error_setg(errp, "eeprom-fail-stage must be erase, program, verify, "
               "verify-mismatch, rename, or reboot");
}

char *raspi4b_get_eeprom_drive(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->eeprom_drive ? s->eeprom_drive : "");
}

char *raspi4b_get_eeprom_status_drive(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->eeprom_status_drive ? s->eeprom_status_drive : "");
}

char *raspi4b_get_recovery_trusted_sha256(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->recovery_trusted_sha256 ?
                    s->recovery_trusted_sha256 : "");
}

char *raspi4b_get_otp_drive(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->otp_drive ? s->otp_drive : "");
}

void raspi4b_get_otp_rpiboot_gpio(Object *obj, Visitor *v,
                                         const char *name, void *opaque,
                                         Error **errp)
{
    uint8_t value = RASPI4B_MACHINE(obj)->otp_rpiboot_gpio;

    visit_type_uint8(v, name, &value, errp);
}

void raspi4b_set_eeprom_drive(Object *obj, const char *value,
                                     Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    g_free(s->eeprom_drive);
    s->eeprom_drive = value[0] ? g_strdup(value) : NULL;
}

void raspi4b_set_eeprom_status_drive(Object *obj, const char *value,
                                            Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    g_free(s->eeprom_status_drive);
    s->eeprom_status_drive = value[0] ? g_strdup(value) : NULL;
}

char *raspi4b_get_recovery_status(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->recovery_status ? s->recovery_status : "none");
}

char *raspi4b_get_recovery_sha256(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->recovery_sha256[0] ?
                    (char *)s->recovery_sha256 : "none");
}

void raspi4b_get_recovery_key_index(Object *obj, Visitor *v,
                                           const char *name, void *opaque,
                                           Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->recovery_key_index;

    visit_type_uint32(v, name, &value, errp);
}

void raspi4b_get_otp_bootmode(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->otp_bootmode;

    visit_type_uint32(v, name, &value, errp);
}

void raspi4b_get_otp_secure_boot_flags(Object *obj, Visitor *v,
                                              const char *name, void *opaque,
                                              Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    uint32_t value = bcm2835_otp_get_row(
        raspi4_otp(s), BCM2711_OTP_SECURE_BOOT_FLAGS_ROW);

    visit_type_uint32(v, name, &value, errp);
}

void raspi4b_get_otp_board_revision(Object *obj, Visitor *v,
                                           const char *name, void *opaque,
                                           Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->otp_board_revision;

    visit_type_uint32(v, name, &value, errp);
}

bool raspi4b_get_eeprom_build_timestamp_valid(Object *obj,
                                                     Error **errp)
{
    return RASPI4B_MACHINE(obj)->eeprom_build_timestamp_valid;
}

void raspi4b_get_eeprom_build_timestamp(Object *obj, Visitor *v,
                                               const char *name,
                                               void *opaque, Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->eeprom_build_timestamp;

    visit_type_uint32(v, name, &value, errp);
}

bool raspi4b_get_eeprom_update_timestamp_valid(Object *obj,
                                                      Error **errp)
{
    return RASPI4B_MACHINE(obj)->eeprom_update_timestamp_valid;
}

void raspi4b_get_eeprom_update_timestamp(Object *obj, Visitor *v,
                                                const char *name,
                                                void *opaque, Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->eeprom_update_timestamp;

    visit_type_uint32(v, name, &value, errp);
}

bool raspi4b_get_eeprom_capabilities_valid(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->eeprom_capabilities_valid;
}

void raspi4b_get_eeprom_capabilities(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->eeprom_capabilities;

    visit_type_uint32(v, name, &value, errp);
}

char *raspi4b_get_eeprom_version(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->eeprom_version[0] ?
                    (const char *)s->eeprom_version : "none");
}

void raspi4b_get_eeprom_config_append_size(Object *obj, Visitor *v,
                                                  const char *name,
                                                  void *opaque,
                                                  Error **errp)
{
    uint32_t value =
        RASPI4B_MACHINE(obj)->eeprom_config_append_size;

    visit_type_uint32(v, name, &value, errp);
}

char *raspi4b_get_eeprom_config_append_sha256(Object *obj,
                                                     Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    if (!s->eeprom_config_append_size) {
        return g_strdup("none");
    }
    return g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, s->eeprom_config_append,
        s->eeprom_config_append_size);
}

bool raspi4b_get_otp_secure_boot(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->otp_secure_boot;
}
