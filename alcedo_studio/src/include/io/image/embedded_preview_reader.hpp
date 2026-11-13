//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <filesystem>
#include <opencv2/core/mat.hpp>
#include <optional>

namespace alcedo {

/// Pixel format of the preview stored in a RAW file.
enum class EmbeddedPreviewFormat : uint8_t {
  kJpeg,
  kRgb8Bitmap,
};

/**
 * @brief The camera-rendered preview image stored inside a RAW file, decoded to RGB8.
 *
 * `rgb8` is CV_8UC3 in R, G, B channel order and upright: the RAW file's orientation (the LibRaw
 * flip of the image) is applied, so its extent matches the oriented RAW image. A JPEG preview is
 * decoded with TurboJPEG DCT scaling, so `rgb8` can be smaller than the stored preview.
 */
struct EmbeddedPreview {
  cv::Mat               rgb8;
  EmbeddedPreviewFormat format           = EmbeddedPreviewFormat::kJpeg;
  /// Width and height of the stored preview before scaling and orientation.
  uint32_t              stored_width     = 0;
  uint32_t              stored_height    = 0;
  /// LibRaw flip code of the RAW image that was applied: 0, 3 (180 degrees), 5 (90 degrees
  /// counterclockwise), or 6 (90 degrees clockwise).
  int                   orientation_flip = 0;
};

/**
 * @brief Reads the preview image that a camera or a RAW converter stored inside a RAW file.
 *
 * Reading a preview opens the file with LibRaw, parses the metadata, and reads only the preview
 * bytes: it does not unpack the sensor data. For a 24 MP file this reads a few MB instead of the
 * whole file, and a JPEG preview decodes at 1/2, 1/4, or 1/8 scale without a full-size decode.
 *
 * Thread: every call uses its own LibRaw processor; calls on different threads run in parallel.
 */
class EmbeddedPreviewReader {
 public:
  /**
   * @brief Read the preview of @p raw_path at the smallest decode scale whose shorter edge is at
   * least @p min_short_edge pixels.
   *
   * The preview is LibRaw's main preview of the file (the largest one for the supported cameras).
   * When that image has one sample per pixel and the file stores a colour image too, the preview is
   * the largest colour image: a DNG merged from several frames stores a full-size transparency mask
   * that LibRaw would pick.
   *
   * @return std::nullopt when the file has no preview whose shorter edge is at least
   *         @p min_short_edge, when its preview has another format than JPEG or 8-bit RGB, or when
   *         LibRaw does not read the file as a RAW image (JPEG, PNG, and other raster files).
   * @throws std::runtime_error when the file cannot be opened or read, or when the preview data is
   *         corrupt.
   */
  [[nodiscard]] static auto Read(const std::filesystem::path& raw_path, uint32_t min_short_edge)
      -> std::optional<EmbeddedPreview>;
};

}  // namespace alcedo
