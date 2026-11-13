//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "opencl/opencl_geometry_utils.hpp"

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <opencv2/core.hpp>
#include <opencv2/core/cuda.hpp>
#include <opencv2/imgproc.hpp>
#include <string>
#include <vector>

#include "decoders/dng_default_crop.hpp"
#include "decoders/processor/operators/gpu/cuda_dng_warp.hpp"
#include "edit/runtime/lens/cuda/cuda_geometry_ops.hpp"
#include "image/image_buffer.hpp"
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_runtime.hpp"

namespace alcedo {
namespace {

struct DiffStats {
  float max_diff = 0.0f;
  int   x        = 0;
  int   y        = 0;
  int   channel  = 0;
  float a_value  = 0.0f;
  float b_value  = 0.0f;
};

auto EnsureOpenClRuntime() -> bool {
  if (TryPrepareOpenClRuntime()) {
    return true;
  }
  return OpenClContext::Instance().IsInitialized();
}

auto EnsureCudaDevice() -> bool {
#ifndef HAVE_CUDA
  return false;
#else
  const int device_count = cv::cuda::getCudaEnabledDeviceCount();
  if (device_count <= 0) {
    return false;
  }
  cv::cuda::setDevice(0);
  return true;
#endif
}

auto OpenClAndCudaSkipReason() -> std::string {
  if (!EnsureOpenClRuntime()) {
    const std::string error = OpenClContext::Instance().LastInitializationError();
    return error.empty() ? "OpenCL runtime is unavailable in this environment." : error;
  }
  if (!EnsureCudaDevice()) {
    return "CUDA device is unavailable in this environment.";
  }
  return {};
}

void RequireOpenClAndCuda() {
  if (const std::string skip_reason = OpenClAndCudaSkipReason(); !skip_reason.empty()) {
    GTEST_SKIP() << skip_reason;
  }
}

auto MakePattern(int width, int height, int type) -> cv::Mat {
  cv::Mat   image(height, width, type);
  const int channels = CV_MAT_CN(type);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      float* pixel = image.ptr<float>(y) + x * channels;
      for (int c = 0; c < channels; ++c) {
        const float gradient = static_cast<float>((x * 17 + y * 29 + c * 11) % 257) / 257.0f;
        pixel[c]             = gradient + static_cast<float>(c) * 0.03125f;
      }
    }
  }
  return image;
}

auto ComputeMaxAbsDiff(const cv::Mat& a, const cv::Mat& b) -> DiffStats {
  CV_Assert(a.size() == b.size());
  CV_Assert(a.type() == b.type());
  CV_Assert(CV_MAT_DEPTH(a.type()) == CV_32F);

  DiffStats stats;
  const int channels = a.channels();
  for (int y = 0; y < a.rows; ++y) {
    const float* row_a = a.ptr<float>(y);
    const float* row_b = b.ptr<float>(y);
    for (int x = 0; x < a.cols; ++x) {
      for (int c = 0; c < channels; ++c) {
        const int   index = x * channels + c;
        const float diff  = std::abs(row_a[index] - row_b[index]);
        if (diff > stats.max_diff) {
          stats.max_diff = diff;
          stats.x        = x;
          stats.y        = y;
          stats.channel  = c;
          stats.a_value  = row_a[index];
          stats.b_value  = row_b[index];
        }
      }
    }
  }
  return stats;
}

void ExpectGeometryNear(const char* label, const cv::Mat& opencl_result, const cv::Mat& cuda_result,
                        float tolerance, double cuda_ms = -1.0, double opencl_ms = -1.0) {
  ASSERT_EQ(opencl_result.cols, cuda_result.cols) << label;
  ASSERT_EQ(opencl_result.rows, cuda_result.rows) << label;
  ASSERT_EQ(opencl_result.type(), cuda_result.type()) << label;

  const DiffStats diff_stats = ComputeMaxAbsDiff(cuda_result, opencl_result);
  std::cout << "[" << label << " Compare] CUDA";
  if (cuda_ms >= 0.0) {
    std::cout << ": " << cuda_ms << " ms";
  }
  std::cout << " (" << cuda_result.cols << "x" << cuda_result.rows << ") | OpenCL";
  if (opencl_ms >= 0.0) {
    std::cout << ": " << opencl_ms << " ms";
  }
  std::cout << " (" << opencl_result.cols << "x" << opencl_result.rows
            << ") | max_abs_diff: " << diff_stats.max_diff << " at (" << diff_stats.x << ","
            << diff_stats.y << ") channel=" << diff_stats.channel << " cuda=" << diff_stats.a_value
            << " opencl=" << diff_stats.b_value << "\n";

  EXPECT_LE(diff_stats.max_diff, tolerance)
      << label << " OpenCL output differs from CUDA beyond tolerance.";
}

