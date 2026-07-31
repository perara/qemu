.. SPDX-License-Identifier: GPL-2.0-or-later

Raspberry Pi 4 platform laboratory
===================================

This directory contains the first executable milestone of the
``feature/raspi4-full-platform`` branch.  It validates the host-side part of
the production path without claiming that QEMU already executes the BCM2711
VideoCore boot ROM.

Flash a regular-file SD image and verify every written byte::

  python3 contrib/raspi4/rpi_image.py flash build/os.img work/sd.img

Inspect its MBR/GPT, partition identities, and FAT/ext UUIDs and labels::

  python3 contrib/raspi4/rpi_image.py inspect work/sd.img

Fail closed unless every ``PARTUUID``, ``PARTLABEL``, ``UUID``, or ``LABEL``
reference in extracted boot configuration and ``fstab`` files resolves to
exactly one partition in those unchanged raw-image bytes::

  python3 contrib/raspi4/rpi_image.py inspect work/sd.img \
      --boot-config work/cmdline.txt --fstab work/fstab

``--boot-config`` and ``--fstab`` may be repeated.  The JSON report records
each reference and its resolved partition index.  The inspector derives MBR
disk-signature PARTUUIDs, GPT unique GUIDs and labels, FAT volume IDs/labels,
and ext superblock UUIDs/labels without mounting or modifying the image.
For the standard one-FAT/one-ext Raspberry Pi image layout, read and validate
the files from the unchanged image itself::

  python3 contrib/raspi4/rpi_image.py inspect work/sd.img \
      --validate-contained-identities

This bounded path follows FAT12/16/32 cluster chains for root
``cmdline.txt`` and ext extent/directory metadata for ``/etc/fstab``.  It
requires exactly one FAT and one ext partition, regular UTF-8 files without
NUL bytes, supported extents, and uniquely resolvable identifiers.  Invalid
geometry, bounds, chains, extents, directories, or references fail closed.

Exercise recovery from an interrupted flash.  Exit status 75 means that the
requested power loss was injected, and the partial image is intentionally
preserved together with a versioned durable resume journal::

  python3 contrib/raspi4/rpi_image.py flash build/os.img work/sd.img \
      --fail-after 64MiB

Resume only after the tool re-hashes the current source, the journaled source
prefix, and the target prefix, and verifies that the exact target object is
still exclusively owned::

  python3 contrib/raspi4/rpi_image.py flash build/os.img work/sd.img --resume

The default journal is ``work/sd.img.rpi-resume.json``.  It is fsynced after
each target checkpoint and removed only after final whole-image SHA-256
verification.  A changed source, replaced or corrupted target, malformed or
stale journal, concurrent writer, or ``--no-verify`` resume fails closed.  A
second ``--fail-after`` remains an absolute image offset, enabling repeated
interruption tests.  ``--resume-journal`` selects an explicit journal path.

Size options accept Python integer syntax or binary size suffixes, for example
``0x4000000`` or ``64MiB``.  A physical block device is rejected unless
``--allow-block-device`` is present.  That opt-in is deliberate because
selecting the wrong device destroys data.  Even with it, Linux flashing opens
the target and validates its actual major/minor identity before writing.  The
target must be a writable, removable whole device rather than a partition;
neither it nor any sysfs descendant may appear in any currently inspectable
Linux mount namespace, no descendant may be active raw swap, and its
512-byte-sector capacity must fit the complete source image.  The preflight
deduplicates namespace identities through ``/proc/PID/ns/mnt``, bounds process
enumeration, and rejects missing, malformed, unreadable, or repeatedly changing
namespace evidence.  Processes which have already disappeared are ignored.
Missing or malformed sysfs, mountinfo, or swap metadata also fails closed.  A
block-device run requires an explicit journal outside ``/dev``::

  sudo python3 contrib/raspi4/rpi_image.py flash os.img /dev/sdX \
      --allow-block-device \
      --resume-journal /var/tmp/sdX.rpi-resume.json

The successful JSON result contains ``target_identity`` taken from the opened
and locked file descriptor rather than reconstructed from the command-line
path.  It records the canonical path, filesystem device/inode identity, target
kind and ``rdev``.  For block media it additionally records major/minor,
resolved sysfs path and kernel name, validated capacity, logical sector size,
removable state, and read-only state.  The exclusive lock remains held while a
second descriptor is opened for whole-image verification; if the path now
resolves to a different identity, verification fails and the resume journal is
retained.  Release automation should retain this object verbatim for operator
review.

These checks do not make a guessed device name safe: inspect the resolved
device and unplug unrelated media before granting the explicit opt-in.
Electrical removal/power behavior remains a HIL responsibility.

Decode an EEPROM boot order in actual attempt order (least-significant nibble
first)::

  python3 contrib/raspi4/rpi_image.py boot-order 0xf41

Boot unchanged USB media through EEPROM mode 4 on Pi 4B's live VL805/xHCI
bus, or use mode 5 with ``usb-boot-controller=dwc2`` for the BCM2711 USB2
connector path::

  qemu-system-aarch64 \
      -M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,usb-boot-drives=usb0+usb1:usb2 \
      -drive if=none,id=pieeprom,format=raw,file=pieeprom.bin \
      -drive if=none,id=usb0,format=raw,file=device0-lun0.img \
      -drive if=none,id=usb1,format=raw,file=device0-lun1.img \
      -drive if=none,id=usb2,format=raw,file=device1-lun0.img

For a CM4 carrier with an external VL805, select
``usb-boot-controller=xhci`` and configure the unchanged EEPROM with
``VL805=1``.  CM4 mode 4 otherwise fails that controller and falls through
without issuing xHCI traffic, matching the documented opt-in.  Query
``boot-vl805-enabled``, ``boot-vl805-initialized``, and
``boot-vl805-status``.  VMState v64 preserves this state across an active USB
startup wait.  Pi 4B's onboard controller does not require the CM4-only
setting.  QEMU models controller-visible initialization, not execution of the
opaque embedded VL805 MCU firmware.

Here ``+`` adds a LUN to one BOT device and ``:`` starts another USB device.
The realized devices, LUNs, and serials have stable identities.  The qtest
suite verifies them on the actual buses, assigns and enumerates root ports,
automatic hubs, and downstream devices, queries their maximum LUN,
completes SCSI INQUIRY and READ CAPACITY CBW/data/CSW traffic, and compares a
READ(10) sector from every LUN with the exact named backend.  Behavioral
selection uses the selected VL805/xHCI or DWC2/BOT transport as the
authoritative FAT/MBR/GPT reader.  The VL805 owner executes command and event
rings, device/endpoint contexts, EP0 control TDs, and bulk Normal TRBs from
guest DMA; hub topology, ring recycling, media reprobe, and migration are
covered.  QOM reports
``vl805-xhci-host-bot-scsi-read10-v1`` or
``dwc2-host-bot-scsi-read10-v1`` and the exact command, byte, and failed-CSW
counts; permanent backend read failure is observed as a
SCSI command failure before the configured LUN timeout falls through to the
next ``BOOT_ORDER`` source.  Add ``usb-boot-bot-stall-once=on`` to inject one
invalid CBW, or ``usb-boot-bot-stall-count=N`` for up to eight consecutive
faults, and exercise bounded BOT Mass Storage Reset Recovery through either
controller.  ``usb-boot-bot-recoveries`` reports completed recoveries; the
xHCI path includes controller Reset Endpoint and Set TR Dequeue commands.
``usb-boot-bot-phase-count=N`` cycles through all six USB-IF relations that
require Reset Recovery: cases 2, 3, 7, 8, 10, and 13.  They share the same
combined eight-recovery bound, and ``usb-boot-bot-phase-errors`` reports them.
Qtests run four consecutive malformed-CBW recoveries and all six phase-error
recoveries through each controller.  ``usb-boot-bot-case-count=13`` executes
the complete USB-IF direction/length matrix with no-op TEST UNIT READY,
INQUIRY, and MODE SELECT commands, verifies exact CSW residue and phase
classification, performs Reset Recovery only for the six required cases, and
then retries the original unchanged command.  Read-only
``usb-boot-bot-cases-tested`` reports progress and migrates with the campaign.
Qtests also compare the complete USB image before and after all thirteen cases.
Physical timing and long-duration campaigns remain conformance work.

To scan a separately attached QEMU device or a physical host USB device,
enable the mutually exclusive external topology mode.  The stable Pi 4B xHCI
bus is ``vl805.0``::

  qemu-system-aarch64 \
    -M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,\
usb-boot-external=on \
    -drive if=none,id=pieeprom,format=raw,file=pieeprom.bin \
    -device usb-host,bus=vl805.0,hostbus=3,hostport=2

For EEPROM mode 5, add ``usb-boot-controller=dwc2`` and attach to
``usb-bus.0``.  Resolve ``hostbus``/``hostport`` from the actual host, unmount
the target first, and ensure no other process owns it.  QEMU needs libusb
permissions and detaches the host kernel driver while the device is owned.
The external scan uses the same descriptor, BOT, READ CAPACITY, and READ(10)
path as machine-owned media.  It does not simulate physical port power,
overcurrent, cables, or analogue USB signalling.

Boot unchanged raw NVMe media through EEPROM ``BOOT_ORDER`` mode 6 while
exposing the same bytes to the guest PCIe controller::

  qemu-system-aarch64 \
      -M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,nvme-drive=nvme0 \
      -drive if=none,id=pieeprom,format=raw,file=pieeprom.bin \
      -drive if=none,id=nvme0,format=raw,file=nvme.img

Pi 4B normally creates its PCIe VL805-compatible XHCI endpoint.  Because the
BCM2711 board has one downstream link, ``nvme-drive`` replaces that automatic
endpoint; CM4 instead leaves ``pcie-root`` available for an explicit device.
PCIe currently provides the Broadcom register/config windows, outbound memory,
link state, legacy INTx, and the native internal MSI controller.  Its root port identifies as the production
Broadcom BCM2711 ``14e4:2711`` revision ``20`` PCI bridge and advertises the
board's Gen2 x1 maximum link.  Qtests retain those values through reset and
migration, and the unchanged production kernel verifies them through PCI
sysfs.  The MSI model accepts the two documented below/above-4-GiB doorbells,
matches the low 16-bit 32-vector data pattern, implements status/clear/mask
registers, and drives GIC SPI 148.  Pending state and masks migrate; reset
clears and masks the controller.  The production DT retains its
``msi-controller``/``msi-parent`` binding, and unchanged Linux proves active
MSI IRQs for VL805 and NVMe while completing their normal I/O.  SSC, remaining
controller errors/registers, and electrical link behavior remain conformance
work.
The standard production-kernel gate binds both the default VL805 and the
alternate NVMe topology in separate runs and verifies an NVMe sector against
the unchanged host backend.  It also verifies a durable guest write and PCI
function reset.  A deterministic RSA/OTP qtest separately proves signed
mode-6 handoff and rejection of a tampered outer image.  The shared namespace
also accepts standard ``blkdebug`` faults: the same read failure drives mode-6
fallback and an NVMe ``Unrecovered Read Error`` CQE.  WRITE faults return
``Write Fault`` and are tested for exact non-mutation before a one-shot
recovery WRITE becomes durable.  Dirty FLUSH fault/recovery and non-mutating
COMPARE mismatch/success statuses use the same queue gate.

Boot trace contract
-------------------

``boot-trace-schema-v1.json`` defines the JSON Lines contract shared by QEMU
and the physical Pi 4B/CM4 capture fixture.  Every trace begins with a header
that pins its board and artifacts.  Ordered events then record reset, ROM,
recovery, EEPROM, boot-source, firmware, ARM handoff, kernel, health, and fault
transitions.

Validate a hardware or QEMU capture::

  python3 contrib/raspi4/boot_trace.py validate work/boot.jsonl

Compare a QEMU capture against the pinned hardware oracle::

  python3 contrib/raspi4/boot_trace.py compare \
      oracle/pi4b-2026-05-11.jsonl work/qemu.jsonl

Convert the QEMU trace backend output into the same JSON Lines contract::

  python3 contrib/raspi4/boot_trace.py from-qemu work/qemu-trace.log \
      --board-revision 0xb03115 \
      --eeprom-sha256 "$EEPROM_SHA256" \
      --otp-sha256 "$OTP_SHA256" \
      --media-sha256 "$MEDIA_SHA256" > work/qemu.jsonl

The comparator checks ordered stable observations exactly.  It removes the
producer, capture ID, wall-clock fields, event timestamps, and documented
volatile data keys such as serial numbers.  Additional keys are ignored only
when explicitly passed with ``--ignore-data-key``.

For an automated release run, use ``conformance_gate.py`` with a version-2
JSON manifest.  Each case declares its platform and board revision, pins every
input artifact by path and SHA-256, names hardware and QEMU trace outputs, and
provides a non-empty ``event_contract`` array.  Contract entries require
``phase`` and ``event`` and may additionally pin ``source`` and ``outcome``.
The array is the exact ordered event envelope: a matching but truncated
hardware/QEMU pair therefore cannot pass.  Hardware contract violations are
invalid oracle captures; QEMU contract violations are behavioral drift.
An optional ``command`` is an argv array executed directly (never through a
shell) for each producer.  The gate deletes that producer's old trace first,
requires the command to create a new valid trace, verifies ``hardware`` versus
``qemu`` producer identity and trace-header hashes, then compares all cases::

  python3 contrib/raspi4/conformance_gate.py work/conformance.json

Exit status 0 means every ordered semantic trace matched, 1 means measured
behavior drifted, and 2 means a manifest, artifact, capture, or trace was
invalid.  Set ``capture_stdout`` to ``true`` when the argv command writes its
JSON Lines capture to standard output; the gate writes it directly to the
declared trace without requiring shell redirection.  Omitting ``command``
deliberately supports review and replay of an
immutable checked-in physical corpus.  Extra ignored data keys must be listed
per case as ``ignore_data_keys`` so weakening the oracle is visible in review.
For a live hardware producer, replace ``command`` with paired ``hil_plan`` and
``hil_report`` paths.  The gate validates every case, artifact, duplicate ID,
and HIL binding before executing the first fixture command.  It also requires
the plan's platform, board revision, trace path, artifact set, resolved
artifact paths, and SHA-256 values to match the conformance case exactly.
The complete fixed hardware workflow and QEMU producer then run from the same
manifest invocation, and any HIL failure returns status 2 before comparison.
Preflight also isolates the plan, hardware trace, retained report, QEMU trace,
and every pinned artifact by resolved path and existing file identity.  A
direct collision, hard-link alias, or symlink output is rejected before any
fixture command.  After capture, the gate re-reads the atomic report and checks
its platform, revision, artifact hashes, success state, trace path, trace hash,
and record count against the newly validated hardware trace.

