//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only

__constant sampler_t kNearestClamp =
    CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST;

typedef struct {
  int input_width;
  int input_height;
  int output_width;
  int output_height;
} ExtractParams;

typedef struct {
  int   input_width;
  int   input_height;
  int   output_width;
  int   output_height;
  float full_ref_w;
  float full_ref_h;
  float pad0;
  float pad1;
  float reference_to_render[12];
} ExtractReferenceParams;

typedef struct {
  int   width;
  int   height;
  float gamma;
  float target;
  float beta;
  float alpha;
  float sigma;
  int   pad;
} RemapParams;

typedef struct {
  int src_width;
  int src_height;
  int dst_width;
  int dst_height;
} PyrDownParams;

typedef struct {
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
} SelectParams;

typedef struct {
  int width;
  int height;
  int coarse_width;
  int coarse_height;
} CollapseParams;

typedef struct {
  int   width;
  int   height;
  int   adjusted_width;
  int   adjusted_height;
  float render_to_uv[12];
} ApplyParams;

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

static inline float AcesccDecode(float value) {
  const float kA         = 9.72f;
  const float kB         = 17.52f;
  const float kOffset    = 0.0000152587890625f;
  const float kFloor     = (-16.0f + kA) / kB;
  const float kThreshold = (-15.0f + kA) / kB;
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
  const float4 linear = (float4)(AcesccDecode(acescc.x), AcesccDecode(acescc.y),
                                 AcesccDecode(acescc.z), acescc.w);
  return AcesccEncode(fmax(Ap1Intensity(linear), 1.0e-6f));
}

static inline float4 ReadRgbaBilinear(__read_only image2d_t input, int width, int height, float x,
                                      float y) {
  x = fmin(fmax(x, 0.0f), (float)(width - 1));
  y = fmin(fmax(y, 0.0f), (float)(height - 1));
  const int   x0 = (int)floor(x);
  const int   y0 = (int)floor(y);
  const int   x1 = min(x0 + 1, width - 1);
  const int   y1 = min(y0 + 1, height - 1);
  const float tx = x - (float)x0;
  const float ty = y - (float)y0;
  const float4 a  = read_imagef(input, kNearestClamp, (int2)(x0, y0));
  const float4 b  = read_imagef(input, kNearestClamp, (int2)(x1, y0));
  const float4 c  = read_imagef(input, kNearestClamp, (int2)(x0, y1));
  const float4 d  = read_imagef(input, kNearestClamp, (int2)(x1, y1));
  return mix(mix(a, b, tx), mix(c, d, tx), ty);
}

static inline float PlaneRead(__global const float* src, uint offset, int x, int y, int width,
                              int height) {
  x = min(max(x, 0), width - 1);
  y = min(max(y, 0), height - 1);
  return src[offset + (uint)y * (uint)width + (uint)x];
}

static inline float Weight(int tap) {
  return (tap == -2 || tap == 2) ? 1.0f / 16.0f
       : (tap == -1 || tap == 1) ? 4.0f / 16.0f
                                 : 6.0f / 16.0f;
}

static inline float Expand(__global const float* coarse, uint offset, int coarse_width,
                           int coarse_height, int x, int y) {
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
      sum += 4.0f * Weight(kx) * Weight(ky) *
             coarse[offset + (uint)cy * (uint)coarse_width + (uint)cx];
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
    return sign * sigma * pow(fmin(magnitude / fmax(sigma, 1.0e-6f), 1.0f), alpha);
  }
  return sign * (sigma + beta * (magnitude - sigma));
}

static inline float2 Transform(const float* matrix, float x, float y) {
  return (float2)(matrix[0] * x + matrix[1] * y + matrix[2],
                  matrix[3] * x + matrix[4] * y + matrix[5]);
}

