//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/result_content_key.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <variant>

#include "../graph/grade_owned_mask_support.hpp"
#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/legacy_pipeline_importer.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/runtime/graph_compiler.hpp"

namespace alcedo {
namespace {

auto MakePrepared() -> PreparedRawInput {
  return RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(16, 12),
                                       gpu_dag_test::FullSensor(16, 12));
}

void ConnectRasterMask(PipelineDocument& document, std::string asset_key = "test.raster") {
  grade_mask_test::AddBrushMask(document, MaskId{"mask.raster"}, MaskAssetKey{std::move(asset_key)});
}

TEST(GpuDagResultContentKey, GraphCompilerAssignsDistinctSensorGeometryAndDevelopValueIds) {
  auto       document = CreateDefaultPipelineDocument();
  const auto plan =
      GraphCompiler::Compile(document, MakePrepared().CompileSource(), RenderRequest{});
  EXPECT_EQ(plan.sensor_linear_output.producer.Value(), "develop");
  EXPECT_EQ(plan.sensor_linear_output.output_port.Value(), "sensor_linear");
  EXPECT_EQ(plan.geometry_output.producer.Value(), "geometry");
  EXPECT_EQ(plan.geometry_output.output_port.Value(), "scene_source");
  EXPECT_EQ(plan.develop_output.producer.Value(), "develop");
  EXPECT_EQ(plan.develop_output.output_port.Value(), "image");
  EXPECT_TRUE(plan.sensor_linear_output < plan.geometry_output ||
              plan.geometry_output < plan.sensor_linear_output);
  EXPECT_TRUE(plan.geometry_output < plan.develop_output ||
              plan.develop_output < plan.geometry_output);
  EXPECT_TRUE(plan.sensor_linear_output < plan.develop_output ||
              plan.develop_output < plan.sensor_linear_output);
}

TEST(GpuDagResultContentKey, SensorLinearKeyIgnoresCctTintGradeAndDrt) {
  auto       prepared = MakePrepared();
  auto       document = CreateDefaultPipelineDocument();
  auto       plan     = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  const auto base     = BuildFrameResultContentKeys(plan, prepared, document);

  auto       develop  = document.Develop()->Params().Params();
  develop.wb_mode     = "custom";
  develop.custom_cct  = 4200.0f;
  develop.custom_tint = 8.0f;
  document.Develop()->Params().ReplaceParams(develop);
  auto* exposure = dynamic_cast<ExposureModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(1.25f);
  auto drt           = document.Drt()->Params().Params();
  drt.peak_luminance = 200.0f;
  document.Drt()->Params().ReplaceParams(drt);

  const auto edited = BuildFrameResultContentKeys(plan, prepared, document);
  EXPECT_EQ(edited.sensor_linear, base.sensor_linear);
  EXPECT_EQ(edited.geometry_scene_source, base.geometry_scene_source);
  EXPECT_NE(edited.develop_image, base.develop_image);
  EXPECT_NE(edited.primary_grade, base.primary_grade);
  EXPECT_NE(edited.drt_display, base.drt_display);
}

TEST(GpuDagResultContentKey, GeometryKeyIncludesViewportAndCropAndIgnoresGrade) {
  auto          prepared = MakePrepared();
  auto          document = CreateDefaultPipelineDocument();
  auto          plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  const auto    base = BuildFrameResultContentKeys(plan, prepared, document);

  RenderRequest viewport;
  viewport.view.visible_rect_in_edit_space = {0.1f, 0.1f, 0.8f, 0.8f};
  viewport.view.viewport_extent            = {12, 8};
  GraphCompiler::BindFrameGeometry(plan, document, viewport);
  const auto after_view = BuildFrameResultContentKeys(plan, prepared, document);
  EXPECT_EQ(after_view.sensor_linear, base.sensor_linear);
  EXPECT_NE(after_view.geometry_scene_source, base.geometry_scene_source);

  document.Geometry().SetCropRect({0.05f, 0.05f, 0.9f, 0.9f});
  GraphCompiler::BindFrameGeometry(plan, document, RenderRequest{});
  const auto after_crop = BuildFrameResultContentKeys(plan, prepared, document);
  EXPECT_EQ(after_crop.sensor_linear, base.sensor_linear);
  EXPECT_NE(after_crop.geometry_scene_source, base.geometry_scene_source);

  document.Geometry().SetCropRect({});
  GraphCompiler::BindFrameGeometry(plan, document, RenderRequest{});
  auto* exposure = dynamic_cast<ExposureModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(-0.5f);
  const auto after_grade = BuildFrameResultContentKeys(plan, prepared, document);
  EXPECT_EQ(after_grade.geometry_scene_source, base.geometry_scene_source);
  EXPECT_EQ(after_grade.develop_image, base.develop_image);
  EXPECT_NE(after_grade.primary_grade, base.primary_grade);
}

TEST(GpuDagResultContentKey, LlfReferenceKeyIgnoresViewportAndFollowsCropAndGrade) {
  auto          prepared = MakePrepared();
  auto          document = CreateDefaultPipelineDocument();
  auto          plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  const auto    base = HashLlfReferenceKey(plan, prepared, document);
  const auto    base_keys = BuildFrameResultContentKeys(plan, prepared, document);

  RenderRequest viewport;
  viewport.view.visible_rect_in_edit_space = {0.25f, 0.25f, 0.5f, 0.5f};
  viewport.view.viewport_extent            = {8, 6};
  GraphCompiler::BindFrameGeometry(plan, document, viewport);
  EXPECT_EQ(HashLlfReferenceKey(plan, prepared, document), base);
  EXPECT_NE(BuildFrameResultContentKeys(plan, prepared, document).geometry_scene_source,
            base_keys.geometry_scene_source);

  document.Geometry().SetCropRect({0.1f, 0.1f, 0.8f, 0.8f});
  GraphCompiler::BindFrameGeometry(plan, document, viewport);
  EXPECT_NE(HashLlfReferenceKey(plan, prepared, document), base);

  document.Geometry().SetCropRect({});
  GraphCompiler::BindFrameGeometry(plan, document, RenderRequest{});
  auto* shadows = dynamic_cast<ShadowsModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Shadows()));
  ASSERT_NE(shadows, nullptr);
  shadows->SetValue(40.0f);
  EXPECT_NE(HashLlfReferenceKey(plan, prepared, document), base);
}

