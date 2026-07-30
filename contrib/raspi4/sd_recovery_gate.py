#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prove exact Raspberry Pi 4 SD recovery, reset, and firmware handoff."""

from __future__ import annotations

import argparse
import hashlib
import json
import mmap
import os
from pathlib import Path
import subprocess
import sys
from typing import Any

SCRIPT_DIRECTORY = Path(__file__).resolve().parent
if str(SCRIPT_DIRECTORY) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIRECTORY))

from cm4_provision import ProvisionError, QMPClient
from network_boot_gate import (
    EEPROM_SIZE,
    GateError,
    qemu_opt_path,
    sha256_file,
    terminate,
    wait_for_handoff,
)
from sd_recovery_manifest import SCHEMA, checked_file


RECOVERY_NAME = b"RECOVERYBIN"
RECOVERY_DONE_NAME = b"RECOVERY000"


def load_manifest(path: Path) -> tuple[dict[str, Any], str]:
    path = checked_file(path, "manifest")
    data = path.read_bytes()
    if len(data) > 1024 * 1024:
        raise GateError("manifest exceeds 1 MiB")
    digest = hashlib.sha256(data).hexdigest()
    try:
        manifest = json.loads(data)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise GateError(f"invalid manifest JSON: {error}") from error
    expected_keys = {
        "schema", "machine", "memory", "eeprom", "sd_image", "artifacts",
    }
    if not isinstance(manifest, dict) or set(manifest) != expected_keys:
        raise GateError("manifest has missing or unknown keys")
    if manifest["schema"] != SCHEMA or manifest["machine"] != "raspi4b":
        raise GateError("manifest schema or machine is invalid")
    if manifest["memory"] not in {"1G", "2G", "4G", "8G"}:
        raise GateError("manifest memory model is invalid")
    if set(manifest["eeprom"]) != {"sha256", "size"} or \
            manifest["eeprom"]["size"] != EEPROM_SIZE:
        raise GateError("manifest EEPROM metadata is invalid")
    if set(manifest["sd_image"]) != {"sha256", "size"} or \
            not isinstance(manifest["sd_image"]["size"], int) or \
            manifest["sd_image"]["size"] <= 0:
        raise GateError("manifest SD metadata is invalid")
    artifact_names = {
        "recovery.bin", "pieeprom.upd",
        "start4.elf", "fixup4.dat", "kernel8.img",
    }
    if not isinstance(manifest["artifacts"], dict) or \
            set(manifest["artifacts"]) != artifact_names:
        raise GateError("manifest artifact metadata is invalid")
    for digest_value in (
            manifest["eeprom"]["sha256"],
            manifest["sd_image"]["sha256"],
            *manifest["artifacts"].values()):
        if not isinstance(digest_value, str) or \
                len(digest_value) != 64 or \
                any(character not in "0123456789abcdef"
                    for character in digest_value):
            raise GateError("manifest contains an invalid SHA-256")
    return manifest, digest


def expected_renamed_sd_hash(path: Path) -> tuple[str, int]:
    with path.open("rb") as stream:
        with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as image:
            offset = image.find(RECOVERY_NAME)
            if offset < 0 or image.find(RECOVERY_NAME, offset + 1) >= 0:
                raise GateError(
                    "SD image must contain exactly one RECOVERY.BIN "
                    "short-name entry")
            if offset % 32 or offset + 12 > len(image) or \
                    image[offset + 11] not in (0x20, 0x21):
                raise GateError(
                    "RECOVERY.BIN is not an aligned FAT regular-file entry")
            if image.find(RECOVERY_DONE_NAME) >= 0:
                raise GateError("SD image already contains RECOVERY.000")
        digest = hashlib.sha256()
        remaining = offset
        while remaining:
            chunk = stream.read(min(1024 * 1024, remaining))
            if not chunk:
                raise GateError("SD image ended before recovery entry")
            digest.update(chunk)
            remaining -= len(chunk)
        old_name = stream.read(len(RECOVERY_NAME))
        if old_name != RECOVERY_NAME:
            raise GateError("SD recovery entry changed during preflight")
        digest.update(RECOVERY_DONE_NAME)
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest(), offset


