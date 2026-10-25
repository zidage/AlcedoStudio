//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/mask/ai_mask_coverage_texture.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <opencv2/core.hpp>
#include <opencv2/core/mat.hpp>
#include <opencv2/core/utility.hpp>
#include <opencv2/imgproc.hpp>
#include <stdexcept>
#include <string>
#include <utility>

namespace alcedo {

namespace {

/// Call @p work for row bands [first, end) of @p rows rows on the OpenCV worker threads.
template <class Work>
void ForEachRowBand(int rows, const Work& work) {
  const int bands = std::max(1, std::min(cv::getNumThreads() * 2, rows / 16));
  cv::parallel_for_(cv::Range(0, bands), [&](const cv::Range& range) {
    for (int band = range.start; band < range.end; ++band) {
      work(rows * band / bands, rows * (band + 1) / bands);
    }
  });
}

/**
 * @brief cv::GaussianBlur(src, dst, {0, 0}, sigma, sigma, BORDER_REPLICATE) for CV_32F, in two
 *        separable passes over row bands on the worker threads.
 *
 * Same kernel as GaussianBlur for float images (size `round(8 * sigma + 1) | 1`). A band is a
 * region of the whole matrix, so the vertical pass reads the real rows above and below it and
 * replicates only at the image edge: the result equals the single call, which runs on one
 * thread.
 */
void GaussianBlurByRowBands(const cv::Mat& src, cv::Mat& dst, double sigma) {
  const int     size   = cvRound(sigma * 8.0 + 1.0) | 1;
  const cv::Mat kernel = cv::getGaussianKernel(size, sigma, CV_32F);
  const cv::Mat one    = cv::Mat::ones(1, 1, CV_32F);
  cv::Mat       horizontal(src.size(), CV_32F);
  dst.create(src.size(), CV_32F);
  ForEachRowBand(src.rows, [&](int first_row, int end_row) {
    cv::Mat out = horizontal.rowRange(first_row, end_row);
    cv::sepFilter2D(src.rowRange(first_row, end_row), out, CV_32F, kernel, one, cv::Point(-1, -1),
                    0.0, cv::BORDER_REPLICATE);
  });
  ForEachRowBand(src.rows, [&](int first_row, int end_row) {
    cv::Mat out = dst.rowRange(first_row, end_row);
    cv::sepFilter2D(horizontal.rowRange(first_row, end_row), out, CV_32F, one, kernel,
                    cv::Point(-1, -1), 0.0, cv::BORDER_REPLICATE);
  });
}

}  // namespace

auto AiMaskEdgeMargin(const AiMaskEdgeAdjustment& edge) -> std::int32_t {
  ValidateAiMaskEdgeAdjustment(edge);
  std::int32_t margin = std::max(edge.expand_px, 0);
  if (edge.feather_px > 0.0f) {
    margin += static_cast<std::int32_t>(std::ceil(4.0f * edge.feather_px)) + 1;
  }
  return margin;
}

auto MakeAiMaskCoverageTexture(const MaskAsset& asset, Extent2D analysis_extent,
                               const AiMaskEdgeAdjustment& edge) -> AiMaskCoverageTexture {
  ValidateMaskAssetPixels(asset.descriptor, asset.pixels);
  return MakeAiMaskCoverageTexture(MakeAiMaskCoverageWork(asset.descriptor, analysis_extent, edge),
                                   asset.pixels);
}

auto MakeAiMaskCoverageTexture(const AiMaskCoverageWork&     work,
                               std::span<const std::uint8_t> source_pixels)
    -> AiMaskCoverageTexture {
  ValidateMaskAssetPixels(work.source, source_pixels);
  if (!work.HasEdgeAdjustment()) {
    return AiMaskCoverageTexture{work.source, {source_pixels.begin(), source_pixels.end()}};
  }

  const auto&           rect   = work.source_rect;
  const auto            left   = work.output_rect.x;
  const auto            top    = work.output_rect.y;
  const auto            width  = work.output_rect.width;
  const auto            height = work.output_rect.height;
  const auto&           edge   = work.edge;

  AiMaskCoverageTexture texture;
  texture.pixels.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
  cv::Mat       coverage(height, width, CV_8U, texture.pixels.data());
  const cv::Mat asset_r8(rect.height, rect.width, CV_8U,
                         const_cast<std::uint8_t*>(source_pixels.data()));
  asset_r8.copyTo(coverage(cv::Rect(rect.x - left, rect.y - top, rect.width, rect.height)));

  // A maximum or minimum commutes with the monotone map v / 255, so Expand on the R8 values gives
  // the float result of F8 exactly.
  const cv::Mat kernels[2] = {cv::getStructuringElement(cv::MORPH_CROSS, cv::Size(3, 3)),
                              cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3))};
  const auto    steps      = std::abs(edge.expand_px);
  if (steps > 0) {
    cv::Mat scratch(height, width, CV_8U);
    cv::Mat source = coverage;
    cv::Mat target = scratch;
    for (std::int32_t step = 0; step < steps; ++step) {
      const auto& kernel = kernels[step % 2];
      ForEachRowBand(height, [&](int first_row, int end_row) {
        cv::Mat out = target.rowRange(first_row, end_row);
        if (edge.expand_px > 0) {
          cv::dilate(source.rowRange(first_row, end_row), out, kernel, cv::Point(-1, -1), 1,
                     cv::BORDER_REPLICATE);
        } else {
          cv::erode(source.rowRange(first_row, end_row), out, kernel, cv::Point(-1, -1), 1,
                    cv::BORDER_REPLICATE);
        }
      });
      std::swap(source, target);
    }
    if (source.data != coverage.data) {
      source.copyTo(coverage);
    }
  }

  if (edge.feather_px > 0.0f) {
    cv::Mat values;
    coverage.convertTo(values, CV_32F, 1.0 / 255.0);
    cv::Mat blurred;
    GaussianBlurByRowBands(values, blurred, edge.feather_px);
    // Saturating conversion: clamps to [0, 255] and rounds to the nearest integer.
    blurred.convertTo(coverage, CV_8U, 255.0);
  }

  texture.descriptor = work.OutputDescriptor();
  return texture;
}

}  // namespace alcedo
