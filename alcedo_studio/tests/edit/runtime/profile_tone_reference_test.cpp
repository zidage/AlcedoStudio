// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Host reference of the profile tone stage (dcp_profile_tone_curve_study.md, sections 6.1 and 8,
// phase T1): the neutral responses of ACES 2.0 and OpenDRT, their inverse tables, the neutral
// error of the stage, and the CIEDE2000 measurement record of section 8 against an SDK-style
// rendering of the same curve.

#include "edit/runtime/profile_tone_reference.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <numeric>
#include <opencv2/core.hpp>
#include <stdexcept>
#include <vector>

#include "color/color_encoding_catalog.hpp"
#include "edit/runtime/aces2_reference_math.h"
#include "edit/runtime/drt/aces2_reference_runtime.hpp"
#include "edit/runtime/profile_tone_gpu_math.h"
#include "edit/runtime/profile_tone_stage.hpp"
#include "image/dng_tone_curve.hpp"
#include "oklab_contrast_reference.hpp"

namespace alcedo {
namespace {

using Rgb = std::array<float, 3>;

constexpr std::array<DrtMethod, 2> kMethods{DrtMethod::Aces20, DrtMethod::OpenDrt};

auto                               MethodName(DrtMethod method) -> const char* {
  return method == DrtMethod::Aces20 ? "ACES 2.0" : "OpenDRT";
}

/// A camera-like curve: 128 points of a power curve with a shoulder, 0.444 at scene 0.18, near
/// the median of the user folder (study section 3.1).
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

/// Raw value (ACR scale, raw white 1) of an AP1 neutral to the AP1 value at the DRT entry of the
/// Default document: the exposure anchor applies.
auto AnchoredNeutral(float raw) -> Rgb {
  const float value = raw * std::exp2(kProfileToneExposureAnchorEv);
  return {value, value, value};
}

TEST(ProfileToneReferenceTest, NeutralResponsesRenderMidGreyAndReachThePeak) {
  // ACES 2.0 SDR renders scene 0.18 at 10 nits (OpenColorIO ToneScaleParams, study section 3.1).
  EXPECT_NEAR(ProfileToneNeutralResponse::Resolve(DrtMethod::Aces20)->Forward(0.18f), 0.100f,
              1.5e-3f);
  // OpenDRT places its 10 nit display grey with the contrast tn_con = 1.66, and the default
  // Rec.709 gamma 2.2 encoding renders with the average surround, power ts_p = 1.66 / 1.1. The
  // lower power lifts scene 0.18 to 0.1 ^ (1 / 1.1) before the toe: 12.4 nits.
  EXPECT_NEAR(ProfileToneNeutralResponse::Resolve(DrtMethod::OpenDrt)->Forward(0.18f), 0.1237f,
              1e-3f);
  for (const auto method : kMethods) {
    const auto response = ProfileToneNeutralResponse::Resolve(method);
    EXPECT_NEAR(response->Forward(response->SceneAtPeak()), 1.0f, 1e-5f) << MethodName(method);
    EXPECT_FLOAT_EQ(response->Forward(response->SceneAtBlack()), 0.0f) << MethodName(method);
  }
  // ACES 2.0 reaches the 100 nit peak at r_hit = 128 (study section 10).
  EXPECT_NEAR(ProfileToneNeutralResponse::Resolve(DrtMethod::Aces20)->SceneAtPeak(), 128.0f, 2.0f);
}

TEST(ProfileToneReferenceTest, NeutralResponseAndInverseTableRiseMonotonically) {
  for (const auto method : kMethods) {
    const auto response = ProfileToneNeutralResponse::Resolve(method);
    float      previous = response->Forward(response->SceneAtBlack());
    for (int i = 0; i <= 4000; ++i) {
      const float scene =
          response->SceneAtBlack() +
          (response->SceneAtPeak() - response->SceneAtBlack()) * std::pow(i / 4000.0f, 4.0f);
      const float value = response->Forward(scene);
      ASSERT_GE(value, previous) << MethodName(method) << " scene " << scene;
      previous = value;
    }
    const auto table = response->InverseTable();
    ASSERT_EQ(table.size(), kProfileToneInverseTableEntries);
    EXPECT_FLOAT_EQ(table.front(), response->SceneAtBlack());
    EXPECT_FLOAT_EQ(table.back(), response->SceneAtPeak());
    EXPECT_TRUE(std::is_sorted(table.begin(), table.end())) << MethodName(method);
  }
}

TEST(ProfileToneReferenceTest, InverseTableReturnsTheSceneValueOfEachDisplayValue) {
  for (const auto method : kMethods) {
    const auto response  = ProfileToneNeutralResponse::Resolve(method);
    double     max_error = 0.0;
    for (float stops = -14.0f; stops <= 0.0f; stops += 1.0f / 32.0f) {
      const float display = std::exp2(stops);
      const float round   = response->Forward(response->Inverse(display));
      max_error           = std::max(max_error, std::abs(round / display - 1.0));
    }
    EXPECT_LT(max_error, 1e-4) << MethodName(method);
    EXPECT_FLOAT_EQ(response->Inverse(1.5f), response->SceneAtPeak());
    EXPECT_FLOAT_EQ(response->Inverse(-0.5f), response->SceneAtBlack());
  }
}

/// Largest relative error of `N_m(stage(v))` against `C(H(v))` on a neutral ramp from 12 stops
/// below raw white to 4 stops above it (measurement (a) of study section 8). Below the knee of the
/// highlight shoulder, `C(H(v))` is `C(v)`.
auto NeutralRampError(const DngToneCurve& curve, const ProfileToneNeutralResponse& response)
    -> double {
  double max_error = 0.0;
  for (float stops = -12.0f; stops <= 4.0f; stops += 1.0f / 16.0f) {
    const float raw      = std::exp2(stops);
    const Rgb   staged   = ApplyProfileToneStageReference(curve, response, AnchoredNeutral(raw));
    const float expected = curve.Interpolate(ProfileToneHighlightShoulder(raw));
    for (float channel : staged) {
      max_error = std::max(max_error, std::abs(response.Forward(channel) / expected - 1.0));
    }
  }
  return max_error;
}

TEST(ProfileToneReferenceTest, NeutralRampThroughTheStageRendersTheProfileCurveWithin1e3) {
  const auto curve = CameraLikeCurve();
  for (const auto method : kMethods) {
    const auto   response = ProfileToneNeutralResponse::Resolve(method);
    const double error    = NeutralRampError(*curve, *response);
    std::printf("[measurement] neutral error, %s: %.2e relative\n", MethodName(method), error);
    EXPECT_LT(error, 1e-3) << MethodName(method);
  }
}

TEST(ProfileToneReferenceTest, HighlightShoulderKeepsTheCurveBelowTheKneeAndStaysBelowWhite) {
  constexpr float k = kProfileToneHighlightKnee;
  EXPECT_FLOAT_EQ(ProfileToneHighlightShoulder(-0.25f), 0.0f);
  for (float x : {0.0f, 0.01f, 0.18f, 0.3f, k}) EXPECT_FLOAT_EQ(ProfileToneHighlightShoulder(x), x);
  // Slope 1 on both sides of the knee: no kink where the shoulder starts.
  constexpr float h = 1e-4f;
  const float below = (ProfileToneHighlightShoulder(k) - ProfileToneHighlightShoulder(k - h)) / h;
  const float above = (ProfileToneHighlightShoulder(k + h) - ProfileToneHighlightShoulder(k)) / h;
  EXPECT_NEAR(below, 1.0f, 2e-3f);
  EXPECT_NEAR(above, 1.0f, 2e-3f);
  // Raw white maps to 0.75; each further stop halves the distance to the curve's white.
  EXPECT_FLOAT_EQ(ProfileToneHighlightShoulder(1.0f), 0.75f);
  float previous = ProfileToneHighlightShoulder(k);
  for (int stop = 1; stop <= 12; ++stop) {
    const float x     = std::exp2(static_cast<float>(stop - 1));
    const float value = ProfileToneHighlightShoulder(2.0f * x);
    EXPECT_NEAR(1.0f - value, 0.5f * (1.0f - ProfileToneHighlightShoulder(x)), 1e-6f) << stop;
    EXPECT_GT(value, previous) << stop;
    EXPECT_LT(value, 1.0f) << stop;
    previous = value;
  }
}

TEST(ProfileToneReferenceTest, SceneLightAboveRawWhiteRollsOffTowardThePeak) {
  const auto curve = CameraLikeCurve();
  for (const auto method : kMethods) {
    const auto response = ProfileToneNeutralResponse::Resolve(method);
    float      previous = 0.0f;
    for (float raw : {0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f}) {
      const Rgb   staged  = ApplyProfileToneStageReference(*curve, *response, AnchoredNeutral(raw));
      const float display = response->Forward(staged[1]);
      // Each stop above the knee keeps a step in display light: the stage does not clip.
      EXPECT_GT(display, previous + 1e-4f) << MethodName(method) << ' ' << raw;
      EXPECT_LT(display, 1.0f) << MethodName(method) << ' ' << raw;
      previous = display;
    }
    const Rgb far = ApplyProfileToneStageReference(*curve, *response, AnchoredNeutral(4096.0f));
    EXPECT_NEAR(response->Forward(far[1]), 1.0f, 1e-3f) << MethodName(method);
  }
}

TEST(ProfileToneReferenceTest, NegativeProPhotoChannelsRenderAsBlack) {
  const auto curve    = CameraLikeCurve();
  const auto response = ProfileToneNeutralResponse::Resolve(DrtMethod::Aces20);
  const Rgb  staged   = ApplyProfileToneStageReference(*curve, *response, {-0.5f, -0.2f, -1.0f});
  for (float channel : staged) EXPECT_NEAR(channel, 0.0f, 1e-6f);
}

TEST(ProfileToneReferenceTest, StageMatricesAreInversesAndKeepNeutralsNeutral) {
  const auto& m = ProfileToneStageMatrices();
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      float sum = 0.0f;
      for (int k = 0; k < 3; ++k)
        sum += m.ap1_to_prophoto[r * 3 + k] * m.prophoto_to_ap1[k * 3 + c];
      EXPECT_NEAR(sum, r == c ? 1.0f : 0.0f, 1e-5f) << r << ' ' << c;
    }
    const float row =
        m.ap1_to_prophoto[r * 3] + m.ap1_to_prophoto[r * 3 + 1] + m.ap1_to_prophoto[r * 3 + 2];
    EXPECT_NEAR(row, 1.0f, 1e-3f) << r;
  }
}

