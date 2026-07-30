.. SPDX-License-Identifier: GPL-2.0-or-later

Raspberry Pi 4 platform fidelity
================================

Scope and definition of done
----------------------------

The reference targets for this branch are a Raspberry Pi 4 Model B revision
1.5 and Compute Module 4 revision 1.0, both with 2 GiB RAM and BCM2711.  They
are separate machines because CM4 has soldered eMMC, a dedicated GPIO40
``nRPIBOOT`` input, and a USB-device provisioning path.

The machine family also models the production 1, 2, 4, and 8 GiB RAM SKUs.
Standard ``-m 1G|2G|4G|8G`` is the single selector; it derives matching
revision-code memory bits, factory OTP identity, mapped RAM, and final DT
memory nodes.  A persistent OTP image remains authoritative and fails closed
when its board revision does not match the selected capacity.  RAM capacity
and CM4 eMMC capacity are deliberately independent dimensions.

For this project, *platform fidelity* means that software-visible state
transitions, register behavior, persistent bytes, reset causes, interrupts,
and boot-media decisions match a physical reference board for a versioned test
corpus.  It does not mean analog or electrical identity.  SD wear, marginal
signal integrity, voltage rails, RF behavior, and cycle-exact VideoCore timing
remain hardware-in-the-loop tests.

The branch must not describe a stage as emulated until an automated
conformance test observes it.  A behavioral replacement for closed firmware
must be identified as such; it must not be called execution of the real
firmware.

Behavioral boot therefore exposes a versioned VideoCore boundary through
QOM.  ``videocore-execution-mode=behavioral-replacement-v1`` and
``videocore-boundary-version=1`` identify the clean-room contract, while
``videocore-artifact-policy=exact-input-bytes-not-instruction-executed``
states how unchanged ROM/EEPROM/``start4.elf``/fixup artifacts are used.
Their exact sizes and SHA-256 values remain separately observable.  Direct
loader mode reports this boundary inactive.  Changing the version requires
updating its behavioral corpus and conformance expectations.

Upstream baseline
-----------------

The current ``raspi4b`` and ``raspi-cm4`` machines provide four Cortex-A72
CPUs, selectable 1/2/4/8 GiB RAM (2 GiB by default),
GICv2, DMA, timers, CPRMAN, GPIO, PL011 and AUX UART, BCM2711 RNG200,
BCM2711 AVS thermal monitor, framebuffer, SD/MMC, DWC2/MPHI USB2, mailbox
properties, SPI, and I2C.

The default machine path calls ``arm_load_kernel()`` from ``hw/arm/raspi.c``.
``-kernel`` and ``-dtb`` therefore place artifacts directly into ARM-visible
memory.  Supplying ``-bios``/``-M firmware=`` similarly copies the file to
``0x80000`` and starts the ARM CPU there.  The branch also provides an opt-in
``boot-mode=behavioral`` boundary described below.  That path now reaches
tested ARM64 and ARM32 handoffs without using any direct-loader argument.

The three inherited BCM2835 BSC controllers are mapped at the BCM2711
peripheral addresses.  Their control path now distinguishes enable from the
one-shot start command, applies the public register field masks, completes an
address NACK with ``ERR|DONE``, recomputes IRQ output after W1C status
changes, and terminates a live bus transaction on reset.  Post-load restores
the IRQ from migrated control/status state.  The generic qtest covers all
three instances with TMP105 traffic plus negative/register/reset cases, and a
Pi 4 qtest repeats the boundary at ``0xfe205000``, ``0xfe804000``, and
``0xfe805000``.  That gate additionally issues a repeated start from an
active write into read mode, migrates with one returned byte still pending,
and completes the exact remaining byte on the destination.  The modeled
directional FIFO is 16 bytes deep, applies the public RXR/TXW thresholds,
ignores writes while full, stalls reads while full, supports active clear,
and migrates its contents and stalled transfer.  Deterministic
``clock-stretch-after``/``clock-stretch-cycles`` fault injection delays a
selected byte in live SCL cycles: a short hold resumes, while a hold beyond
``CLKT`` completes with ``CLKT|DONE`` and the normal DONE IRQ.  Its timer
tracks the connected VPU/core clock and divider, pauses under clock gating,
migrates, and resets cleanly.  The generic ``I2CSlaveClass::stretch``
callback lets an attached slave request the same delay once per byte; the
TMP105 test-only stretch properties cover short resume, timeout, reset,
clock-gating, and active migration through that device-originated path.
The BCM2711 BSC is single-master-only; multi-master arbitration is therefore
outside the hardware contract.  The documented ten-bit sequence is supported:
``11110xx`` in A selects the high bits and the first FIFO byte supplies the
low eight bits.  Generic targets expose ``ten-bit-address``; active target
identity, BSC address phase, FIFO contents, DIV, FEDL/REDL, and CLKT migrate.
Tests cover valid write/read, address and data NACK recovery, reset, and an
active ten-bit write migration.  Physical FEDL/REDL edge timing remains in
the deferred Pass 2 HIL conformance campaign.

``hw/arm/raspi4b.c`` retains the modeled PCIe, GENET, and BCM2711 AON L2
interrupt-controller nodes.  It preserves the following unimplemented node
in a supplied BCM2711 device tree and forces its final ``status`` to
``disabled``:

* ``brcm,brcm2711-dvp``

The same post-overlay policy preserves and disables the unmodeled CYW43455
SDIO and Bluetooth endpoints.  ``peripheral-model-policy`` reports
``preserve-disabled-unmodeled-v1`` and ``peripheral-exclusions`` reports the
exact semicolon-separated compatible-string set.  This distinguishes an
explicit machine exclusion from a missing firmware node and prevents an
overlay from advertising hardware that the machine does not implement.

Both BCM2711 HDMI DDC controllers are mapped at their production BSC and
auto-I2C addresses.  They implement the register layout used by Linux
``i2c-brcmstb``, including auto-I2C ownership release, 32-byte packed
transfers, completion and address-NAK status, the EDID segment pointer, and
the DDC EDID address.  HDMI0 and HDMI1 return the exact validated connector
bytes also consumed by firmware filters and mailbox properties.  When a CTA
HDMI Forum VSDB advertises SCDC, address 0x54 exposes the standard version,
TMDS clock-ratio, scrambling, read-request, and channel-status registers;
non-advertising connectors NAK that address.  EDID and SCDC pointers,
negotiated state, registers, and packed buffers migrate in controller VMState
v2; reset clears controller state while firmware resamples connector inputs.
The production DDC nodes are therefore no longer in
``peripheral-exclusions``.  Electrical bus timing, clock stretching,
arbitration, and signal integrity remain open.

HDMI0 and HDMI1 expose the BCM2711 core ``HOTPLUG`` connected bit at their
production windows.  The mutable ``connected`` QOM property represents a
host-side cable transition, gates DDC access, and pulses the production
connected/removed AON inputs 4/5 and 10/11.  The edge-latched AON L2
controller at 0xfef00100 implements raw status, W1C clear, mask
status/set/clear, and the parent GIC SPI 96 route.  Both controller layers
migrate; reset clears the interrupt state and derives connector presence from
the newly validated EDID.  The AON DT node is therefore no longer in
``peripheral-exclusions``.  Physical cable voltage, debounce, and edge timing
remain hardware-conformance work.

Each HDMI port also maps its production BCM2711 CEC window.  The VC4 control,
timing, address, and 16-byte TX/RX data registers support nominally timed
transmit completion against a configurable peer logical-address mask,
ACK/NACK fault selection, and bounded host-side receive injection.  HDMI0
TX/RX use AON lines 0/1; HDMI1 uses 8/7.  Active transmit deadlines,
register/data state, peer topology, and fault selection migrate, while reset
cancels pending traffic.  This is protocol-level CEC behavior; open-drain
voltage, bus arbitration, edge shape, and physical timing require HIL.

RNG200 now provides a stateful 16-word FIFO, warm-up/total-bit accounting,
writable total-bit and FIFO thresholds, W1C interrupt status and enable
registers, and the native GIC SPI 125 output.  Reads deplete the FIFO, normal
mode refills it, and a test-only no-refill mode makes depletion deterministic.
NIST-failure and master-lockout injection, a deterministic xorshift input
seed, resets, unread FIFO bytes, interrupt state, and generator progress all
survive their declared reset or migration boundary.  The default path still
uses QEMU's guest-random source; deterministic mode must only be used by tests.

The firmware framebuffer property path supports physical and virtual geometry,
viewport offsets, pitch, palette updates, allocation, and display blanking.
The virtual surface is never smaller than its physical viewport, horizontal
offsets are converted from pixels to bytes at the selected depth, and palette
ranges beyond entry 255 fail.  Blanking clears the rendered surface and
migrates with the framebuffer configuration; reset returns to the configured
unblanked mode.  This is a software-visible scanout boundary, not an HDMI,
HVS or physical-display timing model.  The
raw EDID input described below supplies both firmware filter identity and
firmware-property EDID blocks.
Calibrated generation timing, statistical entropy quality, and physical
silicon conformance remain outside this logical model.
The BCM2711 AVS monitor is mapped at ``0xfd5d2000``.  Its status register
provides the Linux-visible 10-bit code and both validity bits, while the
unchanged production DT supplies the calibration coefficients.  Qtests cover
default/custom temperature, invalid-sensor injection, writes, and reset.
``thermal-temperature-millicelsius`` and ``thermal-sensor-valid`` expose these
inputs directly on both Pi machine types and remain writable through QOM for
deterministic runtime thermal/fault injection.
Live-migration tests start the destination with deliberately opposite
temperature/valid defaults and prove that both a valid 80,000 m°C sample and
an invalid-sensor state are restored exactly and survive reset.  The unchanged
Pi 4B and CM4 kernels expose 24,823 m°C through thermal sysfs.
Dynamic workload coupling and physical calibration remain conformance work.
The firmware property mailbox reads the same live millidegree sample for
``GET_TEMPERATURE`` and reports 85,000 m°C for
``GET_MAX_TEMPERATURE`` on BCM2711.  The public mailbox value has no validity
bit, so invalid-sensor injection clears the native AVS validity flags without
inventing a second temperature value.  Pi 4B/CM4 qtests prove mailbox/QOM
coherence through runtime updates, reset, and migration.

The property mailbox also implements the Pi 4/CM4 ``GET_THROTTLED`` current
and sticky-history contract.  The machine's
``firmware-throttled-current`` input accepts bits 0--2 for under-voltage,
Arm-frequency capping, and active throttling.  As a current bit is asserted,
the corresponding history bit 16--18 is latched.  The read-only
``firmware-throttled-status`` value is identical to the guest mailbox
response.  Both current and history state survive warm reset and migrate; a
Pi 4B/CM4 qtest changes the live input, clears the current condition, and
proves that history remains latched on the destination.  This is a logical
fault-injection boundary.  Electrical rail thresholds, PMIC behavior,
brownout timing, and temperature-to-throttle dynamics remain HIL-only.

Firmware clock properties are connected to the CPRMAN muxes for EMMC, UART,
ARM, CORE/VPU, V3D, H264, ISP, PWM, EMMC2, and VEC.  Clock-state and rate
writes therefore change the same QEMU clocks consumed by peripherals.
``GET_CLOCK_RATE`` returns the programmed next-enable rate when a mux is
stopped, while ``GET_CLOCK_MEASURED`` reports the live output and returns zero
while stopped.  Unsupported IDs report the ABI's nonexistent state or a zero
rate instead of accepting a no-op.  The CPRMAN reset path resynchronizes every
exported mux output after restoring registers, including muxes whose selected
source did not emit a clock event.  A Pi 4B/CM4 qtest gates the shared PWM
clock, changes its disabled next rate, resumes both PWM0 and PWM1 at the new
rate, migrates the state, and verifies reset.  Firmware min/max policy,
voltage/DVFS coupling, and calibrated transitions remain incomplete.

Before executing any firmware property tag, the mailbox validates the complete
declared buffer structure.  The header, every 32-bit-padded tag extent, and an
end tag must fit without wrapping the 32-bit VideoCore address space.
Structural failure returns ``0x80000001`` before tag side effects.  Six-byte
values such as the board MAC advance over their two padding bytes without
modifying them.  Pi 4B/CM4 qtests cover undersized buffers, truncated tag
extents, a missing end tag, address overflow, non-mutation, and valid MAC
alignment.  All response payload writes are clipped to the caller's declared
value size while the response header reports the full desired length.  A
chained short-buffer test proves truncated revision, MAC, ARM-memory, and
serial tags cannot overwrite padding, the next tag, or the end marker.
The preflight also requires the ABI's zero request code and a value buffer at
least as large as the fixed payload consumed by that tag.  Dynamic palette,
customer-OTP, and private-key requests must contain every declared word before
processing begins.  A chained Pi 4B/CM4 test proves that a valid
``SET_REBOOT_FLAGS`` tag preceding an undersized clock request has no side
effect when the complete message is rejected.

Framebuffer properties additionally follow the firmware transaction rule:
all supported Set tags are applied before any framebuffer Get response,
independent of their order in the message.  The prepass covers geometry,
offsets, depth, pixel and alpha modes, overscan, palette, blanking, release,
and allocation without reordering unrelated property tags.  Mixing framebuffer
Test tags with framebuffer Get or Set tags rejects the complete request before
response headers or side effects, as does repeating any framebuffer tag.
Test-only transactions run against one temporary configuration: physical and
virtual geometry plus viewport offsets are normalized in sequence, and
unsupported depth, pixel-order, or alpha candidates retain the previous
temporary value.  The Test result never changes live scanout, while Set tags
share the same scalar validation.  Pi 4B/CM4 qtests prove Get-before-Set
geometry and palette results, mixed Test/Set non-mutation, chained normalized
Test responses, and duplicate rejection before mutation.
Unsupported tags retain their request code and value bytes while the complete
buffer still succeeds and later aligned tags continue to execute.  This also
applies to known tags that the model has not implemented, such as
``GET_TOUCHBUF``; they do not advertise successful responses containing
uninitialized payload.  A chained Pi 4B/CM4 test covers an odd-sized unknown
tag followed by successful board-model and board-revision responses.

Firmware power tags implement the documented logical device IDs 0--10.
``GET_POWER_STATE`` and ``SET_POWER_STATE`` return the current logical on/off
bit, nonexistent IDs return bit 1, and the request's wait bit is not reflected
as device state.  ``GET_TIMING`` returns zero because power-stabilization
latency is not modeled.  State resets to the available-device mask and
migrates in property VMState v5; Pi 4B/CM4 qtests cover both ID boundaries,
round trips, reset, and live migration.  These tags do not claim to model
physical rails or peripheral electrical shutdown.

Framebuffer palette get/test/set tags share the 256-entry RGBA table at the
VideoCore RAM base used by 8-bit scanout.  ``GET_PALETTE`` reports a desired
1,024-byte response and clips writes to the caller buffer.
``TEST_PALETTE`` validates the complete declared range and payload exactly as
set does, but leaves all entries unchanged.  Pi 4B/CM4 qtests cover the final
two entries, valid and overflowing tests, full readback, non-mutation, and a
six-byte response with untouched padding.

Framebuffer overscan get/set tags retain top, bottom, left, and right values,
while the test tag evaluates proposed values without changing configuration.
Each margin pair must leave at least one display pixel.  Valid margins render
black borders and bilinearly scale the complete transformed framebuffer into
the remaining rectangle, following the FKMS destination-margin operation.
The four values migrate in framebuffer VMState v6 and return to board defaults
on reset.  Pi 4B/CM4 qtests cover valid and invalid Set/Test requests, exact
asymmetric borders before and after transpose, migration, and reset.  Exact
VideoCore HVS scaler coefficients remain outside this software renderer.

``FRAMEBUFFER_RELEASE`` disables scanout and clears the rendered surface while
retaining geometry and VRAM bytes.  A subsequent ``FRAMEBUFFER_ALLOCATE``
re-enables that configuration and returns the same base and size.  The enabled
state migrates in framebuffer VMState v6; post-load resizes the destination
console to the migrated geometry, and reset restores the configured enabled
surface.  Pi 4B/CM4 qtests prove rendered release, released-state migration,
reallocation, and reset.

Framebuffer layer and transform Get/Test/Set tags participate in the same
atomic transaction.  Layer retains its opaque 32-bit firmware value.
Transform accepts the eight VideoCore rotation/mirror bit combinations;
invalid candidates return the previous temporary value.  Test responses never
alter live state.  Both values migrate in framebuffer VMState v6 and reset to
zero.

Transform also controls scanout for all eight VideoCore rotation/mirror
combinations.  Transposed modes swap console width and height; signed
destination row/column pitches implement rotation and reflection through the
existing dirty-memory renderer.  Reconfigure, reset, and VMState post-load
restore transformed geometry.  Pi 4B/CM4 qtests compare every RGB pixel of a
six-color source under all eight transforms and repeat the final comparison
after migration.

``SET_VSYNC`` follows the Linux firmware ABI as a synchronous command with a
dummy u32 payload.  The property response remains pending until the next
virtual vblank calculated from the selected display's active FKMS refresh
rate, with a 60 Hz fallback when no timing exists.  Property VMState v10
migrates the pending wait and timer; reset cancels them.  Undocumented Get/Test
forms return zero.  Pi 4B/CM4 qtests prove the exact 50 Hz boundary, mailbox
blocking, migration midway through a wait, reset cancellation, and recovery.

``SET_CURSOR_INFO`` accepts the documented 24-byte request for a 16--64
pixel, 32-bit ARGB surface in guest DMA memory.  ``SET_CURSOR_STATE`` accepts
the 16-byte visibility, signed-position, and coordinate-space request.
Hotspots, display coordinates, framebuffer coordinates, output clipping, and
ARGB composition affect the rendered console.  Invalid dimensions, hotspots,
addresses, enable values, or flags return a nonzero firmware result without
changing the retained cursor.  A visible cursor is sampled from guest RAM on
each display update, and framebuffer VMState v6 migrates its pointer,
geometry, position, flags, visibility, and backing RAM.  Reset removes custom
cursor state.  The opaque firmware default cursor used before an info request
is not reproduced without a physical-firmware capture.

Pi 4B and CM4 expose two firmware displays.  Sequential framebuffer indices
zero and one map to the fixed DispmanX identifiers HDMI0=2 and HDMI1=7 used by
the Raspberry Pi firmware-KMS driver.  ``SET_DISPLAY_NUM`` retains only a valid
index, while ``SET_DISPLAY_POWER`` retains an independent boolean for each
firmware display ID.  Invalid selection or power values do not alter state.
Read-only QOM properties expose the current index and two-bit power mask;
property VMState v10 migrates both and reset selects display zero with both
outputs powered.  Pi 4B/CM4 qtests cover enumeration, ID mapping, validation,
selection, power, migration, and reset.  The model still has one rendered
console, so this state does not claim independent dual-head scanout.

The FKMS ``GET_DISPLAY_TIMING`` and ``SET_TIMING`` tags use the documented
36-byte payload for HDMI0 ID 2 and HDMI1 ID 7.  Reset derives each port's
preferred pixel clock, sync intervals, totals, refresh, polarity, interlace,
aspect, and HDMI/DVI flag from the first detailed timing in that port's
validated EDID.  Set accepts only internally ordered timings, known flags,
zero padding, a nonzero refresh, and a pixel clock no greater than 600 MHz.
An invalid request returns the retained mode without changing it.  Timing Set
also participates in framebuffer Set-before-Get transaction ordering.
Property VMState v10 migrates both exact payloads and derives them from EDID
when loading an older stream; reset resamples the configured connector files.
This is firmware mode negotiation state, not an HDMI pixel-clock, HVS/PV, HPD,
or electrical timing model.

The AUX mini-UART retains its eight-byte receive FIFO and now provides a
separate eight-byte transmit FIFO instead of blocking QEMU and delivering
every byte instantaneously.  Its clock input consumes the BCM2711 VPU/core
output from CPRMAN.  The Pi 4 reset profile supplies the firmware's fixed
250 MHz mini-UART clock, and the programmed 16-bit divider schedules one
ten-bit frame at a time.  Runtime CPRMAN divider writes immediately change
baud timing; stopping the VPU clock freezes TX, and a mid-frame rate change
preserves the exact remaining source-clock cycles.  LSR, STAT, IIR, and the
parent IRQ reflect FIFO full/empty/fill and transmit completion; the IIR clear
command can discard either FIFO.  VMState version 4 preserves both FIFO
orders, register state, active transmit deadline, and a clock-stopped frame's
remaining cycles.  CPRMAN post-load reconstruction restores derived clocks
from migrated registers rather than destination defaults.  Socket qtests
prove active and stopped-clock migration, ordered timed output, and ordered
input consumption.  Physical baud drift, break/framing injection, and
electrical flow control remain conformance work.

