//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/lens/cuda/cuda_lens_calib_ops.hpp"

#include <cuda_runtime.h>
#include <opencv2/core.hpp>
#include <opencv2/core/cuda_stream_accessor.hpp>
#include <opencv2/core/cuda_types.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "decoders/processor/operators/gpu/cuda_raw_proc_utils.hpp"

namespace alcedo {
namespace CUDA {
namespace {

constexpr float kPi    = 3.14159265358979323846f;
constexpr float kEps   = 1e-8f;
constexpr float kHuge  = 1.6e16f;
constexpr float kThobyK1 = 1.47f;
constexpr float kThobyK2 = 0.713f;

struct CropRectPx {
  float left   = 0.0f;
  float right  = 0.0f;
  float top    = 0.0f;
  float bottom = 0.0f;
};

template <typename SwapT>
__host__ __device__ void SwapValues(SwapT& a, SwapT& b) {
  const SwapT tmp = a;
  a               = b;
  b               = tmp;
}

auto GetCudaStream(cv::cuda::Stream* stream) -> cudaStream_t {
  if (stream == nullptr) {
    return nullptr;
  }
  return cv::cuda::StreamAccessor::getStream(*stream);
}

void SyncIfDefaultStream(cv::cuda::Stream* stream) {
  if (stream == nullptr) {
    CUDA_CHECK(cudaDeviceSynchronize());
  }
}

auto ResolveCropRectPxHost(const LensCalibGpuParams& params) -> CropRectPx {
  CropRectPx rect{};
  const float width  = static_cast<float>(params.dst_width);
  const float height = static_cast<float>(params.dst_height);
  if (width <= 0.0f || height <= 0.0f) {
    return rect;
  }

  // Lensfun crop semantics:
  // crop[0..1] = left/right on long side, crop[2..3] = top/bottom on short side.
  if (params.dst_width >= params.dst_height) {
    rect.left   = params.crop_bounds[0] * width;
    rect.right  = params.crop_bounds[1] * width;
    rect.top    = params.crop_bounds[2] * height;
    rect.bottom = params.crop_bounds[3] * height;
  } else {
    rect.left   = params.crop_bounds[2] * width;
    rect.right  = params.crop_bounds[3] * width;
    rect.top    = params.crop_bounds[0] * height;
    rect.bottom = params.crop_bounds[1] * height;
  }

  if (rect.left > rect.right) {
    SwapValues(rect.left, rect.right);
  }
  if (rect.top > rect.bottom) {
    SwapValues(rect.top, rect.bottom);
  }
  return rect;
}

__device__ auto ResolveCropRectPxDevice(const LensCalibGpuParams& p) -> CropRectPx {
  CropRectPx rect{};
  const float width  = static_cast<float>(p.dst_width);
  const float height = static_cast<float>(p.dst_height);
  if (width <= 0.0f || height <= 0.0f) {
    return rect;
  }

  if (p.dst_width >= p.dst_height) {
    rect.left   = p.crop_bounds[0] * width;
    rect.right  = p.crop_bounds[1] * width;
    rect.top    = p.crop_bounds[2] * height;
    rect.bottom = p.crop_bounds[3] * height;
  } else {
    rect.left   = p.crop_bounds[2] * width;
    rect.right  = p.crop_bounds[3] * width;
    rect.top    = p.crop_bounds[0] * height;
    rect.bottom = p.crop_bounds[1] * height;
  }

  if (rect.left > rect.right) {
    SwapValues(rect.left, rect.right);
  }
  if (rect.top > rect.bottom) {
    SwapValues(rect.top, rect.bottom);
  }
  return rect;
}

__device__ auto PixelToNormalized(float x, float y, const LensCalibGpuParams& p) -> float2 {
  return make_float2(x * p.norm_scale - p.center_x,
                     y * p.norm_scale - p.center_y);
}

__device__ auto NormalizedToPixel(const float2& pt, const LensCalibGpuParams& p) -> float2 {
  return make_float2((pt.x + p.center_x) * p.norm_unscale,
                     (pt.y + p.center_y) * p.norm_unscale);
}

__device__ auto SafeAtan2(float y, float x) -> float {
  return (std::fabs(y) <= kEps && std::fabs(x) <= kEps) ? 0.0f : atan2f(y, x);
}

__device__ auto ApplyProjection_FishEye_Rect(const float2& in) -> float2 {
  const float r = hypotf(in.x, in.y);
  float rho     = 1.0f;
  if (r >= (kPi * 0.5f)) {
    rho = kHuge;
  } else if (r > kEps) {
    rho = tanf(r) / r;
  }
  return make_float2(rho * in.x, rho * in.y);
}

__device__ auto ApplyProjection_Rect_FishEye(const float2& in) -> float2 {
  const float r     = hypotf(in.x, in.y);
  const float theta = (r <= kEps) ? 1.0f : atanf(r) / r;
  return make_float2(theta * in.x, theta * in.y);
}

__device__ auto ApplyProjection_Panoramic_Rect(const float2& in) -> float2 {
  return make_float2(tanf(in.x), in.y / cosf(in.x));
}

__device__ auto ApplyProjection_Rect_Panoramic(const float2& in) -> float2 {
  const float x = atanf(in.x);
  return make_float2(x, in.y * cosf(x));
}

__device__ auto ApplyProjection_FishEye_Panoramic(const float2& in) -> float2 {
  const float  r = hypotf(in.x, in.y);
  const float  s = (r <= kEps) ? 1.0f : (sinf(r) / r);
  const double vx = cosf(r);
  const double vy = s * in.x;
  return make_float2(atan2f(static_cast<float>(vy), static_cast<float>(vx)),
                     static_cast<float>(s * in.y / sqrt(vx * vx + vy * vy)));
}

__device__ auto ApplyProjection_Panoramic_FishEye(const float2& in) -> float2 {
  const float s     = sinf(in.x);
  const float r     = sqrtf(s * s + in.y * in.y);
  const float theta = (r <= kEps) ? 0.0f : (atan2f(r, cosf(in.x)) / r);
  return make_float2(theta * s, theta * in.y);
}

__device__ auto ApplyProjection_ERect_Rect(const float2& in) -> float2 {
  float x = in.x;
  float y = in.y;
  float theta = -y + static_cast<float>(kPi * 0.5f);
  if (theta < 0.0f) {
    theta = -theta;
    x += kPi;
  }
  if (theta > kPi) {
    theta = 2.0f * kPi - theta;
    x += kPi;
  }
  return make_float2(tanf(x), 1.0f / (tanf(theta) * cosf(x)));
}

__device__ auto ApplyProjection_Rect_ERect(const float2& in) -> float2 {
  return make_float2(atan2f(in.x, 1.0f), atan2f(in.y, sqrtf(1.0f + in.x * in.x)));
}

__device__ auto ApplyProjection_ERect_FishEye(const float2& in) -> float2 {
  float x = in.x;
  float y = in.y;
  double theta = -y + kPi / 2.0;
  if (theta < 0.0) {
    theta = -theta;
    x += static_cast<float>(kPi);
  }
  if (theta > kPi) {
    theta = 2.0 * kPi - theta;
    x += static_cast<float>(kPi);
  }
  const double s  = sin(theta);
  const double vx = s * sin(x);
  const double vy = cos(theta);
  const double r  = sqrt(vx * vx + vy * vy);
  theta           = atan2(r, s * cos(x));
  const double inv_r = (r <= kEps) ? 0.0 : (1.0 / r);
  return make_float2(static_cast<float>(theta * vx * inv_r),
                     static_cast<float>(theta * vy * inv_r));
}

__device__ auto ApplyProjection_FishEye_ERect(const float2& in) -> float2 {
  const double r  = hypot(in.x, in.y);
  const double s  = (r <= kEps) ? 1.0 : (sin(r) / r);
  const double vx = cos(r);
  const double vy = s * in.x;
  return make_float2(static_cast<float>(atan2(vy, vx)),
                     static_cast<float>(atan(s * in.y / sqrt(vx * vx + vy * vy))));
}

__device__ auto ApplyProjection_ERect_Panoramic(const float2& in) -> float2 {
  return make_float2(in.x, tanf(in.y));
}

__device__ auto ApplyProjection_Panoramic_ERect(const float2& in) -> float2 {
  return make_float2(in.x, atanf(in.y));
}

__device__ auto ApplyProjection_Orthographic_ERect(const float2& in) -> float2 {
  const double r     = hypot(in.x, in.y);
  const double theta = (r < 1.0) ? asin(r) : (kPi / 2.0);
  const double phi   = atan2(in.y, in.x);
  const double s     = (theta <= kEps) ? 1.0 : (sin(theta) / theta);
  const double vx    = cos(theta);
  const double vy    = s * theta * cos(phi);
  return make_float2(static_cast<float>(atan2(vy, vx)),
                     static_cast<float>(atan(s * theta * sin(phi) / sqrt(vx * vx + vy * vy))));
}

__device__ auto ApplyProjection_ERect_Orthographic(const float2& in) -> float2 {
  float x = in.x;
  float y = in.y;
  double theta = -y + kPi / 2.0;
  if (theta < 0.0) {
    theta = -theta;
    x += static_cast<float>(kPi);
  }
  if (theta > kPi) {
    theta = 2.0 * kPi - theta;
    x += static_cast<float>(kPi);
  }
  const double s      = sin(theta);
  const double vx     = s * sin(x);
  const double vy     = cos(theta);
  const double theta2 = atan2(sqrt(vx * vx + vy * vy), s * cos(x));
  const double phi2   = atan2(vy, vx);
  const double rho    = sin(theta2);
  return make_float2(static_cast<float>(rho * cos(phi2)), static_cast<float>(rho * sin(phi2)));
}

__device__ auto ApplyProjection_Stereographic_ERect(const float2& in) -> float2 {
  const double rh   = hypot(in.x, in.y);
  const double c    = 2.0 * atan(rh / 2.0);
  const double sinc = sin(c);
  const double cosc = cos(c);

  float out_x = 0.0f;
  float out_y = 0.0f;
  if (std::fabs(rh) <= kEps) {
    out_y = kHuge;
  } else {
    out_y = static_cast<float>(asin(in.y * sinc / rh));
    if (std::fabs(cosc) >= kEps || std::fabs(in.x) >= kEps) {
      out_x = static_cast<float>(atan2(in.x * sinc, cosc * rh));
    } else {
      out_x = kHuge;
    }
  }
  return make_float2(out_x, out_y);
}

__device__ auto ApplyProjection_ERect_Stereographic(const float2& in) -> float2 {
  const double cos_phi = cos(in.y);
  const double ksp     = 2.0 / (1.0 + cos_phi * cos(in.x));
  return make_float2(static_cast<float>(ksp * cos_phi * sin(in.x)),
                     static_cast<float>(ksp * sin(in.y)));
}

__device__ auto ApplyProjection_Equisolid_ERect(const float2& in) -> float2 {
  const double r     = hypot(in.x, in.y);
  const double theta = (r < 2.0) ? 2.0 * asin(r / 2.0) : (kPi / 2.0);
  const double phi   = atan2(in.y, in.x);
  const double s     = (theta <= kEps) ? 1.0 : (sin(theta) / theta);
  const double vx    = cos(theta);
  const double vy    = s * theta * cos(phi);
  return make_float2(static_cast<float>(atan2(vy, vx)),
                     static_cast<float>(atan(s * theta * sin(phi) / sqrt(vx * vx + vy * vy))));
}

__device__ auto ApplyProjection_ERect_Equisolid(const float2& in) -> float2 {
  if (std::fabs(cos(in.y) * cos(in.x) + 1.0) <= kEps) {
    return make_float2(kHuge, kHuge);
  }
  const double k1 = sqrt(2.0 / (1.0 + cos(in.y) * cos(in.x)));
  return make_float2(static_cast<float>(k1 * cos(in.y) * sin(in.x)),
                     static_cast<float>(k1 * sin(in.y)));
}

__device__ auto ApplyProjection_Thoby_ERect(const float2& in) -> float2 {
  const double rho = hypot(in.x, in.y);
  if (rho < -kThobyK1 || rho > kThobyK1) {
    return make_float2(kHuge, kHuge);
  }
  const double theta = asin(rho / kThobyK1) / kThobyK2;
  const double phi   = atan2(in.y, in.x);
  const double s     = (theta <= kEps) ? 1.0 : (sin(theta) / theta);
  const double vx    = cos(theta);
  const double vy    = s * theta * cos(phi);
  return make_float2(static_cast<float>(atan2(vy, vx)),
                     static_cast<float>(atan(s * theta * sin(phi) / sqrt(vx * vx + vy * vy))));
}

__device__ auto ApplyProjection_ERect_Thoby(const float2& in) -> float2 {
  float x = in.x;
  float y = in.y;
  double theta = -y + kPi / 2.0;
  if (theta < 0.0) {
    theta = -theta;
    x += static_cast<float>(kPi);
  }
  if (theta > kPi) {
    theta = 2.0 * kPi - theta;
    x += static_cast<float>(kPi);
  }

  const double s      = sin(theta);
  const double vx     = s * sin(x);
  const double vy     = cos(theta);
  const double theta2 = atan2(sqrt(vx * vx + vy * vy), s * cos(x));
  const double phi2   = atan2(vy, vx);
  const double rho    = kThobyK1 * sin(theta2 * kThobyK2);
  return make_float2(static_cast<float>(rho * cos(phi2)), static_cast<float>(rho * sin(phi2)));
}

__device__ auto ConvertProjectionToERect(const float2& in, LensCalibProjectionType projection) -> float2 {
  switch (projection) {
    case LensCalibProjectionType::RECTILINEAR:
      return ApplyProjection_Rect_ERect(in);
    case LensCalibProjectionType::FISHEYE:
      return ApplyProjection_FishEye_ERect(in);
    case LensCalibProjectionType::PANORAMIC:
      return ApplyProjection_Panoramic_ERect(in);
    case LensCalibProjectionType::FISHEYE_ORTHOGRAPHIC:
      return ApplyProjection_Orthographic_ERect(in);
    case LensCalibProjectionType::FISHEYE_STEREOGRAPHIC:
      return ApplyProjection_Stereographic_ERect(in);
    case LensCalibProjectionType::FISHEYE_EQUISOLID:
      return ApplyProjection_Equisolid_ERect(in);
    case LensCalibProjectionType::FISHEYE_THOBY:
      return ApplyProjection_Thoby_ERect(in);
    case LensCalibProjectionType::EQUIRECTANGULAR:
    case LensCalibProjectionType::UNKNOWN:
    default:
      return in;
  }
}

__device__ auto ConvertERectToProjection(const float2& in, LensCalibProjectionType projection) -> float2 {
  switch (projection) {
    case LensCalibProjectionType::RECTILINEAR:
      return ApplyProjection_ERect_Rect(in);
    case LensCalibProjectionType::FISHEYE:
      return ApplyProjection_ERect_FishEye(in);
    case LensCalibProjectionType::PANORAMIC:
      return ApplyProjection_ERect_Panoramic(in);
    case LensCalibProjectionType::FISHEYE_ORTHOGRAPHIC:
      return ApplyProjection_ERect_Orthographic(in);
    case LensCalibProjectionType::FISHEYE_STEREOGRAPHIC:
      return ApplyProjection_ERect_Stereographic(in);
    case LensCalibProjectionType::FISHEYE_EQUISOLID:
      return ApplyProjection_ERect_Equisolid(in);
    case LensCalibProjectionType::FISHEYE_THOBY:
      return ApplyProjection_ERect_Thoby(in);
    case LensCalibProjectionType::EQUIRECTANGULAR:
    case LensCalibProjectionType::UNKNOWN:
    default:
      return in;
  }
}

__device__ auto ApplyProjectionTransform(const float2& in, const LensCalibGpuParams& p) -> float2 {
  if (p.apply_projection == 0) {
    return in;
  }
  const auto target = static_cast<LensCalibProjectionType>(p.target_projection);
  const auto source = static_cast<LensCalibProjectionType>(p.source_projection);
  if (target == LensCalibProjectionType::UNKNOWN || source == LensCalibProjectionType::UNKNOWN ||
      target == source) {
    return in;
  }
  const float2 erect = ConvertProjectionToERect(in, target);
  return ConvertERectToProjection(erect, source);
}

__device__ auto ApplyDistortion(const float2& in, const LensCalibGpuParams& p) -> float2 {
  if (p.apply_distortion == 0) {
    return in;
  }

  const auto model = static_cast<LensCalibDistortionModel>(p.distortion_model);
  const float x    = in.x;
  const float y    = in.y;
  const float ru2  = x * x + y * y;

  switch (model) {
    case LensCalibDistortionModel::POLY3: {
      const float k1   = p.distortion_terms[0];
      const float poly = 1.0f + k1 * ru2;
      return make_float2(x * poly, y * poly);
    }
    case LensCalibDistortionModel::POLY5: {
      const float k1   = p.distortion_terms[0];
      const float k2   = p.distortion_terms[1];
      const float poly = 1.0f + k1 * ru2 + k2 * ru2 * ru2;
      return make_float2(x * poly, y * poly);
    }
    case LensCalibDistortionModel::PTLENS: {
      const float a    = p.distortion_terms[0];
      const float b    = p.distortion_terms[1];
      const float c    = p.distortion_terms[2];
      const float r    = sqrtf(ru2);
      const float poly = a * ru2 * r + b * ru2 + c * r + 1.0f;
      return make_float2(x * poly, y * poly);
    }
    case LensCalibDistortionModel::NONE:
    default:
      return in;
  }
}

__device__ void ApplyTca(const float2& in, float2& red, float2& blue, const LensCalibGpuParams& p) {
  red  = in;
  blue = in;
  if (p.apply_tca == 0) {
    return;
  }

  const auto model = static_cast<LensCalibTCAModel>(p.tca_model);
  switch (model) {
    case LensCalibTCAModel::LINEAR: {
      const float k_r = p.tca_terms[0];
      const float k_b = p.tca_terms[1];
      red             = make_float2(in.x * k_r, in.y * k_r);
      blue            = make_float2(in.x * k_b, in.y * k_b);
      return;
    }
    case LensCalibTCAModel::POLY3: {
      const float vr  = p.tca_terms[0];
      const float vb  = p.tca_terms[1];
      const float cr  = p.tca_terms[2];
      const float cb  = p.tca_terms[3];
      const float br  = p.tca_terms[4];
      const float bb  = p.tca_terms[5];
      const float r2  = in.x * in.x + in.y * in.y;
      const float rr  = sqrtf(r2);
      const float fr  = br * r2 + cr * rr + vr;
      const float fb  = bb * r2 + cb * rr + vb;
      red             = make_float2(in.x * fr, in.y * fr);
      blue            = make_float2(in.x * fb, in.y * fb);
      return;
    }
    case LensCalibTCAModel::NONE:
    default:
      return;
  }
}

__device__ auto ReadWithBorder(const cv::cuda::PtrStepSz<float4>& src, int x, int y) -> float4 {
  if (x < 0 || y < 0 || x >= src.cols || y >= src.rows) {
    return make_float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
  return src(y, x);
}

__device__ auto BilinearSample(const cv::cuda::PtrStepSz<float4>& src, float sx, float sy) -> float4 {
  const int x0 = static_cast<int>(floorf(sx));
  const int y0 = static_cast<int>(floorf(sy));
  const int x1 = x0 + 1;
  const int y1 = y0 + 1;

  const float fx = sx - static_cast<float>(x0);
  const float fy = sy - static_cast<float>(y0);
  const float w00 = (1.0f - fx) * (1.0f - fy);
  const float w10 = fx * (1.0f - fy);
  const float w01 = (1.0f - fx) * fy;
  const float w11 = fx * fy;

  const float4 p00 = ReadWithBorder(src, x0, y0);
  const float4 p10 = ReadWithBorder(src, x1, y0);
  const float4 p01 = ReadWithBorder(src, x0, y1);
  const float4 p11 = ReadWithBorder(src, x1, y1);

  float4 out;
  out.x = p00.x * w00 + p10.x * w10 + p01.x * w01 + p11.x * w11;
  out.y = p00.y * w00 + p10.y * w10 + p01.y * w01 + p11.y * w11;
  out.z = p00.z * w00 + p10.z * w10 + p01.z * w01 + p11.z * w11;
  out.w = p00.w * w00 + p10.w * w10 + p01.w * w01 + p11.w * w11;
  return out;
}

__device__ auto BilinearSampleChannel(const cv::cuda::PtrStepSz<float4>& src, float sx, float sy,
                                      int channel) -> float {
  const float4 pixel = BilinearSample(src, sx, sy);
  switch (channel) {
    case 0:
      return pixel.x;
    case 1:
      return pixel.y;
    case 2:
      return pixel.z;
    case 3:
    default:
      return pixel.w;
  }
}

__device__ auto ApplyScaleAndPerspective(const float2& in, const LensCalibGpuParams& p) -> float2 {
  float2 out = in;
  if (std::fabs(p.resolved_scale) > kEps) {
    const float inv_scale = 1.0f / p.resolved_scale;
    out.x *= inv_scale;
    out.y *= inv_scale;
  }
  // Perspective correction extension point.
  return out;
}

__device__ auto ApplyCircleCropAlpha(const float4& in, int x, int y, const LensCalibGpuParams& p) -> float4 {
  if (p.apply_crop_circle == 0) {
    return in;
  }
  const CropRectPx rect = ResolveCropRectPxDevice(p);
  const float left      = rect.left;
  const float right     = rect.right;
  const float top       = rect.top;
  const float bottom    = rect.bottom;

  const float cx = 0.5f * (left + right);
  const float cy = 0.5f * (top + bottom);
  const float rx = 0.5f * std::fabs(right - left);
  const float ry = 0.5f * std::fabs(bottom - top);
  const float radius = fminf(rx, ry);
  if (radius <= kEps) {
    return in;
  }

  const float dx = (static_cast<float>(x) + 0.5f) - cx;
  const float dy = (static_cast<float>(y) + 0.5f) - cy;
  if ((dx * dx + dy * dy) > (radius * radius)) {
    float4 out = in;
    out.w      = 0.0f;
    return out;
  }
  return in;
}

__global__ void LensVignettingKernel(cv::cuda::PtrStepSz<float4> image, LensCalibGpuParams p) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= image.cols || y >= image.rows) {
    return;
  }

