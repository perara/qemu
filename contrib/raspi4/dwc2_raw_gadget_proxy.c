/*
 * Linux Raw Gadget packet proxy for QEMU DWC2 device mode
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <linux/usb/ch9.h>
#include <linux/usb/raw_gadget.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "hw/usb/dwc2-device-transport.h"

#define EP0_CAPACITY 512
#define BULK_CAPACITY (16 * 1024)
#define TRANSPORT_TIMEOUT_MS 20000
#define RPIBOOT_HOST_TIMEOUT_MS 20000
#define DEFAULT_FAULT_TIMEOUT_MS 21000
#define RPIBOOT_BULK_TIMEOUT_MS 5000
#define DEFAULT_BULK_FAULT_TIMEOUT_MS 6000
#define GUEST_READY_TIMEOUT_MS 300000
#define GUEST_ENDPOINT_MAX 16
#define GUEST_CONFIG_CAPACITY EP0_CAPACITY

typedef enum ProxyTimeout {
    PROXY_TIMEOUT_NONE,
    PROXY_TIMEOUT_ROM_STATUS,
    PROXY_TIMEOUT_FILE_REQUEST,
} ProxyTimeout;

typedef struct ControlEvent {
    struct usb_raw_event event;
    struct usb_ctrlrequest ctrl;
} ControlEvent;

typedef struct ControlIO {
    struct usb_raw_ep_io io;
    uint8_t data[EP0_CAPACITY];
} ControlIO;

typedef struct BulkIO {
    struct usb_raw_ep_io io;
    uint8_t data[BULK_CAPACITY];
} BulkIO;

typedef struct Proxy Proxy;

typedef struct GuestEndpoint {
    Proxy *proxy;
    struct usb_endpoint_descriptor descriptor;
    int raw_ep;
    pthread_t thread;
    bool started;
    bool halted;
    uint64_t packets;
} GuestEndpoint;

struct Proxy {
    const char *socket_path;
    const char *udc_driver;
    const char *udc_device;
    int transport_fd;
    int raw_fd;
    int bulk_out;
    pthread_mutex_t transport_lock;
    pthread_mutex_t control_lock;
    pthread_mutex_t bulk_lock;
    pthread_cond_t bulk_cond;
    pthread_t bulk_thread;
    bool bulk_started;
    bool bulk_stop;
    bool configured;
    bool addressed;
    bool second_stage;
    bool stage_done;
    bool expect_bulk_announcement;
    bool reset_waited;
    bool disconnect_second_stage;
    bool hold_second_stage;
    bool bulk_timeout_second_stage;
    bool second_stage_only;
    bool continue_guest;
    bool guest_only;
    bool guest_stage;
    bool guest_configured;
    bool guest_stop;
    bool timeout_injected;
    uint32_t reset_after;
    uint32_t reset_count;
    uint32_t reset_completed;
    uint32_t disconnect_after;
    uint32_t hold_after;
    uint32_t bulk_timeout_after;
    uint32_t bulk_timeout_ms;
    uint32_t timeout_ms;
    ProxyTimeout timeout;
    uint32_t reset_progress;
    uint64_t bulk_forwarded;
    uint64_t bulk_announced;
    uint64_t reset_generation;
    uint8_t guest_config[GUEST_CONFIG_CAPACITY];
    uint32_t guest_config_length;
    uint32_t guest_config_expected;
    GuestEndpoint guest_endpoints[GUEST_ENDPOINT_MAX];
    uint32_t guest_endpoint_count;
};

static struct usb_endpoint_descriptor bulk_out_descriptor = {
    .bLength = USB_DT_ENDPOINT_SIZE,
    .bDescriptorType = USB_DT_ENDPOINT,
    .bEndpointAddress = USB_DIR_OUT | 1,
    .bmAttributes = USB_ENDPOINT_XFER_BULK,
    .wMaxPacketSize = __cpu_to_le16(512),
};

static __attribute__((noreturn)) void fail(const char *message)
{
    fprintf(stderr, "qemu-rpi-dwc2-proxy: %s\n", message);
    exit(EXIT_FAILURE);
}

static __attribute__((noreturn)) void fail_errno(const char *message)
{
    fprintf(stderr, "qemu-rpi-dwc2-proxy: %s: %s\n",
            message, strerror(errno));
    exit(EXIT_FAILURE);
}

static void put_le32(uint8_t *data, uint32_t value)
{
    data[0] = value;
    data[1] = value >> 8;
    data[2] = value >> 16;
    data[3] = value >> 24;
}

static uint32_t get_le32(const uint8_t *data)
{
    return data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static uint16_t get_le16(const uint8_t *data)
{
    return data[0] | ((uint16_t)data[1] << 8);
}

static bool parse_uint32(const char *text, uint32_t *value)
{
    uint64_t result = 0;

    if (!*text) {
        return false;
    }
    while (*text) {
        if (*text < '0' || *text > '9') {
            return false;
        }
        result = result * 10 + (*text++ - '0');
        if (result > UINT32_MAX) {
            return false;
        }
    }
    *value = result;
    return true;
}

static void sleep_milliseconds(uint32_t milliseconds)
{
    struct timespec delay = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000 * 1000,
    };

    while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {
    }
}

static void write_all(int fd, const void *data, size_t length)
{
    const uint8_t *bytes = data;
    size_t offset = 0;

    while (offset < length) {
        ssize_t written = write(fd, bytes + offset, length - offset);

        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            fail_errno("write DWC2 transport");
        }
        offset += written;
    }
}

static void read_all(int fd, void *data, size_t length)
{
    uint8_t *bytes = data;
    size_t offset = 0;

    while (offset < length) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int ready = poll(&pfd, 1, TRANSPORT_TIMEOUT_MS);
        ssize_t received;

        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready != 1) {
            fail(ready ? "DWC2 transport poll failed" :
                         "DWC2 transport timed out");
        }
        received = read(fd, bytes + offset, length - offset);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received <= 0) {
            fail_errno("read DWC2 transport");
        }
        offset += received;
    }
}

static int32_t transport_exchange(Proxy *p, uint8_t opcode, uint8_t ep,
                                  uint32_t value, const void *payload,
                                  uint32_t length, void *reply,
                                  uint32_t reply_capacity,
                                  uint32_t *reply_length)
{
    uint8_t request[DWC2_DEVICE_TRANSPORT_HEADER_SIZE] = { 0 };
    uint8_t response[DWC2_DEVICE_TRANSPORT_HEADER_SIZE];
    uint32_t response_length;
    int32_t result;

    put_le32(request, DWC2_DEVICE_TRANSPORT_MAGIC);
    request[4] = DWC2_DEVICE_TRANSPORT_VERSION;
    request[5] = opcode;
    request[6] = ep;
    put_le32(request + 8, length);
    put_le32(request + 12, value);

    pthread_mutex_lock(&p->transport_lock);
    write_all(p->transport_fd, request, sizeof(request));
    if (length) {
        write_all(p->transport_fd, payload, length);
    }
    read_all(p->transport_fd, response, sizeof(response));
    if (get_le32(response) != DWC2_DEVICE_TRANSPORT_MAGIC ||
        response[4] != DWC2_DEVICE_TRANSPORT_VERSION ||
        response[5] != (opcode | DWC2_DEVICE_TRANSPORT_RESPONSE) ||
        response[6] != ep) {
        pthread_mutex_unlock(&p->transport_lock);
        fail("invalid DWC2 transport response");
    }
    response_length = get_le32(response + 8);
    if (response_length > reply_capacity) {
        pthread_mutex_unlock(&p->transport_lock);
        fail("oversized DWC2 transport response");
    }
    if (response_length) {
        if (!reply) {
            pthread_mutex_unlock(&p->transport_lock);
            fail("unexpected DWC2 transport payload");
        }
        read_all(p->transport_fd, reply, response_length);
    }
    pthread_mutex_unlock(&p->transport_lock);
    if (reply_length) {
        *reply_length = response_length;
    }
    result = (int32_t)get_le32(response + 12);
    return result;
}

static int32_t transport_retry(Proxy *p, uint8_t opcode, uint8_t ep,
                               uint32_t value, const void *payload,
                               uint32_t length, void *reply,
                               uint32_t reply_capacity,
                               uint32_t *reply_length)
{
    unsigned int elapsed = 0;
    int32_t result;

    do {
        result = transport_exchange(p, opcode, ep, value, payload, length,
                                    reply, reply_capacity, reply_length);
        if (result != -EAGAIN) {
            return result;
        }
        usleep(1000);
    } while (++elapsed < TRANSPORT_TIMEOUT_MS);
    return -ETIMEDOUT;
}

static void transport_expect(Proxy *p, uint8_t opcode, uint32_t value)
{
    int32_t result = transport_retry(p, opcode, 0, value, NULL, 0,
                                     NULL, 0, NULL);

    if (result < 0) {
        errno = -result;
        fail_errno("DWC2 lifecycle request");
    }
}

static void wait_guest_state(Proxy *p, uint32_t required,
                             const char *waiting, const char *failure)
{
    fprintf(stderr, "qemu-rpi-dwc2-proxy: %s\n", waiting);
    fflush(stderr);
    for (unsigned int elapsed = 0;
         elapsed < GUEST_READY_TIMEOUT_MS; elapsed++) {
        int32_t state = transport_exchange(
            p, DWC2_DEVICE_TRANSPORT_STATE, 0, 0,
            NULL, 0, NULL, 0, NULL);

        if (state >= 0 && ((uint32_t)state & required) == required) {
            fprintf(stderr,
                    "qemu-rpi-dwc2-proxy: guest DWC2 is ready after %u ms\n",
                    elapsed);
            fflush(stderr);
            return;
        }
        if (state < 0 && state != -EAGAIN && state != -ENODEV) {
            errno = -state;
            fail_errno("query guest DWC2 readiness");
        }
        usleep(1000);
    }
    fail(failure);
}

static void wait_guest_ready(Proxy *p)
{
    wait_guest_state(
        p, DWC2_DEVICE_TRANSPORT_STATE_PULLUP |
           DWC2_DEVICE_TRANSPORT_STATE_EP0_SETUP,
        "waiting for guest DWC2 pull-up and initial EP0",
        "guest DWC2 did not assert pull-up and arm initial EP0");
}

static void wait_guest_ep0(Proxy *p)
{
    wait_guest_state(
        p, DWC2_DEVICE_TRANSPORT_STATE_PULLUP |
           DWC2_DEVICE_TRANSPORT_STATE_EP0_SETUP,
        "waiting for guest DWC2 EP0 after bus reset",
        "guest DWC2 did not arm EP0 after bus reset");
}

static int raw_ioctl(int fd, unsigned long request, void *arg,
                     const char *message)
{
    int result = ioctl(fd, request, arg);

    if (result < 0) {
        fail_errno(message);
    }
    return result;
}

static void *bulk_worker(void *opaque)
{
    Proxy *p = opaque;
    BulkIO io = { .io = { .flags = 0 } };

    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    for (;;) {
        uint64_t remaining;
        uint64_t generation;
        int received;

        pthread_mutex_lock(&p->bulk_lock);
        while (!p->bulk_stop &&
               p->bulk_forwarded == p->bulk_announced) {
            pthread_cond_wait(&p->bulk_cond, &p->bulk_lock);
        }
        if (p->bulk_stop) {
            pthread_mutex_unlock(&p->bulk_lock);
            return NULL;
        }
        if (p->reset_after && !p->reset_waited &&
            p->reset_progress == p->reset_after) {
            uint64_t wait_generation = p->reset_generation;

            p->reset_waited = true;
            fprintf(stderr,
                    "qemu-rpi-dwc2-proxy: waiting for host USB reset "
                    "during %s after %u bytes (cycle %u/%u)\n",
                    p->second_stage ? "file-server" : "ROM",
                    p->reset_after, p->reset_completed + 1,
                    p->reset_count);
            fflush(stderr);
            while (!p->bulk_stop &&
                   p->reset_generation == wait_generation) {
                pthread_cond_wait(&p->bulk_cond, &p->bulk_lock);
            }
            if (p->bulk_stop) {
                pthread_mutex_unlock(&p->bulk_lock);
                return NULL;
            }
            fprintf(stderr,
                    "qemu-rpi-dwc2-proxy: observed host USB reset "
                    "during %s (cycle %u/%u)\n",
                    p->second_stage ? "file-server" : "ROM",
                    p->reset_completed, p->reset_count);
            fflush(stderr);
            pthread_mutex_unlock(&p->bulk_lock);
            continue;
        }
        if (p->disconnect_after &&
            p->second_stage == p->disconnect_second_stage &&
            p->reset_progress == p->disconnect_after) {
            fprintf(stderr,
                    "qemu-rpi-dwc2-proxy: injected disconnect during %s "
                    "after %u bytes\n",
                    p->second_stage ? "file-server" : "ROM",
                    p->disconnect_after);
            fflush(stderr);
            _exit(75);
        }
        if (p->hold_after &&
            p->second_stage == p->hold_second_stage &&
            p->reset_progress == p->hold_after) {
            fprintf(stderr,
                    "qemu-rpi-dwc2-proxy: holding %s bulk transfer after "
                    "%u bytes for external termination\n",
                    p->second_stage ? "file-server" : "ROM",
                    p->hold_after);
            fflush(stderr);
            pthread_mutex_unlock(&p->bulk_lock);
            for (;;) {
                pause();
            }
        }
        if (p->bulk_timeout_after &&
            p->second_stage == p->bulk_timeout_second_stage &&
            p->reset_progress == p->bulk_timeout_after) {
            fprintf(stderr,
                    "qemu-rpi-dwc2-proxy: withholding %s bulk completion "
                    "for %u ms after %u bytes\n",
                    p->second_stage ? "file-server" : "ROM",
                    p->bulk_timeout_ms, p->bulk_timeout_after);
            fflush(stderr);
            pthread_mutex_unlock(&p->bulk_lock);
            sleep_milliseconds(p->bulk_timeout_ms);
            fprintf(stderr,
                    "qemu-rpi-dwc2-proxy: injected %s bulk timeout\n",
                    p->second_stage ? "file-server" : "ROM");
            fflush(stderr);
            _exit(75);
        }
        remaining = p->bulk_announced - p->bulk_forwarded;
        if (p->reset_after && !p->reset_waited &&
            p->reset_progress < p->reset_after &&
            remaining > p->reset_after - p->reset_progress) {
            remaining = p->reset_after - p->reset_progress;
        }
        if (p->disconnect_after &&
            p->second_stage == p->disconnect_second_stage &&
            p->reset_progress < p->disconnect_after &&
            remaining > p->disconnect_after - p->reset_progress) {
            remaining = p->disconnect_after - p->reset_progress;
        }
        if (p->hold_after &&
            p->second_stage == p->hold_second_stage &&
            p->reset_progress < p->hold_after &&
            remaining > p->hold_after - p->reset_progress) {
            remaining = p->hold_after - p->reset_progress;
        }
        if (p->bulk_timeout_after &&
            p->second_stage == p->bulk_timeout_second_stage &&
            p->reset_progress < p->bulk_timeout_after &&
            remaining > p->bulk_timeout_after - p->reset_progress) {
            remaining = p->bulk_timeout_after - p->reset_progress;
        }
        generation = p->reset_generation;
        pthread_mutex_unlock(&p->bulk_lock);

        io.io.ep = p->bulk_out;
        io.io.length = remaining < sizeof(io.data) ?
                       remaining : sizeof(io.data);
        /*
         * Raw Gadget has no nonblocking endpoint-read mode.  Make only this
         * lock-free ioctl cancellable so stage teardown can always wake it.
         */
        pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
        received = ioctl(p->raw_fd, USB_RAW_IOCTL_EP_READ, &io);
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
        if (received < 0) {
            int error = errno;
            bool stop;

            pthread_mutex_lock(&p->bulk_lock);
            stop = p->bulk_stop;
            pthread_mutex_unlock(&p->bulk_lock);
            if (stop || error == EBADF) {
                return NULL;
            }
            if (error == ESHUTDOWN || error == ECONNRESET) {
                continue;
            }
            errno = error;
            fail_errno("read Raw Gadget bulk OUT");
        }
        for (int offset = 0; offset < received; ) {
            uint32_t chunk = received - offset;
            int32_t result;
            bool current;

            if (chunk > 512) {
                chunk = 512;
            }
            pthread_mutex_lock(&p->bulk_lock);
            current = generation == p->reset_generation;
            pthread_mutex_unlock(&p->bulk_lock);
            if (!current) {
                break;
            }
            result = transport_retry(
                p, DWC2_DEVICE_TRANSPORT_OUT, 1, 0,
                io.data + offset, chunk, NULL, 0, NULL);
            if (result != (int32_t)chunk) {
                if (result < 0) {
                    errno = -result;
                }
                fail_errno("forward Raw Gadget bulk OUT");
            }
            offset += chunk;
        }
        pthread_mutex_lock(&p->bulk_lock);
        if (generation != p->reset_generation) {
            pthread_mutex_unlock(&p->bulk_lock);
            continue;
        }
        if (!p->second_stage ||
            (p->bulk_forwarded >> 20) !=
            ((p->bulk_forwarded + received) >> 20) ||
            received == 24) {
            fprintf(stderr, "qemu-rpi-dwc2-proxy: %s bulk forwarded: %"
                    PRIu64 " bytes\n",
                    p->second_stage ? "file-server" : "ROM",
                    p->bulk_forwarded + received);
        }
        p->bulk_forwarded += received;
        p->reset_progress += received;
        pthread_cond_broadcast(&p->bulk_cond);
        pthread_mutex_unlock(&p->bulk_lock);
    }
    return NULL;
}

