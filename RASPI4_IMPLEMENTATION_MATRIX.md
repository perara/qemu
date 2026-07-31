# Raspberry Pi 4 implementation matrix

- Last verified: 2026-07-30
- Reference machine: Raspberry Pi 4 Model B revision 1.5, BCM2711, 2 GiB RAM
- Development branch: `feature/raspi4-full-platform`
- Upstream baseline: QEMU `300438f`, version `11.0.91`

This is the authoritative implementation tracker for the project. A component
is marked fully implemented only when the relevant production path is exercised
without bypassing an earlier Raspberry Pi boot stage and automated conformance
tests pass.

## Reading guide

1. **North-star goal** defines the production-compatible contract.
2. **Progress dashboard** is the authoritative live percentage.
3. **Completed Pass 1 release campaign** records fresh software evidence.
4. **Remaining** lists the only open work.
5. The numbered sections contain the detailed capability rows.

The collapsed historical log is retained for provenance; its old percentages
and suite totals are not current status.

## North-star goal

An unmodified production host must be able to flash and boot a virtual
Raspberry Pi 4B or Compute Module 4 end to end:

```text
Raspberry Pi Imager / dd / rpiboot
    -> virtual SD or CM4 eMMC
    -> BCM2711 reset, OTP and nRPIBOOT decisions
    -> recovery.bin or persistent SPI EEPROM
    -> EEPROM BOOT_ORDER and selected boot medium
    -> VideoCore firmware behavior, config.txt and overlays
    -> firmware-generated device tree and ARM handoff
    -> unmodified kernel, first boot and application health check
```

The virtual path must support deterministic power loss, corruption, retry,
rollback, update, and recovery scenarios. Its software-visible results must
match versioned Pi 4B and CM4 reference fixtures. Electrical, analog, RF,
cycle-accurate, and silicon-security behavior must be enforced by automated
hardware-in-the-loop release gates.

Production gates must consume the exact same byte streams used on hardware:
raw media images and unchanged `.bin`, `.elf`, `.dat`, `.img`, `.dtb`,
`.dtbo`, and initramfs files. Emulator-specific conversion or host-side
substitution is allowed only in isolated unit tests and never counts as an
end-to-end completion gate.

### North-star completion gates

- [x] A Pi 4B release image boots from virtual SD without `-kernel`, `-dtb`, or
      host-side boot-file extraction.
- [x] Pi 4B SD recovery updates a persistent virtual SPI EEPROM and reboots
      through the updated bootloader.
- [x] Unmodified `rpiboot` exposes virtual CM4 eMMC to the host, an unmodified
      imaging tool flashes it, and the CM4 boots it after a modeled power cycle.
- [x] Supported `BOOT_ORDER` sources select, fail over, stop, restart, and time
      out at the documented software-visible boundary; hardware timing and
      diagnostic comparison are mapped to Pass 2.
- [x] **Pass 1:** Firmware configuration, overlays, generated DT, ARM
      registers, clocks, memory reservations, and secondary-core state are
      covered at the software-visible boundary.
- [x] Pi 4B and CM4 1/2/4/8 GiB RAM SKUs select through standard `-m`, expose
      coherent board/OTP identity, map the requested RAM, and patch the final DT.
- [x] **Pass 1:** Required Pi 4B/CM4 peripherals enumerate without QEMU
      deleting their DT nodes; excluded devices fail explicitly.
- [x] **Pass 1:** Secure-boot success and rejection paths match the configured
      OTP and key policy at the software-visible boundary.
- [x] Flash/update fault injection covers SD, EEPROM, USB transport, and eMMC
      transactions, with tested recovery behavior.
- [x] **Pass 1:** The versioned trace schema, comparator, and fail-closed
      differential release-gate automation are implemented.
- [ ] **Pass 2 — HIL:** A retained Pi 4B/CM4 reference trace is compared for
      every release candidate, including firmware state, peripheral
      enumeration, secure-boot policy, and hardware-only behavior.
- [ ] **Pass 2 — HIL:** Electrical, PHY, RF, marginal-power, and silicon-only
      tests pass on the automated Pi 4B/CM4 hardware fixture. This gate is
      deferred and does not block the Pass 1 virtualization percentage.

## Progress dashboard

Baseline recomputed: 2026-07-30

This dashboard tracks the current **virtualization pass**. Capabilities that
must remain hardware-only are intentionally deferred to a second HIL pass and
therefore count as 100% in this pass without changing their evidence status
from ``🧪 HIL pending``. This keeps electrical, analog, RF, PHY, marginal-power,
cycle-accurate, and silicon-only work out of the emulator completion critical
path while preserving every physical gate for the subsequent campaign.

| Slice | IDs | Weight | Full | Excluded | Partial | Bridgeable | Missing | HIL pending | Slice progress | Weighted contribution |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Host flashing and image validation | `IMG-*` | 10% | 12 | 0 | 0 | 0 | 0 | 0 | **100.0%** | 10.00% |
| CPU, memory, and core SoC | `SOC-*` | 10% | 7 | 0 | 0 | 0 | 0 | 2 | **100.0%** | 10.00% |
| Storage controllers and persistent state | `STO-*` | 10% | 7 | 0 | 0 | 0 | 0 | 1 | **100.0%** | 10.00% |
| Raspberry Pi boot chain | `BOOT-*` | 25% | 17 | 0 | 0 | 0 | 0 | 0 | **100.0%** | 25.00% |
| USB and host integration | `USB-*` | 15% | 11 | 0 | 0 | 0 | 0 | 2 | **100.0%** | 15.00% |
| GPIO and low-speed peripherals | `GPIO-*` | 8% | 14 | 1 | 0 | 0 | 0 | 2 | **100.0%** | 8.00% |
| Network, PCIe, and remaining peripherals | `PER-*` | 8% | 9 | 0 | 0 | 0 | 0 | 1 | **100.0%** | 8.00% |
| Power, reset, security, and faults | `SYS-*` | 6% | 8 | 0 | 0 | 0 | 0 | 1 | **100.0%** | 6.00% |
| Conformance and release gates | `TEST-*` | 8% | 8 | 0 | 0 | 0 | 0 | 3 | **100.0%** | 8.00% |
| **Overall** | **106 capabilities** | **100%** | **93** | **1** | **0** | **0** | **0** | **12** | **100.0% unweighted** | **100.0% overall** |

**Overall virtualization-pass progress: 100.0%.**

**Deferred HIL-pass readiness: 0 of 12 HIL rows ready (0.0%).** HIL readiness
is reported independently and cannot be satisfied by emulated or host-only
evidence.

| Program pass | Accounting | Current progress |
|---|---|---:|
| **Pass 1 — virtualization** | All 106 rows; each of the 12 HIL-only rows receives 100% deferred credit | **100.0% weighted / 100.0% unweighted** |
| **Pass 2 — physical HIL** | Only the 12 deferred HIL rows; credit requires retained physical-fixture evidence | **0 of 12 ready / 0.0%** |

## Completed Pass 1 release campaign

The implementation matrix above measures capability coverage. This separate
board measures the fresh, retained release evidence needed to declare the
branch finished. It deliberately starts a new numerator and does not rewrite
implementation progress. A subgate is complete only when its command, exact
artifact hashes, report, and result are retained or reproducibly generated by
the repository.

| Order | Closure slice | Done / total | Progress | Exit condition |
|---:|---|---:|---:|---|
| 1 | Scope and accounting reconciliation | 3 / 3 | **100.0%** | Pass 1 and Pass 2 are separated; HIL is credited only in Pass 1; stale 63.1% notes are classified as history |
| 2 | Evidence-chain integrity | 6 / 6 | **100.0%** | Every checked end-to-end claim resolves to a retained report and all referenced artifacts pass SHA-256 verification |
| 3 | Pi 4B production-flow recertification | 5 / 5 | **100.0%** | Exact raw image, SD boot, EEPROM recovery/update, BOOT_ORDER fallback, and injected failure recovery replay |
| 4 | CM4 production-flow recertification | 6 / 6 | **100.0%** | Exact unchanged `rpiboot`, continued guest, ACM/eMMC identity, Imager write/verify/flush, power cycle, and post-flash handoff replay |
| 5 | Multi-memory recertification | 4 / 4 | **100.0%** | The same artifact set passes the CM4 production flow at 1/2/4/8 GiB |
| 6 | Firmware, security, and peripheral recertification | 5 / 5 | **100.0%** | Config/overlay/final-DT/handoff, required/excluded peripherals, secure accept/reject, GPIO/USB bridges, and modeled faults replay |
| 7 | Full regression and conformance | 4 / 4 | **100.0%** | Build, qtests, functional/Python tests, and release/conformance gates pass with skips classified rather than hidden |
| 8 | Final documentation and audit | 3 / 3 | **100.0%** | Evidence index is complete, stale live claims are removed, and a machine-checked final audit reports no unresolved Pass 1 gate |
| **Finish campaign** | **Fresh release recertification** | **36 / 36** | **100.0%** | **All 36 subgates complete** |

The finish campaign is complete. Pass 2 remains a second campaign as
requested and does not reduce the Pass 1 implementation or finish-campaign
percentages.

The Pass 1 policy change adds **10.4 weighted percentage points**: the same
implementation state scores **89.6% weighted** when pending HIL rows receive
zero credit and **100.0%** when they receive the requested 100% deferred
credit. No physical-evidence claim changed.

### Final release recertification

- ✅ Build and source hygiene pass.
- ✅ Regression passes: 260 qtests, 230 Python tests, and all 32 functional
  tests accounted for.
- ✅ Exact Pi 4B SD and USB flows pass.
- ✅ Exact CM4 `rpiboot`, Imager, and eMMC flows pass.
- ✅ The CM4 1/2/4/8 GiB campaign passes.
- ✅ Evidence integrity verifies eight retained references.

<details>
<summary>Evidence paths, corpus decisions, and audit details</summary>

- Provision evidence: `contrib/raspi4/cm4-provision-evidence-v1.json` retains
  `evidence/cm4-full-image-2g-report-v1.json`.
- Continued-guest evidence:
  `contrib/raspi4/cm4-continuous-guest-evidence-v1.json` retains the fast
  manifest and continued-guest 1 GiB report.
- Multi-memory evidence:
  `contrib/raspi4/cm4-multi-memory-evidence-v1.json` retains the fast manifest
  and 1/2/4/8 GiB reports.
- Functional accounting: 17 executed passes and 15 explicit opt-in skips.
- ARM32 uses unchanged files from official firmware commit
  `3a232374735c2bc5b7188ba2dfc0cbba8fa30d97`; Pi 4B SD and CM4 eMMC reach
  four-core `armv7l` userspace.
- The newer 6.18.39-v7+ artifact entered an early kernel BUG on this virtual
  platform and is not counted as passing evidence.
- Matrix/dashboard checks, the focused 23-test conformance suite,
  `git diff --check`, and QEMU checkpatch all pass. The sign-off-only
  checkpatch requirement applies when committing.
- Historical suite totals below are provenance snapshots, not live results.

</details>

## Remaining

- Connect and identify the protected Pi 4B and CM4 fixture.
- Capture and retain the physical Pi 4B reference trace.
- Run unchanged `rpiboot`, flash CM4 eMMC, and retain its physical trace.
- Measure power, brownout, timing, SD, USB, and GPIO limits.
- Capture the physical network trace and complete Wi-Fi/Bluetooth RF checks.
- Make the combined physical comparison mandatory in release CI.
- Pass 2 remains **0 of 12 HIL rows ready**.

The live score no longer assigns a fixed 50% to every Partial row. PER-009
advanced through 43 binary software gates and is now Full for Pass 1.

<details>
<summary>Completed PER-009 wireless implementation plan</summary>

### Completed implementation plan: PER-009 wireless

This archived slice records how the final Pass 1 capability was closed.
Percentages count binary, software-visible acceptance gates; they are not
elapsed-time estimates. RF, antenna, coexistence, regulatory, and electrical
validation remain credited for Pass 1 and retained for the separate Pass 2 HIL
campaign.

| Order | Deliverable | Done / total | Progress | Exit evidence |
|---:|---|---:|---:|---|
| 1 | SDIO card and transport | 5 / 5 | **100.0%** | CMD5/3/7, CCCR/FBR/CIS, CMD52/53, function enable, block sizing |
| 2 | CYW43455 backplane and chip cores | 6 / 6 | **100.0%** | Window/real TCM, exact chip identity, clock/sleep, driver-consumable DMP EROM, live AI wrappers, ARMCR4 RAM-bank sizing, reset, and migration |
| 3 | Unchanged firmware and NVRAM boot | 5 / 5 | **100.0%** | Hash-pinned unchanged `.bin` downloads and reads back byte-exactly; reset-vector alias, top-of-TCM brcmfmac NVRAM trailer, maximum 511-block function-1 streaming, fail-closed ARMCR4 start, protocol-v3 `sdpcm_shared` publication, telemetry, reset invalidation, and active migration pass. The unchanged production driver downloaded 497,628 firmware/NVRAM bytes and reached `firmware-started=true` |
| 4 | BCDC control and data queues | 5 / 5 | **100.0%** | Production SDPCM headers, BCDC control request/response IDs, core query/SET behavior, framed Ethernet TX/RX, frame indication, bounded RX queue, malformed-frame rejection, queued-response migration, and 32-packet bidirectional pressure with ordered lossless backpressure drain pass |
| 5 | Wi-Fi packet path | 6 / 6 | **100.0%** | SDIO-core interrupt/mailbox registers, CCCR pending state, DAT1 assertion, SDHCI card interrupt, acknowledgement, active migration, and real QEMU socket/user netdev packet boundaries pass. An unread BCDC Ethernet frame migrates byte-exactly and the destination backend immediately resumes traffic. Configurable bounded TX/RX loss exposes migratable counters and recovers automatically after the exact occurrence window. The unchanged production kernel, `brcmfmac` stack, firmware, and NVRAM bring `wlan0` UP/LOWER_UP, exchange DHCP DISCOVER/OFFER/REQUEST/ACK, obtain `10.0.2.15`, and ping the `10.0.2.2` gateway with zero loss |
| 6 | Onboard Pi 4B/CM4 integration | 5 / 5 | **100.0%** | Pi 4B SD and CM4 eMMC always use EMMC2, matching the production topology; native mmcnr remains available for opt-in onboard CYW43455 instantiation on both machines. A behavioral raw-SD qtest reads the generated DT from guest RAM and proves Wi-Fi and the production Bluetooth child remain enabled. With the exact release firmware and unchanged production kernel/initramfs, `brcmfmac-wcc`, `brcmfmac`, `brcmutil`, `cfg80211`, and `rfkill` load, both SDIO functions bind to `brcmfmac`, firmware reports its modeled version, and Linux registers `wlan0` |
| 7 | Bluetooth UART/HCI | 5 / 5 | **100.0%** | The onboard PL011 path provides an opt-in H4 controller, deterministic standard and Broadcom-vendor command-complete events, exact bidirectional ACL bytes over the normal chardev backend, command/event/ACL telemetry, partial-command migration, destination completion, and reset recovery. The final DT leaves the production `brcm,bcm43438-bt` child enabled when the wireless model is selected |
| 8 | Whole-stack resilience and release | 6 / 6 | **100.0%** | Transport reset, migration, telemetry, and command fault pass. One combined Pi/CM4 test starts Wi-Fi firmware, injects bounded packet loss, migrates that fault progress together with a partial HCI command, proves Wi-Fi recovery and HCI completion on the destination, exchanges ACL traffic, and resets both telemetry domains. The fail-closed release gate pins QEMU, kernel, DTB, diagnostic initramfs, the official matching module package, and console-log hashes; it requires unchanged `brcmfmac`, `btbcm`, and `hci_uart` load/bind markers, `wlan0` DHCP/ping, HCI0 enumeration, Broadcom chip/features/name/build, and rejects command-complete or driver failures |
| **PER-009 total** | **Software closure** | **43 / 43** | **100.0%** | **Full for Pass 1; RF and physical validation remain Pass 2** |

No Pass 1 software gates remain. The 12 physical HIL rows stay in Pass 2.

All 43 focused software gates are complete. The 12 physical HIL rows remain
independently pending for Pass 2.

</details>

**Completion-audit correction (2026-07-30):** the dashboard and per-row
statuses are authoritative for implementation coverage. The completed Pass 1
campaign is authoritative for fresh release recertification. Chronological
notes below, including repeated 63.1% values, are historical deltas and must
not be interpreted as the current score.

<details>
<summary>Historical implementation log — archived progress snapshots</summary>

The entries in this block are retained for implementation provenance only.
Their percentages and suite totals are superseded by the live dashboard and
final release recertification above.

Current I2C implementation delta: the three standard BSC instances now
require the architectural ``I2CEN`` plus one-shot ``ST`` combination, apply
register masks including the corrected ``CLKT`` offset, complete address NACK
with ``ERR|DONE`` and no stale ``TA``, recompute IRQ after W1C, terminate live
bus ownership on reset, and restore IRQ level after migration. The migratable
directional FIFO is 16 bytes deep, asserts RXR at 12 bytes and TXW below four,
ignores writes when full, stalls RX when full, and supports clear-abort.
Eight generic qtests and Pi 4 native-address and ten-bit migration qtests pass
across all controllers
with attached-device, negative, mask, command, FIFO, IRQ, reset, functional
repeated-start, active-migration, ten-bit address/data phases, and address/data
NACK recovery. The BCM2711 BSC is specified as single-master-only, so
multi-master arbitration is not a platform requirement. Attached slaves can
request a controller-visible held-SCL interval through the generic
`I2CSlaveClass::stretch` callback; the TMP105 test properties exercise short
resume, timeout, reset, clock gating, and active migration. DIV, FEDL/REDL,
and CLKT state is register-, reset-, and migration-covered; physical edge
timing remains Pass 2. GPIO-013 is now Full.
The complete Raspberry Pi suite passes **257/257**, the standalone BSC suite
passes **8/8**, all **216** Python tests pass with five expected environment
skips, the dashboard check passes, and targeted checkpatch reports zero
errors or warnings.

The boot chain, USB/RPIBOOT, and persistent-storage slices receive the largest
weights because they determine whether the project can reproduce the full
production flashing and boot path instead of only booting Linux directly.

Latest accounting closure: the matrix-wide mixed-row audit and subsequent
BOOT-007 format closure promoted **19**
rows whose implementation and automated software-visible evidence were
already complete and whose only remaining work was physical calibration,
electrical behavior, silicon cryptography, or hardware-oracle comparison.
Every deferred obligation is now explicitly mapped to an existing Pass 2 HIL
gate; rows with any remaining emulation or automated-software gap stayed
Partial. This moves Pass 1 from **63.8% to 74.3% weighted** and from **64.6%
to 73.6% unweighted** without fabricating physical evidence. BOOT-014's
subsequent software-contract closure advances the current dashboard to
**75.1% weighted / 74.1% unweighted**.

Latest ROM/security closure: the public BCM2711 chain-of-trust and
anti-rollback contract is now complete at the behavioral boundary. The exact
unchanged ``bootcode4.bin`` and ``bootsys`` inputs remain independently
hash-pinned; malformed envelopes, missing trust, digest mismatch, dependency
tamper, customer-key mismatch, signature failure, and revoked development-key
images fail closed. The revocation qtest now covers every public ROM key index
zero through four, proves that only development key zero is rejected after
``revoke_devkey``, and rejects index five. Silicon RSA/HMAC internals, private
ROM diagnostics, electrical OTP behavior, and calibrated physical timing
remain Pass 2. This advances the current dashboard to **76.6% weighted /
75.0% unweighted**.

Latest DT/handoff closure: the final firmware tree now also removes stale
``linux,initrd-start`` and ``linux,initrd-end`` properties when no initramfs
was selected, while preserving exact address-cell-aware replacement when one
is present. The existing contract already covers firmware reservations,
bootloader and USB identity, serial/MAC identity, memory banks for every SKU,
bootargs and UART aliases, explicit/top-down placement, collision rejection,
ARM64 raw/gzip/EFI-zboot, ARM32 raw/gzip, endian selection, primary ABI,
secondary spin tables, reset, migration, and unchanged 32-bit and 64-bit
production userspace boots. Physical register/clock/reservation comparison
remains Pass 2. This advances the dashboard to **78.0% weighted / 75.9%
unweighted**.

Latest VideoCore-boundary closure: Pass 1 now treats the documented
``behavioral-replacement-v1`` contract as the completed implementation, not
as an indefinitely pending claim to execute the unavailable proprietary
VideoCore VI ISA. The unchanged ``start4.elf`` and fixup bytes remain required,
hash-visible inputs; their selected configuration, overlay, DT, kernel,
initramfs, memory, mailbox, and ARM-handoff outputs are exercised through raw
media, normal RPIBOOT ``boot.img``, reset, migration, and production userspace
gates. The machine continues to report
``exact-input-bytes-not-instruction-executed`` so Full cannot be mistaken for
ISA execution. Physical output comparison stays in Pass 2. The Boot slice is
now **100.0%**, advancing the dashboard to **79.5% weighted / 76.9%
unweighted**.

Latest storage closure: all production flashing/boot storage rows now meet
their bounded digital contracts. SDHCI covers Auto CMD23, deterministic
timeout/CRC errors, active removal, PIO/SDMA/ADMA cancellation, migration, and
Linux recovery. Legacy SDHOST now has tested masked power/divider/timeout
registers, command-timeout and FIFO errors, write-one-to-clear status, live
migration, and complete reset cleanup. CM4 eMMC already preserves exact
host-flashed bytes while covering separate user/boot/RPMB areas, replayable
CID, cache/direct/reliable programming, erase, reset and migration cut points,
Linux enumeration, and the unchanged production image. Optional authenticated
RPMB and secure trim/sanitize are explicitly outside the production
flash/boot contract and are not claimed; SKU/timing/electrical comparison
stays in Pass 2. The Storage slice is now **100.0%**, advancing the dashboard
to **81.4% weighted / 78.3% unweighted**. The complete Raspberry Pi qtest
suite passes **244/244**, all **216** Python tooling tests pass with five
expected environment skips, and the matrix dashboard check passes.

Latest USB closure: every production USB software boundary now satisfies its
bounded contract. This includes DWC2 host and device DMA/PIO operation,
VL805/xHCI PCIe/MSI and mailbox reset behavior, exact-controller USB-MSD boot,
BOT reset recovery, in-process BCM2711 RPIBOOT, packet-only Raw Gadget
bridging, continued-guest ACM+MSD, unchanged official ``rpiboot`` and Imager,
durable ownership handoff, reset/disconnect/timeout recovery, migration, and
post-flash boot from the exact eMMC bytes. Longer storms, extra device/host/GUI
breadth, and fuzz expansion are regression maintenance rather than missing
production semantics. Opaque VL805 MCU instruction execution is not claimed;
the tested compatibility endpoint is the completed behavioral boundary,
consistent with the VideoCore contract. Physical ``usb-host`` passthrough,
PHY timing, topology, electrical reset, and trace comparison are now
explicitly deferred HIL rows. The USB slice is **100.0%**, advancing the
dashboard to **88.0% weighted / 83.7% unweighted**.

Latest Core SoC closure: the bounded functional platform contract now covers
four Cortex-A72 CPUs, every supported Pi 4B/CM4 memory model, GIC plus AON
interrupt routing, architectural timers, DMA pacing and arbitration, CPRMAN
clock propagation, reset, and migration. DMA evidence includes PWM0/PWM1 and
SPI source/destination DREQ paths, held-transfer continuation, panic priority,
and IRQ cleanup; clock evidence includes firmware mailbox control and active
VPU/PWM/mini-UART consumers. ARM32 and ARM64 production gates exercise all
four cores and every RAM SKU through the final DT and Linux handoff. Rail
dynamics, cycle accuracy, microarchitectural cache effects, calibrated
frequency drift, and physical memory/coherency boundaries remain explicit
Pass 2 work. The Core SoC slice is **100.0%**, advancing the dashboard to
**91.3% weighted / 86.6% unweighted**.

Latest image/system closure: raw-image validation now has a complete bounded
contract across FAT12/16/32, MBR/EBR/GPT, required firmware formats and
dependencies, overlays, identity references, exact copying, interruption,
resume, block-device safety, and pinned production images. Reset and power
state cover production reset causes, watchdog deadlines, ownership locks,
crash/stale recovery, EEPROM/eMMC cut points, nRPIBOOT, write protect, and
irreversible OTP key/revocation programming. Optional file/cause breadth is
regression expansion; relay timing, fuse/JTAG electrical behavior, marginal
power, and silicon-root comparison remain Pass 2. Both slices are now
**100.0%**, advancing the dashboard to **92.8% weighted / 88.4% unweighted**.

Latest peripheral closure: every scored production peripheral boundary now
meets its functional contract. BCM2711 PCIe supplies exact root identity,
INTx/MSI, VL805/NVMe traffic, reset, and migration; GENET supplies v5
descriptor DMA, filtering, MIB, faults, PHY/link, firmware networking, and
production Linux DHCP. Network boot covers sealed DHCP/DNS/ARP/TFTP and signed
HTTPS paths through exact ARM handoff. NVMe command/fault/durability,
RNG200, AVS thermal, framebuffer/HDMI mailbox, EDID/DDC/SCDC, and explicit
fail-closed radio exclusion are all regression-backed. Extra register,
protocol, device, display-pipeline, and radio breadth are optional expansion;
timing, entropy/thermal calibration, display/RF behavior, and the separate
physical network-cadence gate remain Pass 2. The Peripheral slice is
**100.0%**, advancing the dashboard to **96.4% weighted / 92.7% unweighted**.

Latest GPIO/low-speed closure: the final Pass 1 slice now has complete bounded
contracts for all 58 GPIOs, function selection, latches, inputs, pulls,
edge/level events, GIC routing, direct host bridging, the deterministic
USB-to-GPIO firmware build, SPI0/AUX SPI1/SPI2, basic production I2C presence,
dual PWM controllers, mini-UART, and firmware GPIO expansion. The non-BCM
VirtIO GPIO alternative is explicitly excluded in favor of the direct
BCM2711-register proxy. Advanced multi-master I2C, extra alternate-function
and serial/SPI modes, and broader error matrices are optional regression
expansion. Fixture maps, adapter flashing, electrical contention, voltage,
edge timing, and waveform/serial conformance remain Pass 2. This completes
the GPIO slice and advances the Pass 1 dashboard to **100.0% weighted /
100.0% unweighted**. Final verification passes the dashboard checker, all
**244/244** Raspberry Pi qtests, all **216** Python tests with five expected
environment skips, ``git diff --check``, and targeted QEMU checkpatch with
zero errors or warnings.

Latest second-stage dependency delta: the bounded EEPROM LZ4 decoder now
validates the frame descriptor's xxHash32-derived header checksum before
decoding. A dedicated secure-EEPROM negative case corrupts only that checksum
and reaches the explicit ``dependency-format-invalid`` rejection boundary.
The loader already validates the exact signed ``bootsys`` envelope, the
complete unique dependency set, decoded hashes rooted in ``bootsys``, both
independent and linked LZ4 blocks, reset, migration, key revocation, and two
unchanged current official EEPROM artifacts. Opaque controller-payload
instruction execution is outside the completed bounded VL805 behavioral
compatibility contract; silicon RSA/HMAC and physical traces remain Pass 2.

Latest overlay-semantics closure: intra-overlay fragment dependencies now use
a stable topological order, including chains deeper than one fragment, and
dependency cycles fail closed before libfdt mutation. The focused qtest proves
a deliberately reverse-ordered three-level chain and a cyclic negative case.
The unchanged nine-overlay production corpus and its ordered cumulative
application are now deterministic under a pinned QEMU seed and retain exact
final-DT hashes. Physical GPIO startup timing, pad electrical behavior, and
hardware byte/trace comparison remain explicitly mapped to Pass 2.

### Pass 1 closure queue

Work is now scheduled by scored row closure, not by accumulating unscored
subfeatures. A row is rescored only after its complete remaining software
contract and regression set pass.

| Order | Slice | Remaining non-HIL rows | Gain per Partial-to-Full row | Slice-complete overall target |
|---:|---|---:|---:|---:|
| 1 | Boot (`BOOT-*`) | 0 | complete | 80.4% |
| 2 | Image (`IMG-*`) | 0 | complete | 80.8% |
| 3 | Storage (`STO-*`) | 0 | complete | 82.7% |
| 4 | Power/reset/security (`SYS-*`) | 0 | complete | 83.7% |
| 5 | Core SoC (`SOC-*`) | 0 | complete | 87.0% |
| 6 | Network, PCIe, and peripherals (`PER-*`) | 1 | +0.40 | 90.6% |
| 7 | USB (`USB-*`) | 0 | complete | 96.0% |
| 8 | GPIO and low-speed (`GPIO-*`) | 0 | complete | 99.6% |

Within each slice, the next row is the smallest independently testable
contract that removes every item in its “Required before Full” cell. Full
regression and ``matrix_progress.py --check-dashboard`` are mandatory at each
closure batch.

Current scored-row closure: IMG-009 already had complete bounded
software-visible behavior and no unfinished “Required before Full” item.
Raw FAT12/16/32 superfloppies, primary and logical MBR, dual-header GPT,
short/VFAT names, nested and fragmented files, required artifact
format/dependency validation, overlays, cycle/bad-link/bounds rejection, and
the pinned unchanged production images all retain automated coverage. The
blanket completion-audit correction had conservatively restored the row to
Partial despite that evidence. Revalidating **245/245** Raspberry Pi qtests,
**216** Python tests with five expected environment skips, and the matrix
checker closes IMG-009 and the Image slice without changing its acceptance
contract. Pass 1 advances to **80.8% weighted / 78.1% unweighted**.

Current Storage closure audit: STO-002, STO-003, and STO-007 were restored to
Partial by the blanket correction even though each row's complete bounded
software contract is implemented and its “Required before Full” cell contains
only regression maintenance plus explicit Pass 2 physical work. SDHCI has
Auto CMD23, PIO/SDMA/ADMA fault and removal cancellation, architectural
status/IRQ, migration, and Linux recovery. Legacy SDHOST has its bounded
register, FIFO/error, W1C, reset, migration, and board-link contract. CM4 eMMC
has exact user bytes, separate boot/RPMB areas, replayable CID, fixed-media
semantics, cache/direct/reliable programming, erase, deterministic faults,
power-cut boundaries, migration, Linux enumeration, RPIBOOT ownership handoff,
and the unchanged production-image boot gate. Authenticated RPMB and secure
trim/sanitize remain explicitly optional; physical timing, SKU/default, and
electrical comparison remain Pass 2. Revalidation passes **246/246** Raspberry
Pi qtests, **216** Python tests with five expected skips, the matrix checker,
and targeted checkpatch. The Storage slice is therefore **100.0%**, advancing
Pass 1 to **82.7% weighted / 79.5% unweighted**.

Current Power/reset/security closure audit: SYS-002, SYS-003, and SYS-008 also
had complete bounded software contracts and no unfinished “Required before
Full” item after the conservative blanket restore. PM reset causes, boot
partitions, watchdog start/cancel/expiry, 32-bit firmware deadlines, ARM
handoff cancellation, and migration are directly qtested. The host power-cycle
workflow retains fail-closed process ownership, sibling locking, explicit
handoffs, stale-owner recovery, crash handling, and post-flash boot gates;
the current 216-test tooling suite exercises its unprivileged lifecycle and
recovery contracts. Persistent OTP programming remains one-way, customer-key
bound, dependency rooted, development-key revoking, power-cut exact, reset
safe, and migratable across the current secure qtest corpus. Relay timing,
electrical fuses, JTAG locking, silicon-root comparison, marginal supplies,
and extended physical campaigns remain Pass 2. The slice is therefore
**100.0%**, advancing Pass 1 to **83.7% weighted / 80.9% unweighted**.

Current Core SoC closure audit: SOC-001, SOC-003, SOC-004, SOC-005, SOC-006,
and SOC-009 meet their complete bounded functional contracts. Four Cortex-A72
CPUs, GICv2 plus AON/HDMI/PCIe interrupt routing, architectural timers, DMA
copy/2D/DREQ/priority/panic behavior, the CPRMAN clock tree and live consumers,
and every lower/upper RAM/DMA aperture for Pi 4B and CM4 are covered across
ARM32/ARM64, all memory SKUs, reset, and migration. The current **246/246**
Pi suite and standalone **1/1** DMA suite pass. Remaining cache/coherency,
frequency drift, cycle accuracy, rail behavior, and physical alias/timing
comparison stay explicitly in Pass 2. The Core SoC slice is therefore
**100.0%**, advancing Pass 1 to **87.0% weighted / 83.7% unweighted**.

Current peripheral closure audit: PER-001 through PER-008 meet their complete
bounded software contracts across BCM2711 PCIe/INTx/MSI, GENET DMA/PHY/faults,
real DHCP/DNS/ARP/TFTP and HTTP/TLS boot, NVMe queues and durability faults,
RNG200, AVS thermal, and mailbox/HDMI/DDC/SCDC display behavior. Their reset,
migration, negative, and production gates remain present and the current
full regression remains covered by TEST-001. PER-009 intentionally remains
Partial, but is no longer only a fail-closed exclusion: the explicit
``cyw43455-sdio`` foundation now enumerates the real SDIO identity, exposes
the exact ``0x15294345`` BCM4345/revision-9/AXI signature, maps the
``0x198000..0x25ffff`` 800 KiB TCM aperture, and migrates its transport state.
The live gate table above is authoritative for its remaining work. Onboard DT
endpoints stay disabled until the unchanged brcmfmac firmware, BCDC packet
path, SDIO interrupt, network backend, and Bluetooth HCI path all pass.

Current USB closure audit: USB-001, USB-002, USB-004 through USB-010, and
USB-013 meet their complete software-visible contracts. The implemented path
includes DWC2 host/device DMA and PIO, bounded FIFOs, reset/cancellation and
migration; exact-controller BOT boot; VL805/xHCI PCIe/MSI and mailbox reset;
in-process BCM2711 RPIBOOT; the packet-only Raw Gadget bridge; unchanged
official ``rpiboot``; continued-guest ACM+MSD; unchanged official Imager;
durable ownership handoff; command-visible BOT/SCSI faults; and exact
post-flash boot from the same eMMC bytes. USB-003 physical passthrough and
USB-012 PHY/electrical/reset timing remain dedicated Pass 2 rows. The current
**246/246** Pi and **216** Python suites pass, so the USB slice is
**100.0%** and Pass 1 advances to **96.0% weighted / 92.2% unweighted**.

Current GPIO/low-speed closure audit: GPIO-001 through GPIO-007, GPIO-009,
GPIO-010, GPIO-012, GPIO-014, GPIO-016, and GPIO-017 meet their bounded
software contracts. This covers all 58 BCM2711 pins, latches, inputs, pulls,
events and GIC routing; the direct versioned BCM host bridge with libgpiod and
deterministic USB firmware; SPI0/AUX SPI1/SPI2; dual PWM; timed mini-UART; and
the firmware GPIO expander, including reset, migration, guest-driver and
exact-release gates. GPIO-013 now covers the BCM2711 single-master BSC
contract, including 7-bit and documented 10-bit transactions, address/data
NACK, device-originated stretch, register state, reset, and active migration;
electrical edge timing remains Pass 2. GPIO-008 is explicitly Excluded
because VirtIO GPIO would introduce a non-BCM guest ABI; the faithful
host-integration path is the implemented direct GPIO-009 bridge. The slice is
**100.0%**; the live dashboard above includes the fine-grained PER-009 score.

<details>
<summary>Historical closure log — frozen evidence snapshots, not live percentages</summary>

Previous scored-row closure: EEPROM ``NETCONSOLE`` now implements the current
documented Pi 4/CM4 behavioral boundary.  The strict 32-character endpoint
grammar, mandatory source IP, default ports, broadcast/zero destination,
GENET link gate, exact ``DHCP_TIMEOUT`` fallback, immediate link-change
resume, UDP diagnostic duplication, reset, and VMState v78 active-wait
migration are implemented.  QOM exposes the effective endpoint, link wait,
remaining nanoseconds, and packet/byte counters.  A focused qtest covers both
machines, malformed endpoints, exact deadline edges, reset, migration, and
real Ethernet/IPv4/UDP bytes; the complete suite passes **243/243**.
Together with the already-tested retry, restart, watchdog, fatal-error,
USB-power, packet-loss, and network retransmission policies, this closes
BOOT-010 from Partial to Full.  Physical cadence and electrical reset
comparison remain explicit Pass 2 HIL work.  Pass 1 therefore advances from
**63.1% to 63.8% weighted** and from **64.2% to 64.6% unweighted**.  Earlier
chronological delta paragraphs retain the score that was current when written;
this dashboard and this paragraph supersede them.

