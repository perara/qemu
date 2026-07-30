/*
 * BCM2838 peripherals emulation
 *
 * Copyright (C) 2022 Ovchinnikov Vitalii <vitalii.ovchinnikov@auriga.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/arm/raspi_platform.h"
#include "hw/arm/bcm2838_peripherals.h"

#define CLOCK_ISP_OFFSET        0xc11000
#define CLOCK_ISP_SIZE          0x100
#define BCM2711_AVS_MONITOR_OFFSET 0x15d2000
#define BCM2711_PWM1_OFFSET     0x20c800

/* Lower peripheral base address on the VC (GPU) system bus */
#define BCM2838_VC_PERI_LOW_BASE 0x7c000000

/* Capabilities for SD controller: no DMA, high-speed, default clocks etc. */
#define BCM2835_SDHC_CAPAREG 0x52134b4

static void bcm2838_peripherals_init(Object *obj)
{
    BCM2838PeripheralState *s = BCM2838_PERIPHERALS(obj);
    BCM2838PeripheralClass *bc = BCM2838_PERIPHERALS_GET_CLASS(obj);
    BCMSocPeripheralBaseState *s_base = BCM_SOC_PERIPHERALS_BASE(obj);

    s_base->cprman.vpu_clock_reset_hz = 250000000;

    /* Lower memory region for peripheral devices (exported to the Soc) */
    memory_region_init(&s->peri_low_mr, obj, "bcm2838-peripherals",
                       bc->peri_low_size);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->peri_low_mr);

    /* Extended Mass Media Controller 2 */
    object_initialize_child(obj, "emmc2", &s->emmc2, TYPE_SYSBUS_SDHCI);

    /* BCM2711 RNG200 */
    object_initialize_child(obj, "rng200", &s->rng, TYPE_BCM2835_RNG);
    object_property_set_bool(OBJECT(&s->rng), "rng200", true, &error_abort);

    /* BCM2711 AVS ring-oscillator thermal monitor. */
    object_initialize_child(obj, "avs-monitor", &s->thermal2711,
                            TYPE_BCM2835_THERMAL);
    object_property_set_bool(OBJECT(&s->thermal2711), "bcm2711", true,
                             &error_abort);
    bcm2835_property_set_thermal(&s_base->property, &s->thermal2711);

    /* BCM2711 has a second, independent two-channel PWM controller. */
    object_initialize_child(obj, "pwm1", &s->pwm1, TYPE_BCM2835_PWM);

    /* GPIO */
    object_initialize_child(obj, "gpio", &s->gpio, TYPE_BCM2838_GPIO);

    /* BCM2711 GENET Ethernet controller */
    object_initialize_child(obj, "genet", &s->genet, TYPE_BCM2711_GENET);

    object_initialize_child(obj, "aon-intr", &s->aon_intr,
                            TYPE_BCM2711_AON_INTR);

    for (unsigned int port = 0; port < ARRAY_SIZE(s->hdmi); port++) {
        g_autofree char *name = g_strdup_printf("hdmi%u", port);

        object_initialize_child(obj, name, &s->hdmi[port],
                                TYPE_BCM2711_HDMI);
        object_property_set_uint(OBJECT(&s->hdmi[port]), "port", port,
                                 &error_abort);
        bcm2711_hdmi_set_property(&s->hdmi[port], &s_base->property);
    }

    for (unsigned int port = 0; port < ARRAY_SIZE(s->hdmi_i2c); port++) {
        g_autofree char *name = g_strdup_printf("hdmi%u-ddc", port);

        object_initialize_child(obj, name, &s->hdmi_i2c[port],
                                TYPE_BCM2711_HDMI_I2C);
        object_property_set_uint(OBJECT(&s->hdmi_i2c[port]), "port", port,
                                 &error_abort);
        bcm2711_hdmi_i2c_set_property(&s->hdmi_i2c[port],
                                      &s_base->property);
        bcm2711_hdmi_i2c_set_hdmi(&s->hdmi_i2c[port], &s->hdmi[port]);
    }

    object_property_add_const_link(OBJECT(&s->gpio), "sdbus-sdhci",
                                   OBJECT(&s_base->sdhci.sdbus));
    object_property_add_const_link(OBJECT(&s->gpio), "sdbus-sdhost",
                                   OBJECT(&s_base->sdhost.sdbus));
    object_property_add_const_link(OBJECT(&s->gpio), "powermgt",
                                   OBJECT(&s_base->powermgt));

    object_initialize_child(obj, "mmc_irq_orgate", &s->mmc_irq_orgate,
                            TYPE_OR_IRQ);
    object_property_set_int(OBJECT(&s->mmc_irq_orgate), "num-lines", 2,
                            &error_abort);

    object_initialize_child(obj, "dma_7_8_irq_orgate", &s->dma_7_8_irq_orgate,
                            TYPE_OR_IRQ);
    object_property_set_int(OBJECT(&s->dma_7_8_irq_orgate), "num-lines", 2,
                            &error_abort);

    object_initialize_child(obj, "dma_9_10_irq_orgate", &s->dma_9_10_irq_orgate,
                            TYPE_OR_IRQ);
    object_property_set_int(OBJECT(&s->dma_9_10_irq_orgate), "num-lines", 2,
                            &error_abort);
}

