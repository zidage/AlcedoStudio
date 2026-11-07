//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

// CPU reference of the lens calibration pass (vignetting, warp, rectangle crop) and the
// measurements of the issue #713 tests. The CUDA, OpenCL, and Metal lens tests compare their
// kernel output with ApplyLensCalibCpuReference. Each backend stays within the same stated
// tolerance of this one reference, so any two backends agree within twice that tolerance.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <numbers>
#include <opencv2/core.hpp>
#include <stdexcept>
#include <string>
#include <utility>

#include "edit/runtime/lens/lens_calib_runtime.hpp"
#include "edit/runtime/lens/lens_calibration_resolver.hpp"

namespace alcedo::lens_warp_test {

struct CropRectPx {
  float left   = 0.0f;
  float right  = 0.0f;
  float top    = 0.0f;
  float bottom = 0.0f;
};

inline auto ResolveCropRectPx(const LensCalibGpuParams& p) -> CropRectPx {
  CropRectPx  rect{};
  const float width  = static_cast<float>(p.dst_width);
  const float height = static_cast<float>(p.dst_height);
  if (width <= 0.0f || height <= 0.0f) {
    return rect;
  }
  if (p.dst_width >= p.dst_height) {
    rect = {p.crop_bounds[0] * width, p.crop_bounds[1] * width, p.crop_bounds[2] * height,
            p.crop_bounds[3] * height};
  } else {
    rect = {p.crop_bounds[2] * width, p.crop_bounds[3] * width, p.crop_bounds[0] * height,
            p.crop_bounds[1] * height};
  }
  if (rect.left > rect.right) {
    std::swap(rect.left, rect.right);
  }
  if (rect.top > rect.bottom) {
    std::swap(rect.top, rect.bottom);
  }
  return rect;
}

inline auto PixelToNormalized(const LensCalibGpuParams& p, float x, float y) -> cv::Point2f {
  return {x * p.norm_scale - p.center_x, y * p.norm_scale - p.center_y};
}

inline auto NormalizedToPixel(const LensCalibGpuParams& p, cv::Point2f pt) -> cv::Point2f {
  return {(pt.x + p.center_x) * p.norm_unscale, (pt.y + p.center_y) * p.norm_unscale};
}

inline auto ApplyScale(const LensCalibGpuParams& p, cv::Point2f in) -> cv::Point2f {
  if (std::fabs(p.resolved_scale) > 1.0e-8f) {
    const float inv_scale = 1.0f / p.resolved_scale;
    in.x *= inv_scale;
    in.y *= inv_scale;
  }
  return in;
}

inline auto ApplyDistortion(const LensCalibGpuParams& p, cv::Point2f in) -> cv::Point2f {
  if (p.apply_distortion == 0) {
    return in;
  }
  const float ru2  = in.x * in.x + in.y * in.y;
  float       poly = 1.0f;
  switch (static_cast<LensCalibDistortionModel>(p.distortion_model)) {
    case LensCalibDistortionModel::POLY3:
      poly = 1.0f + p.distortion_terms[0] * ru2;
      break;
    case LensCalibDistortionModel::POLY5:
      poly = 1.0f + p.distortion_terms[0] * ru2 + p.distortion_terms[1] * ru2 * ru2;
      break;
    case LensCalibDistortionModel::PTLENS: {
      const float r = std::sqrt(ru2);
      poly          = p.distortion_terms[0] * ru2 * r + p.distortion_terms[1] * ru2 +
             p.distortion_terms[2] * r + 1.0f;
      break;
    }
    case LensCalibDistortionModel::NONE:
    default:
      return in;
  }
  return {in.x * poly, in.y * poly};
}

inline void ApplyTca(const LensCalibGpuParams& p, cv::Point2f in, cv::Point2f& red,
                     cv::Point2f& blue) {
  red  = in;
  blue = in;
  if (p.apply_tca == 0) {
    return;
  }
  switch (static_cast<LensCalibTCAModel>(p.tca_model)) {
    case LensCalibTCAModel::LINEAR:
      red  = in * p.tca_terms[0];
      blue = in * p.tca_terms[1];
      return;
    case LensCalibTCAModel::POLY3: {
      const float r2 = in.x * in.x + in.y * in.y;
      const float rr = std::sqrt(r2);
      red            = in * (p.tca_terms[4] * r2 + p.tca_terms[2] * rr + p.tca_terms[0]);
      blue           = in * (p.tca_terms[5] * r2 + p.tca_terms[3] * rr + p.tca_terms[1]);
      return;
    }
    case LensCalibTCAModel::NONE:
    default:
      return;
  }
}

/// Source positions of one output pixel: green (also alpha), red, and blue.
struct WarpPositions {
  cv::Point2f green;
  cv::Point2f red;
  cv::Point2f blue;
};

inline auto MapOutputPixel(const LensCalibGpuParams& p, int x, int y) -> WarpPositions {
  if (p.apply_projection != 0) {
    throw std::invalid_argument("The lens CPU reference has no projection conversion.");
  }
  cv::Point2f g = PixelToNormalized(p, static_cast<float>(x), static_cast<float>(y));
  g             = ApplyDistortion(p, ApplyScale(p, g));
  cv::Point2f r = g;
  cv::Point2f b = g;
  ApplyTca(p, g, r, b);
  return {NormalizedToPixel(p, g), NormalizedToPixel(p, r), NormalizedToPixel(p, b)};
}

inline auto Lanczos3Weight(double t) -> double {
  if (std::fabs(t) < 1.0e-5) {
    return 1.0;
  }
  if (std::fabs(t) >= 3.0) {
    return 0.0;
  }
  const double pi_t = std::numbers::pi * t;
  return 3.0 * std::sin(pi_t) * std::sin(pi_t / 3.0) / (pi_t * pi_t);
}

inline void Lanczos3Weights(double f, double (&w)[6]) {
  double sum = 0.0;
  for (int i = 0; i < 6; ++i) {
    w[i] = Lanczos3Weight(f - static_cast<double>(i - 2));
    sum += w[i];
  }
  for (double& weight : w) {
    weight /= sum;
  }
}

inline auto ReadClamped(const cv::Mat& src, int x, int y) -> cv::Vec4f {
  return src.at<cv::Vec4f>(std::clamp(y, 0, src.rows - 1), std::clamp(x, 0, src.cols - 1));
}

inline auto BorderCoverage(float s, int size) -> float {
  return std::clamp(std::min(s + 1.0f, static_cast<float>(size) - s), 0.0f, 1.0f);
}

/// Lanczos-3 sample with the 2 x 2 clamp and the border coverage of the lens warp kernels.
inline auto Lanczos3Sample(const cv::Mat& src, cv::Point2f pos) -> cv::Vec4f {
  const float coverage = BorderCoverage(pos.x, src.cols) * BorderCoverage(pos.y, src.rows);
  if (!(coverage > 0.0f)) {
    return cv::Vec4f(0.0f, 0.0f, 0.0f, 0.0f);
  }
  const int x0 = static_cast<int>(std::floor(pos.x));
  const int y0 = static_cast<int>(std::floor(pos.y));
  double    wx[6];
  double    wy[6];
  Lanczos3Weights(static_cast<double>(pos.x) - x0, wx);
  Lanczos3Weights(static_cast<double>(pos.y) - y0, wy);

  cv::Vec4d acc(0.0, 0.0, 0.0, 0.0);
  for (int j = 0; j < 6; ++j) {
    cv::Vec4d row(0.0, 0.0, 0.0, 0.0);
    for (int i = 0; i < 6; ++i) {
      row += cv::Vec4d(ReadClamped(src, x0 + i - 2, y0 + j - 2)) * wx[i];
    }
    acc += row * wy[j];
  }

  const cv::Vec4f p00 = ReadClamped(src, x0, y0);
  const cv::Vec4f p10 = ReadClamped(src, x0 + 1, y0);
  const cv::Vec4f p01 = ReadClamped(src, x0, y0 + 1);
  const cv::Vec4f p11 = ReadClamped(src, x0 + 1, y0 + 1);
  cv::Vec4f       out;
  for (int c = 0; c < 4; ++c) {
    const float lo = std::min(std::min(p00[c], p10[c]), std::min(p01[c], p11[c]));
    const float hi = std::max(std::max(p00[c], p10[c]), std::max(p01[c], p11[c]));
    out[c]         = std::clamp(static_cast<float>(acc[c]), lo, hi) * coverage;
  }
  return out;
}

inline auto ApplyCircleCropAlpha(const LensCalibGpuParams& p, cv::Vec4f in, int x, int y)
    -> cv::Vec4f {
  if (p.apply_crop_circle == 0) {
    return in;
  }
  const CropRectPx rect = ResolveCropRectPx(p);
  const float      radius =
      std::min(0.5f * std::fabs(rect.right - rect.left), 0.5f * std::fabs(rect.bottom - rect.top));
  if (radius <= 1.0e-8f) {
    return in;
  }
  const float dx = (static_cast<float>(x) + 0.5f) - 0.5f * (rect.left + rect.right);
  const float dy = (static_cast<float>(y) + 0.5f) - 0.5f * (rect.top + rect.bottom);
  if ((dx * dx + dy * dy) > radius * radius) {
    in[3] = 0.0f;
  }
  return in;
}

inline auto ApplyVignettingCpu(const cv::Mat& src, const LensCalibGpuParams& p) -> cv::Mat {
  cv::Mat dst = src.clone();
  for (int y = 0; y < dst.rows; ++y) {
    for (int x = 0; x < dst.cols; ++x) {
      const cv::Point2f pt = PixelToNormalized(p, static_cast<float>(x), static_cast<float>(y));
      const float       r2 = pt.x * pt.x + pt.y * pt.y;
      const float       c  = 1.0f + p.vignetting_terms[0] * r2 + p.vignetting_terms[1] * r2 * r2 +
                      p.vignetting_terms[2] * r2 * r2 * r2;
      const float gain  = (std::fabs(c) > 1.0e-8f) ? (1.0f / c) : 1.0f;
      auto&       pixel = dst.at<cv::Vec4f>(y, x);
      pixel[0] *= gain;
      pixel[1] *= gain;
      pixel[2] *= gain;
    }
  }
  return dst;
}

inline auto ApplyWarpCpu(const cv::Mat& src, const LensCalibGpuParams& p) -> cv::Mat {
  cv::Mat dst(src.rows, src.cols, CV_32FC4, cv::Scalar::all(0.0));
  for (int y = 0; y < dst.rows; ++y) {
    for (int x = 0; x < dst.cols; ++x) {
      const WarpPositions pos = MapOutputPixel(p, x, y);
      cv::Vec4f           out = Lanczos3Sample(src, pos.green);
      if (p.apply_tca != 0) {
        out[0] = Lanczos3Sample(src, pos.red)[0];
        out[2] = Lanczos3Sample(src, pos.blue)[2];
      }
      dst.at<cv::Vec4f>(y, x) = ApplyCircleCropAlpha(p, out, x, y);
    }
  }
  return dst;
}

inline auto ComputeRectCropRoi(const LensCalibGpuParams& p) -> cv::Rect {
  const int width  = p.dst_width;
  const int height = p.dst_height;
  if (width <= 0 || height <= 0) {
    return {};
  }
  const CropRectPx rect = ResolveCropRectPx(p);
  int              x0   = static_cast<int>(std::lround(rect.left));
  int              x1   = static_cast<int>(std::lround(rect.right));
  int              y0   = static_cast<int>(std::lround(rect.top));
  int              y1   = static_cast<int>(std::lround(rect.bottom));
  x0                    = std::clamp(x0, 0, width - 1);
  y0                    = std::clamp(y0, 0, height - 1);
  x1                    = std::clamp(x1, x0 + 1, width);
  y1                    = std::clamp(y1, y0 + 1, height);
  return {x0, y0, std::max(1, x1 - x0), std::max(1, y1 - y0)};
}

/// Full lens pass on the CPU: vignetting, warp when any geometry applies, then rectangle crop.
inline auto ApplyLensCalibCpuReference(const cv::Mat& src, const LensCalibGpuParams& p) -> cv::Mat {
  cv::Mat out = src.clone();
  if (p.apply_vignetting != 0) {
    out = ApplyVignettingCpu(out, p);
  }
  if (p.apply_distortion != 0 || p.apply_tca != 0 || p.apply_projection != 0 ||
      p.apply_crop_circle != 0) {
    out = ApplyWarpCpu(out, p);
  }
  if (p.apply_crop != 0 &&
      static_cast<LensCalibCropMode>(p.crop_mode) == LensCalibCropMode::RECTANGLE) {
    out = out(ComputeRectCropRoi(p)).clone();
  }
  return out;
}

inline auto MaxAbsDiff(const cv::Mat& a, const cv::Mat& b) -> double {
  cv::Mat diff;
  cv::absdiff(a, b, diff);
  double max_value = 0.0;
  cv::minMaxLoc(diff.reshape(1), nullptr, &max_value);
  return max_value;
}

// --- Issue #713 fixtures ---------------------------------------------------------------------

constexpr float kGratingMean      = 0.5f;
constexpr float kGratingAmplitude = 0.25f;

/// RGB stripes along x: kGratingMean + kGratingAmplitude * cos(2 pi f x); alpha 1.
inline auto     MakeVerticalGrating(int width, int height, double cycles_per_px) -> cv::Mat {
  cv::Mat out(height, width, CV_32FC4);
  for (int x = 0; x < width; ++x) {
    const float v =
        kGratingMean + kGratingAmplitude *
                           static_cast<float>(std::cos(2.0 * std::numbers::pi * cycles_per_px * x));
    for (int y = 0; y < height; ++y) {
      out.at<cv::Vec4f>(y, x) = cv::Vec4f(v, v, v, 1.0f);
    }
  }
  return out;
}

/// Left of @p edge_x: a dark value; from @p edge_x on: a highlight far above 1.
inline auto MakeHighlightEdge(int width, int height, int edge_x) -> cv::Mat {
  cv::Mat out(height, width, CV_32FC4);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const float v           = x < edge_x ? 0.05f : 8.0f;
      out.at<cv::Vec4f>(y, x) = cv::Vec4f(v, v, v, 1.0f);
    }
  }
  return out;
}

