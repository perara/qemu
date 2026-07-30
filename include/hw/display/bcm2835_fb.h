/*
 * Raspberry Pi emulation (c) 2012 Gregory Estrade
 * Upstreaming code cleanup [including bcm2835_*] (c) 2013 Jan Petrous
 *
 * Rasperry Pi 2 emulation and refactoring Copyright (c) 2015, Microsoft
 * Written by Andrew Baumann
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef BCM2835_FB_H
#define BCM2835_FB_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define UPPER_RAM_BASE 0x40000000

#define TYPE_BCM2835_FB "bcm2835-fb"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2835FBState, BCM2835_FB)

/*
 * Configuration information about the fb which the guest can program
 * via the mailbox property interface.
 */
typedef struct {
    uint32_t xres, yres;
    uint32_t xres_virtual, yres_virtual;
    uint32_t xoffset, yoffset;
    uint32_t bpp;
    uint32_t base;
    uint32_t pixo;
    uint32_t alpha;
    uint32_t overscan_top;
    uint32_t overscan_bottom;
    uint32_t overscan_left;
    uint32_t overscan_right;
    uint32_t layer;
    uint32_t transform;
    uint32_t vsync;
} BCM2835FBConfig;

struct BCM2835FBState {
    /*< private >*/
    SysBusDevice busdev;
    /*< public >*/

    uint32_t vcram_base, vcram_size;
    MemoryRegion *dma_mr;
    AddressSpace dma_as;
    MemoryRegion iomem;
    MemoryRegionSection fbsection;
    QemuConsole *con;
    qemu_irq mbox_irq;

    bool lock, invalidate, pending, blank, enabled;
    bool cursor_info_valid, cursor_enabled;
    uint32_t cursor_width, cursor_height;
    uint32_t cursor_address;
    uint32_t cursor_hotspot_x, cursor_hotspot_y;
    uint32_t cursor_x, cursor_y, cursor_flags;

    BCM2835FBConfig config;
    BCM2835FBConfig initial_config;
};

void bcm2835_fb_reconfigure(BCM2835FBState *s, BCM2835FBConfig *newconfig);
void bcm2835_fb_set_blank(BCM2835FBState *s, bool blank);
void bcm2835_fb_set_enabled(BCM2835FBState *s, bool enabled);
bool bcm2835_fb_set_cursor_info(BCM2835FBState *s, uint32_t width,
                                uint32_t height, uint32_t address,
                                uint32_t hotspot_x, uint32_t hotspot_y);
bool bcm2835_fb_set_cursor_state(BCM2835FBState *s, uint32_t enable,
                                 uint32_t x, uint32_t y, uint32_t flags);
bool bcm2835_fb_set_overscan(BCM2835FBConfig *config, uint32_t top,
                             uint32_t bottom, uint32_t left,
                             uint32_t right);

/**
 * bcm2835_fb_get_pitch: return number of bytes per line of the framebuffer
 * @config: configuration info for the framebuffer
 *
 * Return the number of bytes per line of the framebuffer, ie the number
 * that must be added to a pixel address to get the address of the pixel
 * directly below it on screen.
 */
static inline uint32_t bcm2835_fb_get_pitch(BCM2835FBConfig *config)
{
    uint32_t xres = MAX(config->xres, config->xres_virtual);
    return xres * (config->bpp >> 3);
}

/**
 * bcm2835_fb_get_size: return total size of framebuffer in bytes
 * @config: configuration info for the framebuffer
 */
static inline uint32_t bcm2835_fb_get_size(BCM2835FBConfig *config)
{
    uint32_t yres = MAX(config->yres, config->yres_virtual);
    return yres * bcm2835_fb_get_pitch(config);
}

/**
 * bcm2835_fb_validate_config: check provided config
 *
 * Validates the configuration information provided by the guest and
 * adjusts it if necessary.
 */
void bcm2835_fb_validate_config(BCM2835FBConfig *config);

#endif
