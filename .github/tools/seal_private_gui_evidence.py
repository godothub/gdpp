#!/usr/bin/env python3
"""Encrypt bounded editor-input evidence for the fixed offline recipient."""

from __future__ import annotations

import argparse
import base64
from contextlib import contextmanager
import hashlib
import io
import json
import os
from pathlib import Path
import re
import stat
import struct
import sys
from typing import Iterator
import zipfile

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.hkdf import HKDF


SCHEMA = "gdpp-private-gui-evidence-v1"
RECIPIENT_PUBLIC_KEY = bytes.fromhex(
    "6ab76180605e4563aa574b4b7f662291bbb6999c09fdb8ae5af7d58d71697660"
)
ENGINE_VERSIONS = frozenset({"4.4.1", "4.5.2", "4.6.3", "4.7.2"})
MAX_RAW_BYTES = 48 * 1024
PADDED_BYTES = 64 * 1024
FILE_NAMES = ("editor.log", "validation.json")
CONTEXT_FIELDS = frozenset({"schema", "source_sha", "pipeline_sha", "engine_version"})
HKDF_INFO = b"gdpp-private-gui-evidence-v1\x00"


class ContractError(ValueError):
    """A fixed, non-sensitive rejection category."""


def canonical_context(context: dict[str, str]) -> bytes:
    if not isinstance(context, dict) or set(context) != CONTEXT_FIELDS:
        raise ContractError("context")
    if any(not isinstance(value, str) for value in context.values()):
        raise ContractError("context")
    if context["schema"] != SCHEMA or context["engine_version"] not in ENGINE_VERSIONS:
        raise ContractError("context")
    for field in ("source_sha", "pipeline_sha"):
        if not isinstance(context[field], str) or re.fullmatch(r"[0-9a-f]{40}", context[field]) is None:
            raise ContractError("context")
    return json.dumps(context, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii")


def _absolute_path(path: Path) -> Path:
    if any(part == ".." for part in path.parts) or any(ord(char) < 32 or ord(char) == 127 for char in str(path)):
        raise ContractError("path")
    return path if path.is_absolute() else Path.cwd() / path


@contextmanager
def _open_root(root: Path) -> Iterator[int]:
    """Walk each directory with NOFOLLOW, including all ancestors."""
    if root.parts[-3:] != ("build", "ci", "editor-progress-input"):
        raise ContractError("path")
    if not hasattr(os, "O_NOFOLLOW") or os.open not in os.supports_dir_fd:
        raise ContractError("platform")
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW
    descriptor = os.open(root.anchor, flags)
    try:
        for component in root.parts[1:]:
            child = os.open(component, flags, dir_fd=descriptor)
            os.close(descriptor)
            descriptor = child
        if os.fstat(descriptor).st_uid != os.geteuid():
            raise ContractError("ownership")
        yield descriptor
    finally:
        os.close(descriptor)


def _read_regular(directory: int, name: str, limit: int) -> bytes:
    descriptor = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory)
    try:
        before = os.fstat(descriptor)
        if not stat.S_ISREG(before.st_mode) or before.st_nlink != 1 or before.st_uid != os.geteuid():
            raise ContractError("input")
        if before.st_size > limit:
            raise ContractError("size")
        with os.fdopen(descriptor, "rb", closefd=False) as stream:
            payload = stream.read(limit + 1)
        after = os.fstat(descriptor)
        if len(payload) > limit:
            raise ContractError("size")
        if (before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (
            after.st_size, after.st_mtime_ns, after.st_ctime_ns
        ) or len(payload) != after.st_size:
            raise ContractError("input")
        return payload
    finally:
        os.close(descriptor)


def collect_payload(root_descriptor: int) -> bytes:
    candidates = []
    with os.scandir(root_descriptor) as entries:
        for count, entry in enumerate(entries, 1):
            if count > 32:
                raise ContractError("input")
            if entry.name.startswith("editor-progress-input-"):
                if re.fullmatch(r"editor-progress-input-[A-Za-z0-9_-]{1,80}", entry.name) is None:
                    raise ContractError("input")
                candidates.append(entry.name)
    if len(candidates) != 1:
        raise ContractError("input")
    descriptor = os.open(candidates[0], os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=root_descriptor)
    try:
        if os.fstat(descriptor).st_uid != os.geteuid():
            raise ContractError("ownership")
        files = []
        remaining = MAX_RAW_BYTES
        for name in FILE_NAMES:
            data = _read_regular(descriptor, name, remaining)
            files.append((name, data))
            remaining -= len(data)
    finally:
        os.close(descriptor)
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", compression=zipfile.ZIP_STORED) as archive:
        for name, data in files:
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.external_attr = 0o600 << 16
            archive.writestr(info, data)
    return buffer.getvalue()


def encrypt_payload(payload: bytes, context: dict[str, str]) -> dict[str, object]:
    aad = canonical_context(context)
    if len(payload) > MAX_RAW_BYTES + 1024 or len(payload) + 4 > PADDED_BYTES:
        raise ContractError("size")
    padded = struct.pack(">I", len(payload)) + payload + os.urandom(PADDED_BYTES - 4 - len(payload))
    ephemeral = X25519PrivateKey.generate()
    ephemeral_public = ephemeral.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    salt, nonce = os.urandom(32), os.urandom(12)
    shared = ephemeral.exchange(X25519PublicKey.from_public_bytes(RECIPIENT_PUBLIC_KEY))
    key = HKDF(
        algorithm=hashes.SHA256(), length=32, salt=salt,
        info=HKDF_INFO + aad + ephemeral_public + RECIPIENT_PUBLIC_KEY,
    ).derive(shared)
    ciphertext = AESGCM(key).encrypt(nonce, padded, aad)
    return {
        "context": dict(context),
        "ephemeral_public_key": base64.b64encode(ephemeral_public).decode("ascii"),
        "salt": base64.b64encode(salt).decode("ascii"),
        "nonce": base64.b64encode(nonce).decode("ascii"),
        "ciphertext": base64.b64encode(ciphertext).decode("ascii"),
    }


def seal(root: Path, output: Path, source_sha: str, pipeline_sha: str, engine_version: str) -> str:
    context = {"schema": SCHEMA, "source_sha": source_sha, "pipeline_sha": pipeline_sha, "engine_version": engine_version}
    canonical_context(context)
    root, output = _absolute_path(root), _absolute_path(output)
    if output.parent != root or re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,79}\.json", output.name) is None:
        raise ContractError("output")
    with _open_root(root) as root_descriptor:
        envelope = encrypt_payload(collect_payload(root_descriptor), context)
        document = (json.dumps(envelope, sort_keys=True, separators=(",", ":")) + "\n").encode("ascii")
        descriptor = os.open(output.name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600, dir_fd=root_descriptor)
        try:
            with os.fdopen(descriptor, "wb", closefd=False) as stream:
                stream.write(document)
                stream.flush()
                os.fsync(descriptor)
        except BaseException:
            os.unlink(output.name, dir_fd=root_descriptor)
            raise
        finally:
            os.close(descriptor)
    return hashlib.sha256(document).hexdigest()


class SafeArgumentParser(argparse.ArgumentParser):
    def error(self, message: str) -> None:
        self.exit(2, "private-gui-evidence status=rejected category=arguments\n")


def main(argv: list[str] | None = None) -> int:
    parser = SafeArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--pipeline-sha", required=True)
    parser.add_argument("--engine-version", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        digest = seal(args.root, args.output, args.source_sha, args.pipeline_sha, args.engine_version)
    except Exception:
        print("private-gui-evidence status=rejected", file=sys.stderr)
        return 1
    print(f"private-gui-evidence status=sealed schema={SCHEMA} sha256={digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
