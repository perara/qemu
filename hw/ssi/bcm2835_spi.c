/*
 * BCM2835 SPI Master Controller
 *
 * Copyright (c) 2024 Rayhan Faizel <rayhan.faizel@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/fifo8.h"
#include "qapi/visitor.h"
#include "hw/ssi/bcm2835_spi.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

static void bcm2835_spi_update_outputs(BCM2835SPIState *s);
static void bcm2835_spi_schedule_transfer(BCM2835SPIState *s);

static void bcm2835_spi_update_int(BCM2835SPIState *s)
{
    int do_interrupt = 0;

    /* Interrupt on DONE */
    if (s->cs & BCM2835_SPI_CS_INTD && s->cs & BCM2835_SPI_CS_DONE) {
        do_interrupt = 1;
    }
    /* Interrupt on RXR */
    if (s->cs & BCM2835_SPI_CS_INTR && s->cs & BCM2835_SPI_CS_RXR) {
        do_interrupt = 1;
    }
    qemu_set_irq(s->irq, do_interrupt);
}

static void bcm2835_spi_update_rx_flags(BCM2835SPIState *s)
{
    /* Set RXD if RX FIFO is non empty */
    if (!fifo8_is_empty(&s->rx_fifo)) {
        s->cs |= BCM2835_SPI_CS_RXD;
    } else {
        s->cs &= ~BCM2835_SPI_CS_RXD;
    }

    /* Set RXF if RX FIFO is full */
    if (fifo8_is_full(&s->rx_fifo)) {
        s->cs |= BCM2835_SPI_CS_RXF;
    } else {
        s->cs &= ~BCM2835_SPI_CS_RXF;
    }

    /* Set RXR if RX FIFO is 3/4th used or above */
    if (fifo8_num_used(&s->rx_fifo) >= FIFO_SIZE_3_4) {
        s->cs |= BCM2835_SPI_CS_RXR;
    } else {
        s->cs &= ~BCM2835_SPI_CS_RXR;
    }
}

static void bcm2835_spi_update_tx_flags(BCM2835SPIState *s)
{
    /* Set TXD if TX FIFO is not full */
    if (fifo8_is_full(&s->tx_fifo)) {
        s->cs &= ~BCM2835_SPI_CS_TXD;
    } else {
        s->cs |= BCM2835_SPI_CS_TXD;
    }

    /*
     * Poll mode completes when the transmit FIFO drains.  DMA mode also
     * requires the programmed DLEN byte count to reach zero.
     */
    if (fifo8_is_empty(&s->tx_fifo) &&
        s->cs & BCM2835_SPI_CS_TA &&
        (!(s->cs & BCM2835_SPI_CS_DMAEN) || s->dma_complete)) {
        s->cs |= BCM2835_SPI_CS_DONE;
    } else {
        s->cs &= ~BCM2835_SPI_CS_DONE;
    }
}

static bool bcm2835_spi_chip_select_active(BCM2835SPIState *s)
{
    return (s->cs & BCM2835_SPI_CS_TA) &&
           (!(s->cs & BCM2835_SPI_CS_DMAEN) ||
            !(s->cs & BCM2835_SPI_CS_ADCS) || !s->dma_complete);
}

