/*
 * Raspberry Pi emulation (c) 2012 Gregory Estrade
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/misc/bcm2835_property.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "hw/core/irq.h"
#include "hw/misc/bcm2835_mbox_defs.h"
#include "hw/arm/raspberrypi-fw-defs.h"
#include "system/dma.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"
#include "hw/arm/raspi_platform.h"

#define VCHI_BUSADDR_SIZE       sizeof(uint32_t)
#define RPI_EXP_GPIO_BASE       128
#define DEFAULT_DISPLAY_REFRESH_HZ 60

static int bcm2835_property_exp_gpio_index(uint32_t gpio)
{
    if (gpio < RPI_EXP_GPIO_BASE ||
        gpio >= RPI_EXP_GPIO_BASE + BCM2835_PROPERTY_EXP_GPIO_COUNT) {
        return -1;
    }
    return gpio - RPI_EXP_GPIO_BASE;
}

static void bcm2835_property_exp_gpio_update(BCM2835PropertyState *s,
                                              int line);

static void bcm2835_property_exp_gpio_input(void *opaque, int line, int level)
{
    BCM2835PropertyState *s = opaque;

    if (!s->exp_gpio_direction[line]) {
        s->exp_gpio_state[line] = !!level;
        bcm2835_property_exp_gpio_update(s, line);
    }
}

static void bcm2835_property_exp_gpio_update(BCM2835PropertyState *s,
                                              int line)
{
    qemu_set_irq(s->exp_gpio_out[line], s->exp_gpio_state[line]);
}

static bool bcm2835_property_clock_mux(uint32_t clock_id,
                                       CprmanClockMux *mux)
{
    switch (clock_id) {
    case RPI_FIRMWARE_EMMC_CLK_ID:
        *mux = CPRMAN_CLOCK_EMMC;
        return true;
    case RPI_FIRMWARE_UART_CLK_ID:
        *mux = CPRMAN_CLOCK_UART;
        return true;
    case RPI_FIRMWARE_ARM_CLK_ID:
        *mux = CPRMAN_CLOCK_ARM;
        return true;
    case RPI_FIRMWARE_CORE_CLK_ID:
        *mux = CPRMAN_CLOCK_VPU;
        return true;
    case RPI_FIRMWARE_V3D_CLK_ID:
        *mux = CPRMAN_CLOCK_V3D;
        return true;
    case RPI_FIRMWARE_H264_CLK_ID:
        *mux = CPRMAN_CLOCK_H264;
        return true;
    case RPI_FIRMWARE_ISP_CLK_ID:
        *mux = CPRMAN_CLOCK_ISP;
        return true;
    case RPI_FIRMWARE_PWM_CLK_ID:
        *mux = CPRMAN_CLOCK_PWM;
        return true;
    case RPI_FIRMWARE_EMMC2_CLK_ID:
        *mux = CPRMAN_CLOCK_EMMC2;
        return true;
    case RPI_FIRMWARE_VEC_CLK_ID:
        *mux = CPRMAN_CLOCK_VEC;
        return true;
    default:
        return false;
    }
}

static bool bcm2835_property_power_device_is_valid(uint32_t device_id)
{
    return device_id < BCM2835_PROPERTY_POWER_DEVICE_COUNT;
}

static const uint32_t bcm2835_property_display_ids[
    BCM2835_PROPERTY_DISPLAY_COUNT] = {
    2, /* HDMI0 */
    7, /* HDMI1 */
};

static int bcm2835_property_display_index_from_id(uint32_t display_id)
{
    for (size_t index = 0;
         index < ARRAY_SIZE(bcm2835_property_display_ids); index++) {
        if (bcm2835_property_display_ids[index] == display_id) {
            return index;
        }
    }
    return -1;
}

static uint32_t bcm2835_property_display_id_from_index(uint32_t index)
{
    return index < ARRAY_SIZE(bcm2835_property_display_ids) ?
        bcm2835_property_display_ids[index] : UINT32_MAX;
}

static bool bcm2835_property_is_framebuffer_test(uint32_t tag)
{
    switch (tag) {
    case RPI_FWREQ_FRAMEBUFFER_TEST_PHYSICAL_WIDTH_HEIGHT:
    case RPI_FWREQ_FRAMEBUFFER_TEST_VIRTUAL_WIDTH_HEIGHT:
    case RPI_FWREQ_FRAMEBUFFER_TEST_DEPTH:
    case RPI_FWREQ_FRAMEBUFFER_TEST_PIXEL_ORDER:
    case RPI_FWREQ_FRAMEBUFFER_TEST_ALPHA_MODE:
    case RPI_FWREQ_FRAMEBUFFER_TEST_VIRTUAL_OFFSET:
    case RPI_FWREQ_FRAMEBUFFER_TEST_OVERSCAN:
    case RPI_FWREQ_FRAMEBUFFER_TEST_PALETTE:
    case RPI_FWREQ_FRAMEBUFFER_TEST_LAYER:
    case RPI_FWREQ_FRAMEBUFFER_TEST_TRANSFORM:
    case RPI_FWREQ_FRAMEBUFFER_TEST_VSYNC:
        return true;
    default:
        return false;
    }
}

static bool bcm2835_property_is_framebuffer_transaction(uint32_t tag)
{
    if (bcm2835_property_is_framebuffer_test(tag)) {
        return true;
    }

    switch (tag) {
    case RPI_FWREQ_SET_CURSOR_INFO:
    case RPI_FWREQ_SET_CURSOR_STATE:
    case RPI_FWREQ_FRAMEBUFFER_ALLOCATE:
    case RPI_FWREQ_FRAMEBUFFER_RELEASE:
    case RPI_FWREQ_FRAMEBUFFER_BLANK:
    case RPI_FWREQ_FRAMEBUFFER_GET_PHYSICAL_WIDTH_HEIGHT:
    case RPI_FWREQ_FRAMEBUFFER_SET_PHYSICAL_WIDTH_HEIGHT:
    case RPI_FWREQ_FRAMEBUFFER_GET_VIRTUAL_WIDTH_HEIGHT:
    case RPI_FWREQ_FRAMEBUFFER_SET_VIRTUAL_WIDTH_HEIGHT:
    case RPI_FWREQ_FRAMEBUFFER_GET_DEPTH:
    case RPI_FWREQ_FRAMEBUFFER_SET_DEPTH:
    case RPI_FWREQ_FRAMEBUFFER_GET_PIXEL_ORDER:
    case RPI_FWREQ_FRAMEBUFFER_SET_PIXEL_ORDER:
    case RPI_FWREQ_FRAMEBUFFER_GET_ALPHA_MODE:
    case RPI_FWREQ_FRAMEBUFFER_SET_ALPHA_MODE:
    case RPI_FWREQ_FRAMEBUFFER_GET_PITCH:
    case RPI_FWREQ_FRAMEBUFFER_GET_VIRTUAL_OFFSET:
    case RPI_FWREQ_FRAMEBUFFER_SET_VIRTUAL_OFFSET:
    case RPI_FWREQ_FRAMEBUFFER_GET_OVERSCAN:
    case RPI_FWREQ_FRAMEBUFFER_SET_OVERSCAN:
    case RPI_FWREQ_FRAMEBUFFER_GET_PALETTE:
    case RPI_FWREQ_FRAMEBUFFER_SET_PALETTE:
    case RPI_FWREQ_FRAMEBUFFER_GET_LAYER:
    case RPI_FWREQ_FRAMEBUFFER_SET_LAYER:
    case RPI_FWREQ_FRAMEBUFFER_GET_TRANSFORM:
    case RPI_FWREQ_FRAMEBUFFER_SET_TRANSFORM:
    case RPI_FWREQ_FRAMEBUFFER_GET_VSYNC:
    case RPI_FWREQ_FRAMEBUFFER_SET_VSYNC:
    case RPI_FWREQ_FRAMEBUFFER_GET_TOUCHBUF:
    case RPI_FWREQ_FRAMEBUFFER_SET_TOUCHBUF:
    case RPI_FWREQ_FRAMEBUFFER_GET_GPIOVIRTBUF:
    case RPI_FWREQ_FRAMEBUFFER_SET_GPIOVIRTBUF:
    case RPI_FWREQ_FRAMEBUFFER_SET_BACKLIGHT:
    case RPI_FWREQ_FRAMEBUFFER_SET_DISPLAY_NUM:
    case RPI_FWREQ_FRAMEBUFFER_GET_NUM_DISPLAYS:
    case RPI_FWREQ_FRAMEBUFFER_GET_DISPLAY_SETTINGS:
    case RPI_FWREQ_FRAMEBUFFER_GET_DISPLAY_ID:
    case RPI_FWREQ_SET_PLANE:
    case RPI_FWREQ_GET_DISPLAY_TIMING:
    case RPI_FWREQ_SET_TIMING:
    case RPI_FWREQ_GET_DISPLAY_CFG:
    case RPI_FWREQ_SET_DISPLAY_POWER:
        return true;
    default:
        return false;
    }
}

static bool bcm2835_property_framebuffer_depth_supported(uint32_t depth)
{
    return depth == 8 || depth == 16 || depth == 24 || depth == 32;
}

/* https://github.com/raspberrypi/firmware/wiki/Mailbox-property-interface */

