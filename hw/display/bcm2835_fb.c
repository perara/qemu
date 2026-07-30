/*
 * Raspberry Pi emulation (c) 2012 Gregory Estrade
 * Refactoring for Pi2 Copyright (c) 2015, Microsoft. Written by Andrew Baumann.
 *
 * Heavily based on milkymist-vgafb.c, copyright terms below:
 *  QEMU model of the Milkymist VGA framebuffer.
 *
 *  Copyright (c) 2010-2012 Michael Walle <michael@walle.cc>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/display/bcm2835_fb.h"
#include "hw/core/hw-error.h"
#include "hw/core/irq.h"
#include "ui/console.h"
#include "framebuffer.h"
#include "ui/pixel_ops.h"
#include "hw/misc/bcm2835_mbox_defs.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/dma.h"

#define DEFAULT_VCRAM_SIZE 0x4000000
#define BCM2835_FB_OFFSET  0x00100000

/* Maximum permitted framebuffer size; experimentally determined on an rpi2 */
#define XRES_MAX 3840
#define YRES_MAX 2560
/* Framebuffer size used if guest requests zero size */
#define XRES_SMALL 592
#define YRES_SMALL 488

#define VC_IMAGE_TRANSFORM_HFLIP     BIT(0)
#define VC_IMAGE_TRANSFORM_VFLIP     BIT(1)
#define VC_IMAGE_TRANSFORM_TRANSPOSE BIT(2)

static void fb_invalidate_display(void *opaque)
{
    BCM2835FBState *s = BCM2835_FB(opaque);

    s->invalidate = true;
}

static void draw_line_src16(void *opaque, uint8_t *dst, const uint8_t *src,
                            int width, int deststep)
{
    BCM2835FBState *s = opaque;
    uint16_t rgb565;
    uint32_t rgb888;
    uint8_t r, g, b;
    DisplaySurface *surface = qemu_console_surface(s->con);
    int bpp = surface_bits_per_pixel(surface);

    while (width--) {
        switch (s->config.bpp) {
        case 8:
            /* lookup palette starting at video ram base
             * TODO: cache translation, rather than doing this each time!
             */
            rgb888 = ldl_le_phys(&s->dma_as, s->vcram_base + (*src << 2));
            r = (rgb888 >> 0) & 0xff;
            g = (rgb888 >> 8) & 0xff;
            b = (rgb888 >> 16) & 0xff;
            src++;
            break;
        case 16:
            rgb565 = lduw_le_p(src);
            r = ((rgb565 >> 11) & 0x1f) << 3;
            g = ((rgb565 >>  5) & 0x3f) << 2;
            b = ((rgb565 >>  0) & 0x1f) << 3;
            src += 2;
            break;
        case 24:
            rgb888 = src[0] | (src[1] << 8) | (src[2] << 16);
            r = (rgb888 >> 0) & 0xff;
            g = (rgb888 >> 8) & 0xff;
            b = (rgb888 >> 16) & 0xff;
            src += 3;
            break;
        case 32:
            rgb888 = ldl_le_p(src);
            r = (rgb888 >> 0) & 0xff;
            g = (rgb888 >> 8) & 0xff;
            b = (rgb888 >> 16) & 0xff;
            src += 4;
            break;
        default:
            r = 0;
            g = 0;
            b = 0;
            break;
        }

        if (s->config.pixo == 0) {
            /* swap to BGR pixel format */
            uint8_t tmp = r;
            r = b;
            b = tmp;
        }

        switch (bpp) {
        case 8:
            *dst = rgb_to_pixel8(r, g, b);
            break;
        case 15:
            *(uint16_t *)dst = rgb_to_pixel15(r, g, b);
            break;
        case 16:
            *(uint16_t *)dst = rgb_to_pixel16(r, g, b);
            break;
        case 24:
            rgb888 = rgb_to_pixel24(r, g, b);
            dst[0] = rgb888 & 0xff;
            dst[1] = (rgb888 >> 8) & 0xff;
            dst[2] = (rgb888 >> 16) & 0xff;
            break;
        case 32:
            *(uint32_t *)dst = rgb_to_pixel32(r, g, b);
            break;
        default:
            return;
        }
        dst += deststep;
    }
}