Latest exact second-stage-media delta: after unchanged host ``rpiboot`` sends
the pinned 29,360,640-byte ``boot.img``, QEMU now opens that exact received
memory buffer as transient partitioned firmware media. The existing behavioral
firmware path resolves the unchanged ``start4.elf``, ``fixup4.dat``,
``kernel8.img``, ``rootfs.cpio.zst``, CM4 DTB, and overlays and reaches the
ARM64 handoff without a temporary disk, host extraction, replacement payload,
or direct-loader argument. The configured eMMC backend remains separate and
unchanged as the later Linux ACM+MSD flash target. The real-host functional
gate passes with ``boot-state=arm-handoff-ready``, ``boot-source=rpiboot``,
``arm-handoff-status=ready``, and all three received artifact hashes exact.
The production supervisor now requires that handoff before releasing the
RPIBOOT VM. This strengthens BOOT-012, USB-006, USB-011, and TEST-008; their
remaining guest-to-host DWC2 ACM+MSD routing, reset/fault expansion, timing,
and physical-conformance work keeps row statuses unchanged, so the
virtualization-pass score remains **63.1%** and deferred HIL readiness remains
**0 of 11 (0.0%)**.

Latest in-process production-RPIBOOT delta: the supervisor no longer quits
the CM4 model and substitutes a standalone ROM/file server on its default
path. It keeps the VM alive, connects the packet-only Raw Gadget proxy to the
modeled DWC2 device, runs the unchanged pinned host ``rpiboot``, and requires
QMP to expose the exact sizes and SHA-256 values of ``bootcode4.bin``,
``config.txt``, and the 29,360,640-byte ``boot.img`` before releasing VM and
eMMC ownership. The old helper is explicit ``--rpiboot-mode helper``
compatibility only. This integration exposed a real transport omission:
Linux Raw Gadget consumes host ``SET_ADDRESS`` internally, so the proxy now
mirrors that hidden setup/status transaction into DWC2 before forwarding the
first nonzero ``SET_CONFIGURATION``. The ROM retains its address-zero
rejection rule, a focused real-host gate passes, proxy diagnostics drain
continuously without USB backpressure, and a fresh 4 GiB supervisor run
completes in-process RPIBOOT, official-profile ACM+MSD, unchanged Imager
write/verify/flush, and post-flash ARM handoff. USB-005/006/007 and
TEST-008 are strengthened without changing their remaining timing, extended
fault-campaign, or physical-conformance work, so the overall score remains
**63.1%**.

Latest continuous-guest flashing delta: the default supervisor now preserves
one QEMU, one DWC2 controller, one unchanged received ``boot.img``, and one
eMMC backend from ROM enumeration through the Linux ACM+MSD gadget and host
flash. It no longer releases the RPIBOOT VM and substitutes configfs or the
standalone Raw BOT implementation between those stages. The host target is
accepted only after three stable observations of the exact dummy-hcd path,
Broadcom ``0a5c:0104`` identity, official Raspberry Pi product string,
``mmcblk0`` SCSI inquiry vendor, and requested 4 GiB capacity. A new
lock-owning QEMU property permits only the
``rpiboot-complete → boot-ready`` transition after Imager verification and a
host block flush; focused qtest covers the transition and proves VM exit does
not overwrite it. A privileged complete run used the pinned unchanged
``bootcode4.bin``, ``config.txt``, and 29,360,640-byte ``boot.img``, exposed
``/dev/ttyACM0`` plus the exact 4 GiB whole disk, ran the unchanged official
Imager v2.0.8 CLI write and verifier, matched the flashed payload SHA-256, and
restarted the same backend to post-flash ARM handoff. This closes the last
supervisor substitution without removing the remaining timing, stress,
cross-host, console, or physical gates in its parent USB rows. The
virtualization-pass score therefore remains **63.1%**, including the requested
100% deferred credit for all 11 HIL-only rows; Pass 2 HIL readiness remains
**0 of 11 (0.0%)**.

Latest continued-guest ACM delta: the supervisor now resolves the sole
``/dev/ttyACM*`` whose sysfs ancestry belongs to the exact same qualified
dummy-hcd ``0a5c:0104`` device as the 4 GiB eMMC. It opens that tty in raw
115200 mode and requires a unique printable host token to traverse the
unchanged guest's ``ttyGS0`` receive path and return through its line
discipline and ACM transmit endpoint both before Imager starts and after the
complete write/verify/flush interval. A fresh privileged run passed both
bidirectional checks, exact image verification, lock-safe ``boot-ready``, and
post-flash ARM handoff. The retained
``cm4-continuous-guest-evidence-v1.json`` binds the exact artifact hashes,
USB/SCSI/ACM identity, event sequence, report hash, and explicit non-HIL
boundary. This closes the modeled-console attachment item in USB-008 and
USB-013. Their remaining software stress/fuzz/cross-host work keeps both rows
Partial, so the virtualization score remains **63.1%** and Pass 2 remains
**0 of 11 (0.0%)**.

The same retained gate now runs an optional four-reset composite campaign
after Imager verification. Every iteration resolves the exact USB bus/device
from the already-qualified eMMC ancestry, resets only that dummy-hcd device,
requalifies USB/SCSI/capacity and ACM identity, requires a direct 1 MiB host
SCSI read whose SHA-256 matches the QEMU backend, and proves a new
bidirectional guest ACM echo. Transient kernel I/O errors while the device is
offline are not counted as recovery; the campaign advances only after exact
post-reset block data and ACM traffic both succeed. This strengthens
USB-005/006/007/008/013 and TEST-008 without yet satisfying their
long-duration, fuzz, cross-host, timing, or physical gates, so percentages
remain unchanged.

Latest multi-memory production-flow delta: the identical pinned EEPROM,
``bootcode4.bin``, ``config.txt``, 29,360,640-byte ``boot.img``, Imager
binary, image payload, and 4 GiB eMMC capacity now complete the unchanged
RPIBOOT → continued guest ACM+MSD → Imager write/verify/flush → post-flash
ARM handoff sequence on CM4 ``-m 1G``, ``2G``, ``4G``, and ``8G``. The 8 GiB
gate exposed that the production DTB's one-cell memory-size encoding cannot
represent its single 4 GiB + 64 MiB high range. QEMU now retains the exact
BCM2711 64 MiB peripheral hole while describing that contiguous high DRAM as
representable 2 GiB, 2 GiB, and 64 MiB banks. A focused all-SKU qtest uses
the production one-cell encoding for 8 GiB and checks every resulting base
and size. The supervisor's default bounded wait is 60 seconds because the
unchanged 8 GiB guest reaches its gadget later than the smaller SKUs. The
retained ``cm4-multi-memory-evidence-v1.json`` binds all four report hashes
to one artifact-set hash. SOC-002 was already Full, so the Pass 1 score
remains **63.1%** and Pass 2 remains **0 of 11 (0.0%)**.

Latest SPI0 DMA/timing delta: the BCM2835-compatible SPI0 block no longer treats
``DMAEN`` as unsupported. It consumes the documented first DMA framing word
(``DLEN`` plus low CS controls), transfers subsequent 32-bit FIFO words as
little-endian bytes, stops at the exact programmed byte count, and retains
trailing RX bytes for the independent receive channel. The four programmable
``DC`` thresholds now drive TX/RX DREQ and panic signals into BCM2711 DMA
peripheral maps 6/7. Three physical chip-select outputs follow TA, selected
CE, per-line polarity, and ADCS end-of-frame deassertion. VMState v2 preserves
the active byte count, current byte deadline, and completion state and
regenerates CS, DREQ, panic, FIFO flags, and IRQ after migration. Each byte now
completes after eight SCLK cycles derived from CPRMAN's VPU core clock and the
programmed even-rounded ``CLK`` divisor (including the documented zero-divisor
encoding). Clock stop/resume and divider changes preserve remaining byte
cycles. An end-to-end qtest freezes a half-transmitted byte by stopping VPU,
resumes it at the retained deadline, drives a paced TX DMA framing/data
sequence, migrates halfway through its first byte with DMA held, drains the
completed RX FIFO through RX DMA, and checks interrupt, ADCS, reset, and
register defaults. This materially strengthens GPIO-012 and SOC-005, but
bit-edge/CPOL/CPHA waveform conformance, LoSSI, remaining error behavior,
auxiliary SPI, production guest-driver coverage, and physical conformance keep
GPIO-012 Partial; overall Pass 1 remains **63.1%**.

Latest auxiliary-SPI delta: the shared BCM2835 AUX block no longer rejects
enable bits 1/2. Both production register windows implement four-entry
32-bit TX/RX FIFOs, fixed or FIFO-supplied transfer widths, IO versus TXHOLD
frame termination, status levels, peek/pop behavior, TX-empty and idle
interrupts, and enable-gated register access. ``aux-spi1`` and ``aux-spi2``
are independent SSI buses; native CS patterns drive attached peripherals and
three observable CS lines per controller. Entries are paced from CPRMAN VPU
clock at ``core / (2 * (speed + 1))``; clock stop/resume, FIFO progress,
active/held CS, IRQ state, and remaining core cycles migrate in AUX VMState
v5. A qtest reads the real ``EF 40 14`` identity from an attached W25Q80BL
through a held SPI1 frame, independently starts SPI2, stops VPU halfway
through the entry, migrates while busy, resumes, and verifies reset and shared
IRQ state. A production functional gate additionally boots the pinned,
unchanged 5.15 Raspberry Pi kernel with the official byte-unchanged
``spi1-1cs.dtbo``, ``spi2-1cs.dtbo``, ``spi-bcm2835aux.ko.xz``, and
``spidev.ko.xz`` artifacts. Both stock platform drivers bind, both
``/dev/spidev`` nodes appear, and writes complete to attached SPI1/SPI2 flash
devices. GPIO-012 remains Partial because arbitrary-bit edge behavior,
CPOL/input/output-edge conformance, special post-input/hold modes, native-CS
quirks, and physical conformance remain; Pass 1 therefore remains **63.1%**.

Latest fatal-boot recovery delta: the current Pi 4 bootloader's documented
``REBOOT_ON_FATAL_ERROR`` policy is now parsed strictly and defaults on.
Unsupported BOOT_ORDER nibbles and source exhaustion without an explicit STOP
enter a deterministic three-pattern wait and then trigger the modeled BCM
watchdog hard-reset path. Setting the property to zero retains the exact fatal
observation until an external reset. Read-only QOM exposes the active policy,
remaining deadline, and a warm-reset-persistent reboot counter. Behavioral
boot VMState v60 preserves the policy, source, counter, and exact remaining
deadline; a qtest migrates after one pattern, proves the destination retains
two seconds, observes the watchdog RESET, and verifies that the unchanged
EEPROM starts a fresh fatal cycle with the counter incremented. The standard
unchanged production-artifact boot gate requires the default-on policy.
BOOT-009/010 remain Partial because the behavioral pattern interval still
requires a pinned physical cadence oracle and broader fatal-error trace
comparison; Pass 1 therefore remains **63.1%** and Pass 2 remains
**0 of 11 (0.0%)**.

Latest EEPROM-to-firmware configuration delta: ``bootconf.txt`` now recognizes
the documented ``[config.txt]`` section and appends every following byte to
the selected media configuration. The parser preserves conditional-filter
state across the boundary, resolves appended includes from the same SD/eMMC,
USB, NVMe, or network source, and accepts the EEPROM suffix as the complete
configuration when media ``config.txt`` is absent. Read-only QOM reports the
exact suffix size and SHA-256, while behavioral boot VMState v61 retains its
bytes through a pending boot migration. A qtest selects cut-down firmware
from an EEPROM-only suffix on both SD and network paths and migrates midway
through an EEPROM-supplied delay. A production functional gate pins and runs
the official ``rpi-eeprom-config`` tool against the official 512 KiB image,
then reaches userspace with unchanged start/fixup/kernel/DTB/initramfs bytes
while observing the EEPROM-supplied SDRAM request. This strengthens BOOT-007,
BOOT-013, and TEST-001; hardware-oracle result/cadence work keeps the rows
Partial, so Pass 1 remains **63.1%** and Pass 2 remains
**0 of 11 (0.0%)**.

Latest bootloader-UART delta: EEPROM ``BOOT_UART`` is now parsed strictly,
defaults off, and when enabled selects the Pi 4 primary PL011 UART0 on
GPIO14/GPIO15 ALT0 at 115200 8N1. The clean-room trace records boot-order
entry, every source attempt, and second-stage handoff; ``uart_2ndstage=1``
takes ownership only after that handoff. Read-only QOM exposes enablement,
active ownership, byte/line counters, and the explicit modeled format.
Behavioral boot VMState v62 preserves active ownership and counters without
replaying output after migration. A qtest covers SD handoff, invalid values,
and migration during a USB-discovery deadline; the production gate uses the
official unchanged ``rpi-eeprom-config`` tool to enable ``BOOT_UART`` in the
pinned EEPROM while booting the unchanged release artifacts. This strengthens
BOOT-007, BOOT-009, and TEST-001, while private bootloader text/cadence and
electrical UART validation remain for the physical-reference/HIL pass. Pass 1
therefore remains **63.1%** and Pass 2 remains **0 of 11 (0.0%)**.

Latest CM4 external-VL805 delta: EEPROM ``VL805`` is now parsed strictly,
defaults off, and gates bootloader initialization of a separately attached
PCIe VL805/xHCI controller on CM4. Mode 4 with explicit xHCI wiring and no
``VL805=1`` fails immediately to the next ``BOOT_ORDER`` source without
issuing controller traffic; Pi 4B's onboard VL805 remains unaffected.
Read-only QOM distinguishes EEPROM enablement, successful realized-controller
initialization, disabled policy, and missing-controller topology. Behavioral
boot VMState v64 retains the policy and initialized state. A qtest covers
enabled handoff, disabled STOP fallback, reset, invalid values, and migration
during the exact USB startup delay. A standard functional gate uses the
official unchanged ``rpi-eeprom-config`` tool to enable ``VL805=1`` in the
pinned 512 KiB EEPROM, then boots unchanged start/fixup/kernel/CM4-DTB/initramfs
bytes through guest-DMA xHCI/BOT to userspace. Opaque VL805 MCU instruction
execution, firmware-load cadence, and physical port behavior
remain explicit conformance/HIL work. This strengthens BOOT-009, USB-004,
USB-010, and TEST-001 without changing their Partial/Full status, so Pass 1
remains **63.1%** and Pass 2 remains **0 of 11 (0.0%)**.

Latest legacy-baseline delta: the upstream direct-kernel ``ttyAMA0`` test is
again attached to serial index zero, matching the unchanged PL011
``serial_hd(0)`` wiring. The accidental serial-index-one selection had routed
the test socket to the AUX mini-UART and could wait indefinitely despite a
running guest. Both upstream direct-kernel subtests now pass on the current
binary, while production mini-UART gates retain serial index one. This restores
the TEST-002 evidence without changing any emulator behavior or percentage.

Latest SD-policy delta: Pi 4B EEPROM ``SD_QUIRKS`` now accepts only the
documented BCM2711 bit field values zero and one. Bit zero disables bootloader
high-speed SD policy and publishes the documented 12.5 MHz clock ceiling
through read-only QOM; reserved bits invalidate the EEPROM configuration. The
Pi 4B SDHCI Host Control register now has high speed cleared and Clock Control
selects the fastest modeled divisor below that ceiling: 8,666,666 Hz from the
advertised 52 MHz base clock. Applied-controller QOM reports both values. CM4
parses the common EEPROM but does not apply the Pi 4B-only SD-card quirk.
Reset re-reads and reapplies the policy, including after live migration, while
VMState v64 migrates the boot policy and SDHCI migrates its register state.
Existing SD over-current qtests now cover default/limited policy, exact
guest-visible controller registers, invalid reserved bits, CM4
non-application, reset, an active timed migration, and reset of the migrated
destination. Behavioral file reads are not clock-paced; actual waveform,
marginal-card interoperability, and calibrated transfer cadence remain
hardware-oracle/HIL work. This strengthens STO-002, BOOT-009, BOOT-010, and
TEST-001 without changing their statuses, so Pass 1 remains **63.1%** and
Pass 2 remains **0 of 11 (0.0%)**.

Latest PCIe-root delta: the Pi 4B and CM4 root port no longer exposes the
generic QEMU ``1b36:000c`` identity or its unrealistic Gen4 x32 maximum link.
A dedicated inherited root-port type now exposes the hardware-visible
Broadcom BCM2711 ``14e4:2711`` identity, revision ``20``, PCI bridge class
``060400``, and Gen2 x1 maximum link. Pi 4B/VL805 and CM4/NVMe qtests validate
the exact configuration space, endpoint access, reset restoration, controller
register migration, downstream-device migration, and retained root identity.
The unchanged production EEPROM/firmware/kernel path independently verifies
the root vendor, device, revision, and class through Linux PCI sysfs. The stale
documentation entry that listed PCIe as missing is removed. SSC, remaining
controller registers/errors, calibrated timing, and electrical
link behavior remain conformance/HIL work. This strengthens PER-001 and
TEST-001 without changing their statuses, so Pass 1 remains **63.1%** and
Pass 2 remains **0 of 11 (0.0%)**.

Latest PCIe-MSI delta: the final production DT now retains the BCM2711
``msi-controller`` and self-referential ``msi-parent`` binding instead of
forcing every endpoint onto legacy INTx. The native controller implements the
documented below-4-GiB ``0xfffffffc`` and above-4-GiB ``0xffffffffc``
doorbells, 32-vector low-16-bit data matching, interrupt status, W1C clear,
mask-set, mask-clear, and GIC SPI 148. Both target choices are qtested because
the driver selects between them from the rounded inbound RAM aperture.
Masked, pending, cleared, reset, and live-migrated vector state is verified
through controller registers and the GIC distributor. Pi 4B VL805 now uses
MSI rather than its compatibility INTx path. The unchanged production kernel
independently creates MSI IRQs for both VL805 and NVMe, completes xHCI and
NVMe initialization, performs real storage I/O, verifies durable NVMe bytes,
and passes PCI function reset. SSC, remaining controller errors/registers,
opaque VL805 firmware behavior, calibrated timing, and electrical link
behavior remain conformance/HIL work. This strengthens PER-001, USB-010,
PER-005, and TEST-001 without changing their statuses, so Pass 1 remains
**63.1%** and Pass 2 remains **0 of 11 (0.0%)**.

Latest firmware-reservation delta: behavioral firmware now retains the exact
``bootconf.txt`` and 264-byte ``pubkey.bin`` records parsed from the unchanged
SPI EEPROM. When the final production DT provides its
``raspberrypi,bootloader-config`` and
``raspberrypi,bootloader-public-key`` placeholders, QEMU allocates
64-byte-aligned ranges immediately below the effective ARM/VideoCore split,
writes the byte-identical records into guest RAM, updates each reserved-memory
``reg`` tuple, and changes only the corresponding node to ``status = "okay"``.
Absent records or DT placeholders remain unadvertised, so no hidden range is
removed from guest RAM. Kernel, initramfs, explicit DT placement, and automatic
top-down DT placement now reject overlap with these firmware-owned records.
Pi 4B and CM4 qtests verify exact DT tuples and bytes across the 128 MiB
``total_mem`` minimum and every 1/2/4/8 GiB model, reset, and live migration.
Unchanged production Pi 4B SD and CM4 eMMC boots remain green. This strengthens
SOC-002, SOC-009, BOOT-015, BOOT-016, BOOT-017, and TEST-001 without changing
their statuses, so Pass 1 remains **63.1%** and Pass 2 remains
**0 of 11 (0.0%)**.

Latest firmware-system-identity delta: behavioral firmware now creates the
production ``/system`` node when absent and always replaces
``linux,revision`` and the two-cell ``linux,serial`` with the same selected
board revision and OTP row-28 serial used by the machine and firmware mailbox.
It also replaces the root ``serial-number`` with the production 16-digit,
lower-case, zero-padded hexadecimal representation of that OTP value. The
final packed DT therefore cannot retain stale media-supplied identity.
ARM32 raw-media qtests validate exact big-endian property widths and values
for Pi 4B and CM4 at 1/2/4/8 GiB, a nonzero persistent OTP serial is checked
against the mailbox response, and reset plus live migration retain the same
identity, including exact root-string formatting. This strengthens SOC-002,
BOOT-015, BOOT-016, and TEST-001 without
changing their statuses, so Pass 1 remains **63.1%** and Pass 2 remains
**0 of 11 (0.0%)**.

Latest firmware-KASLR delta: final DT generation no longer retains a
media-supplied ``/chosen/kaslr-seed``. Behavioral firmware obtains a fresh
64-bit value from QEMU's guest-visible entropy API and writes it in the
production big-endian two-cell form before packing the DT. Entropy acquisition
fails the handoff instead of silently exposing stale input. Raw-media qtests
prove stale-value replacement across every Pi 4B/CM4 1/2/4/8 GiB model, a
fresh value after reset, exact retention across live migration, and repeatable
initial values across fresh VMs when QEMU's explicit deterministic ``-seed``
mode is selected. This strengthens BOOT-015, BOOT-016, BOOT-017, and TEST-001
without changing their statuses, so Pass 1 remains **63.1%** and Pass 2
remains **0 of 11 (0.0%)**.

Latest minimum-bootloader-version delta: the current official BCM2711
2026-05-17 release embeds ``MFG_VER: 1`` and documents the board-manufacturing
minimum at ``/chosen/rpi-min-boot-ver``. Pi 4B and CM4 now accept a
construction-only ``min-boot-version`` board attribute and behavioral firmware
always replaces the final DT property with its big-endian 32-bit value, so
unchanged ``rpi-eeprom-update`` can perform its normal candidate-image check.
The default zero models boards without a programmed minimum; value one models
the current manufacturing generation. Qtests cover stale input replacement
for all eight Pi 4B/CM4 memory models, nonzero reset persistence, runtime
mutation rejection, and VMState v66 migration against a deliberately
conflicting destination value. The private physical OTP bit/row encoding is
not fabricated and remains an explicit hardware-oracle/HIL item. This
strengthens BOOT-003, BOOT-007, BOOT-015, BOOT-016, and TEST-001 without
changing their statuses, so Pass 1 remains **63.1%** and Pass 2 remains
**0 of 11 (0.0%)**.

Latest installed-SDRAM-identity delta: the current official BCM2711 EEPROM
firmware reports fitted SDRAM in gigabits at
``/chosen/rpi-sdram-size-gbit``. Behavioral firmware now always replaces that
property from the authoritative 1/2/4/8 GiB machine selection, yielding the
production big-endian 32-bit values 8/16/32/64. The property remains tied to
installed SDRAM when ``total_mem`` reduces the effective memory map or
``gpu_mem`` changes the ARM/VideoCore split. Qtests cover stale input
replacement across all eight Pi 4B/CM4 memory models, every existing
``total_mem`` boundary case, reset, and live migration. This strengthens
SOC-002, SOC-009, BOOT-015, BOOT-016, and TEST-001 without changing their
statuses, so Pass 1 remains **63.1%** and Pass 2 remains
**0 of 11 (0.0%)**.

Latest extended-board-revision delta: official firmware defines
``/chosen/rpi-boardrev-ext`` as the big-endian 32-bit value from OTP row 33.
Behavioral firmware now obtains it directly from the existing persistent
66-row OTP backend and always replaces stale media input in the final DT.
It remains independent of the normal row-30 board revision; an unprogrammed
row reports zero. Qtests cover zero replacement across all eight Pi 4B/CM4
memory models and an exact nonzero value across reset, fresh restart, and live
migration. This strengthens SOC-002, BOOT-003, BOOT-015, BOOT-016, and
TEST-001 without changing their statuses, so Pass 1 remains **63.1%** and
Pass 2 remains **0 of 11 (0.0%)**.

Latest firmware-prefix-identity delta: behavioral firmware now always
replaces the documented ``/chosen/os_prefix`` and
``/chosen/overlay_prefix`` strings with the effective values selected by the
unchanged ``config.txt`` parser. The value is written after prefix fallback
validation, so it describes the paths actually used for artifact loading.
Qtests replace stale DT input for all Pi 4B/CM4 memory models, cover the
production defaults and an explicit ``os_prefix=alt/`` plus empty
``overlay_prefix``, and retain both values over reset and live migration.
This strengthens BOOT-014, BOOT-015, BOOT-016, and TEST-001 without changing
their statuses, so Pass 1 remains **63.1%** and Pass 2 remains
**0 of 11 (0.0%)**.

Latest boot-mode identity delta: behavioral firmware now publishes the
successful ``BOOT_ORDER`` nibble as the documented big-endian
``/chosen/bootloader/boot-mode`` cell. The value is synchronized at the final
ARM handoff so failed earlier sources cannot leak stale state. Read-only QOM
exposes the same selected mode, VMState v67 preserves it, and qtests cover SD
mode 1, network mode 2, USB modes 4/5, NVMe mode 6, HTTP mode 7, reset, stale
DT replacement, and NVMe/SD live migration. This strengthens BOOT-009,
BOOT-015, BOOT-016, and TEST-001 without changing their statuses, so Pass 1
remains **63.1%** and Pass 2 remains **0 of 11 (0.0%)**.

Latest EEPROM-build-identity delta: behavioral ROM now extracts the bounded
``BUILD_TIMESTAMP=<epoch>`` and ``VERSION:<git>`` strings from the unchanged
512 KiB BCM2711 EEPROM image. Firmware replaces stale media-DT input with the
documented big-endian ``/chosen/bootloader/build_timestamp`` cell and
``/chosen/bootloader/version`` string; missing or malformed metadata is
omitted rather than fabricated. Read-only QOM exposes the parsed values,
VMState v68 preserves them, and qtests cover Pi 4B/CM4, every memory SKU,
stale replacement, reset, fresh restart, and live migration. The production
Pi 4B SD and CM4 eMMC gates assert the exact 2026-05-17 EEPROM values. This
strengthens BOOT-007, BOOT-015, BOOT-016, and TEST-001 without changing their
statuses, so Pass 1 remains **63.1%** and Pass 2 remains
**0 of 11 (0.0%)**.

Latest secure-boot-identity delta: EEPROM ``SIGNED_BOOT`` is now parsed as a
strict boolean and the final firmware DT always replaces
``/chosen/bootloader/signed`` with the documented public bit field. Bit zero
tracks the active EEPROM setting, bit two maps the persistent BCM2711
development-key-revocation fuse, and bit three reports the programmed customer
public-key digest; private physical OTP bit positions are not leaked into the
DT ABI. QOM exposes the same value and VMState v69 preserves it. Qtests cover
unsigned stale replacement (zero), signed customer-key mode (``0x9``),
production revocation mode (``0xd``), malformed EEPROM rejection, reset, and
mid-HTTP live migration. The unchanged production Pi 4B/CM4 EEPROM gates
assert zero. This strengthens BOOT-003, BOOT-007, BOOT-015, BOOT-016,
SYS-008, and TEST-001 without changing their statuses, so Pass 1 remains
**63.1%** and Pass 2 remains **0 of 11 (0.0%)**.

Latest EEPROM-update-identity delta: both SD ``recovery.bin`` flashing and
boot-filesystem self-update now parse the official ``rpi-eeprom-digest``
``ts: <epoch>`` line from the unchanged ``pieeprom.sig``.  Only a
digest-verified, fully programmed EEPROM commits the value; after that,
missing or non-increasing self-update timestamps are rejected as stale, while
malformed, write-protected, failed, stale, and already-current paths retain the
previous timestamp.  Firmware replaces stale media-DT input with the documented
big-endian ``/chosen/bootloader/update_timestamp`` cell, while absent metadata
removes it.  Read-only QOM exposes validity/value, VMState v70 retains it over
live migration, and the backward-compatible 512-byte EEPROM-status sector
persists it across independent QEMU processes without modifying the supplied
EEPROM ``.bin``.  Qtests cover successful update, reset, migration, restart,
malformed metadata, stale-value retention, and recovery write-protect
persistence.  This strengthens STO-004, STO-005, BOOT-006, BOOT-007,
BOOT-015, BOOT-016, and TEST-001 without changing their row statuses, so Pass
1 remains **63.1% weighted / 64.2% unweighted** and Pass 2 remains
**0 of 11 (0.0%)**.

Latest EEPROM-capabilities delta: behavioral firmware now publishes the
documented big-endian ``/chosen/bootloader/capabilities`` cell from
evidence-bounded BCM2711 release identity rather than from the boot source or
QEMU's own feature set.  Public hardware output pins ``0x1f`` to the
2020-12-11 build and ``0x7f`` from the 2021-07-06 build through the exact
2026-05-17 production EEPROM (timestamp ``1779045198``, version
``224877da``).  Older/intermediate images outside those proven families omit
the property, removing stale media-DT input without guessing.  Read-only QOM
exposes validity/value, VMState v71 preserves both, and all Pi 4B/CM4 RAM-model
tests plus reset, fresh restart, and migration assert the current production
value.  This strengthens BOOT-007, BOOT-015, BOOT-016, and TEST-001 without
changing their row statuses, so Pass 1 remains **63.1% weighted / 64.2%
unweighted** and Pass 2 remains **0 of 11 (0.0%)**.

Latest USB-boot-identity delta: the firmware-owned VL805/xHCI and BCM2711
DWC2 enumeration paths now retain the topology of the candidate that actually
reached ARM handoff.  The final DT replaces
``/chosen/bootloader/usb/{usb-version,route-string,root-hub-port-number,lun}``
from the selected device descriptor, root port, one-hub route, and LUN;
non-USB fallback removes a stale input subtree.  This covers direct USB 3,
direct/companion USB 2, multi-device root ports, DWC2 hub routes, and
multi-LUN selection without deriving identity from a launch option.  Read-only
QOM mirrors the values and VMState v72 preserves them.  Qtests cover modes 4
and 5, reset, external devices, hot insertion, USB 2/3 selection, one hub,
multiple devices/LUNs, stale-node removal across every Pi 4B/CM4 RAM model,
and live migration.  This strengthens BOOT-009, BOOT-015, BOOT-016, USB-003,
USB-004, and TEST-001 without changing their remaining timing/HIL row
statuses, so Pass 1 remains **63.1% weighted / 64.2% unweighted** and Pass 2
remains **0 of 11 (0.0%)**.

Latest reset-status ABI delta: final Pi 4B and CM4 firmware DTs now publish
the raw big-endian reset status at the documented
``/chosen/bootloader/pm_rsts`` property.  The previous nonstandard ``rsts``
name is no longer emitted and is removed when present in an input media DT.
The common bootloader-state assertion covers partition and tryboot selection,
reset, live migration, and all eight Pi 4B/CM4 RAM models.  This corrects a
guest-visible BOOT-009/BOOT-015/SYS-002 ABI mismatch without changing their
remaining differential/HIL work, so Pass 1 remains **63.1% weighted / 64.2%
unweighted** and Pass 2 remains **0 of 11 (0.0%)**.

Latest halt/power-policy delta: Pi 4B and CM4 now parse strict EEPROM
``WAKE_ON_GPIO`` and ``POWER_OFF_ON_HALT`` booleans and apply them to the real
PM watchdog partition-63 halt path.  GPIO wake suspends the VM until a falling
GPIO3 edge arrives through either qtest or the existing real socket/USB QGPIO
bridge; GPIO-disabled halt waits for falling ``global-en``; and the documented
``WAKE_ON_GPIO=0`` plus ``POWER_OFF_ON_HALT=1`` combination emits guest
shutdown.  QOM exposes both policies and distinct running, GPIO-wake,
GLOBAL_EN-wait, and powered-off states.  Power-management VMState v4 retains
the policy and input state, and qtests cover both platforms, invalid values,
reset, live migration, the real socket bridge, GPIO3 rejection when disabled,
GLOBAL_EN wake, and ``-no-shutdown`` observation.  The expanded full behavioral
suite passes all **236 qtests**; unchanged production Pi 4B SD and CM4 eMMC
boots also pass.  Electrical PMIC rails and physical timing remain Pass 2 HIL,
so Pass 1 remains **63.1% weighted / 64.2% unweighted** and Pass 2 remains
**0 of 11 (0.0%)**.

Latest Network Install keyboard delta: the unchanged EEPROM ``.bin`` now
controls the boot-time keyboard scan through strict
``NET_INSTALL_KEYBOARD_WAIT`` parsing over the complete unsigned 32-bit
millisecond range, including the documented 900 ms default and zero-disable
behavior.  Pi 4B defaults the installer on, CM4 defaults it off, and
``NET_INSTALL_AT_POWER_ON=1`` overrides a conflicting disabled setting without
silently selecting mode 7.  Host-visible keyboard-presence and Shift inputs
can select mode 7
during the active window; timeout resumes the byte-unchanged configured
``BOOT_ORDER``.  QOM exposes the parsed policy, active state, and exact
remaining deadline.  Behavioral-boot VMState v73 preserves an in-flight scan
across migration.  Qtests cover the exact deadline, runtime Shift selection,
reset, migration, disabled/zero policy, and invalid values.  This strengthens
BOOT-006, BOOT-015, PER-004, and TEST-001 without changing their remaining
differential/HIL work, so Pass 1 remains **63.1% weighted / 64.2%
unweighted** and Pass 2 remains **0 of 11 (0.0%)**.

Latest ProxyDHCP policy delta: unchanged EEPROM binaries now control
ProxyDHCP selection through ``PXE_OPTION43``, with the documented
``Raspberry Pi Boot`` default and a strict nonempty printable override up to
the 255-byte DHCP option limit.  Address-bearing DHCP offers remain
unaffected; zero-address proxy offers must contain the configured match bytes
or they are ignored without changing the active transaction.  A real socket
qtest rejects the default string under a custom policy, accepts the custom
string, migrates before the main DHCP ACK, and proves that the selected proxy
TFTP address becomes the outgoing ARP target.  Reset, default/custom QOM,
empty/oversized values, and VMState v74 are covered.  This strengthens
BOOT-006, BOOT-015, PER-002, and TEST-001 without changing their remaining
differential/HIL work, so Pass 1 remains **63.1% weighted / 64.2%
unweighted** and Pass 2 remains **0 of 11 (0.0%)**.

Latest Imager repository handoff delta: ``IMAGER_REPO_URL`` needs no
emulator-only parser or replacement command line. The official network
installer's unchanged ``rpi-imager`` reads
``/sys/bus/nvmem/devices/rmem0/nvmem`` and extracts
``IMAGER_REPO_URL=`` from the raw EEPROM boot configuration. QEMU already
copies those exact ``bootconf.txt`` bytes into aligned reserved guest memory
and enables the official ``raspberrypi,bootloader-config`` /
``nvmem-rmem`` DT node. A focused qtest now supplies the same configured
EEPROM byte stream to Pi 4B and CM4, verifies the complete URL-bearing bytes
through that guest-visible node, resets both platforms, live-migrates both,
and verifies the bytes again. The full suite passes all **237 qtests**. This
strengthens BOOT-015, BOOT-016, PER-004, and TEST-001 without changing their
remaining boot-to-userspace, differential, and HIL work, so Pass 1 remains
**63.1% weighted / 64.2% unweighted** and Pass 2 remains
**0 of 11 (0.0%)**.

Latest Network Install display-policy delta: unchanged EEPROM binaries now
apply the documented ``DISABLE_HDMI=1`` interaction before any physical,
keyboard, Shift, or ``NET_INSTALL_AT_POWER_ON`` request can select the
embedded installer. The effective read-only Network Install policy is disabled
on both Pi 4B and CM4 while an explicit HTTP ``BOOT_ORDER=7`` attempt remains
independent. Values zero and reserved value two do not disable HDMI; negative
or greater-than-32-bit values fail the EEPROM configuration. Behavioral-boot
VMState v75 retains the effective policy. Qtests cover both platforms,
conflicting enable/at-power-on settings, host request plus keyboard and Shift,
reset, live migration, explicit HTTP selection, reserved values, and malformed
boundaries. The full suite passes all **238 qtests**. This strengthens
BOOT-009, BOOT-015, PER-004, and TEST-001 without changing their remaining
diagnostic-rendering, boot-to-userspace, differential, and HIL work, so Pass 1
remains **63.1% weighted / 64.2% unweighted** and Pass 2 remains
**0 of 11 (0.0%)**.

Latest USB recovery-to-installer delta: with Network Install enabled and
``NET_INSTALL_KEYBOARD_WAIT=0``, a held Shift key discovered during a
``BOOT_ORDER`` mode-4 USB scan now launches the installer when readable USB
media contains no Pi boot files. The transition is one-shot per reset and
uses the existing unchanged HTTP/HTTPS ``boot.sig``/``boot.img`` path; failure
there resumes the byte-unchanged configured boot order without looping back
into the override. It does not activate for an absent USB device, transport or
artifact read errors, a missing Shift key, mode 5, disabled Network Install,
or ``DISABLE_HDMI=1``. Read-only QOM distinguishes missing boot files from
fallback consumption, and behavioral-boot VMState v76 preserves both across
the USB LUN deadline. A qtest migrates after 40 ms of a 100 ms LUN wait,
reaches the exact network firmware handoff at the remaining deadline, repeats
after reset, and covers every exclusion boundary. The full suite passes all
**239 qtests**. This strengthens BOOT-009, BOOT-010, USB-004, PER-004, and
TEST-001 without changing their remaining timing, boot-to-userspace,
differential, and HIL work, so Pass 1 remains **63.1% weighted / 64.2%
unweighted** and Pass 2 remains **0 of 11 (0.0%)**.

