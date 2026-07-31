<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Raspberry Pi 4B/CM4 platform Pass 1

This release provides the first complete software-visible Pass 1 platform for
Raspberry Pi 4 Model B and Compute Module 4.

## Included

- Unchanged raw-image flashing and verification.
- Pi 4B SD, USB, NVMe, TFTP, and HTTP boot paths.
- Persistent EEPROM, OTP policy, recovery, fallback, and fault injection.
- CM4 `rpiboot`, host imaging, eMMC handoff, and 1/2/4/8 GiB models.
- Behavioral firmware configuration, overlays, generated DT, and ARM handoff.
- GPIO, serial, I²C, SPI, display, USB, storage, GENET, and wireless contracts.
- Host USB/GPIO bridge tooling and fail-closed HIL orchestration.
- Versioned conformance traces and hash-verified retained evidence.

## Evidence boundary

- Pass 1 software-visible implementation: **100%**.
- Pass 2 physical HIL readiness: **0 of 12**.
- Electrical, analogue, PHY, RF, timing, physical-fuse, and silicon
  root-of-trust conformance are not claimed by this release.

## Upstream baseline

- QEMU commit: `0345ef676befc1a180d2f63bceaf3fca1d07ee88`
- QEMU development version: `11.0.92`

## Verify downloaded assets

```sh
sha256sum --check SHA256SUMS
python3 contrib/raspi4/release_manifest.py verify \
  --manifest raspi4-release-manifest-v1.json \
  --artifact-dir .
gh attestation verify qemu-rpi4-*.tar.xz --repo perara/qemu
```

Production Raspberry Pi firmware and operating-system images are not
redistributed. Supply the exact artifacts locally and validate their pinned
SHA-256 values as documented in `contrib/raspi4/README.rst`.