// The GPU form of the stage (profile_tone_gpu_math.h on the buffer of PackProfileToneGpuData),
// compiled for the host, against the scalar reference (phase T2).
TEST(ProfileToneReferenceTest, GpuMathOnThePackedBufferMatchesTheScalarReference) {
  const auto curve = CameraLikeCurve();
  for (const auto method : kMethods) {
    const auto response = ProfileToneNeutralResponse::Resolve(method);
    const auto data     = PackProfileToneGpuData(ProfileToneStage{curve, method});
    ASSERT_EQ(data.size(), static_cast<std::size_t>(PROFILE_TONE_GPU_FLOATS));
    float largest = 0.0f;
    for (float stops = -12.0f; stops <= 2.0f; stops += 0.5f) {
      const float level = std::exp2(stops + kProfileToneExposureAnchorEv);
      for (int hue = 0; hue < 12; ++hue) {
        for (float purity : {0.0f, 0.3f, 0.6f, 0.95f}) {
          const float angle = 6.2831853f * static_cast<float>(hue) / 12.0f;
          Rgb         ap1{};
          for (int c = 0; c < 3; ++c) {
            ap1[c] = level * (1.0f + purity * std::cos(angle - 2.0943951f * static_cast<float>(c)));
          }
          const auto reference = ApplyProfileToneStageReference(*curve, *response, ap1);
          const auto gpu =
              ProfileToneApplyStage(ProfileToneMakeRgb(ap1[0], ap1[1], ap1[2]), data.data());
          const Rgb out{gpu.r, gpu.g, gpu.b};
          for (int c = 0; c < 3; ++c) {
            largest = std::max(
                largest, std::abs(out[c] - reference[c]) / std::max(std::abs(reference[c]), 1e-3f));
          }
        }
      }
    }
    EXPECT_LT(largest, 1e-5f) << MethodName(method);
  }
}

