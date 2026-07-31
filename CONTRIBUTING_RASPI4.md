<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Contributing to the Raspberry Pi platform fork

Contributions to the Raspberry Pi 4B/CM4 platform are developed against
`raspi4/full-platform`. Changes intended for upstream QEMU must also follow
QEMU's current submission process.

## Change contract

- Preserve support for unchanged production `.bin`, `.elf`, `.dat`, `.img`,
  `.dtb`, `.dtbo`, and initramfs byte streams.
- Do not replace an earlier boot stage with a host-side shortcut in an
  end-to-end gate.
- Keep deterministic software-visible evidence separate from physical HIL
  evidence.
- Do not commit operating-system images, firmware downloads, credentials,
  private keys, device captures containing secrets, or build output.
- Add focused tests for new behavior and fault paths.
- Update `RASPI4_IMPLEMENTATION_MATRIX.md` when capability status or evidence
  changes.

## Commit requirements

- Create focused, dependency-ordered commits.
- Follow QEMU coding style and run `scripts/checkpatch.pl`.
- Add `Signed-off-by: Real Name <email>` to every commit in accordance with
  QEMU's Developer Certificate of Origin.
- Keep generated artifacts out of Git unless they are small, deterministic,
  versioned evidence reports verified by `evidence_integrity.py`.

## Required local checks

```sh
python3 -m unittest discover -v -s contrib/raspi4 -p 'test_*.py'
python3 contrib/raspi4/matrix_progress.py --check
python3 contrib/raspi4/matrix_progress.py --check-dashboard
python3 contrib/raspi4/publication_audit.py \
  --baseline 300438ffbb8d9430cac2fcc15cba6f482b2c0587
git diff --check
```

After configuring an AArch64 system build:

```sh
ninja -C build qemu-system-aarch64 tests/qtest/raspi4-boot-test
QTEST_QEMU_BINARY=./build/qemu-system-aarch64 \
  ./build/tests/qtest/raspi4-boot-test
```

Opt-in production and HIL gates require their explicitly documented,
hash-pinned artifacts or fixtures. A skipped opt-in gate is not passing
evidence.
