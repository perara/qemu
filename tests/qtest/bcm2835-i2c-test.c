/*
 * QTest testcase for Broadcom Serial Controller (BSC)
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

#include "qemu/osdep.h"
#include "libqtest-single.h"

#include "hw/i2c/bcm2835_i2c.h"
#include "hw/sensor/tmp105_regs.h"

static const uint32_t bsc_base_addrs[] = {
    0x3f205000,                         /* I2C0 */
    0x3f804000,                         /* I2C1 */
    0x3f805000,                         /* I2C2 */
};

static void bcm2835_i2c_init_transfer(uint32_t base_addr, bool read)
{
    /* read flag is bit 0 so we can write it directly */
    int interrupt = read ? BCM2835_I2C_C_INTR : BCM2835_I2C_C_INTT;

    writel(base_addr + BCM2835_I2C_C,
           BCM2835_I2C_C_I2CEN | BCM2835_I2C_C_INTD |
           BCM2835_I2C_C_ST | BCM2835_I2C_C_CLEAR | interrupt | read);
}

static void test_i2c_read_write(gconstpointer data)
{
    uint32_t i2cdata;
    intptr_t index = (intptr_t) data;
    uint32_t base_addr = bsc_base_addrs[index];

    /* Write to TMP105 register */
    writel(base_addr + BCM2835_I2C_A, 0x50);
    writel(base_addr + BCM2835_I2C_DLEN, 3);

    bcm2835_i2c_init_transfer(base_addr, 0);

    writel(base_addr + BCM2835_I2C_FIFO, TMP105_REG_T_HIGH);
    writel(base_addr + BCM2835_I2C_FIFO, 0xde);
    writel(base_addr + BCM2835_I2C_FIFO, 0xad);

    /* Clear flags */
    writel(base_addr + BCM2835_I2C_S, BCM2835_I2C_S_DONE | BCM2835_I2C_S_ERR |
                                      BCM2835_I2C_S_CLKT);

    /* Read from TMP105 register */
    writel(base_addr + BCM2835_I2C_A, 0x50);
    writel(base_addr + BCM2835_I2C_DLEN, 1);

    bcm2835_i2c_init_transfer(base_addr, 0);

    writel(base_addr + BCM2835_I2C_FIFO, TMP105_REG_T_HIGH);

    writel(base_addr + BCM2835_I2C_DLEN, 2);
    bcm2835_i2c_init_transfer(base_addr, 1);

    i2cdata = readl(base_addr + BCM2835_I2C_FIFO);
    g_assert_cmpint(i2cdata, ==, 0xde);

    i2cdata = readl(base_addr + BCM2835_I2C_FIFO);
    g_assert_cmpint(i2cdata, ==, 0xa0);

    /* Clear flags */
    writel(base_addr + BCM2835_I2C_S, BCM2835_I2C_S_DONE | BCM2835_I2C_S_ERR |
                                      BCM2835_I2C_S_CLKT);

}

