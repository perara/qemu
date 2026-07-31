#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify retained Raspberry Pi evidence paths against their declared hashes."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any


class EvidenceError(ValueError):
    pass


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_reference(
    summary_path: Path,
    record: dict[str, Any],
    path_key: str,
    hash_key: str,
) -> None:
    relative = record.get(path_key)
    expected = record.get(hash_key)
    if not isinstance(relative, str) or not relative:
        raise EvidenceError(f"{summary_path}: missing {path_key}")
    if not isinstance(expected, str) or len(expected) != 64:
        raise EvidenceError(f"{summary_path}: invalid {hash_key}")
    target = (summary_path.parent / relative).resolve()
    if not target.is_file():
        raise EvidenceError(f"{summary_path}: missing retained file {target}")
    actual = sha256_file(target)
    if actual != expected:
        raise EvidenceError(
            f"{summary_path}: {target} SHA-256 {actual} != {expected}"
        )


def verify_summary(summary_path: Path) -> int:
    summary = json.loads(summary_path.read_text(encoding="utf-8"))
    if summary.get("status") != "complete":
        raise EvidenceError(f"{summary_path}: status is not complete")

    verified = 0
    if "manifest_path" in summary:
        verify_reference(
            summary_path, summary, "manifest_path", "manifest_sha256"
        )
        verified += 1
    if "supervisor_report_path" in summary:
        verify_reference(
            summary_path,
            summary,
            "supervisor_report_path",
            "supervisor_report_sha256",
        )
        verified += 1

    for model in summary.get("models", []):
        verify_reference(
            summary_path,
            model,
            "supervisor_report_path",
            "supervisor_report_sha256",
        )
        verified += 1

    if verified == 0:
        raise EvidenceError(f"{summary_path}: no retained references")
    return verified


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("summaries", type=Path, nargs="+")
    args = parser.parse_args()
    total = 0
    try:
        for summary in args.summaries:
            count = verify_summary(summary.resolve())
            total += count
            print(f"PASS {summary}: {count} retained reference(s)")
    except (EvidenceError, OSError, json.JSONDecodeError) as error:
        print(f"FAIL: {error}")
        return 1
    print(f"PASS evidence integrity: {total} retained reference(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
