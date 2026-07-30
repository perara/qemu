/*
 * Raspberry Pi 4 network boot: DHCP, DNS, TFTP and netconsole
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/arm/raspi4b-internal.h"

bool raspi4_parse_mac_address(const char *value, MACAddr *mac)
{
    if (strlen(value) != 17) {
        return false;
    }
    for (unsigned int i = 0; i < 6; i++) {
        int high = g_ascii_xdigit_value(value[i * 3]);
        int low = g_ascii_xdigit_value(value[i * 3 + 1]);

        if (high < 0 || low < 0 || (i != 5 && value[i * 3 + 2] != ':')) {
            return false;
        }
        mac->a[i] = (high << 4) | low;
    }
    return raspi4_mac_address_valid(mac);
}

bool raspi4_parse_mac_address_otp(Raspi4bMachineState *s,
                                         const char *value, MACAddr *mac)
{
    uint32_t first;
    uint32_t second;

    if (strlen(value) != 3 || value[1] != ',' ||
        value[0] < '0' || value[0] > '7' ||
        value[2] < '0' || value[2] > '7' ||
        value[0] == value[2]) {
        return false;
    }
    first = bcm2835_otp_get_row(
        raspi4_otp(s), BCM2835_OTP_CUSTOMER_OTP + value[0] - '0');
    second = bcm2835_otp_get_row(
        raspi4_otp(s), BCM2835_OTP_CUSTOMER_OTP + value[2] - '0');
    mac->a[0] = second >> 24;
    mac->a[1] = second >> 16;
    mac->a[2] = second >> 8;
    mac->a[3] = second;
    mac->a[4] = first >> 24;
    mac->a[5] = first >> 16;
    return raspi4_mac_address_valid(mac);
}

bool raspi4_parse_ipv4(const char *text, uint32_t *value)
{
    struct in_addr address;
    uint32_t parsed;

    if (inet_pton(AF_INET, text, &address) != 1) {
        return false;
    }
    parsed = ntohl(address.s_addr);
    if (!raspi4_ipv4_is_unicast(parsed)) {
        return false;
    }
    *value = parsed;
    return true;
}

static bool raspi4_parse_netconsole_port(const char *text,
                                         uint16_t default_port,
                                         uint16_t *value)
{
    uint64_t parsed;

    if (!text[0]) {
        *value = default_port;
        return true;
    }
    if (qemu_strtou64(text, NULL, 10, &parsed) < 0 || !parsed ||
        parsed > UINT16_MAX) {
        return false;
    }
    *value = parsed;
    return true;
}

static bool raspi4_parse_netconsole_mac(const char *text, uint8_t mac[6])
{
    if (!text[0]) {
        memset(mac, 0, 6);
        return true;
    }
    if (strlen(text) != 17) {
        return false;
    }
    for (unsigned int i = 0; i < 6; i++) {
        int high = g_ascii_xdigit_value(text[i * 3]);
        int low = g_ascii_xdigit_value(text[i * 3 + 1]);

        if (high < 0 || low < 0 ||
            (i != 5 && text[i * 3 + 2] != ':')) {
            return false;
        }
        mac[i] = (high << 4) | low;
    }
    return true;
}

bool raspi4_parse_netconsole(const char *text,
                                    Raspi4BootConfig *config)
{
    char value[RASPI4_NETCONSOLE_MAX + 1];
    char *comma;
    char *source_at;
    char *source_slash;
    char *destination_at;
    char *destination_slash;
    struct in_addr destination_address;
    size_t length = strlen(text);

    config->netconsole_enabled = false;
    config->netconsole[0] = 0;
    if (!length) {
        return true;
    }
    if (length > RASPI4_NETCONSOLE_MAX) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        if ((unsigned char)text[i] < 0x20 ||
            (unsigned char)text[i] >= 0x7f) {
            return false;
        }
    }
    pstrcpy(value, sizeof(value), text);
    comma = strchr(value, ',');
    if (!comma || strchr(comma + 1, ',')) {
        return false;
    }
    *comma++ = 0;
    source_at = strchr(value, '@');
    source_slash = strchr(value, '/');
    destination_at = strchr(comma, '@');
    destination_slash = strchr(comma, '/');
    if (!source_at || !source_slash || source_at > source_slash ||
        strchr(source_at + 1, '@') || strchr(source_slash + 1, '/') ||
        !destination_at || !destination_slash ||
        destination_at > destination_slash ||
        strchr(destination_at + 1, '@') ||
        strchr(destination_slash + 1, '/')) {
        return false;
    }
    *source_at++ = 0;
    *source_slash++ = 0;
    *destination_at++ = 0;
    *destination_slash++ = 0;
    if (!raspi4_parse_netconsole_port(
            value, RASPI4_NETCONSOLE_SOURCE_PORT,
            &config->netconsole_source_port) ||
        !raspi4_parse_ipv4(source_at, &config->netconsole_source_ip) ||
        !raspi4_parse_netconsole_port(
            comma, RASPI4_NETCONSOLE_DESTINATION_PORT,
            &config->netconsole_destination_port) ||
        !raspi4_parse_netconsole_mac(
            destination_slash, config->netconsole_destination_mac)) {
        return false;
    }
    if (!destination_at[0]) {
        config->netconsole_destination_ip = UINT32_MAX;
    } else if (inet_pton(AF_INET, destination_at,
                         &destination_address) != 1) {
        return false;
    } else {
        config->netconsole_destination_ip =
            ntohl(destination_address.s_addr);
    }
    pstrcpy(config->netconsole, sizeof(config->netconsole), text);
    config->netconsole_enabled = true;
    return true;
}

bool raspi4_parse_netmask(const char *text, uint32_t *value)
{
    struct in_addr address;
    uint32_t mask;
    uint32_t inverse;

    if (inet_pton(AF_INET, text, &address) != 1) {
        return false;
    }
    mask = ntohl(address.s_addr);
    inverse = ~mask;
    if (!mask || (inverse & (inverse + 1))) {
        return false;
    }
    *value = mask;
    return true;
}

static bool raspi4_lz4_read_length(const uint8_t **source,
                                   const uint8_t *source_end,
                                   size_t *length)
{
    uint8_t value;

    do {
        if (*source == source_end) {
            return false;
        }
        value = *(*source)++;
        if (*length > RASPI4_EEPROM_DEPENDENCY_MAX_UNCOMPRESSED - value) {
            return false;
        }
        *length += value;
    } while (value == UINT8_MAX);
    return true;
}

static bool raspi4_lz4_block_decode(const uint8_t *source,
                                    size_t source_size,
                                    uint8_t *output,
                                    size_t output_capacity,
                                    size_t output_offset,
                                    bool independent,
                                    size_t *decoded_size)
{
    const uint8_t *source_end = source + source_size;
    size_t history_start = independent ? output_offset : 0;
    size_t produced = output_offset;

    while (source < source_end) {
        uint8_t token = *source++;
        size_t literal_size = token >> 4;
        size_t match_size = token & 0xf;
        size_t offset;

        if (literal_size == 15 &&
            !raspi4_lz4_read_length(&source, source_end, &literal_size)) {
            return false;
        }
        if (literal_size > (size_t)(source_end - source) ||
            literal_size > output_capacity - produced) {
            return false;
        }
        memcpy(output + produced, source, literal_size);
        source += literal_size;
        produced += literal_size;
        if (source == source_end) {
            *decoded_size = produced - output_offset;
            return true;
        }
        if (source_end - source < 2) {
            return false;
        }
        offset = lduw_le_p(source);
        source += 2;
        if (!offset || offset > produced - history_start) {
            return false;
        }
        if (match_size == 15 &&
            !raspi4_lz4_read_length(&source, source_end, &match_size)) {
            return false;
        }
        if (match_size > SIZE_MAX - 4) {
            return false;
        }
        match_size += 4;
        if (match_size > output_capacity - produced) {
            return false;
        }
        for (size_t i = 0; i < match_size; i++) {
            output[produced + i] = output[produced - offset + i];
        }
        produced += match_size;
    }
    return false;
}

GByteArray *raspi4_lz4_frame_decode(const uint8_t *frame,
                                           size_t frame_size)
{
    g_autoptr(GByteArray) output = NULL;
    uint64_t expected_size;
    uint32_t block_maximum;
    uint8_t flags;
    uint8_t descriptor;
    bool independent;
    size_t produced = 0;
    size_t offset;

    if (frame_size < 15 || ldl_le_p(frame) != RASPI4_LZ4_FRAME_MAGIC) {
        return NULL;
    }
    flags = frame[4];
    descriptor = frame[5];
    if ((flags & 0xc0) != 0x40 ||
        !(flags & BIT(3)) || (flags & (BIT(4) | BIT(2) | BIT(1) | BIT(0))) ||
        (descriptor & 0x8f) || (descriptor >> 4) < 4 ||
        (descriptor >> 4) > 7) {
        return NULL;
    }
    if ((uint8_t)(raspi4_xxh32_short(frame + 4, 10) >> 8) != frame[14]) {
        return NULL;
    }
    independent = flags & BIT(5);
    block_maximum = 64 * KiB << (2 * ((descriptor >> 4) - 4));
    expected_size = ldq_le_p(frame + 6);
    if (expected_size > RASPI4_EEPROM_DEPENDENCY_MAX_UNCOMPRESSED) {
        return NULL;
    }
    /*
     * The signed dependency digest below authenticates decoded bytes.  The
     * frame checksum above additionally preserves the LZ4 rejection boundary;
     * optional checksum/dictionary modes remain explicitly unsupported.
     */
    offset = 15;
    output = g_byte_array_sized_new(expected_size);
    g_byte_array_set_size(output, expected_size);
    while (offset + sizeof(uint32_t) <= frame_size) {
        uint32_t block_header = ldl_le_p(frame + offset);
        bool uncompressed = block_header & BIT(31);
        size_t block_size = block_header & ~BIT(31);
        size_t decoded_size;

        offset += sizeof(uint32_t);
        if (!block_header) {
            if (offset != frame_size || produced != expected_size) {
                return NULL;
            }
            return g_steal_pointer(&output);
        }
        if (block_size > block_maximum ||
            block_size > frame_size - offset) {
            return NULL;
        }
        if (uncompressed) {
            if (block_size > expected_size - produced) {
                return NULL;
            }
            memcpy(output->data + produced, frame + offset, block_size);
            decoded_size = block_size;
        } else {
            if (!raspi4_lz4_block_decode(frame + offset, block_size,
                                         output->data, expected_size,
                                         produced, independent,
                                         &decoded_size)) {
                return NULL;
            }
        }
        produced += decoded_size;
        offset += block_size;
    }
    return NULL;
}

void raspi4_net_install_request_from(Raspi4bMachineState *s,
                                            const char *trigger)
{
    timer_del(s->boot_timeout_timer);
    s->pending_boot_action = RASPI4_PENDING_NONE;
    s->net_install_override = true;
    raspi4_set_boot_observation(s, "net-install-requested", trigger);
    if (raspi4_execute_boot_source(s, 0x7, 0)) {
        return;
    }
    s->net_install_override = false;
    raspi4_execute_boot_order(s);
}

void raspi4_net_install_request(Raspi4bMachineState *s)
{
    raspi4_net_install_request_from(s, "keyboard");
}


static void raspi4_network_http_fail(Raspi4bMachineState *s,
                                     const char *state);
static bool raspi4_network_http_plain_append(Raspi4bMachineState *s,
                                             const uint8_t *payload,
                                             size_t payload_size, bool fin);

static uint8_t *raspi4_network_read_artifact(
    void *opaque, const char *path, size_t maximum, size_t *length,
    RaspiFatResult *result, Error **errp)
{
    Raspi4bMachineState *s = opaque;

    for (unsigned int i = 0; i < s->network_artifact_count; i++) {
        const Raspi4NetworkArtifact *artifact = &s->network_artifacts[i];

        if (!strcmp(raspi4_network_artifact_filename(artifact), path)) {
            if (artifact->missing) {
                *result = RASPI_FAT_NOT_FOUND;
                return NULL;
            }
            if (!artifact->data || artifact->size > maximum) {
                *result = RASPI_FAT_ERROR;
                return NULL;
            }
            *length = artifact->size;
            *result = RASPI_FAT_FOUND;
            return g_memdup2(artifact->data, artifact->size);
        }
    }
    *result = RASPI_FAT_NOT_FOUND;
    return NULL;
}

const Raspi4NetworkArtifact *raspi4_network_find_artifact(
    const Raspi4bMachineState *s, Raspi4NetworkArtifactKind kind)
{
    for (unsigned int i = 0; i < s->network_artifact_count; i++) {
        if (s->network_artifacts[i].kind == kind) {
            return &s->network_artifacts[i];
        }
    }
    return NULL;
}

static Raspi4NetworkArtifact *raspi4_network_find_artifact_mutable(
    Raspi4bMachineState *s, Raspi4NetworkArtifactKind kind,
    unsigned int *index)
{
    for (unsigned int i = 0; i < s->network_artifact_count; i++) {
        if (s->network_artifacts[i].kind == kind) {
            if (index) {
                *index = i;
            }
            return &s->network_artifacts[i];
        }
    }
    return NULL;
}

static void raspi4_network_apply_response_fallbacks(
    Raspi4bMachineState *s, RaspiFirmwareConfig *config)
{
    const Raspi4NetworkArtifact *start = raspi4_network_find_artifact(
        s, RASPI4_NETWORK_ARTIFACT_START);

    if (start && !strcmp(raspi4_network_artifact_filename(start),
                         "start.elf")) {
        g_free(config->start_file);
        config->start_file = g_strdup("start.elf");
        g_free(config->fixup_file);
        config->fixup_file = g_strdup("fixup.dat");
    } else if (start &&
               !strcmp(raspi4_network_artifact_filename(start),
                       "start_x.elf")) {
        g_free(config->start_file);
        config->start_file = g_strdup("start_x.elf");
        g_free(config->fixup_file);
        config->fixup_file = g_strdup("fixup_x.dat");
    }
    if (s->network_prefix_fallback) {
        g_free(config->os_prefix);
        config->os_prefix = g_strdup("");
    }
}

Raspi4BootAttemptResult
raspi4_try_network_artifacts(Raspi4bMachineState *s)
{
    const Raspi4NetworkArtifact *start;
    const Raspi4NetworkArtifact *fixup;
    const Raspi4NetworkArtifact *kernel;
    const Raspi4NetworkArtifact *device_tree = NULL;
    const Raspi4NetworkArtifact *cmdline = NULL;
    g_autofree uint8_t *initramfs = NULL;
    unsigned int initramfs_count;
    RaspiFatVolume volume;
    RaspiFirmwareResolveResult result;
    bool have_volume = s->network_boot && blk_is_inserted(s->network_boot) &&
                       raspi_fat_open(&volume, s->network_boot, NULL);

    if (s->otp_secure_boot || s->network_http_default_host) {
        const Raspi4NetworkArtifact *signature =
            raspi4_network_find_artifact(
                s, RASPI4_NETWORK_ARTIFACT_SECURE_SIGNATURE);
        const Raspi4NetworkArtifact *image = raspi4_network_find_artifact(
            s, RASPI4_NETWORK_ARTIFACT_SECURE_IMAGE);

        if ((!s->network_http_default_host &&
             !s->secure_public_key_valid) ||
            !signature || !image ||
            signature->missing || image->missing ||
            !signature->data || !image->data) {
            raspi4_set_firmware_status(s, "secure-boot-image-missing");
            raspi4_set_secure_boot_status(s, "image-missing");
            return RASPI4_BOOT_ATTEMPT_FAILED;
        }
        return raspi4_try_signed_image_with_key(
            s, s->network_http_default_host ?
                   raspi_secure_default_network_key :
                   s->secure_public_key,
            image->data, image->size, signature->data, signature->size,
            s->network_http_default_host ? "https" : "network");
    }
    if ((!have_volume && !s->network_response_discovery) ||
        s->network_artifact_index != s->network_artifact_count) {
        raspi4_set_firmware_status(s, "network-artifacts-incomplete");
        return RASPI4_BOOT_ATTEMPT_FAILED;
    }
    if (!raspi_firmware_config_load_with_reader_named(
            &s->firmware_config, raspi4_network_read_artifact, s,
            raspi4_config_path(s), NULL)) {
        raspi4_set_firmware_status(s, "config-invalid");
        return RASPI4_BOOT_ATTEMPT_FAILED;
    }
    if (s->network_response_discovery) {
        raspi4_network_apply_response_fallbacks(
            s, &s->firmware_config);
    }
    raspi4_apply_firmware_gpu_mem(s);
    raspi4_firmware_uart_begin(s);
    if (raspi4_schedule_bootcode_delay(
            s, s->network_http_mode ? "http" : "network")) {
        return RASPI4_BOOT_ATTEMPT_PENDING;
    }
    if (have_volume) {
        result = raspi_firmware_resolve(
            &volume, &s->firmware_config, &s->firmware_manifest, NULL);
        if (result != RASPI_FIRMWARE_READY) {
            raspi4_set_firmware_status(s, "network-manifest-invalid");
            return RASPI4_BOOT_ATTEMPT_FAILED;
        }
    } else {
        memset(&s->firmware_manifest, 0, sizeof(s->firmware_manifest));
    }
    raspi4_firmware_uart_manifest(
        s, s->network_http_mode ? "http" : "network");

    start = raspi4_network_find_artifact(
        s, RASPI4_NETWORK_ARTIFACT_START);
    fixup = raspi4_network_find_artifact(
        s, RASPI4_NETWORK_ARTIFACT_FIXUP);
    kernel = raspi4_network_find_artifact(
        s, RASPI4_NETWORK_ARTIFACT_KERNEL);
    if (s->firmware_config.device_tree_enabled) {
        device_tree = raspi4_network_find_artifact(
            s, RASPI4_NETWORK_ARTIFACT_DTB);
    }
    cmdline = raspi4_network_find_artifact(
        s, RASPI4_NETWORK_ARTIFACT_CMDLINE);
    if (cmdline && cmdline->missing) {
        cmdline = NULL;
    }
    initramfs_count = raspi4_initramfs_file_count(
        s->firmware_config.initramfs_file);
    if (initramfs_count) {
        initramfs = raspi4_concat_network_initramfs(
            s, initramfs_count, &s->initramfs_size,
            &s->initramfs_sha256);
    }
    if (!start || start->missing || !fixup || fixup->missing || !kernel ||
        kernel->missing || (s->firmware_config.device_tree_enabled &&
                            (!device_tree || device_tree->missing)) ||
        (initramfs_count && !initramfs)) {
        raspi4_set_firmware_status(s, "network-manifest-mismatch");
        return RASPI4_BOOT_ATTEMPT_FAILED;
    }
    raspi4_observe_network_artifact(
        start, &s->firmware_size, &s->firmware_sha256);
    raspi4_observe_network_artifact(
        fixup, &s->fixup_size, &s->fixup_sha256);
    raspi4_observe_network_artifact(
        kernel, &s->kernel_size, &s->kernel_sha256);
    if (device_tree) {
        raspi4_observe_network_artifact(
            device_tree, &s->device_tree_size, &s->device_tree_sha256);
    }
    if (cmdline) {
        raspi4_observe_network_artifact(
            cmdline, &s->cmdline_size, &s->cmdline_sha256);
    }
    return raspi4_finish_firmware(
        s, have_volume ? &volume : NULL, "network", kernel->data,
        device_tree ? device_tree->data : NULL,
        cmdline ? cmdline->data : NULL,
        initramfs,
        raspi4_network_read_artifact, s);
}

