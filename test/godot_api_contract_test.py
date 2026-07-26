#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import sys
import tempfile


SOURCE_ROOT = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(SOURCE_ROOT / "tools"))

from godot_api_contract import validate_godot_api_contract  # noqa: E402


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def expect_failure(fragment: str, callback) -> None:
    try:
        callback()
    except ValueError as error:
        if fragment not in str(error):
            raise AssertionError(f"expected {fragment!r} in {error!r}") from error
        return
    raise AssertionError(f"expected contract failure containing {fragment!r}")


def main() -> None:
    official = SOURCE_ROOT / "third/godot-cpp/gdextension/extension_api.json"
    contract = validate_godot_api_contract(
        official, "4.7", "single", "official", digest(official)
    )
    assert contract.godot_cpp_cmake_arguments() == [
        "-DGODOTCPP_API_VERSION=4.7",
        "-DGODOTCPP_PRECISION=single",
    ]

    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        custom = root / "extension_api.json"
        custom.write_text(
            json.dumps(
                {
                    "header": {
                        "version_major": 4,
                        "version_minor": 7,
                        "version_build": "custom_build",
                        "precision": "double",
                    }
                }
            ),
            encoding="utf-8",
        )
        custom_contract = validate_godot_api_contract(
            custom, "4.7", "double", "custom", digest(custom)
        )
        assert custom_contract.godot_cpp_cmake_arguments() == [
            "-DGODOTCPP_API_VERSION=4.7",
            "-DGODOTCPP_PRECISION=double",
            f"-DGODOTCPP_CUSTOM_API_FILE={custom.resolve()}",
        ]
        expect_failure(
            "precision mismatch",
            lambda: validate_godot_api_contract(
                custom, "4.7", "single", "custom", digest(custom)
            ),
        )
        expect_failure(
            "version mismatch",
            lambda: validate_godot_api_contract(
                custom, "4.6", "double", "custom", digest(custom)
            ),
        )
        expect_failure(
            "SHA-256 mismatch",
            lambda: validate_godot_api_contract(
                custom, "4.7", "double", "custom", "0" * 64
            ),
        )
        expect_failure(
            "version_build=official",
            lambda: validate_godot_api_contract(
                custom, "4.7", "double", "official", digest(custom)
            ),
        )


if __name__ == "__main__":
    main()
