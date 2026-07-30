/*
 * Raspberry Pi emulation (c) 2012 Gregory Estrade
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/dma/bcm2835_dma.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"

/* Control blocks followed in a single synchronous dispatch */
#define BCM2835_DMA_MAX_CB_PER_DISPATCH 4096

/* DMA CS Control and Status bits */
#define BCM2708_DMA_ACTIVE      (1 << 0)
#define BCM2708_DMA_END         (1 << 1) /* GE */
#define BCM2708_DMA_INT         (1 << 2)
#define BCM2708_DMA_CS_DREQ     (1 << 3)
#define BCM2708_DMA_ISPAUSED    (1 << 4)  /* Pause requested or not active */
#define BCM2708_DMA_ISHELD      (1 << 5)  /* Is held by DREQ flow control */
#define BCM2708_DMA_ERR         (1 << 8)
#define BCM2708_DMA_ABORT       (1 << 30) /* stop current CB, go to next, WO */
#define BCM2708_DMA_RESET       (1 << 31) /* WO, self clearing */

/* DMA control block "info" field bits */
#define BCM2708_DMA_INT_EN      (1 << 0)
#define BCM2708_DMA_TDMODE      (1 << 1)
#define BCM2708_DMA_WAIT_RESP   (1 << 3)
#define BCM2708_DMA_D_INC       (1 << 4)
#define BCM2708_DMA_D_WIDTH     (1 << 5)
#define BCM2708_DMA_D_DREQ      (1 << 6)
#define BCM2708_DMA_D_IGNORE    (1 << 7)
#define BCM2708_DMA_S_INC       (1 << 8)
#define BCM2708_DMA_S_WIDTH     (1 << 9)
#define BCM2708_DMA_S_DREQ      (1 << 10)
#define BCM2708_DMA_S_IGNORE    (1 << 11)
#define BCM2708_DMA_PER_MAP_SHIFT 16
#define BCM2708_DMA_PER_MAP_MASK  0x1f

/* Register offsets */
#define BCM2708_DMA_CS          0x00 /* Control and Status */
#define BCM2708_DMA_ADDR        0x04 /* Control block address */
/* the current control block appears in the following registers - read only */
#define BCM2708_DMA_INFO        0x08
#define BCM2708_DMA_SOURCE_AD   0x0c
#define BCM2708_DMA_DEST_AD     0x10
#define BCM2708_DMA_TXFR_LEN    0x14
#define BCM2708_DMA_STRIDE      0x18
#define BCM2708_DMA_NEXTCB      0x1C
#define BCM2708_DMA_DEBUG       0x20

#define BCM2708_DMA_INT_STATUS  0xfe0 /* Interrupt status of each channel */
#define BCM2708_DMA_ENABLE      0xff0 /* Global enable bits for each channel */

#define BCM2708_DMA_CS_RW_MASK  0x30ff0001 /* All RW bits in DMA_CS */
#define BCM2708_DMA_PRIORITY_SHIFT 16
#define BCM2708_DMA_PANIC_PRIORITY_SHIFT 20

static bool bcm2835_dma_uses_dreq(const BCM2835DMAChan *ch)
{
    return ch->ti & (BCM2708_DMA_D_DREQ | BCM2708_DMA_S_DREQ);
}

static unsigned bcm2835_dma_permap(const BCM2835DMAChan *ch)
{
    return (ch->ti >> BCM2708_DMA_PER_MAP_SHIFT) &
           BCM2708_DMA_PER_MAP_MASK;
}

static bool bcm2835_dma_load_cb(BCM2835DMAState *s, unsigned c)
{
    BCM2835DMAChan *ch = &s->chan[c];

    ch->ti = ldl_le_phys(&s->dma_as, ch->conblk_ad);
    ch->source_ad = ldl_le_phys(&s->dma_as, ch->conblk_ad + 4);
    ch->dest_ad = ldl_le_phys(&s->dma_as, ch->conblk_ad + 8);
    ch->txfr_len = ldl_le_phys(&s->dma_as, ch->conblk_ad + 12);
    ch->stride = ldl_le_phys(&s->dma_as, ch->conblk_ad + 16);
    ch->nextconbk = ldl_le_phys(&s->dma_as, ch->conblk_ad + 20);

    ch->ylen = 1;
    if (ch->ti & BCM2708_DMA_TDMODE) {
        ch->ylen += (ch->txfr_len >> 16) & 0x3fff;
        ch->xlen_td = ch->txfr_len & 0xffff;
    } else {
        ch->xlen_td = ch->txfr_len;
    }

    if (ch->ti & BCM2708_DMA_D_WIDTH) {
        qemu_log_mask(LOG_UNIMP,
                      "%s: 128bit transfers not yet supported\n", __func__);
        ch->cs |= BCM2708_DMA_ERR;
        return false;
    }

    /*
     * Datasheet implies 32bit or 128bit transfers only.
     *
     * TODO: test on real HW and report back.
     */
    if (ch->xlen_td & 0x3) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad transfer size\n", __func__);
        ch->cs |= BCM2708_DMA_ERR;
        return false;
    }

    ch->cb_loaded = true;
    return true;
}

