//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only

typedef struct {
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
} GeometryResampleParams;

static inline float4 ReadBorder(__read_only image2d_t src, int width, int height, int x, int y,
                                float4 border) {
  if (x < 0 || y < 0 || x >= width || y >= height) {
    return border;
  }
  const sampler_t nearest =
      CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST;
  return read_imagef(src, nearest, (int2)(x, y));
}

static inline float4 BilinearSample(__read_only image2d_t src, int width, int height, float sx,
                                    float sy, float4 border) {
  const float px = sx - 0.5f;
  const float py = sy - 0.5f;
  const int   x0 = (int)floor(px);
  const int   y0 = (int)floor(py);
  const float fx = px - (float)x0;
  const float fy = py - (float)y0;
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

static inline float4 BicubicSample(__read_only image2d_t src, int width, int height, float sx,
                                   float sy, float4 border) {
  const float px   = sx - 0.5f;
  const float py   = sy - 0.5f;
  const int   x0   = (int)floor(px);
  const int   y0   = (int)floor(py);
  const float fx   = px - (float)x0;
  const float fy   = py - (float)y0;
  float4      acc  = (float4)(0.0f);
  float       wsum = 0.0f;
  for (int j = -1; j <= 2; ++j) {
    const float wy = CubicWeight((float)j - fy);
    for (int i = -1; i <= 2; ++i) {
      const float wx = CubicWeight((float)i - fx);
      const float w  = wx * wy;
      acc += ReadBorder(src, width, height, x0 + i, y0 + j, border) * w;
      wsum += w;
    }
  }
  if (wsum <= 1.0e-8f) {
    return border;
  }
  return acc / wsum;
}

__kernel void geometry_resample_rgba32f(__read_only image2d_t src, __write_only image2d_t dst,
                                        GeometryResampleParams params) {
  const uint x = get_global_id(0);
  const uint y = get_global_id(1);
  if (x >= params.render_width || y >= params.render_height) {
    return;
  }
  const float  cx     = (float)x + 0.5f;
  const float  cy     = (float)y + 0.5f;
  const float  sx     = params.m00 * cx + params.m01 * cy + params.m02;
  const float  sy     = params.m10 * cx + params.m11 * cy + params.m12;
  const float4 border = (float4)(params.border[0], params.border[1], params.border[2],
                                 params.border[3]);
  const int    width  = (int)params.decoded_width;
  const int    height = (int)params.decoded_height;
  const float4 pixel  = params.use_bicubic != 0u
                            ? BicubicSample(src, width, height, sx, sy, border)
                            : BilinearSample(src, width, height, sx, sy, border);
  write_imagef(dst, (int2)((int)x, (int)y), pixel);
}

static inline float AcesccEncode(float value) {
  const float kA          = 9.72f;
  const float kB          = 17.52f;
  const float kOffset     = 0.0000152587890625f;
  const float kTransition = 0.000030517578125f;
  const float kFloor      = (-16.0f + kA) / kB;
  if (value < 0.0f) {
    return kFloor + value;
  }
  if (value < kTransition) {
    return (log2(kOffset + value * 0.5f) + kA) / kB;
  }
  return (log2(value) + kA) / kB;
}

__kernel void camera_color_acescc(__read_only image2d_t src, __write_only image2d_t dst,
                                  global const float* camera_params, uint offset_floats,
                                  global const float* dng_profile) {
  const int2 gid  = (int2)((int)get_global_id(0), (int)get_global_id(1));
  const int2 size = get_image_dim(src);
  if (gid.x >= size.x || gid.y >= size.y) {
    return;
  }
  const sampler_t nearest =
      CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST;
  const float4 source = read_imagef(src, nearest, gid);
  const global float* m = camera_params + offset_floats;
  const float x = m[0] * source.x + m[1] * source.y + m[2] * source.z;
  const float y = m[3] * source.x + m[4] * source.y + m[5] * source.z;
  const float z = m[6] * source.x + m[7] * source.y + m[8] * source.z;
  const DngRgb        corrected = DngApplyColorProfile(DngMakeRgb(x, y, z), dng_profile);
  write_imagef(dst, gid,
               (float4)(AcesccEncode(corrected.r), AcesccEncode(corrected.g),
                        AcesccEncode(corrected.b), source.w));
}

typedef struct {
  uint  coefficient_set_count;
  uint  width;
  uint  height;
  float coefficient_sets[3][6];
  float center_x;
  float center_y;
} OpenClImageWarpParams;

static inline float2 WarpRectilinearSource(int x, int y, int plane, OpenClImageWarpParams p) {
  const float x0 = 0.0f;
  const float y0 = 0.0f;
  const float x1 = (float)max((int)p.width - 1, 0);
  const float y1 = (float)max((int)p.height - 1, 0);
  const float cx = x0 + p.center_x * (x1 - x0);
  const float cy = y0 + p.center_y * (y1 - y0);
  const float mx = fmax(fabs(x0 - cx), fabs(x1 - cx));
  const float my = fmax(fabs(y0 - cy), fabs(y1 - cy));
  const float m  = sqrt(mx * mx + my * my);
  if (m <= 1.0e-8f) {
    return (float2)((float)x, (float)y);
  }
  const uint  set_index = p.coefficient_set_count <= 1u ? 0u : (uint)min(max(plane, 0), 2);
  const float dx        = ((float)x - cx) / m;
  const float dy        = ((float)y - cy) / m;
  const float r2        = dx * dx + dy * dy;
  const float f         = p.coefficient_sets[set_index][0] + p.coefficient_sets[set_index][1] * r2 +
                  p.coefficient_sets[set_index][2] * r2 * r2 +
                  p.coefficient_sets[set_index][3] * r2 * r2 * r2;
  const float dxr = f * dx;
  const float dyr = f * dy;
  const float dxt = p.coefficient_sets[set_index][4] * (2.0f * dx * dy) +
                    p.coefficient_sets[set_index][5] * (r2 + 2.0f * dx * dx);
  const float dyt = p.coefficient_sets[set_index][5] * (2.0f * dx * dy) +
                    p.coefficient_sets[set_index][4] * (r2 + 2.0f * dy * dy);
  return (float2)(cx + m * (dxr + dxt), cy + m * (dyr + dyt));
}

__kernel void warp_rectilinear_rgba32f(__read_only image2d_t src, __write_only image2d_t dst,
                                       OpenClImageWarpParams params) {
  const int x = (int)get_global_id(0);
  const int y = (int)get_global_id(1);
  if (x >= (int)params.width || y >= (int)params.height) {
    return;
  }
  const float4 border = (float4)(0.0f, 0.0f, 0.0f, 1.0f);
  const float2 red    = WarpRectilinearSource(x, y, 0, params);
  const float2 green  = WarpRectilinearSource(x, y, 1, params);
  const float2 blue   = WarpRectilinearSource(x, y, 2, params);
  const int    width  = (int)params.width;
  const int    height = (int)params.height;
  float4       out;
  out.x = BilinearSample(src, width, height, red.x + 0.5f, red.y + 0.5f, border).x;
  out.y = BilinearSample(src, width, height, green.x + 0.5f, green.y + 0.5f, border).y;
  out.z = BilinearSample(src, width, height, blue.x + 0.5f, blue.y + 0.5f, border).z;
  out.w = BilinearSample(src, width, height, green.x + 0.5f, green.y + 0.5f, border).w;
  write_imagef(dst, (int2)(x, y), out);
}
