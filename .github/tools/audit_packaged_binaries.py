#!/usr/bin/env python3
"""Reject checkout paths in every native binary shipped by a GDPP release."""

from __future__ import annotations

import argparse
from pathlib import Path
import re


NATIVE_SUFFIXES = {".a", ".dll", ".dylib", ".lib", ".so"}
SCAN_CHUNK_SIZE = 4 * 1024 * 1024
SCAN_OVERLAP = 4096
GENERIC_CHECKOUT_PATTERNS = (
    re.compile(rb"/home/runner/work/"),
    re.compile(rb"/Users/runner/work/"),
    re.compile(rb"[A-Za-z]:[\\/](?:a|actions-runner[\\/]_work)[\\/]", re.IGNORECASE),
)


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--addon", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, required=True)
    return parser.parse_args()


def native_products(addon: Path) -> list[Path]:
    roots = (addon / "binary", addon / "sdk/lib")
    products = {
        path
        for root in roots
        if root.is_dir()
        for path in root.rglob("*")
        if path.is_file() and path.suffix.lower() in NATIVE_SUFFIXES
    }
    return sorted(products, key=lambda path: path.relative_to(addon).as_posix())


def encoded_needles(source_root: Path) -> set[bytes]:
    roots = {
        str(source_root.resolve()),
        source_root.resolve().as_posix(),
    }
    needles: set[bytes] = set()
    for root in roots:
        for separator in ("/", "\\"):
            normalized = root.replace("/", separator).replace("\\", separator)
            needles.add(normalized.encode("utf-8"))
            needles.add(normalized.encode("utf-16-le"))
    return {needle for needle in needles if needle}


def leaks_checkout_path(payload: bytes, needles: set[bytes]) -> bool:
    if any(needle in payload for needle in needles):
        return True
    if any(pattern.search(payload) is not None for pattern in GENERIC_CHECKOUT_PATTERNS):
        return True
    return any(
        pattern.search(payload[offset::2]) is not None
        for pattern in GENERIC_CHECKOUT_PATTERNS
        for offset in (0, 1)
    )


def file_leaks_checkout_path(path: Path, needles: set[bytes]) -> bool:
    overlap = b""
    overlap_size = max(SCAN_OVERLAP, *(len(needle) for needle in needles))
    with path.open("rb") as stream:
        while chunk := stream.read(SCAN_CHUNK_SIZE):
            payload = overlap + chunk
            if leaks_checkout_path(payload, needles):
                return True
            overlap = payload[-overlap_size:]
    return False


def audit(addon: Path, source_root: Path) -> list[str]:
    addon = addon.resolve()
    products = native_products(addon)
    if not products:
        return ["release contains no native products to audit"]
    needles = encoded_needles(source_root)
    return [
        path.relative_to(addon).as_posix()
        for path in products
        if file_leaks_checkout_path(path, needles)
    ]


def main() -> int:
    options = arguments()
    failures = audit(options.addon, options.source_root)
    if failures:
        for relative in failures:
            print(f"binary path audit: checkout path in {relative}")
        return 1
    print(
        "binary path audit: all packaged editor binaries and SDK archives are clean"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
