/*
 * Linux Raw Gadget ACM + USB Mass Storage bridge for a virtual CM4 eMMC.
 *
 * Copyright (c) 2026 QEMU contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The descriptors follow the unchanged Raspberry Pi mass-storage-gadget64
 * second-stage image: Broadcom 0a5c:0104, one writable eMMC mass-storage
 * function followed by CDC ACM.  Unlike the kernel configfs mass-storage
 * function, this userspace BOT target sees every SCSI CDB.  It can therefore
 * fail SYNCHRONIZE CACHE, a WRITE with FUA, or an explicitly selected opcode
 * while leaving the composite device and LUN enumerated.
 */

#include <endian.h>
#include <errno.h>
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
#include <unistd.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define SECTOR_SIZE 512U
#define EP0_MAX_DATA 512U
#define BULK_CHUNK (128U * 1024U)
#define CBW_SIGNATURE 0x43425355U
#define CSW_SIGNATURE 0x53425355U
#define CSW_PASSED 0U
#define CSW_FAILED 1U
#define CSW_PHASE_ERROR 2U
#define CSW_COMMAND_ABORTED 3U /* Internal: do not emit a CSW after reset. */
#define MSC_REQUEST_RESET 0xffU
#define MSC_REQUEST_GET_MAX_LUN 0xfeU
#define CDC_SET_LINE_CODING 0x20U
#define CDC_GET_LINE_CODING 0x21U
#define CDC_SET_CONTROL_LINE_STATE 0x22U
#define CDC_SEND_BREAK 0x23U

#define SCSI_TEST_UNIT_READY 0x00U
#define SCSI_REQUEST_SENSE 0x03U
#define SCSI_INQUIRY 0x12U
#define SCSI_MODE_SENSE_6 0x1aU
#define SCSI_START_STOP 0x1bU
#define SCSI_PREVENT_ALLOW 0x1eU
#define SCSI_READ_FORMAT_CAPACITIES 0x23U
#define SCSI_READ_CAPACITY_10 0x25U
#define SCSI_READ_10 0x28U
#define SCSI_WRITE_10 0x2aU
#define SCSI_VERIFY_10 0x2fU
#define SCSI_SYNCHRONIZE_CACHE_10 0x35U
#define SCSI_MODE_SENSE_10 0x5aU
#define SCSI_READ_16 0x88U
#define SCSI_WRITE_16 0x8aU
#define SCSI_SYNCHRONIZE_CACHE_16 0x91U
#define SCSI_SERVICE_ACTION_IN_16 0x9eU
#define SCSI_REPORT_LUNS 0xa0U
#define SCSI_READ_CAPACITY_16 0x10U

#define SENSE_NONE 0x00U
#define SENSE_MEDIUM_ERROR 0x03U
#define SENSE_ILLEGAL_REQUEST 0x05U
#define SENSE_HARDWARE_ERROR 0x04U
#define ASC_INVALID_COMMAND 0x20U
#define ASC_LBA_OUT_OF_RANGE 0x21U
#define ASC_INVALID_FIELD 0x24U
#define ASC_INTERNAL_TARGET_FAILURE 0x44U
#define ASC_WRITE_ERROR 0x0cU

typedef enum FaultKind {
    FAULT_NONE,
    FAULT_SYNCHRONIZE_CACHE,
    FAULT_WRITE_FUA,
    FAULT_OPCODE,
    FAULT_BOT_PHASE,
    FAULT_BOT_TIMEOUT,
    FAULT_BOT_DATA_RESET,
    FAULT_BOT_WRITE_RESET,
} FaultKind;

typedef enum ResetKind {
    RESET_NONE,
    RESET_BOT_CLASS,
    RESET_USB_BUS,
} ResetKind;

typedef enum BotDirection {
    BOT_DATA_NONE,
    BOT_DATA_IN,
    BOT_DATA_OUT,
    BOT_DATA_UNKNOWN,
} BotDirection;

typedef struct BotIntent {
    BotDirection direction;
    uint32_t length;
} BotIntent;

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

typedef struct AcmIO {
    struct usb_raw_ep_io io;
    uint8_t data[4096];
} AcmIO;

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

typedef struct MsdDevice {
    const char *image_path;
    const char *lifecycle;
    const char *udc_driver;
    const char *udc_device;
    int raw_fd;
    int image_fd;
    int lifecycle_lock_fd;
    int bulk_in;
    int bulk_out;
    int acm_notify;
    int acm_in;
    int acm_out;
    uint64_t image_size;
    bool configured;
    bool reset_pending;
    bool transport_fault_waiting;
    bool invalid_cbw_waiting;
    bool invalid_cbw_reset_seen;
    bool invalid_cbw_clearing;
    bool bulk_in_halt_waiting;
    bool bot_running;
    bool bot_thread_started;
    bool acm_thread_started;
    bool foreground_owner;
    bool owner_stopping;
    bool command_active;
    bool verbose;
    pthread_t bot_thread;
    pthread_t acm_thread;
    pthread_t owner_thread;
    pthread_mutex_t reset_lock;
    pthread_cond_t reset_cond;
    uint64_t reset_generation;
    ResetKind reset_kind;
    uint8_t sense_key;
    uint8_t sense_asc;
    uint8_t sense_ascq;
    FaultKind fault;
    uint8_t fault_opcode;
    uint64_t fault_after;
    uint32_t fault_bytes;
    uint64_t fault_matches;
    uint64_t fault_limit;
    bool fault_active;
    bool fault_reported;
    uint64_t transport_faults_fired;
    uint64_t command_count;
    uint8_t line_coding[7];
    uint16_t control_line_state;
} MsdDevice;

static const struct usb_device_descriptor device_descriptor = {
    .bLength = USB_DT_DEVICE_SIZE,
    .bDescriptorType = USB_DT_DEVICE,
    .bcdUSB = __cpu_to_le16(0x0200),
    .bDeviceClass = USB_CLASS_PER_INTERFACE,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = 64,
    .idVendor = __cpu_to_le16(0x0a5c),
    .idProduct = __cpu_to_le16(0x0104),
    .bcdDevice = __cpu_to_le16(0x0100),
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

static const struct usb_qualifier_descriptor qualifier_descriptor = {
    .bLength = sizeof(struct usb_qualifier_descriptor),
    .bDescriptorType = USB_DT_DEVICE_QUALIFIER,
    .bcdUSB = __cpu_to_le16(0x0200),
    .bDeviceClass = USB_CLASS_PER_INTERFACE,
    .bMaxPacketSize0 = 64,
    .bNumConfigurations = 1,
};

static const struct usb_config_descriptor config_descriptor = {
    .bLength = USB_DT_CONFIG_SIZE,
    .bDescriptorType = USB_DT_CONFIG,
    .wTotalLength = __cpu_to_le16(98),
    .bNumInterfaces = 3,
    .bConfigurationValue = 1,
    .iConfiguration = 4,
    .bmAttributes = USB_CONFIG_ATT_ONE,
    .bMaxPower = 250,
};

static const struct usb_interface_descriptor msd_interface_descriptor = {
    .bLength = USB_DT_INTERFACE_SIZE,
    .bDescriptorType = USB_DT_INTERFACE,
    .bInterfaceNumber = 0,
    .bAlternateSetting = 0,
    .bNumEndpoints = 2,
    .bInterfaceClass = USB_CLASS_MASS_STORAGE,
    .bInterfaceSubClass = 0x06,
    .bInterfaceProtocol = 0x50,
    .iInterface = 0,
};

static struct usb_endpoint_descriptor bulk_in_descriptor = {
    .bLength = USB_DT_ENDPOINT_SIZE,
    .bDescriptorType = USB_DT_ENDPOINT,
    .bEndpointAddress = USB_DIR_IN | 1,
    .bmAttributes = USB_ENDPOINT_XFER_BULK,
    .wMaxPacketSize = __cpu_to_le16(512),
};

static struct usb_endpoint_descriptor bulk_out_descriptor = {
    .bLength = USB_DT_ENDPOINT_SIZE,
    .bDescriptorType = USB_DT_ENDPOINT,
    .bEndpointAddress = USB_DIR_OUT | 1,
    .bmAttributes = USB_ENDPOINT_XFER_BULK,
    .wMaxPacketSize = __cpu_to_le16(512),
};

typedef struct CdcIadDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bFirstInterface;
    uint8_t bInterfaceCount;
    uint8_t bFunctionClass;
    uint8_t bFunctionSubClass;
    uint8_t bFunctionProtocol;
    uint8_t iFunction;
} __attribute__((packed)) CdcIadDescriptor;

typedef struct CdcFunctionalDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bDescriptorSubType;
    uint8_t data[2];
} __attribute__((packed)) CdcFunctionalDescriptor;

static const CdcIadDescriptor acm_iad_descriptor = {
    .bLength = sizeof(CdcIadDescriptor),
    .bDescriptorType = USB_DT_INTERFACE_ASSOCIATION,
    .bFirstInterface = 1,
    .bInterfaceCount = 2,
    .bFunctionClass = USB_CLASS_COMM,
    .bFunctionSubClass = 2,
    .bFunctionProtocol = 1,
};

static const struct usb_interface_descriptor acm_control_descriptor = {
    .bLength = USB_DT_INTERFACE_SIZE,
    .bDescriptorType = USB_DT_INTERFACE,
    .bInterfaceNumber = 1,
    .bNumEndpoints = 1,
    .bInterfaceClass = USB_CLASS_COMM,
    .bInterfaceSubClass = 2,
    .bInterfaceProtocol = 1,
};

static const CdcFunctionalDescriptor acm_header_descriptor = {
    .bLength = 5,
    .bDescriptorType = USB_DT_CS_INTERFACE,
    .bDescriptorSubType = 0,
    .data = { 0x10, 0x01 },
};

static const CdcFunctionalDescriptor acm_call_management_descriptor = {
    .bLength = 5,
    .bDescriptorType = USB_DT_CS_INTERFACE,
    .bDescriptorSubType = 1,
    .data = { 0x00, 0x02 },
};

static const uint8_t acm_functional_descriptor[] = {
    4, USB_DT_CS_INTERFACE, 2, 2,
};

static const CdcFunctionalDescriptor acm_union_descriptor = {
    .bLength = 5,
    .bDescriptorType = USB_DT_CS_INTERFACE,
    .bDescriptorSubType = 6,
    .data = { 0x01, 0x02 },
};

static struct usb_endpoint_descriptor acm_notify_descriptor = {
    .bLength = USB_DT_ENDPOINT_SIZE,
    .bDescriptorType = USB_DT_ENDPOINT,
    .bEndpointAddress = USB_DIR_IN | 2,
    .bmAttributes = USB_ENDPOINT_XFER_INT,
    .wMaxPacketSize = __cpu_to_le16(16),
    .bInterval = 9,
};

static const struct usb_interface_descriptor acm_data_descriptor = {
    .bLength = USB_DT_INTERFACE_SIZE,
    .bDescriptorType = USB_DT_INTERFACE,
    .bInterfaceNumber = 2,
    .bNumEndpoints = 2,
    .bInterfaceClass = USB_CLASS_CDC_DATA,
};

static struct usb_endpoint_descriptor acm_out_descriptor = {
    .bLength = USB_DT_ENDPOINT_SIZE,
    .bDescriptorType = USB_DT_ENDPOINT,
    .bEndpointAddress = USB_DIR_OUT | 2,
    .bmAttributes = USB_ENDPOINT_XFER_BULK,
    .wMaxPacketSize = __cpu_to_le16(512),
};

static struct usb_endpoint_descriptor acm_in_descriptor = {
    .bLength = USB_DT_ENDPOINT_SIZE,
    .bDescriptorType = USB_DT_ENDPOINT,
    .bEndpointAddress = USB_DIR_IN | 3,
    .bmAttributes = USB_ENDPOINT_XFER_BULK,
    .wMaxPacketSize = __cpu_to_le16(512),
};

