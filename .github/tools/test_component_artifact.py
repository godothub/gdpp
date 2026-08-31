#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


TOOL = Path(__file__).with_name("component_artifact.py")
SPEC = importlib.util.spec_from_file_location("gdpp_component_artifact", TOOL)
assert SPEC is not None and SPEC.loader is not None
component_artifact = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(component_artifact)

SOURCE_SHA = "1" * 40
ARTIFACT_NAME = "gdpp-host-linux-x64-" + SOURCE_SHA


class ComponentArtifactTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name) / "component"
        (self.root / "payload/include").mkdir(parents=True)
        (self.root / "payload/include/contract.hpp").write_text(
            "contract\n", encoding="utf-8", newline="\n"
        )
        (self.root / "payload/runtime.bin").write_bytes(b"\x00runtime\xff")

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def seal(self) -> None:
        component_artifact.seal(self.root, ARTIFACT_NAME, SOURCE_SHA)

    def test_round_trip_and_canonical_manifest(self) -> None:
        self.seal()
        component_artifact.verify(self.root, ARTIFACT_NAME, SOURCE_SHA)
        manifest_path = self.root / component_artifact.MANIFEST_NAME
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        self.assertEqual(manifest["source_sha"], SOURCE_SHA)
        self.assertEqual(manifest["artifact_name"], ARTIFACT_NAME)
        self.assertEqual(
            manifest["directories"], ["payload", "payload/include"]
        )
        self.assertEqual(
            [entry["path"] for entry in manifest["files"]],
            ["payload/include/contract.hpp", "payload/runtime.bin"],
        )

    def test_materializes_only_authenticated_payload(self) -> None:
        self.seal()
        destination = Path(self.temporary.name) / "authenticated"
        component_artifact.materialize(
            self.root, destination, ARTIFACT_NAME, SOURCE_SHA
        )
        self.assertEqual(
            sorted(
                path.relative_to(destination).as_posix()
                for path in destination.rglob("*")
                if path.is_file()
            ),
            ["payload/include/contract.hpp", "payload/runtime.bin"],
        )
        self.assertFalse((destination / component_artifact.MANIFEST_NAME).exists())

    def test_rejects_wrong_source_and_name(self) -> None:
        self.seal()
        with self.assertRaisesRegex(component_artifact.ContractError, "different private source"):
            component_artifact.verify(self.root, ARTIFACT_NAME, "2" * 40)
        with self.assertRaisesRegex(component_artifact.ContractError, "name does not match"):
            component_artifact.verify(
                self.root, "gdpp-host-windows-x64-" + SOURCE_SHA, SOURCE_SHA
            )

    def test_rejects_tampering_and_unexpected_files(self) -> None:
        self.seal()
        (self.root / "payload/runtime.bin").write_bytes(b"tampered")
        with self.assertRaisesRegex(component_artifact.ContractError, "content differs"):
            component_artifact.verify(self.root, ARTIFACT_NAME, SOURCE_SHA)
        (self.root / "payload/runtime.bin").write_bytes(b"\x00runtime\xff")
        (self.root / "payload/private.cpp").write_text("private\n", encoding="utf-8")
        with self.assertRaisesRegex(component_artifact.ContractError, "topology differs"):
            component_artifact.verify(self.root, ARTIFACT_NAME, SOURCE_SHA)

    def test_rejects_unexpected_empty_directories(self) -> None:
        self.seal()
        (self.root / "payload/unexpected-empty").mkdir()
        with self.assertRaisesRegex(
            component_artifact.ContractError, "directory topology differs"
        ):
            component_artifact.verify(self.root, ARTIFACT_NAME, SOURCE_SHA)

    def test_manifest_topology_is_globally_sorted(self) -> None:
        (self.root / "payload/zz").mkdir()
        (self.root / "payload.txt/nested").mkdir(parents=True)
        (self.root / "payload.txt/nested/value.bin").write_bytes(b"ordered")
        self.seal()
        component_artifact.verify(self.root, ARTIFACT_NAME, SOURCE_SHA)
        manifest = component_artifact.read_manifest(
            self.root / component_artifact.MANIFEST_NAME
        )
        self.assertEqual(manifest["directories"], sorted(manifest["directories"]))
        self.assertEqual(
            [entry["path"] for entry in manifest["files"]],
            sorted(entry["path"] for entry in manifest["files"]),
        )

    def test_rejects_symlinks(self) -> None:
        target = self.root / "payload/runtime.bin"
        link = self.root / "payload/runtime-link.bin"
        try:
            link.symlink_to(target.name)
        except OSError as error:
            self.skipTest(f"symbolic links are unavailable: {error}")
        with self.assertRaisesRegex(component_artifact.ContractError, "symbolic links"):
            component_artifact.seal(self.root, ARTIFACT_NAME, SOURCE_SHA)

    def test_rejects_noncanonical_or_duplicate_manifest(self) -> None:
        self.seal()
        manifest_path = self.root / component_artifact.MANIFEST_NAME
        original = manifest_path.read_text(encoding="utf-8")
        manifest_path.write_text(original.replace("{", "{\n", 1), encoding="utf-8")
        with self.assertRaisesRegex(component_artifact.ContractError, "not canonical"):
            component_artifact.verify(self.root, ARTIFACT_NAME, SOURCE_SHA)
        manifest_path.write_text(
            '{"artifact_name":"a","artifact_name":"b"}\n', encoding="utf-8"
        )
        with self.assertRaisesRegex(component_artifact.ContractError, "repeats key"):
            component_artifact.verify(self.root, ARTIFACT_NAME, SOURCE_SHA)


if __name__ == "__main__":
    unittest.main()
