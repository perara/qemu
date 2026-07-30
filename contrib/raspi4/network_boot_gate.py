#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run Raspberry Pi behavioral network boot against a real dnsmasq/TFTP."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from typing import Any

SCRIPT_DIRECTORY = Path(__file__).resolve().parent
if str(SCRIPT_DIRECTORY) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIRECTORY))

from cm4_provision import ProvisionError, QMPClient
import network_cadence


class GateError(RuntimeError):
    pass


MANIFEST_SCHEMA = "qemu-rpi-network-boot-manifest-v2"
EEPROM_SIZE = 512 * 1024
EEPROM_UPDATE_RESULTS = {
    "success", "invalid-signature", "write-protected", "program-failure",
}
CAPTURE_MAX_PACKETS = 1_000_000
CAPTURE_MAX_BYTES = 1024 * 1024 * 1024


class TapCapture:
    def __init__(self, interface: str):
        self.interface = interface
        self.socket = socket.socket(
            socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x0003))
        self.socket.bind((interface, 0))
        self.socket.settimeout(0.05)
        self.stop_event = threading.Event()
        self.packets: list[tuple[int, bytes]] = []
        self.total_bytes = 0
        self.failure: BaseException | None = None
        self.thread = threading.Thread(
            target=self._run, name="qemu-rpi-tap-capture", daemon=True)

    def start(self) -> None:
        self.thread.start()

    def _run(self) -> None:
        try:
            while not self.stop_event.is_set():
                try:
                    packet = self.socket.recv(65535)
                except TimeoutError:
                    continue
                if (len(self.packets) >= CAPTURE_MAX_PACKETS or
                        self.total_bytes + len(packet) > CAPTURE_MAX_BYTES):
                    raise GateError("TAP packet capture exceeded its bound")
                self.packets.append((time.time_ns(), packet))
                self.total_bytes += len(packet)
        except Exception as error:
            if not self.stop_event.is_set():
                self.failure = error

    def stop(self) -> None:
        self.stop_event.set()
        self.thread.join(timeout=1)
        self.socket.close()
        self.thread.join(timeout=1)
        if self.thread.is_alive():
            raise GateError("TAP packet-capture thread did not stop")
        if self.failure is not None:
            raise GateError(f"TAP packet capture failed: {self.failure}")

    def write(self, path: Path) -> dict[str, Any]:
        if not self.packets:
            raise GateError("TAP packet capture contains no packets")
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_name(path.name + ".tmp")
        with temporary.open("wb") as stream:
            stream.write(struct.pack(
                "<IHHIIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 1))
            for timestamp_ns, packet in self.packets:
                seconds, remainder = divmod(timestamp_ns, 1_000_000_000)
                stream.write(struct.pack(
                    "<IIII", seconds, remainder // 1000,
                    len(packet), len(packet)))
                stream.write(packet)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        return {
            "path": str(path), "sha256": sha256_file(path),
            "packets": len(self.packets), "captured_bytes": self.total_bytes,
            "link_type": "ethernet", "timestamp_resolution": "microsecond",
        }


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def hash_tree(root: Path) -> dict[str, str]:
    if not root.is_dir():
        raise GateError(f"TFTP root is not a directory: {root}")
    result: dict[str, str] = {}
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root).as_posix()
        if path.is_symlink():
            raise GateError(f"TFTP tree contains a symbolic link: {relative}")
        if path.is_dir():
            continue
        if not path.is_file():
            raise GateError(
                f"TFTP tree contains a non-regular file: {relative}")
        result[relative] = sha256_file(path)
    if not result:
        raise GateError("TFTP root contains no files")
    return result


def qemu_opt_path(path: Path) -> str:
    return str(path).replace(",", ",,")


def load_manifest(path: Path, machine: str, memory: str,
                  expected_firmware: str, eeprom_hash: str,
                  tftp_files: dict[str, str]) -> tuple[dict[str, Any], str]:
    if not path.is_file() or path.is_symlink():
        raise GateError(f"manifest is not a regular non-symlink file: {path}")
    data = path.read_bytes()
    if len(data) > 1024 * 1024:
        raise GateError("manifest exceeds 1 MiB")
    manifest_hash = hashlib.sha256(data).hexdigest()
    try:
        manifest = json.loads(data)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise GateError(f"invalid manifest JSON: {error}") from error
    if not isinstance(manifest, dict):
        raise GateError("manifest root must be an object")
    expected_keys = {
        "schema", "machine", "memory", "expected_firmware", "eeprom",
        "eeprom_update", "tftp_files",
    }
    if set(manifest) != expected_keys:
        raise GateError("manifest has missing or unknown top-level keys")
    if manifest["schema"] != MANIFEST_SCHEMA:
        raise GateError(f"unsupported manifest schema: {manifest['schema']!r}")
    if manifest["machine"] != machine or manifest["memory"] != memory:
        raise GateError("manifest machine or memory does not match the command")
    if manifest["expected_firmware"] != expected_firmware:
        raise GateError("manifest expected firmware does not match the command")
    if manifest["eeprom"] != {"sha256": eeprom_hash}:
        raise GateError("EEPROM does not match the manifest")
    if manifest["tftp_files"] != tftp_files:
        raise GateError("TFTP tree does not match the manifest")
    return manifest, manifest_hash


def resolve_eeprom_update(manifest: dict[str, Any],
                          tftp_root: Path,
                          tftp_files: dict[str, str]) -> Path | None:
    update = manifest["eeprom_update"]
    if update is None:
        return None
    if manifest["machine"] != "raspi4b":
        raise GateError("network EEPROM self-update is only valid for raspi4b")
    if not isinstance(update, dict):
        raise GateError("manifest EEPROM update metadata is invalid")
    result = update.get("expected_result")
    expected_keys = {"path", "sha256", "size", "expected_result"}
    if result == "program-failure":
        expected_keys.add("fail_after")
    if set(update) != expected_keys:
        raise GateError("manifest EEPROM update metadata is invalid")
    if result not in EEPROM_UPDATE_RESULTS:
        raise GateError("manifest EEPROM update result is invalid")
    if result == "program-failure" and (
            not isinstance(update["fail_after"], int) or
            isinstance(update["fail_after"], bool) or
            not 0 <= update["fail_after"] < EEPROM_SIZE):
        raise GateError("manifest EEPROM fail-after is invalid")
    relative = update["path"]
    if (not isinstance(relative, str) or not relative or
            relative.startswith("/") or relative.split("/")[-1] !=
            "pieeprom.upd" or
            any(part in ("", ".", "..") for part in relative.split("/"))):
        raise GateError("manifest EEPROM update path is invalid")
    if update["size"] != EEPROM_SIZE:
        raise GateError("manifest EEPROM update size is not 512 KiB")
    digest = tftp_files.get(relative)
    if digest is None or digest != update["sha256"]:
        raise GateError("manifest EEPROM update hash is not in the TFTP tree")
    path = tftp_root / relative
    if path.is_symlink() or not path.is_file():
        raise GateError("manifest EEPROM update is absent or unsafe")
    if path.stat().st_size != EEPROM_SIZE or sha256_file(path) != digest:
        raise GateError("manifest EEPROM update bytes do not match metadata")
    signature = path.with_name("pieeprom.sig")
    try:
        signature_relative = signature.relative_to(tftp_root).as_posix()
    except ValueError as error:
        raise GateError(
            "manifest EEPROM signature escapes TFTP root") from error
    if signature_relative not in tftp_files:
        raise GateError("manifest EEPROM update signature is absent")
    return path


def eeprom_update_result(manifest: dict[str, Any]) -> str:
    update = manifest["eeprom_update"]
    return "none" if update is None else update["expected_result"]


def expected_eeprom_hash(before: str, update: Path | None, result: str,
                         fail_after: int | None = None) -> str:
    if update is None or result in {
            "none", "invalid-signature", "write-protected"}:
        return before
    if result == "success":
        return sha256_file(update)
    if result == "program-failure":
        assert fail_after is not None
        digest = hashlib.sha256()
        with update.open("rb") as stream:
            digest.update(stream.read(fail_after))
        digest.update(b"\xff" * (EEPROM_SIZE - fail_after))
        return digest.hexdigest()
    raise GateError(f"unsupported EEPROM update result: {result}")


def verify_eeprom_transition(before: str, after: str,
                             update: Path | None,
                             result: str = "success",
                             fail_after: int | None = None
                             ) -> dict[str, Any]:
    expected = expected_eeprom_hash(
        before, update, result, fail_after)
    if after != expected:
        if update is None or result in {
                "none", "invalid-signature", "write-protected"}:
            raise GateError("EEPROM input changed during the gate")
        raise GateError(
            "EEPROM does not exactly match the expected update boundary"
        )
    return {
        "before_sha256": before,
        "after_sha256": after,
        "expected_update": None if update is None else str(update),
        "expected_result": result,
        "fail_after": fail_after,
        "changed": before != after,
        "verified": True,
    }


def authorize_eeprom_update(update: Path | None, allow: bool,
                            eeprom_size: int) -> None:
    if (update is not None) != allow:
        raise GateError(
            "manifested EEPROM update and --allow-eeprom-update must "
            "be selected together"
        )
    if update is not None and eeprom_size != EEPROM_SIZE:
        raise GateError("writable EEPROM backend must be exactly 512 KiB")


def verify_eeprom_update_trace(text: str, result: str) -> dict[str, Any]:
    expected_by_result = {
        "success": (
            ("eeprom.self-update", "restart"),
            ("eeprom.self-update-reboot", "success"),
            ("eeprom.self-update", "up-to-date"),
            ("handoff.arm", "success"),
        ),
        "invalid-signature": (
            ("eeprom.self-update", "invalid"),
        ),
        "write-protected": (
            ("eeprom.self-update", "write-protected"),
        ),
        "program-failure": (
            ("eeprom.self-update", "failure"),
        ),
    }
    observed: list[dict[str, Any]] = []
    search_from = 0
    lines = text.splitlines()
    if result == "none":
        return {
            "required": False, "expected_result": result,
            "events": observed, "verified": True,
        }
    expected = expected_by_result[result]
    for event, outcome in expected:
        for index in range(search_from, len(lines)):
            line = lines[index]
            if f"event={event}" in line and f"outcome={outcome}" in line:
                observed.append({
                    "line": index + 1,
                    "event": event,
                    "outcome": outcome,
                })
                search_from = index + 1
                break
        else:
            raise GateError(
                f"QEMU trace lacks ordered {event}/{outcome} evidence")
    return {
        "required": True, "expected_result": result,
        "events": observed, "verified": True,
    }


def verify_eeprom_update_wire(
        events: list[network_cadence.Event],
        update: Path | None,
        tftp_root: Path,
        result: str = "success") -> dict[str, Any]:
    if update is None:
        return {"required": False, "rrq": [], "verified": True}
    update_name = update.relative_to(tftp_root).as_posix()
    signature_name = update.with_name(
        "pieeprom.sig").relative_to(tftp_root).as_posix()
    expected = [update_name, signature_name]
    if result == "success":
        expected += [update_name, signature_name]
    rrq = [
        event.detail for event in events
        if event.direction == "tx" and event.protocol == "tftp" and
        event.action == "rrq"
    ]
    positions: list[int] = []
    search_from = 0
    for filename in expected:
        try:
            index = rrq.index(filename, search_from)
        except ValueError as error:
            raise GateError(
                "packet capture lacks the ordered EEPROM "
                f"request sequence: {expected}") from error
        positions.append(index)
        search_from = index + 1
    if result == "success" and search_from >= len(rrq):
        raise GateError(
            "packet capture lacks a firmware request after EEPROM no-op")
    if result != "success" and search_from != len(rrq):
        raise GateError(
            "packet capture contains requests after expected EEPROM failure")
    evidence = {
        "required": True,
        "expected_result": result,
        "rrq": rrq,
        "update_positions": positions,
        "verified": True,
    }
    evidence["first_firmware_rrq"] = (
        rrq[search_from] if result == "success" else None)
    return evidence


def require_program(name: str) -> str:
    path = shutil.which(name)
    if not path:
        raise GateError(f"required host program is missing: {name}")
    return path


def terminate(process: subprocess.Popen[Any] | None) -> None:
    if process is None or process.poll() is not None:
        return
    process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def log_tail(path: Path, limit: int = 12000) -> str:
    if not path.exists():
        return ""
    data = path.read_bytes()
    return data[-limit:].decode("utf-8", "replace")


def wait_for_handoff(qmp: QMPClient, timeout: float) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    last_state: Any = None
    while time.monotonic() < deadline:
        last_state = qmp.qom_get("boot-state")
        if last_state == "arm-handoff-ready":
            properties = {}
            for name in (
                "boot-state", "boot-source", "boot-client-ip",
                "boot-tftp-ip", "boot-network-server-ip",
                "boot-dns-server-ip", "boot-tftp-hostname",
                "boot-dns-retransmit-count",
                "boot-self-update-status",
                "firmware-file", "firmware-sha256",
                "firmware-fixup-file", "firmware-fixup-sha256",
                "firmware-kernel-file", "firmware-kernel-sha256",
                "firmware-device-tree-file",
                "firmware-device-tree-sha256", "arm-handoff-status",
                "firmware-cmdline-file", "firmware-cmdline-sha256",
                "firmware-initramfs-file", "firmware-initramfs-sha256",
            ):
                properties[name] = qmp.qom_get(name)
            return properties
        time.sleep(0.05)
    raise GateError(
        f"QEMU did not reach ARM handoff; last state={last_state!r}")


def wait_for_update_failure(qmp: QMPClient, qemu_log: Path,
                            result: str, timeout: float) -> dict[str, Any]:
    status_by_result = {
        "invalid-signature": "invalid",
        "write-protected": "write-protected",
        "program-failure": "program-failed",
    }
    expected_status = status_by_result[result]
    expected_outcome = (
        "failure" if result == "program-failure" else expected_status)
    marker = (
        "event=eeprom.self-update source=network "
        f"outcome={expected_outcome}")
    deadline = time.monotonic() + timeout
    last_state: Any = None
    while time.monotonic() < deadline:
        last_state = qmp.qom_get("boot-state")
        text = (qemu_log.read_text(encoding="utf-8", errors="replace")
                if qemu_log.exists() else "")
        if marker in text:
            status = qmp.qom_get("boot-self-update-status")
            if status != expected_status:
                raise GateError(
                    "EEPROM failure status did not remain observable; "
                    "use a fail-stop EEPROM BOOT_ORDER policy")
            return {
                "boot-state": last_state,
                "boot-source": qmp.qom_get("boot-source"),
                "boot-self-update-status": status,
                "arm-handoff-status": qmp.qom_get("arm-handoff-status"),
            }
        time.sleep(0.05)
    raise GateError(
        f"QEMU did not reach {expected_status}; last state={last_state!r}")


def verify_handoff_artifacts(
        properties: dict[str, Any],
        tftp_files: dict[str, str],
        events: list[network_cadence.Event]) -> dict[str, Any]:
    artifacts = (
        ("firmware", True), ("firmware-fixup", True),
        ("firmware-kernel", True), ("firmware-device-tree", False),
        ("firmware-cmdline", False), ("firmware-initramfs", False),
    )
    rrq = [
        event.detail for event in events
        if event.direction == "tx" and event.protocol == "tftp" and
        event.action == "rrq"
    ]
    evidence: dict[str, Any] = {}
    for prefix, required in artifacts:
        filename = properties[f"{prefix}-file"]
        digest = properties[f"{prefix}-sha256"]
        if filename == "none" or digest == "none":
            if required:
                raise GateError(f"QEMU did not report required {prefix}")
            if digest != "none":
                raise GateError(f"QEMU reported a hash without {prefix}")
            evidence[prefix] = None
            continue
        suffix = "/" + filename
        candidates = []
        for wire_path in rrq:
            if (wire_path == filename or wire_path.endswith(suffix)) and \
                    tftp_files.get(wire_path) == digest and \
                    wire_path not in candidates:
                candidates.append(wire_path)
        if not candidates:
            raise GateError(
                f"QEMU {prefix} has no matching manifested wire request: "
                f"{filename}"
            )
        if len(candidates) != 1:
            raise GateError(
                f"QEMU {prefix} wire request is ambiguous: {candidates}"
            )
        evidence[prefix] = {
            "logical_path": filename,
            "wire_path": candidates[0],
            "sha256": digest,
        }
    return evidence


def run_inner(args: argparse.Namespace) -> dict[str, Any]:
    qemu = args.qemu.resolve(strict=True)
    eeprom = args.eeprom.resolve(strict=True)
    tftp_root = args.tftp_root.resolve(strict=True)
    manifest_path = args.manifest.resolve(strict=True)
    output = args.output.resolve()
    pcap_output = args.pcap_output.resolve() if args.pcap_output else None
    cadence_reference = (args.cadence_reference.resolve(strict=True)
                         if args.cadence_reference else None)
    cadence_output = (args.cadence_output.resolve()
                      if args.cadence_output else None)
    expected_firmware = tftp_root / args.expected_firmware
    if not qemu.is_file() or not os.access(qemu, os.X_OK):
        raise GateError(f"QEMU is not executable: {qemu}")
    if not eeprom.is_file() or eeprom.is_symlink():
        raise GateError(f"EEPROM is not a regular non-symlink file: {eeprom}")
    if not expected_firmware.is_file() or expected_firmware.is_symlink():
        raise GateError(
            f"expected firmware is absent or unsafe: {expected_firmware}"
        )
    try:
        output.relative_to(tftp_root)
    except ValueError:
        pass
    else:
        raise GateError("report output must be outside the TFTP root")
    try:
        manifest_path.relative_to(tftp_root)
    except ValueError:
        pass
    else:
        raise GateError("manifest must be outside the TFTP root")
    if pcap_output is not None:
        try:
            pcap_output.relative_to(tftp_root)
        except ValueError:
            pass
        else:
            raise GateError("PCAP output must be outside the TFTP root")
        if pcap_output in {
                output, eeprom, manifest_path, qemu, cadence_reference}:
            raise GateError("PCAP output conflicts with another gate path")
    if cadence_reference is not None:
        try:
            cadence_reference.relative_to(tftp_root)
        except ValueError:
            pass
        else:
            raise GateError("cadence reference must be outside the TFTP root")
    if cadence_output is not None:
        try:
            cadence_output.relative_to(tftp_root)
        except ValueError:
            pass
        else:
            raise GateError("cadence output must be outside the TFTP root")
        if cadence_output in {
                output, pcap_output, eeprom, manifest_path, qemu,
                cadence_reference}:
            raise GateError("cadence output conflicts with another gate path")

    ip = require_program("ip")
    dnsmasq_binary = require_program("dnsmasq")
    before_tree = hash_tree(tftp_root)
    before_eeprom = sha256_file(eeprom)
    expected_firmware_hash = sha256_file(expected_firmware)
    manifest, manifest_hash = load_manifest(
        manifest_path, args.machine, args.memory, args.expected_firmware,
        before_eeprom, before_tree,
    )
    eeprom_update = resolve_eeprom_update(
        manifest, tftp_root, before_tree)
    update_result = eeprom_update_result(manifest)
    update_fail_after = (
        manifest["eeprom_update"].get("fail_after")
        if manifest["eeprom_update"] is not None else None)
    authorize_eeprom_update(
        eeprom_update, args.allow_eeprom_update, eeprom.stat().st_size)

    with tempfile.TemporaryDirectory(prefix="qemu-rpi-netboot-") as temp_name:
        temp = Path(temp_name)
        qmp_path = temp / "qmp.sock"
        lease_path = temp / "dnsmasq.leases"
        dnsmasq_log = temp / "dnsmasq.log"
        qemu_log = temp / "qemu.log"
        tap = "rpi-gate0"
        subprocess.run(
            [ip, "tuntap", "add", "dev", tap, "mode", "tap", "user", "0"],
            check=True,
        )
        subprocess.run(
            [ip, "addr", "add", f"{args.server_ip}/24", "dev", tap],
            check=True,
        )
        subprocess.run([ip, "link", "set", tap, "up"], check=True)

        dnsmasq_command = [
            dnsmasq_binary, "--no-daemon", "--user=root", "--port=0",
            f"--interface={tap}", "--bind-interfaces", "--dhcp-authoritative",
            f"--dhcp-range={args.client_start},{args.client_end},"
            "255.255.255.0,1h",
            f"--dhcp-option=option:router,{args.server_ip}",
            f"--dhcp-option=option:tftp-server,{args.server_ip}",
            f"--dhcp-boot=unused,,{args.server_ip}", "--enable-tftp",
            f"--tftp-root={tftp_root}", f"--dhcp-leasefile={lease_path}",
            "--log-dhcp",
        ]
        machine_options = (
            f"{args.machine},boot-mode=behavioral,eeprom-drive=pieeprom")
        if update_result == "write-protected":
            machine_options += ",eeprom-write-protect=on"
        if update_result == "program-failure":
            machine_options += (
                ",eeprom-fail-stage=program,"
                f"eeprom-fail-after={update_fail_after}")
        qemu_command = [
            str(qemu), "-M",
            machine_options,
            "-m", args.memory,
            "-drive",
            "if=none,id=pieeprom,format=raw,"
            f"{'snapshot=on,' if eeprom_update is None else ''}file="
            f"{qemu_opt_path(eeprom)}",
            "-netdev", f"tap,id=net0,ifname={tap},script=no,downscript=no",
            "-global", "bcm2711-genet.netdev=net0",
            "-global", f"bcm2711-genet.mac={args.mac}",
            "-display", "none", "-serial", "none", "-monitor", "none",
            "-qmp", f"unix:{qmp_path},server=on,wait=off",
        ]
        if update_result != "success":
            qemu_command.append("-no-reboot")
        qemu_command.extend(["-trace", "enable=raspi4b_boot_event"])
        drop_directions = {"none": 0, "tx": 1, "rx": 2, "both": 3}
        if args.packet_drop_direction != "none":
            qemu_command.extend([
                "-global", "bcm2711-genet.packet-drop-direction="
                f"{drop_directions[args.packet_drop_direction]}",
                "-global", "bcm2711-genet.packet-drop-after="
                f"{args.packet_drop_after}",
                "-global", "bcm2711-genet.packet-drop-count="
                f"{args.packet_drop_count}",
            ])
        dnsmasq: subprocess.Popen[Any] | None = None
        qemu_process: subprocess.Popen[Any] | None = None
        capture = TapCapture(tap)
        capture.start()
        try:
            with dnsmasq_log.open("wb") as dnsmasq_stream:
                dnsmasq = subprocess.Popen(
                    dnsmasq_command, stdout=dnsmasq_stream,
                    stderr=subprocess.STDOUT,
                )
            time.sleep(0.2)
            if dnsmasq.poll() is not None:
                raise GateError(
                    "dnsmasq exited before QEMU started:\n" +
                    log_tail(dnsmasq_log)
                )
            with qemu_log.open("wb") as qemu_stream:
                qemu_process = subprocess.Popen(
                    qemu_command, stdout=qemu_stream,
                    stderr=subprocess.STDOUT,
                )
            try:
                with QMPClient(qmp_path, qemu_process, args.timeout) as qmp:
                    if update_result in {"none", "success"}:
                        properties = wait_for_handoff(qmp, args.timeout)
                    else:
                        properties = wait_for_update_failure(
                            qmp, qemu_log, update_result, args.timeout)
                    genet_path = "/machine/soc/peripherals/genet"
                    packet_fault = {
                        "direction": args.packet_drop_direction,
                        "after": args.packet_drop_after,
                        "count": args.packet_drop_count,
                        "packets_seen": qmp.command(
                            "qom-get", {"path": genet_path, "property":
                                        "packet-drop-packets-seen"}),
                        "drops_injected": qmp.command(
                            "qom-get", {"path": genet_path, "property":
                                        "packet-drops-injected"}),
                    }
            except ProvisionError as error:
                raise GateError(str(error)) from error
            if update_result in {"none", "success"}:
                if properties["boot-source"] != "network":
                    raise GateError(
                        "unexpected boot source: "
                        f"{properties['boot-source']!r}"
                    )
                if properties["arm-handoff-status"] != "ready":
                    raise GateError("ARM handoff status is not ready")
                if (update_result == "success" and
                        properties["boot-self-update-status"] !=
                        "up-to-date"):
                    raise GateError(
                        "unexpected EEPROM self-update status at handoff: "
                        f"{properties['boot-self-update-status']!r}")
                if properties["firmware-sha256"] != expected_firmware_hash:
                    raise GateError(
                        "QEMU firmware hash does not match the unchanged "
                        "TFTP file"
                    )
        except Exception as error:
            raise GateError(
                f"{error}\n\ndnsmasq tail:\n{log_tail(dnsmasq_log)}"
                f"\nQEMU tail:\n{log_tail(qemu_log)}"
            ) from error
        finally:
            terminate(qemu_process)
            terminate(dnsmasq)
            capture.stop()
        normalized_events = network_cadence.normalize(
            capture.packets, network_cadence.parse_mac(args.mac))
        artifact_evidence = (
            verify_handoff_artifacts(
                properties, before_tree, normalized_events)
            if update_result in {"none", "success"} else {})
        trace_evidence = verify_eeprom_update_trace(
            qemu_log.read_text(encoding="utf-8", errors="replace"),
            update_result)
        wire_evidence = verify_eeprom_update_wire(
            normalized_events,
            eeprom_update, tftp_root, update_result)

    capture_report = (
        capture.write(pcap_output) if pcap_output is not None else None)
    cadence_report = None
    if cadence_reference is not None:
        assert pcap_output is not None
        assert cadence_output is not None
        cadence_report = network_cadence.run(
            cadence_reference, pcap_output,
            args.cadence_reference_client_mac, args.mac,
            args.cadence_absolute_tolerance_ms,
            args.cadence_relative_tolerance, cadence_output)
        if not cadence_report["match"]:
            raise GateError(
                f"network cadence differs; see {cadence_output}")

    after_tree = hash_tree(tftp_root)
    after_eeprom = sha256_file(eeprom)
    after_manifest = sha256_file(manifest_path)
    if after_tree != before_tree:
        raise GateError("TFTP input tree changed during the gate")
    if after_manifest != manifest_hash:
        raise GateError("manifest changed during the gate")
    eeprom_transition = verify_eeprom_transition(
        before_eeprom, after_eeprom, eeprom_update, update_result,
        update_fail_after)
    report = {
        "schema": "qemu-rpi-network-boot-gate-v2",
        "machine": args.machine,
        "memory": args.memory,
        "eeprom": {
            "path": str(eeprom),
            "sha256": after_eeprom,
            "transition": eeprom_transition,
        },
        "manifest": {
            "path": str(manifest_path), "sha256": manifest_hash,
            "schema": manifest["schema"],
        },
        "tftp_root": str(tftp_root),
        "tftp_files": before_tree,
        "qmp": properties,
        "server": {
            "implementation": "dnsmasq",
            "address": args.server_ip,
            "client_range": [args.client_start, args.client_end],
        },
        "packet_fault": packet_fault,
        "artifact_wire_paths": artifact_evidence,
        "eeprom_update_trace": trace_evidence,
        "eeprom_update_wire": wire_evidence,
        "capture": capture_report,
        "cadence": cadence_report,
        "inputs_unchanged": not eeprom_transition["changed"],
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary_output = output.with_name(output.name + ".tmp")
    temporary_output.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    os.replace(temporary_output, output)
    return report


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", required=True, type=Path)
    parser.add_argument("--eeprom", required=True, type=Path)
    parser.add_argument("--tftp-root", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--pcap-output", type=Path)
    parser.add_argument("--cadence-reference", type=Path)
    parser.add_argument("--cadence-reference-client-mac")
    parser.add_argument("--cadence-output", type=Path)
    parser.add_argument("--cadence-absolute-tolerance-ms",
                        type=float, default=25.0)
    parser.add_argument("--cadence-relative-tolerance",
                        type=float, default=0.10)
    parser.add_argument("--expected-firmware", default="start4.elf")
    parser.add_argument(
        "--allow-eeprom-update", action="store_true",
        help="allow only the exact EEPROM transition sealed by the manifest",
    )
    parser.add_argument("--machine", choices=("raspi4b", "raspi-cm4"),
                        default="raspi4b")
    parser.add_argument("--memory", choices=("1G", "2G", "4G", "8G"),
                        default="2G")
    parser.add_argument("--server-ip", default="10.42.0.1")
    parser.add_argument("--client-start", default="10.42.0.10")
    parser.add_argument("--client-end", default="10.42.0.50")
    parser.add_argument("--mac", default="52:54:00:12:34:56")
    parser.add_argument("--packet-drop-direction",
                        choices=("none", "tx", "rx", "both"),
                        default="none")
    parser.add_argument("--packet-drop-after", type=int, default=0)
    parser.add_argument("--packet-drop-count", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--inner", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    if args.packet_drop_after < 0:
        parser.error("--packet-drop-after must be nonnegative")
    if args.packet_drop_count <= 0:
        parser.error("--packet-drop-count must be positive")
    cadence_options = (
        args.cadence_reference, args.cadence_reference_client_mac,
        args.cadence_output,
    )
    if any(value is not None for value in cadence_options) and (
            not all(value is not None for value in cadence_options) or
            args.pcap_output is None):
        parser.error(
            "cadence comparison requires --pcap-output, "
            "--cadence-reference, --cadence-reference-client-mac, and "
            "--cadence-output")
    if (not math.isfinite(args.cadence_absolute_tolerance_ms) or
            args.cadence_absolute_tolerance_ms < 0):
        parser.error("--cadence-absolute-tolerance-ms must be nonnegative")
    if (not math.isfinite(args.cadence_relative_tolerance) or
            args.cadence_relative_tolerance < 0):
        parser.error("--cadence-relative-tolerance must be nonnegative")
    return args


def main(argv: list[str] | None = None) -> int:
    arguments = list(sys.argv[1:] if argv is None else argv)
    args = parse_args(arguments)
    try:
        if not args.inner:
            unshare = require_program("unshare")
            command = [
                unshare, "--user", "--map-root-user", "--net",
                sys.executable, str(Path(__file__).resolve()),
                *arguments, "--inner",
            ]
            return subprocess.run(command, check=False).returncode
        report = run_inner(args)
        print(json.dumps(report, sort_keys=True))
        return 0
    except (GateError, OSError, subprocess.SubprocessError) as error:
        print(f"network boot gate: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
