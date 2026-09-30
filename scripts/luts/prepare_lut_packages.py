#!/usr/bin/env python3
"""Validate official LUT folders, build one 7z per package, and sign the LUT feed.

Every package is validated and archived independently. Without --private-key the
script stops after local package validation and writes an unsigned manifest for
inspection. Nothing is uploaded; see publish_lut_packages.py.
"""

from __future__ import annotations

import argparse
import base64
import datetime as dt
import json
from pathlib import Path
import re
import subprocess
import sys

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

from alcedo_lut_metadata import LutMetadataError, read_cube_header_file
from lut_inventory import LutInventoryError, build_package_inventory, hash_file
from lut_package_archive import verify_package_archive, write_package_archive

REPO_ROOT = Path(__file__).resolve().parents[2]

DEFAULT_PUBLIC_BASE = "https://static.aoraw.org"
DEFAULT_PREFIX = "luts/v1"
MANIFEST_KIND = "alcedo-lut-packages"
_REVISION_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")


def archive_key(prefix: str, package_id: str, revision: str) -> str:
    return f"{prefix}/packages/{package_id}/{revision}/{package_id}-{revision}.7z"


def package_display_name(source: Path, inventory: dict) -> str | None:
    """The ``source.name`` its film simulations declare, for the feed's optional ``name``.

    Every film simulation of a package declares ``source.id`` equal to the package ID
    (checked by the inventory). Different ``source.name`` values are an error.
    """
    names: set[str] = set()
    for record in inventory["luts"]:
        try:
            metadata = read_cube_header_file(source / record["path"]).metadata
        except LutMetadataError as error:
            raise LutInventoryError(f"{record['path']}: {error}") from error
        if metadata and metadata["category"] == "film_simulation":
            names.add(metadata["source"]["name"])
    if len(names) > 1:
        raise LutInventoryError(f"the package declares different source names: {sorted(names)}")
    return next(iter(names), None)


def build_package(package_id: str, source: Path, revision: str, output_dir: Path, prefix: str,
                  public_base: str, workers: int | None) -> dict:
    """Validate ``source``, write its archive, re-read it, and return its feed descriptor."""
    inventory = build_package_inventory(source, package_id, revision, workers)
    archive_path = output_dir / "packages" / f"{package_id}-{revision}.7z"
    write_package_archive(source, inventory, archive_path)
    sha256, size = hash_file(archive_path)
    descriptor = {"id": package_id}
    name = package_display_name(source, inventory)
    if name is not None:
        descriptor["name"] = name
    descriptor |= {
        "revision": revision,
        "file_count": inventory["file_count"],
        "inventory_sha256": inventory["inventory_sha256"],
        "unpacked_bytes": inventory["unpacked_bytes"],
        "artifact": {
            "url": f"{public_base.rstrip('/')}/{archive_key(prefix, package_id, revision)}",
            "size": size,
            "sha256": sha256,
        },
    }
    verify_package_archive(archive_path, descriptor, output_dir / "verify")
    return descriptor


