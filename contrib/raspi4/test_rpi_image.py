#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

from pathlib import Path
import binascii
import hashlib
import importlib.util
import io
import json
import os
import stat
import struct
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock
from contextlib import redirect_stdout


MODULE_PATH = Path(__file__).with_name("rpi_image.py")
SPEC = importlib.util.spec_from_file_location("rpi_image", MODULE_PATH)
assert SPEC and SPEC.loader
rpi_image = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(rpi_image)


class FlashImageTests(unittest.TestCase):
    def test_flash_and_verify(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.img"
            target = root / "target.img"
            source.write_bytes(bytes(range(256)) * 33)

            result = rpi_image.flash_image(
                source, target, chunk_size=257, verify=True
            )

            self.assertEqual(target.read_bytes(), source.read_bytes())
            self.assertTrue(result["verified"])
            self.assertEqual(result["bytes_written"], source.stat().st_size)
            self.assertFalse(Path(result["resume_journal"]).exists())
            identity = result["target_identity"]
            self.assertEqual(identity["path"], str(target.resolve()))
            self.assertEqual(identity["kind"], "regular")
            self.assertEqual(identity["inode"], target.stat().st_ino)

    def test_power_loss_leaves_exact_partial_image(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.img"
            target = root / "target.img"
            payload = b"raspberry-pi" * 1000
            source.write_bytes(payload)

            with self.assertRaises(rpi_image.PowerLossInjected):
                rpi_image.flash_image(
                    source, target, chunk_size=1024, fail_after=4097
                )

            self.assertEqual(target.read_bytes(), payload[:4097])
            journal = Path(str(target) + ".rpi-resume.json")
            state = json.loads(journal.read_text(encoding="utf-8"))
            self.assertEqual(state["version"], 1)
            self.assertEqual(state["bytes_written"], 4097)
            self.assertEqual(
                state["prefix_sha256"],
                hashlib.sha256(payload[:4097]).hexdigest(),
            )

    def test_verified_resume_completes_exact_image(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.img"
            target = root / "target.img"
            payload = bytes(range(251)) * 100
            source.write_bytes(payload)

            with self.assertRaises(rpi_image.PowerLossInjected):
                rpi_image.flash_image(
                    source, target, chunk_size=701, fail_after=4097
                )
            result = rpi_image.flash_image(
                source, target, chunk_size=997, resume=True
            )

            self.assertEqual(target.read_bytes(), payload)
            self.assertTrue(result["verified"])
            self.assertTrue(result["resumed"])
            self.assertEqual(result["resume_from"], 4097)
            self.assertFalse(Path(result["resume_journal"]).exists())

    def test_resume_survives_second_interruption(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.img"
            target = root / "target.img"
            payload = b"resume-twice" * 2000
            source.write_bytes(payload)

            with self.assertRaises(rpi_image.PowerLossInjected):
                rpi_image.flash_image(source, target, fail_after=3001)
            with self.assertRaises(rpi_image.PowerLossInjected):
                rpi_image.flash_image(
                    source, target, resume=True, fail_after=9003
                )
            state = json.loads(
                Path(str(target) + ".rpi-resume.json").read_text()
            )
            self.assertEqual(state["bytes_written"], 9003)

            result = rpi_image.flash_image(source, target, resume=True)
            self.assertTrue(result["verified"])
            self.assertEqual(result["resume_from"], 9003)
            self.assertEqual(target.read_bytes(), payload)

    def test_resume_rejects_changed_source(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.img"
            target = root / "target.img"
            source.write_bytes(b"a" * 8192)
            with self.assertRaises(rpi_image.PowerLossInjected):
                rpi_image.flash_image(source, target, fail_after=4096)
            source.write_bytes(b"b" * 8192)

            with self.assertRaisesRegex(rpi_image.ImageError,
                                       "source SHA-256"):
                rpi_image.flash_image(source, target, resume=True)
            self.assertEqual(target.read_bytes(), b"a" * 4096)

    def test_resume_rejects_corrupt_target_prefix(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.img"
            target = root / "target.img"
            source.write_bytes(bytes(range(256)) * 40)
            with self.assertRaises(rpi_image.PowerLossInjected):
                rpi_image.flash_image(source, target, fail_after=5000)
            with target.open("r+b") as stream:
                stream.seek(1234)
                stream.write(b"corrupt")

            with self.assertRaisesRegex(rpi_image.ImageError,
                                       "target prefix"):
                rpi_image.flash_image(source, target, resume=True)

    def test_resume_rejects_replaced_target(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.img"
            target = root / "target.img"
            replacement = root / "replacement.img"
            source.write_bytes(b"identity" * 2000)
            with self.assertRaises(rpi_image.PowerLossInjected):
                rpi_image.flash_image(source, target, fail_after=4096)
            replacement.write_bytes(target.read_bytes())
            replacement.replace(target)

            with self.assertRaisesRegex(rpi_image.ImageError,
                                       "target identity"):
                rpi_image.flash_image(source, target, resume=True)

    def test_verification_rejects_replaced_opened_target(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.img"
            target = root / "target.img"
            source.write_bytes(b"verify-opened-identity" * 100)
            first = {
                "path": str(target.resolve()),
                "device": 1,
                "inode": 100,
                "rdev": 0,
                "kind": "regular",
            }
            replacement = {**first, "inode": 101}
            with mock.patch.object(
                rpi_image, "_target_identity",
                side_effect=[first, replacement],
            ):
                with self.assertRaisesRegex(
                    rpi_image.ImageError,
                    "identity changed before verification",
                ):
                    rpi_image.flash_image(source, target)
            self.assertEqual(target.read_bytes(), source.read_bytes())
            self.assertTrue(
                Path(str(target) + ".rpi-resume.json").exists()
            )

    def test_symlink_target_reports_resolved_opened_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.img"
            target = root / "target.img"
            alias = root / "by-id-media"
            source.write_bytes(b"stable-device-alias" * 100)
            target.write_bytes(b"")
            alias.symlink_to(target)

            result = rpi_image.flash_image(source, alias)

            self.assertEqual(result["target"], str(alias))
            self.assertEqual(
                result["target_identity"]["path"], str(target.resolve())
            )
            self.assertEqual(
                result["target_identity"]["inode"], target.stat().st_ino
            )

    def test_stale_or_malformed_journal_fails_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.img"
            target = root / "target.img"
            journal = Path(str(target) + ".rpi-resume.json")
            source.write_bytes(b"journal" * 1000)
            target.write_bytes(b"")
            journal.write_text("not-json", encoding="utf-8")

            with self.assertRaisesRegex(rpi_image.ImageError,
                                       "journal already exists"):
                rpi_image.flash_image(source, target)
            with self.assertRaisesRegex(rpi_image.ImageError,
                                       "cannot read resume journal"):
                rpi_image.flash_image(source, target, resume=True)

    def test_resume_requires_final_verification(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.img"
            target = root / "target.img"
            source.write_bytes(b"verify" * 1000)
            with self.assertRaises(rpi_image.PowerLossInjected):
                rpi_image.flash_image(source, target, fail_after=512)

            with self.assertRaisesRegex(rpi_image.ImageError,
                                       "requires final"):
                rpi_image.flash_image(
                    source, target, resume=True, verify=False
                )

    @unittest.skipIf(rpi_image.fcntl is None, "requires POSIX flock")
    def test_concurrent_target_owner_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.img"
            target = root / "target.img"
            source.write_bytes(b"exclusive" * 1000)
            target.write_bytes(b"")

            with target.open("r+b") as owner:
                rpi_image.fcntl.flock(
                    owner.fileno(),
                    rpi_image.fcntl.LOCK_EX | rpi_image.fcntl.LOCK_NB,
                )
                with self.assertRaisesRegex(rpi_image.ImageError,
                                           "another flash process"):
                    rpi_image.flash_image(source, target)

    def test_cli_interruption_then_resume(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.img"
            target = root / "target.img"
            source.write_bytes(bytes(range(253)) * 100)

            output = io.StringIO()
            with redirect_stdout(output):
                status = rpi_image.main([
                    "flash", str(source), str(target),
                    "--chunk-size", "777", "--fail-after", "5001",
                ])
            self.assertEqual(status, rpi_image.POWER_LOSS_EXIT)
            self.assertEqual(json.loads(output.getvalue())["status"],
                             "power-loss")

            output = io.StringIO()
            with redirect_stdout(output):
                status = rpi_image.main([
                    "flash", str(source), str(target), "--resume",
                ])
            self.assertEqual(status, 0)
            result = json.loads(output.getvalue())
            self.assertTrue(result["verified"])
            self.assertTrue(result["resumed"])
            self.assertEqual(result["resume_from"], 5001)
            self.assertEqual(
                result["target_identity"]["path"], str(target.resolve())
            )

    def test_same_file_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "image"
            image.write_bytes(b"data")
            with self.assertRaises(rpi_image.ImageError):
                rpi_image.flash_image(image, image)

    def test_size_suffixes_are_binary(self):
        self.assertEqual(rpi_image._positive_int("4MiB"), 4 * 1024 * 1024)
        self.assertEqual(rpi_image._non_negative_int("0"), 0)


class BlockDevicePreflightTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.sysfs = self.root / "sys"
        self.node = self.sysfs / "devices" / "mock" / "block" / "sdz"
        self.partition = self.node / "sdz1"
        self.dev_block = self.sysfs / "dev" / "block"
        self.node.mkdir(parents=True)
        self.partition.mkdir()
        self.dev_block.mkdir(parents=True)
        (self.dev_block / "8:0").symlink_to(self.node)
        (self.dev_block / "8:1").symlink_to(self.partition)
        (self.node / "removable").write_text("1\n", encoding="ascii")
        (self.node / "ro").write_text("0\n", encoding="ascii")
        (self.node / "size").write_text("16384\n", encoding="ascii")
        (self.partition / "partition").write_text("1\n", encoding="ascii")
        self.mountinfo = self.root / "mountinfo"
        self.mountinfo.write_text(
            "24 1 0:22 / / rw - tmpfs tmpfs rw\n", encoding="utf-8"
        )
        self.proc = self.root / "proc"
        self.namespace_files = self.root / "namespace-files"
        self.namespace_files.mkdir()
        (self.namespace_files / "100").write_bytes(b"")
        (self.proc / "self" / "ns").mkdir(parents=True)
        os.link(
            self.namespace_files / "100",
            self.proc / "self" / "ns" / "mnt",
        )
        self.swaps = self.root / "swaps"
        self.swaps.write_text(
            "Filename Type Size Used Priority\n", encoding="utf-8"
        )

    def tearDown(self):
        self.temporary.cleanup()

    def validate(self, major=8, minor=0, size=4 * 1024 * 1024,
                 proc_root=None):
        return rpi_image._validate_linux_block_device(
            major, minor, size, sysfs_root=self.sysfs,
            mountinfo_path=self.mountinfo, swaps_path=self.swaps,
            proc_root=proc_root,
        )

    def add_mount_namespace(self, pid, namespace, mountinfo):
        process = self.proc / str(pid)
        (process / "ns").mkdir(parents=True)
        identity = self.namespace_files / str(namespace)
        identity.touch(exist_ok=True)
        os.link(identity, process / "ns" / "mnt")
        (process / "mountinfo").write_text(mountinfo, encoding="utf-8")

    def test_whole_unmounted_removable_media_passes(self):
        self.assertEqual(self.validate(), 8 * 1024 * 1024)

    def test_block_target_evidence_uses_resolved_sysfs_identity(self):
        evidence = rpi_image._linux_block_target_evidence(
            8, 0, 8 * 1024 * 1024, sysfs_root=self.sysfs
        )
        self.assertEqual(
            evidence,
            {
                "major": 8,
                "minor": 0,
                "kernel_name": "sdz",
                "sysfs_path": str(self.node.resolve()),
                "capacity": 8 * 1024 * 1024,
                "logical_sector_size": 512,
                "removable": True,
                "read_only": False,
            },
        )

    def test_partition_target_is_rejected(self):
        with self.assertRaisesRegex(rpi_image.ImageError, "a partition"):
            self.validate(minor=1)

    def test_fixed_or_read_only_media_is_rejected(self):
        (self.node / "removable").write_text("0\n", encoding="ascii")
        with self.assertRaisesRegex(rpi_image.ImageError, "not removable"):
            self.validate()
        (self.node / "removable").write_text("1\n", encoding="ascii")
        (self.node / "ro").write_text("1\n", encoding="ascii")
        with self.assertRaisesRegex(rpi_image.ImageError, "read-only"):
            self.validate()

    def test_undersized_media_is_rejected_before_write(self):
        with self.assertRaisesRegex(rpi_image.ImageError, "smaller than"):
            self.validate(size=9 * 1024 * 1024)

    def test_mounted_partition_rejects_whole_device(self):
        self.mountinfo.write_text(
            "36 25 8:1 / /mnt/pi rw - ext4 /dev/sdz1 rw\n",
            encoding="utf-8",
        )
        with self.assertRaisesRegex(rpi_image.ImageError, "8:1 is mounted"):
            self.validate()

    def test_malformed_mountinfo_fails_closed(self):
        self.mountinfo.write_text("malformed\n", encoding="utf-8")
        with self.assertRaisesRegex(rpi_image.ImageError, "malformed"):
            self.validate()

    def test_mount_in_other_namespace_rejects_whole_device(self):
        self.add_mount_namespace(
            101, 200,
            "36 25 8:1 / /container/pi rw - ext4 /dev/sdz1 rw\n",
        )
        with self.assertRaisesRegex(
            rpi_image.ImageError, "8:1 is mounted"
        ):
            self.validate(proc_root=self.proc)

    def test_duplicate_mount_namespaces_are_inspected_once(self):
        self.add_mount_namespace(
            101, 200, "24 1 0:22 / / rw - tmpfs tmpfs rw\n"
        )
        self.add_mount_namespace(102, 200, "malformed\n")
        self.assertEqual(
            self.validate(proc_root=self.proc), 8 * 1024 * 1024
        )

    def test_malformed_other_namespace_mountinfo_fails_closed(self):
        self.add_mount_namespace(101, 200, "malformed\n")
        with self.assertRaisesRegex(
            rpi_image.ImageError, "process 101.*malformed"
        ):
            self.validate(proc_root=self.proc)

    def test_missing_mount_namespace_identity_fails_closed(self):
        process = self.proc / "101"
        (process / "ns").mkdir(parents=True)
        (process / "ns" / "mnt").symlink_to("not-a-mount-namespace")
        (process / "mountinfo").write_text(
            "24 1 0:22 / / rw - tmpfs tmpfs rw\n", encoding="utf-8"
        )
        with self.assertRaisesRegex(
            rpi_image.ImageError, "namespace is absent"
        ):
            self.validate(proc_root=self.proc)

    def test_mount_namespace_enumeration_is_bounded(self):
        self.add_mount_namespace(
            101, 200, "24 1 0:22 / / rw - tmpfs tmpfs rw\n"
        )
        with mock.patch.object(rpi_image, "MAX_MOUNT_NAMESPACES", 0):
            with self.assertRaisesRegex(
                rpi_image.ImageError, "process count exceeds"
            ):
                self.validate(proc_root=self.proc)

    def test_changing_mount_namespace_fails_closed(self):
        self.add_mount_namespace(
            101, 200, "24 1 0:22 / / rw - tmpfs tmpfs rw\n"
        )
        with mock.patch.object(
            rpi_image, "_mount_namespace_key",
            side_effect=["1:200", "1:201"] * 3,
        ):
            with self.assertRaisesRegex(
                rpi_image.ImageError, "namespace is unstable"
            ):
                rpi_image._namespace_mounted_device_numbers(
                    self.proc / "101", "changing mount namespace"
                )

    def test_active_swap_partition_rejects_whole_device(self):
        with mock.patch.object(
            rpi_image, "_swap_device_numbers", return_value={(8, 1)}
        ):
            with self.assertRaisesRegex(rpi_image.ImageError, "active swap"):
                self.validate()

    def test_raw_swap_path_resolves_device_numbers(self):
        self.swaps.write_text(
            "Filename Type Size Used Priority\n"
            "/dev/sdz1 partition 1024 0 -2\n",
            encoding="utf-8",
        )
        info = SimpleNamespace(
            st_mode=stat.S_IFBLK, st_rdev=os.makedev(8, 1)
        )
        with mock.patch.object(rpi_image.os, "stat", return_value=info):
            self.assertEqual(rpi_image._swap_device_numbers(self.swaps),
                             {(8, 1)})

    def test_malformed_swap_metadata_fails_closed(self):
        self.swaps.write_text("not-a-header\n", encoding="utf-8")
        with self.assertRaisesRegex(rpi_image.ImageError, "swap.*malformed"):
            self.validate()


class InspectImageTests(unittest.TestCase):
    def _make_mbr_image(self, path: Path):
        image = bytearray(16 * 1024 * 1024)
        struct.pack_into("<I", image, 440, 0x1234ABCD)
        entry = bytearray(16)
        entry[4] = 0x06
        struct.pack_into("<II", entry, 8, 2048, 8192)
        image[446:462] = entry
        entry = bytearray(16)
        entry[4] = 0x83
        struct.pack_into("<II", entry, 8, 12288, 8192)
        image[462:478] = entry
        image[510:512] = b"\x55\xaa"

        boot = 2048 * rpi_image.SECTOR_SIZE
        struct.pack_into("<H", image, boot + 11, 512)
        image[boot + 13] = 1
        struct.pack_into("<H", image, boot + 14, 1)
        image[boot + 16] = 1
        struct.pack_into("<H", image, boot + 17, 32)
        struct.pack_into("<H", image, boot + 19, 8192)
        image[boot + 21] = 0xF8
        struct.pack_into("<H", image, boot + 22, 32)
        image[boot + 38] = 0x29
        struct.pack_into("<I", image, boot + 39, 0xA1B2C3D4)
        image[boot + 43:boot + 54] = b"boot       "
        image[boot + 54:boot + 62] = b"FAT16   "
        image[boot + 510:boot + 512] = b"\x55\xaa"
        fat = boot + rpi_image.SECTOR_SIZE
        struct.pack_into("<H", image, fat + 4, 0xFFFF)
        root = boot + 33 * rpi_image.SECTOR_SIZE
        image[root:root + 11] = b"CMDLINE TXT"
        image[root + 11] = 0x20
        struct.pack_into("<H", image, root + 26, 2)
        cmdline = b"console=tty1 root=PARTUUID=1234abcd-02 rw\n"
        struct.pack_into("<I", image, root + 28, len(cmdline))
        data = boot + 35 * rpi_image.SECTOR_SIZE
        image[data:data + len(cmdline)] = cmdline

        ext_start = 12288 * rpi_image.SECTOR_SIZE
        ext = ext_start + 1024
        struct.pack_into("<I", image, ext + 24, 0)
        struct.pack_into("<I", image, ext + 40, 128)
        image[ext + 56:ext + 58] = b"\x53\xef"
        struct.pack_into("<H", image, ext + 88, 128)
        struct.pack_into("<I", image, ext + 96, 0x40)
        image[ext + 104:ext + 120] = bytes(range(16))
        image[ext + 120:ext + 126] = b"rootfs"
        group = ext_start + 2 * 1024
        struct.pack_into("<I", image, group + 8, 5)

        def extent_inode(number, mode, physical, size):
            inode = ext_start + 5 * 1024 + (number - 1) * 128
            struct.pack_into("<H", image, inode, mode)
            struct.pack_into("<I", image, inode + 4, size)
            struct.pack_into("<I", image, inode + 32, 0x80000)
            struct.pack_into("<HHHHI", image, inode + 40,
                             0xF30A, 1, 4, 0, 0)
            struct.pack_into("<IHHI", image, inode + 52,
                             0, 1, 0, physical)

        def directory_block(entries):
            block = bytearray(1024)
            offset = 0
            for index, (number, name, file_type) in enumerate(entries):
                encoded = name.encode("ascii")
                length = (8 + len(encoded) + 3) & ~3
                if index == len(entries) - 1:
                    length = 1024 - offset
                struct.pack_into("<IHBB", block, offset, number, length,
                                 len(encoded), file_type)
                block[offset + 8:offset + 8 + len(encoded)] = encoded
                offset += length
            return block

        extent_inode(2, 0x41ED, 20, 1024)
        extent_inode(12, 0x41ED, 21, 1024)
        fstab = (
            b"PARTUUID=1234abcd-01 /boot vfat defaults 0 2\n"
            b"UUID=00010203-0405-0607-0809-0a0b0c0d0e0f / ext4 defaults 0 1\n"
        )
        extent_inode(13, 0x81A4, 22, len(fstab))
        image[ext_start + 20 * 1024:ext_start + 21 * 1024] = \
            directory_block([(2, ".", 2), (2, "..", 2), (12, "etc", 2)])
        image[ext_start + 21 * 1024:ext_start + 22 * 1024] = \
            directory_block([(12, ".", 2), (2, "..", 2),
                             (13, "fstab", 1)])
        image[ext_start + 22 * 1024:
              ext_start + 22 * 1024 + len(fstab)] = fstab
        path.write_bytes(image)

    def test_mbr_and_fat_are_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "mbr.img"
            self._make_mbr_image(image)

            result = rpi_image.inspect_image(image)

            self.assertEqual(result["partition_table"], "mbr")
            self.assertEqual(result["partitions"][0]["filesystem"], "fat")
            self.assertTrue(result["partitions"][0]["in_bounds"])
            self.assertEqual(result["partitions"][0]["partition_uuid"],
                             "1234abcd-01")
            self.assertEqual(result["partitions"][0]["filesystem_uuid"],
                             "A1B2-C3D4")
            self.assertEqual(result["partitions"][0]["filesystem_label"],
                             "boot")
            self.assertEqual(result["partitions"][1]["filesystem"], "ext")
            self.assertEqual(result["partitions"][1]["filesystem_uuid"],
                             "00010203-0405-0607-0809-0a0b0c0d0e0f")
            self.assertEqual(result["partitions"][1]["filesystem_label"],
                             "rootfs")

    def test_boot_and_fstab_identities_must_resolve(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "mbr.img"
            self._make_mbr_image(image)

            result = rpi_image.inspect_image(image, {
                "cmdline.txt": "console=tty1 root=PARTUUID=1234ABCD-02 rw",
                "fstab": (
                    "UUID=A1B2-C3D4 /boot/firmware vfat defaults 0 2\n"
                    "LABEL=rootfs / ext4 defaults 0 1\n"
                    "# PARTUUID=deadbeef-01 /old ext4 defaults 0 0\n"
                ),
            })

            validation = result["identity_validation"]
            self.assertTrue(validation["valid"])
            self.assertEqual(
                [item["partition_indexes"] for item in
                 validation["references"]], [[2], [1], [2]])

            with self.assertRaisesRegex(rpi_image.ImageError,
                                       "unknown PARTUUID"):
                rpi_image.inspect_image(
                    image, {"cmdline.txt": "root=PARTUUID=deadbeef-02"})

            with image.open("r+b") as stream:
                stream.seek(2048 * rpi_image.SECTOR_SIZE + 43)
                stream.write(b"rootfs     ")
            with self.assertRaisesRegex(rpi_image.ImageError,
                                       "ambiguous LABEL"):
                rpi_image.inspect_image(image, {"fstab": "LABEL=rootfs /"})

    def test_inspect_cli_validates_reference_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image = root / "mbr.img"
            cmdline = root / "cmdline.txt"
            fstab = root / "fstab"
            self._make_mbr_image(image)
            cmdline.write_text(
                "root=PARTUUID=1234abcd-02 rw\n", encoding="utf-8")
            fstab.write_text(
                "PARTUUID=1234abcd-01 /boot vfat defaults 0 2\n",
                encoding="utf-8")

            output = io.StringIO()
            with redirect_stdout(output):
                status = rpi_image.main([
                    "inspect", str(image), "--boot-config", str(cmdline),
                    "--fstab", str(fstab),
                ])

            self.assertEqual(status, 0)
            report = json.loads(output.getvalue())
            self.assertTrue(report["identity_validation"]["valid"])

    def test_inspect_reads_contained_cmdline_and_fstab(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "mbr.img"
            self._make_mbr_image(image)

            result = rpi_image.inspect_image(image, validate_contained=True)

            references = result["identity_validation"]["references"]
            self.assertEqual(len(references), 3)
            self.assertEqual(
                {reference["source"] for reference in references},
                {"partition-1:/cmdline.txt", "partition-2:/etc/fstab"})
            self.assertEqual(
                [reference["partition_indexes"] for reference in references],
                [[2], [1], [2]])

            output = io.StringIO()
            with redirect_stdout(output):
                status = rpi_image.main([
                    "inspect", str(image),
                    "--validate-contained-identities",
                ])
            self.assertEqual(status, 0)
            self.assertTrue(json.loads(output.getvalue())
                            ["identity_validation"]["valid"])

            boot = 2048 * rpi_image.SECTOR_SIZE
            with image.open("r+b") as stream:
                stream.seek(boot + 33 * rpi_image.SECTOR_SIZE + 28)
                original_size = stream.read(4)
                stream.seek(boot + 33 * rpi_image.SECTOR_SIZE + 28)
                stream.write(struct.pack("<I", 513))
                stream.seek(boot + rpi_image.SECTOR_SIZE + 4)
                stream.write(struct.pack("<H", 2))
            with self.assertRaisesRegex(rpi_image.ImageError, "FAT.*loop"):
                rpi_image.inspect_image(image, validate_contained=True)

            with image.open("r+b") as stream:
                stream.seek(boot + 33 * rpi_image.SECTOR_SIZE + 28)
                stream.write(original_size)
                stream.seek(boot + rpi_image.SECTOR_SIZE + 4)
                stream.write(struct.pack("<H", 0xFFFF))
                root_inode = 12288 * rpi_image.SECTOR_SIZE + 5 * 1024 + 128
                stream.seek(root_inode + 40)
                stream.write(b"\0\0")
            with self.assertRaisesRegex(rpi_image.ImageError,
                                       "extent header"):
                rpi_image.inspect_image(image, validate_contained=True)

    def test_gpt_crcs_and_partition_are_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "gpt.img"
            sector_count = 8192
            image = bytearray(sector_count * rpi_image.SECTOR_SIZE)

            protective = bytearray(16)
            protective[4] = 0xEE
            struct.pack_into("<II", protective, 8, 1, sector_count - 1)
            image[446:462] = protective
            image[510:512] = b"\x55\xaa"

            entries = bytearray(128 * 128)
            entries[:16] = bytes.fromhex(
                "28732ac11ff8d211ba4b00a0c93ec93b"
            )
            entries[16:32] = bytes(range(16))
            struct.pack_into("<QQ", entries, 32, 2048, 4095)
            entries[56:64] = "boot".encode("utf-16-le")
            entries_crc = binascii.crc32(entries) & 0xFFFFFFFF
            image[2 * 512:2 * 512 + len(entries)] = entries

            header = bytearray(512)
            header[:8] = b"EFI PART"
            struct.pack_into("<IIII", header, 8, 0x00010000, 92, 0, 0)
            struct.pack_into("<QQQQ", header, 24, 1, sector_count - 1,
                             34, sector_count - 34)
            header[56:72] = bytes(reversed(range(16)))
            struct.pack_into("<QIII", header, 72, 2, 128, 128, entries_crc)
            header_crc = binascii.crc32(header[:92]) & 0xFFFFFFFF
            struct.pack_into("<I", header, 16, header_crc)
            image[512:1024] = header
            path.write_bytes(image)

            result = rpi_image.inspect_image(path)

            self.assertEqual(result["partition_table"], "gpt")
            self.assertTrue(result["header_crc_valid"])
            self.assertTrue(result["entries_crc_valid"])
            self.assertEqual(result["partitions"][0]["name"], "boot")
            self.assertTrue(result["partitions"][0]["in_bounds"])
            self.assertEqual(result["partitions"][0]["partition_label"],
                             "boot")
            self.assertEqual(result["partitions"][0]["partition_uuid"],
                             "03020100-0504-0706-0809-0a0b0c0d0e0f")

    def test_boot_order_is_low_nibble_first(self):
        self.assertEqual(
            list(rpi_image.decode_boot_order(0xF41)),
            ["sd-card", "usb-msd", "restart"],
        )


if __name__ == "__main__":
    unittest.main()
