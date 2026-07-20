//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_adjustment_pipeline.hpp"

#include <gtest/gtest.h>

#include <string>

#include "edit/operators/operator_registeration.hpp"
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
      EditorAdjustmentPatch{"exposure", R"({"exposure":-0.75})", false},
      EditorAdjustmentPatch{"contrast", R"({"contrast":18.0})", false},
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
  first.patches = {EditorAdjustmentPatch{"exposure", R"({"exposure":0.25})", false}};
  EditorRenderAdjustmentSnapshot latest;
  latest.patches = {EditorAdjustmentPatch{"exposure", R"({"exposure":1.25})", false}};

  std::string error;
  ASSERT_TRUE(ApplyEditorAdjustmentSnapshot(executor, first, &error)) << error;
  ASSERT_TRUE(ApplyEditorAdjustmentSnapshot(executor, latest, &error)) << error;

  const auto exposure =
      executor.GetStage(PipelineStageName::Basic_Adjustment).GetOperator(OperatorType::EXPOSURE);
  ASSERT_TRUE(exposure.has_value());
  EXPECT_FLOAT_EQ(exposure.value()->ExportOperatorParams()["params"]["exposure"].get<float>(),
                  1.25f);
}

TEST_F(EditorAdjustmentPipelineTest, RejectsUnknownFieldWithoutMutatingKnownOperators) {
  CPUPipelineExecutor executor;
  executor.ResetToCleanBaselineAdjustments();
  const auto before = executor.GetStage(PipelineStageName::Basic_Adjustment)
                          .GetOperator(OperatorType::EXPOSURE)
                          .value()
                          ->ExportOperatorParams();

  EditorRenderAdjustmentSnapshot snapshot;
  snapshot.patches = {EditorAdjustmentPatch{"not_a_field", R"({"value":1})", false}};

  std::string error;
  EXPECT_FALSE(ApplyEditorAdjustmentSnapshot(executor, snapshot, &error));
  EXPECT_NE(error.find("not_a_field"), std::string::npos);
  EXPECT_EQ(executor.GetStage(PipelineStageName::Basic_Adjustment)
                .GetOperator(OperatorType::EXPOSURE)
                .value()
                ->ExportOperatorParams(),
            before);
}

}  // namespace
}  // namespace alcedo
