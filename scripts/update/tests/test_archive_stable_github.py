#!/usr/bin/env python3
"""Archive workflow records a live stable pair and never reads a beta feed."""

from __future__ import annotations

from pathlib import Path
import sys
import unittest
import unittest.mock


UPDATE_SCRIPTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(UPDATE_SCRIPTS))

import archive_stable_github as archive  # noqa: E402


COMMIT = "0123456789abcdef0123456789abcdef01234567"
OTHER = "abcdef0123456789abcdef0123456789abcdef01"


def manifest(
    *,
    platform: str,
    version: str = "0.2.9",
    build: int,
    commit: str = COMMIT,
    notes_build: int | None = None,
    extra_platform: str | None = None,
) -> dict[str, object]:
    heading_en = f"Alcedo Studio {version} (Build {notes_build or build})"
    heading_zh = f"Alcedo Studio {version}（构建 {notes_build or build}）"
    english = f"{heading_en}\n\nUpdates\n-------\n- Shared user-visible change."
    chinese = f"{heading_zh}\n\n更新内容\n--------\n- 同一份用户可见改动。"
    prefix = (
        f"https://static.aoraw.org/updates/v1/stable/builds/{build}/{platform}/"
    )
    name = (
        "AlcedoStudio-0.2.9-Windows-AMD64.exe"
        if platform.startswith("windows")
        else "AlcedoStudio-0.2.9-Darwin-arm64.zip"
    )
    item: dict[str, object] = {
        "url": prefix + name,
        "sha256": "ab" * 32,
        "size": 12,
    }
    if platform == "macos-arm64":
        item["manualUrl"] = prefix + "AlcedoStudio-0.2.9-Darwin-arm64.dmg"
        item["manualSha256"] = "cd" * 32
        item["manualSize"] = 20
    artifacts = {platform: item}
    if extra_platform:
        artifacts[extra_platform] = item
    return {
        "schema": 1,
        "sequence": 20260816010101,
        "version": version,
        "build": build,
        "commit": commit,
        "publishedAt": "2026-08-16T01:01:01Z",
        "changelog": english,
        "changelogs": {"en": english, "zh-CN": chinese},
        "artifacts": artifacts,
    }


