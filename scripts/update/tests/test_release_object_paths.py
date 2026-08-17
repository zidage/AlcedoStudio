#!/usr/bin/env python3
"""Verify stable and beta release objects use isolated immutable paths."""

from __future__ import annotations

from pathlib import Path
import sys
import unittest


UPDATE_SCRIPTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(UPDATE_SCRIPTS))

from create_update_manifest import artifact_url  # noqa: E402
from publish_update import matching_object, planned_keys  # noqa: E402


class ReleaseObjectPathTest(unittest.TestCase):
    def test_uploaded_object_requires_matching_size_and_digest(self) -> None:
        head = {
            "ContentLength": 123,
            "Metadata": {"alcedo-sha256": "abc123"},
        }

        self.assertTrue(matching_object(head, 123, "abc123"))
        self.assertFalse(matching_object(head, 124, "abc123"))
        self.assertFalse(matching_object(head, 123, "different"))
        self.assertFalse(matching_object({"ContentLength": 123, "Metadata": {}}, 123, "abc123"))
        self.assertTrue(
            matching_object(
                {"ContentLength": 123, "Metadata": {}},
                123,
                "abc123",
                allow_missing_digest=True,
            )
        )

    def test_artifact_url_is_channel_build_and_platform_under_updates(self) -> None:
        beta = artifact_url(
            "beta",
            2008,
            "windows-x86_64",
            "AlcedoStudio-0.2.7-Windows-AMD64.exe",
        )
        stable = artifact_url(
            "stable",
            2008,
            "windows-x86_64",
            "AlcedoStudio-0.2.7-Windows-AMD64.exe",
        )

        self.assertEqual(
            beta,
            "https://static.aoraw.org/updates/v1/beta/builds/2008/"
            "windows-x86_64/AlcedoStudio-0.2.7-Windows-AMD64.exe",
        )
        self.assertEqual(
            stable,
            "https://static.aoraw.org/updates/v1/stable/builds/2008/"
            "windows-x86_64/AlcedoStudio-0.2.7-Windows-AMD64.exe",
        )
        self.assertNotIn("/releases/", stable)

    def test_beta_uploads_only_use_beta_build_or_live_feed_paths(self) -> None:
        keys = [
            key
            for key, _ in planned_keys(
                2008,
                20260812123456,
                "windows-x86_64",
                "AlcedoStudio-0.2.7-Windows-AMD64.exe",
                "",
                "beta",
            )
        ]

        self.assertTrue(all(key.startswith("updates/v1/beta/") for key in keys))
        self.assertIn(
            "updates/v1/beta/builds/2008/windows-x86_64/"
            "AlcedoStudio-0.2.7-Windows-AMD64.exe",
            keys,
        )
        self.assertIn(
            "updates/v1/beta/builds/2008/windows-x86_64/"
            "manifests/20260812123456/manifest.json",
            keys,
        )
        self.assertFalse(any(key.startswith("releases/") for key in keys))

    def test_stable_uploads_use_updates_paths_and_never_latest_aliases(self) -> None:
        keys = [
            key
            for key, _ in planned_keys(
                2008,
                20260812123456,
                "windows-x86_64",
                "AlcedoStudio-0.2.7-Windows-AMD64.exe",
                "",
                "stable",
            )
        ]

        self.assertTrue(all(key.startswith("updates/v1/stable/") for key in keys))
        self.assertIn(
            "updates/v1/stable/builds/2008/windows-x86_64/"
            "AlcedoStudio-0.2.7-Windows-AMD64.exe",
            keys,
        )
        self.assertIn("updates/v1/stable/windows-x86_64/manifest.json", keys)
        self.assertFalse(any(key.startswith("releases/") for key in keys))
        self.assertFalse(any("latest" in key for key in keys))


if __name__ == "__main__":
    unittest.main()
