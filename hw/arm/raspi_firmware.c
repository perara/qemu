/*
 * Raspberry Pi behavioral firmware configuration
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/units.h"
#include "hw/arm/raspi_firmware.h"

#define RASPI_CONFIG_FILE_MAX (1 * MiB)
#define RASPI_CONFIG_LINE_MAX 98
#define RASPI_CONFIG_INCLUDE_MAX 8
/*
 * The include depth limit alone does not bound total work: sibling includes
 * and fan-out are re-parsed once the active path is unwound, so the whole
 * configuration also needs cumulative include, byte, and line budgets.
 */
#define RASPI_CONFIG_TOTAL_INCLUDE_MAX 64
#define RASPI_CONFIG_TOTAL_BYTES_MAX (8 * MiB)
#define RASPI_CONFIG_TOTAL_LINES_MAX 65536

typedef struct RaspiConfigFilterState {
    bool model;
    bool edid;
    bool serial;
    bool gpio;
    bool expression;
    bool other;
    bool disabled;
} RaspiConfigFilterState;

typedef struct RaspiConfigParser {
    RaspiFatVolume *volume;
    RaspiFirmwareReadPath *read_path;
    void *read_opaque;
    RaspiFirmwareConfig *config;
    GHashTable *include_paths;
    RaspiConfigFilterState filters;
    uint64_t parsed_bytes;
} RaspiConfigParser;

static void raspi_replace_string(char **target, const char *value)
{
    g_free(*target);
    *target = g_strdup(value);
}

void raspi_firmware_config_init(RaspiFirmwareConfig *config, bool cm4)
{
    memset(config, 0, sizeof(*config));
    config->cm4 = cm4;
    config->bootvar0 = 0;
    config->arm_64bit = true;
    config->device_tree_enabled = true;
    config->force_eeprom_read = true;
    config->start_file = g_strdup("start4.elf");
    config->fixup_file = g_strdup("fixup4.dat");
    config->kernel_file = g_strdup("kernel8.img");
    config->device_tree_file = g_strdup(cm4 ? "bcm2711-rpi-cm4.dtb" :
                                             "bcm2711-rpi-4-b.dtb");
    config->cmdline_file = g_strdup("cmdline.txt");
    config->initramfs_address = g_strdup("followkernel");
    config->os_prefix = g_strdup("");
    config->overlay_prefix = g_strdup("overlays/");
    config->overlays = g_ptr_array_new_with_free_func(g_free);
    config->dtparams = g_ptr_array_new_with_free_func(g_free);
    config->dt_commands = g_ptr_array_new_with_free_func(g_free);
    config->include_files = g_ptr_array_new_with_free_func(g_free);
}

void raspi_firmware_config_clear(RaspiFirmwareConfig *config)
{
    g_free(config->start_file);
    g_free(config->fixup_file);
    g_free(config->kernel_file);
    g_free(config->device_tree_file);
    g_free(config->cmdline_file);
    g_free(config->initramfs_file);
    g_free(config->initramfs_address);
    g_free(config->os_prefix);
    g_free(config->overlay_prefix);
    g_ptr_array_unref(config->overlays);
    g_ptr_array_unref(config->dtparams);
    g_ptr_array_unref(config->dt_commands);
    g_ptr_array_unref(config->include_files);
    memset(config, 0, sizeof(*config));
}

static char *raspi_normalize_path(const char *base, const char *path,
                                  Error **errp)
{
    g_autofree char *combined = NULL;
    g_auto(GStrv) components = NULL;
    g_autoptr(GPtrArray) output = g_ptr_array_new();
    GString *normalized;

    if (!path[0]) {
        error_setg(errp, "Raspberry Pi boot path is empty");
        return NULL;
    }
    if (path[0] == '/') {
        combined = g_strdup(path + 1);
    } else if (base && base[0]) {
        combined = g_strdup_printf("%s/%s", base, path);
    } else {
        combined = g_strdup(path);
    }
    components = g_strsplit(combined, "/", -1);
    for (char **component = components; *component; component++) {
        if (!(*component)[0] || !strcmp(*component, ".")) {
            continue;
        }
        if (!strcmp(*component, "..")) {
            if (!output->len) {
                error_setg(errp, "Raspberry Pi boot path escapes its volume");
                return NULL;
            }
            g_ptr_array_remove_index(output, output->len - 1);
            continue;
        }
        g_ptr_array_add(output, *component);
    }
    if (!output->len) {
        error_setg(errp, "Raspberry Pi boot path does not name a file");
        return NULL;
    }
    normalized = g_string_new(NULL);
    for (unsigned int i = 0; i < output->len; i++) {
        if (i) {
            g_string_append_c(normalized, '/');
        }
        g_string_append(normalized, g_ptr_array_index(output, i));
    }
    return g_string_free(normalized, false);
}

static char *raspi_path_directory(const char *path)
{
    const char *slash = strrchr(path, '/');

    return slash ? g_strndup(path, slash - path) : g_strdup("");
}

