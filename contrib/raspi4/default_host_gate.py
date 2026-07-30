#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify the Raspberry Pi default HTTPS network-install trust boundary."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from typing import Any


SCHEMA = "qemu-rpi-default-host-gate-v1"
DEFAULT_HOST = "fw-download-alias1.raspberrypi.com"
DEFAULT_PATH = "net_install"
EEPROM_SIZE = 512 * 1024
MAX_BOOT_IMAGE_SIZE = 1024 * 1024 * 1024
MAX_SIGNATURE_SIZE = 64 * 1024
MAX_KEY_SIZE = 64 * 1024
MAX_CERTIFICATE_SIZE = 64 * 1024
SHA256_RE = re.compile(r"[0-9a-f]{64}")
RSA_RE = re.compile(r"[0-9a-f]{512}")


class GateError(RuntimeError):
    pass


def require_regular(path: Path, maximum: int, description: str) -> Path:
    try:
        resolved = path.resolve(strict=True)
    except OSError as error:
        raise GateError(f"{description} is unavailable: {path}") from error
    if path.is_symlink() or not resolved.is_file():
        raise GateError(f"{description} is not a regular non-symlink file")
    size = resolved.stat().st_size
    if not size or size > maximum:
        raise GateError(
            f"{description} size {size} is outside 1..{maximum} bytes"
        )
    return resolved


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def check_expected(actual: str, expected: str | None,
                   description: str) -> None:
    if expected is None:
        return
    expected = expected.lower()
    if not SHA256_RE.fullmatch(expected):
        raise GateError(f"invalid expected {description} SHA-256")
    if actual != expected:
        raise GateError(f"{description} SHA-256 does not match")


def parse_signature(path: Path) -> tuple[str, int, bytes]:
    try:
        text = path.read_text(encoding="ascii")
    except (OSError, UnicodeDecodeError) as error:
        raise GateError("boot signature is not ASCII") from error
    lines = text.splitlines()
    if len(lines) != 3 or not SHA256_RE.fullmatch(lines[0]):
        raise GateError("boot signature digest line is invalid")
    if not lines[1].startswith("ts: "):
        raise GateError("boot signature timestamp line is invalid")
    try:
        timestamp = int(lines[1][4:], 10)
    except ValueError as error:
        raise GateError("boot signature timestamp is invalid") from error
    if timestamp < 0 or timestamp > (1 << 63) - 1:
        raise GateError("boot signature timestamp is outside range")
    if not lines[2].startswith("rsa2048: "):
        raise GateError("boot signature RSA line is invalid")
    signature_hex = lines[2][9:]
    if not RSA_RE.fullmatch(signature_hex):
        raise GateError("boot signature RSA-2048 value is invalid")
    return lines[0], timestamp, bytes.fromhex(signature_hex)


