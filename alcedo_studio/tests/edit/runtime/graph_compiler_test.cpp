//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/graph_compiler.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../graph/grade_owned_mask_support.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/operators/models/adjustment_catalog.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/runtime/metal/metal_backend.hpp"
#include "edit/runtime/pass_kind.hpp"
#include "edit/runtime/result_content_key.hpp"
#include "multi_grade_runtime_test_support.hpp"

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
  ASSERT_NE(plan.FirstGrade(), nullptr);
  EXPECT_EQ(plan.FirstGrade()->scene_input, plan.develop_output);
  EXPECT_EQ(plan.SceneInputForDrt(), plan.FirstGrade()->scene_output);
  const auto grades = plan.PassesOfKind(GpuPassKind::PrimaryColorGrade);
  ASSERT_EQ(grades.size(), 1U);
  ASSERT_FALSE(grades.front()->inputs.empty());
  ASSERT_FALSE(grades.front()->outputs.empty());
  EXPECT_EQ(grades.front()->inputs.front().source, plan.develop_output);
  EXPECT_EQ(grades.front()->outputs.front().value, plan.FirstGrade()->scene_output);
  EXPECT_EQ(grades.front()->instance.owner, plan.FirstGrade()->node_id);
  EXPECT_EQ(grades.front()->instance.kind, GpuPassKind::PrimaryColorGrade);
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

  const auto* grade_plan = plan.FirstGrade();
  ASSERT_NE(grade_plan, nullptr);
  bool       saw_shadows    = false;
  bool       saw_highlights = false;
  bool       saw_curve      = false;
  bool       saw_lmt        = false;
  for (const auto& adjustment : grade_plan->adjustments) {
    if (adjustment.type == type_ids::Shadows()) {
      saw_shadows = true;
      EXPECT_EQ(adjustment.algorithm, CompiledAdjustmentAlgorithm::LocalLaplacian);
    } else if (adjustment.type == type_ids::Highlights()) {
      saw_highlights = true;
      EXPECT_EQ(adjustment.algorithm, CompiledAdjustmentAlgorithm::LocalLaplacian);
    } else if (adjustment.type == type_ids::Curve()) {
      saw_curve = true;
      EXPECT_EQ(adjustment.algorithm, CompiledAdjustmentAlgorithm::Pointwise);
    } else if (adjustment.type == type_ids::Lmt()) {
      saw_lmt = true;
      EXPECT_EQ(adjustment.algorithm, CompiledAdjustmentAlgorithm::Pointwise);
    } else {
      EXPECT_NE(adjustment.algorithm, CompiledAdjustmentAlgorithm::Neighborhood);
    }
  }
  EXPECT_TRUE(saw_shadows);
  EXPECT_TRUE(saw_highlights);
  EXPECT_TRUE(saw_curve);
  EXPECT_TRUE(saw_lmt);
  ASSERT_EQ(grade_plan->stages.size(), 3U);
  EXPECT_EQ(grade_plan->stages.front().kind, CompiledGradeStageKind::Pointwise);
  bool saw_llf_stage = false;
  for (const auto& stage : grade_plan->stages) {
    EXPECT_NE(stage.kind, CompiledGradeStageKind::Neighborhood);
    if (stage.kind == CompiledGradeStageKind::LocalLaplacian) {
      saw_llf_stage = true;
      EXPECT_EQ(stage.count, 2U);
    }
  }
  EXPECT_TRUE(saw_llf_stage);
  ASSERT_EQ(plan.drt.post_adjustments.size(), 4U);
  EXPECT_EQ(plan.drt.post_adjustments[0].type, type_ids::Clarity());
  EXPECT_EQ(plan.drt.post_adjustments[1].type, type_ids::Sharpen());
  EXPECT_EQ(plan.drt.post_adjustments[2].type, type_ids::Halation());
  EXPECT_EQ(plan.drt.post_adjustments[3].type, type_ids::FilmGrain());
  for (const auto& adjustment : plan.drt.post_adjustments) {
    EXPECT_EQ(adjustment.algorithm, CompiledAdjustmentAlgorithm::Neighborhood);
  }
}

