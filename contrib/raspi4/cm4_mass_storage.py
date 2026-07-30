#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Expose a virtual CM4 eMMC image through Linux USB mass storage.

This is the post-rpiboot transport stage.  It binds the same regular-file
backend later passed to ``raspi-cm4,emmc-drive=...`` to a Linux configfs mass
storage gadget.  Standard host tools, including Raspberry Pi Imager and dd,
therefore write the exact byte stream that the virtual CM4 subsequently boots.

The tool deliberately prints a stable /dev/disk/by-id path and refuses unsafe
images or teardown while the exported disk (or one of its partitions) is
mounted.
"""

from __future__ import annotations

import argparse
import fcntl
import io
import os
from pathlib import Path
import re
import signal
import stat
import subprocess
import sys
import time


GADGET_NAME = "qemu_rpi_cm4"
SERIAL = "51554d5552504934"  # ASCII-ish stable identity: QEMURPI4
VENDOR_ID = "0x1d6b"
PRODUCT_ID = "0x0104"
COMPAT_BY_ID_NAME = f"usb-Linux_File-Stor_Gadget_{SERIAL}-0:0"
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


class GadgetError(RuntimeError):
    """A safe, user-actionable gadget setup failure."""


class TerminationRequested(GadgetError):
    """The foreground gadget owner was asked to terminate."""


def _write(path: Path, value: str) -> None:
    path.write_text(value, encoding="ascii")


def read_lifecycle(path: Path) -> str:
    try:
        state = path.read_text(encoding="ascii").strip()
    except (OSError, UnicodeError) as error:
        raise GadgetError(
            f"cannot read lifecycle state {path}: {error}"
        ) from error
    if state not in LIFECYCLE_STATES:
        raise GadgetError(f"invalid lifecycle state in {path}: {state!r}")
    return state


def write_lifecycle(path: Path, state: str) -> None:
    if state not in LIFECYCLE_STATES:
        raise GadgetError(f"invalid lifecycle state: {state!r}")
    temporary = path.with_name(f".{path.name}.tmp.{os.getpid()}")
    try:
        metadata = path.stat()
        fd = os.open(
            temporary,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
            0o644,
        )
        try:
            if os.geteuid() == 0:
                os.fchown(fd, metadata.st_uid, metadata.st_gid)
            os.fchmod(fd, stat.S_IMODE(metadata.st_mode))
            contents = f"{state}\n".encode("ascii")
            offset = 0
            while offset < len(contents):
                offset += os.write(fd, contents[offset:])
            os.fsync(fd)
        finally:
            os.close(fd)
        os.replace(temporary, path)
    except OSError as error:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass
        raise GadgetError(
            f"cannot publish lifecycle state {path}: {error}"
        ) from error


def transition_lifecycle(
        path: Path | None, expected: set[str], state: str) -> None:
    if path is None:
        return
    current = read_lifecycle(path)
    if current not in expected:
        allowed = ", ".join(sorted(expected))
        raise GadgetError(
            f"lifecycle state {current!r} cannot transition to {state!r}; "
            f"expected one of: {allowed}"
        )
    write_lifecycle(path, state)


def acquire_lifecycle_lock(path: Path | None) -> int:
    if path is None:
        return -1
    lock_path = path.with_name(f"{path.name}.lock")
    try:
        fd = os.open(lock_path, os.O_RDWR | os.O_CREAT | os.O_CLOEXEC, 0o644)
        try:
            fcntl.lockf(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            os.close(fd)
            raise
    except OSError as error:
        raise GadgetError(
            f"CM4 provision lifecycle is owned by another process: {lock_path}"
        ) from error
    return fd


def validate_image(value: str) -> Path:
    """Return a resolved, sector-aligned regular file suitable as an eMMC."""
    path = Path(value).expanduser().resolve(strict=True)
    info = path.stat()
    if not stat.S_ISREG(info.st_mode):
        raise GadgetError(f"eMMC backend is not a regular file: {path}")
    if info.st_size == 0 or info.st_size % 512:
        raise GadgetError(
            f"eMMC backend size must be non-zero and 512-byte aligned: "
            f"{info.st_size}"
        )
    if not os.access(path, os.R_OK | os.W_OK):
        raise GadgetError(f"eMMC backend is not readable and writable: {path}")
    return path


def require_root(configfs_root: Path) -> None:
    """Require privilege for the real kernel configfs.

    Fake trees used by the unit tests are permitted.
    """
    try:
        configfs_root.resolve(
            ).relative_to(Path("/sys/kernel/config").resolve()
        )
    except ValueError:
        return
    if os.geteuid() != 0:
        raise GadgetError("run this command as root (for example with sudo)")


def ensure_kernel_module(
    module: str,
    *parameters: str,
    module_root: Path = Path("/sys/module"),
) -> None:
    module_path = module_root / module

    if module_path.is_dir():
        for assignment in parameters:
            name, separator, expected = assignment.partition("=")
            parameter = module_path / "parameters" / name
            if not separator or not name or not expected:
                raise GadgetError(
                    f"invalid {module} module parameter {assignment!r}"
                )
            try:
                actual = parameter.read_text(encoding="ascii").strip()
            except (OSError, UnicodeError) as error:
                raise GadgetError(
                    f"cannot verify loaded {module} parameter {name}: {error}"
                ) from error
            normalized = {
                "0": "0", "N": "0", "n": "0", "false": "0",
                "1": "1", "Y": "1", "y": "1", "true": "1",
            }
            if normalized.get(actual, actual) != normalized.get(
                expected, expected
            ):
                raise GadgetError(
                    f"loaded {module} parameter {name} differs: "
                    f"expected {expected}, found {actual}"
                )
        return

    subprocess.run(["modprobe", module, *parameters], check=True)
    if not module_path.is_dir():
        raise GadgetError(f"modprobe did not load required module {module}")


def load_kernel_support(configfs_root: Path) -> None:
    if configfs_root == Path("/sys/kernel/config/usb_gadget"):
        ensure_kernel_module("dummy_hcd", "is_high_speed=1")
        ensure_kernel_module("libcomposite")
    if not configfs_root.is_dir():
        raise GadgetError(
            f"USB gadget configfs is unavailable: {configfs_root}"
        )


def gadget_path(configfs_root: Path) -> Path:
    return configfs_root / GADGET_NAME


def create_gadget(configfs_root: Path, image: Path, udc: str) -> Path:
    """Create and bind one guarded configfs mass-storage gadget."""
    root = gadget_path(configfs_root)
    if root.exists():
        raise GadgetError(
            f"gadget already exists at {root}; use 'status' or 'stop' first"
        )

    try:
        root.mkdir()
        _write(root / "idVendor", VENDOR_ID)
        _write(root / "idProduct", PRODUCT_ID)
        _write(root / "bcdUSB", "0x0200")
        _write(root / "bcdDevice", "0x0100")

        strings = root / "strings" / "0x409"
        strings.mkdir()
        _write(strings / "serialnumber", SERIAL)
        _write(strings / "manufacturer", "Raspberry Pi")
        _write(strings / "product", "Compute Module 4 eMMC")

        config = root / "configs" / "c.1"
        config.mkdir()
        config_strings = config / "strings" / "0x409"
        config_strings.mkdir()
        _write(config_strings / "configuration", "RPIBOOT eMMC")
        _write(config / "MaxPower", "2")

        function = root / "functions" / "mass_storage.usb0"
        function.mkdir()
        lun = function / "lun.0"
        _write(lun / "removable", "0")
        _write(lun / "ro", "0")
        _write(lun / "nofua", "0")
        _write(lun / "file", str(image))
        (config / "mass_storage.usb0").symlink_to(function)

        _write(root / "UDC", udc)
    except Exception:
        try:
            destroy_gadget(configfs_root, check_mounts=False)
        except Exception:
            pass
        raise
    return root


def backing_image(configfs_root: Path) -> Path | None:
    lun_file = (gadget_path(configfs_root)
                / "functions/mass_storage.usb0/lun.0/file")
    if not lun_file.exists():
        return None
    value = lun_file.read_text(encoding="ascii").strip()
    return Path(value).resolve() if value else None


def gadget_is_active(configfs_root: Path) -> bool:
    """Return whether the USB gadget exists, even if its medium was ejected."""
    return gadget_path(configfs_root).is_dir()


def by_id_path(dev_root: Path) -> Path:
    """Return the serial-bound whole-disk link, independent of SCSI naming."""
    by_id = dev_root / "disk" / "by-id"
    candidates = sorted(
        path for path in by_id.glob(f"*{SERIAL}*")
        if not re.search(r"-part[0-9]+$", path.name)
    )
    if len(candidates) == 1:
        return candidates[0]
    if len(candidates) > 1:
        raise GadgetError(
            f"CM4 serial {SERIAL} resolves to multiple whole-disk links: "
            + ", ".join(map(str, candidates))
        )
    # Retain a deterministic path in diagnostics before enumeration.
    return by_id / COMPAT_BY_ID_NAME


def wait_for_block_device(dev_root: Path, timeout: float) -> Path:
    stable = by_id_path(dev_root)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if stable.exists():
            return stable
        time.sleep(0.1)
    raise GadgetError(
        f"USB mass-storage block device did not appear at {stable}"
    )


def block_device(dev_root: Path) -> Path:
    stable = by_id_path(dev_root)
    if not stable.exists():
        raise GadgetError(f"USB mass-storage block device is absent: {stable}")
    return stable.resolve(strict=True)


def sectors_written(device: Path, sys_block_root: Path) -> int:
    stat_path = sys_block_root / device.name / "stat"
    try:
        fields = stat_path.read_text(encoding="ascii").split()
        return int(fields[6])
    except (OSError, ValueError, IndexError) as error:
        raise GadgetError(
            f"cannot read block write statistics: {stat_path}"
        ) from error


def flushes_completed(device: Path, sys_block_root: Path) -> int:
    """Return completed host flush requests from Linux block statistics."""
    stat_path = sys_block_root / device.name / "stat"
    try:
        fields = stat_path.read_text(encoding="ascii").split()
        # Linux block stat field 16 is the number of completed flush requests.
        return int(fields[15])
    except (OSError, ValueError, IndexError) as error:
        raise GadgetError(
            f"cannot read block flush statistics: {stat_path}"
        ) from error


def wait_for_block_writes(device: Path, sys_block_root: Path, baseline: int,
                          after_sectors: int, timeout: float,
                          poll_interval: float = 0.01) -> int:
    """Wait for an exact minimum number of host sectors to be issued."""
    if after_sectors <= 0:
        raise GadgetError("fault write threshold must be positive")
    if timeout <= 0:
        raise GadgetError("fault watch timeout must be positive")
    target = baseline + after_sectors
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        current = sectors_written(device, sys_block_root)
        if current >= target:
            return current
        time.sleep(poll_interval)
    raise GadgetError(
        f"host did not write {after_sectors} sectors within {timeout:g} seconds"
    )


def wait_for_block_flushes(device: Path, sys_block_root: Path, baseline: int,
                           after_flushes: int, timeout: float,
                           poll_interval: float = 0.01) -> int:
    """Wait for host SYNCHRONIZE CACHE/flush requests to complete."""
    if after_flushes <= 0:
        raise GadgetError("fault flush threshold must be positive")
    if timeout <= 0:
        raise GadgetError("fault watch timeout must be positive")
    target = baseline + after_flushes
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        current = flushes_completed(device, sys_block_root)
        if current >= target:
            return current
        time.sleep(poll_interval)
    raise GadgetError(
        f"host did not complete {after_flushes} flush requests within "
        f"{timeout:g} seconds"
    )


def exported_block_names(dev_root: Path) -> set[str]:
    stable = by_id_path(dev_root)
    if not stable.exists():
        return set()
    disk = stable.resolve()
    names = {disk.name}
    sys_block = Path("/sys/class/block") / disk.name
    if sys_block.is_dir():
        names.update(
            path.name for path in sys_block.iterdir()
            if path.name != disk.name and (path / "partition").exists()
        )
    return names


def mounted_exports(
    dev_root: Path,
    mountinfo: Path = Path("/proc/self/mountinfo"),
) -> list[str]:
    names = exported_block_names(dev_root)
    if not names:
        return []
    mounted: list[str] = []
    for line in mountinfo.read_text(encoding="utf-8").splitlines():
        fields = line.split()
        separator = fields.index("-") if "-" in fields else -1
        if separator < 0 or separator + 2 >= len(fields):
            continue
        source = fields[separator + 2]
        source_path = Path(source)
        source_name = (
            source_path.resolve().name if source_path.exists()
            else source_path.name
        )
        if source_name in names:
            mounted.append(f"{source} on {fields[4]}")
    return mounted


def destroy_gadget(configfs_root: Path, *, check_mounts: bool = True,
                   dev_root: Path = Path("/dev")) -> None:
    root = gadget_path(configfs_root)
    if not root.exists():
        raise GadgetError(f"gadget is not active: {root}")
    if check_mounts:
        mounted = mounted_exports(dev_root)
        if mounted:
            raise GadgetError(
                "refusing to disconnect mounted virtual eMMC: " + "; ".join(
                    mounted
                )
            )

    udc = root / "UDC"
    if udc.exists():
        _write(udc, "")
    link = root / "configs/c.1/mass_storage.usb0"
    if link.is_symlink():
        link.unlink()
    lun_file = root / "functions/mass_storage.usb0/lun.0/file"
    if lun_file.exists():
        _write(lun_file, "")

    # Configfs creates the lun.0 directory as part of the function; removing
    # the function removes it as well.
    for path in (
        root / "functions/mass_storage.usb0",
        root / "configs/c.1/strings/0x409",
        root / "configs/c.1",
        root / "strings/0x409",
        root,
    ):
        path.rmdir()


def force_eject(configfs_root: Path) -> None:
    """Remove the SCSI medium while leaving the USB device enumerated."""
    eject = (gadget_path(configfs_root) /
             "functions/mass_storage.usb0/lun.0/forced_eject")
    if not eject.exists():
        raise GadgetError(
            f"mass-storage forced-eject control is absent: {eject}"
        )
    _write(eject, "1")


def _report_export(image: Path, stable: Path, output: io.TextIOBase) -> None:
    print(f"CM4 eMMC backend: {image}", file=output)
    print(f"Host imaging target: {stable} -> {stable.resolve()}", file=output)
    print("Only write the exact Host imaging target shown above.", file=output)


def activate_gadget(configfs_root: Path, dev_root: Path, image: Path,
                    udc: str, timeout: float,
                    lifecycle: Path | None) -> Path:
    """Create, enumerate, and publish ownership of one mass-storage LUN."""
    create_gadget(configfs_root, image, udc)
    try:
        stable = wait_for_block_device(dev_root, timeout)
        transition_lifecycle(
            lifecycle, {"rpiboot-complete", "flash-failed"},
            "mass-storage-active"
        )
    except Exception:
        destroy_gadget(configfs_root, check_mounts=False, dev_root=dev_root)
        raise
    return stable


def serve_gadget(configfs_root: Path, dev_root: Path, image: Path,
                 udc: str, timeout: float, lifecycle: Path | None,
                 input_stream: io.TextIOBase = sys.stdin,
                 output: io.TextIOBase = sys.stdout) -> None:
    """Own the LUN until the controller explicitly reports its result.

    The caller must already hold the lifecycle lock.  Keeping this function
    in the foreground makes that lock cover the entire host imaging session,
    instead of only the configfs setup and teardown system calls.
    """
    stable = activate_gadget(
        configfs_root, dev_root, image, udc, timeout, lifecycle
    )
    _report_export(image, stable, output)
    print(
        "CM4 mass-storage owner ready; send 'complete' or 'failed' on stdin",
        file=output, flush=True,
    )

    command = ""
    pending_error: BaseException | None = None
    try:
        command = input_stream.readline().strip()
        if command not in {"complete", "failed"}:
            raise GadgetError(
                "foreground owner requires exactly 'complete' or 'failed' "
                "on stdin"
            )
    except BaseException as error:
        pending_error = error

    # A missing/invalid completion acknowledgement is always a failed flash.
    # If teardown itself fails, retain mass-storage-active so that QEMU still
    # fails closed and an operator can recover with the explicit stop command.
    destroy_gadget(configfs_root, dev_root=dev_root)
    if lifecycle is not None:
        write_lifecycle(
            lifecycle,
            "boot-ready" if command == "complete" and pending_error is None
            else "flash-failed",
        )
    if pending_error is not None:
        raise pending_error
    print(f"Disconnected CM4 eMMC backend: {image}", file=output, flush=True)


def recover_stale_gadget(configfs_root: Path, dev_root: Path,
                         lifecycle: Path | None) -> Path | None:
    """Tear down an orphaned configfs owner and retain failed media state.

    The caller must hold the lifecycle lock.  An absent gadget covers a crash
    after kernel teardown but before the lifecycle transition was published.
    """
    if lifecycle is None:
        raise GadgetError("recover-stale requires --lifecycle")
    current = read_lifecycle(lifecycle)
    if current not in {"mass-storage-active", "flash-failed"}:
        raise GadgetError(
            "recover-stale requires lifecycle state 'mass-storage-active' "
            "or 'flash-failed'"
        )
    image = backing_image(configfs_root)
    if gadget_path(configfs_root).exists():
        destroy_gadget(configfs_root, dev_root=dev_root)
    write_lifecycle(lifecycle, "flash-failed")
    return image


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--configfs-root", type=Path,
        default=Path("/sys/kernel/config/usb_gadget"),
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--dev-root", type=Path, default=Path("/dev"),
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--sys-block-root", type=Path, default=Path("/sys/class/block"),
        help=argparse.SUPPRESS,
    )
    parser.add_argument("--lifecycle", type=Path)
    subparsers = parser.add_subparsers(dest="command", required=True)

    start = subparsers.add_parser("start", help="export an eMMC image")
    start.add_argument("image")
    start.add_argument("--udc", default="dummy_udc.0")
    start.add_argument("--timeout", type=float, default=10.0)
    serve = subparsers.add_parser(
        "serve",
        help="hold exclusive ownership until stdin reports complete or failed",
    )
    serve.add_argument("image")
    serve.add_argument("--udc", default="dummy_udc.0")
    serve.add_argument("--timeout", type=float, default=10.0)
    subparsers.add_parser(
        "status", help="show the active backend and host block path"
    )
    stop = subparsers.add_parser(
        "stop", help="safely disconnect and remove the gadget"
    )
    stop.add_argument("--result", choices=("complete", "failed"),
                      default="complete")
    subparsers.add_parser(
        "recover-stale",
        help="tear down an orphaned owner and retain flash-failed state",
    )
    fault = subparsers.add_parser(
        "fault-disconnect",
        help="disconnect USB after a host write-sector threshold",
    )
    fault.add_argument("--after-sectors", type=int, default=1)
    fault.add_argument("--timeout", type=float, default=30.0)
    eject = subparsers.add_parser(
        "fault-eject",
        help="force SCSI medium removal after a host write-sector threshold",
    )
    eject.add_argument("--after-sectors", type=int, default=1)
    eject.add_argument("--timeout", type=float, default=30.0)
    flush_eject = subparsers.add_parser(
        "fault-flush-eject",
        help="force medium removal after a completed host flush request",
    )
    flush_eject.add_argument("--after-flushes", type=int, default=1)
    flush_eject.add_argument("--timeout", type=float, default=30.0)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    configfs_root: Path = args.configfs_root
    dev_root: Path = args.dev_root
    sys_block_root: Path = args.sys_block_root
    lifecycle: Path | None = args.lifecycle
    lifecycle_lock_fd = -1
    try:
        require_root(configfs_root)
        load_kernel_support(configfs_root)
        lifecycle_lock_fd = acquire_lifecycle_lock(lifecycle)
        if args.command in {"start", "serve"}:
            image = validate_image(args.image)
            if lifecycle is not None:
                current = read_lifecycle(lifecycle)
                if current not in {"rpiboot-complete", "flash-failed"}:
                    raise GadgetError(
                        "mass storage requires lifecycle state "
                        f"rpiboot-complete or flash-failed, found {current!r}"
                    )
            if args.command == "serve":
                previous_handler = signal.signal(
                    signal.SIGTERM,
                    lambda _signum, _frame: (_ for _ in ()).throw(
                        TerminationRequested("foreground owner was terminated")
                    ),
                )
                try:
                    serve_gadget(
                        configfs_root, dev_root, image, args.udc,
                        args.timeout, lifecycle
                    )
                finally:
                    signal.signal(signal.SIGTERM, previous_handler)
            else:
                stable = activate_gadget(
                    configfs_root, dev_root, image, args.udc,
                    args.timeout, lifecycle
                )
                _report_export(image, stable, sys.stdout)
        elif args.command == "status":
            if not gadget_is_active(configfs_root):
                raise GadgetError("CM4 eMMC gadget is not active")
            image = backing_image(configfs_root)
            stable = by_id_path(dev_root)
            target = (f" -> {stable.resolve()}" if stable.exists()
                      else " (not enumerated)")
            backend = str(image) if image is not None else "<medium ejected>"
            print(f"CM4 eMMC backend: {backend}")
            print(f"Host imaging target: {stable}{target}")
        elif args.command == "stop":
            if not gadget_is_active(configfs_root):
                raise GadgetError("CM4 eMMC gadget is not active")
            image = backing_image(configfs_root)
            if lifecycle is not None:
                current = read_lifecycle(lifecycle)
                if current not in {"mass-storage-active", "flash-failed"}:
                    raise GadgetError(
                        "stop requires lifecycle state 'mass-storage-active' "
                        "or 'flash-failed'"
                    )
                if current == "flash-failed" and args.result == "complete":
                    raise GadgetError(
                        "a failed flash cannot be promoted to boot-ready"
                    )
            destroy_gadget(configfs_root, dev_root=dev_root)
            if lifecycle is not None:
                write_lifecycle(
                    lifecycle,
                    "boot-ready" if args.result == "complete" else
                    "flash-failed"
                )
            backend = str(image) if image is not None else "<medium ejected>"
            print(f"Disconnected CM4 eMMC backend: {backend}")
        elif args.command == "recover-stale":
            image = recover_stale_gadget(configfs_root, dev_root, lifecycle)
            backend = (str(image) if image is not None
                       else "<unknown or ejected>")
            print(
                "Recovered stale CM4 mass-storage owner as flash-failed: "
                f"{backend}"
            )
        else:
            image = backing_image(configfs_root)
            if image is None:
                raise GadgetError("CM4 eMMC gadget is not active")
            if lifecycle is not None and read_lifecycle(lifecycle) != \
                    "mass-storage-active":
                raise GadgetError(
                    "fault injection requires lifecycle state "
                    "'mass-storage-active'"
                )
            device = block_device(dev_root)
            if args.command == "fault-flush-eject":
                baseline = flushes_completed(device, sys_block_root)
                print(
                    "Watching CM4 eMMC host flushes before injected "
                    "SCSI medium removal: "
                    f"device={device} baseline-flushes={baseline} "
                    f"after-flushes={args.after_flushes}",
                    flush=True,
                )
                current = wait_for_block_flushes(
                    device, sys_block_root, baseline, args.after_flushes,
                    args.timeout
                )
                force_eject(configfs_root)
                detail = (
                    "Injected CM4 eMMC SCSI medium removal after host flush: "
                    f"baseline-flushes={baseline} "
                    f"observed-flushes={current} backend={image}"
                )
            else:
                baseline = sectors_written(device, sys_block_root)
                fault_label = (
                    "SCSI medium removal" if args.command == "fault-eject" else
                    "disconnect"
                )
                print(
                    "Watching CM4 eMMC host writes before injected "
                    f"{fault_label}: device={device} "
                    f"baseline-sectors={baseline} "
                    f"after-sectors={args.after_sectors}",
                    flush=True,
                )
                current = wait_for_block_writes(
                    device, sys_block_root, baseline, args.after_sectors,
                    args.timeout
                )
                if args.command == "fault-eject":
                    force_eject(configfs_root)
                else:
                    destroy_gadget(configfs_root, dev_root=dev_root)
                detail = (
                    ("Injected CM4 eMMC SCSI medium removal during host write: "
                     if args.command == "fault-eject" else
                     "Injected CM4 eMMC USB disconnect during host write: ") +
                    f"baseline-sectors={baseline} observed-sectors={current} "
                    f"backend={image}"
                )
            if lifecycle is not None:
                write_lifecycle(lifecycle, "flash-failed")
            print(detail)
        return 0
    except (GadgetError, OSError, subprocess.CalledProcessError) as error:
        print(f"cm4-mass-storage: {error}", file=sys.stderr)
        return 1
    finally:
        if lifecycle_lock_fd >= 0:
            os.close(lifecycle_lock_fd)


if __name__ == "__main__":
    raise SystemExit(main())
