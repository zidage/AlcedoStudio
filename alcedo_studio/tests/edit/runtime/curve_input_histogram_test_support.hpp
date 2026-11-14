//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

/// @file curve_input_histogram_test_support.hpp
/// @brief CPU reference of the curve input histogram (issue #888) for the CUDA, OpenCL, and Metal
///        grade tests.
///
/// The reference reads the grade input that the test downloaded, takes the same sample grid as
/// the kernels (CurveInputHistogramSampleStep), and bins each channel with
/// CurveInputHistogramBin. With neutral adjustments before the Curve, the kernel input of the bins
/// is the grade input bit for bit, so the counts must be equal. With an Exposure offset, the GPU
/// and the CPU can round the offset differently in the last bit, so a value that lies within
/// kBinEdgeMargin of a bin edge may count in either bin.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "edit/runtime/curve_input_histogram.hpp"

namespace alcedo::curve_histogram_test {

/// Distance from a bin edge, on the ACEScc axis, inside which a shifted value has no exact bin.
inline constexpr float kBinEdgeMargin = 1.0e-5f;

/// Sampled values of channel @p channel (0 red, 1 green, 2 blue) of a pixel with members r, g, b.
template <class Pixel>
auto ChannelValue(const Pixel& pixel, std::uint32_t channel) -> float {
  return channel == 0U ? pixel.r : (channel == 1U ? pixel.g : pixel.b);
}

/// Call @p visit(value, channel) for each channel of each pixel of the sample grid.
template <class Pixel, class Visit>
void ForEachSampledValue(const std::vector<Pixel>& pixels, std::uint32_t width,
                         std::uint32_t height, Visit&& visit) {
  const auto step = CurveInputHistogramSampleStep(width, height);
  for (std::uint32_t y = 0; y < height; y += step) {
    for (std::uint32_t x = 0; x < width; x += step) {
      const auto& pixel = pixels[static_cast<std::size_t>(y) * width + x];
      for (std::uint32_t channel = 0; channel < kCurveInputHistogramChannelCount; ++channel) {
        visit(ChannelValue(pixel, channel), channel);
      }
    }
  }
}

/// Counts of the sample grid of @p pixels (row-major, @p width x @p height).
template <class Pixel>
auto ReferenceCounts(const std::vector<Pixel>& pixels, std::uint32_t width, std::uint32_t height)
    -> std::vector<std::uint32_t> {
  std::vector<std::uint32_t> counts(kCurveInputHistogramValueCount, 0U);
  ForEachSampledValue(pixels, width, height, [&counts](float value, std::uint32_t channel) {
    ++counts[channel * kCurveInputHistogramBins + CurveInputHistogramBin(value)];
  });
  return counts;
}

/**
 * @brief Expect that @p counts are the histogram of @p pixels shifted by @p offset on each channel.
 *
 * A shifted value farther than kBinEdgeMargin from a bin edge has one bin; a closer value can be in
 * either of its two bins. Each bin must hold at least its certain values and at most its certain
 * and possible values, and each channel must count every sample once.
 */
template <class Pixel>
void ExpectCountsOfShiftedPixels(const std::vector<std::uint32_t>& counts,
                                 const std::vector<Pixel>& pixels, std::uint32_t width,
                                 std::uint32_t height, float offset, const std::string& label) {
  ASSERT_EQ(counts.size(), kCurveInputHistogramValueCount) << label;
  std::vector<std::uint32_t> certain(kCurveInputHistogramValueCount, 0U);
  std::vector<std::uint32_t> possible(kCurveInputHistogramValueCount, 0U);
  ForEachSampledValue(pixels, width, height, [&](float value, std::uint32_t channel) {
    const float shifted = value + offset;
    const auto  low     = CurveInputHistogramBin(shifted - kBinEdgeMargin);
    const auto  high    = CurveInputHistogramBin(shifted + kBinEdgeMargin);
    const auto  base    = channel * kCurveInputHistogramBins;
    if (low == high) {
      ++certain[base + low];
    } else {
      ++possible[base + low];
      ++possible[base + high];
    }
  });
  const auto samples = CurveInputHistogramSampleCount(width, height);
  for (std::uint32_t channel = 0; channel < kCurveInputHistogramChannelCount; ++channel) {
    std::uint64_t total = 0;
    for (std::uint32_t bin = 0; bin < kCurveInputHistogramBins; ++bin) {
      const auto index = channel * kCurveInputHistogramBins + bin;
      total += counts[index];
      EXPECT_GE(counts[index], certain[index])
          << label << ": channel " << channel << " bin " << bin;
      EXPECT_LE(counts[index], certain[index] + possible[index])
          << label << ": channel " << channel << " bin " << bin;
    }
    EXPECT_EQ(total, samples) << label << ": channel " << channel;
  }
}

}  // namespace alcedo::curve_histogram_test
