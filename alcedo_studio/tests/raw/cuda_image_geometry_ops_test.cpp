//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <opencv2/core.hpp>
#include <opencv2/core/cuda.hpp>
#include <opencv2/imgproc.hpp>

#include "decoders/processor/operators/gpu/cuda_image_ops.hpp"
#include "edit/runtime/lens/cuda/cuda_geometry_ops.hpp"
#include "edit/runtime/lens/cuda/cuda_lens_calib_ops.hpp"
#include "image/image_buffer.hpp"
#include "../edit/runtime/lens_warp_test_support.hpp"

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

auto MakeGradientMat(int rows, int cols, int type) -> cv::Mat {
  cv::Mat out(rows, cols, type);
  for (int y = 0; y < rows; ++y) {
    for (int x = 0; x < cols; ++x) {
      const float fx = static_cast<float>(x) / static_cast<float>(std::max(cols - 1, 1));
      const float fy = static_cast<float>(y) / static_cast<float>(std::max(rows - 1, 1));
      if (type == CV_32FC1) {
        out.at<float>(y, x) = 0.25f * fx + 0.75f * fy;
      } else if (type == CV_32FC3) {
        out.at<cv::Vec3f>(y, x) = cv::Vec3f(fx, fy, 0.5f * (fx + fy));
      } else if (type == CV_32FC4) {
        out.at<cv::Vec4f>(y, x) = cv::Vec4f(fx, fy, 0.5f * (fx + fy), 1.0f);
      }
    }
  }
  return out;
}

auto MeanAbsError(const cv::Mat& a, const cv::Mat& b) -> double {
  cv::Mat diff;
  cv::absdiff(a, b, diff);
  cv::Scalar m = cv::mean(diff);
  double total = 0.0;
  for (int c = 0; c < a.channels(); ++c) {
    total += m[c];
  }
  return total / static_cast<double>(a.channels());
}

auto MeanAbsErrorChannel(const cv::Mat& a, const cv::Mat& b, int channel) -> double {
  std::vector<cv::Mat> a_channels;
  std::vector<cv::Mat> b_channels;
  cv::split(a, a_channels);
  cv::split(b, b_channels);
  cv::Mat diff;
  cv::absdiff(a_channels[channel], b_channels[channel], diff);
  return cv::mean(diff)[0];
}

auto MakeIdentityLensParams(int rows, int cols) -> LensCalibGpuParams {
  LensCalibGpuParams params{};
  params.src_width  = cols;
  params.src_height = rows;
  params.dst_width  = cols;
  params.dst_height = rows;

  const float width  = static_cast<float>(std::max(cols - 1, 1));
  const float height = static_cast<float>(std::max(rows - 1, 1));
  params.norm_scale  = 1.0f / static_cast<float>(std::max(cols, rows));
  params.norm_unscale = 1.0f / params.norm_scale;
  params.center_x    = 0.5f * width * params.norm_scale;
  params.center_y    = 0.5f * height * params.norm_scale;

  params.camera_crop_factor = 1.0f;
  params.nominal_focal_mm   = 35.0f;
  params.real_focal_mm      = 35.0f;
  params.resolved_scale     = 1.0f;
  return params;
}

// The lens warp CPU reference (Lanczos-3, issue #713) is shared with the OpenCL and Metal tests.
using lens_warp_test::ApplyLensCalibCpuReference;
using lens_warp_test::ComputeRectCropRoi;

}  // namespace

