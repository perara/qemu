#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from pathlib import Path
import tempfile
import unittest

try:
    from contrib.raspi4.release_manifest import (
        ManifestError,
        create_manifest,
        load_manifest,
        verify_artifacts,
        _write_manifest,
    )
except ModuleNotFoundError:
    from release_manifest import (
        ManifestError,
        create_manifest,
        load_manifest,
        verify_artifacts,
        _write_manifest,
    )


COMMIT = "1" * 40
BASELINE = "2" * 40


class ReleaseManifestTests(unittest.TestCase):
    def test_create_load_and_verify(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "qemu-rpi4.tar.xz"
            sbom = root / "qemu-rpi4.spdx.json"
            archive.write_bytes(b"archive")
            sbom.write_text('{"spdxVersion":"SPDX-2.3"}\n', encoding="utf-8")
            manifest = create_manifest(
                "rpi4-v0.1.0",
                COMMIT,
                BASELINE,
                [f"binary={archive}", f"sbom={sbom}"],
                "complete",
                0,
                12,
            )
            manifest_path = root / "manifest.json"
            _write_manifest(manifest_path, manifest)
            loaded = load_manifest(manifest_path)
            self.assertEqual(verify_artifacts(loaded, root), 2)
            self.assertEqual(
                [item["name"] for item in loaded["artifacts"]],
                ["qemu-rpi4.spdx.json", "qemu-rpi4.tar.xz"],
            )

    def test_rejects_tampered_artifact(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "qemu-rpi4.tar.xz"
            archive.write_bytes(b"original")
            manifest = create_manifest(
                "rpi4-v0.1.0",
                COMMIT,
                BASELINE,
                [f"binary={archive}"],
                "complete",
                0,
                12,
            )
            archive.write_bytes(b"tampered")
            with self.assertRaisesRegex(ManifestError, "mismatch"):
                verify_artifacts(manifest, root)

    def test_rejects_symlink_artifact(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / "target"
            target.write_bytes(b"content")
            link = root / "asset"
            link.symlink_to(target)
            with self.assertRaisesRegex(ManifestError, "non-symlink"):
                create_manifest(
                    "rpi4-v0.1.0",
                    COMMIT,
                    BASELINE,
                    [f"binary={link}"],
                    "complete",
                    0,
                    12,
                )

    def test_rejects_invalid_claims(self):
        with tempfile.TemporaryDirectory() as directory:
            asset = Path(directory) / "asset"
            asset.write_bytes(b"x")
            with self.assertRaisesRegex(ManifestError, "exceeds"):
                create_manifest(
                    "rpi4-v0.1.0",
                    COMMIT,
                    BASELINE,
                    [f"binary={asset}"],
                    "complete",
                    13,
                    12,
                )


if __name__ == "__main__":
    unittest.main()
