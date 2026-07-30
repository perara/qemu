Raspberry Pi boards (``raspi0``, ``raspi1ap``, ``raspi2b``, ``raspi3ap``, ``raspi3b``, ``raspi4b``, ``raspi-cm4``)
====================================================================================================================


QEMU provides models of the following Raspberry Pi boards:

``raspi0`` and ``raspi1ap``
  ARM1176JZF-S core, 512 MiB of RAM
``raspi2b``
  Cortex-A7 (4 cores), 1 GiB of RAM
``raspi3ap``
  Cortex-A53 (4 cores), 512 MiB of RAM
``raspi3b``
  Cortex-A53 (4 cores), 1 GiB of RAM
``raspi4b``
  Cortex-A72 (4 cores), 1/2/4/8 GiB of RAM (2 GiB default)
``raspi-cm4``
  Cortex-A72 (4 cores), 1/2/4/8 GiB of RAM (2 GiB default), persistent eMMC
  backing, and dedicated nRPIBOOT selection.  The full-boot path is
  experimental; see
  :doc:`../../devel/raspi4-platform`.

For both Pi 4 machines, select a production RAM SKU with ``-m 1G``, ``-m 2G``,
``-m 4G``, or ``-m 8G``.  The chosen capacity also selects the matching board
revision/OTP identity and firmware-visible DT memory map.  Other capacities
are rejected.

Implemented devices
-------------------

 * ARM1176JZF-S, Cortex-A7, Cortex-A53 or Cortex-A72 CPU
 * Interrupt controller
 * DMA controller
 * Clock and reset controller (CPRMAN)
 * System Timer
 * GPIO controller
 * Serial ports (BCM2835 AUX - 16550 based - and PL011)
 * Random Number Generator (RNG)
 * Frame Buffer
 * USB host (USBH)
 * GPIO controller
 * SD/MMC host controller
 * SoC thermal sensor
 * USB2 DWC2 host controller plus a partial device-mode register boundary
 * MailBox controller (MBOX)
 * VideoCore firmware (property)
 * Peripheral SPI controller (SPI)
 * Broadcom Serial Controller (I2C)
 * Two-channel Pulse Width Modulation controller (PWM0; logical/register model)
 * BCM2711 GENET v5 NIC with descriptor DMA and MDIO/PHY

The three standard BSC/I2C controllers require both ``I2CEN`` and the
one-shot ``ST`` command before beginning a transfer.  ``ST`` and FIFO-clear
commands read back as zero, architectural address/length/divider/timeout
field masks are applied, and an address NACK completes with ``ERR|DONE``
without leaving ``TA`` asserted.  Clearing status immediately updates the
IRQ.  Reset terminates a live bus transfer, restores register defaults, and
lowers the IRQ; migration restores its level and active bus ownership from
the migrated state.  The directional FIFO has the documented 16-byte depth:
RX stalls when full, RXR asserts at 12 bytes, TXW below four bytes, writes to
a full TX FIFO are ignored, and FIFO clear can abort an active transfer.  A
functional repeated start can change direction while the transfer remains
active.  FIFO contents and a stalled active transfer survive migration.
The opt-in ``clock-stretch-after`` and ``clock-stretch-cycles`` controller
properties inject a deterministic held-SCL interval at a byte boundary.
An I2C slave may request the same controller-visible interval through the
generic ``I2CSlaveClass::stretch`` callback; the request is sampled once for
each byte.  The TMP105 test device exposes test-only properties that exercise
this device-originated path.
Intervals below ``CLKT`` resume the transfer; longer intervals finish with
``CLKT|DONE``.  The deadline derives from the live VPU/core clock and BSC
divider, pauses while that clock is stopped, survives migration, and is
cancelled by reset.  The BCM2711 BSC is a single-master controller, so
multi-master arbitration is deliberately not modeled.  Ten-bit targets use
the documented ``11110xx`` address-register prefix plus the low address byte
from the FIFO; write/read phases, address and data NACK, reset, and active
migration retain the exact target.  DIV, FEDL/REDL, and CLKT values are
stored, reset, and migrated.  Electrical edge timing remains a Pass 2 HIL
conformance boundary.

GENET is mapped at ``0xfd580000`` with its two native GIC interrupt lines.
The production DT node is retained, and an unchanged Pi kernel discovers the
v5 controller, UniMAC MDIO bus, external PHY, and ``eth0``.  Its 40-bit
descriptor rings transfer packets through standard QEMU network backends,
including legacy ring-16 and current ring-0 receive layouts.  Backend link
state drives the PHY, and the production functional gate obtains DHCP through
the user network.  The 17-slot MDF accepts programmed unicast, multicast, and
broadcast addresses or bypasses filtering in promiscuous mode.  Basic RX/TX
MIB packet, byte, size, class, filter-miss, and reset behavior is implemented.
The device properties can be selected with, for example,
``-global bcm2711-genet.dma-error=1`` (TX) or ``dma-error=2`` (RX), together
with ``dma-error-after`` and ``dma-error-count``, to provide bounded
transaction-level failure and recovery; read-only byte/error progress survives
reset and migration.  ``packet-drop-direction=1`` (TX), ``2`` (RX), or ``3``
(both), together with ``packet-drop-after`` and ``packet-drop-count``, drops a
bounded occurrence range at the NIC boundary before backend TX or receive
delivery.  Seen/drop counters are read-only, survive reset, and migrate, so
the same control covers EEPROM DHCP/DNS/ARP/TFTP and Linux driver traffic.  HFB
classification/steering, WOL/EEE, remaining statistics, in-flight packet
migration, and link/error scripting remain incomplete.

The BCM2711 AVS monitor is mapped at ``0xfd5d2000`` and retains the production
``brcm,bcm2711-thermal`` DT node.  It exposes the 10-bit temperature code and
both status-valid bits consumed by Linux, with configurable
``temperature-millicelsius`` and ``sensor-valid`` device properties.  The
production DT calibration converts the default code to 24,823 m°C.  This is a
logical sensor value; physical thermal dynamics and calibration remain HIL.
Firmware ``GET_TEMPERATURE`` reads that same live numeric sample instead of a
separate constant, and ``GET_MAX_TEMPERATURE`` reports the Pi 4/CM4 85°C
safety limit.  The mailbox response has no validity field: ``sensor-valid``
continues to control the native AVS status bits while the numeric mailbox
sample remains observable for deterministic fault tests.

The firmware ``GET_THROTTLED`` property has a separate logical fault input.
``firmware-throttled-current`` accepts the Pi 4/CM4 current under-voltage,
Arm-frequency-cap, and active-throttling bits.  Each asserted bit sets its
matching sticky history bit 16--18.  ``firmware-throttled-status`` exposes the
combined value returned to the guest, and both portions survive warm reset and
live migration.  This models software-visible reporting, not voltage
thresholds, PMIC rail dynamics, thermal coupling, or physical brownouts.

The BCM2711 RNG200 at ``0xfe104000`` has a stateful 16-word FIFO, warm-up and
total-bit counters, programmable total-bit/FIFO thresholds, W1C interrupt
status, and a level output wired to GIC SPI 125.  The
``rng200-refill``, ``rng200-nist-fail``, ``rng200-master-fail``, and
``rng200-deterministic-seed`` properties on ``bcm2835-rng`` support bounded
depletion, failure recovery, and repeatable tests, for example
``-global bcm2835-rng.rng200-refill=off``.  FIFO contents, counters, status,
enable state, and deterministic-generator progress migrate.  A nonzero
deterministic seed is test-only; the normal path uses QEMU guest randomness.
Entropy quality and silicon timing still require a physical Pi/CM4 oracle.

The ``cyw43455-sdio`` device provides the first deterministic radio-transport
layer: SDIO CMD5/3/7/52/53, CCCR/FBR/CIS identity for Broadcom 02d0:a9bf,
function enable/ready, block sizing, the exact ``0x15294345``
BCM4345/revision-9/AXI signature, the real 800 KiB TCM aperture at
``0x198000``, a DMP EROM with ChipCommon/SDIO/D11/ARMCR4 cores, live
migratable AI wrapper registers, ARMCR4 TCM bank sizing, function-1
backplane/window access, function-2 BCDC control and Ethernet transport,
command-timeout injection, reset, telemetry, and active migration.  Unread
BCDC Ethernet frames migrate byte-exactly and resume on the destination
network backend.  ``packet-drop-direction=1`` (TX), ``2`` (RX), or ``3``
(both), together with ``packet-drop-after`` and ``packet-drop-count``,
injects a bounded deterministic loss window; progress counters migrate and
traffic resumes automatically after the window.  It may be
instantiated explicitly for tests or enabled onboard with
``-M raspi4b,wireless-model=on`` (and equivalently for ``raspi-cm4``).
``wireless-netdev=ID`` attaches that onboard data path to a named QEMU
``-netdev`` backend without introducing a second guest NIC.
An opt-in hash-pinned qtest sends an unchanged production firmware ``.bin``
through CMD53 and verifies its complete TCM readback without conversion.
The unchanged production kernel, modules, NVRAM, and firmware bind both SDIO
functions and register ``wlan0``.  The same opt-in enables the production
``brcm,bcm43438-bt`` DT child and an H4 controller on the onboard PL011 path.
Standard controller-information and Broadcom-vendor commands complete
deterministically; HCI events and bidirectional ACL traffic use the ordinary
UART/chardev byte stream.  Parser, pending-event, counter, and FIFO state
migrate, including commands split across migration.  When the model is
disabled, final DT
generation forces the ``brcm,bcm2835-mmc`` radio endpoint to
``status = "disabled"`` and ``wireless-status=excluded-unmodeled`` remains
fail-closed; the parent serial controller remains enabled for ``serial0``.
The fail-closed wireless release gate hashes every production input and
requires one console trace to prove unchanged brcmfmac WLAN DHCP/ping and
unchanged btbcm/hci_uart HCI0 enumeration together.
Association, coexistence, antenna, regulatory, RF, and radio-power validation
remain hardware-in-the-loop requirements.

Other unmodeled BCM2711 endpoints are retained in the final device tree with
``status = "disabled"`` instead of being deleted.  The machine's
``peripheral-model-policy=preserve-disabled-unmodeled-v1`` and
``peripheral-exclusions`` QOM properties make the exact exclusion contract
observable.  The final pass runs after overlays, so unsupported hardware
cannot be silently re-enabled.