TEST(ProfileToneReferenceTest, ResolveBuildsEachMethodOnceAndRejectsFilmPrint) {
  EXPECT_EQ(ProfileToneNeutralResponse::Resolve(DrtMethod::Aces20),
            ProfileToneNeutralResponse::Resolve(DrtMethod::Aces20));
  EXPECT_EQ(ProfileToneNeutralResponse::Resolve(DrtMethod::OpenDrt)->Method(), DrtMethod::OpenDrt);
  EXPECT_THROW((void)ProfileToneNeutralResponse::Resolve(DrtMethod::FilmPrint),
               std::invalid_argument);
}

// ---------------------------------------------------------------------------------------------
// Measurement record (b) and (c) of study section 8: CIEDE2000 against an SDK-style rendering.
// ---------------------------------------------------------------------------------------------

auto ToMatx(const color::Matrix33d& m) -> cv::Matx33d {
  return {m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]};
}

auto Mul(const cv::Matx33d& m, const Rgb& v) -> Rgb {
  const cv::Vec3d out = m * cv::Vec3d(v[0], v[1], v[2]);
  return {static_cast<float>(out[0]), static_cast<float>(out[1]), static_cast<float>(out[2])};
}

auto Bradford(const cv::Vec3d& source_white, const cv::Vec3d& target_white) -> cv::Matx33d {
  const cv::Matx33d m(0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296);
  const cv::Vec3d   s = m * source_white;
  const cv::Vec3d   t = m * target_white;
  return m.inv() * cv::Matx33d::diag(cv::Vec3d(t[0] / s[0], t[1] / s[1], t[2] / s[2])) * m;
}