Latest HTTP-host policy delta: unchanged EEPROM binaries now follow the
documented rule that an invalid ``HTTP_HOST`` is ignored instead of invalidating
the complete EEPROM configuration. Malformed uppercase, leading-hyphen, and
empty-label hostnames therefore select the default-host path; valid custom
hosts remain subject to the existing signed customer-key policy. A focused
qtest exercises Pi 4B and CM4, proves the malformed value is absent from the
effective configuration while HTTP proceeds to DHCP, repeats after reset, and
live-migrates the pending attempt. That migration test also found and fixed an
observation-only drift where an HTTP-mode DHCP wait changed its reported source
from ``network`` to ``http`` after migration. The full suite passes all **240
qtests**. This strengthens BOOT-007, BOOT-009, BOOT-015, PER-004, and TEST-001
without changing their remaining boot-to-userspace, differential, or HIL work,
so Pass 1 remains **63.1% weighted / 64.2% unweighted** and Pass 2 remains
**0 of 11 (0.0%)**.

Latest partition-walk default-policy delta: unchanged current EEPROM binaries
that omit ``PARTITION_WALK`` now retain the documented enabled default and walk
away from an unbootable requested partition. Explicit ``PARTITION_WALK=0``
disables that recovery search. Read-only QOM exposes the effective policy, and
a focused qtest covers Pi 4B SD and CM4 eMMC, reset, live migration, malformed
``autoboot.txt``, and rejection of invalid ``PARTITION_WALK=2``. The full suite
passes all **241 qtests**. This strengthens
BOOT-009, BOOT-015, and TEST-001 without changing their remaining
boot-to-userspace, differential, or HIL work, so Pass 1 remains **63.1%
weighted / 64.2% unweighted** and Pass 2 remains **0 of 11 (0.0%)**.

Latest HDMI diagnostics-policy delta: unchanged EEPROM binaries now parse
``HDMI_DELAY`` over the complete unsigned 32-bit range with the documented
five-second default. The independent virtual deadline exposes pending,
visible, and remaining-time observations; successful ARM handoff cancels it,
``HDMI_DELAY=0`` displays immediately, fatal errors bypass the delay, and
``DISABLE_HDMI=1`` suppresses diagnostics. Behavioral-boot VMState v77
preserves the deadline. A qtest covers Pi 4B and CM4 default timing, exact
expiry, reset, live migration, immediate, disabled, invalid, fatal, and
successful-handoff boundaries. The full suite passes all **242 qtests**. This
strengthens BOOT-009, BOOT-015, PER-008, and TEST-001 without claiming the
private diagnostic artwork or physical HDMI timing, so Pass 1 remains
**63.1% weighted / 64.2% unweighted** and Pass 2 remains
**0 of 11 (0.0%)**.

Latest target-identity delta: both foreground mass-storage backends now select
the stable serial-bound whole-disk link while explicitly excluding udev
``-partN`` links. A previously flashed image with two partitions therefore
resolves to one eMMC owner instead of failing as three apparent targets;
multiple distinct whole disks still fail closed. Inspection of the unchanged
pinned ``mass-storage-gadget64/boot.img`` also records the remaining exact
identity boundary: its ``configure-gadgets`` service creates Broadcom
``0a5c:0104``, product ``Raspberry Pi multi-function USB device``, one or more
MSD functions with device-name inquiry strings, and ACM serial. The Raw
Gadget target now implements the one-eMMC form of that composite profile:
three interfaces, bus-powered 500 mA configuration, the official strings,
``mmcblk0`` SCSI identity, CDC line-coding/control requests, and a live ACM
data pair. A privileged gate proves exact host enumeration, `/dev/disk/by-id`
storage, `/dev/ttyACM*` discovery and 115200-byte round-trip both before and
after the complete BOT corpus and reset recovery. The portable configfs helper
remains a storage-only compatibility backend. This closes the command-visible
descriptor/identity item and a real re-enumeration bug, but the affected USB
rows retain their remaining reset-storm, cross-host, console-backend, timing,
and HIL work, so the overall virtualization-pass score remains **63.1%**.

Latest command-visible production-flash delta: the Raw Gadget BOT target now
has a foreground-owner mode for the successful path. It prevents a new SCSI
command from starting, waits for any active command to finish, durably syncs
the exact eMMC backend, and publishes `boot-ready` only after the supervisor
sends an exact `complete` acknowledgement; malformed input, EOF, and explicit
failure publish `flash-failed`. The supervisor retains configfs as its default
compatibility backend and adds `--mass-storage-mode raw-bot`, stable target
discovery from the fixed USB serial, and the same unchanged official Imager
write/verify/flush command. A fresh privileged run passed with the pinned
EEPROM, `bootcode4.bin`, `config.txt`, `boot.img`, Imager AppImage, and
Raspberry Pi OS `.img.xz`, then reached post-flash CM4 ARM handoff and
`qemu-stopped`. This closes the successful-production-ownership item in
USB-008/USB-013 and strengthens USB-011/TEST-008. Those parent rows retain
their remaining fuzz, reset-storm, cross-host, console/timing, and HIL
work, so the overall score remains **63.1%**.

Latest final-DT placement delta: behavioral firmware now parses the production
``device_tree_address`` and exclusive ``device_tree_end`` configuration
properties as strict unsigned addresses. An explicit address places the final
packed and overlaid DTB exactly there; an end-only limit bounds the existing
top-down placement. The common layout validator rejects kernel, initramfs,
low-memory firmware-state, end-bound, and low-RAM collisions before any DTB
bytes are written. Pi 4B SD and CM4 eMMC qtests prove exact placement and
bytes, invalid configuration, address/end failures, reset, and live migration.
This strengthens BOOT-013/BOOT-015 without changing their remaining
virtualization work or the **63.1%** overall score.

Latest ARM64 Image-contract delta: unchanged raw, gzip, and EFI-zboot
``kernel8.img`` inputs now require the complete 64-byte Linux Image header
instead of accepting a magic-bearing 61--63-byte prefix. The little-endian
``text_offset`` placement follows the boot protocol for the previously
uncovered sub-4-KiB nonzero range: it is added to a 2 MiB-aligned base, while a
zero offset retains the Pi firmware-compatible ``0x200000`` placement. A
Pi 4B SD/CM4 eMMC qtest proves that ``text_offset=0x100`` enters at
``0x200100`` before and after live migration and reset, and that a 63-byte
magic-bearing image fails closed as ``kernel-invalid`` without creating a
handoff address. This strengthens IMG-009/BOOT-014 and exact production
artifact validation without changing their partial status, so BOOT remains
**55.9%** and overall progress remains **63.1%**.

Latest ARM64 execution-state delta: Image-header flag bit 0 is no longer
ignored. Behavioral firmware now enters the primary and all three secondary
Cortex-A72 cores with ``SCTLR_EL2.EE`` matching the unchanged Image's declared
little- or big-endian data state. The legacy CPU-on API remains
little-endian-compatible while the Raspberry Pi handoff passes the explicit
state. Read-only ``arm-handoff-endianness`` makes the contract observable. The
Pi 4B SD/CM4 eMMC Image-header qtest now proves both endian selections, all-core
release, live migration, reset, and unchanged little-endian production
firmware boot. This closes another false-success path inside BOOT-016, but
physical firmware differential evidence remains outstanding, so BOOT and
overall percentages are unchanged.

Latest handoff-layout delta: an explicit initramfs address can no longer
collide with the exact low-memory primary entry stub, four-core spin table, or
architecture-specific secondary spin code and still report success. Firmware
now validates those occupied ranges for both initramfs and final DT before any
guest-memory write. It does not reserve an arbitrary low-memory block: the
first byte after the 44-byte ARM64 spin stub remains legal. Pi 4B SD and CM4
eMMC cases prove rejection at ``0x300``, exact unchanged initramfs bytes at the
valid ``0x32c`` boundary, live migration, and reset. The complete modeled load
region remains below 1 GiB, so kernel and initramfs also satisfy the ARM64
aligned physical-window rule. This strengthens BOOT-016 without removing its
remaining physical differential gate; percentages are unchanged.

Latest ARM32 handoff-layout delta: the collision contract now has independent
Pi 4B SD and CM4 eMMC ARM32 proof rather than relying on ARM64 coverage.
Unchanged ``kernel7l.img`` media reject initramfs placement inside the primary
entry stub at ``0x4``, spin table at ``0xd8``, and 40-byte secondary spin code
at ``0x300``. The exact first legal byte at ``0x328`` preserves the complete
initramfs across four-core release, reset, and live migration. This strengthens
BOOT-016 and TEST-001 without changing their status or overall percentage.

Latest ``cmdline.txt`` delta: the behavioral firmware now follows the
documented Raspberry Pi single-line contract. Only bytes before the first LF
or CRLF terminator are appended after firmware-owned DT ``bootargs``; later
lines remain covered by the unchanged file's exact size and SHA-256 evidence
but cannot inject additional kernel parameters. A single trailing NUL remains
accepted, while an embedded NUL fails closed as ``device-tree-invalid``.
Pi 4B SD/CM4 eMMC qtests prove LF, CRLF, serial-alias rewriting, ignored second
lines, exact hashes, embedded-NUL rejection, reset, and live migration. This
strengthens IMG-009/BOOT-015 without changing their partial status, so overall
progress remains **63.1%**.

Latest CM4 RPIBOOT-control delta: the in-process BCM2711 ROM no longer
acknowledges standard USB GET requests sent in the OUT direction or SET
requests sent in the IN direction. It stalls the matching EP0 direction,
prevents malformed SET_ADDRESS from changing ``DCFG.DEVADDR``, and recovers
through USB reset into the unchanged descriptor/vendor/bulk sequence. The
trusted, untrusted, and secure-provisioning RPIBOOT qtests all exercise these
negative controls before completing their existing exact-byte paths. This
strengthens USB-005/USB-006 without replacing the physical descriptor/timing
gate, so percentages remain unchanged.

Latest CM4 RPIBOOT-standard-control delta: all supported standard requests now
validate recipient, value, index, and length in addition to direction.
``SET_ADDRESS`` changes ``DCFG.DEVADDR`` only after the status-IN stage.
Configuration starts at zero, follows valid ``SET_CONFIGURATION(0|1)``, gates
interface and non-control endpoint access, clears on reset/disconnect, and
migrates in VMState v59. Device ``GET_STATUS`` reports self-power, while
endpoint-1 halt supports set/query/clear. Malformed configuration values stall
without mutation. Trusted, untrusted, secure-provisioning, reset, and active
migration qtests retain the unchanged RPIBOOT artifact sequence. This
strengthens USB-005/USB-006 without replacing physical descriptor/timing
conformance, so percentages remain unchanged.

Latest CM4 RPIBOOT-configuration-gating delta: configuration one now requires
a nonzero USB address, and configuration zero can no longer reach the vendor
RPIBOOT protocol or arm endpoint-1 bulk DMA. Reconfiguration disables endpoint
1, clears its transfer-register/DMA state, and rolls a partial bootcode or file
transfer back to the complete stage boundary. Every first-stage retry,
disconnect-driven second enumeration, file-server retry, and active-migration
test now performs SET_ADDRESS plus SET_CONFIGURATION before continuing with
the unchanged artifacts. Negative qtests prove address-zero configuration and
configuration-zero vendor/bulk traffic stall without progress. This
strengthens USB-005/USB-006 without replacing physical descriptor/timing
conformance, so percentages remain unchanged.

Latest CM4 supervisor-evidence delta: the opt-in functional gate now emits the
mandatory independently pinned EEPROM ``bootsys`` digest, key index one, and
ordered thirteen-dependency digest instead of an obsolete manifest that would
fail before provisioning. A fresh privileged 4 GiB run authenticates every
unchanged official artifact, completes official ``rpiboot``, Imager
write/verify/flush, verifies the exact 2,977,955,840-byte payload prefix, and
reaches post-flash ARM handoff with a complete atomic report. The refreshed
run uses the official-profile ACM+MSD Raw Gadget target and retains exact USB
identity, storage, ACM, reset-recovery, and BOT-corpus observations in the
evidence record. It also exposed and fixed unconditional ``modprobe`` calls:
the supervisor and
mass-storage owner now verify an already-loaded module and ``dummy_hcd``
high-speed parameter, while remaining fail-closed when the module is neither
loaded nor loadable. The compact
``contrib/raspi4/cm4-provision-evidence-v1.json`` retains the exact hashes,
event sequence, host-fixture terminal state, and explicit non-HIL boundary.
This refreshes USB-008/USB-011/USB-013 evidence without changing their row
statuses or the overall percentage.

Latest mailbox-structure delta: every firmware property request now receives a
complete read-only structural preflight before any tag is processed. The
declared buffer must contain its header, bounded padded tag extents, and an end
tag without overflowing the 32-bit VideoCore address space. Malformed,
truncated, overflowed, or missing-end buffers return the documented partial
error code before tag side effects. Tag traversal now rounds six-byte values
to their required 32-bit boundary instead of treating following padding as a
new header. A Pi 4B/CM4 qtest covers undersized buffers, oversized tag extents,
missing terminators, address overflow, non-mutation, and a valid six-byte MAC
response with untouched padding. Every tag response write is additionally
clipped to its declared value buffer while the response header retains the
full desired length. A chained short-buffer qtest proves that truncated board
revision, MAC, ARM-memory, and serial responses leave padding, following tag
headers, and the end marker untouched. The same whole-buffer preflight now
requires the ABI's zero request code and validates the value-buffer capacity
against the minimum payload consumed by each tag before any read or side
effect. It rejects reserved/response-coded requests, fixed-size underflow, and
truncated dynamic palette, customer-OTP, or private-key arrays. A Pi 4B/CM4
chained qtest proves that a valid reboot-flags write before a malformed clock
request remains unexecuted. The overall percentage is unchanged because this
closes robustness inside the existing partial mailbox capability rather than
completing another row.

Latest mailbox-feature-detection delta: unsupported property tags are now
ignored exactly at the tag boundary: their request code and value/padding stay
untouched, the buffer retains global success, and processing continues at the
next 32-bit-aligned tag. A chained Pi 4B/CM4 qtest places an odd-sized unknown
tag before supported board-model and board-revision requests and proves the
unknown header remains clear while both following responses are correct. The
overall percentage is unchanged.

Latest board-model delta: the documented `GET_BOARD_MODEL` property tag now
returns a successful four-byte response containing the legacy firmware value
zero instead of being treated as unsupported. Response clipping preserves the
full four-byte desired length: Pi 4B/CM4 qtests prove a two-byte caller buffer
receives only two zero bytes without overwriting padding or the end tag.
Identity tests cover the full response before reset, after reset, and after
live migration. A pinned differential Pi 4B/CM4 firmware trace is still
required for release conformance, so the overall percentage is unchanged.

Latest framebuffer-palette delta: `GET_PALETTE` now returns all 256 current
RGBA entries from the same VideoCore RAM table consumed by 8-bit scanout, and
reports the full 1,024-byte desired length when the caller supplies a shorter
buffer. `TEST_PALETTE` applies the same offset/length and complete-payload
validation as `SET_PALETTE` but never mutates the table. Pi 4B/CM4 qtests set
the last two entries, test valid replacement colors and an overflowing range,
prove the test did not mutate either entry, read the entire table, and prove a
six-byte get cannot overwrite padding or its end marker. This improves the
existing partial framebuffer row without changing the overall percentage.

Latest framebuffer-overscan delta: `GET_OVERSCAN` now returns the four retained
top, bottom, left, and right values; `SET_OVERSCAN` updates them; and
`TEST_OVERSCAN` evaluates a candidate without changing configuration. Valid
margins now create exact black output borders and bilinearly scale the complete
transformed framebuffer into the remaining display rectangle, matching FKMS
destination-margin semantics. A margin pair that consumes an entire axis is
rejected and returns the retained values without mutation; later geometry
changes bound retained margins to at least one visible pixel. The values reset
to board defaults and migrate in framebuffer VMState v6. Pi 4B/CM4 qtests prove
set/get round trips, valid and invalid Test/Set behavior, exact asymmetric
rendering before and after transpose, live migration, and reset. Exact HVS
filter coefficients and physical-display timing remain open, so the partial
row and overall percentage remain unchanged.

Latest firmware-EDID delta: the validated raw HDMI0/HDMI1 streams used by
`config.txt` filters now also back the documented `GET_EDID_BLOCK` and
display-selecting `GET_EDID_BLOCK_DISPLAY` mailbox tags. The legacy tag returns
the requested block, status, and 128 unchanged bytes; missing blocks return a
nonzero status and zero data. The display tag selects either Pi 4 port. Both
report the full 136-byte desired response while clipping writes to short caller
buffers. Property VMState v10 preserves the exact sampled blocks across live
migration, and reset atomically resamples validated connector files. Pi 4B/CM4
qtests cover two-block HDMI0, HDMI1, invalid blocks and ports, request underflow,
short responses, destination-input disagreement, migration, and reset.

Latest HDMI-DDC/SCDC delta: both production BCM2711 HDMI-I2C BSC and auto-I2C
register windows are now guest-visible. They implement the packed 32-byte
transfer, ownership-release, completion/NAK, DDC segment-pointer, and EDID
address behavior consumed by Linux `i2c-brcmstb`. HDMI0/HDMI1 read the exact
validated raw connector bytes shared with firmware filters, mailbox EDID, and
FKMS timing rather than generating another monitor identity. A connector whose
CTA HDMI Forum VSDB advertises SCDC also exposes the standard address `0x54`
sink/source version, TMDS configuration, scrambling status, read-request
configuration, and channel-lock status registers. Non-advertising connectors
NAK `0x54`; reserved bits and registers are masked. Controller VMState v2
migrates registers, buffers, EDID/SCDC pointers, and negotiated SCDC state;
reset clears controller and negotiation state while property reset resamples
connector inputs. The
HDMI-I2C compatible has been removed from the explicit final-DT exclusion
list. Pi 4B/CM4 qtests prove both ports, 384-byte segment addressing, NAK,
SCDC advertisement gating and negotiation, destination-input disagreement,
migration mid-read, and reset resampling.
HDMI0/HDMI1 now also expose the BCM2711 `HOTPLUG` connected bit at their
production core windows. Runtime connector changes pulse the correct
connected/removed line (AON inputs 4/5 and 10/11), gate DDC access, and retain
connector state across migration; reset derives presence from the newly
validated EDID. The production edge-latched AON L2 controller at `0xfef00100`
implements raw status, W1C clear, mask status/set/clear, reset, VMState, and
its GIC SPI 96 route, so its DT node is no longer forced disabled. Pi 4B/CM4
qtests cover the complete MMIO-to-AON-to-GIC chain. Physical HPD debounce,
rise/fall timing, cable voltage, signal integrity, and bus
timing/arbitration/clock stretching remain HIL work. HDMI/HVS
execution, and electrical conformance remain open, so the partial display row
and overall percentage remain unchanged.

Latest HDMI-CEC delta: the network/PCIe/remaining-peripherals slice remains
**54.5%** and overall progress remains **63.1%** because PER-008 is still
Partial.
The production CEC windows at `0xfef04300` and `0xfef09300` now implement the
VC4 control/timing/address and 16-byte TX/RX data contract. Nominally timed
transmit completion evaluates a configurable peer logical-address mask and
reports ACK or an injected NACK; bounded QOM receive injection raises HDMI0
AON lines 0/1 and HDMI1 lines 8/7. Active deadlines, registers, buffers, peer
topology, and fault selection migrate and reset cancels traffic.
Pi 4B/CM4 qtests cover successful transmit, NACK, RX, both-port routing,
migration, and reset. Open-drain voltage, multi-initiator arbitration, edge
shape, and physical timing remain HIL work.

Latest framebuffer-release delta: `FRAMEBUFFER_RELEASE` now disables scanout
instead of returning a false-success no-op. It clears the rendered surface
without discarding retained geometry or VRAM bytes; a later
`FRAMEBUFFER_ALLOCATE` re-enables the same configuration and returns its base
and size. Framebuffer VMState v6 migrates the enabled and display-control state and post-load now
resizes the destination console to the migrated geometry. Reset restores the
configured default enabled surface. Pi 4B/CM4 qtests prove exact
green-to-black release, black released-state migration, red reallocation, and
reset restoration. This closes lifecycle behavior inside the partial display
row without changing the overall percentage.

Latest framebuffer-transaction delta: property requests now apply every
supported framebuffer Set before producing any framebuffer Get response,
regardless of tag order. Geometry, offsets, depth, pixel/alpha modes, overscan,
palette, blanking, release, and allocation share one read-only classified
prepass; unrelated property tags retain normal ordered behavior. Requests that
mix framebuffer Test tags with framebuffer Get/Set tags fail before response
headers or side effects, as required by the published mailbox contract.
Pi 4B/CM4 qtests prove Get-before-Set geometry, a short Get-before-Set palette
response with the full desired length, and mixed Test/Set rejection without
depth mutation. This closes transaction ordering within the partial display
row without changing the overall percentage.

Latest framebuffer-validation delta: duplicate framebuffer tags now reject the
complete property request before response headers or side effects. Test-only
transactions evaluate geometry, virtual size, viewport offsets, supported
8/16/24/32-bit depths, pixel order, and alpha mode against one temporary
configuration: clipped results feed later Test tags but never alter live
scanout. Set tags use the same scalar validation, so unsupported depth,
pixel-order, and alpha candidates retain the prior configuration. Pi 4B/CM4
qtests prove chained normalized Test results, live-state non-mutation, invalid
scalar responses, and duplicate Set rejection. This closes the documented
transaction-validation gap inside the partial display row without changing the
overall percentage.

Latest framebuffer-display-control delta: layer and transform share the atomic
framebuffer transaction. Layer retains the complete opaque 32-bit firmware
value, while transform accepts the eight official VideoCore rotation/mirror
encodings. `SET_VSYNC` now implements the Linux firmware ABI as a synchronous
wait command with a dummy u32: its mailbox response remains pending until the
next virtual vblank derived from the selected display's active FKMS refresh
rate, falling back to 60 Hz when no mode is present. Property VMState v10
migrates an in-flight wait and its timer; reset cancels it. Undocumented
Get/Test forms return zero without creating retained configuration. Pi 4B/CM4
qtests prove exact 50 Hz completion boundaries, pending mailbox behavior,
mid-wait migration, reset cancellation, and recovery. This improves the partial
display row without changing the overall percentage.

Latest framebuffer-transform-rendering delta: the retained transform now drives
the scanout destination pitches for all eight official VideoCore combinations:
0/90/180/270-degree rotation and their mirrored variants. Transposed modes swap
the QEMU console width/height, while horizontal/vertical reflection uses signed
row/column pitches through the existing dirty-memory renderer. Reconfigure,
reset, and VMState v5 post-load all restore the correct output geometry.
Pi 4B/CM4 qtests render one six-color 2x3 source through every transform and
compare every output RGB pixel, then migrate the mirrored-transposed surface and
repeat the exact comparison. This improves the partial display row without
claiming HDMI/HVS scaling or physical timing, so the overall percentage remains
unchanged.

Latest framebuffer-cursor delta: the documented `SET_CURSOR_INFO` and
`SET_CURSOR_STATE` tags now drive a rendered programmable cursor instead of
being acknowledged as unsupported. The model accepts 16--64-pixel ARGB
surfaces in guest DMA memory, validates the complete 24-byte info and 16-byte
state requests, applies hotspots and signed positions, honors display versus
framebuffer coordinate flags, and clips composition at the output boundary.
Transparent/opaque pixels are composed through Pixman over the transformed
scanout, and a visible cursor re-reads guest RAM so content changes do not
require an emulator-only upload. Invalid dimensions, hotspots, address
overflow, enable values, or flags return the firmware's nonzero result without
changing the prior cursor. Framebuffer VMState v6 migrates the exact pointer,
geometry, position, flags, visibility, and backing RAM; reset removes custom
cursor state. Read-only QOM exposes custom-info validity and visibility.
A Pi 4B/CM4 qtest proves exact rendered color and hotspot placement, invalid
request non-mutation, movement, short-request rejection, live guest-buffer
replacement, migration, hide, and reset. The proprietary firmware's default
64x64 cursor artwork used before `SET_CURSOR_INFO` still requires an oracle,
so PER-008 and the overall percentage remain partial and unchanged.

Latest firmware-multidisplay delta: the Pi 4B and CM4 property interface now
reports two displays and maps sequential framebuffer indices to the fixed
firmware DispmanX identifiers HDMI0=2 and HDMI1=7 used by Raspberry Pi's FKMS
driver. `SET_DISPLAY_NUM` retains a validated index for following framebuffer
operations; invalid indices return the previous selection. `SET_DISPLAY_POWER`
retains independent boolean power state by firmware display ID, rejects invalid
states without mutation, and leaves unknown IDs unchanged. Read-only QOM
exposes the selected index and two-bit power mask. Property VMState v10 migrates
both and reset restores display zero with both outputs powered. Pi 4B/CM4 qtests
cover enumeration, IDs, invalid requests, selection, independent power,
migration, and reset. Two independent rendered consoles and electrical HPD/DDC
remain open, so the partial display row and overall percentage remain
unchanged.

Latest firmware-display-timing delta: `GET_DISPLAY_TIMING` and `SET_TIMING`
now implement the exact 36-byte FKMS timing contract independently for HDMI0
ID 2 and HDMI1 ID 7. Each reset derives the preferred mode from the first
valid detailed-timing descriptor in the same checksum-validated raw EDID bytes
used by firmware filters and EDID mailbox reads, including pixel clock,
horizontal/vertical sync intervals, totals, refresh, polarity, interlace,
aspect ratio, and HDMI-versus-DVI signaling. Set rejects malformed ordering,
unknown flags, bad padding, unknown displays, and clocks above 600 MHz without
changing the retained mode. Timing Set participates in framebuffer-wide
Set-before-Get ordering, so an earlier Get in the same request observes the
accepted mode. Property VMState v10 migrates both exact timing payloads; older
streams derive them from their migrated EDIDs, while reset deliberately
resamples destination connector files. A Pi 4B/CM4 qtest proves exact HDMI and
DVI payloads, unknown IDs, short-request rejection, valid and invalid Set,
Get-before-Set ordering, destination-input disagreement, migration, and reset.
Independent HDMI heads, SCDC/HPD, pixel-clock/HVS execution, and physical
timing conformance remain open, so the partial row and overall percentage stay
unchanged.

Latest firmware-power delta: the property mailbox now implements the documented
device IDs 0--10 for `GET_POWER_STATE`, `SET_POWER_STATE`, and `GET_TIMING`
instead of acknowledging writes without retaining them. Logical on/off state
echoes the device ID, reports bit 1 for nonexistent IDs, ignores the request's
wait bit in the returned state, resets to the model's available-device mask,
and migrates in property VMState v5. Timing is explicitly zero because no
firmware power-stabilization delay is modeled. Pi 4B/CM4 qtests cover both
boundaries, round trips, reset, and live migration. This is logical firmware
ABI state only; peripheral rail gating, stabilization timing, and electrical
power behavior remain physical-conformance/HIL work, so the overall percentage
is unchanged.

Latest VL805-reset delta: Pi 4B now connects the production
`NOTIFY_XHCI_RESET` property tag to its hardwired bus-1/slot-0/function-0
VL805-compatible xHCI endpoint. The documented `0x00100000` request performs
a controller cold reset while retaining PCI topology; other addresses and a
default CM4 without an onboard VL805 are no-ops. A saturating read-only reset
counter migrates in property VMState v6 and clears on machine reset. Qtest
proves a running xHCI command register is reset, invalid addresses do not reset
it, the counter migrates, and CM4 stays at zero. Opaque VL805 firmware bytes,
load timing, MSI, PHY/port electrical behavior, and hardware traces remain
open, so the overall percentage is unchanged.

Latest firmware-clock delta: property-mailbox clock state/rate operations no
longer return fixed values or acknowledge ignored writes for clocks backed by
CPRMAN. EMMC, UART, ARM, CORE/VPU, V3D, H264, ISP, PWM, EMMC2, and VEC IDs map
to their live muxes. ``GET_CLOCK_RATE`` returns the configured next-enable
rate even while gated, ``GET_CLOCK_MEASURED`` returns zero while gated, and
set-state/set-rate immediately propagate through QEMU clocks to consumers.
Unknown or intentionally unmapped IDs report nonexistent/zero rather than
false success. A Pi 4B/CM4 qtest retimes the shared PWM0/PWM1 source from
100 kHz to 200 kHz while stopped, re-enables it, migrates it, and verifies
mailbox plus both consumer views. It also caught and fixed stale exported
clock output after CPRMAN reset: every mux is now resynchronized from reset
registers. Rate limits, DVFS coupling, and physical transition timing remain
open, so the overall percentage is unchanged.

Latest firmware-temperature delta: the property mailbox no longer returns a
hard-coded 25°C sample disconnected from the BCM2711 AVS monitor. Its
``GET_TEMPERATURE`` value now comes from the same live
``thermal-temperature-millicelsius`` state used by the native AVS registers,
and Pi 4B/CM4 ``GET_MAX_TEMPERATURE`` reports the documented 85°C safety
limit. The mailbox ABI contains no validity bit, so an invalid AVS sample
retains its configured numeric value while the native status register and
``thermal-sensor-valid`` report invalid. A cross-machine qtest proves default
and runtime values, invalid status, warm reset, and live migration. Physical
calibration and temperature dynamics remain HIL work, so the overall
percentage is unchanged.

Latest logical power-fault delta: firmware ``GET_THROTTLED`` no longer always
returns zero. Both machines expose a launch- and runtime-writable
``firmware-throttled-current`` input for the Pi 4/CM4 under-voltage,
ARM-frequency-cap, and active-throttling flags, plus a read-only combined
status. Every asserted current flag permanently sets its matching bit 16--18
history flag. Current conditions and sticky history survive warm reset and
live migration, including a destination launched with conflicting defaults.
A qtest covers the mailbox and QOM views on Pi 4B and CM4. This is a
software-visible fault boundary only; voltage thresholds, rail dynamics,
thermal coupling, and electrical brownouts remain explicit HIL work, so the
overall percentage is unchanged.

Latest firmware-identity delta: the property mailbox returns the
hardware-observed legacy board-model value zero and the persistent OTP row-28
board serial through the 64-bit mailbox ABI, with the modeled 32-bit serial
zero-extended. The board revision continues to come from the same persistent
OTP identity, while the board-MAC response stays synchronized with the live
configured GENET address in both direct-loader and behavioral-boot modes. One
Pi 4B/CM4 qtest proves model, revision, serial, MAC, and the machine MAC
observation before reset, after reset, and after live migration. This closes an
internal identity-coherence gap but does not replace a pinned physical
differential trace, so the overall percentage is unchanged.

Latest peripheral-contract delta: final DT generation preserves the production
BCM2711 AON L2 interrupt-controller node without forcing it disabled now that
its edge-latched status/mask/clear registers and GIC route are modeled. The
unmodeled DVP node remains present and forced ``status = "disabled"`` after all
overlays, using the same rule as the explicitly excluded CYW43455 SDIO and
Bluetooth endpoints. The implemented HDMI-I2C nodes are also preserved without
being forced off. The versioned
``peripheral-model-policy=preserve-disabled-unmodeled-v1`` QOM contract and
exact ``peripheral-exclusions`` list distinguish an intentional machine
exclusion from a firmware tree that never described the device. A behavioral
qtest starts every endpoint enabled, proves the modeled AON and HDMI-I2C nodes
stay enabled while excluded nodes remain present but disabled, and checks the
policy/list.
Production gates
also check the machine-readable policy. Required peripherals still need their
remaining behavioral models and HIL evidence, so the overall percentage is
unchanged.

Latest host-flash safety delta: physical block-device preflight now inspects
every currently visible Linux mount namespace instead of only the caller's
namespace. It identifies and deduplicates namespaces through
``/proc/PID/ns/mnt``, parses each distinct ``mountinfo``, and rejects the
selected whole device when any namespace mounts it or a sysfs descendant.
Enumeration is bounded and fails closed on missing, unreadable, malformed, or
repeatedly changing namespace evidence while safely tolerating processes that
have already exited. Six new rootless synthetic-kernel tests cover
cross-namespace descendants, duplicate namespaces, missing identities,
malformed metadata, instability, and the enumeration bound; a live scan of the
development host also passes. Destructive media and CM4 gadget qualification
remain HIL, so IMG-012 and the overall percentage remain partial/unchanged.

Latest opened-media identity delta: every successful flash JSON report now
includes a ``target_identity`` captured from the opened and exclusively locked
file descriptor. Regular files expose canonical path, device/inode, ``rdev``,
and kind; physical block media additionally expose major/minor, resolved sysfs
path and kernel name, validated capacity/sector size, removable state, and
read-only state. The original lock remains held while a second descriptor is
opened for verification, and any path-to-device replacement fails before
hashing while retaining the recovery journal. Tests cover regular and block
evidence, stable ``/dev/disk/by-id``-style aliases, CLI/report propagation, and
replacement between write and verification. This closes the missing digital
identity evidence in IMG-003; operator review and destructive physical
qualification remain explicit.

Latest HIL flash-attestation delta: HIL plan/report version 2 now requires the
Pi 4B ``flash-media`` and CM4 ``flash-emmc`` commands to emit exactly one
successful ``rpi_image.py flash`` JSON object. The orchestrator binds its byte
count and source/target hashes to the pinned media artifact, requires verified
removable block media, validates canonical path, inode, ``rdev`` versus
major/minor, sysfs/kernel identity, capacity, sector size, and safety state,
then retains the complete object in the atomic report. The conformance gate
re-reads and independently revalidates that retained object before accepting
the physical trace. Missing/malformed output, wrong media, regular targets, and
post-run report tampering fail closed while mandatory cleanup still runs.
Fixture bundles generate this strict plan contract automatically. These tests
are software evidence only; no physical Pi 4B/CM4 attestation is claimed.

Latest authorized-target delta: fixture bundle specifications, HIL plans,
conformance cases, and retained reports now share one mandatory normalized
``flash_target`` under ``/dev/disk/by-id`` or ``/dev/disk/by-path``. Only the
platform flash step may contain the exact ``{flash-target}`` argv placeholder.
The runner expands it without a shell and rejects a successful attestation
whose original target string differs, even when the other device is removable
and carries the correct bytes. The conformance manifest independently pins the
same target and rejects plan/report drift before fixture commands execute.
Tests cover unstable raw ``/dev/sdX`` names, missing placeholders, a valid
attestation for the wrong disk, and plan-versus-case mismatch. This supplies
the operator-reviewable authorization half of the opened-device evidence;
physical identity/label review remains part of the HIL procedure.

Latest EEPROM-lifecycle delta: Pi 4B now parses ``ENABLE_SELF_UPDATE`` and
``FREEZE_VERSION`` from the unchanged EEPROM boot configuration and checks
the selected SD, USB-MSD, or NVMe FAT filesystem for the unchanged
``pieeprom.upd``/``pieeprom.sig`` pair before firmware handoff. Exact installed
images continue without writes; differing signed images persist byte-for-byte,
reboot, then continue from the same medium as up-to-date. Invalid signatures,
write protection, disabled/frozen policy, CM4 exclusion, reset, and VMState
v58 pending-reboot migration are qtested. Real GENET/TFTP discovery now requests
optional ``pieeprom.upd`` before ``config.txt`` or secure boot artifacts,
requests the required ``pieeprom.sig`` only when the update exists, and feeds
the received bytes into that same validated update engine. Packet-level qtests
prove missing-update continuation, exact request order, unchanged 512 KiB
``.bin`` persistence, reboot-before-handoff, the post-reset identical-image
no-op, subsequent ARM handoff, and fail-without-mutation behavior for invalid
signatures and write protection. Calibrated flash timing and physical wire
conformance remain open, so the affected partial rows and weighted overall
percentage do not change.

Latest production-gate delta: network manifest v2 can explicitly seal one
prefixed or root ``pieeprom.upd`` plus its sibling ``pieeprom.sig``. The real
dnsmasq/TAP gate requires a matching ``--allow-eeprom-update`` opt-in,
disables its EEPROM snapshot only for that case, runs through automatic reset
and ARM handoff, and accepts only a byte-exact final match to the sealed
512 KiB update. Missing signatures, path traversal, wrong geometry, CM4,
one-sided authorization, TFTP-tree drift, and any other EEPROM result fail
closed. The atomic report records the before/after hashes and verified
transition without labeling the intended mutation as unchanged input. An
internal TAP capture plus the QEMU event trace now also prove two ordered
``pieeprom.upd``/``pieeprom.sig`` request pairs, updater restart, reset,
up-to-date no-op, the first firmware request, and ARM handoff; matching final
bytes alone are insufficient gate evidence.