The AUX mini-UART implements the 8250-visible divisor-latch access, control,
status, scratch, enable, interrupt, baud, reset, and migration state needed by
the production Linux driver.  Behavioral firmware preserves the DT's
firmware-owned boot arguments, so ``8250.nr_uarts=1`` and ``serial0`` produce
the expected ``ttyS0`` registration.  Its eight-byte TX FIFO drains at the
divider-derived baud from CPRMAN's VPU/core clock.  Runtime core-clock
divider, stop, resume, reset, and migration changes preserve the active
frame's remaining clock cycles.  RX error injection, detailed flow-control
behavior, and physical serial timing remain incomplete.

Firmware mailbox clock state, configured rate, and measured-rate operations
use the live CPRMAN muxes for EMMC, UART, ARM, CORE/VPU, V3D, H264, ISP, PWM,
EMMC2, and VEC.  Rate changes and gating propagate to connected peripherals;
a stopped clock retains its next-enable rate but measures zero.  Unsupported
IDs report nonexistent or zero.  Reset restores both mux registers and the
exported QEMU clock levels, and live migration retains the configured state.
Firmware min/max and DVFS/voltage policy plus physical transition timing
remain incomplete.

Firmware property buffers are structurally validated before any tag runs.
Their declared header, padded tag extents, and end marker must remain inside
the 32-bit VideoCore address space; malformed requests receive the partial
error response without tag side effects.  Tag traversal honors 32-bit padding,
including the six-byte board-MAC response.  Each response write is clipped to
the declared value size while its header reports the full desired length, so
short responses cannot overwrite padding or following tags.  The same
preflight requires the zero request code and validates every value buffer
before reads or side effects, including dynamically sized palette and OTP/key
writes; malformed later tags therefore cannot partially execute valid earlier
tags.
Unsupported tags retain a clear response bit and untouched value buffer while
later aligned tags continue to execute and the complete buffer can still
succeed.  This permits firmware feature detection without false success.
Within a framebuffer transaction, all supported Set tags take effect before
any Get response even when a Get appears first.  Mixing framebuffer Test tags
with Get or Set tags, or repeating any framebuffer tag, rejects the complete
request before mutation.  Test-only transactions normalize geometry, virtual
size, offsets, supported depth, pixel order, and alpha mode through one
temporary configuration.  Each result feeds the next Test, but the transaction
does not change live scanout; Set tags share the same scalar validation.

The power-property interface retains logical state for documented device IDs
0--10.  Get/set operations report on/off or nonexistent-device status, while
the timing query returns zero because no rail-stabilization delay is modeled.
Logical state migrates and returns to the available-device mask on reset; this
does not emulate electrical rail behavior.

Framebuffer palette get/test/set operations use the same 256-entry VideoCore
RAM table as 8-bit scanout.  Get supports bounded short responses while
reporting the full 1,024-byte desired length; test validates ranges and
declared colors without modifying the table.

Framebuffer overscan get/set operations retain top, bottom, left, and right
values, while test evaluates candidates without changing configuration.
Valid margins render black borders and scale the transformed framebuffer into
the remaining display rectangle; margins that consume an entire axis retain
the previous values.  This state migrates and resets to board defaults.
QEMU uses Pixman bilinear filtering rather than claiming cycle- or
coefficient-exact VideoCore HVS scaling.

Framebuffer release disables scanout and clears the rendered surface without
discarding retained geometry or VRAM.  A later allocation re-enables it.
Framebuffer layer and transform tags retain validated firmware-visible state.
Transform accepts all eight VideoCore rotation/mirror encodings; Test does not
change live configuration.  Framebuffer VMState v6 migrates this state and
restores the destination surface geometry; reset returns to the configured
enabled display.
All eight transforms affect rendered scanout, including width/height exchange
for transposed modes.  ``SET_VSYNC`` is a synchronous wait command: QEMU holds
the property-mailbox response until the next virtual vblank at the selected
display's active FKMS refresh rate, or 60 Hz when no timing exists.  An
in-flight wait migrates in property VMState v10 and reset cancels it.
Undocumented Get/Test variants return zero.

Firmware cursor info/state tags accept a 16--64-pixel ARGB surface in guest
DMA memory, a hotspot, signed display position, visibility, and display- or
framebuffer-coordinate selection.  Valid custom cursors are composed over the
rendered scanout and clipped at its boundary; malformed requests retain the
previous state.  Framebuffer VMState v6 migrates the cursor and its guest RAM,
and reset clears it.  The proprietary firmware's default cursor artwork used
before a cursor-info request is not available to QEMU.

The firmware interface reports two displays and maps framebuffer indices zero
and one to HDMI0 ID 2 and HDMI1 ID 7.  Display selection and independent
boolean power state migrate in property VMState v10 and reset to display zero
with both outputs powered.  QEMU currently renders one selected framebuffer
console rather than two independent HDMI heads.

Firmware ``GET_DISPLAY_TIMING`` and ``SET_TIMING`` use the FKMS 36-byte
payload for HDMI0 ID 2 and HDMI1 ID 7.  Each port's reset mode is derived from
the preferred detailed timing in its validated EDID, including clock, sync
intervals, totals, refresh, polarity, interlace, aspect, and HDMI/DVI state.
Valid Set payloads are retained independently and migrate in property VMState
v10; malformed timings do not change state, and reset re-derives modes from
newly sampled connector files.  Pixel-clock execution, HVS/PV behavior,
physical HPD timing, and electrical display timing are not modeled.

The BCM2711 HDMI0 and HDMI1 DDC BSC controllers are guest-visible at their
production addresses.  Linux's ``i2c-brcmstb`` register contract is modeled:
auto-I2C ownership release, packed 32-byte transfers, completion/NAK status,
the DDC segment pointer, and EDID reads at address 0x50.  Both ports expose the
same reset-sampled, checksum-validated bytes used by firmware filters and
mailbox EDID tags.  Sinks whose CTA HDMI Forum VSDB advertises SCDC expose
address 0x54 with standard version, TMDS ratio, scrambling, read-request, and
channel-status behavior; other sinks NAK it.  Controller position, registers,
and negotiated SCDC state migrate in VMState v2.  The production HDMI core
``HOTPLUG`` register reports live connector presence.  Host-driven transitions
gate DDC and pulse connected/removed inputs 4/5 or 10/11 into the modeled
edge-latched BCM2711 AON L2 controller at 0xfef00100, which implements the
Linux status/clear/mask contract and drives GIC SPI 96.  HPD and interrupt
state migrate; reset resamples validated EDID presence.  The production CEC
windows implement VC4 control/timing/address and 16-byte TX/RX registers,
nominally timed completion against a configurable peer logical-address mask,
ACK/NACK fault selection, host receive injection, and the HDMI0 lines 0/1
plus HDMI1 lines 8/7 through AON.  CEC transaction state and active deadlines
migrate.  Physical HPD voltage, debounce, CEC open-drain voltage and
arbitration, I2C timing, clock stretching, and electrical conformance remain
outside this model.

SPI0 implements polled and interrupt FIFO transfers plus BCM DMA maps 6/7.
The first TX DMA word frames ``DLEN`` and the low CS controls; later words
carry four little-endian bytes.  Programmable DREQ/panic thresholds pace the
shared DMA engine, while TA, CE selection, polarity, and ADCS drive three
chip-select outputs.  Each byte consumes eight serial-clock cycles derived
from CPRMAN VPU clock / ``CLK``.  Clock stop/resume and rate/divider changes
retain the remaining cycles, and the active deadline, FIFO/DMA progress,
interrupt, and derived outputs migrate.  LoSSI, bit-edge CPOL/CPHA waveform
conformance, and electrical behavior are not modeled for SPI0.

The shared AUX block also implements SPI1 and SPI2 at their native register
windows.  AUX enable bits gate access to independent four-entry TX/RX FIFOs
and SSI buses.  Fixed/variable widths, IO versus TXHOLD frame termination,
native CS patterns, FIFO status/peek/pop, and shared TX-empty/idle interrupts
are modeled.  Entries are paced at VPU core clock divided by
``2 * (speed + 1)``.  Clock stop/resume, active and held-CS state, FIFO/IRQ
progress, and remaining deadlines migrate.  The SSI exchange boundary is
byte-granular.  A production functional gate uses the pinned unchanged
Raspberry Pi 5.15 kernel, official SPI1/SPI2 overlays, and byte-unchanged
packaged AUX-SPI/spidev modules to bind both stock drivers, create both
``/dev/spidev`` nodes, and complete writes to attached flash devices.
Arbitrary-bit edge behavior, post-input/DOUT-hold electrical timing, and
native-CS silicon quirks are not modeled.

PWM0 and PWM1 are mapped at ``0xfe20c000`` and ``0xfe20c800`` on BCM2711.
Each implements the two-channel control/status/range/data register contract
plus an independent shared 16-word FIFO.
Each FIFO-enabled channel consumes one word after its programmed range of
CPRMAN PWM source-clock cycles.  Clock-rate changes, stop/resume, reset, and
migration preserve the remaining source cycles.  Empty consumption sets the
channel gap flag, and named normal/panic DMA-threshold outputs track the DMAC
configuration.  PWM0's normal request is connected to BCM DMA peripheral map
5 and PWM1's to map 1; their panic outputs select the corresponding channel's
four-bit DMA panic priority while the normal request continues to gate
transfers.  Simultaneously ready channels are serviced by effective priority,
with stable channel-number ordering for ties.  The DMA CS DREQ bit reports the
selected peripheral request level;
destination-DREQ transfers fill only through the configured threshold, retain
ACTIVE/HELD and exact control-block progress while waiting, and migrate live.
Named ``channel-enabled`` outputs expose logical enable state.  Separate
``waveform`` outputs implement the distributed PWM algorithm, mark-space mode,
MSB-first serializer, FIFO repeat, idle bit, and polarity at CPRMAN source
clock boundaries.  When both channels use one controller's FIFO, A/C/E words
remain assigned to channel 0 and B/D/F to channel 1.  The channels request the
next words in lock-step: a shorter range idles until the longer channel reaches
the same boundary, and the next-owner state survives FIFO starvation and live
migration.  BCM2711 routes PWM0 channel 0 to GPIO12 ALT0 and GPIO18
ALT5, PWM0 channel 1 to GPIO13 ALT0, GPIO19 ALT5, and GPIO45 ALT0, and PWM1
channels 0/1 to GPIO40/41 ALT0; ``GPLEV``, edge detection, logical output
wires, the host GPIO bridge, and migration see those transitions.
Electrical edge shape and physical timing behavior are not modeled.

