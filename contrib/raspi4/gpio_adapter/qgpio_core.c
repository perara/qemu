/*
 * Portable QGPIO protocol core.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qgpio_core.h"

#include <stdio.h>
#include <string.h>

static void qgpio_reset_lines(QGPIOCore *core)
{
    memset(core->lines, 0, sizeof(core->lines));
}

void qgpio_force_safe(QGPIOCore *core)
{
    core->hal.safe(core->hal.opaque);
    qgpio_reset_lines(core);
}

static void qgpio_fail(QGPIOCore *core, const char *reason)
{
    char response[64];

    qgpio_force_safe(core);
    core->session = false;
    snprintf(response, sizeof(response), "ERR %s", reason);
    core->hal.emit(core->hal.opaque, response);
}

static size_t qgpio_split(char *line, char **fields, size_t capacity)
{
    size_t count = 0;
    char *cursor = line;

    while (*cursor) {
        while (*cursor == ' ') {
            cursor++;
        }
        if (!*cursor) {
            break;
        }
        if (count == capacity) {
            return capacity + 1;
        }
        fields[count++] = cursor;
        while (*cursor && *cursor != ' ') {
            cursor++;
        }
        if (*cursor) {
            *cursor++ = '\0';
        }
    }
    return count;
}

static bool qgpio_unsigned(const char *text, unsigned long maximum,
                           unsigned long *value)
{
    unsigned long parsed = 0;
    const char *cursor;

    if (!*text) {
        return false;
    }
    for (cursor = text; *cursor; cursor++) {
        unsigned int digit;

        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
        digit = *cursor - '0';
        if (digit > maximum || parsed > (maximum - digit) / 10) {
            return false;
        }
        parsed = parsed * 10 + digit;
    }
    *value = parsed;
    return true;
}

static bool qgpio_bias(const char *text, QGPIOBias *bias)
{
    static const char *const names[] = {
        [QGPIO_BIAS_AS_IS] = "as-is",
        [QGPIO_BIAS_DISABLED] = "disabled",
        [QGPIO_BIAS_PULL_UP] = "pull-up",
        [QGPIO_BIAS_PULL_DOWN] = "pull-down",
    };
    unsigned int i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!strcmp(text, names[i])) {
            *bias = i;
            return true;
        }
    }
    return false;
}

static bool qgpio_drive(const char *text, QGPIODrive *drive)
{
    static const char *const names[] = {
        [QGPIO_DRIVE_PUSH_PULL] = "push-pull",
        [QGPIO_DRIVE_OPEN_DRAIN] = "open-drain",
        [QGPIO_DRIVE_OPEN_SOURCE] = "open-source",
    };
    unsigned int i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!strcmp(text, names[i])) {
            *drive = i;
            return true;
        }
    }
    return false;
}

static void qgpio_ok(QGPIOCore *core, uint64_t now_us)
{
    core->last_valid_us = now_us;
    core->hal.emit(core->hal.opaque, "OK");
}

static void qgpio_process_line(QGPIOCore *core, char *line, uint64_t now_us)
{
    char *fields[7];
    size_t count = qgpio_split(line, fields, 7);
    unsigned long line_number, flag, value, debounce;
    QGPIOLineState *state;
    QGPIOBias bias;
    QGPIODrive drive;
    bool raw;
    char response[64];

    if (count == 2 && !strcmp(fields[0], "HELLO") &&
        !strcmp(fields[1], "1")) {
        qgpio_force_safe(core);
        core->session = true;
        core->last_valid_us = now_us;
        snprintf(response, sizeof(response), "QGPIO %u %u",
                 QGPIO_PROTOCOL_VERSION, core->line_count);
        core->hal.emit(core->hal.opaque, response);
        return;
    }
    if (!core->session) {
        qgpio_fail(core, "state");
        return;
    }
    if (count == 1 && !strcmp(fields[0], "PING")) {
        qgpio_ok(core, now_us);
        return;
    }
    if (count == 1 && !strcmp(fields[0], "SAFE")) {
        qgpio_force_safe(core);
        qgpio_ok(core, now_us);
        return;
    }
    if (count == 6 && !strcmp(fields[0], "CONFIG") &&
        !strcmp(fields[2], "IN") &&
        qgpio_unsigned(fields[1], core->line_count - 1, &line_number) &&
        qgpio_unsigned(fields[3], 1, &flag) &&
        qgpio_bias(fields[4], &bias) &&
        qgpio_unsigned(fields[5], 1000000, &debounce)) {
        state = &core->lines[line_number];
        if (!core->hal.configure_input(core->hal.opaque, line_number, bias) ||
            !core->hal.read(core->hal.opaque, line_number, &raw)) {
            qgpio_fail(core, "io");
            return;
        }
        memset(state, 0, sizeof(*state));
        state->configured = true;
        state->active_low = flag;
        state->debounce_us = debounce;
        state->last_raw = raw;
        state->stable_raw = raw;
        state->raw_changed_us = now_us;
        qgpio_ok(core, now_us);
        return;
    }
    if (count == 6 && !strcmp(fields[0], "CONFIG") &&
        !strcmp(fields[2], "OUT") &&
        qgpio_unsigned(fields[1], core->line_count - 1, &line_number) &&
        qgpio_unsigned(fields[3], 1, &flag) &&
        qgpio_drive(fields[4], &drive) &&
        qgpio_unsigned(fields[5], 1, &value)) {
        raw = !!value ^ !!flag;
        if (!core->hal.configure_output(core->hal.opaque, line_number,
                                        drive, raw)) {
            qgpio_fail(core, "io");
            return;
        }
        state = &core->lines[line_number];
        memset(state, 0, sizeof(*state));
        state->configured = true;
        state->output = true;
        state->active_low = flag;
        state->drive = drive;
        qgpio_ok(core, now_us);
        return;
    }
    if (count == 3 && !strcmp(fields[0], "WRITE") &&
        qgpio_unsigned(fields[1], core->line_count - 1, &line_number) &&
        qgpio_unsigned(fields[2], 1, &value)) {
        state = &core->lines[line_number];
        if (!state->configured || !state->output) {
            qgpio_fail(core, "state");
            return;
        }
        raw = !!value ^ state->active_low;
        if (!core->hal.write(core->hal.opaque, line_number, state->drive,
                             raw)) {
            qgpio_fail(core, "io");
            return;
        }
        qgpio_ok(core, now_us);
        return;
    }
    if (count == 2 && !strcmp(fields[0], "READ") &&
        qgpio_unsigned(fields[1], core->line_count - 1, &line_number)) {
        state = &core->lines[line_number];
        if (!state->configured || state->output ||
            !core->hal.read(core->hal.opaque, line_number, &raw)) {
            qgpio_fail(core, "state");
            return;
        }
        snprintf(response, sizeof(response), "VALUE %lu %u", line_number,
                 !!raw ^ state->active_low);
        core->last_valid_us = now_us;
        core->hal.emit(core->hal.opaque, response);
        return;
    }
    qgpio_fail(core, "syntax");
}

bool qgpio_init(QGPIOCore *core, const QGPIOHal *hal,
                unsigned int line_count, uint64_t now_us)
{
    if (!core || !hal || !hal->configure_input || !hal->configure_output ||
        !hal->write || !hal->read || !hal->safe || !hal->emit ||
        !line_count || line_count > QGPIO_MAX_LINES) {
        return false;
    }
    memset(core, 0, sizeof(*core));
    core->hal = *hal;
    core->line_count = line_count;
    core->last_valid_us = now_us;
    qgpio_force_safe(core);
    return true;
}

void qgpio_set_connected(QGPIOCore *core, bool connected, uint64_t now_us)
{
    if (core->connected == connected) {
        return;
    }
    qgpio_force_safe(core);
    core->session = false;
    core->receive_length = 0;
    core->discarding = false;
    core->connected = connected;
    core->last_valid_us = now_us;
}

void qgpio_receive(QGPIOCore *core, const void *data, size_t length,
                   uint64_t now_us)
{
    const unsigned char *bytes = data;
    size_t i;

    if (!core->connected) {
        return;
    }
    for (i = 0; i < length; i++) {
        unsigned char byte = bytes[i];

        if (core->discarding) {
            if (byte == '\n') {
                core->discarding = false;
                core->receive_length = 0;
            }
            continue;
        }
        if (byte == '\n') {
            if (core->receive_length &&
                core->receive_line[core->receive_length - 1] == '\r') {
                core->receive_length--;
            }
            core->receive_line[core->receive_length] = '\0';
            qgpio_process_line(core, core->receive_line, now_us);
            core->receive_length = 0;
            continue;
        }
        if (core->receive_length == QGPIO_MAX_LINE_LENGTH) {
            qgpio_fail(core, "line-too-long");
            core->discarding = true;
            core->receive_length = 0;
            continue;
        }
        if (byte < 0x20 || byte > 0x7e) {
            qgpio_fail(core, "encoding");
            core->discarding = true;
            core->receive_length = 0;
            continue;
        }
        core->receive_line[core->receive_length++] = byte;
    }
}

void qgpio_poll(QGPIOCore *core, uint64_t now_us)
{
    unsigned int line;

    if (!core->connected || !core->session) {
        return;
    }
    if (now_us - core->last_valid_us >= QGPIO_SESSION_TIMEOUT_US) {
        qgpio_force_safe(core);
        core->session = false;
        return;
    }
    for (line = 0; line < core->line_count; line++) {
        QGPIOLineState *state = &core->lines[line];
        bool raw;
        char response[64];

        if (!state->configured || state->output) {
            continue;
        }
        if (!core->hal.read(core->hal.opaque, line, &raw)) {
            qgpio_fail(core, "io");
            return;
        }
        if (raw != state->last_raw) {
            state->last_raw = raw;
            state->raw_changed_us = now_us;
        }
        if (raw != state->stable_raw &&
            now_us - state->raw_changed_us >= state->debounce_us) {
            state->stable_raw = raw;
            snprintf(response, sizeof(response), "EDGE %u %u", line,
                     !!raw ^ state->active_low);
            core->hal.emit(core->hal.opaque, response);
        }
    }
}
