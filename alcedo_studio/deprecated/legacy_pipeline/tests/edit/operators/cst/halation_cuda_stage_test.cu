//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <opencv2/core.hpp>
#include <opencv2/core/cuda.hpp>

#include "edit/operators/GPU_kernels/cst.cuh"
#include "edit/operators/GPU_kernels/halation.cuh"
#include "edit/operators/cst/halation_op.hpp"
#include "edit/operators/cst/odt_op.hpp"
#include "edit/pipeline/cuda_output_texture_tail.cuh"
#include "edit/pipeline/default_pipeline_params.hpp"
#include "edit/pipeline/gpu_scheduler.cuh"
#include "edit/pipeline/kernel_stream_gpu.cuh"
#include "image/image_buffer.hpp"

namespace alcedo {
namespace {

using OutputOnlyStream =
    CUDA::GPU_StaticKernelStream<CUDA::GPU_PointChain<CUDA::GPU_OUTPUT_Kernel>>;
using OutputHalationStream =
    CUDA::GPU_StaticKernelStream<CUDA::GPU_PointChain<CUDA::GPU_OUTPUT_Kernel>,
                                 CUDA::GPU_HalationBlurHorizontalKernel,
                                 CUDA::GPU_HalationApplyVerticalKernel>;
using DirectHalationStream = CUDA::GPU_StaticKernelStream<CUDA::GPU_HalationBlurHorizontalKernel,
                                                          CUDA::GPU_HalationApplyVerticalKernel>;

struct HalationRunResult {
  cv::Mat image;
  size_t  scratch_bytes = 0;
};

auto EnsureCudaDevice() -> bool {
  const int device_count = cv::cuda::getCudaEnabledDeviceCount();
  if (device_count <= 0) {
    return false;
  }
  cv::cuda::setDevice(0);
  return true;
}

auto AcesccEncode(float linear) -> float {
  constexpr float kLog2Denorm   = -16.0f;
  constexpr float kDenormTrans  = 0.00003051757812f;
  constexpr float kDenormOffset = 0.00001525878906f;
  constexpr float kA            = 9.72f;
  constexpr float kB            = 17.52f;

  if (linear <= 0.0f) {
    return (kLog2Denorm + kA) / kB;
  }
  if (linear < kDenormTrans) {
    return (std::log2(kDenormOffset + linear * 0.5f) + kA) / kB;
  }
  return (std::log2(linear) + kA) / kB;
}

void SetOdtParams(OperatorParams& params) {
  nlohmann::json odt_params   = pipeline_defaults::MakeDefaultODTParams();
  odt_params["odt"]["method"] = "open_drt";
  ODT_Op odt(odt_params);
  odt.SetGlobalParams(params);
}

auto MakeBrightAcesccInput() -> cv::Mat {
  cv::Mat input(5, 5, CV_32FC4);
  for (int y = 0; y < input.rows; ++y) {
    for (int x = 0; x < input.cols; ++x) {
      const float linear        = (x == 2 && y == 2) ? 40.0f : 0.05f;
      const float acescc        = AcesccEncode(linear);
      input.at<cv::Vec4f>(y, x) = cv::Vec4f(acescc, acescc, acescc, 1.0f);
    }
  }
  return input;
}

auto MakeDisplayGrayInput(int width, int height, float value) -> cv::Mat {
  cv::Mat input(height, width, CV_32FC4);
  input.setTo(cv::Scalar(value, value, value, 1.0f));
  return input;
}

auto MakeDisplayPointInput(int size, float center_value) -> cv::Mat {
  cv::Mat input(size, size, CV_32FC4);
  input.setTo(cv::Scalar(0.0f, 0.0f, 0.0f, 1.0f));
  input.at<cv::Vec4f>(size / 2, size / 2) =
      cv::Vec4f(center_value, center_value, center_value, 1.0f);
  return input;
}

auto RunOutputOnly(const cv::Mat& input) -> cv::Mat {
  OperatorParams params;
  SetOdtParams(params);

  auto             input_buffer  = std::make_shared<ImageBuffer>(input.clone());
  auto             output_buffer = std::make_shared<ImageBuffer>();

  OutputOnlyStream stream{CUDA::GPU_PointChain(CUDA::GPU_OUTPUT_Kernel())};
  CUDA::GPU_KernelLauncher<OutputOnlyStream> launcher(nullptr, stream);
  launcher.SetInputImage(input_buffer);
  launcher.SetParams(params);
  launcher.SetOutputImage(output_buffer);
  launcher.Execute();

  output_buffer->SyncToCPU();
  return output_buffer->GetCPUData().clone();
}

auto RunOutputWithHalation(const cv::Mat& input, float strength) -> cv::Mat {
  OperatorParams params;
  SetOdtParams(params);
  HalationOp({{"halation", {{"strength", strength}}}}).SetGlobalParams(params);

  auto                 input_buffer  = std::make_shared<ImageBuffer>(input.clone());
  auto                 output_buffer = std::make_shared<ImageBuffer>();

  OutputHalationStream stream{CUDA::GPU_PointChain(CUDA::GPU_OUTPUT_Kernel()),
                              CUDA::GPU_HalationBlurHorizontalKernel(),
                              CUDA::GPU_HalationApplyVerticalKernel()};
  CUDA::GPU_KernelLauncher<OutputHalationStream> launcher(nullptr, stream);
  launcher.SetInputImage(input_buffer);
  launcher.SetParams(params);
  launcher.SetOutputImage(output_buffer);
  launcher.Execute();

  output_buffer->SyncToCPU();
  return output_buffer->GetCPUData().clone();
}

auto RunHalationOnlyWithParams(const cv::Mat& input, const OperatorParams& params)
    -> HalationRunResult {
  auto                 input_buffer  = std::make_shared<ImageBuffer>(input.clone());
  auto                 output_buffer = std::make_shared<ImageBuffer>();

  DirectHalationStream stream{CUDA::GPU_HalationBlurHorizontalKernel(),
                              CUDA::GPU_HalationApplyVerticalKernel()};
  CUDA::GPU_KernelLauncher<DirectHalationStream> launcher(nullptr, stream);
  launcher.SetInputImage(input_buffer);
  auto mutable_params = params;
  launcher.SetParams(mutable_params);
  launcher.SetOutputImage(output_buffer);
  launcher.Execute();

  output_buffer->SyncToCPU();
  return {output_buffer->GetCPUData().clone(), launcher.GetAllocatedScratchBytes()};
}

auto RunHalationOnly(const cv::Mat& input, float strength) -> HalationRunResult {
  OperatorParams params;
  HalationOp({{"halation", {{"strength", strength}}}}).SetGlobalParams(params);
  return RunHalationOnlyWithParams(input, params);
}

auto MakeHalationOnlyParams(float strength, float sigma, float render_scale_x = 1.0f,
                            float render_scale_y = 1.0f) -> OperatorParams {
  OperatorParams params;
  HalationOp({{"halation", {{"strength", strength}}}}).SetGlobalParams(params);
  params.halation_.sigma_       = sigma;
  params.render_output_scale_x_ = render_scale_x;
  params.render_output_scale_y_ = render_scale_y;
  return params;
}

void ExpectExactMatch(const cv::Mat& lhs, const cv::Mat& rhs) {
  ASSERT_EQ(lhs.size(), rhs.size());
  ASSERT_EQ(lhs.type(), rhs.type());
  for (int y = 0; y < lhs.rows; ++y) {
    for (int x = 0; x < lhs.cols; ++x) {
      const cv::Vec4f a = lhs.at<cv::Vec4f>(y, x);
      const cv::Vec4f b = rhs.at<cv::Vec4f>(y, x);
      for (int c = 0; c < 4; ++c) {
        EXPECT_EQ(a[c], b[c]) << "Mismatch at (" << x << ", " << y << "), channel " << c;
      }
    }
  }
}

void ExpectNearMatch(const cv::Mat& lhs, const cv::Mat& rhs, float tolerance) {
  ASSERT_EQ(lhs.size(), rhs.size());
  ASSERT_EQ(lhs.type(), rhs.type());
  for (int y = 0; y < lhs.rows; ++y) {
    for (int x = 0; x < lhs.cols; ++x) {
      const cv::Vec4f a = lhs.at<cv::Vec4f>(y, x);
      const cv::Vec4f b = rhs.at<cv::Vec4f>(y, x);
      for (int c = 0; c < 4; ++c) {
        EXPECT_NEAR(a[c], b[c], tolerance)
            << "Mismatch at (" << x << ", " << y << "), channel " << c;
      }
    }
  }
}

auto CountChangedRgbPixels(const cv::Mat& lhs, const cv::Mat& rhs) -> int {
  int changed = 0;
  for (int y = 0; y < lhs.rows; ++y) {
    for (int x = 0; x < lhs.cols; ++x) {
      const cv::Vec4f a           = lhs.at<cv::Vec4f>(y, x);
      const cv::Vec4f b           = rhs.at<cv::Vec4f>(y, x);
      bool            rgb_changed = false;
      for (int c = 0; c < 3; ++c) {
        EXPECT_TRUE(std::isfinite(b[c]));
        rgb_changed = rgb_changed || std::abs(a[c] - b[c]) > 1.0e-6f;
      }
      EXPECT_EQ(a[3], b[3]);
      if (rgb_changed) {
        ++changed;
      }
    }
  }
  return changed;
}

}  // namespace

TEST(HalationCudaStageTest, StrengthZeroIsExactPassThroughAfterOdt) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  const cv::Mat input = MakeBrightAcesccInput();

