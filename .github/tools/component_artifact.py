#!/usr/bin/env python3
"""Seal and verify source-bound binary component artifacts."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import tempfile
from typing import Any, Iterator


MANIFEST_NAME = "GDPP_COMPONENT_ARTIFACT.json"
SCHEMA = "gdpp-component-artifact-v1"
MAX_MANIFEST_SIZE = 16 * 1024 * 1024
SHA_PATTERN = re.compile(r"[0-9a-f]{40}")
ARTIFACT_PATTERN = re.compile(r"[a-z0-9][a-z0-9._-]{0,199}")


class ContractError(ValueError):
    """A component artifact violates its authenticated transport contract."""


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_source_sha(value: str) -> str:
    if SHA_PATTERN.fullmatch(value) is None:
        raise ContractError("source SHA must be exactly 40 lowercase hexadecimal characters")
    return value


def validate_artifact_name(value: str) -> str:
    if ARTIFACT_PATTERN.fullmatch(value) is None:
        raise ContractError("artifact name contains unsupported characters")
    return value


def validate_relative_path(value: str) -> str:
    if not value or value.startswith("/") or "\\" in value or "\x00" in value:
        raise ContractError(f"artifact path is not canonical: {value!r}")
    parts = value.split("/")
    if any(not part or part in (".", "..") for part in parts):
        raise ContractError(f"artifact path is not canonical: {value!r}")
    if any(ord(character) < 32 or ord(character) == 127 for character in value):
        raise ContractError(f"artifact path contains a control character: {value!r}")
    if value == MANIFEST_NAME:
        raise ContractError("artifact payload collides with its manifest")
    return value


def _walk_entries(
    root: Path, directory: Path
) -> Iterator[tuple[str, Path, bool]]:
    try:
        entries = sorted(os.scandir(directory), key=lambda entry: entry.name)
    except OSError as error:
        raise ContractError(f"cannot enumerate artifact directory: {directory}") from error
    for entry in entries:
        path = Path(entry.path)
        try:
            mode = entry.stat(follow_symlinks=False).st_mode
        except OSError as error:
            raise ContractError(f"cannot inspect artifact entry: {path}") from error
        relative = path.relative_to(root).as_posix()
        if relative != MANIFEST_NAME:
            relative = validate_relative_path(relative)
        if stat.S_ISLNK(mode):
            raise ContractError(f"artifact entries cannot be symbolic links: {relative}")
        if stat.S_ISDIR(mode):
            yield relative, path, True
            yield from _walk_entries(root, path)
        elif stat.S_ISREG(mode):
            yield relative, path, False
        else:
            raise ContractError(f"artifact entry is not a regular file or directory: {relative}")


def payload_tree(root: Path) -> tuple[list[str], list[tuple[str, Path]]]:
    try:
        root_mode = root.lstat().st_mode
    except OSError as error:
        raise ContractError(f"artifact root cannot be inspected: {root}") from error
    if not stat.S_ISDIR(root_mode) or root.is_symlink():
        raise ContractError("artifact root must be a real directory")
    directories: list[str] = []
    files: list[tuple[str, Path]] = []
    for relative, path, is_directory in _walk_entries(root, root):
        if relative == MANIFEST_NAME:
            continue
        if is_directory:
            directories.append(relative)
        else:
            files.append((relative, path))
    directories.sort()
    files.sort(key=lambda item: item[0])
    return directories, files


def payload_files(root: Path) -> list[tuple[str, Path]]:
    return payload_tree(root)[1]


def canonical_json(value: object) -> bytes:
    document = json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    )
    return (document + "\n").encode("utf-8")


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            raise ContractError(f"component manifest repeats key: {key}")
        value[key] = item
    return value


def read_manifest(path: Path) -> dict[str, Any]:
    try:
        mode = path.lstat().st_mode
    except OSError as error:
        raise ContractError("component manifest is missing") from error
    if not stat.S_ISREG(mode) or path.is_symlink():
        raise ContractError("component manifest must be a regular file")
    size = path.stat().st_size
    if size <= 0 or size > MAX_MANIFEST_SIZE:
        raise ContractError("component manifest has an invalid size")
    try:
        value = json.loads(
            path.read_text(encoding="utf-8"), object_pairs_hook=_reject_duplicate_keys
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ContractError("component manifest is not canonical UTF-8 JSON") from error
    if not isinstance(value, dict):
        raise ContractError("component manifest root must be an object")
    if canonical_json(value) != path.read_bytes():
        raise ContractError("component manifest encoding is not canonical")
    return value


def expected_manifest(root: Path, artifact_name: str, source_sha: str) -> dict[str, Any]:
    directories, files = payload_tree(root)
    if not files:
        raise ContractError("component artifact cannot be empty")
    return {
        "artifact_name": validate_artifact_name(artifact_name),
        "directories": directories,
        "files": [
            {"path": relative, "sha256": sha256(path), "size": path.stat().st_size}
            for relative, path in files
        ],
        "schema": SCHEMA,
        "source_sha": validate_source_sha(source_sha),
    }


def seal(root: Path, artifact_name: str, source_sha: str) -> None:
    manifest_path = root / MANIFEST_NAME
    if manifest_path.exists() or manifest_path.is_symlink():
        raise ContractError("component artifact is already sealed")
    manifest = expected_manifest(root, artifact_name, source_sha)
    payload = canonical_json(manifest)
    temporary: Path | None = None
    try:
        descriptor, temporary_name = tempfile.mkstemp(
            prefix=f".{MANIFEST_NAME}.", suffix=".incoming", dir=root
        )
        temporary = Path(temporary_name)
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, manifest_path)
        temporary = None
        if os.name != "nt":
            directory_descriptor = os.open(root, os.O_RDONLY)
            try:
                os.fsync(directory_descriptor)
            finally:
                os.close(directory_descriptor)
        verify(root, artifact_name, source_sha)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def verify(root: Path, artifact_name: str, source_sha: str) -> None:
    artifact_name = validate_artifact_name(artifact_name)
    source_sha = validate_source_sha(source_sha)
    manifest = read_manifest(root / MANIFEST_NAME)
    if set(manifest) != {
        "artifact_name",
        "directories",
        "files",
        "schema",
        "source_sha",
    }:
        raise ContractError("component manifest fields differ from the fixed schema")
    if manifest["schema"] != SCHEMA:
        raise ContractError("component manifest schema is unsupported")
    if manifest["artifact_name"] != artifact_name:
        raise ContractError("component artifact name does not match its consumer")
    if manifest["source_sha"] != source_sha:
        raise ContractError("component artifact belongs to a different private source revision")
    directories = manifest["directories"]
    if not isinstance(directories, list):
        raise ContractError("component manifest directory list must be an array")
    previous = ""
    declared_directories: set[str] = set()
    for item in directories:
        path = validate_relative_path(item if isinstance(item, str) else "")
        if path <= previous:
            raise ContractError("component manifest directory paths are not strictly sorted")
        previous = path
        declared_directories.add(path)
    files = manifest["files"]
    if not isinstance(files, list) or not files:
        raise ContractError("component manifest file list must be nonempty")
    previous = ""
    declared: dict[str, tuple[int, str]] = {}
    for item in files:
        if not isinstance(item, dict) or set(item) != {"path", "sha256", "size"}:
            raise ContractError("component manifest file entry differs from the fixed schema")
        path = validate_relative_path(item["path"] if isinstance(item["path"], str) else "")
        if path <= previous:
            raise ContractError("component manifest file paths are not strictly sorted")
        previous = path
        size = item["size"]
        digest = item["sha256"]
        if not isinstance(size, int) or isinstance(size, bool) or size < 0:
            raise ContractError(f"component manifest has an invalid size: {path}")
        if not isinstance(digest, str) or re.fullmatch(r"[0-9a-f]{64}", digest) is None:
            raise ContractError(f"component manifest has an invalid digest: {path}")
        declared[path] = (size, digest)

    actual_directories, actual_files = payload_tree(root)
    if set(actual_directories) != declared_directories:
        missing = sorted(declared_directories - set(actual_directories))
        unexpected = sorted(set(actual_directories) - declared_directories)
        raise ContractError(
            f"component artifact directory topology differs: "
            f"missing={missing}, unexpected={unexpected}"
        )
    actual = {relative: path for relative, path in actual_files}
    if set(actual) != set(declared):
        missing = sorted(set(declared) - set(actual))
        unexpected = sorted(set(actual) - set(declared))
        raise ContractError(
            f"component artifact topology differs: missing={missing}, unexpected={unexpected}"
        )
    for relative, path in actual.items():
        size, digest = declared[relative]
        if path.stat().st_size != size or sha256(path) != digest:
            raise ContractError(f"component artifact content differs: {relative}")


def materialize(root: Path, destination: Path, artifact_name: str, source_sha: str) -> None:
    verify(root, artifact_name, source_sha)
    if destination.exists() or destination.is_symlink():
        raise ContractError("authenticated component destination already exists")
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary: Path | None = Path(
        tempfile.mkdtemp(prefix=f".{destination.name}.", suffix=".incoming", dir=destination.parent)
    )
    try:
        manifest = read_manifest(root / MANIFEST_NAME)
        for relative in manifest["directories"]:
            temporary.joinpath(*relative.split("/")).mkdir()
        for relative, source in payload_files(root):
            target = temporary.joinpath(*relative.split("/"))
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
        declared = {
            item["path"]: (item["size"], item["sha256"]) for item in manifest["files"]
        }
        copied = {relative: path for relative, path in payload_files(temporary)}
        if set(copied) != set(declared):
            raise ContractError("materialized component topology differs from its manifest")
        for relative, path in copied.items():
            size, digest = declared[relative]
            if path.stat().st_size != size or sha256(path) != digest:
                raise ContractError(f"materialized component content differs: {relative}")
        os.replace(temporary, destination)
        temporary = None
    finally:
        if temporary is not None and temporary.exists():
            shutil.rmtree(temporary)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    for command in ("seal", "verify", "materialize"):
        subparser = subparsers.add_parser(command)
        subparser.add_argument("--root", type=Path, required=True)
        subparser.add_argument("--artifact-name", required=True)
        subparser.add_argument("--source-sha", required=True)
        if command == "materialize":
            subparser.add_argument("--destination", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        if arguments.command == "seal":
            seal(arguments.root, arguments.artifact_name, arguments.source_sha)
        elif arguments.command == "materialize":
            materialize(
                arguments.root,
                arguments.destination,
                arguments.artifact_name,
                arguments.source_sha,
            )
        else:
            verify(arguments.root, arguments.artifact_name, arguments.source_sha)
    except (ContractError, OSError) as error:
        raise SystemExit(f"component artifact: {error}") from error
    print(
        f"Authenticated component artifact {arguments.artifact_name} "
        f"for {arguments.source_sha}."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
