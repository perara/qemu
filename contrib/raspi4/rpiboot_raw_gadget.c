/*
 * Linux Raw Gadget bridge for the BCM2711 RPIBOOT protocol.
 *
 * Copyright (c) 2026 QEMU contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This helper intentionally implements the device side of the protocol used
 * by the unmodified Raspberry Pi rpiboot host program.  It is not a private
 * replacement for rpiboot.  The first enumeration receives bootcode4.bin;
 * the second enumeration asks rpiboot for the requested files.  Every byte
 * received from the host is written to capture-dir for exact comparison by
 * the caller.
 */

#include <errno.h>
#include <endian.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <linux/usb/ch9.h>
#include <linux/usb/raw_gadget.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define BCM2711_VID 0x0a5c
#define BCM2711_PID 0x2711
#define EP0_MAX_DATA 512
#define BULK_CHUNK (16 * 1024)
#define MAX_BOOTCODE_SIZE (128 * 1024 * 1024)
#define MAX_SECOND_STAGE_FILE_SIZE (128 * 1024 * 1024)
#define MAX_SECOND_STAGE_TOTAL_SIZE (512ULL * 1024 * 1024)
#define MAX_REQUESTS 128
#define RPIBOOT_NAME_SIZE 256
#define FAULT_EXIT_STATUS 75

typedef enum FaultPoint {
    FAULT_NONE,
    FAULT_BOOTCODE_DISCONNECT,
    FAULT_FILE_DISCONNECT,
    FAULT_BOOTCODE_HOLD,
    FAULT_FILE_HOLD,
    FAULT_BOOTCODE_RESET,
    FAULT_FILE_RESET,
    FAULT_BOTH_RESET,
    FAULT_ROM_STATUS_STALL,
    FAULT_ROM_STATUS_TIMEOUT,
    FAULT_FILE_REQUEST_TIMEOUT,
} FaultPoint;

typedef enum BulkTask {
    BULK_TASK_NONE,
    BULK_TASK_BOOT_MESSAGE,
    BULK_TASK_CAPTURE,
} BulkTask;

typedef struct ControlEvent {
    struct usb_raw_event event;
    struct usb_ctrlrequest ctrl;
} ControlEvent;

typedef struct ControlIO {
    struct usb_raw_ep_io io;
    uint8_t data[EP0_MAX_DATA];
} ControlIO;

typedef struct BulkIO {
    struct usb_raw_ep_io io;
    uint8_t data[BULK_CHUNK];
} BulkIO;

typedef struct BootMessage {
    uint32_t length;
    uint8_t signature[20];
} __attribute__((packed)) BootMessage;

typedef struct FileMessage {
    uint32_t command;
    char name[RPIBOOT_NAME_SIZE];
} __attribute__((packed)) FileMessage;

typedef struct Bridge {
    const char *capture_dir;
    const char *lifecycle;
    int lifecycle_lock_fd;
    const char *udc_driver;
    const char *udc_device;
    const char *requests[MAX_REQUESTS];
    uint32_t request_sizes[MAX_REQUESTS];
    bool request_size_known[MAX_REQUESTS];
    size_t request_count;
    size_t request_index;
    uint64_t request_total_size;
    int fd;
    int bulk_out;
    bool configured;
    bool second_stage;
    bool finished;
    bool mass_storage;
    bool recover_stale;
    bool self_test;
    bool udc_option;
    pthread_t bulk_thread;
    pthread_mutex_t bulk_lock;
    pthread_cond_t bulk_cond;
    bool bulk_thread_started;
    bool bulk_stop;
    bool bulk_pending;
    BulkTask bulk_task;
    uint32_t bulk_length;
    char bulk_name[RPIBOOT_NAME_SIZE];
    uint64_t bulk_generation;
    uint64_t reset_generation;
    FaultPoint fault;
    uint32_t fault_after;
    const char *fault_file;
    uint32_t fault_count;
    uint32_t boot_reset_events;
    uint32_t file_reset_events;
    bool fault_count_set;
    uint32_t fault_delay_ms;
    bool fault_delay_set;
    uint32_t expected_bootcode;
    enum {
        FIRST_EXPECT_MESSAGE,
        FIRST_EXPECT_BOOTCODE,
        FIRST_EXPECT_STATUS,
        FILE_SEND_SIZE,
        FILE_WAIT_SIZE,
        FILE_SEND_READ,
        FILE_WAIT_DATA,
        FILE_SEND_DONE,
    } state;
} Bridge;

static const struct usb_device_descriptor device_descriptor = {
    .bLength = USB_DT_DEVICE_SIZE,
    .bDescriptorType = USB_DT_DEVICE,
    .bcdUSB = __cpu_to_le16(0x0200),
    .bDeviceClass = USB_CLASS_VENDOR_SPEC,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = 64,
    .idVendor = __cpu_to_le16(BCM2711_VID),
    .idProduct = __cpu_to_le16(BCM2711_PID),
    .bcdDevice = __cpu_to_le16(0x0100),
    .iManufacturer = 1,
    .iProduct = 2,
    /* Patched to zero for ROM and one for the second-stage file server. */
    .iSerialNumber = 0,
    .bNumConfigurations = 1,
};

static const struct usb_qualifier_descriptor qualifier_descriptor = {
    .bLength = sizeof(struct usb_qualifier_descriptor),
    .bDescriptorType = USB_DT_DEVICE_QUALIFIER,
    .bcdUSB = __cpu_to_le16(0x0200),
    .bDeviceClass = USB_CLASS_VENDOR_SPEC,
    .bMaxPacketSize0 = 64,
    .bNumConfigurations = 1,
};

static struct usb_endpoint_descriptor bulk_out_descriptor = {
    .bLength = USB_DT_ENDPOINT_SIZE,
    .bDescriptorType = USB_DT_ENDPOINT,
    .bEndpointAddress = USB_DIR_OUT | 1,
    .bmAttributes = USB_ENDPOINT_XFER_BULK,
    .wMaxPacketSize = __cpu_to_le16(512),
};

static const struct usb_config_descriptor config_descriptor = {
    .bLength = USB_DT_CONFIG_SIZE,
    .bDescriptorType = USB_DT_CONFIG,
    .wTotalLength = __cpu_to_le16(USB_DT_CONFIG_SIZE +
                                  USB_DT_INTERFACE_SIZE +
                                  USB_DT_ENDPOINT_SIZE),
    .bNumInterfaces = 1,
    .bConfigurationValue = 1,
    .iConfiguration = 0,
    .bmAttributes = USB_CONFIG_ATT_ONE | USB_CONFIG_ATT_SELFPOWER,
    .bMaxPower = 1,
};

static const struct usb_interface_descriptor interface_descriptor = {
    .bLength = USB_DT_INTERFACE_SIZE,
    .bDescriptorType = USB_DT_INTERFACE,
    .bInterfaceNumber = 0,
    .bAlternateSetting = 0,
    .bNumEndpoints = 1,
    .bInterfaceClass = USB_CLASS_VENDOR_SPEC,
    .bInterfaceSubClass = 0,
    .bInterfaceProtocol = 0,
    .iInterface = 0,
};

static __attribute__((noreturn)) void fail_errno(const char *what)
{
    fprintf(stderr, "rpiboot-raw-gadget: %s: %s\n", what, strerror(errno));
    exit(EXIT_FAILURE);
}