``cyw43455-sdio`` now supplies a migratable, fault-injectable transport
foundation.  It implements SDIO enumeration and direct/extended I/O,
Broadcom/CYW43455 CIS identity, CCCR/FBR enable and block-size state,
function-1 backplane/window storage, the exact ``0x15294345``
BCM4345/revision-9/AXI signature, the real 800 KiB TCM aperture at
``0x198000``, and a driver-consumable DMP EROM for its ChipCommon, SDIO,
D11, and ARMCR4 cores.  Each core has a live AI slave-wrapper aperture;
IOCTL/reset state migrates, and ARMCR4 reports the exact TCM bank size.
Function-2 implements production SDPCM/BCDC control and Ethernet framing over
standard QEMU network backends.  Unread receive frames, active interrupt state,
and bounded packet-loss progress migrate; configurable TX/RX loss recovers
automatically after the exact occurrence window.  Reset and byte, command,
control, packet, and fault telemetry are also implemented.  Qtests
independently parse the EROM, migrate active wrapper, backplane, and packet
state, exercise both functions, and prove deterministic CMD5
timeout/reset recovery.  The opt-in
``QTEST_CYW43455_FIRMWARE`` gate additionally requires an expected SHA-256,
downloads that unchanged production ``.bin`` through CMD53, and reads every
byte back from TCM.  The device can be instantiated onboard on Pi 4B or CM4;
an unchanged production kernel, brcmfmac modules, firmware, and NVRAM bind
both SDIO functions, register ``wlan0``, complete DHCP, and ping the QEMU
user-network gateway.  The same opt-in enables the production Bluetooth DT
child and PL011 H4 controller.  Standard and Broadcom-vendor commands produce
deterministic command-complete events, while exact ACL bytes bridge through
the ordinary serial chardev.  Partial commands, pending events, FIFO state,
and command/event/ACL counters migrate and reset deterministically.
``contrib/raspi4/wireless_release_gate.py`` freezes the combined software
release proof.  It hashes QEMU, kernel, DTB, diagnostic initramfs, matching
official kernel-module package, and console log, then requires successful
unchanged brcmfmac/btbcm/hci_uart load and bind markers, WLAN DHCP/ping, HCI0
enumeration, and BCM4345 identity while rejecting driver and command-complete
failures.  The diagnostic initramfs changes only orchestration and embeds the
byte-identical modules from the pinned official package.
RF, antenna, coexistence, regulatory, and radio-power behavior remains
assigned to the hardware fixture.

PWM0 is mapped at VC address ``0x7e20c000`` (``0xfe20c000`` from the
BCM2711 ARM view).  BCM2711 also instantiates an independent PWM1 block at
``0x7e20c800`` (``0xfe20c800``).  Each controller has two-channel control,
status, range/data registers, DMA configuration, and its own shared 16-word
FIFO, including full/empty and sticky write/read errors.  FIFO-enabled
channels consume one word after each programmed range of their shared CPRMAN
PWM source-clock cycles.  Live clock-rate changes, stop/resume, reset, and
migration preserve each active period's remaining source cycles.  Empty
consumption sets the channel gap flag, while named normal and panic
DMA-threshold outputs track each controller's DMAC configuration.  PWM0's
normal request drives BCM DMA peripheral map 5 and PWM1 drives map 1.  Each
panic output selects the matching channel's four-bit DMA panic priority while
normal DREQ remains the transfer gate.  Simultaneously ready channels use
stable highest-effective-priority-first arbitration, with channel number
breaking ties, and CS.DREQ reports the selected request level.  Normal and
panic request state migrates live.  A
destination-DREQ control block therefore fills only through the requested
level, retains ACTIVE/HELD plus exact source, length, row, and control-block
progress while waiting, and resumes across live migration.  Two named
``channel-enabled`` output lines expose logical enable state for board
composition.  Two separate logical ``waveform`` outputs run on QEMU virtual
time and implement the distributed PWM algorithm, mark-space mode, MSB-first
serializer, FIFO repeat, idle bit, and output polarity.  When both channels
share a controller FIFO, A/C/E words remain assigned to channel 0 and B/D/F
to channel 1.  A shorter-range channel idles at its boundary until the longer
channel also requests data; the next owner persists across starvation, clock
stop, and live migration.  BCM2711 connects
PWM0_0 to GPIO12 ALT0 and GPIO18 ALT5, PWM0_1 to GPIO13 ALT0, GPIO19 ALT5,
and GPIO45 ALT0, and PWM1 channels 0/1 to GPIO40/41 ALT0.  The resulting
levels and edges propagate through ``GPLEV``, GPIO event detection, named
output/output-enable wires, the optional host bridge, reset, and live
migration.  Electrical edge shape and physical waveform conformance remain
incomplete.

VideoCore firmware GPIO expander
--------------------------------

The property-mailbox device models firmware GPIO IDs 128--135 used by Linux's
``raspberrypi-exp-gpio`` driver.  GET/SET STATE and GET/SET CONFIG retain
direction, polarity, termination, pull, and state, return the firmware success
convention by clearing the response GPIO ID, and reject IDs outside the eight
line window.  Eight named ``exp-gpio-in`` and ``exp-gpio-out`` lines expose a
logical board-composition boundary.  State is resettable and VMState v2 keeps
compatibility with the earlier property device.  A live migration qtest
verifies that configuration and driven state survive and continue to control
the logical output on the destination.

The exact release gate proves this removes the former firmware-GPIO, LED, and
regulator probe failures and allows the unchanged image to reach its serial
login prompt.  The logical lines are not yet connected to board-specific
internal rails, and reset defaults, polarity, and electrical effects still
require Pi 4B/CM4 hardware traces.

BCM2711 GPIO logical bridge
---------------------------

The GPIO controller has a direction-aware logical bridge boundary for all 58
BCM2711 pins.  Each pin has a named ``pin-input`` input, its existing value
output, and a named ``pin-output-enable`` output.  An input level of zero or
one actively drives the pin; a negative level disconnects the external driver.
For GPIO input mode, ``GPLEV`` then reports an active external drive or resolves
the configured BCM2711 pull.  For GPIO output mode, it reports the separately
retained GPSET/GPCLR latch and ignores the external input.  Direction changes
update both value and output-enable signals, including GPIO54--57 in the upper
bank.  Input state and drive-valid masks are migration state.

The synchronous and asynchronous rising/falling enable registers and high/low
level enable registers latch ``GPEDS``.  Status is write-one-to-clear; an
enabled active level immediately relatches.  The three Linux pin groups
GPIO0--27, GPIO28--45, and GPIO46--57 drive GIC SPI 113--115.  The fourth
BCM2711 wake parent at SPI 116 is exposed but remains inactive until wake
policy is modeled.  The logical model does not distinguish the silicon's
synchronous filter from asynchronous edge sampling.

The optional machine property ``gpio-chardev=ID`` binds this contract to any
QEMU chardev.  A local socket configuration is::

  -chardev socket,id=gpio,path=/run/user/1000/rpi-gpio.sock,server=on,wait=off \
  -M raspi4b,gpio-chardev=gpio

``raspi-cm4`` inherits the same property.  The ASCII version-1 protocol is
LF-framed, ignores CR, accepts fragmented or batched lines, limits a line to
95 bytes, and starts each connection with::

  RPI-GPIO 1 58

Host-to-QEMU commands are::

  PING
  GET <pin>
  GET ALL
  SET <pin> 0
  SET <pin> 1
  SET <pin> Z
  GET SIGNALS
  GET SIGNAL EEPROM_NWP
  SET SIGNAL EEPROM_NWP 0
  SET SIGNAL EEPROM_NWP 1
  SET SIGNAL EEPROM_NWP Z
  GET SIGNAL SD_OVERCURRENT
  SET SIGNAL SD_OVERCURRENT 0
  SET SIGNAL SD_OVERCURRENT 1
  SET SIGNAL SD_OVERCURRENT Z
  RELEASE ALL

``Z`` disconnects the external driver so the configured pull resolves the
input.  QEMU replies with ``PONG 1``, ``PIN <pin> <level> <output-enable>``,
``SIGNAL <EEPROM_NWP|SD_OVERCURRENT> <0|1|Z>``, ``END``, ``OK``, or a stable
``ERR`` code. A
successful ``SET`` emits the
resulting ``PIN`` record before ``OK``.  ``GET ALL`` emits all 58 records and
``END``.  ``GET SIGNALS`` emits both dedicated board-level signals and
``END``.  QEMU also pushes ``PIN`` when guest direction, output, input, or pull
state changes, and emits ``RESET`` after a device reset.  ``RELEASE ALL`` and
transport disconnect both return every pin and board signal to high impedance.
Externally driven input levels remain connected across a controller or system
reset, matching a physical fixture that continues to drive the pin while the
BCM2711 resets.  Disconnecting the chardev still releases every input.

An incoming migration destination deliberately suppresses its protocol banner
while VMState is still loading.  A connected physical daemon therefore cannot
observe or apply the destination's reset-default directions and levels.  After
the input masks/values, output latches/directions, pulls, event configuration,
latched status, and IRQ state are restored, QEMU emits the normal idempotent
banner.  The existing daemon responds with ``GET ALL``, applies the complete
migrated output state, and resends current physical inputs.  A qtest connects
both source and destination sockets, proves there is no pre-load destination
traffic, then verifies the restored driven input, output-enable/value, pull,
latched edge and IRQ before detecting a new post-migration edge.

On CM4, GPIO40 is also the active-low ``EMMC_DISABLE``/``nRPIBOOT`` strap.
If the host bridge explicitly drives that line, behavioral ROM samples it on
reset and it takes precedence over the backwards-compatible ``nrpiboot``
machine property.  ``SET 40 0`` selects RPIBOOT, ``SET 40 1`` deasserts it,
and ``SET 40 Z`` restores property control.  The read-only
``nrpiboot-sampled`` and ``nrpiboot-source`` machine observations report the
latched result and whether it came from ``gpio40`` or ``machine-property``.
This lets the same reviewed GPIO/HIL map drive both runtime GPIO and the real
CM4 boot-mode boundary.

``EEPROM_NWP`` is a dedicated input-only board signal rather than a fabricated
BCM GPIO number.  When driven through the socket, it overrides the
``eeprom-nwp`` fallback property for live status changes and recovery
transactions.  Low locks status-register protection changes; high permits
them; ``Z`` restores property control.  ``eeprom-nwp-source`` reports
``gpio-bridge`` or ``machine-property``. GPIO VMState v6 preserves the
driven board signal, while behavioral-boot VMState v45 preserves the sampled
level and source.

``SD_OVERCURRENT`` is a second input-only board signal. A driven value
overrides the ``sd-overcurrent`` fallback property and
``sd-overcurrent-source`` reports the active boundary. On Pi 4B behavioral
boot, an asserted signal with ``SD_OVERCURRENT_CHECK=1`` disables modeled SD
power for exactly five virtual seconds before probing again. The loop, power
state, count, and exact remaining deadline migrate. Setting the EEPROM option
to zero records a warning and continues without removing power. CM4 ignores
this Pi 4B-only signal.

``contrib/raspi4/gpio_proxy.py`` implements the host daemon for official
libgpiod v2 GPIO character devices.  Its versioned JSON map rejects unknown
fields, duplicate virtual or physical lines, invalid GPIOs, and unsafe drive
settings.  It requests all lines as inputs before connecting, requires an
explicit ``allow_output`` for each driven line, atomically changes direction
and output value, monitors both physical input edges, and returns all lines to
input on reset, denied output, protocol failure, signal, or disconnect.  Bias,
active-low polarity, drive mode, and kernel debounce are per-line policy.  The
mock backend makes these transitions testable without claiming HIL coverage.
An optional ``signals`` array maps the input-only ``EEPROM_NWP`` and
``SD_OVERCURRENT`` names to libgpiod or USB-adapter lines without exposing
them to the guest as BCM2711 GPIOs. Repeated banners, reset, migration
resynchronization, and physical edge events refresh them through
``GET SIGNALS`` and ``SET SIGNAL``.

This transport and daemon connect isolated ``/dev/gpiochip`` lines without
replacing the guest-visible BCM2711 controller.  The ``usb-serial`` backend
connects the same boundary to a dedicated USB CDC ACM MCU using the
deterministic protocol in
``contrib/raspi4/gpio-usb-serial-protocol-v1.md``.  It performs a versioned
handshake, input-first configuration, atomic output changes, reads and edge
delivery, and acknowledged ``SAFE`` release.  Pseudo-terminal tests exercise
the entire host-side wire contract, asynchronous-edge command races, and
disconnect cleanup.  ``contrib/raspi4/gpio_adapter`` supplies a portable
firmware core, a compiled host self-test, and an RP2040 Pico SDK 2.3.0
USB/GPIO frontend.  The firmware enforces the 500 ms session watchdog while
the host sends a 200 ms keepalive; a separate MCU watchdog covers a stalled
firmware loop.

It deliberately does not grant a remote transport any authentication.  Use a
permission-restricted local Unix socket and an electrically protected fixture.
The RP2040 frontend cross-builds with the pinned Pico SDK 2.3.0 commit and
official Arm GNU 15.2.Rel1 toolchain.  Two independent clean builds produced
the manifest-pinned byte-identical ``.bin`` and ``.uf2``.  No flashed adapter,
fixture-specific reviewed board map, active contention detector, wake policy,
sampling-time conformance, or analog/electrical behavior is verified yet.
The local host lacks suitable hardware, so both physical backends remain
HIL-unverified.

Capability map
--------------

.. list-table::
   :header-rows: 1
   :widths: 24 16 35 25

   * - Layer
     - Current state
     - Evidence / limitation
     - Required gate
   * - Raw SD image write
     - Implemented in branch tooling
     - Exact byte copy, SHA-256 verification, injected interruption, and
       durable verified resume; opened Linux devices must be removable,
       writable, whole, large enough, unmounted, and absent from raw swap
     - Cross-namespace mount audit and destructive real-reader HIL fixtures
   * - MBR/GPT image structure
     - Implemented in branch tooling
     - Bounds and GPT CRC checks; FAT/ext signature detection.  Behavioral
       boot also validates matching primary/backup GPT metadata and entry
       arrays before selecting a bounded FAT partition
     - Broaden the production-image and malformed-media corpus
   * - ARM kernel and userspace
     - Branch partial
     - Direct-load tests remain; production gates boot both unchanged component
       artifacts and the complete pinned raw Raspberry Pi OS release through
       EEPROM, raw SD, firmware behavior, ARM handoff, serial login, and a
       guest-originated durable first-boot disk-ID update
     - Match final DT and hardware handoff
   * - SD/MMC controller
     - Upstream partial
     - Block access and event-driven empty-slot insertion/ejection exist;
       early VideoCore ownership is behavioral
     - Card-detect timing and register-trace differential tests
   * - BCM2711 ROM, OTP, ``nRPIBOOT``
     - Branch partial
     - Persistent public OTP rows, reset causes, and gated input are qtested
     - Private GPIO encoding, RSA, remaining policy, and physical conformance
   * - SPI boot EEPROM
     - Branch partial
     - Exact 512 KiB persistent backend, section parser, logical
       erase/program and faults
     - NOR bit-transition rules, flash timing, and physical conformance
   * - ``recovery.bin``
     - Branch partial
     - Raw FAT discovery and clean-room update outcomes are qtested
     - Executable/RSA policy, automatic reboot, status outputs, and oracle
   * - EEPROM ``BOOT_ORDER``
     - Branch partial
     - Ordered SD/eMMC and USB-MSD plus GENET-backed DHCP/ARP and complete
       resolved-file TFTP, media events, retry/fallback, migration, STOP, and
       RESTART are qtested
     - Add static-IP and final-block policy, measured timing, and oracle
   * - VideoCore firmware / ``start4.elf``
     - Branch behavioral handoff
     - FAT paths and a bounded config subset select unchanged artifacts; the
       model patches DT and performs ARM64 handoff, but does not execute
       VideoCore instructions
     - Overlay, complete generated-DT, clock, and hardware-trace conformance
   * - PCIe root complex
     - Branch partial
     - BCM2711-compatible root/indexed config windows, 1 GiB outbound window,
       INTx A-D, Pi 4B VL805, and a CM4 endpoint slot enumerate under Linux
     - Internal MSI/SSC, remaining controller registers/errors/timing, exact
       root-port identity, and hardware conformance
   * - GENET Ethernet
     - Branch partial
     - Native v5 revision, register file, two interrupt banks, UniMAC MDIO,
       external PHY identity/link, reset/migration state, v5 40-bit descriptor
       DMA, all 17 TX/RX rings, and a standard QEMU NIC backend; socket qtests
       prove bidirectional packet bytes, 17-slot MDF filtering, promiscuous
       mode, MIB packet counters/reset, bounded migratable TX/RX DMA fault and
       recovery behavior, and the production kernel obtains DHCP
     - HFB classification/steering, remaining statistics, traffic migration,
       WOL/EEE, link/error scripting beyond DMA, exact-release networking, and
       hardware conformance
   * - PWM
     - Branch partial
     - Two channels, control/status/range/data, 16-word FIFO, sticky errors,
       CPRMAN-cycle FIFO scheduling, gap state, DMA request thresholds,
       DREQ-paced BCM DMA refill, all three transmitter modes, FIFO repeat,
       independent PWM0/PWM1 blocks and FIFOs, GPIO12/13/18/19/40/41/45
       muxing and edge detection, stopped-clock migration, and logical outputs
       are qtested; an unchanged official
       ``pwm-2chan.dtbo`` enables the production DT node
     - Broader malformed/error conformance, production PWM1 overlay/driver,
       and HIL
   * - Firmware GPIO expander
     - Branch partial
     - Eight mailbox-managed lines retain config/state, expose logical wires,
       migrate live, and allow the exact release to reach serial login without
       regulator probe failures
     - Board wiring, hardware reset defaults, older-version migration, and HIL
   * - Power faults and timing
     - Logical injection only
     - Host image tool can stop at an exact byte
     - QEMU reset/fault points; electrical HIL remains required

Architecture
------------

The work is split into boundaries that can be reviewed independently:

1. **Image laboratory**: deterministic host-side flash, inspection, corruption,
   and power-loss scenarios.  This stage is usable now.
2. **Boot contract**: a trace format shared by QEMU and a physical UART/USB
   fixture.  Every transition records reset cause, source, persistent writes,
   and handoff.
3. **Persistent platform state**: QEMU block-backed SPI EEPROM, OTP properties,
   write protection, reset causes, and explicit ``nRPIBOOT`` input.
4. **Behavioral first stage**: a documented ROM state machine that selects
   recovery, EEPROM, or USB device boot.  This is a clean-room behavioral model,
   not execution of Broadcom mask-ROM bytes.
5. **Firmware boundary**: either a legally redistributable executable
   VideoCore path or a tested behavioral loader for FAT, ``config.txt``,
   overlays, mailbox state, DT construction, and ARM handoff.
6. **Board completion**: persistent CM4 eMMC plus logical RNG200, AVS thermal,
   PWM0, a packet-capable GENET NIC with MDF filtering, and a partial BCM2711
   PCIe/VL805 path and a migratable DWC2 device-register, endpoint-DMA,
   framed host transport, and behavioral two-stage RPIBOOT ROM/file server
   exist;
   PCIe MSI/SSC and full error behavior, GENET HFB/WOL/EEE, timed
   Raw Gadget packet proxying for DWC2 device mode,
   active-transfer migration, detailed eMMC identity, and peripheral/HIL
   conformance remain.

Each boundary retains the existing direct-kernel mode for regression testing.
A future full-boot mode must be opt-in until it boots the conformance corpus,
then it can become the machine default only through QEMU's compatibility
versioning process.

Experimental behavioral boot boundary
-------------------------------------

The legacy direct-loader remains the default.  Select the clean-room first
stage explicitly and bind an exact 512 KiB BCM2711 SPI EEPROM image through a
named block backend.  Other geometries fail closed instead of importing the
2 MiB A/B flash layout used by BCM2712.  An optional 512-byte OTP backend stores the 66 public
32-bit rows in little-endian order followed by unused padding::

  qemu-system-aarch64 \
      -M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,otp-drive=piotp \
      -drive if=none,id=pieeprom,format=raw,file=pieeprom.bin \
      -drive if=none,id=piotp,format=raw,file=otp.bin \
      -drive if=sd,format=raw,file=sd.img

USB mass-storage boot uses one named raw image for both sides of the modeled
handoff.  The behavioral EEPROM reader selects unchanged files from the image,
and QEMU attaches the same backend as a real ``usb-storage`` peripheral so the
guest can enumerate the same medium after ARM handoff.  ``BOOT_ORDER`` mode 4
uses the PCIe VL805/xHCI path; mode 5 uses the BCM2711 DWC2 USB2 path.
``usb-boot-controller=auto`` selects VL805 on Pi 4B and DWC2 on CM4, while
``xhci`` and ``dwc2`` explicitly model alternate carrier/connector wiring::

  qemu-system-aarch64 \
      -M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,usb-boot-drive=usbboot \
      -drive if=none,id=pieeprom,format=raw,file=pieeprom.bin \
      -drive if=none,id=usbboot,format=raw,file=usb.img

