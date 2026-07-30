#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Seal exact Raspberry Pi 4 EEPROM SD-recovery inputs."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sys

from network_boot_gate import EEPROM_SIZE, GateError, sha256_file


SCHEMA = "qemu-rpi-sd-recovery-manifest-v1"


def checked_file(path: Path, description: str,
                 size: int | None = None) -> Path:
    if path.is_symlink():
        raise GateError(f"{description} is not a safe regular file")
    path = path.resolve(strict=True)
    if not path.is_file():
        raise GateError(f"{description} is not a safe regular file")
    if size is not None and path.stat().st_size != size:
        raise GateError(f"{description} must be exactly {size} bytes")
    return path


def build_manifest(
        eeprom: Path, sd_image: Path, recovery: Path, update: Path,
        firmware: Path, fixup: Path, kernel: Path, memory: str
        ) -> dict[str, object]:
    eeprom = checked_file(eeprom, "EEPROM", EEPROM_SIZE)
    sd_image = checked_file(sd_image, "recovery SD image")
    recovery = checked_file(recovery, "recovery.bin")
    update = checked_file(update, "pieeprom.upd", EEPROM_SIZE)
    firmware = checked_file(firmware, "firmware")
    fixup = checked_file(fixup, "fixup")
    kernel = checked_file(kernel, "kernel")
    return {
        "schema": SCHEMA,
        "machine": "raspi4b",
        "memory": memory,
        "eeprom": {
            "sha256": sha256_file(eeprom),
            "size": EEPROM_SIZE,
        },
        "sd_image": {
            "sha256": sha256_file(sd_image),
            "size": sd_image.stat().st_size,
        },
        "artifacts": {
            "recovery.bin": sha256_file(recovery),
            "pieeprom.upd": sha256_file(update),
            "start4.elf": sha256_file(firmware),
            "fixup4.dat": sha256_file(fixup),
            "kernel8.img": sha256_file(kernel),
        },
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--eeprom", required=True, type=Path)
    parser.add_argument("--sd-image", required=True, type=Path)
    parser.add_argument("--recovery", required=True, type=Path)
    parser.add_argument("--update", required=True, type=Path)
    parser.add_argument("--firmware", required=True, type=Path)
    parser.add_argument("--fixup", required=True, type=Path)
    parser.add_argument("--kernel", required=True, type=Path)
    parser.add_argument("--memory", choices=("1G", "2G", "4G", "8G"),
                        default="2G")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--force", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        output = args.output.resolve()
        inputs = {
            path.resolve(strict=True)
            for path in (
                args.eeprom, args.sd_image, args.recovery, args.update,
                args.firmware, args.fixup, args.kernel)
        }
        if output in inputs:
            raise GateError("manifest output conflicts with an input")
        if output.exists() and not args.force:
            raise GateError(f"manifest already exists: {output}")
        manifest = build_manifest(
            args.eeprom, args.sd_image, args.recovery, args.update,
            args.firmware, args.fixup, args.kernel, args.memory)
        output.parent.mkdir(parents=True, exist_ok=True)
        temporary = output.with_name(output.name + ".tmp")
        temporary.write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8")
        os.replace(temporary, output)
        print(json.dumps(manifest, sort_keys=True))
        return 0
    except (GateError, OSError) as error:
        print(f"SD recovery manifest: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
