#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import argparse
import hashlib
import json
from pathlib import Path


REQUIRED_MARKERS = (
    "Linux version 6.18.34+rpt-rpi-v8",
    "QEMU-RPI-WIFI-DIAG: brcmfmac-status=0",
    "QEMU-RPI-WIFI-DIAG: wcc-status=0",
    "post-bind=mmc1:0001:1 driver=",
    "post-bind=mmc1:0001:2 driver=",
    "QEMU-RPI-WIFI-DIAG: netdev=wlan0",
    "QEMU-RPI-WIFI-DIAG: link-up-status=0",
    "QEMU-RPI-WIFI-DIAG: dhcp-status=0",
    "QEMU-RPI-WIFI-DIAG: ping-status=0",
    "QEMU-RPI-BT-DIAG: bluetooth-status=0",
    "QEMU-RPI-BT-DIAG: btbcm-status=0",
    "QEMU-RPI-BT-DIAG: hci_uart-status=0",
    "QEMU-RPI-BT-DIAG: hci0-present=0",
    "Bluetooth: HCI UART protocol Broadcom registered",
    "Bluetooth: hci0: BCM: chip id 74",
    "Bluetooth: hci0: BCM: features 0x01",
    "Bluetooth: hci0: QEMU CYW43455 Bluetooth",
    "Bluetooth: hci0: BCM4345C0 (003.001.025) build 0001",
)

FORBIDDEN_MARKERS = (
    "unexpected cc",
    "Opcode 0x",
    "QEMU-RPI-WIFI-DIAG: brcmfmac-status=1",
    "QEMU-RPI-WIFI-DIAG: wcc-status=1",
    "QEMU-RPI-BT-DIAG: bluetooth-status=1",
    "QEMU-RPI-BT-DIAG: btbcm-status=1",
    "QEMU-RPI-BT-DIAG: hci_uart-status=1",
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify(paths: dict[str, Path]) -> dict:
    for name, path in paths.items():
        if not path.is_file():
            raise ValueError(f"{name} is not a regular file: {path}")
    text = paths["console_log"].read_text(errors="replace")
    missing = [marker for marker in REQUIRED_MARKERS if marker not in text]
    forbidden = [marker for marker in FORBIDDEN_MARKERS if marker in text]
    if missing:
        raise ValueError("missing console markers: " + "; ".join(missing))
    if forbidden:
        raise ValueError("forbidden console markers: " + "; ".join(forbidden))
    return {
        "schema": "qemu-rpi-wireless-release-evidence-v1",
        "result": "pass",
        "artifacts": {
            name: {
                "filename": path.name,
                "bytes": path.stat().st_size,
                "sha256": sha256(path),
            }
            for name, path in sorted(paths.items())
        },
        "required_markers": list(REQUIRED_MARKERS),
        "forbidden_markers": list(FORBIDDEN_MARKERS),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    for name in ("qemu", "kernel", "dtb", "initramfs",
                 "module_package", "console_log"):
        parser.add_argument(f"--{name.replace('_', '-')}", type=Path,
                            required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    paths = {
        name: getattr(args, name)
        for name in ("qemu", "kernel", "dtb", "initramfs",
                     "module_package", "console_log")
    }
    try:
        report = verify(paths)
    except ValueError as error:
        parser.error(str(error))
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