void SynchronizeCuda() {
  const cudaError_t error = cudaDeviceSynchronize();
  if (error != cudaSuccess) {
    throw std::runtime_error(std::string("cudaDeviceSynchronize failed: ") +
                             cudaGetErrorString(error));
  }
}

auto MeasureAverageMs(const std::function<void()>& run_once, int warmup_count, int iteration_count)
    -> double {
  for (int i = 0; i < warmup_count; ++i) {
    run_once();
  }

  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < iteration_count; ++i) {
    run_once();
  }
  const auto end = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start)
             .count() /
         static_cast<double>(iteration_count);
}

auto RunOpenClCropResize(const cv::Mat& src, const cv::Rect& crop_rect, cv::Size dst_size,
                         ResizeDownsampleAlgorithm algorithm) -> cv::Mat {
  opencl::OpenClImage src_image;
  src_image.Upload(src);
  opencl::OpenClImage dst_image;
  OpenCL::Geometry::CropResize(src_image, dst_image, crop_rect, dst_size, algorithm);
  cv::Mat output;
  dst_image.Download(output);
  return output;
}

auto RunCudaCropResize(const cv::Mat& src, const cv::Rect& crop_rect, cv::Size dst_size,
                       ResizeDownsampleAlgorithm algorithm) -> cv::Mat {
  cv::cuda::GpuMat src_image(src);
  cv::cuda::GpuMat roi_image = src_image(crop_rect);
  cv::cuda::GpuMat dst_image;
  if (dst_size == crop_rect.size()) {
    dst_image = roi_image;
  } else if (algorithm == ResizeDownsampleAlgorithm::Bilinear) {
    CUDA::ResizeLinear(roi_image, dst_image, dst_size);
  } else {
    CUDA::ResizeAreaApprox(roi_image, dst_image, dst_size);
  }
  SynchronizeCuda();
  cv::Mat output;
  dst_image.download(output);
  return output;
}

auto RunOpenClRotate(const cv::Mat& src, int rotate_code) -> cv::Mat {
  opencl::OpenClImage image;
  image.Upload(src);
  switch (rotate_code) {
    case cv::ROTATE_180:
      OpenCL::Geometry::Rotate180(image);
      break;
    case cv::ROTATE_90_CLOCKWISE:
      OpenCL::Geometry::Rotate90CW(image);
      break;
    case cv::ROTATE_90_COUNTERCLOCKWISE:
      OpenCL::Geometry::Rotate90CCW(image);
      break;
    default:
      throw std::runtime_error("Unsupported rotate code");
  }
  cv::Mat output;
  image.Download(output);
  return output;
}

auto RunOpenClWarpAffine(const cv::Mat& src, const cv::Mat& matrix, cv::Size out_size,
                         const cv::Scalar& border_value) -> cv::Mat {
  opencl::OpenClImage src_image;
  src_image.Upload(src);
  opencl::OpenClImage dst_image;
  OpenCL::Geometry::WarpAffineLinear(src_image, dst_image, matrix, out_size, border_value);
  cv::Mat output;
  dst_image.Download(output);
  return output;
}

auto RunOpenClDngWarp(const cv::Mat& src, const dng::WarpRectilinear& warp) -> cv::Mat {
  opencl::OpenClImage src_image;
  src_image.Upload(src);
  opencl::OpenClImage dst_image;
  OpenCL::Geometry::WarpRectilinear(src_image, dst_image, warp);
  cv::Mat output;
  dst_image.Download(output);
  return output;
}

auto RunCudaDngWarp(const cv::Mat& src, const dng::WarpRectilinear& warp) -> cv::Mat {
  cv::cuda::GpuMat image(src);
  CUDA::ApplyDngWarpRectilinear(image, warp);
  SynchronizeCuda();
  cv::Mat output;
  image.download(output);
  return output;
}

auto RunCudaWarpAffine(const cv::Mat& src, const cv::Mat& matrix, cv::Size out_size,
                       const cv::Scalar& border_value) -> cv::Mat {
  cv::cuda::GpuMat src_image(src);
  cv::cuda::GpuMat dst_image;
  CUDA::WarpAffineLinear(src_image, dst_image, matrix, out_size, border_value);
  SynchronizeCuda();
  cv::Mat output;
  dst_image.download(output);
  return output;
}