static inline float Bilinear(__global const float* plane, uint offset, int width, int height,
                             float x, float y) {
  x = fmin(fmax(x, 0.0f), (float)(width - 1));
  y = fmin(fmax(y, 0.0f), (float)(height - 1));
  const int   x0 = (int)floor(x);
  const int   y0 = (int)floor(y);
  const int   x1 = min(x0 + 1, width - 1);
  const int   y1 = min(y0 + 1, height - 1);
  const float tx = x - (float)x0;
  const float ty = y - (float)y0;
  const float a  = plane[offset + (uint)y0 * (uint)width + (uint)x0] +
                  (plane[offset + (uint)y0 * (uint)width + (uint)x1] -
                   plane[offset + (uint)y0 * (uint)width + (uint)x0]) *
                      tx;
  const float b  = plane[offset + (uint)y1 * (uint)width + (uint)x0] +
                  (plane[offset + (uint)y1 * (uint)width + (uint)x1] -
                   plane[offset + (uint)y1 * (uint)width + (uint)x0]) *
                      tx;
  return a + (b - a) * ty;
}

__kernel void local_tone_extract(__read_only image2d_t src, __global float* dst, ExtractParams params,
                                 uint dst_offset) {
  const int x = (int)get_global_id(0);
  const int y = (int)get_global_id(1);
  if (x >= params.output_width || y >= params.output_height) {
    return;
  }
  const float sx = ((float)x + 0.5f) * (float)params.input_width /
                      (float)params.output_width -
                  0.5f;
  const float sy = ((float)y + 0.5f) * (float)params.input_height /
                      (float)params.output_height -
                  0.5f;
  dst[dst_offset + (uint)y * (uint)params.output_width + (uint)x] =
      LogIntensity(ReadRgbaBilinear(src, params.input_width, params.input_height, sx, sy));
}

__kernel void local_tone_extract_reference(__read_only image2d_t src, __global float* dst,
                                            ExtractReferenceParams params, uint dst_offset) {
  const int x = (int)get_global_id(0);
  const int y = (int)get_global_id(1);
  if (x >= params.output_width || y >= params.output_height) {
    return;
  }
  const float  u = ((float)x + 0.5f) / (float)params.output_width;
  const float  v = ((float)y + 0.5f) / (float)params.output_height;
  const float2 source = Transform(params.reference_to_render, u * params.full_ref_w,
                                   v * params.full_ref_h);
  dst[dst_offset + (uint)y * (uint)params.output_width + (uint)x] =
      LogIntensity(ReadRgbaBilinear(src, params.input_width, params.input_height,
                                    source.x - 0.5f, source.y - 0.5f));
}

__kernel void local_tone_pyr_down(__global const float* src, __global float* dst,
                                  PyrDownParams params, uint src_offset, uint dst_offset) {
  const int x = (int)get_global_id(0);
  const int y = (int)get_global_id(1);
  if (x >= params.dst_width || y >= params.dst_height) {
    return;
  }
  float sum = 0.0f;
  for (int ky = -2; ky <= 2; ++ky) {
    for (int kx = -2; kx <= 2; ++kx) {
      sum += Weight(kx) * Weight(ky) *
             PlaneRead(src, src_offset, x * 2 + kx, y * 2 + ky, params.src_width,
                       params.src_height);
    }
  }
  dst[dst_offset + (uint)y * (uint)params.dst_width + (uint)x] = sum;
}

__kernel void local_tone_remap(__global const float* src, __global float* dst, RemapParams params,
                               uint src_offset, uint dst_offset) {
  const int x = (int)get_global_id(0);
  const int y = (int)get_global_id(1);
  if (x >= params.width || y >= params.height) {
    return;
  }
  const uint index = (uint)y * (uint)params.width + (uint)x;
  dst[dst_offset + index] =
      params.target + RemapDelta(src[src_offset + index] - params.gamma, params.sigma,
                                 params.alpha, params.beta);
}

