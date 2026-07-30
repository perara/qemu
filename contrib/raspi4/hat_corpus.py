#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run hash-pinned HAT EEPROM images through behavioral Raspberry Pi boot."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
from typing import Any

from cm4_provision import ProvisionError, QMPClient


SCHEMA = "qemu-rpi-hat-corpus-manifest-v1"
REPORT_SCHEMA = "qemu-rpi-hat-corpus-report-v1"
SHA256_RE = re.compile(r"[0-9a-f]{64}")
CASE_ID_RE = re.compile(r"[a-z0-9][a-z0-9._-]{0,63}")
UUID_RE = re.compile(
    r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-"
    r"[0-9a-f]{4}-[0-9a-f]{12}")
MAX_HAT_SIZE = 1024 * 1024
MAX_BOOTLOADER_SIZE = 1024 * 1024
MAX_MEDIA_SIZE = 16 * 1024**4
PLATFORMS = {"raspi4b", "raspi-cm4"}
EXPECTED_TYPES: dict[str, type] = {
    "boot_source": str,
    "vendor": str,
    "product": str,
    "uuid": str,
    "product_id": int,
    "product_version": int,
    "custom_count": int,
    "overlay": str,
    "overlays_applied": int,
    "gpio_status": str,
    "gpio_used_mask": int,
    "gpio_drive": int,
    "gpio_slew": int,
    "gpio_hysteresis": int,
    "gpio_back_power": int,
    "final_device_tree_sha256": str,
}
QOM_PROPERTIES = {
    "boot_source": "boot-source",
    "vendor": "hat-vendor",
    "product": "hat-product",
    "uuid": "hat-uuid",
    "product_id": "hat-product-id",
    "product_version": "hat-product-version",
    "custom_count": "hat-custom-count",
    "overlay": "hat-overlay",
    "overlays_applied": "firmware-overlay-applied",
    "gpio_status": "hat-gpio-map-status",
    "gpio_used_mask": "hat-gpio-used-mask",
    "gpio_drive": "hat-gpio-drive",
    "gpio_slew": "hat-gpio-slew",
    "gpio_hysteresis": "hat-gpio-hysteresis",
    "gpio_back_power": "hat-gpio-back-power",
    "final_device_tree_sha256": "firmware-final-device-tree-sha256",
}


class CorpusError(RuntimeError):
    """Invalid manifest, artifact, execution, or QMP response."""


class CorpusDrift(RuntimeError):
    """QEMU observations differ from the pinned expected result."""


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def resolve_artifact(base: Path, entry: Any, maximum: int,
                     description: str) -> dict[str, Any]:
    if not isinstance(entry, dict) or set(entry) != {"path", "sha256"}:
        raise CorpusError(
            f"{description} must contain exactly path and sha256")
    if not isinstance(entry["path"], str) or not entry["path"]:
        raise CorpusError(f"{description} path is invalid")
    expected_hash = entry["sha256"]
    if not isinstance(expected_hash, str) or not SHA256_RE.fullmatch(
            expected_hash):
        raise CorpusError(f"{description} SHA-256 is invalid")
    supplied = Path(entry["path"])
    candidate = supplied if supplied.is_absolute() else base / supplied
    try:
        path = candidate.resolve(strict=True)
    except OSError as error:
        raise CorpusError(f"{description} is unavailable: {candidate}") \
            from error
    if candidate.is_symlink() or not path.is_file():
        raise CorpusError(
            f"{description} must be a regular non-symlink file")
    size = path.stat().st_size
    if size < 1 or size > maximum:
        raise CorpusError(
            f"{description} size {size} is outside 1..{maximum}")
    actual_hash = sha256_file(path)
    if actual_hash != expected_hash:
        raise CorpusError(f"{description} SHA-256 does not match")
    return {"path": path, "size": size, "sha256": actual_hash}


def derive_hat(path: Path) -> dict[str, Any]:
    data = path.read_bytes()
    if len(data) < 12 or data[:4] != b"R-Pi":
        raise CorpusError("HAT EEPROM header is invalid")
    declared_size = int.from_bytes(data[8:12], "little")
    if declared_size < 12 or declared_size > len(data):
        raise CorpusError("HAT EEPROM declared size is invalid")
    backend_size = (len(data) + 511) & ~511
    backend = data + bytes(backend_size - len(data))
    return {
        "file_size": len(data),
        "file_sha256": sha256_bytes(data),
        "backend_size": backend_size,
        "backend_sha256": sha256_bytes(backend),
        "declared_size": declared_size,
        "content_sha256": sha256_bytes(data[:declared_size]),
    }


