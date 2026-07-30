#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prove network retry when an interrupted EEPROM remains network-bootable."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
from typing import Any

SCRIPT_DIRECTORY = Path(__file__).resolve().parent
if str(SCRIPT_DIRECTORY) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIRECTORY))

from network_boot_gate import (
    EEPROM_SIZE,
    GateError,
    MANIFEST_SCHEMA,
    sha256_file,
)


NETWORK_GATE = SCRIPT_DIRECTORY / "network_boot_gate.py"
MANIFEST_BUILDER = SCRIPT_DIRECTORY / "network_boot_manifest.py"


def load_failure_contract(path: Path) -> dict[str, Any]:
    if not path.is_file() or path.is_symlink():
        raise GateError(
            f"failure manifest is not a regular non-symlink file: {path}")
    data = path.read_bytes()
    if len(data) > 1024 * 1024:
        raise GateError("failure manifest exceeds 1 MiB")
    try:
        manifest = json.loads(data)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise GateError(f"invalid failure manifest JSON: {error}") from error
    if not isinstance(manifest, dict) or \
            manifest.get("schema") != MANIFEST_SCHEMA:
        raise GateError("failure manifest schema is invalid")
    update = manifest.get("eeprom_update")
    if not isinstance(update, dict) or \
            update.get("expected_result") != "program-failure":
        raise GateError(
            "recovery campaign requires a program-failure manifest")
    if update.get("size") != EEPROM_SIZE:
        raise GateError("recovery update must be exactly 512 KiB")
    if not isinstance(update.get("path"), str) or not update["path"] or \
            not isinstance(update.get("sha256"), str) or \
            len(update["sha256"]) != 64:
        raise GateError("recovery update identity is invalid")
    fail_after = update.get("fail_after")
    if not isinstance(fail_after, int) or isinstance(fail_after, bool) or \
            not 0 <= fail_after < EEPROM_SIZE:
        raise GateError("recovery fail-after boundary is invalid")
    for name in ("machine", "memory", "expected_firmware"):
        if not isinstance(manifest.get(name), str) or not manifest[name]:
            raise GateError(f"failure manifest {name} is invalid")
    return manifest


def run_checked(command: list[str], description: str) -> None:
    result = subprocess.run(
        command, capture_output=True, text=True, check=False)
    if result.returncode:
        raise GateError(
            f"{description} failed with status {result.returncode}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")


def run(args: argparse.Namespace) -> dict[str, Any]:
    qemu = args.qemu.resolve(strict=True)
    eeprom = args.eeprom.resolve(strict=True)
    tftp_root = args.tftp_root.resolve(strict=True)
    failure_manifest = args.manifest.resolve(strict=True)
    output = args.output.resolve()
    evidence = args.evidence_directory.resolve()
    contract = load_failure_contract(failure_manifest)
    update_metadata = contract["eeprom_update"]
    update = (tftp_root / update_metadata["path"]).resolve(strict=True)
    try:
        update.relative_to(tftp_root)
    except ValueError as error:
        raise GateError("recovery update escapes the TFTP root") from error
    if update.is_symlink() or not update.is_file():
        raise GateError("recovery update is absent or unsafe")
    if sha256_file(update) != update_metadata["sha256"]:
        raise GateError("recovery update does not match the failure manifest")
    if evidence.exists():
        raise GateError(f"evidence directory already exists: {evidence}")
    if output.exists():
        raise GateError(f"campaign report already exists: {output}")
    for candidate, description in (
            (output, "campaign report"),
            (evidence, "evidence directory")):
        try:
            candidate.relative_to(tftp_root)
        except ValueError:
            pass
        else:
            raise GateError(f"{description} must be outside the TFTP root")

    evidence.mkdir(parents=True)
    failure_report = evidence / "failure-report.json"
    failure_pcap = evidence / "failure.pcap"
    recovery_manifest = evidence / "recovery-manifest.json"
    recovery_report = evidence / "recovery-report.json"
    recovery_pcap = evidence / "recovery.pcap"
    common_gate = [
        sys.executable, str(NETWORK_GATE),
        "--qemu", str(qemu), "--eeprom", str(eeprom),
        "--tftp-root", str(tftp_root),
        "--machine", contract["machine"],
        "--memory", contract["memory"],
        "--expected-firmware", contract["expected_firmware"],
        "--server-ip", args.server_ip,
        "--client-start", args.client_start,
        "--client-end", args.client_end,
        "--mac", args.mac,
        "--timeout", str(args.timeout),
        "--allow-eeprom-update",
    ]
    run_checked([
        *common_gate,
        "--manifest", str(failure_manifest),
        "--output", str(failure_report),
        "--pcap-output", str(failure_pcap),
    ], "interrupted update phase")
    failure = json.loads(failure_report.read_text(encoding="utf-8"))
    transition = failure["eeprom"]["transition"]
    if transition.get("expected_result") != "program-failure" or \
            not transition.get("verified") or not transition.get("changed"):
        raise GateError("failure phase lacks a verified partial transition")
    partial_hash = failure["eeprom"]["sha256"]
    if partial_hash == update_metadata["sha256"]:
        raise GateError("failure phase unexpectedly installed the full update")

    run_checked([
        sys.executable, str(MANIFEST_BUILDER),
        "--eeprom", str(eeprom), "--tftp-root", str(tftp_root),
        "--output", str(recovery_manifest),
        "--machine", contract["machine"],
        "--memory", contract["memory"],
        "--expected-firmware", contract["expected_firmware"],
        "--eeprom-update", update_metadata["path"],
    ], "recovery manifest phase")
    run_checked([
        *common_gate,
        "--manifest", str(recovery_manifest),
        "--output", str(recovery_report),
        "--pcap-output", str(recovery_pcap),
    ], "recovery update phase")
    recovery = json.loads(recovery_report.read_text(encoding="utf-8"))
    if recovery["eeprom"]["sha256"] != update_metadata["sha256"] or \
            recovery["qmp"].get("arm-handoff-status") != "ready":
        raise GateError("recovery phase did not install and boot the update")

    report = {
        "schema": "qemu-rpi-network-recovery-gate-v1",
        "machine": contract["machine"],
        "memory": contract["memory"],
        "eeprom": {
            "path": str(eeprom),
            "initial_sha256": transition["before_sha256"],
            "partial_sha256": partial_hash,
            "final_sha256": recovery["eeprom"]["sha256"],
            "update_sha256": update_metadata["sha256"],
            "fail_after": update_metadata["fail_after"],
        },
        "recovery_method": "network-self-update",
        "failure": {
            "manifest": str(failure_manifest),
            "manifest_sha256": sha256_file(failure_manifest),
            "report": str(failure_report),
            "report_sha256": sha256_file(failure_report),
            "pcap": str(failure_pcap),
            "pcap_sha256": sha256_file(failure_pcap),
        },
        "recovery": {
            "manifest": str(recovery_manifest),
            "manifest_sha256": sha256_file(recovery_manifest),
            "report": str(recovery_report),
            "report_sha256": sha256_file(recovery_report),
            "pcap": str(recovery_pcap),
            "pcap_sha256": sha256_file(recovery_pcap),
        },
        "verified": True,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
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
    parser.add_argument("--tftp-root", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--evidence-directory", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--server-ip", default="10.42.0.1")
    parser.add_argument("--client-start", default="10.42.0.10")
    parser.add_argument("--client-end", default="10.42.0.50")
    parser.add_argument("--mac", default="52:54:00:12:34:56")
    parser.add_argument("--timeout", type=float, default=60.0)
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
        print(f"network recovery gate: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
