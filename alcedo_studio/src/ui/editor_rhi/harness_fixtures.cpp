//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/editor_rhi/harness_fixtures.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace alcedo::editor_rhi {
namespace {

auto Allocate(int width, int height) -> HarnessFixtureImage {
  HarnessFixtureImage image;
  image.width  = std::max(0, width);
  image.height = std::max(0, height);
  if (image.width > 0 && image.height > 0) {
    image.pixels.assign(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height),
                        Rgba32fPixel{});
  }
  return image;
}

}  // namespace

auto MakeFp32Gradient(int width, int height) -> HarnessFixtureImage {
  auto image = Allocate(width, height);
  if (image.pixels.empty()) {
    return image;
  }
  const float inv_x = width > 1 ? 1.0f / static_cast<float>(width - 1) : 0.0f;
  const float inv_y = height > 1 ? 1.0f / static_cast<float>(height - 1) : 0.0f;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      Rgba32fPixel& p = image.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                     static_cast<std::size_t>(x)];
      p.r = static_cast<float>(x) * inv_x;
      p.g = static_cast<float>(y) * inv_y;
      p.b = 0.5f * (p.r + p.g);
      p.a = 1.0f;
    }
  }
  return image;
}

auto MakeCheckerboard(int width, int height, int cell_size) -> HarnessFixtureImage {
  auto image = Allocate(width, height);
  if (image.pixels.empty()) {
    return image;
  }
  const int cell = std::max(1, cell_size);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const bool on = ((x / cell) + (y / cell)) % 2 == 0;
      Rgba32fPixel& p =
          image.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                       static_cast<std::size_t>(x)];
      p.r = on ? 0.9f : 0.1f;
      p.g = on ? 0.9f : 0.1f;
      p.b = on ? 0.9f : 0.1f;
      p.a = 1.0f;
    }
  }
  return image;
}

auto MakeRoiPatch(int width, int height, int roi_x, int roi_y, int roi_w, int roi_h,
                  Rgba32fPixel roi_color) -> HarnessFixtureImage {
  auto image = MakeFp32Gradient(width, height);
  if (image.pixels.empty()) {
    return image;
  }
  const int x0 = std::clamp(roi_x, 0, width);
  const int y0 = std::clamp(roi_y, 0, height);
  const int x1 = std::clamp(roi_x + roi_w, 0, width);
  const int y1 = std::clamp(roi_y + roi_h, 0, height);
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      image.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                   static_cast<std::size_t>(x)] = roi_color;
    }
  }
  return image;
}

auto MakeOddSized(int width, int height) -> HarnessFixtureImage {
  // Force odd dimensions when callers pass defaults or even values.
  const int w = (width % 2 == 0) ? width + 1 : width;
  const int h = (height % 2 == 0) ? height + 1 : height;
  return MakeCheckerboard(w, h, 7);
}

auto MakeFixture(HarnessFixtureKind kind) -> HarnessFixtureImage {
  switch (kind) {
    case HarnessFixtureKind::Fp32Gradient:
      return MakeFp32Gradient(64, 48);
    case HarnessFixtureKind::Checkerboard:
      return MakeCheckerboard(64, 48, 8);
    case HarnessFixtureKind::RoiPatch:
      return MakeRoiPatch(64, 48, 16, 12, 24, 18);
    case HarnessFixtureKind::OddSized:
      return MakeOddSized(63, 47);
  }
  return {};
}

auto ParseHarnessFixtureKind(std::string_view token) -> std::optional<HarnessFixtureKind> {
  if (token == "gradient" || token == "fp32-gradient") {
    return HarnessFixtureKind::Fp32Gradient;
  }
  if (token == "checkerboard" || token == "checker") {
    return HarnessFixtureKind::Checkerboard;
  }
  if (token == "roi" || token == "roi-patch") {
    return HarnessFixtureKind::RoiPatch;
  }
  if (token == "odd" || token == "odd-sized") {
    return HarnessFixtureKind::OddSized;
  }
  return std::nullopt;
}

auto ToString(HarnessFixtureKind kind) -> const char* {
  switch (kind) {
    case HarnessFixtureKind::Fp32Gradient:
      return "fp32-gradient";
    case HarnessFixtureKind::Checkerboard:
      return "checkerboard";
    case HarnessFixtureKind::RoiPatch:
      return "roi-patch";
    case HarnessFixtureKind::OddSized:
      return "odd-sized";
  }
  return "unknown";
}

auto MaxAbsPixelError(const HarnessFixtureImage& expected, const float* actual_rgba,
                      int actual_width, int actual_height, std::size_t actual_row_bytes) -> float {
  if (!actual_rgba || expected.width != actual_width || expected.height != actual_height ||
      expected.pixels.empty()) {
    return -1.0f;
  }
  const std::size_t min_row =
      std::min(expected.row_bytes(), actual_row_bytes);
  float max_err = 0.0f;
  for (int y = 0; y < expected.height; ++y) {
    const auto* exp_row =
        reinterpret_cast<const float*>(&expected.pixels[static_cast<std::size_t>(y) *
                                                        static_cast<std::size_t>(expected.width)]);
    const auto* act_row = reinterpret_cast<const float*>(
        reinterpret_cast<const char*>(actual_rgba) +
        static_cast<std::size_t>(y) * actual_row_bytes);
    const int channels = static_cast<int>(min_row / sizeof(float));
    for (int c = 0; c < channels; ++c) {
      max_err = std::max(max_err, std::fabs(exp_row[c] - act_row[c]));
    }
  }
  return max_err;
}

auto SmallRealRawFixtureRelativePath() -> std::string {
  // Small ARW used by existing CI/raw tests; relative to repository root.
  return "alcedo_studio/tests/resources/sample_images/raw/plant/_DSC0180.ARW";
}

}  // namespace alcedo::editor_rhi
