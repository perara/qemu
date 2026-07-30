#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

import json
import os
from pathlib import Path
import pty
import select
import socket
import tempfile
import threading
import unittest

from gpio_proxy import (
    BridgeController,
    ConfigurationError,
    LibgpiodBackend,
    MockBackend,
    PinMapping,
    ProtocolError,
    ProxyConfig,
    ResponseParser,
    SafetyError,
    UsbSerialBackend,
    load_config,
    run_proxy,
)


class FakeLine:
    class Bias:
        AS_IS = "bias-as-is"
        DISABLED = "bias-disabled"
        PULL_UP = "bias-up"
        PULL_DOWN = "bias-down"

    class Drive:
        PUSH_PULL = "push-pull"
        OPEN_DRAIN = "open-drain"
        OPEN_SOURCE = "open-source"

    class Direction:
        INPUT = "input"
        OUTPUT = "output"

    class Edge:
        BOTH = "both"

    class Value:
        ACTIVE = 1
        INACTIVE = 0


class FakeSettings:
    def __init__(self, **kwargs):
        self.__dict__.update(kwargs)


class FakeEvent:
    def __init__(self, line_offset):
        self.line_offset = line_offset


class FakeRequest:
    next_fd = 100

    def __init__(self, config):
        self.config = dict(config)
        self.values = {line: FakeLine.Value.INACTIVE for line in config}
        self.events = []
        self.fd = FakeRequest.next_fd
        FakeRequest.next_fd += 1
        self.released = False

    def reconfigure_lines(self, config):
        self.config.update(config)
        for line, settings in config.items():
            if settings.direction == FakeLine.Direction.OUTPUT:
                self.values[line] = settings.output_value

    def set_value(self, line, value):
        self.values[line] = value

    def get_value(self, line):
        return self.values[line]

    def read_edge_events(self):
        events, self.events = self.events, []
        return events

    def release(self):
        self.released = True


class FakeGpiod:
    line = FakeLine
    LineSettings = FakeSettings
    requests = []

    @classmethod
    def is_gpiochip_device(cls, path):
        return path.startswith("/dev/gpiochip")

    @classmethod
    def request_lines(cls, path, consumer, config):
        request = FakeRequest(config)
        cls.requests.append((path, consumer, request))
        return request


class ConfigTest(unittest.TestCase):
    def write_config(self, value):
        directory = tempfile.TemporaryDirectory()
        path = Path(directory.name) / "gpio.json"
        path.write_text(json.dumps(value), encoding="utf-8")
        self.addCleanup(directory.cleanup)
        return path

    def base(self):
        return {
            "version": 1,
            "consumer": "qemu-rpi-test",
            "pins": [
                {"virtual": 5, "chip": "/dev/gpiochip-test", "line": 3},
                {
                    "virtual": 6,
                    "chip": "/dev/gpiochip-test",
                    "line": 4,
                    "allow_output": True,
                    "active_low": True,
                    "bias": "pull-up",
                    "drive": "open-drain",
                    "debounce_us": 100,
                },
            ],
        }

    def test_valid_and_fail_closed_defaults(self):
        config = load_config(self.write_config(self.base()))
        self.assertEqual(config.consumer, "qemu-rpi-test")
        self.assertEqual(len(config.pins), 2)
        self.assertFalse(config.pins[0].allow_output)
        self.assertEqual(config.pins[0].bias, "as-is")
        self.assertTrue(config.pins[1].allow_output)
        self.assertEqual(config.pins[1].drive, "open-drain")

    def test_accepts_explicit_usb_adapter_lines(self):
        value = self.base()
        for pin in value["pins"]:
            pin["chip"] = "usb"
        config = load_config(self.write_config(value))
        self.assertEqual([pin.chip for pin in config.pins], ["usb", "usb"])

    def test_accepts_dedicated_eeprom_nwp_signal(self):
        value = self.base()
        value["signals"] = [
            {
                "name": "EEPROM_NWP",
                "chip": "usb",
                "line": 7,
                "bias": "pull-up",
            }
        ]
        config = load_config(self.write_config(value))
        signal = config.pins[-1]
        self.assertEqual(signal.virtual, "EEPROM_NWP")
        self.assertEqual(signal.line, 7)
        self.assertFalse(signal.allow_output)

    def test_accepts_dedicated_sd_overcurrent_signal(self):
        value = self.base()
        value["signals"] = [
            {
                "name": "SD_OVERCURRENT",
                "chip": "usb",
                "line": 8,
                "bias": "pull-down",
            }
        ]
        config = load_config(self.write_config(value))
        signal = config.pins[-1]
        self.assertEqual(signal.virtual, "SD_OVERCURRENT")
        self.assertEqual(signal.line, 8)
        self.assertFalse(signal.allow_output)

    def test_rejects_unknown_and_missing_fields(self):
        value = self.base()
        value["typo"] = True
        with self.assertRaises(ConfigurationError):
            load_config(self.write_config(value))
        value = self.base()
        del value["pins"][0]["line"]
        with self.assertRaises(ConfigurationError):
            load_config(self.write_config(value))

    def test_rejects_duplicate_and_unsafe_maps(self):
        cases = []
        value = self.base()
        value["pins"][1]["virtual"] = 5
        cases.append(value)
        value = self.base()
        value["pins"][1]["line"] = 3
        cases.append(value)
        value = self.base()
        value["pins"][0]["virtual"] = 58
        cases.append(value)
        value = self.base()
        value["pins"][0]["chip"] = "gpiochip0"
        cases.append(value)
        value = self.base()
        value["pins"][0]["drive"] = "open-drain"
        cases.append(value)
        value = self.base()
        value["pins"][0]["allow_output"] = 1
        cases.append(value)
        for case in cases:
            with self.subTest(case=case), self.assertRaises(ConfigurationError):
                load_config(self.write_config(case))


