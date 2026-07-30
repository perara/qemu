/*
 * BCM2835 Power Management emulation
 *
 * Copyright (C) 2017 Marcin Chojnacki <marcinch7@gmail.com>
 * Copyright (C) 2021 Nolan Leake <nolan@sigbus.net>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/misc/bcm2835_powermgt.h"
#include "migration/vmstate.h"
#include "system/runstate.h"

#define PASSWORD 0x5a000000
#define PASSWORD_MASK 0xff000000
#define WDOG_TICKS_MASK 0x000fffff
#define WDOG_TICKS_PER_SECOND 65536

#define R_RSTC 0x1c
#define V_RSTC_RESET 0x20
#define R_RSTS 0x20
#define V_RSTS_HADWRF BIT(5)
#define V_RSTS_POWEROFF 0x555 /* Linux uses partition 63 to indicate halt. */
#define R_WDOG 0x24

static void bcm2835_powermgt_wake(BCM2835PowerMgtState *s)
{
    Error *local_err = NULL;

    if (s->halt_state != BCM2835_HALT_SUSPENDED_GPIO &&
        s->halt_state != BCM2835_HALT_SUSPENDED_GLOBAL_EN) {
        return;
    }
    s->halt_state = BCM2835_HALT_RUNNING;
    qemu_system_wakeup_request(QEMU_WAKEUP_REASON_OTHER, &local_err);
    if (local_err) {
        error_free(local_err);
    }
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
}

static void bcm2835_powermgt_watchdog_expired(void *opaque)
{
    BCM2835PowerMgtState *s = opaque;
    bool poweroff = (s->rsts & 0xfff) == V_RSTS_POWEROFF;

    s->rsts |= V_RSTS_HADWRF;
    if (poweroff) {
        if (!s->wake_on_gpio && s->power_off_on_halt) {
            s->halt_state = BCM2835_HALT_POWERED_OFF;
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        } else {
            s->halt_state = s->wake_on_gpio ?
                BCM2835_HALT_SUSPENDED_GPIO :
                BCM2835_HALT_SUSPENDED_GLOBAL_EN;
            qemu_system_suspend_request();
        }
    } else {
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
}

void bcm2835_powermgt_set_halt_policy(BCM2835PowerMgtState *s,
                                      bool wake_on_gpio,
                                      bool power_off_on_halt)
{
    s->wake_on_gpio = wake_on_gpio;
    s->power_off_on_halt = power_off_on_halt;
}

void bcm2835_powermgt_gpio3_input(BCM2835PowerMgtState *s, int level)
{
    int old_level = s->gpio3_level;

    s->gpio3_level = level;
    if (s->wake_on_gpio && s->halt_state == BCM2835_HALT_SUSPENDED_GPIO &&
        level == 0 && old_level != 0) {
        bcm2835_powermgt_wake(s);
    }
}

void bcm2835_powermgt_global_en_input(BCM2835PowerMgtState *s, bool level)
{
    bool old_level = s->global_en;

    s->global_en = level;
    if (!level && old_level &&
        (s->halt_state == BCM2835_HALT_SUSPENDED_GPIO ||
         s->halt_state == BCM2835_HALT_SUSPENDED_GLOBAL_EN)) {
        bcm2835_powermgt_wake(s);
    }
}

const char *bcm2835_powermgt_halt_state(BCM2835PowerMgtState *s)
{
    static const char * const names[] = {
        [BCM2835_HALT_RUNNING] = "running",
        [BCM2835_HALT_SUSPENDED_GPIO] = "halted-gpio-wake",
        [BCM2835_HALT_SUSPENDED_GLOBAL_EN] = "halted-global-en",
        [BCM2835_HALT_POWERED_OFF] = "powered-off",
    };

    return s->halt_state < ARRAY_SIZE(names) && names[s->halt_state] ?
        names[s->halt_state] : "invalid";
}

static void bcm2835_powermgt_update_watchdog(BCM2835PowerMgtState *s)
{
    uint32_t ticks = s->wdog & WDOG_TICKS_MASK;

    timer_del(s->watchdog_timer);
    if ((s->rstc & 0x30) == V_RSTC_RESET && ticks) {
        int64_t delay = muldiv64(ticks, NANOSECONDS_PER_SECOND,
                                 WDOG_TICKS_PER_SECOND);

        timer_mod_ns(s->watchdog_timer,
                     qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay);
    }
}

void bcm2835_powermgt_watchdog_start(BCM2835PowerMgtState *s,
                                     uint32_t milliseconds)
{
    uint64_t ticks = DIV_ROUND_UP((uint64_t)milliseconds *
                                  WDOG_TICKS_PER_SECOND, 1000);

    s->wdog = MIN(ticks, (uint64_t)WDOG_TICKS_MASK);
    s->rstc = (s->rstc & ~0x30) | V_RSTC_RESET;
    bcm2835_powermgt_update_watchdog(s);
}

void bcm2835_powermgt_watchdog_trigger(BCM2835PowerMgtState *s)
{
    timer_del(s->watchdog_timer);
    s->wdog = 0;
    bcm2835_powermgt_watchdog_expired(s);
}

static uint64_t bcm2835_powermgt_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    BCM2835PowerMgtState *s = (BCM2835PowerMgtState *)opaque;
    uint32_t res = 0;

    switch (offset) {
    case R_RSTC:
        res = s->rstc;
        break;
    case R_RSTS:
        res = s->rsts;
        break;
    case R_WDOG:
        res = s->wdog;
        break;

    default:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_powermgt_read: Unknown offset 0x%08"HWADDR_PRIx
                      "\n", offset);
        res = 0;
        break;
    }

    return res;
}

