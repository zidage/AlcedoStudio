//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Readers for the header structures of JPEG, PNG, TIFF and OpenEXR files that carry color
// information. They read only headers and tags, never pixel data. Every reader works on the
// complete file content in memory and throws RasterContainerError when a structure that it
// must read is out of bounds or malformed.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace alcedo {

class RasterContainerError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// JPEG markers up to the first SOS.
struct JpegContainerInfo {
  /// ICC profile reassembled from the APP2 `ICC_PROFILE` segments in sequence order. Empty
  /// when the file has none or the segments are incomplete.
  std::vector<std::byte> icc_profile_;
  /// TIFF structure of the APP1 `Exif` segment, without the `Exif\0\0` prefix.
  std::vector<std::byte> exif_tiff_;
  /// Component count of the SOF segment (1 gray, 3 YCbCr/RGB, 4 CMYK/YCCK).
  int                    component_count_ = 0;
};

/// @throws RasterContainerError when the SOI marker or a segment length is invalid.
auto ReadJpegContainer(std::span<const std::byte> file_bytes) -> JpegContainerInfo;

/// PNG chunks before the first IDAT.
struct PngContainerInfo {
  uint8_t                                bit_depth_  = 0;
  /// IHDR color type: 0 gray, 2 RGB, 3 palette, 4 gray + alpha, 6 RGBA.
  uint8_t                                color_type_ = 0;
  /// cICP: colour primaries, transfer characteristics, matrix coefficients, full range flag.
  std::optional<std::array<uint8_t, 4>>  cicp_;
  /// Decompressed iCCP profile. Empty when absent or not decompressible.
  std::vector<std::byte>                 icc_profile_;
  bool                                   has_iccp_ = false;
  bool                                   has_srgb_ = false;
  /// gAMA: file gamma times 100000 (the encoding exponent).
  std::optional<uint32_t>                gama_;
  /// cHRM: Wx Wy Rx Ry Gx Gy Bx By, each times 100000.
  std::optional<std::array<uint32_t, 8>> chrm_;
  /// mDCv maximum mastering display luminance in nits.
  std::optional<float>                   mastering_max_luminance_nits_;
  /// cLLI maximum content light level in nits.
  std::optional<float>                   max_content_light_level_nits_;
  /// TIFF structure of the eXIf chunk.
  std::vector<std::byte>                 exif_tiff_;
};

/// @throws RasterContainerError when the signature, IHDR or a chunk length is invalid.
auto ReadPngContainer(std::span<const std::byte> file_bytes) -> PngContainerInfo;

/// Tags of the first TIFF image file directory that describe the pixel layout and color.
struct TiffContainerInfo {
  uint32_t               width_             = 0;
  uint32_t               height_            = 0;
  uint16_t               samples_per_pixel_ = 1;
  uint16_t               bits_per_sample_   = 1;
  /// 1 unsigned integer, 2 signed integer, 3 IEEE floating point.
  uint16_t               sample_format_     = 1;
  /// 0/1 gray, 2 RGB, 3 palette, 5 separated (CMYK), 6 YCbCr, 32803 CFA, 34892 LinearRaw.
  uint16_t               photometric_       = 0;
  /// Orientation tag value 1..8, 1 when absent.
  uint16_t               orientation_       = 1;
  std::vector<std::byte> icc_profile_;
  std::string            make_;
  bool                   has_dng_version_ = false;
};

/// @throws RasterContainerError when the header or the first directory is malformed.
auto ReadTiffContainer(std::span<const std::byte> file_bytes) -> TiffContainerInfo;

/// EXIF color tags read from a TIFF-structured EXIF block.
struct ExifColorTags {
  /// ColorSpace (0xA001): 1 sRGB, 0xFFFF uncalibrated.
  std::optional<uint16_t> color_space_;
  /// InteroperabilityIndex of the interoperability IFD, for example "R98" or "R03".
  std::string             interop_index_;
  /// Orientation of IFD0, 1..8.
  std::optional<uint16_t> orientation_;
};

/// Read the EXIF color tags. A malformed block yields empty values; EXIF is optional data.
auto ReadExifColorTags(std::span<const std::byte> exif_tiff) -> ExifColorTags;

/// Attributes of a single-part OpenEXR header that describe color.
struct ExrHeaderInfo {
  /// `chromaticities`: Rx Ry Gx Gy Bx By Wx Wy.
  std::optional<std::array<float, 8>> chromaticities_;
  /// `acesImageContainerFlag` equal to 1.
  bool                                aces_container_ = false;
};

/// @throws RasterContainerError when the magic number or an attribute is malformed.
auto ReadExrHeader(std::span<const std::byte> file_bytes) -> ExrHeaderInfo;

}  // namespace alcedo