static __attribute__((noreturn)) void fail(const char *what)
{
    fprintf(stderr, "rpiboot-raw-gadget: %s\n", what);
    exit(EXIT_FAILURE);
}

static void lifecycle_lock(Bridge *b);
static void record_usb_reset(Bridge *b, bool log_event);
static bool record_file_size(Bridge *b, size_t index, uint32_t length);
static bool file_data_size_matches(const Bridge *b, size_t index,
                                   uint32_t length);

static void lifecycle_write(Bridge *b, const char *state)
{
    struct stat metadata;
    char temporary[4096];
    char contents[128];
    int content_length;
    int path_length;
    int fd;
    ssize_t offset = 0;

    if (!b->lifecycle) {
        return;
    }
    if (stat(b->lifecycle, &metadata) < 0) {
        fail_errno("stat lifecycle state");
    }
    path_length = snprintf(temporary, sizeof(temporary), "%s.tmp.%ld",
                           b->lifecycle, (long)getpid());
    content_length = snprintf(contents, sizeof(contents), "%s\n", state);
    if (path_length < 0 || (size_t)path_length >= sizeof(temporary) ||
        content_length < 0 || (size_t)content_length >= sizeof(contents)) {
        fail("lifecycle path or state is too long");
    }
    fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
              O_NOFOLLOW, 0644);
    if (fd < 0) {
        fail_errno("create lifecycle temporary file");
    }
    if ((geteuid() == 0 && fchown(fd, metadata.st_uid, metadata.st_gid) < 0) ||
        fchmod(fd, metadata.st_mode & 0777) < 0) {
        close(fd);
        unlink(temporary);
        fail_errno("preserve lifecycle ownership");
    }
    while (offset < content_length) {
        ssize_t written = write(fd, contents + offset,
                                content_length - offset);

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            unlink(temporary);
            fail_errno("write lifecycle state");
        }
        offset += written;
    }
    if (fsync(fd) < 0 || close(fd) < 0) {
        unlink(temporary);
        fail_errno("flush lifecycle state");
    }
    if (rename(temporary, b->lifecycle) < 0) {
        unlink(temporary);
        fail_errno("publish lifecycle state");
    }
}

static void lifecycle_read(Bridge *b, char *state, size_t state_size)
{
    ssize_t length;
    int fd;

    if (!b->lifecycle || state_size < 2) {
        fail("lifecycle state is unavailable");
    }
    fd = open(b->lifecycle, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        fail_errno("open lifecycle state");
    }
    length = read(fd, state, state_size - 1);
    if (length < 0) {
        close(fd);
        fail_errno("read lifecycle state");
    }
    if (close(fd) < 0) {
        fail_errno("close lifecycle state");
    }
    while (length && (state[length - 1] == '\n' ||
                      state[length - 1] == '\r')) {
        length--;
    }
    state[length] = '\0';
}

static bool lifecycle_is_recoverable(const char *state)
{
    return !strcmp(state, "rpiboot-active") ||
           !strcmp(state, "rpiboot-failed");
}

static void lifecycle_require_rpiboot_ready(Bridge *b)
{
    char state[128];

    if (!b->lifecycle) {
        return;
    }
    lifecycle_read(b, state, sizeof(state));
    if (strcmp(state, "rpiboot-host-ready") &&
        strcmp(state, "rpiboot-failed")) {
        fprintf(stderr,
                "rpiboot-raw-gadget: lifecycle state '%s' does not release "
                "the device to RPIBOOT\n",
                state);
        exit(EXIT_FAILURE);
    }
}

static void lifecycle_recover_stale(Bridge *b)
{
    char state[128];

    lifecycle_read(b, state, sizeof(state));
    if (!lifecycle_is_recoverable(state)) {
        fprintf(stderr,
                "rpiboot-raw-gadget: recover-stale requires lifecycle state "
                "'rpiboot-active' or 'rpiboot-failed', found '%s'\n",
                state);
        exit(EXIT_FAILURE);
    }
    lifecycle_write(b, "rpiboot-failed");
}

static void lifecycle_self_test(void)
{
    char directory[] = "/tmp/qemu-rpiboot-selftest-XXXXXX";
    char state_path[4096];
    char lock_path[4096];
    char state[128];
    Bridge bridge = { .lifecycle_lock_fd = -1, .fd = -1 };
    static const char active[] = "rpiboot-active\n";
    int state_length;
    int lock_length;
    int status;
    int thread_error;
    int fd;
    pid_t child;

    if (!mkdtemp(directory)) {
        fail_errno("create lifecycle self-test paths");
    }
    state_length = snprintf(state_path, sizeof(state_path), "%s/state",
                            directory);
    lock_length = snprintf(lock_path, sizeof(lock_path), "%s.lock",
                           state_path);
    if (state_length < 0 || (size_t)state_length >= sizeof(state_path) ||
        lock_length < 0 || (size_t)lock_length >= sizeof(lock_path)) {
        fail("lifecycle self-test path is too long");
    }
    fd = open(state_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        fail_errno("create lifecycle self-test state");
    }
    if (write(fd, active, sizeof(active) - 1) != sizeof(active) - 1) {
        close(fd);
        fail_errno("write lifecycle self-test state");
    }
    if (close(fd) < 0) {
        fail_errno("close lifecycle self-test state");
    }
    bridge.lifecycle = state_path;
    lifecycle_lock(&bridge);

    child = fork();
    if (child < 0) {
        fail_errno("fork lifecycle lock contender");
    }
    if (child == 0) {
        Bridge contender = {
            .lifecycle = state_path,
            .lifecycle_lock_fd = -1,
            .fd = -1,
        };

        close(bridge.lifecycle_lock_fd);
        lifecycle_lock(&contender);
        _exit(EXIT_SUCCESS);
    }
    if (waitpid(child, &status, 0) < 0) {
        fail_errno("wait for lifecycle lock contender");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) == EXIT_SUCCESS) {
        fail("live lifecycle owner was not rejected");
    }

    thread_error = pthread_mutex_init(&bridge.bulk_lock, NULL);
    if (!thread_error) {
        thread_error = pthread_cond_init(&bridge.bulk_cond, NULL);
    }
    if (thread_error) {
        errno = thread_error;
        fail_errno("initialize protocol reset self-test");
    }
    bridge.configured = true;
    bridge.second_stage = true;
    bridge.state = FILE_WAIT_DATA;
    bridge.request_index = 1;
    bridge.expected_bootcode = 1234;
    record_usb_reset(&bridge, false);
    if (bridge.configured || bridge.reset_generation != 1 ||
        bridge.state != FILE_WAIT_DATA || bridge.request_index != 1 ||
        bridge.expected_bootcode != 1234) {
        fail("USB reset protocol-state preservation self-test failed");
    }
    bridge.request_count = 5;
    if (!record_file_size(&bridge, 0, MAX_SECOND_STAGE_FILE_SIZE) ||
        !record_file_size(&bridge, 1, MAX_SECOND_STAGE_FILE_SIZE) ||
        !record_file_size(&bridge, 2, MAX_SECOND_STAGE_FILE_SIZE) ||
        !record_file_size(&bridge, 3, MAX_SECOND_STAGE_FILE_SIZE) ||
        record_file_size(&bridge, 4, 1) ||
        record_file_size(&bridge, 0, MAX_SECOND_STAGE_FILE_SIZE - 1) ||
        record_file_size(&bridge, 0, UINT32_MAX) ||
        !file_data_size_matches(
            &bridge, 0, MAX_SECOND_STAGE_FILE_SIZE) ||
        file_data_size_matches(
            &bridge, 0, MAX_SECOND_STAGE_FILE_SIZE - 1)) {
        fail("second-stage capture bounds self-test failed");
    }
    pthread_cond_destroy(&bridge.bulk_cond);
    pthread_mutex_destroy(&bridge.bulk_lock);

    lifecycle_recover_stale(&bridge);
    lifecycle_read(&bridge, state, sizeof(state));
    if (strcmp(state, "rpiboot-failed") ||
        !lifecycle_is_recoverable("rpiboot-active") ||
        !lifecycle_is_recoverable("rpiboot-failed") ||
        lifecycle_is_recoverable("rpiboot-host-ready") ||
        lifecycle_is_recoverable("rpiboot-complete") ||
        lifecycle_is_recoverable("mass-storage-active")) {
        fail("lifecycle recovery self-test failed");
    }
    close(bridge.lifecycle_lock_fd);
    unlink(lock_path);
    unlink(state_path);
    rmdir(directory);
    puts("rpiboot-raw-gadget lifecycle self-test: PASS");
}