static void bcm2835_powermgt_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    BCM2835PowerMgtState *s = (BCM2835PowerMgtState *)opaque;

    if ((value & PASSWORD_MASK) != PASSWORD) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bcm2835_powermgt_write: Bad password 0x%"PRIx64
                      " at offset 0x%08"HWADDR_PRIx"\n",
                      value, offset);
        return;
    }

    value = value & ~PASSWORD_MASK;

    switch (offset) {
    case R_RSTC:
        s->rstc = value;
        bcm2835_powermgt_update_watchdog(s);
        break;
    case R_RSTS:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_powermgt_write: RSTS\n");
        s->rsts = value;
        break;
    case R_WDOG:
        s->wdog = value;
        bcm2835_powermgt_update_watchdog(s);
        break;

    default:
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_powermgt_write: Unknown offset 0x%08"HWADDR_PRIx
                      "\n", offset);
        break;
    }
}

static const MemoryRegionOps bcm2835_powermgt_ops = {
    .read = bcm2835_powermgt_read,
    .write = bcm2835_powermgt_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static int bcm2835_powermgt_pre_save(void *opaque)
{
    BCM2835PowerMgtState *s = opaque;
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t expiry;

    s->watchdog_pending = timer_pending(s->watchdog_timer);
    s->watchdog_remaining_ns = 0;
    if (!s->watchdog_pending) {
        return 0;
    }
    expiry = timer_expire_time_ns(s->watchdog_timer);
    if (expiry > now) {
        s->watchdog_remaining_ns = expiry - now;
    }
    return 0;
}

static int bcm2835_powermgt_post_load(void *opaque, int version_id)
{
    BCM2835PowerMgtState *s = opaque;

    if (version_id < 4) {
        s->wake_on_gpio = true;
        s->power_off_on_halt = false;
        s->halt_state = BCM2835_HALT_RUNNING;
        s->gpio3_level = -1;
        s->global_en = true;
    }
    if (s->halt_state >= BCM2835_HALT_STATE__MAX ||
        (s->halt_state == BCM2835_HALT_SUSPENDED_GPIO &&
         !s->wake_on_gpio) ||
        (s->halt_state == BCM2835_HALT_SUSPENDED_GLOBAL_EN &&
         (s->wake_on_gpio || s->power_off_on_halt)) ||
        (s->halt_state == BCM2835_HALT_POWERED_OFF &&
         (s->wake_on_gpio || !s->power_off_on_halt))) {
        return -EINVAL;
    }
    if (version_id < 3) {
        return 0;
    }
    timer_del(s->watchdog_timer);
    if (s->watchdog_pending) {
        timer_mod_ns(s->watchdog_timer,
                     qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                     s->watchdog_remaining_ns);
    }
    return 0;
}

static const VMStateDescription vmstate_bcm2835_powermgt = {
    .name = TYPE_BCM2835_POWERMGT,
    .version_id = 4,
    .minimum_version_id = 1,
    .pre_save = bcm2835_powermgt_pre_save,
    .post_load = bcm2835_powermgt_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(rstc, BCM2835PowerMgtState),
        VMSTATE_UINT32(rsts, BCM2835PowerMgtState),
        VMSTATE_UINT32(wdog, BCM2835PowerMgtState),
        VMSTATE_TIMER_PTR_V(watchdog_timer, BCM2835PowerMgtState, 2),
        VMSTATE_BOOL_V(watchdog_pending, BCM2835PowerMgtState, 3),
        VMSTATE_UINT64_V(watchdog_remaining_ns, BCM2835PowerMgtState, 3),
        VMSTATE_BOOL_V(wake_on_gpio, BCM2835PowerMgtState, 4),
        VMSTATE_BOOL_V(power_off_on_halt, BCM2835PowerMgtState, 4),
        VMSTATE_UINT8_V(halt_state, BCM2835PowerMgtState, 4),
        VMSTATE_INT8_V(gpio3_level, BCM2835PowerMgtState, 4),
        VMSTATE_BOOL_V(global_en, BCM2835PowerMgtState, 4),
        VMSTATE_END_OF_LIST()
    }
};