TEST(CudaImageOpsTest, MergeRgbAndRgbToRgbaMatchesCpuReference) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  cv::Mat red(3, 4, CV_32FC1);
  cv::Mat green(3, 4, CV_32FC1);
  cv::Mat blue(3, 4, CV_32FC1);
  for (int y = 0; y < red.rows; ++y) {
    for (int x = 0; x < red.cols; ++x) {
      red.at<float>(y, x)   = static_cast<float>(x + y);
      green.at<float>(y, x) = static_cast<float>(x * 2 + y);
      blue.at<float>(y, x)  = static_cast<float>(x + y * 3);
    }
  }

  cv::cuda::GpuMat d_red(red);
  cv::cuda::GpuMat d_green(green);
  cv::cuda::GpuMat d_blue(blue);
  cv::cuda::GpuMat d_merged;
  CUDA::MergeRGB(d_red, d_green, d_blue, d_merged);

  cv::Mat merged_cpu;
  d_merged.download(merged_cpu);
  ASSERT_EQ(merged_cpu.type(), CV_32FC3);

  for (int y = 0; y < merged_cpu.rows; ++y) {
    for (int x = 0; x < merged_cpu.cols; ++x) {
      const cv::Vec3f v = merged_cpu.at<cv::Vec3f>(y, x);
      EXPECT_FLOAT_EQ(v[0], red.at<float>(y, x));
      EXPECT_FLOAT_EQ(v[1], green.at<float>(y, x));
      EXPECT_FLOAT_EQ(v[2], blue.at<float>(y, x));
    }
  }

  CUDA::RGBToRGBA(d_merged);
  cv::Mat rgba_cpu;
  d_merged.download(rgba_cpu);
  ASSERT_EQ(rgba_cpu.type(), CV_32FC4);
  for (int y = 0; y < rgba_cpu.rows; ++y) {
    for (int x = 0; x < rgba_cpu.cols; ++x) {
      const cv::Vec4f v = rgba_cpu.at<cv::Vec4f>(y, x);
      EXPECT_FLOAT_EQ(v[0], red.at<float>(y, x));
      EXPECT_FLOAT_EQ(v[1], green.at<float>(y, x));
      EXPECT_FLOAT_EQ(v[2], blue.at<float>(y, x));
      EXPECT_FLOAT_EQ(v[3], 1.0f);
    }
  }
}

TEST(CudaGeometryOpsTest, ResizeAreaApproxVisuallyMatchesCpuInterArea) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  const cv::Mat src = MakeGradientMat(96, 144, CV_32FC3);

  cv::cuda::GpuMat d_src(src);
  cv::cuda::GpuMat d_dst;
  CUDA::ResizeAreaApprox(d_src, d_dst, cv::Size(31, 19));

  cv::Mat gpu_out;
  d_dst.download(gpu_out);

  cv::Mat cpu_ref;
  cv::resize(src, cpu_ref, cv::Size(31, 19), 0.0, 0.0, cv::INTER_AREA);

  EXPECT_LT(MeanAbsError(gpu_out, cpu_ref), 2e-3);
}

TEST(CudaGeometryOpsTest, WarpAffineLinearVisuallyMatchesCpuWarpAffine) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  const cv::Mat src = MakeGradientMat(80, 120, CV_32FC4);

  const cv::Mat matrix = (cv::Mat_<double>(2, 3) << 0.98, -0.12, 5.0, 0.12, 0.98, -3.0);
  const cv::Size out_size(96, 64);
  const cv::Scalar border(0.0, 0.0, 0.0, 1.0);

  cv::cuda::GpuMat d_src(src);
  cv::cuda::GpuMat d_dst;
  CUDA::WarpAffineLinear(d_src, d_dst, matrix, out_size, border);

  cv::Mat gpu_out;
  d_dst.download(gpu_out);

  cv::Mat cpu_ref;
  cv::warpAffine(src, cpu_ref, matrix, out_size, cv::INTER_LINEAR | cv::WARP_INVERSE_MAP,
                 cv::BORDER_CONSTANT, border);

  EXPECT_LT(MeanAbsError(gpu_out, cpu_ref), 3e-3);
}

TEST(CudaLensCalibOpsTest, VignettingPaMatchesCpuReference) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  const cv::Mat src = MakeGradientMat(96, 120, CV_32FC4);

  LensCalibGpuParams params = MakeIdentityLensParams(src.rows, src.cols);
  params.apply_vignetting   = 1;
  params.vignetting_model   = static_cast<std::int32_t>(LensCalibVignettingModel::PA);
  params.vignetting_terms[0] = -0.12f;
  params.vignetting_terms[1] = 0.08f;
  params.vignetting_terms[2] = -0.02f;

  cv::cuda::GpuMat d_img(src.clone());
  CUDA::ApplyLensCalibration(d_img, params);

  cv::Mat gpu_out;
  d_img.download(gpu_out);

  const cv::Mat cpu_ref = ApplyLensCalibCpuReference(src, params);
  EXPECT_LT(MeanAbsError(gpu_out, cpu_ref), 3e-5);
}

