//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <metal_stdlib>

using namespace metal;

struct ExtractParams {
  int input_width;
  int input_height;
  int output_width;
  int output_height;
};

struct ExtractReferenceParams {
  int   input_width;
  int   input_height;
  int   output_width;
  int   output_height;
  float full_ref_w;
  float full_ref_h;
  float pad0;
  float pad1;
  float reference_to_render[12];
};

struct RemapParams {
  int   width;
  int   height;
  float gamma;
  float target;
  float beta;
  float alpha;
  float sigma;
  int   pad;
};

struct PyrDownParams {
  int src_width;
  int src_height;
  int dst_width;
  int dst_height;
};

struct SelectParams {
  int   width;
  int   height;
  int   coarse_width;
  int   coarse_height;
  float gamma_lo;
  float gamma_hi;
  int   first;
  int   last;
  int   top;
  int   pad0;
  int   pad1;
  int   pad2;
};

struct CollapseParams {
  int width;
  int height;
  int coarse_width;
  int coarse_height;
};

struct ApplyParams {
  int   width;
  int   height;
  int   adjusted_width;
  int   adjusted_height;
  float render_to_uv[12];
};

static inline float AcesccEncode(float value) {
  constexpr float kA          = 9.72f;
  constexpr float kB          = 17.52f;
  constexpr float kOffset     = 0.0000152587890625f;
  constexpr float kTransition = 0.000030517578125f;
  constexpr float kFloor      = (-16.0f + kA) / kB;
  if (value < 0.0f) {
    return kFloor + value;
  }
  if (value < kTransition) {
    return (log2(kOffset + value * 0.5f) + kA) / kB;
  }
  return (log2(value) + kA) / kB;
}

static inline float AcesccDecode(float value) {
  constexpr float kA         = 9.72f;
  constexpr float kB         = 17.52f;
  constexpr float kOffset    = 0.0000152587890625f;
  constexpr float kFloor     = (-16.0f + kA) / kB;
  constexpr float kThreshold = (-15.0f + kA) / kB;
  if (value < kFloor) {
    return value - kFloor;
  }
  if (value <= kThreshold) {
    return (exp2(value * kB - kA) - kOffset) * 2.0f;
  }
  return exp2(value * kB - kA);
}

static inline float Ap1Intensity(float4 pixel) {
  return 0.272229f * pixel.x + 0.674082f * pixel.y + 0.053689f * pixel.z;
}

static inline float LogIntensity(float4 acescc) {
  const float4 linear =
      float4(AcesccDecode(acescc.x), AcesccDecode(acescc.y), AcesccDecode(acescc.z), acescc.w);
  return AcesccEncode(max(Ap1Intensity(linear), 1.0e-6f));
}

static inline float4 ReadRgbaBilinear(texture2d<float, access::read> input, int width, int height,
                                      float x, float y) {
  x               = min(max(x, 0.0f), float(width - 1));
  y               = min(max(y, 0.0f), float(height - 1));
  const int    x0 = int(floor(x));
  const int    y0 = int(floor(y));
  const int    x1 = min(x0 + 1, width - 1);
  const int    y1 = min(y0 + 1, height - 1);
  const float  tx = x - float(x0);
  const float  ty = y - float(y0);
  const float4 a  = input.read(uint2(uint(x0), uint(y0)));
  const float4 b  = input.read(uint2(uint(x1), uint(y0)));
  const float4 c  = input.read(uint2(uint(x0), uint(y1)));
  const float4 d  = input.read(uint2(uint(x1), uint(y1)));
  return mix(mix(a, b, tx), mix(c, d, tx), ty);
}

static inline float PlaneRead(device const float* src, int x, int y, int width, int height) {
  x = min(max(x, 0), width - 1);
  y = min(max(y, 0), height - 1);
  return src[y * width + x];
}

static inline float Weight(int tap) {
  return (tap == -2 || tap == 2) ? 1.0f / 16.0f : (tap == -1 || tap == 1) ? 4.0f / 16.0f : 6.0f / 16.0f;
}

static inline float Expand(device const float* coarse, int coarse_width, int coarse_height, int x,
                           int y) {
  float sum = 0.0f;
  for (int ky = -2; ky <= 2; ++ky) {
    const int sample_y = y - ky;
    if ((sample_y & 1) != 0) {
      continue;
    }
    const int cy = min(max(sample_y / 2, 0), coarse_height - 1);
    for (int kx = -2; kx <= 2; ++kx) {
      const int sample_x = x - kx;
      if ((sample_x & 1) != 0) {
        continue;
      }
      const int cx = min(max(sample_x / 2, 0), coarse_width - 1);
      sum += 4.0f * Weight(kx) * Weight(ky) * coarse[cy * coarse_width + cx];
    }
  }
  return sum;
}

