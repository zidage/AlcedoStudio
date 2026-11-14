// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#include "edit/runtime/profile_tone_curve_import.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "color/color_encoding_math.h"
#include "edit/operators/models/tone_curve_spline.hpp"

namespace alcedo {
namespace {

/// Axis range of the L* table of the fit: below the ACEScc floor up to the top of the axis.
constexpr float                kTableAxisLow   = -0.4f;
constexpr float                kTableAxisHigh  = 1.0f;
constexpr std::size_t          kTableEntries   = 8193;
constexpr int                  kBisectionSteps = 48;
/// Steps of the pattern search on the curve axis, from coarse to fine.
constexpr std::array<float, 6> kSearchSteps  = {0.008f, 0.004f, 0.002f, 0.001f, 0.0005f, 0.00025f};
constexpr int                  kSearchPasses = 4;

/// CIE 1976 L* of display light @p display relative to the peak.
auto                           Lightness(float display) -> float {
  const float y = std::clamp(display, 0.0f, 1.0f);
  return y > 216.0f / 24389.0f ? 116.0f * std::cbrt(y) - 16.0f : y * (24389.0f / 27.0f);
}

auto Round(float value) -> float {
  const float scale = std::pow(10.0f, static_cast<float>(kProfileToneCurveDigits));
  return std::round(value * scale) / scale;
}

/// One fit: the target L* at the samples and a table of the L* of each curve output.
class Fitter {
 public:
  Fitter(const DngToneCurve& curve, const ProfileToneNeutralResponse& response)
      : curve_(curve), response_(response) {
    xs_.resize(kProfileToneCurveFitSamples);
    target_lightness_.resize(kProfileToneCurveFitSamples);
    for (std::size_t i = 0; i < kProfileToneCurveFitSamples; ++i) {
      xs_[i] = static_cast<float>(i) / static_cast<float>(kProfileToneCurveFitSamples - 1);
      target_lightness_[i] = Lightness(ProfileToneCurveTargetDisplay(curve_, xs_[i]));
    }
    table_.resize(kTableEntries);
    for (std::size_t i = 0; i < kTableEntries; ++i) {
      table_[i] = ExactLightness(TableAxis(i));
    }
  }

  auto Fit() -> ProfileToneCurveFit {
    auto  points     = Start();
    auto  best       = points;
    float best_error = Error(points);
    while (true) {
      Refine(points);
      const float error = Error(points);
      if (error < best_error) {
        best       = points;
        best_error = error;
      }
      if (error <= kProfileToneCurveFitGoalLightness ||
          points.size() >= tone_curve::kMaxControlPoints || !Insert(points)) {
        break;
      }
      // The search starts again from points on the target: a run of equal y values that an
      // earlier search left would hold every later point of the run flat.
      Snap(points);
    }
    points = std::move(best);
    for (auto& point : points) {
      point = {Round(point.x), Round(point.y)};
    }
    ProfileToneCurveFit fit;
    fit.error_lightness = ExactError(points);
    fit.points          = std::move(points);
    return fit;
  }

 private:
  auto TargetOutput(float x) const -> float {
    return ProfileToneCurveTargetOutput(curve_, response_, x);
  }

  auto TableAxis(std::size_t index) const -> float {
    return kTableAxisLow + (kTableAxisHigh - kTableAxisLow) * static_cast<float>(index) /
                               static_cast<float>(kTableEntries - 1);
  }

  /// L* of curve output @p y through the DRT.
  auto ExactLightness(float y) const -> float {
    return Lightness(response_.Forward(CeAcesccDecode(y)));
  }

  auto TableLightness(float y) const -> float {
    const float coordinate = (std::clamp(y, kTableAxisLow, kTableAxisHigh) - kTableAxisLow) /
                             (kTableAxisHigh - kTableAxisLow) *
                             static_cast<float>(kTableEntries - 1);
    const auto  index = std::min(static_cast<std::size_t>(coordinate), kTableEntries - 2);
    const float fract = coordinate - static_cast<float>(index);
    return table_[index] * (1.0f - fract) + table_[index + 1] * fract;
  }

  /// The curve starts at x = 0, or where the target reaches 0 on the axis, and ends at x = 1.
  auto Start() const -> std::vector<CurvePoint> {
    float x_low = 0.0f;
    if (TargetOutput(0.0f) < 0.0f) {
      float low  = 0.0f;
      float high = 1.0f;
      for (int step = 0; step < kBisectionSteps; ++step) {
        const float mid                         = 0.5f * (low + high);
        (TargetOutput(mid) < 0.0f ? low : high) = mid;
      }
      x_low = high;
    }
    const float y_low = x_low > 0.0f ? 0.0f : std::clamp(TargetOutput(0.0f), 0.0f, 1.0f);
    return {{x_low, y_low}, {1.0f, std::clamp(TargetOutput(1.0f), 0.0f, 1.0f)}};
  }

  /// L* error per sample, from the L* table or exact.
  auto Errors(const std::vector<CurvePoint>& points, bool exact) const -> std::vector<float> {
    const auto         spline = tone_curve::SolveToneCurveSpline(points);
    std::vector<float> errors(xs_.size());
    for (std::size_t i = 0; i < xs_.size(); ++i) {
      const float y = tone_curve::EvaluateToneCurveSpline(spline, xs_[i]);
      errors[i] = std::abs((exact ? ExactLightness(y) : TableLightness(y)) - target_lightness_[i]);
    }
    return errors;
  }