auto DownloadOpenCl(const opencl::OpenClImage& image) -> cv::Mat {
  cv::Mat output;
  image.Download(output);
  return output;
}

auto DownloadCuda(const cv::cuda::GpuMat& image) -> cv::Mat {
  cv::Mat output;
  image.download(output);
  return output;
}

}  // namespace

TEST(OpenClGeometryUtilsTest, CropResizeCopiesCudaRoi) {
  RequireOpenClAndCuda();

  const cv::Mat  src = MakePattern(6, 5, CV_32FC3);
  const cv::Rect roi(2, 1, 3, 2);
  const cv::Mat  actual =
      RunOpenClCropResize(src, roi, roi.size(), ResizeDownsampleAlgorithm::Bilinear);
  const cv::Mat expected =
      RunCudaCropResize(src, roi, roi.size(), ResizeDownsampleAlgorithm::Bilinear);

  ExpectGeometryNear("CropResizeRoiCopy", actual, expected, 0.0f);
}

TEST(OpenClGeometryUtilsTest, CropResizeAreaMatchesCudaRoiResize) {
  RequireOpenClAndCuda();

  const cv::Mat  src = MakePattern(8, 6, CV_32FC4);
  const cv::Rect roi(1, 1, 6, 4);
  const cv::Size dst_size(3, 2);

  const cv::Mat  actual = RunOpenClCropResize(src, roi, dst_size, ResizeDownsampleAlgorithm::Area);
  const cv::Mat  expected = RunCudaCropResize(src, roi, dst_size, ResizeDownsampleAlgorithm::Area);

  ExpectGeometryNear("CropResizeAreaRoi", actual, expected, 1.0e-5f);
}

TEST(OpenClGeometryUtilsTest, ResizeLinearMatchesCudaForUpsampleAndDownsample) {
  RequireOpenClAndCuda();

  struct Case {
    const char* name;
    cv::Size    src_size;
    cv::Size    dst_size;
    int         type;
  };

  const std::vector<Case> cases = {
      {"ResizeLinearUpsample3C", cv::Size(7, 5), cv::Size(13, 9), CV_32FC3},
      {"ResizeLinearDownsample4C", cv::Size(11, 9), cv::Size(6, 4), CV_32FC4},
      {"ResizeLinearSingleChannel", cv::Size(9, 7), cv::Size(5, 11), CV_32FC1},
  };

  for (const Case& test_case : cases) {
    const cv::Mat src =
        MakePattern(test_case.src_size.width, test_case.src_size.height, test_case.type);
    const cv::Mat actual =
        RunOpenClCropResize(src, cv::Rect(0, 0, src.cols, src.rows), test_case.dst_size,
                            ResizeDownsampleAlgorithm::Bilinear);
    const cv::Mat expected =
        RunCudaCropResize(src, cv::Rect(0, 0, src.cols, src.rows), test_case.dst_size,
                          ResizeDownsampleAlgorithm::Bilinear);
    ExpectGeometryNear(test_case.name, actual, expected, 1.0e-5f);
  }
}

TEST(OpenClGeometryUtilsTest, WarpAffineLinearMatchesCuda) {
  RequireOpenClAndCuda();

  const cv::Mat     src = MakePattern(9, 7, CV_32FC4);
  const cv::Point2f center(4.0f, 3.0f);
  const cv::Mat     forward = cv::getRotationMatrix2D(center, 17.0, 1.0);
  cv::Mat           matrix;
  cv::invertAffineTransform(forward, matrix);
  matrix.convertTo(matrix, CV_32F);

  const cv::Size   out_size(11, 9);
  const cv::Scalar border(0.25, 0.5, 0.75, 1.0);
  const cv::Mat    actual   = RunOpenClWarpAffine(src, matrix, out_size, border);
  const cv::Mat    expected = RunCudaWarpAffine(src, matrix, out_size, border);

  ExpectGeometryNear("WarpAffineLinear", actual, expected, 1.0e-5f);
}

