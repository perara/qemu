#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare Raspberry Pi network-boot protocol sequence and packet cadence."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
from typing import Iterable


MAX_CAPTURE_SIZE = 1024 * 1024 * 1024
MAX_PACKETS = 1_000_000
MAX_CAPTURED_PACKET = 65535


class CadenceError(RuntimeError):
    pass


@dataclass(frozen=True)
class Event:
    timestamp_ns: int
    direction: str
    protocol: str
    action: str
    detail: str = ""

    @property
    def signature(self) -> tuple[str, str, str, str]:
        return self.direction, self.protocol, self.action, self.detail


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def parse_mac(text: str) -> bytes:
    try:
        value = bytes.fromhex(text.replace(":", "").replace("-", ""))
    except ValueError as error:
        raise CadenceError(f"invalid MAC address {text!r}") from error
    if len(value) != 6:
        raise CadenceError(f"invalid MAC address {text!r}")
    return value


def read_pcap(path: Path) -> tuple[list[tuple[int, bytes]], str]:
    if not path.is_file() or path.is_symlink():
        raise CadenceError(f"capture is not a regular non-symlink file: {path}")
    size = path.stat().st_size
    if size > MAX_CAPTURE_SIZE:
        raise CadenceError(f"capture exceeds {MAX_CAPTURE_SIZE} bytes: {path}")
    data = path.read_bytes()
    if len(data) < 24:
        raise CadenceError(f"truncated PCAP global header: {path}")
    formats = {
        b"\xd4\xc3\xb2\xa1": ("<", 1000),
        b"\xa1\xb2\xc3\xd4": (">", 1000),
        b"\x4d\x3c\xb2\xa1": ("<", 1),
        b"\xa1\xb2\x3c\x4d": (">", 1),
    }
    try:
        endian, fraction_scale = formats[data[:4]]
    except KeyError as error:
        raise CadenceError(f"unsupported PCAP magic in {path}") from error
    version_major, version_minor, _zone, _sigfigs, snaplen, linktype = \
        struct.unpack_from(endian + "HHIIII", data, 4)
    if (version_major, version_minor) != (2, 4):
        raise CadenceError(f"unsupported PCAP version in {path}")
    if linktype != 1:
        raise CadenceError(f"PCAP link type must be Ethernet in {path}")
    if not snaplen or snaplen > MAX_CAPTURED_PACKET:
        raise CadenceError(f"invalid PCAP snaplen in {path}")

    packets: list[tuple[int, bytes]] = []
    offset = 24
    previous_timestamp = -1
    while offset < len(data):
        if len(data) - offset < 16:
            raise CadenceError(f"truncated PCAP packet header in {path}")
        seconds, fraction, captured, original = struct.unpack_from(
            endian + "IIII", data, offset)
        offset += 16
        if fraction >= 1_000_000_000 // fraction_scale:
            raise CadenceError(f"invalid PCAP timestamp fraction in {path}")
        if (captured > snaplen or captured > original or
                captured > len(data) - offset):
            raise CadenceError(f"invalid or truncated PCAP packet in {path}")
        timestamp = seconds * 1_000_000_000 + fraction * fraction_scale
        if timestamp < previous_timestamp:
            raise CadenceError(f"PCAP timestamps are not monotonic in {path}")
        previous_timestamp = timestamp
        packets.append((timestamp, data[offset:offset + captured]))
        if len(packets) > MAX_PACKETS:
            raise CadenceError(f"capture exceeds {MAX_PACKETS} packets: {path}")
        offset += captured
    return packets, sha256_bytes(data)


def dhcp_message(payload: bytes) -> str | None:
    if len(payload) < 240 or payload[236:240] != b"\x63\x82\x53\x63":
        return None
    names = {1: "discover", 2: "offer", 3: "request", 5: "ack", 6: "nak"}
    cursor = 240
    while cursor < len(payload):
        code = payload[cursor]
        cursor += 1
        if code == 0:
            continue
        if code == 255:
            break
        if cursor >= len(payload):
            return None
        length = payload[cursor]
        cursor += 1
        if length > len(payload) - cursor:
            return None
        if code == 53 and length == 1:
            return names.get(payload[cursor], f"type-{payload[cursor]}")
        cursor += length
    return None


