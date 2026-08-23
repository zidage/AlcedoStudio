//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <memory>

#include "edit/graph/legacy_pipeline_importer.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/raster_mask_node_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/cat02_white_balance_model.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "test_camera_profile.hpp"

namespace alcedo {

namespace {

auto MakeLegacyStageJson() -> nlohmann::json {
  nlohmann::json json;
  json["Image Loading"]["Image Loading"]["raw_decode"] = {
      {"type", 0},
      {"enable", true},
      {"params", {{"raw", {{"method", "legacy"}, {"highlights_reconstruct", false},
                           {"use_camera_wb", false}, {"user_wb", 5200.0}}}}}};
  json["Image Loading"]["Image Loading"]["lens_calib"] = {
      {"type", 25},
      {"enable", true},
      {"params", {{"lens_calib", {{"enabled", true}, {"apply_distortion", false}}}}}};
  json["Geometry Adjustment"]["Geometry Adjustment"]["crop_rotate"] = {
      {"type", 24},
      {"enable", true},
      {"params",
       {{"crop_rotate",
         {{"enabled", true},
          {"angle_degrees", 12.0},
          {"enable_crop", true},
          {"expand_to_fit", false},
          {"crop_rect", {{"x", 0.1}, {"y", 0.2}, {"w", 0.5}, {"h", 0.6}}}}}}}};
  json["To Working Space"]["To Working Space"]["color_temp"] = {
      {"type", 26},
      {"enable", true},
      {"params",
       {{"color_temp",
         {{"mode", "custom"}, {"custom_cct", 7200.0}, {"custom_tint", 8.0}}}}}};
  json["Basic Adjustment"]["Basic Adjustment"]["exposure"] = {
      {"type", 2}, {"enable", true}, {"params", {{"exposure", 1.5}}}};
  json["Color Adjustment"]["Color Adjustment"]["saturation"] = {
      {"type", 10}, {"enable", true}, {"params", {{"saturation", 30.0}}}};
  json["Color Adjustment"]["Color Adjustment"]["tint"] = {
      {"type", 11}, {"enable", true}, {"params", {{"tint", 12.0}}}};
  json["Output Transform"]["Output Transform"]["odt"] = {
      {"type", 17},
      {"enable", true},
      {"params", {{"odt", {{"method", "aces_2_0"}, {"peak_luminance", 200.0}}}}}};
  return json;
}

void ConnectRasterMask(PipelineDocument& document, std::string asset_key = "test.raster") {
  auto node = std::make_unique<RasterMaskNodeModel>(NodeId{"mask.raster"});
  node->SetAssetKey(std::move(asset_key));
  document.Graph().AddNode(std::move(node));
  document.Graph().Connect(NodeId{"mask.raster"}, PortId{"mask"}, NodeId{"grade.primary"},
                           PortId{"mask"});
  document.MarkTopologyDirty();
}

}  // namespace

TEST(GpuDagModelGraph, LegacyStageJsonMapsRawGradeGeometryAndDrtToNewDocument) {
  const auto result = LegacyPipelineImporter::Import(MakeLegacyStageJson());
  ASSERT_TRUE(result.Ok()) << result.error;
  const auto& document = *result.document;
  ASSERT_NE(document.Develop(), nullptr);
  ASSERT_NE(document.PrimaryGrade(), nullptr);
  ASSERT_NE(document.Drt(), nullptr);

  const auto develop = document.Develop()->Params().Params();
  EXPECT_EQ(develop.demosaic_method, "legacy");
  EXPECT_FALSE(develop.highlights_reconstruct);
  EXPECT_FALSE(develop.use_camera_wb);
  EXPECT_FLOAT_EQ(develop.user_wb, 5200.0f);
  EXPECT_TRUE(develop.lens_enabled);
  EXPECT_FALSE(develop.apply_distortion);
  EXPECT_EQ(develop.wb_mode, "custom");
  EXPECT_FLOAT_EQ(develop.custom_cct, 7200.0f);
  EXPECT_FLOAT_EQ(develop.custom_tint, 8.0f);

  EXPECT_FLOAT_EQ(document.Geometry().RotationDegrees(), 12.0f);
  EXPECT_FALSE(document.Geometry().ExpandToFit());
  EXPECT_FLOAT_EQ(document.Geometry().CropRect().x, 0.1f);
  EXPECT_FLOAT_EQ(document.Geometry().CropRect().w, 0.5f);

  const auto* exposure = dynamic_cast<const ExposureModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  EXPECT_FLOAT_EQ(exposure->Value(), 1.5f);

  const auto* saturation = dynamic_cast<const SaturationModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Saturation()));
  ASSERT_NE(saturation, nullptr);
  EXPECT_FLOAT_EQ(saturation->Value(), 1.3f);

  EXPECT_EQ(document.Drt()->Params().Params().method, DrtMethod::Aces20);
  EXPECT_FLOAT_EQ(document.Drt()->Params().Params().peak_luminance, 200.0f);

  EXPECT_EQ(document.PrimaryGrade()->FindAdjustmentByType(type_ids::Tint()), nullptr);
  const auto* cat02 = dynamic_cast<const Cat02WhiteBalanceModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Cat02WhiteBalance()));
  ASSERT_NE(cat02, nullptr);
  EXPECT_FLOAT_EQ(cat02->TintOffset(), 12.0f);
}

