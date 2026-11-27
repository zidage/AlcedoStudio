#  Copyright 2026 Yurun Zi
#  SPDX-License-Identifier: GPL-3.0-only
#  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
"""Unit tests of the reference model of the file management fuzz (#1192): the fingerprint rule,
the scan prediction of the cases M2 and M3 of #1188, and the folder rules of A and B.

Run from the repository root:  python3 -m unittest discover -s scripts/file_fuzz/tests
"""

from __future__ import annotations

import os
import shutil
import sys
import tempfile
import unittest
import unicodedata
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import fuzz_disk  # noqa: E402
import fuzz_model  # noqa: E402
from fuzz_disk import FINGERPRINT_BLOCK, fingerprint_bytes, jpeg_content  # noqa: E402
from fuzz_model import (INVALID_NAME, MOUNTED_READ_ONLY, NAME_TAKEN, OWN_SUBTREE,  # noqa: E402
                        ROOT, ROOT_CANNOT_CHANGE, UNKNOWN_FOLDER, LibraryModel)


class FingerprintRuleTest(unittest.TestCase):
    def test_small_file_hashes_size_and_all_bytes(self):
        data = b"abc" * 100
        self.assertEqual(fingerprint_bytes(data), fingerprint_bytes(bytes(data)))
        self.assertNotEqual(fingerprint_bytes(data), fingerprint_bytes(data + b"d"))
        self.assertNotEqual(fingerprint_bytes(data), fingerprint_bytes(b"x" + data[1:]))

    def test_file_of_two_blocks_counts_every_byte_once(self):
        data = bytearray(os.urandom(2 * FINGERPRINT_BLOCK))
        before = fingerprint_bytes(bytes(data))
        data[FINGERPRINT_BLOCK] ^= 1
        self.assertNotEqual(before, fingerprint_bytes(bytes(data)))

    def test_change_in_the_middle_of_a_large_file_keeps_the_fingerprint(self):
        data = bytearray(os.urandom(3 * FINGERPRINT_BLOCK))
        before = fingerprint_bytes(bytes(data))
        data[FINGERPRINT_BLOCK + 10] ^= 1
        self.assertEqual(before, fingerprint_bytes(bytes(data)))
        data[5] ^= 1
        self.assertNotEqual(before, fingerprint_bytes(bytes(data)))

    def test_change_in_the_last_block_changes_the_fingerprint(self):
        data = bytearray(os.urandom(3 * FINGERPRINT_BLOCK))
        before = fingerprint_bytes(bytes(data))
        data[-3] ^= 1
        self.assertNotEqual(before, fingerprint_bytes(bytes(data)))

    def test_read_of_a_file_equals_the_rule_over_its_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            for size in (10, 2 * FINGERPRINT_BLOCK, 2 * FINGERPRINT_BLOCK + 1, 400000):
                path = Path(directory) / f"f{size}"
                data = os.urandom(size)
                path.write_bytes(data)
                self.assertEqual(fuzz_disk.read_fingerprint(path), fingerprint_bytes(data))

    def test_generated_jpeg_has_the_size_and_its_variants_change_one_region(self):
        size = 400 * 1024
        base = jpeg_content(7, size)
        self.assertEqual(len(base), size)
        self.assertEqual(base[:2], b"\xff\xd8")
        self.assertIn(b"\xff\xd9", base)
        head, middle, tail = (jpeg_content(7, size, head=1), jpeg_content(7, size, middle=1),
                              jpeg_content(7, size, tail=1))
        self.assertNotEqual(fingerprint_bytes(base), fingerprint_bytes(head))
        self.assertEqual(fingerprint_bytes(base), fingerprint_bytes(middle))
        self.assertNotEqual(base, middle)
        self.assertNotEqual(fingerprint_bytes(base), fingerprint_bytes(tail))
        self.assertEqual(len(jpeg_content(3, 900)), 900)


class ListingRuleTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def test_listing_keeps_supported_files_and_skips_period_names(self):
        for name in ("a.jpg", "b.JPEG", "c.arw", "d.txt", ".e.jpg", "f.jpg.part", "g.x3f"):
            (self.root / name).write_bytes(b"x")
        (self.root / ".dot").mkdir()
        (self.root / ".dot" / "h.jpg").write_bytes(b"x")
        (self.root / "empty").mkdir()
        listing = fuzz_disk.list_mount_directory(self.root)
        self.assertEqual([f.rel for f in listing.files], ["a.jpg", "b.JPEG", "c.arw", "g.x3f"])
        self.assertEqual(listing.directories, ["empty"])

    def test_listing_gives_names_in_nfc(self):
        name = unicodedata.normalize("NFD", "café.jpg")
        (self.root / name).write_bytes(b"x")
        listing = fuzz_disk.list_mount_directory(self.root)
        self.assertEqual([f.rel for f in listing.files], ["café.jpg"])

    def test_missing_root_is_not_a_complete_listing(self):
        self.assertFalse(fuzz_disk.list_mount_directory(self.root / "gone").complete)


class ScanPredictionTest(unittest.TestCase):
    """Each test mounts a directory in the model, applies the first scan, changes the disk, and
    checks the counts of the next scan and the model after it."""

    def setUp(self):
        self.base = Path(tempfile.mkdtemp())
        self.root = self.base / "mnt"
        self.root.mkdir()
        self.model = LibraryModel()
        self.next_eid = 100
        self.seed = 0

    def tearDown(self):
        shutil.rmtree(self.base, ignore_errors=True)

    def write(self, rel: str, size: int = 3000, seed: int | None = None) -> Path:
        path = self.root.joinpath(*rel.split("/"))
        path.parent.mkdir(parents=True, exist_ok=True)
        self.seed += 1
        path.write_bytes(jpeg_content(self.seed if seed is None else seed, size))
        return path

    def bind(self) -> None:
        for photo in self.model.photos.values():
            if photo.eid is None:
                self.next_eid += 1
                photo.eid = self.next_eid

    def mount(self) -> fuzz_model.Mount:
        mount = self.model.add_mount(self.root, 1, 1, "m1")
        prediction = self.model.predict_scan("m1")
        prediction.apply()
        self.bind()
        return mount

    def scan(self, fingerprint=None) -> dict:
        prediction = self.model.predict_scan("m1", fingerprint)
        prediction.apply()
        self.bind()
        counts = prediction.counts()
        del counts["status"], counts["not_imported"]
        return counts

    def photo_at(self, rel: str) -> fuzz_model.Photo:
        return next(p for p in self.model.photos.values() if p.rel == rel)

    def folders(self) -> list[str]:
        return sorted(f.rel for f in self.model.folders.values() if f.mount == "m1" and f.rel)

    @staticmethod
    def counts(new=0, moved=0, changed=0, missing=0, returned=0) -> dict:
        return {"new": new, "moved": moved, "changed": changed, "missing": missing,
                "returned": returned}

    def test_first_scan_imports_each_listed_file_and_adds_folders_with_files(self):
        self.write("a.jpg")
        self.write("sub/b.jpg")
        (self.root / "empty").mkdir()
        (self.root / "notes.txt").write_text("x")
        prediction = self.model.predict_scan(self.model.add_mount(self.root, 1, 1, "m1").handle)
        self.assertEqual(prediction.new, 2)
        prediction.apply()
        self.assertEqual(self.folders(), ["sub"])

    def test_added_file_is_new(self):
        self.write("a.jpg")
        self.mount()
        self.write("b.jpg")
        self.assertEqual(self.scan(), self.counts(new=1))

    def test_deleted_file_is_missing_and_returns_with_the_same_photo(self):
        path = self.write("a.jpg")
        self.mount()
        handle = self.photo_at("a.jpg").handle
        data, info = path.read_bytes(), path.stat()
        path.unlink()
        self.assertEqual(self.scan(), self.counts(missing=1))
        self.assertEqual(self.model.photos[handle].state, "missing")
        path.write_bytes(data)
        os.utime(path, ns=(info.st_atime_ns, info.st_mtime_ns))
        self.assertEqual(self.scan(), self.counts(returned=1))
        self.assertEqual(self.model.photos[handle].state, "present")

    def test_returned_file_with_a_new_time_is_returned(self):
        path = self.write("a.jpg")
        self.mount()
        data = path.read_bytes()
        path.unlink()
        self.scan()
        path.write_bytes(data)
        self.assertEqual(self.scan(), self.counts(returned=1))

    def test_renamed_file_keeps_its_photo(self):
        path = self.write("a.jpg")
        self.mount()
        handle = self.photo_at("a.jpg").handle
        os.rename(path, self.root / "b.jpg")
        self.assertEqual(self.scan(), self.counts(moved=1))
        self.assertEqual(self.model.photos[handle].rel, "b.jpg")

    def test_case_only_rename_is_a_move(self):
        path = self.write("a.jpg")
        self.mount()
        os.rename(path, self.root / "A.JPG")
        self.assertEqual(self.scan(), self.counts(moved=1))
        self.assertEqual(self.photo_at("A.JPG").handle, next(iter(self.model.photos)))

    def test_file_moved_to_a_subdirectory_keeps_its_photo_in_a_new_folder(self):
        path = self.write("a.jpg")
        self.mount()
        (self.root / "sub").mkdir()
        os.rename(path, self.root / "sub" / "a.jpg")
        self.assertEqual(self.scan(), self.counts(moved=1))
        photo = self.photo_at("sub/a.jpg")
        self.assertEqual(self.model.folders[photo.home].rel, "sub")

    def test_renamed_subdirectory_moves_its_files_and_replaces_its_folder(self):
        self.write("old/a.jpg")
        self.write("old/b.jpg")
        self.mount()
        os.rename(self.root / "old", self.root / "new")
        self.assertEqual(self.scan(), self.counts(moved=2))
        self.assertEqual(self.folders(), ["new"])

    def test_same_name_with_new_content_is_changed_in_place(self):
        path = self.write("a.jpg", size=3000)
        self.mount()
        handle = self.photo_at("a.jpg").handle
        path.write_bytes(jpeg_content(999, 4000))
        self.assertEqual(self.scan(), self.counts(changed=1))
        self.assertEqual(self.model.photos[handle].size, 4000)

    def test_change_of_only_the_time_is_a_change_in_place(self):
        # BuildMountScanChangeList: a different size or time is a change in place; the
        # fingerprint stays.
        path = self.write("a.jpg")
        self.mount()
        before = self.photo_at("a.jpg").fingerprint
        info = path.stat()
        os.utime(path, ns=(info.st_atime_ns, info.st_mtime_ns + 60 * 10 ** 9))
        self.assertEqual(self.scan(), self.counts(changed=1))
        self.assertEqual(self.photo_at("a.jpg").fingerprint, before)

    def test_file_replaced_by_a_directory_is_missing_and_its_file_is_new(self):
        path = self.write("a.jpg")
        self.mount()
        path.unlink()
        path.mkdir()
        self.write("a.jpg/inner.jpg", size=5000)
        self.assertEqual(self.scan(), self.counts(new=1, missing=1))
        self.assertIn("a.jpg", self.folders())

    def test_missing_photo_keeps_its_folder(self):
        path = self.write("sub/a.jpg")
        self.mount()
        path.unlink()
        self.scan()
        self.assertEqual(self.folders(), ["sub"])

    def test_two_moved_files_with_one_content_pair_by_name_then_by_order(self):
        first = self.write("a.jpg", seed=5)
        self.mount()
        handle = self.photo_at("a.jpg").handle
        shutil.copyfile(first, self.root / "twin.jpg")
        (self.root / "x").mkdir()
        os.rename(first, self.root / "x" / "z.jpg")
        os.rename(self.root / "twin.jpg", self.root / "x" / "a.jpg")
        # One lost record, two new paths with its fingerprint: the path with its file name.
        self.assertEqual(self.scan(), self.counts(new=1, moved=1))
        self.assertEqual(self.model.photos[handle].rel, "x/a.jpg")

    def test_lost_records_with_one_content_take_the_sorted_paths(self):
        self.write("p.jpg", seed=8)
        shutil.copyfile(self.root / "p.jpg", self.root / "q.jpg")
        self.mount()
        p, q = self.photo_at("p.jpg").handle, self.photo_at("q.jpg").handle
        os.rename(self.root / "p.jpg", self.root / "m2.jpg")
        os.rename(self.root / "q.jpg", self.root / "m1.jpg")
        self.assertEqual(self.scan(), self.counts(moved=2))
        self.assertEqual(self.model.photos[p].rel, "m1.jpg")
        self.assertEqual(self.model.photos[q].rel, "m2.jpg")

    def test_move_and_change_in_one_step_is_missing_and_new(self):
        path = self.write("a.jpg", size=3000)
        self.mount()
        os.rename(path, self.root / "b.jpg")
        (self.root / "b.jpg").write_bytes(jpeg_content(77, 3000))
        self.assertEqual(self.scan(), self.counts(new=1, missing=1))

    def test_move_with_a_change_in_the_middle_of_a_large_file_is_a_move(self):
        size = 400 * 1024
        path = self.root / "big.jpg"
        path.write_bytes(jpeg_content(11, size))
        self.mount()
        os.rename(path, self.root / "moved.jpg")
        (self.root / "moved.jpg").write_bytes(jpeg_content(11, size, middle=3))
        self.assertEqual(self.scan(), self.counts(moved=1))

    def test_hard_link_is_a_second_photo(self):
        path = self.write("a.jpg")
        self.mount()
        try:
            os.link(path, self.root / "b.jpg")
        except OSError:
            self.skipTest("no hard link on this file system")
        self.assertEqual(self.scan(), self.counts(new=1))

    def test_ordinary_photo_below_the_root_is_adopted(self):
        path = self.write("a.jpg")
        photo = self.model.add_photo(str(path), ROOT)
        photo.eid = 7
        self.model.add_mount(self.root, 1, 1, "m1")
        prediction = self.model.predict_scan("m1")
        self.assertEqual(prediction.new, 1)
        prediction.apply()
        self.assertEqual(len(self.model.photos), 1)
        self.assertEqual(photo.mount, "m1")

    def test_unreadable_file_is_neither_new_nor_changed(self):
        self.write("a.jpg")
        self.mount()
        path = self.write("b.jpg")

        def locked(file):
            return None if file.path == path else fuzz_disk.read_fingerprint(file.path)
        self.assertEqual(self.scan(locked), self.counts())
        self.assertEqual(self.scan(), self.counts(new=1))

    def test_records_below_an_unreadable_directory_stay_present(self):
        self.write("sub/a.jpg")
        self.mount()
        listing = fuzz_disk.list_mount_directory(self.root)
        listing.files = []
        listing.unreadable_directories = ["sub"]
        prediction = self.model.predict_scan("m1", listing=listing)
        self.assertEqual(prediction.missing, 0)
        prediction.apply()
        self.assertEqual(self.folders(), ["sub"])

    def test_unreachable_root_is_offline_and_changes_nothing(self):
        self.write("a.jpg")
        self.mount()
        os.rename(self.root, self.base / "away")
        prediction = self.model.predict_scan("m1")
        self.assertEqual(prediction.status, "offline")
        prediction.apply()
        self.assertEqual(self.photo_at("a.jpg").state, "present")
        self.assertEqual(self.model.mounts["m1"].state, "offline")

    def test_batch_limits_import_every_new_file(self):
        self.mount()
        for index in range(501):
            (self.root / f"n{index:04d}.jpg").write_bytes(jpeg_content(index, 900))
        self.assertEqual(self.scan(), self.counts(new=501))