static void lifecycle_lock(Bridge *b)
{
    struct flock lock = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0,
    };
    char path[4096];
    int length;

    if (!b->lifecycle) {
        return;
    }
    length = snprintf(path, sizeof(path), "%s.lock", b->lifecycle);
    if (length < 0 || (size_t)length >= sizeof(path)) {
        fail("lifecycle lock path is too long");
    }
    b->lifecycle_lock_fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC |
                                O_NOFOLLOW, 0644);
    if (b->lifecycle_lock_fd < 0) {
        fail_errno("open lifecycle lock");
    }
    if (fcntl(b->lifecycle_lock_fd, F_SETLK, &lock) < 0) {
        if (errno == EACCES || errno == EAGAIN) {
            fail("lifecycle is still owned by another process");
        }
        fail_errno("lock lifecycle");
    }
}

static void inject_disconnect(Bridge *b, const char *name, uint32_t captured)
{
    fprintf(stderr,
            "rpiboot-raw-gadget: injected disconnect while receiving %s "
            "after %" PRIu32 " bytes\n",
            name, captured);
    lifecycle_write(b, "rpiboot-failed");
    if (b->fd >= 0) {
        close(b->fd);
        b->fd = -1;
    }
    exit(FAULT_EXIT_STATUS);
}

static void hold_for_external_termination(const char *name,
                                          uint32_t captured)
{
    printf("holding RPIBOOT %s transfer after %" PRIu32
           " bytes for external termination\n", name, captured);
    fflush(stdout);
    for (;;) {
        pause();
    }
}

static void inject_control_timeout(Bridge *b, const char *name)
{
    struct timespec remaining = {
        .tv_sec = b->fault_delay_ms / 1000,
        .tv_nsec = (b->fault_delay_ms % 1000) * 1000000L,
    };

    printf("withholding RPIBOOT %s control response for %" PRIu32 " ms\n",
           name, b->fault_delay_ms);
    fflush(stdout);
    while (nanosleep(&remaining, &remaining) < 0) {
        if (errno != EINTR) {
            fail_errno("wait for injected control timeout");
        }
    }
    lifecycle_write(b, "rpiboot-failed");
    if (b->fd >= 0) {
        close(b->fd);
        b->fd = -1;
    }
    fprintf(stderr,
            "rpiboot-raw-gadget: injected %s control timeout completed\n",
            name);
    exit(FAULT_EXIT_STATUS);
}

static int raw_ioctl(int fd, unsigned long request, void *arg,
                     const char *what)
{
    int ret = ioctl(fd, request, arg);

    if (ret < 0) {
        fail_errno(what);
    }
    return ret;
}

static uint16_t get_le16(__le16 value)
{
    return le16toh(value);
}

static uint32_t get_le32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void put_le32(uint8_t *data, uint32_t value)
{
    data[0] = value;
    data[1] = value >> 8;
    data[2] = value >> 16;
    data[3] = value >> 24;
}

static uint32_t announced_length(const struct usb_ctrlrequest *ctrl)
{
    return get_le16(ctrl->wValue) | ((uint32_t)get_le16(ctrl->wIndex) << 16);
}

static bool record_file_size(Bridge *b, size_t index, uint32_t length)
{
    if (index >= b->request_count || !length ||
        length > MAX_SECOND_STAGE_FILE_SIZE) {
        return false;
    }
    if (b->request_size_known[index]) {
        return b->request_sizes[index] == length;
    }
    if (b->request_total_size + length > MAX_SECOND_STAGE_TOTAL_SIZE) {
        return false;
    }
    b->request_sizes[index] = length;
    b->request_size_known[index] = true;
    b->request_total_size += length;
    return true;
}

static bool file_data_size_matches(const Bridge *b, size_t index,
                                   uint32_t length)
{
    return index < b->request_count && b->request_size_known[index] &&
           b->request_sizes[index] == length;
}

static size_t build_config(uint8_t *data, size_t capacity)
{
    size_t needed = sizeof(config_descriptor) + sizeof(interface_descriptor) +
                    sizeof(bulk_out_descriptor);
    uint8_t *p = data;

    if (capacity < needed) {
        fail("internal configuration descriptor buffer is too small");
    }
    memcpy(p, &config_descriptor, sizeof(config_descriptor));
    p += sizeof(config_descriptor);
    memcpy(p, &interface_descriptor, sizeof(interface_descriptor));
    p += sizeof(interface_descriptor);
    memcpy(p, &bulk_out_descriptor, sizeof(bulk_out_descriptor));
    return needed;
}

static size_t build_string(uint8_t *data, size_t capacity, uint8_t index)
{
    static const char *const strings[] = {
        NULL,
        "Broadcom",
        "BCM2711 Boot",
    };
    const char *text;
    size_t i, len;

    if (capacity < 4) {
        fail("internal string descriptor buffer is too small");
    }
    if (index == 0) {
        data[0] = 4;
        data[1] = USB_DT_STRING;
        data[2] = 0x09;
        data[3] = 0x04;
        return 4;
    }
    if (index >= ARRAY_SIZE(strings) || !strings[index]) {
        return 0;
    }
    text = strings[index];
    len = strlen(text);
    if (len > 126 || 2 + len * 2 > capacity) {
        fail("USB string descriptor is too long");
    }
    data[0] = 2 + len * 2;
    data[1] = USB_DT_STRING;
    for (i = 0; i < len; i++) {
        data[2 + i * 2] = text[i];
        data[3 + i * 2] = 0;
    }
    return data[0];
}

static void ep0_reply(Bridge *b, const struct usb_ctrlrequest *ctrl,
                      const void *data, size_t length)
{
    ControlIO io = { .io = { .ep = 0, .flags = 0 } };
    size_t requested = get_le16(ctrl->wLength);

    if (length > sizeof(io.data)) {
        fail("EP0 response exceeds bridge buffer");
    }
    if (length > requested) {
        length = requested;
    }
    io.io.length = length;
    if (length) {
        memcpy(io.data, data, length);
    }
    raw_ioctl(b->fd, USB_RAW_IOCTL_EP0_WRITE, &io,
              "USB_RAW_IOCTL_EP0_WRITE");
}