Raspi4BootAttemptResult
raspi4_try_sd_firmware(Raspi4bMachineState *s)
{
    return raspi4_try_firmware(s, s->sd, raspi4_boot_media_name(s));
}


static bool raspi4_network_begin_dns(Raspi4bMachineState *s);

void raspi4_network_artifacts_clear(Raspi4bMachineState *s)
{
    for (unsigned int i = 0; i < RASPI4_NETWORK_ARTIFACTS_MAX; i++) {
        Raspi4NetworkArtifact *artifact = &s->network_artifacts[i];

        g_clear_pointer(&artifact->data, g_free);
        g_clear_pointer(&artifact->filename, g_free);
        g_clear_pointer(&artifact->expected_sha256, g_free);
        artifact->size = 0;
        artifact->expected_size = 0;
        artifact->maximum_size = 0;
        artifact->kind = RASPI4_NETWORK_ARTIFACT_CONFIG;
        artifact->required = false;
        artifact->missing = false;
        artifact->wire_filename[0] = 0;
    }
    s->network_artifact_count = 0;
    s->network_artifact_index = 0;
    s->network_base_artifact_index = 0;
    s->network_overlay_artifact_index = 0;
    s->network_response_discovery = false;
    s->network_prefix_fallback = false;
    s->network_tftp_prefix_fallback = false;
    s->network_discovery_phase = RASPI4_NETWORK_DISCOVERY_NONE;
}

const char *raspi4_network_artifact_filename(
    const Raspi4NetworkArtifact *artifact)
{
    return artifact->filename ? artifact->filename :
                                (const char *)artifact->wire_filename;
}

static bool raspi4_network_artifact_set_paths(
    Raspi4bMachineState *s, Raspi4NetworkArtifact *artifact,
    const char *filename)
{
    const char *prefix = s->network_tftp_prefix_fallback ?
                         "" : (const char *)s->tftp_prefix;
    g_autofree char *wire_filename = g_strconcat(
        prefix, filename, NULL);

    if (!filename[0] || strlen(wire_filename) > RASPI4_TFTP_PATH_MAX) {
        return false;
    }
    g_free(artifact->filename);
    artifact->filename = g_strdup(filename);
    pstrcpy((char *)artifact->wire_filename,
            sizeof(artifact->wire_filename), wire_filename);
    return true;
}

static char *raspi4_network_artifact_path(const RaspiFirmwareConfig *config,
                                          const char *filename,
                                          bool prefixed)
{
    if (filename[0] == '/') {
        return g_strdup(filename + 1);
    }
    return prefixed && config->os_prefix[0] ?
        g_strconcat(config->os_prefix, filename, NULL) :
        g_strdup(filename);
}

static bool raspi4_network_artifact_add(
    Raspi4bMachineState *s, RaspiFatVolume *volume, const RaspiFatFile *file,
    const char *filename, size_t maximum, Raspi4NetworkArtifactKind kind,
    bool preserve_data)
{
    Raspi4NetworkArtifact *artifact;
    g_autofree uint8_t *data = NULL;
    g_autofree char *sha256 = NULL;
    size_t length;

    if (s->network_artifact_count >= RASPI4_NETWORK_ARTIFACTS_MAX) {
        return false;
    }
    artifact = &s->network_artifacts[s->network_artifact_count];
    data = raspi_fat_read_file(volume, file, maximum, &length, NULL);
    if (!data || length != file->size || length > UINT32_MAX) {
        return false;
    }
    sha256 = g_compute_checksum_for_data(G_CHECKSUM_SHA256, data, length);
    if (preserve_data &&
        s->network_artifact_count < s->network_artifact_index &&
        (artifact->size != length || !artifact->data)) {
        return false;
    }
    if (preserve_data && artifact->data) {
        g_autofree char *received_sha256 = g_compute_checksum_for_data(
            G_CHECKSUM_SHA256, artifact->data, artifact->size);

        if (strcmp(received_sha256, sha256)) {
            return false;
        }
    }
    if (!raspi4_network_artifact_set_paths(s, artifact, filename)) {
        return false;
    }
    g_free(artifact->expected_sha256);
    artifact->expected_sha256 = g_steal_pointer(&sha256);
    artifact->expected_size = length;
    artifact->maximum_size = maximum;
    artifact->kind = kind;
    artifact->required = true;
    artifact->missing = false;
    s->network_artifact_count++;
    return true;
}


static uint8_t *raspi4_network_discover_artifact(
    void *opaque, const char *path, size_t maximum, size_t *length,
    RaspiFatResult *result, Error **errp)
{
    Raspi4NetworkDiscovery *discovery = opaque;
    RaspiFatFile file;
    uint8_t *data;

    *result = raspi_fat_find_path(discovery->volume, path, &file, errp);
    if (*result != RASPI_FAT_FOUND) {
        return NULL;
    }
    data = raspi_fat_read_file(
        discovery->volume, &file, maximum, length, errp);
    if (!data || *length != file.size) {
        g_free(data);
        *result = RASPI_FAT_ERROR;
        return NULL;
    }
    for (unsigned int i = 0;
         i < discovery->machine->network_artifact_count; i++) {
        const Raspi4NetworkArtifact *artifact =
            &discovery->machine->network_artifacts[i];

        if (artifact->filename && !strcmp(artifact->filename, path)) {
            return data;
        }
    }
    if (!raspi4_network_artifact_add(
            discovery->machine, discovery->volume, &file, path, maximum,
            RASPI4_NETWORK_ARTIFACT_OVERLAY,
            discovery->preserve_data)) {
        g_free(data);
        *result = RASPI_FAT_ERROR;
        return NULL;
    }
    return data;
}

static bool raspi4_network_select_artifact(Raspi4bMachineState *s,
                                           bool reset_transfer)
{
    Raspi4NetworkArtifact *artifact;

    if (s->network_artifact_index >= s->network_artifact_count) {
        return false;
    }
    artifact = &s->network_artifacts[s->network_artifact_index];
    g_free(s->network_tftp_filename);
    s->network_tftp_filename = g_strdup(
        (const char *)artifact->wire_filename);
    g_free(s->network_tftp_expected_sha256);
    s->network_tftp_expected_sha256 = g_strdup(
        artifact->expected_sha256);
    s->network_tftp_expected_size = artifact->expected_sha256 ?
        artifact->expected_size : artifact->maximum_size;
    if (reset_transfer) {
        s->network_tftp_server_port = 0;
        s->network_tftp_next_block = 1;
        s->network_tftp_block_number = 1;
        s->network_tftp_size = 0;
        s->network_tftp_block_size = RASPI4_TFTP_CLASSIC_BLOCK_SIZE;
        s->network_tftp_options_disabled = false;
        s->network_tftp_oack_accepted = false;
        s->network_tftp_tsize_valid = false;
        s->network_tftp_tsize = 0;
        g_clear_pointer(&s->network_tftp_data, g_free);
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
        s->network_http_ooo_fin_sequence = 0;
    }
    return true;
}

static const Raspi4NetworkArtifact *raspi4_network_find_path(
    const Raspi4bMachineState *s, const char *path)
{
    for (unsigned int i = 0; i < s->network_artifact_count; i++) {
        const Raspi4NetworkArtifact *artifact = &s->network_artifacts[i];

        if (!strcmp(raspi4_network_artifact_filename(artifact), path)) {
            return artifact;
        }
    }
    return NULL;
}

static bool raspi4_network_request_add(
    Raspi4bMachineState *s, const char *filename, size_t maximum,
    Raspi4NetworkArtifactKind kind, bool required)
{
    Raspi4NetworkArtifact *artifact;

    if (!filename[0] ||
        strlen(s->network_tftp_prefix_fallback ?
               "" : (const char *)s->tftp_prefix) + strlen(filename) >
            RASPI4_TFTP_PATH_MAX ||
        maximum > UINT32_MAX ||
        s->network_artifact_count >= RASPI4_NETWORK_ARTIFACTS_MAX ||
        raspi4_network_find_path(s, filename)) {
        return false;
    }
    artifact = &s->network_artifacts[s->network_artifact_count++];
    if (!raspi4_network_artifact_set_paths(s, artifact, filename)) {
        s->network_artifact_count--;
        return false;
    }
    artifact->maximum_size = maximum;
    artifact->expected_size = maximum;
    artifact->kind = kind;
    artifact->required = required;
    artifact->missing = false;
    return true;
}


static uint8_t *raspi4_network_probe_artifact(
    void *opaque, const char *path, size_t maximum, size_t *length,
    RaspiFatResult *result, Error **errp)
{
    Raspi4NetworkReadProbe *probe = opaque;
    uint8_t *data = raspi4_network_read_artifact(
        probe->machine, path, maximum, length, result, errp);

    if (!data && *result == RASPI_FAT_NOT_FOUND && !probe->missing_path &&
        !raspi4_network_find_path(probe->machine, path)) {
        probe->missing_path = g_strdup(path);
        probe->missing_maximum = maximum;
    }
    return data;
}

static int raspi4_network_response_parse_config(
    Raspi4bMachineState *s, RaspiFirmwareConfig *config)
{
    raspi_firmware_config_init(config, s->cm4);
    config->bootvar0 = s->bootvar0;
    raspi4_set_firmware_filter_inputs(s, config);
    if (raspi_firmware_config_load_with_reader_named(
            config, raspi4_network_read_artifact, s,
            raspi4_config_path(s), NULL)) {
        raspi4_network_apply_response_fallbacks(s, config);
        return 1;
    }
    for (unsigned int i = 0; i < config->include_files->len; i++) {
        const char *include = g_ptr_array_index(config->include_files, i);

        if (!raspi4_network_find_path(s, include)) {
            bool added = raspi4_network_request_add(
                s, include, RASPI4_CONFIG_MAX_SIZE,
                RASPI4_NETWORK_ARTIFACT_INCLUDE, true);

            raspi_firmware_config_clear(config);
            return added ? 0 : -1;
        }
    }
    raspi_firmware_config_clear(config);
    return -1;
}

static bool raspi4_network_request_boot_root(Raspi4bMachineState *s)
{
    s->network_discovery_phase = RASPI4_NETWORK_DISCOVERY_CONFIG;
    if (s->otp_secure_boot || s->network_http_default_host) {
        if (!raspi4_network_request_add(
                s, s->tryboot ? "tryboot.sig" : "boot.sig",
                RASPI4_SIGNATURE_MAX_SIZE,
                RASPI4_NETWORK_ARTIFACT_SECURE_SIGNATURE, true) ||
            !raspi4_network_request_add(
                s, s->tryboot ? "tryboot.img" : "boot.img",
                RASPI4_INITRAMFS_MAX_SIZE,
                RASPI4_NETWORK_ARTIFACT_SECURE_IMAGE, true)) {
            return false;
        }
    } else if (!raspi4_network_request_add(
                   s, raspi4_config_path(s), RASPI4_CONFIG_MAX_SIZE,
                   RASPI4_NETWORK_ARTIFACT_CONFIG, false)) {
        return false;
    }
    return raspi4_network_select_artifact(s, true);
}

static bool raspi4_network_self_update_enabled(Raspi4bMachineState *s)
{
    s->self_update_status = RASPI4_SELF_UPDATE_NONE;
    if (s->cm4) {
        s->self_update_status = RASPI4_SELF_UPDATE_UNSUPPORTED;
        return false;
    }
    if (!s->enable_self_update) {
        s->self_update_status = RASPI4_SELF_UPDATE_DISABLED;
        return false;
    }
    if (s->freeze_version) {
        s->self_update_status = RASPI4_SELF_UPDATE_FROZEN;
        return false;
    }
    return true;
}

/*
 * Grow the standalone TFTP request list only from bytes already returned by
 * the server.  A true return means either another request is ready or the
 * complete received corpus is ready for handoff.
 */
/*
 * Grow the standalone TFTP request list only from bytes already returned by
 * the server.  A true return means either another request is ready or the
 * complete received corpus is ready for handoff.
 */
static bool raspi4_network_response_advance(Raspi4bMachineState *s)
{
    RaspiFirmwareConfig config;
    RaspiFirmwarePaths paths;
    Raspi4NetworkReadProbe probe = { .machine = s };
    const Raspi4NetworkArtifact *device_tree;
    g_autofree uint8_t *effective_tree = NULL;
    g_autofree uint8_t *hat_eeprom = NULL;
    RaspiOverlayState overlay_state = { 0 };
    size_t effective_tree_size;
    size_t hat_eeprom_size = 0;
    RaspiOverlayResult overlay_result;
    int parsed;
    bool ready = false;

    if (s->network_artifact_index < s->network_artifact_count) {
        return raspi4_network_select_artifact(s, true);
    }
    if (s->network_discovery_phase ==
        RASPI4_NETWORK_DISCOVERY_SELF_UPDATE) {
        const Raspi4NetworkArtifact *update = raspi4_network_find_artifact(
            s, RASPI4_NETWORK_ARTIFACT_EEPROM_UPDATE);
        const Raspi4NetworkArtifact *signature =
            raspi4_network_find_artifact(
                s, RASPI4_NETWORK_ARTIFACT_EEPROM_SIGNATURE);
        Raspi4SelfUpdateResult result;

        if (!update || update->missing) {
            return raspi4_network_request_boot_root(s);
        }
        if (!signature) {
            return raspi4_network_request_add(
                       s, "pieeprom.sig", RASPI4_SIGNATURE_MAX_SIZE,
                       RASPI4_NETWORK_ARTIFACT_EEPROM_SIGNATURE, true) &&
                   raspi4_network_select_artifact(s, true);
        }
        result = raspi4_apply_self_update(
            s, update->data, update->size, signature->data, signature->size,
            "network");
        if (result == RASPI4_SELF_UPDATE_PENDING) {
            return true;
        }
        if (result == RASPI4_SELF_UPDATE_FAILED) {
            return false;
        }
        return raspi4_network_request_boot_root(s);
    }
    if (s->otp_secure_boot || s->network_http_default_host) {
        s->network_discovery_phase = RASPI4_NETWORK_DISCOVERY_READY;
        return true;
    }

    parsed = raspi4_network_response_parse_config(s, &config);
    if (parsed <= 0) {
        return parsed == 0 &&
               s->network_artifact_index < s->network_artifact_count &&
               raspi4_network_select_artifact(s, true);
    }
    if (s->network_discovery_phase == RASPI4_NETWORK_DISCOVERY_CONFIG) {
        if (!raspi_firmware_paths_resolve(&config, &paths, NULL)) {
            goto out_config;
        }
        s->network_base_artifact_index = s->network_artifact_count;
        if (!raspi4_network_request_add(
                s, paths.start, RASPI4_FIRMWARE_MAX_SIZE,
                RASPI4_NETWORK_ARTIFACT_START, true) ||
            !raspi4_network_request_add(
                s, paths.fixup, RASPI4_FIXUP_MAX_SIZE,
                RASPI4_NETWORK_ARTIFACT_FIXUP, true) ||
            !raspi4_network_request_add(
                s, paths.kernel, RASPI4_KERNEL_MAX_SIZE,
                RASPI4_NETWORK_ARTIFACT_KERNEL, true) ||
            (config.device_tree_enabled &&
             !raspi4_network_request_add(
                 s, paths.device_tree, RASPI4_DTB_MAX_SIZE,
                 RASPI4_NETWORK_ARTIFACT_DTB, true)) ||
            !raspi4_network_request_add(
                s, paths.cmdline, RASPI4_CMDLINE_MAX_SIZE,
                RASPI4_NETWORK_ARTIFACT_CMDLINE, false)) {
            raspi_firmware_paths_clear(&paths);
            goto out_config;
        }
        for (unsigned int i = 0; i < paths.initramfs_count; i++) {
            if (!raspi4_network_request_add(
                    s, paths.initramfs[i], RASPI4_INITRAMFS_MAX_SIZE,
                    RASPI4_NETWORK_ARTIFACT_INITRAMFS, true)) {
                raspi_firmware_paths_clear(&paths);
                goto out_config;
            }
        }
        raspi_firmware_paths_clear(&paths);
        s->network_discovery_phase = RASPI4_NETWORK_DISCOVERY_BASE;
        ready = raspi4_network_select_artifact(s, true);
        goto out_config;
    }

    if (s->network_discovery_phase == RASPI4_NETWORK_DISCOVERY_BASE) {
        s->network_overlay_artifact_index = s->network_artifact_count;
        s->network_discovery_phase = RASPI4_NETWORK_DISCOVERY_OVERLAYS;
        if (config.dt_commands->len || s->hat_eeprom) {
            g_autofree char *map = g_strdup_printf(
                "%s%soverlay_map.dtb", config.os_prefix,
                config.overlay_prefix);

            if (!raspi4_network_request_add(
                    s, map, RASPI4_DTB_MAX_SIZE,
                    RASPI4_NETWORK_ARTIFACT_OVERLAY_MAP, false)) {
                goto out_config;
            }
            ready = raspi4_network_select_artifact(s, true);
            goto out_config;
        }
    }

    if (s->network_discovery_phase == RASPI4_NETWORK_DISCOVERY_OVERLAYS) {
        device_tree = raspi4_network_find_artifact(
            s, RASPI4_NETWORK_ARTIFACT_DTB);
        if (!device_tree || !device_tree->data) {
            goto out_config;
        }
        hat_eeprom = raspi4_read_hat_eeprom(s, &hat_eeprom_size);
        if (s->hat_eeprom && !hat_eeprom) {
            goto out_config;
        }
        overlay_result = raspi_overlay_apply_config_with_reader(
            &config, hat_eeprom, hat_eeprom_size,
            device_tree->data, device_tree->size,
            raspi4_network_probe_artifact, &probe, &effective_tree,
            &effective_tree_size, &overlay_state, NULL);
        if (probe.missing_path) {
            ready = raspi4_network_request_add(
                        s, probe.missing_path, probe.missing_maximum,
                        RASPI4_NETWORK_ARTIFACT_OVERLAY, false) &&
                    raspi4_network_select_artifact(s, true);
            goto out_config;
        }
        if (overlay_result != RASPI_OVERLAY_READY) {
            goto out_config;
        }
        s->network_discovery_phase = RASPI4_NETWORK_DISCOVERY_READY;
    }
    ready = s->network_discovery_phase == RASPI4_NETWORK_DISCOVERY_READY;

out_config:
    g_free(probe.missing_path);
    raspi_overlay_state_clear(&overlay_state);
    raspi_firmware_config_clear(&config);
    return ready;
}

