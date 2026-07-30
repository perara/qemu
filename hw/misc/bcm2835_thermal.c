/*
 * BCM2835 dummy thermal sensor
 *
 * Copyright (C) 2019 Philippe Mathieu-Daudé
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/misc/bcm2835_thermal.h"
#include "hw/core/registerfields.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

REG32(CTL, 0)
FIELD(CTL, POWER_DOWN, 0, 1)
FIELD(CTL, RESET, 1, 1)
FIELD(CTL, BANDGAP_CTRL, 2, 3)
FIELD(CTL, INTERRUPT_ENABLE, 5, 1)
FIELD(CTL, DIRECT, 6, 1)
FIELD(CTL, INTERRUPT_CLEAR, 7, 1)
FIELD(CTL, HOLD, 8, 10)
FIELD(CTL, RESET_DELAY, 18, 8)
FIELD(CTL, REGULATOR_ENABLE, 26, 1)

REG32(STAT, 4)
FIELD(STAT, DATA, 0, 10)
FIELD(STAT, VALID, 10, 1)
FIELD(STAT, INTERRUPT, 11, 1)

#define THERMAL_OFFSET_C 412
#define THERMAL_COEFF  (-0.538f)

#define BCM2711_AVS_SIZE 0xf00
#define BCM2711_TEMP_STATUS 0x200
#define BCM2711_TEMP_VALID (BIT(16) | BIT(10))
#define BCM2711_TEMP_OFFSET_MC 410040
#define BCM2711_TEMP_SLOPE_MC 487

static uint16_t bcm2835_thermal_temp2adc(int temp_C)
{
    return (temp_C - THERMAL_OFFSET_C) / THERMAL_COEFF;
}

static uint16_t bcm2711_thermal_temp2adc(int temp_mC)
{
    int delta = BCM2711_TEMP_OFFSET_MC - temp_mC;
    int code = delta >= 0 ?
        (delta + BCM2711_TEMP_SLOPE_MC / 2) / BCM2711_TEMP_SLOPE_MC :
        (delta - BCM2711_TEMP_SLOPE_MC / 2) / BCM2711_TEMP_SLOPE_MC;

    return CLAMP(code, 0, 0x3ff);
}

static uint64_t bcm2835_thermal_read(void *opaque, hwaddr addr, unsigned size)
{
    Bcm2835ThermalState *s = BCM2835_THERMAL(opaque);
    uint32_t val = 0;

    if (s->bcm2711) {
        if (addr == BCM2711_TEMP_STATUS && s->sensor_valid) {
            val = BCM2711_TEMP_VALID |
                  bcm2711_thermal_temp2adc(
                      s->temperature_millicelsius);
        }
        return val;
    }

    switch (addr) {
    case A_CTL:
        val = s->ctl;
        break;
    case A_STAT:
        val = FIELD_DP32(
            bcm2835_thermal_temp2adc(s->temperature_millicelsius / 1000),
            STAT, VALID, s->sensor_valid);
        break;
    default:
        /* MemoryRegionOps are aligned, so this can not happen. */
        g_assert_not_reached();
    }
    return val;
}

static void bcm2835_thermal_write(void *opaque, hwaddr addr,
                                  uint64_t value, unsigned size)
{
    Bcm2835ThermalState *s = BCM2835_THERMAL(opaque);

    if (s->bcm2711) {
        return;
    }

    switch (addr) {
    case A_CTL:
        s->ctl = value;
        break;
    case A_STAT:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: write 0x%" PRIx64
                                       " to 0x%" HWADDR_PRIx "\n",
                       __func__, value, addr);
        break;
    default:
        /* MemoryRegionOps are aligned, so this can not happen. */
        g_assert_not_reached();
    }
}

static const MemoryRegionOps bcm2835_thermal_ops = {
    .read = bcm2835_thermal_read,
    .write = bcm2835_thermal_write,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void bcm2835_thermal_reset(DeviceState *dev)
{
    Bcm2835ThermalState *s = BCM2835_THERMAL(dev);

    s->ctl = 0;
}

static void bcm2835_thermal_realize(DeviceState *dev, Error **errp)
{
    Bcm2835ThermalState *s = BCM2835_THERMAL(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &bcm2835_thermal_ops,
                          s, TYPE_BCM2835_THERMAL,
                          s->bcm2711 ? BCM2711_AVS_SIZE : 8);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static const VMStateDescription bcm2835_thermal_vmstate = {
    .name = "bcm2835_thermal",
    .version_id = 2,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctl, Bcm2835ThermalState),
        VMSTATE_INT32_V(temperature_millicelsius,
                        Bcm2835ThermalState, 2),
        VMSTATE_BOOL_V(sensor_valid, Bcm2835ThermalState, 2),
        VMSTATE_END_OF_LIST()
    }
};

static const Property bcm2835_thermal_properties[] = {
    DEFINE_PROP_INT32("temperature-millicelsius", Bcm2835ThermalState,
                      temperature_millicelsius, 25000),
    DEFINE_PROP_BOOL("sensor-valid", Bcm2835ThermalState,
                     sensor_valid, true),
    DEFINE_PROP_BOOL("bcm2711", Bcm2835ThermalState, bcm2711, false),
};

static void bcm2835_thermal_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = bcm2835_thermal_realize;
    device_class_set_legacy_reset(dc, bcm2835_thermal_reset);
    device_class_set_props(dc, bcm2835_thermal_properties);
    dc->vmsd = &bcm2835_thermal_vmstate;
}

static const TypeInfo bcm2835_thermal_info = {
    .name = TYPE_BCM2835_THERMAL,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Bcm2835ThermalState),
    .class_init = bcm2835_thermal_class_init,
};

static void bcm2835_thermal_register_types(void)
{
    type_register_static(&bcm2835_thermal_info);
}

type_init(bcm2835_thermal_register_types)