static bool fb_use_offsets(BCM2835FBConfig *config)
{
    /*
     * Return true if we should use the viewport offsets.
     * Experimentally, the hardware seems to do this only if the
     * viewport size is larger than the physical screen. (It doesn't
     * prevent the guest setting this silly viewport setting, though...)
     */
    return config->xres_virtual > config->xres ||
        config->yres_virtual > config->yres;
}

static void bcm2835_fb_output_size(const BCM2835FBConfig *config,
                                   uint32_t *width, uint32_t *height)
{
    if (config->transform & VC_IMAGE_TRANSFORM_TRANSPOSE) {
        *width = config->yres;
        *height = config->xres;
    } else {
        *width = config->xres;
        *height = config->yres;
    }
}

static bool bcm2835_fb_overscan_is_valid(const BCM2835FBConfig *config,
                                          uint32_t top, uint32_t bottom,
                                          uint32_t left, uint32_t right)
{
    uint32_t width;
    uint32_t height;

    bcm2835_fb_output_size(config, &width, &height);
    return (uint64_t)top + bottom < height &&
           (uint64_t)left + right < width;
}

static bool bcm2835_fb_has_overscan(const BCM2835FBConfig *config)
{
    return config->overscan_top || config->overscan_bottom ||
           config->overscan_left || config->overscan_right;
}

static bool bcm2835_fb_cursor_info_is_valid(uint32_t width, uint32_t height,
                                             uint32_t address,
                                             uint32_t hotspot_x,
                                             uint32_t hotspot_y)
{
    uint64_t size = (uint64_t)width * height * sizeof(uint32_t);

    return width >= 16 && width <= 64 &&
           height >= 16 && height <= 64 &&
           address && address + size <= (1ULL << 32) &&
           hotspot_x < width && hotspot_y < height;
}

/*
 * Overlaying the cursor and rescaling for overscan are both compositing
 * operations.  A QEMU built without pixman gets pixman-minimal.h, which
 * allocates images but cannot composite them, so both effects are left
 * out there and the framebuffer is presented unscaled.
 */
#ifdef CONFIG_PIXMAN
static void bcm2835_fb_draw_cursor(BCM2835FBState *s,
                                   DisplaySurface *surface)
{
    g_autofree uint32_t *pixels = NULL;
    pixman_image_t *cursor;
    int64_t x = (int32_t)s->cursor_x;
    int64_t y = (int32_t)s->cursor_y;

    if (!s->cursor_enabled || !s->cursor_info_valid) {
        return;
    }

    x -= s->cursor_hotspot_x;
    y -= s->cursor_hotspot_y;
    if (s->cursor_flags & BIT(0)) {
        bool transpose =
            s->config.transform & VC_IMAGE_TRANSFORM_TRANSPOSE;
        uint32_t xoff = 0;
        uint32_t yoff = 0;

        if (fb_use_offsets(&s->config)) {
            xoff = s->config.xoffset;
            yoff = s->config.yoffset;
        }
        x -= xoff;
        y -= yoff;
        if (transpose) {
            int64_t old_x = x;

            x = y;
            y = old_x;
            if (s->config.transform & VC_IMAGE_TRANSFORM_HFLIP) {
                x = s->config.yres - x - s->cursor_width;
            }
            if (s->config.transform & VC_IMAGE_TRANSFORM_VFLIP) {
                y = s->config.xres - y - s->cursor_height;
            }
        } else {
            if (s->config.transform & VC_IMAGE_TRANSFORM_HFLIP) {
                x = s->config.xres - x - s->cursor_width;
            }
            if (s->config.transform & VC_IMAGE_TRANSFORM_VFLIP) {
                y = s->config.yres - y - s->cursor_height;
            }
        }
    }

    pixels = g_new0(uint32_t, s->cursor_width * s->cursor_height);
    if (dma_memory_read(&s->dma_as, s->cursor_address, pixels,
                        s->cursor_width * s->cursor_height *
                        sizeof(uint32_t), MEMTXATTRS_UNSPECIFIED) !=
        MEMTX_OK) {
        return;
    }
    cursor = pixman_image_create_bits(
        PIXMAN_a8r8g8b8, s->cursor_width, s->cursor_height,
        pixels, s->cursor_width * sizeof(uint32_t));
    if (!cursor) {
        return;
    }
    pixman_image_composite(PIXMAN_OP_OVER, cursor, NULL, surface->image,
                           0, 0, 0, 0, x, y,
                           s->cursor_width, s->cursor_height);
    pixman_image_unref(cursor);
}
#endif /* CONFIG_PIXMAN */