For more than one device or LUN, ``usb-boot-drives`` describes the topology
in deterministic enumeration order.  A colon starts the next USB device and
a plus starts the next LUN on the current device.  For example,
``usb0+usb1:usb2`` creates LUNs 0 and 1 on device 0 and LUN 0 on device 1::

  qemu-system-aarch64 \
      -M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,usb-boot-drives=usb0+usb1:usb2 \
      -drive if=none,id=pieeprom,format=raw,file=pieeprom.bin \
      -drive if=none,id=usb0,format=raw,file=device0-lun0.img \
      -drive if=none,id=usb1,format=raw,file=device0-lun1.img \
      -drive if=none,id=usb2,format=raw,file=device1-lun0.img

Enumeration records the successful candidate before firmware handoff.
The generated tree replaces ``/chosen/bootloader/usb`` with four big-endian
cells: descriptor-derived ``usb-version``, the xHCI-compatible
``route-string``, one-based ``root-hub-port-number``, and selected ``lun``.
Direct and one-hub-deep topologies are covered for VL805/xHCI and BCM2711
DWC2; non-USB handoff removes a stale subtree.  Read-only QOM mirrors the
identity and VMState version 72 migrates it after handoff.

The legacy singular property remains shorthand for device 0, LUN 0.  QEMU
accepts at most eight USB devices and eight total LUN backends, probes devices
then LUNs in ascending order, rejects duplicate or missing backends, and uses
``usb-storage`` for single-LUN devices or USB BOT plus SCSI disks for
multi-LUN devices.  Each realized USB device has the stable QEMU ID
``raspi4-usb-boot-N`` and serial ``QEMU-RPI-BOOT-DN``; disks behind a
multi-LUN BOT device use ``raspi4-usb-boot-N-lun-L`` IDs.  A qtest performs a
DMA-driven control transfer through the DWC2 host-channel registers, reads the
automatically inserted root-hub device descriptor, assigns the hub, resets and
enumerates both downstream devices, verifies their SCSI-over-BOT interfaces,
queries each maximum LUN, and completes an INQUIRY CBW/data/CSW sequence
against all three LUNs.  After consuming the normal initial unit-attention
status, it also validates READ CAPACITY and compares a 512-byte READ(10) from
each LUN byte-for-byte with its named raw backend.  This verifies that the
configured identities and unchanged media are usable through the live
controller bus, not only present in the QOM tree.

The ``usb-storage`` endpoint is removable and the backend may start empty.
Standard QMP media change and eject operations notify both the SCSI device and
the behavioral ROM.  Insertion during discovery or LUN wait probes the new
unchanged bytes immediately; removal during LUN wait returns to discovery and
restarts that timeout.  A qtest covers empty discovery, invalid insertion,
eject, bootable replacement, ordered device/LUN fallback, exact artifact hash,
live migration, and immediate ARM handoff.  The behavioral firmware reader
owns the selected controller before ARM release, enumerates the same realized
devices, and feeds FAT/MBR/GPT and artifact reads exclusively from BOT
READ(10) results.  ``usb-boot-transport`` reports either
``vl805-xhci-host-bot-scsi-read10-v1`` or
``dwc2-host-bot-scsi-read10-v1``; migratable command, byte, and
command-failure counters make the path observable.  Controller-mismatch tests
prove that modes 4 and 5 cannot alias.  A permanent
backend read error becomes a failed SCSI CSW and takes the configured
``USB_MSD_LUN_TIMEOUT`` fallback to SD.

For deterministic recovery testing, ``usb-boot-bot-stall-once=on`` replaces
the first CBW signature with an invalid value.  The bounded
``usb-boot-bot-stall-count=N`` form injects up to eight consecutive invalid
CBWs.  After each one, the firmware owner performs the USB Mass Storage class
reset, clears both bulk endpoint halts, and retries the complete command.  The
xHCI owner additionally submits Reset Endpoint and Set TR Dequeue commands
before ringing the recovered transfer rings.  ``usb-boot-bot-recoveries``
reports completed recovery sequences and is preserved by migration.  Qtests
exercise four consecutive recoveries through each controller.

``usb-boot-bot-phase-count=N`` cycles through all six USB-IF BOT relations
that require Reset Recovery: cases 2 (``Hn<Di``), 3 (``Hn<Do``), 7
(``Hi<Di``), 8 (``Hi<>Do``), 10 (``Ho<>Di``), and 13 (``Ho<Do``), up to the
same combined eight-recovery bound.  A phase-error CSW is never accepted as
an ordinary command failure: both owners execute ordered Reset Recovery and
retry the original unchanged command.  ``usb-boot-bot-phase-errors`` exposes
the number of injected relations and migrates with their consumption state.
Qtests run all six phase-error cases through each controller and migrate the
xHCI campaign.

``usb-boot-bot-case-count=N`` exercises a prefix of the complete thirteen-case
USB-IF direction/length matrix.  A count of 13 uses non-mutating TEST UNIT
READY, INQUIRY, and no-op MODE SELECT commands to cover Hn, Hi, Ho, Dn, Di,
and Do equality, short, long, and direction-mismatch relations.  The owner
validates exact CSW residue for the seven non-phase cases, accepts GOOD or
ordinary command-failure status as SCSI policy, and requires Reset Recovery
for exactly cases 2, 3, 7, 8, 10, and 13.  It then executes the original
unchanged boot command.  ``usb-boot-bot-cases-tested`` exposes the number of
completed relations.  VMState version 40 preserves that counter and campaign
position.  The qtest compares the entire USB backend before and after the
campaign and migrates the xHCI run.

This remains a clean-room behavioral replacement, not instruction execution
of the VideoCore USB boot stack.  The bounded VL805 firmware owner programs
the xHCI model's DCBAA, command/event rings, input/output contexts, Address
Device and Configure Endpoint commands, EP0 Setup/Data/Status TDs, and bulk
Normal TRBs in guest DMA.  It recycles transfer and event rings, disables old
slots before media reprobe, and reinitializes after controller reset.  Direct
root devices, automatic hubs, multiple devices/LUNs, hot media, and migration
are covered.  Bounded repeated malformed-CBW recovery and the complete
thirteen-case USB-IF BOT direction/length matrix are covered on both xHCI and
DWC2.  DWC2 GRSTCTL FIFO/token flush, frame-counter reset, HCLK soft reset,
and core soft reset now complete as self-clearing commands.  Host reset
cancels pending asynchronous packets and clears channel state without
disconnecting the root port; core reset also clears active device endpoints
and their interrupt summaries while preserving programmable configuration
and selected mode.  A qtest covers register scope, frame restart, migration,
and attachment/configuration preservation.  Device-mode PIO now queues
bounded OUT/SETUP receive statuses and payload words, consumes per-endpoint IN
Tx FIFOs, reports ``DTXFSTS`` space and FIFO-empty interrupts, applies
selective/all flushes, rejects overflow atomically, and migrates queued bytes
plus incomplete ``device-chardev`` frames.  EP0 SETUP completion consumes
``DOEPTSIZ0.SUPCNT`` and keeps the endpoint armed until a programmed
three-request back-to-back window or its residual bytes are exhausted.  The
first request migrates midway through its transport frame and the remaining
two complete without re-arming.  Zero-length IN/OUT status stages and
full-plus-short multi-packet bulk transfers complete with exact
packet/residual accounting.
The framed device transport also carries suspend and resume lifecycle events.
Suspend sets ``DSTS.SUSPSTS`` and ``GINTSTS.USBSUSP`` and returns NAK-equivalent
backpressure for endpoint tokens while preserving armed endpoints and FIFO
bytes.  Suspended state migrates.  Resume clears the status bit, raises the W1C
``GINTSTS.WKUPINT``, and exact traffic continues; USB reset or disconnect
clears suspended state and stale lifecycle interrupts.
USB bus reset clears both global NAK latches as well as endpoint/FIFO state.
A sixteen-cycle qtest combines EP0 SETUP with EP1 and EP2 in both directions,
verifies NAK backpressure, resets and re-enumerates, then requires exact
interleaved PIO traffic with no stale status or payload.  A SETUP frame split
across source and destination transport sockets proves parser-state migration.
Host mode with DMA disabled stages OUT/SETUP words in each channel's
``HCFIFO``, waits for complete packets without advancing ``HCDMA``, reports
non-periodic and periodic transmit space, and delivers IN packet status plus
payload through ``GRXSTSP`` and ``FIFO(0)``.  Receive-depth backpressure,
selected/all transmit flushes, receive flush, reset, and migration of both
partially staged transmit packets and queued receive packets are qtested
against a live USB keyboard descriptor transaction.  The DMA-owned RPIBOOT
path is unchanged.  Before submitting an asynchronous slave-mode IN packet,
the controller reserves its rounded payload bytes and two receive-status
slots; other channels backpressure against both queued and reserved capacity.
Reservations migrate and are released on completion, cancellation, channel
disable, host reset, or core reset.  A delayed USB-storage qtest alternates
host/core reset across sixteen cycles with one asynchronous and one
backpressured IN channel, then proves BOT class-control recovery.  Physical
enumeration/FIFO timing, long-duration multi-device campaigns, and hardware
trace conformance remain incomplete.

External controller-owned USB media
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``usb-boot-external=on`` leaves storage realization to normal QEMU ``-device``
arguments and dynamically discovers up to eight USB-MSD devices and their
reported LUNs on the selected boot controller.  It is mutually exclusive with
the named machine-owned backend properties.  A qtest attaches unchanged media
as an ordinary ``usb-storage`` device to stable buses ``vl805.0`` and
``usb-bus.0`` and proves mode-4 xHCI and mode-5 DWC2 handoff, exact hashes,
BOT command counts, and selected device/LUN identity.  Because ``usb-host`` is
another USB device on those buses, the same bounded path supports libusb
passthrough without a special block-byte shortcut.

The host operator must identify and unmount the physical target, grant libusb
access, and use an exclusive stable selector such as ``hostbus`` plus
``hostport``.  Physical port power, reset timing, disconnect races, cable
integrity, and electrical conformance remain HIL gates.
Pi 4B now also exposes a PCIe
``1106:3483`` VL805-compatible
XHCI controller whose USB2/USB3 ports are guest-visible; the production kernel
enumerates it and binds ``xhci_hcd``.  The functional gate uses the
unchanged official EEPROM default ``BOOT_ORDER=0xf41`` and boots the pinned
official firmware, fixup, kernel, DTB, overlay, and initramfs files through
the USB fallback without direct loader arguments.
The production ``NOTIFY_XHCI_RESET`` mailbox tag recognizes the hardwired
bus-1/slot-0/function-0 address ``0x00100000`` and cold-resets this controller
without removing its PCI function.  Other addresses and CM4 without an
onboard VL805 are no-ops.  Read-only ``xhci-reset-count`` telemetry migrates
with property VMState v6 and clears on machine reset.  Opaque VL805 firmware
content/load timing, MSI, and physical port behavior remain incomplete.

Network mode 2 can discover its boot set directly from TFTP::

  qemu-system-aarch64 \
      -M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom \
      -drive if=none,id=pieeprom,format=raw,file=pieeprom.bin \
      -netdev user,id=net0 \
      -global bcm2711-genet.netdev=net0

An optional named raw FAT backend adds an exact integrity oracle for a known
TFTP corpus::

  qemu-system-aarch64 \
      -M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,network-boot-drive=netboot \
      -drive if=none,id=pieeprom,format=raw,file=pieeprom.bin \
      -drive if=none,id=netboot,format=raw,file=tftp-corpus.img

The behavioral boundary distinguishes a missing DHCP response from a TFTP
server whose required files cannot be resolved.  DHCP traverses the configured
GENET backend as a real DISCOVER/OFFER/REQUEST/ACK exchange before an optional
integrity corpus is accepted.  ``network-boot-wire=off`` is an explicit
compatibility escape for synthetic corpus-only tests.

Firmware-pair selection follows the BCM2711 bootloader-owned configuration
boundary.  Only the top-level configuration may set ``start_x``,
``start_debug``, ``gpu_mem``, ``gpu_mem_256``, ``gpu_mem_512``, or
``gpu_mem_1024``.  The selected pairs are
``start4x.elf``/``fixup4x.dat`` (falling back together to
``start_x.elf``/``fixup_x.dat``), ``start_db.elf``/``fixup_db.dat``, or,
for effective ``gpu_mem=16``, ``start4cd.elf``/``fixup4cd.dat``.
The installed-memory selector overrides ``gpu_mem``; therefore
``gpu_mem_1024`` applies to every supported 1--8 GiB Pi 4B/CM4 model while
the 256/512 MiB selectors remain inactive.  The effective reservation
defaults to 76 MiB and is clamped to the documented 16 MiB minimum.  One
resolved value drives cut-down-pair selection, final DT memory banks,
framebuffer VideoCore base/size, and ARM/VC memory mailbox responses.  Values
that leave no addressable low ARM memory fail closed.  QOM reports the
effective MiB and selector source.  Reset and live migration reconstruct the
same value from unchanged boot media; qtest checks the QOM selection, final DT
banks, and mailbox boundary after both lifecycle operations.  A complete explicit
``start_file``/``fixup_file`` pair wins over
those shortcuts, while directly naming a cut-down pair is rejected.  Static
FAT and response-discovered network artifacts use the same selection rules.

Firmware identity is sourced consistently at the property-mailbox boundary.
The board-model response is the legacy firmware value zero, the board-revision
response comes from the selected persistent OTP identity, and the 64-bit
board-serial response zero-extends OTP serial row 28.  The board-MAC response
uses the same configured GENET address exposed by the machine.  Pi 4B and CM4
qtests verify all four responses before reset, after reset, and after live
migration, including the direct-loader reset path.  A short-buffer test proves
the model response reports its complete four-byte desired length while writing
only the caller's two bytes and preserving padding.

Top-level ``total_mem`` is parsed as an unsigned MiB capacity and clamped to
128 MiB through the selected machine's installed RAM.  Handoff removes every
input DT memory node after the board callback, then publishes the lower ARM
bank after the existing VideoCore reservation and an upper
``memory@40000000`` bank only for effective capacity above 1 GiB.  This keeps
physical installed RAM, OTP board revision, and ``memory-model`` stable while
changing exactly the firmware-advertised capacity.  The result is shared by
Pi 4B/CM4 and block/response-discovered network paths, and is observable as
``firmware-total-mem-mb``.

Top-level ``bootcode_delay`` is a strict unsigned seconds value.  A nonzero
value creates a pre-firmware pending action on every block and network source,
rather than sleeping the host thread.  Timer expiry resamples both EDIDs,
reinitializes the firmware observation, and resolves the same media again.
Behavioral-boot VMState version 47 preserves the selected seconds, consumed
state, and exact remaining virtual nanoseconds.  Reset re-arms the property
and included files cannot set it.  QOM exposes the value as
``firmware-bootcode-delay-seconds``.

The bootloader-owned ``sdram_freq`` setting is also top-level-only and parsed
as an unsigned MHz request.  BCM2711 does not support configurable SDRAM
frequency, so every Pi 4B/CM4 memory model retains the documented effective
3200 MHz LPDDR4 rate.  QOM separately exposes
``firmware-sdram-frequency-requested-mhz``,
``firmware-sdram-frequency-requested``, and
``firmware-sdram-frequency-mhz`` so tests can prove that even an explicit
zero request was observed without falsely changing the modeled memory clock.

The top-level-only ``uart_2ndstage`` boolean drives the actual modeled PL011
UART0 frontend instead of a side-band logger.  Enabling it selects
GPIO14/GPIO15 ALT0, programs 115200 8N1, and emits a deterministic clean-room
three-line record for enablement, source/artifact selection, and ARM handoff.
The byte and line counters are read-only QOM observations.  Behavioral-boot
VMState version 48 migrates enablement and counters without replay; reset
starts a new record.  Includes cannot set the property and non-boolean values
fail configuration parsing.  Hardware-oracle work must still compare the
private VideoCore text, ordering, timing, voltage levels, and baud tolerance.

The client emits a checksummed broadcast IPv4/UDP/BOOTP frame with the Pi PXE
vendor class, validates reply framing, checksums, transaction ID and client
MAC, records the packets in GENET MIB counters, and releases its receive hook
before Linux descriptor DMA owns the NIC.  After ACK it broadcasts ARP for the
server and transfers the complete resolved chain through TFTP RRQ/DATA/ACK:
``config.txt`` and recursive includes, start/fixup, kernel, DTB, optional
cmdline/initramfs, ``overlay_map.dtb`` when present, and selected overlays.
EEPROM ``PXE_OPTION43`` defaults to ``Raspberry Pi Boot`` and accepts a
nonempty printable match string up to the DHCP option's 255-byte wire limit.
Ordinary address-bearing DHCP offers are unaffected.  A zero-address
ProxyDHCP offer is retained only when its Option 43 bytes contain the selected
string; mismatches are ignored without disturbing the main DHCP transaction.
The selected string is visible as ``boot-pxe-option43`` and migrates in
behavioral-boot VMState version 74.
GENET ``packet-drop-direction``, ``packet-drop-after``, and
``packet-drop-count`` properties inject bounded TX/RX loss at the shared NIC
boundary.  Progress counters survive reset and VMState v3 migration; tests
prove a lost DHCP DISCOVER, retry after migration, and a selected RX loss.
The client emits the Raspberry Pi RRQ option sequence ``tsize=0`` followed by
``blksize=1024``.  It accepts a strict RFC 2347 OACK, acknowledges block zero,
validates the reported size against role/corpus bounds, and uses a negotiated
8--1024-byte block size.  A direct DATA response selects classic 512-byte mode,
while server ERROR code 8 causes one option-free retry.  Malformed, duplicate,
unrequested, or out-of-range options receive ERROR code 8 and terminate that
attempt.  The client locks each server transfer ID, accepts ordered blocks,
re-ACKs duplicates, rolls the 16-bit wire block number for large files,
retries lost RRQs and mid-file ACKs on a deterministic 500 ms, 1 s, 2 s, then
4 s-capped exponential schedule inside the unchanged per-file deadline,
enforces each artifact's bound, and, when a corpus backend is present, requires
received size and SHA-256 to match it.  Without a corpus it grows the bounded
request queue from received config/includes and overlay-map responses; a TFTP
file-not-found ERROR skips optional config, cmdline, or overlay-map probes.  It applies
``NET_BOOT_MAX_RETRIES``, ``DHCP_TIMEOUT``, ``DHCP_REQ_TIMEOUT``, and
``TFTP_FILE_TIMEOUT`` with the documented defaults and minima, rechecks the
backend at each deadline, retransmits DISCOVER and REQUEST within the overall
DHCP deadline, restarts discovery with a fresh transaction ID after a valid
NAK, and
preserves active waits and the completed/partial artifact queue across
migration, including the remaining packet-retry delay and backoff state.
``NETCONSOLE`` is parsed independently of network BOOT_ORDER using the
documented exact separator grammar, 32-character limit, default ports,
broadcast destination, and zero destination MAC.  It gates post-EEPROM boot
until GENET reports link-up or ``DHCP_TIMEOUT`` expires, without starting DHCP
unless a network boot source is later selected.  Link changes are delivered
from the GENET net client so QMP ``set_link`` can release an active wait
immediately.  UDP packets use the configured source/destination tuple and
carry deterministic clean-room boot observations.  VMState v78 retains the
endpoint, counters, pending action, and nanosecond deadline across migration.
Server ERROR packets terminate a transfer immediately; file-not-found retains
its optional-file and firmware/prefix fallback semantics, while other errors
apply network retry/fallthrough policy without consuming the complete file
deadline.  A wrong transfer ID receives ERROR code 5.  The last DATA block
enters a 500 ms dally state that re-ACKs duplicate final DATA and restarts the
dally interval; VMState preserves both the final transfer identity and exact
remaining delay.
Received bytes directly drive the same configuration, overlay,
and ARM-handoff code as SD and USB.  ``TFTP_IP`` can replace the DHCP-selected
ARP/TFTP server without changing the leased client address and migrates with
the active request.  DHCP subnet-mask and router options select the Ethernet
next hop independently from the TFTP IP destination.  A complete EEPROM
``TFTP_IP``/``CLIENT_IP``/``SUBNET`` tuple skips DHCP; an off-subnet server is
sent through ``GATEWAY`` when configured, while an on-subnet server is ARPed
directly.  The static configuration, resolved next hop, and ARP deadline
migrate.  DHCP option 97 is generated from the configured 32-bit
prefix, board revision, least-significant four MAC bytes, and OTP serial;
``DHCP_OPTION97=0`` selects the legacy repeated-serial GUID.  The configured
prefix migrates with an active request.  Option 54 selects and validates the
DHCP server independently from the TFTP destination.  BOOTP ``siaddr`` selects
the next/TFTP server.  When ``siaddr`` is empty, option 66 accepts a
dotted-decimal address or a validated DNS host name; the first unicast option-6
resolver is queried for an A record with ARP/gateway selection, checksum
validation, bounded retry, and the original DHCP deadline.  The selected DHCP
server remains the legacy final fallback when option 66 is absent.
DHCPREQUEST transmits the selected option-54 identity, and ACK/NAK packets from
another server are ignored.  ACK also has to confirm the offered client
address.  A zero-``yiaddr`` proxy offer can supply or replace the TFTP server
during DISCOVER or the selected-server REQUEST wait without replacing either
the lease or DHCP server.  DHCP, proxy, TFTP, DNS-server, host-name, query,
retry, and ARP-purpose state migrates independently in VMState v21.  RFC 2132
option overload parses options from the fixed BOOTP
``file`` and/or ``sname`` regions with independent bounds.  Known scalar
options must have valid lengths and repeated values must agree; nested overload
markers and truncated regions reject the packet.  Text options accept the
RFC-required receiver compatibility for one trailing NUL while still rejecting
embedded NULs.  Option 67 is validated across primary and overloaded regions,
repeated values must agree, and the value is intentionally ignored because Pi
4/CM4 network boot fetches a firmware file set instead of one PXE boot image;
``config.txt`` remains the first RRQ.  The bounded
``network_cadence.py`` oracle normalizes classic Ethernet PCAP captures into
DHCP/ARP/DNS/TFTP events, requires the semantic sequence to agree, compares
each adjacent packet interval against absolute and relative tolerances, and
emits a versioned hash-pinned JSON report.  A pinned physical capture and
wire-level physical conformance run remain M9.  TFTP not-found responses implement
the same default
``start4.elf`` to legacy ``start.elf``/``fixup.dat`` fallback as FAT media.
An incomplete ``os_prefix`` kernel/DT pair discards prefixed base data and
restarts that set from the root; this decision migrates in flight.
EEPROM ``TFTP_PREFIX`` mode 0 derives ``%08x/`` from OTP serial row 28, mode 1
uses the exact printable ``TFTP_PREFIX_STR`` up to 32 characters, and mode 2
derives the lower-case hyphenated GENET MAC.  Artifact state keeps a logical
filename separate from the prefixed wire RRQ, so recursive includes and
overlays resolve exactly as they do without a device directory.  After both
prefixed modern and legacy start probes fail, the device prefix alone is
cleared and the selected start artifact is retried at the server root.
``MAC_ADDRESS`` and ``MAC_ADDRESS_OTP`` are applied before prefix selection.
The latter accepts two distinct customer-row indices and reproduces the
published Raspberry Pi row-composition example.  One effective MAC feeds the
GENET NIC, firmware mailbox, DHCP/ARP/TFTP traffic, and final Device Tree.
The same mailbox identity is installed on direct and behavioral reset and is
checked together with persistent revision and serial across live migration.
VMState version 57 preserves the address and its configured, explicit, or OTP
source.

