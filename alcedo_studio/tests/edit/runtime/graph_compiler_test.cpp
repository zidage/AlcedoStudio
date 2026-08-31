//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/graph_compiler.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/raster_mask_node_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/runtime/metal/metal_backend.hpp"
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
  bool       saw_lmt        = false;
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
  ASSERT_EQ(plan.primary_grade_stages.size(), 3U);
  EXPECT_EQ(plan.primary_grade_stages.front().kind, CompiledGradeStageKind::Pointwise);
  bool saw_llf_stage = false;
  for (const auto& stage : plan.primary_grade_stages) {
    EXPECT_NE(stage.kind, CompiledGradeStageKind::Neighborhood);
    if (stage.kind == CompiledGradeStageKind::LocalLaplacian) {
      saw_llf_stage = true;
      EXPECT_EQ(stage.count, 2U);
    }
  }
  EXPECT_TRUE(saw_llf_stage);
  ASSERT_EQ(plan.drt_post_adjustments.size(), 4U);
  EXPECT_EQ(plan.drt_post_adjustments[0].type, type_ids::Clarity());
  EXPECT_EQ(plan.drt_post_adjustments[1].type, type_ids::Sharpen());
  EXPECT_EQ(plan.drt_post_adjustments[2].type, type_ids::Halation());
  EXPECT_EQ(plan.drt_post_adjustments[3].type, type_ids::FilmGrain());
  for (const auto& adjustment : plan.drt_post_adjustments) {
    EXPECT_EQ(adjustment.algorithm, CompiledAdjustmentAlgorithm::Neighborhood);
  }
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
  auto node = std::make_unique<RasterMaskNodeModel>(NodeId{"mask.raster"});
  node->SetAssetKey(MaskAssetKey{"test.raster"});
  document.Graph().AddNode(std::move(node));
  document.Graph().Connect(NodeId{"mask.raster"}, PortId{"mask"}, NodeId{"grade.primary"},
                           PortId{"mask"});
  const auto with_mask =
      GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});

  EXPECT_TRUE(with_mask.Contains(GpuPassKind::MaskEvaluate));
  EXPECT_EQ(with_mask.peak_transient_bytes, without_mask.peak_transient_bytes);
}

TEST(GpuDagGraphCompiler, GraphCompilerPassListIsBackendNativeTypeFree) {
  static_assert(std::is_same_v<decltype(GpuPassDesc{}.kind), GpuPassKind>);
  static_assert(std::is_trivially_copyable_v<GpuPassDesc>);

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

}  // namespace
}  // namespace alcedo
