//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

// Independent host references of the AI Mask Feather and Expand (SM8 section 5.1). Plain loops in
// double precision; no OpenCV and no code of the runtime implementations.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <stdexcept>
#include <vector>

#include "edit/geometry/types.hpp"
#include "edit/mask/ai_mask_recipe.hpp"
#include "edit/runtime/ai_mask_coverage_work.hpp"

namespace alcedo::ai_mask_test {

/// One 3 x 3 dilate or erode step (cross or square kernel) with replicated borders.
inline auto MorphologyStep(std::span<const std::uint8_t> input, int width, int height, bool dilate,
                           bool square) -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> output(input.size());
  const auto                at = [&](int x, int y) {
    x = std::clamp(x, 0, width - 1);
    y = std::clamp(y, 0, height - 1);
    return input[static_cast<std::size_t>(y) * width + x];
  };
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      std::uint8_t value = at(x, y);
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          if ((dx == 0 && dy == 0) || (!square && dx != 0 && dy != 0)) {
            continue;
          }
          const auto neighbor = at(x + dx, y + dy);
          value               = dilate ? std::max(value, neighbor) : std::min(value, neighbor);
        }
      }
      output[static_cast<std::size_t>(y) * width + x] = value;
    }
  }
  return output;
}

/// Normalized Gaussian of sigma @p sigma with `round(8 * sigma + 1) | 1` taps, in double.
inline auto ReferenceGaussianKernel(double sigma) -> std::vector<double> {
  const int           size   = static_cast<int>(std::nearbyint(sigma * 8.0 + 1.0)) | 1;
  const int           radius = size / 2;
  std::vector<double> kernel(static_cast<std::size_t>(size));
  double              sum = 0.0;
  for (int i = 0; i < size; ++i) {
    const double x                      = i - radius;
    kernel[static_cast<std::size_t>(i)] = std::exp(-(x * x) / (2.0 * sigma * sigma));
    sum += kernel[static_cast<std::size_t>(i)];
  }
  for (auto& weight : kernel) {
    weight /= sum;
  }
  return kernel;
}

/**
 * @brief Feather and Expand of a whole @p width x @p height R8 image (lab function F8): alternating
 *        cross/square morphology steps, then a separable Gaussian on v / 255 in double. Replicated
 *        borders; each texel is `round(clamp(v, 0, 1) * 255)`.
 */
inline auto ReferenceAiMaskEdgeOnImage(std::span<const std::uint8_t> image, int width, int height,
                                       const AiMaskEdgeAdjustment& edge)
    -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> current(image.begin(), image.end());
  for (int step = 0; step < std::abs(edge.expand_px); ++step) {
    current = MorphologyStep(current, width, height, edge.expand_px > 0, step % 2 == 1);
  }
  if (edge.feather_px <= 0.0f) {
    return current;
  }
  const auto          kernel = ReferenceGaussianKernel(edge.feather_px);
  const int           radius = static_cast<int>(kernel.size()) / 2;
  const auto          index = [&](int x, int y) { return static_cast<std::size_t>(y) * width + x; };
  std::vector<double> rows(current.size());
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      double sum = 0.0;
      for (int k = -radius; k <= radius; ++k) {
        sum += kernel[static_cast<std::size_t>(k + radius)] *
               current[index(std::clamp(x + k, 0, width - 1), y)] / 255.0;
      }
      rows[index(x, y)] = sum;
    }
  }
  std::vector<std::uint8_t> output(current.size());
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      double sum = 0.0;
      for (int k = -radius; k <= radius; ++k) {
        sum += kernel[static_cast<std::size_t>(k + radius)] *
               rows[index(x, std::clamp(y + k, 0, height - 1))];
      }
      output[index(x, y)] =
          static_cast<std::uint8_t>(std::nearbyint(std::clamp(sum, 0.0, 1.0) * 255.0));
    }
  }
  return output;
}