  ExpectExactMatch(RunOutputOnly(input), RunOutputWithHalation(input, 0.0f));
}

TEST(HalationCudaStageTest, PositiveStrengthChangesDisplayEncodedOutputAfterOdt) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  const cv::Mat input    = MakeBrightAcesccInput();
  const cv::Mat odt_only = RunOutputOnly(input);
  const cv::Mat halated  = RunOutputWithHalation(input, 100.0f);

  EXPECT_GT(CountChangedRgbPixels(odt_only, halated), 0);
}

TEST(HalationCudaStageTest, DisabledStateDoesNotIncreaseScratchBufferCount) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  const cv::Mat  input = MakeDisplayPointInput(9, 1.0f);
  OperatorParams params;
  HalationOp({{"halation", {{"strength", 100.0f}}}}).SetGlobalParams(params);
  params.halation_.enabled_      = false;

  const HalationRunResult result = RunHalationOnlyWithParams(input, params);
  const size_t            expected_bytes =
      static_cast<size_t>(input.cols) * static_cast<size_t>(input.rows) * sizeof(float4);

  ExpectExactMatch(input, result.image);
  EXPECT_EQ(result.scratch_bytes, expected_bytes);
}

TEST(HalationCudaStageTest, RunsAfterDetailAndBeforeFilmGrainInOutputTextureTail) {
  constexpr auto order = CUDA::kCudaOutputTextureTailOrder;

  EXPECT_EQ(order[0], "sharpen_blur_horizontal");
  EXPECT_EQ(order[1], "sharpen_apply_vertical");
  EXPECT_EQ(order[2], "clarity_blur_horizontal");
  EXPECT_EQ(order[3], "clarity_apply_vertical");
  EXPECT_EQ(order[4], "halation_blur_horizontal");
  EXPECT_EQ(order[5], "halation_apply_vertical");
  EXPECT_EQ(order[6], "film_grain_blur_horizontal");
  EXPECT_EQ(order[7], "film_grain_apply_vertical");
}

