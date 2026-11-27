//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group dng_warp: the DNG OpcodeList3 WarpRectilinear kernel warp_rectilinear_rgba32f
// of opencl_shader/geometry_camera.cl (plan phase VK5), on RGBA32F storage images. The name, the
// bindings, and the push constant layout are in include/vulkan/vulkan_dng_warp_kernels.hpp. The
// bilinear sample is the one of geometry_sample.slangh (phase VK7).

#include "geometry_sample.slangh"

// WarpRectilinearPushConstants: 92 bytes, the OpenCL OpenClImageWarpParams with one array for
// each coefficient set. A kernel selects a set with constant indices (plan section 4.2).
struct WarpRectilinearPushConstants {
  uint  coefficient_set_count;
  uint  width;
  uint  height;
  float set_r[6];  // the set of plane 0, and of every plane when the opcode has one set
  float set_g[6];
  float set_b[6];
  float center_x;
  float center_y;
};

[[vk::binding(0, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> source;
[[vk::binding(1, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> destination;

struct WarpCoefficients {
  float k0;
  float k1;
  float k2;
  float k3;
  float p0;  // tangential
  float p1;
};

static WarpCoefficients CoefficientsOfSet(float set[6]) {
  WarpCoefficients c;
  c.k0 = set[0];
  c.k1 = set[1];
  c.k2 = set[2];
  c.k3 = set[3];
  c.p0 = set[4];
  c.p1 = set[5];
  return c;
}

// The set of @p plane: plane 0, 1, or 2 when the opcode has three sets, else the first set.
static WarpCoefficients CoefficientsOf(WarpRectilinearPushConstants push, int plane) {
  if (push.coefficient_set_count > 1u && plane == 2) {
    return CoefficientsOfSet(push.set_b);
  }
  if (push.coefficient_set_count > 1u && plane == 1) {
    return CoefficientsOfSet(push.set_g);
  }
  return CoefficientsOfSet(push.set_r);
}

// The source position of output texel (x, y) for @p plane (the OpenCL WarpRectilinearSource).
static float2 WarpRectilinearSource(int x, int y, int plane, WarpRectilinearPushConstants push) {
  const float x0 = 0.0f;
  const float y0 = 0.0f;
  const float x1 = float(max(int(push.width) - 1, 0));
  const float y1 = float(max(int(push.height) - 1, 0));
  const float cx = x0 + push.center_x * (x1 - x0);
  const float cy = y0 + push.center_y * (y1 - y0);
  const float mx = max(abs(x0 - cx), abs(x1 - cx));
  const float my = max(abs(y0 - cy), abs(y1 - cy));
  const float m  = sqrt(mx * mx + my * my);
  if (m <= 1.0e-8f) {
    return float2(float(x), float(y));
  }
  const WarpCoefficients c   = CoefficientsOf(push, plane);
  const float            dx  = (float(x) - cx) / m;
  const float            dy  = (float(y) - cy) / m;
  const float            r2  = dx * dx + dy * dy;
  const float            f   = c.k0 + c.k1 * r2 + c.k2 * r2 * r2 + c.k3 * r2 * r2 * r2;
  const float            dxr = f * dx;
  const float            dyr = f * dy;
  const float            dxt = c.p0 * (2.0f * dx * dy) + c.p1 * (r2 + 2.0f * dx * dx);
  const float            dyt = c.p1 * (2.0f * dx * dy) + c.p0 * (r2 + 2.0f * dy * dy);
  return float2(cx + m * (dxr + dxt), cy + m * (dyr + dyt));
}

// Each channel samples at the position of its plane; alpha samples at the green position.
[shader("compute")]
[numthreads(16, 16, 1)]
void warp_rectilinear_rgba32f(uint3 id: SV_DispatchThreadID,
                              uniform WarpRectilinearPushConstants push) {
  const int x = int(id.x);
  const int y = int(id.y);
  if (x >= int(push.width) || y >= int(push.height)) {
    return;
  }
  const float4 border = float4(0.0f, 0.0f, 0.0f, 1.0f);
  const float2 red    = WarpRectilinearSource(x, y, 0, push);
  const float2 green  = WarpRectilinearSource(x, y, 1, push);
  const float2 blue   = WarpRectilinearSource(x, y, 2, push);
  const int    width  = int(push.width);
  const int    height = int(push.height);
  const float4 g =
      BilinearSample(source, width, height, green.x + 0.5f, green.y + 0.5f, border);
  float4       out_value;
  out_value.x = BilinearSample(source, width, height, red.x + 0.5f, red.y + 0.5f, border).x;
  out_value.y = g.y;
  out_value.z = BilinearSample(source, width, height, blue.x + 0.5f, blue.y + 0.5f, border).z;
  out_value.w = g.w;
  destination[id.xy] = out_value;
}