auto Lab(const cv::Vec3d& xyz, const cv::Vec3d& white) -> cv::Vec3d {
  const auto f = [](double t) {
    constexpr double kDelta = 6.0 / 29.0;
    return t > kDelta * kDelta * kDelta ? std::cbrt(t) : t / (3 * kDelta * kDelta) + 4.0 / 29.0;
  };
  const double fx = f(xyz[0] / white[0]);
  const double fy = f(xyz[1] / white[1]);
  const double fz = f(xyz[2] / white[2]);
  return {116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz)};
}

/// CIEDE2000 (Sharma, Wu, Dalal 2005) with kL = kC = kH = 1.
auto Ciede2000(const cv::Vec3d& lab1, const cv::Vec3d& lab2) -> double {
  constexpr double kPi  = 3.14159265358979323846;
  const auto       rad  = [](double degrees) { return degrees * kPi / 180.0; };
  const double     c1   = std::hypot(lab1[1], lab1[2]);
  const double     c2   = std::hypot(lab2[1], lab2[2]);
  const double     cbar = 0.5 * (c1 + c2);
  const double     c7   = std::pow(cbar, 7);
  const double     g    = 0.5 * (1 - std::sqrt(c7 / (c7 + std::pow(25.0, 7))));
  const double     a1   = (1 + g) * lab1[1];
  const double     a2   = (1 + g) * lab2[1];
  const double     cp1  = std::hypot(a1, lab1[2]);
  const double     cp2  = std::hypot(a2, lab2[2]);
  const auto       hue  = [&](double b, double a) {
    if (a == 0 && b == 0) return 0.0;
    double h = std::atan2(b, a) * 180.0 / kPi;
    return h < 0 ? h + 360.0 : h;
  };
  const double hp1 = hue(lab1[2], a1);
  const double hp2 = hue(lab2[2], a2);
  const double dl  = lab2[0] - lab1[0];
  const double dc  = cp2 - cp1;
  double       dh  = 0;
  if (cp1 * cp2 != 0) {
    dh = hp2 - hp1;
    if (dh > 180) dh -= 360;
    if (dh < -180) dh += 360;
  }
  const double dhh  = 2 * std::sqrt(cp1 * cp2) * std::sin(rad(dh / 2));
  const double lbar = 0.5 * (lab1[0] + lab2[0]);
  const double cpb  = 0.5 * (cp1 + cp2);
  double       hbar = hp1 + hp2;
  if (cp1 * cp2 != 0) {
    if (std::abs(hp1 - hp2) <= 180) {
      hbar *= 0.5;
    } else {
      hbar = hbar < 360 ? 0.5 * (hbar + 360) : 0.5 * (hbar - 360);
    }
  }
  const double t = 1 - 0.17 * std::cos(rad(hbar - 30)) + 0.24 * std::cos(rad(2 * hbar)) +
                   0.32 * std::cos(rad(3 * hbar + 6)) - 0.20 * std::cos(rad(4 * hbar - 63));
  const double dtheta = 30 * std::exp(-std::pow((hbar - 275) / 25, 2));
  const double cpb7   = std::pow(cpb, 7);
  const double rc     = 2 * std::sqrt(cpb7 / (cpb7 + std::pow(25.0, 7)));
  const double l50    = (lbar - 50) * (lbar - 50);
  const double sl     = 1 + 0.015 * l50 / std::sqrt(20 + l50);
  const double sc     = 1 + 0.045 * cpb;
  const double sh     = 1 + 0.015 * cpb * t;
  const double rt     = -std::sin(rad(2 * dtheta)) * rc;
  return std::sqrt(std::pow(dl / sl, 2) + std::pow(dc / sc, 2) + std::pow(dhh / sh, 2) +
                   rt * (dc / sc) * (dhh / sh));
}

