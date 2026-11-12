// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace alcedo {

/// Number of intervals of the sampled tone table (DNG SDK `dng_1d_table::kTableSize`).
inline constexpr std::size_t kDngToneTableIntervals = 4096;
/// Entries of the sampled tone table: one sample at each interval end, `j / 4096` for j = 0..4096.
inline constexpr std::size_t kDngToneTableEntries   = kDngToneTableIntervals + 1;
/// Largest point count that the curve accepts. The user folder holds at most 256 points per curve
/// (dcp_profile_tone_curve_study.md, section 3.1).
inline constexpr std::size_t kMaxDngToneCurvePoints = 4096;

/// One control point of a `ProfileToneCurve` (DNG tag 50940): scene value and display value.
struct DngToneCurvePoint {
  float x                                                  = 0.0f;
  float y                                                  = 0.0f;
  auto  operator==(const DngToneCurvePoint&) const -> bool = default;
};

/**
 * @brief Immutable `ProfileToneCurve` of a camera profile, solved and sampled as the DNG SDK does.
 *
 * @ref points are the tag values. @ref slopes are the knot slopes of the cubic spline of
 * `dng_spline_solver` (C2 continuous, second derivative zero at both ends). @ref table holds the
 * spline at `j / 4096` for j = 0..4096 as float, the `dng_1d_table` of `dng_render`
 * (dcp_profile_tone_curve_study.md, section 2).
 *
 * The value is session data like the profile tables. @ref fingerprint is FNV-1a of the points; it
 * is the curve identity for result caches and stays outside the profile fingerprint
 * (study section 6.3).
 */
struct DngToneCurve {
  std::vector<DngToneCurvePoint>          points;
  std::vector<double>                     slopes;
  std::array<float, kDngToneTableEntries> table{};
  std::uint64_t                           fingerprint = 0;

  /// Spline value at @p x (`dng_spline_solver::Evaluate`): the end values hold outside the points.
  [[nodiscard]] auto                      Evaluate(double x) const -> double;
  /// Table value at @p x (`dng_1d_table::Interpolate`) with @p x clamped to [0, 1].
  [[nodiscard]] auto                      Interpolate(float x) const -> float;
};
using DngToneCurvePtr = std::shared_ptr<const DngToneCurve>;

/**
 * @brief Validate the points of a `ProfileToneCurve`, solve the spline, and sample the table.
 *
 * Preconditions checked here (`dng_tone_curve::IsValid`): 2 to @ref kMaxDngToneCurvePoints points,
 * finite values, `x` strictly rising, and `x` and `y` on [0, 1].
 *
 * @throws std::runtime_error that names the failed condition and the point index.
 */
auto MakeDngToneCurve(std::vector<DngToneCurvePoint> points) -> DngToneCurvePtr;

/**
 * @brief The DNG SDK baseline RGB tone (`RefBaselineRGBTone`) of one pixel, in place.
 *
 * The table applies to the largest and the smallest channel; the middle channel keeps its
 * relative position between them. Each channel must lie on [0, 1], as after the SDK exposure
 * ramp; the table clamps a value outside that range.
 */
void ApplyDngBaselineRgbTone(const DngToneCurve& curve, std::span<float, 3> rgb);

}  // namespace alcedo