static bool bcm2835_property_request_is_valid(
    BCM2835PropertyState *s, uint64_t tag_address, uint32_t tag,
    uint32_t buffer_size)
{
    uint64_t minimum = 0;

    switch (tag) {
    case RPI_FWREQ_GET_POWER_STATE:
    case RPI_FWREQ_GET_TIMING:
    case RPI_FWREQ_GET_CLOCK_STATE:
    case RPI_FWREQ_GET_CLOCK_RATE:
    case RPI_FWREQ_GET_CLOCK_MEASURED:
    case RPI_FWREQ_GET_MAX_CLOCK_RATE:
    case RPI_FWREQ_GET_MIN_CLOCK_RATE:
    case RPI_FWREQ_GET_TEMPERATURE:
    case RPI_FWREQ_GET_MAX_TEMPERATURE:
    case RPI_FWREQ_GET_GPIO_STATE:
    case RPI_FWREQ_GET_GPIO_CONFIG:
    case RPI_FWREQ_FRAMEBUFFER_ALLOCATE:
    case RPI_FWREQ_FRAMEBUFFER_BLANK:
    case RPI_FWREQ_GET_EDID_BLOCK:
    case RPI_FWREQ_FRAMEBUFFER_TEST_DEPTH:
    case RPI_FWREQ_FRAMEBUFFER_SET_DEPTH:
    case RPI_FWREQ_FRAMEBUFFER_TEST_PIXEL_ORDER:
    case RPI_FWREQ_FRAMEBUFFER_SET_PIXEL_ORDER:
    case RPI_FWREQ_FRAMEBUFFER_TEST_ALPHA_MODE:
    case RPI_FWREQ_FRAMEBUFFER_SET_ALPHA_MODE:
    case RPI_FWREQ_FRAMEBUFFER_TEST_LAYER:
    case RPI_FWREQ_FRAMEBUFFER_SET_LAYER:
    case RPI_FWREQ_FRAMEBUFFER_TEST_TRANSFORM:
    case RPI_FWREQ_FRAMEBUFFER_SET_TRANSFORM:
    case RPI_FWREQ_FRAMEBUFFER_TEST_VSYNC:
    case RPI_FWREQ_FRAMEBUFFER_SET_VSYNC:
    case RPI_FWREQ_FRAMEBUFFER_SET_DISPLAY_NUM:
    case RPI_FWREQ_FRAMEBUFFER_GET_DISPLAY_ID:
    case RPI_FWREQ_SET_REBOOT_FLAGS:
    case RPI_FWREQ_NOTIFY_XHCI_RESET:
    case RPI_FWREQ_VCHIQ_INIT:
        minimum = 4;
        break;
    case RPI_FWREQ_SET_POWER_STATE:
    case RPI_FWREQ_SET_CLOCK_STATE:
    case RPI_FWREQ_SET_MAX_CLOCK_RATE:
    case RPI_FWREQ_SET_MIN_CLOCK_RATE:
    case RPI_FWREQ_SET_GPIO_STATE:
    case RPI_FWREQ_FRAMEBUFFER_TEST_PHYSICAL_WIDTH_HEIGHT:
    case RPI_FWREQ_FRAMEBUFFER_TEST_VIRTUAL_WIDTH_HEIGHT:
    case RPI_FWREQ_FRAMEBUFFER_SET_PHYSICAL_WIDTH_HEIGHT:
    case RPI_FWREQ_FRAMEBUFFER_SET_VIRTUAL_WIDTH_HEIGHT:
    case RPI_FWREQ_FRAMEBUFFER_TEST_VIRTUAL_OFFSET:
    case RPI_FWREQ_FRAMEBUFFER_SET_VIRTUAL_OFFSET:
    case RPI_FWREQ_GET_CUSTOMER_OTP:
    case RPI_FWREQ_GET_PRIVATE_KEY:
    case RPI_FWREQ_GET_EDID_BLOCK_DISPLAY:
    case RPI_FWREQ_SET_DISPLAY_POWER:
        minimum = 8;
        break;
    case RPI_FWREQ_SET_CLOCK_RATE:
        minimum = 12;
        break;
    case RPI_FWREQ_SET_CURSOR_STATE:
        minimum = 16;
        break;
    case RPI_FWREQ_SET_CURSOR_INFO:
        minimum = 24;
        break;
    case RPI_FWREQ_GET_DISPLAY_TIMING:
    case RPI_FWREQ_SET_TIMING:
        minimum = BCM2835_PROPERTY_DISPLAY_TIMING_SIZE;
        break;
    case RPI_FWREQ_FRAMEBUFFER_GET_OVERSCAN:
        break;
    case RPI_FWREQ_FRAMEBUFFER_TEST_OVERSCAN:
    case RPI_FWREQ_FRAMEBUFFER_SET_OVERSCAN:
        minimum = 16;
        break;
    case RPI_FWREQ_SET_GPIO_CONFIG:
        minimum = 24;
        break;
    case RPI_FWREQ_FRAMEBUFFER_TEST_PALETTE:
    case RPI_FWREQ_FRAMEBUFFER_SET_PALETTE:
        if (buffer_size < 8) {
            return false;
        }
        minimum = 8;
        {
            uint32_t offset = ldl_le_phys(&s->dma_as, tag_address + 12);
            uint32_t length = ldl_le_phys(&s->dma_as, tag_address + 16);

            if (offset <= 255 && length >= 1 && length <= 256 - offset) {
                minimum += (uint64_t)length * sizeof(uint32_t);
            }
        }
        break;
    case RPI_FWREQ_SET_CUSTOMER_OTP:
    case RPI_FWREQ_SET_PRIVATE_KEY:
        if (buffer_size < 8) {
            return false;
        }
        minimum = 8;
        {
            uint32_t start = ldl_le_phys(&s->dma_as, tag_address + 12);
            uint32_t number = ldl_le_phys(&s->dma_as, tag_address + 16);

            if (tag != RPI_FWREQ_SET_CUSTOMER_OTP ||
                start != BCM2835_OTP_LOCK_NUM1 ||
                number != BCM2835_OTP_LOCK_NUM2) {
                minimum += (uint64_t)number * sizeof(uint32_t);
            }
        }
        break;
    default:
        break;
    }
    return buffer_size >= minimum;
}

static bool bcm2835_property_buffer_is_well_formed(
    BCM2835PropertyState *s, uint32_t address, uint32_t total_length)
{
    uint64_t end = (uint64_t)address + total_length;
    uint64_t cursor = (uint64_t)address + 8;

    if (total_length < 12 || total_length % sizeof(uint32_t) ||
        end > (1ULL << 32)) {
        return false;
    }
    while (cursor + sizeof(uint32_t) <= end) {
        uint32_t tag = ldl_le_phys(&s->dma_as, cursor);
        uint64_t next;
        uint32_t buffer_size;
        uint32_t request_code;

        if (tag == RPI_FWREQ_PROPERTY_END) {
            return true;
        }
        if (cursor + 12 > end) {
            return false;
        }
        buffer_size = ldl_le_phys(&s->dma_as, cursor + 4);
        request_code = ldl_le_phys(&s->dma_as, cursor + 8);
        if (request_code != 0) {
            return false;
        }
        if (!bcm2835_property_request_is_valid(
                s, cursor, tag, buffer_size)) {
            return false;
        }
        next = cursor + 12 + QEMU_ALIGN_UP((uint64_t)buffer_size, 4);
        if (next < cursor || next > end) {
            return false;
        }
        cursor = next;
    }
    return false;
}

static void bcm2835_property_payload_write(BCM2835PropertyState *s,
                                            uint32_t tag_address,
                                            uint32_t buffer_size,
                                            uint32_t offset,
                                            const void *data,
                                            size_t length)
{
    if (offset >= buffer_size) {
        return;
    }
    length = MIN(length, buffer_size - offset);
    dma_memory_write(&s->dma_as, tag_address + 12 + offset, data, length,
                     MEMTXATTRS_UNSPECIFIED);
}

static void bcm2835_property_write_edid_block(
    BCM2835PropertyState *s, uint32_t tag_address, uint32_t buffer_size,
    unsigned int port, uint32_t block, uint32_t payload_offset)
{
    static const uint8_t empty[BCM2835_PROPERTY_EDID_BLOCK_SIZE];
    const uint8_t *data = empty;
    uint64_t offset = (uint64_t)block * BCM2835_PROPERTY_EDID_BLOCK_SIZE;

    if (port < BCM2835_PROPERTY_EDID_PORT_COUNT &&
        offset + BCM2835_PROPERTY_EDID_BLOCK_SIZE <= s->edid_size[port]) {
        data = s->edid[port] + offset;
    }
    bcm2835_property_payload_write(
        s, tag_address, buffer_size, payload_offset, data, sizeof(empty));
}

#define DISPLAY_TIMING_CLOCK          4
#define DISPLAY_TIMING_HDISPLAY       8
#define DISPLAY_TIMING_HSYNC_START   10
#define DISPLAY_TIMING_HSYNC_END     12
#define DISPLAY_TIMING_HTOTAL        14
#define DISPLAY_TIMING_HSKEW         16
#define DISPLAY_TIMING_VDISPLAY      18
#define DISPLAY_TIMING_VSYNC_START   20
#define DISPLAY_TIMING_VSYNC_END     22
#define DISPLAY_TIMING_VTOTAL        24
#define DISPLAY_TIMING_VSCAN         26
#define DISPLAY_TIMING_VREFRESH      28
#define DISPLAY_TIMING_FLAGS         32

#define DISPLAY_TIMING_FLAG_HSYNC_POS BIT(0)
#define DISPLAY_TIMING_FLAG_VSYNC_POS BIT(1)
#define DISPLAY_TIMING_FLAG_INTERLACE BIT(2)
#define DISPLAY_TIMING_FLAG_ASPECT_SHIFT 4
#define DISPLAY_TIMING_FLAG_ASPECT_MASK  (0xf << 4)
#define DISPLAY_TIMING_FLAG_RGB_LIMITED BIT(8)
#define DISPLAY_TIMING_FLAG_DVI         BIT(9)
#define DISPLAY_TIMING_FLAG_DBL_CLK     BIT(10)
#define DISPLAY_TIMING_FLAG_VALID_MASK \
    (DISPLAY_TIMING_FLAG_HSYNC_POS | DISPLAY_TIMING_FLAG_VSYNC_POS | \
     DISPLAY_TIMING_FLAG_INTERLACE | DISPLAY_TIMING_FLAG_ASPECT_MASK | \
     DISPLAY_TIMING_FLAG_RGB_LIMITED | DISPLAY_TIMING_FLAG_DVI | \
     DISPLAY_TIMING_FLAG_DBL_CLK)

static bool bcm2835_property_edid_is_hdmi(
    const BCM2835PropertyState *s, unsigned int port)
{
    for (size_t offset = BCM2835_PROPERTY_EDID_BLOCK_SIZE;
         offset + BCM2835_PROPERTY_EDID_BLOCK_SIZE <= s->edid_size[port];
         offset += BCM2835_PROPERTY_EDID_BLOCK_SIZE) {
        const uint8_t *extension = s->edid[port] + offset;
        unsigned int end;

        if (extension[0] != 0x02 || extension[1] < 0x03) {
            continue;
        }
        end = extension[2] ? extension[2] : 127;
        if (end < 4 || end > 127) {
            continue;
        }
        for (unsigned int block = 4; block < end; ) {
            unsigned int length = extension[block] & 0x1f;
            unsigned int tag = extension[block] >> 5;

            if (block + 1 + length > end) {
                break;
            }
            if (tag == 3 && length >= 3 &&
                ((!memcmp(extension + block + 1,
                          (uint8_t[]) { 0x03, 0x0c, 0x00 }, 3)) ||
                 (!memcmp(extension + block + 1,
                          (uint8_t[]) { 0xd8, 0x5d, 0xc4 }, 3)))) {
                return true;
            }
            block += 1 + length;
        }
    }
    return false;
}

static uint32_t bcm2835_property_timing_aspect(uint32_t width,
                                               uint32_t height)
{
    if ((uint64_t)width * 3 == (uint64_t)height * 4) {
        return 1;
    }
    if ((uint64_t)width * 9 == (uint64_t)height * 16) {
        return 2;
    }
    if ((uint64_t)width * 27 == (uint64_t)height * 64) {
        return 3;
    }
    if ((uint64_t)width * 135 == (uint64_t)height * 256) {
        return 4;
    }
    return 0;
}

