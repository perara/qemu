#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Seal an exact Raspberry Pi EEPROM and TFTP tree for production gating."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sys

from network_boot_gate import (
    EEPROM_SIZE,
    GateError,
    MANIFEST_SCHEMA,
    hash_tree,
    sha256_file,
)


def build_manifest(eeprom: Path, tftp_root: Path, machine: str, memory: str,
                   expected_firmware: str,
                   eeprom_update: str | None = None,
                   eeprom_update_result: str = "success",
                   eeprom_update_fail_after: int | None = None,
                   ) -> dict[str, object]:
    if eeprom_update_result not in {
            "success", "invalid-signature", "write-protected",
            "program-failure"}:
        raise GateError("EEPROM update result is invalid")
    if ((eeprom_update_result == "program-failure") !=
            (eeprom_update_fail_after is not None)):
        raise GateError(
            "program-failure and EEPROM fail-after must be selected together")
    if (eeprom_update_fail_after is not None and
            not 0 <= eeprom_update_fail_after < EEPROM_SIZE):
        raise GateError("EEPROM fail-after must be below 512 KiB")
    eeprom = eeprom.resolve(strict=True)
    tftp_root = tftp_root.resolve(strict=True)
    if not eeprom.is_file() or eeprom.is_symlink():
        raise GateError(
            f"EEPROM is not a regular non-symlink file: {eeprom}"
        )
    firmware = tftp_root / expected_firmware
    if not firmware.is_file() or firmware.is_symlink():
        raise GateError(f"expected firmware is absent or unsafe: {firmware}")
    update_metadata = None
    if eeprom_update is None and eeprom_update_result != "success":
        raise GateError(
            "EEPROM update result requires an EEPROM update path")
    if eeprom_update is not None:
        parts = eeprom_update.split("/")
        if (machine != "raspi4b" or not eeprom_update or
                eeprom_update.startswith("/") or
                parts[-1] != "pieeprom.upd" or
                any(part in ("", ".", "..") for part in parts)):
            raise GateError("EEPROM update path is invalid")
        update = tftp_root / eeprom_update
        signature = update.with_name("pieeprom.sig")
        if update.is_symlink() or not update.is_file():
            raise GateError("EEPROM update is absent or unsafe")
        if update.stat().st_size != EEPROM_SIZE:
            raise GateError("EEPROM update must be exactly 512 KiB")
        if signature.is_symlink() or not signature.is_file():
            raise GateError("EEPROM update signature is absent or unsafe")
        update_hash = sha256_file(update)
        signature_bytes = signature.read_bytes()
        signature_valid = (
            len(signature_bytes) >= 64 and
            signature_bytes[:64].lower() ==
            update_hash.encode("ascii")
        )
        if eeprom_update_result == "invalid-signature":
            if signature_valid:
                raise GateError(
                    "invalid-signature result requires a mismatched signature"
                )
        elif not signature_valid:
            raise GateError(
                f"{eeprom_update_result} result requires a valid signature")
        update_metadata = {
            "path": eeprom_update,
            "sha256": update_hash,
            "size": EEPROM_SIZE,
            "expected_result": eeprom_update_result,
        }
        if eeprom_update_fail_after is not None:
            update_metadata["fail_after"] = eeprom_update_fail_after
    return {
        "schema": MANIFEST_SCHEMA,
        "machine": machine,
        "memory": memory,
        "expected_firmware": expected_firmware,
        "eeprom": {"sha256": sha256_file(eeprom)},
        "eeprom_update": update_metadata,
        "tftp_files": hash_tree(tftp_root),
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--eeprom", required=True, type=Path)
    parser.add_argument("--tftp-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--expected-firmware", default="start4.elf")
    parser.add_argument(
        "--eeprom-update",
        help="relative TFTP path to an explicitly expected pieeprom.upd",
    )
    parser.add_argument(
        "--eeprom-update-result",
        choices=(
            "success", "invalid-signature", "write-protected",
            "program-failure",
        ),
        default="success",
    )
    parser.add_argument("--eeprom-update-fail-after", type=int)
    parser.add_argument("--machine", choices=("raspi4b", "raspi-cm4"),
                        default="raspi4b")
    parser.add_argument("--memory", choices=("1G", "2G", "4G", "8G"),
                        default="2G")
    parser.add_argument("--force", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        tftp_root = args.tftp_root.resolve(strict=True)
        output = args.output.resolve()
        try:
            output.relative_to(tftp_root)
        except ValueError:
            pass
        else:
            raise GateError("manifest output must be outside the TFTP root")
        if output.exists() and not args.force:
            raise GateError(f"manifest already exists: {output}")
        manifest = build_manifest(
            args.eeprom, tftp_root, args.machine, args.memory,
            args.expected_firmware, args.eeprom_update,
            args.eeprom_update_result,
            args.eeprom_update_fail_after,
        )
        output.parent.mkdir(parents=True, exist_ok=True)
        temporary = output.with_name(output.name + ".tmp")
        temporary.write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        os.replace(temporary, output)
        print(json.dumps(manifest, sort_keys=True))
        return 0
    except (GateError, OSError) as error:
        print(f"network boot manifest: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