def validate_expected(value: Any, case_id: str) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != set(EXPECTED_TYPES):
        raise CorpusError(
            f"case {case_id} expected keys must be exactly "
            f"{sorted(EXPECTED_TYPES)}")
    result: dict[str, Any] = {}
    for name, expected_type in EXPECTED_TYPES.items():
        item = value[name]
        if expected_type is int:
            if isinstance(item, bool) or not isinstance(item, int) or item < 0:
                raise CorpusError(
                    f"case {case_id} expected {name} is invalid")
        elif not isinstance(item, expected_type):
            raise CorpusError(
                f"case {case_id} expected {name} is invalid")
        result[name] = item
    if not SHA256_RE.fullmatch(result["final_device_tree_sha256"]):
        raise CorpusError(
            f"case {case_id} final Device Tree SHA-256 is invalid")
    if not UUID_RE.fullmatch(result["uuid"]):
        raise CorpusError(f"case {case_id} UUID is invalid")
    if result["product_id"] > 0xffff or result["product_version"] > 0xffff:
        raise CorpusError(f"case {case_id} product number is out of range")
    if (result["custom_count"] > 0xffff or
            result["overlays_applied"] > 0xffffffff):
        raise CorpusError(f"case {case_id} atom count is out of range")
    if result["gpio_used_mask"] > 0x0fffffff:
        raise CorpusError(f"case {case_id} GPIO mask is out of range")
    if (result["gpio_drive"] > 8 or result["gpio_slew"] > 2 or
            result["gpio_hysteresis"] > 2 or
            result["gpio_back_power"] > 2):
        raise CorpusError(f"case {case_id} GPIO policy is out of range")
    return result


def preflight_manifest(path: Path) -> list[dict[str, Any]]:
    try:
        manifest_path = path.resolve(strict=True)
        document = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CorpusError(f"manifest cannot be read: {error}") from error
    if not isinstance(document, dict) or set(document) != {"schema", "cases"}:
        raise CorpusError("manifest must contain exactly schema and cases")
    if document["schema"] != SCHEMA:
        raise CorpusError(f"manifest schema must be {SCHEMA}")
    cases = document["cases"]
    if not isinstance(cases, list) or not cases:
        raise CorpusError("manifest cases must be a non-empty array")
    base = manifest_path.parent
    seen: set[str] = set()
    resolved: list[dict[str, Any]] = []
    for index, case in enumerate(cases):
        if not isinstance(case, dict) or set(case) != {
                "id", "platform", "bootloader", "media", "hat",
                "expected"}:
            raise CorpusError(f"case {index} has unknown or missing keys")
        case_id = case["id"]
        if not isinstance(case_id, str) or not CASE_ID_RE.fullmatch(case_id):
            raise CorpusError(f"case {index} id is invalid")
        if case_id in seen:
            raise CorpusError(f"duplicate case id: {case_id}")
        seen.add(case_id)
        platform = case["platform"]
        if platform not in PLATFORMS:
            raise CorpusError(f"case {case_id} platform is invalid")
        resolved.append({
            "id": case_id,
            "platform": platform,
            "bootloader": resolve_artifact(
                base, case["bootloader"], MAX_BOOTLOADER_SIZE,
                f"case {case_id} bootloader"),
            "media": resolve_artifact(
                base, case["media"], MAX_MEDIA_SIZE,
                f"case {case_id} media"),
            "hat": resolve_artifact(
                base, case["hat"], MAX_HAT_SIZE,
                f"case {case_id} HAT EEPROM"),
            "expected": validate_expected(case["expected"], case_id),
        })
    return resolved


def qemu_opt_path(path: Path) -> str:
    return str(path).replace(",", ",,")


def qemu_command(qemu: Path, case: dict[str, Any],
                 qmp_path: Path) -> list[str]:
    platform = case["platform"]
    machine = (
        f"{platform},boot-mode=behavioral,eeprom-drive=pieeprom,"
        "hat-eeprom-drive=hat"
    )
    if platform == "raspi-cm4":
        machine += ",emmc-drive=emmc"
    command = [
        str(qemu), "-display", "none", "-M", machine,
        "-drive",
        ("if=none,id=pieeprom,format=raw,snapshot=on,file=" +
         qemu_opt_path(case["bootloader"]["path"])),
        "-drive",
        ("if=none,id=hat,format=raw,readonly=on,file=" +
         qemu_opt_path(case["hat"]["path"])),
    ]
    if platform == "raspi-cm4":
        command.extend([
            "-drive",
            ("if=none,id=emmc,format=raw,snapshot=on,file=" +
             qemu_opt_path(case["media"]["path"])),
        ])
    else:
        command.extend([
            "-drive",
            ("if=sd,format=raw,snapshot=on,file=" +
             qemu_opt_path(case["media"]["path"])),
        ])
    command.extend([
        "-nic", "none", "-no-reboot",
        "-qmp", f"unix:{qmp_path},server=on,wait=off",
    ])
    return command


