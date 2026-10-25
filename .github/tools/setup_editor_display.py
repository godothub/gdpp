#!/usr/bin/env python3
"""Configure the installed software graphics provider for this CI job only."""
from __future__ import annotations

import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile


LAVAPIPE_MANIFESTS = frozenset(("lvp_icd.x86_64.json", "lvp_icd.json"))
PROBE_COMMAND = ("xvfb-run", "-a", "-s", "-screen 0 1280x720x24 -nolisten tcp",
                 "vulkaninfo", "--summary")
SURFACE_EXTENSION = re.compile(r"(?m)^[ \t]*VK_KHR_surface[ \t]*:")
CPU_DEVICE = re.compile(r"(?m)^[ \t]*deviceType[ \t]*=[ \t]*PHYSICAL_DEVICE_TYPE_CPU[ \t]*\r?$")
FAILURE_CAUSES = frozenset(("environment", "runtime-directory", "package-query", "manifest",
                            "probe-launch", "probe-timeout", "probe-exit", "surface-extension",
                            "cpu-device", "environment-write", "internal"))


class DisplaySetupError(RuntimeError):
    """A fixed cause safe to print outside the private system log."""


def select_manifest(package_files: str) -> Path:
    candidates = {
        Path(line) for line in package_files.splitlines()
        if Path(line).is_absolute() and Path(line).name in LAVAPIPE_MANIFESTS
        and Path(line).is_file() and os.access(line, os.R_OK)
    }
    if len(candidates) != 1:
        raise DisplaySetupError("manifest")
    return candidates.pop()


def query_manifest(environment: dict[str, str], log) -> Path:
    try:
        result = subprocess.run(
            ["dpkg-query", "-L", "mesa-vulkan-drivers:amd64"],
            env=environment, stdout=subprocess.PIPE, stderr=log,
            text=True, encoding="utf-8", errors="replace", timeout=10, check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        raise DisplaySetupError("package-query") from None
    log.write(result.stdout.encode("utf-8"))
    log.flush()
    if result.returncode:
        raise DisplaySetupError("package-query")
    return select_manifest(result.stdout)


def run_probe(environment: dict[str, str], log) -> None:
    try:
        process = subprocess.Popen(PROBE_COMMAND, env=environment, stdout=log,
                                   stderr=subprocess.STDOUT, start_new_session=True)
    except OSError:
        raise DisplaySetupError("probe-launch") from None
    try:
        returncode = process.wait(timeout=30)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait(timeout=5)
        raise DisplaySetupError("probe-timeout") from None
    if returncode:
        raise DisplaySetupError("probe-exit")


def verify_summary(payload: str) -> None:
    if SURFACE_EXTENSION.search(payload) is None:
        raise DisplaySetupError("surface-extension")
    if CPU_DEVICE.search(payload) is None:
        raise DisplaySetupError("cpu-device")


def configure(environment: dict[str, str]) -> None:
    runner_temp = environment.get("RUNNER_TEMP", "")
    github_env = environment.get("GITHUB_ENV", "")
    if (not runner_temp or not github_env or any(char in runner_temp + github_env for char in "\r\n")
            or not Path(runner_temp).is_absolute() or not Path(github_env).is_absolute()):
        raise DisplaySetupError("environment")
    try:
        runtime = Path(tempfile.mkdtemp(prefix="gdpp-editor-runtime-", dir=runner_temp)).resolve()
        runtime.chmod(0o700)
        descriptor = os.open(runtime / "vulkaninfo.log", os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except OSError:
        raise DisplaySetupError("runtime-directory") from None
    probe_environment = {**environment, "LC_ALL": "C", "LANG": "C"}
    with os.fdopen(descriptor, "wb") as log:
        manifest = query_manifest(probe_environment, log)
        if any(char in str(manifest) for char in "\r\n"):
            raise DisplaySetupError("manifest")
        exports = {"VK_DRIVER_FILES": str(manifest), "VK_ICD_FILENAMES": str(manifest),
                   "XDG_RUNTIME_DIR": str(runtime)}
        probe_environment.update(exports)
        probe_start = log.tell()
        run_probe(probe_environment, log)
    try:
        with (runtime / "vulkaninfo.log").open("rb") as input_log:
            input_log.seek(probe_start)
            verify_summary(input_log.read().decode("utf-8", errors="replace"))
        with Path(github_env).open("a", encoding="utf-8", newline="\n") as output:
            output.write("".join(f"{name}={value}\n" for name, value in exports.items()))
    except OSError:
        raise DisplaySetupError("environment-write") from None


def main() -> int:
    try:
        configure(dict(os.environ))
    except DisplaySetupError as error:
        cause = str(error) if str(error) in FAILURE_CAUSES else "internal"
        print(f"GDPP_EDITOR_DISPLAY_FAILED={cause}", flush=True)
        return 1
    except Exception:
        print("GDPP_EDITOR_DISPLAY_FAILED=internal", flush=True)
        return 1
    print("GDPP_EDITOR_DISPLAY_READY=lavapipe", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