static bool bcm2835_dma_dreq_ready(BCM2835DMAState *s,
                                   BCM2835DMAChan *ch)
{
    return !bcm2835_dma_uses_dreq(ch) ||
           s->dreq[bcm2835_dma_permap(ch)];
}

static unsigned bcm2835_dma_effective_priority(BCM2835DMAState *s,
                                               BCM2835DMAChan *ch)
{
    unsigned shift = BCM2708_DMA_PRIORITY_SHIFT;

    if (bcm2835_dma_uses_dreq(ch) &&
        s->panic[bcm2835_dma_permap(ch)]) {
        shift = BCM2708_DMA_PANIC_PRIORITY_SHIFT;
    }
    return extract32(ch->cs, shift, 4);
}

static void bcm2835_dma_update(BCM2835DMAState *s, unsigned c)
{
    BCM2835DMAChan *ch = &s->chan[c];
    unsigned int control_blocks = 0;
    uint32_t data;

    if (!(s->enable & (1 << c)) || !(ch->cs & BCM2708_DMA_ACTIVE)) {
        return;
    }

    ch->cs &= ~(BCM2708_DMA_ISPAUSED | BCM2708_DMA_ISHELD);

    while ((s->enable & (1 << c)) && (ch->cs & BCM2708_DMA_ACTIVE) &&
           ch->conblk_ad != 0) {
        uint32_t xlen;

        /*
         * A control block that points back at itself would keep this
         * synchronous loop running forever.  Bound the chain per dispatch
         * and leave the channel active so a legitimate long chain simply
         * continues on the next one.
         */
        if (control_blocks++ >= BCM2835_DMA_MAX_CB_PER_DISPATCH) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: channel %u exceeded its control-block budget\n",
                          __func__, c);
            ch->cs |= BCM2708_DMA_ISHELD;
            return;
        }

        if (!ch->cb_loaded && !bcm2835_dma_load_cb(s, c)) {
            break;
        }

        xlen = ch->ti & BCM2708_DMA_TDMODE ?
               ch->txfr_len & 0xffff : ch->txfr_len;

        while (ch->ylen != 0) {
            /* Normal transfer mode */
            while (xlen != 0) {
                if (!bcm2835_dma_dreq_ready(s, ch)) {
                    ch->cs |= BCM2708_DMA_ISHELD;
                    return;
                }

                if (ch->ti & BCM2708_DMA_S_IGNORE) {
                    /* Ignore reads */
                    data = 0;
                } else {
                    data = ldl_le_phys(&s->dma_as, ch->source_ad);
                }
                if (ch->ti & BCM2708_DMA_S_INC) {
                    ch->source_ad += 4;
                }

                if (ch->ti & BCM2708_DMA_D_IGNORE) {
                    /* Ignore writes */
                } else {
                    stl_le_phys(&s->dma_as, ch->dest_ad, data);
                }
                if (ch->ti & BCM2708_DMA_D_INC) {
                    ch->dest_ad += 4;
                }

                /* update remaining transfer length */
                xlen -= 4;
                if (ch->ti & BCM2708_DMA_TDMODE) {
                    ch->txfr_len = (ch->ylen << 16) | xlen;
                } else {
                    ch->txfr_len = xlen;
                }
            }

            if (--ch->ylen != 0) {
                ch->source_ad += (int16_t)(ch->stride & 0xffff);
                ch->dest_ad += (int16_t)(ch->stride >> 16);
                xlen = ch->xlen_td;
                ch->txfr_len = (ch->ylen << 16) | xlen;
            } else {
                ch->txfr_len = 0;
            }
        }

        ch->cs |= BCM2708_DMA_END;
        if (ch->ti & BCM2708_DMA_INT_EN) {
            ch->cs |= BCM2708_DMA_INT;
            s->int_status |= (1 << c);
            qemu_set_irq(ch->irq, 1);
        }

        /* Process next CB */
        ch->conblk_ad = ch->nextconbk;
        ch->cb_loaded = false;
    }

    ch->cs &= ~BCM2708_DMA_ACTIVE;
    ch->cs |= BCM2708_DMA_ISPAUSED;
}

