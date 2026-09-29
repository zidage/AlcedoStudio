//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/geometry/crop_frame.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <stdexcept>

namespace alcedo {
namespace {

constexpr float kCornerEps = 1.0e-2f;

void            ExpectCornersInsideSource(NormalizedRect crop, float degrees, Extent2D source) {
  const float full_w = static_cast<float>(source.width);
  const float full_h = static_cast<float>(source.height);
  for (const auto& corner : CropFrameCornersInReference(crop, degrees, source)) {
    EXPECT_GE(corner.x, -kCornerEps) << degrees << " degrees";
    EXPECT_LE(corner.x, full_w + kCornerEps) << degrees << " degrees";
    EXPECT_GE(corner.y, -kCornerEps) << degrees << " degrees";
    EXPECT_LE(corner.y, full_h + kCornerEps) << degrees << " degrees";
  }
}

}  // namespace

TEST(GpuDagGeometry, ClampedRotatedCropCornersStayInsideSource) {
  const Extent2D       sources[] = {{6000, 4000}, {4000, 6000}, {1000, 1000}};
  const NormalizedRect crops[]   = {
      {0.0f, 0.0f, 1.0f, 1.0f},    // full frame
      {0.6f, 0.1f, 0.4f, 0.3f},    // touching the right edge
      {0.1f, 0.45f, 0.9f, 0.05f},  // long, thin frame
  };
  const float angles[] = {-180.0f, -135.0f, -45.0f, -3.5f, 0.0f, 7.0f, 45.0f, 90.0f, 170.0f};
  for (const auto& source : sources) {
    for (const auto& crop : crops) {
      for (const float degrees : angles) {
        const auto clamped = ClampCropToRotatedSource(crop, degrees, source);
        ExpectCornersInsideSource(clamped, degrees, source);
        EXPECT_GT(clamped.w, 0.0f);
        EXPECT_GT(clamped.h, 0.0f);
      }
    }
  }
}

TEST(GpuDagGeometry, ClampKeepsCenterAndAspectWhenShrinkingARotatedFrame) {
  const Extent2D       source{3000, 2000};
  const NormalizedRect crop{0.0f, 0.0f, 1.0f, 1.0f};
  const auto           clamped = ClampCropToRotatedSource(crop, 10.0f, source);

  EXPECT_NEAR(clamped.x + 0.5f * clamped.w, 0.5f, 1.0e-5f);
  EXPECT_NEAR(clamped.y + 0.5f * clamped.h, 0.5f, 1.0e-5f);
  EXPECT_NEAR(clamped.w, clamped.h, 1.0e-5f);  // 3:2 pixels, same normalized scale
  EXPECT_LT(clamped.w, 1.0f);
}

TEST(GpuDagGeometry, ClampReturnsAFittingRotatedFrameUnchanged) {
  const Extent2D       source{2000, 1000};
  const NormalizedRect crop{0.3f, 0.25f, 0.4f, 0.5f};
  const auto           clamped = ClampCropToRotatedSource(crop, 10.0f, source);
  EXPECT_NEAR(clamped.x, crop.x, 1.0e-6f);
  EXPECT_NEAR(clamped.y, crop.y, 1.0e-6f);
  EXPECT_NEAR(clamped.w, crop.w, 1.0e-6f);
  EXPECT_NEAR(clamped.h, crop.h, 1.0e-6f);
}

TEST(GpuDagGeometry, ClampMovesAFrameThatLeavesTheSourceBackInside) {
  const Extent2D       source{1000, 1000};
  const NormalizedRect crop{0.8f, 0.8f, 0.4f, 0.4f};
  const auto           clamped = ClampCropToRotatedSource(crop, 0.0f, source);
  EXPECT_NEAR(clamped.x, 0.6f, 1.0e-5f);
  EXPECT_NEAR(clamped.y, 0.6f, 1.0e-5f);
  EXPECT_NEAR(clamped.w, 0.4f, 1.0e-5f);
  EXPECT_NEAR(clamped.h, 0.4f, 1.0e-5f);
}

TEST(GpuDagGeometry, ClampRejectsNonFiniteCropAndEmptySource) {
  EXPECT_THROW(ClampCropToRotatedSource({0.0f, 0.0f, std::nanf(""), 1.0f}, 0.0f, {10, 10}),
               std::runtime_error);
  EXPECT_THROW(ClampCropToRotatedSource({0.0f, 0.0f, 1.0f, 1.0f}, 0.0f, {0, 10}),
               std::runtime_error);
}

}  // namespace alcedo