TEST(GpuDagGraphCompiler, GraphCompilerEmitsMaskEvaluateWhenRasterMaskConnected) {
  const auto pattern  = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto document = CreateDefaultPipelineDocument();
  grade_mask_test::AddBrushMask(document, MaskId{"mask.raster"}, MaskAssetKey{"test.raster"});
  const auto plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});

  EXPECT_TRUE(plan.Contains(GpuPassKind::MaskEvaluate));
  EXPECT_TRUE(plan.Contains(GpuPassKind::MaskUnion));
  EXPECT_FALSE(plan.Contains(GpuPassKind::MaskFeather));
  ASSERT_NE(plan.FirstGrade(), nullptr);
  ASSERT_TRUE(plan.FirstGrade()->mask_stack.has_value());
  EXPECT_EQ(plan.FirstGrade()->mask_stack->owner_node_id, NodeId{"grade.primary"});
  ASSERT_EQ(plan.FirstGrade()->mask_stack->sources.size(), 1U);
  EXPECT_EQ(plan.FirstGrade()->mask_stack->sources.front().mask_id, MaskId{"mask.raster"});
  EXPECT_EQ(plan.FirstGrade()->mask_stack->sources.front().source_kind, MaskSourceKind::Brush);
  EXPECT_EQ(plan.FirstGrade()->mask_stack->sources.front().range_input,
            plan.FirstGrade()->scene_input);
  EXPECT_EQ(plan.FirstGrade()->mask_output, plan.FirstGrade()->mask_stack->union_output);
  EXPECT_LT(plan.IndexOf(GpuPassKind::CameraToAp1), plan.IndexOf(GpuPassKind::MaskEvaluate));
  EXPECT_LT(plan.IndexOf(GpuPassKind::MaskEvaluate), plan.IndexOf(GpuPassKind::MaskUnion));
  EXPECT_LT(plan.IndexOf(GpuPassKind::MaskUnion), plan.IndexOf(GpuPassKind::PrimaryColorGrade));
}

auto Align256(std::size_t value) -> std::size_t { return (value + 255) & ~std::size_t{255}; }

auto PlaneBytes(std::size_t pixels, std::size_t bytes_per_pixel) -> std::size_t {
  return Align256(pixels * bytes_per_pixel);
}

auto ExclusiveBayerDevelopBytes(std::size_t pixels) -> std::size_t {
  return PlaneBytes(pixels, 2) + PlaneBytes(pixels, 4) + 5 * PlaneBytes(pixels, 4) +
         PlaneBytes(pixels, 12) + Align256(4 + 16 + 16) + 64 * 256;
}

auto ExclusiveXTransDevelopBytes(std::size_t pixels) -> std::size_t {
  return PlaneBytes(pixels, 2) + PlaneBytes(pixels, 4) + PlaneBytes(pixels, 4) +
         PlaneBytes(pixels, 12) + PlaneBytes(pixels, 12) + Align256(4 + 16 + 16) + 64 * 256;
}

auto SummedExclusiveDemosaicBytes(std::size_t pixels) -> std::size_t {
  return PlaneBytes(pixels, 2) + PlaneBytes(pixels, 4) + 5 * PlaneBytes(pixels, 4) +
         PlaneBytes(pixels, 12) + PlaneBytes(pixels, 12) + Align256(4 + 16 + 16) +
         PlaneBytes(pixels, 4) + PlaneBytes(pixels, 12) + 64 * 256;
}

TEST(GpuDagGraphCompiler, PeakTransientUsesOneDemosaicPathInsteadOfSummingBayerAndXTrans) {
  const auto pattern  = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(256, 256, pattern), pattern,
      gpu_dag_test::DefaultLinearization(), gpu_dag_test::FullSensor(256, 256), DecodeRes::FULL);
  auto       document = CreateDefaultPipelineDocument();
  const auto plan     = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});

  constexpr std::size_t kPixels = 256U * 256U;
  const auto            bayer   = ExclusiveBayerDevelopBytes(kPixels);
  const auto            summed  = SummedExclusiveDemosaicBytes(kPixels);
  EXPECT_EQ(prepared.CompileSource().kind, DevelopInputKind::BayerCfa);
  EXPECT_EQ(plan.peak_transient_bytes, bayer);
  EXPECT_LT(plan.peak_transient_bytes, summed);
  EXPECT_GT(summed - bayer, PlaneBytes(kPixels, 12));
}

