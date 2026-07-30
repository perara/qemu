/*
 * Broadcom Serial Controller (BSC)
 *
 * Copyright (c) 2024 Rayhan Faizel <rayhan.faizel@gmail.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/i2c/bcm2835_i2c.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qapi/visitor.h"
#include "qemu/main-loop.h"

static void bcm2835_i2c_update_interrupt(BCM2835I2CState *s);
static void bcm2835_i2c_transfer_bh(void *opaque);
static void bcm2835_i2c_schedule_stretch(BCM2835I2CState *s);
static void bcm2835_i2c_finish_transfer(BCM2835I2CState *s);

static void bcm2835_i2c_fifo_reset(BCM2835I2CState *s)
{
    memset(s->fifo, 0, sizeof(s->fifo));
    s->fifo_pos = 0;
    s->fifo_len = 0;
}

static bool bcm2835_i2c_fifo_push(BCM2835I2CState *s, uint8_t value)
{
    unsigned int index;

    if (s->fifo_len == BCM2835_I2C_FIFO_LEN) {
        return false;
    }

    index = (s->fifo_pos + s->fifo_len) %
            BCM2835_I2C_FIFO_LEN;
    s->fifo[index] = value;
    s->fifo_len++;
    return true;
}

static uint8_t bcm2835_i2c_fifo_pop(BCM2835I2CState *s)
{
    uint8_t value;

    if (!s->fifo_len) {
        return 0;
    }

    value = s->fifo[s->fifo_pos];
    s->fifo[s->fifo_pos] = 0;
    s->fifo_pos = (s->fifo_pos + 1) % BCM2835_I2C_FIFO_LEN;
    s->fifo_len--;
    return value;
}

static void bcm2835_i2c_update_fifo_status(BCM2835I2CState *s)
{
    s->s &= ~(BCM2835_I2C_S_RXF | BCM2835_I2C_S_TXE |
              BCM2835_I2C_S_RXD | BCM2835_I2C_S_TXD |
              BCM2835_I2C_S_RXR | BCM2835_I2C_S_TXW);

    if (s->c & BCM2835_I2C_C_READ) {
        /*
         * TX is idle during a read transfer.  The receive side stalls when
         * its shared directional FIFO reaches sixteen bytes.
         */
        s->s |= BCM2835_I2C_S_TXD | BCM2835_I2C_S_TXE;
        if (s->fifo_len) {
            s->s |= BCM2835_I2C_S_RXD;
        }
        if (s->fifo_len == BCM2835_I2C_FIFO_LEN) {
            s->s |= BCM2835_I2C_S_RXF;
        }
        if ((s->s & BCM2835_I2C_S_TA) &&
            s->fifo_len >= BCM2835_I2C_RXR_THRESHOLD) {
            s->s |= BCM2835_I2C_S_RXR;
        }
    } else {
        if (!s->fifo_len) {
            s->s |= BCM2835_I2C_S_TXE;
        }
        if (s->fifo_len < BCM2835_I2C_FIFO_LEN) {
            s->s |= BCM2835_I2C_S_TXD;
        }
        if ((s->s & BCM2835_I2C_S_TA) && s->dlen &&
            s->fifo_len < BCM2835_I2C_TXW_THRESHOLD) {
            s->s |= BCM2835_I2C_S_TXW;
        }
    }
}

static void bcm2835_i2c_update_interrupt(BCM2835I2CState *s)
{
    int do_interrupt = 0;
    /* Interrupt on RXR (Needs reading) */
    if (s->c & BCM2835_I2C_C_INTR && s->s & BCM2835_I2C_S_RXR) {
        do_interrupt = 1;
    }

    /* Interrupt on TXW (Needs writing) */
    if (s->c & BCM2835_I2C_C_INTT && s->s & BCM2835_I2C_S_TXW) {
        do_interrupt = 1;
    }

    /* Interrupt on DONE (Transfer complete) */
    if (s->c & BCM2835_I2C_C_INTD && s->s & BCM2835_I2C_S_DONE) {
        do_interrupt = 1;
    }
    qemu_set_irq(s->irq, do_interrupt);
}

static uint32_t bcm2835_i2c_clock_divisor(const BCM2835I2CState *s)
{
    uint32_t divisor = s->div & 0xffff;

    if (!divisor) {
        return 32768;
    }

    /* Odd divisors are rounded down; two is the fastest legal divisor. */
    return MAX(2U, divisor & ~1U);
}

