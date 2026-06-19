//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <opencv2/core.hpp>
#include <opencv2/core/cuda.hpp>

#include "edit/operators/GPU_kernels/cst.cuh"
#include "edit/operators/GPU_kernels/film_grain.cuh"
#include "edit/operators/cst/film_grain_op.hpp"
#include "edit/operators/cst/odt_op.hpp"
#include "edit/pipeline/default_pipeline_params.hpp"
#include "edit/pipeline/gpu_scheduler.cuh"
#include "edit/pipeline/kernel_stream_gpu.cuh"
#include "image/image_buffer.hpp"

namespace alcedo {
namespace {

using OutputOnlyStream =
    CUDA::GPU_StaticKernelStream<CUDA::GPU_PointChain<CUDA::GPU_OUTPUT_Kernel>>;
using OutputFilmGrainStream =
    CUDA::GPU_StaticKernelStream<CUDA::GPU_PointChain<CUDA::GPU_OUTPUT_Kernel>,
                                 CUDA::GPU_FilmGrainBlurHorizontalKernel,
                                 CUDA::GPU_FilmGrainApplyVerticalKernel>;
using DirectFilmGrainStream = CUDA::GPU_StaticKernelStream<CUDA::GPU_FilmGrainBlurHorizontalKernel,
                                                           CUDA::GPU_FilmGrainApplyVerticalKernel>;

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

auto MakeAcesccInput() -> cv::Mat {
  cv::Mat input(3, 3, CV_32FC4);
  for (int y = 0; y < input.rows; ++y) {
    for (int x = 0; x < input.cols; ++x) {
      const float r = 0.05f + 0.12f * static_cast<float>(x + y);
      const float g = 0.10f + 0.08f * static_cast<float>(x * 2 + y);
      const float b = 0.03f + 0.05f * static_cast<float>(x + y * 2);
      input.at<cv::Vec4f>(y, x) =
          cv::Vec4f(AcesccEncode(r), AcesccEncode(g), AcesccEncode(b), 1.0f);
    }
  }
  return input;
}

void SetOdtParams(OperatorParams& params) {
  nlohmann::json odt_params   = pipeline_defaults::MakeDefaultODTParams();
  odt_params["odt"]["method"] = "open_drt";
  ODT_Op odt(odt_params);
  odt.SetGlobalParams(params);
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

auto RunOutputWithFilmGrain(const cv::Mat& input, float strength) -> cv::Mat {
  OperatorParams params;
  SetOdtParams(params);
  FilmGrainOp({{"film_grain", {{"strength", strength}}}}).SetGlobalParams(params);

  auto                  input_buffer  = std::make_shared<ImageBuffer>(input.clone());
  auto                  output_buffer = std::make_shared<ImageBuffer>();

  OutputFilmGrainStream stream{CUDA::GPU_PointChain(CUDA::GPU_OUTPUT_Kernel()),
                               CUDA::GPU_FilmGrainBlurHorizontalKernel(),
                               CUDA::GPU_FilmGrainApplyVerticalKernel()};
  CUDA::GPU_KernelLauncher<OutputFilmGrainStream> launcher(nullptr, stream);
  launcher.SetInputImage(input_buffer);
  launcher.SetParams(params);
  launcher.SetOutputImage(output_buffer);
  launcher.Execute();

  output_buffer->SyncToCPU();
  return output_buffer->GetCPUData().clone();
}

auto RunFilmGrainOnlyWithParams(const cv::Mat& input, const OperatorParams& params) -> cv::Mat {
  auto                  input_buffer  = std::make_shared<ImageBuffer>(input.clone());
  auto                  output_buffer = std::make_shared<ImageBuffer>();

  DirectFilmGrainStream stream{CUDA::GPU_FilmGrainBlurHorizontalKernel(),
                               CUDA::GPU_FilmGrainApplyVerticalKernel()};
  CUDA::GPU_KernelLauncher<DirectFilmGrainStream> launcher(nullptr, stream);
  launcher.SetInputImage(input_buffer);
  auto mutable_params = params;
  launcher.SetParams(mutable_params);
  launcher.SetOutputImage(output_buffer);
  launcher.Execute();

  output_buffer->SyncToCPU();
  return output_buffer->GetCPUData().clone();
}

auto RunFilmGrainOnly(const cv::Mat& input, float strength,
                      std::uint64_t seed = 0x6a09e667f3bcc909ULL) -> cv::Mat {
  OperatorParams params;
  FilmGrainOp({{"film_grain", {{"strength", strength}}}}).SetGlobalParams(params);
  params.film_grain_.seed_ = seed;
  return RunFilmGrainOnlyWithParams(input, params);
}

auto MakeDisplayGrayInput(int width, int height, float value) -> cv::Mat {
  cv::Mat input(height, width, CV_32FC4);
  input.setTo(cv::Scalar(value, value, value, 1.0f));
  return input;
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

auto AverageChannel(const cv::Mat& image, int channel) -> float {
  double sum = 0.0;
  for (int y = 0; y < image.rows; ++y) {
    for (int x = 0; x < image.cols; ++x) {
      sum += image.at<cv::Vec4f>(y, x)[channel];
    }
  }
  return static_cast<float>(sum / static_cast<double>(image.rows * image.cols));
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

__global__ void FilmGrainDatasheetScaleProbeKernel(float signal, float* out_scales) {
  const int channel     = threadIdx.x;
  out_scales[channel]   = CUDA::FilmGrainDatasheetGranularityScale(signal, channel);
}

__global__ void FilmGrainDyeCloudProbeKernel(float4 transmittance, float4 layer_coverage,
                                             float strength, float* out_values) {
  const float4 result = CUDA::FilmGrainApplyDyeClouds(transmittance, layer_coverage, strength);
  if (threadIdx.x == 0) {
    out_values[0] = result.x;
    out_values[1] = result.y;
    out_values[2] = result.z;
  }
}

auto ReadDatasheetScales(float signal) -> std::array<float, 3> {
  std::array<float, 3> scales = {};
  float*              dev_scales = nullptr;
  cudaMalloc(&dev_scales, sizeof(float) * scales.size());
  FilmGrainDatasheetScaleProbeKernel<<<1, 3>>>(signal, dev_scales);
  cudaDeviceSynchronize();
  cudaMemcpy(scales.data(), dev_scales, sizeof(float) * scales.size(), cudaMemcpyDeviceToHost);
  cudaFree(dev_scales);
  return scales;
}

auto ReadDyeCloudValues(float red_transmittance, float green_transmittance,
                        float blue_transmittance, float red_coverage, float green_coverage,
                        float blue_coverage, float strength) -> std::array<float, 3> {
  std::array<float, 3> values = {};
  float*              dev_values = nullptr;
  cudaMalloc(&dev_values, sizeof(float) * values.size());
  FilmGrainDyeCloudProbeKernel<<<1, 1>>>(
      make_float4(red_transmittance, green_transmittance, blue_transmittance, 1.0f),
      make_float4(red_coverage, green_coverage, blue_coverage, 1.0f), strength, dev_values);
  cudaDeviceSynchronize();
  cudaMemcpy(values.data(), dev_values, sizeof(float) * values.size(), cudaMemcpyDeviceToHost);
  cudaFree(dev_values);
  return values;
}

auto ReadDyeCloudValues(float transmittance, float layer_coverage,
                        float strength) -> std::array<float, 3> {
  return ReadDyeCloudValues(transmittance, transmittance, transmittance, layer_coverage,
                            layer_coverage, layer_coverage, strength);
}

}  // namespace

TEST(FilmGrainCudaStageTest, DatasheetGranularityScaleFollowsNegativeFilmShape) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  const auto shadows    = ReadDatasheetScales(0.08f);
  const auto lower_mids = ReadDatasheetScales(0.22f);
  const auto midtones   = ReadDatasheetScales(0.50f);
  const auto highlights = ReadDatasheetScales(0.92f);

  EXPECT_GT(lower_mids[2], midtones[2]);
  EXPECT_GT(lower_mids[1], midtones[1]);
  EXPECT_GT(lower_mids[0], midtones[0]);

  EXPECT_GT(lower_mids[2], lower_mids[1]);
  EXPECT_GT(lower_mids[1], lower_mids[0]);

  for (int channel = 0; channel < 3; ++channel) {
    EXPECT_GT(lower_mids[channel], highlights[channel]);
    EXPECT_GT(shadows[channel], highlights[channel]);
    EXPECT_TRUE(std::isfinite(lower_mids[channel]));
  }
}

TEST(FilmGrainCudaStageTest, PositiveDyeDensityPerturbationAttenuatesColorRecords) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  constexpr float kTransmittance = 0.50f;
  constexpr float kStrength      = 0.20f;

  const auto thicker_dye = ReadDyeCloudValues(kTransmittance, 0.75f, kStrength);
  const auto thinner_dye = ReadDyeCloudValues(kTransmittance, 0.25f, kStrength);

  for (int channel = 0; channel < 3; ++channel) {
    EXPECT_LT(thicker_dye[channel], kTransmittance);
    EXPECT_GT(thinner_dye[channel], kTransmittance);
    EXPECT_TRUE(std::isfinite(thicker_dye[channel]));
    EXPECT_TRUE(std::isfinite(thinner_dye[channel]));
  }
}

TEST(FilmGrainCudaStageTest, SingleDyeLayerPerturbationKeepsNeutralDensityComponent) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  constexpr float kTransmittance = 0.50f;
  constexpr float kStrength      = 0.20f;