static bool raspi4_network_prepare_response_tftp(
    Raspi4bMachineState *s, bool preserve_data)
{
    if (preserve_data) {
        if (!s->network_response_discovery ||
            s->network_discovery_phase >= RASPI4_NETWORK_DISCOVERY__MAX ||
            s->network_artifact_count > RASPI4_NETWORK_ARTIFACTS_MAX ||
            s->network_artifact_index >= s->network_artifact_count) {
            return false;
        }
        for (unsigned int i = 0; i < s->network_artifact_count; i++) {
            Raspi4NetworkArtifact *artifact = &s->network_artifacts[i];
            const char *wire_filename =
                (const char *)artifact->wire_filename;
            const char *prefix = s->network_tftp_prefix_fallback ?
                                 "" : (const char *)s->tftp_prefix;

            if (!artifact->filename) {
                if (!g_str_has_prefix(wire_filename, prefix) ||
                    !wire_filename[strlen(prefix)]) {
                    return false;
                }
                artifact->filename = g_strdup(
                    wire_filename + strlen(prefix));
            }

            if (!artifact->wire_filename[0] ||
                artifact->kind >= RASPI4_NETWORK_ARTIFACT__MAX ||
                artifact->size > artifact->maximum_size ||
                (i < s->network_artifact_index && artifact->required &&
                 (artifact->missing || !artifact->data))) {
                return false;
            }
        }
        return raspi4_network_select_artifact(s, false);
    }
    raspi4_network_artifacts_clear(s);
    s->network_response_discovery = true;
    if (!s->network_http_mode && !s->network_http_default_host &&
        raspi4_network_self_update_enabled(s)) {
        s->network_discovery_phase =
            RASPI4_NETWORK_DISCOVERY_SELF_UPDATE;
        if (!raspi4_network_request_add(
                s, "pieeprom.upd", RASPI4_EEPROM_SIZE,
                RASPI4_NETWORK_ARTIFACT_EEPROM_UPDATE, false)) {
            return false;
        }
        return raspi4_network_select_artifact(s, true);
    }
    return raspi4_network_request_boot_root(s);
}

bool raspi4_network_prepare_tftp(Raspi4bMachineState *s,
                                        bool preserve_data)
{
    RaspiFirmwareConfig config;
    RaspiFirmwareManifest manifest;
    RaspiFatVolume volume;
    RaspiFatFile config_file;
    Raspi4NetworkDiscovery discovery = {
        .machine = s,
        .volume = &volume,
        .preserve_data = preserve_data,
    };
    g_autofree uint8_t *device_tree_data = NULL;
    g_autofree uint8_t *effective_tree = NULL;
    g_autofree uint8_t *hat_eeprom = NULL;
    RaspiOverlayState overlay_state = { 0 };
    size_t device_tree_length;
    size_t effective_tree_size;
    size_t hat_eeprom_size = 0;
    g_autofree char *kernel = NULL;
    g_autofree char *device_tree = NULL;
    g_autofree char *cmdline = NULL;
    uint8_t migrated_count = s->network_artifact_count;
    uint8_t migrated_index = s->network_artifact_index;
    uint8_t migrated_base_index = s->network_base_artifact_index;
    uint8_t migrated_overlay_index = s->network_overlay_artifact_index;
    bool ready = false;

    if (!s->network_boot || !blk_is_inserted(s->network_boot)) {
        return raspi4_network_prepare_response_tftp(s, preserve_data);
    }
    if (!raspi_fat_open(&volume, s->network_boot, NULL)) {
        return false;
    }
    if (!preserve_data) {
        raspi4_network_artifacts_clear(s);
    } else {
        s->network_artifact_count = 0;
    }
    raspi_firmware_config_init(&config, s->cm4);
    config.bootvar0 = s->bootvar0;
    raspi4_set_firmware_filter_inputs(s, &config);
    if (!raspi_firmware_config_load_named(
            &volume, &config, raspi4_config_path(s), NULL) ||
        raspi_firmware_resolve(&volume, &config, &manifest, NULL) !=
            RASPI_FIRMWARE_READY) {
        goto out;
    }
    if (config.config_present) {
        if (raspi_fat_find_path(
                &volume, raspi4_config_path(s), &config_file, NULL) !=
                RASPI_FAT_FOUND ||
            !raspi4_network_artifact_add(
                s, &volume, &config_file, raspi4_config_path(s),
                RASPI4_CONFIG_MAX_SIZE, RASPI4_NETWORK_ARTIFACT_CONFIG,
                preserve_data)) {
            goto out;
        }
    }
    for (unsigned int i = 0; i < config.include_files->len; i++) {
        const char *include = g_ptr_array_index(config.include_files, i);

        if (raspi_fat_find_path(&volume, include, &config_file, NULL) !=
                RASPI_FAT_FOUND ||
            !raspi4_network_artifact_add(
                s, &volume, &config_file, include,
                RASPI4_CONFIG_MAX_SIZE, RASPI4_NETWORK_ARTIFACT_INCLUDE,
                preserve_data)) {
            goto out;
        }
    }
    s->network_base_artifact_index = s->network_artifact_count;
    kernel = raspi4_network_artifact_path(&config, config.kernel_file, true);
    device_tree = raspi4_network_artifact_path(
        &config, config.device_tree_file, true);
    cmdline = raspi4_network_artifact_path(
        &config, config.cmdline_file, true);
    if (!raspi4_network_artifact_add(
            s, &volume, &manifest.start, config.start_file,
            RASPI4_FIRMWARE_MAX_SIZE, RASPI4_NETWORK_ARTIFACT_START,
            preserve_data) ||
        !raspi4_network_artifact_add(
            s, &volume, &manifest.fixup, config.fixup_file,
            RASPI4_FIXUP_MAX_SIZE, RASPI4_NETWORK_ARTIFACT_FIXUP,
            preserve_data) ||
        !raspi4_network_artifact_add(
            s, &volume, &manifest.kernel, kernel,
            RASPI4_KERNEL_MAX_SIZE, RASPI4_NETWORK_ARTIFACT_KERNEL,
            preserve_data) ||
        (config.device_tree_enabled &&
         !raspi4_network_artifact_add(
             s, &volume, &manifest.device_tree, device_tree,
             RASPI4_DTB_MAX_SIZE, RASPI4_NETWORK_ARTIFACT_DTB,
             preserve_data)) ||
        (manifest.cmdline_present &&
         !raspi4_network_artifact_add(
             s, &volume, &manifest.cmdline, cmdline,
             RASPI4_CMDLINE_MAX_SIZE, RASPI4_NETWORK_ARTIFACT_CMDLINE,
             preserve_data))) {
        goto out;
    }
    if (manifest.initramfs_count) {
        g_auto(GStrv) names = g_strsplit(config.initramfs_file, ",", -1);

        for (unsigned int i = 0; i < manifest.initramfs_count; i++) {
            g_autofree char *path = raspi4_network_artifact_path(
                &config, names[i], true);

            if (!path ||
                !raspi4_network_artifact_add(
                    s, &volume, &manifest.initramfs[i], path,
                    RASPI4_INITRAMFS_MAX_SIZE,
                    RASPI4_NETWORK_ARTIFACT_INITRAMFS, preserve_data)) {
                goto out;
            }
        }
    }
    s->network_overlay_artifact_index = s->network_artifact_count;
    if (config.dt_commands->len || s->hat_eeprom) {
        device_tree_data = raspi_fat_read_file(
            &volume, &manifest.device_tree, RASPI4_DTB_MAX_SIZE,
            &device_tree_length, NULL);
        hat_eeprom = raspi4_read_hat_eeprom(s, &hat_eeprom_size);
        if (!device_tree_data || (s->hat_eeprom && !hat_eeprom)) {
            goto out;
        }
        if (raspi_overlay_apply_config_with_reader(
                &config, hat_eeprom, hat_eeprom_size,
                device_tree_data, device_tree_length,
                raspi4_network_discover_artifact, &discovery,
                &effective_tree, &effective_tree_size, &overlay_state,
                NULL) != RASPI_OVERLAY_READY) {
            goto out;
        }
    }
    if (preserve_data) {
        if (s->network_artifact_count != migrated_count ||
            migrated_index >= s->network_artifact_count ||
            s->network_base_artifact_index != migrated_base_index ||
            s->network_overlay_artifact_index != migrated_overlay_index) {
            goto out;
        }
        s->network_artifact_index = migrated_index;
    }
    ready = raspi4_network_select_artifact(s, !preserve_data);
out:
    raspi_overlay_state_clear(&overlay_state);
    raspi_firmware_config_clear(&config);
    return ready;
}

static uint8_t *raspi4_dhcp_option(uint8_t *cursor, uint8_t code,
                                   const void *value, uint8_t length)
{
    *cursor++ = code;
    *cursor++ = length;
    memcpy(cursor, value, length);
    return cursor + length;
}

static bool raspi4_network_send_dhcp(Raspi4bMachineState *s, bool request)
{
    static const uint8_t broadcast[6] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    };
    static const char vendor[] = "PXEClient:Arch:00000:UNDI:002001";
    static const uint8_t parameter_request[] = {
        1, 3, 6, 12, 15, 17, 26, 40, 42, 43, 51, 54, 58, 59, 66, 67,
    };
    BCM2711GenetState *genet = raspi4_genet(s);
    const MACAddr *mac = bcm2711_genet_mac(genet);
    uint8_t packet[576] = { 0 };
    uint8_t *ip = packet + 14;
    uint8_t *udp = ip + 20;
    uint8_t *bootp = udp + 8;
    uint8_t *options = bootp + 240;
    uint8_t machine_id[17] = { 0 };
    uint8_t message = request ? 3 : 1;
    uint32_t serial = bcm2835_otp_get_row(
        raspi4_otp(s), BCM2711_OTP_SERIAL_ROW);
    uint16_t udp_length;
    uint16_t ip_length;
    size_t packet_length;

    memcpy(packet, broadcast, sizeof(broadcast));
    memcpy(packet + 6, mac->a, sizeof(mac->a));
    stw_be_p(packet + 12, 0x0800);

    bootp[0] = 1;
    bootp[1] = 1;
    bootp[2] = 6;
    stl_be_p(bootp + 4, s->network_dhcp_xid);
    stw_be_p(bootp + 10, 0x8000);
    memcpy(bootp + 28, mac->a, sizeof(mac->a));
    stl_be_p(bootp + 236, 0x63825363);
    options = raspi4_dhcp_option(options, 53, &message, sizeof(message));
    options = raspi4_dhcp_option(options, 60, vendor,
                                 sizeof(vendor) - 1);
    if (s->dhcp_option97) {
        stl_le_p(machine_id + 1, s->dhcp_option97);
        stl_le_p(machine_id + 5, s->board_revision);
        memcpy(machine_id + 9, mac->a + 2, 4);
        stl_le_p(machine_id + 13, serial);
    } else {
        for (unsigned int i = 0; i < 4; i++) {
            stl_le_p(machine_id + 1 + i * sizeof(serial), serial);
        }
    }
    options = raspi4_dhcp_option(options, 97, machine_id,
                                 sizeof(machine_id));
    options = raspi4_dhcp_option(options, 55, parameter_request,
                                 sizeof(parameter_request));
    if (request) {
        uint8_t address[4];

        stl_be_p(address, s->network_offered_ip);
        options = raspi4_dhcp_option(options, 50, address, sizeof(address));
        stl_be_p(address, s->network_dhcp_server_ip);
        options = raspi4_dhcp_option(options, 54, address, sizeof(address));
    }
    *options++ = 255;

    udp_length = options - udp;
    stw_be_p(udp, 68);
    stw_be_p(udp + 2, 67);
    stw_be_p(udp + 4, udp_length);
    ip_length = 20 + udp_length;
    ip[0] = 0x45;
    stw_be_p(ip + 2, ip_length);
    stw_be_p(ip + 4, s->network_dhcp_xid & 0xffff);
    ip[8] = 64;
    ip[9] = 17;
    memset(ip + 16, 0xff, 4);
    stw_be_p(ip + 10,
             net_checksum_finish(net_checksum_add(20, ip)));
    packet_length = 14 + ip_length;
    return bcm2711_genet_boot_send(genet, packet, packet_length) ==
           packet_length;
}

static bool raspi4_network_send_udp_from_to(
    Raspi4bMachineState *s, const uint8_t destination_mac[6],
    uint32_t source_ip, uint32_t destination_ip, uint16_t source_port,
    uint16_t destination_port, const uint8_t *payload, size_t payload_size)
{
    BCM2711GenetState *genet = raspi4_genet(s);
    const MACAddr *mac = bcm2711_genet_mac(genet);
    g_autofree uint8_t *packet = NULL;
    uint8_t *ip;
    uint8_t *udp;
    uint16_t udp_length;
    uint16_t ip_length;
    uint16_t udp_checksum;
    size_t packet_length;

    if (payload_size > UINT16_MAX - 28) {
        return false;
    }
    packet_length = 14 + 20 + 8 + payload_size;
    packet = g_malloc0(packet_length);
    ip = packet + 14;
    udp = ip + 20;
    memcpy(packet, destination_mac, 6);
    memcpy(packet + 6, mac->a, sizeof(mac->a));
    stw_be_p(packet + 12, 0x0800);
    udp_length = 8 + payload_size;
    stw_be_p(udp, source_port);
    stw_be_p(udp + 2, destination_port);
    stw_be_p(udp + 4, udp_length);
    if (payload_size) {
        memcpy(udp + 8, payload, payload_size);
    }
    ip_length = 20 + udp_length;
    ip[0] = 0x45;
    stw_be_p(ip + 2, ip_length);
    stw_be_p(ip + 4, s->network_dhcp_xid & 0xffff);
    ip[8] = 64;
    ip[9] = 17;
    stl_be_p(ip + 12, source_ip);
    stl_be_p(ip + 16, destination_ip);
    stw_be_p(ip + 10,
             net_checksum_finish(net_checksum_add(20, ip)));
    udp_checksum = net_checksum_tcpudp(udp_length, 17, ip + 12, udp);
    stw_be_p(udp + 6, udp_checksum ? udp_checksum : 0xffff);
    return bcm2711_genet_boot_send(genet, packet, packet_length) >= 0;
}

static bool raspi4_network_send_udp_to(Raspi4bMachineState *s,
                                       const uint8_t destination_mac[6],
                                       uint32_t destination_ip,
                                       uint16_t source_port,
                                       uint16_t destination_port,
                                       const uint8_t *payload,
                                       size_t payload_size)
{
    return raspi4_network_send_udp_from_to(
        s, destination_mac, s->network_offered_ip, destination_ip,
        source_port, destination_port, payload, payload_size);
}

static void raspi4_netconsole_write(Raspi4bMachineState *s,
                                    const char *text)
{
    size_t length;

    if (!s->netconsole_enabled) {
        return;
    }
    length = strlen(text);
    if (raspi4_network_send_udp_from_to(
            s, s->netconsole_destination_mac,
            s->netconsole_source_ip, s->netconsole_destination_ip,
            s->netconsole_source_port, s->netconsole_destination_port,
            (const uint8_t *)text, length)) {
        s->netconsole_packets++;
        s->netconsole_bytes += length;
    }
}

void raspi4_netconsole_begin(Raspi4bMachineState *s)
{
    g_autofree char *line = NULL;

    if (!s->netconsole_enabled) {
        return;
    }
    line = g_strdup_printf(
        "RPI4-BOOT: netconsole enabled order=0x%08" PRIx32 "\r\n",
        s->boot_order);
    raspi4_netconsole_write(s, line);
}

void raspi4_netconsole_observation(Raspi4bMachineState *s,
                                          const char *state,
                                          const char *source)
{
    g_autofree char *line = NULL;

    if (!s->netconsole_enabled) {
        return;
    }
    line = g_strdup_printf(
        "RPI4-BOOT: state=%s source=%s attempt=%" PRIu64 "\r\n",
        state, source, s->boot_attempt_count);
    raspi4_netconsole_write(s, line);
}

static bool raspi4_network_send_udp(Raspi4bMachineState *s,
                                    const uint8_t destination_mac[6],
                                    uint16_t source_port,
                                    uint16_t destination_port,
                                    const uint8_t *payload,
                                    size_t payload_size)
{
    return raspi4_network_send_udp_to(
        s, destination_mac, s->network_server_ip, source_port,
        destination_port, payload, payload_size);
}

static bool raspi4_network_send_tcp(Raspi4bMachineState *s, uint8_t flags,
                                    const uint8_t *payload,
                                    size_t payload_size)
{
    BCM2711GenetState *genet = raspi4_genet(s);
    const MACAddr *mac = bcm2711_genet_mac(genet);
    g_autofree uint8_t *packet = NULL;
    uint8_t *ip;
    uint8_t *tcp;
    uint16_t tcp_length;
    uint16_t ip_length;
    uint16_t checksum;
    size_t packet_length;

    if (payload_size > UINT16_MAX - 40) {
        return false;
    }
    packet_length = 14 + 20 + 20 + payload_size;
    packet = g_malloc0(packet_length);
    ip = packet + 14;
    tcp = ip + 20;
    memcpy(packet, s->network_server_mac, sizeof(s->network_server_mac));
    memcpy(packet + 6, mac->a, sizeof(mac->a));
    stw_be_p(packet + 12, 0x0800);
    tcp_length = 20 + payload_size;
    stw_be_p(tcp, s->network_http_client_port);
    stw_be_p(tcp + 2, s->http_port);
    stl_be_p(tcp + 4, s->network_http_client_seq);
    stl_be_p(tcp + 8, s->network_http_server_seq);
    tcp[12] = 5 << 4;
    tcp[13] = flags;
    stw_be_p(tcp + 14, 64240);
    if (payload_size) {
        memcpy(tcp + 20, payload, payload_size);
    }
    ip_length = 20 + tcp_length;
    ip[0] = 0x45;
    stw_be_p(ip + 2, ip_length);
    stw_be_p(ip + 4, s->network_dhcp_xid & 0xffff);
    ip[8] = 64;
    ip[9] = 6;
    stl_be_p(ip + 12, s->network_offered_ip);
    stl_be_p(ip + 16, s->network_server_ip);
    stw_be_p(ip + 10,
             net_checksum_finish(net_checksum_add(20, ip)));
    checksum = net_checksum_tcpudp(tcp_length, 6, ip + 12, tcp);
    stw_be_p(tcp + 16, checksum ? checksum : 0xffff);
    return bcm2711_genet_boot_send(genet, packet, packet_length) ==
           packet_length;
}

