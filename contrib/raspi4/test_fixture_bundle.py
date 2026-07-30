#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from pathlib import Path
import hashlib
import json
import sys
import tempfile
import unittest


ROOT = Path(__file__).parent
sys.path.insert(0, str(ROOT))
import fixture_bundle as bundle
import hil_orchestrator
import conformance_gate


class FixtureBundleTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.artifacts = {}
        for name in ("eeprom", "firmware", "media"):
            path = self.root / f"{name}.bin"
            path.write_bytes(f"exact {name}".encode())
            self.artifacts[name] = path.name
        self.output = self.root / "bundle"

    def tearDown(self):
        self.temporary.cleanup()

    def command(self, name):
        command = [sys.executable, "-c", f"print({name!r})"]
        if name == "flash-media":
            command.extend(["{artifact:media}", "{flash-target}"])
        if name == "capture-uart":
            command.extend(["{artifact:eeprom}", "{artifact:firmware}"])
        return {"command": command, "timeout_s": 10}

    def spec(self, platform="raspi4b"):
        fixture_commands = {
            name: self.command(name)
            for name in (
                *hil_orchestrator.SEQUENCES[platform],
                *hil_orchestrator.CLEANUP_SEQUENCES[platform],
            )
        }
        if platform == "cm4":
            fixture_commands["flash-emmc"]["command"].append(
                "{artifact:media}"
            )
            fixture_commands["flash-emmc"]["command"].append(
                "{flash-target}"
            )
            fixture_commands["flash-media"] = None
            del fixture_commands["flash-media"]
        qemu_command = [
            sys.executable,
            "-c",
            "print('qemu trace')",
            "{artifact:eeprom}",
            "{artifact:firmware}",
            "{artifact:media}",
        ]
        value = {
            "schema": bundle.SCHEMA,
            "version": bundle.VERSION,
            "id": f"{platform}-release",
            "platform": platform,
            "board_revision": (
                "0xb03115" if platform == "raspi4b" else "0xc03140"
            ),
            "flash_target": "/dev/disk/by-id/mock-rpi-media",
            "artifacts": self.artifacts,
            "event_contract": [
                {
                    "phase": "health",
                    "event": "health.application-ready",
                    "outcome": "success",
                }
            ],
            "fixture_commands": fixture_commands,
            "qemu": {"command": qemu_command, "timeout_s": 60},
        }
        path = self.root / f"{platform}.spec.json"
        path.write_text(json.dumps(value), encoding="utf-8")
        return path

    def test_pi4_bundle_pins_artifacts_and_preflights(self):
        plan_path, manifest_path = bundle.build_bundle(
            self.spec(), self.output
        )
        plan = hil_orchestrator.load_plan(plan_path)
        self.assertEqual(
            plan["flash_target"], "/dev/disk/by-id/mock-rpi-media"
        )
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        self.assertEqual(
            manifest["cases"][0]["flash_target"],
            "/dev/disk/by-id/mock-rpi-media",
        )
        manifest = conformance_gate.load_manifest(manifest_path)
        case = manifest["cases"][0]
        self.assertEqual(plan["platform"], "raspi4b")
        self.assertEqual(manifest["version"], 2)
        self.assertEqual(
            [item["name"] for item in plan["steps"]],
            list(hil_orchestrator.SEQUENCES["raspi4b"]),
        )
        flash = next(
            item for item in plan["steps"]
            if item["name"] == "flash-media"
        )
        self.assertTrue(flash["capture_flash_attestation"])
        for name, artifact in case["artifacts"].items():
            path = self.root / f"{name}.bin"
            self.assertEqual(artifact["path"], str(path.resolve()))
            self.assertEqual(
                artifact["sha256"],
                hashlib.sha256(path.read_bytes()).hexdigest(),
            )
            self.assertIn(
                str(path.resolve()), case["qemu"]["command"]
            )
        conformance_gate._prepare_case(manifest_path.parent, case)

    def test_cm4_bundle_has_exact_rpiboot_sequence_and_cleanup(self):
        plan_path, manifest_path = bundle.build_bundle(
            self.spec("cm4"), self.output
        )
        plan = json.loads(plan_path.read_text())
        self.assertEqual(
            [item["name"] for item in plan["steps"]],
            list(hil_orchestrator.SEQUENCES["cm4"]),
        )
        self.assertEqual(
            [item["name"] for item in plan["cleanup"]],
            list(hil_orchestrator.CLEANUP_SEQUENCES["cm4"]),
        )
        self.assertTrue(manifest_path.is_file())

    def test_existing_bundle_is_not_overwritten_without_force(self):
        spec = self.spec()
        plan, manifest = bundle.build_bundle(spec, self.output)
        original = plan.read_bytes(), manifest.read_bytes()
        with self.assertRaisesRegex(bundle.BundleError, "already exists"):
            bundle.build_bundle(spec, self.output)
        self.assertEqual((plan.read_bytes(), manifest.read_bytes()), original)
        bundle.build_bundle(spec, self.output, force=True)

    def test_missing_artifact_placeholder_fails_before_output(self):
        spec = self.spec()
        value = json.loads(spec.read_text())
        value["qemu"]["command"].remove("{artifact:firmware}")
        spec.write_text(json.dumps(value), encoding="utf-8")
        with self.assertRaisesRegex(
            bundle.BundleError, "QEMU command must reference"
        ):
            bundle.build_bundle(spec, self.output)
        self.assertEqual(list(self.output.iterdir()), [])

        value = json.loads(self.spec().read_text())
        del value["fixture_commands"]["health-check"]
        spec.write_text(json.dumps(value), encoding="utf-8")
        with self.assertRaisesRegex(
            bundle.BundleError, "fixture_commands missing health-check"
        ):
            bundle.build_bundle(spec, self.output)
        self.assertEqual(list(self.output.iterdir()), [])

        value = json.loads(self.spec().read_text())
        value["fixture_commands"]["flash-media"]["command"].remove(
            "{flash-target}"
        )
        spec.write_text(json.dumps(value), encoding="utf-8")
        with self.assertRaisesRegex(
            bundle.BundleError, "flash-media must reference"
        ):
            bundle.build_bundle(spec, self.output)
        self.assertEqual(list(self.output.iterdir()), [])

    def test_symlink_and_same_file_artifacts_are_rejected(self):
        spec = self.spec()
        media = self.root / "media.bin"
        real_media = self.root / "real-media.bin"
        media.rename(real_media)
        media.symlink_to(real_media)
        with self.assertRaisesRegex(
                bundle.BundleError, "must not be a symlink"):
            bundle.build_bundle(spec, self.output)

        media.unlink()
        media.hardlink_to(self.root / "firmware.bin")
        with self.assertRaisesRegex(bundle.BundleError, "same file"):
            bundle.build_bundle(spec, self.output)


if __name__ == "__main__":
    unittest.main()
