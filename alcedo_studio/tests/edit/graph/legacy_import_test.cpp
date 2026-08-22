//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include "edit/graph/legacy_pipeline_importer.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"

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
  json["Output Transform"]["Output Transform"]["odt"] = {
      {"type", 17},
      {"enable", true},
      {"params", {{"odt", {{"method", "aces_2_0"}, {"peak_luminance", 200.0}}}}}};
  return json;
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

}  // namespace alcedo