static void *bot_worker(void *opaque);
static void *acm_worker(void *opaque);
static void *owner_worker(void *opaque);
static void set_sense(MsdDevice *d, uint8_t key, uint8_t asc, uint8_t ascq);
static void set_halt(MsdDevice *d, int endpoint);
static void send_csw(MsdDevice *d, const CommandBlockWrapper *cbw,
                     uint32_t residue, uint8_t status);

static __attribute__((noreturn)) void fail_errno(const char *what)
{
    fprintf(stderr, "cm4-msd-raw-gadget: %s: %s\n", what, strerror(errno));
    exit(EXIT_FAILURE);
}

static __attribute__((noreturn)) void fail(const char *what)
{
    fprintf(stderr, "cm4-msd-raw-gadget: %s\n", what);
    exit(EXIT_FAILURE);
}

static void lifecycle_write(MsdDevice *d, const char *state)
{
    struct stat metadata;
    char temporary[4096];
    char contents[128];
    int content_length;
    int path_length;
    int fd;
    ssize_t offset = 0;

    if (!d->lifecycle) {
        return;
    }
    if (stat(d->lifecycle, &metadata) < 0) {
        fail_errno("stat lifecycle state");
    }
    path_length = snprintf(temporary, sizeof(temporary), "%s.tmp.%ld",
                           d->lifecycle, (long)getpid());
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
    if (rename(temporary, d->lifecycle) < 0) {
        unlink(temporary);
        fail_errno("publish lifecycle state");
    }
}

static void lifecycle_lock(MsdDevice *d)
{
    struct flock lock = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0,
    };
    char path[4096];
    int length;

    if (!d->lifecycle) {
        return;
    }
    length = snprintf(path, sizeof(path), "%s.lock", d->lifecycle);
    if (length < 0 || (size_t)length >= sizeof(path)) {
        fail("lifecycle lock path is too long");
    }
    d->lifecycle_lock_fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC |
                                O_NOFOLLOW, 0644);
    if (d->lifecycle_lock_fd < 0) {
        fail_errno("open lifecycle lock");
    }
    if (fcntl(d->lifecycle_lock_fd, F_SETLK, &lock) < 0) {
        if (errno == EACCES || errno == EAGAIN) {
            fail("CM4 provision lifecycle is owned by another process");
        }
        fail_errno("lock lifecycle");
    }
}

static void lifecycle_claim_mass_storage(MsdDevice *d)
{
    char state[128];
    ssize_t length;
    int fd;

    if (!d->lifecycle) {
        return;
    }
    fd = open(d->lifecycle, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        fail_errno("open lifecycle state");
    }
    length = read(fd, state, sizeof(state) - 1);
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
    if (strcmp(state, "rpiboot-complete") && strcmp(state, "flash-failed")) {
        fprintf(stderr,
                "cm4-msd-raw-gadget: lifecycle state '%s' cannot release "
                "the eMMC to mass storage\n",
                state);
        exit(EXIT_FAILURE);
    }
    lifecycle_write(d, "mass-storage-active");
}

static int raw_ioctl(int fd, unsigned long request, void *arg,
                     const char *what)
{
    int ret;

    do {
        ret = ioctl(fd, request, arg);
    } while (ret < 0 && errno == EINTR);
    if (ret < 0) {
        fail_errno(what);
    }
    return ret;
}

static uint16_t get_le16(__le16 value)
{
    return le16toh(value);
}

static uint16_t get_be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t get_be64(const uint8_t *p)
{
    return ((uint64_t)get_be32(p) << 32) | get_be32(p + 4);
}

static void put_be16(uint8_t *p, uint16_t value)
{
    p[0] = value >> 8;
    p[1] = value;
}

static void put_be24(uint8_t *p, uint32_t value)
{
    p[0] = value >> 16;
    p[1] = value >> 8;
    p[2] = value;
}

static void put_be32(uint8_t *p, uint32_t value)
{
    p[0] = value >> 24;
    p[1] = value >> 16;
    p[2] = value >> 8;
    p[3] = value;
}

static void put_be64(uint8_t *p, uint64_t value)
{
    put_be32(p, value >> 32);
    put_be32(p + 4, value);
}

static size_t build_config(uint8_t *data, size_t capacity)
{
    size_t needed = get_le16(config_descriptor.wTotalLength);
    uint8_t *p = data;

    if (capacity < needed) {
        fail("configuration descriptor buffer is too small");
    }
#define APPEND_DESCRIPTOR(descriptor) do {              \
        memcpy(p, &(descriptor), sizeof(descriptor));   \
        p += sizeof(descriptor);                        \
    } while (0)
#define APPEND_ENDPOINT(descriptor) do {                 \
        memcpy(p, &(descriptor), USB_DT_ENDPOINT_SIZE); \
        p += USB_DT_ENDPOINT_SIZE;                       \
    } while (0)
    memcpy(p, &config_descriptor, sizeof(config_descriptor));
    p += sizeof(config_descriptor);
    APPEND_DESCRIPTOR(msd_interface_descriptor);
    APPEND_ENDPOINT(bulk_in_descriptor);
    APPEND_ENDPOINT(bulk_out_descriptor);
    APPEND_DESCRIPTOR(acm_iad_descriptor);
    APPEND_DESCRIPTOR(acm_control_descriptor);
    APPEND_DESCRIPTOR(acm_header_descriptor);
    APPEND_DESCRIPTOR(acm_call_management_descriptor);
    memcpy(p, acm_functional_descriptor, sizeof(acm_functional_descriptor));
    p += sizeof(acm_functional_descriptor);
    APPEND_DESCRIPTOR(acm_union_descriptor);
    APPEND_ENDPOINT(acm_notify_descriptor);
    APPEND_DESCRIPTOR(acm_data_descriptor);
    APPEND_ENDPOINT(acm_out_descriptor);
    APPEND_ENDPOINT(acm_in_descriptor);
#undef APPEND_ENDPOINT
#undef APPEND_DESCRIPTOR
    if ((size_t)(p - data) != needed) {
        fail("configuration descriptor length mismatch");
    }
    return needed;
}

