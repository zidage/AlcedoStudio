//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "edit/operators/op_base.hpp"

namespace alcedo {
struct SharpenOpKernel : NeighborOpTag {
  inline void operator()(Tile& in, OperatorParams& params) const {
    if (!params.sharpen_enabled_) return;
    cv::Mat tile_mat(in.height_, in.width_, CV_32FC4, in.ptr_);
    cv::Mat blurred;
    cv::GaussianBlur(tile_mat, blurred, cv::Size(), params.sharpen_radius_, params.sharpen_radius_,
                     cv::BORDER_REPLICATE);
    cv::Mat high_pass = tile_mat - blurred;
    if (params.sharpen_threshold_ > 0.0f) {
      cv::Mat high_pass_gray;
      cv::cvtColor(high_pass, high_pass_gray, cv::COLOR_BGR2GRAY);
      cv::Mat abs_high_pass_gray = cv::abs(high_pass_gray);
      cv::Mat mask;
      cv::threshold(abs_high_pass_gray, mask, params.sharpen_threshold_, 1.0f, cv::THRESH_BINARY);
      cv::Mat mask_3channel;
      cv::cvtColor(mask, mask_3channel, cv::COLOR_GRAY2BGR);
      cv::multiply(high_pass, mask_3channel, high_pass);
    }
    cv::scaleAdd(high_pass, params.sharpen_offset_, tile_mat, tile_mat);
  }
};
};  // namespace alcedo
