#!/usr/bin/env python3
"""Stage one commercial host add-on with only its runtime-loadable binaries."""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

import package_release


def fail(message: str) -> None:
    raise ValueError(message)


def require_no_symlinks(root: Path) -> None:
    if root.is_symlink():
        fail(f"host add-on cannot be a symbolic link: {root}")
    for path in root.rglob("*"):
        if path.is_symlink():
            fail(f"host add-on cannot contain symbolic links: {path}")


def normalize_godot_cpp_sources(addon: Path) -> None:
    for godot_version in package_release.SUPPORTED_GODOT_VERSIONS:
        source_root = addon / "sdk" / godot_version / "godot-cpp"
        if not source_root.is_dir():
            fail(f"host SDK godot-cpp source tree is missing: {source_root}")
        files = sorted(
            candidate for candidate in source_root.rglob("*") if candidate.is_file()
        )
        for path in files:
            try:
                content = path.read_bytes().decode("utf-8")
            except UnicodeDecodeError as error:
                raise ValueError(
                    f"host SDK godot-cpp source is not UTF-8 text: {path}"
                ) from error
            normalized = content.replace("\r\n", "\n").replace("\r", "\n")
            if normalized != content:
                path.write_bytes(normalized.encode("utf-8"))


def stage_host_component(source: Path, destination: Path, component_host: str) -> None:
    if component_host not in package_release.HOSTS:
        fail(f"unsupported host component: {component_host}")
    if source.name != "gdpp" or source.parent.name != "addons" or not source.is_dir():
        fail(f"source path must be an existing addons/gdpp directory: {source}")
    if destination.exists():
        fail(f"host staging destination already exists: {destination}")
    require_no_symlinks(source)
    if (source / "build").exists():
        fail(f"host add-on still contains generated project products: {source / 'build'}")

    host = package_release.HOSTS[component_host]
    expected_binaries = (host.compiler_library, host.fallback_library)
    source_binary = source / "binary"
    for library in expected_binaries:
        path = source_binary / library
        if not path.is_file():
            fail(f"host runtime binary is missing: {path}")
    executor = source / host.build_executor
    if not executor.is_file():
        fail(f"bundled Ninja build executor is missing: {executor}")
    ninja_license = source / "tools/NINJA-LICENSE.txt"
    ninja_version = source / "tools/NINJA-VERSION.txt"
    ninja_gdignore = source / "tools/.gdignore"
    for path in (ninja_license, ninja_version, ninja_gdignore):
        if not path.is_file():
            fail(f"bundled Ninja metadata is missing: {path}")
    expected_version = (
        f"Ninja {package_release.NINJA_VERSION}\n"
        f"commit {package_release.NINJA_COMMIT}\n"
    )
    if ninja_version.read_text(encoding="utf-8") != expected_version:
        fail(f"bundled Ninja version metadata is invalid: {ninja_version}")

    source_root = source.resolve()

    def ignore_root_products(path: str, names: list[str]) -> set[str]:
        resolved = Path(path).resolve()
        if resolved == source_root:
            return {"binary", "build"}.intersection(names)
        if resolved == (source_root / "tools"):
            return {
                name
                for name in names
                if name in package_release.HOSTS and name != component_host
            }
        return set()

    shutil.copytree(source, destination, ignore=ignore_root_products)
    normalize_godot_cpp_sources(destination)
    destination_binary = destination / "binary"
    destination_binary.mkdir()
    for library in expected_binaries:
        shutil.copy2(source_binary / library, destination_binary / library)
    staged_executor = destination / host.build_executor
    staged_executor.chmod(0o755)

    actual_binaries = {
        path.name for path in destination_binary.iterdir() if path.is_file()
    }
    if actual_binaries != set(expected_binaries):
        fail(
            f"staged host component must contain exactly {sorted(expected_binaries)}, "
            f"got {sorted(actual_binaries)}"
        )
    actual_executors = {
        path.relative_to(destination).as_posix()
        for path in (destination / "tools").glob("*/gdpp-ninja*")
        if path.is_file()
    }
    if actual_executors != {host.build_executor}:
        fail(
            f"staged host component must contain exactly {host.build_executor}, "
            f"got {sorted(actual_executors)}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("--host", choices=sorted(package_release.HOSTS), required=True)
    arguments = parser.parse_args()
    try:
        stage_host_component(
            arguments.source.resolve(),
            arguments.destination.resolve(),
            arguments.host,
        )
    except (OSError, ValueError) as error:
        print(f"host component staging failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