static inline float RemapDelta(float delta, float sigma, float alpha, float beta) {
  const float magnitude = fabs(delta);
  if (magnitude <= 1.0e-6f) {
    return 0.0f;
  }
  const float sign = copysign(1.0f, delta);
  if (magnitude <= sigma) {
    return sign * sigma * pow(min(magnitude / max(sigma, 1.0e-6f), 1.0f), alpha);
  }
  return sign * (sigma + beta * (magnitude - sigma));
}

static inline float2 Transform(constant float* matrix, float x, float y) {
  return float2(matrix[0] * x + matrix[1] * y + matrix[2],
                matrix[3] * x + matrix[4] * y + matrix[5]);
}

static inline float Bilinear(device const float* plane, int width, int height, float x, float y) {
  x             = min(max(x, 0.0f), float(width - 1));
  y             = min(max(y, 0.0f), float(height - 1));
  const int   x0 = int(floor(x));
  const int   y0 = int(floor(y));
  const int   x1 = min(x0 + 1, width - 1);
  const int   y1 = min(y0 + 1, height - 1);
  const float tx = x - float(x0);
  const float ty = y - float(y0);
  const float a  = plane[y0 * width + x0] + (plane[y0 * width + x1] - plane[y0 * width + x0]) * tx;
  const float b  = plane[y1 * width + x0] + (plane[y1 * width + x1] - plane[y1 * width + x0]) * tx;
  return a + (b - a) * ty;
}

kernel void local_tone_extract(texture2d<float, access::read> src [[texture(0)]],
                               device float* dst [[buffer(0)]],
                               constant ExtractParams& params [[buffer(1)]],
                               uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= uint(params.output_width) || gid.y >= uint(params.output_height)) {
    return;
  }
  const float sx = (float(gid.x) + 0.5f) * float(params.input_width) /
                       float(params.output_width) -
                   0.5f;
  const float sy = (float(gid.y) + 0.5f) * float(params.input_height) /
                       float(params.output_height) -
                   0.5f;
  dst[gid.y * uint(params.output_width) + gid.x] =
      LogIntensity(ReadRgbaBilinear(src, params.input_width, params.input_height, sx, sy));
}

kernel void local_tone_extract_reference(texture2d<float, access::read> src [[texture(0)]],
                                         device float* dst [[buffer(0)]],
                                         constant ExtractReferenceParams& params [[buffer(1)]],
                                         uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= uint(params.output_width) || gid.y >= uint(params.output_height)) {
    return;
  }
  const float  u   = (float(gid.x) + 0.5f) / float(params.output_width);
  const float  v   = (float(gid.y) + 0.5f) / float(params.output_height);
  const float2 src_xy =
      Transform(params.reference_to_render, u * params.full_ref_w, v * params.full_ref_h);
  dst[gid.y * uint(params.output_width) + gid.x] = LogIntensity(
      ReadRgbaBilinear(src, params.input_width, params.input_height, src_xy.x - 0.5f, src_xy.y - 0.5f));
}

kernel void local_tone_pyr_down(device const float* src [[buffer(0)]], device float* dst [[buffer(1)]],
                               constant PyrDownParams& params [[buffer(2)]],
                               uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= uint(params.dst_width) || gid.y >= uint(params.dst_height)) {
    return;
  }
  float sum = 0.0f;
  for (int ky = -2; ky <= 2; ++ky) {
    for (int kx = -2; kx <= 2; ++kx) {
      sum += Weight(kx) * Weight(ky) *
             PlaneRead(src, int(gid.x) * 2 + kx, int(gid.y) * 2 + ky, params.src_width,
                       params.src_height);
    }
  }
  dst[gid.y * uint(params.dst_width) + gid.x] = sum;
}

kernel void local_tone_remap(device const float* src [[buffer(0)]], device float* dst [[buffer(1)]],
                             constant RemapParams& params [[buffer(2)]],
                             uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= uint(params.width) || gid.y >= uint(params.height)) {
    return;
  }
  const uint index = gid.y * uint(params.width) + gid.x;
  dst[index] =
      params.target + RemapDelta(src[index] - params.gamma, params.sigma, params.alpha, params.beta);
}

