#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

import argparse
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("network_boot_gate.py")
SPEC = importlib.util.spec_from_file_location("network_boot_gate", SCRIPT)
assert SPEC and SPEC.loader
network_boot_gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(network_boot_gate)

CADENCE_SCRIPT = Path(__file__).with_name("network_cadence.py")
CADENCE_SPEC = importlib.util.spec_from_file_location(
    "network_cadence_for_gate", CADENCE_SCRIPT)
assert CADENCE_SPEC and CADENCE_SPEC.loader
network_cadence = importlib.util.module_from_spec(CADENCE_SPEC)
sys.modules[CADENCE_SPEC.name] = network_cadence
CADENCE_SPEC.loader.exec_module(network_cadence)


class NetworkBootGateTests(unittest.TestCase):
    def test_hash_tree_is_stable_and_recursive(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "overlays").mkdir()
            (root / "start4.elf").write_bytes(b"firmware")
            (root / "overlays" / "one.dtbo").write_bytes(b"overlay")
            first = network_boot_gate.hash_tree(root)
            second = network_boot_gate.hash_tree(root)
            self.assertEqual(first, second)
            self.assertEqual(set(first), {"start4.elf", "overlays/one.dtbo"})

    def test_hash_tree_rejects_symlink(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / "target"
            target.write_bytes(b"data")
            (root / "link").symlink_to(target)
            with self.assertRaises(network_boot_gate.GateError):
                network_boot_gate.hash_tree(root)

    def test_qemu_option_path_escapes_comma(self):
        self.assertEqual(
            network_boot_gate.qemu_opt_path(Path("a,b.bin")), "a,,b.bin"
        )

    def test_capture_writes_atomic_classic_pcap(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "capture.pcap"
            capture = object.__new__(network_boot_gate.TapCapture)
            capture.packets = [
                (1_000_000_000, b"first"),
                (1_001_000_000, b"second"),
            ]
            capture.total_bytes = 11
            report = capture.write(output)
            packets, digest = network_cadence.read_pcap(output)
            self.assertEqual(packets, [
                (1_000_000_000, b"first"),
                (1_001_000_000, b"second"),
            ])
            self.assertEqual(report["sha256"], digest)
            self.assertFalse(output.with_name("capture.pcap.tmp").exists())

    def test_timeout_must_be_positive(self):
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                network_boot_gate.parse_args([
                    "--qemu", "qemu", "--eeprom", "eeprom",
                    "--tftp-root", "tftp", "--manifest", "manifest.json",
                    "--output", "report",
                    "--timeout", "0",
                ])

    def test_packet_fault_bounds(self):
        base = [
            "--qemu", "qemu", "--eeprom", "eeprom",
            "--tftp-root", "tftp", "--manifest", "manifest.json",
            "--output", "report",
        ]
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                network_boot_gate.parse_args([
                    *base, "--packet-drop-after", "-1",
                ])
            with self.assertRaises(SystemExit):
                network_boot_gate.parse_args([
                    *base, "--packet-drop-count", "0",
                ])
            with self.assertRaises(SystemExit):
                network_boot_gate.parse_args([
                    *base, "--cadence-reference", "physical.pcap",
                ])
        args = network_boot_gate.parse_args([
            *base, "--packet-drop-direction", "both",
            "--packet-drop-after", "7", "--packet-drop-count", "3",
        ])
        self.assertEqual(
            (args.packet_drop_direction, args.packet_drop_after,
             args.packet_drop_count), ("both", 7, 3))
        args = network_boot_gate.parse_args([
            *base, "--pcap-output", "qemu.pcap",
            "--cadence-reference", "physical.pcap",
            "--cadence-reference-client-mac", "dc:a6:32:01:36:c2",
            "--cadence-output", "cadence.json",
        ])
        self.assertEqual(args.cadence_output, Path("cadence.json"))

    def test_manifest_pins_all_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            eeprom = root / "pieeprom.bin"
            tftp = root / "tftp"
            manifest_path = root / "manifest.json"
            tftp.mkdir()
            eeprom.write_bytes(b"eeprom")
            (tftp / "start4.elf").write_bytes(b"firmware")
            eeprom_hash = network_boot_gate.sha256_file(eeprom)
            tree = network_boot_gate.hash_tree(tftp)
            manifest = {
                "schema": network_boot_gate.MANIFEST_SCHEMA,
                "machine": "raspi4b", "memory": "2G",
                "expected_firmware": "start4.elf",
                "eeprom": {"sha256": eeprom_hash},
                "eeprom_update": None,
                "tftp_files": tree,
            }
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            loaded, _ = network_boot_gate.load_manifest(
                manifest_path, "raspi4b", "2G", "start4.elf",
                eeprom_hash, tree,
            )
            self.assertEqual(loaded, manifest)
            with self.assertRaisesRegex(
                    network_boot_gate.GateError, "EEPROM"):
                network_boot_gate.load_manifest(
                    manifest_path, "raspi4b", "2G", "start4.elf",
                    "0" * 64, tree,
                )

    def test_manifested_eeprom_transition_is_exact_and_bounded(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tftp = root / "tftp"
            tftp.mkdir()
            update = tftp / "pieeprom.upd"
            signature = tftp / "pieeprom.sig"
            update.write_bytes(b"\xa5" * network_boot_gate.EEPROM_SIZE)
            signature.write_bytes(b"signature")
            files = network_boot_gate.hash_tree(tftp)
            manifest = {
                "machine": "raspi4b",
                "eeprom_update": {
                    "path": "pieeprom.upd",
                    "sha256": files["pieeprom.upd"],
                    "size": network_boot_gate.EEPROM_SIZE,
                    "expected_result": "success",
                },
            }
            resolved = network_boot_gate.resolve_eeprom_update(
                manifest, tftp, files)
            self.assertEqual(resolved, update)
            transition = network_boot_gate.verify_eeprom_transition(
                "0" * 64, files["pieeprom.upd"], resolved)
            self.assertTrue(transition["changed"])
            self.assertTrue(transition["verified"])
            with self.assertRaisesRegex(
                    network_boot_gate.GateError, "exactly match"):
                network_boot_gate.verify_eeprom_transition(
                    "0" * 64, "f" * 64, resolved)
            network_boot_gate.authorize_eeprom_update(
                resolved, True, network_boot_gate.EEPROM_SIZE)
            with self.assertRaisesRegex(
                    network_boot_gate.GateError, "selected together"):
                network_boot_gate.authorize_eeprom_update(
                    resolved, False, network_boot_gate.EEPROM_SIZE)
            with self.assertRaisesRegex(
                    network_boot_gate.GateError, "512 KiB"):
                network_boot_gate.authorize_eeprom_update(
                    resolved, True, network_boot_gate.EEPROM_SIZE - 1)

            manifest["machine"] = "raspi-cm4"
            with self.assertRaisesRegex(
                    network_boot_gate.GateError, "only valid"):
                network_boot_gate.resolve_eeprom_update(
                    manifest, tftp, files)

    def test_manifested_eeprom_update_requires_signature(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            update = root / "pieeprom.upd"
            update.write_bytes(b"\xa5" * network_boot_gate.EEPROM_SIZE)
            files = network_boot_gate.hash_tree(root)
            manifest = {
                "machine": "raspi4b",
                "eeprom_update": {
                    "path": "pieeprom.upd",
                    "sha256": files["pieeprom.upd"],
                    "size": network_boot_gate.EEPROM_SIZE,
                    "expected_result": "success",
                },
            }
            with self.assertRaisesRegex(
                    network_boot_gate.GateError, "signature"):
                network_boot_gate.resolve_eeprom_update(
                    manifest, root, files)

    def test_eeprom_update_trace_and_wire_require_two_boots(self):
        trace = "\n".join([
            "event=eeprom.self-update outcome=restart",
            "event=eeprom.self-update-reboot outcome=success",
            "event=eeprom.self-update outcome=up-to-date",
            "event=handoff.arm64 outcome=success",
        ])
        evidence = network_boot_gate.verify_eeprom_update_trace(
            trace, "success")
        self.assertTrue(evidence["verified"])
        self.assertEqual(len(evidence["events"]), 4)
        with self.assertRaisesRegex(
                network_boot_gate.GateError, "ordered"):
            network_boot_gate.verify_eeprom_update_trace(
                "\n".join(trace.splitlines()[::2]), "success")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            update = root / "pieeprom.upd"
            update.write_bytes(b"\xff" * network_boot_gate.EEPROM_SIZE)
            events = [
                network_cadence.Event(
                    index, "tx", "tftp", "rrq", filename)
                for index, filename in enumerate([
                    "pieeprom.upd", "pieeprom.sig",
                    "pieeprom.upd", "pieeprom.sig", "config.txt",
                ])
            ]
            wire = network_boot_gate.verify_eeprom_update_wire(
                events, update, root)
            self.assertEqual(wire["update_positions"], [0, 1, 2, 3])
            self.assertEqual(wire["first_firmware_rrq"], "config.txt")
            with self.assertRaisesRegex(
                    network_boot_gate.GateError, "ordered EEPROM"):
                network_boot_gate.verify_eeprom_update_wire(
                    events[:2], update, root)

    def test_expected_update_failures_prove_wire_and_non_mutation(self):
        before = "1" * 64
        for result, outcome in (
                ("invalid-signature", "invalid"),
                ("write-protected", "write-protected")):
            with self.subTest(result=result):
                trace = (
                    "event=eeprom.self-update source=network "
                    f"outcome={outcome}"
                )
                evidence = network_boot_gate.verify_eeprom_update_trace(
                    trace, result)
                self.assertEqual(evidence["expected_result"], result)
                transition = network_boot_gate.verify_eeprom_transition(
                    before, before, Path("pieeprom.upd"), result)
                self.assertFalse(transition["changed"])
                with self.assertRaisesRegex(
                        network_boot_gate.GateError, "changed"):
                    network_boot_gate.verify_eeprom_transition(
                        before, "2" * 64, Path("pieeprom.upd"), result)

                with tempfile.TemporaryDirectory() as directory:
                    root = Path(directory)
                    update = root / "pieeprom.upd"
                    update.write_bytes(
                        b"\xff" * network_boot_gate.EEPROM_SIZE)
                    events = [
                        network_cadence.Event(
                            index, "tx", "tftp", "rrq", filename)
                        for index, filename in enumerate([
                            "pieeprom.upd", "pieeprom.sig",
                        ])
                    ]
                    wire = network_boot_gate.verify_eeprom_update_wire(
                        events, update, root, result)
                    self.assertEqual(wire["update_positions"], [0, 1])
                    self.assertIsNone(wire["first_firmware_rrq"])
                    with self.assertRaisesRegex(
                            network_boot_gate.GateError, "requests after"):
                        network_boot_gate.verify_eeprom_update_wire(
                            events + [network_cadence.Event(
                                2, "tx", "tftp", "rrq", "config.txt")],
                            update, root, result)

    def test_program_failure_transition_has_exact_durable_boundary(self):
        with tempfile.TemporaryDirectory() as directory:
            update = Path(directory) / "pieeprom.upd"
            update.write_bytes(
                bytes(range(256)) *
                (network_boot_gate.EEPROM_SIZE // 256))
            expected = hashlib.sha256(
                update.read_bytes()[:512] +
                b"\xff" * (network_boot_gate.EEPROM_SIZE - 512)
            ).hexdigest()
            transition = network_boot_gate.verify_eeprom_transition(
                "0" * 64, expected, update, "program-failure", 512)
            self.assertTrue(transition["changed"])
            self.assertEqual(transition["fail_after"], 512)
            with self.assertRaisesRegex(
                    network_boot_gate.GateError, "expected update boundary"):
                network_boot_gate.verify_eeprom_transition(
                    "0" * 64, network_boot_gate.sha256_file(update),
                    update, "program-failure", 512)

    def test_handoff_artifacts_must_match_manifest(self):
        names = {
            "firmware": "start4.elf",
            "firmware-fixup": "fixup4.dat",
            "firmware-kernel": "kernel8.img",
            "firmware-device-tree": "board.dtb",
            "firmware-cmdline": "cmdline.txt",
            "firmware-initramfs": "initramfs8",
        }
        properties = {}
        files = {}
        events = []
        for index, (prefix, filename) in enumerate(names.items()):
            digest = f"{index + 1:064x}"
            properties[f"{prefix}-file"] = filename
            properties[f"{prefix}-sha256"] = digest
            wire_path = f"device-prefix/{filename}"
            files[wire_path] = digest
            events.append(network_cadence.Event(
                index, "tx", "tftp", "rrq", wire_path))
        evidence = network_boot_gate.verify_handoff_artifacts(
            properties, files, events)
        self.assertEqual(
            evidence["firmware"]["wire_path"],
            "device-prefix/start4.elf")
        properties["firmware-initramfs-sha256"] = "f" * 64
        with self.assertRaisesRegex(
                network_boot_gate.GateError, "initramfs"):
            network_boot_gate.verify_handoff_artifacts(
                properties, files, events)

        properties["firmware-initramfs-file"] = "none"
        properties["firmware-initramfs-sha256"] = "none"
        files["other/start4.elf"] = files["device-prefix/start4.elf"]
        events.append(network_cadence.Event(
            99, "tx", "tftp", "rrq", "other/start4.elf"))
        with self.assertRaisesRegex(
                network_boot_gate.GateError, "ambiguous"):
            network_boot_gate.verify_handoff_artifacts(
                properties, files, events)

    @unittest.skipUnless(
        os.environ.get("QEMU_RPI_GATE_QEMU") and
        os.environ.get("QEMU_RPI_GATE_TFTP_ROOT"),
        "set QEMU_RPI_GATE_QEMU and QEMU_RPI_GATE_TFTP_ROOT",
    )
    def test_real_dnsmasq_tap_tftp_gate(self):
        qemu = Path(os.environ["QEMU_RPI_GATE_QEMU"]).resolve()
        tftp_root = Path(os.environ["QEMU_RPI_GATE_TFTP_ROOT"]).resolve()
        config = b"BOOT_ORDER=0xe2\nDHCP_TIMEOUT=15000\n"
        image = bytearray(b"\xff" * (512 * 1024))
        struct.pack_into(">II", image, 0, 0x55AAF11F, 16 + len(config))
        image[8:20] = b"bootconf.txt"
        image[20:24] = b"\0" * 4
        image[24:24 + len(config)] = config

        with tempfile.TemporaryDirectory(prefix="qemu-rpi-gate-test-") as name:
            temp = Path(name)
            eeprom = temp / "pieeprom.bin"
            manifest_path = temp / "manifest.json"
            report_path = temp / "report.json"
            pcap_path = temp / "qemu.pcap"
            eeprom.write_bytes(image)
            before = network_boot_gate.sha256_file(eeprom)
            manifest_path.write_text(json.dumps({
                "schema": network_boot_gate.MANIFEST_SCHEMA,
                "machine": "raspi4b", "memory": "2G",
                "expected_firmware": "start4.elf",
                "eeprom": {"sha256": before},
                "eeprom_update": None,
                "tftp_files": network_boot_gate.hash_tree(tftp_root),
            }), encoding="utf-8")
            command = [
                sys.executable, str(SCRIPT), "--qemu", str(qemu),
                "--eeprom", str(eeprom), "--tftp-root", str(tftp_root),
                "--manifest", str(manifest_path),
                "--output", str(report_path), "--timeout", "15",
                "--pcap-output", str(pcap_path),
                "--packet-drop-direction", "tx",
                "--packet-drop-after", "0", "--packet-drop-count", "1",
            ]
            result = subprocess.run(
                command, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False,
            )
            self.assertEqual(result.returncode, 0, result.stdout)
            report = json.loads(report_path.read_text(encoding="utf-8"))
            self.assertTrue(report["inputs_unchanged"])
            self.assertEqual(report["qmp"]["boot-source"], "network")
            self.assertEqual(report["qmp"]["arm-handoff-status"], "ready")
            self.assertEqual(report["packet_fault"]["direction"], "tx")
            self.assertEqual(report["packet_fault"]["drops_injected"], 1)
            packets, digest = network_cadence.read_pcap(pcap_path)
            events = network_cadence.normalize(
                packets, network_cadence.parse_mac("52:54:00:12:34:56"))
            self.assertGreater(len(events), 5)
            self.assertEqual(report["capture"]["sha256"], digest)
            self.assertEqual(network_boot_gate.sha256_file(eeprom), before)

    @unittest.skipUnless(
        os.environ.get("QEMU_RPI_GATE_QEMU") and
        os.environ.get("QEMU_RPI_GATE_TFTP_ROOT"),
        "set QEMU_RPI_GATE_QEMU and QEMU_RPI_GATE_TFTP_ROOT",
    )
    def test_real_dnsmasq_tap_tftp_self_update_gate(self):
        qemu = Path(os.environ["QEMU_RPI_GATE_QEMU"]).resolve()
        source_tftp = Path(
            os.environ["QEMU_RPI_GATE_TFTP_ROOT"]).resolve()

        def make_eeprom(bootvar: int) -> bytes:
            config = (
                "BOOT_ORDER=0xe2\nENABLE_SELF_UPDATE=1\n"
                "TFTP_PREFIX=2\n"
                f"BOOTVAR0=0x{bootvar:x}\n"
            ).encode("ascii")
            image = bytearray(b"\xff" * network_boot_gate.EEPROM_SIZE)
            struct.pack_into(">II", image, 0, 0x55AAF11F,
                             16 + len(config))
            image[8:20] = b"bootconf.txt"
            image[20:24] = b"\0" * 4
            image[24:24 + len(config)] = config
            return bytes(image)

        with tempfile.TemporaryDirectory(
                prefix="qemu-rpi-update-gate-test-") as name:
            temp = Path(name)
            tftp = temp / "tftp"
            shutil.copytree(source_tftp, tftp, symlinks=True)
            prefix_name = "52-54-00-12-34-56"
            prefix = tftp / prefix_name
            shutil.copytree(source_tftp, prefix, symlinks=True)
            eeprom = temp / "pieeprom.bin"
            manifest_path = temp / "manifest.json"
            report_path = temp / "report.json"
            initial = make_eeprom(0x1111)
            update = make_eeprom(0x2222)
            eeprom.write_bytes(initial)
            (prefix / "pieeprom.upd").write_bytes(update)
            update_hash = network_boot_gate.sha256_file(
                prefix / "pieeprom.upd")
            (prefix / "pieeprom.sig").write_text(
                update_hash + "\n", encoding="ascii")
            manifest_path.write_text(json.dumps({
                "schema": network_boot_gate.MANIFEST_SCHEMA,
                "machine": "raspi4b", "memory": "2G",
                "expected_firmware": "start4.elf",
                "eeprom": {
                    "sha256": network_boot_gate.sha256_file(eeprom),
                },
                "eeprom_update": {
                    "path": f"{prefix_name}/pieeprom.upd",
                    "sha256": update_hash,
                    "size": network_boot_gate.EEPROM_SIZE,
                    "expected_result": "success",
                },
                "tftp_files": network_boot_gate.hash_tree(tftp),
            }), encoding="utf-8")
            result = subprocess.run([
                sys.executable, str(SCRIPT), "--qemu", str(qemu),
                "--eeprom", str(eeprom), "--tftp-root", str(tftp),
                "--manifest", str(manifest_path),
                "--allow-eeprom-update", "--output", str(report_path),
                "--timeout", "30",
            ], capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(
                report_path.read_text(encoding="utf-8"))
            self.assertFalse(report["inputs_unchanged"])
            self.assertTrue(report["eeprom"]["transition"]["changed"])
            self.assertTrue(report["eeprom"]["transition"]["verified"])
            self.assertEqual(report["eeprom"]["sha256"], update_hash)
            self.assertEqual(
                report["qmp"]["boot-self-update-status"], "up-to-date")
            self.assertTrue(report["eeprom_update_trace"]["verified"])
            self.assertEqual(
                len(report["eeprom_update_trace"]["events"]), 4)
            self.assertTrue(report["eeprom_update_wire"]["verified"])
            self.assertEqual(
                report["eeprom_update_wire"]["update_positions"],
                [0, 1, 2, 3])
            self.assertEqual(
                report["eeprom_update_wire"]["first_firmware_rrq"],
                f"{prefix_name}/config.txt")
            self.assertEqual(
                report["artifact_wire_paths"]["firmware"]["wire_path"],
                f"{prefix_name}/start4.elf")
            self.assertEqual(
                network_boot_gate.sha256_file(eeprom), update_hash)

    @unittest.skipUnless(
        os.environ.get("QEMU_RPI_GATE_QEMU") and
        os.environ.get("QEMU_RPI_GATE_TFTP_ROOT"),
        "set QEMU_RPI_GATE_QEMU and QEMU_RPI_GATE_TFTP_ROOT",
    )
    def test_real_tftp_expected_update_failures_do_not_mutate_eeprom(self):
        qemu = Path(os.environ["QEMU_RPI_GATE_QEMU"]).resolve()
        source_tftp = Path(
            os.environ["QEMU_RPI_GATE_TFTP_ROOT"]).resolve()

        def make_eeprom(bootvar: int) -> bytes:
            config = (
                "BOOT_ORDER=0xe2\nENABLE_SELF_UPDATE=1\n"
                "NET_BOOT_MAX_RETRIES=0\n"
                f"BOOTVAR0=0x{bootvar:x}\n"
            ).encode("ascii")
            image = bytearray(b"\xff" * network_boot_gate.EEPROM_SIZE)
            struct.pack_into(">II", image, 0, 0x55AAF11F,
                             16 + len(config))
            image[8:20] = b"bootconf.txt"
            image[20:24] = b"\0" * 4
            image[24:24 + len(config)] = config
            image[8192:12288] = bytes([bootvar & 0xff]) * 4096
            return bytes(image)

        for expected_result in (
                "invalid-signature", "write-protected", "program-failure"):
            with self.subTest(expected_result=expected_result), \
                    tempfile.TemporaryDirectory(
                        prefix="qemu-rpi-update-failure-gate-") as name:
                temp = Path(name)
                tftp = temp / "tftp"
                shutil.copytree(source_tftp, tftp, symlinks=True)
                eeprom = temp / "pieeprom.bin"
                manifest_path = temp / "manifest.json"
                report_path = temp / "report.json"
                initial = make_eeprom(0x1111)
                eeprom.write_bytes(initial)
                update = make_eeprom(0x2222)
                (tftp / "pieeprom.upd").write_bytes(update)
                update_hash = network_boot_gate.sha256_file(
                    tftp / "pieeprom.upd")
                signature = (
                    "0" * 64 if expected_result == "invalid-signature"
                    else update_hash)
                (tftp / "pieeprom.sig").write_text(
                    signature + "\n", encoding="ascii")
                manifest_path.write_text(json.dumps({
                    "schema": network_boot_gate.MANIFEST_SCHEMA,
                    "machine": "raspi4b", "memory": "2G",
                    "expected_firmware": "start4.elf",
                    "eeprom": {
                        "sha256": network_boot_gate.sha256_file(eeprom),
                    },
                    "eeprom_update": {
                        "path": "pieeprom.upd",
                        "sha256": update_hash,
                        "size": network_boot_gate.EEPROM_SIZE,
                        "expected_result": expected_result,
                        **({"fail_after": 512}
                           if expected_result == "program-failure" else {}),
                    },
                    "tftp_files": network_boot_gate.hash_tree(tftp),
                }), encoding="utf-8")
                result = subprocess.run([
                    sys.executable, str(SCRIPT), "--qemu", str(qemu),
                    "--eeprom", str(eeprom), "--tftp-root", str(tftp),
                    "--manifest", str(manifest_path),
                    "--allow-eeprom-update", "--output", str(report_path),
                    "--timeout", "30",
                ], capture_output=True, text=True, check=False)
                self.assertEqual(result.returncode, 0, result.stderr)
                report = json.loads(
                    report_path.read_text(encoding="utf-8"))
                if expected_result == "program-failure":
                    expected = (
                        update[:512] +
                        b"\xff" * (network_boot_gate.EEPROM_SIZE - 512))
                    self.assertNotEqual(expected, update)
                    self.assertFalse(report["inputs_unchanged"])
                    self.assertTrue(
                        report["eeprom"]["transition"]["changed"])
                    self.assertEqual(eeprom.read_bytes(), expected)
                else:
                    self.assertTrue(report["inputs_unchanged"])
                    self.assertFalse(
                        report["eeprom"]["transition"]["changed"])
                    self.assertEqual(eeprom.read_bytes(), initial)
                self.assertEqual(
                    report["eeprom"]["transition"]["expected_result"],
                    expected_result)
                self.assertEqual(
                    report["eeprom_update_trace"]["expected_result"],
                    expected_result)
                self.assertEqual(
                    report["eeprom_update_wire"]["expected_result"],
                    expected_result)
                self.assertEqual(
                    report["eeprom_update_wire"]["update_positions"],
                    [0, 1])
                if expected_result == "program-failure":
                    recovery_report_path = temp / "recovery-report.json"
                    manifest_path.write_text(json.dumps({
                        "schema": network_boot_gate.MANIFEST_SCHEMA,
                        "machine": "raspi4b", "memory": "2G",
                        "expected_firmware": "start4.elf",
                        "eeprom": {
                            "sha256":
                            network_boot_gate.sha256_file(eeprom),
                        },
                        "eeprom_update": {
                            "path": "pieeprom.upd",
                            "sha256": update_hash,
                            "size": network_boot_gate.EEPROM_SIZE,
                            "expected_result": "success",
                        },
                        "tftp_files": network_boot_gate.hash_tree(tftp),
                    }), encoding="utf-8")
                    recovery = subprocess.run([
                        sys.executable, str(SCRIPT),
                        "--qemu", str(qemu),
                        "--eeprom", str(eeprom),
                        "--tftp-root", str(tftp),
                        "--manifest", str(manifest_path),
                        "--allow-eeprom-update",
                        "--output", str(recovery_report_path),
                        "--timeout", "30",
                    ], capture_output=True, text=True, check=False)
                    self.assertEqual(
                        recovery.returncode, 0, recovery.stderr)
                    recovery_report = json.loads(
                        recovery_report_path.read_text(encoding="utf-8"))
                    self.assertEqual(eeprom.read_bytes(), update)
                    self.assertEqual(
                        recovery_report["eeprom"]["sha256"], update_hash)
                    self.assertEqual(
                        recovery_report["qmp"]["arm-handoff-status"],
                        "ready")


if __name__ == "__main__":
    unittest.main()
