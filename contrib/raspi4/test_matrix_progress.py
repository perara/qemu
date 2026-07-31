#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from pathlib import Path
import importlib.util
import unittest


MODULE_PATH = Path(__file__).with_name("matrix_progress.py")
SPEC = importlib.util.spec_from_file_location("matrix_progress", MODULE_PATH)
assert SPEC and SPEC.loader
matrix_progress = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(matrix_progress)


class MatrixProgressTests(unittest.TestCase):
    def test_repository_dashboard_matches_capability_rows(self):
        matrix = (MODULE_PATH.resolve().parents[2]
                  / "RASPI4_IMPLEMENTATION_MATRIX.md")
        report = matrix_progress.compute(matrix)
        matrix_progress.check_dashboard(matrix, report)
        self.assertEqual(report["capabilities"], 106)
        self.assertEqual(report["full"], 93)
        self.assertEqual(report["excluded"], 1)
        self.assertEqual(report["partial"], 0)
        self.assertEqual(report["bridgeable"], 0)
        self.assertEqual(report["missing"], 0)
        self.assertEqual(report["hil_pending"], 12)
        self.assertEqual(report["partial_scores"], {})
        self.assertAlmostEqual(report["weighted_percent"], 100.0,
                               places=6)
        self.assertAlmostEqual(report["unweighted_percent"], 100.0,
                               places=6)


if __name__ == "__main__":
    unittest.main()