void raspi4_network_tls_clear(Raspi4bMachineState *s)
{
    g_clear_pointer(&s->network_tls_session, qcrypto_tls_session_free);
    g_clear_pointer(&s->network_tls_rx, g_free);
    g_clear_pointer(&s->network_tls_tx, g_free);
    s->network_tls_handshake_complete = false;
    s->network_tls_rx_size = 0;
    s->network_tls_rx_offset = 0;
    s->network_tls_tx_sequence = 0;
    s->network_tls_tx_size = 0;
}

static ssize_t raspi4_network_tls_write(const void *buf, size_t len,
                                        void *opaque, Error **errp)
{
    Raspi4bMachineState *s = opaque;
    const uint8_t *data = buf;
    uint32_t old_client_sequence = s->network_http_client_seq;
    uint32_t old_tx_size = s->network_tls_tx_size;
    size_t sent = 0;

    if (len > RASPI4_TLS_TX_MAX - s->network_tls_tx_size) {
        error_setg(errp, "TLS transmit flight exceeds modeled TCP window");
        return -1;
    }
    if (!s->network_tls_tx_size) {
        s->network_tls_tx_sequence = s->network_http_client_seq;
    }
    s->network_tls_tx = g_realloc(
        s->network_tls_tx, s->network_tls_tx_size + len);
    memcpy(s->network_tls_tx + s->network_tls_tx_size, buf, len);
    s->network_tls_tx_size += len;
    while (sent < len) {
        size_t segment = MIN(len - sent,
                             (size_t)RASPI4_HTTP_TCP_PAYLOAD_MAX);

        if (!raspi4_network_send_tcp(s, 0x18, data + sent, segment)) {
            s->network_http_client_seq = old_client_sequence;
            s->network_tls_tx_size = old_tx_size;
            if (!old_tx_size) {
                g_clear_pointer(&s->network_tls_tx, g_free);
                s->network_tls_tx_sequence = 0;
            }
            error_setg(errp, "failed to transmit TLS TCP segment");
            return -1;
        }
        s->network_http_client_seq += segment;
        sent += segment;
    }
    return len;
}

static bool raspi4_network_tls_ack(Raspi4bMachineState *s,
                                   uint32_t acknowledgement)
{
    uint32_t acknowledged;

    if ((int32_t)(acknowledgement - s->network_http_client_seq) > 0) {
        return false;
    }
    if (!s->network_tls_tx_size) {
        return true;
    }
    if ((int32_t)(acknowledgement - s->network_tls_tx_sequence) < 0) {
        return true;
    }
    acknowledged = acknowledgement - s->network_tls_tx_sequence;
    if (acknowledged > s->network_tls_tx_size) {
        return false;
    }
    if (!acknowledged) {
        return true;
    }
    memmove(s->network_tls_tx, s->network_tls_tx + acknowledged,
            s->network_tls_tx_size - acknowledged);
    s->network_tls_tx_size -= acknowledged;
    s->network_tls_tx_sequence = acknowledgement;
    if (!s->network_tls_tx_size) {
        g_clear_pointer(&s->network_tls_tx, g_free);
        s->network_tls_tx_sequence = 0;
    }
    return true;
}

static bool raspi4_network_tls_retransmit(Raspi4bMachineState *s)
{
    uint32_t current_sequence = s->network_http_client_seq;
    size_t sent = 0;

    if (!s->network_tls_tx_size) {
        return raspi4_network_send_tcp(s, 0x10, NULL, 0);
    }
    s->network_http_client_seq = s->network_tls_tx_sequence;
    while (sent < s->network_tls_tx_size) {
        size_t segment = MIN(
            s->network_tls_tx_size - sent,
            (size_t)RASPI4_HTTP_TCP_PAYLOAD_MAX);

        if (!raspi4_network_send_tcp(
                s, 0x18, s->network_tls_tx + sent, segment)) {
            s->network_http_client_seq = current_sequence;
            return false;
        }
        s->network_http_client_seq += segment;
        sent += segment;
    }
    s->network_http_client_seq = current_sequence;
    return true;
}

static ssize_t raspi4_network_tls_read(void *buf, size_t len,
                                       void *opaque, Error **errp)
{
    Raspi4bMachineState *s = opaque;
    size_t available = s->network_tls_rx_size - s->network_tls_rx_offset;
    size_t copied;

    if (!available) {
        return QCRYPTO_TLS_SESSION_ERR_BLOCK;
    }
    copied = MIN(available, len);
    memcpy(buf, s->network_tls_rx + s->network_tls_rx_offset, copied);
    s->network_tls_rx_offset += copied;
    if (s->network_tls_rx_offset == s->network_tls_rx_size) {
        g_clear_pointer(&s->network_tls_rx, g_free);
        s->network_tls_rx_size = 0;
        s->network_tls_rx_offset = 0;
    }
    return copied;
}

static size_t raspi4_network_http_format_request(Raspi4bMachineState *s,
                                                 char **request)
{
    g_autofree char *host =
        (s->http_port == RASPI4_HTTP_DEFAULT_PORT ||
         s->http_port == RASPI4_HTTPS_DEFAULT_PORT) ?
        g_strdup((const char *)s->http_host) :
        g_strdup_printf("%s:%u", s->http_host, s->http_port);

    *request = g_strdup_printf(
        "GET /%s/%s HTTP/1.1\r\nHost: %s\r\n"
        "Connection: close\r\nUser-Agent: Raspberry-Pi-Bootloader\r\n\r\n",
        s->http_path, s->network_tftp_filename, host);
    return strlen(*request);
}

static bool raspi4_network_http_send_request(Raspi4bMachineState *s,
                                             bool first)
{
    g_autofree char *request = NULL;
    size_t request_size = raspi4_network_http_format_request(s, &request);
    uint32_t current_sequence = s->network_http_client_seq;
    Error *error = NULL;
    ssize_t written;
    bool sent;

    if (s->network_http_tls) {
        written = qcrypto_tls_session_write(
            s->network_tls_session, request, request_size, &error);
        if (written != request_size) {
            error_free(error);
            raspi4_network_http_fail(
                s, written < 0 ? "network-https-write-failed" :
                           "network-https-write-blocked");
            return false;
        }
        return true;
    }
    if (first) {
        s->network_http_request_seq = current_sequence;
    } else {
        s->network_http_client_seq = s->network_http_request_seq;
    }
    sent = raspi4_network_send_tcp(
        s, 0x18, (const uint8_t *)request, request_size);
    if (first) {
        s->network_http_client_seq += request_size;
    } else {
        s->network_http_client_seq = current_sequence;
    }
    return sent;
}

static bool raspi4_network_tls_advance(Raspi4bMachineState *s)
{
    Error *error = NULL;
    uint8_t plaintext[RASPI4_HTTP_TCP_PAYLOAD_MAX];
    int status;

    if (!s->network_tls_handshake_complete) {
        status = qcrypto_tls_session_handshake(
            s->network_tls_session, &error);
        if (status < 0) {
            error_free(error);
            raspi4_network_http_fail(s, "network-https-handshake-failed");
            return false;
        }
        if (status != QCRYPTO_TLS_HANDSHAKE_COMPLETE) {
            return true;
        }
        if (qcrypto_tls_session_check_credentials(
                s->network_tls_session, &error) < 0) {
            warn_report_err(error);
            raspi4_network_http_fail(s, "network-https-peer-rejected");
            return false;
        }
        s->network_tls_handshake_complete = true;
        if (!raspi4_network_http_send_request(s, true)) {
            return false;
        }
    }
    while (s->pending_boot_action == RASPI4_PENDING_NETWORK_HTTP &&
           s->network_http_state == RASPI4_HTTP_RESPONSE) {
        ssize_t received = qcrypto_tls_session_read(
            s->network_tls_session, (char *)plaintext, sizeof(plaintext),
            &error);

        if (received == QCRYPTO_TLS_SESSION_ERR_BLOCK) {
            return true;
        }
        if (received <= 0) {
            error_free(error);
            raspi4_network_http_fail(
                s, received == QCRYPTO_TLS_SESSION_PREMATURE_TERMINATION ?
                       "network-https-truncated" :
                       "network-https-read-failed");
            return false;
        }
        if (!raspi4_network_http_plain_append(
                s, plaintext, received, false)) {
            return false;
        }
    }
    return true;
}

static bool raspi4_network_tls_start(Raspi4bMachineState *s)
{
    Object *object = NULL;
    QCryptoTLSCreds *creds = NULL;
    bool builtin = false;
    Error *error = NULL;

    raspi4_network_tls_clear(s);
    if (s->http_tls_creds) {
        object = object_resolve_path_component(
            object_get_objects_root(), s->http_tls_creds);
        creds = object ? (QCryptoTLSCreds *)object_dynamic_cast(
            object, TYPE_QCRYPTO_TLS_CREDS) : NULL;
        if (!creds ||
            !object_dynamic_cast(object, TYPE_QCRYPTO_TLS_CREDS_X509) ||
            !object_property_get_bool(object, "verify-peer", &error) ||
            !qcrypto_tls_creds_check_endpoint(
                creds, QCRYPTO_TLS_CREDS_ENDPOINT_CLIENT, &error)) {
            error_free(error);
            raspi4_network_http_fail(s, "network-https-creds-invalid");
            return false;
        }
    } else {
        creds = qcrypto_tls_creds_x509_new_client_from_pem(
            raspi4_network_install_ca,
            sizeof(raspi4_network_install_ca) - 1, &error);
        if (!creds) {
            error_free(error);
            raspi4_network_http_fail(s, "network-https-creds-invalid");
            return false;
        }
        builtin = true;
    }
    s->network_tls_session = qcrypto_tls_session_new(
        creds, (const char *)s->http_host, NULL,
        QCRYPTO_TLS_CREDS_ENDPOINT_CLIENT, &error);
    if (builtin) {
        object_unref(OBJECT(creds));
    }
    if (!s->network_tls_session) {
        error_free(error);
        raspi4_network_http_fail(s, "network-https-session-failed");
        return false;
    }
    qcrypto_tls_session_set_callbacks(
        s->network_tls_session, raspi4_network_tls_write,
        raspi4_network_tls_read, s);
    return raspi4_network_tls_advance(s);
}

static bool raspi4_network_send_arp(Raspi4bMachineState *s)
{
    static const uint8_t broadcast[6] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    };
    BCM2711GenetState *genet = raspi4_genet(s);
    const MACAddr *mac = bcm2711_genet_mac(genet);
    uint8_t packet[60] = { 0 };
    uint8_t *arp = packet + 14;

    memcpy(packet, broadcast, sizeof(broadcast));
    memcpy(packet + 6, mac->a, sizeof(mac->a));
    stw_be_p(packet + 12, 0x0806);
    stw_be_p(arp, 1);
    stw_be_p(arp + 2, 0x0800);
    arp[4] = 6;
    arp[5] = 4;
    stw_be_p(arp + 6, 1);
    memcpy(arp + 8, mac->a, sizeof(mac->a));
    stl_be_p(arp + 14, s->network_offered_ip);
    stl_be_p(arp + 24, s->network_next_hop_ip);
    return bcm2711_genet_boot_send(genet, packet, sizeof(packet)) ==
           sizeof(packet);
}

bool raspi4_network_static_configured(const Raspi4bMachineState *s)
{
    return s->tftp_ip_set && s->client_ip_set && s->subnet_set;
}

static uint32_t raspi4_network_next_hop_to(const Raspi4bMachineState *s,
                                           uint32_t destination)
{
    if (s->network_subnet && s->network_gateway &&
        ((s->network_offered_ip ^ destination) & s->network_subnet)) {
        return s->network_gateway;
    }
    return destination;
}

static uint32_t raspi4_network_next_hop(const Raspi4bMachineState *s)
{
    return raspi4_network_next_hop_to(s, s->network_server_ip);
}

static bool raspi4_network_send_dns_query(Raspi4bMachineState *s)
{
    uint8_t query[12 + RASPI4_DNS_NAME_MAX + 2 + 4] = { 0 };
    uint8_t *cursor = query + 12;
    const char *label = (const char *)s->network_tftp_hostname;

    if (!*label) {
        return false;
    }
    stw_be_p(query, s->network_dns_query_id);
    stw_be_p(query + 2, 0x0100);
    stw_be_p(query + 4, 1);
    while (*label) {
        const char *dot = strchr(label, '.');
        size_t length = dot ? dot - label : strlen(label);

        if (!length || length > 63 || cursor + 1 + length + 5 >
            query + sizeof(query)) {
            return false;
        }
        *cursor++ = length;
        memcpy(cursor, label, length);
        cursor += length;
        label = dot ? dot + 1 : label + length;
    }
    *cursor++ = 0;
    stw_be_p(cursor, 1);
    stw_be_p(cursor + 2, 1);
    cursor += 4;
    return raspi4_network_send_udp_to(
        s, s->network_server_mac, s->network_dns_server_ip,
        RASPI4_DNS_CLIENT_PORT, 53, query, cursor - query);
}

static bool raspi4_network_send_tftp_rrq(Raspi4bMachineState *s)
{
    static const char mode[] = "octet";
    static const char options[] = "tsize\0" "0\0" "blksize\0" "1024";
    g_autofree uint8_t *request = NULL;
    size_t filename_size;
    size_t request_size;

    if (!s->network_tftp_filename) {
        return false;
    }
    filename_size = strlen(s->network_tftp_filename) + 1;
    request_size = 2 + filename_size + sizeof(mode) +
                   (s->network_tftp_options_disabled ? 0 : sizeof(options));
    if (request_size > 512) {
        return false;
    }
    request = g_malloc0(request_size);
    stw_be_p(request, 1);
    memcpy(request + 2, s->network_tftp_filename, filename_size);
    memcpy(request + 2 + filename_size, mode, sizeof(mode));
    if (!s->network_tftp_options_disabled) {
        memcpy(request + 2 + filename_size + sizeof(mode),
               options, sizeof(options));
    }
    return raspi4_network_send_udp(
        s, s->network_server_mac, RASPI4_TFTP_CLIENT_PORT, 69,
        request, request_size);
}

static bool raspi4_network_send_tftp_ack_to(Raspi4bMachineState *s,
                                            uint16_t port, uint16_t block)
{
    uint8_t ack[4];

    stw_be_p(ack, 4);
    stw_be_p(ack + 2, block);
    return raspi4_network_send_udp(
        s, s->network_server_mac, RASPI4_TFTP_CLIENT_PORT,
        port, ack, sizeof(ack));
}

static bool raspi4_network_send_tftp_ack(Raspi4bMachineState *s,
                                         uint16_t block)
{
    return raspi4_network_send_tftp_ack_to(
        s, s->network_tftp_server_port, block);
}

static bool raspi4_network_send_tftp_error(Raspi4bMachineState *s,
                                           uint16_t port, uint16_t code,
                                           const char *message)
{
    g_autofree uint8_t *error = NULL;
    size_t message_size = strlen(message) + 1;
    size_t error_size = 4 + message_size;

    error = g_malloc(error_size);
    stw_be_p(error, 5);
    stw_be_p(error + 2, code);
    memcpy(error + 4, message, message_size);
    return raspi4_network_send_udp(
        s, s->network_server_mac, RASPI4_TFTP_CLIENT_PORT,
        port, error, error_size);
}

static uint32_t raspi4_network_tftp_retransmit_delay(
    const Raspi4bMachineState *s)
{
    uint64_t delay = (uint64_t)RASPI4_TFTP_RETRANSMIT_INITIAL_MS <<
                     MIN(s->network_tftp_file_retransmits, 3U);

    return MIN(delay, (uint64_t)RASPI4_TFTP_RETRANSMIT_MAX_MS);
}

static void raspi4_network_tftp_arm_retransmit(Raspi4bMachineState *s)
{
    timer_mod(s->network_tftp_retransmit_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
              raspi4_network_tftp_retransmit_delay(s));
}

void raspi4_network_tftp_cancel_retransmit(Raspi4bMachineState *s)
{
    timer_del(s->network_tftp_retransmit_timer);
    s->network_tftp_file_retransmits = 0;
}

static void raspi4_network_tftp_send_request(Raspi4bMachineState *s)
{
    raspi4_network_send_tftp_rrq(s);
    raspi4_network_tftp_arm_retransmit(s);
}

void raspi4_network_tftp_retransmit(void *opaque)
{
    Raspi4bMachineState *s = opaque;
    bool acknowledge;

    if (s->network_boot_wire &&
        s->pending_boot_action == RASPI4_PENDING_NETWORK_HTTP) {
        const char *action;

        if (s->network_http_state == RASPI4_HTTP_SYN_SENT) {
            uint32_t sequence = s->network_http_client_seq;

            s->network_http_client_seq--;
            raspi4_network_send_tcp(s, 0x02, NULL, 0);
            s->network_http_client_seq = sequence;
            action = "syn";
        } else if (s->network_http_tls) {
            raspi4_network_tls_retransmit(s);
            action = s->network_tls_tx_size ? "tls-flight" : "tls-ack";
        } else if (!s->network_http_response_size &&
                   !s->network_http_header_parsed) {
            raspi4_network_http_send_request(s, false);
            action = "get";
        } else {
            raspi4_network_send_tcp(s, 0x10, NULL, 0);
            action = "ack";
        }
        s->network_tftp_file_retransmits++;
        s->network_tftp_retransmit_count++;
        trace_raspi4b_boot_event(
            "boot-source", "boot-source.http-retransmit", "http",
            action, s->network_tftp_retransmit_count);
        raspi4_network_tftp_arm_retransmit(s);
        return;
    }
    if (!s->network_boot_wire ||
        s->pending_boot_action != RASPI4_PENDING_NETWORK_TFTP) {
        return;
    }
    acknowledge = s->network_tftp_server_port &&
                  (s->network_tftp_oack_accepted ||
                   s->network_tftp_block_number > 1);
    if (acknowledge) {
        raspi4_network_send_tftp_ack(
            s, s->network_tftp_oack_accepted ? 0 :
               (uint16_t)(s->network_tftp_block_number - 1));
    } else {
        raspi4_network_send_tftp_rrq(s);
    }
    s->network_tftp_file_retransmits++;
    s->network_tftp_retransmit_count++;
    trace_raspi4b_boot_event(
        "boot-source", "boot-source.tftp-retransmit", "network",
        acknowledge ? "ack" : "rrq",
        s->network_tftp_retransmit_count);
    raspi4_network_tftp_arm_retransmit(s);
}

void raspi4_network_dhcp_arm_retransmit(Raspi4bMachineState *s)
{
    timer_mod(s->network_dhcp_retransmit_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
              s->dhcp_req_timeout);
}

void raspi4_network_dhcp_cancel_retransmit(Raspi4bMachineState *s)
{
    timer_del(s->network_dhcp_retransmit_timer);
}

