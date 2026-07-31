#!/usr/bin/env python3
"""Fail closed when a public diagnostic artifact contains private source material."""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path


FORBIDDEN_SUFFIXES = {
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
    ".i",
    ".ii",
    ".ipp",
    ".pdb",
}
PRIVATE_ROOTS = ("include/", "src/", "test/", "tools/")
PRIVATE_SOURCE_SUFFIXES = {
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".gd",
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
    ".ipp",
    ".js",
    ".py",
}
TEXT_LIMIT = 16 * 1024 * 1024
MIN_SOURCE_LINE = 32
MAX_REPORTED_VIOLATIONS = 20


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--path", type=Path, action="append", required=True)
    return parser.parse_args()


def tracked_private_lines(source: Path) -> set[str]:
    result = subprocess.run(
        ["git", "-C", str(source), "ls-files", "-z", "--", *PRIVATE_ROOTS],
        check=True,
        capture_output=True,
    )
    lines: set[str] = set()
    for relative in result.stdout.decode().split("\0"):
        if not relative:
            continue
        path = source / relative
        if (
            not path.is_file()
            or path.suffix.casefold() not in PRIVATE_SOURCE_SUFFIXES
            or path.stat().st_size > TEXT_LIMIT
        ):
            continue
        try:
            content = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        for line in content.splitlines():
            normalized = " ".join(line.split())
            if len(normalized) >= MIN_SOURCE_LINE:
                lines.add(normalized)
    return lines


def artifact_files(paths: list[Path]) -> list[Path]:
    files: list[Path] = []
    for path in paths:
        if not path.exists():
            continue
        if path.is_file():
            files.append(path)
        else:
            files.extend(candidate for candidate in path.rglob("*") if candidate.is_file())
    return sorted(set(files))


def main() -> int:
    options = arguments()
    source = options.source.resolve()
    private_lines = tracked_private_lines(source)
    violations: list[str] = []
    files = artifact_files(options.path)
    for path in files:
        if path.suffix.casefold() in FORBIDDEN_SUFFIXES:
            violations.append(f"{path}: private-source file type is forbidden")
            continue
        if path.stat().st_size > TEXT_LIMIT:
            continue
        try:
            content = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        for number, line in enumerate(content.splitlines(), start=1):
            normalized = " ".join(line.split())
            if len(normalized) >= MIN_SOURCE_LINE and normalized in private_lines:
                violations.append(f"{path}:{number}: exact private source line")
                break
    if violations:
        visible = violations[:MAX_REPORTED_VIOLATIONS]
        remaining = len(violations) - len(visible)
        suffix = f"\n... and {remaining} more violation(s)" if remaining else ""
        raise SystemExit(
            "Public artifact source-disclosure audit failed:\n"
            + "\n".join(visible)
            + suffix
        )
    print(f"Audited {len(files)} public artifact files without private source disclosure.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
