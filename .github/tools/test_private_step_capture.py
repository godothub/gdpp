#!/usr/bin/env python3
"""Behavior tests for the private CI output boundary."""

from __future__ import annotations

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
            "'The following tests FAILED:' "
            "'  7 - gdpp.runtime.contract (Failed)'; exit 7"
        )
        self.assertEqual(result.returncode, 7)
        self.assertEqual(result.stderr, b"")
        self.assertIn(b"status=failed", result.stdout)
        self.assertIn(b"category=compile", result.stdout)
        self.assertIn(b"exit=7", result.stdout)
        self.assertIn(b"tests=gdpp.runtime.contract", result.stdout)
        self.assertNotIn(b"runner/work", result.stdout)
        logs = list(self.root.glob("gdpp-private-step.*.log"))
        self.assertEqual(len(logs), 1)
        self.assertIn(b"secret", logs[0].read_bytes())
        self.assertEqual(stat.S_IMODE(logs[0].stat().st_mode), 0o600)

    def test_workflow_cleanup_can_chain_the_capture_exit_handler(self) -> None:
        result = self.run_script(
            "trap 'status=$?; printf cleanup-secret; "
            "gdpp_private_capture_exit \"$status\"' EXIT; exit 9"
        )
        self.assertEqual(result.returncode, 9)
        self.assertIn(b"status=failed", result.stdout)
        self.assertNotIn(b"cleanup-secret", result.stdout)

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


if __name__ == "__main__":
    unittest.main()
