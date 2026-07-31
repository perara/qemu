#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fail closed on unsafe or incomplete Raspberry Pi publication contents."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess
from typing import Sequence


SOURCE_SUFFIXES = {".c", ".h", ".py", ".sh", ".rs"}
DISALLOWED_SUFFIXES = {
    ".bin", ".dat", ".dtb", ".dtbo", ".elf", ".img", ".iso", ".key",
    ".p12", ".pfx", ".qcow2", ".raw",
}
SECRET_PATTERNS = (
    re.compile(rb"-----BEGIN (?:RSA |EC |DSA |OPENSSH )?PRIVATE KEY-----"),
    re.compile(rb"\bAKIA[0-9A-Z]{16}\b"),
    re.compile(rb"\bgh[pousr]_[A-Za-z0-9_]{20,}\b"),
    re.compile(rb"\bgithub_pat_[A-Za-z0-9_]{20,}\b"),
    re.compile(rb"\bxox[baprs]-[A-Za-z0-9-]{10,}\b"),
)
SIGNOFF_RE = re.compile(
    r"^Signed-off-by: [^<>\n]+ <[^<>\s]+@[^<>\s]+>$", re.MULTILINE)
SPDX_TAG = "SPDX-License-" + "Identifier:"


class AuditError(ValueError):
    pass


def _git(repo: Path, *args: str, text: bool = True) -> str | bytes:
    try:
        return subprocess.check_output(
            ("git", *args),
            cwd=repo,
            text=text,
            stderr=subprocess.STDOUT,
        )
    except subprocess.CalledProcessError as exc:
        output = exc.output.strip() if exc.output else str(exc)
        raise AuditError(f"git {' '.join(args)} failed: {output}") from exc


def _changed_paths(repo: Path, baseline: str, filter_value: str) -> list[str]:
    output = _git(
        repo, "diff", "--name-only", f"--diff-filter={filter_value}",
        f"{baseline}..HEAD")
    assert isinstance(output, str)
    return [line for line in output.splitlines() if line]


def _check_commits(repo: Path, baseline: str) -> int:
    output = _git(repo, "log", "--format=%H%x00%B%x00", f"{baseline}..HEAD")
    assert isinstance(output, str)
    fields = output.split("\x00")
    commits = 0
    for index in range(0, len(fields) - 1, 2):
        commit = fields[index].strip()
        body = fields[index + 1]
        if not commit:
            continue
        commits += 1
        if not SIGNOFF_RE.search(body):
            raise AuditError(f"commit lacks a valid DCO sign-off: {commit}")
    if commits == 0:
        raise AuditError("publication range contains no commits")
    return commits


def _check_added_source_licenses(repo: Path, baseline: str) -> int:
    checked = 0
    for name in _changed_paths(repo, baseline, "A"):
        path = repo / name
        if path.is_symlink():
            raise AuditError(
                f"tracked symbolic link is not publishable: {name}"
            )
        if not path.is_file():
            continue
        if path.suffix not in SOURCE_SUFFIXES and path.name != "CMakeLists.txt":
            continue
        try:
            header = "\n".join(
                path.read_text(encoding="utf-8").splitlines()[:40])
        except (OSError, UnicodeDecodeError) as exc:
            raise AuditError(f"cannot inspect source license for {name}: {exc}")
        if SPDX_TAG not in header:
            raise AuditError(f"added source lacks an SPDX header: {name}")
        checked += 1
    return checked


def _check_paths(repo: Path, baseline: str, maximum_size: int) -> int:
    checked = 0
    for name in _changed_paths(repo, baseline, "ACMR"):
        path = repo / name
        if path.is_symlink():
            raise AuditError(
                f"tracked symbolic link is not publishable: {name}"
            )
        if not path.is_file():
            continue
        checked += 1
        if name == "build" or name.startswith("build/"):
            raise AuditError(f"build output is tracked: {name}")
        if path.suffix.lower() in DISALLOWED_SUFFIXES:
            raise AuditError(f"restricted binary artifact is tracked: {name}")
        size = path.stat().st_size
        if size > maximum_size:
            raise AuditError(
                f"changed file exceeds {maximum_size} bytes: {name} ({size})")
        if size > 8 * 1024 * 1024:
            continue
        try:
            content = path.read_bytes()
        except OSError as exc:
            raise AuditError(f"cannot read changed file {name}: {exc}") from exc
        for pattern in SECRET_PATTERNS:
            if pattern.search(content):
                raise AuditError(f"possible embedded credential in {name}")
    return checked


def audit(repo: Path, baseline: str, maximum_size: int) -> dict[str, int | str]:
    resolved = _git(repo, "rev-parse", "--verify", f"{baseline}^{{commit}}")
    assert isinstance(resolved, str)
    baseline_commit = resolved.strip()
    ancestor = subprocess.run(
        ("git", "merge-base", "--is-ancestor", baseline_commit, "HEAD"),
        cwd=repo,
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if ancestor.returncode != 0:
        raise AuditError("baseline is not an ancestor of HEAD")
    commits = _check_commits(repo, baseline_commit)
    sources = _check_added_source_licenses(repo, baseline_commit)
    paths = _check_paths(repo, baseline_commit, maximum_size)
    return {
        "baseline": baseline_commit,
        "commits": commits,
        "licensed_added_sources": sources,
        "publication_paths": paths,
        "status": "pass",
    }


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument("--max-file-size", type=int, default=50 * 1024 * 1024)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        result = audit(args.repo.resolve(), args.baseline, args.max_file_size)
    except AuditError as exc:
        print(f"publication audit error: {exc}", file=__import__("sys").stderr)
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