static GuestEndpoint *guest_endpoint_by_address(Proxy *p, uint8_t address)
{
    for (uint32_t i = 0; i < p->guest_endpoint_count; i++) {
        if (p->guest_endpoints[i].descriptor.bEndpointAddress == address) {
            return &p->guest_endpoints[i];
        }
    }
    return NULL;
}

static bool guest_endpoint_set_matches(
    Proxy *p, const struct usb_endpoint_descriptor *descriptors,
    uint32_t endpoint_count)
{
    if (p->guest_endpoint_count != endpoint_count) {
        return false;
    }
    for (uint32_t i = 0; i < endpoint_count; i++) {
        if (memcmp(&p->guest_endpoints[i].descriptor, &descriptors[i],
                   USB_DT_ENDPOINT_SIZE)) {
            return false;
        }
    }
    return true;
}

static void disable_guest_endpoints(Proxy *p, bool bump_generation,
                                    bool cancel_workers)
{
    pthread_mutex_lock(&p->bulk_lock);
    p->guest_configured = false;
    p->guest_stop = true;
    if (bump_generation) {
        p->reset_generation++;
    }
    pthread_cond_broadcast(&p->bulk_cond);
    pthread_mutex_unlock(&p->bulk_lock);

    for (uint32_t i = 0; i < p->guest_endpoint_count; i++) {
        GuestEndpoint *endpoint = &p->guest_endpoints[i];

        if (endpoint->started) {
            /*
             * A bus reset completes queued Raw Gadget I/O with ESHUTDOWN.
             * Let that completion unwind naturally so raw-gadget clears its
             * internal urb_queued flag before EP_DISABLE.  Cancellation is
             * needed only for deconfiguration/replacement without a reset.
             */
            if (cancel_workers) {
                pthread_cancel(endpoint->thread);
            }
            pthread_join(endpoint->thread, NULL);
            endpoint->started = false;
        }
        if (p->raw_fd >= 0 && endpoint->raw_ep >= 0) {
            uint32_t raw_ep = endpoint->raw_ep;

            if (ioctl(p->raw_fd, USB_RAW_IOCTL_EP_DISABLE, raw_ep) < 0 &&
                errno != ESHUTDOWN && errno != ENODEV) {
                fail_errno("disable Raw Gadget guest endpoint");
            }
        }
        endpoint->raw_ep = -1;
    }

    pthread_mutex_lock(&p->bulk_lock);
    p->guest_stop = false;
    pthread_mutex_unlock(&p->bulk_lock);
}

