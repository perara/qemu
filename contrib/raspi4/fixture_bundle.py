#!/usr/bin/env python3
#
# Raspberry Pi 4B / CM4 physical-oracle bundle builder
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Build pinned HIL-plan and conformance-manifest files from one strict spec."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import tempfile
from typing import Any

import boot_trace
import conformance_gate
import hil_orchestrator


SCHEMA = "qemu.raspi.fixture-bundle-spec"
VERSION = 2
ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]*$")
SPEC_KEYS = {
    "schema",
    "version",
    "id",
    "platform",
    "board_revision",
    "flash_target",
    "artifacts",
    "event_contract",
    "fixture_commands",
    "qemu",
}
COMMAND_KEYS = {"command", "timeout_s"}
QEMU_KEYS = {"command", "timeout_s"}


class BundleError(RuntimeError):
    pass


def _keys(value: dict[str, Any], allowed: set[str], where: str) -> None:
    unknown = value.keys() - allowed
    if unknown:
        raise BundleError(f"{where}: unknown {', '.join(sorted(unknown))}")


def _path(root: Path, value: str) -> Path:
    path = Path(value)
    return path if path.is_absolute() else root / path


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as error:
        raise BundleError(f"{path}: {error.strerror}") from error
    return digest.hexdigest()


def _command(value: Any, where: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise BundleError(f"{where} must be an object")
    _keys(value, COMMAND_KEYS, where)
    command = value.get("command")
    if (
        not isinstance(command, list)
        or not command
        or any(not isinstance(item, str) or not item for item in command)
    ):
        raise BundleError(f"{where}.command must be a non-empty argv array")
    timeout = value.get("timeout_s", 300)
    if type(timeout) is not int or timeout <= 0 or timeout > 3600:
        raise BundleError(f"{where}.timeout_s must be in 1..3600")
    return {"command": list(command), "timeout_s": timeout}


def load_spec(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise BundleError(f"{path}: {error}") from error
    if not isinstance(value, dict):
        raise BundleError("spec must be an object")
    _keys(value, SPEC_KEYS, "spec")
    if value.get("schema") != SCHEMA or value.get("version") != VERSION:
        raise BundleError(f"expected {SCHEMA!r} version {VERSION}")
    bundle_id = value.get("id")
    if not isinstance(bundle_id, str) or not ID_RE.fullmatch(bundle_id):
        raise BundleError("spec.id must be a safe lowercase file stem")
    platform = value.get("platform")
    if platform not in hil_orchestrator.SEQUENCES:
        raise BundleError("spec.platform must be raspi4b or cm4")
    revision = value.get("board_revision")
    if not isinstance(revision, str) or not boot_trace.REVISION_RE.fullmatch(
        revision
    ):
        raise BundleError("spec.board_revision is invalid")
    try:
        hil_orchestrator._validate_flash_target(
            value.get("flash_target"), "spec.flash_target"
        )
    except hil_orchestrator.HILError as error:
        raise BundleError(str(error)) from error

    artifacts = value.get("artifacts")
    if not isinstance(artifacts, dict):
        raise BundleError("spec.artifacts must be an object")
    _keys(artifacts, hil_orchestrator.ARTIFACTS, "spec.artifacts")
    if not hil_orchestrator.REQUIRED_ARTIFACTS <= artifacts.keys():
        missing = hil_orchestrator.REQUIRED_ARTIFACTS - artifacts.keys()
        raise BundleError(
            f"spec.artifacts missing {', '.join(sorted(missing))}"
        )
    if (any(not isinstance(item, str)
       or not item for item in artifacts.values())):
        raise BundleError("spec.artifacts paths must be non-empty strings")

    try:
        conformance_gate._validate_event_contract(
            value.get("event_contract"), bundle_id
        )
    except conformance_gate.GateError as error:
        raise BundleError(str(error)) from error

    fixture_commands = value.get("fixture_commands")
    if not isinstance(fixture_commands, dict):
        raise BundleError("spec.fixture_commands must be an object")
    required_commands = set(hil_orchestrator.SEQUENCES[platform])
    required_commands.update(hil_orchestrator.CLEANUP_SEQUENCES[platform])
    _keys(fixture_commands, required_commands, "spec.fixture_commands")
    missing_commands = required_commands - fixture_commands.keys()
    if missing_commands:
        raise BundleError(
            "spec.fixture_commands missing "
            + ", ".join(sorted(missing_commands))
        )
    for name, command in fixture_commands.items():
        _command(command, f"spec.fixture_commands.{name}")

    qemu = value.get("qemu")
    if not isinstance(qemu, dict):
        raise BundleError("spec.qemu must be an object")
    _keys(qemu, QEMU_KEYS, "spec.qemu")
    _command(qemu, "spec.qemu")
    return value


def _resolve_artifacts(
    root: Path, values: dict[str, str]
) -> dict[str, dict[str, str]]:
    artifacts = {}
    identities: dict[tuple[int, int], str] = {}
    for name, value in sorted(values.items()):
        candidate = _path(root, value)
        if candidate.is_symlink():
            raise BundleError(f"artifact {name} must not be a symlink")
        try:
            path = candidate.resolve(strict=True)
            status = path.stat()
        except OSError as error:
            raise BundleError(f"artifact {name}: {error}") from error
        if not path.is_file():
            raise BundleError(f"artifact {name} must be a regular file")
        identity = (status.st_dev, status.st_ino)
        previous = identities.get(identity)
        if previous is not None:
            raise BundleError(
                f"artifacts {previous} and {name} are the same file"
            )
        identities[identity] = name
        artifacts[name] = {"path": str(path), "sha256": _sha256(path)}
    return artifacts


def _require_artifact_placeholders(
    commands: list[list[str]], artifacts: dict[str, dict[str, str]], where: str
) -> None:
    arguments = [item for command in commands for item in command]
    for name in artifacts:
        placeholder = f"{{artifact:{name}}}"
        if not any(placeholder in item for item in arguments):
            raise BundleError(f"{where} must reference {placeholder}")


def _expand_qemu_command(
    command: list[str], artifacts: dict[str, dict[str, str]]
) -> list[str]:
    expanded = []
    for argument in command:
        item = argument
        for name, artifact in artifacts.items():
            item = item.replace(
                f"{{artifact:{name}}}", artifact["path"]
            )
        if "{artifact:" in item:
            raise BundleError("spec.qemu.command has an invalid placeholder")
        expanded.append(item)
    return expanded


def _check_output_paths(
    paths: dict[str, Path], artifacts: dict[str, dict[str, str]]
) -> None:
    candidates = {
        **{name: path.absolute() for name, path in paths.items()},
        **{
            f"artifact {name}": Path(value["path"])
            for name, value in artifacts.items()
        },
    }
    for name, path in candidates.items():
        if path.is_symlink():
            raise BundleError(f"{name} path must not be a symlink")
    resolved = {name: path.resolve() for name, path in candidates.items()}
    if len(set(resolved.values())) != len(resolved):
        raise BundleError("bundle outputs and artifacts must be distinct")
    identities = {}
    for name, path in candidates.items():
        if not path.exists():
            continue
        if not path.is_file():
            raise BundleError(f"{name} path must be a regular file")
        status = path.stat()
        identity = (status.st_dev, status.st_ino)
        if identity in identities:
            raise BundleError(
                f"{identities[identity]} and {name} are the same file"
            )
        identities[identity] = name


def build_bundle(
    spec_path: Path, output_dir: Path, force: bool = False
) -> tuple[Path, Path]:
    spec = load_spec(spec_path)
    spec_root = spec_path.resolve().parent
    output_dir = output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    bundle_id = spec["id"]
    names = {
        "plan": f"{bundle_id}.hil-plan.json",
        "manifest": f"{bundle_id}.conformance.json",
        "hardware trace": f"{bundle_id}.hardware.jsonl",
        "HIL report": f"{bundle_id}.hil-report.json",
        "QEMU trace": f"{bundle_id}.qemu.jsonl",
    }
    paths = {name: output_dir / value for name, value in names.items()}
    artifacts = _resolve_artifacts(spec_root, spec["artifacts"])
    _check_output_paths(paths, artifacts)
    for name in ("plan", "manifest"):
        if paths[name].exists() and not force:
            raise BundleError(f"{name} already exists: {paths[name]}")

    fixture = spec["fixture_commands"]
    fixture_argv = [fixture[name]["command"] for name in fixture]
    _require_artifact_placeholders(fixture_argv, artifacts, "fixture commands")
    flash_name = next(
        name for name in hil_orchestrator.SEQUENCES[spec["platform"]]
        if name in hil_orchestrator.FLASH_STEPS
    )
    if "{flash-target}" not in fixture[flash_name]["command"]:
        raise BundleError(
            f"fixture command {flash_name} must reference "
            "{flash-target}"
        )
    qemu_command = spec["qemu"]["command"]
    _require_artifact_placeholders([qemu_command], artifacts, "QEMU command")

    def step(name: str) -> dict[str, Any]:
        command = fixture[name]
        return {
            "name": name,
            "command": command["command"],
            "timeout_s": command.get("timeout_s", 300),
            "capture_stdout": name == "capture-uart",
            "capture_flash_attestation": (
                name in hil_orchestrator.FLASH_STEPS
            ),
        }

    plan = {
        "schema": hil_orchestrator.SCHEMA,
        "version": hil_orchestrator.VERSION,
        "id": bundle_id,
        "platform": spec["platform"],
        "board_revision": spec["board_revision"],
        "flash_target": spec["flash_target"],
        "artifacts": artifacts,
        "trace": names["hardware trace"],
        "steps": [
            step(name) for name in hil_orchestrator.SEQUENCES[spec["platform"]]
        ],
        "cleanup": [
            step(name)
            for name in hil_orchestrator.CLEANUP_SEQUENCES[spec["platform"]]
        ],
    }
    case = {
        "id": bundle_id,
        "platform": spec["platform"],
        "board_revision": spec["board_revision"],
        "flash_target": spec["flash_target"],
        "artifacts": artifacts,
        "event_contract": spec["event_contract"],
        "hardware": {
            "trace": names["hardware trace"],
            "hil_plan": names["plan"],
            "hil_report": names["HIL report"],
        },
        "qemu": {
            "trace": names["QEMU trace"],
            "command": _expand_qemu_command(qemu_command, artifacts),
            "timeout_s": spec["qemu"].get("timeout_s", 300),
            "capture_stdout": True,
        },
    }
    manifest = {
        "schema": conformance_gate.SCHEMA,
        "version": conformance_gate.VERSION,
        "cases": [case],
    }

    with tempfile.TemporaryDirectory(
        dir=output_dir, prefix=".fixture-bundle-"
    ) as temporary:
        temporary_root = Path(temporary)
        temporary_plan = temporary_root / names["plan"]
        temporary_manifest = temporary_root / names["manifest"]
        temporary_plan.write_text(
            json.dumps(plan, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        temporary_manifest.write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        try:
            loaded_plan = hil_orchestrator.load_plan(temporary_plan)
            hil_orchestrator.validate_plan_paths(
                temporary_plan,
                temporary_root / names["HIL report"],
                loaded_plan,
            )
            conformance_gate._prepare_case(temporary_root, case)
        except (
            hil_orchestrator.HILError,
            conformance_gate.GateError,
        ) as error:
            raise BundleError(f"generated bundle failed preflight: {error}")
        os.replace(temporary_plan, paths["plan"])
        os.replace(temporary_manifest, paths["manifest"])
    return paths["plan"], paths["manifest"]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("spec", type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args(argv)
    try:
        plan, manifest = build_bundle(
            args.spec, args.output_dir, args.force
        )
        print(json.dumps(
            {"hil_plan": str(plan), "conformance_manifest": str(manifest)},
            sort_keys=True,
        ))
        return 0
    except (BundleError, OSError) as error:
        print(f"raspi-fixture-bundle: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