static bool fb_update_display(void *opaque)
{
    BCM2835FBState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    int first = 0;
    int last = 0;
    int src_width = 0;
    int dest_row_pitch;
    int dest_col_pitch;
    int dest_bpp;
    uint32_t xoff = 0, yoff = 0;
    bool transpose;
    bool cursor_active;
    bool overscan_active;
    bool redraw_all;
    pixman_image_t *overscan_image = NULL;
    DisplaySurface overscan_surface = { 0 };
    DisplaySurface *draw_surface = surface;

    if (s->lock || !s->config.xres) {
        return true;
    }

    if (!s->enabled || s->blank) {
        memset(surface_data(surface), 0,
               surface_stride(surface) * surface_height(surface));
        qemu_console_update(s->con, 0, 0,
                            surface_width(surface), surface_height(surface));
        s->invalidate = false;
        return true;
    }

    cursor_active = s->cursor_enabled && s->cursor_info_valid;
    overscan_active = bcm2835_fb_has_overscan(&s->config) &&
                      bcm2835_fb_overscan_is_valid(
                          &s->config, s->config.overscan_top,
                          s->config.overscan_bottom,
                          s->config.overscan_left,
                          s->config.overscan_right);
#ifndef CONFIG_PIXMAN
    cursor_active = false;
    overscan_active = false;
#endif
    if (overscan_active) {
        overscan_image = pixman_image_create_bits(
            surface_format(surface), surface_width(surface),
            surface_height(surface), NULL, 0);
        if (!overscan_image) {
            return true;
        }
        overscan_surface.image = overscan_image;
        draw_surface = &overscan_surface;
    }
    redraw_all = s->invalidate || cursor_active || overscan_active;
    src_width = bcm2835_fb_get_pitch(&s->config);
    if (fb_use_offsets(&s->config)) {
        xoff = s->config.xoffset;
        yoff = s->config.yoffset;
    }

    switch (surface_bits_per_pixel(draw_surface)) {
    case 0:
        if (overscan_image) {
            pixman_image_unref(overscan_image);
        }
        return true;
    case 8:
        dest_bpp = 1;
        break;
    case 15:
    case 16:
        dest_bpp = 2;
        break;
    case 24:
        dest_bpp = 3;
        break;
    case 32:
        dest_bpp = 4;
        break;
    default:
        hw_error("bcm2835_fb: bad color depth\n");
        return true;
    }

    transpose = s->config.transform & VC_IMAGE_TRANSFORM_TRANSPOSE;
    if (transpose) {
        dest_row_pitch = dest_bpp;
        dest_col_pitch = surface_stride(draw_surface);
    } else {
        dest_row_pitch = surface_stride(draw_surface);
        dest_col_pitch = dest_bpp;
    }
    if (s->config.transform & VC_IMAGE_TRANSFORM_HFLIP) {
        if (transpose) {
            dest_row_pitch = -dest_row_pitch;
        } else {
            dest_col_pitch = -dest_col_pitch;
        }
    }
    if (s->config.transform & VC_IMAGE_TRANSFORM_VFLIP) {
        if (transpose) {
            dest_col_pitch = -dest_col_pitch;
        } else {
            dest_row_pitch = -dest_row_pitch;
        }
    }

    if (s->invalidate) {
        hwaddr base = s->config.base +
            (hwaddr)xoff * (s->config.bpp >> 3) +
            (hwaddr)yoff * src_width;
        framebuffer_update_memory_section(&s->fbsection, s->dma_mr,
                                          base,
                                          s->config.yres, src_width);
    }

    framebuffer_update_display(draw_surface, &s->fbsection,
                               s->config.xres, s->config.yres,
                               src_width, dest_row_pitch, dest_col_pitch,
                               redraw_all,
                               draw_line_src16, s, &first, &last);

#ifdef CONFIG_PIXMAN
    if (overscan_active) {
        pixman_transform_t transform;
        uint32_t inner_width =
            surface_width(surface) - s->config.overscan_left -
            s->config.overscan_right;
        uint32_t inner_height =
            surface_height(surface) - s->config.overscan_top -
            s->config.overscan_bottom;

        memset(surface_data(surface), 0,
               surface_stride(surface) * surface_height(surface));
        pixman_transform_init_scale(
            &transform,
            pixman_double_to_fixed((double)surface_width(surface) /
                                   inner_width),
            pixman_double_to_fixed((double)surface_height(surface) /
                                   inner_height));
        pixman_image_set_transform(overscan_image, &transform);
        pixman_image_set_filter(
            overscan_image, PIXMAN_FILTER_BILINEAR, NULL, 0);
        pixman_image_set_repeat(overscan_image, PIXMAN_REPEAT_PAD);
        pixman_image_composite(
            PIXMAN_OP_SRC, overscan_image, NULL, surface->image,
            0, 0, 0, 0,
            s->config.overscan_left, s->config.overscan_top,
            inner_width, inner_height);
        first = 0;
        last = surface_height(surface) - 1;
    }
    bcm2835_fb_draw_cursor(s, surface);
#endif /* CONFIG_PIXMAN */
    if (cursor_active) {
        first = 0;
        last = surface_height(surface) - 1;
    }
    if (first >= 0) {
        if (s->config.transform == 0 && !cursor_active) {
            qemu_console_update(s->con, 0, first, s->config.xres,
                                last - first + 1);
        } else {
            qemu_console_update(s->con, 0, 0, surface_width(surface),
                                surface_height(surface));
        }
    }

    s->invalidate = false;
    if (overscan_image) {
        pixman_image_unref(overscan_image);
    }
    return true;
}

