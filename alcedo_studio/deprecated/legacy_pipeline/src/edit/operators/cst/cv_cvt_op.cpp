//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/cst/cv_cvt_op.hpp"

namespace alcedo {
CVCvtColorOp::CVCvtColorOp(int code, std::optional<size_t> channel_index)
    : code_(code), channel_index_(channel_index) {}

CVCvtColorOp::CVCvtColorOp(const nlohmann::json& params) { SetParams(params); }

void CVCvtColorOp::Apply(std::shared_ptr<ImageBuffer> input) {
  cv::Mat& img = input->GetCPUData();
  cv::UMat src;
  img.copyTo(src);

  if (img.empty()) {
    throw std::runtime_error("CVCvtColorOp: Input image is empty");
  }
  cv::cvtColor(src, src, code_);

  cv::Mat dst;
  src.copyTo(dst);

  if (channel_index_.has_value()) {
    std::vector<cv::Mat> channels;
    cv::split(dst, channels);
    img = channels.at(channel_index_.value());
  }
}
};  // namespace alcedo