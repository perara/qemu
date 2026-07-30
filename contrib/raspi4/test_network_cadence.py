#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

import importlib.util
import contextlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("network_cadence.py")
SPEC = importlib.util.spec_from_file_location("network_cadence", SCRIPT)
assert SPEC and SPEC.loader
network_cadence = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = network_cadence
SPEC.loader.exec_module(network_cadence)

CLIENT = bytes.fromhex("525400123456")
SERVER = bytes.fromhex("525400123402")


def udp_frame(source_mac, destination_mac, source_port, destination_port,
              payload):
    udp = struct.pack("!HHHH", source_port, destination_port,
                      8 + len(payload), 0) + payload
    ip = bytearray(20)
    ip[0] = 0x45
    struct.pack_into("!H", ip, 2, 20 + len(udp))
    ip[8] = 64
    ip[9] = 17
    return destination_mac + source_mac + b"\x08\x00" + bytes(ip) + udp


def dhcp(message):
    payload = bytearray(240)
    payload[236:240] = bytes.fromhex("63825363")
    payload.extend((53, 1, message, 255))
    return bytes(payload)


def arp(source_mac, destination_mac, operation, target):
    payload = struct.pack("!HHBBH", 1, 0x0800, 6, 4, operation)
    payload += source_mac + bytes((10, 0, 2, 15))
    payload += destination_mac + bytes(target)
    return destination_mac + source_mac + b"\x08\x06" + payload


def pcap(packets):
    data = bytearray(struct.pack("<IHHIIII", 0xA1B2C3D4, 2, 4, 0, 0,
                                 65535, 1))
    for timestamp_ns, packet in packets:
        seconds, remainder = divmod(timestamp_ns, 1_000_000_000)
        data.extend(struct.pack("<IIII", seconds, remainder // 1000,
                                len(packet), len(packet)))
        data.extend(packet)
    return bytes(data)


class NetworkCadenceTests(unittest.TestCase):
    def packet_sequence(self, offset=0):
        broadcast = b"\xff" * 6
        return [
            (offset + 0, udp_frame(CLIENT, broadcast, 68, 67, dhcp(1))),
            (offset + 1_000_000_000,
             udp_frame(SERVER, CLIENT, 67, 68, dhcp(2))),
            (offset + 1_100_000_000,
             arp(CLIENT, broadcast, 1, (10, 0, 2, 2))),
            (offset + 1_110_000_000,
             arp(SERVER, CLIENT, 2, (10, 0, 2, 15))),
            (offset + 1_120_000_000,
             udp_frame(CLIENT, SERVER, 49152, 69,
                       b"\0\1config.txt\0octet\0")),
            (offset + 1_130_000_000,
             udp_frame(SERVER, CLIENT, 1200, 49152, b"\0\3\0\1data")),
            (offset + 1_140_000_000,
             udp_frame(CLIENT, SERVER, 49152, 1200, b"\0\4\0\1")),
        ]

    def test_normalizes_boot_protocol_sequence(self):
        events = network_cadence.normalize(self.packet_sequence(), CLIENT)
        self.assertEqual([event.signature for event in events], [
            ("tx", "dhcp", "discover", ""),
            ("rx", "dhcp", "offer", ""),
            ("tx", "arp", "request", "10.0.2.2"),
            ("rx", "arp", "reply", "10.0.2.15"),
            ("tx", "tftp", "rrq", "config.txt"),
            ("rx", "tftp", "data", "1"),
            ("tx", "tftp", "ack", "1"),
        ])
        self.assertEqual(events[0].timestamp_ns, 0)

    def test_compare_reports_sequence_and_timing_drift(self):
        reference = network_cadence.normalize(self.packet_sequence(), CLIENT)
        close = network_cadence.normalize(
            self.packet_sequence(offset=10_000_000), CLIENT)
        self.assertTrue(network_cadence.compare(
            reference, close, 1, 0)["match"])
        delayed_packets = self.packet_sequence()
        timestamp, packet = delayed_packets[-1]
        delayed_packets[-1] = timestamp + 50_000_000, packet
        timing = network_cadence.compare(
            reference, network_cadence.normalize(delayed_packets, CLIENT),
            1, 0)
        self.assertFalse(timing["match"])
        self.assertTrue(timing["timing_mismatches"])
        sequence = network_cadence.compare(reference, reference[:-1], 1, 0)
        self.assertFalse(sequence["match"])
        self.assertTrue(sequence["sequence_mismatches"])

    def test_pcap_reader_hashes_and_rejects_invalid_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = root / "capture.pcap"
            capture.write_bytes(pcap(self.packet_sequence()))
            packets, digest = network_cadence.read_pcap(capture)
            self.assertEqual(len(packets), len(self.packet_sequence()))
            self.assertEqual(digest, network_cadence.sha256_bytes(
                capture.read_bytes()))
            capture.write_bytes(b"bad")
            with self.assertRaises(network_cadence.CadenceError):
                network_cadence.read_pcap(capture)

    def test_cli_writes_match_and_drift_reports(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            reference = root / "reference.pcap"
            candidate = root / "candidate.pcap"
            report = root / "report.json"
            reference.write_bytes(pcap(self.packet_sequence()))
            candidate.write_bytes(pcap(self.packet_sequence(offset=5_000_000)))
            arguments = [
                "--reference", str(reference), "--candidate", str(candidate),
                "--reference-client-mac", "52:54:00:12:34:56",
                "--absolute-tolerance-ms", "1", "--output", str(report),
            ]
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(network_cadence.main(arguments), 0)
            result = json.loads(report.read_text(encoding="utf-8"))
            self.assertTrue(result["match"])
            delayed = self.packet_sequence()
            timestamp, packet = delayed[-1]
            delayed[-1] = timestamp + 50_000_000, packet
            candidate.write_bytes(pcap(delayed))
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(network_cadence.main(arguments), 1)
            self.assertFalse(json.loads(
                report.read_text(encoding="utf-8"))["match"])


if __name__ == "__main__":
    unittest.main()
