#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest


SOURCE_ROOT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).parents[1]
MODULE_PATH = SOURCE_ROOT / "tools/run_external_project_e2e.py"
SPEC = importlib.util.spec_from_file_location("run_external_project_e2e", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
E2E = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(E2E)


class ExternalProjectE2ETest(unittest.TestCase):
    def test_import_gate_rejects_only_diagnostics_added_by_gdpp(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            baseline = root / "baseline.log"
            baseline.write_text(
                "SCRIPT ERROR: existing customer failure\n"
                "WARNING: ObjectDB instances leaked at exit\n",
                encoding="utf-8",
            )
            baseline_diagnostics = set(E2E.diagnostic_fingerprints(baseline))
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
        source = MODULE_PATH.read_text(encoding="utf-8")
        self.assertIn(
            'assert_no_new_diagnostics(export_log, baseline_diagnostics, "GDPP AOT export")',
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


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
