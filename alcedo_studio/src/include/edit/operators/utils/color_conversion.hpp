//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once
#include <opencv2/core.hpp>
#include <opencv2/core/mat.hpp>
#include <opencv2/imgproc.hpp>

namespace alcedo {
inline static void GPUCvtColor(cv::Mat& src, cv::Mat& dst, int code) {
  cv::UMat uSrc, uDst;
  src.copyTo(uSrc);
  cv::cvtColor(uSrc, uDst, code);
  uDst.copyTo(dst);
}

inline static void GPUCvtColor(cv::UMat& src, cv::UMat& dst, int code) {
  cv::cvtColor(src, dst, code);
}
};  // namespace alcedo
