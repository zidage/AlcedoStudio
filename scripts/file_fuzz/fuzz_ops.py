#  Copyright 2026 Yurun Zi
#  SPDX-License-Identifier: GPL-3.0-only
#  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
"""The operations of the file management fuzz (#1192): the weight table, the generator, the
executor, and the checks after each step.

An operation is a JSON object of the operation log. It names folders, photos, and mounts by the
handles of the model and disk entries by their path below the disk directory of the run, so a
log replays in a new session and a shortened log still replays. The generator writes an
operation from the seed and the model; the executor runs it, compares each answer with the
outcomes that the model accepts, and updates the model.
"""

from __future__ import annotations

import json
import os
import random
import shutil
import subprocess
import sys
import threading
import time
import unicodedata
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable

import fuzz_disk
import fuzz_model
from fuzz_disk import DiskTools, Volumes, content_bytes, path_key
from fuzz_model import (HAS_PHOTO, MOUNTED_READ_ONLY, NESTED_MOUNT, NOT_LISTED, NOT_MISSING,
                        OUTSIDE_MOUNT, FILE_UNREADABLE, PHOTO_MISSING, PHOTO_PRESENT, ROOT,
                        UNKNOWN_FOLDER, UNKNOWN_MOUNT, UNREACHABLE, LibraryModel)
from fuzz_session import Answer, HostFailure, Session

IS_WINDOWS = sys.platform == "win32"

# The operation kinds: weight and area of the test list of #1188. A run draws each operation
# with these weights; --areas keeps the kinds of some areas, --weight KIND=W changes one weight.
WEIGHTS: dict[str, tuple[float, str]] = {
    # A. Folder names
    "folder_create": (8, "A"),
    "folder_rename": (5, "A"),
    # B. Folder structure
    "folder_move": (5, "B"),
    "folder_delete": (3, "B"),
    "folder_bad_id": (2, "B"),
    "deep_tree": (0.1, "B"),
    "wide_folder": (0.1, "B"),
    # C. Photo membership
    "import": (6, "C"),
    "import_bad_files": (1, "C"),
    "link": (4, "C"),
    "delete_album": (3, "C"),
    "delete_project": (2, "C"),
    "delete_unknown_ids": (1, "C"),
    # M1. Mount creation and refusals
    "mount": (3, "M1"),
    "mount_refused": (2, "M1"),
    "mounted_structure_edit": (2, "M1"),
    # M2. Disk changes, then a scan
    "disk_add": (6, "M2"),
    "disk_delete": (4, "M2"),
    "disk_rename": (4, "M2"),
    "disk_case_rename": (2, "M2"),
    "disk_move": (4, "M2"),
    "disk_rename_dir": (2, "M2"),
    "disk_move_dir": (2, "M2"),
    "disk_rewrite": (3, "M2"),
    "disk_file_to_dir": (1, "M2"),
    "disk_dir_to_file": (1, "M2"),
    "disk_unicode_name": (2, "M2"),
    "disk_unlisted": (1, "M2"),
    "disk_unreadable_dir": (1, "M2"),
    "disk_readable_dir": (2, "M2"),
    "disk_lock": (1, "M2"),
    "disk_unlock": (2, "M2"),
    "disk_many": (0.2, "M2"),
    "sync": (8, "M2"),
    # M3. Fingerprint pairs
    "disk_copy": (3, "M3"),
    "disk_swap_pair": (2, "M3"),
    "disk_move_and_change": (2, "M3"),
    "disk_middle_change": (1, "M3"),
    "disk_touch": (2, "M3"),
    "disk_hardlink": (1, "M3"),
    # M4. Missing photos
    "locate_photo": (3, "M4"),
    "remove_missing": (2, "M4"),
    "delete_mounted": (2, "M4"),
    # M5. Root and volume
    "locate_root": (2, "M5"),
    "root_away": (1, "M5"),
    "root_back": (3, "M5"),
    "volume_create": (1, "M5"),
    "volume_detach": (1, "M5"),
    "volume_attach": (2, "M5"),
    "volume_rename": (1, "M5"),
    # M6. Interleaving with library operations
    "scan_interleave": (2, "M6"),
    "scan_cancel": (1, "M6"),
    "scan_twice": (1, "M6"),
    # M7. Unmount
    "unmount": (1, "M7"),
    "remount": (1, "M7"),
    # D. Persistence and crashes
    "save_check": (1, "D"),
    "reopen": (1, "D"),
    "kill_reopen": (0.5, "D"),
    # F. Parameter specification
    "param_types": (2, "F"),
}

# Kinds that change only the disk. In watcher-on mode a group of them is followed by 'settle'.
DISK_KINDS = {k for k in WEIGHTS if k.startswith("disk_")} | {"root_away", "root_back"}

NAME_SAMPLES = [
    "", " ", "   ", "\t", " lead", "trail ", " both ", "dot.", "a/b", "a\\b", "a:b", ".", "..",
    "x\x01y", "x\x7fy", "x\x00y", " nbsp ", "　wide　", "@at", "@", "#hash",
    "café", "café", "Case", "case", "CASE", "\U0001F4F7", "风景", "相册",
    "con", "prn", "nul", "aux", "com1", "a" * 255, "é" * 127 + "a", "é" * 128,
    "b" * 4096, "‮abc", "a b", "a  b", "a.b.c", "~$tmp", "\U0001F600\U0001F600",
]

MOUNT_PREFIX = "mnt"


def _folder_names(op: dict) -> list[str]:
    names = [op.get("name", "")] if op["op"] in ("folder_create", "folder_rename",
                                                 "mounted_structure_edit") else []
    if op["op"] == "concurrent_round":
        names += [item.get("name", "") for item in op["a"]]
    return [name or "" for name in names]


# Open issues that a run found. The generator writes no operation that an entry covers, so a run
# finds the next defect instead of the known one; --include-known-issues writes them. Remove the
# entry with the fix of its issue: the fix adds the regression test, and the fuzz covers the case
# again.
def _disk_names(op: dict) -> list[str]:
    if not op["op"].startswith("disk_"):
        return []
    return [op[key].rsplit("/", 1)[-1] for key in ("path", "to", "a_to", "b_to")
            if isinstance(op.get(key), str)]


KNOWN_ISSUES: list[tuple[int, str, Callable[[dict], bool]]] = [
    (1214, "a folder name with ':' loses its parent path on Windows",
     lambda op: IS_WINDOWS and any(":" in name for name in _folder_names(op))),
    (1215, "a scan never imports a mounted file whose name is not in NFC on Windows",
     lambda op: IS_WINDOWS and any(not unicodedata.is_normalized("NFC", name)
                                   for name in _disk_names(op))),
    (1218, "on NTFS the directory entry of a hard link keeps an old size and time",
     lambda op: IS_WINDOWS and op["op"] == "disk_hardlink"),
]


class CheckFailure(Exception):
    """A step that the model does not accept."""

    def __init__(self, check: str, message: str, detail: Any = None) -> None:
        super().__init__(f"{check}: {message}")
        self.check = check
        self.message = message
        self.detail = detail


def answer_text(answer: Answer) -> str:
    return json.dumps(answer.to_json(), ensure_ascii=False)[:2000]


@dataclass
class RunOptions:
    mode: str = "watcher-off"
    check_every: int = 1
    save_every: int = 25
    reopen_every: int = 60
    inject_failure: str | None = None
    scale: float = 1.0
    avoid_known_issues: bool = True


@dataclass
class StepRecord:
    step: int
    op: dict
    outcomes: list[str] = field(default_factory=list)
    seconds: float = 0.0
    skipped: str | None = None


