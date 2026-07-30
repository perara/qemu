/*
 * Raspberry Pi behavioral Device Tree overlay processing
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_RASPI_OVERLAY_H
#define HW_ARM_RASPI_OVERLAY_H

#include "hw/arm/raspi_firmware.h"

typedef enum RaspiOverlayResult {
    RASPI_OVERLAY_ERROR_APPLY = -5,
    RASPI_OVERLAY_ERROR_PARAMETER = -4,
    RASPI_OVERLAY_ERROR_INVALID = -3,
    RASPI_OVERLAY_ERROR_MISSING = -2,
    RASPI_OVERLAY_ERROR_LIMIT = -1,
    RASPI_OVERLAY_READY = 0,
} RaspiOverlayResult;

typedef struct RaspiOverlayState {
    uint32_t overlays_applied;
    uint32_t parameters_applied;
    /* Remaining cumulative property-rewrite budget for this configuration. */
    size_t mutation_budget;
    bool hat_present;
    uint64_t hat_eeprom_size;
    uint32_t hat_eeprom_declared_size;
    char *hat_eeprom_sha256;
    char *hat_content_sha256;
    char *hat_vendor;
    char *hat_product;
    char *hat_uuid;
    uint16_t hat_product_id;
    uint16_t hat_product_version;
    uint32_t hat_custom_count;
    char *hat_overlay;
    bool hat_gpio_valid;
    uint8_t hat_gpio_map[30];
    char *last_overlay_file;
    char *final_dtb_sha256;
} RaspiOverlayState;

RaspiOverlayResult raspi_overlay_apply_config(
    RaspiFatVolume *volume, const RaspiFirmwareConfig *config,
    const uint8_t *hat_eeprom, size_t hat_eeprom_size,
    const uint8_t *device_tree, size_t device_tree_size,
    uint8_t **result_tree, size_t *result_size,
    RaspiOverlayState *state, Error **errp);
RaspiOverlayResult raspi_overlay_apply_config_with_reader(
    const RaspiFirmwareConfig *config,
    const uint8_t *hat_eeprom, size_t hat_eeprom_size,
    const uint8_t *device_tree,
    size_t device_tree_size, RaspiFirmwareReadPath *read_path,
    void *read_opaque, uint8_t **result_tree, size_t *result_size,
    RaspiOverlayState *state, Error **errp);
void raspi_overlay_state_clear(RaspiOverlayState *state);

#endif
