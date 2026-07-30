/*
 * Raspberry Pi behavioral Device Tree overlay processing
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "qemu/units.h"
#include "hw/arm/raspi_overlay.h"
#include <libfdt.h>

#define RASPI_OVERLAY_MAX_SIZE (2 * MiB)
#define RASPI_OVERLAY_MAX_COUNT 64
#define RASPI_OVERLAY_DTB_SLACK (256 * KiB)
#define RASPI_HAT_EEPROM_MAX_SIZE (1 * MiB)
#define RASPI_OVERLAY_MAX_PROPERTY_SIZE (64 * KiB)
#define RASPI_OVERLAY_MAX_MUTATION_BYTES (4 * MiB)
#define RASPI_OVERLAY_MAX_DEPTH 32
#define RASPI_OVERLAY_MAX_NODES 4096
#define RASPI_OVERLAY_MAX_ORDER_WORK (RASPI_OVERLAY_MAX_NODES * 64)

typedef struct RaspiPendingOverlay {
    char *name;
    GPtrArray *parameters;
    const uint8_t *embedded;
    size_t embedded_size;
} RaspiPendingOverlay;

typedef struct RaspiHatEeprom {
    char *vendor;
    char *product;
    char *uuid;
    uint32_t declared_size;
    uint16_t product_id;
    uint16_t product_version;
    const uint8_t *overlay;
    size_t overlay_size;
    char *overlay_name;
    GPtrArray *custom;
    uint8_t gpio_map[30];
    bool gpio_valid;
} RaspiHatEeprom;

typedef struct RaspiOverlaySource {
    RaspiFatVolume *volume;
    RaspiFirmwareReadPath *read_path;
    void *read_opaque;
} RaspiOverlaySource;

typedef enum RaspiOverridePass {
    RASPI_OVERRIDE_ALL,
    RASPI_OVERRIDE_REGULAR,
    RASPI_OVERRIDE_DEFERRED,
} RaspiOverridePass;

/*
 * Overlays arrive from the boot medium and from HAT EEPROMs, so libfdt is
 * only allowed to look at a blob once the buffer is known to hold a whole
 * header and the entire declared tree.  fdt_check_header() itself reads a
 * full struct fdt_header, which a short blob does not provide.
 */
static bool raspi_overlay_blob_valid(const void *blob, size_t size)
{
    size_t totalsize;

    if (!blob || size < sizeof(struct fdt_header) ||
        size > RASPI_OVERLAY_MAX_SIZE) {
        return false;
    }
    if (fdt_check_header(blob)) {
        return false;
    }
    totalsize = fdt_totalsize(blob);
    if (totalsize > size) {
        return false;
    }
    return fdt_check_full(blob, totalsize) == 0;
}

static uint16_t raspi_hat_crc16(const uint8_t *data, size_t size)
{
    uint16_t out = 0;

    for (size_t byte = 0; byte < size; byte++) {
        for (unsigned int bit = 0; bit < 8; bit++) {
            bool high = out & 0x8000;

            out = (out << 1) | ((data[byte] >> bit) & 1);
            if (high) {
                out ^= 0x8005;
            }
        }
    }
    for (unsigned int bit = 0; bit < 16; bit++) {
        bool high = out & 0x8000;

        out <<= 1;
        if (high) {
            out ^= 0x8005;
        }
    }
    out = (out & 0x5555) << 1 | (out >> 1 & 0x5555);
    out = (out & 0x3333) << 2 | (out >> 2 & 0x3333);
    out = (out & 0x0f0f) << 4 | (out >> 4 & 0x0f0f);
    return out << 8 | out >> 8;
}

static void raspi_hat_eeprom_clear(RaspiHatEeprom *hat)
{
    g_free(hat->vendor);
    g_free(hat->product);
    g_free(hat->uuid);
    g_free(hat->overlay_name);
    g_clear_pointer(&hat->custom, g_ptr_array_unref);
    memset(hat, 0, sizeof(*hat));
}

static bool raspi_hat_eeprom_parse(const uint8_t *data, size_t size,
                                   RaspiHatEeprom *hat, Error **errp)
{
    uint32_t total;
    uint16_t atoms;
    size_t offset = 12;
    bool vendor_seen = false;
    bool gpio_seen = false;

    if (size < 12 || size > RASPI_HAT_EEPROM_MAX_SIZE ||
        memcmp(data, "R-Pi", 4) || data[4] != 1 || data[5] != 0) {
        error_setg(errp, "Raspberry Pi HAT EEPROM header is invalid");
        return false;
    }
    atoms = lduw_le_p(data + 6);
    total = ldl_le_p(data + 8);
    if (total < 12 || total > size || !atoms) {
        error_setg(errp, "Raspberry Pi HAT EEPROM length is invalid");
        return false;
    }
    hat->custom = g_ptr_array_new_with_free_func(
        (GDestroyNotify)g_bytes_unref);
    hat->declared_size = total;
    for (unsigned int index = 0; index < atoms; index++) {
        const uint8_t *atom;
        const uint8_t *payload;
        uint16_t type;
        uint16_t count;
        uint32_t dlen;
        size_t payload_size;

        if (offset > total - 10) {
            error_setg(errp, "Raspberry Pi HAT EEPROM atom is truncated");
            return false;
        }
        atom = data + offset;
        type = lduw_le_p(atom);
        count = lduw_le_p(atom + 2);
        dlen = ldl_le_p(atom + 4);
        if (count != index || dlen < 2 || dlen > total - offset - 8) {
            error_setg(errp, "Raspberry Pi HAT EEPROM atom is invalid");
            return false;
        }
        payload = atom + 8;
        payload_size = dlen - 2;
        if (raspi_hat_crc16(atom, 8 + payload_size) !=
            lduw_le_p(payload + payload_size)) {
            error_setg(errp, "Raspberry Pi HAT EEPROM atom CRC is invalid");
            return false;
        }
        if ((index == 0 && type != 1) || (index == 1 && type != 2) ||
            (type == 5 && (index != atoms - 1 || payload_size != 20))) {
            error_setg(errp, "Raspberry Pi HAT EEPROM atom order is invalid");
            return false;
        }
        if (type == 1) {
            uint8_t vendor_length;
            uint8_t product_length;
            uint32_t serial[4];

            if (vendor_seen || payload_size < 22) {
                error_setg(errp,
                           "Raspberry Pi HAT vendor atom is invalid");
                return false;
            }
            vendor_length = payload[20];
            product_length = payload[21];
            if ((size_t)vendor_length + product_length !=
                    payload_size - 22 ||
                memchr(payload + 22, '\0',
                       vendor_length + product_length)) {
                error_setg(errp,
                           "Raspberry Pi HAT identity strings are invalid");
                return false;
            }
            for (unsigned int i = 0; i < 4; i++) {
                serial[i] = ldl_le_p(payload + i * 4);
            }
            hat->product_id = lduw_le_p(payload + 16);
            hat->product_version = lduw_le_p(payload + 18);
            hat->vendor = g_strndup((const char *)payload + 22,
                                    vendor_length);
            hat->product = g_strndup(
                (const char *)payload + 22 + vendor_length,
                product_length);
            hat->uuid = g_strdup_printf(
                "%08x-%04x-%04x-%04x-%04x%08x",
                serial[3], serial[2] >> 16, serial[2] & 0xffff,
                serial[1] >> 16, serial[1] & 0xffff, serial[0]);
            vendor_seen = true;
        } else if (type == 2) {
            if (gpio_seen || payload_size != 30) {
                error_setg(errp, "Raspberry Pi HAT GPIO atom is invalid");
                return false;
            }
            if ((payload[0] & 0x0f) > 8 ||
                ((payload[0] >> 4) & 3) > 2 ||
                ((payload[0] >> 6) & 3) > 2 ||
                (payload[1] & 0xfc) || (payload[1] & 3) > 2) {
                error_setg(errp,
                           "Raspberry Pi HAT GPIO bank policy is invalid");
                return false;
            }
            for (unsigned int pin = 0; pin < 28; pin++) {
                if (payload[pin + 2] & 0x18) {
                    error_setg(errp,
                               "Raspberry Pi HAT GPIO pin map is invalid");
                    return false;
                }
            }
            memcpy(hat->gpio_map, payload, sizeof(hat->gpio_map));
            hat->gpio_valid = true;
            gpio_seen = true;
        } else if (type == 3) {
            g_autofree char *name = NULL;

            if (hat->overlay || hat->overlay_name || !payload_size) {
                error_setg(errp,
                           "Raspberry Pi HAT Device Tree atom is invalid");
                return false;
            }
            if (payload_size >= sizeof(struct fdt_header) &&
                ldl_be_p(payload) == FDT_MAGIC) {
                if (!raspi_overlay_blob_valid(payload, payload_size) ||
                    fdt_totalsize(payload) != payload_size) {
                    error_setg(errp,
                               "Raspberry Pi HAT embedded overlay is invalid");
                    return false;
                }
                hat->overlay = payload;
                hat->overlay_size = payload_size;
            } else {
                name = g_strndup((const char *)payload, payload_size);
                if (memchr(payload, '\0', payload_size)) {
                    error_setg(errp,
                               "Raspberry Pi HAT overlay name is invalid");
                    return false;
                }
                g_strstrip(name);
                if (!name[0] || strchr(name, '/')) {
                    error_setg(errp,
                               "Raspberry Pi HAT overlay name is invalid");
                    return false;
                }
                hat->overlay_name = g_steal_pointer(&name);
            }
        } else if (type == 4) {
            g_ptr_array_add(hat->custom,
                            g_bytes_new(payload, payload_size));
        } else if (type != 5) {
            error_setg(errp,
                       "Raspberry Pi HAT EEPROM atom type %u is unsupported",
                       type);
            return false;
        }
        offset += 8 + dlen;
    }
    if (offset != total || !vendor_seen || !gpio_seen) {
        error_setg(errp, "Raspberry Pi HAT EEPROM atom set is incomplete");
        return false;
    }
    return true;
}

