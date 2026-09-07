//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/grade_schedule.hpp"

#include <span>

#include <gtest/gtest.h>

#include "edit/runtime/adjustment_runtime.hpp"

namespace alcedo {
namespace {

auto EnabledNeighbor(AdjustmentBehavior behavior) -> GradeNeighborParams {
  GradeNeighborParams params;
  params.behavior = static_cast<std::uint32_t>(behavior);
  params.enabled  = 1;
  params.radius   = 4;
  return params;
}

TEST(GradeSchedule, ZeroMixAliasesWithoutRecordingOps) {
  GradeScheduleInput exposure;
  exposure.algorithm    = CompiledAdjustmentAlgorithm::Pointwise;
  exposure.behavior     = AdjustmentBehavior::Exposure;
  exposure.fused_offset = 16;
  const auto schedule   = MakeGradeSchedule(std::span{&exposure, 1}, 0.0f, false);
  const auto trace      = MakeGradeDecisionTrace(schedule);
  EXPECT_TRUE(trace.alias_to_input);
  EXPECT_TRUE(schedule.ops.empty());
  EXPECT_EQ(trace.gpu_write_count, 0U);
  EXPECT_TRUE(GradeWriteSlots(0).empty());
}

TEST(GradeSchedule, AdjacentPointwiseOpsFuseAndSkipMixAtUnity) {
  GradeScheduleInput exposure;
  exposure.algorithm    = CompiledAdjustmentAlgorithm::Pointwise;
  exposure.behavior     = AdjustmentBehavior::Exposure;
  exposure.fused_offset = 0;
  GradeScheduleInput contrast;
  contrast.algorithm    = CompiledAdjustmentAlgorithm::Pointwise;
  contrast.behavior     = AdjustmentBehavior::Contrast;
  contrast.fused_offset = 48;
  const GradeScheduleInput inputs[] = {exposure, contrast};
  const auto schedule = MakeGradeSchedule(inputs, 1.0f, false);
  const auto trace    = MakeGradeDecisionTrace(schedule);
  ASSERT_EQ(trace.op_kinds.size(), 1U);
  EXPECT_EQ(trace.op_kinds.front(), GradeOpKind::Fused);
  EXPECT_EQ(trace.fused_command_count, 2U);
  EXPECT_TRUE(trace.skip_final_mix);
  EXPECT_EQ(trace.gpu_write_count, 1U);
  ASSERT_EQ(GradeWriteSlots(1).size(), 1U);
  EXPECT_EQ(GradeWriteSlots(1).front(), GradeImageSlot::Output);
}

TEST(GradeSchedule, NeighborhoodIsABarrierBetweenFusedGroups) {
  GradeScheduleInput exposure;
  exposure.algorithm    = CompiledAdjustmentAlgorithm::Pointwise;
  exposure.behavior     = AdjustmentBehavior::Exposure;
  exposure.fused_offset = 0;
  GradeScheduleInput clarity;
  clarity.algorithm    = CompiledAdjustmentAlgorithm::Neighborhood;
  clarity.behavior     = AdjustmentBehavior::Clarity;
  clarity.fused_offset = 64;
  clarity.neighbor     = EnabledNeighbor(AdjustmentBehavior::Clarity);
  GradeScheduleInput saturation;
  saturation.algorithm    = CompiledAdjustmentAlgorithm::Pointwise;
  saturation.behavior     = AdjustmentBehavior::Saturation;
  saturation.fused_offset = 96;
  const GradeScheduleInput inputs[] = {exposure, clarity, saturation};
  const auto schedule = MakeGradeSchedule(inputs, 1.0f, false);
  const auto trace    = MakeGradeDecisionTrace(schedule);
  ASSERT_EQ(trace.op_kinds.size(), 3U);
  EXPECT_EQ(trace.op_kinds[0], GradeOpKind::Fused);
  EXPECT_EQ(trace.op_kinds[1], GradeOpKind::Detail);
  EXPECT_EQ(trace.op_kinds[2], GradeOpKind::Fused);
  EXPECT_EQ(trace.gpu_write_count, 3U);
  const auto slots = GradeWriteSlots(3);
  ASSERT_EQ(slots.size(), 3U);
  EXPECT_EQ(slots[0], GradeImageSlot::Ping);
  EXPECT_EQ(slots[1], GradeImageSlot::Pong);
  EXPECT_EQ(slots[2], GradeImageSlot::Output);
}

TEST(GradeSchedule, InactiveNeighborhoodDoesNotBreakFusion) {
  GradeScheduleInput exposure;
  exposure.algorithm    = CompiledAdjustmentAlgorithm::Pointwise;
  exposure.behavior     = AdjustmentBehavior::Exposure;
  exposure.fused_offset = 0;
  GradeScheduleInput clarity;
  clarity.algorithm    = CompiledAdjustmentAlgorithm::Neighborhood;
  clarity.behavior     = AdjustmentBehavior::Clarity;
  clarity.fused_offset = 32;
  clarity.neighbor.enabled = 0;
  GradeScheduleInput contrast;
  contrast.algorithm    = CompiledAdjustmentAlgorithm::Pointwise;
  contrast.behavior     = AdjustmentBehavior::Contrast;
  contrast.fused_offset = 64;
  const GradeScheduleInput inputs[] = {exposure, clarity, contrast};
  const auto schedule = MakeGradeSchedule(inputs, 0.8f, false);
  const auto trace    = MakeGradeDecisionTrace(schedule);
  ASSERT_EQ(trace.op_kinds.size(), 1U);
  EXPECT_EQ(trace.op_kinds.front(), GradeOpKind::Fused);
  EXPECT_EQ(trace.fused_command_count, 2U);
  EXPECT_FALSE(trace.skip_final_mix);
  EXPECT_EQ(trace.gpu_write_count, 2U);
}

TEST(GradeSchedule, InactiveLocalToneBarrierIsDropped) {
  GradeScheduleInput exposure;
  exposure.algorithm    = CompiledAdjustmentAlgorithm::Pointwise;
  exposure.behavior     = AdjustmentBehavior::Exposure;
  exposure.fused_offset = 0;
  GradeScheduleInput shadows;
  shadows.algorithm   = CompiledAdjustmentAlgorithm::LocalLaplacian;
  shadows.behavior    = AdjustmentBehavior::Shadows;
  shadows.llf_control = 0.0f;
  GradeScheduleInput saturation;
  saturation.algorithm    = CompiledAdjustmentAlgorithm::Pointwise;
  saturation.behavior     = AdjustmentBehavior::Saturation;
  saturation.fused_offset = 48;
  const GradeScheduleInput inputs[] = {exposure, shadows, saturation};
  const auto schedule = MakeGradeSchedule(inputs, 1.0f, true);
  const auto trace    = MakeGradeDecisionTrace(schedule);
  EXPECT_FALSE(trace.local_tone_active);
  ASSERT_EQ(trace.op_kinds.size(), 1U);
  EXPECT_EQ(trace.op_kinds.front(), GradeOpKind::Fused);
  EXPECT_EQ(trace.fused_command_count, 2U);
  EXPECT_FALSE(trace.skip_final_mix);
}

TEST(GradeSchedule, ActiveLocalToneIsOneBarrierAndKeepsIndependentSliders) {
  GradeScheduleInput shadows;
  shadows.algorithm   = CompiledAdjustmentAlgorithm::LocalLaplacian;
  shadows.behavior    = AdjustmentBehavior::Shadows;
  shadows.llf_control = 40.0f;
  GradeScheduleInput highlights;
  highlights.algorithm   = CompiledAdjustmentAlgorithm::LocalLaplacian;
  highlights.behavior    = AdjustmentBehavior::Highlights;
  highlights.llf_control = -25.0f;
  GradeScheduleInput exposure;
  exposure.algorithm    = CompiledAdjustmentAlgorithm::Pointwise;
  exposure.behavior     = AdjustmentBehavior::Exposure;
  exposure.fused_offset = 16;
  const GradeScheduleInput inputs[] = {exposure, shadows, highlights};
  const auto schedule = MakeGradeSchedule(inputs, 1.0f, false);
  const auto trace    = MakeGradeDecisionTrace(schedule);
  EXPECT_TRUE(trace.local_tone_active);
  EXPECT_FLOAT_EQ(schedule.shadows_slider, 40.0f);
  EXPECT_FLOAT_EQ(schedule.highlights_slider, -25.0f);
  ASSERT_EQ(trace.op_kinds.size(), 2U);
  EXPECT_EQ(trace.op_kinds[0], GradeOpKind::Fused);
  EXPECT_EQ(trace.op_kinds[1], GradeOpKind::LlfBarrier);
}

TEST(GradeSchedule, EquivalentInputsProduceIdenticalDecisionTraces) {
  GradeScheduleInput exposure;
  exposure.algorithm    = CompiledAdjustmentAlgorithm::Pointwise;
  exposure.behavior     = AdjustmentBehavior::Exposure;
  exposure.fused_offset = 8;
  GradeScheduleInput shadows;
  shadows.algorithm   = CompiledAdjustmentAlgorithm::LocalLaplacian;
  shadows.behavior    = AdjustmentBehavior::Shadows;
  shadows.llf_control = 20.0f;
  GradeScheduleInput clarity;
  clarity.algorithm    = CompiledAdjustmentAlgorithm::Neighborhood;
  clarity.behavior     = AdjustmentBehavior::Clarity;
  clarity.fused_offset = 40;
  clarity.neighbor     = EnabledNeighbor(AdjustmentBehavior::Clarity);
  const GradeScheduleInput inputs[] = {exposure, shadows, clarity};
  const auto first  = MakeGradeDecisionTrace(MakeGradeSchedule(inputs, 0.7f, true));
  const auto second = MakeGradeDecisionTrace(MakeGradeSchedule(inputs, 0.7f, true));
  EXPECT_EQ(first.alias_to_input, second.alias_to_input);
  EXPECT_EQ(first.skip_final_mix, second.skip_final_mix);
  EXPECT_EQ(first.local_tone_active, second.local_tone_active);
  EXPECT_EQ(first.op_kinds, second.op_kinds);
  EXPECT_EQ(first.fused_command_count, second.fused_command_count);
  EXPECT_EQ(first.gpu_write_count, second.gpu_write_count);
}

}  // namespace
}  // namespace alcedo
