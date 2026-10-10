//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "app/editor_adjustment_types.hpp"

namespace alcedo {

/// Editor adjustment that a stable QML field key names. Aliases resolve to the same value.
enum class EditorAdjustmentField {
  Exposure,
  Contrast,
  Whites,
  Blacks,
  Shadows,
  Highlights,
  Curve,
  Saturation,
  Vibrance,
  GradeWhiteBalance,
  Hls,
  ColorWheel,
  Lut,
  Clarity,
  Sharpen,
  Drt,
  FilmGrain,
  Halation,
  CropRotate,
  RawDecode,
  LensCalibration,
  ColorTemperature,
  Diffusion,
  /// Raster input profile override (Develop `input.profile_override`).
  InputProfile,
};

/// Every accepted field key with the adjustment it controls, including the aliases that panels
/// and older history rows use.
auto EditorAdjustmentFieldKeys()
    -> std::span<const std::pair<std::string_view, EditorAdjustmentField>>;

/// Resolve a stable QML field key (or one of its aliases) to the adjustment it controls.
auto ResolveEditorAdjustmentField(const std::string& field_key)
    -> std::optional<EditorAdjustmentField>;

/**
 * @brief Map a field write payload onto PipelineDocument Model JSON keys.
 *
 * Panel writes use field keys (`exposure`) or a scalar `value`. Document Models
 * use `exposure_ev` / `cube_path`. Unknown keys are left unchanged.
 */
auto EditorAdjustmentDocumentParamsFromWrite(const std::string& field_key, nlohmann::json params)
    -> nlohmann::json;

}  // namespace alcedo
