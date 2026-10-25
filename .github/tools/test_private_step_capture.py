#!/usr/bin/env python3
"""Behavior tests for the private CI output boundary."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shlex
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

    def run_script(self, script: str, extra_environment: dict[str, str] | None = None) -> subprocess.CompletedProcess[bytes]:
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
        environment.update(extra_environment or {})
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
        self.assertEqual(list(self.root.glob("gdpp-private-step.*")), [])

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
        logs = list(self.root.glob("gdpp-private-step.*"))
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

    def test_nested_cmake_failures_expose_only_fixed_stage_codes(self) -> None:
        stages = (
            ("Cannot bootstrap the MSVC dependency launcher", "msvc-launcher-bootstrap"),
            ("Cannot execute the MSVC dependency launcher", "msvc-launcher-execute"),
            ("The MSVC host launcher does not implement the dependency encoding protocol",
             "msvc-launcher-protocol"),
            ("Cannot prove the MSVC Unicode header dependency contract", "msvc-unicode-probe"),
            ("Cannot load the selected MSVC native host environment", "msvc-host-environment"),
            ("MSVC native host environment is incomplete", "msvc-host-environment"),
            ("Cannot locate the selected MSVC installation", "msvc-host-compiler"),
            ("The selected MSVC installation has no native host environment", "msvc-host-compiler"),
            ("GDPP_MSVC_DEPENDENCY_HOST_LAUNCHER must be an existing absolute executable",
             "msvc-launcher-override"),
            ("The C++ compiler is not able to compile a simple test program", "cmake-compiler-probe"),
            ("The CXX compiler identification is unknown", "cmake-compiler-identification"),
            ("Could not find compiler set in environment variable CXX", "cmake-compiler-discovery"),
            ("Could not find CMAKE_ROOT", "cmake-installation"),
            ("Could not find any instance of Visual Studio", "cmake-msvc-installation"),
            ("Failed to run MSBuild command", "cmake-msbuild-probe"),
            ("CMAKE_MAKE_PROGRAM is not set", "cmake-build-tool"),
            ("Invalid character escape '\\U'", "cmake-path-escape"),
            ("The source directory does not appear to contain CMakeLists.txt", "cmake-source-directory"),
        )
        for diagnostic, code in stages:
            with self.subTest(code=code, diagnostic=diagnostic):
                lines = ["CMake Error at /private/customer/CMakeLists.txt:42:",
                         diagnostic + "; see C:\\private\\customer\\probe.stderr.bin",
                         "private source line, private assertion, private token"]
                result = self.run_script("printf '%s\\n' " + shlex.join(lines) + "; exit 8")
                self.assertEqual(result.returncode, 8)
                self.assertEqual(result.stderr, b"")
                self.assertEqual(result.stdout,
                    ("private-stage job=compiler-core step=__run_17 status=failed "
                     "category=configure exit=8 codes=" + code + "\n").encode())
                raw_logs = list(self.root.glob("gdpp-private-step.*"))
                self.assertTrue(any(lines[-1].encode() in log.read_bytes() for log in raw_logs))
        self.assertEqual(len(raw_logs), len(stages))
        self.assertTrue(all(stat.S_IMODE(log.stat().st_mode) == 0o600 for log in raw_logs))

    def test_unknown_configure_diagnostics_are_not_copied_to_the_summary(self) -> None:
        result = self.run_script(
            "printf '%s\\n' 'CMake Error: private customer diagnostic' "
            "'/private/customer/source.cpp: private source line'; exit 8"
        )
        self.assertEqual(result.returncode, 8)
        self.assertEqual(result.stdout,
            b"private-stage job=compiler-core step=__run_17 status=failed category=configure exit=8\n")
        self.assertEqual(result.stderr, b"")

    def test_success_never_exposes_a_configure_stage_or_its_raw_diagnostics(self) -> None:
        result = self.run_script(
            "printf '%s\\n' 'Cannot bootstrap the MSVC dependency launcher: private detail'"
        )
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout,
            b"private-stage job=compiler-core step=__run_17 status=success\n")
        self.assertEqual(result.stderr, b"")
        self.assertEqual(list(self.root.glob("gdpp-private-step.*")), [])

    def test_capture_setup_fails_closed_when_the_temp_directory_is_missing(self) -> None:
        result = self.run_script("printf private-command-must-not-run",
                                 {"RUNNER_TEMP": str(self.root / "missing")})
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, b"")
        self.assertEqual(result.stderr,
            b"private-stage status=failed category=capture-initialization exit=1\n")

    def test_capture_setup_fails_closed_when_permissions_cannot_be_set(self) -> None:
        bin_root = self.root / "bin"
        bin_root.mkdir()
        chmod = bin_root / "chmod"
        chmod.write_text("#!/bin/sh\nexit 1\n", encoding="utf-8")
        chmod.chmod(0o755)
        result = self.run_script("printf private-command-must-not-run",
                                 {"PATH": str(bin_root) + os.pathsep + os.environ["PATH"]})
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, b"")
        self.assertEqual(result.stderr,
            b"private-stage status=failed category=capture-initialization exit=1\n")

    def test_editor_workflow_cases_hash_only_the_reason_before_private_details(self) -> None:
        reason = "Native diagnostic module never connected"
        expected = hashlib.sha256(reason.encode()).hexdigest()[:16].encode()
        for detail in ("private checkout/source.gd", "private-token-and-customer-value"):
            with self.subTest(detail=detail):
                payload = "ERROR: GDPP_EDITOR_WORKFLOW_FAILED: " + reason + ": " + detail
                result = self.run_script("printf '%s\\n' " + shlex.quote(payload) + "; exit 8")
                self.assertEqual(result.returncode, 8)
                self.assertEqual(result.stderr, b"")
                self.assertIn(b"cases=" + expected, result.stdout)
                self.assertNotIn(reason.encode(), result.stdout)
                self.assertNotIn(detail.encode(), result.stdout)

    def test_editor_workflow_case_hashes_are_bounded_and_deduplicated(self) -> None:
        reasons = ["case-" + str(index) for index in range(20)]
        lines = ["ERROR: GDPP_EDITOR_WORKFLOW_FAILED: " + reason + ": private details"
                 for reason in reasons + reasons]
        result = self.run_script("printf '%s\\n' " + shlex.join(lines) + "; exit 8")
        wanted = sorted({hashlib.sha256(reason.encode()).hexdigest()[:16]
                         for reason in reasons})[:12]
        self.assertEqual(result.returncode, 8)
        self.assertIn(("cases=" + ",".join(wanted)).encode(), result.stdout)
        self.assertNotIn(b"private details", result.stdout)
        self.assertNotIn(b"case-", result.stdout)

    def test_editor_workflow_success_and_malformed_messages_do_not_publish_case_hashes(self) -> None:
        for payload, status in (
            ("GDPP_EDITOR_WORKFLOW_FAILED: private reason: private details", 0),
            ("GDPP_EDITOR_WORKFLOW_FAILED: : private details", 8),
            ("GDPP_EDITOR_WORKFLOW_FAILED: " + "x" * 513 + ": private details", 8),
        ):
            with self.subTest(status=status, length=len(payload)):
                result = self.run_script("printf '%s\\n' " + shlex.quote(payload) +
                                         "; exit " + str(status))
                self.assertEqual(result.returncode, status)
                self.assertNotIn(b"cases=", result.stdout)
                self.assertNotIn(b"private", result.stdout.replace(b"private-stage", b""))

    def test_runtime_timeout_identifies_only_the_last_safe_phase(self) -> None:
        lines = [
            "GDPP_OFFICIAL_RUNTIME_PHASE stage=import case=none state=done",
            "53: GDPP_OFFICIAL_RUNTIME_PHASE stage=source case=0123456789abcdef state=start",
            "private source/feature.gd private token",
            "command timed out: /private/checkout/project",
        ]
        result = self.run_script("printf '%s\\n' " + shlex.join(lines) + "; exit 8")
        self.assertEqual(result.returncode, 8)
        self.assertIn(b"category=timeout", result.stdout)
        self.assertIn(b"runtime=source:0123456789abcdef:start", result.stdout)
        self.assertNotIn(b"runtime=import", result.stdout)
        for detail in (b"feature.gd", b"token", b"checkout", b"GDPP_OFFICIAL_RUNTIME_PHASE"):
            self.assertNotIn(detail, result.stdout)

    def test_runtime_phase_rejects_private_paths_unknown_stages_and_unbounded_suffixes(self) -> None:
        for line in (
            "GDPP_OFFICIAL_RUNTIME_PHASE stage=private-source case=none state=start",
            "GDPP_OFFICIAL_RUNTIME_PHASE stage=source case=/private/source.gd state=start",
            "GDPP_OFFICIAL_RUNTIME_PHASE stage=aot case=ABCDEF0123456789 state=start",
            "GDPP_OFFICIAL_RUNTIME_PHASE stage=export case=none state=private-state",
            "GDPP_OFFICIAL_RUNTIME_PHASE stage=export case=none state=start private-token",
            "private: GDPP_OFFICIAL_RUNTIME_PHASE stage=export case=none state=start",
        ):
            with self.subTest(line=line):
                result = self.run_script("printf '%s\\n' " + shlex.quote(line) + "; exit 8")
                self.assertEqual(result.returncode, 8)
                self.assertNotIn(b"runtime=", result.stdout)
                self.assertNotIn(b"source.gd", result.stdout)
                self.assertNotIn(b"private-token", result.stdout)

    def test_cold_import_failure_reports_only_last_fixed_phase(self) -> None:
        lines = (
            "GDPP_ADDON_COLD_IMPORT_PHASE phase=export state=done",
            "24: GDPP_ADDON_COLD_IMPORT_PHASE phase=cold-import state=start",
            "/private/customer/source.gd token-secret",
        )
        script = "printf '%s\\n' " + " ".join(shlex.quote(line) for line in lines) + "; exit 8"
        result = self.run_script(script)
        self.assertEqual(result.returncode, 8)
        self.assertIn(b"cold_import=cold-import:start", result.stdout)
        self.assertNotIn(b"cold_import=export", result.stdout)
        for private in (b"source.gd", b"token-secret", b"customer", b"GDPP_ADDON_COLD_IMPORT_PHASE"):
            self.assertNotIn(private, result.stdout)

    def test_cold_import_phase_rejects_unbounded_fields_and_stays_hidden_on_success(self) -> None:
        lines = (
            "GDPP_ADDON_COLD_IMPORT_PHASE phase=/private/source state=start",
            "GDPP_ADDON_COLD_IMPORT_PHASE phase=runtime state=secret",
            "GDPP_ADDON_COLD_IMPORT_PHASE phase=editor state=start token-secret",
            "private: GDPP_ADDON_COLD_IMPORT_PHASE phase=export state=start",
        )
        for line in lines:
            with self.subTest(line=line):
                result = self.run_script("printf '%s\\n' " + shlex.quote(line) + "; exit 8")
                self.assertNotIn(b"cold_import=", result.stdout)
                self.assertNotIn(b"token-secret", result.stdout)
        result = self.run_script(
            "printf '%s\\n' 'GDPP_ADDON_COLD_IMPORT_PHASE phase=runtime state=done'"
        )
        self.assertEqual(result.returncode, 0)
        self.assertNotIn(b"cold_import=", result.stdout)

    def editor_progress_record(self, **changes) -> dict:
        return {"schema": 1, "phase": "cancel_positive", "reason": "cancel_baseline",
                "last_phase": "finish", "returncode": 1, "script_errors": 0,
                "engine_errors": 0, "records": 6, **changes}

    def test_editor_progress_failure_exposes_only_closed_schema_fields(self) -> None:
        marker = "GDPP_EDITOR_PROGRESS_DIAGNOSTIC=" + json.dumps(self.editor_progress_record())
        for prefix in ("", "127: "):
            with self.subTest(prefix=prefix):
                lines = [prefix + marker, "AssertionError: /private/customer/source.gd secret-token",
                         "The following tests FAILED:", "  7 - gdpp.godot.editor_progress_input (Failed)"]
                result = self.run_script("printf '%s\\n' " + shlex.join(lines) + "; exit 8")
                self.assertEqual(result.returncode, 8)
                self.assertIn(b"tests=gdpp.godot.editor_progress_input", result.stdout)
                self.assertIn(b"gui=cancel_positive:cancel_baseline gui_last=finish gui_rc=1 "
                              b"gui_script_errors=0 gui_engine_errors=0 gui_records=6", result.stdout)
                for private in (b"customer", b"source.gd", b"secret-token", b"DIAGNOSTIC", b"AssertionError"):
                    self.assertNotIn(private, result.stdout)
                self.assertTrue(any(b"secret-token" in log.read_bytes()
                                    for log in self.root.glob("gdpp-private-step.*")))

    def test_editor_progress_success_never_publishes_fixture_metadata(self) -> None:
        line = "GDPP_EDITOR_PROGRESS_DIAGNOSTIC=" + json.dumps(self.editor_progress_record())
        result = self.run_script("printf '%s\\n' " + shlex.quote(line))
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, b"private-stage job=compiler-core step=__run_17 status=success\n")

    def test_editor_progress_rejects_unknown_fields_enums_types_and_numeric_overflow(self) -> None:
        variants = [
            self.editor_progress_record(path="/private/customer/source.gd secret-token"),
            self.editor_progress_record(phase="/private/customer/source.gd secret-token"),
            self.editor_progress_record(reason="customer-secret-reason"),
            self.editor_progress_record(last_phase="customer-secret-phase"),
            self.editor_progress_record(schema=True), self.editor_progress_record(schema=2),
            self.editor_progress_record(returncode=True), self.editor_progress_record(returncode="secret-token"),
            self.editor_progress_record(returncode=2 ** 32), self.editor_progress_record(returncode=-(2 ** 31) - 1),
            self.editor_progress_record(script_errors=-1), self.editor_progress_record(engine_errors=65536),
            self.editor_progress_record(records=65), self.editor_progress_record(records=1.5),
            self.editor_progress_record(records={"secret-token": 1}), ["secret-token"],
        ]
        for value in variants:
            with self.subTest(value=value):
                line = "GDPP_EDITOR_PROGRESS_DIAGNOSTIC=" + json.dumps(value)
                result = self.run_script("printf '%s\\n' " + shlex.quote(line) + "; exit 1")
                self.assertEqual(result.returncode, 1)
                self.assertIn(b"gui=invalid", result.stdout)
                for forbidden in (b"gui_last=", b"gui_rc=", b"customer", b"secret-token", b"source.gd"):
                    self.assertNotIn(forbidden, result.stdout)

    def test_editor_progress_rejects_duplicate_records_keys_and_unbounded_json(self) -> None:
        valid = "GDPP_EDITOR_PROGRESS_DIAGNOSTIC=" + json.dumps(self.editor_progress_record())
        malformed = [
            [valid, valid],
            [valid.replace('"schema": 1', '"schema": 1, "schema": 1')],
            [valid + " secret-token"],
            ["GDPP_EDITOR_PROGRESS_DIAGNOSTIC=" + "x" * 1025],
            ["GDPP_EDITOR_PROGRESS_DIAGNOSTIC={\"schema\":NaN,\"secret\":\"secret-token\"}"],
        ]
        for lines in malformed:
            with self.subTest(lines=lines):
                result = self.run_script("printf '%s\\n' " + shlex.join(lines) + "; exit 1")
                self.assertIn(b"gui=invalid", result.stdout)
                self.assertNotIn(b"gui_last=", result.stdout)
                self.assertNotIn(b"secret-token", result.stdout)

    def test_editor_progress_ignores_markers_embedded_in_private_source_text(self) -> None:
        valid = "GDPP_EDITOR_PROGRESS_DIAGNOSTIC=" + json.dumps(self.editor_progress_record())
        result = self.run_script("printf '%s\\n' " + shlex.quote("source.gd: secret-token " + valid) + "; exit 1")
        self.assertNotIn(b"gui=", result.stdout)
        self.assertNotIn(b"secret-token", result.stdout)
        self.assertNotIn(b"source.gd", result.stdout)

    def test_runtime_success_never_publishes_phase_or_case_metadata(self) -> None:
        result = self.run_script(
            "printf '%s\\n' 'GDPP_OFFICIAL_RUNTIME_PHASE stage=aot "
            "case=0123456789abcdef state=done'"
        )
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout,
            b"private-stage job=compiler-core step=__run_17 status=success\n")

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