static void test_i2c_control_error_reset(gconstpointer data)
{
    intptr_t index = (intptr_t)data;
    uint32_t base_addr = bsc_base_addrs[index];
    uint32_t status;

    writel(base_addr + BCM2835_I2C_S, BCM2835_I2C_S_DONE |
                                      BCM2835_I2C_S_ERR |
                                      BCM2835_I2C_S_CLKT);

    /* Architectural field masks and one-shot command bits. */
    writel(base_addr + BCM2835_I2C_A, 0xd1);
    g_assert_cmphex(readl(base_addr + BCM2835_I2C_A), ==, 0x51);
    writel(base_addr + BCM2835_I2C_DLEN, 0x10001);
    g_assert_cmphex(readl(base_addr + BCM2835_I2C_DLEN), ==, 1);
    writel(base_addr + BCM2835_I2C_DIV, 0x12345678);
    g_assert_cmphex(readl(base_addr + BCM2835_I2C_DIV), ==, 0x5678);
    writel(base_addr + BCM2835_I2C_DEL, 0x12345678);
    g_assert_cmphex(readl(base_addr + BCM2835_I2C_DEL), ==, 0x12345678);
    writel(base_addr + BCM2835_I2C_CLKT, 0x10040);
    g_assert_cmphex(readl(base_addr + BCM2835_I2C_CLKT), ==, 0x40);

    /* Neither ST without I2CEN nor I2CEN without ST starts a transfer. */
    writel(base_addr + BCM2835_I2C_C,
           BCM2835_I2C_C_ST | BCM2835_I2C_C_INTD);
    g_assert_cmphex(readl(base_addr + BCM2835_I2C_C), ==,
                    BCM2835_I2C_C_INTD);
    status = readl(base_addr + BCM2835_I2C_S);
    g_assert_false(status & (BCM2835_I2C_S_TA | BCM2835_I2C_S_DONE |
                             BCM2835_I2C_S_ERR));

    writel(base_addr + BCM2835_I2C_C,
           BCM2835_I2C_C_I2CEN | BCM2835_I2C_C_INTD);
    status = readl(base_addr + BCM2835_I2C_S);
    g_assert_false(status & (BCM2835_I2C_S_TA | BCM2835_I2C_S_DONE |
                             BCM2835_I2C_S_ERR));

    /* An address NACK completes with ERR|DONE and never remains active. */
    writel(base_addr + BCM2835_I2C_C,
           BCM2835_I2C_C_I2CEN | BCM2835_I2C_C_INTD |
           BCM2835_I2C_C_ST | BCM2835_I2C_C_CLEAR);
    g_assert_cmphex(readl(base_addr + BCM2835_I2C_C), ==,
                    BCM2835_I2C_C_I2CEN | BCM2835_I2C_C_INTD);
    status = readl(base_addr + BCM2835_I2C_S);
    g_assert_true(status & BCM2835_I2C_S_DONE);
    g_assert_true(status & BCM2835_I2C_S_ERR);
    g_assert_false(status & BCM2835_I2C_S_TA);

    writel(base_addr + BCM2835_I2C_S,
           BCM2835_I2C_S_DONE | BCM2835_I2C_S_ERR);
    status = readl(base_addr + BCM2835_I2C_S);
    g_assert_false(status & (BCM2835_I2C_S_DONE | BCM2835_I2C_S_ERR));

    /* Reset terminates any live bus transaction and restores all registers. */
    writel(base_addr + BCM2835_I2C_A, 0x50);
    writel(base_addr + BCM2835_I2C_DLEN, 1);
    writel(base_addr + BCM2835_I2C_C,
           BCM2835_I2C_C_I2CEN | BCM2835_I2C_C_INTT |
           BCM2835_I2C_C_ST);
    g_assert_true(readl(base_addr + BCM2835_I2C_S) & BCM2835_I2C_S_TA);
    qtest_system_reset(global_qtest);

    g_assert_cmphex(readl(base_addr + BCM2835_I2C_C), ==, 0);
    g_assert_cmphex(readl(base_addr + BCM2835_I2C_DLEN), ==, 0);
    g_assert_cmphex(readl(base_addr + BCM2835_I2C_A), ==, 0);
    g_assert_cmphex(readl(base_addr + BCM2835_I2C_DIV), ==, 0x5dc);
    g_assert_cmphex(readl(base_addr + BCM2835_I2C_DEL), ==, 0x00300030);
    g_assert_cmphex(readl(base_addr + BCM2835_I2C_CLKT), ==, 0x40);
    status = readl(base_addr + BCM2835_I2C_S);
    g_assert_cmphex(status, ==,
                    BCM2835_I2C_S_TXD | BCM2835_I2C_S_TXE);
}

static void test_i2c_ten_bit_addressing(void)
{
    uint32_t base_addr = bsc_base_addrs[0];
    uint32_t status;

    /* 10-bit address 0x2aa uses 1111010 in A and 0xaa in the FIFO. */
    writel(base_addr + BCM2835_I2C_A, 0x7a);
    writel(base_addr + BCM2835_I2C_DLEN, 4);
    bcm2835_i2c_init_transfer(base_addr, false);
    writel(base_addr + BCM2835_I2C_FIFO, 0xaa);
    writel(base_addr + BCM2835_I2C_FIFO, TMP105_REG_T_HIGH);
    writel(base_addr + BCM2835_I2C_FIFO, 0xde);
    writel(base_addr + BCM2835_I2C_FIFO, 0xad);
    status = readl(base_addr + BCM2835_I2C_S);
    g_assert_true(status & BCM2835_I2C_S_DONE);
    g_assert_false(status & (BCM2835_I2C_S_ERR | BCM2835_I2C_S_TA));

    writel(base_addr + BCM2835_I2C_S,
           BCM2835_I2C_S_DONE | BCM2835_I2C_S_ERR);

    /* Select the register through the documented 10-bit write phase. */
    writel(base_addr + BCM2835_I2C_DLEN, 2);
    bcm2835_i2c_init_transfer(base_addr, false);
    writel(base_addr + BCM2835_I2C_FIFO, 0xaa);
    writel(base_addr + BCM2835_I2C_FIFO, TMP105_REG_T_HIGH);
    g_assert_true(readl(base_addr + BCM2835_I2C_S) & BCM2835_I2C_S_DONE);

    writel(base_addr + BCM2835_I2C_S, BCM2835_I2C_S_DONE);
    writel(base_addr + BCM2835_I2C_DLEN, 2);
    bcm2835_i2c_init_transfer(base_addr, true);
    g_assert_cmphex(readl(base_addr + BCM2835_I2C_FIFO), ==, 0xde);
    g_assert_cmphex(readl(base_addr + BCM2835_I2C_FIFO), ==, 0xa0);

    /* A different low address byte must NACK and terminate cleanly. */
    writel(base_addr + BCM2835_I2C_S,
           BCM2835_I2C_S_DONE | BCM2835_I2C_S_ERR);
    writel(base_addr + BCM2835_I2C_DLEN, 1);
    bcm2835_i2c_init_transfer(base_addr, false);
    writel(base_addr + BCM2835_I2C_FIFO, 0xab);
    status = readl(base_addr + BCM2835_I2C_S);
    g_assert_true(status & BCM2835_I2C_S_DONE);
    g_assert_true(status & BCM2835_I2C_S_ERR);
    g_assert_false(status & BCM2835_I2C_S_TA);
}

