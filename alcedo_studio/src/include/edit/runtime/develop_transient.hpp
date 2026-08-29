//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <tuple>

#include "decoders/processor/raw_demosaic_method.hpp"
#include "edit/runtime/develop_compile_source.hpp"

namespace alcedo {

inline constexpr std::size_t kConservativeDevelopBytesPerPixel       = 16;
inline constexpr std::size_t kConservativeNeuralDevelopBytesPerPixel = 24;
inline constexpr std::size_t kConservativeNeuralTileScratchBytes     = 64ull << 20;

struct DevelopTransientLayoutKey {
  DevelopInputKind  kind                         = DevelopInputKind::BayerCfa;
  std::uint32_t     host_width                    = 0;
  std::uint32_t     host_height                   = 0;
  std::uint8_t      downsample_passes             = 0;
  RawDemosaicMethod demosaic_method               = RawDemosaicMethod::Legacy;
  std::uint32_t     backend_capability_version    = 0;

  friend auto operator<(const DevelopTransientLayoutKey& a, const DevelopTransientLayoutKey& b)
      -> bool {
    return std::tie(a.kind, a.host_width, a.host_height, a.downsample_passes, a.demosaic_method,
                    a.backend_capability_version) <
           std::tie(b.kind, b.host_width, b.host_height, b.downsample_passes, b.demosaic_method,
                    b.backend_capability_version);
  }
};

inline auto MakeDevelopTransientLayoutKey(const DevelopCompileSource& source,
                                          std::uint32_t backend_capability_version,
                                          RawDemosaicMethod demosaic_method)
    -> DevelopTransientLayoutKey {
  return DevelopTransientLayoutKey{
      .kind                        = source.kind,
      .host_width                  = source.host_extent.width,
      .host_height                 = source.host_extent.height,
      .downsample_passes           = source.downsample_passes,
      .demosaic_method             = demosaic_method,
      .backend_capability_version = backend_capability_version,
  };
}

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

inline auto ApplyDevelopTransientSafetyMargin(std::size_t observed_capacity) -> std::size_t {
  if (observed_capacity == 0) {
    return 0;
  }
  const auto margin = observed_capacity / 20;
  const auto max     = (std::numeric_limits<std::size_t>::max)();
  if (observed_capacity > max - margin) {
    return max;
  }
  return observed_capacity + margin;
}

/**
 * @brief Last observed Develop transient capacity keyed by source layout.
 *
 * Used as the next exclusive-stage Reserve, not as a compiler plane ledger.
 */
class DevelopTransientHighWaterCache {
 public:
  [[nodiscard]] auto SuggestInitial(const DevelopCompileSource& source,
                                    std::uint32_t backend_capability_version,
                                    RawDemosaicMethod demosaic_method) const -> std::size_t {
    const auto key = MakeDevelopTransientLayoutKey(source, backend_capability_version,
                                                   demosaic_method);
    const auto it  = observed_capacity_.find(key);
    if (it != observed_capacity_.end()) {
      return ApplyDevelopTransientSafetyMargin(it->second);
    }
    return ConservativeDevelopInitialBytes(source, demosaic_method);
  }

  void Record(const DevelopCompileSource& source, std::uint32_t backend_capability_version,
              RawDemosaicMethod demosaic_method, std::size_t capacity) {
    observed_capacity_[MakeDevelopTransientLayoutKey(source, backend_capability_version,
                                                     demosaic_method)] = capacity;
  }

  [[nodiscard]] auto ObservedCapacity(const DevelopCompileSource& source,
                                       std::uint32_t backend_capability_version,
                                       RawDemosaicMethod demosaic_method) const -> std::size_t {
    const auto it = observed_capacity_.find(
        MakeDevelopTransientLayoutKey(source, backend_capability_version, demosaic_method));
    return it == observed_capacity_.end() ? 0 : it->second;
  }

 private:
  std::map<DevelopTransientLayoutKey, std::size_t> observed_capacity_;
};

}  // namespace alcedo