On ``raspi4b`` and ``raspi-cm4``, the BCM2711 GPIO model exposes 58 named
``pin-input`` lines for externally driven or disconnected pin state, 58 value
outputs, and 58 named ``pin-output-enable`` lines.  ``GPLEV`` resolves the
external state and configured pull while a pin is an input, and the retained
output latch while it is an output.  Edge and level detection latch ``GPEDS``
and route the three pin-group interrupts to GIC SPI 113--115.  The fourth
wake-only interrupt is exposed but wake policy is not modeled.  This is a
logical QEMU wiring interface.  An optional versioned host socket is selected
with ``-M gpio-chardev=ID``; ``contrib/raspi4/gpio_proxy.py`` can use either
libgpiod v2 or the versioned USB-serial MCU protocol documented beside it.
The portable reference core and RP2040 frontend are under
``contrib/raspi4/gpio_adapter``.  Electrical behavior and contention
protection remain hardware responsibilities.

For example, create a local Unix socket without delaying machine startup::

  qemu-system-aarch64 \
    -chardev socket,id=gpio,path=/run/user/1000/rpi-gpio.sock,server=on,wait=off \
    -M raspi4b,gpio-chardev=gpio

For ``raspi-cm4``, an explicitly driven GPIO40 is sampled as the active-low
``EMMC_DISABLE``/``nRPIBOOT`` input at behavioral reset.  It overrides the
``nrpiboot`` machine property while driven and remains connected across reset;
high impedance restores property control.  Machine properties
``nrpiboot-sampled`` and ``nrpiboot-source`` expose the latched decision.
The socket also accepts dedicated input-only ``EEPROM_NWP`` and
``SD_OVERCURRENT`` board signals through ``SET SIGNAL <name> 0|1|Z``. They
remain separate from the 58 guest-visible GPIOs. Driven values override the
matching ``eeprom-nwp`` or ``sd-overcurrent`` fallback property; the
corresponding ``*-source`` observation reports which boundary is live.

On Pi 4B, EEPROM ``SD_QUIRKS`` accepts the documented BCM2711 bit-field
values zero and one.  Bit zero disables SD high-speed operation and requests
the documented 12.5 MHz boot-time clock ceiling.  Reserved bits invalidate
the EEPROM configuration instead of silently enabling unknown behavior.
``boot-sd-quirks``, ``boot-sd-high-speed-enabled``, and
``boot-sd-clock-limit-hz`` expose the selected policy.  At firmware handoff
the guest-visible SDHCI Host Control register has high speed disabled and its
Clock Control divisor selects the fastest modeled clock not exceeding that
ceiling.  With this controller's advertised 52 MHz base clock, the result is
8,666,666 Hz; query ``boot-sd-controller-clock-hz`` and
``boot-sd-controller-high-speed`` for the applied state.  CM4 eMMC boot parses
the common setting but does not apply this Pi 4B SD-card quirk.  The policy
and controller state reset from EEPROM and survive migration in
behavioral-boot VMState version 64.  File-transfer execution is not paced by
that clock, and electrical clock shape, calibrated cadence, and marginal-card
interoperability remain HIL/oracle work.

The same machine option works on ``raspi-cm4``.  The protocol has no network
authentication; use a permission-restricted Unix socket unless a trusted
transport supplies access control.

Behavioral boot media may be a FAT12/16/32 superfloppy or a bounded FAT
partition in MBR or GPT media.  GPT selection fails closed unless its primary
and backup headers and entry arrays agree and pass their CRCs.

Firmware ``config.txt`` includes retain the current conditional-filter state,
and filters changed by included bytes remain active when parsing resumes in
the caller.  Model and board-type, EDID, raw OTP serial, externally driven
GPIO, and boot-variable expression categories combine; a later filter replaces
only its own category.  ``[all]`` resets every category and ``[none]`` remains
inactive until that reset.  On BCM2711, expressions support ``bootvar0``, requested
``partition``, selected ``boot_partition``, and ``cust_otp0`` through
``cust_otp7``.  Unknown, malformed, unavailable, or high-impedance conditions
remain inactive; ``[tryboot]`` reads the one-shot reboot state.  This behavior
is shared by Pi 4B SD, CM4 eMMC, and
network-provided configuration.

If EEPROM ``bootconf.txt`` contains a ``[config.txt]`` section, every byte
after that section header is appended to the selected boot-media
``config.txt``.  The existing filter state continues across the boundary,
and an appended ``include`` reads from the selected SD, eMMC, USB, NVMe, or
network source.  The EEPROM appendix can also supply the complete
configuration when the media has no ``config.txt``.  Read-only
``boot-eeprom-config-append-size`` and
``boot-eeprom-config-append-sha256`` expose the exact consumed bytes; VMState
retains them across a pending boot.

EEPROM ``BOOT_UART=1`` enables a deterministic clean-room bootloader trace on
the primary UART.  Pi 4B and CM4 select PL011 UART0 on GPIO14/GPIO15 ALT0,
program 115200 baud, eight data bits, no parity, and one stop bit, and report
boot-order entry, source attempts, and second-stage handoff through serial0.
The option defaults to zero and values other than zero or one invalidate the
EEPROM configuration.  Read-only ``boot-uart-enabled``,
``boot-uart-active``, ``boot-uart-bytes``, ``boot-uart-lines``, and
``boot-uart-format`` expose the modeled contract.  VMState retains active
ownership and counters without replaying already delivered bytes.  A
subsequent ``uart_2ndstage=1`` trace takes ownership of the same UART after
bootloader handoff.  The emitted text is a stable QEMU diagnostic contract,
not a claim of byte-for-byte bootloader output or physical UART cadence
equivalence.

The bootloader-owned ``start_x``, ``start_debug``, ``gpu_mem``,
``gpu_mem_256``, ``gpu_mem_512``, and ``gpu_mem_1024`` settings are accepted
only from the selected top-level configuration file, not from an included
file.  On Pi 4/CM4, ``start_x=1``
selects ``start4x.elf``/``fixup4x.dat`` with fallback to
``start_x.elf``/``fixup_x.dat``; ``start_debug=1`` selects
``start_db.elf``/``fixup_db.dat``.  An effective ``gpu_mem=16`` selects
``start4cd.elf``/``fixup4cd.dat``.  Since every supported Pi 4/CM4 memory
model has at least 1 GiB, ``gpu_mem_1024`` overrides ``gpu_mem`` when present;
the 256 and 512 MiB selectors are parsed but do not match these machines.
The effective value defaults to 76 MiB and has a 16 MiB minimum.  It drives
the low-memory VideoCore reservation consistently in the final DT, firmware
ARM/VC memory mailbox tags, and framebuffer model.  Read-only
``firmware-gpu-mem-mb`` and ``firmware-gpu-mem-source`` expose the result.
An explicit paired ``start_file``/``fixup_file`` takes precedence.  Cut-down
firmware must be selected through ``gpu_mem=16`` rather than by naming the
``*cd`` files directly.  The same rules apply to block and network boot media.

The firmware property mailbox reports one coherent board identity.  The legacy
board-model response is zero; board revision and the zero-extended 64-bit board
serial come from persistent OTP, while board MAC comes from the configured
GENET address.  These values remain coherent across reset and live migration
for both Pi 4B and CM4.  A short model response is clipped to the caller buffer
while retaining the complete four-byte desired length.

Top-level ``total_mem`` limits the firmware-visible capacity in MiB and is
clamped to the documented 128 MiB minimum and the installed 1, 2, 4, or 8 GiB
model maximum.  The final DT is regenerated from that effective capacity:
the existing VideoCore reservation is removed from the lower bank and only
capacity above 1 GiB is published as ``memory@40000000``.  The installed RAM
backend and board revision remain unchanged, allowing one machine model to
exercise every supported limited-memory configuration.  Read-only
``firmware-total-mem-mb`` exposes the effective capacity.  Included files
cannot change it, and block and network configuration use the same path.

Top-level ``bootcode_delay=N`` defers firmware artifact resolution and ARM
handoff by exactly N seconds of QEMU virtual time.  The wait applies equally
to SD/eMMC, USB, NVMe, TFTP, and HTTP-derived configuration; includes cannot
set it.  HDMI EDID inputs are sampled again when the delay expires before
``config.txt`` is re-evaluated.  Reset starts a fresh delay, while live
migration preserves its consumed state and exact remaining virtual time.
Read-only ``firmware-bootcode-delay-seconds`` exposes the selected value.

Top-level ``sdram_freq=N`` is parsed and retained as the requested MHz value,
but Pi 4B and CM4 LPDDR4 remains at the documented, non-configurable
3200 MHz rate.  ``firmware-sdram-frequency-requested-mhz`` exposes the
requested value, ``firmware-sdram-frequency-requested`` reports whether it
was present, and ``firmware-sdram-frequency-mhz`` reports the effective rate.
This invariant is identical across the 1, 2, 4, and 8 GiB models and network
boot.  Included settings cannot change it.

Top-level ``uart_2ndstage=1`` enables a deterministic clean-room firmware
trace on PL011 UART0/serial0.  Behavioral firmware selects GPIO14/GPIO15 ALT0,
programs PL011 for 115200 8N1, and emits stage enablement, selected source and
artifact names, and the final ARM handoff address through the modeled UART
chardev.  ``firmware-uart-2ndstage``,
``firmware-uart-2ndstage-bytes``, and
``firmware-uart-2ndstage-lines`` expose the active state and delivered output.
Reset emits a fresh trace; migration retains counters without replaying old
bytes.  Included settings are ignored and values other than zero or one are
rejected.  The text is a stable QEMU diagnostic contract, not a claim of
byte-for-byte VideoCore log or physical UART cadence equivalence.

