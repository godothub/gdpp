#!/usr/bin/env python3
"""Verify an exported ELF against its native profile and content-bound build receipt."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct


PROFILES = ("debug", "release", "profile")
MACHINES = {"x86_64": 62, "arm64": 183}
MAXIMUM_ARTIFACT_BYTES = 2 * 1024**3


def reject(reason: str) -> None:
    raise ValueError(reason)


def read_artifact(path: Path) -> bytes:
    try:
        size = path.stat().st_size
        if size <= 0 or size > MAXIMUM_ARTIFACT_BYTES:
            reject("artifact_size")
        with path.open("rb") as stream:
            data = stream.read(size + 1)
        if len(data) != size:
            reject("artifact_size_changed")
        return data
    except OSError:
        reject("artifact_io")


def verify(library: Path, receipt: Path, profile: str, architecture: str,
           platform: str = "linux") -> dict:
    if profile not in PROFILES or architecture not in MACHINES or platform not in ("linux", "android"):
        reject("unsupported_target")
    data = read_artifact(library)
    try:
        with receipt.open("rb") as stream:
            raw_receipt = stream.read(1024**2 + 1)
        if len(raw_receipt) > 1024**2:
            reject("receipt_size")
        contract = json.loads(raw_receipt)
    except (OSError, ValueError, UnicodeError):
        reject("receipt_input")
    if not isinstance(contract, dict) or contract.get("schema") != "GDPP_NATIVE_BUILD_RECEIPT 1":
        reject("receipt_schema")
    if contract.get("build_profile") != profile:
        reject("receipt_profile")
    if contract.get("target_platform") != platform or contract.get("target_architecture") != architecture:
        reject("receipt_target")
    plan = contract.get("plan")
    execution = contract.get("execution")
    if not isinstance(plan, dict) or not isinstance(execution, dict):
        reject("receipt_fields")
    output = plan.get("output_library")
    # Packaged Android libraries are copied out of an APK. Bind their original filename and
    # exact bytes, rather than requiring the extracted copy to occupy the producer's path.
    if not isinstance(output, str) or not Path(output).is_absolute() or Path(output).name != library.name:
        reject("receipt_output")
    digest = hashlib.sha256(data).hexdigest()
    expected_digest = execution.get("artifact_content_sha256")
    if not isinstance(expected_digest, str) or not re.fullmatch(r"[0-9a-f]{64}", expected_digest):
        reject("receipt_digest")
    if digest != expected_digest:
        reject("artifact_digest")

    if len(data) < 64 or data[:7] != b"\x7fELF\x02\x01\x01":
        reject("elf_header")
    elf_type, machine, version = struct.unpack_from("<HHI", data, 16)
    if elf_type != 3 or machine != MACHINES[architecture] or version != 1:
        reject("elf_target")
    table = struct.unpack_from("<Q", data, 40)[0]
    entry_size, count, names_index = struct.unpack_from("<HHH", data, 58)

    def bounded(offset: int, size: int) -> bool:
        return offset <= len(data) and size <= len(data) - offset

    if entry_size < 64 or not table or not bounded(table, entry_size):
        reject("elf_section_table")

    def section(index: int) -> tuple:
        offset = table + index * entry_size
        if not bounded(offset, entry_size):
            reject("elf_section_table")
        return struct.unpack_from("<IIQQQQIIQQ", data, offset)

    zero = section(0)
    if zero[1] != 0:
        reject("elf_section_zero")
    if count == 0:
        count = zero[5]
        if count < 0xFF00:
            reject("elf_section_count")
    elif count >= 0xFF00 or zero[5] != 0:
        reject("elf_section_count")
    if count > (len(data) - table) // entry_size:
        reject("elf_section_table")
    if names_index == 0xFFFF:
        names_index = zero[6]
        if names_index < 0xFF00:
            reject("elf_section_names")
    elif names_index >= 0xFF00 or zero[6] != 0:
        reject("elf_section_names")
    if not names_index or names_index >= count:
        reject("elf_section_names")
    sections = [section(index) for index in range(count)]
    for item in sections:
        if item[1] not in (0, 8) and not bounded(item[4], item[5]):
            reject("elf_section_bounds")

    program_table = struct.unpack_from("<Q", data, 32)[0]
    program_size, program_count = struct.unpack_from("<HH", data, 54)
    if program_count == 0xFFFF:
        program_count = zero[7]
        if program_count < 0xFFFF:
            reject("elf_program_count")
    if (program_size < 56 or not program_count or not program_table
            or program_table > len(data)
            or program_count > (len(data) - program_table) // program_size):
        reject("elf_program_table")
    executable_loads = []
    for index in range(program_count):
        program = struct.unpack_from("<IIQQQQQQ", data, program_table + index * program_size)
        if program[0] != 1:
            continue
        if (program[5] > program[6] or not bounded(program[2], program[5])
                or program[3] + program[6] > 2**64):
            reject("elf_load_bounds")
        if program[1] & 1:
            executable_loads.append(program)

    def string(table_section: tuple, index: int) -> bytes:
        if (table_section[1] != 3 or not table_section[5] or index >= table_section[5]
                or data[table_section[4]] != 0
                or data[table_section[4] + table_section[5] - 1] != 0):
            reject("elf_string_table")
        begin = table_section[4] + index
        end = data.find(b"\0", begin, table_section[4] + table_section[5])
        if end < 0:
            reject("elf_string_table")
        return data[begin:end]

    names = [string(sections[names_index], item[0]) for item in sections]
    symtabs = [item for item in sections if item[1] == 2]
    for symbols in symtabs:
        if symbols[9] < 24 or symbols[5] % symbols[9] or symbols[6] >= count:
            reject("elf_symbol_table")
        if sections[symbols[6]][1] != 3:
            reject("elf_symbol_table")
    has_symtab = any(item[5] > item[9] for item in symtabs)
    dwarf_info = any(name in (b".debug_info", b".zdebug_info") and item[5] > 0
                     for name, item in zip(names, sections))
    dwarf_line = any(name in (b".debug_line", b".zdebug_line") and item[5] > 0
                     for name, item in zip(names, sections))
    debug_sections = [name for name in names if name.startswith((b".debug", b".zdebug"))
                      or name in (b".gnu_debugdata", b".gnu_debuglink", b".gnu_debugaltlink",
                                  b".gdb_index", b".stab", b".stabstr")]
    if profile == "debug":
        if not has_symtab or not dwarf_info or not dwarf_line:
            reject("debug_symbols_missing")
    elif symtabs or debug_sections:
        reject("distribution_symbols_present")

    dynamic_tables = 0
    exports = []
    for index, symbols in enumerate(sections):
        if symbols[1] != 11:
            continue
        dynamic_tables += 1
        if symbols[9] < 24 or symbols[5] % symbols[9] or symbols[6] >= count:
            reject("elf_dynamic_symbols")
        extended = [item for item in sections if item[1] == 18 and item[6] == index]
        symbol_count = symbols[5] // symbols[9]
        if len(extended) > 1 or (extended and (extended[0][9] != 4 or extended[0][5] != symbol_count * 4)):
            reject("elf_extended_symbols")
        for symbol_index in range(symbol_count):
            offset = symbols[4] + symbol_index * symbols[9]
            name_index, info, visibility, section_index = struct.unpack_from("<IBBH", data, offset)
            name = string(sections[symbols[6]], name_index)
            if section_index == 0:
                continue
            if info >> 4 not in (1, 2, 10) or visibility & 3 not in (0, 3):
                continue
            if section_index == 0xFFFF:
                if not extended:
                    reject("elf_extended_symbols")
                section_index = struct.unpack_from("<I", data, extended[0][4] + symbol_index * 4)[0]
                if section_index < 0xFF00:
                    reject("elf_extended_symbols")
            if name != b"gdpp_library_init" or info & 15 != 2 or not 0 < section_index < count:
                reject("elf_exports")
            target = sections[section_index]
            value, symbol_size = struct.unpack_from("<QQ", data, offset + 8)
            span = max(symbol_size, 1)
            if (target[1] != 1 or target[2] & 6 != 6 or value < target[3]
                    or value - target[3] >= target[5]
                    or span > target[5] - (value - target[3])):
                reject("elf_entry_not_executable")
            file_offset = target[4] + value - target[3]
            if not any(
                value >= segment[3] and value - segment[3] < segment[5]
                and span <= segment[5] - (value - segment[3])
                and file_offset == segment[2] + value - segment[3]
                for segment in executable_loads
            ):
                reject("elf_entry_not_loadable")
            exports.append(name)
    if dynamic_tables != 1 or exports != [b"gdpp_library_init"]:
        reject("elf_exports")
    return {"profile": profile, "platform": platform, "architecture": architecture,
            "has_symtab": has_symtab, "has_dwarf": dwarf_info and dwarf_line,
            "artifact_sha256": digest}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--receipt", type=Path, required=True)
    parser.add_argument("--profile", choices=PROFILES, required=True)
    parser.add_argument("--architecture", choices=tuple(MACHINES), required=True)
    parser.add_argument("--platform", choices=("linux", "android"), required=True)
    args = parser.parse_args()
    try:
        verify(args.library, args.receipt, args.profile, args.architecture, args.platform)
    except ValueError as error:
        print(f"GDPP_ELF_PROFILE_FAILED reason={error}")
        return 1
    print(f"GDPP_ELF_PROFILE_OK profile={args.profile} platform={args.platform} architecture={args.architecture}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
