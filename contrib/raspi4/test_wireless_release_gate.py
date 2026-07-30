#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

import importlib.util
from pathlib import Path
import tempfile
import unittest


MODULE_PATH = Path(__file__).with_name("wireless_release_gate.py")
SPEC = importlib.util.spec_from_file_location("wireless_release_gate",
                                              MODULE_PATH)
assert SPEC and SPEC.loader
wireless_release_gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(wireless_release_gate)


class WirelessReleaseGateTests(unittest.TestCase):
    def test_accepts_complete_unchanged_stack_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            paths = {}
            for name in ("qemu", "kernel", "dtb", "initramfs",
                         "module_package"):
                paths[name] = root / name
                paths[name].write_bytes(name.encode())
            paths["console_log"] = root / "console.log"
            paths["console_log"].write_text(
                "\n".join(wireless_release_gate.REQUIRED_MARKERS))
            report = wireless_release_gate.verify(paths)
            self.assertEqual(report["result"], "pass")
            self.assertEqual(len(report["artifacts"]), 6)

    def test_rejects_missing_or_failed_driver_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            paths = {}
            for name in ("qemu", "kernel", "dtb", "initramfs",
                         "module_package"):
                paths[name] = root / name
                paths[name].write_bytes(name.encode())
            paths["console_log"] = root / "console.log"
            paths["console_log"].write_text(
                "\n".join(wireless_release_gate.REQUIRED_MARKERS) +
                "\nunexpected cc 0x0c23\n")
            with self.assertRaisesRegex(ValueError, "forbidden"):
                wireless_release_gate.verify(paths)


if __name__ == "__main__":
    unittest.main()
