//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/mask/ai_mask_coverage_texture.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "edit/mask/mask_asset.hpp"
#include "edit/runtime/mask_asset_reader.hpp"

namespace alcedo {
namespace {

/// A whole analysis image of float coverage in [0, 1], row-major.
struct Plane {
  std::int32_t        width  = 0;
  std::int32_t        height = 0;
  std::vector<double> values;

  [[nodiscard]] auto  At(std::int32_t x, std::int32_t y) const -> double {
    x = std::clamp(x, 0, width - 1);
    y = std::clamp(y, 0, height - 1);
    return values[static_cast<std::size_t>(y) * width + x];
  }
};

/// The asset of the pixels of @p plane inside @p rect, with bounds in the plane.
auto MakeAsset(const Plane& plane, RectI rect) -> MaskAsset {
  MaskAsset asset;
  asset.descriptor.extent =
      Extent2D{static_cast<std::uint32_t>(rect.width), static_cast<std::uint32_t>(rect.height)};
  asset.descriptor.reference_bounds =
      NormalizedRect{static_cast<float>(rect.x) / static_cast<float>(plane.width),
                     static_cast<float>(rect.y) / static_cast<float>(plane.height),
                     static_cast<float>(rect.width) / static_cast<float>(plane.width),
                     static_cast<float>(rect.height) / static_cast<float>(plane.height)};
  for (std::int32_t y = rect.y; y < rect.Y1(); ++y) {
    for (std::int32_t x = rect.x; x < rect.X1(); ++x) {
      asset.pixels.push_back(static_cast<std::uint8_t>(std::lround(plane.At(x, y) * 255.0)));
    }
  }
  asset.key = MakeMaskAssetKey(asset.descriptor, asset.pixels);
  return asset;
}

/// The plane with only the asset pixels (zero elsewhere), as R8 / 255.
auto PlaneOfAsset(const MaskAsset& asset, std::int32_t width, std::int32_t height) -> Plane {
  Plane plane{width, height, std::vector<double>(static_cast<std::size_t>(width) * height, 0.0)};
  const auto left = static_cast<std::int32_t>(
      std::lround(asset.descriptor.reference_bounds.x * static_cast<double>(width)));
  const auto top = static_cast<std::int32_t>(
      std::lround(asset.descriptor.reference_bounds.y * static_cast<double>(height)));
  for (std::uint32_t row = 0; row < asset.descriptor.extent.height; ++row) {
    for (std::uint32_t column = 0; column < asset.descriptor.extent.width; ++column) {
      plane.values[static_cast<std::size_t>(top + row) * width + left + column] =
          asset.pixels[row * asset.descriptor.extent.width + column] / 255.0;
    }
  }
  return plane;
}

/// One 3 x 3 dilate or erode with a cross or square neighborhood and replicated borders.
auto Morph(const Plane& plane, bool square, bool dilate) -> Plane {
  Plane result = plane;
  for (std::int32_t y = 0; y < plane.height; ++y) {
    for (std::int32_t x = 0; x < plane.width; ++x) {
      double value = plane.At(x, y);
      for (std::int32_t dy = -1; dy <= 1; ++dy) {
        for (std::int32_t dx = -1; dx <= 1; ++dx) {
          if (!square && dx != 0 && dy != 0) {
            continue;
          }
          const double sample = plane.At(x + dx, y + dy);
          value               = dilate ? std::max(value, sample) : std::min(value, sample);
        }
      }
      result.values[static_cast<std::size_t>(y) * plane.width + x] = value;
    }
  }
  return result;
}

/// Separable normalized Gaussian with the kernel size of OpenCV for float images
/// (`round(8 * sigma + 1) | 1`) and replicated borders, in double precision.
auto Blur(const Plane& plane, double sigma) -> Plane {
  const auto          size   = static_cast<std::int32_t>(std::lround(sigma * 8.0 + 1.0)) | 1;
  const auto          radius = size / 2;
  std::vector<double> kernel(static_cast<std::size_t>(size));
  double              sum = 0.0;
  for (std::int32_t i = 0; i < size; ++i) {
    const double x = i - radius;
    kernel[i]      = std::exp(-(x * x) / (2.0 * sigma * sigma));
    sum += kernel[i];
  }
  for (auto& weight : kernel) {
    weight /= sum;
  }
  Plane rows = plane;
  for (std::int32_t y = 0; y < plane.height; ++y) {
    for (std::int32_t x = 0; x < plane.width; ++x) {
      double value = 0.0;
      for (std::int32_t i = 0; i < size; ++i) {
        value += kernel[i] * plane.At(x + i - radius, y);
      }
      rows.values[static_cast<std::size_t>(y) * plane.width + x] = value;
    }
  }
  Plane result = rows;
  for (std::int32_t y = 0; y < plane.height; ++y) {
    for (std::int32_t x = 0; x < plane.width; ++x) {
      double value = 0.0;
      for (std::int32_t i = 0; i < size; ++i) {
        value += kernel[i] * rows.At(x, y + i - radius);
      }
      result.values[static_cast<std::size_t>(y) * plane.width + x] = value;
    }
  }
  return result;
}

/// Lab F8 on the whole image: expand steps (cross, square, ...), then the blur; R8 result.
auto WholeImageEdgeResult(Plane plane, const AiMaskEdgeAdjustment& edge) -> std::vector<int> {
  for (std::int32_t step = 0; step < std::abs(edge.expand_px); ++step) {
    plane = Morph(plane, step % 2 == 1, edge.expand_px > 0);
  }
  if (edge.feather_px > 0.0f) {
    plane = Blur(plane, edge.feather_px);
  }
  std::vector<int> result(plane.values.size());
  for (std::size_t i = 0; i < result.size(); ++i) {
    result[i] = static_cast<int>(std::lround(std::clamp(plane.values[i], 0.0, 1.0) * 255.0));
  }
  return result;
}

auto TextureRect(const AiMaskCoverageTexture& texture, std::int32_t width, std::int32_t height)
    -> RectI {
  const auto rect = AiMaskAssetPixelRect(
      texture.descriptor,
      Extent2D{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)});
  EXPECT_TRUE(rect.has_value());
  return rect.value_or(RectI{});
}

/// Every texel within 1 of the whole-image result, and the whole-image result 0 outside.
void ExpectTextureEqualsWholeImageResult(const AiMaskCoverageTexture& texture,
                                         const std::vector<int>& expected, std::int32_t width,
                                         std::int32_t height) {
  const auto rect = TextureRect(texture, width, height);
  for (std::int32_t y = 0; y < height; ++y) {
    for (std::int32_t x = 0; x < width; ++x) {
      const int value = expected[static_cast<std::size_t>(y) * width + x];
      if (!rect.Contains(x, y)) {
        ASSERT_EQ(value, 0) << "whole-image value above 0 outside the texture at " << x << ", "
                            << y;
        continue;
      }
      const int texel =
          texture.pixels[static_cast<std::size_t>(y - rect.y) * rect.width + (x - rect.x)];
      ASSERT_LE(std::abs(texel - value), 1) << "texel " << x << ", " << y;
    }
  }
}

auto EmptyPlane(std::int32_t width, std::int32_t height) -> Plane {
  return Plane{width, height, std::vector<double>(static_cast<std::size_t>(width) * height, 0.0)};
}

void Fill(Plane& plane, RectI rect, double value) {
  for (std::int32_t y = rect.y; y < rect.Y1(); ++y) {
    for (std::int32_t x = rect.x; x < rect.X1(); ++x) {
      plane.values[static_cast<std::size_t>(y) * plane.width + x] = value;
    }
  }
}

TEST(AiMaskCoverageTextureTest, CoverageTextureWithoutEdgeEqualsAssetBytes) {
  auto plane = EmptyPlane(32, 24);
  Fill(plane, RectI{9, 7, 6, 5}, 0.8);
  const auto asset   = MakeAsset(plane, RectI{8, 6, 8, 7});
  const auto texture = MakeAiMaskCoverageTexture(asset, Extent2D{32, 24}, AiMaskEdgeAdjustment{});
  EXPECT_EQ(texture.descriptor, asset.descriptor);
  EXPECT_EQ(texture.pixels, asset.pixels);
}

TEST(AiMaskCoverageTextureTest, ExpandDilatesByAlternatingCrossAndSquare) {
  auto plane = EmptyPlane(16, 16);
  Fill(plane, RectI{7, 7, 1, 1}, 1.0);
  const auto asset = MakeAsset(plane, RectI{6, 6, 3, 3});
  const auto texture =
      MakeAiMaskCoverageTexture(asset, Extent2D{16, 16}, AiMaskEdgeAdjustment{0.0f, 2});
  const auto rect = TextureRect(texture, 16, 16);
  EXPECT_EQ(rect.x, 4);
  EXPECT_EQ(rect.y, 4);
  EXPECT_EQ(rect.width, 7);
  EXPECT_EQ(rect.height, 7);
  for (std::int32_t y = 0; y < rect.height; ++y) {
    for (std::int32_t x = 0; x < rect.width; ++x) {
      const int  dx     = std::abs(x - 3);
      const int  dy     = std::abs(y - 3);
      const bool inside = dx <= 2 && dy <= 2 && !(dx == 2 && dy == 2);
      EXPECT_EQ(texture.pixels[static_cast<std::size_t>(y) * rect.width + x], inside ? 255 : 0)
          << x << ", " << y;
    }
  }
}

TEST(AiMaskCoverageTextureTest, NegativeExpandErodes) {
  auto plane = EmptyPlane(16, 16);
  Fill(plane, RectI{4, 4, 7, 7}, 1.0);
  const auto asset = MakeAsset(plane, RectI{3, 3, 9, 9});
  const auto texture =
      MakeAiMaskCoverageTexture(asset, Extent2D{16, 16}, AiMaskEdgeAdjustment{0.0f, -2});
  EXPECT_EQ(texture.descriptor, asset.descriptor);
  for (std::int32_t y = 0; y < 9; ++y) {
    for (std::int32_t x = 0; x < 9; ++x) {
      const bool inside = x >= 3 && x <= 5 && y >= 3 && y <= 5;
      EXPECT_EQ(texture.pixels[static_cast<std::size_t>(y) * 9 + x], inside ? 255 : 0)
          << x << ", " << y;
    }
  }
}

TEST(AiMaskCoverageTextureTest, FeatherMatchesIndependentGaussian) {
  auto plane = EmptyPlane(64, 48);
  Fill(plane, RectI{24, 18, 12, 10}, 1.0);
  const auto                 asset = MakeAsset(plane, RectI{23, 17, 14, 12});
  const AiMaskEdgeAdjustment edge{3.0f, 0};
  const auto                 texture = MakeAiMaskCoverageTexture(asset, Extent2D{64, 48}, edge);
  const auto                 rect    = TextureRect(texture, 64, 48);
  EXPECT_EQ(rect.x, 23 - 13);
  EXPECT_EQ(rect.width, 14 + 2 * 13);
  ExpectTextureEqualsWholeImageResult(
      texture, WholeImageEdgeResult(PlaneOfAsset(asset, 64, 48), edge), 64, 48);
}

TEST(AiMaskCoverageTextureTest, TextureClipsAtImageEdgeAndReplicatesThere) {
  auto plane = EmptyPlane(40, 30);
  Fill(plane, RectI{0, 6, 6, 16}, 1.0);
  const auto                 asset = MakeAsset(plane, RectI{0, 5, 7, 18});
  const AiMaskEdgeAdjustment edge{2.0f, 0};
  const auto                 texture = MakeAiMaskCoverageTexture(asset, Extent2D{40, 30}, edge);
  const auto                 rect    = TextureRect(texture, 40, 30);
  EXPECT_EQ(rect.x, 0);
  const auto expected = WholeImageEdgeResult(PlaneOfAsset(asset, 40, 30), edge);
  // The replicated left edge keeps x = 0 near full coverage in the middle rows; a zero border
  // there would give about half.
  EXPECT_GE(expected[static_cast<std::size_t>(14) * 40], 250);
  ExpectTextureEqualsWholeImageResult(texture, expected, 40, 30);
}

TEST(AiMaskCoverageTextureTest, TextureHoldsAllNonZeroValuesOfTheWholeImageResult) {
  auto plane = EmptyPlane(72, 56);
  for (std::int32_t y = 20; y < 34; ++y) {
    for (std::int32_t x = 25; x < 45; ++x) {
      if ((x - 25) + (y - 20) < 22) {
        plane.values[static_cast<std::size_t>(y) * 72 + x] = ((x * 7 + y * 3) % 11) / 10.0;
      }
    }
  }
  const auto                 asset = MakeAsset(plane, RectI{24, 19, 22, 16});
  const AiMaskEdgeAdjustment edge{2.5f, 3};
  const auto                 texture = MakeAiMaskCoverageTexture(asset, Extent2D{72, 56}, edge);
  ExpectTextureEqualsWholeImageResult(
      texture, WholeImageEdgeResult(PlaneOfAsset(asset, 72, 56), edge), 72, 56);
}

TEST(AiMaskCoverageTextureTest, CoverageTextureRejectsAssetThatDoesNotFit) {
  auto plane = EmptyPlane(32, 24);
  Fill(plane, RectI{9, 7, 6, 5}, 1.0);
  const auto asset = MakeAsset(plane, RectI{8, 6, 8, 7});
  // A different analysis size: the bounds no longer match the asset size.
  EXPECT_THROW((void)MakeAiMaskCoverageTexture(asset, Extent2D{64, 48}, AiMaskEdgeAdjustment{}),
               std::runtime_error);
  auto outside                          = asset;
  outside.descriptor.reference_bounds.x = 0.9f;
  outside.key                           = MakeMaskAssetKey(outside.descriptor, outside.pixels);
  EXPECT_THROW((void)MakeAiMaskCoverageTexture(outside, Extent2D{32, 24}, AiMaskEdgeAdjustment{}),
               std::runtime_error);
  EXPECT_THROW(
      (void)MakeAiMaskCoverageTexture(asset, Extent2D{32, 24}, AiMaskEdgeAdjustment{51.0f, 0}),
      std::runtime_error);
  EXPECT_THROW(
      (void)MakeAiMaskCoverageTexture(asset, Extent2D{32, 24}, AiMaskEdgeAdjustment{0.0f, -51}),
      std::runtime_error);
}

TEST(AiMaskCoverageTextureTest, DefaultMaskAssetReaderReportsMissingStore) {
  const MaskAssetKey key{std::string(32, 'a')};
  try {
    (void)DefaultMaskAssetReader()->Read(key);
    FAIL() << "the default reader returned an asset";
  } catch (const std::runtime_error& error) {
    const std::string message = error.what();
    EXPECT_NE(message.find(std::string(32, 'a')), std::string::npos) << message;
    EXPECT_NE(message.find("no project mask asset store"), std::string::npos) << message;
  }
}

}  // namespace
}  // namespace alcedo