``contrib/raspi4/network_boot_gate.py`` is the real-server boundary for this
path.  It creates a TAP device and launches dnsmasq DHCP/TFTP inside an
unprivileged user/network namespace, then starts behavioral QEMU with the
unchanged EEPROM on a temporary block snapshot.  An explicit manifest-v2
``eeprom_update`` plus ``--allow-eeprom-update`` instead opens the exact
512 KiB EEPROM persistently, performs the real TFTP update/reset/second-boot
sequence, and requires its post-handoff bytes to equal the sealed
``pieeprom.upd`` hash.  Either opt-in without the other fails before QEMU
starts.  The gate accepts the same ``.bin`` and complete TFTP file tree used
on hardware, requires network ARM handoff through QMP, checks the fetched
firmware digest against the source file, and proves every non-target input is
unchanged afterward.  Update mode retains normalized evidence for two ordered
``pieeprom.upd``/``pieeprom.sig`` request pairs, the first subsequent firmware
request, and the ordered restart/reset/up-to-date/handoff trace.  Internal TAP
capture is mandatory for every gate even when no PCAP artifact is requested.
Each QMP logical firmware/fixup/kernel/DT/cmdline/initramfs observation is
resolved to exactly one requested path in the sealed TFTP tree.  This makes
root, serial-prefix, configured-prefix, and MAC-prefix deployments equivalent
without weakening hash or ambiguity checks.
Manifest v2 also seals ``expected_result`` for update attempts.  ``success``
requires a valid sibling signature and the exact persistent transition.
``invalid-signature`` requires a deliberately mismatched signature, while
``write-protected`` requires a valid signature and enables modeled EEPROM
write protection.  These expected failures still require
``--allow-eeprom-update``; the gate proves one ordered update/signature wire
pair, the matching failure trace and QOM status, no later TFTP request, and
an unchanged persistent EEPROM hash.  A rejection without wire transfer or
with any backend mutation fails the production gate.
``program-failure`` additionally requires a bounded ``fail_after`` offset.
The gate injects a program-stage interruption and independently derives the
only accepted persistent result: the update prefix through that exact byte,
followed by erased ``0xff`` bytes to the 512 KiB boundary.  The failure trace,
QOM status, single wire pair, absence of later requests, and exact partial
image are all mandatory.
``network_boot_recovery_gate.py`` composes that interruption with a clean
second process when the programmed prefix still contains a valid
network-booting EEPROM.  It uses the same persistent EEPROM and unchanged TFTP
corpus, seals the observed partial EEPROM into a success manifest, reruns the
normal production gate, requires installation of the original update digest
and ARM handoff, and retains hash-bound manifests, reports, and PCAPs for both
phases in one atomic campaign report.  It intentionally does not claim that
an erased or unbootable prefix can use TFTP; those cutoffs require the SD
``recovery.bin`` path.
``sd_recovery_manifest.py`` and ``sd_recovery_gate.py`` make that path an
independent production boundary.  The manifest pins the exact initial
512 KiB EEPROM, raw recovery SD image, unchanged ``recovery.bin`` and
``pieeprom.upd``, firmware, fixup, kernel, machine, and RAM model.  The gate
opens the same EEPROM and SD files persistently, independently derives the
only accepted FAT mutation (the short-name entry ``RECOVERY.BIN`` becomes
``RECOVERY.000``), and requires the final EEPROM to equal ``pieeprom.upd``
byte for byte.  It then proves the automatic reset and ARM handoff from that
same SD image, binds the QMP firmware/fixup/kernel observations to the
manifest, verifies ordered recovery/program/reset/handoff trace evidence, and
re-hashes every non-media input.  The atomic report retains the manifest,
pre/post media identities, exact rename offset, QMP observations, and QEMU
trace-log hash.  This gate uses the same unconverted ``.bin`` inputs as the
hardware flow; analog power timing and silicon-root cryptography remain HIL
boundaries.
``network_boot_manifest.py`` first seals the exact configured EEPROM, complete
TFTP file set, machine, RAM model, and firmware name.  The gate refuses any
missing, additional, or changed input and rechecks the manifest itself after
handoff.  Its atomic JSON report is suitable for CI evidence.  The opt-in
functional gate additionally requires the immutable official start/fixup,
kernel, DTB, and initramfs hashes.  A production release must run that gate and
compare it with the physical network-boot trace.  The functional wrapper
always enables bounded in-process AF_PACKET capture, independently verifies
the atomic classic PCAP and its hash/count/size evidence, and accepts a
physical PCAP plus client MAC as an optional pair.  Supplying that pair runs
the cadence oracle in the same gate; semantic or timing drift fails closed
while retaining the detailed comparison report.

The Pi 4B SD slot can likewise start empty with ``-drive if=sd,id=sdcard``.
QMP ``blockdev-change-medium`` and ``eject`` events drive the active
SD-card-detect or infinite-retry state through the same backend attached to
the emulated SD controller.  A bootable insertion is consumed unchanged and
can reach ARM handoff immediately; the pending detect state also survives
migration.  Ejecting during an active controller transfer cancels the transfer
timer or partial PIO transaction, clears data-active and buffer-ready state,
and reports both card removal and an SDHCI data timeout when enabled.  A qtest
migrates a live 320-byte partial write before ejection, proves that no
incomplete 512-byte sector reached the image, observes the shared interrupt
route, and then re-inserts, reads, and resets the same medium.  No artificial
SD delay is claimed until hardware timing has been captured.  CM4 eMMC user,
boot, and RPMB areas are fixed media and reject QMP ejection/change-medium
operations.

The legacy BCM2835 SDHOST block remains available at its BCM2711 address for
software that selects that controller instead of EMMC2.  Its guest-visible
power, clock-divider, timeout, command, response, status, and FIFO registers
retain their programmed values, apply the documented masks, and migrate.
Command completion without a selected card reports command timeout; FIFO
underflow or overflow reports the FIFO error; both status bits use
write-one-to-clear semantics.  System reset clears the programmable registers,
responses, status, FIFO contents, and IRQ.  A qtest exercises those errors,
clears them, migrates live programmed state, and then proves the reset
boundary.  This is the digital register/ownership contract used by the
production boot path; GPIO electrical mux behavior and board timing remain
Pass 2 hardware comparisons.

CM4 uses the same unchanged EEPROM/firmware files but binds soldered storage
as a named persistent backend.  The host RPIBOOT/mass-storage helpers export
this same regular file, and QEMU reopens it only after the gadget is stopped::

  qemu-system-aarch64 \
      -M raspi-cm4,boot-mode=behavioral,eeprom-drive=pieeprom,emmc-drive=emmc \
      -drive if=none,id=pieeprom,format=raw,file=pieeprom.bin \
      -drive if=none,id=emmc,format=raw,file=cm4-emmc.img

The backend is attached to EMMC2 as QEMU's ``emmc`` device, not as an SD-card
surrogate.  Linux therefore discovers MMC CID/CSD and EXT_CSD, derives device
capacity from ``SEC_COUNT``, and uses MMC multi-block commands.  Generic SDHCI
Auto CMD23 support issues the block-count command from Argument 2 before
CMD18/CMD25; this is required for the MMC card to leave its data state cleanly
without stale buffer-ready interrupts.  QEMU's eMMC model also supplies reset
and migration state.  Optional ``emmc-boot-drive`` and ``emmc-rpmb-drive``
backends make boot0/boot1 and RPMB persistent while preserving the exact
host-flashed user image at offset zero in ``emmc-drive``.  The boot backend
contains two equal halves and the RPMB backend is independent; their validated
file sizes drive the EXT_CSD size fields.  The production CM4 functional gate
requires Linux to enumerate both boot block devices and RPMB while booting the
unchanged user image.  A qtest migrates this three-backend configuration to a
second CM4 process and verifies the boot state and hidden-area geometry after
load.  The gate also prevents RPMB and EXT_CSD from sharing a VMState
subsection identity.  Generic QEMU eMMC callers retain the legacy combined
boot0/boot1/RPMB/user layout when separate backends are not supplied.
Board-accurate SKU identity and hidden-area defaults remain physical-corpus
inputs.  Authenticated RPMB and secure trim/sanitize are optional eMMC
security extensions outside the production flashing/boot contract, and are
not claimed by this model.  The ``emmc-cid`` machine property provides a
hardware-oracle boundary in the meantime: it accepts either 15 payload bytes
or Linux's 16-byte sysfs form, validates or reconstructs CRC7, survives reset
and migration, and is asserted through Linux CID and product-name sysfs nodes.

``emmc-cache-size=N`` configures a 512-byte-aligned cache up to 16 MiB and
publishes its KiB size in EXT_CSD ``CACHE_SIZE``.  Once the guest enables
EXT_CSD ``CACHE_CTRL``, complete non-RPMB sectors enter a write-back cache and
remain visible to guest reads without modifying the backend.  CMD6
``FLUSH_CACHE`` writes and flushes the selected user/boot backend; filling the
bounded cache performs the same durable writeback before accepting another
sector.  Dirty entries, their partition identity, and bytes migrate in the
``sd-card/emmc-cache-state`` subsection.  The destination requires the same
configured cache size.

``emmc-cache-power-loss-on-reset=on`` deliberately treats system reset as card
power loss and drops dirty entries.  With it off, reset flushes dirty entries
to preserve the pre-cache compatibility contract.  The eMMC child's read-only
``cache-dirty-sectors`` property exposes the current volatile boundary.
Qtests prove EXT_CSD advertisement, guest read-after-write over old host bytes,
dirty migration, loss on reset, explicit flush durability, and exact
capacity-pressure persistence.  These controls do not claim calibrated cache
timing, NAND translation, wear, or electrical power fidelity.

``emmc-cache-flush-sector-delay-us=N`` makes an explicit CMD6
``FLUSH_CACHE`` asynchronous when dirty entries exist.  The card remains in
the MMC programming state and makes exactly one 512-byte cache entry durable
at each virtual deadline.  Read-only ``cache-flush-active`` and
``cache-flush-completed-sectors`` properties expose transaction progress.
The cache VMState subsection version 2 migrates the configured delay, current
deadline, timer, completed count, dirty entries, and active state.  Reset
cancels the timer before applying ``emmc-cache-power-loss-on-reset``: already
completed sectors remain in the exact backend and the dirty tail is discarded.
Qtests cover every cut offset in a four-sector flush and assert the exact
durable-prefix/volatile-suffix split.  A one-shot blkdebug failure in the timer
callback also proves that progress stops without dropping dirty data, reports
R1 ``ERROR``, and resumes from the same first sector on an identical retry.
Zero delay keeps the synchronous compatibility path.  The timing is an
operator-supplied deterministic test input until calibrated against HIL.

``emmc-program-sector-delay-us=N`` independently models card programming for
complete uncached 512-byte sectors.  Instead of modifying the backend as soon
as EMMC2 submits a sector, the card queues it, remains in the MMC programming
state after the bounded transfer, and writes plus flushes one sector at each
virtual deadline.  Reliable sectors use the same queue after bypassing the
volatile cache, so Auto CMD23 bit 31 retains its FUA distinction.
``program-active``, ``program-pending-sectors``, and
``program-completed-sectors`` expose the durability boundary.  The
``sd-card/emmc-program-state`` migration subsection carries the pending sector
bytes and partition identities, configured delay, deadline, timer, and exact
progress.  Reset cancels the timer and discards only pending sectors; completed
sectors are already durable.  A backend error leaves the queue intact and sets
R1 ``ERROR`` so an identical transfer retries the same sectors.  Qtests cover
all five cut points for four-sector normal and reliable writes, active
migration, and injected flush failure/retry.  Zero delay preserves synchronous
programming.  Nonzero timing remains a deterministic input, not a calibrated
NAND or electrical claim.

The mixed durability campaign runs 64 consecutive transactions against one
card/backend.  It rotates volatile-cache FLUSH_CACHE, ordinary uncached CMD25,
and Auto CMD23 reliable CMD25; rotates all five cut positions across each
four-sector transaction; resets one microsecond before every unfinished next
deadline; and performs seven live migrations while a timer is active at that
deadline-minus-one boundary.  After every reset it reinitializes EMMC2 and
checks the exact durable prefix plus untouched suffix in a distinct media
window.  This is the bounded CI reset/migration stress gate.  It does not
replace extended physical power cycling.

The eMMC advertises a 512 KiB high-capacity erase group and
``ERASED_MEM_CONT=1``.  CMD35/CMD36/CMD38 erase bypasses the volatile
write cache and invalidates only overlapping dirty entries, preventing a
later cache flush from resurrecting pre-erase data.  With the default
``emmc-erase-group-delay-us=0``, complete groups are written as ``0xff`` and
flushed synchronously.  A nonzero value leaves the card in programming state
and makes one group durable at each virtual deadline.
``erase-active``, ``erase-pending-groups``, and
``erase-completed-groups`` expose progress.  The
``sd-card/emmc-erase-state`` migration subsection carries the byte range,
selected partition, configured delay, deadline, timer, and completed count.
Reset cancels the operation without rolling back completed groups.  Backend
failure retains the current group for an exact retry and reports R1 ``ERROR``.
Qtests cover all five cuts in a four-group erase, overlapping/nonoverlapping
cache entries, active migration, synchronous compatibility, and injected
flush failure/retry.  Secure erase, secure trim, sanitize, NAND translation,
and calibrated group timing remain outside this digital model.

The card also advertises enhanced reliable-write support in EXT_CSD
``WR_REL_PARAM``.  SDHCI Auto CMD23 already sources its argument from Argument
2; bit 31 now remains attached to the bounded CMD25 transfer as a migratable
card state.  A reliable sector bypasses the volatile cache and flushes the
selected backend before the card completes it.  It does not implicitly commit
unrelated normal cached sectors.  The qtest migrates after one reliable sector
and 320 bytes of the next, completes the remaining bytes on the destination,
then applies cache-loss reset and proves only the unrelated cached sector is
lost.  This is the guest-controller FUA/reliable-write durability boundary,
not a claim about analog hold-up time.
A blkdebug ``flush_to_disk`` fault additionally proves that backend flush
failure appears in the next R1 response as ``ERROR``.  The unrelated normal
cache entry remains volatile, and an exact retry after the one-shot fault
flushes successfully and survives reset.

For reproducible guest-driver failure tests, ``raspi-cm4`` also accepts
``emmc-data-error=timeout`` or ``emmc-data-error=crc`` together with an
aligned ``emmc-data-error-after=N`` byte boundary and a nonzero
``emmc-data-error-count=N``.  The counter covers successful card-side data
bytes (including EXT_CSD traffic), so zero faults the first data transaction.
Injection stops an active PIO, SDMA, or ADMA transfer and raises the standard
SDHCI data-timeout or data-CRC status and interrupt.  Byte and occurrence
progress migrate with the controller; controller reset does not re-arm a
consumed one-shot fault.  Standard Linux gates observe errors -110 and -84,
retry initialization, enumerate the unchanged eMMC, and reach userspace.
This is a block-boundary protocol fault, not an electrical or sub-sector power
cut model.

The modeled EMMC2 controller also has an explicit sector-commit power boundary:
bytes staged in its PIO FIFO do not reach the backing image until all 512 bytes
of a sector have arrived.  A qtest migrates a live write after 320 bytes,
applies a system reset, and proves the image remains unchanged.  It then
completes exactly one sector, resets again, and proves that sector is durable
while the adjacent sector remains untouched.  This makes controller-side
interruption deterministic.  The optional cache model above adds card-side
acknowledged-versus-durable behavior; neither boundary claims NAND translation
or electrical power-loss fidelity.

Host-visible CM4 provisioning boundary
--------------------------------------

The host-visible USB boundary cannot be represented by an ordinary QEMU USB
peripheral: those peripherals are visible to an emulated USB host, while the
official ``rpiboot`` program searches the physical host's libusb device list.
The Linux reference path therefore uses ``dummy_hcd`` plus Raw Gadget.

``contrib/raspi4/rpiboot_raw_gadget.c`` implements the BCM2711 device side of
the public ``rpiboot`` framing.  Its first enumeration has VID/PID
``0a5c:2711`` and accepts the unchanged 24-byte boot message followed by the
unchanged ``bootcode4.bin`` bytes over endpoint 1.  After the four-byte status
reply it disconnects and re-enumerates with a serial descriptor, then sends
the 260-byte get-size/read/done file requests expected by the unmodified host
program.  Received bytes are captured for exact comparison.  This is a
behavioral ROM/firmware boundary: it does not execute VideoCore instructions,
and physical descriptor, error, reset, and timing conformance remains an M6
oracle requirement.

