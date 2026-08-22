#!/usr/bin/env python3
"""Synchronize the public release-facing files from the private source checkout."""

from __future__ import annotations

import argparse
import os
import tempfile
from pathlib import Path


RELEASE_FILES = (
    "CHANGELOG.md",
    "CHANGELOG-ZH.md",
    "README.md",
    "README-ZH.md",
)


def synchronize(source: Path, destination: Path) -> list[str]:
    payloads: dict[str, bytes] = {}
    for relative in RELEASE_FILES:
        source_path = source / relative
        if not source_path.is_file() or source_path.is_symlink():
            raise ValueError(f"private release file must be a regular file: {relative}")
        payloads[relative] = source_path.read_bytes()

    changed: list[str] = []
    for relative in RELEASE_FILES:
        destination_path = destination / relative
        payload = payloads[relative]
        if (
            destination_path.is_file()
            and not destination_path.is_symlink()
            and destination_path.read_bytes() == payload
        ):
            continue
        destination_path.parent.mkdir(parents=True, exist_ok=True)
        descriptor, temporary_name = tempfile.mkstemp(
            prefix=f".{destination_path.name}.",
            suffix=".tmp",
            dir=destination_path.parent,
        )
        temporary_path = Path(temporary_name)
        try:
            with os.fdopen(descriptor, "wb") as stream:
                stream.write(payload)
                stream.flush()
                os.fsync(stream.fileno())
            temporary_path.replace(destination_path)
        except BaseException:
            temporary_path.unlink(missing_ok=True)
            raise
        changed.append(relative)
    return changed


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("--github-output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    changed = synchronize(arguments.source.resolve(), arguments.destination.resolve())
    with arguments.github_output.open("a", encoding="utf-8", newline="\n") as output:
        output.write(f"changed={'true' if changed else 'false'}\n")
        output.write(f"files={','.join(changed)}\n")
    if changed:
        print("Synchronized public release files: " + ", ".join(changed))
    else:
        print("Public release files already match the private source.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
