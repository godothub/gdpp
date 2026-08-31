#!/usr/bin/env python3
"""Publish an authenticated, resumable GitHub draft release transaction."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from typing import Any, Protocol
from urllib.parse import quote


TOOLS_ROOT = Path(__file__).resolve().parent
if str(TOOLS_ROOT) not in sys.path:
    sys.path.insert(0, str(TOOLS_ROOT))

from verify_release_assets import EXPECTED_NAMES, verify as verify_release_assets  # noqa: E402


VERSION_PATTERN = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+")
SHA_PATTERN = re.compile(r"[0-9a-f]{40}")
REPOSITORY_PATTERN = re.compile(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+")
TRANSACTION_SCHEMA = "gdpp-release-transaction-v1"


class ReleaseError(RuntimeError):
    """The release transaction cannot proceed without weakening its identity."""


class GitHub(Protocol):
    def list_releases(self) -> list[dict[str, Any]]: ...
    def create_release(self, payload: dict[str, Any]) -> dict[str, Any]: ...
    def update_release(self, release_id: int, payload: dict[str, Any]) -> dict[str, Any]: ...
    def get_release(self, release_id: int) -> dict[str, Any]: ...
    def upload_asset(self, tag: str, path: Path) -> None: ...
    def download_asset(self, asset_id: int, destination: Path) -> None: ...
    def delete_asset(self, asset_id: int) -> None: ...
    def tag_target(self, tag: str) -> str | None: ...


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


class GhClient:
    def __init__(self, repository: str, executable: str = "gh") -> None:
        if REPOSITORY_PATTERN.fullmatch(repository) is None:
            raise ReleaseError("repository must use the owner/name form")
        self.repository = repository
        self.executable = executable

    def _run(
        self,
        arguments: list[str],
        *,
        input_bytes: bytes | None = None,
        allow_not_found: bool = False,
    ) -> bytes:
        result = subprocess.run(
            [self.executable, *arguments],
            input=input_bytes,
            capture_output=True,
            check=False,
        )
        if result.returncode != 0:
            diagnostic = result.stderr.decode("utf-8", errors="replace").strip()
            if allow_not_found and "HTTP 404" in diagnostic:
                return b""
            raise ReleaseError(
                f"GitHub command failed ({result.returncode}): {diagnostic or 'no diagnostic'}"
            )
        return result.stdout.strip()

    def _api(
        self,
        endpoint: str,
        *,
        method: str = "GET",
        payload: dict[str, Any] | None = None,
        allow_not_found: bool = False,
    ) -> dict[str, Any] | list[Any] | None:
        arguments = ["api", "--method", method, endpoint]
        input_bytes = None
        if payload is not None:
            arguments.extend(("--input", "-"))
            input_bytes = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        output = self._run(
            arguments, input_bytes=input_bytes, allow_not_found=allow_not_found
        )
        if allow_not_found and not output:
            return None
        try:
            return json.loads(output)
        except json.JSONDecodeError as error:
            raise ReleaseError("GitHub API returned invalid JSON") from error

    def list_releases(self) -> list[dict[str, Any]]:
        releases: list[dict[str, Any]] = []
        for page in range(1, 101):
            value = self._api(
                f"repos/{self.repository}/releases?per_page=100&page={page}"
            )
            if not isinstance(value, list):
                raise ReleaseError("GitHub release list is not an array")
            for release in value:
                if not isinstance(release, dict):
                    raise ReleaseError("GitHub release list contains a non-object")
                releases.append(release)
            if len(value) < 100:
                return releases
        raise ReleaseError("GitHub release pagination exceeded the safety boundary")

    def create_release(self, payload: dict[str, Any]) -> dict[str, Any]:
        value = self._api(
            f"repos/{self.repository}/releases", method="POST", payload=payload
        )
        if not isinstance(value, dict):
            raise ReleaseError("GitHub create-release response is not an object")
        return value

    def update_release(self, release_id: int, payload: dict[str, Any]) -> dict[str, Any]:
        value = self._api(
            f"repos/{self.repository}/releases/{release_id}",
            method="PATCH",
            payload=payload,
        )
        if not isinstance(value, dict):
            raise ReleaseError("GitHub update-release response is not an object")
        return value

    def get_release(self, release_id: int) -> dict[str, Any]:
        value = self._api(f"repos/{self.repository}/releases/{release_id}")
        if not isinstance(value, dict):
            raise ReleaseError("GitHub release response is not an object")
        return value

    def upload_asset(self, tag: str, path: Path) -> None:
        self._run(
            [
                "release",
                "upload",
                tag,
                str(path),
                "--repo",
                self.repository,
            ]
        )

    def download_asset(self, asset_id: int, destination: Path) -> None:
        with destination.open("wb") as output:
            result = subprocess.run(
                [
                    self.executable,
                    "api",
                    "-H",
                    "Accept: application/octet-stream",
                    f"repos/{self.repository}/releases/assets/{asset_id}",
                ],
                stdout=output,
                stderr=subprocess.PIPE,
                check=False,
            )
        if result.returncode != 0:
            destination.unlink(missing_ok=True)
            diagnostic = result.stderr.decode("utf-8", errors="replace").strip()
            raise ReleaseError(
                f"GitHub asset download failed ({result.returncode}): "
                f"{diagnostic or 'no diagnostic'}"
            )

    def delete_asset(self, asset_id: int) -> None:
        self._run(
            [
                "api",
                "--method",
                "DELETE",
                f"repos/{self.repository}/releases/assets/{asset_id}",
            ]
        )

    def tag_target(self, tag: str) -> str | None:
        value = self._api(
            f"repos/{self.repository}/git/ref/tags/{quote(tag, safe='')}",
            allow_not_found=True,
        )
        if value is None:
            return None
        if not isinstance(value, dict) or not isinstance(value.get("object"), dict):
            raise ReleaseError("GitHub tag response is malformed")
        target = value["object"].get("sha")
        if not isinstance(target, str) or SHA_PATTERN.fullmatch(target) is None:
            raise ReleaseError("GitHub tag target is malformed")
        return target


def validate_identity(version: str, source_sha: str, target_sha: str) -> None:
    if VERSION_PATTERN.fullmatch(version) is None:
        raise ReleaseError("release version must be an unprefixed semantic version")
    if SHA_PATTERN.fullmatch(source_sha) is None:
        raise ReleaseError("private source SHA must be exactly 40 lowercase hexadecimal characters")
    if SHA_PATTERN.fullmatch(target_sha) is None:
        raise ReleaseError("public target SHA must be exactly 40 lowercase hexadecimal characters")


def transaction_marker(version: str, source_sha: str, target_sha: str) -> str:
    return (
        f"<!-- {TRANSACTION_SCHEMA} version={version} "
        f"source_sha={source_sha} public_target_sha={target_sha} -->"
    )


def release_body(notes: str, version: str, source_sha: str, target_sha: str) -> str:
    notes = notes.rstrip()
    if not notes:
        raise ReleaseError("release notes cannot be empty")
    if TRANSACTION_SCHEMA in notes:
        raise ReleaseError("release notes collide with the authenticated transaction marker")
    return f"{notes}\n\n{transaction_marker(version, source_sha, target_sha)}\n"


def release_id(value: dict[str, Any]) -> int:
    identifier = value.get("id")
    if not isinstance(identifier, int) or isinstance(identifier, bool) or identifier <= 0:
        raise ReleaseError("GitHub release has an invalid identifier")
    return identifier


def validate_release(
    value: dict[str, Any],
    *,
    version: str,
    target_sha: str,
    body: str,
    draft: bool,
) -> None:
    release_id(value)
    expected = {
        "tag_name": version,
        "target_commitish": target_sha,
        "name": version,
        "body": body,
        "draft": draft,
        "prerelease": False,
    }
    for field, expected_value in expected.items():
        if value.get(field) != expected_value:
            raise ReleaseError(
                f"release transaction field {field} does not match its authenticated identity"
            )
    if not isinstance(value.get("assets"), list):
        raise ReleaseError("GitHub release assets are not an array")


def indexed_assets(value: dict[str, Any]) -> dict[str, dict[str, Any]]:
    result: dict[str, dict[str, Any]] = {}
    for asset in value["assets"]:
        if not isinstance(asset, dict):
            raise ReleaseError("GitHub release contains a malformed asset")
        name = asset.get("name")
        identifier = asset.get("id")
        if (
            not isinstance(name, str)
            or not isinstance(identifier, int)
            or isinstance(identifier, bool)
            or identifier <= 0
        ):
            raise ReleaseError("GitHub release contains an unidentified asset")
        if name in result:
            raise ReleaseError(f"GitHub release repeats asset name: {name}")
        result[name] = asset
    unexpected = set(result) - EXPECTED_NAMES
    if unexpected:
        raise ReleaseError(f"draft release contains unexpected assets: {sorted(unexpected)}")
    return result


def verify_remote_assets(
    github: GitHub,
    release: dict[str, Any],
    local_directory: Path,
) -> None:
    assets = indexed_assets(release)
    if set(assets) != EXPECTED_NAMES:
        raise ReleaseError("draft release does not contain the exact release asset set")
    with tempfile.TemporaryDirectory(prefix="gdpp-release-remote-") as temporary_name:
        temporary = Path(temporary_name)
        for name, asset in assets.items():
            if asset.get("state") != "uploaded":
                raise ReleaseError(f"release asset is not completely uploaded: {name}")
            github.download_asset(asset["id"], temporary / name)
        try:
            verify_release_assets(temporary)
        except (OSError, ValueError) as error:
            raise ReleaseError(
                f"remote release assets violate their checksum contract: {error}"
            ) from error
        for name in EXPECTED_NAMES:
            if file_sha256(temporary / name) != file_sha256(local_directory / name):
                raise ReleaseError(f"remote release asset differs from the gated package: {name}")


def publish(
    github: GitHub,
    *,
    version: str,
    source_sha: str,
    target_sha: str,
    notes_path: Path,
    assets_directory: Path,
) -> dict[str, Any]:
    validate_identity(version, source_sha, target_sha)
    try:
        verify_release_assets(assets_directory)
    except (OSError, ValueError) as error:
        raise ReleaseError(
            f"local release assets violate their checksum contract: {error}"
        ) from error
    try:
        notes = notes_path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise ReleaseError("release notes are not readable UTF-8") from error
    body = release_body(notes, version, source_sha, target_sha)

    matches = [
        release for release in github.list_releases() if release.get("tag_name") == version
    ]
    if len(matches) > 1:
        raise ReleaseError("multiple GitHub releases claim the requested version")
    if matches:
        release = matches[0]
        if release.get("draft") is not True:
            raise ReleaseError("the requested version is already publicly released")
        validate_release(
            release, version=version, target_sha=target_sha, body=body, draft=True
        )
    else:
        if github.tag_target(version) is not None:
            raise ReleaseError("the requested release tag already exists")
        release = github.create_release(
            {
                "tag_name": version,
                "target_commitish": target_sha,
                "name": version,
                "body": body,
                "draft": True,
                "prerelease": False,
            }
        )
        validate_release(
            release, version=version, target_sha=target_sha, body=body, draft=True
        )

    identifier = release_id(release)
    existing = indexed_assets(release)
    for name, asset in list(existing.items()):
        if asset.get("state") == "uploaded":
            with tempfile.TemporaryDirectory(prefix="gdpp-release-existing-") as temporary_name:
                downloaded = Path(temporary_name) / name
                github.download_asset(asset["id"], downloaded)
                if file_sha256(downloaded) != file_sha256(assets_directory / name):
                    raise ReleaseError(
                        f"existing draft asset differs from the gated package: {name}"
                    )
        else:
            github.delete_asset(asset["id"])
            del existing[name]

    for name in sorted(EXPECTED_NAMES - set(existing)):
        github.upload_asset(version, assets_directory / name)

    release = github.get_release(identifier)
    validate_release(release, version=version, target_sha=target_sha, body=body, draft=True)
    verify_remote_assets(github, release, assets_directory)
    draft_tag_target = github.tag_target(version)
    if draft_tag_target not in (None, target_sha):
        raise ReleaseError(
            "the release tag targets a different commit than the authenticated draft"
        )

    published = github.update_release(identifier, {"draft": False})
    validate_release(
        published, version=version, target_sha=target_sha, body=body, draft=False
    )
    published = github.get_release(identifier)
    validate_release(
        published, version=version, target_sha=target_sha, body=body, draft=False
    )
    if set(indexed_assets(published)) != EXPECTED_NAMES:
        raise ReleaseError("published release asset set changed during the atomic commit")
    if github.tag_target(version) != target_sha:
        raise ReleaseError("published release tag does not target the authenticated public commit")
    return published


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--target-sha", required=True)
    parser.add_argument("--notes", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        result = publish(
            GhClient(arguments.repository),
            version=arguments.version,
            source_sha=arguments.source_sha,
            target_sha=arguments.target_sha,
            notes_path=arguments.notes,
            assets_directory=arguments.assets,
        )
    except (OSError, ReleaseError) as error:
        raise SystemExit(f"release transaction: {error}") from error
    url = result.get("html_url")
    print(f"Published authenticated release {arguments.version}: {url or 'URL unavailable'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
