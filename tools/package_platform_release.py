#!/usr/bin/env python3
"""Assemble deterministic multi-host GDPP plugin release packages."""

from __future__ import annotations

import argparse
import shutil
import sys
import zipfile
from dataclasses import dataclass
from pathlib import Path

import package_release


WEB_VARIANTS = ("nothreads", "threads")
RUNTIME_FILES = {
    "runtime_header_sha256": "include/gdpp/runtime/variant_ops.hpp",
    "reference_semantics_header_sha256": "include/gdpp/runtime/reference_semantics.hpp",
    "runtime_source_sha256": "src/runtime/variant_ops.cpp",
    "attached_runtime_header_sha256": "include/gdpp/runtime/attached_script.hpp",
    "attached_runtime_registry_source_sha256": "src/runtime/attached_script_registry.cpp",
    "attached_runtime_instance_source_sha256": "src/runtime/attached_script_instance.cpp",
    "attached_runtime_language_source_sha256": "src/runtime/attached_script_language.cpp",
    "integer_semantics_header_sha256": "include/gdpp/numeric/integer_semantics.hpp",
}
RUNTIME_FIELDS = ("runtime_abi", *RUNTIME_FILES)
API_FIELDS = ("api_kind", "api_sha256", "precision")


@dataclass(frozen=True)
class ReleasePackage:
    archive_name: str
    godot_versions: tuple[str, ...]
    desktop_hosts: tuple[str, ...]
    include_android: bool
    include_ios: bool
    web_variants: tuple[str, ...]


FULL_DESKTOP_HOSTS = tuple(package_release.HOSTS)
LITE_DESKTOP_HOSTS = ("mac-universal", "windows-x64")
RELEASE_PACKAGES = {
    "standard": ReleasePackage(
        "gdpp",
        ("4.6", "4.7"),
        FULL_DESKTOP_HOSTS,
        True,
        True,
        WEB_VARIANTS,
    ),
    "all": ReleasePackage(
        "gdpp-all",
        package_release.SUPPORTED_GODOT_VERSIONS,
        FULL_DESKTOP_HOSTS,
        True,
        True,
        WEB_VARIANTS,
    ),
    "lite": ReleasePackage(
        "gdpp-lite",
        ("4.6", "4.7"),
        LITE_DESKTOP_HOSTS,
        True,
        False,
        WEB_VARIANTS,
    ),
}
SHARED_HOST_SDK_PATHS = ("godot-cpp", "include", "src")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--components", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--package", choices=sorted(RELEASE_PACKAGES), required=True)
    return parser.parse_args()


def fail(message: str) -> None:
    raise ValueError(message)


def require_no_symlinks(root: Path) -> None:
    if root.is_symlink():
        fail(f"release component cannot be a symbolic link: {root}")
    if not root.exists():
        fail(f"release component is missing: {root}")
    for path in root.rglob("*"):
        if path.is_symlink():
            fail(f"release component contains a symbolic link: {path}")


def is_generated_project_product(name: str) -> bool:
    lower = name.lower()
    return (
        lower in {"gdpp.lib", "gdpp_project.lib"}
        or lower.startswith(
            (
                "gdpp.debug.",
                "gdpp.release.",
                "libgdpp.debug.",
                "libgdpp.release.",
                "gdpp_project.",
                "libgdpp_project.",
            )
        )
    )


def require_runtime_contract(
    sdk: Path,
    fields: dict[str, str],
    expected: dict[str, str] | None,
) -> dict[str, str]:
    contract = {field: fields.get(field, "") for field in RUNTIME_FIELDS}
    if any(not value for value in contract.values()):
        fail(f"SDK runtime contract is incomplete: {sdk / 'sdk.manifest'}")
    if expected is not None and contract != expected:
        fail(f"SDK runtime contract conflicts with another package component: {sdk}")
    for field, relative in RUNTIME_FILES.items():
        path = sdk / relative
        if not path.is_file():
            fail(f"SDK runtime file is missing: {path}")
        if package_release.sha256(path) != fields[field]:
            fail(f"SDK runtime file does not match {field}: {path}")
    return contract


