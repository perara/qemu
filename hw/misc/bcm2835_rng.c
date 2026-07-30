/*
 * BCM2835 Random Number Generator emulation
 *
 * Copyright (C) 2017 Marcin Chojnacki <marcinch7@gmail.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/guest-random.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/misc/bcm2835_rng.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

#define RNG200_CTRL_ENABLE                 BIT(0)
#define RNG200_CTRL_MASK                   0x001fffff
#define RNG200_FIFO_WORDS                  16
#define RNG200_FIFO_BYTES                  (RNG200_FIFO_WORDS * 4)
#define RNG200_WARM_UP_BITS                32
#define RNG200_INT_TOTAL_BITS_COUNT        BIT(0)
#define RNG200_INT_NIST_FAIL               BIT(5)
#define RNG200_INT_STARTUP_TRANSITIONS_MET BIT(17)
#define RNG200_INT_MASTER_FAIL_LOCKOUT     BIT(31)
#define RNG200_INT_MASK                    \
    (RNG200_INT_TOTAL_BITS_COUNT | RNG200_INT_NIST_FAIL | \
     RNG200_INT_STARTUP_TRANSITIONS_MET | \
     RNG200_INT_MASTER_FAIL_LOCKOUT)

static uint32_t get_random_bytes(BCM2835RngState *s)
{
    uint32_t res;

    if (s->rng200 && s->deterministic_seed) {
        uint32_t value = s->prng_state;

        value ^= value << 13;
        value ^= value >> 17;
        value ^= value << 5;
        s->prng_state = value;
        return value;
    }

    /*
     * On failure we don't want to return the guest a non-random
     * value in case they're really using it for cryptographic
     * purposes, so the best we can do is die here.
     * This shouldn't happen unless something's broken.
     * In theory we could implement this device's full FIFO
     * and interrupt semantics and then just stop filling the
     * FIFO. That's a lot of work, though, so we assume any
     * errors are systematic problems and trust that if we didn't
     * fail as the guest inited then we won't fail later on
     * mid-run.
     */
    qemu_guest_getrandom_nofail(&res, sizeof(res));
    return res;
}

static void bcm2835_rng200_update_irq(BCM2835RngState *s)
{
    qemu_set_irq(s->irq, (s->int_status & s->int_enable) != 0);
}

static void bcm2835_rng200_update_threshold(BCM2835RngState *s)
{
    uint32_t fifo_words = fifo8_num_used(&s->fifo) / sizeof(uint32_t);
    bool total_reached = s->total_bit_count_threshold &&
        s->total_bit_count >= s->total_bit_count_threshold;
    bool fifo_reached = s->fifo_threshold &&
        fifo_words >= s->fifo_threshold;

    if (total_reached || fifo_reached) {
        s->int_status |= RNG200_INT_TOTAL_BITS_COUNT;
    }
    bcm2835_rng200_update_irq(s);
}

static void bcm2835_rng200_fill_fifo(BCM2835RngState *s)
{
    while (fifo8_num_free(&s->fifo) >= sizeof(uint32_t)) {
        uint32_t value = get_random_bytes(s);
        uint8_t bytes[sizeof(value)];

        stl_le_p(bytes, value);
        fifo8_push_all(&s->fifo, bytes, sizeof(bytes));
        s->total_bit_count += 32;
    }
    bcm2835_rng200_update_threshold(s);
}

static void bcm2835_rng200_apply_faults(BCM2835RngState *s)
{
    if (s->rng200_nist_fail) {
        s->int_status |= RNG200_INT_NIST_FAIL;
    }
    if (s->rng200_master_fail) {
        s->int_status |= RNG200_INT_MASTER_FAIL_LOCKOUT;
    }
}

static bool bcm2835_rng200_failed(BCM2835RngState *s)
{
    return s->int_status &
        (RNG200_INT_NIST_FAIL | RNG200_INT_MASTER_FAIL_LOCKOUT);
}

static void bcm2835_rng200_enable(BCM2835RngState *s)
{
    s->total_bit_count = RNG200_WARM_UP_BITS;
    s->int_status |= RNG200_INT_STARTUP_TRANSITIONS_MET;
    bcm2835_rng200_apply_faults(s);
    if (!bcm2835_rng200_failed(s)) {
        bcm2835_rng200_fill_fifo(s);
    } else {
        bcm2835_rng200_update_irq(s);
    }
}

static void bcm2835_rng200_soft_reset(BCM2835RngState *s)
{
    s->rng_ctrl = 0;
    s->total_bit_count = 0;
    s->total_bit_count_threshold = 0;
    s->int_status = 0;
    s->int_enable = 0;
    s->fifo_threshold = 0;
    s->prng_state = s->deterministic_seed;
    fifo8_reset(&s->fifo);
    bcm2835_rng200_update_irq(s);
}