static uint64_t bcm2835_i2c_serial_hz(const BCM2835I2CState *s)
{
    return clock_get_hz(s->core_clk) / bcm2835_i2c_clock_divisor(s);
}

static void bcm2835_i2c_pause_stretch(BCM2835I2CState *s)
{
    uint64_t serial_hz;
    uint64_t now;
    uint64_t expiry;
    uint64_t remaining_ns;

    if (!timer_pending(s->stretch_timer)) {
        return;
    }

    serial_hz = bcm2835_i2c_serial_hz(s);
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    expiry = timer_expire_time_ns(s->stretch_timer);
    remaining_ns = expiry > now ? expiry - now : 1;
    s->stretch_remaining_cycles = serial_hz ?
        DIV_ROUND_UP(remaining_ns * serial_hz,
                     NANOSECONDS_PER_SECOND) : 1;
    timer_del(s->stretch_timer);
}

static void bcm2835_i2c_schedule_stretch(BCM2835I2CState *s)
{
    uint64_t serial_hz;
    uint64_t delay_ns;

    if (!s->stretch_waiting || timer_pending(s->stretch_timer)) {
        return;
    }

    serial_hz = bcm2835_i2c_serial_hz(s);
    if (!serial_hz) {
        return;
    }

    delay_ns = MAX(1ULL,
                   DIV_ROUND_UP((uint64_t)s->stretch_remaining_cycles *
                                NANOSECONDS_PER_SECOND, serial_hz));
    timer_mod_ns(s->stretch_timer,
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay_ns);
}

static void bcm2835_i2c_core_clock_update(void *opaque, ClockEvent event)
{
    BCM2835I2CState *s = opaque;

    if (event == ClockPreUpdate) {
        bcm2835_i2c_pause_stretch(s);
    } else if (event == ClockUpdate) {
        bcm2835_i2c_schedule_stretch(s);
    }
}

static void bcm2835_i2c_core_clock_get(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    BCM2835I2CState *s = BCM2835_I2C(obj);
    uint32_t frequency = clock_get_hz(s->core_clk);

    visit_type_uint32(v, name, &frequency, errp);
}

static void bcm2835_i2c_stretch_timer(void *opaque)
{
    BCM2835I2CState *s = opaque;

    s->stretch_waiting = false;
    s->stretch_remaining_cycles = 0;
    if (!(s->s & BCM2835_I2C_S_TA)) {
        return;
    }

    if (s->stretch_timeout) {
        s->s |= BCM2835_I2C_S_CLKT;
        bcm2835_i2c_finish_transfer(s);
        bcm2835_i2c_update_interrupt(s);
        return;
    }

    qemu_bh_schedule(s->transfer_bh);
}

static bool bcm2835_i2c_maybe_stretch(BCM2835I2CState *s)
{
    uint32_t requested_cycles = 0;
    uint32_t wait_cycles;

    if (s->stretch_completed) {
        return false;
    }

    if (s->clock_stretch_cycles && !s->stretch_injected &&
        s->transfer_bytes == s->clock_stretch_after) {
        requested_cycles = s->clock_stretch_cycles;
        s->stretch_injected = true;
    }
    requested_cycles = MAX(
        requested_cycles,
        i2c_get_stretch_cycles(s->bus, s->c & BCM2835_I2C_C_READ,
                               s->transfer_bytes));
    if (!requested_cycles) {
        return false;
    }

    s->stretch_completed = true;
    s->stretch_waiting = true;
    s->stretch_timeout = s->clkt && requested_cycles > s->clkt;
    wait_cycles = s->stretch_timeout ? s->clkt : requested_cycles;
    s->stretch_remaining_cycles = MAX(1U, wait_cycles);
    bcm2835_i2c_schedule_stretch(s);
    return true;
}

static void bcm2835_i2c_address_nack(BCM2835I2CState *s)
{
    /*
     * The generic I2C core releases a newly-started failed transaction.
     * Release an unsuccessful repeated start explicitly.
     */
    if (i2c_bus_busy(s->bus)) {
        i2c_end_transfer(s->bus);
    }
    s->ten_bit_address_pending = false;
    s->s |= BCM2835_I2C_S_ERR | BCM2835_I2C_S_DONE;
    s->s &= ~BCM2835_I2C_S_TA;
    bcm2835_i2c_update_fifo_status(s);
}