Raw monitor data can be supplied with
``-M hdmi0-edid-file=PATH,hdmi1-edid-file=PATH``.  Each reset
validates and samples complete EDID blocks, derives the Raspberry Pi
``MANUFACTURER-Display_Product_Name`` string, and applies ``[EDID=name]`` if
either Pi 4 HDMI port matches.  Empty data is disconnected and invalid data
fails closed.  ``hdmiN-edid-name`` and ``hdmiN-edid-status`` expose the
sample, which is retained across migration and refreshed on the next reset.
The same bytes are returned by the firmware ``GET_EDID_BLOCK`` tag for HDMI0
and ``GET_EDID_BLOCK_DISPLAY`` for either port, including extension blocks.
Missing blocks return zero data, and short buffers are bounded while reporting
the complete 136-byte desired response.  The guest HDMI DDC controllers expose
the same bytes; physical bus behavior remains a hardware-conformance boundary.

An unchanged HAT ID EEPROM image can be attached as a read-only block backend
with ``-M hat-eeprom-drive=ID``.  Behavioral firmware validates the ``R-Pi``
header, ordered atoms, lengths, and per-atom CRC-16, publishes vendor, product,
UUID, product identifiers, and custom atoms under ``/hat``, and automatically
loads either the embedded DTBO or its named filesystem overlay.  The HAT
overlay owns the initial parameter scope, exactly as a physical Pi: a leading
``dtoverlay=`` suppresses it, and ``force_eeprom_read=0`` suppresses the
EEPROM read and removes ``/hat``.  Used GPIO entries program the BCM2711
function selector and pull registers before ARM handoff and are reapplied on
reset.  ``hat-gpio-used-mask`` plus the ``hat-gpio-drive``,
``hat-gpio-slew``, ``hat-gpio-hysteresis``, and ``hat-gpio-back-power``
properties preserve the validated EEPROM policy across migration.
``hat-gpio-map-status`` deliberately reports that digital state is applied
while electrical policy is only recorded: pad current, edge rate, hysteresis
voltage, back-power current, and rail sequencing remain HIL requirements.
The machine also exposes ``hat-eeprom-status``, the observed padded backend
size and SHA-256 through ``hat-eeprom-size`` and ``hat-eeprom-sha256``, and
the EEPROM-declared logical size and SHA-256 through
``hat-eeprom-declared-size`` and ``hat-content-sha256``.  The parsed identity
is available through ``hat-vendor``, ``hat-product``, ``hat-uuid``,
``hat-product-id``, ``hat-product-version``, ``hat-custom-count``, and
``hat-overlay``.  These read-only properties let a corpus gate distinguish
the exact input file, QEMU block padding, and the logical EEPROM content
declared by its header.
For example::

  qemu-system-aarch64 \
    -M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,hat-eeprom-drive=hat \
    -drive if=none,id=pieeprom,format=raw,file=pieeprom.bin \
    -drive if=none,id=hat,format=raw,readonly=on,file=myhat.eep \
    -drive if=sd,format=raw,file=sd.img

The functional production corpus pins nine unchanged overlays from one
Raspberry Pi firmware commit: ``gpio-led``, ``i2c-gpio``, ``i2c-rtc``,
``mcp2515-can0``, ``miniuart-bt``, ``pwm-2chan``, ``spi0-1cs``, ``uart2``,
and ``w1-gpio``.  It applies each overlay with real parameters and also
applies the complete ordered set, requiring exact final Device Tree hashes.
The gate pins QEMU's random seed so the firmware-owned ``kaslr-seed`` property
does not make those hashes nondeterministic.  Intra-overlay fragments are
stably topologically ordered across multi-level dependencies; a dependency
cycle is rejected before applying the overlay.
Empty compatibility overrides are accepted as intentional no-ops, and
multi-target descriptor streams remain stable while the overlay itself is
modified.  Physical startup timing and electrical state still require the
HIL differential gate.

The machine does not contain a VideoCore VI CPU.  Behavioral boot is an
explicit versioned clean-room replacement, reported as
``videocore-execution-mode=behavioral-replacement-v1`` and
``videocore-boundary-version=1``.  It consumes the unchanged production
firmware and fixup bytes and exposes their hashes, but reports
``videocore-artifact-policy=exact-input-bytes-not-instruction-executed`` so
tooling cannot mistake artifact discovery and output emulation for VideoCore
instruction execution.
This behavioral replacement is the completed Pass 1 contract: unchanged
firmware/fixup bytes are mandatory, hash-visible inputs, while all
software-visible outputs required for the production flash-and-boot workflow
are produced by the clean-room model.  It is deliberately not a claim of
VideoCore ISA equivalence.

The behavioral firmware honors ``arm_64bit`` without converting the selected
kernel.  The default 64-bit ``kernel8.img`` follows the ARM64 Image-header
handoff.  The complete 64-byte header is required; its little-endian
``text_offset`` and ``image_size`` fields determine placement and reservation.
Offsets below 4 KiB are relative to a 2 MiB-aligned base.  Header flag bit 0
selects matching little- or big-endian EL2 data access for all four cores.
Initramfs and DT placement rejects collisions with the low-memory entry stub,
spin table, or architecture-specific spin code before guest RAM is modified.
The production ``device_tree_address`` setting selects an exact final-DTB
address, while ``device_tree_end`` supplies its exclusive upper bound.  Both
are parsed as unsigned addresses and use the same kernel, initramfs,
firmware-state, and low-RAM collision checks as automatic top-down placement.
This applies equally to Pi 4B SD and CM4 eMMC ARM64/ARM32 boot media.
With ``arm_64bit=0``, unchanged ``kernel7l.img`` zImage bytes load at
``0x8000`` and receive the ARM boot-register ABI through an entry stub at
``0x0``; secondary Cortex-A72 cores wait on the BCM2711 local mailbox-3
protocol.  Read-only ``arm-handoff-architecture``,
``arm-handoff-endianness``, and
``arm-handoff-entry-address`` properties expose the selected ABI.  Standard
Pi 4B SD and CM4 eMMC gates boot this path on all four cores to ``armv7l``
userspace using unchanged pinned binary artifacts.  Raw-media qtests also
exercise this ARM32 handoff at every supported 1/2/4/8 GiB capacity on both
machines and validate the resulting lower and upper DT memory ranges.
For either architecture, only the first LF- or CRLF-terminated line of
``cmdline.txt`` is appended to firmware-generated ``bootargs``.  Exact
artifact size/hash reporting still covers every file byte; embedded NULs are
rejected instead of silently truncating the command line.

When BCM2711 secure boot is enabled in the OTP backing, the EEPROM must contain
the upstream ``pubkey.bin``, ``bootconf.txt``, and ``bootconf.sig`` files.  The
public-key SHA-256 must match OTP rows 47-54 and the RSA-2048 PKCS#1 v1.5
SHA-256 signature must verify before configuration is accepted.  A selected
block medium must then provide a matching signed ``boot.img``/``boot.sig``;
QEMU reads firmware and OS files only from the verified read-only FAT image.
Duplicate ``bootconf.txt``, ``bootconf.sig``, or ``pubkey.bin`` EEPROM
sections are rejected before any of those inputs is consumed.
The ``secure-boot-status`` machine property reports the verification boundary.
The signed image follows the same firmware/config/kernel/DT handoff path as an
unsigned block medium; QEMU does not translate or repack its inner files.
Behavioral recovery and CM4 RPIBOOT can provision this state from the same
``config.txt`` ``program_pubkey=1`` request and generated signed
``pieeprom.bin`` used by the secure recovery bundle.  This requires
``otp-drive`` persistence, verifies the customer-signed EEPROM before
mutation, rejects a different existing key, and stores the key hash plus the
current production secure/revocation flags.  ``program_jtag_lock=1`` is
rejected because its irreversible BCM2711 fuse encoding is not public.
``otp-provision-fail-after`` can cut the public eleven-row programming
sequence at any boundary; the persistent prefix and
``otp-provision-rows-programmed`` observation survive reset and migration.
This models behavioral irreversibility, not sub-row electrical fuse physics.
Secure network BOOT_ORDER requests ``boot.sig`` then ``boot.img`` through the
GENET TFTP client and applies that same verified-image handoff.  Mode 7 also
supports a custom ``HTTP_HOST`` over plain HTTP: it performs DHCP/DNS/ARP and
checksummed TCP, requires bounded HTTP 200 responses, and verifies the exact
response bodies before handoff.  Lost SYN and GET packets are retransmitted
with deterministic backoff, while partial responses trigger duplicate ACKs.
A bounded 64 KiB sparse receive window buffers multiple noncontiguous future
segments, preserves first-arrival overlap bytes, and drains them cumulatively
as gaps arrive.  Its data/validity map and FIN sequence, partial body, and
remaining deadline migrate mid-response.  FIN after the exact declared body is acknowledged before
handoff; premature FIN enters retry/fallback immediately as a truncated
response.  The client sends FIN after each complete response, and redirects
fail explicitly rather than changing the authenticated URL policy.  Other HTTP
protocol errors likewise do not wait for the file timeout.  Authenticated host names use the DHCP-provided DNS resolver before
the HTTP ARP/TCP sequence; the signed wire test covers that complete path.
For the standard host, QEMU uses an embedded Raspberry Pi intermediate CA,
sends TLS SNI, verifies the server certificate and hostname, and verifies the
downloaded pair with the official network-install RSA key.  An opt-in
functional gate boots an unchanged production EEPROM through user networking
and the live official HTTPS service to ARM handoff, checking the signed
container and every consumed inner artifact by SHA-256.  BootROM ``bootsys``
bytes in secure OTP mode must carry the BCM2711 signed-envelope structure and
match ``bootsys-trusted-sha256``; ``bootsys-sha256`` reports the observed
section.  QEMU also decompresses and hashes the EEPROM ``bootmain``, MCB,
memory-system, logo, and font sections and requires each digest in that signed
payload; the dependency count and ordered-set digest are observable QOM
properties.  Each bounded LZ4 frame must also carry its valid xxHash32-derived
descriptor checksum; a damaged header is rejected before dependency hashing.
``bootsys-key-index`` and ``otp-secure-boot-flags`` expose the
public development-key revocation boundary: row-55 revocation rejects old
key-index-zero second stages while independently trusted ROM key indices one
through four remain eligible; indices above four fail closed.  Provisioning
forces the current production ``0x81`` flags whenever
``program_pubkey=1``, including when stale input says ``revoke_devkey=0``.
This is not a general BCM2711 firmware-version anti-rollback counter.  The
pinned digest replaces no input bytes and is supplied by the release oracle.
Silicon execution of the non-public Raspberry Pi RSA/HMAC root, electrical
fuse-programming behavior, and TCP SACK are not yet implemented.

