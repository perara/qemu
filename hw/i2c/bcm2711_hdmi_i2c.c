/*
 * BCM2711 HDMI DDC BSC controller
 *
 * This models the register contract consumed by Linux i2c-brcmstb for the
 * two dedicated HDMI DDC buses.  The sink at 0x50 exposes the same validated
 * connector EDID bytes used by the Raspberry Pi firmware property interface.
 * HDMI 2.0 sinks that advertise SCDC in their HF-VSDB also expose the standard
 * SCDC register protocol at DDC address 0x54.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/display/bcm2711_hdmi.h"
#include "hw/i2c/bcm2711_hdmi_i2c.h"
#include "hw/misc/bcm2835_property.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define BSC_CHIP_ADDRESS 0x00
#define BSC_DATA_IN_BASE 0x04
#define BSC_COUNT        0x24
#define BSC_CONTROL      0x28
#define BSC_IIC_ENABLE_REG 0x2c
#define BSC_DATA_OUT_BASE 0x30
#define BSC_CONTROL_HIGH 0x50
#define BSC_SCL_PARAM    0x54

#define BSC_COUNT_MASK       0x3f
#define BSC_CONTROL_DTF_MASK 0x03
#define BSC_CONTROL_DTF_READ 0x01
#define BSC_IIC_NOACK        BIT(2)
#define BSC_IIC_INTERRUPT    BIT(1)
#define BSC_IIC_ENABLE_COMMAND BIT(0)
#define BSC_CONTROL_HIGH_DATAREG_SIZE BIT(6)

#define AUTO_I2C_CONTROL0 0x26c
#define AUTO_I2C_RELEASE_BSC BIT(1)

#define DDC_SEGMENT_ADDRESS 0x30
#define DDC_EDID_ADDRESS    0x50
#define DDC_SCDC_ADDRESS    0x54

#define SCDC_SINK_VERSION   0x01
#define SCDC_SOURCE_VERSION 0x02
#define SCDC_TMDS_CONFIG    0x20
#define SCDC_SCRAMBLER_STATUS 0x21
#define SCDC_CONFIG_0       0x30
#define SCDC_STATUS_FLAGS_0 0x40

#define SCDC_SCRAMBLING_ENABLE BIT(0)
#define SCDC_TMDS_BIT_CLOCK_RATIO_BY_40 BIT(1)
#define SCDC_TMDS_CONFIG_MASK \
    (SCDC_SCRAMBLING_ENABLE | SCDC_TMDS_BIT_CLOCK_RATIO_BY_40)
#define SCDC_READ_REQUEST_ENABLE BIT(0)
#define SCDC_STATUS_CONNECTED 0x0f

#define CTA_EXTENSION_TAG 0x02
#define CTA_VENDOR_DATA_BLOCK 3
#define HDMI_FORUM_OUI_0 0xd8
#define HDMI_FORUM_OUI_1 0x5d
#define HDMI_FORUM_OUI_2 0xc4
#define HDMI_FORUM_SCDC_PRESENT BIT(7)
#define HDMI_FORUM_READ_REQUEST BIT(6)

static uint8_t bcm2711_hdmi_i2c_data_byte(const uint32_t data[8],
                                           unsigned int index)
{
    return extract32(data[index / 4], (index % 4) * 8, 8);
}

static void bcm2711_hdmi_i2c_set_data_byte(uint32_t data[8],
                                           unsigned int index,
                                           uint8_t value)
{
    data[index / 4] = deposit32(data[index / 4],
                                (index % 4) * 8, 8, value);
}

static uint8_t bcm2711_hdmi_i2c_scdc_capabilities(const uint8_t *edid,
                                                   size_t size)
{
    unsigned int extensions;

    if (size < 128) {
        return false;
    }

    extensions = MIN((size_t)edid[126], size / 128 - 1);
    for (unsigned int extension = 0; extension < extensions; extension++) {
        const uint8_t *cta = edid + (extension + 1) * 128;
        unsigned int end;

        if (cta[0] != CTA_EXTENSION_TAG) {
            continue;
        }
        end = cta[2] ? cta[2] : 127;
        if (end < 4 || end > 127) {
            continue;
        }

        for (unsigned int offset = 4; offset < end; ) {
            unsigned int length = cta[offset] & 0x1f;
            unsigned int tag = cta[offset] >> 5;

            if (!length || offset + 1 + length > end) {
                break;
            }
            if (tag == CTA_VENDOR_DATA_BLOCK && length >= 7 &&
                cta[offset + 1] == HDMI_FORUM_OUI_0 &&
                cta[offset + 2] == HDMI_FORUM_OUI_1 &&
                cta[offset + 3] == HDMI_FORUM_OUI_2 &&
                (cta[offset + 6] & HDMI_FORUM_SCDC_PRESENT)) {
                return cta[offset + 6];
            }
            offset += length + 1;
        }
    }
    return 0;
}

static uint8_t bcm2711_hdmi_i2c_scdc_read(BCM2711HDMII2CState *s,
                                          uint8_t offset)
{
    switch (offset) {
    case SCDC_SINK_VERSION:
        return 1;
    case SCDC_SOURCE_VERSION:
        return s->scdc_source_version;
    case SCDC_TMDS_CONFIG:
        return s->scdc_tmds_config;
    case SCDC_SCRAMBLER_STATUS:
        return s->scdc_tmds_config & SCDC_SCRAMBLING_ENABLE;
    case SCDC_CONFIG_0:
        return s->scdc_config;
    case SCDC_STATUS_FLAGS_0:
        return SCDC_STATUS_CONNECTED;
    default:
        return 0;
    }
}

static void bcm2711_hdmi_i2c_scdc_write(BCM2711HDMII2CState *s,
                                        uint8_t offset, uint8_t value,
                                        bool read_request)
{
    switch (offset) {
    case SCDC_SOURCE_VERSION:
        s->scdc_source_version = MIN(value, 1);
        break;
    case SCDC_TMDS_CONFIG:
        s->scdc_tmds_config = value & SCDC_TMDS_CONFIG_MASK;
        break;
    case SCDC_CONFIG_0:
        s->scdc_config = read_request ?
                         value & SCDC_READ_REQUEST_ENABLE : 0;
        break;
    default:
        break;
    }
}

static bool bcm2711_hdmi_i2c_transfer(BCM2711HDMII2CState *s)
{
    const uint8_t *edid;
    size_t edid_size;
    unsigned int count = MIN(s->count & BSC_COUNT_MASK,
                             (uint32_t)sizeof(s->data_in));
    uint8_t address = (s->chip_address >> 1) & 0x7f;
    bool read = (s->control & BSC_CONTROL_DTF_MASK) ==
                BSC_CONTROL_DTF_READ;

    edid_size = bcm2835_property_get_edid(s->property, s->port, &edid);
    memset(s->data_out, 0, sizeof(s->data_out));

    if (!edid_size || !bcm2711_hdmi_is_connected(s->hdmi)) {
        return false;
    }

    if (address == DDC_SEGMENT_ADDRESS && !read) {
        if (count) {
            s->edid_segment = bcm2711_hdmi_i2c_data_byte(s->data_in, 0);
        }
        return true;
    }

    if (address == DDC_SCDC_ADDRESS) {
        uint8_t capabilities =
            bcm2711_hdmi_i2c_scdc_capabilities(edid, edid_size);

        if (!(capabilities & HDMI_FORUM_SCDC_PRESENT)) {
            return false;
        }
        if (!read) {
            if (count) {
                s->scdc_offset =
                    bcm2711_hdmi_i2c_data_byte(s->data_in, 0);
            }
            for (unsigned int index = 1; index < count; index++) {
                bcm2711_hdmi_i2c_scdc_write(
                    s, s->scdc_offset,
                    bcm2711_hdmi_i2c_data_byte(s->data_in, index),
                    capabilities & HDMI_FORUM_READ_REQUEST);
                s->scdc_offset++;
            }
            return true;
        }
        for (unsigned int index = 0; index < count; index++) {
            bcm2711_hdmi_i2c_set_data_byte(
                s->data_out, index,
                bcm2711_hdmi_i2c_scdc_read(s, s->scdc_offset));
            s->scdc_offset++;
        }
        return true;
    }

    if (address != DDC_EDID_ADDRESS) {
        return false;
    }

    if (!read) {
        if (count) {
            s->edid_offset = bcm2711_hdmi_i2c_data_byte(s->data_in, 0);
        }
        return true;
    }

    for (unsigned int index = 0; index < count; index++) {
        uint32_t offset = (uint32_t)s->edid_segment * 256 +
                          s->edid_offset;
        uint8_t value = offset < edid_size ? edid[offset] : 0xff;

        bcm2711_hdmi_i2c_set_data_byte(s->data_out, index, value);
        s->edid_offset++;
    }
    return true;
}

static uint64_t bcm2711_hdmi_i2c_bsc_read(void *opaque, hwaddr offset,
                                          unsigned int size)
{
    BCM2711HDMII2CState *s = opaque;

    if (offset >= BSC_DATA_IN_BASE &&
        offset < BSC_DATA_IN_BASE + sizeof(s->data_in)) {
        return s->data_in[(offset - BSC_DATA_IN_BASE) / sizeof(uint32_t)];
    }
    if (offset >= BSC_DATA_OUT_BASE &&
        offset < BSC_DATA_OUT_BASE + sizeof(s->data_out)) {
        return s->data_out[(offset - BSC_DATA_OUT_BASE) / sizeof(uint32_t)];
    }

    switch (offset) {
    case BSC_CHIP_ADDRESS:
        return s->chip_address;
    case BSC_COUNT:
        return s->count;
    case BSC_CONTROL:
        return s->control;
    case BSC_IIC_ENABLE_REG:
        return s->iic_enable;
    case BSC_CONTROL_HIGH:
        return s->control_high;
    case BSC_SCL_PARAM:
        return s->scl_param;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      TYPE_BCM2711_HDMI_I2C, offset);
        return 0;
    }
}

static void bcm2711_hdmi_i2c_bsc_write(void *opaque, hwaddr offset,
                                       uint64_t value, unsigned int size)
{
    BCM2711HDMII2CState *s = opaque;
    uint32_t word = value;

    if (offset >= BSC_DATA_IN_BASE &&
        offset < BSC_DATA_IN_BASE + sizeof(s->data_in)) {
        s->data_in[(offset - BSC_DATA_IN_BASE) / sizeof(uint32_t)] = word;
        return;
    }

    switch (offset) {
    case BSC_CHIP_ADDRESS:
        s->chip_address = word & 0x7ff;
        break;
    case BSC_COUNT:
        s->count = word & BSC_COUNT_MASK;
        break;
    case BSC_CONTROL:
        s->control = word & 0xff;
        break;
    case BSC_IIC_ENABLE_REG:
        s->iic_enable = word & 0x77;
        if (word & BSC_IIC_ENABLE_COMMAND) {
            s->iic_enable &= ~BSC_IIC_ENABLE_COMMAND;
            s->iic_enable |= BSC_IIC_INTERRUPT;
            if (!bcm2711_hdmi_i2c_transfer(s)) {
                s->iic_enable |= BSC_IIC_NOACK;
            } else {
                s->iic_enable &= ~BSC_IIC_NOACK;
            }
        }
        break;
    case BSC_CONTROL_HIGH:
        s->control_high = word & 0xc3;
        break;
    case BSC_SCL_PARAM:
        s->scl_param = word;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      TYPE_BCM2711_HDMI_I2C, offset);
        break;
    }
}

static uint64_t bcm2711_hdmi_i2c_auto_read(void *opaque, hwaddr offset,
                                           unsigned int size)
{
    BCM2711HDMII2CState *s = opaque;

    if (offset == AUTO_I2C_CONTROL0) {
        return s->auto_control;
    }
    return 0;
}

static void bcm2711_hdmi_i2c_auto_write(void *opaque, hwaddr offset,
                                        uint64_t value, unsigned int size)
{
    BCM2711HDMII2CState *s = opaque;

    if (offset == AUTO_I2C_CONTROL0) {
        s->auto_control = value & AUTO_I2C_RELEASE_BSC;
        if (s->auto_control & AUTO_I2C_RELEASE_BSC) {
            s->iic_enable = 0;
        }
    }
}

static const MemoryRegionOps bcm2711_hdmi_i2c_bsc_ops = {
    .read = bcm2711_hdmi_i2c_bsc_read,
    .write = bcm2711_hdmi_i2c_bsc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static const MemoryRegionOps bcm2711_hdmi_i2c_auto_ops = {
    .read = bcm2711_hdmi_i2c_auto_read,
    .write = bcm2711_hdmi_i2c_auto_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void bcm2711_hdmi_i2c_reset(DeviceState *dev)
{
    BCM2711HDMII2CState *s = BCM2711_HDMI_I2C(dev);

    s->chip_address = 0;
    memset(s->data_in, 0, sizeof(s->data_in));
    s->count = 0;
    s->control = 0;
    s->iic_enable = 0;
    memset(s->data_out, 0, sizeof(s->data_out));
    s->control_high = BSC_CONTROL_HIGH_DATAREG_SIZE;
    s->scl_param = 0;
    s->auto_control = 0;
    s->edid_offset = 0;
    s->edid_segment = 0;
    s->scdc_offset = 0;
    s->scdc_source_version = 0;
    s->scdc_tmds_config = 0;
    s->scdc_config = 0;
}

static void bcm2711_hdmi_i2c_realize(DeviceState *dev, Error **errp)
{
    BCM2711HDMII2CState *s = BCM2711_HDMI_I2C(dev);

    if (!s->property) {
        error_setg(errp, "%s requires a firmware property link",
                   TYPE_BCM2711_HDMI_I2C);
        return;
    }
    if (!s->hdmi) {
        error_setg(errp, "%s requires an HDMI controller link",
                   TYPE_BCM2711_HDMI_I2C);
        return;
    }
    if (s->port >= BCM2835_PROPERTY_EDID_PORT_COUNT) {
        error_setg(errp, "%s port must be 0 or 1",
                   TYPE_BCM2711_HDMI_I2C);
        return;
    }
}

static const VMStateDescription vmstate_bcm2711_hdmi_i2c = {
    .name = TYPE_BCM2711_HDMI_I2C,
    .version_id = 2,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(chip_address, BCM2711HDMII2CState),
        VMSTATE_UINT32_ARRAY(data_in, BCM2711HDMII2CState, 8),
        VMSTATE_UINT32(count, BCM2711HDMII2CState),
        VMSTATE_UINT32(control, BCM2711HDMII2CState),
        VMSTATE_UINT32(iic_enable, BCM2711HDMII2CState),
        VMSTATE_UINT32_ARRAY(data_out, BCM2711HDMII2CState, 8),
        VMSTATE_UINT32(control_high, BCM2711HDMII2CState),
        VMSTATE_UINT32(scl_param, BCM2711HDMII2CState),
        VMSTATE_UINT32(auto_control, BCM2711HDMII2CState),
        VMSTATE_UINT8(edid_offset, BCM2711HDMII2CState),
        VMSTATE_UINT8(edid_segment, BCM2711HDMII2CState),
        VMSTATE_UINT8_V(scdc_offset, BCM2711HDMII2CState, 2),
        VMSTATE_UINT8_V(scdc_source_version, BCM2711HDMII2CState, 2),
        VMSTATE_UINT8_V(scdc_tmds_config, BCM2711HDMII2CState, 2),
        VMSTATE_UINT8_V(scdc_config, BCM2711HDMII2CState, 2),
        VMSTATE_END_OF_LIST()
    }
};

static const Property bcm2711_hdmi_i2c_properties[] = {
    DEFINE_PROP_UINT32("port", BCM2711HDMII2CState, port, 0),
};

static void bcm2711_hdmi_i2c_init(Object *obj)
{
    BCM2711HDMII2CState *s = BCM2711_HDMI_I2C(obj);

    memory_region_init_io(&s->bsc_mmio, obj, &bcm2711_hdmi_i2c_bsc_ops, s,
                          TYPE_BCM2711_HDMI_I2C "-bsc",
                          BCM2711_HDMI_I2C_BSC_SIZE);
    memory_region_init_io(&s->auto_mmio, obj, &bcm2711_hdmi_i2c_auto_ops, s,
                          TYPE_BCM2711_HDMI_I2C "-auto",
                          BCM2711_HDMI_I2C_AUTO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->bsc_mmio);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->auto_mmio);
}

static void bcm2711_hdmi_i2c_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = bcm2711_hdmi_i2c_realize;
    dc->vmsd = &vmstate_bcm2711_hdmi_i2c;
    device_class_set_legacy_reset(dc, bcm2711_hdmi_i2c_reset);
    device_class_set_props(dc, bcm2711_hdmi_i2c_properties);
}

static const TypeInfo bcm2711_hdmi_i2c_info = {
    .name = TYPE_BCM2711_HDMI_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2711HDMII2CState),
    .instance_init = bcm2711_hdmi_i2c_init,
    .class_init = bcm2711_hdmi_i2c_class_init,
};

static void bcm2711_hdmi_i2c_register_types(void)
{
    type_register_static(&bcm2711_hdmi_i2c_info);
}

type_init(bcm2711_hdmi_i2c_register_types)

void bcm2711_hdmi_i2c_set_property(BCM2711HDMII2CState *s,
                                    BCM2835PropertyState *property)
{
    s->property = property;
}

void bcm2711_hdmi_i2c_set_hdmi(BCM2711HDMII2CState *s,
                               BCM2711HDMIState *hdmi)
{
    s->hdmi = hdmi;
}