/**
 * @brief Amplitude of the grating in @p out relative to the input amplitude.
 *
 * Each output pixel at least @p margin px from the border maps to its source position. The
 * channel value minus the grating mean is fitted by least squares to a cos(phi) + b sin(phi) with
 * phi = 2 pi f sx. A sampler that passes the grating unchanged gives 1.
 */
inline auto MeasureGratingAmplitude(const cv::Mat& out, const LensCalibGpuParams& p,
                                    double cycles_per_px, int channel, int margin) -> double {
  double cc = 0.0;
  double ss = 0.0;
  double cs = 0.0;
  double vc = 0.0;
  double vs = 0.0;
  for (int y = margin; y < out.rows - margin; ++y) {
    for (int x = margin; x < out.cols - margin; ++x) {
      const WarpPositions pos = MapOutputPixel(p, x, y);
      const cv::Point2f   src = channel == 0 ? pos.red : (channel == 2 ? pos.blue : pos.green);
      const double        phi = 2.0 * std::numbers::pi * cycles_per_px * src.x;
      const double        c   = std::cos(phi);
      const double        s   = std::sin(phi);
      const double        v = static_cast<double>(out.at<cv::Vec4f>(y, x)[channel]) - kGratingMean;
      cc += c * c;
      ss += s * s;
      cs += c * s;
      vc += v * c;
      vs += v * s;
    }
  }
  const double det = cc * ss - cs * cs;
  const double a   = (vc * ss - vs * cs) / det;
  const double b   = (vs * cc - vc * cs) / det;
  return std::hypot(a, b) / kGratingAmplitude;
}

