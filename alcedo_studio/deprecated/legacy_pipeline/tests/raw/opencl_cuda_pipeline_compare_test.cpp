//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <opencv2/core.hpp>
#include <opencv2/core/cuda.hpp>
#include <stdexcept>
#include <string>
#include <vector>

#include "edit/operators/geometry/lens_calib_op.hpp"
#include "edit/operators/geometry/resize_op.hpp"
#include "edit/operators/raw/raw_decode_op.hpp"
#include "edit/pipeline/default_pipeline_params.hpp"
#include "image/image_buffer.hpp"
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_runtime.hpp"

namespace alcedo {
namespace {

using ProfileClock                           = std::chrono::steady_clock;

constexpr int kFastPreviewMaxLongEdge        = 2560;
constexpr int kDetailRoiPreviewMaxLongEdge   = 4096;
constexpr int kQualityBasePreviewMaxLongEdge = 4096;

struct DiffStats {
  float  max_diff     = 0.0f;
  double mean_abs     = 0.0;
  int    x            = 0;
  int    y            = 0;
  int    channel      = 0;
  float  cuda_value   = 0.0f;
  float  opencl_value = 0.0f;
};

struct DecodedFrame {
  std::shared_ptr<ImageBuffer> buffer;
  OperatorParams               global_params;
  double                       decode_ms = 0.0;
};

struct RenderScenario {
  const char*               name;
  int                       region_x;
  int                       region_y;
  float                     scale_x;
  float                     scale_y;
  bool                      enable_roi;
  int                       maximum_edge;
  ResizeDownsampleAlgorithm downsample_algorithm;
  float                     max_tolerance;
  double                    mean_tolerance;
};

auto EnsureCudaDevice() -> bool {
  const int device_count = cv::cuda::getCudaEnabledDeviceCount();
  if (device_count <= 0) {
    return false;
  }
  cv::cuda::setDevice(0);
  return true;
}

auto EnsureOpenClRuntime() -> bool {
  if (TryPrepareOpenClRuntime()) {
    return true;
  }
  return OpenClContext::Instance().IsInitialized();
}

void SynchronizeCuda() {
  const cudaError_t error = cudaDeviceSynchronize();
  if (error != cudaSuccess) {
    throw std::runtime_error(std::string("cudaDeviceSynchronize failed: ") +
                             cudaGetErrorString(error));
  }
}

auto ElapsedMs(const ProfileClock::time_point start) -> double {
  return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(ProfileClock::now() -
                                                                               start)
      .count();
}

auto ReadFileToBuffer(const std::filesystem::path& path) -> std::vector<std::uint8_t> {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("Unable to open RAW fixture: " + path.string());
  }
  stream.seekg(0, std::ios::end);
  const std::streamoff size = stream.tellg();
  stream.seekg(0, std::ios::beg);

