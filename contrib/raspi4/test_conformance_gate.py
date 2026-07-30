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
from unittest import mock


ROOT = Path(__file__).parent
sys.path.insert(0, str(ROOT))
MODULE_PATH = ROOT / "conformance_gate.py"
SPEC = importlib.util.spec_from_file_location("conformance_gate", MODULE_PATH)
assert SPEC and SPEC.loader
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)
hil_orchestrator = gate.hil_orchestrator


class ConformanceGateTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.artifact = self.root / "media.img"
        self.artifact.write_bytes(b"exact production bytes")
        self.digest = hashlib.sha256(self.artifact.read_bytes()).hexdigest()

    def tearDown(self):
        self.temporary.cleanup()

    def trace(self, producer, outcome="success"):
        return [
            {
                "record": "header",
                "schema": "qemu.raspi.boot-trace",
                "version": 1,
                "platform": "raspi4b",
                "producer": producer,
                "artifacts": {
                    "board_revision": "0xb03115",
                    "media_sha256": self.digest,
                },
            },
            {
                "record": "event",
                "seq": 0,
                "phase": "handoff",
                "event": "handoff.arm",
                "outcome": outcome,
                "data": {"serial": producer, "kernel_size": 4096},
            },
        ]

    def write_trace(self, name, records):
        path = self.root / name
        path.write_text("".join(json.dumps(x) + "\n" for x in records),
                        encoding="utf-8")
        return path

    def manifest(self, **changes):
        case = {
            "id": "pi4-sd",
            "platform": "raspi4b",
            "board_revision": "0xb03115",
            "artifacts": {
                "media": {"path": "media.img", "sha256": self.digest}
            },
            "event_contract": [
                {"phase": "handoff", "event": "handoff.arm"}
            ],
            "hardware": {"trace": "hardware.jsonl"},
            "qemu": {"trace": "qemu.jsonl"},
        }
        case.update(changes)
        if "hil_plan" in case["hardware"]:
            case.setdefault(
                "flash_target", "/dev/disk/by-id/mock-rpi-media"
            )
        value = {
            "schema": gate.SCHEMA,
            "version": gate.VERSION,
            "cases": [case],
        }
        path = self.root / "manifest.json"
        path.write_text(json.dumps(value), encoding="utf-8")
        return path

    def hil_plan(self):
        artifacts = {}
        for name in ("eeprom", "firmware", "media"):
            path = self.root / f"{name}.bin"
            path.write_bytes(f"exact {name} bytes".encode())
            artifacts[name] = {
                "path": path.name,
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            }
        trace = [
            {
                "record": "header",
                "schema": "qemu.raspi.boot-trace",
                "version": 1,
                "platform": "raspi4b",
                "producer": "hardware",
                "artifacts": {
                    "board_revision": "0xb03115",
                    **{
                        f"{name}_sha256": value["sha256"]
                        for name, value in artifacts.items()
                    },
                },
            },
            {
                "record": "event",
                "seq": 0,
                "phase": "handoff",
                "event": "handoff.arm",
                "outcome": "success",
            },
        ]
        trace_text = "".join(json.dumps(item) + "\n" for item in trace)
        media = self.root / "media.bin"
        media_size = media.stat().st_size
        media_digest = artifacts["media"]["sha256"]
        capacity = max(4096, media_size)
        attestation = {
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
            "bytes_written": media_size,
            "source_sha256": media_digest,
            "verified": True,
            "resumed": False,
            "resume_from": 0,
            "resume_journal": "/var/tmp/mock-sdz.rpi-resume.json",
            "target_capacity": capacity,
            "target_sha256": media_digest,
        }
        steps = []
        for name in hil_orchestrator.SEQUENCES["raspi4b"]:
            script = (
                "from pathlib import Path; "
                "p=Path('hil-events'); "
                "p.write_text((p.read_text() if p.exists() else '')"
                f"+'{name}\\n');"
            )
            if name == "capture-uart":
                script += f"print({trace_text!r}, end='')"
            if name in hil_orchestrator.FLASH_STEPS:
                script += (
                    f"print({json.dumps(attestation)!r}, end='')"
                )
            steps.append({
                "name": name,
                "command": [sys.executable, "-c", script],
                "capture_stdout": name == "capture-uart",
                "capture_flash_attestation": (
                    name in hil_orchestrator.FLASH_STEPS
                ),
            })
        steps[1]["command"].extend(
            ["{artifact:media}", "{flash-target}"]
        )
        steps[3]["command"].extend(
            ["{artifact:eeprom}", "{artifact:firmware}"]
        )
        cleanup = [{
            "name": "power-off-cleanup",
            "command": [
                sys.executable,
                "-c",
                "from pathlib import Path; p=Path('hil-events'); "
                "p.write_text(p.read_text()+'power-off-cleanup\\n')",
            ],
            "capture_stdout": False,
            "capture_flash_attestation": False,
        }]
        plan = {
            "schema": hil_orchestrator.SCHEMA,
            "version": hil_orchestrator.VERSION,
            "id": "pi4-release",
            "platform": "raspi4b",
            "board_revision": "0xb03115",
            "flash_target": "/dev/disk/by-id/mock-rpi-media",
            "artifacts": artifacts,
            "trace": "hardware.jsonl",
            "steps": steps,
            "cleanup": cleanup,
        }
        path = self.root / "hil-plan.json"
        path.write_text(json.dumps(plan), encoding="utf-8")
        return path, artifacts, trace

    def test_matching_pinned_captures_pass(self):
        self.write_trace("hardware.jsonl", self.trace("hardware"))
        self.write_trace("qemu.jsonl", self.trace("qemu"))
        result = gate.run_manifest(self.manifest())
        self.assertTrue(result["equal"])

    def test_semantic_drift_fails(self):
        self.write_trace("hardware.jsonl", self.trace("hardware"))
        self.write_trace("qemu.jsonl", self.trace("qemu", "failure"))
        result = gate.run_manifest(self.manifest())
        self.assertFalse(result["equal"])
        self.assertIn("event seq 0", result["cases"][0]["differences"][0])

    def test_matching_truncated_traces_fail_hardware_contract(self):
        hardware = self.trace("hardware")[:1]
        qemu = self.trace("qemu")[:1]
        self.write_trace("hardware.jsonl", hardware)
        self.write_trace("qemu.jsonl", qemu)
        with self.assertRaisesRegex(
            gate.GateError, "hardware event contract.*event count"
        ):
            gate.run_manifest(self.manifest())

    def test_truncated_qemu_trace_is_behavioral_drift(self):
        self.write_trace("hardware.jsonl", self.trace("hardware"))
        self.write_trace("qemu.jsonl", self.trace("qemu")[:1])
        result = gate.run_manifest(self.manifest())
        self.assertFalse(result["equal"])
        self.assertIn(
            "event contract: event count",
            result["cases"][0]["differences"][0],
        )

    def test_event_contract_requires_order_and_selected_fields(self):
        hardware = self.trace("hardware")
        hardware.append({
            "record": "event",
            "seq": 1,
            "phase": "health",
            "event": "health.application-ready",
            "outcome": "success",
        })
        qemu = [
            dict(hardware[0], producer="qemu"),
            dict(hardware[2], seq=0),
            dict(hardware[1], seq=1),
        ]
        self.write_trace("hardware.jsonl", hardware)
        self.write_trace("qemu.jsonl", qemu)
        manifest = self.manifest(event_contract=[
            {
                "phase": "handoff",
                "event": "handoff.arm",
                "outcome": "success",
            },
            {
                "phase": "health",
                "event": "health.application-ready",
                "outcome": "success",
            },
        ])
        result = gate.run_manifest(manifest)
        self.assertFalse(result["equal"])
        self.assertIn(
            "event contract: event 0",
            result["cases"][0]["differences"][0],
        )

    def test_missing_or_invalid_event_contract_fails_preflight(self):
        self.write_trace("hardware.jsonl", self.trace("hardware"))
        self.write_trace("qemu.jsonl", self.trace("qemu"))
        with self.assertRaisesRegex(
            gate.GateError, "event_contract must be a non-empty array"
        ):
            gate.run_manifest(self.manifest(event_contract=None))
        with self.assertRaisesRegex(
            gate.GateError, "event_contract\\[0\\] requires phase and event"
        ):
            gate.run_manifest(self.manifest(event_contract=[
                {"phase": "handoff"}
            ]))

    def test_artifact_hash_mismatch_fails_closed(self):
        self.write_trace("hardware.jsonl", self.trace("hardware"))
        self.write_trace("qemu.jsonl", self.trace("qemu"))
        manifest = self.manifest()
        self.artifact.write_bytes(b"tampered")
        with self.assertRaisesRegex(gate.GateError, "SHA-256 mismatch"):
            gate.run_manifest(manifest)

    def test_wrong_trace_producer_is_rejected(self):
        self.write_trace("hardware.jsonl", self.trace("qemu"))
        self.write_trace("qemu.jsonl", self.trace("qemu"))
        with self.assertRaisesRegex(gate.GateError, "producer must"):
            gate.run_manifest(self.manifest())

    def test_capture_command_replaces_stale_trace(self):
        records = self.trace("qemu")
        self.write_trace("hardware.jsonl", self.trace("hardware"))
        self.write_trace("qemu.jsonl", self.trace("qemu", "failure"))
        script = (
            "from pathlib import Path; import json; "
            "Path('qemu.jsonl').write_text(" + repr(
                "".join(json.dumps(x) + "\n" for x in records)
            ) + ")"
        )
        manifest = self.manifest(qemu={
            "trace": "qemu.jsonl",
            "command": [sys.executable, "-c", script],
            "timeout_s": 10,
        })
        self.assertTrue(gate.run_manifest(manifest)["equal"])

    def test_capture_stdout_writes_trace_without_a_shell(self):
        records = self.trace("qemu")
        self.write_trace("hardware.jsonl", self.trace("hardware"))
        script = "print(" + repr(
            "".join(json.dumps(x) + "\n" for x in records)
        ) + ", end='')"
        manifest = self.manifest(qemu={
            "trace": "qemu.jsonl",
            "command": [sys.executable, "-c", script],
            "capture_stdout": True,
        })
        self.assertTrue(gate.run_manifest(manifest)["equal"])

    def test_unknown_manifest_fields_are_rejected(self):
        self.write_trace("hardware.jsonl", self.trace("hardware"))
        self.write_trace("qemu.jsonl", self.trace("qemu"))
        with self.assertRaisesRegex(gate.GateError, "unknown accidental"):
            gate.run_manifest(self.manifest(accidental=True))

    def test_hil_plan_runs_inside_conformance_gate(self):
        plan, artifacts, trace = self.hil_plan()
        qemu_trace = [dict(item) for item in trace]
        qemu_trace[0] = dict(qemu_trace[0], producer="qemu")
        self.write_trace("qemu.jsonl", qemu_trace)
        result = gate.run_manifest(self.manifest(
            artifacts=artifacts,
            hardware={
                "trace": "hardware.jsonl",
                "hil_plan": plan.name,
                "hil_report": "hil-report.json",
            },
        ))
        self.assertTrue(result["equal"])
        self.assertTrue(
            json.loads((self.root / "hil-report.json").read_text())["success"]
        )
        self.assertEqual(
            (self.root / "hil-events").read_text().splitlines(),
            list(hil_orchestrator.SEQUENCES["raspi4b"])
            + list(hil_orchestrator.CLEANUP_SEQUENCES["raspi4b"]),
        )

    def test_hil_case_mismatch_prevents_fixture_commands(self):
        plan, artifacts, trace = self.hil_plan()
        qemu_trace = [dict(item) for item in trace]
        qemu_trace[0] = dict(qemu_trace[0], producer="qemu")
        self.write_trace("qemu.jsonl", qemu_trace)
        manifest = self.manifest(
            board_revision="0xa03111",
            artifacts=artifacts,
            hardware={
                "trace": "hardware.jsonl",
                "hil_plan": plan.name,
                "hil_report": "hil-report.json",
            },
        )
        with self.assertRaisesRegex(gate.GateError, "board_revision"):
            gate.run_manifest(manifest)
        self.assertFalse((self.root / "hil-events").exists())

    def test_hil_flash_target_mismatch_prevents_fixture_commands(self):
        plan, artifacts, trace = self.hil_plan()
        qemu_trace = [dict(item) for item in trace]
        qemu_trace[0] = dict(qemu_trace[0], producer="qemu")
        self.write_trace("qemu.jsonl", qemu_trace)
        manifest = self.manifest(
            flash_target="/dev/disk/by-id/other-rpi-media",
            artifacts=artifacts,
            hardware={
                "trace": "hardware.jsonl",
                "hil_plan": plan.name,
                "hil_report": "hil-report.json",
            },
        )
        with self.assertRaisesRegex(gate.GateError, "flash_target"):
            gate.run_manifest(manifest)
        self.assertFalse((self.root / "hil-events").exists())

    def test_hil_qemu_trace_collision_prevents_fixture_commands(self):
        plan, artifacts, _ = self.hil_plan()
        manifest = self.manifest(
            artifacts=artifacts,
            hardware={
                "trace": "hardware.jsonl",
                "hil_plan": plan.name,
                "hil_report": "hil-report.json",
            },
            qemu={"trace": "media.bin"},
        )
        with self.assertRaisesRegex(
            gate.GateError, "path collision.*QEMU trace.*artifact media"
        ):
            gate.run_manifest(manifest)
        self.assertFalse((self.root / "hil-events").exists())

        qemu_target = self.write_trace("qemu-target.jsonl", self.trace("qemu"))
        qemu_link = self.root / "qemu-link.jsonl"
        qemu_link.symlink_to(qemu_target)
        manifest = self.manifest(
            artifacts=artifacts,
            hardware={
                "trace": "hardware.jsonl",
                "hil_plan": plan.name,
                "hil_report": "hil-report.json",
            },
            qemu={"trace": qemu_link.name},
        )
        with self.assertRaisesRegex(
            gate.GateError, "QEMU trace path must not be a symlink"
        ):
            gate.run_manifest(manifest)
        self.assertFalse((self.root / "hil-events").exists())

    def test_tampered_retained_hil_report_is_rejected(self):
        plan, artifacts, trace = self.hil_plan()
        qemu_trace = [dict(item) for item in trace]
        qemu_trace[0] = dict(qemu_trace[0], producer="qemu")
        self.write_trace("qemu.jsonl", qemu_trace)
        real_run_plan = hil_orchestrator.run_plan

        def run_then_tamper(plan_path, report_path):
            result = real_run_plan(plan_path, report_path)
            report_path.write_text("{}\n", encoding="utf-8")
            return result

        manifest = self.manifest(
            artifacts=artifacts,
            hardware={
                "trace": "hardware.jsonl",
                "hil_plan": plan.name,
                "hil_report": "hil-report.json",
            },
        )
        with mock.patch.object(
            hil_orchestrator, "run_plan", side_effect=run_then_tamper
        ):
            with self.assertRaisesRegex(
                gate.GateError, "retained HIL report differs"
            ):
                gate.run_manifest(manifest)

    def test_tampered_flash_attestation_is_revalidated(self):
        plan, artifacts, trace = self.hil_plan()
        qemu_trace = [dict(item) for item in trace]
        qemu_trace[0] = dict(qemu_trace[0], producer="qemu")
        self.write_trace("qemu.jsonl", qemu_trace)
        real_run_plan = hil_orchestrator.run_plan

        def run_then_tamper(plan_path, report_path):
            result = real_run_plan(plan_path, report_path)
            result["steps"][1]["flash_attestation"]["target_sha256"] = (
                "0" * 64
            )
            report_path.write_text(
                json.dumps(result) + "\n", encoding="utf-8"
            )
            return result

        manifest = self.manifest(
            artifacts=artifacts,
            hardware={
                "trace": "hardware.jsonl",
                "hil_plan": plan.name,
                "hil_report": "hil-report.json",
            },
        )
        with mock.patch.object(
            hil_orchestrator, "run_plan", side_effect=run_then_tamper
        ):
            with self.assertRaisesRegex(
                gate.GateError, "flash evidence mismatch"
            ):
                gate.run_manifest(manifest)

    def test_entire_batch_preflights_before_hil_commands(self):
        plan, artifacts, trace = self.hil_plan()
        qemu_trace = [dict(item) for item in trace]
        qemu_trace[0] = dict(qemu_trace[0], producer="qemu")
        self.write_trace("qemu.jsonl", qemu_trace)
        manifest = self.manifest(
            artifacts=artifacts,
            hardware={
                "trace": "hardware.jsonl",
                "hil_plan": plan.name,
                "hil_report": "hil-report.json",
            },
        )
        value = json.loads(manifest.read_text())
        value["cases"].append(dict(value["cases"][0]))
        manifest.write_text(json.dumps(value), encoding="utf-8")
        with self.assertRaisesRegex(gate.GateError, "duplicate case id"):
            gate.run_manifest(manifest)
        self.assertFalse((self.root / "hil-events").exists())


if __name__ == "__main__":
    unittest.main()
