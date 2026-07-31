<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Raspberry Pi 4B/CM4 release policy

## Release contents

Every `rpi4-v*` release must be built from a clean, signed-off commit on
`raspi4/full-platform` and contain:

- a reproducible Linux x86_64 binary archive;
- SHA-256 checksums;
- an SPDX JSON software bill of materials;
- a versioned release manifest;
- the exact QEMU source commit and upstream baseline;
- the Pass 1 result and separate Pass 2 HIL readiness count;
- release notes listing known limits and opt-in gates.

The Git tag and manifest are the authoritative identity. GitHub's generated
source archives are convenience copies.

## Artifact policy

The repository and releases must not redistribute Raspberry Pi OS images,
downloaded Raspberry Pi firmware, signing keys, credentials, or proprietary
captures unless redistribution rights have been verified separately.
Production artifacts are referenced by source, version, SHA-256, and license;
users supply the bytes locally.

Build directories, raw SD/eMMC images, and temporary fixture state never enter
Git. Release assets are generated in CI from tracked source.

## Required gates

- All commits carry a DCO sign-off.
- `git diff --check` and QEMU checkpatch pass.
- The Python, matrix, publication, evidence-integrity, build, qtest, and
  functional-accounting jobs pass.
- Release assets verify against the versioned manifest.
- Pass 1 claims match `RASPI4_IMPLEMENTATION_MATRIX.md`.
- Pass 2 remains separately reported until physical evidence is retained and
  the HIL release gate is mandatory.

## Verification

```sh
sha256sum --check SHA256SUMS
python3 contrib/raspi4/release_manifest.py verify \
  --manifest raspi4-release-manifest-v1.json \
  --artifact-dir .
```