Latest negative-update-gate delta: manifest v2 now seals an explicit expected
result of ``success``, ``invalid-signature``, ``write-protected``, or
``program-failure``. Real dnsmasq/TAP integration passes all three rejection
cases using the unchanged 512 KiB update artifact. Invalid signature and
write protection each prove one update/signature request pair, the matching
observable failure, no later TFTP request, and byte-exact non-mutation.
Program failure additionally seals an exact byte cutoff and independently
requires the durable update prefix followed by erased bytes through 512 KiB.
Signature/result mismatches, hidden snapshot execution, a wrong partial
boundary, or any unexpected backend change fail closed. This strengthens
evidence for the existing partial EEPROM/network rows; physical flash timing
and electrical write-protect conformance remain HIL, so percentages do not
change.

Latest network-update-retry delta: for a program cutoff that preserves a valid
network-booting EEPROM prefix, a one-command production campaign now runs the
interruption and clean retry in separate QEMU processes against the same
persistent EEPROM file and unchanged TFTP corpus. It seals the observed
partial state into the retry manifest, requires the second phase to install
the original update hash, reset, and reach ARM handoff, and retains hash-bound
PCAPs, manifests, and reports for both phases. A live dnsmasq/TAP campaign
passes with a genuinely different post-cutoff update region, proving the first
result is partial rather than accidentally equal to the full image. Early
cutoffs that erase the bootloader remain correctly assigned to SD
``recovery.bin`` rather than being mislabeled as network-recoverable.

Latest SD-recovery production delta: a strict versioned manifest now seals the
exact initial 512 KiB EEPROM, complete raw SD image, official
``recovery.bin``/``pieeprom.upd``, firmware, fixup, kernel, board, and RAM
model. The standalone gate opens the same EEPROM and SD files persistently,
requires an exact update image, proves that the sole SD mutation is
``RECOVERY.BIN`` to ``RECOVERY.000``, observes the automatic reset, and reaches
ARM handoff from that same SD image with hash-matched boot artifacts. The live
official-artifact gate passes and retains hash-bound QMP, trace, manifest, and
pre/post media evidence.

Latest clock/serial-fidelity delta: AUX mini-UART now consumes the BCM2711
CPRMAN VPU/core clock rather than a private fixed-rate assumption. The Pi 4
reset profile supplies the documented fixed 250 MHz mini-UART clock; live
divider writes change frame timing, clock stop/resume freezes and continues
TX, and mid-frame changes preserve remaining source cycles. A shared CPRMAN
DIV-register dispatch bug is fixed, and post-migration derived clocks are
rebuilt from migrated registers instead of destination defaults. VMState v4
preserves stopped-frame cycles. Real-socket stopped-clock migration, the
unchanged mini-UART production Linux gate, and all 194 behavioral qtests pass.
Error injection, flow-control detail, and physical baud conformance remain
open, so the GPIO slice percentage does not change.

Latest PWM clock/FIFO/DMA delta: PWM0 now consumes CPRMAN's PWM clock and drains
one shared-FIFO word after each enabled channel's exact programmed range of
source cycles. Live clock-rate changes, clock stop/resume, reset, and
source-to-destination migration preserve the active period's remaining source
cycles. Empty consumption sets the channel GAPO flag without fabricating a CPU
read error, while named normal and panic DMA-threshold outputs follow DMAC
enable and FIFO level. The normal output is connected to BCM DMA peripheral
map 5; source/destination-DREQ control blocks now retain ACTIVE/HELD, exact
source/destination/length/2D-row progress, and migration state while waiting.
A live qtest fills only through the PWM threshold, migrates a stopped-clock
partial transfer with three words outstanding, then proves exact paced refill,
sample order, completion, interrupt, and reset. Dedicated tests also prove
exact 100 microsecond drain periods, thresholds, gap state, and stopped-clock
migration. PWM now emits virtual-clock output edges for the documented
distributed algorithm, mark-space mode, and MSB-first serializer, including
FIFO repeat, idle state, polarity, clock stop/resume, and VMState v3. BCM2711
GPIO VMState v8 routes PWM0_0 through GPIO12 ALT0/GPIO18 ALT5 and PWM0_1
through GPIO13 ALT0/GPIO19 ALT5/GPIO45 ALT0; GPLEV, GPEDS, named logical
outputs, and the host bridge observe the same edges. The DMA request resume
uses a guarded bottom half so a threshold transition during PWM MMIO cannot
cause re-entrant peripheral access. A live migration qtest covers every route,
all transmitter modes, exact edge deadlines, GPIO events, output enable,
stopped-clock mid-mark migration, and reset. BCM2711 now also instantiates an
independent PWM1 at ARM address `0xfe20c800` (VC `0x7e20c800`) on both Pi 4B
and CM4. It shares the CPRMAN PWM source clock but owns separate registers,
FIFO, timers, waveform outputs, reset, and VMState; its normal DMA request
drives peripheral map 1 and its two output channels route through GPIO40/41
ALT0. A live qtest proves PWM0/PWM1 isolation, both PWM1 GPIO outputs, edge
events and output enable, threshold-paced DREQ 1 refill, a held partial DMA
control block, stopped-clock migration with two exact high cycles remaining,
ordered drain/completion, GAPO, and reset. GPIO VMState v8 retains PWM1 input
levels while preserving the v7 PWM0 field.

Latest PWM shared-FIFO delta: when both channels use a controller FIFO, the
arbiter now preserves the documented A/C/E channel-0 and B/D/F channel-1
ownership. Both channels request their next words at a common boundary, so a
shorter range enters an explicit idle wait until the longer channel completes.
One-word starvation preserves the next owner instead of restarting at channel
0. VMState v4 validates and retains both wait bits plus next-owner state while
the PWM clock is stopped. A live qtest proves unequal five/ten-cycle ranges,
no premature consumption, simultaneous next-pair assignment, a migrated
short-channel wait with five exact long-channel cycles remaining, both GAPO
bits on true exhaustion, one-word starvation ordering, reset, and the same
paired state machine in PWM1. All 206 behavioral qtests and the unchanged
production firmware/overlay gate pass. PWM0 and PWM1 panic thresholds now
select each contending DMA channel's four-bit panic priority while normal DREQ
continues to gate the transfer. Stable highest-effective-priority-first
arbitration, CS.DREQ reporting, reset, and VMState v3 request-state migration
are covered with both panic and normal contention. Physical electrical/timing
conformance remains open, so the GPIO slice percentage does not change.

Latest prefix-gate delta: the production network gate now captures the private
TAP for every run and resolves each QMP logical artifact name/hash to exactly
one RRQ path in the sealed tree. Root and ``TFTP_PREFIX`` deployments therefore
use the same evidence contract; missing and duplicate same-hash candidates
fail closed. A live dnsmasq/TAP integration passes with ``TFTP_PREFIX=2``, a
MAC-prefixed update and full firmware corpus, two update checks, reset, and
handoff. The report retains every logical-to-wire mapping.

Latest network-identity delta: EEPROM ``MAC_ADDRESS`` and
``MAC_ADDRESS_OTP`` now select one effective unicast identity for GENET wire
traffic, DHCP option 97, the firmware board-MAC mailbox property,
``TFTP_PREFIX=2``, and final-DT ``local-mac-address``. Strict parsing,
line-ordered precedence, empty-value restoration, the published customer-OTP
row example, reset, and VMState v57 migration are qtested. This deepens the
already-partial PER-003 row without changing its status or the weighted
overall percentage.

Previous network-identity delta: EEPROM ``TFTP_PREFIX`` modes 0, 1, and 2 now
derive the documented lower-case serial directory, exact
``TFTP_PREFIX_STR`` (maximum 32 printable characters), or hyphenated GENET
MAC directory. The prefix is applied to every actual TFTP RRQ while received
files retain their logical names for recursive config/include/overlay
resolution. If both prefixed ``start4.elf`` and legacy ``start.elf`` are
missing, the client clears only the device prefix and retries from the TFTP
root. QOM exposes the configured mode, effective prefix, and fallback
decision; VMState v56 preserves an active prefixed network boot. Parser,
serial/MAC derivation, exact wire-RRQ, and live-migration qtests pass. This
closes production behavior inside the already-partial PER-003 row, so no
percentage is inflated.

Latest BOOT_ORDER timing delta: EEPROM ``USB_MSD_STARTUP_DELAY`` is now parsed
with its documented 0--30,000 ms range and enforced as a distinct
pre-enumeration virtual-time phase for modes 4 and 5.  It does not consume the
subsequent discovery budget.  Read-only QOM exposes the value; VMState v51
preserves an active delay and its exact remaining deadline.  Qtests prove
deadline-minus-one behavior, unchanged-media handoff, invalid-bound rejection,
separate startup/discovery accounting, and live migration.  Physical timing
calibration remains pending, so no row status or percentage is inflated.

Latest recovery delta: EEPROM ``BOOT_WATCHDOG_TIMEOUT`` and
``BOOT_WATCHDOG_PARTITION`` now arm one exact virtual deadline across every
BOOT_ORDER source, including the intentionally unbounded RPIBOOT wait.
Expiration sets the six-bit PM_RSTS boot partition and triggers the BCM power
management watchdog reset path; successful ARM handoff cancels it.  Read-only
QOM exposes the configured timeout, partition, armed state, and remaining
nanoseconds.  VMState v52 preserves an active deadline.  Qtests prove
deadline-minus-one behavior, watchdog reset cause, partition-filtered reboot,
invalid-bound rejection, ARM-handoff cancellation, and live migration.
Physical timeout/reset calibration remains pending, so row status and overall
percentage remain unchanged.

Latest SD recovery delta: Pi 4B behavioral boot now parses
``SD_OVERCURRENT_CHECK`` with the documented default-on boolean policy.
An asserted ``sd-overcurrent`` input disables modeled SD power and retries
after exactly five virtual seconds until the signal clears; disabling the
check records a non-blocking warning and continues. CM4 explicitly ignores
the Pi 4B-only signal. Read-only QOM exposes policy, source, warning, power,
retry count, and remaining nanoseconds. The signal may come from a machine
property or the same libgpiod/USB QGPIO bridge used for other board inputs via
``SET SIGNAL SD_OVERCURRENT 0|1|Z``. GPIO VMState v6 and behavioral VMState
v53 preserve the driven boundary, loop state, and exact deadline. Qtests prove
two consecutive power-off cycles, deadline-minus-one behavior, successful
unchanged-media handoff, policy bypass, invalid-value rejection, CM4
exclusion, live GPIO-bridge migration, and active boot-timer migration.
Physical voltage/current behavior and timing calibration remain HIL gates, so
row status and overall percentage remain unchanged.

Latest USB power delta: Pi 4B behavioral boot now parses
``USB_MSD_PWR_OFF_TIME`` with the documented 0--5,000 ms range and 1,000 ms
default. Exact board revision selection distinguishes PCB revisions through
1.3, which perform the short hardware cycle followed by the full configurable
off interval, from revision 1.4 and newer, which overlap the documented
minimum two seconds of reset-held power-off with memory initialization and
wait only for any configured remainder. ``USB_MSD_PWR_OFF_TIME=0`` skips the
legacy configurable cycle, while CM4 explicitly reports this Pi 4B-only
policy as not applicable. A validated ``board-revision`` override preserves
the selected 1/2/4/8 GiB memory bits and rejects cross-model or non-BCM2711
identities. QOM exposes policy, rail state, configured time, accounted time,
and remaining nanoseconds. Behavioral VMState v54 preserves an active exact
deadline. Qtests prove both board paths, zero bypass, reset re-arming, invalid
bounds, CM4 exclusion, deadline-minus-one behavior, unchanged-media handoff,
and live migration. The undocumented duration of the early board's initial
short pulse and all voltage/current behavior remain HIL gates, so no row
status or percentage is inflated.

Latest USB selection delta: behavioral boot now parses the upstream
``USB_MSD_EXCLUDE_VID_PID`` contract as an empty or comma-separated list of at
most four exact eight-digit hexadecimal ``VIDPID`` identities. Both
VL805/xHCI and BCM2711 DWC2 compare the actual USB device descriptor before
storage configuration. Matching devices are skipped while later eligible
devices retain deterministic order; matching a hub stops hub configuration
and prunes every downstream device. An excluded-only topology remains in the
discovery phase instead of being misclassified as an unbootable LUN. QOM
exposes the canonical policy, excluded and eligible device counts, and the
last matched identity. Behavioral VMState v55 preserves policy, observations,
and the exact remaining discovery deadline. Qtests cover both controllers,
strict invalid syntax and the four-entry bound, fallback past an excluded
non-storage device, hub pruning, reset re-evaluation, and active migration.
Physical USB identity/timing conformance remains pending, so row status and
overall percentage remain unchanged.

</details>

</details>

### Progress calculation

| Row status | Row score |
|---|---:|
| ✅ Full | 100% |
| ➖ Excluded | 100% when primary evidence proves the feature conflicts with the platform fidelity scope |
| 🟡 Partial | 50% |
| 🔌 Bridgeable | 25% |
| ❌ Missing | 0% |
| 🧪 HIL pending | 100% in the virtualization pass; tracked separately in the HIL pass |
| 🏁 HIL ready | 100% |

Every capability has equal weight inside its slice:

```text
slice progress = sum(row scores) / number of rows
overall progress = sum(slice progress * slice weight)
```

The deferred HIL-pass score is computed independently as
``HIL ready rows / all HIL rows``. A pending HIL row receives virtualization
credit only because its behavior is physically out of scope for this pass; it
does not become ``🏁 HIL ready`` and supplies no physical-conformance evidence.

When a row changes status, update its slice counts, recompute the slice
percentage, and then recompute the weighted contribution and overall value.
Do not award intermediate points based only on code volume or elapsed effort.

## Status definitions

| Status | Meaning |
|---|---|
| ✅ Full | Implemented and covered by automated tests at the Pass 1 software-visible boundary; any physical-only obligation is mapped to Pass 2. |
| ➖ Excluded | Explicitly outside the north-star platform contract because implementing it would introduce a non-Raspberry Pi guest ABI; the fidelity alternative is identified. |
| 🟡 Partial | Some behavior exists, but important behavior or conformance coverage is missing. |
| 🔌 Bridgeable | A host-hardware bridge is technically possible, but integration is incomplete. |
| ❌ Missing | No usable implementation exists in the Raspberry Pi machine path. |
| 🧪 HIL pending | Must remain a hardware test; its automated fixture or passing release gate is incomplete. |
| 🏁 HIL ready | The required automated hardware fixture exists and its release gate passes. |

## Completion rules

A row may move to **✅ Full** only when:

1. The implementation is connected to the `raspi4b` or future `cm4` machine,
   not merely present elsewhere in QEMU.
2. The normal production path uses it; direct kernel loading does not count as
   firmware boot coverage.
3. Positive, negative, reset, persistence, and fault cases have automated tests.
4. Software-visible results match a public specification or a retained,
   versioned behavioral fixture. When the only remaining oracle is a physical
   board, that comparison is mapped to Pass 2 and does not block Pass 1 Full.
5. Known unsupported behavior fails explicitly instead of silently succeeding.

### Pass 2 mapping for mixed rows

The following rows are Full for Pass 1 because their remaining obligations are
physical-only. They do not add new capabilities or HIL rows; the named HIL
parents retain the deferred evidence and release responsibility.

| Pass 1 row | Deferred physical obligation | Pass 2 parent gate(s) |
|---|---|---|
| IMG-004 | EEPROM/eMMC interruption and removal calibration | TEST-004, TEST-005, TEST-009 |
| IMG-012 | Destructive flashing through identified SD readers and CM4 USB gadget | TEST-004, TEST-005, TEST-009 |
| STO-001 | Card-detect timing and live physical removal | TEST-004, TEST-009 |
| STO-004 | EEPROM timing calibration, endurance, and physical conformance | STO-008, SYS-009, TEST-004, TEST-005, TEST-009 |
| STO-005 | CM4 EEPROM write-protect setup/hold and electrical behavior | GPIO-015, TEST-005, TEST-009 |
| STO-006 | EEPROM timing and physical power interruption | SYS-009, TEST-004, TEST-005, TEST-009 |
| BOOT-003 | Private fuse/JTAG encoding and electrical fuse behavior | GPIO-015, TEST-004, TEST-005, TEST-009 |
| BOOT-004 | `nRPIBOOT` sampling and USB/electrical timing | GPIO-015, USB-012, TEST-004, TEST-005, TEST-009 |
| BOOT-005 | Silicon RSA/HMAC and recovery discovery timing | TEST-004, TEST-009 |
| BOOT-006 | Silicon RSA/HMAC and recovery execution timing/status | TEST-004, TEST-009 |
| BOOT-007 | Silicon RSA/HMAC and physical second-stage traces | TEST-004, TEST-005, TEST-009 |
| BOOT-009 | Boot-source timing and hardware diagnostic comparison | USB-012, TEST-004, TEST-005, TEST-009 |
| BOOT-013 | Private VideoCore UART text/cadence and hardware result comparison | GPIO-015, TEST-004, TEST-005, TEST-009 |
| SYS-004 | Physical `nRPIBOOT` sampling setup/hold | GPIO-015, USB-012, TEST-004, TEST-005, TEST-009 |
| SYS-005 | Physical EEPROM write-protect setup/hold | GPIO-015, TEST-005, TEST-009 |
| SYS-006 | Physical EEPROM power-cut timing | SYS-009, TEST-004, TEST-005, TEST-009 |
| SYS-007 | Calibrated physical eMMC power-cycle campaigns | SYS-009, USB-012, TEST-005, TEST-009 |
| TEST-003 | Pi 4B/CM4 physical event traces | TEST-004, TEST-005, TEST-009 |
| TEST-006 | Physical plans, traces, and differential release execution | TEST-004, TEST-005, TEST-009 |

## 1. Host flashing and image validation

| ID | Capability | Status | Current implementation / evidence | Required before Full | Milestone |
|---|---|---:|---|---|---|
| IMG-001 | Raw image copy to regular-file media | ✅ Full | • `contrib/raspi4/rpi_image.py flash`; exact byte copy is tested. | Maintain regression coverage. | M0 |
| IMG-002 | SHA-256 post-write verification | ✅ Full | • Source and written byte range are hashed and compared. | Maintain regression coverage. | M0 |
| IMG-003 | Destructive-target protection | ✅ Full | • Same-file writes are rejected; block devices require explicit opt-in.<br>• After opening and locking a Linux block target, the tool validates its actual major/minor identity rather than trusting the path, rejects partitions, fixed/read-only media, mounts in every currently inspectable host namespace or active raw swap on the disk/descendants, malformed/unstable/bounded kernel metadata, and insufficient capacity before writing.<br>• The JSON result records canonical opened-FD and resolved sysfs identity; the lock remains held through a replacement-detecting verification reopen.<br>• Physical runs require an explicit durable journal outside `/dev`. | Retain the emitted identity in release evidence and require operator review. | M0 |
| IMG-004 | Deterministic interrupted flash | ✅ Full | • Exact host-copy interruption via `--fail-after`; partial bytes and the last fsynced resume checkpoint are preserved, including repeated absolute-offset interruptions.<br>• Pi 4B SD removal during a live partial controller write aborts the uncommitted sector deterministically, raises removal plus data-timeout status/IRQ, survives migration, and recovers after reinsertion without changing media bytes.<br>• CM4 EMMC2 likewise migrates a 320-byte partial sector before reset, discards it on the modeled power cut, then proves that a subsequently completed 512-byte sector remains durable across reset while the following sector is untouched.<br>• Its optional card-internal volatile cache distinguishes acknowledged, guest-readable sectors from durable backend bytes and honors MMC `FLUSH_CACHE`; optional card-program timing makes each complete uncached or reliable sector durable at a separate virtual deadline.<br>• Pi 4 EEPROM recovery can schedule each 4 KiB erase, 256-byte page, and 4 KiB verify operation on the virtual clock; reset preserves completed units, and VMState v49 migrates the exact copied `.bin`, stage, progress, configured delays, and remaining deadline. | Maintain virtualization regression. Pass 2 retains EEPROM/eMMC trace calibration and physical power/removal gates through TEST-004, TEST-005, and TEST-009. | M1/M3 |
| IMG-005 | MBR inspection | ✅ Full | • Partition type, offsets, sizes, bounds, and filesystem hints are tested. | Maintain regression coverage. | M0 |
| IMG-006 | GPT inspection and CRC checking | ✅ Full | • The image inspector tests header CRC, entry-array CRC, bounds, names, and LBAs.<br>• The behavioral media loader independently requires valid primary and backup GPT headers, matching geometry and entry arrays, both CRCs, and bounded partitions before selecting a FAT boot volume. | Add repair scenarios to the non-mutating inspector only if a separate recovery workflow requires them; the boot path must continue to fail closed. | M0 |
| IMG-007 | FAT filesystem signature detection | ✅ Full | • FAT12/16/32 signature locations are inspected. | This row covers detection only; see IMG-009 for contents. | M0 |
| IMG-008 | ext filesystem signature detection | ✅ Full | • ext superblock magic is inspected. | This row covers detection only; see IMG-009 for contents. | M0 |
| IMG-009 | Required boot-file validation | ✅ Full | • Behavioral boot traverses raw FAT12/16/32 superfloppies, primary or bounded extended/logical MBR partitions, and GPT partitions using short or VFAT long names and nested directories.<br>• It parses `config.txt`, resolves and byte-chain reads non-empty firmware/fixup/kernel/DTB, plus configured cmdline/initramfs files, and records artifact sizes and hashes.<br>• The shared FAT reader rejects repeated clusters before cyclic media can synthesize a declared file by replaying sectors.<br>• Qtests boot primary MBR, two-entry EBR, dual-header GPT, and fragmented FAT12/16/32 recovery media carrying an exact 512 KiB EEPROM update; reject FAT12/16/32 cycles, reserved/bad links, EBR cycles, out-of-container extents, malformed links, and corrupted GPT before firmware handoff or EEPROM mutation. | Maintain FAT/MBR/EBR/GPT traversal, cycle/bounds rejection, artifact-format/dependency, overlay, and pinned production-image gates; broader corpus coverage is regression expansion. | M1 |
| IMG-010 | Filesystem labels and UUID validation | ✅ Full | • The raw-image inspector derives MBR disk-signature PARTUUIDs, GPT partition GUIDs/labels, FAT volume IDs/labels, and ext superblock UUIDs/labels without mounting or changing the image.<br>• Repeatable external `--boot-config`/`--fstab` inputs or `--validate-contained-identities` fail closed unless every live `PARTUUID`, `PARTLABEL`, `UUID`, or `LABEL` reference resolves to exactly one partition.<br>• The contained path follows bounded FAT12/16/32 root-file cluster chains for `cmdline.txt` and bounded ext extents/directories for `/etc/fstab`, requires the standard one-FAT/one-ext release layout, and reports every mapping.<br>• Synthetic MBR FAT/ext and GPT fixtures cover raw extraction, case-normalized identifiers, API/CLI success, comments, unknown/ambiguous references, a FAT chain loop, and a malformed ext extent header.<br>• The unchanged pinned 2,977,955,840-byte Raspberry Pi OS image passes with its embedded `cmdline.txt` root mapping to partition 2 and `fstab` boot/root mappings to partitions 1/2; the three exact Pi 4B SD, Pi 4B USB-MSD, and CM4 eMMC functional gates enforce this before boot. | Maintain synthetic negative coverage and the pinned exact-release gates; broaden layouts only when a supported production image requires it. | M1 |
| IMG-011 | Flash resume after interruption | ✅ Full | • Every flash creates a versioned adjacent journal containing canonical source path, full size/SHA-256, exact target device/inode identity, durable absolute offset, and prefix SHA-256.<br>• Each checkpoint fsyncs target, atomically replaces/fsyncs the journal, and holds an exclusive target lock.<br>• `--resume` revalidates the full source plus source/target prefixes before writing, requires final whole-image SHA-256 verification, supports repeated interruption, and deletes/fsyncs the journal only after success.<br>• Tests reject changed sources, corrupted/replaced targets, stale/malformed journals, concurrent owners, and unverified resume;<br>• CLI recovery restores exact bytes. | Maintain fail-closed journal compatibility and regression coverage. | M1 |
| IMG-012 | Physical block-device flashing | ✅ Full | • The explicit `--allow-block-device` path rechecks the opened Linux device through `/sys/dev/block/MAJOR:MINOR`, requires writable removable whole media, rejects the target or descendants mounted in any currently inspectable host mount namespace or used as raw swap, verifies 512-byte-sector capacity against the full unchanged image, requires an explicit external resume journal, and retains exclusive locking, durable checkpoints, exact resume, whole-image verification, and opened-device identity evidence.<br>• Namespace identities are deduplicated through `/proc/PID/ns/mnt`; enumeration is bounded and missing, malformed, unreadable, or repeatedly changing evidence fails closed while exited processes are tolerated.<br>• Sixteen rootless synthetic-kernel tests cover success, partitions, fixed/read-only media, undersizing, same/cross-namespace mounted descendants, duplicate namespaces, block identity, metadata/race/bound failures, and raw swap ancestry/parsing; flash tests cover aliases and post-write replacement. | Maintain virtualization regression. Pass 2 owns destructive identified-reader and CM4-gadget validation through TEST-004, TEST-005, and TEST-009. | M1 |

## 2. CPU, memory, and core SoC

| ID | Capability | Status | Current implementation / evidence | Required before Full | Milestone |
|---|---|---:|---|---|---|
| SOC-001 | Four Cortex-A72 CPUs | ✅ Full | • Upstream `raspi4b` creates four QEMU Cortex-A72 CPUs. | Maintain four-core production ARM32/ARM64 boot, exception/MMU/SMP, reset, migration, and SKU gates; microarchitectural cache and physical differential behavior remain Pass 2. | M2 |
| SOC-002 | Selectable Pi 4B/CM4 RAM SKUs | ✅ Full | • Standard `-m 1G`, `2G`, `4G`, and `8G` selects the installed capacity for both machines.<br>• QEMU derives the corresponding `a/b/c/d` revision-memory encoding without duplicating machine types, initializes matching factory OTP identity, exposes `board-revision` and `memory-model`, and rejects unsupported sizes.<br>• An optional exact new-style `board-revision` construction value reproduces a captured PCB revision while validating its Pi 4B/CM4 model, BCM2711 processor, and selected RAM bits; this drives revision-specific USB power policy without splitting memory SKUs.<br>• One qtest covers all eight board/size combinations, probes the highest RAM word, and checks coherent OTP/revision identity.<br>• The firmware mailbox returns the legacy board-model value zero, that persistent board revision, and zero-extends OTP serial row 28 through its 64-bit serial response;<br>• Pi 4B/CM4 reset and migration coverage proves all remain coherent.<br>• ARM64 media validates the final upper-memory DT range through 8 GiB, including production one-cell size encoding where the 8 GiB high range is emitted as adjacent 2 GiB, 2 GiB, and 64 MiB banks around the BCM2711 peripheral hole.<br>• A separate ARM32 raw-media qtest boots all eight Pi 4B SD and CM4 eMMC SKU combinations, verifies board-specific DT selection, revision/OTP/model identity, the default 948 MiB low ARM region after the Pi 4 firmware's 76 MiB VideoCore reservation, exact 1/3/7 GiB upper range, `0x8000`/entry-0 handoff, and four released cores.<br>• The hash-pinned multi-memory production evidence completes unchanged RPIBOOT, continued guest ACM+MSD, Imager write/verify/flush, and post-flash handoff on all four CM4 sizes with the same artifact set.<br>• A persistent OTP image with a different SKU remains fail-closed. | Maintain all SKU gates and pinned identity tables; pin a physical mailbox trace for the legacy board-model response. | M2 |
| SOC-003 | GICv2 interrupt controller | ✅ Full | • GIC and major peripheral IRQ routes exist.<br>• The production BCM2711 AON edge-latched L2 controller implements 32 raw inputs, clear/mask registers, reset/migration state, and GIC SPI 96;<br>• HDMI0/1 connected/removed events use the production child lines 4/5 and 10/11. | Maintain GIC routing plus AON raw/mask/clear, HDMI event, reset, migration, PCIe MSI, and production Linux interrupt gates; additional wake/suspend sources are regression expansion and physical behavior remains Pass 2. | M2 |
| SOC-004 | ARM generic timers | ✅ Full | • CPU timers are connected to the GIC. | Maintain architectural timer routing and production ARM32/ARM64 kernel timing/reset gates; drift, suspend cadence, and frequency conformance remain Pass 2. | M2 |
| SOC-005 | DMA controller | ✅ Full | • The BCM2835 DMA model and BCM2711 interrupt routing exist.<br>• Channels parse the five-bit TI peripheral map and pause source- or destination-DREQ control blocks without losing the current source, destination, remaining length, 2D row/stride, next-control-block, ACTIVE, or ISHELD state.<br>• A rising request resumes all matching active channels, global channel re-enable resumes eligible work, reset lowers every IRQ, and VMState v3 validates and preserves a held partial control block plus all 32 normal and panic request levels.<br>• Normal DREQ gates transfer and is reported through CS.DREQ; a selected panic line switches arbitration from the four-bit normal priority to the four-bit panic priority.<br>• Simultaneously ready channels run in stable highest-effective-priority order with lower channel number breaking ties.<br>• PWM0 map 5, PWM1 map 1, and SPI TX/RX maps 6/7 qtests prove threshold-paced writes, source/destination pacing, normal/panic contention order, migration while held and panicking, exact resume/completion, interrupt, and reset.<br>• Legacy unpaced copy/interrupt qtests remain green. | Maintain normal and 2D copy, PWM0/PWM1/SPI DREQ pacing, hold/resume, priority/panic arbitration, IRQ/reset, and migration gates. Additional channel/debug/burst variants are regression expansion; hardware conformance remains Pass 2. | M2 |
| SOC-006 | CPRMAN clock/reset controller | ✅ Full | • The existing PLL/channel/mux tree now handles each adjacent CTL/DIV register pair correctly; divider writes no longer miss their mux by three register words.<br>• BCM2711 supplies a register-consistent fixed 250 MHz VPU/core reset profile for the mini-UART.<br>• Runtime VPU divider, disable, re-enable, and reset changes propagate through the clock framework.<br>• Firmware mailbox state, configured-rate, and measured-rate operations now control ten mapped CPRMAN muxes; stopped clocks retain their next-enable rate but measure zero, unsupported IDs fail explicitly, and reset resynchronizes all exported clock levels from restored registers.<br>• Post-load recomputes PLL, channel, DSI, TD0/TD1, and remaining derived mux outputs from migrated registers after child clocks load, preventing destination reset defaults from overriding stopped or reconfigured source clocks.<br>• Qtests prove rate changes, gating, reset restoration, active consumer timing, and stopped-clock migration. | Maintain CPRMAN register-pair mapping, firmware mailbox control, VPU/PWM/mini-UART consumer propagation, gating, reset, and migration gates. Additional DVFS/kill/busy variants are regression expansion; physical frequencies and transitions remain Pass 2. | M3 |
| SOC-007 | PMIC and voltage rails | 🧪 HIL pending | • No electrical PMIC model.<br>• The firmware mailbox has a bounded logical Pi 4/CM4 under-voltage, Arm-frequency-cap, and active-throttling input with sticky history, reset, and migration behavior; it does not infer voltage or temperature. | Keep logical status/fault injection in QEMU; test rail thresholds, dynamics, thermal coupling, and brownouts on hardware. | HIL |
| SOC-008 | Cycle-accurate BCM2711 timing | 🧪 HIL pending | • TCG is functional, not cycle accurate. | Hardware timing remains the release oracle. | HIL |
| SOC-009 | BCM2711 physical RAM aliases and DMA view | ✅ Full | • Guest RAM and the final ARM DT expose the selected lower/upper capacity, including the 8 GiB SKU's 7 GiB upper region.<br>• ARM64 and ARM32 raw-media gates independently validate the firmware-visible ranges;<br>• ARM32 covers both Pi 4B SD and CM4 eMMC across every SKU. | Maintain lower/upper RAM, VideoCore reservation, DMA/peripheral/PCIe aperture, ARM32/ARM64, all-SKU, reset, and migration gates; coherency and physical boundary comparison remain Pass 2. | M2/HIL |

## 3. Storage controllers and persistent state