Physical HIL orchestration
--------------------------

``hil_orchestrator.py`` produces the fresh ``hardware`` trace consumed directly
by the conformance gate's ``hil_plan`` mode.  Its version-2 JSON plan pins
``eeprom``, ``firmware``, and
``media`` files by lowercase SHA-256 (plus optional ``otp``), the exact board
revision, one normalized stable ``/dev/disk/by-id/...`` or
``/dev/disk/by-path/...`` flash target, and argv-only fixture commands.  Every
required artifact must be
referenced by ``{artifact:eeprom}``, ``{artifact:firmware}``, or
``{artifact:media}`` in a step command, so a reviewed plan cannot verify one
file and silently flash another.  ``capture-uart`` must write the version-1
hardware boot trace to standard output; the orchestrator deletes any stale
trace first and writes the new bytes itself.
The plan, trace, report, and all artifact paths must be distinct.  Existing
hard-link aliases and symlink inputs or outputs fail before fixture access, so
a trace cleanup or report replacement cannot delete or overwrite a pinned
EEPROM, firmware, media, OTP, or plan file.

The Pi 4B sequence is fixed to::

  power-off, flash-media, power-on, capture-uart, health-check

The CM4 sequence is fixed to::

  power-off, assert-nrpiboot, power-on-rpiboot, run-rpiboot,
  flash-emmc, power-off-flashed, release-nrpiboot, power-on-boot,
  capture-uart, health-check

The ``flash-media`` or ``flash-emmc`` command must write exactly one successful
``rpi_image.py flash`` JSON object to standard output.  The orchestrator
requires byte counts and both hashes to match the pinned ``media`` artifact,
requires whole-image verification, and validates a removable writable block
target whose ``rdev`` agrees with its reported major/minor, sysfs identity,
capacity, and 512-byte logical sector size.  Missing, malformed, regular-file,
wrong-media, inconsistent, or unverified attestations fail the workflow.  The
flash step alone must contain a separate ``{flash-target}`` argument; command
expansion supplies the plan's pre-authorized stable path, and the attestation's
original target string must match it exactly.  A valid attestation for another
removable disk is therefore still rejected.  The
validated object is retained verbatim as ``flash_attestation`` on the flash
step in the atomic version-2 HIL report.  The conformance gate independently
revalidates that retained evidence against the manifested media before
accepting the hardware trace.  A wrapper around another physical imaging tool
must perform equivalent readback and emit this exact contract; command success
alone is insufficient.

Plans also declare mandatory cleanup.  Pi 4B runs ``power-off-cleanup``;
CM4 runs ``power-off-cleanup`` followed by
``release-nrpiboot-cleanup``.  Cleanup runs after success, command failure,
timeout, or invalid trace.  Commands are executed directly without a shell,
each timeout is bounded to 1--3600 seconds, command output is represented by
size and SHA-256 in the report, the flash output is additionally retained as
validated identity evidence, and the success or failure report is replaced
atomically.

Create both a HIL plan and its matching version-2 conformance manifest with
``fixture_bundle.py``.  Its strict version-2 specification names the platform,
board revision, authorized stable ``flash_target``,
EEPROM/firmware/media/optional-OTP files, exact event contract, every required
fixture step/cleanup argv, and a QEMU JSON-Lines producer argv.
Every artifact path is resolved, required to be a distinct regular non-symlink
file, and SHA-256 pinned.  Both fixture and QEMU command sets must visibly
reference every ``{artifact:name}``; the builder expands the QEMU references
to the same verified files while preserving HIL placeholders for execution.
It emits ``ID.hil-plan.json`` and ``ID.conformance.json`` only after both pass
the real orchestrator/conformance preflight.  Existing bundles are not replaced
unless ``--force`` is explicit::

  python3 contrib/raspi4/fixture_bundle.py work/pi4-fixture-spec.json \
      --output-dir work/pi4-release

The generated manifest owns the hardware/QEMU trace and HIL-report filenames,
so it can be passed directly to ``conformance_gate.py``.  The five builder
tests provide complete synthetic Pi 4B/CM4 specification examples while
remaining explicitly non-hardware evidence.

Run a configured fixture directly with::

  python3 contrib/raspi4/hil_orchestrator.py work/pi4-hil-plan.json \
      --output work/pi4-hil-report.json

Exit status 0 requires the complete ordered workflow, cleanup, and a fresh
valid hardware trace whose platform, board revision, and artifact hashes match
the plan.  Status 2 is fail-closed.  The mock-backed unit tests validate both
board sequences and cleanup behavior; they are not physical evidence.

Behavioral machines expose ``boot-health`` for capture producers that observe
the guest console.  After ``arm-handoff-ready``, report ``kernel-started`` and
then ``userspace-ready`` with QMP ``qom-set``; ``failed`` is also accepted.
Skipped or regressing transitions fail closed.  Accepted observations emit
ordered ``health`` events, migrate with the machine, and clear on reset.  The
standard functional gates drive these values only after their real console
markers, keeping this an explicit observation boundary rather than inferred
guest state.

Behavioral QEMU first stage
---------------------------

The branch exposes its in-progress clean-room reset path only when explicitly
selected.  Bind an unmodified BCM2711 EEPROM image as a named persistent block
backend::

  build/qemu-system-aarch64 \
      -M raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom \
      -drive if=none,id=pieeprom,format=raw,file=pieeprom.bin \
      -drive if=sd,format=raw,file=sd.img

For a production-like empty Pi 4B slot, omit the SD filename but retain a
stable backend ID::

  -drive if=sd,id=sdcard

Use standard QMP ``blockdev-change-medium`` and ``eject`` commands against
``sdcard``.  The same backend remains attached to the emulated SD controller;
insertion wakes behavioral ``BOOT_ORDER`` card-detect or infinite-retry state
and probes the unchanged image immediately.  The pending detect state migrates
with the machine.  If ``eject`` arrives during controller I/O, SDHCI cancels
the active transfer, discards any incomplete PIO sector, clears transfer and
buffer-ready state, and raises card-removal plus data-timeout status/IRQ when
enabled.  This state is migration-safe; reinserting the same medium permits a
clean controller reset and read without committing the interrupted partial
sector.  CM4 eMMC is soldered fixed media, so its user, boot, and RPMB backends
reject these QMP removal operations.

For Compute Module 4, bind the soldered storage bytes as a named persistent
backend.  This is the same regular file exported by the RPIBOOT mass-storage
transport::

  build/qemu-system-aarch64 \
      -M raspi-cm4,boot-mode=behavioral,eeprom-drive=pieeprom,emmc-drive=emmc \
      -m 4G \
      -drive if=none,id=pieeprom,format=raw,file=pieeprom.bin \
      -drive if=none,id=emmc,format=raw,file=cm4-emmc.img

``raspi-cm4`` attaches this backend as QEMU's eMMC protocol device on EMMC2.
The guest sees MMC CID/CSD, EXT_CSD capacity, high-speed mode, and MMC
multi-block I/O; SDHCI Auto CMD23 supplies the block count before CMD18/CMD25.
The regular file is still the exact byte surface exported to host flashing
tools.  Optional persistent hidden areas can be attached without changing that
byte layout::

  -M raspi-cm4,...,emmc-drive=emmc,emmc-boot-drive=emmcboot,emmc-rpmb-drive=emmcrpmb \
  -drive if=none,id=emmc,format=raw,file=cm4-emmc.img \
  -drive if=none,id=emmcboot,format=raw,file=cm4-emmc-boot.bin \
  -drive if=none,id=emmcrpmb,format=raw,file=cm4-emmc-rpmb.bin

The boot backend is boot0 followed by an equal-sized boot1; its total length
must be a nonzero multiple of 256 KiB and each half is limited to 32,640 KiB.
The RPMB backend must be a nonzero multiple of 128 KiB up to 16,384 KiB.  QEMU
derives EXT_CSD sizes from these files.  The user area remains byte-for-byte at
offset zero in ``cm4-emmc.img``.  The behavioral qtest also completes a live
migration between two CM4 processes with all three backends attached and
rechecks their derived geometry and unchanged bytes.  Current identity fields
and the absence of board-SKU-specific hidden-area defaults remain generic
rather than a claim
about one physical CM4 eMMC vendor/SKU.  ``emmc-cid=HEX`` can replay either a
15-byte CID payload or the 16-byte value read directly from Linux
``/sys/class/block/mmcblk0/device/cid``.  A zero sysfs CRC byte is reconstructed
for the MMC protocol; a supplied nonzero CRC/end byte must validate.  This
supports hardware-oracle captures without pretending that all CM4 production
lots use one eMMC supplier.

The CM4 machine can inject controller-visible data errors without changing the
same image bytes::

  -M raspi-cm4,...,emmc-data-error=timeout,emmc-data-error-after=0,emmc-data-error-count=1

Use ``crc`` for the SDHCI data-CRC class.  The byte boundary must be a multiple
of 512, zero targets the first card data transaction, and the bounded count is
consumed across guest controller resets.  Linux recovery gates prove both
error classes followed by successful card re-enumeration.  This complements
the host USB/BOT faults below; it is not an electrical-media model.

At the EMMC2 controller boundary, reset is sector-atomic.  PIO bytes staged
below one complete 512-byte sector are discarded, while a fully submitted
sector has already reached the same persistent image.  The migration qtest
cuts a 320-byte write after live migration and proves no mutation, then writes
one complete sector and proves it survives reset without changing the next
sector.  Card-internal cache, NAND translation, and physical power loss remain
HIL boundaries unless the optional digital cache fault model is selected.
``emmc-cache-size=N`` advertises a bounded cache in EXT_CSD, honors CMD6
``CACHE_CTRL`` and ``FLUSH_CACHE``, overlays dirty sectors on guest reads, and
writes the complete cache batch when capacity is reached.
``emmc-cache-power-loss-on-reset=on`` makes reset an explicit card-power-loss
fault: unflushed entries disappear, while flushed or pressure-written sectors
remain in the exact backend.  Dirty entries and contents migrate, and
``cache-dirty-sectors`` on the eMMC child exposes their count.  With the
power-loss switch off, reset flushes for compatibility.  Cache timing, NAND
translation, wear, and physical power behavior remain HIL/calibration
boundaries.
Set ``emmc-cache-flush-sector-delay-us=N`` to make CMD6 ``FLUSH_CACHE``
write and flush one dirty sector at each virtual deadline.  The card stays in
programming state, exposes ``cache-flush-active`` and
``cache-flush-completed-sectors``, and migrates its exact timer/progress.
Power-cut reset retains the completed prefix and discards only the dirty tail.
The qtest campaign covers all five cut positions in a four-sector flush.  It
also injects a timer-callback backend failure, verifies that the entire dirty
set remains retryable, and proves exact persistence after retry.
Zero retains synchronous behavior; the supplied delay is not a physical timing
claim without a pinned hardware trace.
Set ``emmc-program-sector-delay-us=N`` to put complete uncached sectors through
the card-program timing path.  Normal direct and Auto CMD23 reliable writes
then become durable one sector per virtual deadline.  The exact pending queue,
partition identities, progress, and timer migrate; reset drops only the
unfinished suffix.  ``program-active``, ``program-pending-sectors``, and
``program-completed-sectors`` expose the boundary.  A backend failure retains
the queue for an identical retry.  Zero preserves synchronous behavior, and
the configured delay requires hardware calibration before it can represent a
specific eMMC SKU.
The default qtest suite also runs a 64-cycle mixed cache/direct/reliable
campaign.  It covers every four-sector cut point, resets at the unfinished
deadline minus one microsecond, performs seven active live migrations, and
checks distinct persistent media windows after every reset.
The eMMC also models CMD35/CMD36/CMD38 erase as 512 KiB high-capacity groups
with ``0xff`` erased content.  Erase bypasses the volatile write cache and
invalidates overlapping entries.  Set ``emmc-erase-group-delay-us=N`` to make
one group durable per virtual deadline.  ``erase-active``,
``erase-pending-groups``, and ``erase-completed-groups`` expose progress; the
range, partition, timer, and counters migrate.  Reset retains completed groups
and abandons the unfinished suffix, while backend failure leaves the current
group retryable.  Zero keeps synchronous erase.  This does not claim secure
trim/sanitize or calibrated physical NAND behavior.
The model advertises enhanced reliable writes in EXT_CSD as well.  When SDHCI
Auto CMD23 forwards Argument 2 bit 31, every completed sector in the following
bounded CMD25 bypasses the volatile cache and flushes the selected backend.
The flag survives live migration through an active partial sector.  Normal
dirty cache entries remain separate and can still be lost by the explicit
power-cut reset, matching the guest-visible FUA distinction without changing
the raw eMMC image.
A failed backend flush sets the standard R1 ``ERROR`` status.  The qtest
injects that failure with blkdebug, verifies the normal cache stays isolated,
then retries the identical reliable sector and proves reset durability.

Both ``raspi4b`` and ``raspi-cm4`` accept the production 1, 2, 4, and 8 GiB
RAM families through standard QEMU ``-m 1G|2G|4G|8G``.  The selection changes
one coherent SKU: QEMU derives the matching board-revision memory bits,
factory OTP identity, RAM mapping, and final DT memory ranges.  It does not
change eMMC capacity.  A persistent OTP backend must contain the matching
board revision, or behavioral ROM stops at ``otp-board-mismatch``.  Other RAM
sizes are rejected rather than rounded or silently assigned an invented
revision.  ``board-revision=0x...`` may select an exact new-style Pi 4B or CM4
revision when reproducing a captured board.  QEMU rejects a value whose model,
BCM2711 processor, or memory-size bits conflict with the selected machine and
``-m`` value, so revision-specific behavior does not split the coherent
memory model.  BCM2711's 64 MiB peripheral hole at the top of the 32-bit
address space does not discard RAM: the displaced bytes continue from 4 GiB.
For production DTBs with one ``#size-cells``, the 8 GiB model describes its
4 GiB + 64 MiB high range as adjacent 2 GiB, 2 GiB, and 64 MiB banks.  This is
the same contiguous mapped backing store, expressed without overflowing the
DT cell width.

SPI0 DMA
--------

The BCM2835-compatible SPI0 block is connected to the BCM2711 DMA controller
through the documented peripheral maps 6 (TX) and 7 (RX).  With ``DMAEN`` set,
the first 32-bit FIFO write carries ``DLEN`` in bits 31:16 and the low eight
chip-select controls; following words supply four little-endian transfer bytes.
The controller stops after the exact programmed byte count and retains received
bytes until the independent RX DMA channel drains them.

