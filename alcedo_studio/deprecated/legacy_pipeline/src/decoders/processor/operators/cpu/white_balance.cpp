//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// FIXME: What this file does is not just white balance, but also black level correction and
// scaling. Should rename it to something more appropriate.

#include "decoders/processor/operators/cpu/white_balance.hpp"

#include <cstdint>

#include "decoders/processor/raw_normalization.hpp"

namespace alcedo {
namespace CPU {
static auto GetWBCoeff(const libraw_rawdata_t& raw_data) -> const float* {
  return raw_data.color.cam_mul;
}

void ToLinearRef(cv::Mat& img, LibRaw& raw_processor) {
  const auto raw_curve = raw_norm::BuildLinearizationCurve(raw_processor.imgdata.rawdata);
  const auto wb        = GetWBCoeff(raw_processor.imgdata.rawdata);
  const bool apply_wb = (raw_processor.imgdata.color.as_shot_wb_applied & LIBRAW_ASWB_APPLIED) == 0;
  const bool is_u16    = img.type() == CV_16UC1;
  const int  w         = img.cols;
  const int  h         = img.rows;

  CV_Assert(is_u16 || img.type() == CV_32FC1);

  cv::Mat linearized(h, w, CV_32FC1);

#pragma omp parallel for schedule(dynamic)
  for (int y = 0; y < h; ++y) {
    float* dst_row = linearized.ptr<float>(y);
    for (int x = 0; x < w; ++x) {
      const int color_idx = raw_processor.COLOR(y, x);
      const float sample = is_u16 ? static_cast<float>(img.at<uint16_t>(y, x)) : img.at<float>(y, x);
      const float black = raw_curve.black_level[color_idx] +
                          raw_norm::PatternBlackAt(raw_processor.imgdata.rawdata, y, x);
      float pixel = raw_norm::NormalizeSample(sample, black, raw_curve.white_level[color_idx]);
      pixel *= raw_norm::RelativeWhiteBalanceMultiplier(wb, color_idx, apply_wb);
      dst_row[x] = pixel;
    }
  }

  img = std::move(linearized);
}

};  // namespace CPU
};  // namespace alcedo