bool bcm2835_fb_set_overscan(BCM2835FBConfig *config, uint32_t top,
                             uint32_t bottom, uint32_t left,
                             uint32_t right)
{
    if (!bcm2835_fb_overscan_is_valid(
            config, top, bottom, left, right)) {
        return false;
    }
    config->overscan_top = top;
    config->overscan_bottom = bottom;
    config->overscan_left = left;
    config->overscan_right = right;
    return true;
}

void bcm2835_fb_validate_config(BCM2835FBConfig *config)
{
    /*
     * Validate the config, and clip any bogus values into range,
     * as the hardware does. Note that fb_update_display() relies on
     * this happening to prevent it from performing out-of-range
     * accesses on redraw.
     */
    config->xres = MIN(config->xres, XRES_MAX);
    config->xres_virtual = MIN(config->xres_virtual, XRES_MAX);
    config->yres = MIN(config->yres, YRES_MAX);
    config->yres_virtual = MIN(config->yres_virtual, YRES_MAX);

    /*
     * These are not minima: a 40x40 framebuffer will be accepted.
     * They're only used as defaults if the guest asks for zero size.
     */
    if (config->xres == 0) {
        config->xres = XRES_SMALL;
    }
    if (config->yres == 0) {
        config->yres = YRES_SMALL;
    }
    if (config->xres_virtual == 0) {
        config->xres_virtual = config->xres;
    }
    if (config->yres_virtual == 0) {
        config->yres_virtual = config->yres;
    }

    /*
     * The virtual surface contains the physical viewport.  Enforcing this
     * before clipping offsets also prevents unsigned underflow below.
     */
    config->xres_virtual = MAX(config->xres_virtual, config->xres);
    config->yres_virtual = MAX(config->yres_virtual, config->yres);

    if (fb_use_offsets(config)) {
        /* Clip the offsets so the viewport is within the physical screen */
        config->xoffset = MIN(config->xoffset,
                              config->xres_virtual - config->xres);
        config->yoffset = MIN(config->yoffset,
                              config->yres_virtual - config->yres);
    }
    if (!bcm2835_fb_overscan_is_valid(
            config, config->overscan_top, config->overscan_bottom,
            config->overscan_left, config->overscan_right)) {
        uint32_t width;
        uint32_t height;

        bcm2835_fb_output_size(config, &width, &height);
        config->overscan_top = MIN(config->overscan_top, height - 1);
        config->overscan_bottom = MIN(
            config->overscan_bottom,
            height - config->overscan_top - 1);
        config->overscan_left = MIN(config->overscan_left, width - 1);
        config->overscan_right = MIN(
            config->overscan_right,
            width - config->overscan_left - 1);
    }
}

