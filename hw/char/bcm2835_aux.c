/*
 * BCM2835 (Raspberry Pi / Pi 2) Aux block (mini UART and SPI).
 * Copyright (c) 2015, Microsoft
 * Written by Andrew Baumann
 * Based on pl011.c, copyright terms below:
 *
 * Arm PrimeCell PL011 UART
 *
 * Copyright (c) 2006 CodeSourcery.
 * Written by Paul Brook
 *
 * This code is licensed under the GPL.
 *
 * The mini-UART and both auxiliary SPI master data paths are modeled.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/char/bcm2835_aux.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define AUX_IRQ         0x0
#define AUX_ENABLES     0x4
#define AUX_MU_IO_REG   0x40
#define AUX_MU_IER_REG  0x44
#define AUX_MU_IIR_REG  0x48
#define AUX_MU_LCR_REG  0x4c
#define AUX_MU_MCR_REG  0x50
#define AUX_MU_LSR_REG  0x54
#define AUX_MU_MSR_REG  0x58
#define AUX_MU_SCRATCH  0x5c
#define AUX_MU_CNTL_REG 0x60
#define AUX_MU_STAT_REG 0x64
#define AUX_MU_BAUD_REG 0x68
#define AUX_SPI_BASE(n)  (0x80 + (n) * 0x40)
#define AUX_SPI_CNTL0    0x00
#define AUX_SPI_CNTL1    0x04
#define AUX_SPI_STAT     0x08
#define AUX_SPI_PEEK     0x0c
#define AUX_SPI_IO       0x20
#define AUX_SPI_TXHOLD   0x30

#define AUX_SPI_CNTL0_SPEED_MASK 0xfff00000
#define AUX_SPI_CNTL0_CS_MASK    0x000e0000
#define AUX_SPI_CNTL0_VAR_CS     BIT(15)
#define AUX_SPI_CNTL0_VAR_WIDTH  BIT(14)
#define AUX_SPI_CNTL0_ENABLE     BIT(11)
#define AUX_SPI_CNTL0_CLEARFIFO  BIT(9)
#define AUX_SPI_CNTL0_MSBF_OUT   BIT(6)
#define AUX_SPI_CNTL0_SHIFT_MASK 0x3f
#define AUX_SPI_CNTL1_CSHIGH_MASK 0x700
#define AUX_SPI_CNTL1_TXEMPTY_IRQ BIT(7)
#define AUX_SPI_CNTL1_IDLE_IRQ    BIT(6)
#define AUX_SPI_CNTL1_MSBF_IN     BIT(1)
#define AUX_SPI_CNTL1_KEEP_IN     BIT(0)

#define AUX_SPI_STAT_TX_LEVEL_SHIFT 24
#define AUX_SPI_STAT_RX_LEVEL_SHIFT 16
#define AUX_SPI_STAT_TX_FULL BIT(10)
#define AUX_SPI_STAT_TX_EMPTY BIT(9)
#define AUX_SPI_STAT_RX_FULL BIT(8)
#define AUX_SPI_STAT_RX_EMPTY BIT(7)
#define AUX_SPI_STAT_BUSY BIT(6)

/* bits in IER/IIR registers */
#define RX_INT  0x1
#define TX_INT  0x2
#define LCR_DLAB 0x80
#define CNTL_RX_ENABLE 0x1
#define CNTL_TX_ENABLE 0x2
#define AUX_UART_FRAME_BITS 10

static void bcm2835_aux_update(BCM2835AuxState *s);
static void bcm2835_aux_spi_schedule(BCM2835AuxSPIState *spi);

static bool bcm2835_aux_spi_enabled(const BCM2835AuxSPIState *spi)
{
    return spi->parent->enables & BIT(spi->index + 1);
}

static bool bcm2835_aux_spi_irq_pending(const BCM2835AuxSPIState *spi)
{
    if (!bcm2835_aux_spi_enabled(spi)) {
        return false;
    }

    return ((spi->cntl1 & AUX_SPI_CNTL1_TXEMPTY_IRQ) &&
            spi->tx_count == 0) ||
           ((spi->cntl1 & AUX_SPI_CNTL1_IDLE_IRQ) &&
            spi->tx_count == 0 && !spi->active && !spi->cs_held);
}

static void bcm2835_aux_spi_set_cs(BCM2835AuxSPIState *spi,
                                   bool active, uint8_t pattern)
{
    for (unsigned int index = 0; index < 3; index++) {
        DeviceState *peripheral = ssi_get_cs(spi->bus, index);
        int level = active ? extract8(pattern, index, 1) : 1;

        qemu_set_irq(spi->chip_select[index], level);
        if (peripheral) {
            qemu_set_irq(qdev_get_gpio_in_named(
                             peripheral, SSI_GPIO_CS, 0), level);
        }
    }
}