  const float2 pt = PixelToNormalized(static_cast<float>(x), static_cast<float>(y), p);
  const float  r2 = pt.x * pt.x + pt.y * pt.y;
  const float  r4 = r2 * r2;
  const float  r6 = r4 * r2;
  const float  c  = 1.0f + p.vignetting_terms[0] * r2 +
                   p.vignetting_terms[1] * r4 +
                   p.vignetting_terms[2] * r6;
  const float gain = (std::fabs(c) > kEps) ? (1.0f / c) : 1.0f;

  float4 pix = image(y, x);
  pix.x *= gain;
  pix.y *= gain;
  pix.z *= gain;
  image(y, x) = pix;
}

// Mapping order in inverse lookup space (destination -> source):
// 1) scaling, 2) perspective (stub), 3) projection conversion,
// 4) distortion, 5) TCA, 6) sampling from source.
__global__ void LensWarpGeometryTcaKernel(const cv::cuda::PtrStepSz<float4> src,
                                          cv::cuda::PtrStepSz<float4> dst,
                                          LensCalibGpuParams p) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= dst.cols || y >= dst.rows) {
    return;
  }

  float2 g = PixelToNormalized(static_cast<float>(x), static_cast<float>(y), p);
  g        = ApplyScaleAndPerspective(g, p);
  g        = ApplyProjectionTransform(g, p);
  g        = ApplyDistortion(g, p);