Pi 4 SD recovery likewise consumes the unchanged ``recovery.bin``.  QEMU
requires its public payload-length/key-index/RSA-2048/HMAC-SHA1 envelope and
an exact ``recovery-trusted-sha256`` oracle before the behavioral ROM may
update EEPROM.  ``recovery-sha256`` and ``recovery-key-index`` expose the
observed identity.  This is a fail-closed behavioral substitute for the
non-public silicon HMAC boundary, not a claim that QEMU reproduces silicon
cryptographic execution.

For Pi 4B behavioral boot, ``ENABLE_SELF_UPDATE`` defaults to enabled and
``FREEZE_VERSION=1`` overrides it.  Before firmware handoff, SD, USB-MSD, and
NVMe FAT boot filesystems and real GENET/TFTP boot are checked for the
unchanged ``pieeprom.upd`` and ``pieeprom.sig`` pair.  TFTP requests the
optional update before configuration or secure-boot artifacts and requests
the signature only when the update exists.  A valid SHA-256 signature line
and an exact 512 KiB image are required.  A byte-identical installed image
continues boot without writing; a different image is persistently erased,
programmed, verified, and rebooted before the same source is retried.  Invalid
signatures and write-protected EEPROMs fail without mutation.  CM4 reports
this path as unsupported because its EEPROM update workflow remains USB
RPIBOOT.  Packet tests cover the complete TFTP update, automatic reset,
identical-image no-op, and subsequent firmware handoff on the same backend.
``boot-enable-self-update``, ``boot-freeze-version``, and
``boot-self-update-status`` expose the decision, and VMState version 58
preserves an update-triggered reboot.

The persistent Pi 4 EEPROM update path models 4 KiB erase and 256-byte NOR
page programming.  Programming applies ``old & requested`` per byte, exposes
completed-page and illegal zero-to-one bit counts, and supports a deterministic
stuck-at-zero cell fault for durable failure-and-retry tests.
Optional ``eeprom-erase-sector-delay-us``,
``eeprom-program-page-delay-us``, and
``eeprom-verify-sector-delay-us`` values turn recovery into a timer-driven
transaction.  Each completed 4 KiB erase or 256-byte page program is retained
across reset; live migration preserves the exact stage, update bytes, progress,
configured latency, and remaining virtual time.  Zero keeps the compatibility
path synchronous.  These operator-supplied delays are deterministic test
inputs, not calibrated silicon timing.
Persistent status-register block protection rejects erase/program independent
of the physical pin.  The active-low ``EEPROM_nWP`` input does not protect the
array by itself; pulling it low prevents recovery from changing that status.
The unchanged recovery ``config.txt`` value
``eeprom_write_protect=-1|0|1`` leaves protection unchanged, clears it before
flash, or sets it after a verified flash.  The corresponding machine
properties are ``eeprom-status-write-protect``, ``eeprom-nwp``, and the
read-only transaction observation ``eeprom-nwp-sampled``.
An optional ``eeprom-status-drive`` names a raw 512-byte backend that persists
the status across independent QEMU processes.  Byte zero is the write-protect
value (``0`` or ``1``), byte one says whether update-timestamp metadata is
valid, bytes two through five contain that timestamp as a little-endian
32-bit value, and all remaining bytes are zero.  Legacy sectors containing
only byte zero and zeros remain valid.

EEPROM ``REBOOT_ON_FATAL_ERROR`` defaults to one.  An unsupported BOOT_ORDER
nibble or exhausted order without STOP waits for three behavioral error-pattern
intervals and then uses the BCM watchdog hard-reset path.  Zero leaves the
fatal observation stopped until an external reset.  The selected policy,
remaining deadline, and warm-reset-persistent reboot count are available
through machine QOM and migrate in behavioral-boot VMState version 60.
The fixed virtual pattern interval is deterministic test behavior, not
hardware-calibrated LED cadence.

EEPROM ``HDMI_DELAY`` defaults to five seconds and controls the independent
bootloader-diagnostics deadline.  A successful ARM handoff before that
deadline cancels the pending display, while a fatal boot error makes
diagnostics visible immediately.  ``HDMI_DELAY=0`` makes diagnostics visible
without waiting, and ``DISABLE_HDMI=1`` suppresses them in every case.
``boot-hdmi-delay``, ``boot-hdmi-diagnostics-pending``,
``boot-hdmi-diagnostics-visible``, and
``boot-hdmi-diagnostics-remaining-ns`` expose the effective policy and
deadline.  The deadline resets and migrates in behavioral-boot VMState
version 77.  This models display timing and visibility, not the private
bootloader diagnostic artwork or physical HDMI timing.

EEPROM ``WAKE_ON_GPIO`` and ``POWER_OFF_ON_HALT`` are strict booleans with
Pi 4 defaults of one and zero.  A Linux-style PM_RSTS partition-63 halt
suspends the VM in ``halted-gpio-wake`` when GPIO wake is enabled; a falling
GPIO3 edge delivered directly or through ``gpio-chardev`` wakes and resets the
board.  With GPIO wake disabled and PMIC power-off disabled, the state is
``halted-global-en`` and only a falling ``global-en`` machine input restarts
it.  ``WAKE_ON_GPIO=0`` plus ``POWER_OFF_ON_HALT=1`` requests a real QEMU
guest shutdown and reports ``powered-off``.  These states and policy migrate
in the BCM power-management VMState version 4.  Electrical PMIC outputs,
current draw, and physical GLOBAL_EN timing remain hardware-only.

EEPROM Network Install supports ``NET_INSTALL_KEYBOARD_WAIT`` with the Pi 4
default of 900 milliseconds.  Network Install itself defaults on for Pi 4B
and off for CM4; ``NET_INSTALL_AT_POWER_ON=1`` overrides a conflicting
disabled setting without selecting mode 7 by itself.  A host keyboard and Shift key can be attached
through the ``net-install-keyboard-present`` and ``net-install-shift-held``
machine inputs.  The active scan, configured delay, and remaining deadline are
observable through ``net-install-keyboard-waiting``,
``boot-net-install-keyboard-wait-ms``, and
``net-install-keyboard-remaining-ns``.  A zero delay or disabled Network
Install bypasses the scan; expiry continues the original EEPROM
``BOOT_ORDER``.

Network boot also parses EEPROM ``PXE_OPTION43``.  Its default match string is
``Raspberry Pi Boot``; a custom nonempty printable value can use the complete
255-byte DHCP option range.  The policy applies only to zero-address
ProxyDHCP offers.  An Option 43 mismatch is ignored while ordinary DHCP
address acquisition continues, and the selected value is observable through
``boot-pxe-option43``.

The final firmware DT publishes the raw reset-status register through the
documented big-endian ``/chosen/bootloader/pm_rsts`` cell.  Any stale
nonstandard ``rsts`` property supplied by the input DT is removed.  The
adjacent ``partition`` and ``tryboot`` cells continue to describe the selected
boot partition and one-shot tryboot state.

