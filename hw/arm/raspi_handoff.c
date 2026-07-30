/*
 * Raspberry Pi behavioral firmware-to-ARM handoff
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "qemu/guest-random.h"
#include "qemu/units.h"
#include "system/address-spaces.h"
#include "hw/arm/bcm2838.h"
#include "hw/arm/raspi_handoff.h"
#include "hw/core/loader.h"
#include "system/device_tree.h"
#include <libfdt.h>

#define RASPI_ARM64_MAGIC_OFFSET 56
#define RASPI_ARM64_HEADER_SIZE 64
#define RASPI_ARM64_TEXT_OFFSET 8
#define RASPI_ARM64_IMAGE_SIZE_OFFSET 16
#define RASPI_ARM64_FLAGS_OFFSET 24
#define RASPI_ARM64_FLAG_BIG_ENDIAN BIT_ULL(0)
#define RASPI_ARM64_DEFAULT_OFFSET 0x80000
#define RASPI_ARM64_MIN_BASE (2 * MiB)
#define RASPI_ARM64_LOAD_LIMIT 0x2f000000
#define RASPI_ARM64_DTB_MAX_SIZE (2 * MiB)
#define RASPI_ARM64_DTB_SLACK (64 * KiB)
#define RASPI_ARM64_SPIN_CODE_ADDRESS 0x300
#define RASPI_ARM64_SPIN_TABLE_ADDRESS 0xd8
#define RASPI_ARM64_SPIN_CODE_SIZE (11 * sizeof(uint32_t))
#define RASPI_ARM32_ENTRY_SIZE (5 * sizeof(uint32_t))
#define RASPI_ARM32_SPIN_CODE_SIZE (10 * sizeof(uint32_t))
#define RASPI_SPIN_TABLE_SIZE (4 * sizeof(uint64_t))
#define RASPI_ARM32_ZIMAGE_MAGIC_OFFSET 0x24
#define RASPI_ARM32_ZIMAGE_START_OFFSET 0x28
#define RASPI_ARM32_ZIMAGE_END_OFFSET 0x2c
#define RASPI_ARM32_ZIMAGE_MAGIC 0x016f2818
#define RASPI_ARM32_KERNEL_ADDRESS 0x8000
#define RASPI_ARM32_ENTRY_ADDRESS 0
#define RASPI_ARM32_SPIN_CODE_ADDRESS 0x300
#define RASPI_ARM32_MAILBOX3_CLEAR_BASE 0xff8000cc

static bool raspi_handoff_range_valid(hwaddr address, uint64_t size,
                                      hwaddr limit)
{
    return size && address < limit && size <= limit - address;
}

static bool raspi_handoff_ranges_overlap(hwaddr first, uint64_t first_size,
                                         hwaddr second, uint64_t second_size)
{
    return first < second + second_size && second < first + first_size;
}

static bool raspi_handoff_overlaps_bootloader_data(
    const RaspiArmHandoff *handoff, hwaddr address, uint64_t size)
{
    return (handoff->bootloader_config_size &&
            raspi_handoff_ranges_overlap(
                handoff->bootloader_config_address,
                handoff->bootloader_config_size, address, size)) ||
           (handoff->bootloader_public_key_size &&
            raspi_handoff_ranges_overlap(
                handoff->bootloader_public_key_address,
                handoff->bootloader_public_key_size, address, size));
}

static bool raspi_handoff_overlaps_firmware_state(
    const RaspiArmHandoff *handoff, hwaddr address, uint64_t size)
{
    uint64_t spin_code_size = handoff->arm_64bit ?
        RASPI_ARM64_SPIN_CODE_SIZE : RASPI_ARM32_SPIN_CODE_SIZE;

    return raspi_handoff_ranges_overlap(
               RASPI_ARM64_SPIN_TABLE_ADDRESS, RASPI_SPIN_TABLE_SIZE,
               address, size) ||
           raspi_handoff_ranges_overlap(
               RASPI_ARM64_SPIN_CODE_ADDRESS, spin_code_size,
               address, size) ||
           (!handoff->arm_64bit &&
            raspi_handoff_ranges_overlap(
                RASPI_ARM32_ENTRY_ADDRESS, RASPI_ARM32_ENTRY_SIZE,
                address, size));
}

static RaspiHandoffResult raspi_prepare_kernel(
    RaspiArmHandoff *handoff, bool arm_64bit,
    const uint8_t *kernel, size_t kernel_size, Error **errp)
{
    uint8_t *image = g_memdup2(kernel, kernel_size);
    ssize_t image_size = kernel_size;
    uint64_t text_offset;
    uint64_t reserve_size;

    if (kernel_size >= 2 && kernel[0] == 0x1f && kernel[1] == 0x8b) {
        uint8_t *uncompressed = g_malloc(LOAD_IMAGE_MAX_DECOMPRESSED_BYTES);

        image_size = gunzip(uncompressed, LOAD_IMAGE_MAX_DECOMPRESSED_BYTES,
                            image, kernel_size);
        g_free(image);
        if (image_size < 0) {
            g_free(uncompressed);
            error_setg(errp, "Raspberry Pi kernel gzip stream is invalid");
            return RASPI_HANDOFF_ERROR_KERNEL;
        }
        image = g_realloc(uncompressed, image_size);
    } else if (arm_64bit) {
        ssize_t unpacked = unpack_efi_zboot_image(&image, &image_size);

        if (unpacked < 0) {
            g_free(image);
            error_setg(errp, "Raspberry Pi EFI zboot kernel is invalid");
            return RASPI_HANDOFF_ERROR_KERNEL;
        }
    }

    handoff->arm_64bit = arm_64bit;
    if (arm_64bit) {
        if (image_size < RASPI_ARM64_HEADER_SIZE ||
            memcmp(image + RASPI_ARM64_MAGIC_OFFSET, "ARM\x64", 4)) {
            g_free(image);
            error_setg(errp,
                       "Raspberry Pi kernel has no arm64 Image header");
            return RASPI_HANDOFF_ERROR_KERNEL;
        }

        reserve_size = ldq_le_p(image + RASPI_ARM64_IMAGE_SIZE_OFFSET);
        handoff->big_endian =
            ldq_le_p(image + RASPI_ARM64_FLAGS_OFFSET) &
            RASPI_ARM64_FLAG_BIG_ENDIAN;
        if (reserve_size) {
            text_offset = ldq_le_p(image + RASPI_ARM64_TEXT_OFFSET);
        } else {
            text_offset = RASPI_ARM64_DEFAULT_OFFSET;
            reserve_size = image_size;
        }
        reserve_size = MAX(reserve_size, (uint64_t)image_size);
        if (text_offset < 4 * KiB &&
            uadd64_overflow(RASPI_ARM64_MIN_BASE, text_offset,
                            &handoff->kernel_address)) {
            g_free(image);
            error_setg(errp, "Raspberry Pi kernel text offset overflows");
            return RASPI_HANDOFF_ERROR_LAYOUT;
        }
        if (text_offset >= 4 * KiB) {
            handoff->kernel_address = text_offset;
        }
        handoff->entry_address = handoff->kernel_address;
    } else {
        uint32_t zimage_start;
        uint32_t zimage_end;

        if (image_size < RASPI_ARM32_ZIMAGE_END_OFFSET + 4 ||
            ldl_le_p(image + RASPI_ARM32_ZIMAGE_MAGIC_OFFSET) !=
            RASPI_ARM32_ZIMAGE_MAGIC) {
            g_free(image);
            error_setg(errp,
                       "Raspberry Pi kernel has no ARM zImage header");
            return RASPI_HANDOFF_ERROR_KERNEL;
        }
        zimage_start = ldl_le_p(
            image + RASPI_ARM32_ZIMAGE_START_OFFSET);
        zimage_end = ldl_le_p(image + RASPI_ARM32_ZIMAGE_END_OFFSET);
        if (zimage_end <= zimage_start ||
            zimage_end - zimage_start > image_size) {
            g_free(image);
            error_setg(errp, "Raspberry Pi ARM zImage extent is invalid");
            return RASPI_HANDOFF_ERROR_KERNEL;
        }
        reserve_size = image_size;
        handoff->kernel_address = RASPI_ARM32_KERNEL_ADDRESS;
        handoff->entry_address = RASPI_ARM32_ENTRY_ADDRESS;
    }
    if (!raspi_handoff_range_valid(handoff->kernel_address, reserve_size,
                                   RASPI_ARM64_LOAD_LIMIT)) {
        g_free(image);
        error_setg(errp, "Raspberry Pi kernel layout exceeds low RAM");
        return RASPI_HANDOFF_ERROR_LAYOUT;
    }

    handoff->kernel = image;
    handoff->kernel_size = image_size;
    handoff->kernel_reserve_size = reserve_size;
    return RASPI_HANDOFF_READY;
}

static bool raspi_fdt_add_memory(void *fdt, uint32_t acells,
                                 uint32_t scells, const char *path,
                                 uint64_t base, uint64_t size)
{
    return qemu_fdt_add_subnode(fdt, path) >= 0 &&
           qemu_fdt_setprop_string(fdt, path, "device_type",
                                   "memory") >= 0 &&
           qemu_fdt_setprop_sized_cells(fdt, path, "reg",
                                        acells, base,
                                        scells, size) >= 0;
}

static bool raspi_fdt_add_memory_range(void *fdt, uint32_t acells,
                                       uint32_t scells, uint64_t base,
                                       uint64_t size)
{
    /*
     * Production BCM2711 DTBs use #size-cells = <1>.  The 8 GiB model has
     * 4 GiB + 64 MiB above the 32-bit peripheral hole, which therefore
     * cannot be represented as one reg tuple.  Split it into page-aligned
     * 2 GiB banks; adjacent memory nodes are equivalent to one contiguous
     * range and remain representable by both one- and two-cell DTBs.
     */
    uint64_t max_bank = scells == 1 ? 2 * GiB : UINT64_MAX;

    while (size) {
        g_autofree char *path =
            g_strdup_printf("/memory@%" PRIx64, base);
        uint64_t bank = MIN(size, max_bank);

        if (!raspi_fdt_add_memory(fdt, acells, scells, path, base, bank)) {
            return false;
        }
        base += bank;
        size -= bank;
    }
    return true;
}