void raspi4_network_dhcp_retransmit(void *opaque)
{
    Raspi4bMachineState *s = opaque;

    if (s->network_boot_wire &&
        s->pending_boot_action == RASPI4_PENDING_NETWORK_DNS) {
        raspi4_network_send_dns_query(s);
        s->network_dns_retransmit_count++;
        trace_raspi4b_boot_event(
            "boot-source", "boot-source.dns-retransmit", "network",
            "query", s->network_dns_retransmit_count);
        raspi4_network_dhcp_arm_retransmit(s);
        return;
    }
    if (!s->network_boot_wire ||
        s->pending_boot_action != RASPI4_PENDING_NETWORK_DHCP ||
        (s->network_dhcp_phase != RASPI4_DHCP_PHASE_DISCOVER &&
         s->network_dhcp_phase != RASPI4_DHCP_PHASE_REQUEST)) {
        return;
    }
    raspi4_network_send_dhcp(
        s, s->network_dhcp_phase == RASPI4_DHCP_PHASE_REQUEST);
    s->network_dhcp_retransmit_count++;
    trace_raspi4b_boot_event(
        "boot-source", "boot-source.dhcp-retransmit", "network",
        s->network_dhcp_phase == RASPI4_DHCP_PHASE_REQUEST ?
            "request" : "discover",
        s->network_dhcp_retransmit_count);
    raspi4_network_dhcp_arm_retransmit(s);
}

void raspi4_network_boot_tx(void *opaque)
{
    Raspi4bMachineState *s = opaque;

    if (s->network_boot_wire &&
        s->pending_boot_action == RASPI4_PENDING_NETWORK_DHCP &&
        s->network_dhcp_phase == RASPI4_DHCP_PHASE_DISCOVER) {
        raspi4_network_send_dhcp(s, false);
        raspi4_network_dhcp_arm_retransmit(s);
    }
}

void raspi4_network_vm_state_change(void *opaque, bool running,
                                           RunState state)
{
    if (running) {
        raspi4_network_boot_tx(opaque);
    }
}

static bool raspi4_network_http_start_connection(Raspi4bMachineState *s)
{
    raspi4_network_tls_clear(s);
    s->pending_boot_action = RASPI4_PENDING_NETWORK_HTTP;
    s->network_http_state = RASPI4_HTTP_SYN_SENT;
    s->network_http_client_port = RASPI4_HTTP_CLIENT_PORT +
                                  s->network_artifact_index;
    s->network_http_client_seq = 0x52504900U +
                                 ((uint32_t)s->network_artifact_index << 16);
    s->network_http_request_seq = 0;
    s->network_http_server_seq = 0;
    s->network_http_response_size = 0;
    s->network_http_content_length = 0;
    s->network_http_header_parsed = false;
    g_clear_pointer(&s->network_http_response, g_free);
    s->network_http_ooo_sequence = 0;
    s->network_http_ooo_size = 0;
    s->network_http_ooo_fin = false;
    s->network_http_ooo_fin_sequence = 0;
    g_clear_pointer(&s->network_http_ooo_data, g_free);
    g_clear_pointer(&s->network_http_ooo_valid, g_free);
    raspi4_set_boot_observation(s, "network-http-connect", "http");
    if (!raspi4_network_send_tcp(s, 0x02, NULL, 0)) {
        return false;
    }
    s->network_http_client_seq++;
    s->network_tftp_file_retransmits = 0;
    raspi4_network_tftp_arm_retransmit(s);
    timer_mod(s->boot_timeout_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
              s->tftp_file_timeout);
    return true;
}

static bool raspi4_network_arp_receive(Raspi4bMachineState *s,
                                       const uint8_t *packet, size_t size)
{
    const MACAddr *mac = bcm2711_genet_mac(raspi4_genet(s));
    const uint8_t *arp;

    if (size < 42 || lduw_be_p(packet + 12) != 0x0806) {
        return true;
    }
    arp = packet + 14;
    if (lduw_be_p(arp) != 1 || lduw_be_p(arp + 2) != 0x0800 ||
        arp[4] != 6 || arp[5] != 4 || lduw_be_p(arp + 6) != 2 ||
        ldl_be_p(arp + 14) != s->network_next_hop_ip ||
        ldl_be_p(arp + 24) != s->network_offered_ip ||
        memcmp(arp + 18, mac->a, sizeof(mac->a))) {
        return true;
    }
    memcpy(s->network_server_mac, arp + 8,
           sizeof(s->network_server_mac));
    timer_del(s->boot_timeout_timer);
    if (s->network_arp_for_dns) {
        s->pending_boot_action = RASPI4_PENDING_NETWORK_DNS;
        raspi4_set_boot_observation(s, "network-dns-wait", "network");
        raspi4_network_send_dns_query(s);
        raspi4_network_dhcp_arm_retransmit(s);
        timer_mod(s->boot_timeout_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + s->dhcp_timeout);
        return true;
    }
    if (s->network_http_mode) {
        return raspi4_network_http_start_connection(s);
    }
    s->pending_boot_action = RASPI4_PENDING_NETWORK_TFTP;
    s->network_tftp_server_port = 0;
    s->network_tftp_next_block = 1;
    s->network_tftp_block_number = 1;
    s->network_tftp_size = 0;
    g_clear_pointer(&s->network_tftp_data, g_free);
    g_clear_pointer(&s->network_http_response, g_free);
    g_clear_pointer(&s->network_http_ooo_data, g_free);
    g_clear_pointer(&s->network_http_ooo_valid, g_free);
    raspi4_network_tls_clear(s);
    s->network_http_mode = false;
    s->network_http_default_host = false;
    s->network_http_tls = false;
    s->network_http_state = RASPI4_HTTP_IDLE;
    s->network_http_response_size = 0;
    s->network_http_content_length = 0;
    s->network_http_header_parsed = false;
    s->network_http_ooo_sequence = 0;
    s->network_http_ooo_size = 0;
    s->network_http_ooo_fin = false;
    s->network_http_ooo_fin_sequence = 0;
    raspi4_set_boot_observation(s, "network-tftp-transfer", "network");
    raspi4_network_tftp_send_request(s);
    timer_mod(s->boot_timeout_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
              s->tftp_file_timeout);
    return true;
}

bool raspi4_network_begin_tftp(Raspi4bMachineState *s)
{
    if (!raspi4_network_prepare_tftp(s, false)) {
        return false;
    }
    if (s->tftp_ip_set) {
        s->network_server_ip = s->tftp_ip;
    }
    s->network_next_hop_ip = raspi4_network_next_hop(s);
    s->network_arp_for_dns = false;
    memset(s->network_server_mac, 0, sizeof(s->network_server_mac));
    s->pending_boot_action = RASPI4_PENDING_NETWORK_ARP;
    raspi4_set_boot_observation(s, "network-arp-wait", "network");
    raspi4_network_send_arp(s);
    timer_mod(s->boot_timeout_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
              s->tftp_file_timeout);
    return true;
}

static bool raspi4_network_http_begin_arp(Raspi4bMachineState *s)
{
    s->network_next_hop_ip = raspi4_network_next_hop(s);
    s->network_arp_for_dns = false;
    memset(s->network_server_mac, 0, sizeof(s->network_server_mac));
    s->pending_boot_action = RASPI4_PENDING_NETWORK_ARP;
    raspi4_set_boot_observation(s, "network-http-arp-wait", "http");
    raspi4_network_send_arp(s);
    timer_mod(s->boot_timeout_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
              s->tftp_file_timeout);
    return true;
}

bool raspi4_network_begin_http(Raspi4bMachineState *s)
{
    uint32_t address;

    s->network_http_default_host = !s->http_host_set;
    s->network_http_tls = s->network_http_default_host;
    if (s->http_cacert_hash_set ||
        (s->http_host_set &&
         (!s->otp_secure_boot || !s->secure_public_key_valid)) ||
        (s->network_http_default_host && s->otp_secure_boot)) {
        return false;
    }
    if (s->network_http_default_host) {
        pstrcpy((char *)s->http_host, sizeof(s->http_host),
                RASPI4_HTTP_DEFAULT_HOST);
        s->http_port = RASPI4_HTTPS_DEFAULT_PORT;
    }
    if (!raspi4_network_prepare_response_tftp(s, false)) {
        return false;
    }
    if (raspi4_parse_ipv4((const char *)s->http_host, &address)) {
        s->network_server_ip = address;
        return raspi4_network_http_begin_arp(s);
    }
    pstrcpy((char *)s->network_tftp_hostname,
            sizeof(s->network_tftp_hostname),
            (const char *)s->http_host);
    if (!raspi4_ipv4_is_unicast(s->network_dns_server_ip)) {
        return false;
    }
    return raspi4_network_begin_dns(s);
}

static bool raspi4_network_begin_dns(Raspi4bMachineState *s)
{
    if (!s->network_tftp_hostname[0] ||
        !raspi4_ipv4_is_unicast(s->network_dns_server_ip)) {
        return false;
    }
    s->network_dns_query_id = s->network_dhcp_xid & 0xffff;
    s->network_next_hop_ip = raspi4_network_next_hop_to(
        s, s->network_dns_server_ip);
    s->network_arp_for_dns = true;
    memset(s->network_server_mac, 0, sizeof(s->network_server_mac));
    s->pending_boot_action = RASPI4_PENDING_NETWORK_ARP;
    raspi4_set_boot_observation(s, "network-dns-arp-wait", "network");
    raspi4_network_send_arp(s);
    timer_mod(s->boot_timeout_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + s->dhcp_timeout);
    return true;
}

static bool raspi4_dns_skip_name(const uint8_t **cursor, const uint8_t *end)
{
    const uint8_t *position = *cursor;

    while (position < end) {
        uint8_t length = *position++;

        if (!length) {
            *cursor = position;
            return true;
        }
        if ((length & 0xc0) == 0xc0) {
            if (position >= end) {
                return false;
            }
            *cursor = position + 1;
            return true;
        }
        if ((length & 0xc0) || length > end - position) {
            return false;
        }
        position += length;
    }
    return false;
}

static bool raspi4_network_dns_receive(Raspi4bMachineState *s,
                                       const uint8_t *packet, size_t size)
{
    const MACAddr *mac = bcm2711_genet_mac(raspi4_genet(s));
    const uint8_t *ip;
    const uint8_t *udp;
    const uint8_t *dns;
    const uint8_t *cursor;
    const uint8_t *end;
    unsigned int ip_header_length;
    uint16_t ip_length;
    uint16_t udp_length;
    uint16_t flags;
    uint16_t questions;
    uint16_t answers;

    if (size < 14 + 20 + 8 + 12 || lduw_be_p(packet + 12) != 0x0800 ||
        memcmp(packet, mac->a, sizeof(mac->a))) {
        return true;
    }
    ip = packet + 14;
    ip_header_length = (ip[0] & 0xf) * 4;
    ip_length = lduw_be_p(ip + 2);
    if ((ip[0] >> 4) != 4 || ip_header_length < 20 || ip[9] != 17 ||
        (lduw_be_p(ip + 6) & 0x3fff) ||
        ip_length < ip_header_length + 8 + 12 ||
        ip_length > size - 14 ||
        ldl_be_p(ip + 12) != s->network_dns_server_ip ||
        ldl_be_p(ip + 16) != s->network_offered_ip ||
        net_raw_checksum((uint8_t *)ip, ip_header_length)) {
        return true;
    }
    udp = ip + ip_header_length;
    udp_length = lduw_be_p(udp + 4);
    if (lduw_be_p(udp) != 53 ||
        lduw_be_p(udp + 2) != RASPI4_DNS_CLIENT_PORT ||
        udp_length < 8 + 12 || udp_length > ip_length - ip_header_length ||
        (lduw_be_p(udp + 6) &&
         net_checksum_tcpudp(udp_length, 17, (uint8_t *)ip + 12,
                              (uint8_t *)udp))) {
        return true;
    }
    dns = udp + 8;
    end = udp + udp_length;
    flags = lduw_be_p(dns + 2);
    questions = lduw_be_p(dns + 4);
    answers = lduw_be_p(dns + 6);
    if (lduw_be_p(dns) != s->network_dns_query_id ||
        (flags & 0xf80f) != 0x8000 || (flags & 0x0200) ||
        questions != 1 || !answers) {
        return true;
    }
    cursor = dns + 12;
    if (!raspi4_dns_skip_name(&cursor, end) || end - cursor < 4) {
        return true;
    }
    cursor += 4;
    for (unsigned int i = 0; i < answers; i++) {
        uint16_t type;
        uint16_t class;
        uint16_t length;
        uint32_t address;

        if (!raspi4_dns_skip_name(&cursor, end) || end - cursor < 10) {
            return true;
        }
        type = lduw_be_p(cursor);
        class = lduw_be_p(cursor + 2);
        length = lduw_be_p(cursor + 8);
        cursor += 10;
        if (length > end - cursor) {
            return true;
        }
        address = length == 4 ? ldl_be_p(cursor) : 0;
        if (type == 1 && class == 1 && length == 4 &&
            raspi4_ipv4_is_unicast(address)) {
            raspi4_network_dhcp_cancel_retransmit(s);
            timer_del(s->boot_timeout_timer);
            s->pending_boot_action = RASPI4_PENDING_NONE;
            s->network_server_ip = address;
            s->network_arp_for_dns = false;
            trace_raspi4b_boot_event(
                "boot-source", "boot-source.dns", "network",
                "resolved", address);
            if (s->network_http_mode) {
                raspi4_network_http_begin_arp(s);
            } else if (!raspi4_network_begin_tftp(s)) {
                raspi4_schedule_network_wait(s, false);
            }
            return true;
        }
        cursor += length;
    }
    return true;
}

void raspi4_network_complete_artifacts(Raspi4bMachineState *s)
{
    uint32_t completed_size = s->network_artifact_index ?
        s->network_artifacts[s->network_artifact_index - 1].size : 0;

    s->pending_boot_action = RASPI4_PENDING_NONE;
    s->network_tftp_dally_server_port = 0;
    s->network_tftp_dally_block = 0;
    bcm2711_genet_set_boot_client_active(raspi4_genet(s), false);
    if (raspi4_try_network_artifacts(s) == RASPI4_BOOT_ATTEMPT_READY) {
        raspi4_set_boot_observation(s, "arm-handoff-ready", "network");
        trace_raspi4b_boot_event("boot-source", "boot-source.tftp",
                                 "network", "success",
                                 completed_size);
    }
}

static void raspi4_network_continue_after_artifact(Raspi4bMachineState *s,
                                                   bool final_data)
{
    bool ready;

    raspi4_network_tftp_cancel_retransmit(s);
    timer_del(s->boot_timeout_timer);
    if (s->network_response_discovery) {
        ready = raspi4_network_response_advance(s);
        if (s->pending_boot_action == RASPI4_PENDING_RECOVERY_REBOOT) {
            bcm2711_genet_set_boot_client_active(raspi4_genet(s), false);
            return;
        }
        if (!ready) {
            if (s->self_update_status == RASPI4_SELF_UPDATE_INVALID ||
                s->self_update_status ==
                    RASPI4_SELF_UPDATE_WRITE_PROTECTED ||
                s->self_update_status ==
                    RASPI4_SELF_UPDATE_PROGRAM_FAILED) {
                raspi4_network_tftp_fail(s, 0);
                return;
            }
            raspi4_set_boot_observation(s, "network-tftp-discovery-invalid",
                                        "network");
            timer_mod(s->boot_timeout_timer,
                      qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                      s->tftp_file_timeout);
            return;
        }
        if (s->network_discovery_phase != RASPI4_NETWORK_DISCOVERY_READY) {
            s->network_tftp_dally_server_port = 0;
            s->network_tftp_dally_block = 0;
            raspi4_network_tftp_send_request(s);
            timer_mod(s->boot_timeout_timer,
                      qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                      s->tftp_file_timeout);
            return;
        }
    } else if (s->network_artifact_index < s->network_artifact_count) {
        raspi4_network_select_artifact(s, true);
        s->network_tftp_dally_server_port = 0;
        s->network_tftp_dally_block = 0;
        raspi4_network_tftp_send_request(s);
        timer_mod(s->boot_timeout_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                  s->tftp_file_timeout);
        return;
    }

    if (final_data) {
        s->pending_boot_action = RASPI4_PENDING_NETWORK_TFTP_DALLY;
        raspi4_set_boot_observation(s, "network-tftp-dally", "network");
        timer_mod(s->boot_timeout_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                  RASPI4_TFTP_DALLY_MS);
    } else {
        raspi4_network_complete_artifacts(s);
    }
}

void raspi4_network_tftp_fail(Raspi4bMachineState *s,
                                     uint16_t error_code)
{
    uint8_t index = s->boot_order_index;

    raspi4_network_tftp_cancel_retransmit(s);
    timer_del(s->boot_timeout_timer);
    s->pending_boot_action = RASPI4_PENDING_NONE;
    s->network_tftp_dally_server_port = 0;
    s->network_tftp_dally_block = 0;
    raspi4_set_boot_observation(s, "network-tftp-error", "network");
    trace_raspi4b_boot_event("boot-source", "boot-source.tftp-error",
                             "network", "server-error", error_code);
    if (s->net_boot_max_retries >= 0 &&
        s->network_attempt > s->net_boot_max_retries) {
        s->network_dhcp_phase = RASPI4_DHCP_PHASE_NONE;
        bcm2711_genet_set_boot_client_active(raspi4_genet(s), false);
        raspi4_execute_after_source_failure(s, index);
        return;
    }
    s->boot_retry_count++;
    raspi4_try_network_firmware(s);
}

static void raspi4_network_artifact_set_filename(
    Raspi4bMachineState *s, Raspi4NetworkArtifact *artifact,
    const char *filename)
{
    bool valid = raspi4_network_artifact_set_paths(s, artifact, filename);

    g_assert(valid);
}

static void raspi4_network_retry_selected_artifact(Raspi4bMachineState *s,
                                                   const char *state)
{
    raspi4_network_tftp_cancel_retransmit(s);
    raspi4_network_select_artifact(s, true);
    raspi4_set_boot_observation(s, state, "network");
    raspi4_network_tftp_send_request(s);
    timer_mod(s->boot_timeout_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
              s->tftp_file_timeout);
}

static bool raspi4_network_try_legacy_firmware(Raspi4bMachineState *s)
{
    Raspi4NetworkArtifact *start = raspi4_network_find_artifact_mutable(
        s, RASPI4_NETWORK_ARTIFACT_START, NULL);
    Raspi4NetworkArtifact *fixup = raspi4_network_find_artifact_mutable(
        s, RASPI4_NETWORK_ARTIFACT_FIXUP, NULL);
    const char *fallback_start;
    const char *fallback_fixup;

    if (!start || !fixup ||
        &s->network_artifacts[s->network_artifact_index] != start) {
        return false;
    }
    if (!g_ascii_strcasecmp(raspi4_network_artifact_filename(start),
                            "start4.elf") &&
        !g_ascii_strcasecmp(raspi4_network_artifact_filename(fixup),
                            "fixup4.dat")) {
        fallback_start = "start.elf";
        fallback_fixup = "fixup.dat";
    } else if (!g_ascii_strcasecmp(
                   raspi4_network_artifact_filename(start), "start4x.elf") &&
               !g_ascii_strcasecmp(
                   raspi4_network_artifact_filename(fixup), "fixup4x.dat")) {
        fallback_start = "start_x.elf";
        fallback_fixup = "fixup_x.dat";
    } else {
        return false;
    }
    raspi4_network_artifact_set_filename(s, start, fallback_start);
    raspi4_network_artifact_set_filename(s, fixup, fallback_fixup);
    raspi4_network_retry_selected_artifact(
        s, "network-tftp-legacy-firmware");
    return true;
}