/**
 * @brief Lens parameters of issue #713 for a @p width x @p height image.
 *
 * Panasonic DC-G9 with Lensfun lens "Panasonic" / "LEICA DG 12-60/F2.8-4.0" at 32 mm and f/8,
 * auto scale on. Distortion and TCA apply; vignetting and crop are off so that the warp is the
 * only change to the pixels.
 */
inline auto ResolveIssue713Params(int width, int height) -> LensCalibGpuParams {
  LensInputMeta meta;
  meta.cam_maker_         = "Panasonic";
  meta.cam_model_         = "DC-G9";
  meta.lens_maker_        = "Panasonic";
  meta.lens_model_        = "LEICA DG 12-60/F2.8-4.0";
  meta.focal_length_mm_   = 32.0f;
  meta.aperture_f_number_ = 8.0f;

  LensCorrectionSettings settings;
  settings.database_path_    = std::filesystem::path(CONFIG_PATH) / "lens_calib";
  settings.apply_vignetting_ = false;
  settings.apply_crop_       = false;
  settings.auto_scale_       = true;

  const auto resolution      = LensCalibrationResolver::ResolveProfile(meta, settings, false);
  if (resolution.status_ != LensProfileStatus::Resolved) {
    throw std::runtime_error("Issue #713 lens profile did not resolve.");
  }
  LensCalibGpuParams params = resolution.params_;
  LensCalibrationResolver::BindImageExtent(resolution.matched_meta_, settings.database_path_,
                                           params, width, height);
  return params;
}