static void bcm2835_aux_spi_reset_runtime(BCM2835AuxSPIState *spi)
{
    timer_del(spi->timer);
    spi->tx_pos = 0;
    spi->tx_count = 0;
    spi->rx_pos = 0;
    spi->rx_count = 0;
    spi->active = false;
    spi->active_hold = false;
    spi->cs_held = false;
    spi->active_bits = 0;
    spi->remaining_core_cycles = 0;
    spi->input_shift = 0;
    spi->last_rx = 0;
    bcm2835_aux_spi_set_cs(spi, false, 7);
}

static uint8_t bcm2835_aux_spi_word_bits(const BCM2835AuxSPIState *spi,
                                         uint32_t word)
{
    uint8_t bits = spi->cntl0 & AUX_SPI_CNTL0_VAR_WIDTH ?
                   extract32(word, 24, 5) :
                   spi->cntl0 & AUX_SPI_CNTL0_SHIFT_MASK;

    return bits ? bits : 32;
}

static uint64_t bcm2835_aux_spi_transfer_cycles(
    const BCM2835AuxSPIState *spi, uint8_t bits)
{
    uint64_t speed = extract32(spi->cntl0, 20, 12) + 1;
    uint64_t cs_high = extract32(spi->cntl1, 8, 3) + 1;

    /* One setup cycle plus the configured post-transfer CS-high interval. */
    return (bits + 1 + cs_high) * 2 * speed;
}

static bool bcm2835_aux_spi_ready(const BCM2835AuxSPIState *spi)
{
    return bcm2835_aux_spi_enabled(spi) &&
           (spi->cntl0 & AUX_SPI_CNTL0_ENABLE) &&
           !(spi->cntl0 & AUX_SPI_CNTL0_CLEARFIFO) &&
           !spi->active && spi->tx_count &&
           spi->rx_count < BCM2835_AUX_SPI_FIFO_LEN;
}

static void bcm2835_aux_spi_schedule(BCM2835AuxSPIState *spi)
{
    BCM2835AuxState *s = spi->parent;
    uint64_t cycles;
    uint64_t delay;

    if (timer_pending(spi->timer)) {
        return;
    }

    if (!spi->active) {
        if (!bcm2835_aux_spi_ready(spi)) {
            return;
        }
        spi->active_word = spi->tx_fifo[spi->tx_pos];
        spi->active_hold = spi->tx_hold[spi->tx_pos];
        spi->tx_pos = (spi->tx_pos + 1) % BCM2835_AUX_SPI_FIFO_LEN;
        spi->tx_count--;
        spi->active_bits = bcm2835_aux_spi_word_bits(spi, spi->active_word);
        spi->active_cs = spi->cntl0 & AUX_SPI_CNTL0_VAR_CS ?
                         extract32(spi->active_word, 29, 3) :
                         extract32(spi->cntl0, 17, 3);
        spi->active = true;
        spi->cs_held = false;
        bcm2835_aux_spi_set_cs(spi, true, spi->active_cs);
    } else if (!bcm2835_aux_spi_enabled(spi) ||
               !(spi->cntl0 & AUX_SPI_CNTL0_ENABLE) ||
               spi->cntl0 & AUX_SPI_CNTL0_CLEARFIFO) {
        return;
    }

    cycles = spi->remaining_core_cycles ?:
             bcm2835_aux_spi_transfer_cycles(spi, spi->active_bits);
    if (!clock_get_hz(s->core_clk)) {
        spi->remaining_core_cycles = cycles;
        bcm2835_aux_update(s);
        return;
    }

    delay = MAX(1ULL, clock_ticks_to_ns(s->core_clk, cycles));
    timer_mod_ns(spi->timer,
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay);
    spi->remaining_core_cycles = 0;
    bcm2835_aux_update(s);
}

static void bcm2835_aux_spi_pause(BCM2835AuxSPIState *spi)
{
    BCM2835AuxState *s = spi->parent;
    uint64_t now;
    uint64_t expiry;
    uint64_t remaining_ns;
    uint64_t cycles;

    if (!timer_pending(spi->timer)) {
        return;
    }

    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    expiry = timer_expire_time_ns(spi->timer);
    remaining_ns = expiry > now ? expiry - now : 1;
    cycles = clock_ns_to_ticks(s->core_clk, remaining_ns);
    if (clock_ticks_to_ns(s->core_clk, cycles) < remaining_ns) {
        cycles++;
    }
    spi->remaining_core_cycles = MAX(1ULL, cycles);
    timer_del(spi->timer);
}

