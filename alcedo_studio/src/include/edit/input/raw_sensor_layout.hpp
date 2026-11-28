//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <filesystem>

namespace alcedo {

/// The color filter layout of a RAW file, as the loader develops it.
enum class RawSensorLayout : std::uint8_t {
  /// A classic 2x2 Bayer mosaic: the loader gives a Bayer CFA input (AI Denoise can run).
  Bayer,
  /// The 6x6 X-Trans mosaic of Fujifilm.
  XTrans,
  /// Any other file: monochrome, decoded RGB (linear DNG), a four-color or Foveon sensor.
  Other,
};

/**
 * @brief Read the sensor layout of the RAW file at @p path from its LibRaw metadata.
 *
 * Opens the file with LibRaw and reads the CFA description; it does not unpack the sensor data.
 * The editor calls it on a worker thread for the AI Denoise switch (integration plan, DN7).
 * @throws std::runtime_error when LibRaw cannot open the file.
 */
[[nodiscard]] auto ReadRawSensorLayout(const std::filesystem::path& path) -> RawSensorLayout;

}  // namespace alcedo