static void bcm2835_dma_dreq_bh(void *opaque)
{
    BCM2835DMAState *s = opaque;
    uint16_t pending = 0;
    int c;

    for (c = 0; c < BCM2835_DMA_NCHANS; c++) {
        BCM2835DMAChan *ch = &s->chan[c];

        if ((ch->cs & BCM2708_DMA_ACTIVE) && ch->cb_loaded &&
            bcm2835_dma_uses_dreq(ch) &&
            s->dreq[bcm2835_dma_permap(ch)]) {
            pending |= BIT(c);
        }
    }

    while (pending) {
        int selected = -1;
        unsigned selected_priority = 0;

        for (c = 0; c < BCM2835_DMA_NCHANS; c++) {
            unsigned priority;

            if (!(pending & BIT(c))) {
                continue;
            }
            priority = bcm2835_dma_effective_priority(s, &s->chan[c]);
            if (selected < 0 || priority > selected_priority) {
                selected = c;
                selected_priority = priority;
            }
        }
        pending &= ~BIT(selected);
        bcm2835_dma_update(s, selected);
    }
}

static void bcm2835_dma_set_dreq(void *opaque, int n, int level)
{
    BCM2835DMAState *s = opaque;
    bool old_level;

    assert(n >= 0 && n < ARRAY_SIZE(s->dreq));
    old_level = s->dreq[n];
    s->dreq[n] = level;
    if (level && !old_level) {
        qemu_bh_schedule(s->dreq_bh);
    }
}

static void bcm2835_dma_set_panic(void *opaque, int n, int level)
{
    BCM2835DMAState *s = opaque;
    bool old_level;

    assert(n >= 0 && n < ARRAY_SIZE(s->panic));
    old_level = s->panic[n];
    s->panic[n] = level;
    if (old_level != s->panic[n] && s->dreq[n]) {
        qemu_bh_schedule(s->dreq_bh);
    }
}

static void bcm2835_dma_chan_reset(BCM2835DMAChan *ch)
{
    ch->cs = 0;
    ch->conblk_ad = 0;
    ch->ti = 0;
    ch->source_ad = 0;
    ch->dest_ad = 0;
    ch->txfr_len = 0;
    ch->stride = 0;
    ch->nextconbk = 0;
    ch->debug = 0;
    ch->xlen_td = 0;
    ch->ylen = 0;
    ch->cb_loaded = false;
}

static uint64_t bcm2835_dma_read(BCM2835DMAState *s, hwaddr offset,
                                 unsigned size, unsigned c)
{
    BCM2835DMAChan *ch;
    uint32_t res = 0;

    assert(size == 4);
    assert(c < BCM2835_DMA_NCHANS);

    ch = &s->chan[c];

    switch (offset) {
    case BCM2708_DMA_CS:
        res = ch->cs;
        if (ch->cb_loaded &&
            (!bcm2835_dma_uses_dreq(ch) ||
             s->dreq[bcm2835_dma_permap(ch)])) {
            res |= BCM2708_DMA_CS_DREQ;
        }
        break;
    case BCM2708_DMA_ADDR:
        res = ch->conblk_ad;
        break;
    case BCM2708_DMA_INFO:
        res = ch->ti;
        break;
    case BCM2708_DMA_SOURCE_AD:
        res = ch->source_ad;
        break;
    case BCM2708_DMA_DEST_AD:
        res = ch->dest_ad;
        break;
    case BCM2708_DMA_TXFR_LEN:
        res = ch->txfr_len;
        break;
    case BCM2708_DMA_STRIDE:
        res = ch->stride;
        break;
    case BCM2708_DMA_NEXTCB:
        res = ch->nextconbk;
        break;
    case BCM2708_DMA_DEBUG:
        res = ch->debug;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, offset);
        break;
    }
    return res;
}

