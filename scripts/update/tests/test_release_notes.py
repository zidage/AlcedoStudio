#!/usr/bin/env python3

from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch


UPDATE_SCRIPTS = Path(__file__).resolve().parents[1]
TEST_TEMP_ROOT = Path(__file__).resolve().parents[3] / "build" / "tmp" / "update" / "release-notes-tests"
TEST_TEMP_ROOT.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(UPDATE_SCRIPTS))

from export_release_prs import (  # noqa: E402
    inclusion_reasons,
    infer_pr_interval,
    pr_numbers_from_subjects,
)
import create_update_manifest  # noqa: E402
from release_notes import (  # noqa: E402
    ReleaseNotesError,
    load_release_notes,
    load_release_notes_file,
    release_notes_path,
    stamp_build_heading,
    validate_release_notes,
    version_notes_path,
)


class ReleaseNotesTest(unittest.TestCase):
    def test_build_number_selects_plain_text_file(self) -> None:
        directory = Path("docs/changelog")
        self.assertEqual(
            release_notes_path(2014, "en", directory), directory / "2014.en.txt"
        )
        self.assertEqual(
            release_notes_path(2014, "zh-CN", directory),
            directory / "2014.zh-CN.txt",
        )
        self.assertEqual(
            version_notes_path("0.2.9", "en", directory),
            directory / "0.2.9.en.txt",
        )

    def test_valid_notes_load_without_final_manifest_newline(self) -> None:
        text = (
            "Alcedo Studio 0.2.9 (Build 2014)\n\n"
            "Updates\n"
            "-------\n"
            "- Update details are shown inside Settings.\n"
        )
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as root:
            path = Path(root) / "2014.en.txt"
            path.write_text(text, encoding="utf-8", newline="\n")
            self.assertEqual(
                load_release_notes_file(path, "0.2.9", 2014, "en"), text.rstrip("\n")
            )

    def test_bilingual_notes_are_loaded_together(self) -> None:
        english = "Alcedo Studio 0.2.9 (Build 2014)\n\nUpdates\n-------\n- Item.\n"
        chinese = "Alcedo Studio 0.2.9（构建 2014）\n\n更新内容\n--------\n- 项目。\n"
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as root:
            directory = Path(root)
            (directory / "2014.en.txt").write_text(
                english, encoding="utf-8", newline="\n"
            )
            (directory / "2014.zh-CN.txt").write_text(
                chinese, encoding="utf-8", newline="\n"
            )
            self.assertEqual(
                load_release_notes("0.2.9", 2014, directory),
                {"en": english.rstrip("\n"), "zh-CN": chinese.rstrip("\n")},
            )

    def test_missing_language_prevents_pair_from_loading(self) -> None:
        english = "Alcedo Studio 0.2.9 (Build 2014)\n\nUpdates\n-------\n- Item.\n"
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as root:
            directory = Path(root)
            (directory / "2014.en.txt").write_text(
                english, encoding="utf-8", newline="\n"
            )
            with self.assertRaisesRegex(ReleaseNotesError, "2014.zh-CN.txt"):
                load_release_notes("0.2.9", 2014, directory)

    def test_version_notes_are_stamped_with_the_platform_build(self) -> None:
        english = "Alcedo Studio 0.2.9\n\nUpdates\n-------\n- Item.\n"
        chinese = "Alcedo Studio 0.2.9\n\n更新内容\n--------\n- 项目。\n"
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as root:
            directory = Path(root)
            (directory / "0.2.9.en.txt").write_text(english, encoding="utf-8", newline="\n")
            (directory / "0.2.9.zh-CN.txt").write_text(
                chinese, encoding="utf-8", newline="\n"
            )
            loaded = load_release_notes("0.2.9", 2005, directory)
        self.assertEqual(
            loaded["en"],
            stamp_build_heading(english, "0.2.9", 2005, "en"),
        )
        self.assertTrue(loaded["en"].startswith("Alcedo Studio 0.2.9 (Build 2005)"))
        self.assertTrue(loaded["zh-CN"].startswith("Alcedo Studio 0.2.9（构建 2005）"))

    def test_build_specific_pair_wins_over_version_pair(self) -> None:
        version_en = "Alcedo Studio 0.2.9\n\nUpdates\n-------\n- Version item.\n"
        version_zh = "Alcedo Studio 0.2.9\n\n更新内容\n--------\n- 版本项目。\n"
        build_en = "Alcedo Studio 0.2.9 (Build 2006)\n\nUpdates\n-------\n- Hotfix item.\n"
        build_zh = "Alcedo Studio 0.2.9（构建 2006）\n\n更新内容\n--------\n- 热修复项目。\n"
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as root:
            directory = Path(root)
            (directory / "0.2.9.en.txt").write_text(version_en, encoding="utf-8", newline="\n")
            (directory / "0.2.9.zh-CN.txt").write_text(
                version_zh, encoding="utf-8", newline="\n"
            )
            (directory / "2006.en.txt").write_text(build_en, encoding="utf-8", newline="\n")
            (directory / "2006.zh-CN.txt").write_text(
                build_zh, encoding="utf-8", newline="\n"
            )
            loaded = load_release_notes("0.2.9", 2006, directory)
        self.assertIn("Hotfix item.", loaded["en"])

    def test_version_heading_is_accepted_without_a_build(self) -> None:
        validate_release_notes(
            "Alcedo Studio 0.2.9\n\nUpdates\n-------\n- Item.\n",
            "0.2.9",
            None,
            "en",
        )

    def test_wrong_build_heading_is_rejected(self) -> None:
        text = "Alcedo Studio 0.2.9 (Build 2013)\n\nUpdates\n-------\n- Item.\n"
        with self.assertRaisesRegex(ReleaseNotesError, "first line"):
            validate_release_notes(text, "0.2.9", 2014, "en")

    def test_chinese_heading_must_be_localized(self) -> None:
        text = "Alcedo Studio 0.2.9 (Build 2014)\n\n更新内容\n--------\n- 项目。\n"
        with self.assertRaisesRegex(ReleaseNotesError, "构建 2014"):
            validate_release_notes(text, "0.2.9", 2014, "zh-CN")

    def test_long_line_is_rejected(self) -> None:
        text = "Alcedo Studio 0.2.9 (Build 2014)\n\n" + ("x" * 89) + "\n"
        with self.assertRaisesRegex(ReleaseNotesError, "exceeds 88"):
            validate_release_notes(text, "0.2.9", 2014)

    def test_pr_numbers_are_inferred_from_merge_and_squash_subjects(self) -> None:
        numbers = pr_numbers_from_subjects(
            [
                "Merge pull request #81 from zidage/export",
                "Improve update settings (#82)",
                "Regular commit without a PR number",
            ]
        )
        self.assertEqual(numbers, {81, 82})

    def test_pr_is_selected_from_merge_or_branch_commit_membership(self) -> None:
        pull_request = {
            "number": 82,
            "mergeCommit": {"oid": "merge-oid"},
            "commits": [{"oid": "branch-oid"}, {"oid": "other"}],
        }
        reasons = inclusion_reasons(
            pull_request, {"merge-oid", "branch-oid"}, {82}
        )
        self.assertEqual(
            reasons,
            [
                "merge commit is in the build range",
                "PR branch commit is in the build range",
                "a build-range commit names this PR",
            ],
        )

    @patch("export_release_prs.history_pr_numbers")
    def test_direct_commit_only_range_has_no_pr_interval(self, history: object) -> None:
        history.side_effect = [{81}, {81}]
        self.assertIsNone(infer_pr_interval("old", "new"))


