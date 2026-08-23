//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include "../input/prepared_raw_test_support.hpp"
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

}  // namespace
}  // namespace alcedo