static void bcm2835_property_derive_display_timing(
    BCM2835PropertyState *s, unsigned int port)
{
    uint8_t *timing = s->display_timing[port];
    const uint8_t *desc = NULL;
    uint32_t clock;
    uint32_t hactive;
    uint32_t hblank;
    uint32_t hfront;
    uint32_t hsync;
    uint32_t vactive;
    uint32_t vblank;
    uint32_t vfront;
    uint32_t vsync;
    uint32_t htotal;
    uint32_t vtotal;
    uint32_t flags = 0;
    uint64_t refresh_numerator;
    uint64_t refresh_denominator;

    memset(timing, 0, BCM2835_PROPERTY_DISPLAY_TIMING_SIZE);
    timing[0] = bcm2835_property_display_id_from_index(port);
    if (s->edid_size[port] < BCM2835_PROPERTY_EDID_BLOCK_SIZE) {
        return;
    }
    for (unsigned int offset = 54; offset <= 108; offset += 18) {
        if (lduw_le_p(s->edid[port] + offset)) {
            desc = s->edid[port] + offset;
            break;
        }
    }
    if (!desc) {
        return;
    }

    clock = lduw_le_p(desc) * 10;
    hactive = desc[2] | ((desc[4] & 0xf0) << 4);
    hblank = desc[3] | ((desc[4] & 0x0f) << 8);
    vactive = desc[5] | ((desc[7] & 0xf0) << 4);
    vblank = desc[6] | ((desc[7] & 0x0f) << 8);
    hfront = desc[8] | ((desc[11] & 0xc0) << 2);
    hsync = desc[9] | ((desc[11] & 0x30) << 4);
    vfront = (desc[10] >> 4) | ((desc[11] & 0x0c) << 2);
    vsync = (desc[10] & 0x0f) | ((desc[11] & 0x03) << 4);
    htotal = hactive + hblank;
    vtotal = vactive + vblank;
    if (!clock || !hactive || !vactive || !hfront || !hsync ||
        !vfront || !vsync || hactive + hfront + hsync > htotal ||
        vactive + vfront + vsync > vtotal) {
        memset(timing + 1, 0,
               BCM2835_PROPERTY_DISPLAY_TIMING_SIZE - 1);
        return;
    }

    if ((desc[17] & 0x18) == 0x18) {
        if (desc[17] & BIT(1)) {
            flags |= DISPLAY_TIMING_FLAG_HSYNC_POS;
        }
        if (desc[17] & BIT(2)) {
            flags |= DISPLAY_TIMING_FLAG_VSYNC_POS;
        }
    }
    if (desc[17] & BIT(7)) {
        flags |= DISPLAY_TIMING_FLAG_INTERLACE;
    }
    flags |= bcm2835_property_timing_aspect(hactive, vactive) <<
             DISPLAY_TIMING_FLAG_ASPECT_SHIFT;
    if (!bcm2835_property_edid_is_hdmi(s, port)) {
        flags |= DISPLAY_TIMING_FLAG_DVI;
    }

    stl_le_p(timing + DISPLAY_TIMING_CLOCK, clock);
    stw_le_p(timing + DISPLAY_TIMING_HDISPLAY, hactive);
    stw_le_p(timing + DISPLAY_TIMING_HSYNC_START, hactive + hfront);
    stw_le_p(timing + DISPLAY_TIMING_HSYNC_END,
             hactive + hfront + hsync);
    stw_le_p(timing + DISPLAY_TIMING_HTOTAL, htotal);
    stw_le_p(timing + DISPLAY_TIMING_VDISPLAY, vactive);
    stw_le_p(timing + DISPLAY_TIMING_VSYNC_START, vactive + vfront);
    stw_le_p(timing + DISPLAY_TIMING_VSYNC_END,
             vactive + vfront + vsync);
    stw_le_p(timing + DISPLAY_TIMING_VTOTAL, vtotal);
    refresh_numerator = (uint64_t)clock * 1000;
    if (flags & DISPLAY_TIMING_FLAG_INTERLACE) {
        refresh_numerator *= 2;
    }
    refresh_denominator = (uint64_t)htotal * vtotal;
    stw_le_p(timing + DISPLAY_TIMING_VREFRESH,
             (refresh_numerator + refresh_denominator / 2) /
             refresh_denominator);
    stl_le_p(timing + DISPLAY_TIMING_FLAGS, flags);
}

static bool bcm2835_property_display_timing_is_valid(
    const uint8_t timing[BCM2835_PROPERTY_DISPLAY_TIMING_SIZE],
    unsigned int port)
{
    uint32_t flags = ldl_le_p(timing + DISPLAY_TIMING_FLAGS);
    uint32_t aspect =
        (flags & DISPLAY_TIMING_FLAG_ASPECT_MASK) >>
        DISPLAY_TIMING_FLAG_ASPECT_SHIFT;
    uint16_t hdisplay = lduw_le_p(timing + DISPLAY_TIMING_HDISPLAY);
    uint16_t hsync_start = lduw_le_p(
        timing + DISPLAY_TIMING_HSYNC_START);
    uint16_t hsync_end = lduw_le_p(timing + DISPLAY_TIMING_HSYNC_END);
    uint16_t htotal = lduw_le_p(timing + DISPLAY_TIMING_HTOTAL);
    uint16_t vdisplay = lduw_le_p(timing + DISPLAY_TIMING_VDISPLAY);
    uint16_t vsync_start = lduw_le_p(
        timing + DISPLAY_TIMING_VSYNC_START);
    uint16_t vsync_end = lduw_le_p(timing + DISPLAY_TIMING_VSYNC_END);
    uint16_t vtotal = lduw_le_p(timing + DISPLAY_TIMING_VTOTAL);

    return timing[0] == bcm2835_property_display_id_from_index(port) &&
           timing[1] == 0 && !lduw_le_p(timing + 30) &&
           ldl_le_p(timing + DISPLAY_TIMING_CLOCK) > 0 &&
           ldl_le_p(timing + DISPLAY_TIMING_CLOCK) <= 600000 &&
           hdisplay && hdisplay < hsync_start &&
           hsync_start < hsync_end && hsync_end <= htotal &&
           vdisplay && vdisplay < vsync_start &&
           vsync_start < vsync_end && vsync_end <= vtotal &&
           lduw_le_p(timing + DISPLAY_TIMING_VREFRESH) > 0 &&
           !(flags & ~DISPLAY_TIMING_FLAG_VALID_MASK) &&
           aspect <= 4;
}

static bool bcm2835_property_display_timing_state_is_valid(
    const uint8_t timing[BCM2835_PROPERTY_DISPLAY_TIMING_SIZE],
    unsigned int port)
{
    if (timing[0] != bcm2835_property_display_id_from_index(port)) {
        return false;
    }
    if (!ldl_le_p(timing + DISPLAY_TIMING_CLOCK)) {
        for (unsigned int offset = 1;
             offset < BCM2835_PROPERTY_DISPLAY_TIMING_SIZE; offset++) {
            if (timing[offset]) {
                return false;
            }
        }
        return true;
    }
    return bcm2835_property_display_timing_is_valid(timing, port);
}

static int64_t bcm2835_property_vblank_period_ns(
    const BCM2835PropertyState *s)
{
    uint32_t port = s->framebuffer_display_num;
    uint16_t refresh;

    if (port >= BCM2835_PROPERTY_DISPLAY_COUNT) {
        port = 0;
    }
    refresh = lduw_le_p(s->display_timing[port] + DISPLAY_TIMING_VREFRESH);
    if (!refresh) {
        refresh = DEFAULT_DISPLAY_REFRESH_HZ;
    }
    return DIV_ROUND_UP(NANOSECONDS_PER_SECOND, refresh);
}

static void bcm2835_property_vsync_complete(void *opaque)
{
    BCM2835PropertyState *s = opaque;

    if (!s->vsync_wait) {
        return;
    }
    s->vsync_wait = false;
    qemu_set_irq(s->mbox_irq, 1);
}

static void bcm2835_property_wait_for_vsync(BCM2835PropertyState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t period = bcm2835_property_vblank_period_ns(s);
    int64_t deadline = DIV_ROUND_UP(now + 1, period) * period;

    s->vsync_wait = true;
    timer_mod_ns(s->vsync_timer, deadline);
}

static void bcm2835_property_apply_display_timing(
    BCM2835PropertyState *s,
    const uint8_t candidate[BCM2835_PROPERTY_DISPLAY_TIMING_SIZE])
{
    int port = bcm2835_property_display_index_from_id(candidate[0]);

    if (port >= 0 &&
        bcm2835_property_display_timing_is_valid(candidate, port)) {
        memcpy(s->display_timing[port], candidate,
               BCM2835_PROPERTY_DISPLAY_TIMING_SIZE);
    }
}

static void bcm2835_property_stl(BCM2835PropertyState *s,
                                  uint32_t tag_address,
                                  uint32_t buffer_size,
                                  uint32_t offset,
                                  uint32_t value)
{
    uint8_t bytes[sizeof(value)];

    stl_le_p(bytes, value);
    bcm2835_property_payload_write(s, tag_address, buffer_size, offset,
                                   bytes, sizeof(bytes));
}