Behavioral boot can bind a named raw image with
``-M usb-boot-drive=ID``.  EEPROM ``BOOT_ORDER`` mode 4 reads unchanged
firmware artifacts through the PCIe VL805/xHCI path; mode 5 uses BCM2711
DWC2 USB2.  The same backend remains a QEMU USB mass-storage device after ARM
handoff.  ``usb-boot-controller=auto`` selects VL805 on Pi 4B and DWC2 on
CM4; explicit ``xhci`` or ``dwc2`` selects alternate wiring.  CM4 external
VL805/xHCI boot is additionally gated by the production EEPROM
``VL805=1`` setting.  Without that opt-in, mode 4 fails the external
controller initialization and continues to the next ``BOOT_ORDER`` source
without issuing an xHCI command.  ``boot-vl805-enabled``,
``boot-vl805-initialized``, and ``boot-vl805-status`` expose the policy and
realized-controller result.  Reset re-reads the EEPROM, and behavioral-boot
VMState version 64 preserves the selected policy and initialized state.
Pi 4B's onboard VL805 remains available independently of the CM4-only option.
The model executes the controller-visible initialization and subsequent BOT
traffic but does not claim to execute the opaque embedded VL805 MCU image.
On successful USB boot, firmware creates ``/chosen/bootloader/usb`` and
publishes the selected device's descriptor-derived ``usb-version``, xHCI-style
``route-string``, one-based ``root-hub-port-number``, and ``lun`` cells.  The
values come from the topology that completed enumeration: direct SuperSpeed
devices report USB 3 with route zero, USB 2 devices and one supported hub
level retain their actual root/downstream port identity.  A non-USB handoff
deletes any stale input subtree.  QOM exposes the same selected identity and
behavioral-boot VMState version 72 preserves it.
Behavioral
EEPROM parsing honors
``USB_MSD_PWR_OFF_TIME``, ``USB_MSD_STARTUP_DELAY``,
``USB_MSD_DISCOVER_TIMEOUT``, ``USB_MSD_LUN_TIMEOUT``, and
``USB_MSD_EXCLUDE_VID_PID``, including their documented bounds.
The exclusion value accepts the upstream comma-separated list of at most four
exact eight-digit hexadecimal ``VIDPID`` values.  It is applied to device
descriptors before storage configuration on both firmware-owned controllers;
an excluded hub is not configured or scanned, so all devices below it are
excluded as well.  Excluded-only topologies remain in discovery rather than
being misclassified as a failed LUN.  On Pi 4B, the power-off setting
distinguishes PCB revisions through 1.3 from revision 1.4 and newer.  The
legacy path applies
the full configurable interval after the hardware's short cycle; the newer
path overlaps the documented minimum two seconds held off during memory
initialization and waits only for any remainder.  An exact ``board-revision``
override is accepted only when its model, processor, and memory bits agree
with the selected machine and ``-m`` size.  CM4 reports the Pi 4B-only
power-off policy as not applicable.  A nonzero startup delay waits after
controller initialization before any enumeration and does not consume the
later discovery budget.  Missing media then enters a
virtual-time discovery wait;
enumerated but unbootable media enters a per-LUN wait.  At the deadline the
backend is rechecked before ``BOOT_ORDER`` falls through, and consumed time is
exposed through ``boot-elapsed-ms``.  This behavioral scheduler does not claim
physical enumeration timing.  ``usb-boot-drives=usb0+usb1:usb2`` creates two
devices with consecutive LUNs separated by ``+`` and devices separated by
``:``.  Realized USB devices and SCSI LUNs have stable QEMU IDs, and a qtest
drives a device-descriptor control transfer through the DWC2 host-channel DMA
registers, assigns and enumerates the hub and both storage devices, discovers
all three LUNs, completes SCSI INQUIRY and READ CAPACITY over BOT, and compares
each LUN's sector zero with its exact raw backend through READ(10).  The
behavioral firmware uses the selected VL805/xHCI or DWC2 BOT path as its
authoritative FAT/MBR/GPT reader; it no longer opens USB boot bytes through a
parallel block reader.  The VL805 path constructs xHCI command/event rings,
device and endpoint contexts, EP0 control TDs, and bulk Normal TRBs in guest
DMA, including ring recycling and slot teardown for media reprobe.
Mode/controller mismatches fail closed instead of aliasing modes 4 and 5.
Read-only QOM exposes the versioned transport name and migratable
READ-command, byte, and failed-CSW counters.  Permanent SCSI read errors drive
the normal LUN timeout and ``BOOT_ORDER`` fallback.
``usb-boot-bot-stall-once=on`` injects one invalid CBW, while
``usb-boot-bot-stall-count=N`` injects up to eight consecutive faults.  The
bounded firmware owner performs BOT Mass Storage Reset Recovery, clears the
bulk endpoint halts, resets xHCI endpoint/dequeue state when applicable, and
retries the unchanged command after each fault.  Read-only
``usb-boot-bot-recoveries`` reports successful recoveries and migrates with
the boot state.  ``usb-boot-bot-phase-count=N`` cycles through all six USB-IF
relations that require Reset Recovery (cases 2, 3, 7, 8, 10, and 13) under the
same combined eight-recovery limit.  Both controllers require ordered Reset
Recovery even if the device surfaces a phase-error CSW rather than a
transport STALL; read-only
``usb-boot-bot-phase-errors`` is migratable.  Pi 4B
exposes a PCIe VL805-compatible XHCI
endpoint with native BCM2711 MSI.  The production firmware-reset mailbox tag resets
that controller for its hardwired PCI address and exposes a migratable reset
counter; other addresses and CM4 without an onboard VL805 are no-ops.
The root port presents the production-visible Broadcom BCM2711 identity
``14e4:2711``, revision ``20``, PCI bridge class ``060400``, and a Gen2 x1
maximum link instead of the generic QEMU root-port identity and Gen4 x32
capability.  The unchanged production kernel observes that identity through
PCI sysfs.  Root identity and link capability remain stable across reset and
migration.  The internal MSI controller implements both documented
``0xfffffffc`` and ``0xffffffffc`` doorbells, the 32-vector data match,
status, clear, mask-set, and mask-clear registers, and native GIC SPI 148.
Its programmed state, pending vectors, masks, and asserted interrupt survive
live migration; reset masks every vector and clears pending state.  The final
DT retains ``msi-controller`` and ``msi-parent`` so the unchanged production
kernel uses this path for both VL805 and NVMe.  SSC, opaque VL805 firmware
loading, remaining PCIe errors/registers, and physical link timing remain
incomplete.

Behavioral firmware also exposes the exact EEPROM ``bootconf.txt`` and
valid-size ``pubkey.bin`` bytes through the production
``raspberrypi,bootloader-config`` and
``raspberrypi,bootloader-public-key`` reserved-memory nodes.  Their aligned
addresses derive from the effective ARM/VideoCore memory split, migrate with
the boot state, and participate in kernel, initramfs, and DT collision checks.
Missing records or missing DT placeholders remain disabled.

The final firmware DT also creates ``/system`` when it is absent and replaces
``linux,revision`` plus the two-cell ``linux,serial`` property with the
machine's selected board revision and OTP row-28 serial.  The root
``serial-number`` is also replaced with the 16-digit, lower-case, zero-padded
hexadecimal representation of that OTP value.  These are the same identity
sources returned by the property mailbox, so stale values supplied by boot
media cannot leak into the guest-visible tree.

Behavioral firmware also replaces ``/chosen/kaslr-seed`` with a fresh 64-bit
value from QEMU's guest-visible entropy source at every firmware handoff.
Failure to obtain entropy fails the handoff.  The value is retained across
live migration as part of the final guest DT; reset generates a new value.
QEMU's explicit ``-seed`` option retains its normal deterministic-test
semantics.

``min-boot-version=N`` models the board-manufacturing minimum EEPROM
``MFG_VER``.  Behavioral firmware publishes it as the big-endian
``/chosen/rpi-min-boot-ver`` integer consumed by ``rpi-eeprom-update``.
The default is zero for boards without a programmed minimum; the current
2026-05-17 BCM2711 manufacturing release uses version one.  This property is
construction-only and migrates.  Its software-visible value is modeled, while
the undocumented physical OTP bit placement remains a hardware-conformance
boundary.

The final tree also replaces ``/chosen/rpi-sdram-size-gbit`` with the selected
board model's installed SDRAM capacity: 8, 16, 32, or 64 for the supported
1, 2, 4, or 8 GiB models.  This value deliberately remains independent of
``total_mem`` and ``gpu_mem`` because it identifies the fitted SDRAM rather
than the effective ARM-visible allocation.

``/chosen/rpi-boardrev-ext`` is replaced with the exact 32-bit value from
row 33 of the persistent OTP backend.  It is zero when that row is
unprogrammed and does not reuse or derive bits from the normal board revision
in row 30.

The final tree's ``/chosen/os_prefix`` and ``/chosen/overlay_prefix`` strings
contain the effective prefixes selected by ``config.txt``.  Defaults are the
empty OS prefix and ``overlays/``; an explicitly selected empty prefix remains
an empty device-tree string.

The final tree's ``/chosen/bootloader/boot-mode`` cell records the
``BOOT_ORDER`` nibble of the source that reached ARM handoff.  It is updated
after fallback selection, so modes 1, 2, 4, 5, 6, and 7 identify successful
SD/eMMC, network, USB, NVMe, and HTTP boot paths respectively.

Behavioral ROM also reads the EEPROM's embedded ``BUILD_TIMESTAMP=<epoch>``
and ``VERSION:<git>`` strings.  Valid values are published as the big-endian
``/chosen/bootloader/build_timestamp`` cell and
``/chosen/bootloader/version`` string.  Missing or malformed metadata is
omitted rather than synthesized.

The adjacent ``capabilities`` cell is derived only for evidence-bounded
BCM2711 release families.  The 2020-12-11 release publishes ``0x1f`` and
releases from 2021-07-06 through the pinned 2026-05-17 production image
publish ``0x7f``.  Unknown older or intermediate images omit the property
instead of inheriting the emulator's implemented feature set.

``/chosen/bootloader/signed`` is always replaced with the public secure-boot
status bit field.  Bit zero reflects ``SIGNED_BOOT=1``, bit two reports ROM
development-key revocation, and bit three reports a programmed customer
public-key digest.  The EEPROM value is parsed strictly; unsupported values
make the EEPROM configuration invalid.

After a verified EEPROM update is programmed successfully, the ``ts: <epoch>``
line from the unchanged ``pieeprom.sig`` is retained and published as the
big-endian ``/chosen/bootloader/update_timestamp`` cell.  It survives reset,
live migration, and, when ``eeprom-status-drive`` is attached, independent
QEMU processes.  Missing or non-increasing timestamps make a self-update stale
once a successful timestamp exists.  Malformed metadata rejects the update,
and failed, blocked, stale, or already-current updates cannot replace the last
successful timestamp.

``usb-boot-bot-case-count=13`` runs the complete USB-IF BOT direction/length
matrix before unchanged boot traffic.  The firmware owner checks exact CSW
residue, distinguishes ordinary command failure from phase error, performs
Reset Recovery only for cases 2, 3, 7, 8, 10, and 13, and leaves the complete
USB image byte-identical.  ``usb-boot-bot-cases-tested`` and all recovery
state are preserved by migration.  DWC2 channel disable cancels an outstanding
asynchronous packet before the class reset, matching the architectural
channel-abort path.

The opt-in exact-release gate copies the pinned 2,977,955,840-byte Raspberry
Pi OS image unchanged to the USB backend, uses the production EEPROM's default
``0xf41`` fallback, and reaches the serial login prompt with the guest root
filesystem mounted from the same VL805/xHCI device.  Guest first-boot code
durably rewrites the MBR identity and expands partition 2 on that backend.

Alternatively, ``usb-boot-external=on`` scans bounded USB-MSD devices attached
separately to the selected controller.  It is mutually exclusive with
``usb-boot-drive`` and ``usb-boot-drives``.  This supports ordinary QEMU
devices and physical ``usb-host`` passthrough through the same pre-ARM
enumeration and BOT READ(10) reader.  For example, after unmounting the target
and identifying a stable host bus/physical-port pair::

  qemu-system-aarch64 \
    -M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,usb-boot-external=on \
    -drive if=none,id=pieeprom,format=raw,file=pieeprom.bin \
    -device usb-host,bus=vl805.0,hostbus=3,hostport=2

Use ``bus=usb-bus.0`` with ``usb-boot-controller=dwc2`` for mode 5.  Physical
passthrough requires libusb permissions and exclusive ownership.  It does not
model cable quality, connector power, overcurrent, analogue signalling, or
electrical GPIO behavior.

Behavioral NVMe boot uses one unchanged raw backend for firmware selection and
guest PCIe access::

  qemu-system-aarch64 \
      -M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,nvme-drive=nvme0 \
      -drive if=none,id=pieeprom,format=raw,file=pieeprom.bin \
      -drive if=none,id=nvme0,format=raw,file=nvme.img

