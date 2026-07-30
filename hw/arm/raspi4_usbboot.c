/*
 * Raspberry Pi 4 USB mass-storage boot and CM4 rpiboot
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/arm/raspi4b-internal.h"

bool raspi4_provision_write(Raspi4bMachineState *s,
                                   const char *state)
{
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;

    if (!s->provision_state_file) {
        return true;
    }
    contents = g_strdup_printf("%s\n", state);
    if (!g_file_set_contents(s->provision_state_file, contents, -1, &error)) {
        error_report("cannot write CM4 provision state '%s': %s",
                     s->provision_state_file, error->message);
        return false;
    }
    return true;
}

char *raspi4_provision_read(Raspi4bMachineState *s)
{
    g_autoptr(GError) error = NULL;
    char *contents = NULL;

    if (!s->provision_state_file) {
        return NULL;
    }
    if (!g_file_get_contents(s->provision_state_file, &contents, NULL,
                             &error)) {
        error_report("cannot read CM4 provision state '%s': %s",
                     s->provision_state_file, error->message);
        return NULL;
    }
    g_strchomp(contents);
    return contents;
}

bool raspi4_provision_lock(Raspi4bMachineState *s)
{
    g_autofree char *path = NULL;

    if (!s->provision_state_file || s->provision_lock_fd >= 0) {
        return true;
    }
    path = g_strdup_printf("%s.lock", s->provision_state_file);
    s->provision_lock_fd = qemu_open_old(path, O_RDWR | O_CREAT, 0644);
    if (s->provision_lock_fd < 0) {
        error_report("cannot open CM4 provision lock '%s': %s", path,
                     strerror(errno));
        return false;
    }
    if (qemu_lock_fd(s->provision_lock_fd, 0, 0, true) < 0) {
        error_report("CM4 provision lifecycle is owned by another process: "
                     "%s", path);
        close(s->provision_lock_fd);
        s->provision_lock_fd = -1;
        return false;
    }
    return true;
}

void raspi4_provision_release(Raspi4bMachineState *s)
{
    if (s->provision_rpiboot_owned) {
        raspi4_provision_write(s, "rpiboot-host-ready");
        s->provision_rpiboot_owned = false;
        s->provision_emmc_owned = false;
    } else if (s->provision_emmc_owned) {
        raspi4_provision_write(s, "qemu-stopped");
        s->provision_emmc_owned = false;
    }
}

void raspi4_provision_exit_notify(Notifier *notifier, void *data)
{
    Raspi4bMachineState *s = container_of(notifier, Raspi4bMachineState,
                                          provision_exit_notifier);

    raspi4_provision_release(s);
}

bool raspi4_provision_parse_bool(const uint8_t *config,
                                        size_t config_size,
                                        const char *key, bool *present,
                                        bool *value)
{
    g_autofree char *text = g_strndup((const char *)config, config_size);
    g_auto(GStrv) lines = g_strsplit(text, "\n", -1);

    if (memchr(config, '\0', config_size)) {
        return false;
    }
    *present = false;
    *value = false;
    for (unsigned int i = 0; lines[i]; i++) {
        char *line = g_strstrip(lines[i]);
        char *separator;
        uint64_t parsed;

        if (!*line || *line == '#') {
            continue;
        }
        separator = strchr(line, '=');
        if (!separator) {
            continue;
        }
        *separator++ = '\0';
        line = g_strstrip(line);
        separator = g_strstrip(separator);
        if (strcmp(line, key)) {
            continue;
        }
        if (*present || qemu_strtou64(separator, NULL, 0, &parsed) < 0 ||
            parsed > 1) {
            return false;
        }
        *present = true;
        *value = parsed;
    }
    return true;
}


static void raspi4_rpiboot_provision_state(Raspi4bMachineState *s,
                                            const char *state,
                                            bool active);
static bool raspi4_rpiboot_secure_provision(Raspi4bMachineState *s);
static void raspi4_rpiboot_arm_setup(Raspi4bMachineState *s)
{
    int error = dwc2_device_firmware_arm_out(
        raspi4_dwc2(s), 0, RASPI4_RPIBOOT_EP0_DMA, 8, 64,
        DXEPCTL_EPTYPE_CONTROL, true);

    if (error) {
        error_report("failed to arm RPIBOOT EP0 SETUP: %s",
                     strerror(-error));
    }
}

static size_t raspi4_rpiboot_string_descriptor(uint8_t *data,
                                                size_t capacity,
                                                unsigned int index,
                                                bool second_stage)
{
    static const char *const strings[] = {
        NULL, "Broadcom", "BCM2711 Boot",
    };
    const char *text;
    size_t length;

    if (index == 0) {
        static const uint8_t language[] = { 4, USB_DT_STRING, 0x09, 0x04 };

        memcpy(data, language, sizeof(language));
        return sizeof(language);
    }
    if (index == 4 && second_stage) {
        text = "00000001";
    } else if (index < ARRAY_SIZE(strings) && strings[index]) {
        text = strings[index];
    } else {
        return 0;
    }
    length = strlen(text);
    if (2 + length * 2 > capacity) {
        return 0;
    }
    data[0] = 2 + length * 2;
    data[1] = USB_DT_STRING;
    for (size_t i = 0; i < length; i++) {
        data[2 + i * 2] = text[i];
        data[3 + i * 2] = 0;
    }
    return data[0];
}

static size_t raspi4_rpiboot_descriptor(uint8_t *data, size_t capacity,
                                        unsigned int type,
                                        unsigned int index,
                                        bool second_stage)
{
    static const uint8_t device[] = {
        18, USB_DT_DEVICE, 0x00, 0x02, 0xff, 0x00, 0x00, 64,
        0x5c, 0x0a, 0x11, 0x27, 0x00, 0x01, 1, 2, 0, 1,
    };
    static const uint8_t configuration[] = {
        9, USB_DT_CONFIG, 25, 0, 1, 1, 0, 0xc0, 1,
        9, USB_DT_INTERFACE, 0, 0, 1, 0xff, 0, 0, 0,
        7, USB_DT_ENDPOINT, 1, USB_ENDPOINT_XFER_BULK, 0x00, 0x02, 0,
    };
    static const uint8_t qualifier[] = {
        10, USB_DT_DEVICE_QUALIFIER, 0x00, 0x02, 0xff, 0, 0, 64, 1, 0,
    };
    const uint8_t *descriptor;
    size_t length;

    switch (type) {
    case USB_DT_DEVICE:
        descriptor = device;
        length = sizeof(device);
        break;
    case USB_DT_CONFIG:
        descriptor = configuration;
        length = sizeof(configuration);
        break;
    case USB_DT_DEVICE_QUALIFIER:
        descriptor = qualifier;
        length = sizeof(qualifier);
        break;
    case USB_DT_STRING:
        return raspi4_rpiboot_string_descriptor(
            data, capacity, index, second_stage);
    default:
        return 0;
    }
    if (length > capacity) {
        return 0;
    }
    memcpy(data, descriptor, length);
    if (type == USB_DT_DEVICE && second_stage) {
        data[16] = 4;
    }
    return length;
}

static void raspi4_rpiboot_stall_ep0(Raspi4bMachineState *s, bool in)
{
    DWC2State *dwc2 = raspi4_dwc2(s);
    uint32_t *ctl = in ? &dwc2->diepctl(0) : &dwc2->doepctl(0);

    *ctl = DXEPCTL_USBACTEP | DXEPCTL_STALL | D0EPCTL_MPS_64;
}

static bool raspi4_rpiboot_endpoint_valid(uint16_t index)
{
    unsigned int ep = index & 0xf;
    bool in = index & USB_DIR_IN;

    if (index & ~(USB_DIR_IN | 0xf)) {
        return false;
    }
    return ep == 0 || (ep == 1 && !in);
}

static void raspi4_rpiboot_rollback_interrupted(Raspi4bMachineState *s);

static bool raspi4_rpiboot_arm_bulk_out(Raspi4bMachineState *s,
                                        uint32_t length)
{
    uint32_t chunk = MIN(length, 512U * DXEPTSIZ_PKTCNT_LIMIT);
    int error;

    if (!length) {
        return false;
    }
    s->rpiboot_bulk_expected = length;
    s->rpiboot_bulk_received = 0;
    error = dwc2_device_firmware_arm_out(
        raspi4_dwc2(s), 1, RASPI4_RPIBOOT_EP1_DMA, chunk, 512,
        DXEPCTL_EPTYPE_BULK, false);
    if (error) {
        error_report("failed to arm RPIBOOT bulk OUT: %s",
                     strerror(-error));
        return false;
    }
    return true;
}

static bool raspi4_rpiboot_arm_next_bulk_chunk(Raspi4bMachineState *s)
{
    uint32_t remaining = s->rpiboot_bulk_expected -
                         s->rpiboot_bulk_received;
    uint32_t chunk = MIN(remaining, 512U * DXEPTSIZ_PKTCNT_LIMIT);
    int error = dwc2_device_firmware_arm_out(
        raspi4_dwc2(s), 1, RASPI4_RPIBOOT_EP1_DMA, chunk, 512,
        DXEPCTL_EPTYPE_BULK, false);

    if (error) {
        error_report("failed to continue RPIBOOT bulk OUT: %s",
                     strerror(-error));
        return false;
    }
    return true;
}

static void raspi4_rpiboot_file_message(uint8_t response[260],
                                        uint32_t command, const char *name)
{
    stl_le_p(response, command);
    pstrcpy((char *)response + 4, 256, name);
}

static bool raspi4_rpiboot_secure_provision_requested(
    const Raspi4bMachineState *s)
{
    g_autofree char *text = NULL;
    g_auto(GStrv) lines = NULL;
    unsigned int matches = 0;
    bool requested = false;

    if (!s->rpiboot_config || !s->rpiboot_config_size) {
        return false;
    }
    text = g_strndup(
        (const char *)s->rpiboot_config, s->rpiboot_config_size);
    lines = g_strsplit(text, "\n", -1);
    for (unsigned int i = 0; lines[i]; i++) {
        char *line = g_strstrip(lines[i]);
        char *separator;
        uint64_t parsed;

        if (!*line || *line == '#') {
            continue;
        }
        separator = strchr(line, '=');
        if (!separator) {
            continue;
        }
        *separator++ = '\0';
        if (strcmp(g_strstrip(line), "program_pubkey")) {
            continue;
        }
        matches++;
        separator = g_strstrip(separator);
        if (qemu_strtou64(separator, NULL, 0, &parsed) < 0 ||
            parsed > 1) {
            return true;
        }
        requested |= parsed;
    }
    return requested || matches > 1;
}

static const char *raspi4_rpiboot_file_name(
    const Raspi4bMachineState *s)
{
    if (!s->rpiboot_file_index) {
        return "config.txt";
    }
    return raspi4_rpiboot_secure_provision_requested(s) ?
           "pieeprom.bin" : "boot.img";
}

static char *raspi4_rpiboot_sha256(const uint8_t *data, uint32_t size,
                                   Error **errp);

static bool raspi4_rpiboot_bootcode_is_trusted(Raspi4bMachineState *s)
{
    g_autofree char *digest = NULL;
    Error *local_err = NULL;

    if (!s->rpiboot_bootcode_trusted_sha256) {
        return false;
    }
    digest = raspi4_rpiboot_sha256(s->rpiboot_bootcode,
                                   s->rpiboot_bootcode_size, &local_err);
    if (!digest) {
        error_report_err(local_err);
        return false;
    }
    return !strcmp(digest, s->rpiboot_bootcode_trusted_sha256);
}

static bool raspi4_rpiboot_handle_vendor(Raspi4bMachineState *s,
                                         const uint8_t setup[8],
                                         uint8_t response[260],
                                         size_t *response_length)
{
    uint32_t announced = lduw_le_p(setup + 2) |
                         ((uint32_t)lduw_le_p(setup + 4) << 16);
    bool in = setup[0] & USB_DIR_IN;

    if (setup[1] != 0) {
        return false;
    }
    if (in) {
        if (s->rpiboot_phase == RASPI4_RPIBOOT_EXPECT_STATUS &&
            lduw_le_p(setup + 6) == 4) {
            stl_le_p(response, s->rpiboot_bootcode_trusted ? 0 : 1);
            *response_length = 4;
            return true;
        }
        if (!s->rpiboot_second_stage ||
            lduw_le_p(setup + 6) != 260) {
            return false;
        }
        switch (s->rpiboot_phase) {
        case RASPI4_RPIBOOT_FILE_SEND_SIZE:
            raspi4_rpiboot_file_message(
                response, 0, raspi4_rpiboot_file_name(s));
            s->rpiboot_phase = RASPI4_RPIBOOT_FILE_WAIT_SIZE;
            break;
        case RASPI4_RPIBOOT_FILE_SEND_READ:
            raspi4_rpiboot_file_message(
                response, 1, raspi4_rpiboot_file_name(s));
            s->rpiboot_phase = RASPI4_RPIBOOT_FILE_WAIT_DATA;
            break;
        case RASPI4_RPIBOOT_FILE_SEND_DONE:
            raspi4_rpiboot_file_message(response, 2, "done");
            s->rpiboot_phase = RASPI4_RPIBOOT_COMPLETE;
            break;
        default:
            return false;
        }
        *response_length = 260;
        return true;
    }
    if (lduw_le_p(setup + 6) != 0) {
        return false;
    }
    switch (s->rpiboot_phase) {
    case RASPI4_RPIBOOT_EXPECT_MESSAGE:
        if (announced != RASPI4_RPIBOOT_BOOT_MESSAGE_SIZE ||
            !raspi4_rpiboot_arm_bulk_out(s, announced)) {
            return false;
        }
        break;
    case RASPI4_RPIBOOT_EXPECT_BOOTCODE:
        if (announced != s->rpiboot_expected_bootcode ||
            !raspi4_rpiboot_arm_bulk_out(s, announced)) {
            return false;
        }
        g_free(s->rpiboot_bootcode);
        s->rpiboot_bootcode = g_malloc(announced);
        s->rpiboot_bootcode_alloc = announced;
        s->rpiboot_bootcode_size = 0;
        s->rpiboot_bootcode_trusted = false;
        break;
    case RASPI4_RPIBOOT_FILE_WAIT_SIZE:
        if (!announced || announced > RASPI4_RPIBOOT_MAX_FILE_SIZE) {
            return false;
        }
        s->rpiboot_file_expected = announced;
        s->rpiboot_phase = RASPI4_RPIBOOT_FILE_SEND_READ;
        break;
    case RASPI4_RPIBOOT_FILE_WAIT_DATA:
        if (announced != s->rpiboot_file_expected ||
            !raspi4_rpiboot_arm_bulk_out(s, announced)) {
            return false;
        }
        if (s->rpiboot_file_index) {
            g_free(s->rpiboot_boot_img);
            s->rpiboot_boot_img = g_malloc(announced);
            s->rpiboot_boot_img_alloc = announced;
            s->rpiboot_boot_img_size = 0;
        } else {
            g_free(s->rpiboot_config);
            s->rpiboot_config = g_malloc(announced);
            s->rpiboot_config_alloc = announced;
            s->rpiboot_config_size = 0;
        }
        break;
    default:
        return false;
    }
    *response_length = 0;
    return true;
}

static void raspi4_rpiboot_handle_setup(Raspi4bMachineState *s,
                                        const uint8_t setup[8])
{
    uint8_t response[260] = { 0 };
    uint16_t value = lduw_le_p(setup + 2);
    uint16_t index = lduw_le_p(setup + 4);
    uint16_t length = lduw_le_p(setup + 6);
    uint8_t recipient = setup[0] & USB_RECIP_MASK;
    size_t response_length = 0;
    bool in = setup[0] & USB_DIR_IN;
    int error;

    s->rpiboot_control_in = in;
    s->rpiboot_control_pending = RASPI4_RPIBOOT_CONTROL_NONE;
    s->rpiboot_control_value = 0;
    if ((setup[0] & USB_TYPE_MASK) == USB_TYPE_VENDOR) {
        if (!s->rpiboot_configuration) {
            raspi4_rpiboot_stall_ep0(s, in);
            return;
        }
        if (!raspi4_rpiboot_handle_vendor(
                s, setup, response, &response_length)) {
            raspi4_rpiboot_stall_ep0(s, in);
            return;
        }
    } else if ((setup[0] & USB_TYPE_MASK) != USB_TYPE_STANDARD) {
        raspi4_rpiboot_stall_ep0(s, in);
        return;
    } else {
        switch (setup[1]) {
        case USB_REQ_GET_DESCRIPTOR:
            if (!in || recipient != USB_RECIP_DEVICE ||
                ((value >> 8) != USB_DT_STRING &&
                 ((value & 0xff) || index)) ||
                ((value >> 8) == USB_DT_STRING &&
                 index != 0 && index != 0x0409)) {
                raspi4_rpiboot_stall_ep0(s, in);
                return;
            }
            response_length = raspi4_rpiboot_descriptor(
                response, sizeof(response), value >> 8, value & 0xff,
                s->rpiboot_second_stage);
            if (!response_length) {
                raspi4_rpiboot_stall_ep0(s, true);
                return;
            }
            break;
        case USB_REQ_GET_CONFIGURATION:
            if (!in || recipient != USB_RECIP_DEVICE || value || index ||
                length != 1) {
                raspi4_rpiboot_stall_ep0(s, in);
                return;
            }
            response[0] = s->rpiboot_configuration;
            response_length = 1;
            break;
        case USB_REQ_GET_INTERFACE:
            if (!in || recipient != USB_RECIP_INTERFACE || value || index ||
                length != 1 || !s->rpiboot_configuration) {
                raspi4_rpiboot_stall_ep0(s, in);
                return;
            }
            response[0] = 0;
            response_length = 1;
            break;
        case USB_REQ_GET_STATUS:
            if (!in || value || length != 2) {
                raspi4_rpiboot_stall_ep0(s, in);
                return;
            }
            if (recipient == USB_RECIP_DEVICE) {
                if (index) {
                    raspi4_rpiboot_stall_ep0(s, true);
                    return;
                }
                response[0] = BIT(USB_DEVICE_SELF_POWERED);
            } else if (recipient == USB_RECIP_INTERFACE) {
                if (index || !s->rpiboot_configuration) {
                    raspi4_rpiboot_stall_ep0(s, true);
                    return;
                }
            } else if (recipient == USB_RECIP_ENDPOINT) {
                unsigned int ep = index & 0xf;
                bool ep_in = index & USB_DIR_IN;
                uint32_t ctl;

                if (!raspi4_rpiboot_endpoint_valid(index) ||
                    (ep && !s->rpiboot_configuration)) {
                    raspi4_rpiboot_stall_ep0(s, true);
                    return;
                }
                ctl = ep_in ? raspi4_dwc2(s)->diepctl(ep) :
                              raspi4_dwc2(s)->doepctl(ep);
                response[0] = !!(ctl & DXEPCTL_STALL);
            } else {
                raspi4_rpiboot_stall_ep0(s, true);
                return;
            }
            response_length = 2;
            break;
        case USB_REQ_SET_ADDRESS:
            if (in || recipient != USB_RECIP_DEVICE || value > 127 ||
                index || length || s->rpiboot_configuration) {
                raspi4_rpiboot_stall_ep0(s, in);
                return;
            }
            s->rpiboot_control_pending =
                RASPI4_RPIBOOT_CONTROL_SET_ADDRESS;
            s->rpiboot_control_value = value;
            break;
        case USB_REQ_SET_CONFIGURATION:
            if (in || recipient != USB_RECIP_DEVICE || value > 1 ||
                index || length ||
                (value && !(raspi4_dwc2(s)->dcfg & DCFG_DEVADDR_MASK))) {
                raspi4_rpiboot_stall_ep0(s, in);
                return;
            }
            s->rpiboot_control_pending =
                RASPI4_RPIBOOT_CONTROL_SET_CONFIGURATION;
            s->rpiboot_control_value = value;
            break;
        case USB_REQ_SET_INTERFACE:
            if (in || recipient != USB_RECIP_INTERFACE || value || index ||
                length || !s->rpiboot_configuration) {
                raspi4_rpiboot_stall_ep0(s, in);
                return;
            }
            break;
        case USB_REQ_CLEAR_FEATURE:
        case USB_REQ_SET_FEATURE:
            if (in || recipient != USB_RECIP_ENDPOINT ||
                value != USB_ENDPOINT_HALT || length ||
                !s->rpiboot_configuration ||
                !raspi4_rpiboot_endpoint_valid(index) ||
                !(index & 0xf)) {
                raspi4_rpiboot_stall_ep0(s, in);
                return;
            }
            if (setup[1] == USB_REQ_SET_FEATURE) {
                s->rpiboot_control_pending =
                    RASPI4_RPIBOOT_CONTROL_SET_ENDPOINT_HALT;
            } else {
                s->rpiboot_control_pending =
                    RASPI4_RPIBOOT_CONTROL_CLEAR_ENDPOINT_HALT;
            }
            s->rpiboot_control_value = index;
            break;
        default:
            raspi4_rpiboot_stall_ep0(s, in);
            return;
        }
    }

    if (in) {
        if (!response_length && length) {
            raspi4_rpiboot_stall_ep0(s, true);
            return;
        }
        response_length = MIN(response_length, length);
    } else if (length) {
        raspi4_rpiboot_stall_ep0(s, false);
        return;
    }
    error = dwc2_device_firmware_arm_in(
        raspi4_dwc2(s), 0, RASPI4_RPIBOOT_EP0_DMA,
        response, response_length, 64, DXEPCTL_EPTYPE_CONTROL);
    if (error) {
        error_report("failed to arm RPIBOOT EP0 IN: %s",
                     strerror(-error));
    }
}

static void raspi4_rpiboot_packet(void *opaque, unsigned int ep, bool in,
                                  bool setup, const uint8_t *data,
                                  size_t length, bool complete)
{
    Raspi4bMachineState *s = opaque;

    if (!s->rpiboot_dwc2_active) {
        return;
    }
    if (ep == 1 && !in) {
        if (s->rpiboot_bulk_received + length >
            s->rpiboot_bulk_expected) {
            return;
        }
        if (s->rpiboot_phase == RASPI4_RPIBOOT_EXPECT_MESSAGE) {
            memcpy(s->rpiboot_boot_message + s->rpiboot_bulk_received,
                   data, length);
        } else if (s->rpiboot_phase == RASPI4_RPIBOOT_EXPECT_BOOTCODE) {
            memcpy(s->rpiboot_bootcode + s->rpiboot_bulk_received,
                   data, length);
        } else if (s->rpiboot_phase == RASPI4_RPIBOOT_FILE_WAIT_DATA) {
            uint8_t *file = s->rpiboot_file_index ?
                            s->rpiboot_boot_img : s->rpiboot_config;

            memcpy(file + s->rpiboot_bulk_received, data, length);
        }
        s->rpiboot_bulk_received += length;
        if (!complete) {
            return;
        }
        if (s->rpiboot_bulk_received != s->rpiboot_bulk_expected) {
            raspi4_rpiboot_arm_next_bulk_chunk(s);
            return;
        }
        if (s->rpiboot_phase == RASPI4_RPIBOOT_EXPECT_MESSAGE) {
            s->rpiboot_expected_bootcode =
                ldl_le_p(s->rpiboot_boot_message);
            if (!s->rpiboot_expected_bootcode ||
                s->rpiboot_expected_bootcode >
                    RASPI4_RPIBOOT_MAX_BOOTCODE_SIZE) {
                s->rpiboot_expected_bootcode = 0;
                return;
            }
            s->rpiboot_phase = RASPI4_RPIBOOT_EXPECT_BOOTCODE;
        } else if (s->rpiboot_phase ==
                   RASPI4_RPIBOOT_EXPECT_BOOTCODE) {
            s->rpiboot_bootcode_size = s->rpiboot_bulk_received;
            s->rpiboot_bootcode_trusted =
                raspi4_rpiboot_bootcode_is_trusted(s);
            s->rpiboot_phase = RASPI4_RPIBOOT_EXPECT_STATUS;
        } else if (s->rpiboot_phase ==
                   RASPI4_RPIBOOT_FILE_WAIT_DATA) {
            if (s->rpiboot_file_index) {
                s->rpiboot_boot_img_size = s->rpiboot_bulk_received;
            } else {
                s->rpiboot_config_size = s->rpiboot_bulk_received;
            }
            s->rpiboot_file_index++;
            s->rpiboot_phase = s->rpiboot_file_index == 2 ?
                               RASPI4_RPIBOOT_FILE_SEND_DONE :
                               RASPI4_RPIBOOT_FILE_SEND_SIZE;
        }
        return;
    }
    if (ep != 0 || !complete) {
        return;
    }
    if (setup) {
        if (length != 8) {
            raspi4_rpiboot_stall_ep0(s, false);
            return;
        }
        raspi4_rpiboot_handle_setup(s, data);
    } else if (in && s->rpiboot_control_in) {
        int error = dwc2_device_firmware_arm_out(
            raspi4_dwc2(s), 0, RASPI4_RPIBOOT_EP0_DMA, 0, 64,
            DXEPCTL_EPTYPE_CONTROL, false);

        if (error) {
            error_report("failed to arm RPIBOOT EP0 status OUT: %s",
                         strerror(-error));
        }
    } else {
        if (in && !s->rpiboot_control_in) {
            if (s->rpiboot_control_pending ==
                RASPI4_RPIBOOT_CONTROL_SET_ADDRESS) {
                raspi4_dwc2(s)->dcfg &= ~DCFG_DEVADDR_MASK;
                raspi4_dwc2(s)->dcfg |=
                    DCFG_DEVADDR(s->rpiboot_control_value);
            } else if (s->rpiboot_control_pending ==
                       RASPI4_RPIBOOT_CONTROL_SET_CONFIGURATION) {
                uint8_t configuration = s->rpiboot_control_value;

                raspi4_rpiboot_rollback_interrupted(s);
                dwc2_device_firmware_disable_endpoint(raspi4_dwc2(s), 1);
                s->rpiboot_configuration = configuration;
            } else if (s->rpiboot_control_pending ==
                       RASPI4_RPIBOOT_CONTROL_SET_ENDPOINT_HALT) {
                raspi4_dwc2(s)->doepctl(1) |= DXEPCTL_STALL;
            } else if (s->rpiboot_control_pending ==
                       RASPI4_RPIBOOT_CONTROL_CLEAR_ENDPOINT_HALT) {
                raspi4_dwc2(s)->doepctl(1) &= ~DXEPCTL_STALL;
            }
        }
        if (s->rpiboot_phase == RASPI4_RPIBOOT_EXPECT_STATUS &&
            s->rpiboot_control_in) {
            if (s->rpiboot_bootcode_trusted) {
                s->rpiboot_phase = RASPI4_RPIBOOT_BOOTCODE_READY;
                raspi4_set_boot_observation(s, "rpiboot-bootcode-ready",
                                            "rpiboot");
            } else {
                raspi4_set_boot_observation(
                    s, "rpiboot-bootcode-untrusted", "rpiboot");
                raspi4_rpiboot_provision_state(
                    s, "rpiboot-failed", false);
            }
        } else if (s->rpiboot_phase == RASPI4_RPIBOOT_COMPLETE &&
                   s->rpiboot_control_in) {
            if (raspi4_rpiboot_secure_provision(s)) {
                if (!raspi4_rpiboot_secure_provision_requested(s)) {
                    Raspi4BootAttemptResult result;

                    raspi4_sample_edids(s);
                    raspi4_clear_firmware_observation(s);
                    result = raspi4_try_rpiboot_firmware(s);
                    if (result == RASPI4_BOOT_ATTEMPT_READY) {
                        raspi4_set_boot_observation(
                            s, "arm-handoff-ready", "rpiboot");
                    } else if (result == RASPI4_BOOT_ATTEMPT_FAILED) {
                        raspi4_set_boot_observation(
                            s, "rpiboot-boot-image-invalid", "rpiboot");
                    }
                }
                raspi4_rpiboot_provision_state(
                    s, "rpiboot-complete", false);
            }
        }
        s->rpiboot_control_in = false;
        s->rpiboot_control_pending = RASPI4_RPIBOOT_CONTROL_NONE;
        s->rpiboot_control_value = 0;
        if (s->rpiboot_dwc2_active) {
            raspi4_rpiboot_arm_setup(s);
        }
    }
}

static void raspi4_rpiboot_rollback_interrupted(Raspi4bMachineState *s)
{
    if (!s->rpiboot_second_stage &&
        s->rpiboot_phase != RASPI4_RPIBOOT_EXPECT_MESSAGE &&
        s->rpiboot_phase != RASPI4_RPIBOOT_BOOTCODE_READY) {
        s->rpiboot_phase = RASPI4_RPIBOOT_EXPECT_MESSAGE;
        s->rpiboot_bulk_expected = 0;
        s->rpiboot_bulk_received = 0;
        s->rpiboot_expected_bootcode = 0;
        s->rpiboot_bootcode_size = 0;
        s->rpiboot_bootcode_alloc = 0;
        s->rpiboot_bootcode_trusted = false;
        memset(s->rpiboot_boot_message, 0,
               sizeof(s->rpiboot_boot_message));
        g_clear_pointer(&s->rpiboot_bootcode, g_free);
    } else if (s->rpiboot_second_stage &&
               s->rpiboot_phase != RASPI4_RPIBOOT_FILE_SEND_SIZE &&
               s->rpiboot_phase != RASPI4_RPIBOOT_COMPLETE) {
        s->rpiboot_phase = RASPI4_RPIBOOT_FILE_SEND_SIZE;
        s->rpiboot_bulk_expected = 0;
        s->rpiboot_bulk_received = 0;
        s->rpiboot_file_index = 0;
        s->rpiboot_file_expected = 0;
        s->rpiboot_config_size = 0;
        s->rpiboot_config_alloc = 0;
        s->rpiboot_boot_img_size = 0;
        s->rpiboot_boot_img_alloc = 0;
        g_clear_pointer(&s->rpiboot_config, g_free);
        g_clear_pointer(&s->rpiboot_boot_img, g_free);
    }
    s->rpiboot_control_in = false;
    s->rpiboot_control_pending = RASPI4_RPIBOOT_CONTROL_NONE;
    s->rpiboot_control_value = 0;
}

static void raspi4_rpiboot_provision_state(Raspi4bMachineState *s,
                                            const char *state,
                                            bool active)
{
    if (!s->provision_state_file) {
        return;
    }
    s->provision_rpiboot_owned =
        raspi4_provision_write(s, state) && active;
}

static bool raspi4_rpiboot_secure_provision(Raspi4bMachineState *s)
{
    uint8_t key_hash[32];
    Raspi4OtpProvisionResult provision_result;
    Raspi4ProgramResult program_result;
    Raspi4OtpCommitResult commit_result;
    int64_t eeprom_length;
    const char *status;

    if (!raspi4_rpiboot_secure_provision_requested(s)) {
        return true;
    }
    provision_result = raspi4_secure_provision_prepare(
        s, s->rpiboot_config, s->rpiboot_config_size,
        s->rpiboot_boot_img, s->rpiboot_boot_img_size, key_hash);
    if (provision_result != RASPI4_OTP_PROVISION_READY) {
        status =
            provision_result == RASPI4_OTP_PROVISION_SIGNATURE_INVALID ?
                "recovery-provision-signature-invalid" :
            provision_result == RASPI4_OTP_PROVISION_KEY_MISMATCH ?
                "recovery-provision-key-mismatch" :
            provision_result == RASPI4_OTP_PROVISION_PERSISTENCE_REQUIRED ?
                "recovery-provision-persistence-required" :
            provision_result == RASPI4_OTP_PROVISION_JTAG_UNSUPPORTED ?
                "recovery-provision-jtag-unsupported" :
                "recovery-provision-invalid";
        goto failed;
    }
    if (!s->eeprom || !blk_is_inserted(s->eeprom) ||
        s->eeprom_write_protect) {
        status = !s->eeprom || !blk_is_inserted(s->eeprom) ?
                 "recovery-no-eeprom" : "recovery-write-protected";
        goto failed;
    }
    eeprom_length = blk_getlength(s->eeprom);
    if (eeprom_length < 0 ||
        s->rpiboot_boot_img_size != eeprom_length) {
        status = "recovery-size-invalid";
        goto failed;
    }
    program_result = raspi4_program_eeprom(
        s, s->rpiboot_boot_img, s->rpiboot_boot_img_size, false);
    if (program_result != RASPI4_PROGRAM_OK) {
        status = program_result == RASPI4_PROGRAM_ERROR ?
                 "recovery-program-error" :
                 "recovery-provision-write-failed";
        goto failed;
    }
    commit_result = raspi4_secure_provision_commit(s, key_hash);
    if (commit_result != RASPI4_OTP_COMMIT_OK) {
        status = commit_result == RASPI4_OTP_COMMIT_INTERRUPTED ?
                 "recovery-provision-power-failed" :
                 "recovery-provision-write-failed";
        goto failed;
    }
    raspi4_set_recovery_status(s, "recovery-updated-stop");
    raspi4_set_boot_observation(s, "rpiboot-provisioned", "rpiboot");
    return true;

failed:
    raspi4_set_recovery_status(s, status);
    raspi4_set_boot_observation(s, status, "rpiboot");
    raspi4_rpiboot_provision_state(s, "rpiboot-failed", false);
    return false;
}

static void raspi4_rpiboot_event(void *opaque, unsigned int event,
                                 unsigned int value)
{
    Raspi4bMachineState *s = opaque;

    if (!s->rpiboot_dwc2_active) {
        return;
    }
    if (event == DWC2_DEVICE_EVENT_CONNECT) {
        if (s->rpiboot_phase != RASPI4_RPIBOOT_COMPLETE) {
            raspi4_rpiboot_provision_state(s, "rpiboot-active", true);
        }
    } else if (event == DWC2_DEVICE_EVENT_DISCONNECT) {
        s->rpiboot_configuration = 0;
        s->rpiboot_control_pending = RASPI4_RPIBOOT_CONTROL_NONE;
        s->rpiboot_control_value = 0;
        if (s->rpiboot_phase == RASPI4_RPIBOOT_BOOTCODE_READY) {
            s->rpiboot_second_stage = true;
            s->rpiboot_file_index = 0;
            s->rpiboot_phase = RASPI4_RPIBOOT_FILE_SEND_SIZE;
            raspi4_set_boot_observation(s, "rpiboot-file-server-wait",
                                        "rpiboot");
        } else if (s->rpiboot_phase != RASPI4_RPIBOOT_COMPLETE) {
            raspi4_rpiboot_rollback_interrupted(s);
            raspi4_rpiboot_provision_state(s, "rpiboot-failed", false);
        }
    } else if (event == DWC2_DEVICE_EVENT_RESET) {
        /*
         * Enumeration resets at a stage boundary preserve that boundary;
         * interrupted transfers restart the complete ROM or file-server
         * stage just as a fresh rpiboot claim does.
         */
        raspi4_rpiboot_rollback_interrupted(s);
        s->rpiboot_configuration = 0;
        raspi4_rpiboot_arm_setup(s);
    }
}

