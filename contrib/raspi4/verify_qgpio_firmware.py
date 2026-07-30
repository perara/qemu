#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify pinned QGPIO firmware sources and cross-build outputs."""

import argparse
import hashlib
import json
from pathlib import Path
import sys


class VerificationError(Exception):
    pass


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def load_manifest(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise VerificationError(f"cannot load manifest: {exc}") from exc
    if not isinstance(value, dict) or value.get("version") != 1:
        raise VerificationError("unsupported firmware manifest")
    if not isinstance(value.get("sources"), dict):
        raise VerificationError("manifest sources must be an object")
    if not isinstance(value.get("outputs"), dict):
        raise VerificationError("manifest outputs must be an object")
    return value


def verify_sources(manifest_path: Path, manifest: dict) -> None:
    for relative, expected in manifest["sources"].items():
        if not isinstance(relative, str) or not isinstance(expected, str):
            raise VerificationError("invalid source manifest entry")
        path = manifest_path.parent / relative
        if not path.is_file() or path.is_symlink():
            raise VerificationError(f"source is not a regular file: {relative}")
        actual = digest(path)
        if actual != expected:
            raise VerificationError(
                f"source hash mismatch for {relative}: {actual}"
            )


def verify_outputs(build_dir: Path, manifest: dict) -> None:
    for name, expected in manifest["outputs"].items():
        if (
            not isinstance(name, str)
            or not isinstance(expected, dict)
            or not isinstance(expected.get("size"), int)
            or not isinstance(expected.get("sha256"), str)
        ):
            raise VerificationError("invalid output manifest entry")
        path = build_dir / name
        if not path.is_file() or path.is_symlink():
            raise VerificationError(f"output is not a regular file: {name}")
        size = path.stat().st_size
        if size != expected["size"]:
            raise VerificationError(
                f"output size mismatch for {name}: {size}"
            )
        actual = digest(path)
        if actual != expected["sha256"]:
            raise VerificationError(
                f"output hash mismatch for {name}: {actual}"
            )


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    default_manifest = (
        Path(__file__).parent
        / "gpio_adapter"
        / "rp2040"
        / "firmware-manifest.json"
    )
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=default_manifest)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--sources-only", action="store_true")
    args = parser.parse_args(argv)
    if not args.sources_only and args.build_dir is None:
        parser.error("--build-dir is required unless --sources-only is used")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        manifest = load_manifest(args.manifest)
        verify_sources(args.manifest, manifest)
        if not args.sources_only:
            verify_outputs(args.build_dir, manifest)
    except VerificationError as exc:
        print(f"qgpio-firmware: {exc}", file=sys.stderr)
        return 1
    print("qgpio firmware manifest: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