def terminate(process: subprocess.Popen[Any] | None) -> None:
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def collect_observed(qmp: QMPClient, timeout: float) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    last_state: Any = None
    while time.monotonic() < deadline:
        last_state = qmp.qom_get("boot-state")
        if last_state == "arm-handoff-ready":
            break
        time.sleep(0.05)
    else:
        raise CorpusError(
            f"QEMU did not reach ARM handoff; last state={last_state!r}")
    observed = {
        name: qmp.qom_get(property_name)
        for name, property_name in QOM_PROPERTIES.items()
    }
    observed.update({
        "boot_state": qmp.qom_get("boot-state"),
        "handoff_status": qmp.qom_get("arm-handoff-status"),
        "hat_status": qmp.qom_get("hat-eeprom-status"),
        "hat_eeprom_size": qmp.qom_get("hat-eeprom-size"),
        "hat_eeprom_declared_size":
            qmp.qom_get("hat-eeprom-declared-size"),
        "hat_eeprom_sha256": qmp.qom_get("hat-eeprom-sha256"),
        "hat_content_sha256": qmp.qom_get("hat-content-sha256"),
    })
    return observed


def compare_case(case: dict[str, Any], derived: dict[str, Any],
                 observed: dict[str, Any]) -> dict[str, dict[str, Any]]:
    fixed = {
        "boot_state": "arm-handoff-ready",
        "handoff_status": "ready",
        "hat_status": "valid",
        "hat_eeprom_size": derived["backend_size"],
        "hat_eeprom_declared_size": derived["declared_size"],
        "hat_eeprom_sha256": derived["backend_sha256"],
        "hat_content_sha256": derived["content_sha256"],
    }
    expected = {**case["expected"], **fixed}
    differences = {
        key: {"expected": value, "observed": observed.get(key)}
        for key, value in expected.items()
        if observed.get(key) != value
    }
    return differences


def execute_case(qemu: Path, case: dict[str, Any],
                 timeout: float, temporary: Path) -> dict[str, Any]:
    qmp_path = temporary / f"{case['id']}.qmp"
    log_path = temporary / f"{case['id']}.log"
    process: subprocess.Popen[Any] | None = None
    derived = derive_hat(case["hat"]["path"])
    with log_path.open("wb") as log:
        try:
            process = subprocess.Popen(
                qemu_command(qemu, case, qmp_path),
                stdin=subprocess.DEVNULL, stdout=log, stderr=log)
            with QMPClient(qmp_path, process, timeout) as qmp:
                observed = collect_observed(qmp, timeout)
            differences = compare_case(case, derived, observed)
        except (OSError, ProvisionError) as error:
            raise CorpusError(
                f"case {case['id']} could not execute: {error}") from error
        finally:
            terminate(process)
    return {
        "id": case["id"],
        "platform": case["platform"],
        "artifacts": {
            name: {
                "path": str(case[name]["path"]),
                "size": case[name]["size"],
                "sha256": case[name]["sha256"],
            }
            for name in ("bootloader", "media", "hat")
        },
        "hat": derived,
        "expected": case["expected"],
        "observed": observed,
        "differences": differences,
        "match": not differences,
    }


def atomic_write_json(path: Path, document: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            json.dump(document, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def run(args: argparse.Namespace) -> dict[str, Any]:
    qemu = args.qemu.resolve(strict=True)
    if not qemu.is_file() or not os.access(qemu, os.X_OK):
        raise CorpusError("QEMU binary is not an executable regular file")
    cases = preflight_manifest(args.manifest)
    results = []
    with tempfile.TemporaryDirectory(prefix="qemu-rpi-hat-corpus-") as name:
        temporary = Path(name)
        for case in cases:
            results.append(execute_case(qemu, case, args.timeout, temporary))
    report = {
        "schema": REPORT_SCHEMA,
        "manifest_schema": SCHEMA,
        "case_count": len(results),
        "match": all(result["match"] for result in results),
        "cases": results,
    }
    atomic_write_json(args.output, report)
    if not report["match"]:
        mismatch_count = sum(not result["match"] for result in results)
        raise CorpusDrift(
            f"{mismatch_count} of {len(results)} cases differ; "
            f"report written to {args.output}")
    return report


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=30)
    args = parser.parse_args(argv)
    if args.timeout <= 0 or args.timeout > 600:
        parser.error("--timeout must be within (0, 600] seconds")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        run(args)
    except CorpusDrift as error:
        print(f"hat-corpus drift: {error}", file=sys.stderr)
        return 1
    except (CorpusError, OSError) as error:
        print(f"hat-corpus invalid: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
