#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from pathlib import Path
import contextlib
import importlib.util
import io
import json
import tempfile
import unittest


MODULE_PATH = Path(__file__).with_name("boot_trace.py")
SPEC = importlib.util.spec_from_file_location("boot_trace", MODULE_PATH)
assert SPEC and SPEC.loader
boot_trace = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(boot_trace)


def header(producer="hardware"):
    return {
        "record": "header",
        "schema": "qemu.raspi.boot-trace",
        "version": 1,
        "platform": "raspi4b",
        "producer": producer,
        "capture_id": "capture-1",
        "started_utc": "2026-07-25T12:00:00Z",
        "artifacts": {
            "board_revision": "0xb03115",
            "eeprom_release": "2026-05-11",
            "eeprom_sha256": "a" * 64,
            "media_sha256": "b" * 64,
        },
    }


def events():
    return [
        {
            "record": "event",
            "seq": 0,
            "time_us": 10,
            "phase": "reset",
            "event": "reset.released",
            "outcome": "success",
            "data": {"serial": "physical-only", "cause": "power-on"},
        },
        {
            "record": "event",
            "seq": 1,
            "time_us": 20,
            "phase": "boot-source",
            "event": "boot-source.attempt",
            "source": "sd-card",
            "outcome": "start",
        },
    ]