The ``DC`` register's TX/RX DREQ and panic thresholds drive the matching DMA
inputs.  The controller also exposes three ``chip-select`` GPIO outputs and
applies TA, CE selection, polarity, and ADCS end-of-frame deassertion.  FIFO,
remaining-byte, completion, interrupt, CS, and derived DMA threshold state are
migration safe.

SPI0 consumes CPRMAN's VPU core clock.  Each FIFO byte completes after eight
SCLK cycles at ``core / CLK``; ``CLK=0`` selects 65536, odd divisors round down,
and divisor two is the maximum serial rate.  A stopped core clock freezes the
active byte, and core-rate or divider changes preserve its remaining SCLK
cycles.  The active timer and stopped-clock remainder migrate.  Bit-edge
CPOL/CPHA waveform conformance, LoSSI, malformed-transfer semantics, and
electrical timing remain outside this SPI0 slice.

Auxiliary SPI1/SPI2
-------------------

The BCM2835 AUX block exposes two independent SSI buses named ``aux-spi1`` and
``aux-spi2``.  AUX enable bits 1 and 2 gate their production Linux register
windows at offsets ``0x80`` and ``0xc0``.  Each controller has four-entry,
32-bit TX and RX FIFOs, fixed and FIFO-supplied widths, IO and TXHOLD aliases,
peek/pop reads, native CS patterns, status levels, and shared TX-empty/idle
interrupts.  Three named chip-select outputs are available per controller,
and attached SSI peripherals receive the same native levels.

Entries run at ``core / (2 * (speed + 1))`` from CPRMAN's VPU clock.  A stopped
clock retains the remaining core cycles.  FIFO contents, an active entry,
held CS, interrupt state, and active/stopped deadlines migrate in AUX VMState
v5.  The automated test uses TXHOLD plus IO to read a W25Q80BL JEDEC identity
through SPI1 and migrates an independently active SPI2 transfer while VPU is
stopped.  A production functional gate boots the pinned unchanged Raspberry
Pi 5.15 kernel with official byte-unchanged ``spi1-1cs.dtbo`` and
``spi2-1cs.dtbo`` overlays and packaged ``spi-bcm2835aux.ko.xz`` and
``spidev.ko.xz`` modules.  Both platform drivers bind, both ``/dev/spidev``
nodes appear, and guest-driver writes complete to attached flash devices.
SSI exchange is byte-granular; arbitrary-bit edge behavior, post-input and
DOUT-hold electrical timing, and native-CS silicon quirks remain outside this
slice.

BCM2711 GPIO host socket
------------------------

Both machines can expose all 58 guest-visible BCM2711 pins through a QEMU
chardev without substituting a VirtIO GPIO controller::

  -chardev socket,id=gpio,path=/run/user/1000/rpi-gpio.sock,server=on,wait=off \
  -M raspi4b,gpio-chardev=gpio

With ``raspi-cm4,boot-mode=behavioral``, the same protocol controls the
active-low GPIO40 ``EMMC_DISABLE``/``nRPIBOOT`` strap.  Send ``SET 40 0``
before a system reset to enter RPIBOOT, ``SET 40 1`` to deassert it, or
``SET 40 Z`` to fall back to the ``nrpiboot`` machine property.  External
drives persist across reset but are released on bridge disconnect.

For incoming migration, QEMU withholds the destination banner until GPIO
VMState has loaded.  The daemon therefore never applies reset-default output
state during the handover.  The post-load banner triggers the normal
``GET ALL`` exchange, restoring migrated output ownership and refreshing
physical inputs before runtime traffic continues.

The version-1 line protocol supports ``SET <pin> 0|1|Z``, ``GET <pin>``,
``GET ALL``, ``RELEASE ALL``, and ``PING``.  QEMU pushes physical pin level and
output-enable changes, reports resets, validates every command, and releases
external inputs on disconnect.  See ``docs/devel/raspi4-platform.rst`` for the
complete response ordering and safety boundary.  This is a logical transport,
not permission to connect 3.3 V pins directly to an unprotected host adapter.
The backwards-compatible ``GET SIGNALS``,
``SET SIGNAL EEPROM_NWP 0|1|Z``, and
``SET SIGNAL SD_OVERCURRENT 0|1|Z`` commands carry input-only board signals
without assigning them fictitious BCM GPIO numbers. A ``signals`` entry in
either example map binds one to a libgpiod or USB-adapter line. EEPROM nWP low
locks status-register changes and high permits them. SD over-current high
drives the Pi 4 bootloader's five-second SD power-off/retry path when
``SD_OVERCURRENT_CHECK=1``. High impedance restores each machine-property
fallback.

Pi 4B EEPROM ``SD_QUIRKS=1`` selects the documented bootloader policy which
disables SD high-speed operation and limits the requested boot clock to
12.5 MHz.  Query ``boot-sd-quirks``, ``boot-sd-high-speed-enabled``, and
``boot-sd-clock-limit-hz``.  Only defined bit zero is accepted; reserved bits
fail closed.  The Pi 4B SDHCI Host Control high-speed bit is cleared and its
Clock Control divisor is programmed to the fastest modeled rate no greater
than the ceiling: 8,666,666 Hz from the controller's advertised 52 MHz base
clock.  Query ``boot-sd-controller-clock-hz`` and
``boot-sd-controller-high-speed`` for that applied register state.  CM4 eMMC
ignores the Pi 4B-only policy.  VMState v64 retains the selection and SDHCI
state across migration and reset re-applies both from EEPROM.  Behavioral
file reads are not transfer-clock paced; calibrated cadence, physical edge
shape, and marginal-card behavior remain HIL/oracle work.

``gpio_proxy.py`` connects that socket to one or more dedicated Linux GPIO
character devices using the official libgpiod v2 Python bindings.  The chip
and line numbers below are illustrative and must be matched to the isolated
adapter actually installed on the host.

Write the map to ``gpio-map.json``::

  {
    "version": 1,
    "consumer": "qemu-rpi-gpio",
    "pins": [
      {
        "virtual": 17,
        "chip": "/dev/gpiochip1",
        "line": 0,
        "allow_output": false,
        "active_low": false,
        "bias": "pull-down",
        "debounce_us": 1000
      },
      {
        "virtual": 27,
        "chip": "/dev/gpiochip1",
        "line": 1,
        "allow_output": true,
        "active_low": false,
        "bias": "as-is",
        "drive": "open-drain",
        "debounce_us": 0
      }
    ],
    "signals": [
      {
        "name": "EEPROM_NWP",
        "chip": "/dev/gpiochip1",
        "line": 2,
        "active_low": false,
        "bias": "pull-up",
        "debounce_us": 100
      },
      {
        "name": "SD_OVERCURRENT",
        "chip": "/dev/gpiochip1",
        "line": 3,
        "active_low": false,
        "bias": "pull-down",
        "debounce_us": 100
      }
    ]
  }

Then run the daemon::

  python3 contrib/raspi4/gpio_proxy.py \
    --config gpio-map.json \
    --socket /run/user/1000/rpi-gpio.sock

Every mapped line is requested as an input before the daemon connects to QEMU.
Mappings are input-only unless ``allow_output`` is explicitly true.  Guest
output direction and value are then applied atomically; guest input direction
enables both-edge monitoring and sends the physical value back to QEMU.
``active_low``, input bias, output drive mode, and kernel debounce are explicit
per-line settings.  Unknown fields, duplicate virtual or physical lines,
out-of-range GPIOs, and unsafe drive combinations fail before hardware is
requested.  Reset, protocol error, denied output, signal termination, and
socket loss return every physical line to input before releasing it.

For a dedicated USB CDC ACM MCU adapter, select the deterministic version-1
USB-serial backend::

Write the map to ``gpio-map.json``::

  {
    "version": 1,
    "consumer": "qemu-raspi4-usb-gpio",
    "pins": [
      {
        "virtual": 5,
        "chip": "usb",
        "line": 3,
        "bias": "pull-up",
        "debounce_us": 100
      },
      {
        "virtual": 6,
        "chip": "usb",
        "line": 4,
        "allow_output": true,
        "active_low": false,
        "drive": "open-drain"
      },
      {
        "virtual": 40,
        "chip": "usb",
        "line": 5,
        "bias": "pull-up",
        "debounce_us": 100
      }
    ],
    "signals": [
      {
        "name": "EEPROM_NWP",
        "chip": "usb",
        "line": 6,
        "active_low": false,
        "bias": "pull-up",
        "debounce_us": 100
      },
      {
        "name": "SD_OVERCURRENT",
        "chip": "usb",
        "line": 7,
        "active_low": false,
        "bias": "pull-down",
        "debounce_us": 100
      }
    ]
  }

Then run the daemon::

  python3 contrib/raspi4/gpio_proxy.py \
    --backend usb-serial \
    --usb-device /dev/ttyACM0 \
    --config gpio-map.json \
    --socket /run/user/1000/rpi-gpio.sock

The adapter is initialized input-first, then receives atomic direction/value
updates and sends input edges.  It must autonomously release every line on USB
reset/disconnect, watchdog expiry, reboot, or protocol error.  The complete
wire contract and mandatory 500 ms fail-safe watchdog are specified in
``gpio-usb-serial-protocol-v1.md``.  The included pseudo-terminal tests prove
host protocol behavior.  A portable, compiled firmware core plus a pinned
Pico SDK 2.3.0 RP2040 USB/GPIO frontend live in
``contrib/raspi4/gpio_adapter``.  Its host self-test proves command parsing,
polarity, atomic output, debounce, watchdog, disconnect, overflow, and
HELLO-only recovery.  Building and flashing the UF2 still requires the Pico
toolchain and does not substitute for electrical safety testing.  The pinned
SDK/toolchain cross-build has produced byte-identical ``.bin`` and ``.uf2``
outputs in two clean directories; ``firmware-manifest.json`` and
``verify_qgpio_firmware.py`` lock and verify those exact bytes.

For CI or configuration dry-runs without GPIO hardware, add ``--backend mock``.
The mock exercises protocol and safety state transitions but is not an HIL
result.  Real use requires official libgpiod v2 bindings, a dedicated adapter,
level shifting/protection appropriate to the fixture, and a reviewed map.

Unmodified CM4 RPIBOOT and host flashing
----------------------------------------

``rpiboot_raw_gadget.c`` is a Linux Raw Gadget device-side bridge for the
wire protocol used by the unmodified Raspberry Pi ``rpiboot`` program.  It
enumerates as the BCM2711 ROM device ``0a5c:2711``, receives the unchanged
``bootcode4.bin`` vendor-control/bulk stream, disconnects, re-enumerates as
the second-stage file server, and captures every requested boot file without
conversion.  Build it with::

  cc -std=gnu11 -Wall -Wextra -Werror -O2 -pthread \
      -o build/rpiboot-raw-gadget \
      contrib/raspi4/rpiboot_raw_gadget.c

The normal Meson build also produces ``build/qemu-rpi-rpiboot-raw-gadget``
and runs its unprivileged lifecycle/lock self-test.  The command examples
below retain the shorter manually built filename.

On a Linux host with ``dummy_hcd`` and ``raw_gadget``, start the device before
running the official, unmodified host tool.  The example explicitly requests
the documented mass-storage bundle's ``config.txt`` and ``boot.img`` after
receiving ``bootcode4.bin``::

  mkdir -p work/rpiboot-capture
  sudo modprobe dummy_hcd is_high_speed=1
  sudo modprobe raw_gadget
  sudo build/rpiboot-raw-gadget \
      --capture-dir work/rpiboot-capture --mass-storage

  sudo /path/to/official/usbboot/rpiboot -d /path/to/boot-files

For focused protocol tests, ``--request FILE`` may be repeated instead of
``--mass-storage``.  Compare each captured file with the served file using
``sha256sum`` or ``cmp``.  The production bridge test must use the original official
``bootcode4.bin`` and boot files; repacked or emulator-specific substitutes
do not satisfy the gate.

For the QEMU-owned in-process ROM path, pass the SHA-256 of that same unchanged
file as ``rpiboot-bootcode-trusted-sha256``.  Missing or mismatched trust
returns a nonzero ROM status and prevents second-stage re-enumeration.  The
CM4 supervisor reads this value from the mandatory pinned artifact manifest;
it does not transform or replace ``bootcode4.bin``.

The bridge has deterministic transport faults for negative and recovery
testing.  These still run the unmodified host ``rpiboot`` binary; the fault is
injected at the emulated device boundary::

  sudo build/rpiboot-raw-gadget \
      --capture-dir work/rpiboot-partial --mass-storage \
      --fault bootcode-disconnect --fault-after 4096

``file-disconnect`` additionally requires ``--fault-file FILE`` and cuts that
second-stage file at ``--fault-after`` bytes.  ``rom-status-stall`` stalls the
ROM-stage status request.  An injected fault exits with status 75 and preserves
the exact partial capture for inspection.  Start a new bridge instance to
model a clean power-cycle retry.

``rom-status-timeout`` and ``file-request-timeout`` instead withhold the
corresponding control-IN response.  The default 21,000 ms delay deliberately
exceeds the pinned official host's 20,000 ms read timeout; shorter values are
rejected.  The file-server case requires ``--fault-file``::

  sudo build/rpiboot-raw-gadget \
      --capture-dir work/rpiboot-timeout --mass-storage \
      --fault file-request-timeout --fault-file boot.img

After the host timeout boundary, the helper publishes ``rpiboot-failed``,
disconnects, and exits with status 75.  ``--fault-delay-ms`` may lengthen the
silent interval for targeted campaigns.  The privileged gate reaches this
boundary in both enumerations using the unchanged official host, checks the
exact files received before each failure, and completes a clean exact retry.

``bootcode-hold`` and ``file-hold`` stop after the same exact durable byte
boundary without publishing failure or closing the USB transport.  They are
test-only crash boundaries: externally SIGKILL the helper, verify that
``rpiboot-active`` blocks retry, run ``--recover-stale``, and then retry from
``rpiboot-failed``.  ``file-hold`` also requires ``--fault-file FILE``.

The bulk receiver runs independently from EP0 so real host USB resets remain
observable during active transfers.  ``bootcode-reset`` and ``file-reset``
wait after an exact byte boundary for ``usbreset``; ``reset-reenumerate``
applies both boundaries in one helper invocation.  For the combined
campaign, select the second-stage file explicitly::

  sudo build/rpiboot-raw-gadget \
      --capture-dir work/rpiboot-reset --lifecycle work/state \
      --mass-storage --fault reset-reenumerate --fault-after 4096 \
      --fault-file boot.img --fault-count 4