``BOOT_ORDER`` mode 6 reads FAT boot files from ``nvme0`` and enters the same
firmware/config/ARM handoff used by other media.  BCM2711 has one downstream
PCIe link, so an explicit Pi 4B ``nvme-drive`` replaces the automatically
created VL805 endpoint.  ``raspi-cm4`` leaves that downstream bus available as
``pcie-root`` for an explicit endpoint.  A production-kernel gate binds the
NVMe driver and verifies a sector hash from the shared backend; MSI-controller,
error, and timing conformance remains partial.  Block-layer faults such as a
``blkdebug`` ``read_aio`` error are shared by behavioral boot and the PCIe
namespace: mode 6 falls through while a guest queue READ reports the NVMe
``Unrecovered Read Error`` completion status.  Faulted queue WRITEs report
``Write Fault`` without changing the namespace; one-shot recovery persists
the exact bytes of the following successful WRITE.  A dirty one-shot FLUSH
fault and recovery plus mismatch/success COMPARE completions are also qtested
through the real queues.  Secure mode uses the same
Raspberry-format signed EEPROM and ``boot.img`` boundary as SD, eMMC, USB, and
network boot; qtests cover signed mode-6 handoff and outer-image tamper
rejection.

The DWC2 model also accepts ``GUSBCFG.ForceDevMode`` and implements the
software-visible device configuration, control/status, periodic Tx FIFO,
endpoint-control, endpoint-interrupt, DAINT, reset, and migration state.
``GRSTCTL`` Tx/Rx/token-queue flush, frame-counter reset, HCLK soft reset,
and core soft reset commands complete without guest polling delays.  Reset
cancels asynchronous host packets, clears channel or endpoint state and
interrupt summaries according to command scope, retains root-port attachment
and programmable configuration registers, and starts a fresh host frame
epoch.  Stable post-command state migrates.  DMA mode does not stage payload
in a separate software FIFO.  With ``GAHBCFG.DMAEn`` clear in device mode,
host OUT and SETUP packets instead produce pop-on-read ``GRXSTSP`` entries and
little-endian payload words in ``EPFIFO(0)``; guest writes to
``EPFIFO(n)`` supply IN packets and ``DTXFSTS`` reports remaining words.
Configured depths bound both paths, FIFO-empty endpoint interrupts, selective
and all-FIFO flush, USB/core reset, overflow rejection, and live migration are
modeled and qtested.  With DMA disabled in host mode, guest writes to
``HCFIFO(channel)`` stage OUT and SETUP packet words, while IN transactions
produce ``GRXSTSP`` host-channel records and payload words in ``FIFO(0)``.
Non-periodic and periodic free-space reporting, receive backpressure, selected
or all-FIFO flush, reset, and migration of partially filled transmit or queued
receive FIFOs are modeled and qtested.  Asynchronous host IN packets reserve
their receive bytes and status slots before submission, preventing concurrent
channels from overcommitting the FIFO.  A delayed-storage qtest repeatedly
cancels one asynchronous and one backpressured channel through alternating
host/core resets and proves clean class recovery.  Physical FIFO and
enumeration timing remain outside the behavioral model.  Device-mode USB
reset clears global IN/OUT NAK state, endpoint registers, interrupt summaries,
and FIFO contents.  A sixteen-cycle control-plus-two-bulk-endpoint PIO campaign
proves clean re-enumeration and exact interleaved SETUP/IN/OUT traffic after
every reset.  EP0 consumes its programmed SETUP count and supports zero-length
IN/OUT status stages.  A three-deep back-to-back SETUP window stays armed
until its count and residual bytes are consumed, including a first request
completed through a destination socket after live migration.  Multi-packet
bulk transfers terminate on a short packet with residual state retained.
Partially received framed requests migrate with the endpoint and FIFO state.
A suspended device sets ``DSTS.SUSPSTS`` and ``GINTSTS.USBSUSP`` and
backpressures endpoint tokens without discarding that state.  Suspend survives
live migration; resume raises ``GINTSTS.WKUPINT`` and transfers continue,
while USB reset and disconnect clear suspended state.  A
transport-neutral host-token boundary performs SETUP/OUT/IN guest-memory DMA
or device PIO, transfer accounting, and endpoint completion/error interrupts,
and accepts host connect, reset, enumeration, suspend, resume, and disconnect
events.  The
optional ``device-chardev`` property exposes those operations through the versioned
framing contract in ``dwc2-device-transport.h``.  This connects endpoint data
to an in-process transport.  In behavioral RPIBOOT mode the VideoCore ROM now
arms EP0/EP1 itself, enumerates with the BCM2711 ROM descriptors, receives the
24-byte boot message and announced ``bootcode4.bin`` through DWC2 DMA,
exposes the captured byte count/hash, and returns ROM status.  Set
``rpiboot-bootcode-trusted-sha256`` to the digest of that exact unchanged
artifact; absent or mismatched trust returns a nonzero status and blocks the
second enumeration.  ``rpiboot-bootcode-trust`` exposes the result.  After
disconnect/re-enumeration it publishes the serial-bearing descriptors,
issues the exact 260-byte get-size/read/done requests, accepts segmented
``config.txt`` and ``boot.img`` bulk responses, and exposes their sizes and
hashes.  The Linux Raw Gadget packet proxy presents this in-process DWC2
device to unchanged host ``rpiboot`` while retaining protocol ownership
inside QEMU.
Malformed standard EP0 transfers fail like USB hardware: GET requests in the
OUT direction and SET requests in the IN direction stall, SET_ADDRESS remains
non-mutating, and USB reset recovers enumeration.  Supported standard
requests also require their defined recipient, value, index, and length.
SET_ADDRESS is committed after its status stage; USB configuration starts at
zero, changes through SET_CONFIGURATION, survives live migration, and clears
on reset/disconnect.  GET_STATUS reports self-power and endpoint halt state,
and endpoint-1 halt supports SET_FEATURE/CLEAR_FEATURE.  Configuration one
requires a nonzero USB address.  Vendor RPIBOOT control and endpoint-1 bulk
traffic remain unavailable at configuration zero; reset, deconfiguration, and
second-stage enumeration therefore repeat the normal address/configuration
sequence before transfer.

Behavioral network boot mode 2 can either bind a named raw FAT image with
``-M network-boot-drive=ID`` or discover files solely from a TFTP server.
The optional image is an integrity-checking corpus, not a converted guest
disk.  EEPROM ``NET_BOOT_MAX_RETRIES``,
``DHCP_TIMEOUT`` bounds the complete DHCP sequence, ``DHCP_REQ_TIMEOUT`` sets
the retransmission interval for both DISCOVER and REQUEST, and
``TFTP_FILE_TIMEOUT`` bounds each TFTP file.  Active deadlines, retransmission
delays, and their remaining virtual time migrate.  Network BOOT_ORDER uses a
real DHCPDISCOVER/OFFER/REQUEST/ACK exchange through the configured GENET
network backend by default.  ``-M network-boot-wire=off`` retains the older
corpus-only compatibility path for isolated tests.  The pre-ARM client
validates IPv4/UDP/BOOTP framing, checksums,
transaction identity, and the client MAC, and releases receive ownership before
guest descriptor DMA begins.  DHCP option 97 carries the Pi boot machine
identifier: by default its 16-byte GUID contains the ``RPi4`` little-endian
FourCC, board revision, least-significant four MAC-address bytes, and OTP serial.
EEPROM ``NETCONSOLE`` accepts the documented maximum 32-character
``src_port@src_ip/dev,dst_port@dst_ip/dst_mac`` form.  Empty ports select
6665/6666, an empty destination IP selects broadcast, and an empty destination
MAC selects all zeroes.  A configured source IP is mandatory.  Before any
boot source is attempted, QEMU waits for a GENET link or the same
``DHCP_TIMEOUT`` deadline; link-up resumes immediately and timeout continues
without attempting DHCP solely for netconsole.  Clean-room boot observations
are duplicated as UDP payloads, while exact private firmware wording and
cadence remain outside the behavioral contract.  The endpoint, counters, link
wait, and exact remaining deadline are exposed through ``boot-netconsole-*``
properties and survive migration.
EEPROM ``DHCP_OPTION97=0`` selects the legacy serial repeated four times; any
other 32-bit value replaces the FourCC prefix.  It then resolves the server with ARP, fetches
``config.txt``, discovers recursive includes from received bytes, derives the
start/fixup, kernel, DTB, optional cmdline/initramfs names, and finally probes
the overlay map and selected overlays.  Each RRQ carries the Pi-observed
``tsize=0`` and ``blksize=1024`` options.  A valid OACK selects any server
block size from 8 through 1024 bytes, reports the bounded transfer size, and
is acknowledged with block zero.  A server that starts with DATA instead uses
classic 512-byte blocks; ERROR code 8 triggers one option-free classic RRQ.
Malformed, duplicate, out-of-range, or unrequested OACK options fail closed
with ERROR code 8.  File-not-found ERROR replies skip optional files.  Lost RRQs
and mid-file ACKs are retransmitted after 500 ms, then with exponential delays
of 1, 2, and at most 4 seconds, without extending the per-file
``TFTP_FILE_TIMEOUT`` deadline.  The retransmission schedule and remaining
delay migrate with an active transfer; ``boot-tftp-retransmit-count`` exposes
the current attempt's retry count.  Classic
16-bit TFTP block numbers roll over for large kernel and initramfs files.
Valid TFTP ERROR replies terminate the transfer immediately: file-not-found
still drives optional-file and firmware/prefix fallback policy, while other
server errors fail the network attempt without waiting for the file deadline.
Packets from an unexpected transfer ID receive ERROR code 5.  After the last
short DATA block, the client retains the transfer ID and final block for a
500 ms dally interval, re-ACKs repeated final DATA, and restarts that interval
after each duplicate.  The dally identity and exact remaining interval migrate.
Every file has a role-specific bound and all received configuration, handoff,
and overlay bytes are consumed directly.  When the optional corpus is bound,
exact size and SHA-256 equality are additionally required.  A not-found reply
for the default ``start4.elf`` selects the matching legacy
``start.elf``/``fixup.dat`` pair.  If the prefixed kernel-plus-DTB set is
incomplete, already received prefixed base artifacts are discarded and the
set is fetched again without ``os_prefix``.  EEPROM ``TFTP_IP`` accepts a
unicast dotted-decimal address and overrides only the ARP/TFTP server selected
by DHCP; the DHCP-assigned client address is retained.  DHCP subnet-mask and
router options select an on-link server or gateway as the Ethernet next hop
without changing the TFTP destination address.  If ``TFTP_IP``, ``CLIENT_IP``,
and ``SUBNET`` are all configured, the client skips DHCP and begins with ARP;
``GATEWAY`` is used as the next hop when the TFTP server is outside the static
subnet and may be omitted for an on-link server.  Static addresses, masks,
gateway selection, active ARP waits, and remaining deadlines migrate.
EEPROM ``TFTP_PREFIX`` selects an eight-digit lower-case OTP serial
directory (mode 0), the exact ``TFTP_PREFIX_STR`` of at most 32 printable
characters (mode 1), or a lower-case hyphenated GENET MAC directory (mode 2).
This device prefix is present in each wire RRQ but is not confused with the
logical path used to resolve config includes and overlays.  If neither the
modern nor legacy start file exists below it, only the device prefix is
cleared and the start-file request is retried at the TFTP root.  The
``boot-tftp-prefix-mode``, ``boot-tftp-prefix``, and
``boot-tftp-prefix-fallback`` observations and active fallback state migrate.
EEPROM ``MAC_ADDRESS`` accepts an exact colon-separated unicast address.
``MAC_ADDRESS_OTP=A,B`` composes one from two distinct customer OTP rows 0
through 7.  The last active assignment wins and an empty value restores the
configured GENET address.  This one effective identity feeds GENET packets,
DHCP client identity, the firmware board-MAC mailbox property,
``TFTP_PREFIX=2``, and final Device Tree ``local-mac-address``; reset and live
migration retain it in direct-loader and behavioral-boot modes.
The read-only ``boot-client-ip``, ``boot-subnet``, and ``boot-gateway`` machine
properties expose the parsed EEPROM configuration.  Negotiated block size,
OACK/ACK0 retransmission state, reported size, and classic-fallback state
migrate with an active transfer.  DHCP server identifier option 54 is retained
separately from the bootstrap server: requests and ACK/NAK validation use the
former, while BOOTP ``siaddr`` selects TFTP.  If ``siaddr`` is empty, option 66
accepts either a dotted-decimal address or a DNS host name.  Host names are
resolved with an A query through the first unicast option-6 DNS server; DNS
uses the DHCP retry interval and overall deadline, and routes an off-subnet
resolver through the learned gateway.  The selected DHCP server remains the
legacy fallback when option 66 is absent.  A zero-``yiaddr`` proxy-DHCP offer may independently replace
the TFTP server before the selected server's ACK, without changing the lease or
DHCPREQUEST option 54.  An ACK from another server or for another offered
address is ignored.  DHCP, proxy-TFTP, and resolved TFTP identities migrate.
RFC 2132 option 52 enables bounded parsing of the overloaded 128-byte BOOTP
``file`` and 64-byte ``sname`` fields; conflicting scalar values, invalid
lengths, nested overload declarations, and truncated options reject the whole
reply.  In line with Pi 4/CM4's multi-file firmware boot rather than generic
PXE, a syntactically valid option 67 is intentionally ignored: the first RRQ
remains ``config.txt``.  Conflicting repeated option-67 values reject the DHCP
reply.  Hardware-oracle retry cadence remains pending.

