//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include "decoders/processor/raw_color_context.hpp"
#include "decoders/processor/raw_linearization_params.hpp"
#include "decoders/processor/raw_processor_pattern.hpp"
#include "edit/geometry/types.hpp"
#include "edit/runtime/develop_compile_source.hpp"

namespace alcedo {

/// Bumped when LibRaw unpack, CFA downsample, or active-area mapping rules change.
inline constexpr std::uint32_t kRawInputPreparationVersion = 1;

/**
 * @brief FNV-1a 64-bit hash of opaque bytes. Used for encoded-source identity, not pixels.
 */
[[nodiscard]] inline auto HashContentBytes(std::span<const std::byte> bytes) -> std::uint64_t {
  std::uint64_t hash = 14695981039346656037ull;
  for (const auto byte : bytes) {
    hash ^= static_cast<std::uint8_t>(byte);
    hash *= 1099511628211ull;
  }
  return hash;
}

enum class HostPixelFormat : std::uint8_t {
  U16Cfa  = 0,
  F32Rgba = 1,
};

enum class SceneWorkingSpace : std::uint8_t {
  CameraRgb = 0,
};

/**
 * @brief Host pixel plane for a prepared develop input. Shared bytes, tightly packed rows.
 */
struct HostImagePlane {
  std::shared_ptr<const std::byte> bytes;
  Extent2D                         extent{};
  std::uint32_t                    stride_bytes = 0;
  HostPixelFormat                  format       = HostPixelFormat::U16Cfa;

  [[nodiscard]] auto ByteCount() const -> std::size_t {
    return static_cast<std::size_t>(stride_bytes) * extent.height;
  }

  [[nodiscard]] auto Span() const -> std::span<const std::byte> {
    if (!bytes || ByteCount() == 0) {
      return {};
    }
    return {bytes.get(), ByteCount()};
  }
};

/**
 * @brief LibRaw size fields needed to crop after demosaic. No LibRaw type.
 */
struct RawSensorGeometry {
  std::int32_t  raw_width      = 0;
  std::int32_t  raw_height     = 0;
  std::int32_t  width          = 0;
  std::int32_t  height         = 0;
  std::int32_t  left_margin    = 0;
  std::int32_t  top_margin     = 0;
  std::uint16_t default_crop[4] = {};
  std::int32_t  orientation_flip = 0;
};

/**
 * @brief Pre-unpack lookup for PreparedSourceCache. Quality is not part of this identity.
 *
 * DecodeRes is stored as downsample pass count so preview/export frames that share the same
 * downsample policy reuse one host result.
 */
struct PreparedSourceLookup {
  std::uint64_t encoded_content_hash  = 0;
  std::uint64_t encoded_byte_count    = 0;
  std::uint8_t  downsample_passes     = 0;
  std::uint32_t preparation_version   = kRawInputPreparationVersion;
};

inline auto operator==(const PreparedSourceLookup& a, const PreparedSourceLookup& b) -> bool {
  return a.encoded_content_hash == b.encoded_content_hash &&
         a.encoded_byte_count == b.encoded_byte_count &&
         a.downsample_passes == b.downsample_passes &&
         a.preparation_version == b.preparation_version;
}

inline auto operator<(const PreparedSourceLookup& a, const PreparedSourceLookup& b) -> bool {
  if (a.encoded_content_hash != b.encoded_content_hash) {
    return a.encoded_content_hash < b.encoded_content_hash;
  }
  if (a.encoded_byte_count != b.encoded_byte_count) {
    return a.encoded_byte_count < b.encoded_byte_count;
  }
  if (a.downsample_passes != b.downsample_passes) {
    return a.downsample_passes < b.downsample_passes;
  }
  return a.preparation_version < b.preparation_version;
}

/**
 * @brief Content identity of a prepared host source. Viewport ROI is not included.
 */
struct PreparedSourceKey {
  std::uint64_t encoded_content_hash = 0;
  std::uint64_t encoded_byte_count   = 0;
  RawInputKind  input_kind           = RawInputKind::BayerRaw;
  std::uint64_t cfa_hash             = 0;
  std::uint8_t  downsample_passes    = 0;
  RectI         sensor_active_area{};
  std::int32_t  orientation_flip     = 0;
  std::uint32_t preparation_version  = kRawInputPreparationVersion;

  [[nodiscard]] auto Lookup() const -> PreparedSourceLookup {
    return PreparedSourceLookup{encoded_content_hash, encoded_byte_count, downsample_passes,
                                preparation_version};
  }
};

inline auto operator==(const PreparedSourceKey& a, const PreparedSourceKey& b) -> bool {
  return a.encoded_content_hash == b.encoded_content_hash &&
         a.encoded_byte_count == b.encoded_byte_count && a.input_kind == b.input_kind &&
         a.cfa_hash == b.cfa_hash && a.downsample_passes == b.downsample_passes &&
         a.sensor_active_area == b.sensor_active_area &&
         a.orientation_flip == b.orientation_flip &&
         a.preparation_version == b.preparation_version;
}

/**
 * @brief CPU-side develop input. GPU work starts at upload.
 *
 * Output of Develop is camera scene-linear RGB. Camera-to-AP1 is a later node.
 */
struct PreparedRawInput {
  HostImagePlane           pixels;
  Extent2D                 host_extent{};
  Extent2D                 develop_output_extent{};
  Extent2D                 full_reference_extent{};
  RectI                    sensor_active_area{};
  RectI                    demosaic_output_crop{};
  std::uint8_t             downsample_passes = 0;
  RawInputKind             input_kind        = RawInputKind::BayerRaw;
  RawCfaPattern            cfa_pattern{};
  RawLinearizationParams   linearization{};
  RawSensorGeometry        sensor{};
  RawRuntimeColorContext   color_context{};
  SourceContentKey         content_key{};
  PreparedSourceKey        source_key{};
  SceneWorkingSpace        working_space = SceneWorkingSpace::CameraRgb;

  [[nodiscard]] auto CompileSource() const -> DevelopCompileSource;
};

}  // namespace alcedo