The reset generation invalidates the old bulk completion without discarding
the ROM/file-server's persistent transaction state.  An interrupted active
transaction rolls back to its stage entry, re-enumerates, and may be retried
by the unchanged host tool.  Normal enumeration resets preserve protocol
state.  The privileged gate performs real resets in both stages and requires
the final captures to match the official files byte-for-byte.  The bounded
``--fault-count`` repeats each selected reset boundary; the reference gate
performs four active resets in the ROM stage and four in the file-server
stage before allowing completion.

After ``rpiboot`` completes, export the exact eMMC backend as a standard host
USB mass-storage disk.  The production path uses foreground ``serve`` mode so
one process retains the lifecycle lock throughout imaging::

  truncate -s 8G cm4-emmc.img
  sudo python3 contrib/raspi4/cm4_mass_storage.py serve cm4-emmc.img

The helper prints a stable ``/dev/disk/by-id`` imaging target.  Select that
exact target in Raspberry Pi Imager, or write the unchanged release image to
it with another standard imaging tool.  After the tool has written, verified,
and flushed the target, send exactly ``complete`` followed by Enter on the
helper's stdin.  Send ``failed`` when the imaging tool reports an error.  EOF,
SIGTERM, and invalid acknowledgements tear down the LUN as a failed flash.  A
second QEMU or helper process cannot acquire the sibling lock while ``serve``
is active.

The split commands remain available for interactive diagnostics and fault
injection::

  sudo python3 contrib/raspi4/cm4_mass_storage.py start cm4-emmc.img
  sudo python3 contrib/raspi4/cm4_mass_storage.py status
  sudo python3 contrib/raspi4/cm4_mass_storage.py stop

The stop command refuses to disconnect if the virtual disk or any partition
is mounted.  Once stopped, boot the same ``cm4-emmc.img`` with the
``raspi-cm4`` command above.  QEMU and the mass-storage gadget must never own
the image concurrently.

For a deterministic live transport-loss test, arm the fault watcher after
``start`` and before starting the imaging tool::

  sudo python3 contrib/raspi4/cm4_mass_storage.py \
      fault-disconnect --after-sectors 8192 --timeout 30

The watcher resolves only the gadget's stable by-id device, snapshots its
kernel block-write sector counter, and unbinds/removes the gadget once at
least the requested number of additional sectors have been issued.  It still
refuses teardown if the exported disk is mounted.  The imaging tool must
report failure, and the partial backing file remains available for inspection
or a modeled power cycle.  Re-run ``start`` to test recovery.

For a SCSI-level fault that keeps the USB device enumerated, use the same
threshold with ``fault-eject``::

  sudo python3 contrib/raspi4/cm4_mass_storage.py \
      --lifecycle work/cm4-provision.state \
      fault-eject --after-sectors 8192 --timeout 30

This writes the kernel mass-storage LUN's ``forced_eject`` control.  The stable
USB block identity remains present, but reads return ``EIO`` and Imager must
fail because the SCSI medium is no longer present.  The helper publishes
``flash-failed`` and deliberately leaves the empty LUN enumerated for
inspection.  Finish with ``stop --result failed``; a failed state cannot be
promoted directly to ``boot-ready``.

To inject medium loss immediately after a completed host cache flush, arm the
flush counter instead of the write-sector counter::

  sudo python3 contrib/raspi4/cm4_mass_storage.py \
      --lifecycle work/cm4-provision.state \
      fault-flush-eject --after-flushes 1 --timeout 30

The watcher uses Linux block-stat field 16, which counts completed flush
requests, and then uses the same SCSI forced-eject control.  This models loss
at the post-flush boundary; it does not claim that the already completed flush
failed.  The privileged gate drives the boundary with a real ``fsync``, keeps
USB enumerated, and proves the following read fails with ``EIO``.

Command-specific USB/SCSI fault target
--------------------------------------

The Linux build also produces ``build/qemu-rpi-cm4-msd``.  This Raw Gadget
target implements USB Mass Storage Bulk-Only Transport in userspace, so it
observes the CDB that configfs normally handles inside the kernel.  Its clean
path serves INQUIRY, sense, capacity, mode-page, read, write, FUA, cache-sync,
verify, and LUN commands against the exact regular-file eMMC backend::

  ninja -C build qemu-rpi-cm4-msd
  build/qemu-rpi-cm4-msd --self-test
  sudo build/qemu-rpi-cm4-msd \
      --image cm4-emmc.img \
      --lifecycle work/cm4-provision.state \
      --fault synchronize-cache --fault-after 1

The named faults are ``synchronize-cache``, ``write-fua``, ``bot-phase``,
``bot-timeout``, ``bot-data-reset``, and ``bot-write-reset``.  An arbitrary
opcode can instead be selected as ``opcode:0xNN``.  Starting with the declared
matching-command count, command faults return CHECK CONDITION with ``Medium
Error / Write error`` while retaining the USB enumeration, stable serial, and
ordinary nonmatching I/O.  A first injected command failure atomically
publishes ``flash-failed`` while the process continues to own the lifecycle
lock.  BOT transport faults additionally accept ``--fault-count COUNT``;
the default remains one, and the target stops injecting after that bounded
number of recoveries.  The two data-reset faults accept
``--fault-bytes BYTES`` from 1 through 131,072; omitting it preserves the
512-byte boundary.

``bot-phase`` returns BOT phase-error status for a TEST UNIT READY command.
EP0 remains live in a separate control loop while bulk traffic is active.  On
Linux, ``usb-storage`` responds with its preferred USB port reset, the target
waits for host reconfiguration, and bulk processing resumes.  It also accepts
the class-specific Mass Storage Reset fallback; the gate drives that request
eight times back-to-back through ``sg_reset --device --no-escalate`` without
intervening I/O.  The recovered disk uses the same by-id identity, and INQUIRY
plus a block read after the reset burst must succeed.

``bot-timeout`` consumes a TEST UNIT READY CBW but deliberately returns no
data or CSW.  EP0 remains responsive while the host command waits.  The real
Linux gate lets that command reach its normal timeout, observes the resulting
USB port reset and SET_CONFIGURATION sequence, and requires the stable disk,
INQUIRY, and ordinary reads to recover before flashing continues.

``bot-data-reset`` arms only on READ(16).  It transfers exactly the
``--fault-bytes`` prefix (512 bytes by default), leaves the rest of that data
phase pending, and waits with EP0 live.
The gate repeats this on four distinct 4 KiB reads through ``sg_raw`` on one
target enumeration, issues ``sg_reset`` only after each boundary is reported,
and requires every old command to fail.  After every host reset sequence, the
target discards the old transaction without sending its CSW; a fresh INQUIRY
and read must succeed on the same identity before the next cycle.

``bot-write-reset`` is the symmetric WRITE(16) fault.  It receives and writes
exactly the configurable ``--fault-bytes`` prefix (1 through 131,072 bytes),
durably flushes that prefix to the eMMC backend, leaves the remainder of the
probe untouched, and then waits for reset.  The old command emits no stale
CSW.  The gate repeats four cycles at distinct LBAs with a 128-byte cutoff and
requires byte-for-byte comparison of each 4 KiB backend region, four durable
sub-sector prefixes, and four untouched 3,968-byte suffixes.  Stable identity,
INQUIRY, and ordinary reads recover between cycles in the real Raw Gadget
gate.  When the durability cutoff is smaller than one 512-byte high-speed USB
packet, the target accepts the complete host transfer unit but syncs only the
configured prefix.  The compiled self-test separately proves an exact
127-byte durable prefix at a nonzero backend offset.

The Linux build additionally produces ``qemu-rpi-cm4-bot-probe`` when
libusb is available.  This independent host-side probe detaches
``usb-storage`` and executes all thirteen USB-IF host/device data-direction
and length cases with TEST UNIT READY, INQUIRY, and non-mutating VERIFY data.
The target applies the required short transfer, data/status halt, CSW residue,
passed/phase-error status, and Reset Recovery behavior.  The probe then sends
five valid-but-meaningless CBWs covering reserved flags, unsupported LUN,
reserved length values, and an opcode/length disagreement.  The target's
documented deterministic policy returns a failed CSW and remains ready even
though USB-IF deliberately leaves this response unspecified.  The probe then
sends a reproducible 128-case CBW/SCSI safety corpus.  Its fixed seed
``0x52504934`` and locked FNV-64 digest ``2e5f4d8d3041b5d7`` cover every CDB
byte, all legal CDB lengths, both legal direction-bit values, arbitrary tags,
and unsupported opcodes while keeping the declared data length zero.  Every
case must return a matching failed CSW; an ordinary command is interleaved
every sixteen cases, and the functional gate proves the eMMC SHA-256 is
unchanged before reattaching ``usb-storage``.  The probe also
sends one 31-byte CBW with an invalid signature and one short 30-byte CBW and
requires both Bulk-In and Bulk-Out to stall.  For every phase error and
invalid CBW it performs the USB-IF Reset Recovery order: Mass Storage Reset,
clear Bulk-In halt, then clear Bulk-Out halt.  A fresh TEST UNIT READY and
INQUIRY must succeed before the kernel driver is reattached; the stable by-id
disk and an ordinary kernel read must then recover.

The privileged gate sends exact SYNCHRONIZE CACHE(10) and FUA WRITE(10) CDBs
through Linux ``usb-storage`` and ``sg_raw``.  Both fail while a following
ordinary read succeeds.  It also runs the pinned, unchanged official Imager
against a small raw image and requires Imager to report its real
``Error flushing data to storage device`` path without USB disappearing.
The target also supports a clean foreground ownership mode.  With
``--foreground-owner`` it reads an exact ``complete`` or ``failed`` command
from stdin, quiesces command processing, durably flushes the backend, and
publishes ``boot-ready`` only after successful completion.  Configfs remains
the compatibility default while data-bearing and coverage-guided CBW/SCSI
fuzzing, long-duration/concurrent reset storms, modeled-console attachment,
and additional timing/throughput validation remain open.

Official second-stage USB identity boundary
-------------------------------------------

The unchanged pinned ``mass-storage-gadget64/boot.img`` is the authority for
the production second-stage identity.  Its embedded
``/usr/local/bin/configure-gadgets`` creates a Broadcom ``0a5c:0104`` USB 2.0
device, manufacturer ``Raspberry Pi``, product
``Raspberry Pi multi-function USB device``, and configuration
``Config 1: ACM+MSD gadget`` with ``MaxPower=250``.  It adds a writable,
cache-flushing mass-storage function for every discovered storage device,
sets each LUN inquiry string to the Linux device name (for CM4 eMMC,
``mmcblk0``), and adds an ACM serial interface.

The Raw Gadget target implements the one-eMMC form of this profile directly.
It enumerates MSD first on interface 0, then CDC ACM control/data on interfaces
1 and 2, with the expected endpoint allocation, bus-power declaration,
official strings, and ``mmcblk0`` inquiry field.  CDC line coding,
control-line state, and break requests are retained; its data endpoints use a
deterministic loopback until they are attached to the modeled boot console.
The privileged functional gate asserts the complete ``lsusb`` profile,
stable storage identity, SCSI inquiry, `/dev/ttyACM*` identity, and a 115200
round-trip before and after BOT Reset Recovery.  The configfs helper remains a
portable storage-only compatibility backend.

The production supervisor still uses the unchanged ``bootcode4.bin``,
``config.txt``, ``boot.img``, Imager, and OS image and verifies their pinned
hashes.  On its default continued-guest path it resolves the sole
``/dev/ttyACM*`` below the same qualified dummy-hcd USB device as the eMMC,
sets 115200 raw host transport, and requires a unique host token to traverse
guest ``ttyGS0`` receive and transmit before and after Imager traffic.  The
standalone Raw Gadget target retains deterministic loopback compatibility;
physical descriptor/timing comparison remains a Pass 2 gate.

Automatic production-flow supervisor
------------------------------------

``cm4_provision.py`` drives the clean production sequence as one fail-closed
operation.  It powers QEMU into nRPIBOOT, verifies the QMP/lifecycle release,
runs unchanged official ``rpiboot`` through QEMU's own modeled DWC2 and
behavioral BCM2711 ROM, verifies the exact three received file sizes and
SHA-256 values through QMP, and keeps that same VM alive while the unchanged
received ``boot.img`` starts its Linux ACM+MSD gadget.  The supervisor selects
only the stable ``0a5c:0104`` dummy-hcd whole disk with the exact Raspberry Pi
product, ``mmcblk0`` inquiry identity, and requested eMMC capacity.  It runs
unchanged official Imager with verification enabled against that continued
guest, flushes the LUN, asks the lock-owning QEMU instance to publish
``boot-ready``, and then starts QEMU on the exact same eMMC bytes.
``--handoff-only`` stops
after proving the ARM firmware handoff;
without it the supervisor waits for the post-flash QEMU process, so serial or
other QEMU arguments can be passed with repeated ``--post-qemu-arg=VALUE``::

  python3 contrib/raspi4/cm4_provision.py \
      --qemu build/qemu-system-aarch64 --ram 2G \
      --eeprom artifacts/pieeprom.bin \
      --emmc work/cm4-emmc.img --emmc-size 4GiB \
      --raw-gadget build/qemu-rpi-rpiboot-raw-gadget \
      --dwc2-proxy build/qemu-rpi-dwc2-raw-gadget-proxy \
      --rpiboot artifacts/usbboot/rpiboot \
      --boot-dir artifacts/mass-storage-gadget64 \
      --imager artifacts/Raspberry_Pi_Imager-cli.AppImage \
      --image artifacts/raspios-lite.img.xz \
      --manifest artifacts/cm4-provision-manifest.json \
      --workdir work/cm4-provision

In-process RPIBOOT and its continued Linux ACM+MSD gadget are the default.
The foreground configfs and command-visible Raw BOT owners remain available
only with explicit ``--rpiboot-mode helper`` compatibility mode; add
``--mass-storage-mode raw-bot --raw-msd build/qemu-rpi-cm4-msd`` to select
the latter.

For the bounded reset fault campaign, add ``--guest-reset-count 4``.  After
Imager verification and flush, each iteration derives the exact USB bus and
device number from the already-qualified eMMC sysfs ancestry, invokes
``usbreset`` only for that device, requalifies the complete USB/SCSI identity,
requires a direct 1 MiB host SCSI read to byte-match the QEMU eMMC backend,
and repeats the bidirectional guest ACM echo.  Zero remains the production
default so ordinary flashing does not inject a reset.

The default bounded operation timeout is 60 seconds so the unchanged 8 GiB
guest has enough time to initialize its full memory map and bind the composite
gadget.  A single run covers 1/2/4/8 GiB using one identical hash-pinned
EEPROM, RPIBOOT file set, ``boot.img``, Imager binary, payload, and eMMC
capacity.