static bool bcm2835_property_preprocess_framebuffer(
    BCM2835PropertyState *s, uint32_t address, uint32_t total_length,
    BCM2835FBConfig *config)
{
    uint64_t end = (uint64_t)address + total_length;
    uint64_t cursor = (uint64_t)address + 8;
    g_autoptr(GHashTable) seen = g_hash_table_new(g_direct_hash,
                                                  g_direct_equal);
    bool has_test = false;
    bool has_get_or_set = false;

    while (cursor + sizeof(uint32_t) <= end) {
        uint32_t tag = ldl_le_phys(&s->dma_as, cursor);
        uint32_t buffer_size;

        if (tag == RPI_FWREQ_PROPERTY_END) {
            break;
        }
        buffer_size = ldl_le_phys(&s->dma_as, cursor + 4);
        if (bcm2835_property_is_framebuffer_transaction(tag)) {
            if (!g_hash_table_add(seen, GUINT_TO_POINTER(tag))) {
                return false;
            }
            if (bcm2835_property_is_framebuffer_test(tag)) {
                has_test = true;
            } else {
                has_get_or_set = true;
            }
        }
        cursor += 12 + QEMU_ALIGN_UP((uint64_t)buffer_size, 4);
    }

    if (has_test && has_get_or_set) {
        return false;
    }

    cursor = (uint64_t)address + 8;
    while (cursor + sizeof(uint32_t) <= end) {
        uint32_t tag = ldl_le_phys(&s->dma_as, cursor);
        uint32_t buffer_size;

        if (tag == RPI_FWREQ_PROPERTY_END) {
            break;
        }
        buffer_size = ldl_le_phys(&s->dma_as, cursor + 4);
        switch (tag) {
        case RPI_FWREQ_FRAMEBUFFER_ALLOCATE:
            bcm2835_fb_set_enabled(s->fbdev, true);
            break;
        case RPI_FWREQ_FRAMEBUFFER_RELEASE:
            bcm2835_fb_set_enabled(s->fbdev, false);
            break;
        case RPI_FWREQ_FRAMEBUFFER_BLANK:
            bcm2835_fb_set_blank(
                s->fbdev,
                ldl_le_phys(&s->dma_as, cursor + 12) != 0);
            break;
        case RPI_FWREQ_FRAMEBUFFER_SET_DISPLAY_NUM:
        {
            uint32_t display = ldl_le_phys(&s->dma_as, cursor + 12);

            if (display < BCM2835_PROPERTY_DISPLAY_COUNT) {
                s->framebuffer_display_num = display;
            }
            stl_le_phys(&s->dma_as, cursor + 12,
                        s->framebuffer_display_num);
            break;
        }
        case RPI_FWREQ_SET_CURSOR_INFO:
        {
            uint32_t width = ldl_le_phys(&s->dma_as, cursor + 12);
            uint32_t height = ldl_le_phys(&s->dma_as, cursor + 16);
            uint32_t cursor_address =
                ldl_le_phys(&s->dma_as, cursor + 24);
            uint32_t hotspot_x = ldl_le_phys(&s->dma_as, cursor + 28);
            uint32_t hotspot_y = ldl_le_phys(&s->dma_as, cursor + 32);
            uint32_t result;

            result = !bcm2835_fb_set_cursor_info(
                s->fbdev, width, height, cursor_address,
                hotspot_x, hotspot_y);
            stl_le_phys(&s->dma_as, cursor + 12, result);
            break;
        }
        case RPI_FWREQ_SET_CURSOR_STATE:
        {
            uint32_t enable = ldl_le_phys(&s->dma_as, cursor + 12);
            uint32_t x = ldl_le_phys(&s->dma_as, cursor + 16);
            uint32_t y = ldl_le_phys(&s->dma_as, cursor + 20);
            uint32_t flags = ldl_le_phys(&s->dma_as, cursor + 24);
            uint32_t result;

            result = !bcm2835_fb_set_cursor_state(
                s->fbdev, enable, x, y, flags);
            stl_le_phys(&s->dma_as, cursor + 12, result);
            break;
        }
        case RPI_FWREQ_SET_TIMING:
        {
            uint8_t candidate[BCM2835_PROPERTY_DISPLAY_TIMING_SIZE];
            int port;

            dma_memory_read(&s->dma_as, cursor + 12, candidate,
                            sizeof(candidate), MEMTXATTRS_UNSPECIFIED);
            port = bcm2835_property_display_index_from_id(candidate[0]);
            bcm2835_property_apply_display_timing(s, candidate);
            if (port >= 0) {
                dma_memory_write(&s->dma_as, cursor + 12,
                                 s->display_timing[port],
                                 sizeof(candidate), MEMTXATTRS_UNSPECIFIED);
            }
            break;
        }
        case RPI_FWREQ_FRAMEBUFFER_TEST_PHYSICAL_WIDTH_HEIGHT:
        case RPI_FWREQ_FRAMEBUFFER_SET_PHYSICAL_WIDTH_HEIGHT:
            config->xres = ldl_le_phys(&s->dma_as, cursor + 12);
            config->yres = ldl_le_phys(&s->dma_as, cursor + 16);
            bcm2835_fb_validate_config(config);
            stl_le_phys(&s->dma_as, cursor + 12, config->xres);
            stl_le_phys(&s->dma_as, cursor + 16, config->yres);
            break;
        case RPI_FWREQ_FRAMEBUFFER_TEST_VIRTUAL_WIDTH_HEIGHT:
        case RPI_FWREQ_FRAMEBUFFER_SET_VIRTUAL_WIDTH_HEIGHT:
            config->xres_virtual =
                ldl_le_phys(&s->dma_as, cursor + 12);
            config->yres_virtual =
                ldl_le_phys(&s->dma_as, cursor + 16);
            bcm2835_fb_validate_config(config);
            stl_le_phys(&s->dma_as, cursor + 12, config->xres_virtual);
            stl_le_phys(&s->dma_as, cursor + 16, config->yres_virtual);
            break;
        case RPI_FWREQ_FRAMEBUFFER_TEST_DEPTH:
        case RPI_FWREQ_FRAMEBUFFER_SET_DEPTH:
        {
            uint32_t depth = ldl_le_phys(&s->dma_as, cursor + 12);

            if (bcm2835_property_framebuffer_depth_supported(depth)) {
                config->bpp = depth;
            }
            stl_le_phys(&s->dma_as, cursor + 12, config->bpp);
            break;
        }
        case RPI_FWREQ_FRAMEBUFFER_TEST_PIXEL_ORDER:
        case RPI_FWREQ_FRAMEBUFFER_SET_PIXEL_ORDER:
        {
            uint32_t pixel_order = ldl_le_phys(&s->dma_as, cursor + 12);

            if (pixel_order <= 1) {
                config->pixo = pixel_order;
            }
            stl_le_phys(&s->dma_as, cursor + 12, config->pixo);
            break;
        }
        case RPI_FWREQ_FRAMEBUFFER_TEST_ALPHA_MODE:
        case RPI_FWREQ_FRAMEBUFFER_SET_ALPHA_MODE:
        {
            uint32_t alpha = ldl_le_phys(&s->dma_as, cursor + 12);

            if (alpha <= 2) {
                config->alpha = alpha;
            }
            stl_le_phys(&s->dma_as, cursor + 12, config->alpha);
            break;
        }
        case RPI_FWREQ_FRAMEBUFFER_TEST_VIRTUAL_OFFSET:
        case RPI_FWREQ_FRAMEBUFFER_SET_VIRTUAL_OFFSET:
            config->xoffset = ldl_le_phys(&s->dma_as, cursor + 12);
            config->yoffset = ldl_le_phys(&s->dma_as, cursor + 16);
            bcm2835_fb_validate_config(config);
            stl_le_phys(&s->dma_as, cursor + 12, config->xoffset);
            stl_le_phys(&s->dma_as, cursor + 16, config->yoffset);
            break;
        case RPI_FWREQ_FRAMEBUFFER_TEST_OVERSCAN:
        case RPI_FWREQ_FRAMEBUFFER_SET_OVERSCAN:
            bcm2835_fb_set_overscan(
                config,
                ldl_le_phys(&s->dma_as, cursor + 12),
                ldl_le_phys(&s->dma_as, cursor + 16),
                ldl_le_phys(&s->dma_as, cursor + 20),
                ldl_le_phys(&s->dma_as, cursor + 24));
            stl_le_phys(&s->dma_as, cursor + 12,
                        config->overscan_top);
            stl_le_phys(&s->dma_as, cursor + 16,
                        config->overscan_bottom);
            stl_le_phys(&s->dma_as, cursor + 20,
                        config->overscan_left);
            stl_le_phys(&s->dma_as, cursor + 24,
                        config->overscan_right);
            break;
        case RPI_FWREQ_FRAMEBUFFER_TEST_LAYER:
        case RPI_FWREQ_FRAMEBUFFER_SET_LAYER:
            config->layer = ldl_le_phys(&s->dma_as, cursor + 12);
            break;
        case RPI_FWREQ_FRAMEBUFFER_TEST_TRANSFORM:
        case RPI_FWREQ_FRAMEBUFFER_SET_TRANSFORM:
        {
            uint32_t transform = ldl_le_phys(&s->dma_as, cursor + 12);

            if (transform <= 7) {
                config->transform = transform;
            }
            stl_le_phys(&s->dma_as, cursor + 12, config->transform);
            break;
        }
        case RPI_FWREQ_FRAMEBUFFER_TEST_VSYNC:
        case RPI_FWREQ_FRAMEBUFFER_SET_VSYNC:
            /*
             * SET_VSYNC is a command carrying a dummy u32, not retained
             * framebuffer configuration.  Completion is deferred below.
             */
            stl_le_phys(&s->dma_as, cursor + 12, 0);
            break;
        case RPI_FWREQ_FRAMEBUFFER_SET_PALETTE:
        {
            uint32_t offset = ldl_le_phys(&s->dma_as, cursor + 12);
            uint32_t length = ldl_le_phys(&s->dma_as, cursor + 16);

            if (offset <= 255 && length >= 1 &&
                length <= 256 - offset) {
                for (uint32_t entry = 0; entry < length; entry++) {
                    uint32_t color = ldl_le_phys(
                        &s->dma_as, cursor + 20 + entry * sizeof(uint32_t));

                    stl_le_phys(
                        &s->dma_as,
                        s->fbdev->vcram_base +
                        (offset + entry) * sizeof(uint32_t),
                        color);
                }
            }
            break;
        }
        default:
            break;
        }
        cursor += 12 + QEMU_ALIGN_UP((uint64_t)buffer_size, 4);
    }
    return true;
}