static void bcm2835_fb_resize_console(BCM2835FBState *s)
{
    uint32_t width = s->config.xres;
    uint32_t height = s->config.yres;

    if (!s->con) {
        return;
    }
    if (s->config.transform & VC_IMAGE_TRANSFORM_TRANSPOSE) {
        width = s->config.yres;
        height = s->config.xres;
    }
    qemu_console_resize(s->con, width, height);
}

void bcm2835_fb_reconfigure(BCM2835FBState *s, BCM2835FBConfig *newconfig)
{
    s->lock = true;

    bcm2835_fb_validate_config(newconfig);
    s->config = *newconfig;

    s->invalidate = true;
    bcm2835_fb_resize_console(s);
    s->lock = false;
}

void bcm2835_fb_set_blank(BCM2835FBState *s, bool blank)
{
    if (s->blank == blank) {
        return;
    }

    s->blank = blank;
    s->invalidate = true;
}

void bcm2835_fb_set_enabled(BCM2835FBState *s, bool enabled)
{
    if (s->enabled == enabled) {
        return;
    }

    s->enabled = enabled;
    s->invalidate = true;
}

bool bcm2835_fb_set_cursor_info(BCM2835FBState *s, uint32_t width,
                                uint32_t height, uint32_t address,
                                uint32_t hotspot_x, uint32_t hotspot_y)
{
    if (!bcm2835_fb_cursor_info_is_valid(
            width, height, address, hotspot_x, hotspot_y)) {
        return false;
    }

    s->cursor_width = width;
    s->cursor_height = height;
    s->cursor_address = address;
    s->cursor_hotspot_x = hotspot_x;
    s->cursor_hotspot_y = hotspot_y;
    s->cursor_info_valid = true;
    s->invalidate = true;
    return true;
}

bool bcm2835_fb_set_cursor_state(BCM2835FBState *s, uint32_t enable,
                                 uint32_t x, uint32_t y, uint32_t flags)
{
    if (enable > 1 || flags & ~BIT(0)) {
        return false;
    }

    s->cursor_enabled = enable;
    s->cursor_x = x;
    s->cursor_y = y;
    s->cursor_flags = flags;
    s->invalidate = true;
    return true;
}