class ParserTest(unittest.TestCase):
    def test_fragmented_and_batched_responses(self):
        parser = ResponseParser()
        self.assertEqual(parser.feed(b"RPI-GP"), [])
        events = parser.feed(
            b"IO 1 58\r\nPIN 5 1 0\nPIN 57 0 1\n"
            b"SIGNAL EEPROM_NWP Z\nSIGNAL EEPROM_NWP 0\n"
            b"SIGNAL SD_OVERCURRENT 1\n"
            b"END\nPONG 1\nOK\n"
        )
        self.assertEqual(events[0], ("hello", (1, 58)))
        self.assertIn(("pin", (5, 1, 0)), events)
        self.assertIn(("pin", (57, 0, 1)), events)
        self.assertIn(("signal", ("EEPROM_NWP", -1, 0)), events)
        self.assertIn(("signal", ("EEPROM_NWP", 0, 0)), events)
        self.assertIn(("signal", ("SD_OVERCURRENT", 1, 0)), events)
        self.assertEqual(events[-3:], [("end", ""), ("pong", ""), ("ok", "")])

    def test_rejects_bad_protocol(self):
        payloads = [
            b"PIN 5 1 0\n",
            b"RPI-GPIO 2 58\n",
            b"RPI-GPIO 1 58\nPIN 58 0 0\n",
            b"RPI-GPIO 1 58\nERR syntax\n",
            b"RPI-GPIO 1 58\nWHAT\n",
        ]
        for payload in payloads:
            with self.subTest(payload=payload), \
                    self.assertRaises(ProtocolError):
                ResponseParser().feed(payload)

    def test_rejects_overlong_response(self):
        with self.assertRaises(ProtocolError):
            ResponseParser().feed(b"A" * 129)


