#!/usr/bin/env python3
"""Verify that update preparation reads the commit recorded by the package script."""

from __future__ import annotations

from pathlib import Path
import sys
import tempfile
import unittest


UPDATE_SCRIPTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(UPDATE_SCRIPTS))

from prepare_update import packaged_commit_stamp, read_packaged_commit  # noqa: E402


COMMIT = "0123456789abcdef0123456789abcdef01234567"


class PackagedCommitTest(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.package = Path(self.directory.name) / "AlcedoStudio-0.3.0-Darwin-arm64.zip"
        self.package.write_bytes(b"package")

    def tearDown(self) -> None:
        self.directory.cleanup()

    def test_record_sits_next_to_the_package(self) -> None:
        self.assertEqual(
            packaged_commit_stamp(self.package).name,
            "AlcedoStudio-0.3.0-Darwin-arm64.zip.commit",
        )

    def test_reads_the_recorded_commit(self) -> None:
        packaged_commit_stamp(self.package).write_text(f"{COMMIT}\n", encoding="utf-8")
        self.assertEqual(read_packaged_commit(self.package), COMMIT)

    def test_missing_record_is_refused(self) -> None:
        with self.assertRaises(SystemExit):
            read_packaged_commit(self.package)

    def test_dirty_worktree_record_is_refused(self) -> None:
        packaged_commit_stamp(self.package).write_text(f"{COMMIT}-dirty\n", encoding="utf-8")
        with self.assertRaises(SystemExit) as raised:
            read_packaged_commit(self.package)
        self.assertIn("dirty worktree", str(raised.exception))

    def test_malformed_record_is_refused(self) -> None:
        packaged_commit_stamp(self.package).write_text("main\n", encoding="utf-8")
        with self.assertRaises(SystemExit):
            read_packaged_commit(self.package)


if __name__ == "__main__":
    unittest.main()
