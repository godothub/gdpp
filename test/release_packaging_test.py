#!/usr/bin/env python3
"""Fixture tests for the three cross-version desktop release packages."""

from __future__ import annotations

import hashlib
import json
import shutil
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path


if len(sys.argv) < 3:
    raise SystemExit("usage: release_packaging_test.py SOURCE_ROOT BINARY_ROOT")
SOURCE_ROOT = Path(sys.argv.pop(1)).resolve()
BINARY_ROOT = Path(sys.argv.pop(1)).resolve()
sys.dont_write_bytecode = True
sys.path.insert(0, str(SOURCE_ROOT / "tools"))

import extract_changelog  # noqa: E402
import package_platform_release  # noqa: E402
import package_release  # noqa: E402
import stage_host_component  # noqa: E402


RUNTIME_CONTENT = {
    "runtime_header_sha256": ("include/gdpp/runtime/variant_ops.hpp", "runtime-header"),
    "reference_semantics_header_sha256": (
        "include/gdpp/runtime/reference_semantics.hpp",
        "reference-semantics-header",
    ),
    "runtime_source_sha256": ("src/runtime/variant_ops.cpp", "runtime-source"),
    "attached_runtime_header_sha256": (
        "include/gdpp/runtime/attached_script.hpp",
        "attached-header",
    ),
    "attached_runtime_registry_source_sha256": (
        "src/runtime/attached_script_registry.cpp",
        "attached-registry",
    ),
    "attached_runtime_instance_source_sha256": (
        "src/runtime/attached_script_instance.cpp",
        "attached-instance",
    ),
    "attached_runtime_language_source_sha256": (
        "src/runtime/attached_script_language.cpp",
        "attached-language",
    ),
    "integer_semantics_header_sha256": (
        "include/gdpp/numeric/integer_semantics.hpp",
        "integer-semantics",
    ),
}
RUNTIME_FIELDS = {
    "runtime_abi": "12",
    **{
        field: hashlib.sha256(content.encode("utf-8")).hexdigest()
        for field, (_, content) in RUNTIME_CONTENT.items()
    },
}
COMMON_FIELDS = {
    "api_kind": "official",
    "api_sha256": "a" * 64,
    "precision": "single",
    "distribution_binding": "template_release",
    "distribution_optimization": "Release",
    "gdpp_version": "1.7.10",
    "cxx_standard": "17",
    "exceptions": "disabled",
    **RUNTIME_FIELDS,
}


def write(path: Path, content: str = "fixture") -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def write_manifest(path: Path, fields: dict[str, str]) -> None:
    write(
        path,
        f"GDPP_SDK {package_release.SDK_SCHEMA}\n"
        + "".join(f"{key} {value}\n" for key, value in fields.items()),
    )


def write_runtime(sdk: Path) -> None:
    for relative, content in RUNTIME_CONTENT.values():
        write(sdk / relative, content)
    write(sdk / "godot-cpp/include/godot_cpp/classes/object.hpp")
    write(sdk / "godot-cpp/gen/include/godot_cpp/classes/node.hpp")
    write(sdk / "godot-cpp/LICENSE.md")


def create_host_component(root: Path, component_host: str) -> Path:
    host = package_release.HOSTS[component_host]
    addon = root / f"gdpp-host-{component_host}" / "addons/gdpp"
    for relative in package_release.STATIC_ADDON_FILES:
        write(addon / relative)
    write(addon / "plugin.cfg", '[plugin]\nversion="1.7.10"\n')
    write(addon / "gdpp.gdextension", '[configuration]\ncompatibility_minimum = "4.4"\n')
    write(addon / "binary" / host.compiler_library, f"compiler-{component_host}")
    write(addon / "binary" / host.fallback_library, f"fallback-{component_host}")

    for godot_version in package_release.SUPPORTED_GODOT_VERSIONS:
        sdk = addon / "sdk" / godot_version
        write_runtime(sdk)
        extension = ".lib" if host.platform == "windows" else ".a"
        write(
            sdk
            / "lib"
            / (
                f"libgodot-cpp.{host.platform}.template_release."
                f"{host.architecture}{extension}"
            )
        )
        write_manifest(
            sdk / "sdk.manifest",
            {
                "api": godot_version,
                "platform": host.platform,
                "arch": host.architecture,
                "profiles": "debug,release",
                "platform_minimum": host.platform_minimum,
                "msvc_runtime": "static" if host.platform == "windows" else "not_applicable",
                **(
                    {"compiler": "MSVC", "compiler_version": "19.44.35207.1"}
                    if host.platform == "windows"
                    else {"compiler": "Clang", "compiler_version": "18.1.0"}
                ),
                **COMMON_FIELDS,
            },
        )
    return addon


