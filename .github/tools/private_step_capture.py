#!/usr/bin/env python3
"""Emit a bounded, source-safe summary for one private CI command."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import re


TAIL_LIMIT = 8 * 1024 * 1024
SAFE_IDENTIFIER = re.compile(r"[^A-Za-z0-9_.:+-]+")
FAILED_TEST = re.compile(
    rb"(?m)^\s*\d+\s+-\s+([A-Za-z0-9_.:+-]{1,160})\s+"
    rb"\((?:Failed|Timeout|SEGFAULT|Not Run)\)\s*$"
)
FAILED_CASE = re.compile(rb"(?m)^\[fail\] ([^\r\n]{1,1024})\r?$")
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
        for pattern in (FAILED_CASE, FAILED_UNITTEST_CASE)
        for match in pattern.finditer(payload)
    })[:12]
    if cases:
        fields.append(f"cases={','.join(cases)}")
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
