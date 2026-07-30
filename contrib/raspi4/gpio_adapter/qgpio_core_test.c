/*
 * Host self-test for the portable QGPIO adapter firmware core.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qgpio_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_LINES 8
#define MAX_RESPONSES 64

typedef struct TestHal {
    bool raw[TEST_LINES];
    bool output[TEST_LINES];
    QGPIODrive drive[TEST_LINES];
    unsigned int safe_count;
    char responses[MAX_RESPONSES][64];
    unsigned int response_count;
} TestHal;

static void check(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "qgpio-core-test: %s\n", message);
        exit(EXIT_FAILURE);
    }
}

static bool configure_input(void *opaque, unsigned int line, QGPIOBias bias)
{
    TestHal *hal = opaque;

    (void)bias;
    hal->output[line] = false;
    return true;
}

static bool configure_output(void *opaque, unsigned int line,
                             QGPIODrive drive, bool raw_value)
{
    TestHal *hal = opaque;

    hal->raw[line] = raw_value;
    hal->drive[line] = drive;
    hal->output[line] = true;
    return true;
}

static bool write_line(void *opaque, unsigned int line,
                       QGPIODrive drive, bool raw_value)
{
    TestHal *hal = opaque;

    if (!hal->output[line] || hal->drive[line] != drive) {
        return false;
    }
    hal->raw[line] = raw_value;
    return true;
}

static bool read_line(void *opaque, unsigned int line, bool *raw_value)
{
    TestHal *hal = opaque;

    *raw_value = hal->raw[line];
    return true;
}

static void safe_lines(void *opaque)
{
    TestHal *hal = opaque;
    unsigned int line;

    for (line = 0; line < TEST_LINES; line++) {
        hal->output[line] = false;
    }
    hal->safe_count++;
}

static void emit(void *opaque, const char *line)
{
    TestHal *hal = opaque;

    check(hal->response_count < MAX_RESPONSES, "response overflow");
    snprintf(hal->responses[hal->response_count++],
             sizeof(hal->responses[0]), "%s", line);
}

static void send_line(QGPIOCore *core, const char *line, uint64_t now_us)
{
    qgpio_receive(core, line, strlen(line), now_us);
    qgpio_receive(core, "\n", 1, now_us);
}

static const char *last_response(TestHal *hal)
{
    check(hal->response_count != 0, "missing response");
    return hal->responses[hal->response_count - 1];
}

static void start_session(QGPIOCore *core, TestHal *hal, uint64_t now_us)
{
    qgpio_set_connected(core, true, now_us);
    send_line(core, "HELLO 1", now_us);
    check(!strcmp(last_response(hal), "QGPIO 1 8"), "bad HELLO response");
    check(core->session, "HELLO did not establish session");
}

static void test_input_debounce_and_polarity(QGPIOCore *core, TestHal *hal)
{
    start_session(core, hal, 1000);
    hal->raw[3] = true;
    send_line(core, "CONFIG 3 IN 1 pull-up 100", 2000);
    check(!strcmp(last_response(hal), "OK"), "input CONFIG failed");
    send_line(core, "READ 3", 3000);
    check(!strcmp(last_response(hal), "VALUE 3 0"),
          "active-low READ was not logical");

    hal->raw[3] = false;
    qgpio_poll(core, 3050);
    check(strcmp(last_response(hal), "EDGE 3 1"),
          "edge escaped before debounce");
    qgpio_poll(core, 3149);
    check(strcmp(last_response(hal), "EDGE 3 1"),
          "edge escaped one microsecond before debounce");
    qgpio_poll(core, 3150);
    check(!strcmp(last_response(hal), "EDGE 3 1"),
          "debounced active-low edge missing");
}

static void test_atomic_output_and_safe(QGPIOCore *core, TestHal *hal)
{
    send_line(core, "CONFIG 4 OUT 1 open-drain 1", 4000);
    check(hal->output[4], "output was not enabled");
    check(!hal->raw[4], "active-low initial output was not atomic");
    check(hal->drive[4] == QGPIO_DRIVE_OPEN_DRAIN, "drive mode lost");
    send_line(core, "WRITE 4 0", 5000);
    check(hal->raw[4], "active-low WRITE was not translated");
    send_line(core, "SAFE", 6000);
    check(!hal->output[4], "SAFE retained output ownership");
    check(!strcmp(last_response(hal), "OK"), "SAFE was not acknowledged");
}

static void test_protocol_failure_and_recovery(QGPIOCore *core, TestHal *hal)
{
    unsigned int safe_before = hal->safe_count;

    send_line(core, "CONFIG 4 OUT 0 push-pull 1 trailing", 7000);
    check(!core->session, "malformed command retained session");
    check(hal->safe_count == safe_before + 1,
          "malformed command did not enter SAFE");
    check(!strcmp(last_response(hal), "ERR syntax"),
          "malformed command response changed");
    send_line(core, "PING", 8000);
    check(!strcmp(last_response(hal), "ERR state"),
          "command before HELLO was accepted");
    send_line(core, "HELLO 1", 9000);
    check(core->session, "fresh HELLO did not recover session");
}

static void test_watchdog_disconnect_and_overflow(QGPIOCore *core, TestHal *hal)
{
    unsigned int safe_before;
    char oversized[QGPIO_MAX_LINE_LENGTH + 3];

    send_line(core, "CONFIG 4 OUT 0 push-pull 1", 10000);
    safe_before = hal->safe_count;
    qgpio_poll(core, 509999);
    check(hal->output[4], "watchdog fired too early");
    qgpio_poll(core, 510000);
    check(!core->session && !hal->output[4],
          "watchdog did not release output at 500 ms");
    check(hal->safe_count == safe_before + 1, "watchdog SAFE count wrong");

    start_session(core, hal, 600000);
    send_line(core, "CONFIG 4 OUT 0 push-pull 1", 601000);
    qgpio_set_connected(core, false, 602000);
    check(!core->session && !hal->output[4],
          "disconnect did not release output");

    start_session(core, hal, 700000);
    memset(oversized, 'A', sizeof(oversized));
    oversized[sizeof(oversized) - 1] = '\0';
    send_line(core, oversized, 701000);
    check(!strcmp(last_response(hal), "ERR line-too-long"),
          "overlong line did not fail closed");
    check(!core->session, "overlong line retained session");
}

int main(void)
{
    TestHal test_hal = { 0 };
    QGPIOHal hal = {
        .configure_input = configure_input,
        .configure_output = configure_output,
        .write = write_line,
        .read = read_line,
        .safe = safe_lines,
        .emit = emit,
        .opaque = &test_hal,
    };
    QGPIOCore core;

    check(qgpio_init(&core, &hal, TEST_LINES, 0), "initialization failed");
    test_input_debounce_and_polarity(&core, &test_hal);
    test_atomic_output_and_safe(&core, &test_hal);
    test_protocol_failure_and_recovery(&core, &test_hal);
    test_watchdog_disconnect_and_overflow(&core, &test_hal);
    puts("qgpio adapter firmware core self-test: PASS");
    return EXIT_SUCCESS;
}
