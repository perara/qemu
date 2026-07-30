/*
 * Raspberry Pi (BCM2838) GPIO Controller
 * This implementation is based on bcm2835_gpio (hw/gpio/bcm2835_gpio.c)
 *
 * Copyright (c) 2022 Auriga LLC
 *
 * Authors:
 *  Lotosh, Aleksey <aleksey.lotosh@auriga.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef BCM2838_GPIO_H
#define BCM2838_GPIO_H

#include "hw/sd/sd.h"
#include "hw/core/sysbus.h"
#include "hw/misc/bcm2835_powermgt.h"
#include "chardev/char-fe.h"
#include "qom/object.h"

#define TYPE_BCM2838_GPIO "bcm2838-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2838GpioState, BCM2838_GPIO)

#define BCM2838_GPIO_REGS_SIZE 0x1000
#define BCM2838_GPIO_NUM       58
#define BCM2838_GPIO_BANKS     2
#define BCM2838_GPIO_IRQS      4
#define GPIO_PUP_PDN_CNTRL_NUM 4
#define BCM2838_GPIO_BRIDGE_LINE_SIZE 96

struct BCM2838GpioState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;

    /* SDBus selector */
    SDBus sdbus;
    SDBus *sdbus_sdhci;
    SDBus *sdbus_sdhost;
    BCM2835PowerMgtState *powermgt;

    uint8_t fsel[BCM2838_GPIO_NUM];
    /* GPIO output latch and external input value/drive-valid mask. */
    uint32_t lev0, lev1;
    uint32_t input0, input1;
    uint32_t input_mask0, input_mask1;
    uint32_t event_status[BCM2838_GPIO_BANKS];
    uint32_t rising_enable[BCM2838_GPIO_BANKS];
    uint32_t falling_enable[BCM2838_GPIO_BANKS];
    uint32_t high_enable[BCM2838_GPIO_BANKS];
    uint32_t low_enable[BCM2838_GPIO_BANKS];
    uint32_t async_rising_enable[BCM2838_GPIO_BANKS];
    uint32_t async_falling_enable[BCM2838_GPIO_BANKS];
    uint8_t sd_fsel;
    qemu_irq out[BCM2838_GPIO_NUM];
    qemu_irq out_enable[BCM2838_GPIO_NUM];
    qemu_irq irq[BCM2838_GPIO_IRQS];
    bool pwm_level[2];
    bool pwm1_level[2];
    uint32_t pup_cntrl_reg[GPIO_PUP_PDN_CNTRL_NUM];
    CharFrontend bridge_chr;
    char bridge_line[BCM2838_GPIO_BRIDGE_LINE_SIZE];
    uint8_t bridge_line_len;
    bool bridge_discard_line;
    bool bridge_open;
    int8_t bridge_eeprom_nwp;
    int8_t bridge_sd_overcurrent;
    bool hat_map_applied;
    bool hat_map_configured;
    uint8_t hat_map[30];
    uint8_t hat_drive;
    uint8_t hat_slew;
    uint8_t hat_hysteresis;
    uint8_t hat_back_power;
    uint32_t hat_used_mask;
};

/* Return -1 for high impedance, otherwise the externally driven level. */
int bcm2838_gpio_get_external_input(BCM2838GpioState *s,
                                    unsigned int index);
/* Return -1 for high impedance, otherwise the fixture-driven EEPROM_nWP. */
int bcm2838_gpio_get_eeprom_nwp(BCM2838GpioState *s);
int bcm2838_gpio_get_sd_overcurrent(BCM2838GpioState *s);
void bcm2838_gpio_apply_hat_map(BCM2838GpioState *s,
                                const uint8_t map[30]);
bool bcm2838_gpio_hat_map_applied(BCM2838GpioState *s);
uint32_t bcm2838_gpio_hat_used_mask(BCM2838GpioState *s);
uint8_t bcm2838_gpio_hat_drive(BCM2838GpioState *s);
uint8_t bcm2838_gpio_hat_slew(BCM2838GpioState *s);
uint8_t bcm2838_gpio_hat_hysteresis(BCM2838GpioState *s);
uint8_t bcm2838_gpio_hat_back_power(BCM2838GpioState *s);

#endif