  auto Error(const std::vector<CurvePoint>& points) const -> float {
    const auto errors = Errors(points, false);
    return *std::ranges::max_element(errors);
  }

  auto ExactError(const std::vector<CurvePoint>& points) const -> float {
    const auto errors = Errors(points, true);
    return *std::ranges::max_element(errors);
  }

  /// Add a point at the largest error that keeps the spacing; false when no sample can take one.
  auto Insert(std::vector<CurvePoint>& points) const -> bool {
    const auto               errors = Errors(points, false);
    std::vector<std::size_t> order(errors.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::ranges::stable_sort(
        order, [&errors](std::size_t a, std::size_t b) { return errors[a] > errors[b]; });
    for (const auto index : order) {
      const float x = xs_[index];
      if (x <= points.front().x || x >= points.back().x) continue;
      const bool spaced = std::ranges::all_of(points, [x](const CurvePoint& point) {
        return std::abs(x - point.x) >= kProfileToneCurveMinSpacing;
      });
      if (!spaced) continue;
      const auto  next = std::ranges::upper_bound(points, x, {}, &CurvePoint::x);
      const auto  prev = std::prev(next);
      const float y    = std::clamp(TargetOutput(x), prev->y, next->y);
      points.insert(next, {x, y});
      return true;
    }
    return false;
  }

  /// Puts each point on the target, rising, except a start point in the black clip (y = 0).
  void Snap(std::vector<CurvePoint>& points) const {
    float floor = 0.0f;
    for (std::size_t i = 0; i < points.size(); ++i) {
      if (i == 0 && points[0].y == 0.0f && points[0].x > 0.0f) continue;
      points[i].y = std::max(std::clamp(TargetOutput(points[i].x), 0.0f, 1.0f), floor);
      floor       = points[i].y;
    }
  }

  /// Pattern search over the inner points: each coordinate moves by a step while that lowers the
  /// largest error, with the spacing and the order of the points kept. The end point x = 1 stays.
  void Refine(std::vector<CurvePoint>& points) const {
    if (points.size() < 3) return;
    float best = Error(points);
    for (const float step : kSearchSteps) {
      for (int pass = 0; pass < kSearchPasses; ++pass) {
        bool              improved = false;
        // A curve that starts in the black clip (y = 0) may move its start point along y = 0:
        // the end slope below it then follows the fall of the profile into black.
        const std::size_t first    = points.front().y == 0.0f ? 0 : 1;
        for (std::size_t i = first; i + 1 < points.size(); ++i) {
          for (const bool along_x : {false, true}) {
            if (i == 0 && !along_x) continue;
            for (const float direction : {1.0f, -1.0f}) {
              auto  candidate = points;
              auto& point     = candidate[i];
              if (along_x) {
                point.x += direction * step;
                const float low = i == 0 ? 0.0f : points[i - 1].x + kProfileToneCurveMinSpacing;
                if (point.x < low || point.x > points[i + 1].x - kProfileToneCurveMinSpacing) {
                  continue;
                }
              } else {
                point.y += direction * step;
                // Strictly between the neighbours: equal y values make a flat run.
                if (point.y <= points[i - 1].y || point.y >= points[i + 1].y) continue;
              }
              const float error = Error(candidate);
              if (error < best - 1e-5f) {
                points   = std::move(candidate);
                best     = error;
                improved = true;
                break;
              }
            }
          }
        }
        if (!improved) break;
      }
    }
  }

  const DngToneCurve&               curve_;
  const ProfileToneNeutralResponse& response_;
  std::vector<float>                xs_;
  std::vector<float>                target_lightness_;
  std::vector<float>                table_;
};

}  // namespace

auto ProfileToneCurveTargetDisplay(const DngToneCurve& curve, float x) -> float {
  const float scene = CeAcesccDecode(x) * std::exp2(-kProfileToneExposureAnchorEv);
  return curve.Interpolate(ProfileToneHighlightShoulder(scene));
}

auto ProfileToneCurveTargetOutput(const DngToneCurve&               curve,
                                  const ProfileToneNeutralResponse& response, float x) -> float {
  return CeAcesccEncode(response.Inverse(ProfileToneCurveTargetDisplay(curve, x)));
}

auto FitProfileToneCurve(const DngToneCurve& curve, DrtMethod method) -> ProfileToneCurveFit {
  const auto response = ProfileToneNeutralResponse::Resolve(method);
  return Fitter(curve, *response).Fit();
}

auto ProfileToneCurvePoints(const DngToneCurvePtr& curve, DrtMethod method)
    -> std::shared_ptr<const ProfileToneCurveFit> {
  if (!curve) throw std::invalid_argument("Profile tone curve: the profile has no curve");
  if (method == DrtMethod::FilmPrint) {
    throw std::invalid_argument("Profile tone curve: Film Print has no neutral response");
  }
  using Key = std::pair<std::uint64_t, DrtMethod>;
  static std::mutex                                                mutex;
  static std::map<Key, std::shared_ptr<const ProfileToneCurveFit>> fits;
  const Key                                                        key{curve->fingerprint, method};
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (const auto found = fits.find(key); found != fits.end()) return found->second;
  }
  // Fit outside the lock; two threads that miss together compute the same value.
  auto fit = std::make_shared<const ProfileToneCurveFit>(FitProfileToneCurve(*curve, method));
  std::lock_guard<std::mutex> lock(mutex);
  return fits.emplace(key, std::move(fit)).first->second;
}

}  // namespace alcedo
