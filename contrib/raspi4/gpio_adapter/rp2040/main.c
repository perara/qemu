/*
 * RP2040 USB CDC ACM frontend for the portable QGPIO protocol core.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qgpio_core.h"

#include "hardware/gpio.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "tusb.h"

#include <stdio.h>

#define QGPIO_RP2040_LINES 30
#define QGPIO_LOOP_WATCHDOG_MS 250

#ifndef QGPIO_RP2040_ALLOWED_MASK
/* Raspberry Pi Pico header GPIOs: GP0..22 and GP26..28. */
#define QGPIO_RP2040_ALLOWED_MASK 0x1c7fffffu
#endif

typedef struct RP2040Hal {
    QGPIODrive drive[QGPIO_RP2040_LINES];
} RP2040Hal;

static bool rp2040_line_allowed(unsigned int line)
{
    return line < QGPIO_RP2040_LINES &&
           (QGPIO_RP2040_ALLOWED_MASK & (1u << line));
}

static bool rp2040_configure_input(void *opaque, unsigned int line,
                                   QGPIOBias bias)
{
    (void)opaque;
    if (!rp2040_line_allowed(line)) {
        return false;
    }
    gpio_init(line);
    gpio_set_dir(line, GPIO_IN);
    switch (bias) {
    case QGPIO_BIAS_AS_IS:
        break;
    case QGPIO_BIAS_DISABLED:
        gpio_disable_pulls(line);
        break;
    case QGPIO_BIAS_PULL_UP:
        gpio_pull_up(line);
        break;
    case QGPIO_BIAS_PULL_DOWN:
        gpio_pull_down(line);
        break;
    default:
        return false;
    }
    return true;
}

static bool rp2040_drive(unsigned int line, QGPIODrive drive, bool raw_value)
{
    switch (drive) {
    case QGPIO_DRIVE_PUSH_PULL:
        /* Set the latch before direction to avoid an opposite-level glitch. */
        gpio_put(line, raw_value);
        gpio_set_dir(line, GPIO_OUT);
        return true;
    case QGPIO_DRIVE_OPEN_DRAIN:
        if (raw_value) {
            gpio_set_dir(line, GPIO_IN);
        } else {
            gpio_put(line, false);
            gpio_set_dir(line, GPIO_OUT);
        }
        return true;
    case QGPIO_DRIVE_OPEN_SOURCE:
        if (raw_value) {
            gpio_put(line, true);
            gpio_set_dir(line, GPIO_OUT);
        } else {
            gpio_set_dir(line, GPIO_IN);
        }
        return true;
    default:
        return false;
    }
}

static bool rp2040_configure_output(void *opaque, unsigned int line,
                                    QGPIODrive drive, bool raw_value)
{
    RP2040Hal *hal = opaque;

    if (!rp2040_line_allowed(line)) {
        return false;
    }
    gpio_init(line);
    gpio_disable_pulls(line);
    hal->drive[line] = drive;
    return rp2040_drive(line, drive, raw_value);
}

static bool rp2040_write(void *opaque, unsigned int line,
                         QGPIODrive drive, bool raw_value)
{
    RP2040Hal *hal = opaque;

    if (!rp2040_line_allowed(line) || hal->drive[line] != drive) {
        return false;
    }
    return rp2040_drive(line, drive, raw_value);
}

static bool rp2040_read(void *opaque, unsigned int line, bool *raw_value)
{
    (void)opaque;
    if (!rp2040_line_allowed(line)) {
        return false;
    }
    *raw_value = gpio_get(line);
    return true;
}

static void rp2040_safe(void *opaque)
{
    unsigned int line;

    (void)opaque;
    for (line = 0; line < QGPIO_RP2040_LINES; line++) {
        if (!rp2040_line_allowed(line)) {
            continue;
        }
        gpio_init(line);
        gpio_set_dir(line, GPIO_IN);
        gpio_disable_pulls(line);
    }
}

static void rp2040_emit(void *opaque, const char *line)
{
    (void)opaque;
    puts(line);
}

int main(void)
{
    RP2040Hal rp2040 = { 0 };
    QGPIOHal hal = {
        .configure_input = rp2040_configure_input,
        .configure_output = rp2040_configure_output,
        .write = rp2040_write,
        .read = rp2040_read,
        .safe = rp2040_safe,
        .emit = rp2040_emit,
        .opaque = &rp2040,
    };
    QGPIOCore core;
    bool connected = false;

    stdio_init_all();
    if (!qgpio_init(&core, &hal, QGPIO_RP2040_LINES, time_us_64())) {
        rp2040_safe(&rp2040);
        watchdog_reboot(0, 0, 10);
    }
    /*
     * A stalled main loop resets the MCU.  RP2040 reset defaults restore GPIO
     * inputs before this program's explicit SAFE initialization runs.
     */
    watchdog_enable(QGPIO_LOOP_WATCHDOG_MS, false);

    for (;;) {
        bool link = stdio_usb_connected() && tud_mounted() && !tud_suspended();
        int character;

        if (link != connected) {
            connected = link;
            qgpio_set_connected(&core, connected, time_us_64());
        }
        while ((character = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
            unsigned char byte = character;

            qgpio_receive(&core, &byte, 1, time_us_64());
        }
        qgpio_poll(&core, time_us_64());
        watchdog_update();
        sleep_us(500);
    }
}
