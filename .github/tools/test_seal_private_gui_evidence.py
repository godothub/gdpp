#!/usr/bin/env python3
"""Behavior tests for bounded, recipient-only GUI evidence transport."""

from __future__ import annotations

import base64
from contextlib import redirect_stderr, redirect_stdout
import hashlib
import io
import json
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

import seal_private_gui_evidence as sealer


SOURCE = "1" * 40
PIPELINE = "2" * 40
SECRET = b"PRIVATE_EDITOR_CONTENT /private/source/addons/confidential.gd:71\n"


def decrypt(envelope: dict, private_key: X25519PrivateKey) -> bytes:
    """Independent offline recipient implementation; no production decrypt API."""
    aad = json.dumps(envelope["context"], sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii")
    public = base64.b64decode(envelope["ephemeral_public_key"], validate=True)
    recipient = private_key.public_key().public_bytes_raw()
    shared = private_key.exchange(X25519PublicKey.from_public_bytes(public))
    key = HKDF(
        algorithm=hashes.SHA256(), length=32,
        salt=base64.b64decode(envelope["salt"], validate=True),
        info=b"gdpp-private-gui-evidence-v1\x00" + aad + public + recipient,
    ).derive(shared)
    return AESGCM(key).decrypt(
        base64.b64decode(envelope["nonce"], validate=True),
        base64.b64decode(envelope["ciphertext"], validate=True), aad,
    )


class SealPrivateGuiEvidenceTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="gdpp-seal-gui-")
        self.base = Path(self.temporary.name).resolve()
        self.root = self.base / "build/ci/editor-progress-input"
        self.instance = self.root / "editor-progress-input-test1"
        self.instance.mkdir(parents=True)
        self.log = self.instance / "editor.log"
        self.validation = self.instance / "validation.json"
        self.log.write_bytes(SECRET)
        self.validation.write_bytes(b'{"private_path":"/private/source/project","success":true}\n')
        self.output = self.root / "sealed-private-gui-evidence.json"
        self.private_key = X25519PrivateKey.generate()
        self.recipient_patch = patch.object(sealer, "RECIPIENT_PUBLIC_KEY", self.private_key.public_key().public_bytes_raw())
        self.recipient_patch.start()

    def tearDown(self) -> None:
        self.recipient_patch.stop()
        self.temporary.cleanup()

    def seal(self, output: Path | None = None, **context: str) -> dict:
        sealer.seal(
            self.root, output or self.output, context.get("source_sha", SOURCE),
            context.get("pipeline_sha", PIPELINE), context.get("engine_version", "4.4.1"),
        )
        return json.loads((output or self.output).read_text())

    def assert_rejected(self, root: Path | None = None, output: Path | None = None) -> None:
        with self.assertRaises((sealer.ContractError, OSError)):
            sealer.seal(root or self.root, output or self.output, SOURCE, PIPELINE, "4.4.1")
        self.assertFalse(self.output.exists())

    def test_roundtrip_contains_only_two_exact_raw_files_and_random_padding(self) -> None:
        unrelated = self.instance / "generated-source"
        unrelated.mkdir()
        (unrelated / "must-not-be-collected.gd").write_text("another private source")
        envelope = self.seal()
        self.assertEqual(set(envelope), {"context", "ephemeral_public_key", "salt", "nonce", "ciphertext"})
        self.assertEqual(envelope["context"], {
            "schema": "gdpp-private-gui-evidence-v1", "source_sha": SOURCE,
            "pipeline_sha": PIPELINE, "engine_version": "4.4.1",
        })
        for field, length in (("ephemeral_public_key", 32), ("salt", 32), ("nonce", 12), ("ciphertext", 65552)):
            self.assertEqual(len(base64.b64decode(envelope[field], validate=True)), length)
        padded = decrypt(envelope, self.private_key)
        self.assertEqual(len(padded), 65536)
        size = struct.unpack(">I", padded[:4])[0]
        archive_bytes = padded[4:4 + size]
        self.assertLessEqual(size, 48 * 1024 + 1024)
        with zipfile.ZipFile(io.BytesIO(archive_bytes)) as archive:
            self.assertEqual(archive.namelist(), ["editor.log", "validation.json"])
            self.assertEqual(archive.read("editor.log"), self.log.read_bytes())
            self.assertEqual(archive.read("validation.json"), self.validation.read_bytes())
            self.assertTrue(all(info.compress_type == zipfile.ZIP_STORED for info in archive.infolist()))
        self.assertNotEqual(padded[4 + size:], bytes(len(padded) - 4 - size))
        self.assertEqual(stat.S_IMODE(self.output.stat().st_mode), 0o600)
        document = self.output.read_bytes()
        for plaintext in (SECRET, b"/private/source", b"editor.log", b"validation.json", base64.b64encode(SECRET)):
            self.assertNotIn(plaintext, document)

    def test_repeated_seals_have_fixed_length_and_fresh_ephemeral_salt_nonce_padding(self) -> None:
        first = self.seal()
        second_output = self.root / "second.json"
        second = self.seal(second_output)
        self.assertEqual(self.output.stat().st_size, second_output.stat().st_size)
        for field in ("ephemeral_public_key", "salt", "nonce", "ciphertext"):
            self.assertNotEqual(first[field], second[field])
        self.assertNotEqual(decrypt(first, self.private_key), decrypt(second, self.private_key))
        self.log.write_bytes(b"a" * 20000)
        third_output = self.root / "third.json"
        self.seal(third_output)
        self.assertEqual(self.output.stat().st_size, third_output.stat().st_size)

    def test_all_aad_fields_are_authenticated(self) -> None:
        envelope = self.seal()
        replacements = {"source_sha": "3" * 40, "pipeline_sha": "4" * 40,
                        "engine_version": "4.7.2", "schema": "gdpp-private-gui-evidence-v2"}
        for field, replacement in replacements.items():
            with self.subTest(field=field):
                changed = {**envelope, "context": {**envelope["context"], field: replacement}}
                with self.assertRaises(InvalidTag):
                    decrypt(changed, self.private_key)
        with self.assertRaises(InvalidTag):
            decrypt({**envelope, "context": {**envelope["context"], "unexpected": "value"}}, self.private_key)

    def test_wrong_key_ciphertext_salt_and_nonce_tampering_are_rejected(self) -> None:
        envelope = self.seal()
        with self.assertRaises(InvalidTag):
            decrypt(envelope, X25519PrivateKey.generate())
        for field in ("ciphertext", "salt", "nonce"):
            with self.subTest(field=field):
                changed = bytearray(base64.b64decode(envelope[field]))
                changed[-1] ^= 1
                with self.assertRaises(InvalidTag):
                    decrypt({**envelope, field: base64.b64encode(changed).decode("ascii")}, self.private_key)
        ciphertext = base64.b64decode(envelope["ciphertext"])
        with self.assertRaises(InvalidTag):
            decrypt({**envelope, "ciphertext": base64.b64encode(ciphertext[:-1]).decode("ascii")}, self.private_key)

    def test_raw_total_at_limit_passes_and_one_byte_over_is_rejected(self) -> None:
        self.validation.write_bytes(b"{}")
        self.log.write_bytes(b"x" * (48 * 1024 - 2))
        self.seal()
        self.output.unlink()
        self.log.write_bytes(b"x" * (48 * 1024 - 1))
        self.assert_rejected()
        self.log.write_bytes(b"x" * (48 * 1024 + 1))
        self.assert_rejected()

    def test_missing_files_and_zero_or_multiple_instances_are_rejected(self) -> None:
        self.log.unlink()
        self.assert_rejected()
        self.log.write_bytes(SECRET)
        self.validation.unlink()
        self.assert_rejected()
        self.validation.write_bytes(b"{}")
        renamed = self.root / "unrelated-directory"
        self.instance.rename(renamed)
        self.assert_rejected()
        renamed.rename(self.instance)
        (self.root / "editor-progress-input-second").mkdir()
        self.assert_rejected()

    def test_context_has_exact_fields_and_closed_types_values(self) -> None:
        valid = {"schema": sealer.SCHEMA, "source_sha": SOURCE, "pipeline_sha": PIPELINE, "engine_version": "4.4.1"}
        invalid = [None, [], {**valid, "extra": "secret"}, {key: value for key, value in valid.items() if key != "schema"}]
        for field in valid:
            for value in (None, True, 3, [], {}, "secret"):
                invalid.append({**valid, field: value})
        invalid += [{**valid, "source_sha": "A" * 40}, {**valid, "pipeline_sha": "a" * 41},
                    {**valid, "engine_version": "4.4.1\nsecret"}]
        for context in invalid:
            with self.subTest(context=context):
                with self.assertRaises(sealer.ContractError):
                    sealer.canonical_context(context)
        for version in ("4.4.1", "4.5.2", "4.6.3", "4.7.2"):
            self.assertIn(version.encode(), sealer.canonical_context({**valid, "engine_version": version}))

    def test_paths_must_be_fixed_workroot_and_new_direct_json_child(self) -> None:
        self.assert_rejected(output=self.base / "outside.json")
        self.assert_rejected(output=self.instance / "inside.json")
        self.assert_rejected(output=self.root / "not-json.txt")
        self.assert_rejected(output=self.root / "../escape.json")
        self.assert_rejected(output=self.root / "bad\nname.json")
        other_root = self.base / "arbitrary-root"
        other_root.mkdir()
        self.assert_rejected(root=other_root, output=other_root / "sealed.json")
        self.output.write_bytes(b"existing-owned-file")
        with self.assertRaises(FileExistsError):
            self.seal()
        self.assertEqual(self.output.read_bytes(), b"existing-owned-file")

    def test_log_and_validation_symlinks_and_hardlinks_are_rejected(self) -> None:
        outside = self.base / "outside-private.txt"
        outside.write_bytes(SECRET)
        for path in (self.log, self.validation):
            with self.subTest(path=path.name):
                original = path.read_bytes()
                path.unlink()
                path.symlink_to(outside)
                self.assert_rejected()
                path.unlink()
                os.link(outside, path)
                self.assert_rejected()
                path.unlink()
                path.write_bytes(original)

    def test_instance_root_and_ancestor_symlinks_are_rejected(self) -> None:
        real_instance = self.root / "owned-instance"
        self.instance.rename(real_instance)
        self.instance.symlink_to(real_instance, target_is_directory=True)
        self.assert_rejected()
        self.instance.unlink()
        real_instance.rename(self.instance)
        real_root = self.root.parent / "owned-root"
        self.root.rename(real_root)
        self.root.symlink_to(real_root, target_is_directory=True)
        self.assert_rejected()
        self.root.unlink()
        real_root.rename(self.root)
        real_parent = self.root.parent.with_name("owned-parent")
        self.root.parent.rename(real_parent)
        self.root.parent.symlink_to(real_parent, target_is_directory=True)
        self.assert_rejected()

    def test_output_symlink_cannot_overwrite_any_target(self) -> None:
        outside = self.base / "outside-private.txt"
        outside.write_bytes(SECRET)
        self.output.symlink_to(outside)
        with self.assertRaises(FileExistsError):
            self.seal()
        self.assertEqual(outside.read_bytes(), SECRET)

    def test_nonregular_input_and_oversized_encryption_payload_are_rejected(self) -> None:
        self.log.unlink()
        self.log.mkdir()
        self.assert_rejected()
        self.log.rmdir()
        os.mkfifo(self.log)
        self.assert_rejected()
        context = {"schema": sealer.SCHEMA, "source_sha": SOURCE, "pipeline_sha": PIPELINE, "engine_version": "4.4.1"}
        with self.assertRaises(sealer.ContractError):
            sealer.encrypt_payload(b"x" * 65536, context)

    def test_cli_prints_only_safe_digest_and_never_raw_exception_or_arguments(self) -> None:
        arguments = ["--root", str(self.root), "--source-sha", SOURCE,
                     "--pipeline-sha", PIPELINE, "--engine-version", "4.4.1", "--output", str(self.output)]
        stdout, stderr = io.StringIO(), io.StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            self.assertEqual(sealer.main(arguments), 0)
        digest = hashlib.sha256(self.output.read_bytes()).hexdigest()
        self.assertEqual(stdout.getvalue(), f"private-gui-evidence status=sealed schema=gdpp-private-gui-evidence-v1 sha256={digest}\n")
        self.assertEqual(stderr.getvalue(), "")
        stdout, stderr = io.StringIO(), io.StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            self.assertEqual(sealer.main(arguments), 1)
        self.assertEqual(stdout.getvalue(), "")
        self.assertEqual(stderr.getvalue(), "private-gui-evidence status=rejected\n")
        result = subprocess.run(
            [sys.executable, str(Path(sealer.__file__)), *arguments, "--recipient-public-key", SECRET.decode()],
            capture_output=True, check=False,
        )
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, b"")
        self.assertEqual(result.stderr, b"private-gui-evidence status=rejected category=arguments\n")

    def test_production_recipient_is_fixed_and_environment_cannot_override_it(self) -> None:
        self.recipient_patch.stop()
        expected = bytes.fromhex("6ab76180605e4563aa574b4b7f662291bbb6999c09fdb8ae5af7d58d71697660")
        self.assertEqual(sealer.RECIPIENT_PUBLIC_KEY, expected)
        with patch.dict(os.environ, {"GDPP_GUI_RECIPIENT_PUBLIC_KEY": self.private_key.public_key().public_bytes_raw().hex()}):
            envelope = self.seal()
        self.assertEqual(sealer.RECIPIENT_PUBLIC_KEY, expected)
        with self.assertRaises(InvalidTag):
            decrypt(envelope, self.private_key)


if __name__ == "__main__":
    unittest.main()
