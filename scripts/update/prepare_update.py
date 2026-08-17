#!/usr/bin/env python3
"""Discover, create, sign, and verify one platform update package."""

from __future__ import annotations

import argparse
import datetime as dt
from pathlib import Path
import re
import shutil
import subprocess
import sys

from release_git import read_head_commit, require_clean_worktree, validate_commit


REPO_ROOT = Path(__file__).resolve().parents[2]
PLATFORMS = {
    "windows": {
        "artifact_key": "windows-x86_64",
        "build_dir": REPO_ROOT / "build" / "release",
        "package_pattern": "AlcedoStudio-{version}-Windows-*.exe",
    },
    "macos": {
        "artifact_key": "macos-arm64",
        "build_dir": REPO_ROOT / "build" / "macos-release",
        "package_pattern": "AlcedoStudio-{version}-Darwin-*.zip",
        "dmg_pattern": "AlcedoStudio-{version}-Darwin-*.dmg",
    },
}


def read_cmake_cache(build_dir: Path) -> dict[str, str]:
    cache_path = build_dir / "CMakeCache.txt"
    if not cache_path.is_file():
        raise SystemExit(
            f"CMake cache not found: {cache_path}. Run the platform package script first."
        )
    values: dict[str, str] = {}
    for raw_line in cache_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not raw_line or raw_line.startswith(("#", "//")) or "=" not in raw_line:
            continue
        name_and_type, value = raw_line.split("=", 1)
        values[name_and_type.split(":", 1)[0]] = value
    return values


def project_version(cache: dict[str, str]) -> str:
    version = cache.get("CMAKE_PROJECT_VERSION", "").strip()
    if version:
        return version
    source = (REPO_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r"project\s*\(\s*alcedo\s+VERSION\s+([0-9]+(?:\.[0-9]+){2})", source)
    if not match:
        raise SystemExit("cannot determine the Alcedo project version")
    return match.group(1)


def build_number(cache: dict[str, str], version: str) -> int:
    configured = cache.get("ALCEDO_BUILD_NUMBER", "").strip()
    if configured:
        try:
            value = int(configured)
        except ValueError as error:
            raise SystemExit(f"invalid ALCEDO_BUILD_NUMBER in CMakeCache.txt: {configured}") from error
        if value > 0:
            return value
    major, minor, patch = (int(part) for part in version.split("."))
    return major * 1_000_000 + minor * 1_000 + patch


def discover_one(package_dir: Path, pattern: str, role: str) -> Path:
    matches = sorted(path for path in package_dir.glob(pattern) if path.is_file())
    if len(matches) == 1:
        return matches[0]
    if not matches:
        raise SystemExit(
            f"{role} not found under fixed package directory {package_dir} (expected {pattern}). "
            "Run the platform package script first."
        )
    listed = "\n  ".join(str(path) for path in matches)
    raise SystemExit(
        f"multiple {role} files match {pattern}; keep exactly one package for this version:\n  {listed}"
    )


def default_signer(platform: str, build_dir: Path) -> Path:
    executable = "alcedo_update_signer.exe" if platform == "windows" else "alcedo_update_signer"
    candidates = [
        build_dir / "alcedo_studio" / "src" / executable,
        REPO_ROOT / "build" / "debug" / "alcedo_studio" / "src" / executable,
        REPO_ROOT / "build" / "release" / "alcedo_studio" / "src" / executable,
        REPO_ROOT / "build" / "macos-release" / "alcedo_studio" / "src" / executable,
    ]
    for path in candidates:
        if path.is_file():
            return path
    return candidates[0]