static bool raspi_fdt_replace_memory(
    void *fdt, const RaspiFirmwareConfig *config,
    const struct arm_boot_info *binfo, Error **errp)
{
    uint32_t acells = qemu_fdt_getprop_cell(fdt, "/", "#address-cells",
                                            NULL, errp);
    uint32_t scells = qemu_fdt_getprop_cell(fdt, "/", "#size-cells",
                                            NULL, errp);
    uint64_t installed = config->installed_mem_mb ?
                         (uint64_t)config->installed_mem_mb * MiB :
                         binfo->ram_size;
    uint64_t total = config->total_mem_set ?
                     (uint64_t)config->total_mem_mb * MiB : installed;
    uint64_t reserved = config->gpu_mem_effective_mb ?
                        (uint64_t)config->gpu_mem_effective_mb * MiB :
                        1 * GiB - binfo->ram_size;
    uint64_t lower = MIN(total, 1 * GiB);
    uint64_t middle = 0;
    uint64_t high = 0;
    int offset;

    if (!acells || !scells || binfo->ram_size > 1 * GiB ||
        lower <= reserved) {
        error_setg(errp, "Raspberry Pi DTB has invalid address/size cells");
        return false;
    }
    lower -= reserved;
    if (total > 1 * GiB) {
        middle = MIN(total, (uint64_t)BCM2838_PERI_LOW_BASE) - 1 * GiB;
    }
    if (total > BCM2838_PERI_LOW_BASE) {
        high = total - BCM2838_PERI_LOW_BASE;
    }
    while ((offset = fdt_node_offset_by_prop_value(
                fdt, -1, "device_type", "memory", sizeof("memory"))) >= 0) {
        if (fdt_nop_node(fdt, offset) < 0) {
            error_setg(errp, "could not remove Raspberry Pi DT memory node");
            return false;
        }
    }
    if (offset != -FDT_ERR_NOTFOUND) {
        error_setg(errp, "could not inspect Raspberry Pi DT memory nodes");
        return false;
    }
    if (!raspi_fdt_add_memory(fdt, acells, scells,
                              "/memory@0", 0, lower) ||
        (middle &&
         !raspi_fdt_add_memory(fdt, acells, scells,
                               "/memory@40000000", 1 * GiB,
                               middle)) ||
        (high &&
         !raspi_fdt_add_memory_range(fdt, acells, scells,
                                     4 * GiB, high))) {
        error_setg(errp, "could not create Raspberry Pi DT memory node");
        return false;
    }
    return true;
}

