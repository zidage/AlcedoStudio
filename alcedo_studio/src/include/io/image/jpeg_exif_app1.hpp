//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

#include "image/metadata.hpp"
#include "io/image/exif_ifd_tags.hpp"
#include "io/image/export_color_profile_config.hpp"

namespace alcedo {

struct ExportExifOptions {
  std::optional<ExifDisplayMetaData> metadata_;
  int                                width_            = 0;
  int                                height_           = 0;
  bool                               include_software_ = true;
  /// Source entries from SelectExportExifIfdEntries. They are written unchanged, except where
  /// the export writes its own value for the same tag.
  ExifIfdEntries                     source_entries_;
  /// EXIF ColorSpace of the exported pixels (ExifColorSpaceFor). Empty keeps the source value.
  std::optional<uint16_t>            color_space_;
  /// Resolution in dots per inch for the JPEG IFD0. 0 writes the EXIF default of 72.
  double                             resolution_dpi_ = 0.0;
};

/// EXIF ColorSpace value (0xA001) of pixels encoded with @p profile: 1 (sRGB) for Rec.709
/// primaries with the sRGB or gamma 2.2 curve, 0xFFFF (uncalibrated) for all other encodings.
auto ExifColorSpaceFor(const ExportColorProfileConfig& profile) -> uint16_t;

/**
 * @brief The IFD0, EXIF IFD, and GPS IFD entries of an exported file.
 *
 * Starts from `source_entries_` and sets the values that the export owns: Orientation 1,
 * Software, ExifVersion 0232, FlashpixVersion 0100, ColorSpace, PixelXDimension and
 * PixelYDimension, the current rating, and the current capture time (DateTime,
 * DateTimeOriginal, DateTimeDigitized). The other `metadata_` values (Make, Model, lens,
 * exposure) are written only where the source has no value. Has no image structure entries.
 */
auto BuildExportExifIfdEntries(const ExportExifOptions& options) -> ExifIfdEntries;

/// Build a JPEG APP1 Exif payload (`Exif\0\0` + TIFF IFDs) without Exiv2: the entries of
/// BuildExportExifIfdEntries plus the JPEG IFD0 fields (resolution, YCbCrPositioning) and
/// ComponentsConfiguration. When the payload is larger than the 65533
/// bytes of an APP1 segment, the largest source entries are left out until it fits.
auto BuildJpegExifApp1Payload(const ExportExifOptions& options) -> std::vector<uint8_t>;

/// Build a TIFF block (little-endian TIFF header, IFD0, and the EXIF and GPS IFDs) of
/// @p entries for decoders that take EXIF data, such as OpenImageIO `decode_exif`. Image
/// structure entries of IFD0 are left out. Empty when @p entries has no entries.
auto BuildExifTiffBlock(const ExifIfdEntries& entries) -> std::vector<uint8_t>;

/// Replace or insert the APP1 Exif segment in an existing JPEG file.
auto ReplaceJpegExifApp1Segment(const std::filesystem::path& path,
                                const std::vector<uint8_t>&  exif_payload) -> bool;

/// Build and write APP1 Exif for an exported JPEG (SDR or Ultra HDR).
auto ApplyJpegExifApp1Metadata(const std::filesystem::path& path, const ExportExifOptions& options)
    -> bool;

}  // namespace alcedo