class ArchiveStableGithubTest(unittest.TestCase):
    def test_pairs_different_builds_from_the_same_main_commit(self) -> None:
        pair = archive.pair_stable_manifests(
            manifest(platform="windows-x86_64", build=2005),
            manifest(platform="macos-arm64", build=2001),
            "",
        )
        self.assertEqual(pair["version"], "0.2.9")
        self.assertEqual(pair["windows_commit"], COMMIT)
        self.assertEqual(pair["macos_commit"], COMMIT)
        self.assertEqual(pair["windows_build"], 2005)
        self.assertEqual(pair["macos_build"], 2001)

    def test_github_notes_name_both_builds_and_append_update_urls(self) -> None:
        pair = archive.pair_stable_manifests(
            manifest(platform="windows-x86_64", build=2005),
            manifest(platform="macos-arm64", build=2001),
            "0.2.9",
        )
        title, body = archive.compose_github_release(pair)
        self.assertEqual(title, "Alcedo Studio 0.2.9 (windows 2005/macOS 2001)")
        self.assertIn("Alcedo Studio 0.2.9 (windows 2005/macOS 2001)", body)
        self.assertIn("- Shared user-visible change.", body)
        self.assertIn("- 同一份用户可见改动。", body)
        self.assertIn("https://aoraw.org", body)
        self.assertIn(
            "https://static.aoraw.org/updates/v1/stable/builds/2005/"
            "windows-x86_64/AlcedoStudio-0.2.9-Windows-AMD64.exe",
            body,
        )
        self.assertIn(
            "https://static.aoraw.org/updates/v1/stable/builds/2001/"
            "macos-arm64/AlcedoStudio-0.2.9-Darwin-arm64.dmg",
            body,
        )
        self.assertNotIn("/releases/", body)
        self.assertNotIn("/beta/", body)

    def test_pairs_different_commits_for_the_same_version(self) -> None:
        pair = archive.pair_stable_manifests(
            manifest(platform="windows-x86_64", build=2005, commit=COMMIT),
            manifest(platform="macos-arm64", build=2001, commit=OTHER),
            "",
        )
        self.assertEqual(pair["windows_commit"], COMMIT)
        self.assertEqual(pair["macos_commit"], OTHER)
        self.assertEqual(pair["version"], "0.2.9")

    def test_tag_commit_prefers_later_published_when_history_is_unrelated(self) -> None:
        pair = archive.pair_stable_manifests(
            manifest(platform="windows-x86_64", build=2005, commit=COMMIT),
            manifest(platform="macos-arm64", build=2001, commit=OTHER),
            "",
        )
        pair["windows"]["published_at"] = "2026-08-16T01:00:00Z"
        pair["macos"]["published_at"] = "2026-08-17T01:00:00Z"

        def fake_newer(_repo: object, _left: str, _right: str) -> str | None:
            return None

        with unittest.mock.patch("archive_stable_github.newer_commit", fake_newer):
            self.assertEqual(archive.choose_tag_commit(pair, Path(".")), OTHER)

    def test_rejects_missing_commit(self) -> None:
        payload = manifest(platform="windows-x86_64", build=2005)
        del payload["commit"]
        with self.assertRaisesRegex(archive.ArchiveError, "commit"):
            archive.inspect_manifest(payload, "windows-x86_64")

    def test_rejects_combined_two_platform_manifest(self) -> None:
        payload = manifest(
            platform="windows-x86_64",
            build=2005,
            extra_platform="macos-arm64",
        )
        with self.assertRaisesRegex(archive.ArchiveError, "only the windows-x86_64"):
            archive.inspect_manifest(payload, "windows-x86_64")

    def test_rejects_requested_version_mismatch(self) -> None:
        with self.assertRaisesRegex(archive.ArchiveError, "not the requested"):
            archive.pair_stable_manifests(
                manifest(platform="windows-x86_64", build=2005),
                manifest(platform="macos-arm64", build=2001),
                "0.2.8",
            )

    def test_download_headers_override_python_urllib_user_agent(self) -> None:
        headers = archive.download_headers()
        self.assertEqual(headers["User-Agent"], archive.USER_AGENT)
        self.assertNotIn("Python-urllib", headers["User-Agent"])
        self.assertIn("Cache-Control", headers)

    def test_urllib_fetch_sends_the_archive_user_agent(self) -> None:
        captured: dict[str, str] = {}

        class FakeResponse:
            def read(self) -> bytes:
                return b"{}"

            def __enter__(self) -> FakeResponse:
                return self

            def __exit__(self, *arguments: object) -> None:
                return None

        def fake_urlopen(request: object, timeout: int = 0) -> FakeResponse:
            captured["user_agent"] = request.get_header("User-agent")  # type: ignore[attr-defined]
            captured["timeout"] = str(timeout)
            return FakeResponse()

        with unittest.mock.patch("archive_stable_github.urllib.request.urlopen", fake_urlopen):
            payload = archive.fetch_bytes_with_urllib(
                "https://static.aoraw.org/updates/v1/stable/windows-x86_64/manifest.json"
            )
        self.assertEqual(payload, b"{}")
        self.assertEqual(captured["user_agent"], archive.USER_AGENT)

    def test_live_urls_are_stable_only(self) -> None:
        manifest_url, signature_url = archive.live_urls("windows-x86_64")
        self.assertTrue(manifest_url.startswith(archive.STABLE_FEED))
        self.assertTrue(signature_url.startswith(archive.STABLE_FEED))
        self.assertNotIn("/beta/", manifest_url)
        self.assertNotIn("/releases/", manifest_url)

    def test_package_url_outside_stable_updates_is_rejected(self) -> None:
        payload = manifest(platform="windows-x86_64", build=2005)
        artifacts = payload["artifacts"]
        assert isinstance(artifacts, dict)
        item = artifacts["windows-x86_64"]
        assert isinstance(item, dict)
        item["url"] = (
            "https://static.aoraw.org/releases/v0.2.9/AlcedoStudio-0.2.9-Windows-AMD64.exe"
        )
        with self.assertRaisesRegex(archive.ArchiveError, "not under"):
            archive.inspect_manifest(payload, "windows-x86_64")


if __name__ == "__main__":
    unittest.main()
