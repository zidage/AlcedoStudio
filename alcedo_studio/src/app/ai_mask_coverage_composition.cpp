//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/ai_mask_coverage_composition.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <opencv2/core.hpp>
#include <opencv2/core/mat.hpp>
#include <opencv2/core/utility.hpp>
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

// Joint bilateral upsampling of an alpha grid (#522, master plan D23). The S3OD alpha is almost
// binary, so an interpolation of the grid alone keeps its steps. A session pixel near an edge
// takes the mean of its 4 x 4 nearest grid cells, weighted by the distance to the cell and by the
// color difference between the pixel and the cell in the guide image.

/// Half width of the cell window: the cells floor(u) - 1 to floor(u) + 2 on each axis.
constexpr int    kAlphaWindowRadius  = 2;
/// Standard deviation of the spatial weight, in grid cells.
constexpr float  kAlphaSpatialSigma  = 1.0f;
/// The range sigma is this factor times the color standard deviation of the window cells, so an
/// edge of low contrast separates as well as an edge of high contrast.
constexpr float  kAlphaRangeScale    = 0.7f;
/// Lower limit of the range sigma (RGB in [0, 1]), against the noise of flat windows.
constexpr float  kAlphaMinRangeSigma = 0.01f;
/// A window whose alpha values differ by at most this value is flat: its pixels take the value.
constexpr float  kAlphaFlatSpread    = 1e-3f;
/// Standard deviation, in session pixels, of the smoothing of the guide image against its noise.
constexpr double kGuideSigma         = 1.0;

/// The guide as an 8-bit RGB matrix; it shares the memory.
auto             GuideMat(const AiMaskCoverageGuide& guide) -> cv::Mat {
  // cv::Mat does not change the guide here; the const_cast only adapts the constructor signature.
  return cv::Mat(static_cast<int>(guide.extent.height), static_cast<int>(guide.extent.width),
                             CV_8UC3, const_cast<std::uint8_t*>(guide.rgb8.data()));
}

/// The upsampled alpha of one pixel at grid position (@p u, @p v) with the guide color @p color
/// (null without a guide). @p low holds the guide color of each cell (CV_32FC3).
auto JointBilateralAlpha(const cv::Mat& grid, const cv::Mat& low, float u, float v,
                         const float* color) -> float {
  constexpr int          kTaps        = 2 * kAlphaWindowRadius;
  const int              first_column = static_cast<int>(std::floor(u)) - kAlphaWindowRadius + 1;
  const int              first_row    = static_cast<int>(std::floor(v)) - kAlphaWindowRadius + 1;
  std::array<int, kTaps> columns{};
  std::array<int, kTaps> rows{};
  for (int tap = 0; tap < kTaps; ++tap) {
    columns[tap] = std::clamp(first_column + tap, 0, grid.cols - 1);
    rows[tap]    = std::clamp(first_row + tap, 0, grid.rows - 1);
  }
  float inverse_range = 0.0f;
  if (color != nullptr) {
    std::array<float, 3> sum{};
    std::array<float, 3> square{};
    for (const int row : rows) {
      for (const int column : columns) {
        const auto* cell = low.ptr<float>(row) + 3 * column;
        for (int channel = 0; channel < 3; ++channel) {
          sum[channel] += cell[channel];
          square[channel] += cell[channel] * cell[channel];
        }
      }
    }
    constexpr float kCount   = static_cast<float>(kTaps * kTaps);
    float           variance = 0.0f;
    for (int channel = 0; channel < 3; ++channel) {
      const float mean = sum[channel] / kCount;
      variance += std::max(0.0f, square[channel] / kCount - mean * mean);
    }
    const float sigma_squared = std::max(kAlphaMinRangeSigma * kAlphaMinRangeSigma,
                                         kAlphaRangeScale * kAlphaRangeScale * variance);
    inverse_range             = 0.5f / sigma_squared;
  }
  constexpr float kInverseSpatial  = 0.5f / (kAlphaSpatialSigma * kAlphaSpatialSigma);
  float           weighted         = 0.0f;
  float           total            = 0.0f;
  float           spatial_weighted = 0.0f;
  float           spatial_total    = 0.0f;
  for (const int row : rows) {
    const float dv = static_cast<float>(row) - v;
    for (const int column : columns) {
      const float du      = static_cast<float>(column) - u;
      const float spatial = std::exp(-(du * du + dv * dv) * kInverseSpatial);
      const float value   = grid.at<float>(row, column);
      spatial_weighted += spatial * value;
      spatial_total += spatial;
      if (color != nullptr) {
        const auto* cell     = low.ptr<float>(row) + 3 * column;
        float       distance = 0.0f;
        for (int channel = 0; channel < 3; ++channel) {
          const float difference = color[channel] - cell[channel];
          distance += difference * difference;
        }
        const float weight = spatial * std::exp(-distance * inverse_range);
        weighted += weight * value;
        total += weight;
      }
    }
  }
  // A pixel whose color is far from every cell keeps the spatial mean.
  if (color != nullptr && total > 1e-12f) {
    return weighted / total;
  }
  return spatial_weighted / spatial_total;
}

