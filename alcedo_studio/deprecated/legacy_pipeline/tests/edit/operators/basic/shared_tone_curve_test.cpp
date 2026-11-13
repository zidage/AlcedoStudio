//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>

#include "edit/operators/GPU_kernels/fused_param.hpp"
#include "edit/operators/GPU_kernels/param.cuh"
#include "edit/operators/basic/highlight_op.hpp"
#include "edit/operators/basic/shadows_highlights_shared_curve.hpp"
#include "edit/operators/basic/shadow_op.hpp"

namespace alcedo {
namespace {

struct TestRgb {
  float x;
  float y;
  float z;
};

auto SharedCurveFromParams(const OperatorParams& params) -> detail::SharedToneCurveDescriptor {
  detail::SharedToneCurveDescriptor curve;
  curve.enabled     = params.shared_tone_curve_enabled_;
  curve.point_count = params.shared_tone_curve_ctrl_pts_size_;
  for (int i = 0; i < detail::kSharedToneCurveStorageCount; ++i) {
    curve.x[i] = params.shared_tone_curve_ctrl_pts_x_[i];
    curve.y[i] = params.shared_tone_curve_ctrl_pts_y_[i];
    curve.m[i] = params.shared_tone_curve_m_[i];
    if (i < detail::kSharedToneCurveStorageCount - 1) {
      curve.h[i] = params.shared_tone_curve_h_[i];
    }
  }
  return curve;
}

auto Luma(const TestRgb& rgb) -> float {
  return 0.2126f * rgb.x + 0.7152f * rgb.y + 0.0722f * rgb.z;
}

auto Chroma(const TestRgb& rgb) -> float {
  return std::max(rgb.x, std::max(rgb.y, rgb.z)) - std::min(rgb.x, std::min(rgb.y, rgb.z));
}

auto RatioScale(const TestRgb& rgb, float source_luma, float mapped_luma) -> TestRgb {
  const float scale = (source_luma > 1e-8f) ? (mapped_luma / source_luma) : 1.0f;
  return {rgb.x * scale, rgb.y * scale, rgb.z * scale};
}

}  // namespace

TEST(SharedToneCurveTest, SharedCurveUsesFixedAnchorsWithoutMidpointPin) {
  ShadowsOp    shadows(0.0f);
  HighlightsOp highlights(0.0f);
  OperatorParams params;

  shadows.SetGlobalParams(params);
  highlights.SetGlobalParams(params);

  ASSERT_EQ(params.shared_tone_curve_ctrl_pts_size_, detail::kSharedToneCurvePointCount);
  for (int i = 0; i < detail::kSharedToneCurvePointCount; ++i) {
    EXPECT_NEAR(params.shared_tone_curve_ctrl_pts_x_[i], detail::kSharedToneCurveX[i], 1e-6f);
    EXPECT_NEAR(params.shared_tone_curve_ctrl_pts_y_[i], detail::kSharedToneCurveX[i], 1e-6f);
  }
  EXPECT_NEAR(params.shared_tone_curve_ctrl_pts_x_[0], 0.0f, 1e-6f);
  EXPECT_NEAR(params.shared_tone_curve_ctrl_pts_y_[0], 0.0f, 1e-6f);
  EXPECT_NEAR(params.shared_tone_curve_ctrl_pts_x_[1], 0.25f, 1e-6f);
  EXPECT_NEAR(params.shared_tone_curve_ctrl_pts_x_[2], 0.75f, 1e-6f);
  EXPECT_NEAR(params.shared_tone_curve_ctrl_pts_x_[3], 1.0f, 1e-6f);
  EXPECT_FLOAT_EQ(params.shared_tone_curve_ctrl_pts_x_[4], 0.0f);
  EXPECT_FLOAT_EQ(params.shared_tone_curve_ctrl_pts_y_[4], 0.0f);
  EXPECT_TRUE(params.shared_tone_curve_apply_in_shadows_);
  EXPECT_FALSE(params.shared_tone_curve_apply_in_highlights_);
}

TEST(SharedToneCurveTest, ShadowAndHighlightControlsReachUpperMidtones) {
  ShadowsOp shadow_only(80.0f);
  OperatorParams shadow_params;
  shadow_only.SetGlobalParams(shadow_params);

  EXPECT_NEAR(shadow_params.shared_tone_curve_ctrl_pts_y_[0], 0.0f, 1e-6f);
  EXPECT_GT(shadow_params.shared_tone_curve_ctrl_pts_y_[1], 0.25f);
  EXPECT_NEAR(shadow_params.shared_tone_curve_ctrl_pts_y_[2], 0.75f, 1e-6f);
  EXPECT_NEAR(shadow_params.shared_tone_curve_ctrl_pts_y_[3], 1.0f, 1e-6f);
  const auto shadow_curve = SharedCurveFromParams(shadow_params);
  EXPECT_GT(detail::EvaluateSharedToneCurve(0.12f, shadow_curve) - 0.12f, 0.015f);

  ShadowsOp shadow_negative(-80.0f);
  OperatorParams shadow_negative_params;
  shadow_negative.SetGlobalParams(shadow_negative_params);
  EXPECT_LT(shadow_negative_params.shared_tone_curve_ctrl_pts_y_[1], 0.25f);
  const auto shadow_negative_curve = SharedCurveFromParams(shadow_negative_params);
  EXPECT_LT(detail::EvaluateSharedToneCurve(0.12f, shadow_negative_curve), 0.12f);

  HighlightsOp highlight_only(80.0f);
  OperatorParams highlight_params;
  highlight_only.SetGlobalParams(highlight_params);

  EXPECT_NEAR(highlight_params.shared_tone_curve_ctrl_pts_y_[0], 0.0f, 1e-6f);
  EXPECT_NEAR(highlight_params.shared_tone_curve_ctrl_pts_y_[1], 0.25f, 1e-6f);
  EXPECT_GT(highlight_params.shared_tone_curve_ctrl_pts_y_[2], 0.75f);
  EXPECT_LT(highlight_params.shared_tone_curve_ctrl_pts_y_[3], 1.0f);
  EXPECT_LT(highlight_params.shared_tone_curve_m_[3], 1.0f);
  const auto highlight_curve = SharedCurveFromParams(highlight_params);
  EXPECT_GT(detail::EvaluateSharedToneCurve(0.74f, highlight_curve), 0.74f);
  EXPECT_GT(0.88f - detail::EvaluateSharedToneCurve(0.88f, highlight_curve), 0.015f);
  EXPECT_LT(detail::EvaluateSharedToneCurve(1.2f, highlight_curve), 1.2f);
  EXPECT_NEAR(detail::EvaluateSharedToneCurve(1.2f, highlight_curve),
              highlight_params.shared_tone_curve_ctrl_pts_y_[3] +
                  0.2f * highlight_params.shared_tone_curve_m_[3],
              1e-6f);
  EXPECT_FALSE(highlight_params.shared_tone_curve_apply_in_shadows_);
  EXPECT_TRUE(highlight_params.shared_tone_curve_apply_in_highlights_);
}

TEST(SharedToneCurveTest, ShadowSliderContinuesPastEighty) {
  ShadowsOp shadow_eighty(80.0f);
  ShadowsOp shadow_hundred(100.0f);
  OperatorParams eighty_params;
  OperatorParams hundred_params;

  shadow_eighty.SetGlobalParams(eighty_params);
  shadow_hundred.SetGlobalParams(hundred_params);

  EXPECT_GT(hundred_params.shadows_offset_, eighty_params.shadows_offset_);
  EXPECT_GT(hundred_params.shadows_m0_, eighty_params.shadows_m0_);
}

TEST(SharedToneCurveTest, SharedCurvePreservesShadowChromaBaseline) {
  ShadowsOp    shadows(70.0f);
  HighlightsOp highlights(65.0f);
  OperatorParams params;

  shadows.SetGlobalParams(params);
  highlights.SetGlobalParams(params);
  const auto curve = SharedCurveFromParams(params);

  const TestRgb shadow_rgb = {0.08f, 0.02f, 0.26f};
  const float   shadow_l   = Luma(shadow_rgb);
  const float   shadow_out_l = detail::EvaluateSharedToneCurve(shadow_l, curve);
  const auto    shadow_preserved =
      detail::ReconstructFromSharedToneLuma<TestRgb>(shadow_out_l, shadow_l, shadow_rgb);
  EXPECT_GT(shadow_out_l, shadow_l);
  EXPECT_GE(Chroma(shadow_preserved) + 1e-6f, Chroma(shadow_rgb));

}

TEST(SharedToneCurveTest, GpuUploadCopiesSharedCurvePayload) {
  ShadowsOp    shadows(55.0f);
  HighlightsOp highlights(45.0f);
  OperatorParams cpu_params;

  shadows.SetGlobalParams(cpu_params);
  highlights.SetGlobalParams(cpu_params);

  const auto fused = FusedParamsConverter::ConvertFromCPU(cpu_params);
  GPUOperatorParams gpu_params{};
  gpu_params = GPUParamsConverter::ConvertFromCPU(cpu_params, gpu_params);

  EXPECT_EQ(fused.shared_tone_curve_ctrl_pts_size_, cpu_params.shared_tone_curve_ctrl_pts_size_);
  EXPECT_EQ(gpu_params.shared_tone_curve_ctrl_pts_size_,
            cpu_params.shared_tone_curve_ctrl_pts_size_);
  EXPECT_EQ(gpu_params.shared_tone_curve_apply_in_shadows_,
            cpu_params.shared_tone_curve_apply_in_shadows_);
  EXPECT_EQ(gpu_params.shared_tone_curve_apply_in_highlights_,
            cpu_params.shared_tone_curve_apply_in_highlights_);

  for (int i = 0; i < OperatorParams::kSharedToneCurveControlPointCount; ++i) {
    EXPECT_FLOAT_EQ(fused.shared_tone_curve_ctrl_pts_x_[i], cpu_params.shared_tone_curve_ctrl_pts_x_[i]);
    EXPECT_FLOAT_EQ(fused.shared_tone_curve_ctrl_pts_y_[i], cpu_params.shared_tone_curve_ctrl_pts_y_[i]);
    EXPECT_FLOAT_EQ(fused.shared_tone_curve_m_[i], cpu_params.shared_tone_curve_m_[i]);
    EXPECT_FLOAT_EQ(gpu_params.shared_tone_curve_ctrl_pts_x_[i],
                    cpu_params.shared_tone_curve_ctrl_pts_x_[i]);
    EXPECT_FLOAT_EQ(gpu_params.shared_tone_curve_ctrl_pts_y_[i],
                    cpu_params.shared_tone_curve_ctrl_pts_y_[i]);
    EXPECT_FLOAT_EQ(gpu_params.shared_tone_curve_m_[i], cpu_params.shared_tone_curve_m_[i]);
    if (i < OperatorParams::kSharedToneCurveControlPointCount - 1) {
      EXPECT_FLOAT_EQ(fused.shared_tone_curve_h_[i], cpu_params.shared_tone_curve_h_[i]);
      EXPECT_FLOAT_EQ(gpu_params.shared_tone_curve_h_[i], cpu_params.shared_tone_curve_h_[i]);
    }
  }
}

}  // namespace alcedo
