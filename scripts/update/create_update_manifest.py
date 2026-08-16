#!/usr/bin/env python3
"""Create a deterministic Alcedo Studio update manifest."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path

from release_git import validate_commit
from release_notes import ReleaseNotesError, load_release_notes


PUBLIC_BASE = "https://static.aoraw.org"


def file_digest(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def artifact(path: Path, url: str) -> dict[str, object]:
    return {"url": url, "sha256": file_digest(path), "size": path.stat().st_size}


def artifact_url(channel: str, build: int, platform: str, filename: str,
                 public_base: str = PUBLIC_BASE) -> str:
    return (
        f"{public_base.rstrip('/')}/updates/v1/{channel}/builds/{build}/"
        f"{platform}/{filename}"
    )


def utc_text(value: dt.datetime) -> str:
    return value.astimezone(dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--build", required=True, type=int)
    parser.add_argument("--sequence", required=True, type=int)
    parser.add_argument("--commit", required=True, help="Full 40-character git SHA packaged for this upload.")
    parser.add_argument(
        "--tag",
        default="",
        help="Ignored. Kept so older callers that pass --tag do not break.",
    )
    parser.add_argument("--windows", type=Path)
    parser.add_argument("--macos-arm64", type=Path)
    parser.add_argument("--macos-dmg", type=Path)
    parser.add_argument("--output", default="update-manifest.json", type=Path)
    parser.add_argument("--public-base", default=PUBLIC_BASE)
    parser.add_argument(
        "--base-url",
        default="",
        help="Ignored. Package URLs always live under /updates/v1/<channel>/builds/.",
    )
    parser.add_argument(
        "--beta-base-url",
        default="",
        help="Ignored. Package URLs always live under /updates/v1/<channel>/builds/.",
    )
    parser.add_argument("--channel", choices=("stable", "beta"), default="stable")
    parser.add_argument("--valid-days", default=30, type=int)
    parser.add_argument("--notes-dir", type=Path, default=None)
    args = parser.parse_args()

    if args.build < 1 or args.sequence < 1 or not 1 <= args.valid_days <= 90:
        parser.error("build and sequence must be positive; valid-days must be from 1 to 90")
    commit = validate_commit(args.commit)
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
    if args.macos_dmg is not None:
        if "macos-arm64" not in selected_paths:
            parser.error("--macos-dmg requires --macos-arm64")
        if not args.macos_dmg.is_file() or args.macos_dmg.stat().st_size < 1:
            parser.error(f"manual macOS package does not exist or is empty: {args.macos_dmg}")

    try:
        if args.notes_dir is None:
            changelogs = load_release_notes(args.version, args.build)
        else:
            changelogs = load_release_notes(args.version, args.build, args.notes_dir)
    except ReleaseNotesError as error:
        parser.error(str(error))

    now = dt.datetime.now(dt.timezone.utc)
    artifacts: dict[str, dict[str, object]] = {}
    for key, path in selected_paths.items():
        item = artifact(
            path,
            artifact_url(args.channel, args.build, key, path.name, args.public_base),
        )
        if key == "macos-arm64" and args.macos_dmg is not None:
            item["manualUrl"] = artifact_url(
                args.channel, args.build, key, args.macos_dmg.name, args.public_base
            )
            item["manualSha256"] = file_digest(args.macos_dmg)
            item["manualSize"] = args.macos_dmg.stat().st_size
        artifacts[key] = item

    manifest: dict[str, object] = {
        "schema": 1,
        "sequence": args.sequence,
        "version": args.version,
        "build": args.build,
        "commit": commit,
        "publishedAt": utc_text(now),
        "expiresAt": utc_text(now + dt.timedelta(days=args.valid_days)),
        # Keep the English string for clients released before localized notes.
        "changelog": changelogs["en"],
        "changelogs": changelogs,
        "artifacts": artifacts,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(
        (json.dumps(manifest, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")
    )
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