def dns_name(payload: bytes) -> str:
    if len(payload) < 13:
        return ""
    labels: list[str] = []
    cursor = 12
    while cursor < len(payload) and payload[cursor]:
        length = payload[cursor]
        cursor += 1
        if length > 63 or length > len(payload) - cursor:
            return ""
        label = payload[cursor:cursor + length]
        if any(byte < 0x21 or byte > 0x7e for byte in label):
            return ""
        labels.append(label.decode("ascii"))
        cursor += length
    return ".".join(labels)


class PacketClassifier:
    def __init__(self, client_mac: bytes):
        self.client_mac = client_mac
        self.tftp_client_port: int | None = None
        self.tftp_server_port: int | None = None

    def classify(self, timestamp: int, packet: bytes) -> Event | None:
        if len(packet) < 14:
            return None
        source = packet[6:12]
        direction = "tx" if source == self.client_mac else "rx"
        ethertype = struct.unpack_from("!H", packet, 12)[0]
        offset = 14
        if ethertype == 0x8100 and len(packet) >= 18:
            ethertype = struct.unpack_from("!H", packet, 16)[0]
            offset = 18
        if ethertype == 0x0806:
            if len(packet) < offset + 28:
                return None
            operation = struct.unpack_from("!H", packet, offset + 6)[0]
            target = ".".join(
                str(byte) for byte in packet[offset + 24:offset + 28])
            action = {1: "request", 2: "reply"}.get(operation)
            return (Event(timestamp, direction, "arp", action, target)
                    if action else None)
        if ethertype != 0x0800 or len(packet) < offset + 20:
            return None
        ip = packet[offset:]
        header_length = (ip[0] & 0x0f) * 4
        total_length = struct.unpack_from("!H", ip, 2)[0]
        fragment = struct.unpack_from("!H", ip, 6)[0]
        if ip[0] >> 4 != 4 or header_length < 20 or ip[9] != 17 or \
                fragment & 0x3fff or \
                total_length < header_length + 8 or total_length > len(ip):
            return None
        udp = ip[header_length:total_length]
        source_port, destination_port, udp_length = struct.unpack_from(
            "!HHH", udp)
        if udp_length < 8 or udp_length > len(udp):
            return None
        payload = udp[8:udp_length]
        if {source_port, destination_port} == {67, 68}:
            message = dhcp_message(payload)
            return (Event(timestamp, direction, "dhcp", message)
                    if message else None)
        if source_port == 53 or destination_port == 53:
            if len(payload) < 12:
                return None
            response = bool(struct.unpack_from("!H", payload, 2)[0] & 0x8000)
            return Event(timestamp, direction, "dns",
                         "response" if response else "query",
                         "" if response else dns_name(payload))
        if len(payload) < 2:
            return None
        opcode = struct.unpack_from("!H", payload)[0]
        is_tftp = False
        if destination_port == 69 and opcode == 1:
            self.tftp_client_port = source_port
            self.tftp_server_port = None
            is_tftp = True
        elif (self.tftp_client_port == destination_port and
              opcode in (3, 5, 6)):
            self.tftp_server_port = source_port
            is_tftp = True
        elif self.tftp_server_port == destination_port and opcode == 4:
            is_tftp = True
        if not is_tftp:
            return None
        if opcode == 1:
            end = payload.find(b"\0", 2)
            detail = (payload[2:end].decode("ascii", "replace")
                      if end >= 2 else "")
            return Event(timestamp, direction, "tftp", "rrq", detail)
        if opcode in (3, 4) and len(payload) >= 4:
            block = struct.unpack_from("!H", payload, 2)[0]
            return Event(timestamp, direction, "tftp",
                         "data" if opcode == 3 else "ack", str(block))
        if opcode == 5 and len(payload) >= 4:
            return Event(timestamp, direction, "tftp", "error",
                         str(struct.unpack_from("!H", payload, 2)[0]))
        if opcode == 6:
            return Event(timestamp, direction, "tftp", "oack")
        return None


