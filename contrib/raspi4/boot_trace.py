#!/usr/bin/env python3
#
# Raspberry Pi 4 / CM4 boot trace validator and comparator
#
# Copyright (c) 2026 Per-Arne and contributors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Validate and compare ordered Raspberry Pi boot traces.

The wire format is JSON Lines.  The first record pins the platform and
artifacts; subsequent records describe software-visible boot transitions.
Timestamps and explicitly documented volatile identifiers are discarded for
differential comparison, while event ordering and stable payloads remain exact.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys
from typing import Any, Iterable


SCHEMA_NAME = "qemu.raspi.boot-trace"
SCHEMA_VERSION = 1
PLATFORMS = {"raspi4b", "cm4"}
PRODUCERS = {"qemu", "hardware"}
PHASES = {
    "reset",
    "rom",
    "recovery",
    "eeprom",
    "boot-source",
    "firmware",
    "handoff",
    "kernel",
    "health",
    "fault",
}
SOURCES = {
    "sd-card-detect",
    "sd-card",
    "emmc",
    "network",
    "rpiboot",
    "usb-msd",
    "bcm-usb-msd",
    "nvme",
    "http",
    "stop",
    "restart",
}
OUTCOMES = {
    "start",
    "success",
    "failure",
    "timeout",
    "unsupported",
    "wait",
    "retry",
    "watchdog",
    "stop",
    "restart",
}
EVENT_RE = re.compile(r"^[a-z][a-z0-9-]*(\.[a-z][a-z0-9-]*)+$")
QEMU_EVENT_RE = re.compile(
    r"raspi4b_boot_event "
    r"phase=(?P<phase>\S+) event=(?P<event>\S+) "
    r"source=(?P<source>\S+) outcome=(?P<outcome>\S+) "
    r"value=0x(?P<value>[0-9a-fA-F]+)"
)
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
REVISION_RE = re.compile(r"^(0x)?[0-9a-fA-F]+$")
DEFAULT_VOLATILE_DATA_KEYS = frozenset(
    {"elapsed_us", "mac", "serial", "timestamp", "timestamp_us"}
)
HEADER_KEYS = {
    "record",
    "schema",
    "version",
    "platform",
    "producer",
    "capture_id",
    "started_utc",
    "artifacts",
}
ARTIFACT_KEYS = {
    "board_revision",
    "eeprom_release",
    "eeprom_sha256",
    "firmware_release",
    "firmware_sha256",
    "media_sha256",
    "otp_sha256",
}
EVENT_KEYS = {
    "record",
    "seq",
    "time_us",
    "phase",
    "event",
    "source",
    "outcome",
    "data",
}


class TraceError(RuntimeError):
    """Raised when a trace violates the versioned boot contract."""


def _require_keys(
    record: dict[str, Any], required: set[str], where: str
) -> None:
    missing = required - record.keys()
    if missing:
        raise TraceError(f"{where}: missing {', '.join(sorted(missing))}")


def _reject_unknown_keys(
    record: dict[str, Any], allowed: set[str], where: str
) -> None:
    unknown = record.keys() - allowed
    if unknown:
        raise TraceError(f"{where}: unknown {', '.join(sorted(unknown))}")


def _validate_header(header: dict[str, Any]) -> None:
    where = "line 1 header"
    _require_keys(
        header,
        {"record", "schema", "version", "platform", "producer", "artifacts"},
        where,
    )
    _reject_unknown_keys(header, HEADER_KEYS, where)
    if header["record"] != "header":
        raise TraceError(f"{where}: first record must be a header")
    if header["schema"] != SCHEMA_NAME or header["version"] != SCHEMA_VERSION:
        raise TraceError(
            f"{where}: expected {SCHEMA_NAME!r} version {SCHEMA_VERSION}"
        )
    if header["platform"] not in PLATFORMS:
        raise TraceError(
            f"{where}: unsupported platform {header['platform']!r}"
        )
    if header["producer"] not in PRODUCERS:
        raise TraceError(
            f"{where}: unsupported producer {header['producer']!r}"
        )

    artifacts = header["artifacts"]
    if not isinstance(artifacts, dict):
        raise TraceError(f"{where}: artifacts must be an object")
    _require_keys(artifacts, {"board_revision"}, f"{where} artifacts")
    _reject_unknown_keys(artifacts, ARTIFACT_KEYS, f"{where} artifacts")
    revision = artifacts["board_revision"]
    if not isinstance(revision, str) or not REVISION_RE.fullmatch(revision):
        raise TraceError(f"{where}: invalid board_revision")
    for key, value in artifacts.items():
        if key.endswith("_sha256") and (
            not isinstance(value, str) or not SHA256_RE.fullmatch(value)
        ):
            raise TraceError(f"{where}: {key} must be a lowercase SHA-256")


