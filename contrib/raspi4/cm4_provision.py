#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run one fail-closed CM4 RPIBOOT, Imager, and post-flash boot cycle.

The supervisor does not replace any production artifact or host tool.  It
verifies a versioned SHA-256 manifest, coordinates QEMU and the Linux USB
helpers through their shared lifecycle/lock contract, invokes the official
``rpiboot`` and Raspberry Pi Imager binaries unchanged, and boots the exact
regular-file eMMC backend that Imager wrote.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import select
import selectors
import shlex
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time
import tty
from typing import Any, TextIO


MANIFEST_SCHEMA = "qemu-rpi-cm4-provision-v1"
REQUIRED_ARTIFACTS = {
    "eeprom",
    "rpiboot",
    "bootcode4.bin",
    "config.txt",
    "boot.img",
    "imager",
    "image",
}
RPIBOOT_FILES = ("bootcode4.bin", "config.txt", "boot.img")
LIFECYCLE_STATES = {
    "qemu-rpiboot-wait",
    "rpiboot-host-ready",
    "rpiboot-active",
    "rpiboot-failed",
    "rpiboot-complete",
    "mass-storage-active",
    "flash-failed",
    "boot-ready",
    "qemu-owned",
    "qemu-stopped",
}
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
EEPROM_MAGIC = 0x55AAF00F
EEPROM_MAGIC_MASK = 0xFFFFF00F
EEPROM_DEPENDENCY_MAGIC = 0x55AAF44F
EEPROM_DEPENDENCY_NAME_SIZE = 16
EEPROM_DEPENDENCY_HASH_SIZE = 32
EEPROM_DEPENDENCY_MAX = 32
EEPROM_DEPENDENCY_MAX_UNCOMPRESSED = 16 * 1024 * 1024
LZ4_FRAME_MAGIC = 0x184D2204
BOOTSYS_RSA_SIZE = 256
BOOTSYS_HMAC_SIZE = 20
BOOTSYS_TRAILER_SIZE = 8 + BOOTSYS_RSA_SIZE + BOOTSYS_HMAC_SIZE
TARGET_RE = re.compile(r"^Host imaging target: (\S+)(?: -> .*)?$", re.MULTILINE)
RAW_MSD_SERIAL = "51554d5552504934"
GUEST_MSD_VENDOR_ID = "0a5c"
GUEST_MSD_PRODUCT_ID = "0104"
GUEST_MSD_MODEL = "Raspberry Pi multi-function USB device"
GUEST_MSD_SCSI_VENDOR = "mmcblk0"
CM4_BOARD_REVISIONS = {
    "1G": 0x00A03140,
    "2G": 0x00B03140,
    "4G": 0x00C03140,
    "8G": 0x00D03140,
}


class ProvisionError(RuntimeError):
    """A fail-closed provisioning or validation failure."""


