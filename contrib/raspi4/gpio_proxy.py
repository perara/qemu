#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fail-closed BCM2711 GPIO socket to libgpiod v2 bridge."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import timedelta
import json
import os
from pathlib import Path
import select
import selectors
import signal
import socket
import sys
import termios
import time
import tty
from typing import Iterable, Protocol, TypeAlias


GPIO_COUNT = 58
CONFIG_VERSION = 1
PROTOCOL_VERSION = 1
USB_PROTOCOL_VERSION = 1
MAX_LINE = 128
GLOBAL_KEYS = {"version", "consumer", "pins", "signals"}
PIN_KEYS = {
    "virtual",
    "chip",
    "line",
    "allow_output",
    "active_low",
    "bias",
    "drive",
    "debounce_us",
}
BIAS_VALUES = {"as-is", "disabled", "pull-up", "pull-down"}
DRIVE_VALUES = {"push-pull", "open-drain", "open-source"}
SIGNAL_NAMES = {"EEPROM_NWP", "SD_OVERCURRENT"}
SIGNAL_KEYS = {
    "name",
    "chip",
    "line",
    "active_low",
    "bias",
    "debounce_us",
}
BridgeKey: TypeAlias = int | str


class ConfigurationError(ValueError):
    pass


class ProtocolError(RuntimeError):
    pass


class SafetyError(RuntimeError):
    pass


@dataclass(frozen=True)
class PinMapping:
    virtual: BridgeKey
    chip: str
    line: int
    allow_output: bool = False
    active_low: bool = False
    bias: str = "as-is"
    drive: str = "push-pull"
    debounce_us: int = 0


@dataclass(frozen=True)
class ProxyConfig:
    consumer: str
    pins: tuple[PinMapping, ...]


def _require_type(value: object, expected: type, path: str) -> None:
    if type(value) is not expected:
        raise ConfigurationError(f"{path} must be {expected.__name__}")