static uint8_t *raspi_read_boot_file(RaspiFatVolume *volume,
                                     const char *path, size_t maximum,
                                     size_t *length, RaspiFatFile *file,
                                     RaspiFatResult *result, Error **errp)
{
    *result = raspi_fat_find_path(volume, path, file, errp);
    if (*result != RASPI_FAT_FOUND) {
        return NULL;
    }
    return raspi_fat_read_file(volume, file, maximum, length, errp);
}

static bool raspi_parse_bool(const char *value, bool *result)
{
    uint64_t parsed;

    if (qemu_strtou64(value, NULL, 0, &parsed) < 0 || parsed > 1) {
        return false;
    }
    *result = parsed;
    return true;
}

static bool raspi_config_variable(const RaspiFirmwareConfig *config,
                                  const char *name, size_t length,
                                  uint32_t *value)
{
    if (length == strlen("bootvar0") &&
        !memcmp(name, "bootvar0", length)) {
        *value = config->bootvar0;
        return true;
    }
    if (length == strlen("boot_partition") &&
        !memcmp(name, "boot_partition", length)) {
        *value = config->boot_partition;
        return true;
    }
    if (length == strlen("partition") &&
        !memcmp(name, "partition", length)) {
        *value = config->partition;
        return true;
    }
    if (length == strlen("cust_otp0") &&
        !memcmp(name, "cust_otp", strlen("cust_otp")) &&
        name[length - 1] >= '0' && name[length - 1] <= '7') {
        *value = config->customer_otp[name[length - 1] - '0'];
        return true;
    }
    return false;
}