def create_android_sdk(root: Path, godot_version: str) -> Path:
    sdk = root / f"gdpp-android-arm64-godot-{godot_version}"
    write_runtime(sdk)
    write(sdk / "lib/libgodot-cpp.android.template_release.arm64.a")
    write_manifest(
        sdk / "sdk.manifest",
        {
            "api": godot_version,
            "platform": "android",
            "arch": "arm64",
            "profiles": "debug,release",
            "platform_minimum": "Android_9",
            "android_api_level": "28",
            "android_stl": "c++_shared",
            "msvc_runtime": "not_applicable",
            **COMMON_FIELDS,
        },
    )
    return sdk


def create_ios_sdk(root: Path, godot_version: str) -> Path:
    sdk = root / f"gdpp-ios-godot-{godot_version}"
    write_runtime(sdk)
    write(sdk / "lib/device/libgodot-cpp.ios.template_release.arm64.a")
    write(sdk / "lib/simulator/libgodot-cpp.ios.template_release.universal.a")
    write_manifest(
        sdk / "sdk.manifest",
        {
            "api": godot_version,
            "platform": "ios",
            "arch": "arm64",
            "profiles": "debug,release",
            "platform_minimum": "iOS_16.0",
            "ios_deployment_target": "16.0",
            "ios_slices": "device-arm64,simulator-arm64,simulator-x86_64",
            "source_paths": "mapped",
            "msvc_runtime": "not_applicable",
            **COMMON_FIELDS,
        },
    )
    return sdk


def create_web_sdk(root: Path, godot_version: str, variant: str) -> Path:
    sdk = root / f"gdpp-web-godot-{godot_version}-{variant}"
    write_runtime(sdk)
    suffix = ".nothreads" if variant == "nothreads" else ""
    write(sdk / "lib" / f"libgodot-cpp.web.template_release.wasm32{suffix}.a")
    write_manifest(
        sdk / "sdk.manifest",
        {
            "api": godot_version,
            "platform": "web",
            "arch": "wasm32",
            "profiles": "debug,release",
            "platform_minimum": "none",
            "web_threads": variant,
            "source_paths": "mapped",
            "compiler": "Emscripten",
            "compiler_version": "4.0.0",
            "msvc_runtime": "not_applicable",
            **COMMON_FIELDS,
        },
    )
    return sdk


def create_components(root: Path) -> None:
    for component_host in package_release.HOSTS:
        create_host_component(root, component_host)
    for godot_version in package_release.SUPPORTED_GODOT_VERSIONS:
        create_android_sdk(root, godot_version)
        create_ios_sdk(root, godot_version)
        for variant in package_platform_release.WEB_VARIANTS:
            create_web_sdk(root, godot_version, variant)