def load_config(path: str | os.PathLike[str]) -> ProxyConfig:
    try:
        raw = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ConfigurationError(f"cannot read configuration: {exc}") from exc
    _require_type(raw, dict, "configuration")
    unknown = set(raw) - GLOBAL_KEYS
    if unknown:
        raise ConfigurationError(
            f"unknown configuration keys: {sorted(unknown)}"
        )
    if raw.get("version") != CONFIG_VERSION:
        raise ConfigurationError(f"version must be {CONFIG_VERSION}")
    consumer = raw.get("consumer", "qemu-rpi-gpio")
    _require_type(consumer, str, "consumer")
    if not consumer or len(consumer) > 64:
        raise ConfigurationError("consumer must contain 1 to 64 characters")
    pins = raw.get("pins", [])
    _require_type(pins, list, "pins")
    signals = raw.get("signals", [])
    _require_type(signals, list, "signals")
    if not pins and not signals:
        raise ConfigurationError("pins and signals must not both be empty")

    mappings: list[PinMapping] = []
    virtual_seen: set[BridgeKey] = set()
    physical_seen: set[tuple[str, int]] = set()
    for index, item in enumerate(pins):
        item_path = f"pins[{index}]"
        _require_type(item, dict, item_path)
        unknown = set(item) - PIN_KEYS
        if unknown:
            raise ConfigurationError(
                f"{item_path} has unknown keys: {sorted(unknown)}"
            )
        for required in ("virtual", "chip", "line"):
            if required not in item:
                raise ConfigurationError(f"{item_path}.{required} is required")
        virtual = item["virtual"]
        chip = item["chip"]
        line = item["line"]
        _require_type(virtual, int, f"{item_path}.virtual")
        _require_type(chip, str, f"{item_path}.chip")
        _require_type(line, int, f"{item_path}.line")
        if not 0 <= virtual < GPIO_COUNT:
            raise ConfigurationError(f"{item_path}.virtual must be 0..57")
        if chip != "usb" and (
            not Path(chip).is_absolute()
            or not Path(chip).name.startswith("gpiochip")
        ):
            raise ConfigurationError(
                f"{item_path}.chip must be an absolute gpiochip device path "
                'or "usb"'
            )
        if line < 0:
            raise ConfigurationError(f"{item_path}.line must be non-negative")
        if virtual in virtual_seen:
            raise ConfigurationError(
                f"virtual GPIO {virtual} is mapped more than once"
            )
        if (chip, line) in physical_seen:
            raise ConfigurationError(
                f"physical line {chip}:{line} is mapped more than once"
            )

        allow_output = item.get("allow_output", False)
        active_low = item.get("active_low", False)
        bias = item.get("bias", "as-is")
        drive = item.get("drive", "push-pull")
        debounce_us = item.get("debounce_us", 0)
        _require_type(allow_output, bool, f"{item_path}.allow_output")
        _require_type(active_low, bool, f"{item_path}.active_low")
        _require_type(bias, str, f"{item_path}.bias")
        _require_type(drive, str, f"{item_path}.drive")
        _require_type(debounce_us, int, f"{item_path}.debounce_us")
        if bias not in BIAS_VALUES:
            raise ConfigurationError(
                f"{item_path}.bias must be one of {sorted(BIAS_VALUES)}"
            )
        if drive not in DRIVE_VALUES:
            raise ConfigurationError(
                f"{item_path}.drive must be one of {sorted(DRIVE_VALUES)}"
            )
        if not 0 <= debounce_us <= 1_000_000:
            raise ConfigurationError(
                f"{item_path}.debounce_us must be 0..1000000"
            )
        if not allow_output and drive != "push-pull":
            raise ConfigurationError(
                f"{item_path}.drive requires allow_output=true"
            )

        mappings.append(
            PinMapping(
                virtual=virtual,
                chip=chip,
                line=line,
                allow_output=allow_output,
                active_low=active_low,
                bias=bias,
                drive=drive,
                debounce_us=debounce_us,
            )
        )
        virtual_seen.add(virtual)
        physical_seen.add((chip, line))
    for index, item in enumerate(signals):
        item_path = f"signals[{index}]"
        _require_type(item, dict, item_path)
        unknown = set(item) - SIGNAL_KEYS
        if unknown:
            raise ConfigurationError(
                f"{item_path} has unknown keys: {sorted(unknown)}"
            )
        for required in ("name", "chip", "line"):
            if required not in item:
                raise ConfigurationError(f"{item_path}.{required} is required")
        name = item["name"]
        chip = item["chip"]
        line = item["line"]
        _require_type(name, str, f"{item_path}.name")
        _require_type(chip, str, f"{item_path}.chip")
        _require_type(line, int, f"{item_path}.line")
        if name not in SIGNAL_NAMES:
            raise ConfigurationError(
                f"{item_path}.name must be one of {sorted(SIGNAL_NAMES)}"
            )
        if chip != "usb" and (
            not Path(chip).is_absolute()
            or not Path(chip).name.startswith("gpiochip")
        ):
            raise ConfigurationError(
                f"{item_path}.chip must be an absolute gpiochip device path "
                'or "usb"'
            )
        if line < 0:
            raise ConfigurationError(f"{item_path}.line must be non-negative")
        if name in virtual_seen:
            raise ConfigurationError(f"signal {name} is mapped more than once")
        if (chip, line) in physical_seen:
            raise ConfigurationError(
                f"physical line {chip}:{line} is mapped more than once"
            )
        active_low = item.get("active_low", False)
        bias = item.get("bias", "as-is")
        debounce_us = item.get("debounce_us", 0)
        _require_type(active_low, bool, f"{item_path}.active_low")
        _require_type(bias, str, f"{item_path}.bias")
        _require_type(debounce_us, int, f"{item_path}.debounce_us")
        if bias not in BIAS_VALUES:
            raise ConfigurationError(
                f"{item_path}.bias must be one of {sorted(BIAS_VALUES)}"
            )
        if not 0 <= debounce_us <= 1_000_000:
            raise ConfigurationError(
                f"{item_path}.debounce_us must be 0..1000000"
            )
        mappings.append(
            PinMapping(
                virtual=name,
                chip=chip,
                line=line,
                active_low=active_low,
                bias=bias,
                debounce_us=debounce_us,
            )
        )
        virtual_seen.add(name)
        physical_seen.add((chip, line))
    return ProxyConfig(consumer=consumer, pins=tuple(mappings))