static void bcm2835_fb_mbox_push(BCM2835FBState *s, uint32_t value)
{
    uint32_t pitch;
    uint32_t size;
    BCM2835FBConfig newconf;

    value &= ~0xf;

    newconf.xres = ldl_le_phys(&s->dma_as, value);
    newconf.yres = ldl_le_phys(&s->dma_as, value + 4);
    newconf.xres_virtual = ldl_le_phys(&s->dma_as, value + 8);
    newconf.yres_virtual = ldl_le_phys(&s->dma_as, value + 12);
    newconf.bpp = ldl_le_phys(&s->dma_as, value + 20);
    newconf.xoffset = ldl_le_phys(&s->dma_as, value + 24);
    newconf.yoffset = ldl_le_phys(&s->dma_as, value + 28);

    newconf.base = s->vcram_base + BCM2835_FB_OFFSET;

    /* Copy fields which we don't want to change from the existing config */
    newconf.pixo = s->config.pixo;
    newconf.alpha = s->config.alpha;
    newconf.overscan_top = s->config.overscan_top;
    newconf.overscan_bottom = s->config.overscan_bottom;
    newconf.overscan_left = s->config.overscan_left;
    newconf.overscan_right = s->config.overscan_right;
    newconf.layer = s->config.layer;
    newconf.transform = s->config.transform;
    newconf.vsync = s->config.vsync;

    bcm2835_fb_validate_config(&newconf);

    pitch = bcm2835_fb_get_pitch(&newconf);
    size = bcm2835_fb_get_size(&newconf);

    stl_le_phys(&s->dma_as, value + 16, pitch);
    stl_le_phys(&s->dma_as, value + 32, newconf.base);
    stl_le_phys(&s->dma_as, value + 36, size);

    bcm2835_fb_reconfigure(s, &newconf);
}

static uint64_t bcm2835_fb_read(void *opaque, hwaddr offset, unsigned size)
{
    BCM2835FBState *s = opaque;
    uint32_t res = 0;

    switch (offset) {
    case MBOX_AS_DATA:
        res = MBOX_CHAN_FB;
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

static void bcm2835_fb_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    BCM2835FBState *s = opaque;

    switch (offset) {
    case MBOX_AS_DATA:
        /* bcm2835_mbox should check our pending status before pushing */
        assert(!s->pending);
        s->pending = true;
        bcm2835_fb_mbox_push(s, value);
        qemu_set_irq(s->mbox_irq, 1);
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset %"HWADDR_PRIx"\n",
                      __func__, offset);
        return;
    }
}