TEST(GpuDagModelGraph, LegacyImportFailsOnUnknownOperatorType) {
  auto json = MakeLegacyStageJson();
  json["Basic Adjustment"]["Basic Adjustment"]["mystery"] = {
      {"type", 21}, {"enable", true}, {"params", nlohmann::json::object()}};
  const auto result = LegacyPipelineImporter::Import(json);
  EXPECT_FALSE(result.Ok());
  EXPECT_FALSE(result.error.empty());
  EXPECT_FALSE(result.document.has_value());
}

TEST(GpuDagModelGraph, ApplyOntoKeepsRasterMaskCameraProfileAndUpdatesExposure) {
  auto document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  ConnectRasterMask(document);
  document.ClearTopologyDirty();
  const auto profile_before = document.Develop()->Params().Params().camera_profile;
  const auto method_before  = document.Develop()->Params().Params().demosaic_method;

  nlohmann::json json;
  json["Basic Adjustment"]["Basic Adjustment"]["exposure"] = {
      {"type", 2}, {"enable", true}, {"params", {{"exposure", 2.25}}}};
  EXPECT_TRUE(LegacyPipelineImporter::ApplyOnto(document, json).empty());

  EXPECT_EQ(document.Graph().NodeCount(), 4u);
  EXPECT_FALSE(document.TopologyDirty());
  EXPECT_NE(document.Graph().FindNode(NodeId{"mask.raster"}), nullptr);
  EXPECT_TRUE(AllowsLegacyStageAdapterRemirror(document));
  EXPECT_EQ(document.Develop()->Params().Params().camera_profile, profile_before);
  EXPECT_EQ(document.Develop()->Params().Params().demosaic_method, method_before);
  const auto* exposure = dynamic_cast<const ExposureModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  EXPECT_FLOAT_EQ(exposure->Value(), 2.25f);
  const auto* contrast = dynamic_cast<const ContrastModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Contrast()));
  ASSERT_NE(contrast, nullptr);
  EXPECT_FLOAT_EQ(contrast->Value(), 0.0f);
}

TEST(GpuDagModelGraph, ApplyOntoRejectsUnknownTypeWithoutMutatingDocument) {
  auto document = CreateDefaultPipelineDocument();
  document.ClearTopologyDirty();
  auto json = MakeLegacyStageJson();
  json["Basic Adjustment"]["Basic Adjustment"]["mystery"] = {
      {"type", 21}, {"enable", true}, {"params", nlohmann::json::object()}};
  EXPECT_FALSE(LegacyPipelineImporter::ApplyOnto(document, json).empty());
  EXPECT_EQ(document.Graph().NodeCount(), 3u);
  const auto* exposure = dynamic_cast<const ExposureModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  EXPECT_FLOAT_EQ(exposure->Value(), 0.0f);
}

}  // namespace alcedo
