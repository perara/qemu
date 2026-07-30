#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests for the hash-pinned Raspberry Pi HAT EEPROM corpus runner."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).parent))

import hat_corpus


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def artifact(path: Path) -> dict[str, str]:
    return {"path": path.name, "sha256": digest(path.read_bytes())}


def expected() -> dict[str, object]:
    return {
        "boot_source": "sd-card",
        "vendor": "QEMU Labs",
        "product": "Virtual HAT",
        "uuid": "100f0e0d-0c0b-0a09-0807-060504030201",
        "product_id": 0x1234,
        "product_version": 0x5678,
        "custom_count": 1,
        "overlay": "embedded",
        "overlays_applied": 1,
        "gpio_status": "applied",
        "gpio_used_mask": 1 << 4,
        "gpio_drive": 2,
        "gpio_slew": 1,
        "gpio_hysteresis": 1,
        "gpio_back_power": 0,
        "final_device_tree_sha256": "1" * 64,
    }


class HatCorpusTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.bootloader = self.root / "pieeprom.bin"
        self.media = self.root / "media.img"
        self.hat = self.root / "board.eep"
        self.bootloader.write_bytes(b"bootloader")
        self.media.write_bytes(b"media")
        self.hat_data = (
            b"R-Pi" + bytes((1, 0)) + (0).to_bytes(2, "little") +
            (12).to_bytes(4, "little") + b"ignored-tail"
        )
        self.hat.write_bytes(self.hat_data)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def make_case(self, platform: str = "raspi4b") -> dict[str, object]:
        result = {
            "id": "known-hat",
            "platform": platform,
            "bootloader": artifact(self.bootloader),
            "media": artifact(self.media),
            "hat": artifact(self.hat),
            "expected": expected(),
        }
        if platform == "raspi-cm4":
            result["expected"]["boot_source"] = "emmc"
        return result

    def write_manifest(self, cases: list[dict[str, object]]) -> Path:
        path = self.root / "manifest.json"
        path.write_text(json.dumps({
            "schema": hat_corpus.SCHEMA,
            "cases": cases,
        }), encoding="utf-8")
        return path

    def test_derive_hat_preserves_three_hash_domains(self) -> None:
        derived = hat_corpus.derive_hat(self.hat)
        padded = self.hat_data + bytes(512 - len(self.hat_data))

        self.assertEqual(derived["file_size"], len(self.hat_data))
        self.assertEqual(derived["file_sha256"], digest(self.hat_data))
        self.assertEqual(derived["backend_size"], 512)
        self.assertEqual(derived["backend_sha256"], digest(padded))
        self.assertEqual(derived["declared_size"], 12)
        self.assertEqual(
            derived["content_sha256"], digest(self.hat_data[:12]))

    def test_preflight_resolves_all_hash_pinned_artifacts(self) -> None:
        cases = hat_corpus.preflight_manifest(
            self.write_manifest([self.make_case()]))

        self.assertEqual(len(cases), 1)
        self.assertEqual(cases[0]["hat"]["path"], self.hat.resolve())
        self.assertEqual(cases[0]["hat"]["sha256"], digest(self.hat_data))

    def test_preflight_rejects_hash_mismatch(self) -> None:
        case = self.make_case()
        case["hat"]["sha256"] = "0" * 64

        with self.assertRaisesRegex(
                hat_corpus.CorpusError, "SHA-256 does not match"):
            hat_corpus.preflight_manifest(self.write_manifest([case]))

    def test_preflight_rejects_impossible_gpio_policy(self) -> None:
        case = self.make_case()
        case["expected"]["gpio_drive"] = 9

        with self.assertRaisesRegex(
                hat_corpus.CorpusError, "GPIO policy is out of range"):
            hat_corpus.preflight_manifest(self.write_manifest([case]))

    def test_preflight_rejects_duplicate_before_execution(self) -> None:
        case = self.make_case()

        with self.assertRaisesRegex(
                hat_corpus.CorpusError, "duplicate case id"):
            hat_corpus.preflight_manifest(
                self.write_manifest([case, case]))

    def test_preflight_rejects_symlink_artifact(self) -> None:
        link = self.root / "linked.eep"
        link.symlink_to(self.hat)
        case = self.make_case()
        case["hat"] = artifact(link)

        with self.assertRaisesRegex(
                hat_corpus.CorpusError, "non-symlink"):
            hat_corpus.preflight_manifest(self.write_manifest([case]))

    def test_qemu_commands_are_snapshot_or_read_only(self) -> None:
        qmp = self.root / "qmp.sock"
        pi4 = hat_corpus.preflight_manifest(
            self.write_manifest([self.make_case()]))[0]
        pi4_command = hat_corpus.qemu_command(Path("/qemu"), pi4, qmp)

        self.assertIn("raspi4b,boot-mode=behavioral", pi4_command[4])
        self.assertTrue(any(
            item.startswith("if=sd,format=raw,snapshot=on")
            for item in pi4_command))
        self.assertTrue(any(
            "id=hat,format=raw,readonly=on" in item
            for item in pi4_command))

        cm4 = hat_corpus.preflight_manifest(
            self.write_manifest([self.make_case("raspi-cm4")]))[0]
        cm4_command = hat_corpus.qemu_command(Path("/qemu"), cm4, qmp)
        self.assertIn("emmc-drive=emmc", cm4_command[4])
        self.assertTrue(any(
            "id=emmc,format=raw,snapshot=on" in item
            for item in cm4_command))

    def test_compare_case_reports_exact_drift(self) -> None:
        case = self.make_case()
        derived = hat_corpus.derive_hat(self.hat)
        observed = {
            **case["expected"],
            "boot_state": "arm-handoff-ready",
            "handoff_status": "ready",
            "hat_status": "valid",
            "hat_eeprom_size": derived["backend_size"],
            "hat_eeprom_declared_size": derived["declared_size"],
            "hat_eeprom_sha256": derived["backend_sha256"],
            "hat_content_sha256": derived["content_sha256"],
        }
        self.assertEqual(
            hat_corpus.compare_case(case, derived, observed), {})

        observed["gpio_drive"] = 9
        self.assertEqual(
            hat_corpus.compare_case(case, derived, observed),
            {"gpio_drive": {"expected": 2, "observed": 9}})

    def test_atomic_report_replaces_existing_document(self) -> None:
        output = self.root / "reports" / "result.json"
        output.parent.mkdir()
        output.write_text("old", encoding="utf-8")

        hat_corpus.atomic_write_json(output, {"match": True})

        self.assertEqual(
            json.loads(output.read_text(encoding="utf-8")),
            {"match": True})
        self.assertEqual(list(output.parent.glob(f".{output.name}.*")), [])

    def test_run_writes_complete_report_before_raising_drift(self) -> None:
        qemu = self.root / "qemu"
        qemu.write_bytes(b"binary")
        qemu.chmod(0o700)
        output = self.root / "result.json"
        cases = [{"id": "one"}, {"id": "two"}]
        results = [
            {"id": "one", "match": False},
            {"id": "two", "match": True},
        ]
        args = SimpleNamespace(
            qemu=qemu, manifest=self.root / "unused.json",
            output=output, timeout=1.0)

        with mock.patch.object(
                hat_corpus, "preflight_manifest", return_value=cases), \
             mock.patch.object(
                 hat_corpus, "execute_case", side_effect=results):
            with self.assertRaisesRegex(
                    hat_corpus.CorpusDrift, "1 of 2 cases"):
                hat_corpus.run(args)

        report = json.loads(output.read_text(encoding="utf-8"))
        self.assertFalse(report["match"])
        self.assertEqual(report["cases"], results)


if __name__ == "__main__":
    unittest.main()