class ResponseParser:
    def __init__(self) -> None:
        self._buffer = bytearray()
        self.hello_seen = False

    def feed(self, data: bytes) -> list[tuple[str, tuple[object, ...] | str]]:
        self._buffer.extend(data)
        if len(self._buffer) > MAX_LINE and b"\n" not in self._buffer:
            raise ProtocolError("QEMU response line is too long")
        events: list[tuple[str, tuple[int, ...] | str]] = []
        while True:
            newline = self._buffer.find(b"\n")
            if newline < 0:
                break
            raw = bytes(self._buffer[:newline])
            del self._buffer[: newline + 1]
            if raw.endswith(b"\r"):
                raw = raw[:-1]
            if len(raw) > MAX_LINE:
                raise ProtocolError("QEMU response line is too long")
            try:
                line = raw.decode("ascii")
            except UnicodeDecodeError as exc:
                raise ProtocolError("QEMU response is not ASCII") from exc
            if not line:
                continue
            fields = line.split()
            if fields[:1] == ["RPI-GPIO"] and len(fields) == 3:
                try:
                    version, count = int(fields[1]), int(fields[2])
                except ValueError as exc:
                    raise ProtocolError("invalid protocol banner") from exc
                if version != PROTOCOL_VERSION or count != GPIO_COUNT:
                    raise ProtocolError(
                        f"unsupported QEMU GPIO protocol {version}/{count}"
                    )
                self.hello_seen = True
                events.append(("hello", (version, count)))
            elif not self.hello_seen:
                raise ProtocolError("response received before protocol banner")
            elif fields[:1] == ["PIN"] and len(fields) == 4:
                try:
                    pin, level, output_enable = map(int, fields[1:])
                except ValueError as exc:
                    raise ProtocolError("invalid PIN response") from exc
                if (
                    not 0 <= pin < GPIO_COUNT
                    or level not in (0, 1)
                    or output_enable not in (0, 1)
                ):
                    raise ProtocolError("PIN response is out of range")
                events.append(("pin", (pin, level, output_enable)))
            elif (
                fields[:1] == ["SIGNAL"]
                and len(fields) == 3
                and fields[1] in SIGNAL_NAMES
            ):
                if fields[2] not in ("0", "1", "Z"):
                    raise ProtocolError("SIGNAL response is out of range")
                level = -1 if fields[2] == "Z" else int(fields[2])
                events.append(("signal", (fields[1], level, 0)))
            elif line in ("RESET", "END", "OK"):
                events.append((line.lower(), ""))
            elif fields[:1] == ["PONG"] and fields[1:] == ["1"]:
                events.append(("pong", ""))
            elif fields[:1] == ["ERR"] and len(fields) == 2:
                raise ProtocolError(
                    f"QEMU rejected bridge command: {fields[1]}"
                )
            else:
                raise ProtocolError(f"unknown QEMU response: {line}")
        return events


class GPIOBackend(Protocol):
    def start(self, config: ProxyConfig) -> None: ...
    def apply(
        self, mapping: PinMapping, level: int, output_enable: bool
    ) -> int | None: ...
    def event_fds(self) -> Iterable[int]: ...
    def read_events(self, fd: int) -> Iterable[tuple[BridgeKey, int]]: ...
    def service(self) -> None: ...
    def safe(self) -> None: ...
    def close(self) -> None: ...


class MockBackend:
    """Deterministic backend used by unit tests and protocol dry-runs."""

    def __init__(self, inputs: dict[BridgeKey, int] | None = None) -> None:
        self.inputs = dict(inputs or {})
        self.modes: dict[BridgeKey, tuple[str, int | None]] = {}
        self.started = False
        self.safed = False

    def start(self, config: ProxyConfig) -> None:
        self.started = True
        self.safed = False
        for mapping in config.pins:
            self.modes[mapping.virtual] = ("input", None)
            self.inputs.setdefault(mapping.virtual, 0)

    def apply(
            self, mapping: PinMapping, level: int,
            output_enable: bool) -> int | None:
        if output_enable:
            self.modes[mapping.virtual] = ("output", level)
            return None
        self.modes[mapping.virtual] = ("input", None)
        return self.inputs[mapping.virtual]

    def event_fds(self) -> Iterable[int]:
        return ()

    def read_events(self, fd: int) -> Iterable[tuple[BridgeKey, int]]:
        return ()

    def service(self) -> None:
        pass

    def safe(self) -> None:
        self.safed = True
        for virtual in self.modes:
            self.modes[virtual] = ("input", None)

    def close(self) -> None:
        self.safe()
        self.started = False