| ID | Capability | Status | Current implementation / evidence | Required before Full | Milestone |
|---|---|---:|---|---|---|
| STO-001 | SD/MMC block attachment | ✅ Full | • `raspi4b` attaches one QEMU SD card to the SoC SD bus.<br>• The slot may start empty, is explicitly removable, and delivers QMP insertion/ejection through the same block-backend notifier path used by the card model and behavioral ROM.<br>• Removal during a live 320-byte partial multi-block write atomically aborts the controller transaction before a sector is committed; qtest migrates the live FIFO state, ejects it, proves unchanged media, reinserts the same backend, reads it successfully, and resets cleanly. | Maintain virtualization regression. Pass 2 owns card-detect timing and physical removal through TEST-004 and TEST-009. | M2 |
| STO-002 | SDHCI controller | ✅ Full | • Generic SDHCI is connected.<br>• Its SDHCI-v3 transfer-mode contract preserves Auto CMD23, issues CMD23 with Argument 2 before multi-block MMC I/O, and lets a real eMMC device terminate the transfer at the advertised block count.<br>• Deterministic controller-side timeout and data-CRC injection operates at aligned card-transfer boundaries, raises the architectural SDHCI error status/IRQ, stops active PIO/SDMA/ADMA, has a bounded count, and migrates its progress.<br>• Active SD removal uses the same abort primitive: transfer/data-ready state is cleared, transfer-complete/DMA/buffer-ready status is suppressed, and card-removal plus data-timeout status drives the shared EMMC/EMMC2 interrupt route.<br>• Linux gates observe `-110` and `-84`, retry, enumerate the same eMMC, and reach userspace. | Maintain the SDHCI register/DMA, Auto CMD23, error, removal, migration, and Linux-recovery corpus; finer physical timing and hardware conformance remain STO-008 and TEST-004/005/009. | M2 |
| STO-003 | Legacy SDHOST controller | ✅ Full | • The BCM2835 SDHOST model is connected at the BCM2711 legacy address with the board-owned SD bus.<br>• Power, clock-divider, timeout, command, response, status, FIFO, and IRQ state implement the digital register contract.<br>• Command completion without a selected card raises command timeout;<br>• FIFO underflow/overflow raises FIFO error; both are write-one-to-clear.<br>• Programmed registers and errors migrate, while system reset clears registers, responses, status, FIFO contents, and IRQ.<br>• A focused qtest proves masks, both error paths, clearing, live migration, and reset. | Maintain the register/error/reset/migration and board-link contract; GPIO electrical alternate-function behavior and calibrated physical timing remain GPIO-015 and TEST-004/005/009. | M2 |
| STO-004 | SPI boot EEPROM backing file | ✅ Full | • Behavioral mode binds the exact 512 KiB block-visible BCM2711 SPI EEPROM geometry and fails closed for smaller, larger, or BCM2712-style 2 MiB A/B backends.<br>• The production host gate additionally requires the underlying regular file to be exactly 524,288 bytes rather than relying on the block layer's 512-byte capacity rounding.<br>• It parses the persistent image on reset, erases in 4 KiB sectors, and performs read-before-write 256-byte page programming where stored bytes become `old & requested`, so programming can only clear NOR bits.<br>• Recovery rejects updates that are not exactly the same 512 KiB geometry.<br>• It reports exact durable erase/program counts, page count, violated 0-to-1 bit count, 4 KiB sectors touched, and configured virtual latency consumed.<br>• Optional per-erase-sector, per-program-page, and per-verify-sector microsecond delays run a real timer-driven transaction; zero retains synchronous compatibility.<br>• A deterministic stuck-at-zero byte/mask fault survives erase; the first affected page is durably AND-programmed, reports `recovery-nor-violation`, and a fault-cleared reset reflashes successfully.<br>• Qtests cover the bit/page boundary, exact-size acceptance plus one-sector-short and 2 MiB rejection.<br>• A functional gate flashes unchanged official `pieeprom-2026-05-17.bin` over an older official image using official `recovery.bin`, verifies the persistent result byte for byte, automatically resets, and boots unchanged firmware/kernel artifacts from the same SD through the updated EEPROM. | Maintain the tested logical NOR boundary. Pass 2 owns calibrated timing, physical endurance, and hardware conformance through STO-008, SYS-009, TEST-004, TEST-005, and TEST-009. | M3 |
| STO-005 | EEPROM write protection | ✅ Full | • The model separates persistent EEPROM status-register block protection from the active-low physical `EEPROM_nWP` level, matching the documented two-part contract instead of treating the pin alone as array protection.<br>• `eeprom-write-protect` remains a compatibility alias for `eeprom-status-write-protect`; protected status blocks erase/program regardless of pin level, while low `eeprom-nwp` locks attempts to change that status.<br>• The unchanged recovery `config.txt` consumes `eeprom_write_protect=-1/0/1`: `-1` leaves status unchanged, `0` clears it before flashing when nWP is high, and `1` sets it only after a fully verified update.<br>• Invalid/duplicate values and pin-locked changes fail before EEPROM mutation.<br>• Optional `eeprom-status-drive` binds an exact 512-byte release-owned status sector, loads it authoritatively over conflicting launch defaults, and durably flushes permitted transitions.<br>• The QGPIO host/USB bridge carries a dedicated input-only `EEPROM_NWP` board signal without fabricating a guest GPIO number; it overrides the fallback property while driven and migrates in GPIO VMState v4.<br>• QOM exposes live/sample/source observations, and behavioral VMState v45 migrates the transaction state.<br>• Qtests prove pin-low alone remains writable, both locked transitions fail, clearing allows an update, setting protects the resulting image, migration against opposite destination defaults, shutdown/relaunch persistence, next-recovery blocking, durable runtime clearing, and real-socket low/high recovery decisions. | Maintain virtualization regression. Pass 2 owns CM4 pin/TP5 setup/hold and electrical conformance through GPIO-015, TEST-005, and TEST-009. | M3 |
| STO-006 | EEPROM interrupted erase/program | ✅ Full | • `eeprom-fail-stage=erase/program/verify/verify-mismatch/rename/reboot` selects a deterministic recovery boundary.<br>• `eeprom-fail-after` stops erase, page programming, or verification at an exact byte count;<br>• `verify-mismatch` corrupts one modeled readback bit at that offset without changing persistent bytes and produces a distinct verification failure.<br>• Read-only QOM telemetry exposes the active/terminal flash stage, durable erased/programmed byte counts, completed page count, NOR 0-to-1 violations, successfully matched verification bytes, touched-sector count, and elapsed configured latency.<br>• Nonzero timing properties execute durable units asynchronously; reset cancels the current transaction without rolling back completed units, and an unchanged recovery file can retry.<br>• Rename and reboot faults preserve the correctly ordered persistent boundary.<br>• Real blkdebug write failure preserves the old image when the first erase fails; read-before-program failure preserves the fully erased array before any page program.<br>• VMState v49 migrates the copied exact update, stage, counters, timing configuration, and remaining timer; an incoming destination still defers speculative recovery until shared block nodes activate.<br>• Qtests prove pre-deadline non-mutation, exact first-sector durability on reset, active timing mutation rejection, mid-erase migration, exact program interruption, full timed retry, EEPROM bytes, progress counters, and FAT recovery filenames, plus NOR-cell, mismatch, backend I/O, and post-rename recovery cases. | Maintain virtualization regression. Pass 2 owns timing calibration and physical power validation through SYS-009, TEST-004, TEST-005, and TEST-009. | M3 |
| STO-007 | CM4 eMMC storage | ✅ Full | • `raspi-cm4` supports official 1/2/4/8 GiB CM4 RAM revision encodings independently of eMMC capacity and accepts a named persistent `emmc-drive` backend attached to BCM2711 EMMC2 as QEMU's actual `emmc` device rather than an SD-card surrogate.<br>• Optional `emmc-boot-drive` and `emmc-rpmb-drive` backends persist boot0/boot1 and RPMB separately; validated backend lengths derive EXT_CSD sizes while the exact Imager user-area bytes remain at offset zero.<br>• All three eMMC areas use fixed-media block operations, so QMP rejects eject/change-medium instead of allowing impossible hot removal; qtest covers the user area.<br>• `emmc-cid` replays a captured 15-byte payload, raw CID, or Linux sysfs CID;<br>• QEMU validates or reconstructs CRC7 instead of inventing one fixed vendor for every production lot.<br>• `emmc-data-error=timeout|crc`, `emmc-data-error-after=N`, and `emmc-data-error-count=N` expose repeatable controller-visible failures without modifying the image;<br>• Linux proves both error classes and retry recovery.<br>• `emmc-cache-size=N` advertises a bounded EXT_CSD cache and provides read-after-write visibility for cached sectors;<br>• CMD6 `CACHE_CTRL`, `FLUSH_CACHE`, capacity-pressure writeback, backend flush, live migration, and optional reset loss are modeled at the card boundary.<br>• `emmc-cache-flush-sector-delay-us=N` optionally makes explicit FLUSH_CACHE commit one sector at each virtual deadline; the programming state, dirty entries, completed count, deadline, and timer migrate, while reset cancels the transaction and applies the configured cache-loss policy.<br>• `emmc-program-sector-delay-us=N` queues complete uncached and reliable sectors and durably programs one at each virtual deadline; pending data, progress, deadline, and timer migrate, reset discards only the unfinished suffix, and backend failure retains an exactly retryable queue.<br>• CMD35/CMD36/CMD38 advertises deterministic `0xff` erased content and 512 KiB high-capacity groups, bypasses and invalidates overlapping cache entries, and optionally completes one durable group per `emmc-erase-group-delay-us`; range, partition, timer, and progress migrate, while reset and backend-error retry preserve exact completed/pending boundaries.<br>• EXT_CSD advertises enhanced reliable writes;<br>• SDHCI Auto CMD23 forwards bit 31, and the eMMC card preserves it across the complete CMD25 transfer.<br>• Each completed reliable sector bypasses the volatile cache and flushes its selected backend before completion, while unrelated dirty cache entries remain volatile; active reliable state migrates.<br>• The host-visible BOT target supports 1-to-131,072-byte durable WRITE(16) cutoffs and flushes exactly the selected prefix before reset recovery.<br>• At the guest controller boundary, migration-safe power-cut qtests prove that 320 staged PIO bytes do not mutate the image, that an exactly completed uncached 512-byte sector survives reset, and that acknowledged cached sectors are lost or retained precisely according to timed flush/pressure/reliable-write boundaries.<br>• The endpoint exposes MMC CID/CSD, EXT_CSD capacity/cache/reliable-write capabilities, high-speed mode, partition switching/RPMB framework, reset, and migration state.<br>• Qtests assert the `link<emmc>` topology, configured CID/fault/cache controls, derived sizes, exact user-image non-mutation, reset persistence, and source-to-destination migration with all three backends, dirty/timed cache contents, timed direct/reliable program queues, timed erase groups, and a partially submitted reliable transfer.<br>• The migration gate found and fixed a formerly duplicated RPMB/EXT_CSD wire-subsection identity.<br>• The production Linux gate enumerates 256 KiB boot0/boot1 devices and RPMB and reads the configured CID/product name from sysfs.<br>• Behavioral ROM labels BOOT_ORDER mode 1 as eMMC, selects the CM4 DTB/config scope, and deliberately ignores `recovery.bin` on eMMC.<br>• The automated supervisor performs the fail-closed RPIBOOT-to-gadget-to-QEMU ownership handoff on the same backend.<br>• The unchanged 2026-06-18 Raspberry Pi OS Lite arm64 release identifies a 4.00 GiB high-speed MMC, has no stale data-ready or block-I/O errors, mounts ext4 read/write, rewrites the first-boot PARTUUID, expands partition 2 and ext4, and reaches its serial login prompt. | Maintain exact user-byte, CID/hidden-area, cache/program/erase/reliable-write, reset/migration, Linux, and production flashing gates. Physical SKU/default and timing comparison remain Pass 2; authenticated RPMB and secure trim/sanitize are optional security extensions outside this production flash/boot contract and are not claimed. | M6 |
| STO-008 | SD electrical faults and wear | 🧪 HIL pending | • Raw images model bytes, not electrical media behavior. | Use programmable/fault-injection hardware fixtures. | HIL |

## 4. Raspberry Pi boot chain

| ID | Capability | Status | Current implementation / evidence | Required before Full | Milestone |
|---|---|---:|---|---|---|
| BOOT-001 | Direct ARM kernel loading | ✅ Full | • Existing QEMU loader boots a supplied kernel, DTB, and initrd; upstream functional tests pass. | Retain as regression/debug mode, not the full-boot default. | Baseline |
| BOOT-002 | BCM2711 mask-ROM behavior | ✅ Full | • Opt-in clean-room reset state machine owns behavioral boot, holds ARM cores, validates OTP identity/security policy, samples `nRPIBOOT`, checks SD recovery before EEPROM, and emits trace events.<br>• The in-process RPIBOOT ROM consumes the unchanged `bootcode4.bin` stream and fails closed unless its exact SHA-256 matches `rpiboot-bootcode-trusted-sha256`; a nonzero ROM status blocks second-stage re-enumeration, read-only telemetry distinguishes missing/mismatch/trusted results, and qtests cover success, mismatch, reset retry, and active migration.<br>• The production supervisor derives the oracle from the same pinned manifest artifact rather than converting or replacing the `.bin`. | Maintain the exact bootcode oracle and public USB acceptance boundary. Pass 2 owns non-public mask-ROM key/signature execution, private diagnostics, calibrated timing, and physical silicon conformance through GPIO-015, USB-012, TEST-004, TEST-005, and TEST-009. | M3 |
| BOOT-003 | OTP boot configuration | ✅ Full | • Behavioral ROM reads a persistent 66-row OTP backend, validates rows 17/18 and board row 30, recognizes the secure-boot bit, reconstructs the rows 47-54 customer-key hash byte-for-byte, and requires it to match the EEPROM `pubkey.bin`.<br>• Recovery and USB RPIBOOT consume unchanged `config.txt` `program_pubkey=1` plus generated signed `pieeprom.bin`, verify `bootconf.sig` before mutation, and reproduce the public recovery UART order: boot-mode row, boot-mode copy, production secure/revocation flags `0x81`, then eight customer-key hash rows.<br>• Matching current recovery, `program_pubkey=1` forces development-key revocation even when stale input explicitly requests `revoke_devkey=0`.<br>• Existing-key mismatch, malformed config, missing persistence, unsupported JTAG lock, signature, size, and write failures stop fail-closed.<br>• `otp-provision-fail-after` interrupts at every one of the eleven persistent row boundaries; telemetry and VMState v41 retain the exact completed-row count.<br>• Tests prove every byte-level OTP prefix, migrate a partial transaction, exercise reset outcomes from no-op retry through copy mismatch/key missing/hash mismatch, relaunch from clean OTP/EEPROM bytes, and prove key immutability.<br>• The same persistent identity now supplies raw serial filters, customer-OTP expressions, mailbox serial, system/root DT identity, network prefix/MAC selection, row-33 extended board revision, and the construction-only minimum-boot-version ABI, with reset, restart, and migration coverage. | Maintain the documented software-visible OTP boundary. Pass 2 owns private JTAG/nRPIBOOT fuse encoding, electrical fuse behavior, and physical conformance through GPIO-015, TEST-004, TEST-005, and TEST-009. | M3/M7 |
| BOOT-004 | `nRPIBOOT` sampling | ✅ Full | • The active-low input enters RPIBOOT on Pi 4B only when a supported OTP GPIO selector is configured.<br>• CM4 samples an explicitly driven GPIO40/EMMC-DISABLE line from the same host/HIL GPIO bridge at every behavioral reset; low selects RPIBOOT, high deasserts it, and high impedance falls back to the compatible `nrpiboot` property.<br>• Read-only QOM observations expose the latched value/source, trace value 40 identifies the physical path, and direct plus real-socket qtests cover reset persistence and all three levels.<br>• The sampled decision is connected to QEMU-owned DWC2 ROM/file-server protocol state and, through the Linux Raw Gadget packet proxy, the physical host's unchanged `rpiboot` process. | Maintain the documented behavioral boundary. Pass 2 owns private-encoding discovery plus power-sampling and USB conformance through GPIO-015, USB-012, TEST-004, TEST-005, and TEST-009. | M3/M6 |
| BOOT-005 | SD `recovery.bin` discovery | ✅ Full | • Before EEPROM, Pi 4B behavioral ROM opens a superfloppy or bounded MBR/GPT FAT12/16/32 volume and loads root `recovery.bin` from raw SD bytes.<br>• GPT requires consistent CRC-valid primary and backup metadata.<br>• The exact unchanged file must fit the upstream 110 KiB limit, carry the public BCM2711 payload-length/key-index/RSA-2048/HMAC-SHA1 envelope, contain nonempty RSA/HMAC fields, and match an independently pinned `recovery-trusted-sha256`; absent trust, digest mismatch, bad length/key index, empty signature fields, truncation, and oversize input fail before EEPROM mutation.<br>• Read-only QOM exposes the observed digest and key index across migration.<br>• Qtests complete recovery with fragmented signed recovery, update, and signature files on FAT12 and FAT32, plus the existing noncontiguous FAT16 512 KiB update with junk clusters interleaved.<br>• Self-loops are signed for the bytes an unchecked reader would synthesize, while reserved/bad FAT12 and FAT32 links are injected explicitly; every malformed case is rejected before programming and preserves EEPROM byte for byte.<br>• Synthetic fault qtests and a pinned unchanged official `recovery.bin` functional gate pass.<br>• CM4 explicitly does not execute a recovery file placed on eMMC, covered synthetically and with the official file. | Maintain virtualization regression. Pass 2 owns silicon RSA/HMAC and recovery discovery/status/timing through TEST-004 and TEST-009. | M3 |
| BOOT-006 | `recovery.bin` execution semantics | ✅ Full | • After the signed-envelope and pinned-digest boundary passes, clean-room behavior verifies the update SHA-256, applies unchanged recovery `config.txt` EEPROM write-protect policy, programs EEPROM, renames `recovery.bin` to `RECOVERY.000` for `pieeprom.upd`, retains it and stops for `pieeprom.bin`, and reports failures.<br>• Successful `.upd` recovery schedules a 10 ms virtual reboot.<br>• The production-artifact gate proves an official trusted recovery file updates an official old EEPROM to the exact new bytes, observes the automatic reset, skips the renamed recovery file, reads the new EEPROM, and reaches Linux from unchanged files on the same SD.<br>• The versioned behavioral-boot migration section preserves recovery identity, EEPROM status/nWP policy, pending reboot action, and remaining virtual time; migration qtests prove both retry and reset occur with the trust boundary intact. | Maintain virtualization regression. Pass 2 owns physical status/timing and silicon RSA/HMAC through TEST-004 and TEST-009. | M3 |
| BOOT-007 | EEPROM second-stage loading | ✅ Full | • Behavioral mode validates the complete section chain and finds `bootconf.txt`.<br>• In secure OTP mode the exact first section must carry the public BCM2711 payload-length/key-index/RSA-2048/HMAC-SHA1 envelope and match an independently pinned `bootsys-trusted-sha256`; missing trust, missing/malformed `bootsys`, and digest mismatch fail before customer configuration or irreversible provisioning.<br>• Row-55 development-key revocation rejects old key-index-zero second stages before customer configuration, while current key-index-one production images remain eligible;<br>• QOM exposes both inputs across reset and migration.<br>• The model requires unique `bootmain`, `mcb.bin`, and at least one `memsys*.bin`; safely decodes bounded independent or linked LZ4 frames for every executable/memory/display dependency; recomputes each SHA-256; and requires each digest in the signed `bootsys` payload.<br>• Missing, duplicate, malformed, tampered, or unrooted dependencies fail closed.<br>• Read-only QOM exposes the root digest, dependency count, and ordered dependency-set digest.<br>• Security-critical `bootconf.txt`, `bootconf.sig`, and `pubkey.bin` must each be unique; customer-key OTP binding and strict RSA PKCS#1 v1.5 SHA-256 configuration signing follow.<br>• The CM4 supervisor independently validates the same chain, requires `bootsys_sha256`, `bootsys_key_index`, and `dependencies_sha256` in its release manifest, and retains the unchanged EEPROM `.bin`.<br>• The two local exact official EEPROMs validate as the same key-index-one 13-dependency release.<br>• BCM2711 development-key revocation is not presented as a general firmware-version counter.<br>• The bounded LZ4 decoder validates frame structure, independent/linked history, exact decoded size, and the xxHash32-derived descriptor checksum before accepting rooted decoded hashes.<br>• Opaque authenticated dependency bytes are retained as loading inputs; controller-specific execution semantics are owned by USB-010. | Maintain the pinned approved-release corpus and fail closed on unsupported dependency formats. Pass 2 owns silicon RSA/HMAC and physical second-stage comparison through TEST-004, TEST-005, and TEST-009. | M3/M7 |
| BOOT-008 | `BOOT_ORDER` decoding utility | ✅ Full | • Branch tool decodes least-significant nibble first; tested with `0xf41`. | Maintain tests for all defined and reserved nibbles. | M0 |
| BOOT-009 | `BOOT_ORDER` execution | ✅ Full | • QEMU executes all configured nibbles least-significant first, falls through failed sources, validates SD/eMMC, named USB-MSD, NVMe, or network-server backends, and models SD-detect/RPIBOOT waits, STOP, RESTART, and exhaustion.<br>• EEPROM boot configuration applies ordered `[all]`/`[pi4]` policy to both variants and later `[cm4]` overrides only to CM4.<br>• `[board-type=N]` derives the product type from the new-style revision and raw `[0xSERIAL]` filters consume OTP row 28.<br>• Runtime `[partition=N]` evaluates the six-bit PM_RSTS boot partition and `[gpioN=0|1]` evaluates the externally driven BCM2711 input at reset.<br>• The documented equality, mask, masked-equality, less-than, and greater-than expression grammar operates on `partition`/`boot_partition` and persistent `cust_otp0`...`cust_otp7`; unavailable BCM2711 variables fail closed.<br>• The upstream Linux `SET_REBOOT_FLAGS` mailbox ABI selects `tryboot.txt` with bit 0 for exactly one boot, migrates while pending, clears before firmware handoff, and returns to `config.txt` on the following reset;<br>• `GET_REBOOT_FLAGS` and `NOTIFY_REBOOT` share the production property path.<br>• Secure block and network sources select and verify `tryboot.img`/`tryboot.sig`, then apply the implicit `tryboot_a_b=1` rule inside the ramdisk.<br>• EEPROM `BOOTVAR0` propagates into `config.txt` `bootvar0` equality, mask, masked-equality, and range expressions.<br>• Raw primary MBR, conventional logical MBR partitions 5 and above, and CRC-validated GPT media can open a specific numbered FAT partition while retaining partition-zero/default behavior.<br>• EBR traversal is limited to 128 entries and fails closed on cycles, invalid signatures, malformed links or logical entries, unexpected extra entries, multiple extended containers, integer overflow, and any EBR or partition extent outside the declared container or media.<br>• Two-EBR qtests prove default partition 5, explicit partition 6, bootloader DT selection, exact firmware hashes, and cycle/overflow/malformed-link rejection.<br>• EEPROM `PARTITION` and `PARTITION_WALK`, the 512-byte `autoboot.txt` contract, `[all]`/`[none]`/`[tryboot]`, `boot_partition`, and `tryboot_a_b=1` drive bounded eight-entry selection without processing autoboot files during a walk.<br>• `PARTITION_WALK` defaults enabled when absent and explicit value `0` disables the walk; invalid values fail closed.<br>• Explicit PM_RSTS reboot partitions override autoboot;<br>• `config.txt` sees requested `partition` and actual `boot_partition` separately.<br>• The final firmware DT publishes big-endian `pm_rsts`, selected `partition`, and active `tryboot` values under `/chosen/bootloader`, matching the documented userspace ABI and removing stale legacy `rsts` input.<br>• Read-only QOM exposes the selected partition and effective walk policy, VMState v36 preserves all new selection state, and multi-partition qtests cover normal A, one-shot B, return to A, explicit override, malformed-autoboot walking, enabled walk defaults, explicit disable, EEPROM defaults, selected-partition config filters, DT values, and live migration.<br>• Same-type filters replace while model, identity, GPIO, and expression types combine until `[all]`;<br>• `[none]`, unknown, high-impedance, malformed, and nonmatching conditions stay inactive.<br>• Cross-platform, identity, expression, combination, one-shot tryboot, secure-tamper, BOOTVAR0, A/B, and live-reset qtests prove that one exact EEPROM can select platform-, board-, watchdog/reboot-, OTP-, partition-, and fixture-dependent policy without a matching GPIO accidentally re-enabling a nonmatching model.<br>• Mode 6 reads unchanged raw FAT media from the same backend attached to the PCIe NVMe endpoint and enters the standard firmware/config/ARM handoff; qtest proves unsigned and signed success, tamper rejection, source/PCI-device migration, plus distinct missing-NVMe, corrupt-media, and injected backend-read-error fallback to the next SD nibble while the NVMe endpoint remains enumerated.<br>• The same permanent or one-shot `blkdebug` read fault produces an actual queue-level NVMe `Unrecovered Read Error` CQE; tests prove repeated failure and exact one-command consumption followed by a successful READ.<br>• Network modes 2 and 7 perform real DHCP/static-IP, ARP, TFTP or authenticated HTTP/TLS transfer through GENET; the live official-host gate reaches handoff from unchanged EEPROM bytes.<br>• USB modes 4 and 5 probe a bounded device/LUN topology in deterministic device-then-LUN order and read unchanged artifacts from the same backends attached as removable QEMU USB-storage or BOT/SCSI peripherals.<br>• Missing and unbootable media take distinct bounded paths, backend notifications make changes visible without polling, and QOM plus migration preserve the selected candidate.<br>• Stable realized USB/SCSI identities plus architectural guest-DMA command/event/transfer-ring VL805/xHCI execution and DMA-driven DWC2 enumeration prove that this topology is present on the selected controller bus; mode 4 and mode 5 fail closed when wired to the other controller. | Maintain virtualization regression. Pass 2 owns bootloader-diagnostic, autoboot/PARTITION, NVMe/USB timing, and physical BOT/SCSI comparison through USB-012, TEST-004, TEST-005, and TEST-009. | M4/M9 |
| BOOT-010 | Boot retry, timeout, and `MAX_RESTARTS` | ✅ Full | • `SD_BOOT_MAX_RETRIES`, `NET_BOOT_MAX_RETRIES`, and `MAX_RESTARTS` parse signed `-1`/finite values; attempts, retries, and restart cycles are exposed and qtested.<br>• Network mode applies documented `DHCP_TIMEOUT`, `DHCP_REQ_TIMEOUT`, `DHCP_OPTION97`, `TFTP_IP`, `CLIENT_IP`, `SUBNET`, `GATEWAY`, and `TFTP_FILE_TIMEOUT` policy.<br>• A complete static tuple skips DHCP;<br>• DHCP subnet/router options or the static gateway select the Ethernet next hop independently from the TFTP destination.<br>• One deadline bounds the complete DHCP sequence while DISCOVER and REQUEST retransmit at the configured interval; a valid selected-server NAK restarts discovery with a fresh transaction ID inside that unchanged attempt.<br>• DHCP option 97 uses the Pi 4 prefix/board-revision/MAC/OTP GUID layout, with legacy repeated-serial and custom-prefix modes.<br>• A validated unicast `TFTP_IP` overrides only the TFTP server while retaining a leased client address unless static mode is complete.<br>• Complete attempts retry and fall through only after the configured count.<br>• Wire DHCP/static identity, route, server override, and remaining overall/packet deadlines migrate.<br>• Lost RRQs and mid-file ACKs use deterministic 500 ms, 1 s, 2 s, then 4 s-capped exponential retries without extending each file deadline.<br>• Valid TFTP ERROR terminates immediately; file-not-found drives optional/fallback policy, code 8 makes one classic option-free retry, other codes apply retry/fallthrough without consuming the file timeout, and wrong transfer IDs receive code 5.<br>• A 500 ms final-DATA dally re-ACKs duplicates and restarts its interval.<br>• VMState v20 preserves separate DHCP, proxy-TFTP, and resolved TFTP server identities, DHCP/static identity, routing/policy/retransmission state, TFTP server override/MAC/port, negotiated block size/OACK/reported-size/classic-fallback state, logical block number across 16-bit rollover, response-discovered paths/roles/bounds, optional/missing results, prefix fallback, partial/completed buffers, queue boundaries, retry backoff, final transfer identity, and remaining retransmission/dally deadlines.<br>• Finite RESTART arms the BCM watchdog after its configured threshold.<br>• `USB_MSD_PWR_OFF_TIME` distinguishes the legacy Pi 4B full configurable cycle from the newer reset-held/memory-init-overlap path, while `USB_MSD_STARTUP_DELAY` remains a distinct pre-enumeration phase; both migrate with exact remaining deadlines and USB discovery/LUN timeouts parse fail-closed.<br>• `BOOT_WATCHDOG_TIMEOUT` arms an independent exact-seconds deadline across RPIBOOT and every other source;<br>• `BOOT_WATCHDOG_PARTITION` selects the six-bit PM_RSTS reboot partition, expiry uses the BCM watchdog reset cause, ARM handoff cancels the deadline, and VMState v52 preserves its exact remaining nanoseconds.<br>• `REBOOT_ON_FATAL_ERROR` defaults on, parses only 0/1, waits for three deterministic behavioral error-pattern intervals after an unsupported mode or exhausted order, then hard-resets through the same watchdog cause; zero stays stopped.<br>• QOM and VMState v60 retain the policy, reboot count, source, and exact remaining deadline across live migration and warm reset.<br>• EEPROM `NETCONSOLE` strictly parses the documented bounded endpoint, waits for GENET link or `DHCP_TIMEOUT` without implicitly starting DHCP, resumes on link change, emits configured UDP diagnostics, and preserves its endpoint, counters, and exact deadline across reset and VMState v78 migration.<br>• Deterministic GENET drop injection and cadence comparison cover packet-loss/retry policy at the software boundary. | Maintain automated regression coverage. Physical timing, electrical reset, and private diagnostic cadence remain in Pass 2 HIL. | M4 |
| BOOT-011 | VideoCore execution boundary | ✅ Full | • QEMU has no VideoCore VI CPU model, and the project explicitly chooses a clean-room output-behavior replacement for the north-star path rather than pretending to execute the closed ISA.<br>• Read-only `videocore-execution-mode=behavioral-replacement-v1`, `videocore-boundary-version=1`, and `videocore-artifact-policy=exact-input-bytes-not-instruction-executed` make that decision machine-visible and versioned.<br>• Direct-loader mode reports the boundary inactive.<br>• Qtest and the unchanged production boot gate require this contract. | Maintain the versioned clean-room behavioral contract and production-output regressions. Any future instruction-execution claim requires a legal VideoCore VI model and a distinct machine-visible status; physical differential output comparison remains Pass 2 through TEST-004, TEST-005, and TEST-009. | Research/M5 |
| BOOT-012 | `start4.elf` behavioral consumption | ✅ Full | • Behavioral replacement v1 resolves and reads the unchanged selected `start4.elf`/fixup pair, exposes their exact sizes and SHA-256 values, consumes the same config/DT/overlay/kernel inputs as hardware, produces the modeled final DT and ARM state, and reaches Linux userspace from the official production corpus.<br>• The normal RPIBOOT path now opens the exact received 29,360,640-byte `boot.img` directly from memory as transient partitioned firmware media and reaches ARM64 handoff with its unchanged start/fixup/kernel/initramfs/DT/overlay files while preserving eMMC as the flash target; no host extraction or direct-loader argument is used.<br>• It explicitly reports that VideoCore firmware bytes are behavioral inputs and are not instruction-executed.<br>• Synthetic qtest and the production gate lock the version, policy, exact official hashes, invalid-image failure, and handoff result. | Maintain exact-artifact, raw-media, RPIBOOT, final-DT, ARM-handoff, and production-userspace regressions. Pass 2 owns physical boot-trace, register, clock, reservation, and output comparison through TEST-004, TEST-005, and TEST-009; ISA execution is explicitly outside the behavioral claim. | M5 |
| BOOT-013 | `config.txt` processing | ✅ Full | • Behavioral mode applies the 98-character line limit and treats bounded relative includes as textual insertion: the caller's filter state enters the included bytes and filter changes made there remain active after return, with depth and cycle checks.<br>• Model/board-type, EDID, raw OTP serial, explicitly driven GPIO, and expression filters replace only their own category and combine across categories until `[all]`;<br>• `[none]` remains disabled until that reset, and `[tryboot]` consumes the one-shot reboot state.<br>• BCM2711 expressions consume `bootvar0`, requested `partition`, selected `boot_partition`, and `cust_otp0`...`cust_otp7`; unavailable, malformed, unknown, and high-impedance inputs fail closed.<br>• Both HDMI inputs accept unchanged raw EDID streams, validate the base header, extension count, exact length, and every block checksum, derive the documented manufacturer/product filter name, and match either Pi 4 port.<br>• Empty, malformed, nameless, missing, and checksum-invalid EDIDs fail closed.<br>• Display identities are sampled per reset, retained across migration, and exposed through QOM.<br>• EEPROM `bootconf.txt` `[config.txt]` bytes append after the selected media configuration with inherited filters and same-source includes; exact size/hash QOM plus VMState v61 cover pending migration.<br>• The same live board, display, OTP, and GPIO inputs feed SD, eMMC, TFTP, and HTTP-derived configuration.<br>• The parser also covers paired firmware/fixup selection, `arm_64bit`, kernel/DT/cmdline selection, `os_prefix`, `overlay_prefix`, ordered base/overlay parameter scopes, `auto_initramfs`, up to eight comma-separated `initramfs`/`ramfsfile` names, and strict unsigned `device_tree_address`/exclusive `device_tree_end` placement controls.<br>• Bootloader-owned `start_x`, `start_debug`, `gpu_mem`, `gpu_mem_256`, `gpu_mem_512`, `gpu_mem_1024`, `total_mem`, `bootcode_delay`, `sdram_freq`, and `uart_2ndstage` are accepted only in the top-level file.<br>• Firmware shortcuts select the Pi-specific X pair with generic X fallback, debug pair, or cut-down pair.<br>• Installed-memory-specific GPU settings override `gpu_mem`; every supported Pi 4B/CM4 SKU selects `gpu_mem_1024`, while 256/512 MiB selectors remain inactive.<br>• The effective split defaults to 76 MiB, has a 16 MiB minimum, and drives cut-down selection, final DT memory, framebuffer VC base/size, and ARM/VC mailbox responses from one value.<br>• Complete explicit start/fixup pairs win, and direct cut-down filenames are rejected.<br>• `total_mem` clamps to 128 MiB through installed RAM and regenerates the final lower and above-1-GiB DT memory banks without changing the physical backend or board identity.<br>• `bootcode_delay` defers artifact loading by exact virtual seconds across block/network media, resamples EDID at expiry, re-arms on reset, and migrates with its remaining time.<br>• `sdram_freq` records the requested clock but faithfully retains the non-configurable BCM2711 3200 MHz effective rate across every memory SKU.<br>• `uart_2ndstage=1` selects GPIO14/GPIO15 ALT0, configures modeled PL011 UART0 for 115200 8N1, and emits a deterministic clean-room enable/source/artifact/handoff record through serial0.<br>• Read-only QOM exposes requested/effective memory values plus UART state, bytes, and lines; reset emits a fresh UART record and VMState v48 retains counters without replay.<br>• The identical selection operates on Pi 4B/CM4 block and network corpora.<br>• `arm_64bit=0` selects unchanged `kernel7l.img`;<br>• 64-bit mode selects `kernel8.img`.<br>• Every selected file is required, read unchanged, and concatenated byte-for-byte in list order into the single size/hash/DT initrd range used by hardware.<br>• Qtests prove category replacement/combination, inherited and returning include state, dual-port raw EDID validation/reset/migration, nonzero persistent serial/customer OTP, live GPIO changes across reset, one-shot tryboot selection across migration, firmware-pair precedence/include/fallback across SD and network, `total_mem` minimum/maximum/intermediate bank layouts across every Pi 4B/CM4 installed-memory class plus network and include boundaries, effective `gpu_mem*` selection plus DT/framebuffer/mailbox agreement across all eight Pi 4B/CM4 memory models and network/include/invalid boundaries, exact `bootcode_delay` timing/reset/migration plus network/include/invalid boundaries, requested-vs-effective `sdram_freq` behavior across every memory model plus network/include/invalid boundaries, exact `uart_2ndstage` SD/reset/migration output plus network/include/invalid boundaries, explicit final-DT address/end placement and rejection, exact in-memory concatenation, ordered TFTP requests, missing-file rejection, empty-name rejection, and the file-count bound in addition to long-path, prefix, fallback, overlay-scope, and 32-bit default-kernel coverage. | Maintain virtualization regression. Pass 2 owns hardware error/result and private VideoCore UART text/cadence comparison through GPIO-015, TEST-004, TEST-005, and TEST-009. | M5 |
| BOOT-014 | Device-tree overlay application | ✅ Full | • Behavioral mode reads requested unchanged `.dtbo` files from raw FAT media, applies BCM2711 names from `overlay_map.dtb`, ordered base and overlay `__overrides__`, standard libfdt fixups/fragment merging, and the Pi-specific rule that enabled fragments append `bootargs` instead of replacing it.<br>• Path, direct-phandle, and external-symbol targets are covered, including packed unaligned consecutive targets and ordered accumulation of multiple assignments in one overlay.<br>• It fails closed for missing, malformed, unsupported-parameter, or merge errors.<br>• Covered scalar forms include strings/status, 8/16/32/64-bit offsets, byte strings, regular/inverted booleans, embedded textual literals, conditional/unconditional fragments, and binary cells following an `=` descriptor.<br>• Lookup tables support exact keys, key-as-value entries, quoted values, defaults, unmatched-value pass-through, and local or external binary-cell results; missing entries without fallback, malformed continuations, and truncated cells are rejected.<br>• Binary cells are applied after libfdt fixups so both local intra-overlay and external base-tree phandles resolve before values are written to the merged tree.<br>• Offset-zero `reg` assignments update the node's hexadecimal unit address, and the `name` pseudo-property renames a node without leaking a synthetic property.<br>• Pi intra-overlay fragment dependencies are stably topologically ordered across arbitrary-depth chains even when dependents precede their targets in wire order; dependency cycles fail closed.<br>• Overlay symbols are private by default; zero-length `__exports__` entries alone survive with rewritten merged-tree paths and can be consumed by a later unchanged overlay.<br>• Non-empty exports, missing symbols, and collisions with base symbols fail closed.<br>• An unchanged read-only HAT ID EEPROM backend validates the `R-Pi` header, ordered atom set, lengths, GPIO reserved fields and CRC-16, publishes identity and custom atoms under `/hat`, and automatically loads either an embedded DTBO or a named overlay through normal mapping.<br>• HAT parameters own the initial scope; leading `dtoverlay=` suppresses only the overlay while retaining the EEPROM GPIO map, and `force_eeprom_read=0` suppresses both.<br>• Used GPIO entries program the real BCM2711 function and pull registers before ARM handoff, reapply on reset, and migrate with the GPIO device.<br>• Drive, slew, hysteresis and back-power selections are validated and exposed as policy telemetry without claiming electrical emulation.<br>• The official release maps `vc4-kms-v3d` to `vc4-kms-v3d-pi4`, preserves `8250.nr_uarts=1` across the VC4 overlay, and reaches first boot. | Maintain the unchanged production-overlay and external HAT corpus gates. Pass 2 owns physical final-DT/GPIO startup comparison plus pad-current, edge-rate, hysteresis, and back-power validation through GPIO-015, TEST-004, TEST-005, and TEST-009. | M5 |
| BOOT-015 | Firmware-generated DT | ✅ Full | • Behavioral mode merges the supported ordered overlay/parameter subset into the DTB selected from raw boot media, replaces memory nodes, preserves and prefixes firmware-owned `/chosen/bootargs` (including production `8250.nr_uarts=1`) before the media command line, creates/updates initramfs bounds, resolves `serial0`/`serial1` against the final DT, publishes big-endian bootloader reset/partition/tryboot state, applies board device policy, packs it, hashes it, and loads it at a Pi-compatible top-down address or the exact configured `device_tree_address`, bounded by exclusive `device_tree_end`.<br>• The common validator rejects end-bound and artifact/firmware-state collisions before writing guest RAM.<br>• The modeled BCM2711 AON L2 interrupt-controller and HDMI-I2C nodes retain their real production nodes without forced disable.<br>• Unmodeled DVP, CYW43455 SDIO, and Bluetooth endpoints remain present but are forced disabled after overlay application; the versioned QOM policy and exact exclusion list make this distinguishable from an absent firmware node.<br>• The parent serial UART remains available, and the unchanged kernel registers the official mini-UART as `ttyS0`.<br>• When no initramfs is selected, stale `linux,initrd-start` and `linux,initrd-end` properties are removed; selected initramfs ranges use the root address-cell width. | Maintain final-DT corpus, placement, stale-property, and production-kernel regression. Pass 2 owns physical byte/trace comparison of firmware reservations, aliases, clocks, and device state through TEST-004, TEST-005, and TEST-009. | M5 |
| BOOT-016 | Firmware-to-ARM handoff | ✅ Full | • The clean-room boundary validates raw, gzip, or EFI-zboot ARM64 Images and raw/gzip ARM32 zImages.<br>• ARM64 honors its Image header layout, loads at the selected address, enters EL2 with `x0=DTB`, and parks secondaries on the Pi spin table.<br>• ARM32 validates the zImage magic/extent, loads unchanged bytes at `0x8000`, enters through a firmware stub at `0x0` with `r0=0`, `r1=~0`, and `r2=DTB`, and parks secondaries on the BCM2711 mailbox-3 clear addresses rooted at `0xff8000cc`.<br>• Read-only QOM exposes architecture, entry/load addresses, and released-core mask.<br>• A raw-media qtest proves the 32-bit register view, exact bytes/stubs/DTB, and all four released cores.<br>• Standard Pi 4B SD and CM4 eMMC functional gates boot pinned unchanged official EEPROM, `start4.elf`, `fixup4.dat`, `kernel7l.img`, board-specific BCM2711 DTBs, and a pinned unchanged ARMv7 initramfs into Linux 6.18 ARMv7.<br>• They verify each platform and artifact identity, bring all four CPUs online, reach `Boot successful.`, publish kernel-started/userspace-ready health, and execute `uname -m` as `armv7l`.<br>• Stable trace events cover selected artifact sizes, overlays, architecture-specific ARM entry, and health.<br>• The health boundary migrates and clears on reset.<br>• ARM64 production gates likewise reach userspace. | Maintain raw/gzip/EFI-zboot, layout, endian, reset, migration, and 32-bit/64-bit production-userspace regression. Pass 2 owns calibrated physical clock, reservation, complete-register, and error-trace comparison through TEST-004, TEST-005, and TEST-009. | M5 |
| BOOT-017 | Secure boot/signature policy | ✅ Full | • The BCM2711 behavioral chain consumes upstream-compatible signed EEPROM and `boot.img` bytes, authenticates exact structurally valid `bootsys` bytes against a separately pinned release digest, verifies its LZ4 dependency hashes before configuration or OTP mutation, requires unique critical EEPROM config/signature/key sections, binds OTP customer-key hashes, and verifies strict RSA-2048 PKCS#1 v1.5 SHA-256 signatures before exposing the inner read-only FAT.<br>• SD/eMMC/USB/NVMe and real TFTP/HTTP paths share the same fail-closed boundary.<br>• Recovery/RPIBOOT provisioning verifies the root, dependencies, and customer-signed EEPROM before burning its customer-key hash and current production development-key-revocation flags; current recovery overrides stale `revoke_devkey=0`, a different or unsigned customer key cannot replace the fused key, and a revoked device rejects key-index-zero `bootsys` while accepting key-index one.<br>• Reset and migration tests retain the observed key index and OTP flags.<br>• Row-boundary power interruption produces the same persistent prefix on disk.<br>• The live official default-host gate verifies SNI/X.509, the official signature, exact outer-image and inner-artifact hashes, and reaches ARM handoff.<br>• The public BCM2711 rollback boundary is complete: ROM key indices zero through four are accepted when independently trusted, revoke_devkey rejects only development key zero, and indices above four fail closed.<br>• The behavioral root explicitly does not claim possession of the non-public silicon RSA/HMAC secrets or a general BCM2711 firmware-version counter. | Maintain the approved-release, live-service handoff, and signed userspace corpus gates. Controller-specific opaque dependency execution remains owned by USB-010. Pass 2 owns silicon RSA/HMAC, electrical fuse behavior, and physical root-of-trust conformance through GPIO-015, TEST-004, TEST-005, and TEST-009. | M7 |