def _validate_event(
    event: dict[str, Any], expected_seq: int, line: int
) -> None:
    where = f"line {line} event"
    _require_keys(event, {"record", "seq", "phase", "event"}, where)
    _reject_unknown_keys(event, EVENT_KEYS, where)
    if event["record"] != "event":
        raise TraceError(f"{where}: expected an event record")
    if type(event["seq"]) is not int or event["seq"] != expected_seq:
        raise TraceError(f"{where}: expected seq {expected_seq}")
    if event["phase"] not in PHASES:
        raise TraceError(f"{where}: unknown phase {event['phase']!r}")
    if (
        not isinstance(event["event"], str)
        or not EVENT_RE.fullmatch(event["event"])
    ):
        raise TraceError(f"{where}: invalid dotted event name")
    if "time_us" in event and (
        type(event["time_us"]) is not int or event["time_us"] < 0
    ):
        raise TraceError(f"{where}: time_us must be a non-negative integer")
    if "source" in event and event["source"] not in SOURCES:
        raise TraceError(f"{where}: unknown boot source {event['source']!r}")
    if "outcome" in event and event["outcome"] not in OUTCOMES:
        raise TraceError(f"{where}: unknown outcome {event['outcome']!r}")
    if "data" in event and not isinstance(event["data"], dict):
        raise TraceError(f"{where}: data must be an object")


def load_trace(path: Path) -> list[dict[str, Any]]:
    """Load and validate a version-1 JSON Lines trace."""

    records: list[dict[str, Any]] = []
    try:
        with path.open("r", encoding="utf-8") as stream:
            for line_number, line in enumerate(stream, 1):
                if not line.strip():
                    raise TraceError(
                        f"line {line_number}: blank lines are forbidden"
                    )
                try:
                    record = json.loads(line)
                except json.JSONDecodeError as error:
                    raise TraceError(
                        f"line {line_number}: {error.msg}"
                    ) from error
                if not isinstance(record, dict):
                    raise TraceError(
                        f"line {line_number}: record must be an object"
                    )
                records.append(record)
    except OSError as error:
        raise TraceError(f"{path}: {error.strerror}") from error

    if not records:
        raise TraceError("trace is empty")
    _validate_header(records[0])
    for seq, event in enumerate(records[1:]):
        _validate_event(event, seq, seq + 2)
    return records


def _strip_volatile(value: Any, ignored_data_keys: frozenset[str]) -> Any:
    if isinstance(value, dict):
        return {
            key: _strip_volatile(item, ignored_data_keys)
            for key, item in sorted(value.items())
            if key not in ignored_data_keys
        }
    if isinstance(value, list):
        return [_strip_volatile(item, ignored_data_keys) for item in value]
    return value


def normalize_trace(
    records: Iterable[dict[str, Any]],
    ignored_data_keys: frozenset[str] = DEFAULT_VOLATILE_DATA_KEYS,
) -> list[dict[str, Any]]:
    """Remove producer-only metadata and documented volatile observations."""

    normalized: list[dict[str, Any]] = []
    for index, record in enumerate(records):
        item = dict(record)
        if index == 0:
            item.pop("producer", None)
            item.pop("capture_id", None)
            item.pop("started_utc", None)
        else:
            item.pop("time_us", None)
            if "data" in item:
                item["data"] = _strip_volatile(item["data"], ignored_data_keys)
                if not item["data"]:
                    item.pop("data")
        normalized.append(item)
    return normalized


def compare_traces(
    expected: Iterable[dict[str, Any]],
    actual: Iterable[dict[str, Any]],
    ignored_data_keys: frozenset[str] = DEFAULT_VOLATILE_DATA_KEYS,
) -> list[str]:
    """Return ordered semantic differences between two validated traces."""

    left = normalize_trace(expected, ignored_data_keys)
    right = normalize_trace(actual, ignored_data_keys)
    differences: list[str] = []
    if len(left) != len(right):
        differences.append(
            f"record count: expected {len(left)}, actual {len(right)}"
        )
    for index, (expected_record, actual_record) in enumerate(zip(left, right)):
        if expected_record != actual_record:
            label = "header" if index == 0 else f"event seq {index - 1}"
            differences.append(
                f"{label}: expected "
                f"{json.dumps(expected_record, sort_keys=True)}, actual "
                f"{json.dumps(actual_record, sort_keys=True)}"
            )
    return differences