  const auto red_layer_spike =
      ReadDyeCloudValues(kTransmittance, kTransmittance, kTransmittance, 0.75f, 0.50f, 0.50f,
                         kStrength);

  EXPECT_LT(red_layer_spike[0], kTransmittance);
  EXPECT_LT(red_layer_spike[1], kTransmittance);
  EXPECT_LT(red_layer_spike[2], kTransmittance);
  EXPECT_LT(red_layer_spike[0], red_layer_spike[1]);
  EXPECT_LT(red_layer_spike[0], red_layer_spike[2]);
}

TEST(FilmGrainCudaStageTest, HighlightBrighteningIsCompressed) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  constexpr float kStrength = 0.20f;

  const auto midtone_thin_dye = ReadDyeCloudValues(0.50f, 0.25f, kStrength);
  const auto highlight_thin_dye = ReadDyeCloudValues(0.94f, 0.70f, kStrength);

  for (int channel = 0; channel < 3; ++channel) {
    const float midtone_lift = midtone_thin_dye[channel] - 0.50f;
    const float highlight_lift = highlight_thin_dye[channel] - 0.94f;
    EXPECT_GT(midtone_lift, 0.0f);
    EXPECT_GT(highlight_lift, 0.0f);
    EXPECT_LT(highlight_lift, midtone_lift * 0.35f);
  }
}

