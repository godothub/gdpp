#!/usr/bin/env python3
"""Exercise the actual formatting workflow against real Git release histories."""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

import yaml


WORKFLOW = Path(__file__).parents[1] / "workflows/quality.yml"
FORMAT_STEP = next(
    step
    for step in yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))["jobs"]["source"]["steps"]
    if step.get("name") == "Check formatting of all authored C++ sources"
)


@unittest.skipIf(os.name == "nt", "The formatting job runs on Linux Bash")
class QualityFormattingTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="gdpp-quality-format-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.source.mkdir()
        self.git("init", "-q")
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.log = self.root / "formatter.jsonl"
        formatter = self.bin / "clang-format-18"
        formatter.write_text(
            f"#!{sys.executable}\n"
            "import json, os, pathlib, sys\n"
            "with open(os.environ['GDPP_FORMAT_TEST_LOG'], 'a') as log:\n"
            "    log.write(json.dumps(sys.argv[1:]) + '\\n')\n"
            "if sys.argv[1:] == ['--version']:\n"
            "    print('clang-format version ' + os.environ.get('GDPP_FORMAT_TEST_VERSION', '18.1.8'))\n"
            "    sys.exit(0)\n"
            "sys.exit(any('unformatted' in pathlib.Path(name).read_text() for name in sys.argv[3:]))\n",
            encoding="utf-8",
        )
        formatter.chmod(0o755)
        self.runner_temp = self.root / "runner-temp"
        self.runner_temp.mkdir()

    def git(self, *arguments: str) -> str:
        return subprocess.check_output(
            ["git", "-C", str(self.source), *arguments], text=True
        )

    def write(self, relative: str, content: str = "int value;\n") -> None:
        path = self.source / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")

    def commit(self, message: str) -> None:
        self.git("add", ".")
        self.git("-c", "user.name=CI Test", "-c", "user.email=ci@example.invalid",
                 "commit", "-qm", message)

    def run_step(self, version: str = "18.1.8") -> subprocess.CompletedProcess[str]:
        environment = os.environ.copy()
        environment.update({
            "PATH": str(self.bin) + os.pathsep + environment["PATH"],
            "RUNNER_TEMP": str(self.runner_temp),
            "GDPP_FORMAT_TEST_LOG": str(self.log),
            "GDPP_FORMAT_TEST_VERSION": version,
        })
        # Public CI installs the private output boundary through BASH_ENV. Keep
        # this isolated workflow fixture independent of the caller's shell setup.
        environment.pop("BASH_ENV", None)
        return subprocess.run(
            [shutil.which("bash"), "-e", "-o", "pipefail", "-c", FORMAT_STEP["run"]],
            cwd=self.source, env=environment, text=True, capture_output=True,
            timeout=20, check=False,
        )

    def calls(self) -> list[list[str]]:
        if not self.log.exists():
            return []
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def test_late_version_bump_checks_earlier_and_unchanged_sources(self) -> None:
        self.write("CMakeLists.txt", "project(gdpp\n    VERSION 2.0.8\n)\n")
        self.write("include/existing.hpp")
        self.write("cmake/launcher.cpp")
        self.write("include/gdextension_interface.h", "generated API\n")
        self.write("third/vendor.cpp")
        self.commit("previous release")
        self.write("src/earlier.cpp")
        self.write("test/new_contract.h")
        self.write("src/notes.txt", "notes\n")
        self.commit("implementation before version update")
        self.write("CMakeLists.txt", "project(gdpp\n    VERSION 2.1.0\n)\n")
        self.commit("bump version last")
        self.assertEqual(self.git("diff", "--name-only", "HEAD^", "HEAD").strip(),
                         "CMakeLists.txt")
        self.write("src/untracked.cpp")
        result = self.run_step()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls(), [
            ["--version"],
            ["--dry-run", "--Werror", "cmake/launcher.cpp", "include/existing.hpp", "src/earlier.cpp",
             "test/new_contract.h"],
        ])

    def test_filename_boundaries_are_preserved(self) -> None:
        paths = ["include/with space.hpp", "src/中文.cpp", "test/with\nnewline.h"]
        for path in paths:
            self.write(path)
        self.commit("tracked filenames")
        result = self.run_step()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls()[-1], ["--dry-run", "--Werror", *paths])

    def test_bad_format_before_the_version_commit_still_fails(self) -> None:
        self.write("cmake/earlier.cpp", "unformatted\n")
        self.commit("implementation")
        self.write("CMakeLists.txt", "project(gdpp\n    VERSION 2.1.0\n)\n")
        self.commit("bump version last")
        self.assertNotEqual(self.run_step().returncode, 0)
        self.assertEqual(self.calls()[-1], ["--dry-run", "--Werror", "cmake/earlier.cpp"])

    def test_wrong_formatter_version_is_rejected(self) -> None:
        self.write("src/example.cpp")
        self.commit("implementation")
        self.assertNotEqual(self.run_step(version="19.1.0").returncode, 0)
        self.assertEqual(self.calls(), [["--version"]])

    def test_git_inventory_failure_cannot_appear_as_no_files(self) -> None:
        shutil.rmtree(self.source / ".git")
        self.assertNotEqual(self.run_step().returncode, 0)
        self.assertEqual(self.calls(), [])

    def test_no_tracked_authored_sources_skips_the_formatter(self) -> None:
        self.write("README.md", "documentation\n")
        self.commit("documentation")
        result = self.run_step()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls(), [])


if __name__ == "__main__":
    unittest.main()