static void bcm2835_powermgt_init(Object *obj)
{
    BCM2835PowerMgtState *s = BCM2835_POWERMGT(obj);

    memory_region_init_io(&s->iomem, obj, &bcm2835_powermgt_ops, s,
                          TYPE_BCM2835_POWERMGT, 0x200);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    s->rsts = 0x00001000;
    s->wake_on_gpio = true;
    s->gpio3_level = -1;
    s->global_en = true;
    s->watchdog_timer = timer_new_ns(
        QEMU_CLOCK_VIRTUAL, bcm2835_powermgt_watchdog_expired, s);
}

static void bcm2835_powermgt_finalize(Object *obj)
{
    BCM2835PowerMgtState *s = BCM2835_POWERMGT(obj);

    timer_free(s->watchdog_timer);
}

static void bcm2835_powermgt_reset(DeviceState *dev)
{
    BCM2835PowerMgtState *s = BCM2835_POWERMGT(dev);

    /* https://elinux.org/BCM2835_registers#PM */
    s->rstc = 0x00000102;
    s->wdog = 0x00000000;
    s->halt_state = BCM2835_HALT_RUNNING;
    timer_del(s->watchdog_timer);
}

static void bcm2835_powermgt_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, bcm2835_powermgt_reset);
    dc->vmsd = &vmstate_bcm2835_powermgt;
}

static const TypeInfo bcm2835_powermgt_info = {
    .name          = TYPE_BCM2835_POWERMGT,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2835PowerMgtState),
    .class_init    = bcm2835_powermgt_class_init,
    .instance_init = bcm2835_powermgt_init,
    .instance_finalize = bcm2835_powermgt_finalize,
};

static void bcm2835_powermgt_register_types(void)
{
    type_register_static(&bcm2835_powermgt_info);
}

type_init(bcm2835_powermgt_register_types)
