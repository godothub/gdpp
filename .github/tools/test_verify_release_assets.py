#!/usr/bin/env python3
"""Behavior tests for the release-asset integrity verifier."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path


TOOL = Path(__file__).with_name("verify_release_assets.py")
SPEC = importlib.util.spec_from_file_location("gdpp_verify_release_assets", TOOL)
assert SPEC is not None and SPEC.loader is not None
VERIFY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFY)


class ReleaseAssetVerificationTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="gdpp-release-assets-")
        self.root = Path(self.temporary.name)
        self.archive = self.root / "gdpp.zip"
        self.compiler_archive = self.root / "gdpp-compiler.zip"
        self.manifest = self.root / VERIFY.CHECKSUM_NAME
        self.archive.write_bytes(b"deterministic archive payload\n")
        self.compiler_archive.write_bytes(b"deterministic compiler archive payload\n")
        self.write_manifest()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_manifest(self) -> None:
        self.manifest.write_text(
            "".join(f"{VERIFY.sha256(self.root / name)}  {name}\n" for name in VERIFY.ARCHIVE_NAMES),
            encoding="ascii",
        )

    def test_exact_asset_set_and_digest_are_accepted(self) -> None:
        VERIFY.verify(self.root)

    def test_missing_or_unexpected_asset_is_rejected(self) -> None:
        self.manifest.unlink()
        with self.assertRaisesRegex(ValueError, "missing"):
            VERIFY.verify(self.root)
        self.write_manifest()
        (self.root / "diagnostics.zip").write_bytes(b"private evidence")
        with self.assertRaisesRegex(ValueError, "unexpected"):
            VERIFY.verify(self.root)

    def test_archive_mutation_is_rejected(self) -> None:
        self.archive.write_bytes(b"different payload\n")
        with self.assertRaisesRegex(ValueError, "does not match"):
            VERIFY.verify(self.root)

    def test_compiler_archive_is_required_and_authenticated(self) -> None:
        self.compiler_archive.write_bytes(b"different compiler payload\n")
        with self.assertRaisesRegex(ValueError, "does not match"):
            VERIFY.verify(self.root)
        self.compiler_archive.unlink()
        with self.assertRaisesRegex(ValueError, "missing"):
            VERIFY.verify(self.root)

    def test_noncanonical_manifest_is_rejected(self) -> None:
        canonical = self.manifest.read_text(encoding="ascii")
        for payload in (
            canonical.replace("  gdpp.zip", " *gdpp.zip"),
            canonical.upper(),
            canonical.replace("\n", "\r\n"),
            canonical.replace("  gdpp.zip", "  ../gdpp.zip"),
            "".join(reversed(canonical.splitlines(keepends=True))),
            canonical + "0" * 64 + "  extra.zip\n",
        ):
            with self.subTest(payload=payload):
                self.manifest.write_text(payload, encoding="ascii", newline="")
                with self.assertRaisesRegex(ValueError, "noncanonical"):
                    VERIFY.verify(self.root)

    def test_oversized_manifest_is_rejected_before_reading(self) -> None:
        self.manifest.write_bytes(b"0" * (1024 * 1024))
        with self.assertRaisesRegex(ValueError, "noncanonical size"):
            VERIFY.verify(self.root)

    def test_symlink_asset_is_rejected(self) -> None:
        self.manifest.unlink()
        self.manifest.symlink_to(self.archive)
        with self.assertRaisesRegex(ValueError, "regular file"):
            VERIFY.verify(self.root)


if __name__ == "__main__":
    unittest.main()
