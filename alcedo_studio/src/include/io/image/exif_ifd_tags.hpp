//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace alcedo {

/// One entry of a TIFF/EXIF IFD as stored in the file: tag number, TIFF field type (1..12), the
/// number of values, and the value bytes in little-endian byte order.
struct ExifIfdTag {
  uint16_t             tag_   = 0;
  uint16_t             type_  = 0;
  uint32_t             count_ = 0;
  std::vector<uint8_t> value_;
};

/// The entries of IFD0, of the EXIF IFD (IFD0 tag 0x8769), and of the GPS IFD (IFD0 tag 0x8825).
/// Each list is sorted by tag number.
struct ExifIfdEntries {
  std::vector<ExifIfdTag> ifd0_;
  std::vector<ExifIfdTag> exif_;
  std::vector<ExifIfdTag> gps_;
};

/// Size in bytes of one value of TIFF field type @p type, or 0 for a type outside 1..12.
auto ExifTiffTypeSize(uint16_t type) -> uint32_t;

/// True when @p tag is a TIFF field whose value size matches its type and count.
auto IsWritableExifIfdTag(const ExifIfdTag& tag) -> bool;

/// True when the IFD0 tag @p tag describes the stored image data of its file (dimensions,
/// sample layout, strips, tiles, compression, resolution, sub-IFDs, color data) or points to
/// another IFD. Such entries belong to the file that holds them and are never copied.
auto IsExifImageStructureTag(uint16_t tag) -> bool;

/**
 * @brief Read the entries of IFD0, the EXIF IFD, and the GPS IFD of an image file.
 *
 * Reads any container that Exiv2 reads (TIFF-based RAW files, JPEG, DNG, CR3, HEIF, ...). The
 * entries are returned sorted by tag number with their file types and values unchanged. The IFD
 * pointer entries are not returned.
 *
 * @return The entries; empty lists when Exiv2 does not know the container.
 * @throws std::runtime_error when the file cannot be read or its metadata is invalid.
 */
auto ReadExifIfdEntries(const std::filesystem::path& image_path) -> ExifIfdEntries;

/// The GPS IFD entries of ReadExifIfdEntries.
auto ReadExifGpsTags(const std::filesystem::path& image_path) -> std::vector<ExifIfdTag>;

/**
 * @brief Select the source entries that an exported file carries.
 *
 * Removes the entries that describe the source file and not the exported pixels (image
 * structure, Orientation, pixel dimensions, resolution, thumbnails, MakerNote, DNG and TIFF/EP
 * data, embedded XMP/IPTC/ICC/Photoshop blocks), the entries that the export writes itself
 * (Software, rating, ExifVersion, FlashpixVersion), the GPS IFD unless @p include_location, and
 * the body and lens serial numbers unless @p include_device_serials.
 */
auto SelectExportExifIfdEntries(const ExifIfdEntries& source, bool include_location,
                                bool include_device_serials) -> ExifIfdEntries;

/**
 * @brief Write @p entries into the IFD0, EXIF IFD, and GPS IFD of a little-endian classic TIFF.
 *
 * OpenImageIO writes no GPS IFD and only the EXIF tags that it knows into TIFF files. This
 * appends a new EXIF IFD, a new GPS IFD, and a new IFD0 at the end of the file, then points the
 * TIFF header to the new IFD0. Each new IFD holds the entries of the old IFD, with the entries of
 * @p entries that have the same tag replacing them. Entries of @p entries.ifd0_ for which
 * IsExifImageStructureTag is true are ignored. The pixel data and the other IFDs stay unchanged.
 * Does nothing when all lists of @p entries are empty.
 *
 * @throws std::runtime_error when the file is not a little-endian classic TIFF or cannot be
 *         read or written.
 */
void WriteTiffExifIfdEntries(const std::filesystem::path& tiff_path, const ExifIfdEntries& entries);

/**
 * @brief Replace the eXIf chunk of a PNG file with @p exif_tiff_block.
 *
 * OpenImageIO encodes the eXIf chunk again from its attributes, which drops the EXIF tags that it
 * does not know and changes some values. This removes all eXIf chunks and writes one with
 * @p exif_tiff_block (a TIFF header and its IFDs) before the first IDAT chunk. Does nothing when
 * @p exif_tiff_block is empty.
 *
 * @throws std::runtime_error when the file is not a PNG file or cannot be read or written.
 */
void WritePngExifChunk(const std::filesystem::path& png_path,
                       const std::vector<uint8_t>&  exif_tiff_block);

}  // namespace alcedo
