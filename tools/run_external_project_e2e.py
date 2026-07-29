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
E2E_PRESET_NAME = "GDPP External Project E2E"
FORBIDDEN_DIAGNOSTICS = re.compile(
    r"(^|\s)(SCRIPT ERROR:|ERROR:|CRASH:|FATAL:)|"
    r"Parse Error:|Segmentation fault|EXC_BAD_ACCESS|"
    r"ObjectDB instances (?:were )?leaked|resources still in use at exit|"
    r"Resource still in use",
    re.MULTILINE | re.IGNORECASE,
)
ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
DIAGNOSTIC_CONTEXT = re.compile(
    r"^(?:at:|GDScript backtrace\b|\[\d+\]\s|stack trace\b)",
    re.IGNORECASE,
)
LEAK_MAGNITUDE = re.compile(
    r"\b(\d+)\s+(?=(?:ObjectDB instances|resources|RID allocations|RIDs of type)\b)",
    re.IGNORECASE,
)


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
    runtime_user_arguments(manifest)
    runtime_mode(manifest)
    runtime_duration(manifest)
    runtime_ready_markers(manifest)
    runtime_baseline_samples(manifest)
    return manifest


def runtime_user_arguments(manifest: dict) -> list[str]:
    runtime = manifest.get("runtime", {})
    if not isinstance(runtime, dict):
        fail("external project runtime contract must be an object")
    arguments = runtime.get("arguments", [])
    if not isinstance(arguments, list) or len(arguments) > 64:
        fail("external project runtime arguments must be a list of at most 64 strings")
    if any(
        not isinstance(argument, str)
        or not argument
        or "\0" in argument
        for argument in arguments
    ):
        fail("external project runtime arguments contain an invalid value")
    reserved = (
        "--audio-driver",
        "--editor",
        "--export",
        "--headless",
        "--path",
        "--project-manager",
        "--quit",
        "--quit-after",
        "--script",
    )
    if any(
        argument == "--"
        or any(argument == option or argument.startswith(option + "=") for option in reserved)
        for argument in arguments
    ):
        fail("external project runtime arguments attempt to replace runner-owned Godot options")
    return arguments


def runtime_mode(manifest: dict) -> str:
    runtime = manifest.get("runtime", {})
    mode = runtime.get("mode", "exit") if isinstance(runtime, dict) else "exit"
    if mode not in ("exit", "liveness"):
        fail("external project runtime mode must be 'exit' or 'liveness'")
    if mode == "exit" and isinstance(runtime, dict) and "observation_seconds" in runtime:
        fail("exit runtime contracts must use quit_after, not observation_seconds")
    if mode == "liveness" and isinstance(runtime, dict) and "quit_after" in runtime:
        fail("liveness runtime contracts must use observation_seconds, not quit_after")
    return mode


def runtime_duration(manifest: dict) -> int:
    runtime = manifest.get("runtime", {})
    mode = runtime_mode(manifest)
    key = "observation_seconds" if mode == "liveness" else "quit_after"
    value = runtime.get(key, 300) if isinstance(runtime, dict) else 300
    if isinstance(value, bool) or not isinstance(value, int) or not 1 <= value <= 3600:
        fail(f"external project runtime {key} must be an integer from 1 through 3600")
    return value


def runtime_ready_markers(manifest: dict) -> list[str]:
    runtime = manifest.get("runtime", {})
    markers = runtime.get("ready_markers", []) if isinstance(runtime, dict) else []
    if not isinstance(markers, list) or len(markers) > 16:
        fail("external project runtime ready_markers must be an array of at most 16 strings")
    if any(
        not isinstance(marker, str)
        or not marker.strip()
        or "\n" in marker
        or "\r" in marker
        for marker in markers
    ):
        fail("external project runtime ready_markers must contain non-empty single-line strings")
    return markers


def runtime_baseline_samples(manifest: dict) -> int:
    runtime = manifest.get("runtime", {})
    value = runtime.get("baseline_samples", 1) if isinstance(runtime, dict) else 1
    if isinstance(value, bool) or not isinstance(value, int) or not 1 <= value <= 10:
        fail("external project runtime baseline_samples must be an integer from 1 through 10")
    return value


