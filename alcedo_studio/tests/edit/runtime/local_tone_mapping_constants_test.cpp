//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include "edit/runtime/local_tone_mapping.hpp"

namespace alcedo {
namespace {

namespace tone = local_tone_mapping;

struct TestToneParams {
  std::uint64_t hs_mask_base_cache_key_      = 0;
  bool          shadows_enabled_             = false;
  bool          highlights_enabled_          = false;
  bool          render_roi_enabled_          = false;
  int           render_roi_x_                = 0;
  int           render_roi_y_                = 0;
  float         render_roi_scale_x_          = 1.0f;
  float         render_roi_scale_y_          = 1.0f;
  int           render_roi_reference_width_  = 0;
  int           render_roi_reference_height_ = 0;
};

auto AcesccDecode(float acescc) -> float {
  constexpr float kLog2Min         = -15.0f;
  constexpr float kLog2Denorm      = -16.0f;
  constexpr float kDenormOffset    = 0.00001525878906f;
  constexpr float kA               = 9.72f;
  constexpr float kB               = 17.52f;

  const float     encode_floor     = (kLog2Denorm + kA) / kB;
  const float     denorm_threshold = (kLog2Min + kA) / kB;
  if (acescc < encode_floor) {
    return acescc - encode_floor;
  }
  if (acescc <= denorm_threshold) {
    return (std::exp2(acescc * kB - kA) - kDenormOffset) * 2.0f;
  }
  return std::exp2(acescc * kB - kA);
}

auto AcesccEncode(float linear_ap1) -> float {
  constexpr float kLog2Denorm   = -16.0f;
  constexpr float kDenormTrans  = 0.00003051757812f;
  constexpr float kDenormOffset = 0.00001525878906f;
  constexpr float kA            = 9.72f;
  constexpr float kB            = 17.52f;

  const float     encode_floor  = (kLog2Denorm + kA) / kB;
  if (linear_ap1 <= 0.0f) {
    return encode_floor + linear_ap1;
  }
  if (linear_ap1 < kDenormTrans) {
    return (std::log2(kDenormOffset + linear_ap1 * 0.5f) + kA) / kB;
  }
  return (std::log2(linear_ap1) + kA) / kB;
}

}  // namespace

TEST(LocalToneMappingConstantsMatchRuntime, BuildSamplesCoversConfiguredGammaDomain) {
  const auto  samples = tone::BuildSamples(0.75f, 0.65f);
  const float expected_step =
      std::max(tone::kBaseSigmaR * tone::kGammaStepScale, tone::kMinSampleStep);
  const int expected_count = std::max(
      2, static_cast<int>(std::ceil((tone::kGammaMaxL - tone::kGammaMinL) / expected_step)) + 1);

  ASSERT_EQ(static_cast<int>(samples.size()), expected_count);
  EXPECT_NEAR(samples.front().gamma, tone::kGammaMinL, 1.0e-6f);
  EXPECT_NEAR(samples.back().gamma, tone::kGammaMaxL, 1.0e-6f);
  for (std::size_t i = 1; i < samples.size(); ++i) {
    EXPECT_GT(samples[i].gamma, samples[i - 1].gamma);
  }

  const auto& mid = samples[samples.size() / 2];
  EXPECT_FLOAT_EQ(mid.target, tone::ApplyReferenceCurve(mid.gamma, 0.75f, 0.65f));
  EXPECT_FLOAT_EQ(mid.beta, tone::ToneBeta(mid.gamma, 0.75f, 0.65f));
  EXPECT_FLOAT_EQ(mid.alpha, tone::DetailAlpha(mid.gamma, 0.75f, 0.65f));
}

TEST(LocalToneMappingConstantsMatchRuntime, ReferenceCurvePreservesExpectedToneDirections) {
  const float shadow_l    = tone::kAcesccMiddleGray - 4.5f * tone::kAcesccCodePerEv;
  const float highlight_l = tone::kAcesccMiddleGray + 5.0f * tone::kAcesccCodePerEv;

  EXPECT_GT(tone::ApplyReferenceCurve(shadow_l, 1.0f, 0.0f), shadow_l);
  EXPECT_LT(tone::ApplyReferenceCurve(shadow_l, -1.0f, 0.0f), shadow_l);
  EXPECT_LT(tone::ApplyReferenceCurve(highlight_l, 0.0f, 1.0f), highlight_l);
  EXPECT_GT(tone::ApplyReferenceCurve(highlight_l, 0.0f, -1.0f), highlight_l);

  const float combined = tone::ApplyReferenceCurve(tone::kAcesccMiddleGray, 1.0f, 1.0f);
  EXPECT_TRUE(std::isfinite(combined));
}

TEST(LocalToneMappingConstantsMatchRuntime, DetailAlphaAndToneBetaStayBounded) {
  for (float l = tone::kGammaMinL; l <= tone::kGammaMaxL; l += 0.11f) {
    EXPECT_FLOAT_EQ(tone::DetailAlpha(l, 1.0f, 0.0f), 1.0f);
    EXPECT_FLOAT_EQ(tone::DetailAlpha(l, tone::kShadowLiftAmountLimit, 1.0f), 1.0f);

    const float beta = tone::ToneBeta(l, 1.0f, 1.0f);
    EXPECT_GE(beta, tone::kToneBetaMin);
    EXPECT_LE(beta, tone::kToneBetaMax);
  }
}

// #939: the Shadows lift is the integral of a bounded slope reduction, and the black anchor brings
// it back to 0 at the darkest level. Lift in EV for Shadows +25, +50, +100, +150 at the relative EV
// in the header row (the old curve gave 1.51 / 3.02 / 4.54 EV at -4.3 EV for +50 / +100 / +150).
TEST(LocalToneMappingConstantsMatchRuntime, ShadowLiftKeepsBlackAndLiftsTheShadowBand) {
  const float ev[] = {-9.5f, -9.0f, -8.0f, -7.0f, -5.0f, -4.3f, -3.0f, -2.0f, -1.0f, 0.0f};
  struct Row {
    float slider;
    float lift[10];
    float min_slope;
  };
  const Row rows[] = {
      {25.0f,
       {0.0f, 0.006f, 0.124f, 0.409f, 0.800f, 0.751f, 0.550f, 0.393f, 0.236f, 0.093f},
       0.84f},
      {50.0f,
       {0.0f, 0.012f, 0.247f, 0.817f, 1.599f, 1.502f, 1.099f, 0.785f, 0.471f, 0.186f},
       0.69f},
      {100.0f,
       {0.0f, 0.023f, 0.482f, 1.594f, 3.121f, 2.930f, 2.144f, 1.531f, 0.919f, 0.363f},
       0.39f},
      {150.0f,
       {0.0f, 0.030f, 0.622f, 2.058f, 4.030f, 3.784f, 2.769f, 1.978f, 1.187f, 0.469f},
       0.21f},
  };
  for (const auto& row : rows) {
    const float amount = row.slider * tone::kHighlightStrengthScale / 80.0f;
    for (int i = 0; i < 10; ++i) {
      EXPECT_NEAR(tone::ShadowLiftEv(ev[i], amount), row.lift[i], 0.002f)
          << row.slider << " at " << ev[i] << " EV";
    }
    // Slope of output EV against input EV, measured on a 0.01 EV grid.
    float min_slope = 2.0f;
    for (float x = -9.87f; x < 13.4f; x += 0.01f) {
      const float slope =
          1.0f + (tone::ShadowLiftEv(x + 0.01f, amount) - tone::ShadowLiftEv(x, amount)) / 0.01f;
      min_slope = std::min(min_slope, slope);
      EXPECT_LE(slope, 2.7f) << row.slider << " " << x;
    }
    EXPECT_NEAR(min_slope, row.min_slope, 0.01f) << row.slider;
    EXPECT_GT(min_slope, 1.0f - tone::kLiftSlopeReductionMax) << row.slider;
    // Below the black anchor end the lift is 0: black keeps its level.
    EXPECT_NEAR(tone::ShadowLiftEv(tone::kLiftBlackAnchorEndEv - 0.3f, amount), 0.0f, 1.0e-5f)
        << row.slider;
  }

  EXPECT_FLOAT_EQ(tone::ShadowLiftEv(-4.3f, 0.0f), 0.0f);
  EXPECT_FLOAT_EQ(tone::ShadowLiftEv(-4.3f, -1.0f), 0.0f);
  EXPECT_FLOAT_EQ(tone::ShadowLiftEv(8.0f, tone::kShadowLiftAmountLimit), 0.0f);
}

// #939: the Highlights curve applies to the output of the Shadows curve. With Shadows at 0 the
// Highlights curve applies to the input level.
TEST(LocalToneMappingConstantsMatchRuntime, HighlightsApplyToTheShadowsCurveOutput) {
  for (float rel = -6.0f; rel <= 8.0f; rel += 0.5f) {
    const float l        = tone::kAcesccMiddleGray + rel * tone::kAcesccCodePerEv;
    const float recovery = l - 1.0f * tone::kHighlightStrengthScale *
                                   tone::HighlightProfileEv(rel) * tone::kAcesccCodePerEv;
    EXPECT_NEAR(tone::ApplyReferenceCurve(l, 0.0f, 1.0f), recovery, 1.0e-6f) << rel;

    const float lifted = rel + tone::ShadowLiftEv(rel, 1.0f);
    const float both   = tone::kAcesccMiddleGray +
                       (lifted - tone::kHighlightStrengthScale * tone::HighlightProfileEv(lifted)) *
                           tone::kAcesccCodePerEv;
    EXPECT_NEAR(tone::ApplyReferenceCurve(l, 1.0f, 1.0f), both, 1.0e-5f) << rel;
  }
}

TEST(LocalToneMappingConstantsMatchRuntime, CacheKeysTrackAmountsFlagsAndRoi) {
  TestToneParams params;
  params.hs_mask_base_cache_key_ = 0x12345678ull;
  params.shadows_enabled_        = true;
  params.highlights_enabled_     = true;

  const std::uint64_t base       = tone::BuildAdjustedResultCacheKey(params, 0.5f, 0.25f);
  EXPECT_EQ(base, tone::BuildAdjustedResultCacheKey(params, 0.5f, 0.25f));
  EXPECT_NE(base, tone::BuildAdjustedResultCacheKey(params, 0.6f, 0.25f));
  EXPECT_NE(base, tone::BuildAdjustedResultCacheKey(params, 0.5f, 0.30f));

  params.highlights_enabled_ = false;
  EXPECT_NE(base, tone::BuildAdjustedResultCacheKey(params, 0.5f, 0.25f));
  params.highlights_enabled_          = true;

  params.render_roi_enabled_          = true;
  params.render_roi_x_                = 4;
  params.render_roi_y_                = 7;
  params.render_roi_scale_x_          = 0.5f;
  params.render_roi_scale_y_          = 0.75f;
  params.render_roi_reference_width_  = 1280;
  params.render_roi_reference_height_ = 720;
  const std::uint64_t roi_key         = tone::BuildRoiAdjustedResultCacheKey(params, base);
  EXPECT_NE(base, roi_key);
  params.render_roi_x_ = 5;
  EXPECT_NE(roi_key, tone::BuildRoiAdjustedResultCacheKey(params, base));
}

TEST(LocalToneMappingConstantsMatchRuntime, RoiReferenceReuseDoesNotRequireSamePresentationSize) {
  EXPECT_TRUE(tone::CanReuseReferenceForRoi(
      /*roi_frame_with_source_reference=*/true,
      /*reference_source_cache_valid=*/true,
      /*roi_reference_width=*/4096,
      /*roi_reference_height=*/2731));
  EXPECT_TRUE(tone::CanReuseReferenceForRoi(
      /*roi_frame_with_source_reference=*/true,
      /*reference_source_cache_valid=*/true,
      /*roi_reference_width=*/2560,
      /*roi_reference_height=*/1707));

  EXPECT_FALSE(tone::CanReuseReferenceForRoi(
      /*roi_frame_with_source_reference=*/true,
      /*reference_source_cache_valid=*/false,
      /*roi_reference_width=*/4096,
      /*roi_reference_height=*/2731));
  EXPECT_FALSE(tone::CanReuseReferenceForRoi(
      /*roi_frame_with_source_reference=*/false,
      /*reference_source_cache_valid=*/true,
      /*roi_reference_width=*/4096,
      /*roi_reference_height=*/2731));
  EXPECT_FALSE(tone::CanReuseReferenceForRoi(
      /*roi_frame_with_source_reference=*/true,
      /*reference_source_cache_valid=*/true,
      /*roi_reference_width=*/0,
      /*roi_reference_height=*/2731));
}

TEST(LocalToneMappingConstantsMatchRuntime, AcesccDeltaFastPathMatchesLinearRatioInNormalRange) {
  constexpr float kDenormThreshold = (-15.0f + 9.72f) / 17.52f;
  const float     rgb[][3]         = {
      {0.34f, 0.38f, 0.42f},
      {0.48f, 0.50f, 0.53f},
      {0.58f, 0.62f, 0.66f},
  };
  const float deltas[] = {-0.12f, -0.03f, 0.04f, 0.18f};

  for (const auto& px : rgb) {
    const float source_ap1[3] = {AcesccDecode(px[0]), AcesccDecode(px[1]), AcesccDecode(px[2])};
    const float source_luma =
        0.27222872f * source_ap1[0] + 0.67408177f * source_ap1[1] + 0.05368952f * source_ap1[2];
    ASSERT_GT(AcesccEncode(source_luma), kDenormThreshold);

    for (const float delta : deltas) {
      for (int channel = 0; channel < 3; ++channel) {
        ASSERT_GT(px[channel], kDenormThreshold);
        ASSERT_GT(px[channel] + delta, kDenormThreshold);
      }

      const float ratio = std::exp2(delta * 17.52f);
      for (int channel = 0; channel < 3; ++channel) {
        const float via_ratio = AcesccEncode(source_ap1[channel] * ratio);
        const float via_delta = px[channel] + delta;
        EXPECT_NEAR(via_ratio, via_delta, 2.0e-6f);
      }
    }
  }
}

}  // namespace alcedo