static const MemoryRegionOps bcm2835_fb_ops = {
    .read = bcm2835_fb_read,
    .write = bcm2835_fb_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static int bcm2835_fb_post_load(void *opaque, int version_id)
{
    BCM2835FBState *s = opaque;

    if (version_id < 4) {
        s->enabled = true;
    }
    if (version_id < 5) {
        s->config.layer = 0;
        s->config.transform = 0;
        s->config.vsync = 0;
    }
    if (version_id < 6) {
        s->cursor_info_valid = false;
        s->cursor_enabled = false;
        s->cursor_width = 0;
        s->cursor_height = 0;
        s->cursor_address = 0;
        s->cursor_hotspot_x = 0;
        s->cursor_hotspot_y = 0;
        s->cursor_x = 0;
        s->cursor_y = 0;
        s->cursor_flags = 0;
    } else if ((s->cursor_info_valid &&
                !bcm2835_fb_cursor_info_is_valid(
                    s->cursor_width, s->cursor_height,
                    s->cursor_address, s->cursor_hotspot_x,
                    s->cursor_hotspot_y)) ||
               s->cursor_flags & ~BIT(0)) {
        return -EINVAL;
    }
    bcm2835_fb_validate_config(&s->config);
    bcm2835_fb_resize_console(s);
    s->invalidate = true;
    return 0;
}

static const VMStateDescription vmstate_bcm2835_fb = {
    .name = TYPE_BCM2835_FB,
    .version_id = 6,
    .minimum_version_id = 1,
    .post_load = bcm2835_fb_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(lock, BCM2835FBState),
        VMSTATE_BOOL(invalidate, BCM2835FBState),
        VMSTATE_BOOL(pending, BCM2835FBState),
        VMSTATE_UINT32(config.xres, BCM2835FBState),
        VMSTATE_UINT32(config.yres, BCM2835FBState),
        VMSTATE_UINT32(config.xres_virtual, BCM2835FBState),
        VMSTATE_UINT32(config.yres_virtual, BCM2835FBState),
        VMSTATE_UINT32(config.xoffset, BCM2835FBState),
        VMSTATE_UINT32(config.yoffset, BCM2835FBState),
        VMSTATE_UINT32(config.bpp, BCM2835FBState),
        VMSTATE_UINT32(config.base, BCM2835FBState),
        VMSTATE_UNUSED(8), /* Was pitch and size */
        VMSTATE_UINT32(config.pixo, BCM2835FBState),
        VMSTATE_UINT32(config.alpha, BCM2835FBState),
        VMSTATE_BOOL_V(blank, BCM2835FBState, 2),
        VMSTATE_BOOL_V(enabled, BCM2835FBState, 4),
        VMSTATE_UINT32_V(config.overscan_top, BCM2835FBState, 3),
        VMSTATE_UINT32_V(config.overscan_bottom, BCM2835FBState, 3),
        VMSTATE_UINT32_V(config.overscan_left, BCM2835FBState, 3),
        VMSTATE_UINT32_V(config.overscan_right, BCM2835FBState, 3),
        VMSTATE_UINT32_V(config.layer, BCM2835FBState, 5),
        VMSTATE_UINT32_V(config.transform, BCM2835FBState, 5),
        VMSTATE_UINT32_V(config.vsync, BCM2835FBState, 5),
        VMSTATE_BOOL_V(cursor_info_valid, BCM2835FBState, 6),
        VMSTATE_BOOL_V(cursor_enabled, BCM2835FBState, 6),
        VMSTATE_UINT32_V(cursor_width, BCM2835FBState, 6),
        VMSTATE_UINT32_V(cursor_height, BCM2835FBState, 6),
        VMSTATE_UINT32_V(cursor_address, BCM2835FBState, 6),
        VMSTATE_UINT32_V(cursor_hotspot_x, BCM2835FBState, 6),
        VMSTATE_UINT32_V(cursor_hotspot_y, BCM2835FBState, 6),
        VMSTATE_UINT32_V(cursor_x, BCM2835FBState, 6),
        VMSTATE_UINT32_V(cursor_y, BCM2835FBState, 6),
        VMSTATE_UINT32_V(cursor_flags, BCM2835FBState, 6),
        VMSTATE_END_OF_LIST()
    }
};

static const GraphicHwOps vgafb_ops = {
    .invalidate  = fb_invalidate_display,
    .gfx_update  = fb_update_display,
};

static bool bcm2835_fb_get_blank(Object *obj, Error **errp)
{
    BCM2835FBState *s = BCM2835_FB(obj);

    return s->blank;
}

static bool bcm2835_fb_get_enabled(Object *obj, Error **errp)
{
    BCM2835FBState *s = BCM2835_FB(obj);

    return s->enabled;
}

static bool bcm2835_fb_get_cursor_info_valid(Object *obj, Error **errp)
{
    BCM2835FBState *s = BCM2835_FB(obj);

    return s->cursor_info_valid;
}

static bool bcm2835_fb_get_cursor_enabled(Object *obj, Error **errp)
{
    BCM2835FBState *s = BCM2835_FB(obj);

    return s->cursor_enabled;
}