class LibgpiodBackendTest(unittest.TestCase):
    def setUp(self):
        FakeGpiod.requests = []
        self.config = ProxyConfig(
            consumer="qemu-test",
            pins=(
                PinMapping(5, "/dev/gpiochip-test", 3, bias="pull-up"),
                PinMapping(
                    6,
                    "/dev/gpiochip-test",
                    4,
                    allow_output=True,
                    active_low=True,
                    drive="open-drain",
                    debounce_us=100,
                ),
            ),
        )
        self.backend = LibgpiodBackend(FakeGpiod)
        self.backend.start(self.config)

    def tearDown(self):
        self.backend.close()

    def test_requests_inputs_then_reconfigures_output_atomically(self):
        self.assertEqual(len(FakeGpiod.requests), 1)
        _path, consumer, request = FakeGpiod.requests[0]
        self.assertEqual(consumer, "qemu-test")
        self.assertEqual(request.config[3].direction, FakeLine.Direction.INPUT)
        self.assertEqual(request.config[3].bias, FakeLine.Bias.PULL_UP)
        self.assertIsNone(self.backend.apply(self.config.pins[1], 1, True))
        self.assertEqual(request.config[4].direction, FakeLine.Direction.OUTPUT)
        self.assertEqual(request.config[4].drive, FakeLine.Drive.OPEN_DRAIN)
        self.assertEqual(request.values[4], FakeLine.Value.ACTIVE)
        self.backend.apply(self.config.pins[1], 0, True)
        self.assertEqual(request.values[4], FakeLine.Value.INACTIVE)
        self.assertEqual(self.backend.apply(self.config.pins[1], 0, False), 0)
        self.assertEqual(request.config[4].direction, FakeLine.Direction.INPUT)

    def test_edge_events_safe_state_and_release(self):
        _path, _consumer, request = FakeGpiod.requests[0]
        request.values[3] = FakeLine.Value.ACTIVE
        request.events.append(FakeEvent(3))
        self.assertEqual(list(self.backend.read_events(request.fd)), [(5, 1)])
        self.backend.apply(self.config.pins[1], 1, True)
        self.backend.safe()
        self.assertEqual(request.config[3].direction, FakeLine.Direction.INPUT)
        self.assertEqual(request.config[4].direction, FakeLine.Direction.INPUT)
        self.backend.close()
        self.assertTrue(request.released)


class UsbSerialAdapter:
    def __init__(self, master: int):
        self.master = master
        self.commands = []
        self.values = {3: 1, 4: 0, 7: 1}
        self.interleave_next_read = False
        self.stop = threading.Event()
        self.failure = None
        self.thread = threading.Thread(target=self.run)
        self.thread.start()

    def run(self):
        buffer = bytearray()
        try:
            while not self.stop.is_set():
                readable, _, _ = select.select([self.master], [], [], 0.05)
                if not readable:
                    continue
                try:
                    data = os.read(self.master, 4096)
                except OSError:
                    return
                if not data:
                    return
                buffer.extend(data)
                while b"\n" in buffer:
                    raw, _, remainder = buffer.partition(b"\n")
                    buffer[:] = remainder
                    command = raw.rstrip(b"\r").decode("ascii")
                    self.commands.append(command)
                    fields = command.split()
                    if command == "HELLO 1":
                        response = "QGPIO 1 8"
                    elif fields[:1] == ["CONFIG"] or fields[:1] == ["WRITE"]:
                        if fields[0] == "WRITE":
                            self.values[int(fields[1])] = int(fields[2])
                        response = "OK"
                    elif fields[:1] == ["READ"]:
                        line = int(fields[1])
                        if self.interleave_next_read:
                            os.write(self.master, b"EDGE 3 1\n")
                            self.interleave_next_read = False
                        response = f"VALUE {line} {self.values[line]}"
                    elif command in ("SAFE", "PING"):
                        response = "OK"
                    else:
                        response = "ERR unsupported"
                    os.write(self.master, response.encode("ascii") + b"\n")
        except Exception as exc:  # pragma: no cover - surfaced by cleanup
            if not self.stop.is_set():
                self.failure = exc

    def send(self, line):
        os.write(self.master, line.encode("ascii") + b"\n")

    def close(self):
        self.stop.set()
        self.thread.join(2)
        if self.thread.is_alive():
            raise AssertionError("USB adapter thread did not stop")
        if self.failure is not None:
            raise self.failure


