#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Create and verify deterministic Raspberry Pi platform release manifests."""

from __future__ import annotations

import argparse
import hashlib
import json
import mimetypes
from pathlib import Path
import re
import stat
from typing import Any, Sequence


SCHEMA = "qemu-rpi4-release-manifest-v1"
RELEASE_RE = re.compile(
    r"^rpi4-v[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?$")
HEX40_RE = re.compile(r"^[0-9a-f]{40}$")
HEX64_RE = re.compile(r"^[0-9a-f]{64}$")
NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._+-]*$")
ROLE_RE = re.compile(r"^[a-z][a-z0-9-]*$")
TOP_KEYS = {"schema", "release", "source", "claims", "artifacts"}
SOURCE_KEYS = {"commit", "upstream_baseline"}
CLAIM_KEYS = {"pass1", "hil_ready", "hil_total"}
ARTIFACT_KEYS = {"name", "role", "media_type", "size", "sha256"}


class ManifestError(ValueError):
    pass


def _regular_file(path: Path) -> int:
    try:
        metadata = path.lstat()
    except OSError as exc:
        raise ManifestError(f"cannot inspect artifact {path}: {exc}") from exc
    if path.is_symlink() or not stat.S_ISREG(metadata.st_mode):
        raise ManifestError(
            f"artifact must be a regular non-symlink file: {path}"
        )
    return metadata.st_size


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as exc:
        raise ManifestError(f"cannot read artifact {path}: {exc}") from exc
    return digest.hexdigest()


def _media_type(path: Path) -> str:
    if path.name.endswith(".tar.xz"):
        return "application/x-xz"
    media_type, _ = mimetypes.guess_type(path.name)
    return media_type or "application/octet-stream"


def _expect_keys(value: dict[str, Any], expected: set[str], label: str) -> None:
    if set(value) != expected:
        missing = sorted(expected - set(value))
        extra = sorted(set(value) - expected)
        raise ManifestError(
            f"{label} keys differ: missing={missing}, extra={extra}"
        )


def validate_manifest(value: Any) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ManifestError("manifest root must be an object")
    _expect_keys(value, TOP_KEYS, "manifest")
    if value["schema"] != SCHEMA:
        raise ManifestError(f"unsupported manifest schema: {value['schema']!r}")
    if not isinstance(value["release"], str) or not RELEASE_RE.fullmatch(
            value["release"]):
        raise ManifestError(
            "release tag is not a valid rpi4-v semantic version"
        )

    source = value["source"]
    if not isinstance(source, dict):
        raise ManifestError("source must be an object")
    _expect_keys(source, SOURCE_KEYS, "source")
    for key in SOURCE_KEYS:
        if (not isinstance(source[key], str)
           or not HEX40_RE.fullmatch(source[key])):
            raise ManifestError(
                f"source.{key} must be a lowercase full commit hash"
            )

    claims = value["claims"]
    if not isinstance(claims, dict):
        raise ManifestError("claims must be an object")
    _expect_keys(claims, CLAIM_KEYS, "claims")
    if claims["pass1"] not in {"complete", "incomplete"}:
        raise ManifestError("claims.pass1 must be complete or incomplete")
    for key in ("hil_ready", "hil_total"):
        if isinstance(claims[key], bool) or not isinstance(claims[key], int):
            raise ManifestError(f"claims.{key} must be an integer")
        if claims[key] < 0:
            raise ManifestError(f"claims.{key} must not be negative")
    if claims["hil_ready"] > claims["hil_total"]:
        raise ManifestError("claims.hil_ready exceeds claims.hil_total")

    artifacts = value["artifacts"]
    if not isinstance(artifacts, list) or not artifacts:
        raise ManifestError("artifacts must be a non-empty array")
    names: set[str] = set()
    prior_name = ""
    for index, artifact in enumerate(artifacts):
        if not isinstance(artifact, dict):
            raise ManifestError(f"artifact {index} must be an object")
        _expect_keys(artifact, ARTIFACT_KEYS, f"artifact {index}")
        name = artifact["name"]
        role = artifact["role"]
        if not isinstance(name, str) or not NAME_RE.fullmatch(name):
            raise ManifestError(f"artifact {index} has an unsafe name")
        if name in names:
            raise ManifestError(f"duplicate artifact name: {name}")
        if prior_name and name < prior_name:
            raise ManifestError("artifacts are not sorted by name")
        names.add(name)
        prior_name = name
        if not isinstance(role, str) or not ROLE_RE.fullmatch(role):
            raise ManifestError(f"artifact {index} has an invalid role")
        if not isinstance(artifact["media_type"], str) or "/" not in artifact[
                "media_type"]:
            raise ManifestError(f"artifact {index} has an invalid media type")
        if (isinstance(artifact["size"], bool) or
                not isinstance(artifact["size"], int) or artifact["size"] < 0):
            raise ManifestError(f"artifact {index} has an invalid size")
        if (not isinstance(artifact["sha256"], str) or
                not HEX64_RE.fullmatch(artifact["sha256"])):
            raise ManifestError(f"artifact {index} has an invalid SHA-256")
    return value


