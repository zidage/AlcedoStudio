//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <metal_stdlib>

using namespace metal;

struct ResizeParams {
  uint  origin_x;
  uint  origin_y;
  uint  crop_width;
  uint  crop_height;
  uint  dst_width;
  uint  dst_height;
  float scale_x;
  float scale_y;
};

struct AffineParams {
  float  m00;
  float  m01;
  float  m02;
  float  m10;
  float  m11;
  float  m12;
  uint   src_width;
  uint   src_height;
  uint   dst_width;
  uint   dst_height;
  uint   src_stride;
  uint   dst_stride;
  float4 border;
};

struct WarpRectilinearParams {
  uint  coefficient_set_count;
  uint  width;
  uint  height;
  uint  src_stride;
  uint  dst_stride;
  float coefficient_sets[3][6];
  float center_x;
  float center_y;
};

template <typename PixelT>
struct PixelOps;

template <>
struct PixelOps<float> {
  using Acc = float;

  static inline auto Zero() -> Acc { return 0.0f; }
  static inline auto AddMul(Acc acc, float value, float weight) -> Acc {
    return fma(value, weight, acc);
  }
  static inline auto Div(Acc acc, float denom) -> float { return acc / denom; }
};

template <>
struct PixelOps<float4> {
  using Acc = float4;

  static inline auto Zero() -> Acc { return float4(0.0f); }
  static inline auto AddMul(Acc acc, float4 value, float weight) -> Acc {
    return fma(value, float4(weight), acc);
  }
  static inline auto Div(Acc acc, float denom) -> float4 { return acc / denom; }
};

template <typename PixelT>
static inline auto BorderValue(constant AffineParams& params) -> PixelT;

template <>
inline auto BorderValue<float>(constant AffineParams& params) -> float {
  return params.border.x;
}

template <>
inline auto BorderValue<float4>(constant AffineParams& params) -> float4 {
  return params.border;
}

template <typename PixelT>
static inline auto ReadOrBorder(device const PixelT* src, constant AffineParams& params, int x, int y)
    -> PixelT {
  if (x < 0 || y < 0 || x >= static_cast<int>(params.src_width) ||
      y >= static_cast<int>(params.src_height)) {
    return BorderValue<PixelT>(params);
  }
  return src[static_cast<uint>(y) * params.src_stride + static_cast<uint>(x)];
}

template <typename PixelT>
static inline auto BilinearSampleAffine(device const PixelT* src, constant AffineParams& params,
                                        float sx, float sy) -> PixelT {
  using Ops = PixelOps<PixelT>;
  using Acc = typename Ops::Acc;

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

  const PixelT p00 = ReadOrBorder(src, params, x0, y0);
  const PixelT p10 = ReadOrBorder(src, params, x1, y0);
  const PixelT p01 = ReadOrBorder(src, params, x0, y1);
  const PixelT p11 = ReadOrBorder(src, params, x1, y1);

  Acc acc = Ops::Zero();
  acc     = Ops::AddMul(acc, p00, w00);
  acc     = Ops::AddMul(acc, p10, w10);
  acc     = Ops::AddMul(acc, p01, w01);
  acc     = Ops::AddMul(acc, p11, w11);
  return Ops::Div(acc, 1.0f);
}

template <typename PixelT>
static inline void WarpAffineLinear(device const PixelT* src, device PixelT* dst,
                                    constant AffineParams& params, uint2 gid) {
  if (gid.x >= params.dst_width || gid.y >= params.dst_height) {
    return;
  }

  const float sx = params.m00 * static_cast<float>(gid.x) +
                   params.m01 * static_cast<float>(gid.y) + params.m02;
  const float sy = params.m10 * static_cast<float>(gid.x) +
                   params.m11 * static_cast<float>(gid.y) + params.m12;
  dst[gid.y * params.dst_stride + gid.x] = BilinearSampleAffine(src, params, sx, sy);
}

static inline auto ReadOrZero(device const float4* src, constant WarpRectilinearParams& params,
                              int x, int y) -> float4 {
  if (x < 0 || y < 0 || x >= static_cast<int>(params.width) ||
      y >= static_cast<int>(params.height)) {
    return float4(0.0f);
  }
  return src[static_cast<uint>(y) * params.src_stride + static_cast<uint>(x)];
}