static void replace_guest_endpoints(
    Proxy *p, const struct usb_endpoint_descriptor *descriptors,
    uint32_t endpoint_count)
{
    /*
     * Raw Gadget retains enabled endpoints across a USB bus reset.  Stop the
     * old workers and release those handles only when Linux actually exposes
     * a different descriptor set.  Re-parsing an identical configuration
     * must leave both handles and workers intact.
     */
    if (p->guest_endpoint_count) {
        disable_guest_endpoints(p, true, true);
    }

    memset(p->guest_endpoints, 0, sizeof(p->guest_endpoints));
    p->guest_endpoint_count = endpoint_count;
    for (uint32_t i = 0; i < endpoint_count; i++) {
        GuestEndpoint *endpoint = &p->guest_endpoints[i];

        endpoint->proxy = p;
        endpoint->descriptor = descriptors[i];
        endpoint->raw_ep = -1;
    }
}

static bool parse_guest_configuration(Proxy *p)
{
    struct usb_endpoint_descriptor descriptors[GUEST_ENDPOINT_MAX] = { 0 };
    uint32_t offset = 0;
    uint32_t endpoint_count = 0;

    if (p->guest_config_length < USB_DT_CONFIG_SIZE ||
        p->guest_config[0] != USB_DT_CONFIG_SIZE ||
        p->guest_config[1] != USB_DT_CONFIG ||
        get_le16(p->guest_config + 2) != p->guest_config_length) {
        return false;
    }
    while (offset + 2 <= p->guest_config_length) {
        uint8_t length = p->guest_config[offset];
        uint8_t type = p->guest_config[offset + 1];

        if (length < 2 || length > p->guest_config_length - offset) {
            return false;
        }
        if (type == USB_DT_ENDPOINT) {
            uint8_t address;

            if (length < USB_DT_ENDPOINT_SIZE ||
                endpoint_count == GUEST_ENDPOINT_MAX) {
                return false;
            }
            address = p->guest_config[offset + 2];
            if (!(address & USB_ENDPOINT_NUMBER_MASK) ||
                (address & USB_ENDPOINT_NUMBER_MASK) >= 16) {
                return false;
            }
            for (uint32_t i = 0; i < endpoint_count; i++) {
                if (descriptors[i].bEndpointAddress == address) {
                    return false;
                }
            }
            memcpy(&descriptors[endpoint_count++],
                   p->guest_config + offset,
                   USB_DT_ENDPOINT_SIZE);
        }
        offset += length;
    }
    if (offset != p->guest_config_length || !endpoint_count) {
        return false;
    }
    if (!guest_endpoint_set_matches(p, descriptors, endpoint_count)) {
        replace_guest_endpoints(p, descriptors, endpoint_count);
    }
    return true;
}

static bool guest_endpoint_wait_configured(GuestEndpoint *endpoint,
                                           uint64_t *generation)
{
    Proxy *p = endpoint->proxy;

    pthread_mutex_lock(&p->bulk_lock);
    while (!p->guest_stop && !p->guest_configured) {
        pthread_cond_wait(&p->bulk_cond, &p->bulk_lock);
    }
    *generation = p->reset_generation;
    pthread_mutex_unlock(&p->bulk_lock);
    return !p->guest_stop;
}

static bool guest_endpoint_generation_current(GuestEndpoint *endpoint,
                                              uint64_t generation)
{
    Proxy *p = endpoint->proxy;
    bool current;

    pthread_mutex_lock(&p->bulk_lock);
    current = p->guest_configured &&
              p->reset_generation == generation && !p->guest_stop;
    pthread_mutex_unlock(&p->bulk_lock);
    return current;
}

static void synthesize_guest_clear_halt(GuestEndpoint *endpoint,
                                        uint64_t generation)
{
    Proxy *p = endpoint->proxy;
    struct usb_ctrlrequest clear = {
        .bRequestType = USB_DIR_OUT | USB_TYPE_STANDARD |
                        USB_RECIP_ENDPOINT,
        .bRequest = USB_REQ_CLEAR_FEATURE,
        .wValue = htole16(USB_ENDPOINT_HALT),
        .wIndex = htole16(endpoint->descriptor.bEndpointAddress),
    };
    uint8_t response[EP0_CAPACITY];
    uint32_t received = 0;
    int32_t result;
    int lock_result;

    while ((lock_result = pthread_mutex_trylock(&p->control_lock)) ==
           EBUSY) {
        if (!guest_endpoint_generation_current(endpoint, generation)) {
            return;
        }
        usleep(1000);
    }
    if (lock_result) {
        errno = lock_result;
        fail_errno("lock DWC2 control transaction");
    }
    if (!guest_endpoint_generation_current(endpoint, generation)) {
        pthread_mutex_unlock(&p->control_lock);
        return;
    }
    result = transport_retry(
        p, DWC2_DEVICE_TRANSPORT_SETUP, 0, 0,
        &clear, sizeof(clear), NULL, 0, NULL);
    if (result != sizeof(clear)) {
        pthread_mutex_unlock(&p->control_lock);
        fail("DWC2 rejected synthesized endpoint clear-halt setup");
    }
    result = transport_retry(
        p, DWC2_DEVICE_TRANSPORT_IN, 0, 64,
        NULL, 0, response, sizeof(response), &received);
    pthread_mutex_unlock(&p->control_lock);
    if (result < 0 || result != (int32_t)received || received) {
        fail("DWC2 rejected synthesized endpoint clear-halt status");
    }
    fprintf(stderr,
            "qemu-rpi-dwc2-proxy: synthesized hidden dummy_hcd"
            " CLEAR_FEATURE for guest EP%u %s\n",
            endpoint->descriptor.bEndpointAddress &
                USB_ENDPOINT_NUMBER_MASK,
            endpoint->descriptor.bEndpointAddress & USB_DIR_IN ?
                "IN" : "OUT");
}