kernel void local_tone_select(device const float* source [[buffer(0)]],
                              device const float* lo [[buffer(1)]],
                              device const float* lo_coarse [[buffer(2)]],
                              device const float* hi [[buffer(3)]],
                              device const float* hi_coarse [[buffer(4)]],
                              device float* output [[buffer(5)]],
                              constant SelectParams& params [[buffer(6)]],
                              uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= uint(params.width) || gid.y >= uint(params.height)) {
    return;
  }
  const uint  index = gid.y * uint(params.width) + gid.x;
  const float value = source[index];
  if (!((params.first != 0 && value <= params.gamma_hi) ||
        (params.last != 0 && value >= params.gamma_lo) ||
        (value >= params.gamma_lo && value < params.gamma_hi))) {
    return;
  }
  const float t =
      min(max((value - params.gamma_lo) / max(params.gamma_hi - params.gamma_lo, 1.0e-6f), 0.0f),
          1.0f);
  if (params.top != 0) {
    output[index] = lo[index] + (hi[index] - lo[index]) * t;
    return;
  }
  const float lap_lo =
      lo[index] - Expand(lo_coarse, params.coarse_width, params.coarse_height, int(gid.x), int(gid.y));
  const float lap_hi =
      hi[index] - Expand(hi_coarse, params.coarse_width, params.coarse_height, int(gid.x), int(gid.y));
  output[index] = lap_lo + (lap_hi - lap_lo) * t;
}

kernel void local_tone_collapse(device const float* lap [[buffer(0)]],
                                device const float* coarse [[buffer(1)]],
                                device float* output [[buffer(2)]],
                                constant CollapseParams& params [[buffer(3)]],
                                uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= uint(params.width) || gid.y >= uint(params.height)) {
    return;
  }
  const uint index = gid.y * uint(params.width) + gid.x;
  output[index] =
      lap[index] + Expand(coarse, params.coarse_width, params.coarse_height, int(gid.x), int(gid.y));
}

kernel void local_tone_apply(texture2d<float, access::read> src [[texture(0)]],
                             texture2d<float, access::write> dst [[texture(1)]],
                             device const float* reference [[buffer(0)]],
                             device const float* adjusted [[buffer(1)]],
                             constant ApplyParams& params [[buffer(2)]],
                             uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= uint(params.width) || gid.y >= uint(params.height)) {
    return;
  }
  const float2 uv = Transform(params.render_to_uv, float(gid.x) + 0.5f, float(gid.y) + 0.5f);
  const float  ax = uv.x * float(params.adjusted_width) - 0.5f;
  const float  ay = uv.y * float(params.adjusted_height) - 0.5f;
  const float  reference_l =
      Bilinear(reference, params.adjusted_width, params.adjusted_height, ax, ay);
  const float adjusted_l = Bilinear(adjusted, params.adjusted_width, params.adjusted_height, ax, ay);
  const float4 pixel     = src.read(gid);
  const float  source_l  = LogIntensity(pixel);
  const float  source_intensity = max(AcesccDecode(source_l), 1.0e-5f);
  const float  target_intensity = AcesccDecode(source_l + adjusted_l - reference_l);
  const float  ratio            = min(max(target_intensity / source_intensity, 0.0f), 32.0f);
  float        r                = AcesccDecode(pixel.x) * ratio;
  float        g                = AcesccDecode(pixel.y) * ratio;
  float        b                = AcesccDecode(pixel.z) * ratio;
  constexpr float kLower        = -1.0e-5f;
  float           gamut_scale   = 1.0f;
  if (r < kLower && target_intensity > r) {
    gamut_scale = min(gamut_scale, (target_intensity - kLower) / (target_intensity - r));
  }
  if (g < kLower && target_intensity > g) {
    gamut_scale = min(gamut_scale, (target_intensity - kLower) / (target_intensity - g));
  }
  if (b < kLower && target_intensity > b) {
    gamut_scale = min(gamut_scale, (target_intensity - kLower) / (target_intensity - b));
  }
  gamut_scale = min(max(gamut_scale, 0.0f), 1.0f);
  r           = target_intensity + (r - target_intensity) * gamut_scale;
  g           = target_intensity + (g - target_intensity) * gamut_scale;
  b           = target_intensity + (b - target_intensity) * gamut_scale;
  dst.write(float4(AcesccEncode(r), AcesccEncode(g), AcesccEncode(b), pixel.w), gid);
}
