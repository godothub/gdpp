#!/usr/bin/env python3
"""Run a process while preserving binary output and Windows failure evidence."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
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
    return f"decimal={return_code} unsigned_hex=0x{return_code & 0xFFFFFFFF:08X}"


def main() -> int:
    arguments = parse_arguments()
    arguments.log.parent.mkdir(parents=True, exist_ok=True)
    free_before = disk_free_bytes(arguments.log)
    print(f"{arguments.label}: disk_free_before={free_before}", flush=True)

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
    return 0 if return_code == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