static void bcm2835_property_mbox_push(BCM2835PropertyState *s, uint32_t value)
{
    uint32_t tot_len;

    /*
     * Copy the current state of the framebuffer config; we will update
     * this copy as we process tags and then ask the framebuffer to use
     * it at the end.
     */
    BCM2835FBConfig fbconfig = s->fbdev->config;
    bool fbconfig_updated = false;
    bool wait_for_vsync = false;

    value &= ~0xf;

    s->addr = value;

    tot_len = ldl_le_phys(&s->dma_as, value);
    if (!bcm2835_property_buffer_is_well_formed(s, s->addr, tot_len)) {
        stl_le_phys(&s->dma_as, s->addr + 4, 0x80000001);
        return;
    }
    if (!bcm2835_property_preprocess_framebuffer(
            s, s->addr, tot_len, &fbconfig)) {
        stl_le_phys(&s->dma_as, s->addr + 4, 0x80000001);
        return;
    }

    /* @(addr + 4) : Buffer response code */
    value = s->addr + 8;
    while (value + 8 <= s->addr + tot_len) {
        uint32_t tag = ldl_le_phys(&s->dma_as, value);
        uint32_t bufsize = ldl_le_phys(&s->dma_as, value + 4);
        /* @(value + 8) : Request/response indicator */
        size_t resplen = 0;
        bool handled = true;

        switch (tag) {
        case RPI_FWREQ_PROPERTY_END:
            break;
        case RPI_FWREQ_GET_FIRMWARE_REVISION:
            bcm2835_property_stl(s, value, bufsize, 0, 346337);
            resplen = 4;
            break;
        case RPI_FWREQ_GET_BOARD_MODEL:
            /*
             * Production firmware retains the legacy board-model response
             * value of zero; the board revision carries the actual identity.
             */
            bcm2835_property_stl(s, value, bufsize, 0, 0);
            resplen = 4;
            break;
        case RPI_FWREQ_GET_BOARD_REVISION:
            bcm2835_property_stl(s, value, bufsize, 0, s->board_rev);
            resplen = 4;
            break;
        case RPI_FWREQ_GET_BOARD_MAC_ADDRESS:
            resplen = sizeof(s->macaddr.a);
            bcm2835_property_payload_write(s, value, bufsize, 0,
                                           s->macaddr.a, resplen);
            break;
        case RPI_FWREQ_GET_BOARD_SERIAL:
            /*
             * The BCM2711 model's persistent serial identity is the 32-bit
             * OTP serial row; the property-mailbox ABI returns it as a u64.
             */
            bcm2835_property_stl(
                s, value, bufsize, 0,
                bcm2835_otp_get_row(s->otp, BCM2711_OTP_SERIAL_ROW));
            bcm2835_property_stl(s, value, bufsize, 4, 0);
            resplen = 8;
            break;
        case RPI_FWREQ_GET_ARM_MEMORY:
            /* base */
            bcm2835_property_stl(s, value, bufsize, 0, 0);
            /* size */
            bcm2835_property_stl(s, value, bufsize, 4, s->fbdev->vcram_base);
            resplen = 8;
            break;
        case RPI_FWREQ_GET_VC_MEMORY:
            /* base */
            bcm2835_property_stl(s, value, bufsize, 0, s->fbdev->vcram_base);
            /* size */
            bcm2835_property_stl(s, value, bufsize, 4, s->fbdev->vcram_size);
            resplen = 8;
            break;
        case RPI_FWREQ_GET_POWER_STATE:
        {
            uint32_t device_id = ldl_le_phys(&s->dma_as, value + 12);
            uint32_t state;

            if (bcm2835_property_power_device_is_valid(device_id)) {
                state = !!(s->power_state & BIT(device_id));
            } else {
                state = BIT(1);
            }
            bcm2835_property_stl(s, value, bufsize, 0, device_id);
            bcm2835_property_stl(s, value, bufsize, 4, state);
            resplen = 8;
            break;
        }
        case RPI_FWREQ_GET_TIMING:
        {
            uint32_t device_id = ldl_le_phys(&s->dma_as, value + 12);

            bcm2835_property_stl(s, value, bufsize, 0, device_id);
            bcm2835_property_stl(s, value, bufsize, 4, 0);
            resplen = 8;
            break;
        }
        case RPI_FWREQ_SET_POWER_STATE:
        {
            uint32_t device_id = ldl_le_phys(&s->dma_as, value + 12);
            uint32_t requested = ldl_le_phys(&s->dma_as, value + 16);
            uint32_t state;

            if (bcm2835_property_power_device_is_valid(device_id)) {
                if (requested & BIT(0)) {
                    s->power_state |= BIT(device_id);
                } else {
                    s->power_state &= ~BIT(device_id);
                }
                state = !!(s->power_state & BIT(device_id));
            } else {
                state = BIT(1);
            }
            bcm2835_property_stl(s, value, bufsize, 0, device_id);
            bcm2835_property_stl(s, value, bufsize, 4, state);
            resplen = 8;
            break;
        }

        /* Clocks */

        case RPI_FWREQ_GET_CLOCK_STATE:
        {
            CprmanClockMux mux;
            uint32_t clock_id = ldl_le_phys(&s->dma_as, value + 12);
            uint32_t state = 2;

            if (s->cprman && bcm2835_property_clock_mux(clock_id, &mux)) {
                state = bcm2835_cprman_clock_is_enabled(s->cprman, mux);
            }
            bcm2835_property_stl(s, value, bufsize, 4, state);
            resplen = 8;
            break;
        }

        case RPI_FWREQ_SET_CLOCK_STATE:
        {
            CprmanClockMux mux;
            uint32_t clock_id = ldl_le_phys(&s->dma_as, value + 12);
            uint32_t requested = ldl_le_phys(&s->dma_as, value + 16);
            uint32_t state = 2;

            if (s->cprman && bcm2835_property_clock_mux(clock_id, &mux)) {
                bcm2835_cprman_clock_set_enabled(s->cprman, mux,
                                                  requested & 1);
                state = bcm2835_cprman_clock_is_enabled(s->cprman, mux);
            }
            bcm2835_property_stl(s, value, bufsize, 4, state);
            resplen = 8;
            break;
        }

        case RPI_FWREQ_GET_CLOCK_RATE:
        case RPI_FWREQ_GET_CLOCK_MEASURED:
        {
            CprmanClockMux mux;
            uint32_t clock_id = ldl_le_phys(&s->dma_as, value + 12);
            uint64_t rate = 0;

            if (s->cprman && bcm2835_property_clock_mux(clock_id, &mux)) {
                rate = tag == RPI_FWREQ_GET_CLOCK_RATE ?
                    bcm2835_cprman_clock_get_rate(s->cprman, mux) :
                    bcm2835_cprman_clock_get_measured_rate(s->cprman, mux);
            }
            bcm2835_property_stl(s, value, bufsize, 4, MIN(rate, UINT32_MAX));
            resplen = 8;
            break;
        }

        case RPI_FWREQ_GET_MAX_CLOCK_RATE:
        case RPI_FWREQ_GET_MIN_CLOCK_RATE:
            switch (ldl_le_phys(&s->dma_as, value + 12)) {
            case RPI_FIRMWARE_EMMC_CLK_ID:
                bcm2835_property_stl(
                    s, value, bufsize, 4, RPI_FIRMWARE_EMMC_CLK_RATE);
                break;
            case RPI_FIRMWARE_UART_CLK_ID:
                bcm2835_property_stl(
                    s, value, bufsize, 4, RPI_FIRMWARE_UART_CLK_RATE);
                break;
            case RPI_FIRMWARE_CORE_CLK_ID:
                bcm2835_property_stl(
                    s, value, bufsize, 4, RPI_FIRMWARE_CORE_CLK_RATE);
                break;
            default:
                bcm2835_property_stl(s, value, bufsize, 4,
                            RPI_FIRMWARE_DEFAULT_CLK_RATE);
                break;
            }
            resplen = 8;
            break;

        case RPI_FWREQ_GET_CLOCKS:
            /* TODO: add more clock IDs if needed */
            bcm2835_property_stl(s, value, bufsize, 0, 0);
            bcm2835_property_stl(s, value, bufsize, 4, RPI_FIRMWARE_ARM_CLK_ID);
            resplen = 8;
            break;

        case RPI_FWREQ_SET_CLOCK_RATE:
        {
            CprmanClockMux mux;
            uint32_t clock_id = ldl_le_phys(&s->dma_as, value + 12);
            uint32_t requested = ldl_le_phys(&s->dma_as, value + 16);
            uint64_t rate = 0;

            if (s->cprman && bcm2835_property_clock_mux(clock_id, &mux)) {
                rate = bcm2835_cprman_clock_set_rate(s->cprman, mux,
                                                      requested);
            }
            bcm2835_property_stl(s, value, bufsize, 4, MIN(rate, UINT32_MAX));
            resplen = 8;
            break;
        }
        case RPI_FWREQ_SET_MAX_CLOCK_RATE:
        case RPI_FWREQ_SET_MIN_CLOCK_RATE:
            qemu_log_mask(LOG_UNIMP,
                          "bcm2835_property: 0x%08x set clock rate NYI\n",
                          tag);
            resplen = 8;
            break;

        /* Temperature */

        case RPI_FWREQ_GET_TEMPERATURE:
            bcm2835_property_stl(s, value, bufsize, 4,
                        s->thermal ?
                        s->thermal->temperature_millicelsius : 25000);
            resplen = 8;
            break;

        case RPI_FWREQ_GET_MAX_TEMPERATURE:
            bcm2835_property_stl(s, value, bufsize, 4,
                        s->thermal && s->thermal->bcm2711 ? 85000 : 99000);
            resplen = 8;
            break;

        /* HDMI EDID */

        case RPI_FWREQ_GET_EDID_BLOCK:
        {
            uint32_t block = ldl_le_phys(&s->dma_as, value + 12);
            uint64_t offset =
                (uint64_t)block * BCM2835_PROPERTY_EDID_BLOCK_SIZE;
            bool present =
                offset + BCM2835_PROPERTY_EDID_BLOCK_SIZE <= s->edid_size[0];

            bcm2835_property_stl(s, value, bufsize, 0, block);
            bcm2835_property_stl(s, value, bufsize, 4, present ? 0 : 1);
            bcm2835_property_write_edid_block(
                s, value, bufsize, 0, block, 8);
            resplen = 8 + BCM2835_PROPERTY_EDID_BLOCK_SIZE;
            break;
        }
        case RPI_FWREQ_GET_EDID_BLOCK_DISPLAY:
        {
            uint32_t block = ldl_le_phys(&s->dma_as, value + 12);
            uint32_t port = ldl_le_phys(&s->dma_as, value + 16);

            bcm2835_property_stl(s, value, bufsize, 0, block);
            bcm2835_property_stl(s, value, bufsize, 4, port);
            bcm2835_property_write_edid_block(
                s, value, bufsize, port, block, 8);
            resplen = 8 + BCM2835_PROPERTY_EDID_BLOCK_SIZE;
            break;
        }

        /* VideoCore-managed GPIO expander */

        case RPI_FWREQ_GET_GPIO_STATE:
        case RPI_FWREQ_SET_GPIO_STATE:
        {
            uint32_t gpio = ldl_le_phys(&s->dma_as, value + 12);
            int line = bcm2835_property_exp_gpio_index(gpio);

            if (line < 0) {
                resplen = 8;
                break;
            }
            if (tag == RPI_FWREQ_SET_GPIO_STATE) {
                s->exp_gpio_state[line] =
                    !!ldl_le_phys(&s->dma_as, value + 16);
                bcm2835_property_exp_gpio_update(s, line);
            }
            bcm2835_property_stl(s, value, bufsize, 0, 0);
            bcm2835_property_stl(s, value, bufsize, 4,
                        s->exp_gpio_state[line]);
            resplen = 8;
            break;
        }
        case RPI_FWREQ_GET_GPIO_CONFIG:
        {
            uint32_t gpio = ldl_le_phys(&s->dma_as, value + 12);
            int line = bcm2835_property_exp_gpio_index(gpio);

            if (line < 0) {
                resplen = 20;
                break;
            }
            bcm2835_property_stl(s, value, bufsize, 0, 0);
            bcm2835_property_stl(s, value, bufsize, 4,
                        s->exp_gpio_direction[line]);
            bcm2835_property_stl(s, value, bufsize, 8,
                        s->exp_gpio_polarity[line]);
            bcm2835_property_stl(s, value, bufsize, 12,
                        s->exp_gpio_term_enable[line]);
            bcm2835_property_stl(s, value, bufsize, 16,
                        s->exp_gpio_term_pull_up[line]);
            resplen = 20;
            break;
        }
        case RPI_FWREQ_SET_GPIO_CONFIG:
        {
            uint32_t gpio = ldl_le_phys(&s->dma_as, value + 12);
            int line = bcm2835_property_exp_gpio_index(gpio);

            if (line < 0) {
                resplen = 24;
                break;
            }
            s->exp_gpio_direction[line] =
                !!ldl_le_phys(&s->dma_as, value + 16);
            s->exp_gpio_polarity[line] =
                !!ldl_le_phys(&s->dma_as, value + 20);
            s->exp_gpio_term_enable[line] =
                !!ldl_le_phys(&s->dma_as, value + 24);
            s->exp_gpio_term_pull_up[line] =
                !!ldl_le_phys(&s->dma_as, value + 28);
            s->exp_gpio_state[line] =
                !!ldl_le_phys(&s->dma_as, value + 32);
            bcm2835_property_exp_gpio_update(s, line);
            bcm2835_property_stl(s, value, bufsize, 0, 0);
            resplen = 24;
            break;
        }

        /* Frame buffer */

        case RPI_FWREQ_FRAMEBUFFER_ALLOCATE:
            bcm2835_fb_set_enabled(s->fbdev, true);
            bcm2835_property_stl(s, value, bufsize, 0, fbconfig.base);
            bcm2835_property_stl(s, value, bufsize, 4,
                        bcm2835_fb_get_size(&fbconfig));
            resplen = 8;
            break;
        case RPI_FWREQ_FRAMEBUFFER_RELEASE:
            bcm2835_fb_set_enabled(s->fbdev, false);
            resplen = 0;
            break;
        case RPI_FWREQ_FRAMEBUFFER_BLANK:
            bcm2835_fb_set_blank(
                s->fbdev, ldl_le_phys(&s->dma_as, value + 12) != 0);
            bcm2835_property_stl(s, value, bufsize, 0,
                        s->fbdev->blank ? 1 : 0);
            resplen = 4;
            break;
        case RPI_FWREQ_FRAMEBUFFER_TEST_PHYSICAL_WIDTH_HEIGHT:
        case RPI_FWREQ_FRAMEBUFFER_TEST_VIRTUAL_WIDTH_HEIGHT:
            resplen = 8;
            break;
        case RPI_FWREQ_FRAMEBUFFER_SET_PHYSICAL_WIDTH_HEIGHT:
            fbconfig.xres = ldl_le_phys(&s->dma_as, value + 12);
            fbconfig.yres = ldl_le_phys(&s->dma_as, value + 16);
            bcm2835_fb_validate_config(&fbconfig);
            fbconfig_updated = true;
            /* fall through */
        case RPI_FWREQ_FRAMEBUFFER_GET_PHYSICAL_WIDTH_HEIGHT:
            bcm2835_property_stl(s, value, bufsize, 0, fbconfig.xres);
            bcm2835_property_stl(s, value, bufsize, 4, fbconfig.yres);
            resplen = 8;
            break;
        case RPI_FWREQ_FRAMEBUFFER_SET_VIRTUAL_WIDTH_HEIGHT:
            fbconfig.xres_virtual = ldl_le_phys(&s->dma_as, value + 12);
            fbconfig.yres_virtual = ldl_le_phys(&s->dma_as, value + 16);
            bcm2835_fb_validate_config(&fbconfig);
            fbconfig_updated = true;
            /* fall through */
        case RPI_FWREQ_FRAMEBUFFER_GET_VIRTUAL_WIDTH_HEIGHT:
            bcm2835_property_stl(s, value, bufsize, 0, fbconfig.xres_virtual);
            bcm2835_property_stl(s, value, bufsize, 4, fbconfig.yres_virtual);
            resplen = 8;
            break;
        case RPI_FWREQ_FRAMEBUFFER_TEST_DEPTH:
            resplen = 4;
            break;
        case RPI_FWREQ_FRAMEBUFFER_SET_DEPTH:
            fbconfig.bpp = ldl_le_phys(&s->dma_as, value + 12);
            bcm2835_fb_validate_config(&fbconfig);
            fbconfig_updated = true;
            /* fall through */
        case RPI_FWREQ_FRAMEBUFFER_GET_DEPTH:
            bcm2835_property_stl(s, value, bufsize, 0, fbconfig.bpp);
            resplen = 4;
            break;
        case RPI_FWREQ_FRAMEBUFFER_TEST_PIXEL_ORDER:
            resplen = 4;
            break;
        case RPI_FWREQ_FRAMEBUFFER_SET_PIXEL_ORDER:
            fbconfig.pixo = ldl_le_phys(&s->dma_as, value + 12);
            bcm2835_fb_validate_config(&fbconfig);
            fbconfig_updated = true;
            /* fall through */
        case RPI_FWREQ_FRAMEBUFFER_GET_PIXEL_ORDER:
            bcm2835_property_stl(s, value, bufsize, 0, fbconfig.pixo);
            resplen = 4;
            break;
        case RPI_FWREQ_FRAMEBUFFER_TEST_ALPHA_MODE:
            resplen = 4;
            break;
        case RPI_FWREQ_FRAMEBUFFER_SET_ALPHA_MODE:
            fbconfig.alpha = ldl_le_phys(&s->dma_as, value + 12);
            bcm2835_fb_validate_config(&fbconfig);
            fbconfig_updated = true;
            /* fall through */
        case RPI_FWREQ_FRAMEBUFFER_GET_ALPHA_MODE:
            bcm2835_property_stl(s, value, bufsize, 0, fbconfig.alpha);
            resplen = 4;
            break;
        case RPI_FWREQ_FRAMEBUFFER_GET_PITCH:
            bcm2835_property_stl(s, value, bufsize, 0,
                        bcm2835_fb_get_pitch(&fbconfig));
            resplen = 4;
            break;
        case RPI_FWREQ_FRAMEBUFFER_TEST_VIRTUAL_OFFSET:
            resplen = 8;
            break;
        case RPI_FWREQ_FRAMEBUFFER_SET_VIRTUAL_OFFSET:
            fbconfig.xoffset = ldl_le_phys(&s->dma_as, value + 12);
            fbconfig.yoffset = ldl_le_phys(&s->dma_as, value + 16);
            bcm2835_fb_validate_config(&fbconfig);
            fbconfig_updated = true;
            /* fall through */
        case RPI_FWREQ_FRAMEBUFFER_GET_VIRTUAL_OFFSET:
            bcm2835_property_stl(s, value, bufsize, 0, fbconfig.xoffset);
            bcm2835_property_stl(s, value, bufsize, 4, fbconfig.yoffset);
            resplen = 8;
            break;
        case RPI_FWREQ_FRAMEBUFFER_GET_OVERSCAN:
            bcm2835_property_stl(
                s, value, bufsize, 0, fbconfig.overscan_top);
            bcm2835_property_stl(
                s, value, bufsize, 4, fbconfig.overscan_bottom);
            bcm2835_property_stl(
                s, value, bufsize, 8, fbconfig.overscan_left);
            bcm2835_property_stl(
                s, value, bufsize, 12, fbconfig.overscan_right);
            resplen = 16;
            break;
        case RPI_FWREQ_FRAMEBUFFER_TEST_OVERSCAN:
            resplen = 16;
            break;
        case RPI_FWREQ_FRAMEBUFFER_SET_OVERSCAN:
            fbconfig.overscan_top =
                ldl_le_phys(&s->dma_as, value + 12);
            fbconfig.overscan_bottom =
                ldl_le_phys(&s->dma_as, value + 16);
            fbconfig.overscan_left =
                ldl_le_phys(&s->dma_as, value + 20);
            fbconfig.overscan_right =
                ldl_le_phys(&s->dma_as, value + 24);
            fbconfig_updated = true;
            resplen = 16;
            break;
        case RPI_FWREQ_FRAMEBUFFER_TEST_LAYER:
            resplen = 4;
            break;
        case RPI_FWREQ_FRAMEBUFFER_SET_LAYER:
            fbconfig.layer = ldl_le_phys(&s->dma_as, value + 12);
            fbconfig_updated = true;
            /* fall through */
        case RPI_FWREQ_FRAMEBUFFER_GET_LAYER:
            bcm2835_property_stl(s, value, bufsize, 0, fbconfig.layer);
            resplen = 4;
            break;
        case RPI_FWREQ_FRAMEBUFFER_TEST_TRANSFORM:
            resplen = 4;
            break;
        case RPI_FWREQ_FRAMEBUFFER_SET_TRANSFORM:
            fbconfig.transform = ldl_le_phys(&s->dma_as, value + 12);
            fbconfig_updated = true;
            /* fall through */
        case RPI_FWREQ_FRAMEBUFFER_GET_TRANSFORM:
            bcm2835_property_stl(s, value, bufsize, 0, fbconfig.transform);
            resplen = 4;
            break;
        case RPI_FWREQ_FRAMEBUFFER_TEST_VSYNC:
            bcm2835_property_stl(s, value, bufsize, 0, 0);
            resplen = 4;
            break;
        case RPI_FWREQ_FRAMEBUFFER_SET_VSYNC:
            wait_for_vsync = true;
            /* fall through */
        case RPI_FWREQ_FRAMEBUFFER_GET_VSYNC:
            bcm2835_property_stl(s, value, bufsize, 0, 0);
            resplen = 4;
            break;
        case RPI_FWREQ_FRAMEBUFFER_GET_PALETTE:
            for (uint32_t index = 0; index < 256; index++) {
                bcm2835_property_stl(
                    s, value, bufsize, index * sizeof(uint32_t),
                    ldl_le_phys(&s->dma_as,
                                s->fbdev->vcram_base +
                                index * sizeof(uint32_t)));
            }
            resplen = 256 * sizeof(uint32_t);
            break;
        case RPI_FWREQ_FRAMEBUFFER_TEST_PALETTE:
        case RPI_FWREQ_FRAMEBUFFER_SET_PALETTE:
        {
            uint32_t offset = ldl_le_phys(&s->dma_as, value + 12);
            uint32_t length = ldl_le_phys(&s->dma_as, value + 16);
            int resp;

            if (offset > 255 || length < 1 || length > 256 - offset) {
                resp = 1; /* invalid request */
            } else {
                if (tag == RPI_FWREQ_FRAMEBUFFER_SET_PALETTE) {
                    for (uint32_t e = 0; e < length; e++) {
                        uint32_t color = ldl_le_phys(
                            &s->dma_as, value + 20 + (e << 2));

                        stl_le_phys(
                            &s->dma_as,
                            s->fbdev->vcram_base + ((offset + e) << 2),
                            color);
                    }
                }
                resp = 0;
            }
            bcm2835_property_stl(s, value, bufsize, 0, resp);
            resplen = 4;
            break;
        }
        case RPI_FWREQ_FRAMEBUFFER_GET_NUM_DISPLAYS:
            bcm2835_property_stl(
                s, value, bufsize, 0, BCM2835_PROPERTY_DISPLAY_COUNT);
            resplen = 4;
            break;
        case RPI_FWREQ_SET_CURSOR_INFO:
        case RPI_FWREQ_SET_CURSOR_STATE:
            resplen = 4;
            break;
        case RPI_FWREQ_FRAMEBUFFER_SET_DISPLAY_NUM:
            s->framebuffer_display_num =
                ldl_le_phys(&s->dma_as, value + 12);
            bcm2835_property_stl(
                s, value, bufsize, 0, s->framebuffer_display_num);
            resplen = 4;
            break;
        case RPI_FWREQ_FRAMEBUFFER_GET_DISPLAY_ID:
        {
            uint32_t index = ldl_le_phys(&s->dma_as, value + 12);

            bcm2835_property_stl(
                s, value, bufsize, 0,
                bcm2835_property_display_id_from_index(index));
            resplen = 4;
            break;
        }
        case RPI_FWREQ_SET_DISPLAY_POWER:
        {
            uint32_t display_id = ldl_le_phys(&s->dma_as, value + 12);
            uint32_t state = ldl_le_phys(&s->dma_as, value + 16);
            int index = bcm2835_property_display_index_from_id(display_id);

            if (index >= 0) {
                if (state <= 1) {
                    s->display_power =
                        deposit32(s->display_power, index, 1, state);
                }
                state = extract32(s->display_power, index, 1);
            }
            bcm2835_property_stl(s, value, bufsize, 0, display_id);
            bcm2835_property_stl(s, value, bufsize, 4, state);
            resplen = 8;
            break;
        }
        case RPI_FWREQ_GET_DISPLAY_TIMING:
        case RPI_FWREQ_SET_TIMING:
        {
            uint8_t timing[BCM2835_PROPERTY_DISPLAY_TIMING_SIZE];
            int port;

            dma_memory_read(&s->dma_as, value + 12, timing,
                            sizeof(timing), MEMTXATTRS_UNSPECIFIED);
            port = bcm2835_property_display_index_from_id(timing[0]);
            if (tag == RPI_FWREQ_SET_TIMING) {
                bcm2835_property_apply_display_timing(s, timing);
            }
            if (port >= 0) {
                bcm2835_property_payload_write(
                    s, value, bufsize, 0, s->display_timing[port],
                    sizeof(timing));
            } else {
                memset(timing + 1, 0, sizeof(timing) - 1);
                bcm2835_property_payload_write(
                    s, value, bufsize, 0, timing, sizeof(timing));
            }
            resplen = sizeof(timing);
            break;
        }

        case RPI_FWREQ_GET_DMA_CHANNELS:
            /* channels 2-5 */
            bcm2835_property_stl(s, value, bufsize, 0, 0x003C);
            resplen = 4;
            break;

        case RPI_FWREQ_GET_COMMAND_LINE:
            /*
             * We follow the firmware behaviour: no NUL terminator is
             * written to the buffer, and if the buffer is too short
             * we report the required length in the response header
             * and copy nothing to the buffer.
             */
            resplen = strlen(s->command_line);
            if (bufsize >= resplen)
                bcm2835_property_payload_write(
                    s, value, bufsize, 0, s->command_line, resplen);
            break;

        case RPI_FWREQ_GET_THROTTLED:
            bcm2835_property_stl(s, value, bufsize, 0,
                        bcm2835_property_get_throttled(s));
            resplen = 4;
            break;
        case RPI_FWREQ_GET_REBOOT_FLAGS:
            bcm2835_property_stl(s, value, bufsize, 0, s->reboot_flags);
            resplen = 4;
            break;
        case RPI_FWREQ_SET_REBOOT_FLAGS:
            if (bufsize >= 4) {
                s->reboot_flags = ldl_le_phys(&s->dma_as, value + 12);
            }
            bcm2835_property_stl(s, value, bufsize, 0, s->reboot_flags);
            resplen = 4;
            break;
        case RPI_FWREQ_NOTIFY_REBOOT:
            resplen = 0;
            break;

        case RPI_FWREQ_NOTIFY_XHCI_RESET:
        {
            uint32_t device_address =
                ldl_le_phys(&s->dma_as, value + 12);

            /*
             * Pi 4B's single downstream VL805 is hardwired at
             * PCI bus 1, slot 0, function 0.  CM4 has no onboard VL805.
             */
            if (s->xhci && device_address == 0x00100000) {
                device_cold_reset(s->xhci);
                if (s->xhci_reset_count != UINT32_MAX) {
                    s->xhci_reset_count++;
                }
            }
            bcm2835_property_stl(s, value, bufsize, 0, device_address);
            resplen = 4;
            break;
        }

        case RPI_FWREQ_VCHIQ_INIT:
            bcm2835_property_stl(s, value, bufsize, 0, 0);
            resplen = VCHI_BUSADDR_SIZE;
            break;

        /* Customer OTP */

        case RPI_FWREQ_GET_CUSTOMER_OTP:
        {
            uint32_t start_num = ldl_le_phys(&s->dma_as, value + 12);
            uint32_t number = ldl_le_phys(&s->dma_as, value + 16);

            resplen = 8 + 4 * number;

            for (uint32_t n = start_num; n < start_num + number &&
                 n < BCM2835_OTP_CUSTOMER_OTP_LEN; n++) {
                uint32_t otp_row = bcm2835_otp_get_row(s->otp,
                                              BCM2835_OTP_CUSTOMER_OTP + n);
                bcm2835_property_stl(
                    s, value, bufsize, 8 + ((n - start_num) << 2), otp_row);
            }
            break;
        }
        case RPI_FWREQ_SET_CUSTOMER_OTP:
        {
            uint32_t start_num = ldl_le_phys(&s->dma_as, value + 12);
            uint32_t number = ldl_le_phys(&s->dma_as, value + 16);

            resplen = 4;

            /* Magic numbers to permanently lock customer OTP */
            if (start_num == BCM2835_OTP_LOCK_NUM1 &&
                number == BCM2835_OTP_LOCK_NUM2) {
                bcm2835_otp_set_row(s->otp,
                                    BCM2835_OTP_ROW_32,
                                    BCM2835_OTP_ROW_32_LOCK);
                break;
            }

            /* If row 32 has the lock bit, don't allow further writes */
            if (bcm2835_otp_get_row(s->otp, BCM2835_OTP_ROW_32) &
                                    BCM2835_OTP_ROW_32_LOCK) {
                break;
            }

            for (uint32_t n = start_num; n < start_num + number &&
                 n < BCM2835_OTP_CUSTOMER_OTP_LEN; n++) {
                uint32_t otp_row = ldl_le_phys(&s->dma_as,
                                      value + 20 + ((n - start_num) << 2));
                bcm2835_otp_set_row(s->otp,
                                    BCM2835_OTP_CUSTOMER_OTP + n, otp_row);
            }
            break;
        }

        /* Device-specific private key */
        case RPI_FWREQ_GET_PRIVATE_KEY:
        {
            uint32_t start_num = ldl_le_phys(&s->dma_as, value + 12);
            uint32_t number = ldl_le_phys(&s->dma_as, value + 16);

            resplen = 8 + 4 * number;

            for (uint32_t n = start_num; n < start_num + number &&
                 n < BCM2835_OTP_PRIVATE_KEY_LEN; n++) {
                uint32_t otp_row = bcm2835_otp_get_row(s->otp,
                                              BCM2835_OTP_PRIVATE_KEY + n);
                bcm2835_property_stl(
                    s, value, bufsize, 8 + ((n - start_num) << 2), otp_row);
            }
            break;
        }
        case RPI_FWREQ_SET_PRIVATE_KEY:
        {
            uint32_t start_num = ldl_le_phys(&s->dma_as, value + 12);
            uint32_t number = ldl_le_phys(&s->dma_as, value + 16);

            resplen = 4;

            /* If row 32 has the lock bit, don't allow further writes */
            if (bcm2835_otp_get_row(s->otp, BCM2835_OTP_ROW_32) &
                                    BCM2835_OTP_ROW_32_LOCK) {
                break;
            }

            for (uint32_t n = start_num; n < start_num + number &&
                 n < BCM2835_OTP_PRIVATE_KEY_LEN; n++) {
                uint32_t otp_row = ldl_le_phys(&s->dma_as,
                                      value + 20 + ((n - start_num) << 2));
                bcm2835_otp_set_row(s->otp,
                                    BCM2835_OTP_PRIVATE_KEY + n, otp_row);
            }
            break;
        }
        default:
            qemu_log_mask(LOG_UNIMP,
                          "bcm2835_property: unhandled tag 0x%08x\n", tag);
            handled = false;
            break;
        }

        trace_bcm2835_mbox_property(tag, bufsize, resplen);
        if (tag == 0) {
            break;
        }

        if (handled) {
            stl_le_phys(&s->dma_as, value + 8, (1 << 31) | resplen);
        }
        value += QEMU_ALIGN_UP(bufsize, 4) + 12;
    }

    /* Reconfigure framebuffer if required */
    if (fbconfig_updated) {
        bcm2835_fb_reconfigure(s->fbdev, &fbconfig);
    }

    /* Buffer response code */
    stl_le_phys(&s->dma_as, s->addr + 4, (1 << 31));
    if (wait_for_vsync) {
        bcm2835_property_wait_for_vsync(s);
    }
}