**BOOT-014 production-corpus evidence:** the automated functional gate now
pins nine unchanged `.dtbo` files from firmware commit
`78e81e2cd6e00efeb79c169bff29ec457fa14b11`, applies representative real
parameters to each file independently, locks every final-DT SHA-256, and locks
the ordered combined result.  The corpus exposed and now regresses two
previous gaps: zero-length compatibility overrides are accepted as deliberate
no-ops, and multi-target override bytes are copied before in-place FDT
mutation can relocate them.  A strict external HAT corpus runner now
preflights the complete manifest before execution, pins unchanged EEPROM,
bootloader, and media hashes, and differentially locks parsed identity,
embedded/named overlay selection, GPIO policy, final DT hash, and ARM
handoff for both Pi 4B and CM4.  It records the exact file, sector-padded
backend, and EEPROM-declared logical-content identities separately.  No
authoritative physical GPIO trace is claimed in Pass 1.  The software-visible
contract is complete: stable arbitrary-depth intra-fragment ordering now
extends the earlier first-pass behavior, dependency cycles fail closed, and
the production corpus pins a deterministic seed before hashing the
post-firmware final tree.  External vendor ``.eep`` corpus expansion remains
a regression-maintenance activity; physical byte/trace and pad-electrical
comparison are owned by the mapped Pass 2 gates.

For BOOT-009, the former parallel USB block reader is now removed:
behavioral selection consumes the controller selected by the EEPROM boot
mode: PCIe VL805/xHCI for mode 4 and BCM2711 DWC2 for mode 5.  Both paths use
USB enumeration, hub routing, BOT/SCSI READ CAPACITY, and READ(10) data through
the versioned callback-backed FAT reader.  The VL805 owner programs DCBAA,
command, event, context, control-transfer, and bulk-transfer rings in guest
DMA, including cycle/link recycling and slot teardown for media reprobe.  The
remaining USB trace requirement means physical-oracle enumeration timing and
reset/error-policy comparison, not missing QEMU controller enumeration,
architectural ring execution, or media reads.

## 5. USB and host integration

| ID | Capability | Status | Current implementation / evidence | Required before Full | Milestone |
|---|---|---:|---|---|---|
| USB-001 | DWC2 USB 2 host controller | ✅ Full | • The controller is wired at the Pi MMIO address and exposes `usb-bus.0`.<br>• A qtest resets the root port, assigns the live root hub, reads its descriptor, discovers and resets both occupied downstream ports, and assigns both devices entirely through DWC2 host-channel DMA.<br>• A bounded firmware-owner API executes that same host-channel, DMA, hub-routing, USB-packet, endpoint, and asynchronous completion engine before ARM release.<br>• With `GAHBCFG.DMAEn` clear, host slave/PIO mode stages OUT/SETUP dwords per channel, waits for a complete packet without advancing `HCDMA`, reports shared non-periodic and periodic free space, and delivers IN `GRXSTSP` host-channel status plus `FIFO(0)` payload.<br>• A live `usb-kbd` GET_DESCRIPTOR qtest migrates a partial SETUP FIFO and later the queued exact descriptor, proves receive-depth backpressure/wakeup, and covers bounded overflow, selected periodic/non-periodic flush, refill, and recovery.<br>• Asynchronous PIO IN packets reserve rounded payload bytes and both receive-status slots before USB submission so concurrent channels cannot overcommit configured capacity.<br>• A delayed USB-storage qtest holds one channel asynchronously in flight, backpressures a second channel, alternates host/core reset across sixteen cancellation cycles, and proves class-reset plus GETMAXLUN recovery after every cycle.<br>• FIFO-wait routing metadata now survives service deferral/migration and disappeared devices halt safely instead of reaching `usb_ep_get(NULL)`.<br>• Four consecutive invalid-CBW faults and the complete thirteen-case USB-IF BOT matrix prove repeated Mass Storage Reset, exact non-phase CSW residue, dual-endpoint clear-halt, and full-command retry.<br>• GRSTCTL Tx/Rx/token-queue flush, frame-counter reset, HCLK soft reset, and core soft reset are self-clearing commands: reset cancels asynchronous host packets, clears channel/endpoint state machines and interrupt summaries at the proper scope, preserves port attachment plus programmable configuration, restarts the frame epoch, and migrates in the resulting stable state. | Maintain the DWC2 DMA/PIO, FIFO, enumeration, reset, cancellation, migration, and BOT-recovery corpus; extended storm testing and physical FIFO/enumeration conformance remain Pass 2. | M2 |
| USB-002 | QEMU virtual USB peripherals | ✅ Full | • Keyboard and storage devices can attach to the BCM2711 DWC2 and PCIe VL805/xHCI buses.<br>• Configured boot devices, BOT devices, and SCSI LUNs have deterministic QEMU IDs and per-device USB serials.<br>• Controller-driven enumeration verifies direct root ports, automatic hubs, a two-device/three-LUN topology, each SCSI-over-BOT interface and maximum-LUN value, successful INQUIRY and READ CAPACITY traffic, and exact READ(10) backend comparisons.<br>• A permanent block read fault returns a failed CSW and drives bounded boot fallback.<br>• A malformed CBW puts the QEMU USB-MSD target into reset-required state; class reset cancels active and deferred SCSI state and clears BOT state, while endpoint CLEAR_FEATURE releases the selected bulk halt.<br>• Both firmware owners execute the complete thirteen-case direction/length matrix with exact residue checks, recover only the six phase-error relations, retry unchanged traffic, and leave the whole USB backend byte-identical. | Maintain the keyboard, storage, hub, multi-device/LUN, BOT-error, and backend-integrity corpus; additional device breadth and extended reset stress are regression expansion, with physical behavior owned by USB-012. | M4 |
| USB-003 | Physical host USB passthrough into emulated Pi | 🧪 HIL pending | • `usb-host` and libusb 1.0.30 are available and runtime-probed.<br>• `usb-boot-external=on` now scans separately attached devices on stable `vl805.0` or `usb-bus.0` buses through the same pre-ARM descriptor/BOT reader used by machine-owned media.<br>• A qtest proves unchanged external `usb-storage` images boot through both mode-4 xHCI and mode-5 DWC2 paths; the documented `usb-host,hostbus=N,hostport=P` form connects a physical device without a block-byte shortcut. | Pass 2 must run a dedicated physical USB-MSD fixture with reviewed permissions, stable port mapping, reset/disconnect policy, and automated attach/detach traces. | M2 |
| USB-004 | USB-MSD boot | ✅ Full | • Legacy `usb-boot-drive` binds one named raw backend as device 0/LUN 0.<br>• `usb-boot-drives=dev0+lun1:dev1` describes an ordered topology where `:` separates USB devices and `+` separates consecutive LUNs on one device;<br>• QEMU rejects empty, missing, and duplicate backends and bounds the model to eight devices and eight total LUNs.<br>• `usb-boot-controller=auto` attaches those devices to the Pi 4B VL805/xHCI bus or CM4 BCM2711 DWC2 bus; explicit `xhci` and `dwc2` model alternate carrier/connector wiring.<br>• BOOT_ORDER mode 4 accepts only VL805/xHCI media and mode 5 accepts only BCM-USB-MSD/DWC2 media; mismatch tests prove the modes cannot alias.<br>• Single-LUN groups become removable `usb-storage` devices, while multi-LUN groups become one USB BOT device with ordered `scsi-hd` LUNs.<br>• Each USB device uses stable ID `raspi4-usb-boot-N` and serial `QEMU-RPI-BOOT-DN`; multi-LUN disks use `raspi4-usb-boot-N-lun-L`.<br>• Before ARM release, the behavioral firmware resets and enumerates the selected controller's root ports and hubs, validates maximum LUN and READ CAPACITY, and supplies all FAT/MBR/GPT plus artifact bytes exclusively from BOT READ(10) results; there is no parallel USB block-byte reader.<br>• The VL805 owner initializes DCBAA, command/event rings, input/output contexts, Address Device and Configure Endpoint commands, EP0 Setup/Data/Status TDs, and bulk Normal TRBs in guest DMA.<br>• Cycle/link recycling, controller-reset reinitialization, slot disable/reprobe, automatic hubs, hot insertion/ejection, and multi-device/LUN migration are qtested.<br>• `usb-boot-bot-stall-once=on` injects one invalid CBW and `usb-boot-bot-stall-count=N` injects up to eight consecutive faults; both firmware owners issue BOT class reset and clear both bulk endpoint halts before each whole-command retry, while xHCI additionally executes Reset Endpoint and Set TR Dequeue against freshly reset guest-DMA rings.<br>• `usb-boot-bot-phase-count=N` cycles through all six reset-required USB-IF relations (cases 2, 3, 7, 8, 10, and 13) under the same combined eight-recovery bound; a transport STALL or phase-error CSW both force ordered Reset Recovery.<br>• `usb-boot-bot-case-count=13` executes the complete USB-IF matrix with exact non-phase residue checks and Reset Recovery for exactly the six phase-error relations; the entire backend remains byte-identical.<br>• Qtests prove four consecutive malformed-CBW recoveries and all thirteen matrix cases through each owner and migrate xHCI recovery, phase, matrix, and consumption state.<br>• Read-only QOM publishes `vl805-xhci-host-bot-scsi-read10-v1` or `dwc2-host-bot-scsi-read10-v1` plus migratable command, byte, failed-CSW, BOT-recovery, phase-error, and matrix-case counters.<br>• `usb-boot-external=on` dynamically scans separately attached QEMU or physical `usb-host` devices on stable controller buses, is mutually exclusive with named machine backends, and preserves selected device/LUN identity and recovery state in VMState v40.<br>• Both modes retain deterministic device/LUN selection, exact unchanged image bytes, secure-image processing, firmware/config/DT/kernel resolution, timeout behavior, migration, and ARM64 handoff.<br>• A permanent `blkdebug` read error becomes a failed SCSI CSW, remains in LUN wait for the configured minimum 100 ms, and then falls through to SD.<br>• The standard functional gate uses unchanged official EEPROM default `0xf41`, falls through absent SD to mode-4 USB, and boots Linux from the same pinned official `.elf`, `.dat`, kernel, DTB, overlay, and initramfs bytes as the SD gate.<br>• The exact-release gate copies the full pinned 2,977,955,840-byte Raspberry Pi OS image unchanged to USB, reaches serial login with Linux root mounted from the same xHCI device, and observes guest first-boot identity replacement plus partition expansion. | Maintain exact-image VL805/DWC2 BOT boot, topology, recovery, migration, fallback, and production Linux gates; physical enumeration timing, extended reset stress, and hardware conformance remain Pass 2. | M4 |
| USB-005 | DWC2 USB device/gadget mode | ✅ Full | • DWC2 now honors force-device/force-host selection and exposes migratable device configuration/control/status, periodic Tx FIFO sizing, 16 IN/OUT endpoint register banks, endpoint disable/W1C interrupts, DAINT masking/summary, global endpoint interrupts, NAK commands/status, reset defaults, and mode indication.<br>• A transport-neutral host-token API enforces endpoint active/enable/NAK/stall state and selects either guest-memory DMA or device slave/PIO from `GAHBCFG.DMAEn`.<br>• DMA advances endpoint addresses exactly as before.<br>• PIO OUT/SETUP produces ordered pop-on-read `GRXSTSP` packet/completion entries plus little-endian `EPFIFO(0)` words; guest `EPFIFO(n)` writes supply IN packets, `DTXFSTS` reports configured free words, and `DIEPEMPMSK` drives Tx-empty endpoint interrupts.<br>• Receive and per-endpoint Tx queues are bounded by configured FIFO depth, reject overflow without consuming the armed transfer, support selected/all Tx and Rx flush, clear on USB/core reset, and migrate while payload and status entries are queued.<br>• USB bus reset also clears both global NAK latches so re-enumeration cannot inherit a traffic blockade.<br>• Dedicated qtests prove exact bytes/status, non-word-aligned padding, free space, interrupts, overflow, flush, reset, EP0 zero-length status stages, migratable one- and three-deep back-to-back SETUP-count consumption, multi-packet short termination, and source-to-destination continuation, and sixteen reset/re-enumeration cycles with EP0 SETUP plus simultaneously armed EP1/EP2 IN and OUT PIO traffic.<br>• A versioned framed `device-chardev` transport carries fragmented lifecycle, SETUP/OUT/IN, and suspend/resume requests into that API.<br>• Suspend sets `DSTS.SUSPSTS` and `GINTSTS.USBSUSP`, blocks tokens without losing endpoint/FIFO state, migrates, and resume clears status while raising the W1C wakeup interrupt; reset and disconnect clear suspended state.<br>• Firmware RPIBOOT explicitly retains DMA and its endpoint-arm plus packet/event callbacks let the behavioral VideoCore ROM own EP0 and segmented bulk DMA across both enumerations.<br>• The Linux packet-only proxy forwards lifecycle and endpoint tokens without implementing RPIBOOT state.<br>• VMState v30 preserves active endpoint/ROM state and dynamic captures while DWC2 VMState v7 preserves device/host PIO queues, host receive reservations, and an incomplete framed transport request across migration.<br>• Migration resumes halfway through bootcode; generation-tagged traffic survives one or bounded repeated host resets; chardev closure handles clean exit, exact disconnect, unexpected SIGKILL, and control/bulk timeout rollback.<br>• Active-transfer progress is QOM-visible and migrates.<br>• Reset-command qtests cover frame/channel/endpoint scope, attachment and configuration preservation, immediate command completion, and post-reset migration. | Maintain device-mode registers, DMA/PIO endpoints, framed transport, lifecycle, reset, migration, and active-transfer recovery; physical FIFO timing, extended reset storms, and trace conformance remain Pass 2. | M6 |
| USB-006 | BCM2711 ROM RPIBOOT protocol | ✅ Full | • The behavioral CM4 ROM owns DWC2 device mode, reset re-arming, exact `0a5c:2711` ROM and serial-bearing second-stage descriptors, address/configuration/status requests, stalls, the 24-byte boot message, announced bootcode length, segmented bulk `bootcode4.bin` capture, four-byte ROM status, disconnect/re-enumeration, and exact 260-byte get-size/read/done requests for `config.txt` and `boot.img`.<br>• It captures segmented file responses and exposes exact sizes/SHA-256 values for all three artifacts.<br>• On normal completion, the exact received `boot.img` becomes transient partitioned firmware media; unchanged start/fixup/kernel/initramfs/DT/overlay artifacts reach ARM64 handoff while eMMC remains separate.<br>• Socket qtests prove the protocol sequence, invalid-image rejection, partial-transfer rollback, and active migration; the real-host gate proves exact-image handoff.<br>• Real-host gates run unchanged official `rpiboot` commit `87d6e032` through Linux `dummy_hcd`, the packet-only proxy, and this ROM.<br>• A bounded reset campaign issues four real host resets after exactly 4,096 bytes in each enumeration—eight resets in one proxy/QEMU run—then preserves the pinned 105,984-byte bootcode, 238-byte config, and 29,360,640-byte image.<br>• Exact 4,096-byte disconnect/retry paths preserve the same artifacts.<br>• The timeout gate withholds ROM-status and file-request control replies for 20,001 ms and bulk completions at 4,096 bytes for 5,001 ms in both enumerations, crosses the official host's real deadlines, exits with intentional status 75, reconnects to the same QEMU, and completes exact clean retries.<br>• The same proxy now continues into the handed-off guest's exact ACM+MSD configuration, and the supervisor flashes its 4 GiB eMMC without substituting a host gadget.<br>• QEMU's locked provision-state file follows host ownership durably from wait through active, failed, retried, complete, flushed, and boot-ready states. | Maintain unchanged-rpiboot exact-artifact, reset/disconnect/timeout, migration, continued-guest, and lifecycle gates; extended storms, physical CM4 trace comparison, and privileged release-fixture operation remain Pass 2/release maintenance. | M6 |
| USB-007 | Host-visible virtual USB device | ✅ Full | • `qemu-rpi-dwc2-raw-gadget-proxy` uses host `dummy_hcd` and Raw Gadget to expose the in-process DWC2 device to libusb.<br>• It forwards CONNECT/RESET/ENUM/DISCONNECT, EP0 SETUP/IN/OUT, and endpoint-1 OUT packets over the versioned chardev framing; an announced-length barrier preserves control-after-bulk ordering.<br>• It owns no descriptors, filenames, file bytes, or RPIBOOT state.<br>• Because Linux Raw Gadget consumes physical SET_ADDRESS internally, it mirrors that hidden setup and status-IN stage into modeled DWC2 before the first nonzero SET_CONFIGURATION.<br>• Reset generations discard stale data and retain the endpoint worker across reconfiguration.<br>• `--reset-count` repeats an exact-byte reset boundary independently in both enumerations while retaining its one-reset default.<br>• `--hold-after` exposes an exact active-transfer crash point: the gate observes 4,096 accepted bytes through QOM, SIGKILLs the proxy process group, observes rollback to zero, and reconnects to the same VM.<br>• Exact-byte disconnects, official-boundary ROM/file control timeouts, and exact-byte bulk timeouts in either enumeration exit with status 75 and use the same rollback path.<br>• The QEMU-owned lifecycle moves `qemu-rpiboot-wait → rpiboot-active → rpiboot-failed → rpiboot-active → rpiboot-complete` across all four disconnect/crash scenarios and preserves completion on shutdown.<br>• Unmodified official `rpiboot` completes cleanly after eight active-transfer resets, both proxy crashes, disconnects, and timeout faults. | Maintain the packet-only Linux Raw Gadget bridge, exact lifecycle forwarding, crash/disconnect/timeout rollback, and unchanged-rpiboot gates; non-Linux transports are optional extensions and physical conformance remains Pass 2. | M6 |
| USB-008 | CM4 mass-storage gadget | ✅ Full | • `cm4_mass_storage.py` safely exports the same regular-file eMMC backend as a standard configfs USB mass-storage LUN.<br>• It prints a stable serial-bound whole-disk target, excludes udev partition links after a populated image re-enumerates, rejects multiple whole disks and unsafe media, and refuses mounted teardown.<br>• Its foreground `serve` owner holds the sibling advisory lock for the entire Imager write/verify/flush interval and releases only after an explicit `complete`/`failed` acknowledgement;<br>• EOF and termination fail closed.<br>• `recover-stale` handles SIGKILL/crash windows after winning the same lock: it tears down a remaining or already-removed gadget and can publish only `flash-failed`; live owners, mounted media, teardown failures, and unrelated states remain untouched.<br>• Ordered transitions preserve state-file ownership and publish active, failed, or boot-ready state.<br>• Fault commands disconnect USB, force-eject the SCSI medium after a host-sector threshold, or force-eject immediately after a completed host flush.<br>• Ejection retains enumeration but makes reads return `EIO`; status distinguishes an active empty LUN from an inactive gadget.<br>• The default supervisor retains one QEMU and the unchanged second-stage Linux ACM+MSD guest for the whole flash.<br>• It binds the exact 4 GiB `mmcblk0` disk and sole `/dev/ttyACM*` through the same qualified dummy-hcd `0a5c:0104` ancestry, proves bidirectional guest `ttyGS0` echo before and after unchanged Imager traffic, quiesces and flushes the eMMC, and releases only through lock-owning QEMU after exact acknowledgement.<br>• The compatibility configfs and Raw BOT owners remain available explicitly.<br>• A dedicated Raw BOT privileged gate separately proves storage and ACM enumeration/round-trip before and after the full BOT recovery corpus. | Maintain the continued-guest ACM+MSD production flow, exact eMMC ownership/locking, recovery helpers, fault paths, and host-flush handoff; longer storms, cross-host breadth, and hardware conformance remain Pass 2/regression expansion. | M6 |
| USB-009 | Unmodified Raspberry Pi Imager against virtual CM4 | ✅ Full | • The privileged gate pins the official v2.0.8 CLI-only x86-64 AppImage, compressed 2026-06-18 Raspberry Pi OS Lite archive, and decompressed payload hashes.<br>• It resolves the guarded by-id link to the device Imager enumerated and runs Imager unchanged without the system-drive override.<br>• Imager rejects an undersized virtual eMMC, live USB disconnect after writes begin, forced SCSI medium removal that leaves USB enumerated, and a command-specific failed SYNCHRONIZE CACHE that also retains USB.<br>• A fresh 4 GiB target then writes and verifies successfully, contains the exact payload, and boots through first-boot resize. | Maintain the pinned unchanged CLI Imager write/verify/flush, negative-fault, exact-payload, and post-flash boot gate. GUI/host-platform breadth and additional FUA fault coverage are optional expansion; physical comparison remains Pass 2. | M6 |
| USB-010 | PCIe-connected VL805 USB 3 | ✅ Full | • Pi 4B instantiates a `1106:3483` VL805-compatible XHCI endpoint behind the BCM2711 PCIe link with two USB2 and two USB3 ports.<br>• The production Pi kernel enumerates it, binds `xhci_hcd`, allocates a native BCM2711 MSI IRQ, and completes unchanged USB boot traffic.<br>• The production `NOTIFY_XHCI_RESET` mailbox tag accepts the hardwired `0x00100000` PCI address and cold-resets the modeled xHCI controller without removing its PCI function; invalid addresses and CM4 without an onboard VL805 are no-ops.<br>• A saturating read-only notification counter resets locally and migrates in property VMState v6.<br>• Qtests cover identity, BAR mapping, XHCI capability access, mailbox-driven command-register reset, invalid address, CM4 behavior, reset, and migration; production Linux proves active MSI. | Maintain PCIe identity/BAR/MSI, mailbox-reset, xHCI traffic, production Linux, reset, and migration gates. The opaque VL805 MCU instruction stream is outside the bounded behavioral compatibility contract and is not claimed; physical topology, timing, error behavior, and traces remain Pass 2. | M8 |
| USB-011 | Automated CM4 production-flow supervisor | ✅ Full | • `cm4_provision.py` is a single fail-closed controller for QEMU-owned modeled DWC2/ROM, the packet-only Raw Gadget proxy, unchanged official `rpiboot`, the same continued guest Linux ACM+MSD gadget, unchanged official Imager write/verify/flush, and post-flash QEMU handoff on the same eMMC bytes.<br>• It requires a strict versioned SHA-256 manifest for EEPROM, its independently pinned BCM2711 `bootsys` contents, ROM key index, and ordered dependency set, host binaries, RPIBOOT files, and compressed/raw OS payloads; independently validates the envelope/key index, decompresses and authenticates the EEPROM LZ4 dependency chain, copies the immutable EEPROM byte-for-byte into a writable private SPI backend, asserts lifecycle/lock/boot/RAM/revision/QMP observations, verifies in-process RPIBOOT sizes and hashes through QMP, requires the exact received `boot.img` to reach ARM handoff, qualifies the resulting host whole disk by exact USB/SCSI/path/capacity identity, binds the matching ACM tty through the same USB ancestry, and proves bidirectional guest echo before and after Imager.<br>• Only then may the lock-owning QEMU instance publish `boot-ready` after host flush.<br>• It continuously drains proxy and both guest serial streams, retains explicit standalone-helper/configfs/Raw-BOT recovery compatibility, and writes an atomic JSON report that records `continued-guest` as the active mass-storage mode.<br>• Thirty-three supervisor unit tests and focused lifecycle/memory-layout qtests pass.<br>• Privileged exact-artifact runs prove unchanged RPIBOOT, matching ACM+MSD, unchanged Imager, exact flashed payload verification, and post-flash ARM handoff without a gadget substitution on every 1/2/4/8 GiB CM4 memory model; the retained multi-memory evidence binds all four reports to one artifact-set hash.<br>• Already-loaded host modules are accepted only after parameter verification; absent modules remain bounded and fail-closed. | Maintain the exact gate and refresh the privileged release report whenever the trust manifest changes. | M6 |
| USB-012 | USB electrical/reset timing | 🧪 HIL pending | • Logical USB packets do not reproduce PHY behavior. | Retain cable, hub, signal, and power-cycle tests on hardware. | HIL |
| USB-013 | Command-visible USB Mass Storage BOT target | ✅ Full | • The Linux build produces `qemu-rpi-cm4-msd`, a Raw Gadget ACM+MSD target backed by the exact eMMC file.<br>• Its one-eMMC descriptor profile follows the unchanged pinned second-stage configuration: Broadcom `0a5c:0104`, official Raspberry Pi strings, 500 mA bus power, MSD interface 0, CDC ACM interfaces 1/2, and `mmcblk0` inquiry identity.<br>• CDC implements line coding, control-line state, break acknowledgement, and a reset-safe testable data pair.<br>• The default production path now uses the actual continued Linux guest instead: the supervisor binds its matching ACM and eMMC through one USB ancestry and proves real guest `ttyGS0` bidirectional traffic before and after Imager.<br>• BOT exposes INQUIRY/VPD, sense, capacity, caching/DPOFUA mode pages, READ/WRITE(10/16), FUA, SYNCHRONIZE CACHE(10/16), verify, and LUN commands.<br>• Named command faults return CHECK CONDITION without disconnecting USB.<br>• Independent EP0 and bulk loops inject BOT phase status, consume a command without CSW for a real timeout, or pause READ(16) or WRITE(16) after a configurable 1-to-131,072-byte prefix.<br>• A bounded `--fault-count` preserves one-shot compatibility while enabling four active READ and four active WRITE reset cycles on one target per direction.<br>• The compiled self-test pins all 98 configuration-descriptor bytes and proves an exact 127-byte durable prefix at a nonzero backend offset.<br>• The privileged gate passes four distinct-LBA cycles with 128-byte WRITE cutoffs, four durable sub-sector prefixes, four untouched 3,968-byte suffixes, failed old commands, USB reconfiguration, fresh INQUIRY/read I/O, exact `lsusb` identity, `/dev/ttyACM*` discovery, and 115200 ACM round-trip.<br>• The target accepts a complete host USB transfer unit while syncing only a smaller configured durable prefix.<br>• An eight-command class-reset burst with no intervening I/O also recovers.<br>• A separate libusb host probe executes all thirteen USB-IF direction-length cases, five meaningfulness cases, a fixed-seed locked-digest 128-case randomized corpus, and two invalid wrappers with ordered Reset Recovery; the eMMC SHA-256 remains unchanged, kernel I/O recovers, and the ACM path remains live.<br>• The target shares the lifecycle lock, publishes `flash-failed` for command faults, has two compiled self-tests, and passes real CACHE SYNC/FUA probes.<br>• Official Imager hits its failed-flush path while USB remains present.<br>• Its foreground-owner path prevents new commands, drains an active command, syncs the backend, and passed a clean full-release official-Imager write/verify/flush followed by CM4 handoff. | Maintain descriptor, ACM, BOT/SCSI command/fault, randomized matrix, reset recovery, durability, lifecycle, official-Imager, and handoff gates. Additional fuzz breadth, host backends, and extended storms are regression expansion; physical timing comparison remains Pass 2. | M6 |

### USB-006 in-process RPIBOOT software-boundary submatrix

| Stage | Production evidence | Status |
|---|---|---:|
| BCM2711 ROM enumeration | Unchanged host sees `0a5c:2711`, serial index 0 | ✅ Full |
| Boot message | Exact 24-byte message reaches DWC2 EP1 DMA | ✅ Full |
| `bootcode4.bin` | Exact 105,984 bytes; SHA-256 `79648752…ce694` | ✅ Full |
| Disconnect/re-enumeration | Same host process discovers serial index 4 | ✅ Full |
| `config.txt` file service | Exact 238 bytes; SHA-256 `f74a9db0…b687` | ✅ Full |
| `boot.img` file service | Exact 29,360,640 bytes; SHA-256 `a434c8d5…57c0` | ✅ Full |
| Protocol completion | Lifecycle reports `rpiboot-complete`; official host exits on `Done` | ✅ Full |
| Transient firmware media | Exact received partitioned `boot.img` is read in memory; eMMC remains separate | ✅ Full |
| Exact ARM handoff | Unchanged start/fixup/kernel/initramfs/DT/overlays reach ARM64 entry | ✅ Full |
| Active migration | Half-received bootcode resumes through destination DWC2 and hashes exactly | ✅ Full |
| Active USB reset | Unchanged host survives four resets at 4,096 bytes in each enumeration, then completes exactly | ✅ Full |
| Active disconnect/retry | Both stages fail at 4,096 bytes and cleanly reconnect to the same QEMU | ✅ Full |
| Proxy crash/retry | Both stages expose 4,096 active bytes, survive proxy SIGKILL, roll back to zero, and reconnect to the same QEMU | ✅ Full |
| Durable ownership lifecycle | Wait, active, failed, retried-active, complete, and post-shutdown states are fail-closed across both stages | ✅ Full |
| Official control timeout/retry | ROM status and file request exceed 20 seconds, fail, and retry on the same QEMU | ✅ Full |
| Official bulk timeout/retry | ROM bootcode and file-server image stop at 4,096 bytes for more than 5 seconds, fail, and retry on the same QEMU | ✅ Full |
| **Software boundary** | **16/16 through real Linux USB, in-process QEMU DWC2, exact second-stage handoff, fault recovery, lifecycle, and migration** | **100%** |

The parent USB-006 row remains Partial because fault/reset campaigns through
this proxy, release-CI deployment, and physical descriptor/timing conformance
remain open.

### USB-013 BOT thirteen-case submatrix

This submatrix is scored at the Linux Raw Gadget plus independent libusb
boundary. `Hn`, `Hi`, and `Ho` are the host's no-data, data-in, and data-out
expectations; `Dn`, `Di`, and `Do` are the command's device intents. A phase
error is not considered complete unless the host executes ordered Reset
Recovery and a fresh command succeeds.

| Case | Host / device relation | Implemented result | Host proof | Status |
|---:|---|---|---|---:|
| 1 | `Hn = Dn` | Passed CSW, zero residue | TEST UNIT READY | ✅ Full |
| 2 | `Hn < Di` | Phase-error CSW, Reset Recovery | INQUIRY with zero host length | ✅ Full |
| 3 | `Hn < Do` | Phase-error CSW, Reset Recovery | BYTCHK VERIFY with zero host length | ✅ Full |
| 4 | `Hi > Dn` | Bulk-In halt, passed CSW after clear, full residue | TEST UNIT READY with host IN data | ✅ Full |
| 5 | `Hi > Di` | Short device data, Bulk-In halt, passed CSW, difference residue | 36-byte INQUIRY in a 64-byte host transfer | ✅ Full |
| 6 | `Hi = Di` | Exact data, passed CSW, zero residue | 36-byte INQUIRY | ✅ Full |
| 7 | `Hi < Di` | Host-length data, Bulk-In halt, phase-error CSW, Reset Recovery | 16-byte host transfer for 36-byte INQUIRY | ✅ Full |
| 8 | `Hi <> Do` | Bulk-In halt, phase-error CSW, full residue, Reset Recovery | IN-directed BYTCHK VERIFY | ✅ Full |
| 9 | `Ho > Dn` | Host data accepted/discarded, passed CSW, full residue | OUT-directed TEST UNIT READY | ✅ Full |
| 10 | `Ho <> Di` | Host data accepted/discarded, phase-error CSW, full residue, Reset Recovery | OUT-directed INQUIRY | ✅ Full |
| 11 | `Ho > Do` | Intended data accepted, excess discarded, passed CSW, difference residue | 1,024 host bytes for one-block BYTCHK VERIFY | ✅ Full |
| 12 | `Ho = Do` | Exact data accepted, passed CSW, zero residue | 512-byte BYTCHK VERIFY | ✅ Full |
| 13 | `Ho < Do` | Host bytes accepted, phase-error CSW, zero residue, Reset Recovery | 256 host bytes for one-block BYTCHK VERIFY | ✅ Full |
| **BOT case slice** | **13/13 cases** |  | **Real libusb endpoint traffic** | **100%** |

This 100% is a nested protocol slice, not a promotion of USB-013. USB-013
remains Partial until its wider malformed-command, descriptor/timing,
data-bearing fuzz, reset-storm, cross-host, throughput, and hardware-conformance requirements
pass.

### USB-013 deterministic robustness corpus

| Corpus category | Cases/checkpoints | Completion evidence | Status |
|---|---:|---|---:|
| USB-IF thirteen-case matrix | 13 | Real libusb data, halt, CSW, and Reset Recovery checks | ✅ Full |
| Valid-but-meaningless CBWs | 5 | Deterministic failed CSW and immediate clean command | ✅ Full |
| Invalid transport wrappers | 2 | Both endpoints halt until ordered Reset Recovery | ✅ Full |
| Fixed-seed randomized CBW/SCSI | 128 | Seed `0x52504934`, locked FNV-64 `2e5f4d8d3041b5d7` | ✅ Full |
| Interleaved liveness checkpoints | 8 | TEST UNIT READY every sixteen randomized cases | ✅ Full |
| Non-mutation check | 1 | eMMC SHA-256 identical before and after the corpus | ✅ Full |
| **Deterministic robustness slice** | **157 cases + 9 checkpoints** | **Independent host traffic** | **100%** |

The deterministic slice does not claim exhaustive fuzzing. Coverage-guided,
data-bearing, concurrent-reset, and long-duration randomized campaigns remain
required before USB-013 can become Full.

### USB-013 active reset-storm submatrix

