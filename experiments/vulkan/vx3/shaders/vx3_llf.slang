//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//
// VX3 experiment (vulkan_compute_backend_plan.md, section 6): the Local Tone (LLF) kernels of
// edit/runtime/opencl/shader/local_tone.cl, on planes that are device addresses, and a kernel that
// writes the synthetic 45 megapixel scene. VX3 measures time, so the identity render_to_uv of a
// full-image render replaces the matrices. Experiment code: it does not merge.
//
// - set 0, binding 0: scene RGBA32F storage image (read)
// - set 0, binding 1: destination RGBA32F storage image (write)

#include "color/color_encoding_math.h"

struct LlfPush {
  float* src;
  float* dst;
  float* lo;
  float* lo_coarse;
  float* hi;
  float* hi_coarse;
  int    width;
  int    height;
  int    coarse_width;
  int    coarse_height;
  int    input_width;
  int    input_height;
  float  gamma;
  float  target;
  float  beta;
  float  alpha;
  float  sigma;
  float  gamma_lo;
  float  gamma_hi;
  int    first;
  int    last;
  int    top;
};

[[vk::push_constant]] LlfPush push;

[[vk::binding(0, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> scene_image;
[[vk::binding(1, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> dst_image;

static float Ap1Intensity(float4 pixel) {
  return 0.272229f * pixel.x + 0.674082f * pixel.y + 0.053689f * pixel.z;
}

static float LogIntensity(float4 acescc) {
  const float4 linear = float4(CeAcesccDecode(acescc.x), CeAcesccDecode(acescc.y),
                               CeAcesccDecode(acescc.z), acescc.w);
  return CeAcesccEncode(max(Ap1Intensity(linear), 1.0e-6f));
}

static float4 SceneAt(int x, int y, int width, int height) {
  return scene_image[uint2(uint(clamp(x, 0, width - 1)), uint(clamp(y, 0, height - 1)))];
}

static float4 ReadRgbaBilinear(int width, int height, float x, float y) {
  x = min(max(x, 0.0f), float(width - 1));
  y = min(max(y, 0.0f), float(height - 1));
  const int   x0 = int(floor(x));
  const int   y0 = int(floor(y));
  const int   x1 = min(x0 + 1, width - 1);
  const int   y1 = min(y0 + 1, height - 1);
  const float tx = x - float(x0);
  const float ty = y - float(y0);
  const float4 a = SceneAt(x0, y0, width, height);
  const float4 b = SceneAt(x1, y0, width, height);
  const float4 c = SceneAt(x0, y1, width, height);
  const float4 d = SceneAt(x1, y1, width, height);
  return lerp(lerp(a, b, tx), lerp(c, d, tx), ty);
}

static float PlaneRead(float* src, int x, int y, int width, int height) {
  x = min(max(x, 0), width - 1);
  y = min(max(y, 0), height - 1);
  return src[uint(y) * uint(width) + uint(x)];
}

static float Weight(int tap) {
  return (tap == -2 || tap == 2) ? 1.0f / 16.0f : (tap == -1 || tap == 1) ? 4.0f / 16.0f : 6.0f / 16.0f;
}

static float Expand(float* coarse, int coarse_width, int coarse_height, int x, int y) {
  float sum = 0.0f;
  for (int ky = -2; ky <= 2; ++ky) {
    const int sample_y = y - ky;
    if ((sample_y & 1) != 0) continue;
    const int cy = min(max(sample_y / 2, 0), coarse_height - 1);
    for (int kx = -2; kx <= 2; ++kx) {
      const int sample_x = x - kx;
      if ((sample_x & 1) != 0) continue;
      const int cx = min(max(sample_x / 2, 0), coarse_width - 1);
      sum += 4.0f * Weight(kx) * Weight(ky) * coarse[uint(cy) * uint(coarse_width) + uint(cx)];
    }
  }
  return sum;
}

static float RemapDelta(float delta, float sigma, float alpha, float beta) {
  const float magnitude = abs(delta);
  if (magnitude <= 1.0e-6f) return 0.0f;
  const float s = delta < 0.0f ? -1.0f : 1.0f;
  if (magnitude <= sigma) return s * sigma * pow(min(magnitude / max(sigma, 1.0e-6f), 1.0f), alpha);
  return s * (sigma + beta * (magnitude - sigma));
}

static float Bilinear(float* plane, int width, int height, float x, float y) {
  x = min(max(x, 0.0f), float(width - 1));
  y = min(max(y, 0.0f), float(height - 1));
  const int   x0 = int(floor(x));
  const int   y0 = int(floor(y));
  const int   x1 = min(x0 + 1, width - 1);
  const int   y1 = min(y0 + 1, height - 1);
  const float tx = x - float(x0);
  const float ty = y - float(y0);
  const float p00 = plane[uint(y0) * uint(width) + uint(x0)];
  const float p01 = plane[uint(y0) * uint(width) + uint(x1)];
  const float p10 = plane[uint(y1) * uint(width) + uint(x0)];
  const float p11 = plane[uint(y1) * uint(width) + uint(x1)];
  const float a   = p00 + (p01 - p00) * tx;
  const float b   = p10 + (p11 - p10) * tx;
  return a + (b - a) * ty;
}

static uint Hash(uint v) {
  v ^= v >> 16;
  v *= 0x7feb352dU;
  v ^= v >> 15;
  v *= 0x846ca68bU;
  v ^= v >> 16;
  return v;
}

/// The synthetic scene: ACEScc from 0.1 to 0.75 in smooth fields, with fine noise.
[shader("compute")]
[numthreads(16, 16, 1)]
void vx3_init_scene(uint3 id: SV_DispatchThreadID) {
  const int x = int(id.x);
  const int y = int(id.y);
  if (x >= push.width || y >= push.height) return;
  const float u     = float(x) / float(push.width);
  const float v     = float(y) / float(push.height);
  const float noise = float(Hash(uint(y) * 16384U + uint(x)) & 1023U) / 1023.0f - 0.5f;
  const float base  = 0.42f + 0.25f * sin(6.0f * u + 2.0f * v) * cos(3.0f * v);
  dst_image[id.xy]  = float4(base + 0.04f * sin(11.0f * u) + 0.01f * noise,
                             base + 0.01f * noise, base - 0.03f * cos(7.0f * v) + 0.01f * noise, 1.0f);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void llf_extract(uint3 id: SV_DispatchThreadID) {
  const int x = int(id.x);
  const int y = int(id.y);
  if (x >= push.width || y >= push.height) return;
  const float sx = (float(x) + 0.5f) * float(push.input_width) / float(push.width) - 0.5f;
  const float sy = (float(y) + 0.5f) * float(push.input_height) / float(push.height) - 0.5f;
  push.dst[uint(y) * uint(push.width) + uint(x)] =
      LogIntensity(ReadRgbaBilinear(push.input_width, push.input_height, sx, sy));
}

[shader("compute")]
[numthreads(16, 16, 1)]
void llf_pyr_down(uint3 id: SV_DispatchThreadID) {
  const int x = int(id.x);
  const int y = int(id.y);
  if (x >= push.width || y >= push.height) return;
  float sum = 0.0f;
  for (int ky = -2; ky <= 2; ++ky) {
    for (int kx = -2; kx <= 2; ++kx) {
      sum += Weight(kx) * Weight(ky) *
             PlaneRead(push.src, x * 2 + kx, y * 2 + ky, push.input_width, push.input_height);
    }
  }
  push.dst[uint(y) * uint(push.width) + uint(x)] = sum;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void llf_remap(uint3 id: SV_DispatchThreadID) {
  const int x = int(id.x);
  const int y = int(id.y);
  if (x >= push.width || y >= push.height) return;
  const uint index = uint(y) * uint(push.width) + uint(x);
  push.dst[index] =
      push.target + RemapDelta(push.src[index] - push.gamma, push.sigma, push.alpha, push.beta);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void llf_select(uint3 id: SV_DispatchThreadID) {
  const int x = int(id.x);
  const int y = int(id.y);
  if (x >= push.width || y >= push.height) return;
  const uint  index = uint(y) * uint(push.width) + uint(x);
  const float value = push.src[index];
  if (!((push.first != 0 && value <= push.gamma_hi) || (push.last != 0 && value >= push.gamma_lo) ||
        (value >= push.gamma_lo && value < push.gamma_hi))) {
    return;
  }
  const float t =
      min(max((value - push.gamma_lo) / max(push.gamma_hi - push.gamma_lo, 1.0e-6f), 0.0f), 1.0f);
  if (push.top != 0) {
    push.dst[index] = push.lo[index] + (push.hi[index] - push.lo[index]) * t;
    return;
  }
  const float lap_lo =
      push.lo[index] - Expand(push.lo_coarse, push.coarse_width, push.coarse_height, x, y);
  const float lap_hi =
      push.hi[index] - Expand(push.hi_coarse, push.coarse_width, push.coarse_height, x, y);
  push.dst[index] = lap_lo + (lap_hi - lap_lo) * t;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void llf_collapse(uint3 id: SV_DispatchThreadID) {
  const int x = int(id.x);
  const int y = int(id.y);
  if (x >= push.width || y >= push.height) return;
  const uint index = uint(y) * uint(push.width) + uint(x);
  push.dst[index] =
      push.src[index] + Expand(push.lo, push.coarse_width, push.coarse_height, x, y);
}

/// local_tone_apply with the identity render_to_uv. src = reference plane, lo = adjusted plane.
[shader("compute")]
[numthreads(16, 16, 1)]
void llf_apply(uint3 id: SV_DispatchThreadID) {
  const int x = int(id.x);
  const int y = int(id.y);
  if (x >= push.width || y >= push.height) return;
  const float ax          = (float(x) + 0.5f) / float(push.width) * float(push.coarse_width) - 0.5f;
  const float ay          = (float(y) + 0.5f) / float(push.height) * float(push.coarse_height) - 0.5f;
  const float reference_l = Bilinear(push.src, push.coarse_width, push.coarse_height, ax, ay);
  const float adjusted_l  = Bilinear(push.lo, push.coarse_width, push.coarse_height, ax, ay);
  const float4 pixel      = scene_image[id.xy];
  const float source_l    = LogIntensity(pixel);
  const float source_i    = max(CeAcesccDecode(source_l), 1.0e-5f);
  const float target_i    = CeAcesccDecode(source_l + adjusted_l - reference_l);
  const float ratio       = min(max(target_i / source_i, 0.0f), 32.0f);
  float       r           = CeAcesccDecode(pixel.x) * ratio;
  float       g           = CeAcesccDecode(pixel.y) * ratio;
  float       b           = CeAcesccDecode(pixel.z) * ratio;
  const float kLower      = -1.0e-5f;
  float       gamut_scale = 1.0f;
  if (r < kLower && target_i > r) gamut_scale = min(gamut_scale, (target_i - kLower) / (target_i - r));
  if (g < kLower && target_i > g) gamut_scale = min(gamut_scale, (target_i - kLower) / (target_i - g));
  if (b < kLower && target_i > b) gamut_scale = min(gamut_scale, (target_i - kLower) / (target_i - b));
  gamut_scale = min(max(gamut_scale, 0.0f), 1.0f);
  r = target_i + (r - target_i) * gamut_scale;
  g = target_i + (g - target_i) * gamut_scale;
  b = target_i + (b - target_i) * gamut_scale;
  dst_image[id.xy] = float4(CeAcesccEncode(r), CeAcesccEncode(g), CeAcesccEncode(b), pixel.w);
}
