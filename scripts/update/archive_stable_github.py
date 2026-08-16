#!/usr/bin/env python3
"""Archive an already-live stable update as a GitHub release.

This script does not publish an update. It never writes to R2. It only reads
the public stable feeds, verifies both platform manifests, and records that
already-shipped pair on GitHub.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request

from release_git import require_commit_on_origin_main, validate_commit
from release_notes import notes_body


REPO_ROOT = Path(__file__).resolve().parents[2]
PUBLIC_BASE = "https://static.aoraw.org"
STABLE_FEED = f"{PUBLIC_BASE}/updates/v1/stable"
PLATFORMS = ("windows-x86_64", "macos-arm64")
WEBSITE = "https://aoraw.org"


class ArchiveError(RuntimeError):
    """Raised when the live stable pair cannot be archived."""


def fetch_bytes(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"Cache-Control": "no-cache"})
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            return response.read()
    except urllib.error.URLError as error:
        raise ArchiveError(f"failed to download {url}: {error}") from error


def load_json(url: str) -> dict[str, object]:
    try:
        return json.loads(fetch_bytes(url).decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ArchiveError(f"{url} is not valid JSON: {error}") from error


def live_urls(platform: str) -> tuple[str, str]:
    prefix = f"{STABLE_FEED}/{platform}"
    return f"{prefix}/manifest.json", f"{prefix}/manifest.json.sig"


def required_text(value: object, label: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise ArchiveError(f"stable manifest is missing {label}")
    return value


def required_int(value: object, label: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < 1:
        raise ArchiveError(f"stable manifest has an invalid {label}")
    return value


def artifact_object(manifest: dict[str, object], platform: str) -> dict[str, object]:
    artifacts = manifest.get("artifacts")
    if not isinstance(artifacts, dict) or set(artifacts) != {platform}:
        raise ArchiveError(
            f"stable {platform} manifest must contain only the {platform} artifact"
        )
    item = artifacts.get(platform)
    if not isinstance(item, dict):
        raise ArchiveError(f"stable {platform} artifact is missing")
    return item


def changelogs_of(manifest: dict[str, object], platform: str) -> dict[str, str]:
    changelogs = manifest.get("changelogs")
    if not isinstance(changelogs, dict) or set(changelogs) != {"en", "zh-CN"}:
        raise ArchiveError(f"stable {platform} manifest is missing bilingual notes")
    parsed = {language: required_text(changelogs.get(language), f"{platform} {language} notes")
              for language in ("en", "zh-CN")}
    if manifest.get("changelog") != parsed["en"]:
        raise ArchiveError(f"stable {platform} English fallback does not match changelogs.en")
    return parsed


def inspect_manifest(manifest: dict[str, object], platform: str) -> dict[str, object]:
    version = required_text(manifest.get("version"), f"{platform} version")
    build = required_int(manifest.get("build"), f"{platform} build")
    try:
        commit = validate_commit(str(manifest.get("commit", "")))
    except SystemExit as error:
        raise ArchiveError(f"stable {platform} manifest: {error}") from error
    artifact = artifact_object(manifest, platform)
    package_url = required_text(artifact.get("url"), f"{platform} package URL")
    expected_prefix = f"{STABLE_FEED}/builds/{build}/{platform}/"
    if not package_url.startswith(expected_prefix):
        raise ArchiveError(f"stable {platform} package is not under {expected_prefix}")
    manual_url = artifact.get("manualUrl")
    if manual_url is not None:
        manual_url = required_text(manual_url, f"{platform} manual URL")
        if not manual_url.startswith(expected_prefix):
            raise ArchiveError(f"stable {platform} manual package is not under {expected_prefix}")
    return {
        "platform": platform,
        "version": version,
        "build": build,
        "commit": commit,
        "package_url": package_url,
        "manual_url": manual_url,
        "changelogs": changelogs_of(manifest, platform),
    }


def pair_stable_manifests(
    windows: dict[str, object], macos: dict[str, object], expected_version: str
) -> dict[str, object]:
    left = inspect_manifest(windows, "windows-x86_64")
    right = inspect_manifest(macos, "macos-arm64")
    if left["version"] != right["version"]:
        raise ArchiveError(
            f"stable platforms do not share a version: Windows {left['version']} "
            f"vs macOS {right['version']}"
        )
    if expected_version and left["version"] != expected_version:
        raise ArchiveError(
            f"live stable version is {left['version']}, not the requested {expected_version}"
        )
    if left["commit"] != right["commit"]:
        raise ArchiveError(
            f"stable platforms do not share a commit: Windows {left['commit']} "
            f"vs macOS {right['commit']}"
        )
    return {
        "version": left["version"],
        "commit": left["commit"],
        "windows_build": left["build"],
        "macos_build": right["build"],
        "windows": left,
        "macos": right,
    }


def notes_source_urls(commit: str, version: str, windows_build: int, macos_build: int) -> list[str]:
    base = f"https://github.com/zidage/AlcedoStudio/blob/{commit}/docs/changelog"
    version_en = REPO_ROOT / "docs" / "changelog" / f"{version}.en.txt"
    if version_en.is_file():
        return [
            f"{base}/{version}.en.txt",
            f"{base}/{version}.zh-CN.txt",
        ]
    links = [
        f"{base}/{windows_build}.en.txt",
        f"{base}/{windows_build}.zh-CN.txt",
    ]
    if macos_build != windows_build:
        links.extend(
            [
                f"{base}/{macos_build}.en.txt",
                f"{base}/{macos_build}.zh-CN.txt",
            ]
        )
    return links


def compose_github_release(
    pair: dict[str, object],
) -> tuple[str, str]:
    version = str(pair["version"])
    windows_build = int(pair["windows_build"])
    macos_build = int(pair["macos_build"])
    commit = str(pair["commit"])
    heading = f"Alcedo Studio {version} (windows {windows_build}/macOS {macos_build})"
    windows_notes = pair["windows"]["changelogs"]
    macos_notes = pair["macos"]["changelogs"]
    if not isinstance(windows_notes, dict) or not isinstance(macos_notes, dict):
        raise ArchiveError("stable notes are missing after pairing")

    english_body = notes_body(str(windows_notes["en"]))
    chinese_body = notes_body(str(windows_notes["zh-CN"]))
    if notes_body(str(macos_notes["en"])) != english_body or notes_body(
        str(macos_notes["zh-CN"])
    ) != chinese_body:
        english_body = (
            f"Windows (build {windows_build})\n"
            f"{notes_body(str(windows_notes['en']))}\n\n"
            f"macOS (build {macos_build})\n"
            f"{notes_body(str(macos_notes['en']))}"
        )
        chinese_body = (
            f"Windows（构建 {windows_build}）\n"
            f"{notes_body(str(windows_notes['zh-CN']))}\n\n"
            f"macOS（构建 {macos_build}）\n"
            f"{notes_body(str(macos_notes['zh-CN']))}"
        )

    downloads = [
        str(pair["windows"]["package_url"]),
        str(pair["macos"]["manual_url"] or pair["macos"]["package_url"]),
    ]
    note_links = notes_source_urls(commit, version, windows_build, macos_build)
    body = "\n".join(
        [
            heading,
            "",
            english_body,
            "",
            heading,
            "",
            chinese_body,
            "",
            "Official downloads",
            "------------------",
            f"- Website: {WEBSITE}",
            f"- Windows: {downloads[0]}",
            f"- macOS: {downloads[1]}",
            "",
            "Source notes",
            "------------",
            *[f"- {link}" for link in note_links],
            "",
        ]
    )
    return heading, body


def run(command: list[str], *, dry_run: bool) -> None:
    print("+", " ".join(command))
    if dry_run:
        return
    subprocess.run(command, cwd=REPO_ROOT, check=True)


def release_exists(repo: str, tag: str) -> bool:
    result = subprocess.run(
        ["gh", "release", "view", tag, "--repo", repo],
        cwd=REPO_ROOT,
        check=False,
        capture_output=True,
        text=True,
    )
    return result.returncode == 0


def existing_tag_commit(tag: str) -> str | None:
    result = subprocess.run(
        ["git", "rev-parse", "--verify", f"{tag}^{{commit}}"],
        cwd=REPO_ROOT,
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if result.returncode != 0:
        return None
    return result.stdout.strip().lower()


def download_named(url: str, directory: Path) -> Path:
    name = Path(url).name
    path = directory / name
    path.write_bytes(fetch_bytes(url))
    return path


def archive_pair(
    pair: dict[str, object],
    *,
    repo: str,
    dry_run: bool,
    fetch_manifest,
    fetch_signature,
    public_key: str,
) -> int:
    version = str(pair["version"])
    commit = str(pair["commit"])
    tag = f"v{version}"
    title, body = compose_github_release(pair)
    require_commit_on_origin_main(REPO_ROOT, commit)

    work_root = REPO_ROOT / "build" / "tmp" / "update" / "archive-stable"
    work_root.mkdir(parents=True, exist_ok=True)
    notes_path = work_root / f"{version}-github-notes.txt"
    notes_path.write_text(body, encoding="utf-8", newline="\n")
    print(title)
    print(notes_path)

    tagged = existing_tag_commit(tag)
    if tagged and tagged != commit:
        raise ArchiveError(f"tag {tag} already points at {tagged}, not {commit}")

    with tempfile.TemporaryDirectory(prefix="alcedo-stable-archive-", dir=work_root) as directory:
        root = Path(directory)
        assets: list[Path] = []
        for platform in PLATFORMS:
            manifest_url, signature_url = live_urls(platform)
            platform_dir = root / platform
            platform_dir.mkdir()
            manifest_path = platform_dir / "update-manifest.json"
            signature_path = platform_dir / "update-manifest.json.sig"
            manifest_path.write_bytes(fetch_manifest(manifest_url))
            signature_path.write_bytes(fetch_signature(signature_url))
            item = pair["windows" if platform.startswith("windows") else "macos"]
            package = download_named(str(item["package_url"]), platform_dir) if not dry_run else platform_dir / Path(str(item["package_url"])).name
            if not dry_run:
                assets.append(package)
                if item["manual_url"]:
                    assets.append(download_named(str(item["manual_url"]), platform_dir))
            verify = [
                sys.executable,
                str(REPO_ROOT / "scripts" / "update" / "verify_release_manifest.py"),
                "--manifest",
                str(manifest_path),
                "--signature",
                str(signature_path),
                "--public-key-base64",
                public_key,
                "--artifacts",
                str(platform_dir),
                "--channel",
                "stable",
                "--platform",
                platform,
            ]
            if dry_run:
                print("+", " ".join(verify), "(skipped download verify)")
            else:
                print("+", " ".join(verify))
                subprocess.run(verify, check=True)

        if tagged is None:
            run(
                [
                    "git",
                    "tag",
                    "-a",
                    tag,
                    commit,
                    "-m",
                    title,
                ],
                dry_run=dry_run,
            )
            run(["git", "push", "origin", tag], dry_run=dry_run)

        if dry_run:
            print(f"Dry run complete. Would archive {tag} from {commit} without publishing.")
            return 0

        if release_exists(repo, tag):
            run(
                [
                    "gh",
                    "release",
                    "edit",
                    tag,
                    "--repo",
                    repo,
                    "--title",
                    title,
                    "--notes-file",
                    str(notes_path),
                    "--prerelease=false",
                    "--draft=false",
                ],
                dry_run=False,
            )
            if assets:
                run(
                    [
                        "gh",
                        "release",
                        "upload",
                        tag,
                        "--repo",
                        repo,
                        "--clobber",
                        *[str(path) for path in assets],
                    ],
                    dry_run=False,
                )
        else:
            run(
                [
                    "gh",
                    "release",
                    "create",
                    tag,
                    "--repo",
                    repo,
                    "--title",
                    title,
                    "--notes-file",
                    str(notes_path),
                    *[str(path) for path in assets],
                ],
                dry_run=False,
            )
    print(f"Archived live stable {version} as {tag}.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Create or update a GitHub release from the already-live stable update feeds. "
            "This does not upload to R2 and does not read a beta feed."
        )
    )
    parser.add_argument("--repo", default="zidage/AlcedoStudio")
    parser.add_argument("--version", default="", help="Require this live marketing version.")
    parser.add_argument("--public-key-base64", default="")
    parser.add_argument("--public-key-file", type=Path, default=REPO_ROOT / "alcedo_studio" / "src" / "config" / "update_public_key.txt")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--windows-manifest", type=Path)
    parser.add_argument("--macos-manifest", type=Path)
    args = parser.parse_args()

    public_key = args.public_key_base64.strip()
    if not public_key:
        if not args.public_key_file.is_file():
            raise SystemExit("a public key is required to archive a stable update")
        public_key = args.public_key_file.read_text(encoding="utf-8").strip()

    try:
        if args.windows_manifest or args.macos_manifest:
            if not args.windows_manifest or not args.macos_manifest:
                raise ArchiveError("both --windows-manifest and --macos-manifest are required together")
            windows = json.loads(args.windows_manifest.read_text(encoding="utf-8"))
            macos = json.loads(args.macos_manifest.read_text(encoding="utf-8"))
            fetch_manifest = fetch_bytes
            fetch_signature = fetch_bytes
        else:
            windows = load_json(live_urls("windows-x86_64")[0])
            macos = load_json(live_urls("macos-arm64")[0])
            fetch_manifest = fetch_bytes
            fetch_signature = fetch_bytes
        pair = pair_stable_manifests(windows, macos, args.version)
        return archive_pair(
            pair,
            repo=args.repo,
            dry_run=args.dry_run,
            fetch_manifest=fetch_manifest,
            fetch_signature=fetch_signature,
            public_key=public_key,
        )
    except ArchiveError as error:
        raise SystemExit(str(error)) from error


if __name__ == "__main__":
    raise SystemExit(main())
