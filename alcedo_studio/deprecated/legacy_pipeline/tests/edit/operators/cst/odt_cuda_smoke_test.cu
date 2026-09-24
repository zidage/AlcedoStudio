//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <array>
#include <cmath>

#include <opencv2/core.hpp>
#include <opencv2/core/cuda.hpp>

#include "edit/operators/GPU_kernels/cst.cuh"
#include "edit/operators/cst/odt_op.hpp"
#include "edit/pipeline/default_pipeline_params.hpp"
#include "edit/pipeline/gpu_scheduler.cuh"
#include "edit/pipeline/kernel_stream_gpu.cuh"
#include "image/image_buffer.hpp"

namespace alcedo {
namespace {

using OutputOnlyStream = CUDA::GPU_StaticKernelStream<CUDA::GPU_PointChain<CUDA::GPU_OUTPUT_Kernel>>;

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
  cv::Mat input(2, 2, CV_32FC4);
  input.at<cv::Vec4f>(0, 0) = cv::Vec4f(
      AcesccEncode(0.18f), AcesccEncode(0.18f), AcesccEncode(0.18f), 1.0f);
  input.at<cv::Vec4f>(0, 1) = cv::Vec4f(
      AcesccEncode(1.0f), AcesccEncode(0.5f), AcesccEncode(0.25f), 1.0f);
  input.at<cv::Vec4f>(1, 0) = cv::Vec4f(
      AcesccEncode(4.0f), AcesccEncode(2.0f), AcesccEncode(1.0f), 1.0f);
  input.at<cv::Vec4f>(1, 1) = cv::Vec4f(
      AcesccEncode(0.02f), AcesccEncode(0.01f), AcesccEncode(0.005f), 1.0f);
  return input;
}

auto RunOutputKernel(const nlohmann::json& odt_params, const cv::Mat& input) -> cv::Mat {
  ODT_Op         odt(odt_params);
  OperatorParams params;
  odt.SetGlobalParams(params);

  auto input_buffer  = std::make_shared<ImageBuffer>(input.clone());
  auto output_buffer = std::make_shared<ImageBuffer>();

  OutputOnlyStream                         stream{CUDA::GPU_PointChain(CUDA::GPU_OUTPUT_Kernel())};
  CUDA::GPU_KernelLauncher<OutputOnlyStream> launcher(nullptr, stream);
  launcher.SetInputImage(input_buffer);
  launcher.SetParams(params);
  launcher.SetOutputImage(output_buffer);
  launcher.Execute();

  output_buffer->SyncToCPU();
  return output_buffer->GetCPUData().clone();
}

void ExpectFiniteOutput(const cv::Mat& output) {
  ASSERT_EQ(output.type(), CV_32FC4);
  for (int y = 0; y < output.rows; ++y) {
    for (int x = 0; x < output.cols; ++x) {
      const cv::Vec4f pixel = output.at<cv::Vec4f>(y, x);
      for (int c = 0; c < 4; ++c) {
        EXPECT_TRUE(std::isfinite(pixel[c])) << "Non-finite output at (" << x << ", " << y
                                             << "), channel " << c;
      }
    }
  }
}

}  // namespace

TEST(ODTCudaSmokeTest, OutputKernelProducesFinitePixelsForBothMethods) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  const cv::Mat input = MakeAcesccInput();

  {
    nlohmann::json params = pipeline_defaults::MakeDefaultODTParams();
    params["odt"]["method"] = "open_drt";
    const cv::Mat output = RunOutputKernel(params, input);
    ASSERT_EQ(output.rows, input.rows);
    ASSERT_EQ(output.cols, input.cols);
    ExpectFiniteOutput(output);
  }

  {
    nlohmann::json params = pipeline_defaults::MakeDefaultODTParams();
    params["odt"]["method"] = "aces_2_0";
    params["odt"]["limiting_space"] = "rec709";
    const cv::Mat output = RunOutputKernel(params, input);
    ASSERT_EQ(output.rows, input.rows);
    ASSERT_EQ(output.cols, input.cols);
    ExpectFiniteOutput(output);
  }
}

}  // namespace alcedo
