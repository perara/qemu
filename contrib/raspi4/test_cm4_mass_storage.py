#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Unit tests for the guarded CM4 mass-storage export helper."""

from pathlib import Path
import importlib.util
import io
import tempfile
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("cm4_mass_storage.py")
SPEC = importlib.util.spec_from_file_location("cm4_mass_storage", MODULE_PATH)
assert SPEC and SPEC.loader
cm4_mass_storage = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(cm4_mass_storage)


class Cm4MassStorageTests(unittest.TestCase):
    def test_aligned_regular_image_is_accepted(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "emmc.img"
            image.write_bytes(bytes(1024))
            self.assertEqual(cm4_mass_storage.validate_image(str(image)), image)

    def test_zero_length_image_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "emmc.img"
            image.touch()
            with self.assertRaisesRegex(
                    cm4_mass_storage.GadgetError, "non-zero"):
                cm4_mass_storage.validate_image(str(image))

    def test_unaligned_image_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "emmc.img"
            image.write_bytes(bytes(513))
            with self.assertRaisesRegex(
                    cm4_mass_storage.GadgetError, "512-byte"):
                cm4_mass_storage.validate_image(str(image))

    def test_directory_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(
                    cm4_mass_storage.GadgetError, "regular file"):
                cm4_mass_storage.validate_image(directory)

    def test_loaded_kernel_module_skips_modprobe(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            parameters = root / "dummy_hcd" / "parameters"
            parameters.mkdir(parents=True)
            (parameters / "is_high_speed").write_text(
                "Y\n", encoding="ascii"
            )
            with mock.patch.object(cm4_mass_storage.subprocess, "run") as run:
                cm4_mass_storage.ensure_kernel_module(
                    "dummy_hcd", "is_high_speed=1", module_root=root
                )
            run.assert_not_called()

    def test_loaded_kernel_module_parameter_mismatch_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            parameters = root / "dummy_hcd" / "parameters"
            parameters.mkdir(parents=True)
            (parameters / "is_high_speed").write_text(
                "N\n", encoding="ascii"
            )
            with self.assertRaisesRegex(
                cm4_mass_storage.GadgetError,
                "parameter is_high_speed differs",
            ):
                cm4_mass_storage.ensure_kernel_module(
                    "dummy_hcd", "is_high_speed=1", module_root=root
                )

    def test_stable_by_id_path(self):
        self.assertEqual(
            cm4_mass_storage.by_id_path(Path("/dev")),
            Path("/dev/disk/by-id/usb-Linux_File-Stor_Gadget_"
                 "51554d5552504934-0:0"),
        )

    def test_stable_by_id_discovers_whole_disk_and_ignores_partitions(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            by_id = root / "disk/by-id"
            by_id.mkdir(parents=True)
            disk = by_id / (
                "usb-mmcblk0_51554d5552504934-0:0"
            )
            disk.touch()
            (by_id / f"{disk.name}-part1").touch()
            (by_id / f"{disk.name}-part2").touch()
            self.assertEqual(cm4_mass_storage.by_id_path(root), disk)

    def test_stable_by_id_rejects_multiple_whole_disks(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            by_id = root / "disk/by-id"
            by_id.mkdir(parents=True)
            (by_id / "usb-a_51554d5552504934-0:0").touch()
            (by_id / "usb-b_51554d5552504934-0:0").touch()
            with self.assertRaisesRegex(
                cm4_mass_storage.GadgetError, "multiple whole-disk"
            ):
                cm4_mass_storage.by_id_path(root)

    def test_mounted_export_is_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            mountinfo = Path(directory) / "mountinfo"
            mountinfo.write_text(
                "77 55 8:113 / /media/boot rw - vfat /dev/sdh1 rw\n"
                "78 55 259:2 / /home rw - ext4 /dev/nvme1n1p2 rw\n",
                encoding="utf-8",
            )
            with mock.patch.object(
                cm4_mass_storage, "exported_block_names",
                return_value={"sdh", "sdh1"}
            ):
                self.assertEqual(
                    cm4_mass_storage.mounted_exports(Path("/dev"), mountinfo),
                    ["/dev/sdh1 on /media/boot"],
                )

    def test_fault_watch_detects_block_write_threshold(self):
        device = Path("/dev/sdz")
        with mock.patch.object(
            cm4_mass_storage, "sectors_written", side_effect=[11, 19]
        ):
            observed = cm4_mass_storage.wait_for_block_writes(
                device, Path("/sys/class/block"), 10, 8, 1.0,
                poll_interval=0
            )
        self.assertEqual(observed, 19)

    def test_fault_watch_rejects_zero_threshold(self):
        with self.assertRaisesRegex(
            cm4_mass_storage.GadgetError, "threshold must be positive"
        ):
            cm4_mass_storage.wait_for_block_writes(
                Path("/dev/sdz"), Path("/sys/class/block"), 0, 0, 1.0
            )

    def test_flush_counter_uses_linux_block_stat_field(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            stat_file = root / "sdz" / "stat"
            stat_file.parent.mkdir()
            fields = list(range(1, 18))
            stat_file.write_text(
                " ".join(str(field) for field in fields) + "\n",
                encoding="ascii",
            )
            self.assertEqual(
                cm4_mass_storage.flushes_completed(Path("/dev/sdz"), root),
                16,
            )

    def test_fault_watch_detects_block_flush_threshold(self):
        device = Path("/dev/sdz")
        with mock.patch.object(
            cm4_mass_storage, "flushes_completed", side_effect=[4, 5]
        ):
            observed = cm4_mass_storage.wait_for_block_flushes(
                device, Path("/sys/class/block"), 3, 2, 1.0,
                poll_interval=0,
            )
        self.assertEqual(observed, 5)

    def test_fault_flush_watch_rejects_zero_threshold(self):
        with self.assertRaisesRegex(
            cm4_mass_storage.GadgetError, "flush threshold must be positive"
        ):
            cm4_mass_storage.wait_for_block_flushes(
                Path("/dev/sdz"), Path("/sys/class/block"), 0, 0, 1.0
            )

    def test_force_eject_uses_scsi_medium_control(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            eject = (root / cm4_mass_storage.GADGET_NAME /
                     "functions/mass_storage.usb0/lun.0/forced_eject")
            eject.parent.mkdir(parents=True)
            eject.touch()
            cm4_mass_storage.force_eject(root)
            self.assertEqual(eject.read_text(encoding="ascii"), "1")

    def test_ejected_gadget_remains_active_without_backing_medium(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            gadget = root / cm4_mass_storage.GADGET_NAME
            lun_file = gadget / "functions/mass_storage.usb0/lun.0/file"
            lun_file.parent.mkdir(parents=True)
            lun_file.touch()
            self.assertTrue(cm4_mass_storage.gadget_is_active(root))
            self.assertIsNone(cm4_mass_storage.backing_image(root))

    def test_lifecycle_transition_is_published_atomically(self):
        with tempfile.TemporaryDirectory() as directory:
            lifecycle = Path(directory) / "cm4.state"
            lifecycle.write_text("rpiboot-complete\n", encoding="ascii")
            lifecycle.chmod(0o600)
            cm4_mass_storage.transition_lifecycle(
                lifecycle, {"rpiboot-complete"}, "mass-storage-active"
            )
            self.assertEqual(
                cm4_mass_storage.read_lifecycle(lifecycle),
                "mass-storage-active",
            )
            self.assertEqual(list(Path(directory).iterdir()), [lifecycle])
            self.assertEqual(lifecycle.stat().st_mode & 0o777, 0o600)

    def test_lifecycle_rejects_out_of_order_owner(self):
        with tempfile.TemporaryDirectory() as directory:
            lifecycle = Path(directory) / "cm4.state"
            lifecycle.write_text("qemu-owned\n", encoding="ascii")
            with self.assertRaisesRegex(
                cm4_mass_storage.GadgetError, "cannot transition"
            ):
                cm4_mass_storage.transition_lifecycle(
                    lifecycle, {"rpiboot-complete"}, "mass-storage-active"
                )
            self.assertEqual(lifecycle.read_text(encoding="ascii"),
                             "qemu-owned\n")

    def test_lifecycle_lock_uses_sibling_file(self):
        with tempfile.TemporaryDirectory() as directory:
            lifecycle = Path(directory) / "cm4.state"
            lifecycle.write_text("boot-ready\n", encoding="ascii")
            fd = cm4_mass_storage.acquire_lifecycle_lock(lifecycle)
            try:
                self.assertTrue(Path(f"{lifecycle}.lock").is_file())
            finally:
                cm4_mass_storage.os.close(fd)

    def test_lifecycle_lock_rejects_active_owner(self):
        with tempfile.TemporaryDirectory() as directory:
            lifecycle = Path(directory) / "cm4.state"
            lifecycle.write_text("boot-ready\n", encoding="ascii")
            with mock.patch.object(
                cm4_mass_storage.fcntl, "lockf",
                side_effect=BlockingIOError,
            ):
                with self.assertRaisesRegex(
                    cm4_mass_storage.GadgetError, "owned by another process"
                ):
                    cm4_mass_storage.acquire_lifecycle_lock(lifecycle)

    def test_foreground_owner_releases_only_after_complete(self):
        with tempfile.TemporaryDirectory() as directory:
            lifecycle = Path(directory) / "cm4.state"
            lifecycle.write_text("rpiboot-complete\n", encoding="ascii")
            image = Path(directory) / "emmc.img"
            image.write_bytes(bytes(512))
            output = io.StringIO()
            calls = []

            with mock.patch.object(
                cm4_mass_storage, "create_gadget",
                side_effect=lambda *_args: calls.append("create"),
            ), mock.patch.object(
                cm4_mass_storage, "wait_for_block_device",
                side_effect=lambda *_args: calls.append("enumerate") or
                Path("/dev/disk/by-id/cm4"),
            ), mock.patch.object(
                cm4_mass_storage, "destroy_gadget",
                side_effect=lambda *_args, **_kwargs: calls.append("destroy"),
            ):
                cm4_mass_storage.serve_gadget(
                    Path(directory) / "configfs", Path("/dev"), image,
                    "dummy_udc.0", 1.0, lifecycle,
                    input_stream=io.StringIO("complete\n"), output=output,
                )

            self.assertEqual(calls, ["create", "enumerate", "destroy"])
            self.assertEqual(cm4_mass_storage.read_lifecycle(lifecycle),
                             "boot-ready")
            self.assertIn("owner ready", output.getvalue())

    def test_foreground_owner_eof_fails_closed_after_teardown(self):
        with tempfile.TemporaryDirectory() as directory:
            lifecycle = Path(directory) / "cm4.state"
            lifecycle.write_text("rpiboot-complete\n", encoding="ascii")
            image = Path(directory) / "emmc.img"
            image.write_bytes(bytes(512))

            with mock.patch.object(
                cm4_mass_storage, "create_gadget"
            ), mock.patch.object(
                cm4_mass_storage, "wait_for_block_device",
                return_value=Path("/dev/disk/by-id/cm4"),
            ), mock.patch.object(
                cm4_mass_storage, "destroy_gadget"
            ) as destroy:
                with self.assertRaisesRegex(
                    cm4_mass_storage.GadgetError,
                    "requires exactly 'complete' or 'failed'",
                ):
                    cm4_mass_storage.serve_gadget(
                        Path(directory) / "configfs", Path("/dev"), image,
                        "dummy_udc.0", 1.0, lifecycle,
                        input_stream=io.StringIO(""), output=io.StringIO(),
                    )

            destroy.assert_called_once()
            self.assertEqual(cm4_mass_storage.read_lifecycle(lifecycle),
                             "flash-failed")

    def test_recover_stale_active_owner_tears_down_and_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            lifecycle = root / "cm4.state"
            lifecycle.write_text("mass-storage-active\n", encoding="ascii")
            image = root / "emmc.img"
            gadget = root / cm4_mass_storage.GADGET_NAME
            gadget.mkdir()

            with mock.patch.object(
                cm4_mass_storage, "backing_image", return_value=image,
            ), mock.patch.object(cm4_mass_storage, "destroy_gadget") as destroy:
                recovered = cm4_mass_storage.recover_stale_gadget(
                    root, Path("/dev"), lifecycle
                )

            self.assertEqual(recovered, image)
            destroy.assert_called_once_with(root, dev_root=Path("/dev"))
            self.assertEqual(cm4_mass_storage.read_lifecycle(lifecycle),
                             "flash-failed")

    def test_recover_stale_after_teardown_still_fails_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            lifecycle = root / "cm4.state"
            lifecycle.write_text("mass-storage-active\n", encoding="ascii")

            self.assertIsNone(cm4_mass_storage.recover_stale_gadget(
                root, Path("/dev"), lifecycle
            ))
            self.assertEqual(cm4_mass_storage.read_lifecycle(lifecycle),
                             "flash-failed")

    def test_recover_stale_teardown_failure_retains_active_state(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            lifecycle = root / "cm4.state"
            lifecycle.write_text("mass-storage-active\n", encoding="ascii")
            (root / cm4_mass_storage.GADGET_NAME).mkdir()

            with mock.patch.object(
                cm4_mass_storage, "backing_image", return_value=None,
            ), mock.patch.object(
                cm4_mass_storage, "destroy_gadget",
                side_effect=cm4_mass_storage.GadgetError("still mounted"),
            ):
                with self.assertRaisesRegex(
                    cm4_mass_storage.GadgetError, "still mounted"
                ):
                    cm4_mass_storage.recover_stale_gadget(
                        root, Path("/dev"), lifecycle
                    )

            self.assertEqual(lifecycle.read_text(encoding="ascii"),
                             "mass-storage-active\n")

    def test_recover_stale_rejects_non_storage_owner(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            lifecycle = root / "cm4.state"
            lifecycle.write_text("qemu-owned\n", encoding="ascii")

            with self.assertRaisesRegex(
                cm4_mass_storage.GadgetError,
                "requires lifecycle state 'mass-storage-active'",
            ):
                cm4_mass_storage.recover_stale_gadget(
                    root, Path("/dev"), lifecycle
                )
            self.assertEqual(lifecycle.read_text(encoding="ascii"),
                             "qemu-owned\n")

    def test_recover_stale_cannot_race_live_owner(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            lifecycle = root / "cm4.state"
            lifecycle.write_text("mass-storage-active\n", encoding="ascii")
            stderr = io.StringIO()

            with mock.patch.object(
                cm4_mass_storage, "require_root"
            ), mock.patch.object(
                cm4_mass_storage, "load_kernel_support"
            ), mock.patch.object(
                cm4_mass_storage, "acquire_lifecycle_lock",
                side_effect=cm4_mass_storage.GadgetError(
                    "lifecycle is owned by another process"
                ),
            ), mock.patch.object(
                cm4_mass_storage, "recover_stale_gadget"
            ) as recover, mock.patch.object(
                cm4_mass_storage.sys, "stderr", stderr
            ):
                result = cm4_mass_storage.main([
                    "--configfs-root", str(root),
                    "--lifecycle", str(lifecycle),
                    "recover-stale",
                ])

            self.assertEqual(result, 1)
            recover.assert_not_called()
            self.assertIn("owned by another process", stderr.getvalue())
            self.assertEqual(lifecycle.read_text(encoding="ascii"),
                             "mass-storage-active\n")


if __name__ == "__main__":
    unittest.main()