/// Runs the backend lens pass on a CV_32FC4 host image and returns the result as CV_32FC4.
using LensPassRunner = std::function<cv::Mat(const cv::Mat&, const LensCalibGpuParams&)>;

// Tolerances of a backend against the CPU reference. The kernels compute in float with the
// device sinpi; the reference uses double weights. Two backends therefore agree within twice
// these values.
constexpr double kMaxAbsTolerance         = 2.0e-4;
constexpr double kMinGratingAmplitudeAt03 = 0.9;
constexpr int    kTestWidth               = 640;
constexpr int    kTestHeight              = 480;
constexpr int    kAmplitudeMargin         = 16;

/// Issue #713: a 0.3 cycles/px grating keeps at least 0.9 of its amplitude through the warp.
inline void      VerifyIssue713GratingAmplitude(const LensPassRunner& run) {
  const LensCalibGpuParams params = ResolveIssue713Params(kTestWidth, kTestHeight);
  ASSERT_EQ(params.apply_distortion, 1);
  ASSERT_EQ(params.apply_tca, 1);
  ASSERT_EQ(params.apply_vignetting, 0);
  ASSERT_GT(params.resolved_scale, 1.001f) << "auto scale must put pixels at fractional positions";

  constexpr double kCycles = 0.3;
  const cv::Mat    src     = MakeVerticalGrating(kTestWidth, kTestHeight, kCycles);
  const cv::Mat    gpu     = run(src, params);
  const cv::Mat    cpu     = ApplyLensCalibCpuReference(src, params);
  ASSERT_EQ(gpu.size(), cpu.size());
  ASSERT_EQ(gpu.type(), CV_32FC4);
  const double max_abs_diff = MaxAbsDiff(gpu, cpu);
  std::cout << "Issue #713 grating: resolved_scale=" << params.resolved_scale
            << " max_abs_diff_to_cpu=" << max_abs_diff << '\n';
  EXPECT_LE(max_abs_diff, kMaxAbsTolerance);

  for (int channel = 0; channel < 3; ++channel) {
    const double gpu_amplitude =
        MeasureGratingAmplitude(gpu, params, kCycles, channel, kAmplitudeMargin);
    const double cpu_amplitude =
        MeasureGratingAmplitude(cpu, params, kCycles, channel, kAmplitudeMargin);
    std::cout << "Issue #713 grating: channel " << channel << " amplitude at " << kCycles
              << " cycles/px = " << gpu_amplitude << '\n';
    EXPECT_GE(gpu_amplitude, kMinGratingAmplitudeAt03) << "channel " << channel;
    EXPECT_NEAR(gpu_amplitude, cpu_amplitude, 1.0e-3) << "channel " << channel;
  }
}