The Linux build also produces ``qemu-rpi-dwc2-raw-gadget-proxy``.  Unlike the
standalone protocol fixture, this is a thin packet bridge: Raw Gadget
lifecycle, EP0 SETUP/IN/OUT, and endpoint-1 OUT tokens cross the versioned
``device-chardev`` framing while the behavioral ROM inside QEMU supplies every
descriptor, status, filename, and byte of protocol state.  Its
announced-length barrier prevents a later control request from overtaking bulk
data being forwarded by the second thread.  The opt-in
``test_arm_cm4_inprocess_rpiboot_usb`` gate runs unchanged official
``rpiboot`` commit ``87d6e032`` through this path and verifies QEMU's exact
sizes and SHA-256 values for ``bootcode4.bin``, ``config.txt``, and
``boot.img`` after both enumerations reach ``rpiboot-complete``.
The ROM control endpoint enforces USB standard-request direction: GET
requests received as OUT and SET requests received as IN stall the matching
EP0 direction.  A malformed SET_ADDRESS cannot change ``DCFG.DEVADDR``.
Device reset clears the halt and the same connection can enumerate and finish
RPIBOOT normally.  It also validates the recipient, value, index, and length
for every supported standard request.  ``SET_ADDRESS`` takes effect only
after its status-IN stage.  ``SET_CONFIGURATION(0|1)`` drives the value
returned by ``GET_CONFIGURATION``; interface access requires configuration,
endpoint-1 halt can be set, queried, and cleared, and device status reports
the descriptor's self-powered bit.  USB reset/disconnect returns the device
to configuration zero.  The configuration and pending status-stage action
migrate in behavioral-boot VMState v59.  Configuration one is accepted only
after a nonzero address.  Vendor RPIBOOT requests cannot arm endpoint 1 while
unconfigured.  Reconfiguration disables endpoint 1, clears its DMA/register
state through ``dwc2_device_firmware_disable_endpoint()``, and rolls any
partial bootcode or file transfer back to its complete stage boundary.
Consequently every ROM and file-server retry must repeat SET_ADDRESS and
SET_CONFIGURATION before sending the unchanged artifact bytes.
The first-stage transition is fail-closed: the machine receives the SHA-256
of the exact unchanged artifact as ``rpiboot-bootcode-trusted-sha256``.
QEMU hashes the bytes received through DWC2, returns a nonzero ROM status on
missing or mismatched trust, and never enters the second-stage file server.
``rpiboot-bootcode-trust`` reports ``not-checked``, ``missing``, ``mismatch``,
or ``trusted``.  This reproducible artifact oracle models the software-visible
mask-ROM acceptance boundary; it does not claim the non-public BCM2711 silicon
key or signature implementation.
With ``--reset-after 4096``, the proxy stops inside each stage while remaining
enumerated.  The privileged reset gate invokes the host's real ``usbreset``
tool, tags all worker traffic by reset generation, rolls QEMU back to the ROM
or file-server stage entry, and requires unchanged ``rpiboot`` to re-enumerate
and reproduce all final hashes.  ``--reset-count 4`` repeats the exact
boundary four times in each enumeration without replacing the proxy or QEMU,
so the gate observes eight reset generations before completion.  Omitting the
count retains the one-reset default.  A qtest independently covers the same
rollback after partial bootcode and boot-image DMA.
``--disconnect-after 4096 --disconnect-stage rom|file-server`` closes Raw
Gadget at the corresponding exact byte boundary and exits with intentional
fault status 75.  The DWC2 chardev close becomes a device disconnect, QEMU
rolls back the partial stage, and a clean full or ``--second-stage-only`` proxy
reconnect completes against the same VM.  The privileged disconnect gate
proves both stages and verifies every final artifact hash.
``--hold-after 4096 --hold-stage rom|file-server`` instead waits after the
exact prefix has crossed DWC2.  The gate observes
``rpiboot-transfer-received=4096``, SIGKILLs the complete proxy process group,
requires QEMU to roll the active count back to zero on socket closure, and
then completes the corresponding clean retry against that same VM.
With ``provision-state-file``, QEMU itself retains the lifecycle lock while
the in-process ROM is live.  A proxy connection changes
``qemu-rpiboot-wait`` to ``rpiboot-active``; unexpected closure publishes
``rpiboot-failed``; reconnect claims active ownership again; and only the
final file-server acknowledgement publishes ``rpiboot-complete``.  The gate
requires this sequence in both enumerations and verifies that normal QEMU
shutdown does not overwrite the completed state.
``--timeout rom-status|file-request`` withholds the selected EP0 reply for
20,001 ms in the gate, just beyond unchanged official ``rpiboot``'s 20-second
deadline.  Both host operations time out, the proxy exits with status 75,
QEMU rolls back on chardev closure, and a clean proxy completes against the
same VM with exact hashes.  ``--bulk-timeout-after 4096
--bulk-timeout-stage rom|file-server --bulk-timeout-ms 5001`` caps the Raw
Gadget read at the exact byte boundary and withholds completion beyond
``rpiboot``'s 5-second bulk deadline.  Both enumerations intentionally fail
and then complete exact clean retries against the same VM.

The bridge can deterministically close the real Raw Gadget transport after an
exact byte count in ``bootcode4.bin`` or a selected second-stage file, or stall
the ROM status endpoint.  Fault exit status 75 distinguishes an intentional
cut from an implementation error, and the exact partial capture is retained.
The ``rom-status-timeout`` and ``file-request-timeout`` faults withhold a
control-IN reply for 21,000 ms by default, exceeding the pinned official
``rpiboot`` 20,000 ms read timeout.  Values at or below that host boundary are
rejected.  Once the interval completes, the helper publishes
``rpiboot-failed``, disconnects, and exits with status 75.  A privileged gate
exercises both enumerations, verifies exact pre-fault captures, and then
completes a byte-identical clean retry with the same unchanged host tool.
The corresponding ``bootcode-hold`` and ``file-hold`` boundaries durably
capture the selected prefix and wait without changing ``rpiboot-active``.
The privileged SIGKILL gate terminates the real helper at 4,096 bytes in each
enumeration, proves unsafe retry is rejected, explicitly recovers only to
``rpiboot-failed``, then completes a clean byte-identical transfer.
Bulk endpoint reads execute on a synchronized worker while the main thread
continues fetching EP0/reset events.  Every reset increments a generation;
stale bulk completion can therefore never advance the ROM or file-server
state.  Normal enumeration resets preserve firmware transaction state, while
an active interrupted transaction rolls back to its stage entry.  The
``reset-reenumerate`` fault waits at 4,096 bytes in both bootcode and selected
file transfers.  Its bounded ``--fault-count`` repeats each boundary without
restarting the helper.  A second privileged gate issues four real host
``usbreset`` operations in each stage and requires eight re-enumerations,
clean retries, and byte-identical final captures in the same helper process.

``contrib/raspi4/cm4_mass_storage.py`` implements the next externally visible
stage with Linux configfs.  It binds the same regular-file eMMC backend as a
standard USB mass-storage LUN and prints one stable ``/dev/disk/by-id`` target
for Imager or ``dd``.  It rejects non-regular or non-sector-aligned images and
refuses teardown while the disk or a partition is mounted.  QEMU and the
gadget are mutually exclusive owners of this backend.  A modeled power cycle
is therefore:

1. start Raw Gadget and run the unchanged official ``rpiboot``;
2. export ``cm4-emmc.img`` with the foreground mass-storage ``serve`` owner
   and flash it using the unchanged production image;
3. after the imaging tool verifies and flushes its writes, acknowledge
   ``complete`` so the owner releases the kernel LUN;
4. start ``raspi-cm4`` with that exact file as ``emmc-drive``.

The opt-in ``provision-state-file`` machine property makes this external
sequence fail closed.  On nRPIBOOT, QEMU publishes
``qemu-rpiboot-wait`` and its process-exit notifier publishes
``rpiboot-host-ready``.  Raw Gadget accepts only that released state (or a
retry after ``rpiboot-failed``), publishes ``rpiboot-active`` while it owns
both enumerations, then publishes ``rpiboot-complete``.  An intentional fault
publishes ``rpiboot-failed``.  If the process dies uncleanly,
``--lifecycle FILE --recover-stale`` first wins the sibling lock and changes
only ``rpiboot-active`` (or an idempotent failed retry) to
``rpiboot-failed``.  It cannot advance the flow or disturb a live owner.  The
production supervisor invokes this recovery after stopping a failed Raw
Gadget child.  The
configfs helper permits export only from completed/retryable states and
publishes ``mass-storage-active``, ``flash-failed``, or ``boot-ready``.  A
post-flash QEMU process refuses to initialize eMMC unless the state is
``boot-ready`` or ``qemu-stopped``, writes ``qemu-owned`` before using it, and
publishes ``qemu-stopped`` through the same exit path.  Abrupt death leaves a
stale owner instead of silently allowing concurrent access.  A replacement
QEMU remains fail closed by default.  Explicit
``provision-recover-stale=on`` permits only an unlocked stale
``qemu-owned`` state to transition through ``qemu-stopped`` and back to the
new process's ``qemu-owned`` state.  It cannot promote ``flash-failed``,
``mass-storage-active``, RPIBOOT, unknown, or unreadable states.  The sibling
lock is acquired before recovery, so a live owner still wins.  Read-only
``provision-recovery`` reports ``stale-qemu-owned`` when this policy was used.
The exit notifier tracks states published by its own process; a QEMU that
rejects startup cannot release or rewrite somebody else's stale token.

A sibling advisory lock serializes the processes as well as their state
transitions.  QEMU holds it from machine initialization through process exit;
Raw Gadget holds it across both USB enumerations.  Production configfs
``serve`` mode holds it from LUN creation through the complete host write,
verification, flush, explicit result acknowledgement, and teardown interval.
EOF, invalid input, or termination publishes ``flash-failed`` after teardown.
An uncatchable foreground-owner crash may leave both
``mass-storage-active`` and a kernel gadget.  The explicit
``cm4_mass_storage.py ... recover-stale`` command obtains the sibling lock,
refuses a live owner or mounted export, removes any orphaned gadget, and moves
only to ``flash-failed``.  It also closes the crash window where teardown
finished but publication did not.  Cleanup failure retains the active token;
the recovery command can never create ``boot-ready``.
The exact gate proves a competing owner is rejected while this foreground
process is active.  Split configfs commands remain available for diagnostics
and fault injection.  Root-run helpers preserve the original state-file UID,
GID, and mode during atomic replacement, so the following normal-user QEMU
process can reacquire ownership.

The helper's explicit ``fault-disconnect`` operation arms before imaging,
resolves that same stable device, snapshots its kernel block-write counter,
and unbinds the USB gadget after a declared number of additional sectors.
This produces a real host block-device removal during I/O while retaining the
partial regular-file eMMC backend for failure and recovery assertions.
The sibling ``fault-eject`` operation watches the same counter but writes the
configfs LUN's ``forced_eject`` control.  USB enumeration and the stable block
identity remain while subsequent SCSI reads fail with ``EIO``.  Official
Imager rejects both failure classes, partial media remains, and explicit
failed teardown is required before retry.

``fault-flush-eject`` instead snapshots Linux block-stat field 16 and waits
for a declared number of completed host flush requests before forcing the
same medium removal.  The privileged gate issues real writes followed by
``fsync``, observes one completed flush, retains the stable USB identity, and
requires the next read to fail with ``EIO``.  This is a deterministic
post-flush loss boundary, not a claim that a completed flush was rolled back.

Configfs does not expose individual SCSI CDBs, so
``contrib/raspi4/cm4_msd_raw_gadget.c`` provides a separate command-visible
USB Mass Storage Bulk-Only Transport target.  The Linux build emits
``qemu-rpi-cm4-msd`` and registers its protocol-engine self-test with Meson.
It also builds ``qemu-rpi-rpiboot-raw-gadget`` and registers an unprivileged
RPIBOOT lifecycle self-test that covers active-to-failed recovery and live
lock-owner rejection.
It maps capacity, READ/WRITE(10/16), FUA durability, SYNCHRONIZE CACHE(10/16),
INQUIRY/VPD, sense, caching mode pages, verification, and LUN discovery onto
the same guarded regular-file eMMC backend.  The advertised volatile cache
and DPOFUA capability cause these durability operations to remain visible at
the device boundary.

Fault selection accepts ``synchronize-cache``, ``write-fua``, ``bot-phase``,
``bot-timeout``, ``bot-data-reset``, ``bot-write-reset``, or an arbitrary
``opcode:0xNN`` plus a matching-command threshold.  Matching command faults
return CHECK CONDITION with ``Medium Error / Write error``; they do not
disconnect USB or poison nonmatching reads.  The helper
also accepts a bounded ``--fault-count`` for BOT transport faults, defaulting
to one so existing one-shot invocations retain their behavior.  Data-reset
faults additionally accept ``--fault-bytes`` from 1 through 131,072, with a
backward-compatible 512-byte default.  The helper
participates in the shared lifecycle lock, publishes ``mass-storage-active``
before enumeration, and atomically changes it to ``flash-failed`` on the
first injected command failure.  The privileged gate proves exact CACHE SYNC
and FUA WRITE CDB rejection through Linux ``usb-storage``/``sg_raw``, rejects
a concurrent configfs owner, and then proves ordinary reads still work.  The
pinned official Imager separately hits the same failed cache-sync path and
reports its storage-flush error while the USB identity remains present.

The BOT bulk engine runs independently of EP0.  ``bot-phase`` returns phase
status for a no-data TEST UNIT READY command and waits for transport recovery.
Linux ``usb-storage`` first performs a USB port reset, so the target records
the reset, waits for SET_CONFIGURATION, and resumes the bulk loop only after
the host has reconfigured it.  The class-specific Mass Storage Reset fallback
is also serviced concurrently and clears both endpoint halts; the gate issues
it eight times back-to-back through ``sg_reset --device --no-escalate`` with
no intervening I/O.  The same gate requires the stable by-id disk to return
and proves SCSI INQUIRY plus a block read after the burst.

``bot-timeout`` instead consumes a TEST UNIT READY CBW without returning a
CSW.  The independently serviced EP0 remains available throughout the host's
real command-timeout interval.  Linux then performs its port-reset and
reconfiguration path; the gate again requires the stable identity, INQUIRY,
and block reads to recover before proceeding.

``bot-data-reset`` targets READ(16) after a configurable prefix has crossed
the bulk IN data phase.  ``--fault-bytes=N`` selects 1 through 131,072 bytes
and defaults to 512 for compatibility.  The host gate repeats four 4 KiB
commands with a 512-byte cutoff at distinct
LBAs on the same target, waits for each exact boundary, and issues ``sg_reset``
while each command is still pending.  The reset path marks the old BOT command
internally aborted, so it emits neither remaining payload nor a stale CSW
after recovery.  Every old host command must fail, whereas new INQUIRY and
read commands must succeed between cycles.

``bot-write-reset`` targets the corresponding WRITE(16) data phase.  It
accepts exactly the configured ``--fault-bytes`` prefix, then writes and
calls ``fdatasync()`` on that prefix, and waits for the active-command reset
without consuming the remaining data or emitting the old CSW.  Four cycles use a
128-byte sub-sector cutoff in distinct 4 KiB backend regions.  The host gate
requires each exact prefix to be durable, each following 3,968-byte suffix to
remain untouched, every old command to fail, and new SCSI commands to recover
on the same identity between cycles.  The real Raw Gadget gate passes all
four cycles.  Because 128 bytes is smaller than one high-speed 512-byte bulk
packet, the target receives the complete host transfer unit into its bounded
buffer while retaining and syncing only the configured prefix.  The
unprivileged protocol self-test independently
persists a 127-byte prefix at a nonzero offset and proves every surrounding
byte remains unchanged.

When libusb is present, Meson also builds ``qemu-rpi-cm4-bot-probe``.  It is
an independent host-side BOT conformance driver rather than a target
self-test.  It first executes the complete USB-IF thirteen-case matrix for
host/device data direction and length using TEST UNIT READY, INQUIRY, and
non-mutating VERIFY payloads.  It checks short data, Bulk-In halts, CSW
residue, passed/phase-error status, and mandatory Reset Recovery over the real
bulk endpoints.  It then locks down a deterministic failed-CSW response for
five valid-but-meaningless CBWs: reserved flags, unsupported LUN, zero and
oversized CDB lengths, and a supported opcode with the wrong CDB length.  The
USB-IF specification deliberately leaves that device response unspecified,
so the test claims safety and repeatability rather than wire conformance.  It
then sends 128 fixed-seed randomized CBW/SCSI cases with arbitrary tags,
unsupported opcodes, randomized CDB bytes, every legal CDB length, and both
legal direction-bit values.  The data length remains zero so this corpus is
provably non-mutating.  Its FNV-64 digest is locked by the compiled self-test;
the real host probe requires a failed CSW with the exact tag for every case,
interleaves a clean command every sixteen cases, and the functional gate
compares the eMMC SHA-256 before and after the corpus.  It
then sends invalid-signature and short-length CBWs,
requires both endpoints to stall, and performs the USB-IF Reset Recovery
sequence in its specified order: Mass Storage Reset, clear Bulk-In halt, then
clear Bulk-Out halt.  New TEST UNIT READY and INQUIRY transactions must pass
through libusb, after which ``usb-storage`` is reattached and kernel INQUIRY
plus block I/O must also pass.  The target no longer terminates on these
invalid CBWs; its bulk worker remains blocked until the class reset and
tolerates the endpoint-clear interval before accepting a new CBW.

The Raw Gadget target can now own a successful production flash.  With
``--foreground-owner`` it accepts only an exact ``complete`` or ``failed``
line on stdin, prevents a new BOT command from starting, waits for an active
command to finish, durably flushes the eMMC backend, and publishes
``boot-ready`` only for the successful path.  The supervisor selects this path
with ``--mass-storage-mode raw-bot`` and discovers the stable Linux block
target from the descriptor's fixed serial.  Configfs remains the default
compatibility backend.  Data-bearing and coverage-guided fuzzing,
long-duration and concurrent reset storms, descriptor/timing matching,
non-Linux hosts, and hardware conformance remain open.

Unprivileged Pi 4B release gates separately consume the exact decompressed
2,977,955,840-byte 2026-06-18 Raspberry Pi OS Lite image.  They verify the
pinned SHA-256 before copying those bytes to virtual SD or VL805/xHCI
USB-MSD, boot through the unchanged official EEPROM without direct-loader
arguments or host extraction, assert the release-selected
``vc4-kms-v3d-pi4.dtbo``, and wait for the
release kernel to emit through ``serial0``/``ttyS0``, mount its real root
filesystem, and start its serial getty through the ``raspberrypi login:``
prompt.  The release's own first-boot userspace must also durably replace the
MBR disk identifier, expand partition 2, and leave the VM alive afterward.
The USB variant first falls through absent SD using unchanged BOOT_ORDER
``0xf41``, requires the guest to enumerate the same USB-MSD backend used by
behavioral firmware, and performs those durable rootfs writes through the
architectural xHCI path.  A separate
unchanged-artifact production gate reaches an interactive initramfs shell
through the same UART.

An unprivileged CM4 gate writes the same verified raw payload as the unchanged
prefix of a 4 GiB virtual eMMC, boots it through the unchanged EEPROM and
``bcm2711-rpi-cm4.dtb``, expands partition 2 and ext4 to the full capacity,
and requires the CM4 ``serial0`` path to reach the same login prompt.  The
privileged RPIBOOT/Imager gate now makes that UART/login condition part of its
continuous post-flash phase as well.

``contrib/raspi4/cm4_provision.py`` is the first-class host supervisor for the
clean path.  One invocation verifies a strict
``qemu-rpi-cm4-provision-v1`` SHA-256 manifest, copies the immutable official
EEPROM into a byte-identical private writable backend, starts the nRPIBOOT QEMU
phase, asserts QMP and lifecycle observations, runs unchanged official
``rpiboot``, verifies every captured RPIBOOT file, transfers the lock to the
selected foreground configfs or command-visible Raw BOT owner, runs unchanged
official Imager with its raw-payload
digest, flushes the host block device, acknowledges completion, and starts the
post-flash QEMU phase on those exact bytes.  It also asserts the requested RAM
SKU and derived CM4 revision, then writes an atomic JSON event/hash report.
The EEPROM manifest record separately pins the exact ``bootsys`` section
SHA-256, its ROM key index, and the ordered dependency-set SHA-256.  The
supervisor validates the BCM2711 length/key-index/RSA/HMAC envelope and key
index, safely decompresses every LZ4
``bootmain``/MCB/memory/display dependency, recomputes its hash, requires that
hash in the signed payload, and supplies the independent root digest to both
QEMU phases.  Failures do not advance ownership.  Twenty-eight supervisor unit
tests pass.  A refreshed privileged 4 GiB run with the pinned official corpus
proves the unchanged USB/Imager path under the extended trust contract: the
report records the independently pinned ``bootsys`` digest, key index one, all
thirteen rooted dependencies, their ordered-set digest, exact RPIBOOT
captures, Imager write/verify/flush, and post-flash ARM handoff.  An
already-loaded ``dummy_hcd``/``raw_gadget``/``libcomposite`` stack is accepted
only after its required parameters are checked; a missing module is still
loaded through bounded ``modprobe`` and must appear in sysfs.  The supervisor
emits a compact machine-readable result which explicitly keeps electrical, PHY
timing, analog, RF, and silicon-root proof outside this logical host result.

The current bridge has passed a real host-kernel test with the official
``rpiboot`` source at commit ``87d6e03272b0ae155d85a125bfdec03e3d4a1095``:
the 105,984-byte official ``bootcode4.bin``, ``config.txt``, and 29,360,640-byte
official ``boot.img`` mass-storage bundle were received byte-for-byte across
both enumerations.  The mass-storage stage also accepted a 64 MiB boot image
through its host block device and the resulting backend compared byte-for-byte.
VMState v59 preserves the active ROM phase, partial boot/file captures,
standard-control state, and
file-server position together with the DWC2 endpoint state.  A qtest migrates
after exactly half of a bootcode payload, resumes it through the destination
process's DWC2 socket, and verifies the final hash and ROM status.

The remaining M6 work is physical-oracle validation of the boot-file request
sequence, concurrent/long-duration reset campaigns through the thin proxy,
FUA-through-Imager faults, exact hardware descriptor and timing traces, and a
physical CM4 oracle.

