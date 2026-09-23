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
#include "decoders/processor/operators/gpu/cuda_rotate.hpp"
#include "edit/runtime/lens/cuda/cuda_geometry_ops.hpp"
#include "edit/runtime/lens/cuda/cuda_lens_calib_ops.hpp"
#include "edit/operators/geometry/lens_calib_op.hpp"
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

auto PixelToNormalizedCpu(const LensCalibGpuParams& params, float x, float y) -> cv::Point2f {
  return cv::Point2f(x * params.norm_scale - params.center_x, y * params.norm_scale - params.center_y);
}

auto NormalizedToPixelCpu(const LensCalibGpuParams& params, const cv::Point2f& p) -> cv::Point2f {
  return cv::Point2f((p.x + params.center_x) * params.norm_unscale,
                     (p.y + params.center_y) * params.norm_unscale);
}

auto ApplyScaleAndPerspectiveCpu(const LensCalibGpuParams& params, const cv::Point2f& in)
    -> cv::Point2f {
  cv::Point2f out = in;
  if (std::fabs(params.resolved_scale) > 1e-8f) {
    const float inv_scale = 1.0f / params.resolved_scale;
    out.x *= inv_scale;
    out.y *= inv_scale;
  }
  return out;
}

auto ApplyDistortionCpu(const LensCalibGpuParams& params, const cv::Point2f& in) -> cv::Point2f {
  if (params.apply_distortion == 0) {
    return in;
  }

  const float x   = in.x;
  const float y   = in.y;
  const float ru2 = x * x + y * y;
  switch (static_cast<LensCalibDistortionModel>(params.distortion_model)) {
    case LensCalibDistortionModel::POLY3: {
      const float k1   = params.distortion_terms[0];
      const float poly = 1.0f + k1 * ru2;
      return cv::Point2f(x * poly, y * poly);
    }
    case LensCalibDistortionModel::POLY5: {
      const float k1   = params.distortion_terms[0];
      const float k2   = params.distortion_terms[1];
      const float poly = 1.0f + k1 * ru2 + k2 * ru2 * ru2;
      return cv::Point2f(x * poly, y * poly);
    }
    case LensCalibDistortionModel::PTLENS: {
      const float a    = params.distortion_terms[0];
      const float b    = params.distortion_terms[1];
      const float c    = params.distortion_terms[2];
      const float r    = std::sqrt(ru2);
      const float poly = a * ru2 * r + b * ru2 + c * r + 1.0f;
      return cv::Point2f(x * poly, y * poly);
    }
    case LensCalibDistortionModel::NONE:
    default:
      return in;
  }
}

void ApplyTcaCpu(const LensCalibGpuParams& params, const cv::Point2f& in, cv::Point2f& red,
                 cv::Point2f& blue) {
  red  = in;
  blue = in;
  if (params.apply_tca == 0) {
    return;
  }

  switch (static_cast<LensCalibTCAModel>(params.tca_model)) {
    case LensCalibTCAModel::LINEAR: {
      const float kr = params.tca_terms[0];
      const float kb = params.tca_terms[1];
      red            = cv::Point2f(in.x * kr, in.y * kr);
      blue           = cv::Point2f(in.x * kb, in.y * kb);
      return;
    }
    case LensCalibTCAModel::POLY3: {
      const float vr = params.tca_terms[0];
      const float vb = params.tca_terms[1];
      const float cr = params.tca_terms[2];
      const float cb = params.tca_terms[3];
      const float br = params.tca_terms[4];
      const float bb = params.tca_terms[5];
      const float r2 = in.x * in.x + in.y * in.y;
      const float rr = std::sqrt(r2);
      const float fr = br * r2 + cr * rr + vr;
      const float fb = bb * r2 + cb * rr + vb;
      red            = cv::Point2f(in.x * fr, in.y * fr);
      blue           = cv::Point2f(in.x * fb, in.y * fb);
      return;
    }
    case LensCalibTCAModel::NONE:
    default:
      return;
  }
}

auto ReadWithBorderCpu(const cv::Mat& src, int x, int y) -> cv::Vec4f {
  if (x < 0 || y < 0 || x >= src.cols || y >= src.rows) {
    return cv::Vec4f(0.0f, 0.0f, 0.0f, 0.0f);
  }
  return src.at<cv::Vec4f>(y, x);
}

