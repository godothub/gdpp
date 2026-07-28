#!/usr/bin/env python3
"""Exercise pinned and moving-branch compatibility corpus checkouts."""

from __future__ import annotations

import copy
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
    path: Path,
    repository_url: str,
    revision: dict[str, str],
    checkout: str = "sparse",
) -> None:
    repository = {
        "name": "test/corpus",
        "url": repository_url,
        **revision,
        "license_file": "LICENSE",
        "license": "MIT",
        "required_files": ["project/version.txt"],
    }
    if checkout == "full":
        repository["checkout"] = "full"
    else:
        repository["sparse_paths"] = ["project"]
    path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "repository": repository,
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
    (upstream / "other").mkdir()
    (upstream / "other/complete.txt").write_text(
        "full checkout evidence\n", encoding="utf-8"
    )
    (upstream / "LICENSE").write_text("test license\n", encoding="utf-8")
    (upstream / "project/project.godot").write_text(
        'config_version=5\nconfig/features=PackedStringArray("4.6", "GL Compatibility")\n',
        encoding="utf-8",
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

    full_manifest = test_root / "full.json"
    full_checkout = test_root / "full-checkout"
    write_manifest(
        full_manifest,
        repository_url,
        {"branch": "main"},
        checkout="full",
    )
    fetch(fetch_script, full_manifest, full_checkout, test_root)
    assert_checkout(full_checkout, third_commit, "three\n")
    if not (full_checkout / "other/complete.txt").is_file():
        raise AssertionError("full compatibility checkout omitted repository content")
    sparse_config = subprocess.run(
        ["git", "config", "--bool", "core.sparseCheckout"],
        cwd=full_checkout,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if sparse_config.returncode == 0 and sparse_config.stdout.strip() == "true":
        raise AssertionError("full compatibility checkout retained sparse-checkout mode")

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

    invalid_full_manifest = test_root / "invalid-full.json"
    write_manifest(
        invalid_full_manifest,
        repository_url,
        {"branch": "main"},
        checkout="full",
    )
    invalid_full_data = json.loads(invalid_full_manifest.read_text(encoding="utf-8"))
    invalid_full_data["repository"]["sparse_paths"] = ["project"]
    invalid_full_manifest.write_text(
        json.dumps(invalid_full_data),
        encoding="utf-8",
    )
    invalid_full = subprocess.run(
        [
            sys.executable,
            str(fetch_script),
            "--manifest",
            str(invalid_full_manifest),
            "--destination",
            str(test_root / "invalid-full-checkout"),
            "--build-root",
            str(test_root),
        ],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if (
        invalid_full.returncode == 0
        or "cannot be combined with checkout='full'" not in invalid_full.stderr
    ):
        raise AssertionError("ambiguous full/sparse checkout policy was not rejected")

    sys.path.insert(0, str(source_root / "tools"))
    try:
        import validate_compatibility_godot
    finally:
        sys.path.pop(0)

    contract_manifest = json.loads(full_manifest.read_text(encoding="utf-8"))
    contract_manifest["godot"] = {"target": "4.6", "engine": "4.6.3"}
    report = validate_compatibility_godot.validate_contract(
        contract_manifest,
        full_checkout,
        "4.6.3",
        "4.6.3.stable.official.test",
    )
    if (
        report["status"] != "passed"
        or report["target_godot"] != "4.6"
        or report["projects"][0]["project_feature"] != "4.6"
    ):
        raise AssertionError("valid Godot compatibility contract was not preserved")
    if (
        validate_compatibility_godot.parse_engine_version(
            "4.6.3.stable.official.test\n"
        )
        != "4.6.3"
    ):
        raise AssertionError("official Godot version output was not parsed exactly")

    wrong_engine = copy.deepcopy(contract_manifest)
    wrong_engine["godot"]["engine"] = "4.6.2"
    try:
        validate_compatibility_godot.validate_contract(
            wrong_engine,
            full_checkout,
            "4.6.3",
            "4.6.3.stable.official.test",
        )
    except RuntimeError as error:
        if "engine mismatch" not in str(error):
            raise
    else:
        raise AssertionError("mismatched Godot executable version was accepted")

    wrong_target = copy.deepcopy(contract_manifest)
    wrong_target["godot"] = {"target": "4.7", "engine": "4.7.1"}
    try:
        validate_compatibility_godot.validate_contract(
            wrong_target,
            full_checkout,
            "4.7.1",
            "4.7.1.stable.official.test",
        )
    except RuntimeError as error:
        if "declares Godot 4.6, expected 4.7" not in str(error):
            raise
    else:
        raise AssertionError("mismatched project.godot feature version was accepted")

    print("compatibility fetch policy passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