static void ep0_ack_out(Bridge *b, const struct usb_ctrlrequest *ctrl)
{
    ControlIO io = { .io = { .ep = 0, .flags = 0 } };

    io.io.length = get_le16(ctrl->wLength);
    if (io.io.length > sizeof(io.data)) {
        fail("unexpected EP0 OUT payload");
    }
    raw_ioctl(b->fd, USB_RAW_IOCTL_EP0_READ, &io,
              "USB_RAW_IOCTL_EP0_READ");
}

static void sanitize_capture_name(const char *name, char *out, size_t out_size)
{
    size_t i;

    if (!name[0] || strstr(name, "..")) {
        fail("unsafe or empty requested filename");
    }
    for (i = 0; name[i] && i + 1 < out_size; i++) {
        char c = name[i];

        out[i] = c == '/' || c == '\\' ? '_' : c;
    }
    if (name[i]) {
        fail("requested filename is too long for capture path");
    }
    out[i] = '\0';
}

static int open_capture(Bridge *b, const char *name)
{
    char safe[RPIBOOT_NAME_SIZE];
    char path[4096];
    int ret;

    sanitize_capture_name(name, safe, sizeof(safe));
    ret = snprintf(path, sizeof(path), "%s/%s", b->capture_dir, safe);
    if (ret < 0 || (size_t)ret >= sizeof(path)) {
        fail("capture path is too long");
    }
    ret = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
               0644);
    if (ret < 0) {
        fail_errno("open capture file");
    }
    return ret;
}

static bool bulk_generation_current(Bridge *b, uint64_t generation)
{
    bool current;

    pthread_mutex_lock(&b->bulk_lock);
    current = b->reset_generation == generation;
    pthread_mutex_unlock(&b->bulk_lock);
    return current;
}

static bool reset_interrupted_bulk(int error)
{
    return error == ESHUTDOWN || error == ECONNRESET;
}

static bool receive_bulk_to_file(Bridge *b, uint32_t length, const char *name,
                                 uint64_t generation)
{
    BulkIO io = { .io = { .flags = 0 } };
    uint32_t remaining = length;
    uint32_t captured = 0;
    bool bootcode = !strcmp(name, "bootcode4.bin");
    bool hold = b->fault == FAULT_BOOTCODE_HOLD;
    bool reset_wait = (b->fault == FAULT_BOOTCODE_RESET ||
                       b->fault == FAULT_BOTH_RESET) &&
                      b->boot_reset_events < b->fault_count;
    bool inject = (b->fault == FAULT_BOOTCODE_DISCONNECT || hold ||
                   reset_wait) && bootcode;
    int out_fd = open_capture(b, name);

    if ((b->fault == FAULT_FILE_DISCONNECT ||
         b->fault == FAULT_FILE_HOLD || b->fault == FAULT_FILE_RESET ||
         b->fault == FAULT_BOTH_RESET) && b->second_stage &&
        b->file_reset_events < b->fault_count &&
        !strcmp(name, b->fault_file)) {
        inject = true;
        hold = b->fault == FAULT_FILE_HOLD;
        reset_wait = b->fault == FAULT_FILE_RESET ||
                     b->fault == FAULT_BOTH_RESET;
    }
    if (inject && b->fault_after >= length) {
        close(out_fd);
        fail("transfer fault offset must be smaller than the transfer");
    }

    while (remaining) {
        size_t want = remaining < sizeof(io.data) ? remaining : sizeof(io.data);
        ssize_t offset = 0;
        int got;

        if (inject && want > b->fault_after - captured) {
            want = b->fault_after - captured;
        }
        io.io.ep = b->bulk_out;
        io.io.length = want;
        got = ioctl(b->fd, USB_RAW_IOCTL_EP_READ, &io);
        if (got < 0) {
            int error = errno;

            close(out_fd);
            if (reset_interrupted_bulk(error) ||
                !bulk_generation_current(b, generation)) {
                printf("RPIBOOT reset interrupted %s after %" PRIu32
                       " bytes\n", name, captured);
                fflush(stdout);
                return false;
            }
            errno = error;
            fail_errno("USB_RAW_IOCTL_EP_READ");
        }
        if (!bulk_generation_current(b, generation)) {
            close(out_fd);
            printf("RPIBOOT reset discarded stale %s data after %" PRIu32
                   " bytes\n", name, captured);
            fflush(stdout);
            return false;
        }
        if (got <= 0 || (size_t)got > want || (uint32_t)got > remaining) {
            close(out_fd);
            fail("invalid bulk transfer length");
        }
        while (offset < got) {
            ssize_t written = write(out_fd, io.data + offset, got - offset);

            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                close(out_fd);
                fail_errno("write capture file");
            }
            offset += written;
        }
        remaining -= got;
        captured += got;
        if (inject && captured == b->fault_after) {
            if (fsync(out_fd) < 0 || close(out_fd) < 0) {
                fail_errno("close partial capture file");
            }
            if (hold) {
                hold_for_external_termination(name, captured);
            }
            if (reset_wait) {
                pthread_mutex_lock(&b->bulk_lock);
                if (bootcode) {
                    b->boot_reset_events++;
                } else {
                    b->file_reset_events++;
                }
                printf("waiting for host USB reset during %s after %" PRIu32
                       " bytes\n", name, captured);
                fflush(stdout);
                while (b->reset_generation == generation) {
                    pthread_cond_wait(&b->bulk_cond, &b->bulk_lock);
                }
                pthread_mutex_unlock(&b->bulk_lock);
                printf("observed host USB reset during %s; transfer will "
                       "restart\n", name);
                fflush(stdout);
                return false;
            }
            inject_disconnect(b, name, captured);
        }
    }
    if (fsync(out_fd) < 0 || close(out_fd) < 0) {
        fail_errno("close capture file");
    }
    printf("captured %s (%" PRIu32 " bytes)\n", name, length);
    fflush(stdout);
    return true;
}

static bool receive_boot_message(Bridge *b, uint64_t generation,
                                 uint32_t *bootcode_length)
{
    BulkIO io = { .io = {
        .ep = b->bulk_out,
        .flags = 0,
        .length = sizeof(BootMessage),
    } };
    int got = ioctl(b->fd, USB_RAW_IOCTL_EP_READ, &io);

    if (got < 0) {
        int error = errno;

        if (reset_interrupted_bulk(error) ||
            !bulk_generation_current(b, generation)) {
            puts("RPIBOOT reset interrupted boot message");
            fflush(stdout);
            return false;
        }
        errno = error;
        fail_errno("USB_RAW_IOCTL_EP_READ boot message");
    }
    if (!bulk_generation_current(b, generation)) {
        puts("RPIBOOT reset discarded stale boot message");
        fflush(stdout);
        return false;
    }
    if (got != sizeof(BootMessage)) {
        fail("short rpiboot boot message");
    }
    *bootcode_length = get_le32(io.data);
    if (!*bootcode_length || *bootcode_length > MAX_BOOTCODE_SIZE) {
        fail("bootcode4.bin length is invalid");
    }
    return true;
}