static inline auto BilinearSampleWarp(device const float4* src,
                                      constant WarpRectilinearParams& params, float sx, float sy)
    -> float4 {
  const int   x0  = static_cast<int>(floor(sx));
  const int   y0  = static_cast<int>(floor(sy));
  const int   x1  = x0 + 1;
  const int   y1  = y0 + 1;
  const float fx  = sx - static_cast<float>(x0);
  const float fy  = sy - static_cast<float>(y0);
  const float w00 = (1.0f - fx) * (1.0f - fy);
  const float w10 = fx * (1.0f - fy);
  const float w01 = (1.0f - fx) * fy;
  const float w11 = fx * fy;

  const float4 p00 = ReadOrZero(src, params, x0, y0);
  const float4 p10 = ReadOrZero(src, params, x1, y0);
  const float4 p01 = ReadOrZero(src, params, x0, y1);
  const float4 p11 = ReadOrZero(src, params, x1, y1);
  return p00 * w00 + p10 * w10 + p01 * w01 + p11 * w11;
}

static inline auto WarpRectilinearSourceCoord(uint x, uint y, uint plane,
                                              constant WarpRectilinearParams& params) -> float2 {
  const float x0 = 0.0f;
  const float y0 = 0.0f;
  const float x1 = static_cast<float>(max(static_cast<int>(params.width) - 1, 0));
  const float y1 = static_cast<float>(max(static_cast<int>(params.height) - 1, 0));
  const float cx = x0 + params.center_x * (x1 - x0);
  const float cy = y0 + params.center_y * (y1 - y0);
  const float mx = max(abs(x0 - cx), abs(x1 - cx));
  const float my = max(abs(y0 - cy), abs(y1 - cy));
  const float m  = sqrt(mx * mx + my * my);
  if (m <= 1e-8f) {
    return float2(static_cast<float>(x), static_cast<float>(y));
  }

  const uint   set_index = params.coefficient_set_count <= 1u ? 0u : min(plane, 2u);
  const float  dx        = (static_cast<float>(x) - cx) / m;
  const float  dy        = (static_cast<float>(y) - cy) / m;
  const float  r2        = dx * dx + dy * dy;
  const float  f         = params.coefficient_sets[set_index][0] +
                  params.coefficient_sets[set_index][1] * r2 +
                  params.coefficient_sets[set_index][2] * r2 * r2 +
                  params.coefficient_sets[set_index][3] * r2 * r2 * r2;
  const float dxr = f * dx;
  const float dyr = f * dy;
  const float dxt = params.coefficient_sets[set_index][4] * (2.0f * dx * dy) +
                    params.coefficient_sets[set_index][5] * (r2 + 2.0f * dx * dx);
  const float dyt = params.coefficient_sets[set_index][5] * (2.0f * dx * dy) +
                    params.coefficient_sets[set_index][4] * (r2 + 2.0f * dy * dy);
  return float2(cx + m * (dxr + dxt), cy + m * (dyr + dyt));
}

static inline auto ReadTextureWithinCrop(texture2d<float, access::read> src,
                                         constant ResizeParams& params, int x, int y) -> float4 {
  const int crop_x0 = static_cast<int>(params.origin_x);
  const int crop_y0 = static_cast<int>(params.origin_y);
  const int crop_x1 = static_cast<int>(params.origin_x + params.crop_width);
  const int crop_y1 = static_cast<int>(params.origin_y + params.crop_height);
  if (x < crop_x0 || y < crop_y0 || x >= crop_x1 || y >= crop_y1) {
    return float4(0.0f);
  }
  return src.read(uint2(static_cast<uint>(x), static_cast<uint>(y)));
}

static inline auto BilinearSampleTexture(texture2d<float, access::read> src,
                                         constant ResizeParams& params, float sx,
                                         float sy) -> float4 {
  const int   x0  = static_cast<int>(floor(sx));
  const int   y0  = static_cast<int>(floor(sy));
  const int   x1  = x0 + 1;
  const int   y1  = y0 + 1;
  const float fx  = sx - static_cast<float>(x0);
  const float fy  = sy - static_cast<float>(y0);
  const float w00 = (1.0f - fx) * (1.0f - fy);
  const float w10 = fx * (1.0f - fy);
  const float w01 = (1.0f - fx) * fy;
  const float w11 = fx * fy;
  return ReadTextureWithinCrop(src, params, x0, y0) * w00 +
         ReadTextureWithinCrop(src, params, x1, y0) * w10 +
         ReadTextureWithinCrop(src, params, x0, y1) * w01 +
         ReadTextureWithinCrop(src, params, x1, y1) * w11;
}