def require_api_contract(
    sdk: Path,
    fields: dict[str, str],
    expected: dict[str, str] | None,
) -> dict[str, str]:
    contract = {field: fields.get(field, "") for field in API_FIELDS}
    if contract["api_kind"] != "official":
        fail(f"commercial release SDK must use a pinned official Godot API: {sdk}")
    if contract["precision"] != "single":
        fail(f"commercial release SDK must use the single-precision Godot ABI: {sdk}")
    if (
        len(contract["api_sha256"]) != 64
        or any(character not in "0123456789abcdef" for character in contract["api_sha256"])
    ):
        fail(f"SDK Godot API SHA-256 is missing or malformed: {sdk / 'sdk.manifest'}")
    if expected is not None and contract != expected:
        fail(f"SDK Godot API contract conflicts with another package component: {sdk}")
    return contract


def validate_static_addon(addon: Path, component_host: str) -> str:
    if addon.name != "gdpp" or addon.parent.name != "addons":
        fail(f"host component path must end in addons/gdpp: {addon}")
    require_no_symlinks(addon)
    for relative in package_release.STATIC_ADDON_FILES:
        if not (addon / relative).is_file():
            fail(f"host component is missing static addon file: {addon / relative}")
    version = package_release.read_plugin_version(addon / "plugin.cfg")
    if package_release.read_extension_minimum(addon / "gdpp.gdextension") != "4.4":
        fail("compiler GDExtension must retain the Godot 4.4 compatibility baseline")

    host = package_release.HOSTS[component_host]
    expected_binaries = {host.compiler_library, host.fallback_library}
    binary = addon / "binary"
    actual_binaries = {path.name for path in binary.iterdir() if path.is_file()}
    if actual_binaries != expected_binaries:
        fail(
            f"{component_host} component must contain exactly its compiler and fallback "
            f"binaries; expected {sorted(expected_binaries)}, got {sorted(actual_binaries)}"
        )
    expected_executor = addon / host.build_executor
    if not expected_executor.is_file():
        fail(f"{component_host} component is missing its Ninja executor: {expected_executor}")
    actual_executors = {
        path.relative_to(addon).as_posix()
        for path in (addon / "tools").glob("*/gdpp-ninja*")
        if path.is_file()
    }
    if actual_executors != {host.build_executor}:
        fail(
            f"{component_host} component has an invalid Ninja executor matrix; "
            f"expected {[host.build_executor]}, got {sorted(actual_executors)}"
        )
    expected_version = (
        f"Ninja {package_release.NINJA_VERSION}\n"
        f"commit {package_release.NINJA_COMMIT}\n"
    )
    if (addon / "tools/NINJA-VERSION.txt").read_text(
        encoding="utf-8"
    ) != expected_version:
        fail(f"{component_host} component has invalid Ninja version metadata")
    for relative in ("tools/.gdignore", "tools/NINJA-LICENSE.txt"):
        if not (addon / relative).is_file():
            fail(f"{component_host} component is missing {relative}")

    actual_versions = sorted(
        path.name for path in (addon / "sdk").iterdir() if path.is_dir()
    )
    if actual_versions != list(package_release.SUPPORTED_GODOT_VERSIONS):
        fail(
            f"{component_host} component must contain Godot SDKs "
            f"{list(package_release.SUPPORTED_GODOT_VERSIONS)}, got {actual_versions}"
        )
    return version


