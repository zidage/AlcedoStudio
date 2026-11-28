#  Copyright 2026 Yurun Zi
#  SPDX-License-Identifier: GPL-3.0-only
#  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
"""The disk side of the file management fuzz (#1192): the test files, the listing rules of
ListMountDirectory, the fingerprint rule of ComputeSourceFileFingerprint, and the disk changes
of the driver (files, directories, permissions, links, locks, and macOS disk images).

The model reads the real disk with its own implementation of the listing rules, so a scan
prediction compares the C++ listing and comparison with an independent implementation on the
same files.
"""

from __future__ import annotations

import hashlib
import os
import random
import shutil
import stat
import subprocess
import sys
import unicodedata
from dataclasses import dataclass, field
from pathlib import Path

IS_WINDOWS = sys.platform == "win32"
IS_MACOS = sys.platform == "darwin"

REPO = Path(__file__).resolve().parents[2]
BASE_JPEG = REPO / "alcedo_studio" / "tests" / "resources" / "raster" / "untagged_8bit.jpg"
RAW_FIXTURES = REPO / "alcedo_studio" / "tests" / "resources" / "sample_images" / "ci_rawfiles"

# ComputeSourceFileFingerprint (source_file_fingerprint.hpp).
FINGERPRINT_BLOCK = 128 * 1024

# supported_file_type.hpp: kRasterImportExtensions and kRawExtensions.
RASTER_EXTENSIONS = {".jpg", ".jpeg", ".jpe", ".jfif", ".tif", ".tiff", ".png", ".exr"}
RAW_EXTENSIONS = {".3fr", ".arw", ".cr2", ".cr3", ".crw", ".dcr", ".dng", ".erf", ".fff", ".iiq",
                  ".kdc", ".mef", ".mos", ".mrw", ".nef", ".nrw", ".orf", ".pef", ".raf", ".raw",
                  ".rw2", ".rwl", ".sr2", ".srf", ".srw", ".x3f"}


def is_supported_name(name: str) -> bool:
    """CategoryForPath is not Other: an ASCII extension of 8 characters or less that names a
    raster or RAW type, compared without case."""
    stem, extension = os.path.splitext(name)
    if not extension or not stem and name.startswith("."):
        return False
    if len(extension) > 8 or any(ord(ch) > 0x7F for ch in extension):
        return False
    lower = extension.lower()
    return lower in RASTER_EXTENSIONS or lower in RAW_EXTENSIONS


def has_image_content(path: Path) -> bool:
    """True when the bytes of @p path start as a JPEG or a TIFF-based RAW file. An import of
    named files ('paths') classifies a file by its content, not by its extension
    (ImportServiceImpl::CreatePlaceholdersAndSubmit); the fuzz writes only these two kinds of
    image content."""
    try:
        with open(path, "rb") as stream:
            head = stream.read(4)
    except OSError:
        return False
    return head[:2] == b"\xff\xd8" or head in (b"II*\x00", b"MM\x00*")


def nfc(text: str) -> str:
    return unicodedata.normalize("NFC", text)


def wstring_key(text: str):
    """The order of std::wstring: UTF-16 code units on Windows, code points elsewhere."""
    return text.encode("utf-16-be", "surrogatepass") if IS_WINDOWS else text


def path_key(path: str | Path) -> str:
    """A comparison key of a local path. Windows and the default APFS volume compare names
    without case."""
    text = os.path.normpath(str(path))
    if IS_WINDOWS:
        return os.path.normcase(text)
    if IS_MACOS:
        return nfc(text).casefold()
    return text


def is_at_or_below(path: str | Path, root: str | Path) -> bool:
    """IsPathAtOrBelow: a lexical comparison element by element."""
    path_parts = Path(path_key(path)).parts
    root_parts = Path(path_key(root)).parts
    return len(path_parts) >= len(root_parts) and path_parts[:len(root_parts)] == root_parts


def relative_below(root: str | Path, path: str | Path) -> str | None:
    """MountRelativePathOf: the names below @p root joined with '/', each in NFC; None when
    @p path is not strictly below @p root."""
    path_parts = Path(os.path.normpath(str(path))).parts
    root_parts = Path(os.path.normpath(str(root))).parts
    if len(path_parts) <= len(root_parts):
        return None
    if Path(path_key(os.path.join(*path_parts[:len(root_parts)]))).parts != \
            Path(path_key(root)).parts:
        return None
    return "/".join(nfc(part) for part in path_parts[len(root_parts):])