auto ConvertAlpha(const AiMaskAlphaGrid& alpha, Extent2D extent, const AiMaskCoverageGuide* guide)
    -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> coverage(PixelCount(extent), 0);
  const auto [left, right] = CenterSpan(alpha.box.x0, alpha.box.x1, extent.width);
  const auto [top, bottom] = CenterSpan(alpha.box.y0, alpha.box.y1, extent.height);
  if (left == right || top == bottom) {
    return coverage;
  }
  const int     width  = right - left;
  const int     height = bottom - top;
  // cv::Mat does not change the values; the const_cast only adapts the constructor signature.
  const cv::Mat grid(static_cast<int>(alpha.height), static_cast<int>(alpha.width), CV_32F,
                     const_cast<float*>(alpha.values.data()));
  // The guide color of each cell: the area mean of the guide pixels that the cell covers.
  cv::Mat       low;
  if (guide != nullptr) {
    cv::Mat low8;
    cv::resize(GuideMat(*guide)(cv::Rect(left, top, width, height)), low8, grid.size(), 0.0, 0.0,
               cv::INTER_AREA);
    low8.convertTo(low, CV_32FC3, 1.0 / 255.0);
  }
  // The alpha spread of the 5 x 5 window around each cell: 0 in the flat inside and outside.
  const cv::Mat window =
      cv::Mat::ones(2 * kAlphaWindowRadius + 1, 2 * kAlphaWindowRadius + 1, CV_8U);
  cv::Mat highest;
  cv::Mat lowest;
  cv::dilate(grid, highest, window, cv::Point(-1, -1), 1, cv::BORDER_REPLICATE);
  cv::erode(grid, lowest, window, cv::Point(-1, -1), 1, cv::BORDER_REPLICATE);
  const cv::Mat      spread       = highest - lowest;
  // Where the guide has no edge, the spatial mean is about 2 cells wide; a gain of half the upscale
  // factor around 0.5 makes it about 4 session pixels wide. A grid at the session scale or at twice
  // the session scale keeps its values. The sky head alpha keeps its values at every scale.
  const float        gain         = alpha.gain == AiMaskAlphaGain::kOne
                                        ? 1.0f
                                        : std::max({1.0f, 0.5f * static_cast<float>(width) / grid.cols,
                                                    0.5f * static_cast<float>(height) / grid.rows});

  // The grid position of each column with half-pixel centers, as cv::resize.
  const float        column_scale = static_cast<float>(grid.cols) / static_cast<float>(width);
  const float        row_scale    = static_cast<float>(grid.rows) / static_cast<float>(height);
  std::vector<float> grid_u(static_cast<std::size_t>(width));
  std::vector<int>   nearest_column(static_cast<std::size_t>(width));
  for (int x = 0; x < width; ++x) {
    grid_u[x]         = (static_cast<float>(x) + 0.5f) * column_scale - 0.5f;
    nearest_column[x] = std::clamp(static_cast<int>(std::lround(grid_u[x])), 0, grid.cols - 1);
  }
  const cv::Mat guide_mat = guide != nullptr ? GuideMat(*guide) : cv::Mat();
  cv::Mat       out       = CoverageMat(coverage, extent)(cv::Rect(left, top, width, height));
  cv::parallel_for_(cv::Range(0, height), [&](const cv::Range& range) {
    for (int y = range.start; y < range.end; ++y) {
      const float v           = (static_cast<float>(y) + 0.5f) * row_scale - 0.5f;
      const int   nearest_row = std::clamp(static_cast<int>(std::lround(v)), 0, grid.rows - 1);
      const auto* row_spread  = spread.ptr<float>(nearest_row);
      const auto* row_alpha   = grid.ptr<float>(nearest_row);
      const auto* guide_row =
          guide != nullptr ? guide_mat.ptr<std::uint8_t>(top + y) + 3 * left : nullptr;
      auto* target = out.ptr<std::uint8_t>(y);
      for (int x = 0; x < width; ++x) {
        const int cell  = nearest_column[x];
        float     value = row_alpha[cell];
        if (row_spread[cell] > kAlphaFlatSpread) {
          std::array<float, 3> color{};
          if (guide_row != nullptr) {
            for (int channel = 0; channel < 3; ++channel) {
              color[channel] = static_cast<float>(guide_row[3 * x + channel]) / 255.0f;
            }
          }
          value = JointBilateralAlpha(grid, low, grid_u[x], v,
                                      guide_row != nullptr ? color.data() : nullptr);
        }
        // round(clamp((alpha - 0.5) * gain + 0.5) * 255) with ties to even.
        target[x] = cv::saturate_cast<std::uint8_t>(((value - 0.5f) * gain + 0.5f) * 255.0f);
      }
    }
  });
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

