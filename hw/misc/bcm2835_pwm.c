/*
 * BCM2835 pulse-width modulator
 *
 * This models the software-visible two-channel control block, shared FIFO,
 * CPRMAN-clocked transmitter, and logical output bitstreams.  Output timing
 * uses QEMU virtual time; electrical edge shape remains a board/HIL concern.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/misc/bcm2835_pwm.h"
#include "migration/vmstate.h"

#define PWM_SIZE 0x28

#define PWM_CTL   0x00
#define PWM_STA   0x04
#define PWM_DMAC  0x08
#define PWM_RNG1  0x10
#define PWM_DAT1  0x14
#define PWM_FIF1  0x18
#define PWM_RNG2  0x20
#define PWM_DAT2  0x24

#define PWM_CTL_PWEN1 BIT(0)
#define PWM_CTL_MODE1 BIT(1)
#define PWM_CTL_RPTL1 BIT(2)
#define PWM_CTL_SBIT1 BIT(3)
#define PWM_CTL_POLA1 BIT(4)
#define PWM_CTL_USEF1 BIT(5)
#define PWM_CTL_CLRF1 BIT(6)
#define PWM_CTL_MSEN1 BIT(7)
#define PWM_CTL_PWEN2 BIT(8)
#define PWM_CTL_MODE2 BIT(9)
#define PWM_CTL_RPTL2 BIT(10)
#define PWM_CTL_SBIT2 BIT(11)
#define PWM_CTL_POLA2 BIT(12)
#define PWM_CTL_USEF2 BIT(13)
#define PWM_CTL_MSEN2 BIT(15)
#define PWM_CTL_MASK  0xffff
#define PWM_DMAC_DREQ_MASK 0xff
#define PWM_DMAC_PANIC_SHIFT 8
#define PWM_DMAC_PANIC_MASK (0xff << PWM_DMAC_PANIC_SHIFT)
#define PWM_DMAC_ENABLE BIT(31)
#define PWM_DMAC_MASK (BIT(31) | 0xffff)

#define PWM_STA_FULL1 BIT(0)
#define PWM_STA_EMPT1 BIT(1)
#define PWM_STA_WERR1 BIT(2)
#define PWM_STA_RERR1 BIT(3)
#define PWM_STA_GAPO_MASK (0xf << 4)
#define PWM_STA_BERR  BIT(8)
#define PWM_STA_STA1  BIT(9)
#define PWM_STA_STA2  BIT(10)
#define PWM_STA_W1C_MASK \
    (PWM_STA_WERR1 | PWM_STA_RERR1 | PWM_STA_GAPO_MASK | PWM_STA_BERR)

static void bcm2835_pwm_update_outputs(BCM2835PwmState *s)
{
    qemu_set_irq(s->channel_enabled[0], !!(s->ctl & PWM_CTL_PWEN1));
    qemu_set_irq(s->channel_enabled[1], !!(s->ctl & PWM_CTL_PWEN2));
    qemu_set_irq(
        s->dma_threshold[0],
        (s->dmac & PWM_DMAC_ENABLE) &&
        s->fifo_count <= (s->dmac & PWM_DMAC_DREQ_MASK));
    qemu_set_irq(
        s->dma_threshold[1],
        (s->dmac & PWM_DMAC_ENABLE) &&
        s->fifo_count <=
        ((s->dmac & PWM_DMAC_PANIC_MASK) >> PWM_DMAC_PANIC_SHIFT));
}

static uint32_t bcm2835_pwm_ctl_bit(unsigned channel,
                                    uint32_t channel1,
                                    uint32_t channel2)
{
    return channel ? channel2 : channel1;
}

static bool bcm2835_pwm_channel_enabled(BCM2835PwmState *s,
                                        unsigned channel)
{
    uint32_t enable = bcm2835_pwm_ctl_bit(
        channel, PWM_CTL_PWEN1, PWM_CTL_PWEN2);

    return (s->ctl & enable) && s->range[channel];
}

static bool bcm2835_pwm_channel_fifo_enabled(BCM2835PwmState *s,
                                             unsigned channel)
{
    uint32_t use_fifo = bcm2835_pwm_ctl_bit(
        channel, PWM_CTL_USEF1, PWM_CTL_USEF2);

    return bcm2835_pwm_channel_enabled(s, channel) &&
           (s->ctl & use_fifo);
}

static bool bcm2835_pwm_fifo_shared(BCM2835PwmState *s)
{
    return bcm2835_pwm_channel_fifo_enabled(s, 0) &&
           bcm2835_pwm_channel_fifo_enabled(s, 1);
}

static bool bcm2835_pwm_ctl_enabled(BCM2835PwmState *s, unsigned channel,
                                    uint32_t channel1, uint32_t channel2)
{
    return s->ctl & bcm2835_pwm_ctl_bit(channel, channel1, channel2);
}

static bool bcm2835_pwm_raw_level(BCM2835PwmState *s, unsigned channel,
                                  uint32_t position)
{
    uint32_t range = s->active_range[channel];
    uint32_t data = s->active_data[channel];

    if (bcm2835_pwm_ctl_enabled(s, channel,
                                PWM_CTL_MODE1, PWM_CTL_MODE2)) {
        if (position < 32) {
            return extract32(data, 31 - position, 1);
        }
        return bcm2835_pwm_ctl_enabled(s, channel,
                                       PWM_CTL_SBIT1, PWM_CTL_SBIT2);
    }
    data = MIN(data, range);
    if (bcm2835_pwm_ctl_enabled(s, channel,
                                PWM_CTL_MSEN1, PWM_CTL_MSEN2)) {
        return position < data;
    }
    if (!data || data == range) {
        return data != 0;
    }

    return (((uint64_t)position * data) % range) + data >= range;
}

static void bcm2835_pwm_set_waveform(BCM2835PwmState *s,
                                     unsigned channel, bool raw_level)
{
    bool polarity = bcm2835_pwm_ctl_enabled(
        s, channel, PWM_CTL_POLA1, PWM_CTL_POLA2);
    bool level = raw_level ^ polarity;

    s->waveform_level[channel] = level;
    qemu_set_irq(s->waveform[channel], level);
}

static void bcm2835_pwm_set_idle(BCM2835PwmState *s, unsigned channel)
{
    bool idle = bcm2835_pwm_ctl_enabled(
        s, channel, PWM_CTL_SBIT1, PWM_CTL_SBIT2);

    s->transmitting[channel] = false;
    timer_del(s->edge_timer[channel]);
    s->edge_remaining_cycles[channel] = 0;
    bcm2835_pwm_set_waveform(s, channel, idle);
}

static uint32_t bcm2835_pwm_next_transition(BCM2835PwmState *s,
                                            unsigned channel)
{
    uint32_t position = s->wave_position[channel];
    uint32_t range = s->active_range[channel];
    uint32_t data = s->active_data[channel];
    bool level = bcm2835_pwm_raw_level(s, channel, position);
    uint64_t target;

    if (bcm2835_pwm_ctl_enabled(s, channel,
                                PWM_CTL_MODE1, PWM_CTL_MODE2)) {
        uint32_t limit = MIN(range, 33U);

        for (target = position + 1; target < limit; target++) {
            if (bcm2835_pwm_raw_level(s, channel, target) != level) {
                return target;
            }
        }
        return range;
    }

    data = MIN(data, range);
    if (!data || data == range) {
        return range;
    }
    if (bcm2835_pwm_ctl_enabled(s, channel,
                                PWM_CTL_MSEN1, PWM_CTL_MSEN2)) {
        return level ? data : range;
    }

    {
        uint64_t accumulator =
            (((uint64_t)position + 1) * data) % range;
        uint64_t span;

        if (level) {
            span = 1 + accumulator / (range - data);
        } else {
            span = 1 + (range - 1 - accumulator) / data;
        }
        target = (uint64_t)position + span;
    }
    return MIN(target, range);
}

static void bcm2835_pwm_schedule_edge(BCM2835PwmState *s,
                                      unsigned channel)
{
    uint64_t cycles;
    uint64_t delay;

    if (!s->transmitting[channel] ||
        s->wave_target[channel] >= s->active_range[channel] ||
        timer_pending(s->edge_timer[channel])) {
        return;
    }
    cycles = s->edge_remaining_cycles[channel] ?:
             s->wave_target[channel] - s->wave_position[channel];
    if (!clock_get_hz(s->clk)) {
        s->edge_remaining_cycles[channel] = cycles;
        return;
    }
    delay = MAX(1ULL, clock_ticks_to_ns(s->clk, cycles));
    timer_mod_ns(s->edge_timer[channel],
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay);
    s->edge_remaining_cycles[channel] = 0;
}

static void bcm2835_pwm_begin_waveform(BCM2835PwmState *s,
                                       unsigned channel)
{
    s->active_range[channel] = s->range[channel];
    s->active_data[channel] = s->data[channel];
    s->wave_position[channel] = 0;
    s->transmitting[channel] = true;
    bcm2835_pwm_set_waveform(
        s, channel, bcm2835_pwm_raw_level(s, channel, 0));
    s->wave_target[channel] =
        bcm2835_pwm_next_transition(s, channel);
    bcm2835_pwm_schedule_edge(s, channel);
}

static void bcm2835_pwm_schedule(BCM2835PwmState *s, unsigned channel)
{
    uint64_t cycles;
    uint64_t delay;

    if (!bcm2835_pwm_channel_enabled(s, channel) ||
        !s->transmitting[channel] ||
        timer_pending(s->timer[channel])) {
        return;
    }
    cycles = s->remaining_cycles[channel] ?:
             (uint64_t)s->active_range[channel];
    if (!clock_get_hz(s->clk)) {
        s->remaining_cycles[channel] = cycles;
        return;
    }
    delay = MAX(1ULL, clock_ticks_to_ns(s->clk, cycles));
    timer_mod_ns(s->timer[channel],
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay);
    s->remaining_cycles[channel] = 0;
}

static void bcm2835_pwm_cancel_channel(BCM2835PwmState *s,
                                       unsigned channel)
{
    timer_del(s->timer[channel]);
    s->remaining_cycles[channel] = 0;
    s->fifo_waiting[channel] = false;
    bcm2835_pwm_set_idle(s, channel);
}

static uint64_t bcm2835_pwm_timer_remaining_cycles(BCM2835PwmState *s,
                                                   QEMUTimer *timer)
{
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t expiry = timer_expire_time_ns(timer);
    uint64_t remaining_ns = expiry > now ? expiry - now : 1;
    uint64_t cycles = clock_ns_to_ticks(s->clk, remaining_ns);

    if (clock_ticks_to_ns(s->clk, cycles) < remaining_ns) {
        cycles++;
    }
    return MAX(1ULL, cycles);
}

static void bcm2835_pwm_clock_update(void *opaque, ClockEvent event)
{
    BCM2835PwmState *s = opaque;

    for (unsigned channel = 0; channel < 2; channel++) {
        if (event == ClockPreUpdate && timer_pending(s->timer[channel])) {
            s->remaining_cycles[channel] =
                bcm2835_pwm_timer_remaining_cycles(s, s->timer[channel]);
            timer_del(s->timer[channel]);
        }
        if (event == ClockPreUpdate &&
            timer_pending(s->edge_timer[channel])) {
            s->edge_remaining_cycles[channel] =
                bcm2835_pwm_timer_remaining_cycles(
                    s, s->edge_timer[channel]);
            timer_del(s->edge_timer[channel]);
        } else if (event == ClockUpdate) {
            bcm2835_pwm_schedule(s, channel);
            bcm2835_pwm_schedule_edge(s, channel);
        }
    }
}

static void bcm2835_pwm_clock_get(Object *obj, Visitor *v,
                                  const char *name, void *opaque,
                                  Error **errp)
{
    BCM2835PwmState *s = BCM2835_PWM(obj);
    uint32_t frequency = clock_get_hz(s->clk);

    visit_type_uint32(v, name, &frequency, errp);
}

static uint32_t bcm2835_pwm_status(BCM2835PwmState *s)
{
    uint32_t status = s->sta_sticky;

    if (s->fifo_count == BCM2835_PWM_FIFO_DEPTH) {
        status |= PWM_STA_FULL1;
    }
    if (s->fifo_count == 0) {
        status |= PWM_STA_EMPT1;
    }
    if (s->ctl & PWM_CTL_PWEN1) {
        status |= PWM_STA_STA1;
    }
    if (s->ctl & PWM_CTL_PWEN2) {
        status |= PWM_STA_STA2;
    }
    return status;
}

static void bcm2835_pwm_fifo_clear(BCM2835PwmState *s)
{
    memset(s->fifo, 0, sizeof(s->fifo));
    s->fifo_head = 0;
    s->fifo_count = 0;
    s->fifo_next_channel = 0;
    bcm2835_pwm_update_outputs(s);
}

static uint32_t bcm2835_pwm_fifo_take(BCM2835PwmState *s)
{
    uint32_t value;

    assert(s->fifo_count);
    value = s->fifo[s->fifo_head];
    s->fifo[s->fifo_head] = 0;
    s->fifo_head = (s->fifo_head + 1) % BCM2835_PWM_FIFO_DEPTH;
    s->fifo_count--;
    bcm2835_pwm_update_outputs(s);
    return value;
}

static uint32_t bcm2835_pwm_fifo_pop(BCM2835PwmState *s)
{
    if (!s->fifo_count) {
        s->sta_sticky |= PWM_STA_RERR1;
        return 0;
    }
    return bcm2835_pwm_fifo_take(s);
}

static void bcm2835_pwm_fifo_dispatch_shared(BCM2835PwmState *s)
{
    unsigned request;

    if (!bcm2835_pwm_fifo_shared(s) ||
        !s->fifo_waiting[0] || !s->fifo_waiting[1]) {
        return;
    }

    /*
     * Both state machines request at the same boundary.  Preserve the FIFO
     * turn across starvation: A/C/E go to channel 0 and B/D/F to channel 1.
     */
    for (request = 0; request < 2 && s->fifo_count; request++) {
        unsigned channel = s->fifo_next_channel;

        if (!s->fifo_waiting[channel]) {
            channel ^= 1;
            if (!s->fifo_waiting[channel]) {
                break;
            }
        }
        s->data[channel] = bcm2835_pwm_fifo_take(s);
        s->fifo_waiting[channel] = false;
        s->fifo_next_channel = channel ^ 1;
        bcm2835_pwm_begin_waveform(s, channel);
        bcm2835_pwm_schedule(s, channel);
    }

    for (unsigned channel = 0; channel < 2; channel++) {
        if (s->fifo_waiting[channel]) {
            s->sta_sticky |= BIT(4 + channel);
            bcm2835_pwm_set_idle(s, channel);
        }
    }
    bcm2835_pwm_update_outputs(s);
}

