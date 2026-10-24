#!/usr/bin/env python3
"""Exercise the ELF profile gate with real shared libraries and bound receipts."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib


def executable(name: str) -> str | None:
    local = Path("/opt/homebrew/opt/llvm@18/bin") / name
    return str(local) if local.is_file() else shutil.which(name)


class ElfProfileTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.clang = executable("clang")
        cls.linker = executable("ld.lld")
        cls.objcopy = executable("llvm-objcopy") or shutil.which("objcopy")
        cls.gcc = shutil.which("gcc") if sys.platform.startswith("linux") else None
        if not cls.objcopy or not ((cls.clang and cls.linker) or cls.gcc):
            reason = "real ELF fixtures require clang/lld or native Linux gcc, plus objcopy"
            if os.environ.get("GITHUB_ACTIONS") == "true" and sys.platform.startswith("linux"):
                raise RuntimeError(reason)
            raise unittest.SkipTest(reason)
        spec = importlib.util.spec_from_file_location(
            "gdpp_verify_elf_profile", Path(__file__).with_name("verify_elf_profile.py")
        )
        assert spec and spec.loader
        cls.module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = cls.module
        spec.loader.exec_module(cls.module)
        cls.temporary = tempfile.TemporaryDirectory(prefix="gdpp-real-elf-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.fixtures = Path(cls.temporary.name)
        cls.debug = cls.build_library("debug")
        cls.release = cls.fixtures / "librelease.so"
        cls.command([cls.objcopy, "--strip-all", cls.debug, cls.release])
        cls.only_debug_stripped = cls.fixtures / "libstripdebug.so"
        cls.command([cls.objcopy, "--strip-debug", cls.debug, cls.only_debug_stripped])
        cls.extra = cls.build_library("extra", extra=True)

    @classmethod
    def command(cls, command: list[str | Path]) -> None:
        result = subprocess.run(list(map(str, command)), capture_output=True, check=False)
        if result.returncode:
            raise AssertionError(
                f"real ELF fixture command failed: {command!r}\n"
                + result.stdout.decode(errors="replace")
                + result.stderr.decode(errors="replace")
            )

    @classmethod
    def build_library(cls, name: str, *, extra: bool = False, architecture: str = "x86_64") -> Path:
        source = cls.fixtures / (name + ".c")
        source.write_text(
            '__attribute__((visibility("default"))) int gdpp_library_init(void) { return 17; }\n'
            + ('__attribute__((visibility("default"))) int extra_export(void) { return 23; }\n' if extra else "")
        )
        output = cls.fixtures / ("lib" + name + ".so")
        if cls.clang and cls.linker:
            obj = source.with_suffix(".o")
            triple = "aarch64-linux-gnu" if architecture == "arm64" else "x86_64-linux-gnu"
            cls.command([cls.clang, "--target=" + triple, "-fPIC", "-g", "-O0",
                         "-fvisibility=hidden", "-c", source, "-o", obj])
            cls.command([cls.linker, "-shared", "--build-id=sha1", obj, "-o", output])
        else:
            cls.command([cls.gcc, "-shared", "-fPIC", "-g", "-O0", "-fvisibility=hidden",
                         "-nostdlib", source, "-o", output])
        return output

    def setUp(self) -> None:
        self.work = tempfile.TemporaryDirectory(prefix="gdpp-elf-receipt-")
        self.addCleanup(self.work.cleanup)
        self.root = Path(self.work.name)

    def receipt(self, library: Path, profile: str, architecture: str = "x86_64", platform: str = "linux") -> Path:
        output = self.root / "receipt.json"
        output.write_text(json.dumps({
            "schema": "GDPP_NATIVE_BUILD_RECEIPT 1",
            "build_profile": profile,
            "target_platform": platform,
            "target_architecture": architecture,
            "plan": {"output_library": str(library.resolve())},
            "execution": {"artifact_content_sha256": hashlib.sha256(library.read_bytes()).hexdigest()},
        }))
        return output

    def verify(self, library: Path, profile: str, *, architecture: str = "x86_64", platform: str = "linux") -> dict:
        return self.module.verify(library, self.receipt(library, profile, architecture, platform),
                                  profile, architecture, platform)

    def assert_profile(self, library: Path, profile: str, *, architecture: str = "x86_64", platform: str = "linux") -> None:
        result = self.verify(library, profile, architecture=architecture, platform=platform)
        self.assertEqual(result["profile"], profile)
        self.assertEqual(result["architecture"], architecture)
        self.assertIs(result["has_symtab"], profile == "debug")
        self.assertIs(result["has_dwarf"], profile == "debug")

    def test_real_debug_elf_has_symbols_and_dwarf(self) -> None:
        self.assert_profile(self.debug, "debug")

    def test_real_stripped_elf_accepts_release_and_profile_with_matching_receipt(self) -> None:
        for profile in ("release", "profile"):
            with self.subTest(profile=profile):
                self.assert_profile(self.release, profile)

    def test_android_arm64_uses_actual_elf_architecture(self) -> None:
        if not (self.clang and self.linker):
            self.skipTest("native gcc fallback cannot build the independent arm64 fixture")
        library = self.build_library("android-arm64", architecture="arm64")
        self.assert_profile(library, "debug", architecture="arm64", platform="android")

    def test_profile_is_bound_to_receipt_not_inferred_from_identical_stripped_bytes(self) -> None:
        for actual, requested in (("release", "profile"), ("profile", "release"), ("debug", "release")):
            with self.subTest(receipt=actual, requested=requested):
                library = self.debug if actual == "debug" else self.release
                receipt = self.receipt(library, actual)
                with self.assertRaises(ValueError):
                    self.module.verify(library, receipt, requested, "x86_64")

    def test_strip_debug_only_satisfies_neither_debug_nor_stripped_profiles(self) -> None:
        for profile in ("debug", "release", "profile"):
            with self.subTest(profile=profile), self.assertRaises(ValueError):
                self.verify(self.only_debug_stripped, profile)

    def test_stripped_binary_cannot_satisfy_debug(self) -> None:
        with self.assertRaises(ValueError):
            self.verify(self.release, "debug")

    def test_debug_binary_cannot_satisfy_release_or_profile(self) -> None:
        for profile in ("release", "profile"):
            with self.subTest(profile=profile), self.assertRaises(ValueError):
                self.verify(self.debug, profile)

    def test_extra_dynamic_function_export_is_rejected_in_every_profile(self) -> None:
        stripped = self.root / "libextra.so"
        self.command([self.objcopy, "--strip-all", self.extra, stripped])
        for profile in ("debug", "release", "profile"):
            with self.subTest(profile=profile), self.assertRaises(ValueError):
                self.verify(self.extra if profile == "debug" else stripped, profile)

    def test_receipt_rejects_wrong_hash_basename_absolute_path_profile_platform_and_schema(self) -> None:
        changes = [
            ("execution", "artifact_content_sha256", "0" * 64),
            ("plan", "output_library", str(self.root / "different-name.so")),
            ("plan", "output_library", self.debug.name),
            (None, "build_profile", "release"),
            (None, "target_platform", "android"),
            (None, "target_architecture", "arm64"),
            (None, "schema", "GDPP_NATIVE_BUILD_RECEIPT 0"),
        ]
        for section, key, value in changes:
            with self.subTest(section=section, key=key, value=value):
                path = self.receipt(self.debug, "debug")
                data = json.loads(path.read_text())
                (data[section] if section else data)[key] = value
                path.write_text(json.dumps(data))
                with self.assertRaises(ValueError):
                    self.module.verify(self.debug, path, "debug", "x86_64")

    def test_receipt_requires_output_path_and_artifact_digest(self) -> None:
        for section, key in (("plan", "output_library"), ("execution", "artifact_content_sha256")):
            with self.subTest(section=section):
                path = self.receipt(self.debug, "debug")
                data = json.loads(path.read_text())
                del data[section][key]
                path.write_text(json.dumps(data))
                with self.assertRaises(ValueError):
                    self.module.verify(self.debug, path, "debug", "x86_64")

    def test_packaging_relocation_preserves_bound_basename_and_content(self) -> None:
        receipt = self.receipt(self.debug, "debug")
        relocated = self.root / self.debug.name
        shutil.copyfile(self.debug, relocated)
        result = self.module.verify(relocated, receipt, "debug", "x86_64")
        self.assertEqual(result["profile"], "debug")

    def test_receipt_cannot_override_real_elf_architecture(self) -> None:
        with self.assertRaises(ValueError):
            self.verify(self.debug, "debug", architecture="arm64")

    @staticmethod
    def sections(data: bytes | bytearray) -> dict[str, tuple[int, int, int]]:
        offset = struct.unpack_from("<Q", data, 40)[0]
        size, count, names_index = struct.unpack_from("<HHH", data, 58)
        names_header = offset + names_index * size
        names_offset, names_size = struct.unpack_from("<QQ", data, names_header + 24)
        names = data[names_offset:names_offset + names_size]
        result = {}
        for index in range(count):
            header = offset + index * size
            name_offset = struct.unpack_from("<I", data, header)[0]
            end = names.index(0, name_offset)
            name = bytes(names[name_offset:end]).decode()
            file_offset, file_size = struct.unpack_from("<QQ", data, header + 24)
            result[name] = header, file_offset, file_size
        return result

    def test_debug_accepts_real_shf_compressed_dwarf_sections(self) -> None:
        library = self.root / "libcompressed.so"
        self.command([self.objcopy, "--compress-debug-sections=zlib", self.debug, library])
        self.assert_profile(library, "debug")

    def test_debug_accepts_gnu_compressed_zdebug_sections(self) -> None:
        original = self.debug.read_bytes()
        sections = self.sections(original)
        library = self.root / "libzdebug.so"
        command = [self.objcopy]
        for name in (".debug_info", ".debug_line"):
            _, offset, size = sections[name]
            compressed = self.root / (name + ".zlib")
            compressed.write_bytes(b"ZLIB" + size.to_bytes(8, "big") + zlib.compress(original[offset:offset + size]))
            command += ["--update-section=" + name + "=" + str(compressed),
                        "--rename-section=" + name + "=.zdebug_" + name.removeprefix(".debug_")]
        self.command(command + [self.debug, library])
        self.assert_profile(library, "debug")

    def test_stripped_profiles_reject_all_debug_section_name_families(self) -> None:
        payload = self.root / "debug-payload.bin"
        payload.write_bytes(b"private debug data")
        for name in (".debug_info", ".zdebug_line", ".gnu_debugdata"):
            library = self.root / ("lib" + name + ".so")
            self.command([self.objcopy, "--add-section=" + name + "=" + str(payload), self.release, library])
            for profile in ("release", "profile"):
                with self.subTest(name=name, profile=profile), self.assertRaises(ValueError):
                    self.verify(library, profile)

    def test_malformed_elf_headers_and_section_bounds_fail_with_matching_receipt_hash(self) -> None:
        original = self.debug.read_bytes()
        malformed = {"truncated": original[:20], "magic": b"NOPE" + original[4:]}
        table = bytearray(original)
        struct.pack_into("<Q", table, 40, len(table) + 4096)
        malformed["section-table-outside-file"] = table
        contents = bytearray(original)
        header, _, _ = self.sections(contents)[".debug_info"]
        struct.pack_into("<Q", contents, header + 24, len(contents) + 4096)
        malformed["section-data-outside-file"] = contents
        for name, data in malformed.items():
            with self.subTest(name=name):
                library = self.root / (name + ".so")
                library.write_bytes(data)
                with self.assertRaises(ValueError):
                    self.verify(library, "debug")

    def test_entry_point_must_define_a_function_in_a_loadable_executable_section(self) -> None:
        original = self.debug.read_bytes()
        sections = self.sections(original)
        header, offset, size = sections[".dynsym"]
        entry_size = struct.unpack_from("<Q", original, header + 56)[0]
        strings_index = struct.unpack_from("<I", original, header + 40)[0]
        table = struct.unpack_from("<Q", original, 40)[0]
        section_size = struct.unpack_from("<H", original, 58)[0]
        strings_header = table + strings_index * section_size
        strings_offset, strings_size = struct.unpack_from("<QQ", original, strings_header + 24)
        strings = original[strings_offset:strings_offset + strings_size]
        symbol = next(position for position in range(offset, offset + size, entry_size)
                      if strings[struct.unpack_from("<I", original, position)[0]:].split(b"\0", 1)[0]
                      == b"gdpp_library_init")
        nonloadable = bytearray(original)
        debug_index = (sections[".debug_info"][0] - table) // section_size
        struct.pack_into("<H", nonloadable, symbol + 6, debug_index)
        outside_section = bytearray(original)
        struct.pack_into("<Q", outside_section, symbol + 8, 0xFFFFFFFFFFFFF000)
        nonexecutable_segment = bytearray(original)
        address = struct.unpack_from("<Q", original, symbol + 8)[0]
        program_table = struct.unpack_from("<Q", original, 32)[0]
        program_size, program_count = struct.unpack_from("<HH", original, 54)
        executable_header = next(position for position in
                                 range(program_table, program_table + program_size * program_count, program_size)
                                 if struct.unpack_from("<I", original, position)[0] == 1
                                 and struct.unpack_from("<I", original, position + 4)[0] & 1
                                 and struct.unpack_from("<Q", original, position + 16)[0] <= address
                                 < sum(struct.unpack_from("<Q", original, position + offset)[0] for offset in (16, 40)))
        flags = struct.unpack_from("<I", original, executable_header + 4)[0]
        struct.pack_into("<I", nonexecutable_segment, executable_header + 4, flags & ~1)
        for name, data in (("nonloadable-definition", nonloadable),
                           ("unmapped-function-address", outside_section),
                           ("nonexecutable-load-segment", nonexecutable_segment)):
            with self.subTest(name=name):
                library = self.root / (name + ".so")
                library.write_bytes(data)
                with self.assertRaises(ValueError):
                    self.verify(library, "debug")


if __name__ == "__main__":
    unittest.main()
