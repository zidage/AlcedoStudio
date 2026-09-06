//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_pending_input.hpp"

#include <gtest/gtest.h>

#include <string>

namespace alcedo {
namespace {

auto TestIdentity() -> EditorSessionIdentity {
  EditorSessionIdentity identity;
  identity.element_id = 7;
  identity.image_id   = 70;
  return identity;
}

auto GradeTarget(std::string node, std::string field) -> EditorParameterTarget {
  EditorParameterTarget target;
  target.owner_kind             = EditorParameterOwnerKind::ColorGrade;
  target.node_id                = NodeId{std::move(node)};
  target.adjustment_instance_id = AdjustmentInstanceId{"tone"};
  target.field_key              = std::move(field);
  return target;
}

auto MakePatch(std::string field, std::string json, bool settled = false) -> EditorAdjustmentPatch {
  EditorAdjustmentPatch patch;
  patch.field_key   = std::move(field);
  patch.params_json = std::move(json);
  patch.settled     = settled;
  return patch;
}

TEST(EditorPendingInputTest, ReplacesAbsoluteSameFieldAndKeepsNewestWritePayload) {
  EditorPendingInputQueue queue;
  auto                    first = MakePatch("exposure", R"({"value":0.10})");
  first.target                  = GradeTarget("grade.a", "exposure");
  auto second                   = MakePatch("exposure", R"({"value":0.30})");
  second.target                 = GradeTarget("grade.a", "exposure");
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(), first).accepted);
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(), second).accepted);

  const auto view = queue.Peek();
  ASSERT_EQ(view.sequences.size(), 1u);
  EXPECT_EQ(view.sequences.front().fields.size(), 1u);
  EXPECT_EQ(view.sequences.front().fields.front().params_json, R"({"value":0.30})");
  EXPECT_EQ(view.sequences.front().captured_target.node_id, NodeId{"grade.a"});
  EXPECT_TRUE(view.sequences.front().captured_target.field_key.empty());
}

TEST(EditorPendingInputTest, PendingDifferentFieldsSurviveInputCoalescing) {
  EditorPendingInputQueue queue;
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(), MakePatch("exposure", R"({"value":0.2})"))
                  .accepted);
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(), MakePatch("contrast", R"({"value":12})"))
                  .accepted);
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(), MakePatch("exposure", R"({"value":0.8})"))
                  .accepted);

  const auto view = queue.Peek();
  ASSERT_EQ(view.sequences.size(), 1u);
  EXPECT_EQ(view.sequences.front().fields.size(), 2u);
  const auto* exposure = FindPendingField(view, "exposure");
  const auto* contrast = FindPendingField(view, "contrast");
  ASSERT_NE(exposure, nullptr);
  ASSERT_NE(contrast, nullptr);
  EXPECT_EQ(exposure->params_json, R"({"value":0.8})");
  EXPECT_EQ(contrast->params_json, R"({"value":12})");
}

TEST(EditorPendingInputTest, ReleaseBeforeFirstPreviewKeepsFinalQueuedValuesOnce) {
  EditorPendingInputQueue queue;
  auto                    patch = MakePatch("exposure", R"({"value":1.25})", true);
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(), patch).accepted);

  const auto view = queue.Peek();
  ASSERT_EQ(view.sequences.size(), 1u);
  EXPECT_EQ(view.sequences.front().seal, EditorPendingInputBoundaryKind::Release);
  EXPECT_EQ(view.sequences.front().fields.size(), 1u);
  EXPECT_EQ(view.sequences.front().fields.front().params_json, R"({"value":1.25})");
}

