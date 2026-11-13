#!/usr/bin/env python3
"""Build and check the package-inventory.json of one official LUT package.

The digest rule mirrors alcedo_studio/src/utils/lut/lut_inventory_digest.cpp:
``id NUL path NUL size NUL sha256 LF`` per LUT, sorted by path bytes then ID
bytes, hashed with SHA-256. Header reads and file hashing run on a thread pool.
"""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re

from alcedo_lut_metadata import (LutMetadataError, canonical_file_name, canonical_lut_id,
                                 read_cube_header_file)

INVENTORY_FILE_NAME = "package-inventory.json"
INVENTORY_KIND = "alcedo-lut-package-inventory"
AUXILIARY_SUFFIXES = {".txt", ".md", ".pdf", ".html"}
_SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
_PACKAGE_ID_PATTERN = re.compile(r"^[a-z0-9]+(?:[._-][a-z0-9]+)*$")


class LutInventoryError(ValueError):
    """Raised when package content cannot form a valid inventory."""


@dataclass(frozen=True)
class LutRecord:
    id: str
    path: str
    size: int
    sha256: str


@dataclass(frozen=True)
class AuxiliaryRecord:
    path: str
    size: int
    sha256: str


def is_safe_relative_path(path: str) -> bool:
    """Match IsSafeLutRelativePath: `/` segments, no `.`/`..`, drive, NUL, or line break."""
    if not path or any(character in path for character in "\0\n\r\\:") or path.startswith("/"):
        return False
    return all(segment not in ("", ".", "..") for segment in path.split("/"))


def compute_inventory_digest(records: list[LutRecord]) -> str:
    """Return the canonical inventory digest; input order does not matter."""
    ids: set[str] = set()
    folded: set[bytes] = set()
    for record in records:
        if not record.id or any(character in record.id for character in "\0\n\r"):
            raise LutInventoryError(f"LUT ID is empty or has NUL or a line break: {record.id!r}")
        if not is_safe_relative_path(record.path):
            raise LutInventoryError(f"LUT path is not a safe relative path: {record.path!r}")
        if not _SHA256_PATTERN.match(record.sha256):
            raise LutInventoryError(f"LUT SHA-256 is not lowercase hexadecimal: {record.path}")
        if record.id in ids:
            raise LutInventoryError(f"duplicate LUT ID: {record.id}")
        key = record.path.encode("utf-8").lower()
        if key in folded:
            raise LutInventoryError(f"LUT paths collide on a case-insensitive file system: "
                                    f"{record.path}")
        ids.add(record.id)
        folded.add(key)
    ordered = sorted(records, key=lambda item: (item.path.encode("utf-8"), item.id.encode("utf-8")))
    digest = hashlib.sha256()
    for record in ordered:
        digest.update(record.id.encode("utf-8") + b"\0" + record.path.encode("utf-8") + b"\0" +
                      str(record.size).encode("ascii") + b"\0" + record.sha256.encode("ascii") +
                      b"\n")
    return digest.hexdigest()


def hash_file(path: Path) -> tuple[str, int]:
    """Stream ``path`` through SHA-256 in 1 MiB blocks and return (digest, size)."""
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
            size += len(block)
    return digest.hexdigest(), size


def _classify_lut(root: Path, path: Path, package_id: str) -> LutRecord:
    """Read one official LUT header, check its identity rules, and hash it."""
    relative = path.relative_to(root).as_posix()
    try:
        header = read_cube_header_file(path)
    except LutMetadataError as error:
        raise LutInventoryError(f"{relative}: {error}") from error
    metadata = header.metadata
    if metadata is None:
        raise LutInventoryError(f"{relative}: official package LUTs need an ALCEDO_LUT comment")
    if metadata["origin"] != "alcedo":
        raise LutInventoryError(f"{relative}: official package LUTs must declare origin alcedo")
    if header.lut_3d_size < 2:
        raise LutInventoryError(f"{relative}: official package LUTs must be 3D cubes")
    if metadata["category"] == "film_simulation":
        if metadata["source"]["id"] != package_id:
            raise LutInventoryError(f"{relative}: source.id {metadata['source']['id']!r} is not "
                                    f"the package ID {package_id!r}")
        if metadata["id"] != canonical_lut_id(metadata):
            raise LutInventoryError(f"{relative}: ID must be {canonical_lut_id(metadata)!r}")
        if path.name != canonical_file_name(metadata):
            raise LutInventoryError(f"{relative}: file name must be "
                                    f"{canonical_file_name(metadata)!r}")
    elif not metadata["id"].startswith(f"{package_id}:"):
        raise LutInventoryError(f"{relative}: general LUT IDs must start with '{package_id}:'")
    sha256, size = hash_file(path)
    return LutRecord(metadata["id"], relative, size, sha256)


