#!/usr/bin/env python3
"""Validate the CI graphics capability boundary without a display or packages."""
from __future__ import annotations

from contextlib import redirect_stdout
import io
import os
from pathlib import Path
import signal
import stat
import subprocess
import tempfile
import unittest
from unittest import mock

import setup_editor_display as display


SUMMARY = """Instance Extensions:
    VK_KHR_surface : extension revision 25
GPU0:
    deviceType = PHYSICAL_DEVICE_TYPE_CPU
"""


class SetupEditorDisplayTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="gdpp-display-")
        self.root = Path(self.temporary.name).resolve()
        self.manifest = self.root / "lvp_icd.x86_64.json"
        self.manifest.write_text("{}", encoding="utf-8")
        self.env_file = self.root / "job.env"
        self.env_file.write_text("EXISTING=value\n", encoding="utf-8")
        self.environment = {"RUNNER_TEMP": str(self.root), "GITHUB_ENV": str(self.env_file),
                            "VK_DRIVER_FILES": "old-provider", "VK_ICD_FILENAMES": "other-provider"}

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_selects_the_actual_supported_manifest_and_ignores_other_architectures(self) -> None:
        other = self.root / "lvp_icd.i686.json"
        other.write_text("{}", encoding="utf-8")
        for name in ("lvp_icd.x86_64.json", "lvp_icd.json"):
            with self.subTest(name=name):
                if self.manifest.exists():
                    self.manifest.unlink()
                chosen = self.root / name
                chosen.write_text("{}", encoding="utf-8")
                self.assertEqual(display.select_manifest(str(other) + "\n" + str(chosen)), chosen)
                chosen.unlink()

    def test_missing_unreadable_or_ambiguous_manifests_fail_without_a_fallback(self) -> None:
        generic = self.root / "lvp_icd.json"
        generic.write_text("{}", encoding="utf-8")
        for listing in ("/missing/lvp_icd.x86_64.json", str(self.manifest) + "\n" + str(generic)):
            with self.subTest(listing=listing), self.assertRaisesRegex(display.DisplaySetupError, "^manifest$"):
                display.select_manifest(listing)
        with mock.patch.object(display.os, "access", return_value=False):
            with self.assertRaisesRegex(display.DisplaySetupError, "^manifest$"):
                display.select_manifest(str(self.manifest))

    def test_package_query_is_explicitly_amd64_and_bounded(self) -> None:
        result = subprocess.CompletedProcess([], 0, str(self.manifest) + "\n")
        with mock.patch.object(display.subprocess, "run", return_value=result) as run:
            self.assertEqual(display.query_manifest(self.environment, io.BytesIO()), self.manifest)
        self.assertEqual(run.call_args.args[0], ["dpkg-query", "-L", "mesa-vulkan-drivers:amd64"])
        self.assertEqual(run.call_args.kwargs["timeout"], 10)
        result.returncode = 1
        with mock.patch.object(display.subprocess, "run", return_value=result):
            with self.assertRaisesRegex(display.DisplaySetupError, "^package-query$"):
                display.query_manifest(self.environment, io.BytesIO())

    def test_summary_requires_the_exact_extension_row_and_cpu_assignment(self) -> None:
        display.verify_summary(SUMMARY)
        for payload, cause in (
            (SUMMARY.replace("VK_KHR_surface :", "VK_KHR_surface_private :"), "surface-extension"),
            (SUMMARY.replace("VK_KHR_surface :", "ERROR: VK_KHR_surface :"), "surface-extension"),
            (SUMMARY.replace("PHYSICAL_DEVICE_TYPE_CPU", "PHYSICAL_DEVICE_TYPE_CPU_private"), "cpu-device"),
            (SUMMARY.replace("PHYSICAL_DEVICE_TYPE_CPU", "PHYSICAL_DEVICE_TYPE_DISCRETE_GPU"), "cpu-device"),
        ):
            with self.subTest(cause=cause), self.assertRaisesRegex(display.DisplaySetupError, "^" + cause + "$"):
                display.verify_summary(payload)

    def test_success_exports_one_provider_and_private_runtime_after_verification(self) -> None:
        before = dict(self.environment)

        def probe(environment, log):
            self.assertEqual(environment["VK_DRIVER_FILES"], str(self.manifest))
            self.assertEqual(environment["VK_DRIVER_FILES"], environment["VK_ICD_FILENAMES"])
            self.assertEqual(environment["LC_ALL"], "C")
            runtime = Path(environment["XDG_RUNTIME_DIR"])
            self.assertEqual(runtime.parent, self.root)
            self.assertEqual(stat.S_IMODE(runtime.stat().st_mode), 0o700)
            log.write(SUMMARY.encode())

        with mock.patch.object(display, "query_manifest", return_value=self.manifest), \
                mock.patch.object(display, "run_probe", side_effect=probe):
            display.configure(self.environment)
        self.assertEqual(self.environment, before)
        exports = dict(line.split("=", 1) for line in self.env_file.read_text().splitlines())
        self.assertEqual(exports["VK_DRIVER_FILES"], str(self.manifest))
        self.assertEqual(exports["VK_ICD_FILENAMES"], str(self.manifest))
        log = Path(exports["XDG_RUNTIME_DIR"]) / "vulkaninfo.log"
        self.assertEqual(stat.S_IMODE(log.stat().st_mode), 0o600)
        self.assertIn(SUMMARY, log.read_text())

    def test_failure_keeps_system_output_private_and_does_not_export_a_provider(self) -> None:
        def probe(unused, log):
            log.write(b"/private/customer/driver.so: secret-token\n")
            raise display.DisplaySetupError("probe-exit")

        output = io.StringIO()
        with mock.patch.dict(os.environ, self.environment, clear=True), \
                mock.patch.object(display, "query_manifest", return_value=self.manifest), \
                mock.patch.object(display, "run_probe", side_effect=probe), redirect_stdout(output):
            self.assertEqual(display.main(), 1)
        self.assertEqual(output.getvalue(), "GDPP_EDITOR_DISPLAY_FAILED=probe-exit\n")
        self.assertEqual(self.env_file.read_text(), "EXISTING=value\n")
        self.assertIn(b"secret-token", next(self.root.glob("gdpp-editor-runtime-*/vulkaninfo.log")).read_bytes())

    def test_package_listing_cannot_impersonate_the_actual_vulkan_summary(self) -> None:
        def query(unused, log):
            log.write(SUMMARY.encode())
            return self.manifest

        with mock.patch.object(display, "query_manifest", side_effect=query), \
                mock.patch.object(display, "run_probe"):
            with self.assertRaisesRegex(display.DisplaySetupError, "^surface-extension$"):
                display.configure(self.environment)
        self.assertEqual(self.env_file.read_text(), "EXISTING=value\n")

    def test_probe_timeout_kills_only_its_owned_session_and_stays_bounded(self) -> None:
        process = mock.Mock(pid=19283)
        process.wait.side_effect = [subprocess.TimeoutExpired(display.PROBE_COMMAND, 30), 0]
        with mock.patch.object(display.subprocess, "Popen", return_value=process) as launch, \
                mock.patch.object(display.os, "killpg") as kill:
            with self.assertRaisesRegex(display.DisplaySetupError, "^probe-timeout$"):
                display.run_probe(self.environment, io.BytesIO())
        self.assertTrue(launch.call_args.kwargs["start_new_session"])
        self.assertEqual(launch.call_args.args[0], display.PROBE_COMMAND)
        self.assertEqual(process.wait.call_args_list, [mock.call(timeout=30), mock.call(timeout=5)])
        kill.assert_called_once_with(process.pid, signal.SIGKILL)

    def test_unsafe_environment_values_and_unknown_errors_never_escape_to_stdout(self) -> None:
        invalid = {**self.environment, "GITHUB_ENV": str(self.env_file) + "\nsecret-token"}
        with self.assertRaisesRegex(display.DisplaySetupError, "^environment$"):
            display.configure(invalid)
        output = io.StringIO()
        with mock.patch.object(display, "configure", side_effect=display.DisplaySetupError("private-source")), \
                redirect_stdout(output):
            self.assertEqual(display.main(), 1)
        self.assertEqual(output.getvalue(), "GDPP_EDITOR_DISPLAY_FAILED=internal\n")


if __name__ == "__main__":
    unittest.main()
