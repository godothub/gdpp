#!/usr/bin/env python3
"""Snapshot and verify the official Godot GDScript frontend contract."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path


SCHEMA_VERSION = 1
PARSER_ROOT = Path("modules/gdscript/tests/scripts/parser")
VALID_DIRECTORIES = ("features", "warnings")
INVALID_DIRECTORIES = ("errors",)
WARNING_SOURCE = Path("modules/gdscript/gdscript_warning.cpp")
ANNOTATION_SOURCE = Path("modules/gdscript/gdscript_parser.cpp")
UNICODE_SOURCE = Path("core/string/char_range.cpp")
WARNING_ARRAY = re.compile(
    r"static const char \*names\[\] = \{(?P<body>.*?)\n\s*\};", re.DOTALL
)
WARNING_NAME = re.compile(
    r'PNAME\("(?P<pname>[A-Z0-9_]+)"\)|"(?P<plain>[A-Z][A-Z0-9_]+)"'
)
ANNOTATION_NAME = re.compile(
    r'register_annotation\s*\(\s*MethodInfo\s*\(\s*"@(?P<name>[a-z0-9_]+)"'
)
OFFICIAL_RANGE_ARRAY = re.compile(
    r"const int (?P<name>xid_(?:start|continue))_size = (?P<count>\d+);\s*"
    r"const CharRange (?P=name)\[(?P=name)_size\] = \{(?P<body>.*?)\n\};",
    re.DOTALL,
)
GDPP_RANGE_ARRAY = re.compile(
    r"std::array<UnicodeRange,\s*(?P<count>\d+)>\s+"
    r"(?P<name>xid_(?:start|continue))_ranges\{\{(?P<body>.*?)\n\}\};",
    re.DOTALL,
)
OFFICIAL_RANGE = re.compile(r"\{\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+)\s*\}")
GDPP_RANGE = re.compile(
    r"UnicodeRange\{\s*(0x[0-9a-fA-F]+)U,\s*(0x[0-9a-fA-F]+)U\s*\}"
)
GDPP_ANNOTATION = re.compile(r'AnnotationFeature\{"(?P<name>[a-z0-9_]+)"')
GDPP_WARNING_BLOCK = re.compile(
    r"constexpr auto warning_names = std::array\{(?P<body>.*?)\n\};", re.DOTALL
)
GDPP_WARNING = re.compile(r'std::string_view\{"(?P<name>[a-z0-9_]+)"\}')
GDPP_UNICODE_TAG = re.compile(r"^// Godot source tag: (?P<tag>\S+)$", re.MULTILINE)
GDPP_UNICODE_SOURCE_HASH = re.compile(
    r"^// Godot char_range\.cpp SHA-256: (?P<digest>[0-9a-f]{64})$",
    re.MULTILINE,
)


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def read_required(path: Path) -> bytes:
    try:
        return path.read_bytes()
    except OSError as error:
        raise RuntimeError(f"required Godot frontend source is missing: {path}") from error


def repository_commit(checkout: Path) -> str:
    result = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=checkout,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    )
    return result.stdout.strip()


def parser_entries(checkout: Path, directories: tuple[str, ...]) -> list[dict[str, str]]:
    entries: list[dict[str, str]] = []
    for directory in directories:
        root = checkout / PARSER_ROOT / directory
        if not root.is_dir():
            raise RuntimeError(f"official parser corpus directory is missing: {root}")
        for script in sorted(root.rglob("*.gd")):
            entries.append(
                {
                    "path": script.relative_to(checkout).as_posix(),
                    "sha256": sha256_bytes(script.read_bytes()),
                }
            )
    return entries


def compact_entries(entries: list[dict[str, str]]) -> dict[str, int | str]:
    payload = "".join(f"{item['path']} {item['sha256']}\n" for item in entries)
    return {
        "count": len(entries),
        "tree_sha256": sha256_bytes(payload.encode("utf-8")),
    }


def official_warning_names(source: str) -> list[str]:
    match = WARNING_ARRAY.search(source)
    if match is None:
        raise RuntimeError("cannot locate Godot's warning name table")
    names = [
        (item.group("pname") or item.group("plain")).lower()
        for item in WARNING_NAME.finditer(match.group("body"))
    ]
    if not names or len(names) != len(set(names)):
        raise RuntimeError("Godot's warning name table is empty or contains duplicates")
    return sorted(names)


def official_annotation_names(source: str) -> list[str]:
    names = ANNOTATION_NAME.findall(source)
    if not names or len(names) != len(set(names)):
        raise RuntimeError("Godot's annotation registry is empty or contains duplicates")
    return sorted(names)


def canonical_range_digest(ranges: list[tuple[int, int]]) -> str:
    payload = "".join(f"{first:08x}-{last:08x}\n" for first, last in ranges)
    return sha256_bytes(payload.encode("ascii"))


def parse_range_tables(
    source: str, array_pattern: re.Pattern[str], range_pattern: re.Pattern[str]
) -> dict[str, dict[str, int | str]]:
    tables: dict[str, dict[str, int | str]] = {}
    for match in array_pattern.finditer(source):
        ranges = [
            (int(first, 16), int(last, 16))
            for first, last in range_pattern.findall(match.group("body"))
        ]
        expected = int(match.group("count"))
        if len(ranges) != expected:
            raise RuntimeError(
                f"{match.group('name')} contains {len(ranges)} ranges, expected {expected}"
            )
        if any(first > last for first, last in ranges) or any(
            previous[1] >= current[0]
            for previous, current in zip(ranges, ranges[1:])
        ):
            raise RuntimeError(f"{match.group('name')} ranges are invalid")
        tables[match.group("name")] = {
            "range_count": len(ranges),
            "range_sha256": canonical_range_digest(ranges),
        }
    if set(tables) != {"xid_start", "xid_continue"}:
        raise RuntimeError("Unicode source is missing XID_Start or XID_Continue")
    return tables


def collect_contract(checkout: Path, release_tag: str) -> dict:
    checkout = checkout.resolve()
    warning_bytes = read_required(checkout / WARNING_SOURCE)
    annotation_bytes = read_required(checkout / ANNOTATION_SOURCE)
    unicode_bytes = read_required(checkout / UNICODE_SOURCE)
    unicode_source = unicode_bytes.decode("utf-8")
    return {
        "schema_version": SCHEMA_VERSION,
        "release_tag": release_tag,
        "commit": repository_commit(checkout),
        "parser_corpus": {
            "valid": parser_entries(checkout, VALID_DIRECTORIES),
            "invalid": parser_entries(checkout, INVALID_DIRECTORIES),
        },
        "warnings": official_warning_names(warning_bytes.decode("utf-8")),
        "annotations": official_annotation_names(annotation_bytes.decode("utf-8")),
        "unicode_identifiers": {
            "source_path": UNICODE_SOURCE.as_posix(),
            "source_sha256": sha256_bytes(unicode_bytes),
            "tables": parse_range_tables(
                unicode_source, OFFICIAL_RANGE_ARRAY, OFFICIAL_RANGE
            ),
        },
    }


def collect_snapshot(checkout: Path, release_tag: str) -> dict:
    contract = collect_contract(checkout, release_tag)
    contract["parser_corpus"] = {
        category: compact_entries(contract["parser_corpus"][category])
        for category in ("valid", "invalid")
    }
    return contract


def load_snapshot(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as stream:
        snapshot = json.load(stream)
    if snapshot.get("schema_version") != SCHEMA_VERSION:
        raise RuntimeError("unsupported Godot frontend snapshot schema")
    return snapshot


def write_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    temporary.replace(path)


def keyed_entries(snapshot: dict, category: str) -> dict[str, str]:
    return {
        item["path"]: item["sha256"]
        for item in snapshot["parser_corpus"][category]
    }


def list_delta(before: list[str], after: list[str]) -> dict[str, list[str]]:
    before_set = set(before)
    after_set = set(after)
    return {
        "added": sorted(after_set - before_set),
        "removed": sorted(before_set - after_set),
    }


def entry_delta(before: dict[str, str], after: dict[str, str]) -> dict[str, list[str]]:
    before_paths = set(before)
    after_paths = set(after)
    return {
        "added": sorted(after_paths - before_paths),
        "removed": sorted(before_paths - after_paths),
        "changed": sorted(
            path
            for path in before_paths & after_paths
            if before[path] != after[path]
        ),
    }


def diff_snapshots(before: dict, after: dict) -> dict:
    return {
        "schema_version": SCHEMA_VERSION,
        "before": {
            "release_tag": before["release_tag"],
            "commit": before["commit"],
        },
        "after": {
            "release_tag": after["release_tag"],
            "commit": after["commit"],
        },
        "parser_corpus": {
            category: entry_delta(
                keyed_entries(before, category), keyed_entries(after, category)
            )
            for category in ("valid", "invalid")
        },
        "warnings": list_delta(before["warnings"], after["warnings"]),
        "annotations": list_delta(before["annotations"], after["annotations"]),
        "unicode_identifiers": {
            "changed": before["unicode_identifiers"]
            != after["unicode_identifiers"],
            "before": before["unicode_identifiers"],
            "after": after["unicode_identifiers"],
        },
    }


def verify_gdpp_registry(
    snapshot: dict, language_features: Path, unicode_table: Path
) -> list[str]:
    failures: list[str] = []
    language_source = read_required(language_features).decode("utf-8")
    gdpp_annotations = sorted(GDPP_ANNOTATION.findall(language_source))
    warning_match = GDPP_WARNING_BLOCK.search(language_source)
    if warning_match is None:
        failures.append("GDPP warning registry is missing")
        gdpp_warnings: list[str] = []
    else:
        gdpp_warnings = sorted(GDPP_WARNING.findall(warning_match.group("body")))
    if gdpp_annotations != snapshot["annotations"]:
        failures.append("GDPP annotation registry differs from the pinned Godot registry")
    if gdpp_warnings != snapshot["warnings"]:
        failures.append("GDPP warning registry differs from the pinned Godot registry")

    unicode_source = read_required(unicode_table).decode("utf-8")
    tag = GDPP_UNICODE_TAG.search(unicode_source)
    digest = GDPP_UNICODE_SOURCE_HASH.search(unicode_source)
    if tag is None or tag.group("tag") != snapshot["release_tag"]:
        failures.append("GDPP Unicode table does not name the pinned Godot release")
    if (
        digest is None
        or digest.group("digest")
        != snapshot["unicode_identifiers"]["source_sha256"]
    ):
        failures.append("GDPP Unicode table source hash differs from pinned Godot")
    try:
        gdpp_tables = parse_range_tables(
            unicode_source, GDPP_RANGE_ARRAY, GDPP_RANGE
        )
    except RuntimeError as error:
        failures.append(str(error))
    else:
        if gdpp_tables != snapshot["unicode_identifiers"]["tables"]:
            failures.append("GDPP Unicode identifier ranges differ from pinned Godot")
    return failures


def command_snapshot(arguments: argparse.Namespace) -> int:
    write_json(
        arguments.output,
        collect_snapshot(arguments.checkout, arguments.release_tag),
    )
    return 0


def command_diff(arguments: argparse.Namespace) -> int:
    before = collect_contract(arguments.before_checkout, arguments.before_release_tag)
    after = collect_contract(arguments.checkout, arguments.release_tag)
    write_json(arguments.output, diff_snapshots(before, after))
    if arguments.candidate:
        write_json(arguments.candidate, collect_snapshot(arguments.checkout, arguments.release_tag))
    return 0


def command_verify(arguments: argparse.Namespace) -> int:
    expected = load_snapshot(arguments.snapshot)
    actual = collect_snapshot(arguments.checkout, expected["release_tag"])
    failures: list[str] = []
    if actual != expected:
        failures.append("official Godot frontend checkout differs from its pinned snapshot")
        if arguments.report:
            write_json(arguments.report, diff_snapshots(expected, actual))
    failures.extend(
        verify_gdpp_registry(
            expected, arguments.language_features, arguments.unicode_table
        )
    )
    if failures:
        for failure in failures:
            print(f"error: {failure}")
        return 1
    print(
        "Godot frontend snapshot verified: "
        f"{expected['parser_corpus']['valid']['count']} valid, "
        f"{expected['parser_corpus']['invalid']['count']} invalid, "
        f"{len(expected['annotations'])} annotations, "
        f"{len(expected['warnings'])} warnings"
    )
    return 0


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    snapshot = subparsers.add_parser("snapshot")
    snapshot.add_argument("--checkout", type=Path, required=True)
    snapshot.add_argument("--release-tag", required=True)
    snapshot.add_argument("--output", type=Path, required=True)
    snapshot.set_defaults(handler=command_snapshot)

    difference = subparsers.add_parser("diff")
    difference.add_argument("--before-checkout", type=Path, required=True)
    difference.add_argument("--before-release-tag", required=True)
    difference.add_argument("--checkout", type=Path, required=True)
    difference.add_argument("--release-tag", required=True)
    difference.add_argument("--output", type=Path, required=True)
    difference.add_argument("--candidate", type=Path)
    difference.set_defaults(handler=command_diff)

    verify = subparsers.add_parser("verify")
    verify.add_argument("--snapshot", type=Path, required=True)
    verify.add_argument("--checkout", type=Path, required=True)
    verify.add_argument("--language-features", type=Path, required=True)
    verify.add_argument("--unicode-table", type=Path, required=True)
    verify.add_argument("--report", type=Path)
    verify.set_defaults(handler=command_verify)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    return arguments.handler(arguments)


if __name__ == "__main__":
    raise SystemExit(main())