static void bcm2835_fb_init(Object *obj)
{
    BCM2835FBState *s = BCM2835_FB(obj);

    memory_region_init_io(&s->iomem, obj, &bcm2835_fb_ops, s, TYPE_BCM2835_FB,
                          0x10);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->mbox_irq);
    object_property_add_bool(obj, "blank", bcm2835_fb_get_blank, NULL);
    object_property_add_bool(obj, "enabled", bcm2835_fb_get_enabled, NULL);
    object_property_add_bool(obj, "cursor-info-valid",
                             bcm2835_fb_get_cursor_info_valid, NULL);
    object_property_add_bool(obj, "cursor-enabled",
                             bcm2835_fb_get_cursor_enabled, NULL);
}

static void bcm2835_fb_reset(DeviceState *dev)
{
    BCM2835FBState *s = BCM2835_FB(dev);

    s->pending = false;
    s->blank = false;
    s->enabled = true;
    s->cursor_info_valid = false;
    s->cursor_enabled = false;
    s->cursor_width = 0;
    s->cursor_height = 0;
    s->cursor_address = 0;
    s->cursor_hotspot_x = 0;
    s->cursor_hotspot_y = 0;
    s->cursor_x = 0;
    s->cursor_y = 0;
    s->cursor_flags = 0;

    s->config = s->initial_config;

    s->invalidate = true;
    s->lock = false;
    bcm2835_fb_resize_console(s);
}

static void bcm2835_fb_realize(DeviceState *dev, Error **errp)
{
    BCM2835FBState *s = BCM2835_FB(dev);
    Object *obj;

    if (s->vcram_base == 0) {
        error_setg(errp, "%s: required vcram-base property not set", __func__);
        return;
    }

    obj = object_property_get_link(OBJECT(dev), "dma-mr", &error_abort);

    /* Fill in the parts of initial_config that are not set by QOM properties */
    s->initial_config.xres_virtual = s->initial_config.xres;
    s->initial_config.yres_virtual = s->initial_config.yres;
    s->initial_config.xoffset = 0;
    s->initial_config.yoffset = 0;
    s->initial_config.base = s->vcram_base + BCM2835_FB_OFFSET;

    s->dma_mr = MEMORY_REGION(obj);
    address_space_init(&s->dma_as, s->dma_mr, TYPE_BCM2835_FB "-memory");

    bcm2835_fb_reset(dev);

    s->con = qemu_graphic_console_create(dev, 0, &vgafb_ops, s);
    bcm2835_fb_resize_console(s);
}

static const Property bcm2835_fb_props[] = {
    DEFINE_PROP_UINT32("vcram-base", BCM2835FBState, vcram_base, 0),/*required*/
    DEFINE_PROP_UINT32("vcram-size", BCM2835FBState, vcram_size,
                       DEFAULT_VCRAM_SIZE),
    DEFINE_PROP_UINT32("xres", BCM2835FBState, initial_config.xres, 640),
    DEFINE_PROP_UINT32("yres", BCM2835FBState, initial_config.yres, 480),
    DEFINE_PROP_UINT32("bpp", BCM2835FBState, initial_config.bpp, 16),
    DEFINE_PROP_UINT32("pixo", BCM2835FBState,
                       initial_config.pixo, 1), /* 1=RGB, 0=BGR */
    DEFINE_PROP_UINT32("alpha", BCM2835FBState,
                       initial_config.alpha, 2), /* alpha ignored */
};

static void bcm2835_fb_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, bcm2835_fb_props);
    dc->realize = bcm2835_fb_realize;
    device_class_set_legacy_reset(dc, bcm2835_fb_reset);
    dc->vmsd = &vmstate_bcm2835_fb;
}

static const TypeInfo bcm2835_fb_info = {
    .name          = TYPE_BCM2835_FB,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2835FBState),
    .class_init    = bcm2835_fb_class_init,
    .instance_init = bcm2835_fb_init,
};

static void bcm2835_fb_register_types(void)
{
    type_register_static(&bcm2835_fb_info);
}

type_init(bcm2835_fb_register_types)
