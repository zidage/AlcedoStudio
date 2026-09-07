//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_pending_input.hpp"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <variant>
#include <vector>

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

auto MakePatch(std::string field, float value, bool settled = false) -> EditorAdjustmentPatch {
  EditorAdjustmentPatch patch;
  patch.field_key = std::move(field);
  patch.write     = EditorScalarWrite{value};
  patch.settled   = settled;
  return patch;
}

TEST(EditorPendingInputTest, ReplacesAbsoluteSameFieldAndKeepsNewestWritePayload) {
  EditorPendingInputQueue queue;
  auto                    first = MakePatch("exposure", 0.10f);
  first.target                  = GradeTarget("grade.a", "exposure");
  auto second                   = MakePatch("exposure", 0.30f);
  second.target                 = GradeTarget("grade.a", "exposure");
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(), first).accepted);
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(), second).accepted);

  const auto view = queue.Peek();
  ASSERT_EQ(view.sequences.size(), 1u);
  EXPECT_EQ(view.sequences.front().fields.size(), 1u);
  EXPECT_EQ(PendingScalarValue(view.sequences.front().fields.front()), 0.30f);
  EXPECT_EQ(view.sequences.front().captured_target.node_id, NodeId{"grade.a"});
  EXPECT_TRUE(view.sequences.front().captured_target.field_key.empty());
}

TEST(EditorPendingInputTest, PendingDifferentFieldsSurviveInputCoalescing) {
  EditorPendingInputQueue queue;
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(), MakePatch("exposure", 0.2f)).accepted);
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(), MakePatch("contrast", 12.0f)).accepted);
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(), MakePatch("exposure", 0.8f)).accepted);

  const auto view = queue.Peek();
  ASSERT_EQ(view.sequences.size(), 1u);
  EXPECT_EQ(view.sequences.front().fields.size(), 2u);
  const auto* exposure = FindPendingField(view, "exposure");
  const auto* contrast = FindPendingField(view, "contrast");
  ASSERT_NE(exposure, nullptr);
  ASSERT_NE(contrast, nullptr);
  EXPECT_EQ(PendingScalarValue(*exposure), 0.8f);
  EXPECT_EQ(PendingScalarValue(*contrast), 12.0f);
}

TEST(EditorPendingInputTest, ReleaseBeforeFirstPreviewKeepsFinalQueuedValuesOnce) {
  EditorPendingInputQueue queue;
  auto                    patch = MakePatch("exposure", 1.25f, true);
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(), patch).accepted);

  const auto view = queue.Peek();
  ASSERT_EQ(view.sequences.size(), 1u);
  EXPECT_EQ(view.sequences.front().seal, EditorPendingInputBoundaryKind::Release);
  EXPECT_EQ(view.sequences.front().fields.size(), 1u);
  EXPECT_EQ(PendingScalarValue(view.sequences.front().fields.front()), 1.25f);
}

TEST(EditorPendingInputTest, NodeSwitchKeepsQueuedEditOnOriginalTarget) {
  EditorPendingInputQueue queue;
  auto                    first = MakePatch("exposure", 0.4f);
  first.target                  = GradeTarget("grade.a", "exposure");
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(), first).accepted);
  ASSERT_TRUE(
      queue.AdmitBoundary(TestIdentity(), EditorPendingInputBoundaryKind::NodeSwitch).accepted);

  auto second   = MakePatch("exposure", 0.9f);
  second.target = GradeTarget("grade.b", "exposure");
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(), second).accepted);

  const auto view = queue.Peek();
  ASSERT_EQ(view.sequences.size(), 2u);
  EXPECT_EQ(view.sequences[0].seal, EditorPendingInputBoundaryKind::NodeSwitch);
  EXPECT_EQ(view.sequences[0].captured_target.node_id, NodeId{"grade.a"});
  EXPECT_EQ(view.sequences[0].fields.front().target.node_id, NodeId{"grade.a"});
  EXPECT_EQ(PendingScalarValue(view.sequences[0].fields.front()), 0.4f);
  EXPECT_EQ(view.sequences[1].captured_target.node_id, NodeId{"grade.b"});
  EXPECT_EQ(PendingScalarValue(view.sequences[1].fields.front()), 0.9f);
}

TEST(EditorPendingInputTest, RejectsRetargetingADifferentNodeInTheSameSequence) {
  EditorPendingInputQueue queue;
  auto                    first = MakePatch("exposure", 0.1f);
  first.target                  = GradeTarget("grade.a", "exposure");
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(), first).accepted);
  auto second         = MakePatch("exposure", 0.2f);
  second.target       = GradeTarget("grade.b", "exposure");
  const auto rejected = queue.AdmitFieldChange(TestIdentity(), second);
  EXPECT_FALSE(rejected.accepted);
  EXPECT_NE(rejected.error.find("retarget"), std::string::npos);
  const auto view = queue.Peek();
  ASSERT_EQ(view.sequences.size(), 1u);
  EXPECT_EQ(PendingScalarValue(view.sequences.front().fields.front()), 0.1f);
}