static bool raspi_fdt_set_reserved_blob(void *fdt, const char *compatible,
                                        hwaddr address, uint64_t size,
                                        Error **errp)
{
    uint32_t cells[4];
    int address_cells;
    int size_cells;
    int parent;
    int node;
    int length;
    const fdt32_t *property;
    unsigned int index = 0;

    node = fdt_node_offset_by_compatible(fdt, -1, compatible);
    if (node == -FDT_ERR_NOTFOUND) {
        return true;
    }
    if (node < 0) {
        error_setg(errp, "could not inspect Raspberry Pi DT %s node",
                   compatible);
        return false;
    }
    parent = fdt_parent_offset(fdt, node);
    if (parent < 0) {
        error_setg(errp, "Raspberry Pi DT %s node has no parent",
                   compatible);
        return false;
    }
    property = fdt_getprop(fdt, parent, "#address-cells", &length);
    if (!property || length != sizeof(*property)) {
        error_setg(errp, "Raspberry Pi DT %s address cells are invalid",
                   compatible);
        return false;
    }
    address_cells = fdt32_to_cpu(*property);
    property = fdt_getprop(fdt, parent, "#size-cells", &length);
    if (!property || length != sizeof(*property)) {
        error_setg(errp, "Raspberry Pi DT %s size cells are invalid",
                   compatible);
        return false;
    }
    size_cells = fdt32_to_cpu(*property);
    if (address_cells < 1 || address_cells > 2 ||
        size_cells < 1 || size_cells > 2) {
        error_setg(errp, "Raspberry Pi DT %s cell widths are unsupported",
                   compatible);
        return false;
    }
    if (address_cells == 2) {
        cells[index++] = cpu_to_be32(address >> 32);
    }
    cells[index++] = cpu_to_be32(address);
    if (size_cells == 2) {
        cells[index++] = cpu_to_be32(size >> 32);
    }
    cells[index++] = cpu_to_be32(size);
    if (fdt_setprop(fdt, node, "reg", cells,
                    index * sizeof(cells[0])) < 0 ||
        fdt_setprop_string(fdt, node, "status", "okay") < 0) {
        error_setg(errp, "could not expose Raspberry Pi DT %s data",
                   compatible);
        return false;
    }
    return true;
}

static char *raspi_handoff_cmdline(const uint8_t *contents, size_t size,
                                   Error **errp)
{
    const uint8_t *nul;
    const uint8_t *line_end;
    size_t line_size;
    char *cmdline;

    if (!contents || !size) {
        return NULL;
    }
    nul = memchr(contents, 0, size);
    if (nul && nul != contents + size - 1) {
        error_setg(errp, "Raspberry Pi command line contains embedded NUL");
        return NULL;
    }
    line_end = memchr(contents, '\n', nul ? nul - contents : size);
    line_size = line_end ? line_end - contents :
                (nul ? nul - contents : size);
    if (line_size && contents[line_size - 1] == '\r') {
        line_size--;
    }
    cmdline = g_strndup((const char *)contents, line_size);
    g_strchomp(cmdline);
    return cmdline;
}

static char *raspi_handoff_uart_name(const void *fdt, const char *alias)
{
    const char *path = fdt_get_alias(fdt, alias);
    unsigned int index;
    int node;

    if (!path || sscanf(alias, "serial%u", &index) != 1) {
        return NULL;
    }
    node = fdt_path_offset(fdt, path);
    if (node < 0) {
        return NULL;
    }
    if (!fdt_node_check_compatible(fdt, node, "brcm,bcm2835-aux-uart")) {
        return g_strdup_printf("ttyS%u", index);
    }
    if (!fdt_node_check_compatible(fdt, node, "arm,pl011") ||
        !fdt_node_check_compatible(fdt, node, "arm,pl011-axi")) {
        return g_strdup_printf("ttyAMA%u", index);
    }
    return NULL;
}

static char *raspi_handoff_replace_serial_alias(const char *cmdline,
                                                const char *alias,
                                                const char *uart)
{
    g_autofree char *needle = g_strdup_printf("=%s", alias);
    g_autofree char *replacement = g_strdup_printf("=%s", uart);
    g_autoptr(GString) result = g_string_new(NULL);
    const char *cursor = cmdline;
    size_t needle_length = strlen(needle);

    while (true) {
        const char *match = strstr(cursor, needle);

        if (!match) {
            g_string_append(result, cursor);
            break;
        }
        if (match[needle_length] && match[needle_length] != ',' &&
            !g_ascii_isspace(match[needle_length])) {
            g_string_append_len(result, cursor,
                                match + needle_length - cursor);
            cursor = match + needle_length;
            continue;
        }
        g_string_append_len(result, cursor, match - cursor);
        g_string_append(result, replacement);
        cursor = match + needle_length;
    }
    return g_string_free(g_steal_pointer(&result), false);
}

static char *raspi_handoff_resolve_serial_aliases(const void *fdt,
                                                  char *cmdline)
{
    for (unsigned int index = 0; index < 2; index++) {
        g_autofree char *alias = g_strdup_printf("serial%u", index);
        g_autofree char *uart = raspi_handoff_uart_name(fdt, alias);
        char *resolved;

        if (!uart) {
            continue;
        }
        resolved = raspi_handoff_replace_serial_alias(cmdline, alias, uart);
        g_free(cmdline);
        cmdline = resolved;
    }
    return cmdline;
}