TEST(GpuDagGraphCompiler, PeakTransientForXTransDoesNotReserveBayerRcdPlanes) {
  const auto pattern  = gpu_dag_test::MakeXTransPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(256, 256, pattern), pattern,
      gpu_dag_test::DefaultLinearization(), gpu_dag_test::FullSensor(256, 256), DecodeRes::FULL);
  auto       document = CreateDefaultPipelineDocument();
  const auto plan     = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});

  constexpr std::size_t kPixels = 256U * 256U;
  EXPECT_EQ(prepared.CompileSource().kind, DevelopInputKind::XTransCfa);
  EXPECT_EQ(plan.peak_transient_bytes, ExclusiveXTransDevelopBytes(kPixels));
  EXPECT_LT(plan.peak_transient_bytes, ExclusiveBayerDevelopBytes(kPixels));
}

TEST(GpuDagGraphCompiler, RasterMaskDoesNotAddMaskSdfOnTopOfDevelopTransientPeak) {
  const auto pattern  = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto document = CreateDefaultPipelineDocument();
  const auto without_mask =
      GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  grade_mask_test::AddBrushMask(document, MaskId{"mask.raster"}, MaskAssetKey{"test.raster"});
  const auto with_mask =
      GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});

  EXPECT_TRUE(with_mask.Contains(GpuPassKind::MaskEvaluate));
  EXPECT_EQ(with_mask.peak_transient_bytes, without_mask.peak_transient_bytes);
}

TEST(GpuDagGraphCompiler, GraphCompilerPassListIsBackendNativeTypeFree) {
  static_assert(std::is_same_v<decltype(GpuPassDesc{}.kind), GpuPassKind>);

  const char* files[] = {"graph_compiler.hpp", "execution_plan.hpp", "pass_kind.hpp"};
  const std::filesystem::path root{ALCEDO_RUNTIME_HEADER_ROOT};
  for (const char* name : files) {
    std::ifstream input(root / name);
    ASSERT_TRUE(input) << name;
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    EXPECT_EQ(text.find("cuda_runtime"), std::string::npos) << name;
    EXPECT_EQ(text.find("cuda.h"), std::string::npos) << name;
    EXPECT_EQ(text.find("Metal/"), std::string::npos) << name;
    EXPECT_EQ(text.find("metal.h"), std::string::npos) << name;
    EXPECT_EQ(text.find("OpenCL"), std::string::npos) << name;
  }

  const auto pattern  = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto       document = CreateDefaultPipelineDocument();
  const auto cuda_plan =
      GraphCompiler::CompileStatic(document, prepared.CompileSource(), /*cuda*/ 1U);
  const auto metal_plan = GraphCompiler::CompileStatic(document, prepared.CompileSource(),
                                                       kMetalDagBackendCapabilityVersion);

  ASSERT_EQ(cuda_plan.passes.size(), metal_plan.passes.size());
  ASSERT_FALSE(cuda_plan.passes.empty());
  for (std::size_t i = 0; i < cuda_plan.passes.size(); ++i) {
    EXPECT_EQ(cuda_plan.passes[i].kind, metal_plan.passes[i].kind);
  }
  EXPECT_EQ(cuda_plan.static_key.backend_capability_version, 1U);
  EXPECT_EQ(metal_plan.static_key.backend_capability_version, kMetalDagBackendCapabilityVersion);
  EXPECT_NE(cuda_plan.static_key, metal_plan.static_key);
  EXPECT_NE(kMetalDagBackendCapabilityVersion, 1U);
  EXPECT_EQ(cuda_plan.sensor_linear_output, metal_plan.sensor_linear_output);
  EXPECT_EQ(cuda_plan.geometry_output, metal_plan.geometry_output);
  EXPECT_EQ(cuda_plan.develop_output, metal_plan.develop_output);
}

auto DirectRgbSource() -> DevelopCompileSource {
  DevelopCompileSource source;
  source.kind                  = DevelopInputKind::DirectRgb;
  source.host_extent           = Extent2D{8, 8};
  source.develop_output_extent = Extent2D{8, 8};
  source.full_reference_extent = Extent2D{8, 8};
  return source;
}