TEST(CudaLensCalibOpsTest, VignettingOnCommandStreamMatchesCpuAfterWaitingThatStream) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  const cv::Mat src = MakeGradientMat(96, 120, CV_32FC4);

  LensCalibGpuParams params = MakeIdentityLensParams(src.rows, src.cols);
  params.apply_vignetting   = 1;
  params.vignetting_model   = static_cast<std::int32_t>(LensCalibVignettingModel::PA);
  params.vignetting_terms[0] = -0.12f;
  params.vignetting_terms[1] = 0.08f;
  params.vignetting_terms[2] = -0.02f;

  cv::cuda::Stream stream;
  cv::cuda::GpuMat d_img;
  d_img.upload(src, stream);
  CUDA::ApplyLensCalibration(d_img, params, &stream);

  cv::Mat gpu_out;
  d_img.download(gpu_out, stream);
  stream.waitForCompletion();

  const cv::Mat cpu_ref = ApplyLensCalibCpuReference(src, params);
  EXPECT_LT(MeanAbsError(gpu_out, cpu_ref), 3e-5);
}

TEST(CudaLensCalibOpsTest, DistortionModelsMatchCpuReference) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  const cv::Mat src = MakeGradientMat(84, 128, CV_32FC4);

  struct DistortionCase {
    LensCalibDistortionModel model;
    std::array<float, 3> terms;
    const char* name;
  };
  const std::array<DistortionCase, 3> cases = {
      DistortionCase{LensCalibDistortionModel::POLY3, {0.35f, 0.0f, 0.0f}, "poly3"},
      DistortionCase{LensCalibDistortionModel::POLY5, {0.2f, -0.35f, 0.0f}, "poly5"},
      DistortionCase{LensCalibDistortionModel::PTLENS, {0.05f, -0.08f, 0.1f}, "ptlens"},
  };

  for (const auto& item : cases) {
    LensCalibGpuParams params = MakeIdentityLensParams(src.rows, src.cols);
    params.apply_distortion   = 1;
    params.distortion_model   = static_cast<std::int32_t>(item.model);
    params.distortion_terms[0] = item.terms[0];
    params.distortion_terms[1] = item.terms[1];
    params.distortion_terms[2] = item.terms[2];

    cv::cuda::GpuMat d_img(src.clone());
    CUDA::ApplyLensCalibration(d_img, params);

    cv::Mat gpu_out;
    d_img.download(gpu_out);

    const cv::Mat cpu_ref = ApplyLensCalibCpuReference(src, params);
    EXPECT_LT(MeanAbsError(gpu_out, cpu_ref), 5e-5) << item.name;
  }
}

TEST(CudaLensCalibOpsTest, TcaLinearUsesPerChannelSampling) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  const cv::Mat src = MakeGradientMat(90, 134, CV_32FC4);

  LensCalibGpuParams params = MakeIdentityLensParams(src.rows, src.cols);
  params.apply_tca          = 1;
  params.tca_model          = static_cast<std::int32_t>(LensCalibTCAModel::LINEAR);
  params.tca_terms[0]       = 1.03f;
  params.tca_terms[1]       = 0.96f;

  cv::cuda::GpuMat d_img(src.clone());
  CUDA::ApplyLensCalibration(d_img, params);

  cv::Mat gpu_out;
  d_img.download(gpu_out);
  const cv::Mat cpu_ref = ApplyLensCalibCpuReference(src, params);

  EXPECT_LT(MeanAbsError(gpu_out, cpu_ref), 5e-5);
  EXPECT_GT(MeanAbsErrorChannel(gpu_out, src, 0), 1e-3);
  EXPECT_GT(MeanAbsErrorChannel(gpu_out, src, 2), 1e-3);
}