class ManifestGenerationTest(unittest.TestCase):
    def test_manifest_embeds_both_languages_without_release_note_arguments(self) -> None:
        changelogs = {
            "en": "Alcedo Studio 0.2.9 (Build 2014)\n\nUpdates\n-------\n- Item.",
            "zh-CN": "Alcedo Studio 0.2.9（构建 2014）\n\n更新内容\n--------\n- 项目。",
        }
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as root:
            directory = Path(root)
            package = directory / "alcedo.exe"
            output = directory / "manifest.json"
            package.write_bytes(b"package")
            arguments = [
                "create_update_manifest.py",
                "--version",
                "0.2.9",
                "--build",
                "2014",
                "--sequence",
                "2014",
                "--tag",
                "v0.2.9",
                "--commit",
                "0123456789abcdef0123456789abcdef01234567",
                "--windows",
                str(package),
                "--output",
                str(output),
            ]
            with patch.object(sys, "argv", arguments), patch.object(
                create_update_manifest, "load_release_notes", return_value=changelogs
            ):
                self.assertEqual(create_update_manifest.main(), 0)

            manifest = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(manifest["changelogs"], changelogs)
            self.assertEqual(manifest["changelog"], changelogs["en"])
            self.assertEqual(
                manifest["commit"], "0123456789abcdef0123456789abcdef01234567"
            )
            self.assertTrue(
                manifest["artifacts"]["windows-x86_64"]["url"].startswith(
                    "https://static.aoraw.org/updates/v1/stable/builds/2014/windows-x86_64/"
                )
            )
            self.assertNotIn("notesUrl", manifest)


if __name__ == "__main__":
    unittest.main()
