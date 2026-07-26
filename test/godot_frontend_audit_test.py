#!/usr/bin/env python3
"""Offline contract tests for Godot frontend snapshot/update tooling."""

from __future__ import annotations

import hashlib
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SOURCE_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SOURCE_ROOT / "tools"))

import audit_godot_frontend as audit  # noqa: E402
import update_godot_frontend as update  # noqa: E402


def write(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8", newline="\n")


class GodotFrontendAuditTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.checkout = self.root / "godot"
        self.checkout.mkdir()
        write(
            self.checkout
            / "modules/gdscript/tests/scripts/parser/features/valid.gd",
            "var value := 1\n",
        )
        write(
            self.checkout
            / "modules/gdscript/tests/scripts/parser/warnings/warning.gd",
            "var warning := 2\n",
        )
        write(
            self.checkout
            / "modules/gdscript/tests/scripts/parser/errors/invalid.gd",
            "var value :=\n",
        )
        write(
            self.checkout / audit.WARNING_SOURCE,
            'static const char *names[] = {\n'
            '    PNAME("FIRST_WARNING"),\n'
            '    "DEPRECATED_WARNING",\n'
            "};\n",
        )
        write(
            self.checkout / audit.ANNOTATION_SOURCE,
            'register_annotation(MethodInfo("@tool"), target, action);\n'
            'register_annotation(MethodInfo("@export"), target, action);\n',
        )
        write(
            self.checkout / audit.UNICODE_SOURCE,
            "const int xid_start_size = 1;\n"
            "const CharRange xid_start[xid_start_size] = {\n"
            "    { 0x41, 0x5a },\n"
            "};\n"
            "const int xid_continue_size = 2;\n"
            "const CharRange xid_continue[xid_continue_size] = {\n"
            "    { 0x30, 0x39 },\n"
            "    { 0x41, 0x5a },\n"
            "};\n",
        )
        subprocess.run(["git", "init", "--quiet"], cwd=self.checkout, check=True)
        subprocess.run(
            ["git", "config", "user.email", "test@example.invalid"],
            cwd=self.checkout,
            check=True,
        )
        subprocess.run(
            ["git", "config", "user.name", "GDPP Test"],
            cwd=self.checkout,
            check=True,
        )
        subprocess.run(["git", "add", "."], cwd=self.checkout, check=True)
        subprocess.run(
            ["git", "commit", "--quiet", "-m", "fixture"],
            cwd=self.checkout,
            check=True,
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_snapshot_verifies_all_registry_and_range_contracts(self) -> None:
        snapshot = audit.collect_snapshot(self.checkout, "4.7.1-stable")
        self.assertEqual(snapshot["parser_corpus"]["valid"]["count"], 2)
        self.assertEqual(snapshot["parser_corpus"]["invalid"]["count"], 1)
        self.assertEqual(
            snapshot["warnings"], ["deprecated_warning", "first_warning"]
        )
        self.assertEqual(snapshot["annotations"], ["export", "tool"])

        language_features = self.root / "language_features.cpp"
        write(
            language_features,
            'AnnotationFeature{"export", target, behavior, 0, 0},\n'
            'AnnotationFeature{"tool", target, behavior, 0, 0},\n'
            "constexpr auto warning_names = std::array{\n"
            '    std::string_view{"deprecated_warning"},\n'
            '    std::string_view{"first_warning"},\n'
            "};\n",
        )
        unicode_table = self.root / "unicode.inc"
        source_digest = hashlib.sha256(
            (self.checkout / audit.UNICODE_SOURCE).read_bytes()
        ).hexdigest()
        write(
            unicode_table,
            "// Godot source tag: 4.7.1-stable\n"
            f"// Godot char_range.cpp SHA-256: {source_digest}\n"
            "inline constexpr std::array<UnicodeRange, 1> xid_start_ranges{{\n"
            "    UnicodeRange{0x41U, 0x5aU},\n"
            "}};\n"
            "inline constexpr std::array<UnicodeRange, 2> xid_continue_ranges{{\n"
            "    UnicodeRange{0x30U, 0x39U},\n"
            "    UnicodeRange{0x41U, 0x5aU},\n"
            "}};\n",
        )
        self.assertEqual(
            audit.verify_gdpp_registry(
                snapshot, language_features, unicode_table
            ),
            [],
        )

    def test_differential_report_names_changed_parser_files(self) -> None:
        before = audit.collect_contract(self.checkout, "4.7-stable")
        script = (
            self.checkout
            / "modules/gdscript/tests/scripts/parser/features/valid.gd"
        )
        write(script, "var value := 2\n")
        after = audit.collect_contract(self.checkout, "4.7.1-stable")
        difference = audit.diff_snapshots(before, after)

        self.assertEqual(
            difference["parser_corpus"]["valid"]["changed"],
            ["modules/gdscript/tests/scripts/parser/features/valid.gd"],
        )
        self.assertEqual(difference["parser_corpus"]["invalid"]["changed"], [])

    def test_latest_supported_stable_tag_ignores_other_release_lines(self) -> None:
        subprocess.run(
            ["git", "tag", "4.7-stable"], cwd=self.checkout, check=True
        )
        subprocess.run(
            ["git", "tag", "4.7.1-stable"], cwd=self.checkout, check=True
        )
        subprocess.run(
            ["git", "tag", "4.8-stable"], cwd=self.checkout, check=True
        )
        tag, commit = update.latest_stable_tag(str(self.checkout), "4.7")
        self.assertEqual(tag, "4.7.1-stable")
        self.assertEqual(commit, audit.repository_commit(self.checkout))


if __name__ == "__main__":
    unittest.main()
