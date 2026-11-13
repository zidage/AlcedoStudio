//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Shared helpers of the CUDA, OpenCL and Metal raster develop tests
// (raster_image_input_plan.md, Phase R3).

#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raster_input_loader.hpp"
#include "edit/runtime/display_to_ap1_math.h"
#include "edit/runtime/raster_develop_params.hpp"
#include "edit/runtime/raster_linearize_math.h"
#include "image/raster_color_description.hpp"

namespace alcedo::raster_render_test {

struct Rgba {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;
};

inline auto ReadFixture(const std::string& name) -> std::vector<std::byte> {
  std::ifstream in(std::filesystem::path(ALCEDO_RASTER_FIXTURE_DIR) / name, std::ios::binary);
  if (!in) {
    ADD_FAILURE() << "missing raster fixture " << name;
    return {};
  }
  const std::vector<char> chars((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
  std::vector<std::byte>  bytes(chars.size());
  std::memcpy(bytes.data(), chars.data(), chars.size());
  return bytes;
}

/// A decoded raster fixture with the description and default document import would build.
struct RasterCase {
  PreparedRawInput       input;
  RasterColorDescription description;
  PipelineDocument       document;
};

inline auto LoadCase(const std::string& name, RasterFileKind kind) -> RasterCase {
  const auto bytes       = ReadFixture(name);
  auto       description = ResolveRasterColorDescription(bytes, kind);
  auto       input       = RasterInputLoader::LoadEncoded(bytes, DecodeRes::FULL, kind);
  auto       document    = CreateDefaultRasterPipelineDocument(description);
  return RasterCase{std::move(input), std::move(description), std::move(document)};
}

/// Stored code value of channel @p channel of host pixel @p index, as the kernels read it.
inline auto HostCode(const PreparedRawInput& input, std::size_t index, int channel) -> float {
  const auto* base = input.pixels.bytes.get();
  switch (input.pixels.format) {
    case HostPixelFormat::U8Rgba:
      return static_cast<float>(
          std::to_integer<std::uint8_t>(base[index * 4 + static_cast<std::size_t>(channel)]));
    case HostPixelFormat::U16Rgba: {
      std::uint16_t value = 0;
      std::memcpy(&value, base + (index * 4 + static_cast<std::size_t>(channel)) * 2, 2);
      return static_cast<float>(value);
    }
    case HostPixelFormat::F32Rgba: {
      float value = 0.0f;
      std::memcpy(&value, base + (index * 4 + static_cast<std::size_t>(channel)) * 4, 4);
      return value;
    }
    default:
      ADD_FAILURE() << "not a raster plane";
      return 0.0f;
  }
}

/**
 * @brief Expected develop_output (ACEScc AP1) of an unrotated raster input rendered at its own
 * extent: the host evaluation of LinearizeRaster and DisplayToAp1.
 */
inline auto HostReferenceDevelopOutput(const PreparedRawInput&       input,
                                       const RasterColorDescription& description)
    -> std::vector<Rgba> {
  const auto linearize = PackRasterLinearize(description, input.pixels.format);
  const auto block     = ResolveDisplayToAp1Block(description);
  const auto packed    = block.Packed();
  const auto pixels = static_cast<std::size_t>(input.host_extent.width) * input.host_extent.height;
  std::vector<Rgba> out(pixels);
  for (std::size_t i = 0; i < pixels; ++i) {
    const auto linear = RlLinearize(HostCode(input, i, 0), HostCode(input, i, 1),
                                    HostCode(input, i, 2), linearize.data());
    const auto cc     = D2aSourceToAcesccAp1(A2rMake3(linear.r, linear.g, linear.b), packed.data());
    out[i]            = Rgba{cc.x, cc.y, cc.z, 1.0f};
  }
  return out;
}

/// Linear AP1 value of an ACEScc code value (ACES S-2014-003).
inline auto AcesccToLinear(float cc) -> float {
  if (cc < (9.72f - 15.0f) / 17.52f) {
    return (std::exp2(cc * 17.52f - 9.72f) - std::exp2(-16.0f)) * 2.0f;
  }
  return std::exp2(cc * 17.52f - 9.72f);
}

/// True when two ACEScc values match: within @p tolerance in ACEScc, or within 1e-6 in linear
/// light. Below about 2^-15 linear, ACEScc is steep, and a linear difference far under 16-bit
/// quantization becomes a large ACEScc difference.
inline auto AcesccValuesMatch(float actual, float expected, float tolerance) -> bool {
  return std::abs(actual - expected) <= tolerance ||
         std::abs(AcesccToLinear(actual) - AcesccToLinear(expected)) <= 1e-6f;
}

/// Compare two ACEScc images. 3e-4 in ACEScc is 0.4 percent in linear light.
inline void ExpectAcesccNear(const std::vector<Rgba>& actual, const std::vector<Rgba>& expected,
                             const char* label, float tolerance = 3e-4f) {
  ASSERT_EQ(actual.size(), expected.size()) << label;
  int failures = 0;
  for (std::size_t i = 0; i < expected.size(); ++i) {
    const bool match = AcesccValuesMatch(actual[i].r, expected[i].r, tolerance) &&
                       AcesccValuesMatch(actual[i].g, expected[i].g, tolerance) &&
                       AcesccValuesMatch(actual[i].b, expected[i].b, tolerance);
    if (!match && ++failures <= 5) {
      ADD_FAILURE() << label << " pixel " << i << " actual (" << actual[i].r << ", " << actual[i].g
                    << ", " << actual[i].b << ") expected (" << expected[i].r << ", "
                    << expected[i].g << ", " << expected[i].b << ")";
    }
  }
  EXPECT_EQ(failures, 0) << label;
}

inline auto AcesccDecode(float v) -> float {
  constexpr float kA = 9.72f;
  constexpr float kB = 17.52f;
  if (v < (-16.0f + kA) / kB) return v - (-16.0f + kA) / kB;
  if (v <= (-15.0f + kA) / kB) return (std::exp2(v * kB - kA) - 0.0000152587890625f) * 2.0f;
  return std::exp2(v * kB - kA);
}

}  // namespace alcedo::raster_render_test
