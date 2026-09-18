//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>
#include "support/editor_parameter_write_test.hpp"

#include <memory>
#include <string>

#include <opencv2/core.hpp>
#include <opencv2/core/cuda.hpp>

#include "app/editor_adjustment_pipeline.hpp"
#include "edit/operators/geometry/cuda_geometry_ops.hpp"
#include "edit/operators/geometry/resize_op.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "edit/pipeline/pipeline_accelerator.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "image/gpu_backend.hpp"
#include "image/image_buffer.hpp"

namespace alcedo {
namespace {

auto EnsureCudaDevice() -> bool {
  const int device_count = cv::cuda::getCudaEnabledDeviceCount();
  if (device_count <= 0) {
    return false;
  }
  cv::cuda::setDevice(0);
  return true;
}

auto MakeGradientGpuBuffer(int width, int height, int type) -> std::shared_ptr<ImageBuffer> {
  cv::Mat host(height, width, type);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const float fx = static_cast<float>(x) / static_cast<float>(std::max(width - 1, 1));
      const float fy = static_cast<float>(y) / static_cast<float>(std::max(height - 1, 1));
      if (type == CV_32FC3) {
        host.at<cv::Vec3f>(y, x) = cv::Vec3f(fx, fy, 0.5f * (fx + fy));
      } else {
        host.at<cv::Vec4f>(y, x) = cv::Vec4f(fx, fy, 0.5f * (fx + fy), 1.0f);
      }
    }
  }
  auto buffer = std::make_shared<ImageBuffer>(std::move(host));
  buffer->SyncToGPU(GpuBackendKind::CUDA);
  return buffer;
}

class EditorGeometryOverlayPipelineTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { RegisterAllOperators(); }
};

// Mirrors the user action "open Geometry panel while a committed crop exists":
// apply crop_rotate, then disable it for the source-frame overlay preview and
// re-run the geometry stage with a bilinear preview downscale. This is the
// exact operator sequence the unified scheduler runs for geometry_overlay_only.
TEST_F(EditorGeometryOverlayPipelineTest,
     OverlaySourceFramePreviewDisablesCropAndResizesSharedGpuInput) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  CPUPipelineExecutor executor;
  executor.ResetToCleanBaselineAdjustments();
  executor.SetAcceleratorBackendPreference(AcceleratorBackendPreference::CUDA);
  executor.SetExecutionStages();
  executor.SetResizeDownsampleAlgorithm(ResizeDownsampleAlgorithm::Bilinear);
  executor.SetRenderRegion(0, 0, 1.0f, 1.0f);
  executor.SetRenderRes(false, 256);

  EditorRenderAdjustmentSnapshot snapshot;
  snapshot.patches = {alcedo::test::SnapshotPatch({
      "crop_rotate",
      R"({"crop_rotate":{"enabled":true,"angle_degrees":12.5,"enable_crop":true,"crop_rect":{"x":0.15,"y":0.2,"w":0.55,"h":0.5},"expand_to_fit":false,"aspect_ratio_preset":"free","aspect_ratio":{"width":1.0,"height":1.0}}})",
      false})};

  std::string error;
  ASSERT_TRUE(ApplyEditorAdjustmentSnapshot(executor, snapshot, &error)) << error;
  ASSERT_TRUE(executor.GetStage(PipelineStageName::Geometry_Adjustment)
                  .GetOperator(OperatorType::CROP_ROTATE)
                  .has_value());
  EXPECT_TRUE(executor.GetStage(PipelineStageName::Geometry_Adjustment)
                  .GetOperator(OperatorType::CROP_ROTATE)
                  .value()
                  ->enable_);

  // First paint: committed crop is applied (matches pre-panel preview).
  auto cropped = MakeGradientGpuBuffer(640, 480, CV_32FC4);
  auto& geometry = executor.GetStage(PipelineStageName::Geometry_Adjustment);
  geometry.SetInputImage(cropped);
  geometry.SetOutputCacheValid(false);
  auto cropped_out = geometry.ApplyStage(executor.GetGlobalParams());
  ASSERT_NE(cropped_out, nullptr);
  ASSERT_TRUE(cropped_out->gpu_data_valid_);
  EXPECT_LT(cropped_out->GetGPUWidth(), 640);
  EXPECT_LT(cropped_out->GetGPUHeight(), 480);

  // Open Geometry panel: keep crop params installed but disable the operator so
  // the preview is the full source frame under the crop overlay.
  DisableEditorGeometryOperatorForOverlay(executor);
  EXPECT_FALSE(executor.GetStage(PipelineStageName::Geometry_Adjustment)
                   .GetOperator(OperatorType::CROP_ROTATE)
                   .value()
                   ->enable_);
  const auto crop_params =
      executor.GetStage(PipelineStageName::Geometry_Adjustment)
          .GetOperator(OperatorType::CROP_ROTATE)
          .value()
          ->op_->GetParams();
  EXPECT_FLOAT_EQ(crop_params["crop_rotate"]["angle_degrees"].get<float>(), 12.5f);

  auto full_frame = MakeGradientGpuBuffer(640, 480, CV_32FC4);
  geometry.SetInputImage(full_frame);
  geometry.SetOutputCacheValid(false);
  auto overlay_out = geometry.ApplyStage(executor.GetGlobalParams());
  ASSERT_NE(overlay_out, nullptr);
  ASSERT_TRUE(overlay_out->gpu_data_valid_);
  // Source-frame overlay must not bake the crop; max-edge 256 keeps the long
  // side at 256 while preserving aspect.
  EXPECT_EQ(overlay_out->GetGPUWidth(), 256);
  EXPECT_EQ(overlay_out->GetGPUHeight(), 192);
}