TEST(GpuDagGraphCompiler, ZeroGradesFeedDevelopIntoDrtPost) {
  auto document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(RemoveColorGradeAndBridge(document, NodeId{"grade.primary"}).empty());
  const auto plan = GraphCompiler::CompileStatic(document, DirectRgbSource());

  EXPECT_TRUE(plan.grade_nodes.empty());
  EXPECT_FALSE(plan.Contains(GpuPassKind::PrimaryColorGrade));
  EXPECT_TRUE(plan.Contains(GpuPassKind::Drt));
  EXPECT_EQ(plan.SceneInputForDrt(), plan.develop_output);
  EXPECT_EQ(plan.drt.scene_input, plan.develop_output);
  ASSERT_FALSE(plan.drt.steps.empty());
  EXPECT_EQ(plan.drt.steps.front().kind, CompiledDrtStepKind::Neighborhood);
  EXPECT_EQ(plan.drt.steps.front().input, plan.develop_output);
  EXPECT_EQ(plan.drt.steps.back().kind, CompiledDrtStepKind::DisplayTransform);
  EXPECT_EQ(plan.drt.steps.back().input, plan.drt.scene_output);
  EXPECT_EQ(plan.drt.steps.back().output, plan.display_output);
}

TEST(GpuDagGraphCompiler, GradeWithoutPrimaryIdRendersItsParameters) {
  auto document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.b"}).empty());
  ASSERT_TRUE(RemoveColorGradeAndBridge(document, NodeId{"grade.primary"}).empty());
  ASSERT_EQ(document.PrimaryGrade()->Id(), NodeId{"grade.b"});
  const auto* remaining =
      dynamic_cast<const ColorGradeNodeModel*>(document.Graph().FindNode("grade.b"));
  ASSERT_NE(remaining, nullptr);

  const auto plan = GraphCompiler::CompileStatic(document, DirectRgbSource());
  ASSERT_EQ(plan.grade_nodes.size(), 1U);
  EXPECT_EQ(plan.grade_nodes.front().node_id, NodeId{"grade.b"});
  EXPECT_EQ(plan.FindGrade(NodeId{"grade.b"}), &plan.grade_nodes.front());
  EXPECT_EQ(plan.FindGrade(NodeId{"grade.primary"}), nullptr);
  ASSERT_EQ(plan.grade_nodes.front().adjustments.size(), remaining->AdjustmentCount());
  for (std::size_t i = 0; i < remaining->AdjustmentCount(); ++i) {
    EXPECT_EQ(plan.grade_nodes.front().adjustments[i].instance_id, remaining->AdjustmentIdAt(i));
    EXPECT_EQ(plan.grade_nodes.front().adjustments[i].type, remaining->AdjustmentAt(i).Type());
  }
  const auto grades = plan.PassesOfKind(GpuPassKind::PrimaryColorGrade);
  ASSERT_EQ(grades.size(), 1U);
  EXPECT_EQ(grades.front()->owner, NodeId{"grade.b"});
  EXPECT_EQ(grades.front()->instance.ordinal, 0U);
  EXPECT_NE(plan.FindPass(grades.front()->instance), nullptr);
}

TEST(GpuDagGraphCompiler, ThreeGradesComposeInEdgeOrder) {
  auto document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.b"}).empty());
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.c"}).empty());

  const auto plan = GraphCompiler::CompileStatic(document, DirectRgbSource());
  ASSERT_EQ(plan.grade_nodes.size(), 3U);
  EXPECT_EQ(plan.grade_nodes[0].node_id, NodeId{"grade.primary"});
  EXPECT_EQ(plan.grade_nodes[1].node_id, NodeId{"grade.b"});
  EXPECT_EQ(plan.grade_nodes[2].node_id, NodeId{"grade.c"});
  EXPECT_EQ(plan.grade_nodes[0].scene_input, plan.develop_output);
  EXPECT_EQ(plan.grade_nodes[1].scene_input, plan.grade_nodes[0].scene_output);
  EXPECT_EQ(plan.grade_nodes[2].scene_input, plan.grade_nodes[1].scene_output);
  EXPECT_EQ(plan.drt.scene_input, plan.grade_nodes[2].scene_output);
  EXPECT_EQ(plan.SceneInputForDrt(), plan.grade_nodes[2].scene_output);

  const auto grades = plan.PassesOfKind(GpuPassKind::PrimaryColorGrade);
  ASSERT_EQ(grades.size(), 3U);
  EXPECT_EQ(grades[0]->owner, NodeId{"grade.primary"});
  EXPECT_EQ(grades[1]->owner, NodeId{"grade.b"});
  EXPECT_EQ(grades[2]->owner, NodeId{"grade.c"});
  EXPECT_EQ(grades[0]->instance.ordinal, 0U);
  EXPECT_EQ(grades[1]->instance.ordinal, 1U);
  EXPECT_EQ(grades[2]->instance.ordinal, 2U);
  EXPECT_NE(grades[0]->instance, grades[1]->instance);
  EXPECT_EQ(grades[0]->outputs.front().value, plan.grade_nodes[0].scene_output);
  EXPECT_EQ(grades[1]->outputs.front().value, plan.grade_nodes[1].scene_output);
  EXPECT_EQ(grades[2]->inputs.front().source, plan.grade_nodes[1].scene_output);
}