  std::vector<std::uint8_t> buffer(static_cast<size_t>(size));
  if (!buffer.empty()) {
    stream.read(reinterpret_cast<char*>(buffer.data()), size);
  }
  return buffer;
}

auto MakeRawDecodeParams() -> nlohmann::json {
  // The decode backend is a runtime property, never a param: it is pushed on
  // the op via SetRuntimeGpuBackend, like the pipeline executor does.
  nlohmann::json params;
  params["raw"] = {{"highlights_reconstruct", true},
                   {"use_camera_wb", true},
                   {"backend", "alcedo"},
                   {"decode_res", static_cast<int>(DecodeRes::FULL)}};
  return params;
}

auto DecodeRawFrame(const std::filesystem::path& raw_path,
                    GpuBackendKind expected_backend) -> DecodedFrame {
  auto        input = std::make_shared<ImageBuffer>(ReadFileToBuffer(raw_path));

  RawDecodeOp raw_decode_op(MakeRawDecodeParams());
  raw_decode_op.SetRuntimeGpuBackend(expected_backend);
  const auto  start = ProfileClock::now();
  raw_decode_op.Apply(input);
  const double decode_ms = ElapsedMs(start);

  if (!input->gpu_data_valid_) {
    throw std::runtime_error("RAW decode did not produce GPU data.");
  }
  if (input->GetGPUBackend() != expected_backend) {
    throw std::runtime_error("RAW decode produced an unexpected GPU backend.");
  }
  if (input->GetGPUType() != CV_32FC4 || input->GetGPUWidth() <= 0 || input->GetGPUHeight() <= 0) {
    throw std::runtime_error("RAW decode produced an invalid RGBA32F frame.");
  }

  OperatorParams global_params;
  raw_decode_op.SetGlobalParams(global_params);
  if (!global_params.raw_runtime_valid_ ||
      global_params.raw_decode_input_space_ != RawDecodeInputSpace::CAMERA) {
    throw std::runtime_error("RAW decode did not publish valid camera-space runtime metadata.");
  }

  return {.buffer = input, .global_params = global_params, .decode_ms = decode_ms};
}

auto MakeLensCalibParams() -> nlohmann::json {
  auto  params             = pipeline_defaults::MakeDefaultLensCalibParams();
  auto& lens               = params["lens_calib"];
  lens["enabled"]          = true;
  lens["apply_vignetting"] = true;
  lens["apply_distortion"] = true;
  lens["apply_tca"]        = true;
  lens["apply_crop"]       = false;
  lens["auto_scale"]       = true;
  lens["use_user_scale"]   = false;
  lens["lens_profile_db_path"] =
      (std::filesystem::path("alcedo_studio") / "src" / "config" / "lens_calib").string();
  return params;
}

auto MakeResizeParams(const RenderScenario& scenario, int reference_width, int reference_height)
    -> nlohmann::json {
  nlohmann::json params;
  params["resize"] = {
      {"enable_scale", true},
      {"maximum_edge", scenario.maximum_edge},
      {"enable_roi", scenario.enable_roi},
      {"downsample_algorithm", scenario.downsample_algorithm == ResizeDownsampleAlgorithm::Bilinear
                                   ? "bilinear"
                                   : "inter_area"},
      {"roi",
       {{"x", scenario.region_x},
        {"y", scenario.region_y},
        {"resize_factor_x", scenario.scale_x},
        {"resize_factor_y", scenario.scale_y},
        {"resize_factor", std::min(scenario.scale_x, scenario.scale_y)},
        {"reference_width", reference_width},
        {"reference_height", reference_height}}},
  };
  return params;
}

auto CopyGpuFrame(const ImageBuffer& src) -> std::shared_ptr<ImageBuffer> {
  auto copy = std::make_shared<ImageBuffer>();
  src.CopyGPUDataTo(*copy);
  return copy;
}

auto ComputeDiffStats(const cv::Mat& cuda_result, const cv::Mat& opencl_result) -> DiffStats {
  CV_Assert(cuda_result.size() == opencl_result.size());
  CV_Assert(cuda_result.type() == opencl_result.type());
  CV_Assert(CV_MAT_DEPTH(cuda_result.type()) == CV_32F);

  DiffStats stats;
  const int channels = cuda_result.channels();
  double    sum_abs  = 0.0;
  size_t    count    = 0;
  for (int y = 0; y < cuda_result.rows; ++y) {
    const float* cuda_row   = cuda_result.ptr<float>(y);
    const float* opencl_row = opencl_result.ptr<float>(y);
    for (int x = 0; x < cuda_result.cols; ++x) {
      for (int c = 0; c < channels; ++c) {
        const int   index = x * channels + c;
        const float diff  = std::abs(cuda_row[index] - opencl_row[index]);
        sum_abs += static_cast<double>(diff);
        ++count;
        if (diff > stats.max_diff) {
          stats.max_diff     = diff;
          stats.x            = x;
          stats.y            = y;
          stats.channel      = c;
          stats.cuda_value   = cuda_row[index];
          stats.opencl_value = opencl_row[index];
        }
      }
    }
  }
  stats.mean_abs = count == 0 ? 0.0 : sum_abs / static_cast<double>(count);
  return stats;
}

void ExpectPipelineOutputsNear(const RenderScenario& scenario, const cv::Mat& cuda_result,
                               const cv::Mat& opencl_result, double cuda_ms, double opencl_ms) {
  ASSERT_EQ(cuda_result.size(), opencl_result.size()) << scenario.name;
  ASSERT_EQ(cuda_result.type(), opencl_result.type()) << scenario.name;
  ASSERT_GT(cuda_result.cols, 0) << scenario.name;
  ASSERT_GT(cuda_result.rows, 0) << scenario.name;

  const DiffStats stats = ComputeDiffStats(cuda_result, opencl_result);
  std::cout << "[" << scenario.name << " Pipeline] CUDA: " << cuda_ms << " ms (" << cuda_result.cols
            << "x" << cuda_result.rows << ") | OpenCL: " << opencl_ms << " ms ("
            << opencl_result.cols << "x" << opencl_result.rows
            << ") | max_abs_diff=" << stats.max_diff << " mean_abs=" << stats.mean_abs << " at ("
            << stats.x << "," << stats.y << ") channel=" << stats.channel
            << " cuda=" << stats.cuda_value << " opencl=" << stats.opencl_value << "\n";

  EXPECT_LE(stats.max_diff, scenario.max_tolerance) << scenario.name;
  EXPECT_LE(stats.mean_abs, scenario.mean_tolerance) << scenario.name;
}

auto ApplyManualImageLoadingAndGeometry(const DecodedFrame& decoded, GpuBackendKind backend,
                                        const RenderScenario& scenario, const char* backend_label)
    -> std::pair<cv::Mat, double> {
  auto           frame  = CopyGpuFrame(*decoded.buffer);
  OperatorParams global = decoded.global_params;

  LensCalibOp    lens_op(MakeLensCalibParams());
  lens_op.SetGlobalParams(global);
  if (!global.lens_calib_runtime_valid_) {
    throw std::runtime_error(std::string(backend_label) +
                             " lens calibration runtime did not resolve.");
  }
  const auto& lens_runtime = global.lens_calib_runtime_params_;
  if (lens_runtime.apply_vignetting == 0 && lens_runtime.apply_distortion == 0 &&
      lens_runtime.apply_tca == 0 && lens_runtime.apply_crop == 0) {
    throw std::runtime_error(std::string(backend_label) +
                             " lens calibration resolved without any active correction.");
  }

  const auto start = ProfileClock::now();
  lens_op.ApplyGPU(frame);

  const int reference_width  = frame->GetGPUWidth();
  const int reference_height = frame->GetGPUHeight();
  ResizeOp  resize_op(MakeResizeParams(scenario, reference_width, reference_height));
  resize_op.ApplyGPU(frame);
  if (backend == GpuBackendKind::CUDA) {
    SynchronizeCuda();
  }
  const double elapsed_ms = ElapsedMs(start);

  frame->SyncToCPU();
  cv::Mat output = frame->GetCPUData().clone();
  return {output, elapsed_ms};
}

}  // namespace

