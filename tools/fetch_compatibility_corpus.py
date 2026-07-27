#!/usr/bin/env python3
"""Fetch a sparse compatibility corpus from a pinned commit or moving branch."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import stat
import subprocess
from pathlib import Path


def run(command: list[str], cwd: Path | None = None) -> None:
    subprocess.run(command, cwd=cwd, check=True)


def load_manifest(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as stream:
        manifest = json.load(stream)
    if manifest.get("schema_version") != 1:
        raise RuntimeError("unsupported compatibility corpus manifest schema")
    return manifest


def ensure_build_destination(destination: Path, build_root: Path) -> None:
    resolved_destination = destination.resolve()
    resolved_build_root = build_root.resolve()
    if resolved_destination == resolved_build_root or resolved_build_root not in resolved_destination.parents:
        raise RuntimeError(f"corpus destination must be below build root: {resolved_build_root}")


def current_commit(destination: Path) -> str | None:
    if not (destination / ".git").is_dir():
        return None
    result = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=destination,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return result.stdout.strip() if result.returncode == 0 else None


def repository_revision(repository: dict) -> tuple[str, str]:
    commit = repository.get("commit")
    branch = repository.get("branch")
    if bool(commit) == bool(branch):
        raise RuntimeError("repository must define exactly one of commit or branch")
    if commit:
        if not isinstance(commit, str) or not commit:
            raise RuntimeError("repository.commit must be a non-empty string")
        return "commit", commit
    if not isinstance(branch, str) or not branch:
        raise RuntimeError("repository.branch must be a non-empty string")
    check = subprocess.run(
        ["git", "check-ref-format", "--branch", branch],
        text=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        check=False,
    )
    if check.returncode != 0:
        raise RuntimeError(f"invalid repository branch {branch!r}: {check.stderr.strip()}")
    return "branch", f"refs/heads/{branch}"


def sparse_paths(manifest: dict) -> list[str]:
    repository = manifest["repository"]
    # Cone-mode sparse checkout always materializes repository-root files, including the
    # separately validated license. Supplying an individual file here is rejected by newer Git.
    paths: list[str] = []
    configured_paths = repository.get("sparse_paths")
    if configured_paths is not None:
        if not isinstance(configured_paths, list) or not configured_paths:
            raise RuntimeError("repository.sparse_paths must be a non-empty array")
        paths.extend(configured_paths)
    else:
        paths.extend(project["path"] for project in manifest.get("projects", []))
    return list(dict.fromkeys(paths))


def validate_checkout(destination: Path, manifest: dict, expected_commit: str) -> None:
    repository = manifest["repository"]
    actual_commit = current_commit(destination)
    if actual_commit != expected_commit:
        raise RuntimeError(
            f"corpus commit mismatch: expected {expected_commit}, got {actual_commit}"
        )
    if not (destination / repository["license_file"]).is_file():
        raise RuntimeError("compatibility corpus license file is missing")
    for required_file in repository.get("required_files", []):
        if not (destination / required_file).is_file():
            raise RuntimeError(f"compatibility corpus required file is missing: {required_file}")
    for project in manifest.get("projects", []):
        project_root = destination / project["path"]
        if not (project_root / "project.godot").is_file():
            raise RuntimeError(f"Godot project is missing project.godot: {project['path']}")


def remove_checkout(destination: Path) -> None:
    if not destination.exists():
        return

    def make_writable_and_retry(function, path, error_info) -> None:
        error = error_info[1]
        if not isinstance(error, PermissionError):
            raise error
        os.chmod(path, stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR)
        function(path)

    # Git for Windows marks partial-clone pack indexes read-only. A checkout with
    # a changed remote or damaged metadata must still be replaceable without
    # depending on shell-specific deletion commands.
    shutil.rmtree(destination, onerror=make_writable_and_retry)


def initialize_checkout(destination: Path, repository: dict, paths: list[str]) -> None:
    remove_checkout(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)

    run(["git", "init", "--quiet", str(destination)])
    run(["git", "remote", "add", "origin", repository["url"]], destination)
    run(["git", "config", "extensions.partialClone", "origin"], destination)
    run(["git", "config", "remote.origin.promisor", "true"], destination)
    run(["git", "config", "remote.origin.partialCloneFilter", "blob:none"], destination)
    run(["git", "sparse-checkout", "init", "--cone"], destination)
    run(["git", "sparse-checkout", "set", "--"] + paths, destination)


def checkout_matches_repository(destination: Path, repository: dict) -> bool:
    if current_commit(destination) is None:
        return False
    remote = subprocess.run(
        ["git", "remote", "get-url", "origin"],
        cwd=destination,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return remote.returncode == 0 and remote.stdout.strip() == repository["url"]


def prepare_checkout(destination: Path, repository: dict, paths: list[str]) -> None:
    if not checkout_matches_repository(destination, repository):
        initialize_checkout(destination, repository, paths)
        return

    try:
        # Compatibility corpora are disposable build inputs. Restore tracked,
        # untracked and ignored content so a prior interrupted compiler run can
        # never influence the next gate while preserving the partial-clone cache.
        run(["git", "reset", "--hard", "--quiet"], destination)
        run(["git", "clean", "-ffdx", "--quiet"], destination)
        run(["git", "sparse-checkout", "set", "--"] + paths, destination)
    except subprocess.CalledProcessError:
        # A repository can retain a readable HEAD while its index or sparse
        # metadata is damaged. Rebuild that controlled checkout deterministically.
        initialize_checkout(destination, repository, paths)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    args = parser.parse_args()

    manifest = load_manifest(args.manifest)
    repository = manifest["repository"]
    destination = args.destination.resolve()
    ensure_build_destination(destination, args.build_root)
    revision_kind, revision = repository_revision(repository)
    checkout_paths = sparse_paths(manifest)

    prepare_checkout(destination, repository, checkout_paths)

    if (
        revision_kind == "commit"
        and current_commit(destination) == revision
    ):
        validate_checkout(destination, manifest, revision)
        print(f"pinned compatibility corpus already present at {destination}")
        return 0

    run(
        [
            "git",
            "fetch",
            "--quiet",
            "--depth=1",
            "--filter=blob:none",
            "origin",
            revision,
        ],
        destination,
    )
    fetched_commit = subprocess.run(
        ["git", "rev-parse", "FETCH_HEAD"],
        cwd=destination,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    ).stdout.strip()
    if revision_kind == "commit" and fetched_commit != revision:
        raise RuntimeError(
            f"pinned corpus fetch resolved {revision} to unexpected commit {fetched_commit}"
        )
    run(["git", "checkout", "--quiet", "--detach", "FETCH_HEAD"], destination)

    validate_checkout(destination, manifest, fetched_commit)
    actual_commit = current_commit(destination)

    source = repository.get("branch", revision)
    print(
        f"fetched {repository['name']} {source} at {actual_commit} into {destination}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