static bool raspi_config_parse_u32(const char *text, size_t length,
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

static bool raspi_config_parse_u32_value(const char *text, uint32_t *value)
{
    return raspi_config_parse_u32(text, strlen(text), value);
}

static bool raspi_config_parse_u64_value(const char *text, uint64_t *value)
{
    const char *end;

    return text[0] && text[0] != '-' &&
           qemu_strtou64(text, &end, 0, value) == 0 && !*end;
}

static bool raspi_config_filter_expression(
    const RaspiFirmwareConfig *config, const char *filter, bool *match)
{
    g_autofree char *expression = NULL;
    const char *amp;
    const char *equal;
    const char *less;
    const char *greater;
    const char *operator;
    uint32_t argument;
    uint32_t operand;
    size_t length = strlen(filter);

    *match = false;
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
            !raspi_config_variable(config, expression, amp - expression,
                                   &argument)) {
            return true;
        }
        if (equal) {
            if (!raspi_config_parse_u32(mask, equal - mask, &mask_value) ||
                !raspi_config_parse_u32(equal + 1, strlen(equal + 1),
                                        &operand)) {
                return true;
            }
            *match = (argument & mask_value) == operand;
        } else {
            if (!raspi_config_parse_u32(mask, strlen(mask), &mask_value)) {
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
        !raspi_config_variable(config, expression, operator - expression,
                               &argument) ||
        !raspi_config_parse_u32(operator + 1, strlen(operator + 1),
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

static bool raspi_config_filters_active(
    const RaspiConfigFilterState *filters)
{
    return !filters->disabled && filters->model && filters->edid &&
           filters->serial &&
           filters->gpio && filters->expression && filters->other;
}

static void raspi_config_filters_reset(RaspiConfigFilterState *filters)
{
    *filters = (RaspiConfigFilterState) {
        .model = true,
        .edid = true,
        .serial = true,
        .gpio = true,
        .expression = true,
        .other = true,
    };
}

static void raspi_config_filter_apply(RaspiConfigParser *parser,
                                      const char *filter)
{
    RaspiConfigFilterState *filters = &parser->filters;
    RaspiFirmwareConfig *config = parser->config;
    bool expression_match;
    uint64_t value;

    if (!g_ascii_strcasecmp(filter, "[all]")) {
        raspi_config_filters_reset(filters);
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
        filters->model = config->cm4;
        return;
    }
    if (!g_ascii_strncasecmp(filter, "[board-type=", 12)) {
        const char *end;

        filters->model =
            qemu_strtou64(filter + 12, &end, 0, &value) == 0 &&
            value <= UINT8_MAX && !strcmp(end, "]") &&
            config->board_type == value;
        return;
    }
    if (!g_ascii_strncasecmp(filter, "[pi", 3) ||
        !g_ascii_strncasecmp(filter, "[cm", 3)) {
        filters->model = false;
        return;
    }
    if (!g_ascii_strncasecmp(filter, "[EDID=", 6)) {
        size_t length = strlen(filter);

        filters->edid = false;
        if (length > 7 && filter[length - 1] == ']') {
            g_autofree char *name = g_strndup(filter + 6, length - 7);

            for (unsigned int port = 0;
                 port < RASPI_FIRMWARE_MAX_EDIDS; port++) {
                if ((config->edid_valid_mask & BIT(port)) &&
                    !strcmp(config->edid_names[port], name)) {
                    filters->edid = true;
                    break;
                }
            }
        }
        return;
    }
    if (!g_ascii_strncasecmp(filter, "[0x", 3)) {
        const char *end;

        filters->serial =
            qemu_strtou64(filter + 1, &end, 0, &value) == 0 &&
            value <= UINT32_MAX && !strcmp(end, "]") &&
            config->serial == value;
        return;
    }
    if (!g_ascii_strncasecmp(filter, "[gpio", 5)) {
        const char *equals = strchr(filter + 5, '=');
        g_autofree char *pin_text = equals ?
            g_strndup(filter + 5, equals - filter - 5) : NULL;
        uint64_t pin = 64;
        uint64_t level = 2;
        const char *pin_end = NULL;
        const char *end = NULL;

        filters->gpio =
            pin_text &&
            qemu_strtou64(pin_text, &pin_end, 10, &pin) == 0 &&
            !*pin_end &&
            equals && qemu_strtou64(equals + 1, &end, 10, &level) == 0 &&
            pin < 64 && level <= 1 && !strcmp(end, "]") &&
            (config->gpio_known_mask & BIT_ULL(pin)) &&
            !!(config->gpio_level_mask & BIT_ULL(pin)) == !!level;
        return;
    }
    if (!g_ascii_strcasecmp(filter, "[tryboot]")) {
        filters->other = config->tryboot;
        return;
    }
    if (raspi_config_filter_expression(
            config, filter, &expression_match)) {
        filters->expression = expression_match;
        return;
    }
    filters->other = false;
}

static bool raspi_config_parse_file(RaspiConfigParser *parser,
                                    const char *path, unsigned int depth,
                                    Error **errp);

static bool raspi_config_parse_contents(RaspiConfigParser *parser,
                                        const char *path,
                                        const uint8_t *contents,
                                        size_t length,
                                        unsigned int depth,
                                        Error **errp);

static bool raspi_config_parse_include(RaspiConfigParser *parser,
                                       const char *current_path,
                                       const char *include_path,
                                       unsigned int depth, Error **errp)
{
    g_autofree char *directory = raspi_path_directory(current_path);
    g_autofree char *normalized = raspi_normalize_path(
        directory, include_path, errp);

    if (!normalized) {
        return false;
    }
    if (parser->config->include_count >= RASPI_CONFIG_TOTAL_INCLUDE_MAX) {
        error_setg(errp, "config.txt include count exceeds %u",
                   RASPI_CONFIG_TOTAL_INCLUDE_MAX);
        return false;
    }
    parser->config->include_count++;
    g_ptr_array_add(parser->config->include_files, g_strdup(normalized));
    return raspi_config_parse_file(parser, normalized, depth + 1, errp);
}

static bool raspi_config_parse_initramfs(RaspiFirmwareConfig *config,
                                         char *line, Error **errp)
{
    g_auto(GStrv) fields = g_strsplit_set(line, " \t", -1);
    const char *filename = NULL;
    const char *address = NULL;

    for (char **field = fields + 1; *field; field++) {
        if (!(*field)[0]) {
            continue;
        }
        if (!filename) {
            filename = *field;
        } else if (!address) {
            address = *field;
        } else {
            error_setg(errp, "initramfs has too many arguments");
            return false;
        }
    }
    if (!filename) {
        error_setg(errp, "initramfs requires at least one filename");
        return false;
    }
    {
        g_auto(GStrv) names = g_strsplit(filename, ",", -1);
        unsigned int count = 0;

        for (char **name = names; *name; name++, count++) {
            if (!(*name)[0]) {
                error_setg(errp, "initramfs contains an empty filename");
                return false;
            }
            if (count == RASPI_FIRMWARE_MAX_INITRAMFS_FILES) {
                error_setg(errp, "initramfs supports at most %u files",
                           RASPI_FIRMWARE_MAX_INITRAMFS_FILES);
                return false;
            }
        }
    }
    raspi_replace_string(&config->initramfs_file, filename);
    raspi_replace_string(&config->initramfs_address,
                         address ? address : "followkernel");
    return true;
}

static bool raspi_config_apply_property(RaspiFirmwareConfig *config,
                                        const char *key, const char *value,
                                        bool main_file, Error **errp)
{
    if (!g_ascii_strcasecmp(key, "kernel")) {
        const char *base = strrchr(value, '/');

        base = base ? base + 1 : value;
        raspi_replace_string(&config->kernel_file, value);
        config->kernel_explicit = true;
        if (!g_ascii_strcasecmp(base, "kernel8.img")) {
            config->arm_64bit = true;
        } else if (!g_ascii_strcasecmp(base, "kernel7l.img")) {
            config->arm_64bit = false;
        }
    } else if (!g_ascii_strcasecmp(key, "device_tree")) {
        config->device_tree_enabled = value[0];
        raspi_replace_string(&config->device_tree_file, value);
    } else if (!g_ascii_strcasecmp(key, "device_tree_address")) {
        if (!raspi_config_parse_u64_value(
                value, &config->device_tree_address)) {
            error_setg(errp,
                       "device_tree_address must be an unsigned 64-bit value");
            return false;
        }
        config->device_tree_address_set = true;
    } else if (!g_ascii_strcasecmp(key, "device_tree_end")) {
        if (!raspi_config_parse_u64_value(value, &config->device_tree_end)) {
            error_setg(errp,
                       "device_tree_end must be an unsigned 64-bit value");
            return false;
        }
        config->device_tree_end_set = true;
    } else if (!g_ascii_strcasecmp(key, "cmdline")) {
        raspi_replace_string(&config->cmdline_file, value);
    } else if (!g_ascii_strcasecmp(key, "os_prefix")) {
        raspi_replace_string(&config->os_prefix, value);
    } else if (!g_ascii_strcasecmp(key, "overlay_prefix")) {
        raspi_replace_string(&config->overlay_prefix, value);
    } else if (!g_ascii_strcasecmp(key, "arm_64bit")) {
        if (!raspi_parse_bool(value, &config->arm_64bit)) {
            error_setg(errp, "arm_64bit must be zero or one");
            return false;
        }
        if (!config->kernel_explicit) {
            raspi_replace_string(&config->kernel_file,
                                 config->arm_64bit ? "kernel8.img" :
                                                    "kernel7l.img");
        }
    } else if (!g_ascii_strcasecmp(key, "auto_initramfs")) {
        if (!raspi_parse_bool(value, &config->auto_initramfs)) {
            error_setg(errp, "auto_initramfs must be zero or one");
            return false;
        }
    } else if (!g_ascii_strcasecmp(key, "force_eeprom_read")) {
        if (!raspi_parse_bool(value, &config->force_eeprom_read)) {
            error_setg(errp, "force_eeprom_read must be zero or one");
            return false;
        }
    } else if (!g_ascii_strcasecmp(key, "ramfsfile")) {
        raspi_replace_string(&config->initramfs_file, value);
    } else if (!g_ascii_strcasecmp(key, "ramfsaddr")) {
        raspi_replace_string(&config->initramfs_address, value);
    } else if (!g_ascii_strcasecmp(key, "dtoverlay") ||
               !g_ascii_strcasecmp(key, "device_tree_overlay")) {
        if (value[0]) {
            g_ptr_array_add(config->overlays, g_strdup(value));
        }
        g_ptr_array_add(config->dt_commands,
                        g_strdup_printf("overlay=%s", value));
    } else if (!g_ascii_strcasecmp(key, "dtparam") ||
               !g_ascii_strcasecmp(key, "device_tree_param")) {
        g_ptr_array_add(config->dtparams, g_strdup(value));
        g_ptr_array_add(config->dt_commands,
                        g_strdup_printf("param=%s", value));
    } else if (main_file && !g_ascii_strcasecmp(key, "start_file")) {
        raspi_replace_string(&config->start_file, value);
        config->start_file_explicit = true;
    } else if (main_file && !g_ascii_strcasecmp(key, "fixup_file")) {
        raspi_replace_string(&config->fixup_file, value);
        config->fixup_file_explicit = true;
    } else if (main_file && !g_ascii_strcasecmp(key, "start_x")) {
        if (!raspi_parse_bool(value, &config->start_x)) {
            error_setg(errp, "start_x must be zero or one");
            return false;
        }
    } else if (main_file && !g_ascii_strcasecmp(key, "start_debug")) {
        if (!raspi_parse_bool(value, &config->start_debug)) {
            error_setg(errp, "start_debug must be zero or one");
            return false;
        }
    } else if (main_file && !g_ascii_strcasecmp(key, "gpu_mem")) {
        if (!raspi_config_parse_u32_value(value, &config->gpu_mem)) {
            error_setg(errp, "gpu_mem must be an unsigned 32-bit value");
            return false;
        }
        config->gpu_mem_set = true;
    } else if (main_file && !g_ascii_strcasecmp(key, "gpu_mem_256")) {
        if (!raspi_config_parse_u32_value(value, &config->gpu_mem_256)) {
            error_setg(errp,
                       "gpu_mem_256 must be an unsigned 32-bit value");
            return false;
        }
        config->gpu_mem_256_set = true;
    } else if (main_file && !g_ascii_strcasecmp(key, "gpu_mem_512")) {
        if (!raspi_config_parse_u32_value(value, &config->gpu_mem_512)) {
            error_setg(errp,
                       "gpu_mem_512 must be an unsigned 32-bit value");
            return false;
        }
        config->gpu_mem_512_set = true;
    } else if (main_file && !g_ascii_strcasecmp(key, "gpu_mem_1024")) {
        if (!raspi_config_parse_u32_value(value, &config->gpu_mem_1024)) {
            error_setg(errp,
                       "gpu_mem_1024 must be an unsigned 32-bit value");
            return false;
        }
        config->gpu_mem_1024_set = true;
    } else if (main_file && !g_ascii_strcasecmp(key, "total_mem")) {
        if (!raspi_config_parse_u32_value(value, &config->total_mem_mb)) {
            error_setg(errp, "total_mem must be an unsigned 32-bit value");
            return false;
        }
        config->total_mem_set = true;
    } else if (main_file && !g_ascii_strcasecmp(key, "bootcode_delay")) {
        if (!raspi_config_parse_u32_value(
                value, &config->bootcode_delay)) {
            error_setg(errp,
                       "bootcode_delay must be an unsigned 32-bit value");
            return false;
        }
        config->bootcode_delay_set = true;
    } else if (main_file && !g_ascii_strcasecmp(key, "sdram_freq")) {
        if (!raspi_config_parse_u32_value(
                value, &config->sdram_freq_mhz)) {
            error_setg(errp,
                       "sdram_freq must be an unsigned 32-bit value");
            return false;
        }
        config->sdram_freq_set = true;
    } else if (main_file && !g_ascii_strcasecmp(key, "uart_2ndstage")) {
        if (!raspi_parse_bool(value, &config->uart_2ndstage)) {
            error_setg(errp, "uart_2ndstage must be zero or one");
            return false;
        }
    } else {
        config->ignored_properties++;
    }
    return true;
}

static bool raspi_config_cut_down_pair(const RaspiFirmwareConfig *config)
{
    const char *start = strrchr(config->start_file, '/');
    const char *fixup = strrchr(config->fixup_file, '/');

    start = start ? start + 1 : config->start_file;
    fixup = fixup ? fixup + 1 : config->fixup_file;
    return g_str_has_prefix(start, "start") &&
           g_str_has_suffix(start, "cd.elf") &&
           g_str_has_prefix(fixup, "fixup") &&
           g_str_has_suffix(fixup, "cd.dat");
}

static bool raspi_config_select_firmware(RaspiFirmwareConfig *config,
                                         Error **errp)
{
    bool gpu_mem_set = config->gpu_mem_set;
    uint32_t gpu_mem = config->gpu_mem_set ?
                       config->gpu_mem :
                       RASPI_FIRMWARE_PI4_GPU_MEM_DEFAULT_MB;

    if (config->installed_mem_mb == 256 && config->gpu_mem_256_set) {
        gpu_mem = config->gpu_mem_256;
        gpu_mem_set = true;
    } else if (config->installed_mem_mb == 512 &&
               config->gpu_mem_512_set) {
        gpu_mem = config->gpu_mem_512;
        gpu_mem_set = true;
    } else if (config->installed_mem_mb >= 1024 &&
               config->gpu_mem_1024_set) {
        gpu_mem = config->gpu_mem_1024;
        gpu_mem_set = true;
    }
    config->gpu_mem_effective_mb = MAX(gpu_mem, 16);

    if (config->total_mem_set) {
        config->total_mem_mb = MAX(config->total_mem_mb, 128);
        if (config->installed_mem_mb) {
            config->total_mem_mb = MIN(config->total_mem_mb,
                                       config->installed_mem_mb);
        }
    }
    if (config->installed_mem_mb &&
        config->gpu_mem_effective_mb >=
            MIN(config->installed_mem_mb, 1024U)) {
        error_setg(errp, "gpu_mem must leave addressable low memory for ARM");
        return false;
    }
    if (config->total_mem_set &&
        config->gpu_mem_effective_mb >=
            MIN(config->total_mem_mb, 1024U)) {
        error_setg(errp, "gpu_mem must be smaller than total_mem");
        return false;
    }
    if (config->start_file_explicit != config->fixup_file_explicit) {
        error_setg(errp, "start_file and fixup_file must be configured "
                   "together");
        return false;
    }
    if (config->start_file_explicit) {
        if (raspi_config_cut_down_pair(config)) {
            error_setg(errp, "cut-down firmware must be selected with "
                       "gpu_mem=16");
            return false;
        }
        return true;
    }
    if (config->start_debug) {
        raspi_replace_string(&config->start_file, "start_db.elf");
        raspi_replace_string(&config->fixup_file, "fixup_db.dat");
    } else if (config->start_x) {
        raspi_replace_string(&config->start_file, "start4x.elf");
        raspi_replace_string(&config->fixup_file, "fixup4x.dat");
    } else if (gpu_mem_set && config->gpu_mem_effective_mb == 16) {
        raspi_replace_string(&config->start_file, "start4cd.elf");
        raspi_replace_string(&config->fixup_file, "fixup4cd.dat");
    }
    return true;
}

static bool raspi_config_parse_file(RaspiConfigParser *parser,
                                    const char *path, unsigned int depth,
                                    Error **errp)
{
    RaspiFirmwareConfig *config = parser->config;
    g_autofree uint8_t *contents = NULL;
    RaspiFatFile file;
    RaspiFatResult result;
    size_t length;

    if (depth > RASPI_CONFIG_INCLUDE_MAX) {
        error_setg(errp, "config.txt include depth exceeds %u",
                   RASPI_CONFIG_INCLUDE_MAX);
        return false;
    }
    if (g_hash_table_contains(parser->include_paths, path)) {
        error_setg(errp, "config.txt include cycle at '%s'", path);
        return false;
    }
    g_hash_table_add(parser->include_paths, g_strdup(path));
    contents = parser->read_path ?
        parser->read_path(parser->read_opaque, path, RASPI_CONFIG_FILE_MAX,
                          &length, &result, errp) :
        raspi_read_boot_file(parser->volume, path, RASPI_CONFIG_FILE_MAX,
                             &length, &file, &result, errp);
    if (!contents) {
        g_hash_table_remove(parser->include_paths, path);
        if (depth == 0 && result == RASPI_FAT_NOT_FOUND) {
            if (!config->bootloader_config_append_size) {
                return true;
            }
            config->config_present = true;
            return raspi_config_parse_contents(
                parser, path, config->bootloader_config_append,
                config->bootloader_config_append_size, depth, errp);
        }
        if (result == RASPI_FAT_NOT_FOUND) {
            error_setg(errp, "config.txt include '%s' was not found", path);
        }
        return false;
    }
    if (depth == 0) {
        config->config_present = true;
    }
    if (!raspi_config_parse_contents(
            parser, path, contents, length, depth, errp)) {
        g_hash_table_remove(parser->include_paths, path);
        return false;
    }
    g_hash_table_remove(parser->include_paths, path);

    if (depth == 0 && config->bootloader_config_append_size) {
        return raspi_config_parse_contents(
            parser, path, config->bootloader_config_append,
            config->bootloader_config_append_size, depth, errp);
    }
    return true;
}

static bool raspi_config_parse_contents(RaspiConfigParser *parser,
                                        const char *path,
                                        const uint8_t *contents,
                                        size_t length,
                                        unsigned int depth,
                                        Error **errp)
{
    RaspiFirmwareConfig *config = parser->config;
    g_autofree char *text = NULL;
    g_auto(GStrv) lines = NULL;

    if (parser->parsed_bytes > RASPI_CONFIG_TOTAL_BYTES_MAX - length) {
        error_setg(errp, "config.txt parsed size exceeds %llu bytes",
                   (unsigned long long)RASPI_CONFIG_TOTAL_BYTES_MAX);
        return false;
    }
    parser->parsed_bytes += length;
    text = g_strndup((const char *)contents, length);
    lines = g_strsplit(text, "\n", -1);

    for (char **linep = lines; *linep; linep++) {
        g_autofree char *bounded = g_strndup(
            *linep, MIN(strlen(*linep), (size_t)RASPI_CONFIG_LINE_MAX));
        char *line = g_strstrip(bounded);
        char *separator;

        if (!line[0] || line[0] == '#') {
            continue;
        }
        if (config->parsed_lines >= RASPI_CONFIG_TOTAL_LINES_MAX) {
            error_setg(errp, "config.txt parsed line count exceeds %u",
                       RASPI_CONFIG_TOTAL_LINES_MAX);
            return false;
        }
        config->parsed_lines++;
        if (line[0] == '[') {
            raspi_config_filter_apply(parser, line);
            continue;
        }
        if (!raspi_config_filters_active(&parser->filters)) {
            continue;
        }
        if (g_str_has_prefix(line, "include ") ||
            g_str_has_prefix(line, "include\t")) {
            char *include_path = g_strstrip(line + strlen("include"));

            if (!raspi_config_parse_include(parser, path, include_path,
                                            depth, errp)) {
                return false;
            }
            continue;
        }
        if (g_str_has_prefix(line, "initramfs ") ||
            g_str_has_prefix(line, "initramfs\t")) {
            if (!raspi_config_parse_initramfs(config, line, errp)) {
                return false;
            }
            continue;
        }
        separator = strchr(line, '=');
        if (!separator) {
            config->ignored_properties++;
            continue;
        }
        *separator = 0;
        if (!raspi_config_apply_property(config, g_strstrip(line),
                                         g_strstrip(separator + 1),
                                         depth == 0, errp)) {
            return false;
        }
    }
    return true;
}

bool raspi_firmware_config_load_named(RaspiFatVolume *volume,
                                      RaspiFirmwareConfig *config,
                                      const char *config_path,
                                      Error **errp)
{
    RaspiConfigParser parser = {
        .volume = volume,
        .config = config,
        .include_paths = g_hash_table_new_full(
            g_str_hash, g_str_equal, g_free, NULL),
    };
    raspi_config_filters_reset(&parser.filters);
    bool result = raspi_config_parse_file(&parser, config_path, 0, errp);

    if (result) {
        result = raspi_config_select_firmware(config, errp);
    }

    g_hash_table_unref(parser.include_paths);
    return result;
}

bool raspi_firmware_config_load(RaspiFatVolume *volume,
                                RaspiFirmwareConfig *config, Error **errp)
{
    return raspi_firmware_config_load_named(
        volume, config, "config.txt", errp);
}

bool raspi_firmware_config_load_with_reader_named(
    RaspiFirmwareConfig *config, RaspiFirmwareReadPath *read_path,
    void *opaque, const char *config_path, Error **errp)
{
    RaspiConfigParser parser = {
        .read_path = read_path,
        .read_opaque = opaque,
        .config = config,
        .include_paths = g_hash_table_new_full(
            g_str_hash, g_str_equal, g_free, NULL),
    };
    raspi_config_filters_reset(&parser.filters);
    bool result = raspi_config_parse_file(&parser, config_path, 0, errp);

    if (result) {
        result = raspi_config_select_firmware(config, errp);
    }
    g_hash_table_unref(parser.include_paths);
    return result;
}

bool raspi_firmware_config_load_with_reader(
    RaspiFirmwareConfig *config, RaspiFirmwareReadPath *read_path,
    void *opaque, Error **errp)
{
    return raspi_firmware_config_load_with_reader_named(
        config, read_path, opaque, "config.txt", errp);
}

static char *raspi_prefixed_path(const char *prefix, const char *path,
                                 Error **errp)
{
    g_autofree char *combined = NULL;

    if (path[0] == '/') {
        return raspi_normalize_path(NULL, path, errp);
    }
    combined = prefix[0] ? g_strconcat(prefix, path, NULL) : g_strdup(path);
    return raspi_normalize_path(NULL, combined, errp);
}

static RaspiFatResult raspi_find_nonempty(RaspiFatVolume *volume,
                                          const char *path,
                                          RaspiFatFile *file, Error **errp)
{
    RaspiFatResult result = raspi_fat_find_path(volume, path, file, errp);

    return result == RASPI_FAT_FOUND && !file->size ?
           RASPI_FAT_NOT_FOUND : result;
}

static bool raspi_firmware_fallback_pair(const RaspiFirmwareConfig *config,
                                         const char **start,
                                         const char **fixup)
{
    if (!config->start_file_explicit &&
        !g_ascii_strcasecmp(config->start_file, "start4.elf") &&
        !g_ascii_strcasecmp(config->fixup_file, "fixup4.dat")) {
        *start = "start.elf";
        *fixup = "fixup.dat";
        return true;
    }
    if (!config->start_file_explicit && config->start_x &&
        !g_ascii_strcasecmp(config->start_file, "start4x.elf") &&
        !g_ascii_strcasecmp(config->fixup_file, "fixup4x.dat")) {
        *start = "start_x.elf";
        *fixup = "fixup_x.dat";
        return true;
    }
    return false;
}

static char *raspi_auto_initramfs_name(const char *kernel)
{
    const char *base = strrchr(kernel, '/');
    const char *name = base ? base + 1 : kernel;
    const char *extension;
    g_autofree char *directory = base ?
        g_strndup(kernel, base - kernel + 1) : g_strdup("");

    if (!g_str_has_prefix(name, "kernel")) {
        return NULL;
    }
    extension = strrchr(name, '.');
    return g_strdup_printf("%sinitramfs%.*s", directory,
                           (int)(extension ? extension - name - 6 :
                                            strlen(name) - 6),
                           name + 6);
}

void raspi_firmware_paths_clear(RaspiFirmwarePaths *paths)
{
    g_free(paths->start);
    g_free(paths->fixup);
    g_free(paths->kernel);
    g_free(paths->device_tree);
    g_free(paths->cmdline);
    for (unsigned int i = 0; i < paths->initramfs_count; i++) {
        g_free(paths->initramfs[i]);
    }
    memset(paths, 0, sizeof(*paths));
}

bool raspi_firmware_paths_resolve(RaspiFirmwareConfig *config,
                                  RaspiFirmwarePaths *paths,
                                  Error **errp)
{
    memset(paths, 0, sizeof(*paths));
    paths->start = raspi_normalize_path(NULL, config->start_file, errp);
    paths->fixup = raspi_normalize_path(NULL, config->fixup_file, errp);
    paths->kernel = raspi_prefixed_path(config->os_prefix,
                                        config->kernel_file, errp);
    paths->device_tree = config->device_tree_enabled ?
        raspi_prefixed_path(config->os_prefix, config->device_tree_file,
                            errp) : NULL;
    paths->cmdline = raspi_prefixed_path(config->os_prefix,
                                         config->cmdline_file, errp);
    if (!config->initramfs_file && config->auto_initramfs) {
        config->initramfs_file = raspi_auto_initramfs_name(
            config->kernel_file);
        config->initramfs_auto = config->initramfs_file != NULL;
    }
    if (config->initramfs_file) {
        g_auto(GStrv) names = g_strsplit(config->initramfs_file, ",", -1);

        for (char **name = names; *name; name++) {
            if (!(*name)[0] ||
                paths->initramfs_count ==
                    RASPI_FIRMWARE_MAX_INITRAMFS_FILES) {
                raspi_firmware_paths_clear(paths);
                return false;
            }
            paths->initramfs[paths->initramfs_count] =
                raspi_prefixed_path(config->os_prefix, *name, errp);
            if (!paths->initramfs[paths->initramfs_count]) {
                raspi_firmware_paths_clear(paths);
                return false;
            }
            paths->initramfs_count++;
        }
    }
    if (!paths->start || !paths->fixup || !paths->kernel ||
        (config->device_tree_enabled && !paths->device_tree) ||
        !paths->cmdline ||
        (config->initramfs_file && !paths->initramfs_count)) {
        raspi_firmware_paths_clear(paths);
        return false;
    }
    return true;
}

RaspiFirmwareResolveResult raspi_firmware_resolve(
    RaspiFatVolume *volume, RaspiFirmwareConfig *config,
    RaspiFirmwareManifest *manifest, Error **errp)
{
    g_autofree char *start = raspi_normalize_path(NULL, config->start_file,
                                                  errp);
    g_autofree char *fixup = raspi_normalize_path(NULL, config->fixup_file,
                                                  errp);
    g_autofree char *kernel = NULL;
    g_autofree char *device_tree = NULL;
    g_autofree char *cmdline = NULL;
    RaspiFatResult result;

    memset(manifest, 0, sizeof(*manifest));
    if (!start || !fixup) {
        return RASPI_FIRMWARE_RESOLVE_ERROR;
    }
    result = raspi_find_nonempty(volume, start, &manifest->start, errp);
    if (result == RASPI_FAT_ERROR) {
        return RASPI_FIRMWARE_RESOLVE_ERROR;
    }
    if (result != RASPI_FAT_FOUND) {
        const char *fallback_start;
        const char *fallback_fixup;

        if (!raspi_firmware_fallback_pair(
                config, &fallback_start, &fallback_fixup)) {
            return RASPI_FIRMWARE_MISSING_START;
        }
        g_clear_pointer(&start, g_free);
        g_clear_pointer(&fixup, g_free);
        start = g_strdup(fallback_start);
        fixup = g_strdup(fallback_fixup);
        result = raspi_find_nonempty(volume, start, &manifest->start, errp);
        if (result == RASPI_FAT_ERROR) {
            return RASPI_FIRMWARE_RESOLVE_ERROR;
        }
        if (result != RASPI_FAT_FOUND) {
            return RASPI_FIRMWARE_MISSING_START;
        }
        raspi_replace_string(&config->start_file, start);
        raspi_replace_string(&config->fixup_file, fixup);
    }
    result = raspi_find_nonempty(volume, fixup, &manifest->fixup, errp);
    if (result == RASPI_FAT_ERROR) {
        return RASPI_FIRMWARE_RESOLVE_ERROR;
    }
    if (result != RASPI_FAT_FOUND) {
        return RASPI_FIRMWARE_MISSING_FIXUP;
    }

    kernel = raspi_prefixed_path(config->os_prefix, config->kernel_file,
                                 errp);
    device_tree = config->device_tree_enabled ?
        raspi_prefixed_path(config->os_prefix, config->device_tree_file,
                            errp) : NULL;
    if (!kernel || (config->device_tree_enabled && !device_tree)) {
        return RASPI_FIRMWARE_RESOLVE_ERROR;
    }

    if (config->os_prefix[0]) {
        RaspiFatFile probe;
        bool viable = raspi_find_nonempty(volume, kernel, &probe, NULL) ==
                      RASPI_FAT_FOUND;

        if (config->device_tree_enabled) {
            viable = viable &&
                raspi_find_nonempty(volume, device_tree, &probe, NULL) ==
                RASPI_FAT_FOUND;
        }
        if (!viable) {
            raspi_replace_string(&config->os_prefix, "");
            g_clear_pointer(&kernel, g_free);
            g_clear_pointer(&device_tree, g_free);
            kernel = raspi_prefixed_path("", config->kernel_file, errp);
            device_tree = config->device_tree_enabled ?
                raspi_prefixed_path("", config->device_tree_file, errp) :
                NULL;
        }
    }
    result = raspi_find_nonempty(volume, kernel, &manifest->kernel, errp);
    if (result == RASPI_FAT_ERROR) {
        return RASPI_FIRMWARE_RESOLVE_ERROR;
    }
    if (result != RASPI_FAT_FOUND) {
        return RASPI_FIRMWARE_MISSING_KERNEL;
    }
    if (config->device_tree_enabled) {
        result = raspi_find_nonempty(volume, device_tree,
                                     &manifest->device_tree, errp);
        if (result == RASPI_FAT_ERROR) {
            return RASPI_FIRMWARE_RESOLVE_ERROR;
        }
        if (result != RASPI_FAT_FOUND) {
            return RASPI_FIRMWARE_MISSING_DEVICE_TREE;
        }
    }

    cmdline = raspi_prefixed_path(config->os_prefix, config->cmdline_file,
                                  errp);
    if (!cmdline) {
        return RASPI_FIRMWARE_RESOLVE_ERROR;
    }
    result = raspi_find_nonempty(volume, cmdline, &manifest->cmdline, errp);
    if (result == RASPI_FAT_ERROR) {
        return RASPI_FIRMWARE_RESOLVE_ERROR;
    }
    manifest->cmdline_present = result == RASPI_FAT_FOUND;

    if (!config->initramfs_file && config->auto_initramfs) {
        config->initramfs_file = raspi_auto_initramfs_name(
            config->kernel_file);
        config->initramfs_auto = config->initramfs_file != NULL;
    }
    if (config->initramfs_file) {
        g_auto(GStrv) names = g_strsplit(config->initramfs_file, ",", -1);

        for (char **name = names; *name; name++) {
            g_autofree char *initramfs = NULL;
            unsigned int index = manifest->initramfs_count;

            if (!(*name)[0] ||
                index == RASPI_FIRMWARE_MAX_INITRAMFS_FILES) {
                return RASPI_FIRMWARE_RESOLVE_ERROR;
            }
            initramfs = raspi_prefixed_path(config->os_prefix, *name, errp);
            if (!initramfs) {
                return RASPI_FIRMWARE_RESOLVE_ERROR;
            }
            result = raspi_find_nonempty(volume, initramfs,
                                         &manifest->initramfs[index], errp);
            if (result == RASPI_FAT_ERROR) {
                return RASPI_FIRMWARE_RESOLVE_ERROR;
            }
            if (result != RASPI_FAT_FOUND) {
                return RASPI_FIRMWARE_MISSING_INITRAMFS;
            }
            manifest->initramfs_count++;
        }
    }
    return RASPI_FIRMWARE_READY;
}