static void bcm2835_dma_write(BCM2835DMAState *s, hwaddr offset,
                              uint64_t value, unsigned size, unsigned c)
{
    BCM2835DMAChan *ch;
    uint32_t oldcs;

    assert(size == 4);
    assert(c < BCM2835_DMA_NCHANS);

    ch = &s->chan[c];

    switch (offset) {
    case BCM2708_DMA_CS:
        oldcs = ch->cs;
        if (value & BCM2708_DMA_RESET) {
            bcm2835_dma_chan_reset(ch);
        }
        if (value & BCM2708_DMA_ABORT) {
            /* abort is a no-op, since we always run to completion */
        }
        if (value & BCM2708_DMA_END) {
            ch->cs &= ~BCM2708_DMA_END;
        }
        if (value & BCM2708_DMA_INT) {
            ch->cs &= ~BCM2708_DMA_INT;
            s->int_status &= ~(1 << c);
            qemu_set_irq(ch->irq, 0);
        }
        ch->cs &= ~BCM2708_DMA_CS_RW_MASK;
        ch->cs |= (value & BCM2708_DMA_CS_RW_MASK);
        if (!(oldcs & BCM2708_DMA_ACTIVE) && (ch->cs & BCM2708_DMA_ACTIVE)) {
            bcm2835_dma_update(s, c);
        } else if ((oldcs & BCM2708_DMA_ACTIVE) &&
                   !(ch->cs & BCM2708_DMA_ACTIVE)) {
            ch->cs &= ~BCM2708_DMA_ISHELD;
        }
        break;
    case BCM2708_DMA_ADDR:
        ch->conblk_ad = value;
        ch->cb_loaded = false;
        break;
    case BCM2708_DMA_DEBUG:
        ch->debug = value;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, offset);
        break;
    }
}

static uint64_t bcm2835_dma0_read(void *opaque, hwaddr offset, unsigned size)
{
    BCM2835DMAState *s = opaque;

    if (offset < 0xf00) {
        return bcm2835_dma_read(s, (offset & 0xff), size, (offset >> 8) & 0xf);
    } else {
        switch (offset) {
        case BCM2708_DMA_INT_STATUS:
            return s->int_status;
        case BCM2708_DMA_ENABLE:
            return s->enable;
        default:
            qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%"HWADDR_PRIx"\n",
                          __func__, offset);
            return 0;
        }
    }
}

static uint64_t bcm2835_dma15_read(void *opaque, hwaddr offset, unsigned size)
{
    return bcm2835_dma_read(opaque, (offset & 0xff), size, 15);
}

static void bcm2835_dma0_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    BCM2835DMAState *s = opaque;

    if (offset < 0xf00) {
        bcm2835_dma_write(s, (offset & 0xff), value, size, (offset >> 8) & 0xf);
    } else {
        switch (offset) {
        case BCM2708_DMA_INT_STATUS:
            break;
        case BCM2708_DMA_ENABLE:
        {
            uint32_t old_enable = s->enable;

            s->enable = (value & 0xffff);
            for (unsigned c = 0; c < BCM2835_DMA_NCHANS; c++) {
                if (!(old_enable & (1 << c)) &&
                    (s->enable & (1 << c)) &&
                    (s->chan[c].cs & BCM2708_DMA_ACTIVE)) {
                    bcm2835_dma_update(s, c);
                }
            }
            break;
        }
        default:
            qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%"HWADDR_PRIx"\n",
                          __func__, offset);
        }
    }

}

static void bcm2835_dma15_write(void *opaque, hwaddr offset, uint64_t value,
                                unsigned size)
{
    bcm2835_dma_write(opaque, (offset & 0xff), value, size, 15);
}

static const MemoryRegionOps bcm2835_dma0_ops = {
    .read = bcm2835_dma0_read,
    .write = bcm2835_dma0_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static const MemoryRegionOps bcm2835_dma15_ops = {
    .read = bcm2835_dma15_read,
    .write = bcm2835_dma15_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static const VMStateDescription vmstate_bcm2835_dma_chan = {
    .name = TYPE_BCM2835_DMA "-chan",
    .version_id = 2,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cs, BCM2835DMAChan),
        VMSTATE_UINT32(conblk_ad, BCM2835DMAChan),
        VMSTATE_UINT32(ti, BCM2835DMAChan),
        VMSTATE_UINT32(source_ad, BCM2835DMAChan),
        VMSTATE_UINT32(dest_ad, BCM2835DMAChan),
        VMSTATE_UINT32(txfr_len, BCM2835DMAChan),
        VMSTATE_UINT32(stride, BCM2835DMAChan),
        VMSTATE_UINT32(nextconbk, BCM2835DMAChan),
        VMSTATE_UINT32(debug, BCM2835DMAChan),
        VMSTATE_UINT32_V(xlen_td, BCM2835DMAChan, 2),
        VMSTATE_UINT32_V(ylen, BCM2835DMAChan, 2),
        VMSTATE_BOOL_V(cb_loaded, BCM2835DMAChan, 2),
        VMSTATE_END_OF_LIST()
    }
};

