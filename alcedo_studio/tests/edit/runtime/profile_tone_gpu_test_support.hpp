//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

// The profile tone stage at the entry of the display transform on one GPU backend
// (dcp_profile_tone_curve_study.md, phase T2). Each backend test renders only the DRT pass on an
// AP1 scene image and runs the checks of this file:
// - a neutral ramp renders the profile curve `C` with the highlight shoulder `H` through the DRT
//   (the host `N_m(stage)`), above raw white as well;
// - color patches match the scalar reference ApplyProfileToneStageReference;
// - a document without the stage renders the bytes of a document without a DCP reference.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "../graph/test_camera_profile.hpp"
#include "edit/graph/develop_color_transform.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/runtime/profile_tone_reference.hpp"
#include "image/dcp_camera_profile.hpp"
#include "image/dng_color_profile.hpp"
#include "image/dng_tone_curve.hpp"

namespace alcedo::profile_tone_gpu_test {

struct Rgba {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;
};

/// Render only the DRT pass of a document on an AP1 scene image; returns the display pixels.
using RenderDrt = std::function<std::vector<Rgba>(const PipelineDocument&, std::span<const Rgba>)>;

inline constexpr std::array<DrtMethod, 2> kMethods{DrtMethod::Aces20, DrtMethod::OpenDrt};

inline auto                               MethodName(DrtMethod method) -> const char* {
  return method == DrtMethod::Aces20 ? "ACES 2.0" : "OpenDRT";
}

/// The camera-like curve of ProfileToneReferenceTest: 128 points, 0.444 at scene 0.18.
inline auto CameraLikeCurve() -> DngToneCurvePtr {
  std::vector<DngToneCurvePoint> points;
  constexpr int                  kCount = 128;
  for (int i = 0; i < kCount; ++i) {
    const double x = std::pow(static_cast<double>(i) / (kCount - 1), 1.6);
    const double y = std::min(1.0, std::pow(x, 0.62) * (1.0 + 0.35 * (1.0 - x)));
    points.push_back({static_cast<float>(x), static_cast<float>(y)});
  }
  return MakeDngToneCurve(std::move(points));
}

/// A synthetic DCP profile with @p curve as its `ProfileToneCurve` (null: the file lacks the tag).
inline auto MakeToneProfile(DngToneCurvePtr curve) -> DcpCameraProfilePtr {
  DcpCameraProfile profile;
  profile.unique_camera_model = "Test Camera Z1";
  profile.profile_name        = curve ? "Camera Tone" : "Camera Flat";
  profile.color_matrix_1      = {0.92, -0.31, -0.06, -0.44, 1.21, 0.19, -0.08, 0.27, 0.68};
  profile.color_matrix_2      = {0.81, -0.22, -0.09, -0.41, 1.17, 0.27, -0.06, 0.18, 0.59};
  profile.color_matrix_1_cct  = 2856.0;
  profile.color_matrix_2_cct  = 6504.0;
  profile.tables              = MakeDngColorProfile(DngColorProfile{});
  profile.profile_tone_curve  = std::move(curve);
  return MakeDcpCameraProfile(std::move(profile));
}

/**
 * @brief The Default document with DRT method @p method at Rec.709 gamma 2.2 and 100 nits (the
 *        reference display of `N_m`), and a Develop reference to @p profile when it is set.
 */
inline auto MakeToneDocument(DrtMethod method, const DcpCameraProfilePtr& profile,
                             bool profile_tone = true) -> PipelineDocument {
  auto document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  DrtParameterUpdate drt;
  drt.method         = method;
  drt.encoding_space = DrtColorSpace::Rec709;
  drt.encoding_eotf  = DrtEotf::Gamma22;
  drt.limiting_space = DrtColorSpace::Rec709;
  drt.peak_luminance = kProfileToneReferencePeakNits;
  document.Drt()->Params().ApplyUpdate(drt);
  if (profile) {
    DevelopCameraProfileFileUpdate update;
    update.profile      = profile;
    update.profile_tone = profile_tone;
    document.Develop()->Params().ApplyCameraProfileFileUpdate(std::move(update));
  }
  return document;
}

/// Display light of a gamma 2.2 code value.
inline auto DecodeGamma22(float code) -> float { return std::pow(std::max(code, 0.0f), 2.2f); }

/// Rec.709 luminance of the display light of @p pixel.
inline auto DisplayLuminance(const Rgba& pixel) -> float {
  return 0.2126f * DecodeGamma22(pixel.r) + 0.7152f * DecodeGamma22(pixel.g) +
         0.0722f * DecodeGamma22(pixel.b);
}

/**
 * @brief Neutral ramp: the GPU display luminance against the profile curve with its highlight
 *        shoulder `C(H(v))` at the ACR exposure scale, and against the host `N_m(stage)`.
 *
 * The ramp runs from 12 stops below raw white to 4 stops above it (raw white is scene `2^E_a` at
 * the DRT entry) in quarter stops. The largest relative errors go to @p report for the measurement
 * record.
 */
inline void ExpectNeutralRampRendersTheProfileCurve(const RenderDrt& render, std::string* report) {
  const auto curve   = CameraLikeCurve();
  const auto profile = MakeToneProfile(curve);
  for (const auto method : kMethods) {
    const auto         response = ProfileToneNeutralResponse::Resolve(method);
    const auto         document = MakeToneDocument(method, profile);
    std::vector<Rgba>  ramp;
    std::vector<float> raw;
    for (float stops = -12.0f; stops <= 4.0f; stops += 0.25f) {
      const float value = std::exp2(stops + kProfileToneExposureAnchorEv);
      raw.push_back(std::exp2(stops));
      ramp.push_back({value, value, value, 1.0f});
    }
    const auto pixels = render(document, ramp);
    ASSERT_EQ(pixels.size(), ramp.size());
    double curve_error  = 0.0;
    double host_error   = 0.0;
    double neutral_cast = 0.0;
    for (std::size_t i = 0; i < pixels.size(); ++i) {
      const float gpu    = DisplayLuminance(pixels[i]);
      const float target = curve->Interpolate(ProfileToneHighlightShoulder(raw[i]));
      const auto  staged =
          ApplyProfileToneStageReference(*curve, *response, {ramp[i].r, ramp[i].g, ramp[i].b});
      const float host     = response->Forward(staged[1]);
      // Relative error above display light 1e-3; absolute below, where the encoding quantizes.
      const auto  relative = [](float a, float b) { return std::abs(a - b) / std::max(b, 1e-3f); };
      curve_error          = std::max(curve_error, static_cast<double>(relative(gpu, target)));
      host_error           = std::max(host_error, static_cast<double>(relative(gpu, host)));
      const float lo       = std::min({pixels[i].r, pixels[i].g, pixels[i].b});
      const float hi       = std::max({pixels[i].r, pixels[i].g, pixels[i].b});
      neutral_cast         = std::max(neutral_cast, static_cast<double>(hi - lo));
    }
    EXPECT_LT(curve_error, 1e-4) << MethodName(method);
    EXPECT_LT(host_error, 1e-4) << MethodName(method);
    if (report != nullptr) {
      char line[160];
      std::snprintf(line, sizeof(line),
                    "%s: relative error against C %.2e, against host N_m(stage) %.2e, largest "
                    "channel spread %.2e (code)\n",
                    MethodName(method), curve_error, host_error, neutral_cast);
      *report += line;
    }
  }
}

/// AP1 color patches at the DRT entry of the Default document: 12 hues at two purities and five
/// levels, a neutral, an out-of-ProPhoto color, and a negative channel.
inline auto ColorPatches() -> std::vector<Rgba> {
  std::vector<Rgba> patches;
  for (float stops : {-7.0f, -5.0f, -3.5f, -2.0f, -0.75f}) {
    const float level = std::exp2(stops + kProfileToneExposureAnchorEv);
    for (float purity : {0.3f, 0.6f}) {
      for (int hue = 0; hue < 12; ++hue) {
        const float          angle = 6.2831853f * static_cast<float>(hue) / 12.0f;
        std::array<float, 3> rgb{};
        for (int c = 0; c < 3; ++c) {
          rgb[c] = level * (1.0f + purity * std::cos(angle - 2.0943951f * static_cast<float>(c)));
        }
        patches.push_back({rgb[0], rgb[1], rgb[2], 0.5f});
      }
    }
    patches.push_back({level, level, level, 0.5f});
  }
  patches.push_back({0.02f, 0.9f, 3.5f, 0.5f});
  patches.push_back({-0.05f, 0.4f, 0.6f, 0.5f});
  patches.push_back({40.0f, 30.0f, 20.0f, 0.5f});
  return patches;
}

/**
 * @brief The GPU stage against the scalar reference: the stage on @p patches renders as the
 *        stage off on the reference output, through the same DRT kernel.
 *
 * The largest code difference goes to @p report.
 */
inline void ExpectPatchesMatchTheScalarReference(const RenderDrt& render, std::string* report) {
  const auto curve   = CameraLikeCurve();
  const auto profile = MakeToneProfile(curve);
  const auto patches = ColorPatches();
  for (const auto method : kMethods) {
    const auto        response = ProfileToneNeutralResponse::Resolve(method);
    std::vector<Rgba> staged;
    for (const auto& patch : patches) {
      const auto out =
          ApplyProfileToneStageReference(*curve, *response, {patch.r, patch.g, patch.b});
      staged.push_back({out[0], out[1], out[2], patch.a});
    }
    const auto on  = render(MakeToneDocument(method, profile, true), patches);
    const auto off = render(MakeToneDocument(method, profile, false), staged);
    ASSERT_EQ(on.size(), patches.size());
    ASSERT_EQ(off.size(), patches.size());
    double largest = 0.0;
    for (std::size_t i = 0; i < on.size(); ++i) {
      largest = std::max({largest, std::abs(static_cast<double>(on[i].r) - off[i].r),
                          std::abs(static_cast<double>(on[i].g) - off[i].g),
                          std::abs(static_cast<double>(on[i].b) - off[i].b)});
      EXPECT_EQ(on[i].a, patches[i].a) << MethodName(method) << " patch " << i;
    }
    // 0.02 of an 8-bit code; CUDA and OpenCL measure below 0.001 (study section 8, T2).
    EXPECT_LT(largest * 255.0, 0.02) << MethodName(method);
    if (report != nullptr) {
      char line[128];
      std::snprintf(line, sizeof(line), "%s: largest code difference against the reference %.4f\n",
                    MethodName(method), largest * 255.0);
      *report += line;
    }
  }
}

/// A document without the stage (`profile_tone` false, or a profile that lacks the curve) renders
/// the bytes of the document without a DCP reference.
inline void ExpectDocumentsWithoutTheStageRenderUnchangedBytes(const RenderDrt& render) {
  const auto patches = ColorPatches();
  for (const auto method : kMethods) {
    const auto plain = render(MakeToneDocument(method, nullptr), patches);
    const auto off =
        render(MakeToneDocument(method, MakeToneProfile(CameraLikeCurve()), false), patches);
    const auto no_tag = render(MakeToneDocument(method, MakeToneProfile(nullptr)), patches);
    const auto on = render(MakeToneDocument(method, MakeToneProfile(CameraLikeCurve())), patches);
    ASSERT_EQ(plain.size(), patches.size());
    ASSERT_EQ(off.size(), patches.size());
    ASSERT_EQ(no_tag.size(), patches.size());
    EXPECT_EQ(std::memcmp(plain.data(), off.data(), plain.size() * sizeof(Rgba)), 0)
        << MethodName(method);
    EXPECT_EQ(std::memcmp(plain.data(), no_tag.data(), plain.size() * sizeof(Rgba)), 0)
        << MethodName(method);
    // The stage changes the render, so the comparisons above see the stage when it runs.
    EXPECT_NE(std::memcmp(plain.data(), on.data(), plain.size() * sizeof(Rgba)), 0)
        << MethodName(method);
  }
}

}  // namespace alcedo::profile_tone_gpu_test
