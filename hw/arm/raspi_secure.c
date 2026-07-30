/*
 * Raspberry Pi signed-boot verification helpers
 *
 * Raspberry Pi pubkey.bin stores a 2048-bit RSA modulus followed by an
 * eight-byte exponent, both little-endian.  The matching .sig file contains
 * the SHA-256 digest, a timestamp, and a hexadecimal RSA PKCS#1 v1.5
 * signature.  These are the formats emitted by the upstream Raspberry Pi
 * rpi-eeprom tools.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "crypto/akcipher.h"
#include "crypto/der.h"
#include "crypto/hash.h"
#include "hw/arm/raspi_secure.h"
#ifdef CONFIG_GNUTLS
#include <gnutls/abstract.h>
#endif

#define RASPI_SECURE_RSA_BYTES 256

/*
 * raspberrypi/rpi-eeprom imager/net_install_pubkey.pem, converted to the
 * bootloader's 256-byte little-endian modulus plus 64-bit little-endian
 * exponent representation.  SHA-256 of these 264 bytes:
 * a57cf76a95819bae2e85ec3594347268bd0a045955c1292f56532b193e1bca9a
 */
const uint8_t
raspi_secure_default_network_key[RASPI_SECURE_PUBLIC_KEY_SIZE] = {
    0xcf, 0xc5, 0x14, 0xdf, 0x0e, 0xd3, 0xc4, 0x9c, 0xe7, 0x94, 0xf6, 0x1b,
    0x3d, 0x47, 0x5d, 0xc8, 0x17, 0xa9, 0x09, 0x8c, 0x79, 0x79, 0x3a, 0xb1,
    0x68, 0xff, 0xdd, 0xf1, 0x9a, 0xc1, 0x1c, 0x2f, 0x91, 0x97, 0x86, 0x09,
    0x2f, 0x08, 0x14, 0x40, 0xd2, 0xb9, 0x13, 0x86, 0x3a, 0x46, 0xdc, 0x72,
    0x16, 0xaa, 0xdc, 0xa1, 0x43, 0xbb, 0x95, 0xe7, 0x00, 0x76, 0x81, 0x96,
    0x5f, 0x1d, 0xe4, 0x2a, 0x8f, 0x67, 0xcc, 0x15, 0x38, 0x0c, 0x3d, 0x83,
    0xa9, 0x11, 0xb7, 0x69, 0x57, 0x63, 0x09, 0x6c, 0x57, 0xbd, 0xcc, 0xf4,
    0x0f, 0x23, 0xec, 0x6f, 0x71, 0xa0, 0x6a, 0x01, 0x02, 0xf8, 0x56, 0xe3,
    0x9c, 0x35, 0xb6, 0x18, 0xc4, 0xbb, 0x4c, 0xe4, 0xb9, 0x02, 0x92, 0x6f,
    0x51, 0xec, 0xba, 0x06, 0xc3, 0x69, 0x07, 0x6b, 0xf7, 0x47, 0x85, 0x3b,
    0x6d, 0x30, 0x26, 0xc9, 0x23, 0xae, 0xd8, 0x4e, 0xb8, 0xe6, 0xe3, 0xd3,
    0x0f, 0x38, 0x59, 0xef, 0x35, 0x61, 0xcb, 0x14, 0x1e, 0x05, 0xc9, 0xfd,
    0xe8, 0xe5, 0xd9, 0xd9, 0xe8, 0xd2, 0x33, 0x86, 0xe9, 0x7a, 0xfe, 0xf2,
    0x81, 0x83, 0x83, 0x05, 0x8d, 0xfa, 0x7c, 0x0a, 0x47, 0xf8, 0x87, 0xd9,
    0x3d, 0x05, 0x28, 0xa4, 0x37, 0xfa, 0x13, 0x16, 0x7b, 0x52, 0x7a, 0xf3,
    0xf6, 0xbb, 0xee, 0x41, 0x74, 0xe8, 0x02, 0xb6, 0x0c, 0x30, 0x41, 0xfb,
    0xb6, 0x65, 0x00, 0xaa, 0x54, 0xc3, 0x47, 0x48, 0x02, 0xfb, 0x4b, 0x6e,
    0xb6, 0x8a, 0xec, 0x80, 0xc1, 0x0b, 0x0b, 0x39, 0x82, 0x35, 0xcb, 0xe7,
    0x50, 0xb1, 0x9f, 0xcd, 0x6b, 0xa3, 0xaa, 0x76, 0x2c, 0x77, 0x41, 0xf6,
    0xf0, 0xb6, 0x74, 0x2f, 0xfc, 0xea, 0x2c, 0x20, 0x31, 0x9f, 0x2c, 0xee,
    0x30, 0xb2, 0xcf, 0x7f, 0xf0, 0x64, 0x69, 0xb2, 0xfd, 0x69, 0x81, 0x11,
    0xe9, 0x5f, 0x8f, 0x9e, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static int raspi_secure_hex_nibble(uint8_t value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    value = g_ascii_tolower(value);
    return value >= 'a' && value <= 'f' ? value - 'a' + 10 : -1;
}

static bool raspi_secure_parse_hex(const char *text, size_t text_size,
                                   uint8_t *output, size_t output_size)
{
    if (text_size != output_size * 2) {
        return false;
    }
    for (size_t i = 0; i < output_size; i++) {
        int high = raspi_secure_hex_nibble(text[i * 2]);
        int low = raspi_secure_hex_nibble(text[i * 2 + 1]);

        if (high < 0 || low < 0) {
            return false;
        }
        output[i] = high << 4 | low;
    }
    return true;
}

static uint8_t *raspi_secure_public_key_der(
    const uint8_t public_key[RASPI_SECURE_PUBLIC_KEY_SIZE], size_t *der_size,
    Error **errp)
{
    uint8_t modulus[RASPI_SECURE_RSA_BYTES + 1] = { 0 };
    uint8_t exponent[9] = { 0 };
    const uint8_t *exponent_value;
    uint64_t exponent_le = ldq_le_p(public_key + RASPI_SECURE_RSA_BYTES);
    size_t exponent_size = 0;
    QCryptoEncodeContext *ctx;
    uint8_t *der;

    /* A 2048-bit modulus must have its most significant bit set. */
    if (!(public_key[RASPI_SECURE_RSA_BYTES - 1] & 0x80) ||
        !(public_key[0] & 1) || exponent_le < 3 || !(exponent_le & 1)) {
        error_setg(errp, "Raspberry Pi RSA public key is invalid");
        return NULL;
    }
    for (size_t i = 0; i < RASPI_SECURE_RSA_BYTES; i++) {
        modulus[i + 1] = public_key[RASPI_SECURE_RSA_BYTES - 1 - i];
    }
    do {
        exponent[ARRAY_SIZE(exponent) - 1 - exponent_size++] = exponent_le;
        exponent_le >>= 8;
    } while (exponent_le);
    exponent_value = exponent + ARRAY_SIZE(exponent) - exponent_size;
    if (exponent_value[0] & 0x80) {
        exponent_value--;
        exponent_size++;
    }

    ctx = qcrypto_der_encode_ctx_new();
    qcrypto_der_encode_seq_begin(ctx);
    qcrypto_der_encode_int(ctx, modulus, sizeof(modulus));
    qcrypto_der_encode_int(ctx, exponent_value, exponent_size);
    qcrypto_der_encode_seq_end(ctx);
    *der_size = qcrypto_der_encode_ctx_buffer_len(ctx);
    der = g_malloc(*der_size);
    qcrypto_der_encode_ctx_flush_and_free(ctx, der);
    return der;
}

#ifdef CONFIG_GNUTLS
static bool raspi_secure_verify_gnutls(
    const uint8_t public_key[RASPI_SECURE_PUBLIC_KEY_SIZE],
    const uint8_t signature[RASPI_SECURE_RSA_BYTES],
    const uint8_t digest[RASPI_SECURE_KEY_HASH_SIZE])
{
    uint8_t modulus[RASPI_SECURE_RSA_BYTES];
    uint8_t exponent[8];
    uint64_t exponent_le = ldq_le_p(public_key + RASPI_SECURE_RSA_BYTES);
    size_t exponent_size = 0;
    gnutls_pubkey_t key;
    gnutls_datum_t modulus_datum = {
        .data = modulus,
        .size = sizeof(modulus),
    };
    gnutls_datum_t exponent_datum;
    gnutls_datum_t signature_datum = {
        .data = (uint8_t *)signature,
        .size = RASPI_SECURE_RSA_BYTES,
    };
    gnutls_datum_t digest_datum = {
        .data = (uint8_t *)digest,
        .size = RASPI_SECURE_KEY_HASH_SIZE,
    };
    int ret;

    for (size_t i = 0; i < sizeof(modulus); i++) {
        modulus[i] = public_key[sizeof(modulus) - 1 - i];
    }
    do {
        exponent[sizeof(exponent) - 1 - exponent_size++] = exponent_le;
        exponent_le >>= 8;
    } while (exponent_le);
    exponent_datum.data = exponent + sizeof(exponent) - exponent_size;
    exponent_datum.size = exponent_size;

    if (gnutls_pubkey_init(&key) < 0) {
        return false;
    }
    ret = gnutls_pubkey_import_rsa_raw(key, &modulus_datum,
                                       &exponent_datum);
    if (ret >= 0) {
        ret = gnutls_pubkey_verify_hash2(
            key, GNUTLS_SIGN_RSA_SHA256, 0, &digest_datum,
            &signature_datum);
    }
    gnutls_pubkey_deinit(key);
    return ret >= 0;
}
#endif

static RaspiSecureResult raspi_secure_parse_signature(
    const uint8_t *signature_file, size_t signature_file_size,
    uint8_t expected_digest[RASPI_SECURE_KEY_HASH_SIZE],
    uint8_t signature[RASPI_SECURE_RSA_BYTES], Error **errp)
{
    g_autofree char *text = NULL;
    g_auto(GStrv) lines = NULL;
    uint64_t timestamp;

    if (!signature_file_size ||
        memchr(signature_file, 0, signature_file_size)) {
        goto malformed;
    }
    text = g_strndup((const char *)signature_file, signature_file_size);
    lines = g_strsplit(text, "\n", -1);
    if (!lines[0] || !lines[1] || !lines[2] ||
        (lines[3] && (lines[3][0] || lines[4])) ||
        !raspi_secure_parse_hex(lines[0], strlen(lines[0]), expected_digest,
                                RASPI_SECURE_KEY_HASH_SIZE) ||
        !g_str_has_prefix(lines[1], "ts: ") ||
        qemu_strtou64(lines[1] + 4, NULL, 10, &timestamp) < 0 ||
        !g_str_has_prefix(lines[2], "rsa2048: ") ||
        !raspi_secure_parse_hex(
            lines[2] + strlen("rsa2048: "),
            strlen(lines[2] + strlen("rsa2048: ")), signature,
            RASPI_SECURE_RSA_BYTES)) {
        goto malformed;
    }
    return RASPI_SECURE_OK;

malformed:
    error_setg(errp, "Raspberry Pi signature file is malformed");
    return RASPI_SECURE_SIGNATURE_FORMAT;
}

RaspiSecureResult raspi_secure_check_public_key(
    const uint8_t public_key[RASPI_SECURE_PUBLIC_KEY_SIZE],
    const uint8_t expected_hash[RASPI_SECURE_KEY_HASH_SIZE], Error **errp)
{
    g_autofree uint8_t *actual_hash = NULL;
    g_autofree uint8_t *der = NULL;
    size_t actual_hash_size = 0;
    size_t der_size;

    der = raspi_secure_public_key_der(public_key, &der_size, errp);
    if (!der) {
        return RASPI_SECURE_KEY_FORMAT;
    }
    if (qcrypto_hash_bytes(QCRYPTO_HASH_ALGO_SHA256, public_key,
                           RASPI_SECURE_PUBLIC_KEY_SIZE, &actual_hash,
                           &actual_hash_size, errp) < 0) {
        return RASPI_SECURE_CRYPTO_UNAVAILABLE;
    }
    if (actual_hash_size != RASPI_SECURE_KEY_HASH_SIZE ||
        memcmp(actual_hash, expected_hash, RASPI_SECURE_KEY_HASH_SIZE)) {
        error_setg(errp, "Raspberry Pi public key does not match OTP");
        return RASPI_SECURE_KEY_HASH_MISMATCH;
    }
    return RASPI_SECURE_OK;
}

RaspiSecureResult raspi_secure_verify(
    const uint8_t public_key[RASPI_SECURE_PUBLIC_KEY_SIZE],
    const uint8_t *image, size_t image_size,
    const uint8_t *signature_file, size_t signature_file_size,
    Error **errp)
{
    QCryptoAkCipherOptions options = {
        .alg = QCRYPTO_AK_CIPHER_ALGO_RSA,
        .u.rsa = {
            .padding_alg = QCRYPTO_RSA_PADDING_ALGO_PKCS1,
            .hash_alg = QCRYPTO_HASH_ALGO_SHA256,
        },
    };
    uint8_t signature[RASPI_SECURE_RSA_BYTES];
    uint8_t expected_digest[RASPI_SECURE_KEY_HASH_SIZE];
    g_autofree uint8_t *actual_digest = NULL;
    g_autofree uint8_t *der = NULL;
    g_autoptr(QCryptoAkCipher) cipher = NULL;
    size_t actual_digest_size = 0;
    size_t der_size;
    RaspiSecureResult result;
    Error *local_err = NULL;

    result = raspi_secure_parse_signature(
        signature_file, signature_file_size, expected_digest, signature,
        errp);
    if (result != RASPI_SECURE_OK) {
        return result;
    }
    if (qcrypto_hash_bytes(QCRYPTO_HASH_ALGO_SHA256, image, image_size,
                           &actual_digest, &actual_digest_size, errp) < 0) {
        return RASPI_SECURE_CRYPTO_UNAVAILABLE;
    }
    if (actual_digest_size != RASPI_SECURE_KEY_HASH_SIZE ||
        memcmp(actual_digest, expected_digest, sizeof(expected_digest))) {
        error_setg(errp, "Raspberry Pi signed image digest is invalid");
        return RASPI_SECURE_IMAGE_HASH_MISMATCH;
    }
    der = raspi_secure_public_key_der(public_key, &der_size, errp);
    if (!der) {
        return RASPI_SECURE_KEY_FORMAT;
    }
    if (qcrypto_akcipher_supports(&options)) {
        cipher = qcrypto_akcipher_new(
            &options, QCRYPTO_AK_CIPHER_KEY_TYPE_PUBLIC, der, der_size,
            &local_err);
        if (!cipher) {
            error_propagate(errp, local_err);
            return RASPI_SECURE_KEY_FORMAT;
        }
        if (qcrypto_akcipher_verify(cipher, signature, sizeof(signature),
                                    actual_digest, actual_digest_size,
                                    &local_err) < 0) {
            error_free(local_err);
            error_setg(errp, "Raspberry Pi RSA signature is invalid");
            return RASPI_SECURE_RSA_MISMATCH;
        }
        return RASPI_SECURE_OK;
    }
#ifdef CONFIG_GNUTLS
    if (raspi_secure_verify_gnutls(public_key, signature, actual_digest)) {
        return RASPI_SECURE_OK;
    }
    error_setg(errp, "Raspberry Pi RSA signature is invalid");
    return RASPI_SECURE_RSA_MISMATCH;
#else
    error_setg(errp, "RSA PKCS#1 v1.5 SHA-256 is unavailable");
    return RASPI_SECURE_CRYPTO_UNAVAILABLE;
#endif
}

const char *raspi_secure_result_name(RaspiSecureResult result)
{
    switch (result) {
    case RASPI_SECURE_OK:
        return "verified";
    case RASPI_SECURE_KEY_FORMAT:
        return "key-format-invalid";
    case RASPI_SECURE_KEY_HASH_MISMATCH:
        return "key-hash-mismatch";
    case RASPI_SECURE_SIGNATURE_FORMAT:
        return "signature-format-invalid";
    case RASPI_SECURE_IMAGE_HASH_MISMATCH:
        return "image-hash-mismatch";
    case RASPI_SECURE_RSA_MISMATCH:
        return "rsa-signature-invalid";
    case RASPI_SECURE_CRYPTO_UNAVAILABLE:
        return "crypto-unavailable";
    case RASPI_SECURE_BOOTSYS_MISSING:
        return "bootsys-missing";
    case RASPI_SECURE_BOOTSYS_TRUST_MISSING:
        return "bootsys-trust-missing";
    case RASPI_SECURE_BOOTSYS_FORMAT:
        return "bootsys-format-invalid";
    case RASPI_SECURE_BOOTSYS_HASH_MISMATCH:
        return "bootsys-hash-mismatch";
    case RASPI_SECURE_BOOTSYS_DEVKEY_REVOKED:
        return "bootsys-development-key-revoked";
    case RASPI_SECURE_DEPENDENCY_MISSING:
        return "dependency-missing";
    case RASPI_SECURE_DEPENDENCY_FORMAT:
        return "dependency-format-invalid";
    case RASPI_SECURE_DEPENDENCY_HASH_MISMATCH:
        return "dependency-hash-mismatch";
    default:
        g_assert_not_reached();
    }
}