def validate_host_sdk(
    sdk: Path,
    host: package_release.HostContract,
    godot_version: str,
    gdpp_version: str,
    runtime_contract: dict[str, str] | None,
    api_contract: dict[str, str] | None,
) -> tuple[dict[str, str], dict[str, str]]:
    require_no_symlinks(sdk)
    for relative in package_release.HOST_SDK_PATHS:
        if not (sdk / relative).exists():
            fail(f"host SDK component is missing: {sdk / relative}")
    manifest = sdk / "sdk.manifest"
    schema, fields = package_release.read_sdk_manifest(manifest)
    package_release.require_fields(
        manifest,
        schema,
        fields,
        {
            "api": godot_version,
            "platform": host.platform,
            "arch": host.architecture,
            "profiles": "debug,release",
            "distribution_binding": "template_release",
            "distribution_optimization": "Release",
            "platform_minimum": host.platform_minimum,
            "gdpp_version": gdpp_version,
            "cxx_standard": "17",
            "exceptions": "disabled",
            "msvc_runtime": "static" if host.platform == "windows" else "not_applicable",
        },
    )
    if host.platform == "windows":
        if fields.get("compiler") != "MSVC" or not fields.get("compiler_version", "").startswith(
            "19."
        ):
            fail(f"Windows SDK must use an MSVC 19.x toolset: {manifest}")
    package_release.require_profile_libraries(sdk / "lib", ("template_release",))
    return (
        require_runtime_contract(sdk, fields, runtime_contract),
        require_api_contract(sdk, fields, api_contract),
    )


def validate_android_sdk(
    sdk: Path,
    godot_version: str,
    gdpp_version: str,
    runtime_contract: dict[str, str],
    api_contract: dict[str, str],
) -> None:
    require_no_symlinks(sdk)
    manifest = sdk / "sdk.manifest"
    schema, fields = package_release.read_sdk_manifest(manifest)
    package_release.require_fields(
        manifest,
        schema,
        fields,
        {
            "api": godot_version,
            "platform": "android",
            "arch": "arm64",
            "profiles": "debug,release",
            "distribution_binding": "template_release",
            "distribution_optimization": "Release",
            "platform_minimum": "Android_9",
            "android_api_level": "28",
            "android_stl": "c++_shared",
            "gdpp_version": gdpp_version,
            "cxx_standard": "17",
            "exceptions": "disabled",
            "msvc_runtime": "not_applicable",
        },
    )
    package_release.require_profile_libraries(sdk / "lib", ("template_release",))
    require_runtime_contract(sdk, fields, runtime_contract)
    require_api_contract(sdk, fields, api_contract)


def validate_ios_sdk(
    sdk: Path,
    godot_version: str,
    gdpp_version: str,
    runtime_contract: dict[str, str],
    api_contract: dict[str, str],
) -> None:
    require_no_symlinks(sdk)
    manifest = sdk / "sdk.manifest"
    schema, fields = package_release.read_sdk_manifest(manifest)
    package_release.require_fields(
        manifest,
        schema,
        fields,
        {
            "api": godot_version,
            "platform": "ios",
            "arch": "arm64",
            "profiles": "debug,release",
            "distribution_binding": "template_release",
            "distribution_optimization": "Release",
            "platform_minimum": "iOS_16.0",
            "ios_deployment_target": "16.0",
            "ios_slices": "device-arm64,simulator-arm64,simulator-x86_64",
            "source_paths": "mapped",
            "gdpp_version": gdpp_version,
            "cxx_standard": "17",
            "exceptions": "disabled",
            "msvc_runtime": "not_applicable",
        },
    )
    package_release.require_profile_libraries(sdk / "lib/device", ("template_release",))
    package_release.require_profile_libraries(sdk / "lib/simulator", ("template_release",))
    require_runtime_contract(sdk, fields, runtime_contract)
    require_api_contract(sdk, fields, api_contract)