static void test_i2c_data_nack(void)
{
    uint32_t base_addr = bsc_base_addrs[0];
    uint32_t status;

    writel(base_addr + BCM2835_I2C_S,
           BCM2835_I2C_S_DONE | BCM2835_I2C_S_ERR);
    writel(base_addr + BCM2835_I2C_A, 0x53);
    writel(base_addr + BCM2835_I2C_DLEN, 3);
    bcm2835_i2c_init_transfer(base_addr, false);
    writel(base_addr + BCM2835_I2C_FIFO, TMP105_REG_T_HIGH);
    writel(base_addr + BCM2835_I2C_FIFO, 0xde);
    status = readl(base_addr + BCM2835_I2C_S);
    g_assert_true(status & BCM2835_I2C_S_DONE);
    g_assert_true(status & BCM2835_I2C_S_ERR);
    g_assert_false(status & BCM2835_I2C_S_TA);

    /* STOP resets the target transaction and a different device recovers. */
    writel(base_addr + BCM2835_I2C_S,
           BCM2835_I2C_S_DONE | BCM2835_I2C_S_ERR);
    writel(base_addr + BCM2835_I2C_A, 0x50);
    writel(base_addr + BCM2835_I2C_DLEN, 1);
    bcm2835_i2c_init_transfer(base_addr, false);
    writel(base_addr + BCM2835_I2C_FIFO, TMP105_REG_T_HIGH);
    status = readl(base_addr + BCM2835_I2C_S);
    g_assert_true(status & BCM2835_I2C_S_DONE);
    g_assert_false(status & BCM2835_I2C_S_ERR);
}

int main(int argc, char **argv)
{
    int ret;
    int i;

    g_test_init(&argc, &argv, NULL);

    for (i = 0; i < 3; i++) {
        g_autofree char *test_name =
            g_strdup_printf("/bcm2835/bcm2835-i2c%d/read_write", i);
        g_autofree char *contract_test_name =
            g_strdup_printf(
                "/bcm2835/bcm2835-i2c%d/control_error_reset", i);

        qtest_add_data_func(test_name, (void *)(intptr_t) i,
                            test_i2c_read_write);
        qtest_add_data_func(contract_test_name, (void *)(intptr_t)i,
                            test_i2c_control_error_reset);
    }
    qtest_add_func("/bcm2835/bcm2835-i2c0/ten-bit-addressing",
                   test_i2c_ten_bit_addressing);
    qtest_add_func("/bcm2835/bcm2835-i2c0/data-nack",
                   test_i2c_data_nack);

    /* Run I2C tests with TMP105 slaves on all three buses */
    qtest_start("-M raspi3b "
                "-device tmp105,address=0x50,bus=i2c-bus.0 "
                "-device tmp105,address=0x52,ten-bit-address=0x2aa,"
                "bus=i2c-bus.0 "
                "-device tmp105,address=0x53,test-nack-after=1,"
                "bus=i2c-bus.0 "
                "-device tmp105,address=0x50,bus=i2c-bus.1 "
                "-device tmp105,address=0x50,bus=i2c-bus.2");
    ret = g_test_run();
    qtest_end();

    return ret;
}