TEST(GpuDagGraphCompiler, GradeInputsFollowBackboneEdgesNotContainerOrder) {
  auto document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.z"}).empty());
  ASSERT_TRUE(ReconnectColorGrade(document, NodeId{"grade.z"}, NodeId{"develop"},
                                  NodeId{"grade.primary"})
                  .empty());

  std::vector<NodeId> container_grades;
  for (const auto& node : document.Graph().Nodes()) {
    if (node->Type() == type_ids::ColorGradeNode()) {
      container_grades.push_back(node->Id());
    }
  }
  ASSERT_EQ(container_grades.size(), 2U);
  EXPECT_EQ(container_grades.front(), NodeId{"grade.primary"});
  EXPECT_EQ(container_grades.back(), NodeId{"grade.z"});

  const auto backbone = document.Graph().ImageBackboneNodeIds();
  ASSERT_EQ(backbone.size(), 4U);
  EXPECT_EQ(backbone[1], NodeId{"grade.z"});
  EXPECT_EQ(backbone[2], NodeId{"grade.primary"});

  const auto plan = GraphCompiler::CompileStatic(document, DirectRgbSource());
  ASSERT_EQ(plan.grade_nodes.size(), 2U);
  EXPECT_EQ(plan.grade_nodes[0].node_id, NodeId{"grade.z"});
  EXPECT_EQ(plan.grade_nodes[1].node_id, NodeId{"grade.primary"});
  EXPECT_EQ(plan.grade_nodes[0].scene_input, plan.develop_output);
  EXPECT_EQ(plan.grade_nodes[1].scene_input, plan.grade_nodes[0].scene_output);
}

TEST(GpuDagGraphCompiler, RepeatedAdjustmentInstancesKeepTheirOrder) {
  auto document = CreateDefaultPipelineDocument();
  auto* grade   = document.PrimaryGrade();
  ASSERT_NE(grade, nullptr);
  auto extra = BuiltinAdjustmentCatalog::Instance().CreateDefault(type_ids::Exposure());
  ASSERT_NE(extra, nullptr);
  document.InsertAdjustment(grade->Id(), grade->AdjustmentCount(),
                            AdjustmentInstanceId{"grade.primary.exposure.2"}, std::move(extra));

  const auto plan = GraphCompiler::CompileStatic(document, DirectRgbSource());
  const auto* compiled = plan.FirstGrade();
  ASSERT_NE(compiled, nullptr);
  ASSERT_GE(compiled->adjustments.size(), 2U);
  std::vector<AdjustmentInstanceId> exposures;
  for (const auto& adjustment : compiled->adjustments) {
    if (adjustment.type == type_ids::Exposure()) {
      exposures.push_back(adjustment.instance_id);
    }
  }
  ASSERT_EQ(exposures.size(), 2U);
  std::vector<AdjustmentInstanceId> document_exposures;
  for (std::size_t i = 0; i < document.PrimaryGrade()->AdjustmentCount(); ++i) {
    if (document.PrimaryGrade()->AdjustmentAt(i).Type() == type_ids::Exposure()) {
      document_exposures.push_back(document.PrimaryGrade()->AdjustmentIdAt(i));
    }
  }
  ASSERT_EQ(document_exposures.size(), 2U);
  EXPECT_EQ(exposures, document_exposures);
  EXPECT_EQ(exposures.back(), AdjustmentInstanceId{"grade.primary.exposure.2"});
  EXPECT_NE(exposures.front(), exposures.back());
}