TEST(OpenClCudaPipelineCompare, RawLensAndSchedulerGeometryStatesMatch) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#endif
#ifndef HAVE_OPENCL
  GTEST_SKIP() << "OpenCL is not enabled in this build.";
#endif

  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }
  if (!EnsureOpenClRuntime()) {
    GTEST_SKIP() << "OpenCL runtime is unavailable: "
                 << OpenClContext::Instance().LastInitializationError();
  }

  const std::filesystem::path raw_path =
      std::filesystem::path(TEST_IMG_PATH) / "raw" / "camera" / "canon" / "r8" / "IMG_0365.CR3";
  if (!std::filesystem::exists(raw_path)) {
    GTEST_SKIP() << "RAW fixture not found: " << raw_path.string();
  }

  DecodedFrame cuda_decoded = DecodeRawFrame(raw_path, GpuBackendKind::CUDA);
  DecodedFrame opencl_decoded = DecodeRawFrame(raw_path, GpuBackendKind::OpenCL);

  ASSERT_EQ(cuda_decoded.buffer->GetGPUWidth(), opencl_decoded.buffer->GetGPUWidth());
  ASSERT_EQ(cuda_decoded.buffer->GetGPUHeight(), opencl_decoded.buffer->GetGPUHeight());
  std::cout << "[RawDecode Pipeline] CUDA: " << cuda_decoded.decode_ms
            << " ms | OpenCL: " << opencl_decoded.decode_ms
            << " ms | decoded=" << cuda_decoded.buffer->GetGPUWidth() << "x"
            << cuda_decoded.buffer->GetGPUHeight() << "\n";

  const std::vector<RenderScenario> scenarios = {
      RenderScenario{.name                 = "FastPreviewRoiBilinear",
                     .region_x             = 720,
                     .region_y             = 420,
                     .scale_x              = 0.62f,
                     .scale_y              = 0.58f,
                     .enable_roi           = true,
                     .maximum_edge         = kFastPreviewMaxLongEdge,
                     .downsample_algorithm = ResizeDownsampleAlgorithm::Bilinear,
                     .max_tolerance        = 1.25e-1f,
                     .mean_tolerance       = 2.5e-3},
      RenderScenario{.name                 = "DetailRoiPreviewArea",
                     .region_x             = 1320,
                     .region_y             = 840,
                     .scale_x              = 0.42f,
                     .scale_y              = 0.38f,
                     .enable_roi           = true,
                     .maximum_edge         = kDetailRoiPreviewMaxLongEdge,
                     .downsample_algorithm = ResizeDownsampleAlgorithm::Area,
                     .max_tolerance        = 1.25e-1f,
                     .mean_tolerance       = 2.5e-3},
      RenderScenario{.name                 = "QualityBaseFullFrameArea",
                     .region_x             = 0,
                     .region_y             = 0,
                     .scale_x              = 1.0f,
                     .scale_y              = 1.0f,
                     .enable_roi           = false,
                     .maximum_edge         = kQualityBasePreviewMaxLongEdge,
                     .downsample_algorithm = ResizeDownsampleAlgorithm::Area,
                     .max_tolerance        = 1.25e-1f,
                     .mean_tolerance       = 2.5e-3},
  };

  for (const auto& scenario : scenarios) {
    auto [cuda_result, cuda_ms] =
        ApplyManualImageLoadingAndGeometry(cuda_decoded, GpuBackendKind::CUDA, scenario, "CUDA");
    auto [opencl_result, opencl_ms] = ApplyManualImageLoadingAndGeometry(
        opencl_decoded, GpuBackendKind::OpenCL, scenario, "OpenCL");
    ExpectPipelineOutputsNear(scenario, cuda_result, opencl_result, cuda_ms, opencl_ms);
  }
}

}  // namespace alcedo