static void bcm2835_pwm_fifo_push(BCM2835PwmState *s, uint32_t value)
{
    unsigned tail;

    if (s->fifo_count == BCM2835_PWM_FIFO_DEPTH) {
        s->sta_sticky |= PWM_STA_WERR1;
        return;
    }

    tail = (s->fifo_head + s->fifo_count) % BCM2835_PWM_FIFO_DEPTH;
    s->fifo[tail] = value;
    s->fifo_count++;
    bcm2835_pwm_update_outputs(s);
    if (bcm2835_pwm_fifo_shared(s)) {
        bcm2835_pwm_fifo_dispatch_shared(s);
        return;
    }
    for (unsigned channel = 0; channel < 2; channel++) {
        if (bcm2835_pwm_channel_fifo_enabled(s, channel) &&
            !s->transmitting[channel] && s->fifo_count) {
            s->data[channel] = bcm2835_pwm_fifo_take(s);
            bcm2835_pwm_begin_waveform(s, channel);
            bcm2835_pwm_schedule(s, channel);
        }
    }
}

static void bcm2835_pwm_consume(BCM2835PwmState *s, unsigned channel)
{
    if (!bcm2835_pwm_channel_enabled(s, channel)) {
        bcm2835_pwm_set_idle(s, channel);
        return;
    }
    if (bcm2835_pwm_channel_fifo_enabled(s, channel)) {
        if (bcm2835_pwm_fifo_shared(s)) {
            bcm2835_pwm_set_idle(s, channel);
            s->fifo_waiting[channel] = true;
            bcm2835_pwm_fifo_dispatch_shared(s);
            return;
        }
        if (s->fifo_count) {
            s->data[channel] = bcm2835_pwm_fifo_take(s);
        } else if (!bcm2835_pwm_ctl_enabled(
                       s, channel, PWM_CTL_RPTL1, PWM_CTL_RPTL2)) {
            s->sta_sticky |= BIT(4 + channel);
            bcm2835_pwm_update_outputs(s);
            bcm2835_pwm_set_idle(s, channel);
            return;
        }
    }
    bcm2835_pwm_update_outputs(s);
    bcm2835_pwm_begin_waveform(s, channel);
    bcm2835_pwm_schedule(s, channel);
}

