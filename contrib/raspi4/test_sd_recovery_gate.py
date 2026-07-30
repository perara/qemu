#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT_DIRECTORY = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIRECTORY.parents[1]
FUNCTIONAL_DIRECTORY = REPO_ROOT / "tests" / "functional"
FAT16_SPEC = importlib.util.spec_from_file_location(
    "raspi4_test_fat16",
    FUNCTIONAL_DIRECTORY / "qemu_test" / "fat16.py")
assert FAT16_SPEC and FAT16_SPEC.loader
fat16 = importlib.util.module_from_spec(FAT16_SPEC)
FAT16_SPEC.loader.exec_module(fat16)
create_fat16_image = fat16.create_fat16_image


MANIFEST_SCRIPT = SCRIPT_DIRECTORY / "sd_recovery_manifest.py"
GATE_SCRIPT = SCRIPT_DIRECTORY / "sd_recovery_gate.py"
SPEC = importlib.util.spec_from_file_location(
    "sd_recovery_gate", GATE_SCRIPT)
assert SPEC and SPEC.loader
sd_recovery_gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(sd_recovery_gate)


class SdRecoveryGateTests(unittest.TestCase):
    def test_expected_rename_hash_is_exact(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "sd.img"
            prefix = b"\x00" * 32
            suffix = b"\x20" + b"suffix"
            before = prefix + b"RECOVERYBIN" + suffix
            after = prefix + b"RECOVERY000" + suffix
            image.write_bytes(before)
            digest, offset = sd_recovery_gate.expected_renamed_sd_hash(
                image)
            self.assertEqual(offset, len(prefix))
            self.assertEqual(digest, hashlib.sha256(after).hexdigest())
            image.write_bytes(before + b"RECOVERYBIN")
            with self.assertRaisesRegex(
                    sd_recovery_gate.GateError, "exactly one"):
                sd_recovery_gate.expected_renamed_sd_hash(image)

    def test_manifest_output_cannot_replace_an_input(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            eeprom = root / "pieeprom.bin"
            sd_image = root / "recovery.img"
            recovery = root / "recovery.bin"
            update = root / "pieeprom.upd"
            firmware = root / "start4.elf"
            fixup = root / "fixup4.dat"
            kernel = root / "kernel8.img"
            eeprom.write_bytes(b"\xff" * (512 * 1024))
            sd_image.write_bytes(b"RECOVERYBIN")
            recovery.write_bytes(b"recovery")
            update.write_bytes(b"\x00" * (512 * 1024))
            firmware.write_bytes(b"firmware")
            fixup.write_bytes(b"fixup")
            kernel.write_bytes(b"kernel")
            build = subprocess.run([
                sys.executable, str(MANIFEST_SCRIPT),
                "--eeprom", str(eeprom),
                "--sd-image", str(sd_image),
                "--recovery", str(recovery),
                "--update", str(update),
                "--firmware", str(firmware),
                "--fixup", str(fixup),
                "--kernel", str(kernel),
                "--output", str(eeprom),
                "--force",
            ], capture_output=True, text=True, check=False)
            self.assertEqual(build.returncode, 2)
            self.assertIn("conflicts with an input", build.stderr)
            self.assertEqual(eeprom.read_bytes(), b"\xff" * (512 * 1024))

    @unittest.skipUnless(
        os.environ.get("QEMU_RPI_GATE_QEMU") and
        os.environ.get("QEMU_RPI_GATE_TFTP_ROOT"),
        "set QEMU_RPI_GATE_QEMU and QEMU_RPI_GATE_TFTP_ROOT",
    )
    def test_official_recovery_updates_and_reboots_from_same_sd(self):
        qemu = Path(os.environ["QEMU_RPI_GATE_QEMU"]).resolve()
        boot = Path(os.environ["QEMU_RPI_GATE_TFTP_ROOT"]).resolve()
        eeprom_root = (
            REPO_ROOT / "build" / ".raspi-upstream" /
            "rpi-eeprom" / "firmware-2711")
        recovery = eeprom_root / "default" / "recovery.bin"
        update = eeprom_root / "default" / "pieeprom-2026-05-17.bin"
        initial = (
            eeprom_root / "old" / "stable" /
            "pieeprom-2026-04-14.bin")
        required = [
            recovery, update, initial,
            boot / "start4.elf", boot / "fixup4.dat",
            boot / "kernel8.img", boot / "bcm2711-rpi-4-b.dtb",
            boot / "initramfs8",
        ]
        for path in required:
            self.assertTrue(path.is_file(), path)

        with tempfile.TemporaryDirectory(
                prefix="qemu-rpi-sd-recovery-gate-") as directory:
            root = Path(directory)
            eeprom = root / "pieeprom.bin"
            sd_image = root / "recovery.img"
            manifest = root / "manifest.json"
            report_path = root / "report.json"
            eeprom.write_bytes(initial.read_bytes())
            update_bytes = update.read_bytes()
            signature = (
                hashlib.sha256(update_bytes).hexdigest() +
                "\nts: 1\n").encode("ascii")
            create_fat16_image(sd_image, [
                ("recovery.bin", "RECOVERYBIN", recovery.read_bytes()),
                ("pieeprom.upd", "PIEEPROMUPD", update_bytes),
                ("pieeprom.sig", "PIEEPROMSIG", signature),
                ("start4.elf", "START4  ELF",
                 (boot / "start4.elf").read_bytes()),
                ("fixup4.dat", "FIXUP4  DAT",
                 (boot / "fixup4.dat").read_bytes()),
                ("kernel8.img", "KERNEL8 IMG",
                 (boot / "kernel8.img").read_bytes()),
                ("bcm2711-rpi-4-b.dtb", "BCM271~1DTB",
                 (boot / "bcm2711-rpi-4-b.dtb").read_bytes()),
                ("initramfs8", "INITRA~1   ",
                 (boot / "initramfs8").read_bytes()),
                ("config.txt", "CONFIG  TXT",
                 b"arm_64bit=1\nauto_initramfs=1\n"),
                ("cmdline.txt", "CMDLINE TXT",
                 b"console=ttyAMA0 rdinit=/sbin/init\n"),
            ])
            build = subprocess.run([
                sys.executable, str(MANIFEST_SCRIPT),
                "--eeprom", str(eeprom),
                "--sd-image", str(sd_image),
                "--recovery", str(recovery),
                "--update", str(update),
                "--firmware", str(boot / "start4.elf"),
                "--fixup", str(boot / "fixup4.dat"),
                "--kernel", str(boot / "kernel8.img"),
                "--output", str(manifest),
            ], capture_output=True, text=True, check=False)
            self.assertEqual(build.returncode, 0, build.stderr)
            gate = subprocess.run([
                sys.executable, str(GATE_SCRIPT),
                "--qemu", str(qemu),
                "--eeprom", str(eeprom),
                "--sd-image", str(sd_image),
                "--recovery", str(recovery),
                "--update", str(update),
                "--firmware", str(boot / "start4.elf"),
                "--fixup", str(boot / "fixup4.dat"),
                "--kernel", str(boot / "kernel8.img"),
                "--manifest", str(manifest),
                "--output", str(report_path),
                "--timeout", "30",
            ], capture_output=True, text=True, check=False)
            self.assertEqual(gate.returncode, 0, gate.stderr)
            report = json.loads(
                report_path.read_text(encoding="utf-8"))
            update_hash = hashlib.sha256(update_bytes).hexdigest()
            self.assertTrue(report["verified"])
            self.assertEqual(
                report["eeprom"]["after_sha256"], update_hash)
            self.assertEqual(eeprom.read_bytes(), update_bytes)
            self.assertGreaterEqual(
                report["trace"]["reset_released_count"], 2)
            self.assertEqual(
                report["qmp"]["arm-handoff-status"], "ready")
            self.assertIn(b"RECOVERY000", sd_image.read_bytes())
            self.assertNotIn(b"RECOVERYBIN", sd_image.read_bytes())


if __name__ == "__main__":
    unittest.main()