class LibgpiodBackend:
    def __init__(self, gpiod_module: object | None = None) -> None:
        self.gpiod = gpiod_module
        self.config: ProxyConfig | None = None
        self.requests: dict[str, object] = {}
        self.by_virtual: dict[BridgeKey, PinMapping] = {}
        self.by_physical: dict[tuple[str, int], PinMapping] = {}
        self.fd_to_chip: dict[int, str] = {}
        self.output_enabled: set[BridgeKey] = set()

    def _settings(self, mapping: PinMapping, output: bool, level: int = 0):
        assert self.gpiod is not None
        line = self.gpiod.line
        bias = {
            "as-is": line.Bias.AS_IS,
            "disabled": line.Bias.DISABLED,
            "pull-up": line.Bias.PULL_UP,
            "pull-down": line.Bias.PULL_DOWN,
        }[mapping.bias]
        if output:
            drive = {
                "push-pull": line.Drive.PUSH_PULL,
                "open-drain": line.Drive.OPEN_DRAIN,
                "open-source": line.Drive.OPEN_SOURCE,
            }[mapping.drive]
            return self.gpiod.LineSettings(
                direction=line.Direction.OUTPUT,
                drive=drive,
                active_low=mapping.active_low,
                output_value=(line.Value.ACTIVE if level
                              else line.Value.INACTIVE),
            )
        return self.gpiod.LineSettings(
            direction=line.Direction.INPUT,
            edge_detection=line.Edge.BOTH,
            bias=bias,
            active_low=mapping.active_low,
            debounce_period=timedelta(microseconds=mapping.debounce_us),
        )

    def start(self, config: ProxyConfig) -> None:
        gpiod = self.gpiod
        if gpiod is None:
            try:
                import gpiod  # type: ignore
            except ImportError as exc:
                raise RuntimeError(
                    "official python3 libgpiod v2 bindings are required"
                ) from exc
        if (not hasattr(gpiod, "request_lines")
           or not hasattr(gpiod, "LineSettings")):
            raise RuntimeError("libgpiod v2 Python API is required")
        self.gpiod = gpiod
        self.config = config
        groups: dict[str, list[PinMapping]] = {}
        for mapping in config.pins:
            groups.setdefault(mapping.chip, []).append(mapping)
            self.by_virtual[mapping.virtual] = mapping
            self.by_physical[(mapping.chip, mapping.line)] = mapping
        try:
            for chip, mappings in groups.items():
                if not gpiod.is_gpiochip_device(chip):
                    raise RuntimeError(
                        f"not a gpiochip character device: {chip}"
                    )
                request = gpiod.request_lines(
                    chip,
                    consumer=config.consumer,
                    config={m.line: self._settings(m, False) for m in mappings},
                )
                self.requests[chip] = request
                self.fd_to_chip[request.fd] = chip
        except Exception:
            self.close()
            raise

    def apply(
            self, mapping: PinMapping, level: int,
            output_enable: bool) -> int | None:
        assert self.gpiod is not None
        request = self.requests[mapping.chip]
        if output_enable:
            if mapping.virtual not in self.output_enabled:
                request.reconfigure_lines(
                    {mapping.line: self._settings(mapping, True, level)}
                )
                self.output_enabled.add(mapping.virtual)
            else:
                request.set_value(
                    mapping.line,
                    self.gpiod.line.Value.ACTIVE
                    if level
                    else self.gpiod.line.Value.INACTIVE,
                )
            return None
        if mapping.virtual in self.output_enabled:
            request.reconfigure_lines(
                {mapping.line: self._settings(mapping, False)}
            )
            self.output_enabled.remove(mapping.virtual)
        return int(
            request.get_value(mapping.line) == self.gpiod.line.Value.ACTIVE
        )

    def event_fds(self) -> Iterable[int]:
        return tuple(self.fd_to_chip)

    def read_events(self, fd: int) -> Iterable[tuple[BridgeKey, int]]:
        assert self.gpiod is not None
        chip = self.fd_to_chip[fd]
        request = self.requests[chip]
        result: list[tuple[BridgeKey, int]] = []
        for event in request.read_edge_events():
            mapping = self.by_physical[(chip, event.line_offset)]
            value = int(
                request.get_value(mapping.line) == self.gpiod.line.Value.ACTIVE
            )
            result.append((mapping.virtual, value))
        return result

    def service(self) -> None:
        pass

    def safe(self) -> None:
        if self.gpiod is None or self.config is None:
            return
        for mapping in self.config.pins:
            request = self.requests.get(mapping.chip)
            if request is not None:
                request.reconfigure_lines(
                    {mapping.line: self._settings(mapping, False)}
                )
        self.output_enabled.clear()

    def close(self) -> None:
        try:
            self.safe()
        finally:
            for request in self.requests.values():
                request.release()
            self.requests.clear()
            self.fd_to_chip.clear()