static bool raspi4_network_try_tftp_prefix_fallback(
    Raspi4bMachineState *s)
{
    Raspi4NetworkArtifact *start = raspi4_network_find_artifact_mutable(
        s, RASPI4_NETWORK_ARTIFACT_START, NULL);

    if (!s->tftp_prefix[0] || s->network_tftp_prefix_fallback ||
        !start ||
        &s->network_artifacts[s->network_artifact_index] != start) {
        return false;
    }

    s->network_tftp_prefix_fallback = true;
    for (unsigned int i = 0; i < s->network_artifact_count; i++) {
        Raspi4NetworkArtifact *artifact = &s->network_artifacts[i];
        g_autofree char *filename = g_strdup(
            raspi4_network_artifact_filename(artifact));

        raspi4_network_artifact_set_filename(s, artifact, filename);
    }
    raspi4_network_retry_selected_artifact(
        s, "network-tftp-device-prefix-fallback");
    return true;
}

static bool raspi4_network_try_prefix_fallback(Raspi4bMachineState *s)
{
    RaspiFirmwareConfig config;
    Raspi4NetworkArtifact *kernel;
    Raspi4NetworkArtifact *device_tree;
    unsigned int kernel_index = 0;
    bool ready = false;

    if (s->network_prefix_fallback) {
        return false;
    }
    kernel = raspi4_network_find_artifact_mutable(
        s, RASPI4_NETWORK_ARTIFACT_KERNEL, &kernel_index);
    device_tree = raspi4_network_find_artifact_mutable(
        s, RASPI4_NETWORK_ARTIFACT_DTB, NULL);
    if (!kernel || !device_tree ||
        (&s->network_artifacts[s->network_artifact_index] != kernel &&
         &s->network_artifacts[s->network_artifact_index] != device_tree)) {
        return false;
    }

    raspi_firmware_config_init(&config, s->cm4);
    config.bootvar0 = s->bootvar0;
    raspi4_set_firmware_filter_inputs(s, &config);
    if (!raspi_firmware_config_load_with_reader_named(
            &config, raspi4_network_read_artifact, s,
            raspi4_config_path(s), NULL) ||
        !config.os_prefix[0]) {
        goto out;
    }
    if (!g_str_has_prefix(raspi4_network_artifact_filename(kernel),
                          config.os_prefix) &&
        !g_str_has_prefix(raspi4_network_artifact_filename(device_tree),
                          config.os_prefix)) {
        goto out;
    }
    for (unsigned int i = kernel_index; i < s->network_artifact_count; i++) {
        Raspi4NetworkArtifact *artifact = &s->network_artifacts[i];
        const char *filename = raspi4_network_artifact_filename(artifact);

        if (g_str_has_prefix(filename, config.os_prefix)) {
            g_autofree char *fallback = g_strdup(
                filename + strlen(config.os_prefix));

            raspi4_network_artifact_set_filename(
                s, artifact, fallback);
        }
        g_clear_pointer(&artifact->data, g_free);
        artifact->size = 0;
        artifact->missing = false;
    }
    s->network_prefix_fallback = true;
    s->network_artifact_index = kernel_index;
    raspi4_network_retry_selected_artifact(
        s, "network-tftp-prefix-fallback");
    ready = true;
out:
    raspi_firmware_config_clear(&config);
    return ready;
}

static bool raspi4_tftp_parse_u32(const char *value, uint32_t *result)
{
    uint32_t parsed = 0;

    if (!*value) {
        return false;
    }
    for (; *value; value++) {
        uint32_t digit;

        if (!g_ascii_isdigit(*value)) {
            return false;
        }
        digit = *value - '0';
        if (parsed > (UINT32_MAX - digit) / 10) {
            return false;
        }
        parsed = parsed * 10 + digit;
    }
    *result = parsed;
    return true;
}

static bool raspi4_network_tftp_oack(Raspi4bMachineState *s,
                                     uint16_t source_port,
                                     const uint8_t *tftp,
                                     size_t tftp_size)
{
    const uint8_t *cursor = tftp + 2;
    const uint8_t *end = tftp + tftp_size;
    uint32_t block_size = RASPI4_TFTP_CLASSIC_BLOCK_SIZE;
    uint32_t transfer_size = 0;
    bool have_blksize = false;
    bool have_tsize = false;

    if (s->network_tftp_oack_accepted &&
        s->network_tftp_server_port == source_port &&
        s->network_tftp_block_number == 1 && !s->network_tftp_size) {
        raspi4_network_send_tftp_ack(s, 0);
        s->network_tftp_file_retransmits = 0;
        raspi4_network_tftp_arm_retransmit(s);
        return true;
    }
    if (s->network_tftp_options_disabled || s->network_tftp_server_port ||
        s->network_tftp_block_number != 1 || s->network_tftp_size) {
        goto invalid;
    }
    while (cursor < end) {
        const uint8_t *name_end = memchr(cursor, 0, end - cursor);
        const char *name = (const char *)cursor;
        const uint8_t *value_end;
        const char *value;

        if (!name_end || name_end == cursor || name_end + 1 >= end) {
            goto invalid;
        }
        cursor = name_end + 1;
        value = (const char *)cursor;
        value_end = memchr(cursor, 0, end - cursor);
        if (!value_end || value_end == cursor) {
            goto invalid;
        }
        cursor = value_end + 1;
        if (!g_ascii_strcasecmp(name, "blksize")) {
            if (have_blksize ||
                !raspi4_tftp_parse_u32(value, &block_size) ||
                block_size < 8 ||
                block_size > RASPI4_TFTP_REQUESTED_BLOCK_SIZE) {
                goto invalid;
            }
            have_blksize = true;
        } else if (!g_ascii_strcasecmp(name, "tsize")) {
            if (have_tsize ||
                !raspi4_tftp_parse_u32(value, &transfer_size) ||
                transfer_size > s->network_tftp_expected_size ||
                (s->network_tftp_expected_sha256 &&
                 transfer_size != s->network_tftp_expected_size)) {
                goto invalid;
            }
            have_tsize = true;
        } else {
            goto invalid;
        }
    }
    if (!have_blksize && !have_tsize) {
        goto invalid;
    }

    s->network_tftp_server_port = source_port;
    s->network_tftp_block_size = block_size;
    s->network_tftp_oack_accepted = true;
    s->network_tftp_tsize_valid = have_tsize;
    s->network_tftp_tsize = transfer_size;
    raspi4_network_send_tftp_ack(s, 0);
    s->network_tftp_file_retransmits = 0;
    raspi4_network_tftp_arm_retransmit(s);
    return true;

invalid:
    raspi4_network_send_tftp_error(
        s, source_port, 8, "TFTP option negotiation failed");
    raspi4_network_tftp_fail(s, 8);
    return true;
}

static bool raspi4_network_tftp_receive(Raspi4bMachineState *s,
                                        const uint8_t *packet, size_t size)
{
    const MACAddr *mac = bcm2711_genet_mac(raspi4_genet(s));
    const uint8_t *ip;
    const uint8_t *udp;
    const uint8_t *tftp;
    unsigned int ip_header_length;
    uint16_t ip_length;
    uint16_t udp_length;
    uint16_t source_port;
    uint16_t opcode;
    uint16_t block;
    size_t tftp_size;
    size_t data_size;

    if (size < 14 + 20 + 8 + 4 || lduw_be_p(packet + 12) != 0x0800 ||
        memcmp(packet, mac->a, sizeof(mac->a)) ||
        memcmp(packet + 6, s->network_server_mac,
               sizeof(s->network_server_mac))) {
        return true;
    }
    ip = packet + 14;
    ip_header_length = (ip[0] & 0xf) * 4;
    ip_length = lduw_be_p(ip + 2);
    if ((ip[0] >> 4) != 4 || ip_header_length < 20 || ip[9] != 17 ||
        (lduw_be_p(ip + 6) & 0x3fff) ||
        ip_length < ip_header_length + 12 || ip_length > size - 14 ||
        ldl_be_p(ip + 12) != s->network_server_ip ||
        ldl_be_p(ip + 16) != s->network_offered_ip ||
        net_raw_checksum((uint8_t *)ip, ip_header_length)) {
        return true;
    }
    udp = ip + ip_header_length;
    udp_length = lduw_be_p(udp + 4);
    source_port = lduw_be_p(udp);
    if (lduw_be_p(udp + 2) != RASPI4_TFTP_CLIENT_PORT ||
        udp_length < 12 || udp_length > ip_length - ip_header_length ||
        (lduw_be_p(udp + 6) &&
         net_checksum_tcpudp(udp_length, 17, (uint8_t *)ip + 12,
                              (uint8_t *)udp))) {
        return true;
    }
    tftp = udp + 8;
    tftp_size = udp_length - 8;
    opcode = lduw_be_p(tftp);

    if (source_port == s->network_tftp_dally_server_port && opcode == 3 &&
        tftp_size >= 4 &&
        lduw_be_p(tftp + 2) == s->network_tftp_dally_block) {
        raspi4_network_send_tftp_ack_to(
            s, s->network_tftp_dally_server_port,
            s->network_tftp_dally_block);
        if (s->pending_boot_action == RASPI4_PENDING_NETWORK_TFTP_DALLY) {
            timer_mod(s->boot_timeout_timer,
                      qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                      RASPI4_TFTP_DALLY_MS);
        }
        return true;
    }
    if (s->pending_boot_action == RASPI4_PENDING_NETWORK_TFTP_DALLY) {
        return true;
    }
    if (s->network_tftp_server_port &&
        source_port != s->network_tftp_server_port) {
        raspi4_network_send_tftp_error(
            s, source_port, 5, "Unknown transfer ID");
        return true;
    }
    if (opcode == 5 && s->network_response_discovery &&
        s->network_artifact_index < s->network_artifact_count) {
        Raspi4NetworkArtifact *artifact =
            &s->network_artifacts[s->network_artifact_index];
        uint16_t error_code;

        if (tftp_size < 5 || !memchr(tftp + 4, 0, tftp_size - 4)) {
            return true;
        }
        error_code = lduw_be_p(tftp + 2);

        if (!s->network_tftp_server_port) {
            s->network_tftp_server_port = source_port;
        }
        if (error_code == 8 && !s->network_tftp_options_disabled &&
            s->network_tftp_block_number == 1 && !s->network_tftp_size) {
            raspi4_network_tftp_cancel_retransmit(s);
            s->network_tftp_server_port = 0;
            s->network_tftp_block_size = RASPI4_TFTP_CLASSIC_BLOCK_SIZE;
            s->network_tftp_options_disabled = true;
            s->network_tftp_oack_accepted = false;
            s->network_tftp_tsize_valid = false;
            s->network_tftp_tsize = 0;
            raspi4_network_tftp_send_request(s);
            return true;
        }
        if (error_code == 1 &&
            (raspi4_network_try_legacy_firmware(s) ||
             raspi4_network_try_tftp_prefix_fallback(s) ||
             raspi4_network_try_prefix_fallback(s))) {
            return true;
        }
        if (error_code == 1 && !artifact->required) {
            artifact->missing = true;
            artifact->size = 0;
            s->network_artifact_index++;
            raspi4_network_continue_after_artifact(s, false);
        } else {
            raspi4_network_tftp_fail(s, error_code);
        }
        return true;
    }
    if (opcode == 6) {
        return raspi4_network_tftp_oack(
            s, source_port, tftp, tftp_size);
    }
    if (opcode != 3) {
        raspi4_network_send_tftp_error(
            s, source_port, 4, "Illegal TFTP operation");
        return true;
    }
    block = lduw_be_p(tftp + 2);
    data_size = udp_length - 12;
    if (!s->network_tftp_server_port && block == 1) {
        s->network_tftp_block_size = RASPI4_TFTP_CLASSIC_BLOCK_SIZE;
        s->network_tftp_options_disabled = true;
        s->network_tftp_oack_accepted = false;
        s->network_tftp_tsize_valid = false;
        s->network_tftp_tsize = 0;
    }
    if (data_size > s->network_tftp_block_size) {
        return true;
    }
    if (s->network_tftp_server_port && s->network_tftp_block_number > 1 &&
        block == (uint16_t)(s->network_tftp_block_number - 1)) {
        raspi4_network_send_tftp_ack(s, block);
        s->network_tftp_file_retransmits = 0;
        raspi4_network_tftp_arm_retransmit(s);
        return true;
    }
    if (block != (uint16_t)s->network_tftp_block_number ||
        s->network_tftp_size > s->network_tftp_expected_size ||
        data_size > s->network_tftp_expected_size - s->network_tftp_size) {
        return true;
    }
    if (!s->network_tftp_server_port) {
        s->network_tftp_server_port = source_port;
    }
    s->network_tftp_oack_accepted = false;
    s->network_tftp_data = g_realloc(
        s->network_tftp_data, s->network_tftp_size + data_size);
    memcpy(s->network_tftp_data + s->network_tftp_size, tftp + 4,
           data_size);
    s->network_tftp_size += data_size;
    s->network_tftp_block_number++;
    s->network_tftp_next_block = s->network_tftp_block_number;
    raspi4_network_send_tftp_ack(s, block);
    s->network_tftp_file_retransmits = 0;
    raspi4_network_tftp_arm_retransmit(s);
    if (data_size < s->network_tftp_block_size) {
        g_autofree char *sha256 = g_compute_checksum_for_data(
            G_CHECKSUM_SHA256, s->network_tftp_data,
            s->network_tftp_size);

        if ((s->network_tftp_expected_sha256 &&
             (s->network_tftp_size != s->network_tftp_expected_size ||
              strcmp(sha256, s->network_tftp_expected_sha256))) ||
            (!s->network_tftp_expected_sha256 &&
             s->network_artifacts[s->network_artifact_index].required &&
             !s->network_tftp_size)) {
            raspi4_set_boot_observation(s, "network-tftp-content-invalid",
                                        "network");
            return true;
        }
        if (!s->network_tftp_data) {
            s->network_tftp_data = g_malloc(1);
        }
        s->network_artifacts[s->network_artifact_index].data =
            g_steal_pointer(&s->network_tftp_data);
        s->network_artifacts[s->network_artifact_index].size =
            s->network_tftp_size;
        s->network_tftp_size = 0;
        s->network_tftp_dally_server_port = s->network_tftp_server_port;
        s->network_tftp_dally_block = block;
        s->network_artifact_index++;
        raspi4_network_continue_after_artifact(s, true);
    }
    return true;
}

static bool raspi4_network_http_parse_header(Raspi4bMachineState *s,
                                             const char **error_state)
{
    const uint8_t marker[] = "\r\n\r\n";
    const uint8_t *end = NULL;
    g_autofree char *header = NULL;
    g_auto(GStrv) lines = NULL;
    uint64_t content_length = 0;
    bool have_length = false;
    size_t header_size;

    for (size_t i = 0; i + sizeof(marker) - 1 <=
         s->network_http_response_size; i++) {
        if (!memcmp(s->network_http_response + i, marker,
                    sizeof(marker) - 1)) {
            end = s->network_http_response + i;
            break;
        }
    }
    if (!end) {
        return s->network_http_response_size <= RASPI4_HTTP_HEADER_MAX;
    }
    header_size = end - s->network_http_response + sizeof(marker) - 1;
    header = g_strndup((const char *)s->network_http_response,
                       header_size - 2);
    lines = g_strsplit(header, "\r\n", -1);
    if (strcmp(lines[0], "HTTP/1.1 200 OK") &&
        strcmp(lines[0], "HTTP/1.0 200 OK")) {
        if (g_str_has_prefix(lines[0], "HTTP/1.1 301 ") ||
            g_str_has_prefix(lines[0], "HTTP/1.1 302 ") ||
            g_str_has_prefix(lines[0], "HTTP/1.1 303 ") ||
            g_str_has_prefix(lines[0], "HTTP/1.1 307 ") ||
            g_str_has_prefix(lines[0], "HTTP/1.1 308 ") ||
            g_str_has_prefix(lines[0], "HTTP/1.0 301 ") ||
            g_str_has_prefix(lines[0], "HTTP/1.0 302 ")) {
            *error_state = "network-http-redirect-rejected";
        }
        return false;
    }
    for (char **line = lines + 1; *line && **line; line++) {
        char *separator = strchr(*line, ':');
        uint64_t parsed;

        if (!separator) {
            return false;
        }
        *separator++ = 0;
        separator = g_strstrip(separator);
        if (!g_ascii_strcasecmp(*line, "Content-Length")) {
            if (have_length || qemu_strtou64(separator, NULL, 10,
                                             &parsed) < 0) {
                return false;
            }
            content_length = parsed;
            have_length = true;
        } else if (!g_ascii_strcasecmp(*line, "Transfer-Encoding")) {
            return false;
        }
    }
    if (!have_length || !content_length ||
        content_length > s->network_tftp_expected_size) {
        return false;
    }
    memmove(s->network_http_response,
            s->network_http_response + header_size,
            s->network_http_response_size - header_size);
    s->network_http_response_size -= header_size;
    s->network_http_content_length = content_length;
    s->network_http_header_parsed = true;
    return true;
}

static bool raspi4_network_http_complete(Raspi4bMachineState *s)
{
    Raspi4NetworkArtifact *artifact;

    if (s->network_artifact_index >= s->network_artifact_count) {
        return false;
    }
    if (s->network_http_tls &&
        qcrypto_tls_session_bye(s->network_tls_session, NULL) < 0) {
        raspi4_network_http_fail(s, "network-https-close-failed");
        return true;
    }
    if (!raspi4_network_send_tcp(s, 0x11, NULL, 0)) {
        return false;
    }
    s->network_http_client_seq++;
    artifact = &s->network_artifacts[s->network_artifact_index];
    artifact->data = g_steal_pointer(&s->network_http_response);
    artifact->size = s->network_http_response_size;
    s->network_http_response_size = 0;
    raspi4_network_tls_clear(s);
    s->network_artifact_index++;
    if (s->network_artifact_index < s->network_artifact_count) {
        if (!raspi4_network_select_artifact(s, true)) {
            return false;
        }
        return raspi4_network_http_start_connection(s);
    }
    timer_del(s->boot_timeout_timer);
    raspi4_network_tftp_cancel_retransmit(s);
    s->pending_boot_action = RASPI4_PENDING_NONE;
    s->network_http_state = RASPI4_HTTP_IDLE;
    bcm2711_genet_set_boot_client_active(raspi4_genet(s), false);
    if (raspi4_try_network_artifacts(s) == RASPI4_BOOT_ATTEMPT_READY) {
        raspi4_set_boot_observation(s, "arm-handoff-ready", "http");
        trace_raspi4b_boot_event("boot-source", "boot-source.http",
                                 "http", "success", artifact->size);
    }
    return true;
}

