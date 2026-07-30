#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Unit tests for the fail-closed CM4 provisioning supervisor."""

from pathlib import Path
import hashlib
import importlib.util
import json
import os
import pty
import subprocess
import struct
import sys
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("cm4_provision.py")
SPEC = importlib.util.spec_from_file_location("cm4_provision", MODULE_PATH)
assert SPEC and SPEC.loader
cm4_provision = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(cm4_provision)


class Cm4ProvisionTests(unittest.TestCase):
    @staticmethod
    def _eeprom() -> tuple[bytes, str, str, int]:
        dependency_contents = b"abcdefghabcdefghTAIL!"
        dependency_hash = hashlib.sha256(dependency_contents).digest()
        payload = bytearray(
            (0xA0 + index) & 0xFF for index in range(128)
        )
        payload[:len(dependency_hash)] = dependency_hash
        signature = bytes((index + 1) & 0xFF for index in range(256))
        hmac = bytes(0x80 + index for index in range(20))
        bootsys = payload + struct.pack("<II", len(payload), 1)
        bootsys += signature + hmac
        image = bytearray(
            struct.pack(">II", cm4_provision.EEPROM_MAGIC, len(bootsys))
        )
        image.extend(bootsys)
        image.extend(b"\xff" * (-len(image) % 8))
        dependency_set = hashlib.sha256()
        for name in ("bootmain", "mcb.bin", "memsys00.bin"):
            name_field = name.encode("ascii").ljust(16, b"\0")
            descriptor = struct.pack(
                "<BBQ", 0x48, 0x40, len(dependency_contents)
            )
            header_checksum = (
                cm4_provision._xxh32_short(descriptor) >> 8
            ) & 0xFF
            frame = struct.pack("<I", cm4_provision.LZ4_FRAME_MAGIC)
            frame += descriptor + bytes([header_checksum])
            frame += struct.pack("<I", (1 << 31) | 8)
            second_block = b"\x04\x08\x00\x50TAIL!"
            frame += dependency_contents[:8]
            frame += struct.pack("<I", len(second_block)) + second_block
            frame += struct.pack("<I", 0)
            section = name_field + frame + dependency_hash
            image.extend(struct.pack(
                ">II", cm4_provision.EEPROM_DEPENDENCY_MAGIC, len(section)
            ))
            image.extend(section)
            image.extend(b"\xff" * (-len(image) % 8))
            dependency_set.update(name_field)
            dependency_set.update(dependency_hash)
        return (
            bytes(image),
            hashlib.sha256(bootsys).hexdigest(),
            dependency_set.hexdigest(),
            1,
        )

    def test_dependency_lz4_header_checksum_is_enforced(self):
        image, _, _, _ = self._eeprom()
        bootsys_size = int.from_bytes(image[4:8], "big")
        section_offset = (8 + bootsys_size + 7) & ~7
        section_size = int.from_bytes(
            image[section_offset + 4:section_offset + 8], "big"
        )
        frame = image[
            section_offset + 8 + cm4_provision.EEPROM_DEPENDENCY_NAME_SIZE:
            section_offset + 8 + section_size
            - cm4_provision.EEPROM_DEPENDENCY_HASH_SIZE
        ]

        self.assertEqual(
            cm4_provision._lz4_frame_decode(frame),
            b"abcdefghabcdefghTAIL!",
        )
        damaged = bytearray(frame)
        damaged[14] ^= 1
        with self.assertRaisesRegex(
            cm4_provision.ProvisionError, "header checksum"
        ):
            cm4_provision._lz4_frame_decode(bytes(damaged))

    def _manifest(self, directory: Path) -> tuple[Path, dict[str, Path]]:
        paths: dict[str, Path] = {}
        artifacts = {}
        for index, role in enumerate(sorted(cm4_provision.REQUIRED_ARTIFACTS)):
            path = directory / role.replace("/", "-")
            if role == "eeprom":
                (
                    contents,
                    bootsys_hash,
                    dependencies_hash,
                    bootsys_key_index,
                ) = self._eeprom()
            else:
                contents = f"artifact-{index}-{role}\n".encode("ascii")
            path.write_bytes(contents)
            paths[role] = path
            artifacts[role] = {
                "sha256": hashlib.sha256(contents).hexdigest(),
                "size": len(contents),
            }
            if role == "eeprom":
                artifacts[role]["bootsys_sha256"] = bootsys_hash
                artifacts[role]["dependencies_sha256"] = dependencies_hash
                artifacts[role]["bootsys_key_index"] = bootsys_key_index
        artifacts["image"]["payload_sha256"] = "1" * 64
        manifest = directory / "manifest.json"
        manifest.write_text(
            json.dumps({
                "schema": cm4_provision.MANIFEST_SCHEMA,
                "artifacts": artifacts,
            }),
            encoding="utf-8",
        )
        return manifest, paths

    def test_binary_size_parser(self):
        self.assertEqual(cm4_provision.parse_size("4GiB"), 4 * 1024 ** 3)
        self.assertEqual(cm4_provision.parse_size("8192K"), 8 * 1024 ** 2)

    def test_size_parser_rejects_unaligned_values(self):
        with self.assertRaisesRegex(Exception, "512-byte aligned"):
            cm4_provision.parse_size("513B")

    def test_loaded_kernel_module_skips_modprobe(self):
        with tempfile.TemporaryDirectory() as directory_name:
            root = Path(directory_name)
            parameters = root / "dummy_hcd" / "parameters"
            parameters.mkdir(parents=True)
            (parameters / "is_high_speed").write_text(
                "Y\n", encoding="ascii"
            )
            with mock.patch.object(cm4_provision.subprocess, "run") as run:
                cm4_provision.ensure_kernel_module(
                    ["sudo", "-n"], "dummy_hcd", "is_high_speed=1",
                    module_root=root,
                )
            run.assert_not_called()

    def test_loaded_kernel_module_parameter_mismatch_fails(self):
        with tempfile.TemporaryDirectory() as directory_name:
            root = Path(directory_name)
            parameters = root / "dummy_hcd" / "parameters"
            parameters.mkdir(parents=True)
            (parameters / "is_high_speed").write_text(
                "N\n", encoding="ascii"
            )
            with self.assertRaisesRegex(
                cm4_provision.ProvisionError,
                "parameter is_high_speed differs",
            ):
                cm4_provision.ensure_kernel_module(
                    ["sudo", "-n"], "dummy_hcd", "is_high_speed=1",
                    module_root=root,
                )

    def test_absent_kernel_module_is_loaded_and_verified(self):
        with tempfile.TemporaryDirectory() as directory_name:
            root = Path(directory_name)

            def load(*_args, **_kwargs):
                (root / "raw_gadget").mkdir()

            with mock.patch.object(
                cm4_provision.subprocess, "run", side_effect=load
            ) as run:
                cm4_provision.ensure_kernel_module(
                    ["sudo", "-n"], "raw_gadget", module_root=root
                )
            run.assert_called_once_with(
                ["sudo", "-n", "modprobe", "raw_gadget"],
                check=True,
                timeout=None,
            )

    def test_manifest_and_artifacts_validate(self):
        with tempfile.TemporaryDirectory() as directory_name:
            directory = Path(directory_name)
            manifest_path, paths = self._manifest(directory)
            manifest = cm4_provision.load_manifest(manifest_path)
            observed = cm4_provision.verify_artifacts(manifest, paths)
            self.assertEqual(
                set(observed),
                cm4_provision.REQUIRED_ARTIFACTS |
                {
                    "bootsys",
                    "bootsys-dependencies",
                    "bootsys-dependency-count",
                    "bootsys-key-index",
                },
            )
            self.assertEqual(observed["bootsys-dependency-count"], "3")
            self.assertEqual(observed["bootsys-key-index"], "1")

    def test_manifest_rejects_missing_bootsys_trust(self):
        with tempfile.TemporaryDirectory() as directory_name:
            directory = Path(directory_name)
            manifest_path, _paths = self._manifest(directory)
            document = json.loads(manifest_path.read_text(encoding="utf-8"))
            del document["artifacts"]["eeprom"]["bootsys_sha256"]
            manifest_path.write_text(json.dumps(document), encoding="utf-8")
            with self.assertRaisesRegex(
                cm4_provision.ProvisionError, "trusted bootsys_sha256"
            ):
                cm4_provision.load_manifest(manifest_path)

    def test_bootsys_tamper_is_rejected_independently(self):
        with tempfile.TemporaryDirectory() as directory_name:
            directory = Path(directory_name)
            manifest_path, paths = self._manifest(directory)
            manifest = cm4_provision.load_manifest(manifest_path)
            image = bytearray(paths["eeprom"].read_bytes())
            image[8 + 128 + 8] ^= 1
            paths["eeprom"].write_bytes(image)
            manifest["eeprom"]["sha256"] = hashlib.sha256(image).hexdigest()
            with self.assertRaisesRegex(
                cm4_provision.ProvisionError, "bootsys SHA-256 differs"
            ):
                cm4_provision.verify_artifacts(manifest, paths)

    def test_manifest_rejects_missing_bootsys_key_index(self):
        with tempfile.TemporaryDirectory() as directory_name:
            directory = Path(directory_name)
            manifest_path, _paths = self._manifest(directory)
            document = json.loads(manifest_path.read_text(encoding="utf-8"))
            del document["artifacts"]["eeprom"]["bootsys_key_index"]
            manifest_path.write_text(json.dumps(document), encoding="utf-8")
            with self.assertRaisesRegex(
                cm4_provision.ProvisionError, "requires bootsys_key_index"
            ):
                cm4_provision.load_manifest(manifest_path)

    def test_bootsys_key_index_oracle_mismatch_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory_name:
            directory = Path(directory_name)
            manifest_path, paths = self._manifest(directory)
            manifest = cm4_provision.load_manifest(manifest_path)
            manifest["eeprom"]["bootsys_key_index"] = 0
            with self.assertRaisesRegex(
                cm4_provision.ProvisionError, "bootsys key index differs"
            ):
                cm4_provision.verify_artifacts(manifest, paths)

    def test_manifest_rejects_missing_dependency_trust(self):
        with tempfile.TemporaryDirectory() as directory_name:
            directory = Path(directory_name)
            manifest_path, _paths = self._manifest(directory)
            document = json.loads(manifest_path.read_text(encoding="utf-8"))
            del document["artifacts"]["eeprom"]["dependencies_sha256"]
            manifest_path.write_text(json.dumps(document), encoding="utf-8")
            with self.assertRaisesRegex(
                cm4_provision.ProvisionError, "trusted dependencies_sha256"
            ):
                cm4_provision.load_manifest(manifest_path)

    def test_dependency_tamper_is_rejected_independently(self):
        with tempfile.TemporaryDirectory() as directory_name:
            directory = Path(directory_name)
            manifest_path, paths = self._manifest(directory)
            manifest = cm4_provision.load_manifest(manifest_path)
            image = bytearray(paths["eeprom"].read_bytes())
            dependency_offset = (
                8 + 128 + cm4_provision.BOOTSYS_TRAILER_SIZE + 7
            ) & ~7
            data_offset = dependency_offset + 8 + 16 + 15 + 4
            image[data_offset] ^= 1
            paths["eeprom"].write_bytes(image)
            manifest["eeprom"]["sha256"] = hashlib.sha256(image).hexdigest()
            with self.assertRaisesRegex(
                cm4_provision.ProvisionError,
                "dependency 'bootmain' SHA-256 differs",
            ):
                cm4_provision.verify_artifacts(manifest, paths)

    def test_dependency_oracle_mismatch_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory_name:
            directory = Path(directory_name)
            manifest_path, paths = self._manifest(directory)
            manifest = cm4_provision.load_manifest(manifest_path)
            manifest["eeprom"]["dependencies_sha256"] = "0" * 64
            with self.assertRaisesRegex(
                cm4_provision.ProvisionError,
                "dependency-set SHA-256 differs",
            ):
                cm4_provision.verify_artifacts(manifest, paths)

    def test_qemu_command_pins_bootsys_trust(self):
        args = SimpleNamespace(
            qemu=Path("/opt/qemu-system-aarch64"),
            lifecycle=Path("/run/qemu-rpi/state"),
            workdir=Path("/run/qemu-rpi"),
            eeprom_backend=Path("/run/qemu-rpi/pieeprom.bin"),
            emmc=Path("/run/qemu-rpi/emmc.img"),
            ram="2G",
            post_qemu_arg=[],
            bootsys_trusted_sha256="a" * 64,
            rpiboot_bootcode_trusted_sha256="b" * 64,
        )
        command = cm4_provision.qemu_command(
            args, Path("/run/qemu-rpi/qmp.sock"), nrpiboot=False
        )
        machine = command[command.index("-M") + 1]
        self.assertIn(
            "bootsys-trusted-sha256=" + "a" * 64,
            machine,
        )
        self.assertIn(
            "rpiboot-bootcode-trusted-sha256=" + "b" * 64,
            machine,
        )

    def test_qemu_command_routes_inprocess_dwc2_to_proxy_socket(self):
        args = SimpleNamespace(
            qemu=Path("/opt/qemu-system-aarch64"),
            lifecycle=Path("/run/qemu-rpi/state"),
            workdir=Path("/run/qemu-rpi"),
            eeprom_backend=Path("/run/qemu-rpi/pieeprom.bin"),
            emmc=Path("/run/qemu-rpi/emmc.img"),
            ram="2G",
            post_qemu_arg=[],
            bootsys_trusted_sha256="a" * 64,
            rpiboot_bootcode_trusted_sha256="b" * 64,
        )
        command = cm4_provision.qemu_command(
            args, Path("/run/qemu-rpi/qmp.sock"), nrpiboot=True,
            device_socket=Path("/run/qemu-rpi/dwc2.sock"),
        )
        self.assertIn(
            "socket,id=dwc2dev,path=/run/qemu-rpi/dwc2.sock,"
            "server=on,wait=off",
            command,
        )
        self.assertIn("dwc2-usb.device-chardev=dwc2dev", command)
        self.assertNotIn("-no-reboot", command)

    def test_manifest_rejects_missing_role(self):
        with tempfile.TemporaryDirectory() as directory_name:
            directory = Path(directory_name)
            manifest_path, _paths = self._manifest(directory)
            document = json.loads(manifest_path.read_text(encoding="utf-8"))
            del document["artifacts"]["boot.img"]
            manifest_path.write_text(json.dumps(document), encoding="utf-8")
            with self.assertRaisesRegex(
                cm4_provision.ProvisionError, "manifest roles differ"
            ):
                cm4_provision.load_manifest(manifest_path)

    def test_artifact_byte_change_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory_name:
            directory = Path(directory_name)
            manifest_path, paths = self._manifest(directory)
            manifest = cm4_provision.load_manifest(manifest_path)
            paths["bootcode4.bin"].write_bytes(b"changed")
            with self.assertRaisesRegex(
                cm4_provision.ProvisionError, "size differs"
            ):
                cm4_provision.verify_artifacts(manifest, paths)

    def test_emmc_is_created_at_exact_size(self):
        with tempfile.TemporaryDirectory() as directory_name:
            image = Path(directory_name) / "emmc.img"
            cm4_provision.prepare_emmc(image, 4096)
            self.assertEqual(image.stat().st_size, 4096)
            cm4_provision.prepare_emmc(image, 8192)
            self.assertEqual(image.stat().st_size, 8192)

    def test_emmc_symlink_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory_name:
            directory = Path(directory_name)
            target = directory / "target"
            target.write_bytes(bytes(512))
            link = directory / "emmc.img"
            link.symlink_to(target)
            with self.assertRaises(cm4_provision.ProvisionError):
                cm4_provision.prepare_emmc(link, 1024)
            self.assertEqual(target.stat().st_size, 512)

    def test_eeprom_source_is_copied_byte_identically_and_writable(self):
        with tempfile.TemporaryDirectory() as directory_name:
            directory = Path(directory_name)
            source = directory / "official.bin"
            backend = directory / "work" / "pieeprom.bin"
            contents = bytes(range(256)) * 4
            source.write_bytes(contents)
            source.chmod(0o440)
            expected = hashlib.sha256(contents).hexdigest()
            cm4_provision.prepare_eeprom(source, backend, expected)
            self.assertEqual(backend.read_bytes(), contents)
            self.assertTrue(backend.stat().st_mode & 0o200)

    def test_eeprom_backend_must_not_already_exist(self):
        with tempfile.TemporaryDirectory() as directory_name:
            directory = Path(directory_name)
            source = directory / "official.bin"
            backend = directory / "pieeprom.bin"
            source.write_bytes(b"official")
            backend.write_bytes(b"existing")
            with self.assertRaisesRegex(
                cm4_provision.ProvisionError, "private EEPROM backend"
            ):
                cm4_provision.prepare_eeprom(
                    source, backend, hashlib.sha256(b"official").hexdigest()
                )
            self.assertEqual(backend.read_bytes(), b"existing")

    def test_read_until_observes_process_readiness(self):
        process = subprocess.Popen(
            [sys.executable, "-c",
             "import time; print('ready marker', flush=True); time.sleep(.05)"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        output = cm4_provision.read_until(process, "ready marker", 1.0)
        self.assertIn("ready marker", output)
        process.wait(timeout=1)
        process.stdout.close()

    def test_configfs_owner_command_remains_default(self):
        args = SimpleNamespace(
            mass_storage_mode="configfs",
            sudo=["sudo", "-n"],
            mass_storage=Path("/src/cm4_mass_storage.py"),
            raw_msd=Path("/build/qemu-rpi-cm4-msd"),
            lifecycle=Path("/run/cm4.state"),
            emmc=Path("/run/cm4.img"),
        )
        command, marker = cm4_provision.mass_storage_owner_command(args)
        self.assertEqual(command, [
            "sudo", "-n", sys.executable, "/src/cm4_mass_storage.py",
            "--lifecycle", "/run/cm4.state", "serve", "/run/cm4.img",
        ])
        self.assertEqual(marker, "CM4 mass-storage owner ready")

    def test_raw_bot_owner_command_uses_same_emmc_and_lifecycle(self):
        args = SimpleNamespace(
            mass_storage_mode="raw-bot",
            sudo=[],
            mass_storage=Path("/src/cm4_mass_storage.py"),
            raw_msd=Path("/build/qemu-rpi-cm4-msd"),
            lifecycle=Path("/run/cm4.state"),
            emmc=Path("/run/cm4.img"),
        )
        command, marker = cm4_provision.mass_storage_owner_command(args)
        self.assertEqual(command, [
            "/build/qemu-rpi-cm4-msd", "--image", "/run/cm4.img",
            "--lifecycle", "/run/cm4.state", "--foreground-owner",
        ])
        self.assertEqual(marker, "CM4 raw BOT target ready")

    def _raw_bot_tree(self, root: Path) -> tuple[Path, Path, Path]:
        """Build the sysfs and /dev fixture the raw BOT owner produces."""
        sys_root = root / "sys"
        dev_root = root / "dev"
        usb = sys_root / "devices/platform/dummy_hcd.0/usb6/6-1"
        block = usb / "6-1:1.0/host6/target6:0:0/6:0:0:0/block/sda"
        block.mkdir(parents=True)
        (usb / "idVendor").write_text("0a5c\n", encoding="ascii")
        (usb / "idProduct").write_text("0104\n", encoding="ascii")
        (usb / "product").write_text(
            "Raspberry Pi multi-function USB device\n", encoding="ascii"
        )
        (block / "size").write_text("16384\n", encoding="ascii")
        (block / "device").mkdir()
        (block / "device/vendor").write_text("mmcblk0\n", encoding="ascii")
        class_block = sys_root / "class/block"
        class_block.mkdir(parents=True)
        (class_block / "sda").symlink_to(block)
        dev_root.mkdir()
        target = dev_root / "sda"
        target.write_bytes(b"device")
        by_id = dev_root / "disk/by-id"
        by_id.mkdir(parents=True)
        disk_name = (
            "usb-mmcblk0_Raspberry_Pi_multi-function_USB_device_"
            f"{cm4_provision.RAW_MSD_SERIAL}-0:0"
        )
        (by_id / disk_name).symlink_to(target)
        for index in (1, 2):
            partition = dev_root / f"sda{index}"
            partition.write_bytes(b"partition")
            (by_id / f"{disk_name}-part{index}").symlink_to(partition)
        return sys_root, dev_root, target

    def test_raw_bot_target_requires_correlated_usb_identity(self):
        with tempfile.TemporaryDirectory() as directory_name:
            root = Path(directory_name)
            sys_root, dev_root, target = self._raw_bot_tree(root)
            process = SimpleNamespace(
                returncode=None,
                stdout=None,
                poll=lambda: None,
            )
            with mock.patch.object(
                cm4_provision.stat, "S_ISBLK", return_value=True
            ):
                observed = cm4_provision.wait_raw_msd_target(
                    process, dev_root, sys_root, 16384 * 512, 0.3
                )
            self.assertEqual(observed, target)

    def test_raw_bot_serial_alias_alone_does_not_select_a_target(self):
        """A by-id name is not identity: the block device must corroborate."""
        with tempfile.TemporaryDirectory() as directory_name:
            root = Path(directory_name)
            sys_root, dev_root, target = self._raw_bot_tree(root)
            substitute = dev_root / "sdb"
            substitute.write_bytes(b"substituted device")
            by_id = dev_root / "disk/by-id"
            for link in by_id.iterdir():
                if link.name.endswith("-0:0"):
                    link.unlink()
                    link.symlink_to(substitute)
            process = SimpleNamespace(
                returncode=None,
                stdout=None,
                poll=lambda: None,
            )
            with mock.patch.object(
                cm4_provision.stat, "S_ISBLK", return_value=True
            ), self.assertRaisesRegex(
                cm4_provision.ProvisionError,
                "did not produce a stable",
            ):
                cm4_provision.wait_raw_msd_target(
                    process, dev_root, sys_root, 16384 * 512, 0.3
                )
            self.assertTrue(target.exists())

    def test_continued_guest_target_requires_exact_usb_model_and_size(self):
        with tempfile.TemporaryDirectory() as directory_name:
            root = Path(directory_name)
            sys_root = root / "sys"
            dev_root = root / "dev"
            usb = (
                sys_root / "devices/platform/dummy_hcd.0/usb5/5-1"
            )
            block = (
                usb / "5-1:1.0/host5/target5:0:0/5:0:0:0/block/sdz"
            )
            block.mkdir(parents=True)
            (usb / "idVendor").write_text("0a5c\n", encoding="ascii")
            (usb / "idProduct").write_text("0104\n", encoding="ascii")
            (block / "size").write_text("8388608\n", encoding="ascii")
            (block / "device").mkdir()
            (block / "device/vendor").write_text(
                "mmcblk0\n", encoding="ascii"
            )
            (usb / "product").write_text(
                "Raspberry Pi multi-function USB device\n", encoding="ascii"
            )
            class_block = sys_root / "class/block"
            class_block.mkdir(parents=True)
            (class_block / "sdz").symlink_to(block)
            dev_root.mkdir()
            target = dev_root / "sdz"
            target.write_bytes(b"block device placeholder")
            process = SimpleNamespace(
                returncode=None,
                stdout=None,
                poll=lambda: None,
            )
            with mock.patch.object(
                cm4_provision.stat, "S_ISBLK", return_value=True
            ):
                observed = cm4_provision.wait_guest_msd_target(
                    process, dev_root, sys_root, 4 * 1024 ** 3, 0.3
                )
            self.assertEqual(observed, target)

            tty_device = usb / "5-1:1.1/tty/ttyACM7"
            tty_device.mkdir(parents=True)
            class_tty = sys_root / "class/tty"
            class_tty.mkdir(parents=True)
            (class_tty / "ttyACM7").symlink_to(tty_device)
            acm = dev_root / "ttyACM7"
            acm.write_bytes(b"character device placeholder")
            with mock.patch.object(
                cm4_provision.stat, "S_ISCHR", return_value=True
            ):
                observed_acm = cm4_provision.wait_guest_acm_target(
                    process, target, dev_root, sys_root, 0.3
                )
            self.assertEqual(observed_acm, acm)

            (usb / "busnum").write_text("5\n", encoding="ascii")
            (usb / "devnum").write_text("94\n", encoding="ascii")
            reset_result = subprocess.CompletedProcess(
                [], 0, "Resetting device\n", ""
            )
            with mock.patch.object(
                cm4_provision.subprocess, "run", return_value=reset_result
            ) as run:
                cm4_provision.reset_guest_usb(
                    process, target, sys_root, Path("/usr/bin/usbreset"),
                    ["sudo", "-n"], 3.0,
                )
            run.assert_called_once_with(
                ["sudo", "-n", "/usr/bin/usbreset", "005/094"],
                capture_output=True,
                text=True,
                timeout=3.0,
            )

            (usb / "idProduct").write_text("0001\n", encoding="ascii")
            with mock.patch.object(
                cm4_provision.stat, "S_ISBLK", return_value=True
            ), self.assertRaisesRegex(
                cm4_provision.ProvisionError,
                "continued guest did not expose",
            ):
                cm4_provision.wait_guest_msd_target(
                    process, dev_root, sys_root, 4 * 1024 ** 3, 0.05
                )

    def test_acm_echo_proves_bidirectional_remote_path(self):
        master, slave = pty.openpty()
        target = Path(os.ttyname(slave))
        token = b"QEMU_RPI_ACM_TEST"
        completed = threading.Event()

        def remote_echo() -> None:
            try:
                request = os.read(master, 4096)
                os.write(master, request)
            finally:
                completed.set()

        worker = threading.Thread(target=remote_echo, daemon=True)
        worker.start()
        try:
            cm4_provision.verify_acm_echo(target, token, 1.0)
            self.assertTrue(completed.wait(1.0))
        finally:
            os.close(slave)
            os.close(master)
            worker.join(timeout=1.0)

    def test_guest_block_prefix_must_match_backend_after_reset(self):
        with tempfile.TemporaryDirectory() as directory_name:
            directory = Path(directory_name)
            backend = directory / "emmc.img"
            target = directory / "sdz"
            contents = b"reset-ok"
            backend.write_bytes(contents)
            target.write_bytes(b"unused placeholder")
            process = SimpleNamespace(
                returncode=None,
                poll=lambda: None,
            )
            success = subprocess.CompletedProcess(
                [], 0, contents, b""
            )
            with mock.patch.object(
                cm4_provision.subprocess, "run", return_value=success
            ) as run:
                digest = cm4_provision.verify_guest_block_prefix(
                    process, target, backend, ["sudo", "-n"], 1.0,
                    length=len(contents),
                )
            self.assertEqual(digest, hashlib.sha256(contents).hexdigest())
            run.assert_called_once_with(
                [
                    "sudo", "-n", "dd", f"if={target}",
                    f"bs={len(contents)}", "count=1", "iflag=direct",
                    "status=none",
                ],
                capture_output=True,
                timeout=mock.ANY,
            )

            mismatch = subprocess.CompletedProcess(
                [], 0, b"wrong!!!", b""
            )
            with mock.patch.object(
                cm4_provision.subprocess, "run", return_value=mismatch
            ), self.assertRaisesRegex(
                cm4_provision.ProvisionError, "block prefix differs"
            ):
                cm4_provision.verify_guest_block_prefix(
                    process, target, backend, [], 1.0,
                    length=len(contents),
                )

    def test_qmp_set_targets_machine_property(self):
        client = object.__new__(cm4_provision.QMPClient)
        with mock.patch.object(
            client, "command", return_value=None
        ) as command:
            client.qom_set("provision-flash-complete", True)
        command.assert_called_once_with(
            "qom-set",
            {
                "path": "/machine",
                "property": "provision-flash-complete",
                "value": True,
            },
        )

    def test_report_is_atomic_json(self):
        with tempfile.TemporaryDirectory() as directory_name:
            directory = Path(directory_name)
            report = directory / "report.json"
            cm4_provision.write_report(report, {"status": "complete"})
            self.assertEqual(
                json.loads(report.read_text(encoding="utf-8")),
                {"status": "complete"},
            )
            self.assertEqual(list(directory.iterdir()), [report])

    def test_stale_rpiboot_recovery_is_explicit_and_verified(self):
        with tempfile.TemporaryDirectory() as directory_name:
            lifecycle = Path(directory_name) / "state"
            lifecycle.write_text("rpiboot-active\n", encoding="ascii")
            args = SimpleNamespace(
                lifecycle=lifecycle,
                raw_gadget=Path("/opt/rpiboot-raw-gadget"),
                sudo=["sudo", "-n"],
                timeout=7.0,
            )

            def complete_recovery(command, **kwargs):
                self.assertEqual(
                    command,
                    [
                        "sudo", "-n", "/opt/rpiboot-raw-gadget",
                        "--lifecycle", str(lifecycle), "--recover-stale",
                    ],
                )
                self.assertEqual(kwargs["timeout"], 7.0)
                lifecycle.write_text("rpiboot-failed\n", encoding="ascii")
                return subprocess.CompletedProcess(
                    command, 0, "recovered\n", ""
                )

            with mock.patch.object(
                cm4_provision.subprocess, "run", side_effect=complete_recovery
            ) as run:
                self.assertTrue(cm4_provision.recover_stale_rpiboot(args))
                run.assert_called_once()

    def test_non_active_rpiboot_is_not_recovered(self):
        with tempfile.TemporaryDirectory() as directory_name:
            lifecycle = Path(directory_name) / "state"
            lifecycle.write_text("rpiboot-complete\n", encoding="ascii")
            args = SimpleNamespace(
                lifecycle=lifecycle,
                raw_gadget=Path("unused"),
                sudo=[],
                timeout=1.0,
            )
            with mock.patch.object(cm4_provision.subprocess, "run") as run:
                self.assertFalse(cm4_provision.recover_stale_rpiboot(args))
                run.assert_not_called()

    def test_failed_stale_rpiboot_recovery_stays_fail_closed(self):
        with tempfile.TemporaryDirectory() as directory_name:
            lifecycle = Path(directory_name) / "state"
            lifecycle.write_text("rpiboot-active\n", encoding="ascii")
            args = SimpleNamespace(
                lifecycle=lifecycle,
                raw_gadget=Path("/rpiboot-raw-gadget"),
                sudo=[],
                timeout=1.0,
            )
            result = subprocess.CompletedProcess([], 9, "", "lock busy\n")
            with mock.patch.object(
                cm4_provision.subprocess, "run", return_value=result
            ), self.assertRaisesRegex(
                cm4_provision.ProvisionError, "cannot recover stale RPIBOOT"
            ):
                cm4_provision.recover_stale_rpiboot(args)
            self.assertEqual(
                lifecycle.read_text(encoding="ascii"), "rpiboot-active\n"
            )


if __name__ == "__main__":
    unittest.main()