static uint32_t bcm2835_rng200_fifo_read(BCM2835RngState *s)
{
    uint8_t bytes[sizeof(uint32_t)] = { 0 };

    if (fifo8_num_used(&s->fifo) >= sizeof(bytes)) {
        fifo8_pop_buf(&s->fifo, bytes, sizeof(bytes));
    }
    if (s->rng200_refill && (s->rng_ctrl & RNG200_CTRL_ENABLE) &&
        !bcm2835_rng200_failed(s)) {
        bcm2835_rng200_fill_fifo(s);
    }
    return ldl_le_p(bytes);
}

static uint64_t bcm2835_rng_read(void *opaque, hwaddr offset,
                                 unsigned size)
{
    BCM2835RngState *s = (BCM2835RngState *)opaque;
    uint32_t res = 0;

    assert(size == 4);

    if (s->rng200) {
        switch (offset) {
        case 0x00: /* RNG_CTRL */
            return s->rng_ctrl;
        case 0x04: /* RNG_SOFT_RESET */
        case 0x08: /* RBG_SOFT_RESET */
            return 0;
        case 0x0c: /* RNG_TOTAL_BIT_COUNT */
            return s->total_bit_count;
        case 0x10: /* RNG_TOTAL_BIT_COUNT_THRESHOLD */
            return s->total_bit_count_threshold;
        case 0x18: /* RNG_INT_STATUS */
            return s->int_status;
        case 0x1c: /* RNG_INT_ENABLE */
            return s->int_enable;
        case 0x20: /* RNG_FIFO_DATA */
            return bcm2835_rng200_fifo_read(s);
        case 0x24: /* RNG_FIFO_COUNT */
            return (s->fifo_threshold << 8) |
                (fifo8_num_used(&s->fifo) / sizeof(uint32_t));
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "bcm2711_rng200_read: Bad offset %x\n",
                          (int)offset);
            return 0;
        }
    }

    switch (offset) {
    case 0x0:    /* rng_ctrl */
        res = s->rng_ctrl;
        break;
    case 0x4:    /* rng_status */
        res = s->rng_status | (1 << 24);
        break;
    case 0x8:    /* rng_data */
        res = get_random_bytes(s);
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bcm2835_rng_read: Bad offset %x\n",
                      (int)offset);
        res = 0;
        break;
    }

    return res;
}

static void bcm2835_rng_write(void *opaque, hwaddr offset,
                              uint64_t value, unsigned size)
{
    BCM2835RngState *s = (BCM2835RngState *)opaque;

    assert(size == 4);

    if (s->rng200) {
        switch (offset) {
        case 0x00: /* RNG_CTRL */
        {
            bool was_enabled = s->rng_ctrl & RNG200_CTRL_ENABLE;
            bool enabled;

            s->rng_ctrl = value & RNG200_CTRL_MASK;
            enabled = s->rng_ctrl & RNG200_CTRL_ENABLE;
            if (!was_enabled && enabled) {
                bcm2835_rng200_enable(s);
            }
            break;
        }
        case 0x04: /* RNG_SOFT_RESET */
            if (value & 1) {
                bcm2835_rng200_soft_reset(s);
            }
            break;
        case 0x08: /* RBG_SOFT_RESET */
            if (value & 1) {
                s->total_bit_count = 0;
                s->int_status = 0;
                s->prng_state = s->deterministic_seed;
                fifo8_reset(&s->fifo);
                bcm2835_rng200_update_irq(s);
            }
            break;
        case 0x10: /* RNG_TOTAL_BIT_COUNT_THRESHOLD */
            s->total_bit_count_threshold = value;
            bcm2835_rng200_update_threshold(s);
            break;
        case 0x18: /* RNG_INT_STATUS */
            s->int_status &= ~(value & RNG200_INT_MASK);
            bcm2835_rng200_update_irq(s);
            break;
        case 0x1c: /* RNG_INT_ENABLE */
            s->int_enable = value & RNG200_INT_MASK;
            bcm2835_rng200_update_irq(s);
            break;
        case 0x24: /* RNG_FIFO_COUNT threshold */
            s->fifo_threshold = (value >> 8) & 0xff;
            bcm2835_rng200_update_threshold(s);
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "bcm2711_rng200_write: Bad offset %x\n",
                          (int)offset);
            break;
        }
        return;
    }

    switch (offset) {
    case 0x0:    /* rng_ctrl */
        s->rng_ctrl = value;
        break;
    case 0x4:    /* rng_status */
        /* we shouldn't let the guest write to bits [31..20] */
        s->rng_status &= ~0xFFFFF;        /* clear 20 lower bits */
        s->rng_status |= value & 0xFFFFF; /* set them to new value */
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bcm2835_rng_write: Bad offset %x\n",
                      (int)offset);
        break;
    }
}

