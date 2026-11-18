//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <span>
#include <string_view>
#include <vector>

#include "edit/operators/models/curve_model.hpp"

namespace alcedo {

/// Id of the Cl. Digit preset, the RGB curve of the RAW import look
/// (CreateDefaultPipelineDocument).
inline constexpr std::string_view kClDigitalToneCurvePresetId = "cl_digital";

/**
 * @brief One preset of the Tone Curve section: a list of control points with a stable id.
 *
 * A preset replaces the points of one curve. The document stores only the points; the id is not
 * stored. The points come from tone_curve_preset_study.md (scripts/fit_tone_curve_presets.py):
 * fitted to a source tone, or placed by hand (Cl. Digit). Each preset has 2 to 16
 * points with rising x in the unit square, 18% grey on the curve axis at (0.41359, 0.41359), and a
 * monotone spline.
 */
struct ToneCurvePreset {
  /// Stable id: `linear`, `soft_contrast`, `medium_contrast`, `strong_contrast`, `cl_digital`,
  /// `kodak_2383`, `fuji_velvia_50`, `kodak_ektachrome_e100`.
  std::string_view            id;
  /// English display name, for example `Cl. Digit`. The UI translates it by id.
  std::string_view            display_name;
  /// Menu group: empty for Linear, `Contrast`, `Rendering`, or `Film`.
  std::string_view            group;
  std::span<const CurvePoint> points;

  /// The points as the value of a curve write.
  [[nodiscard]] auto          PointList() const -> std::vector<CurvePoint>;
};

/// All presets in menu order: Linear, the contrast shapes, Cl. Digit, the films.
[[nodiscard]] auto ToneCurvePresets() -> std::span<const ToneCurvePreset>;

/// The preset with id @p id, or null.
[[nodiscard]] auto FindToneCurvePreset(std::string_view id) -> const ToneCurvePreset*;

/**
 * @brief The preset that a write by id applies: the preset with id @p id, or Cl. Digit.
 *
 * An id that names no preset gives Cl. Digit (@ref kClDigitalToneCurvePresetId). That covers
 * `film_like`, the id of the preset until #914, and `filmic`, which left the menu in #914. The
 * curve write (`editor.set` on `curve`) and the preset menu apply presets through this.
 */
[[nodiscard]] auto ResolveToneCurvePreset(std::string_view id) -> const ToneCurvePreset&;

/// The preset whose points equal @p points exactly (float values, same count), or null.
[[nodiscard]] auto MatchToneCurvePreset(std::span<const CurvePoint> points)
    -> const ToneCurvePreset*;

/**
 * @brief Earlier point lists of the Cl. Digit preset (named Film-like until #914), oldest
 * first.
 *
 * - The first fit of #891, before the highlight shoulder.
 * - The fit with the highlight shoulder of #899, before the shadow toe of #910.
 * - The fit with the shadow toe of #910, before the points placed by hand of #914.
 *
 * The RAW import look wrote each of them as the RGB curve of Color Grade 1, and documents store
 * their points, so documents imported with an earlier look keep them. The preset menu shows them
 * as a custom curve.
 */
[[nodiscard]] auto ClDigitalToneCurveEarlierPoints()
    -> std::span<const std::span<const CurvePoint>>;

/// True when @p points equal the Cl. Digit preset exactly, as it is now or as it was earlier
/// (@ref ClDigitalToneCurveEarlierPoints).
[[nodiscard]] auto IsClDigitalToneCurve(std::span<const CurvePoint> points) -> bool;

}  // namespace alcedo
