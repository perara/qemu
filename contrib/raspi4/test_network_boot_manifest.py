#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("network_boot_manifest.py")


class NetworkBootManifestTests(unittest.TestCase):
    def test_builder_pins_exact_inputs_and_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tftp = root / "tftp"
            tftp.mkdir()
            eeprom = root / "pieeprom.bin"
            output = root / "manifest.json"
            eeprom.write_bytes(b"configured EEPROM")
            (tftp / "start4.elf").write_bytes(b"firmware")
            (tftp / "config.txt").write_bytes(b"arm_64bit=1\n")
            command = [
                sys.executable, str(SCRIPT), "--eeprom", str(eeprom),
                "--tftp-root", str(tftp), "--output", str(output),
            ]
            first = subprocess.run(
                command, capture_output=True, text=True, check=False,
            )
            self.assertEqual(first.returncode, 0, first.stderr)
            manifest = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(
                manifest["eeprom"]["sha256"],
                hashlib.sha256(b"configured EEPROM").hexdigest(),
            )
            self.assertEqual(
                manifest["tftp_files"]["start4.elf"],
                hashlib.sha256(b"firmware").hexdigest(),
            )
            self.assertIsNone(manifest["eeprom_update"])
            second = subprocess.run(
                command, capture_output=True, text=True, check=False,
            )
            self.assertEqual(second.returncode, 2)
            self.assertIn("already exists", second.stderr)

    def test_builder_rejects_manifest_inside_tftp_tree(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tftp = root / "tftp"
            tftp.mkdir()
            eeprom = root / "pieeprom.bin"
            eeprom.write_bytes(b"EEPROM")
            (tftp / "start4.elf").write_bytes(b"firmware")
            result = subprocess.run([
                sys.executable, str(SCRIPT), "--eeprom", str(eeprom),
                "--tftp-root", str(tftp),
                "--output", str(tftp / "manifest.json"),
            ], capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 2)
            self.assertIn("outside", result.stderr)

    def test_builder_seals_explicit_eeprom_update_and_signature(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tftp = root / "tftp"
            prefix = tftp / "device"
            prefix.mkdir(parents=True)
            eeprom = root / "pieeprom.bin"
            output = root / "manifest.json"
            update = b"\xa5" * (512 * 1024)
            eeprom.write_bytes(b"initial")
            (tftp / "start4.elf").write_bytes(b"firmware")
            (prefix / "pieeprom.upd").write_bytes(update)
            (prefix / "pieeprom.sig").write_text(
                hashlib.sha256(update).hexdigest() + "\n",
                encoding="ascii",
            )
            result = subprocess.run([
                sys.executable, str(SCRIPT), "--eeprom", str(eeprom),
                "--tftp-root", str(tftp), "--output", str(output),
                "--eeprom-update", "device/pieeprom.upd",
            ], capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            manifest = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(manifest["eeprom_update"], {
                "path": "device/pieeprom.upd",
                "sha256": hashlib.sha256(update).hexdigest(),
                "size": 512 * 1024,
                "expected_result": "success",
            })

    def test_builder_seals_expected_invalid_signature_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tftp = root / "tftp"
            tftp.mkdir()
            eeprom = root / "pieeprom.bin"
            output = root / "manifest.json"
            update = b"\xa5" * (512 * 1024)
            eeprom.write_bytes(b"initial")
            (tftp / "start4.elf").write_bytes(b"firmware")
            (tftp / "pieeprom.upd").write_bytes(update)
            (tftp / "pieeprom.sig").write_text(
                "0" * 64 + "\n", encoding="ascii")
            result = subprocess.run([
                sys.executable, str(SCRIPT), "--eeprom", str(eeprom),
                "--tftp-root", str(tftp), "--output", str(output),
                "--eeprom-update", "pieeprom.upd",
                "--eeprom-update-result", "invalid-signature",
            ], capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            manifest = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(
                manifest["eeprom_update"]["expected_result"],
                "invalid-signature",
            )

    def test_builder_rejects_result_signature_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tftp = root / "tftp"
            tftp.mkdir()
            eeprom = root / "pieeprom.bin"
            eeprom.write_bytes(b"initial")
            (tftp / "start4.elf").write_bytes(b"firmware")
            update = b"\xa5" * (512 * 1024)
            (tftp / "pieeprom.upd").write_bytes(update)
            (tftp / "pieeprom.sig").write_text(
                "0" * 64 + "\n", encoding="ascii")
            result = subprocess.run([
                sys.executable, str(SCRIPT), "--eeprom", str(eeprom),
                "--tftp-root", str(tftp),
                "--output", str(root / "manifest.json"),
                "--eeprom-update", "pieeprom.upd",
            ], capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 2)
            self.assertIn("requires a valid signature", result.stderr)

    def test_builder_seals_program_failure_boundary(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tftp = root / "tftp"
            tftp.mkdir()
            eeprom = root / "pieeprom.bin"
            output = root / "manifest.json"
            eeprom.write_bytes(b"initial")
            (tftp / "start4.elf").write_bytes(b"firmware")
            update = b"\xa5" * (512 * 1024)
            (tftp / "pieeprom.upd").write_bytes(update)
            (tftp / "pieeprom.sig").write_text(
                hashlib.sha256(update).hexdigest() + "\n",
                encoding="ascii")
            result = subprocess.run([
                sys.executable, str(SCRIPT), "--eeprom", str(eeprom),
                "--tftp-root", str(tftp), "--output", str(output),
                "--eeprom-update", "pieeprom.upd",
                "--eeprom-update-result", "program-failure",
                "--eeprom-update-fail-after", "512",
            ], capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            manifest = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(manifest["eeprom_update"]["fail_after"], 512)

    def test_builder_rejects_unsafe_or_wrong_size_update(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tftp = root / "tftp"
            tftp.mkdir()
            eeprom = root / "pieeprom.bin"
            output = root / "manifest.json"
            eeprom.write_bytes(b"initial")
            (tftp / "start4.elf").write_bytes(b"firmware")
            (tftp / "pieeprom.upd").write_bytes(b"short")
            (tftp / "pieeprom.sig").write_bytes(b"signature")
            result = subprocess.run([
                sys.executable, str(SCRIPT), "--eeprom", str(eeprom),
                "--tftp-root", str(tftp), "--output", str(output),
                "--eeprom-update", "pieeprom.upd",
            ], capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 2)
            self.assertIn("512 KiB", result.stderr)


if __name__ == "__main__":
    unittest.main()