TEST(GpuDagResultContentKey, LlfSourceKeyIgnoresShadowsAndHighlightsSliderValues) {
  auto       prepared = MakePrepared();
  auto       document = CreateDefaultPipelineDocument();
  auto       plan     = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  const auto base_source = HashLlfSourceKey(plan, prepared, document);
  const auto base_result = HashLlfReferenceKey(plan, prepared, document);

  auto*      shadows     = dynamic_cast<ShadowsModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Shadows()));
  ASSERT_NE(shadows, nullptr);
  shadows->SetValue(40.0f);
  EXPECT_EQ(HashLlfSourceKey(plan, prepared, document), base_source);
  EXPECT_NE(HashLlfReferenceKey(plan, prepared, document), base_result);

  auto* exposure = dynamic_cast<ExposureModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(-0.5f);
  EXPECT_NE(HashLlfSourceKey(plan, prepared, document), base_source);
}

TEST(GpuDagResultContentKey, HighlightRecoverChangesSensorLinearAndAllDownstreamKeys) {
  auto       prepared = MakePrepared();
  auto       document = CreateDefaultPipelineDocument();
  auto       plan     = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  const auto base     = BuildFrameResultContentKeys(plan, prepared, document);

  auto       develop  = document.Develop()->Params().Params();
  develop.highlights_reconstruct = !develop.highlights_reconstruct;
  document.Develop()->Params().ReplaceParams(develop);
  const auto edited = BuildFrameResultContentKeys(plan, prepared, document);
  EXPECT_NE(edited.sensor_linear, base.sensor_linear);
  EXPECT_NE(edited.geometry_scene_source, base.geometry_scene_source);
  EXPECT_NE(edited.develop_image, base.develop_image);
  EXPECT_NE(edited.primary_grade, base.primary_grade);
  EXPECT_NE(edited.drt_display, base.drt_display);
}