def validate_web_sdk(
    sdk: Path,
    godot_version: str,
    variant: str,
    gdpp_version: str,
    runtime_contract: dict[str, str],
    api_contract: dict[str, str],
) -> None:
    require_no_symlinks(sdk)
    manifest = sdk / "sdk.manifest"
    schema, fields = package_release.read_sdk_manifest(manifest)
    package_release.require_fields(
        manifest,
        schema,
        fields,
        {
            "api": godot_version,
            "platform": "web",
            "arch": "wasm32",
            "profiles": "debug,release",
            "distribution_binding": "template_release",
            "distribution_optimization": "Release",
            "platform_minimum": "none",
            "web_threads": variant,
            "source_paths": "mapped",
            "compiler": "Emscripten",
            "gdpp_version": gdpp_version,
            "cxx_standard": "17",
            "exceptions": "disabled",
            "msvc_runtime": "not_applicable",
        },
    )
    package_release.require_profile_libraries(sdk / "lib", ("template_release",))
    archives = [path.name for path in (sdk / "lib").glob("*.a")]
    if variant == "nothreads" and not any(".nothreads." in name for name in archives):
        fail(f"single-threaded Web SDK archive is not marked nothreads: {sdk}")
    if variant == "threads" and any(".nothreads." in name for name in archives):
        fail(f"multi-threaded Web SDK contains a nothreads archive: {sdk}")
    require_runtime_contract(sdk, fields, runtime_contract)
    require_api_contract(sdk, fields, api_contract)


def host_component(components: Path, component_host: str) -> Path:
    return components / f"gdpp-host-{component_host}" / "addons/gdpp"


def android_component(components: Path, godot_version: str) -> Path:
    return components / f"gdpp-android-arm64-godot-{godot_version}"


def ios_component(components: Path, godot_version: str) -> Path:
    return components / f"gdpp-ios-godot-{godot_version}"


def web_component(components: Path, godot_version: str, variant: str) -> Path:
    return components / f"gdpp-web-godot-{godot_version}-{variant}"


def copy_unique_library(source: Path, destination: Path) -> None:
    target = destination / source.name
    if target.exists():
        fail(f"target SDK libraries collide in the shared SDK layout: {source} and {target}")
    package_release.copy_path(source, target)


def copy_component_libraries(source: Path, destination: Path) -> None:
    libraries = sorted(path for path in source.rglob("*") if path.is_file())
    if not libraries:
        fail(f"target SDK component has no libraries: {source}")
    for library in libraries:
        if library.suffix not in {".a", ".lib"}:
            fail(f"target SDK library directory contains an unexpected file: {library}")
        copy_unique_library(library, destination)


def copy_target_manifest(source_sdk: Path, staged_sdk: Path, target: str) -> None:
    package_release.copy_path(
        source_sdk / "sdk.manifest",
        staged_sdk / "manifests" / f"{target}.sdk.manifest",
    )


def tree_contract(root: Path) -> dict[str, str]:
    return {
        path.relative_to(root).as_posix(): package_release.sha256(path)
        for path in package_release.iter_files(root)
    }


def require_identical_tree(reference: Path, candidate: Path, label: str) -> None:
    if tree_contract(reference) != tree_contract(candidate):
        fail(f"{label} differs between desktop host components: {reference} and {candidate}")


def release_package_manifest(package_name: str, gdpp_version: str) -> str:
    package = RELEASE_PACKAGES[package_name]
    editor_hosts = ",".join(
        f"{package_release.HOSTS[name].platform}-"
        f"{package_release.HOSTS[name].architecture}"
        for name in package.desktop_hosts
    )
    platform_minimums = ",".join(
        f"{package_release.HOSTS[name].platform}="
        f"{package_release.HOSTS[name].platform_minimum}"
        for name in package.desktop_hosts
    )
    export_targets = [
        package_release.HOSTS[name].export_targets[0]
        for name in package.desktop_hosts
    ]
    if package.include_android:
        export_targets.append("android-arm64")
    if package.include_ios:
        export_targets.append("ios-arm64")
    export_targets.extend(f"web-wasm32-{variant}" for variant in package.web_variants)
    target_versions = ",".join(package.godot_versions)
    optional_minimums = ""
    if package.include_android:
        optional_minimums += "android_platform_minimum Android_9_API_28\n"
    if package.include_ios:
        optional_minimums += "ios_platform_minimum iOS_16.0\n"
    return (
        "GDPP_PACKAGE 7\n"
        "kind multi-host\n"
        f"edition {package_name}\n"
        "archive_layout addons/gdpp\n"
        "sdk_layout shared-target-manifests\n"
        f"version {gdpp_version}\n"
        "compiler_godot_api 4.4\n"
        f"target_godot_apis {target_versions}\n"
        "godot_precision single\n"
        "api_fingerprints sha256-verified\n"
        f"build_executor Ninja_{package_release.NINJA_VERSION}\n"
        f"build_executor_hosts {','.join(package.desktop_hosts)}\n"
        f"editor_hosts {editor_hosts}\n"
        f"host_platform_minimums {platform_minimums}\n"
        f"export_targets {','.join(export_targets)}\n"
        f"{optional_minimums}"
    )