static void guest_endpoint_halt_and_wait(GuestEndpoint *endpoint,
                                         uint64_t generation)
{
    Proxy *p = endpoint->proxy;
    uint32_t raw_ep = endpoint->raw_ep;
    unsigned int attempt;

    pthread_mutex_lock(&p->bulk_lock);
    if (!p->guest_configured || p->reset_generation != generation ||
        p->guest_stop) {
        pthread_mutex_unlock(&p->bulk_lock);
        return;
    }
    endpoint->halted = true;
    pthread_mutex_unlock(&p->bulk_lock);

    fprintf(stderr,
            "qemu-rpi-dwc2-proxy: guest EP%u %s halt raw_ep=%u\n",
            endpoint->descriptor.bEndpointAddress &
                USB_ENDPOINT_NUMBER_MASK,
            endpoint->descriptor.bEndpointAddress & USB_DIR_IN ?
                "IN" : "OUT",
            raw_ep);
    if (endpoint->descriptor.bEndpointAddress & USB_DIR_IN) {
        /*
         * EP_WRITE returns just before dummy_hcd retires the host URB.  The
         * guest can already have armed the following BOT status stall at
         * that point.  Let the completed short data packet retire before
         * applying the halt, or the halt can fail the data URB retroactively.
         */
        usleep(10000);
    }
    for (attempt = 0; attempt < 1000; attempt++) {
        if (ioctl(p->raw_fd, USB_RAW_IOCTL_EP_SET_HALT, raw_ep) == 0) {
            break;
        }
        if (errno == ESHUTDOWN || errno == ECONNRESET) {
            break;
        }
        if (errno != EBUSY && errno != EAGAIN && errno != EINVAL) {
            fail_errno("halt Raw Gadget guest endpoint");
        }
        /*
         * dummy_hcd can retain a <=64-byte IN request in its emulated FIFO
         * after Raw Gadget has completed EP_WRITE.  Back off long enough for
         * the host-side timer to retire that packet before retrying halt.
         */
        usleep(10000);
    }
    if (attempt == 1000) {
        fail_errno("Raw Gadget guest endpoint halt remained busy");
    }
    if (attempt) {
        fprintf(stderr,
                "qemu-rpi-dwc2-proxy: guest EP%u %s halt completed"
                " after %u retries\n",
                endpoint->descriptor.bEndpointAddress &
                    USB_ENDPOINT_NUMBER_MASK,
                endpoint->descriptor.bEndpointAddress & USB_DIR_IN ?
                    "IN" : "OUT",
                attempt);
    }

    /*
     * dummy_hcd consumes endpoint CLEAR_FEATURE internally rather than
     * forwarding it to Raw Gadget.  Preserve the physical STALL/clear
     * exchange, then mirror that hidden control request into the guest DWC2.
     * The following Raw write still cannot pass the halted dummy endpoint
     * until the host has actually cleared it.
     */
    if (!strcmp(p->udc_driver, "dummy_udc")) {
        usleep(10000);
        synthesize_guest_clear_halt(endpoint, generation);
        pthread_mutex_lock(&p->bulk_lock);
        endpoint->halted = false;
        pthread_cond_broadcast(&p->bulk_cond);
        pthread_mutex_unlock(&p->bulk_lock);
    }

    pthread_mutex_lock(&p->bulk_lock);
    while (endpoint->halted && p->guest_configured &&
           p->reset_generation == generation && !p->guest_stop) {
        pthread_cond_wait(&p->bulk_cond, &p->bulk_lock);
    }
    pthread_mutex_unlock(&p->bulk_lock);
}

static void *guest_out_worker(void *opaque)
{
    GuestEndpoint *endpoint = opaque;
    Proxy *p = endpoint->proxy;
    BulkIO io = { .io = { .flags = 0 } };
    uint8_t ep = endpoint->descriptor.bEndpointAddress &
                 USB_ENDPOINT_NUMBER_MASK;
    uint32_t mps = le16toh(endpoint->descriptor.wMaxPacketSize) & 0x7ff;
    uint32_t bot_out_remaining = 0;
    uint64_t bot_generation = UINT64_MAX;

    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    for (;;) {
        uint64_t generation;
        bool bot_data;
        int received;

        if (!guest_endpoint_wait_configured(endpoint, &generation)) {
            return NULL;
        }
        if (generation != bot_generation) {
            bot_out_remaining = 0;
            bot_generation = generation;
        }
        io.io.ep = endpoint->raw_ep;
        if (ep == 1 && bot_out_remaining) {
            io.io.length = bot_out_remaining < sizeof(io.data) ?
                           bot_out_remaining : sizeof(io.data);
        } else {
            /*
             * A Raw Gadget OUT request larger than a packet-aligned host
             * transfer cannot complete: there is no short packet at the URB
             * boundary.  BOT supplies its exact data length in the CBW; use
             * one packet for other endpoint protocols.
             */
            io.io.length = ep == 1 ? sizeof(io.data) : mps;
        }
        bot_data = ep == 1 && bot_out_remaining;
        pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
        received = ioctl(p->raw_fd, USB_RAW_IOCTL_EP_READ, &io);
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
        if (received < 0) {
            if (errno == EINTR || errno == ESHUTDOWN ||
                errno == ECONNRESET || errno == EBUSY) {
                usleep(1000);
                continue;
            }
            fail_errno("read Raw Gadget guest OUT endpoint");
        }
        if (bot_data) {
            if ((uint32_t)received > bot_out_remaining) {
                fail("Raw Gadget BOT OUT exceeded announced transfer");
            }
            bot_out_remaining -= received;
            if (!bot_out_remaining && endpoint->packets <= 256) {
                fprintf(stderr,
                        "qemu-rpi-dwc2-proxy: guest BOT OUT data"
                        " transfer complete\n");
            }
        } else if (ep == 1 && received >= 31 &&
                   get_le32(io.data) == 0x43425355 &&
                   !(io.data[12] & USB_DIR_IN)) {
            bot_out_remaining = get_le32(io.data + 8);
        }
        endpoint->packets++;
        if (!bot_data && endpoint->packets <= 256 &&
            ep == 1 && received >= 31 &&
            get_le32(io.data) == 0x43425355) {
            fprintf(stderr,
                    "qemu-rpi-dwc2-proxy: guest BOT CBW #%" PRIu64
                    " tag=0x%08x bytes=%u flags=0x%02x op=0x%02x\n",
                    endpoint->packets, get_le32(io.data + 4),
                    get_le32(io.data + 8), io.data[12], io.data[15]);
        } else if (endpoint->packets <= 32) {
            fprintf(stderr,
                    "qemu-rpi-dwc2-proxy: guest EP%u OUT #%" PRIu64
                    " length=%d\n", ep, endpoint->packets, received);
        }
        for (int offset = 0; offset < received; ) {
            uint32_t chunk = received - offset < (int)mps ?
                             received - offset : mps;
            int32_t result;

            if (!guest_endpoint_generation_current(endpoint, generation)) {
                break;
            }
            result = transport_exchange(
                p, DWC2_DEVICE_TRANSPORT_OUT, ep, 0,
                io.data + offset, chunk, NULL, 0, NULL);
            if (result == -EAGAIN) {
                usleep(1000);
                continue;
            }
            if (result == -EPIPE) {
                guest_endpoint_halt_and_wait(endpoint, generation);
                break;
            }
            if (result == -ENODEV &&
                !guest_endpoint_generation_current(endpoint, generation)) {
                break;
            }
            if (result != (int32_t)chunk) {
                if (result < 0) {
                    errno = -result;
                }
                fail_errno("forward Raw Gadget guest OUT endpoint");
            }
            offset += chunk;
        }
    }
}

static void *guest_in_worker(void *opaque)
{
    GuestEndpoint *endpoint = opaque;
    Proxy *p = endpoint->proxy;
    BulkIO io = { .io = { .flags = 0 } };
    uint8_t ep = endpoint->descriptor.bEndpointAddress &
                 USB_ENDPOINT_NUMBER_MASK;
    uint32_t mps = le16toh(endpoint->descriptor.wMaxPacketSize) & 0x7ff;

    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    for (;;) {
        uint64_t generation;
        uint32_t received = 0;
        int32_t result;
        int written;

        if (!guest_endpoint_wait_configured(endpoint, &generation)) {
            return NULL;
        }
        result = transport_exchange(
            p, DWC2_DEVICE_TRANSPORT_IN, ep, mps, NULL, 0,
            io.data, sizeof(io.data), &received);
        if (result == -EAGAIN || result == -ENODEV) {
            usleep(1000);
            continue;
        }
        if (result == -EPIPE) {
            guest_endpoint_halt_and_wait(endpoint, generation);
            continue;
        }
        if (result < 0 || result != (int32_t)received ||
            received > mps) {
            if (result < 0) {
                errno = -result;
            }
            fail_errno("receive DWC2 guest IN endpoint");
        }
        endpoint->packets++;
        if (endpoint->packets <= 256 && ep == 1 && received == 13 &&
            get_le32(io.data) == 0x53425355) {
            fprintf(stderr,
                    "qemu-rpi-dwc2-proxy: guest BOT CSW #%" PRIu64
                    " tag=0x%08x residue=%u status=%u\n",
                    endpoint->packets, get_le32(io.data + 4),
                    get_le32(io.data + 8), io.data[12]);
        } else if (endpoint->packets <= 64) {
            fprintf(stderr,
                    "qemu-rpi-dwc2-proxy: guest EP%u IN #%" PRIu64
                    " length=%u\n", ep, endpoint->packets, received);
        }
        if (!guest_endpoint_generation_current(endpoint, generation)) {
            continue;
        }
        io.io.ep = endpoint->raw_ep;
        io.io.length = received;
        pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
        written = ioctl(p->raw_fd, USB_RAW_IOCTL_EP_WRITE, &io);
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
        if (endpoint->packets <= 64) {
            fprintf(stderr,
                    "qemu-rpi-dwc2-proxy: guest EP%u IN #%" PRIu64
                    " raw write completed=%d errno=%d\n",
                    ep, endpoint->packets, written,
                    written < 0 ? errno : 0);
        }
        if (written < 0) {
            if (errno == EINTR || errno == ESHUTDOWN ||
                errno == ECONNRESET || errno == EBUSY) {
                usleep(1000);
                continue;
            }
            fail_errno("write Raw Gadget guest IN endpoint");
        }
        if (written != (int)received) {
            fail("short Raw Gadget guest IN endpoint write");
        }
    }
}

