#!/usr/bin/env python3
"""Verify a signed release manifest and its local package files."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


SPKI_PREFIX = bytes.fromhex("302a300506032b6570032100")


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
    args = parser.parse_args()

    raw_key = base64.b64decode(args.public_key_base64, validate=True)
    raw_signature = base64.b64decode(args.signature.read_bytes().strip(), validate=True)
    if len(raw_key) != 32 or len(raw_signature) != 64:
        raise RuntimeError("the public key or signature length is not valid")

    with tempfile.TemporaryDirectory(prefix="alcedo-update-verify-") as directory:
        root = Path(directory)
        der_path = root / "public.der"
        signature_path = root / "signature.bin"
        der_path.write_bytes(SPKI_PREFIX + raw_key)
        signature_path.write_bytes(raw_signature)
        result = subprocess.run(
            ["openssl", "pkeyutl", "-verify", "-rawin", "-pubin", "-keyform", "DER",
             "-inkey", str(der_path), "-sigfile", str(signature_path), "-in", str(args.manifest)],
            check=False,
        )
        if result.returncode != 0:
            raise RuntimeError("the Ed25519 signature is not valid")

    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    if manifest.get("schema") != 1 or not isinstance(manifest.get("sequence"), int):
        raise RuntimeError("the manifest schema or sequence is not valid")
    expected_keys = {"windows-x86_64", "macos-arm64"}
    if set(manifest.get("artifacts", {})) != expected_keys:
        raise RuntimeError("the manifest does not contain the required platform artifacts")
    prefix = f"https://static.aoraw.org/releases/{args.tag}/"
    for platform in sorted(expected_keys):
        item = manifest["artifacts"][platform]
        if not item["url"].startswith(prefix):
            raise RuntimeError(f"{platform} does not use the immutable release URL")
        path = args.artifacts / item["url"].removeprefix(prefix)
        if not path.is_file() or path.stat().st_size != item["size"] or digest(path) != item["sha256"]:
            raise RuntimeError(f"{platform} does not match its signed metadata")
    print("Signed update manifest and package metadata are valid.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