def stage_release_package(
    components: Path,
    output: Path,
    package_name: str,
) -> tuple[Path, str, str]:
    package = RELEASE_PACKAGES[package_name]
    addons: dict[str, Path] = {}
    gdpp_version = ""
    static_contract: dict[str, str] | None = None
    for component_host in package.desktop_hosts:
        addon = host_component(components, component_host)
        component_version = validate_static_addon(addon, component_host)
        if gdpp_version and component_version != gdpp_version:
            fail(
                "desktop host component plugin versions conflict: "
                f"{gdpp_version} and {component_version}"
            )
        gdpp_version = component_version
        component_static_contract = {
            relative: package_release.sha256(addon / relative)
            for relative in package_release.STATIC_ADDON_FILES
        }
        if static_contract is not None and component_static_contract != static_contract:
            fail(f"{component_host} static add-on files conflict with another host component")
        static_contract = component_static_contract
        addons[component_host] = addon

    canonical_host = package.desktop_hosts[0]
    canonical_addon = addons[canonical_host]
    runtime_contract: dict[str, str] | None = None

    for godot_version in package.godot_versions:
        canonical_sdk = canonical_addon / "sdk" / godot_version
        api_contract: dict[str, str] | None = None
        for component_host in package.desktop_hosts:
            host = package_release.HOSTS[component_host]
            host_sdk = addons[component_host] / "sdk" / godot_version
            runtime_contract, api_contract = validate_host_sdk(
                host_sdk,
                host,
                godot_version,
                gdpp_version,
                runtime_contract,
                api_contract,
            )
            if component_host != canonical_host:
                for relative in SHARED_HOST_SDK_PATHS:
                    require_identical_tree(
                        canonical_sdk / relative,
                        host_sdk / relative,
                        f"Godot {godot_version} {relative}",
                    )
        if api_contract is None:
            fail(f"Godot {godot_version} package has no desktop API contract")
        if package.include_android:
            validate_android_sdk(
                android_component(components, godot_version),
                godot_version,
                gdpp_version,
                runtime_contract,
                api_contract,
            )
        for variant in package.web_variants:
            validate_web_sdk(
                web_component(components, godot_version, variant),
                godot_version,
                variant,
                gdpp_version,
                runtime_contract,
                api_contract,
            )
        if package.include_ios:
            validate_ios_sdk(
                ios_component(components, godot_version),
                godot_version,
                gdpp_version,
                runtime_contract,
                api_contract,
            )

    if runtime_contract is None:
        fail("release package has no SDK runtime contract")

    stage_root = output / ".staging" / package.archive_name
    if stage_root.exists():
        shutil.rmtree(stage_root)
    staged_addon = stage_root / "addons" / "gdpp"
    staged_addon.mkdir(parents=True)

    for relative in package_release.STATIC_ADDON_FILES:
        package_release.copy_path(canonical_addon / relative, staged_addon / relative)
    for relative in (
        "tools/.gdignore",
        "tools/NINJA-LICENSE.txt",
        "tools/NINJA-VERSION.txt",
    ):
        package_release.copy_path(canonical_addon / relative, staged_addon / relative)
    for component_host in package.desktop_hosts:
        host = package_release.HOSTS[component_host]
        for filename in (host.compiler_library, host.fallback_library):
            package_release.copy_path(
                addons[component_host] / "binary" / filename,
                staged_addon / "binary" / filename,
            )
        package_release.copy_path(
            addons[component_host] / host.build_executor,
            staged_addon / host.build_executor,
        )
        (staged_addon / host.build_executor).chmod(0o755)

    for godot_version in package.godot_versions:
        source_sdk = canonical_addon / "sdk" / godot_version
        staged_sdk = staged_addon / "sdk" / godot_version
        for relative in SHARED_HOST_SDK_PATHS:
            package_release.copy_path(source_sdk / relative, staged_sdk / relative)
        for component_host in package.desktop_hosts:
            host = package_release.HOSTS[component_host]
            host_sdk = addons[component_host] / "sdk" / godot_version
            copy_component_libraries(host_sdk / "lib", staged_sdk / "lib")
            copy_target_manifest(
                host_sdk,
                staged_sdk,
                f"{host.platform}.{host.architecture}",
            )
        if package.include_android:
            android_sdk = android_component(components, godot_version)
            copy_component_libraries(android_sdk / "lib", staged_sdk / "lib")
            copy_target_manifest(android_sdk, staged_sdk, "android.arm64")
        for variant in package.web_variants:
            web_sdk = web_component(components, godot_version, variant)
            copy_component_libraries(web_sdk / "lib", staged_sdk / "lib")
            copy_target_manifest(
                web_sdk,
                staged_sdk,
                f"web.wasm32.{variant}",
            )
        if package.include_ios:
            ios_sdk = ios_component(components, godot_version)
            copy_component_libraries(ios_sdk / "lib", staged_sdk / "lib")
            copy_target_manifest(ios_sdk, staged_sdk, "ios.arm64")

    (staged_addon / "sdk/.gdignore").write_text("", encoding="utf-8")
    (staged_addon / "PACKAGE_MANIFEST.txt").write_text(
        release_package_manifest(package_name, gdpp_version),
        encoding="utf-8",
    )
    validate_release_stage(staged_addon, package_name, gdpp_version)
    return stage_root, package.archive_name, gdpp_version