static void *bulk_worker(void *opaque)
{
    Bridge *b = opaque;

    for (;;) {
        BulkTask task;
        uint32_t length;
        uint64_t generation;
        char name[RPIBOOT_NAME_SIZE];
        bool completed;

        pthread_mutex_lock(&b->bulk_lock);
        while (!b->bulk_pending && !b->bulk_stop) {
            pthread_cond_wait(&b->bulk_cond, &b->bulk_lock);
        }
        if (b->bulk_stop) {
            pthread_mutex_unlock(&b->bulk_lock);
            return NULL;
        }
        task = b->bulk_task;
        length = b->bulk_length;
        generation = b->bulk_generation;
        memcpy(name, b->bulk_name, sizeof(name));
        pthread_mutex_unlock(&b->bulk_lock);

        if (task == BULK_TASK_BOOT_MESSAGE) {
            completed = receive_boot_message(b, generation, &length);
        } else if (task == BULK_TASK_CAPTURE) {
            completed = receive_bulk_to_file(b, length, name, generation);
        } else {
            fail("invalid RPIBOOT bulk task");
        }

        pthread_mutex_lock(&b->bulk_lock);
        if (completed && b->reset_generation == generation) {
            if (task == BULK_TASK_BOOT_MESSAGE) {
                b->expected_bootcode = length;
                b->state = FIRST_EXPECT_BOOTCODE;
            } else if (!b->second_stage) {
                b->state = FIRST_EXPECT_STATUS;
            } else {
                b->request_index++;
                b->state = b->request_index == b->request_count ?
                           FILE_SEND_DONE : FILE_SEND_SIZE;
            }
        } else if (!completed && b->reset_generation != generation) {
            if (b->second_stage) {
                b->request_index = 0;
                b->state = FILE_SEND_SIZE;
            } else {
                b->expected_bootcode = 0;
                b->state = FIRST_EXPECT_MESSAGE;
            }
        }
        b->bulk_pending = false;
        b->bulk_task = BULK_TASK_NONE;
        pthread_cond_broadcast(&b->bulk_cond);
        pthread_mutex_unlock(&b->bulk_lock);
    }
}

static void queue_bulk_task(Bridge *b, BulkTask task, uint32_t length,
                            const char *name)
{
    int name_length = 0;

    pthread_mutex_lock(&b->bulk_lock);
    if (b->bulk_pending) {
        pthread_mutex_unlock(&b->bulk_lock);
        fail("host queued overlapping RPIBOOT bulk transfers");
    }
    if (name) {
        name_length = snprintf(b->bulk_name, sizeof(b->bulk_name), "%s",
                               name);
        if (name_length < 0 ||
            (size_t)name_length >= sizeof(b->bulk_name)) {
            pthread_mutex_unlock(&b->bulk_lock);
            fail("RPIBOOT bulk task filename is too long");
        }
    } else {
        b->bulk_name[0] = '\0';
    }
    b->bulk_task = task;
    b->bulk_length = length;
    b->bulk_generation = b->reset_generation;
    b->bulk_pending = true;
    pthread_cond_broadcast(&b->bulk_cond);
    pthread_mutex_unlock(&b->bulk_lock);
}

static void wait_for_bulk_completion(Bridge *b)
{
    pthread_mutex_lock(&b->bulk_lock);
    while (b->bulk_pending) {
        pthread_cond_wait(&b->bulk_cond, &b->bulk_lock);
    }
    pthread_mutex_unlock(&b->bulk_lock);
}

static void record_usb_reset(Bridge *b, bool log_event)
{
    pthread_mutex_lock(&b->bulk_lock);
    b->configured = false;
    b->reset_generation++;
    pthread_cond_broadcast(&b->bulk_cond);
    if (log_event) {
        fprintf(stderr,
                "rpiboot-raw-gadget: USB reset generation=%" PRIu64
                " preserved %s protocol state\n",
                b->reset_generation,
                b->second_stage ? "file-server" : "ROM");
        fflush(stderr);
    }
    pthread_mutex_unlock(&b->bulk_lock);
}

static void enable_endpoints(Bridge *b)
{
    uint32_t power = config_descriptor.bMaxPower;
    int error;

    if (b->configured) {
        return;
    }
    if (!b->bulk_thread_started) {
        b->bulk_out = raw_ioctl(b->fd, USB_RAW_IOCTL_EP_ENABLE,
                                &bulk_out_descriptor,
                                "USB_RAW_IOCTL_EP_ENABLE bulk OUT");
    }
    raw_ioctl(b->fd, USB_RAW_IOCTL_VBUS_DRAW, &power,
              "USB_RAW_IOCTL_VBUS_DRAW");
    raw_ioctl(b->fd, USB_RAW_IOCTL_CONFIGURE, NULL,
              "USB_RAW_IOCTL_CONFIGURE");
    pthread_mutex_lock(&b->bulk_lock);
    b->configured = true;
    pthread_cond_broadcast(&b->bulk_cond);
    pthread_mutex_unlock(&b->bulk_lock);
    if (!b->bulk_thread_started) {
        error = pthread_create(&b->bulk_thread, NULL, bulk_worker, b);
        if (error) {
            errno = error;
            fail_errno("start RPIBOOT bulk worker");
        }
        b->bulk_thread_started = true;
    }
}

static bool standard_request(Bridge *b, const struct usb_ctrlrequest *ctrl)
{
    uint8_t data[EP0_MAX_DATA] = { 0 };
    size_t length = 0;
    uint8_t descriptor_type;
    uint8_t descriptor_index;

    switch (ctrl->bRequest) {
    case USB_REQ_GET_DESCRIPTOR:
        descriptor_type = get_le16(ctrl->wValue) >> 8;
        descriptor_index = get_le16(ctrl->wValue);
        switch (descriptor_type) {
        case USB_DT_DEVICE: {
            struct usb_device_descriptor desc = device_descriptor;

            desc.iSerialNumber = b->second_stage ? 4 : 0;
            memcpy(data, &desc, sizeof(desc));
            length = sizeof(desc);
            break;
        }
        case USB_DT_CONFIG:
            length = build_config(data, sizeof(data));
            break;
        case USB_DT_DEVICE_QUALIFIER:
            memcpy(data, &qualifier_descriptor, sizeof(qualifier_descriptor));
            length = sizeof(qualifier_descriptor);
            break;
        case USB_DT_STRING:
            if (descriptor_index == 4 && b->second_stage) {
                static const uint8_t serial[] = {
                    18, USB_DT_STRING, '0', 0, '0', 0, '0', 0, '0', 0,
                    '0', 0, '0', 0, '0', 0, '1', 0,
                };

                memcpy(data, serial, sizeof(serial));
                length = sizeof(serial);
            } else {
                length = build_string(data, sizeof(data), descriptor_index);
            }
            break;
        default:
            return false;
        }
        ep0_reply(b, ctrl, data, length);
        return true;
    case USB_REQ_SET_CONFIGURATION:
        enable_endpoints(b);
        ep0_ack_out(b, ctrl);
        return true;
    case USB_REQ_GET_CONFIGURATION:
        data[0] = b->configured ? 1 : 0;
        ep0_reply(b, ctrl, data, 1);
        return true;
    case USB_REQ_GET_INTERFACE:
        data[0] = 0;
        ep0_reply(b, ctrl, data, 1);
        return true;
    case USB_REQ_SET_INTERFACE:
    case USB_REQ_SET_ADDRESS:
        ep0_ack_out(b, ctrl);
        return true;
    case USB_REQ_GET_STATUS:
        ep0_reply(b, ctrl, data, 2);
        return true;
    case USB_REQ_CLEAR_FEATURE:
    case USB_REQ_SET_FEATURE:
        ep0_ack_out(b, ctrl);
        return true;
    default:
        return false;
    }
}