TEST(HalationCudaStageTest, ConstantFieldHasNoLocalSpill) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  const cv::Mat input = MakeDisplayGrayInput(9, 9, 1.0f);

  ExpectExactMatch(input, RunHalationOnly(input, 100.0f).image);
}

TEST(HalationCudaStageTest, GammaEncodedConstantFieldHasNoLocalSpillInLinearLight) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  constexpr float kLinearValue = 0.18f;
  const float     encoded      = std::pow(kLinearValue, 1.0f / 2.2f);
  const cv::Mat   input        = MakeDisplayGrayInput(9, 9, encoded);

  OperatorParams  params;
  HalationOp({{"halation", {{"strength", 100.0f}}}}).SetGlobalParams(params);
  params.to_output_params_.eotf_ = ColorUtils::EOTF::GAMMA_2_2;
  params.to_output_dirty_        = true;

  ExpectNearMatch(input, RunHalationOnlyWithParams(input, params).image, 1.0e-5f);
}

TEST(HalationCudaStageTest, BrightPointCreatesRedBiasedEdgeWithoutOrangeCore) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  const cv::Mat   input  = MakeDisplayPointInput(17, 1.0f);
  const cv::Mat   output = RunHalationOnly(input, 100.0f).image;
  const cv::Vec4f center = output.at<cv::Vec4f>(8, 8);
  const cv::Vec4f halo   = output.at<cv::Vec4f>(8, 9);

  EXPECT_FLOAT_EQ(center[0], 1.0f);
  EXPECT_FLOAT_EQ(center[1], 1.0f);
  EXPECT_FLOAT_EQ(center[2], 1.0f);
  EXPECT_GT(halo[0], 0.0f);
  EXPECT_GT(halo[0], halo[1]);
  EXPECT_GT(halo[1], halo[2]);
  EXPECT_FLOAT_EQ(halo[3], 1.0f);
}

