#!/usr/bin/env python3
"""Build reproducible GDPP iOS target packs for devices and both Simulator CPUs."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import platform
import shutil
import subprocess

from godot_api_contract import validate_godot_api_contract


GODOT_TARGET = "template_release"
SLICES = {
    "device-arm64": ("OS64", "arm64"),
    "simulator-arm64": ("SIMULATORARM64", "arm64"),
    "simulator-x86_64": ("SIMULATOR64", "x86_64"),
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--addon-root", type=Path, required=True)
    parser.add_argument("--godot-version", choices=["4.4", "4.5", "4.6", "4.7"], required=True)
    parser.add_argument("--api-file", type=Path, required=True)
    parser.add_argument("--api-kind", choices=("official", "custom"), required=True)
    parser.add_argument("--api-sha256", required=True)
    parser.add_argument("--precision", choices=("single", "double"), required=True)
    parser.add_argument("--deployment-target", default="16.0")
    parser.add_argument("--schema", type=int, required=True)
    parser.add_argument("--runtime-abi", type=int, required=True)
    parser.add_argument("--gdpp-version", required=True)
    return parser.parse_args()


def environment() -> dict[str, str]:
    result = os.environ.copy()
    result["PYTHONDONTWRITEBYTECODE"] = "1"
    return result


def run(command: list[str]) -> None:
    print("+", subprocess.list2cmdline(command), flush=True)
    subprocess.run(command, check=True, env=environment())


def output(command: list[str]) -> str:
    return subprocess.run(
        command,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=environment(),
    ).stdout.strip()


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


def find_archive(
    directory: Path, target: str, architecture: str, precision: str
) -> Path:
    precision_suffix = ".double" if precision == "double" else ""
    expected = (
        directory
        / "bin"
        / f"libgodot-cpp.ios.{target}{precision_suffix}.{architecture}.a"
    )
    if expected.is_file():
        return expected
    candidates = sorted((directory / "bin").glob(f"*{target}*{architecture}*.a"))
    if len(candidates) != 1:
        raise SystemExit(
            f"expected one iOS {target}/{architecture} godot-cpp archive, found {candidates}"
        )
    return candidates[0]


def main() -> int:
    args = parse_args()
    if platform.system() != "Darwin":
        raise SystemExit("iOS target packs can only be built on macOS with Xcode")

    source_root = args.source_root.resolve()
    build_root = args.build_root.resolve()
    addon_root = args.addon_root.resolve()
    require_descendant(build_root, source_root / "build", "iOS SDK build root")
    for tool in ("cmake", "ninja", "xcrun", "xcodebuild"):
        if shutil.which(tool) is None:
            raise SystemExit(f"required iOS build tool was not found: {tool}")

    xcode_version = output(["xcodebuild", "-version"]).replace("\n", "_").replace(" ", "-")
    iphoneos_sdk = output(["xcrun", "--sdk", "iphoneos", "--show-sdk-version"])
    simulator_sdk = output(["xcrun", "--sdk", "iphonesimulator", "--show-sdk-version"])
    if args.deployment_target != "16.0":
        raise SystemExit("GDPP's commercial iOS baseline is fixed at iOS 16.0")
    deployment_target = args.deployment_target

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
    toolchain = godot_cpp / "cmake/ios.toolchain.cmake"
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
        toolchain,
        runtime_header,
        reference_semantics_header,
        runtime_source,
        *attached_runtime_files,
        integer_semantics_header,
    ):
        if not required.is_file():
            raise SystemExit(f"missing iOS SDK input: {required}")

    target_root = addon_root / "sdk" / args.godot_version / "ios/arm64"
    stage = build_root / "stage" / args.godot_version / "arm64"
    if stage.exists():
        shutil.rmtree(stage)
    for directory in (
        stage / "include/gdpp/runtime",
        stage / "include/gdpp/numeric",
        stage / "src/runtime",
        stage / "godot-cpp/gen",
        stage / "lib/device",
        stage / "lib/simulator",
    ):
        directory.mkdir(parents=True)

    generated_include: Path | None = None
    archives: dict[str, Path] = {}
    for slice_name, (toolchain_platform, architecture) in SLICES.items():
        directory = build_root / args.godot_version / "release" / slice_name
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
            f"-DPLATFORM={toolchain_platform}",
            f"-DDEPLOYMENT_TARGET={deployment_target}",
            f"-DGODOTCPP_TARGET={GODOT_TARGET}",
            "-DGODOTCPP_ENABLE_TESTING=OFF",
            "-DGODOTCPP_SYSTEM_HEADERS=ON",
            f"-DCMAKE_CXX_FLAGS=-ffile-prefix-map={source_root}=/gdpp",
        ]
        configure_command.extend(api_contract.godot_cpp_cmake_arguments())
        run(configure_command)
        run(["cmake", "--build", str(directory), "--target", "godot-cpp", "--parallel"])
        archive = find_archive(directory, GODOT_TARGET, architecture, args.precision)
        actual_architectures = output(["xcrun", "lipo", "-archs", str(archive)]).split()
        if actual_architectures != [architecture]:
            raise SystemExit(f"unexpected architectures in {archive}: {actual_architectures}")
        archives[slice_name] = archive
        generated_include = directory / "gen/include"

    precision_suffix = ".double" if args.precision == "double" else ""
    device_name = f"libgodot-cpp.ios.{GODOT_TARGET}{precision_suffix}.arm64.a"
    shutil.copy2(archives["device-arm64"], stage / "lib/device" / device_name)
    simulator_name = (
        f"libgodot-cpp.ios.{GODOT_TARGET}{precision_suffix}.universal.a"
    )
    simulator_output = stage / "lib/simulator" / simulator_name
    run(
        [
            "xcrun",
            "lipo",
            "-create",
            str(archives["simulator-arm64"]),
            str(archives["simulator-x86_64"]),
            "-output",
            str(simulator_output),
        ]
    )
    if set(output(["xcrun", "lipo", "-archs", str(simulator_output)]).split()) != {
        "arm64",
        "x86_64",
    }:
        raise SystemExit(f"Universal Simulator archive is incomplete: {simulator_output}")

    assert generated_include is not None
    shutil.copytree(godot_cpp / "include", stage / "godot-cpp/include", dirs_exist_ok=True)
    shutil.copytree(generated_include, stage / "godot-cpp/gen/include", dirs_exist_ok=True)
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

    manifest = (
        f"GDPP_SDK {args.schema}\n"
        f"api {args.godot_version}\n"
        f"api_kind {args.api_kind}\n"
        f"api_sha256 {args.api_sha256}\n"
        f"precision {args.precision}\n"
        "platform ios\n"
        "arch arm64\n"
        "profiles debug,release\n"
        "distribution_binding template_release\n"
        "distribution_optimization Release\n"
        "platform_minimum iOS_16.0\n"
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
        f"compiler {xcode_version}\n"
        f"iphoneos_sdk {iphoneos_sdk}\n"
        f"iphonesimulator_sdk {simulator_sdk}\n"
        f"ios_deployment_target {deployment_target}\n"
        "ios_slices device-arm64,simulator-arm64,simulator-x86_64\n"
        "source_paths mapped\n"
    )
    (stage / "sdk.manifest").write_text(manifest, encoding="utf-8", newline="\n")

    target_root.parent.mkdir(parents=True, exist_ok=True)
    if target_root.exists():
        shutil.rmtree(target_root)
    shutil.copytree(stage, target_root)
    size = sum(path.stat().st_size for path in target_root.rglob("*") if path.is_file())
    print(
        f"Packaged Godot {args.godot_version} iOS/arm64 SDK: "
        f"{size / (1024 * 1024):.2f} MiB -> {target_root}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