def binding_matches(
    filename: str,
    platform: str,
    architecture: str,
    web_variant: str | None = None,
) -> bool:
    if f".{platform}.template_release.{architecture}." not in filename:
        return False
    if platform != "web":
        return True
    has_nothreads = ".nothreads." in filename
    return has_nothreads if web_variant == "nothreads" else not has_nothreads


def require_one_binding(
    names: set[str],
    platform: str,
    architecture: str,
    web_variant: str | None = None,
) -> None:
    matches = [
        name
        for name in names
        if binding_matches(name, platform, architecture, web_variant)
    ]
    if len(matches) != 1:
        variant = f"/{web_variant}" if web_variant else ""
        fail(
            f"shared SDK must contain exactly one {platform}/{architecture}{variant} "
            f"Release binding, found {matches}"
        )


def validate_shared_target_manifest(
    manifest: Path,
    expected: dict[str, str],
    runtime_contract: dict[str, str],
    api_contract: dict[str, str],
) -> None:
    schema, fields = package_release.read_sdk_manifest(manifest)
    package_release.require_fields(
        manifest,
        schema,
        fields,
        {
            "profiles": "debug,release",
            "distribution_binding": "template_release",
            "distribution_optimization": "Release",
            **expected,
        },
    )
    contract = {field: fields.get(field, "") for field in RUNTIME_FIELDS}
    if contract != runtime_contract:
        fail(f"shared target manifest runtime contract conflicts with the host SDK: {manifest}")
    target_api_contract = {field: fields.get(field, "") for field in API_FIELDS}
    if target_api_contract != api_contract:
        fail(f"shared target manifest Godot API contract conflicts with the host SDK: {manifest}")