static uint64_t bcm2835_property_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    BCM2835PropertyState *s = opaque;
    uint32_t res = 0;

    switch (offset) {
    case MBOX_AS_DATA:
        res = MBOX_CHAN_PROPERTY | s->addr;
        s->pending = false;
        qemu_set_irq(s->mbox_irq, 0);
        break;

    case MBOX_AS_PENDING:
        res = s->pending;
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset %"HWADDR_PRIx"\n",
                      __func__, offset);
        return 0;
    }

    return res;
}

static void bcm2835_property_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    BCM2835PropertyState *s = opaque;

    switch (offset) {
    case MBOX_AS_DATA:
        /* bcm2835_mbox should check our pending status before pushing */
        assert(!s->pending);
        s->pending = true;
        bcm2835_property_mbox_push(s, value);
        if (!s->vsync_wait) {
            qemu_set_irq(s->mbox_irq, 1);
        }
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset %"HWADDR_PRIx"\n",
                      __func__, offset);
        return;
    }
}

static const MemoryRegionOps bcm2835_property_ops = {
    .read = bcm2835_property_read,
    .write = bcm2835_property_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static int bcm2835_property_post_load(void *opaque, int version_id)
{
    BCM2835PropertyState *s = opaque;

    if (version_id < 10) {
        s->vsync_wait = false;
        timer_del(s->vsync_timer);
    } else if (s->vsync_wait != timer_pending(s->vsync_timer) ||
               (s->vsync_wait && !s->pending)) {
        return -EINVAL;
    }
    if (version_id < 8) {
        s->framebuffer_display_num = 0;
        s->display_power = BCM2835_PROPERTY_DISPLAY_POWER_DEFAULT_MASK;
    }
    if (s->framebuffer_display_num >= BCM2835_PROPERTY_DISPLAY_COUNT ||
        s->display_power & ~BCM2835_PROPERTY_DISPLAY_POWER_DEFAULT_MASK) {
        return -EINVAL;
    }
    if (version_id < 7) {
        memset(s->edid_size, 0, sizeof(s->edid_size));
        memset(s->edid, 0, sizeof(s->edid));
    }
    for (unsigned int port = 0;
         port < BCM2835_PROPERTY_EDID_PORT_COUNT; port++) {
        if (s->edid_size[port] > BCM2835_PROPERTY_EDID_MAX_SIZE ||
            s->edid_size[port] % BCM2835_PROPERTY_EDID_BLOCK_SIZE) {
            return -EINVAL;
        }
        if (version_id < 9) {
            bcm2835_property_derive_display_timing(s, port);
        } else if (!bcm2835_property_display_timing_state_is_valid(
                       s->display_timing[port], port)) {
            return -EINVAL;
        }
    }
    if (version_id < 5) {
        s->power_state = BCM2835_PROPERTY_POWER_DEFAULT_MASK;
    }
    if (version_id < 6) {
        s->xhci_reset_count = 0;
    }
    if (s->power_state & ~BCM2835_PROPERTY_POWER_DEFAULT_MASK) {
        return -EINVAL;
    }
    if (version_id < 4) {
        s->throttled_current = 0;
        s->throttled_history = 0;
    }
    if ((s->throttled_current &
         ~BCM2835_PROPERTY_THROTTLED_CURRENT_MASK) ||
        (s->throttled_history &
         ~BCM2835_PROPERTY_THROTTLED_CURRENT_MASK)) {
        return -EINVAL;
    }
    if (version_id < 3) {
        s->reboot_flags = 0;
    }
    if (version_id < 2) {
        memset(s->exp_gpio_direction, 0, sizeof(s->exp_gpio_direction));
        memset(s->exp_gpio_polarity, 0, sizeof(s->exp_gpio_polarity));
        memset(s->exp_gpio_term_enable, 0,
               sizeof(s->exp_gpio_term_enable));
        memset(s->exp_gpio_term_pull_up, 0,
               sizeof(s->exp_gpio_term_pull_up));
        memset(s->exp_gpio_state, 0, sizeof(s->exp_gpio_state));
    }
    for (int line = 0; line < BCM2835_PROPERTY_EXP_GPIO_COUNT; line++) {
        bcm2835_property_exp_gpio_update(s, line);
    }
    return 0;
}

static const VMStateDescription vmstate_bcm2835_property = {
    .name = TYPE_BCM2835_PROPERTY,
    .version_id = 10,
    .minimum_version_id = 1,
    .post_load = bcm2835_property_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_MACADDR(macaddr, BCM2835PropertyState),
        VMSTATE_UINT32(addr, BCM2835PropertyState),
        VMSTATE_BOOL(pending, BCM2835PropertyState),
        VMSTATE_BOOL_V(vsync_wait, BCM2835PropertyState, 10),
        VMSTATE_TIMER_PTR_V(vsync_timer, BCM2835PropertyState, 10),
        VMSTATE_UINT32_V(reboot_flags, BCM2835PropertyState, 3),
        VMSTATE_UINT32_V(power_state, BCM2835PropertyState, 5),
        VMSTATE_UINT32_V(xhci_reset_count, BCM2835PropertyState, 6),
        VMSTATE_UINT32_V(framebuffer_display_num, BCM2835PropertyState, 8),
        VMSTATE_UINT32_V(display_power, BCM2835PropertyState, 8),
        VMSTATE_UINT32_ARRAY_V(edid_size, BCM2835PropertyState,
                               BCM2835_PROPERTY_EDID_PORT_COUNT, 7),
        VMSTATE_UINT8_2DARRAY_V(edid, BCM2835PropertyState,
                                BCM2835_PROPERTY_EDID_PORT_COUNT,
                                BCM2835_PROPERTY_EDID_MAX_SIZE, 7),
        VMSTATE_UINT8_2DARRAY_V(display_timing, BCM2835PropertyState,
                                BCM2835_PROPERTY_DISPLAY_COUNT,
                                BCM2835_PROPERTY_DISPLAY_TIMING_SIZE, 9),
        VMSTATE_UINT32_V(throttled_current, BCM2835PropertyState, 4),
        VMSTATE_UINT32_V(throttled_history, BCM2835PropertyState, 4),
        VMSTATE_UINT32_ARRAY_V(exp_gpio_direction, BCM2835PropertyState,
                               BCM2835_PROPERTY_EXP_GPIO_COUNT, 2),
        VMSTATE_UINT32_ARRAY_V(exp_gpio_polarity, BCM2835PropertyState,
                               BCM2835_PROPERTY_EXP_GPIO_COUNT, 2),
        VMSTATE_UINT32_ARRAY_V(exp_gpio_term_enable, BCM2835PropertyState,
                               BCM2835_PROPERTY_EXP_GPIO_COUNT, 2),
        VMSTATE_UINT32_ARRAY_V(exp_gpio_term_pull_up, BCM2835PropertyState,
                               BCM2835_PROPERTY_EXP_GPIO_COUNT, 2),
        VMSTATE_UINT32_ARRAY_V(exp_gpio_state, BCM2835PropertyState,
                               BCM2835_PROPERTY_EXP_GPIO_COUNT, 2),
        VMSTATE_END_OF_LIST()
    }
};

