/*
 * BCM2835 Power Management emulation
 *
 * Copyright (C) 2017 Marcin Chojnacki <marcinch7@gmail.com>
 * Copyright (C) 2021 Nolan Leake <nolan@sigbus.net>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef BCM2835_POWERMGT_H
#define BCM2835_POWERMGT_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_BCM2835_POWERMGT "bcm2835-powermgt"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2835PowerMgtState, BCM2835_POWERMGT)

typedef enum BCM2835HaltState {
    BCM2835_HALT_RUNNING,
    BCM2835_HALT_SUSPENDED_GPIO,
    BCM2835_HALT_SUSPENDED_GLOBAL_EN,
    BCM2835_HALT_POWERED_OFF,
    BCM2835_HALT_STATE__MAX,
} BCM2835HaltState;

struct BCM2835PowerMgtState {
    SysBusDevice busdev;
    MemoryRegion iomem;

    uint32_t rstc;
    uint32_t rsts;
    uint32_t wdog;
    QEMUTimer *watchdog_timer;
    bool watchdog_pending;
    uint64_t watchdog_remaining_ns;
    bool wake_on_gpio;
    bool power_off_on_halt;
    uint8_t halt_state;
    int8_t gpio3_level;
    bool global_en;
};

void bcm2835_powermgt_watchdog_start(BCM2835PowerMgtState *s,
                                     uint32_t milliseconds);
void bcm2835_powermgt_watchdog_trigger(BCM2835PowerMgtState *s);
void bcm2835_powermgt_set_halt_policy(BCM2835PowerMgtState *s,
                                      bool wake_on_gpio,
                                      bool power_off_on_halt);
void bcm2835_powermgt_gpio3_input(BCM2835PowerMgtState *s, int level);
void bcm2835_powermgt_global_en_input(BCM2835PowerMgtState *s, bool level);
const char *bcm2835_powermgt_halt_state(BCM2835PowerMgtState *s);

#endif
