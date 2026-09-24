//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/detail/sharpen_op.hpp"

#include <algorithm>
#include <cmath>
#include <opencv2/core.hpp>
#include <opencv2/core/base.hpp>
#include <opencv2/imgproc.hpp>

#include "edit/operators/op_kernel.hpp"
#include "edit/operators/operator_factory.hpp"
#include "image/image_buffer.hpp"
#include "json.hpp"

namespace alcedo {
namespace {

void BuildGaussianKernel(float sigma, int max_radius, int& tap_count,
                         float (&weights)[OperatorParams::kDetailMaxGaussianTapCount]) {
  std::fill_n(weights, OperatorParams::kDetailMaxGaussianTapCount, 0.0f);
  tap_count = 0;

  if (sigma <= 0.0f) {
    return;
  }

  const float safe_sigma = std::max(sigma, 1.0e-4f);
  const int   radius =
      std::clamp(static_cast<int>(std::ceil(3.0f * safe_sigma)), 1, max_radius);
  tap_count = std::min(radius + 1, OperatorParams::kDetailMaxGaussianTapCount);

  const double inv2sigma2  = 0.5 / (static_cast<double>(safe_sigma) * safe_sigma);
  double       full_weight = 1.0;
  weights[0]               = 1.0f;
  for (int tap = 1; tap < tap_count; ++tap) {
    const double w = std::exp(-(static_cast<double>(tap) * static_cast<double>(tap)) * inv2sigma2);
    weights[tap]   = static_cast<float>(w);
    full_weight += 2.0 * w;
  }

  if (full_weight > 0.0) {
    for (int tap = 0; tap < tap_count; ++tap) {
      weights[tap] = static_cast<float>(static_cast<double>(weights[tap]) / full_weight);
    }
  }
}

}  // namespace

SharpenOp::SharpenOp(float offset, float radius, float threshold)
    : offset_(offset), radius_(radius), threshold_(threshold) {
  ComputeScale();
  threshold_ /= 100.0f;
}

SharpenOp::SharpenOp(const nlohmann::json& params) { SetParams(params); }

void SharpenOp::ComputeScale() { scale_ = offset_ / 100.0f; }

auto SharpenOp::GetParams() const -> nlohmann::json {
  nlohmann::json o;
  nlohmann::json inner;

  inner["offset"]    = offset_;
  inner["radius"]    = radius_;
  inner["threshold"] = threshold_;

  o[script_name_]    = inner;
  return o;
}

void SharpenOp::SetParams(const nlohmann::json& params) {
  if (!params.contains(script_name_)) return;
  nlohmann::json inner = params[script_name_].get<nlohmann::json>();
  if (inner.contains("offset")) {
    offset_ = inner["offset"].get<float>();
  }
  if (inner.contains("radius")) {
    radius_ = inner["radius"].get<float>();
  } else {
    radius_ = 3.0f;
  }
  if (inner.contains("threshold")) {
    threshold_ = inner["threshold"].get<float>();
    threshold_ /= 100.0f;
  }
  ComputeScale();
}

void SharpenOp::Apply(std::shared_ptr<ImageBuffer> input) {
  cv::Mat& img = input->GetCPUData();

  // Use USM to sharpen the image
  cv::Mat  blurred;
  cv::GaussianBlur(img, blurred, cv::Size(), radius_, radius_, cv::BORDER_REPLICATE);

  cv::Mat high_pass = img - blurred;
  if (threshold_ > 0.0f) {
    cv::Mat high_pass_gray;
    cv::cvtColor(high_pass, high_pass_gray, cv::COLOR_BGR2GRAY);

    cv::Mat abs_high_pass_gray = cv::abs(high_pass_gray);

    cv::Mat mask;
    cv::threshold(abs_high_pass_gray, mask, threshold_, 1.0f, cv::THRESH_BINARY);

    cv::Mat mask_3channel;
    cv::cvtColor(mask, mask_3channel, cv::COLOR_GRAY2BGR);
    cv::multiply(high_pass, mask_3channel, high_pass);
  }

  cv::scaleAdd(high_pass, scale_, img, img);
  cv::threshold(img, img, 1.0f, 1.0f, cv::THRESH_TRUNC);
  cv::threshold(img, img, 0.0f, 0.0f, cv::THRESH_TOZERO);
}

void SharpenOp::ApplyGPU(std::shared_ptr<ImageBuffer>) {
  // GPU implementation not available yet.
  throw std::runtime_error("SharpenOp::ApplyGPU not implemented yet.");
}

void SharpenOp::SetGlobalParams(OperatorParams& params) const {
  params.sharpen_offset_    = scale_;
  params.sharpen_radius_    = radius_;
  params.sharpen_threshold_ = threshold_;
  BuildGaussianKernel(radius_, 15, params.sharpen_gaussian_tap_count_,
                      params.sharpen_gaussian_weights_);
}

void SharpenOp::EnableGlobalParams(OperatorParams& params, bool enable) {
  params.sharpen_enabled_ = enable;
}
};  // namespace alcedo
