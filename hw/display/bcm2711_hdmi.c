/*
 * BCM2711 HDMI controller
 *
 * The initial model exposes the production HDMI hot-plug register and the
 * connected/removed edge outputs routed through the BCM2711 AON interrupt
 * controller.  Connector presence defaults to the same validated EDID state
 * used by the firmware property and DDC models.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/display/bcm2711_hdmi.h"
#include "hw/misc/bcm2835_property.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define HDMI_HOTPLUG 0x1a8
#define HDMI_HOTPLUG_CONNECTED BIT(0)

#define CEC_CNTRL_1 0x10
#define CEC_CNTRL_2 0x14
#define CEC_CNTRL_3 0x18
#define CEC_CNTRL_4 0x1c
#define CEC_CNTRL_5 0x20
#define CEC_TX_DATA_1 0x28
#define CEC_RX_DATA_1 0x38

#define CEC_TX_EOM BIT(31)
#define CEC_TX_STATUS_GOOD BIT(30)
#define CEC_RX_EOM BIT(29)
#define CEC_RX_STATUS_GOOD BIT(28)
#define CEC_REC_WRD_CNT_MASK MAKE_64BIT_MASK(24, 4)
#define CEC_CLEAR_RECEIVE_OFF BIT(21)
#define CEC_START_XMIT_BEGIN BIT(20)
#define CEC_MESSAGE_LENGTH_MASK MAKE_64BIT_MASK(16, 4)
#define CEC_TX_SW_RESET BIT(27)
#define CEC_RX_SW_RESET BIT(26)
#define CEC_RX_CEC_INT BIT(23)

/* One start bit plus ten wire bits per byte, at nominal CEC bit timing. */
#define CEC_START_TIME_NS (5 * SCALE_MS)
#define CEC_BYTE_TIME_NS (24 * SCALE_MS)

static void bcm2711_hdmi_cec_tx_complete(void *opaque)
{
    BCM2711HDMIState *s = opaque;
    uint8_t destination = s->cec_tx_data[0] & 0xf;
    bool acknowledged = destination == 0xf ||
                        (s->cec_peer_address_mask & BIT(destination));

    s->cec_tx_active = false;
    s->cec_control[0] |= CEC_TX_EOM;
    if (s->connected && acknowledged && !s->cec_force_nack) {
        s->cec_control[0] |= CEC_TX_STATUS_GOOD;
    } else {
        s->cec_control[0] &= ~CEC_TX_STATUS_GOOD;
    }
    s->cec_control[4] &= ~CEC_RX_CEC_INT;
    s->cec_tx_count++;
    qemu_irq_pulse(s->cec_tx);
}

static void bcm2711_hdmi_cec_start_tx(BCM2711HDMIState *s)
{
    uint32_t length =
        ((s->cec_control[0] & CEC_MESSAGE_LENGTH_MASK) >> 16) + 1;
    int64_t delay = CEC_START_TIME_NS + length * CEC_BYTE_TIME_NS;

    s->cec_control[0] &= ~(CEC_TX_EOM | CEC_TX_STATUS_GOOD);
    s->cec_control[4] &= ~CEC_RX_CEC_INT;
    s->cec_tx_active = true;
    timer_mod(s->cec_tx_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay);
}

static void bcm2711_hdmi_cec_inject(BCM2711HDMIState *s, bool inject)
{
    uint8_t data[16];

    if (!inject || !s->cec_rx_inject_length) {
        return;
    }
    if (s->cec_rx_inject_length > sizeof(s->cec_rx_data) ||
        !s->connected ||
        (s->cec_control[4] & CEC_RX_SW_RESET)) {
        return;
    }

    stq_le_p(data, s->cec_rx_inject_data[0]);
    stq_le_p(data + 8, s->cec_rx_inject_data[1]);
    memset(s->cec_rx_data, 0, sizeof(s->cec_rx_data));
    for (unsigned int i = 0; i < DIV_ROUND_UP(s->cec_rx_inject_length, 4);
         i++) {
        s->cec_rx_data[i] = ldl_le_p(data + i * 4);
    }
    s->cec_control[0] &= ~(CEC_REC_WRD_CNT_MASK |
                           CEC_RX_EOM | CEC_RX_STATUS_GOOD);
    s->cec_control[0] |=
        (s->cec_rx_inject_length - 1) << 24 |
        CEC_RX_EOM | CEC_RX_STATUS_GOOD;
    s->cec_control[4] |= CEC_RX_CEC_INT;
    qemu_irq_pulse(s->cec_rx);
}