static void bcm2835_i2c_begin_transfer(BCM2835I2CState *s)
{
    int direction = s->c & BCM2835_I2C_C_READ;
    bool ten_bit_prefix = (s->a & 0x7c) == 0x78;

    timer_del(s->stretch_timer);
    s->transfer_bytes = 0;
    s->stretch_remaining_cycles = 0;
    s->stretch_injected = false;
    s->stretch_completed = false;
    s->stretch_waiting = false;
    s->stretch_timeout = false;

    if (ten_bit_prefix) {
        if (direction) {
            if (!s->ten_bit_address_valid ||
                extract32(s->ten_bit_address, 8, 2) != (s->a & 0x3) ||
                i2c_start_transfer_10bit(s->bus, s->ten_bit_address, true)) {
                bcm2835_i2c_address_nack(s);
                return;
            }
        } else {
            /*
             * The first FIFO byte contains A7:A0.  Assert TA before it is
             * supplied so software can use TXW to stage a combined transfer.
             */
            s->ten_bit_address_pending = true;
        }
    } else if (i2c_start_transfer(s->bus, s->a, direction)) {
        s->ten_bit_address_valid = false;
        bcm2835_i2c_address_nack(s);
        return;
    } else {
        s->ten_bit_address_valid = false;
        s->ten_bit_address_pending = false;
    }
    s->s |= BCM2835_I2C_S_TA;
    bcm2835_i2c_update_fifo_status(s);
    qemu_bh_schedule(s->transfer_bh);
}

static void bcm2835_i2c_finish_transfer(BCM2835I2CState *s)
{
    /*
     * STOP is sent when DLEN counts down to zero.
     *
     * https://github.com/torvalds/linux/blob/v6.7/drivers/i2c/busses/i2c-bcm2835.c#L223-L261
     * A repeated ST issued while TA is still set is handled by
     * bcm2835_i2c_begin_transfer() before this completion path is reached.
     */
    timer_del(s->stretch_timer);
    s->stretch_waiting = false;
    s->stretch_remaining_cycles = 0;
    s->ten_bit_address_pending = false;
    if (s->c & BCM2835_I2C_C_READ) {
        i2c_nack(s->bus);
    }
    i2c_end_transfer(s->bus);
    s->s |= BCM2835_I2C_S_DONE;
    s->s &= ~BCM2835_I2C_S_TA;
    bcm2835_i2c_update_fifo_status(s);
}

static void bcm2835_i2c_transfer_bh(void *opaque)
{
    BCM2835I2CState *s = opaque;

    if (!(s->s & BCM2835_I2C_S_TA)) {
        return;
    }

    if (s->ten_bit_address_pending) {
        uint8_t low_address;

        if (!s->dlen) {
            bcm2835_i2c_address_nack(s);
            bcm2835_i2c_update_interrupt(s);
            return;
        }
        if (!s->fifo_len) {
            bcm2835_i2c_update_fifo_status(s);
            bcm2835_i2c_update_interrupt(s);
            return;
        }
        if (bcm2835_i2c_maybe_stretch(s)) {
            return;
        }
        low_address = bcm2835_i2c_fifo_pop(s);
        s->ten_bit_address = ((s->a & 0x3) << 8) | low_address;
        s->dlen--;
        s->transfer_bytes++;
        s->stretch_completed = false;
        s->ten_bit_address_pending = false;
        if (i2c_start_transfer_10bit(s->bus, s->ten_bit_address, false)) {
            s->ten_bit_address_valid = false;
            bcm2835_i2c_address_nack(s);
            bcm2835_i2c_update_interrupt(s);
            return;
        }
        s->ten_bit_address_valid = true;
    }

    if (s->c & BCM2835_I2C_C_READ) {
        while (s->dlen && s->fifo_len < BCM2835_I2C_FIFO_LEN) {
            if (bcm2835_i2c_maybe_stretch(s)) {
                return;
            }
            bcm2835_i2c_fifo_push(s, i2c_recv(s->bus));
            s->dlen--;
            s->transfer_bytes++;
            s->stretch_completed = false;
        }
    } else {
        while (s->dlen && s->fifo_len) {
            uint8_t value;

            if (bcm2835_i2c_maybe_stretch(s)) {
                return;
            }
            value = bcm2835_i2c_fifo_pop(s);
            if (i2c_send(s->bus, value)) {
                s->s |= BCM2835_I2C_S_ERR;
                bcm2835_i2c_finish_transfer(s);
                bcm2835_i2c_update_interrupt(s);
                return;
            }
            s->dlen--;
            s->transfer_bytes++;
            s->stretch_completed = false;
        }
    }

    if (!s->dlen) {
        bcm2835_i2c_finish_transfer(s);
    } else {
        bcm2835_i2c_update_fifo_status(s);
    }
    bcm2835_i2c_update_interrupt(s);
}

