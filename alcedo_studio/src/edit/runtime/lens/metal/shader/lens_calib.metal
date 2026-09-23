//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <metal_stdlib>

using namespace metal;

enum LensCalibDistortionModel : int {
  LensDistNone   = 0,
  LensDistPoly3  = 1,
  LensDistPoly5  = 2,
  LensDistPtLens = 3,
};

enum LensCalibTCAModel : int {
  LensTcaNone   = 0,
  LensTcaLinear = 1,
  LensTcaPoly3  = 2,
};

enum LensCalibCropMode : int {
  LensCropNone      = 0,
  LensCropRectangle = 1,
  LensCropCircle    = 2,
};

enum LensCalibProjectionType : int {
  LensProjUnknown               = 0,
  LensProjRectilinear           = 1,
  LensProjFishEye               = 2,
  LensProjPanoramic             = 3,
  LensProjEquirectangular       = 4,
  LensProjFishEyeOrthographic   = 5,
  LensProjFishEyeStereographic  = 6,
  LensProjFishEyeEquisolid      = 7,
  LensProjFishEyeThoby          = 8,
};

struct LensCalibGpuParams {
  int   version;

  int   src_width;
  int   src_height;
  int   dst_width;
  int   dst_height;

  float norm_scale;
  float norm_unscale;
  float center_x;
  float center_y;

  float camera_crop_factor;
  float nominal_focal_mm;
  float real_focal_mm;
  float lens_center_x;
  float lens_center_y;

  int   source_projection;
  int   target_projection;

  int   distortion_model;
  float distortion_terms[5];

  int   tca_model;
  float tca_terms[12];

  int   vignetting_model;
  float vignetting_terms[3];

  int   crop_mode;
  float crop_bounds[4];

  int   interpolation;

  int   apply_vignetting;
  int   apply_distortion;
  int   apply_tca;
  int   apply_projection;
  int   apply_crop;
  int   apply_crop_circle;

  int   use_user_scale;
  int   use_auto_scale;
  float user_scale;
  float resolved_scale;

  int   perspective_mode;
  float perspective_terms[8];

  int   fast_path_distortion_only;
  int   fast_path_vignetting_only;

  int   low_precision_preview;
};

struct LensCalibDispatchParams {
  LensCalibGpuParams params;
  uint               src_stride;
  uint               dst_stride;
};

struct CropRectPx {
  float left;
  float right;
  float top;
  float bottom;
};

constant float kPi      = 3.14159265358979323846f;
constant float kEps     = 1e-8f;
constant float kHuge    = 1.6e16f;
constant float kThobyK1 = 1.47f;
constant float kThobyK2 = 0.713f;

inline void SwapValues(thread float& a, thread float& b) {
  const float tmp = a;
  a               = b;
  b               = tmp;
}

