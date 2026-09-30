"""Tests for package inventories, archives, feed signing, and the publication plan."""

from __future__ import annotations

import contextlib
import io
import json
from pathlib import Path
import subprocess
import unittest

import py7zr

from lut_test_support import FIXTURE_DIR, TemporaryDirectory, film_metadata, plain_cube

from alcedo_lut_metadata import canonical_file_name, format_metadata_line
from lut_inventory import (LutInventoryError, LutRecord, build_package_inventory,
                           compute_inventory_digest, serialize_inventory)
from lut_package_archive import verify_package_archive
import prepare_lut_packages
import publish_lut_packages


def write_official_lut(root: Path, metadata: dict, title: str = "t") -> Path:
    path = root / canonical_file_name(metadata)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(format_metadata_line(metadata).encode() + b"\n" + plain_cube(title))
    return path


def film(source: str, film_id: str, print_id: str | None = None) -> dict:
    metadata = film_metadata(source={"id": source, "name": source},
                             film={"id": film_id, "name": film_id, "brand": "Kodak"})
    if print_id is None:
        metadata.pop("print")
    else:
        metadata["print"] = {"id": print_id, "name": print_id, "brand": "Kodak", "kind": "film"}
    metadata["id"] = ":".join([source, film_id] + ([print_id] if print_id else []))
    return metadata


def populate_package(root: Path, source: str) -> None:
    write_official_lut(root, film(source, "kodak_vision3_250d"))
    write_official_lut(root, film(source, "kodak_vision3_250d", "kodak_2383"))
    write_official_lut(root, film(source, "kodak_portra_400", "kodak_endura"))
    (root / "LICENSE.txt").write_text("license", encoding="utf-8")


def find_signer() -> Path | None:
    signer = prepare_lut_packages.default_signer()
    return signer if signer.is_file() else None


def run_quietly(function, argv: list[str]) -> tuple[int, str]:
    with contextlib.redirect_stdout(io.StringIO()) as printed:
        code = function(argv)
    return code, printed.getvalue()


class LutInventoryTest(unittest.TestCase):
    def test_PythonAndCppInventoryDigestsMatch(self) -> None:
        fixture = json.loads((FIXTURE_DIR / "inventory_digest_records.json")
                             .read_text(encoding="utf-8"))
        records = [LutRecord(**item) for item in fixture["records"]]
        expected = fixture["expected_inventory_sha256"]
        self.assertEqual(compute_inventory_digest(records), expected)
        self.assertEqual(compute_inventory_digest(list(reversed(records))), expected)
        self.assertEqual(compute_inventory_digest(records[1:] + records[:1]), expected)

    def test_InventoryDigestRejectsDuplicateIdsAndCollidingPaths(self) -> None:
        digest = "a" * 64
        for records in ([LutRecord("a", "a.cube", 1, digest), LutRecord("a", "b.cube", 1, digest)],
                        [LutRecord("a", "A.cube", 1, digest), LutRecord("b", "a.cube", 1, digest)],
                        [LutRecord("a", "../a.cube", 1, digest)],
                        [LutRecord("a", "dir\\a.cube", 1, digest)],
                        [LutRecord("a\nb", "a.cube", 1, digest)]):
            with self.assertRaises(LutInventoryError):
                compute_inventory_digest(records)

    def test_PackageInventoryCountsLutsAndHashesAuxiliaryFiles(self) -> None:
        with TemporaryDirectory() as root:
            populate_package(root, "spectral_film_lut")
            inventory = build_package_inventory(root, "spectral_film_lut", "r1", workers=4)
            self.assertEqual(inventory["file_count"], 3)
            self.assertEqual([item["path"] for item in inventory["auxiliary_files"]],
                             ["LICENSE.txt"])
            total = sum(path.stat().st_size for path in root.iterdir())
            self.assertEqual(inventory["unpacked_bytes"], total)
            self.assertEqual(build_package_inventory(root, "spectral_film_lut", "r1", workers=1),
                             inventory)

    def test_CppFixtureMatchesPythonPackageInventory(self) -> None:
        # LutMetadataTest.PythonPackageInventoryParsesInCpp parses this exact file.
        with TemporaryDirectory() as root:
            populate_package(root, "spectral_film_lut")
            inventory = build_package_inventory(root, "spectral_film_lut", "r1")
        self.assertEqual(serialize_inventory(inventory),
                         (FIXTURE_DIR / "package_inventory_from_python.json").read_bytes())

    def test_PackageRejectsNonCanonicalOrUserLuts(self) -> None:
        base = film("spectral_film_lut", "kodak_vision3_250d")
        cases = {
            "renamed": lambda root: write_official_lut(root, base).rename(root / "x.cube"),
            "user origin": lambda root: write_official_lut(root, base | {"origin": "user"}),
            "wrong id": lambda root: write_official_lut(root, base | {"id": "spectral_film_lut:x"}),
            "other source": lambda root: write_official_lut(
                root, film("spektrafilm_lut", "kodak_vision3_250d")),
            "unannotated": lambda root: (root / "plain.cube").write_bytes(plain_cube()),
            "unexpected file": lambda root: (root / "tool.exe").write_bytes(b"MZ"),
        }
        for name, populate in cases.items():
            with self.subTest(case=name), TemporaryDirectory() as root:
                populate(root)
                with self.assertRaises(LutInventoryError):
                    build_package_inventory(root, "spectral_film_lut", "r1")


