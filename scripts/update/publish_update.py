#!/usr/bin/env python3
"""Validate and publish a signed Alcedo Studio update to Cloudflare R2."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


REPO_ROOT = Path(__file__).resolve().parents[2]
PUBLIC_BASE = "https://static.aoraw.org"


def load_env_file(path: Path) -> None:
    if not path.is_file():
        return
    for line in path.read_text(encoding="utf-8").splitlines():
        text = line.strip()
        if not text or text.startswith("#") or "=" not in text:
            continue
        key, value = text.split("=", 1)
        key = key.strip()
        value = value.strip().strip('"').strip("'")
        os.environ.setdefault(key, value)


def require_env(name: str) -> str:
    value = os.environ.get(name, "").strip()
    if not value:
        raise SystemExit(f"missing environment variable: {name}")
    return value


def packaged_channel(platform: str) -> str:
    build_dir = REPO_ROOT / "build" / ("release" if platform == "windows" else "macos-release")
    cache_path = build_dir / "CMakeCache.txt"
    if not cache_path.is_file():
        raise SystemExit(f"CMake cache not found: {cache_path}. Run the platform package script first.")
    for line in cache_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("ALCEDO_UPDATE_CHANNEL:") and "=" in line:
            channel = line.split("=", 1)[1].strip()
            if channel in {"stable", "beta"}:
                return channel
            raise SystemExit(f"invalid ALCEDO_UPDATE_CHANNEL in {cache_path}: {channel}")
    return "stable"


def planned_keys(tag: str, sequence: int, platform_key: str, package_name: str,
                 dmg_name: str, channel: str, promote_latest: bool) -> list[tuple[str, str]]:
    version_prefix = f"releases/{tag}"
    manifest_prefix = f"updates/v1/releases/{tag}/{sequence}/{platform_key}"
    keys = [
        (f"{version_prefix}/{package_name}", "immutable package"),
        (f"{version_prefix}/SHA256SUMS-{platform_key}.txt", "immutable checksums"),
        (f"{manifest_prefix}/manifest.json.sig", "immutable signature"),
        (f"{manifest_prefix}/manifest.json", "immutable manifest"),
        (f"updates/v1/{channel}/{platform_key}/manifest.json.sig", f"{channel} signature"),
        (f"updates/v1/{channel}/{platform_key}/manifest.json", f"{channel} manifest"),
    ]
    if dmg_name:
        keys.insert(1, (f"{version_prefix}/{dmg_name}", "immutable package"))
    # releases/latest/ is the stable website download alias; never repoint it at
    # a beta build, even if the caller left --promote-latest on.
    if channel == "stable" and promote_latest:
        latest_name = ("AlcedoStudio-Windows-x64.exe" if platform_key == "windows-x86_64"
                       else "AlcedoStudio-macos-arm64.zip")
        keys.append((f"releases/latest/{latest_name}", "latest alias"))
        if dmg_name:
            keys.append(("releases/latest/AlcedoStudio-macos-arm64.dmg", "latest alias"))
        keys.append((f"releases/latest/SHA256SUMS-{platform_key}.txt", "latest checksums"))
    return keys


def aws_upload(endpoint: str, bucket: str, source: Path, key: str, content_type: str,
               disposition: str, cache_control: str) -> None:
    command = [
        "aws",
        "s3",
        "cp",
        str(source),
        f"s3://{bucket}/{key}",
        "--endpoint-url",
        endpoint,
        "--content-type",
        content_type,
        "--content-disposition",
        disposition,
        "--cache-control",
        cache_control,
        "--no-progress",
    ]
    print("+", " ".join(command))
    subprocess.run(command, check=True)


def aws_head(endpoint: str, bucket: str, key: str) -> None:
    command = [
        "aws",
        "s3api",
        "head-object",
        "--bucket",
        bucket,
        "--key",
        key,
        "--endpoint-url",
        endpoint,
    ]
    subprocess.run(command, check=True, stdout=subprocess.DEVNULL)


def write_sha256sums(artifacts_dir: Path, names: list[str], output: Path) -> None:
    lines: list[str] = []
    for name in names:
        digest = __import__("hashlib").sha256((artifacts_dir / name).read_bytes()).hexdigest()
        lines.append(f"{digest}  {name}")
    output.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Discover, sign, verify, and publish the fixed package output for one platform."
        )
    )
    parser.add_argument("--platform", required=True, choices=("windows", "macos"))
    parser.add_argument("--private-key", required=True, type=Path)
    parser.add_argument(
        "--public-key-file",
        type=Path,
        default=REPO_ROOT / "alcedo_studio" / "src" / "config" / "update_public_key.txt",
    )
    parser.add_argument(
        "--env-file",
        type=Path,
        default=REPO_ROOT / "rust" / "puerh_mind" / ".env.test",
        help="Optional env file with R2_* credentials (ignored by git).",
    )
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument(
        "--promote-latest",
        action="store_true",
        default=True,
        help="Also publish stable/latest aliases (default: on).",
    )
    parser.add_argument(
        "--channel",
        default=None,
        choices=("stable", "beta"),
        help="Update channel to publish: 'stable' (official) or 'beta' (test). "
             "The channel manifest alias updates/v1/<channel>/<platform>/manifest.json is always "
             "published. releases/latest/ aliases are only written for the stable channel.",
    )
    args = parser.parse_args()
    channel = args.channel or packaged_channel(args.platform)
    platform_key = "windows-x86_64" if args.platform == "windows" else "macos-arm64"
    update_dir = REPO_ROOT / "build" / "tmp" / "update" / channel / platform_key

    prepare = [
        sys.executable,
        str(REPO_ROOT / "scripts" / "update" / "prepare_update.py"),
        "--platform", args.platform,
        "--private-key", str(args.private_key),
        "--channel", channel,
        "--public-key-file", str(args.public_key_file),
    ]
    print("+", " ".join(prepare))
    subprocess.run(prepare, check=True)

    manifest_path = update_dir / "update-manifest.json"
    signature_path = update_dir / "update-manifest.json.sig"
    artifacts_dir = update_dir / "artifacts"
    if not manifest_path.is_file() or not signature_path.is_file():
        raise SystemExit("manifest and signature files are required")
    if not artifacts_dir.is_dir():
        raise SystemExit(f"artifacts directory not found: {artifacts_dir}")

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    version = str(manifest["version"])
    tag = f"v{version}"

    public_key = args.public_key_file.read_text(encoding="utf-8").strip()
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
        str(artifacts_dir),
        "--tag",
        tag,
        "--platform",
        platform_key,
    ]
    print("+", " ".join(verify))
    subprocess.run(verify, check=True)

    sequence = int(manifest["sequence"])
    package_name = Path(manifest["artifacts"][platform_key]["url"]).name
    if not (artifacts_dir / package_name).is_file():
        raise SystemExit(f"missing artifact: {artifacts_dir / package_name}")
    macos_dmg_name = f"AlcedoStudio-{version}-Darwin-arm64.dmg" if args.platform == "macos" else ""
    has_dmg = bool(macos_dmg_name) and (artifacts_dir / macos_dmg_name).is_file()

    keys = planned_keys(tag, sequence, platform_key, package_name,
                        macos_dmg_name if has_dmg else "", channel, args.promote_latest)
    print("Validated update objects:")
    for key, role in keys:
        if "dmg" in key and not has_dmg and args.dry_run:
            print(f"  (optional missing) {key}  [{role}]")
            continue
        print(f"  {PUBLIC_BASE}/{key}  [{role}]")

    if args.dry_run:
        print("Dry run complete. R2 credentials were not read.")
        return 0

    load_env_file(args.env_file)
    account_id = require_env("R2_ACCOUNT_ID")
    bucket = require_env("R2_BUCKET")
    require_env("R2_ACCESS_KEY_ID")
    require_env("R2_SECRET_ACCESS_KEY")
    os.environ["AWS_ACCESS_KEY_ID"] = os.environ["R2_ACCESS_KEY_ID"]
    os.environ["AWS_SECRET_ACCESS_KEY"] = os.environ["R2_SECRET_ACCESS_KEY"]
    os.environ.setdefault("AWS_REGION", "auto")
    os.environ.setdefault("AWS_DEFAULT_REGION", "auto")
    os.environ.setdefault("AWS_EC2_METADATA_DISABLED", "true")
    endpoint = f"https://{account_id}.r2.cloudflarestorage.com"

    checksums = artifacts_dir / f"SHA256SUMS-{platform_key}.txt"
    names = [package_name]
    if has_dmg:
        names.append(macos_dmg_name)
    write_sha256sums(artifacts_dir, names, checksums)

    immutable = "public, max-age=31536000, immutable"
    mutable = "public, max-age=300, must-revalidate"
    version_prefix = f"releases/{tag}"
    manifest_prefix = f"updates/v1/releases/{tag}/{sequence}/{platform_key}"

    package_content_type = (
        "application/vnd.microsoft.portable-executable"
        if args.platform == "windows" else "application/zip"
    )

    uploads: list[tuple[Path, str, str, str, str]] = [
        (
            artifacts_dir / package_name,
            f"{version_prefix}/{package_name}",
            package_content_type,
            f'attachment; filename="{package_name}"',
            immutable,
        ),
        (
            checksums,
            f"{version_prefix}/SHA256SUMS-{platform_key}.txt",
            "text/plain; charset=utf-8",
            f'attachment; filename="SHA256SUMS-{platform_key}.txt"',
            immutable,
        ),
        (
            signature_path,
            f"{manifest_prefix}/manifest.json.sig",
            "text/plain; charset=utf-8",
            'inline; filename="manifest.json.sig"',
            immutable,
        ),
        (
            manifest_path,
            f"{manifest_prefix}/manifest.json",
            "application/json; charset=utf-8",
            'inline; filename="manifest.json"',
            immutable,
        ),
    ]
    if has_dmg:
        uploads.insert(
            1,
            (
                artifacts_dir / macos_dmg_name,
                f"{version_prefix}/{macos_dmg_name}",
                "application/x-apple-diskimage",
                f'attachment; filename="{macos_dmg_name}"',
                immutable,
            ),
        )

    # The per-channel manifest alias is the live feed the client checks; publish
    # it for every channel. Signature first, manifest last, so a client never
    # sees an unsigned or partially published manifest.
    uploads.extend(
        [
            (
                signature_path,
                f"updates/v1/{channel}/{platform_key}/manifest.json.sig",
                "text/plain; charset=utf-8",
                'inline; filename="manifest.json.sig"',
                mutable,
            ),
            (
                manifest_path,
                f"updates/v1/{channel}/{platform_key}/manifest.json",
                "application/json; charset=utf-8",
                'inline; filename="manifest.json"',
                mutable,
            ),
        ]
    )

    # releases/latest/ is the stable website download alias. Only the stable
    # channel may repoint it, and only when the caller did not disable it.
    if channel == "stable" and args.promote_latest:
        if has_dmg:
            uploads.append(
                (
                    artifacts_dir / macos_dmg_name,
                    "releases/latest/AlcedoStudio-macos-arm64.dmg",
                    "application/x-apple-diskimage",
                    'attachment; filename="AlcedoStudio-macos-arm64.dmg"',
                    mutable,
                )
            )
        latest_name = ("AlcedoStudio-Windows-x64.exe" if args.platform == "windows"
                       else "AlcedoStudio-macos-arm64.zip")
        uploads.extend([
            (
                artifacts_dir / package_name,
                f"releases/latest/{latest_name}",
                package_content_type,
                f'attachment; filename="{latest_name}"',
                mutable,
            ),
            (
                checksums,
                f"releases/latest/SHA256SUMS-{platform_key}.txt",
                "text/plain; charset=utf-8",
                f'attachment; filename="SHA256SUMS-{platform_key}.txt"',
                mutable,
            ),
        ])

    uploaded_keys: list[str] = []
    for source, key, content_type, disposition, cache_control in uploads:
        aws_upload(endpoint, bucket, source, key, content_type, disposition, cache_control)
        uploaded_keys.append(key)

    for key in uploaded_keys:
        aws_head(endpoint, bucket, key)

    print(f"Published build {manifest['build']} with sequence {sequence}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