static void bcm2835_property_init(Object *obj)
{
    BCM2835PropertyState *s = BCM2835_PROPERTY(obj);

    s->vsync_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                  bcm2835_property_vsync_complete, s);
    memory_region_init_io(&s->iomem, OBJECT(s), &bcm2835_property_ops, s,
                          TYPE_BCM2835_PROPERTY, 0x10);

    /*
     * bcm2835_property_ops call into bcm2835_mbox, which in-turn reads from
     * iomem. As such, mark iomem as re-entracy safe.
     */
    s->iomem.disable_reentrancy_guard = true;

    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->mbox_irq);
    qdev_init_gpio_in_named(DEVICE(obj), bcm2835_property_exp_gpio_input,
                            "exp-gpio-in",
                            BCM2835_PROPERTY_EXP_GPIO_COUNT);
    qdev_init_gpio_out_named(DEVICE(obj), s->exp_gpio_out, "exp-gpio-out",
                             BCM2835_PROPERTY_EXP_GPIO_COUNT);
    object_property_add_uint32_ptr(obj, "xhci-reset-count",
                                   &s->xhci_reset_count,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "framebuffer-display-num",
                                   &s->framebuffer_display_num,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "display-power-mask",
                                   &s->display_power,
                                   OBJ_PROP_FLAG_READ);
}