def run_checked(command: list[str], description: str) -> str:
    try:
        result = subprocess.run(
            command, check=False, capture_output=True, text=True,
            timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise GateError(f"{description} could not run: {error}") from error
    if result.returncode:
        detail = (result.stderr or result.stdout).strip()
        raise GateError(f"{description} failed: {detail}")
    return result.stdout.strip()


def certificate_metadata(openssl: str, path: Path) -> dict[str, str]:
    output = run_checked(
        [
            openssl, "x509", "-in", str(path), "-noout", "-subject",
            "-issuer", "-fingerprint", "-sha256",
        ],
        "certificate inspection",
    )
    metadata: dict[str, str] = {}
    for line in output.splitlines():
        if line.startswith("subject="):
            metadata["subject"] = line[8:]
        elif line.startswith("issuer="):
            metadata["issuer"] = line[7:]
        elif "Fingerprint=" in line:
            metadata["sha256_fingerprint"] = (
                line.split("=", 1)[1].replace(":", "").lower()
            )
    if set(metadata) != {"subject", "issuer", "sha256_fingerprint"}:
        raise GateError("certificate metadata is incomplete")
    return metadata


def certificate_der_sha256(openssl: str, path: Path,
                           directory: Path) -> str:
    der = directory / "ca.der"
    run_checked(
        [openssl, "x509", "-in", str(path), "-outform", "DER",
         "-out", str(der)],
        "CA certificate DER conversion",
    )
    return sha256_file(der)


def verify(args: argparse.Namespace) -> dict[str, Any]:
    openssl = shutil.which(args.openssl)
    if not openssl:
        raise GateError(f"required OpenSSL program is missing: {args.openssl}")
    eeprom = require_regular(
        args.eeprom, EEPROM_SIZE, "EEPROM image")
    if eeprom.stat().st_size != EEPROM_SIZE:
        raise GateError(
            f"EEPROM image must be exactly {EEPROM_SIZE} bytes")
    boot_image = require_regular(
        args.boot_image, MAX_BOOT_IMAGE_SIZE, "boot image")
    boot_signature = require_regular(
        args.boot_signature, MAX_SIGNATURE_SIZE, "boot signature")
    public_key = require_regular(
        args.public_key, MAX_KEY_SIZE, "network-install public key")
    ca_certificate = require_regular(
        args.ca_certificate, MAX_CERTIFICATE_SIZE, "CA certificate")
    server_certificate = (
        require_regular(
            args.server_certificate, MAX_CERTIFICATE_SIZE,
            "server certificate")
        if args.server_certificate else None
    )

    eeprom_hash = sha256_file(eeprom)
    image_hash = sha256_file(boot_image)
    signature_hash = sha256_file(boot_signature)
    public_key_hash = sha256_file(public_key)
    ca_hash = sha256_file(ca_certificate)
    check_expected(eeprom_hash, args.eeprom_sha256, "EEPROM")
    check_expected(image_hash, args.boot_image_sha256, "boot image")
    check_expected(signature_hash, args.boot_signature_sha256,
                   "boot signature")
    check_expected(public_key_hash, args.public_key_sha256, "public key")
    check_expected(ca_hash, args.ca_certificate_sha256, "CA certificate")

    declared_hash, timestamp, signature = parse_signature(boot_signature)
    if image_hash != declared_hash:
        raise GateError("boot image does not match the signed digest")
    with tempfile.TemporaryDirectory(prefix="qemu-rpi-default-host-") as tmp:
        temporary = Path(tmp)
        signature_bin = temporary / "boot-signature.bin"
        signature_bin.write_bytes(signature)
        run_checked(
            [
                openssl, "dgst", "-sha256", "-verify", str(public_key),
                "-signature", str(signature_bin), str(boot_image),
            ],
            "network-install RSA signature verification",
        )
        ca_der_hash = certificate_der_sha256(
            openssl, ca_certificate, temporary)
        if server_certificate:
            run_checked(
                [
                    openssl, "verify", "-partial_chain",
                    "-purpose", "sslserver",
                    "-verify_hostname", DEFAULT_HOST,
                    "-CAfile", str(ca_certificate),
                    str(server_certificate),
                ],
                "default-host certificate verification",
            )

    report: dict[str, Any] = {
        "schema": SCHEMA,
        "policy": {
            "host": DEFAULT_HOST,
            "path": DEFAULT_PATH,
            "port": 443,
            "transport": "https",
            "payload_signature": "rsa2048-pkcs1-v1_5-sha256",
            "bcm2711_custom_ca_supported": False,
            "secure_boot_without_http_host_allowed": False,
        },
        "eeprom": {
            "path": str(eeprom),
            "size": eeprom.stat().st_size,
            "sha256": eeprom_hash,
        },
        "boot_image": {
            "path": str(boot_image),
            "size": boot_image.stat().st_size,
            "sha256": image_hash,
        },
        "boot_signature": {
            "path": str(boot_signature),
            "size": boot_signature.stat().st_size,
            "sha256": signature_hash,
            "declared_image_sha256": declared_hash,
            "timestamp": timestamp,
            "rsa_verified": True,
        },
        "public_key": {
            "path": str(public_key),
            "sha256": public_key_hash,
        },
        "ca_certificate": {
            "path": str(ca_certificate),
            "sha256": ca_hash,
            "der_sha256": ca_der_hash,
            **certificate_metadata(openssl, ca_certificate),
        },
    }
    if server_certificate:
        report["server_certificate"] = {
            "path": str(server_certificate),
            "sha256": sha256_file(server_certificate),
            "hostname_verified": True,
            **certificate_metadata(openssl, server_certificate),
        }
    return report


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--eeprom", required=True, type=Path)
    parser.add_argument("--boot-image", required=True, type=Path)
    parser.add_argument("--boot-signature", required=True, type=Path)
    parser.add_argument("--public-key", required=True, type=Path)
    parser.add_argument("--ca-certificate", required=True, type=Path)
    parser.add_argument("--server-certificate", type=Path)
    parser.add_argument("--eeprom-sha256")
    parser.add_argument("--boot-image-sha256")
    parser.add_argument("--boot-signature-sha256")
    parser.add_argument("--public-key-sha256")
    parser.add_argument("--ca-certificate-sha256")
    parser.add_argument("--openssl", default="openssl")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--force", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        output = args.output.resolve()
        if output.exists() and not args.force:
            raise GateError(f"output already exists: {output}")
        report = verify(args)
        output.parent.mkdir(parents=True, exist_ok=True)
        temporary = output.with_name(output.name + ".tmp")
        with temporary.open("w", encoding="utf-8") as stream:
            json.dump(report, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, output)
        print(json.dumps(report, sort_keys=True))
        return 0
    except (GateError, OSError) as error:
        print(f"default-host gate: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