class UsbSerialBackendTest(unittest.TestCase):
    def setUp(self):
        self.master, self.slave = pty.openpty()
        self.adapter = UsbSerialAdapter(self.master)
        self.addCleanup(self.cleanup_adapter)
        self.config = ProxyConfig(
            consumer="usb-test",
            pins=(
                PinMapping(5, "usb", 3),
                PinMapping(
                    6,
                    "usb",
                    4,
                    allow_output=True,
                    active_low=True,
                    drive="open-drain",
                    debounce_us=100,
                ),
                PinMapping("EEPROM_NWP", "usb", 7),
            ),
        )
        self.backend = UsbSerialBackend(os.ttyname(self.slave), timeout=0.5)
        self.backend.start(self.config)

    def cleanup_adapter(self):
        self.backend.close()
        self.adapter.close()
        for fd in (self.slave, self.master):
            try:
                os.close(fd)
            except OSError:
                pass

    def test_handshake_io_edges_and_safe_release(self):
        self.assertEqual(
            self.adapter.commands[:4],
            [
                "HELLO 1",
                "CONFIG 3 IN 0 as-is 0",
                "CONFIG 4 IN 1 as-is 100",
                "CONFIG 7 IN 0 as-is 0",
            ],
        )
        self.assertEqual(self.backend.apply(self.config.pins[0], 0, False), 1)
        self.assertIsNone(self.backend.apply(self.config.pins[1], 1, True))
        self.assertIsNone(self.backend.apply(self.config.pins[1], 0, True))
        self.adapter.send("EDGE 3 0")
        readable, _, _ = select.select(
            list(self.backend.event_fds()), [], [], 1
        )
        self.assertEqual(len(readable), 1)
        self.assertEqual(list(self.backend.read_events(readable[0])), [(5, 0)])
        self.adapter.send("EDGE 4 1")
        readable, _, _ = select.select(
            list(self.backend.event_fds()), [], [], 1
        )
        self.assertEqual(list(self.backend.read_events(readable[0])), [])
        self.adapter.send("EDGE 7 0")
        readable, _, _ = select.select(
            list(self.backend.event_fds()), [], [], 1
        )
        self.assertEqual(
            list(self.backend.read_events(readable[0])),
            [("EEPROM_NWP", 0)],
        )
        self.assertEqual(self.backend.apply(self.config.pins[1], 0, False), 0)
        self.backend.safe()
        self.assertIn("CONFIG 4 OUT 1 open-drain 1", self.adapter.commands)
        self.assertIn("WRITE 4 0", self.adapter.commands)
        self.assertIn("CONFIG 4 IN 1 as-is 100", self.adapter.commands)
        self.assertEqual(self.adapter.commands[-1], "SAFE")
        self.backend.last_command = 0
        self.backend.service()
        self.assertEqual(self.adapter.commands[-1], "PING")

    def test_command_interleaved_edge_is_queued_without_loss(self):
        self.adapter.interleave_next_read = True
        self.assertEqual(self.backend.apply(self.config.pins[0], 0, False), 1)
        readable, _, _ = select.select(
            list(self.backend.event_fds()), [], [], 1
        )
        self.assertEqual(len(readable), 1)
        self.assertEqual(list(self.backend.read_events(readable[0])), [(5, 1)])

    def test_disconnect_fails_closed_without_masking_cleanup(self):
        self.adapter.close()
        os.close(self.master)
        self.master = -1
        with self.assertRaises((OSError, ProtocolError)):
            self.backend.apply(self.config.pins[0], 0, False)
        self.backend.close()
        self.assertIsNone(self.backend.fd)


