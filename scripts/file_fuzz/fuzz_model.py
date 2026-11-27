#  Copyright 2026 Yurun Zi
#  SPDX-License-Identifier: GPL-3.0-only
#  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
"""The reference model of the file management fuzz (#1192): the library folder tree, the photos
of each folder, the mounts, and the scan prediction.

The model names each folder, photo, and mount by a handle ('f3', 'p7', 'm1'). The operation log
uses only handles, so a shortened log replays; the run binds each handle to its database id
when the session answers.

The scan prediction ports the rules of BuildMountScanChangeList (mount_scan_change_list.hpp,
Mounted Folder Plan section 2.4) and of MountScanRun::Execute (mount_service.cpp): the join by
relative path, the pairs of a lost record and a new path with one fingerprint (the same file name
first, then the sorted order), adoption of an ordinary photo at a new path, and the folders of
the directories that hold a listed file.
"""

from __future__ import annotations

import copy
import os
import unicodedata
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

import fuzz_disk
from fuzz_disk import Listing, ListedFile, wstring_key

ROOT = "f0"

# FolderEditRefusal names (folder_commands.cpp).
INVALID_NAME = "rejected:invalid_folder_name"
NAME_TAKEN = "rejected:folder_name_taken"
UNKNOWN_FOLDER = "rejected:unknown_folder"
ROOT_CANNOT_CHANGE = "rejected:root_folder_cannot_change"
OWN_SUBTREE = "rejected:move_into_own_subtree"
MOUNTED_READ_ONLY = "rejected:mounted_folder_structure_is_read_only"
# MountError names (mount_commands.cpp).
NESTED_MOUNT = "rejected:nested_mount"
UNREACHABLE = "rejected:directory_unreachable"
UNKNOWN_MOUNT = "rejected:unknown_mount"
NOT_MISSING = "rejected:photo_is_not_missing"
OUTSIDE_MOUNT = "rejected:file_outside_mount"
NOT_LISTED = "rejected:file_is_not_listed"
HAS_PHOTO = "rejected:file_has_photo"
FILE_UNREADABLE = "rejected:file_unreadable"
# library.delete refusals of mounted photos.
PHOTO_PRESENT = "rejected:mounted_photo_is_present"
PHOTO_MISSING = "rejected:mounted_photo_is_missing"


def qt_trimmed(text: str) -> str:
    """QString::trimmed: removes QChar::isSpace characters at both ends (ASCII 9-13 and 32,
    U+0085, U+00A0, and the Unicode separators)."""
    def is_space(ch: str) -> bool:
        code = ord(ch)
        return (9 <= code <= 13 or code in (0x20, 0x85, 0xA0)
                or unicodedata.category(ch) in ("Zs", "Zl", "Zp"))

    start, end = 0, len(text)
    while start < end and is_space(text[start]):
        start += 1
    while end > start and is_space(text[end - 1]):
        end -= 1
    return text[start:end]


def is_valid_folder_name(name: str) -> bool:
    """AlbumBrowseService::IsValidFolderName on the trimmed name."""
    if name in ("", ".", ".."):
        return False
    return not any(ch in "/\\" or ord(ch) < 0x20 or ord(ch) == 0x7F for ch in name)


@dataclass
class Folder:
    handle: str
    parent: str | None
    name: str
    fid: int | None = None
    mount: str | None = None
    rel: str | None = None  # relative path in the mount; '' for the mount root


@dataclass
class Photo:
    handle: str
    path: str                       # the image path: the stored path of the file
    eid: int | None = None
    links: set[str] = field(default_factory=set)  # ordinary folders other than the root
    mount: str | None = None
    rel: str | None = None
    home: str | None = None         # the mounted folder of the file
    state: str = "present"
    size: int = 0
    mtime_ns: int = 0
    fingerprint: str | None = None
    # A photo of an import whose result the model cannot predict (a damaged file, a crash
    # during the import): the next read keeps it when the library has it and drops it otherwise.
    uncertain: bool = False


