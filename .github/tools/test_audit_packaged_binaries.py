#!/usr/bin/env python3
"""Tests for the native release path audit."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest


TOOL = Path(__file__).with_name("audit_packaged_binaries.py")
SPEC = importlib.util.spec_from_file_location("gdpp_binary_path_audit", TOOL)
assert SPEC is not None and SPEC.loader is not None
AUDIT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(AUDIT)


class PackagedBinaryAuditTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="gdpp-binary-audit-")
        self.root = Path(self.temporary.name)
        self.addon = self.root / "release/addons/gdpp"
        self.source = self.root / "private/source"
        (self.addon / "binary").mkdir(parents=True)
        (self.addon / "sdk/lib/windows/x86_64").mkdir(parents=True)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_audits_editor_and_sdk_native_products(self) -> None:
        editor = self.addon / "binary/gdpp_compiler.windows.x86_64.dll"
        linux_editor = self.addon / "binary/libgdpp_compiler.linux.x86_64.so"
        mac_editor = self.addon / "binary/libgdpp_compiler.macos.universal.dylib"
        runtime = self.addon / "sdk/lib/windows/x86_64/gdpp_host_runtime.lib"
        static_runtime = self.addon / "sdk/lib/linux/x86_64/libgdpp_host_runtime.a"
        editor.write_bytes(b"release editor binary")
        linux_editor.write_bytes(b"release Linux editor binary")
        mac_editor.write_bytes(b"release macOS editor binary")
        runtime.write_bytes(b"release runtime archive")
        static_runtime.parent.mkdir(parents=True)
        static_runtime.write_bytes(b"release static runtime archive")
        self.assertEqual(AUDIT.audit(self.addon, self.source), [])
        runtime.write_bytes(
            b"debug path: /home/runner/work/gdpp/gdpp/source/src/runtime.cpp"
        )
        self.assertEqual(
            AUDIT.audit(self.addon, self.source),
            ["sdk/lib/windows/x86_64/gdpp_host_runtime.lib"],
        )

    def test_rejects_the_exact_private_root_in_utf8_and_utf16(self) -> None:
        editor = self.addon / "binary/libgdpp_compiler.macos.universal.dylib"
        editor.write_bytes(str(self.source.resolve()).encode("utf-8"))
        self.assertEqual(
            AUDIT.audit(self.addon, self.source),
            ["binary/libgdpp_compiler.macos.universal.dylib"],
        )
        editor.write_bytes(str(self.source.resolve()).encode("utf-16-le"))
        self.assertEqual(
            AUDIT.audit(self.addon, self.source),
            ["binary/libgdpp_compiler.macos.universal.dylib"],
        )

    def test_detects_a_checkout_path_split_across_streaming_chunks(self) -> None:
        editor = self.addon / "binary/libgdpp_compiler.linux.x86_64.so"
        needle = str(self.source.resolve()).encode("utf-8")
        editor.write_bytes(
            b"x" * (AUDIT.SCAN_CHUNK_SIZE - len(needle) // 2)
            + needle
            + b"tail"
        )
        self.assertEqual(
            AUDIT.audit(self.addon, self.source),
            ["binary/libgdpp_compiler.linux.x86_64.so"],
        )

    def test_requires_at_least_one_native_product(self) -> None:
        self.assertEqual(
            AUDIT.audit(self.addon, self.source),
            ["release contains no native products to audit"],
        )


if __name__ == "__main__":
    unittest.main()
