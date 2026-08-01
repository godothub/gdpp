#!/usr/bin/env python3
"""Validate exact, newline-independent contracts in a process log."""

from __future__ import annotations

import argparse
from pathlib import Path


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    parser.add_argument("--line", action="append", default=[])
    parser.add_argument("--contains", action="append", default=[])
    arguments = parser.parse_args()
    if not arguments.line and not arguments.contains:
        parser.error("at least one --line or --contains contract is required")
    return arguments


def validate(log: Path, required_lines: list[str], required_text: list[str]) -> list[str]:
    try:
        payload = log.read_bytes()
    except OSError as error:
        return [f"cannot read {log}: {error}"]
    text = payload.decode("utf-8", errors="replace")
    lines = set(text.splitlines())
    failures = [
        f"{log}: missing exact line {line!r}"
        for line in required_lines
        if line not in lines
    ]
    failures.extend(
        f"{log}: missing text {fragment!r}"
        for fragment in required_text
        if fragment not in text
    )
    return failures


def main() -> int:
    arguments = parse_arguments()
    failures = validate(arguments.log, arguments.line, arguments.contains)
    if failures:
        for failure in failures:
            print(f"log contract: {failure}")
        return 1
    print(
        f"log contract: {arguments.log} satisfies "
        f"{len(arguments.line) + len(arguments.contains)} requirement(s)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