/// The 2 x 2 clamp keeps the result inside the source range next to a highlight edge.
inline void VerifyHighlightEdgeHasNoRinging(const LensPassRunner& run) {
  const LensCalibGpuParams params = ResolveIssue713Params(kTestWidth, kTestHeight);
  const cv::Mat            src    = MakeHighlightEdge(kTestWidth, kTestHeight, kTestWidth / 3);
  const cv::Mat            gpu    = run(src, params);
  const cv::Mat            cpu    = ApplyLensCalibCpuReference(src, params);
  ASSERT_EQ(gpu.size(), cpu.size());
  EXPECT_LE(MaxAbsDiff(gpu, cpu), 8.0f * kMaxAbsTolerance);

  // Rows and columns near the border can fade with the border coverage; the inner area cannot.
  double min_value = 0.0;
  double max_value = 0.0;
  cv::minMaxLoc(gpu(cv::Rect(4, 4, kTestWidth - 8, kTestHeight - 8)).reshape(1), &min_value,
                &max_value);
  EXPECT_GE(min_value, 0.05f - 1.0e-6f);
  EXPECT_LE(max_value, 8.0f + 1.0e-5f);
}

/// Vignetting alone takes the pass without a warp: pixels keep their positions.
inline void VerifyVignettingOnlyDoesNotResample(const LensPassRunner& run) {
  LensCalibGpuParams params  = ResolveIssue713Params(kTestWidth, kTestHeight);
  params.apply_distortion    = 0;
  params.apply_tca           = 0;
  params.apply_projection    = 0;
  params.apply_crop          = 0;
  params.apply_crop_circle   = 0;
  params.apply_vignetting    = 1;
  params.vignetting_model    = static_cast<std::int32_t>(LensCalibVignettingModel::PA);
  params.vignetting_terms[0] = -0.18f;
  params.vignetting_terms[1] = 0.01f;
  params.vignetting_terms[2] = -0.07f;

  const cv::Mat src          = MakeVerticalGrating(kTestWidth, kTestHeight, 0.45);
  const cv::Mat gpu          = run(src, params);
  const cv::Mat cpu          = ApplyVignettingCpu(src, params);
  ASSERT_EQ(gpu.size(), cpu.size());
  EXPECT_LE(MaxAbsDiff(gpu, cpu), 1.0e-5);
}

}  // namespace alcedo::lens_warp_test
