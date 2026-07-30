/*
 * Raspberry Pi emulation (c) 2012 Gregory Estrade
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef BCM2835_PROPERTY_H
#define BCM2835_PROPERTY_H

#include "hw/core/sysbus.h"
#include "net/net.h"
#include "hw/display/bcm2835_fb.h"
#include "hw/misc/bcm2835_cprman.h"
#include "hw/misc/bcm2835_thermal.h"
#include "hw/nvram/bcm2835_otp.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_BCM2835_PROPERTY "bcm2835-property"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2835PropertyState, BCM2835_PROPERTY)

#define BCM2835_PROPERTY_EXP_GPIO_COUNT 8
#define BCM2835_PROPERTY_POWER_DEVICE_COUNT 11
#define BCM2835_PROPERTY_EDID_PORT_COUNT 2
#define BCM2835_PROPERTY_DISPLAY_COUNT 2
#define BCM2835_PROPERTY_DISPLAY_POWER_DEFAULT_MASK \
    ((1U << BCM2835_PROPERTY_DISPLAY_COUNT) - 1)
#define BCM2835_PROPERTY_DISPLAY_TIMING_SIZE 36
#define BCM2835_PROPERTY_EDID_BLOCK_SIZE 128
#define BCM2835_PROPERTY_EDID_MAX_BLOCKS 256
#define BCM2835_PROPERTY_EDID_MAX_SIZE \
    (BCM2835_PROPERTY_EDID_BLOCK_SIZE * BCM2835_PROPERTY_EDID_MAX_BLOCKS)
#define BCM2835_PROPERTY_POWER_DEFAULT_MASK \
    ((1U << BCM2835_PROPERTY_POWER_DEVICE_COUNT) - 1)
#define BCM2835_PROPERTY_THROTTLED_CURRENT_MASK 0x7
#define BCM2835_PROPERTY_THROTTLED_HISTORY_SHIFT 16
#define BCM2835_PROPERTY_THROTTLED_HISTORY_MASK \
    (BCM2835_PROPERTY_THROTTLED_CURRENT_MASK << \
     BCM2835_PROPERTY_THROTTLED_HISTORY_SHIFT)

struct BCM2835PropertyState {
    /*< private >*/
    SysBusDevice busdev;
    /*< public >*/

    MemoryRegion *dma_mr;
    AddressSpace dma_as;
    MemoryRegion iomem;
    qemu_irq mbox_irq;
    BCM2835FBState *fbdev;
    BCM2835OTPState *otp;
    BCM2835CprmanState *cprman;
    Bcm2835ThermalState *thermal;
    DeviceState *xhci;

    MACAddr macaddr;
    uint32_t board_rev;
    uint32_t addr;
    char *command_line;
    bool pending;
    bool vsync_wait;
    QEMUTimer *vsync_timer;
    uint32_t reboot_flags;
    uint32_t power_state;
    uint32_t xhci_reset_count;
    uint32_t throttled_current;
    uint32_t throttled_history;
    uint32_t framebuffer_display_num;
    uint32_t display_power;
    uint8_t display_timing[BCM2835_PROPERTY_DISPLAY_COUNT]
                          [BCM2835_PROPERTY_DISPLAY_TIMING_SIZE];
    uint32_t edid_size[BCM2835_PROPERTY_EDID_PORT_COUNT];
    uint8_t edid[BCM2835_PROPERTY_EDID_PORT_COUNT]
                [BCM2835_PROPERTY_EDID_MAX_SIZE];

    uint32_t exp_gpio_direction[BCM2835_PROPERTY_EXP_GPIO_COUNT];
    uint32_t exp_gpio_polarity[BCM2835_PROPERTY_EXP_GPIO_COUNT];
    uint32_t exp_gpio_term_enable[BCM2835_PROPERTY_EXP_GPIO_COUNT];
    uint32_t exp_gpio_term_pull_up[BCM2835_PROPERTY_EXP_GPIO_COUNT];
    uint32_t exp_gpio_state[BCM2835_PROPERTY_EXP_GPIO_COUNT];
    qemu_irq exp_gpio_out[BCM2835_PROPERTY_EXP_GPIO_COUNT];
};

void bcm2835_property_set_mac(BCM2835PropertyState *s,
                              const MACAddr *mac);
uint32_t bcm2835_property_get_throttled(const BCM2835PropertyState *s);
void bcm2835_property_set_throttled_current(BCM2835PropertyState *s,
                                            uint32_t current);
void bcm2835_property_set_thermal(BCM2835PropertyState *s,
                                  Bcm2835ThermalState *thermal);
void bcm2835_property_set_cprman(BCM2835PropertyState *s,
                                 BCM2835CprmanState *cprman);
void bcm2835_property_set_xhci(BCM2835PropertyState *s,
                              DeviceState *xhci);
void bcm2835_property_set_edid(BCM2835PropertyState *s, unsigned int port,
                               const uint8_t *edid, size_t size);
size_t bcm2835_property_get_edid(const BCM2835PropertyState *s,
                                 unsigned int port, const uint8_t **edid);

#endif