static inline void CropResizeLinearTexture(texture2d<float, access::read> src,
                                           texture2d<float, access::write> dst,
                                           constant ResizeParams& params, uint2 gid) {
  if (gid.x >= params.dst_width || gid.y >= params.dst_height) {
    return;
  }
  const float sx = static_cast<float>(params.origin_x) +
                   (static_cast<float>(gid.x) + 0.5f) * params.scale_x - 0.5f;
  const float sy = static_cast<float>(params.origin_y) +
                   (static_cast<float>(gid.y) + 0.5f) * params.scale_y - 0.5f;
  dst.write(BilinearSampleTexture(src, params, sx, sy), gid);
}

static inline void CropResizeAreaTexture(texture2d<float, access::read> src,
                                         texture2d<float, access::write> dst,
                                         constant ResizeParams& params, uint2 gid) {
  if (gid.x >= params.dst_width || gid.y >= params.dst_height) {
    return;
  }

  const float sx0 =
      static_cast<float>(params.origin_x) + static_cast<float>(gid.x) * params.scale_x;
  const float sx1 =
      static_cast<float>(params.origin_x) + static_cast<float>(gid.x + 1u) * params.scale_x;
  const float sy0 =
      static_cast<float>(params.origin_y) + static_cast<float>(gid.y) * params.scale_y;
  const float sy1 =
      static_cast<float>(params.origin_y) + static_cast<float>(gid.y + 1u) * params.scale_y;

  const int crop_x0 = static_cast<int>(params.origin_x);
  const int crop_y0 = static_cast<int>(params.origin_y);
  const int crop_x1 = static_cast<int>(params.origin_x + params.crop_width);
  const int crop_y1 = static_cast<int>(params.origin_y + params.crop_height);
  const int ix0     = max(crop_x0, static_cast<int>(floor(sx0)));
  const int ix1     = min(crop_x1, static_cast<int>(ceil(sx1)));
  const int iy0     = max(crop_y0, static_cast<int>(floor(sy0)));
  const int iy1     = min(crop_y1, static_cast<int>(ceil(sy1)));

  float4 acc   = float4(0.0f);
  float  total = 0.0f;
  for (int yy = iy0; yy < iy1; ++yy) {
    const float wy = max(0.0f, min(sy1, static_cast<float>(yy + 1)) -
                                   max(sy0, static_cast<float>(yy)));
    for (int xx = ix0; xx < ix1; ++xx) {
      const float wx = max(0.0f, min(sx1, static_cast<float>(xx + 1)) -
                                     max(sx0, static_cast<float>(xx)));
      const float weight = wx * wy;
      acc += src.read(uint2(static_cast<uint>(xx), static_cast<uint>(yy))) * weight;
      total += weight;
    }
  }

  if (total <= 1e-8f) {
    const int sx = clamp(static_cast<int>(sx0), crop_x0, crop_x1 - 1);
    const int sy = clamp(static_cast<int>(sy0), crop_y0, crop_y1 - 1);
    dst.write(src.read(uint2(static_cast<uint>(sx), static_cast<uint>(sy))), gid);
    return;
  }
  dst.write(acc / total, gid);
}

kernel void crop_resize_linear_r32f(texture2d<float, access::read> src [[texture(0)]],
                                    texture2d<float, access::write> dst [[texture(1)]],
                                    constant ResizeParams& params [[buffer(0)]],
                                    uint2 gid [[thread_position_in_grid]]) {
  CropResizeLinearTexture(src, dst, params, gid);
}

kernel void crop_resize_linear_rgba32f(texture2d<float, access::read> src [[texture(0)]],
                                       texture2d<float, access::write> dst [[texture(1)]],
                                       constant ResizeParams& params [[buffer(0)]],
                                       uint2 gid [[thread_position_in_grid]]) {
  CropResizeLinearTexture(src, dst, params, gid);
}

kernel void crop_resize_area_r32f(texture2d<float, access::read> src [[texture(0)]],
                                  texture2d<float, access::write> dst [[texture(1)]],
                                  constant ResizeParams& params [[buffer(0)]],
                                  uint2 gid [[thread_position_in_grid]]) {
  CropResizeAreaTexture(src, dst, params, gid);
}

