#!/usr/bin/env python3
"""Behavior tests for the public process evidence runner."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


TOOL = Path(__file__).with_name("run_process.py")


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


if __name__ == "__main__":
    unittest.main()