static char *raspi_handoff_merge_firmware_bootargs(const void *fdt,
                                                   char *cmdline,
                                                   Error **errp)
{
    const char *firmware_bootargs;
    size_t firmware_length;
    int property_length;

    firmware_bootargs = fdt_getprop(fdt, fdt_path_offset(fdt, "/chosen"),
                                    "bootargs", &property_length);
    if (!firmware_bootargs) {
        return cmdline;
    }
    if (property_length <= 0) {
        error_setg(errp, "Raspberry Pi firmware DT bootargs is invalid");
        g_free(cmdline);
        return NULL;
    }
    firmware_length = strnlen(firmware_bootargs, property_length);
    if (firmware_length != property_length - 1) {
        error_setg(errp, "Raspberry Pi firmware DT bootargs is invalid");
        g_free(cmdline);
        return NULL;
    }
    if (firmware_length) {
        char *merged = g_strdup_printf("%s%s%s", firmware_bootargs,
                                       cmdline[0] ? " " : "", cmdline);

        g_free(cmdline);
        cmdline = merged;
    }
    return cmdline;
}

/*
 * The firmware hands the DTB over as an untrusted blob, so every libfdt
 * entry point has to be preceded by proof that the buffer really holds a
 * complete header and the whole declared tree.  fdt_check_header() reads a
 * full struct fdt_header, and the traversal helpers walk the structure
 * block, so a short or truncated blob must be rejected before either runs.
 */
static bool raspi_device_tree_valid(const uint8_t *device_tree,
                                    size_t device_tree_size)
{
    size_t totalsize;

    if (!device_tree || device_tree_size < sizeof(struct fdt_header) ||
        device_tree_size > RASPI_ARM64_DTB_MAX_SIZE) {
        return false;
    }
    if (fdt_check_header(device_tree)) {
        return false;
    }
    totalsize = fdt_totalsize(device_tree);
    if (totalsize > device_tree_size) {
        return false;
    }
    return fdt_check_full(device_tree, totalsize) == 0;
}

