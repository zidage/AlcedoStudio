//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <map>
#include <stdexcept>
#include <vector>

#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/result_content_key.hpp"

namespace alcedo {
namespace {

auto MakeBranchJoinPlan() -> ExecutionPlan {
  ExecutionPlan plan;
  const GraphValueId develop{NodeId{"develop"}, PortId{"image"}};
  const GraphValueId grade_a{NodeId{"grade.a"}, PortId{"image"}};
  const GraphValueId grade_b{NodeId{"grade.b"}, PortId{"image"}};
  const GraphValueId joined{NodeId{"join"}, PortId{"image"}};
  plan.develop_output = develop;
  plan.display_output = joined;
  plan.passes.push_back(MakeGpuPass(GpuPassKind::UploadRgb, NodeId{"develop"}, 0, {},
                                    {{develop, CompiledValueKind::SceneImage}}));
  plan.passes.push_back(MakeGpuPass(
      GpuPassKind::PrimaryColorGrade, NodeId{"grade.a"}, 0,
      {{PortId{"image"}, develop, CompiledValueKind::SceneImage}},
      {{grade_a, CompiledValueKind::SceneImage}}));
  plan.passes.push_back(MakeGpuPass(
      GpuPassKind::PrimaryColorGrade, NodeId{"grade.b"}, 1,
      {{PortId{"image"}, develop, CompiledValueKind::SceneImage}},
      {{grade_b, CompiledValueKind::SceneImage}}));
  plan.passes.push_back(MakeGpuPass(
      GpuPassKind::Drt, NodeId{"join"}, 0,
      {{PortId{"in.a"}, grade_a, CompiledValueKind::SceneImage},
       {PortId{"in.b"}, grade_b, CompiledValueKind::SceneImage}},
      {{joined, CompiledValueKind::SceneImage}}));
  ValidateExecutionPlan(plan);
  return plan;
}

TEST(GpuDagBranchJoinPlan, SharedInputSurvivesBothBranchReaders) {
  const auto plan = MakeBranchJoinPlan();
  RemainingValueConsumers remaining(plan);
  const GraphValueId      develop{NodeId{"develop"}, PortId{"image"}};
  EXPECT_EQ(remaining.Remaining(develop), 2U);
  remaining.Consume(develop);
  EXPECT_EQ(remaining.Remaining(develop), 1U);
  EXPECT_FALSE(remaining.Exhausted(develop));
  remaining.Consume(develop);
  EXPECT_TRUE(remaining.Exhausted(develop));
  EXPECT_THROW(remaining.Consume(develop), std::runtime_error);

  RemainingValueConsumers join_readers(plan);
  const GraphValueId      grade_a{NodeId{"grade.a"}, PortId{"image"}};
  const GraphValueId      grade_b{NodeId{"grade.b"}, PortId{"image"}};
  EXPECT_EQ(join_readers.Remaining(grade_a), 1U);
  EXPECT_EQ(join_readers.Remaining(grade_b), 1U);
  join_readers.Consume(grade_a);
  EXPECT_TRUE(join_readers.Exhausted(grade_a));
  EXPECT_FALSE(join_readers.Exhausted(grade_b));
}

TEST(GpuDagBranchJoinPlan, JoinInputsFollowPortBindings) {
  const auto plan = MakeBranchJoinPlan();
  const GraphValueId grade_a{NodeId{"grade.a"}, PortId{"image"}};
  const GraphValueId grade_b{NodeId{"grade.b"}, PortId{"image"}};
  std::map<GraphValueId, ContentKey> produced;
  produced[grade_a] = ContentKey{11};
  produced[grade_b] = ContentKey{22};

  const auto& join = plan.passes.back();
  ASSERT_EQ(join.inputs.size(), 2U);
  const auto in_plan_order = HashBoundInputs(join.inputs, produced);

  std::vector<CompiledPassInput> reversed = {join.inputs.back(), join.inputs.front()};
  EXPECT_EQ(HashBoundInputs(reversed, produced), in_plan_order);

  std::vector<CompiledPassInput> swapped = join.inputs;
  swapped.front().source                 = grade_b;
  swapped.back().source                  = grade_a;
  EXPECT_NE(HashBoundInputs(swapped, produced), in_plan_order);
}

TEST(GpuDagBranchJoinPlan, BranchEditPreservesSiblingResult) {
  const GraphValueId develop{NodeId{"develop"}, PortId{"image"}};
  const GraphValueId grade_a{NodeId{"grade.a"}, PortId{"image"}};
  const GraphValueId grade_b{NodeId{"grade.b"}, PortId{"image"}};
  std::map<GraphValueId, ContentKey> produced;
  produced[develop] = ContentKey{100};

  const std::vector<CompiledPassInput> a_inputs{
      {PortId{"image"}, develop, CompiledValueKind::SceneImage}};
  const std::vector<CompiledPassInput> b_inputs{
      {PortId{"image"}, develop, CompiledValueKind::SceneImage}};
  ContentHash a_params;
  a_params.MixU32(1);
  ContentHash b_params;
  b_params.MixU32(2);
  ContentHash a_out;
  a_out.MixKey(HashBoundInputs(a_inputs, produced));
  a_out.MixKey(a_params.Key());
  ContentHash b_out;
  b_out.MixKey(HashBoundInputs(b_inputs, produced));
  b_out.MixKey(b_params.Key());
  produced[grade_a] = a_out.Key();
  produced[grade_b] = b_out.Key();

  const auto plan     = MakeBranchJoinPlan();
  const auto join_key = HashBoundInputs(plan.passes.back().inputs, produced);

  ContentHash a_edited;
  a_edited.MixU32(3);
  ContentHash a_out_edited;
  a_out_edited.MixKey(HashBoundInputs(a_inputs, produced));
  a_out_edited.MixKey(a_edited.Key());
  auto edited_produced     = produced;
  edited_produced[grade_a] = a_out_edited.Key();
  EXPECT_EQ(edited_produced[grade_b], produced[grade_b]);
  EXPECT_NE(HashBoundInputs(plan.passes.back().inputs, edited_produced), join_key);
}

}  // namespace
}  // namespace alcedo