  float2 r = g;
  float2 b = g;
  ApplyTca(g, r, b, p);

  const float2 gp = NormalizedToPixel(g, p);
  const float2 rp = NormalizedToPixel(r, p);
  const float2 bp = NormalizedToPixel(b, p);

  float4 out{};
  if (p.apply_tca != 0) {
    out.x = BilinearSampleChannel(src, rp.x, rp.y, 0);
    out.y = BilinearSampleChannel(src, gp.x, gp.y, 1);
    out.z = BilinearSampleChannel(src, bp.x, bp.y, 2);
    out.w = BilinearSampleChannel(src, gp.x, gp.y, 3);
  } else {
    out = BilinearSample(src, gp.x, gp.y);
  }

  out      = ApplyCircleCropAlpha(out, x, y, p);
  dst(y, x) = out;
}

auto ComputeRectCropRoi(const LensCalibGpuParams& params) -> cv::Rect {
  const int width  = params.dst_width;
  const int height = params.dst_height;
  if (width <= 0 || height <= 0) {
    return cv::Rect();
  }

  const CropRectPx rect = ResolveCropRectPxHost(params);

  int x0 = static_cast<int>(std::lround(rect.left));
  int x1 = static_cast<int>(std::lround(rect.right));
  int y0 = static_cast<int>(std::lround(rect.top));
  int y1 = static_cast<int>(std::lround(rect.bottom));

  if (x0 > x1) std::swap(x0, x1);
  if (y0 > y1) std::swap(y0, y1);

  x0 = std::clamp(x0, 0, width - 1);
  y0 = std::clamp(y0, 0, height - 1);
  x1 = std::clamp(x1, x0 + 1, width);
  y1 = std::clamp(y1, y0 + 1, height);
  return cv::Rect(x0, y0, std::max(1, x1 - x0), std::max(1, y1 - y0));
}

}  // namespace