static bool bcm2711_hdmi_cec_get_inject(Object *obj, Error **errp)
{
    return false;
}

static void bcm2711_hdmi_cec_set_inject(Object *obj, bool value, Error **errp)
{
    bcm2711_hdmi_cec_inject(BCM2711_HDMI(obj), value);
}

static bool bcm2711_hdmi_cec_get_force_nack(Object *obj, Error **errp)
{
    return BCM2711_HDMI(obj)->cec_force_nack;
}

static void bcm2711_hdmi_cec_set_force_nack(Object *obj, bool value,
                                             Error **errp)
{
    BCM2711_HDMI(obj)->cec_force_nack = value;
}

static void bcm2711_hdmi_change_connected(BCM2711HDMIState *s,
                                          bool connected, bool notify)
{
    if (s->connected == connected) {
        return;
    }

    s->connected = connected;
    if (notify && qdev_is_realized(DEVICE(s))) {
        qemu_irq_pulse(connected ? s->hpd_connected : s->hpd_removed);
    }
}

static bool bcm2711_hdmi_get_connected(Object *obj, Error **errp)
{
    return BCM2711_HDMI(obj)->connected;
}

static void bcm2711_hdmi_set_connected(Object *obj, bool value, Error **errp)
{
    bcm2711_hdmi_change_connected(BCM2711_HDMI(obj), value, true);
}

bool bcm2711_hdmi_is_connected(const BCM2711HDMIState *s)
{
    return s->connected;
}

static uint64_t bcm2711_hdmi_read(void *opaque, hwaddr offset,
                                  unsigned int size)
{
    BCM2711HDMIState *s = opaque;

    if (offset == HDMI_HOTPLUG) {
        return s->connected ? HDMI_HOTPLUG_CONNECTED : 0;
    }

    qemu_log_mask(LOG_UNIMP,
                  "%s%u: unimplemented read at 0x%" HWADDR_PRIx "\n",
                  TYPE_BCM2711_HDMI, s->port, offset);
    return 0;
}

static void bcm2711_hdmi_write(void *opaque, hwaddr offset,
                               uint64_t value, unsigned int size)
{
    BCM2711HDMIState *s = opaque;

    if (offset == HDMI_HOTPLUG) {
        return;
    }

    qemu_log_mask(LOG_UNIMP,
                  "%s%u: unimplemented write at 0x%" HWADDR_PRIx "\n",
                  TYPE_BCM2711_HDMI, s->port, offset);
}

