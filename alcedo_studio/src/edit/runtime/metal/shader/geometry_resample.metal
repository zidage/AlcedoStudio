//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <metal_stdlib>

using namespace metal;

struct GeometryResampleParams {
  float m00;
  float m01;
  float m02;
  float m10;
  float m11;
  float m12;
  uint  decoded_width;
  uint  decoded_height;
  uint  render_width;
  uint  render_height;
  float border[4];
  uint  use_bicubic;
};

static inline float4 ReadBorder(texture2d<float, access::read> src, int width, int height, int x,
                                int y, float4 border) {
  if (x < 0 || y < 0 || x >= width || y >= height) {
    return border;
  }
  return src.read(uint2(static_cast<uint>(x), static_cast<uint>(y)));
}

static inline float4 BilinearSample(texture2d<float, access::read> src, int width, int height,
                                    float sx, float sy, float4 border) {
  const float px = sx - 0.5f;
  const float py = sy - 0.5f;
  const int   x0 = static_cast<int>(floor(px));
  const int   y0 = static_cast<int>(floor(py));
  const float fx = px - static_cast<float>(x0);
  const float fy = py - static_cast<float>(y0);
  const float4 p00 = ReadBorder(src, width, height, x0, y0, border);
  const float4 p10 = ReadBorder(src, width, height, x0 + 1, y0, border);
  const float4 p01 = ReadBorder(src, width, height, x0, y0 + 1, border);
  const float4 p11 = ReadBorder(src, width, height, x0 + 1, y0 + 1, border);
  const float  w00 = (1.0f - fx) * (1.0f - fy);
  const float  w10 = fx * (1.0f - fy);
  const float  w01 = (1.0f - fx) * fy;
  const float  w11 = fx * fy;
  return p00 * w00 + p10 * w10 + p01 * w01 + p11 * w11;
}

static inline float CubicWeight(float x) {
  x = fabs(x);
  if (x < 1.0f) {
    return ((1.5f * x - 2.5f) * x) * x + 1.0f;
  }
  if (x < 2.0f) {
    return (((-0.5f * x + 2.5f) * x) - 4.0f) * x + 2.0f;
  }
  return 0.0f;
}

static inline float4 BicubicSample(texture2d<float, access::read> src, int width, int height,
                                   float sx, float sy, float4 border) {
  const float px   = sx - 0.5f;
  const float py   = sy - 0.5f;
  const int   x0   = static_cast<int>(floor(px));
  const int   y0   = static_cast<int>(floor(py));
  const float fx   = px - static_cast<float>(x0);
  const float fy   = py - static_cast<float>(y0);
  float4      acc  = float4(0.0f);
  float       wsum = 0.0f;
  for (int j = -1; j <= 2; ++j) {
    const float wy = CubicWeight(static_cast<float>(j) - fy);
    for (int i = -1; i <= 2; ++i) {
      const float  wx = CubicWeight(static_cast<float>(i) - fx);
      const float  w  = wx * wy;
      acc += ReadBorder(src, width, height, x0 + i, y0 + j, border) * w;
      wsum += w;
    }
  }
  if (wsum <= 1.0e-8f) {
    return border;
  }
  return acc / wsum;
}

kernel void geometry_resample_rgba32f(texture2d<float, access::read> src [[texture(0)]],
                                      texture2d<float, access::write> dst [[texture(1)]],
                                      constant GeometryResampleParams& params [[buffer(0)]],
                                      uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= params.render_width || gid.y >= params.render_height) {
    return;
  }
  const float  cx     = static_cast<float>(gid.x) + 0.5f;
  const float  cy     = static_cast<float>(gid.y) + 0.5f;
  const float  sx     = params.m00 * cx + params.m01 * cy + params.m02;
  const float  sy     = params.m10 * cx + params.m11 * cy + params.m12;
  const float4 border = float4(params.border[0], params.border[1], params.border[2],
                               params.border[3]);
  const int    width  = static_cast<int>(params.decoded_width);
  const int    height = static_cast<int>(params.decoded_height);
  const float4 pixel  = params.use_bicubic != 0u
                            ? BicubicSample(src, width, height, sx, sy, border)
                            : BilinearSample(src, width, height, sx, sy, border);
  dst.write(pixel, gid);
}