The versioned manifest schema name is ``qemu-rpi-cm4-provision-v1``.  Its
``artifacts`` object must contain exactly ``eeprom``, ``rpiboot``,
``bootcode4.bin``, ``config.txt``, ``boot.img``, ``imager``, and ``image``;
each record has the trusted lowercase ``sha256`` and optional byte ``size``.
The image record additionally requires ``payload_sha256``, the decompressed
digest passed to Imager's own verifier.  The EEPROM record additionally
requires ``bootsys_sha256`` for the exact first section, independently pinned
by the release oracle, ``bootsys_key_index`` for its embedded BCM2711 ROM key
selector, plus ``dependencies_sha256`` for the ordered name-and-hash set rooted
by that section.  The supervisor validates the public BCM2711 signed envelope,
safely decompresses the LZ4 ``bootmain``, ``mcb.bin``, ``memsys*.bin``, logo,
and font sections, recomputes their SHA-256 values, and requires each value to
occur in the signed ``bootsys`` payload.  It reports the independently observed
key index and passes only the trusted ``bootsys`` digest to QEMU.  This trust
manifest is supplied by the release process, not generated from the files
being tested.

The official EEPROM source remains immutable.  The supervisor verifies it,
copies it byte-for-byte into a private writable SPI backend, verifies the copy
again, and passes only that copy to QEMU.  It emits an atomic JSON report with
all observed hashes and completed ownership events.  Any command, hash,
lifecycle, QMP, or cleanup failure prevents the next owner from starting.

The transition is now automatic at the host boundary.  QEMU DWC2 implements
device registers, endpoint
control/interrupts, reset, migration, host-token DMA, and the versioned
``device-chardev`` transport.  The behavioral VideoCore ROM owns ROM
enumeration, boot-message receipt, bulk bootcode DMA, exact bootcode hashing,
and ROM status.  Second-stage re-enumeration, get-size/read/done
requests, segmented file DMA, and exact ``config.txt``/``boot.img`` hashes.
The Linux build produces ``qemu-rpi-dwc2-raw-gadget-proxy``, a packet-only
bridge from Raw Gadget to that transport.  It forwards USB lifecycle, EP0,
and bulk OUT tokens and owns no descriptors, filenames, file bytes, or
RPIBOOT state.  An announced-length barrier retains bus ordering when Linux
delivers control and bulk work on separate threads.
Linux Raw Gadget consumes the physical host's ``SET_ADDRESS`` request inside
the kernel and does not surface it as a userspace control event.  Before the
first nonzero ``SET_CONFIGURATION``, the packet proxy mirrors that hidden
address stage into modeled DWC2 and completes its status IN transaction.
QEMU therefore retains the real rule that configuration at address zero is
invalid while current Linux hosts can enumerate the forwarded device.

An opt-in functional gate runs unchanged official ``rpiboot`` commit
``87d6e032`` through real ``dummy_hcd``/Raw Gadget into the in-process ROM.
Both enumerations complete, and QEMU reports the exact pinned sizes and
SHA-256 values for the 105,984-byte ``bootcode4.bin``, 238-byte ``config.txt``,
and 29,360,640-byte ``boot.img``.  The completed image is then opened directly
from that received memory buffer as transient partitioned firmware media.
QEMU resolves the unchanged ``start4.elf``, ``fixup4.dat``, ``kernel8.img``,
``rootfs.cpio.zst``, CM4 DTB, and overlays and reaches the ARM64 handoff while
leaving the configured eMMC backend separate and unchanged as the Linux
gadget's flash target.  No temporary disk, extracted replacement, or direct
``-kernel``/``-initrd`` argument is used.  VMState v30 also carries the active
ROM phase and partially received dynamically sized buffers; a qtest migrates
halfway through bootcode and resumes through the destination DWC2 socket.
``--reset-after 4096`` provides a deterministic real-host reset checkpoint
inside each enumeration.  The privileged reset gate invokes ``usbreset`` at
that boundary in both ``bootcode4.bin`` and ``boot.img``; generation-tagged
proxy traffic discards stale data, the QEMU ROM rolls back to the appropriate
stage entry, unchanged ``rpiboot`` re-enumerates, and all final hashes match.
``--reset-count 4`` repeats that boundary four times per enumeration in one
proxy/QEMU run.  The production gate therefore covers eight active-transfer
resets before exact completion; omitting the option retains one reset per
stage.
The production supervisor uses this same in-process path by default and
drains proxy diagnostics continuously so logging cannot backpressure USB.
Its privileged report requires the received image to reach ARM handoff before
keeping that VM alive for official-profile ACM+MSD enumeration, unchanged
Imager write/verify/flush, and post-flash ARM handoff from the same 4 GiB eMMC
in one fail-closed invocation.  The report binds the continued guest's block
and ACM targets to one qualified ``0a5c:0104`` sysfs ancestry and requires
bidirectional ACM echo both before Imager starts and after its complete
write/verify/flush sequence.
``--disconnect-after 4096 --disconnect-stage rom|file-server`` instead closes
the active transport with intentional-fault status 75.  QEMU observes the
chardev close, drops the partial capture, and accepts a clean proxy reconnect
without a VM restart.  ``--second-stage-only`` reconnects directly to a
surviving file server after a second-stage cable loss.  The privileged
disconnect gate proves both failure boundaries and exact clean retries.
``--hold-after 4096 --hold-stage rom|file-server`` stops after QEMU has
accepted the exact prefix and waits for external termination.  The same gate
reads ``rpiboot-transfer-received=4096``, SIGKILLs the proxy process group,
requires the chardev-close rollback to publish zero received bytes, and
completes an exact full or second-stage-only retry against the same VM.
When ``provision-state-file`` is configured, QEMU retains the lifecycle lock
for this in-process path and publishes ``qemu-rpiboot-wait`` before a proxy
claim, ``rpiboot-active`` on DWC2 connect, ``rpiboot-failed`` on an unexpected
mid-transfer close, active again on retry, and ``rpiboot-complete`` only after
the final protocol acknowledgement.  Completion remains durable when QEMU
shuts down; exiting before any proxy claim retains the external supervisor's
``rpiboot-host-ready`` handoff.
``--timeout rom-status|file-request`` withholds the selected control response
for 21,000 ms by default.  Values must exceed official ``rpiboot``'s 20,000 ms
deadline and are capped at 60 seconds.  After the deadline the proxy exits
with status 75, QEMU rolls back on chardev closure, and the timeout gate
reconnects a clean proxy to the same VM.  Both official host failures and
exact clean retries are required.  ``--bulk-timeout-after 4096
--bulk-timeout-stage rom|file-server --bulk-timeout-ms 5001`` independently
withholds the bulk completion at an exact byte boundary beyond the official
5,000 ms bulk deadline.  The same gate covers both enumerations and requires
same-VM exact retries after each intentional failure.
Opening the image from QEMU and configfs simultaneously is never valid.

For controller development, attach the framed transport to a local Unix
socket with::

  -chardev socket,id=dwc2dev,path=/run/user/$UID/qemu-dwc2.sock,server=on,wait=off \
  -global dwc2-usb.device-chardev=dwc2dev

The 16-byte little-endian request/response format and bounded 64-KiB payload
are defined in ``include/hw/usb/dwc2-device-transport.h``.  The qtest suite
proves fragmented framing, lifecycle events, EP0 SETUP DMA, bulk OUT and IN
DMA, completion interrupts, NAK responses, suspend/resume, and disconnect.
Suspend raises ``GINTSTS.USBSUSP``, sets ``DSTS.SUSPSTS``, and blocks tokens
without losing endpoint/FIFO state.  That state migrates; resume raises the
W1C wakeup interrupt and exact traffic continues.  USB reset and disconnect
both leave the device nonsuspended.  A separate
device-PIO test clears ``GAHBCFG.DMAEn`` and proves receive-status pop
semantics, exact little-endian OUT/SETUP payload words, per-endpoint IN Tx
FIFOs and free-space reporting, FIFO-empty interrupts, bounded overflow,
selective flush, reset, and migration with queued payloads.  EP0 consumes its
programmed SETUP count and remains armed for a three-deep back-to-back SETUP
window.  The first request can be split across live migration and the next two
complete without guest re-arming; residual bytes and ``SUPCNT`` reach zero
exactly.  Zero-length IN/OUT status stages complete without FIFO data.
Full-plus-short multi-packet bulk transfers preserve residual accounting and
terminate both directions at the short packet.  DWC2 VMState v7 carries the
partially received framed request to the destination.
A reset-storm companion combines EP0 SETUP with simultaneously armed EP1 and
EP2 IN/OUT PIO traffic, asserts both global NAKs, and repeats USB reset plus
re-enumeration sixteen times.  Every reset clears the NAK blockade, endpoint
state, interrupt summaries, and queued bytes before exact interleaved traffic
resumes.
A host-PIO test independently clears ``GAHBCFG.DMAEn`` and enumerates a live
``usb-kbd`` device using the channel ``HCFIFO`` and receive FIFO rather than
guest DMA.  It migrates a partially staged SETUP packet, proves receive-depth
backpressure and wakeup, migrates the queued descriptor status/payload, checks
that ``HCDMA`` never advances, and covers non-periodic/periodic free-space,
overflow, selective flush, and recovery.  Production firmware-owner and
RPIBOOT transfers explicitly retain DMA, so the unchanged firmware and
boot-file path is not redirected through this test-only mode.
The asynchronous companion test uses a delayed USB-storage backend to hold
one PIO IN packet in flight while a second channel contends for the same
receive FIFO.  It proves byte/status reservation prevents overcommit, then
alternates host and core reset for sixteen cancellation cycles and requires a
clean Mass Storage Reset plus GET_MAXLUN recovery after every cycle.

To run only the unchanged-host/in-process-ROM gate::

  QEMU_RPI_RPIBOOT=/path/to/pinned/usbboot/rpiboot \
  QEMU_RPI_USBBOOT_DIR=/path/to/pinned/mass-storage-files \
  QEMU_RPI_DWC2_PROXY=$PWD/build/qemu-rpi-dwc2-raw-gadget-proxy \
  QEMU_TEST_QEMU_BINARY=$PWD/build/qemu-system-aarch64 \
  build/run tests/functional/aarch64/test_raspi4.py \
      Aarch64Raspi4Machine.test_arm_cm4_inprocess_rpiboot_usb

Replace the final test name with
``Aarch64Raspi4Machine.test_arm_cm4_inprocess_rpiboot_usb_resets`` to run the
active 4,096-byte reset/re-enumeration gate, or
``Aarch64Raspi4Machine.test_arm_cm4_inprocess_rpiboot_disconnect_retry`` for
both exact-byte disconnect and same-VM retry paths, or
``Aarch64Raspi4Machine.test_arm_cm4_inprocess_rpiboot_timeout_retry`` for the
two real 20-second control deadlines and both real 5-second bulk deadlines.

Use one lifecycle file across all phases to enforce that ordering.  Start the
nRPIBOOT machine with::

  -M 'raspi-cm4,boot-mode=behavioral,nrpiboot=on,eeprom-drive=pieeprom,emmc-drive=emmc,provision-state-file=work/cm4-provision.state'

QEMU publishes ``qemu-rpiboot-wait`` and, from its process-exit notifier,
``rpiboot-host-ready``.  After QEMU has exited, pass the file to each helper::

  sudo build/rpiboot-raw-gadget \
      --capture-dir work/rpiboot-capture \
      --lifecycle work/cm4-provision.state --mass-storage
  sudo python3 contrib/raspi4/cm4_mass_storage.py \
      --lifecycle work/cm4-provision.state serve cm4-emmc.img

After acquiring the lock, Raw Gadget publishes ``rpiboot-active`` before it
opens the USB transport, then publishes ``rpiboot-complete`` or
``rpiboot-failed``.  The
mass-storage helper accepts only a completed/retryable state, publishes
``mass-storage-active``, and finishes at ``boot-ready`` or ``flash-failed``.
Foreground ``serve`` retains the advisory lock until its stdin receives the
imaging result; ``stop --result failed`` remains the recovery path for split
mode.  Restart QEMU without ``nrpiboot=on`` but with the
same ``provision-state-file``.  It rejects every state except ``boot-ready``
or ``qemu-stopped``, publishes ``qemu-owned`` before boot, and writes
``qemu-stopped`` on exit.  The contract is opt-in so existing QEMU command
lines remain compatible; the production gate requires it.  All three owners
also take an exclusive advisory lock on ``cm4-provision.state.lock``.  QEMU
holds it for its entire process lifetime, Raw Gadget holds it across both USB
enumerations, and configfs ``serve`` holds it throughout write/verify/flush.
Thus no helper can act on a release token before its owner has actually
exited.  Privileged helpers preserve the
state file's original UID, GID, and mode when publishing atomic transitions,
allowing normal-user QEMU to reacquire it safely.

If Raw Gadget is killed while either RPIBOOT enumeration is active, retry is
blocked by the retained ``rpiboot-active`` token.  After confirming the old
process is dead, recover that boundary explicitly::

  sudo build/rpiboot-raw-gadget \
      --lifecycle work/cm4-provision.state --recover-stale

Recovery first obtains the same sibling lock and accepts only
``rpiboot-active`` or an already failed retry.  It can only publish
``rpiboot-failed``; it never advances to completion.  A live helper therefore
wins the lock, and wrong lifecycle phases remain untouched.  The production
supervisor performs this fail-closed recovery automatically after terminating
a failed or timed-out Raw Gadget child.

After an uncatchable foreground-owner crash, recover the configfs boundary
without ever promoting uncertain media::

  sudo python3 contrib/raspi4/cm4_mass_storage.py \
      --lifecycle work/cm4-provision.state recover-stale

The command first obtains the same sibling lock, accepts only
``mass-storage-active`` or an already failed retry, refuses mounted exports,
tears down any remaining gadget, and publishes ``flash-failed``.  If kernel
teardown already completed before the old process died, the absent gadget is
accepted but the result is still failed.  Teardown failure retains
``mass-storage-active`` for another safe retry; a live owner wins the lock and
is not disturbed.

If post-flash QEMU is killed before its exit notifier runs, the state remains
``qemu-owned``.  Restart remains fail closed unless the operator explicitly
adds ``provision-recover-stale=on``.  QEMU first acquires the sibling lock,
then recovers only that stale QEMU state and reports
``provision-recovery=stale-qemu-owned``.  It never promotes
``flash-failed`` or ``mass-storage-active``.  A rejected startup also leaves
the existing lifecycle token untouched because QEMU releases only ownership
that its own process successfully published.

