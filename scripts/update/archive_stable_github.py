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
import shutil
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request

from release_git import newer_commit, require_commit_on_origin_main, validate_commit
from release_notes import notes_body


REPO_ROOT = Path(__file__).resolve().parents[2]
PUBLIC_BASE = "https://static.aoraw.org"
STABLE_FEED = f"{PUBLIC_BASE}/updates/v1/stable"
PLATFORMS = ("windows-x86_64", "macos-arm64")
WEBSITE = "https://aoraw.org"
# Cloudflare WAF returns 403 for the default Python-urllib User-Agent.
USER_AGENT = "AlcedoStudio-archive/1.0 (+https://github.com/zidage/AlcedoStudio)"
FETCH_TIMEOUT_SECONDS = 300


class ArchiveError(RuntimeError):
    """Raised when the live stable pair cannot be archived."""


def download_headers() -> dict[str, str]:
    return {
        "User-Agent": USER_AGENT,
        "Accept": "application/json, application/octet-stream, */*;q=0.8",
        "Cache-Control": "no-cache",
    }


def fetch_bytes_with_curl(url: str, curl: str) -> bytes:
    result = subprocess.run(
        [
            curl,
            "--fail",
            "--silent",
            "--show-error",
            "--location",
            "--max-time",
            str(FETCH_TIMEOUT_SECONDS),
            "--user-agent",
            USER_AGENT,
            "--header",
            "Cache-Control: no-cache",
            "--header",
            "Accept: application/json, application/octet-stream, */*;q=0.8",
            url,
        ],
        check=False,
        capture_output=True,
    )
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", "replace").strip() or f"curl exit {result.returncode}"
        raise ArchiveError(f"failed to download {url}: {detail}")
    return result.stdout


def fetch_bytes_with_urllib(url: str) -> bytes:
    request = urllib.request.Request(url, headers=download_headers())
    try:
        with urllib.request.urlopen(request, timeout=FETCH_TIMEOUT_SECONDS) as response:
            return response.read()
    except urllib.error.HTTPError as error:
        raise ArchiveError(f"failed to download {url}: HTTP {error.code} {error.reason}") from error
    except urllib.error.URLError as error:
        raise ArchiveError(f"failed to download {url}: {error}") from error


def fetch_bytes(url: str) -> bytes:
    curl = shutil.which("curl")
    if curl:
        return fetch_bytes_with_curl(url, curl)
    return fetch_bytes_with_urllib(url)


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
        "published_at": str(manifest.get("publishedAt", "")),
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
    return {
        "version": left["version"],
        "windows_build": left["build"],
        "macos_build": right["build"],
        "windows_commit": left["commit"],
        "macos_commit": right["commit"],
        "windows": left,
        "macos": right,
    }


def notes_source_urls(
    version: str,
    windows_commit: str,
    macos_commit: str,
    windows_build: int,
    macos_build: int,
) -> list[str]:
    commits = [windows_commit]
    if macos_commit != windows_commit:
        commits.append(macos_commit)
    version_en = REPO_ROOT / "docs" / "changelog" / f"{version}.en.txt"
    links: list[str] = []
    for commit in commits:
        base = f"https://github.com/zidage/AlcedoStudio/blob/{commit}/docs/changelog"
        if version_en.is_file():
            links.extend([f"{base}/{version}.en.txt", f"{base}/{version}.zh-CN.txt"])
            continue
        build = windows_build if commit == windows_commit else macos_build
        links.extend([f"{base}/{build}.en.txt", f"{base}/{build}.zh-CN.txt"])
    return list(dict.fromkeys(links))


def choose_tag_commit(pair: dict[str, object], repo: Path) -> str:
    windows_commit = str(pair["windows_commit"])
    macos_commit = str(pair["macos_commit"])
    selected = newer_commit(repo, windows_commit, macos_commit)
    if selected:
        return selected
    windows_published = str(pair["windows"]["published_at"])
    macos_published = str(pair["macos"]["published_at"])
    if macos_published > windows_published:
        return macos_commit
    return windows_commit


def compose_github_release(
    pair: dict[str, object],
) -> tuple[str, str]:
    version = str(pair["version"])
    windows_build = int(pair["windows_build"])
    macos_build = int(pair["macos_build"])
    windows_commit = str(pair["windows_commit"])
    macos_commit = str(pair["macos_commit"])
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
    note_links = notes_source_urls(
        version, windows_commit, macos_commit, windows_build, macos_build
    )
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
    windows_commit = str(pair["windows_commit"])
    macos_commit = str(pair["macos_commit"])
    try:
        require_commit_on_origin_main(REPO_ROOT, windows_commit)
        require_commit_on_origin_main(REPO_ROOT, macos_commit)
    except SystemExit as error:
        raise ArchiveError(str(error)) from error
    tag_commit = choose_tag_commit(pair, REPO_ROOT)
    tag = f"v{version}"
    title, body = compose_github_release(pair)

    work_root = REPO_ROOT / "build" / "tmp" / "update" / "archive-stable"
    work_root.mkdir(parents=True, exist_ok=True)
    notes_path = work_root / f"{version}-github-notes.txt"
    notes_path.write_text(body, encoding="utf-8", newline="\n")
    print(title)
    print(f"Windows commit: {windows_commit}")
    print(f"macOS commit: {macos_commit}")
    print(notes_path)

    tagged = existing_tag_commit(tag)

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
                    tag_commit,
                    "-m",
                    title,
                ],
                dry_run=dry_run,
            )
            run(["git", "push", "origin", tag], dry_run=dry_run)

        if dry_run:
            print(
                f"Dry run complete. Would archive {tag} from {tag_commit} without publishing."
            )
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