def verify_trace(text: str) -> dict[str, Any]:
    expected = (
        ("rom.recovery-auth", "success"),
        ("recovery.discovered", "success"),
        ("eeprom.program", "restart"),
        ("handoff.arm", "success"),
    )
    lines = text.splitlines()
    observed = []
    search_from = 0
    for event, outcome in expected:
        for index in range(search_from, len(lines)):
            if event in lines[index] and f"outcome={outcome}" in lines[index]:
                observed.append({
                    "line": index + 1,
                    "event": event,
                    "outcome": outcome,
                })
                search_from = index + 1
                break
        else:
            raise GateError(
                f"QEMU trace lacks ordered {event}/{outcome} evidence")
    reset_count = sum(
        "event=reset.released" in line and "outcome=success" in line
        for line in lines)
    if reset_count < 2:
        raise GateError("QEMU trace lacks the automatic recovery reset")
    return {
        "events": observed,
        "reset_released_count": reset_count,
        "verified": True,
    }


def run(args: argparse.Namespace) -> dict[str, Any]:
    qemu = checked_file(args.qemu, "QEMU")
    if not os.access(qemu, os.X_OK):
        raise GateError(f"QEMU is not executable: {qemu}")
    eeprom = checked_file(args.eeprom, "EEPROM", EEPROM_SIZE)
    sd_image = checked_file(args.sd_image, "recovery SD image")
    recovery = checked_file(args.recovery, "recovery.bin")
    update = checked_file(args.update, "pieeprom.upd", EEPROM_SIZE)
    firmware = checked_file(args.firmware, "firmware")
    fixup = checked_file(args.fixup, "fixup")
    kernel = checked_file(args.kernel, "kernel")
    manifest_path = checked_file(args.manifest, "manifest")
    output = args.output.resolve()
    manifest, manifest_hash = load_manifest(manifest_path)
    if output.exists() and not args.force:
        raise GateError(f"report already exists: {output}")
    input_paths = {
        qemu, eeprom, sd_image, recovery, update,
        firmware, fixup, kernel, manifest_path,
    }
    if output in input_paths:
        raise GateError("report output conflicts with a gate input")
    inputs = {
        "eeprom": sha256_file(eeprom),
        "sd_image": sha256_file(sd_image),
        "recovery.bin": sha256_file(recovery),
        "pieeprom.upd": sha256_file(update),
        "start4.elf": sha256_file(firmware),
        "fixup4.dat": sha256_file(fixup),
        "kernel8.img": sha256_file(kernel),
    }
    if manifest["eeprom"] != {
            "sha256": inputs["eeprom"], "size": EEPROM_SIZE}:
        raise GateError("EEPROM does not match the manifest")
    if manifest["sd_image"] != {
            "sha256": inputs["sd_image"],
            "size": sd_image.stat().st_size}:
        raise GateError("SD image does not match the manifest")
    if manifest["artifacts"] != {
            name: inputs[name] for name in manifest["artifacts"]}:
        raise GateError("recovery artifacts do not match the manifest")
    expected_sd_hash, rename_offset = expected_renamed_sd_hash(sd_image)

    qmp_path = output.with_name(output.name + ".qmp.sock")
    qemu_log = output.with_name(output.name + ".qemu.log")
    for transient in (qmp_path, qemu_log):
        if transient in input_paths:
            raise GateError("transient gate path conflicts with a gate input")
        if transient.exists():
            raise GateError(f"transient gate path already exists: {transient}")
    qemu_command = [
        str(qemu), "-M",
        "raspi4b,boot-mode=behavioral,eeprom-drive=pieeprom,"
        f"recovery-trusted-sha256={inputs['recovery.bin']}",
        "-m", manifest["memory"],
        "-drive",
        "if=none,id=pieeprom,format=raw,"
        f"file={qemu_opt_path(eeprom)}",
        "-drive", f"if=sd,format=raw,file={qemu_opt_path(sd_image)}",
        "-nic", "none", "-display", "none", "-serial", "none",
        "-monitor", "none",
        "-qmp", f"unix:{qmp_path},server=on,wait=off",
        "-trace", "enable=raspi4b_boot_event",
    ]
    process: subprocess.Popen[Any] | None = None
    try:
        output.parent.mkdir(parents=True, exist_ok=True)
        with qemu_log.open("wb") as log_stream:
            process = subprocess.Popen(
                qemu_command, stdout=log_stream,
                stderr=subprocess.STDOUT)
        try:
            with QMPClient(qmp_path, process, args.timeout) as qmp:
                properties = wait_for_handoff(qmp, args.timeout)
                properties["recovery-status"] = qmp.qom_get(
                    "recovery-status")
        except ProvisionError as error:
            raise GateError(str(error)) from error
    finally:
        terminate(process)
        if qmp_path.exists():
            qmp_path.unlink()
    trace_text = qemu_log.read_text(encoding="utf-8", errors="replace")
    trace = verify_trace(trace_text)
    if properties["boot-source"] != "sd-card" or \
            properties["arm-handoff-status"] != "ready" or \
            properties["recovery-status"] != "none":
        raise GateError("recovery did not reach SD ARM handoff")
    expected_artifacts = {
        "firmware-sha256": inputs["start4.elf"],
        "firmware-fixup-sha256": inputs["fixup4.dat"],
        "firmware-kernel-sha256": inputs["kernel8.img"],
    }
    for name, digest in expected_artifacts.items():
        if properties[name] != digest:
            raise GateError(f"QEMU {name} does not match the manifest")
    final_eeprom = sha256_file(eeprom)
    final_sd = sha256_file(sd_image)
    if final_eeprom != inputs["pieeprom.upd"]:
        raise GateError("persistent EEPROM does not equal pieeprom.upd")
    if final_sd != expected_sd_hash:
        raise GateError("SD mutation is not exactly RECOVERY.BIN to .000")
    if sha256_file(manifest_path) != manifest_hash:
        raise GateError("manifest changed during the gate")
    for path, name in (
            (recovery, "recovery.bin"), (update, "pieeprom.upd"),
            (firmware, "start4.elf"), (fixup, "fixup4.dat"),
            (kernel, "kernel8.img")):
        if sha256_file(path) != inputs[name]:
            raise GateError(f"{name} changed during the gate")
    report = {
        "schema": "qemu-rpi-sd-recovery-gate-v1",
        "machine": "raspi4b",
        "memory": manifest["memory"],
        "manifest": {
            "path": str(manifest_path),
            "sha256": manifest_hash,
        },
        "eeprom": {
            "path": str(eeprom),
            "before_sha256": inputs["eeprom"],
            "after_sha256": final_eeprom,
            "expected_sha256": inputs["pieeprom.upd"],
        },
        "sd_image": {
            "path": str(sd_image),
            "before_sha256": inputs["sd_image"],
            "after_sha256": final_sd,
            "expected_after_sha256": expected_sd_hash,
            "rename_offset": rename_offset,
        },
        "artifacts": manifest["artifacts"],
        "qmp": properties,
        "trace": trace,
        "qemu_log": {
            "path": str(qemu_log),
            "sha256": sha256_file(qemu_log),
        },
        "verified": True,
    }
    temporary = output.with_name(output.name + ".tmp")
    temporary.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    os.replace(temporary, output)
    return report


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", required=True, type=Path)
    parser.add_argument("--eeprom", required=True, type=Path)
    parser.add_argument("--sd-image", required=True, type=Path)
    parser.add_argument("--recovery", required=True, type=Path)
    parser.add_argument("--update", required=True, type=Path)
    parser.add_argument("--firmware", required=True, type=Path)
    parser.add_argument("--fixup", required=True, type=Path)
    parser.add_argument("--kernel", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args(argv)
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    return args


def main() -> int:
    try:
        report = run(parse_args())
        print(json.dumps(report, sort_keys=True))
        return 0
    except (GateError, OSError, subprocess.SubprocessError) as error:
        print(f"SD recovery gate: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