static bool raspi_parameter_bool(const char *value, bool *result)
{
    static const char * const enabled[] = {
        "1", "on", "yes", "true", "y",
    };
    static const char * const disabled[] = {
        "0", "off", "no", "false", "n",
    };

    for (size_t i = 0; i < ARRAY_SIZE(enabled); i++) {
        if (!g_ascii_strcasecmp(value, enabled[i])) {
            *result = true;
            return true;
        }
    }
    for (size_t i = 0; i < ARRAY_SIZE(disabled); i++) {
        if (!g_ascii_strcasecmp(value, disabled[i])) {
            *result = false;
            return true;
        }
    }
    return false;
}

static const char *raspi_parameter_alias(const char *name)
{
    if (!g_ascii_strcasecmp(name, "i2c") ||
        !g_ascii_strcasecmp(name, "i2c_arm")) {
        return "i2c1";
    }
    if (!g_ascii_strcasecmp(name, "i2c_baudrate") ||
        !g_ascii_strcasecmp(name, "i2c_arm_baudrate")) {
        return "i2c1_baudrate";
    }
    return name;
}

static bool raspi_overlay_set_fragment(void *overlay, unsigned int number,
                                       bool enable, Error **errp)
{
    g_autofree char *path = g_strdup_printf("/fragment@%u", number);
    const char *from = enable ? "__dormant__" : "__overlay__";
    const char *to = enable ? "__overlay__" : "__dormant__";
    int fragment = fdt_path_offset(overlay, path);
    int already;
    int node;

    if (fragment < 0) {
        error_setg(errp, "Raspberry Pi overlay has no fragment@%u", number);
        return false;
    }
    already = fdt_subnode_offset(overlay, fragment, to);
    if (already >= 0) {
        return true;
    }
    node = fdt_subnode_offset(overlay, fragment, from);
    if (node < 0 || fdt_set_name(overlay, node, to) < 0) {
        error_setg(errp, "Raspberry Pi overlay fragment@%u cannot be %s",
                   number, enable ? "enabled" : "disabled");
        return false;
    }
    return true;
}

static bool raspi_overlay_fragment_ops(void *overlay, const char *operations,
                                       const char *value, Error **errp)
{
    const char *cursor = operations;
    bool parameter_enabled = false;
    bool conditional = false;

    for (const char *operation = operations; *operation; operation++) {
        if (*operation == '=' || *operation == '!') {
            conditional = true;
            break;
        }
    }
    if (conditional && !raspi_parameter_bool(value, &parameter_enabled)) {
        error_setg(errp, "Raspberry Pi fragment parameter is not boolean");
        return false;
    }
    while (*cursor) {
        char operation = *cursor++;
        const char *end;
        uint64_t fragment;
        bool enable;

        if (!strchr("+-=!", operation) ||
            qemu_strtou64(cursor, &end, 10, &fragment) < 0 ||
            end == cursor || fragment > UINT_MAX) {
            error_setg(errp, "Raspberry Pi fragment override is invalid");
            return false;
        }
        switch (operation) {
        case '+':
            enable = true;
            break;
        case '-':
            enable = false;
            break;
        case '=':
            enable = parameter_enabled;
            break;
        case '!':
            enable = !parameter_enabled;
            break;
        default:
            g_assert_not_reached();
        }
        if (!raspi_overlay_set_fragment(overlay, fragment, enable, errp)) {
            return false;
        }
        cursor = end;
    }
    return true;
}

/*
 * Overlay descriptors carry the target byte offset, so a single parameter
 * could otherwise ask for an allocation just under INT_MAX and repeated
 * parameters could keep asking.  Charge every property rewrite against a
 * cumulative budget and cap the individual property size.
 */
static bool raspi_overlay_charge_mutation(size_t *budget, size_t bytes,
                                          const char *property, Error **errp)
{
    if (bytes > RASPI_OVERLAY_MAX_PROPERTY_SIZE || !budget || bytes > *budget) {
        error_setg(errp,
                   "Raspberry Pi DT parameter target %s exceeds the overlay "
                   "mutation budget", property);
        return false;
    }
    *budget -= bytes;
    return true;
}

static bool raspi_overlay_set_integer(void *fdt, int node,
                                      const char *property, size_t offset,
                                      size_t width, uint64_t value,
                                      size_t *mutation_budget, Error **errp)
{
    int length;
    const uint8_t *existing = fdt_getprop(fdt, node, property, &length);
    size_t old_length = existing && length > 0 ? length : 0;
    size_t new_length;
    g_autofree uint8_t *contents = NULL;

    if (offset > INT_MAX || width > INT_MAX - offset) {
        error_setg(errp, "Raspberry Pi DT parameter target %s is too large",
                   property);
        return false;
    }
    new_length = MAX(old_length, offset + width);
    if (!raspi_overlay_charge_mutation(mutation_budget, new_length, property,
                                       errp)) {
        return false;
    }
    contents = g_malloc0(new_length);
    if (old_length) {
        memcpy(contents, existing, old_length);
    }
    switch (width) {
    case 1:
        contents[offset] = value;
        break;
    case 2:
        stw_be_p(contents + offset, value);
        break;
    case 4:
        stl_be_p(contents + offset, value);
        break;
    case 8:
        stq_be_p(contents + offset, value);
        break;
    default:
        g_assert_not_reached();
    }
    if (fdt_setprop(fdt, node, property, contents, new_length) >= 0) {
        return true;
    }
    error_setg(errp, "Raspberry Pi DT integer parameter %s cannot be set",
               property);
    return false;
}

static bool raspi_overlay_set_byte_string(void *fdt, int node,
                                          const char *property,
                                          const char *value,
                                          size_t *mutation_budget,
                                          Error **errp)
{
    g_autoptr(GByteArray) bytes = g_byte_array_new();
    const char *cursor = value;

    while (*cursor) {
        int high;
        int low;
        uint8_t byte;

        if (*cursor == ':') {
            cursor++;
            continue;
        }
        high = g_ascii_xdigit_value(cursor[0]);
        low = g_ascii_xdigit_value(cursor[1]);
        if (high < 0 || low < 0) {
            error_setg(errp,
                       "Raspberry Pi DT byte-string parameter is invalid");
            return false;
        }
        byte = high << 4 | low;
        g_byte_array_append(bytes, &byte, 1);
        cursor += 2;
    }
    if (!raspi_overlay_charge_mutation(mutation_budget, bytes->len, property,
                                       errp)) {
        return false;
    }
    if (!bytes->len || fdt_setprop(fdt, node, property, bytes->data,
                                   bytes->len) < 0) {
        error_setg(errp, "Raspberry Pi DT byte-string parameter cannot be set");
        return false;
    }
    return true;
}