def validate_release_stage(addon: Path, package_name: str, gdpp_version: str) -> None:
    package = RELEASE_PACKAGES[package_name]
    require_no_symlinks(addon)
    if package_release.read_plugin_version(addon / "plugin.cfg") != gdpp_version:
        fail("release package metadata version changed during staging")

    expected_binaries = {
        filename
        for host in (
            package_release.HOSTS[name] for name in package.desktop_hosts
        )
        for filename in (host.compiler_library, host.fallback_library)
    }
    actual_binaries = {path.name for path in (addon / "binary").iterdir() if path.is_file()}
    if actual_binaries != expected_binaries:
        fail(
            f"{package_name} package contains an invalid desktop compiler/fallback matrix; "
            f"expected {sorted(expected_binaries)}, got {sorted(actual_binaries)}"
        )
    expected_executors = {
        package_release.HOSTS[name].build_executor for name in package.desktop_hosts
    }
    actual_executors = {
        path.relative_to(addon).as_posix()
        for path in (addon / "tools").glob("*/gdpp-ninja*")
        if path.is_file()
    }
    if actual_executors != expected_executors:
        fail(
            f"{package_name} package contains an invalid Ninja executor matrix; "
            f"expected {sorted(expected_executors)}, got {sorted(actual_executors)}"
        )
    expected_ninja_version = (
        f"Ninja {package_release.NINJA_VERSION}\n"
        f"commit {package_release.NINJA_COMMIT}\n"
    )
    if (addon / "tools/NINJA-VERSION.txt").read_text(
        encoding="utf-8"
    ) != expected_ninja_version:
        fail(f"{package_name} package has invalid Ninja version metadata")
    for relative in ("tools/.gdignore", "tools/NINJA-LICENSE.txt"):
        if not (addon / relative).is_file():
            fail(f"{package_name} package is missing {relative}")
    manifest = addon / "PACKAGE_MANIFEST.txt"
    if manifest.read_text(encoding="utf-8") != release_package_manifest(
        package_name, gdpp_version
    ):
        fail(f"{package_name} package manifest does not match its release specification")

    actual_versions = sorted(
        path.name for path in (addon / "sdk").iterdir() if path.is_dir()
    )
    if actual_versions != list(package.godot_versions):
        fail(
            f"{package_name} package must contain exactly Godot SDKs "
            f"{list(package.godot_versions)}, got {actual_versions}"
        )
    for godot_version in package.godot_versions:
        version_root = addon / "sdk" / godot_version
        for relative in SHARED_HOST_SDK_PATHS:
            if not (version_root / relative).exists():
                fail(
                    f"{package_name} package shared SDK input is missing: "
                    f"{version_root / relative}"
                )
        if (version_root / "sdk.manifest").exists():
            fail(
                f"{package_name} package contains an ambiguous single-host manifest: "
                f"{version_root / 'sdk.manifest'}"
            )
        manifests = version_root / "manifests"
        expected_manifest_names = {
            *(f"{host.platform}.{host.architecture}.sdk.manifest"
              for host in (
                  package_release.HOSTS[name] for name in package.desktop_hosts
              )),
            *(("android.arm64.sdk.manifest",) if package.include_android else ()),
            *(("ios.arm64.sdk.manifest",) if package.include_ios else ()),
            *(f"web.wasm32.{variant}.sdk.manifest"
              for variant in package.web_variants),
        }
        actual_manifest_names = {
            path.name for path in manifests.iterdir() if path.is_file()
        }
        if actual_manifest_names != expected_manifest_names:
            fail(
                f"{package_name} Godot {godot_version} target manifests differ; "
                f"expected {sorted(expected_manifest_names)}, got {sorted(actual_manifest_names)}"
            )

        canonical_host = package_release.HOSTS[package.desktop_hosts[0]]
        canonical_manifest = (
            manifests
            / f"{canonical_host.platform}.{canonical_host.architecture}.sdk.manifest"
        )
        _, host_fields = package_release.read_sdk_manifest(canonical_manifest)
        runtime_contract = require_runtime_contract(
            version_root,
            host_fields,
            None,
        )
        api_contract = require_api_contract(version_root, host_fields, None)
        for host in (
            package_release.HOSTS[name] for name in package.desktop_hosts
        ):
            validate_shared_target_manifest(
                manifests / f"{host.platform}.{host.architecture}.sdk.manifest",
                {
                    "api": godot_version,
                    "platform": host.platform,
                    "arch": host.architecture,
                    "gdpp_version": gdpp_version,
                },
                runtime_contract,
                api_contract,
            )
        if package.include_android:
            validate_shared_target_manifest(
                manifests / "android.arm64.sdk.manifest",
                {
                    "api": godot_version,
                    "platform": "android",
                    "arch": "arm64",
                    "gdpp_version": gdpp_version,
                },
                runtime_contract,
                api_contract,
            )
        for variant in package.web_variants:
            validate_shared_target_manifest(
                manifests / f"web.wasm32.{variant}.sdk.manifest",
                {
                    "api": godot_version,
                    "platform": "web",
                    "arch": "wasm32",
                    "web_threads": variant,
                    "gdpp_version": gdpp_version,
                },
                runtime_contract,
                api_contract,
            )
        if package.include_ios:
            validate_shared_target_manifest(
                manifests / "ios.arm64.sdk.manifest",
                {
                    "api": godot_version,
                    "platform": "ios",
                    "arch": "arm64",
                    "gdpp_version": gdpp_version,
                },
                runtime_contract,
                api_contract,
            )

        libraries = {
            path.name for path in (version_root / "lib").iterdir() if path.is_file()
        }
        expected_count = (
            len(package.desktop_hosts)
            + int(package.include_android)
            + len(package.web_variants)
            + (2 if package.include_ios else 0)
        )
        if len(libraries) != expected_count:
            fail(
                f"{package_name} shared Godot {godot_version} SDK must contain "
                f"{expected_count} target libraries, found {sorted(libraries)}"
            )
        for host in (
            package_release.HOSTS[name] for name in package.desktop_hosts
        ):
            require_one_binding(libraries, host.platform, host.architecture)
        if package.include_android:
            require_one_binding(libraries, "android", "arm64")
        for variant in package.web_variants:
            require_one_binding(libraries, "web", "wasm32", variant)
        if package.include_ios:
            require_one_binding(libraries, "ios", "arm64")
            require_one_binding(libraries, "ios", "universal")

        for retired_directory in ("android", "web", "ios", "macos", "linux", "windows"):
            if (version_root / retired_directory).exists():
                fail(
                    f"{package_name} shared SDK contains retired platform directory: "
                    f"{version_root / retired_directory}"
                )

    forbidden = [
        path
        for path in addon.rglob("*")
        if "template_debug" in path.name
        or ".editor." in path.name
        or path.suffix == ".zip"
        or path.name == ".DS_Store"
        or path.name.startswith("._")
        or is_generated_project_product(path.name)
        or path.name == "build"
    ]
    if forbidden:
        fail(f"release package contains forbidden products: {forbidden[:5]}")


def main() -> int:
    args = parse_args()
    components = args.components.resolve()
    output = args.output.resolve()
    source_root = Path(__file__).resolve().parent.parent
    try:
        package_release.require_descendant(components, source_root / "build", "component root")
        package_release.require_descendant(output, source_root / "build", "release output")
        stage_root, archive_name, _ = stage_release_package(
            components, output, args.package
        )
        archive = output / f"{archive_name}.zip"
        package_release.create_zip(stage_root, archive)
        shutil.rmtree(stage_root)
        print(f"{archive}  sha256={package_release.sha256(archive)}")
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        print(f"release packaging failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
