#!/usr/bin/env python3
"""Create a deterministic Alcedo Studio update manifest."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import re
from pathlib import Path


MAXIMUM_CHANGELOG_CHARS = 16 * 1024


def artifact(path: Path, url: str) -> dict[str, object]:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return {"url": url, "sha256": digest.hexdigest(), "size": path.stat().st_size}


def artifact_url(channel: str, stable_base_url: str, beta_base_url: str, tag: str,
                 build: int, platform: str, filename: str) -> str:
    if channel == "beta":
        return f"{beta_base_url.rstrip('/')}/{build}/{platform}/{filename}"
    return f"{stable_base_url.rstrip('/')}/{tag}/{filename}"


def utc_text(value: dt.datetime) -> str:
    return value.astimezone(dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def extract_changelog(changelog_path: Path, version: str) -> str:
    text = changelog_path.read_text(encoding="utf-8")
    pattern = re.compile(
        rf"^## \[{re.escape(version)}\][^\n]*\n(.*?)(?=^## \[|\Z)",
        re.MULTILINE | re.DOTALL,
    )
    match = pattern.search(text)
    if not match:
        return ""
    body = match.group(1).strip()
    if len(body) > MAXIMUM_CHANGELOG_CHARS:
        raise SystemExit(
            f"changelog for {version} exceeds {MAXIMUM_CHANGELOG_CHARS} characters"
        )
    return body


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--build", required=True, type=int)
    parser.add_argument("--sequence", required=True, type=int)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--windows", type=Path)
    parser.add_argument("--macos-arm64", type=Path)
    parser.add_argument("--output", default="update-manifest.json", type=Path)
    parser.add_argument("--base-url", default="https://static.aoraw.org/releases")
    parser.add_argument(
        "--beta-base-url",
        default="https://static.aoraw.org/updates/v1/beta/builds",
    )
    parser.add_argument("--channel", choices=("stable", "beta"), default="stable")
    parser.add_argument("--notes-url", default="")
    parser.add_argument("--changelog-file", type=Path, default=None)
    parser.add_argument(
        "--changelog-from",
        type=Path,
        default=None,
        help="CHANGELOG.md path; extract the ## [version] section when present",
    )
    parser.add_argument("--valid-days", default=30, type=int)
    args = parser.parse_args()

    if args.build < 1 or args.sequence < 1 or not 1 <= args.valid_days <= 90:
        parser.error("build and sequence must be positive; valid-days must be from 1 to 90")
    platform_paths = {
        "windows-x86_64": args.windows,
        "macos-arm64": args.macos_arm64,
    }
    selected_paths = {key: path for key, path in platform_paths.items() if path is not None}
    if not selected_paths:
        parser.error("at least one platform artifact is required")
    for path in selected_paths.values():
        if not path.is_file() or path.stat().st_size < 1:
            parser.error(f"artifact does not exist or is empty: {path}")

    changelog = ""
    if args.changelog_file is not None:
        changelog = args.changelog_file.read_text(encoding="utf-8").strip()
        if len(changelog) > MAXIMUM_CHANGELOG_CHARS:
            parser.error(f"changelog exceeds {MAXIMUM_CHANGELOG_CHARS} characters")
    elif args.changelog_from is not None:
        changelog = extract_changelog(args.changelog_from, args.version)

    now = dt.datetime.now(dt.timezone.utc)
    manifest: dict[str, object] = {
        "schema": 1,
        "sequence": args.sequence,
        "version": args.version,
        "build": args.build,
        "publishedAt": utc_text(now),
        "expiresAt": utc_text(now + dt.timedelta(days=args.valid_days)),
        "notesUrl": args.notes_url or f"https://github.com/zidage/AlcedoStudio/releases/tag/{args.tag}",
        "artifacts": {
            key: artifact(
                path,
                artifact_url(
                    args.channel,
                    args.base_url,
                    args.beta_base_url,
                    args.tag,
                    args.build,
                    key,
                    path.name,
                ),
            )
            for key, path in selected_paths.items()
        },
    }
    if changelog:
        manifest["changelog"] = changelog

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(
        (json.dumps(manifest, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")
    )
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