static uint8_t bcm2835_aux_spi_tx_byte(const BCM2835AuxSPIState *spi,
                                       unsigned int index,
                                       unsigned int byte_count)
{
    unsigned int shift;

    if (spi->cntl0 & AUX_SPI_CNTL0_MSBF_OUT) {
        unsigned int width = spi->cntl0 & AUX_SPI_CNTL0_VAR_WIDTH ?
                             24 : byte_count * 8;

        /*
         * A variable-width transfer carries its data in bits 23:0, so a
         * programmed width of 25 to 31 bits leaves no byte to shift out
         * here.  Keep the shift in range instead of handing extract32 a
         * value that wrapped below zero.
         */
        if (width < (index + 1) * 8) {
            return 0;
        }
        shift = width - (index + 1) * 8;
    } else {
        shift = index * 8;
    }
    return extract32(spi->active_word, shift, 8);
}

static void bcm2835_aux_spi_transfer(void *opaque)
{
    BCM2835AuxSPIState *spi = opaque;
    BCM2835AuxState *s = spi->parent;
    unsigned int byte_count;
    uint32_t received;

    if (!spi->active || !bcm2835_aux_spi_enabled(spi) ||
        !(spi->cntl0 & AUX_SPI_CNTL0_ENABLE)) {
        bcm2835_aux_update(s);
        return;
    }

    byte_count = DIV_ROUND_UP(spi->active_bits, 8);
    if (!(spi->cntl1 & AUX_SPI_CNTL1_KEEP_IN)) {
        spi->input_shift = 0;
    }
    for (unsigned int index = 0; index < byte_count; index++) {
        uint8_t rx = ssi_transfer(
            spi->bus,
            bcm2835_aux_spi_tx_byte(spi, index, byte_count));

        if (spi->cntl1 & AUX_SPI_CNTL1_MSBF_IN) {
            spi->input_shift = (spi->input_shift << 8) | rx;
        } else {
            spi->input_shift |= (uint32_t)rx << (index * 8);
        }
    }
    received = spi->input_shift;
    spi->last_rx = received;
    spi->rx_fifo[(spi->rx_pos + spi->rx_count) %
                 BCM2835_AUX_SPI_FIFO_LEN] = received;
    spi->rx_count++;
    spi->active = false;
    spi->active_bits = 0;
    spi->remaining_core_cycles = 0;
    spi->cs_held = spi->active_hold;
    if (!spi->cs_held) {
        bcm2835_aux_spi_set_cs(spi, false, 7);
    }
    bcm2835_aux_spi_schedule(spi);
    bcm2835_aux_update(s);
}

static uint64_t bcm2835_aux_frame_cycles(const BCM2835AuxState *s)
{
    return AUX_UART_FRAME_BITS * 8ULL * (s->baud + 1);
}

static void bcm2835_aux_core_clock_get(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    BCM2835AuxState *s = BCM2835_AUX(obj);
    uint32_t frequency = clock_get_hz(s->core_clk);

    visit_type_uint32(v, name, &frequency, errp);
}

static uint64_t bcm2835_aux_tx_delay_ns(const BCM2835AuxState *s,
                                        uint64_t cycles)
{
    if (!clock_get_hz(s->core_clk)) {
        return 0;
    }

    return MAX(1ULL, clock_ticks_to_ns(s->core_clk, cycles));
}

static void bcm2835_aux_tx_schedule(BCM2835AuxState *s)
{
    uint64_t cycles;
    uint64_t delay;

    if (s->tx_count && (s->enables & 1) &&
        (s->cntl & CNTL_TX_ENABLE) && !timer_pending(s->tx_timer)) {
        cycles = s->tx_remaining_cycles ?: bcm2835_aux_frame_cycles(s);
        delay = bcm2835_aux_tx_delay_ns(s, cycles);
        if (!delay) {
            s->tx_remaining_cycles = cycles;
            return;
        }
        timer_mod_ns(
            s->tx_timer,
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay);
        s->tx_remaining_cycles = 0;
    }
}

static void bcm2835_aux_core_clk_update(void *opaque, ClockEvent event)
{
    BCM2835AuxState *s = opaque;

    if (event == ClockPreUpdate) {
        if (timer_pending(s->tx_timer)) {
            uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            uint64_t expiry = timer_expire_time_ns(s->tx_timer);
            uint64_t remaining_ns = expiry > now ? expiry - now : 1;
            uint64_t cycles = clock_ns_to_ticks(s->core_clk, remaining_ns);

            if (clock_ticks_to_ns(s->core_clk, cycles) < remaining_ns) {
                cycles++;
            }
            s->tx_remaining_cycles = MAX(1ULL, cycles);
            timer_del(s->tx_timer);
        }
        for (unsigned int index = 0; index < BCM2835_AUX_SPI_COUNT;
             index++) {
            bcm2835_aux_spi_pause(&s->spi[index]);
        }
    } else if (event == ClockUpdate) {
        bcm2835_aux_tx_schedule(s);
        for (unsigned int index = 0; index < BCM2835_AUX_SPI_COUNT;
             index++) {
            bcm2835_aux_spi_schedule(&s->spi[index]);
        }
    }
}