def run(command: list[str]) -> None:
    print("+", " ".join(command))
    subprocess.run(command, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Discover the fixed package output for one platform, create its signed update "
            "manifest, and verify the local package hash."
        )
    )
    parser.add_argument("--platform", required=True, choices=tuple(PLATFORMS))
    parser.add_argument("--private-key", required=True, type=Path)
    parser.add_argument("--version", default="", help="Optional metadata override for recovery use.")
    parser.add_argument("--build", type=int, default=None, help="Optional build-number override.")
    parser.add_argument("--sequence", type=int, default=None, help="Optional sequence override.")
    parser.add_argument("--commit", default="", help="Optional packaged git SHA override.")
    parser.add_argument(
        "--tag",
        default="",
        help="Ignored. Package URLs no longer use a git tag.",
    )
    parser.add_argument("--channel", choices=("stable", "beta"), default=None)
    parser.add_argument(
        "--public-key-file",
        type=Path,
        default=REPO_ROOT / "alcedo_studio" / "src" / "config" / "update_public_key.txt",
    )
    parser.add_argument("--output-dir", type=Path, default=None)
    parser.add_argument("--signer", type=Path, default=None)
    parser.add_argument("--valid-days", default=30, type=int)
    args = parser.parse_args()

    config = PLATFORMS[args.platform]
    build_dir = Path(config["build_dir"])
    cache = read_cmake_cache(build_dir)
    version = args.version or project_version(cache)
    build = args.build if args.build is not None else build_number(cache, version)
    sequence = args.sequence or int(dt.datetime.now(dt.timezone.utc).strftime("%Y%m%d%H%M%S"))
    if args.commit:
        commit = validate_commit(args.commit)
    else:
        require_clean_worktree(REPO_ROOT)
        commit = read_head_commit(REPO_ROOT)
    packaged_channel = cache.get("ALCEDO_UPDATE_CHANNEL", "stable").strip() or "stable"
    channel = args.channel or packaged_channel
    if channel != packaged_channel:
        parser.error(
            f"requested channel '{channel}' does not match packaged channel "
            f"'{packaged_channel}' in {build_dir / 'CMakeCache.txt'}; repackage for {channel}"
        )
    if build < 1 or sequence < 1:
        parser.error("build and sequence must be positive integers")

    package_dir = build_dir / "package"
    package_path = discover_one(
        package_dir,
        str(config["package_pattern"]).format(version=version),
        f"{args.platform} automatic-update package",
    )
    dmg_path: Path | None = None
    if args.platform == "macos":
        dmg_path = discover_one(
            package_dir,
            str(config["dmg_pattern"]).format(version=version),
            "macOS manual-install DMG",
        )

    if not args.private_key.is_file():
        raise SystemExit(f"private key not found: {args.private_key}")
    if not args.public_key_file.is_file():
        raise SystemExit(f"public key file not found: {args.public_key_file}")

    signer = args.signer or default_signer(args.platform, build_dir)
    if not signer.is_file():
        raise SystemExit(
            f"alcedo_update_signer not found at {signer}. "
            "Build target alcedo_update_signer first."
        )

    output_dir = args.output_dir or (
        REPO_ROOT / "build" / "tmp" / "update" / channel / str(config["artifact_key"])
    )
    output_dir.mkdir(parents=True, exist_ok=True)
    manifest = output_dir / "update-manifest.json"
    signature = output_dir / "update-manifest.json.sig"
    artifacts_dir = output_dir / "artifacts"
    artifacts_dir.mkdir(parents=True, exist_ok=True)
    signature.unlink(missing_ok=True)

    package_copy = artifacts_dir / package_path.name
    shutil.copy2(package_path, package_copy)
    if dmg_path is not None:
        shutil.copy2(dmg_path, artifacts_dir / dmg_path.name)

    create_cmd = [
        sys.executable,
        str(REPO_ROOT / "scripts" / "update" / "create_update_manifest.py"),
        "--version",
        version,
        "--build",
        str(build),
        "--sequence",
        str(sequence),
        "--commit",
        commit,
        "--channel",
        channel,
        "--output",
        str(manifest),
        "--valid-days",
        str(args.valid_days),
    ]
    artifact_argument = "--windows" if args.platform == "windows" else "--macos-arm64"
    create_cmd.extend([artifact_argument, str(package_copy)])
    if dmg_path is not None:
        create_cmd.extend(["--macos-dmg", str(artifacts_dir / dmg_path.name)])

    run(create_cmd)
    run(
        [
            str(signer),
            "sign",
            "--private-key",
            str(args.private_key),
            "--manifest",
            str(manifest),
            "--signature",
            str(signature),
        ]
    )

    public_key = args.public_key_file.read_text(encoding="utf-8").strip()
    run(
        [
            sys.executable,
            str(REPO_ROOT / "scripts" / "update" / "verify_release_manifest.py"),
            "--manifest",
            str(manifest),
            "--signature",
            str(signature),
            "--public-key-base64",
            public_key,
            "--artifacts",
            str(artifacts_dir),
            "--channel",
            channel,
            "--platform",
            str(config["artifact_key"]),
        ]
    )

    print(f"Platform: {config['artifact_key']}")
    print(f"Channel: {channel}")
    print(f"Version/build: {version} ({build})")
    print(f"Commit: {commit}")
    print(f"Sequence: {sequence}")
    print(f"Manifest: {manifest}")
    print(f"Signature: {signature}")
    print(f"Artifacts: {artifacts_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