static void bcm2835_spi_update_outputs(BCM2835SPIState *s)
{
    unsigned int tx_level = fifo8_num_used(&s->tx_fifo);
    unsigned int rx_level = fifo8_num_used(&s->rx_fifo);
    unsigned int selected = s->cs & BCM2835_SPI_CS_SELECT_MASK;
    bool dma_enabled = s->cs & BCM2835_SPI_CS_DMAEN;
    bool dma_active = s->cs & BCM2835_SPI_CS_TA;
    bool tx_dreq = false;
    bool rx_dreq = false;
    bool tx_panic = false;
    bool rx_panic = false;

    for (unsigned int index = 0; index < 3; index++) {
        bool active_high =
            (s->cs & BCM2835_SPI_CS_CSPOL) ||
            (s->cs & (BCM2835_SPI_CS_CSPOL0 << index));
        bool active = selected == index &&
                      bcm2835_spi_chip_select_active(s);

        qemu_set_irq(s->chip_select[index],
                     active ? active_high : !active_high);
    }

    if (dma_enabled) {
        unsigned int tdreq = extract32(s->dc, 0, 8);
        unsigned int tpanic = extract32(s->dc, 8, 8);
        unsigned int rdreq = extract32(s->dc, 16, 8);
        unsigned int rpanic = extract32(s->dc, 24, 8);

        /*
         * Before TA the TX request asks DMA for its framing word.  Once TA
         * is active, requests are bounded by the remaining DLEN bytes.
         */
        tx_dreq = !dma_active ||
                  (s->dma_remaining && tx_level <= tdreq);
        tx_panic = (!dma_active || s->dma_remaining) &&
                   tx_level <= tpanic;
        rx_dreq = rx_level &&
                  (rx_level > rdreq || s->dma_complete);
        rx_panic = rx_level > rpanic;
    }

    qemu_set_irq(s->dma_threshold[BCM2835_SPI_DMA_TX_DREQ], tx_dreq);
    qemu_set_irq(s->dma_threshold[BCM2835_SPI_DMA_RX_DREQ], rx_dreq);
    qemu_set_irq(s->dma_threshold[BCM2835_SPI_DMA_TX_PANIC], tx_panic);
    qemu_set_irq(s->dma_threshold[BCM2835_SPI_DMA_RX_PANIC], rx_panic);
}

static uint32_t bcm2835_spi_clock_divisor(const BCM2835SPIState *s)
{
    uint32_t divisor = s->clk & 0xffff;

    if (!divisor) {
        return 65536;
    }

    /* Odd divisors are rounded down; two is the fastest legal divisor. */
    return MAX(2U, divisor & ~1U);
}

static uint64_t bcm2835_spi_serial_hz(const BCM2835SPIState *s)
{
    return clock_get_hz(s->core_clk) / bcm2835_spi_clock_divisor(s);
}

static void bcm2835_spi_core_clock_get(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    BCM2835SPIState *s = BCM2835_SPI(obj);
    uint32_t frequency = clock_get_hz(s->core_clk);

    visit_type_uint32(v, name, &frequency, errp);
}

static bool bcm2835_spi_transfer_ready(const BCM2835SPIState *s)
{
    return (s->cs & BCM2835_SPI_CS_TA) &&
           !fifo8_is_empty(&s->tx_fifo) &&
           !fifo8_is_full(&s->rx_fifo) &&
           (!(s->cs & BCM2835_SPI_CS_DMAEN) || s->dma_remaining);
}

static void bcm2835_spi_pause_transfer(BCM2835SPIState *s)
{
    uint64_t serial_hz;
    uint64_t now;
    uint64_t expiry;
    uint64_t remaining_ns;
    uint64_t cycles;

    if (!timer_pending(s->transfer_timer)) {
        return;
    }

    serial_hz = bcm2835_spi_serial_hz(s);
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    expiry = timer_expire_time_ns(s->transfer_timer);
    remaining_ns = expiry > now ? expiry - now : 1;
    cycles = serial_hz ?
        DIV_ROUND_UP(remaining_ns * serial_hz,
                     NANOSECONDS_PER_SECOND) : 8;
    s->transfer_remaining_cycles = MIN(8ULL, MAX(1ULL, cycles));
    timer_del(s->transfer_timer);
}

static void bcm2835_spi_schedule_transfer(BCM2835SPIState *s)
{
    uint64_t serial_hz;
    uint64_t cycles;
    uint64_t delay_ns;

    if (!bcm2835_spi_transfer_ready(s) ||
        timer_pending(s->transfer_timer)) {
        return;
    }

    cycles = s->transfer_remaining_cycles ?: 8;
    serial_hz = bcm2835_spi_serial_hz(s);
    if (!serial_hz) {
        s->transfer_remaining_cycles = cycles;
        return;
    }

    delay_ns = MAX(1ULL,
                   DIV_ROUND_UP(cycles * NANOSECONDS_PER_SECOND, serial_hz));
    timer_mod_ns(s->transfer_timer,
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay_ns);
    s->transfer_remaining_cycles = 0;
}

