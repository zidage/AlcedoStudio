#!/usr/bin/env python3
"""Build and verify the AI model package archives and the signed AI model feed.

The formats are those that the C++ installer reads (AI Models plan, AM1 completion record):

- ``package-inventory.json``: ``schema``, ``kind``, ``id``, ``model_set_version``, ``revision``,
  ``platform``, and ``files`` (``path``, ``size``, ``sha256`` of every other file).
- ``model_set.json``: ``schema``, ``kind``, ``id``, ``model_set_version``, ``revision``, plus the
  platform and the model files by role.
- The feed entry: ``id``, ``name``, ``model_set_version``, ``revision``, ``licenses``, and
  ``artifacts`` keyed by platform (``url``, ``size``, ``sha256``, ``file_count``,
  ``unpacked_bytes``, ``inventory_sha256``).

A model set definition (``ai_mask_model_set_v1.json``) pins the size and SHA-256 of every model
file. Requires py7zr (``pip install py7zr``) and cryptography.
"""

from __future__ import annotations

import base64
import contextlib
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import tempfile

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
import py7zr

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_DEFINITION = Path(__file__).resolve().parent / "ai_mask_model_set_v1.json"
DEFAULT_PUBLIC_BASE = "https://static.aoraw.org"
DEFAULT_PREFIX = "ai-models/v1"
MANIFEST_KIND = "alcedo-ai-model-packages"
INVENTORY_KIND = "alcedo-ai-model-package-inventory"
MODEL_SET_KIND = "alcedo-ai-model-set"
DEFINITION_KIND = "alcedo-ai-model-set-definition"
INVENTORY_FILE_NAME = "package-inventory.json"
MODEL_SET_FILE_NAME = "model_set.json"
NOTICE_FILE_NAME = "NOTICE.txt"
RECEIPT_FILE_NAME = "installed.json"
PLATFORMS = ("windows-x86_64", "macos-arm64")
ARCHIVE_FILTERS = [{"id": py7zr.FILTER_LZMA2, "preset": 9}]
# Every staged file gets this modification time, so the same input gives the same archive bytes.
ARCHIVE_FILE_TIME = 1767225600  # 2026-01-01T00:00:00Z

_SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
_ID_PATTERN = re.compile(r"^[a-z0-9]+(?:[._-][a-z0-9]+)*$")
_REVISION_PATTERN = re.compile(r"^r[1-9][0-9]{0,8}$")


class AiModelPackageError(ValueError):
    """Raised when a source file, a package, or a feed does not agree with its pinned values."""


def is_safe_relative_path(path: str) -> bool:
    """Match IsSafePackageRelativePath: `/` segments, no `.`/`..`, drive, NUL, or line break."""
    if not path or any(character in path for character in "\0\n\r\\:") or path.startswith("/"):
        return False
    return all(segment not in ("", ".", "..") for segment in path.split("/"))


def hash_file(path: Path) -> tuple[str, int]:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
            size += len(block)
    return digest.hexdigest(), size


def directory_files(root: Path) -> list[str]:
    """Relative POSIX paths of the regular files under ``root``, in path-component order."""
    files = []
    for path in root.rglob("*"):
        if path.is_symlink():
            raise AiModelPackageError(f"a model directory contains a link: {path}")
        if path.is_file():
            files.append(path.relative_to(root).as_posix())
    return sorted(files, key=lambda value: value.split("/"))


def directory_sha256(root: Path) -> str:
    """The lab directory rule (convert_models.directory_sha256) for a Core ML ``.mlmodelc``.

    SHA-256 over ``relative path NUL file SHA-256`` of every file in path-component order.
    """
    digest = hashlib.sha256()
    for relative in directory_files(root):
        digest.update(relative.encode("utf-8") + b"\0")
        digest.update(hash_file(root / relative)[0].encode("ascii"))
    return digest.hexdigest()


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise AiModelPackageError(message)


