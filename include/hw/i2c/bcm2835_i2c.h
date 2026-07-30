/*
 * Broadcom Serial Controller (BSC)
 *
 * Copyright (c) 2024 Rayhan Faizel <rayhan.faizel@gmail.com>
 *
 * SPDX-License-Identifier: MIT
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

#include "hw/core/sysbus.h"
#include "hw/core/clock.h"
#include "hw/i2c/i2c.h"
#include "qom/object.h"
#include "qemu/timer.h"

#define TYPE_BCM2835_I2C "bcm2835-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2835I2CState, BCM2835_I2C)

#define BCM2835_I2C_C       0x0                   /* Control */
#define BCM2835_I2C_S       0x4                   /* Status */
#define BCM2835_I2C_DLEN    0x8                   /* Data Length */
#define BCM2835_I2C_A       0xc                   /* Slave Address */
#define BCM2835_I2C_FIFO    0x10                  /* FIFO */
#define BCM2835_I2C_DIV     0x14                  /* Clock Divider */
#define BCM2835_I2C_DEL     0x18                  /* Data Delay */
#define BCM2835_I2C_CLKT    0x1c                  /* Clock Stretch Timeout */

#define BCM2835_I2C_C_I2CEN     BIT(15)           /* I2C enable */
#define BCM2835_I2C_C_INTR      BIT(10)           /* Interrupt on RXR */
#define BCM2835_I2C_C_INTT      BIT(9)            /* Interrupt on TXW */
#define BCM2835_I2C_C_INTD      BIT(8)            /* Interrupt on DONE */
#define BCM2835_I2C_C_ST        BIT(7)            /* Start transfer */
#define BCM2835_I2C_C_CLEAR     (BIT(5) | BIT(4)) /* Clear FIFO */
#define BCM2835_I2C_C_READ      BIT(0)            /* I2C read mode */
#define BCM2835_I2C_C_MASK      (BCM2835_I2C_C_I2CEN | \
                                 BCM2835_I2C_C_INTR | \
                                 BCM2835_I2C_C_INTT | \
                                 BCM2835_I2C_C_INTD | \
                                 BCM2835_I2C_C_READ)

#define BCM2835_I2C_S_CLKT      BIT(9)            /* Clock stretch timeout */
#define BCM2835_I2C_S_ERR       BIT(8)            /* Slave error */
#define BCM2835_I2C_S_RXF       BIT(7)            /* RX FIFO full */
#define BCM2835_I2C_S_TXE       BIT(6)            /* TX FIFO empty */
#define BCM2835_I2C_S_RXD       BIT(5)            /* RX bytes available */
#define BCM2835_I2C_S_TXD       BIT(4)            /* TX space available */
#define BCM2835_I2C_S_RXR       BIT(3)            /* RX FIFO needs reading */
#define BCM2835_I2C_S_TXW       BIT(2)            /* TX FIFO needs writing */
#define BCM2835_I2C_S_DONE      BIT(1)            /* I2C Transfer complete */
#define BCM2835_I2C_S_TA        BIT(0)            /* I2C Transfer active */

#define BCM2835_I2C_FIFO_LEN        16
#define BCM2835_I2C_RXR_THRESHOLD   12
#define BCM2835_I2C_TXW_THRESHOLD   4

struct BCM2835I2CState {
    /* <private> */
    SysBusDevice parent_obj;

    /* <public> */
    MemoryRegion iomem;
    I2CBus *bus;
    qemu_irq irq;
    Clock *core_clk;
    QEMUTimer *stretch_timer;

    uint32_t c;
    uint32_t s;
    uint32_t dlen;
    uint32_t a;
    uint32_t div;
    uint32_t del;
    uint32_t clkt;

    uint32_t last_dlen;
    uint8_t fifo[BCM2835_I2C_FIFO_LEN];
    uint8_t fifo_pos;
    uint8_t fifo_len;
    QEMUBH *transfer_bh;

    uint32_t clock_stretch_after;
    uint32_t clock_stretch_cycles;
    uint32_t transfer_bytes;
    uint32_t stretch_remaining_cycles;
    uint16_t ten_bit_address;
    bool stretch_injected;
    bool stretch_completed;
    bool stretch_waiting;
    bool stretch_timeout;
    bool ten_bit_address_valid;
    bool ten_bit_address_pending;
};