def create_manifest(
    release: str,
    source_commit: str,
    upstream_baseline: str,
    artifact_specs: Sequence[str],
    pass1: str,
    hil_ready: int,
    hil_total: int,
) -> dict[str, Any]:
    artifacts = []
    for spec in artifact_specs:
        role, separator, raw_path = spec.partition("=")
        if not separator or not ROLE_RE.fullmatch(role):
            raise ManifestError(
                f"artifact must use a valid role=path specification: {spec!r}")
        path = Path(raw_path)
        size = _regular_file(path)
        artifacts.append({
            "name": path.name,
            "role": role,
            "media_type": _media_type(path),
            "size": size,
            "sha256": _sha256(path),
        })
    artifacts.sort(key=lambda item: item["name"])
    return validate_manifest({
        "schema": SCHEMA,
        "release": release,
        "source": {
            "commit": source_commit,
            "upstream_baseline": upstream_baseline,
        },
        "claims": {
            "pass1": pass1,
            "hil_ready": hil_ready,
            "hil_total": hil_total,
        },
        "artifacts": artifacts,
    })


def load_manifest(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ManifestError(f"cannot load manifest {path}: {exc}") from exc
    return validate_manifest(value)


def verify_artifacts(manifest: dict[str, Any], artifact_dir: Path) -> int:
    verified = 0
    for artifact in manifest["artifacts"]:
        path = artifact_dir / artifact["name"]
        size = _regular_file(path)
        if size != artifact["size"]:
            raise ManifestError(
                f"artifact size mismatch for {artifact['name']}: "
                f"{size} != {artifact['size']}")
        digest = _sha256(path)
        if digest != artifact["sha256"]:
            raise ManifestError(
                f"artifact SHA-256 mismatch for {artifact['name']}")
        verified += 1
    return verified


def _write_manifest(path: Path, manifest: dict[str, Any]) -> None:
    encoded = json.dumps(
        manifest, indent=2, sort_keys=True, ensure_ascii=True) + "\n"
    try:
        path.write_text(encoded, encoding="utf-8")
    except OSError as exc:
        raise ManifestError(f"cannot write manifest {path}: {exc}") from exc


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)

    create = subparsers.add_parser("create")
    create.add_argument("--release", required=True)
    create.add_argument("--source-commit", required=True)
    create.add_argument("--upstream-baseline", required=True)
    create.add_argument(
        "--artifact", action="append", default=[], required=True
    )
    create.add_argument("--pass1", choices=("complete", "incomplete"),
                        default="complete")
    create.add_argument("--hil-ready", type=int, default=0)
    create.add_argument("--hil-total", type=int, default=12)
    create.add_argument("--output", type=Path, required=True)

    verify = subparsers.add_parser("verify")
    verify.add_argument("--manifest", type=Path, required=True)
    verify.add_argument("--artifact-dir", type=Path, required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "create":
            manifest = create_manifest(
                args.release,
                args.source_commit,
                args.upstream_baseline,
                args.artifact,
                args.pass1,
                args.hil_ready,
                args.hil_total,
            )
            _write_manifest(args.output, manifest)
            print(json.dumps({
                "artifacts": len(manifest["artifacts"]),
                "manifest": str(args.output),
                "release": manifest["release"],
                "status": "created",
            }, sort_keys=True))
            return 0

        manifest = load_manifest(args.manifest)
        count = verify_artifacts(manifest, args.artifact_dir)
        print(json.dumps({
            "artifacts": count,
            "release": manifest["release"],
            "status": "verified",
        }, sort_keys=True))
        return 0
    except ManifestError as exc:
        print(f"release manifest error: {exc}", file=__import__("sys").stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