class FolderRuleTest(unittest.TestCase):
    def setUp(self):
        self.model = LibraryModel()
        self.model.apply_create("fa", ROOT, "A", 1)
        self.model.apply_create("fb", "fa", "B", 2)

    def test_trim_follows_qstring_trimmed(self):
        self.assertEqual(fuzz_model.qt_trimmed("  x \t"), "x")
        self.assertEqual(fuzz_model.qt_trimmed("　y　"), "y")
        self.assertEqual(fuzz_model.qt_trimmed("\x1cz"), "\x1cz")

    def test_names_that_are_empty_dots_slashes_or_control_characters_are_invalid(self):
        for name in ("", "  ", ".", "..", "a/b", "a\\b", "x\x01y", "x\x7fy", " . "):
            self.assertEqual(self.model.expect_create(ROOT, name), {INVALID_NAME}, repr(name))
        for name in ("dot.", "a:b", "@x", "\U0001F4F7", "a" * 4096, "café"):
            self.assertEqual(self.model.expect_create(ROOT, name), {"ok"}, repr(name))

    def test_taken_name_compares_exactly(self):
        self.assertEqual(self.model.expect_create(ROOT, " A "), {NAME_TAKEN})
        self.assertEqual(self.model.expect_create(ROOT, "a"), {"ok"})
        self.assertEqual(self.model.expect_create("fb", "A"), {"ok"})
        self.model.apply_create("fc", ROOT, "café", 3)
        self.assertEqual(self.model.expect_create(ROOT, "café"), {"ok"})

    def test_rename_to_the_same_name_is_accepted_and_to_a_sibling_name_is_taken(self):
        self.model.apply_create("fc", ROOT, "C", 3)
        self.assertEqual(self.model.expect_rename("fa", "A"), {"ok"})
        self.assertEqual(self.model.expect_rename("fa", "C"), {NAME_TAKEN})
        self.assertEqual(self.model.expect_rename("fa", "a"), {"ok"})
        self.assertEqual(self.model.expect_rename(ROOT, "x"), {ROOT_CANNOT_CHANGE})

    def test_move_into_itself_or_a_descendant_is_refused(self):
        self.assertEqual(self.model.expect_move("fa", "fa"), {OWN_SUBTREE})
        self.assertEqual(self.model.expect_move("fa", "fb"), {OWN_SUBTREE})
        self.assertEqual(self.model.expect_move("fb", "fa"), {"ok"})
        self.assertEqual(self.model.expect_move("fb", ROOT), {"ok"})
        self.assertEqual(self.model.expect_move(ROOT, "fa"), {ROOT_CANNOT_CHANGE})
        self.assertEqual(self.model.expect_move(None, ROOT), {UNKNOWN_FOLDER})

    def test_move_to_a_parent_with_the_name_is_taken(self):
        self.model.apply_create("fb2", ROOT, "B", 4)
        self.assertEqual(self.model.expect_move("fb", ROOT), {NAME_TAKEN})

    def test_delete_takes_the_child_folders_and_the_links(self):
        photo = self.model.add_photo("x.jpg", "fb")
        self.model.apply_delete_folder("fa")
        self.assertEqual(set(self.model.folders), {ROOT})
        self.assertEqual(photo.links, set())
        self.assertIn(photo.handle, self.model.photos)

    def test_mounted_folders_refuse_structure_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            mount = self.model.add_mount(Path(directory) / "A", 9, 9, "m1")
            self.assertEqual(self.model.folders[mount.folder].name, "A@")
            self.assertIn(MOUNTED_READ_ONLY, self.model.expect_create(mount.folder, "x"))
            self.assertIn(MOUNTED_READ_ONLY, self.model.expect_rename(mount.folder, "y"))
            self.assertIn(MOUNTED_READ_ONLY, self.model.expect_move("fa", mount.folder))
            self.assertEqual(self.model.expect_delete_folder(mount.folder), {MOUNTED_READ_ONLY})


if __name__ == "__main__":
    unittest.main()