void raspi4_rpiboot_dwc2_start(Raspi4bMachineState *s)
{
    if (s->rpiboot_dwc2_active) {
        return;
    }
    s->rpiboot_dwc2_active = true;
    s->rpiboot_control_in = false;
    s->rpiboot_control_pending = RASPI4_RPIBOOT_CONTROL_NONE;
    s->rpiboot_control_value = 0;
    s->rpiboot_configuration = 0;
    s->rpiboot_phase = RASPI4_RPIBOOT_EXPECT_MESSAGE;
    s->rpiboot_bulk_expected = 0;
    s->rpiboot_bulk_received = 0;
    s->rpiboot_expected_bootcode = 0;
    s->rpiboot_bootcode_size = 0;
    s->rpiboot_bootcode_alloc = 0;
    s->rpiboot_bootcode_trusted = false;
    s->rpiboot_second_stage = false;
    s->rpiboot_file_index = 0;
    s->rpiboot_file_expected = 0;
    s->rpiboot_config_size = 0;
    s->rpiboot_config_alloc = 0;
    s->rpiboot_boot_img_size = 0;
    s->rpiboot_boot_img_alloc = 0;
    dwc2_device_set_firmware_handlers(raspi4_dwc2(s),
                                      raspi4_rpiboot_packet,
                                      raspi4_rpiboot_event, s);
    dwc2_device_firmware_start(raspi4_dwc2(s));
    raspi4_rpiboot_arm_setup(s);
}

