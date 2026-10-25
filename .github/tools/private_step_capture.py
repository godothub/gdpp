#!/usr/bin/env python3
"""Emit a bounded, source-safe summary for one private CI command."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re


TAIL_LIMIT = 8 * 1024 * 1024
EDITOR_PROGRESS_MARKER = b"GDPP_EDITOR_PROGRESS_DIAGNOSTIC="
EDITOR_PROGRESS_ENGINE_FILES = frozenset((
    "os_unix.cpp", "os_linuxbsd.cpp", "display_server_x11.cpp", "viewport.cpp",
    "window.cpp", "node.cpp", "control.cpp", "resource.cpp", "resource_loader.cpp",
    "object.cpp", "editor_node.cpp", "code_edit.cpp", "main.cpp", "script_editor_plugin.cpp",
))
EDITOR_PROGRESS_PHASES = frozenset((
    "startup", "complete", "unknown", "without_progress", "initial_idle", "begin",
    "repeated_begin", "finish", "cancel_positive", "cancel_finish", "failure_finish",
    "main_resize", "popup_reparent", "popup_resize", "popup_finish", "restore_main",
    "unload_then_resize", "idle_before_unload", "unload_idle",
))
EDITOR_PROGRESS_REASONS = frozenset((
    "unknown", "missing_receipt", "invalid_receipt", "capability", "script_editor",
    "code_edit", "begin_capability", "repeat_surface", "finish_surface", "cancel_baseline",
    "cancel_active", "resize", "reparent", "active_unload", "overlap", "input_delivery",
    "wheel_scroll", "symbol_position", "symbol_lookup", "idle_surface", "active_surface",
    "centering", "script_error", "engine_error", "process_exit", "timeout",
))
SAFE_IDENTIFIER = re.compile(r"[^A-Za-z0-9_.:+-]+")
FAILED_TEST = re.compile(
    rb"(?m)^\s*\d+\s+-\s+([A-Za-z0-9_.:+-]{1,160})\s+"
    rb"\((?:Failed|Timeout|SEGFAULT|Not Run)\)\s*$"
)
FAILED_CASE = re.compile(rb"(?m)^\[fail\] ([^\r\n]{1,1024})\r?$")
FAILED_EDITOR_CASE = re.compile(
    rb"GDPP_EDITOR_WORKFLOW_FAILED: ([^:\r\n]{1,512})(?=:|[\r\n]|$)"
)
OFFICIAL_RUNTIME_PHASE = re.compile(
    rb"(?m)^(?:[0-9]{1,8}: )?GDPP_OFFICIAL_RUNTIME_PHASE stage=(import|source|export|aot) "
    rb"case=(none|[0-9a-f]{16}) state=(start|done|failed)[ \t]*\r?$"
)
ADDON_COLD_IMPORT_PHASE = re.compile(
    rb"(?m)^(?:[0-9]{1,8}: )?GDPP_ADDON_COLD_IMPORT_PHASE "
    rb"phase=(export|other-export|repack|pack-reject|repack-after-rejection|cold-import|editor|runtime) "
    rb"state=(start|done|failed)[ \t]*\r?$"
)
FAILED_UNITTEST_CASE = re.compile(
    rb"(?m)^(?:FAIL|ERROR): ([A-Za-z_][A-Za-z0-9_]{0,159}) "
    rb"\([A-Za-z_][A-Za-z0-9_.]{0,319}\)(?:[ \t]+\([^\r\n]{0,1024}\))?[ \t]*\r?$"
)
FAILED_DOWNLOAD = re.compile(
    rb"(?m)^GDPP_DOWNLOAD_FAILED artifact=(editor|templates|other) "
    rb"http=([0-9]{3}) curl=([0-9]{1,3})\r?$"
)
PACKAGED_BINARY_PATH = re.compile(
    rb"(?m)^binary path audit: checkout path in "
    rb"((?:binary|sdk/lib)/[A-Za-z0-9_.+/-]{1,240})\s*$"
)
COMPILER_DIAGNOSTIC_CODE = re.compile(
    rb"(?:(?:\[|,)(-W(?:error=)?[A-Za-z0-9_+.-]{1,80})(?=[,\]])|\b(?:fatal error|error|warning) (C[0-9]{4}|LNK[0-9]{4})\b|\b(GDS[1-5][0-9]{3})\b)"
)
TIMEOUT_FAILURE = re.compile(
    rb"\btimed out\b|\bTimeout(?:Error|Expired)\b|\*\*\*Timeout\b|\(Timeout\)",
    re.IGNORECASE,
)
COMPILER_ERROR = re.compile(
    rb"(?m)^(?:[^\r\n]{1,1024}:\d+:(?:\d+:)?\s*|(?:clang(?:\+\+)?|gcc|g\+\+):\s*)(?:fatal )?error:",
    re.IGNORECASE,
)
BUILD_FAILURE_MARKERS = (
    (b"killed: 9", "process-killed"),
    (b"killed signal terminated program", "process-killed"),
    (b"out of memory", "out-of-memory"),
    (b"cannot detect msvc header dependencies", "msvc-include-probe"),
    (b"msvc did not report the dependency probe header", "msvc-include-prefix"),
    (b"cannot bootstrap the msvc dependency launcher", "msvc-launcher-bootstrap"),
    (b"cannot execute the msvc dependency launcher", "msvc-launcher-execute"),
    (b"does not implement the dependency encoding protocol", "msvc-launcher-protocol"),
    (b"cannot prove the msvc unicode header dependency contract", "msvc-unicode-probe"),
    (b"cannot load the selected msvc native host environment", "msvc-host-environment"),
    (b"msvc native host environment is incomplete", "msvc-host-environment"),
    (b"cannot locate the selected msvc installation", "msvc-host-compiler"),
    (b"the selected msvc installation has no native host environment", "msvc-host-compiler"),
    (b"gdpp_msvc_dependency_host_launcher must be", "msvc-launcher-override"),
    (b"is not able to compile a simple test program", "cmake-compiler-probe"),
    (b"the cxx compiler identification is unknown", "cmake-compiler-identification"),
    (b"could not find compiler set in environment variable cxx", "cmake-compiler-discovery"),
    (b"could not find cmake_root", "cmake-installation"),
    (b"could not find any instance of visual studio", "cmake-msvc-installation"),
    (b"failed to run msbuild command", "cmake-msbuild-probe"),
    (b"cmake_make_program is not set", "cmake-build-tool"),
    (b"invalid character escape", "cmake-path-escape"),
    (b"does not appear to contain cmakelists.txt", "cmake-source-directory"),
    (b"each download failed", "dependency-download"),
    (b"hash mismatch", "dependency-digest"),
    (b"error evaluating generator expression", "generator-expression"),
)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--status", type=int, required=True)
    parser.add_argument("--job", required=True)
    parser.add_argument("--step", required=True)
    return parser.parse_args()


def safe_identifier(value: str, fallback: str) -> str:
    sanitized = SAFE_IDENTIFIER.sub("-", value).strip("-.")
    return sanitized[:96] or fallback


def bounded_tail(log: Path) -> bytes:
    try:
        with log.open("rb") as stream:
            stream.seek(0, 2)
            size = stream.tell()
            stream.seek(max(0, size - TAIL_LIMIT))
            return stream.read(TAIL_LIMIT)
    except OSError:
        return b""


def failure_category(payload: bytes) -> str:
    if FAILED_DOWNLOAD.search(payload):
        return "download"
    text = payload.lower()
    categories = (
        (b"killed: 9", "resource"),
        (b"killed signal terminated program", "resource"),
        (b"out of memory", "resource"),
        (b"addresssanitizer", "sanitizer"),
        (b"undefinedbehaviorsanitizer", "sanitizer"),
        (b"threadsanitizer", "sanitizer"),
        (b"cmake error", "configure"),
        (b"configuration failed", "configure"),
        (b"linker command failed", "link"),
        (b"undefined reference", "link"),
        (b"unresolved external", "link"),
        (b"fatal error", "compile"),
        (b"compilation terminated", "compile"),
        (b"error c", "compile"),
        (b"the following tests failed", "test"),
        (b"tests failed", "test"),
        (b"ctest", "test"),
        (b"script error", "godot"),
        (b"godot engine", "godot"),
        (b"binary path audit", "package"),
        (b"release packaging failed", "package"),
    )
    if TIMEOUT_FAILURE.search(payload):
        return "timeout"
    for marker, category in categories:
        if marker in text:
            return category
    if COMPILER_ERROR.search(payload):
        return "compile"
    return "command"


def build_failure_codes(payload: bytes) -> list[str]:
    codes = {
        (match.group(1) or match.group(2) or match.group(3)).decode("ascii")
        for match in COMPILER_DIAGNOSTIC_CODE.finditer(payload)
    }
    text = payload.lower()
    codes.update(name for marker, name in BUILD_FAILURE_MARKERS if marker in text)
    if re.search(rb"compatibility with cmake < [0-9.]+ has been removed", text):
        codes.add("cmake-policy-floor")
    return sorted(codes)[:12]


def failed_tests(payload: bytes) -> list[str]:
    names = {
        match.group(1).decode("ascii")
        for match in FAILED_TEST.finditer(payload)
    }
    return sorted(names)[:12]


def safe_package_paths(payload: bytes) -> list[str]:
    paths = {
        match.group(1).decode("ascii")
        for match in PACKAGED_BINARY_PATH.finditer(payload)
        if ".." not in match.group(1).decode("ascii").split("/")
    }
    return sorted(paths)[:8]


def editor_progress_diagnostic(payload: bytes) -> list[str]:
    """Accept one closed-schema fixture record, never a free-form diagnostic."""
    records = []
    for line in payload.splitlines():
        line = re.sub(rb"^[0-9]{1,8}: ", b"", line, count=1)
        if line.startswith(EDITOR_PROGRESS_MARKER):
            records.append(line[len(EDITOR_PROGRESS_MARKER):])
    if not records:
        return []
    if len(records) != 1 or len(records[0]) > 1024:
        return ["gui=invalid"]

    def unique_object(pairs):
        value = dict(pairs)
        if len(value) != len(pairs):
            raise ValueError("duplicate diagnostic field")
        return value

    try:
        value = json.loads(records[0].decode("ascii"), object_pairs_hook=unique_object)
        expected = {"schema", "phase", "reason", "last_phase", "returncode",
                    "script_errors", "engine_errors", "records", "engine_locations"}
        if not isinstance(value, dict) or set(value) != expected:
            raise ValueError("invalid diagnostic fields")
        if type(value["schema"]) is not int or value["schema"] != 2:
            raise ValueError("unsupported diagnostic schema")
        for name, choices in (("phase", EDITOR_PROGRESS_PHASES),
                              ("last_phase", EDITOR_PROGRESS_PHASES),
                              ("reason", EDITOR_PROGRESS_REASONS)):
            if not isinstance(value[name], str) or value[name] not in choices:
                raise ValueError("invalid diagnostic enum")
        for name, lower, upper in (("returncode", -(2 ** 31), 2 ** 32 - 1),
                                   ("script_errors", 0, 65535), ("engine_errors", 0, 65535),
                                   ("records", 0, 64)):
            if type(value[name]) is not int or not lower <= value[name] <= upper:
                raise ValueError("invalid diagnostic count")
        locations = value["engine_locations"]
        if not isinstance(locations, list) or len(locations) > 4:
            raise ValueError("invalid engine location count")
        identities = set()
        for location in locations:
            if not isinstance(location, dict) or set(location) != {"file", "line"}:
                raise ValueError("invalid engine location fields")
            if not isinstance(location["file"], str) or location["file"] not in EDITOR_PROGRESS_ENGINE_FILES:
                raise ValueError("unknown public engine file")
            if type(location["line"]) is not int or not 1 <= location["line"] <= 1000000:
                raise ValueError("invalid engine source line")
            identity = (location["file"], location["line"])
            if identity in identities:
                raise ValueError("duplicate engine location")
            identities.add(identity)
    except (ValueError, TypeError, UnicodeError, RecursionError):
        return ["gui=invalid"]
    fields = [
        f"gui={value['phase']}:{value['reason']}", f"gui_last={value['last_phase']}",
        f"gui_rc={value['returncode']}", f"gui_script_errors={value['script_errors']}",
        f"gui_engine_errors={value['engine_errors']}", f"gui_records={value['records']}",
    ]
    if locations:
        fields.append("gui_engine_at=" + ",".join(f"{location['file']}:{location['line']}" for location in locations))
    return fields


def summary(log: Path, status: int, job: str, step: str) -> str:
    safe_job = safe_identifier(job, "unknown-job")
    safe_step = safe_identifier(step, "run")
    if status == 0:
        return f"private-stage job={safe_job} step={safe_step} status=success"
    payload = bounded_tail(log)
    fields = [
        f"private-stage job={safe_job}",
        f"step={safe_step}",
        "status=failed",
        f"category={failure_category(payload)}",
        f"exit={status}",
    ]
    tests = failed_tests(payload)
    if tests:
        fields.append(f"tests={','.join(tests)}")
    cases = sorted({
        hashlib.sha256(match.group(1)).hexdigest()[:16]
        for pattern in (FAILED_CASE, FAILED_UNITTEST_CASE, FAILED_EDITOR_CASE)
        for match in pattern.finditer(payload)
    })[:12]
    if cases:
        fields.append(f"cases={','.join(cases)}")
    phases = list(OFFICIAL_RUNTIME_PHASE.finditer(payload))
    if phases:
        fields.append("runtime=" + ":".join(part.decode("ascii") for part in phases[-1].groups()))
    cold_phases = list(ADDON_COLD_IMPORT_PHASE.finditer(payload))
    if cold_phases:
        fields.append("cold_import=" + ":".join(part.decode("ascii") for part in cold_phases[-1].groups()))
    fields.extend(editor_progress_diagnostic(payload))
    codes = build_failure_codes(payload)
    if codes:
        fields.append(f"codes={','.join(codes)}")
    paths = safe_package_paths(payload)
    if paths:
        fields.append(f"paths={','.join(paths)}")
    downloads = FAILED_DOWNLOAD.findall(payload)
    if downloads:
        artifact, http_status, curl_status = (value.decode("ascii") for value in downloads[-1])
        fields.extend((f"artifact={artifact}", f"http={http_status}", f"curl={curl_status}"))
    return " ".join(fields)


def main() -> int:
    options = parse_arguments()
    print(summary(options.log, options.status, options.job, options.step))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
