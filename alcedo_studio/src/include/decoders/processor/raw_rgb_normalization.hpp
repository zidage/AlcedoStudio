//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <libraw/libraw.h>

#include <algorithm>
#include <array>
#include <opencv2/core.hpp>
#include <stdexcept>

namespace alcedo::raw_norm {

/**
 * Convert unpacked integer RGB to float without applying white balance again.
 * Sony YCbCr decoders leave a black offset in RGB and publish its decoded range
 * in black/maximum. Their linear_max still describes the original metadata range.
 * Other integer RGB inputs retain the existing full-range conversion.
 */
inline auto ConvertUnpackedRgbToFloat(const cv::Mat& source, const libraw_colordata_t& color)
    -> cv::Mat {
  CV_Assert(source.type() == CV_16UC3 || source.type() == CV_16UC4);
  cv::Mat result;
  if ((color.as_shot_wb_applied & LIBRAW_ASWB_SONY) == 0) {
    source.convertTo(result, CV_32F, 1.0 / 65535.0);
    return result;
  }

  std::array<float, 3> black{};
  std::array<float, 3> scale{};
  for (int c = 0; c < 3; ++c) {
    black[c]          = static_cast<float>(color.black) + static_cast<float>(color.cblack[c]);
    const float range = static_cast<float>(color.maximum) - black[c];
    if (!(range > 0.0f)) {
      throw std::runtime_error("Sony decoded RGB white level must exceed its black level");
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
