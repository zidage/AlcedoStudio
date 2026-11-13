//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/color/tint_op.hpp"

#include <memory>
#include <opencv2/core.hpp>
#include <opencv2/core/mat.hpp>
#include <opencv2/imgproc.hpp>
#include <vector>

#include "edit/operators/operator_factory.hpp"
#include "json.hpp"

namespace alcedo {

TintOp::TintOp() : tint_offset_(0.0f) { scale_ = 0.0f; }

TintOp::TintOp(float tint_offset) : tint_offset_(tint_offset) {
  // In OpenCV, the value of a channel lies between -127 to 127
  scale_ = tint_offset / 1000.0f;
}

TintOp::TintOp(const nlohmann::json& params) { SetParams(params); }

void TintOp::Apply(std::shared_ptr<ImageBuffer> input) {
  cv::Mat&             img = input->GetCPUData();
  std::vector<cv::Mat> bgr_channels;

  cv::split(img, bgr_channels);

  bgr_channels[1] += scale_;
  // Thresholding
  cv::threshold(bgr_channels[1], bgr_channels[1], 1.0f, 1.0f, cv::THRESH_TRUNC);
  cv::threshold(bgr_channels[1], bgr_channels[1], 0.0f, 0.0f, cv::THRESH_TOZERO);

  cv::merge(bgr_channels, img);
}

void TintOp::ApplyGPU(std::shared_ptr<ImageBuffer> input) {
  // GPU implementation can be added here in the future
  throw std::runtime_error("GPU implementation not available for TintOp yet.");
}

auto TintOp::GetParams() const -> nlohmann::json {
  nlohmann::json o;
  o[script_name_] = tint_offset_;
  return o;
}

void TintOp::SetParams(const nlohmann::json& params) {
  if (params.contains(script_name_)) {
    tint_offset_ = params.at(script_name_).get<float>();
    scale_       = tint_offset_ / 5000.0f;
  } else {
    tint_offset_ = 0.0f;
    scale_       = 0.0f;
  }
}

void TintOp::SetGlobalParams(OperatorParams& params) const { params.tint_offset_ = scale_; }

void TintOp::EnableGlobalParams(OperatorParams& params, bool enable) {
  params.tint_enabled_ = enable;
}
};  // namespace alcedo
