#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import re


def fail(message: str) -> None:
    raise SystemExit(f"custom add-on audit failed: {message}")


def read_fields(path: Path, signature: str) -> dict[str, str]:
    if not path.is_file():
        fail(f"manifest is missing: {path}")
    lines = path.read_text(encoding="utf-8").splitlines()
    if not lines or lines[0] != signature:
        fail(f"{path.name} signature differs from {signature!r}")
    fields: dict[str, str] = {}
    for line in lines[1:]:
        if not line:
            continue
        key, separator, value = line.partition(" ")
        if not separator or not key or not value or key in fields:
            fail(f"{path.name} contains an invalid or duplicate field: {line!r}")
        fields[key] = value
    return fields


def require_field(fields: dict[str, str], key: str, expected: str, label: str) -> None:
    actual = fields.get(key)
    if actual != expected:
        fail(f"{label} {key} is {actual!r}, expected {expected!r}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--addon", required=True, type=Path)
    parser.add_argument("--api", required=True, type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--godot-version", required=True)
    parser.add_argument("--precision", required=True, choices=("single", "double"))
    arguments = parser.parse_args()

    addon = arguments.addon.resolve()
    api = arguments.api.resolve()
    if not addon.is_dir():
        fail(f"add-on root is missing: {addon}")
    if not api.is_file():
        fail(f"custom extension API is missing: {api}")

    api_digest = hashlib.sha256(api.read_bytes()).hexdigest()
    package = read_fields(addon / "PACKAGE_MANIFEST.txt", "GDPP_CUSTOM_PACKAGE 1")
    require_field(package, "version", arguments.version, "package")
    require_field(package, "api", arguments.godot_version, "package")
    require_field(package, "api_sha256", api_digest, "package")
    require_field(package, "precision", arguments.precision, "package")
    require_field(package, "layout", "addons/gdpp", "package")
    if not re.fullmatch(r"(linux|macos|windows)/(arm64|universal|x86_64)", package.get("host", "")):
        fail(f"package host is invalid: {package.get('host')!r}")

    sdk_root = addon / "sdk"
    sdk_versions = sorted(path.name for path in sdk_root.iterdir() if path.is_dir())
    if sdk_versions != [arguments.godot_version]:
        fail(f"custom package SDK versions are {sdk_versions!r}")
    sdk = sdk_root / arguments.godot_version
    manifest = read_fields(sdk / "sdk.manifest", "GDPP_SDK 12")
    require_field(manifest, "api", arguments.godot_version, "SDK")
    require_field(manifest, "api_kind", "custom", "SDK")
    require_field(manifest, "api_sha256", api_digest, "SDK")
    require_field(manifest, "precision", arguments.precision, "SDK")
    require_field(manifest, "gdpp_version", arguments.version, "SDK")
    require_field(manifest, "runtime_abi", "18", "SDK")
    require_field(manifest, "distribution_binding", "template_release", "SDK")
    require_field(manifest, "distribution_optimization", "Release", "SDK")

    libraries = sorted((sdk / "lib").glob("*"))
    if len(libraries) != 1 or not libraries[0].is_file():
        fail(f"custom SDK must contain exactly one distribution library, found {libraries!r}")
    library_name = libraries[0].name
    required_fragment = f".template_release.{arguments.precision}."
    if required_fragment not in library_name:
        fail(f"custom SDK library does not encode {required_fragment!r}: {library_name}")

    descriptor = (addon / "gdpp.gdextension").read_text(encoding="utf-8")
    if 'entry_symbol = "gdpp_library_init"' not in descriptor:
        fail("custom descriptor does not use gdpp_library_init")
    if f".editor.{arguments.precision}." not in descriptor:
        fail("custom descriptor does not require the selected precision feature")
    other_precision = "double" if arguments.precision == "single" else "single"
    if f".editor.{other_precision}." in descriptor:
        fail("custom descriptor mixes precision ABIs")

    binary_files = sorted(path for path in (addon / "binary").iterdir() if path.is_file())
    if len(binary_files) != 2:
        fail(f"custom add-on must contain compiler and fallback binaries, found {binary_files!r}")
    names = [path.name for path in binary_files]
    if not any("gdpp_compiler" in name for name in names):
        fail("custom compiler binary is missing")
    if not any("gdpp_fallback" in name for name in names):
        fail("custom fallback binary is missing")

    plugin = (addon / "plugin.cfg").read_text(encoding="utf-8")
    if f'version="{arguments.version}"' not in plugin:
        fail("plugin.cfg version differs from the custom package")

    print(
        f"custom add-on audit passed: Godot {arguments.godot_version} "
        f"{arguments.precision}, API {api_digest}"
    )


if __name__ == "__main__":
    main()
