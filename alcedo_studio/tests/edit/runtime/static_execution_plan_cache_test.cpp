//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include "edit/graph/pipeline_document.hpp"
#include "../graph/grade_owned_mask_support.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/static_execution_plan_cache.hpp"
#include "../input/prepared_raw_test_support.hpp"

namespace alcedo {
namespace {

auto MakePrepared(DecodeRes decode_res = DecodeRes::FULL) -> PreparedRawInput {
  const auto pattern = gpu_dag_test::MakeRggbPattern();
  return RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), decode_res);
}

TEST(GpuDagGraphCompiler, GraphCompilerNeedsRecompileIsFalseForUnchangedTopologyAndSourceLayout) {
  auto       document = CreateDefaultPipelineDocument();
  const auto prepared = MakePrepared();
  const auto plan     = GraphCompiler::CompileStatic(document, prepared.CompileSource());

  auto* exposure = dynamic_cast<ExposureModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(0.75f);

  auto develop = document.Develop()->Params().Params();
  develop.wb_mode    = "custom";
  develop.custom_cct = 4200.0f;
  document.Develop()->Params().ReplaceParams(develop);

  auto drt = document.Drt()->Params().Params();
  drt.peak_luminance = 200.0f;
  document.Drt()->Params().ReplaceParams(drt);
  document.Geometry().SetCropRect({0.1f, 0.1f, 0.8f, 0.8f});
  document.Geometry().SetRotationDegrees(15.0f);

  EXPECT_FALSE(GraphCompiler::NeedsRecompile(plan, document, prepared.CompileSource()));
  EXPECT_EQ(plan.static_key, GraphCompiler::MakeStaticPlanKey(document, prepared.CompileSource()));
}

TEST(GpuDagGraphCompiler, GraphCompilerNeedsRecompileIsTrueWhenAdjustmentOrderChanges) {
  auto       document = CreateDefaultPipelineDocument();
  const auto prepared = MakePrepared();
  const auto plan     = GraphCompiler::CompileStatic(document, prepared.CompileSource());
  auto*      grade    = document.PrimaryGrade();
  grade->MoveAdjustment(grade->AdjustmentIdAt(0), grade->AdjustmentCount() - 1);

  EXPECT_TRUE(GraphCompiler::NeedsRecompile(plan, document, prepared.CompileSource()));
}

TEST(GpuDagGraphCompiler, GraphCompilerNeedsRecompileIsTrueWhenSourceExtentChanges) {
  auto       document = CreateDefaultPipelineDocument();
  const auto full     = MakePrepared(DecodeRes::FULL);
  const auto half     = MakePrepared(DecodeRes::HALF);
  const auto plan     = GraphCompiler::CompileStatic(document, full.CompileSource());

  EXPECT_TRUE(GraphCompiler::NeedsRecompile(plan, document, half.CompileSource()));
}

TEST(GpuDagGraphCompiler, GraphCompilerCompileDoesNotBakeViewportIntoStaticPlanKey) {
  auto           document = CreateDefaultPipelineDocument();
  const auto     prepared = MakePrepared();
  RenderRequest  preview;
  RenderRequest  viewport;
  viewport.view.visible_rect_in_edit_space = {0.25f, 0.25f, 0.5f, 0.5f};
  viewport.view.viewport_extent            = {48, 32};
  viewport.resolution.quality              = RenderQuality::Export;

  const auto preview_plan  = GraphCompiler::Compile(document, prepared.CompileSource(), preview);
  auto       viewport_plan = GraphCompiler::CompileStatic(document, prepared.CompileSource());
  const auto static_key    = viewport_plan.static_key;
  GraphCompiler::BindFrameGeometry(viewport_plan, document, viewport);

  EXPECT_EQ(preview_plan.static_key, viewport_plan.static_key);
  EXPECT_EQ(viewport_plan.static_key, static_key);
  EXPECT_NE(preview_plan.geometry.render_extent.width, viewport_plan.geometry.render_extent.width);
  EXPECT_FALSE(GraphCompiler::NeedsRecompile(preview_plan, document, prepared.CompileSource()));
}