def project_runtime_command(executable: Path, manifest: dict) -> list[str]:
    # Project-specific flags remain visible to OS.get_cmdline_args(), which many real projects use
    # instead of get_cmdline_user_args(). The boundary above prevents them from replacing the
    # executable, project, export, script, audio or lifecycle controls owned by this runner.
    command = [
        str(executable),
        "--headless",
        "--audio-driver",
        "Dummy",
    ]
    if runtime_mode(manifest) == "exit":
        command.extend(("--quit-after", str(runtime_duration(manifest))))
    command.extend(runtime_user_arguments(manifest))
    return command


def run_project_runtime(
    executable: Path,
    manifest: dict,
    *,
    cwd: Path,
    timeout: float,
    log: Path,
) -> dict:
    mode = runtime_mode(manifest)
    command_timeout = timeout
    if mode == "liveness":
        command_timeout = float(runtime_duration(manifest))
        if command_timeout > timeout:
            fail(
                f"external project liveness observation requires {command_timeout:.0f}s but "
                f"the runtime timeout is {timeout:.0f}s"
            )
    result = run(
        project_runtime_command(executable, manifest),
        cwd=cwd,
        timeout=command_timeout,
        log=log,
        allow_timeout=mode == "liveness",
    )
    if mode == "liveness":
        if not result["timed_out"]:
            fail(f"long-running project exited before its liveness observation completed; see {log}")
        result["liveness_observed"] = True
    markers = runtime_ready_markers(manifest)
    if markers:
        content = log.read_text(encoding="utf-8", errors="replace")
        missing = [marker for marker in markers if marker not in content]
        if missing:
            fail(
                "project did not emit its required runtime readiness markers "
                f"{missing!r}; see {log}"
            )
        result["ready_markers_observed"] = markers
    return result


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
    allow_failure: bool = False,
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
    if not timed_out and exit_code != 0 and not allow_failure:
        fail(f"command failed with exit code {exit_code}; see {log}")
    return result


def run_bootstrap_import(
    godot: Path,
    project: Path,
    timeout: float,
    output: Path,
    phase: str,
    maximum_attempts: int = 3,
) -> dict:
    attempts: list[dict] = []
    for attempt in range(1, maximum_attempts + 1):
        log = output / f"{phase}-{attempt}.log"
        result = run(
            [str(godot), "--headless", "--editor", "--path", str(project), "--import"],
            cwd=project,
            timeout=timeout,
            log=log,
            allow_failure=True,
        )
        attempts.append(result)
        if result["exit_code"] == 0:
            # A fresh Godot scan can parse scripts before ResourceUID has indexed every tracked
            # .uid sidecar. The process still exits successfully, and the next identical import
            # resolves those identities from the completed cache. Treat that pass as bootstrap
            # work instead of comparing its order-dependent diagnostics with the pristine run.
            content = (
                log.read_text(encoding="utf-8", errors="replace") if log.is_file() else ""
            )
            if "ERROR: Unrecognized UID:" in content and attempt < maximum_attempts:
                continue
            return {
                "attempts": attempts,
                "successful_attempt": attempt,
            }
    fail(
        f"{phase.replace('-', ' ')} failed in {maximum_attempts} consecutive attempts; "
        f"see {attempts[-1]['log']}"
    )


def assert_clean_log(log: Path, phase: str) -> None:
    content = log.read_text(encoding="utf-8", errors="replace")
    match = FORBIDDEN_DIAGNOSTICS.search(content)
    if match:
        line = content.count("\n", 0, match.start()) + 1
        fail(f"{phase} emitted a forbidden diagnostic at {log}:{line}")


def assert_zero_pck_violations(log: Path) -> None:
    content = log.read_text(encoding="utf-8", errors="replace")
    matches = re.findall(r"^PCK_AUDIT_VIOLATIONS=(\d+)$", content, re.MULTILINE)
    if matches != ["0"]:
        fail("PCK audit did not produce exactly one zero-violation result")