static void first_stage_vendor(Bridge *b, const struct usb_ctrlrequest *ctrl)
{
    uint32_t length;
    int state;

    if (ctrl->bRequest != 0) {
        fail("unexpected first-stage vendor request");
    }
    wait_for_bulk_completion(b);
    if (ctrl->bRequestType & USB_DIR_IN) {
        uint8_t status[4] = { 0 };

        pthread_mutex_lock(&b->bulk_lock);
        state = b->state;
        pthread_mutex_unlock(&b->bulk_lock);
        if (state != FIRST_EXPECT_STATUS || get_le16(ctrl->wLength) != 4) {
            fail("unexpected first-stage status request");
        }
        if (b->fault == FAULT_ROM_STATUS_TIMEOUT) {
            inject_control_timeout(b, "ROM status");
        }
        if (b->fault == FAULT_ROM_STATUS_STALL) {
            raw_ioctl(b->fd, USB_RAW_IOCTL_EP0_STALL, NULL,
                      "USB_RAW_IOCTL_EP0_STALL injected ROM status fault");
            fprintf(stderr,
                    "rpiboot-raw-gadget: injected ROM status endpoint stall\n");
            lifecycle_write(b, "rpiboot-failed");
            close(b->fd);
            b->fd = -1;
            exit(FAULT_EXIT_STATUS);
        }
        ep0_reply(b, ctrl, status, sizeof(status));
        b->finished = true;
        return;
    }

    length = announced_length(ctrl);
    ep0_ack_out(b, ctrl);
    pthread_mutex_lock(&b->bulk_lock);
    state = b->state;
    if (state == FIRST_EXPECT_MESSAGE) {
        if (length != sizeof(BootMessage)) {
            pthread_mutex_unlock(&b->bulk_lock);
            fail("rpiboot boot message has the wrong length");
        }
    } else if (state == FIRST_EXPECT_BOOTCODE) {
        if (length != b->expected_bootcode) {
            pthread_mutex_unlock(&b->bulk_lock);
            fail("bootcode4.bin length differs from boot message");
        }
    } else {
        pthread_mutex_unlock(&b->bulk_lock);
        fail("unexpected first-stage bulk announcement");
    }
    pthread_mutex_unlock(&b->bulk_lock);
    queue_bulk_task(b,
                    state == FIRST_EXPECT_MESSAGE ?
                    BULK_TASK_BOOT_MESSAGE : BULK_TASK_CAPTURE,
                    length,
                    state == FIRST_EXPECT_MESSAGE ? NULL : "bootcode4.bin");
}

static void send_file_message(Bridge *b, const struct usb_ctrlrequest *ctrl,
                              uint32_t command, const char *name)
{
    FileMessage message = { 0 };

    put_le32((uint8_t *)&message.command, command);
    if (snprintf(message.name, sizeof(message.name), "%s", name) < 0 ||
        strlen(name) >= sizeof(message.name)) {
        fail("requested filename is too long");
    }
    ep0_reply(b, ctrl, &message, sizeof(message));
}

static void file_stage_vendor(Bridge *b, const struct usb_ctrlrequest *ctrl)
{
    uint32_t length;
    const char *name;
    int state;

    if (ctrl->bRequest != 0) {
        fail("unexpected file-server vendor request");
    }
    wait_for_bulk_completion(b);
    if (ctrl->bRequestType & USB_DIR_IN) {
        if (get_le16(ctrl->wLength) != sizeof(FileMessage)) {
            fail("unexpected file-server message length");
        }
        pthread_mutex_lock(&b->bulk_lock);
        state = b->state;
        if (state == FILE_SEND_DONE) {
            pthread_mutex_unlock(&b->bulk_lock);
            send_file_message(b, ctrl, 2, "done");
            b->finished = true;
            return;
        }
        if (b->request_index >= b->request_count) {
            pthread_mutex_unlock(&b->bulk_lock);
            fail("file-server request index is out of range");
        }
        name = b->requests[b->request_index];
        if (b->fault == FAULT_FILE_REQUEST_TIMEOUT &&
            !strcmp(name, b->fault_file)) {
            pthread_mutex_unlock(&b->bulk_lock);
            inject_control_timeout(b, name);
        }
        if (state == FILE_SEND_SIZE) {
            b->state = FILE_WAIT_SIZE;
        } else if (state == FILE_SEND_READ) {
            b->state = FILE_WAIT_DATA;
        } else {
            pthread_mutex_unlock(&b->bulk_lock);
            fail("rpiboot polled while the bridge awaited host data");
        }
        pthread_mutex_unlock(&b->bulk_lock);
        send_file_message(b, ctrl, state == FILE_SEND_SIZE ? 0 : 1, name);
        return;
    }

    length = announced_length(ctrl);
    ep0_ack_out(b, ctrl);
    pthread_mutex_lock(&b->bulk_lock);
    if (b->request_index >= b->request_count) {
        pthread_mutex_unlock(&b->bulk_lock);
        fail("unexpected host file response");
    }
    name = b->requests[b->request_index];
    state = b->state;
    if (state == FILE_WAIT_SIZE) {
        if (!record_file_size(b, b->request_index, length)) {
            pthread_mutex_unlock(&b->bulk_lock);
            fail("second-stage file size is invalid or inconsistent");
        }
        b->state = FILE_SEND_READ;
        pthread_mutex_unlock(&b->bulk_lock);
    } else if (state == FILE_WAIT_DATA) {
        if (!file_data_size_matches(b, b->request_index, length)) {
            pthread_mutex_unlock(&b->bulk_lock);
            fail("second-stage file data length differs from size response");
        }
        pthread_mutex_unlock(&b->bulk_lock);
        queue_bulk_task(b, BULK_TASK_CAPTURE, length, name);
    } else {
        pthread_mutex_unlock(&b->bulk_lock);
        fail("unexpected host file response state");
    }
}

