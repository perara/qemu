/*
 * Portable core for the QEMU Raspberry Pi USB GPIO adapter protocol.
 *
 * The core owns protocol parsing, polarity, debounce, session/watchdog, and
 * fail-safe policy.  Platform code supplies only raw GPIO operations and an
 * ASCII response sink.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef QGPIO_CORE_H
#define QGPIO_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define QGPIO_PROTOCOL_VERSION 1
#define QGPIO_MAX_LINES 32
#define QGPIO_MAX_LINE_LENGTH 128
#define QGPIO_SESSION_TIMEOUT_US 500000

typedef enum QGPIOBias {
    QGPIO_BIAS_AS_IS,
    QGPIO_BIAS_DISABLED,
    QGPIO_BIAS_PULL_UP,
    QGPIO_BIAS_PULL_DOWN,
} QGPIOBias;

typedef enum QGPIODrive {
    QGPIO_DRIVE_PUSH_PULL,
    QGPIO_DRIVE_OPEN_DRAIN,
    QGPIO_DRIVE_OPEN_SOURCE,
} QGPIODrive;

typedef struct QGPIOHal {
    bool (*configure_input)(void *opaque, unsigned int line, QGPIOBias bias);
    bool (*configure_output)(void *opaque, unsigned int line,
                             QGPIODrive drive, bool raw_value);
    bool (*write)(void *opaque, unsigned int line, QGPIODrive drive,
                  bool raw_value);
    bool (*read)(void *opaque, unsigned int line, bool *raw_value);
    void (*safe)(void *opaque);
    void (*emit)(void *opaque, const char *line);
    void *opaque;
} QGPIOHal;

typedef struct QGPIOLineState {
    bool configured;
    bool output;
    bool active_low;
    bool last_raw;
    bool stable_raw;
    uint32_t debounce_us;
    uint64_t raw_changed_us;
    QGPIODrive drive;
} QGPIOLineState;

typedef struct QGPIOCore {
    QGPIOHal hal;
    QGPIOLineState lines[QGPIO_MAX_LINES];
    unsigned int line_count;
    bool connected;
    bool session;
    bool discarding;
    uint64_t last_valid_us;
    char receive_line[QGPIO_MAX_LINE_LENGTH + 1];
    size_t receive_length;
} QGPIOCore;

bool qgpio_init(QGPIOCore *core, const QGPIOHal *hal,
                unsigned int line_count, uint64_t now_us);
void qgpio_set_connected(QGPIOCore *core, bool connected, uint64_t now_us);
void qgpio_receive(QGPIOCore *core, const void *data, size_t length,
                   uint64_t now_us);
void qgpio_poll(QGPIOCore *core, uint64_t now_us);
void qgpio_force_safe(QGPIOCore *core);

#endif
