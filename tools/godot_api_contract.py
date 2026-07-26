#!/usr/bin/env python3
"""Validate one immutable Godot extension API/precision build contract."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re


SUPPORTED_VERSIONS = ("4.4", "4.5", "4.6", "4.7")
PRECISIONS = ("single", "double")
API_KINDS = ("official", "custom")


@dataclass(frozen=True)
class GodotApiContract:
    version: str
    precision: str
    kind: str
    sha256: str
    path: Path

    def godot_cpp_cmake_arguments(self) -> list[str]:
        arguments = [
            f"-DGODOTCPP_API_VERSION={self.version}",
            f"-DGODOTCPP_PRECISION={self.precision}",
        ]
        if self.kind == "custom":
            arguments.append(f"-DGODOTCPP_CUSTOM_API_FILE={self.path}")
        return arguments


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_godot_api_contract(
    api_file: Path,
    version: str,
    precision: str,
    kind: str,
    expected_sha256: str,
) -> GodotApiContract:
    path = api_file.resolve()
    if version not in SUPPORTED_VERSIONS:
        raise ValueError(f"unsupported Godot API version: {version}")
    if precision not in PRECISIONS:
        raise ValueError(f"Godot precision must be one of {PRECISIONS}: {precision}")
    if kind not in API_KINDS:
        raise ValueError(f"Godot API kind must be one of {API_KINDS}: {kind}")
    if not re.fullmatch(r"[0-9a-f]{64}", expected_sha256):
        raise ValueError(f"Godot API SHA-256 is malformed: {expected_sha256!r}")
    if not path.is_file():
        raise ValueError(f"Godot extension API is missing: {path}")

    actual_sha256 = file_sha256(path)
    if actual_sha256 != expected_sha256:
        raise ValueError(
            f"Godot extension API SHA-256 mismatch: expected {expected_sha256}, "
            f"got {actual_sha256}: {path}"
        )
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        header = data["header"]
        actual_version = f"{int(header['version_major'])}.{int(header['version_minor'])}"
    except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
        raise ValueError(f"Godot extension API header is invalid: {path}") from error
    if actual_version != version:
        raise ValueError(
            f"Godot extension API version mismatch: expected {version}, "
            f"got {actual_version}: {path}"
        )
    declared_precision = header.get("precision")
    if declared_precision is not None and declared_precision != precision:
        raise ValueError(
            f"Godot extension API precision mismatch: expected {precision}, "
            f"got {declared_precision}: {path}"
        )
    if kind == "official" and header.get("version_build") != "official":
        raise ValueError(
            f"official Godot API contract requires version_build=official: {path}"
        )
    return GodotApiContract(version, precision, kind, actual_sha256, path)