static void bcm2835_pwm_timer0(void *opaque)
{
    bcm2835_pwm_consume(opaque, 0);
}

static void bcm2835_pwm_timer1(void *opaque)
{
    bcm2835_pwm_consume(opaque, 1);
}

static void bcm2835_pwm_edge(BCM2835PwmState *s, unsigned channel)
{
    if (!s->transmitting[channel] ||
        s->wave_target[channel] >= s->active_range[channel]) {
        return;
    }
    s->wave_position[channel] = s->wave_target[channel];
    bcm2835_pwm_set_waveform(
        s, channel,
        bcm2835_pwm_raw_level(s, channel, s->wave_position[channel]));
    s->wave_target[channel] =
        bcm2835_pwm_next_transition(s, channel);
    bcm2835_pwm_schedule_edge(s, channel);
}

static void bcm2835_pwm_edge0(void *opaque)
{
    bcm2835_pwm_edge(opaque, 0);
}

static void bcm2835_pwm_edge1(void *opaque)
{
    bcm2835_pwm_edge(opaque, 1);
}

static void bcm2835_pwm_start_channel(BCM2835PwmState *s,
                                      unsigned channel)
{
    if (!bcm2835_pwm_channel_enabled(s, channel) ||
        s->transmitting[channel]) {
        return;
    }
    if (bcm2835_pwm_channel_fifo_enabled(s, channel)) {
        if (bcm2835_pwm_fifo_shared(s)) {
            s->fifo_waiting[channel] = true;
            bcm2835_pwm_set_idle(s, channel);
            bcm2835_pwm_fifo_dispatch_shared(s);
            return;
        }
        s->fifo_waiting[channel] = false;
        if (!s->fifo_count) {
            bcm2835_pwm_set_idle(s, channel);
            return;
        }
        s->data[channel] = bcm2835_pwm_fifo_take(s);
    } else {
        s->fifo_waiting[channel] = false;
    }
    bcm2835_pwm_begin_waveform(s, channel);
    bcm2835_pwm_schedule(s, channel);
}