TEST(EditorPendingInputTest, CancelDiscardsUnappliedFieldsAndKeepsBoundary) {
  EditorPendingInputQueue queue;
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(), MakePatch("exposure", 0.5f)).accepted);
  ASSERT_TRUE(queue.AdmitBoundary(TestIdentity(), EditorPendingInputBoundaryKind::Cancel).accepted);
  const auto view = queue.Peek();
  ASSERT_EQ(view.sequences.size(), 1u);
  EXPECT_EQ(view.sequences.front().seal, EditorPendingInputBoundaryKind::Cancel);
  EXPECT_TRUE(view.sequences.front().fields.empty());
}

TEST(EditorPendingInputTest, QueuedItemCarriesOnlyChangedFieldPayload) {
  EditorPendingInputQueue queue;
  auto                    patch = MakePatch("exposure", 0.5f);
  patch.target                  = GradeTarget("grade.primary", "exposure");
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(), patch).accepted);
  const auto view = queue.Peek();
  ASSERT_EQ(view.sequences.front().fields.size(), 1u);
  EXPECT_TRUE(std::holds_alternative<EditorScalarWrite>(view.sequences.front().fields.front().write));
  EXPECT_EQ(PendingScalarValue(view.sequences.front().fields.front()), 0.5f);
  EXPECT_TRUE(view.sequences.front().captured_target.field_key.empty());
}

TEST(EditorPendingInputTest, RejectsEmptyFieldKeyAndMissingIdentity) {
  EditorPendingInputQueue queue;
  EXPECT_FALSE(queue.AdmitFieldChange(TestIdentity(), MakePatch("", 0.0f)).accepted);
  EditorSessionIdentity missing;
  EXPECT_FALSE(queue.AdmitFieldChange(missing, MakePatch("exposure", 0.0f)).accepted);
}

TEST(EditorPendingInputTest, RejectsMissingTypedFieldWrite) {
  EditorPendingInputQueue queue;
  EditorAdjustmentPatch   patch;
  patch.field_key = "exposure";
  const auto rejected = queue.AdmitFieldChange(TestIdentity(), patch);
  EXPECT_FALSE(rejected.accepted);
  EXPECT_NE(rejected.error.find("typed field write"), std::string::npos);
}

TEST(EditorPendingInputTest, TakeReadyBatchTransfersOpenFieldWritesAndLeavesSequenceOpen) {
  EditorPendingInputQueue queue;
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(), MakePatch("exposure", 0.2f)).accepted);
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(), MakePatch("contrast", 4.0f)).accepted);

  const auto batch = queue.TakeReadyBatch();
  ASSERT_TRUE(batch.has_value());
  EXPECT_EQ(batch->seal, EditorPendingInputBoundaryKind::None);
  EXPECT_EQ(batch->fields.size(), 2u);
  EXPECT_EQ(PendingScalarValue(batch->fields.front()), 0.2f);
  EXPECT_FALSE(queue.HasConsumableWork());
  const auto leftover = queue.Peek();
  ASSERT_EQ(leftover.sequences.size(), 1u);
  EXPECT_TRUE(leftover.sequences.front().fields.empty());

  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(), MakePatch("exposure", 0.9f)).accepted);
  EXPECT_TRUE(queue.HasConsumableWork());
  const auto second = queue.TakeReadyBatch();
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->fields.size(), 1u);
  EXPECT_EQ(PendingScalarValue(second->fields.front()), 0.9f);
}

TEST(EditorPendingInputTest, TakeReadyBatchPrefersSealedSequenceOverOpenWrites) {
  EditorPendingInputQueue queue;
  auto                    first = MakePatch("exposure", 0.4f, true);
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(), first).accepted);
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(), MakePatch("contrast", 8.0f)).accepted);

  const auto sealed = queue.TakeReadyBatch();
  ASSERT_TRUE(sealed.has_value());
  EXPECT_EQ(sealed->seal, EditorPendingInputBoundaryKind::Release);
  EXPECT_EQ(PendingScalarValue(sealed->fields.front()), 0.4f);
  const auto open_batch = queue.TakeReadyBatch();
  ASSERT_TRUE(open_batch.has_value());
  EXPECT_EQ(open_batch->seal, EditorPendingInputBoundaryKind::None);
  EXPECT_EQ(PendingScalarValue(open_batch->fields.front()), 8.0f);
  EXPECT_FALSE(queue.TakeReadyBatch().has_value());
}