TEST(GpuDagGraphCompiler, GraphCompilerRejectsSceneImageBranch) {
  auto document = CreateDefaultPipelineDocument();
  document.Graph().AddNode(ColorGradeNodeModel::MakeDefault(NodeId{"grade.b"}));
  document.Graph().Connect(NodeId{"grade.primary"}, PortId{"image"}, NodeId{"grade.b"},
                           PortId{"image"});
  EXPECT_THROW((void)GraphCompiler::CompileStatic(document, DirectRgbSource()), std::runtime_error);
}

TEST(GpuDagGraphCompiler, InvalidCompiledBindingsFailBeforeGpuWork) {
  auto document = CreateDefaultPipelineDocument();
  auto plan     = GraphCompiler::CompileStatic(document, DirectRgbSource());
  ASSERT_FALSE(plan.passes.empty());
  auto broken = plan;
  ASSERT_FALSE(broken.passes.back().inputs.empty());
  broken.passes.back().inputs.front().source = GraphValueId{NodeId{"missing"}, PortId{"image"}};
  EXPECT_THROW(ValidateExecutionPlan(broken), std::runtime_error);

  auto duplicate = plan;
  ASSERT_GE(duplicate.passes.size(), 2U);
  duplicate.passes.back().outputs.push_back(duplicate.passes.front().outputs.front());
  EXPECT_THROW(ValidateExecutionPlan(duplicate), std::runtime_error);

  auto wrong_kind = plan;
  ASSERT_FALSE(wrong_kind.passes.back().inputs.empty());
  wrong_kind.passes.back().inputs.front().expected_kind = CompiledValueKind::Mask;
  EXPECT_THROW(ValidateExecutionPlan(wrong_kind), std::runtime_error);
}

auto MakeRgbPrepared() -> PreparedRawInput {
  return RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(16, 12),
                                       gpu_dag_test::FullSensor(16, 12));
}

void AddSortedRadialPair(PipelineDocument& document) {
  grade_mask_test::AddRadialMask(document, MaskId{"mask.z"});
  grade_mask_test::AddRadialMask(document, MaskId{"mask.a"});
}

TEST(GpuDagGraphCompiler, EmptyMaskListUsesFullGradeCoverage) {
  const auto prepared = MakeRgbPrepared();
  auto       document = CreateDefaultPipelineDocument();
  const auto empty_plan =
      GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  EXPECT_FALSE(empty_plan.Contains(GpuPassKind::MaskEvaluate));
  EXPECT_FALSE(empty_plan.Contains(GpuPassKind::MaskUnion));
  ASSERT_NE(empty_plan.FirstGrade(), nullptr);
  EXPECT_FALSE(empty_plan.FirstGrade()->mask_stack.has_value());
  const auto empty_keys = BuildFrameResultContentKeys(empty_plan, prepared, document);
  EXPECT_TRUE(empty_keys.mask.Empty());

  grade_mask_test::AddRadialMask(document, MaskId{"mask.radial"});
  document.PrimaryGrade()->RemoveMask(MaskId{"mask.radial"});
  const auto restored =
      GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  EXPECT_EQ(restored.static_key, empty_plan.static_key);
  const auto restored_keys = BuildFrameResultContentKeys(restored, prepared, document);
  EXPECT_EQ(restored_keys.primary_grade, empty_keys.primary_grade);
  EXPECT_TRUE(restored_keys.mask.Empty());
}