static void bcm2838_peripherals_realize(DeviceState *dev, Error **errp)
{
    DeviceState *mmc_irq_orgate;
    DeviceState *dma_7_8_irq_orgate;
    DeviceState *dma_9_10_irq_orgate;
    MemoryRegion *mphi_mr;
    BCM2838PeripheralState *s = BCM2838_PERIPHERALS(dev);
    BCMSocPeripheralBaseState *s_base = BCM_SOC_PERIPHERALS_BASE(dev);
    int n;

    bcm_soc_peripherals_common_realize(dev, errp);

    qdev_connect_clock_in(DEVICE(&s->pwm1), "clk",
                          qdev_get_clock_out(DEVICE(&s_base->cprman),
                                             "pwm-out"));
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->pwm1), errp)) {
        return;
    }
    memory_region_add_subregion(
        &s_base->peri_mr, BCM2711_PWM1_OFFSET,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->pwm1), 0));
    qdev_connect_gpio_out_named(
        DEVICE(&s->pwm1), "dma-threshold", 0,
        qdev_get_gpio_in_named(DEVICE(&s_base->dma), "dreq",
                               BCM2711_DMA_DREQ_PWM1));
    qdev_connect_gpio_out_named(
        DEVICE(&s->pwm1), "dma-threshold", 1,
        qdev_get_gpio_in_named(DEVICE(&s_base->dma), "panic",
                               BCM2711_DMA_DREQ_PWM1));

    /* BCM2711 RNG200 */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->rng), errp)) {
        return;
    }
    memory_region_add_subregion(
        &s_base->peri_mr, RNG_OFFSET,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->rng), 0));

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->thermal2711), errp)) {
        return;
    }
    memory_region_add_subregion(
        &s->peri_low_mr, BCM2711_AVS_MONITOR_OFFSET,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->thermal2711), 0));

    qemu_configure_nic_device(DEVICE(&s->genet), true, NULL);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->genet), errp)) {
        return;
    }
    memory_region_add_subregion(
        &s->peri_low_mr, BCM2711_GENET_OFFSET,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->genet), 0));

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->aon_intr), errp)) {
        return;
    }
    memory_region_add_subregion(
        &s_base->peri_mr, BCM2711_AON_INTR_OFFSET,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->aon_intr), 0));

    for (unsigned int port = 0; port < ARRAY_SIZE(s->hdmi); port++) {
        static const hwaddr hdmi_offsets[] = {
            BCM2711_HDMI0_OFFSET,
            BCM2711_HDMI1_OFFSET,
        };
        static const hwaddr cec_offsets[] = {
            BCM2711_HDMI0_CEC_OFFSET,
            BCM2711_HDMI1_CEC_OFFSET,
        };
        static const unsigned int cec_tx_irqs[] = { 0, 8 };
        static const unsigned int cec_rx_irqs[] = { 1, 7 };
        static const unsigned int connected_irqs[] = { 4, 10 };
        static const unsigned int removed_irqs[] = { 5, 11 };

        if (!sysbus_realize(SYS_BUS_DEVICE(&s->hdmi[port]), errp)) {
            return;
        }
        memory_region_add_subregion(
            &s_base->peri_mr, hdmi_offsets[port],
            sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->hdmi[port]), 0));
        memory_region_add_subregion(
            &s_base->peri_mr, cec_offsets[port],
            sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->hdmi[port]), 1));
        sysbus_connect_irq(
            SYS_BUS_DEVICE(&s->hdmi[port]), 0,
            qdev_get_gpio_in(DEVICE(&s->aon_intr), connected_irqs[port]));
        sysbus_connect_irq(
            SYS_BUS_DEVICE(&s->hdmi[port]), 1,
            qdev_get_gpio_in(DEVICE(&s->aon_intr), removed_irqs[port]));
        sysbus_connect_irq(
            SYS_BUS_DEVICE(&s->hdmi[port]), 2,
            qdev_get_gpio_in(DEVICE(&s->aon_intr), cec_tx_irqs[port]));
        sysbus_connect_irq(
            SYS_BUS_DEVICE(&s->hdmi[port]), 3,
            qdev_get_gpio_in(DEVICE(&s->aon_intr), cec_rx_irqs[port]));
    }

    for (unsigned int port = 0; port < ARRAY_SIZE(s->hdmi_i2c); port++) {
        static const hwaddr bsc_offsets[] = {
            BCM2711_HDMI0_I2C_OFFSET,
            BCM2711_HDMI1_I2C_OFFSET,
        };
        static const hwaddr auto_offsets[] = {
            BCM2711_HDMI0_AUTO_I2C_OFFSET,
            BCM2711_HDMI1_AUTO_I2C_OFFSET,
        };

        if (!sysbus_realize(SYS_BUS_DEVICE(&s->hdmi_i2c[port]), errp)) {
            return;
        }
        memory_region_add_subregion(
            &s_base->peri_mr, bsc_offsets[port],
            sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->hdmi_i2c[port]), 0));
        memory_region_add_subregion(
            &s_base->peri_mr, auto_offsets[port],
            sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->hdmi_i2c[port]), 1));
    }

    /* Map lower peripherals into the GPU address space */
    memory_region_init_alias(&s->peri_low_mr_alias, OBJECT(s),
                             "bcm2838-peripherals", &s->peri_low_mr, 0,
                             memory_region_size(&s->peri_low_mr));
    memory_region_add_subregion_overlap(&s_base->gpu_bus_mr,
                                        BCM2838_VC_PERI_LOW_BASE,
                                        &s->peri_low_mr_alias, 1);

    /* Extended Mass Media Controller 2 */
    object_property_set_uint(OBJECT(&s->emmc2), "sd-spec-version", 3,
                             &error_abort);
    object_property_set_uint(OBJECT(&s->emmc2), "capareg",
                             BCM2835_SDHC_CAPAREG, &error_abort);
    object_property_set_bool(OBJECT(&s->emmc2), "pending-insert-quirk", true,
                             &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->emmc2), errp)) {
        return;
    }

    memory_region_add_subregion(&s_base->peri_mr, EMMC2_OFFSET,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->emmc2),
                                0));

    /* According to DTS, EMMC and EMMC2 share one irq */
    if (!qdev_realize(DEVICE(&s->mmc_irq_orgate), NULL, errp)) {
        return;
    }

    mmc_irq_orgate = DEVICE(&s->mmc_irq_orgate);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->emmc2), 0,
                       qdev_get_gpio_in(mmc_irq_orgate, 0));

    sysbus_connect_irq(SYS_BUS_DEVICE(&s_base->sdhci), 0,
                       qdev_get_gpio_in(mmc_irq_orgate, 1));

   /* Connect EMMC and EMMC2 to the interrupt controller */
    qdev_connect_gpio_out(mmc_irq_orgate, 0,
                          qdev_get_gpio_in_named(DEVICE(&s_base->ic),
                                                 BCM2835_IC_GPU_IRQ,
                                                 INTERRUPT_ARASANSDIO));

    /* Connect DMA 0-6 to the interrupt controller */
    for (n = 0; n < 7; n++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(&s_base->dma), n,
                           qdev_get_gpio_in_named(DEVICE(&s_base->ic),
                                                  BCM2835_IC_GPU_IRQ,
                                                  GPU_INTERRUPT_DMA0 + n));
    }

   /* According to DTS, DMA 7 and 8 share one irq */
    if (!qdev_realize(DEVICE(&s->dma_7_8_irq_orgate), NULL, errp)) {
        return;
    }
    dma_7_8_irq_orgate = DEVICE(&s->dma_7_8_irq_orgate);

    /* Connect DMA 7-8 to the interrupt controller */
    sysbus_connect_irq(SYS_BUS_DEVICE(&s_base->dma), 7,
                       qdev_get_gpio_in(dma_7_8_irq_orgate, 0));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s_base->dma), 8,
                       qdev_get_gpio_in(dma_7_8_irq_orgate, 1));

    qdev_connect_gpio_out(dma_7_8_irq_orgate, 0,
                          qdev_get_gpio_in_named(DEVICE(&s_base->ic),
                                                 BCM2835_IC_GPU_IRQ,
                                                 GPU_INTERRUPT_DMA7_8));

     /* According to DTS, DMA 9 and 10 share one irq */
    if (!qdev_realize(DEVICE(&s->dma_9_10_irq_orgate), NULL, errp)) {
        return;
    }
    dma_9_10_irq_orgate = DEVICE(&s->dma_9_10_irq_orgate);

   /* Connect DMA 9-10 to the interrupt controller */
    sysbus_connect_irq(SYS_BUS_DEVICE(&s_base->dma), 9,
                       qdev_get_gpio_in(dma_9_10_irq_orgate, 0));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s_base->dma), 10,
                       qdev_get_gpio_in(dma_9_10_irq_orgate, 1));

    qdev_connect_gpio_out(dma_9_10_irq_orgate, 0,
                          qdev_get_gpio_in_named(DEVICE(&s_base->ic),
                                                 BCM2835_IC_GPU_IRQ,
                                                 GPU_INTERRUPT_DMA9_10));

    /* Connect DMA 11-14 to the interrupt controller */
    for (n = 11; n < 15; n++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(&s_base->dma), n,
                           qdev_get_gpio_in_named(DEVICE(&s_base->ic),
                                                  BCM2835_IC_GPU_IRQ,
                                                  GPU_INTERRUPT_DMA11 + n
                                                  - 11));
    }

    /*
     * Connect DMA 15 to the interrupt controller, it is physically removed
     * from other DMA channels and exclusively used by the GPU
     */
    sysbus_connect_irq(SYS_BUS_DEVICE(&s_base->dma), 15,
                        qdev_get_gpio_in_named(DEVICE(&s_base->ic),
                                               BCM2835_IC_GPU_IRQ,
                                               GPU_INTERRUPT_DMA15));

    /* Map MPHI to BCM2838 memory map */
    mphi_mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&s_base->mphi), 0);
    memory_region_init_alias(&s->mphi_mr_alias, OBJECT(s), "mphi", mphi_mr, 0,
                             BCM2838_MPHI_SIZE);
    memory_region_add_subregion(&s_base->peri_mr, BCM2838_MPHI_OFFSET,
                                &s->mphi_mr_alias);

    create_unimp(s_base, &s->clkisp, "bcm2835-clkisp", CLOCK_ISP_OFFSET,
                 CLOCK_ISP_SIZE);

    /* GPIO */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->gpio), errp)) {
        return;
    }
    for (n = 0; n < 2; n++) {
        qdev_connect_gpio_out_named(
            DEVICE(&s_base->pwm), "waveform", n,
            qdev_get_gpio_in_named(DEVICE(&s->gpio), "pwm-input", n));
        qdev_connect_gpio_out_named(
            DEVICE(&s->pwm1), "waveform", n,
            qdev_get_gpio_in_named(DEVICE(&s->gpio), "pwm-input", n + 2));
    }
    memory_region_add_subregion(
        &s_base->peri_mr, GPIO_OFFSET,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->gpio), 0));

    object_property_add_alias(OBJECT(s), "sd-bus", OBJECT(&s->gpio), "sd-bus");

    /* BCM2838 RPiVid ASB must be mapped to prevent kernel crash */
    create_unimp(s_base, &s->asb, "bcm2838-asb", BRDG_OFFSET, 0x24);
}

static void bcm2838_peripherals_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    BCM2838PeripheralClass *bc = BCM2838_PERIPHERALS_CLASS(oc);
    BCMSocPeripheralBaseClass *bc_base = BCM_SOC_PERIPHERALS_BASE_CLASS(oc);

    bc->peri_low_size = 0x2000000;
    bc_base->peri_size = 0x1800000;
    dc->realize = bcm2838_peripherals_realize;
}

static const TypeInfo bcm2838_peripherals_type_info = {
    .name = TYPE_BCM2838_PERIPHERALS,
    .parent = TYPE_BCM_SOC_PERIPHERALS_BASE,
    .instance_size = sizeof(BCM2838PeripheralState),
    .instance_init = bcm2838_peripherals_init,
    .class_size = sizeof(BCM2838PeripheralClass),
    .class_init = bcm2838_peripherals_class_init,
};

static void bcm2838_peripherals_register_types(void)
{
    type_register_static(&bcm2838_peripherals_type_info);
}

type_init(bcm2838_peripherals_register_types)