static size_t build_string(uint8_t *data, size_t capacity, uint8_t index)
{
    static const char *const strings[] = {
        NULL,
        "Raspberry Pi",
        "Raspberry Pi multi-function USB device",
        "51554d5552504934",
        "Config 1: ACM+MSD gadget",
    };
    const char *text;
    size_t i;
    size_t length;

    if (index == 0) {
        if (capacity < 4) {
            fail("string descriptor buffer is too small");
        }
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
    length = strlen(text);
    if (length > 126 || 2 + length * 2 > capacity) {
        fail("USB string descriptor is too long");
    }
    data[0] = 2 + length * 2;
    data[1] = USB_DT_STRING;
    for (i = 0; i < length; i++) {
        data[2 + i * 2] = text[i];
        data[3 + i * 2] = 0;
    }
    return data[0];
}

static void ep0_reply(MsdDevice *d, const struct usb_ctrlrequest *ctrl,
                      const void *data, size_t length)
{
    ControlIO io = { .io = { .ep = 0, .flags = 0 } };
    size_t requested = get_le16(ctrl->wLength);

    if (length > sizeof(io.data)) {
        fail("EP0 response exceeds buffer");
    }
    if (length > requested) {
        length = requested;
    }
    io.io.length = length;
    if (length) {
        memcpy(io.data, data, length);
    }
    raw_ioctl(d->raw_fd, USB_RAW_IOCTL_EP0_WRITE, &io,
              "USB_RAW_IOCTL_EP0_WRITE");
}

static void ep0_ack_out(MsdDevice *d, const struct usb_ctrlrequest *ctrl)
{
    ControlIO io = { .io = { .ep = 0, .flags = 0 } };

    io.io.length = get_le16(ctrl->wLength);
    if (io.io.length > sizeof(io.data)) {
        fail("unexpected EP0 OUT payload");
    }
    raw_ioctl(d->raw_fd, USB_RAW_IOCTL_EP0_READ, &io,
              "USB_RAW_IOCTL_EP0_READ");
}

static void ep0_read_exact(MsdDevice *d, const struct usb_ctrlrequest *ctrl,
                           void *data, size_t length)
{
    ControlIO io = { .io = { .ep = 0, .flags = 0 } };
    int got;

    if (get_le16(ctrl->wLength) != length || length > sizeof(io.data)) {
        fail("unexpected EP0 OUT payload length");
    }
    io.io.length = length;
    got = raw_ioctl(d->raw_fd, USB_RAW_IOCTL_EP0_READ, &io,
                    "USB_RAW_IOCTL_EP0_READ data");
    if ((size_t)got != length) {
        fail("short EP0 OUT payload");
    }
    memcpy(data, io.data, length);
}

static void enable_endpoints(MsdDevice *d)
{
    uint32_t power = config_descriptor.bMaxPower;
    bool publish_bus_reset = false;

    if (!d->bot_thread_started) {
        d->bulk_in = raw_ioctl(d->raw_fd, USB_RAW_IOCTL_EP_ENABLE,
                               &bulk_in_descriptor,
                               "USB_RAW_IOCTL_EP_ENABLE bulk IN");
        d->bulk_out = raw_ioctl(d->raw_fd, USB_RAW_IOCTL_EP_ENABLE,
                                &bulk_out_descriptor,
                                "USB_RAW_IOCTL_EP_ENABLE bulk OUT");
        d->acm_notify = raw_ioctl(d->raw_fd, USB_RAW_IOCTL_EP_ENABLE,
                                  &acm_notify_descriptor,
                                  "USB_RAW_IOCTL_EP_ENABLE ACM notify");
        d->acm_out = raw_ioctl(d->raw_fd, USB_RAW_IOCTL_EP_ENABLE,
                               &acm_out_descriptor,
                               "USB_RAW_IOCTL_EP_ENABLE ACM OUT");
        d->acm_in = raw_ioctl(d->raw_fd, USB_RAW_IOCTL_EP_ENABLE,
                              &acm_in_descriptor,
                              "USB_RAW_IOCTL_EP_ENABLE ACM IN");
    }
    raw_ioctl(d->raw_fd, USB_RAW_IOCTL_VBUS_DRAW, &power,
              "USB_RAW_IOCTL_VBUS_DRAW");
    raw_ioctl(d->raw_fd, USB_RAW_IOCTL_CONFIGURE, NULL,
              "USB_RAW_IOCTL_CONFIGURE");
    pthread_mutex_lock(&d->reset_lock);
    d->configured = true;
    if (d->reset_pending) {
        d->reset_pending = false;
        d->reset_generation++;
        d->reset_kind = RESET_USB_BUS;
        set_sense(d, SENSE_NONE, 0, 0);
        publish_bus_reset = true;
    }
    pthread_cond_broadcast(&d->reset_cond);
    pthread_mutex_unlock(&d->reset_lock);
    if (publish_bus_reset) {
        fprintf(stderr,
                "cm4-msd-raw-gadget: USB bus reset completed host "
                "reconfiguration\n");
        fflush(stderr);
    }
    if (!d->bot_thread_started) {
        int error = pthread_create(&d->bot_thread, NULL, bot_worker, d);

        if (error) {
            errno = error;
            fail_errno("start BOT worker");
        }
        d->bot_thread_started = true;
        error = pthread_create(&d->acm_thread, NULL, acm_worker, d);
        if (error) {
            errno = error;
            fail_errno("start ACM worker");
        }
        d->acm_thread_started = true;
    }
}

/*
 * Keep the production CDC ACM data pair live without inventing a second
 * private transport.  Bytes written by a host terminal are echoed back; the
 * control interface retains the negotiated line coding and modem state.
 * This makes the ACM function testable while the modeled CM4 boot console
 * remains available through QEMU's normal serial backend.
 */
static void *acm_worker(void *opaque)
{
    MsdDevice *d = opaque;

    for (;;) {
        AcmIO io = { .io = {
            .ep = d->acm_out,
            .flags = 0,
            .length = sizeof(io.data),
        } };
        int got = ioctl(d->raw_fd, USB_RAW_IOCTL_EP_READ, &io);

        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            pthread_mutex_lock(&d->reset_lock);
            while (!d->configured) {
                pthread_cond_wait(&d->reset_cond, &d->reset_lock);
            }
            pthread_mutex_unlock(&d->reset_lock);
            continue;
        }
        if (!got) {
            continue;
        }
        io.io.ep = d->acm_in;
        io.io.length = got;
        for (;;) {
            int written = ioctl(d->raw_fd, USB_RAW_IOCTL_EP_WRITE, &io);

            if (written == got) {
                break;
            }
            if (written < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
    }
    return NULL;
}

static int bulk_read(MsdDevice *d, void *buffer, size_t length)
{
    struct usb_raw_ep_io *io;
    uint8_t *storage;
    size_t allocation = sizeof(*io) + length;
    int got;

    io = calloc(1, allocation);
    if (!io) {
        fail_errno("allocate bulk read");
    }
    io->ep = d->bulk_out;
    io->length = length;
    for (;;) {
        got = ioctl(d->raw_fd, USB_RAW_IOCTL_EP_READ, io);
        if (got >= 0) {
            pthread_mutex_lock(&d->reset_lock);
            d->invalid_cbw_clearing = false;
            pthread_mutex_unlock(&d->reset_lock);
            break;
        }
        pthread_mutex_lock(&d->reset_lock);
        if (d->invalid_cbw_clearing &&
            (errno == EBUSY || errno == EPIPE || errno == EAGAIN)) {
            pthread_mutex_unlock(&d->reset_lock);
            usleep(1000);
            continue;
        }
        pthread_mutex_unlock(&d->reset_lock);
        if (errno != ESHUTDOWN && errno != ECONNRESET) {
            fail_errno("USB_RAW_IOCTL_EP_READ");
        }
        pthread_mutex_lock(&d->reset_lock);
        while (!d->configured) {
            pthread_cond_wait(&d->reset_cond, &d->reset_lock);
        }
        pthread_mutex_unlock(&d->reset_lock);
    }
    storage = (uint8_t *)io + sizeof(*io);
    if (got > 0) {
        memcpy(buffer, storage, got);
    }
    free(io);
    return got;
}

static void bulk_read_exact(MsdDevice *d, void *buffer, size_t length)
{
    uint8_t *p = buffer;
    size_t done = 0;

    while (done < length) {
        int got = bulk_read(d, p + done, length - done);

        if (got <= 0 || (size_t)got > length - done) {
            fail("invalid or short bulk OUT transfer");
        }
        done += got;
    }
}

static void bulk_read_prefix(MsdDevice *d, void *buffer, size_t prefix,
                             size_t transfer_length)
{
    BulkIO io;
    uint8_t *retained = buffer;
    size_t consumed = 0;
    size_t copied = 0;

    while (copied < prefix) {
        size_t request = transfer_length - consumed;
        size_t keep;
        int got;

        if (request > sizeof(io.data)) {
            request = sizeof(io.data);
        }
        if (!request) {
            fail("bulk OUT transfer ended before durable prefix");
        }
        /*
         * The host may submit one max-packet or larger URB even when the
         * durable cutoff is smaller.  Give Raw Gadget room for that complete
         * transfer, then retain only the configured prefix.
         */
        got = bulk_read(d, io.data, request);
        if (got <= 0 || (size_t)got > request) {
            fail("invalid bulk OUT transfer before durable prefix");
        }
        keep = (size_t)got < prefix - copied ?
               (size_t)got : prefix - copied;
        memcpy(retained + copied, io.data, keep);
        copied += keep;
        consumed += got;
    }
}

static void durable_pwrite_exact(int fd, const void *buffer, size_t length,
                                 uint64_t offset, const char *context)
{
    const uint8_t *bytes = buffer;
    size_t done = 0;

    while (done < length) {
        ssize_t written = pwrite(fd, bytes + done, length - done,
                                 offset + done);

        if (written < 0) {
            fail_errno(context);
        }
        if (!written) {
            fail("zero-length eMMC backend write");
        }
        done += written;
    }
    if (fdatasync(fd) < 0) {
        fail_errno("sync eMMC backend for reset fault");
    }
}

static void bulk_write(MsdDevice *d, const void *buffer, size_t length)
{
    struct usb_raw_ep_io *io;
    uint8_t *storage;
    size_t allocation = sizeof(*io) + length;
    int written;

    io = calloc(1, allocation);
    if (!io) {
        fail_errno("allocate bulk write");
    }
    io->ep = d->bulk_in;
    io->length = length;
    storage = (uint8_t *)io + sizeof(*io);
    if (length) {
        memcpy(storage, buffer, length);
    }
    for (;;) {
        written = ioctl(d->raw_fd, USB_RAW_IOCTL_EP_WRITE, io);
        if (written >= 0) {
            pthread_mutex_lock(&d->reset_lock);
            d->bulk_in_halt_waiting = false;
            pthread_mutex_unlock(&d->reset_lock);
            break;
        }
        pthread_mutex_lock(&d->reset_lock);
        if (d->bulk_in_halt_waiting &&
            (errno == EBUSY || errno == EPIPE || errno == EAGAIN)) {
            pthread_mutex_unlock(&d->reset_lock);
            usleep(1000);
            continue;
        }
        pthread_mutex_unlock(&d->reset_lock);
        if (errno != ESHUTDOWN && errno != ECONNRESET) {
            fail_errno("USB_RAW_IOCTL_EP_WRITE");
        }
        pthread_mutex_lock(&d->reset_lock);
        while (!d->configured) {
            pthread_cond_wait(&d->reset_cond, &d->reset_lock);
        }
        pthread_mutex_unlock(&d->reset_lock);
    }
    free(io);
    if (written != (int)length) {
        fail("short bulk IN transfer");
    }
}

static void stall_bulk_in_before_csw(MsdDevice *d)
{
    /*
     * Raw Gadget completes EP_WRITE before dummy_hcd has retired the host
     * URB.  Give the short data packet one scheduler turn before halting the
     * following status phase, otherwise the halt can retroactively fail that
     * data URB instead of the host's next Bulk-In request.
     */
    usleep(1000);
    pthread_mutex_lock(&d->reset_lock);
    d->bulk_in_halt_waiting = true;
    pthread_mutex_unlock(&d->reset_lock);
    set_halt(d, d->bulk_in);
}

static void send_short_terminated(MsdDevice *d, const void *data,
                                  uint32_t length, uint32_t requested)
{
    if (length) {
        bulk_write(d, data, length);
    }
    if (length < requested && length % 512 == 0) {
        bulk_write(d, NULL, 0);
    }
}

static void discard_out(MsdDevice *d, uint32_t length)
{
    BulkIO io;
    uint32_t remaining = length;

    while (remaining) {
        size_t chunk = remaining < sizeof(io.data) ?
                       remaining : sizeof(io.data);

        bulk_read_exact(d, io.data, chunk);
        remaining -= chunk;
    }
}

static void set_sense(MsdDevice *d, uint8_t key, uint8_t asc, uint8_t ascq)
{
    d->sense_key = key;
    d->sense_asc = asc;
    d->sense_ascq = ascq;
}

static bool fault_matches(MsdDevice *d, uint8_t opcode, bool fua)
{
    bool matches = false;

    switch (d->fault) {
    case FAULT_SYNCHRONIZE_CACHE:
        matches = opcode == SCSI_SYNCHRONIZE_CACHE_10 ||
                  opcode == SCSI_SYNCHRONIZE_CACHE_16;
        break;
    case FAULT_WRITE_FUA:
        matches = (opcode == SCSI_WRITE_10 || opcode == SCSI_WRITE_16) && fua;
        break;
    case FAULT_OPCODE:
        matches = opcode == d->fault_opcode;
        break;
    case FAULT_BOT_PHASE:
    case FAULT_BOT_TIMEOUT:
    case FAULT_BOT_DATA_RESET:
    case FAULT_BOT_WRITE_RESET:
    case FAULT_NONE:
        break;
    }
    if (!matches) {
        return false;
    }
    d->fault_matches++;
    if (d->fault_matches >= d->fault_after) {
        d->fault_active = true;
    }
    return d->fault_active;
}

static bool transport_fault_matches(MsdDevice *d, uint8_t opcode)
{
    if ((d->fault != FAULT_BOT_PHASE && d->fault != FAULT_BOT_TIMEOUT) ||
        d->transport_faults_fired >= d->fault_limit ||
        opcode != SCSI_TEST_UNIT_READY) {
        return false;
    }
    d->fault_matches++;
    return d->fault_matches >= d->fault_after;
}

static bool data_reset_matches(MsdDevice *d, uint8_t opcode)
{
    bool matches = (d->fault == FAULT_BOT_DATA_RESET &&
                    opcode == SCSI_READ_16) ||
                   (d->fault == FAULT_BOT_WRITE_RESET &&
                    opcode == SCSI_WRITE_16);

    if (!matches || d->transport_faults_fired >= d->fault_limit) {
        return false;
    }
    d->fault_matches++;
    return d->fault_matches >= d->fault_after;
}

static void clear_halt(MsdDevice *d, int endpoint)
{
    unsigned int attempt;

    if (endpoint < 0) {
        return;
    }
    for (attempt = 0; attempt < 100; attempt++) {
        if (ioctl(d->raw_fd, USB_RAW_IOCTL_EP_CLEAR_HALT, endpoint) == 0) {
            return;
        }
        if (errno != EBUSY && errno != EAGAIN) {
            fail_errno("USB_RAW_IOCTL_EP_CLEAR_HALT");
        }
        usleep(1000);
    }
    fail("USB_RAW_IOCTL_EP_CLEAR_HALT remained busy");
}

static void set_halt(MsdDevice *d, int endpoint)
{
    if (endpoint < 0) {
        return;
    }
    if (ioctl(d->raw_fd, USB_RAW_IOCTL_EP_SET_HALT, endpoint) < 0 &&
        errno != EINVAL && errno != EBUSY && errno != ESHUTDOWN) {
        fail_errno("USB_RAW_IOCTL_EP_SET_HALT");
    }
}

static void wait_for_invalid_cbw_recovery(MsdDevice *d, size_t length)
{
    pthread_mutex_lock(&d->reset_lock);
    d->invalid_cbw_waiting = true;
    d->invalid_cbw_reset_seen = false;
    pthread_mutex_unlock(&d->reset_lock);

    fprintf(stderr,
            "cm4-msd-raw-gadget: invalid %zu-byte CBW; stalled Bulk-In "
            "and Bulk-Out pending ordered Reset Recovery\n",
            length);
    fflush(stderr);

    set_halt(d, d->bulk_in);
    set_halt(d, d->bulk_out);
    pthread_mutex_lock(&d->reset_lock);
    while (!d->invalid_cbw_reset_seen) {
        pthread_cond_wait(&d->reset_cond, &d->reset_lock);
    }
    d->invalid_cbw_waiting = false;
    d->invalid_cbw_clearing = true;
    pthread_cond_broadcast(&d->reset_cond);
    pthread_mutex_unlock(&d->reset_lock);
    fprintf(stderr,
            "cm4-msd-raw-gadget: invalid CBW accepted Mass Storage Reset; "
            "awaiting host clear-halt sequence\n");
    fflush(stderr);
}

static void signal_transport_reset(MsdDevice *d, ResetKind kind)
{
    pthread_mutex_lock(&d->reset_lock);
    d->reset_generation++;
    d->reset_kind = kind;
    set_sense(d, SENSE_NONE, 0, 0);
    pthread_cond_broadcast(&d->reset_cond);
    pthread_mutex_unlock(&d->reset_lock);
}

static void wait_for_transport_reset(MsdDevice *d,
                                     const CommandBlockWrapper *cbw,
                                     const char *fault_name,
                                     bool send_phase_status)
{
    uint64_t generation;
    uint32_t residue = le32toh(cbw->transfer_length);
    ResetKind reset_kind;

    pthread_mutex_lock(&d->reset_lock);
    generation = d->reset_generation;
    d->transport_faults_fired++;
    d->transport_fault_waiting = true;
    pthread_mutex_unlock(&d->reset_lock);

    if (send_phase_status) {
        send_csw(d, cbw, residue, CSW_PHASE_ERROR);
    }
    fprintf(stderr,
            "cm4-msd-raw-gadget: injected BOT %s for opcode 0x%02x; "
            "waiting for transport reset (fault=%" PRIu64 "/%" PRIu64 ")\n",
            fault_name, cbw->cdb[0], d->transport_faults_fired,
            d->fault_limit);
    fflush(stderr);

    pthread_mutex_lock(&d->reset_lock);
    while (d->reset_generation == generation) {
        pthread_cond_wait(&d->reset_cond, &d->reset_lock);
    }
    reset_kind = d->reset_kind;
    d->transport_fault_waiting = false;
    pthread_mutex_unlock(&d->reset_lock);
    fprintf(stderr,
            "cm4-msd-raw-gadget: %s recovered BOT %s\n",
            reset_kind == RESET_BOT_CLASS ? "BOT Mass Storage Reset" :
                                            "USB bus reset",
            fault_name);
    fflush(stderr);
}

static void inject_bot_transport_fault(MsdDevice *d,
                                       const CommandBlockWrapper *cbw)
{
    const char *fault_name = d->fault == FAULT_BOT_PHASE ?
                             "phase error" : "command timeout";

    wait_for_transport_reset(d, cbw, fault_name,
                             d->fault == FAULT_BOT_PHASE);
}

static void send_csw(MsdDevice *d, const CommandBlockWrapper *cbw,
                     uint32_t residue, uint8_t status)
{
    CommandStatusWrapper csw = {
        .signature = htole32(CSW_SIGNATURE),
        .tag = cbw->tag,
        .residue = htole32(residue),
        .status = status,
    };

    bulk_write(d, &csw, sizeof(csw));
}

static uint32_t bounded_length(uint64_t available, uint32_t allocation)
{
    uint64_t length = available < allocation ? available : allocation;

    return length > UINT32_MAX ? UINT32_MAX : length;
}

static uint32_t send_response(MsdDevice *d,
                              const CommandBlockWrapper *cbw,
                              const void *data, uint32_t length)
{
    uint32_t requested = le32toh(cbw->transfer_length);
    uint32_t actual = length < requested ? length : requested;

    if (!(cbw->flags & USB_DIR_IN) && requested) {
        discard_out(d, requested);
        return requested;
    }
    send_short_terminated(d, data, actual, requested);
    return requested - actual;
}

static uint32_t inquiry_response(MsdDevice *d,
                                 const CommandBlockWrapper *cbw)
{
    uint8_t data[96] = { 0 };
    uint8_t page = cbw->cdb[2];
    uint32_t length;

    if (!(cbw->cdb[1] & 1)) {
        data[0] = 0x00;
        /* USB-exported eMMC is hot-removable from the host's perspective. */
        data[1] = 0x80;
        data[2] = 0x06;
        data[3] = 0x02;
        data[4] = 31;
        memset(data + 8, ' ', 28);
        memcpy(data + 8, "mmcblk0", 7);
        length = 36;
    } else if (page == 0x00) {
        data[1] = page;
        data[3] = 3;
        data[4] = 0x00;
        data[5] = 0x80;
        data[6] = 0x83;
        length = 7;
    } else if (page == 0x80) {
        static const char serial[] = "51554d5552504934";

        data[1] = page;
        data[3] = sizeof(serial) - 1;
        memcpy(data + 4, serial, sizeof(serial) - 1);
        length = 4 + sizeof(serial) - 1;
    } else if (page == 0x83) {
        static const char identifier[] = "mmcblk0";

        data[1] = page;
        put_be16(data + 2, 4 + sizeof(identifier) - 1);
        data[4] = 0x02;
        data[5] = 0x01;
        data[7] = sizeof(identifier) - 1;
        memcpy(data + 8, identifier, sizeof(identifier) - 1);
        length = 8 + sizeof(identifier) - 1;
    } else {
        set_sense(d, SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
        send_short_terminated(d, NULL, 0, le32toh(cbw->transfer_length));
        return le32toh(cbw->transfer_length);
    }
    length = bounded_length(length, cbw->cdb[4]);
    return send_response(d, cbw, data, length);
}

static uint32_t request_sense_response(MsdDevice *d,
                                       const CommandBlockWrapper *cbw)
{
    uint8_t data[18] = { 0 };
    uint32_t residue;

    data[0] = 0x70;
    data[2] = d->sense_key;
    data[7] = 10;
    data[12] = d->sense_asc;
    data[13] = d->sense_ascq;
    residue = send_response(d, cbw, data,
                            bounded_length(sizeof(data), cbw->cdb[4]));
    set_sense(d, SENSE_NONE, 0, 0);
    return residue;
}

static uint32_t mode_sense_response(MsdDevice *d,
                                    const CommandBlockWrapper *cbw,
                                    bool ten_byte)
{
    uint8_t data[32] = { 0 };
    uint8_t *page;
    uint32_t length;

    if (ten_byte) {
        data[3] = 0x10; /* DPOFUA */
        page = data + 8;
        page[0] = 0x08;
        page[1] = 18;
        page[2] = 0x04; /* WCE: advertise a volatile write cache. */
        length = 28;
        put_be16(data, length - 2);
    } else {
        data[2] = 0x10; /* DPOFUA */
        page = data + 4;
        page[0] = 0x08;
        page[1] = 18;
        page[2] = 0x04;
        length = 24;
        data[0] = length - 1;
    }
    length = bounded_length(length,
                            ten_byte ? get_be16(cbw->cdb + 7) : cbw->cdb[4]);
    return send_response(d, cbw, data, length);
}

static bool parse_rw(const CommandBlockWrapper *cbw, uint64_t *lba,
                     uint32_t *blocks, bool *fua)
{
    switch (cbw->cdb[0]) {
    case SCSI_READ_10:
    case SCSI_WRITE_10:
        *lba = get_be32(cbw->cdb + 2);
        *blocks = get_be16(cbw->cdb + 7);
        *fua = cbw->cdb[1] & 0x08;
        return true;
    case SCSI_READ_16:
    case SCSI_WRITE_16:
        *lba = get_be64(cbw->cdb + 2);
        *blocks = get_be32(cbw->cdb + 10);
        *fua = cbw->cdb[1] & 0x08;
        return true;
    default:
        return false;
    }
}

static BotIntent bot_intent(const CommandBlockWrapper *cbw)
{
    BotIntent intent = { .direction = BOT_DATA_UNKNOWN, .length = 0 };
    uint64_t lba;
    uint32_t blocks;
    bool fua;

    switch (cbw->cdb[0]) {
    case SCSI_TEST_UNIT_READY:
    case SCSI_START_STOP:
    case SCSI_PREVENT_ALLOW:
    case SCSI_SYNCHRONIZE_CACHE_10:
    case SCSI_SYNCHRONIZE_CACHE_16:
        intent.direction = BOT_DATA_NONE;
        break;
    case SCSI_REQUEST_SENSE:
        intent.direction = BOT_DATA_IN;
        intent.length = bounded_length(18, cbw->cdb[4]);
        break;
    case SCSI_INQUIRY: {
        uint32_t available = 36;

        if (cbw->cdb[1] & 1) {
            switch (cbw->cdb[2]) {
            case 0x00:
                available = 7;
                break;
            case 0x80:
                available = 20;
                break;
            case 0x83:
                available = 25;
                break;
            default:
                available = 0;
                break;
            }
        }
        intent.direction = BOT_DATA_IN;
        intent.length = bounded_length(available, cbw->cdb[4]);
        break;
    }
    case SCSI_MODE_SENSE_6:
        intent.direction = BOT_DATA_IN;
        intent.length = bounded_length(24, cbw->cdb[4]);
        break;
    case SCSI_MODE_SENSE_10:
        intent.direction = BOT_DATA_IN;
        intent.length = bounded_length(28, get_be16(cbw->cdb + 7));
        break;
    case SCSI_READ_FORMAT_CAPACITIES:
        intent.direction = BOT_DATA_IN;
        intent.length = bounded_length(12, get_be16(cbw->cdb + 7));
        break;
    case SCSI_READ_CAPACITY_10:
        intent.direction = BOT_DATA_IN;
        intent.length = 8;
        break;
    case SCSI_SERVICE_ACTION_IN_16:
        if ((cbw->cdb[1] & 0x1f) == SCSI_READ_CAPACITY_16) {
            intent.direction = BOT_DATA_IN;
            intent.length = bounded_length(32, get_be32(cbw->cdb + 10));
        }
        break;
    case SCSI_REPORT_LUNS:
        intent.direction = BOT_DATA_IN;
        intent.length = bounded_length(16, get_be32(cbw->cdb + 6));
        break;
    case SCSI_READ_10:
    case SCSI_READ_16:
        parse_rw(cbw, &lba, &blocks, &fua);
        intent.direction = BOT_DATA_IN;
        intent.length = bounded_length((uint64_t)blocks * SECTOR_SIZE,
                                       UINT32_MAX);
        break;
    case SCSI_WRITE_10:
    case SCSI_WRITE_16:
        parse_rw(cbw, &lba, &blocks, &fua);
        intent.direction = BOT_DATA_OUT;
        intent.length = bounded_length((uint64_t)blocks * SECTOR_SIZE,
                                       UINT32_MAX);
        break;
    case SCSI_VERIFY_10:
        if (cbw->cdb[1] & 0x02) {
            blocks = get_be16(cbw->cdb + 7);
            intent.direction = BOT_DATA_OUT;
            intent.length = bounded_length((uint64_t)blocks * SECTOR_SIZE,
                                           UINT32_MAX);
        } else {
            intent.direction = BOT_DATA_NONE;
        }
        break;
    default:
        break;
    }
    return intent;
}

static bool range_valid(MsdDevice *d, uint64_t lba, uint32_t blocks)
{
    uint64_t total = d->image_size / SECTOR_SIZE;

    return lba <= total && blocks <= total - lba;
}

static uint8_t read_blocks(MsdDevice *d, const CommandBlockWrapper *cbw,
                           uint64_t lba, uint32_t blocks, uint32_t *residue)
{
    BulkIO io;
    uint64_t offset = lba * SECTOR_SIZE;
    uint64_t remaining = (uint64_t)blocks * SECTOR_SIZE;
    uint32_t requested = le32toh(cbw->transfer_length);

    if (remaining != requested) {
        set_sense(d, SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
        send_short_terminated(d, NULL, 0, requested);
        *residue = requested;
        return CSW_PHASE_ERROR;
    }
    if (remaining && data_reset_matches(d, cbw->cdb[0])) {
        size_t prefix = d->fault_bytes;
        char reason[96];
        ssize_t got;

        if (prefix > remaining) {
            fail("--fault-bytes exceeds matching READ transfer");
        }
        got = pread(d->image_fd, io.data, prefix, offset);
        if (got < 0) {
            fail_errno("read eMMC backend for reset fault");
        }
        if ((size_t)got != prefix) {
            fail("short eMMC backend read for reset fault");
        }
        bulk_write(d, io.data, prefix);
        *residue = requested - prefix;
        snprintf(reason, sizeof(reason),
                 "READ data-phase reset after %zu bytes", prefix);
        wait_for_transport_reset(d, cbw, reason, false);
        return CSW_COMMAND_ABORTED;
    }
    while (remaining) {
        size_t chunk = remaining < sizeof(io.data) ?
                       remaining : sizeof(io.data);
        ssize_t got = pread(d->image_fd, io.data, chunk, offset);

        if (got < 0) {
            fail_errno("read eMMC backend");
        }
        if ((size_t)got != chunk) {
            fail("short eMMC backend read");
        }
        bulk_write(d, io.data, chunk);
        remaining -= chunk;
        offset += chunk;
    }
    *residue = 0;
    return CSW_PASSED;
}

static void read_blocks_prefix(MsdDevice *d, uint64_t lba, uint32_t length)
{
    BulkIO io;
    uint64_t offset = lba * SECTOR_SIZE;
    uint32_t remaining = length;

    while (remaining) {
        size_t chunk = remaining < sizeof(io.data) ?
                       remaining : sizeof(io.data);
        ssize_t got = pread(d->image_fd, io.data, chunk, offset);

        if (got < 0) {
            fail_errno("read eMMC backend prefix");
        }
        if ((size_t)got != chunk) {
            fail("short eMMC backend prefix read");
        }
        bulk_write(d, io.data, chunk);
        remaining -= chunk;
        offset += chunk;
    }
}

static uint8_t write_blocks(MsdDevice *d, const CommandBlockWrapper *cbw,
                            uint64_t lba, uint32_t blocks, bool fua,
                            bool inject, uint32_t *residue)
{
    BulkIO io;
    uint64_t offset = lba * SECTOR_SIZE;
    uint64_t remaining = (uint64_t)blocks * SECTOR_SIZE;
    uint32_t requested = le32toh(cbw->transfer_length);

    if (remaining != requested) {
        discard_out(d, requested);
        set_sense(d, SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
        *residue = requested;
        return CSW_PHASE_ERROR;
    }
    if (remaining && data_reset_matches(d, cbw->cdb[0])) {
        size_t prefix = d->fault_bytes;
        char reason[96];

        if (prefix > remaining) {
            fail("--fault-bytes exceeds matching WRITE transfer");
        }
        bulk_read_prefix(d, io.data, prefix, remaining);
        durable_pwrite_exact(d->image_fd, io.data, prefix, offset,
                             "write eMMC backend for reset fault");
        *residue = requested - prefix;
        snprintf(reason, sizeof(reason),
                 "WRITE data-phase reset after %zu bytes", prefix);
        wait_for_transport_reset(d, cbw, reason, false);
        return CSW_COMMAND_ABORTED;
    }
    while (remaining) {
        size_t chunk = remaining < sizeof(io.data) ?
                       remaining : sizeof(io.data);
        size_t done = 0;

        bulk_read_exact(d, io.data, chunk);
        while (done < chunk) {
            ssize_t written = pwrite(d->image_fd, io.data + done,
                                     chunk - done, offset + done);

            if (written < 0) {
                fail_errno("write eMMC backend");
            }
            done += written;
        }
        remaining -= chunk;
        offset += chunk;
    }
    *residue = 0;
    if (inject) {
        set_sense(d, SENSE_MEDIUM_ERROR, ASC_WRITE_ERROR, 0);
        return CSW_FAILED;
    }
    if (fua && fdatasync(d->image_fd) < 0) {
        set_sense(d, SENSE_HARDWARE_ERROR, ASC_INTERNAL_TARGET_FAILURE, 0);
        return CSW_FAILED;
    }
    return CSW_PASSED;
}

static uint8_t execute_scsi(MsdDevice *d, const CommandBlockWrapper *cbw,
                            uint32_t *residue)
{
    uint8_t opcode = cbw->cdb[0];
    uint32_t requested = le32toh(cbw->transfer_length);
    uint64_t lba = 0;
    uint32_t blocks = 0;
    bool fua = false;
    bool inject;

    parse_rw(cbw, &lba, &blocks, &fua);
    inject = fault_matches(d, opcode, fua);
    if (inject && opcode != SCSI_WRITE_10 && opcode != SCSI_WRITE_16 &&
        opcode != SCSI_SYNCHRONIZE_CACHE_10 &&
        opcode != SCSI_SYNCHRONIZE_CACHE_16) {
        if (cbw->flags & USB_DIR_IN) {
            send_short_terminated(d, NULL, 0, requested);
        } else {
            discard_out(d, requested);
        }
        set_sense(d, SENSE_MEDIUM_ERROR, ASC_WRITE_ERROR, 0);
        *residue = requested;
        return CSW_FAILED;
    }

    switch (opcode) {
    case SCSI_TEST_UNIT_READY:
    case SCSI_START_STOP:
    case SCSI_PREVENT_ALLOW:
        *residue = requested;
        return CSW_PASSED;
    case SCSI_REQUEST_SENSE:
        *residue = request_sense_response(d, cbw);
        return CSW_PASSED;
    case SCSI_INQUIRY:
        *residue = inquiry_response(d, cbw);
        return d->sense_key == SENSE_NONE ? CSW_PASSED : CSW_FAILED;
    case SCSI_MODE_SENSE_6:
        *residue = mode_sense_response(d, cbw, false);
        return CSW_PASSED;
    case SCSI_MODE_SENSE_10:
        *residue = mode_sense_response(d, cbw, true);
        return CSW_PASSED;
    case SCSI_READ_FORMAT_CAPACITIES: {
        uint8_t data[12] = { 0 };
        uint64_t sectors = d->image_size / SECTOR_SIZE;

        data[3] = 8;
        put_be32(data + 4, sectors > UINT32_MAX ? UINT32_MAX : sectors);
        data[8] = 0x02;
        put_be24(data + 9, SECTOR_SIZE);
        *residue = send_response(
            d, cbw, data,
            bounded_length(sizeof(data), get_be16(cbw->cdb + 7)));
        return CSW_PASSED;
    }
    case SCSI_READ_CAPACITY_10: {
        uint8_t data[8] = { 0 };
        uint64_t sectors = d->image_size / SECTOR_SIZE;
        uint64_t last = sectors - 1;

        put_be32(data, last > UINT32_MAX ? UINT32_MAX : last);
        put_be32(data + 4, SECTOR_SIZE);
        *residue = send_response(d, cbw, data, sizeof(data));
        return CSW_PASSED;
    }
    case SCSI_SERVICE_ACTION_IN_16: {
        uint8_t data[32] = { 0 };

        if ((cbw->cdb[1] & 0x1f) != SCSI_READ_CAPACITY_16) {
            break;
        }
        put_be64(data, d->image_size / SECTOR_SIZE - 1);
        put_be32(data + 8, SECTOR_SIZE);
        *residue = send_response(
            d, cbw, data,
            bounded_length(sizeof(data), get_be32(cbw->cdb + 10)));
        return CSW_PASSED;
    }
    case SCSI_REPORT_LUNS: {
        uint8_t data[16] = { 0 };

        put_be32(data, 8);
        *residue = send_response(
            d, cbw, data,
            bounded_length(sizeof(data), get_be32(cbw->cdb + 6)));
        return CSW_PASSED;
    }
    case SCSI_READ_10:
    case SCSI_READ_16:
        if (!range_valid(d, lba, blocks)) {
            send_short_terminated(d, NULL, 0, requested);
            set_sense(d, SENSE_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE, 0);
            *residue = requested;
            return CSW_FAILED;
        }
        return read_blocks(d, cbw, lba, blocks, residue);
    case SCSI_WRITE_10:
    case SCSI_WRITE_16:
        if (!range_valid(d, lba, blocks)) {
            discard_out(d, requested);
            set_sense(d, SENSE_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE, 0);
            *residue = requested;
            return CSW_FAILED;
        }
        return write_blocks(d, cbw, lba, blocks, fua, inject, residue);
    case SCSI_SYNCHRONIZE_CACHE_10:
    case SCSI_SYNCHRONIZE_CACHE_16:
        *residue = requested;
        if (inject) {
            set_sense(d, SENSE_MEDIUM_ERROR, ASC_WRITE_ERROR, 0);
            return CSW_FAILED;
        }
        if (fdatasync(d->image_fd) < 0) {
            set_sense(d, SENSE_HARDWARE_ERROR,
                      ASC_INTERNAL_TARGET_FAILURE, 0);
            return CSW_FAILED;
        }
        return CSW_PASSED;
    case SCSI_VERIFY_10:
        if (cbw->cdb[1] & 0x02) {
            discard_out(d, requested);
        }
        *residue = 0;
        return CSW_PASSED;
    default:
        break;
    }

    if (cbw->flags & USB_DIR_IN) {
        send_short_terminated(d, NULL, 0, requested);
    } else {
        discard_out(d, requested);
    }
    set_sense(d, SENSE_ILLEGAL_REQUEST, ASC_INVALID_COMMAND, 0);
    *residue = requested;
    return CSW_FAILED;
}

static unsigned int bot_case_number(const CommandBlockWrapper *cbw,
                                    BotIntent intent)
{
    uint32_t requested = le32toh(cbw->transfer_length);

    if (!requested) {
        return intent.direction == BOT_DATA_NONE ? 1 :
               intent.direction == BOT_DATA_IN ? 2 : 3;
    }
    if (cbw->flags & USB_DIR_IN) {
        if (intent.direction == BOT_DATA_NONE) {
            return 4;
        }
        if (intent.direction == BOT_DATA_OUT) {
            return 8;
        }
        return requested > intent.length ? 5 :
               requested == intent.length ? 6 : 7;
    }
    if (intent.direction == BOT_DATA_NONE) {
        return 9;
    }
    if (intent.direction == BOT_DATA_IN) {
        return 10;
    }
    return requested > intent.length ? 11 :
           requested == intent.length ? 12 : 13;
}

static bool scsi_cdb_length_valid(const CommandBlockWrapper *cbw)
{
    uint8_t expected;

    switch (cbw->cdb[0]) {
    case SCSI_TEST_UNIT_READY:
    case SCSI_REQUEST_SENSE:
    case SCSI_INQUIRY:
    case SCSI_MODE_SENSE_6:
    case SCSI_START_STOP:
    case SCSI_PREVENT_ALLOW:
        expected = 6;
        break;
    case SCSI_READ_FORMAT_CAPACITIES:
    case SCSI_READ_CAPACITY_10:
    case SCSI_READ_10:
    case SCSI_WRITE_10:
    case SCSI_VERIFY_10:
    case SCSI_SYNCHRONIZE_CACHE_10:
    case SCSI_MODE_SENSE_10:
        expected = 10;
        break;
    case SCSI_REPORT_LUNS:
        expected = 12;
        break;
    case SCSI_READ_16:
    case SCSI_WRITE_16:
    case SCSI_SYNCHRONIZE_CACHE_16:
    case SCSI_SERVICE_ACTION_IN_16:
        expected = 16;
        break;
    default:
        return true;
    }
    return cbw->cdb_length == expected;
}

static uint8_t execute_bot_scsi(MsdDevice *d,
                                const CommandBlockWrapper *cbw,
                                uint32_t *residue,
                                unsigned int *case_number)
{
    CommandBlockWrapper adjusted = *cbw;
    BotIntent intent = bot_intent(cbw);
    uint32_t requested = le32toh(cbw->transfer_length);
    uint32_t inner_residue = 0;
    uint64_t lba;
    uint32_t blocks;
    bool fua;
    const char *host_direction = "none";
    const char *device_direction = "none";
    uint8_t status;

    if (intent.direction == BOT_DATA_UNKNOWN) {
        *case_number = 0;
        return execute_scsi(d, cbw, residue);
    }
    *case_number = bot_case_number(cbw, intent);
    if (*case_number != 1 && *case_number != 6 && *case_number != 12) {
        if (requested) {
            host_direction = cbw->flags & USB_DIR_IN ? "in" : "out";
        }
        if (intent.direction == BOT_DATA_IN) {
            device_direction = "in";
        } else if (intent.direction == BOT_DATA_OUT) {
            device_direction = "out";
        }
        fprintf(stderr,
                "cm4-msd-raw-gadget: BOT thirteen-case %u opcode=0x%02x "
                "host=%s/%" PRIu32 " device=%s/%" PRIu32 "\n",
                *case_number, cbw->cdb[0], host_direction, requested,
                device_direction, intent.length);
        fflush(stderr);
    }

    switch (*case_number) {
    case 1:
    case 6:
    case 12:
        return execute_scsi(d, cbw, residue);
    case 2:
    case 3:
        *residue = 0;
        return CSW_PHASE_ERROR;
    case 4:
        adjusted.transfer_length = 0;
        status = execute_scsi(d, &adjusted, &inner_residue);
        *residue = requested;
        stall_bulk_in_before_csw(d);
        return status;
    case 5:
        if (cbw->cdb[0] == SCSI_READ_10 ||
            cbw->cdb[0] == SCSI_READ_16) {
            adjusted.transfer_length = htole32(intent.length);
            status = execute_scsi(d, &adjusted, &inner_residue);
            *residue = inner_residue + requested - intent.length;
        } else {
            status = execute_scsi(d, cbw, residue);
        }
        stall_bulk_in_before_csw(d);
        return status;
    case 7:
        if (cbw->cdb[0] == SCSI_READ_10 ||
            cbw->cdb[0] == SCSI_READ_16) {
            parse_rw(cbw, &lba, &blocks, &fua);
            if (range_valid(d, lba, blocks)) {
                read_blocks_prefix(d, lba, requested);
                *residue = 0;
            } else {
                set_sense(d, SENSE_ILLEGAL_REQUEST,
                          ASC_LBA_OUT_OF_RANGE, 0);
                *residue = requested;
            }
        } else {
            execute_scsi(d, cbw, residue);
        }
        stall_bulk_in_before_csw(d);
        return CSW_PHASE_ERROR;
    case 8:
        *residue = requested;
        stall_bulk_in_before_csw(d);
        return CSW_PHASE_ERROR;
    case 9:
        discard_out(d, requested);
        adjusted.transfer_length = 0;
        status = execute_scsi(d, &adjusted, &inner_residue);
        *residue = requested;
        return status;
    case 10:
        discard_out(d, requested);
        *residue = requested;
        return CSW_PHASE_ERROR;
    case 11:
        adjusted.transfer_length = htole32(intent.length);
        status = execute_scsi(d, &adjusted, &inner_residue);
        discard_out(d, requested - intent.length);
        *residue = inner_residue + requested - intent.length;
        return status;
    case 13:
        discard_out(d, requested);
        *residue = 0;
        return CSW_PHASE_ERROR;
    default:
        fail("invalid BOT thirteen-case classification");
    }
    return CSW_PHASE_ERROR;
}

static void send_phase_csw_and_wait(MsdDevice *d,
                                    const CommandBlockWrapper *cbw,
                                    uint32_t residue,
                                    unsigned int case_number)
{
    uint64_t generation;
    ResetKind reset_kind;

    pthread_mutex_lock(&d->reset_lock);
    generation = d->reset_generation;
    d->transport_fault_waiting = true;
    pthread_mutex_unlock(&d->reset_lock);
    send_csw(d, cbw, residue, CSW_PHASE_ERROR);
    fprintf(stderr,
            "cm4-msd-raw-gadget: BOT thirteen-case %u phase error; "
            "waiting for Reset Recovery\n",
            case_number);
    fflush(stderr);

    pthread_mutex_lock(&d->reset_lock);
    while (d->reset_generation == generation) {
        pthread_cond_wait(&d->reset_cond, &d->reset_lock);
    }
    reset_kind = d->reset_kind;
    d->transport_fault_waiting = false;
    pthread_mutex_unlock(&d->reset_lock);
    fprintf(stderr,
            "cm4-msd-raw-gadget: %s recovered BOT thirteen-case %u\n",
            reset_kind == RESET_BOT_CLASS ? "BOT Mass Storage Reset" :
                                            "USB bus reset",
            case_number);
    fflush(stderr);
}

static void bot_loop(MsdDevice *d)
{
    d->bot_running = true;
    printf("CM4 raw BOT target ready: backend=%s size=%" PRIu64
           " bytes serial=51554d5552504934\n",
           d->image_path, d->image_size);
    fflush(stdout);

    for (;;) {
        CommandBlockWrapper cbw;
        uint32_t residue;
        uint8_t status;
        unsigned int case_number;

        size_t cbw_length = bulk_read(d, &cbw, sizeof(cbw));

        if (cbw_length != sizeof(cbw) ||
            le32toh(cbw.signature) != CBW_SIGNATURE) {
            wait_for_invalid_cbw_recovery(d, cbw_length);
            continue;
        }
        pthread_mutex_lock(&d->reset_lock);
        if (d->owner_stopping) {
            pthread_mutex_unlock(&d->reset_lock);
            return;
        }
        d->command_active = true;
        pthread_mutex_unlock(&d->reset_lock);
        if ((cbw.flags & ~USB_DIR_IN) || cbw.lun != 0 ||
            cbw.cdb_length < 1 || cbw.cdb_length > sizeof(cbw.cdb) ||
            !scsi_cdb_length_valid(&cbw)) {
            uint32_t requested = le32toh(cbw.transfer_length);

            fprintf(stderr,
                    "cm4-msd-raw-gadget: valid but not meaningful CBW "
                    "flags=0x%02x lun=%u cdb-length=%u opcode=0x%02x\n",
                    cbw.flags, cbw.lun, cbw.cdb_length, cbw.cdb[0]);
            fflush(stderr);

            if (cbw.flags & USB_DIR_IN) {
                send_short_terminated(d, NULL, 0, requested);
            } else {
                discard_out(d, requested);
            }
            set_sense(d, SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
            send_csw(d, &cbw, requested, CSW_FAILED);
            pthread_mutex_lock(&d->reset_lock);
            d->command_active = false;
            pthread_cond_broadcast(&d->reset_cond);
            pthread_mutex_unlock(&d->reset_lock);
            continue;
        }
        d->command_count++;
        if (transport_fault_matches(d, cbw.cdb[0])) {
            inject_bot_transport_fault(d, &cbw);
            pthread_mutex_lock(&d->reset_lock);
            d->command_active = false;
            pthread_cond_broadcast(&d->reset_cond);
            pthread_mutex_unlock(&d->reset_lock);
            continue;
        }
        status = execute_bot_scsi(d, &cbw, &residue, &case_number);
        if (status == CSW_COMMAND_ABORTED) {
            pthread_mutex_lock(&d->reset_lock);
            d->command_active = false;
            pthread_cond_broadcast(&d->reset_cond);
            pthread_mutex_unlock(&d->reset_lock);
            continue;
        }
        if (status != CSW_PASSED && d->fault_active && !d->fault_reported) {
            lifecycle_write(d, "flash-failed");
            d->fault_reported = true;
        }
        if (status != CSW_PASSED) {
            fprintf(stderr,
                    "cm4-msd-raw-gadget: SCSI opcode 0x%02x failed "
                    "at command=%" PRIu64 " residue=%" PRIu32 "%s\n",
                    cbw.cdb[0], d->command_count, residue,
                    d->fault_active ? " (injected)" : "");
            fflush(stderr);
        }
        if (status == CSW_PHASE_ERROR) {
            send_phase_csw_and_wait(d, &cbw, residue, case_number);
        } else {
            send_csw(d, &cbw, residue, status);
        }
        pthread_mutex_lock(&d->reset_lock);
        d->command_active = false;
        pthread_cond_broadcast(&d->reset_cond);
        pthread_mutex_unlock(&d->reset_lock);
    }
}

static void *bot_worker(void *opaque)
{
    bot_loop(opaque);
    return NULL;
}

static void *owner_worker(void *opaque)
{
    MsdDevice *d = opaque;
    char command[32];
    bool complete;

    if (!fgets(command, sizeof(command), stdin)) {
        command[0] = '\0';
    }
    complete = !strcmp(command, "complete\n");
    if (!complete && strcmp(command, "failed\n")) {
        fprintf(stderr,
                "cm4-msd-raw-gadget: foreground owner requires exact "
                "'complete' or 'failed' command\n");
        fflush(stderr);
    }

    pthread_mutex_lock(&d->reset_lock);
    d->owner_stopping = true;
    while (d->command_active) {
        pthread_cond_wait(&d->reset_cond, &d->reset_lock);
    }
    pthread_mutex_unlock(&d->reset_lock);

    if (complete && fdatasync(d->image_fd) == 0) {
        lifecycle_write(d, "boot-ready");
        puts("CM4 raw BOT owner released boot-ready eMMC");
        fflush(stdout);
        exit(EXIT_SUCCESS);
    }
    lifecycle_write(d, "flash-failed");
    if (complete) {
        fprintf(stderr,
                "cm4-msd-raw-gadget: cannot durably flush eMMC backend: %s\n",
                strerror(errno));
        fflush(stderr);
    }
    exit(EXIT_FAILURE);
}

static bool standard_request(MsdDevice *d,
                             const struct usb_ctrlrequest *ctrl)
{
    uint8_t data[EP0_MAX_DATA] = { 0 };
    uint8_t descriptor_type;
    uint8_t descriptor_index;
    size_t length;

    switch (ctrl->bRequest) {
    case USB_REQ_GET_DESCRIPTOR:
        descriptor_type = get_le16(ctrl->wValue) >> 8;
        descriptor_index = get_le16(ctrl->wValue);
        switch (descriptor_type) {
        case USB_DT_DEVICE:
            memcpy(data, &device_descriptor, sizeof(device_descriptor));
            length = sizeof(device_descriptor);
            break;
        case USB_DT_CONFIG:
            length = build_config(data, sizeof(data));
            break;
        case USB_DT_DEVICE_QUALIFIER:
            memcpy(data, &qualifier_descriptor, sizeof(qualifier_descriptor));
            length = sizeof(qualifier_descriptor);
            break;
        case USB_DT_STRING:
            length = build_string(data, sizeof(data), descriptor_index);
            if (!length) {
                return false;
            }
            break;
        default:
            return false;
        }
        ep0_reply(d, ctrl, data, length);
        return true;
    case USB_REQ_SET_CONFIGURATION:
        enable_endpoints(d);
        ep0_ack_out(d, ctrl);
        return true;
    case USB_REQ_GET_CONFIGURATION:
        data[0] = d->configured ? 1 : 0;
        ep0_reply(d, ctrl, data, 1);
        return true;
    case USB_REQ_GET_INTERFACE:
        data[0] = 0;
        ep0_reply(d, ctrl, data, 1);
        return true;
    case USB_REQ_SET_INTERFACE:
    case USB_REQ_SET_ADDRESS:
        ep0_ack_out(d, ctrl);
        return true;
    case USB_REQ_GET_STATUS:
        ep0_reply(d, ctrl, data, 2);
        return true;
    case USB_REQ_CLEAR_FEATURE:
        if ((ctrl->bRequestType & USB_RECIP_MASK) == USB_RECIP_ENDPOINT &&
            get_le16(ctrl->wValue) == USB_ENDPOINT_HALT) {
            uint8_t address = get_le16(ctrl->wIndex);

            if (address == bulk_in_descriptor.bEndpointAddress) {
                ep0_ack_out(d, ctrl);
                clear_halt(d, d->bulk_in);
            } else if (address == bulk_out_descriptor.bEndpointAddress) {
                ep0_ack_out(d, ctrl);
                clear_halt(d, d->bulk_out);
            } else if (address == acm_notify_descriptor.bEndpointAddress) {
                ep0_ack_out(d, ctrl);
                clear_halt(d, d->acm_notify);
            } else if (address == acm_in_descriptor.bEndpointAddress) {
                ep0_ack_out(d, ctrl);
                clear_halt(d, d->acm_in);
            } else if (address == acm_out_descriptor.bEndpointAddress) {
                ep0_ack_out(d, ctrl);
                clear_halt(d, d->acm_out);
            } else {
                ep0_ack_out(d, ctrl);
            }
            return true;
        }
        ep0_ack_out(d, ctrl);
        return true;
    case USB_REQ_SET_FEATURE:
        ep0_ack_out(d, ctrl);
        return true;
    default:
        return false;
    }
}

static void class_request(MsdDevice *d, const struct usb_ctrlrequest *ctrl)
{
    uint8_t max_lun = 0;
    uint16_t interface = get_le16(ctrl->wIndex);

    if (interface == 0 && ctrl->bRequest == MSC_REQUEST_GET_MAX_LUN &&
        (ctrl->bRequestType & USB_DIR_IN) && get_le16(ctrl->wLength) == 1) {
        ep0_reply(d, ctrl, &max_lun, sizeof(max_lun));
        return;
    }
    if (interface == 0 && ctrl->bRequest == MSC_REQUEST_RESET &&
        !(ctrl->bRequestType & USB_DIR_IN) && !get_le16(ctrl->wLength)) {
        ep0_ack_out(d, ctrl);
        pthread_mutex_lock(&d->reset_lock);
        if (d->invalid_cbw_waiting) {
            d->invalid_cbw_reset_seen = true;
        }
        pthread_mutex_unlock(&d->reset_lock);
        signal_transport_reset(d, RESET_BOT_CLASS);
        fprintf(stderr,
                "cm4-msd-raw-gadget: received BOT Mass Storage Reset\n");
        fflush(stderr);
        return;
    }
    if (interface == 1 && ctrl->bRequest == CDC_SET_LINE_CODING &&
        !(ctrl->bRequestType & USB_DIR_IN) && get_le16(ctrl->wLength) == 7) {
        ep0_read_exact(d, ctrl, d->line_coding, sizeof(d->line_coding));
        return;
    }
    if (interface == 1 && ctrl->bRequest == CDC_GET_LINE_CODING &&
        (ctrl->bRequestType & USB_DIR_IN) && get_le16(ctrl->wLength) == 7) {
        ep0_reply(d, ctrl, d->line_coding, sizeof(d->line_coding));
        return;
    }
    if (interface == 1 && ctrl->bRequest == CDC_SET_CONTROL_LINE_STATE &&
        !(ctrl->bRequestType & USB_DIR_IN) && !get_le16(ctrl->wLength)) {
        d->control_line_state = get_le16(ctrl->wValue);
        ep0_ack_out(d, ctrl);
        return;
    }
    if (interface == 1 && ctrl->bRequest == CDC_SEND_BREAK &&
        !(ctrl->bRequestType & USB_DIR_IN) && !get_le16(ctrl->wLength)) {
        ep0_ack_out(d, ctrl);
        return;
    }
    raw_ioctl(d->raw_fd, USB_RAW_IOCTL_EP0_STALL, NULL,
              "USB_RAW_IOCTL_EP0_STALL class request");
}

static void handle_control(MsdDevice *d, const struct usb_ctrlrequest *ctrl)
{
    if (d->verbose) {
        fprintf(stderr,
                "cm4-msd-raw-gadget: control type=0x%02x request=0x%02x "
                "value=0x%04x index=0x%04x length=%u\n",
                ctrl->bRequestType, ctrl->bRequest,
                get_le16(ctrl->wValue), get_le16(ctrl->wIndex),
                get_le16(ctrl->wLength));
        fflush(stderr);
    }
    switch (ctrl->bRequestType & USB_TYPE_MASK) {
    case USB_TYPE_STANDARD:
        if (!standard_request(d, ctrl)) {
            raw_ioctl(d->raw_fd, USB_RAW_IOCTL_EP0_STALL, NULL,
                      "USB_RAW_IOCTL_EP0_STALL standard request");
        }
        break;
    case USB_TYPE_CLASS:
        class_request(d, ctrl);
        break;
    default:
        raw_ioctl(d->raw_fd, USB_RAW_IOCTL_EP0_STALL, NULL,
                  "USB_RAW_IOCTL_EP0_STALL unsupported request");
        break;
    }
}

static void lock_backend(MsdDevice *d)
{
    struct flock lock = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0,
    };
    struct stat st;

    d->image_fd = open(d->image_path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (d->image_fd < 0) {
        fail_errno("open eMMC backend");
    }
    if (fstat(d->image_fd, &st) < 0) {
        fail_errno("stat eMMC backend");
    }
    if (!S_ISREG(st.st_mode) || st.st_size <= 0 ||
        st.st_size % SECTOR_SIZE) {
        fail("eMMC backend must be a non-empty, sector-aligned regular file");
    }
    if (fcntl(d->image_fd, F_SETLK, &lock) < 0) {
        fail_errno("lock eMMC backend");
    }
    d->image_size = st.st_size;
}

static void run_device(MsdDevice *d)
{
    struct usb_raw_init init = { .speed = USB_SPEED_HIGH };
    int driver_length;
    int device_length;
    int error;

    error = pthread_mutex_init(&d->reset_lock, NULL);
    if (!error) {
        error = pthread_cond_init(&d->reset_cond, NULL);
    }
    if (error) {
        errno = error;
        fail_errno("initialize BOT reset synchronization");
    }
    lifecycle_lock(d);
    lock_backend(d);
    lifecycle_claim_mass_storage(d);
    d->raw_fd = open("/dev/raw-gadget", O_RDWR | O_CLOEXEC);
    if (d->raw_fd < 0) {
        fail_errno("open /dev/raw-gadget");
    }
    driver_length = snprintf((char *)init.driver_name,
                             sizeof(init.driver_name), "%s", d->udc_driver);
    device_length = snprintf((char *)init.device_name,
                             sizeof(init.device_name), "%s", d->udc_device);
    if (driver_length < 0 ||
        (size_t)driver_length >= sizeof(init.driver_name) ||
        device_length < 0 ||
        (size_t)device_length >= sizeof(init.device_name)) {
        fail("invalid UDC name");
    }
    raw_ioctl(d->raw_fd, USB_RAW_IOCTL_INIT, &init, "USB_RAW_IOCTL_INIT");
    raw_ioctl(d->raw_fd, USB_RAW_IOCTL_RUN, NULL, "USB_RAW_IOCTL_RUN");
    if (d->foreground_owner) {
        error = pthread_create(&d->owner_thread, NULL, owner_worker, d);
        if (error) {
            errno = error;
            fail_errno("start foreground ownership worker");
        }
    }
    printf("waiting for host USB Mass Storage enumeration\n");
    fflush(stdout);

    for (;;) {
        ControlEvent event = { .event = {
            .type = USB_RAW_EVENT_INVALID,
            .length = sizeof(event.ctrl),
        } };

        raw_ioctl(d->raw_fd, USB_RAW_IOCTL_EVENT_FETCH, &event,
                  "USB_RAW_IOCTL_EVENT_FETCH");
        switch (event.event.type) {
        case USB_RAW_EVENT_CONNECT:
        case USB_RAW_EVENT_SUSPEND:
        case USB_RAW_EVENT_RESUME:
        case USB_RAW_EVENT_DISCONNECT:
            break;
        case USB_RAW_EVENT_RESET:
            pthread_mutex_lock(&d->reset_lock);
            d->configured = false;
            if (d->transport_fault_waiting) {
                d->reset_pending = true;
            }
            pthread_cond_broadcast(&d->reset_cond);
            pthread_mutex_unlock(&d->reset_lock);
            if (d->verbose) {
                fprintf(stderr,
                        "cm4-msd-raw-gadget: received USB bus reset\n");
                fflush(stderr);
            }
            break;
        case USB_RAW_EVENT_CONTROL:
            if (event.event.length != sizeof(event.ctrl)) {
                fail("malformed Raw Gadget control event");
            }
            handle_control(d, &event.ctrl);
            break;
        default:
            fail("unknown Raw Gadget event");
        }
    }
}

static bool parse_unsigned(const char *value, uint64_t maximum,
                           uint64_t *result)
{
    uint64_t parsed = 0;
    unsigned int base = 10;
    size_t offset = 0;

    if (!value[0]) {
        return false;
    }
    if (value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
        base = 16;
        offset = 2;
        if (!value[offset]) {
            return false;
        }
    }
    for (; value[offset]; offset++) {
        unsigned int digit;

        if (value[offset] >= '0' && value[offset] <= '9') {
            digit = value[offset] - '0';
        } else if (value[offset] >= 'a' && value[offset] <= 'f') {
            digit = value[offset] - 'a' + 10;
        } else if (value[offset] >= 'A' && value[offset] <= 'F') {
            digit = value[offset] - 'A' + 10;
        } else {
            return false;
        }
        if (digit >= base || parsed > (maximum - digit) / base) {
            return false;
        }
        parsed = parsed * base + digit;
    }
    *result = parsed;
    return true;
}

static uint64_t parse_positive(const char *value, const char *option)
{
    uint64_t parsed;

    if (!parse_unsigned(value, UINT64_MAX, &parsed) || !parsed) {
        fprintf(stderr, "cm4-msd-raw-gadget: %s requires a positive integer\n",
                option);
        exit(EXIT_FAILURE);
    }
    return parsed;
}

static uint8_t parse_opcode(const char *value)
{
    uint64_t parsed;

    if (!parse_unsigned(value, UINT8_MAX, &parsed)) {
        fail("opcode fault must be between 0x00 and 0xff");
    }
    return parsed;
}

static FaultKind parse_fault(MsdDevice *d, const char *value)
{
    if (!strcmp(value, "synchronize-cache")) {
        return FAULT_SYNCHRONIZE_CACHE;
    }
    if (!strcmp(value, "write-fua")) {
        return FAULT_WRITE_FUA;
    }
    if (!strcmp(value, "bot-phase")) {
        return FAULT_BOT_PHASE;
    }
    if (!strcmp(value, "bot-timeout")) {
        return FAULT_BOT_TIMEOUT;
    }
    if (!strcmp(value, "bot-data-reset")) {
        return FAULT_BOT_DATA_RESET;
    }
    if (!strcmp(value, "bot-write-reset")) {
        return FAULT_BOT_WRITE_RESET;
    }
    if (!strncmp(value, "opcode:", 7)) {
        d->fault_opcode = parse_opcode(value + 7);
        return FAULT_OPCODE;
    }
    fail("unknown --fault value");
    return FAULT_NONE;
}

static int self_test(void)
{
    static const uint8_t expected_config[] = {
        0x09, 0x02, 0x62, 0x00, 0x03, 0x01, 0x04, 0x80, 0xfa,
        0x09, 0x04, 0x00, 0x00, 0x02, 0x08, 0x06, 0x50, 0x00,
        0x07, 0x05, 0x81, 0x02, 0x00, 0x02, 0x00,
        0x07, 0x05, 0x01, 0x02, 0x00, 0x02, 0x00,
        0x08, 0x0b, 0x01, 0x02, 0x02, 0x02, 0x01, 0x00,
        0x09, 0x04, 0x01, 0x00, 0x01, 0x02, 0x02, 0x01, 0x00,
        0x05, 0x24, 0x00, 0x10, 0x01,
        0x05, 0x24, 0x01, 0x00, 0x02,
        0x04, 0x24, 0x02, 0x02,
        0x05, 0x24, 0x06, 0x01, 0x02,
        0x07, 0x05, 0x82, 0x03, 0x10, 0x00, 0x09,
        0x09, 0x04, 0x02, 0x00, 0x02, 0x0a, 0x00, 0x00, 0x00,
        0x07, 0x05, 0x02, 0x02, 0x00, 0x02, 0x00,
        0x07, 0x05, 0x83, 0x02, 0x00, 0x02, 0x00,
    };
    MsdDevice d = {
        .fault = FAULT_SYNCHRONIZE_CACHE,
        .fault_after = 2,
        .image_size = 8ULL * 1024 * 1024 * 1024,
    };
    CommandBlockWrapper cbw = { 0 };
    uint64_t lba;
    uint32_t blocks;
    bool fua;
    uint64_t parsed;
    MsdDevice opcode_device = { 0 };
    uint8_t persisted[SECTOR_SIZE] = { 0 };
    uint8_t payload[SECTOR_SIZE];
    uint8_t descriptors[EP0_MAX_DATA] = { 0 };
    size_t descriptor_length;
    char temporary[] = "/tmp/qemu-rpi-cm4-msd-XXXXXX";
    int temporary_fd;
    ssize_t got;

    if (sizeof(cbw) != 31 || sizeof(CommandStatusWrapper) != 13) {
        fail("BOT wrapper packing self-test failed");
    }
    descriptor_length = build_config(descriptors, sizeof(descriptors));
    if (descriptor_length != sizeof(expected_config) ||
        memcmp(descriptors, expected_config, sizeof(expected_config)) ||
        get_le16(device_descriptor.idVendor) != 0x0a5c ||
        get_le16(device_descriptor.idProduct) != 0x0104 ||
        descriptors[4] != 3 || descriptors[8] != 250 ||
        descriptors[9 + 5] != USB_CLASS_MASS_STORAGE ||
        descriptors[18 + 2] != 0x81 || descriptors[25 + 2] != 0x01 ||
        descriptors[32 + 1] != USB_DT_INTERFACE_ASSOCIATION ||
        descriptors[32 + 2] != 1 || descriptors[32 + 3] != 2 ||
        descriptors[40 + 5] != USB_CLASS_COMM ||
        descriptors[75 + 5] != USB_CLASS_CDC_DATA ||
        descriptors[84 + 2] != 0x02 || descriptors[91 + 2] != 0x83) {
        fail("official ACM+MSD descriptor self-test failed");
    }
    descriptor_length = build_string(descriptors, sizeof(descriptors), 2);
    if (descriptor_length !=
        2 + strlen("Raspberry Pi multi-function USB device") * 2 ||
        descriptors[2] != 'R' || descriptors[3] != 0) {
        fail("official product string self-test failed");
    }
    cbw.cdb[0] = SCSI_WRITE_10;
    cbw.cdb[1] = 0x08;
    put_be32(cbw.cdb + 2, 0x12345678);
    put_be16(cbw.cdb + 7, 0x42);
    if (!parse_rw(&cbw, &lba, &blocks, &fua) || lba != 0x12345678 ||
        blocks != 0x42 || !fua) {
        fail("READ/WRITE CDB parser self-test failed");
    }
    if (fault_matches(&d, SCSI_SYNCHRONIZE_CACHE_10, false) ||
        !fault_matches(&d, SCSI_SYNCHRONIZE_CACHE_16, false) ||
        !d.fault_active) {
        fail("fault sequence self-test failed");
    }
    if (!range_valid(&d, d.image_size / SECTOR_SIZE - 1, 1) ||
        range_valid(&d, d.image_size / SECTOR_SIZE, 1)) {
        fail("LBA range self-test failed");
    }
    if (parse_fault(&opcode_device, "opcode:0x00") != FAULT_OPCODE ||
        opcode_device.fault_opcode != SCSI_TEST_UNIT_READY) {
        fail("arbitrary opcode fault parser self-test failed");
    }
    if (!parse_unsigned("0xffffffffffffffff", UINT64_MAX, &parsed) ||
        parsed != UINT64_MAX ||
        parse_unsigned("18446744073709551616", UINT64_MAX, &parsed) ||
        parse_unsigned("0x", UINT64_MAX, &parsed) ||
        parse_unsigned("-1", UINT64_MAX, &parsed)) {
        fail("unsigned integer parser self-test failed");
    }
    memset(payload, 0xa5, sizeof(payload));
    temporary_fd = mkstemp(temporary);
    if (temporary_fd < 0) {
        fail_errno("create partial-write self-test backend");
    }
    unlink(temporary);
    if (ftruncate(temporary_fd, sizeof(persisted)) < 0) {
        fail_errno("size partial-write self-test backend");
    }
    durable_pwrite_exact(temporary_fd, payload, 127, 31,
                         "write partial-write self-test backend");
    got = pread(temporary_fd, persisted, sizeof(persisted), 0);
    if (got < 0) {
        fail_errno("read partial-write self-test backend");
    }
    if ((size_t)got != sizeof(persisted)) {
        fail("short partial-write self-test backend read");
    }
    close(temporary_fd);
    for (unsigned int i = 0; i < sizeof(persisted); i++) {
        uint8_t expected = i >= 31 && i < 31 + 127 ? 0xa5 : 0;

        if (persisted[i] != expected) {
            fail("sub-sector durable-prefix self-test failed");
        }
    }
    opcode_device.fault = FAULT_BOT_PHASE;
    opcode_device.fault_after = 2;
    opcode_device.fault_limit = 1;
    if (transport_fault_matches(&opcode_device, SCSI_TEST_UNIT_READY) ||
        !transport_fault_matches(&opcode_device, SCSI_TEST_UNIT_READY) ||
        transport_fault_matches(&opcode_device, SCSI_INQUIRY)) {
        fail("BOT phase fault sequence self-test failed");
    }
    opcode_device = (MsdDevice) {
        .fault = FAULT_BOT_TIMEOUT,
        .fault_after = 1,
        .fault_limit = 1,
    };
    if (!transport_fault_matches(&opcode_device, SCSI_TEST_UNIT_READY)) {
        fail("BOT timeout fault sequence self-test failed");
    }
    opcode_device.transport_faults_fired = 1;
    if (transport_fault_matches(&opcode_device, SCSI_TEST_UNIT_READY)) {
        fail("BOT timeout one-shot self-test failed");
    }
    opcode_device = (MsdDevice) {
        .fault = FAULT_BOT_DATA_RESET,
        .fault_after = 2,
        .fault_limit = 1,
    };
    if (data_reset_matches(&opcode_device, SCSI_READ_16) ||
        !data_reset_matches(&opcode_device, SCSI_READ_16) ||
        data_reset_matches(&opcode_device, SCSI_READ_10)) {
        fail("BOT data-reset fault sequence self-test failed");
    }
    opcode_device = (MsdDevice) {
        .fault = FAULT_BOT_WRITE_RESET,
        .fault_after = 1,
        .fault_limit = 1,
    };
    if (data_reset_matches(&opcode_device, SCSI_READ_16) ||
        !data_reset_matches(&opcode_device, SCSI_WRITE_16)) {
        fail("BOT write-reset fault sequence self-test failed");
    }
    opcode_device.transport_faults_fired = 1;
    if (data_reset_matches(&opcode_device, SCSI_WRITE_16)) {
        fail("BOT write-reset one-shot self-test failed");
    }
    opcode_device = (MsdDevice) {
        .fault = FAULT_BOT_DATA_RESET,
        .fault_after = 1,
        .fault_limit = 3,
    };
    for (unsigned int i = 0; i < 3; i++) {
        if (!data_reset_matches(&opcode_device, SCSI_READ_16)) {
            fail("BOT repeated data-reset fault did not arm");
        }
        opcode_device.transport_faults_fired++;
    }
    if (data_reset_matches(&opcode_device, SCSI_READ_16)) {
        fail("BOT repeated data-reset exceeded its limit");
    }
    puts("cm4-msd-raw-gadget self-test: PASS");
    return EXIT_SUCCESS;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s --image FILE [--lifecycle STATE_FILE] "
            "[--udc-driver dummy_udc] "
            "[--udc-device dummy_udc.0]\n"
            "          [--fault synchronize-cache|write-fua|bot-phase|"
            "bot-timeout|bot-data-reset|bot-write-reset|opcode:0xNN] "
            "[--fault-after COUNT] [--fault-count COUNT] "
            "[--fault-bytes BYTES] [--foreground-owner] [--verbose]\n"
            "       %s --self-test\n",
            program, program);
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        { "image", required_argument, NULL, 'i' },
        { "lifecycle", required_argument, NULL, 'l' },
        { "udc-driver", required_argument, NULL, 'd' },
        { "udc-device", required_argument, NULL, 'u' },
        { "fault", required_argument, NULL, 'f' },
        { "fault-after", required_argument, NULL, 'a' },
        { "fault-count", required_argument, NULL, 'c' },
        { "fault-bytes", required_argument, NULL, 'b' },
        { "foreground-owner", no_argument, NULL, 'o' },
        { "self-test", no_argument, NULL, 's' },
        { "verbose", no_argument, NULL, 'v' },
        { "help", no_argument, NULL, 'h' },
        { 0 }
    };
    MsdDevice device = {
        .udc_driver = "dummy_udc",
        .udc_device = "dummy_udc.0",
        .raw_fd = -1,
        .image_fd = -1,
        .lifecycle_lock_fd = -1,
        .bulk_in = -1,
        .bulk_out = -1,
        .acm_notify = -1,
        .acm_in = -1,
        .acm_out = -1,
        .fault_after = 1,
        .fault_bytes = SECTOR_SIZE,
        .fault_limit = 1,
        .line_coding = { 0x00, 0xc2, 0x01, 0x00, 0x00, 0x00, 0x08 },
    };
    const char *fault_value = NULL;
    bool fault_count_set = false;
    bool fault_bytes_set = false;
    bool run_self_test = false;
    int option;

    while ((option = getopt_long(argc, argv, "i:l:d:u:f:a:c:b:osvh", options,
                                 NULL)) != -1) {
        switch (option) {
        case 'i':
            device.image_path = optarg;
            break;
        case 'l':
            device.lifecycle = optarg;
            break;
        case 'd':
            device.udc_driver = optarg;
            break;
        case 'u':
            device.udc_device = optarg;
            break;
        case 'f':
            fault_value = optarg;
            break;
        case 'a':
            device.fault_after = parse_positive(optarg, "--fault-after");
            break;
        case 'c':
            device.fault_limit = parse_positive(optarg, "--fault-count");
            fault_count_set = true;
            break;
        case 'b': {
            uint64_t bytes = parse_positive(optarg, "--fault-bytes");

            if (bytes > BULK_CHUNK) {
                fail("--fault-bytes cannot exceed the 128 KiB bulk buffer");
            }
            device.fault_bytes = bytes;
            fault_bytes_set = true;
            break;
        }
        case 's':
            run_self_test = true;
            break;
        case 'o':
            device.foreground_owner = true;
            break;
        case 'v':
            device.verbose = true;
            break;
        default:
            usage(argv[0]);
        }
    }
    if (run_self_test) {
        if (device.image_path || device.lifecycle || fault_value ||
            fault_count_set || fault_bytes_set || device.foreground_owner ||
            optind != argc) {
            usage(argv[0]);
        }
        return self_test();
    }
    if (!device.image_path || optind != argc) {
        usage(argv[0]);
    }
    if (fault_value) {
        device.fault = parse_fault(&device, fault_value);
    } else if (device.fault_after != 1) {
        fail("--fault-after requires --fault");
    } else if (fault_count_set) {
        fail("--fault-count requires --fault");
    }
    if (device.fault_limit > 1 &&
        device.fault != FAULT_BOT_PHASE &&
        device.fault != FAULT_BOT_TIMEOUT &&
        device.fault != FAULT_BOT_DATA_RESET &&
        device.fault != FAULT_BOT_WRITE_RESET) {
        fail("--fault-count greater than one requires a BOT transport fault");
    }
    if (fault_bytes_set &&
        device.fault != FAULT_BOT_DATA_RESET &&
        device.fault != FAULT_BOT_WRITE_RESET) {
        fail("--fault-bytes requires bot-data-reset or bot-write-reset");
    }
    run_device(&device);
    return EXIT_SUCCESS;
}
