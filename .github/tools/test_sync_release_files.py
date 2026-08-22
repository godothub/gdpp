#!/usr/bin/env python3
"""Behavior tests for the public release-file synchronizer."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path


TOOL = Path(__file__).with_name("sync_release_files.py")
SPEC = importlib.util.spec_from_file_location("gdpp_sync_release_files", TOOL)
assert SPEC is not None and SPEC.loader is not None
SYNC = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SYNC)


class ReleaseFileSyncTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="gdpp-release-files-")
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.destination = self.root / "public"
        self.source.mkdir()
        self.destination.mkdir()
        for index, relative in enumerate(SYNC.RELEASE_FILES):
            (self.source / relative).write_bytes(f"private-{index}\n".encode())

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_exact_allowlist_is_copied_and_then_stable(self) -> None:
        (self.destination / "README.md").write_text("stale\n", encoding="utf-8")
        (self.destination / "CHANGELOG-ZH.md").write_text(
            "public-only\n", encoding="utf-8"
        )
        (self.destination / "unrelated.txt").write_text("retain\n", encoding="utf-8")
        self.assertEqual(SYNC.synchronize(self.source, self.destination), list(SYNC.RELEASE_FILES))
        self.assertEqual(SYNC.synchronize(self.source, self.destination), [])
        self.assertEqual(
            (self.destination / "CHANGELOG-ZH.md").read_text(encoding="utf-8"),
            "public-only\n",
        )
        self.assertEqual(
            (self.destination / "unrelated.txt").read_text(encoding="utf-8"),
            "retain\n",
        )
        for relative in SYNC.RELEASE_FILES:
            self.assertEqual(
                (self.destination / relative).read_bytes(),
                (self.source / relative).read_bytes(),
            )

    def test_missing_private_file_fails_closed_without_partial_target(self) -> None:
        missing = self.source / "README-ZH.md"
        missing.unlink()
        with self.assertRaisesRegex(ValueError, "README-ZH.md"):
            SYNC.synchronize(self.source, self.destination)
        self.assertEqual(list(self.destination.iterdir()), [])

    def test_private_symlink_is_rejected(self) -> None:
        target = self.source / "README-ZH.md"
        target.unlink()
        target.symlink_to(self.source / "README.md")
        with self.assertRaisesRegex(ValueError, "README-ZH.md"):
            SYNC.synchronize(self.source, self.destination)


if __name__ == "__main__":
    unittest.main()
