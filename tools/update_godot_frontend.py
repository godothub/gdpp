#!/usr/bin/env python3
"""Detect and prepare auditable updates for a supported Godot stable line."""

from __future__ import annotations

import argparse
import copy
import json
import re
import shutil
import subprocess
from pathlib import Path

from audit_godot_frontend import (
    collect_contract,
    collect_snapshot,
    diff_snapshots,
    verify_gdpp_registry,
    write_json,
)


STABLE_TAG = re.compile(
    r"refs/tags/(?P<major>\d+)\.(?P<minor>\d+)"
    r"(?:\.(?P<patch>\d+))?-stable$"
)


def run(command: list[str], cwd: Path | None = None) -> None:
    subprocess.run(command, cwd=cwd, check=True)


def load_manifest(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as stream:
        manifest = json.load(stream)
    if manifest.get("schema_version") != 1:
        raise RuntimeError("unsupported parser corpus manifest schema")
    repository = manifest.get("repository", {})
    if not repository.get("release_line") or not repository.get("release_tag"):
        raise RuntimeError("parser corpus manifest has no release_line/release_tag contract")
    return manifest


def ensure_below_build_root(path: Path, build_root: Path) -> None:
    resolved = path.resolve()
    root = build_root.resolve()
    if resolved == root or root not in resolved.parents:
        raise RuntimeError(f"update workspace must be below build root: {root}")


def latest_stable_tag(repository: str, release_line: str) -> tuple[str, str]:
    expected_major, expected_minor = (int(part) for part in release_line.split(".", 1))
    result = subprocess.run(
        ["git", "ls-remote", "--tags", repository, f"refs/tags/{release_line}*-stable"],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    )
    candidates: list[tuple[tuple[int, int, int], str, str]] = []
    for line in result.stdout.splitlines():
        commit, reference = line.split(maxsplit=1)
        match = STABLE_TAG.fullmatch(reference)
        if match is None:
            continue
        major = int(match.group("major"))
        minor = int(match.group("minor"))
        patch = int(match.group("patch") or 0)
        if (major, minor) == (expected_major, expected_minor):
            candidates.append(((major, minor, patch), reference.removeprefix("refs/tags/"), commit))
    if not candidates:
        raise RuntimeError(f"no stable Godot tag found for release line {release_line}")
    _, tag, commit = max(candidates)
    return tag, commit


def checkout_commit(destination: Path, repository: str, commit: str) -> None:
    if destination.exists():
        shutil.rmtree(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    run(["git", "init", "--quiet", str(destination)])
    run(["git", "remote", "add", "origin", repository], destination)
    run(["git", "config", "extensions.partialClone", "origin"], destination)
    run(["git", "config", "remote.origin.promisor", "true"], destination)
    run(["git", "config", "remote.origin.partialCloneFilter", "blob:none"], destination)
    run(["git", "sparse-checkout", "init", "--cone"], destination)
    run(["git", "sparse-checkout", "set", "--", "modules/gdscript", "core/string"], destination)
    run(
        [
            "git",
            "fetch",
            "--quiet",
            "--depth=1",
            "--filter=blob:none",
            "origin",
            commit,
        ],
        destination,
    )
    run(["git", "checkout", "--quiet", "--detach", "FETCH_HEAD"], destination)


def candidate_manifest(manifest: dict, snapshot: dict) -> dict:
    candidate = copy.deepcopy(manifest)
    repository = candidate["repository"]
    repository["commit"] = snapshot["commit"]
    repository["release_tag"] = snapshot["release_tag"]
    repository["sparse_paths"] = ["modules/gdscript", "core/string"]
    candidate["parser_corpus"]["expected_valid_scripts"] = snapshot["parser_corpus"][
        "valid"
    ]["count"]
    candidate["parser_corpus"]["expected_invalid_scripts"] = snapshot["parser_corpus"][
        "invalid"
    ]["count"]
    return candidate


def update_unicode_release_tag(path: Path, previous: str, replacement: str) -> None:
    source = path.read_text(encoding="utf-8")
    marker = f"// Godot source tag: {previous}"
    if source.count(marker) != 1:
        raise RuntimeError("generated Unicode table has an unexpected release-tag marker")
    path.write_text(
        source.replace(marker, f"// Godot source tag: {replacement}"),
        encoding="utf-8",
        newline="\n",
    )


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--snapshot", type=Path, required=True)
    parser.add_argument("--language-features", type=Path, required=True)
    parser.add_argument("--unicode-table", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--check-only", action="store_true")
    parser.add_argument("--apply", action="store_true")
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    manifest = load_manifest(arguments.manifest)
    repository = manifest["repository"]
    latest_tag, latest_commit = latest_stable_tag(
        repository["url"], repository["release_line"]
    )
    if arguments.check_only:
        if (
            repository["release_tag"] != latest_tag
            or repository["commit"] != latest_commit
        ):
            print(
                "error: pinned Godot frontend is stale: "
                f"{repository['release_tag']} {repository['commit']} -> "
                f"{latest_tag} {latest_commit}"
            )
            return 1
        print(f"Godot frontend pin is current: {latest_tag} {latest_commit}")
        return 0

    ensure_below_build_root(arguments.output, arguments.build_root)
    update_root = arguments.output.resolve()
    before_checkout = update_root / "before-checkout"
    checkout = update_root / "checkout"
    checkout_commit(before_checkout, repository["url"], repository["commit"])
    checkout_commit(checkout, repository["url"], latest_commit)
    before_contract = collect_contract(
        before_checkout, repository["release_tag"]
    )
    after_contract = collect_contract(checkout, latest_tag)
    after = collect_snapshot(checkout, latest_tag)
    difference = diff_snapshots(before_contract, after_contract)
    candidate = candidate_manifest(manifest, after)
    write_json(update_root / "frontend-snapshot.json", after)
    write_json(arguments.output / "frontend-diff.json", difference)
    write_json(arguments.output / "parser-manifest.json", candidate)

    failures = verify_gdpp_registry(
        after, arguments.language_features, arguments.unicode_table
    )
    tag_only = "GDPP Unicode table does not name the pinned Godot release"
    blocking_failures = [failure for failure in failures if failure != tag_only]
    if blocking_failures:
        for failure in blocking_failures:
            print(f"error: {failure}")
        print(
            "error: candidate changes require explicit registry/Unicode regeneration "
            "before they can be applied"
        )
        return 1

    if arguments.apply:
        write_json(arguments.manifest, candidate)
        write_json(arguments.snapshot, after)
        if repository["release_tag"] != latest_tag:
            update_unicode_release_tag(
                arguments.unicode_table, repository["release_tag"], latest_tag
            )
        if arguments.report:
            write_json(arguments.report, difference)
        print(f"updated pinned Godot frontend to {latest_tag} {latest_commit}")
    else:
        print(
            f"prepared Godot frontend candidate {latest_tag} {latest_commit} "
            f"under {arguments.output}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