TEST(GpuDagGraphCompiler, AllDisabledMasksUseZeroGradeCoverage) {
  const auto prepared = MakeRgbPrepared();
  auto       document = CreateDefaultPipelineDocument();
  AddSortedRadialPair(document);
  const auto enabled_plan =
      GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  ASSERT_TRUE(enabled_plan.Contains(GpuPassKind::MaskEvaluate));
  ASSERT_TRUE(enabled_plan.Contains(GpuPassKind::MaskUnion));
  const auto enabled_keys = BuildFrameResultContentKeys(enabled_plan, prepared, document);

  document.PrimaryGrade()->SetMaskEnabled(MaskId{"mask.a"}, false);
  document.PrimaryGrade()->SetMaskEnabled(MaskId{"mask.z"}, false);
  EXPECT_FALSE(GraphCompiler::NeedsRecompile(enabled_plan, document, prepared.CompileSource()));
  const auto disabled_plan =
      GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  EXPECT_EQ(disabled_plan.static_key, enabled_plan.static_key);
  ASSERT_TRUE(disabled_plan.FirstGrade()->mask_stack.has_value());
  EXPECT_EQ(disabled_plan.FirstGrade()->mask_stack->sources.size(), 2U);
  const auto disabled_keys = BuildFrameResultContentKeys(disabled_plan, prepared, document);
  EXPECT_EQ(disabled_keys.Value(disabled_plan.FirstGrade()->mask_output),
            AllDisabledMaskUnionKey());
  EXPECT_FALSE(AllDisabledMaskUnionKey().Empty());
  EXPECT_NE(disabled_keys.primary_grade, enabled_keys.primary_grade);
}

TEST(GpuDagGraphCompiler, MaskDisplayReorderKeepsStaticAndPixelKeys) {
  const auto prepared = MakeRgbPrepared();
  auto       document = CreateDefaultPipelineDocument();
  AddSortedRadialPair(document);
  EXPECT_EQ(document.PrimaryGrade()->MaskAt(0).id, MaskId{"mask.z"});
  EXPECT_EQ(document.PrimaryGrade()->MaskAt(1).id, MaskId{"mask.a"});
  const auto first = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  const auto first_keys = BuildFrameResultContentKeys(first, prepared, document);
  ASSERT_TRUE(first.FirstGrade()->mask_stack.has_value());
  const auto& sources = first.FirstGrade()->mask_stack->sources;
  ASSERT_EQ(sources.size(), 2U);
  EXPECT_EQ(sources[0].mask_id, MaskId{"mask.a"});
  EXPECT_EQ(sources[1].mask_id, MaskId{"mask.z"});

  document.PrimaryGrade()->MoveMaskForDisplay(MaskId{"mask.z"}, 1);
  EXPECT_EQ(document.PrimaryGrade()->MaskAt(0).id, MaskId{"mask.a"});
  EXPECT_EQ(document.PrimaryGrade()->MaskAt(1).id, MaskId{"mask.z"});
  EXPECT_FALSE(GraphCompiler::NeedsRecompile(first, document, prepared.CompileSource()));
  const auto second = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  EXPECT_EQ(second.static_key, first.static_key);
  const auto second_keys = BuildFrameResultContentKeys(second, prepared, document);
  EXPECT_EQ(second_keys.Value(sources[0].effective_output),
            first_keys.Value(sources[0].effective_output));
  EXPECT_EQ(second_keys.Value(sources[1].effective_output),
            first_keys.Value(sources[1].effective_output));
  EXPECT_EQ(second_keys.mask, first_keys.mask);
  EXPECT_EQ(second_keys.primary_grade, first_keys.primary_grade);
}

TEST(GpuDagGraphCompiler, RangeInputUsesOwningGradeSceneInput) {
  const auto prepared = MakeRgbPrepared();
  auto       document = CreateDefaultPipelineDocument();
  multi_grade_test::AddCleanGradesBeforeDrt(document, {"grade.b"});
  auto* grade_b = multi_grade_test::GradeNode(document, "grade.b");
  ASSERT_NE(grade_b, nullptr);
  grade_mask_test::AddMask(*grade_b, grade_mask_test::MakeRadialMask(MaskId{"mask.b"}));
  document.MarkTopologyDirty();
  const auto plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  const auto* compiled_b = plan.FindGrade(NodeId{"grade.b"});
  const auto* compiled_a = plan.FindGrade(NodeId{"grade.primary"});
  ASSERT_NE(compiled_a, nullptr);
  ASSERT_NE(compiled_b, nullptr);
  ASSERT_TRUE(compiled_b->mask_stack.has_value());
  ASSERT_EQ(compiled_b->mask_stack->sources.size(), 1U);
  EXPECT_EQ(compiled_b->mask_stack->sources.front().range_input, compiled_b->scene_input);
  EXPECT_EQ(compiled_b->scene_input, compiled_a->scene_output);
  EXPECT_NE(compiled_b->mask_stack->sources.front().range_input, compiled_b->scene_output);
}