static void bcm2835_property_finalize(Object *obj)
{
    BCM2835PropertyState *s = BCM2835_PROPERTY(obj);

    timer_free(s->vsync_timer);
}

static void bcm2835_property_reset(DeviceState *dev)
{
    BCM2835PropertyState *s = BCM2835_PROPERTY(dev);

    timer_del(s->vsync_timer);
    s->vsync_wait = false;
    qemu_set_irq(s->mbox_irq, 0);
    s->pending = false;
    s->power_state = BCM2835_PROPERTY_POWER_DEFAULT_MASK;
    s->xhci_reset_count = 0;
    s->framebuffer_display_num = 0;
    s->display_power = BCM2835_PROPERTY_DISPLAY_POWER_DEFAULT_MASK;
    for (unsigned int port = 0;
         port < BCM2835_PROPERTY_DISPLAY_COUNT; port++) {
        bcm2835_property_derive_display_timing(s, port);
    }
    /*
     * reboot_flags intentionally survives reset until the behavioral
     * bootloader consumes it; Linux sets it immediately before reboot.
     */
    memset(s->exp_gpio_direction, 0, sizeof(s->exp_gpio_direction));
    memset(s->exp_gpio_polarity, 0, sizeof(s->exp_gpio_polarity));
    memset(s->exp_gpio_term_enable, 0, sizeof(s->exp_gpio_term_enable));
    memset(s->exp_gpio_term_pull_up, 0,
           sizeof(s->exp_gpio_term_pull_up));
    memset(s->exp_gpio_state, 0, sizeof(s->exp_gpio_state));
    for (int line = 0; line < BCM2835_PROPERTY_EXP_GPIO_COUNT; line++) {
        bcm2835_property_exp_gpio_update(s, line);
    }
}

static void bcm2835_property_realize(DeviceState *dev, Error **errp)
{
    BCM2835PropertyState *s = BCM2835_PROPERTY(dev);
    Object *obj;

    obj = object_property_get_link(OBJECT(dev), "fb", &error_abort);
    s->fbdev = BCM2835_FB(obj);

    obj = object_property_get_link(OBJECT(dev), "dma-mr", &error_abort);
    s->dma_mr = MEMORY_REGION(obj);
    address_space_init(&s->dma_as, s->dma_mr, TYPE_BCM2835_PROPERTY "-memory");

    obj = object_property_get_link(OBJECT(dev), "otp", &error_abort);
    s->otp = BCM2835_OTP(obj);

    /* TODO: connect to MAC address of USB NIC device, once we emulate it */
    qemu_macaddr_default_if_unset(&s->macaddr);

    bcm2835_property_reset(dev);
}

void bcm2835_property_set_mac(BCM2835PropertyState *s,
                              const MACAddr *mac)
{
    s->macaddr = *mac;
}

uint32_t bcm2835_property_get_throttled(const BCM2835PropertyState *s)
{
    return s->throttled_current |
           (s->throttled_history <<
            BCM2835_PROPERTY_THROTTLED_HISTORY_SHIFT);
}

void bcm2835_property_set_throttled_current(BCM2835PropertyState *s,
                                            uint32_t current)
{
    assert(!(current & ~BCM2835_PROPERTY_THROTTLED_CURRENT_MASK));
    s->throttled_current = current;
    s->throttled_history |= current;
}

void bcm2835_property_set_thermal(BCM2835PropertyState *s,
                                  Bcm2835ThermalState *thermal)
{
    s->thermal = thermal;
}

void bcm2835_property_set_cprman(BCM2835PropertyState *s,
                                 BCM2835CprmanState *cprman)
{
    s->cprman = cprman;
}

void bcm2835_property_set_xhci(BCM2835PropertyState *s,
                              DeviceState *xhci)
{
    s->xhci = xhci;
}

void bcm2835_property_set_edid(BCM2835PropertyState *s, unsigned int port,
                               const uint8_t *edid, size_t size)
{
    assert(port < BCM2835_PROPERTY_EDID_PORT_COUNT);
    assert(size <= BCM2835_PROPERTY_EDID_MAX_SIZE);
    assert(size % BCM2835_PROPERTY_EDID_BLOCK_SIZE == 0);
    assert(edid || !size);

    memset(s->edid[port], 0, sizeof(s->edid[port]));
    if (size) {
        memcpy(s->edid[port], edid, size);
    }
    s->edid_size[port] = size;
    bcm2835_property_derive_display_timing(s, port);
}

size_t bcm2835_property_get_edid(const BCM2835PropertyState *s,
                                 unsigned int port, const uint8_t **edid)
{
    assert(port < BCM2835_PROPERTY_EDID_PORT_COUNT);
    assert(edid);

    *edid = s->edid[port];
    return s->edid_size[port];
}

static const Property bcm2835_property_props[] = {
    DEFINE_PROP_UINT32("board-rev", BCM2835PropertyState, board_rev, 0),
    DEFINE_PROP_STRING("command-line", BCM2835PropertyState, command_line),
};

static void bcm2835_property_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, bcm2835_property_props);
    dc->realize = bcm2835_property_realize;
    dc->vmsd = &vmstate_bcm2835_property;
    device_class_set_legacy_reset(dc, bcm2835_property_reset);
}

static const TypeInfo bcm2835_property_info = {
    .name          = TYPE_BCM2835_PROPERTY,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2835PropertyState),
    .class_init    = bcm2835_property_class_init,
    .instance_init = bcm2835_property_init,
    .instance_finalize = bcm2835_property_finalize,
};

static void bcm2835_property_register_types(void)
{
    type_register_static(&bcm2835_property_info);
}

type_init(bcm2835_property_register_types)