static void enable_guest_endpoints(Proxy *p)
{
    uint32_t power;

    if (!p->guest_endpoint_count &&
        !parse_guest_configuration(p)) {
        fail("guest configuration has no usable endpoint set");
    }
    for (uint32_t i = 0; i < p->guest_endpoint_count; i++) {
        GuestEndpoint *endpoint = &p->guest_endpoints[i];

        if (endpoint->raw_ep < 0) {
            endpoint->raw_ep = raw_ioctl(
                p->raw_fd, USB_RAW_IOCTL_EP_ENABLE,
                &endpoint->descriptor,
                "enable Raw Gadget guest endpoint");
        }
    }
    power = p->guest_config[8];
    raw_ioctl(p->raw_fd, USB_RAW_IOCTL_VBUS_DRAW, &power,
              "set Raw Gadget guest power");
    raw_ioctl(p->raw_fd, USB_RAW_IOCTL_CONFIGURE, NULL,
              "configure Raw Gadget guest");
    pthread_mutex_lock(&p->bulk_lock);
    p->guest_configured = true;
    pthread_cond_broadcast(&p->bulk_cond);
    pthread_mutex_unlock(&p->bulk_lock);
    for (uint32_t i = 0; i < p->guest_endpoint_count; i++) {
        GuestEndpoint *endpoint = &p->guest_endpoints[i];
        int error;

        if (endpoint->started) {
            continue;
        }
        error = pthread_create(
            &endpoint->thread, NULL,
            endpoint->descriptor.bEndpointAddress & USB_DIR_IN ?
                guest_in_worker : guest_out_worker,
            endpoint);
        if (error) {
            errno = error;
            fail_errno("start Raw Gadget guest endpoint worker");
        }
        endpoint->started = true;
    }
    printf("guest Linux USB gadget configured with %u endpoints\n",
           p->guest_endpoint_count);
    fflush(stdout);
}

static void enable_bulk(Proxy *p)
{
    uint32_t power = 1;
    int error;

    if (p->configured) {
        return;
    }
    if (!p->bulk_started) {
        p->bulk_out = raw_ioctl(
            p->raw_fd, USB_RAW_IOCTL_EP_ENABLE, &bulk_out_descriptor,
            "enable Raw Gadget bulk OUT");
    }
    raw_ioctl(p->raw_fd, USB_RAW_IOCTL_VBUS_DRAW, &power,
              "set Raw Gadget power");
    raw_ioctl(p->raw_fd, USB_RAW_IOCTL_CONFIGURE, NULL,
              "configure Raw Gadget");
    p->configured = true;
    if (!p->bulk_started) {
        error = pthread_create(&p->bulk_thread, NULL, bulk_worker, p);
        if (error) {
            errno = error;
            fail_errno("start Raw Gadget bulk proxy");
        }
        p->bulk_started = true;
    }
}

static void raw_ep0_write(Proxy *p, const void *data, uint32_t length)
{
    ControlIO io = { .io = { .ep = 0, .flags = 0, .length = length } };

    if (length > sizeof(io.data)) {
        fail("Raw Gadget EP0 write exceeds bridge capacity");
    }
    if (length) {
        memcpy(io.data, data, length);
    }
    raw_ioctl(p->raw_fd, USB_RAW_IOCTL_EP0_WRITE, &io,
              "write Raw Gadget EP0");
}

static uint32_t raw_ep0_read(Proxy *p, void *data, uint32_t length)
{
    ControlIO io = { .io = { .ep = 0, .flags = 0, .length = length } };
    int received;

    if (length > sizeof(io.data)) {
        fail("Raw Gadget EP0 read exceeds bridge capacity");
    }
    received = raw_ioctl(p->raw_fd, USB_RAW_IOCTL_EP0_READ, &io,
                         "read Raw Gadget EP0");

    if ((uint32_t)received > length) {
        fail("oversized Raw Gadget EP0 payload");
    }
    if (received) {
        memcpy(data, io.data, received);
    }
    return received;
}

static bool ep0_length_supported(uint32_t length)
{
    return length <= EP0_CAPACITY;
}

static bool needs_synthesized_address(
    const Proxy *p, const struct usb_ctrlrequest *ctrl)
{
    return !p->addressed &&
           (ctrl->bRequestType & (USB_DIR_IN | USB_TYPE_MASK |
                                  USB_RECIP_MASK)) ==
               (USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE) &&
           ctrl->bRequest == USB_REQ_SET_CONFIGURATION &&
           le16toh(ctrl->wValue);
}