TEST(GpuDagGraphCompiler, EnabledMaskSourcesOwnStableMaskIdPasses) {
  auto document = CreateDefaultPipelineDocument();
  AddSortedRadialPair(document);
  const auto plan = GraphCompiler::CompileStatic(document, DirectRgbSource());
  ASSERT_TRUE(plan.FirstGrade()->mask_stack.has_value());
  const auto evals = plan.PassesOfKind(GpuPassKind::MaskEvaluate);
  ASSERT_EQ(evals.size(), 2U);
  EXPECT_EQ(evals[0]->instance.mask_id, MaskId{"mask.a"});
  EXPECT_EQ(evals[1]->instance.mask_id, MaskId{"mask.z"});
  EXPECT_EQ(evals[0]->owner, NodeId{"grade.primary"});
  const auto unions = plan.PassesOfKind(GpuPassKind::MaskUnion);
  ASSERT_EQ(unions.size(), 1U);
  EXPECT_TRUE(unions.front()->instance.mask_id.Empty());
  RemainingValueConsumers remaining(plan);
  EXPECT_EQ(remaining.Remaining(plan.FirstGrade()->mask_stack->sources[0].effective_output), 1U);
  EXPECT_EQ(remaining.Remaining(plan.FirstGrade()->mask_stack->sources[1].effective_output), 1U);
  EXPECT_EQ(remaining.Remaining(plan.FirstGrade()->mask_output), 1U);
}

TEST(GpuDagGraphCompiler, MaskAddRemoveAndSourceKindRebuildStaticPlan) {
  const auto prepared = MakeRgbPrepared();
  auto       document = CreateDefaultPipelineDocument();
  const auto empty = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  grade_mask_test::AddRadialMask(document, MaskId{"mask.a"});
  EXPECT_TRUE(GraphCompiler::NeedsRecompile(empty, document, prepared.CompileSource()));
  const auto with_radial =
      GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  document.PrimaryGrade()->SetMaskOpacity(MaskId{"mask.a"}, 0.4f);
  EXPECT_FALSE(GraphCompiler::NeedsRecompile(with_radial, document, prepared.CompileSource()));
  document.PrimaryGrade()->SetMaskInvert(MaskId{"mask.a"}, true);
  EXPECT_FALSE(GraphCompiler::NeedsRecompile(with_radial, document, prepared.CompileSource()));
  document.PrimaryGrade()->ReplaceMaskSource(MaskId{"mask.a"}, LinearGradientMaskSource{});
  EXPECT_TRUE(GraphCompiler::NeedsRecompile(with_radial, document, prepared.CompileSource()));
  const auto with_gradient =
      GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  document.PrimaryGrade()->RemoveMask(MaskId{"mask.a"});
  EXPECT_TRUE(GraphCompiler::NeedsRecompile(with_gradient, document, prepared.CompileSource()));
}

TEST(GpuDagGraphCompiler, InvalidMaskStackBindingsFailBeforeGpuWork) {
  auto document = CreateDefaultPipelineDocument();
  AddSortedRadialPair(document);
  auto plan = GraphCompiler::CompileStatic(document, DirectRgbSource());
  ASSERT_TRUE(plan.FirstGrade()->mask_stack.has_value());
  ASSERT_EQ(plan.FirstGrade()->mask_stack->sources.size(), 2U);

  auto unsorted = plan;
  std::swap(unsorted.grade_nodes.front().mask_stack->sources[0],
            unsorted.grade_nodes.front().mask_stack->sources[1]);
  EXPECT_THROW(ValidateExecutionPlan(unsorted), std::runtime_error);

  auto wrong_range = plan;
  wrong_range.grade_nodes.front().mask_stack->sources.front().range_input = plan.geometry_output;
  EXPECT_THROW(ValidateExecutionPlan(wrong_range), std::runtime_error);

  auto duplicate_union = plan;
  auto extra           = *duplicate_union.PassesOfKind(GpuPassKind::MaskUnion).front();
  extra.instance.ordinal = 99;
  duplicate_union.passes.push_back(extra);
  EXPECT_THROW(ValidateExecutionPlan(duplicate_union), std::runtime_error);
}

}  // namespace
}  // namespace alcedo
