// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The `ProfileToneCurve` of a DCP profile as points of the RGB curve of the Tone Curve (#911,
// dcp_profile_tone_curve_study.md, section 11): the point rules, the neutral axis that the points
// render through each DRT against the profile, the cache, and a survey of a user DCP folder.

#include "edit/runtime/profile_tone_curve_import.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "color/color_encoding_math.h"
#include "edit/operators/models/tone_curve_spline.hpp"
#include "image/dcp_file_reader.hpp"

namespace alcedo {
namespace {

constexpr std::array<DrtMethod, 2> kMethods{DrtMethod::Aces20, DrtMethod::OpenDrt};

auto                               MethodName(DrtMethod method) -> const char* {
  return method == DrtMethod::Aces20 ? "ACES 2.0" : "OpenDRT";
}

/// CIE L* of display light relative to the peak, the metric of the fit.
auto Lightness(float display) -> float {
  const float y = std::clamp(display, 0.0f, 1.0f);
  return y > 216.0f / 24389.0f ? 116.0f * std::cbrt(y) - 16.0f : y * (24389.0f / 27.0f);
}

/// 8-bit code of display light on a gamma 2.2 display, for the record.
auto Code(float display) -> float {
  return 255.0f * std::pow(std::clamp(display, 0.0f, 1.0f), 1.0f / 2.2f);
}

/// A camera-like curve: a power curve with a shoulder, 0.444 at scene 0.18, near the median of
/// the user folder (study section 3.1).
auto CameraLikeCurve() -> DngToneCurvePtr {
  std::vector<DngToneCurvePoint> points;
  constexpr int                  kCount = 128;
  for (int i = 0; i < kCount; ++i) {
    const double x = std::pow(static_cast<double>(i) / (kCount - 1), 1.6);
    const double y = std::min(1.0, std::pow(x, 0.62) * (1.0 + 0.35 * (1.0 - x)));
    points.push_back({static_cast<float>(x), static_cast<float>(y)});
  }
  return MakeDngToneCurve(std::move(points));
}

/// The identity curve, and a steep S curve that clips the shadows.
auto LinearCurve() -> DngToneCurvePtr { return MakeDngToneCurve({{0.0f, 0.0f}, {1.0f, 1.0f}}); }
auto SteepCurve() -> DngToneCurvePtr {
  return MakeDngToneCurve(
      {{0.0f, 0.0f}, {0.02f, 0.0f}, {0.18f, 0.5f}, {0.5f, 0.92f}, {1.0f, 1.0f}});
}

struct RenderedErrors {
  float lightness   = 0.0f;  ///< Largest L* difference.
  float codes       = 0.0f;  ///< Largest 8-bit code difference (gamma 2.2).
  float lightness_x = 0.0f;  ///< Curve input of the largest L* difference.
};

/// Largest errors of @p fit against the profile on @p samples axis samples.
auto Rendered(const DngToneCurve& curve, DrtMethod method, const ProfileToneCurveFit& fit,
              int samples = 4097) -> RenderedErrors {
  const auto     response = ProfileToneNeutralResponse::Resolve(method);
  const auto     spline   = tone_curve::SolveToneCurveSpline(fit.points);
  RenderedErrors largest;
  for (int i = 0; i < samples; ++i) {
    const float x         = static_cast<float>(i) / static_cast<float>(samples - 1);
    const float output    = tone_curve::EvaluateToneCurveSpline(spline, x);
    const float rendered  = response->Forward(CeAcesccDecode(output));
    const float target    = ProfileToneCurveTargetDisplay(curve, x);
    const float lightness = std::abs(Lightness(rendered) - Lightness(target));
    if (lightness > largest.lightness) {
      largest.lightness   = lightness;
      largest.lightness_x = x;
    }
    largest.codes = std::max(largest.codes, std::abs(Code(rendered) - Code(target)));
  }
  return largest;
}

void ExpectValidPoints(const ProfileToneCurveFit& fit) {
  ASSERT_GE(fit.points.size(), tone_curve::kMinControlPoints);
  ASSERT_LE(fit.points.size(), tone_curve::kMaxControlPoints);
  EXPECT_FALSE(tone_curve::FindToneCurvePointsError(fit.points).has_value());
  EXPECT_EQ(fit.points.back().x, 1.0f);
  for (std::size_t i = 0; i < fit.points.size(); ++i) {
    const auto& point = fit.points[i];
    EXPECT_GE(point.x, 0.0f);
    EXPECT_GE(point.y, 0.0f);
    EXPECT_LE(point.y, 1.0f);
    if (i > 0) {
      // Rounding to 5 decimals keeps the spacing within 1e-5.
      EXPECT_GE(point.x - fit.points[i - 1].x, kProfileToneCurveMinSpacing - 2e-5f) << i;
      EXPECT_GE(point.y, fit.points[i - 1].y) << i;
    }
  }
}

TEST(ProfileToneCurveImportTest, TargetFollowsTheProfileThroughTheInverseNeutralResponse) {
  const auto curve = CameraLikeCurve();
  for (const auto method : kMethods) {
    const auto  response = ProfileToneNeutralResponse::Resolve(method);
    // 18% grey at the curve input of the Default document (+1.5 EV) is 0.18 / 2^1.5 at ACR scale.
    const float x        = CeAcesccEncode(0.18f);
    const float expected = curve->Interpolate(
        ProfileToneHighlightShoulder(0.18f * std::exp2(-kProfileToneExposureAnchorEv)));
    EXPECT_NEAR(ProfileToneCurveTargetDisplay(*curve, x), expected, 1e-6f);
    const float output = ProfileToneCurveTargetOutput(*curve, *response, x);
    EXPECT_NEAR(response->Forward(CeAcesccDecode(output)), expected, 1e-4f) << MethodName(method);
  }
}

TEST(ProfileToneCurveImportTest, FitGivesMonotonePointsThatRenderTheProfileWithinTheGoal) {
  for (const auto& curve : {CameraLikeCurve(), LinearCurve(), SteepCurve()}) {
    for (const auto method : kMethods) {
      SCOPED_TRACE(MethodName(method));
      const auto start = std::chrono::steady_clock::now();
      const auto fit   = FitProfileToneCurve(*curve, method);
      const auto ms    = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - start)
                          .count();
      ExpectValidPoints(fit);
      const auto rendered = Rendered(*curve, method, fit);
      std::printf(
          "[measurement] %s: %zu points, fit error %.3f L*, rendered %.3f L* (%.2f codes), "
          "start (%.5f, %.5f), %lld ms\n",
          MethodName(method), fit.points.size(), fit.error_lightness, rendered.lightness,
          rendered.codes, fit.points.front().x, fit.points.front().y, static_cast<long long>(ms));
      // One L* is about a just noticeable difference.
      EXPECT_LE(fit.error_lightness, 1.0f);
      // The fit measures 1025 samples; 4097 samples find at most a little more.
      EXPECT_LE(rendered.lightness, fit.error_lightness + 0.05f);
    }
  }
}

TEST(ProfileToneCurveImportTest, FitIsDeterministic) {
  const auto curve = CameraLikeCurve();
  const auto first = FitProfileToneCurve(*curve, DrtMethod::OpenDrt);
  const auto again = FitProfileToneCurve(*curve, DrtMethod::OpenDrt);
  EXPECT_EQ(first.points, again.points);
  EXPECT_EQ(first.error_lightness, again.error_lightness);
}

TEST(ProfileToneCurveImportTest, CacheKeepsOneFitPerCurveAndMethod) {
  const auto curve = CameraLikeCurve();
  const auto aces  = ProfileToneCurvePoints(curve, DrtMethod::Aces20);
  const auto open  = ProfileToneCurvePoints(curve, DrtMethod::OpenDrt);
  // Another object with the same points has the same fingerprint.
  const auto copy  = CameraLikeCurve();
  EXPECT_EQ(ProfileToneCurvePoints(copy, DrtMethod::Aces20), aces);
  EXPECT_NE(aces, open);
  EXPECT_NE(aces->points, open->points);
  EXPECT_EQ(aces->points, FitProfileToneCurve(*curve, DrtMethod::Aces20).points);
  EXPECT_THROW((void)ProfileToneCurvePoints(curve, DrtMethod::FilmPrint), std::invalid_argument);
  EXPECT_THROW((void)ProfileToneCurvePoints(nullptr, DrtMethod::Aces20), std::invalid_argument);
}

/// Fits every distinct `ProfileToneCurve` below the folder in `ALCEDO_DCP_FOLDER` (a user DCP
/// library, never part of the repository) for both methods, and records the errors, the point
/// counts, and the time.
TEST(ProfileToneCurveImportFolderTest, EveryCurveOfTheUserFolderFitsWithinOneLightnessUnit) {
  const char* folder = std::getenv("ALCEDO_DCP_FOLDER");
  if (folder == nullptr || *folder == '\0') GTEST_SKIP() << "ALCEDO_DCP_FOLDER is not set";
  std::map<std::uint64_t, DngToneCurvePtr> curves;
  std::map<std::uint64_t, std::string>     names;
  std::size_t                              files = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(folder)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".dcp") continue;
    ++files;
    const auto profile = ReadDcpCameraProfile(entry.path());
    if (profile->profile_tone_curve) {
      curves.emplace(profile->profile_tone_curve->fingerprint, profile->profile_tone_curve);
      names.emplace(profile->profile_tone_curve->fingerprint, entry.path().filename().string());
    }
  }
  std::printf("[measurement] %zu files, %zu distinct curves\n", files, curves.size());
  for (const auto method : kMethods) {
    std::vector<float>       errors;
    std::vector<float>       codes;
    std::vector<std::size_t> counts;
    std::vector<float>       start_y;
    struct Worst {
      float       error;
      float       x;
      std::size_t points;
      std::string name;
    };
    std::vector<Worst> worst;
    const auto         start = std::chrono::steady_clock::now();
    for (const auto& [fingerprint, curve] : curves) {
      const auto fit = FitProfileToneCurve(*curve, method);
      ExpectValidPoints(fit);
      errors.push_back(fit.error_lightness);
      const auto rendered = Rendered(*curve, method, fit, 1025);
      codes.push_back(rendered.codes);
      worst.push_back(
          {fit.error_lightness, rendered.lightness_x, fit.points.size(), names[fingerprint]});
      counts.push_back(fit.points.size());
      start_y.push_back(fit.points.front().x == 0.0f ? fit.points.front().y : 0.0f);
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    std::ranges::sort(errors);
    std::ranges::sort(codes);
    std::ranges::sort(counts);
    std::ranges::sort(start_y);
    const auto at = [](const auto& sorted, double q) {
      return sorted[static_cast<std::size_t>(q * static_cast<double>(sorted.size() - 1))];
    };
    std::printf(
        "[measurement] %s: L* error median %.3f, p95 %.3f, max %.3f; code error median "
        "%.2f, p95 %.2f, max %.2f; points median %zu, max %zu, at 16: %lld; y at x = 0 "
        "median %.4f, max %.4f; %.1f ms per curve\n",
        MethodName(method), at(errors, 0.5), at(errors, 0.95), errors.back(), at(codes, 0.5),
        at(codes, 0.95), codes.back(), at(counts, 0.5), counts.back(),
        static_cast<long long>(std::ranges::count(counts, tone_curve::kMaxControlPoints)),
        at(start_y, 0.5), start_y.back(),
        static_cast<double>(ms) / static_cast<double>(curves.size()));
    std::ranges::sort(worst, [](const Worst& a, const Worst& b) { return a.error > b.error; });
    for (std::size_t i = 0; i < std::min<std::size_t>(8, worst.size()); ++i) {
      std::printf("[measurement]   %.3f L* at x %.4f (%+.2f EV), %zu points: %s\n", worst[i].error,
                  worst[i].x, (worst[i].x - CE_ACESCC_MIDDLE_GREY) * 17.52f, worst[i].points,
                  worst[i].name.c_str());
    }
    EXPECT_LE(errors.back(), 1.0f) << MethodName(method);
  }
}

}  // namespace
}  // namespace alcedo