static void handle_control_inner(Proxy *p,
                                 const struct usb_ctrlrequest *ctrl)
{
    uint8_t response[EP0_CAPACITY] = { 0 };
    uint16_t host_length = le16toh(ctrl->wLength);
    uint32_t total = 0;
    int32_t result;
    bool vendor = (ctrl->bRequestType & USB_TYPE_MASK) == USB_TYPE_VENDOR;

    if (!ep0_length_supported(host_length)) {
        raw_ioctl(p->raw_fd, USB_RAW_IOCTL_EP0_STALL, NULL,
                  "stall oversized Raw Gadget EP0 request");
        return;
    }

    /*
     * Raw Gadget consumes the physical host's SET_ADDRESS request inside the
     * kernel and does not expose a control event for it.  Mirror that hidden
     * protocol stage into the modeled DWC2 device before forwarding the first
     * nonzero SET_CONFIGURATION.  The behavioral ROM can therefore keep its
     * correct rule that configuration at USB address zero is invalid.
     */
    if (needs_synthesized_address(p, ctrl)) {
        struct usb_ctrlrequest address = {
            .bRequestType = USB_DIR_OUT | USB_TYPE_STANDARD |
                            USB_RECIP_DEVICE,
            .bRequest = USB_REQ_SET_ADDRESS,
            .wValue = htole16(1),
        };
        uint32_t received = 0;

        result = transport_retry(
            p, DWC2_DEVICE_TRANSPORT_SETUP, 0, 0,
            &address, sizeof(address), NULL, 0, NULL);
        if (result != sizeof(address)) {
            fail("DWC2 rejected synthesized SET_ADDRESS setup");
        }
        result = transport_retry(
            p, DWC2_DEVICE_TRANSPORT_IN, 0, 64, NULL, 0,
            response, sizeof(response), &received);
        if (result < 0 || result != (int32_t)received || received) {
            fail("DWC2 rejected synthesized SET_ADDRESS status");
        }
        p->addressed = true;
    }
    if (vendor) {
        fprintf(stderr,
                "qemu-rpi-dwc2-proxy: %s vendor %s value=%u index=%u "
                "length=%u\n",
                p->second_stage ? "file-server" : "ROM",
                ctrl->bRequestType & USB_DIR_IN ? "IN" : "OUT",
                le16toh(ctrl->wValue), le16toh(ctrl->wIndex), host_length);
    }
    if (vendor && (ctrl->bRequestType & USB_DIR_IN)) {
        pthread_mutex_lock(&p->bulk_lock);
        while (p->bulk_forwarded < p->bulk_announced) {
            pthread_cond_wait(&p->bulk_cond, &p->bulk_lock);
        }
        pthread_mutex_unlock(&p->bulk_lock);
    }
    if (!p->timeout_injected && vendor &&
        (ctrl->bRequestType & USB_DIR_IN) &&
        ((p->timeout == PROXY_TIMEOUT_ROM_STATUS &&
          !p->second_stage && host_length == 4) ||
         (p->timeout == PROXY_TIMEOUT_FILE_REQUEST &&
          p->second_stage && host_length == 260))) {
        p->timeout_injected = true;
        fprintf(stderr,
                "qemu-rpi-dwc2-proxy: withholding %s control reply for "
                "%u ms\n",
                p->second_stage ? "file-server" : "ROM-status",
                p->timeout_ms);
        fflush(stderr);
        sleep_milliseconds(p->timeout_ms);
        fprintf(stderr,
                "qemu-rpi-dwc2-proxy: injected %s control timeout\n",
                p->second_stage ? "file-server" : "ROM-status");
        fflush(stderr);
        _exit(75);
    }
    result = transport_retry(p, DWC2_DEVICE_TRANSPORT_SETUP, 0, 0,
                             ctrl, sizeof(*ctrl), NULL, 0, NULL);
    if (result != sizeof(*ctrl)) {
        if (p->guest_stage) {
            fprintf(stderr,
                    "qemu-rpi-dwc2-proxy: guest control rejected "
                    "type=0x%02x request=0x%02x value=0x%04x "
                    "index=0x%04x length=%u result=%d\n",
                    ctrl->bRequestType, ctrl->bRequest,
                    le16toh(ctrl->wValue), le16toh(ctrl->wIndex),
                    host_length, result);
        }
        raw_ioctl(p->raw_fd, USB_RAW_IOCTL_EP0_STALL, NULL,
                  "stall Raw Gadget EP0");
        return;
    }

    if (ctrl->bRequestType & USB_DIR_IN) {
        while (total < host_length) {
            uint32_t received = 0;

            result = transport_retry(
                p, DWC2_DEVICE_TRANSPORT_IN, 0, 64, NULL, 0,
                response + total, sizeof(response) - total, &received);
            if (result < 0 || result != (int32_t)received) {
                raw_ioctl(p->raw_fd, USB_RAW_IOCTL_EP0_STALL, NULL,
                          "stall Raw Gadget EP0 IN");
                return;
            }
            total += received;
            if (received < 64) {
                break;
            }
        }
        if (p->guest_stage &&
            (ctrl->bRequestType &
             (USB_DIR_IN | USB_TYPE_MASK | USB_RECIP_MASK)) ==
                (USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE) &&
            ctrl->bRequest == USB_REQ_GET_DESCRIPTOR &&
            (le16toh(ctrl->wValue) >> 8) == USB_DT_CONFIG &&
            total >= USB_DT_CONFIG_SIZE &&
            total <= sizeof(p->guest_config)) {
            uint32_t expected = get_le16(response + 2);

            if (expected >= USB_DT_CONFIG_SIZE &&
                expected <= sizeof(p->guest_config) &&
                total <= expected) {
                memcpy(p->guest_config, response, total);
                p->guest_config_length = total;
                p->guest_config_expected = expected;
                if (total == expected) {
                    if (!parse_guest_configuration(p)) {
                        fail("invalid guest USB configuration descriptor");
                    }
                }
            }
        }
        raw_ep0_write(p, response, total);
        result = transport_retry(p, DWC2_DEVICE_TRANSPORT_OUT, 0, 0,
                                 NULL, 0, NULL, 0, NULL);
        if (result < 0) {
            fail("DWC2 rejected EP0 status OUT");
        }
        if (!p->second_stage && vendor && host_length == 4) {
            p->stage_done = true;
        } else if (p->second_stage && total == 260 &&
                   get_le32(response) == 2) {
            p->stage_done = true;
        } else if (p->second_stage && total == 260 &&
                   get_le32(response) == 1) {
            p->expect_bulk_announcement = true;
        }
    } else {
        if (host_length) {
            total = raw_ep0_read(p, response, host_length);
            result = transport_retry(
                p, DWC2_DEVICE_TRANSPORT_OUT, 0, 0,
                response, total, NULL, 0, NULL);
            if (result >= 0) {
                result = transport_retry(
                    p, DWC2_DEVICE_TRANSPORT_IN, 0, 64,
                    NULL, 0, response, sizeof(response), &total);
            }
        } else {
            result = transport_retry(
                p, DWC2_DEVICE_TRANSPORT_IN, 0, 64,
                NULL, 0, response, sizeof(response), &total);
            raw_ep0_read(p, response, 0);
        }
        if (result < 0) {
            fail("DWC2 rejected EP0 OUT request");
        }
        if (vendor && !host_length) {
            uint32_t announced = le16toh(ctrl->wValue) |
                                 ((uint32_t)le16toh(ctrl->wIndex) << 16);

            if (!p->second_stage || p->expect_bulk_announcement) {
                pthread_mutex_lock(&p->bulk_lock);
                p->bulk_announced += announced;
                p->reset_progress = 0;
                pthread_cond_broadcast(&p->bulk_cond);
                pthread_mutex_unlock(&p->bulk_lock);
                p->expect_bulk_announcement = false;
            }
        }
        if ((ctrl->bRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD &&
            ctrl->bRequest == USB_REQ_SET_CONFIGURATION) {
            if (p->guest_stage) {
                if (le16toh(ctrl->wValue)) {
                    enable_guest_endpoints(p);
                } else {
                    disable_guest_endpoints(p, true, true);
                }
            } else {
                enable_bulk(p);
            }
        } else if (p->guest_stage &&
                   (ctrl->bRequestType &
                    (USB_DIR_IN | USB_TYPE_MASK | USB_RECIP_MASK)) ==
                       (USB_DIR_OUT | USB_TYPE_STANDARD |
                        USB_RECIP_ENDPOINT) &&
                   ctrl->bRequest == USB_REQ_CLEAR_FEATURE &&
                   le16toh(ctrl->wValue) == USB_ENDPOINT_HALT) {
            GuestEndpoint *endpoint = guest_endpoint_by_address(
                p, le16toh(ctrl->wIndex) & 0x8f);

            if (endpoint && endpoint->raw_ep >= 0) {
                uint32_t raw_ep = endpoint->raw_ep;

                fprintf(stderr,
                        "qemu-rpi-dwc2-proxy: guest EP%u %s clear halt"
                        " raw_ep=%u\n",
                        endpoint->descriptor.bEndpointAddress &
                            USB_ENDPOINT_NUMBER_MASK,
                        endpoint->descriptor.bEndpointAddress & USB_DIR_IN ?
                            "IN" : "OUT",
                        raw_ep);
                if (ioctl(p->raw_fd, USB_RAW_IOCTL_EP_CLEAR_HALT,
                          raw_ep) < 0) {
                    if (errno != EBUSY && errno != ESHUTDOWN &&
                        errno != ECONNRESET) {
                        fail_errno("clear Raw Gadget guest endpoint halt");
                    }
                } else {
                    pthread_mutex_lock(&p->bulk_lock);
                    endpoint->halted = false;
                    pthread_cond_broadcast(&p->bulk_cond);
                    pthread_mutex_unlock(&p->bulk_lock);
                }
            }
        }
    }
    if (p->guest_stage) {
        /*
         * Raw Gadget can publish the next bus reset immediately after the
         * host has ACKed a control status stage.  Wait until the guest has
         * consumed that completion and re-armed EP0 so the next physical
         * event cannot overtake the Linux DWC2 IRQ handler under TCG.
         */
        wait_guest_ep0(p);
    }
}

static void handle_control(Proxy *p, const struct usb_ctrlrequest *ctrl)
{
    pthread_mutex_lock(&p->control_lock);
    handle_control_inner(p, ctrl);
    pthread_mutex_unlock(&p->control_lock);
}

static void run_stage(Proxy *p, bool second_stage, bool guest_stage)
{
    struct usb_raw_init init = { .speed = USB_SPEED_HIGH };
    int driver_length;
    int device_length;

    if (guest_stage) {
        wait_guest_ready(p);
    }
    p->raw_fd = open("/dev/raw-gadget", O_RDWR | O_CLOEXEC);
    if (p->raw_fd < 0) {
        fail_errno("open /dev/raw-gadget");
    }
    driver_length = snprintf((char *)init.driver_name,
                             sizeof(init.driver_name), "%s", p->udc_driver);
    device_length = snprintf((char *)init.device_name,
                             sizeof(init.device_name), "%s", p->udc_device);
    if (driver_length < 0 ||
        (size_t)driver_length >= sizeof(init.driver_name) ||
        device_length < 0 ||
        (size_t)device_length >= sizeof(init.device_name)) {
        fail("invalid UDC name");
    }
    raw_ioctl(p->raw_fd, USB_RAW_IOCTL_INIT, &init, "initialize Raw Gadget");
    raw_ioctl(p->raw_fd, USB_RAW_IOCTL_RUN, NULL, "run Raw Gadget");
    p->second_stage = second_stage;
    p->guest_stage = guest_stage;
    p->stage_done = false;
    p->configured = false;
    p->addressed = false;
    p->bulk_started = false;
    p->bulk_stop = false;
    p->bulk_forwarded = 0;
    p->bulk_announced = 0;
    p->expect_bulk_announcement = false;
    p->reset_waited = false;
    p->reset_completed = 0;
    p->reset_progress = 0;
    if (guest_stage) {
        p->guest_configured = false;
        p->guest_stop = false;
        p->guest_config_length = 0;
        p->guest_config_expected = 0;
        p->guest_endpoint_count = 0;
    }
    fprintf(stderr, "qemu-rpi-dwc2-proxy: presenting %s stage\n",
            guest_stage ? "guest Linux gadget" :
            second_stage ? "file-server" : "ROM");
    transport_expect(p, DWC2_DEVICE_TRANSPORT_CONNECT, 0);

    while (!p->stage_done) {
        ControlEvent event = { .event = {
            .type = USB_RAW_EVENT_INVALID,
            .length = sizeof(event.ctrl),
        } };

        raw_ioctl(p->raw_fd, USB_RAW_IOCTL_EVENT_FETCH, &event,
                  "fetch Raw Gadget event");
        switch (event.event.type) {
        case USB_RAW_EVENT_CONNECT:
        case USB_RAW_EVENT_SUSPEND:
        case USB_RAW_EVENT_RESUME:
            break;
        case USB_RAW_EVENT_RESET:
            pthread_mutex_lock(&p->bulk_lock);
            if (p->reset_waited &&
                p->reset_completed < p->reset_count) {
                p->reset_completed++;
                p->reset_waited =
                    p->reset_completed == p->reset_count;
            }
            p->configured = false;
            p->addressed = false;
            if (p->guest_stage) {
                p->guest_configured = false;
                for (uint32_t i = 0; i < p->guest_endpoint_count; i++) {
                    p->guest_endpoints[i].halted = false;
                }
            }
            p->bulk_forwarded = 0;
            p->bulk_announced = 0;
            p->reset_progress = 0;
            p->expect_bulk_announcement = false;
            p->reset_generation++;
            pthread_cond_broadcast(&p->bulk_cond);
            fprintf(stderr,
                    "qemu-rpi-dwc2-proxy: %s reset generation=%" PRIu64
                    "\n", p->guest_stage ? "guest Linux gadget" :
                    p->second_stage ? "file-server" : "ROM",
                    p->reset_generation);
            pthread_mutex_unlock(&p->bulk_lock);
            transport_expect(p, DWC2_DEVICE_TRANSPORT_RESET, 0);
            /*
             * Raw Gadget deliberately retains enabled endpoint handles over
             * a bus reset.  Its queued I/O completes with ESHUTDOWN and each
             * worker waits for SET_CONFIGURATION before requeueing.  Reusing
             * those handles avoids racing EP_DISABLE against reset teardown.
             */
            /*
             * A physical high-speed bus reset is asserted for at least
             * 10 ms.  Give the guest IRQ handler the same reset phase before
             * publishing speed enumeration completion.
             */
            if (p->guest_stage) {
                usleep(10000);
            }
            transport_expect(p, DWC2_DEVICE_TRANSPORT_ENUM_DONE, 0);
            if (p->guest_stage) {
                wait_guest_ep0(p);
                /*
                 * EP0 becomes visible just before the Linux reset handler
                 * finishes its final DCTL and LPM programming.  Do not let a
                 * second physical reset overtake that handler.
                 */
                usleep(10000);
            }
            break;
        case USB_RAW_EVENT_DISCONNECT:
            transport_expect(p, DWC2_DEVICE_TRANSPORT_DISCONNECT, 0);
            break;
        case USB_RAW_EVENT_CONTROL:
            if (event.event.length != sizeof(event.ctrl)) {
                fail("malformed Raw Gadget control event");
            }
            handle_control(p, &event.ctrl);
            break;
        default:
            fail("unknown Raw Gadget event");
        }
    }

    transport_expect(p, DWC2_DEVICE_TRANSPORT_DISCONNECT, 0);
    pthread_mutex_lock(&p->bulk_lock);
    p->bulk_stop = true;
    p->guest_stop = true;
    pthread_cond_broadcast(&p->bulk_cond);
    pthread_mutex_unlock(&p->bulk_lock);
    if (p->bulk_started) {
        uint32_t ep = p->bulk_out;

        /*
         * Wake a worker blocked in EP_READ before joining it.  Closing a file
         * descriptor in another thread does not reliably interrupt an ioctl.
         */
        ioctl(p->raw_fd, USB_RAW_IOCTL_EP_DISABLE, ep);
        pthread_cancel(p->bulk_thread);
        pthread_join(p->bulk_thread, NULL);
        p->bulk_started = false;
    }
    for (uint32_t i = 0; i < p->guest_endpoint_count; i++) {
        GuestEndpoint *endpoint = &p->guest_endpoints[i];

        if (!endpoint->started) {
            continue;
        }
        pthread_cancel(endpoint->thread);
        pthread_join(endpoint->thread, NULL);
        endpoint->started = false;
    }
    close(p->raw_fd);
    p->raw_fd = -1;
}

static int connect_transport(const char *path)
{
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (fd < 0) {
        fail_errno("create DWC2 transport socket");
    }
    if (snprintf(address.sun_path, sizeof(address.sun_path), "%s", path) < 0 ||
        strlen(path) >= sizeof(address.sun_path)) {
        fail("DWC2 transport socket path is too long");
    }
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        fail_errno("connect DWC2 transport socket");
    }
    return fd;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s --dwc2-socket PATH "
            "[--udc-driver dummy_udc] [--udc-device dummy_udc.0] "
            "[--reset-after BYTES [--reset-count COUNT] | "
            "--disconnect-after BYTES "
            "[--disconnect-stage rom|file-server] | "
            "--hold-after BYTES [--hold-stage rom|file-server]] "
            "[--timeout rom-status|file-request [--timeout-ms MS]] "
            "[--bulk-timeout-after BYTES "
            "[--bulk-timeout-stage rom|file-server] "
            "[--bulk-timeout-ms MS]] "
            "[--second-stage-only] [--continue-guest]\n"
            "       %s --dwc2-socket PATH --guest-only\n"
            "       %s --self-test\n", program, program, program);
    exit(EXIT_FAILURE);
}

