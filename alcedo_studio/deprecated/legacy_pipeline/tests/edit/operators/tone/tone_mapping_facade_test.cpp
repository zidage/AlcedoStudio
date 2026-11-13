//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include "edit/operators/GPU_kernels/fused_param.hpp"
#include "edit/operators/basic/highlight_op.hpp"
#include "edit/operators/basic/shadow_op.hpp"
#include "edit/operators/detail/clarity_op.hpp"

namespace alcedo {
namespace {

TEST(ToneMappingFacadeTest, LegacyToneSlidersPopulateGlobalToneMappingPayload) {
  OperatorParams params;

  ShadowsOp      shadows(40.0f);
  HighlightsOp   highlights(-20.0f);
  ClarityOp      clarity(30.0f);

  shadows.SetGlobalParams(params);
  highlights.SetGlobalParams(params);
  clarity.SetGlobalParams(params);

  const auto& tone = params.tone_mapping_;
  EXPECT_TRUE(tone.slider_input_.shadows_operator_present_);
  EXPECT_TRUE(tone.slider_input_.highlights_operator_present_);
  EXPECT_TRUE(tone.slider_input_.clarity_operator_present_);
  EXPECT_FLOAT_EQ(tone.slider_input_.shadows_slider_value_, 40.0f);
  EXPECT_FLOAT_EQ(tone.slider_input_.highlights_slider_value_, -20.0f);
  EXPECT_FLOAT_EQ(tone.slider_input_.clarity_slider_value_, 30.0f);

  EXPECT_FLOAT_EQ(tone.shadow_amount_, params.shadows_offset_);
  EXPECT_FLOAT_EQ(tone.highlight_amount_, params.highlights_offset_);
  EXPECT_FLOAT_EQ(tone.clarity_amount_, params.clarity_offset_);
  EXPECT_TRUE(tone.local_tone_enabled_);
  EXPECT_FLOAT_EQ(tone.local_radius_, params.hs_base_radius_);
  EXPECT_EQ(tone.base_gaussian_tap_count_, params.hs_base_gaussian_tap_count_);
  EXPECT_FLOAT_EQ(tone.shadow_log_pivot_, params.hs_shadow_log_pivot_);
  EXPECT_FLOAT_EQ(tone.highlight_log_pivot_, params.hs_highlight_log_pivot_);
}

TEST(ToneMappingFacadeTest, EnableStateMirrorsIntoToneMappingPayload) {
  OperatorParams params;

  ShadowsOp      shadows(10.0f);
  HighlightsOp   highlights(10.0f);
  ClarityOp      clarity(10.0f);

  shadows.EnableGlobalParams(params, false);
  highlights.EnableGlobalParams(params, false);
  clarity.EnableGlobalParams(params, false);

  shadows.SetGlobalParams(params);
  highlights.SetGlobalParams(params);
  clarity.SetGlobalParams(params);

  EXPECT_FALSE(params.tone_mapping_.shadows_enabled_);
  EXPECT_FALSE(params.tone_mapping_.highlights_enabled_);
  EXPECT_FALSE(params.tone_mapping_.clarity_enabled_);
}

TEST(ToneMappingFacadeTest, FusedParamsUseToneMappingPayloadAsInternalSource) {
  OperatorParams params;

  params.render_source_cache_key_          = 0x1234ull;
  params.render_hs_preserve_source_detail_ = true;
  params.render_roi_enabled_               = true;
  params.render_roi_x_                     = 9;
  params.render_roi_y_                     = 11;
  params.render_roi_scale_x_               = 0.25f;
  params.render_roi_scale_y_               = 0.5f;
  params.render_roi_reference_width_       = 4000;
  params.render_roi_reference_height_      = 3000;

  ShadowsOp    shadows(50.0f);
  HighlightsOp highlights(-25.0f);
  ClarityOp    clarity(20.0f);
  shadows.SetGlobalParams(params);
  highlights.SetGlobalParams(params);
  clarity.SetGlobalParams(params);

  const auto fused = FusedParamsConverter::ConvertFromCPU(params);

  EXPECT_FLOAT_EQ(fused.tone_mapping_.shadow_amount_, fused.shadows_offset_);
  EXPECT_FLOAT_EQ(fused.tone_mapping_.highlight_amount_, fused.highlights_offset_);
  EXPECT_FLOAT_EQ(fused.tone_mapping_.clarity_amount_, fused.clarity_offset_);
  EXPECT_EQ(fused.tone_mapping_.base_cache_key_, fused.hs_mask_base_cache_key_);
  EXPECT_TRUE(fused.tone_mapping_.preserve_source_detail_);
  EXPECT_TRUE(fused.tone_mapping_.roi_enabled_);
  EXPECT_EQ(fused.tone_mapping_.roi_x_, 9);
  EXPECT_EQ(fused.tone_mapping_.roi_reference_width_, 4000);
}

}  // namespace
}  // namespace alcedo
