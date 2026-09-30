"""Tests for the packaged-application LUT payload inspection."""

from __future__ import annotations

import contextlib
import io
from pathlib import Path
import unittest
import zipfile

import py7zr

from lut_test_support import TemporaryDirectory

import inspect_application_payloads as payloads


def write(path: Path, data: bytes = b"x") -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


class PackagedApplicationContainsNoLutPayloadsTest(unittest.TestCase):
    def run_main(self, *arguments: str) -> tuple[int, str]:
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            status = payloads.main(list(arguments))
        return status, output.getvalue()

    def test_install_tree_without_luts_passes(self) -> None:
        with TemporaryDirectory() as temp:
            root = Path(temp) / "install"
            write(root / "bin" / "alcedo_main.exe")
            write(root / "bin" / "config" / "icc" / "display.icc")
            status, output = self.run_main(str(root))
            self.assertEqual(status, 0)
            self.assertIn("NO LUT PAYLOADS", output)

    def test_nested_cube_in_any_case_is_reported(self) -> None:
        with TemporaryDirectory() as temp:
            root = Path(temp) / "Alcedo Studio.app"
            write(root / "Contents" / "MacOS" / "LUTs" / "Kodak_5207.CUBE")
            write(root / "Contents" / "MacOS" / "alcedo_main")
            self.assertEqual(payloads.lut_payloads(root),
                             ["Contents/MacOS/LUTs/Kodak_5207.CUBE"])
            status, output = self.run_main(str(root))
            self.assertEqual(status, 1)
            self.assertIn("Contents/MacOS/LUTs/Kodak_5207.CUBE", output)

    def test_update_archives_are_listed_without_extraction(self) -> None:
        with TemporaryDirectory() as temp:
            clean_zip = Path(temp) / "clean.zip"
            with zipfile.ZipFile(clean_zip, "w") as archive:
                archive.writestr("Alcedo Studio.app/Contents/MacOS/alcedo_main", b"x")
            bundled_7z = Path(temp) / "bundled.7z"
            source = write(Path(temp) / "look.cube", b"LUT_3D_SIZE 2\n")
            with py7zr.SevenZipFile(bundled_7z, "w") as archive:
                archive.write(source, "bin/LUTs/look.cube")
            self.assertEqual(payloads.lut_payloads(clean_zip), [])
            self.assertEqual(payloads.lut_payloads(bundled_7z), ["bin/LUTs/look.cube"])
            status, _ = self.run_main(str(clean_zip), str(bundled_7z))
            self.assertEqual(status, 1)

    def test_unreadable_artifact_is_not_reported_as_clean(self) -> None:
        with TemporaryDirectory() as temp:
            broken = write(Path(temp) / "update.zip", b"not a zip")
            status, output = self.run_main(str(broken), str(Path(temp) / "absent"))
            self.assertEqual(status, 2)
            self.assertNotIn("NO LUT PAYLOADS", output)


if __name__ == "__main__":
    unittest.main()