static const MemoryRegionOps bcm2711_hdmi_ops = {
    .read = bcm2711_hdmi_read,
    .write = bcm2711_hdmi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static uint64_t bcm2711_hdmi_cec_read(void *opaque, hwaddr offset,
                                      unsigned int size)
{
    BCM2711HDMIState *s = opaque;

    if (offset >= CEC_CNTRL_1 && offset <= CEC_CNTRL_5) {
        return s->cec_control[(offset - CEC_CNTRL_1) / 4];
    }
    if (offset >= CEC_TX_DATA_1 && offset < CEC_TX_DATA_1 + 16) {
        return s->cec_tx_data[(offset - CEC_TX_DATA_1) / 4];
    }
    if (offset >= CEC_RX_DATA_1 && offset < CEC_RX_DATA_1 + 16) {
        return s->cec_rx_data[(offset - CEC_RX_DATA_1) / 4];
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "%s%u-cec: bad offset 0x%" HWADDR_PRIx "\n",
                  TYPE_BCM2711_HDMI, s->port, offset);
    return 0;
}

static void bcm2711_hdmi_cec_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned int size)
{
    BCM2711HDMIState *s = opaque;

    if (offset == CEC_CNTRL_1) {
        uint32_t old = s->cec_control[0];
        uint32_t writable = value & ~(CEC_TX_EOM | CEC_TX_STATUS_GOOD |
                                      CEC_RX_EOM | CEC_RX_STATUS_GOOD |
                                      CEC_REC_WRD_CNT_MASK);

        s->cec_control[0] =
            (old & (CEC_TX_EOM | CEC_TX_STATUS_GOOD |
                    CEC_RX_EOM | CEC_RX_STATUS_GOOD |
                    CEC_REC_WRD_CNT_MASK)) | writable;
        if (value & CEC_CLEAR_RECEIVE_OFF) {
            s->cec_control[0] &= ~(CEC_RX_EOM | CEC_RX_STATUS_GOOD |
                                   CEC_REC_WRD_CNT_MASK);
            s->cec_control[4] &= ~CEC_RX_CEC_INT;
        }
        if ((value & CEC_START_XMIT_BEGIN) &&
            !(old & CEC_START_XMIT_BEGIN)) {
            bcm2711_hdmi_cec_start_tx(s);
        }
        return;
    }
    if (offset >= CEC_CNTRL_2 && offset <= CEC_CNTRL_5) {
        unsigned int index = (offset - CEC_CNTRL_1) / 4;

        s->cec_control[index] = value;
        if (offset == CEC_CNTRL_5 &&
            (value & (CEC_TX_SW_RESET | CEC_RX_SW_RESET))) {
            timer_del(s->cec_tx_timer);
            s->cec_tx_active = false;
            s->cec_control[0] &=
                ~(CEC_TX_EOM | CEC_TX_STATUS_GOOD |
                  CEC_RX_EOM | CEC_RX_STATUS_GOOD |
                  CEC_REC_WRD_CNT_MASK);
        }
        return;
    }
    if (offset >= CEC_TX_DATA_1 && offset < CEC_TX_DATA_1 + 16) {
        s->cec_tx_data[(offset - CEC_TX_DATA_1) / 4] = value;
        return;
    }
    if (offset >= CEC_RX_DATA_1 && offset < CEC_RX_DATA_1 + 16) {
        return;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "%s%u-cec: bad offset 0x%" HWADDR_PRIx "\n",
                  TYPE_BCM2711_HDMI, s->port, offset);
}

static const MemoryRegionOps bcm2711_hdmi_cec_ops = {
    .read = bcm2711_hdmi_cec_read,
    .write = bcm2711_hdmi_cec_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void bcm2711_hdmi_reset(DeviceState *dev)
{
    BCM2711HDMIState *s = BCM2711_HDMI(dev);
    const uint8_t *edid;

    s->connected =
        bcm2835_property_get_edid(s->property, s->port, &edid) != 0;
    timer_del(s->cec_tx_timer);
    memset(s->cec_control, 0, sizeof(s->cec_control));
    memset(s->cec_tx_data, 0, sizeof(s->cec_tx_data));
    memset(s->cec_rx_data, 0, sizeof(s->cec_rx_data));
    s->cec_tx_count = 0;
    s->cec_tx_active = false;
}

static void bcm2711_hdmi_realize(DeviceState *dev, Error **errp)
{
    BCM2711HDMIState *s = BCM2711_HDMI(dev);

    if (!s->property) {
        error_setg(errp, "%s requires a firmware property link",
                   TYPE_BCM2711_HDMI);
        return;
    }
    if (s->port >= BCM2835_PROPERTY_EDID_PORT_COUNT) {
        error_setg(errp, "%s port must be 0 or 1", TYPE_BCM2711_HDMI);
        return;
    }
}

static int bcm2711_hdmi_post_load(void *opaque, int version_id)
{
    BCM2711HDMIState *s = opaque;

    if (!s->cec_tx_active) {
        timer_del(s->cec_tx_timer);
    }
    return 0;
}

static const VMStateDescription vmstate_bcm2711_hdmi = {
    .name = TYPE_BCM2711_HDMI,
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = bcm2711_hdmi_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(connected, BCM2711HDMIState),
        VMSTATE_UINT32_ARRAY_V(cec_control, BCM2711HDMIState, 5, 2),
        VMSTATE_UINT32_ARRAY_V(cec_tx_data, BCM2711HDMIState, 4, 2),
        VMSTATE_UINT32_ARRAY_V(cec_rx_data, BCM2711HDMIState, 4, 2),
        VMSTATE_UINT64_ARRAY_V(cec_rx_inject_data, BCM2711HDMIState, 2, 2),
        VMSTATE_UINT32_V(cec_tx_count, BCM2711HDMIState, 2),
        VMSTATE_UINT16_V(cec_peer_address_mask, BCM2711HDMIState, 2),
        VMSTATE_UINT8_V(cec_rx_inject_length, BCM2711HDMIState, 2),
        VMSTATE_BOOL_V(cec_tx_active, BCM2711HDMIState, 2),
        VMSTATE_BOOL_V(cec_force_nack, BCM2711HDMIState, 2),
        VMSTATE_TIMER_PTR_V(cec_tx_timer, BCM2711HDMIState, 2),
        VMSTATE_END_OF_LIST()
    }
};

