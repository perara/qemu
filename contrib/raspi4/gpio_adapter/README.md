<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# QGPIO USB adapter firmware

This directory contains the reference firmware for the version-1 USB GPIO
adapter used by `gpio_proxy.py`.

- `qgpio_core.c` is a portable, host-tested protocol and safety core.
- `rp2040/` connects that core to all 30 RP2040 bank-0 GPIOs over USB CDC ACM.

The firmware starts with all pins as inputs with pulls disabled. It requires a
fresh `HELLO 1` after every USB connect, reset, resume, watchdog event, or
protocol error. Outputs are released after 500 ms without a valid command.
The host sends `PING` every 200 ms. A separate 250 ms hardware watchdog resets
the MCU if its main loop stalls.

## Host self-test

The normal QEMU Meson build compiles and runs `qgpio_core_test.c`. It can also
be run directly:

```sh
cc -std=c11 -Wall -Wextra -Werror -pedantic -O2 \
  -Icontrib/raspi4/gpio_adapter \
  contrib/raspi4/gpio_adapter/qgpio_core.c \
  contrib/raspi4/gpio_adapter/qgpio_core_test.c \
  -o qgpio-core-test
./qgpio-core-test
```

## RP2040 build

Install the pinned Raspberry Pi Pico SDK 2.3.0 commit
`98a542c1a62fb549ffb5d66a3e5892b06276b670` and set `PICO_SDK_PATH`, then:

```sh
cmake -S contrib/raspi4/gpio_adapter/rp2040 -B build-qgpio-rp2040 \
  -DPICO_BOARD=pico
cmake --build build-qgpio-rp2040
python3 contrib/raspi4/verify_qgpio_firmware.py \
  --build-dir build-qgpio-rp2040
```

Copy `qgpio_adapter.uf2` to the RP2040 boot volume. Use the resulting
`/dev/ttyACM*` device with `gpio_proxy.py --backend usb-serial`.

The RP2040 pin number is the protocol `line` number. The default Pico build
allows only header GPIOs GP0..22 and GP26..28; internal GP23..25 and GP29
fail closed. A custom carrier can override `QGPIO_RP2040_ALLOWED_MASK` at
CMake configuration time after review. Board-internal or unexposed pins must
not appear in a fixture map. Every real map requires
reviewed level shifting, current limiting, isolation as appropriate, and a
verified common-ground policy. The firmware and host tests do not establish
electrical or timing conformance; those remain HIL gates.

`firmware-manifest.json` pins the SDK commit, official toolchain archive hash,
source hashes, output sizes, and byte-identical `.bin`/`.uf2` hashes. Two
independent clean builds were compared when the manifest was generated.