static RaspiHandoffResult raspi_prepare_device_tree(
    RaspiArmHandoff *handoff, const RaspiFirmwareConfig *config,
    const struct arm_boot_info *binfo,
    const uint8_t *device_tree, size_t device_tree_size,
    const uint8_t *cmdline, size_t cmdline_size, Error **errp)
{
    g_autofree char *bootargs = NULL;
    g_autofree char *serial_number = NULL;
    size_t capacity;
    void *fdt;
    uint64_t kaslr_seed;
    uint32_t acells;
    int bootloader_node;
    int usb_node;
    int result;

    if (!raspi_device_tree_valid(device_tree, device_tree_size)) {
        error_setg(errp, "Raspberry Pi DTB is invalid or too large");
        return RASPI_HANDOFF_ERROR_DEVICE_TREE;
    }
    QEMU_BUILD_BUG_ON(RASPI_ARM64_DTB_MAX_SIZE >
                      SIZE_MAX - RASPI_ARM64_DTB_SLACK);
    capacity = device_tree_size + RASPI_ARM64_DTB_SLACK;
    fdt = g_malloc0(capacity);
    result = fdt_open_into(device_tree, fdt, capacity);
    if (result < 0) {
        g_free(fdt);
        error_setg(errp, "could not expand Raspberry Pi arm64 DTB: %s",
                   fdt_strerror(result));
        return RASPI_HANDOFF_ERROR_DEVICE_TREE;
    }
    if (fdt_path_offset(fdt, "/chosen") < 0 &&
        qemu_fdt_add_subnode(fdt, "/chosen") < 0) {
        g_free(fdt);
        error_setg(errp, "could not create Raspberry Pi DT /chosen node");
        return RASPI_HANDOFF_ERROR_DEVICE_TREE;
    }
    if (fdt_path_offset(fdt, "/chosen/bootloader") < 0 &&
        qemu_fdt_add_subnode(fdt, "/chosen/bootloader") < 0) {
        g_free(fdt);
        error_setg(errp,
                   "could not create Raspberry Pi DT /chosen/bootloader");
        return RASPI_HANDOFF_ERROR_DEVICE_TREE;
    }
    bootloader_node = fdt_path_offset(fdt, "/chosen/bootloader");
    result = fdt_delprop(fdt, bootloader_node, "rsts");
    if (result < 0 && result != -FDT_ERR_NOTFOUND) {
        g_free(fdt);
        error_setg(errp,
                   "could not remove legacy Raspberry Pi DT rsts property");
        return RASPI_HANDOFF_ERROR_DEVICE_TREE;
    }
    if (qemu_fdt_setprop_cell(fdt, "/chosen/bootloader", "boot-mode",
                              config->boot_mode) < 0 ||
        qemu_fdt_setprop_cell(fdt, "/chosen/bootloader", "pm_rsts",
                              config->reset_status) < 0 ||
        qemu_fdt_setprop_cell(fdt, "/chosen/bootloader", "partition",
                              config->boot_partition) < 0 ||
        qemu_fdt_setprop_cell(fdt, "/chosen/bootloader", "tryboot",
                              config->tryboot) < 0 ||
        qemu_fdt_setprop_cell(fdt, "/chosen/bootloader", "signed",
                              config->bootloader_signed) < 0) {
        g_free(fdt);
        error_setg(errp, "could not set Raspberry Pi DT bootloader state");
        return RASPI_HANDOFF_ERROR_DEVICE_TREE;
    }
    usb_node = fdt_path_offset(fdt, "/chosen/bootloader/usb");
    if (!config->bootloader_usb_valid) {
        result = usb_node >= 0 ? fdt_del_node(fdt, usb_node) : 0;
    } else {
        if (usb_node < 0) {
            result = qemu_fdt_add_subnode(
                fdt, "/chosen/bootloader/usb");
            usb_node = fdt_path_offset(fdt, "/chosen/bootloader/usb");
            if (result >= 0 && usb_node < 0) {
                result = usb_node;
            }
        } else {
            result = 0;
        }
        if (result >= 0 && usb_node >= 0) {
            result = qemu_fdt_setprop_cell(
                fdt, "/chosen/bootloader/usb", "usb-version",
                config->bootloader_usb_version);
        }
        if (result >= 0) {
            result = qemu_fdt_setprop_cell(
                fdt, "/chosen/bootloader/usb", "route-string",
                config->bootloader_usb_route_string);
        }
        if (result >= 0) {
            result = qemu_fdt_setprop_cell(
                fdt, "/chosen/bootloader/usb",
                "root-hub-port-number",
                config->bootloader_usb_root_hub_port);
        }
        if (result >= 0) {
            result = qemu_fdt_setprop_cell(
                fdt, "/chosen/bootloader/usb", "lun",
                config->bootloader_usb_lun);
        }
    }
    if (result < 0) {
        g_free(fdt);
        error_setg(errp,
                   "could not set Raspberry Pi bootloader USB identity");
        return RASPI_HANDOFF_ERROR_DEVICE_TREE;
    }
    result = config->bootloader_build_timestamp_valid ?
        qemu_fdt_setprop_cell(fdt, "/chosen/bootloader",
                              "build_timestamp",
                              config->bootloader_build_timestamp) :
        fdt_delprop(fdt, bootloader_node, "build_timestamp");
    if (result < 0 && result != -FDT_ERR_NOTFOUND) {
        g_free(fdt);
        error_setg(errp,
                   "could not set Raspberry Pi bootloader build timestamp");
        return RASPI_HANDOFF_ERROR_DEVICE_TREE;
    }
    result = config->bootloader_update_timestamp_valid ?
        qemu_fdt_setprop_cell(fdt, "/chosen/bootloader",
                              "update_timestamp",
                              config->bootloader_update_timestamp) :
        fdt_delprop(fdt, bootloader_node, "update_timestamp");
    if (result < 0 && result != -FDT_ERR_NOTFOUND) {
        g_free(fdt);
        error_setg(errp,
                   "could not set Raspberry Pi bootloader update timestamp");
        return RASPI_HANDOFF_ERROR_DEVICE_TREE;
    }
    result = config->bootloader_capabilities_valid ?
        qemu_fdt_setprop_cell(fdt, "/chosen/bootloader", "capabilities",
                              config->bootloader_capabilities) :
        fdt_delprop(fdt, bootloader_node, "capabilities");
    if (result < 0 && result != -FDT_ERR_NOTFOUND) {
        g_free(fdt);
        error_setg(errp,
                   "could not set Raspberry Pi bootloader capabilities");
        return RASPI_HANDOFF_ERROR_DEVICE_TREE;
    }
    result = config->bootloader_version[0] ?
        qemu_fdt_setprop_string(fdt, "/chosen/bootloader", "version",
                                config->bootloader_version) :
        fdt_delprop(fdt, bootloader_node, "version");
    if (result < 0 && result != -FDT_ERR_NOTFOUND) {
        g_free(fdt);
        error_setg(errp, "could not set Raspberry Pi bootloader version");
        return RASPI_HANDOFF_ERROR_DEVICE_TREE;
    }
    if (fdt_path_offset(fdt, "/system") < 0 &&
        qemu_fdt_add_subnode(fdt, "/system") < 0) {
        g_free(fdt);
        error_setg(errp, "could not create Raspberry Pi DT /system node");
        return RASPI_HANDOFF_ERROR_DEVICE_TREE;
    }
    if (qemu_fdt_setprop_cell(fdt, "/system", "linux,revision",
                              config->board_revision) < 0 ||
        qemu_fdt_setprop_sized_cells(fdt, "/system", "linux,serial",
                                     2, config->serial) < 0) {
        g_free(fdt);
        error_setg(errp, "could not set Raspberry Pi DT system identity");
        return RASPI_HANDOFF_ERROR_DEVICE_TREE;
    }
    serial_number = g_strdup_printf("%016" PRIx64,
                                    (uint64_t)config->serial);
    if (qemu_fdt_setprop_string(fdt, "/", "serial-number",
                                serial_number) < 0) {
        g_free(fdt);
        error_setg(errp, "could not set Raspberry Pi DT serial number");
        return RASPI_HANDOFF_ERROR_DEVICE_TREE;
    }
    if (qemu_guest_getrandom(&kaslr_seed, sizeof(kaslr_seed), errp) < 0) {
        g_free(fdt);
        return RASPI_HANDOFF_ERROR_DEVICE_TREE;
    }
    if (qemu_fdt_setprop_u64(fdt, "/chosen", "kaslr-seed",
                             kaslr_seed) < 0) {
        g_free(fdt);
        error_setg(errp, "could not set Raspberry Pi DT KASLR seed");
        return RASPI_HANDOFF_ERROR_DEVICE_TREE;
    }
    if (qemu_fdt_setprop_cell(fdt, "/chosen", "rpi-min-boot-ver",
                              config->min_boot_version) < 0) {
        g_free(fdt);
        error_setg(errp,
                   "could not set Raspberry Pi minimum bootloader version");
        return RASPI_HANDOFF_ERROR_DEVICE_TREE;
    }
    if (qemu_fdt_setprop_cell(fdt, "/chosen", "rpi-sdram-size-gbit",
                              config->installed_mem_mb / 128) < 0) {
        g_free(fdt);
        error_setg(errp,
                   "could not set Raspberry Pi installed SDRAM size");
        return RASPI_HANDOFF_ERROR_DEVICE_TREE;
    }
    if (qemu_fdt_setprop_cell(fdt, "/chosen", "rpi-boardrev-ext",
                              config->board_revision_ext) < 0) {
        g_free(fdt);
        error_setg(errp,
                   "could not set Raspberry Pi extended board revision");
        return RASPI_HANDOFF_ERROR_DEVICE_TREE;
    }
    if (qemu_fdt_setprop_string(fdt, "/chosen", "os_prefix",
                                config->os_prefix) < 0 ||
        qemu_fdt_setprop_string(fdt, "/chosen", "overlay_prefix",
                                config->overlay_prefix) < 0) {
        g_free(fdt);
        error_setg(errp, "could not set Raspberry Pi firmware prefixes");
        return RASPI_HANDOFF_ERROR_DEVICE_TREE;
    }
    if (config->ethernet_mac_set) {
        const char *path = fdt_get_alias(fdt, "ethernet0");
        int ethernet = path ? fdt_path_offset(fdt, path) : -FDT_ERR_NOTFOUND;

        if (ethernet < 0) {
            ethernet = fdt_node_offset_by_compatible(
                fdt, -1, "brcm,bcm2711-genet-v5");
        }
        if (ethernet >= 0 &&
            fdt_setprop(fdt, ethernet, "local-mac-address",
                        config->ethernet_mac,
                        sizeof(config->ethernet_mac)) < 0) {
            g_free(fdt);
            error_setg(errp,
                       "could not set Raspberry Pi DT Ethernet MAC address");
            return RASPI_HANDOFF_ERROR_DEVICE_TREE;
        }
    }
    if (cmdline_size) {
        bootargs = raspi_handoff_cmdline(cmdline, cmdline_size, errp);
        if (bootargs) {
            bootargs = raspi_handoff_merge_firmware_bootargs(
                fdt, g_steal_pointer(&bootargs), errp);
        }
        if (bootargs) {
            bootargs = raspi_handoff_resolve_serial_aliases(
                fdt, g_steal_pointer(&bootargs));
        }
        if (!bootargs || qemu_fdt_setprop_string(
                fdt, "/chosen", "bootargs", bootargs) < 0) {
            g_free(fdt);
            return RASPI_HANDOFF_ERROR_DEVICE_TREE;
        }
    }
    acells = qemu_fdt_getprop_cell(fdt, "/", "#address-cells",
                                   NULL, errp);
    if (handoff->initramfs_size &&
        (qemu_fdt_setprop_sized_cells(
             fdt, "/chosen", "linux,initrd-start", acells,
             handoff->initramfs_address) < 0 ||
         qemu_fdt_setprop_sized_cells(
             fdt, "/chosen", "linux,initrd-end", acells,
             handoff->initramfs_address + handoff->initramfs_size) < 0)) {
        g_free(fdt);
        error_setg(errp, "could not set Raspberry Pi DT initramfs range");
        return RASPI_HANDOFF_ERROR_DEVICE_TREE;
    }
    if (!handoff->initramfs_size) {
        int chosen = fdt_path_offset(fdt, "/chosen");

        result = fdt_delprop(fdt, chosen, "linux,initrd-start");
        if (result >= 0 || result == -FDT_ERR_NOTFOUND) {
            result = fdt_delprop(fdt, chosen, "linux,initrd-end");
        }
        if (result < 0 && result != -FDT_ERR_NOTFOUND) {
            g_free(fdt);
            error_setg(errp,
                       "could not remove stale Raspberry Pi initramfs "
                       "range");
            return RASPI_HANDOFF_ERROR_DEVICE_TREE;
        }
    }
    if (binfo->modify_dtb) {
        binfo->modify_dtb(binfo, fdt);
    }
    if (handoff->bootloader_config_size &&
        !raspi_fdt_set_reserved_blob(
            fdt, "raspberrypi,bootloader-config",
            handoff->bootloader_config_address,
            handoff->bootloader_config_size, errp)) {
        g_free(fdt);
        return RASPI_HANDOFF_ERROR_DEVICE_TREE;
    }
    if (handoff->bootloader_public_key_size &&
        !raspi_fdt_set_reserved_blob(
            fdt, "raspberrypi,bootloader-public-key",
            handoff->bootloader_public_key_address,
            handoff->bootloader_public_key_size, errp)) {
        g_free(fdt);
        return RASPI_HANDOFF_ERROR_DEVICE_TREE;
    }
    if (!raspi_fdt_replace_memory(fdt, config, binfo, errp)) {
        g_free(fdt);
        return RASPI_HANDOFF_ERROR_DEVICE_TREE;
    }
    result = fdt_pack(fdt);
    if (result < 0 || fdt_totalsize(fdt) > RASPI_ARM64_DTB_MAX_SIZE) {
        g_free(fdt);
        error_setg(errp, "could not finalize Raspberry Pi DTB");
        return RASPI_HANDOFF_ERROR_DEVICE_TREE;
    }
    handoff->device_tree_size = fdt_totalsize(fdt);
    handoff->device_tree = g_realloc(fdt, handoff->device_tree_size);
    return RASPI_HANDOFF_READY;
}