static bool raspi_overlay_set_target(void *fdt, int node,
                                     const char *descriptor,
                                     const char *assigned_value,
                                     size_t *mutation_budget,
                                     Error **errp)
{
    g_autofree char *format = g_strdup(descriptor);
    const char *value = assigned_value;
    char *literal = strchr(format, '=');
    char *suffix;
    const char *end;
    uint64_t parsed;
    bool boolean;
    bool invert = false;
    size_t offset = 0;
    size_t width = 0;
    int length;
    const void *existing;

    if (literal) {
        *literal++ = '\0';
        value = literal;
    }
    suffix = strpbrk(format, ".;:#");
    if (suffix) {
        char separator = *suffix;

        *suffix++ = '\0';
        if (qemu_strtou64(suffix, &end, 10, &parsed) < 0 || *end ||
            parsed > SIZE_MAX) {
            error_setg(errp, "Raspberry Pi DT parameter offset is invalid");
            return false;
        }
        offset = parsed;
        switch (separator) {
        case '.':
            width = 1;
            break;
        case ';':
            width = 2;
            break;
        case ':':
            width = 4;
            break;
        case '#':
            width = 8;
            break;
        default:
            g_assert_not_reached();
        }
    }
    suffix = format + strlen(format);
    if (suffix > format && (suffix[-1] == '?' || suffix[-1] == '!')) {
        invert = suffix[-1] == '!';
        *--suffix = '\0';
        if (!raspi_parameter_bool(value, &boolean)) {
            error_setg(errp, "Raspberry Pi DT boolean parameter is invalid");
            return false;
        }
        boolean ^= invert;
        if (boolean) {
            if (fdt_setprop(fdt, node, format, NULL, 0) >= 0) {
                return true;
            }
        } else if (fdt_delprop(fdt, node, format) >= 0 ||
                   fdt_getprop(fdt, node, format, NULL) == NULL) {
            return true;
        }
        error_setg(errp, "Raspberry Pi DT boolean parameter failed");
        return false;
    }

    existing = fdt_getprop(fdt, node, format, &length);
    if (!strcmp(format, "status") &&
        raspi_parameter_bool(value, &boolean)) {
        if (fdt_setprop_string(fdt, node, format,
                               boolean ? "okay" : "disabled") >= 0) {
            return true;
        }
        error_setg(errp, "Raspberry Pi DT status parameter cannot be set");
        return false;
    }
    if (width) {
        g_autofree char *renamed = NULL;
        const char *node_name = NULL;
        const char *address = NULL;

        if (raspi_parameter_bool(value, &boolean)) {
            parsed = boolean;
        } else if (qemu_strtou64(value, &end, 0, &parsed) < 0 || *end) {
            error_setg(errp,
                       "Raspberry Pi DT integer parameter is invalid");
            return false;
        }
        if (width < sizeof(parsed) &&
            parsed >= (UINT64_C(1) << (width * 8))) {
            error_setg(errp,
                       "Raspberry Pi DT integer parameter is out of range");
            return false;
        }
        if (!strcmp(format, "reg") && offset == 0) {
            node_name = fdt_get_name(fdt, node, NULL);
            if (!node_name) {
                error_setg(errp,
                           "Raspberry Pi DT reg target has no node name");
                return false;
            }
            address = strchr(node_name, '@');
            renamed = g_strdup_printf(
                "%.*s@%" PRIx64,
                address ? (int)(address - node_name) : (int)strlen(node_name),
                node_name, parsed);
        }
        if (!raspi_overlay_set_integer(fdt, node, format, offset, width,
                                       parsed, mutation_budget, errp)) {
            return false;
        }
        if (renamed && fdt_set_name(fdt, node, renamed) < 0) {
            error_setg(errp,
                       "Raspberry Pi DT reg target cannot be renamed");
            return false;
        }
        return true;
    }
    suffix = format + strlen(format);
    if (suffix > format && suffix[-1] == '[') {
        suffix[-1] = '\0';
        return raspi_overlay_set_byte_string(fdt, node, format, value,
                                             mutation_budget, errp);
    }
    if (existing && length == 0 &&
        raspi_parameter_bool(value, &boolean)) {
        if (boolean) {
            return true;
        }
        return fdt_delprop(fdt, node, format) >= 0;
    }
    if (!strcmp(format, "name")) {
        if (fdt_set_name(fdt, node, value) >= 0) {
            return true;
        }
        error_setg(errp, "Raspberry Pi DT node cannot be renamed");
        return false;
    }
    if (offset == 0 && (!existing ||
                        (length > 0 && ((const char *)existing)[length - 1] ==
                         '\0'))) {
        if (!strcmp(format, "bootargs") && existing && length > 1) {
            g_autofree char *appended = g_strdup_printf(
                "%s %s", (const char *)existing, value);

            if (fdt_setprop_string(fdt, node, format, appended) >= 0) {
                return true;
            }
            error_setg(errp,
                       "Raspberry Pi DT bootargs parameter cannot be set");
            return false;
        }
        if (fdt_setprop_string(fdt, node, format, value) >= 0) {
            return true;
        }
    }
    error_setg(errp, "Raspberry Pi DT parameter '%s' has unsupported value",
               format);
    return false;
}

static const char *raspi_lookup_closing_brace(const char *value)
{
    bool quoted = false;

    for (; *value; value++) {
        if (*value == '\'') {
            quoted = !quoted;
        } else if (*value == '}' && !quoted) {
            return value;
        }
    }
    return NULL;
}

static bool raspi_overlay_lookup_value(const uint8_t *override, size_t length,
                                       size_t *cursor,
                                       const char *descriptor,
                                       const char *input,
                                       char **target_descriptor,
                                       char **mapped_value,
                                       bool *embedded,
                                       Error **errp)
{
    const char *opening = strchr(descriptor, '{');
    g_autoptr(GString) specification = g_string_new(descriptor);
    g_autofree char *selected = NULL;
    g_autofree char *fallback = NULL;
    size_t opening_offset = opening - descriptor;
    bool passthrough = false;
    const char *closing;
    const char *entry;

    while (!(closing = raspi_lookup_closing_brace(
                 specification->str + opening_offset + 1))) {
        const char *continuation;
        size_t available;
        size_t continuation_length;
        uint32_t cell;

        if (!specification->len ||
            specification->str[specification->len - 1] != '=' ||
            length - *cursor < sizeof(uint32_t)) {
            goto malformed;
        }
        cell = ldl_be_p(override + *cursor);
        *cursor += sizeof(uint32_t);
        g_string_append_printf(specification, "0x%08x", cell);
        *embedded = true;

        if (*cursor >= length) {
            goto malformed;
        }
        continuation = (const char *)override + *cursor;
        available = length - *cursor;
        continuation_length = strnlen(continuation, available);
        if (continuation_length == available) {
            goto malformed;
        }
        *cursor += continuation_length + 1;
        if (continuation[0] != '}') {
            g_string_append_c(specification, ',');
        }
        g_string_append_len(specification, continuation,
                            continuation_length);
    }
    if (closing[1]) {
        goto malformed;
    }

    *target_descriptor = g_strndup(specification->str, opening_offset);
    entry = specification->str + opening_offset + 1;
    while (entry <= closing) {
        const char *end = entry;
        bool quoted = false;
        g_autofree char *copy = NULL;
        char *equals;
        char *key;
        char *lookup_value;
        size_t lookup_length;

        while (end < closing) {
            if (*end == '\'') {
                quoted = !quoted;
            } else if (*end == ',' && !quoted) {
                break;
            }
            end++;
        }
        if (quoted) {
            goto malformed;
        }
        copy = g_strndup(entry, end - entry);
        key = g_strstrip(copy);
        if (!key[0]) {
            passthrough = true;
        } else {
            equals = strchr(key, '=');
            lookup_value = equals ? equals + 1 : key;
            if (equals) {
                *equals = '\0';
                key = g_strstrip(key);
                lookup_value = g_strstrip(lookup_value);
            }
            lookup_length = strlen(lookup_value);
            if (lookup_value[0] == '\'') {
                if (lookup_length < 2 ||
                    lookup_value[lookup_length - 1] != '\'') {
                    goto malformed;
                }
                lookup_value[lookup_length - 1] = '\0';
                lookup_value++;
            } else if (strchr(lookup_value, '\'')) {
                goto malformed;
            }
            if (!key[0]) {
                if (fallback) {
                    goto malformed;
                }
                fallback = g_strdup(lookup_value);
            } else if (!strcmp(key, input)) {
                if (selected) {
                    goto malformed;
                }
                selected = g_strdup(lookup_value);
            }
        }
        if (end == closing) {
            break;
        }
        entry = end + 1;
    }

    if (selected) {
        *mapped_value = g_steal_pointer(&selected);
    } else if (fallback) {
        *mapped_value = g_steal_pointer(&fallback);
    } else if (passthrough) {
        *mapped_value = g_strdup(input);
    } else {
        error_setg(errp,
                   "Raspberry Pi DT parameter lookup has no entry for '%s'",
                   input);
        return false;
    }
    return true;

malformed:
    error_setg(errp, "Raspberry Pi DT parameter lookup is malformed");
    return false;
}