TEST(EditorPendingInputTest, NodeSwitchKeepsQueuedEditOnOriginalTarget) {
  EditorPendingInputQueue queue;
  auto                    first = MakePatch("exposure", R"({"value":0.4})");
  first.target                  = GradeTarget("grade.a", "exposure");
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(), first).accepted);
  ASSERT_TRUE(
      queue.AdmitBoundary(TestIdentity(), EditorPendingInputBoundaryKind::NodeSwitch).accepted);

  auto second      = MakePatch("exposure", R"({"value":0.9})");
  second.target    = GradeTarget("grade.b", "exposure");
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(), second).accepted);

  const auto view = queue.Peek();
  ASSERT_EQ(view.sequences.size(), 2u);
  EXPECT_EQ(view.sequences[0].seal, EditorPendingInputBoundaryKind::NodeSwitch);
  EXPECT_EQ(view.sequences[0].captured_target.node_id, NodeId{"grade.a"});
  EXPECT_EQ(view.sequences[0].fields.front().target.node_id, NodeId{"grade.a"});
  EXPECT_EQ(view.sequences[0].fields.front().params_json, R"({"value":0.4})");
  EXPECT_EQ(view.sequences[1].captured_target.node_id, NodeId{"grade.b"});
  EXPECT_EQ(view.sequences[1].fields.front().params_json, R"({"value":0.9})");
}

TEST(EditorPendingInputTest, RejectsRetargetingADifferentNodeInTheSameSequence) {
  EditorPendingInputQueue queue;
  auto                    first = MakePatch("exposure", R"({"value":0.1})");
  first.target                  = GradeTarget("grade.a", "exposure");
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(), first).accepted);
  auto second   = MakePatch("exposure", R"({"value":0.2})");
  second.target = GradeTarget("grade.b", "exposure");
  const auto rejected = queue.AdmitFieldChange(TestIdentity(), second);
  EXPECT_FALSE(rejected.accepted);
  EXPECT_NE(rejected.error.find("retarget"), std::string::npos);
  const auto view = queue.Peek();
  ASSERT_EQ(view.sequences.size(), 1u);
  EXPECT_EQ(view.sequences.front().fields.front().params_json, R"({"value":0.1})");
}

TEST(EditorPendingInputTest, CancelDiscardsUnappliedFieldsAndKeepsBoundary) {
  EditorPendingInputQueue queue;
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(), MakePatch("exposure", R"({"value":0.5})"))
                  .accepted);
  ASSERT_TRUE(queue.AdmitBoundary(TestIdentity(), EditorPendingInputBoundaryKind::Cancel).accepted);
  const auto view = queue.Peek();
  ASSERT_EQ(view.sequences.size(), 1u);
  EXPECT_EQ(view.sequences.front().seal, EditorPendingInputBoundaryKind::Cancel);
  EXPECT_TRUE(view.sequences.front().fields.empty());
}

TEST(EditorPendingInputTest, QueuedItemCarriesOnlyChangedFieldPayload) {
  EditorPendingInputQueue queue;
  auto                    patch = MakePatch("exposure", R"({"exposure_ev":0.5})");
  patch.target                  = GradeTarget("grade.primary", "exposure");
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(), patch).accepted);
  const auto view = queue.Peek();
  ASSERT_EQ(view.sequences.front().fields.size(), 1u);
  EXPECT_EQ(view.sequences.front().fields.front().params_json, R"({"exposure_ev":0.5})");
  EXPECT_EQ(view.sequences.front().fields.front().params_json.find("contrast"), std::string::npos);
  EXPECT_EQ(view.sequences.front().fields.front().params_json.find("lut"), std::string::npos);
  EXPECT_TRUE(view.sequences.front().captured_target.field_key.empty());
}

TEST(EditorPendingInputTest, RejectsEmptyFieldKeyAndMissingIdentity) {
  EditorPendingInputQueue queue;
  EXPECT_FALSE(queue.AdmitFieldChange(TestIdentity(), MakePatch("", "{}")).accepted);
  EditorSessionIdentity missing;
  EXPECT_FALSE(queue.AdmitFieldChange(missing, MakePatch("exposure", "{}")).accepted);
}

}  // namespace
}  // namespace alcedo
