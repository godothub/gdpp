#!/usr/bin/env python3
"""Behavior tests for the private CI output boundary."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import stat
import subprocess
import tempfile
import unittest


HOOK = Path(__file__).with_name("private_step_capture.sh")


class PrivateStepCaptureTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="gdpp-private-step-")
        self.root = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def run_script(self, script: str) -> subprocess.CompletedProcess[bytes]:
        environment = {
            **os.environ,
            "BASH_ENV": str(HOOK),
            "GITHUB_ACTIONS": "true",
            "GITHUB_ACTION": "__run_17",
            "GITHUB_JOB": "compiler-core",
            "GITHUB_WORKSPACE": str(HOOK.parents[2]),
            "RUNNER_TEMP": str(self.root),
        }
        environment.pop("GDPP_PRIVATE_CAPTURE_ACTIVE", None)
        return subprocess.run(
            ["bash", "--noprofile", "--norc", "-c", script],
            env=environment,
            capture_output=True,
            check=False,
        )

    def test_success_emits_only_a_stage_summary_and_removes_raw_output(self) -> None:
        result = self.run_script(
            "printf '%s\\n' '/private/source/file.cpp: secret source line'"
        )
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stderr, b"")
        self.assertIn(b"status=success", result.stdout)
        self.assertNotIn(b"source/file.cpp", result.stdout)
        self.assertNotIn(b"secret source line", result.stdout)
        self.assertEqual(list(self.root.glob("gdpp-private-step.*.log")), [])

    def test_failure_is_bounded_to_category_exit_and_safe_test_name(self) -> None:
        result = self.run_script(
            "printf '%s\\n' "
            "'/home/runner/work/private/source.cpp:41: fatal error: secret' "
            "'[fail] private unit case' "
            "'       /private/source/test.cpp:42: requirement failed: secret' "
            "'[fail] private unit case' "
            "'The following tests FAILED:' "
            "'  7 - gdpp.runtime.contract (Failed)'; exit 7"
        )
        self.assertEqual(result.returncode, 7)
        self.assertEqual(result.stderr, b"")
        self.assertIn(b"status=failed", result.stdout)
        self.assertIn(b"category=compile", result.stdout)
        self.assertIn(b"exit=7", result.stdout)
        self.assertIn(b"tests=gdpp.runtime.contract", result.stdout)
        case_id = hashlib.sha256(b"private unit case").hexdigest().encode("ascii")[:16]
        self.assertIn(b"cases=" + case_id, result.stdout)
        self.assertEqual(result.stdout.count(case_id), 1)
        self.assertNotIn(b"private unit case", result.stdout)
        self.assertNotIn(b"secret", result.stdout)
        self.assertNotIn(b"test.cpp", result.stdout)
        self.assertNotIn(b"runner/work", result.stdout)
        logs = list(self.root.glob("gdpp-private-step.*.log"))
        self.assertEqual(len(logs), 1)
        self.assertIn(b"secret", logs[0].read_bytes())
        self.assertEqual(stat.S_IMODE(logs[0].stat().st_mode), 0o600)

    def test_unittest_failures_hash_only_method_names(self) -> None:
        result = self.run_script(
            "printf '%s\\n' "
            "'FAIL: test_timeout (__main__.PrivateCase.test_timeout) (argument=secret)' "
            "'ERROR: test_shutdown (__main__.PrivateCase.test_shutdown)' "
            "'FAIL: test_timeout (__main__.PrivateCase.test_timeout) (argument=another-secret)' "
            "'ERROR: parse_file (/private/source.py:42)' "
            "'FAIL: secret assertion content' "
            "'AssertionError: /private/source.py: secret'; exit 1"
        )
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stderr, b"")
        cases = sorted(
            hashlib.sha256(name).hexdigest().encode("ascii")[:16]
            for name in (b"test_timeout", b"test_shutdown")
        )
        fields = result.stdout.split()
        self.assertEqual(
            [field for field in fields if field.startswith(b"cases=")],
            [b"cases=" + b",".join(cases)],
        )
        for private in (b"test_timeout", b"test_shutdown", b"PrivateCase", b"secret", b"source.py"):
            self.assertNotIn(private, result.stdout)

    def test_workflow_cleanup_can_chain_the_capture_exit_handler(self) -> None:
        result = self.run_script(
            "trap 'status=$?; printf cleanup-secret; "
            "gdpp_private_capture_exit \"$status\"' EXIT; exit 9"
        )
        self.assertEqual(result.returncode, 9)
        self.assertIn(b"status=failed", result.stdout)
        self.assertNotIn(b"cleanup-secret", result.stdout)

    def test_build_failures_expose_only_tool_codes_and_fixed_categories(self) -> None:
        result = self.run_script(
            "printf '%s\\n' "
            "'/private/source.cpp:17: error: secret [-Werror=unused-parameter]' "
            "'/private/source.cpp:18: error: secret [-Werror,-Wshadow]' "
            "'/private/source.cpp(17): error C2664: secret' "
            "'clang: error: unable to execute command: Killed: 9'; exit 1"
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn(b"category=resource", result.stdout)
        self.assertIn(b"codes=-Werror,-Werror=unused-parameter,-Wshadow,C2664,process-killed", result.stdout)
        for private in (b"source.cpp", b"secret", b"unable to execute command"):
            self.assertNotIn(private, result.stdout)

    def test_configure_failures_identify_the_probe_without_its_output(self) -> None:
        result = self.run_script(
            "printf '%s\\n' 'CMake Error at /private/CMakeLists.txt:17:' "
            "'Cannot detect MSVC header dependencies: secret'; exit 1"
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn(b"category=configure", result.stdout)
        self.assertIn(b"codes=msvc-include-probe", result.stdout)
        self.assertNotIn(b"secret", result.stdout)
        self.assertNotIn(b"CMakeLists.txt", result.stdout)

    def test_dependency_policy_warning_does_not_obscure_a_generator_error(self) -> None:
        result = self.run_script(
            "printf '%s\\n' 'CMake Deprecation Warning:' "
            "'Compatibility with CMake < 3.10 will be removed' "
            "'CMake Error: Error evaluating generator expression:' "
            "'private target name'; exit 1"
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn(b"codes=generator-expression", result.stdout)
        self.assertNotIn(b"cmake-policy-floor", result.stdout)
        self.assertNotIn(b"private target name", result.stdout)

    def test_script_error_keeps_its_godot_category(self) -> None:
        result = self.run_script(
            "printf '%s\\n' 'SCRIPT ERROR: private detail'; exit 1"
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn(b"category=godot", result.stdout)
        self.assertNotIn(b"private detail", result.stdout)

    def test_command_timeout_is_classified_without_exposing_its_log_path(self) -> None:
        result = self.run_script(
            "printf '%s\\n' 'command timed out after 600s; see /private/export.log'; exit 1"
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn(b"category=timeout", result.stdout)
        self.assertNotIn(b"/private/export.log", result.stdout)

    def test_ctest_timeout_configuration_does_not_mask_a_test_failure(self) -> None:
        result = self.run_script(
            "printf '%s\\n' 'Test timeout computed to be: 1200' "
            "'The following tests FAILED:' "
            "'  7 - gdpp.runtime.contract (Failed)'; exit 8"
        )
        self.assertEqual(result.returncode, 8)
        self.assertIn(b"category=test", result.stdout)
        self.assertNotIn(b"category=timeout", result.stdout)

    def test_actual_ctest_timeout_retains_its_category(self) -> None:
        result = self.run_script(
            "printf '%s\\n' '7/7 Test #7: gdpp.runtime.contract ***Timeout 1200s' "
            "'  7 - gdpp.runtime.contract (Timeout)'; exit 8"
        )
        self.assertEqual(result.returncode, 8)
        self.assertIn(b"category=timeout", result.stdout)

    def test_semantic_failures_expose_only_fixed_diagnostic_codes(self) -> None:
        result = self.run_script(
            "printf '%s\\n' '/private/plugin/source.gd:12: error[GDS4046]: secret' "
            "'ERROR: GDPP AOT: /private/source.gd:6: GDS4075: secret' "
            "'The following tests FAILED:'; exit 8"
        )
        self.assertEqual(result.returncode, 8)
        self.assertIn(b"codes=GDS4046,GDS4075", result.stdout)
        for private in (b"source.gd", b"secret", b"plugin"):
            self.assertNotIn(private, result.stdout)

    def test_package_failure_allows_only_a_packaged_relative_binary_path(self) -> None:
        result = self.run_script(
            "printf '%s\\n' "
            "'binary path audit: checkout path in sdk/lib/linux/x86_64/runtime.a' "
            "'binary path audit: checkout path in ../../private/source.cpp'; exit 1"
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn(b"category=package", result.stdout)
        self.assertIn(b"paths=sdk/lib/linux/x86_64/runtime.a", result.stdout)
        self.assertNotIn(b"source.cpp", result.stdout)

    def test_download_failure_exposes_only_the_last_valid_status_fields(self) -> None:
        result = self.run_script(
            "printf '%s\\n' "
            "'https://private.example/asset?signature=secret' "
            "'GDPP_DOWNLOAD_FAILED artifact=editor http=503 curl=22' "
            "'GDPP_DOWNLOAD_FAILED artifact=templates http=404 curl=22' "
            "'GDPP_DOWNLOAD_FAILED artifact=/private/source http=404 curl=22' "
            "'GDPP_DOWNLOAD_FAILED artifact=other http=secret curl=22'; exit 22"
        )
        self.assertEqual(result.returncode, 22)
        self.assertEqual(result.stderr, b"")
        self.assertIn(b"category=download", result.stdout)
        self.assertIn(b"artifact=templates http=404 curl=22", result.stdout)
        self.assertNotIn(b"artifact=editor", result.stdout)
        self.assertNotIn(b"artifact=other", result.stdout)
        self.assertNotIn(b"secret", result.stdout)
        self.assertNotIn(b"private", result.stdout.replace(b"private-stage", b""))


if __name__ == "__main__":
    unittest.main()