bool raspi4_rpiboot_media_read(void *opaque, int64_t offset,
                                      int64_t bytes, void *buffer,
                                      Error **errp)
{
    Raspi4bMachineState *s = opaque;

    if (offset < 0 || bytes < 0 ||
        offset > s->rpiboot_boot_img_size ||
        bytes > s->rpiboot_boot_img_size - offset) {
        error_setg(errp, "read exceeds received RPIBOOT boot.img");
        return false;
    }
    memcpy(buffer, s->rpiboot_boot_img + offset, bytes);
    return true;
}

static bool raspi4_usb_vid_pid_excluded(Raspi4UsbBootReader *reader,
                                        const uint8_t descriptor[18])
{
    Raspi4bMachineState *s = reader->machine;
    uint32_t value = ((uint32_t)lduw_le_p(descriptor + 8) << 16) |
                     lduw_le_p(descriptor + 10);

    for (unsigned int i = 0; i < s->usb_msd_exclude_count; i++) {
        if (s->usb_msd_exclude_vid_pid[i] != value) {
            continue;
        }
        s->usb_boot_excluded_device_count++;
        s->usb_boot_last_excluded_vid_pid = value;
        return true;
    }
    return false;
}

static ssize_t raspi4_usb_host_transfer(
    Raspi4UsbBootReader *reader, uint8_t address, uint8_t endpoint,
    uint8_t type, bool in, uint32_t pid, void *buffer, size_t length,
    Error **errp)
{
    if (reader->xhci) {
        return xhci_host_firmware_transfer(
            reader->xhci, address, endpoint, type, in, buffer, length, errp);
    }
    return dwc2_host_firmware_transfer(
        raspi4_dwc2(reader->machine), address, endpoint, type, 64, in, pid,
        RASPI4_USB_BOOT_DMA, buffer, length, errp);
}