TEST(GpuDagGraphCompiler, GraphCompilerBindsFrameGeometryWithoutChangingStaticPlanKey) {
  auto       document = CreateDefaultPipelineDocument();
  const auto prepared = MakePrepared();
  auto       plan     = GraphCompiler::CompileStatic(document, prepared.CompileSource());
  EXPECT_EQ(plan.geometry.render_extent.width, 0U);

  RenderRequest request;
  request.view.viewport_extent = {40, 30};
  const auto key_before        = plan.static_key;
  GraphCompiler::BindFrameGeometry(plan, document, request);
  EXPECT_EQ(plan.static_key, key_before);
  EXPECT_GT(plan.geometry.render_extent.width, 0U);
}

TEST(GpuDagGraphCompiler, ParameterAndViewportEditsKeepStaticPlan) {
  auto                      document = CreateDefaultPipelineDocument();
  const auto                prepared = MakePrepared();
  StaticExecutionPlanCache  cache;
  (void)cache.GetOrCompile(document, prepared.CompileSource());

  auto* exposure = dynamic_cast<ExposureModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(-0.5f);
  auto drt = document.Drt()->Params().Params();
  drt.peak_luminance = 160.0f;
  document.Drt()->Params().ReplaceParams(drt);

  auto plan = cache.GetOrCompile(document, prepared.CompileSource());
  RenderRequest request;
  request.view.viewport_extent = {64, 48};
  GraphCompiler::BindFrameGeometry(plan, document, request);

  EXPECT_EQ(cache.GetStats().compiles, 1U);
  EXPECT_EQ(cache.GetStats().hits, 1U);
  EXPECT_EQ(cache.GetStats().misses, 1U);
  EXPECT_FALSE(GraphCompiler::NeedsRecompile(plan, document, prepared.CompileSource()));
}

TEST(GpuDagGraphCompiler, StaticExecutionPlanCacheRecompilesWhenMaskTopologyChanges) {
  auto                     document = CreateDefaultPipelineDocument();
  const auto               prepared = MakePrepared();
  StaticExecutionPlanCache cache;
  const auto               first = cache.GetOrCompile(document, prepared.CompileSource());
  ASSERT_NE(first.FirstGrade(), nullptr);
  EXPECT_FALSE(first.FirstGrade()->mask_stack.has_value());

  grade_mask_test::AddRadialMask(document, MaskId{"mask.radial"});
  const auto second = cache.GetOrCompile(document, prepared.CompileSource());

  EXPECT_EQ(cache.GetStats().compiles, 2U);
  EXPECT_EQ(cache.GetStats().misses, 2U);
  ASSERT_NE(second.FirstGrade(), nullptr);
  EXPECT_TRUE(second.FirstGrade()->mask_stack.has_value());
  EXPECT_TRUE(GraphCompiler::NeedsRecompile(first, document, prepared.CompileSource()));
}

TEST(GpuDagGraphCompiler, StaticExecutionPlanCacheRecompilesWhenSourceLayoutChanges) {
  auto                     document = CreateDefaultPipelineDocument();
  StaticExecutionPlanCache cache;
  (void)cache.GetOrCompile(document, MakePrepared(DecodeRes::FULL).CompileSource());
  (void)cache.GetOrCompile(document, MakePrepared(DecodeRes::HALF).CompileSource());
  EXPECT_EQ(cache.GetStats().compiles, 2U);
  EXPECT_EQ(cache.EntryCount(), 2U);
}

TEST(GpuDagGraphCompiler, StaticExecutionPlanCacheExposesHitMissAndCompileCounts) {
  auto                     document = CreateDefaultPipelineDocument();
  const auto               prepared = MakePrepared();
  StaticExecutionPlanCache cache(3);
  (void)cache.GetOrCompile(document, prepared.CompileSource());
  (void)cache.GetOrCompile(document, prepared.CompileSource());
  (void)cache.GetOrCompile(document, prepared.CompileSource());

  EXPECT_EQ(cache.GetStats().misses, 1U);
  EXPECT_EQ(cache.GetStats().hits, 2U);
  EXPECT_EQ(cache.GetStats().compiles, 1U);
  EXPECT_EQ(cache.BackendCapabilityVersion(), 3U);
}

}  // namespace
}  // namespace alcedo