class Driver:
    """Runs operations on one session and one model, and checks each step."""

    def __init__(self, session: Session, out: Path, options: RunOptions,
                 invariant_check: Path | None) -> None:
        self.session = session
        self.out = out
        self.disk = out / "disk"
        self.disk.mkdir(parents=True, exist_ok=True)
        (self.disk / "src").mkdir(exist_ok=True)
        (self.disk / "outside").mkdir(exist_ok=True)
        self.options = options
        self.invariant_check = invariant_check
        self.model = LibraryModel()
        self.tools = DiskTools()
        self.volumes = Volumes(out / "volumes")
        self.project_path: Path | None = None
        self.previous_read: dict | None = None
        self.touched_mounts: set[str] = set()
        self.unsettled = False
        self.records: list[StepRecord] = []
        self.model_lock = threading.Lock()
        # The expected volume_name of each mount on a disk image, the name of each disk image,
        # and the mount point where it is attached now.
        self.volume_names: dict[str, str] = {}
        self.volume_labels: dict[str, str] = {}
        self.volume_points: dict[str, Path] = {}
        self.mount_volume: dict[str, tuple[str, str]] = {}

    # -------------------------------------------------------------------------------------------
    # Paths and calls
    # -------------------------------------------------------------------------------------------

    def abs(self, rel: str) -> Path:
        if rel.startswith("vol:"):
            handle, _, below = rel[4:].partition("/")
            point = self.volume_points[handle]
            return point / below if below else point
        return self.disk.joinpath(*rel.split("/"))

    def rel_of(self, path: Path) -> str:
        return path.relative_to(self.disk).as_posix()

    def call(self, method: str, params: dict | None = None, timeout_ms: int | None = None,
             expect: set[str] | None = None) -> Answer:
        answer = self.session.call(method, params, timeout_ms)
        self.current.outcomes.append(f"{method}={answer.outcome}")
        if answer.outcome.startswith("error:"):
            raise CheckFailure("error_code", f"{method} {params} answered {answer_text(answer)}")
        if answer.outcome == "invalid_params" and "pointer" not in answer.data:
            raise CheckFailure("error_code", f"{method}: -32602 without data.pointer: "
                                             f"{answer_text(answer)}")
        if expect is not None and answer.outcome not in expect:
            raise CheckFailure("outcome", f"{method} {json.dumps(params, ensure_ascii=False)[:400]}"
                                          f" answered {answer.outcome}, expected one of "
                                          f"{sorted(expect)}: {answer_text(answer)}")
        return answer

    def fid(self, folder: str | None) -> int:
        if folder is None or folder not in self.model.folders:
            return 0x7FFFFFF0
        fid = self.model.folders[folder].fid
        assert fid is not None, f"folder {folder} has no id"
        return fid

    def mid(self, mount: str) -> int:
        if mount not in self.model.mounts:
            return 4000000000
        return self.model.mounts[mount].mid

    def eids(self, photos: list[str]) -> list[int]:
        return [self.model.photos[p].eid for p in photos if p in self.model.photos]

    def wait_task(self, task_id: str) -> Answer:
        return self.call("tasks.wait", {"task_id": task_id}, timeout_ms=1800000)

    def wait_idle(self) -> None:
        self.call("library.mounts.wait_idle", {}, timeout_ms=1800000, expect={"ok"})

    # -------------------------------------------------------------------------------------------
    # Checks
    # -------------------------------------------------------------------------------------------

    def read_state(self) -> dict:
        answers = self.session.batch([
            ("folders", "library.folders", {}),
            ("mounts", "library.mounts", {"with_ids": True}),
            ("root", "library.list", {"limit": 1000}),
        ])
        for key, answer in answers.items():
            if not answer.ok:
                raise CheckFailure("read", f"{key}: {answer_text(answer)}")
        folders = answers["folders"].result["folders"]
        mounts = answers["mounts"].result["mounts"]
        root = answers["root"].result
        root_ids = [item["element_id"] for item in root["items"]]
        pages = [("root|%d" % offset, "library.list", {"offset": offset, "limit": 1000})
                 for offset in range(1000, root["total"], 1000)]
        lists = [(f"list|{f['folder_id']}", "library.list",
                  {"folder_id": f["folder_id"], "limit": 1000}) for f in folders
                 if f["folder_id"] != 0]
        listed: dict[int, list[int]] = {}
        totals: dict[int, int] = {}
        if pages or lists:
            more = self.session.batch(pages + lists)
            for key, answer in more.items():
                if not answer.ok:
                    raise CheckFailure("read", f"{key}: {answer_text(answer)}")
                if key.startswith("root|"):
                    root_ids += [item["element_id"] for item in answer.result["items"]]
                else:
                    fid = int(key.split("|")[1])
                    listed[fid] = [item["element_id"] for item in answer.result["items"]]
                    totals[fid] = answer.result["total"]
            extra = []
            for fid, total in totals.items():
                for offset in range(1000, total, 1000):
                    extra.append((f"list|{fid}|{offset}", "library.list",
                                  {"folder_id": fid, "offset": offset, "limit": 1000}))
            if extra:
                for key, answer in self.session.batch(extra).items():
                    fid = int(key.split("|")[1])
                    listed[fid] += [item["element_id"] for item in answer.result["items"]]
        return {"folders": folders, "mounts": mounts, "root_ids": sorted(root_ids),
                "root_total": root["total"], "lists": {k: sorted(v) for k, v in listed.items()},
                "totals": totals}

    def bind_folders(self, state: dict) -> None:
        by_path = {f["path"]: f for f in state["folders"]}
        model_paths = {self.model.path_of(h): h for h in self.model.folders}
        if set(by_path) != set(model_paths):
            raise CheckFailure("folders", "the folder tree differs from the model",
                               {"only_library": sorted(set(by_path) - set(model_paths))[:20],
                                "only_model": sorted(set(model_paths) - set(by_path))[:20]})
        for path, handle in model_paths.items():
            folder = self.model.folders[handle]
            read = by_path[path]
            if folder.fid is None:
                folder.fid = read["folder_id"]
            elif folder.fid != read["folder_id"]:
                raise CheckFailure("folders", f"{path} has id {read['folder_id']}, the model "
                                              f"has {folder.fid}")
            parent = self.model.folders[folder.parent].fid if folder.parent else 0
            if read["parent_id"] != parent:
                raise CheckFailure("folders", f"{path} has parent {read['parent_id']}, the model "
                                              f"has {parent}")

    def bind_photos(self, state: dict) -> None:
        bound = {p.eid for p in self.model.photos.values() if p.eid is not None}
        unknown = [eid for eid in state["root_ids"] if eid not in bound]
        unbound = [p for p in self.model.photos.values() if p.eid is None]
        gone = [p for p in self.model.photos.values()
                if p.eid is not None and p.eid not in set(state["root_ids"])]
        for photo in gone:
            if getattr(photo, "uncertain", False):
                del self.model.photos[photo.handle]
            else:
                raise CheckFailure("photos", f"photo {photo.handle} ({photo.eid}, {photo.path}) "
                                             "is not in the library")
        if not unknown and not unbound:
            return
        paths: dict[int, str] = {}
        for start in range(0, len(unknown), 500):
            answer = self.session.call("library.info", {"element_ids": unknown[start:start + 500]})
            if not answer.ok:
                raise CheckFailure("read", f"library.info: {answer_text(answer)}")
            for item in answer.result["items"]:
                paths[item["element_id"]] = item.get("file_path", "")
        fid_to_handle = {f.fid: f.handle for f in self.model.folders.values()}
        member: dict[int, set[str]] = {eid: set() for eid in unknown}
        for fid, ids in state["lists"].items():
            for eid in ids:
                if eid in member and fid in fid_to_handle:
                    member[eid].add(fid_to_handle[fid])

        def photo_key(photo) -> tuple:
            folders = set(photo.links)
            if photo.home:
                folders.add(photo.home)
            return path_key(photo.path), frozenset(folders)

        waiting: dict[tuple, list] = {}
        for photo in unbound:  # in the order of creation
            waiting.setdefault(photo_key(photo), []).append(photo)
        stray = []
        for eid in sorted(unknown):
            key = (path_key(paths.get(eid, "")), frozenset(member[eid]))
            if waiting.get(key):
                waiting[key].pop(0).eid = eid
            else:
                stray.append((eid, key))
        if stray:
            raise CheckFailure("photos", "the library has photos that the model does not have",
                               {"photos": [(eid, key[0], sorted(key[1])) for eid, key in stray][:20],
                                "model_unbound": [(p.handle, p.path, sorted(photo_key(p)[1]))
                                                  for v in waiting.values() for p in v][:20]})
        for group in waiting.values():
            for photo in group:
                if getattr(photo, "uncertain", False):
                    del self.model.photos[photo.handle]
                else:
                    raise CheckFailure("photos", f"photo {photo.handle} ({photo.path}) is not in "
                                                 "the library")

    def compare_photos(self, state: dict) -> None:
        model_ids = sorted(p.eid for p in self.model.photos.values())
        if model_ids != state["root_ids"] or state["root_total"] != len(model_ids):
            raise CheckFailure("photos", "All Photos differs from the model",
                               {"library": state["root_ids"][:50], "model": model_ids[:50]})
        for folder in self.model.folders.values():
            if folder.handle == ROOT:
                continue
            expected = sorted(self.model.photos[h].eid for h in self.model.photos_in(folder.handle))
            got = state["lists"].get(folder.fid, [])
            if got != expected or state["totals"].get(folder.fid, 0) != len(expected):
                raise CheckFailure("photos", f"folder {self.model.path_of(folder.handle)} differs "
                                             "from the model",
                                   {"library": got[:50], "model": expected[:50],
                                    "total": state["totals"].get(folder.fid)})

    def compare_mounts(self, state: dict) -> None:
        read = {m["mount_id"]: m for m in state["mounts"]}
        model = {m.mid: m for m in self.model.mounts.values()}
        if set(read) != set(model):
            raise CheckFailure("mounts", f"mount ids {sorted(read)}, the model has "
                                         f"{sorted(model)}")
        for mid, mount in model.items():
            got = read[mid]
            present = sorted(p.eid for p in self.model.mounted_photos(mount.handle)
                             if p.state == "present")
            missing = sorted(p.eid for p in self.model.mounted_photos(mount.handle)
                             if p.state == "missing")
            problems = []
            if path_key(got["path"]) != path_key(mount.root):
                problems.append(f"path {got['path']} != {mount.root}")
            if got["folder_id"] != self.model.folders[mount.folder].fid:
                problems.append(f"folder_id {got['folder_id']}")
            if got["state"] != mount.state:
                problems.append(f"state {got['state']} != {mount.state}")
            if sorted(got["present_element_ids"]) != present:
                problems.append(f"present {sorted(got['present_element_ids'])[:20]} != "
                                f"{present[:20]}")
            if sorted(got["missing_element_ids"]) != missing:
                problems.append(f"missing {sorted(got['missing_element_ids'])[:20]} != "
                                f"{missing[:20]}")
            if mount.handle in self.volume_names and \
                    got.get("volume_name") != self.volume_names[mount.handle]:
                problems.append(f"volume_name {got.get('volume_name')!r} != "
                                f"{self.volume_names[mount.handle]!r}")
            if problems:
                raise CheckFailure("mounts", f"mount {mount.handle} ({mid}): " +
                                   "; ".join(problems))

    def check_state(self, refused: bool = False) -> None:
        """Check 1 (model equality) and check 2 (a refused operation changes nothing)."""
        if self.unsettled:
            return
        state = self.read_state()
        self.bind_folders(state)
        self.bind_photos(state)
        self.compare_photos(state)
        self.compare_mounts(state)
        comparable = {k: state[k] for k in ("folders", "root_ids", "lists")}
        comparable["mounts"] = [{k: v for k, v in m.items() if k not in ("last_scan_at", "scan")}
                                for m in state["mounts"]]
        if refused and self.previous_read is not None and comparable != self.previous_read:
            raise CheckFailure("refusal_changed_state", "a refused operation changed the reads")
        self.previous_read = comparable

    def invariant_check_now(self) -> None:
        """Check 4: project.save, then StorageInvariantCheck on the database of the saved
        project file."""
        answer = self.call("project.save", {}, expect={"ok"})
        package = Path(answer.result["path"])
        database = self.out / "invariant_check.db"
        with open(package, "rb") as stream:
            header = stream.read(36)
            if header[:8] != b"PUERHPK1":
                raise CheckFailure("invariants", f"{package} is not a packed project")
            meta_size = int.from_bytes(header[12:20], "little")
            db_size = int.from_bytes(header[20:28], "little")
            stream.seek(36 + meta_size)
            with open(database, "wb") as out:
                remaining = db_size
                while remaining:
                    chunk = stream.read(min(remaining, 1 << 20))
                    if not chunk:
                        raise CheckFailure("invariants", f"{package} is shorter than its header")
                    out.write(chunk)
                    remaining -= len(chunk)
        if self.invariant_check is None:
            return
        result = subprocess.run([str(self.invariant_check), str(database)], capture_output=True,
                                text=True, encoding="utf-8", errors="replace", timeout=600)
        if result.returncode != 0:
            raise CheckFailure("invariants", f"StorageInvariantCheck exit {result.returncode}",
                               {"stdout": result.stdout[-4000:], "stderr": result.stderr[-2000:]})

    # -------------------------------------------------------------------------------------------
    # Scans
    # -------------------------------------------------------------------------------------------

    def fingerprint(self, file) -> str | None:
        if self.tools.is_locked(file.path):
            return None
        return fuzz_disk.read_fingerprint(file.path)

    def detached(self, mount: str) -> bool:
        volume = self.mount_volume.get(mount)
        return volume is not None and volume[0] not in self.volumes.attached

    def predict(self, mount: str) -> fuzz_model.ScanPrediction:
        if self.detached(mount):
            target = self.model.mounts[mount]

            def offline() -> None:
                target.state = "offline"
            return fuzz_model.ScanPrediction("offline", apply=offline)
        return self.model.predict_scan(mount, self.fingerprint)

    def compare_scan(self, scan: dict, prediction: fuzz_model.ScanPrediction, what: str) -> None:
        got = {k: scan.get(k) for k in prediction.counts()}
        if got != prediction.counts():
            raise CheckFailure("scan_counts", f"{what}: {got} != model {prediction.counts()}",
                               {"scan": scan})

    def scan_all_after_open(self) -> None:
        """The scans of a project open: each mount is scanned once."""
        self.wait_idle()
        for handle in list(self.model.mounts):
            self.locate_moved_volume(handle)
            self.predict(handle).apply()

    def locate_moved_volume(self, handle: str) -> None:
        """An open finds the volume of an offline mount at another mount point and uses the same
        directory there (relocated_root)."""
        volume = self.mount_volume.get(handle)
        if volume is None or volume[0] not in self.volumes.attached:
            return
        mount = self.model.mounts[handle]
        new_root = self.volume_points[volume[0]].joinpath(*[p for p in volume[1].split("/") if p])
        if path_key(new_root) != path_key(mount.root) and not Path(mount.root).is_dir():
            mount.root = str(new_root)

    # -------------------------------------------------------------------------------------------
    # Running an operation
    # -------------------------------------------------------------------------------------------

    def run(self, step: int, op: dict) -> StepRecord:
        self.current = StepRecord(step, op)
        started = time.monotonic()
        kind = op["op"]
        self.model.begin_operation(op.get("id", step))
        for entry in op.get("create", []):
            target = self.abs(entry["path"])
            if not target.exists():
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(content_bytes(entry["content"]))
        if self.options.inject_failure and kind == self.options.inject_failure:
            raise CheckFailure("injected", f"the test-only failure of the kind {kind}")
        handler: Callable[[dict], Any] = getattr(self, f"do_{kind}")
        refused = handler(op)
        if kind in DISK_KINDS and self.options.mode == "watcher-on" and not self.current.skipped:
            # The watcher scans the changed mounts at some time after the change; the reads are
            # compared after the next 'settle'.
            self.unsettled = True
        if not self.unsettled and (step % self.options.check_every == 0 or refused or
                                   kind in ("sync", "mount", "reopen", "kill_reopen", "settle")):
            self.check_state(refused=bool(refused))
        self.current.seconds = time.monotonic() - started
        self.records.append(self.current)
        return self.current

    def skip(self, why: str) -> None:
        self.current.skipped = why

    # A ---------------------------------------------------------------------------------------

    def do_folder_create(self, op: dict) -> bool:
        parent = op["parent"]
        expect = self.model.expect_create(parent if parent in self.model.folders else None,
                                          op["name"])
        answer = self.call("library.folder.create", {"parent_id": self.fid(parent),
                                                     "name": op["name"]}, expect=expect)
        if answer.ok:
            self.model.apply_create(op["handle"], parent, op["name"], answer.result["folder_id"])
            if answer.result["name"] != fuzz_model.qt_trimmed(op["name"]):
                raise CheckFailure("outcome", f"create stored {answer.result['name']!r}")
        return not answer.ok

    def do_folder_rename(self, op: dict) -> bool:
        folder = op["folder"]
        expect = self.model.expect_rename(folder if folder in self.model.folders else None,
                                          op["name"])
        answer = self.call("library.folder.rename", {"folder_id": self.fid(folder),
                                                     "name": op["name"]}, expect=expect)
        if answer.ok:
            self.model.apply_rename(folder, op["name"])
        return not answer.ok

    # B ---------------------------------------------------------------------------------------

    def do_folder_move(self, op: dict) -> bool:
        folder, parent = op["folder"], op["parent"]
        expect = self.model.expect_move(folder if folder in self.model.folders else None,
                                        parent if parent in self.model.folders else None)
        answer = self.call("library.folder.move", {"folder_id": self.fid(folder),
                                                   "parent_id": self.fid(parent)}, expect=expect)
        if answer.ok:
            self.model.apply_move(folder, parent)
        return not answer.ok

    def do_folder_delete(self, op: dict) -> bool:
        folder = op["folder"]
        expect = self.model.expect_delete_folder(folder if folder in self.model.folders else None)
        answer = self.call("library.folder.delete", {"folder_id": self.fid(folder)},
                           expect=expect)
        if answer.ok:
            self.model.apply_delete_folder(folder)
        return not answer.ok

    def do_folder_bad_id(self, op: dict) -> bool:
        value = op["id"]
        if isinstance(value, str) and value.startswith("p"):
            value = self.model.eid_of(value)
            if value is None:
                self.skip("the photo is gone")
                return True
        method = op["method"]
        params = {"library.folder.rename": {"folder_id": value, "name": "bad id"},
                  "library.folder.move": {"folder_id": value, "parent_id": 0},
                  "library.folder.delete": {"folder_id": value},
                  "library.folder.create": {"parent_id": value, "name": "bad id"},
                  "library.list": {"folder_id": value},
                  "library.import": {"paths": [str(self.abs(op["file"]))], "folder_id": value}
                  if op.get("file") else {"folder": str(self.disk / "src"), "folder_id": value},
                  }[method]
        if value == 0 and method in ("library.import", "library.list", "library.folder.create"):
            # The root folder is a valid id of these commands; the generator does not write it.
            self.skip("the root folder is a valid id")
            return True
        if value in {f.fid for f in self.model.folders.values()}:
            self.skip("the id names a folder now")
            return True
        if isinstance(value, int) and value < 0:
            expect = {"invalid_params"}
        elif value == 0:
            expect = {"rejected:root_folder_cannot_change"}
        else:
            # An id above the 32-bit range of the schema answers -32602; another id that no
            # folder has answers unknown_folder (library.list lists no photo of it).
            expect = {UNKNOWN_FOLDER, "invalid_params"}
            if method == "library.list":
                expect.add("ok")
        answer = self.call(method, params, expect=expect)
        if method == "library.list" and answer.ok and answer.result["total"] != 0:
            raise CheckFailure("outcome", f"library.list of the bad id {value} lists photos")
        if answer.ok and method == "library.folder.create":
            raise CheckFailure("outcome", "a folder was created below a bad id")
        return not answer.ok or method == "library.list"

    def do_deep_tree(self, op: dict) -> bool:
        """Test 11: a chain of 'count' folders, each in the one before."""
        parent = op["parent"]
        if parent not in self.model.folders or self.model.is_mounted(parent):
            self.skip("the parent is gone or mounted")
            return True
        for index in range(op["count"]):
            handle, name = f"{op['handle']}.{index}", f"{op['prefix']}{index}"
            answer = self.call("library.folder.create", {"parent_id": self.fid(parent),
                                                         "name": name},
                               expect=self.model.expect_create(parent, name))
            if not answer.ok:
                return True
            self.model.apply_create(handle, parent, name, answer.result["folder_id"])
            parent = handle
        return False

    def do_wide_folder(self, op: dict) -> bool:
        """Test 11 (#1180): 'count' child folders of one folder, created over one connection."""
        parent = op["parent"]
        if parent not in self.model.folders or self.model.is_mounted(parent):
            self.skip("the parent is gone or mounted")
            return True
        names = [f"{op['prefix']}{index}" for index in range(op["count"])]
        expects = {}
        for index, name in enumerate(names):
            expects[str(index)] = self.model.expect_create(parent, name)
        answers = self.session.batch([(str(index), "library.folder.create",
                                       {"parent_id": self.fid(parent), "name": name})
                                      for index, name in enumerate(names)], timeout_ms=600000)
        for index, name in enumerate(names):
            answer = answers[str(index)]
            self.current.outcomes.append(f"library.folder.create={answer.outcome}")
            if answer.outcome not in expects[str(index)]:
                raise CheckFailure("outcome", f"create {name!r} in a wide folder answered "
                                              f"{answer_text(answer)}")
            if answer.ok:
                self.model.apply_create(f"{op['handle']}.{index}", parent, name,
                                        answer.result["folder_id"])
        return False

    # C ---------------------------------------------------------------------------------------

    def import_expect(self, paths: list[Path], folder: str) -> tuple[set[str], list[Path]]:
        if folder not in self.model.folders:
            return {UNKNOWN_FOLDER}, []
        if self.model.is_mounted(folder):
            return {MOUNTED_READ_ONLY}, []
        kept, seen = [], set()
        for path in paths:
            if self.model.mount_of_path(path) is not None:
                continue
            if path_key(path) in seen:
                continue
            seen.add(path_key(path))
            kept.append(path)
        if not kept:
            # #1217: this refusal has no data.error yet.
            return {"rejected:"}, []
        return {"ok"}, kept

    def do_import(self, op: dict, uncertain: bool = False) -> bool:
        folder = op["folder"]
        paths = [self.abs(p) for p in op["paths"]]
        if any(not p.is_file() for p in paths):
            expect = {"invalid_params"}
            kept = []
        else:
            expect, kept = self.import_expect(paths, folder)
        supported = [p for p in kept if fuzz_disk.has_image_content(p)]
        if expect == {"ok"} and not supported:
            # Only files of an unsupported type: no photo is imported.
            expect = {"ok", "rejected:"}
        params = {"paths": [str(p) for p in paths]}
        if folder != ROOT or op.get("explicit_root"):
            params["folder_id"] = self.fid(folder)
        answer = self.call("library.import", params, expect=expect)
        if not answer.ok:
            return True
        final = self.wait_task(answer.result["task_id"])
        state = final.result.get("state") if final.ok else None
        if state != "succeeded" and not uncertain and supported:
            raise CheckFailure("outcome", f"the import ended with {answer_text(final)}")
        for path in supported:
            photo = self.model.add_photo(str(path), folder)
            if uncertain:
                photo.uncertain = True
        return not supported

    def do_import_bad_files(self, op: dict) -> bool:
        return self.do_import(op, uncertain=True)

    def do_link(self, op: dict) -> bool:
        photos = [p for p in op["photos"] if p in self.model.photos]
        folder = op["folder"]
        if not photos:
            self.skip("the photos are gone")
            return True
        if folder not in self.model.folders:
            expect = {UNKNOWN_FOLDER}
        elif folder == ROOT:
            expect = {"rejected:root_folder_cannot_change"}
        elif self.model.is_mounted(folder):
            expect = {MOUNTED_READ_ONLY}
        else:
            expect = {"ok"}
        answer = self.call("library.link", {"element_ids": self.eids(photos),
                                            "folder_id": self.fid(folder)}, expect=expect)
        if answer.ok:
            linked = set(answer.result["linked_ids"])
            already = set(answer.result["already_linked_ids"])
            want_already = {self.model.photos[p].eid for p in photos
                            if folder in self.model.photos[p].links}
            if already != want_already or answer.result["failed_ids"] or \
                    linked != set(self.eids(photos)) - want_already:
                raise CheckFailure("outcome", f"library.link answered {answer_text(answer)}, "
                                              f"the model has {sorted(want_already)} linked")
            for photo in photos:
                self.model.photos[photo].links.add(folder)
        return not answer.ok

    def do_delete_album(self, op: dict) -> bool:
        photos = [p for p in op["photos"] if p in self.model.photos]
        folder = op["folder"]
        if not photos or folder not in self.model.folders:
            self.skip("the photos or the folder are gone")
            return True
        inside = [p for p in photos if folder in self.model.photos[p].links or
                  self.model.photos[p].home == folder]
        if not inside:
            self.skip("no photo of the operation is in the folder")
            return True
        expect = {MOUNTED_READ_ONLY} if self.model.is_mounted(folder) else {"ok"}
        answer = self.call("library.delete", {"element_ids": self.eids(photos), "scope": "album",
                                              "folder_id": self.fid(folder)}, expect=expect)
        if answer.ok:
            deleted = set(answer.result["deleted_ids"])
            together = deleted | set(answer.result["failed_ids"]) | set(answer.result["skipped_ids"])
            want = {self.model.photos[p].eid for p in inside}
            if deleted != want or not together <= set(self.eids(photos)):
                raise CheckFailure("outcome", f"library.delete (album) answered "
                                              f"{answer_text(answer)}, the model deletes "
                                              f"{sorted(want)}")
            for photo in inside:
                self.model.photos[photo].links.discard(folder)
        return not answer.ok

    def do_delete_project(self, op: dict) -> bool:
        photos = [p for p in op["photos"] if p in self.model.photos]
        if not photos:
            self.skip("the photos are gone")
            return True
        ordinary = [p for p in photos if self.model.photos[p].mount is None]
        mounted = [p for p in photos if self.model.photos[p].mount is not None]
        if ordinary:
            expect = {"ok"}
        else:
            expect = set()
            for photo in mounted:
                expect.add(PHOTO_MISSING if self.model.photos[photo].state == "missing"
                           else PHOTO_PRESENT)
        answer = self.call("library.delete", {"element_ids": self.eids(photos),
                                              "scope": "project"}, expect=expect)
        if answer.ok:
            want_deleted = {self.model.photos[p].eid for p in ordinary}
            want_skipped = {self.model.photos[p].eid for p in mounted}
            if set(answer.result["deleted_ids"]) != want_deleted or \
                    set(answer.result["skipped_ids"]) != want_skipped or answer.result["failed_ids"]:
                raise CheckFailure("outcome", f"library.delete (project) answered "
                                              f"{answer_text(answer)}, the model deletes "
                                              f"{sorted(want_deleted)}, skips "
                                              f"{sorted(want_skipped)}")
            for photo in ordinary:
                self.model.remove_photo(photo)
        return not answer.ok

    def do_delete_unknown_ids(self, op: dict) -> bool:
        photos = [p for p in op["photos"] if p in self.model.photos]
        ids = self.eids(photos) + op["unknown_ids"]
        self.call("library.delete", {"element_ids": ids, "scope": "project"},
                  expect={"invalid_params"})
        return True

    # M1 --------------------------------------------------------------------------------------

    def write_tree(self, base: Path, files: list[dict]) -> None:
        for entry in files:
            path = base.joinpath(*entry["path"].split("/"))
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content_bytes(entry["content"]))

    def do_mount(self, op: dict) -> bool:
        path = self.abs(op["path"])
        if op.get("files") is not None and not path.exists():
            path.mkdir(parents=True)
            self.write_tree(path, op["files"])
            for directory in op.get("empty_dirs", []):
                path.joinpath(*directory.split("/")).mkdir(parents=True, exist_ok=True)
        expect = self.model.expect_mount(path)
        answer = self.call("library.mount", {"path": str(path)}, timeout_ms=1800000,
                           expect=expect)
        if not answer.ok:
            return True
        mount = self.model.add_mount(path, answer.result["mount_id"], answer.result["folder_id"],
                                     op.get("handle"))
        if op.get("volume"):
            self.mount_volume[mount.handle] = (op["volume"], op["path"].partition("/")[2])
            self.volume_names[mount.handle] = self.volume_labels.get(op["volume"], "")
        # The disk does not change during the step, so the prediction after the answer is the
        # prediction of the first scan (a mount with no records: each listed file is new or
        # adopted).
        prediction = self.model.predict_scan(mount.handle, self.fingerprint)
        self.compare_scan(answer.result["scan"], prediction, f"first scan of {op['path']}")
        prediction.apply()
        return False

    def do_mount_refused(self, op: dict) -> bool:
        path = self.abs(op["path"])
        if op.get("make") == "file" and not path.exists():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"not a directory")
        if op.get("make") == "symlink" and not path.exists():
            target = self.abs(op["target"])
            if not DiskTools.symlink(target, path, directory=True):
                self.skip("no symbolic link or junction can be created")
                return True
        if op.get("make") == "case":
            pass
        expect = self.model.expect_mount(path)
        if op.get("make") == "symlink" and expect == {"ok"}:
            # A link to a mounted directory names the same directory.
            target = self.abs(op["target"])
            if self.model.mount_of_path(target) is not None:
                expect = {NESTED_MOUNT}
        answer = self.call("library.mount", {"path": str(path)}, timeout_ms=1800000,
                           expect=expect)
        if answer.ok:
            mount = self.model.add_mount(path, answer.result["mount_id"],
                                         answer.result["folder_id"])
            real = self.model.predict_scan(mount.handle, self.fingerprint)
            self.compare_scan(answer.result["scan"], real, "first scan")
            real.apply()
        return not answer.ok

    def do_mounted_structure_edit(self, op: dict) -> bool:
        folder = op["folder"]
        if folder not in self.model.folders or not self.model.is_mounted(folder):
            self.skip("the folder is gone or not mounted")
            return True
        action = op["action"]
        if action == "create":
            return self.do_folder_create({"parent": folder, "name": op["name"],
                                          "handle": op["handle"]})
        if action == "rename":
            return self.do_folder_rename({"folder": folder, "name": op["name"]})
        if action == "move":
            return self.do_folder_move({"folder": folder, "parent": op["parent"]})
        if action == "move_into":
            return self.do_folder_move({"folder": op["parent"], "parent": folder})
        if action == "delete":
            return self.do_folder_delete({"folder": folder})
        if action == "import":
            return self.do_import({"paths": op["paths"], "folder": folder})
        if action == "link":
            return self.do_link({"photos": op["photos"], "folder": folder})
        raise ValueError(action)

    # M2 and M3: disk changes -------------------------------------------------------------

    def touched(self, *paths: Path) -> None:
        for path in paths:
            mount = self.model.mount_of_path(path)
            if mount is not None:
                self.touched_mounts.add(mount.handle)

    def do_disk_add(self, op: dict) -> bool:
        path = self.abs(op["path"])
        if path.exists() or not path.parent.exists() and not op.get("parents"):
            self.skip("the path exists or its directory is gone")
            return False
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content_bytes(op["content"]))
        self.touched(path)
        return False

    do_disk_unicode_name = do_disk_add
    do_disk_unlisted = do_disk_add

    def do_disk_many(self, op: dict) -> bool:
        directory = self.abs(op["dir"])
        if directory.exists():
            self.skip("the directory exists")
            return False
        directory.mkdir(parents=True)
        for index in range(op["count"]):
            (directory / f"n{index:05d}.jpg").write_bytes(
                content_bytes({"kind": "jpeg", "seed": op["seed"] * 100000 + index,
                               "size": 900 + index % 7}))
        self.touched(directory)
        return False

    def remove(self, path: Path) -> bool:
        """Removes @p path after the locks and the denied read permissions below it end.
        Returns False when the path stays."""
        for locked in [Path(p) for p in self.tools.locks if fuzz_disk.is_at_or_below(p, path)]:
            self.tools.unlock(locked)
        for denied in sorted((Path(p) for p in self.tools.unreadable
                              if fuzz_disk.is_at_or_below(p, path)), key=lambda p: len(p.parts)):
            self.tools.make_readable(denied)
        fuzz_disk.remove_tree(path)
        return not path.exists() and not path.is_symlink()

    def follow_rename(self, source: Path, target: Path) -> None:
        """The locks and denied permissions below @p source are below @p target now."""
        for denied in [p for p in self.tools.unreadable if fuzz_disk.is_at_or_below(p, source)]:
            self.tools.unreadable.discard(denied)
            self.tools.unreadable.add(str(target / Path(denied).relative_to(source)))
        for locked in [p for p in self.tools.locks if fuzz_disk.is_at_or_below(p, source)]:
            self.tools.locks[str(target / Path(locked).relative_to(source))] = \
                self.tools.locks.pop(locked)

    def do_disk_delete(self, op: dict) -> bool:
        path = self.abs(op["path"])
        if not path.exists() and not path.is_symlink():
            self.skip("the path is gone")
            return False
        if not self.remove(path):
            self.skip("the path cannot be removed")
        self.touched(path)
        return False

    def do_disk_rename(self, op: dict) -> bool:
        source, target = self.abs(op["path"]), self.abs(op["to"])
        if not source.exists() or (target.exists() and path_key(source) != path_key(target)) or \
                not target.parent.is_dir() or self.tools.is_locked(source):
            self.skip("the source is gone, locked, or the target exists")
            return False
        try:
            os.rename(source, target)
        except OSError as error:
            self.skip(f"rename failed: {error}")
            return False
        self.follow_rename(source, target)
        self.touched(source, target)
        return False

    do_disk_case_rename = do_disk_rename
    do_disk_move = do_disk_rename
    do_disk_rename_dir = do_disk_rename
    do_disk_move_dir = do_disk_rename

    def do_disk_rewrite(self, op: dict) -> bool:
        path = self.abs(op["path"])
        if not path.is_file() or self.tools.is_locked(path):
            self.skip("the file is gone or locked")
            return False
        path.write_bytes(content_bytes(op["content"]))
        self.touched(path)
        return False

    do_disk_middle_change = do_disk_rewrite

    def do_disk_touch(self, op: dict) -> bool:
        path = self.abs(op["path"])
        if not path.is_file():
            self.skip("the file is gone")
            return False
        info = path.stat()
        os.utime(path, ns=(info.st_atime_ns, info.st_mtime_ns + op["delta_s"] * 1_000_000_000))
        self.touched(path)
        return False

    def do_disk_file_to_dir(self, op: dict) -> bool:
        path = self.abs(op["path"])
        if not path.is_file() or self.tools.is_locked(path):
            self.skip("the file is gone or locked")
            return False
        path.unlink()
        path.mkdir()
        if op.get("content"):
            (path / "inner.jpg").write_bytes(content_bytes(op["content"]))
        self.touched(path)
        return False

    def do_disk_dir_to_file(self, op: dict) -> bool:
        path = self.abs(op["path"])
        if not path.is_dir() or any(self.tools.is_locked(Path(p)) for p in self.tools.locks
                                    if fuzz_disk.is_at_or_below(p, path)):
            self.skip("the directory is gone or holds a locked file")
            return False
        if not self.remove(path):
            self.skip("the directory cannot be removed")
            self.touched(path)
            return False
        path.write_bytes(content_bytes(op["content"]))
        self.touched(path)
        return False

    def do_disk_copy(self, op: dict) -> bool:
        source, target = self.abs(op["path"]), self.abs(op["to"])
        if not source.is_file() or target.exists() or not target.parent.is_dir() or \
                self.tools.is_locked(source):
            self.skip("the source is gone or the target exists")
            return False
        shutil.copyfile(source, target)
        self.touched(target)
        return False

    def do_disk_swap_pair(self, op: dict) -> bool:
        """Two files with the same content move in the same step."""
        a, b = self.abs(op["a"]), self.abs(op["b"])
        a_to, b_to = self.abs(op["a_to"]), self.abs(op["b_to"])
        if not a.is_file() or b.exists() or a_to.exists() or b_to.exists() or \
                not a_to.parent.is_dir() or not b_to.parent.is_dir() or self.tools.is_locked(a):
            self.skip("the paths changed")
            return False
        shutil.copyfile(a, b)
        os.rename(a, a_to)
        os.rename(b, b_to)
        self.touched(a, a_to, b_to)
        return False

    def do_disk_move_and_change(self, op: dict) -> bool:
        source, target = self.abs(op["path"]), self.abs(op["to"])
        if not source.is_file() or target.exists() or not target.parent.is_dir() or \
                self.tools.is_locked(source):
            self.skip("the paths changed")
            return False
        os.rename(source, target)
        target.write_bytes(content_bytes(op["content"]))
        self.touched(source, target)
        return False

    def do_disk_hardlink(self, op: dict) -> bool:
        source, target = self.abs(op["path"]), self.abs(op["to"])
        if not source.is_file() or target.exists() or not target.parent.is_dir():
            self.skip("the paths changed")
            return False
        if not DiskTools.hardlink(source, target):
            self.skip("no hard link can be created")
            return False
        self.touched(target)
        return False

    def do_disk_unreadable_dir(self, op: dict) -> bool:
        path = self.abs(op["path"])
        if not path.is_dir() or path.is_symlink() or not self.tools.make_unreadable(path):
            self.skip("the directory is gone or its permission cannot change")
            return False
        self.touched(path)
        return False

    def do_disk_readable_dir(self, op: dict) -> bool:
        path = self.abs(op["path"])
        if not self.tools.make_readable(path):
            self.skip("the directory is readable")
            return False
        self.touched(path)
        return False

    def do_disk_lock(self, op: dict) -> bool:
        path = self.abs(op["path"])
        if not path.is_file() or not self.tools.lock(path):
            self.skip("the file is gone or cannot be locked")
        return False

    def do_disk_unlock(self, op: dict) -> bool:
        if not self.tools.unlock(self.abs(op["path"])):
            self.skip("the file is not locked")
        return False

    def do_root_away(self, op: dict) -> bool:
        mount = self.model.mounts.get(op["mount"])
        if mount is None or not Path(mount.root).is_dir() or self.mount_volume.get(op["mount"]):
            self.skip("the mount or its directory is gone")
            return False
        away = Path(mount.root + "_away")
        if away.exists() or any(fuzz_disk.is_at_or_below(p, mount.root) for p in self.tools.locks):
            self.skip("the directory cannot move")
            return False
        os.rename(mount.root, away)
        self.follow_rename(Path(mount.root), away)
        self.touched_mounts.add(mount.handle)
        return False

    def do_root_back(self, op: dict) -> bool:
        mount = self.model.mounts.get(op["mount"])
        if mount is None:
            self.skip("the mount is gone")
            return False
        away = Path(mount.root + "_away")
        if not away.is_dir() or Path(mount.root).exists():
            self.skip("the directory is not away")
            return False
        os.rename(away, mount.root)
        self.follow_rename(away, Path(mount.root))
        self.touched_mounts.add(mount.handle)
        return False

    def do_settle(self, op: dict) -> bool:
        """Watcher-on mode: waits for the scans that the directory watcher starts, then applies
        one scan of each changed mount to the model."""
        time.sleep(op.get("delay_s", 1.0))
        self.wait_idle()
        for handle in sorted(self.touched_mounts):
            if handle in self.model.mounts:
                self.predict(handle).apply()
        self.touched_mounts.clear()
        self.unsettled = False
        return False

    def do_sync(self, op: dict) -> bool:
        mount = op.get("mount")
        if self.options.mode == "watcher-on" and self.unsettled:
            self.do_settle({"delay_s": 1.0})
        if mount is not None and mount not in self.model.mounts:
            self.call("library.sync", {"mount_id": self.mid(mount)}, expect={UNKNOWN_MOUNT})
            return True
        handles = [mount] if mount else sorted(self.model.mounts,
                                               key=lambda h: self.model.mounts[h].mid)
        predictions = {h: self.predict(h) for h in handles}
        params = {"mount_id": self.mid(mount)} if mount else {}
        answer = self.call("library.sync", params, timeout_ms=1800000, expect={"ok"})
        scans = {scan["mount_id"]: scan for scan in answer.result["scans"]}
        if set(scans) != {self.model.mounts[h].mid for h in handles}:
            raise CheckFailure("scan_counts", f"library.sync scanned {sorted(scans)}")
        for handle in handles:
            prediction = predictions[handle]
            self.compare_scan(scans[self.model.mounts[handle].mid], prediction,
                              f"sync of {handle}")
            prediction.apply()
        self.touched_mounts -= set(handles)
        return False

    # M4 --------------------------------------------------------------------------------------

    def do_locate_photo(self, op: dict) -> bool:
        photo = self.model.photos.get(op["photo"])
        if photo is None:
            self.skip("the photo is gone")
            return True
        path = self.abs(op["path"])
        expect = set()
        if photo.mount is None or photo.state != "missing":
            expect.add(NOT_MISSING)
        mount = self.model.mounts.get(photo.mount) if photo.mount else None
        rel = None
        if mount is not None:
            rel = fuzz_disk.relative_below(mount.root, path)
            if rel is None:
                expect.add(OUTSIDE_MOUNT)
            else:
                parts = rel.split("/")
                listed = path.is_file() and not path.is_symlink() and \
                    fuzz_disk.is_supported_name(path.name) and \
                    not any(part.startswith(".") for part in parts) and \
                    not any(Path(mount.root).joinpath(*parts[:i]).is_symlink() or
                            (IS_WINDOWS and os.path.isjunction(Path(mount.root).joinpath(
                                *parts[:i]))) for i in range(1, len(parts) + 1))
                if not listed:
                    expect.add(NOT_LISTED)
                else:
                    if any(p.rel == rel for p in self.model.mounted_photos(mount.handle)
                           if p.handle != photo.handle):
                        expect.add(HAS_PHOTO)
                    if self.tools.is_locked(path) or fuzz_disk.read_fingerprint(path) is None:
                        expect.add(FILE_UNREADABLE)
        elif photo.mount is None:
            expect.add(OUTSIDE_MOUNT)
        if expect and NOT_MISSING not in expect and not (expect - {NOT_LISTED}) and \
                not path.exists():
            expect |= {FILE_UNREADABLE, NOT_LISTED}
        if not expect:
            expect = {"ok"}
        answer = self.call("library.mount.locate_photo",
                           {"element_id": photo.eid, "path": str(path)}, expect=expect)
        if answer.ok:
            info = path.stat()
            photo.rel = rel
            photo.path = os.path.join(mount.root, *rel.split("/"))
            photo.size, photo.mtime_ns = info.st_size, info.st_mtime_ns
            photo.fingerprint = fuzz_disk.read_fingerprint(path)
            photo.state = "present"
            parent = ""
            for part in rel.split("/")[:-1]:
                parent = f"{parent}/{part}" if parent else part
                if self.model.mount_folder(mount, parent) is None:
                    up = parent.rsplit("/", 1)[0] if "/" in parent else ""
                    handle = self.model.new_handle("f")
                    self.model.folders[handle] = fuzz_model.Folder(
                        handle, self.model.mount_folder(mount, up).handle, part,
                        mount=mount.handle, rel=parent)
            photo.home = self.model.mount_folder(mount, parent).handle
        return not answer.ok

    def do_remove_missing(self, op: dict) -> bool:
        mount = op["mount"]
        if mount not in self.model.mounts:
            self.call("library.mount.remove_missing", {"mount_id": self.mid(mount)},
                      expect={UNKNOWN_MOUNT})
            return True
        missing = [p for p in self.model.mounted_photos(mount) if p.state == "missing"]
        answer = self.call("library.mount.remove_missing", {"mount_id": self.mid(mount)},
                           expect={"ok"})
        if sorted(answer.result["removed_element_ids"]) != sorted(p.eid for p in missing):
            raise CheckFailure("outcome", f"remove_missing answered {answer_text(answer)}, the "
                                          f"model removes {sorted(p.eid for p in missing)}")
        for photo in missing:
            self.model.remove_photo(photo.handle)
        return False

    def do_delete_mounted(self, op: dict) -> bool:
        return self.do_delete_project(op)

    # M5 --------------------------------------------------------------------------------------

    def do_locate_root(self, op: dict) -> bool:
        mount = op["mount"]
        if mount not in self.model.mounts:
            self.call("library.mount.locate", {"mount_id": self.mid(mount),
                                                "path": str(self.abs(op["path"]))},
                      expect={UNKNOWN_MOUNT})
            return True
        path = self.abs(op["path"])
        if op.get("copy_from") is not None and not path.exists():
            source = self.abs(op["copy_from"])
            if source.is_dir() and not any(fuzz_disk.is_at_or_below(p, source)
                                           for p in set(self.tools.locks) | self.tools.unreadable):
                shutil.copytree(source, path, symlinks=True)
                for rel in op.get("drop", []):
                    fuzz_disk.remove_tree(path.joinpath(*rel.split("/")))
        expect = self.model.expect_mount(path, ignore=mount)
        prediction = None
        target = self.model.mounts[mount]
        old_root = target.root
        if expect == {"ok"}:
            target.root = os.path.realpath(path)
            prediction = self.predict(mount)
            target.root = old_root
        answer = self.call("library.mount.locate", {"mount_id": self.mid(mount),
                                                    "path": str(path)}, timeout_ms=1800000,
                           expect=expect)
        if answer.ok:
            target.root = os.path.realpath(path)
            self.mount_volume.pop(mount, None)
            self.compare_scan(answer.result["scan"], prediction, f"locate of {mount}")
            prediction.apply()
        return not answer.ok

    def do_volume_create(self, op: dict) -> bool:
        if not Volumes.available():
            self.skip("no hdiutil")
            return False
        handle = op["volume"]
        self.volumes.create(handle, op["name"])
        point = self.out / "volumes" / f"{handle}_{op['point']}"
        self.volumes.attach(handle, point)
        self.volume_points[handle] = point
        self.volume_labels[handle] = op["name"]
        self.write_tree(point / "photos", op["files"])
        # Test 44: a new volume without a mount of the project gives the card of the Disk
        # section after the volume arrival delay.
        time.sleep(3.0)
        self.wait_idle()
        card = self.call("library.volumes.arrived", {}, expect={"ok"}).result["volume"]
        if card is None or card.get("name") != op["name"] or \
                path_key(card.get("mount_point", "")) != path_key(point):
            raise CheckFailure("volume_card", f"the card of the new volume is {card}")
        return False

    def volume_mounts(self, handle: str) -> list[str]:
        return [m for m, (v, _) in self.mount_volume.items() if v == handle]

    def do_volume_detach(self, op: dict) -> bool:
        handle = op["volume"]
        if handle not in self.volumes.attached:
            self.skip("the volume is not attached")
            return False
        point = self.volume_points[handle]
        self.volumes.detach(handle)
        time.sleep(3.0)
        self.wait_idle()
        card = self.call("library.volumes.arrived", {}, expect={"ok"}).result["volume"]
        if card is not None and path_key(card.get("mount_point", "")) == path_key(point):
            raise CheckFailure("volume_card", f"the card stays after the volume went: {card}")
        # Test 42: the scan of a mount of the removed volume is offline and changes nothing.
        for mount in self.volume_mounts(handle):
            self.do_sync({"mount": mount})
        return False

    def do_volume_attach(self, op: dict) -> bool:
        handle = op["volume"]
        if handle not in self.volumes.images or handle in self.volumes.attached:
            self.skip("the volume is attached or does not exist")
            return False
        point = self.out / "volumes" / f"{handle}_{op['point']}"
        self.volumes.attach(handle, point)
        moved = path_key(point) != path_key(self.volume_points[handle])
        self.volume_points[handle] = point
        # The volume arrival delay (2 s), then the scan of each mount of the volume.
        time.sleep(3.0)
        self.wait_idle()
        for mount in self.volume_mounts(handle):
            if not moved:
                self.predict(mount).apply()
        if moved and self.volume_mounts(handle):
            # The mount follows the volume at the next project open (relocated_root).
            return self.do_reopen({})
        return False

    def do_volume_rename(self, op: dict) -> bool:
        handle = op["volume"]
        if handle not in self.volumes.attached:
            self.skip("the volume is not attached")
            return False
        self.volumes.rename(handle, op["name"])
        self.volume_labels[handle] = op["name"]
        time.sleep(1.0)
        self.wait_idle()
        # A scan that reaches the volume records its name.
        for mount in self.volume_mounts(handle):
            self.do_sync({"mount": mount})
            self.volume_names[mount] = op["name"]
        return False

    # M6 --------------------------------------------------------------------------------------

    def start_scans(self, mount: str) -> list[str]:
        answer = self.call("library.sync", {"mount_id": self.mid(mount), "wait": False},
                           expect={"ok"})
        return [task["task_id"] for task in answer.result["tasks"]]

    def do_scan_interleave(self, op: dict) -> bool:
        """A scan that runs while an import, a delete, or a save runs: the scan pauses and
        completes, and the save waits for the scan worker."""
        mount = op["mount"]
        if mount not in self.model.mounts:
            self.skip("the mount is gone")
            return True
        prediction = self.predict(mount)
        self.start_scans(mount)
        other = op["other"]
        if other == "import":
            self.do_import({"paths": op["paths"], "folder": op["folder"]})
        elif other == "delete":
            # An ordinary photo below the root directory is an adoption candidate of the scan;
            # the delete takes only the others, so the prediction stays exact.
            photos = [p for p in op["photos"] if p in self.model.photos and
                      self.model.photos[p].mount is None and
                      self.model.mount_of_path(self.model.photos[p].path) is None]
            if photos:
                self.do_delete_project({"photos": photos})
        elif other == "save":
            self.call("project.save", {}, expect={"ok"})
        self.wait_idle()
        prediction.apply()
        return False

    def do_scan_cancel(self, op: dict) -> bool:
        """tasks.cancel of a running scan keeps the stored batches; the next scan completes the
        rest, so the final state is the state of one complete scan."""
        mount = op["mount"]
        if mount not in self.model.mounts:
            self.skip("the mount is gone")
            return True
        prediction = self.predict(mount)
        tasks = self.start_scans(mount)
        if op.get("delay_ms"):
            time.sleep(op["delay_ms"] / 1000)
        for task in tasks:
            self.call("tasks.cancel", {"task_id": task},
                      expect={"ok", "rejected:not_cancelable"})
        self.wait_idle()
        self.call("library.sync", {"mount_id": self.mid(mount)}, timeout_ms=1800000,
                  expect={"ok"})
        prediction.apply()
        return False

    def do_scan_twice(self, op: dict) -> bool:
        mount = op["mount"]
        if mount not in self.model.mounts:
            self.skip("the mount is gone")
            return True
        prediction = self.predict(mount)
        self.start_scans(mount)
        self.start_scans(mount)
        if op.get("then") == "unmount":
            answer = self.call("library.unmount", {"mount_id": self.mid(mount)},
                               expect={"ok", "busy", "rejected:"})
            self.wait_idle()
            prediction.apply()
            if answer.ok:
                self.model.unmount(mount)
            return False
        self.wait_idle()
        prediction.apply()
        return False

    # M7 --------------------------------------------------------------------------------------

    def do_unmount(self, op: dict) -> bool:
        mount = op["mount"]
        if mount not in self.model.mounts:
            self.call("library.unmount", {"mount_id": self.mid(mount)}, expect={UNKNOWN_MOUNT})
            return True
        self.call("library.unmount", {"mount_id": self.mid(mount)}, expect={"ok"})
        self.model.unmount(mount)
        self.mount_volume.pop(mount, None)
        self.volume_names.pop(mount, None)
        return False

    def do_remount(self, op: dict) -> bool:
        return self.do_mount({"path": op["path"], "handle": op.get("handle")})

    # D ---------------------------------------------------------------------------------------

    def do_save_check(self, op: dict) -> bool:
        self.invariant_check_now()
        return False

    def do_reopen(self, op: dict) -> bool:
        self.call("project.close", {}, timeout_ms=1800000, expect={"ok"})
        self.call("project.open", {"path": str(self.project_path)}, timeout_ms=1800000,
                  expect={"ok"})
        for mount in self.model.mounts.values():
            mount.state = "not_scanned"
        self.scan_all_after_open()
        return False

    def do_kill_reopen(self, op: dict) -> bool:
        """Check 5 after a crash: the process ends at once; the open gives the state of the last
        save, and the open scans complete a scan that the crash stopped."""
        self.call("project.save", {}, expect={"ok"})
        predictions = {}
        during = op.get("during", "none")
        uncertain_import = []
        if during == "scan" and op.get("mount") in self.model.mounts:
            predictions[op["mount"]] = self.predict(op["mount"])
            self.start_scans(op["mount"])
        elif during == "import":
            paths = [self.abs(p) for p in op.get("paths", [])]
            expect, kept = self.import_expect(paths, ROOT)
            if expect == {"ok"} and all(p.is_file() for p in paths):
                answer = self.call("library.import", {"paths": [str(p) for p in paths]},
                                   expect={"ok"})
                uncertain_import = kept
                del answer
        if op.get("delay_ms"):
            time.sleep(op["delay_ms"] / 1000)
        self.session.kill()
        self.session.start(project=self.project_path, watcher=self.watcher_flag())
        for photo_path in uncertain_import:
            if fuzz_disk.has_image_content(photo_path):
                self.model.add_photo(str(photo_path), ROOT).uncertain = True
        for mount in self.model.mounts.values():
            mount.state = "not_scanned"
        self.scan_all_after_open()
        return False

    def watcher_flag(self) -> str:
        return "on" if self.options.mode == "watcher-on" else "off"

    # F ---------------------------------------------------------------------------------------

    def do_param_types(self, op: dict) -> bool:
        answer = self.call(op["method"], op["params"], expect={"invalid_params"})
        pointer = answer.data.get("pointer")
        if op.get("pointer") is not None and pointer != op["pointer"]:
            raise CheckFailure("outcome", f"{op['method']}: the pointer is {pointer!r}, "
                                          f"expected {op['pointer']!r}")
        return True

    # G ---------------------------------------------------------------------------------------

    def do_concurrent_round(self, op: dict) -> bool:
        """Two connections on disjoint subtrees while a mount scan runs (test 59): side a
        creates and renames ordinary folders below its top folder and imports into them; side b
        changes the directory of one mount and syncs it. The seed gives the operations, not the
        interleaving. The threads only call the session and record the answers; the model
        applies them in the order of each side after the join, which is exact for every
        interleaving because the sides share no folder, photo, or file."""
        failures: list[BaseException] = []
        results_a: list[tuple[dict, list[Answer]]] = []
        results_b: list[tuple[dict, Answer | None]] = []
        created: dict[str, int] = {}

        def fid_a(handle: str) -> int:
            if handle in created:
                return created[handle]
            return self.fid(handle)

        def run_a() -> None:
            try:
                for item in op["a"]:
                    kind = item["op"]
                    if kind == "folder_create":
                        answer = self.session.call("library.folder.create", {
                            "parent_id": fid_a(item["parent"]), "name": item["name"]})
                        if answer.ok:
                            created[item["handle"]] = answer.result["folder_id"]
                        results_a.append((item, [answer]))
                    elif kind == "folder_rename":
                        answer = self.session.call("library.folder.rename", {
                            "folder_id": fid_a(item["folder"]), "name": item["name"]})
                        results_a.append((item, [answer]))
                    elif kind == "import":
                        params = {"paths": [str(self.abs(p)) for p in item["paths"]]}
                        if item["folder"] != ROOT:
                            params["folder_id"] = fid_a(item["folder"])
                        answer = self.session.call("library.import", params)
                        answers = [answer]
                        if answer.ok:
                            answers.append(self.session.call(
                                "tasks.wait", {"task_id": answer.result["task_id"]}, 1800000))
                        results_a.append((item, answers))
            except BaseException as error:  # noqa: BLE001 - raised after the join
                failures.append(error)

        def run_b() -> None:
            try:
                for item in op["b"]:
                    if item["op"] == "sync":
                        results_b.append((item, self.session.call(
                            "library.sync", {"mount_id": self.mid(item["mount"])}, 1800000)))
                    else:
                        path = self.abs(item["path"])
                        if not path.exists() and path.parent.is_dir():
                            path.write_bytes(content_bytes(item["content"]))
                        results_b.append((item, None))
            except BaseException as error:  # noqa: BLE001 - raised after the join
                failures.append(error)

        threads = [threading.Thread(target=run_a), threading.Thread(target=run_b)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        if failures:
            raise failures[0]

        def accept(answer: Answer, expect: set[str], what: str) -> bool:
            self.current.outcomes.append(f"{what}={answer.outcome}")
            if answer.outcome.startswith("error:") or answer.outcome not in expect | {"busy"}:
                raise CheckFailure("outcome", f"concurrent {what} answered {answer.outcome}, "
                                              f"expected one of {sorted(expect)}: "
                                              f"{answer_text(answer)}")
            return answer.ok

        for item, answers in results_a:
            kind = item["op"]
            if kind == "folder_create":
                if accept(answers[0], self.model.expect_create(
                        item["parent"] if item["parent"] in self.model.folders else None,
                        item["name"]), kind):
                    self.model.apply_create(item["handle"], item["parent"], item["name"],
                                            answers[0].result["folder_id"])
            elif kind == "folder_rename":
                if accept(answers[0], self.model.expect_rename(
                        item["folder"] if item["folder"] in self.model.folders else None,
                        item["name"]), kind):
                    self.model.apply_rename(item["folder"], item["name"])
            elif kind == "import":
                expect, kept = self.import_expect([self.abs(p) for p in item["paths"]],
                                                  item["folder"])
                if accept(answers[0], expect, kind):
                    for path in kept:
                        if fuzz_disk.has_image_content(path):
                            self.model.add_photo(str(path), item["folder"])
        for item, answer in results_b:
            if answer is None:
                continue
            # #1216: a sync during an import of side a is refused (-32002 without data.error),
            # and the documentation says that it waits. Remove the refusal with the fix.
            if not accept(answer, {"ok", "rejected:"}, "library.sync"):
                continue
            prediction = self.predict(item["mount"])
            self.compare_scan(answer.result["scans"][0], prediction, "concurrent sync")
            prediction.apply()
        self.check_state()
        self.invariant_check_now()
        return False


# ------------------------------------------------------------------------------------------------
# Generator
# ------------------------------------------------------------------------------------------------


class Generator:
    """Writes the next operation from the seed and the state of the driver."""

    def __init__(self, seed: int, driver: Driver, weights: dict[str, float]) -> None:
        self.rng = random.Random(seed)
        self.driver = driver
        self.weights = {k: w for k, w in weights.items() if w > 0}
        self.pending: list[dict] = []
        self.mount_count = 0
        self.volume_count = 0
        self.burst = 0
        self.content_seed = seed * 7919 % 1000003
        self.fresh_counter = 0
        self.next_id = 1
        self.created: list[dict] = []

    @property
    def model(self) -> LibraryModel:
        return self.driver.model

    # Helpers -------------------------------------------------------------------------------

    def content(self, big: bool = False) -> dict:
        self.content_seed += 1
        roll = self.rng.random()
        if roll < 0.03 and not big:
            try:
                fuzz_disk.raw_fixture()
                return {"kind": "raw"}
            except FileNotFoundError:
                pass
        if big:
            size = self.rng.choice([300 * 1024, 400 * 1024])
        else:
            size = self.rng.choice([1500, 2048, 3000, 4096, 6000, 9000, 20000])
        return {"kind": "jpeg", "seed": self.content_seed, "size": size}

    def name(self, ext: str | None = None) -> str:
        stem = self.rng.choice(["img", "IMG", "dsc", "photo", "a", "b", "x_y", "z"]) + \
            str(self.rng.randrange(1000))
        return stem + (ext or self.rng.choice([".jpg", ".jpg", ".JPG", ".jpeg", ".jpe"]))

    def ordinary_folders(self, with_root: bool = True) -> list[str]:
        return [h for h, f in self.model.folders.items()
                if f.mount is None and (with_root or h != ROOT)]

    def mounted_folders(self) -> list[str]:
        return [h for h, f in self.model.folders.items() if f.mount is not None]

    def live_mounts(self) -> list[str]:
        return [h for h, m in self.model.mounts.items() if Path(m.root).is_dir()]

    def mount_files(self, mount: str | None = None, listed_only: bool = True) -> list[Path]:
        roots = [Path(self.model.mounts[m].root) for m in
                 ([mount] if mount else self.live_mounts())]
        out = []
        for root in roots:
            for directory, dirs, files in os.walk(root):
                dirs[:] = [d for d in dirs if not d.startswith(".")
                           and not os.path.islink(os.path.join(directory, d))]
                for name in files:
                    if not listed_only or (fuzz_disk.is_supported_name(name) and
                                           not name.startswith(".")):
                        out.append(Path(directory) / name)
        return sorted(out)

    def mount_dirs(self, mount: str | None = None) -> list[Path]:
        roots = [Path(self.model.mounts[m].root) for m in
                 ([mount] if mount else self.live_mounts())]
        out = []
        for root in roots:
            out.append(root)
            for directory, dirs, _ in os.walk(root):
                dirs[:] = [d for d in dirs if not d.startswith(".")
                           and not os.path.islink(os.path.join(directory, d))]
                out += [Path(directory) / d for d in dirs]
        return sorted(out)

    def rel(self, path: Path) -> str:
        for handle, point in self.driver.volume_points.items():
            if fuzz_disk.is_at_or_below(path, point):
                below = path.relative_to(point).as_posix()
                return f"vol:{handle}/{below}" if below != "." else f"vol:{handle}"
        return self.driver.rel_of(path)

    def src_files(self) -> list[Path]:
        return sorted((self.driver.disk / "src").glob("*"))

    def new_src_file(self, content: dict | None = None, ext: str | None = None,
                     directory: str = "src") -> str:
        """A new file outside the mounts. The operation that uses it carries it in 'create', so
        a replay writes it before the operation."""
        path = self.driver.disk / directory / self.name(ext)
        while path.exists():
            path = self.driver.disk / directory / self.name(ext)
        spec = content or self.content()
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content_bytes(spec))
        self.created.append({"path": self.rel(path), "content": spec})
        return self.rel(path)

    def fresh(self, kind: str) -> str:
        """A handle of the generator: '<kind>g<n>', apart from the handles of the executor."""
        self.fresh_counter += 1
        return f"{kind}g{self.fresh_counter}"

    def pick(self, items):
        return self.rng.choice(items) if items else None

    def folder_name(self) -> str:
        roll = self.rng.random()
        if roll < 0.35:
            return self.rng.choice(NAME_SAMPLES)
        if roll < 0.5:
            # Names of mount roots stay out: a mount of a directory whose name an ordinary
            # folder of the root has is not a case of the test list.
            names = [f.name for f in self.model.folders.values()
                     if f.name and not f.name.lower().startswith(MOUNT_PREFIX)]
            if names:
                name = self.rng.choice(names)
                return self.rng.choice([name, name.upper(), name.lower(), " " + name,
                                        unicodedata.normalize("NFD", name)])
        name = self.rng.choice(["Album", "Trip", "Family", "x", "2026", "Raw"]) + \
            str(self.rng.randrange(100))
        return name

    # Kinds ---------------------------------------------------------------------------------

    def next(self, step: int) -> dict:
        """The operation of @p step, with its operation id."""
        op = self.pending.pop(0) if self.pending else self.choose(step)
        if self.created:
            op["create"] = op.get("create", []) + self.created
            self.created = []
        op["id"] = self.next_id
        self.next_id += 1
        return op

    def choose(self, step: int) -> dict:
        options = self.driver.options
        unsettled = options.mode == "watcher-on" and self.driver.unsettled
        periodic = None
        if step and options.reopen_every and step % options.reopen_every == 0:
            periodic = {"op": "reopen"}
        elif step and options.save_every and step % options.save_every == 0:
            periodic = {"op": "save_check"}
        if periodic is not None:
            if unsettled:
                self.pending.append(periodic)
                return {"op": "settle", "delay_s": 1.0}
            return periodic
        if unsettled and self.burst >= self.rng.randint(1, 4):
            self.burst = 0
            return {"op": "settle", "delay_s": 1.0}
        kinds = list(self.weights)
        if unsettled:
            kinds = [k for k in kinds if k in DISK_KINDS]
            if not kinds:
                return {"op": "settle", "delay_s": 1.0}
        for _ in range(60):
            kind = self.rng.choices(kinds, weights=[self.weights[k] for k in kinds])[0]
            op = getattr(self, f"gen_{kind}")()
            if op is not None and options.avoid_known_issues and \
                    any(covers(op) for _, _, covers in KNOWN_ISSUES):
                op = None
            if op is not None:
                if kind in DISK_KINDS:
                    self.burst += 1
                return op
        return {"op": "settle", "delay_s": 1.0} if unsettled else {"op": "save_check"}

    def gen_folder_create(self):
        parent = self.pick(self.ordinary_folders()) if self.rng.random() < 0.9 else \
            self.pick(self.mounted_folders()) or ROOT
        return {"op": "folder_create", "parent": parent, "name": self.folder_name(),
                "handle": self.fresh("f")}

    def gen_folder_rename(self):
        folder = self.pick(list(self.model.folders))
        return {"op": "folder_rename", "folder": folder, "name": self.folder_name()}

    def gen_folder_move(self):
        folders = list(self.model.folders)
        folder = self.pick(folders)
        roll = self.rng.random()
        if roll < 0.15:
            parent = self.pick(self.model.subtree(folder))
        elif roll < 0.25:
            parent = self.model.folders[folder].parent or ROOT
        else:
            parent = self.pick(folders)
        return {"op": "folder_move", "folder": folder, "parent": parent}

    def gen_folder_delete(self):
        folders = self.ordinary_folders(with_root=False)
        if self.rng.random() < 0.2 or not folders:
            folders = list(self.model.folders)
        return {"op": "folder_delete", "folder": self.pick(folders)}

    def gen_folder_bad_id(self):
        method = self.rng.choice(["library.folder.rename", "library.folder.move",
                                  "library.folder.delete", "library.folder.create",
                                  "library.list", "library.import"])
        roll = self.rng.random()
        if roll < 0.2:
            value = -self.rng.randrange(1, 5)
        elif roll < 0.4:
            value = self.rng.choice([2 ** 31 + 7, 2 ** 32 + 3, 2 ** 40])
        elif roll < 0.6 and self.model.photos:
            value = self.pick(list(self.model.photos))
        elif roll < 0.7 and method in ("library.folder.rename", "library.folder.move",
                                       "library.folder.delete"):
            value = 0
        else:
            used = {f.fid for f in self.model.folders.values()} | \
                {p.eid for p in self.model.photos.values()}
            value = max([x for x in used if x is not None] + [0]) + self.rng.randrange(1, 50)
        op = {"op": "folder_bad_id", "method": method, "id": value}
        if method == "library.import":
            op["file"] = self.new_src_file()
        return op

    def gen_deep_tree(self):
        parent = self.pick(self.ordinary_folders())
        count = max(2, int(200 * self.driver.options.scale))
        return {"op": "deep_tree", "parent": parent, "prefix": f"d{self.rng.randrange(1000)}_",
                "count": count, "handle": self.fresh("f")}

    def gen_wide_folder(self):
        parent = self.pick(self.ordinary_folders())
        count = max(2, int(2000 * self.driver.options.scale))
        return {"op": "wide_folder", "parent": parent, "prefix": f"w{self.rng.randrange(1000)}_",
                "count": count, "handle": self.fresh("f")}

    def gen_import(self):
        folder = self.pick(self.ordinary_folders()) if self.rng.random() < 0.85 else \
            self.pick(self.mounted_folders()) or ROOT
        paths = []
        for _ in range(self.rng.randint(1, 4)):
            roll = self.rng.random()
            existing = self.src_files()
            if roll < 0.3 and existing:
                paths.append(self.rel(self.pick(existing)))
            elif roll < 0.4 and self.mount_files():
                paths.append(self.rel(self.pick(self.mount_files())))
            elif roll < 0.45:
                paths.append(self.new_src_file(ext=".txt"))
            elif roll < 0.48:
                paths.append("src/does_not_exist.jpg")
            else:
                paths.append(self.new_src_file())
        return {"op": "import", "paths": paths, "folder": folder,
                "explicit_root": folder == ROOT and self.rng.random() < 0.5}

    def gen_import_bad_files(self):
        kind = self.rng.choice(["empty", "text", "cut_jpeg"])
        self.content_seed += 1
        spec = {"kind": kind, "seed": self.content_seed, "size": 3000}
        # The damaged files stay in their own directory: no other operation takes them.
        return {"op": "import_bad_files", "paths": [self.new_src_file(spec, ".jpg", "bad")],
                "folder": self.pick(self.ordinary_folders())}

    def some_photos(self, count: int | None = None) -> list[str]:
        photos = list(self.model.photos)
        if not photos:
            return []
        return self.rng.sample(photos, min(len(photos), count or self.rng.randint(1, 4)))

    def gen_link(self):
        photos = self.some_photos()
        if not photos:
            return None
        roll = self.rng.random()
        if roll < 0.1:
            folder = ROOT
        elif roll < 0.2 and self.mounted_folders():
            folder = self.pick(self.mounted_folders())
        else:
            folder = self.pick(self.ordinary_folders(with_root=False))
        if folder is None:
            return None
        return {"op": "link", "photos": photos, "folder": folder}

    def gen_delete_album(self):
        folders = [f for f in self.ordinary_folders(with_root=False) if self.model.photos_in(f)]
        if self.rng.random() < 0.15 and self.mounted_folders():
            folders = [f for f in self.mounted_folders() if self.model.photos_in(f)]
        folder = self.pick(folders)
        if folder is None:
            return None
        inside = sorted(self.model.photos_in(folder))
        photos = self.rng.sample(inside, self.rng.randint(1, min(3, len(inside))))
        if self.rng.random() < 0.3:
            photos += self.some_photos(1)
        return {"op": "delete_album", "photos": sorted(set(photos)), "folder": folder}

    def gen_delete_project(self):
        photos = [p for p in self.model.photos if self.model.photos[p].mount is None]
        if not photos:
            return None
        chosen = self.rng.sample(photos, self.rng.randint(1, min(3, len(photos))))
        if self.rng.random() < 0.3:
            chosen += self.some_photos(1)
        return {"op": "delete_project", "photos": sorted(set(chosen))}

    def gen_delete_unknown_ids(self):
        used = [p.eid for p in self.model.photos.values() if p.eid is not None]
        unknown = [max(used + [100]) + self.rng.randrange(100, 1000)]
        return {"op": "delete_unknown_ids", "photos": self.some_photos(2), "unknown_ids": unknown}

    def mount_tree(self, files: int) -> tuple[list[dict], list[str]]:
        entries, used = [], set()
        dirs = [""] + [f"sub{self.rng.randrange(5)}" for _ in range(2)] + \
            [f"sub{self.rng.randrange(5)}/deep{self.rng.randrange(3)}"]
        for _ in range(files):
            directory = self.pick(dirs)
            name = self.name()
            path = f"{directory}/{name}" if directory else name
            if path.lower() in used:
                continue
            used.add(path.lower())
            entries.append({"path": path, "content": self.content()})
        extras = []
        if self.rng.random() < 0.5:
            extras.append({"path": ".hidden.jpg", "content": self.content()})
            extras.append({"path": ".DS_Store", "content": {"kind": "text", "seed": 1}})
            extras.append({"path": "~$temp.jpg", "content": self.content()})
            extras.append({"path": "notes.txt", "content": {"kind": "text", "seed": 2}})
            extras.append({"path": ".git/x.jpg", "content": self.content()})
        empty = ["empty_dir"] if self.rng.random() < 0.5 else []
        return entries + extras, empty

    def gen_mount(self):
        self.mount_count += 1
        roll = self.rng.random()
        volumes = list(self.driver.volumes.attached)
        if volumes and roll < 0.3:
            volume = self.pick(volumes)
            path = f"vol:{volume}/photos"
            if any(path_key(self.driver.abs(path)) == path_key(m.root)
                   for m in self.model.mounts.values()):
                return None
            return {"op": "mount", "path": path, "volume": volume, "handle": self.fresh("m")}
        files, empty = self.mount_tree(0 if roll < 0.1 else self.rng.randint(1, 12))
        return {"op": "mount", "path": f"{MOUNT_PREFIX}{self.mount_count}", "files": files,
                "empty_dirs": empty, "handle": self.fresh("m")}

    def gen_mount_refused(self):
        roll = self.rng.random()
        mounts = self.live_mounts()
        if roll < 0.35 and mounts:
            mount = self.model.mounts[self.pick(mounts)]
            dirs = self.mount_dirs(self.pick(mounts))
            choice = self.rng.random()
            if choice < 0.4:
                path = self.rel(self.pick(dirs))
            elif choice < 0.6:
                path = self.rel(Path(mount.root).parent) if \
                    Path(mount.root).parent != self.driver.disk else self.rel(Path(mount.root))
            else:
                path = self.rel(Path(mount.root))
                path = path.upper() if self.rng.random() < 0.5 else path
                if not IS_WINDOWS and sys.platform != "darwin":
                    path = self.rel(Path(mount.root))
            return {"op": "mount_refused", "path": path}
        if roll < 0.5 and mounts:
            mount = self.model.mounts[self.pick(mounts)]
            return {"op": "mount_refused", "path": f"link{self.rng.randrange(10000)}",
                    "make": "symlink", "target": self.rel(Path(mount.root))}
        if roll < 0.75:
            return {"op": "mount_refused", "path": f"nope{self.rng.randrange(10000)}/x"}
        return {"op": "mount_refused", "path": f"outside/file{self.rng.randrange(10000)}.bin",
                "make": "file"}

    def gen_mounted_structure_edit(self):
        folder = self.pick(self.mounted_folders())
        if folder is None:
            return None
        action = self.rng.choice(["create", "rename", "move", "move_into", "delete", "import",
                                  "link"])
        op = {"op": "mounted_structure_edit", "folder": folder, "action": action}
        if action == "create":
            op.update(name=self.folder_name(), handle=self.fresh("f"))
        elif action == "rename":
            op["name"] = self.folder_name()
        elif action in ("move", "move_into"):
            op["parent"] = self.pick(self.ordinary_folders())
        elif action == "import":
            op["paths"] = [self.new_src_file()]
        elif action == "link":
            photos = self.some_photos(2)
            if not photos:
                return None
            op["photos"] = photos
        return op

    # Disk kinds ----------------------------------------------------------------------------

    def target_dir(self) -> Path | None:
        dirs = self.mount_dirs()
        return self.pick(dirs)

    def gen_disk_add(self):
        directory = self.target_dir()
        if directory is None:
            return None
        if self.rng.random() < 0.2:
            directory = directory / f"new{self.rng.randrange(100)}"
        return {"op": "disk_add", "path": self.rel(directory / self.name()),
                "content": self.content(), "parents": True}

    def gen_disk_unicode_name(self):
        directory = self.target_dir()
        if directory is None:
            return None
        base = self.rng.choice(["café", "Ångström", "风景",
                                "\U0001F4F7shot", "naïve", "é" * 60])
        form = self.rng.choice(["NFC", "NFD"])
        name = unicodedata.normalize(form, base) + ".jpg"
        if any(fuzz_disk.nfc(p.name).casefold() == fuzz_disk.nfc(name).casefold()
               for p in directory.iterdir()):
            return None
        return {"op": "disk_unicode_name", "path": self.rel(directory / name),
                "content": self.content()}

    def gen_disk_unlisted(self):
        directory = self.target_dir()
        if directory is None:
            return None
        name = self.rng.choice([".hidden.jpg", "notes.txt", "image.jpg.part", "x.xmp",
                                ".dot/inner.jpg", "image.jpgx", "raw.ARW.bak"])
        return {"op": "disk_unlisted", "path": self.rel(directory / name),
                "content": self.content(), "parents": True}

    def gen_disk_many(self):
        mount = self.pick(self.live_mounts())
        if mount is None:
            return None
        count = self.rng.choice([499, 500, 501, 1001])
        count = max(2, int(count * self.driver.options.scale))
        return {"op": "disk_many", "dir": self.rel(Path(self.model.mounts[mount].root) /
                                                   f"many{self.rng.randrange(1000)}"),
                "count": count, "seed": self.rng.randrange(1000)}

    def gen_disk_delete(self):
        roll = self.rng.random()
        if roll < 0.75:
            path = self.pick(self.mount_files(listed_only=False))
        else:
            dirs = [d for d in self.mount_dirs() if d not in
                    {Path(m.root) for m in self.model.mounts.values()}]
            path = self.pick(dirs)
        return None if path is None else {"op": "disk_delete", "path": self.rel(path)}

    def gen_disk_rename(self):
        path = self.pick(self.mount_files(listed_only=False))
        if path is None:
            return None
        return {"op": "disk_rename", "path": self.rel(path),
                "to": self.rel(path.parent / self.name(path.suffix or None))}

    def gen_disk_case_rename(self):
        path = self.pick(self.mount_files())
        if path is None:
            return None
        name = path.name
        other = name.swapcase() if name.swapcase() != name else name.upper()
        if other == name:
            return None
        return {"op": "disk_case_rename", "path": self.rel(path), "to": self.rel(path.parent / other)}

    def gen_disk_move(self):
        path = self.pick(self.mount_files())
        directory = self.pick(self.mount_dirs())
        if path is None or directory is None:
            return None
        if self.rng.random() < 0.8:
            # Stay in the same mount most of the time.
            same = self.model.mount_of_path(path)
            directory = self.pick(self.mount_dirs(same.handle)) if same else directory
        name = path.name if self.rng.random() < 0.7 else self.name(path.suffix)
        return {"op": "disk_move", "path": self.rel(path), "to": self.rel(directory / name)}

    def gen_disk_rename_dir(self):
        roots = {Path(m.root) for m in self.model.mounts.values()}
        dirs = [d for d in self.mount_dirs() if d not in roots]
        directory = self.pick(dirs)
        if directory is None:
            return None
        return {"op": "disk_rename_dir", "path": self.rel(directory),
                "to": self.rel(directory.parent / f"ren{self.rng.randrange(1000)}")}

    def gen_disk_move_dir(self):
        roots = {Path(m.root) for m in self.model.mounts.values()}
        dirs = [d for d in self.mount_dirs() if d not in roots]
        directory = self.pick(dirs)
        if directory is None:
            return None
        same = self.model.mount_of_path(directory)
        targets = [d for d in self.mount_dirs(same.handle)
                   if not fuzz_disk.is_at_or_below(d, directory)]
        target = self.pick(targets)
        if target is None:
            return None
        return {"op": "disk_move_dir", "path": self.rel(directory),
                "to": self.rel(target / directory.name)}

    def gen_disk_rewrite(self):
        path = self.pick(self.mount_files())
        if path is None:
            return None
        size = path.stat().st_size if self.rng.random() < 0.5 else None
        content = self.content()
        if size is not None and size > 800:
            content["size"] = size
        return {"op": "disk_rewrite", "path": self.rel(path), "content": content}

    def gen_disk_middle_change(self):
        big = [p for p in self.mount_files() if p.stat().st_size > 3 * fuzz_disk.FINGERPRINT_BLOCK]
        if not big or self.rng.random() < 0.3:
            directory = self.target_dir()
            if directory is None:
                return None
            spec = self.content(big=True)
            path = directory / self.name(".jpg")
            self.pending.append({"op": "disk_middle_change", "path": self.rel(path),
                                 "content": dict(spec, middle=self.rng.randrange(1, 100))})
            return {"op": "disk_add", "path": self.rel(path), "content": spec}
        return None

    def gen_disk_touch(self):
        path = self.pick(self.mount_files())
        return None if path is None else {"op": "disk_touch", "path": self.rel(path),
                                          "delta_s": self.rng.choice([-3600, 2, 60])}

    def gen_disk_file_to_dir(self):
        path = self.pick(self.mount_files())
        return None if path is None else {"op": "disk_file_to_dir", "path": self.rel(path),
                                          "content": self.content()}

    def gen_disk_dir_to_file(self):
        roots = {Path(m.root) for m in self.model.mounts.values()}
        dirs = [d for d in self.mount_dirs() if d not in roots]
        directory = self.pick(dirs)
        if directory is None:
            return None
        return {"op": "disk_dir_to_file", "path": self.rel(directory), "content": self.content()}

    def gen_disk_copy(self):
        path = self.pick(self.mount_files())
        directory = self.target_dir()
        if path is None or directory is None:
            return None
        name = path.name if self.rng.random() < 0.5 else self.name(path.suffix)
        return {"op": "disk_copy", "path": self.rel(path), "to": self.rel(directory / name)}

    def gen_disk_swap_pair(self):
        path = self.pick(self.mount_files())
        if path is None:
            return None
        same = self.model.mount_of_path(path)
        dirs = self.mount_dirs(same.handle) if same else []
        if not dirs:
            return None
        return {"op": "disk_swap_pair", "a": self.rel(path),
                "b": self.rel(path.parent / ("twin_" + path.name)),
                "a_to": self.rel(self.pick(dirs) / self.name(path.suffix)),
                "b_to": self.rel(self.pick(dirs) / self.name(path.suffix))}

    def gen_disk_move_and_change(self):
        path = self.pick(self.mount_files())
        directory = self.target_dir()
        if path is None or directory is None:
            return None
        return {"op": "disk_move_and_change", "path": self.rel(path),
                "to": self.rel(directory / self.name(path.suffix)), "content": self.content()}

    def gen_disk_hardlink(self):
        path = self.pick(self.mount_files())
        directory = self.target_dir()
        if path is None or directory is None:
            return None
        return {"op": "disk_hardlink", "path": self.rel(path),
                "to": self.rel(directory / self.name(path.suffix))}

    def gen_disk_unreadable_dir(self):
        roots = {Path(m.root) for m in self.model.mounts.values()}
        dirs = [d for d in self.mount_dirs() if d not in roots]
        directory = self.pick(dirs)
        return None if directory is None else {"op": "disk_unreadable_dir",
                                               "path": self.rel(directory)}

    def gen_disk_readable_dir(self):
        denied = sorted(self.driver.tools.unreadable)
        return None if not denied else {"op": "disk_readable_dir",
                                        "path": self.rel(Path(self.pick(denied)))}

    def gen_disk_lock(self):
        if not DiskTools.can_lock():
            return None
        path = self.pick(self.mount_files())
        return None if path is None else {"op": "disk_lock", "path": self.rel(path)}

    def gen_disk_unlock(self):
        locked = sorted(self.driver.tools.locks)
        return None if not locked else {"op": "disk_unlock",
                                        "path": self.rel(Path(self.pick(locked)))}

    def gen_sync(self):
        if not self.model.mounts:
            return None
        if self.rng.random() < 0.3:
            return {"op": "sync"}
        if self.rng.random() < 0.05:
            return {"op": "sync", "mount": "mg_unknown"}
        return {"op": "sync", "mount": self.pick(list(self.model.mounts))}

    def gen_locate_photo(self):
        missing = [p for p in self.model.photos.values() if p.state == "missing"]
        photo = self.pick(missing) if missing and self.rng.random() < 0.8 else \
            self.pick(list(self.model.photos.values()))
        if photo is None:
            return None
        roll = self.rng.random()
        files = self.mount_files(photo.mount) if photo.mount and \
            photo.mount in self.live_mounts() else []
        if roll < 0.5 and files:
            path = self.pick(files)
        elif roll < 0.6:
            path = self.pick(self.mount_files()) or self.driver.disk / "src" / "none.jpg"
        elif roll < 0.7:
            path = Path(self.driver.abs(self.new_src_file()))
        elif roll < 0.85 and photo.mount in self.model.mounts and \
                Path(self.model.mounts[photo.mount].root).is_dir():
            directory = self.pick(self.mount_dirs(photo.mount))
            name = self.rng.choice(["x.txt", ".hidden.jpg", "fresh.jpg"])
            target = directory / name
            if not target.exists():
                self.pending.append({"op": "locate_photo", "photo": photo.handle,
                                     "path": self.rel(target)})
                return {"op": "disk_add", "path": self.rel(target), "content": self.content()}
            path = target
        else:
            path = self.pick(self.mount_files(listed_only=False)) or \
                self.driver.disk / "outside" / "x.jpg"
        return {"op": "locate_photo", "photo": photo.handle, "path": self.rel(path)}

    def gen_remove_missing(self):
        if not self.model.mounts:
            return None
        return {"op": "remove_missing", "mount": self.pick(list(self.model.mounts))}

    def gen_delete_mounted(self):
        mounted = [p for p in self.model.photos if self.model.photos[p].mount]
        if not mounted:
            return None
        chosen = self.rng.sample(mounted, min(len(mounted), self.rng.randint(1, 3)))
        if self.rng.random() < 0.3:
            chosen += self.some_photos(1)
        return {"op": "delete_mounted", "photos": sorted(set(chosen))}

    def gen_locate_root(self):
        mount = self.pick(list(self.model.mounts))
        if mount is None or self.driver.mount_volume.get(mount):
            return None
        root = Path(self.model.mounts[mount].root)
        roll = self.rng.random()
        if roll < 0.2:
            return {"op": "locate_root", "mount": mount, "path": self.rel(root)}
        if roll < 0.35:
            other = self.pick([m for m in self.live_mounts() if m != mount])
            if other:
                return {"op": "locate_root", "mount": mount,
                        "path": self.rel(Path(self.model.mounts[other].root))}
        if roll < 0.45:
            return {"op": "locate_root", "mount": mount, "path": f"nope{self.rng.randrange(99)}"}
        source = root if root.is_dir() else Path(str(root) + "_away")
        if not source.is_dir():
            return None
        target = f"{self.rel(source).split('/')[0]}_loc{self.rng.randrange(1000)}"
        files = [f.relative_to(source).as_posix() for f in self.mount_files(mount)] \
            if root.is_dir() else []
        drop = self.rng.sample(files, len(files) // 3) if files else []
        return {"op": "locate_root", "mount": mount, "path": target,
                "copy_from": self.rel(source), "drop": drop}

    def gen_root_away(self):
        mount = self.pick(self.live_mounts())
        return None if mount is None else {"op": "root_away", "mount": mount}

    def gen_root_back(self):
        away = [h for h, m in self.model.mounts.items() if Path(m.root + "_away").is_dir()]
        return None if not away else {"op": "root_back", "mount": self.pick(away)}

    def gen_volume_create(self):
        if not Volumes.available() or len(self.driver.volumes.images) >= 2:
            return None
        self.volume_count += 1
        files, _ = self.mount_tree(self.rng.randint(1, 6))
        return {"op": "volume_create", "volume": f"v{self.volume_count}",
                "name": f"FUZZ{self.volume_count}", "point": 0, "files": files}

    def gen_volume_detach(self):
        attached = list(self.driver.volumes.attached)
        return None if not attached else {"op": "volume_detach", "volume": self.pick(attached)}

    def gen_volume_attach(self):
        detached = [v for v in self.driver.volumes.images if v not in self.driver.volumes.attached]
        if not detached:
            return None
        return {"op": "volume_attach", "volume": self.pick(detached),
                "point": self.rng.choice([0, 0, 1])}

    def gen_volume_rename(self):
        attached = list(self.driver.volumes.attached)
        if not attached:
            return None
        return {"op": "volume_rename", "volume": self.pick(attached),
                "name": f"FUZZ{self.rng.randrange(100, 999)}"}

    def gen_scan_interleave(self):
        mount = self.pick(self.live_mounts())
        if mount is None:
            return None
        other = self.rng.choice(["import", "delete", "save"])
        op = {"op": "scan_interleave", "mount": mount, "other": other}
        if other == "import":
            op.update(paths=[self.new_src_file() for _ in range(self.rng.randint(1, 3))],
                      folder=self.pick(self.ordinary_folders()))
        elif other == "delete":
            op["photos"] = [p for p in self.some_photos(3) if self.model.photos[p].mount is None]
        return op

    def gen_scan_cancel(self):
        mount = self.pick(self.live_mounts())
        return None if mount is None else {"op": "scan_cancel", "mount": mount,
                                           "delay_ms": self.rng.choice([0, 5, 50, 200])}

    def gen_scan_twice(self):
        mount = self.pick(self.live_mounts())
        if mount is None:
            return None
        return {"op": "scan_twice", "mount": mount,
                "then": "unmount" if self.rng.random() < 0.2 else "wait"}

    def gen_unmount(self):
        if not self.model.mounts:
            return None
        return {"op": "unmount", "mount": self.pick(list(self.model.mounts))}

    def gen_remount(self):
        mounted = {path_key(m.root) for m in self.model.mounts.values()}
        candidates = [p for p in self.driver.disk.glob(f"{MOUNT_PREFIX}*")
                      if p.is_dir() and path_key(p) not in mounted and
                      not p.name.endswith("_away")]
        path = self.pick(sorted(candidates))
        return None if path is None else {"op": "remount", "path": self.rel(path),
                                          "handle": self.fresh("m")}

    def gen_save_check(self):
        return {"op": "save_check"}

    def gen_reopen(self):
        return {"op": "reopen"}

    def gen_kill_reopen(self):
        roll = self.rng.random()
        if roll < 0.4 and self.live_mounts():
            return {"op": "kill_reopen", "during": "scan", "mount": self.pick(self.live_mounts()),
                    "delay_ms": self.rng.choice([0, 20, 100, 400])}
        if roll < 0.7:
            return {"op": "kill_reopen", "during": "import",
                    "paths": [self.new_src_file() for _ in range(self.rng.randint(1, 5))],
                    "delay_ms": self.rng.choice([0, 30, 200])}
        return {"op": "kill_reopen", "during": "none"}

    def gen_param_types(self):
        cases = [
            ("library.folder.create", {"parent_id": "0", "name": "x"}, "/parent_id"),
            ("library.folder.create", {"parent_id": 0, "name": 5}, "/name"),
            ("library.folder.create", {"parent_id": 0}, None),
            ("library.folder.create", {"parent_id": 0, "name": "x", "extra": 1}, "/extra"),
            ("library.folder.rename", {"folder_id": 1.5, "name": "x"}, "/folder_id"),
            ("library.folder.move", {"folder_id": 1}, None),
            ("library.folder.delete", {"folder_id": [1]}, "/folder_id"),
            ("library.link", {"element_ids": "1", "folder_id": 1}, "/element_ids"),
            ("library.link", {"element_ids": [], "folder_id": 1}, None),
            ("library.mount", {"path": 7}, "/path"),
            ("library.mount", {}, None),
            ("library.mount", {"path": "x", "wait": "no"}, "/wait"),
            ("library.sync", {"mount_id": -1}, "/mount_id"),
            ("library.sync", {"mount_id": "1"}, "/mount_id"),
            ("library.sync", {"mount_id": 1, "x": True}, "/x"),
            ("library.unmount", {}, None),
            ("library.mount.locate", {"mount_id": 1}, None),
            ("library.mount.locate_photo", {"element_id": "a", "path": "x"}, "/element_id"),
            ("library.mount.remove_missing", {"mount_id": None}, "/mount_id"),
            ("library.mounts", {"with_ids": 1}, "/with_ids"),
            ("library.mounts.wait_idle", {"x": 1}, "/x"),
            ("library.import", {"paths": [], "folder_id": 0}, None),
            ("library.import", {"paths": ["a"], "folder": "b"}, None),
            ("library.delete", {"element_ids": [1], "scope": "nowhere"}, "/scope"),
            ("library.delete", {"element_ids": [1]}, None),
            ("library.list", {"folder_id": 0, "limit": 0}, "/limit"),
            ("library.list", {"folder_id": 0, "limit": 1001}, "/limit"),
            ("tasks.cancel", {}, None),
            ("library.folders", {"x": 1}, "/x"),
        ]
        method, params, pointer = self.pick(cases)
        return {"op": "param_types", "method": method, "params": params, "pointer": pointer}

    def gen_concurrent_round(self):
        """Ordinary folder operations below one top folder (side a) and disk changes and syncs
        of one mount (side b)."""
        mount = self.pick(self.live_mounts())
        if mount is None:
            return None
        top = self.fresh("f")
        self.pending.append({"op": "folder_create", "parent": ROOT,
                             "name": f"conc{self.rng.randrange(100000)}", "handle": top})
        side_a = []
        handles = [top]
        for _ in range(self.rng.randint(3, 8)):
            roll = self.rng.random()
            if roll < 0.4:
                handle = self.fresh("f")
                side_a.append({"op": "folder_create", "parent": self.pick(handles),
                               "name": f"c{self.rng.randrange(1000)}", "handle": handle})
                handles.append(handle)
            elif roll < 0.6:
                side_a.append({"op": "folder_rename", "folder": self.pick(handles[1:] or [top]),
                               "name": f"r{self.rng.randrange(1000)}"})
            else:
                side_a.append({"op": "import", "paths": [self.new_src_file()],
                               "folder": self.pick(handles)})
        side_b = []
        root = Path(self.model.mounts[mount].root)
        for _ in range(self.rng.randint(2, 5)):
            side_b.append({"op": "disk_add", "path": self.rel(root / self.name()),
                           "content": self.content()})
        side_b.append({"op": "sync", "mount": mount})
        self.pending.append({"op": "concurrent_round", "a": side_a, "b": side_b})
        return self.pending.pop(0)


def weights_for(areas: list[str] | None, overrides: dict[str, float], mode: str) -> dict:
    weights = {}
    for kind, (weight, area) in WEIGHTS.items():
        if areas and area not in areas and kind != "sync":
            continue
        weights[kind] = overrides.get(kind, weight)
    if mode == "concurrent":
        weights = {"concurrent_round": 4, "mount": 1, "sync": 1, "folder_create": 1,
                   **{k: v for k, v in overrides.items()}}
    for kind, weight in overrides.items():
        weights[kind] = weight
    return weights
