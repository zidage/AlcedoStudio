#!/usr/bin/env python3
"""Sign and upload one platform's update to the public updates/ feed on R2.

A stable upload is live for installed apps and the website. A GitHub release is
created later by archive_stable_github.py and is not part of this script.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

from release_git import commit_is_on_origin_main, require_commit_on_origin_main, validate_commit


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


def immutable_prefixes(build: int, sequence: int, platform_key: str,
                       channel: str) -> tuple[str, str]:
    build_prefix = f"updates/v1/{channel}/builds/{build}/{platform_key}"
    return build_prefix, f"{build_prefix}/manifests/{sequence}"


def planned_keys(build: int, sequence: int, platform_key: str, package_name: str,
                 dmg_name: str, channel: str) -> list[tuple[str, str]]:
    package_prefix, manifest_prefix = immutable_prefixes(
        build, sequence, platform_key, channel
    )
    keys = [
        (f"{package_prefix}/{package_name}", "immutable package"),
        (f"{package_prefix}/SHA256SUMS-{platform_key}.txt", "immutable checksums"),
        (f"{manifest_prefix}/manifest.json.sig", "immutable signature"),
        (f"{manifest_prefix}/manifest.json", "immutable manifest"),
        (f"updates/v1/{channel}/{platform_key}/manifest.json.sig", f"{channel} signature"),
        (f"updates/v1/{channel}/{platform_key}/manifest.json", f"{channel} manifest"),
    ]
    if dmg_name:
        keys.insert(1, (f"{package_prefix}/{dmg_name}", "immutable package"))
    return keys


def file_digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def aws_upload(endpoint: str, bucket: str, source: Path, key: str, content_type: str,
               disposition: str, cache_control: str, sha256: str) -> None:
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
        "--metadata",
        f"alcedo-sha256={sha256}",
        "--cli-connect-timeout",
        "60",
        "--cli-read-timeout",
        "0",
    ]
    if source.stat().st_size < 1024 * 1024:
        command.append("--no-progress")
    print(f"Uploading {source.name} ({source.stat().st_size / (1024 * 1024):.1f} MiB)")
    print("+", " ".join(command))
    subprocess.run(command, check=True)


def aws_head(endpoint: str, bucket: str, key: str) -> dict[str, object] | None:
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
        "--output",
        "json",
        "--cli-connect-timeout",
        "60",
        "--cli-read-timeout",
        "60",
    ]
    result = subprocess.run(command, check=False, capture_output=True, text=True)
    if result.returncode == 0:
        return json.loads(result.stdout)
    error = result.stderr.strip()
    if "404" in error or "Not Found" in error or "NoSuchKey" in error:
        return None
    raise RuntimeError(f"failed to inspect s3://{bucket}/{key}: {error}")


def matching_object(head: dict[str, object] | None, size: int, sha256: str,
                    allow_missing_digest: bool = False) -> bool:
    if head is None or int(head.get("ContentLength", -1)) != size:
        return False
    metadata = head.get("Metadata", {})
    if not isinstance(metadata, dict):
        return False
    remote_digest = str(metadata.get("alcedo-sha256", ""))
    return remote_digest == sha256 or (allow_missing_digest and not remote_digest)


def wait_for_object(endpoint: str, bucket: str, key: str, size: int, sha256: str) -> None:
    delays = (1, 2, 4, 8, 16, 30)
    for attempt, delay in enumerate(delays, start=1):
        head = aws_head(endpoint, bucket, key)
        if matching_object(head, size, sha256):
            print(f"Verified s3://{bucket}/{key}")
            return
        if head is not None:
            raise RuntimeError(
                f"uploaded object metadata does not match the local file: s3://{bucket}/{key}"
            )
        print(f"Object is not visible yet ({attempt}/{len(delays)}); retrying in {delay}s: {key}")
        time.sleep(delay)
    raise RuntimeError(f"uploaded object is still missing: s3://{bucket}/{key}")


def publish_object(endpoint: str, bucket: str, source: Path, key: str, content_type: str,
                   disposition: str, cache_control: str, immutable: bool) -> None:
    sha256 = file_digest(source)
    existing = aws_head(endpoint, bucket, key) if immutable else None
    if existing is not None:
        # Objects uploaded before digest metadata was introduced can still be
        # resumed when the immutable key and exact length match.
        if not matching_object(existing, source.stat().st_size, sha256, True):
            raise RuntimeError(
                f"refusing to replace a different immutable object: s3://{bucket}/{key}"
            )
        print(f"Reusing existing immutable object: s3://{bucket}/{key}")
        return
    aws_upload(endpoint, bucket, source, key, content_type, disposition, cache_control, sha256)
    wait_for_object(endpoint, bucket, key, source.stat().st_size, sha256)


def write_sha256sums(artifacts_dir: Path, names: list[str], output: Path) -> None:
    lines: list[str] = []
    for name in names:
        digest = file_digest(artifacts_dir / name)
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
        "--channel",
        default=None,
        choices=("stable", "beta"),
        help="Update channel to publish: 'stable' (official) or 'beta' (updater tests). "
             "Both channels write only under updates/v1/<channel>/. A stable upload is live "
             "for installed apps and the website. Do not run it unless you intend to ship.",
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
    commit = validate_commit(str(manifest.get("commit", "")))

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
        "--channel",
        channel,
        "--platform",
        platform_key,
    ]
    print("+", " ".join(verify))
    subprocess.run(verify, check=True)

    sequence = int(manifest["sequence"])
    build = int(manifest["build"])
    package_name = Path(manifest["artifacts"][platform_key]["url"]).name
    if not (artifacts_dir / package_name).is_file():
        raise SystemExit(f"missing artifact: {artifacts_dir / package_name}")
    macos_dmg_name = f"AlcedoStudio-{version}-Darwin-arm64.dmg" if args.platform == "macos" else ""
    has_dmg = bool(macos_dmg_name) and (artifacts_dir / macos_dmg_name).is_file()

    keys = planned_keys(build, sequence, platform_key, package_name,
                        macos_dmg_name if has_dmg else "", channel)
    print(f"Packaged commit: {commit}")
    print("Validated update objects:")
    for key, role in keys:
        if "dmg" in key and not has_dmg and args.dry_run:
            print(f"  (optional missing) {key}  [{role}]")
            continue
        print(f"  {PUBLIC_BASE}/{key}  [{role}]")

    if args.dry_run:
        if channel == "stable" and not commit_is_on_origin_main(REPO_ROOT, commit):
            print(
                "WARNING: this stable dry-run is not from origin/main. "
                "A real stable upload would be refused."
            )
        print("Dry run complete. R2 credentials were not read. Nothing was published.")
        return 0

    if channel == "stable":
        require_commit_on_origin_main(REPO_ROOT, commit)
        print(
            f"Publishing a LIVE stable update for {version} (build {build}) from {commit}."
        )

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
    os.environ.setdefault("AWS_RETRY_MODE", "standard")
    os.environ.setdefault("AWS_MAX_ATTEMPTS", "10")
    endpoint = f"https://{account_id}.r2.cloudflarestorage.com"

    checksums = artifacts_dir / f"SHA256SUMS-{platform_key}.txt"
    names = [package_name]
    if has_dmg:
        names.append(macos_dmg_name)
    write_sha256sums(artifacts_dir, names, checksums)

    immutable = "public, max-age=31536000, immutable"
    mutable = "public, max-age=300, must-revalidate"
    package_prefix, manifest_prefix = immutable_prefixes(
        build, sequence, platform_key, channel
    )

    package_content_type = (
        "application/vnd.microsoft.portable-executable"
        if args.platform == "windows" else "application/zip"
    )

    uploads: list[tuple[Path, str, str, str, str]] = [
        (
            artifacts_dir / package_name,
            f"{package_prefix}/{package_name}",
            package_content_type,
            f'attachment; filename="{package_name}"',
            immutable,
        ),
        (
            checksums,
            f"{package_prefix}/SHA256SUMS-{platform_key}.txt",
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
                f"{package_prefix}/{macos_dmg_name}",
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

    immutable_uploads = [item for item in uploads if item[4] == immutable]
    mutable_uploads = [item for item in uploads if item[4] != immutable]

    # Never promote a live channel manifest until every package and archived
    # signed manifest is independently visible. This keeps a failed or slow
    # multipart upload from publishing a feed that points at a missing package.
    for source, key, content_type, disposition, cache_control in immutable_uploads:
        publish_object(
            endpoint, bucket, source, key, content_type, disposition, cache_control, True
        )
    for source, key, content_type, disposition, cache_control in mutable_uploads:
        publish_object(
            endpoint, bucket, source, key, content_type, disposition, cache_control, False
        )

    print(f"Published build {manifest['build']} with sequence {sequence}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