TEST(HalationCudaStageTest, StrengthIncreasesEdgeEnergyWithoutWideningSigma) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  const cv::Mat input = MakeDisplayPointInput(41, 1.0f);

  OperatorParams weak = MakeHalationOnlyParams(25.0f, 6.0f, 1.0f, 1.0f);
  OperatorParams strong = MakeHalationOnlyParams(100.0f, 6.0f, 1.0f, 1.0f);

  const cv::Mat weak_output = RunHalationOnlyWithParams(input, weak).image;
  const cv::Mat strong_output = RunHalationOnlyWithParams(input, strong).image;
  const cv::Vec4f weak_halo = weak_output.at<cv::Vec4f>(20, 22);
  const cv::Vec4f strong_halo = strong_output.at<cv::Vec4f>(20, 22);
  const cv::Vec4f weak_far = weak_output.at<cv::Vec4f>(20, 39);
  const cv::Vec4f strong_far = strong_output.at<cv::Vec4f>(20, 39);

  EXPECT_GT(strong_halo[0], weak_halo[0]);
  EXPECT_GT(strong_halo[1], weak_halo[1]);
  EXPECT_GT(strong_halo[2], weak_halo[2]);
  EXPECT_FLOAT_EQ(weak_far[0], strong_far[0]);
  EXPECT_FLOAT_EQ(weak_far[1], strong_far[1]);
  EXPECT_FLOAT_EQ(weak_far[2], strong_far[2]);
}

TEST(HalationCudaStageTest, RenderOutputScaleKeepsFullResolutionFootprintAcrossPreviewSizes) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  const cv::Mat   input         = MakeDisplayPointInput(41, 1.0f);

  OperatorParams  full_scale    = MakeHalationOnlyParams(100.0f, 8.0f, 1.0f, 1.0f);
  OperatorParams  half_scale    = MakeHalationOnlyParams(100.0f, 8.0f, 0.5f, 0.5f);

  const cv::Mat   full_output   = RunHalationOnlyWithParams(input, full_scale).image;
  const cv::Mat   half_output   = RunHalationOnlyWithParams(input, half_scale).image;
  const cv::Vec4f full_far_halo = full_output.at<cv::Vec4f>(20, 34);
  const cv::Vec4f half_far_halo = half_output.at<cv::Vec4f>(20, 34);

  EXPECT_GT(full_far_halo[0], half_far_halo[0]);
  EXPECT_GT(full_far_halo[1], half_far_halo[1]);
  EXPECT_GT(full_far_halo[2], half_far_halo[2]);
}

TEST(HalationCudaStageTest, RoiMetadataDoesNotOverrideCurrentOutputFootprint) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  const cv::Mat  input                     = MakeDisplayPointInput(25, 1.0f);

  OperatorParams local_render              = MakeHalationOnlyParams(100.0f, 6.0f, 1.0f, 1.0f);
  OperatorParams roi_preview               = local_render;
  roi_preview.render_roi_enabled_          = true;
  roi_preview.render_roi_x_                = 1200;
  roi_preview.render_roi_y_                = 700;
  roi_preview.render_roi_scale_x_          = 0.25f;
  roi_preview.render_roi_scale_y_          = 0.25f;
  roi_preview.render_roi_reference_width_  = 4000;
  roi_preview.render_roi_reference_height_ = 3000;

  ExpectExactMatch(RunHalationOnlyWithParams(input, local_render).image,
                   RunHalationOnlyWithParams(input, roi_preview).image);
}

TEST(HalationCudaStageTest, HdrAndNegativeInputsStayFinite) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  cv::Mat input(2, 2, CV_32FC4);
  input.at<cv::Vec4f>(0, 0) = cv::Vec4f(-1.0f, -0.5f, -0.25f, 1.0f);
  input.at<cv::Vec4f>(0, 1) = cv::Vec4f(0.8f, 1.5f, 2.0f, 0.5f);
  input.at<cv::Vec4f>(1, 0) = cv::Vec4f(4.0f, 0.2f, -0.1f, 0.25f);
  input.at<cv::Vec4f>(1, 1) = cv::Vec4f(16.0f, 8.0f, 2.0f, 0.0f);

  const cv::Mat output      = RunHalationOnly(input, 100.0f).image;
  for (int y = 0; y < output.rows; ++y) {
    for (int x = 0; x < output.cols; ++x) {
      const cv::Vec4f pixel = output.at<cv::Vec4f>(y, x);
      for (int channel = 0; channel < 4; ++channel) {
        EXPECT_TRUE(std::isfinite(pixel[channel]));
      }
    }
  }
}

TEST(HalationCudaStageTest, AlphaIsPreserved) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  cv::Mat input = MakeDisplayPointInput(5, 1.0f);
  for (int y = 0; y < input.rows; ++y) {
    for (int x = 0; x < input.cols; ++x) {
      input.at<cv::Vec4f>(y, x)[3] = static_cast<float>(x + y) / 8.0f;
    }
  }

  const cv::Mat output = RunHalationOnly(input, 100.0f).image;
  for (int y = 0; y < input.rows; ++y) {
    for (int x = 0; x < input.cols; ++x) {
      EXPECT_FLOAT_EQ(output.at<cv::Vec4f>(y, x)[3], input.at<cv::Vec4f>(y, x)[3]);
    }
  }
}

}  // namespace alcedo
