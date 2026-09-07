//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/grade_schedule.hpp"

#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "edit/runtime/adjustment_runtime.hpp"
#include "edit/runtime/execution_plan.hpp"

namespace alcedo {
namespace {

auto EnabledNeighbor(AdjustmentBehavior behavior) -> GradeNeighborParams {
  GradeNeighborParams params;
  params.behavior = static_cast<std::uint32_t>(behavior);
  params.enabled  = 1;
  params.radius   = 4;
  return params;
}

auto BoundPointwise(std::uint32_t offset, AdjustmentBehavior behavior = AdjustmentBehavior::Exposure)
    -> GradeScheduleInput {
  GradeScheduleInput input;
  input.algorithm    = CompiledAdjustmentAlgorithm::Pointwise;
  input.behavior     = behavior;
  input.fused_offset = offset;
  return input;
}

auto BoundLocal(AdjustmentBehavior behavior, float control) -> GradeScheduleInput {
  GradeScheduleInput input;
  input.algorithm   = CompiledAdjustmentAlgorithm::LocalLaplacian;
  input.behavior    = behavior;
  input.llf_control = control;
  return input;
}

auto BoundNeighbor(std::uint32_t offset, AdjustmentBehavior behavior, GradeNeighborParams neighbor)
    -> GradeScheduleInput {
  GradeScheduleInput input;
  input.algorithm    = CompiledAdjustmentAlgorithm::Neighborhood;
  input.behavior     = behavior;
  input.fused_offset = offset;
  input.neighbor     = neighbor;
  return input;
}

TEST(GradeSchedule, ZeroMixAliasesWithoutRecordingOps) {
  const CompiledGradeStage stages[] = {{CompiledGradeStageKind::Pointwise, 0, 1}};
  const GradeScheduleInput bound[]  = {BoundPointwise(16)};
  const auto schedule = MakeGradeSchedule(stages, bound, 0.0f, false);
  const auto trace    = MakeGradeDecisionTrace(schedule);
  EXPECT_TRUE(trace.alias_to_input);
  EXPECT_TRUE(schedule.ops.empty());
  EXPECT_EQ(trace.gpu_write_count, 0U);
  EXPECT_TRUE(GradeWriteSlots(0).empty());
}

TEST(GradeSchedule, CompilerPointwiseStageFusesAndSkipsMixAtUnity) {
  const CompiledGradeStage stages[] = {{CompiledGradeStageKind::Pointwise, 0, 2}};
  const GradeScheduleInput bound[]  = {BoundPointwise(0), BoundPointwise(48, AdjustmentBehavior::Contrast)};
  const auto schedule = MakeGradeSchedule(stages, bound, 1.0f, false);
  const auto trace    = MakeGradeDecisionTrace(schedule);
  ASSERT_EQ(trace.op_kinds.size(), 1U);
  EXPECT_EQ(trace.op_kinds.front(), CompiledGradeStageKind::Pointwise);
  EXPECT_EQ(trace.fused_command_count, 2U);
  EXPECT_TRUE(trace.skip_final_mix);
  EXPECT_EQ(trace.gpu_write_count, 1U);
  ASSERT_EQ(GradeWriteSlots(1).size(), 1U);
  EXPECT_EQ(GradeWriteSlots(1).front(), GradeImageSlot::Output);
}

TEST(GradeSchedule, CompilerNeighborhoodStageIsABarrierBetweenPointwiseGroups) {
  const CompiledGradeStage stages[] = {{CompiledGradeStageKind::Pointwise, 0, 1},
                                       {CompiledGradeStageKind::Neighborhood, 1, 1},
                                       {CompiledGradeStageKind::Pointwise, 2, 1}};
  const GradeScheduleInput bound[]  = {
      BoundPointwise(0),
      BoundNeighbor(64, AdjustmentBehavior::Clarity, EnabledNeighbor(AdjustmentBehavior::Clarity)),
      BoundPointwise(96, AdjustmentBehavior::Saturation)};
  const auto schedule = MakeGradeSchedule(stages, bound, 1.0f, false);
  const auto trace    = MakeGradeDecisionTrace(schedule);
  ASSERT_EQ(trace.op_kinds.size(), 3U);
  EXPECT_EQ(trace.op_kinds[0], CompiledGradeStageKind::Pointwise);
  EXPECT_EQ(trace.op_kinds[1], CompiledGradeStageKind::Neighborhood);
  EXPECT_EQ(trace.op_kinds[2], CompiledGradeStageKind::Pointwise);
  EXPECT_EQ(trace.gpu_write_count, 3U);
  const auto slots = GradeWriteSlots(3);
  ASSERT_EQ(slots.size(), 3U);
  EXPECT_EQ(slots[0], GradeImageSlot::Ping);
  EXPECT_EQ(slots[1], GradeImageSlot::Pong);
  EXPECT_EQ(slots[2], GradeImageSlot::Output);
}

TEST(GradeSchedule, DisabledCompilerNeighborhoodDoesNotBreakPointwiseConcatenation) {
  GradeNeighborParams disabled;
  disabled.enabled = 0;
  const CompiledGradeStage stages[] = {{CompiledGradeStageKind::Pointwise, 0, 1},
                                       {CompiledGradeStageKind::Neighborhood, 1, 1},
                                       {CompiledGradeStageKind::Pointwise, 2, 1}};
  const GradeScheduleInput bound[]  = {
      BoundPointwise(0), BoundNeighbor(32, AdjustmentBehavior::Clarity, disabled),
      BoundPointwise(64, AdjustmentBehavior::Contrast)};
  const auto schedule = MakeGradeSchedule(stages, bound, 0.8f, false);
  const auto trace    = MakeGradeDecisionTrace(schedule);
  ASSERT_EQ(trace.op_kinds.size(), 1U);
  EXPECT_EQ(trace.op_kinds.front(), CompiledGradeStageKind::Pointwise);
  EXPECT_EQ(trace.fused_command_count, 2U);
  EXPECT_FALSE(trace.skip_final_mix);
  EXPECT_EQ(trace.gpu_write_count, 2U);
}

TEST(GradeSchedule, InactiveLocalLaplacianSelectsCompilerPointwiseRangesAsOneLaunch) {
  const CompiledGradeStage stages[] = {{CompiledGradeStageKind::Pointwise, 0, 1},
                                       {CompiledGradeStageKind::LocalLaplacian, 1, 1},
                                       {CompiledGradeStageKind::Pointwise, 2, 1}};
  const GradeScheduleInput bound[]  = {
      BoundPointwise(0), BoundLocal(AdjustmentBehavior::Shadows, 0.0f),
      BoundPointwise(48, AdjustmentBehavior::Saturation)};
  const auto schedule = MakeGradeSchedule(stages, bound, 1.0f, true);
  const auto trace    = MakeGradeDecisionTrace(schedule);
  EXPECT_FALSE(trace.local_tone_active);
  ASSERT_EQ(trace.op_kinds.size(), 1U);
  EXPECT_EQ(trace.op_kinds.front(), CompiledGradeStageKind::Pointwise);
  EXPECT_EQ(trace.fused_command_count, 2U);
  EXPECT_FALSE(trace.skip_final_mix);
}

TEST(GradeSchedule, ActiveLocalLaplacianKeepsCompilerStageOrder) {
  const CompiledGradeStage stages[] = {{CompiledGradeStageKind::Pointwise, 0, 1},
                                       {CompiledGradeStageKind::LocalLaplacian, 1, 2}};
  const GradeScheduleInput bound[]  = {BoundPointwise(16),
                                       BoundLocal(AdjustmentBehavior::Shadows, 40.0f),
                                       BoundLocal(AdjustmentBehavior::Highlights, -25.0f)};
  const auto schedule = MakeGradeSchedule(stages, bound, 1.0f, false);
  const auto trace    = MakeGradeDecisionTrace(schedule);
  EXPECT_TRUE(trace.local_tone_active);
  EXPECT_FLOAT_EQ(schedule.shadows_slider, 40.0f);
  EXPECT_FLOAT_EQ(schedule.highlights_slider, -25.0f);
  ASSERT_EQ(trace.op_kinds.size(), 2U);
  EXPECT_EQ(trace.op_kinds[0], CompiledGradeStageKind::Pointwise);
  EXPECT_EQ(trace.op_kinds[1], CompiledGradeStageKind::LocalLaplacian);
}

TEST(GradeSchedule, EquivalentCompilerStagesProduceIdenticalDecisionTraces) {
  const CompiledGradeStage stages[] = {{CompiledGradeStageKind::Pointwise, 0, 1},
                                       {CompiledGradeStageKind::LocalLaplacian, 1, 1},
                                       {CompiledGradeStageKind::Neighborhood, 2, 1}};
  const GradeScheduleInput bound[]  = {
      BoundPointwise(8), BoundLocal(AdjustmentBehavior::Shadows, 20.0f),
      BoundNeighbor(40, AdjustmentBehavior::Clarity, EnabledNeighbor(AdjustmentBehavior::Clarity))};
  const auto first  = MakeGradeDecisionTrace(MakeGradeSchedule(stages, bound, 0.7f, true));
  const auto second = MakeGradeDecisionTrace(MakeGradeSchedule(stages, bound, 0.7f, true));
  EXPECT_EQ(first.alias_to_input, second.alias_to_input);
  EXPECT_EQ(first.skip_final_mix, second.skip_final_mix);
  EXPECT_EQ(first.local_tone_active, second.local_tone_active);
  EXPECT_EQ(first.op_kinds, second.op_kinds);
  EXPECT_EQ(first.fused_command_count, second.fused_command_count);
  EXPECT_EQ(first.gpu_write_count, second.gpu_write_count);
}

}  // namespace
}  // namespace alcedo