static void handle_control(Bridge *b, const struct usb_ctrlrequest *ctrl)
{
    if ((ctrl->bRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD) {
        if (!standard_request(b, ctrl)) {
            raw_ioctl(b->fd, USB_RAW_IOCTL_EP0_STALL, NULL,
                      "USB_RAW_IOCTL_EP0_STALL");
        }
        return;
    }
    if ((ctrl->bRequestType & USB_TYPE_MASK) != USB_TYPE_VENDOR) {
        raw_ioctl(b->fd, USB_RAW_IOCTL_EP0_STALL, NULL,
                  "USB_RAW_IOCTL_EP0_STALL");
        return;
    }
    if (b->second_stage) {
        file_stage_vendor(b, ctrl);
    } else {
        first_stage_vendor(b, ctrl);
    }
}

static void run_enumeration(Bridge *b, bool second_stage)
{
    struct usb_raw_init init = { .speed = USB_SPEED_HIGH };
    int driver_length;
    int device_length;
    int error;

    b->fd = open("/dev/raw-gadget", O_RDWR | O_CLOEXEC);
    if (b->fd < 0) {
        fail_errno("open /dev/raw-gadget");
    }
    driver_length = snprintf((char *)init.driver_name,
                             sizeof(init.driver_name), "%s", b->udc_driver);
    device_length = snprintf((char *)init.device_name,
                             sizeof(init.device_name), "%s", b->udc_device);
    if (driver_length < 0 ||
        (size_t)driver_length >= sizeof(init.driver_name) ||
        device_length < 0 ||
        (size_t)device_length >= sizeof(init.device_name)) {
        fail("invalid UDC name");
    }
    raw_ioctl(b->fd, USB_RAW_IOCTL_INIT, &init, "USB_RAW_IOCTL_INIT");
    raw_ioctl(b->fd, USB_RAW_IOCTL_RUN, NULL, "USB_RAW_IOCTL_RUN");

    pthread_mutex_lock(&b->bulk_lock);
    b->bulk_out = -1;
    b->configured = false;
    b->second_stage = second_stage;
    b->finished = false;
    b->bulk_stop = false;
    b->bulk_pending = false;
    b->bulk_task = BULK_TASK_NONE;
    b->bulk_thread_started = false;
    b->request_index = 0;
    b->expected_bootcode = 0;
    if (second_stage) {
        memset(b->request_sizes, 0, sizeof(b->request_sizes));
        memset(b->request_size_known, 0, sizeof(b->request_size_known));
        b->request_total_size = 0;
    }
    b->state = second_stage ? FILE_SEND_SIZE : FIRST_EXPECT_MESSAGE;
    pthread_mutex_unlock(&b->bulk_lock);

    while (!b->finished) {
        ControlEvent event = { .event = {
            .type = USB_RAW_EVENT_INVALID,
            .length = sizeof(event.ctrl),
        } };

        raw_ioctl(b->fd, USB_RAW_IOCTL_EVENT_FETCH, &event,
                  "USB_RAW_IOCTL_EVENT_FETCH");
        switch (event.event.type) {
        case USB_RAW_EVENT_CONNECT:
        case USB_RAW_EVENT_SUSPEND:
        case USB_RAW_EVENT_RESUME:
        case USB_RAW_EVENT_DISCONNECT:
            break;
        case USB_RAW_EVENT_RESET:
            record_usb_reset(b, true);
            break;
        case USB_RAW_EVENT_CONTROL:
            if (event.event.length != sizeof(event.ctrl)) {
                fail("malformed Raw Gadget control event");
            }
            handle_control(b, &event.ctrl);
            break;
        default:
            fail("unknown Raw Gadget event");
        }
    }
    pthread_mutex_lock(&b->bulk_lock);
    b->bulk_stop = true;
    pthread_cond_broadcast(&b->bulk_cond);
    pthread_mutex_unlock(&b->bulk_lock);
    if (b->bulk_thread_started) {
        error = pthread_join(b->bulk_thread, NULL);
        if (error) {
            errno = error;
            fail_errno("join RPIBOOT bulk worker");
        }
        b->bulk_thread_started = false;
    }
    close(b->fd);
    b->fd = -1;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s --capture-dir DIR (--mass-storage | --request FILE ...)\n"
            "          [--lifecycle STATE_FILE]\n"
            "          [--udc-driver dummy_udc] [--udc-device dummy_udc.0]\n"
            "          [--fault bootcode-disconnect|file-disconnect|"
            "bootcode-hold|file-hold|bootcode-reset|file-reset|"
            "reset-reenumerate|rom-status-stall|rom-status-timeout|"
            "file-request-timeout]\n"
            "          [--fault-after BYTES] [--fault-file FILE]\n"
            "          [--fault-count COUNT] [--fault-delay-ms MSEC]\n"
            "       %s --lifecycle STATE_FILE --recover-stale\n"
            "       %s --self-test\n",
            program, program, program);
    exit(EXIT_FAILURE);
}

static uint32_t parse_positive_u32(const char *value, const char *error)
{
    uint64_t parsed = 0;
    size_t offset;

    if (!value[0]) {
        fail(error);
    }
    for (offset = 0; value[offset]; offset++) {
        uint8_t digit;

        if (value[offset] < '0' || value[offset] > '9') {
            fail(error);
        }
        digit = value[offset] - '0';
        if (parsed > (UINT32_MAX - digit) / 10) {
            fail(error);
        }
        parsed = parsed * 10 + digit;
    }
    if (!parsed) {
        fail(error);
    }
    return parsed;
}

static uint32_t parse_fault_after(const char *value)
{
    static const char error[] =
        "--fault-after must be an integer from 1 through 4294967295";

    return parse_positive_u32(value, error);
}

static uint32_t parse_fault_count(const char *value)
{
    static const char error[] =
        "--fault-count must be an integer from 1 through 4294967295";

    return parse_positive_u32(value, error);
}

static uint32_t parse_fault_delay(const char *value)
{
    static const char error[] =
        "--fault-delay-ms must be an integer from 1 through 4294967295";

    return parse_positive_u32(value, error);
}

