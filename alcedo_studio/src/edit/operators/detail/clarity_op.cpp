//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/detail/clarity_op.hpp"

#include <opencv2/core/hal/interface.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <opencv2/core.hpp>
#include <opencv2/core/mat.hpp>
#include <opencv2/core/types.hpp>
#include <opencv2/imgproc.hpp>
#include <vector>

#include "edit/operators/curve/curve_op.hpp"
#include "edit/operators/op_kernel.hpp"
#include "edit/operators/operator_factory.hpp"

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
  const int   radius = std::clamp(static_cast<int>(std::ceil(3.0f * safe_sigma)), 1, max_radius);
  tap_count          = std::min(radius + 1, OperatorParams::kDetailMaxGaussianTapCount);

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

float ClarityOp::usm_radius_ = 15.0f;

ClarityOp::ClarityOp() : clarity_offset_(0) { scale_ = 1.0f; }
ClarityOp::ClarityOp(float clarity_offset) : clarity_offset_(clarity_offset) {
  scale_ = clarity_offset / 200.0f;
}

ClarityOp::ClarityOp(const nlohmann::json& params) { SetParams(params); }

void ClarityOp::CreateMidtoneMask(cv::Mat& input, cv::Mat& mask) const {
  cv::Mat luminosity_mask;
  cv::cvtColor(input, luminosity_mask, cv::COLOR_BGR2GRAY);

  // Apply a "U" shape curve
  luminosity_mask = luminosity_mask - 0.5f;
  luminosity_mask = luminosity_mask * 2.0f;
  cv::pow(luminosity_mask, 2.0, luminosity_mask);
  mask = 1.0f - luminosity_mask;

  // if (_blur_sigma > 0) {
  //   cv::GaussianBlur(mask, mask, cv::Size(), _blur_sigma, _blur_sigma);
  // }
}

void ClarityOp::Apply(std::shared_ptr<ImageBuffer> input) {
  // Local contrast enhancement ("Clarity"):
  // 1. Blur with a medium-large radius to obtain local mean.
  // 2. diff = original - local_mean  (local contrast signal)
  // 3. Protect strong edges and highlights/shadows.
  // 4. Gently boost diff in mid-tones / flat areas.
  cv::Mat& img = input->GetCPUData();

  cv::Mat  blurred;
  cv::GaussianBlur(img, blurred, cv::Size(), usm_radius_, usm_radius_, cv::BORDER_REFLECT101);

  const bool      continuous     = img.isContinuous() && blurred.isContinuous();
  const int       rows           = img.rows;
  const int       cols           = img.cols;
  constexpr float kEdgeThreshold = 0.18f;

  if (continuous) {
    const int total    = rows * cols;
    auto*     img_ptr  = img.ptr<cv::Vec3f>();
    auto*     blur_ptr = blurred.ptr<cv::Vec3f>();
    for (int i = 0; i < total; ++i) {
      const cv::Vec3f& orig     = img_ptr[i];
      const cv::Vec3f& blur     = blur_ptr[i];

      cv::Vec3f        diff     = orig - blur;
      float            diff_lum = diff[0] * 0.114f + diff[1] * 0.587f + diff[2] * 0.299f;
      float            edge_mag = std::abs(diff_lum);
      float            t_edge   = std::min(edge_mag / kEdgeThreshold, 1.0f);
      float            protect  = 1.0f - t_edge * t_edge * (3.0f - 2.0f * t_edge);

      float            lum      = orig[0] * 0.114f + orig[1] * 0.587f + orig[2] * 0.299f;
      float            t_lum    = (lum - 0.5f) * 2.0f;
      float            mask     = 1.0f - t_lum * t_lum;
      mask                      = std::max(mask, 0.0f);

      float strength            = scale_ * protect * mask;

      img_ptr[i][0] += diff[0] * strength;
      img_ptr[i][1] += diff[1] * strength;
      img_ptr[i][2] += diff[2] * strength;
    }
  } else {
    for (int r = 0; r < rows; ++r) {
      auto* img_ptr  = img.ptr<cv::Vec3f>(r);
      auto* blur_ptr = blurred.ptr<cv::Vec3f>(r);
      for (int c = 0; c < cols; ++c) {
        const cv::Vec3f& orig     = img_ptr[c];
        const cv::Vec3f& blur     = blur_ptr[c];

        cv::Vec3f        diff     = orig - blur;
        float            diff_lum = diff[0] * 0.114f + diff[1] * 0.587f + diff[2] * 0.299f;
        float            edge_mag = std::abs(diff_lum);
        float            t_edge   = std::min(edge_mag / kEdgeThreshold, 1.0f);
        float            protect  = 1.0f - t_edge * t_edge * (3.0f - 2.0f * t_edge);

        float            lum      = orig[0] * 0.114f + orig[1] * 0.587f + orig[2] * 0.299f;
        float            t_lum    = (lum - 0.5f) * 2.0f;
        float            mask     = 1.0f - t_lum * t_lum;
        mask                      = std::max(mask, 0.0f);

        float strength            = scale_ * protect * mask;

        img_ptr[c][0] += diff[0] * strength;
        img_ptr[c][1] += diff[1] * strength;
        img_ptr[c][2] += diff[2] * strength;
      }
    }
  }
}

void ClarityOp::ApplyGPU(std::shared_ptr<ImageBuffer>) {
  // GPU implementation not provided
  throw std::runtime_error("ClarityOp: ApplyGPU not implemented");
}

auto ClarityOp::GetParams() const -> nlohmann::json {
  nlohmann::json o;
  o[script_name_] = clarity_offset_;

  return o;
}

void ClarityOp::SetParams(const nlohmann::json& params) {
  if (params.contains(script_name_)) {
    clarity_offset_ = params[script_name_];
  } else {
    clarity_offset_ = 0.0f;
  }
  scale_ = clarity_offset_ / 100.0f;
}

void ClarityOp::SetGlobalParams(OperatorParams& params) const {
  auto& tone                                   = params.tone_mapping_;
  tone.slider_input_.clarity_operator_present_ = true;
  tone.slider_input_.clarity_slider_value_     = clarity_offset_;
  tone.clarity_enabled_                        = params.clarity_enabled_;
  tone.clarity_amount_                         = scale_;

  params.clarity_offset_                       = tone.clarity_amount_;
  params.clarity_radius_                       = usm_radius_;
  BuildGaussianKernel(usm_radius_, 60, params.clarity_gaussian_tap_count_,
                      params.clarity_gaussian_weights_);
}

void ClarityOp::EnableGlobalParams(OperatorParams& params, bool enable) {
  params.clarity_enabled_               = enable;
  params.tone_mapping_.clarity_enabled_ = enable;
}
};  // namespace alcedo