struct Statistics {
  double mean = 0;
  double p95  = 0;
  double max  = 0;
};

auto Summarize(std::vector<double> values) -> Statistics {
  std::sort(values.begin(), values.end());
  Statistics result;
  result.mean = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
  result.p95  = values[static_cast<std::size_t>(0.95 * (values.size() - 1))];
  result.max  = values.back();
  return result;
}

/// The Color Grade 1 contrast and saturation of the Default document on linear AP1, in ACEScc
/// order exposure (already in @p ap1), contrast, saturation (pipeline_document.cpp).
auto ApplyImportLook(const Rgb& ap1, float contrast, float saturation) -> Rgb {
  namespace ref = oklab_contrast_reference;
  Rgb cc{ref::AcesccEncode(ap1[0]), ref::AcesccEncode(ap1[1]), ref::AcesccEncode(ap1[2])};
  if (contrast != 0.0f) cc = ref::ApplyContrastAcescc(cc, contrast);
  const float luma = 0.272229f * cc[0] + 0.674082f * cc[1] + 0.053689f * cc[2];
  for (float& channel : cc) channel = luma + (channel - luma) * saturation;
  return {ref::AcesccDecode(cc[0]), ref::AcesccDecode(cc[1]), ref::AcesccDecode(cc[2])};
}

