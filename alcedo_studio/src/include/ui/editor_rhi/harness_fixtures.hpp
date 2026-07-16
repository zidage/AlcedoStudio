//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace alcedo::editor_rhi {

enum class HarnessFixtureKind {
  Fp32Gradient,
  Checkerboard,
  RoiPatch,
  OddSized,
};

struct Rgba32fPixel {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;
};

struct HarnessFixtureImage {
  int                      width     = 0;
  int                      height    = 0;
  std::vector<Rgba32fPixel> pixels{};  // row-major, width * height

  [[nodiscard]] auto row_bytes() const -> std::size_t {
    return static_cast<std::size_t>(width) * sizeof(Rgba32fPixel);
  }
  [[nodiscard]] auto byte_size() const -> std::size_t {
    return row_bytes() * static_cast<std::size_t>(height);
  }
  [[nodiscard]] auto data() const -> const float* {
    return pixels.empty() ? nullptr : &pixels.front().r;
  }
  [[nodiscard]] auto data() -> float* {
    return pixels.empty() ? nullptr : &pixels.front().r;
  }
};

// Deterministic generators (no RNG). Coordinates are pixel centers in [0, w) x [0, h).
[[nodiscard]] auto MakeFp32Gradient(int width, int height) -> HarnessFixtureImage;
[[nodiscard]] auto MakeCheckerboard(int width, int height, int cell_size = 8)
    -> HarnessFixtureImage;
// Fills background with gradient; ROI rectangle with solid color.
[[nodiscard]] auto MakeRoiPatch(int width, int height, int roi_x, int roi_y, int roi_w, int roi_h,
                                Rgba32fPixel roi_color = {1.0f, 0.25f, 0.0f, 1.0f})
    -> HarnessFixtureImage;
// Odd dimensions (e.g. 63x47) for alignment edge cases.
[[nodiscard]] auto MakeOddSized(int width = 63, int height = 47) -> HarnessFixtureImage;

[[nodiscard]] auto MakeFixture(HarnessFixtureKind kind) -> HarnessFixtureImage;
[[nodiscard]] auto ParseHarnessFixtureKind(std::string_view token)
    -> std::optional<HarnessFixtureKind>;
[[nodiscard]] auto ToString(HarnessFixtureKind kind) -> const char*;

// Documented floating-point tolerance for harness readback comparison.
// Linear RGBA32F after native interop + sample; allows small GPU filter/precision drift.
inline constexpr float kHarnessPixelAbsTolerance = 2.0e-3f;

// Returns max abs channel error, or negative if dimensions mismatch.
[[nodiscard]] auto MaxAbsPixelError(const HarnessFixtureImage& expected,
                                    const float* actual_rgba, int actual_width, int actual_height,
                                    std::size_t actual_row_bytes) -> float;

// Repository-relative path to a small real RAW used by later e2e phases.
// Phase 0 only records the path contract; it does not require decoding here.
[[nodiscard]] auto SmallRealRawFixtureRelativePath() -> std::string;

}  // namespace alcedo::editor_rhi
