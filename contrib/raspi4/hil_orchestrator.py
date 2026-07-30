#!/usr/bin/env python3
#
# Raspberry Pi 4B / CM4 hardware-in-the-loop orchestrator
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Run a pinned, fail-closed Raspberry Pi hardware capture plan."""

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
from typing import Any

import boot_trace


SCHEMA = "qemu.raspi.hil-plan"
REPORT_SCHEMA = "qemu.raspi.hil-report"
VERSION = 2
ARTIFACTS = {"eeprom", "firmware", "media", "otp"}
REQUIRED_ARTIFACTS = {"eeprom", "firmware", "media"}
FLASH_STEPS = {"flash-media", "flash-emmc"}
SEQUENCES = {
    "raspi4b": (
        "power-off",
        "flash-media",
        "power-on",
        "capture-uart",
        "health-check",
    ),
    "cm4": (
        "power-off",
        "assert-nrpiboot",
        "power-on-rpiboot",
        "run-rpiboot",
        "flash-emmc",
        "power-off-flashed",
        "release-nrpiboot",
        "power-on-boot",
        "capture-uart",
        "health-check",
    ),
}
CLEANUP_SEQUENCES = {
    "raspi4b": ("power-off-cleanup",),
    "cm4": ("power-off-cleanup", "release-nrpiboot-cleanup"),
}
PLACEHOLDER_RE = re.compile(
    r"\{artifact:([a-z]+)\}|\{trace\}|\{flash-target\}"
)


class HILError(RuntimeError):
    pass


def _validate_flash_target(value: Any, where: str) -> str:
    if not isinstance(value, str) or not value:
        raise HILError(f"{where} must be a non-empty path")
    path = Path(value)
    if (
        not path.is_absolute()
        or str(path) != value
        or path.parent not in (
            Path("/dev/disk/by-id"),
            Path("/dev/disk/by-path"),
        )
        or path.name in ("", ".", "..")
    ):
        raise HILError(
            f"{where} must be a normalized /dev/disk/by-id or by-path path"
        )
    return value


def _keys(value: dict[str, Any], allowed: set[str], where: str) -> None:
    unknown = value.keys() - allowed
    if unknown:
        raise HILError(f"{where}: unknown {', '.join(sorted(unknown))}")


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
        raise HILError(f"{path}: {error.strerror}") from error
    return digest.hexdigest()


def _validate_step(value: Any, where: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise HILError(f"{where} must be an object")
    _keys(
        value,
        {
            "name",
            "command",
            "timeout_s",
            "capture_stdout",
            "capture_flash_attestation",
        },
        where,
    )
    name = value.get("name")
    command = value.get("command")
    timeout = value.get("timeout_s", 300)
    capture = value.get("capture_stdout", False)
    capture_flash = value.get("capture_flash_attestation", False)
    if not isinstance(name, str) or not name:
        raise HILError(f"{where}.name must be a non-empty string")
    if (
        not isinstance(command, list)
        or not command
        or any(not isinstance(item, str) or not item for item in command)
    ):
        raise HILError(f"{where}.command must be a non-empty argv array")
    if type(timeout) is not int or timeout <= 0 or timeout > 3600:
        raise HILError(f"{where}.timeout_s must be in 1..3600")
    if type(capture) is not bool:
        raise HILError(f"{where}.capture_stdout must be boolean")
    if type(capture_flash) is not bool:
        raise HILError(
            f"{where}.capture_flash_attestation must be boolean"
        )
    if capture != (name == "capture-uart"):
        raise HILError(
            f"{where}.capture_stdout must be true only for capture-uart"
        )
    if capture_flash != (name in FLASH_STEPS):
        raise HILError(
            f"{where}.capture_flash_attestation must be true only for "
            "the platform flash step"
        )
    return value


def load_plan(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise HILError(f"{path}: {error}") from error
    if not isinstance(value, dict):
        raise HILError("plan must be an object")
    _keys(
        value,
        {
            "schema",
            "version",
            "id",
            "platform",
            "board_revision",
            "flash_target",
            "artifacts",
            "trace",
            "steps",
            "cleanup",
        },
        "plan",
    )
    if value.get("schema") != SCHEMA or value.get("version") != VERSION:
        raise HILError(f"expected {SCHEMA!r} version {VERSION}")
    if not isinstance(value.get("id"), str) or not value["id"]:
        raise HILError("plan.id must be a non-empty string")
    platform = value.get("platform")
    if platform not in SEQUENCES:
        raise HILError("plan.platform must be raspi4b or cm4")
    revision = value.get("board_revision")
    if not isinstance(revision, str) or not boot_trace.REVISION_RE.fullmatch(
        revision
    ):
        raise HILError("plan.board_revision is invalid")
    if not isinstance(value.get("trace"), str) or not value["trace"]:
        raise HILError("plan.trace must be a non-empty path")
    _validate_flash_target(value.get("flash_target"), "plan.flash_target")
    artifacts = value.get("artifacts")
    if not isinstance(artifacts, dict):
        raise HILError("plan.artifacts must be an object")
    _keys(artifacts, ARTIFACTS, "plan.artifacts")
    if not REQUIRED_ARTIFACTS <= artifacts.keys():
        missing = REQUIRED_ARTIFACTS - artifacts.keys()
        raise HILError(
            f"plan.artifacts missing {', '.join(sorted(missing))}"
        )
    for name, artifact in artifacts.items():
        where = f"plan.artifacts.{name}"
        if not isinstance(artifact, dict):
            raise HILError(f"{where} must be an object")
        _keys(artifact, {"path", "sha256"}, where)
        if not isinstance(artifact.get("path"), str) or not artifact["path"]:
            raise HILError(f"{where}.path must be a non-empty string")
        digest = artifact.get("sha256")
        if not isinstance(digest, str) or not boot_trace.SHA256_RE.fullmatch(
            digest
        ):
            raise HILError(f"{where}.sha256 must be a lowercase SHA-256")
    for collection in ("steps", "cleanup"):
        items = value.get(collection)
        if not isinstance(items, list):
            raise HILError(f"plan.{collection} must be an array")
        for index, item in enumerate(items):
            _validate_step(item, f"plan.{collection}[{index}]")
    names = tuple(item["name"] for item in value["steps"])
    if names != SEQUENCES[platform]:
        raise HILError(
            f"plan.steps for {platform} must be "
            f"{', '.join(SEQUENCES[platform])}"
        )
    cleanup_names = tuple(item["name"] for item in value["cleanup"])
    if cleanup_names != CLEANUP_SEQUENCES[platform]:
        raise HILError(
            f"plan.cleanup for {platform} must be "
            f"{', '.join(CLEANUP_SEQUENCES[platform])}"
        )
    for step in [*value["steps"], *value["cleanup"]]:
        references_target = "{flash-target}" in step["command"]
        if references_target != (step["name"] in FLASH_STEPS):
            raise HILError(
                "only the platform flash step must reference "
                "{flash-target}"
            )
    commands = [
        argument
        for step in value["steps"]
        for argument in step["command"]
    ]
    for name in sorted(REQUIRED_ARTIFACTS):
        placeholder = f"{{artifact:{name}}}"
        if not any(placeholder in argument for argument in commands):
            raise HILError(
                f"plan.steps must reference verified {placeholder}"
            )
    return value


def _verify_artifacts(
    root: Path, artifacts: dict[str, dict[str, str]]
) -> tuple[dict[str, str], dict[str, Path]]:
    verified: dict[str, str] = {}
    paths: dict[str, Path] = {}
    for name, value in sorted(artifacts.items()):
        path = _path(root, value["path"]).resolve()
        actual = _sha256(path)
        if actual != value["sha256"]:
            raise HILError(
                f"artifact {name}: SHA-256 mismatch: "
                f"expected {value['sha256']}, got {actual}"
            )
        verified[name] = actual
        paths[name] = path
    return verified, paths


def validate_plan_paths(
    path: Path,
    report_path: Path,
    plan: dict[str, Any] | None = None,
) -> tuple[Path, Path, dict[str, Path]]:
    """Resolve and isolate every input and output before fixture access."""

    if plan is None:
        plan = load_plan(path)
    plan_path = path.resolve()
    root = plan_path.parent
    candidates = {
        "plan": path.absolute(),
        "trace": _path(root, plan["trace"]).absolute(),
        "report": report_path.absolute(),
        **{
            f"artifact {name}": _path(root, value["path"]).absolute()
            for name, value in plan["artifacts"].items()
        },
    }
    for name, candidate in candidates.items():
        if candidate.is_symlink():
            raise HILError(f"{name} path must not be a symlink")

    resolved = {
        name: candidate.resolve() for name, candidate in candidates.items()
    }
    seen: dict[Path, str] = {}
    for name, candidate in resolved.items():
        previous = seen.get(candidate)
        if previous is not None:
            raise HILError(
                f"path collision: {previous} and {name} resolve to {candidate}"
            )
        seen[candidate] = name

    seen_files: dict[tuple[int, int], str] = {}
    for name, candidate in candidates.items():
        if not candidate.exists():
            continue
        if not candidate.is_file():
            raise HILError(f"{name} path must be a regular file")
        status = candidate.stat()
        identity = (status.st_dev, status.st_ino)
        previous = seen_files.get(identity)
        if previous is not None:
            raise HILError(
                f"path collision: {previous} and {name} are the same file"
            )
        seen_files[identity] = name

    artifact_paths = {
        name: resolved[f"artifact {name}"] for name in plan["artifacts"]
    }
    return resolved["trace"], resolved["report"], artifact_paths


def _expand_command(
    command: list[str],
    artifacts: dict[str, Path],
    trace: Path,
    flash_target: str,
) -> list[str]:
    def replace(match: re.Match[str]) -> str:
        if match.group(0) == "{trace}":
            return str(trace)
        if match.group(0) == "{flash-target}":
            return flash_target
        name = match.group(1)
        assert name is not None
        if name not in artifacts:
            raise HILError(f"command references unavailable artifact {name}")
        return str(artifacts[name])

    expanded = [PLACEHOLDER_RE.sub(replace, item) for item in command]
    if any(
        "{artifact:" in item
        or "{trace}" in item
        or "{flash-target}" in item
        for item in expanded
    ):
        raise HILError("command contains an invalid placeholder")
    return expanded


def _result_record(
    name: str, result: subprocess.CompletedProcess[bytes]
) -> dict[str, Any]:
    return {
        "name": name,
        "returncode": result.returncode,
        "stdout_bytes": len(result.stdout),
        "stdout_sha256": hashlib.sha256(result.stdout).hexdigest(),
        "stderr_bytes": len(result.stderr),
        "stderr_sha256": hashlib.sha256(result.stderr).hexdigest(),
    }


def _validate_flash_attestation(
    value: Any,
    expected_sha256: str,
    expected_size: int,
    expected_source: Path | None = None,
    expected_target: str | None = None,
) -> dict[str, Any]:
    where = "flash attestation"
    if not isinstance(value, dict):
        raise HILError(f"{where} must be a JSON object")
    _keys(
        value,
        {
            "source",
            "target",
            "target_identity",
            "bytes_written",
            "source_sha256",
            "verified",
            "resumed",
            "resume_from",
            "resume_journal",
            "target_capacity",
            "target_sha256",
        },
        where,
    )
    required = {
        "source",
        "target",
        "target_identity",
        "bytes_written",
        "source_sha256",
        "verified",
        "resumed",
        "resume_from",
        "resume_journal",
        "target_capacity",
        "target_sha256",
    }
    missing = required - value.keys()
    if missing:
        raise HILError(
            f"{where} missing {', '.join(sorted(missing))}"
        )
    for name in ("source", "target", "resume_journal"):
        if not isinstance(value[name], str) or not value[name]:
            raise HILError(f"{where}.{name} must be a non-empty string")
    if (
        expected_source is not None
        and Path(value["source"]).resolve() != expected_source.resolve()
    ):
        raise HILError(f"{where}.source does not match media artifact")
    if expected_target is not None and value["target"] != expected_target:
        raise HILError(f"{where}.target does not match authorized target")
    if (
        value["source_sha256"] != expected_sha256
        or value["target_sha256"] != expected_sha256
    ):
        raise HILError(f"{where} media SHA-256 mismatch")
    if type(value["bytes_written"]) is not int or (
        value["bytes_written"] != expected_size
    ):
        raise HILError(f"{where}.bytes_written does not match media")
    if value["verified"] is not True:
        raise HILError(f"{where}.verified must be true")
    if type(value["resumed"]) is not bool:
        raise HILError(f"{where}.resumed must be boolean")
    if type(value["resume_from"]) is not int or not (
        0 <= value["resume_from"] <= expected_size
    ):
        raise HILError(f"{where}.resume_from is invalid")
    if not value["resumed"] and value["resume_from"] != 0:
        raise HILError(f"{where}.resume_from requires resumed=true")

    identity = value["target_identity"]
    if not isinstance(identity, dict):
        raise HILError(f"{where}.target_identity must be an object")
    identity_keys = {
        "path",
        "device",
        "inode",
        "rdev",
        "kind",
        "major",
        "minor",
        "kernel_name",
        "sysfs_path",
        "capacity",
        "logical_sector_size",
        "removable",
        "read_only",
    }
    _keys(identity, identity_keys, f"{where}.target_identity")
    missing = identity_keys - identity.keys()
    if missing:
        raise HILError(
            f"{where}.target_identity missing "
            f"{', '.join(sorted(missing))}"
        )
    if identity["kind"] != "block":
        raise HILError(f"{where} target must be a block device")
    for name in ("path", "sysfs_path"):
        if (
            not isinstance(identity[name], str)
            or not Path(identity[name]).is_absolute()
        ):
            raise HILError(
                f"{where}.target_identity.{name} must be absolute"
            )
    if (
        not isinstance(identity["kernel_name"], str)
        or not identity["kernel_name"]
        or Path(identity["kernel_name"]).name != identity["kernel_name"]
    ):
        raise HILError(
            f"{where}.target_identity.kernel_name is invalid"
        )
    for name in ("device", "inode", "rdev", "major", "minor", "capacity"):
        if type(identity[name]) is not int or identity[name] < 0:
            raise HILError(
                f"{where}.target_identity.{name} is invalid"
            )
    try:
        device_numbers_match = (
            os.major(identity["rdev"]) == identity["major"]
            and os.minor(identity["rdev"]) == identity["minor"]
        )
    except (OverflowError, ValueError):
        device_numbers_match = False
    if not device_numbers_match:
        raise HILError(f"{where} target device numbers are inconsistent")
    if (
        identity["capacity"] != value["target_capacity"]
        or identity["capacity"] < expected_size
    ):
        raise HILError(f"{where} target capacity is inconsistent")
    if identity["logical_sector_size"] != 512:
        raise HILError(f"{where} logical sector size must be 512")
    if identity["removable"] is not True or identity["read_only"] is not False:
        raise HILError(f"{where} target safety state is invalid")
    return value


def _run_step(
    root: Path,
    step: dict[str, Any],
    artifacts: dict[str, Path],
    trace: Path,
    flash_target: str,
) -> dict[str, Any]:
    command = _expand_command(
        step["command"], artifacts, trace, flash_target
    )
    try:
        result = subprocess.run(
            command,
            cwd=root,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=step.get("timeout_s", 300),
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise HILError(f"step {step['name']} failed: {error}") from error
    record = _result_record(step["name"], result)
    if step.get("capture_flash_attestation") and result.returncode == 0:
        try:
            value = json.loads(result.stdout.decode("utf-8"))
        except (UnicodeError, json.JSONDecodeError) as error:
            raise HILError(
                f"step {step['name']} returned invalid flash attestation: "
                f"{error}"
            ) from error
        media = artifacts["media"]
        record["flash_attestation"] = _validate_flash_attestation(
            value,
            _sha256(media),
            media.stat().st_size,
            media,
            flash_target,
        )
    if step.get("capture_stdout"):
        try:
            trace.write_bytes(result.stdout)
        except OSError as error:
            raise HILError(f"cannot write hardware trace: {error}") from error
    return record


def _check_trace(
    trace: Path,
    plan: dict[str, Any],
    artifacts: dict[str, str],
) -> list[dict[str, Any]]:
    records = boot_trace.load_trace(trace)
    header = records[0]
    if header["producer"] != "hardware":
        raise HILError("trace producer must be 'hardware'")
    if header["platform"] != plan["platform"]:
        raise HILError("trace platform does not match plan")
    if header["artifacts"]["board_revision"] != plan["board_revision"]:
        raise HILError("trace board_revision does not match plan")
    for name, digest in artifacts.items():
        if header["artifacts"].get(f"{name}_sha256") != digest:
            raise HILError(f"trace {name}_sha256 does not match artifact")
    return records


def _atomic_report(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    data = (json.dumps(value, indent=2, sort_keys=True) + "\n").encode()
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            dir=path.parent, prefix=f".{path.name}.", delete=False
        ) as stream:
            temporary = Path(stream.name)
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    except OSError as error:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
        raise HILError(f"cannot write report {path}: {error}") from error


def run_plan(path: Path, report_path: Path) -> dict[str, Any]:
    plan = load_plan(path)
    root = path.resolve().parent
    trace, report_path, isolated_artifacts = validate_plan_paths(
        path, report_path, plan
    )
    verified, artifact_paths = _verify_artifacts(root, plan["artifacts"])
    if artifact_paths != isolated_artifacts:
        raise HILError("artifact paths changed during HIL preflight")
    report: dict[str, Any] = {
        "schema": REPORT_SCHEMA,
        "version": VERSION,
        "id": plan["id"],
        "platform": plan["platform"],
        "board_revision": plan["board_revision"],
        "flash_target": plan["flash_target"],
        "artifacts": verified,
        "steps": [],
        "cleanup": [],
        "success": False,
    }
    error_message: str | None = None

    try:
        trace.parent.mkdir(parents=True, exist_ok=True)
        trace.unlink(missing_ok=True)
        for step in plan["steps"]:
            record = _run_step(
                root, step, artifact_paths, trace, plan["flash_target"]
            )
            report["steps"].append(record)
            if record["returncode"]:
                raise HILError(
                    f"step {step['name']} exited with status "
                    f"{record['returncode']}"
                )
        records = _check_trace(trace, plan, verified)
        report["trace"] = {
            "path": str(trace),
            "sha256": _sha256(trace),
            "records": len(records),
        }
        report["success"] = True
    except (HILError, boot_trace.TraceError) as error:
        error_message = str(error)
        report["error"] = error_message
    finally:
        for step in plan["cleanup"]:
            try:
                record = _run_step(
                    root, step, artifact_paths, trace,
                    plan["flash_target"],
                )
                report["cleanup"].append(record)
                if record["returncode"]:
                    error = HILError(
                        f"step {step['name']} exited with status "
                        f"{record['returncode']}"
                    )
                    record["error"] = str(error)
                    raise error
            except HILError as error:
                if (
                    not report["cleanup"]
                    or report["cleanup"][-1].get("name") != step["name"]
                ):
                    report["cleanup"].append(
                        {"name": step["name"], "error": str(error)}
                    )
                report["success"] = False
                if error_message is None:
                    error_message = str(error)
                    report["error"] = error_message

    _atomic_report(report_path, report)
    if error_message is not None:
        raise HILError(error_message)
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("plan", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        run_plan(args.plan, args.output)
        return 0
    except HILError as error:
        print(f"raspi-hil-orchestrator: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
