#!/usr/bin/env python3
"""Behavior tests for the public process evidence runner."""

from __future__ import annotations

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


TOOL = Path(__file__).with_name("run_process.py")
SPEC = importlib.util.spec_from_file_location("gdpp_run_process", TOOL)
assert SPEC is not None and SPEC.loader is not None
RUN_PROCESS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUN_PROCESS)


class ProcessRunnerTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="gdpp-process-runner-")
        self.root = Path(self.temporary.name)
        self.log = self.root / "process.log"

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def run_tool(self, child: str) -> subprocess.CompletedProcess[bytes]:
        return subprocess.run(
            [
                sys.executable,
                str(TOOL),
                "--label",
                "test-child",
                "--log",
                str(self.log),
                "--",
                sys.executable,
                "-c",
                child,
            ],
            capture_output=True,
        )

    def run_tool_with_timeout(
        self, child: str, timeout_seconds: str
    ) -> subprocess.CompletedProcess[bytes]:
        return subprocess.run(
            [
                sys.executable,
                str(TOOL),
                "--label",
                "test-child",
                "--log",
                str(self.log),
                "--timeout-seconds",
                timeout_seconds,
                "--",
                sys.executable,
                "-c",
                child,
            ],
            capture_output=True,
        )

    def test_success_preserves_binary_output(self) -> None:
        result = self.run_tool("import sys; sys.stdout.buffer.write(b'payload\\x00\\xff')")
        self.assertEqual(result.returncode, 0)
        self.assertEqual(self.log.read_bytes(), b"payload\x00\xff")
        self.assertIn(b"payload\x00\xff", result.stdout)
        self.assertIn(b"unsigned_hex=0x00000000", result.stdout)

    def test_failure_reports_full_status_and_returns_gate_failure(self) -> None:
        result = self.run_tool("raise SystemExit(7)")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(self.log.read_bytes(), b"")
        self.assertIn(b"decimal=7 unsigned_hex=0x00000007", result.stdout)

    def test_timeout_preserves_output_and_terminates_the_child(self) -> None:
        result = self.run_tool_with_timeout(
            "import sys,time; print('before-timeout', flush=True); time.sleep(30)",
            "0.1",
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn(b"before-timeout", self.log.read_bytes())
        self.assertIn(b"timeout_seconds=0.1", result.stdout)

    def test_windows_application_error_is_reduced_to_safe_fields(self) -> None:
        payload = b"""\
<Events>
  <Event xmlns="http://schemas.microsoft.com/win/2004/08/events/event">
    <EventData>
      <Data Name="AppName">Godot_v4.7.1-stable_win64.exe</Data>
      <Data Name="AppPath">D:\\private\\customer\\Godot.exe</Data>
      <Data Name="ModuleName">gdpp_compiler.windows.x86_64.dll</Data>
      <Data Name="ModulePath">D:\\private\\gdpp_compiler.dll</Data>
      <Data Name="ExceptionCode">c0000409</Data>
      <Data Name="FaultingOffset">00000000000abcde</Data>
    </EventData>
  </Event>
</Events>
"""
        records = RUN_PROCESS.parse_windows_application_errors(
            payload, "Godot_v4.7.1-stable_win64.exe"
        )
        self.assertEqual(
            records,
            [
                {
                    "app": "Godot_v4.7.1-stable_win64.exe",
                    "module": "gdpp_compiler.windows.x86_64.dll",
                    "exception": "c0000409",
                    "offset": "00000000000abcde",
                }
            ],
        )
        self.assertNotIn("private", repr(records))

    def test_windows_dump_output_is_reduced_to_safe_fields(self) -> None:
        output = """\
PROCESS_NAME:  Godot_v4.7.1-stable_win64.exe
PRIVATE_PATH: D:\\private\\customer\\source.gd
EXCEPTION_CODE: (NTSTATUS) 0xc0000409
STACK_TEXT:
000000ab`1234f000 00007ff9`11112222 : 00000000`00000000 : gdpp_compiler+0x1234
000000ab`1234f080 00007ff9`33334444 : 00000000`00000000 : Godot+0x5678

MODULE_NAME: gdpp_compiler
IMAGE_NAME: gdpp_compiler.windows.x86_64.dll
FAILURE_BUCKET_ID: FAIL_FAST_FATAL_APP_EXIT_c0000409
GDPP_RAW_STACK_BEGIN
000000ab`1234f000 00007ff9`11112222 gdpp_compiler.windows.x86_64+0x1234
000000ab`1234f008 00007ff9`33334444 private_customer_module+0x5678
000000ab`1234f010 00007ff9`55556666 ~gdpp_compiler.windows.x86_64+0x9abc
GDPP_RAW_STACK_END
"""
        evidence = RUN_PROCESS.safe_windows_dump_evidence(output)
        self.assertIn("PROCESS_NAME:  Godot_v4.7.1-stable_win64.exe", evidence)
        self.assertIn("MODULE_NAME: gdpp_compiler", evidence)
        self.assertTrue(any("gdpp_compiler+0x1234" in line for line in evidence))
        self.assertIn("RAW_STACK_POINTERS:", evidence)
        self.assertTrue(
            any(
                "gdpp_compiler.windows.x86_64+0x1234" in line
                for line in evidence
            )
        )
        self.assertTrue(
            any(
                "~gdpp_compiler.windows.x86_64+0x9abc" in line
                for line in evidence
            )
        )
        self.assertFalse(
            any("private_customer_module" in line for line in evidence)
        )
        self.assertNotIn("private", "\n".join(evidence).lower())

    def test_procdump_launch_preserves_the_exact_child_argument_vector(self) -> None:
        command = [
            "D:\\tools\\Godot.exe",
            "--path",
            "D:\\客户项目-é",
            "--export-release",
            "Windows x86_64",
        ]
        launch = RUN_PROCESS.procdump_launch_command(
            Path("D:\\tools\\procdump64.exe"),
            Path("D:\\private-dumps"),
            command,
        )
        self.assertEqual(launch[-len(command) :], command)
        self.assertEqual(launch[1:6], ["-accepteula", "-mt", "-e", "-x", "D:\\private-dumps"])


if __name__ == "__main__":
    unittest.main()
