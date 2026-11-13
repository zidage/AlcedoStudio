//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/basic/contrast_op.hpp"

#include <opencv2/core/hal/interface.h>

#include <opencv2/core.hpp>
#include <stdexcept>

#include "edit/operators/color/conversion/Oklab_cvt.hpp"
#include "edit/operators/operator_factory.hpp"

namespace alcedo {

/**
 * @brief Default construct a new Contrast Op:: Contrast Op object
 *
 */
ContrastOp::ContrastOp() : contrast_offset_(0.0f) { scale_ = 1.0f; }

/**
 * @brief Construct a new Contrast Op:: Contrast Op object
 *
 * @param contrast_offset
 */
ContrastOp::ContrastOp(float contrast_offset) : contrast_offset_(contrast_offset) {
  scale_ = std::exp(contrast_offset / 100.0f);
}

ContrastOp::ContrastOp(const nlohmann::json& params) { SetParams(params); }

/**
 * @brief Apply the contrast adjustment
 *
 * @param input
 * @return ImageBuffer
 */
void ContrastOp::Apply(std::shared_ptr<ImageBuffer> input) {
  cv::Mat& linear_image = input->GetCPUData();

  linear_image.forEach<cv::Vec3f>([this](cv::Vec3f& pixel, const int*) -> void {
    auto lab = OklabCvt::ACESRGB2Oklab(pixel);
    lab.l_    = (lab.l_ - 0.5f) * scale_ + 0.5f;
    pixel    = OklabCvt::Oklab2ACESRGB(lab);
  });

  // linear_image          = (linear_image - 0.5f) * _scale + 0.5f;
  // clamp
  // cv::min(linear_image, 100.0f, linear_image);
  // cv::max(linear_image, 0.0f, linear_image);
}

void ContrastOp::ApplyGPU(std::shared_ptr<ImageBuffer>) {
  throw std::runtime_error("ContrastOp: ApplyGPU not implemented");
}

auto ContrastOp::GetParams() const -> nlohmann::json {
  nlohmann::json o;
  o[GetScriptName()] = contrast_offset_;
  return o;
}

inline float contrast_k_from_slider(int v) // v in [-100,100]
{
    float s = std::max(-99, std::min(100, v)) / 100.0f; // [-1,1]
    s = s * fabsf(s);  // cubic-ish, center softer
    return 4.0f + 4.0f * s;
}

void ContrastOp::SetParams(const nlohmann::json& params) {
  if (params.contains(GetScriptName())) {
    contrast_offset_ = params[GetScriptName()];
  } else {
    contrast_offset_ = 0.0f;
  }
  scale_ = contrast_k_from_slider(static_cast<int>(contrast_offset_));
}

void ContrastOp::SetGlobalParams(OperatorParams& params) const {
  // Should only be called once SetParams has been called
  params.contrast_scale_ = scale_;
}

void ContrastOp::EnableGlobalParams(OperatorParams& params, bool enable) {
  // Contrast op is still broken, so disable it for now
  params.contrast_enabled_ = enable;
}
};  // namespace alcedo