@dataclass
class Mount:
    handle: str
    root: str
    folder: str
    mid: int | None = None
    state: str = "not_scanned"


@dataclass
class ScanPrediction:
    """The counts of library.sync and the changes that the model applies after the scan."""

    status: str
    new: int = 0
    moved: int = 0
    changed: int = 0
    missing: int = 0
    returned: int = 0
    not_imported: int = 0
    apply: Callable[[], None] | None = None

    def counts(self) -> dict:
        return {"status": self.status, "new": self.new, "moved": self.moved,
                "changed": self.changed, "missing": self.missing, "returned": self.returned,
                "not_imported": self.not_imported}


class LibraryModel:
    def __init__(self) -> None:
        self.folders: dict[str, Folder] = {ROOT: Folder(ROOT, None, "", fid=0)}
        self.photos: dict[str, Photo] = {}
        self.mounts: dict[str, Mount] = {}
        # Handles of the objects that an operation creates: '<kind><operation id>.<n>'. The id
        # of an operation stays in the log, so a shortened log gives the same handles.
        self.op_id = 0
        self.op_counter = 0

    def begin_operation(self, op_id: int) -> None:
        self.op_id = op_id
        self.op_counter = 0

    def new_handle(self, kind: str) -> str:
        self.op_counter += 1
        return f"{kind}{self.op_id}.{self.op_counter}"

    def copy(self) -> "LibraryModel":
        return copy.deepcopy(self)

    # -------------------------------------------------------------------------------------------
    # Folders
    # -------------------------------------------------------------------------------------------

    def path_of(self, handle: str) -> str:
        names = []
        folder = self.folders[handle]
        while folder.parent is not None:
            names.append(folder.name)
            folder = self.folders[folder.parent]
        return "/" + "/".join(reversed(names))

    def children(self, handle: str) -> list[Folder]:
        return [f for f in self.folders.values() if f.parent == handle]

    def subtree(self, handle: str) -> list[str]:
        out = [handle]
        index = 0
        while index < len(out):
            out += [f.handle for f in self.children(out[index])]
            index += 1
        return out

    def is_mounted(self, handle: str) -> bool:
        return self.folders[handle].mount is not None

    def child_named(self, parent: str, name: str) -> Folder | None:
        for folder in self.children(parent):
            if folder.name == name:
                return folder
        return None

    def expect_create(self, parent: str | None, name: str) -> set[str]:
        """The accepted outcomes of library.folder.create; 'ok' when no refusal applies."""
        if parent is None or parent not in self.folders:
            return {UNKNOWN_FOLDER}
        trimmed = qt_trimmed(name)
        refusals = set()
        if not is_valid_folder_name(trimmed):
            refusals.add(INVALID_NAME)
        else:
            if self.child_named(parent, trimmed) is not None:
                refusals.add(NAME_TAKEN)
        if self.is_mounted(parent):
            refusals.add(MOUNTED_READ_ONLY)
        return refusals or {"ok"}

    def apply_create(self, handle: str, parent: str, name: str, fid: int) -> None:
        self.folders[handle] = Folder(handle, parent, qt_trimmed(name), fid=fid)

    def expect_rename(self, folder: str | None, name: str) -> set[str]:
        if folder is None or folder not in self.folders:
            return {UNKNOWN_FOLDER}
        refusals = set()
        if folder == ROOT:
            refusals.add(ROOT_CANNOT_CHANGE)
        trimmed = qt_trimmed(name)
        if not is_valid_folder_name(trimmed):
            refusals.add(INVALID_NAME)
        elif folder != ROOT:
            other = self.child_named(self.folders[folder].parent, trimmed)
            if other is not None and other.handle != folder:
                refusals.add(NAME_TAKEN)
        if folder != ROOT and self.is_mounted(folder):
            refusals.add(MOUNTED_READ_ONLY)
        return refusals or {"ok"}

    def apply_rename(self, folder: str, name: str) -> None:
        self.folders[folder].name = qt_trimmed(name)

    def expect_move(self, folder: str | None, parent: str | None) -> set[str]:
        if folder is None or folder not in self.folders or parent is None or \
                parent not in self.folders:
            return {UNKNOWN_FOLDER}
        if folder == ROOT:
            return {ROOT_CANNOT_CHANGE}
        refusals = set()
        if parent in self.subtree(folder):
            refusals.add(OWN_SUBTREE)
        if self.is_mounted(folder) or self.is_mounted(parent):
            refusals.add(MOUNTED_READ_ONLY)
        current = self.folders[folder].parent
        if parent != current and OWN_SUBTREE not in refusals:
            if self.child_named(parent, self.folders[folder].name) is not None:
                refusals.add(NAME_TAKEN)
        return refusals or {"ok"}

    def apply_move(self, folder: str, parent: str) -> None:
        self.folders[folder].parent = parent

    def expect_delete_folder(self, folder: str | None) -> set[str]:
        if folder is None or folder not in self.folders:
            return {UNKNOWN_FOLDER}
        if folder == ROOT:
            return {ROOT_CANNOT_CHANGE}
        if self.is_mounted(folder):
            return {MOUNTED_READ_ONLY}
        return {"ok"}

    def apply_delete_folder(self, folder: str) -> None:
        for handle in self.subtree(folder):
            del self.folders[handle]
            for photo in self.photos.values():
                photo.links.discard(handle)

    # -------------------------------------------------------------------------------------------
    # Photos
    # -------------------------------------------------------------------------------------------

    def photos_in(self, folder: str) -> set[str]:
        """The photos that library.list of @p folder lists: every photo for the root (All
        Photos), else the photos linked into the folder."""
        if folder == ROOT:
            return set(self.photos)
        return {p.handle for p in self.photos.values() if folder in p.links or p.home == folder}

    def mount_of_path(self, path: str | Path) -> Mount | None:
        for mount in self.mounts.values():
            if fuzz_disk.is_at_or_below(path, mount.root):
                return mount
        return None

    def add_photo(self, path: str, folder: str) -> Photo:
        photo = Photo(self.new_handle("p"), path=str(path))
        if folder != ROOT:
            photo.links.add(folder)
        self.photos[photo.handle] = photo
        return photo

    def remove_photo(self, handle: str) -> None:
        del self.photos[handle]

    def eid_of(self, handle: str) -> int | None:
        photo = self.photos.get(handle)
        return None if photo is None else photo.eid

    def photo_of_eid(self, eid: int) -> Photo | None:
        for photo in self.photos.values():
            if photo.eid == eid:
                return photo
        return None

    # -------------------------------------------------------------------------------------------
    # Mounts
    # -------------------------------------------------------------------------------------------

    def mount_folder(self, mount: Mount, rel: str) -> Folder | None:
        for folder in self.folders.values():
            if folder.mount == mount.handle and folder.rel == rel:
                return folder
        return None

    def expect_mount(self, path: Path, ignore: str | None = None) -> set[str]:
        """The accepted outcomes of library.mount of @p path, or of library.mount.locate of the
        mount @p ignore to @p path."""
        refusals = set()
        try:
            if not path.is_dir():
                refusals.add(UNREACHABLE)
            else:
                with os.scandir(path):
                    pass
        except OSError:
            refusals.add(UNREACHABLE)
        # MountService compares the canonical path: a link names its target.
        path = Path(os.path.realpath(path))
        for mount in self.mounts.values():
            if mount.handle == ignore:
                continue
            if fuzz_disk.is_at_or_below(path, mount.root) or \
                    fuzz_disk.is_at_or_below(mount.root, path):
                refusals.add(NESTED_MOUNT)
        return refusals or {"ok"}

    def add_mount(self, path: Path, mid: int, fid: int, handle: str | None = None) -> Mount:
        handle = handle or self.new_handle("m")
        # MountService::CreateMount stores the canonical path (std::filesystem::canonical): links
        # resolved, and on Windows the case of the names on the disk.
        path = Path(os.path.realpath(path))
        # FileSystem::Create: a folder name that the parent has gets '@' until it is free.
        name = path.name
        while self.child_named(ROOT, name) is not None:
            name += "@"
        folder = Folder(self.new_handle("f"), ROOT, name, fid=fid, mount=handle, rel="")
        self.folders[folder.handle] = folder
        mount = Mount(handle, str(path), folder.handle, mid=mid)
        self.mounts[handle] = mount
        return mount

    def unmount(self, handle: str) -> None:
        mount = self.mounts.pop(handle)
        for folder in [f for f in self.folders.values() if f.mount == handle]:
            del self.folders[folder.handle]
        for photo in self.photos.values():
            if photo.mount == handle:
                photo.mount = None
                photo.rel = None
                photo.home = None
                photo.state = "present"
        del mount

    def mounted_photos(self, mount: str) -> list[Photo]:
        return [p for p in self.photos.values() if p.mount == mount]

    def predict_scan(self, mount_handle: str,
                     fingerprint: Callable[[ListedFile], str | None] | None = None,
                     listing: Listing | None = None) -> ScanPrediction:
        """The result of one complete scan of @p mount_handle with the disk as it is now, and the
        changes to apply to the model (MountScanRun::Execute)."""
        mount = self.mounts[mount_handle]
        root = Path(mount.root)
        listing = listing if listing is not None else fuzz_disk.list_mount_directory(root)
        read = fingerprint or (lambda f: fuzz_disk.read_fingerprint(f.path))
        if not listing.complete:
            def offline() -> None:
                mount.state = "offline"
            return ScanPrediction("offline", apply=offline)

        def below_unreadable(rel: str) -> bool:
            for directory in listing.unreadable_directories:
                if directory == "" or rel == directory or rel.startswith(directory + "/"):
                    return True
            return False

        records = sorted(self.mounted_photos(mount_handle),
                         key=lambda p: (wstring_key(p.rel), p.eid or 0))
        record_at: dict[str, Photo] = {}
        lost: list[Photo] = []
        for record in records:
            if record.rel in record_at:
                lost.append(record)
            else:
                record_at[record.rel] = record

        changed: list[tuple[Photo, ListedFile, str]] = []
        returned: list[tuple[Photo, ListedFile, str]] = []
        new_paths: list[ListedFile] = []
        for file in listing.files:
            record = record_at.pop(file.rel, None)
            if record is None:
                new_paths.append(file)
                continue
            same = file.size == record.size and file.mtime_ns == record.mtime_ns
            if same:
                if record.state == "missing":
                    returned.append((record, file, record.fingerprint))
                continue
            key = read(file)
            if key is None:
                continue
            (returned if record.state == "missing" else changed).append((record, file, key))

        lost += list(record_at.values())
        lost.sort(key=lambda p: (wstring_key(p.rel), p.eid or 0))
        unverified: list[Photo] = []
        unmatched: list[Photo] = []
        groups: dict[str, tuple[list[Photo], list[tuple[ListedFile, str]]]] = {}
        unverified_keys: set[str] = set()
        lost_sizes = set()
        for record in lost:
            lost_sizes.add(record.size)
            if below_unreadable(record.rel):
                unverified.append(record)
                unverified_keys.add(record.fingerprint)
                continue
            groups.setdefault(record.fingerprint, ([], []))[0].append(record)

        ordinary_at: dict[str, Photo] = {}
        for photo in sorted(self.photos.values(), key=lambda p: p.eid or 0):
            if photo.mount is None:
                rel = fuzz_disk.relative_below(root, photo.path)
                if rel is not None and rel not in ordinary_at:
                    ordinary_at[rel] = photo

        adopted: list[tuple[Photo, ListedFile, str]] = []
        unread_new: list[ListedFile] = []
        for file in new_paths:
            ordinary = ordinary_at.get(file.rel)
            if ordinary is None and file.size not in lost_sizes:
                unread_new.append(file)
                continue
            key = read(file)
            if key is None:
                continue
            if ordinary is not None:
                adopted.append((ordinary, file, key))
                continue
            groups.setdefault(key, ([], []))[1].append((file, key))

        moved: list[tuple[Photo, ListedFile, str]] = []
        new_files: list[tuple[ListedFile, str]] = []
        for key, (group_records, group_paths) in groups.items():
            taken = [False] * len(group_paths)
            matched = [False] * len(group_records)
            for r, record in enumerate(group_records):
                name = record.rel.rsplit("/", 1)[-1]
                for p, (file, _) in enumerate(group_paths):
                    if not taken[p] and file.rel.rsplit("/", 1)[-1] == name:
                        moved.append((record, file, key))
                        taken[p] = matched[r] = True
                        break
            next_path = 0
            for r, record in enumerate(group_records):
                if matched[r]:
                    continue
                while next_path < len(taken) and taken[next_path]:
                    next_path += 1
                if next_path == len(taken):
                    break
                moved.append((record, group_paths[next_path][0], key))
                taken[next_path] = matched[r] = True
            unmatched += [record for r, record in enumerate(group_records) if not matched[r]]
            for p, (file, file_key) in enumerate(group_paths):
                if not taken[p] and key not in unverified_keys:
                    new_files.append((file, file_key))

        missing = [record for record in unmatched if record.state != "missing"]
        protected: set[str] = set()

        def add_parents(rel: str, into: set[str]) -> None:
            parts = rel.split("/")[:-1]
            for index in range(1, len(parts) + 1):
                into.add("/".join(parts[:index]))

        for record in unmatched + unverified:
            add_parents(record.rel, protected)
        listed_dirs: set[str] = set()
        for file in listing.files:
            add_parents(file.rel, listed_dirs)
        recorded = [f for f in self.folders.values() if f.mount == mount_handle and f.rel]
        for folder in recorded:
            if below_unreadable(folder.rel):
                protected.add(folder.rel)
                add_parents(folder.rel, protected)
        recorded_rels = {f.rel for f in recorded}
        folders_to_add = sorted((d for d in listed_dirs if d not in recorded_rels),
                                key=wstring_key)
        folders_to_remove = [f for f in recorded
                             if f.rel not in listed_dirs and f.rel not in protected]

        unread_readable = []
        for file in sorted(unread_new, key=lambda f: wstring_key(f.rel)):
            key = read(file)
            if key is not None:
                unread_readable.append((file, key))
        imports = sorted(new_files, key=lambda item: wstring_key(item[0].rel)) + unread_readable

        def apply() -> None:
            mount.state = "online"
            for rel in folders_to_add:
                parent_rel = rel.rsplit("/", 1)[0] if "/" in rel else ""
                parent = self.mount_folder(mount, parent_rel)
                handle = self.new_handle("f")
                self.folders[handle] = Folder(handle, parent.handle, rel.rsplit("/", 1)[-1],
                                              mount=mount_handle, rel=rel)
            for folder in folders_to_remove:
                self.folders.pop(folder.handle, None)

            def place(photo: Photo, file: ListedFile, key: str) -> None:
                photo.rel = file.rel
                photo.path = os.path.join(mount.root, *file.rel.split("/"))
                photo.size, photo.mtime_ns, photo.fingerprint = file.size, file.mtime_ns, key
                photo.state = "present"
                parent_rel = file.rel.rsplit("/", 1)[0] if "/" in file.rel else ""
                photo.home = self.mount_folder(mount, parent_rel).handle

            for photo, file, key in moved:
                place(photo, file, key)
            for photo, file, key in adopted:
                photo.mount = mount_handle
                place(photo, file, key)
            for photo, file, key in changed + returned:
                photo.size, photo.mtime_ns, photo.fingerprint = file.size, file.mtime_ns, key
                photo.state = "present"
            for photo in missing:
                photo.state = "missing"
            for file, key in imports:
                photo = Photo(self.new_handle("p"), path="")
                photo.mount = mount_handle
                self.photos[photo.handle] = photo
                place(photo, file, key)

        return ScanPrediction("complete", new=len(imports) + len(adopted), moved=len(moved),
                              changed=len(changed), missing=len(missing),
                              returned=len(returned), apply=apply)
