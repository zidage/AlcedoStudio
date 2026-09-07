//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "decoders/processor/raw_demosaic_method.hpp"
#include "edit/runtime/develop_compile_source.hpp"

namespace alcedo {

inline constexpr std::size_t kConservativeDevelopBytesPerPixel       = 16;
inline constexpr std::size_t kConservativeNeuralDevelopBytesPerPixel = 24;
inline constexpr std::size_t kConservativeNeuralTileScratchBytes     = 64ull << 20;

inline auto ConservativeDevelopInitialBytes(const DevelopCompileSource& source) -> std::size_t {
  if (source.kind == DevelopInputKind::DirectRgb) {
    return 0;
  }
  const auto width  = static_cast<std::size_t>(source.host_extent.width);
  const auto height = static_cast<std::size_t>(source.host_extent.height);
  if (width == 0 || height == 0) {
    return 0;
  }
  const auto max = (std::numeric_limits<std::size_t>::max)();
  if (height > max / width) {
    return max;
  }
  const auto pixels = width * height;
  if (pixels > max / kConservativeDevelopBytesPerPixel) {
    return max;
  }
  return pixels * kConservativeDevelopBytesPerPixel;
}

inline auto ConservativeDevelopInitialBytes(const DevelopCompileSource& source,
                                            RawDemosaicMethod demosaic_method) -> std::size_t {
  if (demosaic_method != RawDemosaicMethod::NeuralEngine) {
    return ConservativeDevelopInitialBytes(source);
  }
  if (source.kind == DevelopInputKind::DirectRgb) {
    return 0;
  }
  const auto width  = static_cast<std::size_t>(source.host_extent.width);
  const auto height = static_cast<std::size_t>(source.host_extent.height);
  if (width == 0 || height == 0) {
    return 0;
  }
  const auto max = (std::numeric_limits<std::size_t>::max)();
  if (height > max / width) {
    return max;
  }
  const auto pixels = width * height;
  if (pixels > max / kConservativeNeuralDevelopBytesPerPixel) {
    return max;
  }
  const auto plane_bytes = pixels * kConservativeNeuralDevelopBytesPerPixel;
  if (plane_bytes > max - kConservativeNeuralTileScratchBytes) {
    return max;
  }
  return plane_bytes + kConservativeNeuralTileScratchBytes;
}

inline auto DescribeDevelopTransientFailure(const DevelopCompileSource& source,
                                             std::string_view method, bool highlights_reconstruct,
                                             std::string_view what) -> std::string {
  std::string message = "SensorDevelop transients";
  message += " extent=";
  message += std::to_string(source.host_extent.width);
  message += "x";
  message += std::to_string(source.host_extent.height);
  message += " kind=";
  message += std::to_string(static_cast<unsigned>(source.kind));
  message += " method=";
  message += method;
  message += " hlr=";
  message += highlights_reconstruct ? "1" : "0";
  message += ": ";
  message += what;
  return message;
}

}  // namespace alcedo