static void raspi4_network_http_fail(Raspi4bMachineState *s,
                                     const char *state)
{
    uint8_t index = s->boot_order_index;

    raspi4_network_tftp_cancel_retransmit(s);
    timer_del(s->boot_timeout_timer);
    s->pending_boot_action = RASPI4_PENDING_NONE;
    s->network_http_state = RASPI4_HTTP_IDLE;
    s->network_http_response_size = 0;
    s->network_http_content_length = 0;
    s->network_http_header_parsed = false;
    g_clear_pointer(&s->network_http_response, g_free);
    s->network_http_ooo_sequence = 0;
    s->network_http_ooo_size = 0;
    s->network_http_ooo_fin = false;
    s->network_http_ooo_fin_sequence = 0;
    g_clear_pointer(&s->network_http_ooo_data, g_free);
    g_clear_pointer(&s->network_http_ooo_valid, g_free);
    raspi4_network_tls_clear(s);
    raspi4_set_firmware_status(s, state);
    raspi4_set_boot_observation(s, state, "http");
    trace_raspi4b_boot_event("boot-source", "boot-source.http-error",
                             "http", state, s->network_artifact_index);
    if (s->net_boot_max_retries >= 0 &&
        s->network_attempt > s->net_boot_max_retries) {
        s->network_dhcp_phase = RASPI4_DHCP_PHASE_NONE;
        bcm2711_genet_set_boot_client_active(raspi4_genet(s), false);
        raspi4_execute_after_source_failure(s, index);
        return;
    }
    s->boot_retry_count++;
    raspi4_try_network_firmware(s);
}

static bool raspi4_network_http_plain_append(Raspi4bMachineState *s,
                                             const uint8_t *payload,
                                             size_t payload_size, bool fin)
{
    const char *header_error = "network-http-header-invalid";

    if (s->network_http_response_size >
        s->network_tftp_expected_size + RASPI4_HTTP_HEADER_MAX ||
        payload_size > s->network_tftp_expected_size +
                       RASPI4_HTTP_HEADER_MAX -
                       s->network_http_response_size) {
        raspi4_network_http_fail(s, "network-http-oversize");
        return true;
    }
    s->network_http_response = g_realloc(
        s->network_http_response,
        s->network_http_response_size + payload_size);
    memcpy(s->network_http_response + s->network_http_response_size,
           payload, payload_size);
    s->network_http_response_size += payload_size;
    if (!s->network_http_header_parsed &&
        !raspi4_network_http_parse_header(s, &header_error)) {
        raspi4_network_http_fail(s, header_error);
        return true;
    }
    if (s->network_http_header_parsed &&
        s->network_http_response_size == s->network_http_content_length) {
        if (fin) {
            s->network_http_server_seq++;
            raspi4_network_send_tcp(s, 0x10, NULL, 0);
        }
        return raspi4_network_http_complete(s);
    }
    if (s->network_http_header_parsed &&
        s->network_http_response_size > s->network_http_content_length) {
        raspi4_network_http_fail(s, "network-http-length-invalid");
        return true;
    }
    if (fin) {
        s->network_http_server_seq++;
        raspi4_network_send_tcp(s, 0x10, NULL, 0);
        raspi4_network_http_fail(s, "network-http-truncated");
    }
    return true;
}

static bool raspi4_network_http_append(Raspi4bMachineState *s,
                                       const uint8_t *payload,
                                       size_t payload_size, bool fin)
{
    size_t unread;

    if (!s->network_http_tls) {
        s->network_http_server_seq += payload_size;
        raspi4_network_send_tcp(s, 0x10, NULL, 0);
        s->network_tftp_file_retransmits = 0;
        raspi4_network_tftp_arm_retransmit(s);
        return raspi4_network_http_plain_append(
            s, payload, payload_size, fin);
    }
    unread = s->network_tls_rx_size - s->network_tls_rx_offset;
    if (unread > RASPI4_HTTP_OOO_MAX ||
        payload_size > RASPI4_HTTP_OOO_MAX - unread) {
        raspi4_network_http_fail(s, "network-https-record-oversize");
        return true;
    }
    if (s->network_tls_rx_offset && unread) {
        memmove(s->network_tls_rx,
                s->network_tls_rx + s->network_tls_rx_offset, unread);
    }
    s->network_tls_rx = g_realloc(s->network_tls_rx,
                                  unread + payload_size);
    memcpy(s->network_tls_rx + unread, payload, payload_size);
    s->network_tls_rx_offset = 0;
    s->network_tls_rx_size = unread + payload_size;
    s->network_http_server_seq += payload_size;
    raspi4_network_send_tcp(s, 0x10, NULL, 0);
    s->network_tftp_file_retransmits = 0;
    raspi4_network_tftp_arm_retransmit(s);
    if (!raspi4_network_tls_advance(s)) {
        return true;
    }
    if (fin) {
        s->network_http_server_seq++;
        raspi4_network_send_tcp(s, 0x10, NULL, 0);
        if (s->pending_boot_action == RASPI4_PENDING_NETWORK_HTTP) {
            raspi4_network_http_fail(s, "network-https-truncated");
        }
    }
    return true;
}

static bool raspi4_network_http_store_ooo(Raspi4bMachineState *s,
                                          uint32_t sequence,
                                          const uint8_t *payload,
                                          size_t payload_size, bool fin)
{
    uint32_t expected = s->network_http_server_seq;
    uint32_t start = sequence - expected;
    uint64_t end = (uint64_t)start + payload_size;
    uint32_t old_start;
    uint32_t old_end;
    uint32_t new_start;
    uint32_t new_end;
    uint32_t new_size;
    g_autofree uint8_t *new_data = NULL;
    g_autofree uint8_t *new_valid = NULL;

    if (!start || end > RASPI4_HTTP_OOO_MAX) {
        raspi4_network_send_tcp(s, 0x10, NULL, 0);
        return true;
    }
    if (!s->network_http_ooo_size) {
        old_start = start;
        old_end = start;
    } else {
        old_start = s->network_http_ooo_sequence - expected;
        old_end = old_start + s->network_http_ooo_size;
    }
    new_start = MIN(start, old_start);
    new_end = MAX((uint32_t)end, old_end);
    new_size = new_end - new_start;
    new_data = g_malloc(new_size);
    new_valid = g_malloc0(new_size);
    if (s->network_http_ooo_size) {
        uint32_t offset = old_start - new_start;

        memcpy(new_data + offset, s->network_http_ooo_data,
               s->network_http_ooo_size);
        memcpy(new_valid + offset, s->network_http_ooo_valid,
               s->network_http_ooo_size);
    }
    for (size_t i = 0; i < payload_size; i++) {
        uint32_t offset = start - new_start + i;

        if (!new_valid[offset]) {
            new_data[offset] = payload[i];
            new_valid[offset] = 1;
        }
    }
    g_free(s->network_http_ooo_data);
    g_free(s->network_http_ooo_valid);
    s->network_http_ooo_data = g_steal_pointer(&new_data);
    s->network_http_ooo_valid = g_steal_pointer(&new_valid);
    s->network_http_ooo_sequence = expected + new_start;
    s->network_http_ooo_size = new_size;
    if (fin) {
        uint32_t fin_sequence = sequence + payload_size;

        if (!s->network_http_ooo_fin ||
            (int32_t)(fin_sequence -
                      s->network_http_ooo_fin_sequence) < 0) {
            s->network_http_ooo_fin = true;
            s->network_http_ooo_fin_sequence = fin_sequence;
        }
    }
    raspi4_network_send_tcp(s, 0x10, NULL, 0);
    return true;
}

static bool raspi4_network_http_drain_ooo(Raspi4bMachineState *s)
{
    while (s->network_http_ooo_size &&
           (int32_t)(s->network_http_ooo_sequence -
                     s->network_http_server_seq) <= 0) {
        uint32_t duplicate = s->network_http_server_seq -
                             s->network_http_ooo_sequence;
        uint32_t available;
        uint32_t consumed;
        bool fin;
        g_autofree uint8_t *queued = NULL;

        if (duplicate >= s->network_http_ooo_size) {
            g_clear_pointer(&s->network_http_ooo_data, g_free);
            g_clear_pointer(&s->network_http_ooo_valid, g_free);
            s->network_http_ooo_sequence = 0;
            s->network_http_ooo_size = 0;
            break;
        }
        available = duplicate;
        while (available < s->network_http_ooo_size &&
               s->network_http_ooo_valid[available]) {
            available++;
        }
        available -= duplicate;
        if (!available) {
            if (duplicate) {
                memmove(s->network_http_ooo_data,
                        s->network_http_ooo_data + duplicate,
                        s->network_http_ooo_size - duplicate);
                memmove(s->network_http_ooo_valid,
                        s->network_http_ooo_valid + duplicate,
                        s->network_http_ooo_size - duplicate);
                s->network_http_ooo_sequence += duplicate;
                s->network_http_ooo_size -= duplicate;
            }
            break;
        }
        if (s->network_http_ooo_fin &&
            (int32_t)(s->network_http_ooo_fin_sequence -
                      s->network_http_server_seq) >= 0 &&
            (uint32_t)(s->network_http_ooo_fin_sequence -
                       s->network_http_server_seq) < available) {
            available = s->network_http_ooo_fin_sequence -
                        s->network_http_server_seq;
        }
        fin = s->network_http_ooo_fin &&
              s->network_http_ooo_fin_sequence ==
                  s->network_http_server_seq + available;
        queued = g_memdup2(s->network_http_ooo_data + duplicate, available);
        consumed = duplicate + available;
        memmove(s->network_http_ooo_data,
                s->network_http_ooo_data + consumed,
                s->network_http_ooo_size - consumed);
        memmove(s->network_http_ooo_valid,
                s->network_http_ooo_valid + consumed,
                s->network_http_ooo_size - consumed);
        s->network_http_ooo_sequence += consumed;
        s->network_http_ooo_size -= consumed;
        if (!s->network_http_ooo_size) {
            g_clear_pointer(&s->network_http_ooo_data, g_free);
            g_clear_pointer(&s->network_http_ooo_valid, g_free);
            s->network_http_ooo_sequence = 0;
        }
        if (fin) {
            s->network_http_ooo_fin = false;
            s->network_http_ooo_fin_sequence = 0;
        }
        if (!raspi4_network_http_append(s, queued, available, fin) ||
            s->pending_boot_action != RASPI4_PENDING_NETWORK_HTTP ||
            s->network_http_state != RASPI4_HTTP_RESPONSE) {
            return true;
        }
    }
    if (s->network_http_ooo_fin &&
        s->network_http_ooo_fin_sequence ==
            s->network_http_server_seq) {
        s->network_http_ooo_fin = false;
        s->network_http_ooo_fin_sequence = 0;
        s->network_http_server_seq++;
        raspi4_network_send_tcp(s, 0x10, NULL, 0);
        raspi4_network_http_fail(s, "network-http-truncated");
    }
    return true;
}

static bool raspi4_network_http_receive(Raspi4bMachineState *s,
                                        const uint8_t *packet, size_t size)
{
    const MACAddr *mac = bcm2711_genet_mac(raspi4_genet(s));
    const uint8_t *ip;
    const uint8_t *tcp;
    const uint8_t *payload;
    unsigned int ip_header_length;
    unsigned int tcp_header_length;
    uint16_t ip_length;
    uint16_t tcp_length;
    uint32_t sequence;
    uint32_t acknowledgement;
    uint8_t flags;
    size_t payload_size;

    if (size < 14 + 20 + 20 || lduw_be_p(packet + 12) != 0x0800 ||
        memcmp(packet, mac->a, sizeof(mac->a)) ||
        memcmp(packet + 6, s->network_server_mac,
               sizeof(s->network_server_mac))) {
        return true;
    }
    ip = packet + 14;
    ip_header_length = (ip[0] & 0xf) * 4;
    ip_length = lduw_be_p(ip + 2);
    if ((ip[0] >> 4) != 4 || ip_header_length < 20 || ip[9] != 6 ||
        (lduw_be_p(ip + 6) & 0x3fff) ||
        ip_length < ip_header_length + 20 || ip_length > size - 14 ||
        ldl_be_p(ip + 12) != s->network_server_ip ||
        ldl_be_p(ip + 16) != s->network_offered_ip ||
        net_raw_checksum((uint8_t *)ip, ip_header_length)) {
        return true;
    }
    tcp = ip + ip_header_length;
    tcp_length = ip_length - ip_header_length;
    tcp_header_length = (tcp[12] >> 4) * 4;
    if (lduw_be_p(tcp) != s->http_port ||
        lduw_be_p(tcp + 2) != s->network_http_client_port ||
        tcp_header_length < 20 || tcp_header_length > tcp_length ||
        net_checksum_tcpudp(tcp_length, 6, (uint8_t *)ip + 12,
                            (uint8_t *)tcp)) {
        return true;
    }
    sequence = ldl_be_p(tcp + 4);
    acknowledgement = ldl_be_p(tcp + 8);
    flags = tcp[13];
    payload = tcp + tcp_header_length;
    payload_size = tcp_length - tcp_header_length;
    if (flags & 0x04) {
        raspi4_network_http_fail(s, "network-http-reset");
        return true;
    }
    if (s->network_http_state == RASPI4_HTTP_SYN_SENT) {
        if ((flags & 0x12) != 0x12 || acknowledgement !=
            s->network_http_client_seq) {
            return true;
        }
        s->network_http_server_seq = sequence + 1;
        s->network_http_state = RASPI4_HTTP_RESPONSE;
        if (s->network_http_tls) {
            if (!raspi4_network_tls_start(s)) {
                return true;
            }
        } else if (!raspi4_network_http_send_request(s, true)) {
            return true;
        }
        s->network_tftp_file_retransmits = 0;
        raspi4_network_tftp_arm_retransmit(s);
        raspi4_set_boot_observation(s, "network-http-response", "http");
        return true;
    }
    if (s->network_http_state != RASPI4_HTTP_RESPONSE) {
        return true;
    }
    if (s->network_http_tls ?
        !raspi4_network_tls_ack(s, acknowledgement) :
        acknowledgement != s->network_http_client_seq) {
        return true;
    }
    if (payload_size) {
        int32_t delta = sequence - s->network_http_server_seq;

        if (delta > 0) {
            return raspi4_network_http_store_ooo(
                s, sequence, payload, payload_size, flags & 0x01);
        }
        if (delta < 0) {
            size_t duplicate = s->network_http_server_seq - sequence;

            if (duplicate >= payload_size) {
                if ((flags & 0x01) && duplicate == payload_size) {
                    s->network_http_server_seq++;
                    raspi4_network_send_tcp(s, 0x10, NULL, 0);
                    raspi4_network_http_fail(s, "network-http-truncated");
                    return true;
                }
                raspi4_network_send_tcp(s, 0x10, NULL, 0);
                return true;
            }
            payload += duplicate;
            payload_size -= duplicate;
        }
        if (!raspi4_network_http_append(s, payload, payload_size,
                                        flags & 0x01) ||
            s->pending_boot_action != RASPI4_PENDING_NETWORK_HTTP ||
            s->network_http_state != RASPI4_HTTP_RESPONSE) {
            return true;
        }
        return raspi4_network_http_drain_ooo(s);
    } else if (flags & 0x01) {
        int32_t delta = sequence - s->network_http_server_seq;

        if (delta > 0 && (uint32_t)delta <= RASPI4_HTTP_OOO_MAX) {
            if (!s->network_http_ooo_fin ||
                (int32_t)(sequence -
                          s->network_http_ooo_fin_sequence) < 0) {
                s->network_http_ooo_fin = true;
                s->network_http_ooo_fin_sequence = sequence;
            }
            raspi4_network_send_tcp(s, 0x10, NULL, 0);
        } else if (!delta) {
            s->network_http_server_seq++;
            raspi4_network_send_tcp(s, 0x10, NULL, 0);
            raspi4_network_http_fail(s, "network-http-truncated");
        }
    }
    return true;
}


static bool raspi4_dhcp_option_u32(bool *seen, uint32_t *current,
                                   uint32_t value)
{
    if (*seen && *current != value) {
        return false;
    }
    *seen = true;
    *current = value;
    return true;
}

bool raspi4_dns_hostname(const uint8_t *value, size_t length,
                                char output[RASPI4_DNS_NAME_MAX + 1])
{
    size_t label_start = 0;

    if (length && !value[length - 1]) {
        length--;
    }
    if (length && value[length - 1] == '.') {
        length--;
    }
    if (!length || length > RASPI4_DNS_NAME_MAX || memchr(value, 0, length)) {
        return false;
    }
    for (size_t i = 0; i <= length; i++) {
        if (i == length || value[i] == '.') {
            size_t label_length = i - label_start;

            if (!label_length || label_length > 63 ||
                value[label_start] == '-' || value[i - 1] == '-') {
                return false;
            }
            label_start = i + 1;
        } else if (!g_ascii_isalnum(value[i]) && value[i] != '-') {
            return false;
        }
    }
    memcpy(output, value, length);
    output[length] = 0;
    return true;
}

