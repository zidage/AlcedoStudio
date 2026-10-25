//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/ai_mask_coverage_composition.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <opencv2/core.hpp>
#include <opencv2/core/mat.hpp>
#include <opencv2/imgproc.hpp>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace alcedo {
namespace {

void Fail(const std::string& message) {
  throw std::invalid_argument("AI Mask coverage: " + message);
}

auto PixelCount(Extent2D extent) -> std::size_t {
  return static_cast<std::size_t>(extent.width) * extent.height;
}

/// An 8-bit matrix over the row-major @p coverage of @p extent; it shares the memory.
auto CoverageMat(std::vector<std::uint8_t>& coverage, Extent2D extent) -> cv::Mat {
  return cv::Mat(static_cast<int>(extent.height), static_cast<int>(extent.width), CV_8U,
                 coverage.data());
}

void RequireExtent(Extent2D extent) {
  if (extent.Empty()) {
    Fail("the session image size is empty");
  }
}

/// The pixels [first, end) of one axis of @p size pixels whose centers lie in [start, stop) of
/// the normalized axis.
auto CenterSpan(float start, float stop, std::uint32_t size) -> std::pair<int, int> {
  const double scale = static_cast<double>(size);
  const int    first = std::max(0, static_cast<int>(std::ceil(start * scale - 0.5)));
  const int end = std::min(static_cast<int>(size), static_cast<int>(std::ceil(stop * scale - 0.5)));
  return {first, std::max(first, end)};
}

auto ConvertLogits(const AiMaskSamLogits& logits, Extent2D extent) -> std::vector<std::uint8_t> {
  const auto    side = static_cast<int>(logits.size);
  // cv::Mat does not change the logits; the const_cast only adapts the constructor signature.
  const cv::Mat grid(side, side, CV_32F, const_cast<float*>(logits.logits.data()));
  cv::Mat       resized;
  cv::resize(grid, resized,
             cv::Size(static_cast<int>(extent.width), static_cast<int>(extent.height)), 0.0, 0.0,
             cv::INTER_LINEAR);
  std::vector<std::uint8_t> coverage(PixelCount(extent));
  cv::Mat                   out = CoverageMat(coverage, extent);
  // 255 where the logit is > 0, else 0.
  cv::compare(resized, 0.0, out, cv::CMP_GT);
  return coverage;
}

/// Standard deviation, in alpha grid cells, of the Gaussian that smooths an alpha grid before its
/// upscale (#522). It removes the steps of the grid from a near-binary S3OD edge on diagonal and
/// curved edges; a feature of one grid cell is dimmed, features of two cells and more stay.
constexpr double kAlphaEdgeSigma = 0.8;

auto ConvertAlpha(const AiMaskAlphaGrid& alpha, Extent2D extent) -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> coverage(PixelCount(extent), 0);
  const auto [left, right] = CenterSpan(alpha.box.x0, alpha.box.x1, extent.width);
  const auto [top, bottom] = CenterSpan(alpha.box.y0, alpha.box.y1, extent.height);
  if (left == right || top == bottom) {
    return coverage;
  }
  const cv::Mat grid(static_cast<int>(alpha.height), static_cast<int>(alpha.width), CV_32F,
                     const_cast<float*>(alpha.values.data()));
  cv::Mat       smoothed;
  cv::GaussianBlur(grid, smoothed, cv::Size(), kAlphaEdgeSigma, kAlphaEdgeSigma,
                   cv::BORDER_REPLICATE);
  cv::Mat resized;
  cv::resize(smoothed, resized, cv::Size(right - left, bottom - top), 0.0, 0.0, cv::INTER_LINEAR);
  // The smoothed edge is about 2 cells wide; a gain of the upscale factor around 0.5 makes it about
  // 2 session pixels wide at every scale. A grid at the session scale or finer keeps its values.
  const double gain =
      std::max({1.0, static_cast<double>(right - left) / static_cast<double>(alpha.width),
                static_cast<double>(bottom - top) / static_cast<double>(alpha.height)});
  // round(((alpha - 0.5) * gain + 0.5) * 255) saturated to [0, 255]: the clamp to [0, 1].
  cv::Mat out = CoverageMat(coverage, extent)(cv::Rect(left, top, right - left, bottom - top));
  resized.convertTo(out, CV_8U, 255.0 * gain, 127.5 * (1.0 - gain));
  return coverage;
}

void RequireUnitBox(const AiMaskBox& box) {
  const auto in_unit = [](float value) {
    return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
  };
  if (!in_unit(box.x0) || !in_unit(box.y0) || !in_unit(box.x1) || !in_unit(box.y1) ||
      box.x0 > box.x1 || box.y0 > box.y1) {
    Fail("an alpha grid box must have ordered corners in [0, 1]");
  }
}

}  // namespace