static ssize_t raspi4_usb_control(
    Raspi4UsbBootReader *reader, uint8_t address, uint8_t request_type,
    uint8_t request, uint16_t value, uint16_t index, uint8_t *data,
    uint16_t length, Error **errp)
{
    uint8_t setup[8] = {
        request_type,
        request,
        value, value >> 8,
        index, index >> 8,
        length, length >> 8,
    };
    bool in = request_type & USB_DIR_IN;
    uint8_t status = 0;
    ssize_t actual = 0;

    if (raspi4_usb_host_transfer(
            reader, address, 0, USB_ENDPOINT_XFER_CONTROL, false,
            TSIZ_SC_MC_PID_SETUP, setup, sizeof(setup), errp) !=
        sizeof(setup)) {
        return -1;
    }
    if (length) {
        actual = raspi4_usb_host_transfer(
            reader, address, 0, USB_ENDPOINT_XFER_CONTROL, in,
            TSIZ_SC_MC_PID_DATA1, data, length, errp);
        if (actual < 0) {
            return -1;
        }
    }
    if (raspi4_usb_host_transfer(
            reader, address, 0, USB_ENDPOINT_XFER_CONTROL, !in,
            TSIZ_SC_MC_PID_DATA1, &status, 0, errp) != 0) {
        return -1;
    }
    return actual;
}

static bool raspi4_usb_set_address(Raspi4UsbBootReader *reader,
                                   uint8_t old_address, uint8_t new_address,
                                   Error **errp)
{
    uint8_t setup[8] = {
        USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        USB_REQ_SET_ADDRESS,
        new_address, 0,
        0, 0,
        0, 0,
    };
    uint8_t status = 0;

    return raspi4_usb_host_transfer(
               reader, old_address, 0, USB_ENDPOINT_XFER_CONTROL, false,
               TSIZ_SC_MC_PID_SETUP, setup, sizeof(setup), errp) ==
               sizeof(setup) &&
           raspi4_usb_host_transfer(
               reader, old_address, 0, USB_ENDPOINT_XFER_CONTROL, true,
               TSIZ_SC_MC_PID_DATA1, &status, 0, errp) == 0;
}

static bool raspi4_usb_configure_storage(Raspi4UsbBootReader *reader,
                                         uint8_t address,
                                         unsigned int group, Error **errp)
{
    uint8_t config[64] = { 0 };
    uint8_t max_lun = 0;
    uint8_t expected = 0;
    uint16_t bulk_in = 0;
    uint16_t bulk_out = 0;
    ssize_t config_length;

    config_length = raspi4_usb_control(
        reader, address,
        USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        USB_REQ_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0,
        config, sizeof(config), errp);
    if (config_length < 9) {
        return false;
    }
    for (size_t offset = 0;
         offset + 2 <= config_length && config[offset] >= 2 &&
         offset + config[offset] <= config_length;
         offset += config[offset]) {
        uint8_t endpoint;
        uint16_t packet;

        if (config[offset + 1] != USB_DT_ENDPOINT ||
            config[offset] < 7 ||
            (config[offset + 3] & 0x03) !=
                USB_ENDPOINT_XFER_BULK) {
            continue;
        }
        endpoint = config[offset + 2];
        packet = lduw_le_p(config + offset + 4) & 0x7ff;
        if (endpoint == (USB_DIR_IN | 1)) {
            bulk_in = packet;
        } else if (endpoint == 2) {
            bulk_out = packet;
        }
    }
    if (!bulk_in || bulk_in != bulk_out) {
        error_setg(errp, "USB boot device %u has invalid BOT endpoints",
                   group);
        return false;
    }
    if (raspi4_usb_control(
            reader, address, USB_TYPE_STANDARD | USB_RECIP_DEVICE,
            USB_REQ_SET_CONFIGURATION, 1, 0, NULL, 0, errp) != 0 ||
        raspi4_usb_control(
            reader, address,
            USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
            0xfe, 0, 0, &max_lun, 1, errp) != 1) {
        return false;
    }
    if (!reader->machine->usb_boot_external) {
        for (unsigned int i = 0;
             i < reader->machine->usb_boot_device_count; i++) {
            if (reader->machine->usb_boot_device_numbers[i] == group) {
                expected = MAX(expected,
                               reader->machine->usb_boot_luns[i]);
            }
        }
        if (max_lun != expected) {
            error_setg(errp,
                       "USB boot device %u reported LUN %u, expected LUN %u",
                       group, max_lun, expected);
            return false;
        }
    }
    reader->addresses[group] = address;
    reader->max_packets[group] = bulk_in;
    reader->max_luns[group] = max_lun;
    reader->group_present[group] = true;
    reader->machine->usb_boot_eligible_device_count++;
    return true;
}

