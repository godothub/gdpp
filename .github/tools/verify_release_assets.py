#!/usr/bin/env python3
"""Verify the exact GDPP release-asset set and its canonical SHA-256 manifest."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path


ARCHIVE_NAME = "gdpp.zip"
CHECKSUM_NAME = "SHA256SUMS"
EXPECTED_NAMES = frozenset((ARCHIVE_NAME, CHECKSUM_NAME))


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify(directory: Path) -> None:
    if not directory.is_dir() or directory.is_symlink():
        raise ValueError("release asset root must be a real directory")

    entries = list(directory.iterdir())
    names = {entry.name for entry in entries}
    if names != EXPECTED_NAMES:
        missing = sorted(EXPECTED_NAMES - names)
        unexpected = sorted(names - EXPECTED_NAMES)
        raise ValueError(
            f"release asset set differs: missing={missing}, unexpected={unexpected}"
        )
    for entry in entries:
        if entry.is_symlink() or not entry.is_file():
            raise ValueError(f"release asset must be a regular file: {entry.name}")

    archive = directory / ARCHIVE_NAME
    if archive.stat().st_size == 0:
        raise ValueError(f"release archive is empty: {ARCHIVE_NAME}")
    digest = sha256(archive)
    expected_manifest = f"{digest}  {ARCHIVE_NAME}\n"
    manifest_path = directory / CHECKSUM_NAME
    if manifest_path.stat().st_size != len(expected_manifest):
        raise ValueError("release checksum manifest has a noncanonical size")
    try:
        manifest = manifest_path.read_bytes().decode("ascii")
    except (OSError, UnicodeError) as error:
        raise ValueError("release checksum manifest is not readable ASCII") from error
    if manifest != expected_manifest:
        raise ValueError("release checksum manifest is noncanonical or does not match")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    try:
        verify(args.directory)
    except (OSError, ValueError) as error:
        raise SystemExit(f"release assets: {error}") from error
    print(f"Verified {ARCHIVE_NAME} and {CHECKSUM_NAME}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
