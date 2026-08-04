//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <metal_stdlib>

using namespace metal;

struct HighlightParams {
  float clips[4];
  float clipdark[4];
  float chrominance[4];
  uint  width;
  uint  height;
  uint  stride;
};

constant uint kDilateRadius  = 3u;
constant uint kPlaneBaseR    = 0u;
constant uint kPlaneBaseG    = 1u;
constant uint kPlaneBaseB    = 2u;
constant uint kPlaneDilatedR = 3u;
constant uint kPlaneDilatedG = 4u;
constant uint kPlaneDilatedB = 5u;
// Reconstruction ramps in over the top (1 - kSoftClipLo) fraction below the clip level instead
// of switching on exactly at the clip point. Shot/read noise makes pixels straddle a hard
// threshold, which decorrelates neighbours and shows up as speckle at highlight edges.
constant float kSoftClipLo = 0.95f;

static inline float Cube(float value) { return value * value * value; }

// A CMOS readout is a finite, non-negative electron count. Anything else (negative black-level
// residue, NaN/Inf from an upstream artefact) is mapped to 0 so a single corrupt sample can
// neither become a dark outlier nor poison the neighbourhood statistics.
static inline float SanitizeChannel(float value) {
  return isfinite(value) ? max(value, 0.0f) : 0.0f;
}

static inline float3 MaxRgb(float4 value) {
  return float3(SanitizeChannel(value.x), SanitizeChannel(value.y), SanitizeChannel(value.z));
}

static inline float SoftClipWeight(float value, float clip) {
  const float lo = kSoftClipLo * clip;
  const float t  = clamp((value - lo) / (clip - lo), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

static inline float ReconstructChannel(float pixel, float ref, float chrominance, float weight) {
  if (weight <= 0.0f) {
    return pixel;
  }
  return pixel + weight * (max(pixel, ref + chrominance) - pixel);
}

// Accumulate one neighbourhood sample. A sample at/above the clip level is censored by the
// sensor (the photosite has reached full well, so its readout only means ">= clip"), or is a
// hot-pixel/demosaic outlier; either way it carries no usable level information and must not
// move the mean at face value. Censored samples still feed the fallback used when a channel
// has no uncensored sample left (e.g. inside a fully blown region). Non-finite samples are
// dropped entirely.
static inline void RefavgAccumulate(thread float* valid_sum, thread float* valid_cnt,
                                    thread float* all_sum, thread float* all_cnt, float3 raw,
                                    constant HighlightParams& params) {
  if (isfinite(raw.x)) {
    const float v = max(raw.x, 0.0f);
    all_sum[0] += v;
    all_cnt[0] += 1.0f;
    if (v < params.clips[0]) {
      valid_sum[0] += v;
      valid_cnt[0] += 1.0f;
    }
  }
  if (isfinite(raw.y)) {
    const float v = max(raw.y, 0.0f);
    all_sum[1] += v;
    all_cnt[1] += 1.0f;
    if (v < params.clips[1]) {
      valid_sum[1] += v;
      valid_cnt[1] += 1.0f;
    }
  }
  if (isfinite(raw.z)) {
    const float v = max(raw.z, 0.0f);
    all_sum[2] += v;
    all_cnt[2] += 1.0f;
    if (v < params.clips[2]) {
      valid_sum[2] += v;
      valid_cnt[2] += 1.0f;
    }
  }
}

static inline float3 CalcRefavg(device const float4* input, int row, int col,
                                constant HighlightParams& params) {
  float valid_sum[3] = {0.0f, 0.0f, 0.0f};
  float valid_cnt[3] = {0.0f, 0.0f, 0.0f};
  float all_sum[3]   = {0.0f, 0.0f, 0.0f};
  float all_cnt[3]   = {0.0f, 0.0f, 0.0f};

  const int dymin = max(0, row - 1);
  const int dxmin = max(0, col - 1);
  const int dymax = min(static_cast<int>(params.height) - 1, row + 1);
  const int dxmax = min(static_cast<int>(params.width) - 1, col + 1);

  for (int dy = dymin; dy <= dymax; ++dy) {
    for (int dx = dxmin; dx <= dxmax; ++dx) {
      RefavgAccumulate(valid_sum, valid_cnt, all_sum, all_cnt,
                       input[static_cast<uint>(dy) * params.stride + static_cast<uint>(dx)].rgb,
                       params);
    }
  }

  float mean[3];
  for (uint c = 0; c < 3u; ++c) {
    const float m = (valid_cnt[c] > 0.0f) ? valid_sum[c] / valid_cnt[c]
                    : (all_cnt[c] > 0.0f)   ? all_sum[c] / all_cnt[c]
                                            : 0.0f;
    mean[c] = pow(m, 1.0f / 3.0f);
  }

  return float3(Cube(0.5f * (mean[1] + mean[2])), Cube(0.5f * (mean[0] + mean[2])),
                Cube(0.5f * (mean[0] + mean[1])));
}

static inline uchar DilateMaskAt(device const uchar* plane, uint width, uint height, int row,
                                 int col, int radius) {
  const int y0 = max(0, row - radius);
  const int x0 = max(0, col - radius);
  const int y1 = min(static_cast<int>(height) - 1, row + radius);
  const int x1 = min(static_cast<int>(width) - 1, col + radius);

  for (int y = y0; y <= y1; ++y) {
    const int row_offset = y * static_cast<int>(width);
    for (int x = x0; x <= x1; ++x) {
      if (plane[row_offset + x] != 0) {
        return 1;
      }
    }
  }

  return 0;
}

kernel void hlr_build_mask(device const float4* input [[buffer(0)]],
                           device uchar*        mask_buf [[buffer(1)]],
                           device atomic_uint*  anyclipped [[buffer(2)]],
                           constant HighlightParams& params [[buffer(3)]],
                           uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= params.width || gid.y >= params.height) {
    return;
  }
  const uint   size  = params.width * params.height;
  const uint   index = gid.y * params.stride + gid.x;
  const uint   idx   = gid.y * params.width + gid.x;
  const float3 pixel = MaxRgb(input[index]);

  mask_buf[kPlaneBaseR * size + idx] = pixel.x >= params.clips[0] ? 1 : 0;
  mask_buf[kPlaneBaseG * size + idx] = pixel.y >= params.clips[1] ? 1 : 0;
  mask_buf[kPlaneBaseB * size + idx] = pixel.z >= params.clips[2] ? 1 : 0;

  if (pixel.x >= params.clips[0] || pixel.y >= params.clips[1] || pixel.z >= params.clips[2]) {
    atomic_store_explicit(anyclipped, 1u, memory_order_relaxed);
  }
}

kernel void hlr_dilate_mask(device const uchar* mask_buf [[buffer(0)]],
                            device uchar*       dilated_mask_buf [[buffer(1)]],
                            constant HighlightParams& params [[buffer(2)]],
                            uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= params.width || gid.y >= params.height) {
    return;
  }
  const uint size = params.width * params.height;
  const uint idx  = gid.y * params.width + gid.x;

  dilated_mask_buf[kPlaneDilatedR * size + idx] =
      DilateMaskAt(mask_buf + kPlaneBaseR * size, params.width, params.height,
                   static_cast<int>(gid.y), static_cast<int>(gid.x), static_cast<int>(kDilateRadius));
  dilated_mask_buf[kPlaneDilatedG * size + idx] =
      DilateMaskAt(mask_buf + kPlaneBaseG * size, params.width, params.height,
                   static_cast<int>(gid.y), static_cast<int>(gid.x), static_cast<int>(kDilateRadius));
  dilated_mask_buf[kPlaneDilatedB * size + idx] =
      DilateMaskAt(mask_buf + kPlaneBaseB * size, params.width, params.height,
                   static_cast<int>(gid.y), static_cast<int>(gid.x), static_cast<int>(kDilateRadius));
}

