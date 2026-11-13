//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include "edit/operators/CPU_kernels/basic/highlight_kernel.hpp"
#include "edit/operators/basic/highlight_op.hpp"

namespace alcedo {
namespace {
constexpr float kTestHighlightAcesccLog2Min      = -15.0f;
constexpr float kTestHighlightAcesccDenormTrans  = 0.00003051757812f;
constexpr float kTestHighlightAcesccDenormOffset = 0.00001525878906f;
constexpr float kTestHighlightAcesccA            = 9.72f;
constexpr float kTestHighlightAcesccB            = 17.52f;
constexpr float kTestHighlightAp1LumaR           = 0.282567f;
constexpr float kTestHighlightAp1LumaG           = 0.664611f;
constexpr float kTestHighlightAp1LumaB           = 0.052823f;

float EncodeAcesccForTest(float x) {
  if (x <= 0.0f) {
    return x;
  }
  if (x < kTestHighlightAcesccDenormTrans) {
    return (std::log2(kTestHighlightAcesccDenormOffset + x * 0.5f) + kTestHighlightAcesccA) /
           kTestHighlightAcesccB;
  }
  return (std::log2(x) + kTestHighlightAcesccA) / kTestHighlightAcesccB;
}

float DecodeAcesccForTest(float x) {
  const float denorm_threshold =
      (kTestHighlightAcesccLog2Min + kTestHighlightAcesccA) / kTestHighlightAcesccB;
  if (x <= 0.0f) {
    return x;
  }
  if (x < denorm_threshold) {
    return (std::exp2(x * kTestHighlightAcesccB - kTestHighlightAcesccA) -
            kTestHighlightAcesccDenormOffset) *
           2.0f;
  }
  return std::exp2(x * kTestHighlightAcesccB - kTestHighlightAcesccA);
}

float HighlightLumaForTest(const Pixel& p) {
  return kTestHighlightAp1LumaR * DecodeAcesccForTest(p.r_) +
         kTestHighlightAp1LumaG * DecodeAcesccForTest(p.g_) +
         kTestHighlightAp1LumaB * DecodeAcesccForTest(p.b_);
}

float Max3ForTest(float a, float b, float c) { return std::max(a, std::max(b, c)); }

float Min3ForTest(float a, float b, float c) { return std::min(a, std::min(b, c)); }

float PixelChromaForTest(const Pixel& p) {
  const float r = DecodeAcesccForTest(p.r_);
  const float g = DecodeAcesccForTest(p.g_);
  const float b = DecodeAcesccForTest(p.b_);
  return Max3ForTest(r, g, b) - Min3ForTest(r, g, b);
}

float EvaluateHighlightLumaWithoutComp(float L, const OperatorParams& params) {
  float outL = L;
  if (L <= params.highlights_k_) {
    outL = L;
  } else if (L < 1.0f) {
    const float segment_m1 = (params.highlights_offset_ < 0.0f) ? 1.0f : params.highlights_m1_;
    const float t   = (L - params.highlights_k_) / params.highlights_dx_;
    const float H00 = 2.0f * t * t * t - 3.0f * t * t + 1.0f;
    const float H10 = t * t * t - 2.0f * t * t + t;
    const float H01 = -2.0f * t * t * t + 3.0f * t * t;
    const float H11 = t * t * t - t * t;
    outL            = H00 * params.highlights_k_ +
           H10 * (params.highlights_dx_ * params.highlights_m0_) + H01 * 1.0f +
           H11 * (params.highlights_dx_ * segment_m1);
  } else {
    const float x  = L - 1.0f;
    const float m1 = params.highlights_m1_;
    if (m1 < 1.0f) {
      const float b = std::max(1e-6f, 1.0f - m1);
      outL          = 1.0f + (m1 * x) / (1.0f + b * x);
    } else {
      outL = 1.0f + x * m1;
    }
  }
  return std::isfinite(outL) ? outL : L;
}
}  // namespace

TEST(HighlightsOpTests, SetGlobalParamsPropagatesNarrowerKnee) {
  HighlightsOp   op(-50.0f);
  OperatorParams params;
  op.SetGlobalParams(params);

  EXPECT_NEAR(params.highlights_k_, 0.90f, 1e-6f);
  EXPECT_NEAR(params.highlights_dx_, 0.10f, 1e-6f);
  EXPECT_NEAR(params.highlights_x0_, params.highlights_k_, 1e-6f);
  EXPECT_NEAR(params.highlights_y0_, params.highlights_k_, 1e-6f);
  EXPECT_NEAR(params.highlights_y1_, 1.0f, 1e-6f);
}

TEST(HighlightsOpTests, KernelUsesLinearAp1RangeAndChromaCompensation) {
  HighlightsOp      op(-50.0f);
  OperatorParams    params;
  HighlightsOpKernel kernel;
  op.SetGlobalParams(params);

  Pixel below_knee = {
      EncodeAcesccForTest(0.30f), EncodeAcesccForTest(0.30f), EncodeAcesccForTest(0.30f), 1.0f};
  const float below_input_luma = HighlightLumaForTest(below_knee);
  kernel(below_knee, params);
  EXPECT_NEAR(HighlightLumaForTest(below_knee), below_input_luma, 1e-5f);

  Pixel bright_neutral = {
      EncodeAcesccForTest(0.96f), EncodeAcesccForTest(0.96f), EncodeAcesccForTest(0.96f), 1.0f};
  const float neutral_input_luma = HighlightLumaForTest(bright_neutral);
  kernel(bright_neutral, params);
  EXPECT_LT(HighlightLumaForTest(bright_neutral), neutral_input_luma);

  Pixel bright_color = {
      EncodeAcesccForTest(1.05f), EncodeAcesccForTest(0.82f), EncodeAcesccForTest(0.62f), 1.0f};
  const float input_luma         = HighlightLumaForTest(bright_color);
  const float input_chroma       = PixelChromaForTest(bright_color);
  const float baseline_out_luma  = EvaluateHighlightLumaWithoutComp(input_luma, params);
  const float baseline_scale     = (input_luma > 1e-8f) ? (baseline_out_luma / input_luma) : 1.0f;
  const float base_r             = DecodeAcesccForTest(bright_color.r_) * baseline_scale;
  const float base_g             = DecodeAcesccForTest(bright_color.g_) * baseline_scale;
  const float base_b             = DecodeAcesccForTest(bright_color.b_) * baseline_scale;
  const float baseline_chroma    = Max3ForTest(base_r, base_g, base_b) - Min3ForTest(base_r, base_g, base_b);

  kernel(bright_color, params);
  const float output_chroma = PixelChromaForTest(bright_color);
  EXPECT_GE(output_chroma, baseline_chroma);
  EXPECT_LE(output_chroma, input_chroma + 1e-5f);
}

}  // namespace alcedo
