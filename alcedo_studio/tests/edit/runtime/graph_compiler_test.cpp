//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/graph_compiler.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/raster_mask_node_model.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/runtime/pass_kind.hpp"

namespace alcedo {
namespace {

TEST(GpuDagGraphCompiler, GraphCompilerKeepsDecodeOutsidePlanAndAddsCameraToAp1BeforeGrade) {
  const auto pattern  = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto       document = CreateDefaultPipelineDocument();
  const auto plan     = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});

  for (const auto& pass : plan.passes) {
    const std::string name = GpuPassKindName(pass.kind);
    EXPECT_EQ(name.find("LibRaw"), std::string::npos);
    EXPECT_EQ(name.find("DecodeRes"), std::string::npos);
    EXPECT_NE(pass.kind, static_cast<GpuPassKind>(255));
  }
  EXPECT_LT(plan.IndexOf(GpuPassKind::GeometryResample), plan.IndexOf(GpuPassKind::CameraToAp1));
  EXPECT_LT(plan.IndexOf(GpuPassKind::CameraToAp1), plan.IndexOf(GpuPassKind::PrimaryColorGrade));
  EXPECT_FALSE(plan.Contains(static_cast<GpuPassKind>(99)));
}

TEST(GpuDagGraphCompiler, GraphCompilerPlacesHighlightRecoverAfterDemosaic) {
  const auto pattern  = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto       document = CreateDefaultPipelineDocument();
  const auto plan     = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});

  const int  demosaic = plan.IndexOf(GpuPassKind::Demosaic);
  const int  hlr      = plan.IndexOf(GpuPassKind::HighlightRecover);
  ASSERT_GE(demosaic, 0);
  ASSERT_GE(hlr, 0);
  EXPECT_LT(demosaic, hlr);
  EXPECT_LT(plan.IndexOf(GpuPassKind::Linearize), demosaic);
  EXPECT_TRUE(plan.Contains(GpuPassKind::GeometryResample));
}

TEST(GpuDagGraphCompiler, HighlightFlagDoesNotChangePassList) {
  const auto pattern  = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto document = CreateDefaultPipelineDocument();
  document.Develop()->Params().ReplaceParams([] {
    DevelopPayload p;
    p.highlights_reconstruct = false;
    return p;
  }());
  const auto plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  EXPECT_TRUE(plan.Contains(GpuPassKind::HighlightRecover));
  EXPECT_TRUE(plan.Contains(GpuPassKind::CfaClamp));
}

TEST(GpuDagGraphCompiler, DefaultPipelineCompilesShadowsAndHighlightsToLocalLaplacianOnly) {
  const auto pattern  = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  const auto document = CreateDefaultPipelineDocument();
  const auto plan     = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});

  bool       saw_shadows    = false;
  bool       saw_highlights = false;
  bool       saw_curve      = false;
  for (const auto& adjustment : plan.primary_grade_adjustments) {
    if (adjustment.type == type_ids::Shadows()) {
      saw_shadows = true;
      EXPECT_EQ(adjustment.algorithm, CompiledAdjustmentAlgorithm::LocalLaplacian);
    } else if (adjustment.type == type_ids::Highlights()) {
      saw_highlights = true;
      EXPECT_EQ(adjustment.algorithm, CompiledAdjustmentAlgorithm::LocalLaplacian);
    } else if (adjustment.type == type_ids::Curve()) {
      saw_curve = true;
      EXPECT_EQ(adjustment.algorithm, CompiledAdjustmentAlgorithm::Pointwise);
    }
  }
  EXPECT_TRUE(saw_shadows);
  EXPECT_TRUE(saw_highlights);
  EXPECT_TRUE(saw_curve);
}

TEST(GpuDagGraphCompiler, GraphCompilerEmitsMaskEvaluateWhenRasterMaskConnected) {
  const auto pattern  = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto document = CreateDefaultPipelineDocument();
  auto node     = std::make_unique<RasterMaskNodeModel>(NodeId{"mask.raster"});
  node->SetAssetKey(MaskAssetKey{"test.raster"});
  document.Graph().AddNode(std::move(node));
  document.Graph().Connect(NodeId{"mask.raster"}, PortId{"mask"}, NodeId{"grade.primary"},
                           PortId{"mask"});
  const auto plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});

  EXPECT_TRUE(plan.Contains(GpuPassKind::MaskEvaluate));
  EXPECT_TRUE(plan.Contains(GpuPassKind::MaskFeather));
  ASSERT_TRUE(plan.primary_grade_mask.has_value());
  EXPECT_EQ(plan.primary_grade_mask->node_id, NodeId{"mask.raster"});
  EXPECT_EQ(plan.primary_grade_mask->kind, CompiledMaskKind::Raster);
  EXPECT_LT(plan.IndexOf(GpuPassKind::CameraToAp1), plan.IndexOf(GpuPassKind::MaskEvaluate));
  EXPECT_LT(plan.IndexOf(GpuPassKind::MaskEvaluate), plan.IndexOf(GpuPassKind::MaskFeather));
  EXPECT_LT(plan.IndexOf(GpuPassKind::MaskFeather), plan.IndexOf(GpuPassKind::PrimaryColorGrade));
}

}  // namespace
}  // namespace alcedo