class LutPackagePreparationTest(unittest.TestCase):
    def prepare(self, work: Path, *extra: str) -> tuple[int, Path]:
        for source in ("spectral_film_lut", "spektrafilm_lut"):
            source_dir = work / "in" / source
            if not source_dir.exists():
                populate_package(source_dir, source)
        output = work / "out"
        code, _ = run_quietly(prepare_lut_packages.main, [
            "--package", f"spectral_film_lut={work / 'in' / 'spectral_film_lut'}",
            "--package", f"spektrafilm_lut={work / 'in' / 'spektrafilm_lut'}",
            "--revision", "2026.09.1", "--sequence", "42", "--output-dir", str(output), *extra])
        return code, output

    def test_TwoPackagesValidateIndependentlyFromLocalFiles(self) -> None:
        with TemporaryDirectory() as work:
            code, output = self.prepare(work)
            self.assertEqual(code, 0)
            manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(manifest["kind"], "alcedo-lut-packages")
            self.assertNotIn("expiresAt", manifest)
            self.assertEqual([item["id"] for item in manifest["packages"]],
                             ["spectral_film_lut", "spektrafilm_lut"])
            self.assertFalse((output / "manifest.json.sig").exists())
            for package in manifest["packages"]:
                archive = output / "packages" / Path(package["artifact"]["url"]).name
                self.assertTrue(package["artifact"]["url"].startswith(
                    "https://static.aoraw.org/luts/v1/packages/"))
                inventory = verify_package_archive(archive, package, work / "verify")
                self.assertEqual(inventory["package_id"], package["id"])
                self.assertEqual(package["file_count"], 3)
                # The feed name is the source name its film simulations declare.
                self.assertEqual(package["name"], package["id"])

    def test_ChangedArchiveBytesFailVerification(self) -> None:
        with TemporaryDirectory() as work:
            code, output = self.prepare(work)
            self.assertEqual(code, 0)
            manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
            package = manifest["packages"][0]
            archive = output / "packages" / Path(package["artifact"]["url"]).name
            forged_dir = work / "forged"
            with py7zr.SevenZipFile(archive, "r") as source:
                source.extractall(path=forged_dir)
            lut = next(forged_dir.glob("*.cube"))
            lut.write_bytes(lut.read_bytes().replace(b"0.5 0.25 1", b"0.5 0.25 0.9"))
            forged = work / "forged.7z"
            with py7zr.SevenZipFile(forged, "w") as target:
                for path in sorted(forged_dir.rglob("*")):
                    target.write(path, path.relative_to(forged_dir).as_posix())
            with self.assertRaises(LutInventoryError):
                verify_package_archive(forged, package, work / "verify")

    def test_InvalidPackageStopsPublicationWithoutManifest(self) -> None:
        with TemporaryDirectory() as work:
            (work / "in" / "spektrafilm_lut").mkdir(parents=True)
            (work / "in" / "spektrafilm_lut" / "plain.cube").write_bytes(plain_cube())
            code, output = self.prepare(work)
            self.assertEqual(code, 1)
            self.assertFalse((output / "manifest.json").exists())

    @unittest.skipIf(find_signer() is None, "alcedo_update_signer is not built")
    def test_SignedFeedVerifiesAndPublicationPlanListsLiveManifestLast(self) -> None:
        with TemporaryDirectory() as work:
            signer = find_signer()
            private_key = work / "key.seed"
            public_key = work / "key.pub"
            subprocess.run([str(signer), "generate", "--private-key", str(private_key),
                            "--public-key", str(public_key)], check=True, capture_output=True)
            code, output = self.prepare(work, "--private-key", str(private_key),
                                        "--public-key-file", str(public_key))
            self.assertEqual(code, 0)
            self.assertTrue((output / "manifest.json.sig").is_file())

            publish_args = ["--output-dir", str(output), "--public-key-file", str(public_key)]
            code, printed = run_quietly(publish_lut_packages.main, publish_args)
            self.assertEqual(code, 0)
            self.assertIn("Nothing was published", printed)
            manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
            keys = [item["key"] for item in
                    publish_lut_packages.planned_uploads(output, manifest, "luts/v1")]
            self.assertEqual(keys, [
                "luts/v1/packages/spectral_film_lut/2026.09.1/spectral_film_lut-2026.09.1.7z",
                "luts/v1/packages/spektrafilm_lut/2026.09.1/spektrafilm_lut-2026.09.1.7z",
                "luts/v1/manifests/42/manifest.json.sig",
                "luts/v1/manifests/42/manifest.json",
                "luts/v1/manifest.json.sig",
                "luts/v1/manifest.json",
            ])

            # A mismatched signature and manifest pair is rejected, never published unsigned.
            manifest_path = output / "manifest.json"
            manifest_path.write_bytes(
                manifest_path.read_bytes().replace(b'"sequence": 42', b'"sequence": 43'))
            with self.assertRaises(RuntimeError):
                run_quietly(publish_lut_packages.main, publish_args)
            (output / "manifest.json.sig").unlink()
            with self.assertRaises(SystemExit):
                run_quietly(publish_lut_packages.main, publish_args)


if __name__ == "__main__":
    unittest.main()