This mode rejects direct kernel/DT/firmware arguments, holds all ARM cores
until the firmware boundary is validated,
reads an optional persistent 512-byte/66-row OTP backend, validates public
bootmode and board rows, samples OTP-gated ``nrpiboot`` at reset, searches raw
FAT12/16/32 SD bytes for ``recovery.bin`` before EEPROM, parses
``bootconf.txt`` from the EEPROM section table, and executes ``BOOT_ORDER``
least-significant nibble first.  EEPROM configuration applies ``[all]`` and
``[pi4]`` to both Pi 4B and CM4, then applies later ``[cm4]`` overrides only
on ``raspi-cm4``; unknown or nonmatching filter sections remain inactive.
``[partition=N]`` reads the six-bit boot partition decoded from PM_RSTS, and
``[gpioN=0|1]`` samples an explicitly driven BCM2711 input at reset.  A
serial filter such as ``[0x12345678]`` reads OTP row 28, and
``[board-type=0x11]``/``[board-type=0x14]`` distinguish Pi 4B from CM4 using
the new-style revision code.  The documented ``[ARG=VALUE]``, ``[ARG&MASK]``,
``[ARG&MASK=VALUE]``, ``[ARG<VALUE]``, and ``[ARG>VALUE]`` grammar operates
on ``partition``/``boot_partition`` and the eight persistent
``cust_otp0``...``cust_otp7`` rows.  Pi-5-only or otherwise unavailable
variables fail closed.  Filters of the same type replace each other;
model, EDID, serial, GPIO, and expression filters combine until ``[all]``
resets them.  ``[none]`` remains disabled until that reset.  A high-impedance,
malformed, out-of-range, unknown, or nonmatching condition is inactive.  This
lets one unchanged EEPROM image carry common policy plus platform-, identity-,
reset-, and fixture-selected boot order or retry settings.  Linux's production
``SET_REBOOT_FLAGS`` mailbox ABI is implemented: flag bit 0 selects
``tryboot.txt`` for exactly the next boot, is cleared before firmware handoff,
survives migration while pending, and then returns to ``config.txt`` on the
following reset.  ``GET_REBOOT_FLAGS`` and ``NOTIFY_REBOOT`` use the same
property-channel path as the upstream kernel reboot notifier.  In secure mode
the same flag selects signed ``tryboot.img``/``tryboot.sig`` instead of
``boot.img``/``boot.sig`` for block and network sources, and the mounted
ramdisk implicitly returns to its own ``config.txt``.  EEPROM ``BOOTVAR0`` is
propagated into ``config.txt`` as ``bootvar0`` with equality, mask,
masked-equality, less-than, and greater-than filters.  It validates
non-empty SD ``start4.elf`` or
``start.elf`` candidates together with the paired fixup, kernel, BCM2711 DTB,
and configured cmdline/initramfs.  The raw FAT reader supports short and VFAT
long names plus nested directories.  A bounded ``config.txt`` subset handles
model/board-type, EDID, OTP serial, GPIO, and boot-variable expression
filters.
An EEPROM ``bootconf.txt`` ``[config.txt]`` section contributes every byte
after its header as an ordered suffix to that same parser.  Filter state is
not reset at the media/EEPROM boundary, includes still resolve from the
selected boot source, and the EEPROM suffix can supply configuration even
when the media file is absent.  QOM reports the exact suffix size and SHA-256,
and behavioral VMState v61 preserves the bytes across a pending live
migration.  A production functional gate uses the pinned official
``rpi-eeprom-config`` tool to generate the configured 512 KiB EEPROM and then
boots byte-unchanged production start/fixup/kernel/DTB/initramfs artifacts.
Filters of the same category replace while different categories combine;
``[all]`` resets them and ``[none]`` remains inactive until that reset.
Includes retain the surrounding filter state and any filter changes made by
the included bytes remain active when parsing resumes in the caller, matching
textual insertion.  High-impedance GPIO and unavailable variables fail
closed, and ``[tryboot]`` reads the one-shot reboot state.
EEPROM ``BOOT_UART=1`` enables a deterministic clean-room bootloader trace on
the primary PL011 UART0 through QEMU serial0.  Pi 4B and CM4 select
GPIO14/GPIO15 ALT0 and program 115200 8N1 before reporting boot-order entry,
source attempts, and second-stage handoff.  The option defaults to zero and
only zero or one is accepted.  Query ``boot-uart-enabled``,
``boot-uart-active``, ``boot-uart-bytes``, ``boot-uart-lines``, and
``boot-uart-format`` for the modeled state and output counters.  VMState v62
retains active ownership and counters without replaying prior bytes.  If
``uart_2ndstage=1`` is also selected, its firmware trace takes ownership after
bootloader handoff.  The clean-room record is not a claim of byte-for-byte
private bootloader text or physical UART cadence.
``hdmi0-edid-file`` and ``hdmi1-edid-file`` accept unchanged raw EDID bytes
(including Linux DRM sysfs ``edid`` files), validate every declared 128-byte
block, and derive the documented ``[EDID=MANUFACTURER-Product]`` name at
reset.  Either Pi 4 HDMI port may match; empty or invalid data cannot.  The
sampled identities migrate and are resampled on reset.  The same identity
inputs drive SD, eMMC, TFTP, and HTTP-derived
configuration.  Boot-file selection, ``os_prefix``, 32/64-bit kernel
defaults, and ordered multi-file initramfs selection are supported; overlays
and parameters are processed in order.  Requested ``.dtbo`` files are loaded unchanged from the
raw FAT volume; the supported ``__overrides__`` subset covers strings/status,
8/16/32/64-bit offsets, byte strings, booleans, textual literals, and fragment
toggles before standard libfdt merging.  A descriptor ending in ``=`` consumes
its following binary cell after libfdt has resolved local or external
phandles, matching the two-pass behavior needed by official overlays.  Lookup
tables support exact keys, key-as-value entries, quoted values, defaults,
pass-through entries, and local or external binary-cell values.  Assigning
``reg`` also rewrites the node's hexadecimal unit address, while the
pseudo-property ``name`` renames the node.  Enabled fragments that assign
``bootargs`` use the Pi firmware's append rule for path, direct-phandle, and
symbol-fixed targets, preserving base arguments that vanilla libfdt would
replace.  Multiple assignments in one overlay accumulate in fragment order.

Bootloader-owned firmware shortcuts must be placed in the top-level
``config.txt``; an included file cannot set them.  ``start_x=1`` selects the
Pi 4 pair ``start4x.elf``/``fixup4x.dat`` and falls back as a pair to
``start_x.elf``/``fixup_x.dat``.  ``start_debug=1`` selects
``start_db.elf``/``fixup_db.dat``.  ``gpu_mem_256``, ``gpu_mem_512``, and
``gpu_mem_1024`` are also top-level-only.  The installed-memory-specific
value overrides ``gpu_mem``; all supported 1--8 GiB Pi 4B/CM4 models select
``gpu_mem_1024`` while the smaller selectors remain inactive.  The effective
value defaults to 76 MiB, has a 16 MiB minimum, and controls the same
low-memory reservation in the final DT, framebuffer, and firmware ARM/VC
memory mailbox responses.  Inspect it with ``firmware-gpu-mem-mb`` and
``firmware-gpu-mem-source``.  An effective ``gpu_mem=16`` selects
``start4cd.elf``/``fixup4cd.dat``.  An explicit paired
``start_file``/``fixup_file`` overrides these shortcuts; select cut-down
firmware with ``gpu_mem=16`` instead of directly naming the ``*cd`` pair.
The rules are identical for SD/eMMC, USB/NVMe, and network boot corpora.

Top-level ``total_mem=N`` limits firmware-visible capacity in MiB.  Values are
clamped to 128 MiB through the installed 1/2/4/8 GiB Pi 4B or CM4 model, then
the final DT lower and above-1-GiB memory banks are regenerated from that
effective value.  The installed backend and board revision do not change.
``firmware-total-mem-mb`` reports the effective capacity; included-file
settings are ignored.  This provides one reproducible machine invocation for
testing multiple constrained-memory configurations without changing the
production boot binaries.

Top-level ``bootcode_delay=N`` waits N seconds of virtual time after
configuration parsing and before loading firmware artifacts.  The behavior is
shared by SD/eMMC, USB, NVMe, and network configuration, re-samples HDMI EDID
at expiry, re-arms after reset, and migrates with its exact remaining time.
Included settings are ignored.  Query
``firmware-bootcode-delay-seconds`` to inspect the active value.

Top-level ``sdram_freq=N`` is observable as the requested MHz value, but it
does not alter the Pi 4B/CM4 clock: BCM2711 SDRAM remains at the documented
non-configurable 3200 MHz rate for all memory capacities.  Query
``firmware-sdram-frequency-requested-mhz``,
``firmware-sdram-frequency-requested``, and
``firmware-sdram-frequency-mhz`` to compare requested and effective values.
Included settings are ignored.

Top-level ``uart_2ndstage=1`` enables the behavioral firmware trace on
PL011 UART0, available through QEMU serial0 (for example,
``-serial file:firmware-uart.log``).  QEMU selects GPIO14/GPIO15 ALT0,
programs 115200 8N1, and emits a stable clean-room record covering enablement,
selected source/artifacts, and ARM handoff.  Query
``firmware-uart-2ndstage``, ``firmware-uart-2ndstage-bytes``, and
``firmware-uart-2ndstage-lines`` for state and delivery counters.  Reset emits
a fresh record and migration does not replay prior bytes.  Included values
are ignored; values other than zero or one are rejected.  This diagnostic
record intentionally does not claim exact private VideoCore text or physical
UART timing.

BCM2711 secure mode consumes the same signed artifacts as the Raspberry Pi
tools.  OTP rows 47-54 must match SHA-256 of EEPROM ``pubkey.bin``;
``bootconf.sig`` must authenticate ``bootconf.txt`` with RSA-2048 PKCS#1 v1.5
SHA-256.  Each selected block medium must then supply an authenticated
``boot.img``/``boot.sig`` pair.  Only the verified image is opened as a
read-only in-memory FAT volume and passed to firmware processing.
``secure-boot-status`` reports exact key, format, digest, RSA, and image
boundaries, and VMState v28 retains the verified key, network-install policy,
and HTTP transaction,
sparse receive-window validity map, and FIN sequence through pending boot waits.  The positive qtest
signs the exact deterministic FAT bytes consumed by
the machine and reaches the ordinary ARM handoff without repacking files.
Secure network mode fetches ``boot.sig`` followed by those same ``boot.img``
bytes through GENET/TFTP and rejects a wire-delivered signature mutation.
Mode 7 can fetch the same ordered pair from a validated custom HTTP host using
real GENET DHCP/ARP/TCP frames.  It requires an HTTP 200 response with one
bounded ``Content-Length``.  The wire qtest deliberately drops the initial SYN
and GET and proves exact-sequence retransmission at the 500 ms boundary;
a second test migrates with a partial signature and two noncontiguous future
TCP ranges, including the final FIN, then drains both gaps and completes on a
replacement network backend.  A final FIN after the exact declared body is
acknowledged, and the client closes every complete response with its own FIN.
Premature FIN, a rejected 302 redirect, and a 404 response prove immediate
BOOT_ORDER fallback.  For an unchanged mode-7 configuration with no
``HTTP_HOST``, QEMU selects
``fw-download-alias1.raspberrypi.com:443`` and creates peer-verifying client
credentials in memory from the embedded Raspberry Pi intermediate CA.  It
does not use the host CA store or require an operator object.
``http-tls-creds`` remains an optional peer-verifying ``tls-creds-x509``
test/lab override.  TLS records travel inside the modeled TCP stream and
the requested hostname is sent as SNI before decrypted response bytes enter
the same bounded parser.  The official
network-install RSA public key is embedded for the final
``boot.sig``/``boot.img`` check.  A qtest proves the unchanged
EEPROM-to-DHCP/DNS/ARP/TCP sequence, drops and byte-compares a retransmitted
ClientHello, completes X.509 hostname verification, and decrypts the exact
first GET.  A separate qtest proves that the built-in CA path reaches its
ClientHello with the override unset.  It returns an encrypted HTTP body and
proves close-notify, TCP FIN, and the next artifact connection.  Invalid or
verify-off override credentials, custom CA policy on BCM2711, and
established-session migration fail closed.  BCM2711
secure boot without a custom ``HTTP_HOST`` also disables HTTP as required by
the firmware policy.  The official-corpus gate supplies positive signature
evidence because Raspberry's signing private key is unavailable.  The opt-in
``QEMU_RPI_DEFAULT_HOST_LIVE=1`` functional test also drives the unchanged
pinned EEPROM through QEMU user networking and the live official HTTPS service
to ARM handoff.  It checks the outer signed-image hash and the exact hashes of
the selected firmware, fixup, kernel, DTB, and initramfs.  TCP SACK remains a
tracked gap.
EEPROM ``NET_INSTALL_ENABLED`` and ``NET_INSTALL_AT_POWER_ON`` are strict
boolean policy inputs.  ``NET_INSTALL_AT_POWER_ON`` exposes the cold-boot UI
but does not select it.  ``-M raspi4b,net-install-requested=on`` models the
boot-time physical request, invokes mode 7 once when enabled, and resumes the
unchanged configured ``BOOT_ORDER`` from its first nibble after failure.
Read-only QOM properties expose both parsed policy bits; negative qtests cover
disabled requests and the non-selecting power-on setting.
An additional signed EEPROM fixture uses ``HTTP_HOST=boot.test`` and proves the
DHCP/DNS/ARP/TCP path without changing authenticated configuration.

