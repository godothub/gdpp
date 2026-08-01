#!/usr/bin/env python3
"""Keep project and manifest paths independent of the Windows ANSI code page."""

from __future__ import annotations

import re
import sys
from pathlib import Path


DIRECT_PATH_CONVERSION = re.compile(
    r"\.(?:string|generic_string|u8string|generic_u8string)\s*\(\s*\)"
)
SOURCE_SUFFIXES = {".cpp", ".hpp"}
ALLOWED_CONVERSION_FILE = Path("include/gdpp/core/path_utf8.hpp")


def production_sources(source_root: Path) -> list[Path]:
    sources: list[Path] = []
    for directory in ("src", "include"):
        sources.extend(
            path
            for path in (source_root / directory).rglob("*")
            if path.suffix in SOURCE_SUFFIXES
        )
    return sorted(sources)


def violations(source_root: Path) -> list[str]:
    findings: list[str] = []
    for path in production_sources(source_root):
        relative = path.relative_to(source_root)
        if relative == ALLOWED_CONVERSION_FILE:
            continue
        for line_number, line in enumerate(
            path.read_text(encoding="utf-8").splitlines(), start=1
        ):
            if DIRECT_PATH_CONVERSION.search(line):
                findings.append(f"{relative}:{line_number}: {line.strip()}")
    return findings


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: path_encoding_audit_test.py <source-root>", file=sys.stderr)
        return 2
    findings = violations(Path(sys.argv[1]).resolve())
    if findings:
        print(
            "filesystem paths must cross UTF-8 boundaries through core/path_utf8.hpp:",
            file=sys.stderr,
        )
        print("\n".join(findings), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