TEST(FilmGrainCudaStageTest, HighlightMixedDyeErrorsDoNotCreateBrightCyanSpeckles) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  constexpr float kHighlight = 0.94f;
  constexpr float kStrength  = 0.20f;

  const auto cyan_prone_highlight =
      ReadDyeCloudValues(kHighlight, kHighlight, kHighlight, 1.0f, 0.70f, 0.70f, kStrength);

  EXPECT_LT(cyan_prone_highlight[1] - cyan_prone_highlight[0], 0.01f);
  EXPECT_LT(cyan_prone_highlight[2] - cyan_prone_highlight[0], 0.01f);
  for (int channel = 0; channel < 3; ++channel) {
    EXPECT_LT(cyan_prone_highlight[channel], 0.955f);
    EXPECT_TRUE(std::isfinite(cyan_prone_highlight[channel]));
  }
}

TEST(FilmGrainCudaStageTest, StrengthZeroIsExactPassThroughAfterOdt) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  const cv::Mat input = MakeAcesccInput();

  ExpectExactMatch(RunOutputOnly(input), RunOutputWithFilmGrain(input, 0.0f));
}

TEST(FilmGrainCudaStageTest, PositiveStrengthChangesDisplayEncodedOutputAfterOdt) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  const cv::Mat input    = MakeAcesccInput();

  const cv::Mat odt_only = RunOutputOnly(input);
  const cv::Mat grained  = RunOutputWithFilmGrain(input, 100.0f);

  EXPECT_GT(CountChangedRgbPixels(odt_only, grained), 0);
}

TEST(FilmGrainCudaStageTest, FixedSeedIsDeterministicAcrossRepeatedLaunches) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  const cv::Mat input = MakeAcesccInput();

  ExpectExactMatch(RunOutputWithFilmGrain(input, 70.0f), RunOutputWithFilmGrain(input, 70.0f));
}

TEST(FilmGrainCudaStageTest, ConstantGrayMeanStaysCloseToInputProbability) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  constexpr float kGray  = 0.45f;
  const cv::Mat   input  = MakeDisplayGrayInput(32, 32, kGray);
  const cv::Mat   output = RunFilmGrainOnly(input, 100.0f);

  EXPECT_NEAR(AverageChannel(output, 0), kGray, 0.08f);
  EXPECT_NEAR(AverageChannel(output, 1), kGray, 0.08f);
  EXPECT_NEAR(AverageChannel(output, 2), kGray, 0.08f);
}

