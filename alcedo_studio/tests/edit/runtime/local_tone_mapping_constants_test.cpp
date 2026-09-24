//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
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

auto ReadSourceFile(const std::filesystem::path& path) -> std::string {
  std::ifstream file(path, std::ios::binary);
  EXPECT_TRUE(file.is_open()) << path.string();
  std::ostringstream contents;
  contents << file.rdbuf();
  return contents.str();
}

auto SourcePath(const std::filesystem::path& relative) -> std::filesystem::path {
  return std::filesystem::path(ALCEDO_SOURCE_ROOT) / relative;
}

auto ExtractFloatLiteral(const std::string& source, const std::string& symbol) -> float {
  const std::regex pattern(symbol + R"(\s*(?:=|\s)\s*(-?\d+(?:\.\d+)?)(?:f)?)");
  std::smatch      match;
  EXPECT_TRUE(std::regex_search(source, match, pattern)) << symbol;
  if (match.size() < 2) {
    return 0.0f;
  }
  return std::stof(match[1].str());
}

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
  const float deep_shadow = tone::kAcesccMiddleGray - 6.0f * tone::kAcesccCodePerEv;
  const float mid_shadow  = tone::kAcesccMiddleGray - 2.0f * tone::kAcesccCodePerEv;

  EXPECT_GT(tone::DetailAlpha(deep_shadow, 1.0f, 0.0f), 1.0f);
  EXPECT_LT(tone::DetailAlpha(mid_shadow, 1.0f, 0.0f), 1.0f);

  for (float l = tone::kGammaMinL; l <= tone::kGammaMaxL; l += 0.11f) {
    const float beta = tone::ToneBeta(l, 1.0f, 1.0f);
    EXPECT_GE(beta, tone::kToneBetaMin);
    EXPECT_LE(beta, tone::kToneBetaMax);
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

TEST(LocalToneMappingConstantsMatchRuntime, LegacyShaderMirrorConstantsMatchRuntime) {
  const auto opencl = ReadSourceFile(SourcePath("edit/pipeline/opencl_shader/tone_mapping.cl"));
  const auto metal =
      ReadSourceFile(SourcePath("edit/operators/GPU_kernels/metal_shader/tone_mapping.metal"));

  EXPECT_NE(opencl.find("Mirrored from edit/runtime/local_tone_mapping.hpp"), std::string::npos);
  EXPECT_NE(metal.find("Mirrored from edit/runtime/local_tone_mapping.hpp"), std::string::npos);

  EXPECT_FLOAT_EQ(ExtractFloatLiteral(opencl, "ALCEDO_OPENCL_HS_ACESCC_MIDDLE_GRAY"),
                  tone::kAcesccMiddleGray);
  EXPECT_FLOAT_EQ(ExtractFloatLiteral(opencl, "ALCEDO_OPENCL_HS_BASE_SIGMA_R"), tone::kBaseSigmaR);
  EXPECT_FLOAT_EQ(ExtractFloatLiteral(opencl, "ALCEDO_OPENCL_HS_HIGHLIGHT_STRENGTH_SCALE"),
                  tone::kHighlightStrengthScale);
  EXPECT_FLOAT_EQ(ExtractFloatLiteral(opencl, "ALCEDO_OPENCL_HS_BACKEND_AMOUNT_LIMIT"),
                  tone::kBackendAmountLimit);
  EXPECT_FLOAT_EQ(ExtractFloatLiteral(opencl, "ALCEDO_OPENCL_HS_TONE_BETA_EPS"),
                  tone::kToneBetaEps);
  EXPECT_FLOAT_EQ(ExtractFloatLiteral(opencl, "ALCEDO_OPENCL_HS_TONE_BETA_MIN"),
                  tone::kToneBetaMin);
  EXPECT_FLOAT_EQ(ExtractFloatLiteral(opencl, "ALCEDO_OPENCL_HS_TONE_BETA_MAX"),
                  tone::kToneBetaMax);

  EXPECT_FLOAT_EQ(ExtractFloatLiteral(metal, "kMetalHsAcesccMiddleGray"), tone::kAcesccMiddleGray);
  EXPECT_FLOAT_EQ(ExtractFloatLiteral(metal, "kMetalHsBaseSigmaR"), tone::kBaseSigmaR);
  EXPECT_FLOAT_EQ(ExtractFloatLiteral(metal, "kMetalHsHighlightStrengthScale"),
                  tone::kHighlightStrengthScale);
  EXPECT_FLOAT_EQ(ExtractFloatLiteral(metal, "kMetalHsBackendAmountLimit"),
                  tone::kBackendAmountLimit);
  EXPECT_FLOAT_EQ(ExtractFloatLiteral(metal, "kMetalHsToneBetaEps"), tone::kToneBetaEps);
  EXPECT_FLOAT_EQ(ExtractFloatLiteral(metal, "kMetalHsToneBetaMin"), tone::kToneBetaMin);
  EXPECT_FLOAT_EQ(ExtractFloatLiteral(metal, "kMetalHsToneBetaMax"), tone::kToneBetaMax);
}

}  // namespace alcedo
