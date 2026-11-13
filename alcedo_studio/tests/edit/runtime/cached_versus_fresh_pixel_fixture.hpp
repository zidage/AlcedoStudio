//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

/// Deterministic ACEScc RGB fixture for later cached-versus-fresh Grade/LLF
/// pixel checks. Independent expected values use the same exposure formula as
/// production Grade tests (`ApplyExposureAcescc`). A cache hit must match both a
/// fresh execution of the same edit and this independent expected buffer.
///
/// Comparison rule (fixed before later execution, not tuned to a failing run):
/// - working space: ACEScc samples stored as linear-ish float RGB in [0, 1]
///   ramps (same layout as `gpu_dag_test::MakeF32RgbaPlane`);
/// - metric: maximum absolute error on R, G, and B; alpha is ignored;
/// - absolute tolerance: 1.0e-5f (matches existing Grade `EXPECT_NEAR` uses of
///   `ApplyExposureAcescc`);
/// - relative tolerance: unused; absolute error is the pass rule;
/// - non-finite handling: any NaN/Inf in either buffer fails the comparison.

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "multi_grade_runtime_test_support.hpp"

namespace alcedo::cached_result_pixel {

struct RgbaF32 {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;
};

struct PixelCompareRule {
  static constexpr float kMaxAbsTolerance = 1.0e-5f;
};

struct PixelCompareResult {
  bool  passed              = false;
  float max_abs_error       = 0.0f;
  bool  saw_non_finite      = false;
  bool  size_mismatch       = false;
};

inline constexpr std::uint32_t kFixtureWidth  = 16;
inline constexpr std::uint32_t kFixtureHeight = 12;

/// Same sample formula as `gpu_dag_test::MakeF32RgbaPlane` so later GPU paths
/// can reuse that packed plane without a second identity.
inline auto SampleAt(std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height)
    -> RgbaF32 {
  return RgbaF32{(static_cast<float>(x) + 0.5f) / static_cast<float>(width),
                 (static_cast<float>(y) + 0.5f) / static_cast<float>(height), 0.25f, 1.0f};
}

inline auto MakeDeterministicRgbRamp(std::uint32_t width = kFixtureWidth,
                                     std::uint32_t height = kFixtureHeight) -> std::vector<RgbaF32> {
  std::vector<RgbaF32> pixels(static_cast<std::size_t>(width) * height);
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      pixels[static_cast<std::size_t>(y) * width + x] = SampleAt(x, y, width, height);
    }
  }
  return pixels;
}

inline auto ApplyIndependentExposure(const std::vector<RgbaF32>& input, float exposure_ev)
    -> std::vector<RgbaF32> {
  std::vector<RgbaF32> output = input;
  for (auto& pixel : output) {
    pixel.r = multi_grade_test::ApplyExposureAcescc(pixel.r, exposure_ev);
    pixel.g = multi_grade_test::ApplyExposureAcescc(pixel.g, exposure_ev);
    pixel.b = multi_grade_test::ApplyExposureAcescc(pixel.b, exposure_ev);
  }
  return output;
}

inline auto CompareRgba(const std::vector<RgbaF32>& left, const std::vector<RgbaF32>& right,
                        float tolerance = PixelCompareRule::kMaxAbsTolerance) -> PixelCompareResult {
  PixelCompareResult result;
  if (left.size() != right.size()) {
    result.size_mismatch = true;
    result.max_abs_error = std::numeric_limits<float>::infinity();
    return result;
  }
  float max_abs = 0.0f;
  for (std::size_t i = 0; i < left.size(); ++i) {
    const float samples[6] = {left[i].r,  left[i].g,  left[i].b,
                              right[i].r, right[i].g, right[i].b};
    for (float sample : samples) {
      if (!std::isfinite(sample)) {
        result.saw_non_finite = true;
        result.max_abs_error  = std::numeric_limits<float>::infinity();
        return result;
      }
    }
    max_abs = std::max(max_abs, std::abs(left[i].r - right[i].r));
    max_abs = std::max(max_abs, std::abs(left[i].g - right[i].g));
    max_abs = std::max(max_abs, std::abs(left[i].b - right[i].b));
  }
  result.max_abs_error = max_abs;
  result.passed        = max_abs <= tolerance;
  return result;
}

}  // namespace alcedo::cached_result_pixel
