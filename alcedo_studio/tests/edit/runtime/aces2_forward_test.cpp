//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Host tests of the OpenColorIO 2.5.1 ACES 2.0 output transform forward (aces2_reference_math.h)
// against the OCIO CPU processor, and of the forward and inverse as a pair
// (lut_color_encoding_plan.md, phase L2).

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <vector>

#include "aces2_ocio_reference.hpp"
#include "color/color_encoding_catalog.hpp"
#include "color/color_encoding_math.h"
#include "edit/runtime/aces2_reference_math.h"
#include "edit/runtime/drt/aces2_reference_runtime.hpp"

namespace alcedo {
namespace {

using aces2_ocio_reference::Aces2Cases;
using aces2_ocio_reference::WithinInverseTolerance;

constexpr int kGridSteps = 33;

/// Apply @p fn to every interleaved RGB triple of @p rgb.
template <typename Fn>
auto MapRgb(const std::vector<float>& rgb, Fn fn) -> std::vector<float> {
  std::vector<float> out(rgb.size());
  for (std::size_t i = 0; i < rgb.size(); i += 3) {
    const A2rFloat3 v = fn(A2rMake3(rgb[i], rgb[i + 1], rgb[i + 2]));
    out[i]            = v.x;
    out[i + 1]        = v.y;
    out[i + 2]        = v.z;
  }
  return out;
}

auto HostForward(const Aces2ReferenceRuntime& runtime, const std::vector<float>& ap0)
    -> std::vector<float> {
  return MapRgb(ap0, [&](A2rFloat3 v) { return A2rAp0ToDisplay(v, runtime.packed_.data()); });
}

auto HostInverse(const Aces2ReferenceRuntime& runtime, const std::vector<float>& display)
    -> std::vector<float> {
  return MapRgb(display, [&](A2rFloat3 v) { return A2rDisplayToAp0(v, runtime.packed_.data()); });
}

/// Per-channel scene values of the grids: 0, then ACEScc code values evenly spaced over (0, 1]
/// decoded to linear (about 1.2e-3 to 222), the range the ACEScc working space covers.
auto SceneLevels() -> std::array<float, kGridSteps> {
  std::array<float, kGridSteps> levels{};
  for (int i = 1; i < kGridSteps; ++i) {
    levels[i] = CeAcesccDecode(static_cast<float>(i) / (kGridSteps - 1));
  }
  return levels;
}

/// 33^3 AP0 grid over SceneLevels(), RGB interleaved.
auto Ap0Grid() -> std::vector<float> {
  const auto         levels = SceneLevels();
  std::vector<float> rgb;
  rgb.reserve(kGridSteps * kGridSteps * kGridSteps * 3);
  for (const float b : levels) {
    for (const float g : levels) {
      for (const float r : levels) {
        rgb.insert(rgb.end(), {r, g, b});
      }
    }
  }
  return rgb;
}

/// 33^3 AP1 grid over SceneLevels() converted to AP0, RGB interleaved. Every point is a
/// non-negative AP1 color below the forward limit, the domain the raster inverse returns.
auto Ap1GridAsAp0(const Aces2ReferenceRuntime& runtime) -> std::vector<float> {
  const auto ap1_to_ap0 =
      color::RgbToRgbMatrix(color::GamutPrimariesXy(color::ColorGamutId::Ap1),
                            color::GamutPrimariesXy(color::ColorGamutId::Ap0),
                            color::ChromaticAdaptation::Cat02);  // same white: no adaptation
  const auto         levels = SceneLevels();
  const float        limit  = runtime.packed_[ALCEDO_A2R_AP1_MAX];
  std::vector<float> rgb;
  for (const float b : levels) {
    for (const float g : levels) {
      for (const float r : levels) {
        if (r > limit || g > limit || b > limit) {
          continue;
        }
        for (int row = 0; row < 3; ++row) {
          rgb.push_back(static_cast<float>(ap1_to_ap0[row * 3] * r + ap1_to_ap0[row * 3 + 1] * g +
                                           ap1_to_ap0[row * 3 + 2] * b));
        }
      }
    }
  }
  return rgb;
}

TEST(Aces2ForwardTest, Aces2ForwardHostReferenceMatchesOcioCpuProcessor) {
  const auto grid = Ap0Grid();
  for (const auto& c : Aces2Cases()) {
    SCOPED_TRACE(c.name_);
    const auto runtime  = BuildAces2ReferenceRuntime(c.primaries_, c.peak_nits_);
    const auto actual   = HostForward(runtime, grid);
    const auto expected = aces2_ocio_reference::OcioForward(c, grid);
    int        failures = 0;
    int        ocio_nan = 0;
    for (std::size_t pixel = 0; pixel < grid.size() / 3; ++pixel) {
      const float* e = expected.data() + pixel * 3;
      const float* a = actual.data() + pixel * 3;
      if (std::isnan(e[0]) || std::isnan(e[1]) || std::isnan(e[2])) {
        // OCIO returns NaN for a negative achromatic response; the port returns black.
        ++ocio_nan;
        EXPECT_EQ(a[0], 0.0f);
        EXPECT_EQ(a[1], 0.0f);
        EXPECT_EQ(a[2], 0.0f);
        continue;
      }
      for (int ch = 0; ch < 3; ++ch) {
        if (!WithinInverseTolerance(a[ch], e[ch]) && ++failures <= 8) {
          ADD_FAILURE() << "AP0 (" << grid[pixel * 3] << ", " << grid[pixel * 3 + 1] << ", "
                        << grid[pixel * 3 + 2] << ") channel " << ch << " = " << a[ch] << ", OCIO "
                        << e[ch];
        }
      }
    }
    EXPECT_EQ(failures, 0);
    std::printf("%s: %d of %zu grid points are NaN in OCIO (negative achromatic response)\n",
                c.name_, ocio_nan, grid.size() / 3);
  }
}

TEST(Aces2ForwardTest, Aces2ForwardMapsBlackToBlackAndAp0NeutralToRisingDisplayNeutral) {
  for (const auto& c : Aces2Cases()) {
    SCOPED_TRACE(c.name_);
    const auto runtime = BuildAces2ReferenceRuntime(c.primaries_, c.peak_nits_);
    EXPECT_EQ(HostForward(runtime, {0.0f, 0.0f, 0.0f}), (std::vector<float>{0.0f, 0.0f, 0.0f}));
    // No white simulation: the ACES white maps to the display white at every level. The
    // reference does not clamp, so very bright neutrals may exceed the peak slightly.
    float previous = 0.0f;
    for (const float level : {0.001f, 0.18f, 1.0f, 16.0f, 200.0f}) {
      const auto display  = HostForward(runtime, {level, level, level});
      const auto [lo, hi] = std::minmax({display[0], display[1], display[2]});
      ASSERT_GT(lo, 0.0f) << "level " << level;
      EXPECT_LT(hi / lo - 1.0f, 1e-4f) << "level " << level << " display " << display[0] << ", "
                                       << display[1] << ", " << display[2];
      EXPECT_GT(lo, previous) << "level " << level;
      previous = hi;
    }
  }
}

/// Points and worst errors of one CheckRoundTrip call.
struct RoundTripResult {
  std::size_t points_           = 0;
  std::size_t direct_points_    = 0;
  float       worst_error_      = 0.0f;
  float       worst_ocio_error_ = 0.0f;
};

/**
 * Check the round trip @p ours of @p input at @p pixels against @p tolerance, or against OCIO's
 * own round trip @p ocio.
 *
 * ACES 2.0 is not invertible everywhere, and OCIO's own forward/inverse pair misses at the same
 * points (display gamut corners outside the reach gamut, the top of the tonescale). A channel
 * passes when its error, divided by max(@p scale_floor, |input|), is within @p tolerance, or at
 * most @p fallback_tolerance above OCIO's own error at that point. The fallback is R2's AP0
 * tolerance (1e-3): near the peak the R2 inverse differs from OCIO's by table rounding, which R2
 * accepts. The caller checks how many points pass @p tolerance directly.
 */
auto CheckRoundTrip(const std::vector<float>& input, const std::vector<float>& ours,
                    const std::vector<float>& ocio, const std::vector<std::size_t>& pixels,
                    float scale_floor, float tolerance, float fallback_tolerance)
    -> RoundTripResult {
  RoundTripResult result;
  int             failures = 0;
  for (const auto pixel : pixels) {
    bool direct = true;
    for (std::size_t ch = 0; ch < 3; ++ch) {
      const std::size_t i          = pixel * 3 + ch;
      const float       scale      = std::max(scale_floor, std::abs(input[i]));
      const float       error      = std::abs(ours[i] - input[i]) / scale;
      const float       ocio_error = std::abs(ocio[i] - input[i]) / scale;
      result.worst_error_          = std::max(result.worst_error_, error);
      result.worst_ocio_error_     = std::max(result.worst_ocio_error_, ocio_error);
      if (error <= tolerance) {
        continue;
      }
      direct = false;
      if (!(error <= ocio_error + fallback_tolerance) && ++failures <= 8) {
        ADD_FAILURE() << "input (" << input[pixel * 3] << ", " << input[pixel * 3 + 1] << ", "
                      << input[pixel * 3 + 2] << ") channel " << ch << " round trip " << ours[i]
                      << ", OCIO round trip " << ocio[i];
      }
    }
    ++result.points_;
    result.direct_points_ += direct ? 1 : 0;
  }
  EXPECT_EQ(failures, 0);
  return result;
}

TEST(Aces2ForwardTest, InverseOfForwardReturnsSceneValueWithin1e3InsideTheForwardLimit) {
  for (const auto& c : Aces2Cases()) {
    SCOPED_TRACE(c.name_);
    const auto               runtime = BuildAces2ReferenceRuntime(c.primaries_, c.peak_nits_);
    const auto               ap0     = Ap1GridAsAp0(runtime);
    const auto               display = HostForward(runtime, ap0);
    const float              peak    = c.peak_nits_ / 100.0f;
    // Scene values the pair carries: the forward result is a display value the inverse accepts
    // (inside the display cube, which the inverse clamps to) below 99 % of the peak. Above that
    // the tonescale is so flat that one float step of display value spans a large scene range;
    // OCIO's own pair errs by up to 2e8 there.
    std::vector<std::size_t> carried;
    for (std::size_t pixel = 0; pixel < ap0.size() / 3; ++pixel) {
      const float* d = display.data() + pixel * 3;
      if (std::min({d[0], d[1], d[2]}) >= 0.0f && std::max({d[0], d[1], d[2]}) <= 0.99f * peak) {
        carried.push_back(pixel);
      }
    }
    const auto ocio_round_trip =
        aces2_ocio_reference::OcioInverse(c, aces2_ocio_reference::OcioForward(c, ap0));
    // Error relative to the scene value, absolute below 1.0.
    const auto result = CheckRoundTrip(ap0, HostInverse(runtime, display), ocio_round_trip, carried,
                                       1.0f, 1e-3f, 1e-3f);
    EXPECT_GE(result.points_, 3000u);
    EXPECT_GE(result.direct_points_, result.points_ * 995 / 1000);
    std::printf(
        "%s: %zu of %zu AP1 grid points carried, %zu within 1e-3; worst error %.3g "
        "(OCIO pair %.3g)\n",
        c.name_, result.points_, ap0.size() / 3, result.direct_points_, result.worst_error_,
        result.worst_ocio_error_);
  }
}

TEST(Aces2ForwardTest, ForwardOfInverseReturnsDisplayValueWithin1e4) {
  for (const auto& c : Aces2Cases()) {
    SCOPED_TRACE(c.name_);
    const auto               runtime = BuildAces2ReferenceRuntime(c.primaries_, c.peak_nits_);
    // Display values in [0, 1] of the display's range (1.0 = peak).
    const auto               display = aces2_ocio_reference::DisplayGrid(c.peak_nits_);
    const float              peak    = c.peak_nits_ / 100.0f;
    std::vector<std::size_t> all(display.size() / 3);
    for (std::size_t i = 0; i < all.size(); ++i) {
      all[i] = i;
    }
    const auto ocio_round_trip =
        aces2_ocio_reference::OcioForward(c, aces2_ocio_reference::OcioInverse(c, display));
    // Error relative to the peak (every input is at most the peak).
    const auto result = CheckRoundTrip(display, HostForward(runtime, HostInverse(runtime, display)),
                                       ocio_round_trip, all, peak, 1e-4f, 1e-3f);
    EXPECT_GE(result.direct_points_, result.points_ * 995 / 1000);
    std::printf(
        "%s: %zu of %zu display grid points within 1e-4 of peak; worst error %.3g "
        "(OCIO pair %.3g)\n",
        c.name_, result.direct_points_, result.points_, result.worst_error_,
        result.worst_ocio_error_);
  }
}

TEST(Aces2ForwardTest, ForwardMatricesAreInversesOfTheInverseDirectionMatrices) {
  for (const auto display_gamut : {color::ColorGamutId::Rec709, color::ColorGamutId::ProPhoto}) {
    const auto runtime = BuildAces2ReferenceRuntime(color::GamutPrimariesXy(display_gamut), 100.0f);
    const float* p     = runtime.packed_.data();
    const auto   check = [&](int a_offset, int b_offset, const char* what) {
      for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
          double sum = 0.0, magnitude = 0.0;
          for (int k = 0; k < 3; ++k) {
            const double term =
                static_cast<double>(p[a_offset + row * 3 + k]) * p[b_offset + k * 3 + col];
            sum += term;
            magnitude += std::abs(term);
          }
          // Single-precision entries: the product is exact up to the float rounding of the terms.
          EXPECT_NEAR(sum, row == col ? 1.0 : 0.0, 1e-6 * magnitude)
              << what << " [" << row << "][" << col << "] limiting is AP1 "
              << runtime.limiting_is_ap1_;
        }
      }
    };
    check(ALCEDO_A2R_AP0_RGB_TO_CAM16, ALCEDO_A2R_AP0_CAM16_TO_RGB, "AP0 CAM16");
    check(ALCEDO_A2R_AP0_CONE_TO_AAB, ALCEDO_A2R_AP0_AAB_TO_CONE, "AP0 Aab");
    check(ALCEDO_A2R_LIMIT_RGB_TO_CAM16, ALCEDO_A2R_LIMIT_CAM16_TO_RGB, "limiting CAM16");
    check(ALCEDO_A2R_LIMIT_CONE_TO_AAB, ALCEDO_A2R_LIMIT_AAB_TO_CONE, "limiting Aab");
    check(ALCEDO_A2R_DISPLAY_TO_LIMIT, ALCEDO_A2R_LIMIT_TO_DISPLAY, "display limiting");
  }
}

TEST(Aces2ForwardTest, DisplayOutsideAp1RoundTripsNeutralsThroughAp1Limiting) {
  // ProPhoto has primaries outside AP1, so AP1 is the limiting gamut and the forward converts
  // back to ProPhoto with the inverse of the display-to-limiting matrix.
  const auto runtime =
      BuildAces2ReferenceRuntime(color::GamutPrimariesXy(color::ColorGamutId::ProPhoto), 100.0f);
  ASSERT_TRUE(runtime.limiting_is_ap1_);
  for (const float level : {0.05f, 0.18f, 0.5f, 0.9f}) {
    const auto round_trip = HostForward(runtime, HostInverse(runtime, {level, level, level}));
    for (const float v : round_trip) {
      EXPECT_NEAR(v, level, 1e-4f) << "level " << level;
    }
  }
}

}  // namespace
}  // namespace alcedo