static FaultPoint parse_fault(const char *value)
{
    if (!strcmp(value, "bootcode-disconnect")) {
        return FAULT_BOOTCODE_DISCONNECT;
    }
    if (!strcmp(value, "file-disconnect")) {
        return FAULT_FILE_DISCONNECT;
    }
    if (!strcmp(value, "bootcode-hold")) {
        return FAULT_BOOTCODE_HOLD;
    }
    if (!strcmp(value, "file-hold")) {
        return FAULT_FILE_HOLD;
    }
    if (!strcmp(value, "bootcode-reset")) {
        return FAULT_BOOTCODE_RESET;
    }
    if (!strcmp(value, "file-reset")) {
        return FAULT_FILE_RESET;
    }
    if (!strcmp(value, "reset-reenumerate")) {
        return FAULT_BOTH_RESET;
    }
    if (!strcmp(value, "rom-status-stall")) {
        return FAULT_ROM_STATUS_STALL;
    }
    if (!strcmp(value, "rom-status-timeout")) {
        return FAULT_ROM_STATUS_TIMEOUT;
    }
    if (!strcmp(value, "file-request-timeout")) {
        return FAULT_FILE_REQUEST_TIMEOUT;
    }
    fail("unknown --fault value");
    return FAULT_NONE;
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        { "capture-dir", required_argument, NULL, 'c' },
        { "lifecycle", required_argument, NULL, 'l' },
        { "request", required_argument, NULL, 'r' },
        { "mass-storage", no_argument, NULL, 'm' },
        { "udc-driver", required_argument, NULL, 'd' },
        { "udc-device", required_argument, NULL, 'u' },
        { "fault", required_argument, NULL, 'f' },
        { "fault-after", required_argument, NULL, 'a' },
        { "fault-file", required_argument, NULL, 'F' },
        { "fault-count", required_argument, NULL, 'N' },
        { "fault-delay-ms", required_argument, NULL, 'D' },
        { "recover-stale", no_argument, NULL, 'R' },
        { "self-test", no_argument, NULL, 's' },
        { "help", no_argument, NULL, 'h' },
        { 0 }
    };
    Bridge bridge = {
        .udc_driver = "dummy_udc",
        .udc_device = "dummy_udc.0",
        .fd = -1,
        .lifecycle_lock_fd = -1,
        .fault_count = 1,
        .fault_delay_ms = 21000,
    };
    int option;
    int thread_error;

    while ((option = getopt_long(argc, argv, "c:l:r:md:u:f:a:F:N:D:Rsh",
                                 options,
                                 NULL)) != -1) {
        switch (option) {
        case 'c':
            bridge.capture_dir = optarg;
            break;
        case 'l':
            bridge.lifecycle = optarg;
            break;
        case 'r':
            if (bridge.request_count == MAX_REQUESTS) {
                fail("too many requested files");
            }
            bridge.requests[bridge.request_count++] = optarg;
            break;
        case 'm':
            bridge.mass_storage = true;
            break;
        case 'd':
            bridge.udc_driver = optarg;
            bridge.udc_option = true;
            break;
        case 'u':
            bridge.udc_device = optarg;
            bridge.udc_option = true;
            break;
        case 'f':
            bridge.fault = parse_fault(optarg);
            break;
        case 'a':
            bridge.fault_after = parse_fault_after(optarg);
            break;
        case 'F':
            bridge.fault_file = optarg;
            break;
        case 'N':
            bridge.fault_count = parse_fault_count(optarg);
            bridge.fault_count_set = true;
            break;
        case 'D':
            bridge.fault_delay_ms = parse_fault_delay(optarg);
            bridge.fault_delay_set = true;
            break;
        case 'R':
            bridge.recover_stale = true;
            break;
        case 's':
            bridge.self_test = true;
            break;
        default:
            usage(argv[0]);
        }
    }
    if (bridge.self_test) {
        if (bridge.recover_stale || bridge.capture_dir || bridge.lifecycle ||
            bridge.request_count || bridge.mass_storage ||
            bridge.fault != FAULT_NONE || bridge.fault_after ||
            bridge.fault_file || bridge.fault_count_set ||
            bridge.fault_delay_set ||
            bridge.udc_option || optind != argc) {
            fail("--self-test cannot be combined with transport options");
        }
        lifecycle_self_test();
        return EXIT_SUCCESS;
    }
    if (bridge.recover_stale) {
        if (!bridge.lifecycle || bridge.capture_dir || bridge.request_count ||
            bridge.mass_storage || bridge.fault != FAULT_NONE ||
            bridge.fault_after || bridge.fault_file ||
            bridge.fault_count_set || bridge.fault_delay_set ||
            bridge.udc_option || optind != argc) {
            fail("--recover-stale requires only --lifecycle STATE_FILE");
        }
        lifecycle_lock(&bridge);
        lifecycle_recover_stale(&bridge);
        puts("stale RPIBOOT transfer recovered as rpiboot-failed");
        return EXIT_SUCCESS;
    }
    if (bridge.mass_storage && bridge.request_count) {
        fail("--mass-storage cannot be combined with --request");
    }
    if (bridge.mass_storage) {
        bridge.requests[bridge.request_count++] = "config.txt";
        bridge.requests[bridge.request_count++] = "boot.img";
    }
    if ((bridge.fault == FAULT_BOOTCODE_DISCONNECT ||
         bridge.fault == FAULT_FILE_DISCONNECT ||
         bridge.fault == FAULT_BOOTCODE_HOLD ||
         bridge.fault == FAULT_FILE_HOLD ||
         bridge.fault == FAULT_BOOTCODE_RESET ||
         bridge.fault == FAULT_FILE_RESET ||
         bridge.fault == FAULT_BOTH_RESET) && !bridge.fault_after) {
        fail("transfer faults require --fault-after");
    }
    if ((bridge.fault == FAULT_FILE_DISCONNECT ||
         bridge.fault == FAULT_FILE_HOLD ||
         bridge.fault == FAULT_FILE_RESET ||
         bridge.fault == FAULT_BOTH_RESET ||
         bridge.fault == FAULT_FILE_REQUEST_TIMEOUT) && !bridge.fault_file) {
        fail("file transfer faults require --fault-file");
    }
    if (bridge.fault != FAULT_FILE_DISCONNECT &&
        bridge.fault != FAULT_FILE_HOLD &&
        bridge.fault != FAULT_FILE_RESET &&
        bridge.fault != FAULT_BOTH_RESET &&
        bridge.fault != FAULT_FILE_REQUEST_TIMEOUT && bridge.fault_file) {
        fail("--fault-file is only valid with a file transfer fault");
    }
    if ((bridge.fault == FAULT_NONE ||
         bridge.fault == FAULT_ROM_STATUS_STALL ||
         bridge.fault == FAULT_ROM_STATUS_TIMEOUT ||
         bridge.fault == FAULT_FILE_REQUEST_TIMEOUT) && bridge.fault_after) {
        fail("--fault-after is only valid with a transfer fault");
    }
    if (bridge.fault_count_set &&
        bridge.fault != FAULT_BOOTCODE_RESET &&
        bridge.fault != FAULT_FILE_RESET &&
        bridge.fault != FAULT_BOTH_RESET) {
        fail("--fault-count is only valid with a reset fault");
    }
    if (bridge.fault_delay_set &&
        bridge.fault != FAULT_ROM_STATUS_TIMEOUT &&
        bridge.fault != FAULT_FILE_REQUEST_TIMEOUT) {
        fail("--fault-delay-ms is only valid with a control timeout fault");
    }
    if ((bridge.fault == FAULT_ROM_STATUS_TIMEOUT ||
         bridge.fault == FAULT_FILE_REQUEST_TIMEOUT) &&
        bridge.fault_delay_ms <= 20000) {
        fail("--fault-delay-ms must exceed rpiboot's 20000 ms read timeout");
    }
    if (!bridge.capture_dir || !bridge.request_count || optind != argc) {
        usage(argv[0]);
    }
    thread_error = pthread_mutex_init(&bridge.bulk_lock, NULL);
    if (!thread_error) {
        thread_error = pthread_cond_init(&bridge.bulk_cond, NULL);
    }
    if (thread_error) {
        errno = thread_error;
        fail_errno("initialize RPIBOOT bulk synchronization");
    }
    lifecycle_lock(&bridge);
    lifecycle_require_rpiboot_ready(&bridge);
    lifecycle_write(&bridge, "rpiboot-active");
    if (mkdir(bridge.capture_dir, 0755) < 0 && errno != EEXIST) {
        fail_errno("create capture directory");
    }

    printf("waiting for unmodified rpiboot as %04x:%04x ROM stage\n",
           BCM2711_VID, BCM2711_PID);
    fflush(stdout);
    run_enumeration(&bridge, false);

    /* Closing /dev/raw-gadget disconnects the ROM-stage device. */
    usleep(500000);
    printf("bootcode4.bin accepted; re-enumerating as file server\n");
    fflush(stdout);
    run_enumeration(&bridge, true);

    printf("RPIBOOT transport complete: bootcode4.bin and %zu file(s) "
           "captured\n",
           bridge.request_count);
    lifecycle_write(&bridge, "rpiboot-complete");
    pthread_cond_destroy(&bridge.bulk_cond);
    pthread_mutex_destroy(&bridge.bulk_lock);
    return EXIT_SUCCESS;
}