| Reset scenario | Cycles | Completion evidence | Status |
|---|---:|---|---:|
| Active READ(16) USB device reset | 4 | Each command stops after 512 bytes; no remaining data or stale CSW | ✅ Full |
| Active WRITE(16) USB device reset | 4 | 128-byte durable prefixes and untouched 3,968-byte suffixes pass through real Raw Gadget | ✅ Full |
| Between-cycle recovery | 8 | Stable by-id identity, INQUIRY, and block read after every active reset | ✅ Full |
| Idle BOT class-reset burst | 8 | Back-to-back reset requests with no intervening I/O; final I/O passes | ✅ Full |
| Timeout-driven USB port reset | 1 | Missing CSW forces Linux reset/reconfiguration and recovery | ✅ Full |
| Phase-error-driven USB port reset | 1 | Phase status forces Linux reset/reconfiguration and recovery | ✅ Full |
| **Bounded active-reset slice** | **18 resets + 8 recovery checkpoints** | **Real Linux USB/SCSI stack** | **100%** |

This bounded slice does not cover simultaneous reset callers, resets at every
byte offset, hub/cable faults, multi-hour stress, or physical USB timing.
Those remain required campaign and HIL work.

## 6. GPIO and low-speed peripherals

| ID | Capability | Status | Current implementation / evidence | Required before Full | Milestone |
|---|---|---:|---|---|---|
| GPIO-001 | BCM2711 GPIO function-select registers | ✅ Full | • Function selection is stored, SD routing is recognized, and transitions into/out of GPIO output mode update the value and explicit output-enable wires.<br>• The documented PWM0 routes are active: channel 0 owns GPIO12 ALT0/GPIO18 ALT5 and channel 1 owns GPIO13 ALT0/GPIO19 ALT5/GPIO45 ALT0.<br>• One qtest covers every PWM route, both channels, logical output enable, reset, and live migration; the general GPIO tests cover both banks. | Maintain GPIO/PWM/SD alternate-function ownership, output-enable, reset, and migration gates; additional peripheral mux breadth is regression expansion and physical comparison remains Pass 2. | M2 |
| GPIO-002 | GPIO output set/clear/level | ✅ Full | • GPSET/GPCLR maintain a distinct output latch for all 58 BCM2711 pins, including GPIO54-57.<br>• `GPLEV` selects the latch in output mode and the peripheral signal on implemented alternate functions.<br>• Direction changes drive a tested `pin-output-enable` signal.<br>• PWM0 waveform edges propagate identically through `GPLEV`, named value/output-enable wires, event detection, the host bridge, and VMState v7. | Maintain 58-pin latch/level/output-enable, implemented peripheral waveform, event, bridge, reset, and migration gates; open-drain/contention and physical conformance remain Pass 2. | M2 |
| GPIO-003 | GPIO input injection | ✅ Full | • BCM2838 GPIO exposes 58 named `pin-input` QEMU lines and the optional `gpio-chardev` host protocol.<br>• Driven high/low and disconnected (`-1`/`SET pin Z`) states update `GPLEV`; qtests cover boundary pins 0/31/32/53/54/57, direction overrides, pulls, events, upper-bank behavior, and externally driven state persisting across controller/system reset.<br>• Dedicated input-only `EEPROM_NWP` and `SD_OVERCURRENT` signals cross the same libgpiod or USB QGPIO bridge without fabricated guest GPIO numbers; high, low, high-impedance fallback, source telemetry, disconnect release, and source-to-destination restoration are tested.<br>• The host daemon propagates physical input edges.<br>• Input value/drive masks migrate from VMState v2 and semantic board signals migrate in VMState v6.<br>• A live source-to-destination bridge test proves the destination hides reset defaults, then restores a driven input plus both board signals and accepts a new edge after migration. | Maintain all-bank injection, pulls/events, semantic board signals, host-daemon edge, reset, migration, and disconnect-release gates; reviewed fixture maps and electrical contention/traces remain Pass 2. | M2 |
| GPIO-004 | Rising/falling event detection | ✅ Full | • GPREN/GPFEN and GPAREN/GPAFEN are stored for both banks and latch GPEDS on logical pin transitions, including CPRMAN-timed PWM0/PWM1 alternate-function edges.<br>• W1C acknowledgment, reset, group boundaries, synchronous/asynchronous enables, stopped-clock PWM migration, and post-migration edges are qtested;<br>• VMState v8 stores event, mux, and both PWM-controller input states while retaining the v7 PWM0 wire field. | Maintain rising/falling synchronous/asynchronous enable, W1C, alternate-function edge, reset, and migration gates; sampling-filter and physical timing distinction remain Pass 2. | M2 |
| GPIO-005 | High/low level detection | ✅ Full | • GPHEN/GPLEN detect current logical levels in both banks; enabling an active level sets GPEDS, and W1C immediately relatches while the condition persists.<br>• VMState v3 stores level-detect configuration/status. | Maintain high/low detect, immediate enable, persistent-condition relatch, reset, and migration gates; sampling/timing and physical conformance remain Pass 2. | M2 |
| GPIO-006 | GPIO interrupt routing | ✅ Full | • GPEDS routes to BCM2711 GIC SPI 113--115 using the Linux-visible pin groups 0--27, 28--45, and 46--57.<br>• All groups and deassertion are qtested; the fourth documented wake line (SPI 116) is present and held inactive. | Maintain all three Linux-visible GPIO IRQ groups, deassertion, reset, migration, and production kernel gates; wake policy expansion and physical interrupt conformance remain Pass 2. | M2 |
| GPIO-007 | Pull-up/down register storage | ✅ Full | • BCM2711 pull-control values are stored and now resolve disconnected input pins dynamically: pull-up reads high, while none/pull-down/reserved resolve low; an actively driven external level wins. | Maintain BCM2711 pull storage, disconnected resolution, external-drive precedence, reset, and migration gates; floating/electrical oracle behavior remains Pass 2. | M2 |
| GPIO-008 | Host `/dev/gpiochip` proxy as VirtIO GPIO | ➖ Excluded | • VirtIO GPIO is an explicitly non-BCM guest ABI and cannot be part of an identical Raspberry Pi platform.<br>• The fidelity path is the implemented, versioned direct BCM host bridge in GPIO-009, which leaves the guest-visible BCM2711 GPIO controller and unchanged Raspberry Pi software path intact. | Retain the explicit exclusion while the north star requires Raspberry Pi ABI fidelity. Reopen only as a separately named convenience-machine feature, never as completion work for the faithful Pi machine. | Optional |
| GPIO-009 | Host GPIO proxy into BCM2711 pins | ✅ Full | • `-M raspi4b,gpio-chardev=ID` and `raspi-cm4` expose the tested versioned socket protocol.<br>• `gpio_proxy.py` adds a strict versioned pin/signal map, official libgpiod v2 backend, and deterministic USB-serial backend with input-first acquisition, explicit output authorization, active-low/bias/drive/debounce policy, atomic direction/value changes, edge propagation, and fail-safe reset/error/signal/disconnect cleanup.<br>• The CM4 behavioral ROM consumes bridged GPIO40 as its real active-low boot strap, while the input-only semantic `EEPROM_NWP` signal crosses the same host/HIL connection without being exposed as a BCM GPIO.<br>• Incoming migration suppresses the pre-load banner so a connected fixture never sees destination reset defaults; after VMState load an idempotent banner plus `GET ALL`/`GET SIGNALS` synchronizes restored pins and the board signal before new edges are accepted.<br>• Fake libgpiod, pseudo-terminal USB protocol including command/edge races, real Unix-socket, migration, and recovery-decision tests exercise the host path without hardware. | Maintain the versioned direct BCM GPIO/semantic-signal protocol, safe mapping, reset/disconnect, migration, libgpiod/USB backends, and host tests. Fixture qualification, electrical contention, and physical conformance remain Pass 2; keep unauthenticated transports local. | M2 |
| GPIO-010 | Dedicated USB-to-GPIO bridge | ✅ Full | • The `usb-serial` backend implements the documented `QGPIO 1` CDC ACM protocol: strict version/line negotiation, input-first configuration, atomic output enable/value, reads, lossless asynchronous edge queueing, `SAFE`, 200 ms keepalive, bounded framing/timeouts, and disconnect cleanup.<br>• A portable firmware core implements logical polarity, debounce, fail-closed parsing, a 500 ms session watchdog, disconnect release, and HELLO-only recovery; its compiled Meson self-test passes.<br>• The Pico SDK 2.3.0 RP2040 frontend adds USB CDC, glitch-resistant push-pull/open-drain/open-source GPIO, a reviewed header-pin allow-mask, suspend/disconnect detection, and a separate 250 ms loop watchdog.<br>• It cross-builds under official Arm GNU 15.2.Rel1; two clean builds produced identical manifest-pinned `.bin` and `.uf2`, and an automated verifier checks source/output hashes and sizes. | Maintain protocol/core/frontend source, deterministic build manifest, self-test, fail-safe parsing/watchdogs, and verifier. Flashing, reconnect/power-cycle, voltage, contention, timing, and physical boot-strap qualification remain Pass 2. | M2 |
| GPIO-011 | Host internal `gpiochip0` use | 🧪 HIL pending | • Detected as Intel `INTC1085:00`; not a safe general-purpose test header. | Use a dedicated isolated GPIO fixture instead. | HIL |
| GPIO-012 | SPI controller | ✅ Full | • SPI0 implements polled/interrupt FIFO transfers, paced SCLK from CPRMAN, physical CE outputs, and BCM DMA maps 6/7 with framing, DREQ/panic thresholds, exact DLEN completion, reset, and live migration.<br>• The shared AUX block additionally implements both SPI1/SPI2 production register windows: enable-gated access, independent four-entry 32-bit TX/RX FIFOs and SSI buses, fixed/variable widths, IO/TXHOLD framing, native CS patterns, peek/pop status, TX-empty/idle shared IRQ, `core / (2 * (speed + 1))` pacing, clock stop/resume, reset, and active migration.<br>• Its qtest reads an attached W25Q80BL JEDEC identity over a held SPI1 frame and migrates a clock-frozen SPI2 entry.<br>• A production functional gate boots the pinned unchanged Raspberry Pi 5.15 kernel, official SPI1/SPI2 overlays, and byte-unchanged packaged AUX-SPI/spidev modules; both stock platform drivers bind, both spidev nodes appear, and both attached flashes accept guest-driver transfers. | Maintain SPI0/AUX SPI1/SPI2 FIFO, framing, CS, clock, DMA, IRQ, reset, migration, guest-driver, and attached-flash gates. Additional mode/quirk/error breadth is regression expansion; bit-edge and hardware conformance remain Pass 2. | M8 |
| GPIO-013 | I²C controllers | ✅ Full | • Three BCM2835 BSC controllers are mapped at the BCM2711 addresses, with the separately tested HDMI-I2C path retained for DDC.<br>• Control requires `I2CEN` plus one-shot `ST`; command bits read back clear; address, DLEN, DIV, DEL, and CLKT registers apply the architectural layout, reset values, and migration contract.<br>• The directional FIFO is 16 bytes deep, asserts RXR at 12 bytes and TXW below four, ignores writes while full, stalls RX while full, and supports clear-abort.<br>• Address or attached-device data NACK completes with `ERR|DONE` and no stale `TA`; status W1C immediately recomputes IRQ.<br>• Reset terminates live bus ownership and post-load restores it and the IRQ.<br>• Generic I2C targets may expose `ten-bit-address`;<br>• BSC translates the documented `11110xx` A-register prefix plus FIFO low-address byte, retaining the selected 10-bit target across write/read phases and active migration.<br>• Controller injection and the generic per-slave `I2CSlaveClass::stretch` callback hold a selected byte for live VPU/core-clock-derived SCL cycles: a short hold resumes, while one beyond `CLKT` completes with `CLKT|DONE`; clock gating pauses the deadline and VMState v5 preserves the one-shot request, ten-bit phase, and remaining interval.<br>• Eight standalone controller tests plus Pi 4 native-address, ten-bit active-migration, and stretch tests cover all three instances, valid 7/10-bit TMP105 traffic, address/data NACK and recovery, command gating, masks, FIFO/IRQ thresholds, overflow, clear-abort, reset, repeated start, clock gating, and active migration.<br>• BCM2711 BSC is specified as single-master-only, so multi-master arbitration is intentionally excluded. | Maintain the 7/10-bit transaction, FIFO, NACK, stretch, register, IRQ, reset, and migration corpus. Electrical FEDL/REDL edge timing and hardware conformance remain deferred Pass 2 work. | M8 |
| GPIO-014 | BCM2835/BCM2711 PWM controllers | ✅ Full | • A two-channel MMIO model is mapped as PWM0 at ARM address `0xfe20c000` (VC `0x7e20c000`) on all supported Pi SoCs.<br>• BCM2711 Pi 4B and CM4 additionally instantiate an independent PWM1 at ARM `0xfe20c800` (VC `0x7e20c800`).<br>• Each block implements CTL, dynamic STA, DMAC masking, RNG/DAT for both channels, its own shared 16-word FIFO, ordered reads/writes, full/empty state, overflow/underflow sticky errors with W1C, FIFO clear command self-clear, reset, and migration validation.<br>• FIFO-enabled channels load the first word when transmission starts and then consume one word per programmed range of shared CPRMAN PWM source-clock cycles;<br>• RPTL repeats the last word without a gap in single-channel use.<br>• When both channels share a FIFO, A/C/E words remain assigned to channel 0 and B/D/F to channel 1; both request the next words in lock-step, so a shorter range idles until the longer channel reaches the same boundary.<br>• One-word starvation retains the next owner.<br>• Live rate changes and stop/resume preserve remaining period and edge source cycles independently.<br>• Firmware PWM-clock mailbox writes gate and retime that same shared source for PWM0 and PWM1, with disabled next-rate, measured-rate, reset, and migration coverage.<br>• Empty consumption sets GAPO, and named normal/panic DMA-threshold outputs follow each DMAC and FIFO level.<br>• PWM0 requests drive BCM DMA peripheral map 5 and PWM1 drives map 1, providing threshold-paced FIFO refill with ACTIVE/HELD, normal/panic priority arbitration, CS.DREQ reporting, and exact partial-control-block migration through a guarded deferred resume.<br>• Separate `channel-enabled` and waveform outputs expose logical state.<br>• Per-device VMState v4 preserves active data/range, waveform position/target/level, both period and edge timers, stopped-clock remaining cycles, shared-FIFO wait bits, and next ownership.<br>• The waveform engine implements the distributed PWM algorithm, mark-space ratio, MSB-first serialization with truncation/padding, SBIT idle, POLA inversion, and both channels.<br>• BCM2711 routes PWM0 through GPIO12/13 ALT0, GPIO18/19 ALT5, and GPIO45 ALT0, and PWM1 through GPIO40/41 ALT0.<br>• Qtests cover both controllers and channels, register/FIFO isolation, equal and unequal-range lock-step, starvation ownership, exact timed load/drain/order/repeat, DMA refill and completion on both DREQ maps, panic and normal contention, thresholds, all waveform modes, every GPIO route/events/output enable, clock gating, five live migrations, outputs, masks, errors, reset, and commands.<br>• Unchanged production Pi EEPROM/firmware/DT plus the official unchanged `pwm-2chan.dtbo` enable and retain the PWM0 Linux DT node on both SD and USB boot paths;<br>• PWM1 remains independently available for BCM2711 DT use. | Maintain dual-controller/channel register, FIFO, waveform, CPRMAN, DMA, GPIO, IRQ/error, reset, migration, overlay, and production Linux gates. Additional malformed/PWM1 driver breadth is regression expansion; waveform/electrical calibration remains Pass 2. | M8 |
| GPIO-015 | Electrical voltage/current/contention | 🧪 HIL pending | • QEMU wires carry logical state only. | Use protected physical fixtures for electrical tests. | HIL |
| GPIO-016 | BCM2835 AUX mini-UART | ✅ Full | • The model stores ENABLES, LCR, MCR, scratch, CNTL, and the native baud register; implements 8250 DLAB divisor access, CTS, enable-gated TX/RX interrupts, reset defaults, and separate eight-byte RX/TX FIFOs.<br>• It consumes CPRMAN's BCM2711 VPU/core output; the Pi 4 reset profile supplies the documented fixed 250 MHz mini-UART clock, while live divider writes, clock stop/resume, and reset propagate normally.<br>• The exact 16-bit UART divider schedules one ten-bit frame per virtual deadline instead of blocking QEMU for instantaneous output.<br>• Mid-frame core-rate changes preserve remaining source cycles and a stopped clock freezes TX.<br>• LSR/STAT expose TX full/empty/fill/idle state;<br>• IIR commands clear either FIFO, and TX-empty IRQ state follows actual drain completion.<br>• VMState v4 validates and preserves both FIFO orders, the active TX timer, and stopped-frame remaining cycles.<br>• A real-socket qtest migrates four queued RX bytes and three active TX bytes with the VPU clock stopped, restores the migrated clock configuration on a fresh destination, and proves exact register/divider state, per-frame timing, byte order, status, and IRQ completion.<br>• A dedicated clock qtest covers live half-rate timing, mid-frame rescaling, gating, resume, and reset.<br>• The register qtest also covers masks, DLAB/native-divider coherence, TX overflow bounding/clear, status, interrupt gating, and reset.<br>• A production gate boots unchanged official EEPROM/firmware/kernel artifacts through `console=serial0`; behavioral firmware resolves it to `ttyS0`, the official kernel registers `fe215040.serial`, and the initramfs shell is usable through the second QEMU serial chardev.<br>• The exact unchanged Raspberry Pi OS release also emits its kernel log through this UART and reaches systemd after mounting root. | Maintain mini-UART registers/FIFOs/IRQ, divider/clock, timed TX, reset, migration, socket, console, and exact-release gates. Break/flow-control/RS-485/DVFS breadth is optional expansion; physical serial conformance remains Pass 2. | M8 |
| GPIO-017 | VideoCore firmware GPIO expander | ✅ Full | • The property-mailbox model implements GET/SET GPIO STATE and CONFIG for firmware GPIOs 128--135 with persistent direction, polarity, termination, pull, and state, success/error conventions, reset, VMState v2, and eight named logical input/output wires.<br>• A live source/destination migration qtest proves configuration and driven state survive, remain readable through the mailbox, and still control the output line; the same test covers ordinary configuration/state round trips, invalid IDs, input/output updates, and reset.<br>• The exact unchanged release reaches its serial login prompt without the prior `raspberrypi-exp-gpio`, LED, or regulator probe failures. | Maintain firmware GPIO mailbox, named logical wires, configuration/state, reset, migration, and exact-release gates. Board-rail mapping/default oracles and physical conformance remain Pass 2. | M8 |

### GPIO logical pin-bridge submatrix

| Contract element | Coverage | Status |
|---|---|---:|
| External driven-low/driven-high inputs | Pins 0, 31, 32, 53, 54, and 57 qtested | ✅ Full |
| External disconnect/high-impedance input | Negative QEMU IRQ level clears the drive-valid mask | ✅ Full |
| Pull resolution | Pull-up, pull-down, and externally driven precedence qtested | ✅ Full |
| Direction-aware `GPLEV` | External input in input mode; output latch in output mode | ✅ Full |
| Output latch across direction changes | GPSET/GPCLR while input is retained and exposed on output transition | ✅ Full |
| Output-enable notification | 58 `pin-output-enable` wires; transitions qtested | ✅ Full |
| Full BCM2711 upper bank | GPSET1/GPLEV1/FSEL cover GPIO32-57, including GPIO54-57 | ✅ Full |
| Migration | Destination suppresses reset defaults, then resynchronizes migrated input, output, pull, event, and IRQ state before accepting a new edge | ✅ Full |
| Host socket/physical adapter | QEMU transport, fail-closed libgpiod/USB-serial/mock backends, portable firmware core, and reproducible RP2040 UF2 build are tested; flashed fixture remains | 🟡 Partial |
| Edge/level events and interrupts | Register, latch, W1C, three pin groups, and parent IRQ routing qtested; wake/sampling conformance remains | 🟡 Partial |
| **Logical bridge slice** | **8 Full, 2 Partial, 0 Missing** | **90.0%** |

## 7. Network, PCIe, and remaining peripherals

| ID | Capability | Status | Current implementation / evidence | Required before Full | Milestone |
|---|---|---:|---|---|---|
| PER-001 | BCM2711 PCIe root complex | ✅ Full | • A GPEX-backed host exposes BCM2711 root configuration, indexed downstream configuration, the 1 GiB outbound window, link/revision status, and native INTx A-D routes.<br>• A dedicated root-port type exposes Broadcom BCM2711 `14e4:2711`, revision `20`, class `060400`, and the board's Gen2 x1 maximum link instead of the generic QEMU identity and Gen4 x32 capability.<br>• The internal MSI block implements both documented target addresses, 32 data-selected vectors, status/clear/mask registers, GIC SPI 148, reset, and migration.<br>• The real `brcm,bcm2711-pcie` DT node remains visible with its MSI binding; the unchanged production driver brings up bus 1, reports the exact root identity in sysfs, allocates native MSI for VL805/NVMe, and completes their I/O.<br>• Qtests cover Pi 4B and CM4 endpoints, exact configuration space, both MSI doorbells, masking/clearing/GIC routing, reset, pending controller migration, PCI-device migration, and retained identity. | Maintain BCM2711 root identity/configuration, outbound window, INTx/MSI, endpoint, reset, migration, and production Linux gates; SSC/register breadth and physical reset/link/timing comparison remain regression expansion and Pass 2. | M8 |
| PER-002 | GENET Ethernet controller | ✅ Full | • A BCM2711 GENET v5 SysBus NIC is mapped at native ARM address `0xfd580000` with both GIC SPIs 157/158.<br>• Its configured unicast identity is also the firmware board-MAC mailbox response in direct and behavioral boot, and remains coherent across reset and migration on Pi 4B and CM4.<br>• It implements the 64 KiB register boundary, immutable v5 revision, both level-2 interrupt banks, Clause 22 UniMAC MDIO, a BCM54213PE-compatible external PHY, backend-driven link state, reset/migration state, and v5 40-bit descriptor DMA.<br>• TX consumes all 17 rings, strips the 64-byte transmit status block, performs requested checksum offload, sends through a standard QEMU network backend, updates indices, and raises the correct priority/default completion interrupt.<br>• RX supports the legacy ring-16 and current ring-0 default layouts plus priority rings, writes the 64-byte receive status block and two-byte alignment pad into guest memory, updates indices, and raises the corresponding interrupt.<br>• The 17-slot UniMAC MDF implements guest-programmed unicast, multicast, and broadcast selection plus promiscuous bypass.<br>• RX/TX MIB groups count wire bytes, size buckets, accepted packets, unicast/multicast/broadcast classes, good packets, and filter misses with architectural group-reset behavior.<br>• Bounded, migratable `packet-drop-direction=1|2|3`, `packet-drop-after`, and `packet-drop-count` controls inject shared-boundary TX/RX loss for both firmware and guest traffic with read-only progress counters.<br>• Bounded `dma-error=1|2`, `dma-error-after`, and `dma-error-count` properties inject TX-read or RX-write failures at deterministic byte boundaries.<br>• TX reports descriptor underrun/TBUF IRQ without emitting a frame;<br>• RX leaves guest memory and the producer index unchanged while reporting RBUF overflow/error.<br>• Read-only progress counters persist across reset and migrate, and traffic recovers after the selected occurrence count.<br>• Socket-backend qtests prove exact packet bytes, filter acceptance/rejection, counters/reset, both fault/recovery paths, fault-state migration, and QMP link propagation through PHY, RGMII, and interrupt state.<br>• An unchanged production EEPROM, `start4.elf`, DTB, 5.15 kernel, and initramfs negotiates 1 Gbps/full duplex, exchanges DHCP through TX ring 2 and RX ring 16, and obtains a user-network `10.0.2.x` address. | Maintain GENET v5 identity, descriptor DMA, filtering, MIB, PHY/link, deterministic faults, migration, firmware traffic, and production Linux DHCP gates. HFB/WOL/EEE/statistics breadth and in-flight stress are regression expansion; physical conformance remains Pass 2. | M8 |
| PER-003 | Network BOOT_ORDER | ✅ Full | • By default, the pre-ARM GENET client performs checksummed DHCP DISCOVER/OFFER/REQUEST/ACK with configured packet retransmission inside one DHCP deadline, emits the Pi-compatible DHCP option 97 GUID, and keeps DHCP option-54 server identity separate from the BOOTP `siaddr` next/TFTP server.<br>• DHCPREQUEST identifies the selected DHCP server;<br>• ACK/NAK from another server and ACK for another offered address are ignored.<br>• `siaddr` selects TFTP; option 66 accepts a dotted-decimal address or validated DNS host name, using the first unicast option-6 resolver with ARP/gateway routing, retransmission, rejection, timeout, QMP observability, and exact-deadline migration.<br>• The selected DHCP server remains the fallback when option 66 is absent.<br>• Option 67 is validated across primary/overloaded regions, repeated values must agree, and it is intentionally ignored so the Pi 4/CM4 multi-file boot still begins with `config.txt` rather than a generic PXE image.<br>• RFC 2132 option 52 safely parses `file`/`sname` overload regions and rejects truncated, nested, invalid-length, or conflicting scalar options; string options accept standards-compatible trailing NULs while rejecting embedded NULs.<br>• A zero-`yiaddr` proxy-DHCP offer may replace the TFTP server during DISCOVER or REQUEST without changing the lease/server selection.<br>• A valid NAK restarts discovery.<br>• EEPROM `TFTP_IP` overrides the DHCP-derived TFTP server, or a complete `TFTP_IP`/`CLIENT_IP`/`SUBNET` tuple skips DHCP.<br>• DHCP option 1/3 or static `SUBNET`/`GATEWAY` routing ARPs the correct next hop while retaining the TFTP destination IP.<br>• EEPROM `MAC_ADDRESS` or `MAC_ADDRESS_OTP` selects one strict unicast identity shared by GENET frames, DHCP option 97, the firmware mailbox, `TFTP_PREFIX=2`, and final-DT `local-mac-address`; reset and migration retain its source.<br>• EEPROM `TFTP_PREFIX` modes derive the lower-case OTP serial directory, exact bounded custom string, or hyphenated GENET MAC directory and apply it to every wire RRQ without corrupting logical config/include paths; failure of both prefixed modern and legacy start files clears only that prefix and retries at the root.<br>• It issues ordered TFTP RRQs for config plus response-discovered recursive includes, start/fixup, kernel, DTB, optional cmdline/initramfs, overlay map, and overlays.<br>• RRQs reproduce the Pi-observed `tsize=0`, `blksize=1024` sequence; strict OACK/ACK0 negotiation selects 8--1024-byte blocks and validates reported size, while direct DATA and one ERROR-8 retry support classic 512-byte servers.<br>• It operates without `network-boot-drive`; that optional corpus only adds exact size/SHA-256 validation.<br>• The client locks each server transfer ID, accepts ordered DATA with 16-bit block rollover, ACKs blocks and duplicates, applies per-artifact bounds, retries lost RRQs/OACK ACK0/mid-file ACKs inside the fixed file deadline, interprets file-not-found ERROR for optional probes, legacy start/fixup selection, device-prefix and `os_prefix` fallback, immediately applies retry/fallthrough for other server errors, responds to wrong transfer IDs, and dallies after the final DATA to recover a lost final ACK.<br>• Received buffers directly drive configuration parsing, overlay application, and ARM handoff.<br>• VMState v57 migrates the effective MAC identity plus DHCP/proxy/TFTP/DNS server identities, host name, query and retry state, static identity and routing, both timers, TFTP override/negotiation state, dynamic manifest, device and OS prefix decisions, packet backoff, final transfer identity, and exact remaining retry/dally delay.<br>• Frames update GENET MIB counters and descriptor DMA regains exclusive receive ownership afterward.<br>• Shared GENET packet-loss controls inject bounded TX/RX occurrence ranges, migrate progress, and are exposed by the production-gate report; qtests prove dropped DHCP TX retry across migration and selected DHCP RX loss.<br>• Explicit `network-boot-wire=off` retains only a synthetic corpus-only compatibility path.<br>• A rootless real dnsmasq/TAP/TFTP run using the pinned release image's unchanged boot tree passes without setting the wire property.<br>• The sealed real-server gate can atomically capture bounded private-TAP Ethernet PCAP with hash/count/size evidence and optionally fail closed by running physical-reference cadence comparison in the same invocation.<br>• A hash-pinned classic-PCAP cadence oracle normalizes DHCP/ARP/DNS/stateful-TFTP semantics and compares every adjacent packet interval with explicit absolute/relative tolerances.<br>• The opt-in production gate requires a sealed exact-file manifest, runs QEMU against real TAP-bound dnsmasq DHCP/TFTP in a rootless namespace, reaches ARM handoff, matches the actual consumed start/fixup/kernel/DTB/cmdline/initramfs QMP filenames and hashes, and proves its configured EEPROM, complete TFTP tree, and manifest are unchanged.<br>• The functional wrapper additionally rejects any corpus whose start/fixup, kernel, DTB, or initramfs hashes differ from the pinned official fixtures. | Maintain the sealed exact-corpus DHCP/DNS/ARP/TFTP, retry, prefix, manifest, PCAP, migration, and ARM-handoff gates. Release scheduling and physical cadence comparison remain release maintenance and Pass 2. | M9 |
| PER-003-GATE | Exact network release evidence | 🧪 HIL pending | • The official-corpus functional wrapper requires the same sealed configured EEPROM `.bin` and TFTP binaries used by hardware, pins the official start/fixup/kernel/DTB/initramfs hashes, always captures QEMU's private TAP, and independently verifies PCAP digest/count/size/link-type evidence.<br>• Paired physical-PCAP/MAC environment inputs run semantic and timing cadence comparison in that same fail-closed test. | Pass 2 executes it with a pinned physical Pi capture and retains the signed manifest, PCAPs, and reports. | M9 |
| PER-004 | HTTP boot | ✅ Full | • BOOT_ORDER mode 7 validates `HTTP_HOST`, `HTTP_PORT`, `HTTP_PATH`, and `HTTP_CACERT_HASH`; invalid hostnames are ignored exactly as documented and enter the default-host policy instead of invalidating EEPROM.<br>• The path reuses the GENET DHCP/DNS/ARP client, opens real checksummed TCP connections, sends bounded HTTP/1.0 or 1.1 requests for `boot.sig` then `boot.img`, requires status 200 plus one bounded `Content-Length`, rejects transfer encoding and malformed/oversized responses, and passes the unchanged bodies through customer RSA verification and the normal memory-FAT ARM handoff.<br>• `NET_INSTALL_ENABLED` and `NET_INSTALL_AT_POWER_ON` parse as strict booleans; a boot-time `net-install-requested` machine input models the held physical/UI request, invokes mode 7 once only when enabled, and resumes the byte-unchanged configured `BOOT_ORDER` from its first nibble after failure.<br>• Qtests prove that disabled mode blocks the request and that `AT_POWER_ON` alone never silently selects it.<br>• A bounded 64 KiB sparse TCP receive window retains multiple noncontiguous future segments, preserves first-arrival bytes across overlaps, cumulatively drains each contiguous range when its gap arrives, and migrates its data/validity map plus FIN sequence.<br>• A FIN after the exact declared body is ACKed, premature FIN fails immediately, each completed response is closed by the client, and redirects are rejected explicitly.<br>• The default-host path resolves `fw-download-alias1.raspberrypi.com`, accepts DNS replies from a router/slirp gateway MAC, selects port 443, creates peer-verifying credentials in memory from the embedded Raspberry Pi intermediate CA without inheriting the host trust store or requiring an operator object, sends the official hostname as TLS SNI, and runs the session inside the modeled GENET/TCP stream.<br>• An optional peer-verifying X.509 object remains a test/lab override.<br>• TLS segments encrypted records, feeds decrypted application bytes into the same bounded HTTP parser, sends close-notify, and verifies the downloaded pair with the official embedded Raspberry Pi network-install RSA key.<br>• A bounded unacknowledged encrypted-flight buffer applies cumulative and stale-ACK semantics and replays the exact ciphertext at the original TCP sequence; one qtest proves that the no-override built-in-CA path emits a ClientHello, and another drops the first ClientHello and proves byte-identical replay after 500 ms.<br>• It then completes an in-process X.509 server handshake for the official hostname, decrypts/asserts the exact first `GET /net_install/boot.sig` and Host header while a physical request overrides unchanged `BOOT_ORDER=0xf21` bytes, returns an encrypted HTTP 200 body, and proves TLS close-notify, TCP FIN, and the next `boot.img` connection.<br>• The independent default-host gate validates the official image/signature/key/CA/hostname corpus because the official signing private key is not available for a synthetic positive fixture.<br>• A second opt-in functional gate boots the unchanged pinned `pieeprom-2026-05-17.bin` through QEMU user networking and the live official HTTPS endpoint with no TLS override, verifies the 32,505,856-byte signed `boot.img`, checks its SHA-256 plus the exact `start4.elf`, `fixup4.dat`, `Image.gz`, DTB, and initramfs hashes, and reaches ARM handoff.<br>• Read-only QOM properties expose exact outer image/signature sizes and SHA-256 values.<br>• Custom-CA HTTPS remains rejected because `HTTP_CACERT_HASH` is BCM2712-only.<br>• VMState v28 preserves pre-session default-host/TLS and network-install policy; migration of an established TLS session is rejected instead of serializing traffic keys.<br>• Existing socket qtests cover loss, sparse reordering, overlap, FIN, redirects/errors, DNS, plaintext lab transport, invalid-host reset/migration on both platforms, and an allowed ARP refresh interleaved with post-migration TCP acknowledgements. | Maintain DHCP/DNS/ARP/TCP/TLS, peer verification, signed-image, retry/reordering, policy, migration-boundary rejection, live official endpoint, and exact ARM-handoff gates. SACK/error breadth and installer userspace are regression expansion; cadence and release evidence remain Pass 2/maintenance. | M9 |
| PER-005 | NVMe boot | ✅ Full | • `nvme-drive=ID` attaches QEMU's NVMe endpoint behind the BCM2711 link and BOOT_ORDER mode 6 loads unchanged raw FAT bytes from that same backend through the standard firmware handoff.<br>• Qtest proves unsigned and signed ARM handoff, signed-image tamper rejection, endpoint discovery, distinct absent, corrupt, and injected-read-error fallback while the endpoint remains enumerated, and a source/destination migration with the same namespace.<br>• Controller-level tests configure real admin/IO submission and completion queues through the BCM2711 PCIe aperture.<br>• Permanent plus one-shot read faults return NVMe `Unrecovered Read Error`, with the one-shot consumed by exactly one READ before success.<br>• Permanent WRITE faults return `Write Fault` twice and leave the host sector byte-identical; a one-shot fault leaves the failed command non-durable, then the following successful WRITE persists its exact 512-byte payload.<br>• A real dirty WRITE followed by one-shot FLUSH fault returns `Write Fault` and the next FLUSH succeeds.<br>• COMPARE returns `Compare Failure | DNR` for mismatched 512-byte data, succeeds for matching data, and leaves the namespace unchanged.<br>• A production Linux gate enumerates `1b36:0010`, binds the `nvme` driver, verifies namespace capacity, reads a sector whose SHA-256 must match the host backend, writes and fsyncs a deterministic final sector, re-reads its hash in the guest, verifies the exact bytes from the host, performs a PCI function reset, and re-enumerates the namespace.<br>• On Pi 4B the explicit NVMe endpoint replaces the board's single-link automatic VL805;<br>• CM4 exposes the downstream slot directly. | Maintain BCM2711 PCIe discovery, admin/I/O queues, MSI, exact-image boot, read/write/flush/compare faults, migration, production Linux I/O, reset, and fallback gates. Additional admin/hotplug/status breadth is regression expansion; timing and hardware conformance remain Pass 2. | M9 |
| PER-006 | RNG200 compatibility | ✅ Full | • BCM2711 instantiates a stateful 16-word RNG200 FIFO at `0xfe104000`, counts the 32-bit warm-up plus generated bits, implements writable total-bit/FIFO thresholds, W1C status/enable registers, and drives native GIC SPI 125.<br>• Normal reads refill while an explicit no-refill control proves exact depletion and empty reads.<br>• Persistent NIST-failure and master-lockout controls exercise the upstream Linux driver's reset path; a nonzero deterministic xorshift seed is explicitly test-only while the default uses QEMU guest randomness.<br>• Device reset clears the complete register/FIFO state.<br>• VMState v2 preserves unread FIFO bytes, thresholds, status/enable, total count, and deterministic-generator progress.<br>• Qtests prove exact data/counts, IRQ assertion/clearing and GIC wiring, both failure classes, reset, refill/depletion, and source-to-destination migration.<br>• The unchanged official kernel registers `iproc-rng200` as `/dev/hwrng` and initializes the CRNG. | Maintain RNG200 register/FIFO/IRQ, failure/reset, migration, production randomness, and Linux CRNG gates. Deterministic mode remains test-only; calibrated timing, entropy certification, and silicon conformance remain Pass 2. | M8 |
| PER-007 | BCM2711 thermal sensor compatibility | ✅ Full | • A dedicated AVS monitor variant maps the production syscon window at `0xfd5d2000`, returns the public Linux contract's 10-bit temperature code plus bits 10/16 valid, retains the unchanged `brcm,bcm2711-thermal` DT child, and migrates temperature/valid state.<br>• The production DT's `-487/410040` coefficients convert default code 791 to 24,823 m°C.<br>• Both Pi machine types expose construction- and runtime-writable `thermal-temperature-millicelsius` and `thermal-sensor-valid` controls instead of requiring a device-global override.<br>• Firmware `GET_TEMPERATURE` consumes that same live sample, while `GET_MAX_TEMPERATURE` reports 85,000 m°C; because the mailbox ABI has no validity bit, invalid state remains explicit in AVS/QOM while the configured numeric sample stays observable.<br>• Qtests cover mapping, default/custom conversion, runtime invalid-sensor injection, ignored writes, reset, retained final DT, and two live migrations against deliberately opposite destination defaults.<br>• Both a valid 80,000 m°C sample and invalid-sensor state survive migration and reset exactly, and machine QOM reports the migrated live value.<br>• Unchanged Pi 4B SD/USB and CM4 eMMC boots all read `24823` from Linux thermal sysfs. | Maintain AVS register, mailbox, validity/fault, QOM, reset, migration, DT, and production Linux thermal gates; workload dynamics and physical calibration/trips remain Pass 2. | M8 |
| PER-008 | Framebuffer/mailbox display | ✅ Full | • The mailbox implements validated framebuffer geometry, depth/order/alpha, palette, viewport, allocation/release, blanking, rendered overscan, all eight transforms, a programmable guest-RAM ARGB cursor, two-display selection/power, EDID queries, per-port FKMS timing, and a synchronous refresh-driven `SET_VSYNC` wait.<br>• Atomic Set-before-Get, Test non-mutation, duplicate rejection, short-buffer clipping, reset, and migration contracts are covered.<br>• HDMI0/HDMI1 expose production core `HOTPLUG` state plus connected/removed edges through the modeled AON L2 controller and GIC SPI 96.<br>• Their production HDMI-I2C BSC/auto-I2C windows perform Linux-compatible packed DDC 0x30/0x50 transfers, gated by live connector presence.<br>• HDMI 2.0 connectors advertising SCDC expose the standard 0x54 version, TMDS ratio, scrambling, read-request, and status protocol; other connectors NAK it.<br>• VMState retains HPD, interrupt, DDC, and SCDC transaction/negotiation state.<br>• Pi 4B/CM4 qtests prove rendered output, timing, vblank waits, both DDC ports including 384-byte segment addressing, SCDC negotiation, HPD connect/disconnect interrupt routing, destination-input disagreement, migration, and reset. | Maintain mailbox framebuffer, transforms/cursor, multi-display, EDID/DDC/SCDC, HPD/AON IRQ, timing/vsync, render, reset, and migration gates. HVS/scanline/pipeline and userspace-display breadth are optional expansion; opaque artwork and physical display/DDC conformance remain Pass 2. | M8 |
| PER-009 | Wi-Fi and Bluetooth | ✅ Full | • A new `cyw43455-sdio` device provides the deterministic transport foundation: CMD5/3/7/52/53, Broadcom 02d0:a9bf CCCR/FBR/CIS enumeration, function enable/ready and block sizing, function-1 backplane/window access, the exact `0x15294345` BCM4345/revision-9/AXI chip signature, the real `0x198000..0x25ffff` 800 KiB TCM aperture, function-2 traffic loopback, reset, byte/command telemetry, command-timeout injection, and active migration.<br>• ChipCommon exposes a driver-consumable DMP EROM containing ChipCommon, SDIO, D11, and ARMCR4 cores; their 4 KiB slave/wrapper apertures provide live migratable AI IOCTL/reset state, and ARMCR4 reports one 800 KiB TCM bank.<br>• Independent qtests parse every EROM descriptor, validate identities/revisions/bases/wrappers, and migrate active wrapper state.<br>• An opt-in fail-closed test accepts an unchanged production `.bin` plus its expected SHA-256, downloads all 643651 tested bytes through CMD53 into TCM, and reads them back byte-exactly; no emulator-specific conversion is used.<br>• Function-1 streaming now accepts brcmfmac's maximum 511-block/32,704-byte DMA request instead of inheriting the bounded packet buffer;<br>• BCM2835 DMA DREQ 11 is connected to the SDHCI data-ready boundary.<br>• The ARMCR4 start boundary implements the production reset-vector alias, validates brcmfmac's words/complement NVRAM trailer, publishes the protocol-v3 `sdpcm_shared` structure that executed firmware normally creates, rejects incomplete activation deterministically, exposes start/shared/NVRAM telemetry, invalidates modified images, and migrates active state.<br>• The unchanged production driver cleanly enumerates the SDIO card, downloads 497,628 firmware/NVRAM bytes through the modeled DMA path, and reaches `firmware-started=true`.<br>• The SDIO-core mailbox publishes the normal protocol-v4 firmware-ready event; its live interrupt status/mask and acknowledgement registers drive CCCR pending, DAT1, and SDHCI card-interrupt state through a generic SD-bus SDIO IRQ signal, including active migration and level sampling when the controller enables card interrupts after the device has asserted DAT1.<br>• Function 2 validates production 12-byte and txglom hardware-extension SDPCM headers, normalizes firmware responses to the receive format, pairs BCDC control responses by request ID, implements deterministic ioctl-version, firmware-version, band-list, chanspec, MAC, capability, SET, and unknown-query behavior, drives frame-indication interrupts, bounds its RX queue, rejects malformed frames, and migrates an unread response.<br>• Its data channel strips and generates the normal four-byte BCDC header and exchanges exact Ethernet payloads with a configurable QEMU NIC backend; a socket-netdev qtest proves 32-packet bidirectional pressure with ordered lossless backpressure drain.<br>• Opt-in onboard topology instantiates the chip on mmcnr for Pi 4B and CM4;<br>• Pi 4B moves its boot SD card to EMMC2 while CM4 retains eMMC there, and a final-DT qtest proves Wi-Fi remains enabled while Bluetooth stays disabled.<br>• The `wireless-netdev=ID` machine property attaches that onboard path to a standard QEMU backend.<br>• With the exact release firmware and unchanged production kernel/initramfs, `brcmfmac-wcc`, `brcmfmac`, `brcmutil`, `cfg80211`, and `rfkill` load, both SDIO functions bind to `brcmfmac`, firmware reports its modeled version, and Linux registers `wlan0`.<br>• A firmware SET_SSID success event after interface configuration supplies the RF-excluded virtual link boundary; unchanged `brcmfmac` then reports `UP,LOWER_UP`, completes DHCP DISCOVER/OFFER/REQUEST/ACK through QEMU user networking, obtains `10.0.2.15`, and pings `10.0.2.2` with zero loss.<br>• The production Bluetooth DT child is enabled with the wireless model; unchanged btbcm and hci_uart enumerate hci0 through the modeled PL011 H4 controller.<br>• The hash-pinned release gate passes.<br>• RF propagation/certification remains HIL-only. | Maintain the hash-pinned Wi-Fi/Bluetooth software release gate and focused reset, migration, packet, HCI, ACL, and fault regressions. Association, coexistence, RF, antenna, regulatory, and physical power validation remain Pass 2. | M10 |
| PER-010 | RF behavior | 🧪 HIL pending | • RF propagation cannot be represented by the board model. | Keep RF certification and coexistence tests on hardware. | HIL |