kernel void crop_resize_area_rgba32f(texture2d<float, access::read> src [[texture(0)]],
                                     texture2d<float, access::write> dst [[texture(1)]],
                                     constant ResizeParams& params [[buffer(0)]],
                                     uint2 gid [[thread_position_in_grid]]) {
  CropResizeAreaTexture(src, dst, params, gid);
}

kernel void warp_affine_linear_r32f(device const float* src [[buffer(0)]],
                                    device float*       dst [[buffer(1)]],
                                    constant AffineParams& params [[buffer(2)]],
                                    uint2 gid [[thread_position_in_grid]]) {
  WarpAffineLinear<float>(src, dst, params, gid);
}

kernel void warp_affine_linear_rgba32f(device const float4* src [[buffer(0)]],
                                       device float4*       dst [[buffer(1)]],
                                       constant AffineParams& params [[buffer(2)]],
                                       uint2 gid [[thread_position_in_grid]]) {
  WarpAffineLinear<float4>(src, dst, params, gid);
}

kernel void warp_rectilinear_rgba32f(device const float4* src [[buffer(0)]],
                                     device float4*       dst [[buffer(1)]],
                                     constant WarpRectilinearParams& params [[buffer(2)]],
                                     uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= params.width || gid.y >= params.height) {
    return;
  }

  const float2 red   = WarpRectilinearSourceCoord(gid.x, gid.y, 0u, params);
  const float2 green = WarpRectilinearSourceCoord(gid.x, gid.y, 1u, params);
  const float2 blue  = WarpRectilinearSourceCoord(gid.x, gid.y, 2u, params);
  const float4 r_px  = BilinearSampleWarp(src, params, red.x, red.y);
  const float4 g_px  = BilinearSampleWarp(src, params, green.x, green.y);
  const float4 b_px  = BilinearSampleWarp(src, params, blue.x, blue.y);
  dst[gid.y * params.dst_stride + gid.x] = float4(r_px.x, g_px.y, b_px.z, g_px.w);
}

static inline auto ReadTextureOrZero(texture2d<float, access::read> src, int width, int height,
                                     int x, int y) -> float4 {
  if (x < 0 || y < 0 || x >= width || y >= height) {
    return float4(0.0f);
  }
  return src.read(uint2(static_cast<uint>(x), static_cast<uint>(y)));
}

static inline auto BilinearSampleTextureWarp(texture2d<float, access::read> src, int width,
                                             int height, float sx, float sy) -> float4 {
  const int   x0  = static_cast<int>(floor(sx));
  const int   y0  = static_cast<int>(floor(sy));
  const float fx  = sx - static_cast<float>(x0);
  const float fy  = sy - static_cast<float>(y0);
  const float w00 = (1.0f - fx) * (1.0f - fy);
  const float w10 = fx * (1.0f - fy);
  const float w01 = (1.0f - fx) * fy;
  const float w11 = fx * fy;
  return ReadTextureOrZero(src, width, height, x0, y0) * w00 +
         ReadTextureOrZero(src, width, height, x0 + 1, y0) * w10 +
         ReadTextureOrZero(src, width, height, x0, y0 + 1) * w01 +
         ReadTextureOrZero(src, width, height, x0 + 1, y0 + 1) * w11;
}

kernel void warp_rectilinear_tex_rgba32f(texture2d<float, access::read> src [[texture(0)]],
                                         texture2d<float, access::write> dst [[texture(1)]],
                                         constant WarpRectilinearParams& params [[buffer(0)]],
                                         uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= params.width || gid.y >= params.height) {
    return;
  }
  const int    width = static_cast<int>(params.width);
  const int    height = static_cast<int>(params.height);
  const float2 red    = WarpRectilinearSourceCoord(gid.x, gid.y, 0u, params);
  const float2 green  = WarpRectilinearSourceCoord(gid.x, gid.y, 1u, params);
  const float2 blue   = WarpRectilinearSourceCoord(gid.x, gid.y, 2u, params);
  const float4 r_px   = BilinearSampleTextureWarp(src, width, height, red.x, red.y);
  const float4 g_px   = BilinearSampleTextureWarp(src, width, height, green.x, green.y);
  const float4 b_px   = BilinearSampleTextureWarp(src, width, height, blue.x, blue.y);
  dst.write(float4(r_px.x, g_px.y, b_px.z, g_px.w), gid);
}