def normalize(packets: Iterable[tuple[int, bytes]],
              client_mac: bytes) -> list[Event]:
    classifier = PacketClassifier(client_mac)
    events = [event for timestamp, packet in packets
              if (event := classifier.classify(timestamp, packet)) is not None]
    if not events:
        raise CadenceError("capture contains no recognized network-boot events")
    origin = events[0].timestamp_ns
    return [Event(event.timestamp_ns - origin, event.direction, event.protocol,
                  event.action, event.detail) for event in events]


def compare(reference: list[Event], candidate: list[Event],
            absolute_tolerance_ms: float,
            relative_tolerance: float) -> dict[str, object]:
    sequence_mismatches: list[dict[str, object]] = []
    for index in range(max(len(reference), len(candidate))):
        expected = (reference[index].signature
                    if index < len(reference) else None)
        actual = candidate[index].signature if index < len(candidate) else None
        if expected != actual:
            sequence_mismatches.append({"index": index, "expected": expected,
                                        "actual": actual})
            if len(sequence_mismatches) == 100:
                break
    timing_mismatches: list[dict[str, object]] = []
    if not sequence_mismatches:
        absolute_ns = round(absolute_tolerance_ms * 1_000_000)
        for index in range(1, len(reference)):
            expected = (reference[index].timestamp_ns -
                        reference[index - 1].timestamp_ns)
            actual = (candidate[index].timestamp_ns -
                      candidate[index - 1].timestamp_ns)
            tolerance = max(absolute_ns, round(expected * relative_tolerance))
            if abs(actual - expected) > tolerance:
                timing_mismatches.append({
                    "index": index, "expected_delta_ns": expected,
                    "actual_delta_ns": actual, "tolerance_ns": tolerance,
                })
                if len(timing_mismatches) == 100:
                    break
    return {
        "match": not sequence_mismatches and not timing_mismatches,
        "reference_event_count": len(reference),
        "candidate_event_count": len(candidate),
        "sequence_mismatches": sequence_mismatches,
        "timing_mismatches": timing_mismatches,
    }


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", required=True, type=Path)
    parser.add_argument("--candidate", required=True, type=Path)
    parser.add_argument("--reference-client-mac", required=True)
    parser.add_argument("--candidate-client-mac")
    parser.add_argument("--absolute-tolerance-ms", type=float, default=25.0)
    parser.add_argument("--relative-tolerance", type=float, default=0.10)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    if not math.isfinite(args.absolute_tolerance_ms) or \
            args.absolute_tolerance_ms < 0:
        parser.error("--absolute-tolerance-ms must be nonnegative")
    if (not math.isfinite(args.relative_tolerance) or
            args.relative_tolerance < 0):
        parser.error("--relative-tolerance must be nonnegative")
    return args


def run(reference: Path, candidate: Path, reference_client_mac: str,
        candidate_client_mac: str, absolute_tolerance_ms: float,
        relative_tolerance: float, output: Path) -> dict[str, object]:
    if output.absolute() in {reference.absolute(), candidate.absolute()}:
        raise CadenceError("output must not overwrite an input capture")
    reference_packets, reference_hash = read_pcap(reference)
    candidate_packets, candidate_hash = read_pcap(candidate)
    result = compare(
        normalize(reference_packets, parse_mac(reference_client_mac)),
        normalize(candidate_packets, parse_mac(candidate_client_mac)),
        absolute_tolerance_ms, relative_tolerance)
    report = {
        "schema": "qemu-rpi-network-cadence-v1",
        "reference": {"path": str(reference), "sha256": reference_hash,
                      "client_mac": reference_client_mac},
        "candidate": {"path": str(candidate), "sha256": candidate_hash,
                      "client_mac": candidate_client_mac},
        "absolute_tolerance_ms": absolute_tolerance_ms,
        "relative_tolerance": relative_tolerance,
        **result,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + ".tmp")
    temporary.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(output)
    return report


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        report = run(
            args.reference, args.candidate, args.reference_client_mac,
            args.candidate_client_mac or args.reference_client_mac,
            args.absolute_tolerance_ms, args.relative_tolerance, args.output)
        print(json.dumps(report, sort_keys=True))
        return 0 if report["match"] else 1
    except (CadenceError, OSError) as error:
        print(f"network cadence: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
