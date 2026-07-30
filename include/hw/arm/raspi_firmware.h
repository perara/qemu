/*
 * Raspberry Pi behavioral firmware configuration
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_RASPI_FIRMWARE_H
#define HW_ARM_RASPI_FIRMWARE_H

#include "hw/arm/raspi_boot.h"

#define RASPI_FIRMWARE_MAX_INITRAMFS_FILES 8
#define RASPI_FIRMWARE_MAX_EDIDS 2
#define RASPI_FIRMWARE_EDID_NAME_MAX 32
#define RASPI_FIRMWARE_BOOTLOADER_VERSION_MAX 40
#define RASPI_FIRMWARE_PI4_SDRAM_FREQ_MHZ 3200
#define RASPI_FIRMWARE_PI4_GPU_MEM_DEFAULT_MB 76

typedef struct RaspiFirmwareConfig {
    bool cm4;
    bool config_present;
    bool arm_64bit;
    bool auto_initramfs;
    bool initramfs_auto;
    bool device_tree_enabled;
    bool device_tree_address_set;
    bool device_tree_end_set;
    bool force_eeprom_read;
    bool kernel_explicit;
    bool start_file_explicit;
    bool fixup_file_explicit;
    bool start_x;
    bool start_debug;
    bool gpu_mem_set;
    bool gpu_mem_256_set;
    bool gpu_mem_512_set;
    bool gpu_mem_1024_set;
    bool total_mem_set;
    bool bootcode_delay_set;
    bool sdram_freq_set;
    bool uart_2ndstage;
    uint32_t bootvar0;
    uint32_t boot_mode;
    bool bootloader_build_timestamp_valid;
    uint32_t bootloader_build_timestamp;
    bool bootloader_update_timestamp_valid;
    uint32_t bootloader_update_timestamp;
    bool bootloader_capabilities_valid;
    uint32_t bootloader_capabilities;
    bool bootloader_usb_valid;
    uint32_t bootloader_usb_version;
    uint32_t bootloader_usb_route_string;
    uint32_t bootloader_usb_root_hub_port;
    uint32_t bootloader_usb_lun;
    char bootloader_version[RASPI_FIRMWARE_BOOTLOADER_VERSION_MAX + 1];
    uint32_t bootloader_signed;
    uint32_t partition;
    uint32_t boot_partition;
    uint32_t reset_status;
    uint32_t board_revision;
    uint32_t board_revision_ext;
    uint32_t min_boot_version;
    uint32_t serial;
    uint32_t customer_otp[8];
    uint64_t gpio_known_mask;
    uint64_t gpio_level_mask;
    uint8_t board_type;
    bool ethernet_mac_set;
    uint8_t ethernet_mac[6];
    uint32_t gpu_mem;
    uint32_t gpu_mem_256;
    uint32_t gpu_mem_512;
    uint32_t gpu_mem_1024;
    uint32_t gpu_mem_effective_mb;
    uint32_t installed_mem_mb;
    uint32_t total_mem_mb;
    uint32_t bootcode_delay;
    uint32_t sdram_freq_mhz;
    uint64_t device_tree_address;
    uint64_t device_tree_end;
    char edid_names[RASPI_FIRMWARE_MAX_EDIDS][RASPI_FIRMWARE_EDID_NAME_MAX];
    uint8_t edid_valid_mask;
    bool tryboot;
    const uint8_t *bootloader_config_append;
    uint32_t bootloader_config_append_size;
    const uint8_t *bootloader_config;
    uint32_t bootloader_config_size;
    const uint8_t *bootloader_public_key;
    uint32_t bootloader_public_key_size;
    char *start_file;
    char *fixup_file;
    char *kernel_file;
    char *device_tree_file;
    char *cmdline_file;
    char *initramfs_file;
    char *initramfs_address;
    char *os_prefix;
    char *overlay_prefix;
    GPtrArray *overlays;
    GPtrArray *dtparams;
    GPtrArray *dt_commands;
    GPtrArray *include_files;
    uint32_t parsed_lines;
    uint32_t ignored_properties;
    uint32_t include_count;
} RaspiFirmwareConfig;

typedef struct RaspiFirmwareManifest {
    RaspiFatFile start;
    RaspiFatFile fixup;
    RaspiFatFile kernel;
    RaspiFatFile device_tree;
    RaspiFatFile cmdline;
    RaspiFatFile initramfs[RASPI_FIRMWARE_MAX_INITRAMFS_FILES];
    bool cmdline_present;
    uint8_t initramfs_count;
} RaspiFirmwareManifest;

typedef struct RaspiFirmwarePaths {
    char *start;
    char *fixup;
    char *kernel;
    char *device_tree;
    char *cmdline;
    char *initramfs[RASPI_FIRMWARE_MAX_INITRAMFS_FILES];
    uint8_t initramfs_count;
} RaspiFirmwarePaths;

typedef enum RaspiFirmwareResolveResult {
    RASPI_FIRMWARE_RESOLVE_ERROR = -1,
    RASPI_FIRMWARE_MISSING_START,
    RASPI_FIRMWARE_MISSING_FIXUP,
    RASPI_FIRMWARE_MISSING_KERNEL,
    RASPI_FIRMWARE_MISSING_DEVICE_TREE,
    RASPI_FIRMWARE_MISSING_INITRAMFS,
    RASPI_FIRMWARE_READY,
} RaspiFirmwareResolveResult;

typedef uint8_t *RaspiFirmwareReadPath(
    void *opaque, const char *path, size_t maximum, size_t *length,
    RaspiFatResult *result, Error **errp);

void raspi_firmware_config_init(RaspiFirmwareConfig *config, bool cm4);
void raspi_firmware_config_clear(RaspiFirmwareConfig *config);
bool raspi_firmware_config_load(RaspiFatVolume *volume,
                                RaspiFirmwareConfig *config, Error **errp);
bool raspi_firmware_config_load_named(RaspiFatVolume *volume,
                                      RaspiFirmwareConfig *config,
                                      const char *config_path,
                                      Error **errp);
bool raspi_firmware_config_load_with_reader(
    RaspiFirmwareConfig *config, RaspiFirmwareReadPath *read_path,
    void *opaque, Error **errp);
bool raspi_firmware_config_load_with_reader_named(
    RaspiFirmwareConfig *config, RaspiFirmwareReadPath *read_path,
    void *opaque, const char *config_path, Error **errp);
bool raspi_firmware_paths_resolve(RaspiFirmwareConfig *config,
                                  RaspiFirmwarePaths *paths,
                                  Error **errp);
void raspi_firmware_paths_clear(RaspiFirmwarePaths *paths);
RaspiFirmwareResolveResult raspi_firmware_resolve(
    RaspiFatVolume *volume, RaspiFirmwareConfig *config,
    RaspiFirmwareManifest *manifest, Error **errp);

#endif