Missing overlays and unsupported parameter encodings fail closed.  It falls
through unavailable or unsupported
sources and represents SD/eMMC retries, terminal waits, STOP, and RESTART limits
without claiming unsupported media succeeded.  ``USB_MSD_STARTUP_DELAY`` adds
its documented pre-enumeration interval without consuming the discovery
budget.  Pi 4B also consumes ``USB_MSD_PWR_OFF_TIME=0..5000`` before first USB
enumeration.  Revisions through 1.3 model the short hardware cycle followed by
the full configurable off interval; revision 1.4 and later overlap the
documented minimum two seconds of reset-held power-off with memory
initialization and wait only for the remaining configured interval.  Zero
skips the legacy configurable cycle.  CM4 reports this Pi 4B-only setting as
not applicable.  USB discovery and per-LUN failures remain pending for their
configured virtual-time intervals.  QEMU
rechecks the backend at each deadline, so media made bootable during the wait
can satisfy the same attempt before fallback proceeds to the next nibble.
``USB_MSD_EXCLUDE_VID_PID`` accepts the documented list of up to four exact
``VIDPID`` hexadecimal identities.  Both xHCI and DWC2 firmware paths compare
the real device descriptor before configuring storage; excluded devices are
skipped, eligible devices remain in deterministic order, and excluding a hub
prunes everything downstream.  QOM reports the canonical policy plus excluded
and eligible counts and the last matched identity.
The USB endpoint is removable and may start empty.  Standard QMP media change
and eject commands notify the SCSI device and behavioral ROM through the same
backend.  Inserted bootable media completes the active attempt immediately;
invalid media enters the LUN wait, and eject returns it to discovery without
charging an unfinished timeout.  The hotplug qtest covers that full sequence
and verifies the unchanged inserted firmware hash at ARM handoff.
The versioned machine migration state carries the current BOOT_ORDER nibble,
counters, pending action, and remaining virtual time.  It re-arms USB startup,
discovery, LUN, restart-cycle, and recovery-reboot waits relative to the
destination clock.  The BCM watchdog VMState v3 similarly carries its armed state and
remaining ticks while retaining older stream compatibility.  Migration qtests
cover every pending action and prove the exact remaining-time boundaries,
including the watchdog RESET and recovery reboot into the programmed EEPROM.
SD card-detect migration and both SD card-detect/infinite-retry insertion paths
are covered separately; no unmeasured SD enumeration delay is invented.
Finite ``MAX_RESTARTS`` executes complete repeated BOOT_ORDER cycles.  Crossing
the threshold arms the modeled BCM watchdog, records HADWRF, performs a real
QEMU reset, and begins a new ROM/EEPROM cycle; infinite restart remains an
explicit terminal observation to avoid an intentional busy loop.
``REBOOT_ON_FATAL_ERROR`` defaults to one and is parsed strictly as a boolean.
An unsupported boot nibble or exhaustion without STOP enters a deterministic
three-pattern wait and then performs a BCM-watchdog hard reset.  Setting the
property to zero preserves the fatal observation until an external power
cycle/reset.  QOM exposes the selected policy, remaining deadline, and a warm
reset-persistent reboot counter.  VMState v60 retains the policy, counter,
source, and exact remaining deadline; the pattern cadence is behavioral and
still requires calibration against a physical bootloader trace.
``PM_RSTS`` survives warm
resets and exposes the raw status, reset cause, and decoded boot partition.
The clean-room recovery boundary supports digest-checked
``pieeprom.upd``/``pieeprom.bin`` updates,
persistent erase/program/readback verification, write protection, and exact
fault boundaries selected with
``eeprom-fail-stage=erase|program|verify|rename|reboot``.  The byte-oriented
stages use ``eeprom-fail-after=N``; rename/reboot faults preserve their exact
FAT/EEPROM ordering.  A successful ``pieeprom.upd`` schedules an automatic
guest reset after 10 ms of virtual time; the next ROM pass skips
``RECOVERY.000``, reads the new EEPROM, and resumes ``BOOT_ORDER``.  The
optional timing controls
``eeprom-erase-sector-delay-us=N``,
``eeprom-program-page-delay-us=N``, and
``eeprom-verify-sector-delay-us=N`` schedule every 4 KiB erase, 256-byte page,
and 4 KiB verification read on the virtual clock.  Reset cancels the active
operation while retaining completed persistent units; a subsequent reset
retries the unchanged recovery files.  Migration carries the copied 512 KiB
update, exact stage/counters, configured delays, and remaining deadline.
``eeprom-flash-elapsed-us`` reports consumed configured latency.  All delays
default to zero for compatibility and require a pinned hardware trace before
being described as physically calibrated.
The production gate carries unchanged official recovery, EEPROM, firmware, DT,
kernel, overlay, and initramfs bytes through that transition on one VM and the
same SD image.  The 10 ms value is deterministic virtual scheduling, not a
physical timing claim.  Valid raw, gzip, and EFI-zboot ARM64 kernels, plus raw
or gzip ARM32 zImages, are loaded into guest RAM with the selected initramfs
and a patched DTB.  ARM64 uses its Image entry and spin table.  ARM32 loads
unchanged ``kernel7l.img`` at ``0x8000``, enters through the firmware register
stub at ``0x0``, and releases secondaries through BCM2711 mailbox 3 at the
``0xff8000cc`` clear base.  Pinned Pi 4B SD and CM4 eMMC gates exercise this
path with their unchanged board-specific DTBs and a pinned unchanged ARMv7
initramfs, then reach four-core ``armv7l`` userspace.  The model
does not execute the VideoCore recovery or firmware binaries, validate the
silicon execution of the Raspberry Pi BootROM ``bootsys`` signature/HMAC
root and revocation policy,
schedule every physical boot-source timing, or yet implement the remaining
Pi-specific intra-overlay and electrical GPIO rules.  Unsupported
secure transports and policy stages fail explicitly instead of booting
unchecked.

HAT EEPROM corpus gate
----------------------

``hat_corpus.py`` runs a hash-pinned set of unchanged HAT ``.eep`` files
through the complete behavioral EEPROM, boot-media, firmware, overlay, GPIO,
and ARM-handoff path for ``raspi4b`` and ``raspi-cm4``::

  python3 contrib/raspi4/hat_corpus.py \
    --qemu build/qemu-system-aarch64 \
    --manifest work/hat-corpus.json \
    --output work/hat-corpus-report.json

The manifest uses schema ``qemu-rpi-hat-corpus-manifest-v1`` and contains a
non-empty ``cases`` array.  Every case has exactly ``id``, ``platform``,
``bootloader``, ``media``, ``hat``, and ``expected``.  Each artifact is a
``{"path": "...", "sha256": "..."}`` object; paths are relative to the
manifest and symlinks are rejected.  ``expected`` pins the boot source,
vendor, product, UUID, product ID/version, custom-atom count, overlay,
overlay count, GPIO status/mask/policy, and final Device Tree SHA-256.

The entire manifest is preflighted before QEMU starts.  Execution uses
snapshot boot media and a read-only HAT backend.  The deterministic atomic
report records the unchanged file size/hash, QEMU's sector-padded backend
size/hash, EEPROM-declared logical size/content hash, parsed identity, GPIO
policy, final Device Tree hash, and ARM-handoff result.  A mismatch exits 1;
an invalid manifest, artifact, EEPROM, QMP response, or execution exits 2.
This is the software differential gate.  Vendor/hardware capture acquisition,
electrical pad current, edge rate, hysteresis voltage, back-power current, and
rail sequencing remain explicit HIL work.

Pi 4 recovery uses the same unchanged signed ``recovery.bin`` as hardware.
The machine validates its public BCM2711 payload-length/key-index/RSA-2048/
HMAC-SHA1 envelope and the upstream 110 KiB limit, then requires the exact
file SHA-256 through ``recovery-trusted-sha256``.  Missing trust, a digest
mismatch, malformed length/key index, empty signature fields, truncation, or
oversize input fails before EEPROM mutation.  ``recovery-sha256`` and
``recovery-key-index`` expose the observed identity.  The pinned digest is the
behavioral oracle for the non-public ROM HMAC key; silicon RSA/HMAC execution
remains a hardware-in-the-loop gate.  Recovery qtests fragment the signed
recovery, exact update, and signature files across FAT12, FAT16, and FAT32;
self-loops and reserved/bad FAT12 and FAT32 links fail before changing the
persistent EEPROM.

EEPROM recovery uses 4 KiB erase sectors and read-before-write 256-byte NOR
page programming: stored bytes become ``old & requested``.  A deterministic
``eeprom-stuck-zero-offset``/``eeprom-stuck-zero-mask`` fault survives erase,
reports the exact violated-bit and completed-page counts, and stops before
verification.  Clearing it and resetting retries the unchanged recovery media.
Write protection follows the board's two-part contract.  Persistent
status-register block protection rejects array erase/program regardless of
pin level.  The active-low physical ``EEPROM_nWP`` input does not protect the
array by itself; low locks changes to that status.  Recovery consumes the
unchanged ``config.txt`` ``eeprom_write_protect=-1|0|1`` policy, leaving,
clearing before flash, or setting after verified flash respectively.  QOM
exposes these as ``eeprom-status-write-protect``, ``eeprom-nwp``, and
``eeprom-nwp-sampled``; ``eeprom-write-protect`` remains a compatibility alias.
``eeprom-status-drive=ID`` optionally binds a dedicated raw 512-byte persistent
status sector (first byte ``0`` or ``1``, reserved bytes zero).  It is
authoritative across independent launches and is synchronously flushed on
each permitted status transition.

The functional suite has three pinned production-artifact gates.  One boots
unchanged official EEPROM/firmware files, an official ``i2c-gpio.dtbo``, and a
real compressed Pi kernel to userspace without direct-loader arguments.  The
other uses official
``recovery.bin`` to flash an unchanged official ``pieeprom.upd`` over an older
official EEPROM image, pins that exact recovery file's SHA-256, and compares
the persistent result byte for byte.  The
third boots ``raspi-cm4`` from a named persistent eMMC backend with unchanged
official EEPROM, firmware, kernel, CM4 DTB, recovery, and initramfs files; the
recovery file is intentionally ignored on eMMC.

Default-host HTTPS trust oracle
-------------------------------

``default_host_gate.py`` seals the trust inputs for the Pi 4 network-install
HTTPS path before the packet-level TLS client consumes them.  It verifies the
unchanged EEPROM image, strict three-line ``boot.sig`` format, declared
``boot.img`` SHA-256, RSA-2048 PKCS#1 v1.5 signature with Raspberry Pi's
published ``imager/net_install_pubkey.pem``, the private CA certificate, and
optionally the live server certificate and
``fw-download-alias1.raspberrypi.com`` hostname.  Every supplied expected hash
is checked before a deterministic JSON report is written::

  python3 contrib/raspi4/default_host_gate.py \
      --eeprom artifacts/pieeprom.bin \
      --eeprom-sha256 "$EEPROM_SHA256" \
      --boot-image artifacts/boot.img \
      --boot-image-sha256 "$BOOT_IMAGE_SHA256" \
      --boot-signature artifacts/boot.sig \
      --boot-signature-sha256 "$BOOT_SIGNATURE_SHA256" \
      --public-key artifacts/net_install_pubkey.pem \
      --public-key-sha256 "$PUBLIC_KEY_SHA256" \
      --ca-certificate artifacts/raspberry-pi-intermediate-ca.pem \
      --ca-certificate-sha256 "$CA_PEM_SHA256" \
      --server-certificate artifacts/default-host.pem \
      --output work/default-host-trust.json

The report records both the certificate-file hash and the canonical DER
fingerprint.  It also records the BCM2711 policy boundary: custom-CA HTTPS is
not supported, and secure boot without a custom ``HTTP_HOST`` disables HTTP
boot.  This gate does not substitute host OpenSSL transport for emulated
GENET/TCP/TLS; it pins the exact key, certificate, signature, and payload
oracle that the wire implementation must satisfy.

The machine exposes read-only ``secure-boot-image-*`` and
``secure-boot-signature-*`` size/SHA-256 properties so this gate can bind the
outer signed container to the inner artifact observations.  To exercise the
live path explicitly::

  QEMU_RPI_DEFAULT_HOST_LIVE=1 \
    QEMU_TEST_QEMU_BINARY="$PWD/build/qemu-system-aarch64" \
    ./build/run tests/functional/aarch64/test_raspi4.py \
    Aarch64Raspi4Machine.test_arm_raspi4_default_host_live

Run the self-contained tests with::

  python3 -m unittest discover -v \
      -s contrib/raspi4 -p 'test_*.py'

Recompute the implementation matrix and fail if its headline is stale with::

  python3 contrib/raspi4/matrix_progress.py --check-dashboard

After configuring QEMU, run the behavioral boundary qtests with::

  ninja -C build qemu-system-aarch64 tests/qtest/raspi4-boot-test
  QTEST_QEMU_BINARY=./build/qemu-system-aarch64 \
      ./build/tests/qtest/raspi4-boot-test

Run the rootless real-server network-boot production gate with an unchanged
EEPROM ``.bin`` configured for ``BOOT_ORDER`` mode 2 and the exact TFTP tree
used by the physical deployment::

  python3 contrib/raspi4/network_boot_manifest.py \
      --eeprom /artifacts/pieeprom.bin \
      --tftp-root /artifacts/boot \
      --output /artifacts/network-boot-manifest.json

  python3 contrib/raspi4/network_boot_gate.py \
      --qemu "$PWD/build/qemu-system-aarch64" \
      --eeprom /artifacts/pieeprom.bin \
      --tftp-root /artifacts/boot \
      --manifest /artifacts/network-boot-manifest.json \
      --pcap-output work/qemu-network-boot.pcap \
      --output work/network-boot-gate.json

To validate a real network EEPROM self-update, explicitly seal the expected
update and opt in to changing the EEPROM backend::

  python3 contrib/raspi4/network_boot_manifest.py \
      --eeprom /artifacts/pieeprom-old.bin \
      --tftp-root /artifacts/boot \
      --eeprom-update device-prefix/pieeprom.upd \
      --output /artifacts/network-update-manifest.json

  python3 contrib/raspi4/network_boot_gate.py \
      --qemu "$PWD/build/qemu-system-aarch64" \
      --eeprom /artifacts/pieeprom-old.bin \
      --tftp-root /artifacts/boot \
      --manifest /artifacts/network-update-manifest.json \
      --allow-eeprom-update \
      --output work/network-update-gate.json

The manifest requires the update to be named ``pieeprom.upd``, exactly 512 KiB,
and accompanied by ``pieeprom.sig`` in the same directory.  The gate permits
no arbitrary EEPROM change: after the automatic reset and ARM handoff, the
backend must match the sealed update hash exactly.  Selecting the manifest
update without ``--allow-eeprom-update``, or opting in without a manifested
update, fails before QEMU starts.  CM4 remains excluded because it uses the
RPIBOOT workflow.  Self-update mode always captures the private TAP internally,
even when no PCAP output was requested.  The report proves the ordered
``pieeprom.upd``/``pieeprom.sig`` requests on both boots, the updater
restart/reset/up-to-date trace, the first following firmware request, final
``boot-self-update-status``, and the exact EEPROM transition.

Expected rejection paths use the same production gate and the same two-key
authorization.  Add either
``--eeprom-update-result invalid-signature`` or
``--eeprom-update-result write-protected`` when creating the manifest, then
run the gate with ``--allow-eeprom-update`` as above.  The invalid-signature
manifest requires a mismatched sibling ``pieeprom.sig``; the write-protected
manifest requires a valid signature and launches the modeled EEPROM with
write protection enabled.  Both gates require exactly one ordered
``pieeprom.upd``/``pieeprom.sig`` wire sequence, the matching QEMU rejection
trace and status, no subsequent TFTP request, and a byte-identical persistent
EEPROM afterward.

