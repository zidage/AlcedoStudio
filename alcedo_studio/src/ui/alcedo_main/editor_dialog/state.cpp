//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/editor_dialog/state.hpp"

#include <algorithm>
#include <cmath>

namespace alcedo::ui {

void UpdateCdlWheelDerivedColor(CdlWheelState& wheel, bool add_unity, bool invert_delta) {
  wheel.disc_position_ = color_wheel::ClampDiscPoint(wheel.disc_position_);
  wheel.strength_      = std::clamp(wheel.strength_, 0.0f, color_wheel::kStrengthDefault);
  const auto  delta    = color_wheel::DiscToCdlDelta(wheel.disc_position_, wheel.strength_);
  const float base     = add_unity ? 1.0f : 0.0f;
  const float sign     = invert_delta ? -1.0f : 1.0f;
  wheel.color_offset_  = {base + sign * delta[0], base + sign * delta[1], base + sign * delta[2]};
}

void UpdateAllCdlWheelDerivedColors(AdjustmentState& state) {
  UpdateCdlWheelDerivedColor(state.lift_wheel_, false, false);
  UpdateCdlWheelDerivedColor(state.gamma_wheel_, true, true);
  UpdateCdlWheelDerivedColor(state.gain_wheel_, true, false);
}

auto ActiveHlsProfileIndex(const AdjustmentState& state) -> int {
  return std::clamp(hls::ClosestCandidateHueIndex(state.hls_target_hue_), 0,
                    static_cast<int>(hls::kCandidateHues.size()) - 1);
}

void SaveActiveHlsProfile(AdjustmentState& state) {
  const int idx                           = ActiveHlsProfileIndex(state);
  state.hls_hue_adjust_table_[idx]        = state.hls_hue_adjust_;
  state.hls_lightness_adjust_table_[idx]  = state.hls_lightness_adjust_;
  state.hls_saturation_adjust_table_[idx] = state.hls_saturation_adjust_;
  state.hls_hue_range_table_[idx]         = state.hls_hue_range_;
}

void LoadActiveHlsProfile(AdjustmentState& state) {
  const int idx                = ActiveHlsProfileIndex(state);
  state.hls_hue_adjust_        = state.hls_hue_adjust_table_[idx];
  state.hls_lightness_adjust_  = state.hls_lightness_adjust_table_[idx];
  state.hls_saturation_adjust_ = state.hls_saturation_adjust_table_[idx];
  state.hls_hue_range_         = state.hls_hue_range_table_[idx];
}

auto ParseColorTempMode(const std::string& mode) -> ColorTempMode {
  if (mode == "custom") {
    return ColorTempMode::CUSTOM;
  }
  if (mode == "as-shot" || mode == "as_shot") {
    return ColorTempMode::AS_SHOT;
  }
  return ColorTempMode::AS_SHOT;
}

auto ColorTempModeToString(ColorTempMode mode) -> std::string {
  switch (mode) {
    case ColorTempMode::CUSTOM:
      return "custom";
    case ColorTempMode::AS_SHOT:
    default:
      return "as_shot";
  }
}

void CopyFieldState(AdjustmentField field, const AdjustmentState& from, AdjustmentState& to) {
  switch (field) {
    case AdjustmentField::Exposure:
      to.exposure_ = from.exposure_;
      return;
    case AdjustmentField::Contrast:
      to.contrast_ = from.contrast_;
      return;
    case AdjustmentField::Saturation:
      to.saturation_ = from.saturation_;
      return;
    case AdjustmentField::RawDecode:
      to.raw_demosaic_method_        = from.raw_demosaic_method_;
      to.raw_highlights_reconstruct_ = from.raw_highlights_reconstruct_;
      return;
    case AdjustmentField::LensCalib:
      to.lens_calib_enabled_  = from.lens_calib_enabled_;
      to.lens_override_make_  = from.lens_override_make_;
      to.lens_override_model_ = from.lens_override_model_;
      return;
    case AdjustmentField::ColorTemp:
      to.color_temp_mode_          = from.color_temp_mode_;
      to.color_temp_custom_cct_    = from.color_temp_custom_cct_;
      to.color_temp_custom_tint_   = from.color_temp_custom_tint_;
      to.color_temp_resolved_cct_  = from.color_temp_resolved_cct_;
      to.color_temp_resolved_tint_ = from.color_temp_resolved_tint_;
      to.color_temp_supported_     = from.color_temp_supported_;
      return;
    case AdjustmentField::Hls:
      to.hls_target_hue_              = from.hls_target_hue_;
      to.hls_hue_adjust_              = from.hls_hue_adjust_;
      to.hls_lightness_adjust_        = from.hls_lightness_adjust_;
      to.hls_saturation_adjust_       = from.hls_saturation_adjust_;
      to.hls_hue_range_               = from.hls_hue_range_;
      to.hls_hue_adjust_table_        = from.hls_hue_adjust_table_;
      to.hls_lightness_adjust_table_  = from.hls_lightness_adjust_table_;
      to.hls_saturation_adjust_table_ = from.hls_saturation_adjust_table_;
      to.hls_hue_range_table_         = from.hls_hue_range_table_;
      return;
    case AdjustmentField::ColorWheel:
      to.lift_wheel_  = from.lift_wheel_;
      to.gamma_wheel_ = from.gamma_wheel_;
      to.gain_wheel_  = from.gain_wheel_;
      return;
    case AdjustmentField::Blacks:
      to.blacks_ = from.blacks_;
      return;
    case AdjustmentField::Whites:
      to.whites_ = from.whites_;
      return;
    case AdjustmentField::Shadows:
      to.shadows_ = from.shadows_;
      return;
    case AdjustmentField::Highlights:
      to.highlights_ = from.highlights_;
      return;
    case AdjustmentField::Curve:
      to.curve_points_ = from.curve_points_;
      return;
    case AdjustmentField::Sharpen:
      to.sharpen_ = from.sharpen_;
      return;
    case AdjustmentField::Clarity:
      to.clarity_ = from.clarity_;
      return;
    case AdjustmentField::FilmGrain:
      to.film_grain_ = from.film_grain_;
      return;
    case AdjustmentField::Halation:
      to.halation_ = from.halation_;
      return;
    case AdjustmentField::Lut:
      to.lut_path_ = from.lut_path_;
      return;
    case AdjustmentField::Odt:
      to.odt_ = from.odt_;
      return;
    case AdjustmentField::CropRotate:
      to.rotate_degrees_     = from.rotate_degrees_;
      to.crop_enabled_       = from.crop_enabled_;
      to.crop_x_             = from.crop_x_;
      to.crop_y_             = from.crop_y_;
      to.crop_w_             = from.crop_w_;
      to.crop_h_             = from.crop_h_;
      to.crop_expand_to_fit_ = from.crop_expand_to_fit_;
      to.crop_aspect_preset_ = from.crop_aspect_preset_;
      to.crop_aspect_width_  = from.crop_aspect_width_;
      to.crop_aspect_height_ = from.crop_aspect_height_;
      return;
  }
}

}  // namespace alcedo::ui
