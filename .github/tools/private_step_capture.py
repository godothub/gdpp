#!/usr/bin/env python3
"""Emit a bounded, source-safe summary for one private CI command."""

from __future__ import annotations

import argparse
from pathlib import Path
import re


TAIL_LIMIT = 8 * 1024 * 1024
SAFE_IDENTIFIER = re.compile(r"[^A-Za-z0-9_.:+-]+")
FAILED_TEST = re.compile(
    rb"(?m)^\s*\d+\s+-\s+([A-Za-z0-9_.:+-]{1,160})\s+"
    rb"\((?:Failed|Timeout|SEGFAULT|Not Run)\)\s*$"
)
PACKAGED_BINARY_PATH = re.compile(
    rb"(?m)^binary path audit: checkout path in "
    rb"((?:binary|sdk/lib)/[A-Za-z0-9_.+/-]{1,240})\s*$"
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
    text = payload.lower()
    categories = (
        (b"timeout", "timeout"),
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
    for marker, category in categories:
        if marker in text:
            return category
    return "command"


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
    paths = safe_package_paths(payload)
    if paths:
        fields.append(f"paths={','.join(paths)}")
    return " ".join(fields)


def main() -> int:
    options = parse_arguments()
    print(summary(options.log, options.status, options.job, options.step))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