def parse_size(value: str) -> int:
    match = re.fullmatch(r"([0-9]+)([KMGTP]i?B?|B)?", value, re.IGNORECASE)
    if not match:
        raise argparse.ArgumentTypeError(f"invalid byte size: {value!r}")
    number = int(match.group(1))
    suffix = (match.group(2) or "B").upper()
    suffix = suffix.removesuffix("B").removesuffix("I")
    powers = {"": 0, "K": 1, "M": 2, "G": 3, "T": 4, "P": 5}
    size = number * (1024 ** powers[suffix])
    if size <= 0 or size % 512:
        raise argparse.ArgumentTypeError(
            "eMMC size must be positive and 512-byte aligned"
        )
    return size


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(4 * 1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _lz4_length(source: bytes, offset: int, length: int) -> tuple[int, int]:
    while True:
        if offset == len(source):
            raise ProvisionError("EEPROM dependency LZ4 length is truncated")
        value = source[offset]
        offset += 1
        length += value
        if length > EEPROM_DEPENDENCY_MAX_UNCOMPRESSED:
            raise ProvisionError("EEPROM dependency LZ4 length is excessive")
        if value != 0xFF:
            return offset, length


def _lz4_block_decode(
    source: bytes,
    output: bytearray,
    output_capacity: int,
    history_start: int,
) -> None:
    offset = 0
    while offset < len(source):
        token = source[offset]
        offset += 1
        literal_size = token >> 4
        if literal_size == 15:
            offset, literal_size = _lz4_length(source, offset, literal_size)
        if (
            literal_size > len(source) - offset
            or literal_size > output_capacity - len(output)
        ):
            raise ProvisionError("EEPROM dependency LZ4 literal is invalid")
        output.extend(source[offset:offset + literal_size])
        offset += literal_size
        if offset == len(source):
            return
        if len(source) - offset < 2:
            raise ProvisionError("EEPROM dependency LZ4 match is truncated")
        match_offset = int.from_bytes(source[offset:offset + 2], "little")
        offset += 2
        if match_offset == 0 or match_offset > len(output) - history_start:
            raise ProvisionError(
                "EEPROM dependency LZ4 match offset is invalid"
            )
        match_size = token & 0xF
        if match_size == 15:
            offset, match_size = _lz4_length(source, offset, match_size)
        match_size += 4
        if match_size > output_capacity - len(output):
            raise ProvisionError("EEPROM dependency LZ4 match is excessive")
        for _ in range(match_size):
            output.append(output[-match_offset])
    raise ProvisionError("EEPROM dependency LZ4 block has no final literal")


def _xxh32_short(data: bytes) -> int:
    if len(data) >= 16:
        raise ValueError("short xxHash32 input is too long")
    mask = (1 << 32) - 1
    prime1 = 0x9E3779B1
    prime2 = 0x85EBCA77
    prime3 = 0xC2B2AE3D
    prime4 = 0x27D4EB2F
    prime5 = 0x165667B1
    result = (prime5 + len(data)) & mask
    offset = 0

    while len(data) - offset >= 4:
        word = int.from_bytes(data[offset:offset + 4], "little")
        result = (result + word * prime3) & mask
        result = (((result << 17) | (result >> 15)) * prime4) & mask
        offset += 4
    while offset < len(data):
        result = (result + data[offset] * prime5) & mask
        result = (((result << 11) | (result >> 21)) * prime1) & mask
        offset += 1
    result ^= result >> 15
    result = (result * prime2) & mask
    result ^= result >> 13
    result = (result * prime3) & mask
    return (result ^ (result >> 16)) & mask


def _lz4_frame_decode(frame: bytes) -> bytes:
    if (len(frame) < 15
       or int.from_bytes(frame[:4], "little") != LZ4_FRAME_MAGIC):
        raise ProvisionError("EEPROM dependency LZ4 frame header is invalid")
    flags = frame[4]
    descriptor = frame[5]
    block_code = descriptor >> 4
    if (
        flags & 0xC0 != 0x40
        or not flags & (1 << 3)
        or flags & ((1 << 4) | (1 << 2) | (1 << 1) | 1)
        or descriptor & 0x8F
        or block_code not in range(4, 8)
    ):
        raise ProvisionError("EEPROM dependency LZ4 frame mode is unsupported")
    if (_xxh32_short(frame[4:14]) >> 8) & 0xFF != frame[14]:
        raise ProvisionError("EEPROM dependency LZ4 header checksum is invalid")
    block_maximum = 64 * 1024 << (2 * (block_code - 4))
    expected_size = int.from_bytes(frame[6:14], "little")
    if expected_size > EEPROM_DEPENDENCY_MAX_UNCOMPRESSED:
        raise ProvisionError("EEPROM dependency LZ4 content is excessive")
    offset = 15
    output = bytearray()
    independent = bool(flags & (1 << 5))
    while len(frame) - offset >= 4:
        block_header = int.from_bytes(frame[offset:offset + 4], "little")
        offset += 4
        if block_header == 0:
            if offset != len(frame) or len(output) != expected_size:
                raise ProvisionError("EEPROM dependency LZ4 extent is invalid")
            return bytes(output)
        uncompressed = bool(block_header & (1 << 31))
        block_size = block_header & ~(1 << 31)
        if block_size > block_maximum or block_size > len(frame) - offset:
            raise ProvisionError("EEPROM dependency LZ4 block size is invalid")
        block = frame[offset:offset + block_size]
        block_start = len(output)
        if uncompressed:
            output.extend(block)
        else:
            _lz4_block_decode(
                block,
                output,
                expected_size,
                block_start if independent else 0,
            )
        if len(output) - block_start > block_maximum:
            raise ProvisionError("EEPROM dependency LZ4 block is excessive")
        if len(output) > expected_size:
            raise ProvisionError("EEPROM dependency LZ4 output is excessive")
        offset += block_size
    raise ProvisionError("EEPROM dependency LZ4 frame is truncated")


def eeprom_trust(path: Path) -> tuple[str, str, int, int]:
    try:
        image = path.read_bytes()
    except OSError as error:
        raise ProvisionError(
            f"cannot read EEPROM trust chain: {error}"
        ) from error
    if len(image) > 512 * 1024 or len(image) < 8:
        raise ProvisionError("EEPROM image size is invalid")
    magic, length = struct.unpack_from(">II", image)
    if magic != EEPROM_MAGIC or length > len(image) - 8:
        raise ProvisionError("EEPROM bootsys section header is invalid")
    bootsys = image[8:8 + length]
    if len(bootsys) != length or length < BOOTSYS_TRAILER_SIZE:
        raise ProvisionError("EEPROM bootsys section is truncated")
    trailer = bootsys[-BOOTSYS_TRAILER_SIZE:]
    payload_size, key_index = struct.unpack_from("<II", trailer)
    signature = trailer[8:8 + BOOTSYS_RSA_SIZE]
    hmac = trailer[8 + BOOTSYS_RSA_SIZE:]
    if (
        payload_size != length - BOOTSYS_TRAILER_SIZE
        or key_index > 4
        or signature in (bytes(BOOTSYS_RSA_SIZE), b"\xff" * BOOTSYS_RSA_SIZE)
        or hmac in (bytes(BOOTSYS_HMAC_SIZE), b"\xff" * BOOTSYS_HMAC_SIZE)
    ):
        raise ProvisionError("EEPROM bootsys signed envelope is invalid")
    bootsys_payload = bootsys[:payload_size]
    dependencies: list[tuple[bytes, bytes]] = []
    names: set[str] = set()
    offset = 0
    while len(image) - offset >= 8:
        section_magic, section_size = struct.unpack_from(">II", image, offset)
        if section_magic in (0, 0xFFFFFFFF):
            break
        if (
            section_magic & EEPROM_MAGIC_MASK != EEPROM_MAGIC
            or section_size > len(image) - offset - 8
        ):
            raise ProvisionError("EEPROM section chain is invalid")
        contents = image[offset + 8:offset + 8 + section_size]
        if section_magic == EEPROM_DEPENDENCY_MAGIC:
            if (
                len(contents)
                <= EEPROM_DEPENDENCY_NAME_SIZE + EEPROM_DEPENDENCY_HASH_SIZE
                or len(dependencies) == EEPROM_DEPENDENCY_MAX
            ):
                raise ProvisionError("EEPROM dependency section is invalid")
            name_field = contents[:EEPROM_DEPENDENCY_NAME_SIZE]
            try:
                terminator = name_field.index(0)
                name = name_field[:terminator].decode("ascii")
            except (ValueError, UnicodeDecodeError) as error:
                raise ProvisionError(
                    "EEPROM dependency name is invalid"
                ) from error
            if (
                not name
                or any(name_field[terminator:])
                or not all(character.isprintable() for character in name)
                or name in names
            ):
                raise ProvisionError("EEPROM dependency name is invalid")
            names.add(name)
            dependency = contents[EEPROM_DEPENDENCY_NAME_SIZE:]
            expected_hash = dependency[-EEPROM_DEPENDENCY_HASH_SIZE:]
            if expected_hash not in bootsys_payload:
                raise ProvisionError(
                    f"EEPROM dependency {name!r} is not rooted in bootsys"
                )
            decoded = _lz4_frame_decode(
                dependency[:-EEPROM_DEPENDENCY_HASH_SIZE]
            )
            if hashlib.sha256(decoded).digest() != expected_hash:
                raise ProvisionError(
                    f"EEPROM dependency {name!r} SHA-256 differs"
                )
            dependencies.append((name_field, expected_hash))
        next_offset = (offset + 8 + section_size + 7) & ~7
        if next_offset <= offset or next_offset > len(image):
            raise ProvisionError("EEPROM section alignment is invalid")
        offset = next_offset
    if (
        "bootmain" not in names
        or "mcb.bin" not in names
        or not any(
            name.startswith("memsys") and name.endswith(".bin")
            for name in names
        )
    ):
        raise ProvisionError("EEPROM required bootsys dependency is missing")
    dependency_set = hashlib.sha256()
    for name_field, expected_hash in dependencies:
        dependency_set.update(name_field)
        dependency_set.update(expected_hash)
    return (
        hashlib.sha256(bootsys).hexdigest(),
        dependency_set.hexdigest(),
        len(dependencies),
        key_index,
    )


def bootsys_sha256(path: Path) -> str:
    return eeprom_trust(path)[0]


def load_manifest(path: Path) -> dict[str, dict[str, Any]]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ProvisionError(
            f"cannot load artifact manifest {path}: {error}"
        ) from error
    if (not isinstance(document, dict)
       or document.get("schema") != MANIFEST_SCHEMA):
        raise ProvisionError(
            f"artifact manifest schema must be {MANIFEST_SCHEMA!r}"
        )
    artifacts = document.get("artifacts")
    if not isinstance(artifacts, dict):
        raise ProvisionError("artifact manifest 'artifacts' must be an object")
    missing = REQUIRED_ARTIFACTS - artifacts.keys()
    extra = artifacts.keys() - REQUIRED_ARTIFACTS
    if missing or extra:
        raise ProvisionError(
            "artifact manifest roles differ: "
            f"missing={sorted(missing)} extra={sorted(extra)}"
        )
    for role, record in artifacts.items():
        if not isinstance(record, dict):
            raise ProvisionError(
                f"manifest artifact {role!r} must be an object"
            )
        digest = record.get("sha256")
        if not isinstance(digest, str) or not SHA256_RE.fullmatch(digest):
            raise ProvisionError(
                f"manifest artifact {role!r} has invalid sha256"
            )
        if role == "image":
            payload = record.get("payload_sha256")
            if not isinstance(payload, str) or not SHA256_RE.fullmatch(payload):
                raise ProvisionError(
                    "manifest image requires the raw payload_sha256 used by "
                    "Imager"
                )
        if role == "eeprom":
            bootsys = record.get("bootsys_sha256")
            if not isinstance(bootsys, str) or not SHA256_RE.fullmatch(bootsys):
                raise ProvisionError(
                    "manifest eeprom requires the trusted bootsys_sha256"
                )
            dependencies = record.get("dependencies_sha256")
            if (
                not isinstance(dependencies, str)
                or not SHA256_RE.fullmatch(dependencies)
            ):
                raise ProvisionError(
                    "manifest eeprom requires the trusted dependencies_sha256"
                )
            key_index = record.get("bootsys_key_index")
            if (
                not isinstance(key_index, int)
                or isinstance(key_index, bool)
                or key_index < 0
                or key_index > 4
            ):
                raise ProvisionError(
                    "manifest eeprom requires bootsys_key_index from 0 to 4"
                )
        if "size" in record and (
            not isinstance(record["size"], int) or record["size"] < 0
        ):
            raise ProvisionError(f"manifest artifact {role!r} has invalid size")
    return artifacts


def verify_artifacts(
    manifest: dict[str, dict[str, Any]], paths: dict[str, Path]
) -> dict[str, str]:
    observed: dict[str, str] = {}
    for role in sorted(REQUIRED_ARTIFACTS):
        path = paths[role]
        try:
            info = path.stat()
        except OSError as error:
            raise ProvisionError(
                f"cannot stat {role} artifact {path}: {error}"
            ) from error
        if not path.is_file():
            raise ProvisionError(
                f"{role} artifact is not a regular file: {path}"
            )
        expected_size = manifest[role].get("size")
        if expected_size is not None and info.st_size != expected_size:
            raise ProvisionError(
                f"{role} size differs: expected {expected_size}, found "
                f"{info.st_size}"
            )
        observed[role] = sha256_file(path)
        if observed[role] != manifest[role]["sha256"]:
            raise ProvisionError(
                f"{role} SHA-256 differs: expected {manifest[role]['sha256']}, "
                f"found {observed[role]}"
            )
    (
        observed_bootsys,
        observed_dependencies,
        dependency_count,
        observed_key_index,
    ) = eeprom_trust(paths["eeprom"])
    if observed_bootsys != manifest["eeprom"]["bootsys_sha256"]:
        raise ProvisionError(
            "EEPROM bootsys SHA-256 differs: expected "
            f"{manifest['eeprom']['bootsys_sha256']}, "
            f"found {observed_bootsys}"
        )
    observed["bootsys"] = observed_bootsys
    if observed_dependencies != manifest["eeprom"]["dependencies_sha256"]:
        raise ProvisionError(
            "EEPROM dependency-set SHA-256 differs: expected "
            f"{manifest['eeprom']['dependencies_sha256']}, "
            f"found {observed_dependencies}"
        )
    observed["bootsys-dependencies"] = observed_dependencies
    observed["bootsys-dependency-count"] = str(dependency_count)
    if observed_key_index != manifest["eeprom"]["bootsys_key_index"]:
        raise ProvisionError(
            "EEPROM bootsys key index differs: expected "
            f"{manifest['eeprom']['bootsys_key_index']}, "
            f"found {observed_key_index}"
        )
    observed["bootsys-key-index"] = str(observed_key_index)
    return observed


def ensure_kernel_module(
    sudo: list[str],
    module: str,
    *parameters: str,
    module_root: Path = Path("/sys/module"),
    timeout: float | None = None,
) -> None:
    module_path = module_root / module

    if module_path.is_dir():
        for assignment in parameters:
            name, separator, expected = assignment.partition("=")
            parameter = module_path / "parameters" / name
            if not separator or not name or not expected:
                raise ProvisionError(
                    f"invalid {module} module parameter {assignment!r}"
                )
            try:
                actual = parameter.read_text(encoding="ascii").strip()
            except (OSError, UnicodeError) as error:
                raise ProvisionError(
                    f"cannot verify loaded {module} parameter {name}: {error}"
                ) from error
            normalized = {
                "0": "0", "N": "0", "n": "0", "false": "0",
                "1": "1", "Y": "1", "y": "1", "true": "1",
            }
            if normalized.get(actual, actual) != normalized.get(
                expected, expected
            ):
                raise ProvisionError(
                    f"loaded {module} parameter {name} differs: "
                    f"expected {expected}, found {actual}"
                )
        return

    subprocess.run(
        sudo_command(sudo, "modprobe", module, *parameters),
        check=True,
        timeout=timeout,
    )
    if not module_path.is_dir():
        raise ProvisionError(f"modprobe did not load required module {module}")


def read_lifecycle(path: Path) -> str:
    try:
        state = path.read_text(encoding="ascii").strip()
    except (OSError, UnicodeError) as error:
        raise ProvisionError(
            f"cannot read lifecycle {path}: {error}"
        ) from error
    if state not in LIFECYCLE_STATES:
        raise ProvisionError(f"invalid lifecycle state {state!r} in {path}")
    return state


def wait_lifecycle(
    path: Path, expected: str, process: subprocess.Popen[Any], timeout: float
) -> None:
    deadline = time.monotonic() + timeout
    last = "absent"
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise ProvisionError(
                f"process exited with status {process.returncode} before "
                f"lifecycle "
                f"reached {expected!r} (last {last!r})"
            )
        try:
            last = read_lifecycle(path)
        except ProvisionError:
            last = "unreadable"
        if last == expected:
            return
        time.sleep(0.05)
    raise ProvisionError(
        f"lifecycle did not reach {expected!r} within {timeout:g}s (last "
        f"{last!r})"
    )


def recover_stale_rpiboot(args: argparse.Namespace) -> bool:
    """Fail a lock-free transfer that died after publishing rpiboot-active."""
    if read_lifecycle(args.lifecycle) != "rpiboot-active":
        return False
    recovery = subprocess.run(
        sudo_command(
            args.sudo, str(args.raw_gadget), "--lifecycle",
            str(args.lifecycle), "--recover-stale",
        ),
        capture_output=True,
        text=True,
        timeout=args.timeout,
    )
    if recovery.returncode:
        detail = f"{recovery.stdout}{recovery.stderr}"
        raise ProvisionError(
            "cannot recover stale RPIBOOT transfer "
            f"(rc={recovery.returncode}):\n{detail}"
        )
    if read_lifecycle(args.lifecycle) != "rpiboot-failed":
        raise ProvisionError(
            "RPIBOOT recovery helper did not publish rpiboot-failed"
        )
    return True


def read_until(
    process: subprocess.Popen[str], phrase: str, timeout: float
) -> str:
    if process.stdout is None:
        raise ProvisionError("process stdout is not available")
    selector = selectors.DefaultSelector()
    descriptor = process.stdout.fileno()
    selector.register(descriptor, selectors.EVENT_READ)
    output = ""
    deadline = time.monotonic() + timeout
    try:
        while time.monotonic() < deadline:
            if process.poll() is not None:
                output += os.read(descriptor, 65536).decode("utf-8", "replace")
                raise ProvisionError(
                    f"process exited with status {process.returncode} before "
                    f"announcing {phrase!r}:\n{output}"
                )
            events = selector.select(deadline - time.monotonic())
            if not events:
                continue
            chunk = os.read(descriptor, 65536)
            if not chunk:
                continue
            output += chunk.decode("utf-8", "replace")
            if phrase in output:
                return output
    finally:
        selector.close()
    raise ProvisionError(f"process did not announce {phrase!r}:\n{output}")


class QMPClient:
    def __init__(
            self, path: Path, process: subprocess.Popen[Any], timeout: float):
        self.path = path
        self.process = process
        self.timeout = timeout
        self.socket: socket.socket | None = None
        self.stream: TextIO | None = None

    def __enter__(self) -> "QMPClient":
        deadline = time.monotonic() + self.timeout
        last_error: OSError | None = None
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise ProvisionError(
                    f"QEMU exited with status {self.process.returncode} before "
                    f"QMP"
                )
            connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            connection.settimeout(max(0.1, deadline - time.monotonic()))
            try:
                connection.connect(str(self.path))
                self.socket = connection
                self.stream = connection.makefile("rw", encoding="utf-8")
                greeting = self._read_message()
                if "QMP" not in greeting:
                    raise ProvisionError(f"invalid QMP greeting: {greeting!r}")
                self.command("qmp_capabilities")
                return self
            except OSError as error:
                last_error = error
                connection.close()
                time.sleep(0.05)
        raise ProvisionError(
            f"cannot connect to QMP socket {self.path}: {last_error}"
        )

    def __exit__(self, *_args: object) -> None:
        if self.stream is not None:
            self.stream.close()
        if self.socket is not None:
            self.socket.close()

    def _read_message(self) -> dict[str, Any]:
        if self.stream is None:
            raise ProvisionError("QMP is not connected")
        while True:
            line = self.stream.readline()
            if not line:
                raise ProvisionError("QMP connection closed")
            try:
                message = json.loads(line)
            except json.JSONDecodeError as error:
                raise ProvisionError(
                    f"invalid QMP response: {line!r}"
                ) from error
            if "event" not in message:
                return message

    def command(
            self, execute: str, arguments: dict[str, Any] | None = None) -> Any:
        if self.stream is None:
            raise ProvisionError("QMP is not connected")
        request: dict[str, Any] = {"execute": execute}
        if arguments:
            request["arguments"] = arguments
        self.stream.write(json.dumps(request, separators=(",", ":")) + "\n")
        self.stream.flush()
        response = self._read_message()
        if "error" in response:
            raise ProvisionError(f"QMP {execute} failed: {response['error']}")
        if "return" not in response:
            raise ProvisionError(
                f"QMP {execute} returned no result: {response}"
            )
        return response["return"]

    def qom_get(self, property_name: str) -> Any:
        return self.command(
            "qom-get", {"path": "/machine", "property": property_name}
        )

    def qom_set(self, property_name: str, value: Any) -> None:
        self.command(
            "qom-set",
            {"path": "/machine", "property": property_name, "value": value},
        )


def sudo_command(prefix: list[str], *arguments: str) -> list[str]:
    return [*prefix, *arguments]


def terminate_process(
        process: subprocess.Popen[Any] | None, sudo: list[str]) -> None:
    if process is None or process.poll() is not None:
        return
    if sudo:
        subprocess.run(
            sudo_command(sudo, "kill", "-TERM", str(process.pid)),
            capture_output=True,
            check=False,
            text=True,
        )
    else:
        process.terminate()
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        if sudo:
            subprocess.run(
                sudo_command(sudo, "kill", "-KILL", str(process.pid)),
                capture_output=True,
                check=False,
                text=True,
            )
        else:
            process.kill()
        process.wait(timeout=10)


def qemu_command(
    args: argparse.Namespace, qmp: Path, *, nrpiboot: bool,
    device_socket: Path | None = None,
) -> list[str]:
    machine = (
        "raspi-cm4,boot-mode=behavioral,eeprom-drive=pieeprom,"
        "emmc-drive=emmc,"
        f"bootsys-trusted-sha256={args.bootsys_trusted_sha256},"
        f"rpiboot-bootcode-trusted-sha256="
        f"{args.rpiboot_bootcode_trusted_sha256},"
        f"provision-state-file={args.lifecycle}"
    )
    if nrpiboot:
        machine += ",nrpiboot=on"
    command = [
        str(args.qemu),
        "-M", machine,
        "-m", args.ram,
        "-drive", f"if=none,id=pieeprom,format=raw,file={args.eeprom_backend}",
        "-drive", f"if=none,id=emmc,format=raw,file={args.emmc}",
        "-qmp", f"unix:{qmp},server=on,wait=off",
        "-display", "none",
    ]
    if nrpiboot:
        if device_socket is not None:
            command.extend([
                "-chardev",
                f"socket,id=dwc2dev,path={device_socket},server=on,wait=off",
                "-global", "dwc2-usb.device-chardev=dwc2dev",
                "-serial", f"file:{args.workdir / 'rpiboot-aux.log'}",
                "-serial", f"file:{args.workdir / 'rpiboot-pl011.log'}",
            ])
        else:
            command.append("-no-reboot")
    else:
        command.extend(args.post_qemu_arg)
    return command


def prepare_emmc(path: Path, size: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        fd = os.open(
            path,
            os.O_RDWR | os.O_CREAT | os.O_CLOEXEC | os.O_NOFOLLOW,
            0o644,
        )
        try:
            info = os.fstat(fd)
            if not stat.S_ISREG(info.st_mode):
                raise ProvisionError(
                    f"eMMC backend is not a regular file: {path}"
                )
            if info.st_size != size:
                os.ftruncate(fd, size)
            os.fsync(fd)
        finally:
            os.close(fd)
    except OSError as error:
        raise ProvisionError(
            f"cannot prepare eMMC backend {path}: {error}"
        ) from error


def prepare_eeprom(source: Path, backend: Path, expected_sha256: str) -> None:
    """Copy an official EEPROM image into a private writable backend."""
    backend.parent.mkdir(parents=True, exist_ok=True)
    backend_created = False
    try:
        source_fd = os.open(source, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
        try:
            backend_fd = os.open(
                backend,
                os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC |
                os.O_NOFOLLOW,
                0o644,
            )
            backend_created = True
            try:
                while chunk := os.read(source_fd, 1024 * 1024):
                    offset = 0
                    while offset < len(chunk):
                        written = os.write(backend_fd, chunk[offset:])
                        if written <= 0:
                            raise OSError("zero-length EEPROM backend write")
                        offset += written
                os.fsync(backend_fd)
            finally:
                os.close(backend_fd)
        finally:
            os.close(source_fd)
    except OSError as error:
        if backend_created:
            try:
                backend.unlink()
            except FileNotFoundError:
                pass
        raise ProvisionError(
            f"cannot create private EEPROM backend {backend}: {error}"
        ) from error
    observed = sha256_file(backend)
    if observed != expected_sha256:
        backend.unlink()
        raise ProvisionError(
            f"private EEPROM copy differs: expected {expected_sha256}, "
            f"found {observed}"
        )


def verify_rpiboot_capture(boot_dir: Path, capture_dir: Path) -> None:
    for name in RPIBOOT_FILES:
        served = sha256_file(boot_dir / name)
        captured = sha256_file(capture_dir / name)
        if captured != served:
            raise ProvisionError(
                f"RPIBOOT changed {name}: served {served}, captured {captured}"
            )


def wait_raw_msd_target(
    process: subprocess.Popen[str],
    dev_root: Path,
    sys_root: Path,
    expected_size: int,
    timeout: float,
) -> Path:
    """Select only the raw BOT owner's own exported eMMC block device.

    A by-id name containing the serial proves nothing about which device the
    kernel actually bound: the name is attacker-influenceable and the link can
    be replaced between resolution and the privileged write.  Correlate the
    serial with the block device's sysfs ancestry, exact USB identity, SCSI
    identity, whole-disk status, and size before the path reaches Imager, and
    require the answer to stay the same while the owner is still alive.
    """
    by_id = dev_root / "disk/by-id"
    block_root = sys_root / "class/block"
    deadline = time.monotonic() + timeout
    stable: Path | None = None
    stable_count = 0
    while time.monotonic() < deadline:
        if process.poll() is not None:
            output = process.stdout.read() if process.stdout else ""
            raise ProvisionError(
                f"Raw BOT owner exited {process.returncode} before its "
                f"stable target appeared:\n{output}"
            )
        serial_targets = set()
        if by_id.is_dir():
            for path in by_id.glob(f"*{RAW_MSD_SERIAL}*"):
                # udev creates sibling by-id links for every partition once
                # the image has a partition table.  Ownership must resolve
                # the whole exported eMMC, never mistake its partitions for
                # additional devices, and remain stable across
                # re-enumeration after flashing.
                if re.search(r"-part[0-9]+$", path.name):
                    continue
                try:
                    resolved = path.resolve(strict=True)
                    if stat.S_ISBLK(resolved.stat().st_mode):
                        serial_targets.add(resolved)
                except OSError:
                    continue
        candidates = sorted({
            candidate
            for block in block_root.iterdir()
            if (candidate := _guest_msd_candidate(
                block, dev_root, expected_size
            )) is not None and candidate in serial_targets
        }) if block_root.is_dir() else []
        if len(candidates) > 1:
            raise ProvisionError(
                f"Raw BOT serial {RAW_MSD_SERIAL} resolves to multiple "
                "targets: " + ", ".join(map(str, candidates))
            )
        current = candidates[0] if candidates else None
        if current is not None and current == stable:
            stable_count += 1
            if stable_count >= 3:
                return current
        else:
            stable = current
            stable_count = 1 if current is not None else 0
        time.sleep(0.05)
    raise ProvisionError(
        f"Raw BOT serial {RAW_MSD_SERIAL} did not produce a stable "
        f"{GUEST_MSD_VENDOR_ID}:{GUEST_MSD_PRODUCT_ID} "
        f"{expected_size}-byte block target"
    )


def _read_sysfs_text(path: Path) -> str | None:
    try:
        return path.read_text(encoding="ascii").strip()
    except (OSError, UnicodeError):
        return None


def _guest_usb_device(device_path: Path) -> Path | None:
    for parent in (device_path, *device_path.parents):
        vendor = _read_sysfs_text(parent / "idVendor")
        product = _read_sysfs_text(parent / "idProduct")
        if vendor is not None or product is not None:
            if (
                vendor == GUEST_MSD_VENDOR_ID and
                product == GUEST_MSD_PRODUCT_ID and
                _read_sysfs_text(parent / "product") == GUEST_MSD_MODEL and
                "dummy_hcd" in str(parent)
            ):
                return parent
            return None
    return None


def _guest_msd_candidate(
    block: Path, dev_root: Path, expected_size: int
) -> Path | None:
    if (block / "partition").exists():
        return None
    sectors = _read_sysfs_text(block / "size")
    scsi_vendor = _read_sysfs_text(block / "device/vendor")
    try:
        if sectors is None or int(sectors) * 512 != expected_size:
            return None
    except ValueError:
        return None
    if scsi_vendor != GUEST_MSD_SCSI_VENDOR:
        return None

    try:
        device_path = block.resolve(strict=True)
    except OSError:
        return None
    if _guest_usb_device(device_path) is None:
        return None

    target = dev_root / block.name
    try:
        if not stat.S_ISBLK(target.stat().st_mode):
            return None
    except OSError:
        return None
    return target.resolve(strict=True)


def _matching_guest_acm_targets(
    block_target: Path, dev_root: Path, sys_root: Path
) -> list[Path]:
    usb_device = _guest_usb_device_for_block(block_target, sys_root)
    if usb_device is None:
        return []

    candidates = []
    tty_root = sys_root / "class/tty"
    if not tty_root.is_dir():
        return candidates
    for tty_class in tty_root.glob("ttyACM*"):
        try:
            tty_device = tty_class.resolve(strict=True)
            if usb_device not in tty_device.parents:
                continue
            target = dev_root / tty_class.name
            if not stat.S_ISCHR(target.stat().st_mode):
                continue
            candidates.append(target.resolve(strict=True))
        except OSError:
            continue
    return sorted(set(candidates))


def _guest_usb_device_for_block(
    block_target: Path, sys_root: Path
) -> Path | None:
    block = sys_root / "class/block" / block_target.name
    try:
        return _guest_usb_device(block.resolve(strict=True))
    except OSError:
        return None


def reset_guest_usb(
    process: subprocess.Popen[str],
    block_target: Path,
    sys_root: Path,
    usbreset: Path,
    sudo: list[str],
    timeout: float,
) -> None:
    if process.poll() is not None:
        raise ProvisionError(
            f"DWC2 proxy exited {process.returncode} before USB reset"
        )
    usb_device = _guest_usb_device_for_block(block_target, sys_root)
    if usb_device is None:
        raise ProvisionError(
            "cannot resolve the qualified continued-guest USB device for reset"
        )
    bus_text = _read_sysfs_text(usb_device / "busnum")
    device_text = _read_sysfs_text(usb_device / "devnum")
    try:
        bus = int(bus_text or "")
        device = int(device_text or "")
    except ValueError as error:
        raise ProvisionError(
            "continued-guest USB bus/device number is invalid"
        ) from error
    if not (1 <= bus <= 999 and 1 <= device <= 999):
        raise ProvisionError(
            "continued-guest USB bus/device number is out of range"
        )
    result = subprocess.run(
        sudo_command(sudo, str(usbreset), f"{bus:03d}/{device:03d}"),
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    if result.returncode:
        raise ProvisionError(
            f"continued-guest USB reset failed with {result.returncode}:\n"
            f"{result.stdout}{result.stderr}"
        )


def wait_guest_acm_target(
    process: subprocess.Popen[str],
    block_target: Path,
    dev_root: Path,
    sys_root: Path,
    timeout: float,
) -> Path:
    """Select the sole ACM tty belonging to the qualified guest USB device."""
    deadline = time.monotonic() + timeout
    stable: Path | None = None
    stable_count = 0
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise ProvisionError(
                f"DWC2 proxy exited {process.returncode} before its matching "
                "continued-guest ACM target appeared"
            )
        candidates = _matching_guest_acm_targets(
            block_target, dev_root, sys_root
        )
        if len(candidates) > 1:
            raise ProvisionError(
                "continued guest exposes multiple matching ACM targets: " +
                ", ".join(map(str, candidates))
            )
        current = candidates[0] if candidates else None
        if current is not None and current == stable:
            stable_count += 1
            if stable_count >= 3:
                return current
        else:
            stable = current
            stable_count = 1 if current is not None else 0
        time.sleep(0.05)
    raise ProvisionError(
        "continued guest did not expose one stable ACM target on the same "
        "qualified USB device as its eMMC"
    )


def verify_acm_echo(target: Path, token: bytes, timeout: float) -> None:
    """Prove host TX and guest tty line-discipline RX/TX on one ACM target."""
    if not token or any(byte < 0x21 or byte > 0x7e for byte in token):
        raise ProvisionError("ACM echo token must be non-empty printable ASCII")
    deadline = time.monotonic() + timeout
    descriptor = -1
    saved: list[Any] | None = None
    received = bytearray()
    next_write = 0.0
    pending = b""
    try:
        descriptor = os.open(
            target, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK | os.O_CLOEXEC
        )
        saved = termios.tcgetattr(descriptor)
        tty.setraw(descriptor, termios.TCSANOW)
        attributes = termios.tcgetattr(descriptor)
        attributes[4] = termios.B115200
        attributes[5] = termios.B115200
        termios.tcsetattr(descriptor, termios.TCSANOW, attributes)
        termios.tcflush(descriptor, termios.TCIOFLUSH)
        while time.monotonic() < deadline:
            now = time.monotonic()
            if not pending and now >= next_write:
                pending = token + b"\r\n"
                next_write = now + 0.5
            readable, writable, _ = select.select(
                [descriptor], [descriptor] if pending else [], [],
                max(0.0, min(0.1, deadline - now)),
            )
            if writable:
                try:
                    pending = pending[os.write(descriptor, pending):]
                except BlockingIOError:
                    pass
            if readable:
                try:
                    received.extend(os.read(descriptor, 4096))
                except BlockingIOError:
                    continue
                if token in received:
                    return
                if len(received) > 65536:
                    del received[:-32768]
    except (OSError, termios.error) as error:
        raise ProvisionError(
            f"continued-guest ACM echo failed on {target}: {error}"
        ) from error
    finally:
        if descriptor >= 0:
            if saved is not None:
                try:
                    termios.tcsetattr(descriptor, termios.TCSANOW, saved)
                except (OSError, termios.error):
                    pass
            os.close(descriptor)
    raise ProvisionError(
        f"continued-guest ACM target {target} did not echo the host token "
        f"within {timeout:g}s"
    )


def verify_guest_block_prefix(
    process: subprocess.Popen[str],
    target: Path,
    backend: Path,
    sudo: list[str],
    timeout: float,
    length: int = 1024 * 1024,
) -> str:
    """Require post-reset host SCSI READ data to match the QEMU eMMC backend."""
    try:
        with backend.open("rb") as source:
            expected = source.read(length)
    except OSError as error:
        raise ProvisionError(
            f"cannot read continued-guest eMMC backend {backend}: {error}"
        ) from error
    if len(expected) != length:
        raise ProvisionError(
            f"continued-guest eMMC backend is shorter than {length} bytes"
        )

    deadline = time.monotonic() + timeout
    last_error = ""
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise ProvisionError(
                f"DWC2 proxy exited {process.returncode} before post-reset "
                "block I/O recovered"
            )
        result = subprocess.run(
            sudo_command(
                sudo, "dd", f"if={target}", f"bs={length}", "count=1",
                "iflag=direct", "status=none",
            ),
            capture_output=True,
            timeout=max(0.1, deadline - time.monotonic()),
        )
        if result.returncode == 0:
            if result.stdout != expected:
                raise ProvisionError(
                    "continued-guest block prefix differs from the exact "
                    "QEMU eMMC backend"
                )
            return hashlib.sha256(result.stdout).hexdigest()
        last_error = result.stderr.decode("utf-8", "replace")
        time.sleep(0.1)
    raise ProvisionError(
        "continued-guest block I/O did not recover within "
        f"{timeout:g}s after USB reset:\n{last_error}"
    )


def wait_guest_msd_target(
    process: subprocess.Popen[str],
    dev_root: Path,
    sys_root: Path,
    expected_size: int,
    timeout: float,
) -> Path:
    """Select only the continued guest's exact dummy_hcd CM4 block device."""
    block_root = sys_root / "class/block"
    deadline = time.monotonic() + timeout
    stable: Path | None = None
    stable_count = 0
    while time.monotonic() < deadline:
        if process.poll() is not None:
            output = process.stdout.read() if process.stdout else ""
            raise ProvisionError(
                f"DWC2 proxy exited {process.returncode} before the continued "
                f"guest mass-storage target appeared:\n{output}"
            )
        candidates = sorted({
            candidate
            for block in block_root.iterdir()
            if (candidate := _guest_msd_candidate(
                block, dev_root, expected_size
            )) is not None
        }) if block_root.is_dir() else []
        if len(candidates) > 1:
            raise ProvisionError(
                "continued guest identity resolves to multiple targets: " +
                ", ".join(map(str, candidates))
            )
        current = candidates[0] if candidates else None
        if current is not None and current == stable:
            stable_count += 1
            if stable_count >= 3:
                return current
        else:
            stable = current
            stable_count = 1 if current is not None else 0
        time.sleep(0.05)
    raise ProvisionError(
        "continued guest did not expose one stable "
        f"{GUEST_MSD_VENDOR_ID}:{GUEST_MSD_PRODUCT_ID} "
        f"{expected_size}-byte eMMC target through dummy_hcd"
    )


def wait_proxy_phrase(
    process: subprocess.Popen[str],
    initial: str,
    lines: list[str],
    phrase: str,
    timeout: float,
) -> str:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        output = initial + "".join(lines)
        if phrase in output:
            return output
        if process.poll() is not None:
            raise ProvisionError(
                f"DWC2 proxy exited {process.returncode} before announcing "
                f"{phrase!r}:\n{output}"
            )
        time.sleep(0.05)
    raise ProvisionError(
        f"DWC2 proxy did not announce {phrase!r} within {timeout:g}s:\n"
        f"{initial}{''.join(lines)}"
    )


def mass_storage_owner_command(
    args: argparse.Namespace,
) -> tuple[list[str], str]:
    if args.mass_storage_mode == "configfs":
        return (
            sudo_command(
                args.sudo, sys.executable, str(args.mass_storage),
                "--lifecycle", str(args.lifecycle), "serve", str(args.emmc),
            ),
            "CM4 mass-storage owner ready",
        )
    return (
        sudo_command(
            args.sudo, str(args.raw_msd), "--image", str(args.emmc),
            "--lifecycle", str(args.lifecycle), "--foreground-owner",
        ),
        "CM4 raw BOT target ready",
    )


def run_supervisor(
    args: argparse.Namespace, report: dict[str, Any] | None = None
) -> dict[str, Any]:
    manifest = load_manifest(args.manifest)
    artifact_paths = {
        "eeprom": args.eeprom,
        "rpiboot": args.rpiboot,
        "bootcode4.bin": args.boot_dir / "bootcode4.bin",
        "config.txt": args.boot_dir / "config.txt",
        "boot.img": args.boot_dir / "boot.img",
        "imager": args.imager,
        "image": args.image,
    }
    observed = verify_artifacts(manifest, artifact_paths)
    args.bootsys_trusted_sha256 = manifest["eeprom"]["bootsys_sha256"]
    args.rpiboot_bootcode_trusted_sha256 = \
        manifest["bootcode4.bin"]["sha256"]
    owner_helper = (
        args.mass_storage
        if args.mass_storage_mode == "configfs"
        else args.raw_msd
    )
    rpiboot_helper = (
        args.dwc2_proxy if args.rpiboot_mode == "inprocess"
        else args.raw_gadget
    )
    required_helpers = [args.qemu, rpiboot_helper]
    if args.rpiboot_mode == "helper":
        required_helpers.append(owner_helper)
    if args.rpiboot_mode == "inprocess" and args.guest_reset_count:
        required_helpers.append(args.usbreset)
    for path in required_helpers:
        if not path.is_file():
            raise ProvisionError(
                f"required executable/helper is missing: {path}"
            )
    executable_names = ["qemu", "rpiboot", "imager"]
    executable_names.append(
        "dwc2_proxy" if args.rpiboot_mode == "inprocess" else "raw_gadget"
    )
    if args.rpiboot_mode == "inprocess" and args.guest_reset_count:
        executable_names.append("usbreset")
    if (
        args.rpiboot_mode == "helper" and
        args.mass_storage_mode == "raw-bot"
    ):
        executable_names.append("raw_msd")
    for name in executable_names:
        path = getattr(args, name)
        if not os.access(path, os.X_OK | os.R_OK):
            raise ProvisionError(
                f"{name.replace('_', ' ')} is not executable: {path}"
            )
    if any("," in str(path) for path in (args.lifecycle, args.emmc)):
        raise ProvisionError("lifecycle and eMMC paths cannot contain commas")

    args.workdir.mkdir(parents=True, exist_ok=True)
    args.capture_dir.mkdir(parents=True, exist_ok=False)
    args.eeprom_backend = args.workdir / "pieeprom.bin"
    prepare_eeprom(args.eeprom, args.eeprom_backend, observed["eeprom"])
    prepare_emmc(args.emmc, args.emmc_size)
    qmp_directory = tempfile.TemporaryDirectory(prefix="qemu-rpi-cm4-qmp-")
    qmp_root = Path(qmp_directory.name)

    if report is None:
        report = {}
    report.update({
        "schema": MANIFEST_SCHEMA,
        "status": "running",
        "artifacts": observed,
        "ram": args.ram,
        "emmc_size": args.emmc_size,
        "rpiboot_mode": args.rpiboot_mode,
        "mass_storage_mode": (
            "continued-guest"
            if args.rpiboot_mode == "inprocess"
            else args.mass_storage_mode
        ),
        "guest_reset_count": args.guest_reset_count,
        "events": [],
    })
    before: subprocess.Popen[Any] | None = None
    raw: subprocess.Popen[str] | None = None
    raw_drain: threading.Thread | None = None
    raw_lines: list[str] = []
    owner: subprocess.Popen[str] | None = None
    after: subprocess.Popen[Any] | None = None
    owner_active = False
    try:
        before_qmp = qmp_root / "before.qmp"
        dwc2_socket = qmp_root / "dwc2.sock"
        before = subprocess.Popen(qemu_command(
            args, before_qmp, nrpiboot=True,
            device_socket=(
                dwc2_socket if args.rpiboot_mode == "inprocess" else None
            ),
        ))
        wait_lifecycle(
            args.lifecycle, "qemu-rpiboot-wait", before, args.timeout
        )
        with QMPClient(before_qmp, before, args.timeout) as qmp:
            observations = {
                "boot-state": qmp.qom_get("boot-state"),
                "boot-source": qmp.qom_get("boot-source"),
                "provision-state": qmp.qom_get("provision-state"),
                "memory-model": qmp.qom_get("memory-model"),
                "board-revision": qmp.qom_get("board-revision"),
            }
            expected = {
                "boot-state": "rpiboot-wait",
                "boot-source": "rpiboot",
                "provision-state": "qemu-rpiboot-wait",
                "memory-model": args.ram.replace("G", "GiB"),
                "board-revision": CM4_BOARD_REVISIONS[args.ram],
            }
            if observations != expected:
                raise ProvisionError(
                    f"pre-RPIBOOT QEMU observations differ: {observations}"
                )
            if args.rpiboot_mode == "helper":
                qmp.command("quit")
        if args.rpiboot_mode == "helper":
            before.wait(timeout=args.timeout)
            if before.returncode:
                raise ProvisionError(
                    f"pre-RPIBOOT QEMU exited {before.returncode}"
                )
            if read_lifecycle(args.lifecycle) != "rpiboot-host-ready":
                raise ProvisionError("QEMU did not release RPIBOOT ownership")
            report["events"].append("qemu-rpiboot-released")

        ensure_kernel_module(
            args.sudo, "dummy_hcd", "is_high_speed=1",
            timeout=args.timeout,
        )
        ensure_kernel_module(args.sudo, "raw_gadget", timeout=args.timeout)
        if args.rpiboot_mode == "inprocess":
            raw = subprocess.Popen(
                sudo_command(
                    args.sudo, str(args.dwc2_proxy),
                    "--dwc2-socket", str(dwc2_socket),
                    "--continue-guest",
                ),
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            raw_ready = read_until(raw, "presenting ROM stage", args.timeout)

            def drain_proxy() -> None:
                assert raw is not None and raw.stdout is not None
                for line in raw.stdout:
                    raw_lines.append(line)

            raw_drain = threading.Thread(
                target=drain_proxy, name="cm4-dwc2-proxy-output",
                daemon=True,
            )
            raw_drain.start()
        else:
            raw = subprocess.Popen(
                sudo_command(
                    args.sudo, str(args.raw_gadget), "--capture-dir",
                    str(args.capture_dir), "--lifecycle", str(args.lifecycle),
                    "--mass-storage",
                ),
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            raw_ready = read_until(
                raw, "waiting for unmodified rpiboot", args.timeout
            )
        rpiboot_run = subprocess.run(
            sudo_command(args.sudo, str(args.rpiboot), "-d",
                         str(args.boot_dir)),
            capture_output=True,
            text=True,
            timeout=args.rpiboot_timeout,
        )
        if args.rpiboot_mode == "inprocess":
            raw_output = wait_proxy_phrase(
                raw, raw_ready, raw_lines,
                "unchanged rpiboot completed through emulated DWC2",
                args.timeout,
            )
        else:
            raw_output = raw.communicate(timeout=args.timeout)[0]
        raw_failed = (
            raw.returncode not in (None, 0)
            if args.rpiboot_mode == "inprocess" else raw.returncode != 0
        )
        if rpiboot_run.returncode or raw_failed:
            raise ProvisionError(
                f"rpiboot rc={rpiboot_run.returncode}:\n{rpiboot_run.stdout}"
                f"\n{rpiboot_run.stderr}\nRaw Gadget rc={raw.returncode}:\n"
                f"{raw_ready}{raw_output}"
            )
        if args.rpiboot_mode == "inprocess":
            wait_lifecycle(
                args.lifecycle, "rpiboot-complete", before, args.timeout
            )
            with QMPClient(before_qmp, before, args.timeout) as qmp:
                observations = {
                    "boot-state": qmp.qom_get("boot-state"),
                    "boot-source": qmp.qom_get("boot-source"),
                    "provision-state": qmp.qom_get("provision-state"),
                    "arm-handoff-status": qmp.qom_get(
                        "arm-handoff-status"
                    ),
                    "firmware-status": qmp.qom_get("firmware-status"),
                }
                expected_observations = {
                    "boot-state": "arm-handoff-ready",
                    "boot-source": "rpiboot",
                    "provision-state": "rpiboot-complete",
                    "arm-handoff-status": "ready",
                    "firmware-status": "config-ready",
                }
                if observations != expected_observations:
                    raise ProvisionError(
                        "in-process RPIBOOT observations differ: "
                        f"{observations}; expected {expected_observations}"
                    )
                for name, prefix in (
                    ("bootcode4.bin", "rpiboot-bootcode"),
                    ("config.txt", "rpiboot-config"),
                    ("boot.img", "rpiboot-boot-img"),
                ):
                    size = qmp.qom_get(f"{prefix}-size")
                    digest = qmp.qom_get(f"{prefix}-sha256")
                    expected_size = (args.boot_dir / name).stat().st_size
                    expected_digest = observed[name]
                    if size != expected_size or digest != expected_digest:
                        raise ProvisionError(
                            f"in-process RPIBOOT changed {name}: "
                            f"size={size}/{expected_size} "
                            f"sha256={digest}/{expected_digest}"
                        )
            report["events"].append("rpiboot-arm-handoff")
        else:
            verify_rpiboot_capture(args.boot_dir, args.capture_dir)
            if read_lifecycle(args.lifecycle) != "rpiboot-complete":
                raise ProvisionError(
                    "RPIBOOT helper did not publish completion"
                )
        report["events"].append("rpiboot-complete")

        if args.rpiboot_mode == "inprocess":
            assert raw is not None
            wait_proxy_phrase(
                raw, raw_ready, raw_lines,
                "guest Linux USB gadget configured with",
                args.timeout,
            )
            target = wait_guest_msd_target(
                raw, args.dev_root, args.sys_root, args.emmc_size,
                args.timeout,
            )
            report["events"].append("continued-guest-mass-storage-ready")
            acm_target = wait_guest_acm_target(
                raw, target, args.dev_root, args.sys_root, args.timeout
            )
            verify_acm_echo(
                acm_target,
                f"QEMU_RPI_ACM_PRE_{os.getpid():x}".encode("ascii"),
                args.timeout,
            )
            report["continued_guest"] = {
                "usb_id": (
                    f"{GUEST_MSD_VENDOR_ID}:{GUEST_MSD_PRODUCT_ID}"
                ),
                "product": GUEST_MSD_MODEL,
                "scsi_vendor": GUEST_MSD_SCSI_VENDOR,
                "block_target": str(target),
                "acm_target": str(acm_target),
            }
            report["events"].append("continued-guest-acm-preflash-echo")
        else:
            owner_command, ready_marker = mass_storage_owner_command(args)
            owner = subprocess.Popen(
                owner_command,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            owner_active = True
            owner_ready = read_until(owner, ready_marker, args.timeout)
            if args.mass_storage_mode == "configfs":
                match = TARGET_RE.search(owner_ready)
                if not match:
                    raise ProvisionError(
                        f"mass-storage helper gave no stable target:\n"
                        f"{owner_ready}"
                    )
                target = Path(match.group(1)).resolve(strict=True)
            else:
                target = wait_raw_msd_target(
                    owner, args.dev_root, args.sys_root, args.emmc_size,
                    args.timeout,
                )
        imager_run = subprocess.run(
            sudo_command(
                args.sudo, str(args.imager), "--disable-eject", "--sha256",
                manifest["image"]["payload_sha256"], str(args.image),
                str(target),
            ),
            timeout=args.imager_timeout,
        )
        if imager_run.returncode:
            raise ProvisionError(
                f"Raspberry Pi Imager exited {imager_run.returncode}"
            )
        subprocess.run(
            sudo_command(args.sudo, "blockdev", "--flushbufs", str(target)),
            check=True,
            timeout=args.timeout,
        )
        if args.rpiboot_mode == "inprocess":
            assert raw is not None
            acm_target = wait_guest_acm_target(
                raw, target, args.dev_root, args.sys_root, args.timeout
            )
            verify_acm_echo(
                acm_target,
                f"QEMU_RPI_ACM_POST_{os.getpid():x}".encode("ascii"),
                args.timeout,
            )
            report["events"].append("continued-guest-acm-postflash-echo")
            for reset_index in range(args.guest_reset_count):
                reset_guest_usb(
                    raw, target, args.sys_root, args.usbreset, args.sudo,
                    args.timeout,
                )
                target = wait_guest_msd_target(
                    raw, args.dev_root, args.sys_root, args.emmc_size,
                    args.timeout,
                )
                reset_prefix_digest = verify_guest_block_prefix(
                    raw, target, args.emmc, args.sudo, args.timeout
                )
                acm_target = wait_guest_acm_target(
                    raw, target, args.dev_root, args.sys_root, args.timeout
                )
                verify_acm_echo(
                    acm_target,
                    (
                        f"QEMU_RPI_ACM_RESET_{reset_index + 1}_"
                        f"{os.getpid():x}"
                    ).encode("ascii"),
                    args.timeout,
                )
            if args.guest_reset_count:
                report["continued_guest"].update({
                    "reset_count": args.guest_reset_count,
                    "reset_verified_prefix_sha256": reset_prefix_digest,
                })
                report["events"].append(
                    "continued-guest-usb-reset-campaign-recovered"
                )
            with QMPClient(before_qmp, before, args.timeout) as qmp:
                qmp.qom_set("provision-flash-complete", True)
                if qmp.qom_get("provision-state") != "boot-ready":
                    raise ProvisionError(
                        "QEMU did not publish boot-ready after the continued "
                        "guest eMMC flush"
                    )
                qmp.command("quit")
            before.wait(timeout=args.timeout)
            if before.returncode:
                raise ProvisionError(
                    f"in-process flashing QEMU exited {before.returncode}"
                )
            terminate_process(raw, args.sudo)
            if raw_drain is not None:
                raw_drain.join(timeout=args.timeout)
                if raw_drain.is_alive():
                    raise ProvisionError(
                        "DWC2 proxy output drain did not stop"
                    )
            report["events"].append("qemu-rpiboot-released")
        else:
            assert owner is not None
            if owner.stdin is None:
                raise ProvisionError(
                    "mass-storage owner stdin is unavailable"
                )
            owner.stdin.write("complete\n")
            owner.stdin.flush()
            owner_output = owner.communicate(timeout=args.timeout)[0]
            owner_active = False
            if owner.returncode:
                raise ProvisionError(
                    f"mass-storage owner exited {owner.returncode}:\n"
                    f"{owner_ready}{owner_output}"
                )
        if read_lifecycle(args.lifecycle) != "boot-ready":
            raise ProvisionError(
                "flashing owner did not release boot-ready eMMC"
            )
        report["events"].append("image-written-verified-flushed")

        after_qmp = qmp_root / "after.qmp"
        after = subprocess.Popen(qemu_command(args, after_qmp, nrpiboot=False))
        wait_lifecycle(args.lifecycle, "qemu-owned", after, args.timeout)
        with QMPClient(after_qmp, after, args.timeout) as qmp:
            observations = {
                "boot-state": qmp.qom_get("boot-state"),
                "boot-source": qmp.qom_get("boot-source"),
                "provision-state": qmp.qom_get("provision-state"),
            }
            if observations != {
                "boot-state": "arm-handoff-ready",
                "boot-source": "emmc",
                "provision-state": "qemu-owned",
            }:
                raise ProvisionError(
                    f"post-flash QEMU observations differ: {observations}"
                )
            report["events"].append("post-flash-arm-handoff")
            if args.handoff_only:
                qmp.command("quit")
        if args.handoff_only:
            after.wait(timeout=args.timeout)
        elif args.post_boot_timeout:
            after.wait(timeout=args.post_boot_timeout)
        else:
            after.wait()
        if after.returncode:
            raise ProvisionError(f"post-flash QEMU exited {after.returncode}")
        if read_lifecycle(args.lifecycle) != "qemu-stopped":
            raise ProvisionError("post-flash QEMU did not publish qemu-stopped")
        report["events"].append("post-flash-qemu-stopped")
        report["status"] = "complete"
        return report
    except (OSError, subprocess.CalledProcessError,
            subprocess.TimeoutExpired) as error:
        raise ProvisionError(str(error)) from error
    finally:
        if owner_active and owner is not None and owner.poll() is None:
            try:
                if owner.stdin is not None:
                    owner.stdin.write("failed\n")
                    owner.stdin.flush()
                owner.communicate(timeout=10)
            except (BrokenPipeError, OSError, subprocess.TimeoutExpired):
                terminate_process(owner, args.sudo)
        terminate_process(raw, args.sudo)
        if raw_drain is not None:
            raw_drain.join(timeout=2)
        if raw is not None:
            if args.rpiboot_mode == "inprocess":
                deadline = time.monotonic() + min(args.timeout, 5.0)
                while (
                    read_lifecycle(args.lifecycle) == "rpiboot-active" and
                    before is not None and before.poll() is None and
                    time.monotonic() < deadline
                ):
                    time.sleep(0.05)
            else:
                recover_stale_rpiboot(args)
        terminate_process(before, [])
        if (
            raw is not None and args.rpiboot_mode == "inprocess" and
            read_lifecycle(args.lifecycle) == "rpiboot-active"
        ):
            recover_stale_rpiboot(args)
        terminate_process(after, [])
        qmp_directory.cleanup()


def write_report(path: Path, report: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp.{os.getpid()}")
    contents = json.dumps(report, indent=2, sort_keys=True) + "\n"
    try:
        with temporary.open("x", encoding="utf-8") as output:
            output.write(contents)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    except OSError as error:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass
        raise ProvisionError(f"cannot write report {path}: {error}") from error


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path, required=True)
    parser.add_argument("--eeprom", type=Path, required=True)
    parser.add_argument("--emmc", type=Path, required=True)
    parser.add_argument("--emmc-size", type=parse_size, required=True)
    parser.add_argument("--ram", choices=("1G", "2G", "4G", "8G"), default="2G")
    parser.add_argument("--raw-gadget", type=Path, required=True)
    parser.add_argument(
        "--rpiboot-mode", choices=("inprocess", "helper"),
        default="inprocess",
        help=(
            "run unchanged rpiboot through the CM4 model's DWC2 device "
            "(default), or retain the standalone compatibility helper"
        ),
    )
    parser.add_argument(
        "--dwc2-proxy", type=Path,
        default=(
            Path(__file__).parents[2] /
            "build/qemu-rpi-dwc2-raw-gadget-proxy"
        ),
        help="packet-only Raw Gadget proxy used by inprocess rpiboot mode",
    )
    parser.add_argument("--rpiboot", type=Path, required=True)
    parser.add_argument("--boot-dir", type=Path, required=True)
    parser.add_argument("--imager", type=Path, required=True)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--workdir", type=Path, required=True)
    parser.add_argument("--capture-dir", type=Path)
    parser.add_argument("--lifecycle", type=Path)
    parser.add_argument("--report", type=Path)
    parser.add_argument(
        "--mass-storage", type=Path,
        default=Path(__file__).with_name("cm4_mass_storage.py"),
    )
    parser.add_argument(
        "--mass-storage-mode", choices=("configfs", "raw-bot"),
        default="configfs",
        help="successful Imager target backend; configfs remains compatible",
    )
    parser.add_argument(
        "--raw-msd", type=Path,
        default=Path(__file__).parents[2] / "build/qemu-rpi-cm4-msd",
        help="command-visible Raw Gadget BOT target used in raw-bot mode",
    )
    parser.add_argument(
        "--dev-root", type=Path, default=Path("/dev"),
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--sys-root", type=Path, default=Path("/sys"),
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--usbreset", type=Path, default=Path("/usr/bin/usbreset"),
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--guest-reset-count", type=int, default=0,
        help=(
            "after Imager verification, reset and requalify the exact "
            "continued-guest composite USB device this many times"
        ),
    )
    parser.add_argument(
        "--sudo", default="sudo -n",
        help="privilege prefix parsed without a shell; empty when already root",
    )
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--rpiboot-timeout", type=float, default=90.0)
    parser.add_argument("--imager-timeout", type=float, default=300.0)
    parser.add_argument("--post-boot-timeout", type=float, default=0.0)
    parser.add_argument("--handoff-only", action="store_true")
    parser.add_argument("--post-qemu-arg", action="append", default=[])
    return parser


def normalize_args(args: argparse.Namespace) -> None:
    for name in (
        "qemu", "eeprom", "emmc", "raw_gadget", "dwc2_proxy",
        "rpiboot", "boot_dir",
        "imager", "image", "manifest", "workdir", "mass_storage", "raw_msd",
        "dev_root", "sys_root", "usbreset",
    ):
        setattr(args, name, getattr(args, name).expanduser().resolve())
    args.capture_dir = (
        args.capture_dir.expanduser().resolve()
        if args.capture_dir else args.workdir / "rpiboot-capture"
    )
    args.lifecycle = (
        args.lifecycle.expanduser().resolve()
        if args.lifecycle else args.workdir / "cm4-provision.state"
    )
    args.report = (
        args.report.expanduser().resolve()
        if args.report else args.workdir / "cm4-provision-report.json"
    )
    args.sudo = shlex.split(args.sudo)
    if (args.timeout <= 0
       or args.rpiboot_timeout <= 0 or args.imager_timeout <= 0):
        raise ProvisionError("timeouts must be positive")
    if args.post_boot_timeout < 0:
        raise ProvisionError("post-boot timeout cannot be negative")
    if args.guest_reset_count < 0 or args.guest_reset_count > 32:
        raise ProvisionError("guest reset count must be between 0 and 32")


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    report: dict[str, Any] = {"schema": MANIFEST_SCHEMA, "status": "failed"}
    try:
        normalize_args(args)
        report = run_supervisor(args, report)
    except ProvisionError as error:
        report["status"] = "failed"
        report["error"] = str(error)
        print(f"cm4-provision: {error}", file=sys.stderr)
        result = 1
    except KeyboardInterrupt:
        report["status"] = "failed"
        report["error"] = "interrupted"
        print("cm4-provision: interrupted", file=sys.stderr)
        result = 130
    else:
        print("CM4 provisioning and post-flash boot completed")
        result = 0
    try:
        if "args" in locals() and getattr(args, "report", None):
            write_report(args.report, report)
    except ProvisionError as error:
        print(f"cm4-provision: {error}", file=sys.stderr)
        return 1
    return result


if __name__ == "__main__":
    raise SystemExit(main())