static void bcm2835_aux_update(BCM2835AuxState *s)
{
    bool shared_irq = false;

    /* signal an interrupt if either:
     * 1. rx interrupt is enabled and we have a non-empty rx fifo, or
     * 2. the tx interrupt is enabled and the transmit FIFO is empty
     */
    s->iir = 0;
    if (s->enables & 1) {
        if ((s->ier & RX_INT) && (s->cntl & CNTL_RX_ENABLE) &&
            s->read_count != 0) {
            s->iir |= RX_INT;
        }
        if ((s->ier & TX_INT) && (s->cntl & CNTL_TX_ENABLE) &&
            s->tx_count == 0) {
            s->iir |= TX_INT;
        }
        shared_irq = s->iir != 0;
    }
    for (unsigned int index = 0; index < BCM2835_AUX_SPI_COUNT; index++) {
        shared_irq |= bcm2835_aux_spi_irq_pending(&s->spi[index]);
    }
    qemu_set_irq(s->irq, shared_irq);
}

static uint32_t bcm2835_aux_spi_status(const BCM2835AuxSPIState *spi)
{
    uint32_t status =
        (uint32_t)spi->tx_count << AUX_SPI_STAT_TX_LEVEL_SHIFT |
        (uint32_t)spi->rx_count << AUX_SPI_STAT_RX_LEVEL_SHIFT;

    if (spi->tx_count == BCM2835_AUX_SPI_FIFO_LEN) {
        status |= AUX_SPI_STAT_TX_FULL;
    }
    if (!spi->tx_count) {
        status |= AUX_SPI_STAT_TX_EMPTY;
    }
    if (spi->rx_count == BCM2835_AUX_SPI_FIFO_LEN) {
        status |= AUX_SPI_STAT_RX_FULL;
    }
    if (!spi->rx_count) {
        status |= AUX_SPI_STAT_RX_EMPTY;
    }
    if (spi->active || spi->cs_held) {
        status |= AUX_SPI_STAT_BUSY;
    }
    if (spi->active) {
        status |= spi->active_bits & 0x3f;
    }
    return status;
}

static uint32_t bcm2835_aux_spi_read(BCM2835AuxSPIState *spi,
                                     hwaddr reg)
{
    BCM2835AuxState *s = spi->parent;
    uint32_t value;

    if (!bcm2835_aux_spi_enabled(spi)) {
        return 0;
    }

    switch (reg) {
    case AUX_SPI_CNTL0:
        return spi->cntl0;
    case AUX_SPI_CNTL1:
        return spi->cntl1;
    case AUX_SPI_STAT:
        return bcm2835_aux_spi_status(spi);
    case AUX_SPI_PEEK:
        return spi->rx_count ? spi->rx_fifo[spi->rx_pos] : spi->last_rx;
    default:
        if ((reg >= AUX_SPI_IO && reg < AUX_SPI_IO + 0x10) ||
            (reg >= AUX_SPI_TXHOLD && reg < AUX_SPI_TXHOLD + 0x10)) {
            value = spi->last_rx;
            if (spi->rx_count) {
                value = spi->rx_fifo[spi->rx_pos];
                spi->rx_pos =
                    (spi->rx_pos + 1) % BCM2835_AUX_SPI_FIFO_LEN;
                spi->rx_count--;
            }
            bcm2835_aux_spi_schedule(spi);
            bcm2835_aux_update(s);
            return value;
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad SPI%u register offset 0x%" HWADDR_PRIx "\n",
                      __func__, spi->index + 1, reg);
        return 0;
    }
}

static void bcm2835_aux_spi_write(BCM2835AuxSPIState *spi,
                                  hwaddr reg, uint32_t value)
{
    BCM2835AuxState *s = spi->parent;

    if (!bcm2835_aux_spi_enabled(spi)) {
        return;
    }

    switch (reg) {
    case AUX_SPI_CNTL0:
        if (spi->active) {
            bcm2835_aux_spi_pause(spi);
        }
        spi->cntl0 = value;
        if (value & AUX_SPI_CNTL0_CLEARFIFO) {
            bcm2835_aux_spi_reset_runtime(spi);
        } else {
            bcm2835_aux_spi_schedule(spi);
        }
        break;
    case AUX_SPI_CNTL1:
        spi->cntl1 = value & 0x7c3;
        break;
    case AUX_SPI_STAT:
        break;
    default:
        if ((reg >= AUX_SPI_IO && reg < AUX_SPI_IO + 0x10) ||
            (reg >= AUX_SPI_TXHOLD && reg < AUX_SPI_TXHOLD + 0x10)) {
            if (spi->tx_count < BCM2835_AUX_SPI_FIFO_LEN) {
                unsigned int slot =
                    (spi->tx_pos + spi->tx_count) %
                    BCM2835_AUX_SPI_FIFO_LEN;

                spi->tx_fifo[slot] = value;
                spi->tx_hold[slot] = reg >= AUX_SPI_TXHOLD;
                spi->tx_count++;
                bcm2835_aux_spi_schedule(spi);
            }
            break;
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad SPI%u register offset 0x%" HWADDR_PRIx "\n",
                      __func__, spi->index + 1, reg);
        break;
    }
    bcm2835_aux_update(s);
}