# ------------------------------------------------------------------------------------------------
# Fingerprint
# ------------------------------------------------------------------------------------------------


def fingerprint_bytes(data: bytes) -> str:
    """The fingerprint rule over the bytes of a file: the 8-byte little-endian size, the first
    block, and the last block (all bytes once for a file of two blocks or less). The C++ code
    hashes these bytes with XXH3-128; the model hashes the same bytes with SHA-256, so two files
    have one model key exactly when they have one fingerprint."""
    size = len(data)
    if size <= 2 * FINGERPRINT_BLOCK:
        hashed = size.to_bytes(8, "little") + data
    else:
        hashed = size.to_bytes(8, "little") + data[:FINGERPRINT_BLOCK] + data[-FINGERPRINT_BLOCK:]
    return hashlib.sha256(hashed).hexdigest()


def read_fingerprint(path: Path) -> str | None:
    """The model key of the file at @p path, or None when ComputeSourceFileFingerprint returns
    an error (the file cannot be opened, for example while another process locks it)."""
    try:
        with open(path, "rb") as stream:
            size = os.fstat(stream.fileno()).st_size
            if size <= 2 * FINGERPRINT_BLOCK:
                data = stream.read(size)
                if len(data) != size:
                    return None
                return fingerprint_bytes(data)
            head = stream.read(FINGERPRINT_BLOCK)
            stream.seek(size - FINGERPRINT_BLOCK)
            tail = stream.read(FINGERPRINT_BLOCK)
            if os.fstat(stream.fileno()).st_size != size:
                return None
            hashed = size.to_bytes(8, "little") + head + tail
            return hashlib.sha256(hashed).hexdigest()
    except OSError:
        return None


# ------------------------------------------------------------------------------------------------
# Listing
# ------------------------------------------------------------------------------------------------


@dataclass
class ListedFile:
    rel: str
    size: int
    mtime_ns: int
    path: Path


@dataclass
class Listing:
    complete: bool = True
    directories: list[str] = field(default_factory=list)
    files: list[ListedFile] = field(default_factory=list)
    unreadable_directories: list[str] = field(default_factory=list)


def _is_link(entry: os.DirEntry) -> bool:
    if entry.is_symlink():
        return True
    is_junction = getattr(entry, "is_junction", None)
    return bool(is_junction and is_junction())


def list_mount_directory(root: Path) -> Listing:
    """ListMountDirectory (mount_directory_listing.hpp): the supported regular files and the
    directories below @p root; no symbolic link or junction is followed, and no entry whose name
    starts with a period is entered or listed. Names are in NFC."""
    listing = Listing()
    try:
        if not root.is_dir():
            return Listing(complete=False)
        entries = list(os.scandir(root))
    except OSError:
        return Listing(complete=False)
    stack: list[tuple[list[os.DirEntry], str]] = [(entries, "")]
    while stack:
        entries, rel = stack.pop()
        for entry in entries:
            if entry.name.startswith("."):
                continue
            name = nfc(entry.name)
            child = f"{rel}/{name}" if rel else name
            try:
                if _is_link(entry):
                    continue
                if entry.is_dir(follow_symlinks=False):
                    listing.directories.append(child)
                    try:
                        stack.append((list(os.scandir(entry.path)), child))
                    except OSError:
                        listing.unreadable_directories.append(child)
                    continue
                if entry.is_file(follow_symlinks=False) and is_supported_name(entry.name):
                    info = entry.stat(follow_symlinks=False)
                    listing.files.append(ListedFile(child, info.st_size, info.st_mtime_ns,
                                                    Path(entry.path)))
            except OSError:
                listing.unreadable_directories.append(rel)
    listing.directories.sort(key=wstring_key)
    listing.files.sort(key=lambda f: wstring_key(f.rel))
    listing.unreadable_directories = sorted(set(listing.unreadable_directories), key=wstring_key)
    return listing


# ------------------------------------------------------------------------------------------------
# Test images
# ------------------------------------------------------------------------------------------------

COM_SEGMENT_DATA = 65533


def raw_fixture(name: str | None = None) -> Path:
    files = sorted(p for p in RAW_FIXTURES.glob("*") if p.suffix.lower() in RAW_EXTENSIONS)
    if not files:
        raise FileNotFoundError(f"no RAW fixture in {RAW_FIXTURES} (git lfs pull)")
    if name is None:
        return min(files, key=lambda p: p.stat().st_size)
    for path in files:
        if path.name == name:
            return path
    raise FileNotFoundError(name)


