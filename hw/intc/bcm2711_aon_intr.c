/*
 * BCM2711 always-on level-2 interrupt controller
 *
 * This is the edge-latched Broadcom L2 controller used for HDMI CEC and
 * hot-plug interrupts.  Its register contract is consumed by Linux
 * irq-brcmstb-l2.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/intc/bcm2711_aon_intr.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define AON_CPU_STATUS      0x00
#define AON_CPU_CLEAR       0x08
#define AON_CPU_MASK_STATUS 0x0c
#define AON_CPU_MASK_SET    0x10
#define AON_CPU_MASK_CLEAR  0x14

static void bcm2711_aon_intr_update(BCM2711AONIntrState *s)
{
    qemu_set_irq(s->irq, !!(s->status & ~s->mask));
}

static void bcm2711_aon_intr_set_irq(void *opaque, int irq, int level)
{
    BCM2711AONIntrState *s = opaque;
    uint32_t bit = BIT(irq);

    if (level && !(s->levels & bit)) {
        s->status |= bit;
    }
    if (level) {
        s->levels |= bit;
    } else {
        s->levels &= ~bit;
    }
    bcm2711_aon_intr_update(s);
}

static uint64_t bcm2711_aon_intr_read(void *opaque, hwaddr offset,
                                      unsigned int size)
{
    BCM2711AONIntrState *s = opaque;

    switch (offset) {
    case AON_CPU_STATUS:
        return s->status;
    case AON_CPU_MASK_STATUS:
        return s->mask;
    case AON_CPU_CLEAR:
    case AON_CPU_MASK_SET:
    case AON_CPU_MASK_CLEAR:
        return 0;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      TYPE_BCM2711_AON_INTR, offset);
        return 0;
    }
}

static void bcm2711_aon_intr_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned int size)
{
    BCM2711AONIntrState *s = opaque;
    uint32_t bits = value;

    switch (offset) {
    case AON_CPU_CLEAR:
        s->status &= ~bits;
        break;
    case AON_CPU_MASK_SET:
        s->mask |= bits;
        break;
    case AON_CPU_MASK_CLEAR:
        s->mask &= ~bits;
        break;
    case AON_CPU_STATUS:
    case AON_CPU_MASK_STATUS:
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      TYPE_BCM2711_AON_INTR, offset);
        return;
    }
    bcm2711_aon_intr_update(s);
}

static const MemoryRegionOps bcm2711_aon_intr_ops = {
    .read = bcm2711_aon_intr_read,
    .write = bcm2711_aon_intr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void bcm2711_aon_intr_reset(DeviceState *dev)
{
    BCM2711AONIntrState *s = BCM2711_AON_INTR(dev);

    s->status = 0;
    s->mask = UINT32_MAX;
    s->levels = 0;
    bcm2711_aon_intr_update(s);
}

static int bcm2711_aon_intr_post_load(void *opaque, int version_id)
{
    bcm2711_aon_intr_update(opaque);
    return 0;
}

static const VMStateDescription vmstate_bcm2711_aon_intr = {
    .name = TYPE_BCM2711_AON_INTR,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = bcm2711_aon_intr_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(status, BCM2711AONIntrState),
        VMSTATE_UINT32(mask, BCM2711AONIntrState),
        VMSTATE_UINT32(levels, BCM2711AONIntrState),
        VMSTATE_END_OF_LIST()
    }
};

static void bcm2711_aon_intr_init(Object *obj)
{
    BCM2711AONIntrState *s = BCM2711_AON_INTR(obj);

    memory_region_init_io(&s->iomem, obj, &bcm2711_aon_intr_ops, s,
                          TYPE_BCM2711_AON_INTR, BCM2711_AON_INTR_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_in(DEVICE(obj), bcm2711_aon_intr_set_irq,
                      BCM2711_AON_INTR_LINES);
}

static void bcm2711_aon_intr_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_bcm2711_aon_intr;
    device_class_set_legacy_reset(dc, bcm2711_aon_intr_reset);
}

static const TypeInfo bcm2711_aon_intr_info = {
    .name = TYPE_BCM2711_AON_INTR,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2711AONIntrState),
    .instance_init = bcm2711_aon_intr_init,
    .class_init = bcm2711_aon_intr_class_init,
};

static void bcm2711_aon_intr_register_types(void)
{
    type_register_static(&bcm2711_aon_intr_info);
}

type_init(bcm2711_aon_intr_register_types)
