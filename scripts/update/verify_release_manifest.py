#!/usr/bin/env python3
"""Verify a signed release manifest and its local package files."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from release_notes import ReleaseNotesError, validate_release_notes


SPKI_PREFIX = bytes.fromhex("302a300506032b6570032100")


def openssl_executable() -> str:
    discovered = shutil.which("openssl")
    if discovered:
        return discovered
    if os.name == "nt":
        candidates = [
            Path(os.environ.get("ProgramFiles", r"C:\Program Files"))
            / "OpenSSL-Win64" / "bin" / "openssl.exe",
            Path(os.environ.get("ProgramFiles", r"C:\Program Files"))
            / "Git" / "mingw64" / "bin" / "openssl.exe",
        ]
        for candidate in candidates:
            if candidate.is_file():
                return str(candidate)
    raise RuntimeError("openssl was not found; install OpenSSL or Git for Windows")


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--signature", required=True, type=Path)
    parser.add_argument("--public-key-base64", required=True)
    parser.add_argument("--artifacts", required=True, type=Path)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--channel", choices=("stable", "beta"), default="stable")
    parser.add_argument(
        "--beta-base-url",
        default="https://static.aoraw.org/updates/v1/beta/builds",
    )
    parser.add_argument(
        "--platform",
        choices=("windows-x86_64", "macos-arm64"),
        default="",
        help="Verify only this platform artifact. By default both are required.",
    )
    args = parser.parse_args()

    raw_key = base64.b64decode(args.public_key_base64, validate=True)
    raw_signature = base64.b64decode(args.signature.read_bytes().strip(), validate=True)
    if len(raw_key) != 32 or len(raw_signature) != 64:
        raise RuntimeError("the public key or signature length is not valid")

    temporary_root = Path(__file__).resolve().parents[2] / "build" / "tmp"
    temporary_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="alcedo-update-verify-", dir=temporary_root) as directory:
        root = Path(directory)
        der_path = root / "public.der"
        signature_path = root / "signature.bin"
        der_path.write_bytes(SPKI_PREFIX + raw_key)
        signature_path.write_bytes(raw_signature)
        result = subprocess.run(
            [openssl_executable(), "pkeyutl", "-verify", "-rawin", "-pubin", "-keyform", "DER",
             "-inkey", str(der_path), "-sigfile", str(signature_path), "-in", str(args.manifest)],
            check=False,
        )
        if result.returncode != 0:
            raise RuntimeError("the Ed25519 signature is not valid")

    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    if manifest.get("schema") != 1 or not isinstance(manifest.get("sequence"), int):
        raise RuntimeError("the manifest schema or sequence is not valid")
    if not isinstance(manifest.get("version"), str) or not isinstance(manifest.get("build"), int):
        raise RuntimeError("the manifest version or build is not valid")
    changelogs = manifest.get("changelogs")
    if not isinstance(changelogs, dict) or set(changelogs) != {"en", "zh-CN"}:
        raise RuntimeError("the manifest does not contain both en and zh-CN release notes")
    if manifest.get("changelog") != changelogs.get("en"):
        raise RuntimeError("the legacy changelog fallback does not match the English notes")
    for language in ("en", "zh-CN"):
        if not isinstance(changelogs.get(language), str):
            raise RuntimeError(f"the manifest {language} release notes are not text")
        try:
            validate_release_notes(
                changelogs[language] + "\n",
                manifest["version"],
                manifest["build"],
                language,
            )
        except ReleaseNotesError as error:
            raise RuntimeError(
                f"the manifest {language} release notes are not valid: {error}"
            ) from error
    expected_keys = {args.platform} if args.platform else {"windows-x86_64", "macos-arm64"}
    if set(manifest.get("artifacts", {})) != expected_keys:
        raise RuntimeError("the manifest does not contain the required platform artifacts")
    for platform in sorted(expected_keys):
        item = manifest["artifacts"][platform]
        prefix = (
            f"{args.beta_base_url.rstrip('/')}/{manifest['build']}/{platform}/"
            if args.channel == "beta"
            else f"https://static.aoraw.org/releases/{args.tag}/"
        )
        if not item["url"].startswith(prefix):
            raise RuntimeError(f"{platform} does not use the immutable {args.channel} URL")
        path = args.artifacts / item["url"].removeprefix(prefix)
        if not path.is_file() or path.stat().st_size != item["size"] or digest(path) != item["sha256"]:
            raise RuntimeError(f"{platform} does not match its signed metadata")
    print("Signed update manifest and package metadata are valid.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