inline auto ResolveCropRectPx(constant LensCalibGpuParams& p) -> CropRectPx {
  CropRectPx rect = {0.0f, 0.0f, 0.0f, 0.0f};
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

inline auto PixelToNormalized(float x, float y, constant LensCalibGpuParams& p) -> float2 {
  return float2(x * p.norm_scale - p.center_x, y * p.norm_scale - p.center_y);
}

inline auto NormalizedToPixel(float2 pt, constant LensCalibGpuParams& p) -> float2 {
  return float2((pt.x + p.center_x) * p.norm_unscale, (pt.y + p.center_y) * p.norm_unscale);
}

inline auto ApplyProjectionFishEyeRect(float2 in) -> float2 {
  const float r = length(in);
  float       rho = 1.0f;
  if (r >= (kPi * 0.5f)) {
    rho = kHuge;
  } else if (r > kEps) {
    rho = tan(r) / r;
  }
  return float2(rho * in.x, rho * in.y);
}

inline auto ApplyProjectionRectFishEye(float2 in) -> float2 {
  const float r     = length(in);
  const float theta = (r <= kEps) ? 1.0f : (atan(r) / r);
  return float2(theta * in.x, theta * in.y);
}

inline auto ApplyProjectionPanoramicRect(float2 in) -> float2 {
  return float2(tan(in.x), in.y / cos(in.x));
}

inline auto ApplyProjectionRectPanoramic(float2 in) -> float2 {
  const float x = atan(in.x);
  return float2(x, in.y * cos(x));
}

inline auto ApplyProjectionFishEyePanoramic(float2 in) -> float2 {
  const float r = length(in);
  const float s = (r <= kEps) ? 1.0f : (sin(r) / r);
  const float vx = cos(r);
  const float vy = s * in.x;
  return float2(atan2(vy, vx), s * in.y / sqrt(vx * vx + vy * vy));
}

inline auto ApplyProjectionPanoramicFishEye(float2 in) -> float2 {
  const float s     = sin(in.x);
  const float r     = sqrt(s * s + in.y * in.y);
  const float theta = (r <= kEps) ? 0.0f : (atan2(r, cos(in.x)) / r);
  return float2(theta * s, theta * in.y);
}

inline auto ApplyProjectionERectRect(float2 in) -> float2 {
  float x = in.x;
  float y = in.y;
  float theta = -y + (kPi * 0.5f);
  if (theta < 0.0f) {
    theta = -theta;
    x += kPi;
  }
  if (theta > kPi) {
    theta = 2.0f * kPi - theta;
    x += kPi;
  }
  return float2(tan(x), 1.0f / (tan(theta) * cos(x)));
}

inline auto ApplyProjectionRectERect(float2 in) -> float2 {
  return float2(atan2(in.x, 1.0f), atan2(in.y, sqrt(1.0f + in.x * in.x)));
}

inline auto ApplyProjectionERectFishEye(float2 in) -> float2 {
  float x = in.x;
  float y = in.y;
  float theta = -y + (kPi * 0.5f);
  if (theta < 0.0f) {
    theta = -theta;
    x += kPi;
  }
  if (theta > kPi) {
    theta = 2.0f * kPi - theta;
    x += kPi;
  }
  const float s  = sin(theta);
  const float vx = s * sin(x);
  const float vy = cos(theta);
  const float r  = sqrt(vx * vx + vy * vy);
  theta          = atan2(r, s * cos(x));
  const float inv_r = (r <= kEps) ? 0.0f : (1.0f / r);
  return float2(theta * vx * inv_r, theta * vy * inv_r);
}

inline auto ApplyProjectionFishEyeERect(float2 in) -> float2 {
  const float r  = length(in);
  const float s  = (r <= kEps) ? 1.0f : (sin(r) / r);
  const float vx = cos(r);
  const float vy = s * in.x;
  return float2(atan2(vy, vx), atan(s * in.y / sqrt(vx * vx + vy * vy)));
}

inline auto ApplyProjectionERectPanoramic(float2 in) -> float2 {
  return float2(in.x, tan(in.y));
}

inline auto ApplyProjectionPanoramicERect(float2 in) -> float2 {
  return float2(in.x, atan(in.y));
}

inline auto ApplyProjectionOrthographicERect(float2 in) -> float2 {
  const float r     = length(in);
  const float theta = (r < 1.0f) ? asin(r) : (kPi * 0.5f);
  const float phi   = atan2(in.y, in.x);
  const float s     = (theta <= kEps) ? 1.0f : (sin(theta) / theta);
  const float vx    = cos(theta);
  const float vy    = s * theta * cos(phi);
  return float2(atan2(vy, vx), atan(s * theta * sin(phi) / sqrt(vx * vx + vy * vy)));
}

inline auto ApplyProjectionERectOrthographic(float2 in) -> float2 {
  float x = in.x;
  float y = in.y;
  float theta = -y + (kPi * 0.5f);
  if (theta < 0.0f) {
    theta = -theta;
    x += kPi;
  }
  if (theta > kPi) {
    theta = 2.0f * kPi - theta;
    x += kPi;
  }
  const float s      = sin(theta);
  const float vx     = s * sin(x);
  const float vy     = cos(theta);
  const float theta2 = atan2(sqrt(vx * vx + vy * vy), s * cos(x));
  const float phi2   = atan2(vy, vx);
  const float rho    = sin(theta2);
  return float2(rho * cos(phi2), rho * sin(phi2));
}

inline auto ApplyProjectionStereographicERect(float2 in) -> float2 {
  const float rh   = length(in);
  const float c    = 2.0f * atan(rh / 2.0f);
  const float sinc = sin(c);
  const float cosc = cos(c);

  float out_x = 0.0f;
  float out_y = 0.0f;
  if (fabs(rh) <= kEps) {
    out_y = kHuge;
  } else {
    out_y = asin(in.y * sinc / rh);
    out_x = (fabs(cosc) >= kEps || fabs(in.x) >= kEps) ? atan2(in.x * sinc, cosc * rh) : kHuge;
  }
  return float2(out_x, out_y);
}

inline auto ApplyProjectionERectStereographic(float2 in) -> float2 {
  const float cos_phi = cos(in.y);
  const float ksp     = 2.0f / (1.0f + cos_phi * cos(in.x));
  return float2(ksp * cos_phi * sin(in.x), ksp * sin(in.y));
}

inline auto ApplyProjectionEquisolidERect(float2 in) -> float2 {
  const float r     = length(in);
  const float theta = (r < 2.0f) ? (2.0f * asin(r / 2.0f)) : (kPi * 0.5f);
  const float phi   = atan2(in.y, in.x);
  const float s     = (theta <= kEps) ? 1.0f : (sin(theta) / theta);
  const float vx    = cos(theta);
  const float vy    = s * theta * cos(phi);
  return float2(atan2(vy, vx), atan(s * theta * sin(phi) / sqrt(vx * vx + vy * vy)));
}

inline auto ApplyProjectionERectEquisolid(float2 in) -> float2 {
  if (fabs(cos(in.y) * cos(in.x) + 1.0f) <= kEps) {
    return float2(kHuge, kHuge);
  }
  const float k1 = sqrt(2.0f / (1.0f + cos(in.y) * cos(in.x)));
  return float2(k1 * cos(in.y) * sin(in.x), k1 * sin(in.y));
}

inline auto ApplyProjectionThobyERect(float2 in) -> float2 {
  const float rho = length(in);
  if (rho < -kThobyK1 || rho > kThobyK1) {
    return float2(kHuge, kHuge);
  }
  const float theta = asin(rho / kThobyK1) / kThobyK2;
  const float phi   = atan2(in.y, in.x);
  const float s     = (theta <= kEps) ? 1.0f : (sin(theta) / theta);
  const float vx    = cos(theta);
  const float vy    = s * theta * cos(phi);
  return float2(atan2(vy, vx), atan(s * theta * sin(phi) / sqrt(vx * vx + vy * vy)));
}

inline auto ApplyProjectionERectThoby(float2 in) -> float2 {
  float x = in.x;
  float y = in.y;
  float theta = -y + (kPi * 0.5f);
  if (theta < 0.0f) {
    theta = -theta;
    x += kPi;
  }
  if (theta > kPi) {
    theta = 2.0f * kPi - theta;
    x += kPi;
  }

  const float s      = sin(theta);
  const float vx     = s * sin(x);
  const float vy     = cos(theta);
  const float theta2 = atan2(sqrt(vx * vx + vy * vy), s * cos(x));
  const float phi2   = atan2(vy, vx);
  const float rho    = kThobyK1 * sin(theta2 * kThobyK2);
  return float2(rho * cos(phi2), rho * sin(phi2));
}

inline auto ConvertProjectionToERect(float2 in, int projection) -> float2 {
  switch (projection) {
    case LensProjRectilinear:
      return ApplyProjectionRectERect(in);
    case LensProjFishEye:
      return ApplyProjectionFishEyeERect(in);
    case LensProjPanoramic:
      return ApplyProjectionPanoramicERect(in);
    case LensProjFishEyeOrthographic:
      return ApplyProjectionOrthographicERect(in);
    case LensProjFishEyeStereographic:
      return ApplyProjectionStereographicERect(in);
    case LensProjFishEyeEquisolid:
      return ApplyProjectionEquisolidERect(in);
    case LensProjFishEyeThoby:
      return ApplyProjectionThobyERect(in);
    case LensProjEquirectangular:
    case LensProjUnknown:
    default:
      return in;
  }
}

inline auto ConvertERectToProjection(float2 in, int projection) -> float2 {
  switch (projection) {
    case LensProjRectilinear:
      return ApplyProjectionERectRect(in);
    case LensProjFishEye:
      return ApplyProjectionERectFishEye(in);
    case LensProjPanoramic:
      return ApplyProjectionERectPanoramic(in);
    case LensProjFishEyeOrthographic:
      return ApplyProjectionERectOrthographic(in);
    case LensProjFishEyeStereographic:
      return ApplyProjectionERectStereographic(in);
    case LensProjFishEyeEquisolid:
      return ApplyProjectionERectEquisolid(in);
    case LensProjFishEyeThoby:
      return ApplyProjectionERectThoby(in);
    case LensProjEquirectangular:
    case LensProjUnknown:
    default:
      return in;
  }
}

inline auto ApplyProjectionTransform(float2 in, constant LensCalibGpuParams& p) -> float2 {
  if (p.apply_projection == 0) {
    return in;
  }
  if (p.target_projection == LensProjUnknown || p.source_projection == LensProjUnknown ||
      p.target_projection == p.source_projection) {
    return in;
  }
  return ConvertERectToProjection(ConvertProjectionToERect(in, p.target_projection),
                                  p.source_projection);
}

inline auto ApplyDistortion(float2 in, constant LensCalibGpuParams& p) -> float2 {
  if (p.apply_distortion == 0) {
    return in;
  }

  const float x   = in.x;
  const float y   = in.y;
  const float ru2 = x * x + y * y;

  switch (p.distortion_model) {
    case LensDistPoly3: {
      const float k1 = p.distortion_terms[0];
      const float poly = 1.0f + k1 * ru2;
      return float2(x * poly, y * poly);
    }
    case LensDistPoly5: {
      const float k1 = p.distortion_terms[0];
      const float k2 = p.distortion_terms[1];
      const float poly = 1.0f + k1 * ru2 + k2 * ru2 * ru2;
      return float2(x * poly, y * poly);
    }
    case LensDistPtLens: {
      const float a = p.distortion_terms[0];
      const float b = p.distortion_terms[1];
      const float c = p.distortion_terms[2];
      const float r = sqrt(ru2);
      const float poly = a * ru2 * r + b * ru2 + c * r + 1.0f;
      return float2(x * poly, y * poly);
    }
    case LensDistNone:
    default:
      return in;
  }
}

inline void ApplyTca(float2 in, thread float2& red, thread float2& blue,
                     constant LensCalibGpuParams& p) {
  red  = in;
  blue = in;
  if (p.apply_tca == 0) {
    return;
  }

  switch (p.tca_model) {
    case LensTcaLinear: {
      const float k_r = p.tca_terms[0];
      const float k_b = p.tca_terms[1];
      red             = float2(in.x * k_r, in.y * k_r);
      blue            = float2(in.x * k_b, in.y * k_b);
      return;
    }
    case LensTcaPoly3: {
      const float vr = p.tca_terms[0];
      const float vb = p.tca_terms[1];
      const float cr = p.tca_terms[2];
      const float cb = p.tca_terms[3];
      const float br = p.tca_terms[4];
      const float bb = p.tca_terms[5];
      const float r2 = in.x * in.x + in.y * in.y;
      const float rr = sqrt(r2);
      const float fr = br * r2 + cr * rr + vr;
      const float fb = bb * r2 + cb * rr + vb;
      red            = float2(in.x * fr, in.y * fr);
      blue           = float2(in.x * fb, in.y * fb);
      return;
    }
    case LensTcaNone:
    default:
      return;
  }
}

inline auto ReadWithBorder(device const float4* src, constant LensCalibDispatchParams& params,
                           int x, int y) -> float4 {
  if (x < 0 || y < 0 || x >= params.params.src_width || y >= params.params.src_height) {
    return float4(0.0f);
  }
  return src[static_cast<uint>(y) * params.src_stride + static_cast<uint>(x)];
}

inline auto BilinearSample(device const float4* src, constant LensCalibDispatchParams& params,
                           float sx, float sy) -> float4 {
  const int x0 = static_cast<int>(floor(sx));
  const int y0 = static_cast<int>(floor(sy));
  const int x1 = x0 + 1;
  const int y1 = y0 + 1;

  const float fx = sx - static_cast<float>(x0);
  const float fy = sy - static_cast<float>(y0);
  const float w00 = (1.0f - fx) * (1.0f - fy);
  const float w10 = fx * (1.0f - fy);
  const float w01 = (1.0f - fx) * fy;
  const float w11 = fx * fy;

  const float4 p00 = ReadWithBorder(src, params, x0, y0);
  const float4 p10 = ReadWithBorder(src, params, x1, y0);
  const float4 p01 = ReadWithBorder(src, params, x0, y1);
  const float4 p11 = ReadWithBorder(src, params, x1, y1);
  return p00 * w00 + p10 * w10 + p01 * w01 + p11 * w11;
}

inline auto BilinearSampleChannel(device const float4* src, constant LensCalibDispatchParams& params,
                                  float sx, float sy, int channel) -> float {
  const float4 pixel = BilinearSample(src, params, sx, sy);
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

inline auto ApplyScaleAndPerspective(float2 in, constant LensCalibGpuParams& p) -> float2 {
  float2 out = in;
  if (fabs(p.resolved_scale) > kEps) {
    const float inv_scale = 1.0f / p.resolved_scale;
    out *= inv_scale;
  }
  return out;
}

inline auto ApplyCircleCropAlpha(float4 in, uint x, uint y, constant LensCalibGpuParams& p) -> float4 {
  if (p.apply_crop_circle == 0) {
    return in;
  }

  const CropRectPx rect = ResolveCropRectPx(p);
  const float cx        = 0.5f * (rect.left + rect.right);
  const float cy        = 0.5f * (rect.top + rect.bottom);
  const float rx        = 0.5f * fabs(rect.right - rect.left);
  const float ry        = 0.5f * fabs(rect.bottom - rect.top);
  const float radius    = min(rx, ry);
  if (radius <= kEps) {
    return in;
  }

  const float dx = (static_cast<float>(x) + 0.5f) - cx;
  const float dy = (static_cast<float>(y) + 0.5f) - cy;
  if ((dx * dx + dy * dy) > (radius * radius)) {
    in.w = 0.0f;
  }
  return in;
}

kernel void lens_vignetting_rgba32f(device float4* image [[buffer(0)]],
                                    constant LensCalibDispatchParams& dispatch_params [[buffer(1)]],
                                    uint2 gid [[thread_position_in_grid]]) {
  constant LensCalibGpuParams& p = dispatch_params.params;
  if (gid.x >= static_cast<uint>(p.src_width) || gid.y >= static_cast<uint>(p.src_height)) {
    return;
  }

  const float2 pt = PixelToNormalized(static_cast<float>(gid.x), static_cast<float>(gid.y), p);
  const float  r2 = dot(pt, pt);
  const float  r4 = r2 * r2;
  const float  r6 = r4 * r2;
  const float  c  = 1.0f + p.vignetting_terms[0] * r2 + p.vignetting_terms[1] * r4 +
                   p.vignetting_terms[2] * r6;
  const float gain = (fabs(c) > kEps) ? (1.0f / c) : 1.0f;

  const uint index = gid.y * dispatch_params.src_stride + gid.x;
  float4     pix   = image[index];
  pix.xyz *= gain;
  image[index] = pix;
}

kernel void lens_warp_rgba32f(device const float4* src [[buffer(0)]],
                              device float4* dst [[buffer(1)]],
                              constant LensCalibDispatchParams& dispatch_params [[buffer(2)]],
                              uint2 gid [[thread_position_in_grid]]) {
  constant LensCalibGpuParams& p = dispatch_params.params;
  if (gid.x >= static_cast<uint>(p.dst_width) || gid.y >= static_cast<uint>(p.dst_height)) {
    return;
  }

  float2 g = PixelToNormalized(static_cast<float>(gid.x), static_cast<float>(gid.y), p);
  g        = ApplyScaleAndPerspective(g, p);
  g        = ApplyProjectionTransform(g, p);
  g        = ApplyDistortion(g, p);

  float2 r = g;
  float2 b = g;
  ApplyTca(g, r, b, p);

  const float2 gp = NormalizedToPixel(g, p);
  const float2 rp = NormalizedToPixel(r, p);
  const float2 bp = NormalizedToPixel(b, p);

  float4 out;
  if (p.apply_tca != 0) {
    out.x = BilinearSampleChannel(src, dispatch_params, rp.x, rp.y, 0);
    out.y = BilinearSampleChannel(src, dispatch_params, gp.x, gp.y, 1);
    out.z = BilinearSampleChannel(src, dispatch_params, bp.x, bp.y, 2);
    out.w = BilinearSampleChannel(src, dispatch_params, gp.x, gp.y, 3);
  } else {
    out = BilinearSample(src, dispatch_params, gp.x, gp.y);
  }

  out = ApplyCircleCropAlpha(out, gid.x, gid.y, p);
  dst[gid.y * dispatch_params.dst_stride + gid.x] = out;
}