TEST(FilmGrainCudaStageTest, DistinctSeedsChangeTheGrainPattern) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  const cv::Mat input  = MakeDisplayGrayInput(16, 16, 0.5f);
  const cv::Mat seed_a = RunFilmGrainOnly(input, 100.0f, 0x6a09e667f3bcc909ULL);
  const cv::Mat seed_b = RunFilmGrainOnly(input, 100.0f, 0xbb67ae8584caa73bULL);

  EXPECT_GT(CountChangedRgbPixels(seed_a, seed_b), 0);
}

TEST(FilmGrainCudaStageTest, ColorChannelsUseIndependentGrainStreams) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  const cv::Mat input           = MakeDisplayGrayInput(16, 16, 0.5f);
  const cv::Mat output          = RunFilmGrainOnly(input, 100.0f);

  int           distinct_pixels = 0;
  for (int y = 0; y < output.rows; ++y) {
    for (int x = 0; x < output.cols; ++x) {
      const cv::Vec4f pixel = output.at<cv::Vec4f>(y, x);
      if (std::abs(pixel[0] - pixel[1]) > 1.0e-6f || std::abs(pixel[1] - pixel[2]) > 1.0e-6f) {
        ++distinct_pixels;
      }
    }
  }
  EXPECT_GT(distinct_pixels, 0);
}

TEST(FilmGrainCudaStageTest, RoiPreviewUsesLocalOutputGrainCoordinates) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  constexpr int   kFullSize = 32;
  constexpr int   kRoiSize  = 16;
  constexpr int   kRoiX     = 8;
  constexpr int   kRoiY     = 6;
  constexpr float kGray     = 0.5f;

  const cv::Mat   roi_input = MakeDisplayGrayInput(kRoiSize, kRoiSize, kGray);

  OperatorParams  local_params;
  FilmGrainOp({{"film_grain", {{"strength", 100.0f}}}}).SetGlobalParams(local_params);

  OperatorParams roi_params               = local_params;
  roi_params.render_roi_enabled_          = true;
  roi_params.render_roi_x_                = kRoiX;
  roi_params.render_roi_y_                = kRoiY;
  roi_params.render_roi_scale_x_          = static_cast<float>(kRoiSize) / kFullSize;
  roi_params.render_roi_scale_y_          = static_cast<float>(kRoiSize) / kFullSize;
  roi_params.render_roi_reference_width_  = kFullSize;
  roi_params.render_roi_reference_height_ = kFullSize;

  const cv::Mat local_output              = RunFilmGrainOnlyWithParams(roi_input, local_params);
  const cv::Mat roi_output                = RunFilmGrainOnlyWithParams(roi_input, roi_params);

  for (int y = 0; y < kRoiSize; ++y) {
    for (int x = 0; x < kRoiSize; ++x) {
      const cv::Vec4f local_pixel = local_output.at<cv::Vec4f>(y, x);
      const cv::Vec4f roi_pixel   = roi_output.at<cv::Vec4f>(y, x);
      for (int channel = 0; channel < 4; ++channel) {
        EXPECT_FLOAT_EQ(local_pixel[channel], roi_pixel[channel])
            << "Mismatch at ROI-local (" << x << ", " << y << "), channel " << channel;
      }
    }
  }
}

TEST(FilmGrainCudaStageTest, EdgeProbabilitiesAndHdrInputsStayFinite) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  cv::Mat input(2, 2, CV_32FC4);
  input.at<cv::Vec4f>(0, 0) = cv::Vec4f(0.0f, 0.0f, 0.0f, 1.0f);
  input.at<cv::Vec4f>(0, 1) = cv::Vec4f(0.5f, 0.5f, 0.5f, 1.0f);
  input.at<cv::Vec4f>(1, 0) = cv::Vec4f(1.0f, 1.0f, 1.0f, 1.0f);
  input.at<cv::Vec4f>(1, 1) = cv::Vec4f(1.5f, 1.25f, 2.0f, 1.0f);

  const cv::Mat output      = RunFilmGrainOnly(input, 100.0f);
  for (int y = 0; y < output.rows; ++y) {
    for (int x = 0; x < output.cols; ++x) {
      const cv::Vec4f pixel = output.at<cv::Vec4f>(y, x);
      for (int channel = 0; channel < 4; ++channel) {
        EXPECT_TRUE(std::isfinite(pixel[channel]));
      }
      EXPECT_EQ(pixel[3], 1.0f);
    }
  }
}

}  // namespace alcedo