A deterministic mid-program interruption uses
``--eeprom-update-result program-failure`` together with
``--eeprom-update-fail-after N``.  The bound is an exact byte offset below
512 KiB.  The gate independently hashes the expected durable NOR boundary:
the first ``N`` update bytes followed by erased ``0xff`` bytes.  It then
requires the matching wire pair and ``program-failed`` observation, no later
request, and that exact persistent partial image.

Run the interrupted-write and clean-retry sequence as one retained production
campaign with::

  python3 contrib/raspi4/network_boot_recovery_gate.py \
      --qemu "$PWD/build/qemu-system-aarch64" \
      --eeprom /artifacts/pieeprom-old.bin \
      --tftp-root /artifacts/boot \
      --manifest /artifacts/network-program-failure-manifest.json \
      --evidence-directory work/network-recovery-evidence \
      --output work/network-recovery-gate.json

The input manifest must seal ``program-failure`` and its exact cutoff.  Phase
one proves and retains the partial persistent image.  For a cutoff that leaves
a valid network-booting EEPROM prefix, the campaign seals that partial image
in a new success manifest and starts a clean QEMU process against the same
EEPROM file and unchanged TFTP tree.  Phase two must install the original
``pieeprom.upd`` hash, reset, and reach ARM handoff.  Both PCAPs, both phase
reports, the recovery manifest, and their SHA-256 identities are retained in
the atomic campaign report.  An early erase/program cut that destroys the
EEPROM bootloader cannot self-recover over TFTP; it must use the modeled
``recovery.bin`` SD path, just as hardware does.

Run that SD recovery path as a separately sealed production gate with the
same raw SD image and EEPROM ``.bin`` files that are used for flashing::

  python3 contrib/raspi4/sd_recovery_manifest.py \
      --eeprom /artifacts/pieeprom-old.bin \
      --sd-image /artifacts/recovery-sd.img \
      --recovery /artifacts/recovery.bin \
      --update /artifacts/pieeprom.upd \
      --firmware /artifacts/start4.elf \
      --fixup /artifacts/fixup4.dat \
      --kernel /artifacts/kernel8.img \
      --output /artifacts/sd-recovery-manifest.json

  python3 contrib/raspi4/sd_recovery_gate.py \
      --qemu "$PWD/build/qemu-system-aarch64" \
      --eeprom /artifacts/pieeprom-old.bin \
      --sd-image /artifacts/recovery-sd.img \
      --recovery /artifacts/recovery.bin \
      --update /artifacts/pieeprom.upd \
      --firmware /artifacts/start4.elf \
      --fixup /artifacts/fixup4.dat \
      --kernel /artifacts/kernel8.img \
      --manifest /artifacts/sd-recovery-manifest.json \
      --output work/sd-recovery-gate.json

The manifest seals the exact 512 KiB initial EEPROM, complete raw SD image,
official recovery/update files, firmware, fixup, kernel, Pi 4B machine, and RAM
model.  QEMU opens that EEPROM and SD image persistently.  The gate requires
the EEPROM to become byte-identical to ``pieeprom.upd``, the FAT short-name
entry to change only from ``RECOVERY.BIN`` to ``RECOVERY.000``, the automatic
reset to occur, and the same SD image to reach ARM handoff with the sealed
firmware/fixup/kernel hashes.  It retains a hash-bound QEMU trace log and an
atomic JSON report.  This proves the digital flash/reset/boot process with the
same ``.bin`` inputs; electrical power-cut timing and silicon signature
execution remain physical-fixture gates.

Real GENET DHCP/TFTP is the machine default for network ``BOOT_ORDER``; the
gate intentionally relies on that default.  ``network-boot-wire=off`` exists
only for isolated corpus-only compatibility tests.
The EEPROM's production ``TFTP_PREFIX`` policy is also honored: mode 0 uses
the lower-case OTP serial directory, mode 1 uses ``TFTP_PREFIX_STR`` (maximum
32 printable characters), and mode 2 uses the lower-case hyphenated GENET MAC
directory.  Keep the sealed TFTP tree laid out exactly as the same EEPROM
expects on hardware.  If neither prefixed modern nor legacy start file exists,
the emulator performs the bootloader's root-directory fallback.

Add ``--packet-drop-direction tx|rx|both``, ``--packet-drop-after N``, and
``--packet-drop-count N`` to run the same sealed gate with deterministic packet
loss.  The report records configured fault bounds, packets observed at the
GENET boundary, and injected drops.
Every production gate uses a bounded in-process packet socket on the private
TAP interface to bind QMP artifact hashes to the exact manifested TFTP RRQs.
This supports root and device-prefixed trees without guessing the effective
wire path, rejects ambiguous same-name candidates, and records the mapping in
``artifact_wire_paths``.  The optional ``--pcap-output`` additionally retains
the complete Ethernet capture and its hash.  With an existing physical
capture, the gate can also run conformance in the same invocation::

  --cadence-reference captures/pi4b.pcap \
  --cadence-reference-client-mac dc:a6:32:01:36:c2 \
  --cadence-output work/network-cadence.json

A sequence or timing mismatch fails the production gate while retaining the
cadence report for diagnosis.

The host needs ``unshare``, ``ip``, ``dnsmasq`` with DHCP/TFTP support, and
``/dev/net/tun``.  The helper creates a private user/network namespace, a TAP
link, and a real dnsmasq DHCP/TFTP server.  Normal boot reads the supplied
EEPROM through a temporary writable snapshot, so the original ``.bin`` is
never modified.  Explicit self-update mode instead opens the exact 512 KiB
backend persistently and accepts only the manifested final image.  Before
launching, the gate hashes the EEPROM, expected firmware, and every regular
file below the TFTP root; after ARM handoff it requires the QMP firmware hash
to match the unchanged file and re-hashes every input.  It rejects symlinks,
non-regular inputs, an output report inside the TFTP tree, or any unmanifested
byte change.  The separately stored manifest must match the exact
EEPROM hash, complete TFTP file set and hashes, selected board, RAM model, and
firmware name before QEMU can start; it is also hashed again after the run.
The resulting versioned JSON report records the verified EEPROM transition,
ordered internal and wire update evidence, all hashes, the leased address,
selected boot source, firmware/kernel/DT identities, and handoff state.

The comparator can also be run independently using::

  python3 contrib/raspi4/network_cadence.py \
      --reference captures/pi4b.pcap \
      --candidate captures/qemu.pcap \
      --reference-client-mac dc:a6:32:01:36:c2 \
      --candidate-client-mac 52:54:00:12:34:56 \
      --absolute-tolerance-ms 25 --relative-tolerance 0.10 \
      --output work/network-cadence.json

The oracle accepts bounded classic Ethernet PCAP input, hashes both captures,
normalizes DHCP, ARP, DNS, and stateful TFTP traffic, and fails on semantic
sequence or adjacent-packet timing drift.  Exit status 0 means match, 1 means
drift, and 2 means invalid input.

The default Python suite skips the host-integration case.  Exercise the same
gate with the disposable test EEPROM and a selected TFTP fixture using::

  QEMU_RPI_GATE_QEMU="$PWD/build/qemu-system-aarch64" \
  QEMU_RPI_GATE_TFTP_ROOT=/path/to/tftp-root \
      python3 -m unittest -v contrib/raspi4/test_network_boot_gate.py

Release CI can exercise the official-artifact functional gate with the same
configured EEPROM, TFTP tree, and sealed manifest::

  QEMU_RPI_NETWORK_EEPROM=/artifacts/pieeprom.bin \
  QEMU_RPI_NETWORK_TFTP_ROOT=/artifacts/boot \
  QEMU_RPI_NETWORK_MANIFEST=/artifacts/network-boot-manifest.json \
  QEMU_RPI_NETWORK_PHYSICAL_PCAP=/captures/pi4b-network-boot.pcap \
  QEMU_RPI_NETWORK_PHYSICAL_MAC=dc:a6:32:01:36:c2 \
  QEMU_TEST_QEMU_BINARY="$PWD/build/qemu-system-aarch64" \
      build/run tests/functional/aarch64/test_raspi4.py \
      Aarch64Raspi4Machine.test_arm_raspi4_exact_network_boot

That test additionally rejects a corpus unless the manifested start/fixup,
kernel, Pi 4B DTB, and initramfs hashes equal the immutable official fixtures
used by the other production boot gates.  It always captures QEMU's private
TAP traffic and verifies the PCAP digest, packet count, byte count, and link
type in the release report.  The two physical-capture variables are optional
as a pair; when supplied, the same test also requires semantic and timing
cadence conformance and verifies the resulting report.  The configured EEPROM
may differ from the stock fixture because it must contain deployment
``BOOT_ORDER`` mode 2, but the exact configured ``.bin`` is shared with
hardware and remains immutable during the gate.

Run the production boot and recovery gates with::

  QEMU_TEST_QEMU_BINARY=$PWD/build/qemu-system-aarch64 build/run \
      tests/functional/aarch64/test_raspi4.py \
      Aarch64Raspi4Machine.test_arm_raspi4_production_firmware_boot \
      Aarch64Raspi4Machine.test_arm_raspi4_production_mini_uart_boot \
      Aarch64Raspi4Machine.test_arm_raspi4_production_eeprom_recovery \
      Aarch64Raspi4Machine.test_arm_cm4_production_emmc_boot

Run the exact, unprivileged Pi 4B SD, Pi 4B USB-MSD, and CM4 eMMC
release-image gates with the pinned decompressed raw payload::

  QEMU_RPI_RELEASE_RAW_IMAGE=/path/to/2026-06-18-raspios-trixie-arm64-lite.img \
  QEMU_TEST_QEMU_BINARY=$PWD/build/qemu-system-aarch64 \
  build/run tests/functional/aarch64/test_raspi4.py \
      Aarch64Raspi4Machine.test_arm_raspi4_exact_release_sd_first_boot \
      Aarch64Raspi4Machine.test_arm_raspi4_exact_release_usb_first_boot \
      Aarch64Raspi4Machine.test_arm_cm4_exact_release_emmc_first_boot

The gate requires size 2,977,955,840 and SHA-256
``e235fd24fc5f039c08daba7d3abc04aecc7313f979d16d2a3fdad29dd44c33a9``.
The Pi 4B gates copy the payload byte-for-byte to writable virtual SD or
VL805/xHCI USB-MSD, use the unchanged official EEPROM, forbid every
direct-loader shortcut, and require the
release kernel to emit through its selected ``serial0``/``ttyS0`` console,
mount root, and start the unchanged release's serial getty through the
``raspberrypi login:`` prompt.  The firmware mailbox GPIO-expander calls must
complete without the former LED/regulator probe failures, and the release's
own first-boot code must also durably replace the disk identifier while the VM
remains alive.  The USB gate additionally requires mode-4 BOOT_ORDER fallback,
the architectural xHCI/BOT transport, guest USB-storage enumeration, and
root-filesystem expansion on the same backend used by behavioral firmware.
The CM4 gate preserves that exact payload as the prefix of a
4 GiB virtual eMMC, selects the unchanged CM4 DT, and additionally requires
Linux to identify a 4.00 GiB high-speed MMC without stale data-ready or block
I/O errors, then requires partition 2 and ext4 to expand to the full device
before the same login milestone.  These gates need no root privileges or USB
fixture.

The opt-in privileged Linux gate joins nRPIBOOT wait, pinned official
``rpiboot`` and boot files, host USB mass-storage flashing, ownership release,
and boot of the exact flashed eMMC bytes.  Build the Raw Gadget helper, place
the pinned ``bootcode4.bin``, ``config.txt``, and ``boot.img`` in one directory,
and run::

  QEMU_RPI_RPIBOOT=/path/to/usbboot/rpiboot \
  QEMU_RPI_USBBOOT_DIR=/path/to/pinned/mass-storage-files \
  QEMU_RPI_RAW_GADGET=$PWD/build/rpiboot-raw-gadget \
  QEMU_RPI_RAW_MSD=$PWD/build/qemu-rpi-cm4-msd \
  QEMU_RPI_BOT_PROBE=$PWD/build/qemu-rpi-cm4-bot-probe \
  QEMU_RPI_IMAGER=/path/to/Raspberry_Pi_Imager-v2.0.8-cli-x86_64.AppImage \
  QEMU_RPI_RELEASE_IMAGE=/path/to/2026-06-18-raspios-trixie-arm64-lite.img.xz \
  QEMU_TEST_QEMU_BINARY=$PWD/build/qemu-system-aarch64 \
  build/run tests/functional/aarch64/test_raspi4.py \
      Aarch64Raspi4Machine.test_arm_cm4_unmodified_rpiboot_usb_flash_boot

Use the same environment variables and replace the final test name with
``Aarch64Raspi4Machine.test_arm_cm4_supervised_provision`` to run the separate
single-command supervisor gate.  It constructs the trust manifest from the
pinned test constants, checks the supervisor's atomic report, and hashes the
raw payload in the flashed eMMC.

The test requires passwordless ``sudo`` for the tightly scoped kernel-module,
Raw Gadget, configfs, and stable by-id block-device operations.  It rejects
any RPIBOOT input whose SHA-256 differs from the pinned official bundle and
skips unless the three RPIBOOT environment variables are explicitly provided.
When ``QEMU_RPI_IMAGER`` is set, it also requires the published SHA-256 of the
official v2.0.8 CLI-only x86-64 AppImage, leaves Imager verification enabled,
and requires Imager to complete the write.  Without it, the lower-level gate
uses unmodified ``dd``.

When ``QEMU_RPI_RELEASE_IMAGE`` is set, the gate additionally requires the
unchanged official 2026-06-18 Raspberry Pi OS Lite arm64 ``.img.xz`` archive.
It pins both compressed and decompressed SHA-256 values, gives Imager a 4 GiB
virtual eMMC, verifies the exact decompressed payload before boot, then checks
that first boot changes the disk ID, survives its requested reboot, expands
partition 2, grows ext4 to the resulting device size, and reaches the
unchanged release's ``raspberrypi login:`` prompt through CM4 ``serial0``.
The gate drains serial continuously so guest progress cannot be backpressured.
Before the
successful path, the same gate proves
that official ``rpiboot`` rejects a disconnect after exactly 4,096 bootcode
bytes, that official Imager rejects the release on an undersized 64 MiB eMMC,
that Imager fails when the real USB LUN disconnects after at least 8,192
issued sectors, and that forced SCSI medium removal at the same threshold
leaves USB enumerated while block reads fail with ``EIO``.  Clean retries must
then complete.  Omitting the variable retains the smaller synthetic fixture
for quicker transport testing.

The implementation roadmap and the line between modeled and unmodeled
behavior are documented in ``docs/devel/raspi4-platform.rst``.