def load_definition(path: Path) -> dict:
    """Read and check a model set definition; source paths become absolute."""
    definition = json.loads(path.read_text(encoding="utf-8"))
    _require(definition.get("schema") == 1 and definition.get("kind") == DEFINITION_KIND,
             f"{path} is not a model set definition")
    _require(bool(_ID_PATTERN.match(definition["id"])), "the package ID is not a valid slug")
    version = definition["model_set_version"]
    _require(isinstance(version, int) and version >= 1, "model_set_version must be positive")
    _require(bool(_REVISION_PATTERN.match(definition["revision"])),
             "revision must be r1, r2, ...")
    base = path.resolve().parent
    definition["notice"] = base / definition["notice"]
    license_names = set()
    for license in definition["licenses"]:
        _require(license["file"].startswith("licenses/") and
                 is_safe_relative_path(license["file"]), f"license path {license['file']}")
        license["source"] = base / license["source"]
        license_names.add(license["name"])
    _require(len(license_names) == len(definition["licenses"]), "license names must be unique")
    platform_paths = {platform: {item["path"] for item in definition["platforms"][platform]}
                      for platform in definition["platforms"]}
    _require(set(platform_paths) <= set(PLATFORMS), "unknown platform in the definition")
    for model in definition["models"]:
        _require(set(model["licenses"]) <= license_names,
                 f"model {model['role']} names an unknown license")
        _require(set(model["files"]) == set(platform_paths),
                 f"model {model['role']} needs one file per platform")
        for platform, file_path in model["files"].items():
            _require(file_path in platform_paths[platform],
                     f"model {model['role']} names a file that is not pinned: {file_path}")
    for platform, paths in platform_paths.items():
        used = {model["files"][platform] for model in definition["models"]}
        _require(used == paths, f"{platform} pins a file that no model uses")
    return definition


def license_text(license: dict) -> bytes:
    """The license text that the package contains; ``end_before`` cuts an appended section."""
    data = license["source"].read_bytes()
    if "end_before" in license:
        marker = license["end_before"].encode("utf-8")
        _require(marker in data, f"{license['source']} has no line {license['end_before']!r}")
        data = data[:data.index(marker)].rstrip() + b"\n"
    if "sha256" in license:
        _require(hashlib.sha256(data).hexdigest() == license["sha256"],
                 f"{license['source']} differs from its pinned SHA-256")
    return data


def check_source_file(source: Path, pin: dict) -> None:
    """Compare one source model file or ``.mlmodelc`` directory with its pinned values."""
    if "files" not in pin:
        _require(source.is_file() and not source.is_symlink(),
                 f"model file is missing: {source}")
        sha256, size = hash_file(source)
        _require(size == pin["size"] and sha256 == pin["sha256"],
                 f"{pin['path']} differs from its pinned size and SHA-256 "
                 f"({size} bytes, {sha256})")
        return
    _require(source.is_dir() and not source.is_symlink(), f"model directory is missing: {source}")
    pinned = {item["path"]: item for item in pin["files"]}
    found = directory_files(source)
    _require(set(found) == set(pinned),
             f"{pin['path']} has other files than its pins: {sorted(set(found) ^ set(pinned))}")
    total = 0
    for relative in found:
        sha256, size = hash_file(source / relative)
        _require(size == pinned[relative]["size"] and sha256 == pinned[relative]["sha256"],
                 f"{pin['path']}/{relative} differs from its pinned size and SHA-256")
        total += size
    _require(total == pin["size"], f"{pin['path']} has {total} bytes, pinned {pin['size']}")
    _require(directory_sha256(source) == pin["directory_sha256"],
             f"{pin['path']} differs from its pinned directory SHA-256")