TEST(GpuDagResultContentKey, ExposureEditWithRasterMaskKeepsSensorGeometryDevelopAndMask) {
  auto prepared = MakePrepared();
  auto document = CreateDefaultPipelineDocument();
  ConnectRasterMask(document);
  auto       plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  const auto base = BuildFrameResultContentKeys(plan, prepared, document);
  ASSERT_FALSE(base.mask.Empty());

  auto* exposure = dynamic_cast<ExposureModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(2.0f);
  const auto edited = BuildFrameResultContentKeys(plan, prepared, document);
  EXPECT_EQ(edited.sensor_linear, base.sensor_linear);
  EXPECT_EQ(edited.geometry_scene_source, base.geometry_scene_source);
  EXPECT_EQ(edited.develop_image, base.develop_image);
  EXPECT_EQ(edited.mask, base.mask);
  EXPECT_NE(edited.primary_grade, base.primary_grade);
  EXPECT_NE(edited.drt_display, base.drt_display);
}

TEST(GpuDagResultContentKey, RasterMaskParamChangeInvalidatesMaskAndGradeNotSensor) {
  auto prepared = MakePrepared();
  auto document = CreateDefaultPipelineDocument();
  ConnectRasterMask(document);
  auto       plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  const auto base = BuildFrameResultContentKeys(plan, prepared, document);

  auto* mask = document.PrimaryGrade()->FindMask(MaskId{"mask.raster"});
  ASSERT_NE(mask, nullptr);
  std::get<BrushMaskSource>(mask->source).feather_radius = 1.25f;
  const auto edited = BuildFrameResultContentKeys(plan, prepared, document);
  EXPECT_EQ(edited.sensor_linear, base.sensor_linear);
  EXPECT_EQ(edited.geometry_scene_source, base.geometry_scene_source);
  EXPECT_EQ(edited.develop_image, base.develop_image);
  EXPECT_NE(edited.mask, base.mask);
  EXPECT_NE(edited.primary_grade, base.primary_grade);
}

TEST(GpuDagResultContentKey, IdenticalInputsProduceIdenticalKeys) {
  auto       prepared = MakePrepared();
  auto       document = CreateDefaultPipelineDocument();
  auto       plan     = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  const auto a        = BuildFrameResultContentKeys(plan, prepared, document);
  const auto b        = BuildFrameResultContentKeys(plan, prepared, document);
  EXPECT_EQ(a.sensor_linear, b.sensor_linear);
  EXPECT_EQ(a.geometry_scene_source, b.geometry_scene_source);
  EXPECT_EQ(a.develop_image, b.develop_image);
  EXPECT_EQ(a.primary_grade, b.primary_grade);
  EXPECT_EQ(a.drt_display, b.drt_display);
}

TEST(GpuDagResultContentKey, CameraProfileChangeInvalidatesDevelopImageNotSensorLinear) {
  auto       prepared = MakePrepared();
  auto       document = CreateDefaultPipelineDocument();
  auto       plan     = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  const auto base     = BuildFrameResultContentKeys(plan, prepared, document);

  auto       develop  = document.Develop()->Params().Params();
  develop.camera_profile.color_matrices_valid = true;
  develop.camera_profile.color_matrix_1[0]    = 0.91;
  document.Develop()->Params().ReplaceParams(develop);
  const auto edited = BuildFrameResultContentKeys(plan, prepared, document);
  EXPECT_EQ(edited.sensor_linear, base.sensor_linear);
  EXPECT_EQ(edited.geometry_scene_source, base.geometry_scene_source);
  EXPECT_NE(edited.develop_image, base.develop_image);
}

TEST(GpuDagResultContentKey, ApplyOntoExposureKeepsSensorGeometryCameraAndMaskWithRaster) {
  auto prepared = MakePrepared();
  auto document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  ConnectRasterMask(document);
  auto       plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  const auto base = BuildFrameResultContentKeys(plan, prepared, document);
  ASSERT_FALSE(base.mask.Empty());

  nlohmann::json json;
  json["Basic Adjustment"]["Basic Adjustment"]["exposure"] = {
      {"type", 2}, {"enable", true}, {"params", {{"exposure", 2.0}}}};
  ASSERT_TRUE(LegacyPipelineImporter::ApplyOnto(document, json).empty());
  GraphCompiler::BindFrameGeometry(plan, document, RenderRequest{});
  const auto edited = BuildFrameResultContentKeys(plan, prepared, document);
  EXPECT_EQ(edited.sensor_linear, base.sensor_linear);
  EXPECT_EQ(edited.geometry_scene_source, base.geometry_scene_source);
  EXPECT_EQ(edited.develop_image, base.develop_image);
  EXPECT_EQ(edited.mask, base.mask);
  EXPECT_NE(edited.primary_grade, base.primary_grade);
  EXPECT_NE(edited.drt_display, base.drt_display);
}