kernel void hlr_chrominance_contrib(device const float4* input [[buffer(0)]],
                                    device const uchar*  mask_buf [[buffer(1)]],
                                    device float4*       contrib [[buffer(2)]],
                                    device float4*       counts [[buffer(3)]],
                                    constant HighlightParams& params [[buffer(4)]],
                                    uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= params.width || gid.y >= params.height) {
    return;
  }

  const uint   size  = params.width * params.height;
  const uint   index = gid.y * params.stride + gid.x;
  const uint   idx   = gid.y * params.width + gid.x;
  const float3 pixel = MaxRgb(input[index]);

  const bool use_r = mask_buf[kPlaneDilatedR * size + idx] && pixel.x > params.clipdark[0] &&
                     pixel.x < params.clips[0];
  const bool use_g = mask_buf[kPlaneDilatedG * size + idx] && pixel.y > params.clipdark[1] &&
                     pixel.y < params.clips[1];
  const bool use_b = mask_buf[kPlaneDilatedB * size + idx] && pixel.z > params.clipdark[2] &&
                     pixel.z < params.clips[2];

  float4 contrib_value = float4(0.0f);
  float4 count_value   = float4(0.0f);

  // refavg costs nine reads; only pay for it inside the chrominance ring.
  if (use_r || use_g || use_b) {
    const float3 ref =
        CalcRefavg(input, static_cast<int>(gid.y), static_cast<int>(gid.x), params);
    if (use_r) {
      contrib_value.x = pixel.x - ref.x;
      count_value.x   = 1.0f;
    }
    if (use_g) {
      contrib_value.y = pixel.y - ref.y;
      count_value.y   = 1.0f;
    }
    if (use_b) {
      contrib_value.z = pixel.z - ref.z;
      count_value.z   = 1.0f;
    }
  }

  contrib[index] = contrib_value;
  counts[index]  = count_value;
}

kernel void hlr_reconstruct(device const float4* input [[buffer(0)]],
                            device float4*       output [[buffer(1)]],
                            constant HighlightParams& params [[buffer(2)]],
                            uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= params.width || gid.y >= params.height) {
    return;
  }

  const uint  index = gid.y * params.stride + gid.x;
  const float4 input_pixel = input[index];
  const float3 pixel       = MaxRgb(input_pixel);
  const float3 weight      = float3(SoftClipWeight(pixel.x, params.clips[0]),
                                    SoftClipWeight(pixel.y, params.clips[1]),
                                    SoftClipWeight(pixel.z, params.clips[2]));

  float3 result = pixel;
  // refavg costs nine reads; unclipped pixels keep their value and never touch it.
  if (weight.x > 0.0f || weight.y > 0.0f || weight.z > 0.0f) {
    const float3 ref =
        CalcRefavg(input, static_cast<int>(gid.y), static_cast<int>(gid.x), params);
    result.x = ReconstructChannel(pixel.x, ref.x, params.chrominance[0], weight.x);
    result.y = ReconstructChannel(pixel.y, ref.y, params.chrominance[1], weight.y);
    result.z = ReconstructChannel(pixel.z, ref.z, params.chrominance[2], weight.z);
  }

  output[index] = float4(result, input_pixel.w);
}
