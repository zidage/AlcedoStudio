//  Copyright 2025-2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include "edit/operators/op_base.hpp"
#include "edit/pipeline/pipeline_stage.hpp"
#include "pipeline_test_fixation.hpp"

using namespace alcedo;

TEST_F(PipelineTests, SerialDummyTest) {
  // This is just a dummy test to ensure the test fixture is working
  ASSERT_TRUE(true);
}

TEST_F(PipelineTests, PipelineStageExportTest_Simple) {
  PipelineStage stage{PipelineStageName::Image_Loading, true, false};

  stage.SetOperator(OperatorType::EXPOSURE, {{"exposure", 1.5f}});
  EXPECT_NO_THROW({
    nlohmann::json exported = stage.ExportStageParams();
    std::cout << exported.dump(2) << std::endl;
  });
}

TEST_F(PipelineTests, PipelineStageExportTest_MultipleOps) {
  PipelineStage stage{PipelineStageName::Basic_Adjustment, true, false};

  stage.SetOperator(OperatorType::CONTRAST, {{"contrast", 12.f}});
  stage.SetOperator(OperatorType::SATURATION, {{"saturation", 80.f}});
  stage.SetOperator(OperatorType::HIGHLIGHTS, {{"highlights", -10.0f}});

  EXPECT_NO_THROW({
    nlohmann::json exported = stage.ExportStageParams();
    std::cout << exported.dump(2) << std::endl;
    EXPECT_TRUE(exported.contains("Basic Adjustment"));
    EXPECT_TRUE(exported["Basic Adjustment"].contains("contrast"));
    EXPECT_TRUE(exported["Basic Adjustment"].contains("saturation"));
    EXPECT_TRUE(exported["Basic Adjustment"].contains("highlights"));
  });
}

TEST_F(PipelineTests, PipelineStageExportTest_EmptyStage) {
  PipelineStage stage{PipelineStageName::Color_Adjustment, true, false};

  EXPECT_NO_THROW({
    nlohmann::json exported = stage.ExportStageParams();
    std::cout << exported.dump(2) << std::endl;
    EXPECT_TRUE(exported.contains("Color Adjustment"));
    EXPECT_TRUE(exported["Color Adjustment"].is_null());
  });
}

TEST_F(PipelineTests, PipelineStageImportTest) {
  PipelineStage  stage{PipelineStageName::Basic_Adjustment, true, true};
  nlohmann::json import_json;
  import_json["Basic Adjustment"]["exposure"] = {
      {"enabled", true}, {"params", {{"exposure", 2.0f}}}, {"type", OperatorType::EXPOSURE}};
  EXPECT_NO_THROW(stage.ImportStageParams(import_json));

  // After import, verify the operator is set correctly
  nlohmann::json exported_after_import = stage.ExportStageParams();
  auto           expected_op           = stage.GetOperator(OperatorType::EXPOSURE);
  nlohmann::json op_param_json         = expected_op.value()->op_->GetParams();
  EXPECT_EQ(op_param_json["exposure"], 2.0f);
  ASSERT_TRUE(expected_op.has_value());
  std::cout << exported_after_import.dump(2) << std::endl;
}