class BootTraceTests(unittest.TestCase):
    def write_trace(self, root, name, records):
        path = Path(root) / name
        path.write_text(
            "".join(json.dumps(record) + "\n" for record in records),
            encoding="utf-8",
        )
        return path

    def test_valid_ordered_trace_loads(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_trace(
                directory, "trace.jsonl", [header(), *events()]
            )
            records = boot_trace.load_trace(path)
            self.assertEqual(len(records), 3)

    def test_cm4_emmc_trace_loads(self):
        with tempfile.TemporaryDirectory() as directory:
            cm4_header = header()
            cm4_header["platform"] = "cm4"
            cm4_header["artifacts"]["board_revision"] = "0xb03140"
            cm4_events = events()
            cm4_events[1]["source"] = "emmc"
            path = self.write_trace(
                directory, "cm4.jsonl", [cm4_header, *cm4_events]
            )
            records = boot_trace.load_trace(path)
            self.assertEqual(records[2]["source"], "emmc")

    def test_sequence_gap_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            broken = events()
            broken[1]["seq"] = 2
            path = self.write_trace(
                directory, "trace.jsonl", [header(), *broken]
            )
            with self.assertRaisesRegex(
                boot_trace.TraceError, "expected seq 1"
            ):
                boot_trace.load_trace(path)

    def test_unknown_record_field_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            broken = events()
            broken[0]["accidental"] = True
            path = self.write_trace(
                directory, "trace.jsonl", [header(), *broken]
            )
            with self.assertRaisesRegex(
                boot_trace.TraceError, "unknown accidental"
            ):
                boot_trace.load_trace(path)

    def test_bad_digest_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            broken_header = header()
            broken_header["artifacts"]["media_sha256"] = "NOT-A-DIGEST"
            path = self.write_trace(directory, "trace.jsonl", [broken_header])
            with self.assertRaisesRegex(boot_trace.TraceError, "media_sha256"):
                boot_trace.load_trace(path)

    def test_hardware_and_qemu_traces_ignore_only_volatile_fields(self):
        expected = [header("hardware"), *events()]
        actual = [header("qemu"), *events()]
        actual[0]["capture_id"] = "qemu-run-7"
        actual[0]["started_utc"] = "2026-07-26T00:00:00Z"
        actual[1]["time_us"] = 9000
        actual[1]["data"]["serial"] = "emulated"
        self.assertEqual(boot_trace.compare_traces(expected, actual), [])

    def test_stable_payload_mismatch_is_reported(self):
        expected = [header(), *events()]
        actual = [header("qemu"), *events()]
        actual[1]["data"]["cause"] = "watchdog"
        differences = boot_trace.compare_traces(expected, actual)
        self.assertEqual(len(differences), 1)
        self.assertIn("event seq 0", differences[0])
        self.assertIn("watchdog", differences[0])

    def test_cli_compare_exit_status(self):
        with tempfile.TemporaryDirectory() as directory:
            expected = self.write_trace(
                directory, "hardware.jsonl", [header(), *events()]
            )
            actual_records = [header("qemu"), *events()]
            actual_records[2]["outcome"] = "failure"
            actual = self.write_trace(directory, "qemu.jsonl", actual_records)
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                status = boot_trace.main(
                    ["compare", str(expected), str(actual)]
                )
            self.assertEqual(status, 1)
            self.assertIn('"equal": false', output.getvalue())

    def test_qemu_trace_conversion_produces_valid_contract(self):
        lines = [
            "unrelated QEMU output",
            "raspi4b_boot_event phase=reset event=reset.released "
            "source=none outcome=success value=0x00000000",
            "raspi4b_boot_event phase=boot-source "
            "event=boot-source.attempt source=sd-card outcome=success "
            "value=0x0000000000000f41",
            "raspi4b_boot_event phase=recovery event=eeprom.program "
            "source=sd-card outcome=restart value=0x00080000",
        ]
        records = boot_trace.convert_qemu_trace(
            lines,
            platform="raspi4b",
            board_revision="0xb03115",
            eeprom_sha256="a" * 64,
            otp_sha256="b" * 64,
        )
        self.assertEqual(records[1]["seq"], 0)
        self.assertNotIn("source", records[1])
        self.assertEqual(records[2]["source"], "sd-card")
        self.assertEqual(records[2]["data"]["value"], "0xf41")
        self.assertEqual(records[3]["phase"], "recovery")
        self.assertEqual(records[3]["outcome"], "restart")
        self.assertEqual(records[0]["artifacts"]["otp_sha256"], "b" * 64)

    def test_qemu_wait_retry_and_watchdog_outcomes(self):
        records = boot_trace.convert_qemu_trace(
            [
                "raspi4b_boot_event phase=boot-source "
                "event=boot-source.attempt source=rpiboot outcome=wait "
                "value=0x0000000000000001",
                "raspi4b_boot_event phase=boot-source "
                "event=boot-source.attempt source=sd-card outcome=retry "
                "value=0xffffffffffffffff",
                "raspi4b_boot_event phase=boot-source "
                "event=boot-source.restart source=restart outcome=watchdog "
                "value=0x0000000000000003",
            ],
            platform="raspi4b",
            board_revision="0xb03115",
        )
        self.assertEqual(
            [record["outcome"] for record in records[1:]],
            ["wait", "retry", "watchdog"],
        )
        self.assertEqual(records[2]["data"]["value"], "0xffffffffffffffff")

    def test_reserved_qemu_source_is_preserved_as_data(self):
        records = boot_trace.convert_qemu_trace(
            [
                "raspi4b_boot_event phase=boot-source "
                "event=boot-source.attempt source=reserved-0x8 "
                "outcome=unsupported value=0x00000008"
            ],
            platform="raspi4b",
            board_revision="0xb03115",
        )
        self.assertNotIn("source", records[1])
        self.assertEqual(records[1]["data"]["requested_source"], "reserved-0x8")

    def test_firmware_handoff_and_health_events_are_preserved(self):
        records = boot_trace.convert_qemu_trace(
            [
                "raspi4b_boot_event phase=firmware "
                "event=firmware.kernel source=sd-card outcome=success "
                "value=0x0000000000800000",
                "raspi4b_boot_event phase=handoff event=handoff.arm64 "
                "source=sd-card outcome=success value=0x0000000000200000",
                "raspi4b_boot_event phase=health "
                "event=health.kernel-started source=sd-card "
                "outcome=success value=0x0000000000000001",
                "raspi4b_boot_event phase=health "
                "event=health.userspace-ready source=sd-card "
                "outcome=success value=0x0000000000000002",
            ],
            platform="raspi4b",
            board_revision="0xb03115",
        )
        self.assertEqual(
            [record["phase"] for record in records[1:]],
            ["firmware", "handoff", "health", "health"],
        )
        self.assertEqual(records[1]["data"]["value"], "0x800000")
        self.assertEqual(records[4]["event"], "health.userspace-ready")


if __name__ == "__main__":
    unittest.main()