static bool raspi4_dhcp_parse_options(const uint8_t *cursor,
                                      const uint8_t *end,
                                      Raspi4DhcpOptions *options,
                                      bool primary)
{
    while (cursor < end) {
        uint8_t code = *cursor++;
        uint8_t length;

        if (!code) {
            continue;
        }
        if (code == 255) {
            return true;
        }
        if (cursor >= end) {
            trace_raspi4b_boot_event("boot-source", "boot-source.dhcp-option",
                                     "network", "missing-length", code);
            return false;
        }
        length = *cursor++;
        if (length > end - cursor) {
            trace_raspi4b_boot_event("boot-source", "boot-source.dhcp-option",
                                     "network", "truncated", code);
            return false;
        }
        switch (code) {
        case 1: {
            uint32_t subnet = length == 4 ? ldl_be_p(cursor) : 0;
            uint32_t inverse = ~subnet;

            if (length != 4 || !subnet ||
                (inverse & (inverse + 1)) ||
                !raspi4_dhcp_option_u32(
                    &options->subnet_seen, &options->subnet,
                    subnet)) {
                trace_raspi4b_boot_event(
                    "boot-source", "boot-source.dhcp-option", "network",
                    "invalid", code);
                return false;
            }
            break;
        }
        case 3:
            if (length < 4 || length % 4 ||
                !raspi4_ipv4_is_unicast(ldl_be_p(cursor)) ||
                !raspi4_dhcp_option_u32(
                    &options->gateway_seen, &options->gateway,
                    ldl_be_p(cursor))) {
                trace_raspi4b_boot_event(
                    "boot-source", "boot-source.dhcp-option", "network",
                    "invalid", code);
                return false;
            }
            break;
        case 6:
            if (length < 4 || length % 4 ||
                !raspi4_ipv4_is_unicast(ldl_be_p(cursor)) ||
                !raspi4_dhcp_option_u32(
                    &options->dns_server_seen, &options->dns_server,
                    ldl_be_p(cursor))) {
                trace_raspi4b_boot_event(
                    "boot-source", "boot-source.dhcp-option", "network",
                    "invalid", code);
                return false;
            }
            break;
        case 43:
            if (!length ||
                (options->pxe_option43_seen &&
                 (options->pxe_option43_length != length ||
                  memcmp(options->pxe_option43, cursor, length)))) {
                trace_raspi4b_boot_event(
                    "boot-source", "boot-source.dhcp-option", "network",
                    options->pxe_option43_seen ? "conflict" : "invalid",
                    code);
                return false;
            }
            memcpy(options->pxe_option43, cursor, length);
            options->pxe_option43_length = length;
            options->pxe_option43_seen = true;
            break;
        case 52:
            if (!primary || length != 1 || !cursor[0] || cursor[0] > 3 ||
                (options->overload_seen &&
                 options->overload != cursor[0])) {
                trace_raspi4b_boot_event(
                    "boot-source", "boot-source.dhcp-option", "network",
                    "invalid", code);
                return false;
            }
            options->overload_seen = true;
            options->overload = cursor[0];
            break;
        case 53:
            if (length != 1 || !cursor[0] || cursor[0] > 8 ||
                (options->message_seen &&
                 options->message != cursor[0])) {
                trace_raspi4b_boot_event(
                    "boot-source", "boot-source.dhcp-option", "network",
                    "invalid", code);
                return false;
            }
            options->message_seen = true;
            options->message = cursor[0];
            break;
        case 54:
            if (length != 4 ||
                !raspi4_ipv4_is_unicast(ldl_be_p(cursor)) ||
                !raspi4_dhcp_option_u32(
                    &options->server_seen, &options->server,
                    ldl_be_p(cursor))) {
                trace_raspi4b_boot_event(
                    "boot-source", "boot-source.dhcp-option", "network",
                    "invalid", code);
                return false;
            }
            break;
        case 66: {
            size_t string_length = length;
            char hostname[RASPI4_DNS_NAME_MAX + 1];

            if (string_length && !cursor[string_length - 1]) {
                string_length--;
            }
            if (!string_length || memchr(cursor, 0, string_length)) {
                trace_raspi4b_boot_event(
                    "boot-source", "boot-source.dhcp-option", "network",
                    "invalid", code);
                return false;
            }
            if (!raspi4_dns_hostname(cursor, length, hostname) ||
                (options->tftp_hostname_seen &&
                 g_ascii_strcasecmp(options->tftp_hostname, hostname))) {
                trace_raspi4b_boot_event(
                    "boot-source", "boot-source.dhcp-option", "network",
                    "invalid", code);
                return false;
            }
            options->tftp_hostname_seen = true;
            pstrcpy(options->tftp_hostname, sizeof(options->tftp_hostname),
                    hostname);
            if (string_length < INET_ADDRSTRLEN) {
                char text[INET_ADDRSTRLEN];
                uint32_t address;

                memcpy(text, cursor, string_length);
                text[string_length] = 0;
                if (raspi4_parse_ipv4(text, &address) &&
                    !raspi4_dhcp_option_u32(
                        &options->tftp_server_seen,
                        &options->tftp_server, address)) {
                    trace_raspi4b_boot_event(
                        "boot-source", "boot-source.dhcp-option", "network",
                        "conflict", code);
                    return false;
                }
            }
            break;
        }
        case 67: {
            size_t string_length = length;

            if (string_length && !cursor[string_length - 1]) {
                string_length--;
            }
            if (!string_length || memchr(cursor, 0, string_length) ||
                (options->bootfile_seen &&
                 (strlen(options->bootfile) != string_length ||
                  memcmp(options->bootfile, cursor, string_length)))) {
                trace_raspi4b_boot_event(
                    "boot-source", "boot-source.dhcp-option", "network",
                    options->bootfile_seen ? "conflict" : "invalid", code);
                return false;
            }
            memcpy(options->bootfile, cursor, string_length);
            options->bootfile[string_length] = 0;
            options->bootfile_seen = true;
            break;
        }
        default:
            break;
        }
        cursor += length;
    }
    return true;
}

bool raspi4_network_dhcp_receive(void *opaque, const uint8_t *packet,
                                        size_t size)
{
    Raspi4bMachineState *s = opaque;
    BCM2711GenetState *genet = raspi4_genet(s);
    const MACAddr *mac = bcm2711_genet_mac(genet);
    const uint8_t *ip;
    const uint8_t *udp;
    const uint8_t *bootp;
    Raspi4DhcpOptions options = { 0 };
    unsigned int ip_header_length;
    uint16_t ip_length;
    uint16_t udp_length;

    if (!s->network_boot_wire) {
        return false;
    }
    if (s->pending_boot_action == RASPI4_PENDING_NETWORK_ARP) {
        return raspi4_network_arp_receive(s, packet, size);
    }
    if (s->pending_boot_action == RASPI4_PENDING_NETWORK_DNS) {
        return raspi4_network_dns_receive(s, packet, size);
    }
    if (s->pending_boot_action == RASPI4_PENDING_NETWORK_TFTP ||
        s->pending_boot_action == RASPI4_PENDING_NETWORK_TFTP_DALLY) {
        return raspi4_network_tftp_receive(s, packet, size);
    }
    if (s->pending_boot_action == RASPI4_PENDING_NETWORK_HTTP) {
        return raspi4_network_http_receive(s, packet, size);
    }
    if (s->pending_boot_action != RASPI4_PENDING_NETWORK_DHCP) {
        return false;
    }
    if (size < 14 + 20 + 8 + 240 || lduw_be_p(packet + 12) != 0x0800) {
        trace_raspi4b_boot_event("boot-source", "boot-source.dhcp-rx",
                                 "network", "frame-rejected", size);
        return true;
    }
    ip = packet + 14;
    ip_header_length = (ip[0] & 0xf) * 4;
    ip_length = lduw_be_p(ip + 2);
    if ((ip[0] >> 4) != 4 || ip_header_length < 20 || ip[9] != 17 ||
        (lduw_be_p(ip + 6) & 0x3fff) ||
        ip_length < ip_header_length + 8 + 240 ||
        ip_length > size - 14 ||
        net_raw_checksum((uint8_t *)ip, ip_header_length)) {
        trace_raspi4b_boot_event("boot-source", "boot-source.dhcp-rx",
                                 "network", "ip-rejected", ip_length);
        return true;
    }
    udp = ip + ip_header_length;
    udp_length = lduw_be_p(udp + 4);
    if (lduw_be_p(udp + 2) != 68 || udp_length < 8 + 240 ||
        udp_length > ip_length - ip_header_length ||
        (lduw_be_p(udp + 6) &&
         net_checksum_tcpudp(udp_length, 17, (uint8_t *)ip + 12,
                              (uint8_t *)udp))) {
        trace_raspi4b_boot_event("boot-source", "boot-source.dhcp-rx",
                                 "network", "udp-rejected", udp_length);
        return true;
    }
    bootp = udp + 8;
    if (bootp[0] != 2 || bootp[1] != 1 || bootp[2] != 6 ||
        ldl_be_p(bootp + 4) != s->network_dhcp_xid ||
        memcmp(bootp + 28, mac->a, sizeof(mac->a)) ||
        ldl_be_p(bootp + 236) != 0x63825363) {
        trace_raspi4b_boot_event("boot-source", "boot-source.dhcp-rx",
                                 "network", "bootp-rejected",
                                 ldl_be_p(bootp + 4));
        return true;
    }
    if (!raspi4_dhcp_parse_options(bootp + 240, udp + udp_length,
                                   &options, true) ||
        ((options.overload & 1) &&
         !raspi4_dhcp_parse_options(bootp + 108, bootp + 236,
                                    &options, false)) ||
        ((options.overload & 2) &&
         !raspi4_dhcp_parse_options(bootp + 44, bootp + 108,
                                    &options, false))) {
        trace_raspi4b_boot_event("boot-source", "boot-source.dhcp-rx",
                                 "network", "options-rejected", 0);
        return true;
    }

    if (options.message == 2 && !ldl_be_p(bootp + 16)) {
        uint32_t proxy_tftp_server = ldl_be_p(bootp + 20);

        if (!raspi4_pxe_option43_matches(s, &options)) {
            trace_raspi4b_boot_event(
                "boot-source", "boot-source.dhcp-option", "network",
                "pxe-mismatch", 43);
            return true;
        }
        if (!raspi4_ipv4_is_unicast(proxy_tftp_server)) {
            proxy_tftp_server = options.tftp_server;
        }
        if (raspi4_ipv4_is_unicast(proxy_tftp_server)) {
            s->network_proxy_tftp_ip = proxy_tftp_server;
            if (s->network_dhcp_phase == RASPI4_DHCP_PHASE_REQUEST) {
                s->network_server_ip = proxy_tftp_server;
            }
        }
        return true;
    }

    if (s->network_dhcp_phase == RASPI4_DHCP_PHASE_DISCOVER &&
        options.message == 2) {
        s->network_offered_ip = ldl_be_p(bootp + 16);
        s->network_dhcp_server_ip = options.server;
        s->network_server_ip = ldl_be_p(bootp + 20);
        if (!raspi4_ipv4_is_unicast(s->network_server_ip)) {
            s->network_server_ip = options.tftp_server;
        }
        s->network_dns_server_ip = options.dns_server;
        if (options.tftp_hostname_seen) {
            pstrcpy((char *)s->network_tftp_hostname,
                    sizeof(s->network_tftp_hostname),
                    options.tftp_hostname);
        } else {
            s->network_tftp_hostname[0] = 0;
        }
        if (!raspi4_ipv4_is_unicast(s->network_server_ip) &&
            (!s->network_tftp_hostname[0] ||
             options.tftp_server_seen)) {
            s->network_server_ip = s->network_dhcp_server_ip;
        }
        if (raspi4_ipv4_is_unicast(s->network_proxy_tftp_ip)) {
            s->network_server_ip = s->network_proxy_tftp_ip;
        }
        s->network_subnet = options.subnet;
        s->network_gateway = options.gateway;
        if (!raspi4_ipv4_is_unicast(s->network_offered_ip) ||
            !raspi4_ipv4_is_unicast(s->network_dhcp_server_ip) ||
            (!raspi4_ipv4_is_unicast(s->network_server_ip) &&
             (!s->network_tftp_hostname[0] ||
              !raspi4_ipv4_is_unicast(s->network_dns_server_ip)))) {
            trace_raspi4b_boot_event("boot-source", "boot-source.dhcp-rx",
                                     "network", "offer-rejected",
                                     s->network_offered_ip);
            return true;
        }
        trace_raspi4b_boot_event("boot-source", "boot-source.dhcp-rx",
                                 "network", "offer", s->network_offered_ip);
        s->network_dhcp_phase = RASPI4_DHCP_PHASE_REQUEST;
        raspi4_set_boot_observation(s, "network-dhcp-request-wait",
                                    "network");
        raspi4_network_dhcp_cancel_retransmit(s);
        raspi4_network_send_dhcp(s, true);
        raspi4_network_dhcp_arm_retransmit(s);
        return true;
    }
    if (s->network_dhcp_phase == RASPI4_DHCP_PHASE_REQUEST &&
        options.message == 6 && options.server &&
        options.server == s->network_dhcp_server_ip) {
        raspi4_network_dhcp_cancel_retransmit(s);
        s->network_dhcp_phase = RASPI4_DHCP_PHASE_DISCOVER;
        s->network_dhcp_xid++;
        s->network_offered_ip = 0;
        s->network_dhcp_server_ip = 0;
        s->network_proxy_tftp_ip = 0;
        s->network_server_ip = 0;
        s->network_subnet = 0;
        s->network_gateway = 0;
        s->network_next_hop_ip = 0;
        s->network_dns_server_ip = 0;
        s->network_dns_query_id = 0;
        s->network_arp_for_dns = false;
        s->network_tftp_hostname[0] = 0;
        raspi4_set_boot_observation(s, "network-dhcp-nak-restart",
                                    "network");
        trace_raspi4b_boot_event("boot-source", "boot-source.dhcp",
                                 "network", "nak-restart",
                                 s->network_dhcp_xid);
        raspi4_network_send_dhcp(s, false);
        raspi4_network_dhcp_arm_retransmit(s);
        return true;
    }
    if (s->network_dhcp_phase == RASPI4_DHCP_PHASE_REQUEST &&
        options.message == 5 &&
        options.server == s->network_dhcp_server_ip &&
        ldl_be_p(bootp + 16) == s->network_offered_ip) {
        uint32_t acknowledged_tftp_server = ldl_be_p(bootp + 20);

        if (options.bootfile_seen) {
            /* Pi 4/CM4 DHCP uses PXE discovery but not its bootfile name. */
            trace_raspi4b_boot_event(
                "boot-source", "boot-source.dhcp-option", "network",
                "ignored", 67);
        }

        if (!raspi4_ipv4_is_unicast(acknowledged_tftp_server)) {
            acknowledged_tftp_server = options.tftp_server;
        }
        if (raspi4_ipv4_is_unicast(s->network_proxy_tftp_ip)) {
            s->network_server_ip = s->network_proxy_tftp_ip;
        } else if (raspi4_ipv4_is_unicast(acknowledged_tftp_server)) {
            s->network_server_ip = acknowledged_tftp_server;
        }
        if (options.subnet) {
            s->network_subnet = options.subnet;
        }
        if (options.gateway) {
            s->network_gateway = options.gateway;
        }
        if (options.dns_server) {
            s->network_dns_server_ip = options.dns_server;
        }
        if (options.tftp_hostname_seen) {
            pstrcpy((char *)s->network_tftp_hostname,
                    sizeof(s->network_tftp_hostname),
                    options.tftp_hostname);
            if (!options.tftp_server_seen &&
                !raspi4_ipv4_is_unicast(acknowledged_tftp_server) &&
                !raspi4_ipv4_is_unicast(s->network_proxy_tftp_ip)) {
                s->network_server_ip = 0;
            }
        }
        raspi4_network_dhcp_cancel_retransmit(s);
        timer_del(s->boot_timeout_timer);
        s->pending_boot_action = RASPI4_PENDING_NONE;
        s->network_dhcp_phase = RASPI4_DHCP_PHASE_NONE;
        if (s->network_http_mode) {
            if (raspi4_network_begin_http(s)) {
                trace_raspi4b_boot_event(
                    "boot-source", "boot-source.http", "http",
                    "network-ready", s->network_offered_ip);
                return true;
            }
        } else if (!raspi4_ipv4_is_unicast(s->network_server_ip) &&
            s->network_tftp_hostname[0] &&
            raspi4_ipv4_is_unicast(s->network_dns_server_ip)) {
            if (raspi4_network_begin_dns(s)) {
                trace_raspi4b_boot_event(
                    "boot-source", "boot-source.dhcp", "network",
                    "dns", s->network_dns_server_ip);
                return true;
            }
        } else if (raspi4_network_begin_tftp(s)) {
            trace_raspi4b_boot_event("boot-source", "boot-source.dhcp",
                                     "network", "ack",
                                     s->network_offered_ip);
            return true;
        }
        raspi4_schedule_network_wait(s, false);
        return true;
    }
    return true;
}

static bool raspi4_network_wait_pending(const Raspi4bMachineState *s)
{
    return s->pending_boot_action == RASPI4_PENDING_NETWORK_DHCP ||
           s->pending_boot_action == RASPI4_PENDING_NETWORK_TFTP ||
           s->pending_boot_action == RASPI4_PENDING_NETWORK_ARP ||
           s->pending_boot_action == RASPI4_PENDING_NETWORK_DNS ||
           s->pending_boot_action == RASPI4_PENDING_NETWORK_HTTP ||
           s->pending_boot_action == RASPI4_PENDING_NETWORK_TFTP_DALLY;
}

void raspi4_network_hotplug_bh(void *opaque)
{
    Raspi4bMachineState *s = opaque;
    bool inserted;

    s->network_boot_hotplug_pending = false;
    if (!raspi4_network_wait_pending(s)) {
        return;
    }
    if (s->network_boot_wire) {
        return;
    }
    inserted = s->network_boot && blk_is_inserted(s->network_boot);
    if (!inserted &&
        s->pending_boot_action == RASPI4_PENDING_NETWORK_DHCP) {
        return;
    }
    timer_del(s->boot_timeout_timer);
    s->pending_boot_action = RASPI4_PENDING_NONE;
    if (inserted) {
        Raspi4BootAttemptResult result =
            raspi4_try_firmware(s, s->network_boot, "network");

        if (result == RASPI4_BOOT_ATTEMPT_PENDING) {
            return;
        }
        if (result == RASPI4_BOOT_ATTEMPT_READY) {
            raspi4_set_boot_observation(s, "arm-handoff-ready", "network");
            trace_raspi4b_boot_event(
                "boot-source", "boot-source.hotplug",
                "network", "success", s->firmware_size);
            return;
        }
    }
    trace_raspi4b_boot_event("boot-source", "boot-source.hotplug",
                             "network", inserted ? "tftp-wait" :
                                                   "removed",
                             inserted ? s->tftp_file_timeout :
                                        s->dhcp_timeout);
    raspi4_schedule_network_wait(s, !inserted);
}

void raspi4_network_media_inserted(Notifier *notifier, void *data)
{
    Raspi4bMachineState *s = container_of(
        notifier, Raspi4bMachineState, network_boot_insert_notifier);

    if (data == s->network_boot) {
        s->network_boot_hotplug_pending = true;
        qemu_bh_schedule(s->network_boot_hotplug_bh);
    }
}

void raspi4_network_media_removed(Notifier *notifier, void *data)
{
    Raspi4bMachineState *s = container_of(
        notifier, Raspi4bMachineState, network_boot_remove_notifier);

    if (data == s->network_boot) {
        s->network_boot_hotplug_pending = true;
        qemu_bh_schedule(s->network_boot_hotplug_bh);
    }
}

void raspi4_netconsole_link_changed(void *opaque, bool link_up)
{
    Raspi4bMachineState *s = opaque;

    if (!link_up ||
        s->pending_boot_action != RASPI4_PENDING_NETCONSOLE_LINK) {
        return;
    }
    timer_del(s->boot_timeout_timer);
    s->pending_boot_action = RASPI4_PENDING_NONE;
    raspi4_netconsole_begin(s);
    raspi4_continue_after_eeprom_config(s);
}