bool raspi4_usb_enumerate(Raspi4UsbBootReader *reader, Error **errp)
{
    uint8_t descriptor[18] = { 0 };
    uint8_t hub_descriptor[16] = { 0 };
    uint8_t hub_address = 1;
    uint32_t hub_root_port = 1;
    bool hybrid_hub = false;
    unsigned int groups = 0;

    memset(reader->addresses, 0, sizeof(reader->addresses));
    memset(reader->max_packets, 0, sizeof(reader->max_packets));
    memset(reader->max_luns, 0, sizeof(reader->max_luns));
    memset(reader->group_present, 0, sizeof(reader->group_present));
    memset(reader->versions, 0, sizeof(reader->versions));
    memset(reader->route_strings, 0, sizeof(reader->route_strings));
    memset(reader->root_hub_ports, 0, sizeof(reader->root_hub_ports));
    reader->group_count = 0;
    if (reader->xhci) {
        if (!xhci_host_firmware_init(
                reader->xhci, RASPI4_XHCI_BOOT_DMA, errp)) {
            return false;
        }
        for (unsigned int port = 0, group = 0;
             port < xhci_host_firmware_port_count(reader->xhci) &&
             (reader->machine->usb_boot_external ||
              group < reader->machine->usb_boot_group_count);
             port++) {
            uint8_t address;

            if (!xhci_host_firmware_reset_port(reader->xhci, port, NULL)) {
                continue;
            }
            memset(descriptor, 0, sizeof(descriptor));
            if (raspi4_usb_control(
                    reader, 0,
                    USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                    USB_REQ_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0,
                    descriptor, sizeof(descriptor), errp) !=
                sizeof(descriptor)) {
                return false;
            }
            if (descriptor[1] != USB_DT_DEVICE) {
                error_setg(errp,
                           "xHCI root boot device descriptor is invalid "
                           "(type=%u class=%u group=%u)",
                           descriptor[1], descriptor[4], group);
                return false;
            }
            if (raspi4_usb_vid_pid_excluded(reader, descriptor)) {
                if (descriptor[4] == USB_CLASS_HUB) {
                    groups = reader->machine->usb_boot_external ?
                        group : reader->machine->usb_boot_group_count;
                    reader->group_count = groups;
                    return true;
                }
                if (!raspi4_usb_set_address(
                        reader, 0, group + 1, errp)) {
                    return false;
                }
                group++;
                groups = group;
                continue;
            }
            if (descriptor[4] == USB_CLASS_HUB) {
                hub_address = reader->machine->usb_boot_group_count + 1;
                if (reader->machine->usb_boot_external) {
                    hub_address = RASPI4_USB_BOOT_MAX_DEVICES + 1;
                }
                hub_root_port = port + 1;
                hybrid_hub = group != 0;
                goto enumerate_hub;
            }
            address = group + 1;
            reader->versions[group] = descriptor[3];
            reader->root_hub_ports[group] = port + 1;
            if (!raspi4_usb_set_address(reader, 0, address, errp) ||
                !raspi4_usb_configure_storage(
                    reader, address, group, errp)) {
                return false;
            }
            group++;
            groups = group;
        }
        if (!reader->machine->usb_boot_external &&
            groups != reader->machine->usb_boot_group_count) {
            error_setg(errp,
                       "xHCI discovered %u USB boot devices, expected %u",
                       groups, reader->machine->usb_boot_group_count);
            return false;
        }
        reader->group_count = groups;
        return true;
    }
    if (!dwc2_host_firmware_reset_port(
            raspi4_dwc2(reader->machine), errp) ||
        raspi4_usb_control(
            reader, 0,
            USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
            USB_REQ_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0,
            descriptor, sizeof(descriptor), errp) != sizeof(descriptor)) {
        return false;
    }
    if (descriptor[1] != USB_DT_DEVICE) {
        error_setg(errp, "DWC2 root device descriptor is invalid");
        return false;
    }

    if (raspi4_usb_vid_pid_excluded(reader, descriptor)) {
        reader->group_count = reader->machine->usb_boot_external ?
            0 : reader->machine->usb_boot_group_count;
        return true;
    }
    if (descriptor[4] != USB_CLASS_HUB) {
        if ((!reader->machine->usb_boot_external &&
             reader->machine->usb_boot_group_count != 1) ||
            !raspi4_usb_set_address(reader, 0, 1, errp)) {
            return false;
        }
        if (!raspi4_usb_configure_storage(reader, 1, 0, errp)) {
            return false;
        }
        reader->versions[0] = descriptor[3];
        reader->root_hub_ports[0] = 1;
        reader->group_count = 1;
        return true;
    }

enumerate_hub:
    if (!raspi4_usb_set_address(reader, 0, hub_address, errp) ||
        raspi4_usb_control(
            reader, hub_address, USB_TYPE_STANDARD | USB_RECIP_DEVICE,
            USB_REQ_SET_CONFIGURATION, 1, 0, NULL, 0, errp) != 0 ||
        raspi4_usb_control(
            reader, hub_address,
            USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE,
            USB_REQ_GET_DESCRIPTOR, 0x2900, 0,
            hub_descriptor, sizeof(hub_descriptor), errp) < 9) {
        return false;
    }
    if (hub_descriptor[1] != 0x29 || !hub_descriptor[2] ||
        hub_descriptor[2] > 8) {
        error_setg(errp, "DWC2 USB hub descriptor is invalid");
        return false;
    }

    for (unsigned int port = 1; port <= hub_descriptor[2]; port++) {
        uint8_t port_status[4] = { 0 };
        uint8_t address;

        if (raspi4_usb_control(
                reader, hub_address,
                USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_OTHER,
                USB_REQ_GET_STATUS, 0, port, port_status,
                sizeof(port_status), errp) != sizeof(port_status)) {
            return false;
        }
        if (!(lduw_le_p(port_status) & 0x0001)) {
            continue;
        }
        if (groups >= RASPI4_USB_BOOT_MAX_DEVICES ||
            (!reader->machine->usb_boot_external &&
             groups >= reader->machine->usb_boot_group_count)) {
            error_setg(errp, "DWC2 discovered an unexpected USB boot device");
            return false;
        }
        if (raspi4_usb_control(
                reader, hub_address, USB_TYPE_CLASS | USB_RECIP_OTHER,
                USB_REQ_SET_FEATURE, 4, port, NULL, 0, errp) != 0) {
            return false;
        }
        memset(descriptor, 0, sizeof(descriptor));
        if (raspi4_usb_control(
                reader, 0,
                USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                USB_REQ_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0,
                descriptor, sizeof(descriptor), errp) != sizeof(descriptor)) {
            return false;
        }
        if (descriptor[1] != USB_DT_DEVICE ||
            descriptor[4] == USB_CLASS_HUB) {
            error_setg(errp, "DWC2 downstream device descriptor is invalid");
            return false;
        }
        if (raspi4_usb_vid_pid_excluded(reader, descriptor)) {
            address = groups + (hybrid_hub ? 1 : 2);
            if (!raspi4_usb_set_address(reader, 0, address, errp)) {
                return false;
            }
            groups++;
            continue;
        }
        address = groups + (hybrid_hub ? 1 : 2);
        reader->versions[groups] = descriptor[3];
        reader->route_strings[groups] = port;
        reader->root_hub_ports[groups] = hub_root_port;
        if (!raspi4_usb_set_address(reader, 0, address, errp) ||
            !raspi4_usb_configure_storage(
                reader, address, groups, errp)) {
            return false;
        }
        groups++;
    }
    if (!reader->machine->usb_boot_external &&
        groups != reader->machine->usb_boot_group_count) {
        error_setg(errp, "DWC2 discovered %u USB boot devices, expected %u",
                   groups, reader->machine->usb_boot_group_count);
        return false;
    }
    reader->group_count = groups;
    return true;
}

static bool raspi4_usb_bot_case_requires_recovery(uint8_t bot_case)
{
    switch (bot_case) {
    case 2:
    case 3:
    case 7:
    case 8:
    case 10:
    case 13:
        return true;
    default:
        return false;
    }
}

unsigned int raspi4_usb_bot_case_recovery_count(uint8_t count)
{
    unsigned int recoveries = 0;

    for (uint8_t bot_case = 1; bot_case <= count; bot_case++) {
        recoveries += raspi4_usb_bot_case_requires_recovery(bot_case);
    }
    return recoveries;
}

static bool raspi4_usb_bot_recover(Raspi4UsbBootReader *reader,
                                   uint8_t address, Error **errp)
{
    uint8_t endpoint_in = USB_DIR_IN | 1;
    uint8_t endpoint_out = 2;

    if (!reader->xhci &&
        !dwc2_host_firmware_abort_transfer(
            raspi4_dwc2(reader->machine), errp)) {
        error_prepend(errp, "USB BOT abort DWC2 host channel: ");
        return false;
    }
    if (raspi4_usb_control(
            reader, address, USB_TYPE_CLASS | USB_RECIP_INTERFACE,
            0xff, 0, 0, NULL, 0, errp) != 0) {
        error_prepend(errp, "USB BOT class reset: ");
        return false;
    }
    if (raspi4_usb_control(
            reader, address, USB_TYPE_STANDARD | USB_RECIP_ENDPOINT,
            USB_REQ_CLEAR_FEATURE, USB_ENDPOINT_HALT, endpoint_in,
            NULL, 0, errp) != 0) {
        error_prepend(errp, "USB BOT clear IN halt: ");
        return false;
    }
    if (raspi4_usb_control(
            reader, address, USB_TYPE_STANDARD | USB_RECIP_ENDPOINT,
            USB_REQ_CLEAR_FEATURE, USB_ENDPOINT_HALT, endpoint_out,
            NULL, 0, errp) != 0) {
        error_prepend(errp, "USB BOT clear OUT halt: ");
        return false;
    }
    if (reader->xhci) {
        if (!xhci_host_firmware_recover_endpoint(
                reader->xhci, address, 1, true, errp)) {
            error_prepend(errp, "USB BOT recover xHCI IN endpoint: ");
            return false;
        }
        if (!xhci_host_firmware_recover_endpoint(
                reader->xhci, address, 2, false, errp)) {
            error_prepend(errp, "USB BOT recover xHCI OUT endpoint: ");
            return false;
        }
    }
    reader->machine->usb_boot_bot_recoveries++;
    return true;
}


static Raspi4UsbBotCase raspi4_usb_bot_prepare_case(uint8_t bot_case,
                                                    uint8_t cbw[31])
{
    Raspi4UsbBotCase result = {
        .requires_recovery =
            raspi4_usb_bot_case_requires_recovery(bot_case),
    };
    uint8_t device_length;

    memset(cbw + 15, 0, 16);
    cbw[14] = 6;
    switch (bot_case) {
    case 1: /* Hn = Dn */
        cbw[15] = 0x00; /* TEST UNIT READY */
        break;
    case 2: /* Hn < Di */
        cbw[15] = 0x12; /* INQUIRY */
        cbw[19] = 36;
        break;
    case 3: /* Hn < Do */
        cbw[15] = 0x15; /* MODE SELECT(6), no-op header */
        cbw[16] = 0x10; /* PF */
        cbw[19] = 4;
        result.data_out = true;
        break;
    case 4: /* Hi > Dn */
        cbw[15] = 0x00;
        result.host_length = 8;
        result.expected_residue = 8;
        break;
    case 5: /* Hi > Di */
        device_length = 8;
        cbw[15] = 0x12;
        cbw[19] = device_length;
        result.host_length = 36;
        result.expected_residue = result.host_length - device_length;
        break;
    case 6: /* Hi = Di */
        cbw[15] = 0x12;
        cbw[19] = 36;
        result.host_length = 36;
        break;
    case 7: /* Hi < Di */
        cbw[15] = 0x12;
        cbw[19] = 36;
        result.host_length = 8;
        break;
    case 8: /* Hi <> Do */
        cbw[15] = 0x15;
        cbw[16] = 0x10;
        cbw[19] = 4;
        result.host_length = 8;
        break;
    case 9: /* Ho > Dn */
        cbw[15] = 0x00;
        result.host_length = 8;
        result.expected_residue = 8;
        result.data_out = true;
        break;
    case 10: /* Ho <> Di */
        cbw[15] = 0x12;
        cbw[19] = 36;
        result.host_length = 8;
        result.data_out = true;
        break;
    case 11: /* Ho > Do */
        cbw[15] = 0x15;
        cbw[16] = 0x10;
        cbw[19] = 4;
        result.host_length = 8;
        result.expected_residue = 4;
        result.data_out = true;
        break;
    case 12: /* Ho = Do */
        cbw[15] = 0x15;
        cbw[16] = 0x10;
        cbw[19] = 4;
        result.host_length = 4;
        result.data_out = true;
        break;
    case 13: /* Ho < Do */
        cbw[15] = 0x15;
        cbw[16] = 0x10;
        cbw[19] = 4;
        result.host_length = 2;
        result.data_out = true;
        break;
    default:
        g_assert_not_reached();
    }
    stl_le_p(cbw + 8, result.host_length);
    cbw[12] = result.data_out ? 0 : USB_DIR_IN;
    return result;
}

