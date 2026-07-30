/*
 * Raspberry Pi signed-boot verification helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_RASPI_SECURE_H
#define HW_ARM_RASPI_SECURE_H

#include "qapi/error.h"

#define RASPI_SECURE_PUBLIC_KEY_SIZE 264
#define RASPI_SECURE_KEY_HASH_SIZE 32

/*
 * Public signing key embedded by the BCM2711 bootloader for the official
 * fw-download-alias1.raspberrypi.com network-install service.
 */
extern const uint8_t
raspi_secure_default_network_key[RASPI_SECURE_PUBLIC_KEY_SIZE];

typedef enum RaspiSecureResult {
    RASPI_SECURE_OK,
    RASPI_SECURE_KEY_FORMAT,
    RASPI_SECURE_KEY_HASH_MISMATCH,
    RASPI_SECURE_SIGNATURE_FORMAT,
    RASPI_SECURE_IMAGE_HASH_MISMATCH,
    RASPI_SECURE_RSA_MISMATCH,
    RASPI_SECURE_CRYPTO_UNAVAILABLE,
    RASPI_SECURE_BOOTSYS_MISSING,
    RASPI_SECURE_BOOTSYS_TRUST_MISSING,
    RASPI_SECURE_BOOTSYS_FORMAT,
    RASPI_SECURE_BOOTSYS_HASH_MISMATCH,
    RASPI_SECURE_BOOTSYS_DEVKEY_REVOKED,
    RASPI_SECURE_DEPENDENCY_MISSING,
    RASPI_SECURE_DEPENDENCY_FORMAT,
    RASPI_SECURE_DEPENDENCY_HASH_MISMATCH,
} RaspiSecureResult;

RaspiSecureResult raspi_secure_check_public_key(
    const uint8_t public_key[RASPI_SECURE_PUBLIC_KEY_SIZE],
    const uint8_t expected_hash[RASPI_SECURE_KEY_HASH_SIZE], Error **errp);
RaspiSecureResult raspi_secure_verify(
    const uint8_t public_key[RASPI_SECURE_PUBLIC_KEY_SIZE],
    const uint8_t *image, size_t image_size,
    const uint8_t *signature_file, size_t signature_file_size,
    Error **errp);
const char *raspi_secure_result_name(RaspiSecureResult result);

#endif