static uint64_t bcm2835_pwm_read(void *opaque, hwaddr offset, unsigned size)
{
    BCM2835PwmState *s = BCM2835_PWM(opaque);

    switch (offset) {
    case PWM_CTL:
        return s->ctl;
    case PWM_STA:
        return bcm2835_pwm_status(s);
    case PWM_DMAC:
        return s->dmac;
    case PWM_RNG1:
        return s->range[0];
    case PWM_DAT1:
        return s->data[0];
    case PWM_FIF1:
        return bcm2835_pwm_fifo_pop(s);
    case PWM_RNG2:
        return s->range[1];
    case PWM_DAT2:
        return s->data[1];
    default:
        return 0;
    }
}

static void bcm2835_pwm_write(void *opaque, hwaddr offset,
                              uint64_t value, unsigned size)
{
    BCM2835PwmState *s = BCM2835_PWM(opaque);
    bool old_fifo_shared;

    switch (offset) {
    case PWM_CTL:
        old_fifo_shared = bcm2835_pwm_fifo_shared(s);
        if (value & PWM_CTL_CLRF1) {
            bcm2835_pwm_fifo_clear(s);
        }
        s->ctl = value & (PWM_CTL_MASK & ~PWM_CTL_CLRF1);
        if (old_fifo_shared != bcm2835_pwm_fifo_shared(s)) {
            memset(s->fifo_waiting, 0, sizeof(s->fifo_waiting));
            s->fifo_next_channel = 0;
        }
        bcm2835_pwm_update_outputs(s);
        for (unsigned channel = 0; channel < 2; channel++) {
            if (bcm2835_pwm_channel_enabled(s, channel)) {
                if (s->transmitting[channel]) {
                    timer_del(s->edge_timer[channel]);
                    s->edge_remaining_cycles[channel] = 0;
                    bcm2835_pwm_set_waveform(
                        s, channel,
                        bcm2835_pwm_raw_level(
                            s, channel, s->wave_position[channel]));
                    s->wave_target[channel] =
                        bcm2835_pwm_next_transition(s, channel);
                    bcm2835_pwm_schedule_edge(s, channel);
                } else {
                    bcm2835_pwm_start_channel(s, channel);
                }
            } else {
                bcm2835_pwm_cancel_channel(s, channel);
            }
        }
        break;
    case PWM_STA:
        s->sta_sticky &= ~(value & PWM_STA_W1C_MASK);
        break;
    case PWM_DMAC:
        s->dmac = value & PWM_DMAC_MASK;
        bcm2835_pwm_update_outputs(s);
        break;
    case PWM_RNG1:
        old_fifo_shared = bcm2835_pwm_fifo_shared(s);
        s->range[0] = value;
        if (old_fifo_shared != bcm2835_pwm_fifo_shared(s)) {
            memset(s->fifo_waiting, 0, sizeof(s->fifo_waiting));
            s->fifo_next_channel = 0;
            for (unsigned channel = 0; channel < 2; channel++) {
                if (bcm2835_pwm_channel_enabled(s, channel)) {
                    bcm2835_pwm_start_channel(s, channel);
                } else {
                    bcm2835_pwm_cancel_channel(s, channel);
                }
            }
        } else if (value) {
            bcm2835_pwm_start_channel(s, 0);
        } else {
            bcm2835_pwm_cancel_channel(s, 0);
        }
        break;
    case PWM_DAT1:
        s->data[0] = value;
        break;
    case PWM_FIF1:
        bcm2835_pwm_fifo_push(s, value);
        break;
    case PWM_RNG2:
        old_fifo_shared = bcm2835_pwm_fifo_shared(s);
        s->range[1] = value;
        if (old_fifo_shared != bcm2835_pwm_fifo_shared(s)) {
            memset(s->fifo_waiting, 0, sizeof(s->fifo_waiting));
            s->fifo_next_channel = 0;
            for (unsigned channel = 0; channel < 2; channel++) {
                if (bcm2835_pwm_channel_enabled(s, channel)) {
                    bcm2835_pwm_start_channel(s, channel);
                } else {
                    bcm2835_pwm_cancel_channel(s, channel);
                }
            }
        } else if (value) {
            bcm2835_pwm_start_channel(s, 1);
        } else {
            bcm2835_pwm_cancel_channel(s, 1);
        }
        break;
    case PWM_DAT2:
        s->data[1] = value;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps bcm2835_pwm_ops = {
    .read = bcm2835_pwm_read,
    .write = bcm2835_pwm_write,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void bcm2835_pwm_reset(DeviceState *dev)
{
    BCM2835PwmState *s = BCM2835_PWM(dev);

    s->ctl = 0;
    s->sta_sticky = 0;
    s->dmac = 0;
    memset(s->range, 0, sizeof(s->range));
    memset(s->data, 0, sizeof(s->data));
    memset(s->active_range, 0, sizeof(s->active_range));
    memset(s->active_data, 0, sizeof(s->active_data));
    memset(s->wave_position, 0, sizeof(s->wave_position));
    memset(s->wave_target, 0, sizeof(s->wave_target));
    memset(s->transmitting, 0, sizeof(s->transmitting));
    memset(s->waveform_level, 0, sizeof(s->waveform_level));
    memset(s->fifo_waiting, 0, sizeof(s->fifo_waiting));
    memset(s->edge_remaining_cycles, 0,
           sizeof(s->edge_remaining_cycles));
    bcm2835_pwm_fifo_clear(s);
    for (unsigned channel = 0; channel < 2; channel++) {
        bcm2835_pwm_cancel_channel(s, channel);
    }
    bcm2835_pwm_update_outputs(s);
}

static int bcm2835_pwm_post_load(void *opaque, int version_id)
{
    BCM2835PwmState *s = opaque;

    if (s->fifo_head >= BCM2835_PWM_FIFO_DEPTH ||
        s->fifo_count > BCM2835_PWM_FIFO_DEPTH) {
        return -EINVAL;
    }
    if (version_id < 4) {
        memset(s->fifo_waiting, 0, sizeof(s->fifo_waiting));
        s->fifo_next_channel = 0;
    } else if (s->fifo_next_channel >= 2 ||
               ((!bcm2835_pwm_fifo_shared(s)) &&
                (s->fifo_waiting[0] || s->fifo_waiting[1]))) {
        return -EINVAL;
    }
    for (unsigned channel = 0; channel < 2; channel++) {
        if (version_id < 2) {
            s->remaining_cycles[channel] = 0;
        }
        if (version_id < 3) {
            s->active_range[channel] = s->range[channel];
            s->active_data[channel] = s->data[channel];
            s->wave_position[channel] = 0;
            s->wave_target[channel] = s->active_range[channel];
            s->transmitting[channel] =
                (timer_pending(s->timer[channel]) ||
                 s->remaining_cycles[channel]) &&
                s->active_range[channel];
            s->edge_remaining_cycles[channel] = 0;
        }
        if (s->transmitting[channel]) {
            if (s->fifo_waiting[channel]) {
                return -EINVAL;
            }
            if (!s->active_range[channel] ||
                s->wave_position[channel] >=
                    s->active_range[channel] ||
                s->wave_target[channel] <
                    s->wave_position[channel] ||
                s->wave_target[channel] >
                    s->active_range[channel]) {
                return -EINVAL;
            }
            if (!timer_pending(s->timer[channel]) &&
                !s->remaining_cycles[channel]) {
                return -EINVAL;
            }
            if (s->wave_target[channel] <
                    s->active_range[channel] &&
                !timer_pending(s->edge_timer[channel]) &&
                !s->edge_remaining_cycles[channel]) {
                return -EINVAL;
            }
            bcm2835_pwm_set_waveform(
                s, channel,
                bcm2835_pwm_raw_level(
                    s, channel, s->wave_position[channel]));
        } else {
            if (version_id >= 3 &&
                (timer_pending(s->timer[channel]) ||
                 s->remaining_cycles[channel] ||
                 timer_pending(s->edge_timer[channel]) ||
                 s->edge_remaining_cycles[channel])) {
                return -EINVAL;
            }
            bcm2835_pwm_set_idle(s, channel);
        }
        bcm2835_pwm_schedule(s, channel);
        bcm2835_pwm_schedule_edge(s, channel);
    }
    bcm2835_pwm_update_outputs(s);
    return 0;
}

static const VMStateDescription bcm2835_pwm_vmstate = {
    .name = "bcm2835_pwm",
    .version_id = 4,
    .minimum_version_id = 1,
    .post_load = bcm2835_pwm_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctl, BCM2835PwmState),
        VMSTATE_UINT32(sta_sticky, BCM2835PwmState),
        VMSTATE_UINT32(dmac, BCM2835PwmState),
        VMSTATE_UINT32_ARRAY(range, BCM2835PwmState, 2),
        VMSTATE_UINT32_ARRAY(data, BCM2835PwmState, 2),
        VMSTATE_UINT32_ARRAY(fifo, BCM2835PwmState,
                             BCM2835_PWM_FIFO_DEPTH),
        VMSTATE_UINT8(fifo_head, BCM2835PwmState),
        VMSTATE_UINT8(fifo_count, BCM2835PwmState),
        VMSTATE_TIMER_PTR_V(timer[0], BCM2835PwmState, 2),
        VMSTATE_TIMER_PTR_V(timer[1], BCM2835PwmState, 2),
        VMSTATE_UINT64_ARRAY_V(remaining_cycles, BCM2835PwmState, 2, 2),
        VMSTATE_TIMER_PTR_V(edge_timer[0], BCM2835PwmState, 3),
        VMSTATE_TIMER_PTR_V(edge_timer[1], BCM2835PwmState, 3),
        VMSTATE_UINT64_ARRAY_V(edge_remaining_cycles,
                               BCM2835PwmState, 2, 3),
        VMSTATE_UINT32_ARRAY_V(active_range, BCM2835PwmState, 2, 3),
        VMSTATE_UINT32_ARRAY_V(active_data, BCM2835PwmState, 2, 3),
        VMSTATE_UINT32_ARRAY_V(wave_position, BCM2835PwmState, 2, 3),
        VMSTATE_UINT32_ARRAY_V(wave_target, BCM2835PwmState, 2, 3),
        VMSTATE_BOOL_ARRAY_V(transmitting, BCM2835PwmState, 2, 3),
        VMSTATE_BOOL_ARRAY_V(waveform_level, BCM2835PwmState, 2, 3),
        VMSTATE_BOOL_ARRAY_V(fifo_waiting, BCM2835PwmState, 2, 4),
        VMSTATE_UINT8_V(fifo_next_channel, BCM2835PwmState, 4),
        VMSTATE_END_OF_LIST()
    }
};

static void bcm2835_pwm_init(Object *obj)
{
    BCM2835PwmState *s = BCM2835_PWM(obj);

    memory_region_init_io(&s->iomem, obj, &bcm2835_pwm_ops, s,
                          TYPE_BCM2835_PWM, PWM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    qdev_init_gpio_out_named(DEVICE(s), s->channel_enabled,
                             "channel-enabled", 2);
    qdev_init_gpio_out_named(DEVICE(s), s->waveform,
                             "waveform", 2);
    qdev_init_gpio_out_named(
        DEVICE(s), s->dma_threshold, "dma-threshold", 2);
    s->clk = qdev_init_clock_in(
        DEVICE(s), "clk", bcm2835_pwm_clock_update, s,
        ClockPreUpdate | ClockUpdate);
    s->timer[0] = timer_new_ns(
        QEMU_CLOCK_VIRTUAL, bcm2835_pwm_timer0, s);
    s->timer[1] = timer_new_ns(
        QEMU_CLOCK_VIRTUAL, bcm2835_pwm_timer1, s);
    s->edge_timer[0] = timer_new_ns(
        QEMU_CLOCK_VIRTUAL, bcm2835_pwm_edge0, s);
    s->edge_timer[1] = timer_new_ns(
        QEMU_CLOCK_VIRTUAL, bcm2835_pwm_edge1, s);
    object_property_add(obj, "clock-frequency", "uint32",
                        bcm2835_pwm_clock_get, NULL, NULL, NULL);
}

static void bcm2835_pwm_finalize(Object *obj)
{
    BCM2835PwmState *s = BCM2835_PWM(obj);

    timer_free(s->timer[0]);
    timer_free(s->timer[1]);
    timer_free(s->edge_timer[0]);
    timer_free(s->edge_timer[1]);
}

static void bcm2835_pwm_realize(DeviceState *dev, Error **errp)
{
    BCM2835PwmState *s = BCM2835_PWM(dev);

    if (!clock_has_source(s->clk)) {
        error_setg(errp, "BCM2835 PWM: clock input must be connected");
    }
}

static void bcm2835_pwm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = bcm2835_pwm_realize;
    device_class_set_legacy_reset(dc, bcm2835_pwm_reset);
    dc->vmsd = &bcm2835_pwm_vmstate;
}

static const TypeInfo bcm2835_pwm_info = {
    .name = TYPE_BCM2835_PWM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2835PwmState),
    .instance_init = bcm2835_pwm_init,
    .instance_finalize = bcm2835_pwm_finalize,
    .class_init = bcm2835_pwm_class_init,
};

static void bcm2835_pwm_register_types(void)
{
    type_register_static(&bcm2835_pwm_info);
}

type_init(bcm2835_pwm_register_types)