def manifest_bytes(sequence: int, descriptors: list[dict]) -> bytes:
    manifest = {
        "schema": 1,
        "kind": MANIFEST_KIND,
        "sequence": sequence,
        "packages": sorted(descriptors, key=lambda item: item["id"]),
    }
    return (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def verify_signature(manifest_path: Path, signature_path: Path, public_key_base64: str) -> None:
    """Verify the detached Ed25519 signature over the exact manifest bytes."""
    raw_key = base64.b64decode(public_key_base64, validate=True)
    raw_signature = base64.b64decode(signature_path.read_bytes().strip(), validate=True)
    if len(raw_key) != 32 or len(raw_signature) != 64:
        raise RuntimeError("the public key or signature length is not valid")
    try:
        Ed25519PublicKey.from_public_bytes(raw_key).verify(raw_signature,
                                                           manifest_path.read_bytes())
    except InvalidSignature as error:
        raise RuntimeError("the LUT feed signature is not valid") from error


def default_signer() -> Path:
    executable = "alcedo_update_signer.exe" if sys.platform == "win32" else "alcedo_update_signer"
    for build in ("release", "debug", "macos-release", "macos-debug"):
        candidate = REPO_ROOT / "build" / build / "alcedo_studio" / "src" / executable
        if candidate.is_file():
            return candidate
    return REPO_ROOT / "build" / "release" / "alcedo_studio" / "src" / executable


def parse_package_argument(value: str) -> tuple[str, Path]:
    package_id, separator, directory = value.partition("=")
    if not separator or not package_id or not directory:
        raise argparse.ArgumentTypeError("use --package ID=DIRECTORY")
    return package_id, Path(directory)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", action="append", required=True, type=parse_package_argument,
                        help="Package ID and its annotated LUT directory, e.g. "
                             "spectral_film_lut=D:/out/spectral_film_lut. Repeat per package.")
    parser.add_argument("--revision", required=True, help="Release label, e.g. 2026.09.1")
    parser.add_argument("--sequence", type=int, default=None,
                        help="Feed sequence; default is the UTC time as YYYYMMDDHHMMSS.")
    parser.add_argument("--public-base", default=DEFAULT_PUBLIC_BASE)
    parser.add_argument("--prefix", default=DEFAULT_PREFIX,
                        help="R2 key prefix supplied by the release operator.")
    parser.add_argument("--output-dir", type=Path, default=None)
    parser.add_argument("--private-key", type=Path, default=None)
    parser.add_argument("--public-key-file", type=Path,
                        default=REPO_ROOT / "alcedo_studio" / "src" / "config" /
                        "update_public_key.txt")
    parser.add_argument("--signer", type=Path, default=None)
    parser.add_argument("--workers", type=int, default=None)
    args = parser.parse_args(argv)

    if not _REVISION_PATTERN.match(args.revision):
        parser.error("revision must be 1-64 URL-safe characters: letters, digits, '.', '_', '-'")
    package_ids = [package_id for package_id, _ in args.package]
    if len(package_ids) != len(set(package_ids)):
        parser.error("each package ID may appear only once")
    sequence = args.sequence or int(dt.datetime.now(dt.timezone.utc).strftime("%Y%m%d%H%M%S"))
    if sequence < 1:
        parser.error("sequence must be positive")
    prefix = args.prefix.strip("/")
    output_dir = args.output_dir or REPO_ROOT / "build" / "tmp" / "luts" / str(sequence)
    if output_dir.exists() and any(output_dir.iterdir()):
        parser.error(f"output directory is not empty: {output_dir}")
    output_dir.mkdir(parents=True, exist_ok=True)

    descriptors: list[dict] = []
    failures: list[str] = []
    for package_id, source in args.package:
        try:
            descriptor = build_package(package_id, source, args.revision, output_dir, prefix,
                                       args.public_base, args.workers)
        except (LutInventoryError, OSError) as error:
            failures.append(f"{package_id}: {error}")
            print(f"FAILED {package_id}: {error}")
            continue
        descriptors.append(descriptor)
        print(f"Validated {package_id}: {descriptor['file_count']} LUTs, "
              f"inventory {descriptor['inventory_sha256']}, "
              f"archive {descriptor['artifact']['size']} bytes")
    if failures:
        print("Publication stopped; the previous live feed is unchanged.")
        return 1

    manifest_path = output_dir / "manifest.json"
    manifest_path.write_bytes(manifest_bytes(sequence, descriptors))
    print(f"Manifest: {manifest_path}")
    if args.private_key is None:
        print("No --private-key given: packages validated locally; the manifest is unsigned.")
        return 0

    signer = args.signer or default_signer()
    if not signer.is_file():
        raise SystemExit(f"alcedo_update_signer not found at {signer}; build it first.")
    signature_path = output_dir / "manifest.json.sig"
    subprocess.run([str(signer), "sign", "--private-key", str(args.private_key), "--manifest",
                    str(manifest_path), "--signature", str(signature_path)], check=True)
    verify_signature(manifest_path, signature_path,
                     args.public_key_file.read_text(encoding="utf-8").strip())
    print(f"Signature: {signature_path} (verified)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