def jpeg_content(seed: int, size: int, head: int = 0, middle: int = 0, tail: int = 0) -> bytes:
    """A valid JPEG of exactly @p size bytes (at least the base image plus 64): the base image
    with comment segments of seeded bytes after the start marker and seeded bytes after the end
    marker. @p head, @p middle, and @p tail select other bytes in the first block, in the middle
    (outside both blocks, for a file over two blocks), and in the last block, at the same size."""
    base = BASE_JPEG.read_bytes()
    size = max(size, len(base) + 64)
    rng = random.Random(seed)
    trailing = min(max(16, size // 8), 4096, size - len(base) - 40)
    payload_total = size - len(base) - trailing
    # payload_total bytes hold the comment segments: 4 marker and length bytes per segment.
    segments = []
    remaining = payload_total
    while remaining > 0:
        data_len = min(COM_SEGMENT_DATA, remaining - 4)
        if data_len < 1:
            # Too few bytes for one more segment: give them to the trailing bytes.
            trailing += remaining
            break
        segments.append(data_len)
        remaining -= data_len + 4
    payload = bytearray(rng.randbytes(sum(segments)))
    trail = bytearray(rng.randbytes(trailing))

    def data_offset(index: int) -> int:
        return 2 + (index // COM_SEGMENT_DATA + 1) * 4 + index

    if head and payload:
        variant = random.Random(f"{seed}/h{head}")
        for _ in range(8):
            payload[variant.randrange(min(len(payload), 1024))] ^= 0x5A
    if middle and size > 2 * FINGERPRINT_BLOCK + 4096:
        variant = random.Random(f"{seed}/m{middle}")
        low, high = FINGERPRINT_BLOCK + 64, size - FINGERPRINT_BLOCK - 64
        candidates = [i for i in range(0, len(payload), 97) if low <= data_offset(i) < high]
        for index in variant.sample(candidates, min(8, len(candidates))):
            payload[index] ^= 0xA5
    if tail and trail:
        variant = random.Random(f"{seed}/t{tail}")
        for _ in range(8):
            trail[variant.randrange(len(trail))] ^= 0x3C
    out = bytearray(base[:2])
    offset = 0
    for data_len in segments:
        out += b"\xff\xfe" + (data_len + 2).to_bytes(2, "big") + payload[offset:offset + data_len]
        offset += data_len
    out += base[2:] + trail
    assert len(out) == size, (len(out), size)
    return bytes(out)


def content_bytes(spec: dict) -> bytes:
    """The bytes of a content spec of the operation log."""
    kind = spec["kind"]
    if kind == "jpeg":
        return jpeg_content(spec["seed"], spec["size"], spec.get("head", 0),
                            spec.get("middle", 0), spec.get("tail", 0))
    if kind == "raw":
        return raw_fixture(spec.get("fixture")).read_bytes()
    if kind == "empty":
        return b""
    if kind == "text":
        return random.Random(spec["seed"]).randbytes(spec.get("size", 64))
    if kind == "cut_jpeg":
        return jpeg_content(spec["seed"], spec["size"])[: spec["size"] // 2]
    raise ValueError(f"unknown content kind {kind}")


# ------------------------------------------------------------------------------------------------
# Disk changes
# ------------------------------------------------------------------------------------------------


class DiskTools:
    """The disk changes that need a platform API: locks, read permission, and links. Holds the
    locks of the run, so the driver can release them."""

    def __init__(self) -> None:
        self.locks: dict[str, object] = {}
        self.unreadable: set[str] = set()

    # Locks: Windows opens the file with share mode 0, so no other process can open it
    # (ComputeSourceFileFingerprint returns kCannotOpen). Other platforms have no mandatory lock.
    @staticmethod
    def can_lock() -> bool:
        return IS_WINDOWS

    def lock(self, path: Path) -> bool:
        if not IS_WINDOWS or str(path) in self.locks:
            return False
        import ctypes
        from ctypes import wintypes
        create = ctypes.windll.kernel32.CreateFileW
        create.restype = wintypes.HANDLE
        create.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
                           wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        handle = create(str(path), 0x80000000, 0, None, 3, 0x80, None)
        if handle in (None, wintypes.HANDLE(-1).value):
            return False
        self.locks[str(path)] = handle
        return True

    def unlock(self, path: Path) -> bool:
        handle = self.locks.pop(str(path), None)
        if handle is None:
            return False
        import ctypes
        ctypes.windll.kernel32.CloseHandle(handle)
        return True

    def unlock_all(self) -> None:
        for path in list(self.locks):
            self.unlock(Path(path))

    def is_locked(self, path: Path) -> bool:
        return str(path) in self.locks

    # Read permission of a directory.
    def make_unreadable(self, path: Path) -> bool:
        if str(path) in self.unreadable:
            return False
        if IS_WINDOWS:
            user = os.environ.get("USERNAME", "")
            result = subprocess.run(["icacls", str(path), "/deny", f"{user}:(RD)"],
                                    capture_output=True)
            if result.returncode != 0:
                return False
        else:
            if os.geteuid() == 0:
                return False
            os.chmod(path, 0)
        self.unreadable.add(str(path))
        return True

    def make_readable(self, path: Path) -> bool:
        if str(path) not in self.unreadable:
            return False
        if IS_WINDOWS:
            user = os.environ.get("USERNAME", "")
            subprocess.run(["icacls", str(path), "/remove:d", user], capture_output=True)
        else:
            os.chmod(path, stat.S_IRWXU)
        self.unreadable.discard(str(path))
        return True

    def restore_all(self) -> None:
        self.unlock_all()
        for path in list(self.unreadable):
            self.make_readable(Path(path))

    @staticmethod
    def symlink(target: Path, link: Path, directory: bool) -> bool:
        """A symbolic link, or on Windows a junction when the process may not create symbolic
        links. Returns False when neither can be created."""
        try:
            os.symlink(target, link, target_is_directory=directory)
            return True
        except OSError:
            pass
        if IS_WINDOWS and directory:
            result = subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(target)],
                                    capture_output=True)
            return result.returncode == 0
        return False

    @staticmethod
    def hardlink(target: Path, link: Path) -> bool:
        try:
            os.link(target, link)
            return True
        except OSError:
            return False


def remove_tree(path: Path) -> None:
    def on_error(function, failed, _info):
        try:
            os.chmod(failed, stat.S_IRWXU)
            function(failed)
        except OSError:
            pass

    if path.is_symlink() or (IS_WINDOWS and os.path.isjunction(path)):
        os.unlink(path) if not path.is_dir() or path.is_symlink() else os.rmdir(path)
        return
    if path.is_dir():
        shutil.rmtree(path, onexc=on_error) if sys.version_info >= (3, 12) else \
            shutil.rmtree(path, onerror=on_error)
        if path.exists() and IS_WINDOWS:
            # A deny entry that an earlier run left in the tree: reset the permissions of the
            # tree to the inherited ones, then remove it again.
            subprocess.run(["icacls", str(path), "/reset", "/T", "/C", "/Q"], capture_output=True)
            shutil.rmtree(path, ignore_errors=True)
    elif path.exists():
        path.unlink()


# ------------------------------------------------------------------------------------------------
# macOS disk images
# ------------------------------------------------------------------------------------------------


class Volumes:
    """macOS disk images for real volume events (Mounted Folder Plan, #1020, #1021): create,
    attach at a mount point, detach, and rename. The other platforms have no volume step."""

    def __init__(self, work: Path) -> None:
        self.work = work
        self.images: dict[str, Path] = {}
        self.attached: dict[str, Path] = {}

    @staticmethod
    def available() -> bool:
        return IS_MACOS and shutil.which("hdiutil") is not None

    @staticmethod
    def _run(command: list[str]) -> None:
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode != 0:
            raise RuntimeError(f"{' '.join(command)} exited with {result.returncode}: "
                               f"{result.stdout.strip()} {result.stderr.strip()}")

    def create(self, handle: str, name: str) -> Path:
        self.work.mkdir(parents=True, exist_ok=True)
        image = self.work / f"{handle}.dmg"
        self._run(["hdiutil", "create", "-size", "64m", "-fs", "APFS", "-volname", name, "-ov",
                   str(image)])
        self.images[handle] = image
        return image

    def attach(self, handle: str, mount_point: Path) -> None:
        mount_point.mkdir(parents=True, exist_ok=True)
        self._run(["hdiutil", "attach", str(self.images[handle]), "-mountpoint",
                   str(mount_point), "-nobrowse"])
        self.attached[handle] = mount_point

    def detach(self, handle: str) -> None:
        mount_point = self.attached.pop(handle)
        self._run(["hdiutil", "detach", str(mount_point), "-force"])

    def rename(self, handle: str, name: str) -> None:
        self._run(["diskutil", "rename", str(self.attached[handle]), name])

    def detach_all(self) -> None:
        for handle in list(self.attached):
            try:
                self.detach(handle)
            except RuntimeError:
                pass