TEST(ProfileToneReferenceTest, RecordsCiede2000OfTheAces2StageAgainstTheSdkRendering) {
  const auto curve    = CameraLikeCurve();
  const auto response = ProfileToneNeutralResponse::Resolve(DrtMethod::Aces20);
  const auto aces     = ResolveAces2ReferenceRuntime(
      color::GamutPrimariesXy(color::ColorGamutId::Rec709), kProfileToneReferencePeakNits);
  const float*      packed           = aces->packed_.data();

  const auto&       stage            = ProfileToneStageMatrices();
  const cv::Matx33d prophoto_to_xyz  = ToMatx(color::RgbToXyzMatrix(color::ColorGamutId::ProPhoto));
  const cv::Matx33d rec709_to_xyz    = ToMatx(color::RgbToXyzMatrix(color::ColorGamutId::Rec709));
  const cv::Matx33d ap1_to_xyz       = ToMatx(color::RgbToXyzMatrix(color::ColorGamutId::Ap1));
  const cv::Matx33d ap0_to_xyz       = ToMatx(color::RgbToXyzMatrix(color::ColorGamutId::Ap0));
  const cv::Matx33d ap1_to_ap0       = ap0_to_xyz.inv() * ap1_to_xyz;
  const cv::Vec3d   d50              = prophoto_to_xyz * cv::Vec3d(1, 1, 1);
  const cv::Vec3d   d65              = rec709_to_xyz * cv::Vec3d(1, 1, 1);
  // SDK step 5 for sRGB: ProPhoto to XYZ D50, Bradford to D65, XYZ to Rec.709.
  const cv::Matx33d prophoto_to_srgb = rec709_to_xyz.inv() * Bradford(d50, d65) * prophoto_to_xyz;
  cv::Matx33d       prophoto_to_ap1;
  for (int i = 0; i < 9; ++i) prophoto_to_ap1.val[i] = stage.prophoto_to_ap1[i];

  const auto display_lab = [&](Rgb display) {
    for (float& channel : display) channel = std::clamp(channel, 0.0f, 1.0f);
    return Lab(rec709_to_xyz * cv::Vec3d(display[0], display[1], display[2]), d65);
  };
  const auto aces_forward = [&](const Rgb& ap1) {
    const Rgb       ap0     = Mul(ap1_to_ap0, ap1);
    const A2rFloat3 display = A2rAp0ToDisplay(A2rMake3(ap0[0], ap0[1], ap0[2]), packed);
    return Rgb{display.x, display.y, display.z};
  };

  // Patches in linear ProPhoto at the ACR scale: a neutral and 12 hues at two purities, at seven
  // levels from 7 stops below raw white to 0.75 stops below it.
  std::vector<Rgb> patches;
  for (float stops : {-7.0f, -5.5f, -4.0f, -3.5f, -2.5f, -1.5f, -0.75f}) {
    const float base = std::exp2(stops);
    patches.push_back({base, base, base});
    for (int hue = 0; hue < 360; hue += 30) {
      for (float purity : {0.3f, 0.6f}) {
        Rgb patch{};
        for (int c = 0; c < 3; ++c) {
          patch[c] = base * (1.0f + purity * std::cos((hue - 120.0f * c) * 3.14159265f / 180.0f));
        }
        patches.push_back(patch);
      }
    }
  }

  std::vector<double> per_channel;
  std::vector<double> full_inverse;
  std::vector<double> before_reset;
  for (const Rgb& patch : patches) {
    // SDK-style reference: exposure ramp clip, baseline RGB tone, ProPhoto to sRGB.
    Rgb sdk = patch;
    for (float& channel : sdk) channel = std::clamp(channel, 0.0f, 1.0f);
    ApplyDngBaselineRgbTone(*curve, sdk);
    const Rgb       sdk_display = Mul(prophoto_to_srgb, sdk);
    const cv::Vec3d reference   = display_lab(sdk_display);

    // Default document after the entry reset: exposure +1.5 EV, contrast 0, saturation 1.0.
    const Rgb anchored = Mul(prophoto_to_ap1 * std::exp2(kProfileToneExposureAnchorEv), patch);
    per_channel.push_back(Ciede2000(
        reference,
        display_lab(aces_forward(ApplyProfileToneStageReference(*curve, *response, anchored)))));

    // Alternative A4: the full ACES 2.0 inverse of the SDK display value.
    Rgb clamped = sdk_display;
    for (float& channel : clamped) channel = std::clamp(channel, 0.0f, 1.0f);
    const A2rFloat3 ap0 = A2rDisplayToAp0(A2rMake3(clamped[0], clamped[1], clamped[2]), packed);
    const A2rFloat3 a4  = A2rAp0ToDisplay(ap0, packed);
    full_inverse.push_back(Ciede2000(reference, display_lab({a4.x, a4.y, a4.z})));

    // Default document before the entry reset: the RAW import look stays on.
    const Rgb graded =
        ApplyImportLook(anchored, kImportLookContrastBeforeFilmLike, kDefaultPipelineSaturation);
    before_reset.push_back(Ciede2000(
        reference,
        display_lab(aces_forward(ApplyProfileToneStageReference(*curve, *response, graded)))));
  }

  const auto print = [&](const char* name, const std::vector<double>& values) {
    const auto stats = Summarize(values);
    std::printf("[measurement] CIEDE2000, %s: mean %.2f, p95 %.2f, max %.2f (%zu patches)\n", name,
                stats.mean, stats.p95, stats.max, values.size());
    return stats;
  };
  const auto stage_stats =
      print("per-channel inverse, Default document after the reset", per_channel);
  const auto worst = static_cast<std::size_t>(
      std::max_element(per_channel.begin(), per_channel.end()) - per_channel.begin());
  std::printf("[measurement]   largest at ProPhoto (%.4f, %.4f, %.4f); mean by level:",
              patches[worst][0], patches[worst][1], patches[worst][2]);
  for (std::size_t level = 0; level < patches.size(); level += 25) {
    std::printf(
        " %.2f",
        std::accumulate(per_channel.begin() + level, per_channel.begin() + level + 25, 0.0) / 25.0);
  }
  std::printf("\n");
  for (const auto method : kMethods) {
    std::printf("[measurement] %s scene value at the peak: %.2f\n", MethodName(method),
                ProfileToneNeutralResponse::Resolve(method)->SceneAtPeak());
  }
  const auto a4_stats     = print("full ACES 2.0 inverse (A4)", full_inverse);
  const auto before_stats = print("Default document before the reset", before_reset);
  EXPECT_TRUE(std::isfinite(stage_stats.max));
  EXPECT_TRUE(std::isfinite(a4_stats.max));
  EXPECT_TRUE(std::isfinite(before_stats.max));
  // The neutral patches below the knee of the highlight shoulder render as the profile renders
  // them. The neutral patch above the knee (0.75 stops below raw white) records the shoulder: the
  // SDK clips there and keeps the curve.
  std::printf("[measurement] neutral patch 0.75 stops below raw white: CIEDE2000 %.2f\n",
              per_channel[patches.size() - 25]);
  for (std::size_t i = 0; i < patches.size(); i += 25) {
    if (patches[i][0] <= kProfileToneHighlightKnee) {
      EXPECT_LT(per_channel[i], 0.1) << i;
    } else {
      EXPECT_LT(per_channel[i], 1.0) << i;
    }
  }
}

}  // namespace
}  // namespace alcedo
