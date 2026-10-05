//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Host tests of the ACES 2.0 inverse runtime (raster_image_input_plan.md, section 5.7): the
// tables against OpenColorIO 2.5.1, the runtime cache, and the host evaluation of the shared
// per-pixel code (display_to_ap1_math.h) against the OCIO CPU processor.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "aces2_inverse_ocio_reference.hpp"
#include "edit/runtime/display_to_ap1_math.h"
#include "edit/runtime/drt/aces2_inverse_runtime.hpp"
#include "image/raster_color_description.hpp"

namespace alcedo {
namespace {

using aces2_inverse_test::InverseCases;

auto HostDisplayToAp0(const Aces2InverseRuntime& runtime, const std::vector<float>& rgb)
    -> std::vector<float> {
  std::vector<float> out(rgb.size());
  for (std::size_t i = 0; i < rgb.size(); i += 3) {
    const auto ap0 =
        D2aDisplayToAp0(D2aMake3(rgb[i], rgb[i + 1], rgb[i + 2]), runtime.packed_.data());
    out[i]     = ap0.x;
    out[i + 1] = ap0.y;
    out[i + 2] = ap0.z;
  }
  return out;
}

auto AcesccDecode(float value) -> float {
  constexpr float kA = 9.72f, kB = 17.52f, kOffset = 0.0000152587890625f;
  constexpr float kFloor = (-16.0f + kA) / kB, kThreshold = (-15.0f + kA) / kB;
  if (value < kFloor) return value - kFloor;
  if (value <= kThreshold) return (std::exp2(value * kB - kA) - kOffset) * 2.0f;
  return std::exp2(value * kB - kA);
}

void ExpectTableNear(std::span<const float> actual, const std::vector<float>& expected,
                     float relative, float absolute, const char* what, const char* case_name) {
  ASSERT_EQ(actual.size(), expected.size()) << what << " " << case_name;
  int failures = 0;
  for (std::size_t i = 0; i < expected.size(); ++i) {
    const float tolerance = std::max(absolute, relative * std::abs(expected[i]));
    if (!(std::abs(actual[i] - expected[i]) <= tolerance) && ++failures <= 5) {
      ADD_FAILURE() << case_name << " " << what << "[" << i << "] actual " << actual[i]
                    << " expected " << expected[i];
    }
  }
  EXPECT_EQ(failures, 0) << case_name << " " << what;
}

TEST(Aces2InverseTest, Aces2InverseHostTablesMatchOcioWithinTolerance) {
  for (const auto& c : InverseCases()) {
    const auto runtime = BuildAces2InverseRuntime(c.primaries_, c.peak_nits_);
    const auto ocio    = aces2_inverse_test::ExtractOcioInverseTables(c);
    ASSERT_EQ(ocio.reach_m_.size(), static_cast<std::size_t>(ALCEDO_D2A_TABLE_SIZE)) << c.name_;
    ASSERT_EQ(ocio.hues_.size(), static_cast<std::size_t>(ALCEDO_D2A_TABLE_SIZE)) << c.name_;
    ASSERT_EQ(ocio.cusps_.size(), static_cast<std::size_t>(3 * ALCEDO_D2A_TABLE_SIZE)) << c.name_;
    // The reach search stops at a 0.01 bracket.
    ExpectTableNear(runtime.ReachMTable(), ocio.reach_m_, 1e-5f, 0.011f, "reach_m", c.name_);
    // OCIO prints the hue array into the shader text with limited digits.
    ExpectTableNear(runtime.HueTable(), ocio.hues_, 1e-6f, 2e-4f, "hue", c.name_);
    // Cusp J and M agree to float rounding. The upper hull gamma comes from a threshold search
    // (does the boundary estimate leave the display cube?), which turns that rounding into a
    // gamma difference of up to a few 1e-4; the warm start does not change it.
    std::vector<float> ocio_jm, ocio_gamma, our_jm, our_gamma;
    for (int i = 0; i < ALCEDO_D2A_TABLE_SIZE; ++i) {
      ocio_jm.push_back(ocio.cusps_[i * 3]);
      ocio_jm.push_back(ocio.cusps_[i * 3 + 1]);
      ocio_gamma.push_back(ocio.cusps_[i * 3 + 2]);
      our_jm.push_back(runtime.CuspTable()[i * 3]);
      our_jm.push_back(runtime.CuspTable()[i * 3 + 1]);
      our_gamma.push_back(runtime.CuspTable()[i * 3 + 2]);
    }
    ExpectTableNear(our_jm, ocio_jm, 1e-5f, 1e-6f, "cusp J/M", c.name_);
    ExpectTableNear(our_gamma, ocio_gamma, 5e-4f, 1e-6f, "upper hull gamma inverse", c.name_);
    EXPECT_EQ(runtime.hue_linearity_search_range_, ocio.hue_search_range_) << c.name_;
  }
}

TEST(Aces2InverseTest, Aces2InverseHostReferenceMatchesOcioCpuProcessor) {
  for (const auto& c : InverseCases()) {
    SCOPED_TRACE(c.name_);
    const auto runtime = BuildAces2InverseRuntime(c.primaries_, c.peak_nits_);
    ASSERT_FALSE(runtime.limiting_is_ap1_);
    const auto grid = aces2_inverse_test::DisplayGrid(c.peak_nits_);
    aces2_inverse_test::ExpectMatchesOcio(c, grid, HostDisplayToAp0(runtime, grid));
  }
}

TEST(Aces2InverseTest, Aces2InverseReturnsBlackForBlackAndNeutralForSourceWhite) {
  for (const auto& c : InverseCases()) {
    SCOPED_TRACE(c.name_);
    const auto runtime = BuildAces2InverseRuntime(c.primaries_, c.peak_nits_);
    const auto black   = HostDisplayToAp0(runtime, {0.0f, 0.0f, 0.0f});
    EXPECT_EQ(black, (std::vector<float>{0.0f, 0.0f, 0.0f}));
    const float peak = c.peak_nits_ / 100.0f;
    for (const float fraction : {0.0005f, 0.05f, 0.5f, 0.9f}) {
      const float level   = fraction * peak;
      const auto  white   = HostDisplayToAp0(runtime, {level, level, level});
      const auto [lo, hi] = std::minmax({white[0], white[1], white[2]});
      ASSERT_GT(lo, 0.0f) << "level " << level;
      EXPECT_LT(hi / lo - 1.0f, 1e-4f)
          << "level " << level << " AP0 " << white[0] << ", " << white[1] << ", " << white[2];
    }
    // Peak white maps to the top of the tonescale, where the inverse amplifies the float
    // rounding of a near-zero M. It must still match OCIO within the section 5.7 tolerance.
    const auto peak_white = HostDisplayToAp0(runtime, {peak, peak, peak});
    const auto ocio_peak  = aces2_inverse_test::OcioInverse(c, {peak, peak, peak});
    for (int i = 0; i < 3; ++i) {
      EXPECT_TRUE(aces2_inverse_test::WithinInverseTolerance(peak_white[i], ocio_peak[i]))
          << "peak channel " << i << " " << peak_white[i] << " vs " << ocio_peak[i];
    }
  }
}

TEST(Aces2InverseTest, Aces2InverseRuntimeIsBuiltOncePerPrimariesAndPeak) {
  // Unusual peaks keep these keys out of every other test in the process.
  const auto before = Aces2InverseRuntimeBuildCount();
  const auto a      = ResolveAces2InverseRuntime(kRasterPrimariesDisplayP3, 123.0f);
  const auto b      = ResolveAces2InverseRuntime(kRasterPrimariesDisplayP3, 123.0f);
  EXPECT_EQ(a.get(), b.get());
  EXPECT_EQ(Aces2InverseRuntimeBuildCount(), before + 1);
  const auto other_peak  = ResolveAces2InverseRuntime(kRasterPrimariesDisplayP3, 124.0f);
  const auto other_space = ResolveAces2InverseRuntime(kRasterPrimariesRec709, 123.0f);
  EXPECT_NE(other_peak.get(), a.get());
  EXPECT_NE(other_space.get(), a.get());
  EXPECT_EQ(Aces2InverseRuntimeBuildCount(), before + 3);
  EXPECT_EQ(a->packed_.size(), static_cast<std::size_t>(ALCEDO_D2A_PACKED_SIZE));
}

TEST(Aces2InverseTest, SourceOutsideAp1UsesAp1LimitingAndKeepsSourceWhiteNeutral) {
  EXPECT_TRUE(SourcePrimariesInsideAp1(kRasterPrimariesRec2020));
  EXPECT_TRUE(SourcePrimariesInsideAp1(kRasterPrimariesAdobeRgb));
  EXPECT_FALSE(SourcePrimariesInsideAp1(kRasterPrimariesProPhoto));

  const auto runtime = BuildAces2InverseRuntime(kRasterPrimariesProPhoto, 100.0f);
  EXPECT_TRUE(runtime.limiting_is_ap1_);
  EXPECT_FLOAT_EQ(runtime.limiting_primaries_xy_[0], 0.713f);
  // ProPhoto white (D50) is adapted to the AP1 white before the inverse, so it stays neutral.
  const auto white    = HostDisplayToAp0(runtime, {0.5f, 0.5f, 0.5f});
  const auto [lo, hi] = std::minmax({white[0], white[1], white[2]});
  ASSERT_GT(lo, 0.0f);
  EXPECT_LT(hi / lo - 1.0f, 1e-3f);
  // A saturated ProPhoto green leaves the AP1 limiting gamut and is clamped, not NaN.
  const auto green = HostDisplayToAp0(runtime, {0.0f, 1.0f, 0.0f});
  for (const float v : green) {
    EXPECT_TRUE(std::isfinite(v));
  }
}

TEST(Aces2InverseTest, AcesccOutputClampsAp1AndEncodesLikeCameraToAp1) {
  const auto  runtime = BuildAces2InverseRuntime(kRasterPrimariesRec709, 100.0f);
  const auto& p       = runtime.packed_;
  for (const auto& rgb :
       {std::array<float, 3>{0.18f, 0.18f, 0.18f}, std::array<float, 3>{1.0f, 0.0f, 0.0f},
        std::array<float, 3>{0.2f, 0.6f, 0.9f}}) {
    const auto   ap0 = D2aDisplayToAp0(D2aMake3(rgb[0], rgb[1], rgb[2]), p.data());
    const auto   cc  = D2aSourceToAcesccAp1(D2aMake3(rgb[0], rgb[1], rgb[2]), p.data());
    const float* m   = p.data() + ALCEDO_D2A_AP0_TO_AP1;
    const std::array<float, 3> ap1     = {m[0] * ap0.x + m[1] * ap0.y + m[2] * ap0.z,
                                          m[3] * ap0.x + m[4] * ap0.y + m[5] * ap0.z,
                                          m[6] * ap0.x + m[7] * ap0.y + m[8] * ap0.z};
    const std::array<float, 3> decoded = {AcesccDecode(cc.x), AcesccDecode(cc.y),
                                          AcesccDecode(cc.z)};
    for (int i = 0; i < 3; ++i) {
      const float expected = std::clamp(ap1[i], 0.0f, p[ALCEDO_D2A_AP1_MAX]);
      EXPECT_NEAR(decoded[i], expected, 1e-4f * std::max(1.0f, expected));
    }
  }
}

TEST(Aces2InverseTest, SceneLinearMatrixMapsSourceWhiteToAp1Neutral) {
  for (const auto& primaries : {kRasterPrimariesRec709, kRasterPrimariesDisplayP3,
                                kRasterPrimariesProPhoto, kRasterPrimariesAp0}) {
    const auto packed = PackSceneLinearToAp1(primaries);
    EXPECT_EQ(packed[ALCEDO_D2A_BRANCH], 1.0f);
    const auto cc = D2aSourceToAcesccAp1(D2aMake3(0.18f, 0.18f, 0.18f), packed.data());
    EXPECT_NEAR(AcesccDecode(cc.x), 0.18f, 1e-4f);
    EXPECT_NEAR(AcesccDecode(cc.y), 0.18f, 1e-4f);
    EXPECT_NEAR(AcesccDecode(cc.z), 0.18f, 1e-4f);
  }
  // AP0 red lies outside AP1; the reference gamut compression keeps the result finite and
  // bounded below.
  const auto ap0 = PackSceneLinearToAp1(kRasterPrimariesAp0);
  const auto red = D2aSourceToAcesccAp1(D2aMake3(1.0f, 0.0f, 0.0f), ap0.data());
  EXPECT_GT(AcesccDecode(red.y), -0.1f);
  EXPECT_GT(AcesccDecode(red.z), -0.1f);
}

TEST(Aces2InverseTest, TableBuildTimeIsMeasured) {
  // Section 5.4 target: at most 20 ms per new key in a release build. A debug build only
  // reports the time.
  const auto start   = std::chrono::steady_clock::now();
  const auto runtime = BuildAces2InverseRuntime(kRasterPrimariesAdobeRgb, 1000.0f);
  const auto elapsed =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  RecordProperty("table_build_ms", std::to_string(elapsed));
  std::printf("ACES 2.0 inverse table build: %.2f ms\n", elapsed);
  EXPECT_EQ(runtime.packed_.size(), static_cast<std::size_t>(ALCEDO_D2A_PACKED_SIZE));
#ifdef NDEBUG
  EXPECT_LE(elapsed, 20.0);
#endif
}

}  // namespace
}  // namespace alcedo
