#!/usr/bin/env python3
"""Behavior tests for portable process-log contracts."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path


TOOL = Path(__file__).with_name("check_log_contract.py")
SPEC = importlib.util.spec_from_file_location("gdpp_check_log_contract", TOOL)
assert SPEC is not None and SPEC.loader is not None
CHECK_LOG = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECK_LOG)


class LogContractTest(unittest.TestCase):
    def test_exact_lines_accept_lf_crlf_and_final_line_without_newline(self) -> None:
        for payload in (
            b"prefix\nGDPP_RUNTIME_FAILURE_OK\n",
            b"prefix\r\nGDPP_RUNTIME_FAILURE_OK\r\n",
            b"prefix\rGDPP_RUNTIME_FAILURE_OK",
        ):
            with self.subTest(payload=payload):
                with tempfile.TemporaryDirectory(prefix="gdpp-log-contract-") as root:
                    log = Path(root) / "runtime.log"
                    log.write_bytes(payload)
                    self.assertEqual(
                        CHECK_LOG.validate(
                            log, ["GDPP_RUNTIME_FAILURE_OK"], ["prefix"]
                        ),
                        [],
                    )

    def test_missing_contracts_are_reported_independently(self) -> None:
        with tempfile.TemporaryDirectory(prefix="gdpp-log-contract-") as root:
            log = Path(root) / "runtime.log"
            log.write_text("GDPP_OTHER_MARKER\n", encoding="utf-8")
            failures = CHECK_LOG.validate(
                log, ["GDPP_RUNTIME_FAILURE_OK"], ["GDPP_AOT_SUMMARY"]
            )
            self.assertEqual(len(failures), 2)
            self.assertTrue(all(str(log) in failure for failure in failures))

    def test_missing_log_is_an_actionable_failure(self) -> None:
        log = Path("missing-runtime.log")
        failures = CHECK_LOG.validate(log, ["GDPP_RUNTIME_FAILURE_OK"], [])
        self.assertEqual(len(failures), 1)
        self.assertIn("cannot read", failures[0])


if __name__ == "__main__":
    unittest.main()