static bool raspi_override_requires_postmerge(const char *descriptor)
{
    size_t property_length = strcspn(descriptor, ".;:#[?!={");

    return (property_length == strlen("reg") &&
            !memcmp(descriptor, "reg", property_length)) ||
           (property_length == strlen("name") &&
            !memcmp(descriptor, "name", property_length));
}

static bool raspi_overlay_apply_parameter(void *target_fdt,
                                          const void *override_fdt,
                                          const char *assignment,
                                          RaspiOverridePass pass,
                                          bool *deferred_seen,
                                          size_t *mutation_budget,
                                          Error **errp)
{
    g_autofree char *copy = g_strdup(assignment);
    g_autofree uint8_t *override_copy = NULL;
    char *separator = strchr(copy, '=');
    const char *value = "on";
    const char *parameter;
    const uint8_t *override;
    int overrides;
    int length;
    size_t cursor = 0;

    if (separator) {
        *separator++ = '\0';
        value = g_strstrip(separator);
    }
    parameter = raspi_parameter_alias(g_strstrip(copy));
    if (!parameter[0] || !value[0]) {
        error_setg(errp, "Raspberry Pi DT parameter assignment is empty");
        return false;
    }
    overrides = fdt_path_offset(override_fdt, "/__overrides__");
    if (overrides < 0) {
        error_setg(errp, "Raspberry Pi DT has no __overrides__ node");
        return false;
    }
    override = fdt_getprop(override_fdt, overrides, parameter, &length);
    if (!override) {
        error_setg(errp, "Raspberry Pi DT parameter '%s' is unknown",
                   parameter);
        return false;
    }
    /*
     * Production overlays deliberately publish empty overrides for obsolete
     * parameters that firmware must accept and silently ignore.
     */
    if (length == 0) {
        return true;
    }
    /*
     * Applying one descriptor can move the FDT structure block.  Keep the
     * complete descriptor stream stable while properties or node names in
     * the same overlay are being changed.
     */
    override_copy = g_memdup2(override, length);
    override = override_copy;

    while (cursor < length) {
        uint32_t phandle;
        const char *descriptor;
        size_t available;
        size_t descriptor_length;
        bool embedded;
        int node;

        if (length - cursor < sizeof(uint32_t)) {
            goto malformed;
        }
        phandle = ldl_be_p(override + cursor);
        cursor += sizeof(uint32_t);
        descriptor = (const char *)override + cursor;
        available = length - cursor;
        descriptor_length = strnlen(descriptor, available);
        if (descriptor_length == available) {
            goto malformed;
        }
        cursor += descriptor_length + 1;
        if (strchr(descriptor, '{')) {
            g_autofree char *lookup_descriptor = NULL;
            g_autofree char *lookup_value = NULL;
            bool lookup_embedded = false;
            bool lookup_deferred;

            if (!raspi_overlay_lookup_value(
                    override, length, &cursor, descriptor, value,
                    &lookup_descriptor, &lookup_value, &lookup_embedded,
                    errp)) {
                return false;
            }
            lookup_deferred = lookup_embedded ||
                              raspi_override_requires_postmerge(
                                  lookup_descriptor);
            if (lookup_deferred && deferred_seen) {
                *deferred_seen = true;
            }
            if ((lookup_deferred && pass == RASPI_OVERRIDE_REGULAR) ||
                (!lookup_deferred && pass == RASPI_OVERRIDE_DEFERRED)) {
                continue;
            }
            if (!phandle) {
                if (!raspi_overlay_fragment_ops(
                        target_fdt, lookup_descriptor, lookup_value, errp)) {
                    return false;
                }
                continue;
            }
            node = fdt_node_offset_by_phandle(target_fdt, phandle);
            if (node < 0 || !raspi_overlay_set_target(
                    target_fdt, node, lookup_descriptor, lookup_value,
                    mutation_budget, errp)) {
                if (node < 0) {
                    error_setg(errp,
                               "Raspberry Pi DT lookup target is missing");
                }
                return false;
            }
            continue;
        }
        embedded = descriptor_length && descriptor[descriptor_length - 1] ==
                   '=';
        if (embedded) {
            g_autofree char *cell_descriptor = NULL;
            g_autofree char *cell_value = NULL;
            uint32_t cell;

            if (deferred_seen) {
                *deferred_seen = true;
            }
            if (length - cursor < sizeof(uint32_t)) {
                goto malformed;
            }
            cell = ldl_be_p(override + cursor);
            cursor += sizeof(uint32_t);
            if (pass == RASPI_OVERRIDE_REGULAR) {
                continue;
            }
            if (!phandle) {
                error_setg(errp,
                           "Raspberry Pi embedded-cell override has no target");
                return false;
            }
            cell_descriptor = g_strndup(descriptor,
                                        descriptor_length - 1);
            if (!strchr(cell_descriptor, ':')) {
                error_setg(errp,
                           "Raspberry Pi embedded-cell override is not a cell");
                return false;
            }
            cell_value = g_strdup_printf("0x%08x", cell);
            node = fdt_node_offset_by_phandle(target_fdt, phandle);
            if (node < 0 || !raspi_overlay_set_target(
                    target_fdt, node, cell_descriptor, cell_value,
                    mutation_budget, errp)) {
                if (node < 0) {
                    error_setg(
                        errp,
                        "Raspberry Pi embedded-cell target phandle is missing");
                }
                return false;
            }
            continue;
        }
        if (raspi_override_requires_postmerge(descriptor)) {
            if (deferred_seen) {
                *deferred_seen = true;
            }
            if (pass == RASPI_OVERRIDE_REGULAR) {
                continue;
            }
        } else if (pass == RASPI_OVERRIDE_DEFERRED) {
            continue;
        }
        if (!phandle) {
            if (!raspi_overlay_fragment_ops(target_fdt, descriptor, value,
                                            errp)) {
                return false;
            }
            continue;
        }
        node = fdt_node_offset_by_phandle(target_fdt, phandle);
            if (node < 0 || !raspi_overlay_set_target(
                    target_fdt, node, descriptor, value,
                    mutation_budget, errp)) {
                if (node < 0) {
                    error_setg(
                        errp,
                        "Raspberry Pi DT parameter '%s' target phandle "
                        "0x%" PRIx32 " for '%s' is missing",
                        parameter, phandle, descriptor);
                }
                return false;
            }
    }
    return true;

malformed:
    error_setg(errp, "Raspberry Pi DT parameter '%s' is malformed",
               parameter);
    return false;
}

static bool raspi_overlay_apply_parameter_list(
                                               void *target_fdt,
                                               const void *override_fdt,
                                               const char *list,
                                               RaspiOverridePass pass,
                                               uint32_t *applied,
                                               bool *deferred_seen,
                                               size_t *mutation_budget,
                                               Error **errp)
{
    g_auto(GStrv) assignments = g_strsplit(list, ",", -1);

    for (char **assignment = assignments; *assignment; assignment++) {
        char *trimmed = g_strstrip(*assignment);

        if (!trimmed[0] || !raspi_overlay_apply_parameter(
                target_fdt, override_fdt, trimmed, pass, deferred_seen,
                mutation_budget, errp)) {
            if (!trimmed[0]) {
                error_setg(errp, "Raspberry Pi DT parameter list is empty");
            }
            return false;
        }
        if (pass != RASPI_OVERRIDE_DEFERRED) {
            (*applied)++;
        }
    }
    return true;
}

static uint8_t *raspi_overlay_read(RaspiOverlaySource *source,
                                   const char *path,
                                   size_t *size, RaspiFatResult *result,
                                   Error **errp)
{
    RaspiFatFile file;

    if (source->read_path) {
        return source->read_path(source->read_opaque, path,
                                 RASPI_OVERLAY_MAX_SIZE, size,
                                 result, errp);
    }
    *result = raspi_fat_find_path(source->volume, path, &file, errp);
    if (*result != RASPI_FAT_FOUND) {
        return NULL;
    }
    return raspi_fat_read_file(source->volume, &file, RASPI_OVERLAY_MAX_SIZE,
                               size, errp);
}

