#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


MODULE_PATH = Path(__file__).with_name("default_host_gate.py")
SPEC = importlib.util.spec_from_file_location("default_host_gate", MODULE_PATH)
assert SPEC and SPEC.loader
default_host_gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(default_host_gate)


class DefaultHostGateTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.openssl = default_host_gate.shutil.which("openssl")
        if not self.openssl:
            self.skipTest("OpenSSL is unavailable")
        self.eeprom = self.root / "pieeprom.bin"
        self.image = self.root / "boot.img"
        self.signature = self.root / "boot.sig"
        self.public_key = self.root / "public.pem"
        self.ca_certificate = self.root / "ca.pem"
        self.server_certificate = self.root / "server.pem"
        self.eeprom.write_bytes(
            b"official-eeprom-fixture".ljust(
                default_host_gate.EEPROM_SIZE, b"\xff"))
        self.image.write_bytes(b"unchanged signed boot image")
        self._make_keys_and_certificates()
        self._sign_image()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def _run(self, *arguments: str) -> None:
        subprocess.run(
            [self.openssl, *arguments],
            check=True, capture_output=True, timeout=30)

    def _make_keys_and_certificates(self) -> None:
        signing_key = self.root / "signing-key.pem"
        ca_key = self.root / "ca-key.pem"
        server_key = self.root / "server-key.pem"
        server_request = self.root / "server.csr"
        extensions = self.root / "server.ext"
        extensions.write_text(
            "subjectAltName=DNS:fw-download-alias1.raspberrypi.com\n",
            encoding="ascii")
        self._run("genpkey", "-algorithm", "RSA", "-pkeyopt",
                  "rsa_keygen_bits:2048", "-out", str(signing_key))
        self._run("pkey", "-in", str(signing_key), "-pubout",
                  "-out", str(self.public_key))
        self._run("genpkey", "-algorithm", "RSA", "-pkeyopt",
                  "rsa_keygen_bits:2048", "-out", str(ca_key))
        self._run("req", "-x509", "-new", "-key", str(ca_key),
                  "-subj", "/CN=Raspberry Pi Test CA", "-days", "1",
                  "-out", str(self.ca_certificate))
        self._run("genpkey", "-algorithm", "RSA", "-pkeyopt",
                  "rsa_keygen_bits:2048", "-out", str(server_key))
        self._run(
            "req", "-new", "-key", str(server_key),
            "-subj",
            "/CN=fw-download-alias1.raspberrypi.com",
            "-out", str(server_request))
        self._run(
            "x509", "-req", "-in", str(server_request),
            "-CA", str(self.ca_certificate), "-CAkey", str(ca_key),
            "-CAcreateserial", "-days", "1", "-extfile", str(extensions),
            "-out", str(self.server_certificate))
        self.signing_key = signing_key

    def _sign_image(self) -> None:
        signature_bin = self.root / "signature.bin"
        self._run("dgst", "-sha256", "-sign", str(self.signing_key),
                  "-out", str(signature_bin), str(self.image))
        self.signature.write_text(
            f"{default_host_gate.sha256_file(self.image)}\n"
            "ts: 1\n"
            f"rsa2048: {signature_bin.read_bytes().hex()}\n",
            encoding="ascii")

    def _args(self) -> argparse.Namespace:
        return argparse.Namespace(
            eeprom=self.eeprom,
            boot_image=self.image,
            boot_signature=self.signature,
            public_key=self.public_key,
            ca_certificate=self.ca_certificate,
            server_certificate=self.server_certificate,
            eeprom_sha256=None,
            boot_image_sha256=None,
            boot_signature_sha256=None,
            public_key_sha256=None,
            ca_certificate_sha256=None,
            openssl=self.openssl,
        )

    def test_valid_trust_boundary(self) -> None:
        report = default_host_gate.verify(self._args())
        self.assertEqual(report["schema"], default_host_gate.SCHEMA)
        self.assertEqual(
            report["policy"]["host"], default_host_gate.DEFAULT_HOST)
        self.assertTrue(report["boot_signature"]["rsa_verified"])
        self.assertTrue(
            report["server_certificate"]["hostname_verified"])

    def test_tampered_image_rejected(self) -> None:
        self.image.write_bytes(self.image.read_bytes() + b"tampered")
        with self.assertRaisesRegex(
                default_host_gate.GateError, "signed digest"):
            default_host_gate.verify(self._args())

    def test_eeprom_geometry_rejected(self) -> None:
        for size in (
                default_host_gate.EEPROM_SIZE - 1,
                default_host_gate.EEPROM_SIZE + 1):
            with self.subTest(size=size):
                self.eeprom.write_bytes(b"\xff" * size)
                with self.assertRaisesRegex(
                        default_host_gate.GateError, "524288 bytes"):
                    default_host_gate.verify(self._args())

    def test_wrong_public_key_rejected(self) -> None:
        wrong_key = self.root / "wrong-key.pem"
        self._run("genpkey", "-algorithm", "RSA", "-pkeyopt",
                  "rsa_keygen_bits:2048", "-out", str(wrong_key))
        self._run("pkey", "-in", str(wrong_key), "-pubout",
                  "-out", str(self.public_key))
        with self.assertRaisesRegex(
                default_host_gate.GateError, "RSA signature"):
            default_host_gate.verify(self._args())

    def test_wrong_hostname_rejected(self) -> None:
        extensions = self.root / "wrong.ext"
        request = self.root / "wrong.csr"
        certificate = self.root / "wrong.pem"
        key = self.root / "wrong-server-key.pem"
        ca_key = self.root / "other-ca-key.pem"
        self._run("genpkey", "-algorithm", "RSA", "-pkeyopt",
                  "rsa_keygen_bits:2048", "-out", str(ca_key))
        extensions.write_text(
            "subjectAltName=DNS:wrong.example\n", encoding="ascii")
        self._run("req", "-new", "-key", str(ca_key),
                  "-subj", "/CN=wrong.example", "-out", str(request))
        self._run(
            "x509", "-req", "-in", str(request),
            "-CA", str(self.ca_certificate), "-CAkey",
            str(self.root / "ca-key.pem"), "-CAcreateserial",
            "-days", "1", "-extfile", str(extensions),
            "-out", str(certificate))
        args = self._args()
        args.server_certificate = certificate
        with self.assertRaisesRegex(
                default_host_gate.GateError, "certificate verification"):
            default_host_gate.verify(args)

    def test_client_only_certificate_rejected(self) -> None:
        extensions = self.root / "client-only.ext"
        request = self.root / "client-only.csr"
        certificate = self.root / "client-only.pem"
        key = self.root / "client-only-key.pem"
        extensions.write_text(
            "subjectAltName=DNS:fw-download-alias1.raspberrypi.com\n"
            "extendedKeyUsage=clientAuth\n",
            encoding="ascii",
        )
        self._run("genpkey", "-algorithm", "RSA", "-pkeyopt",
                  "rsa_keygen_bits:2048", "-out", str(key))
        self._run(
            "req", "-new", "-key", str(key),
            "-subj", "/CN=fw-download-alias1.raspberrypi.com",
            "-out", str(request),
        )
        self._run(
            "x509", "-req", "-in", str(request),
            "-CA", str(self.ca_certificate),
            "-CAkey", str(self.root / "ca-key.pem"),
            "-CAcreateserial", "-days", "1",
            "-extfile", str(extensions), "-out", str(certificate),
        )
        args = self._args()
        args.server_certificate = certificate
        with self.assertRaisesRegex(
                default_host_gate.GateError, "certificate verification"):
            default_host_gate.verify(args)

    def test_symlink_rejected(self) -> None:
        link = self.root / "linked-boot.img"
        link.symlink_to(self.image)
        args = self._args()
        args.boot_image = link
        with self.assertRaisesRegex(
                default_host_gate.GateError, "non-symlink"):
            default_host_gate.verify(args)

    def test_report_is_json_serializable(self) -> None:
        json.dumps(default_host_gate.verify(self._args()))


if __name__ == "__main__":
    unittest.main()