auto BilinearSampleCpu(const cv::Mat& src, float sx, float sy) -> cv::Vec4f {
  const int x0 = static_cast<int>(std::floor(sx));
  const int y0 = static_cast<int>(std::floor(sy));
  const int x1 = x0 + 1;
  const int y1 = y0 + 1;

  const float fx  = sx - static_cast<float>(x0);
  const float fy  = sy - static_cast<float>(y0);
  const float w00 = (1.0f - fx) * (1.0f - fy);
  const float w10 = fx * (1.0f - fy);
  const float w01 = (1.0f - fx) * fy;
  const float w11 = fx * fy;

  const cv::Vec4f p00 = ReadWithBorderCpu(src, x0, y0);
  const cv::Vec4f p10 = ReadWithBorderCpu(src, x1, y0);
  const cv::Vec4f p01 = ReadWithBorderCpu(src, x0, y1);
  const cv::Vec4f p11 = ReadWithBorderCpu(src, x1, y1);

  return p00 * w00 + p10 * w10 + p01 * w01 + p11 * w11;
}

auto BilinearSampleChannelCpu(const cv::Mat& src, float sx, float sy, int channel) -> float {
  return BilinearSampleCpu(src, sx, sy)[channel];
}

auto ApplyCircleCropAlphaCpu(const LensCalibGpuParams& params, const cv::Vec4f& in, int x, int y)
    -> cv::Vec4f {
  if (params.apply_crop_circle == 0) {
    return in;
  }
  const float left = params.crop_bounds[0] * static_cast<float>(params.dst_width);
  const float right = params.crop_bounds[1] * static_cast<float>(params.dst_width);
  const float top = params.crop_bounds[2] * static_cast<float>(params.dst_height);
  const float bottom = params.crop_bounds[3] * static_cast<float>(params.dst_height);

  const float cx = 0.5f * (left + right);
  const float cy = 0.5f * (top + bottom);
  const float rx = 0.5f * std::fabs(right - left);
  const float ry = 0.5f * std::fabs(bottom - top);
  const float radius = std::min(rx, ry);
  if (radius <= 1e-8f) {
    return in;
  }

  const float dx = (static_cast<float>(x) + 0.5f) - cx;
  const float dy = (static_cast<float>(y) + 0.5f) - cy;
  if ((dx * dx + dy * dy) > radius * radius) {
    cv::Vec4f out = in;
    out[3]        = 0.0f;
    return out;
  }
  return in;
}

auto ApplyVignettingCpu(const cv::Mat& src, const LensCalibGpuParams& params) -> cv::Mat {
  cv::Mat dst = src.clone();
  for (int y = 0; y < dst.rows; ++y) {
    for (int x = 0; x < dst.cols; ++x) {
      const cv::Point2f p = PixelToNormalizedCpu(params, static_cast<float>(x), static_cast<float>(y));
      const float r2       = p.x * p.x + p.y * p.y;
      const float r4       = r2 * r2;
      const float r6       = r4 * r2;
      const float c        = 1.0f + params.vignetting_terms[0] * r2 + params.vignetting_terms[1] * r4 +
                      params.vignetting_terms[2] * r6;
      const float gain     = (std::fabs(c) > 1e-8f) ? (1.0f / c) : 1.0f;
      cv::Vec4f  pixel     = dst.at<cv::Vec4f>(y, x);
      pixel[0] *= gain;
      pixel[1] *= gain;
      pixel[2] *= gain;
      dst.at<cv::Vec4f>(y, x) = pixel;
    }
  }
  return dst;
}

auto ApplyWarpCpuSinglePass(const cv::Mat& src, const LensCalibGpuParams& params) -> cv::Mat {
  cv::Mat dst(src.rows, src.cols, CV_32FC4, cv::Scalar(0.0f, 0.0f, 0.0f, 0.0f));
  for (int y = 0; y < dst.rows; ++y) {
    for (int x = 0; x < dst.cols; ++x) {
      cv::Point2f g = PixelToNormalizedCpu(params, static_cast<float>(x), static_cast<float>(y));
      g             = ApplyScaleAndPerspectiveCpu(params, g);
      g             = ApplyDistortionCpu(params, g);

      cv::Point2f r = g;
      cv::Point2f b = g;
      ApplyTcaCpu(params, g, r, b);

      const cv::Point2f gp = NormalizedToPixelCpu(params, g);
      const cv::Point2f rp = NormalizedToPixelCpu(params, r);
      const cv::Point2f bp = NormalizedToPixelCpu(params, b);

      cv::Vec4f out{};
      if (params.apply_tca != 0) {
        out[0] = BilinearSampleChannelCpu(src, rp.x, rp.y, 0);
        out[1] = BilinearSampleChannelCpu(src, gp.x, gp.y, 1);
        out[2] = BilinearSampleChannelCpu(src, bp.x, bp.y, 2);
        out[3] = BilinearSampleChannelCpu(src, gp.x, gp.y, 3);
      } else {
        out = BilinearSampleCpu(src, gp.x, gp.y);
      }
      dst.at<cv::Vec4f>(y, x) = ApplyCircleCropAlphaCpu(params, out, x, y);
    }
  }
  return dst;
}

