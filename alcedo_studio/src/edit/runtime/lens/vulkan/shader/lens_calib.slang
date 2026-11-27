//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group lens_calib: the Slang form of the vignetting and warp kernels of
// edit/runtime/lens/opencl/shader/lens_calib.cl (plan phase VK5), on RGBA32F storage images. The
// parameter record is LensCalibGpuParams (edit/runtime/lens/lens_calib_runtime.hpp) in a buffer:
// it is above the 128 push constant bytes. The names, the bindings, and the push constant layout
// are in include/vulkan/vulkan_lens_calib_kernels.hpp.

#include "vulkan/shader/math.slangh"

// LensCalibGpuParams: 268 bytes, the layout of the host struct (a flat record: no struct member,
// plan section 4.2).
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

// LensCalibPushConstants: 8 bytes, the address of one LensCalibGpuParams record.
struct LensCalibPushConstants {
  LensCalibGpuParams* params;
};

// edit_geometry_lens_vignetting_rgba32f: the image, read and written in place.
[[vk::binding(0, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> image;
// edit_geometry_lens_warp_rgba32f: the source (binding 0, the same binding as `image`) and the
// destination (binding 1).
[[vk::binding(1, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> destination;

static const float kPi       = 3.1415926535897932f;
static const float kHalfPi   = 1.5707963267948966f;
static const float kTwoPi    = 6.2831853071795864f;
static const float kPiSquare = 9.8696044010893586f;
static const float kFarAway  = 1.6e16f;

static float2 PixelToNormalized(float x, float y, LensCalibGpuParams* p) {
  return float2(x * p->norm_scale - p->center_x, y * p->norm_scale - p->center_y);
}

static float2 NormalizedToPixel(float2 pt, LensCalibGpuParams* p) {
  return float2((pt.x + p->center_x) * p->norm_unscale, (pt.y + p->center_y) * p->norm_unscale);
}

// ---- Projections (Lensfun), the functions of lens_calib.cl ---------------------------------

static float2 RectToErect(float2 v) {
  return float2(AlcedoAtan2(v.x, 1.0f), AlcedoAtan2(v.y, sqrt(1.0f + v.x * v.x)));
}

static float2 FisheyeToErect(float2 v) {
  const float r  = length(v);
  const float s  = (r <= 1.0e-8f) ? 1.0f : (AlcedoSin(r) / r);
  const float vx = AlcedoCos(r);
  const float vy = s * v.x;
  return float2(AlcedoAtan2(vy, vx), AlcedoAtan(s * v.y / sqrt(vx * vx + vy * vy)));
}

static float2 PanoramicToErect(float2 v) {
  return float2(v.x, AlcedoAtan(v.y));
}

// The common end of the orthographic, equisolid, and Thoby conversions to erect.
static float2 PolarThetaToErect(float theta, float2 v) {
  const float phi = AlcedoAtan2(v.y, v.x);
  const float s   = (theta <= 1.0e-8f) ? 1.0f : (AlcedoSin(theta) / theta);
  const float vx  = AlcedoCos(theta);
  const float vy  = s * theta * AlcedoCos(phi);
  return float2(AlcedoAtan2(vy, vx), AlcedoAtan(s * theta * AlcedoSin(phi) / sqrt(vx * vx + vy * vy)));
}

static float2 OrthographicToErect(float2 v) {
  const float r = length(v);
  return PolarThetaToErect((r < 1.0f) ? AlcedoAsin(r) : kHalfPi, v);
}

static float2 StereographicToErect(float2 v) {
  const float rh    = length(v);
  const float c     = 2.0f * AlcedoAtan(rh / 2.0f);
  const float sinc  = AlcedoSin(c);
  const float cosc  = AlcedoCos(c);
  float       out_x = 0.0f;
  float       out_y = 0.0f;
  if (abs(rh) <= 1.0e-8f) {
    out_y = kFarAway;
  } else {
    out_y = AlcedoAsin(v.y * sinc / rh);
    if (abs(cosc) >= 1.0e-8f || abs(v.x) >= 1.0e-8f) {
      out_x = AlcedoAtan2(v.x * sinc, cosc * rh);
    } else {
      out_x = kFarAway;
    }
  }
  return float2(out_x, out_y);
}

static float2 EquisolidToErect(float2 v) {
  const float r = length(v);
  return PolarThetaToErect((r < 2.0f) ? 2.0f * AlcedoAsin(r / 2.0f) : kHalfPi, v);
}

static float2 ThobyToErect(float2 v) {
  const float rho = length(v);
  if (rho > 1.47f) {
    return float2(kFarAway, kFarAway);
  }
  return PolarThetaToErect(AlcedoAsin(rho / 1.47f) / 0.713f, v);
}

// The folded polar angle and longitude of an erect position (the first lines of each
// erect_to_* function of lens_calib.cl).
static void FoldErect(float2 v, out float theta, out float x) {
  theta = -v.y + kHalfPi;
  x     = v.x;
  if (theta < 0.0f) {
    theta = -theta;
    x += kPi;
  }
  if (theta > kPi) {
    theta = kTwoPi - theta;
    x += kPi;
  }
}

static float2 ErectToRect(float2 v) {
  float theta;
  float x;
  FoldErect(v, theta, x);
  return float2(AlcedoTan(x), 1.0f / (AlcedoTan(theta) * AlcedoCos(x)));
}

static float2 ErectToFisheye(float2 v) {
  float theta;
  float x;
  FoldErect(v, theta, x);
  const float s     = AlcedoSin(theta);
  const float vx    = s * AlcedoSin(x);
  const float vy    = AlcedoCos(theta);
  const float r     = sqrt(vx * vx + vy * vy);
  const float t     = AlcedoAtan2(r, s * AlcedoCos(x));
  const float inv_r = (r <= 1.0e-8f) ? 0.0f : (1.0f / r);
  return float2(t * vx * inv_r, t * vy * inv_r);
}

static float2 ErectToPanoramic(float2 v) {
  return float2(v.x, AlcedoTan(v.y));
}

// theta2 and phi2 of the erect to orthographic and Thoby conversions.
static float2 ErectToPolar(float2 v) {
  float theta;
  float x;
  FoldErect(v, theta, x);
  const float s  = AlcedoSin(theta);
  const float vx = s * AlcedoSin(x);
  const float vy = AlcedoCos(theta);
  return float2(AlcedoAtan2(sqrt(vx * vx + vy * vy), s * AlcedoCos(x)), AlcedoAtan2(vy, vx));
}

static float2 ErectToOrthographic(float2 v) {
  const float2 polar = ErectToPolar(v);
  const float  rho   = AlcedoSin(polar.x);
  return float2(rho * AlcedoCos(polar.y), rho * AlcedoSin(polar.y));
}

static float2 ErectToStereographic(float2 v) {
  const float cos_phi = AlcedoCos(v.y);
  const float ksp     = 2.0f / (1.0f + cos_phi * AlcedoCos(v.x));
  return float2(ksp * cos_phi * AlcedoSin(v.x), ksp * AlcedoSin(v.y));
}

static float2 ErectToEquisolid(float2 v) {
  if (abs(AlcedoCos(v.y) * AlcedoCos(v.x) + 1.0f) <= 1.0e-8f) {
    return float2(kFarAway, kFarAway);
  }
  const float k1 = sqrt(2.0f / (1.0f + AlcedoCos(v.y) * AlcedoCos(v.x)));
  return float2(k1 * AlcedoCos(v.y) * AlcedoSin(v.x), k1 * AlcedoSin(v.y));
}

static float2 ErectToThoby(float2 v) {
  const float2 polar = ErectToPolar(v);
  const float  rho   = 1.47f * AlcedoSin(polar.x * 0.713f);
  return float2(rho * AlcedoCos(polar.y), rho * AlcedoSin(polar.y));
}

static float2 ConvertToErect(float2 v, int projection) {
  switch (projection) {
    case 1:
      return RectToErect(v);
    case 2:
      return FisheyeToErect(v);
    case 3:
      return PanoramicToErect(v);
    case 5:
      return OrthographicToErect(v);
    case 6:
      return StereographicToErect(v);
    case 7:
      return EquisolidToErect(v);
    case 8:
      return ThobyToErect(v);
    default:  // 0 unknown, 4 equirectangular
      return v;
  }
}

static float2 ConvertFromErect(float2 v, int projection) {
  switch (projection) {
    case 1:
      return ErectToRect(v);
    case 2:
      return ErectToFisheye(v);
    case 3:
      return ErectToPanoramic(v);
    case 5:
      return ErectToOrthographic(v);
    case 6:
      return ErectToStereographic(v);
    case 7:
      return ErectToEquisolid(v);
    case 8:
      return ErectToThoby(v);
    default:
      return v;
  }
}

static float2 ApplyProjectionTransform(float2 v, LensCalibGpuParams* p) {
  if (p->apply_projection == 0 || p->target_projection == 0 || p->source_projection == 0 ||
      p->target_projection == p->source_projection) {
    return v;
  }
  return ConvertFromErect(ConvertToErect(v, p->target_projection), p->source_projection);
}

// ---- Distortion, TCA, scale ----------------------------------------------------------------

static float2 ApplyDistortion(float2 v, LensCalibGpuParams* p) {
  if (p->apply_distortion == 0) {
    return v;
  }
  const float ru2 = v.x * v.x + v.y * v.y;
  if (p->distortion_model == 1) {
    const float poly = 1.0f + p->distortion_terms[0] * ru2;
    return v * poly;
  }
  if (p->distortion_model == 2) {
    const float poly = 1.0f + p->distortion_terms[0] * ru2 + p->distortion_terms[1] * ru2 * ru2;
    return v * poly;
  }
  if (p->distortion_model == 3) {
    const float r    = sqrt(ru2);
    const float poly = p->distortion_terms[0] * ru2 * r + p->distortion_terms[1] * ru2 +
                       p->distortion_terms[2] * r + 1.0f;
    return v * poly;
  }
  return v;
}

static void ApplyTca(float2 v, out float2 red, out float2 blue, LensCalibGpuParams* p) {
  red  = v;
  blue = v;
  if (p->apply_tca == 0) {
    return;
  }
  if (p->tca_model == 1) {
    red  = v * p->tca_terms[0];
    blue = v * p->tca_terms[1];
    return;
  }
  if (p->tca_model == 2) {
    const float r2 = dot(v, v);
    const float rr = sqrt(r2);
    red            = v * (p->tca_terms[4] * r2 + p->tca_terms[2] * rr + p->tca_terms[0]);
    blue           = v * (p->tca_terms[5] * r2 + p->tca_terms[3] * rr + p->tca_terms[1]);
  }
}

static float2 ApplyScale(float2 v, LensCalibGpuParams* p) {
  if (abs(p->resolved_scale) > 1.0e-8f) {
    return v * (1.0f / p->resolved_scale);
  }
  return v;
}

// ---- Lanczos-3 sampler (lens_lanczos3_note of lens_calib.cl) --------------------------------

// AlcedoSin(pi t) with the argument reduced exactly to [-0.5, 0.5] (the OpenCL sinpi), so the weights
// do not depend on the precision of the driver sin for |pi t| up to 3 pi.
static float SinPi(float t) {
  const float n = round(t);
  const float r = t - n;
  const float s = AlcedoSin(kPi * r);
  return (int(n) & 1) != 0 ? -s : s;
}

static float Lanczos3Weight(float t) {
  if (abs(t) < 1.0e-5f) {
    return 1.0f;
  }
  if (abs(t) >= 3.0f) {
    return 0.0f;
  }
  return 3.0f * SinPi(t) * SinPi(t * (1.0f / 3.0f)) / (kPiSquare * t * t);
}

struct Lanczos3Weights {
  float w0;
  float w1;
  float w2;
  float w3;
  float w4;
  float w5;
};

// The six weights of the taps at offsets -2 to 3 from floor(s), normalized to a sum of 1.
static Lanczos3Weights MakeLanczos3Weights(float f) {
  Lanczos3Weights w;
  w.w0            = Lanczos3Weight(f + 2.0f);
  w.w1            = Lanczos3Weight(f + 1.0f);
  w.w2            = Lanczos3Weight(f);
  w.w3            = Lanczos3Weight(f - 1.0f);
  w.w4            = Lanczos3Weight(f - 2.0f);
  w.w5            = Lanczos3Weight(f - 3.0f);
  const float sum = w.w0 + w.w1 + w.w2 + w.w3 + w.w4 + w.w5;
  const float inv = 1.0f / sum;
  w.w0 *= inv;
  w.w1 *= inv;
  w.w2 *= inv;
  w.w3 *= inv;
  w.w4 *= inv;
  w.w5 *= inv;
  return w;
}

static float BorderCoverage(float s, int size) {
  return clamp(min(s + 1.0f, float(size) - s), 0.0f, 1.0f);
}

static float4 ReadClamped(int width, int height, int x, int y) {
  return image[uint2(uint(clamp(x, 0, width - 1)), uint(clamp(y, 0, height - 1)))];
}

static float4 Lanczos3Row(int width, int height, int x0, int y, Lanczos3Weights wx) {
  return ReadClamped(width, height, x0 - 2, y) * wx.w0 +
         ReadClamped(width, height, x0 - 1, y) * wx.w1 + ReadClamped(width, height, x0, y) * wx.w2 +
         ReadClamped(width, height, x0 + 1, y) * wx.w3 +
         ReadClamped(width, height, x0 + 2, y) * wx.w4 +
         ReadClamped(width, height, x0 + 3, y) * wx.w5;
}

static float4 Lanczos3Sample(int width, int height, float sx, float sy) {
  const float coverage = BorderCoverage(sx, width) * BorderCoverage(sy, height);
  // The negated test also returns zero for a NaN position.
  if (!(coverage > 0.0f)) {
    return float4(0.0f);
  }
  const int             x0 = int(floor(sx));
  const int             y0 = int(floor(sy));
  const Lanczos3Weights wx = MakeLanczos3Weights(sx - float(x0));
  const Lanczos3Weights wy = MakeLanczos3Weights(sy - float(y0));

  float4 acc = float4(0.0f);
  acc += Lanczos3Row(width, height, x0, y0 - 2, wx) * wy.w0;
  acc += Lanczos3Row(width, height, x0, y0 - 1, wx) * wy.w1;
  acc += Lanczos3Row(width, height, x0, y0, wx) * wy.w2;
  acc += Lanczos3Row(width, height, x0, y0 + 1, wx) * wy.w3;
  acc += Lanczos3Row(width, height, x0, y0 + 2, wx) * wy.w4;
  acc += Lanczos3Row(width, height, x0, y0 + 3, wx) * wy.w5;

  const float4 p00 = ReadClamped(width, height, x0, y0);
  const float4 p10 = ReadClamped(width, height, x0 + 1, y0);
  const float4 p01 = ReadClamped(width, height, x0, y0 + 1);
  const float4 p11 = ReadClamped(width, height, x0 + 1, y0 + 1);
  const float4 lo  = min(min(p00, p10), min(p01, p11));
  const float4 hi  = max(max(p00, p10), max(p01, p11));
  return clamp(acc, lo, hi) * coverage;
}

// ---- Circle crop ---------------------------------------------------------------------------

struct CropRectPx {
  float left;
  float right;
  float top;
  float bottom;
};

static CropRectPx ResolveCropRectPx(LensCalibGpuParams* p) {
  CropRectPx  rect   = { 0.0f, 0.0f, 0.0f, 0.0f };
  const float width  = float(p->dst_width);
  const float height = float(p->dst_height);
  if (width <= 0.0f || height <= 0.0f) {
    return rect;
  }
  if (p->dst_width >= p->dst_height) {
    rect.left   = p->crop_bounds[0] * width;
    rect.right  = p->crop_bounds[1] * width;
    rect.top    = p->crop_bounds[2] * height;
    rect.bottom = p->crop_bounds[3] * height;
  } else {
    rect.left   = p->crop_bounds[2] * width;
    rect.right  = p->crop_bounds[3] * width;
    rect.top    = p->crop_bounds[0] * height;
    rect.bottom = p->crop_bounds[1] * height;
  }
  if (rect.left > rect.right) {
    const float tmp = rect.left;
    rect.left       = rect.right;
    rect.right      = tmp;
  }
  if (rect.top > rect.bottom) {
    const float tmp = rect.top;
    rect.top        = rect.bottom;
    rect.bottom     = tmp;
  }
  return rect;
}

static float4 ApplyCircleCropAlpha(float4 v, int x, int y, LensCalibGpuParams* p) {
  if (p->apply_crop_circle == 0) {
    return v;
  }
  const CropRectPx rect   = ResolveCropRectPx(p);
  const float      cx     = 0.5f * (rect.left + rect.right);
  const float      cy     = 0.5f * (rect.top + rect.bottom);
  const float      rx     = 0.5f * abs(rect.right - rect.left);
  const float      ry     = 0.5f * abs(rect.bottom - rect.top);
  const float      radius = min(rx, ry);
  if (radius <= 1.0e-8f) {
    return v;
  }
  const float dx = (float(x) + 0.5f) - cx;
  const float dy = (float(y) + 0.5f) - cy;
  if ((dx * dx + dy * dy) > (radius * radius)) {
    v.w = 0.0f;
  }
  return v;
}

// ---- Kernels -------------------------------------------------------------------------------

[shader("compute")]
[numthreads(16, 16, 1)]
void edit_geometry_lens_vignetting_rgba32f(uint3 id: SV_DispatchThreadID,
                                           uniform LensCalibPushConstants push) {
  LensCalibGpuParams* p = push.params;
  const int           x = int(id.x);
  const int           y = int(id.y);
  if (x >= p->dst_width || y >= p->dst_height) {
    return;
  }
  const float2 pt   = PixelToNormalized(float(x), float(y), p);
  const float  r2   = dot(pt, pt);
  const float  r4   = r2 * r2;
  const float  r6   = r4 * r2;
  const float  c    = 1.0f + p->vignetting_terms[0] * r2 + p->vignetting_terms[1] * r4 +
                  p->vignetting_terms[2] * r6;
  const float  gain = (abs(c) > 1.0e-8f) ? (1.0f / c) : 1.0f;
  float4       pix  = image[id.xy];
  pix.xyz *= gain;
  image[id.xy] = pix;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void edit_geometry_lens_warp_rgba32f(uint3 id: SV_DispatchThreadID,
                                     uniform LensCalibPushConstants push) {
  LensCalibGpuParams* p = push.params;
  const int           x = int(id.x);
  const int           y = int(id.y);
  if (x >= p->dst_width || y >= p->dst_height) {
    return;
  }
  float2 g = PixelToNormalized(float(x), float(y), p);
  g        = ApplyScale(g, p);
  g        = ApplyProjectionTransform(g, p);
  g        = ApplyDistortion(g, p);
  float2 r;
  float2 b;
  ApplyTca(g, r, b, p);

  const float2 gp = NormalizedToPixel(g, p);
  float4       v  = Lanczos3Sample(p->src_width, p->src_height, gp.x, gp.y);
  if (p->apply_tca != 0) {
    // Red and blue each sample at their own TCA position; green and alpha keep the green sample.
    const float2 rp = NormalizedToPixel(r, p);
    const float2 bp = NormalizedToPixel(b, p);
    v.x             = Lanczos3Sample(p->src_width, p->src_height, rp.x, rp.y).x;
    v.z             = Lanczos3Sample(p->src_width, p->src_height, bp.x, bp.y).z;
  }
  destination[id.xy] = ApplyCircleCropAlpha(v, x, y, p);
}
