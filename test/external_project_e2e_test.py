#!/usr/bin/env python3

from __future__ import annotations

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

    def test_import_gate_rejects_only_diagnostics_added_by_gdpp(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            baseline = root / "baseline.log"
            baseline.write_text(
                "SCRIPT ERROR: existing customer failure\n"
                "WARNING: ObjectDB instances leaked at exit\n",
                encoding="utf-8",
            )
            baseline_diagnostics, report = E2E.report_diagnostics(baseline)
            self.assertEqual(
                report,
                [
                    {
                        "message": "SCRIPT ERROR: existing customer failure",
                        "count": 1,
                    }
                ],
            )
            unchanged = root / "unchanged.log"
            unchanged.write_text(
                "\x1b[31mSCRIPT ERROR: existing customer failure\x1b[0m\n"
                "WARNING: ObjectDB instances leaked at exit\n",
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
            with self.assertRaisesRegex(RuntimeError, "additional occurrence"):
                E2E.assert_no_new_diagnostics(
                    repeated, baseline_diagnostics, "Godot import"
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
        source = MODULE_PATH.read_text(encoding="utf-8")
        self.assertIn(
            "baseline_export_diagnostics",
            source,
        )
        self.assertIn(
            "baseline_runtime_diagnostics",
            source,
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
            recovered = E2E.prepare_pristine_e2e_state(project)
            self.assertEqual(recovered["project.godot"], original_project.encode())
            self.assertEqual(
                (project / "project.godot").read_text(encoding="utf-8"),
                original_project,
            )

    def test_install_addon_selects_one_sdk_and_excludes_generated_state(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            addon = root / "source"
            project = root / "project"
            for version in ("4.6", "4.7"):
                sdk = addon / "sdk" / version
                sdk.mkdir(parents=True)
                (sdk / "sdk.manifest").write_text(version, encoding="utf-8")
            (addon / "binary").mkdir()
            (addon / "binary/compiler.so").write_text("compiler", encoding="utf-8")
            (addon / "binary/libgdpp.release.linux.x86_64.so").write_text(
                "generated", encoding="utf-8"
            )
            (addon / "build").mkdir()
            (addon / "build/cache").write_text("cache", encoding="utf-8")
            (addon / "plugin.cfg").write_text("[plugin]\n", encoding="utf-8")
            destination = E2E.install_addon(addon, project, "4.7")
            self.assertTrue((destination / "sdk/4.7/sdk.manifest").is_file())
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
            self.assertTrue((project / "artifacts").is_dir())

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
