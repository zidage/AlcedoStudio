//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_adjustment_pipeline.hpp"
#include "support/editor_parameter_write_test.hpp"

#include <gtest/gtest.h>

#include <string>

#include "edit/operators/operator_registeration.hpp"
#include "edit/pipeline/default_pipeline_params.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"

namespace alcedo {
namespace {

class EditorAdjustmentPipelineTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { RegisterAllOperators(); }
};

TEST_F(EditorAdjustmentPipelineTest, AppliesTonePatchesToMatchingOperators) {
  CPUPipelineExecutor executor;
  executor.ResetToCleanBaselineAdjustments();

  EditorRenderAdjustmentSnapshot snapshot;
  snapshot.patches = {
      alcedo::test::SnapshotPatch({"exposure", R"({"exposure":-0.75})", false}),
      alcedo::test::SnapshotPatch({"contrast", R"({"contrast":18.0})", false}),
  };

  std::string error;
  ASSERT_TRUE(ApplyEditorAdjustmentSnapshot(executor, snapshot, &error)) << error;

  const auto exposure =
      executor.GetStage(PipelineStageName::Basic_Adjustment).GetOperator(OperatorType::EXPOSURE);
  const auto contrast =
      executor.GetStage(PipelineStageName::Basic_Adjustment).GetOperator(OperatorType::CONTRAST);
  ASSERT_TRUE(exposure.has_value());
  ASSERT_TRUE(contrast.has_value());
  EXPECT_FLOAT_EQ(exposure.value()->ExportOperatorParams()["params"]["exposure"].get<float>(),
                  -0.75f);
  EXPECT_FLOAT_EQ(contrast.value()->ExportOperatorParams()["params"]["contrast"].get<float>(),
                  18.0f);
}

TEST_F(EditorAdjustmentPipelineTest, LatestSnapshotCanUpdateExistingFieldAgain) {
  CPUPipelineExecutor executor;
  executor.ResetToCleanBaselineAdjustments();

  EditorRenderAdjustmentSnapshot first;
  first.patches = {alcedo::test::SnapshotPatch({"exposure", R"({"exposure":0.25})", false})};
  EditorRenderAdjustmentSnapshot latest;
  latest.patches = {alcedo::test::SnapshotPatch({"exposure", R"({"exposure":1.25})", false})};

  std::string error;
  ASSERT_TRUE(ApplyEditorAdjustmentSnapshot(executor, first, &error)) << error;
  ASSERT_TRUE(ApplyEditorAdjustmentSnapshot(executor, latest, &error)) << error;

  const auto exposure =
      executor.GetStage(PipelineStageName::Basic_Adjustment).GetOperator(OperatorType::EXPOSURE);
  ASSERT_TRUE(exposure.has_value());
  EXPECT_FLOAT_EQ(exposure.value()->ExportOperatorParams()["params"]["exposure"].get<float>(),
                  1.25f);
}

TEST_F(EditorAdjustmentPipelineTest, SnapshotTouchesImageLoadingDetectsRawPatch) {
  EditorRenderAdjustmentSnapshot tone_only;
  tone_only.patches = {alcedo::test::SnapshotPatch({"exposure", R"({"exposure":0.5})", false})};
  EXPECT_FALSE(SnapshotTouchesImageLoading(tone_only));

  EditorRenderAdjustmentSnapshot raw_patch;
  raw_patch.patches = {alcedo::test::SnapshotPatch({"raw_decode", R"({"raw":{"method":"default"}})", true})};
  EXPECT_TRUE(SnapshotTouchesImageLoading(raw_patch));
}

// Content-path regression: re-applying an unchanged Image Loading operator must
// not clear stage cache. View/detail/scope renders must not call this API at
// all (see ReasonAppliesAdjustmentSnapshot); if they do, RAW_DECODE misses.
TEST_F(EditorAdjustmentPipelineTest,
       PreservesLensCalibrationCacheWhenCumulativeSnapshotReappliesLensPatch) {
  CPUPipelineExecutor executor(true);
  executor.ResetToCleanBaselineAdjustments();

  auto lens_params                        = pipeline_defaults::MakeDefaultLensCalibParams();
  lens_params["lens_calib"]["enabled"]    = true;
  lens_params["lens_calib"]["lens_maker"] = "Nikon";
  lens_params["lens_calib"]["lens_model"] = "Nikkor Test Lens";

  EditorRenderAdjustmentSnapshot first;
  first.patches = {
      alcedo::test::SnapshotPatch({"lens_calib", lens_params.dump(), false}),
      alcedo::test::SnapshotPatch({"exposure", R"({"exposure":0.25})", false}),
  };

  std::string error;
  ASSERT_TRUE(ApplyEditorAdjustmentSnapshot(executor, first, &error)) << error;

  auto& loading = executor.GetStage(PipelineStageName::Image_Loading);
  ASSERT_TRUE(loading.GetOperator(OperatorType::LENS_CALIBRATION).has_value());
  loading.SetOutputCacheValid(true);
  const auto lens_before =
      loading.GetOperator(OperatorType::LENS_CALIBRATION).value()->op_->GetParams();

  EditorRenderAdjustmentSnapshot second;
  second.patches = {
      alcedo::test::SnapshotPatch({"lens_calib", lens_params.dump(), false}),
      alcedo::test::SnapshotPatch({"exposure", R"({"exposure":0.75})", false}),
  };
  ASSERT_TRUE(ApplyEditorAdjustmentSnapshot(executor, second, &error)) << error;

  EXPECT_TRUE(loading.CacheValid());
  EXPECT_EQ(loading.GetOperator(OperatorType::LENS_CALIBRATION).value()->op_->GetParams(),
            lens_before);

  loading.SetOutputCacheValid(true);
  auto changed_lens_params                        = lens_params;
  changed_lens_params["lens_calib"]["lens_model"] = "Another Test Lens";
  EditorRenderAdjustmentSnapshot changed;
  changed.patches = {
      alcedo::test::SnapshotPatch({"lens_calib", changed_lens_params.dump(), false}),
  };
  ASSERT_TRUE(ApplyEditorAdjustmentSnapshot(executor, changed, &error)) << error;
  EXPECT_FALSE(loading.CacheValid());
}

TEST_F(EditorAdjustmentPipelineTest, RejectsUnknownFieldWithoutMutatingKnownOperators) {
  CPUPipelineExecutor executor;
  executor.ResetToCleanBaselineAdjustments();
  const auto before = executor.GetStage(PipelineStageName::Basic_Adjustment)
                          .GetOperator(OperatorType::EXPOSURE)
                          .value()
                          ->ExportOperatorParams();

  EditorRenderAdjustmentSnapshot snapshot;
  snapshot.patches = {alcedo::test::SnapshotPatch({"not_a_field", R"({"value":1})", false})};

  std::string error;
  EXPECT_FALSE(ApplyEditorAdjustmentSnapshot(executor, snapshot, &error));
  EXPECT_NE(error.find("not_a_field"), std::string::npos);
  EXPECT_EQ(executor.GetStage(PipelineStageName::Basic_Adjustment)
                .GetOperator(OperatorType::EXPOSURE)
                .value()
                ->ExportOperatorParams(),
            before);
}

TEST_F(EditorAdjustmentPipelineTest, GeometryOverlayPreviewKeepsCropParametersButDisablesOperator) {
  CPUPipelineExecutor executor;
  executor.ResetToCleanBaselineAdjustments();

  EditorRenderAdjustmentSnapshot snapshot;
  snapshot.patches = {alcedo::test::SnapshotPatch({
      "crop_rotate",
      R"({"crop_rotate":{"enabled":true,"angle_degrees":35.0,"enable_crop":true,"crop_rect":{"x":0.1,"y":0.1,"w":0.7,"h":0.7}}})",
      false})};

  std::string error;
  ASSERT_TRUE(ApplyEditorAdjustmentSnapshot(executor, snapshot, &error)) << error;
  auto& stage = executor.GetStage(PipelineStageName::Geometry_Adjustment);
  ASSERT_TRUE(stage.GetOperator(OperatorType::CROP_ROTATE).has_value());
  EXPECT_TRUE(stage.GetOperator(OperatorType::CROP_ROTATE).value()->enable_);

  DisableEditorGeometryOperatorForOverlay(executor);

  const auto crop = stage.GetOperator(OperatorType::CROP_ROTATE);
  ASSERT_TRUE(crop.has_value());
  EXPECT_FALSE(crop.value()->enable_);
  EXPECT_FLOAT_EQ(crop.value()->op_->GetParams()["crop_rotate"]["angle_degrees"].get<float>(),
                  35.0f);
}

TEST_F(EditorAdjustmentPipelineTest, WritePayloadMapsOntoDocumentAndExecutorKeys) {
  const auto from_field = EditorAdjustmentDocumentParamsFromWrite("exposure", {{"exposure", 1.25}});
  EXPECT_FLOAT_EQ(from_field.at("exposure_ev").get<float>(), 1.25f);
  EXPECT_FALSE(from_field.contains("exposure"));

  const auto from_value = EditorAdjustmentDocumentParamsFromWrite("exposure", {{"value", 0.5}});
  EXPECT_FLOAT_EQ(from_value.at("exposure_ev").get<float>(), 0.5f);

  const auto from_model =
      EditorAdjustmentExecutorParamsFromWrite("exposure", {{"exposure_ev", -0.75}});
  EXPECT_FLOAT_EQ(from_model.at("exposure").get<float>(), -0.75f);
  EXPECT_FALSE(from_model.contains("exposure_ev"));

  CPUPipelineExecutor executor;
  executor.ResetToCleanBaselineAdjustments();
  EditorRenderAdjustmentSnapshot snapshot;
  snapshot.patches = {alcedo::test::SnapshotPatch({
      "exposure", EditorAdjustmentExecutorParamsFromWrite("exposure", {{"value", 2.0}}).dump(),
      false})};
  std::string error;
  ASSERT_TRUE(ApplyEditorAdjustmentSnapshot(executor, snapshot, &error)) << error;
  const auto exposure =
      executor.GetStage(PipelineStageName::Basic_Adjustment).GetOperator(OperatorType::EXPOSURE);
  ASSERT_TRUE(exposure.has_value());
  EXPECT_FLOAT_EQ(exposure.value()->ExportOperatorParams()["params"]["exposure"].get<float>(),
                  2.0f);
}

}  // namespace
}  // namespace alcedo