TEST(CudaLensCalibOpsTest, TcaPoly3UsesPerChannelSampling) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  const cv::Mat src = MakeGradientMat(92, 136, CV_32FC4);

  LensCalibGpuParams params = MakeIdentityLensParams(src.rows, src.cols);
  params.apply_tca          = 1;
  params.tca_model          = static_cast<std::int32_t>(LensCalibTCAModel::POLY3);
  params.tca_terms[0]       = 1.0f;
  params.tca_terms[1]       = 1.0f;
  params.tca_terms[2]       = 0.03f;
  params.tca_terms[3]       = -0.02f;
  params.tca_terms[4]       = 0.08f;
  params.tca_terms[5]       = -0.06f;

  cv::cuda::GpuMat d_img(src.clone());
  CUDA::ApplyLensCalibration(d_img, params);

  cv::Mat gpu_out;
  d_img.download(gpu_out);
  const cv::Mat cpu_ref = ApplyLensCalibCpuReference(src, params);

  EXPECT_LT(MeanAbsError(gpu_out, cpu_ref), 6e-5);
  EXPECT_GT(MeanAbsErrorChannel(gpu_out, src, 0), 1e-3);
  EXPECT_GT(MeanAbsErrorChannel(gpu_out, src, 2), 1e-3);
}

TEST(CudaLensCalibOpsTest, GeometryAndTcaCombinedSinglePassMatchesCpuReference) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  const cv::Mat src = MakeGradientMat(104, 156, CV_32FC4);

  LensCalibGpuParams params = MakeIdentityLensParams(src.rows, src.cols);
  params.apply_distortion   = 1;
  params.distortion_model   = static_cast<std::int32_t>(LensCalibDistortionModel::POLY5);
  params.distortion_terms[0] = 0.18f;
  params.distortion_terms[1] = -0.24f;
  params.apply_tca           = 1;
  params.tca_model           = static_cast<std::int32_t>(LensCalibTCAModel::LINEAR);
  params.tca_terms[0]        = 1.02f;
  params.tca_terms[1]        = 0.98f;

  cv::cuda::GpuMat d_img(src.clone());
  CUDA::ApplyLensCalibration(d_img, params);

  cv::Mat gpu_out;
  d_img.download(gpu_out);
  const cv::Mat cpu_ref = ApplyLensCalibCpuReference(src, params);

  EXPECT_LT(MeanAbsError(gpu_out, cpu_ref), 7e-5);
}

TEST(CudaLensCalibOpsTest, CropRectangleAdjustsOutputBounds) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  const cv::Mat src = MakeGradientMat(80, 120, CV_32FC4);

  LensCalibGpuParams params = MakeIdentityLensParams(src.rows, src.cols);
  params.apply_crop         = 1;
  params.crop_mode          = static_cast<std::int32_t>(LensCalibCropMode::RECTANGLE);
  params.crop_bounds[0]     = 0.2f;
  params.crop_bounds[1]     = 0.78f;
  params.crop_bounds[2]     = 0.1f;
  params.crop_bounds[3]     = 0.82f;

  cv::cuda::GpuMat d_img(src.clone());
  CUDA::ApplyLensCalibration(d_img, params);

  cv::Mat gpu_out;
  d_img.download(gpu_out);

  const cv::Rect roi = ComputeRectCropRoi(params);
  EXPECT_EQ(gpu_out.cols, roi.width);
  EXPECT_EQ(gpu_out.rows, roi.height);

  const cv::Mat cpu_ref = src(roi).clone();
  EXPECT_LT(MeanAbsError(gpu_out, cpu_ref), 1e-6);
}

TEST(CudaLensCalibOpsTest, CropCircleWritesAlphaMask) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  const cv::Mat src = MakeGradientMat(90, 90, CV_32FC4);

  LensCalibGpuParams params = MakeIdentityLensParams(src.rows, src.cols);
  params.apply_crop         = 1;
  params.crop_mode          = static_cast<std::int32_t>(LensCalibCropMode::CIRCLE);
  params.apply_crop_circle  = 1;
  params.crop_bounds[0]     = 0.15f;
  params.crop_bounds[1]     = 0.85f;
  params.crop_bounds[2]     = 0.15f;
  params.crop_bounds[3]     = 0.85f;

  cv::cuda::GpuMat d_img(src.clone());
  CUDA::ApplyLensCalibration(d_img, params);

  cv::Mat gpu_out;
  d_img.download(gpu_out);

  const cv::Vec4f center = gpu_out.at<cv::Vec4f>(gpu_out.rows / 2, gpu_out.cols / 2);
  const cv::Vec4f corner = gpu_out.at<cv::Vec4f>(0, 0);
  EXPECT_NEAR(center[3], 1.0f, 1e-6f);
  EXPECT_NEAR(corner[3], 0.0f, 1e-6f);
}

}  // namespace alcedo
