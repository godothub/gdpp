#!/usr/bin/env python3
"""Build reproducible, target-specific Android compiler SDK modules for GDPP."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess

from godot_api_contract import validate_godot_api_contract


ANDROID_ABIS = {"arm64": "arm64-v8a", "x86_64": "x86_64"}
GODOT_TARGET = "template_release"


def clean_environment() -> dict[str, str]:
    environment = os.environ.copy()
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    return environment


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--addon-root", type=Path, required=True)
    parser.add_argument("--ndk-root", type=Path, required=True)
    parser.add_argument("--godot-version", required=True)
    parser.add_argument("--api-file", type=Path, required=True)
    parser.add_argument("--api-kind", choices=("official", "custom"), required=True)
    parser.add_argument("--api-sha256", required=True)
    parser.add_argument("--precision", choices=("single", "double"), required=True)
    parser.add_argument("--architecture", choices=sorted(ANDROID_ABIS), required=True)
    parser.add_argument("--api-level", type=int, default=28)
    parser.add_argument("--schema", type=int, required=True)
    parser.add_argument("--runtime-abi", type=int, required=True)
    parser.add_argument("--gdpp-version", required=True)
    return parser.parse_args()


def run(command: list[str]) -> None:
    print("+", subprocess.list2cmdline(command), flush=True)
    subprocess.run(command, check=True, env=clean_environment())


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def require_descendant(path: Path, parent: Path, label: str) -> None:
    try:
        path.relative_to(parent)
    except ValueError as error:
        raise SystemExit(f"{label} must remain below {parent}: {path}") from error


def main() -> int:
    args = parse_args()
    source_root = args.source_root.resolve()
    build_root = args.build_root.resolve()
    addon_root = args.addon_root.resolve()
    ndk_root = args.ndk_root.resolve()
    require_descendant(build_root, source_root / "build", "Android SDK build root")

    toolchain = ndk_root / "build/cmake/android.toolchain.cmake"
    if not toolchain.is_file():
        raise SystemExit(f"Android NDK CMake toolchain does not exist: {toolchain}")
    if args.api_level != 28:
        raise SystemExit("GDPP Android SDK ABI baseline is fixed at API level 28 (Android 9)")

    godot_cpp = source_root / "third/godot-cpp"
    try:
        api_contract = validate_godot_api_contract(
            args.api_file,
            args.godot_version,
            args.precision,
            args.api_kind,
            args.api_sha256,
        )
    except ValueError as error:
        raise SystemExit(str(error)) from error
    runtime_header = source_root / "include/gdpp/runtime/variant_ops.hpp"
    reference_semantics_header = source_root / "include/gdpp/runtime/reference_semantics.hpp"
    runtime_source = source_root / "src/runtime/variant_ops.cpp"
    attached_runtime_files = (
        source_root / "include/gdpp/runtime/attached_script.hpp",
        source_root / "src/runtime/attached_script_registry.cpp",
        source_root / "src/runtime/attached_script_instance.cpp",
        source_root / "src/runtime/attached_script_language.cpp",
    )
    integer_semantics_header = source_root / "include/gdpp/numeric/integer_semantics.hpp"
    for required in (
        godot_cpp / "CMakeLists.txt",
        runtime_header,
        reference_semantics_header,
        runtime_source,
        *attached_runtime_files,
        integer_semantics_header,
    ):
        if not required.is_file():
            raise SystemExit(f"missing Android SDK input: {required}")
    architecture = args.architecture
    abi = ANDROID_ABIS[architecture]
    target_root = addon_root / "sdk" / args.godot_version / "android" / architecture
    stage = build_root / "stage" / args.godot_version / architecture
    if stage.exists():
        shutil.rmtree(stage)
    (stage / "include/gdpp/runtime").mkdir(parents=True)
    (stage / "include/gdpp/numeric").mkdir(parents=True)
    (stage / "src/runtime").mkdir(parents=True)
    (stage / "godot-cpp/gen").mkdir(parents=True)
    (stage / "lib").mkdir(parents=True)

    directory = build_root / args.godot_version / architecture / "release"
    configure_command = [
        "cmake",
        "-S",
        str(godot_cpp),
        "-B",
        str(directory),
        "-G",
        "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
        f"-DANDROID_ABI={abi}",
        f"-DANDROID_PLATFORM={args.api_level}",
        "-DANDROID_STL=c++_shared",
        f"-DGODOTCPP_TARGET={GODOT_TARGET}",
        "-DGODOTCPP_ENABLE_TESTING=OFF",
        "-DGODOTCPP_SYSTEM_HEADERS=ON",
    ]
    configure_command.extend(api_contract.godot_cpp_cmake_arguments())
    run(configure_command)
    run(["cmake", "--build", str(directory), "--target", "godot-cpp", "--parallel"])
    precision_suffix = ".double" if args.precision == "double" else ""
    expected = directory / "bin" / (
        f"libgodot-cpp.android.{GODOT_TARGET}{precision_suffix}.{architecture}.a"
    )
    if not expected.is_file():
        candidates = sorted((directory / "bin").glob(f"*{GODOT_TARGET}*{architecture}*.a"))
        if len(candidates) != 1:
            raise SystemExit(
                f"expected one Android distribution godot-cpp library, found {candidates}"
            )
        expected = candidates[0]
    packaged_library = stage / "lib" / expected.name
    shutil.copy2(expected, packaged_library)

    strip_candidates = sorted((ndk_root / "toolchains/llvm/prebuilt").glob("*/bin/llvm-strip*"))
    strip_tools = [path for path in strip_candidates if path.name in {"llvm-strip", "llvm-strip.exe"}]
    if len(strip_tools) != 1:
        raise SystemExit(f"expected one Android NDK llvm-strip, found {strip_tools}")
    # Static archives need exported/relocation symbols, not compiler-local symbol tables.
    # Removing only unneeded symbols cuts target-pack size substantially without changing ABI.
    run([str(strip_tools[0]), "--strip-unneeded", str(packaged_library)])

    shutil.copytree(godot_cpp / "include", stage / "godot-cpp/include", dirs_exist_ok=True)
    shutil.copytree(directory / "gen/include", stage / "godot-cpp/gen/include", dirs_exist_ok=True)
    shutil.copy2(godot_cpp / "LICENSE.md", stage / "godot-cpp/LICENSE.md")
    shutil.copy2(runtime_header, stage / "include/gdpp/runtime/variant_ops.hpp")
    shutil.copy2(
        reference_semantics_header,
        stage / "include/gdpp/runtime/reference_semantics.hpp",
    )
    shutil.copy2(runtime_source, stage / "src/runtime/variant_ops.cpp")
    shutil.copy2(attached_runtime_files[0], stage / "include/gdpp/runtime/attached_script.hpp")
    for source in attached_runtime_files[1:]:
        shutil.copy2(source, stage / "src/runtime" / source.name)
    shutil.copy2(
        integer_semantics_header,
        stage / "include/gdpp/numeric/integer_semantics.hpp",
    )

    ndk_revision = "unknown"
    properties = ndk_root / "source.properties"
    if properties.is_file():
        for line in properties.read_text(encoding="utf-8").splitlines():
            if line.startswith("Pkg.Revision"):
                ndk_revision = line.partition("=")[2].strip()
                break
    manifest = (
        f"GDPP_SDK {args.schema}\n"
        f"api {args.godot_version}\n"
        f"api_kind {args.api_kind}\n"
        f"api_sha256 {args.api_sha256}\n"
        f"precision {args.precision}\n"
        "platform android\n"
        f"arch {architecture}\n"
        "profiles debug,release\n"
        "distribution_binding template_release\n"
        "distribution_optimization Release\n"
        "platform_minimum Android_9\n"
        f"gdpp_version {args.gdpp_version}\n"
        "cxx_standard 17\n"
        "exceptions disabled\n"
        "msvc_runtime not_applicable\n"
        f"runtime_abi {args.runtime_abi}\n"
        f"runtime_header_sha256 {sha256(runtime_header)}\n"
        f"reference_semantics_header_sha256 {sha256(reference_semantics_header)}\n"
        f"runtime_source_sha256 {sha256(runtime_source)}\n"
        f"attached_runtime_header_sha256 {sha256(attached_runtime_files[0])}\n"
        f"attached_runtime_registry_source_sha256 {sha256(attached_runtime_files[1])}\n"
        f"attached_runtime_instance_source_sha256 {sha256(attached_runtime_files[2])}\n"
        f"attached_runtime_language_source_sha256 {sha256(attached_runtime_files[3])}\n"
        f"integer_semantics_header_sha256 {sha256(integer_semantics_header)}\n"
        f"compiler Android_NDK\n"
        f"compiler_version {ndk_revision}\n"
        f"android_api_level {args.api_level}\n"
        "android_stl c++_shared\n"
    )
    (stage / "sdk.manifest").write_text(manifest, encoding="utf-8", newline="\n")

    target_root.parent.mkdir(parents=True, exist_ok=True)
    if target_root.exists():
        shutil.rmtree(target_root)
    shutil.copytree(stage, target_root)
    total = sum(path.stat().st_size for path in target_root.rglob("*") if path.is_file())
    print(
        f"Packaged Godot {args.godot_version} Android/{architecture} SDK: "
        f"{total / (1024 * 1024):.2f} MiB -> {target_root}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