void ValidateAiMaskModelOutput(const AiMaskModelOutput& output, Extent2D extent) {
  RequireExtent(extent);
  std::visit(
      [extent](const auto& value) {
        using Output = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Output, AiMaskSamLogits>) {
          if (value.size == 0 ||
              value.logits.size() != static_cast<std::size_t>(value.size) * value.size) {
            Fail("decoder logits must hold size x size values");
          }
        } else if constexpr (std::is_same_v<Output, AiMaskAlphaGrid>) {
          if (value.width == 0 || value.height == 0 ||
              value.values.size() != static_cast<std::size_t>(value.width) * value.height) {
            Fail("an alpha grid must hold width x height values");
          }
          RequireUnitBox(value.box);
        } else {
          static_assert(std::is_same_v<Output, AiMaskBinaryR8>,
                        "ValidateAiMaskModelOutput misses a model output kind");
          if (value.width != extent.width || value.height != extent.height ||
              value.values.size() != PixelCount(extent)) {
            Fail("an R8 mask must have the session image size " + std::to_string(extent.width) +
                 " x " + std::to_string(extent.height));
          }
        }
      },
      output);
}

auto ConvertModelOutputToCoverage(const AiMaskModelOutput& output, Extent2D extent)
    -> std::vector<std::uint8_t> {
  ValidateAiMaskModelOutput(output, extent);
  return std::visit(
      [extent](const auto& value) -> std::vector<std::uint8_t> {
        using Output = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Output, AiMaskSamLogits>) {
          return ConvertLogits(value, extent);
        } else if constexpr (std::is_same_v<Output, AiMaskAlphaGrid>) {
          return ConvertAlpha(value, extent);
        } else {
          static_assert(std::is_same_v<Output, AiMaskBinaryR8>,
                        "ConvertModelOutputToCoverage misses a model output kind");
          return value.values;
        }
      },
      output);
}

void FoldAiMaskCoverage(std::vector<std::uint8_t>&    coverage,
                        std::span<const std::uint8_t> component,
                        AiMaskComponentOperation      operation) {
  if (coverage.size() != component.size()) {
    Fail("a component must have the size of the coverage");
  }
  const auto size = static_cast<int>(coverage.size());
  cv::Mat    total(1, size, CV_8U, coverage.data());
  // cv::Mat does not change the component; the const_cast only adapts the constructor signature.
  const cv::Mat part(1, size, CV_8U, const_cast<std::uint8_t*>(component.data()));
  if (operation == AiMaskComponentOperation::Add) {
    cv::max(total, part, total);
    return;
  }
  // 255 - component of an 8-bit value is its bitwise inverse.
  cv::Mat inverse;
  cv::bitwise_not(part, inverse);
  cv::min(total, inverse, total);
}

auto CropAiMaskCoverage(std::span<const std::uint8_t> coverage, Extent2D extent)
    -> std::optional<MaskAsset> {
  RequireExtent(extent);
  if (coverage.size() != PixelCount(extent)) {
    Fail("the coverage must have the session image size");
  }
  std::uint32_t left = extent.width, top = extent.height, right = 0, bottom = 0;
  for (std::uint32_t row = 0; row < extent.height; ++row) {
    const auto* values = coverage.data() + static_cast<std::size_t>(row) * extent.width;
    for (std::uint32_t column = 0; column < extent.width; ++column) {
      if (values[column] > 0) {
        left   = std::min(left, column);
        right  = std::max(right, column + 1);
        top    = std::min(top, row);
        bottom = std::max(bottom, row + 1);
      }
    }
  }
  if (right == 0) {
    return std::nullopt;
  }
  left   = left > 0 ? left - 1 : 0;
  top    = top > 0 ? top - 1 : 0;
  right  = std::min(extent.width, right + 1);
  bottom = std::min(extent.height, bottom + 1);

  MaskAsset asset;
  asset.descriptor.extent = Extent2D{right - left, bottom - top};
  asset.descriptor.reference_bounds =
      NormalizedRect{static_cast<float>(left) / static_cast<float>(extent.width),
                     static_cast<float>(top) / static_cast<float>(extent.height),
                     static_cast<float>(right - left) / static_cast<float>(extent.width),
                     static_cast<float>(bottom - top) / static_cast<float>(extent.height)};
  asset.pixels.resize(PixelCount(asset.descriptor.extent));
  for (std::uint32_t row = top; row < bottom; ++row) {
    const auto* source = coverage.data() + static_cast<std::size_t>(row) * extent.width + left;
    std::copy_n(source, right - left,
                asset.pixels.data() + static_cast<std::size_t>(row - top) * (right - left));
  }
  asset.key = MakeMaskAssetKey(asset.descriptor, asset.pixels);
  return asset;
}

auto PlaceAiMaskAssetInCoverage(const MaskAsset& asset, Extent2D extent)
    -> std::vector<std::uint8_t> {
  RequireExtent(extent);
  ValidateMaskAssetPixels(asset.descriptor, asset.pixels);
  const auto rect = AiMaskAssetPixelRect(asset.descriptor, extent);
  if (!rect.has_value()) {
    Fail("the stored asset does not fit the session image " + std::to_string(extent.width) + " x " +
         std::to_string(extent.height));
  }
  const auto                width  = asset.descriptor.extent.width;
  const auto                height = asset.descriptor.extent.height;
  const auto                left   = static_cast<std::size_t>(rect->x);
  const auto                top    = static_cast<std::size_t>(rect->y);
  std::vector<std::uint8_t> coverage(PixelCount(extent), 0);
  for (std::uint32_t row = 0; row < height; ++row) {
    std::copy_n(asset.pixels.data() + static_cast<std::size_t>(row) * width, width,
                coverage.data() + static_cast<std::size_t>(top + row) * extent.width + left);
  }
  return coverage;
}

}  // namespace alcedo
