//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "decoders/processor/raw_processor_pattern.hpp"
#include "edit/input/prepared_raw_input.hpp"

namespace alcedo {
namespace gpu_dag_test {

inline auto MakeRggbPattern() -> RawCfaPattern {
  RawCfaPattern pattern;
  pattern.kind = RawCfaKind::Bayer2x2;
  return pattern;
}

inline auto MakeXTransPattern() -> RawCfaPattern {
  static constexpr int kRawFc[36] = {
      1, 2, 1, 1, 0, 1, 1, 0, 1, 2, 1, 2, 0, 1, 0, 1, 2, 1,
      1, 2, 1, 1, 0, 1, 1, 0, 1, 2, 1, 2, 2, 1, 2, 0, 1, 0,
  };
  RawCfaPattern pattern;
  pattern.kind = RawCfaKind::XTrans6x6;
  for (int i = 0; i < 36; ++i) {
    pattern.xtrans_pattern.raw_fc[i] = kRawFc[i];
    pattern.xtrans_pattern.rgb_fc[i] = FoldRawColorToRgb(kRawFc[i]);
  }
  return pattern;
}

inline auto DefaultLinearization() -> RawLinearizationParams {
  RawLinearizationParams params;
  for (int c = 0; c < 4; ++c) {
    params.black_level[c] = 0.0f;
    params.white_level[c] = 16383.0f;
  }
  params.cam_mul[0]        = 2.0f;
  params.cam_mul[1]        = 1.0f;
  params.cam_mul[2]        = 1.5f;
  params.cam_mul[3]        = 1.0f;
  params.apply_as_shot_wb  = 1;
  return params;
}

inline auto MakeU16CfaPlane(std::uint32_t width, std::uint32_t height, const RawCfaPattern& pattern)
    -> HostImagePlane {
  HostImagePlane plane;
  plane.extent       = Extent2D{width, height};
  plane.stride_bytes = width * static_cast<std::uint32_t>(sizeof(std::uint16_t));
  plane.format       = HostPixelFormat::U16Cfa;
  const std::size_t bytes = plane.ByteCount();
  auto storage = std::shared_ptr<std::byte>(new std::byte[bytes], [](std::byte* p) { delete[] p; });
  auto* samples = reinterpret_cast<std::uint16_t*>(storage.get());
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const int color = RgbColorAt(pattern, static_cast<int>(y), static_cast<int>(x));
      const std::uint16_t base[3] = {4000, 5000, 3000};
      samples[y * width + x] =
          static_cast<std::uint16_t>(base[color] + ((7 * y + 3 * x) % 17) * 10);
    }
  }
  plane.bytes = std::const_pointer_cast<const std::byte>(storage);
  return plane;
}

inline auto MakeF32RgbaPlane(std::uint32_t width, std::uint32_t height) -> HostImagePlane {
  HostImagePlane plane;
  plane.extent       = Extent2D{width, height};
  plane.stride_bytes = width * 16U;
  plane.format       = HostPixelFormat::F32Rgba;
  const std::size_t bytes = plane.ByteCount();
  auto storage = std::shared_ptr<std::byte>(new std::byte[bytes], [](std::byte* p) { delete[] p; });
  auto* px = reinterpret_cast<float*>(storage.get());
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4;
      px[i + 0] = (static_cast<float>(x) + 0.5f) / static_cast<float>(width);
      px[i + 1] = (static_cast<float>(y) + 0.5f) / static_cast<float>(height);
      px[i + 2] = 0.25f;
      px[i + 3] = 1.0f;
    }
  }
  plane.bytes = std::const_pointer_cast<const std::byte>(storage);
  return plane;
}

inline auto FullSensor(std::uint32_t width, std::uint32_t height) -> RawSensorGeometry {
  RawSensorGeometry sensor;
  sensor.raw_width  = static_cast<std::int32_t>(width);
  sensor.raw_height = static_cast<std::int32_t>(height);
  sensor.width      = sensor.raw_width;
  sensor.height     = sensor.raw_height;
  return sensor;
}

}  // namespace gpu_dag_test
}  // namespace alcedo