class ReleasePackagingTest(unittest.TestCase):
    def setUp(self) -> None:
        BINARY_ROOT.mkdir(parents=True, exist_ok=True)
        self.temporary = Path(tempfile.mkdtemp(prefix="gdpp-platform-release-"))
        self.components = self.temporary / "components"
        create_components(self.components)

    def tearDown(self) -> None:
        shutil.rmtree(self.temporary)

    def stage(self, host: str) -> tuple[Path, str]:
        stage, archive_name, version = package_platform_release.stage_platform_package(
            self.components,
            self.temporary / f"release-{host}",
            host,
        )
        self.assertEqual(version, "1.7.10")
        return stage, archive_name

    def test_three_packages_contain_all_versions_and_only_supported_targets(self) -> None:
        expected_names = {
            "mac": "gdpp-mac",
            "linux": "gdpp-linux",
            "win": "gdpp-win",
        }
        for package_name, expected_archive in expected_names.items():
            with self.subTest(package=package_name):
                stage, archive_name = self.stage(package_name)
                self.assertEqual(archive_name, expected_archive)
                addon = stage / "addons" / "gdpp"
                package = package_platform_release.PLATFORM_PACKAGES[package_name]
                host = package_release.HOSTS[package.component_host]
                self.assertEqual(
                    {path.name for path in (addon / "binary").iterdir()},
                    {host.compiler_library, host.fallback_library},
                )
                self.assertEqual(
                    sorted(path.name for path in (addon / "sdk").iterdir() if path.is_dir()),
                    list(package_release.SUPPORTED_GODOT_VERSIONS),
                )
                for godot_version in package_release.SUPPORTED_GODOT_VERSIONS:
                    sdk = addon / "sdk" / godot_version
                    self.assertTrue((sdk / "sdk.manifest").is_file())
                    manifests = sdk / "manifests"
                    self.assertTrue(
                        (manifests / "android.arm64.sdk.manifest").is_file()
                    )
                    self.assertTrue(
                        (manifests / "web.wasm32.nothreads.sdk.manifest").is_file()
                    )
                    self.assertTrue(
                        (manifests / "web.wasm32.threads.sdk.manifest").is_file()
                    )
                    self.assertEqual(
                        (manifests / "ios.arm64.sdk.manifest").is_file(),
                        package_name == "mac",
                    )
                    self.assertEqual(
                        len(list((sdk / "lib").iterdir())),
                        6 if package_name == "mac" else 4,
                    )
                    for retired in ("android", "ios", "web", "macos", "linux", "windows"):
                        self.assertFalse((sdk / retired).exists())
                manifest = (addon / "PACKAGE_MANIFEST.txt").read_text(encoding="utf-8")
                self.assertTrue(manifest.startswith("GDPP_PACKAGE 5\n"))
                self.assertIn("archive_layout addons/gdpp", manifest)
                self.assertIn("target_godot_apis 4.4,4.5,4.6,4.7", manifest)
                self.assertIn(f"host {package_name}", manifest)
                self.assertIn("sdk_layout shared-target-manifests", manifest)

    def test_zip_is_reproducible_and_contains_no_nested_or_debug_products(self) -> None:
        stage, archive_name = self.stage("mac")
        first_archive = self.temporary / "first.zip"
        package_release.create_zip(stage, first_archive)
        first_hash = package_release.sha256(first_archive)
        with zipfile.ZipFile(first_archive) as packaged:
            names = packaged.namelist()
        self.assertTrue(all(path.startswith("addons/gdpp/") for path in names))
        self.assertFalse(any(path.endswith(".zip") for path in names))
        self.assertFalse(any("template_debug" in path for path in names))
        self.assertFalse(any(".editor." in path for path in names))
        self.assertFalse(
            any(
                package_platform_release.is_generated_project_product(
                    Path(path.rstrip("/")).name
                )
                for path in names
            )
        )
        self.assertIn("addons/gdpp/build_progress.gd", names)
        self.assertIn("addons/gdpp/native_build_job.gd", names)
        self.assertFalse(any(path.startswith("gdpp/") for path in names))

        second_stage, second_name, _ = package_platform_release.stage_platform_package(
            self.components,
            self.temporary / "second-release",
            "mac",
        )
        self.assertEqual(second_name, archive_name)
        second_archive = self.temporary / "second.zip"
        package_release.create_zip(second_stage, second_archive)
        self.assertEqual(first_hash, package_release.sha256(second_archive))

    def test_generated_project_product_names_are_rejected_without_blocking_plugin_binaries(
        self,
    ) -> None:
        for name in (
            "gdpp.release.windows.x86_64.dll",
            "libgdpp.debug.linux.x86_64.so",
            "libgdpp.release.web.wasm32.nothreads.wasm",
            "libgdpp.release.ios.arm64.xcframework",
            "gdpp.lib",
            "gdpp_project.release.windows.x86_64.dll",
            "libgdpp_project.release.macos.arm64.dylib",
            "gdpp_project.lib",
        ):
            self.assertTrue(package_platform_release.is_generated_project_product(name))
        for name in (
            "gdpp.gdextension",
            "gdpp_compiler.windows.x86_64.dll",
            "libgdpp_compiler.macos.universal.dylib",
            "libgdpp_fallback.linux.x86_64.so",
        ):
            self.assertFalse(package_platform_release.is_generated_project_product(name))

    def test_missing_godot_version_fails_closed(self) -> None:
        shutil.rmtree(
            self.components / "gdpp-host-linux-x64/addons/gdpp/sdk/4.7"
        )
        with self.assertRaisesRegex(ValueError, "must contain Godot SDKs"):
            package_platform_release.stage_platform_package(
                self.components,
                self.temporary / "release",
                "linux",
            )

    def test_missing_required_target_fails_closed(self) -> None:
        shutil.rmtree(self.components / "gdpp-web-godot-4.6-threads")
        with self.assertRaisesRegex(ValueError, "component is missing"):
            package_platform_release.stage_platform_package(
                self.components,
                self.temporary / "release",
                "win",
            )

    def test_runtime_contract_conflict_fails_closed_across_versions(self) -> None:
        manifest = self.components / "gdpp-android-arm64-godot-4.7/sdk.manifest"
        manifest.write_text(
            manifest.read_text(encoding="utf-8").replace(
                "runtime_abi 12", "runtime_abi 13"
            ),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(ValueError, "runtime contract conflicts"):
            package_platform_release.stage_platform_package(
                self.components,
                self.temporary / "release",
                "mac",
            )

    def test_editor_or_debug_binding_fails_closed(self) -> None:
        sdk = self.components / "gdpp-host-windows-x64/addons/gdpp/sdk/4.6/lib"
        write(sdk / "libgodot-cpp.windows.editor.x86_64.lib")
        with self.assertRaisesRegex(ValueError, "unexpected bindings"):
            package_platform_release.stage_platform_package(
                self.components,
                self.temporary / "release",
                "win",
            )
        (sdk / "libgodot-cpp.windows.editor.x86_64.lib").unlink()
        write(sdk / "libgodot-cpp.windows.template_debug.x86_64.lib")
        with self.assertRaisesRegex(ValueError, "unexpected bindings"):
            package_platform_release.stage_platform_package(
                self.components,
                self.temporary / "release-debug",
                "win",
            )

    def test_release_workflow_declares_only_the_three_platform_archives(self) -> None:
        workflow_root = SOURCE_ROOT / ".github/workflows"
        orchestrator = (workflow_root / "release.yml").read_text(encoding="utf-8")
        host_components = (workflow_root / "host-components.yml").read_text(
            encoding="utf-8"
        )
        packages = (workflow_root / "package-release.yml").read_text(encoding="utf-8")
        for archive in ("gdpp-mac.zip", "gdpp-linux.zip", "gdpp-win.zip"):
            self.assertIn(archive, packages)
        self.assertIn("python3 tools/stage_host_component.py", host_components)
        self.assertIn("--host '${{ matrix.host }}'", host_components)
        self.assertIn(
            "uses: ./.github/workflows/host-components.yml",
            orchestrator,
        )
        self.assertIn(
            "uses: ./.github/workflows/package-release.yml",
            orchestrator,
        )
        self.assertIn(
            "uses: ./.github/workflows/release-package-smoke.yml",
            orchestrator,
        )
        installed_smoke = (
            workflow_root / "release-package-smoke.yml"
        ).read_text(encoding="utf-8")
        self.assertIn("name: gdpp-release-${{ matrix.package_host }}", installed_smoke)
        self.assertIn("Install the final ZIP into a clean customer project", installed_smoke)
        self.assertIn("PCK_AUDIT_VIOLATIONS=0", installed_smoke)
        self.assertIn("PCK_AUDIT_PROJECT_LIBRARIES=1", installed_smoke)
        for macos_gate in (host_components, installed_smoke):
            self.assertIn("--quit-after 120", macos_gate)
            self.assertIn('"$root/warmup.log"', macos_gate)
            self.assertIn(
                'load_commands="$(otool -arch "$architecture" -l',
                macos_gate,
            )
            self.assertNotIn("| grep -A3 LC_BUILD_VERSION", macos_gate)
        self.assertNotIn("complete-packages:", orchestrator)
        self.assertNotIn("16-archive matrix", packages)

    def test_platform_workflows_audit_short_project_product_names(self) -> None:
        workflow_root = SOURCE_ROOT / ".github/workflows"
        expected_names = {
            "godot-compatibility.yml": (
                "libgdpp.release.linux.x86_64.so",
                "libgdpp.debug.linux.x86_64.so",
            ),
            "android.yml": ("libgdpp\\.release\\.android\\.arm64\\.so",),
            "ios.yml": (
                "libgdpp.release.ios.arm64.xcframework",
                "libgdpp.framework/libgdpp",
            ),
            "web.yml": ("libgdpp.release.web.wasm32",),
        }
        for workflow_name, expected in expected_names.items():
            workflow = (workflow_root / workflow_name).read_text(encoding="utf-8")
            for value in expected:
                self.assertIn(value, workflow)
            self.assertNotIn("libgdpp_project.", workflow)
            self.assertNotIn("gdpp_project.release.", workflow)
            self.assertNotIn("gdpp_project.debug.", workflow)
        packages = (workflow_root / "package-release.yml").read_text(encoding="utf-8")
        self.assertIn("(lib)?gdpp\\.(debug|release)\\.", packages)

    def test_ios_upstream_warning_allowlist_is_exact_and_fail_closed(self) -> None:
        workflow = (
            SOURCE_ROOT / ".github/workflows/ios.yml"
        ).read_text(encoding="utf-8")
        self.assertIn(
            "grep -Eh '(^| )ERROR:|SCRIPT ERROR:|WARNING:|Unable to open'",
            workflow,
        )
        self.assertIn(
            "grep -Fvx \\\n"
            "            'WARNING: Property not found: "
            "application/boot_splash/fullsize'",
            workflow,
        )
        self.assertIn('if [[ -s "$forbidden_diagnostics" ]]; then', workflow)

    def test_godot_compatibility_workflow_tracks_complete_external_projects(self) -> None:
        workflow = (
            SOURCE_ROOT / ".github/workflows/godot-compatibility.yml"
        ).read_text(encoding="utf-8")
        self.assertIn('"4.7.1"', workflow)
        self.assertNotIn('"4.7"]', workflow)
        self.assertIn('typed_variadic_line="$(', workflow)
        self.assertIn('"$fixture/vendor_child.gd" | cut -d: -f1', workflow)
        self.assertIn(
            "GDScript::reload (res://vendor_child.gd:$typed_variadic_line)",
            workflow,
        )
        self.assertNotIn("vendor_child.gd:24", workflow)
        self.assertIn(
            "Verify the latest supported Godot stable frontend pin", workflow
        )
        self.assertIn("tools/update_godot_frontend.py", workflow)
        self.assertIn("test/compatibility/godot_frontend_4_7.json", workflow)
        self.assertIn("tools/run_external_project_e2e.py", workflow)
        for manifest_name in (
            "konado.json",
            "pixelorama.json",
            "open_rpg.json",
            "source_of_mana.json",
        ):
            manifest_path = SOURCE_ROOT / "test/compatibility" / manifest_name
            self.assertIn(f"test/compatibility/{manifest_name}", workflow)
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            self.assertEqual(manifest["repository"].get("checkout"), "full")
            self.assertIn("branch", manifest["repository"])
            self.assertNotIn("commit", manifest["repository"])
            self.assertRegex(manifest["godot"]["engine"], r"^4\.[0-9]+\.[0-9]+$")

    def test_host_staging_excludes_msvc_import_products(self) -> None:
        source = create_host_component(self.temporary / "source", "windows-x64")
        write(source / "binary/gdpp_compiler.windows.x86_64.lib")
        write(source / "binary/gdpp_compiler.windows.x86_64.exp")
        write(source / "binary/gdpp_fallback.windows.x86_64.lib")
        write(source / "binary/gdpp_fallback.windows.x86_64.exp")
        destination = self.temporary / "staged/addons/gdpp"

        stage_host_component.stage_host_component(
            source, destination, "windows-x64"
        )

        self.assertEqual(
            {path.name for path in (destination / "binary").iterdir()},
            {
                "gdpp_compiler.windows.x86_64.dll",
                "gdpp_fallback.windows.x86_64.dll",
            },
        )
        self.assertFalse((destination / "build").exists())

    def test_changelog_uses_unprefixed_exact_version_section(self) -> None:
        content = "## 1.1.0\n\n- New\n\n## 1.0.0\n\n- Initial release\n"
        self.assertEqual(extract_changelog.extract(content, "1.0.0"), "- Initial release\n")
        with self.assertRaisesRegex(ValueError, "without a v prefix"):
            extract_changelog.extract(content, "v1.0.0")
        with self.assertRaisesRegex(ValueError, "exactly one"):
            extract_changelog.extract(content, "2.0.0")


if __name__ == "__main__":
    unittest.main()