static int bcm2835_dma_post_load(void *opaque, int version_id)
{
    BCM2835DMAState *s = opaque;
    bool resume = false;
    int c;

    if (version_id < 3) {
        memset(s->panic, 0, sizeof(s->panic));
    }
    for (c = 0; c < BCM2835_DMA_NCHANS; c++) {
        BCM2835DMAChan *ch = &s->chan[c];

        qemu_set_irq(ch->irq, ch->cs & BCM2708_DMA_INT);
        if (ch->cb_loaded &&
            (!(ch->cs & BCM2708_DMA_ACTIVE) || ch->conblk_ad == 0 ||
             ch->ylen == 0 || ch->xlen_td == 0 ||
             (ch->xlen_td & 3))) {
            return -EINVAL;
        }
        resume |= ch->cb_loaded && bcm2835_dma_uses_dreq(ch) &&
                  s->dreq[bcm2835_dma_permap(ch)];
    }
    if (resume) {
        qemu_bh_schedule(s->dreq_bh);
    }

    return 0;
}

static const VMStateDescription vmstate_bcm2835_dma = {
    .name = TYPE_BCM2835_DMA,
    .version_id = 3,
    .minimum_version_id = 1,
    .post_load = bcm2835_dma_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(chan, BCM2835DMAState, BCM2835_DMA_NCHANS, 1,
                             vmstate_bcm2835_dma_chan, BCM2835DMAChan),
        VMSTATE_UINT32(int_status, BCM2835DMAState),
        VMSTATE_UINT32(enable, BCM2835DMAState),
        VMSTATE_BOOL_ARRAY_V(dreq, BCM2835DMAState, 32, 2),
        VMSTATE_BOOL_ARRAY_V(panic, BCM2835DMAState, 32, 3),
        VMSTATE_END_OF_LIST()
    }
};

static void bcm2835_dma_init(Object *obj)
{
    BCM2835DMAState *s = BCM2835_DMA(obj);
    int n;

    /* DMA channels 0-14 occupy a contiguous block of IO memory, along
     * with the global enable and interrupt status bits. Channel 15
     * has the same register map, but is mapped at a discontiguous
     * address in a separate IO block.
     */
    memory_region_init_io(&s->iomem0, OBJECT(s), &bcm2835_dma0_ops, s,
                          TYPE_BCM2835_DMA, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem0);

    memory_region_init_io(&s->iomem15, OBJECT(s), &bcm2835_dma15_ops, s,
                          TYPE_BCM2835_DMA "-chan15", 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem15);

    for (n = 0; n < 16; n++) {
        sysbus_init_irq(SYS_BUS_DEVICE(s), &s->chan[n].irq);
    }
    qdev_init_gpio_in_named(DEVICE(obj), bcm2835_dma_set_dreq, "dreq", 32);
    qdev_init_gpio_in_named(DEVICE(obj), bcm2835_dma_set_panic, "panic", 32);
    s->dreq_bh = qemu_bh_new_guarded(
        bcm2835_dma_dreq_bh, s, &DEVICE(obj)->mem_reentrancy_guard);
}

static void bcm2835_dma_finalize(Object *obj)
{
    BCM2835DMAState *s = BCM2835_DMA(obj);

    qemu_bh_delete(s->dreq_bh);
}

static void bcm2835_dma_reset(DeviceState *dev)
{
    BCM2835DMAState *s = BCM2835_DMA(dev);
    int n;

    s->enable = 0xffff;
    s->int_status = 0;
    qemu_bh_cancel(s->dreq_bh);
    memset(s->dreq, 0, sizeof(s->dreq));
    memset(s->panic, 0, sizeof(s->panic));
    for (n = 0; n < BCM2835_DMA_NCHANS; n++) {
        qemu_set_irq(s->chan[n].irq, 0);
        bcm2835_dma_chan_reset(&s->chan[n]);
    }
}

static void bcm2835_dma_realize(DeviceState *dev, Error **errp)
{
    BCM2835DMAState *s = BCM2835_DMA(dev);
    Object *obj;

    obj = object_property_get_link(OBJECT(dev), "dma-mr", &error_abort);
    s->dma_mr = MEMORY_REGION(obj);
    address_space_init(&s->dma_as, s->dma_mr, TYPE_BCM2835_DMA "-memory");

    bcm2835_dma_reset(dev);
}

static void bcm2835_dma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = bcm2835_dma_realize;
    device_class_set_legacy_reset(dc, bcm2835_dma_reset);
    dc->vmsd = &vmstate_bcm2835_dma;
}

static const TypeInfo bcm2835_dma_info = {
    .name          = TYPE_BCM2835_DMA,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2835DMAState),
    .class_init    = bcm2835_dma_class_init,
    .instance_init = bcm2835_dma_init,
    .instance_finalize = bcm2835_dma_finalize,
};

static void bcm2835_dma_register_types(void)
{
    type_register_static(&bcm2835_dma_info);
}

type_init(bcm2835_dma_register_types)
