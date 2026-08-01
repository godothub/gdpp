#!/usr/bin/env python3
"""Run a process while preserving binary output and Windows failure evidence."""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--label", required=True)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    arguments = parser.parse_args()
    if arguments.command[:1] == ["--"]:
        arguments.command = arguments.command[1:]
    if not arguments.command:
        parser.error("a command is required after --")
    return arguments


def disk_free_bytes(path: Path) -> int:
    probe = path.resolve()
    while not probe.exists() and probe != probe.parent:
        probe = probe.parent
    return shutil.disk_usage(probe).free


def format_status(return_code: int) -> str:
    unsigned = return_code & 0xFFFFFFFF
    known_statuses = {
        0xC000007F: "STATUS_DISK_FULL",
        0xC0000409: "STATUS_STACK_BUFFER_OVERRUN",
    }
    suffix = (
        f" windows_status={known_statuses[unsigned]}"
        if unsigned in known_statuses
        else ""
    )
    return f"decimal={return_code} unsigned_hex=0x{unsigned:08X}{suffix}"


def local_name(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def parse_windows_application_errors(
    payload: bytes, executable_name: str
) -> list[dict[str, str]]:
    if payload.startswith((b"\xff\xfe", b"\xfe\xff")):
        text = payload.decode("utf-16", errors="replace")
    else:
        text = payload.decode("utf-8", errors="replace")
    text = re.sub(r"<\?xml[^>]*\?>", "", text).strip()
    if not text:
        return []
    try:
        root = ET.fromstring(text)
    except ET.ParseError:
        try:
            root = ET.fromstring(f"<Events>{text}</Events>")
        except ET.ParseError:
            return []

    wanted = executable_name.casefold()
    records: list[dict[str, str]] = []
    events = (
        [root]
        if local_name(root.tag) == "Event"
        else [
            element for element in root.iter() if local_name(element.tag) == "Event"
        ]
    )
    for event in events:
        values = {
            element.attrib["Name"]: element.text or ""
            for element in event.iter()
            if local_name(element.tag) == "Data" and "Name" in element.attrib
        }
        app_name = values.get("AppName", "")
        if Path(app_name).name.casefold() != wanted:
            continue
        records.append(
            {
                "app": Path(app_name).name,
                "module": Path(values.get("ModuleName", "")).name,
                "exception": values.get("ExceptionCode", ""),
                "offset": values.get("FaultingOffset", ""),
            }
        )
    return records


def report_windows_application_errors(label: str, executable_name: str) -> None:
    if os.name != "nt":
        return
    for attempt in range(6):
        try:
            query = subprocess.run(
                [
                    "wevtutil",
                    "qe",
                    "Application",
                    "/q:*[System[(EventID=1000)]]",
                    "/f:xml",
                    "/rd:true",
                    "/c:20",
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                check=False,
            )
        except OSError:
            return
        records = parse_windows_application_errors(query.stdout, executable_name)
        if records:
            for record in records[:3]:
                print(
                    f"{label}: application_error app={record['app']} "
                    f"module={record['module']} exception={record['exception']} "
                    f"offset={record['offset']}",
                    flush=True,
                )
            return
        if attempt != 5:
            time.sleep(2)


def configure_windows_local_dumps(executable_name: str, dump_directory: Path) -> bool:
    if os.name != "nt":
        return False
    configured = os.environ.get("GDPP_WINDOWS_LOCAL_DUMPS", "").strip()
    if configured != "1":
        return False
    try:
        import winreg

        dump_directory.mkdir(parents=True, exist_ok=True)
        for existing in dump_directory.glob("*.dmp"):
            existing.unlink()
        key_path = (
            r"Software\Microsoft\Windows\Windows Error Reporting\LocalDumps"
            rf"\{executable_name}"
        )
        with winreg.CreateKeyEx(
            winreg.HKEY_CURRENT_USER,
            key_path,
            0,
            winreg.KEY_SET_VALUE,
        ) as key:
            winreg.SetValueEx(
                key,
                "DumpFolder",
                0,
                winreg.REG_EXPAND_SZ,
                str(dump_directory.resolve()),
            )
            winreg.SetValueEx(key, "DumpType", 0, winreg.REG_DWORD, 2)
            winreg.SetValueEx(key, "DumpCount", 0, winreg.REG_DWORD, 1)
    except (OSError, ImportError):
        return False
    return True


def find_windows_debugger() -> Path | None:
    if os.name != "nt":
        return None
    from_path = shutil.which("cdb.exe")
    if from_path:
        return Path(from_path)
    roots = [
        os.environ.get("ProgramFiles(x86)", ""),
        os.environ.get("ProgramFiles", ""),
    ]
    for root in roots:
        if not root:
            continue
        candidate = Path(root) / "Windows Kits" / "10" / "Debuggers" / "x64" / "cdb.exe"
        if candidate.is_file():
            return candidate
    return None


def safe_windows_dump_evidence(output: str) -> list[str]:
    fields = (
        "PROCESS_NAME:",
        "EXCEPTION_CODE:",
        "ERROR_CODE:",
        "EXCEPTION_STR:",
        "SYMBOL_NAME:",
        "MODULE_NAME:",
        "IMAGE_NAME:",
        "FAILURE_BUCKET_ID:",
    )
    evidence: list[str] = []
    in_stack = False
    for raw_line in output.splitlines():
        line = raw_line.strip()
        if line == "STACK_TEXT:":
            evidence.append(line)
            in_stack = True
            continue
        if in_stack and re.match(r"^[0-9a-fA-F`]{8,}\s+", line):
            evidence.append(line)
            continue
        if in_stack and not line:
            in_stack = False
            continue
        if line.startswith(fields):
            evidence.append(line)
    return evidence[:80]


def report_windows_local_dump(label: str, dump_directory: Path) -> None:
    if os.name != "nt":
        return
    dumps: list[Path] = []
    for attempt in range(10):
        dumps = sorted(dump_directory.glob("*.dmp"), key=lambda path: path.stat().st_mtime)
        if dumps:
            break
        if attempt != 9:
            time.sleep(1)
    if not dumps:
        print(f"{label}: local_dump=missing", flush=True)
        return
    dump = dumps[-1]
    print(f"{label}: local_dump=captured bytes={dump.stat().st_size}", flush=True)
    debugger = find_windows_debugger()
    if debugger is None:
        print(f"{label}: local_dump_debugger=unavailable", flush=True)
        return
    try:
        analysis = subprocess.run(
            [
                str(debugger),
                "-z",
                str(dump),
                "-c",
                "!analyze -v; .ecxr; k; q",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
            timeout=120,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        print(f"{label}: local_dump_analysis=failed", flush=True)
        return
    evidence = safe_windows_dump_evidence(analysis.stdout)
    print(
        f"{label}: local_dump_analysis="
        f"{'available' if evidence else 'no-safe-evidence'}",
        flush=True,
    )
    for line in evidence:
        print(f"{label}: dump_evidence {line}", flush=True)


def main() -> int:
    arguments = parse_arguments()
    arguments.log.parent.mkdir(parents=True, exist_ok=True)
    free_before = disk_free_bytes(arguments.log)
    print(f"{arguments.label}: disk_free_before={free_before}", flush=True)
    executable_name = Path(arguments.command[0]).name
    dump_directory = arguments.log.parent / "windows-local-dumps"
    dump_configured = configure_windows_local_dumps(executable_name, dump_directory)
    if dump_configured:
        print(f"{arguments.label}: local_dump_capture=enabled", flush=True)

    try:
        with arguments.log.open("wb") as log:
            process = subprocess.Popen(
                arguments.command,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            assert process.stdout is not None
            while chunk := process.stdout.read(64 * 1024):
                log.write(chunk)
                log.flush()
                sys.stdout.buffer.write(chunk)
                sys.stdout.buffer.flush()
            return_code = process.wait()
    except OSError as error:
        print(
            f"{arguments.label}: launch_error={error.__class__.__name__} "
            f"windows_error={getattr(error, 'winerror', None)} errno={error.errno}",
            file=sys.stderr,
            flush=True,
        )
        return 1

    free_after = disk_free_bytes(arguments.log)
    print(f"{arguments.label}: disk_free_after={free_after}", flush=True)
    print(
        f"{arguments.label}: process_exit {format_status(return_code)}",
        flush=True,
    )
    if return_code != 0:
        report_windows_application_errors(arguments.label, executable_name)
        if dump_configured:
            report_windows_local_dump(arguments.label, dump_directory)
    return 0 if return_code == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