An opt-in privileged functional gate now joins the available behavioral
boundaries in one run.  It first observes ``rpiboot-wait`` with the dedicated
CM4 nRPIBOOT input asserted, verifies QEMU's lifecycle release while powering
it off, runs
the pinned official host tool and Raw Gadget phases, exports and flashes that
backend through its stable USB block-device identity while a foreground owner
retains the lock, explicitly releases it after verification and flush, and then
boots the exact resulting bytes to Linux userspace after a modeled power
cycle.  It asserts every lifecycle transition and proves QEMU rejects a
``flash-failed`` eMMC before accepting ``boot-ready``.  It also rejects any
official RPIBOOT bundle file whose SHA-256 differs from the pinned corpus.
With the optional pinned Imager input, the gate runs
the official v2.0.8 CLI-only AppImage unchanged, leaves its verification
enabled, resolves the guarded by-id target to the exact device Imager
enumerated, proves a concurrent helper is rejected, writes without its
system-drive override, observes success, and
boots the exact result.  This proves the full externally orchestrated Imager
path and the supervisor repeats its clean handoff automatically; neither yet
proves an in-platform USB device-mode transition,
remaining transport faults, or other Imager host platforms.  With the pinned
``QEMU_RPI_RELEASE_IMAGE`` input, the same gate writes the unchanged official
2026-06-18 Raspberry Pi OS Lite arm64 ``.img.xz`` to a 4 GiB eMMC, checks both
archive and decompressed hashes, and boots it unchanged.  Before that clean
path, it proves official ``rpiboot`` rejects a disconnect after 4,096 bytes
and official Imager rejects a 64 MiB LUN, a live disconnect after at least
8,192 issued sectors, and forced SCSI medium removal at the same threshold.
The latter leaves the USB device enumerated while subsequent block reads fail
with ``EIO``; all operations then recover on fresh device enumerations.  Its
first-boot
initramfs rewrites ``PARTUUID``, expands partition 2, mounts root read/write,
grows ext4 to 915,456 4 KiB blocks, and starts the unchanged release's serial
getty through ``raspberrypi login:``.  Behavioral handoff preserves the
firmware-generated DT boot arguments before the media command line and
resolves ``serial0`` and ``serial1`` from the final DT to Linux UART device
names.  Only the first LF- or CRLF-terminated ``cmdline.txt`` line reaches
``/chosen/bootargs``; later bytes remain part of the exact size/hash evidence
but do not become kernel parameters.  A single trailing NUL is accepted and
an embedded NUL fails the handoff.  Preserving ``8250.nr_uarts=1`` fixes
official-kernel mini-UART
registration.  The AUX model now supports the 8250 DLAB divisor contract and
its control/status/reset registers, and a standard production gate reaches the
initramfs shell through ``ttyS0``.  The exact release now uses that console for
its unchanged kernel log and reaches systemd after mounting the release root
filesystem.

Behavioral mode rejects ``-kernel``, ``-dtb``, ``-initrd``, and ``-bios`` so
it cannot silently fall back to direct loading.  All ARM cores remain powered
off until artifact validation and the firmware-to-ARM handoff release them.
On every system reset the first stage reads OTP, validates the row-17 bootmode
copy and row-30 board identity, samples the ``nrpiboot`` machine input, checks
the primary SD FAT volume for ``recovery.bin`` before EEPROM, and then reads
the persistent EEPROM section table and ``bootconf.txt`` when recovery does
not take ownership.  CM4 instead samples its dedicated nRPIBOOT input, never
executes ``recovery.bin`` from eMMC, and falls back to RPIBOOT if EEPROM is
unavailable.  It exposes these QOM observations on ``/machine``:

``boot-state``
  Recovery outcomes, ``recovery-required``, ``rpiboot-wait``,
  ``eeprom-invalid``, ``arm-handoff-ready``, ``boot-source-unsupported``,
  ``boot-order-exhausted``, ``sd-card-detect-wait``, ``sd-retry-loop``,
  ``emmc-retry-loop``, ``restart-loop``, ``restart-cycle-wait``,
  ``restart-watchdog-pending``, or ``stopped``.
``boot-source``
  The current or terminal ``BOOT_ORDER`` source, or ``none``.
``boot-order``
  The decoded 32-bit order, with the documented ``0xf41`` default.
``boot-order-index``, ``boot-attempt-count``
  The least-significant-first nibble index and number of media attempts
  represented by the current observation.
``boot-retry-count``, ``boot-restart-count``
  Failed SD retries and completed ``RESTART`` encounters in the current
  bootloader reset cycle.
``boot-max-restarts``, ``boot-sd-max-retries``
  The parsed ``MAX_RESTARTS`` and ``SD_BOOT_MAX_RETRIES`` values; ``-1`` means
  infinite.
``boot-usb-power-off-time-ms``, ``boot-usb-startup-delay-ms``, ``boot-usb-discover-timeout-ms``, ``boot-usb-lun-timeout-ms``
  The parsed ``USB_MSD_PWR_OFF_TIME``, ``USB_MSD_STARTUP_DELAY``,
  ``USB_MSD_DISCOVER_TIMEOUT``, and ``USB_MSD_LUN_TIMEOUT`` values.  Defaults
  are 1,000, 0, 20,000, and 2,000 milliseconds.  Power-off and startup delay
  are bounded to 5,000 and 30,000 milliseconds; discovery and LUN values below
  the documented 5,000 and 100 millisecond minima fail closed.
``boot-usb-power-enabled``, ``boot-usb-power-off-applicable``, ``boot-usb-power-cycle-mode``
  The modeled Pi 4B USB rail state and revision-derived policy.  PCB revisions
  through 1.3 report ``legacy-short-then-configurable``; revision 1.4 and newer
  report ``reset-held-with-memory-init-overlap``.  CM4 reports
  ``not-applicable`` and does not delay enumeration for this Pi 4B-only
  setting.
``boot-usb-power-off-elapsed-ms``, ``boot-usb-power-off-remaining-ns``
  The accounted reset-to-power-on interval and exact remaining active timer.
  Newer Pi 4B revisions begin with the documented minimum 2,000 ms overlap
  from memory initialization.  A configured value above that lower bound and
  the legacy configurable interval use the normal migratable virtual timer.
  ``USB_MSD_STARTUP_DELAY`` remains a separate timer and does not consume the
  discovery budget.
``usb-boot-selected-index``, ``usb-boot-selected-device``, ``usb-boot-selected-lun``
  The flattened candidate index and its modeled USB device/LUN coordinates.
  Each is 255 until one bootable LUN has been selected.  The selected index is
  migrated; device and LUN are derived from the unchanged machine topology.
``usb-msd-exclude-vid-pid``
  The active EEPROM ``USB_MSD_EXCLUDE_VID_PID`` policy in canonical
  comma-separated lower-case hexadecimal form.
``usb-boot-excluded-device-count``, ``usb-boot-eligible-device-count``, ``usb-boot-last-excluded-vid-pid``
  Descriptor-level results from the latest enumeration attempt.  The last
  value is ``0xffffffff`` when no device was excluded.  Policy and observations
  migrate with behavioral VMState v55.
``boot-elapsed-ms``
  Virtual time consumed by asynchronous source failures.  A missing USB
  backend enters ``usb-discovery-wait`` and a present but unbootable backend
  enters ``usb-lun-wait``.  At the configured deadline QEMU rechecks the
  backend, allowing media that became bootable during the wait to succeed;
  otherwise execution resumes at the next least-significant-first
  ``BOOT_ORDER`` nibble.  Successful media present at the initial attempt has
  no artificial delay.  These are behavioral scheduling boundaries, not
  physical USB enumeration timing claims.
``firmware-status``
  The exact manifest outcome, including ``config-ready``, missing artifact,
  invalid configuration, unreadable media, or artifact read failure.
  Overlay failures also emit the exact parser or merge diagnostic while the
  machine exposes the stable failure class through ``arm-handoff-status``.
``firmware-file``, ``firmware-size``, ``firmware-fixup-*``, ``firmware-kernel-*``, ``firmware-device-tree-*``, ``firmware-cmdline-*``, ``firmware-initramfs-*``
  Selected paths and byte counts.  Firmware, kernel, and DTB SHA-256
  observations are also exposed after complete FAT-chain reads.
``firmware-config-present``, ``firmware-arm-64bit``, ``firmware-os-prefix``, ``firmware-config-lines``, ``firmware-config-includes``, ``firmware-config-ignored``, ``firmware-overlay-count``, ``firmware-dtparam-count``
  Configuration decisions and parsing counters at the behavioral boundary.
``firmware-overlay-applied``, ``firmware-dtparam-applied``, ``firmware-overlay-file``, ``firmware-final-device-tree-sha256``
  Successfully merged overlay/parameter counts, the final overlay path, and
  the hash of the post-overlay, post-firmware-mutation Device Tree.
``arm-handoff-status``, ``arm-handoff-architecture``, ``arm-handoff-kernel-address``, ``arm-handoff-kernel-size``, ``arm-handoff-entry-address``, ``arm-handoff-device-tree-address``, ``arm-handoff-device-tree-size``, ``arm-handoff-initramfs-address``, ``arm-handoff-core-mask``
  Final architecture, load/entry layout, validation result, and released-core
  mask.  ARM64 core 0 enters at the Image address in EL2 with ``x0`` pointing
  to the final DTB; secondaries wait in the Pi spin-table stub.  ARM32 loads
  ``kernel7l.img`` at ``0x8000`` and enters through ``0x0`` with ``r0=0``,
  ``r1=~0``, and ``r2`` pointing to the final DTB.  ARM32 secondaries wait on
  BCM2711 mailbox 3 using the local-peripheral clear base ``0xff8000cc``.
``recovery-status``
  Discovery, validation, programming, interruption, write-protection, rename,
  stop, or reboot-pending outcome for the most recent reset.
``recovery-trusted-sha256``, ``recovery-sha256``, ``recovery-key-index``
  The configured fail-closed trust oracle, observed exact-file digest, and
  public BCM2711 signing-key index for ``recovery.bin``.  The observed values
  migrate with an in-progress behavioral boot.
``otp-bootmode``, ``otp-board-revision``, ``otp-secure-boot``
  Public BCM2711 OTP policy sampled by the modeled ROM.
``board-revision``, ``memory-model``
  The effective board identity and installed RAM SKU.  ``board-revision`` may
  be supplied at machine construction to reproduce an exact new-style Pi 4B
  or CM4 revision, but QEMU rejects model, BCM2711 processor, or memory bits
  that conflict with the machine type and ``-m 1G|2G|4G|8G``.
``reset-status``, ``reset-cause``, ``boot-partition``
  Raw reset-safe ``PM_RSTS``, its decoded cause, and the partition encoded in
  alternating bits 0, 2, 4, 6, 8, and 10.

Without ``otp-drive``, QEMU creates volatile zeroed OTP rows and programs the
machine's board revision into row 30.  A backing image is persistent, must
contain the matching factory board revision, and retains the OTP device's
one-way OR behavior for mailbox programming.  Public BCM2711 secure-boot
state is recognized from row 17 bit 15.  In secure mode, rows 47-54 must equal
the SHA-256 of the exact 264-byte ``pubkey.bin`` stored in EEPROM.  The key is
the upstream Raspberry Pi representation: a 256-byte RSA modulus followed by
an eight-byte exponent, both little-endian.  QEMU then verifies the upstream
``bootconf.sig`` digest/timestamp/``rsa2048`` format over ``bootconf.txt``
using RSA-2048 PKCS#1 v1.5 SHA-256.
The EEPROM section parser requires exactly one instance of each
security-critical ``bootconf.txt``, ``bootconf.sig``, and ``pubkey.bin`` file.
Any duplicate fails before configuration, customer-key, or signature use, so
different consumers cannot select different signed inputs.

Recovery and CM4 USB RPIBOOT also consume the upstream
``program_pubkey=1`` request from unchanged ``config.txt``.  The RPIBOOT file
server then requests the generated ``pieeprom.bin`` used by the secure
recovery bundle.  Before mutation, QEMU parses that exact image, requires
``pubkey.bin`` and a valid customer signature over ``bootconf.txt``, requires
a persistent ``otp-drive``, and rejects a different existing key.  Only after
the EEPROM erase/program/read-back cycle succeeds does it reproduce the public
recovery UART order: burn secure mode into row 17, burn its row-18 copy, OR
the current production secure and development-key-revocation flags ``0x81``
into row 55, then store the key hash in rows 47-54.
Duplicate or malformed settings, signature failure, missing persistence, key
replacement, write protection, size mismatch, and unsupported
``program_jtag_lock=1`` fail closed.  JTAG locking is rejected because its
irreversible BCM2711 fuse encoding is not public.

``otp-provision-fail-after=N`` interrupts before row operation ``N`` of that
eleven-operation sequence.  ``otp-provision-rows-programmed`` exposes the
exact durable prefix and VMState v41 migrates it.  Qtests cover all cutoffs:
zero writes can retry cleanly, while later irreversible prefixes reset into
boot-mode-copy mismatch, missing-key, or key-hash mismatch as appropriate.
This is a row-level behavioral fault boundary; sub-row fuse physics and
programming timing remain HIL-only.

Each selected block medium must contain ``boot.img`` and ``boot.sig``.  The
same digest and RSA checks run before any inner byte is treated as firmware;
only the verified image is opened as a read-only in-memory FAT12/16/32 volume
and passed to normal configuration, overlay, kernel, DT, and ARM-handoff
processing.  ``secure-boot-status`` exposes config/image verification and
distinct missing, format, key-hash, content-hash, RSA, and inner-image errors.
Qtests authenticate a complete deterministic inner image through the normal
firmware, kernel, DT, and ARM-handoff path and repeat that path after reset.
VMState v28 retains the verified public key, network-install policy, and HTTP configuration,
transaction, sparse receive-window validity map, and FIN sequence across
asynchronous boot waits.
For network BOOT_ORDER, secure mode requests ``boot.sig`` followed by the
unchanged ``boot.img`` through the real GENET TFTP client and applies the same
verification and memory-FAT handoff.  Secure OTP mode first requires the exact
initial EEPROM section to have the public BCM2711
``payload-length/key-index/RSA-2048/HMAC-SHA1`` envelope and match
``bootsys-trusted-sha256``.  Missing trust, a malformed envelope, or a digest
mismatch fails before customer configuration is consumed.  Read-only
``bootsys-sha256`` reports the observed exact bytes and remains coherent
across reset and migration.  The model then requires unique ``bootmain``,
``mcb.bin``, and at least one ``memsys*.bin`` section; safely decodes bounded
independent or linked LZ4 frames; recomputes every executable/memory/display
dependency SHA-256; and requires each digest in the signed ``bootsys`` payload.
The LZ4 descriptor's xxHash32-derived header checksum is validated before
decoding, so a damaged frame cannot pass merely because its decoded-byte
digest would otherwise match.
``bootsys-dependency-count`` and ``bootsys-dependencies-sha256`` expose the
ordered result across reset and migration.  ``bootsys-key-index`` exposes the
signed-envelope selector and ``otp-secure-boot-flags`` exposes row 55.  When
the public development-key-revocation bit is programmed, key-index-zero
second stages fail before the release oracle or customer configuration is
consumed; key-index-one production images remain eligible.  Current recovery
also forces the production ``0x81`` flags whenever ``program_pubkey=1``, even
if an older configuration explicitly supplies ``revoke_devkey=0``.  The root
digest is an independently pinned release oracle: QEMU does not claim to
possess or execute the non-public Raspberry Pi BootROM keys/HMAC root.
BCM2711 has no modeled general firmware-version counter: the public contract
prevents fallback to old development-key second stages, while exact accepted
production releases remain a release-oracle choice.  Provisioning models this
policy plus row-boundary interruption, but does not claim electrical fuse
timing or sub-row power-loss equivalence.

BOOT_ORDER mode 7 supports a customer-key, custom-host HTTP subset over the
same GENET client.  EEPROM ``HTTP_HOST``, ``HTTP_PORT``, and ``HTTP_PATH`` are
validated before DHCP/DNS/ARP.  Separate TCP connections issue HTTP/1.1 GETs
for ``boot.sig`` and then ``boot.img``; responses require status 200 and one
bounded ``Content-Length`` and may not use transfer encoding.  The response
bodies are passed unchanged to the signed-image verifier.  A shared migratable
timer retransmits lost SYN/GET packets with deterministic backoff and sends a
duplicate ACK after a stalled partial response.  A bounded 64 KiB sparse
receive window retains multiple noncontiguous future ranges and preserves
first-arrival bytes across retransmitted overlap.  A qtest migrates after the
header and 32 signature bytes with two later ranges and FIN queued across two
gaps, verifies the remaining retry delay on the new network backend, and
completes secure handoff after cumulatively draining both ranges.
FIN after the exact declared body is acknowledged before handoff; premature
FIN fails immediately as a truncated response.  Each complete response is
closed with a client FIN before the next connection or handoff.  Redirects are
rejected explicitly so they cannot change the authenticated EEPROM URL policy.
Other non-200 or malformed responses cancel both transport deadlines and enter
BOOT_ORDER retry/fallback immediately.

EEPROM ``NET_INSTALL_ENABLED`` and ``NET_INSTALL_AT_POWER_ON`` accept only
``0`` or ``1``.  The latter controls cold-boot UI availability rather than
selecting mode 7 by itself, but value one overrides a conflicting disabled
setting.  Network Install defaults on for Pi 4B and off for CM4.  The writable
machine input
``net-install-requested`` models the boot-time user request; it invokes mode 7
once only when enabled, then resumes the unchanged configured ``BOOT_ORDER``
from its first nibble after failure.  Read-only
``boot-net-install-enabled`` and ``boot-net-install-at-power-on`` properties
expose the parsed policy.  Qtests prove that a disabled installer rejects the
request and that power-on visibility alone does not select network install.
EEPROM ``NET_INSTALL_KEYBOARD_WAIT`` accepts the complete unsigned 32-bit
millisecond range, defaults to 900, and treats zero as disabling the keyboard
scan.  ``net-install-keyboard-present`` exposes host keyboard attachment and
``net-install-shift-held`` exposes the Shift input.  When a keyboard is
present, boot waits for Shift until the exact configured virtual deadline;
pressing Shift during the window selects mode 7, while expiry resumes the
unchanged ``BOOT_ORDER``.  QOM exposes the configured delay, active wait, and
remaining nanoseconds.  Behavioral-boot VMState version 73 preserves the
input state and exact remaining deadline across migration.

With no ``HTTP_HOST``, mode 7 selects
``fw-download-alias1.raspberrypi.com:443``.  QEMU creates peer-verifying
client credentials in memory from the Raspberry Pi intermediate CA embedded
at the board trust boundary; it never inherits the host CA store and needs no
operator-supplied credentials object.  ``http-tls-creds`` remains an optional
test/lab override and must name a peer-verifying client ``tls-creds-x509``
object.  The TLS session runs over the same modeled TCP stream, emits segmented
encrypted records, sends the requested host as SNI, validates the certificate
and hostname, decrypts records
into the bounded HTTP parser, and emits close-notify before TCP close.
``boot.sig`` and ``boot.img`` are verified with the official network-install
public key embedded by the BCM2711 bootloader.  Anonymous, PSK, verify-off, and
invalid override credentials fail closed.  Custom ``HTTP_CACERT_HASH`` remains
rejected because that facility is BCM2712-only, and BCM2711 secure boot without
a custom ``HTTP_HOST`` disables HTTP as on hardware.  VMState v28 retains policy before TLS starts; migration
rejects an established session because traffic keys are not serialized.  A
qtest drops the first ClientHello, proves exact ciphertext/sequence replay at
500 ms, completes an X.509 server handshake for the official hostname, and
decrypts the exact first GET while a physical request overrides unchanged
``BOOT_ORDER=0xf21`` bytes.  It
then returns an encrypted HTTP response and proves close-notify, TCP FIN, and
the next artifact connection.  The hash-pinned official-corpus gate is needed
for a positive signature result because Raspberry's signing private key is not
available.  The opt-in ``QEMU_RPI_DEFAULT_HOST_LIVE=1`` functional test runs
that positive path with an unchanged pinned production EEPROM, QEMU user
networking, the built-in CA, and the live official service.  It requires ARM
handoff and compares the complete signed ``boot.img`` plus consumed
``start4.elf``, ``fixup4.dat``, kernel, DTB, and initramfs hashes with the
pinned corpus.  DNS replies are matched by transaction, destination, and
protocol fields so a router or slirp gateway may legitimately use a source MAC
different from the queried DNS pseudo-service.  TCP SACK, boot-to-userspace on
the installer image, and physical cadence remain explicit gaps.

Read-only ``secure-boot-image-size``, ``secure-boot-signature-size``,
``secure-boot-image-sha256``, and ``secure-boot-signature-sha256`` properties
record the exact outer signed-container boundary independently of the existing
inner firmware-artifact observations.

Hostname policy is exercised without weakening signed configuration.  A
dedicated qtest EEPROM contains an authenticated ``HTTP_HOST=boot.test``;
DHCP supplies the resolver, and the GENET client performs resolver ARP, an A
query, resolved-server ARP, and both signed HTTP downloads before handoff.

