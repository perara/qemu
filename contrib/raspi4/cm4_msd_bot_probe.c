/*
 * Host-side USB Mass Storage Bulk-Only Transport invalid-CBW probe.
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <endian.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libusb.h>

#define TARGET_VID 0x0a5c
#define TARGET_PID 0x0104
#define TARGET_SERIAL "51554d5552504934"
#define INTERFACE 0
#define BULK_OUT 0x01
#define BULK_IN 0x81
#define CBW_SIGNATURE 0x43425355U
#define CSW_SIGNATURE 0x53425355U
#define MSC_REQUEST_RESET 0xff
#define TIMEOUT_MS 5000
#define CSW_PASSED 0
#define CSW_FAILED 1
#define CSW_PHASE_ERROR 2
#define FUZZ_SEED 0x52504934U
#define FUZZ_CASES 128U
#define FUZZ_FNV64 UINT64_C(0x2e5f4d8d3041b5d7)

typedef struct CommandBlockWrapper {
    uint32_t signature;
    uint32_t tag;
    uint32_t transfer_length;
    uint8_t flags;
    uint8_t lun;
    uint8_t cdb_length;
    uint8_t cdb[16];
} __attribute__((packed)) CommandBlockWrapper;

typedef struct CommandStatusWrapper {
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;
    uint8_t status;
} __attribute__((packed)) CommandStatusWrapper;

static libusb_context *usb_context;
static libusb_device_handle *handle;
static bool interface_claimed;
static bool kernel_detached;
static uint32_t next_tag = 1;
static const char *probe_stage = "startup";

static uint32_t prng_next(uint32_t *state)
{
    uint32_t value = *state;

    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

static bool implemented_opcode(uint8_t opcode)
{
    switch (opcode) {
    case 0x00: /* TEST UNIT READY */
    case 0x03: /* REQUEST SENSE */
    case 0x12: /* INQUIRY */
    case 0x1a: /* MODE SENSE(6) */
    case 0x1b: /* START STOP UNIT */
    case 0x1e: /* PREVENT ALLOW MEDIUM REMOVAL */
    case 0x23: /* READ FORMAT CAPACITIES */
    case 0x25: /* READ CAPACITY(10) */
    case 0x28: /* READ(10) */
    case 0x2a: /* WRITE(10) */
    case 0x2f: /* VERIFY(10) */
    case 0x35: /* SYNCHRONIZE CACHE(10) */
    case 0x5a: /* MODE SENSE(10) */
    case 0x88: /* READ(16) */
    case 0x8a: /* WRITE(16) */
    case 0x91: /* SYNCHRONIZE CACHE(16) */
    case 0x9e: /* SERVICE ACTION IN(16) */
    case 0xa0: /* REPORT LUNS */
        return true;
    default:
        return false;
    }
}

static CommandBlockWrapper fuzz_cbw(uint32_t *state)
{
    CommandBlockWrapper cbw = {
        .signature = htole32(CBW_SIGNATURE),
        .tag = htole32(prng_next(state)),
        .transfer_length = 0,
        .flags = prng_next(state) & 1 ? 0x80 : 0,
        .lun = 0,
        .cdb_length = 1 + prng_next(state) % 16,
    };
    uint8_t opcode;
    size_t i;

    for (i = 0; i < sizeof(cbw.cdb); i++) {
        cbw.cdb[i] = prng_next(state);
    }
    do {
        opcode = prng_next(state);
    } while (implemented_opcode(opcode));
    cbw.cdb[0] = opcode;
    return cbw;
}

