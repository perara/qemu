#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Recompute and verify the Raspberry Pi 4 implementation matrix dashboard."""

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import re


STATUS_SCORES = {
    "✅ Full": 1.0,
    "➖ Excluded": 1.0,
    "🟡 Partial": 0.5,
    "🔌 Bridgeable": 0.25,
    "❌ Missing": 0.0,
    "🧪 HIL pending": 1.0,
    "🏁 HIL ready": 1.0,
}
PREFIXES = ("IMG", "SOC", "STO", "BOOT", "USB", "GPIO", "PER", "SYS", "TEST")
WEIGHTS = {
    "IMG": 10.0,
    "SOC": 10.0,
    "STO": 10.0,
    "BOOT": 25.0,
    "USB": 15.0,
    "GPIO": 8.0,
    "PER": 8.0,
    "SYS": 6.0,
    "TEST": 8.0,
}
ROW_RE = re.compile(
    r"^\| (?P<id>(?P<prefix>IMG|SOC|STO|BOOT|USB|GPIO|PER|SYS|TEST)-\d+) "
    r"\|.*?\| (?P<status>"
    + "|".join(re.escape(status) for status in STATUS_SCORES)
    + r") \|"
)
PARTIAL_SCORE_RE = re.compile(
    r"^<!-- matrix-partial-score: "
    r"(?P<id>(?:IMG|SOC|STO|BOOT|USB|GPIO|PER|SYS|TEST)-\d+)="
    r"(?P<score>(?:0(?:\.\d+)?|1(?:\.0+)?)) -->$"
)


class MatrixError(ValueError):
    pass


@dataclass(frozen=True)
class SliceProgress:
    total: int
    statuses: dict[str, int]
    percent: float
    contribution: float


def compute(matrix: Path) -> dict:
    lines = matrix.read_text(encoding="utf-8").splitlines()
    rows: dict[str, tuple[str, str]] = {}
    partial_scores: dict[str, float] = {}
    for line in lines:
        score_match = PARTIAL_SCORE_RE.match(line)
        if score_match:
            row_id = score_match.group("id")
            if row_id in partial_scores:
                raise MatrixError(f"duplicate partial score: {row_id}")
            partial_scores[row_id] = float(score_match.group("score"))

        match = ROW_RE.match(line)
        if not match:
            continue
        row_id = match.group("id")
        if row_id in rows:
            raise MatrixError(f"duplicate capability row: {row_id}")
        rows[row_id] = (match.group("prefix"), match.group("status"))

    if len(rows) != 106:
        raise MatrixError(f"expected 106 capability rows, found {len(rows)}")
    for row_id, score in partial_scores.items():
        if row_id not in rows:
            raise MatrixError(f"partial score has no capability row: {row_id}")
        if rows[row_id][1] != "🟡 Partial":
            raise MatrixError(
                f"partial score belongs to non-Partial row: {row_id}"
            )
        if not 0.0 <= score < 1.0:
            raise MatrixError(f"partial score must be in [0, 1): {row_id}")

    slices = {}
    weighted = 0.0
    total_score = 0.0
    baseline_weighted = 0.0
    hil_pending = 0
    hil_ready = 0
    for prefix in PREFIXES:
        selected = [(row_id, status)
                    for row_id, (row_prefix, status) in rows.items()
                    if row_prefix == prefix]
        selected_statuses = [status for _, status in selected]
        statuses = {
            status: selected_statuses.count(status)
            for status in STATUS_SCORES
        }
        score = sum(partial_scores.get(row_id, STATUS_SCORES[status])
                    for row_id, status in selected)
        percent = score * 100.0 / len(selected)
        contribution = percent * WEIGHTS[prefix] / 100.0
        slices[prefix] = SliceProgress(
            total=len(selected),
            statuses=statuses,
            percent=percent,
            contribution=contribution,
        )
        weighted += contribution
        total_score += score
        pending = statuses["🧪 HIL pending"]
        baseline_weighted -= pending * WEIGHTS[prefix] / len(selected)
        hil_pending += pending
        hil_ready += statuses["🏁 HIL ready"]

    baseline_weighted += weighted
    return {
        "capabilities": len(rows),
        "full": sum(1 for _, status in rows.values() if status == "✅ Full"),
        "excluded": sum(1 for _, status in rows.values()
                        if status == "➖ Excluded"),
        "partial": sum(1 for _, status in rows.values()
                       if status == "🟡 Partial"),
        "bridgeable": sum(1 for _, status in rows.values()
                          if status == "🔌 Bridgeable"),
        "missing": sum(1 for _, status in rows.values()
                       if status == "❌ Missing"),
        "hil_pending": hil_pending,
        "hil_ready": hil_ready,
        "weighted_percent": weighted,
        "unweighted_percent": total_score * 100.0 / len(rows),
        "baseline_weighted_without_hil": baseline_weighted,
        "partial_scores": partial_scores,
        "slices": slices,
    }


def check_dashboard(matrix: Path, report: dict) -> None:
    text = matrix.read_text(encoding="utf-8")
    hil_total = report["hil_pending"] + report["hil_ready"]
    hil_percent = 100.0 * report["hil_ready"] / max(1, hil_total)
    expected = [
        f"**Overall virtualization-pass progress: "
        f"{report['weighted_percent']:.1f}%.**",
        f"**{report['weighted_percent']:.1f}% weighted / "
        f"{report['unweighted_percent']:.1f}% unweighted**",
        f"**{report['baseline_weighted_without_hil']:.1f}% weighted**",
        f"**Deferred HIL-pass readiness: {report['hil_ready']} of "
        f"{hil_total} HIL rows ready ({hil_percent:.1f}%).**",
    ]
    missing = [fragment for fragment in expected if fragment not in text]
    if missing:
        raise MatrixError("dashboard is stale; missing: " + "; ".join(missing))


def serializable(report: dict) -> dict:
    result = dict(report)
    result["slices"] = {
        prefix: {
            "total": item.total,
            "statuses": item.statuses,
            "percent": round(item.percent, 6),
            "contribution": round(item.contribution, 6),
        }
        for prefix, item in report["slices"].items()
    }
    for key in (
        "weighted_percent",
        "unweighted_percent",
        "baseline_weighted_without_hil",
    ):
        result[key] = round(result[key], 6)
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "matrix",
        nargs="?",
        type=Path,
        default=Path(__file__).resolve().parents[2]
        / "RASPI4_IMPLEMENTATION_MATRIX.md",
    )
    parser.add_argument("--check-dashboard", action="store_true")
    args = parser.parse_args()

    try:
        report = compute(args.matrix)
        if args.check_dashboard:
            check_dashboard(args.matrix, report)
    except (OSError, MatrixError) as error:
        parser.error(str(error))
    print(json.dumps(serializable(report), indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