On Pi 4B, asserted ``nrpiboot`` only takes effect when
``otp-rpiboot-gpio`` selects one of the documented GPIOs 2, 4, 5, 6, 7, or 8.
The selector is a clean-room high-level OTP input because the physical fuse
encoding is not public.  On ``raspi-cm4``, the same machine input models the
dedicated active-low GPIO40/EMMC-DISABLE jumper directly, without OTP
selection.  ``PM_RSTS`` now survives warm reset; full watchdog reset
sets HADWRF, while power-on, watchdog-full, and software-full observations are
qtested.  Quick/hard/debug reset variants and firmware flag-clearing policy
still require oracle coverage.

The same PM watchdog recognizes Linux's partition-63 halt request and applies
the EEPROM ``WAKE_ON_GPIO``/``POWER_OFF_ON_HALT`` policy.  GPIO wake enters a
QEMU suspended state and a falling BCM2711 GPIO3 input, including the existing
socket/USB host bridge, wakes and resets the board.  Disabling GPIO wake
requires a falling ``global-en`` machine input unless PMIC power-off is also
enabled, in which case QEMU emits guest shutdown.  Read-only policy and
``halt-state`` observations distinguish the three outcomes.  Power-management
VMState version 4 preserves policy, input levels, halt state, and any active
watchdog deadline; physical PMIC rails and setup/hold timing remain HIL.

The SD reader operates on raw block bytes and supports a FAT12/16/32
superfloppy, a primary MBR FAT partition, or a GPT FAT partition.  GPT media
must have CRC-valid primary and backup headers, identical CRC-valid entry
arrays, consistent geometry, and a partition whose FAT volume fits its declared
bounds.  Firmware paths support short names, VFAT long names, and nested
directories; recovery filenames remain fixed root names.  A discovered
``recovery.bin`` is a behavioral trigger: QEMU
does not execute its VideoCore instructions.  Before accepting that trigger,
QEMU enforces the public BCM2711 signed-file layout: a payload followed by its
little-endian length, key index, 256-byte RSA field, and 20-byte HMAC field,
with the same 110 KiB maximum used by the upstream signing tool.  Empty
signature fields, invalid length or key index, truncation, and oversize input
fail closed.  Because the silicon HMAC key is not public, behavioral execution
also requires the exact unchanged file SHA-256 in
``recovery-trusted-sha256``; ``recovery-sha256`` and
``recovery-key-index`` report what was observed.  A missing or mismatched
oracle prevents every EEPROM write.  This structural-plus-pinned-digest
boundary is not claimed as silicon RSA/HMAC execution, which remains a
hardware-in-the-loop gate.  Cross-format qtests fragment the signed recovery,
exact 512 KiB update, and signature files across FAT12 and FAT32 cluster
chains, in addition to FAT16.  Self-looping chains and explicit reserved/bad
FAT12 and FAT32 links fail before EEPROM mutation; the cycle fixtures sign the
bytes an unchecked reader would synthesize so a digest mismatch cannot hide
missing cycle detection.  With ``pieeprom.upd`` and
``pieeprom.sig``, the model verifies the signature file's SHA-256 digest line,
erases in 4 KiB units, programs 256-byte pages, flushes persistent bytes, and
renames ``recovery.bin`` to ``RECOVERY.000``.  With ``pieeprom.bin`` it keeps
the recovery filename and enters the documented stop outcome.

Page programming follows NOR semantics rather than replacing bytes like RAM:
each stored byte becomes ``old & requested`` and therefore cannot change a
zero bit back to one without erase.  ``eeprom-program-page-count`` reports
completed page commands and ``eeprom-nor-violation-bits`` reports requested
zero-to-one transitions.  ``eeprom-stuck-zero-offset`` plus
``eeprom-stuck-zero-mask`` injects a deterministic erase-resistant cell fault.
The affected page is durably AND-programmed before
``recovery-nor-violation`` stops the update.  Disabling the fault and resetting
re-erases and reflashes the same unchanged update successfully.

The Pi 4B boot-filesystem self-update path parses ``ENABLE_SELF_UPDATE`` and
``FREEZE_VERSION`` from the unchanged EEPROM boot configuration.  It checks
the selected SD, USB-MSD, or NVMe FAT volume for ``pieeprom.upd`` and
``pieeprom.sig`` before reading ``config.txt``.  Exact-image no-op, invalid
signature, write protection, persistent replacement, ten-millisecond reboot,
post-reset no-op, and migration during the pending reboot are qtested.
The same FAT reader is used by every controller owner, so this check does not
bypass USB BOT or NVMe boot media access.  CM4 deliberately continues to use
the RPIBOOT provisioning path.

Real GENET/TFTP boot uses the same update engine.  Before ``config.txt`` (or
the secure ``boot.sig``/``boot.img`` pair), the client requests optional
``pieeprom.upd``.  A file-not-found response continues normal boot without
requesting a signature.  When the update exists, ``pieeprom.sig`` is required;
the exact received 512 KiB ``.bin`` and signature bytes are validated,
persisted, and rebooted before any firmware artifact is requested.  An
identical installed image continues with the normal TFTP request chain.
Packet-level coverage runs the complete update, reset, second identical-image
check, and ARM handoff on one EEPROM backend.  It also proves that a malformed
wire signature or write-protected EEPROM terminates the attempt without
changing any EEPROM byte.

Nonzero ``eeprom-erase-sector-delay-us``,
``eeprom-program-page-delay-us``, or
``eeprom-verify-sector-delay-us`` enables the asynchronous flash engine.
Every 4 KiB erase, 256-byte program page, and 4 KiB verification read receives
its configured virtual delay and updates ``eeprom-flash-elapsed-us``.  A reset
cancels the in-flight unit while preserving earlier completed bytes, then a
fresh ROM pass may retry from the unchanged SD files.  VMState v49 carries the
copied exact update, current stage, counters, configured delays, and remaining
deadline; changing timing while a transaction is active is rejected.
Zero-valued delays retain the synchronous compatibility path.  These values
are explicit deterministic inputs and do not become physical timing claims
until populated and checked by the HIL cadence oracle.

EEPROM protection has two independent inputs.  The persistent status-register
block-protect bit, exposed as ``eeprom-status-write-protect`` (and the
compatibility alias ``eeprom-write-protect``), rejects array erase/program
regardless of pin level.  ``eeprom-nwp`` exposes the active-high logical level
of the physical active-low ``EEPROM_nWP`` pin: pulling it low does not itself
protect array writes, but locks changes to the status-register protection.
Recovery consumes the unchanged ``config.txt``
``eeprom_write_protect=-1|0|1`` policy.  ``-1`` leaves protection unchanged,
``0`` clears it before flashing, and ``1`` sets it only after a fully verified
update; a low nWP pin rejects a requested status transition before EEPROM
mutation.  ``eeprom-nwp-sampled`` reports the level used by the current
transaction.
To retain the status register across independent QEMU processes, attach a
dedicated raw 512-byte block sector.  Byte zero is the block-protect bit,
byte one is the applied-update-timestamp-valid bit, bytes two through five
are the little-endian 32-bit timestamp, and the remaining bytes are zero.
The original byte-zero-plus-zeros format is a valid timestamp-absent sector::

  qemu-system-aarch64 \
      -M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,eeprom-status-drive=pieeprom-status \
      -drive if=none,id=pieeprom,format=raw,file=pieeprom.bin \
      -drive if=none,id=pieeprom-status,format=raw,file=pieeprom-status.bin

The backend is authoritative at launch, is flushed at each permitted status
transition or successfully applied update, and therefore overrides conflicting
property defaults on relaunch.  Recovery and boot-filesystem self-update accept the
upstream ``rpi-eeprom-digest`` ``ts: <epoch>`` line only when it is bounded and
well formed.  They commit it after EEPROM verification, expose it through QOM,
and publish it at ``/chosen/bootloader/update_timestamp``.  Once valid state
exists, self-update rejects missing or non-increasing timestamps as stale.
Failed, protected, malformed, stale, and up-to-date paths preserve the prior
value.

Select a power failure boundary with
``eeprom-fail-stage=erase|program|verify|verify-mismatch|rename|reboot``.
For erase, program, or verify, ``eeprom-fail-after=N`` stops after exactly
``N`` bytes in that stage.  ``verify-mismatch`` flips one modeled readback bit
at byte ``N`` without changing the persistent EEPROM and reports
``recovery-verify-failed``.  Actual backend read/write errors report
``recovery-program-error`` while retaining the bytes durably completed before
the failed operation.  Rename fails after verified EEPROM bytes are durable but before the FAT
directory entry changes.  Reboot fails after ``recovery.bin`` has become
``RECOVERY.000``; the next reset therefore boots the updated EEPROM without
repeating recovery.  Clearing a byte-count program fault and issuing a system
reset retries from the still-present recovery file.  Recovery outcomes migrate
in VMState v49 together with the observed recovery identity, protection
status, live and sampled nWP levels, page/NOR
telemetry, latest flash
stage, durable erased/programmed
byte counts, successfully matched verification bytes, and number of 4 KiB
sectors touched.  These values are exposed as read-only
``eeprom-flash-stage``, ``eeprom-erased-bytes``,
``eeprom-programmed-bytes``, ``eeprom-verified-bytes``, and
``eeprom-dirty-sector-count`` machine properties.  A mismatch reports the
exact successfully verified prefix rather than the surrounding read chunk.
An incoming destination suppresses speculative SD recovery while shared block
nodes are inactive.  Qtests migrate a verify mismatch with its progress,
clear it, and complete the retry; separate blkdebug cases prove recovery after
real read and write errors.  A successful ``.upd``
reports ``recovery-updated-reboot`` and schedules a guest reset after 10 ms of
virtual time.  The renamed recovery file is skipped on that reset, so the ROM
reads the newly programmed EEPROM and continues its ``BOOT_ORDER`` without
host intervention.  The production gate performs the whole transition on one
VM and the same SD image before requiring Linux userspace.  Qtests can inspect
the pre-reset status before advancing the virtual-time boundary.  The 10 ms
value is a deterministic behavioral scheduling boundary, not a claim about
physical EEPROM recovery timing.
Silicon execution of the BootROM ``bootsys`` RSA/HMAC root, opaque non-LZ4
firmware formats such as VL805 payload internals, LED/HDMI status, flash
timing, per-bit NOR programming behavior, and execution of arbitrary recovery
programs remain unmodeled.  The public BCM2711 rollback boundary is modeled:
ROM key indices zero through four are accepted when the complete independently
pinned image validates, ``revoke_devkey`` rejects development key zero, and
indices above four fail closed.  BCM2711 does not expose a general
customer-controlled firmware-version counter.  The behavioral root requires
the independently pinned exact ``bootsys`` digest and its verified LZ4
dependency chain described above.

Raw FAT file traversal follows noncontiguous cluster chains and rejects a
cluster as soon as it repeats.  This prevents a cyclic FAT from satisfying a
declared file length by replaying earlier sectors.  Recovery qtests program a
complete 512 KiB update from an interleaved FAT16 chain and reject a self-loop
whose repeated payload has an otherwise matching update signature.

The EEPROM state machine reads ``BOOT_ORDER`` least-significant nibble first
and supports up to eight modes.  Its bootloader configuration parser applies
``[all]`` and ``[pi4]`` sections to both platform variants and applies
``[cm4]`` only to ``raspi-cm4``, preserving ordered override semantics from
the same exact EEPROM bytes.  ``[partition=N]`` evaluates the reset-selected
six-bit PM_RSTS partition, while ``[gpioN=0|1]`` evaluates the explicitly
driven BCM2711 input sampled at reset.  Raw serial filters read OTP row 28,
and ``[board-type=N]`` compares the product-type field from the new-style
revision.  The complete documented equality, mask, masked-equality, less-than,
and greater-than expression grammar evaluates ``partition``,
``boot_partition``, and persistent ``cust_otp0``...``cust_otp7`` values;
variables unavailable on BCM2711 remain inactive.  Same-type filters replace;
model, identity, GPIO, and expression
types combine until ``[all]`` resets them, preventing a matching GPIO from
accidentally re-enabling a nonmatching CM4 section on Pi 4B.  High-impedance,
unknown, and nonmatching filters remain inactive.  Qtests prove cross-platform
overrides, OTP serial and board-type selection, combined-type behavior, plus
live partition, GPIO, mask/range, and customer-OTP changes across reset.  SD success requires complete
readable,
non-empty firmware/fixup/kernel/DTB artifacts on the raw FAT volume, plus any
explicitly configured initramfs.  ``initramfs`` and ``ramfsfile`` accept up
to eight comma-separated filenames.  QEMU applies ``os_prefix`` to each name,
requires every file to be present and non-empty, and concatenates their exact
bytes in list order into one bounded blob before calculating the published
size/hash and DT initrd range.  The same contract drives raw block media and
response-discovered TFTP, including migration between individual requests.
The default Pi 4 pair is
``start4.elf``/``fixup4.dat`` with a compatibility fallback to
``start.elf``/``fixup.dat``.  The default 64-bit kernel is ``kernel8.img``;
``arm_64bit=0`` selects ``kernel7l.img``.
The upstream Linux reboot-notifier ABI is modeled as well: mailbox
``SET_REBOOT_FLAGS`` bit 0 selects ``tryboot.txt`` for one boot,
``GET_REBOOT_FLAGS`` reports the pending value, and reset consumes and clears
the flag before firmware handoff.  A pending flag and an active tryboot
selection migrate; the following reset returns to ``config.txt``.  Secure
block and network boot select ``tryboot.img``/``tryboot.sig`` and apply the
implicit ``tryboot_a_b=1`` rule inside the verified ramdisk.  The EEPROM
``BOOTVAR0`` value is exposed to ``config.txt`` as ``bootvar0`` with the
documented equality, mask, masked-equality, and range operators.
Raw primary MBR, bounded extended/logical MBR, and CRC-validated GPT media
support explicit numbered FAT partitions.  Standard MBR numbering is retained:
primary slots are 1--4 and EBR logical partitions begin at 5.  EBR traversal is
bounded to 128 entries and rejects cycles, malformed links, multiple extended
containers, and any EBR or partition extent outside its container or media.
EEPROM ``PARTITION`` and ``PARTITION_WALK`` policy, plus the
512-byte ``autoboot.txt`` ``boot_partition`` and ``tryboot_a_b`` settings,
select an A/B partition before firmware loading.  Only ``[all]``, ``[none]``,
and ``[tryboot]`` sections are active in that file.  A reboot partition in
PM_RSTS has priority, an unsuccessful walk checks at most eight partition
numbers without recursively processing autoboot files,
``PARTITION_WALK`` defaults enabled for the pinned current release and can be
disabled explicitly with zero, and QOM distinguishes
the requested ``boot-partition`` from the actual
``selected-boot-partition``.  The final DT publishes the raw reset status,
selected partition, and active tryboot flag as big-endian cells at
``/chosen/bootloader/pm_rsts``, ``/chosen/bootloader/partition``, and
``/chosen/bootloader/tryboot``.  The adjacent ``boot-mode`` cell is the
successful ``BOOT_ORDER`` nibble, synchronized only after source fallback has
resolved.  The boot ROM extracts bounded ``BUILD_TIMESTAMP`` and hexadecimal
``VERSION`` records directly from the unchanged EEPROM bytes and publishes
them as ``build_timestamp`` and ``version``; invalid or absent records remove
stale input properties.  Evidence-bounded release identity also supplies
``capabilities=0x1f`` for the observed 2020-12-11 build and ``0x7f`` from the
observed 2021-07-06 build through the pinned 2026-05-17 production build.
Unbounded older/intermediate versions remove stale capability input instead of
being assigned QEMU's feature set.  The adjacent ``signed`` cell maps the public ABI:
bit zero is the strict EEPROM ``SIGNED_BOOT`` setting, bit two is persistent
development-key revocation, and bit three is customer-key-digest presence.
It does not expose the private physical OTP bit layout.  The adjacent
``update_timestamp`` cell is the last successfully applied EEPROM update's
``ts:`` value from the unchanged signature file; absent metadata removes
stale input.  Within
``config.txt``, ``partition`` is the
requested value and ``boot_partition`` is the selected value.
The parser enforces a 98-character line bound and treats a bounded relative
``include`` as textual insertion while retaining cycle detection.  Model
(``[pi4]``, ``[cm4]``, and ``[board-type=N]``), EDID, raw OTP serial,
explicitly driven ``[gpioN=0|1]``, and boot-variable expression filters
replace only their own category and combine with the other categories until
``[all]``.
``[none]`` remains inactive until that reset.  The supported BCM2711
expression inputs are ``bootvar0``, requested ``partition``, selected
``boot_partition``, and ``cust_otp0`` through ``cust_otp7``; unavailable,
malformed, high-impedance, and unknown filters stay fail-closed.  The same
board/OTP/GPIO/display inputs are populated for SD, eMMC, TFTP, and
HTTP-derived firmware configuration.  ``[tryboot]`` consumes the one-shot
reboot state.

