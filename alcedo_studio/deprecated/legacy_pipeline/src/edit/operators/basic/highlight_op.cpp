//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/basic/highlight_op.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "edit/operators/basic/shadows_highlights_shared_curve.hpp"
#include "edit/operators/op_base.hpp"
#include "image/image_buffer.hpp"

namespace alcedo {
HighlightsOp::HighlightsOp(float offset) : offset_(offset) {}

HighlightsOp::HighlightsOp(const nlohmann::json& params) { SetParams(params); }

static inline float clampf(float v, float a, float b) { return std::max(a, std::min(b, v)); }

namespace {
constexpr float kHighlightsAdjustmentStrengthScale = 1.5f;

void            BuildGaussianKernel(float sigma, int max_radius, int& tap_count,
                                    float (&weights)[OperatorParams::kDetailMaxGaussianTapCount]) {
  std::fill_n(weights, OperatorParams::kDetailMaxGaussianTapCount, 0.0f);
  tap_count = 0;
  if (sigma <= 0.0f) {
    return;
  }

  const float safe_sigma = std::max(sigma, 1.0e-4f);
  const int   radius = std::clamp(static_cast<int>(std::ceil(3.0f * safe_sigma)), 1, max_radius);
  tap_count = std::min(radius + 1, OperatorParams::kDetailMaxGaussianTapCount);

  const double inv2sigma2  = 0.5 / (static_cast<double>(safe_sigma) * safe_sigma);
  double       full_weight = 1.0;
  weights[0]               = 1.0f;
  for (int tap = 1; tap < tap_count; ++tap) {
    const double w = std::exp(-(static_cast<double>(tap) * static_cast<double>(tap)) * inv2sigma2);
    weights[tap] = static_cast<float>(w);
    full_weight += 2.0 * w;
  }
  if (full_weight > 0.0) {
    for (int tap = 0; tap < tap_count; ++tap) {
      weights[tap] = static_cast<float>(static_cast<double>(weights[tap]) / full_weight);
    }
  }
}

void UpdateHsLocalTonePayload(OperatorParams& params) {
  auto& tone                   = params.tone_mapping_;
  tone.local_tone_enabled_     = true;
  tone.local_radius_           = 18.0f;
  tone.shadow_log_pivot_       = -3.35f;
  tone.shadow_log_width_       = 0.62f;
  tone.highlight_log_pivot_    = -2.80f;
  tone.highlight_log_width_    = 3.65f;
  tone.preserve_source_detail_ = params.render_hs_preserve_source_detail_;
  tone.roi_enabled_            = params.render_roi_enabled_;
  tone.roi_x_                  = params.render_roi_x_;
  tone.roi_y_                  = params.render_roi_y_;
  tone.roi_scale_x_            = params.render_roi_scale_x_;
  tone.roi_scale_y_            = params.render_roi_scale_y_;
  tone.roi_reference_width_    = params.render_roi_reference_width_;
  tone.roi_reference_height_   = params.render_roi_reference_height_;
  BuildGaussianKernel(tone.local_radius_, 48, tone.base_gaussian_tap_count_,
                      tone.base_gaussian_weights_);

  params.hs_local_tone_enabled_      = tone.local_tone_enabled_;
  params.hs_base_radius_             = tone.local_radius_;
  params.hs_base_gaussian_tap_count_ = tone.base_gaussian_tap_count_;
  std::copy_n(tone.base_gaussian_weights_, OperatorParams::kDetailMaxGaussianTapCount,
              params.hs_base_gaussian_weights_);
  params.hs_shadow_log_pivot_    = tone.shadow_log_pivot_;
  params.hs_shadow_log_width_    = tone.shadow_log_width_;
  params.hs_highlight_log_pivot_ = tone.highlight_log_pivot_;
  params.hs_highlight_log_width_ = tone.highlight_log_width_;
}

void UpdateSharedToneCurvePayload(OperatorParams& params) {
  const auto& tone          = params.tone_mapping_;
  const bool shadows_active = tone.slider_input_.shadows_operator_present_ && tone.shadows_enabled_;
  const bool highlights_active =
      tone.slider_input_.highlights_operator_present_ && tone.highlights_enabled_;
  const float scaled_highlights_slider_value =
      tone.slider_input_.highlights_slider_value_ * kHighlightsAdjustmentStrengthScale;
  const auto curve =
      detail::BuildSharedToneCurve(shadows_active, tone.slider_input_.shadows_slider_value_,
                                   highlights_active, scaled_highlights_slider_value);
  detail::StoreSharedToneCurve(curve, params);
  params.shared_tone_curve_apply_in_shadows_    = shadows_active;
  params.shared_tone_curve_apply_in_highlights_ = (!shadows_active) && highlights_active;
}
}  // namespace

// Luminance (linear, BGR)

auto HighlightsOp::GetScale() -> float { return offset_ / 300.0f; }

void HighlightsOp::Apply(std::shared_ptr<ImageBuffer> input) { (void)input; }

void HighlightsOp::ApplyGPU(std::shared_ptr<ImageBuffer>) {
  throw std::runtime_error("HighlightsOp: ApplyGPU not implemented");
}

auto HighlightsOp::GetParams() const -> nlohmann::json {
  return {{std::string(script_name_), offset_}};
}

void HighlightsOp::SetParams(const nlohmann::json& params) {
  bool found = false;
  if (params.is_object() && params.contains(script_name_)) {
    offset_ = params[script_name_].get<float>();
    found   = true;
  } else if (params.is_array() && params.size() == 2) {
    // Backward compatibility for legacy snapshots serialized as ["highlights", value].
    try {
      if (params[0].is_string() && params[0].get<std::string>() == script_name_) {
        offset_ = params[1].get<float>();
        found   = true;
      }
    } catch (...) {
    }
  }
  if (!found) {
    offset_ = 0.0f;
  }
  const float scaled_offset = offset_ * kHighlightsAdjustmentStrengthScale;
  curve_.control_           = clampf(scaled_offset / 50.0f, -2.0f, 2.0f);
  const float c             = std::max(0.0f, curve_.control_);
  curve_.knee_start_        = clampf(0.75f + 0.1f * c, 0.0f, 0.95f);
  // curve_.knee_start_ = clampf(0.8f, 0.0f, 1.0f);  // ensure <= whitepoint
  // map control -> slope at whitepoint (m1)
  // design: control = +1 => strong compression (m1 -> small, e.g. 0.2)
  //         control =  0 => identity slope (1.0)
  //         control = -1 => boost highlights (m1 -> >1, e.g. 1.8)
  curve_.m1_ = 1.0f - curve_.control_ * curve_.slope_range_;  // in [1-slope_range, 1+slope_range]

  // endpoints for Hermite between x0 = knee_start, x1 = whitepoint
  curve_.x0_ = curve_.knee_start_;
  curve_.y0_ = curve_.x0_;  // keep continuity (identity at x0)
  curve_.y1_ = curve_.x1_;  // identity at x1 (we'll control slope to shape shoulder)

  // For Hermite formula we need derivatives dy/dx at endpoints.
  // m0 and m1 are dy/dx at x0 and x1 respectively.
  // But Hermite cubic uses tangents scaled by (x1-x0) in the basis:
  curve_.dx_ = (curve_.x1_ - curve_.x0_);
}

void HighlightsOp::SetGlobalParams(OperatorParams& params) const {
  auto& tone                                      = params.tone_mapping_;
  tone.slider_input_.highlights_operator_present_ = true;
  tone.slider_input_.highlights_slider_value_     = offset_;
  tone.highlights_enabled_                        = params.highlights_enabled_;
  tone.highlight_amount_              = (offset_ * kHighlightsAdjustmentStrengthScale) / 100.0f;

  params.highlights_operator_present_ = tone.slider_input_.highlights_operator_present_;
  params.highlights_slider_value_     = tone.slider_input_.highlights_slider_value_;
  params.highlights_offset_           = tone.highlight_amount_;
  params.highlights_m1_               = curve_.m1_;
  UpdateSharedToneCurvePayload(params);
  UpdateHsLocalTonePayload(params);
}

void HighlightsOp::EnableGlobalParams(OperatorParams& params, bool enable) {
  params.highlights_enabled_               = enable;
  params.tone_mapping_.highlights_enabled_ = enable;
}
}  // namespace alcedo