static bool raspi_parse_initramfs_address(const char *value, hwaddr *address)
{
    const char *end;
    uint64_t parsed;

    if (!g_ascii_strcasecmp(value, "followkernel") || !strcmp(value, "0")) {
        return false;
    }
    if (qemu_strtou64(value, &end, 0, &parsed) < 0 || *end) {
        return false;
    }
    *address = parsed;
    return true;
}

RaspiHandoffResult raspi_arm_prepare_handoff(
    RaspiArmHandoff *handoff, RaspiFirmwareConfig *config,
    const struct arm_boot_info *binfo,
    const uint8_t *kernel, size_t kernel_size,
    const uint8_t *device_tree, size_t device_tree_size,
    const uint8_t *cmdline, size_t cmdline_size,
    const uint8_t *initramfs, size_t initramfs_size, Error **errp)
{
    RaspiHandoffResult result;
    hwaddr device_tree_limit;
    hwaddr firmware_data_limit;
    hwaddr kernel_end;
    uint64_t installed;
    uint64_t lower;
    uint64_t reserved;
    uint64_t total;
    bool expose_bootloader_config;
    bool expose_bootloader_public_key;

    memset(handoff, 0, sizeof(*handoff));
    expose_bootloader_config =
        raspi_device_tree_valid(device_tree, device_tree_size) &&
        fdt_node_offset_by_compatible(
            device_tree, -1,
            "raspberrypi,bootloader-config") >= 0;
    expose_bootloader_public_key =
        raspi_device_tree_valid(device_tree, device_tree_size) &&
        fdt_node_offset_by_compatible(
            device_tree, -1,
            "raspberrypi,bootloader-public-key") >= 0;
    installed = config->installed_mem_mb ?
                (uint64_t)config->installed_mem_mb * MiB :
                binfo->ram_size;
    total = config->total_mem_set ?
            (uint64_t)config->total_mem_mb * MiB : installed;
    reserved = config->gpu_mem_effective_mb ?
               (uint64_t)config->gpu_mem_effective_mb * MiB :
               1 * GiB - binfo->ram_size;
    lower = MIN(total, 1 * GiB);
    if (lower <= reserved) {
        error_setg(errp, "Raspberry Pi firmware data has no ARM memory");
        return RASPI_HANDOFF_ERROR_LAYOUT;
    }
    firmware_data_limit = lower - reserved;
    if (config->bootloader_config_size && expose_bootloader_config) {
        if (!config->bootloader_config ||
            config->bootloader_config_size > firmware_data_limit) {
            error_setg(errp,
                       "Raspberry Pi bootloader config data is invalid");
            return RASPI_HANDOFF_ERROR_LAYOUT;
        }
        handoff->bootloader_config_size = config->bootloader_config_size;
        handoff->bootloader_config = g_memdup2(
            config->bootloader_config, config->bootloader_config_size);
        handoff->bootloader_config_address = QEMU_ALIGN_DOWN(
            firmware_data_limit - handoff->bootloader_config_size, 64);
        firmware_data_limit = handoff->bootloader_config_address;
    }
    if (config->bootloader_public_key_size &&
        expose_bootloader_public_key) {
        if (!config->bootloader_public_key ||
            config->bootloader_public_key_size > firmware_data_limit) {
            error_setg(errp,
                       "Raspberry Pi bootloader public key data is invalid");
            result = RASPI_HANDOFF_ERROR_LAYOUT;
            goto fail;
        }
        handoff->bootloader_public_key_size =
            config->bootloader_public_key_size;
        handoff->bootloader_public_key = g_memdup2(
            config->bootloader_public_key,
            config->bootloader_public_key_size);
        handoff->bootloader_public_key_address = QEMU_ALIGN_DOWN(
            firmware_data_limit - handoff->bootloader_public_key_size, 64);
        firmware_data_limit = handoff->bootloader_public_key_address;
    }
    result = raspi_prepare_kernel(
        handoff, config->arm_64bit, kernel, kernel_size, errp);
    if (result != RASPI_HANDOFF_READY) {
        goto fail;
    }
    kernel_end = handoff->kernel_address + handoff->kernel_reserve_size;
    if (raspi_handoff_overlaps_bootloader_data(
            handoff, handoff->kernel_address,
            handoff->kernel_reserve_size)) {
        error_setg(errp,
                   "Raspberry Pi kernel overlaps bootloader data");
        result = RASPI_HANDOFF_ERROR_LAYOUT;
        goto fail;
    }

    if (initramfs_size) {
        handoff->initramfs_size = initramfs_size;
        if (config->initramfs_auto) {
            hwaddr initramfs_limit = MIN(
                (uint64_t)RASPI_ARM64_LOAD_LIMIT,
                (uint64_t)firmware_data_limit);

            if (initramfs_size > initramfs_limit) {
                error_setg(errp,
                           "Raspberry Pi initramfs exceeds ARM memory");
                result = RASPI_HANDOFF_ERROR_LAYOUT;
                goto fail;
            }
            handoff->initramfs_address = QEMU_ALIGN_DOWN(
                initramfs_limit - initramfs_size, 4 * KiB);
        } else if (!raspi_parse_initramfs_address(
                       config->initramfs_address,
                       &handoff->initramfs_address)) {
            if (g_ascii_strcasecmp(config->initramfs_address,
                                   "followkernel") &&
                strcmp(config->initramfs_address, "0")) {
                error_setg(errp, "Raspberry Pi initramfs address is invalid");
                result = RASPI_HANDOFF_ERROR_LAYOUT;
                goto fail;
            }
            handoff->initramfs_address = QEMU_ALIGN_UP(kernel_end, 4 * KiB);
        }
        if (!raspi_handoff_range_valid(handoff->initramfs_address,
                                       initramfs_size,
                                       RASPI_ARM64_LOAD_LIMIT) ||
            raspi_handoff_overlaps_firmware_state(
                handoff, handoff->initramfs_address, initramfs_size) ||
            raspi_handoff_overlaps_bootloader_data(
                handoff, handoff->initramfs_address, initramfs_size) ||
            raspi_handoff_ranges_overlap(
                handoff->kernel_address, handoff->kernel_reserve_size,
                handoff->initramfs_address, initramfs_size)) {
            error_setg(errp, "Raspberry Pi initramfs layout overlaps RAM");
            result = RASPI_HANDOFF_ERROR_LAYOUT;
            goto fail;
        }
    }

    result = raspi_prepare_device_tree(
        handoff, config, binfo, device_tree, device_tree_size,
        cmdline, cmdline_size, errp);
    if (result != RASPI_HANDOFF_READY) {
        goto fail;
    }
    device_tree_limit = config->device_tree_end_set ?
                        config->device_tree_end : RASPI_ARM64_LOAD_LIMIT;
    device_tree_limit = MIN(device_tree_limit, firmware_data_limit);
    if (config->device_tree_address_set) {
        handoff->device_tree_address = config->device_tree_address;
    } else {
        handoff->device_tree_address =
            device_tree_limit < handoff->device_tree_size ? 0 :
            QEMU_ALIGN_DOWN(
                device_tree_limit - handoff->device_tree_size, 0x100);
        if (handoff->initramfs_size &&
            (config->initramfs_auto ||
             raspi_handoff_ranges_overlap(
                 handoff->device_tree_address, handoff->device_tree_size,
                 handoff->initramfs_address, handoff->initramfs_size))) {
            device_tree_limit = MIN(
                device_tree_limit, handoff->initramfs_address);
            handoff->device_tree_address =
                device_tree_limit < handoff->device_tree_size ? 0 :
                QEMU_ALIGN_DOWN(
                    device_tree_limit - handoff->device_tree_size, 0x100);
        }
    }
    if (!raspi_handoff_range_valid(handoff->device_tree_address,
                                   handoff->device_tree_size,
                                   RASPI_ARM64_LOAD_LIMIT) ||
        (config->device_tree_end_set &&
         !raspi_handoff_range_valid(
             handoff->device_tree_address, handoff->device_tree_size,
             config->device_tree_end)) ||
        raspi_handoff_overlaps_firmware_state(
            handoff, handoff->device_tree_address,
            handoff->device_tree_size) ||
        raspi_handoff_overlaps_bootloader_data(
            handoff, handoff->device_tree_address,
            handoff->device_tree_size) ||
        raspi_handoff_ranges_overlap(
            handoff->kernel_address, handoff->kernel_reserve_size,
            handoff->device_tree_address, handoff->device_tree_size) ||
        (handoff->initramfs_size &&
         raspi_handoff_ranges_overlap(
             handoff->device_tree_address, handoff->device_tree_size,
             handoff->initramfs_address, handoff->initramfs_size))) {
        error_setg(errp, "Raspberry Pi DTB layout overlaps another artifact");
        result = RASPI_HANDOFF_ERROR_LAYOUT;
        goto fail;
    }
    return RASPI_HANDOFF_READY;

fail:
    raspi_arm_handoff_clear(handoff);
    return result;
}

