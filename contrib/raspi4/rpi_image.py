#!/usr/bin/env python3
#
# Raspberry Pi image flashing and inspection helpers
#
# Copyright (c) 2026 Per-Arne and contributors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Flash, verify, and inspect raw Raspberry Pi media images.

The tool deliberately operates below the filesystem layer.  It therefore tests
the same byte stream that would be sent to an SD card or eMMC device, while
remaining safe to use with regular files in CI.
"""

from __future__ import annotations

import argparse
import binascii
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import sys
import tempfile
from typing import BinaryIO, Iterator
import uuid

try:
    import fcntl
except ImportError:  # pragma: no cover - the production fixture is Linux
    fcntl = None


DEFAULT_CHUNK_SIZE = 4 * 1024 * 1024
SECTOR_SIZE = 512
POWER_LOSS_EXIT = 75
RESUME_JOURNAL_VERSION = 1
LINUX_SYSFS_ROOT = Path("/sys")
LINUX_PROC_ROOT = Path("/proc")
LINUX_MOUNTINFO = Path("/proc/self/mountinfo")
LINUX_SWAPS = Path("/proc/swaps")
MAX_EMBEDDED_CONFIG = 16 * 1024 * 1024
MAX_MOUNT_NAMESPACES = 65536
MOUNT_NAMESPACE_STABILITY_RETRIES = 3


class ImageError(RuntimeError):
    """Raised for an invalid image or unsafe flashing request."""


class PowerLossInjected(RuntimeError):
    """Raised after deliberately stopping a flash at a byte boundary."""


def _size(value: str) -> int:
    match = re.fullmatch(r"\s*(0[xob][0-9a-f]+|\d+)\s*([kmgt](?:i?b)?)?\s*",
                         value, re.IGNORECASE)
    if not match:
        raise argparse.ArgumentTypeError(
            "expected an integer with optional KiB/MiB/GiB/TiB suffix"
        )
    number = int(match.group(1), 0)
    suffix = (match.group(2) or "").lower()
    if suffix:
        number *= 1024 ** ("kmgt".index(suffix[0]) + 1)
    return number


def _positive_int(value: str) -> int:
    number = _size(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("value must be greater than zero")
    return number


def _non_negative_int(value: str) -> int:
    number = _size(value)
    if number < 0:
        raise argparse.ArgumentTypeError("value must not be negative")
    return number


def _same_file(source: Path, target: Path) -> bool:
    try:
        return os.path.samefile(source, target)
    except FileNotFoundError:
        return source.resolve() == target.resolve()


def _validate_open_target(mode: int, allow_block_device: bool) -> str:
    """Validate the already-open target, never a raceable path lookup."""

    if stat.S_ISREG(mode):
        return "regular"
    if stat.S_ISBLK(mode) and allow_block_device:
        return "block"
    if stat.S_ISBLK(mode):
        raise ImageError(
            "target is a block device; pass --allow-block-device explicitly"
        )
    raise ImageError(
        "target must be a regular file or an approved block device"
    )


def _resolve_target_endpoint(target: Path) -> Path:
    """Resolve a stable alias once, then operate only on that endpoint."""

    try:
        if target.is_symlink():
            return target.resolve(strict=True)
        return target.resolve(strict=False)
    except OSError as error:
        raise ImageError(f"cannot resolve target endpoint: {error}") from error


def _open_target(path: Path, *, exists: bool, writable: bool) -> int:
    flags = os.O_CLOEXEC | os.O_NOFOLLOW
    flags |= os.O_RDWR if writable else os.O_RDONLY
    if writable:
        # On Linux, O_EXCL without O_CREAT requests exclusive block-device
        # ownership.  With O_CREAT it also prevents a create/open race.
        flags |= os.O_EXCL
        if not exists:
            flags |= os.O_CREAT
    try:
        return os.open(path, flags, 0o600)
    except OSError as error:
        raise ImageError(f"cannot open target endpoint safely: {error}") \
            from error


def _read_sysfs_integer(path: Path, description: str) -> int:
    try:
        value = path.read_text(encoding="ascii").strip()
        return int(value, 10)
    except (OSError, UnicodeError, ValueError) as error:
        raise ImageError(f"cannot read block-device {description}: {error}") \
            from error


def _mounted_device_numbers(path: Path) -> set[tuple[int, int]]:
    devices: set[tuple[int, int]] = set()
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as error:
        raise ImageError(f"cannot inspect mounted filesystems: {error}") \
            from error
    for line_number, line in enumerate(lines, 1):
        fields = line.split()
        if len(fields) < 6 or ":" not in fields[2]:
            raise ImageError(
                f"mountinfo line {line_number} is malformed"
            )
        major_text, minor_text = fields[2].split(":", 1)
        try:
            devices.add((int(major_text, 10), int(minor_text, 10)))
        except ValueError as error:
            raise ImageError(
                f"mountinfo line {line_number} has invalid device numbers"
            ) from error
    return devices


def _mount_namespace_key(path: Path, description: str,
                         *, vanished_ok: bool = False) -> str | None:
    try:
        info = os.stat(path)
    except FileNotFoundError:
        process = path.parent.parent
        if vanished_ok and not process.exists():
            return None
        raise ImageError(f"cannot inspect {description}: namespace is absent")
    except OSError as error:
        raise ImageError(f"cannot inspect {description}: {error}") from error
    return f"{info.st_dev}:{info.st_ino}"


def _namespace_mounted_device_numbers(
    process: Path, description: str
) -> tuple[str | None, set[tuple[int, int]]]:
    """Read one process mount namespace without accepting a setns race."""

    devices: set[tuple[int, int]] = set()
    for _ in range(MOUNT_NAMESPACE_STABILITY_RETRIES):
        before = _mount_namespace_key(
            process / "ns" / "mnt", description, vanished_ok=True
        )
        if before is None:
            return None, devices
        try:
            current = _mounted_device_numbers(process / "mountinfo")
        except ImageError as error:
            if not process.exists():
                return None, devices
            raise ImageError(f"cannot inspect {description}: {error}") \
                from error
        devices.update(current)
        after = _mount_namespace_key(
            process / "ns" / "mnt", description, vanished_ok=True
        )
        if after is None or after == before:
            return before, devices
    raise ImageError(f"cannot inspect {description}: namespace is unstable")


def _all_mounted_device_numbers(
    proc_root: Path,
    current_mountinfo_path: Path = LINUX_MOUNTINFO,
) -> set[tuple[int, int]]:
    """Collect devices mounted in every currently inspectable namespace."""

    current_key = _mount_namespace_key(
        proc_root / "self" / "ns" / "mnt", "current mount namespace"
    )
    devices = _mounted_device_numbers(current_mountinfo_path)
    seen = {current_key}
    try:
        processes = sorted(
            (entry for entry in proc_root.iterdir() if entry.name.isdecimal()),
            key=lambda entry: int(entry.name),
        )
    except OSError as error:
        raise ImageError(
            f"cannot enumerate host mount namespaces: {error}"
        ) from error
    if len(processes) > MAX_MOUNT_NAMESPACES:
        raise ImageError(
            "host process count exceeds the mount-namespace safety bound"
        )

    for process in processes:
        description = f"mount namespace for process {process.name}"
        key = _mount_namespace_key(
            process / "ns" / "mnt", description, vanished_ok=True
        )
        if key is None or key in seen:
            continue
        stable_key, mounted = _namespace_mounted_device_numbers(
            process, description
        )
        devices.update(mounted)
        if stable_key is not None:
            seen.add(stable_key)
    return devices


def _swap_device_numbers(path: Path) -> set[tuple[int, int]]:
    devices: set[tuple[int, int]] = set()
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as error:
        raise ImageError(f"cannot inspect active swap: {error}") from error
    if not lines or not lines[0].split() or lines[0].split()[0] != "Filename":
        raise ImageError("active-swap metadata is malformed")
    for line_number, line in enumerate(lines[1:], 2):
        fields = line.split()
        if len(fields) < 2:
            raise ImageError(f"active-swap line {line_number} is malformed")
        try:
            info = os.stat(fields[0])
        except OSError as error:
            raise ImageError(
                f"cannot inspect active swap {fields[0]}: {error}"
            ) from error
        if stat.S_ISBLK(info.st_mode):
            devices.add((os.major(info.st_rdev), os.minor(info.st_rdev)))
    return devices


def _sysfs_device_node(sysfs_root: Path, major: int, minor: int) -> Path:
    link = sysfs_root / "dev" / "block" / f"{major}:{minor}"
    try:
        return link.resolve(strict=True)
    except OSError as error:
        raise ImageError(
            f"cannot resolve block device {major}:{minor} in sysfs: {error}"
        ) from error


def _validate_linux_block_device(
    major: int,
    minor: int,
    source_size: int,
    *,
    sysfs_root: Path = LINUX_SYSFS_ROOT,
    mountinfo_path: Path = LINUX_MOUNTINFO,
    swaps_path: Path = LINUX_SWAPS,
    proc_root: Path | None = LINUX_PROC_ROOT,
) -> int:
    """Fail closed unless an opened Linux whole device is safe to overwrite."""

    node = _sysfs_device_node(sysfs_root, major, minor)
    if (node / "partition").exists():
        raise ImageError("block-device target is a partition, not whole media")
    if _read_sysfs_integer(node / "ro", "read-only state") != 0:
        raise ImageError("block-device target is read-only")
    if _read_sysfs_integer(node / "removable", "removable state") != 1:
        raise ImageError("block-device target is not removable")

    sectors = _read_sysfs_integer(node / "size", "capacity")
    if sectors < 0:
        raise ImageError("block-device capacity is negative")
    capacity = sectors * SECTOR_SIZE
    if capacity < source_size:
        raise ImageError(
            f"block-device capacity {capacity} is smaller than image "
            f"size {source_size}"
        )

    mounted_devices = (
        _all_mounted_device_numbers(proc_root, mountinfo_path)
        if proc_root is not None else
        _mounted_device_numbers(mountinfo_path)
    )
    for mounted_major, mounted_minor in mounted_devices:
        try:
            mounted_node = _sysfs_device_node(
                sysfs_root, mounted_major, mounted_minor
            )
        except ImageError:
            continue
        if mounted_node == node or node in mounted_node.parents:
            raise ImageError(
                f"block-device target or descendant {mounted_major}:"
                f"{mounted_minor} is mounted"
            )
    for swap_major, swap_minor in _swap_device_numbers(swaps_path):
        swap_node = _sysfs_device_node(sysfs_root, swap_major, swap_minor)
        if swap_node == node or node in swap_node.parents:
            raise ImageError(
                f"block-device target or descendant {swap_major}:"
                f"{swap_minor} is active swap"
            )
    return capacity


def _sha256_range(stream: BinaryIO, length: int, chunk_size: int) -> str:
    digest = hashlib.sha256()
    remaining = length
    while remaining:
        data = stream.read(min(chunk_size, remaining))
        if not data:
            raise ImageError("target ended before verification completed")
        digest.update(data)
        remaining -= len(data)
    return digest.hexdigest()


def _sha256_file(path: Path, chunk_size: int) -> str:
    with path.open("rb") as stream:
        return _sha256_range(stream, path.stat().st_size, chunk_size)


def _default_journal_path(target: Path) -> Path:
    return target.with_name(target.name + ".rpi-resume.json")


def _target_identity(target: Path, stream: BinaryIO) -> dict[str, object]:
    info = os.fstat(stream.fileno())
    return {
        "path": str(target.resolve()),
        "device": info.st_dev,
        "inode": info.st_ino,
        "rdev": info.st_rdev,
        "kind": "block" if stat.S_ISBLK(info.st_mode) else "regular",
    }


def _linux_block_target_evidence(
    major: int,
    minor: int,
    capacity: int,
    *,
    sysfs_root: Path = LINUX_SYSFS_ROOT,
) -> dict[str, object]:
    node = _sysfs_device_node(sysfs_root, major, minor)
    return {
        "major": major,
        "minor": minor,
        "kernel_name": node.name,
        "sysfs_path": str(node),
        "capacity": capacity,
        "logical_sector_size": SECTOR_SIZE,
        "removable": True,
        "read_only": False,
    }


def _fsync_parent(path: Path) -> None:
    flags = os.O_RDONLY
    if hasattr(os, "O_DIRECTORY"):
        flags |= os.O_DIRECTORY
    fd = os.open(path.parent, flags)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def _write_journal(path: Path, journal: dict[str, object], *,
                   create: bool = False) -> None:
    payload = (json.dumps(journal, indent=2, sort_keys=True) + "\n").encode()
    path.parent.mkdir(parents=True, exist_ok=True)
    if create:
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
        fd = os.open(path, flags, 0o600)
        try:
            with os.fdopen(fd, "wb", closefd=False) as stream:
                stream.write(payload)
                stream.flush()
                os.fsync(stream.fileno())
        finally:
            os.close(fd)
    else:
        fd, temporary_name = tempfile.mkstemp(
            prefix=path.name + ".", suffix=".tmp", dir=path.parent
        )
        temporary = Path(temporary_name)
        try:
            with os.fdopen(fd, "wb") as stream:
                stream.write(payload)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, path)
        finally:
            try:
                temporary.unlink()
            except FileNotFoundError:
                pass
    _fsync_parent(path)


def _load_journal(path: Path) -> dict[str, object]:
    try:
        journal = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ImageError(f"cannot read resume journal: {error}") from error
    if not isinstance(journal, dict):
        raise ImageError("resume journal root must be an object")
    required = {
        "version", "source_path", "source_size", "source_sha256",
        "target", "bytes_written", "prefix_sha256",
    }
    if not required.issubset(journal):
        raise ImageError("resume journal is missing required fields")
    if journal["version"] != RESUME_JOURNAL_VERSION:
        raise ImageError("unsupported resume journal version")
    return journal


def _write_all(stream: BinaryIO, data: bytes) -> None:
    view = memoryview(data)
    while view:
        written = stream.write(view)
        if written is None or written <= 0:
            raise ImageError("target write made no progress")
        view = view[written:]


def _lock_target(stream: BinaryIO) -> None:
    if fcntl is None:
        raise ImageError("safe flashing requires POSIX target locking")
    try:
        fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError as error:
        raise ImageError("target is owned by another flash process") from error


def flash_image(
    source: Path,
    target: Path,
    *,
    chunk_size: int = DEFAULT_CHUNK_SIZE,
    verify: bool = True,
    fail_after: int | None = None,
    allow_block_device: bool = False,
    resume: bool = False,
    journal_path: Path | None = None,
) -> dict[str, object]:
    """Copy a raw image to media and optionally verify the written byte range.

    ``fail_after`` models sudden power loss.  The write is stopped exactly at
    the requested absolute byte boundary and the partial target plus durable
    resume journal are left in place.  Resume verifies the source identity,
    target identity, and both byte prefixes before continuing.
    """

    if not source.is_file():
        raise ImageError("source must be a regular file")
    target.parent.mkdir(parents=True, exist_ok=True)
    target_endpoint = _resolve_target_endpoint(target)
    if _same_file(source, target_endpoint):
        raise ImageError("source and target resolve to the same file")

    source_size = source.stat().st_size
    source_hash = _sha256_file(source, chunk_size)
    explicit_journal = journal_path is not None
    journal_path = journal_path or _default_journal_path(target)
    if fail_after is not None and fail_after > source_size:
        raise ImageError("--fail-after exceeds the source image size")
    if resume and not verify:
        raise ImageError("resume requires final whole-image verification")

    target_exists = target_endpoint.exists()
    if resume and not target_exists:
        raise ImageError("resume target does not exist")
    bytes_written = 0
    resume_from = 0
    target_capacity = None
    target_digest = None
    opened_identity: dict[str, object] | None = None
    identity: dict[str, object] | None = None

    target_fd = _open_target(
        target_endpoint, exists=target_exists, writable=True)
    with source.open("rb") as src, os.fdopen(
            target_fd, "r+b", buffering=0) as dst:
        opened_info = os.fstat(dst.fileno())
        opened_kind = _validate_open_target(
            opened_info.st_mode, allow_block_device)
        if opened_kind == "block" and not explicit_journal:
            raise ImageError(
                "block-device flashing requires --resume-journal outside /dev"
            )
        _lock_target(dst)
        opened_identity = _target_identity(target_endpoint, dst)
        identity = dict(opened_identity)
        if identity["kind"] == "block":
            info = os.fstat(dst.fileno())
            major = os.major(info.st_rdev)
            minor = os.minor(info.st_rdev)
            target_capacity = _validate_linux_block_device(
                major, minor, source_size
            )
            identity.update(
                _linux_block_target_evidence(
                    major, minor, target_capacity
                )
            )
        if resume:
            journal = _load_journal(journal_path)
            if journal["source_path"] != str(source.resolve()):
                raise ImageError("resume source path does not match journal")
            if journal["source_size"] != source_size:
                raise ImageError("resume source size does not match journal")
            if journal["source_sha256"] != source_hash:
                raise ImageError("resume source SHA-256 does not match journal")
            if journal["target"] != identity:
                raise ImageError(
                    "resume target identity does not match journal"
                )
            progress = journal["bytes_written"]
            if (not isinstance(progress, int) or isinstance(progress, bool) or
                    progress < 0 or progress > source_size):
                raise ImageError("resume journal byte offset is invalid")
            if fail_after is not None and fail_after < progress:
                raise ImageError("--fail-after precedes the resume offset")
            src.seek(0)
            source_digest = hashlib.sha256()
            remaining = progress
            while remaining:
                data = src.read(min(chunk_size, remaining))
                if not data:
                    raise ImageError("source ended while hashing resume prefix")
                source_digest.update(data)
                remaining -= len(data)
            source_prefix = source_digest.hexdigest()
            dst.seek(0)
            target_prefix = _sha256_range(dst, progress, chunk_size)
            if source_prefix != journal["prefix_sha256"]:
                raise ImageError("resume source prefix does not match journal")
            if target_prefix != journal["prefix_sha256"]:
                raise ImageError("resume target prefix does not match journal")
            bytes_written = progress
            resume_from = progress
            src.seek(progress)
            dst.seek(progress)
        else:
            if journal_path.exists():
                raise ImageError(
                    "resume journal already exists; use --resume or remove it"
                )
            source_digest = hashlib.sha256()
            journal = {
                "version": RESUME_JOURNAL_VERSION,
                "source_path": str(source.resolve()),
                "source_size": source_size,
                "source_sha256": source_hash,
                "target": identity,
                "bytes_written": 0,
                "prefix_sha256": source_digest.hexdigest(),
            }
            _write_journal(journal_path, journal, create=True)

        while bytes_written < source_size:
            limit = min(chunk_size, source_size - bytes_written)
            if fail_after is not None:
                limit = min(limit, fail_after - bytes_written)
                if limit == 0:
                    dst.flush()
                    os.fsync(dst.fileno())
                    raise PowerLossInjected(
                        f"power loss injected after {bytes_written} bytes"
                    )

            data = src.read(limit)
            if not data:
                raise ImageError("source ended before its reported size")
            _write_all(dst, data)
            source_digest.update(data)
            bytes_written += len(data)
            dst.flush()
            os.fsync(dst.fileno())
            journal["bytes_written"] = bytes_written
            journal["prefix_sha256"] = source_digest.hexdigest()
            _write_journal(journal_path, journal)

        if identity["kind"] == "regular":
            dst.truncate(source_size)
        dst.flush()
        os.fsync(dst.fileno())
        if verify:
            verifier_fd = _open_target(
                target_endpoint, exists=True, writable=False)
            with os.fdopen(verifier_fd, "rb", buffering=0) as verifier:
                _validate_open_target(
                    os.fstat(verifier.fileno()).st_mode,
                    allow_block_device,
                )
                if (_target_identity(target_endpoint, verifier) !=
                        opened_identity):
                    raise ImageError(
                        "target identity changed before verification"
                    )
                target_digest = _sha256_range(
                    verifier, source_size, chunk_size
                )

    assert opened_identity is not None
    assert identity is not None
    result: dict[str, object] = {
        "source": str(source),
        "target": str(target),
        "target_identity": identity,
        "bytes_written": bytes_written,
        "source_sha256": source_hash,
        "verified": False,
        "resumed": resume,
        "resume_from": resume_from,
        "resume_journal": str(journal_path),
    }
    if target_capacity is not None:
        result["target_capacity"] = target_capacity

    if verify:
        assert target_digest is not None
        result["target_sha256"] = target_digest
        result["verified"] = target_digest == result["source_sha256"]
        if not result["verified"]:
            raise ImageError("SHA-256 verification failed")

    journal_path.unlink()
    _fsync_parent(journal_path)

    return result


def _filesystem_metadata(
    stream: BinaryIO, offset: int, length: int
) -> dict[str, object]:
    result: dict[str, object] = {
        "filesystem": "unknown",
        "filesystem_uuid": None,
        "filesystem_label": None,
    }
    if length < 1082:
        return result

    stream.seek(offset)
    boot_sector = stream.read(SECTOR_SIZE)
    if (
        len(boot_sector) == SECTOR_SIZE
        and boot_sector[510:512] == b"\x55\xaa"
        and (
            boot_sector[54:62].startswith(b"FAT")
            or boot_sector[82:90].startswith(b"FAT")
        )
    ):
        fat32 = boot_sector[82:90].startswith(b"FAT")
        signature_offset = 66 if fat32 else 38
        serial_offset = 67 if fat32 else 39
        label_offset = 71 if fat32 else 43
        result["filesystem"] = "fat"
        if boot_sector[signature_offset] in (0x28, 0x29):
            serial = struct.unpack_from("<I", boot_sector, serial_offset)[0]
            result["filesystem_uuid"] = (
                f"{serial >> 16:04X}-{serial & 0xFFFF:04X}"
            )
            if boot_sector[signature_offset] == 0x29:
                label = boot_sector[label_offset:label_offset + 11]
                result["filesystem_label"] = label.decode(
                    "ascii", errors="replace"
                ).rstrip(" \0") or None
        return result

    stream.seek(offset + 1024)
    superblock = stream.read(1024)
    if len(superblock) == 1024 and superblock[56:58] == b"\x53\xef":
        result["filesystem"] = "ext"
        result["filesystem_uuid"] = str(uuid.UUID(bytes=superblock[104:120]))
        result["filesystem_label"] = superblock[120:136].split(
            b"\0", 1
        )[0].decode("utf-8", errors="replace") or None
    return result


def _partition_region(partition: dict[str, object]) -> tuple[int, int]:
    first_lba = partition.get("first_lba")
    sectors = partition.get("sectors")
    if sectors is None and partition.get("last_lba") is not None:
        sectors = int(partition["last_lba"]) - int(first_lba) + 1
    if (not isinstance(first_lba, int) or not isinstance(sectors, int) or
            first_lba < 0 or sectors <= 0 or not partition.get("in_bounds")):
        raise ImageError("partition bounds are invalid")
    start = first_lba * SECTOR_SIZE
    return start, start + sectors * SECTOR_SIZE


def _read_region(
    stream: BinaryIO, offset: int, size: int, start: int, end: int,
    description: str,
) -> bytes:
    if size < 0 or offset < start or offset + size > end:
        raise ImageError(f"{description} exceeds its partition")
    stream.seek(offset)
    data = stream.read(size)
    if len(data) != size:
        raise ImageError(f"{description} is truncated")
    return data


def _fat_read_file(
    stream: BinaryIO, partition: dict[str, object], filename: str
) -> bytes:
    start, end = _partition_region(partition)
    boot = _read_region(stream, start, SECTOR_SIZE, start, end,
                        "FAT boot sector")
    bytes_per_sector = struct.unpack_from("<H", boot, 11)[0]
    sectors_per_cluster = boot[13]
    reserved_sectors = struct.unpack_from("<H", boot, 14)[0]
    fat_count = boot[16]
    root_entries = struct.unpack_from("<H", boot, 17)[0]
    total_sectors = struct.unpack_from("<H", boot, 19)[0]
    if total_sectors == 0:
        total_sectors = struct.unpack_from("<I", boot, 32)[0]
    fat_sectors = struct.unpack_from("<H", boot, 22)[0]
    if fat_sectors == 0:
        fat_sectors = struct.unpack_from("<I", boot, 36)[0]
    if (bytes_per_sector not in (512, 1024, 2048, 4096) or
            sectors_per_cluster == 0 or
            sectors_per_cluster & (sectors_per_cluster - 1) or
            reserved_sectors == 0 or fat_count == 0 or fat_sectors == 0 or
            total_sectors == 0 or
            total_sectors * bytes_per_sector > end - start):
        raise ImageError("invalid FAT geometry")
    root_sectors = (
        (root_entries * 32 + bytes_per_sector - 1) // bytes_per_sector
    )
    first_data_sector = reserved_sectors + fat_count * fat_sectors + \
        root_sectors
    if first_data_sector >= total_sectors:
        raise ImageError("invalid FAT data region")
    cluster_count = (
        (total_sectors - first_data_sector) // sectors_per_cluster
    )
    fat_bits = 12 if cluster_count < 4085 else (
        16 if cluster_count < 65525 else 32
    )
    cluster_size = bytes_per_sector * sectors_per_cluster
    fat_offset = start + reserved_sectors * bytes_per_sector

    def cluster_offset(cluster: int) -> int:
        if cluster < 2 or cluster >= cluster_count + 2:
            raise ImageError("FAT cluster is outside the data region")
        return start + (
            first_data_sector + (cluster - 2) * sectors_per_cluster
        ) * bytes_per_sector

    def next_cluster(cluster: int) -> int | None:
        if fat_bits == 12:
            entry_offset = cluster + cluster // 2
            value = struct.unpack_from(
                "<H", _read_region(stream, fat_offset + entry_offset, 2,
                                    start, end, "FAT12 entry"))[0]
            value = value >> 4 if cluster & 1 else value & 0x0FFF
            eoc = 0x0FF8
        elif fat_bits == 16:
            value = struct.unpack_from(
                "<H", _read_region(stream, fat_offset + cluster * 2, 2,
                                    start, end, "FAT16 entry"))[0]
            eoc = 0xFFF8
        else:
            value = struct.unpack_from(
                "<I", _read_region(stream, fat_offset + cluster * 4, 4,
                                    start, end, "FAT32 entry"))[0] & 0x0FFFFFFF
            eoc = 0x0FFFFFF8
        if value >= eoc:
            return None
        if value < 2:
            raise ImageError("FAT cluster chain terminates unexpectedly")
        return value

    def cluster_chain(first: int) -> Iterator[int]:
        seen: set[int] = set()
        cluster: int | None = first
        while cluster is not None:
            if cluster in seen or len(seen) > cluster_count:
                raise ImageError("FAT cluster chain contains a loop")
            seen.add(cluster)
            yield cluster
            cluster = next_cluster(cluster)

    if fat_bits == 32:
        root_cluster = struct.unpack_from("<I", boot, 44)[0] & 0x0FFFFFFF
        directory = bytearray()
        for cluster in cluster_chain(root_cluster):
            if len(directory) + cluster_size > MAX_EMBEDDED_CONFIG:
                raise ImageError("FAT root directory exceeds its bound")
            directory.extend(_read_region(
                stream, cluster_offset(cluster), cluster_size, start, end,
                "FAT root directory cluster"))
    else:
        root_offset = start + (
            reserved_sectors + fat_count * fat_sectors
        ) * bytes_per_sector
        directory = bytearray(_read_region(
            stream, root_offset, root_entries * 32, start, end,
            "FAT root directory"))

    wanted = filename.casefold()
    found: tuple[int, int] | None = None
    for offset in range(0, len(directory), 32):
        entry = directory[offset:offset + 32]
        if len(entry) != 32 or entry[0] == 0x00:
            break
        if entry[0] == 0xE5 or entry[11] == 0x0F or entry[11] & 0x18:
            continue
        raw_name = bytes(entry[:8]).rstrip(b" ").decode(
            "ascii", errors="replace")
        raw_extension = bytes(entry[8:11]).rstrip(b" ").decode(
            "ascii", errors="replace")
        short_name = raw_name + (("." + raw_extension)
                                 if raw_extension else "")
        if short_name.casefold() != wanted:
            continue
        first_cluster = struct.unpack_from("<H", entry, 26)[0]
        if fat_bits == 32:
            first_cluster |= struct.unpack_from("<H", entry, 20)[0] << 16
        found = first_cluster, struct.unpack_from("<I", entry, 28)[0]
        break
    if found is None:
        raise ImageError(f"FAT root file {filename} is missing")
    first_cluster, file_size = found
    if file_size > MAX_EMBEDDED_CONFIG:
        raise ImageError(f"FAT root file {filename} exceeds its bound")
    if file_size == 0:
        return b""
    output = bytearray()
    for cluster in cluster_chain(first_cluster):
        output.extend(_read_region(
            stream, cluster_offset(cluster), cluster_size, start, end,
            f"FAT file {filename} cluster"))
        if len(output) >= file_size:
            return bytes(output[:file_size])
    raise ImageError(f"FAT file {filename} cluster chain is truncated")


def _ext_read_file(
    stream: BinaryIO, partition: dict[str, object], path: str
) -> bytes:
    start, end = _partition_region(partition)
    superblock = _read_region(
        stream, start + 1024, 1024, start, end, "ext superblock")
    if superblock[56:58] != b"\x53\xef":
        raise ImageError("ext superblock magic is invalid")
    block_size = 1024 << struct.unpack_from("<I", superblock, 24)[0]
    inodes_per_group = struct.unpack_from("<I", superblock, 40)[0]
    inode_size = struct.unpack_from("<H", superblock, 88)[0]
    incompat = struct.unpack_from("<I", superblock, 96)[0]
    descriptor_size = struct.unpack_from("<H", superblock, 254)[0] \
        if incompat & 0x80 else 32
    if (block_size < 1024 or block_size > 65536 or
            block_size & (block_size - 1) or inodes_per_group == 0 or
            inode_size < 128 or inode_size > block_size or
            descriptor_size < 32 or descriptor_size > block_size):
        raise ImageError("invalid ext geometry")
    descriptor_table = 2 if block_size == 1024 else 1

    def read_block(block: int, description: str) -> bytes:
        if block <= 0:
            raise ImageError(f"{description} has an invalid block number")
        return _read_region(
            stream, start + block * block_size, block_size, start, end,
            description)

    def read_inode(number: int) -> bytes:
        if number <= 0:
            raise ImageError("ext inode number is invalid")
        group, index = divmod(number - 1, inodes_per_group)
        descriptor_offset = start + descriptor_table * block_size + \
            group * descriptor_size
        descriptor = _read_region(
            stream, descriptor_offset, descriptor_size, start, end,
            "ext group descriptor")
        table = struct.unpack_from("<I", descriptor, 8)[0]
        if incompat & 0x80 and descriptor_size >= 64:
            table |= struct.unpack_from("<I", descriptor, 40)[0] << 32
        inode_offset = start + table * block_size + index * inode_size
        return _read_region(stream, inode_offset, inode_size, start, end,
                            "ext inode")

    def extent_blocks(node: bytes, expected_depth: int | None = None
                      ) -> dict[int, int]:
        if len(node) < 12:
            raise ImageError("ext extent node is truncated")
        magic, entries, maximum, depth = struct.unpack_from("<HHHH", node, 0)
        if (magic != 0xF30A or entries > maximum or
                12 + entries * 12 > len(node) or depth > 5 or
                (expected_depth is not None and depth != expected_depth)):
            raise ImageError("ext extent header is invalid")
        blocks: dict[int, int] = {}
        for index in range(entries):
            entry = 12 + index * 12
            logical = struct.unpack_from("<I", node, entry)[0]
            if depth == 0:
                length = struct.unpack_from("<H", node, entry + 4)[0]
                if length == 0 or length > 0x8000:
                    raise ImageError("ext extent length is invalid")
                if length == 0x8000:
                    length = 32768
                elif length & 0x8000:
                    raise ImageError("unwritten ext extent is unsupported")
                physical = struct.unpack_from("<I", node, entry + 8)[0]
                physical |= struct.unpack_from("<H", node, entry + 6)[0] << 32
                for delta in range(length):
                    if logical + delta in blocks:
                        raise ImageError("ext extents overlap")
                    blocks[logical + delta] = physical + delta
            else:
                child = struct.unpack_from("<I", node, entry + 4)[0]
                child |= struct.unpack_from("<H", node, entry + 8)[0] << 32
                for logical_block, physical_block in extent_blocks(
                    read_block(child, "ext extent index"), depth - 1
                ).items():
                    if logical_block in blocks:
                        raise ImageError("ext extent indexes overlap")
                    blocks[logical_block] = physical_block
        return blocks

    def inode_data(inode: bytes) -> bytes:
        size = struct.unpack_from("<I", inode, 4)[0]
        size |= struct.unpack_from("<I", inode, 108)[0] << 32
        if size > MAX_EMBEDDED_CONFIG:
            raise ImageError("ext file exceeds its inspection bound")
        flags = struct.unpack_from("<I", inode, 32)[0]
        if not flags & 0x80000:
            raise ImageError("ext inode does not use supported extents")
        blocks = extent_blocks(inode[40:100])
        output = bytearray()
        for logical in range((size + block_size - 1) // block_size):
            physical = blocks.get(logical)
            output.extend(
                b"\0" * block_size if physical is None else
                read_block(physical, "ext file data")
            )
        return bytes(output[:size])

    def directory_entry(inode_number: int, name: str) -> int:
        inode = read_inode(inode_number)
        if struct.unpack_from("<H", inode, 0)[0] & 0xF000 != 0x4000:
            raise ImageError("ext path component is not a directory")
        data = inode_data(inode)
        offset = 0
        encoded_name = name.encode("utf-8")
        while offset < len(data):
            if len(data) - offset < 8:
                raise ImageError("ext directory entry is truncated")
            child, record_length = struct.unpack_from("<IH", data, offset)
            name_length = data[offset + 6]
            if (record_length < 8 or record_length & 3 or
                    offset + record_length > len(data) or
                    name_length > record_length - 8):
                raise ImageError("ext directory entry is invalid")
            if child and data[offset + 8:offset + 8 + name_length] == \
                    encoded_name:
                return child
            offset += record_length
        raise ImageError(f"ext path component {name} is missing")

    inode_number = 2
    components = [component for component in path.split("/") if component]
    if not components:
        raise ImageError("ext file path is empty")
    for component in components:
        inode_number = directory_entry(inode_number, component)
    inode = read_inode(inode_number)
    if struct.unpack_from("<H", inode, 0)[0] & 0xF000 != 0x8000:
        raise ImageError(f"ext path {path} is not a regular file")
    return inode_data(inode)


def _mbr_partitions(
    stream: BinaryIO, image_size: int
) -> list[dict[str, object]]:
    stream.seek(0)
    sector = stream.read(SECTOR_SIZE)
    if len(sector) != SECTOR_SIZE or sector[510:512] != b"\x55\xaa":
        raise ImageError("missing MBR signature")

    partitions: list[dict[str, object]] = []
    disk_signature = struct.unpack_from("<I", sector, 440)[0]
    for index in range(4):
        entry = sector[446 + index * 16:462 + index * 16]
        part_type = entry[4]
        first_lba, sectors = struct.unpack_from("<II", entry, 8)
        if part_type == 0 or sectors == 0:
            continue
        offset = first_lba * SECTOR_SIZE
        length = sectors * SECTOR_SIZE
        in_bounds = offset + length <= image_size
        partition = {
            "index": index + 1,
            "type": f"0x{part_type:02x}",
            "first_lba": first_lba,
            "sectors": sectors,
            "in_bounds": in_bounds,
            "partition_uuid": f"{disk_signature:08x}-{index + 1:02x}",
            "partition_label": None,
        }
        partition.update(
            _filesystem_metadata(stream, offset, length) if in_bounds else
            _filesystem_metadata(stream, 0, 0)
        )
        partitions.append(partition)
    return partitions


def _gpt_partitions(stream: BinaryIO, image_size: int) -> dict[str, object]:
    stream.seek(SECTOR_SIZE)
    header_sector = stream.read(SECTOR_SIZE)
    if header_sector[:8] != b"EFI PART":
        raise ImageError("protective MBR is present but GPT header is missing")

    header_size, stored_header_crc = struct.unpack_from(
        "<II", header_sector, 12
    )
    if header_size < 92 or header_size > SECTOR_SIZE:
        raise ImageError("invalid GPT header size")
    header = bytearray(header_sector[:header_size])
    struct.pack_into("<I", header, 16, 0)
    computed_header_crc = binascii.crc32(header) & 0xFFFFFFFF

    entries_lba = struct.unpack_from("<Q", header_sector, 72)[0]
    entry_count, entry_size, stored_entries_crc = struct.unpack_from(
        "<III", header_sector, 80
    )
    entries_bytes = entry_count * entry_size
    if entry_size < 128 or entries_bytes > image_size:
        raise ImageError("invalid GPT partition-entry array size")

    stream.seek(entries_lba * SECTOR_SIZE)
    entries = stream.read(entries_bytes)
    if len(entries) != entries_bytes:
        raise ImageError("truncated GPT partition-entry array")
    computed_entries_crc = binascii.crc32(entries) & 0xFFFFFFFF

    partitions: list[dict[str, object]] = []
    for index in range(entry_count):
        entry = entries[index * entry_size:(index + 1) * entry_size]
        if entry[:16] == b"\0" * 16:
            continue
        first_lba, last_lba = struct.unpack_from("<QQ", entry, 32)
        if last_lba < first_lba:
            in_bounds = False
            length = 0
        else:
            length = (last_lba - first_lba + 1) * SECTOR_SIZE
            in_bounds = (last_lba + 1) * SECTOR_SIZE <= image_size
        name = entry[56:128].decode("utf-16-le", errors="replace").rstrip("\0")
        partition = {
            "index": index + 1,
            "name": name,
            "first_lba": first_lba,
            "last_lba": last_lba,
            "in_bounds": in_bounds,
            "partition_uuid": str(uuid.UUID(bytes_le=entry[16:32])),
            "partition_label": name or None,
        }
        partition.update(
            _filesystem_metadata(
                stream, first_lba * SECTOR_SIZE, length
            ) if in_bounds else _filesystem_metadata(stream, 0, 0)
        )
        partitions.append(partition)

    return {
        "header_crc_valid": stored_header_crc == computed_header_crc,
        "entries_crc_valid": stored_entries_crc == computed_entries_crc,
        "partitions": partitions,
    }


def _identity_references(text: str) -> Iterator[tuple[str, str]]:
    pattern = re.compile(
        r"(?<![A-Z0-9_])(PARTUUID|PARTLABEL|UUID|LABEL)=([^\s,]+)",
        re.IGNORECASE,
    )
    for line in text.splitlines():
        content = line.split("#", 1)[0]
        for match in pattern.finditer(content):
            yield match.group(1).upper(), match.group(2).strip("\"'")


def _validate_identity_references(
    partitions: list[dict[str, object]], reference_texts: dict[str, str]
) -> dict[str, object]:
    fields = {
        "PARTUUID": "partition_uuid",
        "PARTLABEL": "partition_label",
        "UUID": "filesystem_uuid",
        "LABEL": "filesystem_label",
    }
    references: list[dict[str, object]] = []
    for source, text in reference_texts.items():
        for kind, value in _identity_references(text):
            field = fields[kind]
            casefold = kind in {"PARTUUID", "UUID"}
            expected = value.casefold() if casefold else value
            matches = [
                int(partition["index"])
                for partition in partitions
                if partition[field] is not None and
                ((str(partition[field]).casefold() if casefold else
                  str(partition[field])) == expected)
            ]
            reference = {
                "source": source, "kind": kind, "value": value,
                "partition_indexes": matches,
            }
            references.append(reference)
            if not matches:
                raise ImageError(
                    f"{source} references unknown {kind}={value}"
                )
            if len(matches) != 1:
                raise ImageError(
                    f"{source} references ambiguous {kind}={value}"
                )
    if not references:
        raise ImageError("boot configuration contains no UUID/label references")
    return {"valid": True, "references": references}


def _contained_identity_references(
    stream: BinaryIO, partitions: list[dict[str, object]]
) -> dict[str, str]:
    fat = [partition for partition in partitions
           if partition["filesystem"] == "fat"]
    ext = [partition for partition in partitions
           if partition["filesystem"] == "ext"]
    if len(fat) != 1 or len(ext) != 1:
        raise ImageError(
            "contained identity validation requires exactly one FAT and "
            "one ext partition"
        )
    files = {
        f"partition-{fat[0]['index']}:/cmdline.txt":
            _fat_read_file(stream, fat[0], "cmdline.txt"),
        f"partition-{ext[0]['index']}:/etc/fstab":
            _ext_read_file(stream, ext[0], "/etc/fstab"),
    }
    references: dict[str, str] = {}
    for source, data in files.items():
        if b"\0" in data:
            raise ImageError(f"{source} contains a NUL byte")
        try:
            references[source] = data.decode("utf-8")
        except UnicodeDecodeError as error:
            raise ImageError(f"{source} is not valid UTF-8") from error
    return references


def inspect_image(
    image: Path, reference_texts: dict[str, str] | None = None,
    validate_contained: bool = False,
) -> dict[str, object]:
    """Return partition-table and filesystem-signature facts for an image."""

    if not image.is_file():
        raise ImageError("image must be a regular file")
    image_size = image.stat().st_size
    if image_size < SECTOR_SIZE:
        raise ImageError("image is smaller than one sector")

    with image.open("rb") as stream:
        mbr = _mbr_partitions(stream, image_size)
        protective = any(part["type"] == "0xee" for part in mbr)
        if protective:
            gpt = _gpt_partitions(stream, image_size)
            result = {
                "image": str(image),
                "bytes": image_size,
                "partition_table": "gpt",
                **gpt,
            }
        else:
            result = {
                "image": str(image),
                "bytes": image_size,
                "partition_table": "mbr",
                "partitions": mbr,
            }
        contained = (_contained_identity_references(
            stream, result["partitions"]) if validate_contained else {})
    references = {**contained, **(reference_texts or {})}
    if references:
        result["identity_validation"] = _validate_identity_references(
            result["partitions"], references
        )
    return result


def decode_boot_order(value: int) -> Iterator[str]:
    """Decode Raspberry Pi EEPROM BOOT_ORDER from low to high nibble."""

    names = {
        0x0: "sd-card-detect",
        0x1: "sd-card",
        0x2: "network",
        0x3: "rpiboot",
        0x4: "usb-msd",
        0x5: "bcm-usb-msd",
        0x6: "nvme",
        0x7: "http",
        0xE: "stop",
        0xF: "restart",
    }
    if value < 0 or value > 0xFFFFFFFF:
        raise ImageError("BOOT_ORDER must fit in 32 bits")
    if value == 0:
        yield names[0]
        return
    while value:
        nibble = value & 0xF
        yield names.get(nibble, f"reserved-0x{nibble:x}")
        value >>= 4


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    flash = subparsers.add_parser("flash", help="write and verify a raw image")
    flash.add_argument("source", type=Path)
    flash.add_argument("target", type=Path)
    flash.add_argument("--chunk-size", type=_positive_int,
                       default=DEFAULT_CHUNK_SIZE)
    flash.add_argument("--fail-after", type=_non_negative_int)
    flash.add_argument("--no-verify", action="store_true")
    flash.add_argument(
        "--allow-block-device", action="store_true",
        help="allow an unmounted, writable, removable Linux whole device",
    )
    flash.add_argument(
        "--resume", action="store_true",
        help="verify a durable journal and continue an interrupted flash",
    )
    flash.add_argument(
        "--resume-journal", type=Path,
        help=("journal path (required for block devices; otherwise "
              "TARGET.rpi-resume.json)"),
    )

    inspect = subparsers.add_parser("inspect", help="inspect MBR/GPT metadata")
    inspect.add_argument("image", type=Path)
    inspect.add_argument(
        "--boot-config", action="append", default=[], type=Path,
        help="validate UUID/label references in cmdline or boot configuration",
    )
    inspect.add_argument(
        "--fstab", action="append", default=[], type=Path,
        help="validate UUID/label references in an fstab file",
    )
    inspect.add_argument(
        "--validate-contained-identities", action="store_true",
        help="read and validate /cmdline.txt and /etc/fstab from the image",
    )

    boot_order = subparsers.add_parser(
        "boot-order", help="decode an EEPROM BOOT_ORDER value"
    )
    boot_order.add_argument("value", type=lambda value: int(value, 0))
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    try:
        if args.command == "flash":
            result = flash_image(
                args.source,
                args.target,
                chunk_size=args.chunk_size,
                verify=not args.no_verify,
                fail_after=args.fail_after,
                allow_block_device=args.allow_block_device,
                resume=args.resume,
                journal_path=args.resume_journal,
            )
        elif args.command == "inspect":
            references = {
                str(path): path.read_text(encoding="utf-8")
                for path in [*args.boot_config, *args.fstab]
            }
            result = inspect_image(
                args.image, references or None,
                validate_contained=args.validate_contained_identities,
            )
        else:
            result = {
                "value": f"0x{args.value:x}",
                "attempt_order": list(decode_boot_order(args.value)),
            }
    except PowerLossInjected as error:
        print(json.dumps({"status": "power-loss", "error": str(error)}))
        return POWER_LOSS_EXIT
    except (ImageError, OSError, ValueError) as error:
        print(f"rpi-image: {error}", file=sys.stderr)
        return 1

    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
