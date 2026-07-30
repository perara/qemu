/*
 * BCM2711 always-on level-2 interrupt controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_INTC_BCM2711_AON_INTR_H
#define HW_INTC_BCM2711_AON_INTR_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_BCM2711_AON_INTR "bcm2711-aon-intr"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2711AONIntrState, BCM2711_AON_INTR)

#define BCM2711_AON_INTR_SIZE 0x30
#define BCM2711_AON_INTR_LINES 32

struct BCM2711AONIntrState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t status;
    uint32_t mask;
    uint32_t levels;
};

#endif
