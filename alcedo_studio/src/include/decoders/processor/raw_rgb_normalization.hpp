//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <libraw/libraw.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <opencv2/core.hpp>
#include <stdexcept>

#include "decoders/processor/raw_rgb_linearization_params.hpp"

namespace alcedo::raw_norm {

/// Read post-unpack levels and WB state without inspecting the file or camera type.
inline auto BuildRgbLinearization(const libraw_colordata_t& color, bool integer_codes)
    -> RawRgbLinearizationParams {
  RawRgbLinearizationParams params;
  params.integer_codes = integer_codes ? 1U : 0U;
  for (int c = 0; c < 3; ++c) {
    if (!std::isfinite(color.cam_mul[c]) || color.cam_mul[c] <= 0.0f) {
      throw std::runtime_error("Decoded RGB requires positive finite camera white balance gains");
    }
  }
  for (int c = 0; c < 3; ++c) {
    const float gain = color.cam_mul[c] / color.cam_mul[1];
    if (!std::isfinite(gain) || gain <= 0.0f || !std::isfinite(1.0f / gain)) {
      throw std::runtime_error("Decoded RGB camera white balance ratio is not finite and positive");
    }
    if (integer_codes) {
      params.black[c]   = static_cast<float>(color.black) + static_cast<float>(color.cblack[c]);
      const float range = static_cast<float>(color.maximum) - params.black[c];
      if (!(range > 0.0f)) {
        throw std::runtime_error("Decoded RGB white level must exceed its black level");
      }
      params.scale[c] = 1.0f / range;
    }
    if ((color.as_shot_wb_applied & LIBRAW_ASWB_APPLIED) == 0) {
      params.scale[c] *= gain;
    }
    if (!std::isfinite(params.scale[c]) || params.scale[c] <= 0.0f) {
      throw std::runtime_error("Decoded RGB normalization gain is not positive and finite");
    }
  }
  return params;
}

/**
 * Convert unpacked integer RGB to float without applying white balance again.
 * Use the post-unpack black/cblack and maximum for every file format. Integer
 * storage does not imply a 0..65535 signal range. linear_max describes sensor
 * linearity limits and can remain unchanged when a decoder converts to RGB.
 */
inline auto ConvertUnpackedRgbToFloat(const cv::Mat& source, const libraw_colordata_t& color)
    -> cv::Mat {
  CV_Assert(source.type() == CV_16UC3 || source.type() == CV_16UC4);
  cv::Mat              result;
  std::array<float, 3> black{};
  std::array<float, 3> scale{};
  for (int c = 0; c < 3; ++c) {
    black[c]          = static_cast<float>(color.black) + static_cast<float>(color.cblack[c]);
    const float range = static_cast<float>(color.maximum) - black[c];
    if (!(range > 0.0f)) {
      throw std::runtime_error("Decoded RGB white level must exceed its black level");
    }
    scale[c] = 1.0f / range;
  }

  result.create(source.size(), CV_MAKETYPE(CV_32F, source.channels()));
  for (int y = 0; y < source.rows; ++y) {
    const auto* src = source.ptr<ushort>(y);
    auto*       dst = result.ptr<float>(y);
    for (int x = 0; x < source.cols; ++x) {
      const int offset = x * source.channels();
      for (int c = 0; c < 3; ++c) {
        // Keep highlight headroom above the decoder's estimated white point.
        dst[offset + c] = std::max(0.0f, (src[offset + c] - black[c]) * scale[c]);
      }
      if (source.channels() == 4) {
        dst[offset + 3] = 0.0f;  // LibRaw's unused fourth plane is not alpha.
      }
    }
  }
  return result;
}

}  // namespace alcedo::raw_norm
