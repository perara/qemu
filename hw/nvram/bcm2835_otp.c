/*
 * BCM2835 One-Time Programmable (OTP) Memory
 *
 * The OTP implementation is mostly a stub except for the OTP rows
 * which are accessed directly by other peripherals such as the mailbox.
 *
 * The OTP registers are unimplemented due to lack of documentation.
 *
 * Copyright (c) 2024 Rayhan Faizel <rayhan.faizel@gmail.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "hw/nvram/bcm2835_otp.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"

#define BCM2835_OTP_BACKING_SIZE 512

static bool bcm2835_otp_sync_row(BCM2835OTPState *s, unsigned int row,
                                 Error **errp)
{
    uint8_t data[sizeof(uint32_t)];
    int ret;

    if (!s->blk) {
        return true;
    }
    stl_le_p(data, s->otp_rows[row - 1]);
    ret = blk_pwrite(s->blk, (row - 1) * sizeof(uint32_t), sizeof(data),
                     data, 0);
    if (ret < 0) {
        error_setg_errno(errp, -ret, "failed to persist BCM2835 OTP row %u",
                         row);
        return false;
    }
    ret = blk_flush(s->blk);
    if (ret < 0) {
        error_setg_errno(errp, -ret, "failed to flush BCM2835 OTP backing");
        return false;
    }
    return true;
}

/* OTP rows are 1-indexed */
uint32_t bcm2835_otp_get_row(BCM2835OTPState *s, unsigned int row)
{
    assert(row <= BCM2835_OTP_ROW_COUNT && row >= 1);

    return s->otp_rows[row - 1];
}

void bcm2835_otp_set_row(BCM2835OTPState *s, unsigned int row,
                           uint32_t value)
{
    uint32_t old_value;
    Error *local_err = NULL;

    assert(row <= BCM2835_OTP_ROW_COUNT && row >= 1);

    /* Real OTP rows work as e-fuses */
    old_value = s->otp_rows[row - 1];
    s->otp_rows[row - 1] |= value;
    if (s->otp_rows[row - 1] != old_value &&
        !bcm2835_otp_sync_row(s, row, &local_err)) {
        error_report_err(local_err);
    }
}

static uint64_t bcm2835_otp_read(void *opaque, hwaddr addr, unsigned size)
{
    switch (addr) {
    case BCM2835_OTP_BOOTMODE_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_BOOTMODE_REG\n");
        break;
    case BCM2835_OTP_CONFIG_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_CONFIG_REG\n");
        break;
    case BCM2835_OTP_CTRL_LO_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_CTRL_LO_REG\n");
        break;
    case BCM2835_OTP_CTRL_HI_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_CTRL_HI_REG\n");
        break;
    case BCM2835_OTP_STATUS_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_STATUS_REG\n");
        break;
    case BCM2835_OTP_BITSEL_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_BITSEL_REG\n");
        break;
    case BCM2835_OTP_DATA_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_DATA_REG\n");
        break;
    case BCM2835_OTP_ADDR_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_ADDR_REG\n");
        break;
    case BCM2835_OTP_WRITE_DATA_READ_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_WRITE_DATA_READ_REG\n");
        break;
    case BCM2835_OTP_INIT_STATUS_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_INIT_STATUS_REG\n");
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Bad offset 0x%" HWADDR_PRIx "\n", __func__, addr);
    }

    return 0;
}

static void bcm2835_otp_write(void *opaque, hwaddr addr,
                              uint64_t value, unsigned int size)
{
    switch (addr) {
    case BCM2835_OTP_BOOTMODE_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_BOOTMODE_REG\n");
        break;
    case BCM2835_OTP_CONFIG_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_CONFIG_REG\n");
        break;
    case BCM2835_OTP_CTRL_LO_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_CTRL_LO_REG\n");
        break;
    case BCM2835_OTP_CTRL_HI_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_CTRL_HI_REG\n");
        break;
    case BCM2835_OTP_STATUS_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_STATUS_REG\n");
        break;
    case BCM2835_OTP_BITSEL_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_BITSEL_REG\n");
        break;
    case BCM2835_OTP_DATA_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_DATA_REG\n");
        break;
    case BCM2835_OTP_ADDR_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_ADDR_REG\n");
        break;
    case BCM2835_OTP_WRITE_DATA_READ_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_WRITE_DATA_READ_REG\n");
        break;
    case BCM2835_OTP_INIT_STATUS_REG:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_otp: BCM2835_OTP_INIT_STATUS_REG\n");
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Bad offset 0x%" HWADDR_PRIx "\n", __func__, addr);
    }
}

static const MemoryRegionOps bcm2835_otp_ops = {
    .read = bcm2835_otp_read,
    .write = bcm2835_otp_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void bcm2835_otp_realize(DeviceState *dev, Error **errp)
{
    BCM2835OTPState *s = BCM2835_OTP(dev);
    uint8_t backing[BCM2835_OTP_BACKING_SIZE];
    int64_t length;

    memory_region_init_io(&s->iomem, OBJECT(dev), &bcm2835_otp_ops, s,
                          TYPE_BCM2835_OTP, 0x80);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);

    memset(s->otp_rows, 0x00, sizeof(s->otp_rows));
    if (s->blk) {
        length = blk_getlength(s->blk);
        if (length != BCM2835_OTP_BACKING_SIZE) {
            error_setg(errp,
                       "BCM2835 OTP backing must be %zu bytes, got %" PRId64,
                       (size_t)BCM2835_OTP_BACKING_SIZE, length);
            return;
        }
        if (blk_set_perm(s->blk,
                         BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                         BLK_PERM_ALL, errp) < 0) {
            return;
        }
        if (blk_pread(s->blk, 0, sizeof(backing), backing, 0) < 0) {
            error_setg(errp, "failed to read BCM2835 OTP backing");
            return;
        }
        for (unsigned int row = 0; row < BCM2835_OTP_ROW_COUNT; row++) {
            s->otp_rows[row] = ldl_le_p(backing + row * sizeof(uint32_t));
        }
    } else {
        s->otp_rows[BCM2711_OTP_BOARD_REVISION_ROW - 1] = s->board_rev;
    }
}

static const Property bcm2835_otp_properties[] = {
    DEFINE_PROP_DRIVE("drive", BCM2835OTPState, blk),
    DEFINE_PROP_UINT32("board-rev", BCM2835OTPState, board_rev, 0),
};

static const VMStateDescription vmstate_bcm2835_otp = {
    .name = TYPE_BCM2835_OTP,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(otp_rows, BCM2835OTPState, BCM2835_OTP_ROW_COUNT),
        VMSTATE_END_OF_LIST()
    }
};

static void bcm2835_otp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = bcm2835_otp_realize;
    dc->vmsd = &vmstate_bcm2835_otp;
    device_class_set_props(dc, bcm2835_otp_properties);
}

static const TypeInfo bcm2835_otp_info = {
    .name = TYPE_BCM2835_OTP,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2835OTPState),
    .class_init = bcm2835_otp_class_init,
};

static void bcm2835_otp_register_types(void)
{
    type_register_static(&bcm2835_otp_info);
}

type_init(bcm2835_otp_register_types)
