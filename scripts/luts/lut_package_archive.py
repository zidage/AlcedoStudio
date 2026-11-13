#!/usr/bin/env python3
"""Create and verify one official LUT package archive (7z, LZMA2 preset 9).

Requires py7zr (``pip install py7zr``). Archives contain only regular files with
relative paths: the LUTs, auxiliary files, and package-inventory.json.
"""

from __future__ import annotations

import json
from pathlib import Path
import shutil
import tempfile

import py7zr

from lut_inventory import (INVENTORY_FILE_NAME, LutInventoryError, check_inventory, hash_file,
                           is_safe_relative_path, serialize_inventory)

ARCHIVE_FILTERS = [{"id": py7zr.FILTER_LZMA2, "preset": 9}]


def write_package_archive(source_root: Path, inventory: dict, archive_path: Path) -> None:
    """Write the listed files and the inventory into a new 7z archive."""
    archive_path.parent.mkdir(parents=True, exist_ok=True)
    if archive_path.exists():
        raise LutInventoryError(f"archive already exists: {archive_path}")
    listed = [item["path"] for item in inventory["luts"]] + \
        [item["path"] for item in inventory["auxiliary_files"]]
    with py7zr.SevenZipFile(archive_path, "w", filters=ARCHIVE_FILTERS) as archive:
        for relative in sorted(listed, key=lambda value: value.encode("utf-8")):
            archive.write(source_root / relative, relative)
        archive.writestr(serialize_inventory(inventory), INVENTORY_FILE_NAME)


def verify_package_archive(archive_path: Path, expected: dict, work_root: Path) -> dict:
    """Extract ``archive_path`` into a new directory and verify every listed file.

    ``expected`` holds ``id``, ``revision``, ``file_count``, ``inventory_sha256``,
    and ``unpacked_bytes`` from the feed descriptor. Returns the archive's
    inventory. Rejects unlisted, missing, unsafe, or changed entries.
    """
    work_root.mkdir(parents=True, exist_ok=True)
    with py7zr.SevenZipFile(archive_path, "r") as archive:
        entries = archive.list()
        names = [entry.filename.replace("\\", "/") for entry in entries if not entry.is_directory]
        for entry in entries:
            name = entry.filename.replace("\\", "/")
            if not is_safe_relative_path(name) or getattr(entry, "is_symlink", False):
                raise LutInventoryError(f"archive entry is not a safe regular file: {name!r}")
    if len(names) != len(set(names)):
        raise LutInventoryError("archive has duplicate entries")
    directory = Path(tempfile.mkdtemp(prefix="verify-", dir=work_root))
    try:
        with py7zr.SevenZipFile(archive_path, "r") as archive:
            archive.extractall(path=directory)
        inventory = json.loads((directory / INVENTORY_FILE_NAME).read_text(encoding="utf-8"))
        check_inventory(inventory)
        for key in ("file_count", "inventory_sha256", "unpacked_bytes", "revision"):
            if inventory[key] != expected[key]:
                raise LutInventoryError(f"archive inventory {key} differs from its descriptor")
        if inventory["package_id"] != expected["id"]:
            raise LutInventoryError("archive inventory package ID differs from its descriptor")
        listed = {item["path"]: item for item in inventory["luts"] + inventory["auxiliary_files"]}
        if set(names) != set(listed) | {INVENTORY_FILE_NAME}:
            raise LutInventoryError("archive entries differ from the inventory file list")
        for relative, item in listed.items():
            sha256, size = hash_file(directory / relative)
            if sha256 != item["sha256"] or size != item["size"]:
                raise LutInventoryError(f"archive file does not match its inventory: {relative}")
        return inventory
    finally:
        shutil.rmtree(directory, ignore_errors=True)