static void bcm2835_spi_transfer(void *opaque)
{
    BCM2835SPIState *s = opaque;
    uint8_t tx_byte;
    uint8_t rx_byte;

    if (bcm2835_spi_transfer_ready(s)) {
        tx_byte = fifo8_pop(&s->tx_fifo);
        rx_byte = ssi_transfer(s->bus, tx_byte);
        fifo8_push(&s->rx_fifo, rx_byte);
        if (s->cs & BCM2835_SPI_CS_DMAEN) {
            s->dma_remaining--;
            if (!s->dma_remaining) {
                s->dma_complete = true;
            }
        }
    }

    bcm2835_spi_update_tx_flags(s);
    bcm2835_spi_update_rx_flags(s);
    bcm2835_spi_update_outputs(s);
    bcm2835_spi_update_int(s);
    bcm2835_spi_schedule_transfer(s);
}

static void bcm2835_spi_core_clk_update(void *opaque, ClockEvent event)
{
    BCM2835SPIState *s = opaque;

    if (event == ClockPreUpdate) {
        bcm2835_spi_pause_transfer(s);
    } else if (event == ClockUpdate) {
        bcm2835_spi_schedule_transfer(s);
    }
}

static uint64_t bcm2835_spi_read(void *opaque, hwaddr addr, unsigned size)
{
    BCM2835SPIState *s = opaque;
    uint32_t readval = 0;

    switch (addr) {
    case BCM2835_SPI_CS:
        readval = s->cs & 0xffffffff;
        break;
    case BCM2835_SPI_FIFO:
        if (s->cs & BCM2835_SPI_CS_DMAEN) {
            for (unsigned int index = 0;
                 index < sizeof(readval) && !fifo8_is_empty(&s->rx_fifo);
                 index++) {
                readval |= (uint32_t)fifo8_pop(&s->rx_fifo) << (index * 8);
            }
        } else if (s->cs & BCM2835_SPI_CS_RXD) {
            readval = fifo8_pop(&s->rx_fifo);
        }
        if (s->cs & BCM2835_SPI_CS_RXD) {
            bcm2835_spi_update_rx_flags(s);
        }
        bcm2835_spi_update_outputs(s);
        bcm2835_spi_schedule_transfer(s);
        bcm2835_spi_update_int(s);
        break;
    case BCM2835_SPI_CLK:
        readval = s->clk & 0xffff;
        break;
    case BCM2835_SPI_DLEN:
        readval = s->dlen & 0xffff;
        break;
    case BCM2835_SPI_LTOH:
        readval = s->ltoh & 0xf;
        break;
    case BCM2835_SPI_DC:
        readval = s->dc & 0xffffffff;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Bad offset 0x%" HWADDR_PRIx "\n", __func__, addr);
    }
    return readval;
}