static void self_test(void)
{
    uint8_t header[DWC2_DEVICE_TRANSPORT_HEADER_SIZE] = { 0 };
    static const uint8_t guest_config[] = {
        9, USB_DT_CONFIG, 32, 0, 1, 1, 0, 0x80, 50,
        9, USB_DT_INTERFACE, 0, 0, 2, 0xff, 0, 0, 0,
        7, USB_DT_ENDPOINT, USB_DIR_IN | 1, USB_ENDPOINT_XFER_BULK,
        0x00, 0x02, 0,
        7, USB_DT_ENDPOINT, USB_DIR_OUT | 2, USB_ENDPOINT_XFER_BULK,
        0x00, 0x02, 0,
    };
    Proxy proxy = { .raw_fd = -1 };
    struct usb_ctrlrequest configuration = {
        .bRequestType = USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        .bRequest = USB_REQ_SET_CONFIGURATION,
        .wValue = htole16(1),
    };

    put_le32(header, DWC2_DEVICE_TRANSPORT_MAGIC);
    header[4] = DWC2_DEVICE_TRANSPORT_VERSION;
    header[5] = DWC2_DEVICE_TRANSPORT_SETUP;
    header[6] = 3;
    put_le32(header + 8, 1234);
    put_le32(header + 12, 5678);
    if (get_le32(header) != DWC2_DEVICE_TRANSPORT_MAGIC ||
        header[4] != DWC2_DEVICE_TRANSPORT_VERSION ||
        header[5] != DWC2_DEVICE_TRANSPORT_SETUP || header[6] != 3 ||
        get_le32(header + 8) != 1234 || get_le32(header + 12) != 5678) {
        fail("DWC2 transport codec self-test failed");
    }
    if (!needs_synthesized_address(&proxy, &configuration)) {
        fail("hidden SET_ADDRESS synthesis self-test failed");
    }
    if (!ep0_length_supported(EP0_CAPACITY) ||
        ep0_length_supported(EP0_CAPACITY + 1) ||
        ep0_length_supported(UINT16_MAX)) {
        fail("EP0 transfer capacity self-test failed");
    }
    proxy.addressed = true;
    if (needs_synthesized_address(&proxy, &configuration)) {
        fail("SET_ADDRESS synthesis repeated after assignment");
    }
    proxy.addressed = false;
    configuration.wValue = 0;
    if (needs_synthesized_address(&proxy, &configuration)) {
        fail("configuration-zero synthesized an address");
    }
    memcpy(proxy.guest_config, guest_config, sizeof(guest_config));
    proxy.guest_config_length = sizeof(guest_config);
    if (!parse_guest_configuration(&proxy) ||
        proxy.guest_endpoint_count != 2 ||
        !guest_endpoint_by_address(&proxy, USB_DIR_IN | 1) ||
        !guest_endpoint_by_address(&proxy, USB_DIR_OUT | 2) ||
        guest_endpoint_by_address(&proxy, USB_DIR_IN | 2)) {
        fail("guest configuration endpoint parser self-test failed");
    }
    proxy.guest_endpoints[0].raw_ep = 4;
    proxy.guest_endpoints[1].raw_ep = 5;
    if (!parse_guest_configuration(&proxy) ||
        proxy.guest_endpoints[0].raw_ep != 4 ||
        proxy.guest_endpoints[1].raw_ep != 5) {
        fail("guest endpoint reset-reuse self-test failed");
    }
    puts("DWC2 Raw Gadget proxy self-test passed");
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        { "dwc2-socket", required_argument, NULL, 's' },
        { "udc-driver", required_argument, NULL, 'd' },
        { "udc-device", required_argument, NULL, 'u' },
        { "reset-after", required_argument, NULL, 'r' },
        { "reset-count", required_argument, NULL, 'c' },
        { "disconnect-after", required_argument, NULL, 'x' },
        { "disconnect-stage", required_argument, NULL, 'g' },
        { "hold-after", required_argument, NULL, 'a' },
        { "hold-stage", required_argument, NULL, 'f' },
        { "second-stage-only", no_argument, NULL, '2' },
        { "continue-guest", no_argument, NULL, 'G' },
        { "guest-only", no_argument, NULL, 'J' },
        { "timeout", required_argument, NULL, 'o' },
        { "timeout-ms", required_argument, NULL, 'm' },
        { "bulk-timeout-after", required_argument, NULL, 'b' },
        { "bulk-timeout-stage", required_argument, NULL, 'e' },
        { "bulk-timeout-ms", required_argument, NULL, 'l' },
        { "self-test", no_argument, NULL, 't' },
        { "help", no_argument, NULL, 'h' },
        { 0 }
    };
    Proxy proxy = {
        .udc_driver = "dummy_udc",
        .udc_device = "dummy_udc.0",
        .transport_fd = -1,
        .raw_fd = -1,
    };
    bool test = false;
    int option;

    while ((option = getopt_long(argc, argv,
                                 "s:d:u:r:c:x:g:a:f:2GJo:m:b:e:l:th",
                                 options, NULL)) != -1) {
        switch (option) {
        case 's':
            proxy.socket_path = optarg;
            break;
        case 'd':
            proxy.udc_driver = optarg;
            break;
        case 'u':
            proxy.udc_device = optarg;
            break;
        case 'r': {
            if (!parse_uint32(optarg, &proxy.reset_after) ||
                !proxy.reset_after) {
                usage(argv[0]);
            }
            break;
        }
        case 'c':
            if (!parse_uint32(optarg, &proxy.reset_count) ||
                !proxy.reset_count) {
                usage(argv[0]);
            }
            break;
        case 'x':
            if (!parse_uint32(optarg, &proxy.disconnect_after) ||
                !proxy.disconnect_after) {
                usage(argv[0]);
            }
            break;
        case 'g':
            if (!strcmp(optarg, "rom")) {
                proxy.disconnect_second_stage = false;
            } else if (!strcmp(optarg, "file-server")) {
                proxy.disconnect_second_stage = true;
            } else {
                usage(argv[0]);
            }
            break;
        case 'a':
            if (!parse_uint32(optarg, &proxy.hold_after) ||
                !proxy.hold_after) {
                usage(argv[0]);
            }
            break;
        case 'f':
            if (!strcmp(optarg, "rom")) {
                proxy.hold_second_stage = false;
            } else if (!strcmp(optarg, "file-server")) {
                proxy.hold_second_stage = true;
            } else {
                usage(argv[0]);
            }
            break;
        case '2':
            proxy.second_stage_only = true;
            break;
        case 'G':
            proxy.continue_guest = true;
            break;
        case 'J':
            proxy.guest_only = true;
            break;
        case 'o':
            if (!strcmp(optarg, "rom-status")) {
                proxy.timeout = PROXY_TIMEOUT_ROM_STATUS;
            } else if (!strcmp(optarg, "file-request")) {
                proxy.timeout = PROXY_TIMEOUT_FILE_REQUEST;
            } else {
                usage(argv[0]);
            }
            break;
        case 'm':
            if (!parse_uint32(optarg, &proxy.timeout_ms)) {
                usage(argv[0]);
            }
            break;
        case 'b':
            if (!parse_uint32(optarg, &proxy.bulk_timeout_after) ||
                !proxy.bulk_timeout_after) {
                usage(argv[0]);
            }
            break;
        case 'e':
            if (!strcmp(optarg, "rom")) {
                proxy.bulk_timeout_second_stage = false;
            } else if (!strcmp(optarg, "file-server")) {
                proxy.bulk_timeout_second_stage = true;
            } else {
                usage(argv[0]);
            }
            break;
        case 'l':
            if (!parse_uint32(optarg, &proxy.bulk_timeout_ms)) {
                usage(argv[0]);
            }
            break;
        case 't':
            test = true;
            break;
        default:
            usage(argv[0]);
        }
    }
    if (test) {
        if (proxy.socket_path || proxy.reset_after ||
            proxy.reset_count ||
            proxy.disconnect_after || proxy.hold_after || proxy.timeout ||
            proxy.timeout_ms || proxy.bulk_timeout_after ||
            proxy.bulk_timeout_ms || proxy.second_stage_only ||
            proxy.continue_guest || proxy.guest_only ||
            optind != argc) {
            usage(argv[0]);
        }
        self_test();
        return EXIT_SUCCESS;
    }
    if (!proxy.socket_path || optind != argc) {
        usage(argv[0]);
    }
    if (proxy.guest_only &&
        (proxy.second_stage_only || proxy.continue_guest ||
         proxy.reset_after || proxy.disconnect_after || proxy.hold_after ||
         proxy.timeout || proxy.bulk_timeout_after)) {
        usage(argv[0]);
    }
    if (!!proxy.reset_after + !!proxy.disconnect_after +
        !!proxy.hold_after + !!proxy.timeout +
        !!proxy.bulk_timeout_after > 1) {
        usage(argv[0]);
    }
    if (!proxy.reset_after && proxy.reset_count) {
        usage(argv[0]);
    }
    if (proxy.reset_after && !proxy.reset_count) {
        proxy.reset_count = 1;
    }
    if (proxy.timeout && !proxy.timeout_ms) {
        proxy.timeout_ms = DEFAULT_FAULT_TIMEOUT_MS;
    }
    if ((!proxy.timeout && proxy.timeout_ms) ||
        (proxy.timeout &&
         (proxy.timeout_ms <= RPIBOOT_HOST_TIMEOUT_MS ||
          proxy.timeout_ms > 60000))) {
        usage(argv[0]);
    }
    if (proxy.bulk_timeout_after && !proxy.bulk_timeout_ms) {
        proxy.bulk_timeout_ms = DEFAULT_BULK_FAULT_TIMEOUT_MS;
    }
    if ((!proxy.bulk_timeout_after && proxy.bulk_timeout_ms) ||
        (proxy.bulk_timeout_after &&
         (proxy.bulk_timeout_ms <= RPIBOOT_BULK_TIMEOUT_MS ||
          proxy.bulk_timeout_ms > 60000))) {
        usage(argv[0]);
    }
    option = pthread_mutex_init(&proxy.transport_lock, NULL);
    if (option) {
        errno = option;
        fail_errno("initialize DWC2 transport lock");
    }
    option = pthread_mutex_init(&proxy.control_lock, NULL);
    if (option) {
        errno = option;
        fail_errno("initialize DWC2 control lock");
    }
    option = pthread_mutex_init(&proxy.bulk_lock, NULL);
    if (option) {
        errno = option;
        fail_errno("initialize bulk ordering lock");
    }
    option = pthread_cond_init(&proxy.bulk_cond, NULL);
    if (option) {
        errno = option;
        fail_errno("initialize bulk ordering condition");
    }
    proxy.transport_fd = connect_transport(proxy.socket_path);
    if (proxy.guest_only) {
        run_stage(&proxy, true, true);
        goto done;
    }
    if (!proxy.second_stage_only) {
        run_stage(&proxy, false, false);
        usleep(500000);
    }
    run_stage(&proxy, true, false);
    puts("unchanged rpiboot completed through emulated DWC2");
    fflush(stdout);
    if (proxy.continue_guest) {
        usleep(500000);
        run_stage(&proxy, true, true);
    }
done:
    close(proxy.transport_fd);
    pthread_cond_destroy(&proxy.bulk_cond);
    pthread_mutex_destroy(&proxy.bulk_lock);
    pthread_mutex_destroy(&proxy.control_lock);
    pthread_mutex_destroy(&proxy.transport_lock);
    return EXIT_SUCCESS;
}