def diagnostic_fingerprints(log: Path) -> list[str]:
    fingerprints: list[str] = []
    lines = [
        " ".join(ANSI_ESCAPE.sub("", line).split())
        for line in log.read_text(encoding="utf-8", errors="replace").splitlines()
    ]
    for index, normalized in enumerate(lines):
        if not FORBIDDEN_DIAGNOSTICS.search(normalized):
            continue
        context: list[str] = []
        for following in lines[index + 1 : index + 9]:
            if FORBIDDEN_DIAGNOSTICS.search(following):
                break
            if DIAGNOSTIC_CONTEXT.search(following):
                context.append(following)
        fingerprints.append(" | ".join([normalized, *context]))
    return fingerprints


def diagnostic_envelope(*logs: Path) -> Counter[str]:
    # Editor imports execute customer @tool plugins while the resource scan is converging. The
    # number of times an existing customer diagnostic is emitted can vary between the bootstrap,
    # settled and export passes even with identical project bytes. Preserve every pristine
    # signature with its origin context. Occurrence counts remain evidence, but scheduling alone
    # is not a regression; a new origin or a larger resource-leak magnitude still fails closed.
    envelope: Counter[str] = Counter()
    for log in logs:
        current = Counter(diagnostic_fingerprints(log))
        for message, count in current.items():
            envelope[message] = max(envelope[message], count)
    return envelope


def normalized_leak_signature(fingerprint: str) -> str:
    header, separator, context = fingerprint.partition(" | ")
    if not re.search(r"\b(?:leaked|still in use)\b", header, re.IGNORECASE):
        return fingerprint
    normalized = LEAK_MAGNITUDE.sub("<count> ", header)
    return normalized + (separator + context if separator else "")


def leak_magnitude(fingerprint: str) -> int | None:
    header = fingerprint.partition(" | ")[0]
    if not re.search(r"\b(?:leaked|still in use)\b", header, re.IGNORECASE):
        return None
    match = LEAK_MAGNITUDE.search(header)
    return int(match.group(1)) if match else None


def assert_no_new_diagnostics(
    log: Path,
    baseline: Counter[str],
    phase: str,
) -> None:
    current = Counter(diagnostic_fingerprints(log))
    baseline_leaks: dict[str, int] = {}
    for fingerprint in baseline:
        magnitude = leak_magnitude(fingerprint)
        if magnitude is None:
            continue
        signature = normalized_leak_signature(fingerprint)
        baseline_leaks[signature] = max(baseline_leaks.get(signature, 0), magnitude)

    regressions: list[str] = []
    for fingerprint in current:
        if fingerprint in baseline:
            continue
        magnitude = leak_magnitude(fingerprint)
        if magnitude is not None:
            signature = normalized_leak_signature(fingerprint)
            if magnitude <= baseline_leaks.get(signature, -1):
                continue
        regressions.append(fingerprint)
    if regressions:
        message = sorted(regressions)[0]
        fail(
            f"{phase} introduced a diagnostic signature or leak magnitude "
            f"not present in the pristine project: {message}"
        )


def report_diagnostics(log: Path) -> tuple[Counter[str], list[dict]]:
    diagnostics = Counter(diagnostic_fingerprints(log))
    return diagnostics, [
        {"message": message, "count": count}
        for message, count in sorted(diagnostics.items())
    ]


def clear_stale_evidence_logs(output: Path) -> None:
    # A failed run can stop before later phases overwrite their logs. Evidence uploads must never
    # combine a current report with a previous run's export or runtime output.
    for log in output.glob("*.log"):
        if log.is_symlink():
            fail(f"end-to-end evidence log must not be a symbolic link: {log}")
        if log.is_file():
            log.unlink()


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