static uint64_t bcm2835_i2c_read(void *opaque, hwaddr addr, unsigned size)
{
    BCM2835I2CState *s = opaque;
    uint32_t readval = 0;

    switch (addr) {
    case BCM2835_I2C_C:
        readval = s->c;
        break;
    case BCM2835_I2C_S:
        readval = s->s;
        break;
    case BCM2835_I2C_DLEN:
        readval = s->dlen;
        break;
    case BCM2835_I2C_A:
        readval = s->a;
        break;
    case BCM2835_I2C_FIFO:
        if (s->fifo_len) {
            readval = bcm2835_i2c_fifo_pop(s);
            bcm2835_i2c_update_fifo_status(s);
            if (s->s & BCM2835_I2C_S_TA) {
                qemu_bh_schedule(s->transfer_bh);
            }
        }
        bcm2835_i2c_update_interrupt(s);
        break;
    case BCM2835_I2C_DIV:
        readval = s->div;
        break;
    case BCM2835_I2C_DEL:
        readval = s->del;
        break;
    case BCM2835_I2C_CLKT:
        readval = s->clkt;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Bad offset 0x%" HWADDR_PRIx "\n", __func__, addr);
    }

    return readval;
}

static void bcm2835_i2c_write(void *opaque, hwaddr addr,
                              uint64_t value, unsigned int size)
{
    BCM2835I2CState *s = opaque;
    uint32_t writeval = value;

    switch (addr) {
    case BCM2835_I2C_C:
    {
        bool clear = writeval & BCM2835_I2C_C_CLEAR;
        bool start = (writeval &
                      (BCM2835_I2C_C_ST | BCM2835_I2C_C_I2CEN)) ==
                     (BCM2835_I2C_C_ST | BCM2835_I2C_C_I2CEN);

        if (clear) {
            timer_del(s->stretch_timer);
            s->stretch_waiting = false;
            s->stretch_remaining_cycles = 0;
            s->ten_bit_address_pending = false;
            bcm2835_i2c_fifo_reset(s);
            if ((s->s & BCM2835_I2C_S_TA) && !start) {
                i2c_end_transfer(s->bus);
                s->s &= ~BCM2835_I2C_S_TA;
                s->s |= BCM2835_I2C_S_DONE;
            }
        }

        /* ST and CLEAR are commands and always read back as zero. */
        s->c = writeval & BCM2835_I2C_C_MASK;

        /* A transfer starts only when the controller is enabled and ST set. */
        if (start) {
            bcm2835_i2c_begin_transfer(s);
            /*
             * Handle special case where transfer starts with zero data length.
             * Required for zero length i2c quick messages to work.
             */
            if (s->dlen == 0) {
                bcm2835_i2c_finish_transfer(s);
            }
        } else {
            bcm2835_i2c_update_fifo_status(s);
        }

        bcm2835_i2c_update_interrupt(s);
        break;
    }
    case BCM2835_I2C_S:
        if (writeval & BCM2835_I2C_S_DONE && s->s & BCM2835_I2C_S_DONE) {
            /* When DONE is cleared, DLEN should read last written value. */
            s->dlen = s->last_dlen;
        }

        /* Clear DONE, CLKT and ERR by writing 1 */
        s->s &= ~(writeval & (BCM2835_I2C_S_DONE |
                  BCM2835_I2C_S_ERR | BCM2835_I2C_S_CLKT));
        bcm2835_i2c_update_interrupt(s);
        break;
    case BCM2835_I2C_DLEN:
        s->dlen = writeval & 0xffff;
        s->last_dlen = s->dlen;
        break;
    case BCM2835_I2C_A:
        s->a = writeval & 0x7f;
        break;
    case BCM2835_I2C_FIFO:
        if (!(s->c & BCM2835_I2C_C_READ) &&
            bcm2835_i2c_fifo_push(s, writeval & 0xff)) {
            bcm2835_i2c_update_fifo_status(s);
            if (s->s & BCM2835_I2C_S_TA) {
                qemu_bh_schedule(s->transfer_bh);
            }
        }
        bcm2835_i2c_update_interrupt(s);
        break;
    case BCM2835_I2C_DIV:
        s->div = writeval & 0xffff;
        break;
    case BCM2835_I2C_DEL:
        s->del = writeval;
        break;
    case BCM2835_I2C_CLKT:
        s->clkt = writeval & 0xffff;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Bad offset 0x%" HWADDR_PRIx "\n", __func__, addr);
    }
}

