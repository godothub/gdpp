#!/usr/bin/env python3

from __future__ import annotations

import copy
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest


TOOL = Path(__file__).with_name("publish_release_transaction.py")
SPEC = importlib.util.spec_from_file_location("gdpp_publish_release", TOOL)
assert SPEC is not None and SPEC.loader is not None
release_transaction = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release_transaction)

VERSION = "2.0.5"
SOURCE_SHA = "1" * 40
TARGET_SHA = "2" * 40


class FakeGitHub:
    def __init__(self) -> None:
        self.releases: list[dict] = []
        self.asset_bytes: dict[int, bytes] = {}
        self.next_release_id = 1
        self.next_asset_id = 100
        self.tag: str | None = None
        self.deleted_assets: list[int] = []
        self.create_count = 0

    def list_releases(self) -> list[dict]:
        return copy.deepcopy(self.releases)

    def create_release(self, payload: dict) -> dict:
        self.create_count += 1
        release = {
            **copy.deepcopy(payload),
            "id": self.next_release_id,
            "assets": [],
            "html_url": "https://example.invalid/draft",
        }
        self.next_release_id += 1
        self.releases.append(release)
        return copy.deepcopy(release)

    def update_release(self, release_id: int, payload: dict) -> dict:
        release = self._release(release_id)
        release.update(copy.deepcopy(payload))
        if release.get("draft") is False:
            self.tag = release["target_commitish"]
            release["html_url"] = "https://example.invalid/release"
        return copy.deepcopy(release)

    def get_release(self, release_id: int) -> dict:
        return copy.deepcopy(self._release(release_id))

    def upload_asset(self, tag: str, path: Path) -> None:
        release = next(value for value in self.releases if value["tag_name"] == tag)
        asset = {
            "id": self.next_asset_id,
            "name": path.name,
            "size": path.stat().st_size,
            "state": "uploaded",
        }
        self.next_asset_id += 1
        release["assets"].append(asset)
        self.asset_bytes[asset["id"]] = path.read_bytes()

    def download_asset(self, asset_id: int, destination: Path) -> None:
        destination.write_bytes(self.asset_bytes[asset_id])

    def delete_asset(self, asset_id: int) -> None:
        self.deleted_assets.append(asset_id)
        for release in self.releases:
            release["assets"] = [asset for asset in release["assets"] if asset["id"] != asset_id]
        self.asset_bytes.pop(asset_id, None)

    def tag_target(self, tag: str) -> str | None:
        del tag
        return self.tag

    def add_asset(self, release: dict, name: str, content: bytes, state: str = "uploaded") -> None:
        asset = {
            "id": self.next_asset_id,
            "name": name,
            "size": len(content),
            "state": state,
        }
        self.next_asset_id += 1
        release["assets"].append(asset)
        self.asset_bytes[asset["id"]] = content

    def _release(self, identifier: int) -> dict:
        return next(value for value in self.releases if value["id"] == identifier)


class PublishReleaseTransactionTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.assets = self.root / "assets"
        self.assets.mkdir()
        archive = b"authenticated release archive"
        (self.assets / "gdpp.zip").write_bytes(archive)
        digest = hashlib.sha256(archive).hexdigest()
        (self.assets / "SHA256SUMS").write_text(
            f"{digest}  gdpp.zip\n", encoding="ascii", newline="\n"
        )
        self.notes = self.root / "notes.md"
        self.notes.write_text("- Complete release.\n", encoding="utf-8", newline="\n")
        self.github = FakeGitHub()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def publish(self) -> dict:
        return release_transaction.publish(
            self.github,
            version=VERSION,
            source_sha=SOURCE_SHA,
            target_sha=TARGET_SHA,
            notes_path=self.notes,
            assets_directory=self.assets,
        )

    def create_bound_draft(self) -> dict:
        body = release_transaction.release_body(
            self.notes.read_text(encoding="utf-8"), VERSION, SOURCE_SHA, TARGET_SHA
        )
        return self.github.create_release(
            {
                "tag_name": VERSION,
                "target_commitish": TARGET_SHA,
                "name": VERSION,
                "body": body,
                "draft": True,
                "prerelease": False,
            }
        )

    def test_creates_verifies_and_atomically_publishes(self) -> None:
        release = self.publish()
        self.assertFalse(release["draft"])
        self.assertEqual(self.github.tag, TARGET_SHA)
        self.assertEqual(
            {asset["name"] for asset in release["assets"]},
            {"gdpp.zip", "SHA256SUMS"},
        )

    def test_resumes_matching_partial_draft_without_replacing_valid_asset(self) -> None:
        draft = self.create_bound_draft()
        archive = (self.assets / "gdpp.zip").read_bytes()
        self.github.add_asset(self.github._release(draft["id"]), "gdpp.zip", archive)
        release = self.publish()
        self.assertFalse(release["draft"])
        self.assertEqual(self.github.create_count, 1)
        self.assertEqual(self.github.deleted_assets, [])

    def test_resumes_matching_complete_draft_without_reuploading(self) -> None:
        draft = self.create_bound_draft()
        mutable = self.github._release(draft["id"])
        for name in ("gdpp.zip", "SHA256SUMS"):
            self.github.add_asset(mutable, name, (self.assets / name).read_bytes())
        next_asset_id = self.github.next_asset_id
        release = self.publish()
        self.assertFalse(release["draft"])
        self.assertEqual(self.github.next_asset_id, next_asset_id)
        self.assertEqual(self.github.deleted_assets, [])

    def test_accepts_the_drafts_matching_server_side_tag(self) -> None:
        self.create_bound_draft()
        self.github.tag = TARGET_SHA
        release = self.publish()
        self.assertFalse(release["draft"])
        self.assertEqual(self.github.tag, TARGET_SHA)

    def test_rejects_a_drafts_mismatched_server_side_tag(self) -> None:
        self.create_bound_draft()
        self.github.tag = "3" * 40
        with self.assertRaisesRegex(release_transaction.ReleaseError, "different commit"):
            self.publish()

    def test_removes_incomplete_asset_before_resuming(self) -> None:
        draft = self.create_bound_draft()
        self.github.add_asset(
            self.github._release(draft["id"]), "gdpp.zip", b"partial", state="starter"
        )
        release = self.publish()
        self.assertFalse(release["draft"])
        self.assertEqual(len(self.github.deleted_assets), 1)

    def test_rejects_mismatched_draft_identity(self) -> None:
        draft = self.create_bound_draft()
        self.github._release(draft["id"])["body"] = "forged\n"
        with self.assertRaisesRegex(release_transaction.ReleaseError, "body"):
            self.publish()

    def test_rejects_existing_asset_mismatch_or_unexpected_asset(self) -> None:
        draft = self.create_bound_draft()
        release = self.github._release(draft["id"])
        self.github.add_asset(release, "gdpp.zip", b"wrong")
        with self.assertRaisesRegex(release_transaction.ReleaseError, "differs"):
            self.publish()
        release["assets"].clear()
        self.github.asset_bytes.clear()
        self.github.add_asset(release, "diagnostics.log", b"private")
        with self.assertRaisesRegex(release_transaction.ReleaseError, "unexpected"):
            self.publish()

    def test_recovers_after_the_public_commit_response_is_lost(self) -> None:
        published = self.publish()
        next_asset_id = self.github.next_asset_id
        recovered = self.publish()
        self.assertEqual(recovered, published)
        self.assertFalse(recovered["draft"])
        self.assertEqual(self.github.next_asset_id, next_asset_id)

    def test_rejects_mismatched_public_release_and_unowned_tag(self) -> None:
        published = self.publish()
        self.github._release(published["id"])["body"] = "forged\n"
        with self.assertRaisesRegex(release_transaction.ReleaseError, "body"):
            self.publish()
        self.github.releases.clear()
        self.github.tag = "3" * 40
        with self.assertRaisesRegex(release_transaction.ReleaseError, "tag already exists"):
            self.publish()

    def test_rejects_transaction_marker_in_user_release_notes(self) -> None:
        self.notes.write_text(
            f"- Notes.\n\n{release_transaction.TRANSACTION_SCHEMA}\n",
            encoding="utf-8",
            newline="\n",
        )
        with self.assertRaisesRegex(release_transaction.ReleaseError, "collide"):
            self.publish()


if __name__ == "__main__":
    unittest.main()