static uint64_t bcm2835_aux_read(void *opaque, hwaddr offset, unsigned size)
{
    BCM2835AuxState *s = opaque;
    uint32_t c, res;

    if (offset >= AUX_SPI_BASE(0)) {
        unsigned int index = (offset - AUX_SPI_BASE(0)) / 0x40;

        if (index < BCM2835_AUX_SPI_COUNT) {
            return bcm2835_aux_spi_read(
                &s->spi[index], offset - AUX_SPI_BASE(index));
        }
    }

    switch (offset) {
    case AUX_IRQ:
        res = s->iir ? BIT(0) : 0;
        for (unsigned int index = 0; index < BCM2835_AUX_SPI_COUNT;
             index++) {
            if (bcm2835_aux_spi_irq_pending(&s->spi[index])) {
                res |= BIT(index + 1);
            }
        }
        return res;

    case AUX_ENABLES:
        return s->enables;

    case AUX_MU_IO_REG:
        if (s->lcr & LCR_DLAB) {
            return s->baud & 0xff;
        }
        c = s->read_fifo[s->read_pos];
        if (s->read_count > 0) {
            s->read_count--;
            if (++s->read_pos == BCM2835_AUX_RX_FIFO_LEN) {
                s->read_pos = 0;
            }
        }
        qemu_chr_fe_accept_input(&s->chr);
        bcm2835_aux_update(s);
        return c;

    case AUX_MU_IER_REG:
        if (s->lcr & LCR_DLAB) {
            return s->baud >> 8;
        }
        return 0xc0 | s->ier; /* FIFO enables always read 1 */

    case AUX_MU_IIR_REG:
        res = 0xc0; /* FIFO enables */
        /* The spec is unclear on what happens when both tx and rx
         * interrupts are active, besides that this cannot occur. At
         * present, we choose to prioritise the rx interrupt, since
         * the tx fifo is always empty. */
        if ((s->iir & RX_INT) && s->read_count != 0) {
            res |= 0x4;
        } else {
            res |= 0x2;
        }
        if (s->iir == 0) {
            res |= 0x1;
        }
        return res;

    case AUX_MU_LCR_REG:
        return s->lcr;

    case AUX_MU_MCR_REG:
        return s->mcr;

    case AUX_MU_LSR_REG:
        res = 0;
        if (s->tx_count == 0) {
            res |= 0x60; /* transmitter idle and FIFO empty */
        }
        if (s->read_count != 0) {
            res |= 0x1;
        }
        return res;

    case AUX_MU_MSR_REG:
        return 0x10; /* CTS asserted */

    case AUX_MU_SCRATCH:
        return s->scratch;

    case AUX_MU_CNTL_REG:
        return s->cntl;

    case AUX_MU_STAT_REG:
        res = 0x4; /* receiver idle */
        if (s->tx_count < BCM2835_AUX_TX_FIFO_LEN) {
            res |= 0x2; /* space in the output buffer */
        } else {
            res |= 0x20; /* transmit FIFO full */
        }
        if (s->tx_count == 0) {
            res |= 0x308; /* transmitter idle, FIFO empty and done */
        } else {
            res |= ((uint32_t)s->tx_count) << 24;
        }
        if (s->read_count > 0) {
            res |= 0x1; /* data in input buffer */
            assert(s->read_count <= BCM2835_AUX_RX_FIFO_LEN);
            res |= ((uint32_t)s->read_count) << 16; /* rx fifo fill level */
        }
        return res;

    case AUX_MU_BAUD_REG:
        return s->baud;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset %"HWADDR_PRIx"\n",
                      __func__, offset);
        return 0;
    }
}

