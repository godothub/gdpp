#!/usr/bin/env python3
"""Validate that public engineering documentation matches source-of-truth contracts."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DOCS = ROOT / "docs"

REQUIRED_DOCUMENTS = {
    "ARCHITECTURE.md",
    "COMMERCIAL_DELIVERY.md",
    "COMPATIBILITY.md",
    "CONTRIBUTING.md",
    "GDEXTENSION_BRIDGE.md",
    "GODOT_API.md",
    "IOS.md",
    "PERFORMANCE.md",
    "PLATFORM_TEST_REPORT.md",
    "PROJECT_BUILD.md",
    "ROADMAP.md",
    "STATUS.md",
    "TYPED_IR.md",
    "WEB.md",
}

CUSTOMER_IDENTIFIERS = (
    "Castle Defense",
    "Dungeon Rush",
    "NoahEngine",
    "Pong Duel",
    "pong-duel",
)

STALE_CLAIMS = (
    "待首次留档",
    "待流水线首次留档",
)

MARKDOWN_LINK = re.compile(r"!?\[[^\]]*]\(([^)]+)\)")


def one_match(pattern: str, content: str, source: Path, label: str) -> str:
    matches = re.findall(pattern, content, re.MULTILINE | re.DOTALL)
    if len(matches) != 1:
        raise ValueError(
            f"{source.relative_to(ROOT)} must contain exactly one {label}; "
            f"found {len(matches)}"
        )
    match = matches[0]
    return match if isinstance(match, str) else match[0]


def source_contracts() -> tuple[str, tuple[str, ...], tuple[str, ...], int, int]:
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    plugin = (ROOT / "example/addons/gdpp/plugin.cfg").read_text(encoding="utf-8")
    packaging = (ROOT / "tools/package_release.py").read_text(encoding="utf-8")
    platform_packaging = (ROOT / "tools/package_platform_release.py").read_text(
        encoding="utf-8"
    )

    version = one_match(
        r"project\(\s*gdpp\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)",
        cmake,
        ROOT / "CMakeLists.txt",
        "project version",
    )
    plugin_version = one_match(
        r'^version="([0-9]+\.[0-9]+\.[0-9]+)"$',
        plugin,
        ROOT / "example/addons/gdpp/plugin.cfg",
        "plugin version",
    )
    if plugin_version != version:
        raise ValueError(
            f"plugin version {plugin_version} does not match CMake version {version}"
        )

    cmake_versions = tuple(
        one_match(
            r'set\(GDPP_SUPPORTED_GODOT_VERSIONS\s+"([^"]+)"\)',
            cmake,
            ROOT / "CMakeLists.txt",
            "supported Godot version list",
        ).split(";")
    )
    package_versions = tuple(
        re.findall(
            r'"([0-9]+\.[0-9]+)"',
            one_match(
                r"SUPPORTED_GODOT_VERSIONS\s*=\s*\((.*?)\)",
                packaging,
                ROOT / "tools/package_release.py",
                "packaged Godot version tuple",
            ),
        )
    )
    if package_versions != cmake_versions:
        raise ValueError(
            "tools/package_release.py Godot versions do not match CMake: "
            f"{package_versions} != {cmake_versions}"
        )

    schema = int(
        one_match(
            r"set\(GDPP_NATIVE_SDK_SCHEMA\s+([0-9]+)\)",
            cmake,
            ROOT / "CMakeLists.txt",
            "SDK schema",
        )
    )
    package_schema = int(
        one_match(
            r"^SDK_SCHEMA\s*=\s*([0-9]+)$",
            packaging,
            ROOT / "tools/package_release.py",
            "packaging SDK schema",
        )
    )
    if package_schema != schema:
        raise ValueError(
            f"packaging SDK schema {package_schema} does not match CMake schema {schema}"
        )

    runtime_abi = int(
        one_match(
            r"set\(GDPP_NATIVE_RUNTIME_ABI\s+([0-9]+)\)",
            cmake,
            ROOT / "CMakeLists.txt",
            "native runtime ABI",
        )
    )
    archives = tuple(
        f"{name}.zip"
        for name in re.findall(
            r'ReleasePackage\(\s*"(gdpp(?:-all|-lite)?)",',
            platform_packaging,
        )
    )
    if archives != ("gdpp.zip", "gdpp-all.zip", "gdpp-lite.zip"):
        raise ValueError(
            "tools/package_platform_release.py must define gdpp.zip, gdpp-all.zip, "
            "and gdpp-lite.zip"
        )
    return version, cmake_versions, archives, schema, runtime_abi


def validate_document_set(errors: list[str]) -> dict[str, str]:
    actual = {path.name for path in DOCS.glob("*.md")}
    missing = sorted(REQUIRED_DOCUMENTS - actual)
    unexpected = sorted(actual - REQUIRED_DOCUMENTS)
    if missing:
        errors.append("missing required docs: " + ", ".join(missing))
    if unexpected:
        errors.append(
            "new docs must be added to tools/check_docs.py: " + ", ".join(unexpected)
        )
    return {
        path.name: path.read_text(encoding="utf-8")
        for path in sorted(DOCS.glob("*.md"))
    }


def validate_links(documents: dict[str, str], errors: list[str]) -> None:
    for name, content in documents.items():
        source = DOCS / name
        for line_number, line in enumerate(content.splitlines(), 1):
            for raw_target in MARKDOWN_LINK.findall(line):
                target = raw_target.strip().split(maxsplit=1)[0].strip("<>")
                if (
                    not target
                    or target.startswith(("#", "http://", "https://", "mailto:"))
                ):
                    continue
                relative = target.split("#", 1)[0]
                destination = (source.parent / relative).resolve()
                try:
                    destination.relative_to(ROOT)
                except ValueError:
                    errors.append(
                        f"docs/{name}:{line_number}: link escapes repository: {target}"
                    )
                    continue
                if not destination.exists():
                    errors.append(
                        f"docs/{name}:{line_number}: missing relative link target: {target}"
                    )


def validate_claims(
    documents: dict[str, str],
    version: str,
    versions: tuple[str, ...],
    archives: tuple[str, ...],
    schema: int,
    runtime_abi: int,
    errors: list[str],
) -> None:
    expected_versions = "、".join(versions)

    required_claims = {
        ("STATUS.md", f"审计版本：GDPP {version}。"): "product version",
        ("STATUS.md", f"Godot 目标：{expected_versions}；"): "Godot matrix",
        (
            "STATUS.md",
            f"SDK schema：{schema}；生成项目 runtime ABI：{runtime_abi}。",
        ): "SDK/runtime contract",
        ("COMPATIBILITY.md", f"GDPP {version} 能接受"): "compatibility version",
        ("PERFORMANCE.md", f"{version} 插件发布资产大小"): "performance version",
        ("PLATFORM_TEST_REPORT.md", f"| GDPP | {version} |"): "platform-report version",
        (
            "PROJECT_BUILD.md",
            "gdpp.release.windows.x86_64.dll",
        ): "current Windows project-library name",
        (
            "PROJECT_BUILD.md",
            "libgdpp.release.linux.x86_64.so",
        ): "current Linux project-library name",
        (
            "PROJECT_BUILD.md",
            "libgdpp.release.macos.universal.dylib",
        ): "current macOS project-library name",
        (
            "PROJECT_BUILD.md",
            "GDExtension C 入口固定为 `gdpp_library_init`",
        ): "current project-library entry ABI",
    }
    for archive in archives:
        required_claims[("COMMERCIAL_DELIVERY.md", f"`{archive}`")] = (
            "release archive set"
        )

    for (name, claim), label in required_claims.items():
        if claim not in documents.get(name, ""):
            errors.append(
                f"docs/{name} {label} is missing the source-of-truth claim: {claim!r}"
            )

    for name, content in documents.items():
        for identifier in CUSTOMER_IDENTIFIERS:
            if identifier.casefold() in content.casefold():
                errors.append(
                    f"docs/{name}: customer identifier must not be published: {identifier}"
                )
        for claim in STALE_CLAIMS:
            if claim in content:
                errors.append(f"docs/{name}: stale status claim remains: {claim}")
        if re.search(r"(?:lib)?gdpp_project\.(?:debug|release)", content):
            errors.append(
                f"docs/{name}: retired project-library filename prefix remains"
            )
        if "gdpp_project_library_init" in content:
            errors.append(f"docs/{name}: retired project-library entry ABI remains")


def validate_release_default(version: str, errors: list[str]) -> None:
    workflow = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
    section = re.search(
        r"^\s{6}release_version:\s*$"
        r"(.*?)"
        r"^(?:\s{6}[a-zA-Z_][a-zA-Z0-9_]*:|\S)",
        workflow,
        re.MULTILINE | re.DOTALL,
    )
    if section is None:
        errors.append(".github/workflows/release.yml: cannot locate release_version input")
        return
    default = re.search(
        r"^\s{8}default:\s*([0-9]+\.[0-9]+\.[0-9]+)\s*$",
        section.group(1),
        re.MULTILINE,
    )
    if default is None or default.group(1) != version:
        actual = default.group(1) if default else "<missing>"
        errors.append(
            ".github/workflows/release.yml: release_version default "
            f"{actual} does not match product version {version}"
        )


def main() -> int:
    errors: list[str] = []
    try:
        version, versions, archives, schema, runtime_abi = source_contracts()
    except ValueError as error:
        errors.append(str(error))
        version, versions, archives, schema, runtime_abi = "", (), (), -1, -1

    documents = validate_document_set(errors)
    validate_links(documents, errors)
    if version:
        validate_claims(
            documents, version, versions, archives, schema, runtime_abi, errors
        )
        validate_release_default(version, errors)

    if errors:
        for error in errors:
            print(f"documentation contract error: {error}", file=sys.stderr)
        return 1

    print(
        "documentation contracts are current: "
        f"GDPP {version}, Godot {','.join(versions)}, SDK {schema}, runtime ABI {runtime_abi}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
