/*
 * BCM2711 HDMI DDC BSC controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_I2C_BCM2711_HDMI_I2C_H
#define HW_I2C_BCM2711_HDMI_I2C_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

typedef struct BCM2835PropertyState BCM2835PropertyState;
typedef struct BCM2711HDMIState BCM2711HDMIState;

#define TYPE_BCM2711_HDMI_I2C "bcm2711-hdmi-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2711HDMII2CState, BCM2711_HDMI_I2C)

#define BCM2711_HDMI_I2C_BSC_SIZE  0x100
#define BCM2711_HDMI_I2C_AUTO_SIZE 0x300

struct BCM2711HDMII2CState {
    SysBusDevice parent_obj;

    MemoryRegion bsc_mmio;
    MemoryRegion auto_mmio;
    BCM2835PropertyState *property;
    BCM2711HDMIState *hdmi;

    uint32_t port;
    uint32_t chip_address;
    uint32_t data_in[8];
    uint32_t count;
    uint32_t control;
    uint32_t iic_enable;
    uint32_t data_out[8];
    uint32_t control_high;
    uint32_t scl_param;
    uint32_t auto_control;
    uint8_t edid_offset;
    uint8_t edid_segment;
    uint8_t scdc_offset;
    uint8_t scdc_source_version;
    uint8_t scdc_tmds_config;
    uint8_t scdc_config;
};

void bcm2711_hdmi_i2c_set_property(BCM2711HDMII2CState *s,
                                    BCM2835PropertyState *property);
void bcm2711_hdmi_i2c_set_hdmi(BCM2711HDMII2CState *s,
                               BCM2711HDMIState *hdmi);

#endif