static bool raspi4_usb_bot_in(Raspi4UsbBootReader *reader,
                              const uint8_t *cdb, uint8_t cdb_length,
                              uint8_t *data, size_t data_length,
                              uint8_t *command_status, Error **errp)
{
    uint8_t cbw[31] = { 0 };
    uint8_t clean_cbw[31];
    uint8_t csw[13] = { 0 };
    uint8_t group = reader->group;
    uint8_t address = reader->addresses[group];
    uint16_t max_packet = reader->max_packets[group];
    uint32_t tag = ++reader->tag;
    unsigned int recovery_attempts = 0;
    uint8_t injected_faults =
        reader->machine->usb_boot_bot_stall_count ?
        reader->machine->usb_boot_bot_stall_count :
        reader->machine->usb_boot_bot_stall_once;

    if (!address || !max_packet || !cdb_length || cdb_length > 16 ||
        !data_length || data_length > 127 * 512) {
        error_setg(errp, "invalid USB boot BOT command");
        return false;
    }
    stl_le_p(cbw + 4, tag);
    stl_le_p(cbw + 8, data_length);
    cbw[12] = USB_DIR_IN;
    cbw[13] = reader->lun;
    cbw[14] = cdb_length;
    memcpy(cbw + 15, cdb, cdb_length);
    memcpy(clean_cbw, cbw, sizeof(clean_cbw));

    for (;;) {
        Error *transfer_err = NULL;
        uint8_t matrix_data[36] = { 0 };
        uint8_t matrix_case_number = 0;
        uint8_t *transfer_data = data;
        size_t transfer_length = data_length;
        uint32_t expected_residue = 0;
        bool phase_fault = false;
        bool phase_none = false;
        bool phase_out = false;
        bool write_intent = false;
        bool matrix_case = false;
        bool matrix_normal = false;
        ssize_t actual;

        memcpy(cbw, clean_cbw, sizeof(cbw));
        memset(csw, 0, sizeof(csw));
        stl_le_p(cbw, 0x43425355);
        if (reader->machine->usb_boot_bot_stalls_consumed <
            injected_faults) {
            stl_le_p(cbw, 0);
            reader->machine->usb_boot_bot_stalls_consumed++;
        } else if (reader->machine->usb_boot_bot_phases_consumed <
                   reader->machine->usb_boot_bot_phase_count) {
            static const uint8_t phase_cases[] = { 2, 3, 7, 8, 10, 13 };
            uint8_t phase_index =
                reader->machine->usb_boot_bot_phases_consumed++;
            uint8_t phase_case =
                phase_cases[phase_index % ARRAY_SIZE(phase_cases)];

            phase_fault = true;
            reader->machine->usb_boot_bot_phase_errors++;
            switch (phase_case) {
            case 2:
                /* Hn < Di: no host data for the original IN command. */
                stl_le_p(cbw + 8, 0);
                phase_none = true;
                break;
            case 3:
                /*
                 * Hn < Do: no host data for an out-of-range WRITE(10).
                 */
                stl_le_p(cbw + 8, 0);
                phase_none = true;
                write_intent = true;
                break;
            case 7:
                /*
                 * Hi < Di: the host allocation is shorter than INQUIRY's
                 * device intent.
                 */
                memset(cbw + 15, 0, 16);
                cbw[14] = 6;
                cbw[15] = 0x12;
                cbw[19] = 36;
                break;
            case 8:
                /* Hi <> Do: host IN for an out-of-range WRITE(10). */
                write_intent = true;
                break;
            case 10:
                /* Ho <> Di: host OUT for the original IN command. */
                cbw[12] = 0;
                phase_out = true;
                break;
            case 13:
                /* Ho < Do: short host OUT for an out-of-range WRITE(10). */
                phase_out = true;
                write_intent = true;
                break;
            default:
                g_assert_not_reached();
            }
            if (write_intent) {
                memset(cbw + 15, 0, 16);
                cbw[14] = 10;
                cbw[15] = 0x2a;
                memset(cbw + 17, 0xff, 4);
                cbw[23] = 1;
            }
        } else if (reader->machine->usb_boot_bot_cases_consumed <
                   reader->machine->usb_boot_bot_case_count) {
            Raspi4UsbBotCase bot_case;

            matrix_case_number =
                ++reader->machine->usb_boot_bot_cases_consumed;
            bot_case = raspi4_usb_bot_prepare_case(matrix_case_number, cbw);
            reader->machine->usb_boot_bot_cases_tested++;
            matrix_case = true;
            matrix_normal = !bot_case.requires_recovery;
            phase_fault = bot_case.requires_recovery;
            phase_none = bot_case.host_length == 0;
            phase_out = bot_case.data_out;
            transfer_data = matrix_data;
            transfer_length = bot_case.host_length;
            expected_residue = bot_case.expected_residue;
            if (phase_fault) {
                reader->machine->usb_boot_bot_phase_errors++;
            }
        }
        actual = raspi4_usb_host_transfer(
            reader, address, 2, USB_ENDPOINT_XFER_BULK, false,
            TSIZ_SC_MC_PID_DATA0, cbw, sizeof(cbw), &transfer_err);
        if (actual != sizeof(cbw)) {
            if (!transfer_err) {
                error_setg(&transfer_err, "USB boot BOT CBW was truncated");
            }
            goto transport_failure;
        }
        if (!phase_none) {
            actual = raspi4_usb_host_transfer(
                reader, address, phase_out ? 2 : 1,
                USB_ENDPOINT_XFER_BULK, !phase_out,
                TSIZ_SC_MC_PID_DATA1, transfer_data, transfer_length,
                &transfer_err);
            if (actual != transfer_length) {
                if (!transfer_err) {
                    error_setg(&transfer_err,
                               "USB boot BOT data phase was truncated");
                }
                goto transport_failure;
            }
        }
        actual = raspi4_usb_host_transfer(
            reader, address, 1, USB_ENDPOINT_XFER_BULK, true,
            TSIZ_SC_MC_PID_DATA1, csw, sizeof(csw), &transfer_err);
        if (actual != sizeof(csw)) {
            if (!transfer_err) {
                error_setg(&transfer_err, "USB boot BOT CSW was truncated");
            }
            goto transport_failure;
        }
        if (ldl_le_p(csw) != 0x53425355 ||
            ldl_le_p(csw + 4) != tag ||
            ldl_le_p(csw + 8) > transfer_length || csw[12] > 2) {
            error_setg(&transfer_err,
                       "USB boot BOT command returned an invalid CSW");
            goto transport_failure;
        }
        if (phase_fault || csw[12] == 2) {
            error_setg(&transfer_err,
                       "USB boot BOT command requires phase-error recovery");
            goto transport_failure;
        }
        if (matrix_normal) {
            if (ldl_le_p(csw + 8) != expected_residue) {
                error_setg(&transfer_err,
                           "USB-IF BOT case %u returned status %u and "
                           "residue %u, expected non-phase status and "
                           "residue %u",
                           matrix_case_number, csw[12],
                           ldl_le_p(csw + 8), expected_residue);
                goto transport_failure;
            }
            continue;
        }
        *command_status = csw[12];
        if (*command_status) {
            reader->machine->usb_boot_controller_failures++;
        }
        return true;

transport_failure:
        if (matrix_case && matrix_normal) {
            error_prepend(&transfer_err, "USB-IF BOT case %u: ",
                          matrix_case_number);
            error_propagate(errp, transfer_err);
            return false;
        }
        if (recovery_attempts < RASPI4_USB_BOT_MAX_RECOVERIES) {
            error_free(transfer_err);
            if (raspi4_usb_bot_recover(reader, address, errp)) {
                recovery_attempts++;
                continue;
            }
            if (matrix_case) {
                error_prepend(errp, "USB-IF BOT case %u: ",
                              matrix_case_number);
            }
            return false;
        }
        error_propagate(errp, transfer_err);
        return false;
    }
    g_assert_not_reached();
}

bool raspi4_usb_prepare_reader(Raspi4UsbBootReader *reader,
                                      unsigned int group, unsigned int lun,
                                      Error **errp)
{
    uint8_t cdb[10] = { 0x25 };
    uint8_t capacity[8] = { 0 };
    uint8_t status;
    uint32_t last_lba;
    uint32_t block_size;

    reader->group = group;
    reader->lun = lun;
    for (unsigned int attempt = 0; attempt < 2; attempt++) {
        memset(capacity, 0, sizeof(capacity));
        if (!raspi4_usb_bot_in(
                reader, cdb, sizeof(cdb), capacity, sizeof(capacity),
                &status, errp)) {
            return false;
        }
        if (status == 0) {
            break;
        }
    }
    if (status != 0) {
        error_setg(errp, "USB boot LUN %u did not become ready", reader->lun);
        return false;
    }
    last_lba = ldl_be_p(capacity);
    block_size = ldl_be_p(capacity + 4);
    if (block_size != 512 || last_lba == UINT32_MAX) {
        error_setg(errp, "USB boot LUN %u has unsupported capacity",
                   reader->lun);
        return false;
    }
    reader->size = ((uint64_t)last_lba + 1) * block_size;
    return true;
}

bool raspi4_usb_media_read(void *opaque, int64_t offset,
                                  int64_t bytes, void *buffer,
                                  Error **errp)
{
    Raspi4UsbBootReader *reader = opaque;
    uint8_t *output = buffer;

    while (bytes) {
        uint8_t cdb[10] = { 0x28 };
        uint64_t lba = offset / 512;
        size_t leading = offset % 512;
        size_t needed = leading + bytes;
        uint16_t blocks = MIN(
            DIV_ROUND_UP(needed, 512), (size_t)127);
        size_t transfer = blocks * 512;
        size_t copy = MIN((int64_t)(transfer - leading), bytes);
        uint8_t status;

        if (lba > UINT32_MAX) {
            error_setg(errp, "USB boot READ(10) LBA exceeds 32 bits");
            return false;
        }
        stl_be_p(cdb + 2, lba);
        stw_be_p(cdb + 7, blocks);
        if (!raspi4_usb_bot_in(
                reader, cdb, sizeof(cdb), reader->bounce, transfer,
                &status, errp)) {
            return false;
        }
        if (status != 0) {
            error_setg(errp, "USB boot READ(10) failed for LUN %u",
                       reader->lun);
            return false;
        }
        reader->machine->usb_boot_controller_reads++;
        reader->machine->usb_boot_controller_bytes += transfer;
        memcpy(output, reader->bounce + leading, copy);
        output += copy;
        offset += copy;
        bytes -= copy;
    }
    return true;
}