## 8. Power, reset, security, and fault behavior

| ID | Capability | Status | Current implementation / evidence | Required before Full | Milestone |
|---|---|---:|---|---|---|
| SYS-001 | QEMU machine reset | ✅ Full | • QEMU can reset modeled devices and restart direct boot. | This covers generic machine reset only. | Baseline |
| SYS-002 | Pi reset-cause registers/state | ✅ Full | • `PM_RSTS` survives warm reset; behavioral ROM reports raw status, decoded power-on/watchdog/software cause, and alternating-bit boot partition.<br>• The BCM watchdog uses its 65,536 Hz countdown instead of resetting on the arm write; start, cancellation, expiry, reset cause, and Linux systemd watchdog use pass.<br>• The EEPROM boot watchdog supports full 32-bit second deadlines without truncating them to the PM watchdog counter, then triggers the real PM reset path at expiry with the configured six-bit partition.<br>• Exact-deadline, handoff-cancel, reset-cause, partition-filter, and migration qtests pass.<br>• VMState v3 retains v1/v2 compatibility for the PM device while machine VMState v52 migrates the firmware-owned deadline relative to the destination clock. | Maintain power-on/watchdog/software cause, partition, deadline, cancel, reset, migration, and production Linux watchdog gates. Additional debug/quick/halt variants are regression expansion; calibrated timing and hardware traces remain Pass 2. | M3 |
| SYS-003 | Host-controlled power cycle | ✅ Full | • The exact gate drives QMP/process power phases and asserts a persistent fail-closed lifecycle plus sibling OS advisory lock across QEMU, Raw Gadget, configfs, fault recovery, and post-flash boot.<br>• QEMU holds the lock for its process lifetime;<br>• Raw Gadget publishes `rpiboot-active` and holds the lock across both enumerations; the foreground configfs owner holds it across the complete official Imager write, verify, and flush interval.<br>• The single supervisor executes and verifies every handoff.<br>• QEMU releases `rpiboot-host-ready`, accepts only `boot-ready`/`qemu-stopped` eMMC, marks `qemu-owned`, and publishes `qemu-stopped`.<br>• Abrupt QEMU death leaves a stale token; explicit `provision-recover-stale=on` recovers only unlocked `qemu-owned`, never failed/active flash states.<br>• A standard gate proves default rejection does not mutate the token, opt-in still rejects `flash-failed`, successful recovery reaches userspace, and the new owner is observable.<br>• Exit notification releases only states published by that process.<br>• Raw Gadget recovery separately changes only unlocked `rpiboot-active` to `rpiboot-failed`; a privileged gate SIGKILLs the real helper at an exact byte boundary in each enumeration, proves blocked retry and recovery, then completes a clean official transfer.<br>• The supervisor performs the same recovery after child termination.<br>• Configfs `recover-stale` wins the owner lock, cleans an orphaned or already-removed gadget, and retains `flash-failed`; unit/self-test gates cover live owners, teardown failure, wrong states, and crash windows. | Maintain fail-closed process/QMP ownership, lock, stale recovery, exact handoff, crash, and post-flash boot gates; relay correlation, setup/hold timing, extended campaigns, and hardware conformance remain Pass 2. | M3/M6 |
| SYS-004 | Host-controlled `nRPIBOOT` | ✅ Full | • Machine/QOM input is sampled at behavioral reset and gated by the Pi 4B OTP selector.<br>• On CM4, an explicitly driven active-low GPIO40/EMMC-DISABLE input from the bidirectional socket/HIL boundary takes precedence, persists across reset, and reports its sampled source/value; high impedance safely restores property control. | Maintain virtualization regression. Pass 2 owns power-removal/setup/hold and CM4 sampling comparison through GPIO-015, USB-012, TEST-004, TEST-005, and TEST-009. | M3/M6 |
| SYS-005 | Host-controlled EEPROM write protect | ✅ Full | • Machine/QOM separately controls and observes active-high logical `eeprom-nwp`, its transaction sample/source, and persistent status-register block protection.<br>• Low nWP alone does not protect array writes; it prevents recovery `config.txt` from changing the block-protect status.<br>• Status protection blocks erase/program independent of pin level.<br>• An optional exact 512-byte `eeprom-status-drive` makes the register authoritative across process lifetimes and durably records recovery or runtime transitions.<br>• A dedicated input-only `EEPROM_NWP` signal crosses the existing libgpiod/USB QGPIO bridge, takes precedence while driven, returns to the property on `Z`/disconnect, and migrates.<br>• Runtime property changes, reset retry, permanent recovery, invalid configuration, migration against opposite destination defaults, shutdown/relaunch, durable clearing, and real-socket low/high decisions are qtested. | Maintain virtualization regression. Pass 2 owns isolated CM4 pin/TP5 setup/hold/electrical behavior through GPIO-015, TEST-005, and TEST-009. | M3 |
| SYS-006 | EEPROM update power failure | ✅ Full | • Deterministic erase, program, verify, recovery-file rename, reboot, stuck-cell, and real backend I/O failures preserve the exact durable EEPROM/FAT boundary.<br>• Exact byte/page assertions cover erase/program/verify, a forced read-back mismatch, 0-to-1 NOR rejection, failed first erase, failed read-before-program after full erase, clearing faults and resetting, and the post-rename reboot boundary.<br>• The timer-driven path proves no mutation before the first deadline, one durable 4 KiB erase after it, reset retention, retry, mid-operation migration, exact 512-byte program cutoff, and a fully timed successful update. | Maintain virtualization regression. Pass 2 owns trace-calibrated real power cuts through SYS-009, TEST-004, TEST-005, and TEST-009. | M3 |
| SYS-007 | eMMC flash power failure | ✅ Full | • The real Raw Gadget link disconnects after exactly 4,096 bytes while official `rpiboot` sends `bootcode4.bin`; the partial transfer persists and a clean retry succeeds.<br>• Official Imager rejects full-device failure, enumerated-medium removal, live USB disconnect, and failed SYNCHRONIZE CACHE.<br>• Exact FUA WRITE and cache-sync CDBs fail selectively through the real host stack.<br>• BOT phase and no-CSW timeout faults force Linux USB resets.<br>• Active READ(16) and WRITE(16) commands accept configurable 1-to-131,072-byte reset cutoffs.<br>• The compiled self-test proves an exact 127-byte durable prefix at a nonzero backend offset.<br>• The privileged four-cycle gate selects distinct 4 KiB regions and passes a real 128-byte WRITE boundary with exact durable/untouched media and recovery.<br>• The target receives a complete USB packet while syncing only the smaller configured prefix.<br>• Eight back-to-back BOT class resets also recover.<br>• A real host `fsync` separately triggers deterministic medium loss immediately after one completed flush.<br>• Allocated partial media persists, Imager fails, and a fresh 4 GiB retry writes, verifies, and boots the same release.<br>• Inside QEMU, bounded SDHCI data-timeout and data-CRC injection stops PIO/SDMA/ADMA at a configured aligned byte boundary;<br>• Linux observes the architectural error and successfully reinitializes the card.<br>• A direct EMMC2 test migrates an active 320-byte PIO write, resets it without media mutation, then proves a completed uncached 512-byte sector is durable across reset.<br>• A second test migrates an acknowledged dirty card-cache sector, proves guest read-after-write while the backend remains old, loses it on the configured power-cut reset, persists it through MMC `FLUSH_CACHE`, and verifies exact two-sector pressure writeback while the remaining volatile sector is lost.<br>• Timed FLUSH_CACHE tests prove no pre-deadline mutation, one durable sector per deadline, active timer migration, and every power-cut offset across a four-sector transaction: the exact completed prefix survives and only the dirty suffix is lost.<br>• A timer-callback blkdebug flush failure preserves all dirty sectors, stops progress with standard R1 `ERROR`, and an identical retry durably commits both sectors and clears the error.<br>• Timed card-program tests exercise every power-cut offset across both normal uncached and Auto CMD23 reliable four-sector CMD25 transfers; only the completed prefix survives reset.<br>• An active reliable queue migrates after one durable sector, resumes for exactly one more deadline, then loses only its final unfinished sector on reset.<br>• A timer-callback backend failure retains both pending reliable sectors, reports R1 `ERROR`, and retries them exactly.<br>• Timed CMD38 tests cover every cutoff in a four-group erase, proving exact completed-group persistence, unfinished-tail retention, cache invalidation, active migration, and backend-error retry without resurrecting pre-erase cached data.<br>• A 64-cycle mixed campaign rotates cache flush, ordinary direct, and reliable writes through all five four-sector cut positions, resets at the next deadline minus one microsecond, crosses seven active live migrations, and verifies a distinct exact durable-prefix/untouched-suffix media window after every cycle.<br>• A third transfer test uses the real SDHCI Auto CMD23 path, leaves an unrelated normal sector volatile, durably commits one reliable sector, migrates 320 bytes into the next reliable sector, completes it on the destination, and proves reset loses only the normal cached sector.<br>• A real blkdebug `flush_to_disk` failure makes the first reliable write report standard R1 `ERROR` without committing the unrelated cache entry; the bounded retry clears the error, flushes successfully, and survives cache-loss reset. | Maintain virtualization regression. Pass 2 owns calibrated physical power-cycle campaigns through SYS-009, USB-012, TEST-005, and TEST-009. | M6 |
| SYS-008 | Secure-boot OTP irreversibility | ✅ Full | • Persistent OTP rows use one-way OR programming;<br>• ROM detects row-17 secure mode and requires rows 47-54 to match the exact EEPROM customer public-key SHA-256.<br>• Recovery and RPIBOOT accept `program_pubkey=1` only with persistent OTP, an independently pinned valid `bootsys`, its complete rooted LZ4 dependency chain, and a valid customer-signed EEPROM; they always program production `0x81` secure/revocation flags, overriding stale `revoke_devkey=0`, and reject key replacement.<br>• Row-55 revocation then blocks old key-index-zero second stages while current key-index-one images remain eligible.<br>• Eleven row-boundary power cutoffs preserve the exact already-burned prefix.<br>• A cutoff before the first write retries cleanly; later cutoffs remain irreversible and reset into boot-mode-copy, missing-key, or hash-mismatch failures.<br>• Completed-row telemetry migrates. | Maintain irreversible OTP rows, customer-key binding, rooted dependency validation, development-key revocation, exact power-cut prefixes, reset, migration, and release gates. Electrical fuse timing, JTAG lock, broader silicon oracles, and silicon-root conformance remain Pass 2. | M7 |
| SYS-009 | Brownout and marginal supply behavior | 🧪 HIL pending | • `firmware-throttled-current` injects the three Pi 4/CM4 software-visible current flags and latches matching history bits 16--18 for `GET_THROTTLED`;<br>• QOM and mailbox values survive reset and migration on both machines.<br>• It deliberately does not simulate a voltage waveform or autonomously derive throttling. | Use a programmable PSU and real Pi/CM4 fixture to validate voltage thresholds, rail/clock/thermal coupling, timing, storage effects, and physical recovery. | HIL |

## 9. Conformance and release gates

Current full behavioral regression total: **260/260 qtests passing**, including
the BCDC control, packet, onboard-topology, final-DT, maximum function-1
streaming, and `sdpcm_shared` coverage. This
dashboard value supersedes older embedded prose counts in the detailed and
historical evidence.

| ID | Capability | Status | Current implementation / evidence | Required before Full | Milestone |
|---|---|---:|---|---|---|
| TEST-001 | Branch platform-tool unit tests | ✅ Full | • Two hundred thirty default Python tests (five expected opt-in skips), opt-in real-host integrations, two hundred sixty behavioral qtests, the compiled QGPIO firmware-core and RPIBOOT lifecycle self-tests, and both compiled raw-BOT self-tests pass.<br>• Coverage spans safe physical flashing, persistent EEPROM/OTP/eMMC and their faults, timer-driven page-accurate NOR programming and power cuts, card-internal eMMC timed cache loss/flush/pressure/reliable-write/error/migration boundaries, timed uncached/reliable card programming and erase groups, a 64-cycle mixed reset/migration durability campaign, stuck-cell retry, status-register/nWP write protection through properties, persistence, host GPIO/USB bridge SD-overcurrent recovery, revision-specific USB power-off/reset/migration timing, signed recovery-envelope/trust rejection plus fragmented and malformed FAT12/16/32 recovery, strict same-media official SD recovery manifest/flash/reset/handoff proof, complete ARM64 Image headers and sub-4-KiB offset placement, ARM64/ARM32 firmware handoff across every Pi 4B/CM4 RAM SKU, raw MBR/EBR/GPT/FAT boot, SD/USB/NVMe/network selection and failure, exact EEPROM boot-watchdog deadline/cancellation/partition reset/migration, documented-enabled partition-walk default and explicit-disable policy, default-on fatal-error hard reset, rooted EEPROM dependencies and signing-key revocation, secure boot plus irreversible SD/RPIBOOT OTP provisioning and every row-level power cutoff, tryboot/A-B policy, migration-safe active SD removal and eMMC sector-boundary power cuts, migratable PCIe/xHCI/DWC2/GENET/RNG200/framebuffer/AUX-UART/AUX-SPI, CPRMAN, independent PWM0/PWM1 waveform/GPIO mux, shared-FIFO lock-step/starvation ownership, and DREQ-held BCM DMA state including stopped-clock TX, FIFO-period/edge, and partial-control-block recovery, host/device slave FIFO operation, and asynchronous host-PIO plus multi-endpoint device-PIO reset stress, GPIO/peripherals including 7/10-bit BSC addressing and migration, CYW43455 SDIO identity/TCM/migration/faults plus opt-in hash-pinned unchanged firmware download/readback, firmware identity including board model/revision/serial/MAC, rendered framebuffer viewport/blanking/transforms and retained display-control, multidisplay, EDID-derived FKMS timing, guest-visible HDMI DDC/SCDC/CEC and HPD/AON/GIC routing, and rendered programmable-cursor state, lossless PTY-backed USB-adapter command/edge races, adapter watchdog/safety core, reproducible-firmware manifest enforcement, strict HAT corpus preflight/differential reporting, RPIBOOT lifecycle and BOT recovery, complete pre-ARM TFTP/HTTP/TLS paths, exact EEPROM Imager-repository NVMEM handoff, display-gated Network Install selection, USB no-boot-files installer fallback, invalid HTTP-host default-policy behavior, HDMI diagnostics delay/fatal/handoff policy, real TFTP EEPROM success/rejection/non-mutation/partial-program/retry boundaries, and fail-closed Pi 4B/CM4 HIL orchestration.<br>• Seventeen executed functional gates include unchanged official ARM64 userspace, official EEPROM-enabled CM4 external-VL805 USB boot, stock AUX-SPI/spidev driver binding and transfer, plus Pi 4B SD and CM4 eMMC ARM32 four-core `armv7l` userspace; fifteen opt-in exact-release SD/USB/eMMC, live HTTPS, RPIBOOT/Imager, and supervisor gates remain explicitly classified. | Expand when new image, boot, or trace features land. | M0 |
| TEST-002 | Upstream `raspi4b` functional test | ✅ Full | • Both upstream direct-kernel subtests pass. | Keep as baseline; it does not prove firmware boot. | Baseline |
| TEST-003 | Versioned boot event trace schema | ✅ Full | • Version 1 JSON Lines contract is validated.<br>• QEMU reset/ROM/recovery/EEPROM/source, firmware artifact, overlay, ARM64 handoff, and host-observed kernel/userspace health events have a tested converter.<br>• Health transitions are monotonic, post-handoff only, migration-safe, reset-clean, and driven by real console markers in standard production gates. | Maintain the versioned trace schema and converter. Pass 2 supplies and compares physical traces through TEST-004, TEST-005, and TEST-009. | M1 |
| TEST-004 | Physical Pi 4B oracle fixture | 🧪 HIL pending | • A strict fixture-bundle builder derives a validated version-2 HIL plan and version-2 conformance case from explicit operator argv, one event contract, and the exact pinned artifacts without fabricating hardware evidence.<br>• The HIL plan pins board revision, exact EEPROM/firmware/media hashes, and one normalized `/dev/disk/by-id` or `/dev/disk/by-path` target, then enforces `power-off → flash-media → power-on → capture-uart → health-check` followed by mandatory power-off cleanup.<br>• Only the flash step receives the authorized target placeholder.<br>• It must emit a verified block-device attestation matching both that target and the media bytes; canonical path, inode, `rdev`, major/minor, sysfs/kernel identity, capacity, sector size, and safety state are validated and retained.<br>• Artifact/target drift, missing/wrong flash evidence, stale/invalid traces, reordered steps, timeouts, and cleanup failure are rejected.<br>• Mock coverage passes, but no attached Pi 4B capture exists yet. | Connect the relay/flasher/UART/health commands, run the plan on a pinned board, retain the resulting trace/report, and pass differential release comparison. | M1 |
| TEST-005 | Physical CM4 oracle fixture | 🧪 HIL pending | • The strict fixture-bundle builder derives the CM4 version-2 HIL plan and matching version-2 conformance case from explicit operator commands, the event contract, pinned artifacts, and an authorized stable eMMC gadget path.<br>• The plan enforces power-off, asserted `nRPIBOOT`, ROM power-on, unchanged `rpiboot`, exact eMMC flashing, powered release of `nRPIBOOT`, normal boot, UART capture, and health check.<br>• Power-off plus `nRPIBOOT` release always run as cleanup.<br>• Exact EEPROM/firmware/media paths must appear in verified command placeholders, and `flash-emmc` alone receives the authorized target and must emit the same verified opened-block-device identity contract as Pi 4B.<br>• The atomic report binds every command, target, flash attestation, and trace hash.<br>• Mock coverage passes, but no attached CM4/RPIBOOT/eMMC reference capture exists yet. | Connect the CM4 relay/GPIO/USB/UART/health fixture, capture eMMC and USB evidence before/after, retain the trace/report, and pass differential release comparison. | M6 |
| TEST-006 | Differential QEMU-vs-hardware runner | ✅ Full | • The strict comparator normalizes documented volatile fields and reports ordered semantic differences.<br>• A version-2 batch manifest pins each case's platform, board revision, exact artifact paths/SHA-256 values, an authorized stable flash target for HIL cases, and a non-empty exact ordered event contract.<br>• Matching but truncated captures fail: an incomplete hardware trace is invalid oracle evidence, while QEMU truncation or reordering is reported as behavioral drift.<br>• The manifest can execute QEMU producers or replay immutable captures.<br>• A hardware producer can name a HIL plan/report directly, making the fixed Pi 4B/CM4 hardware workflow, QEMU producer, and comparison one fail-closed invocation.<br>• The entire batch, duplicate IDs, and every plan/case platform, revision, trace, resolved artifact path, artifact set, and digest are checked before the first fixture command; malformed later cases therefore cannot partially execute an earlier hardware campaign.<br>• The orchestrator retains bounded argv-only steps, mandatory cleanup, fresh trace validation, opened-block-device flash evidence, and atomic reports.<br>• Resolved-path, hard-link, and symlink isolation prevents trace/report outputs or the QEMU trace from aliasing plans or artifacts before fixture access; the gate re-reads the retained report and independently revalidates its identity, success, media-bound flash attestation, trace SHA-256, and record count.<br>• The comparator removes stale commanded outputs, checks producer/platform/revision/header hashes against verified bytes, rejects unknown configuration, exposes any extra ignored keys for review, and emits a deterministic all-case JSON report with distinct drift versus invalid-capture exit states.<br>• The production gate now generates its QEMU PCAP internally and can invoke cadence comparison against a supplied physical capture without a separate capture workflow.<br>• A dedicated network-cadence comparator directly parses bounded Ethernet PCAP, normalizes DHCP/ARP/DNS/stateful-TFTP events, compares semantic order and inter-packet timing, and emits a deterministic hash-pinned report with distinct match/drift/invalid exits.<br>• Eighteen general conformance cases, eleven HIL-orchestrator cases, five fixture-bundle cases, and the network cases cover matching, drift, tampering, producer identity, stale replacement, direct capture, strict ordering, cleanup, flash evidence, integrated HIL execution, batch preflight, and invalid input. | Maintain comparator regression. Pass 2 supplies physical plans/traces and release execution through TEST-004, TEST-005, and TEST-009. | M1 |
| TEST-007 | Full Pi 4B SD boot without `-kernel` | ✅ Full | • The standard functional gate boots to `Boot successful.` from raw FAT using pinned, byte-unchanged official EEPROM/firmware/kernel/DT/overlay/initramfs artifacts; a second standard gate does so through official `serial0`/`ttyS0`.<br>• The unprivileged exact-release gate verifies the full 2,977,955,840-byte 2026-06-18 Raspberry Pi OS Lite raw image and SHA-256, copies it byte-for-byte to virtual SD, boots it through unchanged official EEPROM, reaches ARM handoff with the release's official `vc4-kms-v3d-pi4.dtbo`, emits the release kernel log through `serial0`/`ttyS0`, mounts the release root filesystem, starts the serial getty to the `raspberrypi login:` application boundary without firmware-GPIO probe failures, and observes first-boot userspace durably replace the MBR disk identifier while the VM remains alive.<br>• No path uses `-kernel`, `-dtb`, `-initrd`, `-append`, host-side boot-file extraction, or test-side media mutation. | Maintain the pinned exact-image gate; broader overlay/DT and physical differential conformance remain tracked by BOOT-014/015 and TEST-004/006/009. | M5 |
| TEST-008 | Full CM4 `rpiboot` flash and boot | ✅ Full | • A focused opt-in privileged gate starts from QEMU's nRPIBOOT release, SIGKILLs Raw Gadget after exactly 4,096 durable bytes in both ROM and second-stage `boot.img` transfers, proves `rpiboot-active` blocks unsafe retry, performs explicit failed-only recovery, and then completes and byte-compares a clean transfer of the pinned official corpus.<br>• The in-process proxy gate independently exposes 4,096 active bytes through QOM in each enumeration, SIGKILLs the packet-only bridge, requires rollback to zero, completes on the same QEMU, and verifies the locked lifecycle's wait/active/failed/retry/complete transitions plus post-shutdown persistence.<br>• A second focused gate issues four real host USB resets at exactly 4,096 bytes in each active transfer; the threaded proxy observes all eight resets, rejects stale completions, re-enumerates both stages, and finishes byte-identically without process replacement.<br>• A third focused gate crosses the unchanged host's real timeout boundaries for ROM-status and file-request control replies and for 4,096-byte-stalled bulk traffic in both enumerations, requires intentional failure, then completes exact clean retries on the same QEMU.<br>• The combined gate proves official `rpiboot` rejects a 4,096-byte disconnect and then completes; official Imager rejects undersized media, enumerated-medium removal, live disconnect, and failed cache sync.<br>• Exact CACHE SYNC and FUA WRITE failures retain USB.<br>• BOT phase-error and no-CSW timeout force real reset/reconfiguration.<br>• Four active READ commands retain their validated 512-byte reset boundary.<br>• The four active WRITE commands select 128 bytes and pass with failed old commands, recovered identity/INQUIRY/read, and exact durable/untouched regions across all cycles.<br>• Eight class resets run back-to-back.<br>• The independent libusb probe covers the complete thirteen-case matrix, five meaningfulness cases, 128 fixed-seed randomized commands, and two invalid wrappers while proving backend non-mutation and kernel recovery.<br>• The gate drains target diagnostics concurrently so logging cannot backpressure the USB command loop; failures identify the exact deterministic case/opcode/CDB/tag.<br>• Post-flush loss returns `EIO`.<br>• The gate then writes/verifies the exact 4 GiB 2026-06-18 Raspberry Pi OS payload, boots it through unchanged EEPROM and the CM4 DT, rewrites PARTUUID, expands partition 2 and ext4, and mounts root read/write.<br>• Its post-flash phase also requires `serial0` to reach `raspberrypi login:` without firmware-GPIO failures, matching TEST-010.<br>• All three focused RPIBOOT fault gates and the full combined privileged gate pass with the pinned corpus, including the 128-byte durable-WRITE boundary.<br>• The supervisor now uses the in-process DWC2/ROM path by default, requires the exact received `boot.img` to reach ARM64 handoff in memory while eMMC remains separate, keeps that same guest and DWC2 connection alive for exact `0a5c:0104` ACM+MSD exposure, proves matching guest ACM traffic before and after unchanged Imager write/verify/flush, and emits a hash-pinned report before rebooting the same eMMC backend. | Maintain the complete pinned combined gate. Extended USB fuzz/reset campaigns remain tracked by USB-013, other hosts by USB-009, and physical comparison by TEST-005/006/009. | M6 |
| TEST-009 | Real Pi/CM4 final release gate | 🧪 HIL pending | • The versioned release manifest can now invoke the strict Pi 4B/CM4 HIL workflow, retain its atomic hardware report, run the matching QEMU producer, and compare traces in one command after whole-batch preflight.<br>• This is the software release-gate boundary, but no attached relay/USB/UART/network/measurement fixture or passing physical campaign is present. | Connect and identify the physical fixtures, add electrical/PHY/RF/power measurements, retain Pi 4B/CM4 reports and traces, and make both cases mandatory in release CI. | HIL |
| TEST-010 | Exact CM4 release boot from eMMC | ✅ Full | • An unprivileged gate verifies the exact 2,977,955,840-byte 2026-06-18 Raspberry Pi OS Lite raw payload and SHA-256, preserves it byte-for-byte as the prefix of a 4 GiB backend, and boots through unchanged EEPROM without direct-loader arguments or host extraction.<br>• The backend is attached as a real MMC endpoint;<br>• Linux must identify `mmc0:0001 QEMU!!` as a 4.00 GiB high-speed MMC and the gate rejects stale data-ready interrupts or `mmcblk0` I/O errors.<br>• It also asserts eMMC selection, the official CM4 DT and VC4 overlay, no eMMC recovery execution, firmware-GPIO health, first-boot disk-ID replacement, partition-2 expansion to 7,323,648 sectors, ext4 growth to 915,456 blocks, and the unchanged `serial0` path reaching `raspberrypi login:` while QEMU remains alive. | Maintain the exact pinned gate; host flashing is covered separately by TEST-008. | M6 |
| TEST-011 | Exact Pi 4B release boot from USB-MSD | ✅ Full | • An unprivileged gate verifies the exact 2,977,955,840-byte 2026-06-18 Raspberry Pi OS Lite raw payload and SHA-256, copies it byte-for-byte to the USB backend, and boots it through unchanged production EEPROM default `BOOT_ORDER=0xf41`.<br>• Absent SD falls through to mode 4; behavioral firmware reads the release exclusively through guest-DMA VL805/xHCI BOT traffic, then Linux enumerates that same USB Mass Storage device, mounts its real root filesystem, reaches `raspberrypi login:` through `serial0`/`ttyS0`, and remains alive.<br>• The release's own first-boot code durably replaces the MBR disk identifier and expands partition 2 on the USB backend.<br>• No direct-loader argument, host extraction, or test-side media mutation substitutes for the production path. | Maintain the exact pinned gate; physical timing and differential USB conformance remain tracked by USB-003/004 and TEST-004/006/009. | M4 |

## Milestone order

| Milestone | Deliverable | Exit condition |
|---|---|---|
| M0 | Platform laboratory | Build passes; 260/260 Raspberry Pi qtests, 230 Python tests with five expected skips, and all 32 functional tests are accounted for. Exact-release and privileged host gates have retained evidence. |
| M1 | Boot contract and physical Pi 4B oracle | Image contents and ordered boot events can be compared. |
| M2 | Core SoC, SD, USB-host, and bidirectional GPIO | Linux-visible controller behavior matches the reference corpus. |
| M3 | ROM, OTP, `nRPIBOOT`, and persistent SPI EEPROM | Recovery and EEPROM update flows match Pi 4B behavior. |
| M4 | EEPROM second stage and BOOT_ORDER | SD and USB sources select, fail over, stop, and restart correctly. |
| M5 | Behavioral VideoCore firmware boundary | A release SD image boots without `-kernel` or host-supplied DTB. |
| M6 | CM4, USB gadget/RPIBOOT, and eMMC | Unmodified host tooling flashes virtual eMMC and the CM4 boots it. |
| M7 | Secure boot | Success and rejection behavior matches pinned hardware policy. |
| M8 | PCIe, GENET, USB3, PWM, and remaining peripherals | Disabled DT nodes are eliminated for implemented devices. |
| M9 | Network, HTTP, and NVMe boot | Remaining supported boot sources pass fallback tests. |
| M10 | Explicit platform exclusions and final conformance | Every row is Full, deliberately HIL-only, or explicitly out of scope. |

## Current status

- Pass 1 virtualization and its fresh release campaign are complete at
  **100.0%**.
- Exact Pi 4B SD/USB and CM4 `rpiboot`/Imager/eMMC flows boot unchanged
  production artifacts through first-boot userspace.
- Pi 4B and CM4 support coherent 1/2/4/8 GiB memory models through standard
  `-m`; storage, OTP identity, firmware DT, and ARM handoff stay aligned.
- Host USB and GPIO bridges are implemented and software-tested. They do not
  substitute for electrical, PHY, timing, or RF measurements.
- The sole unfinished campaign is Pass 2 HIL: **0 of 12 scored rows ready**.
  Its remaining work is listed near the top of this file.