class UsbSerialBackend:
    """Deterministic fail-closed GPIO MCU protocol over a USB serial TTY."""

    def __init__(self, device: str, timeout: float = 2.0) -> None:
        self.device = device
        self.timeout = timeout
        self.fd: int | None = None
        self._buffer = bytearray()
        self.by_line: dict[int, PinMapping] = {}
        self.output_enabled: set[BridgeKey] = set()
        self.pending_events: list[tuple[BridgeKey, int]] = []
        self.event_read_fd: int | None = None
        self.event_write_fd: int | None = None
        self.last_command = 0.0

    def _extract_line(self) -> str | None:
        newline = self._buffer.find(b"\n")
        if newline < 0:
            if len(self._buffer) > MAX_LINE:
                raise ProtocolError("USB GPIO response line is too long")
            return None
        raw = bytes(self._buffer[:newline])
        del self._buffer[: newline + 1]
        if raw.endswith(b"\r"):
            raw = raw[:-1]
        if len(raw) > MAX_LINE:
            raise ProtocolError("USB GPIO response line is too long")
        try:
            return raw.decode("ascii")
        except UnicodeDecodeError as exc:
            raise ProtocolError("USB GPIO response is not ASCII") from exc

    def _readline(self) -> str:
        assert self.fd is not None
        deadline = time.monotonic() + self.timeout
        while True:
            line = self._extract_line()
            if line is not None:
                return line
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("USB GPIO adapter response timed out")
            readable, _, _ = select.select([self.fd], [], [], remaining)
            if not readable:
                raise TimeoutError("USB GPIO adapter response timed out")
            try:
                data = os.read(self.fd, 4096)
            except BlockingIOError:
                continue
            if not data:
                raise ProtocolError("USB GPIO adapter disconnected")
            self._buffer.extend(data)

    def _write(self, data: bytes) -> None:
        assert self.fd is not None
        offset = 0
        deadline = time.monotonic() + self.timeout
        while offset < len(data):
            try:
                written = os.write(self.fd, data[offset:])
            except BlockingIOError:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError("USB GPIO adapter write timed out")
                _, writable, _ = select.select([], [self.fd], [], remaining)
                if not writable:
                    raise TimeoutError("USB GPIO adapter write timed out")
                continue
            if written <= 0:
                raise ProtocolError("USB GPIO adapter disconnected")
            offset += written

    def _command(self, command: str) -> str:
        self._write(command.encode("ascii") + b"\n")
        while (response := self._readline()).startswith("EDGE "):
            self._queue_edge(response)
        if response.startswith("ERR "):
            raise ProtocolError(
                f"USB GPIO adapter rejected command: {response[4:]}"
            )
        self.last_command = time.monotonic()
        return response

    def _parse_edge(self, line: str) -> tuple[BridgeKey, int] | None:
        fields = line.split()
        if len(fields) != 3 or fields[0] != "EDGE":
            raise ProtocolError(f"unexpected USB GPIO event: {line}")
        try:
            physical, value = int(fields[1]), int(fields[2])
        except ValueError as exc:
            raise ProtocolError(f"invalid USB GPIO event: {line}") from exc
        mapping = self.by_line.get(physical)
        if mapping is None or value not in (0, 1):
            raise ProtocolError(f"invalid USB GPIO event: {line}")
        if mapping.virtual in self.output_enabled:
            return None
        return mapping.virtual, value

    def _queue_edge(self, line: str) -> None:
        event = self._parse_edge(line)
        if event is None:
            return
        notify = not self.pending_events
        self.pending_events.append(event)
        if notify and self.event_write_fd is not None:
            try:
                os.write(self.event_write_fd, b"\x01")
            except BlockingIOError:
                pass

    @staticmethod
    def _expect_ok(response: str) -> None:
        if response != "OK":
            raise ProtocolError(f"unexpected USB GPIO response: {response}")

    def _configure_input(self, mapping: PinMapping) -> None:
        response = self._command(
            f"CONFIG {mapping.line} IN {int(mapping.active_low)} "
            f"{mapping.bias} {mapping.debounce_us}"
        )
        self._expect_ok(response)
        self.output_enabled.discard(mapping.virtual)

    def start(self, config: ProxyConfig) -> None:
        if self.timeout <= 0:
            raise ConfigurationError("USB GPIO timeout must be positive")
        if not Path(self.device).is_absolute():
            raise ConfigurationError(
                "USB GPIO device must be an absolute TTY path"
            )
        if any(mapping.chip != "usb" for mapping in config.pins):
            raise ConfigurationError(
                'USB serial backend requires chip="usb" for every mapping'
            )

        fd = os.open(
            self.device,
            os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK,
        )
        event_read_fd: int | None = None
        event_write_fd: int | None = None
        try:
            event_read_fd, event_write_fd = os.pipe2(
                os.O_NONBLOCK | os.O_CLOEXEC
            )
            tty.setraw(fd, termios.TCSANOW)
            attrs = termios.tcgetattr(fd)
            attrs[4] = termios.B115200
            attrs[5] = termios.B115200
            termios.tcsetattr(fd, termios.TCSANOW, attrs)
            self.fd = fd
            self.event_read_fd = event_read_fd
            self.event_write_fd = event_write_fd
            self.last_command = time.monotonic()
            response = self._command(f"HELLO {USB_PROTOCOL_VERSION}")
            fields = response.split()
            if len(fields) != 3 or fields[0] != "QGPIO":
                raise ProtocolError(f"invalid USB GPIO banner: {response}")
            try:
                version, line_count = int(fields[1]), int(fields[2])
            except ValueError as exc:
                raise ProtocolError(
                    f"invalid USB GPIO banner: {response}"
                ) from exc
            if version != USB_PROTOCOL_VERSION or not 1 <= line_count <= 256:
                raise ProtocolError(
                    f"unsupported USB GPIO protocol {version}/{line_count}"
                )
            for mapping in config.pins:
                if mapping.line >= line_count:
                    raise ConfigurationError(
                        f"USB GPIO line {mapping.line} exceeds adapter "
                        f"line count {line_count}"
                    )
                self.by_line[mapping.line] = mapping
                self._configure_input(mapping)
        except Exception:
            if self.fd is not None:
                try:
                    self._expect_ok(self._command("SAFE"))
                except (OSError, ProtocolError, TimeoutError):
                    pass
            self.fd = None
            self.by_line.clear()
            self.output_enabled.clear()
            self.event_read_fd = None
            self.event_write_fd = None
            for event_fd in (event_read_fd, event_write_fd):
                if event_fd is not None:
                    os.close(event_fd)
            os.close(fd)
            raise

    def apply(
            self, mapping: PinMapping, level: int,
            output_enable: bool) -> int | None:
        if output_enable:
            if mapping.virtual not in self.output_enabled:
                response = self._command(
                    f"CONFIG {mapping.line} OUT {int(mapping.active_low)} "
                    f"{mapping.drive} {level}"
                )
                self._expect_ok(response)
                self.output_enabled.add(mapping.virtual)
            else:
                self._expect_ok(self._command(f"WRITE {mapping.line} {level}"))
            return None

        if mapping.virtual in self.output_enabled:
            self._configure_input(mapping)
        response = self._command(f"READ {mapping.line}")
        fields = response.split()
        if len(fields) != 3 or fields[:2] != ["VALUE", str(mapping.line)]:
            raise ProtocolError(f"unexpected USB GPIO response: {response}")
        try:
            value = int(fields[2])
        except ValueError as exc:
            raise ProtocolError(f"invalid USB GPIO value: {response}") from exc
        if value not in (0, 1):
            raise ProtocolError(f"invalid USB GPIO value: {response}")
        return value

    def event_fds(self) -> Iterable[int]:
        if self.fd is None or self.event_read_fd is None:
            return ()
        return self.fd, self.event_read_fd

    def read_events(self, fd: int) -> Iterable[tuple[BridgeKey, int]]:
        if self.event_read_fd is not None and fd == self.event_read_fd:
            while True:
                try:
                    if not os.read(fd, 4096):
                        break
                except BlockingIOError:
                    break
            events, self.pending_events = self.pending_events, []
            return events
        if self.fd is None or fd != self.fd:
            raise ProtocolError("unknown USB GPIO event descriptor")
        while True:
            try:
                data = os.read(fd, 4096)
            except BlockingIOError:
                break
            if not data:
                raise ProtocolError("USB GPIO adapter disconnected")
            self._buffer.extend(data)
            if len(data) < 4096:
                break

        events: list[tuple[BridgeKey, int]] = []
        while (line := self._extract_line()) is not None:
            event = self._parse_edge(line)
            if event is not None:
                events.append(event)
        return events

    def service(self) -> None:
        if self.fd is not None and time.monotonic() - self.last_command >= 0.2:
            self._expect_ok(self._command("PING"))

    def safe(self) -> None:
        if self.fd is None:
            return
        self._expect_ok(self._command("SAFE"))
        self.output_enabled.clear()

    def close(self) -> None:
        fd = self.fd
        if fd is None:
            return
        try:
            try:
                self.safe()
            except (OSError, ProtocolError, TimeoutError):
                # Link loss prevents acknowledgement. Adapter firmware is
                # required to enter SAFE autonomously on disconnect/watchdog.
                pass
        finally:
            self.fd = None
            self.by_line.clear()
            self.output_enabled.clear()
            self.pending_events.clear()
            os.close(fd)
            for event_fd in (self.event_read_fd, self.event_write_fd):
                if event_fd is not None:
                    os.close(event_fd)
            self.event_read_fd = None
            self.event_write_fd = None