static void bcm2835_spi_write(void *opaque, hwaddr addr,
                              uint64_t value, unsigned int size)
{
    BCM2835SPIState *s = opaque;

    switch (addr) {
    case BCM2835_SPI_CS:
    {
        bool old_ta = s->cs & BCM2835_SPI_CS_TA;

        s->cs = (value & ~RO_MASK) | (s->cs & RO_MASK);
        if (!(s->cs & BCM2835_SPI_CS_TA)) {
            timer_del(s->transfer_timer);
            s->transfer_remaining_cycles = 0;
            /* Clear DONE and RXR if TA is off */
            s->cs &= ~(BCM2835_SPI_CS_DONE);
            s->cs &= ~(BCM2835_SPI_CS_RXR);
            s->dma_remaining = 0;
            s->dma_complete = false;
        }

        /* Clear RX FIFO */
        if (s->cs & BCM2835_SPI_CLEAR_RX) {
            fifo8_reset(&s->rx_fifo);
            bcm2835_spi_update_rx_flags(s);
        }

        /* Clear TX FIFO*/
        if (s->cs & BCM2835_SPI_CLEAR_TX) {
            timer_del(s->transfer_timer);
            s->transfer_remaining_cycles = 0;
            fifo8_reset(&s->tx_fifo);
            bcm2835_spi_update_tx_flags(s);
        }
        s->cs &= ~(BCM2835_SPI_CLEAR_RX | BCM2835_SPI_CLEAR_TX);

        /* Set Transfer Active */
        if (s->cs & BCM2835_SPI_CS_TA) {
            if (!old_ta && s->cs & BCM2835_SPI_CS_DMAEN) {
                s->dma_remaining = s->dlen;
                s->dma_complete = !s->dma_remaining;
            }
            bcm2835_spi_update_tx_flags(s);
        }

        if (s->cs & BCM2835_SPI_CS_LEN) {
            qemu_log_mask(LOG_UNIMP, "%s: " \
                          "LoSSI not supported\n", __func__);
        }

        bcm2835_spi_update_outputs(s);
        bcm2835_spi_schedule_transfer(s);
        bcm2835_spi_update_int(s);
        break;
    }
    case BCM2835_SPI_FIFO:
        if ((s->cs & BCM2835_SPI_CS_DMAEN) &&
            !(s->cs & BCM2835_SPI_CS_TA)) {
            /*
             * The first DMA word is the hardware framing contract: DLEN in
             * bits 31:16 and the low eight CS control bits.
             */
            s->dlen = extract64(value, 16, 16);
            s->cs = (s->cs & ~(uint32_t)0xff) | (value & 0xff);
            if (s->cs & BCM2835_SPI_CLEAR_RX) {
                fifo8_reset(&s->rx_fifo);
            }
            if (s->cs & BCM2835_SPI_CLEAR_TX) {
                fifo8_reset(&s->tx_fifo);
            }
            s->cs &= ~(BCM2835_SPI_CLEAR_RX | BCM2835_SPI_CLEAR_TX);
            s->dma_remaining = s->dlen;
            s->dma_complete = !s->dma_remaining;
            bcm2835_spi_update_rx_flags(s);
            bcm2835_spi_update_tx_flags(s);
            bcm2835_spi_update_outputs(s);
            bcm2835_spi_update_int(s);
        } else if (s->cs & BCM2835_SPI_CS_TA) {
            if (s->cs & BCM2835_SPI_CS_TXD) {
                unsigned int bytes =
                    s->cs & BCM2835_SPI_CS_DMAEN ? sizeof(uint32_t) : 1;

                for (unsigned int index = 0;
                     index < bytes && !fifo8_is_full(&s->tx_fifo);
                     index++) {
                    if ((s->cs & BCM2835_SPI_CS_DMAEN) &&
                        fifo8_num_used(&s->tx_fifo) >= s->dma_remaining) {
                        break;
                    }
                    fifo8_push(&s->tx_fifo,
                               extract64(value, index * 8, 8));
                }
                bcm2835_spi_update_tx_flags(s);
            }

            bcm2835_spi_update_outputs(s);
            bcm2835_spi_schedule_transfer(s);
            bcm2835_spi_update_int(s);
        }
        break;
    case BCM2835_SPI_CLK:
        bcm2835_spi_pause_transfer(s);
        s->clk = value & 0xffff;
        bcm2835_spi_schedule_transfer(s);
        break;
    case BCM2835_SPI_DLEN:
        s->dlen = value & 0xffff;
        if ((s->cs & (BCM2835_SPI_CS_DMAEN | BCM2835_SPI_CS_TA)) ==
            (BCM2835_SPI_CS_DMAEN | BCM2835_SPI_CS_TA)) {
            s->dma_remaining = s->dlen;
            s->dma_complete = !s->dma_remaining;
            bcm2835_spi_update_tx_flags(s);
            bcm2835_spi_update_outputs(s);
            bcm2835_spi_update_int(s);
        }
        break;
    case BCM2835_SPI_LTOH:
        s->ltoh = value & 0xf;
        break;
    case BCM2835_SPI_DC:
        s->dc = value & 0xffffffff;
        bcm2835_spi_update_outputs(s);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Bad offset 0x%" HWADDR_PRIx "\n", __func__, addr);
    }
}

