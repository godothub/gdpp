#!/usr/bin/env python3
"""Exercise pinned and moving-branch compatibility corpus checkouts."""

from __future__ import annotations

import json
import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path


def run(command: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    )


def write_manifest(
    path: Path, repository_url: str, revision: dict[str, str]
) -> None:
    path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "repository": {
                    "name": "test/corpus",
                    "url": repository_url,
                    **revision,
                    "license_file": "LICENSE",
                    "license": "MIT",
                    "sparse_paths": ["project"],
                    "required_files": ["project/version.txt"],
                },
                "projects": [
                    {
                        "path": "project",
                        "minimum_isolated_successes": 0,
                    }
                ],
            }
        ),
        encoding="utf-8",
    )


def commit(upstream: Path, value: str) -> str:
    (upstream / "project/version.txt").write_text(value, encoding="utf-8")
    run(["git", "add", "."], upstream)
    run(["git", "commit", "--quiet", "-m", f"version {value}"], upstream)
    return run(["git", "rev-parse", "HEAD"], upstream).stdout.strip()


def fetch(
    script: Path, manifest: Path, destination: Path, build_root: Path
) -> subprocess.CompletedProcess[str]:
    return run(
        [
            sys.executable,
            str(script),
            "--manifest",
            str(manifest),
            "--destination",
            str(destination),
            "--build-root",
            str(build_root),
        ]
    )


def assert_checkout(destination: Path, commit_id: str, value: str) -> None:
    actual_commit = run(["git", "rev-parse", "HEAD"], destination).stdout.strip()
    if actual_commit != commit_id:
        raise AssertionError(f"expected checkout {commit_id}, got {actual_commit}")
    actual_value = (destination / "project/version.txt").read_text(encoding="utf-8")
    if actual_value != value:
        raise AssertionError(f"expected corpus value {value!r}, got {actual_value!r}")


def remove_tree(path: Path) -> None:
    def make_writable_and_retry(function, entry, error_info) -> None:
        error = error_info[1]
        if not isinstance(error, PermissionError):
            raise error
        os.chmod(entry, stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR)
        function(entry)

    shutil.rmtree(path, onerror=make_writable_and_retry)


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: compatibility_fetch_test.py <source-root> <test-root>")

    source_root = Path(sys.argv[1]).resolve()
    test_root = Path(sys.argv[2]).resolve()
    if test_root.exists():
        remove_tree(test_root)
    test_root.mkdir(parents=True)

    upstream = test_root / "upstream"
    upstream.mkdir()
    run(["git", "init", "--quiet", "--initial-branch=main"], upstream)
    run(["git", "config", "user.name", "GDPP Test"], upstream)
    run(["git", "config", "user.email", "gdpp-test@example.invalid"], upstream)
    (upstream / "project").mkdir()
    (upstream / "LICENSE").write_text("test license\n", encoding="utf-8")
    (upstream / "project/project.godot").write_text(
        "config_version=5\n", encoding="utf-8"
    )
    first_commit = commit(upstream, "one\n")
    repository_url = upstream.as_uri()

    fetch_script = source_root / "tools/fetch_compatibility_corpus.py"
    branch_manifest = test_root / "branch.json"
    branch_checkout = test_root / "branch-checkout"
    write_manifest(branch_manifest, repository_url, {"branch": "main"})

    fetch(fetch_script, branch_manifest, branch_checkout, test_root)
    assert_checkout(branch_checkout, first_commit, "one\n")

    second_commit = commit(upstream, "two\n")
    fetch(fetch_script, branch_manifest, branch_checkout, test_root)
    assert_checkout(branch_checkout, second_commit, "two\n")

    (branch_checkout / "project/version.txt").write_text(
        "dirty checkout\n", encoding="utf-8"
    )
    (branch_checkout / "untracked.txt").write_text(
        "stale compiler output\n", encoding="utf-8"
    )
    third_commit = commit(upstream, "three\n")
    fetch(fetch_script, branch_manifest, branch_checkout, test_root)
    assert_checkout(branch_checkout, third_commit, "three\n")
    if (branch_checkout / "untracked.txt").exists():
        raise AssertionError("untracked compatibility output was not removed")

    run(
        ["git", "remote", "set-url", "origin", (test_root / "wrong-remote").as_uri()],
        branch_checkout,
    )
    fetch(fetch_script, branch_manifest, branch_checkout, test_root)
    assert_checkout(branch_checkout, third_commit, "three\n")

    pinned_manifest = test_root / "pinned.json"
    pinned_checkout = test_root / "pinned-checkout"
    write_manifest(pinned_manifest, repository_url, {"commit": first_commit})
    fetch(fetch_script, pinned_manifest, pinned_checkout, test_root)
    assert_checkout(pinned_checkout, first_commit, "one\n")

    (pinned_checkout / "project/version.txt").write_text(
        "dirty pinned checkout\n", encoding="utf-8"
    )
    fetch(fetch_script, pinned_manifest, pinned_checkout, test_root)
    assert_checkout(pinned_checkout, first_commit, "one\n")

    commit(upstream, "four\n")
    fetch(fetch_script, pinned_manifest, pinned_checkout, test_root)
    assert_checkout(pinned_checkout, first_commit, "one\n")

    invalid_manifest = test_root / "invalid.json"
    write_manifest(
        invalid_manifest,
        repository_url,
        {"branch": "main", "commit": first_commit},
    )
    invalid = subprocess.run(
        [
            sys.executable,
            str(fetch_script),
            "--manifest",
            str(invalid_manifest),
            "--destination",
            str(test_root / "invalid-checkout"),
            "--build-root",
            str(test_root),
        ],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if invalid.returncode == 0 or "exactly one of commit or branch" not in invalid.stderr:
        raise AssertionError("ambiguous compatibility revision was not rejected")

    print("compatibility fetch policy passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