static char *raspi_overlay_path(const RaspiFirmwareConfig *config,
                                const char *name, const char *suffix)
{
    return g_strdup_printf("%s%s%s%s", config->os_prefix,
                           config->overlay_prefix, name, suffix);
}

static char *raspi_overlay_mapped_name(
    RaspiOverlaySource *source, const RaspiFirmwareConfig *config,
    const char *name, RaspiOverlayResult *result, Error **errp)
{
    g_autofree char *path = raspi_overlay_path(
        config, "overlay_map", ".dtb");
    g_autofree uint8_t *contents = NULL;
    RaspiFatResult find_result;
    const char *mapped;
    int node;
    int length;
    size_t map_size;

    contents = raspi_overlay_read(source, path, &map_size,
                                  &find_result, errp);
    if (!contents) {
        if (find_result == RASPI_FAT_NOT_FOUND) {
            return g_strdup(name);
        }
        *result = RASPI_OVERLAY_ERROR_INVALID;
        return NULL;
    }
    if (!raspi_overlay_blob_valid(contents, map_size)) {
        error_setg(errp, "Raspberry Pi overlay map '%s' is invalid", path);
        *result = RASPI_OVERLAY_ERROR_INVALID;
        return NULL;
    }

    node = fdt_subnode_offset(contents, 0, name);
    if (node == -FDT_ERR_NOTFOUND) {
        return g_strdup(name);
    }
    if (node < 0) {
        error_setg(errp, "Raspberry Pi overlay map '%s' cannot be read",
                   path);
        *result = RASPI_OVERLAY_ERROR_INVALID;
        return NULL;
    }
    mapped = fdt_getprop(contents, node, "bcm2711", &length);
    if (!mapped || length == 0) {
        return g_strdup(name);
    }
    if (mapped[length - 1] != '\0' || strnlen(mapped, length) != length - 1 ||
        !mapped[0] || strchr(mapped, '/')) {
        error_setg(errp,
                   "Raspberry Pi overlay map entry '%s' is invalid", name);
        *result = RASPI_OVERLAY_ERROR_INVALID;
        return NULL;
    }
    return g_strdup(mapped);
}

static bool raspi_hat_update_device_tree(void *base,
                                         const RaspiHatEeprom *hat,
                                         Error **errp)
{
    int node = fdt_path_offset(base, "/hat");
    int result;

    if (!hat) {
        if (node >= 0 && fdt_del_node(base, node) < 0) {
            error_setg(errp, "Raspberry Pi stale HAT node cannot be removed");
            return false;
        }
        return true;
    }
    if (node == -FDT_ERR_NOTFOUND) {
        node = fdt_add_subnode(base, 0, "hat");
    }
    if (node < 0) {
        error_setg(errp, "Raspberry Pi HAT node cannot be created");
        return false;
    }
    result = fdt_setprop_string(base, node, "vendor", hat->vendor);
    result = result ?: fdt_setprop_string(base, node, "product",
                                          hat->product);
    result = result ?: fdt_setprop_u32(base, node, "product_id",
                                       hat->product_id);
    result = result ?: fdt_setprop_u32(base, node, "product_ver",
                                       hat->product_version);
    result = result ?: fdt_setprop_string(base, node, "uuid", hat->uuid);
    for (unsigned int i = 0; !result && i < hat->custom->len; i++) {
        GBytes *bytes = g_ptr_array_index(hat->custom, i);
        g_autofree char *name = g_strdup_printf("custom_%u", i);
        gsize length;
        const void *data = g_bytes_get_data(bytes, &length);

        result = fdt_setprop(base, node, name, data, length);
    }
    if (result < 0) {
        error_setg(errp, "Raspberry Pi HAT identity cannot be published");
        return false;
    }
    return true;
}

static int raspi_overlay_symbol_target(const void *base, const void *overlay,
                                       const char *fragment_path)
{
    g_autofree char *wanted =
        g_strdup_printf("%s:target:0", fragment_path);
    int fixups = fdt_path_offset(overlay, "/__fixups__");
    int symbols = fdt_path_offset(base, "/__symbols__");
    int property;

    if (fixups < 0 || symbols < 0) {
        return -FDT_ERR_NOTFOUND;
    }
    fdt_for_each_property_offset(property, overlay, fixups) {
        const char *symbol;
        const char *entry;
        const char *symbol_path;
        int length;
        int symbol_path_length;

        entry = fdt_getprop_by_offset(overlay, property, &symbol, &length);
        if (!entry || length <= 0) {
            continue;
        }
        while (length > 0) {
            size_t entry_length = strnlen(entry, length);

            if (entry_length == length) {
                break;
            }
            if (!strcmp(entry, wanted)) {
                symbol_path = fdt_getprop(base, symbols, symbol,
                                          &symbol_path_length);
                if (!symbol_path || symbol_path_length <= 0 ||
                    symbol_path[symbol_path_length - 1] != '\0' ||
                    strlen(symbol_path) + 1 != symbol_path_length) {
                    return -FDT_ERR_BADVALUE;
                }
                return fdt_path_offset(base, symbol_path);
            }
            entry += entry_length + 1;
            length -= entry_length + 1;
        }
    }
    return -FDT_ERR_NOTFOUND;
}

static int raspi_overlay_fragment_target(const void *base,
                                         const void *overlay, int fragment,
                                         const char *fragment_path)
{
    const char *target_path;
    const fdt32_t *target_phandle;
    int length;

    target_path = fdt_getprop(overlay, fragment, "target-path", &length);
    if (target_path) {
        if (length <= 0 || target_path[length - 1] != '\0' ||
            strlen(target_path) + 1 != length) {
            return -FDT_ERR_BADVALUE;
        }
        return fdt_path_offset(base, target_path);
    }

    target_phandle = fdt_getprop(overlay, fragment, "target", &length);
    if (target_phandle && length == sizeof(*target_phandle)) {
        int target = fdt_node_offset_by_phandle(
            base, fdt32_to_cpu(*target_phandle));

        if (target >= 0) {
            return target;
        }
    }
    return raspi_overlay_symbol_target(base, overlay, fragment_path);
}

static bool raspi_overlay_prepare_exports(const void *base, void *overlay,
                                          Error **errp)
{
    g_autoptr(GHashTable) exported = g_hash_table_new(g_str_hash, g_str_equal);
    g_autoptr(GPtrArray) private =
        g_ptr_array_new_with_free_func(g_free);
    int exports = fdt_path_offset(overlay, "/__exports__");
    int symbols = fdt_path_offset(overlay, "/__symbols__");
    int base_symbols = fdt_path_offset(base, "/__symbols__");
    int property;

    if (exports >= 0) {
        fdt_for_each_property_offset(property, overlay, exports) {
            const char *name;
            const void *value;
            const char *path;
            int length;
            int path_length;

            value = fdt_getprop_by_offset(overlay, property, &name, &length);
            if (!value || length != 0 || !name || !name[0]) {
                error_setg(errp,
                           "Raspberry Pi overlay export is not empty");
                return false;
            }
            path = symbols < 0 ? NULL :
                fdt_getprop(overlay, symbols, name, &path_length);
            if (!path || path_length <= 1 ||
                path[path_length - 1] != '\0' ||
                strnlen(path, path_length) != path_length - 1 ||
                path[0] != '/') {
                error_setg(errp,
                           "Raspberry Pi overlay export '%s' has no valid "
                           "symbol", name);
                return false;
            }
            if (base_symbols >= 0 &&
                fdt_getprop(base, base_symbols, name, NULL)) {
                error_setg(errp,
                           "Raspberry Pi overlay export '%s' collides with "
                           "an existing symbol", name);
                return false;
            }
            g_hash_table_add(exported, (void *)name);
        }
    } else if (exports != -FDT_ERR_NOTFOUND) {
        error_setg(errp, "Raspberry Pi overlay exports cannot be read");
        return false;
    }

    if (symbols < 0) {
        return symbols == -FDT_ERR_NOTFOUND;
    }
    fdt_for_each_property_offset(property, overlay, symbols) {
        const char *name;

        if (!fdt_getprop_by_offset(overlay, property, &name, NULL)) {
            error_setg(errp, "Raspberry Pi overlay symbols cannot be read");
            return false;
        }
        if (!g_hash_table_contains(exported, name)) {
            g_ptr_array_add(private, g_strdup(name));
        }
    }
    for (unsigned int i = 0; i < private->len; i++) {
        const char *name = g_ptr_array_index(private, i);

        symbols = fdt_path_offset(overlay, "/__symbols__");
        if (symbols < 0 || fdt_delprop(overlay, symbols, name) < 0) {
            error_setg(errp,
                       "Raspberry Pi overlay private symbol '%s' cannot be "
                       "hidden", name);
            return false;
        }
    }
    return true;
}