def git_file_at_head(project: Path, relative: str) -> bytes | None:
    completed = subprocess.run(
        ["git", "show", f"HEAD:{relative}"],
        cwd=project,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return completed.stdout if completed.returncode == 0 else None


def without_plugin(content: str) -> str:
    lines = content.splitlines()
    start, end = section_bounds(lines, "editor_plugins")
    if start is None:
        return "\n".join(lines) + ("\n" if content.endswith("\n") else "")
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
        return "\n".join(lines) + ("\n" if content.endswith("\n") else "")
    assert assignment_end is not None
    plugins = re.findall(
        r'"((?:[^"\\]|\\.)*)"', "\n".join(lines[assignment:assignment_end])
    )
    plugins = [plugin for plugin in plugins if plugin != PLUGIN_RESOURCE]
    encoded = ", ".join(json.dumps(plugin) for plugin in plugins)
    lines[assignment:assignment_end] = [f"enabled=PackedStringArray({encoded})"]
    return "\n".join(lines) + "\n"


def without_e2e_presets(content: str) -> str:
    header = re.compile(r"^\[preset\.(\d+)(?:\.options)?\]$", re.MULTILINE)
    matches = list(header.finditer(content))
    removable_indexes: set[str] = set()
    for position, match in enumerate(matches):
        if ".options]" in match.group(0):
            continue
        end = matches[position + 1].start() if position + 1 < len(matches) else len(content)
        block = content[match.start() : end]
        if re.search(
            rf'^name={re.escape(json.dumps(E2E_PRESET_NAME))}$', block, re.MULTILINE
        ):
            removable_indexes.add(match.group(1))
    if not removable_indexes:
        return content
    spans: list[tuple[int, int]] = []
    for position, match in enumerate(matches):
        if match.group(1) not in removable_indexes:
            continue
        end = matches[position + 1].start() if position + 1 < len(matches) else len(content)
        spans.append((match.start(), end))
    for begin, end in reversed(spans):
        content = content[:begin] + content[end:]
    return content.rstrip() + ("\n" if content.strip() else "")


def prepare_pristine_e2e_state(project: Path) -> dict[str, bytes | None]:
    addon = project / "addons/gdpp"
    if addon.exists():
        plugin = addon / "plugin.cfg"
        if not plugin.is_file() or "GDPP" not in plugin.read_text(
            encoding="utf-8", errors="replace"
        ):
            fail("refusing to remove an untracked addons/gdpp directory not identified as GDPP")
        shutil.rmtree(addon)

    # Godot's generated extension list survives an interrupted run and is evaluated before the
    # next editor scan can notice that GDPP was removed. A pristine comparison must therefore
    # start without the previous run's imported extension/resource cache.
    godot_cache = project / ".godot"
    if godot_cache.is_symlink():
        fail("refusing to remove a symbolic .godot cache from the external project")
    if godot_cache.exists():
        shutil.rmtree(godot_cache)

    managed: dict[str, bytes | None] = {}
    project_file = project / "project.godot"
    project_head = git_file_at_head(project, "project.godot")
    if project_head is None:
        fail("external project does not track project.godot")
    current_project = project_file.read_text(encoding="utf-8")
    head_project = project_head.decode("utf-8")
    if current_project != head_project:
        if PLUGIN_RESOURCE not in current_project or without_plugin(
            current_project
        ) != without_plugin(head_project):
            fail("project.godot contains changes not attributable to a previous GDPP E2E run")
        project_file.write_bytes(project_head)
    managed["project.godot"] = project_head

    presets = project / "export_presets.cfg"
    presets_head = git_file_at_head(project, "export_presets.cfg")
    current_presets = presets.read_text(encoding="utf-8") if presets.is_file() else ""
    head_presets = presets_head.decode("utf-8") if presets_head is not None else ""
    if current_presets != head_presets:
        if without_e2e_presets(current_presets) != head_presets:
            fail(
                "export_presets.cfg contains changes not attributable to a previous GDPP E2E run"
            )
        if presets_head is None:
            presets.unlink(missing_ok=True)
        else:
            presets.write_bytes(presets_head)
    managed["export_presets.cfg"] = presets_head
    return managed


def restore_pristine_e2e_state(project: Path, managed: dict[str, bytes | None]) -> None:
    addon = project / "addons/gdpp"
    if addon.exists():
        shutil.rmtree(addon)
    for relative, content in managed.items():
        path = project / relative
        if content is None:
            path.unlink(missing_ok=True)
        else:
            path.write_bytes(content)


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
    target_manifests = sdk / "manifests"
    if not (sdk / "sdk.manifest").is_file() and not (
        target_manifests.is_dir()
        and any(target_manifests.glob("*.sdk.manifest"))
    ):
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


def replace_preset_assignment(body: str, key: str, value: str) -> str:
    assignment = re.compile(rf"^{re.escape(key)}=.*$", re.MULTILINE)
    if assignment.search(body):
        return assignment.sub(f"{key}={value}", body, count=1)
    return body.rstrip() + f"\n{key}={value}\n"


def source_preset_bodies(content: str, platform: str) -> tuple[str, str] | None:
    header = re.compile(r"^\[preset\.(\d+)(?:\.options)?\]$", re.MULTILINE)
    matches = list(header.finditer(content))
    sections: dict[tuple[str, bool], str] = {}
    for position, match in enumerate(matches):
        end = matches[position + 1].start() if position + 1 < len(matches) else len(content)
        sections[(match.group(1), ".options]" in match.group(0))] = content[match.end() : end]
    for (index, options), body in sections.items():
        if options:
            continue
        if re.search(
            rf"^platform={re.escape(json.dumps(platform))}$",
            body,
            re.MULTILINE,
        ):
            return body, sections.get((index, True), "\n")
    return None


def append_export_preset(project: Path, host: str, output: Path) -> tuple[str, Path]:
    output.mkdir(parents=True, exist_ok=True)
    preset_file = project / "export_presets.cfg"
    content = preset_file.read_text(encoding="utf-8") if preset_file.is_file() else ""
    indexes = [int(value) for value in re.findall(r"^\[preset\.(\d+)\]$", content, re.MULTILINE)]
    index = max(indexes, default=-1) + 1
    name = E2E_PRESET_NAME
    platform, architecture, export_path = export_contract(host, output)
    source = source_preset_bodies(content, platform)
    if source is None:
        preset_body = (
            "\n"
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
            "script_export_mode=2\n"
        )
        options_body = '\napplication/bundle_identifier="com.gdpp.compatibility"\n'
    else:
        # Exercise the same resource inclusion, exclusion, feature, template and platform
        # contract the customer actually ships. A synthetic all-resources preset silently drops
        # non-imported files covered by include_filter and can therefore test a different product.
        preset_body, options_body = source

    preset_body = replace_preset_assignment(preset_body, "name", json.dumps(name))
    preset_body = replace_preset_assignment(preset_body, "platform", json.dumps(platform))
    preset_body = replace_preset_assignment(preset_body, "runnable", "false")
    preset_body = replace_preset_assignment(
        preset_body,
        "export_path",
        json.dumps(export_path.replace(os.sep, "/")),
    )
    preset_body = replace_preset_assignment(preset_body, "script_export_mode", "2")
    preset_body = replace_preset_assignment(
        preset_body, "gdpp/strip_gdscript_sources", "true"
    )
    preset_body = replace_preset_assignment(
        preset_body, "gdpp/allow_source_fallback", "false"
    )
    architecture_key, architecture_value = architecture.split("=", 1)
    options_body = replace_preset_assignment(
        options_body, architecture_key, architecture_value
    )
    # The binary-only audit opens the exported PCK independently from the executable. Keep the
    # runner-owned preset deterministic even when the customer's shipping preset embeds its PCK.
    options_body = replace_preset_assignment(
        options_body, "binary_format/embed_pck", "false"
    )
    block = (
        f"\n[preset.{index}]"
        f"{preset_body.rstrip()}\n\n"
        f"[preset.{index}.options]"
        f"{options_body.rstrip()}\n"
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
    clear_stale_evidence_logs(output)
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
    managed_state: dict[str, bytes | None] = {}

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
        managed_state = prepare_pristine_e2e_state(project)

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

        report["phases"]["baseline_import_bootstrap"] = run_bootstrap_import(
            godot,
            project,
            args.import_timeout,
            output,
            "baseline-import-bootstrap",
        )
        baseline_bootstrap_attempt = report["phases"]["baseline_import_bootstrap"][
            "successful_attempt"
        ]
        baseline_bootstrap_log = Path(
            report["phases"]["baseline_import_bootstrap"]["attempts"][
                baseline_bootstrap_attempt - 1
            ]["log"]
        )
        baseline_import_log = output / "baseline-import.log"
        report["phases"]["baseline_import"] = run(
            [str(godot), "--headless", "--editor", "--path", str(project), "--import"],
            cwd=project,
            timeout=args.import_timeout,
            log=baseline_import_log,
        )
        _, report["baseline_import_diagnostics"] = report_diagnostics(baseline_import_log)
        baseline_import_envelope = diagnostic_envelope(
            baseline_bootstrap_log,
            baseline_import_log,
        )
        report["baseline_import_diagnostic_envelope"] = [
            {"message": message, "maximum_pristine_count": count}
            for message, count in sorted(baseline_import_envelope.items())
        ]

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
        baseline_runtime_logs: list[Path] = []
        baseline_runtime_runs: list[dict] = []
        for sample in range(1, runtime_baseline_samples(manifest) + 1):
            baseline_runtime_log = output / (
                "baseline-runtime.log"
                if sample == 1
                else f"baseline-runtime-{sample}.log"
            )
            baseline_runtime_logs.append(baseline_runtime_log)
            baseline_runtime_runs.append(
                run_project_runtime(
                    baseline_executable,
                    manifest,
                    cwd=baseline_product.parent,
                    timeout=args.runtime_timeout,
                    log=baseline_runtime_log,
                )
            )
        report["phases"]["baseline_runtime"] = {"samples": baseline_runtime_runs}
        baseline_runtime_diagnostics = diagnostic_envelope(*baseline_runtime_logs)
        report["baseline_runtime_diagnostics"] = [
            {"message": message, "maximum_pristine_count": count}
            for message, count in sorted(baseline_runtime_diagnostics.items())
        ]
        shutil.rmtree(output / "baseline-product")

        installed_addon = install_addon(addon_source, project, manifest["godot"]["target"])
        enable_plugin(project_file)
        product_directory = output / "product"
        product_directory.mkdir(parents=True, exist_ok=True)
        _, _, product_path = export_contract(args.host, product_directory)
        product = Path(product_path)

        report["phases"]["import_bootstrap"] = run_bootstrap_import(
            godot,
            project,
            args.import_timeout,
            output,
            "import-bootstrap",
        )
        import_bootstrap_attempt = report["phases"]["import_bootstrap"]["successful_attempt"]
        import_bootstrap_log = Path(
            report["phases"]["import_bootstrap"]["attempts"][import_bootstrap_attempt - 1]["log"]
        )
        assert_no_new_diagnostics(
            import_bootstrap_log,
            baseline_import_envelope,
            "Godot bootstrap import",
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
        assert_no_new_diagnostics(import_log, baseline_import_envelope, "Godot import")
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
        # Keep the isolated host deterministic: it audits package contents, the runtime
        # descriptor, source disclosure, and native payload shape without pretending to own the
        # customer's autoloads or custom ResourceFormatLoaders. Mounting the customer's PCK after
        # the empty host has initialized cannot load the project's native script-path provider, so
        # Godot may report missing custom loader scripts while the audit host shuts down. Only the
        # structured violation count is authoritative here; the exported executable launched
        # immediately afterwards owns the full extension/resource runtime gate.
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
        assert_zero_pck_violations(audit_log)

        executable = find_executable(product, args.host)
        runtime_log = output / "runtime.log"
        report["phases"]["runtime"] = run_project_runtime(
            executable,
            manifest,
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
        restore_pristine_e2e_state(project, managed_state)
        managed_state = {}
        print(
            f"complete external project E2E passed: {manifest['repository']['name']} "
            f"at {report['resolved_commit']}"
        )
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        if managed_state:
            try:
                restore_pristine_e2e_state(project, managed_state)
            except OSError as cleanup_error:
                error = RuntimeError(f"{error}; cleanup also failed: {cleanup_error}")
        report["status"] = "failed"
        report["failure"] = str(error)
        atomic_json(report_path, report)
        print(f"external project E2E failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
