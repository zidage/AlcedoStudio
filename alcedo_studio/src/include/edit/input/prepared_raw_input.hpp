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
  SceneWorkingSpace        working_space = SceneWorkingSpace::CameraRgb;

  [[nodiscard]] auto CompileSource() const -> DevelopCompileSource;
};

}  // namespace alcedo
