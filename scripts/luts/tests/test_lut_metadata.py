"""Tests for the ALCEDO_LUT CUBE metadata comment."""

from __future__ import annotations

import unittest

from lut_test_support import NUMERIC_TABLE, film_metadata, plain_cube

from alcedo_lut_metadata import (LutMetadataError, canonical_file_name, canonical_lut_id,
                                 format_metadata_line, insert_metadata_comment, read_cube_header,
                                 validate_metadata)


class LutMetadataTest(unittest.TestCase):
    def test_CubeCommentPreservesNumericLutRows(self) -> None:
        for newline in (b"\n", b"\r\n"):
            original = plain_cube().replace(b"\n", newline)
            annotated = insert_metadata_comment(original, film_metadata())
            first_line, separator, remainder = annotated.partition(newline)
            self.assertEqual(separator, newline)
            self.assertTrue(first_line.startswith(b"# ALCEDO_LUT {"))
            self.assertEqual(remainder, original)
            self.assertTrue(annotated.endswith(NUMERIC_TABLE.replace(b"\n", newline)))
            self.assertEqual(read_cube_header(annotated).metadata, film_metadata())

    def test_MetadataClassifiesUserFilmWithPrint(self) -> None:
        metadata = film_metadata(
            id="my_lab:kodak_portra_400:fujifilm_crystal_archive", origin="user",
            source={"id": "my_lab", "name": "My Lab"},
            film={"id": "kodak_portra_400", "name": "Portra 400", "brand": "Kodak"},
            print={"id": "fujifilm_crystal_archive", "name": "Crystal Archive",
                   "brand": "Fujifilm", "kind": "paper"},
            aliases=["portra"], future_field={"ignored": True})
        header = read_cube_header(format_metadata_line(metadata).encode() + b"\n" + plain_cube())
        self.assertEqual(header.metadata["origin"], "user")
        self.assertEqual(header.metadata["print"]["kind"], "paper")
        self.assertEqual(header.lut_3d_size, 2)
        self.assertEqual(header.title, "t")

    def test_MetadataRejectsDuplicateAndOversizedComments(self) -> None:
        line = format_metadata_line(film_metadata()).encode()
        with self.assertRaisesRegex(LutMetadataError, "more than one"):
            read_cube_header(line + b"\n" + line + b"\n" + plain_cube())
        with self.assertRaisesRegex(LutMetadataError, "already has"):
            insert_metadata_comment(line + b"\n" + plain_cube(), film_metadata())
        with self.assertRaisesRegex(LutMetadataError, "exceeds"):
            format_metadata_line(film_metadata(description="a" * 1024,
                                               aliases=["b" * 128] * 16,
                                               display_requirement="c" * 512) |
                                 {"extra": "d" * 16000})
        with self.assertRaisesRegex(LutMetadataError, "header exceeds"):
            read_cube_header(b"#" * (64 * 1024 + 8) + b"\n" + NUMERIC_TABLE)

    def test_MalformedMetadataIsAnErrorNotAGeneralLut(self) -> None:
        cases = [
            b"# ALCEDO_LUT {not json",
            b'# ALCEDO_LUT {"schema":2}',
            format_metadata_line(film_metadata()).replace('"film"', '"filmx"').encode(),
            format_metadata_line(film_metadata()).replace(
                '"kind":"film"', '"kind":"slide"').encode(),
        ]
        for line in cases:
            with self.subTest(line=line[:60]):
                with self.assertRaises(LutMetadataError):
                    read_cube_header(line + b"\n" + plain_cube())
        errors = validate_metadata(film_metadata(category="general"))
        self.assertTrue(any("not allowed on a general" in error for error in errors), errors)

    def test_OfficialIdentityAndFileNameFollowMetadata(self) -> None:
        metadata = film_metadata(variant="soft")
        self.assertEqual(canonical_lut_id(metadata),
                         "spectral_film_lut:kodak_vision3_250d_5207:kodak_vision_2383:soft")
        self.assertEqual(canonical_file_name(metadata),
                         "kodak_vision3_250d_5207__kodak_vision_2383__soft.cube")
        self.assertTrue(canonical_file_name(metadata).startswith(metadata["film"]["id"]))

    def test_UnannotatedCubeHasNoMetadata(self) -> None:
        header = read_cube_header(b"\xef\xbb\xbf" + plain_cube())
        self.assertIsNone(header.metadata)
        self.assertEqual(header.lut_3d_size, 2)


if __name__ == "__main__":
    unittest.main()
