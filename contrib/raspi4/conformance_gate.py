#!/usr/bin/env python3
#
# Raspberry Pi 4 / CM4 behavioral conformance gate
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Run pinned hardware and QEMU captures and compare their boot contracts."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from typing import Any

import boot_trace
import hil_orchestrator


SCHEMA = "qemu.raspi.conformance-manifest"
VERSION = 2
ARTIFACTS = {"eeprom", "firmware", "media", "otp"}
EVENT_CONTRACT_KEYS = {"phase", "event", "source", "outcome"}


class GateError(RuntimeError):
    pass


def _keys(value: dict[str, Any], allowed: set[str], where: str) -> None:
    unknown = value.keys() - allowed
    if unknown:
        raise GateError(f"{where}: unknown {', '.join(sorted(unknown))}")


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
        raise GateError(f"{path}: {error.strerror}") from error
    return digest.hexdigest()


def load_manifest(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise GateError(f"{path}: {error}") from error
    if not isinstance(value, dict):
        raise GateError("manifest must be an object")
    _keys(value, {"schema", "version", "cases"}, "manifest")
    if value.get("schema") != SCHEMA or value.get("version") != VERSION:
        raise GateError(f"expected {SCHEMA!r} version {VERSION}")
    cases = value.get("cases")
    if not isinstance(cases, list) or not cases:
        raise GateError("manifest cases must be a non-empty array")
    return value


def _validate_producer(
    value: Any, where: str, producer: str
) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise GateError(f"{where} must be an object")
    _keys(
        value,
        {
            "trace",
            "command",
            "timeout_s",
            "capture_stdout",
            "hil_plan",
            "hil_report",
        },
        where,
    )
    if not isinstance(value.get("trace"), str) or not value["trace"]:
        raise GateError(f"{where}.trace must be a non-empty string")
    command = value.get("command")
    if command is not None and (
        not isinstance(command, list)
        or not command
        or any(not isinstance(item, str) or not item for item in command)
    ):
        raise GateError(f"{where}.command must be a non-empty argv array")
    timeout = value.get("timeout_s", 300)
    if type(timeout) is not int or timeout <= 0:
        raise GateError(f"{where}.timeout_s must be a positive integer")
    capture_stdout = value.get("capture_stdout", False)
    if type(capture_stdout) is not bool:
        raise GateError(f"{where}.capture_stdout must be boolean")
    if capture_stdout and command is None:
        raise GateError(f"{where}.capture_stdout requires command")
    hil_plan = value.get("hil_plan")
    hil_report = value.get("hil_report")
    if (hil_plan is None) != (hil_report is None):
        raise GateError(f"{where}.hil_plan and hil_report must be paired")
    if hil_plan is not None:
        if producer != "hardware":
            raise GateError(f"{where}: HIL plans are hardware-only")
        if command is not None or capture_stdout:
            raise GateError(
                f"{where}: HIL plans cannot be combined with command capture"
            )
        if not isinstance(hil_plan, str) or not hil_plan:
            raise GateError(f"{where}.hil_plan must be a non-empty path")
        if not isinstance(hil_report, str) or not hil_report:
            raise GateError(f"{where}.hil_report must be a non-empty path")
    return value


def _validate_hil_binding(
    root: Path,
    value: dict[str, Any],
    where: str,
    case: dict[str, Any],
    artifacts: dict[str, str],
) -> tuple[Path, Path, Path]:
    plan_input = _path(root, value["hil_plan"])
    report_input = _path(root, value["hil_report"])
    trace_input = _path(root, value["trace"])
    plan_path = plan_input.resolve()
    report_path = report_input.resolve()
    trace = trace_input.resolve()
    try:
        plan = hil_orchestrator.load_plan(plan_input)
        isolated_trace, isolated_report, isolated_artifacts = (
            hil_orchestrator.validate_plan_paths(
                plan_input, report_input, plan
            )
        )
    except hil_orchestrator.HILError as error:
        raise GateError(f"{where}: {error}") from error
    if isolated_trace != trace or isolated_report != report_path:
        raise GateError(f"{where}: HIL output paths changed during preflight")
    if plan["platform"] != case["platform"]:
        raise GateError(f"{where}: HIL plan platform does not match case")
    if plan["board_revision"] != case["board_revision"]:
        raise GateError(
            f"{where}: HIL plan board_revision does not match case"
        )
    if plan["flash_target"] != case.get("flash_target"):
        raise GateError(f"{where}: HIL plan flash_target does not match case")
    plan_trace = _path(plan_path.parent, plan["trace"]).resolve()
    if plan_trace != trace:
        raise GateError(f"{where}: HIL plan trace does not match producer")
    if plan["artifacts"].keys() != case["artifacts"].keys():
        raise GateError(f"{where}: HIL plan artifact set does not match case")
    for name, digest in artifacts.items():
        plan_artifact = plan["artifacts"][name]
        if plan_artifact["sha256"] != digest:
            raise GateError(
                f"{where}: HIL plan {name} SHA-256 does not match case"
            )
        plan_file = _path(plan_path.parent, plan_artifact["path"]).resolve()
        case_file = _path(root, case["artifacts"][name]["path"]).resolve()
        if plan_file != case_file or isolated_artifacts[name] != case_file:
            raise GateError(
                f"{where}: HIL plan {name} path does not match case"
            )
    qemu_trace = _path(root, case["qemu"]["trace"]).resolve()
    protected_paths = {
        plan_path,
        report_path,
        trace,
        *isolated_artifacts.values(),
    }
    if qemu_trace in protected_paths:
        raise GateError(f"{where}: QEMU trace collides with a HIL path")
    if qemu_trace.exists():
        for protected in protected_paths:
            if protected.exists() and qemu_trace.samefile(protected):
                raise GateError(
                    f"{where}: QEMU trace aliases a protected HIL file"
                )
    return plan_path, report_path, trace


def _validate_hil_report(
    report_path: Path,
    report: dict[str, Any],
    trace: Path,
    case: dict[str, Any],
    artifacts: dict[str, str],
    where: str,
) -> None:
    try:
        stored = json.loads(report_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise GateError(
            f"{where}: cannot read retained HIL report: {error}"
        ) from error
    if stored != report:
        raise GateError(f"{where}: retained HIL report differs from result")
    if (
        report.get("schema") != hil_orchestrator.REPORT_SCHEMA
        or report.get("version") != hil_orchestrator.VERSION
        or report.get("success") is not True
    ):
        raise GateError(f"{where}: retained HIL report is not successful")
    if (
        report.get("platform") != case["platform"]
        or report.get("board_revision") != case["board_revision"]
        or report.get("flash_target") != case.get("flash_target")
        or report.get("artifacts") != artifacts
    ):
        raise GateError(f"{where}: retained HIL report identity mismatch")
    steps = report.get("steps")
    expected_steps = hil_orchestrator.SEQUENCES[case["platform"]]
    if (
        not isinstance(steps, list)
        or tuple(
            step.get("name") if isinstance(step, dict) else None
            for step in steps
        ) != expected_steps
    ):
        raise GateError(f"{where}: retained HIL step evidence mismatch")
    flash_name = next(
        name for name in expected_steps
        if name in hil_orchestrator.FLASH_STEPS
    )
    flash_record = next(step for step in steps if step["name"] == flash_name)
    media_path = _path(
        report_path.parent, case["artifacts"]["media"]["path"]
    ).resolve()
    try:
        hil_orchestrator._validate_flash_attestation(
            flash_record.get("flash_attestation"),
            artifacts["media"],
            media_path.stat().st_size,
            media_path,
            case.get("flash_target"),
        )
    except (OSError, hil_orchestrator.HILError) as error:
        raise GateError(
            f"{where}: retained HIL flash evidence mismatch: {error}"
        ) from error
    trace_report = report.get("trace")
    if not isinstance(trace_report, dict):
        raise GateError(f"{where}: retained HIL report has no trace record")
    try:
        records = boot_trace.load_trace(trace)
    except boot_trace.TraceError as error:
        raise GateError(f"{where}: {error}") from error
    if (
        trace_report.get("path") != str(trace)
        or trace_report.get("sha256") != _sha256(trace)
        or trace_report.get("records") != len(records)
    ):
        raise GateError(f"{where}: retained HIL trace evidence mismatch")


def _run_hil_producer(
    root: Path,
    value: dict[str, Any],
    where: str,
    case: dict[str, Any],
    artifacts: dict[str, str],
) -> Path:
    plan_path, report_path, trace = _validate_hil_binding(
        root, value, where, case, artifacts
    )
    try:
        report = hil_orchestrator.run_plan(plan_path, report_path)
    except hil_orchestrator.HILError as error:
        raise GateError(f"{where}: {error}") from error
    _validate_hil_report(
        report_path, report, trace, case, artifacts, where
    )
    return trace


def _run_producer(
    root: Path,
    value: dict[str, Any],
    where: str,
    producer: str,
    case: dict[str, Any],
    artifacts: dict[str, str],
) -> Path:
    if "hil_plan" in value:
        return _run_hil_producer(root, value, where, case, artifacts)
    trace_input = _path(root, value["trace"])
    if trace_input.is_symlink():
        raise GateError(f"{where}.trace became a symlink after preflight")
    trace = trace_input.resolve()
    for name, artifact in case["artifacts"].items():
        artifact_path = _path(root, artifact["path"]).resolve()
        if trace == artifact_path or (
            trace.exists()
            and artifact_path.exists()
            and trace.samefile(artifact_path)
        ):
            raise GateError(
                f"{where}.trace aliases protected artifact {name}"
            )
    command = value.get("command")
    if command is not None:
        try:
            trace.unlink(missing_ok=True)
            if value.get("capture_stdout", False):
                with trace.open("wb") as output:
                    result = subprocess.run(
                        command, cwd=root, check=False, stdout=output,
                        timeout=value.get("timeout_s", 300),
                    )
            else:
                result = subprocess.run(
                    command, cwd=root, check=False,
                    timeout=value.get("timeout_s", 300),
                )
        except (OSError, subprocess.TimeoutExpired) as error:
            raise GateError(f"{where} capture failed: {error}") from error
        if result.returncode:
            raise GateError(
                f"{where} capture exited with status {result.returncode}"
            )
    if not trace.is_file():
        raise GateError(f"{where} did not produce {trace}")
    return trace


def _check_artifacts(
    root: Path, case: dict[str, Any]
) -> dict[str, str]:
    artifacts = case.get("artifacts")
    if not isinstance(artifacts, dict) or not artifacts:
        raise GateError(f"case {case['id']}: artifacts must be non-empty")
    _keys(artifacts, ARTIFACTS, f"case {case['id']} artifacts")
    verified: dict[str, str] = {}
    for name, value in sorted(artifacts.items()):
        where = f"case {case['id']} artifact {name}"
        if not isinstance(value, dict):
            raise GateError(f"{where} must be an object")
        _keys(value, {"path", "sha256"}, where)
        path_value = value.get("path")
        expected = value.get("sha256")
        if not isinstance(path_value, str) or not path_value:
            raise GateError(f"{where}.path must be a non-empty string")
        if not isinstance(expected, str) or not boot_trace.SHA256_RE.fullmatch(
            expected
        ):
            raise GateError(f"{where}.sha256 must be a lowercase SHA-256")
        actual = _sha256(_path(root, path_value))
        if actual != expected:
            raise GateError(
                f"{where}: SHA-256 mismatch: expected {expected}, got {actual}"
            )
        verified[name] = actual
    return verified


def _validate_case_paths(
    root: Path,
    case: dict[str, Any],
    hardware: dict[str, Any],
    qemu: dict[str, Any],
) -> None:
    case_id = case["id"]
    candidates = {
        "hardware trace": _path(root, hardware["trace"]).absolute(),
        "QEMU trace": _path(root, qemu["trace"]).absolute(),
        **{
            f"artifact {name}": _path(root, value["path"]).absolute()
            for name, value in case["artifacts"].items()
        },
    }
    for name, candidate in candidates.items():
        if candidate.is_symlink():
            raise GateError(
                f"case {case_id} {name} path must not be a symlink"
            )

    resolved = {
        name: candidate.resolve() for name, candidate in candidates.items()
    }
    seen: dict[Path, str] = {}
    for name, candidate in resolved.items():
        previous = seen.get(candidate)
        if previous is not None:
            raise GateError(
                f"case {case_id} path collision: "
                f"{previous} and {name} resolve to {candidate}"
            )
        seen[candidate] = name

    seen_files: dict[tuple[int, int], str] = {}
    for name, candidate in candidates.items():
        if not candidate.exists():
            continue
        if not candidate.is_file():
            raise GateError(
                f"case {case_id} {name} path must be a regular file"
            )
        status = candidate.stat()
        identity = (status.st_dev, status.st_ino)
        previous = seen_files.get(identity)
        if previous is not None:
            raise GateError(
                f"case {case_id} path collision: "
                f"{previous} and {name} are the same file"
            )
        seen_files[identity] = name


def _check_header(
    records: list[dict[str, Any]], case: dict[str, Any], producer: str,
    artifacts: dict[str, str],
) -> None:
    header = records[0]
    where = f"case {case['id']} {producer} header"
    if header["producer"] != producer:
        raise GateError(f"{where}: producer must be {producer!r}")
    if header["platform"] != case["platform"]:
        raise GateError(f"{where}: platform does not match manifest")
    if header["artifacts"]["board_revision"] != case["board_revision"]:
        raise GateError(f"{where}: board_revision does not match manifest")
    for name, digest in artifacts.items():
        key = f"{name}_sha256"
        if header["artifacts"].get(key) != digest:
            raise GateError(f"{where}: {key} does not match verified artifact")


def _validate_event_contract(value: Any, case_id: str) -> list[dict[str, str]]:
    where = f"case {case_id} event_contract"
    if not isinstance(value, list) or not value:
        raise GateError(f"{where} must be a non-empty array")
    contract = []
    for index, item in enumerate(value):
        item_where = f"{where}[{index}]"
        if not isinstance(item, dict):
            raise GateError(f"{item_where} must be an object")
        _keys(item, EVENT_CONTRACT_KEYS, item_where)
        if not {"phase", "event"} <= item.keys():
            raise GateError(f"{item_where} requires phase and event")
        if item["phase"] not in boot_trace.PHASES:
            raise GateError(f"{item_where}.phase is invalid")
        event = item["event"]
        if not isinstance(event, str) or not boot_trace.EVENT_RE.fullmatch(
            event
        ):
            raise GateError(f"{item_where}.event is invalid")
        source = item.get("source")
        if source is not None and source not in boot_trace.SOURCES:
            raise GateError(f"{item_where}.source is invalid")
        outcome = item.get("outcome")
        if outcome is not None and outcome not in boot_trace.OUTCOMES:
            raise GateError(f"{item_where}.outcome is invalid")
        contract.append(dict(item))
    return contract


def _event_contract_differences(
    records: list[dict[str, Any]], contract: list[dict[str, str]]
) -> list[str]:
    events = records[1:]
    differences = []
    if len(events) != len(contract):
        differences.append(
            f"event count: expected {len(contract)}, actual {len(events)}"
        )
    for index, (expected, actual) in enumerate(zip(contract, events)):
        observed = {key: actual.get(key) for key in expected}
        if observed != expected:
            differences.append(
                f"event {index}: expected "
                f"{json.dumps(expected, sort_keys=True)}, actual "
                f"{json.dumps(observed, sort_keys=True)}"
            )
    return differences


def _prepare_case(root: Path, case: Any) -> dict[str, Any]:
    if not isinstance(case, dict):
        raise GateError("case must be an object")
    _keys(
        case,
        {"id", "platform", "board_revision", "artifacts", "hardware",
         "qemu", "event_contract", "ignore_data_keys", "flash_target"},
        "case",
    )
    case_id = case.get("id")
    if not isinstance(case_id, str) or not case_id:
        raise GateError("case.id must be a non-empty string")
    if case.get("platform") not in boot_trace.PLATFORMS:
        raise GateError(f"case {case_id}: invalid platform")
    revision = case.get("board_revision")
    if not isinstance(revision, str) or not boot_trace.REVISION_RE.fullmatch(
        revision
    ):
        raise GateError(f"case {case_id}: invalid board_revision")
    event_contract = _validate_event_contract(
        case.get("event_contract"), case_id
    )
    ignored = case.get("ignore_data_keys", [])
    if not isinstance(ignored, list) or any(
        not isinstance(item, str) or not item for item in ignored
    ):
        raise GateError(f"case {case_id}: ignore_data_keys must be strings")
    hardware = _validate_producer(
        case.get("hardware"), f"case {case_id} hardware", "hardware"
    )
    qemu = _validate_producer(
        case.get("qemu"), f"case {case_id} qemu", "qemu"
    )
    if "hil_plan" in hardware:
        try:
            hil_orchestrator._validate_flash_target(
                case.get("flash_target"), f"case {case_id}.flash_target"
            )
        except hil_orchestrator.HILError as error:
            raise GateError(str(error)) from error
    elif case.get("flash_target") is not None:
        raise GateError(
            f"case {case_id}.flash_target requires a HIL plan"
        )
    artifacts = _check_artifacts(root, case)
    _validate_case_paths(root, case, hardware, qemu)
    if "hil_plan" in hardware:
        _validate_hil_binding(
            root,
            hardware,
            f"case {case_id} hardware",
            case,
            artifacts,
        )
    return {
        "case": case,
        "hardware": hardware,
        "qemu": qemu,
        "artifacts": artifacts,
        "event_contract": event_contract,
    }


def _run_prepared(root: Path, prepared: dict[str, Any]) -> dict[str, Any]:
    case = prepared["case"]
    case_id = case["id"]
    hardware = prepared["hardware"]
    qemu = prepared["qemu"]
    artifacts = prepared["artifacts"]
    event_contract = prepared["event_contract"]
    hardware_trace = boot_trace.load_trace(
        _run_producer(
            root,
            hardware,
            f"case {case_id} hardware",
            "hardware",
            case,
            artifacts,
        )
    )
    _check_header(hardware_trace, case, "hardware", artifacts)
    hardware_contract_differences = _event_contract_differences(
        hardware_trace, event_contract
    )
    if hardware_contract_differences:
        raise GateError(
            f"case {case_id} hardware event contract: "
            + "; ".join(hardware_contract_differences)
        )
    qemu_trace = boot_trace.load_trace(
        _run_producer(
            root,
            qemu,
            f"case {case_id} qemu",
            "qemu",
            case,
            artifacts,
        )
    )
    _check_header(qemu_trace, case, "qemu", artifacts)
    ignored_keys = (
        boot_trace.DEFAULT_VOLATILE_DATA_KEYS
        | frozenset(case.get("ignore_data_keys", []))
    )
    differences = [
        f"event contract: {item}"
        for item in _event_contract_differences(qemu_trace, event_contract)
    ]
    differences.extend(boot_trace.compare_traces(
        hardware_trace, qemu_trace, ignored_keys
    ))
    return {"id": case_id, "equal": not differences,
            "differences": differences}


def run_case(root: Path, case: Any) -> dict[str, Any]:
    return _run_prepared(root, _prepare_case(root, case))


def run_manifest(path: Path) -> dict[str, Any]:
    manifest = load_manifest(path)
    root = path.resolve().parent
    seen: set[str] = set()
    prepared_cases = []
    for case in manifest["cases"]:
        prepared = _prepare_case(root, case)
        case_id = prepared["case"]["id"]
        if case_id in seen:
            raise GateError(f"duplicate case id {case_id!r}")
        seen.add(case_id)
        prepared_cases.append(prepared)
    results = [_run_prepared(root, prepared) for prepared in prepared_cases]
    return {"schema": SCHEMA, "version": VERSION,
            "equal": all(item["equal"] for item in results),
            "cases": results}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    args = parser.parse_args(argv)
    try:
        result = run_manifest(args.manifest)
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0 if result["equal"] else 1
    except (GateError, boot_trace.TraceError) as error:
        print(f"raspi-conformance-gate: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