def _classify_auxiliary(root: Path, path: Path) -> AuxiliaryRecord:
    sha256, size = hash_file(path)
    return AuxiliaryRecord(path.relative_to(root).as_posix(), size, sha256)


def build_package_inventory(root: Path, package_id: str, revision: str,
                            workers: int | None = None) -> dict:
    """Validate every file under ``root`` and return the package-inventory document.

    ``.cube`` files (any case) are LUTs; files with a documented auxiliary suffix
    are auxiliary files; anything else, links, and an existing inventory file are
    rejected so the archive holds only listed regular files.
    """
    if not _PACKAGE_ID_PATTERN.match(package_id):
        raise LutInventoryError(f"package ID is not a slug: {package_id!r}")
    if not revision or len(revision) > 64 or revision != revision.strip():
        raise LutInventoryError("revision must be 1-64 characters without surrounding spaces")
    root = Path(root)
    luts: list[Path] = []
    auxiliary: list[Path] = []
    for directory, directory_names, file_names in os.walk(root, followlinks=False):
        base = Path(directory)
        for name in directory_names:
            if (base / name).is_symlink():
                raise LutInventoryError(f"links are not allowed in a package: {base / name}")
        for name in sorted(file_names):
            path = base / name
            relative = path.relative_to(root).as_posix()
            if path.is_symlink() or not path.is_file():
                raise LutInventoryError(f"only regular files are allowed: {relative}")
            if relative == INVENTORY_FILE_NAME:
                raise LutInventoryError(f"{INVENTORY_FILE_NAME} is generated; remove it first")
            if not is_safe_relative_path(relative):
                raise LutInventoryError(f"unsafe package path: {relative!r}")
            if path.suffix.lower() == ".cube":
                luts.append(path)
            elif path.suffix.lower() in AUXILIARY_SUFFIXES:
                auxiliary.append(path)
            else:
                raise LutInventoryError(f"unexpected file type in a package: {relative}")
    if not luts:
        raise LutInventoryError(f"package {package_id} has no LUT files")

    with ThreadPoolExecutor(max_workers=workers or min(8, os.cpu_count() or 1)) as pool:
        lut_records = list(pool.map(lambda item: _classify_lut(root, item, package_id), luts))
        auxiliary_records = list(pool.map(lambda item: _classify_auxiliary(root, item), auxiliary))

    inventory_sha256 = compute_inventory_digest(lut_records)
    folded = {record.path.encode("utf-8").lower() for record in lut_records}
    for record in auxiliary_records:
        if record.path.encode("utf-8").lower() in folded:
            raise LutInventoryError(f"package paths collide: {record.path}")
        folded.add(record.path.encode("utf-8").lower())
    lut_records.sort(key=lambda item: item.path.encode("utf-8"))
    auxiliary_records.sort(key=lambda item: item.path.encode("utf-8"))
    return {
        "schema": 1,
        "kind": INVENTORY_KIND,
        "package_id": package_id,
        "revision": revision,
        "file_count": len(lut_records),
        "inventory_sha256": inventory_sha256,
        "unpacked_bytes": sum(item.size for item in lut_records) +
        sum(item.size for item in auxiliary_records),
        "luts": [record.__dict__ for record in lut_records],
        "auxiliary_files": [record.__dict__ for record in auxiliary_records],
    }


def serialize_inventory(inventory: dict) -> bytes:
    return (json.dumps(inventory, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def check_inventory(inventory: dict) -> None:
    """Cross-check count, byte total, and digest like ParseLutPackageInventory."""
    if inventory.get("schema") != 1 or inventory.get("kind") != INVENTORY_KIND:
        raise LutInventoryError("package inventory schema or kind is not supported")
    records = [LutRecord(**item) for item in inventory["luts"]]
    auxiliary = [AuxiliaryRecord(**item) for item in inventory["auxiliary_files"]]
    if inventory["file_count"] != len(records):
        raise LutInventoryError("package inventory file count does not match its LUT records")
    if inventory["unpacked_bytes"] != sum(item.size for item in records) + \
            sum(item.size for item in auxiliary):
        raise LutInventoryError("package inventory byte total does not match its records")
    if compute_inventory_digest(records) != inventory["inventory_sha256"]:
        raise LutInventoryError("package inventory digest does not match its LUT records")
