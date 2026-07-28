#!/usr/bin/env python3
"""Shared contracts and deterministic archive helpers for GDPP releases."""

from __future__ import annotations

import hashlib
import re
import shutil
import stat
import zipfile
from dataclasses import dataclass
from pathlib import Path


SUPPORTED_GODOT_VERSIONS = ("4.4", "4.5", "4.6", "4.7")
SDK_SCHEMA = 12
STATIC_ADDON_FILES = (
    "build_progress.gd",
    "native_build_job.gd",
    "export_plugin.gd",
    "scene_transform_worker.gd",
    "gdpp.gdextension",
    "plugin.cfg",
    "plugin.gd",
)
HOST_SDK_PATHS = ("godot-cpp", "include", "lib", "src", "sdk.manifest")
FIXED_ZIP_TIMESTAMP = (1980, 1, 1, 0, 0, 0)


@dataclass(frozen=True)
class HostContract:
    platform: str
    architecture: str
    platform_minimum: str
    compiler_library: str
    fallback_library: str
    export_targets: tuple[str, ...]


HOSTS = {
    "mac-universal": HostContract(
        platform="macos",
        architecture="universal",
        platform_minimum="macOS_11.0",
        compiler_library="libgdpp_compiler.macos.universal.dylib",
        fallback_library="libgdpp_fallback.macos.universal.dylib",
        export_targets=(
            "macos-universal",
            "android-arm64",
            "ios-arm64",
            "web-wasm32-nothreads",
            "web-wasm32-threads",
        ),
    ),
    "linux-x64": HostContract(
        platform="linux",
        architecture="x86_64",
        platform_minimum="Ubuntu_22.04",
        compiler_library="libgdpp_compiler.linux.x86_64.so",
        fallback_library="libgdpp_fallback.linux.x86_64.so",
        export_targets=(
            "linux-x64",
            "android-arm64",
            "web-wasm32-nothreads",
            "web-wasm32-threads",
        ),
    ),
    "windows-x64": HostContract(
        platform="windows",
        architecture="x86_64",
        platform_minimum="Windows_10",
        compiler_library="gdpp_compiler.windows.x86_64.dll",
        fallback_library="gdpp_fallback.windows.x86_64.dll",
        export_targets=(
            "windows-x64",
            "android-arm64",
            "web-wasm32-nothreads",
            "web-wasm32-threads",
        ),
    ),
}


def fail(message: str) -> None:
    raise ValueError(message)


def require_descendant(path: Path, parent: Path, label: str) -> None:
    try:
        path.relative_to(parent)
    except ValueError as error:
        raise ValueError(f"{label} must remain below {parent}: {path}") from error


def read_plugin_version(plugin_cfg: Path) -> str:
    content = plugin_cfg.read_text(encoding="utf-8")
    matches = re.findall(r'^version\s*=\s*"([^"]+)"\s*$', content, re.MULTILINE)
    if len(matches) != 1 or not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", matches[0]):
        fail(f"plugin.cfg must contain exactly one unprefixed semantic version: {plugin_cfg}")
    return matches[0]


def read_extension_minimum(descriptor: Path) -> str:
    content = descriptor.read_text(encoding="utf-8")
    matches = re.findall(
        r'^compatibility_minimum\s*=\s*"([^"]+)"\s*$', content, re.MULTILINE
    )
    if len(matches) != 1:
        fail(f"GDExtension descriptor must declare one compatibility_minimum: {descriptor}")
    return matches[0]


def read_sdk_manifest(path: Path) -> tuple[int, dict[str, str]]:
    if not path.is_file():
        fail(f"SDK manifest is missing: {path}")
    lines = path.read_text(encoding="utf-8").splitlines()
    if not lines:
        fail(f"SDK manifest is empty: {path}")
    header = re.fullmatch(r"GDPP_SDK ([0-9]+)", lines[0])
    if not header:
        fail(f"SDK manifest header is invalid: {path}")
    fields: dict[str, str] = {}
    for line in lines[1:]:
        key, separator, value = line.partition(" ")
        if not separator or not key or not value or key in fields:
            fail(f"SDK manifest field is invalid or duplicated in {path}: {line!r}")
        fields[key] = value
    return int(header.group(1)), fields


def require_fields(
    path: Path, schema: int, fields: dict[str, str], expected: dict[str, str]
) -> None:
    if schema != SDK_SCHEMA:
        fail(f"SDK schema must be {SDK_SCHEMA} in {path}, got {schema}")
    for key, value in expected.items():
        if fields.get(key) != value:
            fail(f"SDK field {key} must be {value!r} in {path}, got {fields.get(key)!r}")


def require_profile_libraries(directory: Path, profiles: tuple[str, ...]) -> None:
    if not directory.is_dir():
        fail(f"SDK library directory is missing: {directory}")
    names = sorted(path.name for path in directory.iterdir() if path.is_file())
    for profile in profiles:
        matches = [name for name in names if f".{profile}." in name]
        if len(matches) != 1:
            fail(
                f"SDK library directory must contain exactly one {profile} binding: "
                f"{directory}, found {matches}"
            )
    allowed = [
        name
        for name in names
        if any(f".{profile}." in name for profile in profiles)
    ]
    if len(allowed) != len(names):
        unexpected = sorted(set(names) - set(allowed))
        fail(f"SDK library directory contains unexpected bindings: {directory}: {unexpected}")


def copy_path(source: Path, destination: Path) -> None:
    if source.is_symlink():
        fail(f"release packages cannot contain symbolic links: {source}")
    if source.is_dir():
        shutil.copytree(source, destination, symlinks=False)
    else:
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)


def iter_files(root: Path) -> list[Path]:
    files: list[Path] = []
    for path in root.rglob("*"):
        if path.is_symlink():
            fail(f"release packages cannot contain symbolic links: {path}")
        if path.is_file():
            files.append(path)
    return sorted(files, key=lambda path: path.relative_to(root).as_posix())


def create_zip(stage_root: Path, archive: Path) -> None:
    files = iter_files(stage_root)
    if not files:
        fail("refusing to create an empty release archive")
    archive.parent.mkdir(parents=True, exist_ok=True)
    temporary = archive.with_suffix(archive.suffix + ".tmp")
    temporary.unlink(missing_ok=True)
    with zipfile.ZipFile(temporary, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as output:
        for path in files:
            relative = path.relative_to(stage_root).as_posix()
            info = zipfile.ZipInfo(relative, FIXED_ZIP_TIMESTAMP)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            info.external_attr = (stat.S_IFREG | 0o644) << 16
            with path.open("rb") as source, output.open(info, "w", force_zip64=True) as target:
                shutil.copyfileobj(source, target, length=1024 * 1024)
    temporary.replace(archive)

    with zipfile.ZipFile(archive) as packaged:
        names = packaged.namelist()
        if names != sorted(names) or len(names) != len(set(names)):
            fail(f"archive order or uniqueness check failed: {archive}")
        if any(name.startswith("/") or ".." in Path(name).parts for name in names):
            fail(f"archive contains an unsafe path: {archive}")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()