def convert_qemu_trace(
    lines: Iterable[str],
    *,
    platform: str,
    board_revision: str,
    eeprom_sha256: str | None = None,
    media_sha256: str | None = None,
    otp_sha256: str | None = None,
) -> list[dict[str, Any]]:
    """Convert ``raspi4b_boot_event`` log lines to contract records."""

    artifacts = {"board_revision": board_revision}
    if eeprom_sha256:
        artifacts["eeprom_sha256"] = eeprom_sha256
    if media_sha256:
        artifacts["media_sha256"] = media_sha256
    if otp_sha256:
        artifacts["otp_sha256"] = otp_sha256
    records: list[dict[str, Any]] = [
        {
            "record": "header",
            "schema": SCHEMA_NAME,
            "version": SCHEMA_VERSION,
            "platform": platform,
            "producer": "qemu",
            "artifacts": artifacts,
        }
    ]

    for line in lines:
        match = QEMU_EVENT_RE.search(line)
        if not match:
            continue
        source = match.group("source")
        event: dict[str, Any] = {
            "record": "event",
            "seq": len(records) - 1,
            "phase": match.group("phase"),
            "event": match.group("event"),
            "outcome": match.group("outcome"),
            "data": {"value": f"0x{int(match.group('value'), 16):x}"},
        }
        if source in SOURCES:
            event["source"] = source
        elif source != "none":
            event["data"]["requested_source"] = source
        records.append(event)

    if len(records) == 1:
        raise TraceError("QEMU log contains no raspi4b_boot_event records")
    _validate_header(records[0])
    for seq, event in enumerate(records[1:]):
        _validate_event(event, seq, seq + 2)
    return records


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    validate = subparsers.add_parser("validate", help="validate one trace")
    validate.add_argument("trace", type=Path)

    normalize = subparsers.add_parser(
        "normalize", help="write canonical records to standard output"
    )
    normalize.add_argument("trace", type=Path)

    compare = subparsers.add_parser(
        "compare", help="compare an expected hardware trace with a QEMU trace"
    )
    compare.add_argument("expected", type=Path)
    compare.add_argument("actual", type=Path)
    compare.add_argument(
        "--ignore-data-key",
        action="append",
        default=[],
        help="additional volatile data key to ignore (repeatable)",
    )

    from_qemu = subparsers.add_parser(
        "from-qemu", help="convert raspi4b QEMU trace output to JSON Lines"
    )
    from_qemu.add_argument("log", type=Path)
    from_qemu.add_argument("--platform", choices=sorted(PLATFORMS),
                           default="raspi4b")
    from_qemu.add_argument("--board-revision", required=True)
    from_qemu.add_argument("--eeprom-sha256")
    from_qemu.add_argument("--media-sha256")
    from_qemu.add_argument("--otp-sha256")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    try:
        if args.command == "validate":
            records = load_trace(args.trace)
            print(json.dumps({"records": len(records), "valid": True}))
            return 0
        if args.command == "normalize":
            for record in normalize_trace(load_trace(args.trace)):
                print(json.dumps(record, sort_keys=True, separators=(",", ":")))
            return 0
        if args.command == "from-qemu":
            try:
                lines = args.log.read_text(encoding="utf-8").splitlines()
            except OSError as error:
                raise TraceError(f"{args.log}: {error.strerror}") from error
            records = convert_qemu_trace(
                lines,
                platform=args.platform,
                board_revision=args.board_revision,
                eeprom_sha256=args.eeprom_sha256,
                media_sha256=args.media_sha256,
                otp_sha256=args.otp_sha256,
            )
            for record in records:
                print(json.dumps(record, sort_keys=True, separators=(",", ":")))
            return 0

        ignored = DEFAULT_VOLATILE_DATA_KEYS | frozenset(args.ignore_data_key)
        differences = compare_traces(
            load_trace(args.expected), load_trace(args.actual), ignored
        )
        if differences:
            result = {"equal": False, "differences": differences}
            print(json.dumps(result, indent=2))
            return 1
        print(json.dumps({"equal": True, "differences": []}))
        return 0
    except (TraceError, ValueError) as error:
        print(f"raspi-boot-trace: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