TEST(OpenClGeometryUtilsTest, WarpRectilinearMatchesCuda) {
  RequireOpenClAndCuda();

  dng::WarpRectilinear warp{};
  warp.coefficient_set_count = 3;
  warp.center_x              = 0.48;
  warp.center_y              = 0.53;
  warp.coefficient_sets[0]   = {1.0, -0.08, 0.012, -0.002, 0.0015, -0.001};
  warp.coefficient_sets[1]   = {1.0, -0.06, 0.010, -0.001, 0.0010, -0.0005};
  warp.coefficient_sets[2]   = {1.0, -0.04, 0.008, -0.001, 0.0005, -0.0002};

  const std::vector<std::pair<const char*, int>> cases = {
      {"WarpRectilinear3C", CV_32FC3},
      {"WarpRectilinear4C", CV_32FC4},
  };

  for (const auto& [name, type] : cases) {
    const cv::Mat src      = MakePattern(17, 13, type);
    const cv::Mat actual   = RunOpenClDngWarp(src, warp);
    const cv::Mat expected = RunCudaDngWarp(src, warp);
    ExpectGeometryNear(name, actual, expected, 1.0e-5f);
  }
}

TEST(OpenClGeometryUtilsTest, RotatesLikeCpuReference) {
  RequireOpenClAndCuda();

  struct Case {
    const char* name;
    int         rotate_code;
    int         type;
  };

  const std::vector<Case> cases = {
      {"Rotate1803C", cv::ROTATE_180, CV_32FC3},
      {"Rotate90CW4C", cv::ROTATE_90_CLOCKWISE, CV_32FC4},
      {"Rotate90CCW1C", cv::ROTATE_90_COUNTERCLOCKWISE, CV_32FC1},
  };

  for (const Case& test_case : cases) {
    const cv::Mat src      = MakePattern(5, 3, test_case.type);
    const cv::Mat actual   = RunOpenClRotate(src, test_case.rotate_code);
    cv::Mat       expected;
    cv::rotate(src, expected, test_case.rotate_code);
    ExpectGeometryNear(test_case.name, actual, expected, 0.0f);
  }
}

TEST(OpenClGeometryUtilsTest, GeometryKernelsReportCudaOpenClPerformance) {
  RequireOpenClAndCuda();

  constexpr int kWarmups    = 2;
  constexpr int kIterations = 8;

  {
    const cv::Mat       src = MakePattern(1920, 1080, CV_32FC4);
    const cv::Rect      roi(64, 32, 1792, 1000);
    const cv::Size      dst_size(896, 500);

    opencl::OpenClImage opencl_src;
    opencl_src.Upload(src);
    opencl::OpenClImage opencl_dst;
    cv::cuda::GpuMat    cuda_src(src);
    cv::cuda::GpuMat    cuda_roi = cuda_src(roi);
    cv::cuda::GpuMat    cuda_dst;

    const double        opencl_ms = MeasureAverageMs(
        [&] {
          OpenCL::Geometry::CropResize(opencl_src, opencl_dst, roi, dst_size,
                                              ResizeDownsampleAlgorithm::Area);
        },
        kWarmups, kIterations);

    const double cuda_ms = MeasureAverageMs(
        [&] {
          CUDA::ResizeAreaApprox(cuda_roi, cuda_dst, dst_size);
          SynchronizeCuda();
        },
        kWarmups, kIterations);

    ExpectGeometryNear("PerfCropResizeArea", DownloadOpenCl(opencl_dst), DownloadCuda(cuda_dst),
                       1.0e-5f, cuda_ms, opencl_ms);
  }

  {
    const cv::Mat     src = MakePattern(1280, 960, CV_32FC4);
    const cv::Point2f center(640.0f, 480.0f);
    const cv::Mat     forward = cv::getRotationMatrix2D(center, 8.0, 0.98);
    cv::Mat           matrix;
    cv::invertAffineTransform(forward, matrix);
    matrix.convertTo(matrix, CV_32F);
    const cv::Size      out_size(1280, 960);
    const cv::Scalar    border(0.0, 0.0, 0.0, 0.0);

    opencl::OpenClImage opencl_src;
    opencl_src.Upload(src);
    opencl::OpenClImage opencl_dst;
    cv::cuda::GpuMat    cuda_src(src);
    cv::cuda::GpuMat    cuda_dst;

    const double        opencl_ms = MeasureAverageMs(
        [&] {
          OpenCL::Geometry::WarpAffineLinear(opencl_src, opencl_dst, matrix, out_size, border);
        },
        kWarmups, kIterations);

    const double cuda_ms = MeasureAverageMs(
        [&] {
          CUDA::WarpAffineLinear(cuda_src, cuda_dst, matrix, out_size, border);
          SynchronizeCuda();
        },
        kWarmups, kIterations);

    ExpectGeometryNear("PerfWarpAffineLinear", DownloadOpenCl(opencl_dst), DownloadCuda(cuda_dst),
                       5.0e-4f, cuda_ms, opencl_ms);
  }
}

}  // namespace alcedo