static bool raspi_handoff_write(AddressSpace *as, hwaddr address,
                                const void *data, size_t size,
                                const char *name, Error **errp)
{
    if (address_space_write(as, address, MEMTXATTRS_UNSPECIFIED,
                            data, size) != MEMTX_OK) {
        error_setg(errp, "could not load Raspberry Pi %s at 0x%" HWADDR_PRIx,
                   name, address);
        return false;
    }
    return true;
}

bool raspi_arm_install_handoff(const RaspiArmHandoff *handoff,
                               const uint8_t *initramfs,
                               AddressSpace *as, Error **errp)
{
    static const uint32_t spin_code[] = {
        0xd2801b05, /* mov x5, #0xd8 */
        0xd53800a6, /* mrs x6, mpidr_el1 */
        0x924004c6, /* and x6, x6, #3 */
        0xd503205f, /* wfe */
        0xf86678a4, /* ldr x4, [x5, x6, lsl #3] */
        0xb4ffffc4, /* cbz x4, spin */
        0xd2800000, /* mov x0, #0 */
        0xd2800001, /* mov x1, #0 */
        0xd2800002, /* mov x2, #0 */
        0xd2800003, /* mov x3, #0 */
        0xd61f0080, /* br x4 */
    };
    static const uint32_t arm32_entry_code[] = {
        0xe1a02000, /* mov r2, r0 */
        0xe3a00000, /* mov r0, #0 */
        0xe3e01000, /* mvn r1, #0 */
        0xe51ff004, /* ldr pc, [pc, #-4] */
        RASPI_ARM32_KERNEL_ADDRESS,
    };
    static const uint32_t arm32_spin_code[] = {
        0xee100fb0, /* mrc p15, 0, r0, c0, c0, 5 */
        0xe7e10050, /* ubfx r0, r0, #0, #2 */
        0xe59f5014, /* ldr r5, =0xff8000cc */
        0xe320f001, /* yield */
        0xe7953200, /* ldr r3, [r5, r0, lsl #4] */
        0xe3530000, /* cmp r3, #0 */
        0x0afffffb, /* beq spin */
        0xe7853200, /* str r3, [r5, r0, lsl #4] */
        0xe12fff13, /* bx r3 */
        RASPI_ARM32_MAILBOX3_CLEAR_BASE,
    };
    uint8_t spin_blob[sizeof(spin_code)];
    uint8_t entry_blob[sizeof(arm32_entry_code)];
    uint8_t spin_table[4 * sizeof(uint64_t)] = { 0 };

    if (handoff->arm_64bit) {
        for (unsigned int i = 0; i < ARRAY_SIZE(spin_code); i++) {
            stl_le_p(spin_blob + i * sizeof(uint32_t), spin_code[i]);
        }
    } else {
        for (unsigned int i = 0; i < ARRAY_SIZE(arm32_entry_code); i++) {
            stl_le_p(entry_blob + i * sizeof(uint32_t),
                     arm32_entry_code[i]);
        }
        for (unsigned int i = 0; i < ARRAY_SIZE(arm32_spin_code); i++) {
            stl_le_p(spin_blob + i * sizeof(uint32_t),
                     arm32_spin_code[i]);
        }
    }
    if (!raspi_handoff_write(
               as, handoff->kernel_address, handoff->kernel,
               handoff->kernel_size, "kernel", errp) ||
        (handoff->bootloader_config_size &&
         !raspi_handoff_write(
             as, handoff->bootloader_config_address,
             handoff->bootloader_config,
             handoff->bootloader_config_size,
             "bootloader config", errp)) ||
        (handoff->bootloader_public_key_size &&
         !raspi_handoff_write(
             as, handoff->bootloader_public_key_address,
             handoff->bootloader_public_key,
             handoff->bootloader_public_key_size,
             "bootloader public key", errp)) ||
        (handoff->initramfs_size && !raspi_handoff_write(
               as, handoff->initramfs_address, initramfs,
               handoff->initramfs_size, "initramfs", errp)) ||
        !raspi_handoff_write(
               as, handoff->device_tree_address, handoff->device_tree,
               handoff->device_tree_size, "device tree", errp)) {
        return false;
    }
    if (handoff->arm_64bit) {
        return raspi_handoff_write(
                   as, RASPI_ARM64_SPIN_CODE_ADDRESS, spin_blob,
                   sizeof(spin_code), "secondary-core stub", errp) &&
               raspi_handoff_write(
               as, RASPI_ARM64_SPIN_TABLE_ADDRESS, spin_table,
               sizeof(spin_table), "secondary-core table", errp);
    }
    return raspi_handoff_write(
               as, RASPI_ARM32_ENTRY_ADDRESS, entry_blob,
               sizeof(entry_blob), "primary-core stub", errp) &&
           raspi_handoff_write(
               as, RASPI_ARM32_SPIN_CODE_ADDRESS, spin_blob,
               sizeof(arm32_spin_code), "secondary-core stub", errp) &&
           raspi_handoff_write(
               as, RASPI_ARM64_SPIN_TABLE_ADDRESS, spin_table,
               sizeof(spin_table), "secondary-core table", errp);
}

void raspi_arm_handoff_clear(RaspiArmHandoff *handoff)
{
    g_free(handoff->kernel);
    g_free(handoff->device_tree);
    g_free(handoff->bootloader_config);
    g_free(handoff->bootloader_public_key);
    memset(handoff, 0, sizeof(*handoff));
}