static const MemoryRegionOps bcm2835_spi_ops = {
    .read = bcm2835_spi_read,
    .write = bcm2835_spi_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void bcm2835_spi_realize(DeviceState *dev, Error **errp)
{
    BCM2835SPIState *s = BCM2835_SPI(dev);
    s->bus = ssi_create_bus(dev, "spi");

    memory_region_init_io(&s->iomem, OBJECT(dev), &bcm2835_spi_ops, s,
                          TYPE_BCM2835_SPI, 0x18);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    fifo8_create(&s->tx_fifo, FIFO_SIZE);
    fifo8_create(&s->rx_fifo, FIFO_SIZE);
}

static int bcm2835_spi_post_load(void *opaque, int version_id)
{
    BCM2835SPIState *s = opaque;

    if (version_id >= 2 &&
        (s->dma_remaining > s->dlen ||
         (s->dma_complete && s->dma_remaining) ||
         s->transfer_remaining_cycles > 8)) {
        return -EINVAL;
    }
    bcm2835_spi_update_rx_flags(s);
    bcm2835_spi_update_tx_flags(s);
    bcm2835_spi_update_outputs(s);
    bcm2835_spi_update_int(s);
    bcm2835_spi_schedule_transfer(s);
    return 0;
}

static void bcm2835_spi_reset(DeviceState *dev)
{
    BCM2835SPIState *s = BCM2835_SPI(dev);

    fifo8_reset(&s->tx_fifo);
    fifo8_reset(&s->rx_fifo);
    timer_del(s->transfer_timer);

    /* Reset values according to BCM2835 Peripheral Documentation */
    s->cs = BCM2835_SPI_CS_TXD | BCM2835_SPI_CS_REN;
    s->clk = 0;
    s->dlen = 0;
    s->ltoh = 0x1;
    s->dc = 0x30201020;
    s->dma_remaining = 0;
    s->transfer_remaining_cycles = 0;
    s->dma_complete = false;
    bcm2835_spi_update_rx_flags(s);
    bcm2835_spi_update_tx_flags(s);
    bcm2835_spi_update_outputs(s);
    qemu_set_irq(s->irq, 0);
}

static const VMStateDescription vmstate_bcm2835_spi = {
    .name = TYPE_BCM2835_SPI,
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = bcm2835_spi_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_FIFO8(tx_fifo, BCM2835SPIState),
        VMSTATE_FIFO8(rx_fifo, BCM2835SPIState),
        VMSTATE_UINT32(cs, BCM2835SPIState),
        VMSTATE_UINT32(clk, BCM2835SPIState),
        VMSTATE_UINT32(dlen, BCM2835SPIState),
        VMSTATE_UINT32(ltoh, BCM2835SPIState),
        VMSTATE_UINT32(dc, BCM2835SPIState),
        VMSTATE_UINT32_V(dma_remaining, BCM2835SPIState, 2),
        VMSTATE_TIMER_PTR_V(transfer_timer, BCM2835SPIState, 2),
        VMSTATE_UINT8_V(transfer_remaining_cycles, BCM2835SPIState, 2),
        VMSTATE_BOOL_V(dma_complete, BCM2835SPIState, 2),
        VMSTATE_END_OF_LIST()
    }
};

static void bcm2835_spi_init(Object *obj)
{
    BCM2835SPIState *s = BCM2835_SPI(obj);

    qdev_init_gpio_out_named(DEVICE(s), s->chip_select,
                             "chip-select", 3);
    qdev_init_gpio_out_named(DEVICE(s), s->dma_threshold,
                             "dma-threshold",
                             BCM2835_SPI_DMA_OUTPUTS);
    s->transfer_timer = timer_new_ns(
        QEMU_CLOCK_VIRTUAL, bcm2835_spi_transfer, s);
    s->core_clk = qdev_init_clock_in(
        DEVICE(s), "core", bcm2835_spi_core_clk_update, s,
        ClockPreUpdate | ClockUpdate);
    object_property_add(obj, "core-clock-frequency", "uint32",
                        bcm2835_spi_core_clock_get, NULL, NULL, NULL);
}

static void bcm2835_spi_finalize(Object *obj)
{
    BCM2835SPIState *s = BCM2835_SPI(obj);

    timer_free(s->transfer_timer);
}

static void bcm2835_spi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, bcm2835_spi_reset);
    dc->realize = bcm2835_spi_realize;
    dc->vmsd = &vmstate_bcm2835_spi;
}

static const TypeInfo bcm2835_spi_info = {
    .name = TYPE_BCM2835_SPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2835SPIState),
    .instance_init = bcm2835_spi_init,
    .instance_finalize = bcm2835_spi_finalize,
    .class_init = bcm2835_spi_class_init,
};

static void bcm2835_spi_register_types(void)
{
    type_register_static(&bcm2835_spi_info);
}

type_init(bcm2835_spi_register_types)
