//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/analytic_mask_node_model.hpp"
#include "edit/graph/legacy_pipeline_importer.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/result_content_key.hpp"

namespace alcedo {
namespace {

auto MakePrepared() -> PreparedRawInput {
  return RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(16, 12),
                                       gpu_dag_test::FullSensor(16, 12));
}

TEST(GpuDagResultContentKey, GraphCompilerAssignsDistinctSensorGeometryAndDevelopValueIds) {
  auto       document = CreateDefaultPipelineDocument();
  const auto plan = GraphCompiler::Compile(document, MakePrepared().CompileSource(), RenderRequest{});
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
  auto prepared = MakePrepared();
  auto document = CreateDefaultPipelineDocument();
  auto plan     = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  const auto base = BuildFrameResultContentKeys(plan, prepared, document);

  auto develop = document.Develop()->Params().Params();
  develop.wb_mode    = "custom";
  develop.custom_cct = 4200.0f;
  develop.custom_tint = 8.0f;
  document.Develop()->Params().ReplaceParams(develop);
  auto* exposure = dynamic_cast<ExposureModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(1.25f);
  auto drt = document.Drt()->Params().Params();
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
  auto prepared = MakePrepared();
  auto document = CreateDefaultPipelineDocument();
  auto plan     = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  const auto base = BuildFrameResultContentKeys(plan, prepared, document);

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
  auto prepared = MakePrepared();
  auto document = CreateDefaultPipelineDocument();
  auto plan     = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  const auto base      = HashLlfReferenceKey(plan, prepared, document);
  const auto base_keys = BuildFrameResultContentKeys(plan, prepared, document);

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

TEST(GpuDagResultContentKey, HighlightRecoverChangesSensorLinearAndAllDownstreamKeys) {
  auto prepared = MakePrepared();
  auto document = CreateDefaultPipelineDocument();
  auto plan     = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  const auto base = BuildFrameResultContentKeys(plan, prepared, document);

  auto develop = document.Develop()->Params().Params();
  develop.highlights_reconstruct = !develop.highlights_reconstruct;
  document.Develop()->Params().ReplaceParams(develop);
  const auto edited = BuildFrameResultContentKeys(plan, prepared, document);
  EXPECT_NE(edited.sensor_linear, base.sensor_linear);
  EXPECT_NE(edited.geometry_scene_source, base.geometry_scene_source);
  EXPECT_NE(edited.develop_image, base.develop_image);
  EXPECT_NE(edited.primary_grade, base.primary_grade);
  EXPECT_NE(edited.drt_display, base.drt_display);
}

TEST(GpuDagResultContentKey, ExposureEditWithTemporaryOvalKeepsSensorGeometryDevelopAndMask) {
  auto prepared = MakePrepared();
  auto document = CreateDefaultPipelineDocument();
  AttachTemporaryPrimaryGradeOvalMask(document);
  auto plan     = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
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

TEST(GpuDagResultContentKey, AnalyticMaskParamChangeInvalidatesMaskAndGradeNotSensor) {
  auto prepared = MakePrepared();
  auto document = CreateDefaultPipelineDocument();
  AttachTemporaryPrimaryGradeOvalMask(document);
  auto plan     = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  const auto base = BuildFrameResultContentKeys(plan, prepared, document);

  auto* mask = dynamic_cast<AnalyticMaskNodeModel*>(
      document.Graph().FindNode(NodeId{"mask.ui_test.radial"}));
  ASSERT_NE(mask, nullptr);
  auto radial            = mask->Radial();
  radial.major_radius    = 0.41f;
  mask->SetRadial(radial);
  const auto edited = BuildFrameResultContentKeys(plan, prepared, document);
  EXPECT_EQ(edited.sensor_linear, base.sensor_linear);
  EXPECT_EQ(edited.geometry_scene_source, base.geometry_scene_source);
  EXPECT_EQ(edited.develop_image, base.develop_image);
  EXPECT_NE(edited.mask, base.mask);
  EXPECT_NE(edited.primary_grade, base.primary_grade);
}

TEST(GpuDagResultContentKey, IdenticalInputsProduceIdenticalKeys) {
  auto prepared = MakePrepared();
  auto document = CreateDefaultPipelineDocument();
  auto plan     = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  const auto a  = BuildFrameResultContentKeys(plan, prepared, document);
  const auto b  = BuildFrameResultContentKeys(plan, prepared, document);
  EXPECT_EQ(a.sensor_linear, b.sensor_linear);
  EXPECT_EQ(a.geometry_scene_source, b.geometry_scene_source);
  EXPECT_EQ(a.develop_image, b.develop_image);
  EXPECT_EQ(a.primary_grade, b.primary_grade);
  EXPECT_EQ(a.drt_display, b.drt_display);
}

TEST(GpuDagResultContentKey, CameraProfileChangeInvalidatesDevelopImageNotSensorLinear) {
  auto prepared = MakePrepared();
  auto document = CreateDefaultPipelineDocument();
  auto plan     = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  const auto base = BuildFrameResultContentKeys(plan, prepared, document);

  auto develop = document.Develop()->Params().Params();
  develop.camera_profile.color_matrices_valid = true;
  develop.camera_profile.color_matrix_1[0]    = 0.91;
  document.Develop()->Params().ReplaceParams(develop);
  const auto edited = BuildFrameResultContentKeys(plan, prepared, document);
  EXPECT_EQ(edited.sensor_linear, base.sensor_linear);
  EXPECT_EQ(edited.geometry_scene_source, base.geometry_scene_source);
  EXPECT_NE(edited.develop_image, base.develop_image);
}

TEST(GpuDagResultContentKey, ApplyOntoExposureKeepsSensorGeometryCameraAndMaskWithOval) {
  auto prepared = MakePrepared();
  auto document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  AttachTemporaryPrimaryGradeOvalMask(document);
  auto plan     = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
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

}  // namespace
}  // namespace alcedo
