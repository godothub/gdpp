#!/usr/bin/env python3
"""Validate a moving compatibility corpus against its declared Godot engine."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path


SUPPORTED_TARGETS = frozenset({"4.4", "4.5", "4.6", "4.7"})
TARGET_PATTERN = re.compile(r"^[0-9]+\.[0-9]+$")
ENGINE_PATTERN = re.compile(r"^([0-9]+\.[0-9]+\.[0-9]+)(?:[.\s-]|$)")
FEATURES_PATTERN = re.compile(
    r'(?m)^\s*config/features\s*=\s*PackedStringArray\((.*?)\)\s*$'
)
QUOTED_VALUE_PATTERN = re.compile(r'"((?:[^"\\]|\\.)*)"')


def load_manifest(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as stream:
        manifest = json.load(stream)
    if manifest.get("schema_version") != 1:
        raise RuntimeError("unsupported compatibility corpus manifest schema")
    return manifest


def manifest_contract(manifest: dict) -> tuple[str, str]:
    contract = manifest.get("godot")
    if not isinstance(contract, dict):
        raise RuntimeError("compatibility manifest must define a godot contract")
    target = contract.get("target")
    engine = contract.get("engine")
    if not isinstance(target, str) or not TARGET_PATTERN.fullmatch(target):
        raise RuntimeError("godot.target must be a major.minor version")
    if target not in SUPPORTED_TARGETS:
        raise RuntimeError(f"Godot target {target} is outside the supported 4.4-4.7 range")
    if not isinstance(engine, str) or ENGINE_PATTERN.fullmatch(engine) is None:
        raise RuntimeError("godot.engine must be an exact major.minor.patch version")
    if ".".join(engine.split(".")[:2]) != target:
        raise RuntimeError(
            f"Godot engine {engine} does not belong to target minor {target}"
        )
    return target, engine


def parse_engine_version(output: str) -> str:
    match = ENGINE_PATTERN.match(output.strip())
    if match is None:
        raise RuntimeError(f"cannot parse Godot --version output: {output.strip()!r}")
    return match.group(1)


def query_engine_version(executable: Path) -> tuple[str, str]:
    resolved = executable.resolve()
    if not resolved.is_file():
        raise RuntimeError(f"Godot executable is missing: {resolved}")
    result = subprocess.run(
        [str(resolved), "--version"],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=30,
        check=False,
    )
    output = "\n".join(part for part in (result.stdout, result.stderr) if part).strip()
    if result.returncode != 0:
        raise RuntimeError(
            f"Godot --version failed with exit code {result.returncode}: {output}"
        )
    return parse_engine_version(output), output


def project_feature_version(project_file: Path) -> tuple[str, list[str]]:
    content = project_file.read_text(encoding="utf-8")
    match = FEATURES_PATTERN.search(content)
    if match is None:
        raise RuntimeError(f"project has no config/features declaration: {project_file}")
    features = QUOTED_VALUE_PATTERN.findall(match.group(1))
    versions = [feature for feature in features if TARGET_PATTERN.fullmatch(feature)]
    if len(versions) != 1:
        raise RuntimeError(
            f"project must declare exactly one Godot major.minor feature: {project_file}"
        )
    return versions[0], features


def validate_contract(
    manifest: dict,
    corpus: Path,
    actual_engine: str,
    raw_engine_output: str,
) -> dict:
    target, expected_engine = manifest_contract(manifest)
    if actual_engine != expected_engine:
        raise RuntimeError(
            f"Godot engine mismatch: expected {expected_engine}, got {actual_engine}"
        )

    corpus = corpus.resolve()
    projects: list[dict] = []
    for project in manifest.get("projects", []):
        relative = project.get("path")
        if not isinstance(relative, str) or not relative:
            raise RuntimeError("compatibility project path must be a non-empty string")
        project_root = (corpus / relative).resolve()
        if project_root != corpus and corpus not in project_root.parents:
            raise RuntimeError(f"compatibility project path escapes the corpus: {relative}")
        project_file = project_root / "project.godot"
        if not project_file.is_file():
            raise RuntimeError(f"Godot project is missing project.godot: {relative}")
        feature_version, features = project_feature_version(project_file)
        if feature_version != target:
            raise RuntimeError(
                f"{relative} declares Godot {feature_version}, expected {target}"
            )
        projects.append(
            {
                "path": relative,
                "project_feature": feature_version,
                "features": features,
            }
        )

    if not projects:
        raise RuntimeError("compatibility manifest contains no projects")
    return {
        "schema_version": 1,
        "status": "passed",
        "repository": manifest.get("repository", {}).get("name", ""),
        "target_godot": target,
        "expected_engine": expected_engine,
        "actual_engine": actual_engine,
        "raw_engine_output": raw_engine_output,
        "projects": projects,
    }


def write_report(path: Path, report: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--godot-executable", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    report: dict
    try:
        manifest = load_manifest(args.manifest)
        actual_engine, raw_output = query_engine_version(args.godot_executable)
        report = validate_contract(
            manifest,
            args.corpus,
            actual_engine,
            raw_output,
        )
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        report = {
            "schema_version": 1,
            "status": "failed",
            "error": str(error),
        }
        write_report(args.output, report)
        print(f"compatibility Godot contract failed: {error}", file=sys.stderr)
        return 1

    write_report(args.output, report)
    print(
        f"compatibility Godot contract: {report['repository']} uses "
        f"{report['actual_engine']} for target {report['target_godot']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
