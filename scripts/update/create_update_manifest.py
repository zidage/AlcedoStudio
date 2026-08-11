#!/usr/bin/env python3
"""Create a deterministic Alcedo Studio update manifest."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path


def artifact(path: Path, url: str) -> dict[str, object]:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return {"url": url, "sha256": digest.hexdigest(), "size": path.stat().st_size}


def utc_text(value: dt.datetime) -> str:
    return value.astimezone(dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--build", required=True, type=int)
    parser.add_argument("--sequence", required=True, type=int)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--windows", required=True, type=Path)
    parser.add_argument("--macos-arm64", required=True, type=Path)
    parser.add_argument("--output", default="update-manifest.json", type=Path)
    parser.add_argument("--base-url", default="https://static.aoraw.org/releases")
    parser.add_argument("--notes-url", default="")
    parser.add_argument("--valid-days", default=30, type=int)
    args = parser.parse_args()

    if args.build < 1 or args.sequence < 1 or not 1 <= args.valid_days <= 90:
        parser.error("build and sequence must be positive; valid-days must be from 1 to 90")
    for path in (args.windows, args.macos_arm64):
        if not path.is_file() or path.stat().st_size < 1:
            parser.error(f"artifact does not exist or is empty: {path}")

    now = dt.datetime.now(dt.timezone.utc)
    prefix = f"{args.base_url.rstrip('/')}/{args.tag}"
    manifest: dict[str, object] = {
        "schema": 1,
        "sequence": args.sequence,
        "version": args.version,
        "build": args.build,
        "publishedAt": utc_text(now),
        "expiresAt": utc_text(now + dt.timedelta(days=args.valid_days)),
        "notesUrl": args.notes_url or f"https://github.com/zidage/AlcedoStudio/releases/tag/{args.tag}",
        "artifacts": {
            "windows-x86_64": artifact(args.windows, f"{prefix}/{args.windows.name}"),
            "macos-arm64": artifact(args.macos_arm64, f"{prefix}/{args.macos_arm64.name}"),
        },
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(
        (json.dumps(manifest, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")
    )
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