``hdmi0-edid-file`` and ``hdmi1-edid-file`` point at raw EDID byte streams;
they can also point directly at Linux DRM ``edid`` sysfs files for host/HIL
use.  Behavioral reset samples each port once, validates the base header,
declared extension count, exact block length, and every 128-byte checksum,
then derives the documented ``MANUFACTURER-Display_Product_Name`` identity
from the vendor code and display-name descriptor.  An empty stream means
disconnected, while missing, malformed, checksum-invalid, or nameless data is
invalid and cannot satisfy ``[EDID=name]``.  On Pi 4, a filter matches either
port; sequential matching sections can therefore apply configuration for
both attached monitors.  The sampled names and status migrate so a transfer
cannot silently resample a different destination display midway through a
boot.  Read-only ``hdmiN-edid-name`` and ``hdmiN-edid-status`` properties
expose the boot-time decision; the next reset resamples the files.  The same
validated bytes back ``GET_EDID_BLOCK`` for HDMI0 and
``GET_EDID_BLOCK_DISPLAY`` for either port.  Responses echo the requested
block (and display for the latter) followed by 128 unchanged EDID bytes.
The legacy tag returns nonzero status and zero data when no block exists.
Both retain the complete 136-byte desired response length when clipped to a
short caller buffer.  Property VMState v10 migrates the exact sampled blocks.
The guest-visible HDMI DDC BSC controllers expose these same bytes through
the production 0x30 segment-pointer and 0x50 EDID addresses.  Validated CTA
HDMI Forum SCDC advertisement gates the standard 0x54 SCDC protocol.
The parser supports core firmware, kernel, DT,
cmdline, ``os_prefix``,
``overlay_prefix``, automatic initramfs selection, and ordered multi-file
initramfs concatenation.  It preserves ordered
``dtoverlay``/``dtparam`` scope, loads requested unchanged ``.dtbo`` files from
the raw FAT volume, applies BCM2711 name substitutions from
``overlay_map.dtb``, applies supported base and overlay ``__overrides__``, and
uses libfdt fixup/fragment merging.  Overlay fragment assignments to
``bootargs`` use the Pi firmware's append rule instead of libfdt's replacement
rule for target paths, direct phandles, and external symbol fixups; multiple
assignments accumulate in fragment order.  The supported parameter forms are
strings/status, 8/16/32/64-bit offsets, byte strings, normal and inverted
booleans, textual literal assignment, and conditional/unconditional fragment
selection.  Embedded binary-cell descriptors ending in ``=`` are deferred
until libfdt has resolved their local or external phandle fixups, then applied
to the merged tree.  Lookup tables support exact keys, key-as-value entries,
quoted values, defaults, unmatched-value pass-through, and local or external
binary-cell results.  Consecutive override targets and lookup continuations
follow dtc's packed, unaligned byte layout.  Missing keys without fallback,
malformed tables, and truncated cells fail closed.  Assigning offset zero of
``reg`` rewrites the target node's hexadecimal unit address, and assigning
the ``name`` pseudo-property renames the node without creating a property.
HAT EEPROM overlays use the same parser, fixup, parameter, and fragment path.
The read-only machine contract reports the HAT identity, selected embedded or
named overlay, GPIO-map policy, raw padded block-backend identity, and
EEPROM-declared logical-content identity independently.  Exported overlay
labels are private unless named in a zero-length ``__exports__`` property;
their rewritten merged-tree paths can be consumed by later overlays.  The
intra-overlay scheduler uses stable topological ordering for arbitrary-depth
fragment chains and rejects dependency cycles before libfdt mutation.
Physical GPIO startup semantics remain differential-conformance work.  After
validation, raw, gzip,
and EFI-zboot ARM64 Images require the complete 64-byte Image header and are
loaded according to its little-endian ``text_offset`` and ``image_size``
fields.  A nonzero ``text_offset`` below 4 KiB is relative to a 2 MiB-aligned
base; zero retains the firmware-compatible default placement at ``0x200000``.
Image-header flag bit 0 selects the matching little- or big-endian
``SCTLR_EL2.EE`` entry state for the primary and every secondary core;
``arm-handoff-endianness`` exposes the selected state.
Explicit, ``followkernel``, and automatic initramfs placement is checked
against the exact low-memory primary-entry stub, secondary spin table, and
architecture-specific spin-code ranges before any artifact is written.  A
collision fails the handoff rather than silently replacing initramfs bytes;
the first byte after an occupied range remains a valid placement boundary.
When no initramfs is selected, stale ``linux,initrd-start`` and
``linux,initrd-end`` properties from the input DTB are removed rather than
being exposed to Linux as a nonexistent ramdisk.
The same final layout pass implements ``device_tree_address`` as an exact
packed-DTB destination and ``device_tree_end`` as an exclusive bound for
explicit or automatic top-down placement.  Malformed unsigned values,
kernel/initramfs/firmware-state overlap, an undersized end bound, and the
low-RAM limit fail before DT bytes are installed.  Pi 4B SD and CM4 eMMC
qtests retain the exact destination and bytes across migration and reset.
The exact EEPROM ``bootconf.txt`` and valid-size ``pubkey.bin`` records are
also retained.  If the final DT contains the production
``raspberrypi,bootloader-config`` or
``raspberrypi,bootloader-public-key`` reserved-memory placeholder, behavioral
firmware places the corresponding bytes at a 64-byte-aligned address below
the effective ARM/VideoCore split, updates its ``reg`` tuple, and enables the
node.  Missing records and DTs without the placeholder remain unadvertised.
Kernel, initramfs, and DT placement treat the enabled ranges as firmware-owned
and fail on overlap.  Tests read the exact bytes back through the final DT on
Pi 4B and CM4 from the 128 MiB ``total_mem`` minimum through every supported
1/2/4/8 GiB model, then repeat the check across reset and live migration.
The same finalization pass creates ``/system`` when needed and overwrites
``linux,revision`` and the two-cell ``linux,serial`` from the selected machine
revision and OTP row 28.  It also replaces the root ``serial-number`` with
that serial formatted as 16 lower-case hexadecimal digits.  Qtests cover stale
input replacement across every Pi 4B/CM4 memory model, a nonzero persistent
serial, mailbox agreement, reset, and migration.
Firmware-owned ``/chosen/kaslr-seed`` is likewise replaced with a fresh
64-bit value from QEMU's guest-visible entropy source.  Entropy failure stops
the handoff rather than retaining untrusted media input.  Reset advances the
seed, live migration retains the handed-off value, and explicit QEMU ``-seed``
mode makes fresh test VMs reproducible.
The construction-only ``min-boot-version=N`` board attribute supplies the
big-endian ``/chosen/rpi-min-boot-ver`` value consumed by the unchanged
``rpi-eeprom-update`` workflow.  Zero represents an older board without a
programmed minimum and one represents the current BCM2711 manufacturing
release.  Finalization overwrites stale media input, reset preserves the
board attribute, and VMState v66 overrides conflicting destination launch
defaults.  The private physical OTP encoding is deliberately not invented.
The same finalization pass publishes the installed SDRAM capacity as the
big-endian ``/chosen/rpi-sdram-size-gbit`` value introduced by the Pi 4 EEPROM
firmware.  It is derived from the selected 1/2/4/8 GiB board model as
8/16/32/64 Gbit, independently of ``total_mem`` and the VideoCore reservation:
the property describes fitted SDRAM, not the amount exposed to the ARM.
Tests replace stale media input for every Pi 4B and CM4 model and retain the
installed value across reset and migration.
The adjacent ``/chosen/rpi-boardrev-ext`` property is likewise replaced from
the persistent OTP backend's row 33 in the documented big-endian 32-bit form.
It is intentionally independent of the normal board revision in OTP row 30.
An unprogrammed row reports zero; nonzero values survive reset and migration
through the same OTP and final-DT paths.
Finalization also replaces the documented ``/chosen/os_prefix`` and
``/chosen/overlay_prefix`` strings with the effective values selected by
``config.txt``.  This happens after ``os_prefix`` fallback validation, so the
tree reports the prefix actually used to locate the kernel, DT, command line,
and initramfs rather than an unusable input.  Empty strings remain explicit
zero-length prefixes, and stale media-supplied properties cannot survive.
Pi 4B SD and CM4 eMMC qtests cover the distinct ARM64 and ARM32 ranges,
including the ARM32 primary entry stub at ``0x0``, the shared spin table at
``0xd8``, and the architecture-specific spin-code end boundaries.
Raw or gzip ARM32 zImages are validated from their magic and extent and loaded
at ``0x8000``.  The initramfs and final DTB use
the Pi firmware's top-down low-memory layout; ``/chosen`` boot arguments and
initramfs bounds plus the board memory map are updated before all four cores
are released.  Portable ``serial0``/``serial1`` command-line values are
resolved against the final overlaid DT to the corresponding ``ttyS`` or
``ttyAMA`` console name.  This is a clean-room behavioral replacement and is
not VideoCore execution.

Memory-model conformance uses three qtests.  The first starts every Pi 4B/CM4
and 1/2/4/8 GiB combination, checks the derived revision and factory OTP rows,
and probes the highest guest RAM word.  The second boots the behavioral
firmware boundary at all four capacities and reads the final DT back from
guest RAM, validating the 64-bit upper-memory range through the 8 GiB model.
The third boots ARM32 raw media through Pi 4B SD and CM4 eMMC at every
capacity, checks the architecture-specific entry/load state and all four
cores, and validates the default 948 MiB low region plus exact upper range in each
board-specific final DT.
Unsupported capacities fail explicitly.  Peripheral DMA aliases, VideoCore
reservations, and physical memory-hole behavior remain differential/HIL work.

Failed SD attempts honor finite and infinite ``SD_BOOT_MAX_RETRIES``.  USB-MSD
and BCM-USB-MSD probe the configured devices and LUNs in topology order,
classify discovery versus present-LUN failures, account the configured
timeout, and fall through.
Backend insert/eject notifications also re-evaluate the active attempt without
polling: a bootable insertion completes immediately, an invalid insertion
enters the LUN wait, and removal returns that wait to discovery without
charging a timeout that never completed.
Network mode retries distinct DHCP and TFTP waits and then falls through.
NVMe mode 6 reads unchanged FAT media from ``nvme-drive=ID`` and exposes the
same backend through a PCIe NVMe endpoint before using the common firmware
handoff.  The secure path verifies the signed EEPROM configuration and signed
outer ``boot.img`` before exposing its inner FAT; positive and tampered mode-6
qtests cover that boundary.  A ``blkdebug`` read fault on this one shared
backend both makes behavioral mode 6 fall through and produces the real
``Unrecovered Read Error`` status in a queue-level NVMe READ completion.
Permanent and one-shot qtests prove repeated failure and exact one-command
consumption followed by a successful READ.  Queue-level WRITE faults likewise
return ``Write Fault``: a permanent fault leaves the complete host sector
unchanged, while a one-shot fault leaves the failed command non-durable and
persists the exact 512-byte payload from the following successful command.
After a real dirty WRITE, a one-shot FLUSH fault returns ``Write Fault`` and
the following FLUSH succeeds.  COMPARE returns ``Compare Failure`` with DNR
for a mismatched 512-byte buffer, succeeds for the matching buffer, and leaves
the namespace unchanged.
RPIBOOT and SD-card-detect
enter terminal waits, STOP stops, and RESTART either reports an infinite loop
or schedules the next complete BOOT_ORDER cycle.  When RESTART is encountered
more than the finite ``MAX_RESTARTS`` value, the bootloader boundary arms the
existing BCM power-management watchdog.  Its expiry sets PM_RSTS HADWRF,
requests an actual QEMU reset, and starts a fresh ROM/EEPROM cycle with
``reset-cause=watchdog``.  A 1 ms virtual boundary separates ordinary restart
cycles and a nominal 10 ms watchdog boundary separates the threshold from
reset; these deterministic values make ordering testable and are not physical
bootloader timing claims.  Infinite ``MAX_RESTARTS=-1`` remains represented as
the terminal ``restart-loop`` observation instead of intentionally consuming
unbounded host resources.

Behavioral boot state has a versioned migration section.  Version 69 adds the
firmware-visible secure-boot status field.  Version 68 adds the
EEPROM build timestamp validity/value and Git version string.  Version 67 adds the
selected successful boot mode used by the final firmware DT.  Version 56 adds the
configured TFTP-prefix mode and bytes plus the active root-fallback decision.
Version 20 adds the
independent proxy-DHCP TFTP server.  Version 19 separates
the selected DHCP server identifier from the next/TFTP server address.  Version 18 adds the
negotiated TFTP block size, option/classic mode, OACK retransmission state, and
reported transfer size.  Version 17 adds the
static client/subnet/gateway configuration, DHCP-derived route, and resolved
ARP next hop.  Version 16 adds the
final-block dally transfer ID and block number; the generic pending timer stores
its relative deadline.  Version 15 adds the
``DHCP_OPTION97`` prefix.  Version 14 adds the
validated ``TFTP_IP`` override.  Version 13 adds the
DHCP packet-retransmission count and remaining relative delay.  Version 12
adds the DHCP request-interval configuration.  Version 11 adds the
active TFTP packet-retransmission count, backoff state, and remaining relative
deadline.  Version 10 adds the
in-flight ``os_prefix`` fallback decision.  Version 9 adds the
response-discovered artifact roles, bounds, optional/missing state, and TFTP
paths so a corpus-free transfer can migrate without a local manifest.  Version 8 adds the
logical TFTP block number used across 16-bit wire rollover.  Version 7 adds the
ordered artifact queue, completed received buffers, and base/overlay queue
boundaries.  Version 6 added the ARP server MAC, TFTP server port, next block,
expected size, and allocated partial file buffer.  Version 5 added the
wire-DHCP phase, transaction ID,
offered address, and server address to the
version 4 network retry configuration, current attempt, and DHCP/TFTP wait,
the version 3 host-observed health milestone, and the
parsed BOOT_ORDER configuration, active nibble, attempt/retry/restart counters,
elapsed timeout accounting, and pending SD detect/retry, USB discovery/LUN,
network DHCP/TFTP, restart, or recovery-reboot action.  Timer state is represented as remaining
nanoseconds and re-armed relative to the destination virtual clock, avoiding
deadline extension when source and destination clock origins differ.  The BCM
power-management watchdog uses the same relative-deadline rule in VMState v3,
while retaining older stream compatibility.  Qtests migrate SD detect, USB
discovery/LUN, behavioral DHCP timeout, wire DHCP REQUEST, and a TFTP transfer
after completed config/start/fixup files and between kernel DATA blocks, plus
an RRQ loss, a mid-file ACK loss, migration of the next ACK retry,
restart-cycle, recovery-reboot, watchdog-pending, and health
states, verify stability one nanosecond before each timed deadline, and verify
the exact transition, including watchdog HADWRF RESET and a fresh ROM cycle.

The state machine emits ``raspi4b_boot_event`` QEMU trace records for reset,
ROM, recovery, EEPROM, source attempts, waits, retries, restart outcomes,
firmware/fixup/kernel/DT/initramfs sizes, overlay application, and
architecture-specific ARM handoff.
The ``from-qemu`` command in
``contrib/raspi4/boot_trace.py`` converts them into the versioned JSON Lines
contract while pinning the board and artifact hashes.  The explicit
``arm-handoff-ready`` observation distinguishes a validated, installed ARM
entry state from earlier manifest selection and from later kernel health.
The writable ``boot-health`` observation lets a release harness report only
milestones it has actually detected on the guest console.  It accepts the
monotonic ``kernel-started`` then ``userspace-ready`` sequence, or ``failed``,
only after successful behavioral handoff; invalid/skipped transitions fail.
Each accepted transition emits a ``health`` trace event, survives migration,
and resets to ``none`` on the next machine reset.  Standard production gates
set these milestones after their command-line and ``Boot successful.``
observations; exact-release gates do so after the serial login boundary.  This
is deliberately a host-observation bridge, not an invented claim that QEMU can
infer guest health internally.

Production-artifact acceptance
------------------------------

The functional suite pins immutable Raspberry Pi repository commits and
SHA-256 values for official BCM2711 ``pieeprom*.bin``, ``recovery.bin``,
``start4.elf``, and ``fixup4.dat`` files.  It combines them with the existing
pinned real compressed ``kernel8.img``, BCM2711 DTB, and compressed initramfs.
The ARM32 gates separately pin the current official ``kernel7l.img`` and both
Pi 4B and CM4 BCM2711 DTBs from the same immutable firmware commit, plus an
unchanged pinned ARMv7 initramfs.
Only the FAT container and its text configuration are generated; every binary
payload is copied byte for byte.

One gate boots that media to the initramfs ``Boot successful.`` health marker
without ``-kernel``, ``-dtb``, ``-initrd``, ``-append``, or host-extracted
arguments.  Pi 4B SD and CM4 eMMC gates boot unchanged ``kernel7l.img`` in
ARMv7/HYP mode, verify their distinct board/DT identities, bring all four CPUs
online through the BCM2711 mailbox protocol, reach ``Boot successful.`` in the
unchanged initramfs, and execute ``uname -m`` as ``armv7l``.  A further gate
places official
``recovery.bin``, an unchanged old
EEPROM, and an unchanged new ``pieeprom.upd`` on the modeled production path,
then verifies the persistent EEPROM result byte for byte.  These gates prove
artifact compatibility at the behavioral boundary; they do not imply that
closed VideoCore instructions are executed or replace physical conformance.

Boot trace contract
-------------------

``contrib/raspi4/boot-trace-schema-v1.json`` is the machine-readable JSON
Lines record contract.  ``contrib/raspi4/boot_trace.py`` validates captures,
normalizes only documented volatile fields, and compares ordered observations.
The first record pins the platform, board revision, and available EEPROM,
firmware, and media hashes.  Event sequence numbers are contiguous from zero;
unknown top-level fields, unknown phases, invalid boot sources, and malformed
hashes fail validation.

Version 1 defines these phases in order of responsibility, while permitting a
phase to emit more than one event or to be revisited during retry and restart:

``reset``
  Power-on, watchdog, software, and fault-induced reset observations.
``rom`` and ``recovery``
  OTP and ``nRPIBOOT`` decisions, recovery selection, and recovery outcome.
``eeprom`` and ``boot-source``
  Persistent reads/writes and ordered ``BOOT_ORDER`` attempts and results.
``firmware`` and ``handoff``
  Boot-file, configuration, overlay, generated-DT, and ARM-entry observations.
``kernel`` and ``health``
  Guest-visible boot identity and the declared end-to-end readiness gate.
``fault``
  The exact persistent transaction or transport boundary where a deterministic
  fault was injected.

The comparator deliberately drops producer identity, capture identity,
wall-clock start, per-event timestamps, and a small fixed set of volatile data
keys.  Stable payloads, event ordering, artifact hashes, reset causes, sources,
and outcomes must match exactly.  Any additional normalization must be named on
the command line so a conformance run cannot silently weaken its oracle.

``contrib/raspi4/conformance_gate.py`` turns this contract into a batch release
gate.  Its version-2 manifest pins each Pi 4B or CM4 case to an exact board
revision and SHA-256-verified EEPROM, firmware, media, and OTP subset.  Each
case also pins a non-empty, exact ordered event envelope through
``event_contract``.  Entries require phase and event and can additionally pin
source and outcome.  Thus two identically truncated captures cannot satisfy
the release contract.  A hardware mismatch is an invalid oracle capture; a
QEMU mismatch is behavioral drift.  Hardware and QEMU producer entries either
replay declared trace files or execute a declared argv array without a shell.
A producer can have its standard output captured directly as JSON Lines,
avoiding shell redirection.  Executed producers cannot pass with stale
results: the prior output is removed and a newly created trace is required.
The hardware entry can instead name paired ``hil_plan`` and ``hil_report``
paths.  Before any fixture command runs, the gate preflights the entire batch
and requires each HIL plan's platform, board revision, trace path, artifact
set, resolved artifact paths, and digests to equal its conformance case.
Thus one release-gate invocation executes the fixed hardware sequence, retains
its atomic report, runs the QEMU producer, and performs the differential
comparison; malformed later cases cannot leave an earlier board half-tested.
The trace platform, producer identity, board revision, and every verified
artifact digest must agree with the manifest before semantic comparison begins.
The plan, hardware trace, retained report, QEMU trace, and every input artifact
must also be isolated by resolved path and existing file identity.  Direct
collisions, hard-link aliases, and symlink outputs fail before fixture access.
After capture, the conformance gate re-reads the atomic HIL report and verifies
its successful platform/revision/artifact identity plus trace path, SHA-256,
and record count against the fresh validated trace.
The JSON report distinguishes capture/configuration failure from behavioral
drift and retains ordered differences for every case.  Per-case extra volatile
keys are explicit manifest data and therefore reviewable.  Sixteen unit tests
cover matching replay, stable drift, matching truncation rejection, QEMU
truncation and ordering drift, contract validation, artifact tampering, wrong
producer identity, stale-output replacement, direct standard-output capture,
unknown-field rejection, integrated HIL execution, cross-manifest mismatch,
path collision preflight, retained-report tamper rejection, and whole-batch
preflight.  Physical oracle data is still required; this runner does not
convert missing HIL measurements into emulated evidence.

``contrib/raspi4/hil_orchestrator.py`` supplies the missing hardware-producer
layer.  Its strict version-1 plan binds every command sequence to
SHA-256-verified EEPROM, firmware, and media paths and executes argv arrays
without a shell.  Pi 4B plans must power off, flash media, power on, capture
UART, and check health in that order.  CM4 plans must additionally assert
``nRPIBOOT``, power the ROM path, run unchanged ``rpiboot``, flash eMMC,
power-cycle, release ``nRPIBOOT``, and boot normally in the prescribed order.
The UART producer's standard output becomes the fresh hardware JSON Lines
trace.  Stale output is deleted before the first fixture action.

Mandatory platform-specific cleanup runs in ``finally`` after success,
failure, timeout, or trace rejection: power is removed and CM4
``nRPIBOOT`` is released.  The atomic report includes verified input hashes,
ordered command result/output hashes, cleanup outcomes, and the validated
trace hash and record count.  Eight mock-backed tests cover the complete Pi 4B
and CM4 order, artifact tampering before hardware access, mid-flash failure
with cleanup, malformed sequence rejection, resolved path collisions,
hard-link aliases, and symlink outputs.  A plan and mock execution do not
count as HIL success; TEST-004/005/009 require an attached board and checked-in
or release-retained captures.

``contrib/raspi4/fixture_bundle.py`` removes the remaining hand-authored-plan
gap without inventing fixture commands.  A strict version-1 operator spec must
provide every platform-specific step and cleanup argv, the exact board
revision and event contract, a QEMU JSON-Lines producer argv, and paths to
EEPROM, firmware, media, and optional OTP inputs.  The builder resolves,
de-aliases, and SHA-256 pins those regular non-symlink files; both fixture and
QEMU command sets must reference every artifact placeholder.  It derives the
fixed Pi 4B or CM4 sequence, expands QEMU references to those same verified
bytes, and emits a HIL plan plus version-2 conformance manifest only after the
real validators accept the pair.  Existing outputs require explicit
``--force``.  Five tests cover Pi 4B and CM4 generation, exact sequences and
digests, preflight, overwrite protection, missing commands/placeholders,
symlinks, and same-file artifacts.  Generated plans are deployment artifacts,
not physical evidence.

Conformance strategy
--------------------

The oracle is a pinned Pi 4B board revision, EEPROM release, firmware release,
SD image, and power supply.  Capture UART, USB enumeration, EEPROM before and
after, partition hashes, DTB, kernel log, and selected MMIO traces.  Normalize
timestamps and serial numbers, then compare ordered events rather than wall
clock duration.

Release gates are:

* clean and interrupted image writes produce expected persistent bytes;
* EEPROM recovery and update success/failure match the fixture;
* each supported ``BOOT_ORDER`` source succeeds, falls through, stops, or
  restarts at the same boundary;
* the firmware-generated DT and ARM register handoff match;
* Linux enumerates the same modeled devices without QEMU removing DT nodes;
* first boot reaches the same systemd target and health check;
* unsupported behavior is reported as unsupported, never silently bypassed.

Primary references
------------------

* QEMU Raspberry Pi machine documentation:
  https://www.qemu.org/docs/master/system/arm/raspi.html
* QEMU machine and SoC sources: ``hw/arm/raspi.c``,
  ``hw/arm/raspi4b.c``, and ``hw/arm/bcm2838.c``
* Raspberry Pi EEPROM boot flow and ``BOOT_ORDER``:
  https://www.raspberrypi.com/documentation/computers/raspberry-pi.html
* Raspberry Pi firmware ``config.txt`` behavior:
  https://www.raspberrypi.com/documentation/computers/config_txt.html
* BCM2711 ARM-visible peripheral documentation:
  https://datasheets.raspberrypi.com/bcm2711/bcm2711-peripherals.pdf
* Raspberry Pi EEPROM release images and tools:
  https://github.com/raspberrypi/rpi-eeprom
* Raspberry Pi bootloader configuration and ``BOOT_ORDER`` field definitions:
  https://github.com/raspberrypi/documentation/blob/master/documentation/asciidoc/computers/raspberry-pi/eeprom-bootloader.adoc