static void bcm2835_aux_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    BCM2835AuxState *s = opaque;
    unsigned int slot;

    if (offset >= AUX_SPI_BASE(0)) {
        unsigned int index = (offset - AUX_SPI_BASE(0)) / 0x40;

        if (index < BCM2835_AUX_SPI_COUNT) {
            bcm2835_aux_spi_write(
                &s->spi[index], offset - AUX_SPI_BASE(index), value);
            return;
        }
    }

    switch (offset) {
    case AUX_ENABLES:
    {
        uint8_t old_enables = s->enables;

        s->enables = value & 0x7;
        if (!(s->enables & 1)) {
            timer_del(s->tx_timer);
            s->tx_remaining_cycles = 0;
        } else {
            bcm2835_aux_tx_schedule(s);
        }
        for (unsigned int index = 0; index < BCM2835_AUX_SPI_COUNT;
             index++) {
            uint8_t enable_bit = BIT(index + 1);

            if ((old_enables & enable_bit) &&
                !(s->enables & enable_bit)) {
                bcm2835_aux_spi_reset_runtime(&s->spi[index]);
            } else if (s->enables & enable_bit) {
                bcm2835_aux_spi_schedule(&s->spi[index]);
            }
        }
        break;
    }

    case AUX_MU_IO_REG:
        if (s->lcr & LCR_DLAB) {
            s->baud = (s->baud & 0xff00) | (value & 0xff);
            break;
        }
        if ((s->enables & 1) && (s->cntl & CNTL_TX_ENABLE)) {
            if (s->tx_count < BCM2835_AUX_TX_FIFO_LEN) {
                slot = (s->tx_pos + s->tx_count) %
                       BCM2835_AUX_TX_FIFO_LEN;
                s->tx_fifo[slot] = value;
                s->tx_count++;
                bcm2835_aux_tx_schedule(s);
            } else {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "%s: transmit FIFO overflow\n", __func__);
            }
        }
        break;

    case AUX_MU_IER_REG:
        if (s->lcr & LCR_DLAB) {
            s->baud = (s->baud & 0xff) | ((value & 0xff) << 8);
            break;
        }
        s->ier = value & (TX_INT | RX_INT);
        bcm2835_aux_update(s);
        break;

    case AUX_MU_IIR_REG:
        if (value & 0x2) {
            s->read_count = 0;
        }
        if (value & 0x4) {
            s->tx_pos = 0;
            s->tx_count = 0;
            timer_del(s->tx_timer);
            s->tx_remaining_cycles = 0;
        }
        break;

    case AUX_MU_LCR_REG:
        s->lcr = value & 0xc3;
        break;

    case AUX_MU_MCR_REG:
        s->mcr = value & 0x2;
        break;

    case AUX_MU_SCRATCH:
        s->scratch = value;
        break;

    case AUX_MU_CNTL_REG:
        s->cntl = value;
        if (!(s->cntl & CNTL_TX_ENABLE)) {
            timer_del(s->tx_timer);
            s->tx_remaining_cycles = 0;
        } else {
            bcm2835_aux_tx_schedule(s);
        }
        break;

    case AUX_MU_BAUD_REG:
        s->baud = value;
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset %"HWADDR_PRIx"\n",
                      __func__, offset);
    }

    bcm2835_aux_update(s);
}

static void bcm2835_aux_tx(void *opaque)
{
    BCM2835AuxState *s = opaque;
    int written;

    if (!s->tx_count || !(s->enables & 1) ||
        !(s->cntl & CNTL_TX_ENABLE)) {
        bcm2835_aux_update(s);
        return;
    }
    written = qemu_chr_fe_write(&s->chr, &s->tx_fifo[s->tx_pos], 1);
    if (written == 1) {
        s->tx_pos = (s->tx_pos + 1) % BCM2835_AUX_TX_FIFO_LEN;
        s->tx_count--;
    }
    bcm2835_aux_tx_schedule(s);
    bcm2835_aux_update(s);
}

static int bcm2835_aux_can_receive(void *opaque)
{
    BCM2835AuxState *s = opaque;

    return BCM2835_AUX_RX_FIFO_LEN - s->read_count;
}

static void bcm2835_aux_put_fifo(void *opaque, uint8_t value)
{
    BCM2835AuxState *s = opaque;
    int slot;

    slot = s->read_pos + s->read_count;
    if (slot >= BCM2835_AUX_RX_FIFO_LEN) {
        slot -= BCM2835_AUX_RX_FIFO_LEN;
    }
    s->read_fifo[slot] = value;
    s->read_count++;
    if (s->read_count == BCM2835_AUX_RX_FIFO_LEN) {
        /* buffer full */
    }
    bcm2835_aux_update(s);
}

static void bcm2835_aux_receive(void *opaque, const uint8_t *buf, int size)
{
    for (int i = 0; i < size; i++) {
        bcm2835_aux_put_fifo(opaque, buf[i]);
    }
}