TEST(GpuDagResultContentKey, ClarityOnDrtChangesDisplayKeyNotGradeKey) {
  auto       prepared = MakePrepared();
  auto       document = CreateDefaultPipelineDocument();
  auto       plan     = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  const auto base     = BuildFrameResultContentKeys(plan, prepared, document);

  auto* clarity = dynamic_cast<ClarityModel*>(
      document.Drt()->FindAdjustmentByType(type_ids::Clarity()));
  ASSERT_NE(clarity, nullptr);
  clarity->SetValue(40.0f);
  const auto edited = BuildFrameResultContentKeys(plan, prepared, document);
  EXPECT_EQ(edited.sensor_linear, base.sensor_linear);
  EXPECT_EQ(edited.geometry_scene_source, base.geometry_scene_source);
  EXPECT_EQ(edited.develop_image, base.develop_image);
  EXPECT_EQ(edited.primary_grade, base.primary_grade);
  EXPECT_NE(edited.drt_display, base.drt_display);
}

TEST(GpuDagResultContentKey, MiddleGradeEditReusesUpstreamResults) {
  auto prepared = MakePrepared();
  auto document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.b"}).empty());
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.c"}).empty());
  auto       plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  const auto base = BuildFrameResultContentKeys(plan, prepared, document);
  ASSERT_EQ(plan.grade_nodes.size(), 3U);
  EXPECT_EQ(base.GradeScene(NodeId{"grade.primary"}), base.primary_grade);
  EXPECT_NE(base.GradeScene(NodeId{"grade.b"}), base.primary_grade);
  EXPECT_EQ(base.Value(plan.drt.scene_input), base.GradeScene(NodeId{"grade.c"}));

  auto* middle = dynamic_cast<ColorGradeNodeModel*>(document.Graph().FindNode(NodeId{"grade.b"}));
  ASSERT_NE(middle, nullptr);
  auto* exposure = dynamic_cast<ExposureModel*>(middle->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(0.75f);
  const auto edited = BuildFrameResultContentKeys(plan, prepared, document);
  EXPECT_EQ(edited.sensor_linear, base.sensor_linear);
  EXPECT_EQ(edited.develop_image, base.develop_image);
  EXPECT_EQ(edited.GradeScene(NodeId{"grade.primary"}), base.GradeScene(NodeId{"grade.primary"}));
  EXPECT_NE(edited.GradeScene(NodeId{"grade.b"}), base.GradeScene(NodeId{"grade.b"}));
  EXPECT_NE(edited.GradeScene(NodeId{"grade.c"}), base.GradeScene(NodeId{"grade.c"}));
  EXPECT_NE(edited.drt_display, base.drt_display);
}

TEST(GpuDagResultContentKey, ReconnectChangesNoncommutingGradeResult) {
  auto prepared = MakePrepared();
  auto document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.z"}).empty());
  auto* primary = document.PrimaryGrade();
  auto* extra   = dynamic_cast<ColorGradeNodeModel*>(document.Graph().FindNode(NodeId{"grade.z"}));
  ASSERT_NE(primary, nullptr);
  ASSERT_NE(extra, nullptr);
  auto* primary_exposure =
      dynamic_cast<ExposureModel*>(primary->FindAdjustmentByType(type_ids::Exposure()));
  auto* extra_exposure =
      dynamic_cast<ExposureModel*>(extra->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(primary_exposure, nullptr);
  ASSERT_NE(extra_exposure, nullptr);
  primary_exposure->SetValue(1.5f);
  extra_exposure->SetValue(-0.5f);

  auto       before_plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  const auto before      = BuildFrameResultContentKeys(before_plan, prepared, document);
  EXPECT_EQ(before_plan.grade_nodes.front().node_id, NodeId{"grade.primary"});
  EXPECT_EQ(before_plan.grade_nodes.back().node_id, NodeId{"grade.z"});

  ASSERT_TRUE(ReconnectColorGrade(document, NodeId{"grade.z"}, NodeId{"develop"},
                                  NodeId{"grade.primary"})
                  .empty());
  auto       after_plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  const auto after      = BuildFrameResultContentKeys(after_plan, prepared, document);
  EXPECT_EQ(after_plan.grade_nodes.front().node_id, NodeId{"grade.z"});
  EXPECT_EQ(after.develop_image, before.develop_image);
  EXPECT_EQ(after.GradeScene(NodeId{"grade.z"}), after.primary_grade);
  EXPECT_NE(after.GradeScene(NodeId{"grade.z"}), before.GradeScene(NodeId{"grade.z"}));
  EXPECT_NE(after.GradeScene(NodeId{"grade.primary"}), before.GradeScene(NodeId{"grade.primary"}));
  EXPECT_NE(after.drt_display, before.drt_display);
}

TEST(GpuDagResultContentKey, TwoLocalToneGradesUseTheirOwnSources) {
  auto prepared = MakePrepared();
  auto document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.b"}).empty());
  auto* primary = document.PrimaryGrade();
  auto* second  = dynamic_cast<ColorGradeNodeModel*>(document.Graph().FindNode(NodeId{"grade.b"}));
  ASSERT_NE(primary, nullptr);
  ASSERT_NE(second, nullptr);
  auto* primary_shadows =
      dynamic_cast<ShadowsModel*>(primary->FindAdjustmentByType(type_ids::Shadows()));
  auto* second_shadows =
      dynamic_cast<ShadowsModel*>(second->FindAdjustmentByType(type_ids::Shadows()));
  ASSERT_NE(primary_shadows, nullptr);
  ASSERT_NE(second_shadows, nullptr);
  primary_shadows->SetValue(20.0f);
  second_shadows->SetValue(40.0f);

  auto       plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  const auto primary_source =
      HashLlfSourceKey(plan, prepared, document, NodeId{"grade.primary"});
  const auto second_source = HashLlfSourceKey(plan, prepared, document, NodeId{"grade.b"});
  const auto primary_ref =
      HashLlfReferenceKey(plan, prepared, document, NodeId{"grade.primary"});
  const auto second_ref = HashLlfReferenceKey(plan, prepared, document, NodeId{"grade.b"});
  EXPECT_NE(primary_source, second_source);
  EXPECT_NE(primary_ref, second_ref);

  auto* exposure =
      dynamic_cast<ExposureModel*>(primary->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(-0.25f);
  EXPECT_NE(HashLlfSourceKey(plan, prepared, document, NodeId{"grade.primary"}), primary_source);
  EXPECT_NE(HashLlfSourceKey(plan, prepared, document, NodeId{"grade.b"}), second_source);

  exposure->SetValue(kDefaultPipelineExposureEv);
  EXPECT_EQ(HashLlfSourceKey(plan, prepared, document, NodeId{"grade.primary"}), primary_source);
  EXPECT_EQ(HashLlfSourceKey(plan, prepared, document, NodeId{"grade.b"}), second_source);

  primary_shadows->SetValue(55.0f);
  EXPECT_EQ(HashLlfSourceKey(plan, prepared, document, NodeId{"grade.primary"}), primary_source);
  EXPECT_NE(HashLlfSourceKey(plan, prepared, document, NodeId{"grade.b"}), second_source);
  EXPECT_NE(HashLlfReferenceKey(plan, prepared, document, NodeId{"grade.primary"}), primary_ref);
}

TEST(GpuDagResultContentKey, LocalToneReferenceRemainsStableAcrossViewportChanges) {
  auto prepared = MakePrepared();
  auto document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.b"}).empty());
  auto       plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  const auto primary_ref =
      HashLlfReferenceKey(plan, prepared, document, NodeId{"grade.primary"});
  const auto second_ref = HashLlfReferenceKey(plan, prepared, document, NodeId{"grade.b"});
  const auto primary_source =
      HashLlfSourceKey(plan, prepared, document, NodeId{"grade.primary"});
  const auto base_geometry = BuildFrameResultContentKeys(plan, prepared, document).geometry_scene_source;

  RenderRequest viewport;
  viewport.view.visible_rect_in_edit_space = {0.2f, 0.2f, 0.5f, 0.5f};
  viewport.view.viewport_extent            = {10, 8};
  GraphCompiler::BindFrameGeometry(plan, document, viewport);
  EXPECT_EQ(HashLlfReferenceKey(plan, prepared, document, NodeId{"grade.primary"}), primary_ref);
  EXPECT_EQ(HashLlfReferenceKey(plan, prepared, document, NodeId{"grade.b"}), second_ref);
  EXPECT_EQ(HashLlfSourceKey(plan, prepared, document, NodeId{"grade.primary"}), primary_source);
  EXPECT_NE(BuildFrameResultContentKeys(plan, prepared, document).geometry_scene_source,
            base_geometry);
}

TEST(GpuDagResultContentKey, GradeWithoutPrimaryIdSelectsCompiledNodeKeys) {
  auto prepared = MakePrepared();
  auto document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.b"}).empty());
  ASSERT_TRUE(RemoveColorGradeAndBridge(document, NodeId{"grade.primary"}).empty());
  ASSERT_EQ(document.PrimaryGrade(), nullptr);
  auto* remaining =
      dynamic_cast<ColorGradeNodeModel*>(document.Graph().FindNode(NodeId{"grade.b"}));
  ASSERT_NE(remaining, nullptr);
  auto* exposure =
      dynamic_cast<ExposureModel*>(remaining->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(0.4f);

  auto       plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  const auto keys = BuildFrameResultContentKeys(plan, prepared, document);
  ASSERT_EQ(plan.grade_nodes.size(), 1U);
  EXPECT_EQ(plan.grade_nodes.front().node_id, NodeId{"grade.b"});
  EXPECT_EQ(keys.primary_grade, keys.GradeScene(NodeId{"grade.b"}));
  EXPECT_TRUE(keys.GradeScene(NodeId{"grade.primary"}).Empty());
  EXPECT_EQ(keys.Value(plan.SceneInputForDrt()), keys.GradeScene(NodeId{"grade.b"}));
}

TEST(GpuDagResultContentKey, SameAdjustmentTypeUsesDistinctNodeSlots) {
  auto prepared = MakePrepared();
  auto document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.b"}).empty());
  auto* primary = document.PrimaryGrade();
  auto* extra = dynamic_cast<ColorGradeNodeModel*>(document.Graph().FindNode(NodeId{"grade.b"}));
  ASSERT_NE(primary, nullptr);
  ASSERT_NE(extra, nullptr);
  AdjustmentInstanceId primary_exposure;
  AdjustmentInstanceId extra_exposure;
  for (std::size_t i = 0; i < primary->AdjustmentCount(); ++i) {
    if (primary->AdjustmentAt(i).Type() == type_ids::Exposure()) {
      primary_exposure = primary->AdjustmentIdAt(i);
    }
  }
  for (std::size_t i = 0; i < extra->AdjustmentCount(); ++i) {
    if (extra->AdjustmentAt(i).Type() == type_ids::Exposure()) {
      extra_exposure = extra->AdjustmentIdAt(i);
    }
  }
  EXPECT_FALSE(primary_exposure.Empty());
  EXPECT_FALSE(extra_exposure.Empty());
  EXPECT_NE(primary_exposure, extra_exposure);

  const auto plan  = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  const auto slots = CollectParameterSlotKeys(plan);
  bool       saw_primary = false;
  bool       saw_extra   = false;
  for (const auto& slot : slots) {
    if (slot.node_id == NodeId{"grade.primary"} && slot.adjustment_id == primary_exposure) {
      saw_primary = true;
    }
    if (slot.node_id == NodeId{"grade.b"} && slot.adjustment_id == extra_exposure) {
      saw_extra = true;
    }
  }
  EXPECT_TRUE(saw_primary);
  EXPECT_TRUE(saw_extra);

  const auto keys = BuildFrameResultContentKeys(plan, prepared, document);
  auto* exposure = dynamic_cast<ExposureModel*>(extra->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(0.9f);
  const auto edited = BuildFrameResultContentKeys(plan, prepared, document);
  EXPECT_EQ(edited.GradeScene(NodeId{"grade.primary"}), keys.GradeScene(NodeId{"grade.primary"}));
  EXPECT_NE(edited.GradeScene(NodeId{"grade.b"}), keys.GradeScene(NodeId{"grade.b"}));
}

}  // namespace
}  // namespace alcedo
