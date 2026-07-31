#!/usr/bin/env python3
"""Behavior tests for the public artifact source-disclosure gate."""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path


TOOL = Path(__file__).with_name("audit_public_artifact.py")


class ArtifactAuditTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="gdpp-artifact-audit-")
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.evidence = self.root / "evidence"
        (self.source / "src").mkdir(parents=True)
        (self.source / "test/compatibility").mkdir(parents=True)
        self.evidence.mkdir()
        (self.source / "src/private.cpp").write_text(
            "const auto proprietary_runtime_contract = "
            '"private compiler implementation marker";\n',
            encoding="utf-8",
        )
        (self.source / "test/compatibility/public.json").write_text(
            '{"url":"https://github.com/godotengine/godot.git"}\n',
            encoding="utf-8",
        )
        subprocess.run(["git", "init", "-q", str(self.source)], check=True)
        subprocess.run(["git", "-C", str(self.source), "add", "."], check=True)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def audit(self, path: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                "python3",
                str(TOOL),
                "--source",
                str(self.source),
                "--path",
                str(path),
            ],
            text=True,
            capture_output=True,
        )

    def test_public_metadata_does_not_match_non_source_manifests(self) -> None:
        report = self.evidence / "report.json"
        report.write_text(
            '{"url":"https://github.com/godotengine/godot.git"}\n',
            encoding="utf-8",
        )
        self.assertEqual(self.audit(report).returncode, 0)

    def test_exact_private_source_line_is_rejected(self) -> None:
        log = self.evidence / "compiler.log"
        log.write_text(
            "const auto proprietary_runtime_contract = "
            '"private compiler implementation marker";\n',
            encoding="utf-8",
        )
        result = self.audit(log)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("exact private source line", result.stderr)

    def test_source_file_payload_is_rejected(self) -> None:
        generated = self.evidence / "generated.cpp"
        generated.write_text("int generated = 1;\n", encoding="utf-8")
        result = self.audit(generated)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("private-source file type is forbidden", result.stderr)


if __name__ == "__main__":
    unittest.main()