def model_set_bytes(definition: dict, platform: str) -> bytes:
    document = {
        "schema": 1,
        "kind": MODEL_SET_KIND,
        "id": definition["id"],
        "model_set_version": definition["model_set_version"],
        "revision": definition["revision"],
        "platform": platform,
        "models": [{"role": model["role"], "path": model["files"][platform],
                    "licenses": model["licenses"]} for model in definition["models"]],
    }
    return (json.dumps(document, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def stage_package(definition: dict, platform: str, source_dir: Path, staging: Path) -> None:
    """Check every pinned source file and copy the complete package content into ``staging``.

    The copies are checked again, so a source change during the copy cannot enter the archive.
    """
    _require(not staging.exists(), f"staging directory exists: {staging}")
    staging.mkdir(parents=True)
    for pin in definition["platforms"][platform]:
        source = source_dir / pin["path"]
        check_source_file(source, pin)
        target = staging / pin["path"]
        if "files" in pin:
            shutil.copytree(source, target)
        else:
            shutil.copyfile(source, target)
        check_source_file(target, pin)
    (staging / MODEL_SET_FILE_NAME).write_bytes(model_set_bytes(definition, platform))
    (staging / NOTICE_FILE_NAME).write_bytes(definition["notice"].read_bytes())
    for license in definition["licenses"]:
        target = staging / license["file"]
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(license_text(license))


def build_inventory(definition: dict, platform: str, staging: Path) -> dict:
    """List every staged file except the inventory, sorted by UTF-8 path bytes."""
    files = []
    for relative in directory_files(staging):
        _require(is_safe_relative_path(relative), f"unsafe package path: {relative}")
        _require(relative not in (INVENTORY_FILE_NAME, RECEIPT_FILE_NAME),
                 f"the staged content must not contain {relative}")
        sha256, size = hash_file(staging / relative)
        files.append({"path": relative, "size": size, "sha256": sha256})
    files.sort(key=lambda item: item["path"].encode("utf-8"))
    folded = {item["path"].lower() for item in files}
    _require(len(folded) == len(files), "package paths collide on a case-insensitive file system")
    return {
        "schema": 1,
        "kind": INVENTORY_KIND,
        "id": definition["id"],
        "model_set_version": definition["model_set_version"],
        "revision": definition["revision"],
        "platform": platform,
        "files": files,
    }


def serialize_inventory(inventory: dict) -> bytes:
    return (json.dumps(inventory, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def archive_key(prefix: str, definition: dict, platform: str) -> str:
    package_id = definition["id"]
    version = definition["model_set_version"]
    revision = definition["revision"]
    return (f"{prefix}/packages/{package_id}/v{version}/{revision}/"
            f"{package_id}-v{version}-{revision}-{platform}.7z")


def write_package_archive(staging: Path, inventory: dict, archive_path: Path) -> bytes:
    """Write the staged files and the inventory into a new 7z archive; return the inventory bytes.

    Every entry gets the time ``ARCHIVE_FILE_TIME``, so the archive bytes depend only on the
    content.
    """
    _require(not archive_path.exists(), f"archive already exists: {archive_path}")
    archive_path.parent.mkdir(parents=True, exist_ok=True)
    inventory_bytes = serialize_inventory(inventory)
    (staging / INVENTORY_FILE_NAME).write_bytes(inventory_bytes)
    names = [item["path"] for item in inventory["files"]] + [INVENTORY_FILE_NAME]
    for name in names:
        os.utime(staging / name, (ARCHIVE_FILE_TIME, ARCHIVE_FILE_TIME))
    with py7zr.SevenZipFile(archive_path, "w", filters=ARCHIVE_FILTERS) as archive:
        for name in sorted(names, key=lambda value: value.encode("utf-8")):
            archive.write(staging / name, name)
    return inventory_bytes


def build_package(definition: dict, platform: str, source_dir: Path, output_dir: Path,
                  prefix: str, public_base: str) -> dict:
    """Stage, archive, and re-verify one platform package; return its feed artifact."""
    staging = output_dir / "staging" / platform
    stage_package(definition, platform, source_dir, staging)
    inventory = build_inventory(definition, platform, staging)
    key = archive_key(prefix, definition, platform)
    archive_path = output_dir / "packages" / Path(key).name
    inventory_bytes = write_package_archive(staging, inventory, archive_path)
    sha256, size = hash_file(archive_path)
    artifact = {
        "url": f"{public_base.rstrip('/')}/{key}",
        "size": size,
        "sha256": sha256,
        "file_count": len(inventory["files"]),
        "unpacked_bytes": sum(item["size"] for item in inventory["files"]),
        "inventory_sha256": hashlib.sha256(inventory_bytes).hexdigest(),
    }
    entry = feed_entry(definition, {platform: artifact})
    verify_package_archive(archive_path, entry, platform, output_dir / "verify")
    shutil.rmtree(staging)
    with contextlib.suppress(OSError):
        staging.parent.rmdir()  # Only when no other platform is staged.
    return artifact


def feed_entry(definition: dict, artifacts: dict) -> dict:
    return {
        "id": definition["id"],
        "name": definition["name"],
        "model_set_version": definition["model_set_version"],
        "revision": definition["revision"],
        "licenses": [{"name": license["name"], "file": license["file"]}
                     for license in definition["licenses"]],
        "artifacts": {platform: artifacts[platform] for platform in sorted(artifacts)},
    }


def manifest_bytes(sequence: int, entries: list[dict]) -> bytes:
    keys = [(entry["id"], entry["model_set_version"]) for entry in entries]
    _require(len(keys) == len(set(keys)), "the feed lists an (id, model_set_version) twice")
    manifest = {
        "schema": 1,
        "kind": MANIFEST_KIND,
        "sequence": sequence,
        "packages": sorted(entries, key=lambda item: (item["id"], item["model_set_version"])),
    }
    return (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def verify_package_archive(archive_path: Path, entry: dict, platform: str,
                           work_root: Path) -> dict:
    """Extract ``archive_path`` into a new directory and check it as the C++ installer does.

    Checks the archive size and SHA-256, the entry paths, the inventory digest and fields, every
    listed file, ``model_set.json``, and the license files of ``entry``. Returns the inventory.
    """
    artifact = entry["artifacts"][platform]
    sha256, size = hash_file(archive_path)
    _require(size == artifact["size"] and sha256 == artifact["sha256"],
             f"{archive_path.name} differs from its feed size and SHA-256")
    with py7zr.SevenZipFile(archive_path, "r") as archive:
        entries = archive.list()
    names = []
    for item in entries:
        name = item.filename.replace("\\", "/").rstrip("/")
        _require(is_safe_relative_path(name) and not getattr(item, "is_symlink", False),
                 f"archive entry is not a safe regular file: {name!r}")
        if not item.is_directory:
            names.append(name)
    _require(len({name.lower() for name in names}) == len(names), "archive has duplicate entries")
    _require(len(names) <= artifact["file_count"] + 1, "the archive holds more files than its feed")
    work_root.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix="verify-", dir=work_root))
    try:
        with py7zr.SevenZipFile(archive_path, "r") as archive:
            archive.extractall(path=directory)
        inventory_bytes = (directory / INVENTORY_FILE_NAME).read_bytes()
        _require(hashlib.sha256(inventory_bytes).hexdigest() == artifact["inventory_sha256"],
                 "the package inventory differs from the signed package descriptor")
        inventory = json.loads(inventory_bytes)
        listed = {item["path"]: item for item in inventory["files"]}
        _require(inventory["schema"] == 1 and inventory["kind"] == INVENTORY_KIND and
                 inventory["id"] == entry["id"] and
                 inventory["model_set_version"] == entry["model_set_version"] and
                 inventory["revision"] == entry["revision"] and
                 inventory["platform"] == platform and
                 len(listed) == artifact["file_count"] and
                 sum(item["size"] for item in listed.values()) == artifact["unpacked_bytes"],
                 "the archive inventory differs from the signed package descriptor")
        _require(set(names) == set(listed) | {INVENTORY_FILE_NAME},
                 "the archive entries differ from the inventory file list")
        _require(RECEIPT_FILE_NAME not in listed, f"the archive contains {RECEIPT_FILE_NAME}")
        for relative, item in listed.items():
            file_sha256, file_size = hash_file(directory / relative)
            _require(file_sha256 == item["sha256"] and file_size == item["size"],
                     f"an extracted file differs from the package inventory: {relative}")
        model_set = json.loads((directory / MODEL_SET_FILE_NAME).read_text(encoding="utf-8"))
        _require(model_set["schema"] == 1 and model_set["kind"] == MODEL_SET_KIND and
                 model_set["id"] == entry["id"] and
                 model_set["model_set_version"] == entry["model_set_version"] and
                 model_set["revision"] == entry["revision"],
                 "model_set.json differs from the signed package descriptor")
        for license in entry["licenses"]:
            _require(license["file"] in listed, f"the archive does not contain {license['file']}")
        return inventory
    finally:
        shutil.rmtree(directory, ignore_errors=True)


def verify_signature(manifest_path: Path, signature_path: Path, public_key_base64: str) -> None:
    """Verify the detached Ed25519 signature over the exact manifest bytes."""
    raw_key = base64.b64decode(public_key_base64, validate=True)
    raw_signature = base64.b64decode(signature_path.read_bytes().strip(), validate=True)
    if len(raw_key) != 32 or len(raw_signature) != 64:
        raise AiModelPackageError("the public key or signature length is not valid")
    try:
        Ed25519PublicKey.from_public_bytes(raw_key).verify(raw_signature,
                                                           manifest_path.read_bytes())
    except InvalidSignature as error:
        raise AiModelPackageError("the AI model feed signature is not valid") from error
