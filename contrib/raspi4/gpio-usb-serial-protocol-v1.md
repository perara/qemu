<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# QEMU Raspberry Pi USB GPIO protocol v1

This protocol connects `gpio_proxy.py` to a dedicated USB CDC ACM GPIO
adapter without replacing the emulated BCM2711 GPIO controller. It transports
logical pin state only. Voltage levels, current limits, contention, isolation,
and analog timing remain properties of the protected hardware fixture.

## Transport

- USB CDC ACM or equivalent raw TTY, nominally 115200 baud, 8N1.
- ASCII commands and responses terminated by LF; CR before LF is accepted.
- Lines are at most 128 bytes, excluding the terminator.
- The host issues one command at a time. `EDGE` is asynchronous and may be
  observed between the command and its response; the host queues it without
  losing the synchronous response.
- Unknown, malformed, out-of-order, or unsupported input returns
  `ERR <reason>` and puts every line in `SAFE`.

## Session and commands

The host begins every session with `HELLO 1`. The adapter responds with:

```text
QGPIO 1 <line-count>
```

The remaining commands and responses are:

```text
CONFIG <line> IN  <active-low> <bias>  <debounce-us>   -> OK
CONFIG <line> OUT <active-low> <drive> <initial-value> -> OK
WRITE  <line> <value>                                  -> OK
READ   <line>                                          -> VALUE <line> <value>
PING                                                   -> OK
SAFE                                                   -> OK
```

`active-low` and values are `0` or `1`. `bias` is `as-is`, `disabled`,
`pull-up`, or `pull-down`. `drive` is `push-pull`, `open-drain`, or
`open-source`. Values are logical values after polarity handling.

The host configuration may map adapter lines to the dedicated input-only QEMU
board signals `EEPROM_NWP` and `SD_OVERCURRENT`. The MCU protocol remains
line-numbered and unchanged; `gpio_proxy.py` translates those physical lines
to semantic `SET SIGNAL EEPROM_NWP` and
`SET SIGNAL SD_OVERCURRENT` commands on the QEMU socket. This avoids
presenting board-level power and EEPROM signals as guest-visible BCM2711
GPIOs.

The adapter emits input transitions as complete lines:

```text
EDGE <line> <value>
```

`CONFIG ... OUT` must set the initial value before enabling the output so no
opposite-level glitch is exposed. `SAFE` makes every line an input/high
impedance and cancels all output ownership.

## Mandatory fail-safe behavior

Adapter firmware must enter `SAFE` on:

- USB reset, suspend, disconnect, or host process loss;
- adapter reboot or watchdog expiry (500 ms maximum without valid traffic);
- serial framing, buffer, parsing, or protocol error;
- unknown command or invalid line/configuration;
- failure to apply a requested direction or value atomically.

The host daemon also sends `SAFE` during orderly shutdown, QEMU reset,
protocol failure, denied guest output, and socket loss. A missing
acknowledgement is treated as link failure; safety must therefore not depend
on the host command reaching the adapter. It sends `PING` at least every
200 ms while otherwise idle so a healthy session satisfies the watchdog.

Automatic output reclamation after reconnect is forbidden. Restart the daemon
to perform a new `HELLO`, input-first configuration, and QEMU state
synchronization.

## Evidence boundary

Pseudo-terminal tests cover framing, handshake, input/output transitions,
interleaved edge delivery, direction changes, `SAFE`, and disconnect cleanup.
The compiled portable firmware-core self-test covers parsing, polarity,
debounce, atomic output, watchdog, disconnect, and recovery. These tests do
do not prove the flashed MCU, USB electrical behavior, voltage protection,
contention detection, or physical timing. Those remain HIL release gates. The
RP2040 frontend itself is cross-built reproducibly from pinned SDK/toolchain
inputs and checked against `firmware-manifest.json`.
