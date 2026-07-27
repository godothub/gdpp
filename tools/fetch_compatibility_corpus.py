#!/usr/bin/env python3
"""Fetch a sparse compatibility corpus from a pinned commit or moving branch."""

from __future__ import annotations

import argparse
import json
import shutil
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


def initialize_checkout(destination: Path, repository: dict, paths: list[str]) -> None:
    if destination.exists():
        shutil.rmtree(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)

    run(["git", "init", "--quiet", str(destination)])
    run(["git", "remote", "add", "origin", repository["url"]], destination)
    run(["git", "config", "extensions.partialClone", "origin"], destination)
    run(["git", "config", "remote.origin.promisor", "true"], destination)
    run(["git", "config", "remote.origin.partialCloneFilter", "blob:none"], destination)
    run(["git", "sparse-checkout", "init", "--cone"], destination)
    run(["git", "sparse-checkout", "set", "--"] + paths, destination)


def checkout_is_reusable(destination: Path, repository: dict) -> bool:
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
    if remote.returncode != 0 or remote.stdout.strip() != repository["url"]:
        return False
    dirty = subprocess.run(
        ["git", "status", "--porcelain", "--untracked-files=no"],
        cwd=destination,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return dirty.returncode == 0 and not dirty.stdout.strip()


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

    if (
        revision_kind == "commit"
        and current_commit(destination) == revision
        and checkout_is_reusable(destination, repository)
    ):
        # A manifest may add new sparse paths while keeping the same authoritative commit.
        # Reconcile the checkout before validation so persistent developer and CI caches do not
        # require manual deletion merely because coverage expanded.
        run(["git", "sparse-checkout", "set", "--"] + checkout_paths, destination)
        validate_checkout(destination, manifest, revision)
        print(f"pinned compatibility corpus already present at {destination}")
        return 0

    if not checkout_is_reusable(destination, repository):
        initialize_checkout(destination, repository, checkout_paths)
    else:
        run(["git", "sparse-checkout", "set", "--"] + checkout_paths, destination)

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