static const MemoryRegionOps bcm2835_i2c_ops = {
    .read = bcm2835_i2c_read,
    .write = bcm2835_i2c_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void bcm2835_i2c_realize(DeviceState *dev, Error **errp)
{
    BCM2835I2CState *s = BCM2835_I2C(dev);
    s->bus = i2c_init_bus(dev, NULL);
    s->transfer_bh = qemu_bh_new_guarded(
        bcm2835_i2c_transfer_bh, s, &dev->mem_reentrancy_guard);

    memory_region_init_io(&s->iomem, OBJECT(dev), &bcm2835_i2c_ops, s,
                          TYPE_BCM2835_I2C, 0x24);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static void bcm2835_i2c_reset(DeviceState *dev)
{
    BCM2835I2CState *s = BCM2835_I2C(dev);

    qemu_bh_cancel(s->transfer_bh);
    timer_del(s->stretch_timer);
    if (i2c_bus_busy(s->bus)) {
        i2c_end_transfer(s->bus);
    }

    /* Reset values according to BCM2835 Peripheral Documentation */
    s->c = 0x0;
    s->s = BCM2835_I2C_S_TXD | BCM2835_I2C_S_TXE;
    s->dlen = 0x0;
    s->a = 0x0;
    s->div = 0x5dc;
    s->del = 0x00300030;
    s->clkt = 0x40;
    s->last_dlen = 0;
    s->transfer_bytes = 0;
    s->stretch_remaining_cycles = 0;
    s->stretch_injected = false;
    s->stretch_completed = false;
    s->stretch_waiting = false;
    s->stretch_timeout = false;
    s->ten_bit_address = 0;
    s->ten_bit_address_valid = false;
    s->ten_bit_address_pending = false;
    bcm2835_i2c_fifo_reset(s);
    qemu_set_irq(s->irq, 0);
}

static int bcm2835_i2c_post_load(void *opaque, int version_id)
{
    BCM2835I2CState *s = opaque;

    if (version_id < 2) {
        bcm2835_i2c_fifo_reset(s);
    }
    if (version_id < 3) {
        s->transfer_bytes = 0;
        s->stretch_remaining_cycles = 0;
        s->stretch_injected = false;
        s->stretch_completed = false;
        s->stretch_waiting = false;
        s->stretch_timeout = false;
    }
    if (version_id < 4) {
        s->stretch_completed = false;
    }
    if (version_id < 5) {
        s->ten_bit_address = 0;
        s->ten_bit_address_valid = false;
        s->ten_bit_address_pending = false;
    }
    if (s->fifo_len > BCM2835_I2C_FIFO_LEN ||
        s->fifo_pos >= BCM2835_I2C_FIFO_LEN ||
        (s->stretch_waiting && !(s->s & BCM2835_I2C_S_TA)) ||
        (s->ten_bit_address_pending &&
         (!(s->s & BCM2835_I2C_S_TA) || s->c & BCM2835_I2C_C_READ)) ||
        (!s->stretch_waiting && timer_pending(s->stretch_timer))) {
        return -EINVAL;
    }
    bcm2835_i2c_update_fifo_status(s);
    bcm2835_i2c_update_interrupt(s);
    if (s->stretch_waiting) {
        bcm2835_i2c_schedule_stretch(s);
    } else if (s->s & BCM2835_I2C_S_TA) {
        qemu_bh_schedule(s->transfer_bh);
    }
    return 0;
}

static void bcm2835_i2c_unrealize(DeviceState *dev)
{
    BCM2835I2CState *s = BCM2835_I2C(dev);

    timer_del(s->stretch_timer);
    qemu_bh_delete(s->transfer_bh);
}

static const VMStateDescription vmstate_bcm2835_i2c = {
    .name = TYPE_BCM2835_I2C,
    .version_id = 5,
    .minimum_version_id = 1,
    .post_load = bcm2835_i2c_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(c, BCM2835I2CState),
        VMSTATE_UINT32(s, BCM2835I2CState),
        VMSTATE_UINT32(dlen, BCM2835I2CState),
        VMSTATE_UINT32(a, BCM2835I2CState),
        VMSTATE_UINT32(div, BCM2835I2CState),
        VMSTATE_UINT32(del, BCM2835I2CState),
        VMSTATE_UINT32(clkt, BCM2835I2CState),
        VMSTATE_UINT32(last_dlen, BCM2835I2CState),
        VMSTATE_UINT8_ARRAY_V(fifo, BCM2835I2CState,
                             BCM2835_I2C_FIFO_LEN, 2),
        VMSTATE_UINT8_V(fifo_pos, BCM2835I2CState, 2),
        VMSTATE_UINT8_V(fifo_len, BCM2835I2CState, 2),
        VMSTATE_UINT32_V(transfer_bytes, BCM2835I2CState, 3),
        VMSTATE_UINT32_V(stretch_remaining_cycles, BCM2835I2CState, 3),
        VMSTATE_BOOL_V(stretch_injected, BCM2835I2CState, 3),
        VMSTATE_BOOL_V(stretch_completed, BCM2835I2CState, 4),
        VMSTATE_BOOL_V(stretch_waiting, BCM2835I2CState, 3),
        VMSTATE_BOOL_V(stretch_timeout, BCM2835I2CState, 3),
        VMSTATE_TIMER_PTR_V(stretch_timer, BCM2835I2CState, 3),
        VMSTATE_UINT16_V(ten_bit_address, BCM2835I2CState, 5),
        VMSTATE_BOOL_V(ten_bit_address_valid, BCM2835I2CState, 5),
        VMSTATE_BOOL_V(ten_bit_address_pending, BCM2835I2CState, 5),
        VMSTATE_END_OF_LIST()
    }
};

static const Property bcm2835_i2c_properties[] = {
    DEFINE_PROP_UINT32("clock-stretch-after", BCM2835I2CState,
                       clock_stretch_after, UINT32_MAX),
    DEFINE_PROP_UINT32("clock-stretch-cycles", BCM2835I2CState,
                       clock_stretch_cycles, 0),
};

static void bcm2835_i2c_init(Object *obj)
{
    BCM2835I2CState *s = BCM2835_I2C(obj);

    s->stretch_timer = timer_new_ns(
        QEMU_CLOCK_VIRTUAL, bcm2835_i2c_stretch_timer, s);
    s->core_clk = qdev_init_clock_in(
        DEVICE(s), "core", bcm2835_i2c_core_clock_update, s,
        ClockPreUpdate | ClockUpdate);
    object_property_add(obj, "core-clock-frequency", "uint32",
                        bcm2835_i2c_core_clock_get, NULL, NULL, NULL);
}

static void bcm2835_i2c_finalize(Object *obj)
{
    BCM2835I2CState *s = BCM2835_I2C(obj);

    timer_free(s->stretch_timer);
}

static void bcm2835_i2c_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, bcm2835_i2c_reset);
    device_class_set_props(dc, bcm2835_i2c_properties);
    dc->realize = bcm2835_i2c_realize;
    dc->unrealize = bcm2835_i2c_unrealize;
    dc->vmsd = &vmstate_bcm2835_i2c;
}

static const TypeInfo bcm2835_i2c_info = {
    .name = TYPE_BCM2835_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2835I2CState),
    .instance_init = bcm2835_i2c_init,
    .instance_finalize = bcm2835_i2c_finalize,
    .class_init = bcm2835_i2c_class_init,
};

static void bcm2835_i2c_register_types(void)
{
    type_register_static(&bcm2835_i2c_info);
}

type_init(bcm2835_i2c_register_types)