auto MakeAiMaskCoverageGuide(Extent2D extent, std::span<const std::uint8_t> rgb8)
    -> AiMaskCoverageGuide {
  RequireExtent(extent);
  if (rgb8.size() != PixelCount(extent) * 3) {
    Fail("the guide image must hold width x height RGB8 pixels");
  }
  AiMaskCoverageGuide guide;
  guide.extent = extent;
  guide.rgb8.resize(rgb8.size());
  const cv::Mat source(static_cast<int>(extent.height), static_cast<int>(extent.width), CV_8UC3,
                       const_cast<std::uint8_t*>(rgb8.data()));
  cv::Mat       target = GuideMat(guide);
  cv::GaussianBlur(source, target, cv::Size(), kGuideSigma, kGuideSigma, cv::BORDER_REPLICATE);
  return guide;
}

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
        } else {
          static_assert(std::is_same_v<Output, AiMaskAlphaGrid>,
                        "ValidateAiMaskModelOutput misses a model output kind");
          if (value.width == 0 || value.height == 0 ||
              value.values.size() != static_cast<std::size_t>(value.width) * value.height) {
            Fail("an alpha grid must hold width x height values");
          }
          RequireUnitBox(value.box);
        }
      },
      output);
}

auto ConvertModelOutputToCoverage(const AiMaskModelOutput& output, Extent2D extent,
                                  const AiMaskCoverageGuide* guide) -> std::vector<std::uint8_t> {
  ValidateAiMaskModelOutput(output, extent);
  if (guide != nullptr && guide->extent != extent) {
    Fail("the guide image must have the session image size");
  }
  return std::visit(
      [extent, guide](const auto& value) -> std::vector<std::uint8_t> {
        using Output = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Output, AiMaskSamLogits>) {
          return ConvertLogits(value, extent);
        } else {
          static_assert(std::is_same_v<Output, AiMaskAlphaGrid>,
                        "ConvertModelOutputToCoverage misses a model output kind");
          return ConvertAlpha(value, extent, guide);
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