On ``raspi-cm4``, ``emmc-drive=ID`` attaches the named backend to EMMC2 as an
eMMC device.  The guest observes MMC CID/CSD and EXT_CSD capacity and uses
SDHCI Auto CMD23 for bounded multi-block transfers.  RAM selection and eMMC
capacity remain independent.  The optional ``emmc-boot-drive=ID`` backend
contains equal-sized boot0 and boot1 areas in that order; its total size must
be a nonzero multiple of 256 KiB.  ``emmc-rpmb-drive=ID`` supplies a separate
RPMB area in 128 KiB units.  With either hidden-area backend, ``emmc-drive`` is
still the exact user-area image starting at byte zero, so an unchanged Imager
image needs no private prefix or conversion.  Device identity and hidden-area
defaults are currently generic rather than board-SKU-specific.  A captured
device identity can be replayed with ``emmc-cid=HEX``.  This accepts the 15 CID
payload bytes or the 16-byte Linux sysfs representation; QEMU validates or
reconstructs the CRC/end byte.  Storage capacity continues to come solely from
the user backend, independently of the selected 1/2/4/8 GiB RAM model.

Deterministic guest-visible controller failures are available with
``emmc-data-error=timeout|crc``, ``emmc-data-error-after=N`` (512-byte
aligned), and ``emmc-data-error-count=N``.  They raise the standard SDHCI
data-error status after N successful card data bytes and leave the backing
image layout unchanged.  These options are intended for driver recovery tests;
they do not model analog signaling or sub-sector power-loss durability.

The EMMC2 controller commits PIO writes only when a complete 512-byte sector
has arrived.  Reset therefore discards a partially staged sector, while an
already completed sector remains in the persistent user-area image.  This
controller-side boundary is migration-safe.  For card-side durability tests,
``emmc-cache-size=N`` advertises and models a bounded volatile cache through
EXT_CSD.  The eMMC device implements CMD6 ``CACHE_CTRL`` and ``FLUSH_CACHE``,
guest read-after-write, capacity-pressure writeback, backend flush, and dirty
cache migration.  ``emmc-cache-power-loss-on-reset=on`` explicitly discards
unflushed sectors on reset; flushed and pressure-written sectors remain.
The default is off so existing reset behavior remains durable.  This models
digital durability boundaries, not NAND translation, wear, cache timing, or
electrical power loss.
``emmc-cache-flush-sector-delay-us=N`` optionally commits one dirty sector per
virtual deadline during CMD6 ``FLUSH_CACHE``.  Active progress and its timer
migrate; reset retains completed sectors and applies the selected loss policy
to the remaining tail.  Qtests cover every cut in a four-sector flush and a
timer-callback backend failure followed by an exact retry.  Zero remains
synchronous, and nonzero values are test inputs rather than calibrated device
timing.
``emmc-program-sector-delay-us=N`` similarly queues complete uncached sectors
and makes one durable per virtual deadline.  It applies to both normal direct
programming and reliable writes; pending bytes, partition identities, timer,
and progress migrate.  Reset preserves the completed prefix and discards the
unfinished suffix.  Read-only program active/pending/completed properties make
that boundary observable, and backend failure retains the queue for retry.
Zero keeps the synchronous path.
A bounded stress qtest runs 64 mixed cache/direct/reliable transactions,
rotates every four-sector cut point, resets at the next deadline minus one
microsecond, and crosses seven active live migrations.  Each cycle verifies
the exact persistent prefix and untouched suffix.
CMD35/CMD36/CMD38 eMMC erase uses the advertised 512 KiB high-capacity erase
groups and ``0xff`` erased-memory content.  Erase bypasses and invalidates
overlapping volatile-cache entries.  ``emmc-erase-group-delay-us=N`` can make
one group durable per virtual deadline; active range/progress migrate, reset
keeps the completed prefix, and backend failure retains the current group for
retry.  Zero is synchronous.  Secure trim/sanitize and physical NAND behavior
remain unmodeled.
EXT_CSD also advertises enhanced reliable writes.  Bit 31 in the Auto CMD23
Argument 2 marks the following bounded CMD25 transfer reliable; each completed
sector bypasses the volatile cache and flushes its selected backend before
completion.  Active reliable state migrates, while unrelated normal cache
entries remain volatile.  This supplies the MMC-side FUA durability contract
without altering the attached image format.
Backend flush failure is reported through the standard R1 ``ERROR`` bit; a
one-shot blkdebug gate proves retry and cache isolation.

For removable Pi 4B SD media, a QMP ``eject`` during active controller I/O
aborts the pending transfer, clears data-active and buffer-ready state, and
raises card-removal plus SDHCI data-timeout status when enabled.  Partial PIO
bytes below one complete sector are discarded.  The abort and interrupt state
remain deterministic across migration, and the same image can be reinserted,
reset, and read again.  Electrical contact bounce and physical power-loss
durability remain hardware-test boundaries.

The legacy SDHOST controller exposes programmable power, clock-divider,
timeout, command, response, status, and FIFO state at the BCM2711 address.
Command timeout and FIFO underflow/overflow are guest-visible write-one-to-clear
errors.  Those registers and errors migrate, while system reset clears them,
the FIFO, and the IRQ.  GPIO electrical mux behavior and physical timing are
outside this digital register contract.

The fail-closed ``contrib/raspi4/gpio_proxy.py`` helper can map the socket to
dedicated GPIO character-device lines with official libgpiod v2 bindings or
to a dedicated USB CDC ACM MCU.  Physical outputs require explicit per-line
authorization.  The USB adapter must implement autonomous disconnect/watchdog
release.  Neither backend replaces the requirement for an isolated,
voltage-safe HIL fixture.

Missing devices
---------------

 * In-process host-visible USB RPIBOOT transport (raspi-cm4).  Experimental
   Linux Raw Gadget and configfs helpers provide the current external
   provisioning boundary.  The opt-in ``provision-state-file`` machine
   property enforces fail-closed QEMU/helper ownership across that boundary,
   including foreground configfs and command-visible Raw BOT owners that
   retain the lock for the whole host imaging interval.
   ``contrib/raspi4/cm4_provision.py`` automatically
   supervises the complete external QEMU/RPIBOOT/Imager/QEMU sequence;
   see :doc:`../../devel/raspi4-platform`.
   An abrupt post-flash QEMU death deliberately leaves ``qemu-owned``.
   ``provision-recover-stale=on`` explicitly recovers only that state after
   obtaining the lifecycle lock; failed or active flashing states remain
   rejected.  ``provision-recovery`` exposes whether recovery occurred.
   A killed configfs foreground owner is recovered separately with
   ``cm4_mass_storage.py ... recover-stale``; this lock-ordered teardown always
   leaves ``flash-failed`` and never promotes uncertain media.