__kernel void local_tone_select(__global const float* source, __global const float* lo,
                                __global const float* lo_coarse, __global const float* hi,
                                __global const float* hi_coarse, __global float* output,
                                SelectParams params, uint source_offset, uint lo_offset,
                                uint lo_coarse_offset, uint hi_offset, uint hi_coarse_offset,
                                uint output_offset) {
  const int x = (int)get_global_id(0);
  const int y = (int)get_global_id(1);
  if (x >= params.width || y >= params.height) {
    return;
  }
  const uint  index = (uint)y * (uint)params.width + (uint)x;
  const float value = source[source_offset + index];
  if (!((params.first != 0 && value <= params.gamma_hi) ||
        (params.last != 0 && value >= params.gamma_lo) ||
        (value >= params.gamma_lo && value < params.gamma_hi))) {
    return;
  }
  const float t = fmin(fmax((value - params.gamma_lo) /
                                fmax(params.gamma_hi - params.gamma_lo, 1.0e-6f),
                            0.0f),
                       1.0f);
  if (params.top != 0) {
    output[output_offset + index] = lo[lo_offset + index] +
                                    (hi[hi_offset + index] - lo[lo_offset + index]) * t;
    return;
  }
  const float lap_lo = lo[lo_offset + index] -
                       Expand(lo_coarse, lo_coarse_offset, params.coarse_width,
                              params.coarse_height, x, y);
  const float lap_hi = hi[hi_offset + index] -
                       Expand(hi_coarse, hi_coarse_offset, params.coarse_width,
                              params.coarse_height, x, y);
  output[output_offset + index] = lap_lo + (lap_hi - lap_lo) * t;
}

__kernel void local_tone_collapse(__global const float* lap, __global const float* coarse,
                                  __global float* output, CollapseParams params, uint lap_offset,
                                  uint coarse_offset, uint output_offset) {
  const int x = (int)get_global_id(0);
  const int y = (int)get_global_id(1);
  if (x >= params.width || y >= params.height) {
    return;
  }
  const uint index = (uint)y * (uint)params.width + (uint)x;
  output[output_offset + index] =
      lap[lap_offset + index] +
      Expand(coarse, coarse_offset, params.coarse_width, params.coarse_height, x, y);
}

__kernel void local_tone_apply(__read_only image2d_t src, __write_only image2d_t dst,
                               __global const float* reference, __global const float* adjusted,
                               ApplyParams params, uint reference_offset, uint adjusted_offset) {
  const int x = (int)get_global_id(0);
  const int y = (int)get_global_id(1);
  if (x >= params.width || y >= params.height) {
    return;
  }
  const float2 uv = Transform(params.render_to_uv, (float)x + 0.5f, (float)y + 0.5f);
  const float  ax = uv.x * (float)params.adjusted_width - 0.5f;
  const float  ay = uv.y * (float)params.adjusted_height - 0.5f;
  const float  reference_l =
      Bilinear(reference, reference_offset, params.adjusted_width, params.adjusted_height, ax, ay);
  const float adjusted_l =
      Bilinear(adjusted, adjusted_offset, params.adjusted_width, params.adjusted_height, ax, ay);
  const float4 pixel = read_imagef(src, kNearestClamp, (int2)(x, y));
  const float  source_l = LogIntensity(pixel);
  const float  source_intensity = fmax(AcesccDecode(source_l), 1.0e-5f);
  const float  target_intensity = AcesccDecode(source_l + adjusted_l - reference_l);
  const float  ratio = fmin(fmax(target_intensity / source_intensity, 0.0f), 32.0f);
  float        r = AcesccDecode(pixel.x) * ratio;
  float        g = AcesccDecode(pixel.y) * ratio;
  float        b = AcesccDecode(pixel.z) * ratio;
  const float  kLower = -1.0e-5f;
  float        gamut_scale = 1.0f;
  if (r < kLower && target_intensity > r) {
    gamut_scale = fmin(gamut_scale, (target_intensity - kLower) / (target_intensity - r));
  }
  if (g < kLower && target_intensity > g) {
    gamut_scale = fmin(gamut_scale, (target_intensity - kLower) / (target_intensity - g));
  }
  if (b < kLower && target_intensity > b) {
    gamut_scale = fmin(gamut_scale, (target_intensity - kLower) / (target_intensity - b));
  }
  gamut_scale = fmin(fmax(gamut_scale, 0.0f), 1.0f);
  r = target_intensity + (r - target_intensity) * gamut_scale;
  g = target_intensity + (g - target_intensity) * gamut_scale;
  b = target_intensity + (b - target_intensity) * gamut_scale;
  write_imagef(dst, (int2)(x, y), (float4)(AcesccEncode(r), AcesccEncode(g), AcesccEncode(b),
                                             pixel.w));
}