static const MemoryRegionOps bcm2835_rng_ops = {
    .read = bcm2835_rng_read,
    .write = bcm2835_rng_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static int bcm2835_rng_post_load(void *opaque, int version_id);

static const VMStateDescription vmstate_bcm2835_rng = {
    .name = TYPE_BCM2835_RNG,
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = bcm2835_rng_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(rng_ctrl, BCM2835RngState),
        VMSTATE_UINT32(rng_status, BCM2835RngState),
        VMSTATE_UINT32_V(total_bit_count, BCM2835RngState, 2),
        VMSTATE_UINT32_V(total_bit_count_threshold, BCM2835RngState, 2),
        VMSTATE_UINT32_V(int_status, BCM2835RngState, 2),
        VMSTATE_UINT32_V(int_enable, BCM2835RngState, 2),
        VMSTATE_UINT32_V(fifo_threshold, BCM2835RngState, 2),
        VMSTATE_UINT32_V(prng_state, BCM2835RngState, 2),
        VMSTATE_STRUCT(fifo, BCM2835RngState, 2, vmstate_fifo8, Fifo8),
        VMSTATE_END_OF_LIST()
    }
};

static void bcm2835_rng_init(Object *obj)
{
    BCM2835RngState *s = BCM2835_RNG(obj);

    memory_region_init_io(&s->iomem, obj, &bcm2835_rng_ops, s,
                          TYPE_BCM2835_RNG, 0x28);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    fifo8_create(&s->fifo, RNG200_FIFO_BYTES);
}

static void bcm2835_rng_reset(DeviceState *dev)
{
    BCM2835RngState *s = BCM2835_RNG(dev);

    s->rng_ctrl = 0;
    s->rng_status = 0;
    bcm2835_rng200_soft_reset(s);
}

static int bcm2835_rng_post_load(void *opaque, int version_id)
{
    BCM2835RngState *s = opaque;

    if (version_id < 2) {
        s->total_bit_count = 0;
        s->total_bit_count_threshold = 0;
        s->int_status = 0;
        s->int_enable = 0;
        s->fifo_threshold = 0;
        s->prng_state = s->deterministic_seed;
        fifo8_reset(&s->fifo);
        if (s->rng200 && (s->rng_ctrl & RNG200_CTRL_ENABLE)) {
            bcm2835_rng200_enable(s);
        }
    }
    /*
     * The generic Fifo8 vmstate carries head and num without checking them
     * against the capacity this device created.  Inconsistent metadata
     * underflows the free-space calculation that drives the refill, so
     * reject it before any refill or IRQ update runs.
     */
    if (s->fifo.capacity != RNG200_FIFO_BYTES ||
        s->fifo.num > s->fifo.capacity ||
        s->fifo.head >= s->fifo.capacity ||
        s->fifo_threshold > 0xff) {
        return -EINVAL;
    }
    bcm2835_rng200_update_irq(s);
    return 0;
}

static void bcm2835_rng_finalize(Object *obj)
{
    BCM2835RngState *s = BCM2835_RNG(obj);

    fifo8_destroy(&s->fifo);
}

static void bcm2835_rng_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    static const Property properties[] = {
        DEFINE_PROP_BOOL("rng200", BCM2835RngState, rng200, false),
        DEFINE_PROP_BOOL("rng200-refill", BCM2835RngState,
                         rng200_refill, true),
        DEFINE_PROP_BOOL("rng200-nist-fail", BCM2835RngState,
                         rng200_nist_fail, false),
        DEFINE_PROP_BOOL("rng200-master-fail", BCM2835RngState,
                         rng200_master_fail, false),
        DEFINE_PROP_UINT32("rng200-deterministic-seed", BCM2835RngState,
                           deterministic_seed, 0),
    };

    device_class_set_legacy_reset(dc, bcm2835_rng_reset);
    device_class_set_props(dc, properties);
    dc->vmsd = &vmstate_bcm2835_rng;
}

static const TypeInfo bcm2835_rng_info = {
    .name          = TYPE_BCM2835_RNG,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2835RngState),
    .class_init    = bcm2835_rng_class_init,
    .instance_init = bcm2835_rng_init,
    .instance_finalize = bcm2835_rng_finalize,
};

static void bcm2835_rng_register_types(void)
{
    type_register_static(&bcm2835_rng_info);
}

type_init(bcm2835_rng_register_types)