class BridgeController:
    def __init__(
            self, config: ProxyConfig, backend: GPIOBackend,
            sock: socket.socket):
        self.config = config
        self.backend = backend
        self.sock = sock
        self.parser = ResponseParser()
        self.mappings = {mapping.virtual: mapping for mapping in config.pins}
        self.last_input_sent: dict[BridgeKey, int] = {}

    def send(self, command: str) -> None:
        self.sock.sendall(command.encode("ascii") + b"\n")

    def _send_input(self, virtual: BridgeKey, value: int) -> None:
        if self.last_input_sent.get(virtual) == value:
            return
        if isinstance(virtual, str):
            self.send(f"SET SIGNAL {virtual} {value}")
        else:
            self.send(f"SET {virtual} {value}")
        self.last_input_sent[virtual] = value

    def handle_pin(
        self, virtual: BridgeKey, level: int, output_enable: int
    ) -> None:
        mapping = self.mappings.get(virtual)
        if mapping is None:
            return
        if output_enable and not mapping.allow_output:
            raise SafetyError(
                f"guest attempted output on input-only virtual GPIO {virtual}"
            )
        physical_input = self.backend.apply(mapping, level, bool(output_enable))
        if output_enable:
            self.last_input_sent.pop(virtual, None)
        elif physical_input is not None:
            self._send_input(virtual, physical_input)

    def handle_data(self, data: bytes) -> None:
        for kind, payload in self.parser.feed(data):
            if kind == "hello":
                self.send("GET ALL")
                if any(isinstance(key, str) for key in self.mappings):
                    self.send("GET SIGNALS")
            elif kind in ("pin", "signal"):
                assert isinstance(payload, tuple)
                self.handle_pin(*payload)
            elif kind == "reset":
                self.last_input_sent.clear()
                self.backend.safe()
                self.send("GET ALL")
                if any(isinstance(key, str) for key in self.mappings):
                    self.send("GET SIGNALS")

    def handle_physical_events(self, fd: int) -> None:
        for virtual, value in self.backend.read_events(fd):
            self._send_input(virtual, value)

    def shutdown(self) -> None:
        try:
            self.send("RELEASE ALL")
        except OSError:
            pass
        self.backend.close()


