//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <exiv2/exif.hpp>
#include <exiv2/exiv2.hpp>
#include <json.hpp>
#include <stdexcept>

#include "decoders/processor/raw_color_context.hpp"
#include "image.hpp"
#include "type/type.hpp"
#include "utils/import/import_error_code.hpp"

// Forward declare LibRaw so callers that only need the header do not pull in libraw.h
class LibRaw;

namespace alcedo {

class MetadataExtractionError : public std::runtime_error {
 public:
  MetadataExtractionError(ImportErrorCode code, image_path_t path, std::string message,
                          uint16_t nef_compression = 0)
      : std::runtime_error(message),
        code_(code),
        path_(std::move(path)),
        message_(std::move(message)),
        nef_compression_(nef_compression) {}

  [[nodiscard]] auto code() const -> ImportErrorCode { return code_; }
  [[nodiscard]] auto path() const -> const image_path_t& { return path_; }
  [[nodiscard]] auto message() const -> const std::string& { return message_; }
  [[nodiscard]] auto nef_compression() const -> uint16_t { return nef_compression_; }

 private:
  ImportErrorCode code_ = ImportErrorCode::UNKNOWN;
  image_path_t    path_{};
  std::string     message_{};
  uint16_t        nef_compression_ = 0;
};

class MetadataExtractor {
 public:
  /**
   * @brief Extract EXIF metadata from image file
   *
   * @param image_path
   * @return Exiv2::Image::UniquePtr
   */
  static auto ExtractEXIF(const image_path_t& image_path) -> Exiv2::Image::UniquePtr;
  static auto ExtractEXIFFromBuffer(const uint8_t* buffer, size_t size) -> Exiv2::Image::UniquePtr;

  /**
   * @brief Convert EXIF data to JSON format
   *
   * @param exif_data
   * @return nlohmann::json
   */
  static auto EXIFToJSON(const Exiv2::Image::UniquePtr& exif_data) -> nlohmann::json;

  /**
   * @brief Convert EXIF data to display-friendly format
   *
   * @param exif_data
   * @return ExifDisplayMetaData
   */
  static auto EXIFToDisplayMetaData(const Exiv2::Image::UniquePtr& exif_data)
      -> ExifDisplayMetaData;
  static auto BufferToDisplayMetaData(const uint8_t* buffer, size_t size) -> ExifDisplayMetaData;

  /**
   * @brief Detect Ultra HDR / HDR gain-map / Rec.2100 markers for album badges.
   *
   * Prefer the open Exiv2 image when available so XMP gain-map tags are checked
   * without a second file open; the path is still used for OIIO/signature probes.
   */
  static auto DetectHdrDisplayMetadata(const image_path_t& image_path,
                                       const Exiv2::Image* exif_image = nullptr) -> bool;

  /**
   * @brief Extract EXIF metadata and populate the Image object
   *
   * @param image_path
   * @param image
   */
  static void ExtractEXIF_ToImage(const image_path_t& image_path, Image& image);

  /**
   * @brief Extract metadata from a raw file using libraw and populate the Image with
   *        both display metadata (ExifDisplayMetaData) and raw color context
   *        (RawRuntimeColorContext). This replaces the Exiv2-based extraction for raw
   *        files, providing better lens ID resolution (e.g. Nikon) and all data
   *        needed for color temperature and lens calibration operators.
   *
   * @param image_path Path to the raw image file
   * @param image      Image object to populate
   * @return true if extraction succeeded
   */
  static auto ExtractRawMetadata_ToImage(const image_path_t& image_path, Image& image) -> bool;

  /**
   * @brief Populate a RawRuntimeColorContext from an already-opened LibRaw instance.
   *        Requires open_file + unpack to have been called.  This is the single source
   *        of truth for raw metadata extraction (camera/lens strings, color matrices,
   *        Nikon lens ID lookup, etc.).
   *
   * @param raw_processor  A LibRaw instance after open_file + unpack
   * @param ctx            Output context to populate
   */
  static void PopulateRuntimeContextFromOpenLibRaw(LibRaw&                 raw_processor,
                                                   RawRuntimeColorContext& ctx);

  /**
   * @brief Merge an Exiv2-based ExifDisplayMetaData hint into a RawRuntimeColorContext,
   *        filling in any fields that are still empty/zero.
   *
   * @param metadata_hint  The Exiv2-based metadata (may be nullptr — no-op in that case)
   * @param ctx            Context to merge into
   */
  static void MergeMetadataHint(const ExifDisplayMetaData* metadata_hint,
                                RawRuntimeColorContext&    ctx);
};
}  // namespace alcedo
