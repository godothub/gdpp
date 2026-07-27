#!/usr/bin/env python3
"""Compile a pinned official Godot corpus and enforce compatibility safety gates."""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import importlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path


def load_json(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def write_report(
    report: dict,
    report_path: Path,
    total_scripts: int,
    total_successes: int,
    hard_failures: list[str],
    status: str,
) -> None:
    report["status"] = status
    report["summary"] = {
        "script_count": total_scripts,
        "isolated_successes": total_successes,
        "isolated_success_rate": (
            round(total_successes / total_scripts, 4) if total_scripts else 0.0
        ),
        "completed_project_count": sum(
            project.get("status") == "completed" for project in report["projects"]
        ),
        "hard_failure_count": len(hard_failures),
        "hard_failures": hard_failures,
    }
    temporary_path = report_path.with_suffix(".json.tmp")
    with temporary_path.open("w", encoding="utf-8") as stream:
        json.dump(report, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    temporary_path.replace(report_path)


def generate_binding_headers(source_root: Path, output: Path) -> tuple[Path, float]:
    godot_cpp = source_root / "third/godot-cpp"
    api = godot_cpp / "gdextension/extension_api.json"
    interface = godot_cpp / "gdextension/gdextension_interface.json"
    for required in (godot_cpp / "binding_generator.py", api, interface):
        if not required.is_file():
            raise RuntimeError(f"godot-cpp binding input is missing: {required}")

    started = time.monotonic()
    sys.path.insert(0, str(godot_cpp))
    try:
        binding_generator = importlib.import_module("binding_generator")
        binding_generator.generate_bindings(
            api_filepath=str(api),
            interface_filepath=str(interface),
            use_template_get_node=True,
            output_dir=str(output / "binding-headers"),
        )
    finally:
        sys.path.remove(str(godot_cpp))
    generated_include = output / "binding-headers/gen/include"
    if not (generated_include / "godot_cpp/classes/object.hpp").is_file():
        raise RuntimeError("godot-cpp binding generator did not produce Object headers")
    return generated_include, round(time.monotonic() - started, 4)


def native_syntax_commands(
    compiler: Path,
    compiler_id: str,
    source_root: Path,
    generated_include: Path,
    project_output: Path,
) -> list[tuple[Path, list[str]]]:
    sources = sorted((project_output / "generated").glob("*.cpp"))
    sources.append(project_output / "register_types.cpp")
    missing = [path for path in sources if not path.is_file()]
    if missing:
        raise RuntimeError(
            "generated native source is missing: " + ", ".join(str(path) for path in missing)
        )

    include_directories = (
        project_output / "generated",
        source_root / "include",
        source_root / "third/godot-cpp/include",
        generated_include,
    )
    if compiler_id.upper() == "MSVC":
        definitions = (
            "GDEXTENSION",
            "THREADS_ENABLED",
            "WINDOWS_ENABLED",
            "TYPED_METHOD_BIND",
            "_HAS_EXCEPTIONS=0",
            "NOMINMAX",
        )
        command = [
            str(compiler),
            "/nologo",
            "/std:c++17",
            "/permissive-",
            "/Zc:__cplusplus",
            "/utf-8",
            "/bigobj",
            "/EHsc",
            "/Zs",
        ]
        command.extend(f"/D{definition}" for definition in definitions)
        command.extend(f"/I{directory}" for directory in include_directories)
    else:
        definitions = ["GDEXTENSION", "THREADS_ENABLED"]
        if sys.platform == "darwin":
            definitions.extend(("MACOS_ENABLED", "UNIX_ENABLED"))
        elif sys.platform.startswith("linux"):
            definitions.extend(("LINUX_ENABLED", "UNIX_ENABLED"))
        command = [
            str(compiler),
            "-std=c++17",
            "-fsyntax-only",
            "-fno-exceptions",
            "-fPIC",
        ]
        command.extend(f"-D{definition}" for definition in definitions)
        command.extend(f"-I{directory}" for directory in include_directories)
    return [(source, command + [str(source)]) for source in sources]


def invoke_native_syntax(
    commands: list[tuple[Path, list[str]]], timeout: float, jobs: int
) -> dict:
    started = time.monotonic()

    def compile_unit(item: tuple[Path, list[str]]) -> dict:
        source, command = item
        result = invoke(command, timeout)
        result["path"] = source.as_posix()
        return result

    with concurrent.futures.ThreadPoolExecutor(
        max_workers=min(max(1, jobs), len(commands))
    ) as executor:
        translation_units = list(executor.map(compile_unit, commands))

    failures = [result for result in translation_units if result["exit_code"] != 0]
    diagnostics = []
    for result in failures:
        output = result["stderr"] or result["stdout"]
        diagnostics.append(f"{result['path']}:\n{output}")
    return {
        "exit_code": 0 if not failures else 1,
        "elapsed_seconds": round(time.monotonic() - started, 4),
        "translation_unit_count": len(translation_units),
        "failed_translation_unit_count": len(failures),
        "translation_units": translation_units,
        "stdout": "",
        "stderr": "\n".join(diagnostics)[-16000:],
        "timed_out": any(result["timed_out"] for result in translation_units),
        "crashed": any(result["crashed"] for result in translation_units),
    }


def invoke(command: list[str], timeout: float) -> dict:
    started = time.monotonic()
    try:
        result = subprocess.run(
            command,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
            check=False,
        )
        return {
            "exit_code": result.returncode,
            "elapsed_seconds": round(time.monotonic() - started, 4),
            "stdout": result.stdout[-4000:],
            "stderr": result.stderr[-4000:],
            "timed_out": False,
            "crashed": result.returncode < 0,
        }
    except subprocess.TimeoutExpired as error:
        stdout = error.stdout.decode() if isinstance(error.stdout, bytes) else (error.stdout or "")
        stderr = error.stderr.decode() if isinstance(error.stderr, bytes) else (error.stderr or "")
        return {
            "exit_code": None,
            "elapsed_seconds": round(time.monotonic() - started, 4),
            "stdout": stdout[-4000:],
            "stderr": stderr[-4000:],
            "timed_out": True,
            "crashed": False,
        }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--cxx-compiler", type=Path, required=True)
    parser.add_argument("--compiler-id", required=True)
    parser.add_argument("--sdk-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--target-godot", default="4.7")
    parser.add_argument("--file-timeout", type=float, default=10.0)
    parser.add_argument("--project-timeout", type=float, default=60.0)
    parser.add_argument("--native-build-timeout", type=float, default=600.0)
    parser.add_argument(
        "--native-jobs", type=int, default=min(8, os.cpu_count() or 1)
    )
    args = parser.parse_args()
    if args.native_jobs < 1:
        parser.error("--native-jobs must be at least 1")

    manifest = load_json(args.manifest)
    corpus = args.corpus.resolve()
    compiler = args.compiler.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report_path = output / "report.json"

    actual_commit = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=corpus,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    ).stdout.strip()
    expected_commit = manifest["repository"]["commit"]
    if actual_commit != expected_commit:
        raise RuntimeError(f"corpus commit mismatch: expected {expected_commit}, got {actual_commit}")

    report = {
        "schema_version": 2,
        "repository": manifest["repository"],
        "target_godot": args.target_godot,
        "projects": [],
        "native_validation": {
            "binding_headers_generated": False,
            "binding_generation_seconds": 0.0,
            "validated_project_count": 0,
        },
        "summary": {},
    }
    hard_failures: list[str] = []
    total_scripts = 0
    total_successes = 0
    source_sdk_root = args.sdk_root.resolve()
    godot_cpp_directory = source_sdk_root / "third/godot-cpp"
    generated_include, generation_seconds = generate_binding_headers(
        source_sdk_root, output
    )
    report["native_validation"]["binding_headers_generated"] = True
    report["native_validation"]["binding_generation_seconds"] = generation_seconds
    write_report(report, report_path, total_scripts, total_successes, hard_failures, "running")

    project_count = len(manifest["projects"])
    for project_index, project_spec in enumerate(manifest["projects"], 1):
        relative_project = project_spec["path"]
        project_root = corpus / relative_project
        scripts = sorted(project_root.rglob("*.gd"))
        run_isolated = project_spec.get("run_isolated", True)
        project_report = {
            "path": relative_project,
            "status": "running",
            "phase": "isolated_compile" if run_isolated else "project_compile",
            "script_count": len(scripts),
            "isolated_successes": 0,
            "isolated_compile_enabled": run_isolated,
            "minimum_isolated_successes": project_spec["minimum_isolated_successes"],
            "require_project_native_success": project_spec.get(
                "require_project_native_success", False
            ),
            "scripts": [],
        }
        report["projects"].append(project_report)
        if run_isolated:
            print(
                f"[{project_index}/{project_count}] {relative_project}: "
                f"compiling {len(scripts)} script(s) in isolation",
                flush=True,
            )

            for script in scripts:
                relative_script = script.relative_to(corpus).as_posix()
                digest = hashlib.sha256(relative_script.encode("utf-8")).hexdigest()[:16]
                generated = output / "generated" / digest
                result = invoke(
                    [
                        str(compiler),
                        "compile",
                        str(script),
                        "--output",
                        str(generated),
                        "--target-godot",
                        args.target_godot,
                    ],
                    args.file_timeout,
                )
                result["path"] = relative_script
                project_report["scripts"].append(result)
                total_scripts += 1
                if result["exit_code"] == 0:
                    project_report["isolated_successes"] += 1
                    total_successes += 1
                elif (
                    result["timed_out"]
                    or result["crashed"]
                    or result["exit_code"] not in (1,)
                ):
                    hard_failures.append(
                        f"unsafe isolated compile result for {relative_script}"
                    )
        write_report(report, report_path, total_scripts, total_successes, hard_failures, "running")

        # ProjectCompiler intentionally requires its output to remain inside the Godot project.
        # The sparse corpus itself lives below the repository build/ root, so this still obeys
        # the global artifact-location invariant without modifying checked-in source.
        project_output = project_root / "addons/gdpp/build/compatibility"
        project_report["phase"] = "project_compile"
        print(
            f"[{project_index}/{project_count}] {relative_project}: project compile",
            flush=True,
        )
        project_result = invoke(
            [
                str(compiler),
                "project",
                str(project_root),
                "--output",
                str(project_output),
                "--target-godot",
                args.target_godot,
                "--sdk-root",
                str(source_sdk_root),
                "--godot-cpp",
                str(godot_cpp_directory),
            ],
            args.project_timeout,
        )
        project_report["project_compile"] = project_result
        write_report(report, report_path, total_scripts, total_successes, hard_failures, "running")
        if (
            project_result["timed_out"]
            or project_result["crashed"]
            or project_result["exit_code"] not in (0, 1)
        ):
            hard_failures.append(f"unsafe project compile result for {relative_project}")
        if project_report["require_project_native_success"] and project_result["exit_code"] != 0:
            hard_failures.append(f"required project compile failed for {relative_project}")
        if project_result["exit_code"] == 0:
            project_report["phase"] = "native_syntax"
            print(
                f"[{project_index}/{project_count}] {relative_project}: "
                "validating generated C++",
                flush=True,
            )
            native_result = invoke_native_syntax(
                native_syntax_commands(
                    args.cxx_compiler.resolve(),
                    args.compiler_id,
                    source_sdk_root,
                    generated_include,
                    project_output,
                ),
                args.native_build_timeout,
                args.native_jobs,
            )
            project_report["native_syntax"] = native_result
            if native_result["exit_code"] != 0:
                hard_failures.append(f"generated native code failed for {relative_project}")
            else:
                report["native_validation"]["validated_project_count"] += 1
                if project_report["require_project_native_success"]:
                    project_report["project_native_gate_passed"] = True
        if project_report["isolated_successes"] < project_spec["minimum_isolated_successes"]:
            hard_failures.append(
                f"compatibility regression for {relative_project}: "
                f"{project_report['isolated_successes']} < {project_spec['minimum_isolated_successes']}"
            )
        project_report["status"] = "completed"
        project_report["phase"] = "completed"
        write_report(report, report_path, total_scripts, total_successes, hard_failures, "running")
        isolated_status = (
            f"{project_report['isolated_successes']}/{project_report['script_count']} isolated"
            if project_report["isolated_compile_enabled"]
            else "isolated compile skipped"
        )
        print(
            f"[{project_index}/{project_count}] {relative_project}: "
            f"{isolated_status}; "
            f"project exit={project_result['exit_code']}; "
            f"native exit={project_report.get('native_syntax', {}).get('exit_code', 'skipped')}",
            flush=True,
        )

    write_report(report, report_path, total_scripts, total_successes, hard_failures, "completed")

    print(
        f"compatibility corpus: {total_successes}/{total_scripts} scripts compiled in isolation; "
        f"{len(hard_failures)} hard failure(s); report: {report_path}",
        flush=True,
    )
    for project in report["projects"]:
        isolated_status = (
            f"{project['isolated_successes']}/{project['script_count']}"
            if project["isolated_compile_enabled"]
            else "skipped"
        )
        print(
            f"  {project['path']}: isolated={isolated_status}; "
            f"project exit={project['project_compile']['exit_code']}; "
            f"native exit={project.get('native_syntax', {}).get('exit_code', 'skipped')}"
        )
    if hard_failures:
        for failure in hard_failures:
            print(f"error: {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