static const Property bcm2711_hdmi_properties[] = {
    DEFINE_PROP_UINT32("port", BCM2711HDMIState, port, 0),
};

static void bcm2711_hdmi_init(Object *obj)
{
    BCM2711HDMIState *s = BCM2711_HDMI(obj);

    memory_region_init_io(&s->iomem, obj, &bcm2711_hdmi_ops, s,
                          TYPE_BCM2711_HDMI, BCM2711_HDMI_CORE_SIZE);
    memory_region_init_io(&s->cec_iomem, obj, &bcm2711_hdmi_cec_ops, s,
                          TYPE_BCM2711_HDMI "-cec",
                          BCM2711_HDMI_CEC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->cec_iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->hpd_connected);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->hpd_removed);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->cec_tx);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->cec_rx);
    s->cec_tx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                   bcm2711_hdmi_cec_tx_complete, s);
    s->cec_peer_address_mask = BIT(0);
    object_property_add_bool(obj, "connected", bcm2711_hdmi_get_connected,
                             bcm2711_hdmi_set_connected);
    object_property_add_bool(obj, "cec-rx-inject",
                             bcm2711_hdmi_cec_get_inject,
                             bcm2711_hdmi_cec_set_inject);
    object_property_add_uint64_ptr(obj, "cec-rx-data-low",
                                   &s->cec_rx_inject_data[0],
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint64_ptr(obj, "cec-rx-data-high",
                                   &s->cec_rx_inject_data[1],
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint8_ptr(obj, "cec-rx-length",
                                  &s->cec_rx_inject_length,
                                  OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint16_ptr(obj, "cec-peer-address-mask",
                                   &s->cec_peer_address_mask,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_bool(obj, "cec-force-nack",
                             bcm2711_hdmi_cec_get_force_nack,
                             bcm2711_hdmi_cec_set_force_nack);
}

static void bcm2711_hdmi_finalize(Object *obj)
{
    BCM2711HDMIState *s = BCM2711_HDMI(obj);

    timer_free(s->cec_tx_timer);
}

static void bcm2711_hdmi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = bcm2711_hdmi_realize;
    dc->vmsd = &vmstate_bcm2711_hdmi;
    device_class_set_legacy_reset(dc, bcm2711_hdmi_reset);
    device_class_set_props(dc, bcm2711_hdmi_properties);
}

static const TypeInfo bcm2711_hdmi_info = {
    .name = TYPE_BCM2711_HDMI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2711HDMIState),
    .instance_init = bcm2711_hdmi_init,
    .instance_finalize = bcm2711_hdmi_finalize,
    .class_init = bcm2711_hdmi_class_init,
};

static void bcm2711_hdmi_register_types(void)
{
    type_register_static(&bcm2711_hdmi_info);
}

type_init(bcm2711_hdmi_register_types)

void bcm2711_hdmi_set_property(BCM2711HDMIState *s,
                               BCM2835PropertyState *property)
{
    s->property = property;
}
