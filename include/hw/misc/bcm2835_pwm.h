/*
 * BCM2835 pulse-width modulator
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_BCM2835_PWM_H
#define HW_MISC_BCM2835_PWM_H

#include "hw/core/clock.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/timer.h"

#define TYPE_BCM2835_PWM "bcm2835-pwm"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2835PwmState, BCM2835_PWM)

#define BCM2835_PWM_FIFO_DEPTH 16

struct BCM2835PwmState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq channel_enabled[2];
    qemu_irq waveform[2];
    qemu_irq dma_threshold[2];
    Clock *clk;
    QEMUTimer *timer[2];
    QEMUTimer *edge_timer[2];
    uint64_t remaining_cycles[2];
    uint64_t edge_remaining_cycles[2];
    uint32_t active_range[2];
    uint32_t active_data[2];
    uint32_t wave_position[2];
    uint32_t wave_target[2];
    bool transmitting[2];
    bool waveform_level[2];
    bool fifo_waiting[2];
    uint8_t fifo_next_channel;

    uint32_t ctl;
    uint32_t sta_sticky;
    uint32_t dmac;
    uint32_t range[2];
    uint32_t data[2];
    uint32_t fifo[BCM2835_PWM_FIFO_DEPTH];
    uint8_t fifo_head;
    uint8_t fifo_count;
};

#endif
