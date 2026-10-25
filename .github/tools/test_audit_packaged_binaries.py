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

    def test_audits_standalone_compilers_and_bundled_executors(self) -> None:
        package = self.root / "compiler-package"
        for relative in ("macos/universal/gdpp", "linux/x86_64/gdpp", "windows/x86_64/gdpp.exe"):
            executable = package / relative
            executable.parent.mkdir(parents=True, exist_ok=True)
            executable.write_bytes(b"clean executable")
        self.assertEqual(AUDIT.audit(package, self.source), [])
        (package / "windows/x86_64/gdpp.exe").write_bytes(str(self.source.resolve()).encode("utf-16-le"))
        self.assertEqual(AUDIT.audit(package, self.source), ["windows/x86_64/gdpp.exe"])
        executor = self.addon / "tools/mac-universal/gdpp-ninja"
        executor.parent.mkdir(parents=True)
        executor.write_bytes(str(self.source.resolve()).encode("utf-8"))
        self.assertEqual(AUDIT.audit(self.addon, self.source), ["tools/mac-universal/gdpp-ninja"])

    def test_audits_native_diagnostics_on_every_host(self) -> None:
        for relative in (
            "tools/mac-universal/gdpp-diagnostics",
            "tools/linux-x64/gdpp-diagnostics",
            "tools/windows-x64/gdpp-diagnostics.exe",
        ):
            with self.subTest(path=relative):
                tool = self.addon / relative
                tool.parent.mkdir(parents=True, exist_ok=True)
                tool.write_bytes(str(self.source.resolve()).encode("utf-8"))
                self.assertEqual(AUDIT.audit(self.addon, self.source), [relative])
                tool.unlink()

    def test_component_audit_covers_raw_sdks_host_addons_and_cli_before_sealing(self) -> None:
        component = self.root / "component"
        products = (
            "lib/web/wasm32/nothreads/godot44/runtime.a",
            "lib/ios/arm64/device/runtime.a",
            "addons/gdpp/binary/compiler.dylib",
            "compiler/windows/x86_64/gdpp.exe",
            "addons/gdpp/tools/linux-x64/gdpp-diagnostics",
        )
        for relative in products:
            path = component / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"clean native payload")
        self.assertEqual(AUDIT.audit(component, self.source, component=True), [])
        for relative in products:
            with self.subTest(relative=relative):
                path = component / relative
                path.write_bytes(b"/home/runner/work/gdpp/gdpp/build/ci/toolchain/include")
                self.assertEqual(AUDIT.audit(component, self.source, component=True), [relative])
                path.write_bytes(b"clean native payload")

    def test_component_audit_rejects_ci_temporary_toolchains_in_sdk_debug_metadata(self) -> None:
        component = self.root / "component"
        sdk = component / "lib/windows/x86_64/mingw/runtime.a"
        sdk.parent.mkdir(parents=True)
        for path in ("D:/a/_temp/gdpp-mingw/mingw64/include/c++", "D:\\a\\_temp\\mingw64\\include"):
            for encoding in ("utf-8", "utf-16-le"):
                with self.subTest(path=path, encoding=encoding):
                    sdk.write_bytes(path.encode(encoding))
                    self.assertEqual(AUDIT.audit(component, self.source, component=True),
                                     ["lib/windows/x86_64/mingw/runtime.a"])


if __name__ == "__main__":
    unittest.main()