def connect_unix(path: str, timeout: float) -> socket.socket:
    deadline = time.monotonic() + timeout
    last_error: OSError | None = None
    while time.monotonic() < deadline:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            sock.connect(path)
            return sock
        except OSError as exc:
            last_error = exc
            sock.close()
            time.sleep(0.05)
    raise TimeoutError(f"could not connect to {path}: {last_error}")


def run_proxy(config: ProxyConfig, socket_path: str, backend: GPIOBackend,
              connect_timeout: float) -> None:
    backend.start(config)
    sock: socket.socket | None = None
    controller: BridgeController | None = None
    selector = selectors.DefaultSelector()
    stop = False

    def request_stop(signum: int, frame: object) -> None:
        nonlocal stop
        stop = True

    old_handlers = {
        signum: signal.signal(signum, request_stop)
        for signum in (signal.SIGINT, signal.SIGTERM)
    }
    try:
        sock = connect_unix(socket_path, connect_timeout)
        sock.setblocking(False)
        controller = BridgeController(config, backend, sock)
        selector.register(sock, selectors.EVENT_READ, ("qemu", None))
        for fd in backend.event_fds():
            selector.register(fd, selectors.EVENT_READ, ("gpio", fd))
        while not stop:
            for key, _events in selector.select(timeout=0.1):
                kind, fd = key.data
                if kind == "qemu":
                    data = sock.recv(4096)
                    if not data:
                        return
                    controller.handle_data(data)
                else:
                    controller.handle_physical_events(fd)
            backend.service()
    except (ProtocolError, SafetyError):
        if controller is not None:
            try:
                controller.send("RELEASE ALL")
            except OSError:
                pass
        raise
    finally:
        selector.close()
        if controller is not None:
            controller.shutdown()
        else:
            backend.close()
        if sock is not None:
            sock.close()
        for signum, handler in old_handlers.items():
            signal.signal(signum, handler)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--config", required=True, help="versioned pin-map JSON"
    )
    parser.add_argument("--socket", required=True, help="QEMU GPIO Unix socket")
    parser.add_argument(
        "--backend", choices=("libgpiod", "usb-serial", "mock"),
        default="libgpiod",
        help="physical GPIO implementation (mock is for CI/dry-runs)",
    )
    parser.add_argument(
        "--usb-device",
        help="USB serial adapter TTY, required for --backend usb-serial",
    )
    parser.add_argument("--usb-timeout", type=float, default=2.0)
    parser.add_argument("--connect-timeout", type=float, default=10.0)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        config = load_config(args.config)
        if args.connect_timeout <= 0:
            raise ConfigurationError("connect timeout must be positive")
        if args.backend == "libgpiod":
            backend: GPIOBackend = LibgpiodBackend()
        elif args.backend == "usb-serial":
            if not args.usb_device:
                raise ConfigurationError(
                    "--usb-device is required for --backend usb-serial"
                )
            backend = UsbSerialBackend(args.usb_device, args.usb_timeout)
        else:
            backend = MockBackend()
        run_proxy(config, args.socket, backend, args.connect_timeout)
    except (ConfigurationError, ProtocolError, SafetyError, RuntimeError,
            TimeoutError, OSError) as exc:
        print(f"gpio-proxy: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
