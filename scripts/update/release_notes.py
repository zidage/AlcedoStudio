#!/usr/bin/env python3
"""Locate and validate build-specific Alcedo Studio release notes."""

from __future__ import annotations

import argparse
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
RELEASE_NOTES_DIR = REPO_ROOT / "docs" / "changelog"
MAXIMUM_RELEASE_NOTES_CHARS = 16 * 1024
MAXIMUM_LINE_CHARS = 88
SUPPORTED_LANGUAGES = ("en", "zh-CN")


class ReleaseNotesError(ValueError):
    """Raised when tracked release notes are missing or not publishable."""


def normalize_language(language: str) -> str:
    if language not in SUPPORTED_LANGUAGES:
        raise ReleaseNotesError(
            f"unsupported release-note language: {language}; expected en or zh-CN"
        )
    return language


def release_notes_path(
    build: int, language: str, directory: Path = RELEASE_NOTES_DIR
) -> Path:
    if build < 1:
        raise ReleaseNotesError("build must be a positive integer")
    return directory / f"{build}.{normalize_language(language)}.txt"


def expected_heading(version: str, build: int, language: str) -> str:
    if normalize_language(language) == "zh-CN":
        return f"Alcedo Studio {version}（构建 {build}）"
    return f"Alcedo Studio {version} (Build {build})"


def validate_release_notes(
    text: str, version: str, build: int, language: str = "en"
) -> None:
    if not text:
        raise ReleaseNotesError("release notes are empty")
    if len(text) > MAXIMUM_RELEASE_NOTES_CHARS:
        raise ReleaseNotesError(
            f"release notes exceed {MAXIMUM_RELEASE_NOTES_CHARS} characters"
        )
    if text.startswith("\ufeff"):
        raise ReleaseNotesError("release notes must be UTF-8 without a byte-order mark")
    if "\r" in text:
        raise ReleaseNotesError("release notes must use LF line endings")
    if "\0" in text:
        raise ReleaseNotesError("release notes contain a NUL character")
    if "\t" in text:
        raise ReleaseNotesError("release notes must use spaces instead of tabs")
    if not text.endswith("\n"):
        raise ReleaseNotesError("release notes must end with a newline")

    lines = text.splitlines()
    heading = expected_heading(version, build, language)
    if not lines or lines[0] != heading:
        raise ReleaseNotesError(f"first line must be exactly: {heading}")
    if len(lines) < 3 or lines[1] != "":
        raise ReleaseNotesError("the heading must be followed by one blank line")

    for number, line in enumerate(lines, start=1):
        if line != line.rstrip():
            raise ReleaseNotesError(f"line {number} has trailing whitespace")
        if len(line) > MAXIMUM_LINE_CHARS:
            raise ReleaseNotesError(
                f"line {number} exceeds {MAXIMUM_LINE_CHARS} characters"
            )
        if line.startswith("#") or "```" in line:
            raise ReleaseNotesError(
                f"line {number} uses Markdown syntax; release notes must be plain text"
            )


def load_release_notes_file(
    path: Path, version: str, build: int, language: str = "en"
) -> str:
    if not path.is_file():
        raise ReleaseNotesError(
            f"release notes not found: {path}. Review the exported PR data and add this file "
            "before publishing."
        )
    try:
        text = path.read_bytes().decode("utf-8")
    except UnicodeDecodeError as error:
        raise ReleaseNotesError(f"release notes are not valid UTF-8: {path}") from error
    validate_release_notes(text, version, build, language)
    return text.rstrip("\n")


def load_release_notes(
    version: str, build: int, directory: Path = RELEASE_NOTES_DIR
) -> dict[str, str]:
    return {
        language: load_release_notes_file(
            release_notes_path(build, language, directory), version, build, language
        )
        for language in SUPPORTED_LANGUAGES
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate one Alcedo release-note draft.")
    parser.add_argument("--version", required=True)
    parser.add_argument("--build", required=True, type=int)
    parser.add_argument("--language", required=True, choices=SUPPORTED_LANGUAGES)
    parser.add_argument("--file", required=True, type=Path)
    args = parser.parse_args()
    try:
        load_release_notes_file(args.file, args.version, args.build, args.language)
    except ReleaseNotesError as error:
        parser.error(str(error))
    print(args.file)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