static uint64_t fuzz_hash_update(uint64_t hash, const void *data,
                                 size_t length)
{
    const uint8_t *bytes = data;
    size_t i;

    for (i = 0; i < length; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void cleanup(void)
{
    if (handle && interface_claimed) {
        libusb_release_interface(handle, INTERFACE);
        interface_claimed = false;
    }
    if (handle && kernel_detached) {
        libusb_attach_kernel_driver(handle, INTERFACE);
        kernel_detached = false;
    }
    if (handle) {
        libusb_close(handle);
        handle = NULL;
    }
    if (usb_context) {
        libusb_exit(usb_context);
        usb_context = NULL;
    }
}

static __attribute__((noreturn)) void fail(const char *what)
{
    fprintf(stderr, "cm4-msd-bot-probe: %s\n", what);
    exit(EXIT_FAILURE);
}

static __attribute__((noreturn)) void fail_libusb(const char *what, int error)
{
    fprintf(stderr, "cm4-msd-bot-probe: %s during %s: %s\n",
            what, probe_stage, libusb_error_name(error));
    exit(EXIT_FAILURE);
}

static libusb_device_handle *open_target(void)
{
    libusb_device **devices;
    libusb_device_handle *candidate = NULL;
    ssize_t count;
    ssize_t i;

    count = libusb_get_device_list(usb_context, &devices);
    if (count < 0) {
        fail_libusb("list USB devices", count);
    }
    for (i = 0; i < count; i++) {
        struct libusb_device_descriptor descriptor;
        unsigned char serial[128];
        int length;
        int error;

        error = libusb_get_device_descriptor(devices[i], &descriptor);
        if (error || descriptor.idVendor != TARGET_VID ||
            descriptor.idProduct != TARGET_PID || !descriptor.iSerialNumber) {
            continue;
        }
        error = libusb_open(devices[i], &candidate);
        if (error) {
            continue;
        }
        length = libusb_get_string_descriptor_ascii(
            candidate, descriptor.iSerialNumber, serial, sizeof(serial));
        if (length == strlen(TARGET_SERIAL) &&
            !memcmp(serial, TARGET_SERIAL, length)) {
            break;
        }
        libusb_close(candidate);
        candidate = NULL;
    }
    libusb_free_device_list(devices, 1);
    return candidate;
}

static void bulk_out(const void *data, int length)
{
    int transferred = 0;
    int error = libusb_bulk_transfer(handle, BULK_OUT,
                                     (unsigned char *)data, length,
                                     &transferred, TIMEOUT_MS);

    if (error) {
        fail_libusb("Bulk-Out transfer", error);
    }
    if (transferred != length) {
        fail("short Bulk-Out transfer");
    }
}

static void bulk_in(void *data, int length, int expected,
                    const char *what)
{
    int transferred = 0;
    int error = libusb_bulk_transfer(handle, BULK_IN, data, length,
                                     &transferred, TIMEOUT_MS);

    if (error && !(expected < length && transferred == expected &&
                   (error == LIBUSB_ERROR_IO ||
                    error == LIBUSB_ERROR_PIPE))) {
        fail_libusb(what, error);
    }
    if (transferred != expected) {
        fprintf(stderr,
                "cm4-msd-bot-probe: %s: expected %d bytes, got %d\n",
                what, expected, transferred);
        exit(EXIT_FAILURE);
    }
}

static void expect_pipe(uint8_t endpoint, void *data, int length,
                        const char *what)
{
    int transferred = 0;
    int error = libusb_bulk_transfer(handle, endpoint, data, length,
                                     &transferred, TIMEOUT_MS);

    if (error != LIBUSB_ERROR_PIPE) {
        fprintf(stderr,
                "cm4-msd-bot-probe: %s: expected PIPE, got %s and %d bytes\n",
                what, libusb_error_name(error), transferred);
        exit(EXIT_FAILURE);
    }
}

static void expect_data_halt(void *data, int length, const char *what)
{
    int transferred = 0;
    int error = libusb_bulk_transfer(handle, BULK_IN, data, length,
                                     &transferred, TIMEOUT_MS);

    if ((error != LIBUSB_ERROR_PIPE && error != LIBUSB_ERROR_IO) ||
        transferred != 0) {
        fprintf(stderr,
                "cm4-msd-bot-probe: %s: expected halted Bulk-In, got %s "
                "and %d bytes\n",
                what, libusb_error_name(error), transferred);
        exit(EXIT_FAILURE);
    }
}

static void send_invalid_cbw(const void *data, int length)
{
    int transferred = 0;
    int error = libusb_bulk_transfer(handle, BULK_OUT,
                                     (unsigned char *)data, length,
                                     &transferred, TIMEOUT_MS);

    if (error != 0 && error != LIBUSB_ERROR_PIPE) {
        fail_libusb("invalid-CBW Bulk-Out transfer", error);
    }
    if (!error && transferred != length) {
        fail("short invalid-CBW Bulk-Out transfer");
    }
}

static CommandBlockWrapper command(uint8_t opcode, uint32_t length,
                                   uint8_t flags, uint8_t cdb_length)
{
    CommandBlockWrapper cbw = {
        .signature = htole32(CBW_SIGNATURE),
        .tag = htole32(next_tag++),
        .transfer_length = htole32(length),
        .flags = flags,
        .lun = 0,
        .cdb_length = cdb_length,
        .cdb = { opcode },
    };

    return cbw;
}

static void read_csw(const CommandBlockWrapper *cbw, uint32_t residue,
                     uint8_t status)
{
    CommandStatusWrapper csw;
    int transferred = 0;
    int error = libusb_bulk_transfer(handle, BULK_IN,
                                     (unsigned char *)&csw, sizeof(csw),
                                     &transferred, TIMEOUT_MS);

    if (error) {
        fail_libusb("read CSW", error);
    }
    if (transferred != sizeof(csw) ||
        le32toh(csw.signature) != CSW_SIGNATURE ||
        csw.tag != cbw->tag || le32toh(csw.residue) != residue ||
        csw.status != status) {
        fprintf(stderr,
                "cm4-msd-bot-probe: invalid CSW: residue=%" PRIu32
                " status=%u, expected residue=%" PRIu32 " status=%u\n",
                le32toh(csw.residue), csw.status, residue, status);
        exit(EXIT_FAILURE);
    }
}

static void test_unit_ready(void)
{
    CommandBlockWrapper cbw = command(0x00, 0, 0, 6);

    bulk_out(&cbw, sizeof(cbw));
    read_csw(&cbw, 0, CSW_PASSED);
}

static void inquiry(void)
{
    CommandBlockWrapper cbw = command(0x12, 36, 0x80, 6);
    unsigned char data[36];

    cbw.cdb[4] = sizeof(data);
    bulk_out(&cbw, sizeof(cbw));
    bulk_in(data, sizeof(data), sizeof(data), "INQUIRY data");
    if (memcmp(data + 8, "mmcblk0 ", 8)) {
        fail("invalid INQUIRY after recovery");
    }
    read_csw(&cbw, 0, CSW_PASSED);
}

static void clear_in_halt(void)
{
    int error = libusb_clear_halt(handle, BULK_IN);

    if (error) {
        fail_libusb("clear Bulk-In halt", error);
    }
}

static void reset_recovery(void)
{
    int error;

    error = libusb_control_transfer(
        handle,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_CLASS |
        LIBUSB_RECIPIENT_INTERFACE,
        MSC_REQUEST_RESET, 0, INTERFACE, NULL, 0, TIMEOUT_MS);
    if (error < 0) {
        fail_libusb("Mass Storage Reset", error);
    }
    error = libusb_clear_halt(handle, BULK_IN);
    if (error) {
        fail_libusb("clear Bulk-In halt", error);
    }
    error = libusb_clear_halt(handle, BULK_OUT);
    if (error) {
        fail_libusb("clear Bulk-Out halt", error);
    }
}

static void invalid_cbw(const CommandBlockWrapper *cbw, int length)
{
    CommandStatusWrapper scratch;
    CommandBlockWrapper valid = command(0x00, 0, 0, 6);

    send_invalid_cbw(cbw, length);
    expect_pipe(BULK_IN, &scratch, sizeof(scratch),
                "Bulk-In invalid-CBW stall");
    expect_pipe(BULK_OUT, &valid, sizeof(valid), "Bulk-Out invalid-CBW stall");
    reset_recovery();
    test_unit_ready();
}

static CommandBlockWrapper inquiry_command(uint32_t host_length,
                                           uint8_t flags)
{
    CommandBlockWrapper cbw = command(0x12, host_length, flags, 6);

    cbw.cdb[4] = 36;
    return cbw;
}

static CommandBlockWrapper verify_command(uint32_t host_length,
                                          uint8_t flags)
{
    CommandBlockWrapper cbw = command(0x2f, host_length, flags, 10);

    cbw.cdb[1] = 0x02; /* BYTCHK: one block of non-mutating data-out. */
    cbw.cdb[7] = 0;
    cbw.cdb[8] = 1;
    return cbw;
}

static void phase_recovery(const CommandBlockWrapper *cbw, uint32_t residue)
{
    read_csw(cbw, residue, CSW_PHASE_ERROR);
    reset_recovery();
    test_unit_ready();
}

static void thirteen_cases(void)
{
    CommandBlockWrapper cbw;
    unsigned char data[1024] = { 0xa5 };

    probe_stage = "BOT case 1 Hn=Dn";
    test_unit_ready();

    probe_stage = "BOT case 2 Hn<Di";
    cbw = inquiry_command(0, 0);
    bulk_out(&cbw, sizeof(cbw));
    phase_recovery(&cbw, 0);

    probe_stage = "BOT case 3 Hn<Do";
    cbw = verify_command(0, 0);
    bulk_out(&cbw, sizeof(cbw));
    phase_recovery(&cbw, 0);

    probe_stage = "BOT case 4 Hi>Dn";
    cbw = command(0x00, 1, 0x80, 6);
    bulk_out(&cbw, sizeof(cbw));
    expect_data_halt(data, 1, "case 4 data stall");
    clear_in_halt();
    read_csw(&cbw, 1, CSW_PASSED);

    probe_stage = "BOT case 5 Hi>Di";
    cbw = inquiry_command(64, 0x80);
    bulk_out(&cbw, sizeof(cbw));
    bulk_in(data, 64, 36, "case 5 short INQUIRY");
    expect_data_halt(data, 13, "case 5 status stall");
    clear_in_halt();
    read_csw(&cbw, 28, CSW_PASSED);

    probe_stage = "BOT case 6 Hi=Di";
    inquiry();

    probe_stage = "BOT case 7 Hi<Di";
    cbw = inquiry_command(16, 0x80);
    bulk_out(&cbw, sizeof(cbw));
    bulk_in(data, 16, 16, "case 7 truncated INQUIRY");
    expect_data_halt(data, 13, "case 7 status stall");
    clear_in_halt();
    phase_recovery(&cbw, 0);

    probe_stage = "BOT case 8 Hi<>Do";
    cbw = verify_command(512, 0x80);
    bulk_out(&cbw, sizeof(cbw));
    expect_data_halt(data, 512, "case 8 data stall");
    clear_in_halt();
    phase_recovery(&cbw, 512);

    probe_stage = "BOT case 9 Ho>Dn";
    cbw = command(0x00, 32, 0, 6);
    bulk_out(&cbw, sizeof(cbw));
    bulk_out(data, 32);
    read_csw(&cbw, 32, CSW_PASSED);

    probe_stage = "BOT case 10 Ho<>Di";
    cbw = inquiry_command(36, 0);
    bulk_out(&cbw, sizeof(cbw));
    bulk_out(data, 36);
    phase_recovery(&cbw, 36);

    probe_stage = "BOT case 11 Ho>Do";
    cbw = verify_command(1024, 0);
    bulk_out(&cbw, sizeof(cbw));
    bulk_out(data, 1024);
    read_csw(&cbw, 512, CSW_PASSED);

    probe_stage = "BOT case 12 Ho=Do";
    cbw = verify_command(512, 0);
    bulk_out(&cbw, sizeof(cbw));
    bulk_out(data, 512);
    read_csw(&cbw, 0, CSW_PASSED);

    probe_stage = "BOT case 13 Ho<Do";
    cbw = verify_command(256, 0);
    bulk_out(&cbw, sizeof(cbw));
    bulk_out(data, 256);
    phase_recovery(&cbw, 0);
}

static void meaningless_cbw(CommandBlockWrapper cbw, const char *stage)
{
    probe_stage = stage;
    bulk_out(&cbw, sizeof(cbw));
    read_csw(&cbw, 0, CSW_FAILED);
    test_unit_ready();
}

static void meaningless_cbws(void)
{
    CommandBlockWrapper cbw;

    cbw = command(0x00, 0, 0x01, 6);
    meaningless_cbw(cbw, "meaningless reserved flag");

    cbw = command(0x00, 0, 0, 6);
    cbw.lun = 1;
    meaningless_cbw(cbw, "meaningless unsupported LUN");

    cbw = command(0x00, 0, 0, 0);
    meaningless_cbw(cbw, "meaningless zero CDB length");

    cbw = command(0x00, 0, 0, 17);
    meaningless_cbw(cbw, "meaningless oversized CDB length");

    cbw = command(0x00, 0, 0, 10);
    meaningless_cbw(cbw, "meaningless opcode CDB length");
}

static uint64_t randomized_cbw_corpus(bool transmit)
{
    uint32_t state = FUZZ_SEED;
    uint64_t hash = UINT64_C(14695981039346656037);
    static char stage[160];
    unsigned int i;

    for (i = 0; i < FUZZ_CASES; i++) {
        CommandBlockWrapper cbw = fuzz_cbw(&state);

        hash = fuzz_hash_update(hash, &cbw, sizeof(cbw));
        if (transmit) {
            snprintf(stage, sizeof(stage),
                     "randomized case %u opcode=0x%02x cdb-length=%u "
                     "flags=0x%02x tag=0x%08x",
                     i, cbw.cdb[0], cbw.cdb_length, cbw.flags,
                     le32toh(cbw.tag));
            probe_stage = stage;
            bulk_out(&cbw, sizeof(cbw));
            read_csw(&cbw, 0, CSW_FAILED);
            if ((i + 1) % 16 == 0) {
                test_unit_ready();
            }
        }
    }
    return hash;
}

static int self_test(void)
{
    CommandBlockWrapper cbw = command(0x12, 36, 0x80, 6);
    uint64_t fuzz_hash = randomized_cbw_corpus(false);

    if (sizeof(cbw) != 31 || sizeof(CommandStatusWrapper) != 13 ||
        le32toh(cbw.signature) != CBW_SIGNATURE || cbw.cdb[0] != 0x12 ||
        fuzz_hash != FUZZ_FNV64) {
        fail("wrapper self-test failed");
    }
    printf("cm4-msd-bot-probe self-test: fuzz-seed=0x%08x cases=%u "
           "fnv64=%016" PRIx64 ": PASS\n",
           FUZZ_SEED, FUZZ_CASES, fuzz_hash);
    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    CommandBlockWrapper bad_signature;
    CommandBlockWrapper short_cbw;
    int active;
    int error;

    if (argc == 2 && !strcmp(argv[1], "--self-test")) {
        return self_test();
    }
    if (argc != 1) {
        fail("usage: qemu-rpi-cm4-bot-probe [--self-test]");
    }
    if (atexit(cleanup)) {
        fail("register cleanup");
    }
    error = libusb_init(&usb_context);
    if (error) {
        fail_libusb("initialize libusb", error);
    }
    handle = open_target();
    if (!handle) {
        fail("target 0a5c:0104 serial 51554d5552504934 not found");
    }
    active = libusb_kernel_driver_active(handle, INTERFACE);
    if (active < 0) {
        fail_libusb("query kernel driver", active);
    }
    if (active) {
        error = libusb_detach_kernel_driver(handle, INTERFACE);
        if (error) {
            fail_libusb("detach usb-storage", error);
        }
        kernel_detached = true;
    }
    error = libusb_claim_interface(handle, INTERFACE);
    if (error) {
        fail_libusb("claim mass-storage interface", error);
    }
    interface_claimed = true;

    test_unit_ready();
    thirteen_cases();
    meaningless_cbws();
    printf("cm4-msd-bot-probe: fuzz-seed=0x%08x cases=%u fnv64=%016"
           PRIx64 "\n",
           FUZZ_SEED, FUZZ_CASES, randomized_cbw_corpus(true));
    fflush(stdout);
    probe_stage = "bad-signature CBW";
    bad_signature = command(0x00, 0, 0, 6);
    bad_signature.signature = htole32(0xdeadbeefU);
    invalid_cbw(&bad_signature, sizeof(bad_signature));

    probe_stage = "short CBW";
    short_cbw = command(0x00, 0, 0, 6);
    invalid_cbw(&short_cbw, sizeof(short_cbw) - 1);
    probe_stage = "final INQUIRY";
    inquiry();

    puts("cm4-msd-bot-probe: all 13 BOT cases, 5 valid-but-meaningless "
         "CBWs, 128 deterministic randomized CBW/SCSI cases, invalid "
         "signature, short CBW, endpoint stalls, ordered Reset Recovery, "
         "and clean INQUIRY: PASS");
    return EXIT_SUCCESS;
}