/// The @p rect texels of a row-major image of width @p image_width.
inline auto CropRect(std::span<const std::uint8_t> image, int image_width, RectI rect)
    -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> crop;
  crop.reserve(static_cast<std::size_t>(rect.width) * rect.height);
  for (int y = rect.y; y < rect.Y1(); ++y) {
    for (int x = rect.x; x < rect.X1(); ++x) {
      crop.push_back(image[static_cast<std::size_t>(y) * image_width + x]);
    }
  }
  return crop;
}

/// An @p image_width x @p image_height image of zeros with @p pixels placed at @p rect.
inline auto PlaceInImage(std::span<const std::uint8_t> pixels, RectI rect, int image_width,
                         int image_height) -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> image(static_cast<std::size_t>(image_width) * image_height, 0);
  for (int y = 0; y < rect.height; ++y) {
    for (int x = 0; x < rect.width; ++x) {
      image[static_cast<std::size_t>(rect.y + y) * image_width + rect.x + x] =
          pixels[static_cast<std::size_t>(y) * rect.width + x];
    }
  }
  return image;
}

/**
 * @brief Host execution of @p dispatch as AiMaskEdgeDispatch describes it, in float like the
 *        devices. Host resource factories of the tests use it for RecordAiMaskEdge.
 */
inline auto ExecuteAiMaskEdgeDispatchOnHost(const AiMaskEdgeDispatch&     dispatch,
                                            std::span<const std::uint8_t> source)
    -> std::vector<std::uint8_t> {
  const int width  = static_cast<int>(dispatch.output_extent.width);
  const int height = static_cast<int>(dispatch.output_extent.height);
  if (source.size() !=
      static_cast<std::size_t>(dispatch.source_extent.width) * dispatch.source_extent.height) {
    throw std::runtime_error("the source does not have the size of the dispatch");
  }
  auto current = PlaceInImage(
      source,
      RectI{dispatch.source_x, dispatch.source_y, static_cast<int>(dispatch.source_extent.width),
            static_cast<int>(dispatch.source_extent.height)},
      width, height);
  for (int step = 0; step < dispatch.expand_steps; ++step) {
    current = MorphologyStep(current, width, height, dispatch.dilate, step % 2 == 1);
  }
  if (!dispatch.HasFeather()) {
    return current;
  }
  const auto&        w      = dispatch.feather_weights;
  const int          radius = static_cast<int>(w.size()) - 1;
  const auto         index  = [&](int x, int y) { return static_cast<std::size_t>(y) * width + x; };
  constexpr float    kScale = 1.0f / 255.0f;
  std::vector<float> rows(current.size());
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      float sum = w[0] * (current[index(x, y)] * kScale);
      for (int k = 1; k <= radius; ++k) {
        sum += w[static_cast<std::size_t>(k)] *
               (current[index(std::clamp(x - k, 0, width - 1), y)] * kScale +
                current[index(std::clamp(x + k, 0, width - 1), y)] * kScale);
      }
      rows[index(x, y)] = sum;
    }
  }
  std::vector<std::uint8_t> output(current.size());
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      float sum = w[0] * rows[index(x, y)];
      for (int k = 1; k <= radius; ++k) {
        sum += w[static_cast<std::size_t>(k)] * (rows[index(x, std::clamp(y - k, 0, height - 1))] +
                                                 rows[index(x, std::clamp(y + k, 0, height - 1))]);
      }
      output[index(x, y)] =
          static_cast<std::uint8_t>(std::clamp(std::nearbyint(sum * 255.0f), 0.0f, 255.0f));
    }
  }
  return output;
}

/// Largest absolute difference of two equal-size R8 buffers.
inline auto MaxR8Difference(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b)
    -> int {
  if (a.size() != b.size()) {
    throw std::runtime_error("MaxR8Difference: the sizes differ");
  }
  int largest = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    largest = std::max(largest, std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
  }
  return largest;
}

}  // namespace alcedo::ai_mask_test
