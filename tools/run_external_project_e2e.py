#!/usr/bin/env python3
"""Exercise a complete upstream Godot project through the installed GDPP add-on."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from collections import Counter
from pathlib import Path


PLUGIN_RESOURCE = "res://addons/gdpp/plugin.cfg"
FORBIDDEN_DIAGNOSTICS = re.compile(
    r"(^|\s)(SCRIPT ERROR:|ERROR:|CRASH:|FATAL:)|"
    r"Parse Error:|Segmentation fault|EXC_BAD_ACCESS|"
    r"ObjectDB instances were leaked|resources still in use at exit|"
    r"Resource still in use",
    re.MULTILINE | re.IGNORECASE,
)
ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def fail(message: str) -> None:
    raise RuntimeError(message)


def load_manifest(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as stream:
        manifest = json.load(stream)
    if manifest.get("schema_version") != 1:
        fail("unsupported compatibility corpus manifest schema")
    if manifest.get("repository", {}).get("checkout") != "full":
        fail("external project end-to-end validation requires checkout='full'")
    godot = manifest.get("godot")
    if not isinstance(godot, dict) or not godot.get("target") or not godot.get("engine"):
        fail("external project manifest must define its Godot target and exact engine")
    return manifest


def atomic_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    temporary.replace(path)


def run(
    command: list[str],
    *,
    cwd: Path | None,
    timeout: float,
    log: Path,
    allow_timeout: bool = False,
) -> dict:
    started = time.monotonic()
    creationflags = 0
    if os.name == "nt":
        creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    try:
        completed = subprocess.run(
            command,
            cwd=cwd,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
            creationflags=creationflags,
        )
        output = completed.stdout or ""
        timed_out = False
        exit_code = completed.returncode
    except subprocess.TimeoutExpired as error:
        output = error.stdout.decode() if isinstance(error.stdout, bytes) else (error.stdout or "")
        timed_out = True
        exit_code = None
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text(output, encoding="utf-8")
    result = {
        "command": command,
        "exit_code": exit_code,
        "timed_out": timed_out,
        "elapsed_seconds": round(time.monotonic() - started, 3),
        "log": str(log),
    }
    if timed_out and not allow_timeout:
        fail(f"command timed out after {timeout:.0f}s; see {log}")
    if not timed_out and exit_code != 0:
        fail(f"command failed with exit code {exit_code}; see {log}")
    return result


def assert_clean_log(log: Path, phase: str) -> None:
    content = log.read_text(encoding="utf-8", errors="replace")
    match = FORBIDDEN_DIAGNOSTICS.search(content)
    if match:
        line = content.count("\n", 0, match.start()) + 1
        fail(f"{phase} emitted a forbidden diagnostic at {log}:{line}")


def diagnostic_fingerprints(log: Path) -> list[str]:
    fingerprints: list[str] = []
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        normalized = ANSI_ESCAPE.sub("", line)
        if FORBIDDEN_DIAGNOSTICS.search(normalized):
            fingerprints.append(" ".join(normalized.split()))
    return fingerprints


def assert_no_new_diagnostics(
    log: Path,
    baseline: Counter[str],
    phase: str,
) -> None:
    current = Counter(diagnostic_fingerprints(log))
    regressions = sorted(
        (message, count - baseline[message])
        for message, count in current.items()
        if count > baseline[message]
    )
    if regressions:
        message, added_count = regressions[0]
        fail(
            f"{phase} introduced a diagnostic with {added_count} additional occurrence(s) "
            f"not present at that count in the pristine project: {message}"
        )


def report_diagnostics(log: Path) -> tuple[Counter[str], list[dict]]:
    diagnostics = Counter(diagnostic_fingerprints(log))
    return diagnostics, [
        {"message": message, "count": count}
        for message, count in sorted(diagnostics.items())
    ]


def git_output(project: Path, arguments: list[str]) -> str:
    return subprocess.run(
        ["git", *arguments],
        cwd=project,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    ).stdout.strip()


def validate_checkout(project: Path, manifest: dict) -> str:
    if not (project / ".git").is_dir():
        fail(f"external project is not a complete Git checkout: {project}")
    sparse = subprocess.run(
        ["git", "config", "--bool", "core.sparseCheckout"],
        cwd=project,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    if sparse.returncode == 0 and sparse.stdout.strip() == "true":
        fail("external project checkout is unexpectedly sparse")
    tracked_plugin = git_output(project, ["ls-files", "--", "addons/gdpp"])
    if tracked_plugin:
        fail("upstream project already tracks addons/gdpp; refusing to overwrite customer files")
    return git_output(project, ["rev-parse", "HEAD"])


def link_or_copy(source: str, destination: str) -> str:
    try:
        os.link(source, destination)
    except OSError:
        shutil.copy2(source, destination)
    return destination


def install_addon(addon: Path, project: Path, target_godot: str) -> Path:
    addon = addon.resolve()
    project = project.resolve()
    if not (addon / "plugin.cfg").is_file():
        fail(f"GDPP add-on source is incomplete: {addon}")
    sdk = addon / "sdk" / target_godot
    if not (sdk / "sdk.manifest").is_file():
        fail(f"GDPP add-on does not contain the required {target_godot} SDK")
    destination = project / "addons/gdpp"
    if destination.exists():
        shutil.rmtree(destination)

    def ignore(directory: str, names: list[str]) -> set[str]:
        relative = Path(directory).resolve().relative_to(addon)
        ignored: set[str] = set()
        if relative == Path("."):
            ignored.add("build")
        if relative == Path("sdk"):
            ignored.update(name for name in names if name != target_godot)
        if relative == Path("binary"):
            ignored.update(
                name
                for name in names
                if re.match(r"^(?:lib)?gdpp\.(?:debug|release)\.", name, re.IGNORECASE)
            )
        return ignored

    shutil.copytree(
        addon,
        destination,
        ignore=ignore,
        copy_function=link_or_copy,
        symlinks=False,
    )
    if (destination / "build").exists():
        fail("installed add-on unexpectedly contains a build cache")
    installed_sdks = sorted(path.name for path in (destination / "sdk").iterdir() if path.is_dir())
    if installed_sdks != [target_godot]:
        fail(f"installed add-on SDK set is not isolated to {target_godot}: {installed_sdks}")
    return destination


def section_bounds(lines: list[str], section: str) -> tuple[int | None, int]:
    header = f"[{section}]"
    start: int | None = None
    for index, line in enumerate(lines):
        stripped = line.strip()
        if stripped == header:
            start = index
            continue
        if start is not None and index > start and stripped.startswith("[") and stripped.endswith("]"):
            return start, index
    return start, len(lines)


def enable_plugin(project_file: Path) -> None:
    content = project_file.read_text(encoding="utf-8")
    lines = content.splitlines()
    start, end = section_bounds(lines, "editor_plugins")
    if start is None:
        if lines and lines[-1]:
            lines.append("")
        lines.extend(["[editor_plugins]", "", f'enabled=PackedStringArray("{PLUGIN_RESOURCE}")'])
    else:
        assignment = None
        assignment_end = None
        depth = 0
        for index in range(start + 1, end):
            stripped = lines[index].strip()
            if assignment is None and stripped.startswith("enabled="):
                assignment = index
            if assignment is not None:
                depth += lines[index].count("(") - lines[index].count(")")
                if depth <= 0:
                    assignment_end = index + 1
                    break
        if assignment is None:
            lines.insert(end, f'enabled=PackedStringArray("{PLUGIN_RESOURCE}")')
        else:
            assert assignment_end is not None
            joined = "\n".join(lines[assignment:assignment_end])
            plugins = re.findall(r'"((?:[^"\\]|\\.)*)"', joined)
            if PLUGIN_RESOURCE not in plugins:
                plugins.append(PLUGIN_RESOURCE)
            encoded = ", ".join(json.dumps(plugin) for plugin in plugins)
            lines[assignment:assignment_end] = [f"enabled=PackedStringArray({encoded})"]
    project_file.write_text("\n".join(lines) + "\n", encoding="utf-8")


def export_contract(host: str, output: Path) -> tuple[str, str, str]:
    if host == "linux":
        return "Linux", "binary_format/architecture=\"x86_64\"", str(output / "product.x86_64")
    if host == "macos":
        # Official macOS export templates are Universal binaries. Matching that contract also
        # exercises GDPP's dual-architecture SDK and catches either slice regressing.
        return "macOS", 'binary_format/architecture="universal"', str(output / "product.app")
    if host == "windows":
        return "Windows Desktop", "binary_format/architecture=\"x86_64\"", str(output / "product.exe")
    fail(f"unsupported end-to-end host platform: {host}")


def append_export_preset(project: Path, host: str, output: Path) -> tuple[str, Path]:
    output.mkdir(parents=True, exist_ok=True)
    preset_file = project / "export_presets.cfg"
    content = preset_file.read_text(encoding="utf-8") if preset_file.is_file() else ""
    indexes = [int(value) for value in re.findall(r"^\[preset\.(\d+)\]$", content, re.MULTILINE)]
    index = max(indexes, default=-1) + 1
    name = "GDPP External Project E2E"
    platform, architecture, export_path = export_contract(host, output)
    block = (
        f"\n[preset.{index}]\n\n"
        f'name="{name}"\n'
        f'platform="{platform}"\n'
        "runnable=false\n"
        "dedicated_server=false\n"
        'custom_features=""\n'
        'export_filter="all_resources"\n'
        'include_filter=""\n'
        'exclude_filter=""\n'
        f'export_path="{export_path.replace(os.sep, "/")}"\n'
        'encryption_include_filters=""\n'
        'encryption_exclude_filters=""\n'
        "encrypt_pck=false\n"
        "encrypt_directory=false\n"
        "script_export_mode=2\n\n"
        f"[preset.{index}.options]\n\n"
        f"{architecture}\n"
        'application/bundle_identifier="com.gdpp.compatibility"\n'
        "gdpp/strip_gdscript_sources=true\n"
        "gdpp/allow_source_fallback=false\n"
    )
    preset_file.write_text(content.rstrip() + block, encoding="utf-8")
    return name, Path(export_path)


def find_project_library(addon: Path, host: str) -> Path:
    suffix = {"linux": ".so", "macos": ".dylib", "windows": ".dll"}[host]
    candidates = sorted(
        path
        for path in (addon / "binary").glob("*")
        if path.is_file()
        and path.name.lower().endswith(suffix)
        and (
            path.name.lower().startswith("gdpp.release.")
            or path.name.lower().startswith("libgdpp.release.")
        )
    )
    if len(candidates) != 1:
        fail(f"expected one host Release project library, found {len(candidates)}: {candidates}")
    return candidates[0]


def find_pck(product_root: Path) -> Path:
    search_root = product_root if product_root.is_dir() else product_root.parent
    candidates = sorted(search_root.rglob("*.pck"))
    if len(candidates) != 1:
        fail(f"expected one exported PCK, found {len(candidates)} below {search_root}")
    return candidates[0]


def find_executable(product: Path, host: str) -> Path:
    if host != "macos":
        if not product.is_file():
            fail(f"exported executable is missing: {product}")
        return product
    executable_directory = product / "Contents/MacOS"
    candidates = sorted(path for path in executable_directory.iterdir() if path.is_file())
    if len(candidates) != 1:
        fail(f"expected one macOS bundle executable, found {len(candidates)}")
    return candidates[0]


def hash_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def customer_worktree_state(project: Path) -> bytes:
    result = subprocess.run(
        [
            "git",
            "status",
            "--porcelain=v1",
            "-z",
            "--untracked-files=all",
            "--",
            ".",
            ":(exclude)addons/gdpp",
        ],
        cwd=project,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    )
    return result.stdout


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--addon", type=Path, required=True)
    parser.add_argument("--godot-executable", type=Path, required=True)
    parser.add_argument("--audit-script", type=Path, required=True)
    parser.add_argument("--immutability-script", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--host", choices=("linux", "macos", "windows"), required=True)
    parser.add_argument("--import-timeout", type=float, default=600)
    parser.add_argument("--export-timeout", type=float, default=3600)
    parser.add_argument("--runtime-timeout", type=float, default=60)
    args = parser.parse_args()

    manifest = load_manifest(args.manifest.resolve())
    corpus = args.corpus.resolve()
    addon_source = args.addon.resolve()
    godot = args.godot_executable.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    for generated_directory in (
        output / "baseline-product",
        output / "product",
        output / "audit-host",
    ):
        if generated_directory.is_symlink():
            fail(f"end-to-end output must not be a symbolic link: {generated_directory}")
        if generated_directory.exists():
            shutil.rmtree(generated_directory)
    report_path = output / "report.json"
    report: dict = {
        "schema_version": 1,
        "status": "running",
        "repository": manifest["repository"]["name"],
        "resolved_commit": "",
        "godot": manifest["godot"],
        "host": args.host,
        "phases": {},
    }
    atomic_json(report_path, report)

    try:
        report["resolved_commit"] = validate_checkout(corpus, manifest)
        project_specs = manifest.get("projects", [])
        if len(project_specs) != 1:
            fail("external project end-to-end manifests must define exactly one project")
        project = (corpus / project_specs[0]["path"]).resolve()
        if project != corpus and corpus not in project.parents:
            fail("external project path escapes its corpus checkout")
        project_file = project / "project.godot"
        if not project_file.is_file():
            fail(f"Godot project is missing: {project_file}")

        validation_log = output / "godot-contract.log"
        report["phases"]["godot_contract"] = run(
            [
                sys.executable,
                str(Path(__file__).with_name("validate_compatibility_godot.py")),
                "--manifest",
                str(args.manifest.resolve()),
                "--corpus",
                str(corpus),
                "--godot-executable",
                str(godot),
                "--output",
                str(output / "godot-contract.json"),
            ],
            cwd=project,
            timeout=60,
            log=validation_log,
        )

        baseline_bootstrap_log = output / "baseline-import-bootstrap.log"
        report["phases"]["baseline_import_bootstrap"] = run(
            [str(godot), "--headless", "--editor", "--path", str(project), "--import"],
            cwd=project,
            timeout=args.import_timeout,
            log=baseline_bootstrap_log,
        )
        baseline_import_log = output / "baseline-import.log"
        report["phases"]["baseline_import"] = run(
            [str(godot), "--headless", "--editor", "--path", str(project), "--import"],
            cwd=project,
            timeout=args.import_timeout,
            log=baseline_import_log,
        )
        baseline_import_diagnostics, report["baseline_import_diagnostics"] = (
            report_diagnostics(baseline_import_log)
        )

        preset, baseline_product = append_export_preset(
            project,
            args.host,
            output / "baseline-product",
        )
        baseline_export_log = output / "baseline-export.log"
        report["phases"]["baseline_export"] = run(
            [
                str(godot),
                "--headless",
                "--editor",
                "--path",
                str(project),
                "--export-release",
                preset,
                str(baseline_product),
            ],
            cwd=project,
            timeout=args.export_timeout,
            log=baseline_export_log,
        )
        baseline_export_diagnostics, report["baseline_export_diagnostics"] = (
            report_diagnostics(baseline_export_log)
        )

        baseline_executable = find_executable(baseline_product, args.host)
        baseline_runtime_log = output / "baseline-runtime.log"
        report["phases"]["baseline_runtime"] = run(
            [
                str(baseline_executable),
                "--headless",
                "--audio-driver",
                "Dummy",
                "--quit-after",
                "300",
            ],
            cwd=baseline_product.parent,
            timeout=args.runtime_timeout,
            log=baseline_runtime_log,
        )
        baseline_runtime_diagnostics, report["baseline_runtime_diagnostics"] = (
            report_diagnostics(baseline_runtime_log)
        )
        shutil.rmtree(output / "baseline-product")

        installed_addon = install_addon(addon_source, project, manifest["godot"]["target"])
        enable_plugin(project_file)
        product_directory = output / "product"
        product_directory.mkdir(parents=True, exist_ok=True)
        _, _, product_path = export_contract(args.host, product_directory)
        product = Path(product_path)

        bootstrap_import_log = output / "import-bootstrap.log"
        report["phases"]["import_bootstrap"] = run(
            [str(godot), "--headless", "--editor", "--path", str(project), "--import"],
            cwd=project,
            timeout=args.import_timeout,
            log=bootstrap_import_log,
        )

        # The pristine two-pass baseline separates customer-project diagnostics from changes
        # introduced by installing GDPP. Fresh-import transients disappear before either
        # authoritative pass, and any new stable diagnostic remains a release blocker.
        import_log = output / "import.log"
        report["phases"]["import"] = run(
            [str(godot), "--headless", "--editor", "--path", str(project), "--import"],
            cwd=project,
            timeout=args.import_timeout,
            log=import_log,
        )
        assert_no_new_diagnostics(import_log, baseline_import_diagnostics, "Godot import")
        customer_state = customer_worktree_state(project)
        report["customer_worktree_sha256_before_export"] = hashlib.sha256(
            customer_state
        ).hexdigest()

        state = output / "extension-state.json"
        report["phases"]["immutability_snapshot"] = run(
            [
                sys.executable,
                str(args.immutability_script.resolve()),
                "snapshot",
                "--project",
                str(project),
                "--state",
                str(state),
            ],
            cwd=project,
            timeout=60,
            log=output / "immutability-snapshot.log",
        )

        export_log = output / "export.log"
        report["phases"]["export"] = run(
            [
                str(godot),
                "--headless",
                "--editor",
                "--path",
                str(project),
                "--export-release",
                preset,
                str(product),
            ],
            cwd=project,
            timeout=args.export_timeout,
            log=export_log,
        )
        assert_no_new_diagnostics(
            export_log,
            baseline_export_diagnostics,
            "GDPP AOT export",
        )
        customer_state_after_export = customer_worktree_state(project)
        report["customer_worktree_sha256_after_export"] = hashlib.sha256(
            customer_state_after_export
        ).hexdigest()
        if customer_state_after_export != customer_state:
            fail("GDPP export modified customer project files outside addons/gdpp")
        export_text = export_log.read_text(encoding="utf-8", errors="replace")
        if "GDPP_AOT_SUMMARY scenes=" not in export_text:
            fail("export did not report a completed GDPP AOT project build")

        report["phases"]["immutability_verify"] = run(
            [
                sys.executable,
                str(args.immutability_script.resolve()),
                "verify",
                "--project",
                str(project),
                "--state",
                str(state),
            ],
            cwd=project,
            timeout=60,
            log=output / "immutability-verify.log",
        )

        native_library = find_project_library(installed_addon, args.host)
        pck = find_pck(product)
        audit_host = output / "audit-host"
        audit_host.mkdir(parents=True, exist_ok=True)
        (audit_host / "project.godot").write_text(
            '[application]\nconfig/name="GDPP External Project Audit"\n',
            encoding="utf-8",
        )
        audit_copy = audit_host / "audit_export_pck.gd"
        shutil.copy2(args.audit_script.resolve(), audit_copy)
        audit_log = output / "pck-audit.log"
        report["phases"]["pck_audit"] = run(
            [
                str(godot),
                "--headless",
                "--path",
                str(audit_host),
                "--script",
                str(audit_copy),
                "--",
                str(pck),
                str(product if product.is_dir() else product.parent),
                str(native_library),
            ],
            cwd=audit_host,
            timeout=args.import_timeout,
            log=audit_log,
        )
        assert_clean_log(audit_log, "PCK audit")
        if "PCK_AUDIT_VIOLATIONS=0" not in audit_log.read_text(
            encoding="utf-8", errors="replace"
        ):
            fail("PCK audit did not produce a zero-violation result")

        executable = find_executable(product, args.host)
        runtime_log = output / "runtime.log"
        report["phases"]["runtime"] = run(
            [
                str(executable),
                "--headless",
                "--audio-driver",
                "Dummy",
                "--quit-after",
                "300",
            ],
            cwd=product.parent,
            timeout=args.runtime_timeout,
            log=runtime_log,
        )
        assert_no_new_diagnostics(
            runtime_log,
            baseline_runtime_diagnostics,
            "exported project runtime",
        )
        report["artifacts"] = {
            "product": str(product),
            "pck": str(pck),
            "project_library": str(native_library),
            "project_library_sha256": hash_file(native_library),
        }
        report["status"] = "passed"
        atomic_json(report_path, report)
        print(
            f"complete external project E2E passed: {manifest['repository']['name']} "
            f"at {report['resolved_commit']}"
        )
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        report["status"] = "failed"
        report["failure"] = str(error)
        atomic_json(report_path, report)
        print(f"external project E2E failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
