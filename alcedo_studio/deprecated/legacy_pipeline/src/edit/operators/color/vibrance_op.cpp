//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/color/vibrance_op.hpp"

#include <algorithm>
#include <opencv2/core/mat.hpp>
#include <utility>

#include "edit/operators/color/conversion/Oklab_cvt.hpp"
#include "edit/operators/operator_factory.hpp"
#include "json.hpp"

namespace alcedo {

VibranceOp::VibranceOp() : vibrance_offset_(0) {}

VibranceOp::VibranceOp(float vibrance_offset) : vibrance_offset_(vibrance_offset) {}

VibranceOp::VibranceOp(const nlohmann::json& params) { SetParams(params); }

auto VibranceOp::ComputeScale(float chroma) -> float {
  // chroma in [0, max], vibrance_offset in [-100, 100]
  float strength = vibrance_offset_ / 100.0f;

  // Protect already highly saturated color
  float falloff  = std::exp(-3.0f * chroma);

  return 1.0f + strength * falloff;
}

void VibranceOp::Apply(std::shared_ptr<ImageBuffer> input) {
  cv::Mat& img = input->GetCPUData();

  img.forEach<cv::Vec3f>([&](cv::Vec3f& pixel, const int*) {
    // Adpated from https://github.com/tannerhelland/PhotoDemon
    float r = pixel[0], g = pixel[1], b = pixel[2];

    float max_val = std::max({r, g, b});
    float min_val = std::min({r, g, b});
    float chroma  = max_val - min_val;

    float scale   = ComputeScale(chroma);

    if (vibrance_offset_ >= 0.0f) {
      float luma = r * 0.299f + g * 0.587f + b * 0.114f;

      r          = luma + (r - luma) * scale;
      g          = luma + (g - luma) * scale;
      b          = luma + (b - luma) * scale;

    } else {
      float avg = (r + g + b) / 3.0f;
      r += (avg - r) * (1.0f - scale);
      g += (avg - g) * (1.0f - scale);
      b += (avg - b) * (1.0f - scale);
    }

    // clamp
    r     = std::clamp(r, 0.0f, 1.0f);
    g     = std::clamp(g, 0.0f, 1.0f);
    b     = std::clamp(b, 0.0f, 1.0f);

    pixel = {r, g, b};
  });
}

void VibranceOp::ApplyGPU(std::shared_ptr<ImageBuffer> input) {
  // GPU implementation can be added here in the future
  throw std::runtime_error("GPU implementation not available for VibranceOp yet.");
}

auto VibranceOp::GetParams() const -> nlohmann::json {
  nlohmann::json o;
  // SetParams divides the UI value by 100 for internal [-1, 1] range;
  // scale back to [-100, 100] so GetParams matches the submitted value.
  o[script_name_] = vibrance_offset_ * 100.0f;

  return o;
}

void VibranceOp::SetParams(const nlohmann::json& params) {
  if (params.contains(script_name_)) {
    vibrance_offset_ = params[script_name_].get<float>() / 100.0f;
  } else {
    vibrance_offset_ = 0.0f;
  }
}

void VibranceOp::SetGlobalParams(OperatorParams& params) const {
  params.vibrance_offset_ = vibrance_offset_;
}

void VibranceOp::EnableGlobalParams(OperatorParams& params, bool enable) {
  params.vibrance_enabled_ = enable;
}
};  // namespace alcedo