TEST(EditorPendingInputTest, TakeReadyBatchKeepsEmptyCancelSeal) {
  EditorPendingInputQueue queue;
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(), MakePatch("exposure", 0.5f)).accepted);
  ASSERT_TRUE(queue.AdmitBoundary(TestIdentity(), EditorPendingInputBoundaryKind::Cancel).accepted);
  const auto batch = queue.TakeReadyBatch();
  ASSERT_TRUE(batch.has_value());
  EXPECT_EQ(batch->seal, EditorPendingInputBoundaryKind::Cancel);
  EXPECT_TRUE(batch->fields.empty());
  EXPECT_FALSE(queue.HasConsumableWork());
}

auto TypedPatch(std::string field, EditorParameterWrite write) -> EditorAdjustmentPatch {
  EditorAdjustmentPatch patch;
  patch.field_key = std::move(field);
  patch.write     = std::move(write);
  return patch;
}

TEST(EditorPendingInputTest, OpenSequenceHoldsIndependentTypedWritesUntilMovedOnConsume) {
  EditorPendingInputQueue queue;
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(), MakePatch("exposure", 0.4f)).accepted);
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(),
                                     TypedPatch("curve", EditorCurveWrite{{{0.0f, 0.0f},
                                                                           {0.5f, 0.6f},
                                                                           {1.0f, 1.0f}}}))
                  .accepted);
  ASSERT_TRUE(
      queue.AdmitFieldChange(TestIdentity(), TypedPatch("lut", EditorLutWrite{"looks/film.cube"}))
          .accepted);
  DevelopRawDecodeUpdate raw;
  raw.demosaic_method = "neural_engine";
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(), TypedPatch("raw_decode", raw)).accepted);
  DrtParameterUpdate odt;
  odt.method = DrtMethod::Aces20;
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(), TypedPatch("odt", odt)).accepted);
  DevelopLensCalibrationUpdate lens;
  lens.apply_distortion = true;
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(), TypedPatch("lens_calib", lens)).accepted);
  ImageGeometryUpdate geometry;
  geometry.rotation_degrees = 3.0f;
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(), TypedPatch("crop_rotate", geometry)).accepted);
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(),
                                     TypedPatch("color_temp_mode", EditorEnumWrite{"custom"}))
                  .accepted);
  ASSERT_TRUE(queue.AdmitFieldChange(TestIdentity(),
                                     TypedPatch("lens_calib_enabled", EditorToggleWrite{true}))
                  .accepted);

  const auto view = queue.Peek();
  ASSERT_EQ(view.sequences.size(), 1u);
  ASSERT_EQ(view.sequences.front().fields.size(), 9u);
  const auto* exposure = FindPendingField(view, "exposure");
  const auto* curve    = FindPendingField(view, "curve");
  const auto* lut      = FindPendingField(view, "lut");
  const auto* raw_field = FindPendingField(view, "raw_decode");
  const auto* odt_field = FindPendingField(view, "odt");
  const auto* lens_field = FindPendingField(view, "lens_calib");
  const auto* crop     = FindPendingField(view, "crop_rotate");
  const auto* mode     = FindPendingField(view, "color_temp_mode");
  const auto* enabled  = FindPendingField(view, "lens_calib_enabled");
  ASSERT_NE(exposure, nullptr);
  ASSERT_NE(curve, nullptr);
  ASSERT_NE(lut, nullptr);
  ASSERT_NE(raw_field, nullptr);
  ASSERT_NE(odt_field, nullptr);
  ASSERT_NE(lens_field, nullptr);
  ASSERT_NE(crop, nullptr);
  ASSERT_NE(mode, nullptr);
  ASSERT_NE(enabled, nullptr);
  EXPECT_TRUE(std::holds_alternative<EditorScalarWrite>(exposure->write));
  EXPECT_TRUE(std::holds_alternative<EditorCurveWrite>(curve->write));
  EXPECT_TRUE(std::holds_alternative<EditorLutWrite>(lut->write));
  EXPECT_TRUE(std::holds_alternative<DevelopRawDecodeUpdate>(raw_field->write));
  EXPECT_TRUE(std::holds_alternative<DrtParameterUpdate>(odt_field->write));
  EXPECT_TRUE(std::holds_alternative<DevelopLensCalibrationUpdate>(lens_field->write));
  EXPECT_TRUE(std::holds_alternative<ImageGeometryUpdate>(crop->write));
  EXPECT_TRUE(std::holds_alternative<EditorEnumWrite>(mode->write));
  EXPECT_TRUE(std::holds_alternative<EditorToggleWrite>(enabled->write));

  const auto batch = queue.TakeReadyBatch();
  ASSERT_TRUE(batch.has_value());
  EXPECT_EQ(batch->fields.size(), 9u);
  const auto leftover = queue.Peek();
  ASSERT_EQ(leftover.sequences.size(), 1u);
  EXPECT_TRUE(leftover.sequences.front().fields.empty());
}

}  // namespace
}  // namespace alcedo
