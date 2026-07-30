/*
 * Raspberry Pi behavioral firmware-to-ARM handoff
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_RASPI_HANDOFF_H
#define HW_ARM_RASPI_HANDOFF_H

#include "exec/hwaddr.h"
#include "hw/arm/boot.h"
#include "hw/arm/raspi_firmware.h"

typedef enum RaspiHandoffResult {
    RASPI_HANDOFF_ERROR_KERNEL = -3,
    RASPI_HANDOFF_ERROR_DEVICE_TREE = -2,
    RASPI_HANDOFF_ERROR_LAYOUT = -1,
    RASPI_HANDOFF_READY = 0,
} RaspiHandoffResult;

typedef struct RaspiArmHandoff {
    bool arm_64bit;
    bool big_endian;
    uint8_t *kernel;
    size_t kernel_size;
    hwaddr kernel_address;
    hwaddr entry_address;
    uint64_t kernel_reserve_size;
    uint8_t *device_tree;
    size_t device_tree_size;
    hwaddr device_tree_address;
    hwaddr initramfs_address;
    size_t initramfs_size;
    hwaddr bootloader_config_address;
    uint8_t *bootloader_config;
    size_t bootloader_config_size;
    hwaddr bootloader_public_key_address;
    uint8_t *bootloader_public_key;
    size_t bootloader_public_key_size;
} RaspiArmHandoff;

RaspiHandoffResult raspi_arm_prepare_handoff(
    RaspiArmHandoff *handoff, RaspiFirmwareConfig *config,
    const struct arm_boot_info *binfo,
    const uint8_t *kernel, size_t kernel_size,
    const uint8_t *device_tree, size_t device_tree_size,
    const uint8_t *cmdline, size_t cmdline_size,
    const uint8_t *initramfs, size_t initramfs_size, Error **errp);
bool raspi_arm_install_handoff(const RaspiArmHandoff *handoff,
                              const uint8_t *initramfs,
                              AddressSpace *as, Error **errp);
void raspi_arm_handoff_clear(RaspiArmHandoff *handoff);

#endif