void ApplyLensCalibration(cv::cuda::GpuMat& image, const LensCalibGpuParams& params,
                          cv::cuda::Stream* stream) {
  if (image.empty()) {
    return;
  }
  if (image.type() != CV_32FC4) {
    throw std::runtime_error("CUDA::ApplyLensCalibration expects CV_32FC4 input");
  }

  LensCalibGpuParams launch = params;
  if (launch.src_width <= 0 || launch.src_height <= 0) {
    launch.src_width  = image.cols;
    launch.src_height = image.rows;
  }
  if (launch.dst_width <= 0 || launch.dst_height <= 0) {
    launch.dst_width  = image.cols;
    launch.dst_height = image.rows;
  }

  const bool has_vignetting = (launch.apply_vignetting != 0);
  const bool has_warp       = (launch.apply_distortion != 0 || launch.apply_tca != 0 ||
                               launch.apply_projection != 0 || launch.apply_crop_circle != 0);
  const bool has_rect_crop  = (launch.apply_crop != 0 &&
                              static_cast<LensCalibCropMode>(launch.crop_mode) ==
                                  LensCalibCropMode::RECTANGLE);
  if (!has_vignetting && !has_warp && !has_rect_crop) {
    return;
  }

  const dim3 block(16, 16);
  const dim3 grid((image.cols + block.x - 1) / block.x, (image.rows + block.y - 1) / block.y);
  const cudaStream_t cuda_stream = GetCudaStream(stream);

  if (has_vignetting) {
    LensVignettingKernel<<<grid, block, 0, cuda_stream>>>(image, launch);
    CUDA_CHECK(cudaGetLastError());
    SyncIfDefaultStream(stream);
  }

  if (has_warp) {
    cv::cuda::GpuMat warped(image.rows, image.cols, image.type());
    LensWarpGeometryTcaKernel<<<grid, block, 0, cuda_stream>>>(image, warped, launch);
    CUDA_CHECK(cudaGetLastError());
    SyncIfDefaultStream(stream);
    image = std::move(warped);
  }

  if (has_rect_crop) {
    const cv::Rect roi = ComputeRectCropRoi(launch);
    std::cout << "LensCalib CUDA rect crop: src=" << image.cols << "x" << image.rows
              << " roi=(" << roi.x << ", " << roi.y << ", " << roi.width << ", " << roi.height
              << ")" << std::endl;
    if (roi.width > 0 && roi.height > 0) {
      if (stream != nullptr) {
        cv::cuda::GpuMat cropped;
        image(roi).copyTo(cropped, *stream);
        image = std::move(cropped);
      } else {
        image = image(roi).clone();
      }
      std::cout << "LensCalib CUDA rect crop result: " << image.cols << "x" << image.rows
                << std::endl;
    }
  }
}

}  // namespace CUDA
}  // namespace alcedo