/*
 * Overlay tree depth is attacker-controlled, so bound the recursion rather
 * than letting a deeply nested overlay decide the host C stack depth.
 */
static int raspi_overlay_copy_node(const void *source, int source_node,
                                   void *dest, int dest_parent,
                                   const char *name, unsigned int depth)
{
    g_autoptr(GPtrArray) children =
        g_ptr_array_new_with_free_func(g_free);
    int dest_node;
    int property;
    int child;

    if (depth >= RASPI_OVERLAY_MAX_DEPTH) {
        return -FDT_ERR_BADSTRUCTURE;
    }
    dest_node = fdt_add_subnode(dest, dest_parent, name);
    if (dest_node < 0) {
        return dest_node;
    }
    fdt_for_each_property_offset(property, source, source_node) {
        const char *property_name;
        const void *value;
        int length;
        int result;

        value = fdt_getprop_by_offset(source, property, &property_name,
                                      &length);
        if (!value) {
            return length;
        }
        result = fdt_setprop(dest, dest_node, property_name, value, length);
        if (result < 0) {
            return result;
        }
    }
    fdt_for_each_subnode(child, source, source_node) {
        g_ptr_array_add(children,
                        g_strdup(fdt_get_name(source, child, NULL)));
    }
    for (int i = children->len - 1; i >= 0; i--) {
        const char *child_name = g_ptr_array_index(children, i);

        child = fdt_subnode_offset(source, source_node, child_name);
        if (child < 0) {
            return child;
        }
        child = raspi_overlay_copy_node(source, child, dest, dest_node,
                                        child_name, depth + 1);
        if (child < 0) {
            return child;
        }
    }
    return dest_node;
}

static char *raspi_overlay_intra_parent(const void *overlay, int fragment)
{
    const fdt32_t *target;
    char path[512];
    const char *parent_end;
    const char *target_path;
    int target_node;
    int length;

    if (fdt_subnode_offset(overlay, fragment, "__overlay__") < 0) {
        return NULL;
    }
    target = fdt_getprop(overlay, fragment, "target", &length);
    if (!target || length != sizeof(*target)) {
        return NULL;
    }
    target_node = fdt_node_offset_by_phandle(
        overlay, fdt32_to_cpu(*target));
    if (target_node < 0 ||
        fdt_get_path(overlay, target_node, path, sizeof(path)) < 0) {
        return NULL;
    }
    if (!g_str_has_prefix(path, "/fragment@")) {
        return NULL;
    }
    parent_end = strchr(path + 1, '/');
    if (!parent_end) {
        return NULL;
    }
    target_path = parent_end;
    if (!(g_str_has_prefix(target_path, "/__overlay__") &&
          (target_path[sizeof("/__overlay__") - 1] == '\0' ||
           target_path[sizeof("/__overlay__") - 1] == '/')) &&
        !(g_str_has_prefix(target_path, "/__dormant__") &&
          (target_path[sizeof("/__dormant__") - 1] == '\0' ||
           target_path[sizeof("/__dormant__") - 1] == '/'))) {
        return NULL;
    }
    return g_strndup(path + 1, parent_end - path - 1);
}

/*
 * Raspberry Pi firmware applies fragments which target nodes in another
 * fragment before applying regular fragments.  libfdt can produce the same
 * final tree if regular fragments are merged first and those dependent
 * fragments follow: all phandle and external-fixup processing remains native.
 */
static bool raspi_overlay_order_intra_fragments(void **overlayp,
                                                size_t capacity,
                                                Error **errp)
{
    void *overlay = *overlayp;
    g_autoptr(GPtrArray) regular =
        g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) intra =
        g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) sorted =
        g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) ordered =
        g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GHashTable) dependencies = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, g_free);
    g_autoptr(GHashTable) emitted =
        g_hash_table_new(g_str_hash, g_str_equal);
    g_autofree void *ordered_fdt = NULL;
    unsigned int work = 0;
    int child;
    int property;
    int result;

    fdt_for_each_subnode(child, overlay, 0) {
        const char *name = fdt_get_name(overlay, child, NULL);
        g_autofree char *parent = raspi_overlay_intra_parent(overlay, child);

        if (regular->len + intra->len >= RASPI_OVERLAY_MAX_NODES) {
            error_setg(errp,
                       "Raspberry Pi overlay has too many fragments to "
                       "order");
            return false;
        }
        if (parent) {
            g_ptr_array_add(intra, g_strdup(name));
            g_hash_table_insert(dependencies, g_strdup(name),
                                g_steal_pointer(&parent));
        } else {
            g_ptr_array_add(regular, g_strdup(name));
        }
    }
    if (!intra->len) {
        return true;
    }

    while (sorted->len < intra->len) {
        bool progress = false;

        if (work > RASPI_OVERLAY_MAX_ORDER_WORK - intra->len) {
            error_setg(errp,
                       "Raspberry Pi intra-overlay fragment ordering exceeds "
                       "its work budget");
            return false;
        }
        work += intra->len;
        for (unsigned int i = 0; i < intra->len; i++) {
            const char *name = g_ptr_array_index(intra, i);
            const char *parent = g_hash_table_lookup(dependencies, name);

            if (g_hash_table_contains(emitted, name)) {
                continue;
            }
            if (g_hash_table_contains(dependencies, parent) &&
                !g_hash_table_contains(emitted, parent)) {
                continue;
            }
            g_ptr_array_add(sorted, g_strdup(name));
            g_hash_table_add(emitted, (void *)name);
            progress = true;
        }
        if (!progress) {
            error_setg(errp,
                       "Raspberry Pi intra-overlay fragment dependency "
                       "cycle");
            return false;
        }
    }
    for (unsigned int i = 0; i < regular->len; i++) {
        g_ptr_array_add(ordered,
                        g_strdup(g_ptr_array_index(regular, i)));
    }
    for (unsigned int i = 0; i < sorted->len; i++) {
        g_ptr_array_add(ordered, g_strdup(g_ptr_array_index(sorted, i)));
    }

    ordered_fdt = g_malloc0(capacity);
    result = fdt_create_empty_tree(ordered_fdt, capacity);
    if (result < 0) {
        goto fail;
    }
    fdt_for_each_property_offset(property, overlay, 0) {
        const char *name;
        const void *value;
        int length;

        value = fdt_getprop_by_offset(overlay, property, &name, &length);
        if (!value) {
            result = length;
            goto fail;
        }
        result = fdt_setprop(ordered_fdt, 0, name, value, length);
        if (result < 0) {
            goto fail;
        }
    }
    for (int i = ordered->len - 1; i >= 0; i--) {
        const char *name = g_ptr_array_index(ordered, i);

        child = fdt_subnode_offset(overlay, 0, name);
        if (child < 0) {
            result = child;
            goto fail;
        }
        result = raspi_overlay_copy_node(overlay, child, ordered_fdt, 0,
                                         name, 0);
        if (result < 0) {
            goto fail;
        }
    }
    g_free(*overlayp);
    *overlayp = g_steal_pointer(&ordered_fdt);
    return true;

fail:
    error_setg(errp,
               "Raspberry Pi intra-overlay fragments cannot be ordered: %s",
               fdt_strerror(result));
    return false;
}

/*
 * Raspberry Pi firmware treats an overlay assignment to bootargs specially:
 * the new text is appended instead of replacing the base property.  libfdt's
 * generic overlay merge cannot express that rule, so apply it to the base and
 * remove it from each enabled fragment before the generic merge.
 */
