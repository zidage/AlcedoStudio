//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QColor>
#include <QPointF>
#include <QString>
#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "edit/operators/cst/open_drt_cpu.hpp"
#include "edit/operators/op_base.hpp"
#include "edit/pipeline/default_pipeline_params.hpp"
#include "renderer/pipeline_task.hpp"
#include "ui/alcedo_main/editor_dialog/modules/color_temp.hpp"
#include "ui/alcedo_main/editor_dialog/modules/color_wheel.hpp"
#include "ui/alcedo_main/editor_dialog/modules/curve.hpp"
#include "ui/alcedo_main/editor_dialog/modules/geometry.hpp"
#include "ui/alcedo_main/editor_dialog/modules/hls.hpp"
#include "ui/alcedo_main/editor_dialog/state/raw_demosaic_method_selection.hpp"

namespace alcedo::ui {

// ---------------------------------------------------------------------------
//  Timing
// ---------------------------------------------------------------------------

struct EditorDialogTiming {
  std::chrono::milliseconds fast_preview_min_submit_interval_{16};
  std::chrono::milliseconds quality_preview_debounce_interval_{180};
};

// ---------------------------------------------------------------------------
//  Enums
// ---------------------------------------------------------------------------

enum class ControlPanelKind { Tone, Look, DisplayRenderingTransform, Geometry, RawDecode };

enum class AdjustmentField {
  Exposure,
  Contrast,
  Saturation,
  RawDecode,
  LensCalib,
  ColorTemp,
  Hls,
  ColorWheel,
  Blacks,
  Whites,
  Shadows,
  Highlights,
  Curve,
  Sharpen,
  Clarity,
  FilmGrain,
  Halation,
  Lut,
  Odt,
  CropRotate,
};

// ---------------------------------------------------------------------------
//  CDL Wheel State
// ---------------------------------------------------------------------------

struct CdlWheelState {
  QPointF              disc_position_ = QPointF(0.0, 0.0);
  float                master_offset_ = 0.0f;
  std::array<float, 3> color_offset_  = {0.0f, 0.0f, 0.0f};
  float                strength_      = color_wheel::kStrengthDefault;
};

inline auto DefaultLiftWheelState() -> CdlWheelState {
  CdlWheelState wheel;
  wheel.color_offset_ = {0.0f, 0.0f, 0.0f};
  return wheel;
}

inline auto DefaultGammaGainWheelState() -> CdlWheelState {
  CdlWheelState wheel;
  wheel.color_offset_ = {1.0f, 1.0f, 1.0f};
  return wheel;
}

struct AcesOdtState {
  ColorUtils::ColorSpace limiting_space_ = ColorUtils::ColorSpace::REC709;
};

using OpenDrtOdtState = odt_cpu::OpenDRTSettings;

struct OdtState {
  ColorUtils::ODTMethod  method_         = ColorUtils::ODTMethod::OPEN_DRT;
  ColorUtils::ColorSpace encoding_space_ = ColorUtils::ColorSpace::REC709;
  ColorUtils::EOTF       encoding_eotf_  = ColorUtils::EOTF::GAMMA_2_2;
  float                  peak_luminance_ = 100.0f;
  AcesOdtState           aces_           = {};
  OpenDrtOdtState        open_drt_       = {};
};

// ---------------------------------------------------------------------------
//  Adjustment State — full editor snapshot
// ---------------------------------------------------------------------------

struct AdjustmentState {
  float                      exposure_            = pipeline_defaults::kCleanBaselineExposure;
  float                      contrast_            = 0.0f;
  float                      saturation_          = pipeline_defaults::kCleanBaselineSaturation;
  RawDemosaicMethodSelection raw_demosaic_method_ = RawDemosaicMethodSelection::Default;
  bool                       raw_highlights_reconstruct_ = true;
  bool                 lens_calib_enabled_ = pipeline_defaults::kCleanBaselineLensCalibEnabled;
  std::string          lens_override_make_{};
  std::string          lens_override_model_{};
  ColorTempMode        color_temp_mode_             = ColorTempMode::AS_SHOT;
  float                color_temp_custom_cct_       = 6500.0f;
  float                color_temp_custom_tint_      = 0.0f;
  float                color_temp_resolved_cct_     = 6500.0f;
  float                color_temp_resolved_tint_    = 0.0f;
  bool                 color_temp_supported_        = true;
  float                hls_target_hue_              = 0.0f;
  float                hls_hue_adjust_              = 0.0f;
  float                hls_lightness_adjust_        = 0.0f;
  float                hls_saturation_adjust_       = 0.0f;
  float                hls_hue_range_               = hls::kDefaultHueRange;
  CdlWheelState        lift_wheel_                  = DefaultLiftWheelState();
  CdlWheelState        gamma_wheel_                 = DefaultGammaGainWheelState();
  CdlWheelState        gain_wheel_                  = DefaultGammaGainWheelState();
  hls::HlsProfileArray hls_hue_adjust_table_        = {};
  hls::HlsProfileArray hls_lightness_adjust_table_  = {};
  hls::HlsProfileArray hls_saturation_adjust_table_ = {};
  hls::HlsProfileArray hls_hue_range_table_         = hls::MakeFilledArray(hls::kDefaultHueRange);
  float                blacks_                      = 0.0f;
  float                whites_                      = 0.0f;
  float                shadows_                     = 0.0f;
  float                highlights_                  = 0.0f;
  std::vector<QPointF> curve_points_                = curve::DefaultCurveControlPoints();
  float                sharpen_                     = 0.0f;
  float                clarity_                     = 0.0f;
  float                film_grain_                  = 0.0f;
  float                halation_                    = 0.0f;
  OdtState             odt_                         = {};
  float                rotate_degrees_              = 0.0f;
  bool                 crop_enabled_                = true;
  float                crop_x_                      = 0.0f;
  float                crop_y_                      = 0.0f;
  float                crop_w_                      = 1.0f;
  float                crop_h_                      = 1.0f;
  bool                 crop_expand_to_fit_          = true;
  geometry::CropAspectPreset crop_aspect_preset_    = geometry::CropAspectPreset::Free;
  float                      crop_aspect_width_     = 1.0f;
  float                      crop_aspect_height_    = 1.0f;
  std::string                lut_path_;
  RenderType                 type_ = RenderType::FAST_PREVIEW;
};

struct PendingRenderRequest {
  AdjustmentState      state_;
  FramePreviewMetadata frame_metadata_{};
  bool                 apply_state_         = true;
  bool                 use_viewport_region_ = true;
};

// ---------------------------------------------------------------------------
//  Color-temp request snapshot
// ---------------------------------------------------------------------------

constexpr float kColorTempRequestEpsilon = 1e-3f;

struct ColorTempRequestSnapshot {
  ColorTempMode mode_ = ColorTempMode::AS_SHOT;
  float         cct_  = 6500.0f;
  float         tint_ = 0.0f;
};

inline auto BuildColorTempRequest(const AdjustmentState& state) -> ColorTempRequestSnapshot {
  return {state.color_temp_mode_, state.color_temp_custom_cct_, state.color_temp_custom_tint_};
}

inline auto ColorTempRequestEqual(const ColorTempRequestSnapshot& a,
                                  const ColorTempRequestSnapshot& b) -> bool {
  return a.mode_ == b.mode_ && std::abs(a.cct_ - b.cct_) <= kColorTempRequestEpsilon &&
         std::abs(a.tint_ - b.tint_) <= kColorTempRequestEpsilon;
}

// ---------------------------------------------------------------------------
//  Free-function helpers on state types
// ---------------------------------------------------------------------------

inline bool NearlyEqual(float a, float b) { return std::abs(a - b) <= 1e-6f; }

void UpdateCdlWheelDerivedColor(CdlWheelState& wheel, bool add_unity, bool invert_delta = false);
void UpdateAllCdlWheelDerivedColors(AdjustmentState& state);

inline auto DisplayWheelDelta(const CdlWheelState& wheel, bool add_unity) -> std::array<float, 3> {
  const float master = wheel.master_offset_;
  if (add_unity) {
    return {wheel.color_offset_[0] - 1.0f + master, wheel.color_offset_[1] - 1.0f + master,
            wheel.color_offset_[2] - 1.0f + master};
  }
  return {wheel.color_offset_[0] + master, wheel.color_offset_[1] + master,
          wheel.color_offset_[2] + master};
}

inline auto FormatSigned3(float value) -> QString {
  if (value >= 0.0f) {
    return QString("+%1").arg(value, 0, 'f', 3);
  }
  return QString::number(value, 'f', 3);
}

inline auto FormatWheelDeltaText(const CdlWheelState& wheel, bool add_unity) -> QString {
  const auto delta = DisplayWheelDelta(wheel, add_unity);
  return QString("R %1  G %2  B %3")
      .arg(FormatSigned3(delta[0]), FormatSigned3(delta[1]), FormatSigned3(delta[2]));
}

auto        ActiveHlsProfileIndex(const AdjustmentState& state) -> int;
void        SaveActiveHlsProfile(AdjustmentState& state);
void        LoadActiveHlsProfile(AdjustmentState& state);

auto        ParseColorTempMode(const std::string& mode) -> ColorTempMode;
auto        ColorTempModeToString(ColorTempMode mode) -> std::string;

inline auto ColorTempModeToComboIndex(ColorTempMode mode) -> int {
  return mode == ColorTempMode::CUSTOM ? 1 : 0;
}

inline auto ComboIndexToColorTempMode(int index) -> ColorTempMode {
  return index == 1 ? ColorTempMode::CUSTOM : ColorTempMode::AS_SHOT;
}

inline auto DisplayedColorTempCct(const AdjustmentState& state) -> float {
  const float cct = (state.color_temp_mode_ == ColorTempMode::AS_SHOT)
                        ? state.color_temp_resolved_cct_
                        : state.color_temp_custom_cct_;
  return std::clamp(cct, static_cast<float>(color_temp::kCctMin),
                    static_cast<float>(color_temp::kCctMax));
}

inline auto DisplayedColorTempTint(const AdjustmentState& state) -> float {
  const float tint = (state.color_temp_mode_ == ColorTempMode::AS_SHOT)
                         ? state.color_temp_resolved_tint_
                         : state.color_temp_custom_tint_;
  return std::clamp(tint, static_cast<float>(color_temp::kTintMin),
                    static_cast<float>(color_temp::kTintMax));
}

void CopyFieldState(AdjustmentField field, const AdjustmentState& from, AdjustmentState& to);

}  // namespace alcedo::ui