static const MemoryRegionOps bcm2835_aux_ops = {
    .read = bcm2835_aux_read,
    .write = bcm2835_aux_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static int bcm2835_aux_post_load(void *opaque, int version_id)
{
    BCM2835AuxState *s = opaque;

    if (s->read_pos >= BCM2835_AUX_RX_FIFO_LEN ||
        s->read_count > BCM2835_AUX_RX_FIFO_LEN ||
        s->tx_pos >= BCM2835_AUX_TX_FIFO_LEN ||
        s->tx_count > BCM2835_AUX_TX_FIFO_LEN) {
        return -EINVAL;
    }
    if (version_id < 2) {
        s->enables = 1;
        s->cntl = CNTL_RX_ENABLE | CNTL_TX_ENABLE;
    }
    if (version_id < 3) {
        s->tx_pos = 0;
        s->tx_count = 0;
        timer_del(s->tx_timer);
    }
    if (version_id < 4) {
        s->tx_remaining_cycles = 0;
    }
    for (unsigned int index = 0; index < BCM2835_AUX_SPI_COUNT; index++) {
        BCM2835AuxSPIState *spi = &s->spi[index];

        if (version_id < 5) {
            spi->cntl0 = AUX_SPI_CNTL0_CS_MASK;
            spi->cntl1 = 0;
            bcm2835_aux_spi_reset_runtime(spi);
        } else if (spi->tx_pos >= BCM2835_AUX_SPI_FIFO_LEN ||
                   spi->tx_count > BCM2835_AUX_SPI_FIFO_LEN ||
                   spi->rx_pos >= BCM2835_AUX_SPI_FIFO_LEN ||
                   spi->rx_count > BCM2835_AUX_SPI_FIFO_LEN ||
                   spi->active_bits > 32 || spi->active_cs > 7) {
            return -EINVAL;
        } else {
            bcm2835_aux_spi_set_cs(
                spi, spi->active || spi->cs_held, spi->active_cs);
            bcm2835_aux_spi_schedule(spi);
        }
    }
    bcm2835_aux_tx_schedule(s);
    bcm2835_aux_update(s);
    return 0;
}

static const VMStateDescription vmstate_bcm2835_aux_spi = {
    .name = TYPE_BCM2835_AUX "/spi",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cntl0, BCM2835AuxSPIState),
        VMSTATE_UINT32(cntl1, BCM2835AuxSPIState),
        VMSTATE_UINT32_ARRAY(tx_fifo, BCM2835AuxSPIState,
                             BCM2835_AUX_SPI_FIFO_LEN),
        VMSTATE_UINT32_ARRAY(rx_fifo, BCM2835AuxSPIState,
                             BCM2835_AUX_SPI_FIFO_LEN),
        VMSTATE_BOOL_ARRAY(tx_hold, BCM2835AuxSPIState,
                           BCM2835_AUX_SPI_FIFO_LEN),
        VMSTATE_UINT8(tx_pos, BCM2835AuxSPIState),
        VMSTATE_UINT8(tx_count, BCM2835AuxSPIState),
        VMSTATE_UINT8(rx_pos, BCM2835AuxSPIState),
        VMSTATE_UINT8(rx_count, BCM2835AuxSPIState),
        VMSTATE_UINT32(active_word, BCM2835AuxSPIState),
        VMSTATE_UINT32(input_shift, BCM2835AuxSPIState),
        VMSTATE_UINT32(last_rx, BCM2835AuxSPIState),
        VMSTATE_TIMER_PTR(timer, BCM2835AuxSPIState),
        VMSTATE_UINT64(remaining_core_cycles, BCM2835AuxSPIState),
        VMSTATE_UINT8(active_bits, BCM2835AuxSPIState),
        VMSTATE_UINT8(active_cs, BCM2835AuxSPIState),
        VMSTATE_BOOL(active, BCM2835AuxSPIState),
        VMSTATE_BOOL(active_hold, BCM2835AuxSPIState),
        VMSTATE_BOOL(cs_held, BCM2835AuxSPIState),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_bcm2835_aux = {
    .name = TYPE_BCM2835_AUX,
    .version_id = 5,
    .minimum_version_id = 1,
    .post_load = bcm2835_aux_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(read_fifo, BCM2835AuxState,
                            BCM2835_AUX_RX_FIFO_LEN),
        VMSTATE_UINT8(read_pos, BCM2835AuxState),
        VMSTATE_UINT8(read_count, BCM2835AuxState),
        VMSTATE_UINT8(ier, BCM2835AuxState),
        VMSTATE_UINT8(iir, BCM2835AuxState),
        VMSTATE_UINT8_V(enables, BCM2835AuxState, 2),
        VMSTATE_UINT8_V(lcr, BCM2835AuxState, 2),
        VMSTATE_UINT8_V(mcr, BCM2835AuxState, 2),
        VMSTATE_UINT8_V(scratch, BCM2835AuxState, 2),
        VMSTATE_UINT8_V(cntl, BCM2835AuxState, 2),
        VMSTATE_UINT16_V(baud, BCM2835AuxState, 2),
        VMSTATE_UINT8_ARRAY_V(tx_fifo, BCM2835AuxState,
                             BCM2835_AUX_TX_FIFO_LEN, 3),
        VMSTATE_UINT8_V(tx_pos, BCM2835AuxState, 3),
        VMSTATE_UINT8_V(tx_count, BCM2835AuxState, 3),
        VMSTATE_TIMER_PTR_V(tx_timer, BCM2835AuxState, 3),
        VMSTATE_UINT64_V(tx_remaining_cycles, BCM2835AuxState, 4),
        VMSTATE_STRUCT_ARRAY(spi, BCM2835AuxState, BCM2835_AUX_SPI_COUNT, 5,
                             vmstate_bcm2835_aux_spi,
                             BCM2835AuxSPIState),
        VMSTATE_END_OF_LIST()
    }
};