static bool raspi_overlay_append_bootargs(void *base, void *overlay,
                                          Error **errp)
{
    g_autoptr(GPtrArray) fragments =
        g_ptr_array_new_with_free_func(g_free);
    int fragment;

    fdt_for_each_subnode(fragment, overlay, 0) {
        const char *name = fdt_get_name(overlay, fragment, NULL);
        int node = fdt_subnode_offset(overlay, fragment, "__overlay__");

        if (name && g_str_has_prefix(name, "fragment@") && node >= 0 &&
            fdt_getprop(overlay, node, "bootargs", NULL)) {
            g_ptr_array_add(fragments, g_strdup(name));
        }
    }

    for (unsigned int i = 0; i < fragments->len; i++) {
        const char *name = g_ptr_array_index(fragments, i);
        g_autofree char *path = g_strdup_printf("/%s", name);
        const char *base_args;
        const char *overlay_args;
        g_autofree char *combined = NULL;
        int base_length;
        int overlay_length;
        int target;
        int node;

        fragment = fdt_path_offset(overlay, path);
        node = fdt_subnode_offset(overlay, fragment, "__overlay__");
        target = raspi_overlay_fragment_target(base, overlay, fragment, path);
        if (target < 0) {
            /* Intra-overlay targets remain for libfdt's normal handling. */
            continue;
        }
        overlay_args = fdt_getprop(overlay, node, "bootargs",
                                   &overlay_length);
        if (!overlay_args || overlay_length <= 0 ||
            overlay_args[overlay_length - 1] != '\0' ||
            strlen(overlay_args) + 1 != overlay_length) {
            error_setg(errp,
                       "Raspberry Pi overlay %s bootargs is not a string",
                       name);
            return false;
        }
        base_args = fdt_getprop(base, target, "bootargs", &base_length);
        if (base_args &&
            (base_length <= 0 || base_args[base_length - 1] != '\0' ||
             strlen(base_args) + 1 != base_length)) {
            error_setg(errp,
                       "Raspberry Pi overlay %s target bootargs is not a string",
                       name);
            return false;
        }
        if (!base_args || !base_args[0] || !overlay_args[0]) {
            combined = g_strconcat(base_args ? base_args : "",
                                   overlay_args, NULL);
        } else {
            combined = g_strconcat(base_args, " ", overlay_args, NULL);
        }
        if (fdt_setprop_string(base, target, "bootargs", combined) < 0 ||
            fdt_delprop(overlay, node, "bootargs") < 0) {
            error_setg(errp,
                       "Raspberry Pi overlay %s bootargs cannot be appended",
                       name);
            return false;
        }
    }
    return true;
}

static RaspiOverlayResult raspi_overlay_flush(
    RaspiOverlaySource *source, const RaspiFirmwareConfig *config,
    RaspiPendingOverlay *pending, void **base, size_t *capacity,
    RaspiOverlayState *state, Error **errp)
{
    g_autofree char *path = NULL;
    g_autofree char *mapped_name = NULL;
    g_autofree uint8_t *contents = NULL;
    void *overlay;
    void *expanded;
    size_t size;
    size_t overlay_capacity;
    RaspiFatResult find_result;
    g_autoptr(GPtrArray) deferred_parameters =
        g_ptr_array_new();
    int result;

    if (!pending->name) {
        return RASPI_OVERLAY_READY;
    }
    if (state->overlays_applied >= RASPI_OVERLAY_MAX_COUNT) {
        error_setg(errp, "Raspberry Pi overlay count exceeds %u",
                   RASPI_OVERLAY_MAX_COUNT);
        return RASPI_OVERLAY_ERROR_LIMIT;
    }

    if (pending->embedded) {
        path = g_strdup("hat-eeprom:embedded");
        size = pending->embedded_size;
        contents = g_memdup2(pending->embedded, size);
    } else {
        mapped_name = raspi_overlay_mapped_name(
            source, config, pending->name, &result, errp);
        if (!mapped_name) {
            return result;
        }
        path = raspi_overlay_path(
            config, mapped_name,
            g_str_has_suffix(mapped_name, ".dtbo") ? "" : ".dtbo");
        contents = raspi_overlay_read(source, path, &size, &find_result,
                                      errp);
        if (!contents && find_result == RASPI_FAT_NOT_FOUND &&
            !g_str_has_suffix(mapped_name, ".dtbo")) {
            g_clear_pointer(&path, g_free);
            path = raspi_overlay_path(config, mapped_name, "-overlay.dtb");
            contents = raspi_overlay_read(source, path, &size, &find_result,
                                          errp);
        }
        if (!contents) {
            if (find_result == RASPI_FAT_NOT_FOUND) {
                error_setg(errp, "Raspberry Pi overlay '%s' was not found",
                           path);
                return RASPI_OVERLAY_ERROR_MISSING;
            }
            return RASPI_OVERLAY_ERROR_INVALID;
        }
    }
    if (!raspi_overlay_blob_valid(contents, size)) {
        error_setg(errp, "Raspberry Pi overlay '%s' is invalid", path);
        return RASPI_OVERLAY_ERROR_INVALID;
    }

    QEMU_BUILD_BUG_ON(RASPI_OVERLAY_MAX_SIZE >
                      SIZE_MAX - RASPI_OVERLAY_DTB_SLACK);
    overlay_capacity = size + RASPI_OVERLAY_DTB_SLACK;
    overlay = g_malloc0(overlay_capacity);
    result = fdt_open_into(contents, overlay, overlay_capacity);
    if (result < 0) {
        g_free(overlay);
        error_setg(errp, "Raspberry Pi overlay '%s' cannot be expanded: %s",
                   path, fdt_strerror(result));
        return RASPI_OVERLAY_ERROR_INVALID;
    }
    for (unsigned int i = 0; i < pending->parameters->len; i++) {
        const char *parameter = g_ptr_array_index(pending->parameters, i);
        bool deferred_seen = false;

        if (!raspi_overlay_apply_parameter_list(
                overlay, overlay, parameter, RASPI_OVERRIDE_REGULAR,
                &state->parameters_applied, &deferred_seen,
                &state->mutation_budget, errp)) {
            g_free(overlay);
            return RASPI_OVERLAY_ERROR_PARAMETER;
        }
        if (deferred_seen) {
            g_ptr_array_add(deferred_parameters, (void *)parameter);
        }
    }
    if (!raspi_overlay_prepare_exports(*base, overlay, errp) ||
        !raspi_overlay_order_intra_fragments(&overlay, overlay_capacity,
                                             errp)) {
        g_free(overlay);
        return RASPI_OVERLAY_ERROR_APPLY;
    }

    if (*capacity > SIZE_MAX - size - RASPI_OVERLAY_DTB_SLACK) {
        g_free(overlay);
        return RASPI_OVERLAY_ERROR_LIMIT;
    }
    *capacity += size + RASPI_OVERLAY_DTB_SLACK;
    expanded = g_malloc0(*capacity);
    result = fdt_open_into(*base, expanded, *capacity);
    g_free(*base);
    *base = expanded;
    if (result < 0) {
        g_free(overlay);
        error_setg(errp, "Raspberry Pi base DT cannot grow for overlay: %s",
                   fdt_strerror(result));
        return RASPI_OVERLAY_ERROR_LIMIT;
    }
    if (!raspi_overlay_append_bootargs(*base, overlay, errp)) {
        g_free(overlay);
        return RASPI_OVERLAY_ERROR_APPLY;
    }
    result = fdt_overlay_apply(*base, overlay);
    if (result < 0) {
        g_free(overlay);
        error_setg(errp, "Raspberry Pi overlay '%s' failed: %s", path,
                   fdt_strerror(result));
        return RASPI_OVERLAY_ERROR_APPLY;
    }
    /*
     * libfdt deliberately invalidates the overlay header after applying it.
     * Its structure and resolved local/external fixups are still needed for
     * Raspberry Pi embedded-cell overrides, and the blob is freed below.
     */
    fdt_set_magic(overlay, FDT_MAGIC);
    for (unsigned int i = 0; i < deferred_parameters->len; i++) {
        const char *parameter = g_ptr_array_index(deferred_parameters, i);

        if (!raspi_overlay_apply_parameter_list(
                *base, overlay, parameter, RASPI_OVERRIDE_DEFERRED,
                &state->parameters_applied, NULL,
                &state->mutation_budget, errp)) {
            g_free(overlay);
            return RASPI_OVERLAY_ERROR_PARAMETER;
        }
    }
    g_free(overlay);
    state->overlays_applied++;
    g_free(state->last_overlay_file);
    state->last_overlay_file = g_steal_pointer(&path);
    return RASPI_OVERLAY_READY;
}

static void raspi_pending_overlay_clear(RaspiPendingOverlay *pending)
{
    g_clear_pointer(&pending->name, g_free);
    pending->embedded = NULL;
    pending->embedded_size = 0;
    if (pending->parameters) {
        g_ptr_array_set_size(pending->parameters, 0);
    }
}