bool raspi4_usb_media_present(const Raspi4bMachineState *s)
{
    if (s->usb_boot_external) {
        return true;
    }
    for (unsigned int i = 0; i < s->usb_boot_device_count; i++) {
        if (s->usb_boot_devices[i] &&
            blk_is_inserted(s->usb_boot_devices[i])) {
            return true;
        }
    }
    return false;
}

bool raspi4_usb_only_excluded(const Raspi4bMachineState *s)
{
    return s->usb_boot_excluded_device_count &&
           !s->usb_boot_eligible_device_count;
}

bool raspi4_usb_legacy_power_cycle(const Raspi4bMachineState *s)
{
    return !s->cm4 && (s->board_revision & 0xf) <= 3;
}

/*
 * Pi 4B PCB revisions through 1.3 briefly restore USB power before the
 * bootloader can request a second, configurable power-off interval.  From
 * revision 1.4, hardware holds USB power off from reset; memory
 * initialization overlaps at least two seconds of that interval.
 */
/*
 * Pi 4B PCB revisions through 1.3 briefly restore USB power before the
 * bootloader can request a second, configurable power-off interval.  From
 * revision 1.4, hardware holds USB power off from reset; memory
 * initialization overlaps at least two seconds of that interval.
 */
bool raspi4_usb_power_prepare(Raspi4bMachineState *s,
                                     const char *source)
{
    uint64_t already_off;
    uint32_t remaining;

    if (s->cm4 || s->usb_power_off_consumed) {
        return true;
    }
    s->usb_power_off_consumed = true;
    if (s->usb_power_cycle_legacy) {
        already_off = 0;
        remaining = s->usb_msd_power_off_time;
    } else {
        already_off = RASPI4_USB_MSD_NEW_BOARD_MIN_OFF_MS +
                      s->boot_elapsed_ms;
        remaining = already_off >= s->usb_msd_power_off_time ?
                    0 : s->usb_msd_power_off_time - already_off;
    }
    s->usb_power_off_elapsed_ms = already_off;
    if (remaining) {
        raspi4_schedule_usb_power_off(s, source, remaining);
        return false;
    }
    s->usb_power_enabled = true;
    trace_raspi4b_boot_event("boot-source", "boot-source.usb-power",
                             source, "power-on",
                             s->usb_power_off_elapsed_ms);
    return true;
}

bool raspi4_usb_controller_bootable(Raspi4bMachineState *s,
                                            const char *source)
{
    /*
     * Pi 4B has its VL805 onboard.  CM4 only initializes a separately
     * attached VL805 from the copy embedded in bootloader EEPROM when the
     * documented VL805=1 policy is enabled.
     */
    if (!strcmp(source, "usb-msd") && s->cm4 &&
        s->usb_boot_on_xhci && !s->vl805_initialized) {
        raspi4_set_firmware_status(
            s, s->vl805_enabled ? "vl805-controller-missing" :
                                  "vl805-disabled");
        trace_raspi4b_boot_event(
            "boot-source", "boot-source.vl805", source, "failure",
            s->vl805_enabled);
        return false;
    }
    return true;
}

void raspi4_usb_continue_after_power(Raspi4bMachineState *s,
                                             const char *source)
{
    Raspi4BootAttemptResult result;

    if (s->usb_msd_startup_delay) {
        raspi4_schedule_usb_startup(s, source);
        return;
    }
    if (!raspi4_usb_media_present(s)) {
        raspi4_schedule_usb_wait(s, source, true);
        return;
    }
    result = raspi4_try_usb_firmware(s, source);
    if (result == RASPI4_BOOT_ATTEMPT_READY) {
        raspi4_set_boot_observation(s, "arm-handoff-ready", source);
        trace_raspi4b_boot_event("boot-source", "boot-source.attempt",
                                 source, "success", s->firmware_size);
    } else if (result != RASPI4_BOOT_ATTEMPT_PENDING) {
        raspi4_schedule_usb_wait(
            s, source, raspi4_usb_only_excluded(s));
    }
}

static bool raspi4_usb_wait_pending(const Raspi4bMachineState *s)
{
    return s->pending_boot_action == RASPI4_PENDING_USB_DISCOVERY ||
           s->pending_boot_action == RASPI4_PENDING_USB_LUN;
}

void raspi4_usb_hotplug_bh(void *opaque)
{
    Raspi4bMachineState *s = opaque;
    uint32_t nibble;
    const char *source;
    Raspi4BootAttemptResult result;

    s->usb_boot_hotplug_pending = false;
    if (!raspi4_usb_wait_pending(s)) {
        return;
    }
    nibble = (s->boot_order >> (s->boot_order_index * 4)) & 0xf;
    source = raspi4_boot_source_name(s, nibble);
    if (!raspi4_usb_media_present(s)) {
        if (s->pending_boot_action == RASPI4_PENDING_USB_LUN) {
            timer_del(s->boot_timeout_timer);
            s->pending_boot_action = RASPI4_PENDING_NONE;
            trace_raspi4b_boot_event("boot-source", "boot-source.hotplug",
                                     source, "removed",
                                     s->usb_msd_discover_timeout);
            raspi4_schedule_usb_wait(s, source, true);
        }
        return;
    }
    timer_del(s->boot_timeout_timer);
    s->pending_boot_action = RASPI4_PENDING_NONE;
    result = raspi4_try_usb_firmware(s, source);

    if (result == RASPI4_BOOT_ATTEMPT_READY) {
        raspi4_set_boot_observation(s, "arm-handoff-ready", source);
        trace_raspi4b_boot_event("boot-source", "boot-source.hotplug",
                                 source, "success", s->firmware_size);
    } else if (result != RASPI4_BOOT_ATTEMPT_PENDING) {
        bool excluded_only = raspi4_usb_only_excluded(s);

        trace_raspi4b_boot_event("boot-source", "boot-source.hotplug",
                                 source,
                                 excluded_only ? "discover-wait" :
                                                 "lun-wait",
                                 excluded_only ?
                                     s->usb_msd_discover_timeout :
                                     s->usb_msd_lun_timeout);
        raspi4_schedule_usb_wait(s, source, excluded_only);
    }
}

void raspi4_usb_media_inserted(Notifier *notifier, void *data)
{
    Raspi4UsbBootNotifier *entry = container_of(
        notifier, Raspi4UsbBootNotifier, notifier);
    Raspi4bMachineState *s = entry->machine;

    for (unsigned int i = 0; i < s->usb_boot_device_count; i++) {
        if (data != s->usb_boot_devices[i]) {
            continue;
        }
        s->usb_boot_hotplug_pending = true;
        qemu_bh_schedule(s->usb_boot_hotplug_bh);
        return;
    }
}

void raspi4_usb_media_removed(Notifier *notifier, void *data)
{
    Raspi4UsbBootNotifier *entry = container_of(
        notifier, Raspi4UsbBootNotifier, notifier);
    Raspi4bMachineState *s = entry->machine;

    for (unsigned int i = 0; i < s->usb_boot_device_count; i++) {
        if (data != s->usb_boot_devices[i]) {
            continue;
        }
        s->usb_boot_hotplug_pending = true;
        qemu_bh_schedule(s->usb_boot_hotplug_bh);
        return;
    }
}

bool raspi4_usb_net_install_fallback(Raspi4bMachineState *s,
                                            uint32_t nibble)
{
    if (nibble != 0x4 || !s->net_install_enabled ||
        s->net_install_usb_fallback_consumed ||
        !s->net_install_keyboard_present ||
        !s->net_install_shift_held ||
        !s->usb_boot_files_missing) {
        return false;
    }
    s->net_install_usb_fallback_consumed = true;
    trace_raspi4b_boot_event(
        "boot-source", "boot-source.net-install", "usb-msd",
        "no-boot-files", s->boot_attempt_count);
    raspi4_net_install_request_from(s, "usb-msd");
    return true;
}

void raspi4b_get_rpiboot_bootcode_size(Object *obj, Visitor *v,
                                               const char *name,
                                               void *opaque, Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->rpiboot_bootcode_size;

    visit_type_uint32(v, name, &value, errp);
}

void raspi4b_get_rpiboot_transfer_received(Object *obj, Visitor *v,
                                                   const char *name,
                                                   void *opaque,
                                                   Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->rpiboot_bulk_received;

    visit_type_uint32(v, name, &value, errp);
}

void raspi4b_get_rpiboot_configuration(Object *obj, Visitor *v,
                                               const char *name,
                                               void *opaque, Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->rpiboot_configuration;

    visit_type_uint32(v, name, &value, errp);
}

static char *raspi4_rpiboot_sha256(const uint8_t *data, uint32_t size,
                                   Error **errp)
{
    struct iovec iov = {
        .iov_base = (void *)data,
        .iov_len = size,
    };
    char *digest = NULL;

    if (!size) {
        return g_strdup("");
    }
    if (qcrypto_hash_digestv(QCRYPTO_HASH_ALGO_SHA256, &iov, 1,
                             &digest, errp) < 0) {
        return NULL;
    }
    return digest;
}

char *raspi4b_get_rpiboot_bootcode_sha256(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return raspi4_rpiboot_sha256(s->rpiboot_bootcode,
                                 s->rpiboot_bootcode_size, errp);
}

void raspi4b_get_rpiboot_config_size(Object *obj, Visitor *v,
                                             const char *name, void *opaque,
                                             Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->rpiboot_config_size;

    visit_type_uint32(v, name, &value, errp);
}

char *raspi4b_get_rpiboot_config_sha256(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return raspi4_rpiboot_sha256(s->rpiboot_config,
                                 s->rpiboot_config_size, errp);
}

void raspi4b_get_rpiboot_boot_img_size(Object *obj, Visitor *v,
                                               const char *name, void *opaque,
                                               Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->rpiboot_boot_img_size;

    visit_type_uint32(v, name, &value, errp);
}

char *raspi4b_get_rpiboot_boot_img_sha256(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return raspi4_rpiboot_sha256(s->rpiboot_boot_img,
                                 s->rpiboot_boot_img_size, errp);
}

char *raspi4b_get_usb_boot_drive(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->usb_boot_drive ? s->usb_boot_drive : "");
}

void raspi4b_set_usb_boot_drive(Object *obj, const char *value,
                                       Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    g_free(s->usb_boot_drive);
    s->usb_boot_drive = value[0] ? g_strdup(value) : NULL;
}

char *raspi4b_get_usb_boot_drives(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->usb_boot_drives ? s->usb_boot_drives : "");
}

void raspi4b_set_usb_boot_drives(Object *obj, const char *value,
                                        Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    g_free(s->usb_boot_drives);
    s->usb_boot_drives = value[0] ? g_strdup(value) : NULL;
}

