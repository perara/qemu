#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from verify_qgpio_firmware import (
    VerificationError,
    load_manifest,
    verify_outputs,
    verify_sources,
)


class QGPIOFirmwareManifestTest(unittest.TestCase):
    def test_repository_sources_match_pinned_manifest(self):
        manifest_path = (
            Path(__file__).parent
            / "gpio_adapter"
            / "rp2040"
            / "firmware-manifest.json"
        )
        manifest = load_manifest(manifest_path)
        verify_sources(manifest_path, manifest)

    def test_output_size_and_hash_are_enforced(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.c"
            output = root / "firmware.uf2"
            source.write_bytes(b"source")
            output.write_bytes(b"firmware")
            manifest_path = root / "manifest.json"
            manifest_path.write_text(
                json.dumps(
                    {
                        "version": 1,
                        "sources": {
                            "source.c": hashlib.sha256(b"source").hexdigest()
                        },
                        "outputs": {
                            "firmware.uf2": {
                                "size": 8,
                                "sha256":
                                    hashlib.sha256(b"firmware").hexdigest(),
                            }
                        },
                    }
                ),
                encoding="utf-8",
            )
            manifest = load_manifest(manifest_path)
            verify_sources(manifest_path, manifest)
            verify_outputs(root, manifest)
            output.write_bytes(b"tampered")
            with self.assertRaises(VerificationError):
                verify_outputs(root, manifest)


if __name__ == "__main__":
    unittest.main()
