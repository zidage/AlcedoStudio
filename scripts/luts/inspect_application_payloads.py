#!/usr/bin/env python3
"""Check packaged application artifacts for LUT payloads.

Official LUTs ship only as signed LUT packages that Settings downloads into the
user's library; the application, its installers, and its update archives must
not contain `.cube` files. Each argument is an install tree (directory, for
example `build/install` or `Alcedo Studio.app`), a `.zip` or `.7z` archive, or
an installer (`.exe`, `.msi`, `.dmg`) that 7-Zip can list (`--seven-zip`).

Exit status: 0 when no artifact contains a LUT, 1 when any does, 2 when an
artifact cannot be read.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

LUT_SUFFIX = ".cube"
INSTALLER_SUFFIXES = {".exe", ".msi", ".dmg"}


class ArtifactReadError(RuntimeError):
    """An artifact exists but its file list cannot be read."""


def is_lut_path(name: str) -> bool:
    return name.replace("\\", "/").rstrip("/").lower().endswith(LUT_SUFFIX)


def directory_entries(root: Path) -> list[str]:
    """Relative paths of every file under ``root``; links are listed, not followed."""
    entries: list[str] = []
    for path in root.rglob("*"):
        if path.is_file() or path.is_symlink():
            entries.append(path.relative_to(root).as_posix())
    return entries


def zip_entries(path: Path) -> list[str]:
    try:
        with zipfile.ZipFile(path) as archive:
            return archive.namelist()
    except zipfile.BadZipFile as error:
        raise ArtifactReadError(f"{path}: {error}") from error


def seven_zip_entries(path: Path, seven_zip: str | None) -> list[str]:
    """List an archive or installer with 7-Zip (`7z l -slt`)."""
    tool = seven_zip or shutil.which("7z") or shutil.which("7za")
    if tool is None:
        raise ArtifactReadError(f"{path}: 7-Zip is required to list this artifact (--seven-zip)")
    result = subprocess.run([tool, "l", "-slt", "-ba", str(path)], capture_output=True,
                            text=True, encoding="utf-8", errors="replace", check=False)
    if result.returncode != 0:
        raise ArtifactReadError(f"{path}: 7-Zip could not list the artifact: "
                                f"{result.stderr.strip() or result.stdout.strip()}")
    # 7-Zip prints native separators; report archive paths with "/".
    return [line[len("Path = "):].replace("\\", "/") for line in result.stdout.splitlines()
            if line.startswith("Path = ")]


def artifact_entries(path: Path, seven_zip: str | None = None) -> list[str]:
    if path.is_dir():
        return directory_entries(path)
    if not path.is_file():
        raise ArtifactReadError(f"{path}: the artifact does not exist")
    suffix = path.suffix.lower()
    if suffix == ".zip":
        return zip_entries(path)
    if suffix == ".7z" or suffix in INSTALLER_SUFFIXES:
        return seven_zip_entries(path, seven_zip)
    raise ArtifactReadError(f"{path}: unsupported artifact type {suffix!r}")


def lut_payloads(path: Path, seven_zip: str | None = None) -> list[str]:
    """Entries of ``path`` that are LUT files, sorted."""
    return sorted(entry for entry in artifact_entries(path, seven_zip) if is_lut_path(entry))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("artifacts", nargs="+", type=Path)
    parser.add_argument("--seven-zip", default=None, help="7-Zip executable for installers")
    args = parser.parse_args(argv)

    status = 0
    for artifact in args.artifacts:
        try:
            entries = artifact_entries(artifact, args.seven_zip)
        except ArtifactReadError as error:
            print(f"UNREADABLE {error}")
            status = max(status, 2)
            continue
        found = sorted(entry for entry in entries if is_lut_path(entry))
        if found:
            print(f"LUT PAYLOADS {artifact}: {len(found)} of {len(entries)} entries")
            for entry in found:
                print(f"  {entry}")
            status = max(status, 1)
        else:
            print(f"NO LUT PAYLOADS {artifact}: {len(entries)} entries checked")
    return status


if __name__ == "__main__":
    sys.exit(main())
