#!/usr/bin/env python3

from __future__ import annotations

from collections import Counter
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


SOURCE_ROOT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).parents[1]
MODULE_PATH = SOURCE_ROOT / "tools/run_external_project_e2e.py"
SPEC = importlib.util.spec_from_file_location("run_external_project_e2e", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
E2E = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(E2E)


class ExternalProjectE2ETest(unittest.TestCase):
    def test_runtime_arguments_select_a_project_mode_after_godot_options(self) -> None:
        command = E2E.project_runtime_command(
            Path("/product"),
            {
                "runtime": {
                    "quit_after": 2,
                    "arguments": ["--server", "--fixture"],
                }
            },
        )
        self.assertEqual(
            command,
            [
                str(Path("/product")),
                "--headless",
                "--audio-driver",
                "Dummy",
                "--quit-after",
                "2",
                "--server",
                "--fixture",
            ],
        )
        with self.assertRaisesRegex(RuntimeError, "runner-owned"):
            E2E.runtime_user_arguments(
                {"runtime": {"arguments": ["--server", "--path=/tmp/other"]}}
            )
        with self.assertRaisesRegex(RuntimeError, "1 through 3600"):
            E2E.runtime_duration({"runtime": {"quit_after": 0}})

    def test_long_running_server_uses_a_liveness_observation(self) -> None:
        manifest = {
            "runtime": {
                "mode": "liveness",
                "observation_seconds": 2,
                "ready_markers": ["server ready"],
                "arguments": ["--server"],
            }
        }
        command = E2E.project_runtime_command(Path("/product"), manifest)
        self.assertEqual(
            command,
            [
                str(Path("/product")),
                "--headless",
                "--audio-driver",
                "Dummy",
                "--server",
            ],
        )
        observed = {
            "exit_code": None,
            "timed_out": True,
            "elapsed_seconds": 2.0,
            "log": "/output/runtime.log",
        }
        with (
            mock.patch.object(E2E, "run", return_value=observed) as runner,
            mock.patch.object(Path, "read_text", return_value="server ready\n"),
        ):
            result = E2E.run_project_runtime(
                Path("/product"),
                manifest,
                cwd=Path("/output"),
                timeout=60,
                log=Path("/output/runtime.log"),
            )
        self.assertTrue(result["liveness_observed"])
        self.assertEqual(result["ready_markers_observed"], ["server ready"])
        self.assertEqual(runner.call_args.kwargs["timeout"], 2.0)
        self.assertTrue(runner.call_args.kwargs["allow_timeout"])
        exited = {
            "exit_code": 0,
            "timed_out": False,
            "elapsed_seconds": 0.5,
            "log": "/output/runtime.log",
        }
        with mock.patch.object(E2E, "run", return_value=exited):
            with self.assertRaisesRegex(RuntimeError, "exited before"):
                E2E.run_project_runtime(
                    Path("/product"),
                    manifest,
                    cwd=Path("/output"),
                    timeout=60,
                    log=Path("/output/runtime.log"),
                )
        with self.assertRaisesRegex(RuntimeError, "must use observation_seconds"):
            E2E.runtime_mode(
                {
                    "runtime": {
                        "mode": "liveness",
                        "quit_after": 2,
                    }
                }
            )
        with self.assertRaisesRegex(RuntimeError, "runner-owned"):
            E2E.runtime_user_arguments(
                {"runtime": {"arguments": ["--server", "--quit-after=1"]}}
            )
        with self.assertRaisesRegex(RuntimeError, "single-line"):
            E2E.runtime_ready_markers(
                {"runtime": {"ready_markers": ["server ready\nforged"]}}
            )
        with self.assertRaisesRegex(RuntimeError, "1 through 10"):
            E2E.runtime_baseline_samples(
                {"runtime": {"baseline_samples": 0}}
            )
        with (
            mock.patch.object(E2E, "run", return_value=observed),
            mock.patch.object(Path, "read_text", return_value="not ready\n"),
            self.assertRaisesRegex(RuntimeError, "required runtime readiness"),
        ):
            E2E.run_project_runtime(
                Path("/product"),
                manifest,
                cwd=Path("/output"),
                timeout=60,
                log=Path("/output/runtime.log"),
            )

    def test_bootstrap_import_retries_only_failed_processes(self) -> None:
        failed = {"exit_code": -6, "timed_out": False, "log": "attempt-1.log"}
        passed = {"exit_code": 0, "timed_out": False, "log": "attempt-2.log"}
        with mock.patch.object(E2E, "run", side_effect=[failed, passed]) as runner:
            result = E2E.run_bootstrap_import(
                Path("/godot"),
                Path("/project"),
                60,
                Path("/output"),
                "baseline-import-bootstrap",
            )
        self.assertEqual(result["successful_attempt"], 2)
        self.assertEqual(len(result["attempts"]), 2)
        self.assertEqual(runner.call_count, 2)
        self.assertTrue(runner.call_args_list[0].kwargs["allow_failure"])

    def test_bootstrap_import_rejects_persistent_failures(self) -> None:
        failed = {"exit_code": -6, "timed_out": False, "log": "attempt.log"}
        with mock.patch.object(E2E, "run", return_value=failed) as runner:
            with self.assertRaisesRegex(RuntimeError, "3 consecutive attempts"):
                E2E.run_bootstrap_import(
                    Path("/godot"),
                    Path("/project"),
                    60,
                    Path("/output"),
                    "import-bootstrap",
                )
        self.assertEqual(runner.call_count, 3)

    def test_bootstrap_import_converges_successful_uid_scans(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)

            def import_run(*args, **kwargs):
                log = kwargs["log"]
                if log.name.endswith("-1.log"):
                    log.write_text(
                        'ERROR: Unrecognized UID: "uid://fresh".\n',
                        encoding="utf-8",
                    )
                else:
                    log.write_text("", encoding="utf-8")
                return {"exit_code": 0, "timed_out": False, "log": str(log)}

            with mock.patch.object(E2E, "run", side_effect=import_run) as runner:
                result = E2E.run_bootstrap_import(
                    Path("/godot"),
                    Path("/project"),
                    60,
                    output,
                    "import-bootstrap",
                )

        self.assertEqual(result["successful_attempt"], 2)
        self.assertEqual(len(result["attempts"]), 2)
        self.assertEqual(runner.call_count, 2)

    def test_import_gate_rejects_only_diagnostics_added_by_gdpp(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            baseline = root / "baseline.log"
            baseline.write_text(
                "SCRIPT ERROR: existing customer failure\n"
                "WARNING: ObjectDB instances leaked at exit\n"
                "   at: cleanup (core/object/object.cpp:2663)\n",
                encoding="utf-8",
            )
            baseline_diagnostics, report = E2E.report_diagnostics(baseline)
            self.assertEqual(
                report,
                [
                    {
                        "message": "SCRIPT ERROR: existing customer failure",
                        "count": 1,
                    },
                    {
                        "message": (
                            "WARNING: ObjectDB instances leaked at exit | "
                            "at: cleanup (core/object/object.cpp:2663)"
                        ),
                        "count": 1,
                    },
                ],
            )
            unchanged = root / "unchanged.log"
            unchanged.write_text(
                "\x1b[31mSCRIPT ERROR: existing customer failure\x1b[0m\n"
                "WARNING: ObjectDB instances leaked at exit\n"
                "   at: cleanup (core/object/object.cpp:2663)\n",
                encoding="utf-8",
            )
            E2E.assert_no_new_diagnostics(
                unchanged, baseline_diagnostics, "Godot import"
            )
            regressed = root / "regressed.log"
            regressed.write_text(
                unchanged.read_text(encoding="utf-8")
                + "ERROR: GDPP introduced a failure\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(RuntimeError, "introduced a diagnostic"):
                E2E.assert_no_new_diagnostics(
                    regressed, baseline_diagnostics, "Godot import"
                )
            repeated = root / "repeated.log"
            repeated.write_text(
                unchanged.read_text(encoding="utf-8")
                + "SCRIPT ERROR: existing customer failure\n",
                encoding="utf-8",
            )
            E2E.assert_no_new_diagnostics(
                repeated, baseline_diagnostics, "Godot import"
            )
            different_origin = root / "different-origin.log"
            different_origin.write_text(
                "SCRIPT ERROR: existing customer failure\n"
                "   at: customer_plugin (res://addons/customer/plugin.gd:10)\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(RuntimeError, "diagnostic signature"):
                E2E.assert_no_new_diagnostics(
                    different_origin, baseline_diagnostics, "Godot import"
                )
            bootstrap = root / "bootstrap.log"
            bootstrap.write_text(
                "SCRIPT ERROR: existing customer failure\n"
                "SCRIPT ERROR: existing customer failure\n",
                encoding="utf-8",
            )
            envelope = E2E.diagnostic_envelope(baseline, bootstrap)
            self.assertEqual(
                envelope["SCRIPT ERROR: existing customer failure"],
                2,
            )
            E2E.assert_no_new_diagnostics(repeated, envelope, "Godot import")
            bootstrap_regression = root / "bootstrap-regression.log"
            bootstrap_regression.write_text(
                bootstrap.read_text(encoding="utf-8")
                + "ERROR: GDPP bootstrap failure\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(RuntimeError, "introduced a diagnostic"):
                E2E.assert_no_new_diagnostics(
                    bootstrap_regression,
                    E2E.diagnostic_envelope(bootstrap),
                    "Godot bootstrap import",
                )
            baseline_leak = root / "baseline-leak.log"
            baseline_leak.write_text(
                "ERROR: 168 resources still in use at exit.\n"
                "   at: clear (core/io/resource.cpp:822)\n",
                encoding="utf-8",
            )
            smaller_leak = root / "smaller-leak.log"
            smaller_leak.write_text(
                "ERROR: 4 resources still in use at exit.\n"
                "   at: clear (core/io/resource.cpp:822)\n",
                encoding="utf-8",
            )
            E2E.assert_no_new_diagnostics(
                smaller_leak,
                E2E.diagnostic_envelope(baseline_leak),
                "Godot bootstrap import",
            )
            larger_leak = root / "larger-leak.log"
            larger_leak.write_text(
                "ERROR: 169 resources still in use at exit.\n"
                "   at: clear (core/io/resource.cpp:822)\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(RuntimeError, "leak magnitude"):
                E2E.assert_no_new_diagnostics(
                    larger_leak,
                    E2E.diagnostic_envelope(baseline_leak),
                    "Godot bootstrap import",
                )
        source = MODULE_PATH.read_text(encoding="utf-8")
        self.assertIn(
            "baseline_export_diagnostics",
            source,
        )
        self.assertIn(
            "baseline_runtime_diagnostics",
            source,
        )

    def test_editor_tls_transport_noise_is_isolated_only_during_aot_export(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            transport = root / "transport.log"
            fingerprint = (
                "ERROR: TLS handshake error: -27648 | "
                "at: _do_handshake (modules/mbedtls/stream_peer_mbedtls.cpp:88)"
            )
            transport.write_text(
                "ERROR: TLS handshake error: -27648\n"
                "   at: _do_handshake (modules/mbedtls/stream_peer_mbedtls.cpp:88)\n"
                "mbedtls error: returned -0x6c00\n",
                encoding="utf-8",
            )

            with self.assertRaisesRegex(RuntimeError, "introduced a diagnostic"):
                E2E.assert_no_new_diagnostics(
                    transport,
                    Counter(),
                    "exported runtime",
                )
            isolated = E2E.assert_no_new_diagnostics(
                transport,
                Counter(),
                "GDPP AOT export",
                allow_editor_tls_transport=True,
            )
            detail = "mbedtls error: returned -0x6c00"
            self.assertEqual(isolated, Counter({fingerprint: 1, detail: 1}))
            self.assertEqual(
                E2E.diagnostic_report(isolated),
                [
                    {"message": fingerprint, "count": 1},
                    {"message": detail, "count": 1},
                ],
            )
            detail_only = root / "detail-only.log"
            detail_only.write_text(detail + "\n", encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "introduced a diagnostic"):
                E2E.assert_no_new_diagnostics(
                    detail_only,
                    Counter(),
                    "GDPP AOT export",
                    allow_editor_tls_transport=True,
                )

            project_origin = root / "project-origin.log"
            project_origin.write_text(
                "ERROR: TLS handshake error: -27648\n"
                "   at: request (res://network/client.gd:42)\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(RuntimeError, "introduced a diagnostic"):
                E2E.assert_no_new_diagnostics(
                    project_origin,
                    Counter(),
                    "GDPP AOT export",
                    allow_editor_tls_transport=True,
                )

            mixed = root / "mixed.log"
            mixed.write_text(
                transport.read_text(encoding="utf-8")
                + "ERROR: GDPP project library failed to load\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                RuntimeError,
                "GDPP project library failed to load",
            ):
                E2E.assert_no_new_diagnostics(
                    mixed,
                    Counter(),
                    "GDPP AOT export",
                    allow_editor_tls_transport=True,
                )

    def test_objectdb_cleanup_context_is_owned_by_the_leak_diagnostic(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "runtime.log"
            log.write_text(
                "ERROR: Shader parameter mismatch\n"
                "   at: set_shader_parameter (scene/resources/material.cpp:1)\n"
                "WARNING: ObjectDB instances leaked at exit\n"
                "   at: cleanup (core/object/object.cpp:2663)\n",
                encoding="utf-8",
            )
            self.assertEqual(
                E2E.diagnostic_fingerprints(log),
                [
                    (
                        "ERROR: Shader parameter mismatch | "
                        "at: set_shader_parameter (scene/resources/material.cpp:1)"
                    ),
                    (
                        "WARNING: ObjectDB instances leaked at exit | "
                        "at: cleanup (core/object/object.cpp:2663)"
                    ),
                ],
            )

    def test_enable_plugin_preserves_existing_multiline_entries(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            project = Path(temporary) / "project.godot"
            project.write_text(
                '[application]\nconfig/name="Fixture"\n\n'
                "[editor_plugins]\n\n"
                "enabled=PackedStringArray(\n"
                '    "res://addons/one/plugin.cfg",\n'
                '    "res://addons/two/plugin.cfg"\n'
                ")\n\n"
                "[rendering]\nrenderer/rendering_method=\"gl_compatibility\"\n",
                encoding="utf-8",
            )
            E2E.enable_plugin(project)
            content = project.read_text(encoding="utf-8")
            self.assertEqual(content.count(E2E.PLUGIN_RESOURCE), 1)
            self.assertIn("res://addons/one/plugin.cfg", content)
            self.assertIn("res://addons/two/plugin.cfg", content)
            self.assertIn("[rendering]", content)

    def test_evidence_reset_removes_only_stale_top_level_logs(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            stale = output / "export.log"
            retained = output / "report.json"
            nested = output / "nested"
            nested.mkdir()
            nested_log = nested / "runtime.log"
            stale.write_text("old", encoding="utf-8")
            retained.write_text("{}", encoding="utf-8")
            nested_log.write_text("nested", encoding="utf-8")

            E2E.clear_stale_evidence_logs(output)

            self.assertFalse(stale.exists())
            self.assertTrue(retained.exists())
            self.assertTrue(nested_log.exists())

    def test_repeated_runs_restore_the_exact_pristine_project_state(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            project = Path(temporary)
            subprocess.run(["git", "init", "-q"], cwd=project, check=True)
            subprocess.run(
                ["git", "config", "user.email", "e2e@example.invalid"],
                cwd=project,
                check=True,
            )
            subprocess.run(
                ["git", "config", "user.name", "E2E"],
                cwd=project,
                check=True,
            )
            original_project = (
                '[application]\nconfig/name="Fixture"\n\n'
                "[editor_plugins]\n\n"
                "enabled=PackedStringArray(\n"
                '    "res://addons/one/plugin.cfg",\n'
                '    "res://addons/two/plugin.cfg"\n'
                ")\n"
            )
            original_presets = '[preset.0]\n\nname="Existing"\n'
            (project / "project.godot").write_text(
                original_project, encoding="utf-8"
            )
            (project / "export_presets.cfg").write_text(
                original_presets, encoding="utf-8"
            )
            subprocess.run(["git", "add", "."], cwd=project, check=True)
            subprocess.run(["git", "commit", "-qm", "fixture"], cwd=project, check=True)

            state = E2E.prepare_pristine_e2e_state(project)
            E2E.enable_plugin(project / "project.godot")
            E2E.append_export_preset(project, "linux", project / "output")
            addon = project / "addons/gdpp"
            addon.mkdir(parents=True)
            (addon / "plugin.cfg").write_text(
                '[plugin]\nname="GDPP"\n', encoding="utf-8"
            )
            E2E.restore_pristine_e2e_state(project, state)
            self.assertEqual(
                (project / "project.godot").read_text(encoding="utf-8"),
                original_project,
            )
            self.assertEqual(
                (project / "export_presets.cfg").read_text(encoding="utf-8"),
                original_presets,
            )
            self.assertFalse(addon.exists())

            # A killed previous run is also recovered before the next pristine baseline.
            E2E.enable_plugin(project / "project.godot")
            E2E.append_export_preset(project, "linux", project / "output")
            godot_cache = project / ".godot"
            godot_cache.mkdir()
            (godot_cache / "extension_list.cfg").write_text(
                'res://addons/gdpp/gdpp.gdextension\n',
                encoding="utf-8",
            )
            recovered = E2E.prepare_pristine_e2e_state(project)
            self.assertEqual(recovered["project.godot"], original_project.encode())
            self.assertEqual(
                (project / "project.godot").read_text(encoding="utf-8"),
                original_project,
            )
            self.assertFalse(godot_cache.exists())

    def test_install_addon_selects_one_sdk_and_excludes_generated_state(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            addon = root / "source"
            project = root / "project"
            for version in ("4.6", "4.7"):
                sdk = addon / "sdk" / version
                sdk.mkdir(parents=True)
                manifest = (
                    sdk / "sdk.manifest"
                    if version == "4.6"
                    else sdk / "manifests/linux.x86_64.sdk.manifest"
                )
                manifest.parent.mkdir(parents=True, exist_ok=True)
                manifest.write_text(version, encoding="utf-8")
            (addon / "binary").mkdir()
            (addon / "binary/compiler.so").write_text("compiler", encoding="utf-8")
            (addon / "binary/libgdpp.release.linux.x86_64.so").write_text(
                "generated", encoding="utf-8"
            )
            (addon / "build").mkdir()
            (addon / "build/cache").write_text("cache", encoding="utf-8")
            (addon / "plugin.cfg").write_text("[plugin]\n", encoding="utf-8")
            destination = E2E.install_addon(addon, project, "4.7")
            self.assertTrue(
                (
                    destination
                    / "sdk/4.7/manifests/linux.x86_64.sdk.manifest"
                ).is_file()
            )
            self.assertFalse((destination / "sdk/4.6").exists())
            self.assertTrue((destination / "binary/compiler.so").is_file())
            self.assertFalse(
                (destination / "binary/libgdpp.release.linux.x86_64.so").exists()
            )
            self.assertFalse((destination / "build").exists())

    def test_export_preset_uses_a_new_index_and_binary_only_options(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            project = Path(temporary)
            (project / "export_presets.cfg").write_text(
                '[preset.2]\n\nname="Existing"\n', encoding="utf-8"
            )
            name, product = E2E.append_export_preset(
                project, "linux", project / "artifacts"
            )
            content = (project / "export_presets.cfg").read_text(encoding="utf-8")
            self.assertEqual(name, "GDPP External Project E2E")
            self.assertEqual(product, project / "artifacts/product.x86_64")
            self.assertIn("[preset.3]", content)
            self.assertIn("script_export_mode=2", content)
            self.assertIn("gdpp/strip_gdscript_sources=true", content)
            self.assertIn("gdpp/allow_source_fallback=false", content)
            self.assertIn("binary_format/embed_pck=false", content)
            self.assertTrue((project / "artifacts").is_dir())

    def test_export_preset_clones_customer_platform_resource_contract(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            project = Path(temporary)
            (project / "export_presets.cfg").write_text(
                '[preset.4]\n\n'
                'name="Customer macOS"\n'
                'platform="macOS"\n'
                "runnable=true\n"
                'custom_features="customer_feature"\n'
                'export_filter="all_resources"\n'
                'include_filter="data/conf/*,data/db/*"\n'
                'exclude_filter="data/maps/*"\n'
                'export_path="customer.zip"\n'
                "script_export_mode=1\n\n"
                "[preset.4.options]\n\n"
                'binary_format/architecture="arm64"\n'
                'application/bundle_identifier="org.customer.game"\n'
                "codesign/codesign=0\n",
                encoding="utf-8",
            )

            _, product = E2E.append_export_preset(
                project, "macos", project / "artifacts"
            )
            content = (project / "export_presets.cfg").read_text(encoding="utf-8")
            cloned = content[content.index("[preset.5]") :]

            self.assertEqual(product, project / "artifacts/product.app")
            self.assertIn('custom_features="customer_feature"', cloned)
            self.assertIn('include_filter="data/conf/*,data/db/*"', cloned)
            self.assertIn('exclude_filter="data/maps/*"', cloned)
            self.assertIn(
                'application/bundle_identifier="org.customer.game"', cloned
            )
            self.assertIn("codesign/codesign=0", cloned)
            self.assertIn('binary_format/architecture="universal"', cloned)
            self.assertNotIn('binary_format/architecture="arm64"', cloned)
            self.assertIn("script_export_mode=2", cloned)
            self.assertIn("gdpp/strip_gdscript_sources=true", cloned)
            self.assertIn("gdpp/allow_source_fallback=false", cloned)
            self.assertIn("binary_format/embed_pck=false", cloned)

    def test_export_preset_overrides_an_embedded_customer_pck_for_audit(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            project = Path(temporary)
            (project / "export_presets.cfg").write_text(
                '[preset.0]\n\n'
                'name="Customer Linux"\n'
                'platform="Linux"\n'
                'export_path="customer.x86_64"\n\n'
                "[preset.0.options]\n\n"
                'binary_format/architecture="x86_64"\n'
                "binary_format/embed_pck=true\n",
                encoding="utf-8",
            )

            E2E.append_export_preset(project, "linux", project / "artifacts")
            content = (project / "export_presets.cfg").read_text(encoding="utf-8")
            cloned = content[content.index("[preset.1]") :]

            self.assertIn("binary_format/embed_pck=false", cloned)
            self.assertNotIn("binary_format/embed_pck=true", cloned)

    def test_macos_export_matches_the_official_universal_template(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            project = Path(temporary)
            _, product = E2E.append_export_preset(
                project, "macos", project / "artifacts"
            )
            content = (project / "export_presets.cfg").read_text(encoding="utf-8")
            self.assertEqual(product, project / "artifacts/product.app")
            self.assertIn('binary_format/architecture="universal"', content)

    def test_pck_audit_defers_loader_runtime_diagnostics_to_the_exported_app(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "audit.log"
            log.write_text(
                "PCK_AUDIT_FILES=20\n"
                "PCK_AUDIT_PROJECT_LIBRARIES=1\n"
                "PCK_AUDIT_VIOLATIONS=0\n"
                "ERROR: custom loader is unavailable in the isolated audit host\n",
                encoding="utf-8",
            )
            E2E.assert_zero_pck_violations(log)
            log.write_text(
                "PCK_AUDIT_VIOLATIONS=1\nERROR: forbidden source file\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(RuntimeError, "zero-violation"):
                E2E.assert_zero_pck_violations(log)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
