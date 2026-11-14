// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "edit/graph/drt_node_model.hpp"
#include "edit/operators/models/curve_model.hpp"
#include "edit/runtime/profile_tone_reference.hpp"
#include "image/dng_tone_curve.hpp"

// The `ProfileToneCurve` of a DCP camera profile as points of the RGB curve of the Tone Curve
// (#911, dcp_profile_tone_curve_study.md, section 11). The editor writes the points into the RGB
// curve of Color Grade 1, so the Tone Curve panel shows the profile curve and the user can edit
// it. The points follow the neutral axis of the profile tone stage of T1 to T3, which they
// replace: at curve input `x` (ACEScc), the target output is
//
//   y_t(x) = ACEScc(N_m^-1(R_C(H(v(x) * 2^-E_a)))),
//
// with `v(x)` the scene value of `x`, `E_a` the exposure anchor, `H` the highlight shoulder, `R_C`
// the profile curve, and `N_m^-1` the inverse neutral response of the DRT method `m`.

namespace alcedo {

/// Fit error at which the fit stops adding points: CIE L* of display light relative to the peak.
/// 0.2 is about 0.5 of an 8-bit code (gamma 2.2) at 18% grey; one L* is about a just noticeable
/// difference.
inline constexpr float       kProfileToneCurveFitGoalLightness = 0.2f;
/// Smallest x step between two fitted points on the curve axis: about 1/3 stop.
inline constexpr float       kProfileToneCurveMinSpacing       = 0.02f;
/// Samples of the curve axis at which the fit measures its error: x = i / (count - 1).
inline constexpr std::size_t kProfileToneCurveFitSamples       = 1025;
/// Decimals of a fitted coordinate, as the Tone Curve presets store them.
inline constexpr int         kProfileToneCurveDigits           = 5;

/// The fitted RGB curve of one profile curve and one DRT method.
struct ProfileToneCurveFit {
  /// 2 to 16 points with x rising by at least @ref kProfileToneCurveMinSpacing, x and y on
  /// [0, 1], y not falling.
  std::vector<CurvePoint> points;
  /// Largest CIE L* difference of display light between the curve through `N_m` and the target
  /// over the fit samples. L* weighs the deep shadows as the eye does; 8-bit codes with gamma 2.2
  /// overstate them, where the floor of a curve output of at least 0 (the scene offset of OpenDRT)
  /// meets the true black of a profile.
  float                   error_lightness = 0.0f;
};

/// Display light (relative to the peak) that the profile renders at curve input @p x:
/// `R_C(H(v(x) * 2^-E_a))`.
[[nodiscard]] auto ProfileToneCurveTargetDisplay(const DngToneCurve& curve, float x) -> float;

/// The target output `y_t(x)` on the curve axis for the DRT of @p response.
[[nodiscard]] auto ProfileToneCurveTargetOutput(const DngToneCurve&               curve,
                                                const ProfileToneNeutralResponse& response, float x)
    -> float;

/**
 * @brief Fit the points of the RGB curve that render the neutral axis of @p curve through the
 *        DRT @p method as the profile renders it.
 *
 * The curve starts at x = 0, or where `y_t` reaches 0, and ends at x = 1. Points go in at the
 * largest L* error until the error is at most @ref kProfileToneCurveFitGoalLightness or the
 * curve has 16 points; a pattern search then moves the inner points to lower the largest error.
 * The result is deterministic.
 *
 * @throws std::invalid_argument for DrtMethod::FilmPrint, which has no neutral response.
 */
[[nodiscard]] auto FitProfileToneCurve(const DngToneCurve& curve, DrtMethod method)
    -> ProfileToneCurveFit;

/**
 * @brief @ref FitProfileToneCurve of @p curve and @p method, built once per curve fingerprint and
 *        method in the process. Thread-safe.
 * @throws std::invalid_argument for a null curve or DrtMethod::FilmPrint.
 */
[[nodiscard]] auto ProfileToneCurvePoints(const DngToneCurvePtr& curve, DrtMethod method)
    -> std::shared_ptr<const ProfileToneCurveFit>;

}  // namespace alcedo
