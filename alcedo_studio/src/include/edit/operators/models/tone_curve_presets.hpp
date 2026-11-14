//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <span>
#include <string_view>
#include <vector>

#include "edit/operators/models/curve_model.hpp"

namespace alcedo {

/// Id of the Film-like preset, the RGB curve of the RAW import look
/// (CreateDefaultPipelineDocument).
inline constexpr std::string_view kFilmLikeToneCurvePresetId = "film_like";

/**
 * @brief One preset of the Tone Curve section: a list of control points with a stable id.
 *
 * A preset replaces the points of one curve. The document stores only the points; the id is not
 * stored. The points of the fitted presets come from tone_curve_preset_study.md
 * (scripts/fit_tone_curve_presets.py): 2 to 16 points with rising x in the unit square, 18% grey
 * on the curve axis at (0.41359, 0.41359), and a monotone spline.
 */
struct ToneCurvePreset {
  /// Stable id: `linear`, `soft_contrast`, `medium_contrast`, `strong_contrast`, `film_like`,
  /// `filmic`, `kodak_2383`, `fuji_velvia_50`, `kodak_ektachrome_e100`.
  std::string_view            id;
  /// English display name, for example `Film-like`. The UI translates it by id.
  std::string_view            display_name;
  /// Menu group: empty for Linear, `Contrast`, `Rendering`, or `Film`.
  std::string_view            group;
  std::span<const CurvePoint> points;

  /// The points as the value of a curve write.
  [[nodiscard]] auto          PointList() const -> std::vector<CurvePoint>;
};

/// All presets in menu order: Linear, the contrast shapes, the rendering looks, the films.
[[nodiscard]] auto ToneCurvePresets() -> std::span<const ToneCurvePreset>;

/// The preset with id @p id, or null.
[[nodiscard]] auto FindToneCurvePreset(std::string_view id) -> const ToneCurvePreset*;

/// The preset whose points equal @p points exactly (float values, same count), or null.
[[nodiscard]] auto MatchToneCurvePreset(std::span<const CurvePoint> points)
    -> const ToneCurvePreset*;

}  // namespace alcedo
