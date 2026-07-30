#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from pathlib import Path
import hashlib
import importlib.util
import json
import os
import sys
import tempfile
import unittest


ROOT = Path(__file__).parent
sys.path.insert(0, str(ROOT))
MODULE_PATH = ROOT / "hil_orchestrator.py"
SPEC = importlib.util.spec_from_file_location("hil_orchestrator", MODULE_PATH)
assert SPEC and SPEC.loader
hil = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(hil)


class HILOrchestratorTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.artifacts = {}
        for name in ("eeprom", "firmware", "media"):
            path = self.root / f"{name}.bin"
            path.write_bytes(f"exact {name} bytes".encode())
            self.artifacts[name] = {
                "path": path.name,
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            }
        self.events = self.root / "events"
        self.trace = self.root / "hardware.jsonl"
        self.report = self.root / "report.json"

    def tearDown(self):
        self.temporary.cleanup()

    def command(self, name, fail=False, capture=False):
        script = (
            "from pathlib import Path; "
            "p=Path('events'); "
            f"p.write_text((p.read_text() if p.exists() else '')+'{name}\\n');"
        )
        if capture:
            script += f"print({self.trace_text()!r}, end='');"
        if name in hil.FLASH_STEPS and not fail:
            script += (
                f"print({json.dumps(self.flash_attestation())!r}, end='');"
            )
        if fail:
            script += "raise SystemExit(7)"
        return {
            "name": name,
            "command": [sys.executable, "-c", script],
            "timeout_s": 10,
            "capture_stdout": capture,
            "capture_flash_attestation": name in hil.FLASH_STEPS,
        }

    def flash_attestation(self):
        media = self.root / "media.bin"
        size = media.stat().st_size
        digest = self.artifacts["media"]["sha256"]
        capacity = max(4096, size)
        return {
            "source": str(media),
            "target": "/dev/disk/by-id/mock-rpi-media",
            "target_identity": {
                "path": "/dev/mock-sdz",
                "device": 1,
                "inode": 2,
                "rdev": os.makedev(8, 0),
                "kind": "block",
                "major": 8,
                "minor": 0,
                "kernel_name": "sdz",
                "sysfs_path": "/sys/devices/mock/block/sdz",
                "capacity": capacity,
                "logical_sector_size": 512,
                "removable": True,
                "read_only": False,
            },
            "bytes_written": size,
            "source_sha256": digest,
            "verified": True,
            "resumed": False,
            "resume_from": 0,
            "resume_journal": "/var/tmp/mock-sdz.rpi-resume.json",
            "target_capacity": capacity,
            "target_sha256": digest,
        }

    def trace_text(self):
        header = {
            "record": "header",
            "schema": "qemu.raspi.boot-trace",
            "version": 1,
            "platform": self.platform,
            "producer": "hardware",
            "artifacts": {
                "board_revision": self.revision,
                **{
                    f"{name}_sha256": value["sha256"]
                    for name, value in self.artifacts.items()
                },
            },
        }
        event = {
            "record": "event",
            "seq": 0,
            "phase": "health",
            "event": "health.application-ready",
            "outcome": "success",
        }
        return json.dumps(header) + "\n" + json.dumps(event) + "\n"

    def plan(self, platform="raspi4b", fail_step=None):
        self.platform = platform
        self.revision = "0xb03115" if platform == "raspi4b" else "0xc03140"
        steps = [
            self.command(
                name,
                fail=name == fail_step,
                capture=name == "capture-uart",
            )
            for name in hil.SEQUENCES[platform]
        ]
        flash_name = "flash-media" if platform == "raspi4b" else "flash-emmc"
        next(step for step in steps if step["name"] == flash_name)[
            "command"
        ].extend(["{artifact:media}", "{flash-target}"])
        capture = next(
            step for step in steps if step["name"] == "capture-uart"
        )
        capture["command"].extend(
            ["{artifact:eeprom}", "{artifact:firmware}"]
        )
        cleanup = [
            self.command(name)
            for name in hil.CLEANUP_SEQUENCES[platform]
        ]
        value = {
            "schema": hil.SCHEMA,
            "version": hil.VERSION,
            "id": f"{platform}-release",
            "platform": platform,
            "board_revision": self.revision,
            "flash_target": "/dev/disk/by-id/mock-rpi-media",
            "artifacts": self.artifacts,
            "trace": self.trace.name,
            "steps": steps,
            "cleanup": cleanup,
        }
        path = self.root / "plan.json"
        path.write_text(json.dumps(value), encoding="utf-8")
        return path

    def test_pi4_sequence_trace_and_report(self):
        result = hil.run_plan(self.plan(), self.report)
        self.assertTrue(result["success"])
        self.assertEqual(
            self.events.read_text().splitlines(),
            list(hil.SEQUENCES["raspi4b"]) +
            list(hil.CLEANUP_SEQUENCES["raspi4b"]),
        )
        saved = json.loads(self.report.read_text())
        self.assertEqual(saved["trace"]["records"], 2)
        self.assertEqual(saved["trace"]["sha256"],
                         hashlib.sha256(self.trace.read_bytes()).hexdigest())
        self.assertEqual(
            saved["steps"][1]["flash_attestation"],
            self.flash_attestation(),
        )

    def test_cm4_sequence_includes_nrpiboot_and_cleanup(self):
        result = hil.run_plan(self.plan("cm4"), self.report)
        self.assertTrue(result["success"])
        self.assertEqual(
            self.events.read_text().splitlines(),
            list(hil.SEQUENCES["cm4"]) +
            list(hil.CLEANUP_SEQUENCES["cm4"]),
        )

    def test_failure_skips_remaining_steps_and_always_cleans_up(self):
        with self.assertRaisesRegex(hil.HILError, "status 7"):
            hil.run_plan(self.plan(fail_step="flash-media"), self.report)
        self.assertEqual(
            self.events.read_text().splitlines(),
            ["power-off", "flash-media", "power-off-cleanup"],
        )
        saved = json.loads(self.report.read_text())
        self.assertFalse(saved["success"])
        self.assertIn("flash-media", saved["error"])
        self.assertEqual(saved["steps"][-1]["returncode"], 7)
        self.assertFalse(self.trace.exists())

    def test_artifact_mismatch_prevents_hardware_commands(self):
        plan = self.plan()
        (self.root / "media.bin").write_bytes(b"tampered")
        with self.assertRaisesRegex(hil.HILError, "SHA-256 mismatch"):
            hil.run_plan(plan, self.report)
        self.assertFalse(self.events.exists())
        self.assertFalse(self.report.exists())

    def test_trace_artifact_collision_prevents_hardware_commands(self):
        plan = self.plan()
        value = json.loads(plan.read_text())
        value["trace"] = value["artifacts"]["media"]["path"]
        plan.write_text(json.dumps(value), encoding="utf-8")
        original_media = (self.root / "media.bin").read_bytes()
        with self.assertRaisesRegex(hil.HILError, "path collision"):
            hil.run_plan(plan, self.report)
        self.assertEqual((self.root / "media.bin").read_bytes(), original_media)
        self.assertFalse(self.events.exists())
        self.assertFalse(self.report.exists())

    def test_report_hardlink_collision_prevents_hardware_commands(self):
        plan = self.plan()
        media = self.root / "media.bin"
        self.report.hardlink_to(media)
        original_media = media.read_bytes()
        with self.assertRaisesRegex(hil.HILError, "same file"):
            hil.run_plan(plan, self.report)
        self.assertEqual(media.read_bytes(), original_media)
        self.assertFalse(self.events.exists())

    def test_symlink_output_prevents_hardware_commands(self):
        plan = self.plan()
        self.trace.symlink_to(self.root / "outside-trace")
        with self.assertRaisesRegex(hil.HILError, "must not be a symlink"):
            hil.run_plan(plan, self.report)
        self.assertTrue(self.trace.is_symlink())
        self.assertFalse(self.events.exists())
        self.assertFalse(self.report.exists())

        self.trace.unlink()
        media = self.root / "media.bin"
        media_target = self.root / "media-real.bin"
        media.rename(media_target)
        media.symlink_to(media_target)
        with self.assertRaisesRegex(
            hil.HILError, "artifact media path must not be a symlink"
        ):
            hil.run_plan(plan, self.report)
        self.assertFalse(self.events.exists())

        media.unlink()
        media_target.rename(media)
        plan_link = self.root / "plan-link.json"
        plan_link.symlink_to(plan)
        with self.assertRaisesRegex(
            hil.HILError, "plan path must not be a symlink"
        ):
            hil.run_plan(plan_link, self.report)
        self.assertFalse(self.events.exists())

    def test_wrong_platform_sequence_is_rejected(self):
        plan = self.plan()
        value = json.loads(plan.read_text())
        value["steps"][1]["name"] = "assert-nrpiboot"
        plan.write_text(json.dumps(value), encoding="utf-8")
        with self.assertRaisesRegex(hil.HILError, "plan.steps"):
            hil.run_plan(plan, self.report)
        self.assertFalse(self.events.exists())

    def test_missing_or_invalid_flash_attestation_fails_closed(self):
        plan = self.plan()
        value = json.loads(plan.read_text())
        flash = value["steps"][1]
        flash["command"] = [
            sys.executable,
            "-c",
            "print('not-json')",
            "{artifact:media}",
            "{flash-target}",
        ]
        plan.write_text(json.dumps(value), encoding="utf-8")
        with self.assertRaisesRegex(
            hil.HILError, "invalid flash attestation"
        ):
            hil.run_plan(plan, self.report)
        saved = json.loads(self.report.read_text())
        self.assertFalse(saved["success"])
        self.assertIn("invalid flash attestation", saved["error"])

    def test_wrong_media_or_regular_target_attestation_fails_closed(self):
        for change, message in (
            ({"source_sha256": "0" * 64}, "media SHA-256 mismatch"),
            ({"source": "/tmp/other.img"}, "source does not match"),
            (
                {"target": "/dev/disk/by-id/other-media"},
                "target does not match authorized target",
            ),
            (
                {"target_identity": {
                    **self.flash_attestation()["target_identity"],
                    "kind": "regular",
                }},
                "target must be a block device",
            ),
        ):
            with self.subTest(message=message):
                plan = self.plan()
                value = json.loads(plan.read_text())
                attestation = {**self.flash_attestation(), **change}
                value["steps"][1]["command"] = [
                    sys.executable,
                    "-c",
                    f"print({json.dumps(attestation)!r}, end='')",
                    "{artifact:media}",
                    "{flash-target}",
                ]
                plan.write_text(json.dumps(value), encoding="utf-8")
                with self.assertRaisesRegex(hil.HILError, message):
                    hil.run_plan(plan, self.report)
                self.report.unlink(missing_ok=True)
                self.events.unlink(missing_ok=True)

    def test_unstable_flash_target_is_rejected_before_commands(self):
        for flash_target in (
            "/dev/sdz",
            "/dev/disk/by-id/nested/mock-rpi-media",
        ):
            with self.subTest(flash_target=flash_target):
                plan = self.plan()
                value = json.loads(plan.read_text())
                value["flash_target"] = flash_target
                plan.write_text(json.dumps(value), encoding="utf-8")
                with self.assertRaisesRegex(
                    hil.HILError, "normalized /dev/disk"
                ):
                    hil.run_plan(plan, self.report)
                self.assertFalse(self.events.exists())


if __name__ == "__main__":
    unittest.main()