static void bcm2835_aux_reset(DeviceState *dev)
{
    BCM2835AuxState *s = BCM2835_AUX(dev);

    memset(s->read_fifo, 0, sizeof(s->read_fifo));
    s->read_pos = 0;
    s->read_count = 0;
    memset(s->tx_fifo, 0, sizeof(s->tx_fifo));
    s->tx_pos = 0;
    s->tx_count = 0;
    timer_del(s->tx_timer);
    s->tx_remaining_cycles = 0;
    s->ier = 0;
    s->iir = 0;
    s->enables = 1;
    s->lcr = 0;
    s->mcr = 0;
    s->scratch = 0;
    s->cntl = CNTL_RX_ENABLE | CNTL_TX_ENABLE;
    s->baud = 0;
    for (unsigned int index = 0; index < BCM2835_AUX_SPI_COUNT; index++) {
        s->spi[index].cntl0 = AUX_SPI_CNTL0_CS_MASK;
        s->spi[index].cntl1 = 0;
        memset(s->spi[index].tx_fifo, 0,
               sizeof(s->spi[index].tx_fifo));
        memset(s->spi[index].rx_fifo, 0,
               sizeof(s->spi[index].rx_fifo));
        memset(s->spi[index].tx_hold, 0,
               sizeof(s->spi[index].tx_hold));
        bcm2835_aux_spi_reset_runtime(&s->spi[index]);
    }
    bcm2835_aux_update(s);
}

static void bcm2835_aux_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    BCM2835AuxState *s = BCM2835_AUX(obj);

    memory_region_init_io(&s->iomem, OBJECT(s), &bcm2835_aux_ops, s,
                          TYPE_BCM2835_AUX, 0x100);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    s->tx_timer = timer_new_ns(
        QEMU_CLOCK_VIRTUAL, bcm2835_aux_tx, s);
    s->core_clk = qdev_init_clock_in(
        DEVICE(s), "core", bcm2835_aux_core_clk_update, s,
        ClockPreUpdate | ClockUpdate);
    for (unsigned int index = 0; index < BCM2835_AUX_SPI_COUNT; index++) {
        BCM2835AuxSPIState *spi = &s->spi[index];

        spi->parent = s;
        spi->index = index;
        spi->bus = ssi_create_bus(
            DEVICE(s), index ? "aux-spi2" : "aux-spi1");
        spi->timer = timer_new_ns(
            QEMU_CLOCK_VIRTUAL, bcm2835_aux_spi_transfer, spi);
        qdev_init_gpio_out_named(
            DEVICE(s), spi->chip_select,
            index ? "spi2-chip-select" : "spi1-chip-select", 3);
    }
    object_property_add(obj, "core-clock-frequency", "uint32",
                        bcm2835_aux_core_clock_get, NULL, NULL, NULL);
}

static void bcm2835_aux_finalize(Object *obj)
{
    BCM2835AuxState *s = BCM2835_AUX(obj);

    timer_free(s->tx_timer);
    for (unsigned int index = 0; index < BCM2835_AUX_SPI_COUNT; index++) {
        timer_free(s->spi[index].timer);
    }
}

static void bcm2835_aux_realize(DeviceState *dev, Error **errp)
{
    BCM2835AuxState *s = BCM2835_AUX(dev);

    if (!clock_has_source(s->core_clk)) {
        error_setg(errp, "BCM2835 AUX: core clock must be connected");
        return;
    }
    qemu_chr_fe_set_handlers(&s->chr, bcm2835_aux_can_receive,
                             bcm2835_aux_receive, NULL, NULL, s, NULL, true);
}

static const Property bcm2835_aux_props[] = {
    DEFINE_PROP_CHR("chardev", BCM2835AuxState, chr),
};

static void bcm2835_aux_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = bcm2835_aux_realize;
    device_class_set_legacy_reset(dc, bcm2835_aux_reset);
    dc->vmsd = &vmstate_bcm2835_aux;
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
    device_class_set_props(dc, bcm2835_aux_props);
}

static const TypeInfo bcm2835_aux_info = {
    .name          = TYPE_BCM2835_AUX,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2835AuxState),
    .instance_init = bcm2835_aux_init,
    .instance_finalize = bcm2835_aux_finalize,
    .class_init    = bcm2835_aux_class_init,
};

static void bcm2835_aux_register_types(void)
{
    type_register_static(&bcm2835_aux_info);
}

type_init(bcm2835_aux_register_types)
