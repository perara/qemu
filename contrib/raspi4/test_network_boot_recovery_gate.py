#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("network_boot_recovery_gate.py")
MANIFEST_SCRIPT = Path(__file__).with_name("network_boot_manifest.py")
SPEC = importlib.util.spec_from_file_location(
    "network_boot_recovery_gate", SCRIPT)
assert SPEC and SPEC.loader
recovery_gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(recovery_gate)


class NetworkBootRecoveryGateTests(unittest.TestCase):
    def test_contract_requires_bounded_program_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest = Path(directory) / "manifest.json"
            data = {
                "schema": recovery_gate.MANIFEST_SCHEMA,
                "machine": "raspi4b",
                "memory": "2G",
                "expected_firmware": "start4.elf",
                "eeprom_update": {
                    "path": "pieeprom.upd",
                    "sha256": "1" * 64,
                    "size": recovery_gate.EEPROM_SIZE,
                    "expected_result": "program-failure",
                    "fail_after": 512,
                },
            }
            manifest.write_text(json.dumps(data), encoding="utf-8")
            self.assertEqual(
                recovery_gate.load_failure_contract(manifest), data)
            data["eeprom_update"]["expected_result"] = "success"
            manifest.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaisesRegex(
                    recovery_gate.GateError, "program-failure"):
                recovery_gate.load_failure_contract(manifest)

    @unittest.skipUnless(
        os.environ.get("QEMU_RPI_GATE_QEMU") and
        os.environ.get("QEMU_RPI_GATE_TFTP_ROOT"),
        "set QEMU_RPI_GATE_QEMU and QEMU_RPI_GATE_TFTP_ROOT",
    )
    def test_real_interrupted_update_recovers_with_same_artifacts(self):
        qemu = Path(os.environ["QEMU_RPI_GATE_QEMU"]).resolve()
        source_tftp = Path(
            os.environ["QEMU_RPI_GATE_TFTP_ROOT"]).resolve()

        def make_eeprom(bootvar: int) -> bytes:
            config = (
                "BOOT_ORDER=0xe2\nENABLE_SELF_UPDATE=1\n"
                "NET_BOOT_MAX_RETRIES=0\n"
                f"BOOTVAR0=0x{bootvar:x}\n"
            ).encode("ascii")
            image = bytearray(b"\xff" * recovery_gate.EEPROM_SIZE)
            struct.pack_into(">II", image, 0, 0x55AAF11F,
                             16 + len(config))
            image[8:20] = b"bootconf.txt"
            image[20:24] = b"\0" * 4
            image[24:24 + len(config)] = config
            image[8192:12288] = bytes([bootvar & 0xff]) * 4096
            return bytes(image)

        with tempfile.TemporaryDirectory(
                prefix="qemu-rpi-recovery-campaign-") as name:
            temp = Path(name)
            tftp = temp / "tftp"
            shutil.copytree(source_tftp, tftp, symlinks=True)
            eeprom = temp / "pieeprom.bin"
            update = make_eeprom(0x2222)
            eeprom.write_bytes(make_eeprom(0x1111))
            (tftp / "pieeprom.upd").write_bytes(update)
            update_hash = recovery_gate.sha256_file(
                tftp / "pieeprom.upd")
            (tftp / "pieeprom.sig").write_text(
                update_hash + "\n", encoding="ascii")
            failure_manifest = temp / "failure-manifest.json"
            build = subprocess.run([
                sys.executable, str(MANIFEST_SCRIPT),
                "--eeprom", str(eeprom),
                "--tftp-root", str(tftp),
                "--output", str(failure_manifest),
                "--eeprom-update", "pieeprom.upd",
                "--eeprom-update-result", "program-failure",
                "--eeprom-update-fail-after", "512",
            ], capture_output=True, text=True, check=False)
            self.assertEqual(build.returncode, 0, build.stderr)
            report_path = temp / "campaign.json"
            evidence = temp / "evidence"
            result = subprocess.run([
                sys.executable, str(SCRIPT),
                "--qemu", str(qemu),
                "--eeprom", str(eeprom),
                "--tftp-root", str(tftp),
                "--manifest", str(failure_manifest),
                "--evidence-directory", str(evidence),
                "--output", str(report_path),
                "--timeout", "30",
            ], capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(
                report_path.read_text(encoding="utf-8"))
            self.assertTrue(report["verified"])
            self.assertNotEqual(
                report["eeprom"]["partial_sha256"], update_hash)
            self.assertEqual(report["eeprom"]["final_sha256"], update_hash)
            self.assertEqual(eeprom.read_bytes(), update)
            for name in (
                    "failure-report.json", "failure.pcap",
                    "recovery-manifest.json", "recovery-report.json",
                    "recovery.pcap"):
                self.assertTrue((evidence / name).is_file(), name)


if __name__ == "__main__":
    unittest.main()
