//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include "edit/graph/adjustment_ownership.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/models/adjustment_catalog.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/runtime/develop_compile_source.hpp"
#include "edit/runtime/graph_compiler.hpp"

namespace alcedo {
namespace {

auto DirectRgbSource() -> DevelopCompileSource {
  DevelopCompileSource source;
  source.kind                   = DevelopInputKind::DirectRgb;
  source.host_extent            = {16, 12};
  source.develop_output_extent  = {16, 12};
  source.full_reference_extent  = {16, 12};
  return source;
}

}  // namespace

TEST(GpuDagModelGraph, PostAdjustmentsRejectGradeOwnership) {
  auto document = CreateDefaultPipelineDocument();
  auto* grade   = document.PrimaryGrade();
  auto* drt     = document.Drt();
  ASSERT_NE(grade, nullptr);
  ASSERT_NE(drt, nullptr);
  EXPECT_EQ(grade->FindAdjustmentByType(type_ids::Clarity()), nullptr);
  EXPECT_NE(drt->FindAdjustmentByType(type_ids::Clarity()), nullptr);

  const auto before = document.ToJson();
  auto       clarity =
      BuiltinAdjustmentCatalog::Instance().CreateDefault(type_ids::Clarity());
  ASSERT_NE(clarity, nullptr);
  EXPECT_THROW(grade->InsertAdjustment(grade->AdjustmentCount(),
                                       AdjustmentInstanceId{"grade.primary.clarity"},
                                       std::move(clarity)),
               std::runtime_error);
  EXPECT_EQ(document.ToJson(), before);

  auto exposure =
      BuiltinAdjustmentCatalog::Instance().CreateDefault(type_ids::Exposure());
  ASSERT_NE(exposure, nullptr);
  EXPECT_THROW(drt->InsertAdjustment(drt->AdjustmentCount(), AdjustmentInstanceId{"drt.exposure"},
                                     std::move(exposure)),
               std::runtime_error);
  EXPECT_EQ(document.ToJson(), before);

  auto grade_json = grade->ToJson();
  auto default_clarity =
      BuiltinAdjustmentCatalog::Instance().CreateDefault(type_ids::Clarity());
  grade_json["adjustments"].push_back(
      {{"id", "grade.primary.clarity"},
       {"type", std::string{type_ids::Clarity().Text()}},
       {"params", default_clarity->ToJson()}});
  EXPECT_THROW(ColorGradeNodeModel::FromJson(grade_json), std::runtime_error);

  auto document_json = document.ToJson();
  EXPECT_NO_THROW(PipelineDocument::FromJson(document_json));
  for (auto& node : document_json["nodes"]) {
    if (node.at("id") == "grade.primary") {
      node["adjustments"] = grade_json["adjustments"];
    }
  }
  EXPECT_THROW(PipelineDocument::FromJson(document_json), std::runtime_error);

  auto missing_post = document.ToJson();
  for (auto& node : missing_post["nodes"]) {
    if (node.at("id") == "drt") {
      node.erase("adjustments");
    }
  }
  EXPECT_THROW(PipelineDocument::FromJson(missing_post), std::runtime_error);

  auto extra_exposure = document.ToJson();
  auto default_exposure =
      BuiltinAdjustmentCatalog::Instance().CreateDefault(type_ids::Exposure());
  for (auto& node : extra_exposure["nodes"]) {
    if (node.at("id") == "drt") {
      node["adjustments"].push_back({{"id", "drt.exposure"},
                                     {"type", std::string{type_ids::Exposure().Text()}},
                                     {"params", default_exposure->ToJson()}});
    }
  }
  EXPECT_THROW(PipelineDocument::FromJson(extra_exposure), std::runtime_error);

  auto incomplete = CreateDefaultPipelineDocument();
  incomplete.Graph().RemoveNode(NodeId{"drt"});
  incomplete.Graph().AddNode(std::make_unique<DrtNodeModel>(NodeId{"drt"}));
  incomplete.Graph().Connect(NodeId{"grade.primary"}, PortId{"image"}, NodeId{"drt"},
                             PortId{"image"});
  EXPECT_THROW((void)incomplete.Drt()->ToJson(), std::runtime_error);
  EXPECT_THROW((void)GraphCompiler::CompileStatic(incomplete, DirectRgbSource()),
               std::runtime_error);

  EXPECT_NO_THROW((void)GraphCompiler::CompileStatic(document, DirectRgbSource()));
  const auto plan = GraphCompiler::CompileStatic(document, DirectRgbSource());
  ASSERT_EQ(plan.drt.post_adjustments.size(), 4u);
  EXPECT_EQ(plan.drt.post_adjustments[0].type, type_ids::Clarity());
}

}  // namespace alcedo
