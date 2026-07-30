/*
 * Rasperry Pi 2 emulation and refactoring Copyright (c) 2015, Microsoft
 * Written by Andrew Baumann
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef BCM2835_AUX_H
#define BCM2835_AUX_H

#include "hw/core/sysbus.h"
#include "hw/core/clock.h"
#include "hw/ssi/ssi.h"
#include "chardev/char-fe.h"
#include "qom/object.h"
#include "qemu/timer.h"

#define TYPE_BCM2835_AUX "bcm2835-aux"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2835AuxState, BCM2835_AUX)

#define BCM2835_AUX_RX_FIFO_LEN 8
#define BCM2835_AUX_TX_FIFO_LEN 8
#define BCM2835_AUX_SPI_COUNT 2
#define BCM2835_AUX_SPI_FIFO_LEN 4

typedef struct BCM2835AuxSPIState {
    BCM2835AuxState *parent;
    SSIBus *bus;
    QEMUTimer *timer;
    qemu_irq chip_select[3];

    uint32_t cntl0;
    uint32_t cntl1;
    uint32_t tx_fifo[BCM2835_AUX_SPI_FIFO_LEN];
    uint32_t rx_fifo[BCM2835_AUX_SPI_FIFO_LEN];
    bool tx_hold[BCM2835_AUX_SPI_FIFO_LEN];
    uint8_t tx_pos;
    uint8_t tx_count;
    uint8_t rx_pos;
    uint8_t rx_count;
    uint8_t index;

    uint32_t active_word;
    uint32_t input_shift;
    uint32_t last_rx;
    uint64_t remaining_core_cycles;
    uint8_t active_bits;
    uint8_t active_cs;
    bool active;
    bool active_hold;
    bool cs_held;
} BCM2835AuxSPIState;

struct BCM2835AuxState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion iomem;
    CharFrontend chr;
    qemu_irq irq;

    uint8_t read_fifo[BCM2835_AUX_RX_FIFO_LEN];
    uint8_t read_pos, read_count;
    uint8_t tx_fifo[BCM2835_AUX_TX_FIFO_LEN];
    uint8_t tx_pos, tx_count;
    QEMUTimer *tx_timer;
    Clock *core_clk;
    BCM2835AuxSPIState spi[BCM2835_AUX_SPI_COUNT];
    uint64_t tx_remaining_cycles;
    uint8_t ier, iir;
    uint8_t enables;
    uint8_t lcr;
    uint8_t mcr;
    uint8_t scratch;
    uint8_t cntl;
    uint16_t baud;
};

#endif