class ControllerTest(unittest.TestCase):
    def setUp(self):
        self.proxy, self.peer = socket.socketpair()
        self.addCleanup(self.proxy.close)
        self.addCleanup(self.peer.close)
        self.peer.settimeout(0.1)
        self.config = ProxyConfig(
            consumer="test",
            pins=(
                PinMapping(5, "/dev/gpiochip-test", 0),
                PinMapping(6, "/dev/gpiochip-test", 1, allow_output=True),
                PinMapping(7, "/dev/gpiochip-test", 2),
                PinMapping("EEPROM_NWP", "/dev/gpiochip-test", 3),
            ),
        )
        self.backend = MockBackend(
            {5: 1, 6: 0, 7: 0, "EEPROM_NWP": 0}
        )
        self.backend.start(self.config)
        self.controller = BridgeController(
            self.config, self.backend, self.proxy
        )

    def receive(self):
        return self.peer.recv(4096).decode("ascii")

    def test_handshake_input_and_output(self):
        self.controller.handle_data(b"RPI-GPIO 1 58\n")
        self.assertEqual(self.receive(), "GET ALL\nGET SIGNALS\n")
        self.controller.handle_data(b"PIN 5 0 0\n")
        self.assertEqual(self.receive(), "SET 5 1\n")
        self.controller.handle_data(b"PIN 5 1 0\n")
        with self.assertRaises(TimeoutError):
            self.receive()

        self.controller.handle_data(b"PIN 6 1 1\n")
        self.assertEqual(self.backend.modes[6], ("output", 1))
        self.controller.handle_data(b"PIN 6 0 0\n")
        self.assertEqual(self.backend.modes[6], ("input", None))
        self.assertEqual(self.receive(), "SET 6 0\n")
        self.controller.handle_data(b"SIGNAL EEPROM_NWP Z\n")
        self.assertEqual(self.receive(), "SET SIGNAL EEPROM_NWP 0\n")

    def test_repeated_banner_resynchronizes_after_migration(self):
        self.controller.handle_data(b"RPI-GPIO 1 58\n")
        self.assertEqual(self.receive(), "GET ALL\nGET SIGNALS\n")
        self.controller.handle_data(b"PIN 6 1 1\n")
        self.assertEqual(self.backend.modes[6], ("output", 1))
        self.controller.handle_data(b"RPI-GPIO 1 58\n")
        self.assertEqual(self.receive(), "GET ALL\nGET SIGNALS\n")

    def test_denies_unapproved_guest_output(self):
        self.controller.handle_data(b"RPI-GPIO 1 58\n")
        self.receive()
        with self.assertRaises(SafetyError):
            self.controller.handle_data(b"PIN 7 1 1\n")
        self.assertEqual(self.backend.modes[7], ("input", None))

    def test_reset_and_shutdown_are_safe(self):
        self.controller.handle_data(b"RPI-GPIO 1 58\n")
        self.receive()
        self.controller.handle_data(b"PIN 6 1 1\n")
        self.controller.handle_data(b"RESET\n")
        self.assertTrue(self.backend.safed)
        self.assertEqual(self.backend.modes[6], ("input", None))
        self.assertEqual(self.receive(), "GET ALL\nGET SIGNALS\n")
        self.controller.shutdown()
        self.assertEqual(self.receive(), "RELEASE ALL\n")
        self.assertFalse(self.backend.started)


class IntegrationTest(unittest.TestCase):
    def test_mock_backend_over_real_unix_socket(self):
        with tempfile.TemporaryDirectory() as directory:
            path = str(Path(directory) / "gpio.sock")
            ready = threading.Event()
            transcript = []
            failures = []

            def qemu_stub():
                try:
                    with socket.socket(socket.AF_UNIX,
                                       socket.SOCK_STREAM) as server:
                        server.bind(path)
                        server.listen(1)
                        ready.set()
                        connection, _ = server.accept()
                        with connection:
                            connection.settimeout(2)
                            connection.sendall(b"RPI-GPIO 1 58\n")
                            transcript.append(
                                connection.recv(4096).decode("ascii")
                            )
                            connection.sendall(b"PIN 5 0 0\nEND\n")
                            transcript.append(
                                connection.recv(4096).decode("ascii")
                            )
                except Exception as exc:  # pragma: no cover - surfaced below
                    failures.append(exc)

            thread = threading.Thread(target=qemu_stub)
            thread.start()
            self.assertTrue(ready.wait(2))
            config = ProxyConfig(
                consumer="test",
                pins=(PinMapping(5, "/dev/gpiochip-test", 0),),
            )
            backend = MockBackend({5: 1})
            run_proxy(config, path, backend, 2)
            thread.join(2)
            self.assertFalse(thread.is_alive())
            self.assertEqual(failures, [])
            self.assertEqual(transcript[0], "GET ALL\n")
            self.assertIn("SET 5 1\n", transcript[1])
            self.assertTrue(backend.safed)
            self.assertFalse(backend.started)


if __name__ == "__main__":
    unittest.main()