auto ComputeRectCropRoiCpu(const LensCalibGpuParams& params) -> cv::Rect {
  const int width  = params.dst_width;
  const int height = params.dst_height;
  if (width <= 0 || height <= 0) {
    return cv::Rect();
  }

  int x0 = static_cast<int>(std::lround(params.crop_bounds[0] * static_cast<float>(width)));
  int x1 = static_cast<int>(std::lround(params.crop_bounds[1] * static_cast<float>(width)));
  int y0 = static_cast<int>(std::lround(params.crop_bounds[2] * static_cast<float>(height)));
  int y1 = static_cast<int>(std::lround(params.crop_bounds[3] * static_cast<float>(height)));

  if (x0 > x1) std::swap(x0, x1);
  if (y0 > y1) std::swap(y0, y1);

  x0 = std::clamp(x0, 0, width - 1);
  y0 = std::clamp(y0, 0, height - 1);
  x1 = std::clamp(x1, x0 + 1, width);
  y1 = std::clamp(y1, y0 + 1, height);
  return cv::Rect(x0, y0, std::max(1, x1 - x0), std::max(1, y1 - y0));
}

auto ApplyLensCalibCpuReference(const cv::Mat& src, const LensCalibGpuParams& params) -> cv::Mat {
  cv::Mat out = src.clone();
  if (params.apply_vignetting != 0) {
    out = ApplyVignettingCpu(out, params);
  }

  const bool has_warp = (params.apply_distortion != 0 || params.apply_tca != 0 ||
                         params.apply_projection != 0 || params.apply_crop_circle != 0);
  if (has_warp) {
    out = ApplyWarpCpuSinglePass(out, params);
  }

  const bool rect_crop =
      (params.apply_crop != 0 &&
       static_cast<LensCalibCropMode>(params.crop_mode) == LensCalibCropMode::RECTANGLE);
  if (rect_crop) {
    const cv::Rect roi = ComputeRectCropRoiCpu(params);
    out                = out(roi).clone();
  }
  return out;
}

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

TEST(CudaImageOpsTest, Rotate90And180MatchesCpuRotate) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  const cv::Mat src = MakeGradientMat(5, 7, CV_32FC1);

  cv::cuda::GpuMat d_img(src);
  CUDA::Rotate90CW(d_img);
  cv::Mat cw_out;
  d_img.download(cw_out);
  cv::Mat cw_ref;
  cv::rotate(src, cw_ref, cv::ROTATE_90_CLOCKWISE);
  EXPECT_LT(MeanAbsError(cw_out, cw_ref), 1e-6);

  d_img.upload(src);
  CUDA::Rotate90CCW(d_img);
  cv::Mat ccw_out;
  d_img.download(ccw_out);
  cv::Mat ccw_ref;
  cv::rotate(src, ccw_ref, cv::ROTATE_90_COUNTERCLOCKWISE);
  EXPECT_LT(MeanAbsError(ccw_out, ccw_ref), 1e-6);

  d_img.upload(src);
  CUDA::Rotate180(d_img);
  cv::Mat r180_out;
  d_img.download(r180_out);
  cv::Mat r180_ref;
  cv::rotate(src, r180_ref, cv::ROTATE_180);
  EXPECT_LT(MeanAbsError(r180_out, r180_ref), 1e-6);
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

  const cv::Rect roi = ComputeRectCropRoiCpu(params);
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

TEST(LensCalibOpTest, InvalidMetadataDisablesOperatorAndLeavesImageUnchanged) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  const cv::Mat src = MakeGradientMat(60, 92, CV_32FC4);

  nlohmann::json params = {
      {"lens_calib",
       {{"enabled", true},
        {"apply_vignetting", true},
        {"apply_distortion", true},
        {"apply_tca", true},
        {"apply_crop", true},
        {"lens_profile_db_path", "__missing_lens_db_for_test__"}}},
  };

  LensCalibOp   op(params);
  OperatorParams global{};
  global.raw_runtime_valid_         = true;
  global.raw_camera_make_           = "UnitTestCam";
  global.raw_camera_model_          = "UnitTestModel";
  global.raw_lens_make_             = "UnitTestLens";
  global.raw_lens_model_            = "UnitTestLens 50mm";
  global.raw_lens_focal_mm_         = 50.0f;
  global.raw_lens_aperture_f_       = 2.8f;
  global.raw_lens_focus_distance_m_ = 2.0f;
  global.raw_lens_focal_35mm_       = 75.0f;
  global.raw_lens_crop_factor_hint_ = 1.5f;

  op.SetGlobalParams(global);
  EXPECT_FALSE(global.lens_calib_runtime_valid_);
  EXPECT_TRUE(global.lens_calib_runtime_failed_);

  auto buffer = std::make_shared<ImageBuffer>(src.clone());
  op.ApplyGPU(buffer);

  const cv::Mat& out = buffer->GetCPUData();
  EXPECT_LT(MeanAbsError(out, src), 1e-6);
}

}  // namespace alcedo