static RaspiOverlayResult raspi_overlay_apply_config_source(
    RaspiOverlaySource *source, const RaspiFirmwareConfig *config,
    const uint8_t *hat_eeprom, size_t hat_eeprom_size,
    const uint8_t *device_tree, size_t device_tree_size,
    uint8_t **result_tree, size_t *result_size,
    RaspiOverlayState *state, Error **errp)
{
    RaspiPendingOverlay pending = {
        .parameters = g_ptr_array_new_with_free_func(g_free),
    };
    RaspiOverlayResult result = RASPI_OVERLAY_READY;
    size_t capacity;
    void *base;
    RaspiHatEeprom hat = { 0 };
    bool hat_present = false;
    bool hat_suppressed = false;
    bool overlay_scope = false;
    int fdt_result;

    raspi_overlay_state_clear(state);
    if (!raspi_overlay_blob_valid(device_tree, device_tree_size)) {
        error_setg(errp, "Raspberry Pi base DT is invalid");
        g_ptr_array_unref(pending.parameters);
        return RASPI_OVERLAY_ERROR_INVALID;
    }
    QEMU_BUILD_BUG_ON(RASPI_OVERLAY_MAX_SIZE >
                      SIZE_MAX - RASPI_OVERLAY_DTB_SLACK);
    capacity = device_tree_size + RASPI_OVERLAY_DTB_SLACK;
    base = g_malloc0(capacity);
    state->mutation_budget = RASPI_OVERLAY_MAX_MUTATION_BYTES;
    fdt_result = fdt_open_into(device_tree, base, capacity);
    if (fdt_result < 0) {
        error_setg(errp, "Raspberry Pi base DT is invalid: %s",
                   fdt_strerror(fdt_result));
        result = RASPI_OVERLAY_ERROR_INVALID;
        goto out;
    }

    if (hat_eeprom && config->force_eeprom_read) {
        if (!raspi_hat_eeprom_parse(hat_eeprom, hat_eeprom_size,
                                    &hat, errp)) {
            result = RASPI_OVERLAY_ERROR_INVALID;
            goto out;
        }
        hat_present = true;
        if (config->dt_commands->len) {
            const char *first = g_ptr_array_index(config->dt_commands, 0);

            hat_suppressed = !strcmp(first, "overlay=");
        }
    }
    if (!raspi_hat_update_device_tree(base, hat_present ? &hat : NULL,
                                      errp)) {
        result = RASPI_OVERLAY_ERROR_APPLY;
        goto out;
    }
    if (hat_present && !hat_suppressed &&
        (hat.overlay || hat.overlay_name)) {
        pending.name = g_strdup(hat.overlay_name ?
                                hat.overlay_name : "hat-eeprom");
        pending.embedded = hat.overlay;
        pending.embedded_size = hat.overlay_size;
        overlay_scope = true;
    }
    if (hat_present && hat.gpio_valid) {
        state->hat_gpio_valid = true;
        memcpy(state->hat_gpio_map, hat.gpio_map,
               sizeof(state->hat_gpio_map));
    }
    if (hat_present) {
        state->hat_present = true;
        state->hat_eeprom_size = hat_eeprom_size;
        state->hat_eeprom_declared_size = hat.declared_size;
        state->hat_eeprom_sha256 = g_compute_checksum_for_data(
            G_CHECKSUM_SHA256, hat_eeprom, hat_eeprom_size);
        state->hat_content_sha256 = g_compute_checksum_for_data(
            G_CHECKSUM_SHA256, hat_eeprom, hat.declared_size);
        state->hat_vendor = g_strdup(hat.vendor);
        state->hat_product = g_strdup(hat.product);
        state->hat_uuid = g_strdup(hat.uuid);
        state->hat_product_id = hat.product_id;
        state->hat_product_version = hat.product_version;
        state->hat_custom_count = hat.custom->len;
        state->hat_overlay = g_strdup(
            hat.overlay ? "embedded" :
            hat.overlay_name ? hat.overlay_name : "none");
    }

    for (unsigned int i = 0; i < config->dt_commands->len; i++) {
        const char *command = g_ptr_array_index(config->dt_commands, i);
        const char *value = strchr(command, '=') + 1;

        if (g_str_has_prefix(command, "overlay=")) {
            result = raspi_overlay_flush(source, config, &pending,
                                         &base, &capacity, state, errp);
            raspi_pending_overlay_clear(&pending);
            if (result != RASPI_OVERLAY_READY) {
                goto out;
            }
            overlay_scope = value[0];
            if (overlay_scope) {
                g_auto(GStrv) fields = g_strsplit(value, ",", -1);

                pending.name = g_strdup(g_strstrip(fields[0]));
                if (!pending.name[0]) {
                    error_setg(errp, "Raspberry Pi overlay name is empty");
                    result = RASPI_OVERLAY_ERROR_INVALID;
                    goto out;
                }
                for (char **field = fields + 1; *field; field++) {
                    char *trimmed = g_strstrip(*field);

                    if (!trimmed[0]) {
                        error_setg(errp,
                                   "Raspberry Pi overlay parameter is empty");
                        result = RASPI_OVERLAY_ERROR_PARAMETER;
                        goto out;
                    }
                    g_ptr_array_add(pending.parameters, g_strdup(trimmed));
                }
            }
        } else if (g_str_has_prefix(command, "param=")) {
            if (overlay_scope) {
                g_ptr_array_add(pending.parameters, g_strdup(value));
            } else if (!raspi_overlay_apply_parameter_list(
                           base, base, value, RASPI_OVERRIDE_ALL,
                           &state->parameters_applied, NULL,
                           &state->mutation_budget, errp)) {
                result = RASPI_OVERLAY_ERROR_PARAMETER;
                goto out;
            }
        } else {
            g_assert_not_reached();
        }
    }
    result = raspi_overlay_flush(source, config, &pending,
                                 &base, &capacity, state, errp);
    if (result != RASPI_OVERLAY_READY) {
        goto out;
    }
    fdt_result = fdt_pack(base);
    if (fdt_result < 0) {
        error_setg(errp, "Raspberry Pi final overlay DT cannot be packed");
        result = RASPI_OVERLAY_ERROR_APPLY;
        goto out;
    }
    *result_size = fdt_totalsize(base);
    *result_tree = g_realloc(base, *result_size);
    base = NULL;
    state->final_dtb_sha256 = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256, *result_tree, *result_size);

out:
    raspi_pending_overlay_clear(&pending);
    g_ptr_array_unref(pending.parameters);
    g_free(base);
    raspi_hat_eeprom_clear(&hat);
    if (result != RASPI_OVERLAY_READY) {
        raspi_overlay_state_clear(state);
    }
    return result;
}

RaspiOverlayResult raspi_overlay_apply_config(
    RaspiFatVolume *volume, const RaspiFirmwareConfig *config,
    const uint8_t *hat_eeprom, size_t hat_eeprom_size,
    const uint8_t *device_tree, size_t device_tree_size,
    uint8_t **result_tree, size_t *result_size,
    RaspiOverlayState *state, Error **errp)
{
    RaspiOverlaySource source = { .volume = volume };

    return raspi_overlay_apply_config_source(
        &source, config, hat_eeprom, hat_eeprom_size,
        device_tree, device_tree_size, result_tree,
        result_size, state, errp);
}

RaspiOverlayResult raspi_overlay_apply_config_with_reader(
    const RaspiFirmwareConfig *config,
    const uint8_t *hat_eeprom, size_t hat_eeprom_size,
    const uint8_t *device_tree,
    size_t device_tree_size, RaspiFirmwareReadPath *read_path,
    void *read_opaque, uint8_t **result_tree, size_t *result_size,
    RaspiOverlayState *state, Error **errp)
{
    RaspiOverlaySource source = {
        .read_path = read_path,
        .read_opaque = read_opaque,
    };

    return raspi_overlay_apply_config_source(
        &source, config, hat_eeprom, hat_eeprom_size,
        device_tree, device_tree_size, result_tree,
        result_size, state, errp);
}

void raspi_overlay_state_clear(RaspiOverlayState *state)
{
    g_free(state->hat_eeprom_sha256);
    g_free(state->hat_content_sha256);
    g_free(state->hat_vendor);
    g_free(state->hat_product);
    g_free(state->hat_uuid);
    g_free(state->hat_overlay);
    g_free(state->last_overlay_file);
    g_free(state->final_dtb_sha256);
    memset(state, 0, sizeof(*state));
}