char *raspi4b_get_usb_boot_controller(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->usb_boot_controller ?
                    s->usb_boot_controller : "auto");
}

void raspi4b_set_usb_boot_controller(Object *obj, const char *value,
                                             Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    if (strcmp(value, "auto") && strcmp(value, "xhci") &&
        strcmp(value, "dwc2")) {
        error_setg(errp,
                   "usb-boot-controller must be auto, xhci, or dwc2");
        return;
    }
    g_free(s->usb_boot_controller);
    s->usb_boot_controller = g_strdup(value);
}

bool raspi4b_get_usb_boot_external(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->usb_boot_external;
}

void raspi4b_set_usb_boot_external(Object *obj, bool value,
                                           Error **errp)
{
    RASPI4B_MACHINE(obj)->usb_boot_external = value;
}

bool raspi4b_get_usb_boot_bot_stall_once(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->usb_boot_bot_stall_once;
}

void raspi4b_set_usb_boot_bot_stall_once(Object *obj, bool value,
                                                 Error **errp)
{
    RASPI4B_MACHINE(obj)->usb_boot_bot_stall_once = value;
}

void raspi4b_get_usb_boot_bot_stall_count(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint8_t value = RASPI4B_MACHINE(obj)->usb_boot_bot_stall_count;

    visit_type_uint8(v, name, &value, errp);
}

void raspi4b_set_usb_boot_bot_stall_count(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint8_t value;

    if (!visit_type_uint8(v, name, &value, errp)) {
        return;
    }
    if (value > RASPI4_USB_BOT_MAX_RECOVERIES) {
        error_setg(errp, "usb-boot-bot-stall-count must be between 0 and %u",
                   RASPI4_USB_BOT_MAX_RECOVERIES);
        return;
    }
    RASPI4B_MACHINE(obj)->usb_boot_bot_stall_count = value;
}

void raspi4b_get_usb_boot_bot_phase_count(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint8_t value = RASPI4B_MACHINE(obj)->usb_boot_bot_phase_count;

    visit_type_uint8(v, name, &value, errp);
}

void raspi4b_set_usb_boot_bot_phase_count(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint8_t value;

    if (!visit_type_uint8(v, name, &value, errp)) {
        return;
    }
    if (value > RASPI4_USB_BOT_MAX_RECOVERIES) {
        error_setg(errp, "usb-boot-bot-phase-count must be between 0 and %u",
                   RASPI4_USB_BOT_MAX_RECOVERIES);
        return;
    }
    RASPI4B_MACHINE(obj)->usb_boot_bot_phase_count = value;
}

void raspi4b_get_usb_boot_bot_case_count(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint8_t value = RASPI4B_MACHINE(obj)->usb_boot_bot_case_count;

    visit_type_uint8(v, name, &value, errp);
}

void raspi4b_set_usb_boot_bot_case_count(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint8_t value;

    if (!visit_type_uint8(v, name, &value, errp)) {
        return;
    }
    if (value > RASPI4_USB_BOT_CASE_COUNT) {
        error_setg(errp, "usb-boot-bot-case-count must be between 0 and %u",
                   RASPI4_USB_BOT_CASE_COUNT);
        return;
    }
    RASPI4B_MACHINE(obj)->usb_boot_bot_case_count = value;
}

char *raspi4b_get_rpiboot_bootcode_trusted_sha256(Object *obj,
                                                          Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->rpiboot_bootcode_trusted_sha256 ?
                    s->rpiboot_bootcode_trusted_sha256 : "");
}

char *raspi4b_get_rpiboot_bootcode_trust(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    if (!s->rpiboot_bootcode_size) {
        return g_strdup("not-checked");
    }
    if (!s->rpiboot_bootcode_trusted_sha256) {
        return g_strdup("missing");
    }
    return g_strdup(s->rpiboot_bootcode_trusted ? "trusted" : "mismatch");
}

bool raspi4b_get_usb_boot_files_missing(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->usb_boot_files_missing;
}

void raspi4b_get_usb_msd_discover_timeout(Object *obj, Visitor *v,
                                                  const char *name,
                                                  void *opaque, Error **errp)
{
    uint32_t value =
        RASPI4B_MACHINE(obj)->usb_msd_discover_timeout;

    visit_type_uint32(v, name, &value, errp);
}

void raspi4b_get_usb_msd_lun_timeout(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->usb_msd_lun_timeout;

    visit_type_uint32(v, name, &value, errp);
}

void raspi4b_get_usb_msd_startup_delay(Object *obj, Visitor *v,
                                               const char *name, void *opaque,
                                               Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->usb_msd_startup_delay;

    visit_type_uint32(v, name, &value, errp);
}

void raspi4b_get_usb_msd_power_off_time(Object *obj, Visitor *v,
                                                const char *name,
                                                void *opaque, Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->usb_msd_power_off_time;

    visit_type_uint32(v, name, &value, errp);
}

bool raspi4b_get_usb_power_enabled(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->usb_power_enabled;
}

bool raspi4b_get_usb_power_off_applicable(Object *obj, Error **errp)
{
    return !RASPI4B_MACHINE(obj)->cm4;
}

char *raspi4b_get_usb_power_cycle_mode(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    return g_strdup(s->cm4 ? "not-applicable" :
                    s->usb_power_cycle_legacy ?
                        "legacy-short-then-configurable" :
                        "reset-held-with-memory-init-overlap");
}

void raspi4b_get_usb_power_off_elapsed(Object *obj, Visitor *v,
                                               const char *name,
                                               void *opaque, Error **errp)
{
    uint64_t value = RASPI4B_MACHINE(obj)->usb_power_off_elapsed_ms;

    visit_type_uint64(v, name, &value, errp);
}

void raspi4b_get_usb_power_off_remaining(Object *obj, Visitor *v,
                                                 const char *name,
                                                 void *opaque, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    uint64_t value = 0;

    if (s->pending_boot_action == RASPI4_PENDING_USB_POWER_OFF &&
        timer_pending(s->boot_timeout_timer)) {
        uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        uint64_t expiry = timer_expire_time_ns(s->boot_timeout_timer);

        if (expiry > now) {
            value = expiry - now;
        }
    }
    visit_type_uint64(v, name, &value, errp);
}

void raspi4b_get_usb_boot_selected_index(Object *obj, Visitor *v,
                                                const char *name,
                                                void *opaque, Error **errp)
{
    uint8_t value = RASPI4B_MACHINE(obj)->usb_boot_selected_index;

    visit_type_uint8(v, name, &value, errp);
}

void raspi4b_get_usb_boot_selected_device(Object *obj, Visitor *v,
                                                 const char *name,
                                                 void *opaque, Error **errp)
{
    uint8_t value = RASPI4B_MACHINE(obj)->usb_boot_selected_device;
    visit_type_uint8(v, name, &value, errp);
}

void raspi4b_get_usb_boot_selected_lun(Object *obj, Visitor *v,
                                             const char *name,
                                             void *opaque, Error **errp)
{
    uint8_t value = RASPI4B_MACHINE(obj)->usb_boot_selected_lun;
    visit_type_uint8(v, name, &value, errp);
}

bool raspi4b_get_usb_boot_identity_valid(Object *obj, Error **errp)
{
    return RASPI4B_MACHINE(obj)->usb_boot_identity_valid;
}

void raspi4b_get_usb_boot_version(Object *obj, Visitor *v,
                                         const char *name, void *opaque,
                                         Error **errp)
{
    uint8_t value = RASPI4B_MACHINE(obj)->usb_boot_version;

    visit_type_uint8(v, name, &value, errp);
}

void raspi4b_get_usb_boot_route_string(Object *obj, Visitor *v,
                                              const char *name, void *opaque,
                                              Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->usb_boot_route_string;

    visit_type_uint32(v, name, &value, errp);
}

void raspi4b_get_usb_boot_root_hub_port(Object *obj, Visitor *v,
                                               const char *name, void *opaque,
                                               Error **errp)
{
    uint32_t value = RASPI4B_MACHINE(obj)->usb_boot_root_hub_port;

    visit_type_uint32(v, name, &value, errp);
}

char *raspi4b_get_usb_msd_exclude_vid_pid(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);
    GString *value = g_string_new(NULL);

    for (unsigned int i = 0; i < s->usb_msd_exclude_count; i++) {
        g_string_append_printf(value, "%s%08x", i ? "," : "",
                               s->usb_msd_exclude_vid_pid[i]);
    }
    return g_string_free(value, false);
}

void raspi4b_get_usb_boot_excluded_device_count(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint8_t value =
        RASPI4B_MACHINE(obj)->usb_boot_excluded_device_count;

    visit_type_uint8(v, name, &value, errp);
}

void raspi4b_get_usb_boot_eligible_device_count(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint8_t value =
        RASPI4B_MACHINE(obj)->usb_boot_eligible_device_count;

    visit_type_uint8(v, name, &value, errp);
}

void raspi4b_get_usb_boot_last_excluded_vid_pid(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint32_t value =
        RASPI4B_MACHINE(obj)->usb_boot_last_excluded_vid_pid;

    visit_type_uint32(v, name, &value, errp);
}

char *raspi4b_get_usb_boot_transport(Object *obj, Error **errp)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(obj);

    if (!s->behavioral_boot ||
        (!s->usb_boot_device_count && !s->usb_boot_external)) {
        return g_strdup("inactive");
    }
    return g_strdup(s->usb_boot_on_xhci ?
                    "vl805-xhci-host-bot-scsi-read10-v1" :
                    "dwc2-host-bot-scsi-read10-v1");
}

void raspi4b_get_usb_boot_controller_reads(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint64_t value = RASPI4B_MACHINE(obj)->usb_boot_controller_reads;

    visit_type_uint64(v, name, &value, errp);
}

void raspi4b_get_usb_boot_controller_bytes(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint64_t value = RASPI4B_MACHINE(obj)->usb_boot_controller_bytes;

    visit_type_uint64(v, name, &value, errp);
}

void raspi4b_get_usb_boot_controller_failures(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint64_t value = RASPI4B_MACHINE(obj)->usb_boot_controller_failures;

    visit_type_uint64(v, name, &value, errp);
}

void raspi4b_get_usb_boot_bot_recoveries(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint64_t value = RASPI4B_MACHINE(obj)->usb_boot_bot_recoveries;

    visit_type_uint64(v, name, &value, errp);
}

void raspi4b_get_usb_boot_bot_phase_errors(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint64_t value = RASPI4B_MACHINE(obj)->usb_boot_bot_phase_errors;

    visit_type_uint64(v, name, &value, errp);
}

void raspi4b_get_usb_boot_bot_cases_tested(
    Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint64_t value = RASPI4B_MACHINE(obj)->usb_boot_bot_cases_tested;

    visit_type_uint64(v, name, &value, errp);
}

bool raspi4_usb_boot_uses_xhci(const Raspi4bMachineState *s)
{
    if (s->usb_boot_controller) {
        if (!strcmp(s->usb_boot_controller, "xhci")) {
            return true;
        }
        if (!strcmp(s->usb_boot_controller, "dwc2")) {
            return false;
        }
    }
    return !s->cm4;
}