TEST_F(EditorGeometryOverlayPipelineTest,
     OverlayPreviewWithRotationParamsStillSkipsWarpAndOnlyResizes) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  CPUPipelineExecutor executor;
  executor.ResetToCleanBaselineAdjustments();
  executor.SetAcceleratorBackendPreference(AcceleratorBackendPreference::CUDA);
  executor.SetExecutionStages();
  executor.SetResizeDownsampleAlgorithm(ResizeDownsampleAlgorithm::Bilinear);
  executor.SetRenderRegion(0, 0, 1.0f, 1.0f);
  executor.SetRenderRes(false, 128);

  EditorRenderAdjustmentSnapshot snapshot;
  snapshot.patches = {alcedo::test::SnapshotPatch({
      "crop_rotate",
      R"({"crop_rotate":{"enabled":true,"angle_degrees":35.0,"enable_crop":true,"crop_rect":{"x":0.1,"y":0.1,"w":0.8,"h":0.8}}})",
      false})};
  std::string error;
  ASSERT_TRUE(ApplyEditorAdjustmentSnapshot(executor, snapshot, &error)) << error;
  DisableEditorGeometryOperatorForOverlay(executor);

  auto input = MakeGradientGpuBuffer(320, 240, CV_32FC3);
  auto& geometry = executor.GetStage(PipelineStageName::Geometry_Adjustment);
  geometry.SetInputImage(input);
  geometry.SetOutputCacheValid(false);
  auto out = geometry.ApplyStage(executor.GetGlobalParams());
  ASSERT_NE(out, nullptr);
  ASSERT_TRUE(out->gpu_data_valid_);
  EXPECT_EQ(out->GetGPUWidth(), 128);
  EXPECT_EQ(out->GetGPUHeight(), 96);
}

// Adversarial ResizeOp GPU cases that ordinary unit tests skip: ROI of a shared
// GpuMat, bilinear downscale (the overlay FAST_PREVIEW algorithm), odd sizes.
TEST(ResizeOpCudaOverlayCases, BilinearRoiDownscaleOnSharedGpuMatDoesNotAbort) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  auto buffer = MakeGradientGpuBuffer(401, 307, CV_32FC4);
  auto shared = std::make_shared<ImageBuffer>();
  shared->ShareGPUDataFrom(*buffer);

  nlohmann::json params;
  params["resize"] = {{"enable_scale", true},
                      {"maximum_edge", 97},
                      {"enable_roi", true},
                      {"downsample_algorithm", "bilinear"},
                      {"roi",
                       {{"x", 17},
                        {"y", 11},
                        {"resize_factor_x", 0.63f},
                        {"resize_factor_y", 0.41f},
                        {"resize_factor", 0.63f},
                        {"reference_width", 401},
                        {"reference_height", 307}}}};

  ResizeOp op(params);
  EXPECT_NO_THROW(op.ApplyGPU(shared));
  ASSERT_TRUE(shared->gpu_data_valid_);
  EXPECT_GT(shared->GetGPUWidth(), 0);
  EXPECT_GT(shared->GetGPUHeight(), 0);
  EXPECT_LE(std::max(shared->GetGPUWidth(), shared->GetGPUHeight()), 97);
}

TEST(CudaGeometryOpsOverlayCases, ResizeLinearHandlesRoiAndEmptyWithoutAbort) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  const cv::Mat host = cv::Mat::zeros(255, 511, CV_32FC3);
  cv::cuda::GpuMat src(host);
  cv::cuda::GpuMat roi = src(cv::Rect(3, 5, 127, 63));
  cv::cuda::GpuMat dst;
  EXPECT_NO_THROW(CUDA::ResizeLinear(roi, dst, cv::Size(41, 19)));
  EXPECT_EQ(dst.cols, 41);
  EXPECT_EQ(dst.rows, 19);

  cv::cuda::GpuMat empty;
  cv::cuda::GpuMat empty_dst;
  EXPECT_NO_THROW(CUDA::ResizeLinear(empty, empty_dst, cv::Size(16, 16)));
  EXPECT_TRUE(empty_dst.empty());
}

}  // namespace
}  // namespace alcedo
