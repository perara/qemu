#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

import hashlib
import json
import tempfile
import unittest
from pathlib import Path

try:
    from contrib.raspi4.evidence_integrity import EvidenceError, verify_summary
except ModuleNotFoundError:
    from evidence_integrity import EvidenceError, verify_summary


class EvidenceIntegrityTests(unittest.TestCase):
    def test_matching_retained_reference_passes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            report = root / "report.json"
            report.write_bytes(b'{"status":"complete"}\n')
            summary = root / "summary.json"
            summary.write_text(json.dumps({
                "status": "complete",
                "supervisor_report_path": "report.json",
                "supervisor_report_sha256":
                    hashlib.sha256(report.read_bytes()).hexdigest(),
            }), encoding="utf-8")
            self.assertEqual(verify_summary(summary), 1)

    def test_missing_retained_reference_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            summary = Path(directory) / "summary.json"
            summary.write_text(json.dumps({
                "status": "complete",
                "supervisor_report_path": "missing.json",
                "supervisor_report_sha256": "0" * 64,
            }), encoding="utf-8")
            with self.assertRaises(EvidenceError):
                verify_summary(summary)

    def test_hash_mismatch_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "report.json").write_text("changed", encoding="utf-8")
            summary = root / "summary.json"
            summary.write_text(json.dumps({
                "status": "complete",
                "supervisor_report_path": "report.json",
                "supervisor_report_sha256": "0" * 64,
            }), encoding="utf-8")
            with self.assertRaises(EvidenceError):
                verify_summary(summary)


if __name__ == "__main__":
    unittest.main()
