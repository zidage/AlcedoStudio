//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/geometry/crop_rotate_op.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <opencv2/core.hpp>

#include "edit/operators/geometry/resize_op.hpp"
#include "edit/pipeline/default_pipeline_params.hpp"
#include "image/image_buffer.hpp"

namespace alcedo {
namespace {

auto MakeTestBuffer(int width, int height) -> std::shared_ptr<ImageBuffer> {
  cv::Mat image(height, width, CV_32FC3, cv::Scalar(0.0f, 0.0f, 0.0f));
  return std::make_shared<ImageBuffer>(std::move(image));
}

}  // namespace

TEST(CropRotateOpTests, DefaultFreeRoundTripPreservesFreeAspect) {
  const nlohmann::json params = pipeline_defaults::MakeDefaultCropRotateParams();
  CropRotateOp         op(params);

  const auto           exported = op.GetParams();
  ASSERT_TRUE(exported.contains("crop_rotate"));
  EXPECT_EQ(exported["crop_rotate"]["aspect_ratio_preset"], "free");
  EXPECT_FLOAT_EQ(exported["crop_rotate"]["aspect_ratio"]["width"].get<float>(), 1.0f);
  EXPECT_FLOAT_EQ(exported["crop_rotate"]["aspect_ratio"]["height"].get<float>(), 1.0f);
  EXPECT_EQ(exported["crop_rotate"]["source_size"]["width"], 0U);
  EXPECT_EQ(exported["crop_rotate"]["source_size"]["height"], 0U);
}

TEST(CropRotateOpTests, SourceSizeRoundTripPreservesPipelineImageGeometry) {
  nlohmann::json params                = pipeline_defaults::MakeDefaultCropRotateParams();
  params["crop_rotate"]["source_size"] = {{"width", 2731U}, {"height", 4096U}};

  CropRotateOp op(params);
  const auto   exported = op.GetParams();

  EXPECT_EQ(exported["crop_rotate"]["source_size"]["width"], 2731U);
  EXPECT_EQ(exported["crop_rotate"]["source_size"]["height"], 4096U);
}

TEST(CropRotateOpTests, PresetRoundTripPreservesPresetAndNumericRatio) {
  nlohmann::json params                        = pipeline_defaults::MakeDefaultCropRotateParams();
  params["crop_rotate"]["enabled"]             = true;
  params["crop_rotate"]["aspect_ratio_preset"] = "ratio_16_9";

  CropRotateOp op(params);
  const auto   exported = op.GetParams();

  EXPECT_EQ(exported["crop_rotate"]["aspect_ratio_preset"], "ratio_16_9");
  EXPECT_FLOAT_EQ(exported["crop_rotate"]["aspect_ratio"]["width"].get<float>(), 16.0f);
  EXPECT_FLOAT_EQ(exported["crop_rotate"]["aspect_ratio"]["height"].get<float>(), 9.0f);
}

TEST(CropRotateOpTests, FullImagePresetResolvesExpectedOutputSize) {
  nlohmann::json params                        = pipeline_defaults::MakeDefaultCropRotateParams();
  params["crop_rotate"]["enabled"]             = true;
  params["crop_rotate"]["enable_crop"]         = true;
  params["crop_rotate"]["aspect_ratio_preset"] = "ratio_16_9";

  CropRotateOp op(params);
  auto         buffer = MakeTestBuffer(400, 300);
  op.Apply(buffer);
  const auto& output = buffer->GetCPUData();

  EXPECT_EQ(output.cols, 400);
  EXPECT_EQ(output.rows, 225);
}

TEST(CropRotateOpTests, SquarePresetUsesSourceShortEdgeAsOutputLongEdge) {
  nlohmann::json params                        = pipeline_defaults::MakeDefaultCropRotateParams();
  params["crop_rotate"]["enabled"]             = true;
  params["crop_rotate"]["enable_crop"]         = true;
  params["crop_rotate"]["aspect_ratio_preset"] = "ratio_1_1";

  CropRotateOp op(params);
  auto         buffer = MakeTestBuffer(400, 300);
  op.Apply(buffer);
  const auto& output = buffer->GetCPUData();

  EXPECT_EQ(output.cols, 300);
  EXPECT_EQ(output.rows, 300);
}

TEST(CropRotateOpTests, FullImagePresetCentersAndMaximizesPortraitFrame) {
  nlohmann::json params                        = pipeline_defaults::MakeDefaultCropRotateParams();
  params["crop_rotate"]["enabled"]             = true;
  params["crop_rotate"]["enable_crop"]         = true;
  params["crop_rotate"]["aspect_ratio_preset"] = "ratio_4_3_35mm";

  CropRotateOp op(params);
  auto         buffer = MakeTestBuffer(300, 400);
  op.Apply(buffer);
  const auto& output = buffer->GetCPUData();

  EXPECT_EQ(output.cols, 300);
  EXPECT_EQ(output.rows, 400);
}

TEST(CropRotateOpTests, LandscapePresetUsesPortraitOrientationForPortraitSource) {
  nlohmann::json params                        = pipeline_defaults::MakeDefaultCropRotateParams();
  params["crop_rotate"]["enabled"]             = true;
  params["crop_rotate"]["enable_crop"]         = true;
  params["crop_rotate"]["aspect_ratio_preset"] = "ratio_16_9";

  CropRotateOp op(params);
  auto         buffer = MakeTestBuffer(300, 400);
  op.Apply(buffer);
  const auto& output = buffer->GetCPUData();

  EXPECT_EQ(output.cols, 225);
  EXPECT_EQ(output.rows, 400);
  EXPECT_NEAR(static_cast<float>(output.cols) / static_cast<float>(output.rows), 9.0f / 16.0f,
              0.01f);
}

TEST(CropRotateOpTests, AspectPresetFitsInsideCurrentCropAndPreservesCenter) {
  nlohmann::json params                = pipeline_defaults::MakeDefaultCropRotateParams();
  params["crop_rotate"]["enabled"]     = true;
  params["crop_rotate"]["enable_crop"] = true;
  params["crop_rotate"]["crop_rect"]   = {{"x", 0.1f}, {"y", 0.1f}, {"w", 0.6f}, {"h", 0.6f}};
  params["crop_rotate"]["aspect_ratio_preset"] = "ratio_16_9";

  CropRotateOp op(params);
  const auto   exported = op.GetParams();
  const auto&  rect     = exported["crop_rotate"]["crop_rect"];

  EXPECT_NEAR(rect["x"].get<float>(), 0.1f, 1e-6f);
  EXPECT_NEAR(rect["y"].get<float>(), 0.1f, 1e-6f);
  EXPECT_NEAR(rect["w"].get<float>(), 0.6f, 1e-6f);
  EXPECT_NEAR(rect["h"].get<float>(), 0.6f, 1e-6f);

  auto buffer = MakeTestBuffer(400, 300);
  op.Apply(buffer);
  const auto& output = buffer->GetCPUData();

  EXPECT_EQ(output.cols, 240);
  EXPECT_EQ(output.rows, 135);
}

TEST(CropRotateOpTests, RotationClampKeepsResolvedAspectRatio) {
  nlohmann::json params                        = pipeline_defaults::MakeDefaultCropRotateParams();
  params["crop_rotate"]["enabled"]             = true;
  params["crop_rotate"]["enable_crop"]         = true;
  params["crop_rotate"]["angle_degrees"]       = 45.0f;
  params["crop_rotate"]["aspect_ratio_preset"] = "ratio_16_9";

  CropRotateOp op(params);
  auto         buffer = MakeTestBuffer(400, 300);
  op.Apply(buffer);
  const auto& output = buffer->GetCPUData();

  ASSERT_GT(output.cols, 0);
  ASSERT_GT(output.rows, 0);
  EXPECT_NEAR(static_cast<float>(output.cols) / static_cast<float>(output.rows), 16.0f / 9.0f,
              0.02f);
}

TEST(CropRotateOpTests, InvalidAspectSanitizesBackToFree) {
  nlohmann::json params                        = pipeline_defaults::MakeDefaultCropRotateParams();
  params["crop_rotate"]["enabled"]             = true;
  params["crop_rotate"]["enable_crop"]         = true;
  params["crop_rotate"]["aspect_ratio_preset"] = "custom";
  params["crop_rotate"]["aspect_ratio"]        = {{"width", 0.0f}, {"height", -1.0f}};

  CropRotateOp op(params);
  const auto   exported = op.GetParams();

  EXPECT_EQ(exported["crop_rotate"]["aspect_ratio_preset"], "free");

  auto buffer = MakeTestBuffer(400, 300);
  op.Apply(buffer);
  const auto& output = buffer->GetCPUData();
  EXPECT_EQ(output.cols, 400);
  EXPECT_EQ(output.rows, 300);
}

TEST(CropRotateOpTests, ResizeOpScalesViewportOriginFromReferenceDimensions) {
  cv::Mat image(100, 200, CV_32FC3);
  for (int y = 0; y < image.rows; ++y) {
    for (int x = 0; x < image.cols; ++x) {
      image.at<cv::Vec3f>(y, x) = cv::Vec3f(static_cast<float>(x), static_cast<float>(y), 0.0f);
    }
  }

  nlohmann::json params;
  params["resize"] = {{"enable_scale", false},
                      {"maximum_edge", 4096},
                      {"enable_roi", true},
                      {"downsample_algorithm", "inter_area"},
                      {"roi",
                       {{"x", 25},
                        {"y", 10},
                        {"resize_factor_x", 0.5f},
                        {"resize_factor_y", 0.5f},
                        {"resize_factor", 0.5f},
                        {"reference_width", 100},
                        {"reference_height", 50}}}};

  ResizeOp op(params);
  auto     buffer = std::make_shared<ImageBuffer>(std::move(image));
  op.Apply(buffer);
  const auto& output = buffer->GetCPUData();

  ASSERT_EQ(output.cols, 100);
  ASSERT_EQ(output.rows, 50);
  EXPECT_FLOAT_EQ(output.at<cv::Vec3f>(0, 0)[0], 50.0f);
  EXPECT_FLOAT_EQ(output.at<cv::Vec3f>(0, 0)[1], 20.0f);
}

TEST(CropRotateOpTests, ResizeOpKeepsRoiNativeSizeWhenScaleDisabled) {
  cv::Mat        image(100, 200, CV_32FC3, cv::Scalar(0.0f, 0.0f, 0.0f));

  nlohmann::json params;
  params["resize"] = {{"enable_scale", false},
                      {"maximum_edge", 10},
                      {"enable_roi", true},
                      {"downsample_algorithm", "inter_area"},
                      {"roi",
                       {{"x", 0},
                        {"y", 0},
                        {"resize_factor_x", 0.5f},
                        {"resize_factor_y", 0.5f},
                        {"resize_factor", 0.5f},
                        {"reference_width", 200},
                        {"reference_height", 100}}}};

  ResizeOp op(params);
  auto     buffer = std::make_shared<ImageBuffer>(std::move(image));
  op.Apply(buffer);
  const auto& output = buffer->GetCPUData();

  EXPECT_EQ(output.cols, 100);
  EXPECT_EQ(output.rows, 50);
}

TEST(CropRotateOpTests, ResizeOpDoesNotUpsampleRoiWhenTargetExceedsNativeSize) {
  cv::Mat        image(100, 200, CV_32FC3, cv::Scalar(0.0f, 0.0f, 0.0f));

  nlohmann::json params;
  params["resize"] = {{"enable_scale", true},
                      {"maximum_edge", 400},
                      {"enable_roi", true},
                      {"downsample_algorithm", "inter_area"},
                      {"roi",
                       {{"x", 0},
                        {"y", 0},
                        {"resize_factor_x", 0.5f},
                        {"resize_factor_y", 0.5f},
                        {"resize_factor", 0.5f},
                        {"reference_width", 200},
                        {"reference_height", 100}}}};

  ResizeOp op(params);
  auto     buffer = std::make_shared<ImageBuffer>(std::move(image));
  op.Apply(buffer);
  const auto& output = buffer->GetCPUData();

  EXPECT_EQ(output.cols, 100);
  EXPECT_EQ(output.rows, 50);
}

}  // namespace alcedo
