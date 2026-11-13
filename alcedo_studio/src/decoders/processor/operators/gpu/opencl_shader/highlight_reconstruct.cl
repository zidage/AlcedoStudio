//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// OpenCL kernels for highlight reconstruction (opposed algorithm).
// Operates on RGBA32F buffers.

typedef struct {
  float clips[4];
  float clipdark[4];
  float chrominance[4];
  uint  width;
  uint  height;
  uint  stride;
} HighlightParams;

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
static inline float SanitizeChannel(float value) { return isfinite(value) ? fmax(value, 0.0f) : 0.0f; }

static inline float4 MaxRgb(float4 value) {
  return (float4)(SanitizeChannel(value.x), SanitizeChannel(value.y), SanitizeChannel(value.z),
                  value.w);
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
  return pixel + weight * (fmax(pixel, ref + chrominance) - pixel);
}

// Accumulate one neighbourhood sample. A sample at/above the clip level is censored by the
// sensor (the photosite has reached full well, so its readout only means ">= clip"), or is a
// hot-pixel/demosaic outlier; either way it carries no usable level information and must not
// move the mean at face value. Censored samples still feed the fallback used when a channel
// has no uncensored sample left (e.g. inside a fully blown region). Non-finite samples are
// dropped entirely.
static inline void RefavgAccumulate(float* valid_sum, float* valid_cnt, float* all_sum,
                                    float* all_cnt, float4 raw, HighlightParams params) {
  if (isfinite(raw.x)) {
    const float v = fmax(raw.x, 0.0f);
    all_sum[0] += v;
    all_cnt[0] += 1.0f;
    if (v < params.clips[0]) {
      valid_sum[0] += v;
      valid_cnt[0] += 1.0f;
    }
  }
  if (isfinite(raw.y)) {
    const float v = fmax(raw.y, 0.0f);
    all_sum[1] += v;
    all_cnt[1] += 1.0f;
    if (v < params.clips[1]) {
      valid_sum[1] += v;
      valid_cnt[1] += 1.0f;
    }
  }
  if (isfinite(raw.z)) {
    const float v = fmax(raw.z, 0.0f);
    all_sum[2] += v;
    all_cnt[2] += 1.0f;
    if (v < params.clips[2]) {
      valid_sum[2] += v;
      valid_cnt[2] += 1.0f;
    }
  }
}

static inline float4 CalcRefavg(global const float4* input, int row, int col,
                                HighlightParams params) {
  float valid_sum[3] = {0.0f, 0.0f, 0.0f};
  float valid_cnt[3] = {0.0f, 0.0f, 0.0f};
  float all_sum[3]   = {0.0f, 0.0f, 0.0f};
  float all_cnt[3]   = {0.0f, 0.0f, 0.0f};

  const int dymin = max(0, row - 1);
  const int dxmin = max(0, col - 1);
  const int dymax = min((int)params.height - 1, row + 1);
  const int dxmax = min((int)params.width - 1, col + 1);

  for (int dy = dymin; dy <= dymax; ++dy) {
    for (int dx = dxmin; dx <= dxmax; ++dx) {
      RefavgAccumulate(valid_sum, valid_cnt, all_sum, all_cnt, input[dy * params.stride + dx],
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

  return (float4)(Cube(0.5f * (mean[1] + mean[2])),
                  Cube(0.5f * (mean[0] + mean[2])),
                  Cube(0.5f * (mean[0] + mean[1])),
                  0.0f);
}

static inline uchar DilateMaskAt(global const uchar* plane, uint width, uint height, int row,
                                 int col, int radius) {
  const int y0 = max(0, row - radius);
  const int x0 = max(0, col - radius);
  const int y1 = min((int)height - 1, row + radius);
  const int x1 = min((int)width - 1, col + radius);

  for (int y = y0; y <= y1; ++y) {
    const int row_offset = y * (int)width;
    for (int x = x0; x <= x1; ++x) {
      if (plane[row_offset + x] != 0) {
        return 1;
      }
    }
  }

  return 0;
}

static inline void AtomicAddFloat(global float* addr, float val) {
  int old_val = as_int(*addr);
  int new_val;
  int cur_val;
  do {
    cur_val = old_val;
    new_val = as_int(as_float(cur_val) + val);
    old_val = atomic_cmpxchg((global int*)addr, cur_val, new_val);
  } while (old_val != cur_val);
}

__kernel void hlr_build_mask(global const float4* input,
                             global uchar*        mask_buf,
                             global int*          anyclipped,
                             HighlightParams      params,
                             uint in_off, uint mask_off, uint anyclipped_off) {
  uint x = get_global_id(0);
  uint y = get_global_id(1);
  if (x >= params.width || y >= params.height) {
    return;
  }
  const uint   size  = params.width * params.height;
  const uint   index = in_off + y * params.stride + x;
  const uint   idx   = y * params.width + x;
  const float4 pixel = MaxRgb(input[index]);

  mask_buf[mask_off + kPlaneBaseR * size + idx] = pixel.x >= params.clips[0] ? 1 : 0;
  mask_buf[mask_off + kPlaneBaseG * size + idx] = pixel.y >= params.clips[1] ? 1 : 0;
  mask_buf[mask_off + kPlaneBaseB * size + idx] = pixel.z >= params.clips[2] ? 1 : 0;

  if (pixel.x >= params.clips[0] || pixel.y >= params.clips[1] || pixel.z >= params.clips[2]) {
    atomic_add(anyclipped + anyclipped_off, 1);
  }
}

__kernel void hlr_dilate_mask(global const uchar* mask_buf,
                              global uchar*       dilated_mask_buf,
                              HighlightParams     params,
                              uint mask_off, uint dilated_off) {
  uint x = get_global_id(0);
  uint y = get_global_id(1);
  if (x >= params.width || y >= params.height) {
    return;
  }
  const uint size = params.width * params.height;
  const uint idx  = y * params.width + x;

  dilated_mask_buf[dilated_off + kPlaneDilatedR * size + idx] =
      DilateMaskAt(mask_buf + mask_off + kPlaneBaseR * size, params.width, params.height,
                   (int)y, (int)x, (int)kDilateRadius);
  dilated_mask_buf[dilated_off + kPlaneDilatedG * size + idx] =
      DilateMaskAt(mask_buf + mask_off + kPlaneBaseG * size, params.width, params.height,
                   (int)y, (int)x, (int)kDilateRadius);
  dilated_mask_buf[dilated_off + kPlaneDilatedB * size + idx] =
      DilateMaskAt(mask_buf + mask_off + kPlaneBaseB * size, params.width, params.height,
                   (int)y, (int)x, (int)kDilateRadius);
}

__kernel void hlr_chrominance_contrib(global const float4* input,
                                      global const uchar*  mask_buf,
                                      global float*        global_sums,
                                      global float*        global_cnts,
                                      HighlightParams      params,
                                      uint in_off, uint mask_off, uint sums_off, uint cnts_off) {
  uint x = get_global_id(0);
  uint y = get_global_id(1);
  bool in_bounds = x < params.width && y < params.height;

  float4 contrib_value = (float4)(0.0f, 0.0f, 0.0f, 0.0f);
  float4 count_value   = (float4)(0.0f, 0.0f, 0.0f, 0.0f);

  if (in_bounds) {
    const uint   size  = params.width * params.height;
    const uint   index = in_off + y * params.stride + x;
    const uint   idx   = y * params.width + x;
    const float4 pixel = MaxRgb(input[index]);

    const bool use_r = mask_buf[mask_off + kPlaneDilatedR * size + idx] &&
                       pixel.x > params.clipdark[0] && pixel.x < params.clips[0];
    const bool use_g = mask_buf[mask_off + kPlaneDilatedG * size + idx] &&
                       pixel.y > params.clipdark[1] && pixel.y < params.clips[1];
    const bool use_b = mask_buf[mask_off + kPlaneDilatedB * size + idx] &&
                       pixel.z > params.clipdark[2] && pixel.z < params.clips[2];

    // refavg costs nine reads; only pay for it inside the chrominance ring.
    if (use_r || use_g || use_b) {
      const float4 ref = CalcRefavg(input + in_off, (int)y, (int)x, params);
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
  }

  const uint lid  = get_local_id(1) * get_local_size(0) + get_local_id(0);
  const uint lsz  = get_local_size(0) * get_local_size(1);

  local float l_contrib_r[256];
  local float l_contrib_g[256];
  local float l_contrib_b[256];
  local float l_cnt_r[256];
  local float l_cnt_g[256];
  local float l_cnt_b[256];

  l_contrib_r[lid] = contrib_value.x;
  l_contrib_g[lid] = contrib_value.y;
  l_contrib_b[lid] = contrib_value.z;
  l_cnt_r[lid]     = count_value.x;
  l_cnt_g[lid]     = count_value.y;
  l_cnt_b[lid]     = count_value.z;

  barrier(CLK_LOCAL_MEM_FENCE);

  for (uint stride = lsz / 2; stride > 0; stride >>= 1) {
    if (lid < stride) {
      l_contrib_r[lid] += l_contrib_r[lid + stride];
      l_contrib_g[lid] += l_contrib_g[lid + stride];
      l_contrib_b[lid] += l_contrib_b[lid + stride];
      l_cnt_r[lid] += l_cnt_r[lid + stride];
      l_cnt_g[lid] += l_cnt_g[lid + stride];
      l_cnt_b[lid] += l_cnt_b[lid + stride];
    }
    barrier(CLK_LOCAL_MEM_FENCE);
  }

  if (lid == 0) {
    AtomicAddFloat(global_sums + sums_off + 0, l_contrib_r[0]);
    AtomicAddFloat(global_sums + sums_off + 1, l_contrib_g[0]);
    AtomicAddFloat(global_sums + sums_off + 2, l_contrib_b[0]);
    AtomicAddFloat(global_cnts + cnts_off + 0, l_cnt_r[0]);
    AtomicAddFloat(global_cnts + cnts_off + 1, l_cnt_g[0]);
    AtomicAddFloat(global_cnts + cnts_off + 2, l_cnt_b[0]);
  }
}

__kernel void hlr_reconstruct(global const float4* input,
                              global float4*       output,
                              HighlightParams      params) {
  uint x = get_global_id(0);
  uint y = get_global_id(1);
  if (x >= params.width || y >= params.height) {
    return;
  }

  const uint   index       = y * params.stride + x;
  const float4 input_pixel = input[index];
  const float4 pixel       = MaxRgb(input_pixel);
  const float3 weight      = (float3)(SoftClipWeight(pixel.x, params.clips[0]),
                                      SoftClipWeight(pixel.y, params.clips[1]),
                                      SoftClipWeight(pixel.z, params.clips[2]));

  float4 result = pixel;
  // refavg costs nine reads; unclipped pixels keep their value and never touch it.
  if (weight.x > 0.0f || weight.y > 0.0f || weight.z > 0.0f) {
    const float4 ref = CalcRefavg(input, (int)y, (int)x, params);
    result.x = ReconstructChannel(pixel.x, ref.x, params.chrominance[0], weight.x);
    result.y = ReconstructChannel(pixel.y, ref.y, params.chrominance[1], weight.y);
    result.z = ReconstructChannel(pixel.z, ref.z, params.chrominance[2], weight.z);
  }

  output[index] = (float4)(result.x, result.y, result.z, input_pixel.w);
}

__kernel void hlr_reconstruct_from_stats(global const float4* input,
                                         global float4*       output,
                                         global const float*  sums,
                                         global const float*  cnts,
                                         HighlightParams      params,
                                         uint in_off, uint out_off, uint sums_off, uint cnts_off) {
  uint x = get_global_id(0);
  uint y = get_global_id(1);
  if (x >= params.width || y >= params.height) {
    return;
  }

  const uint   index       = in_off + y * params.stride + x;
  const uint   out_index   = out_off + y * params.stride + x;
  const float4 input_pixel = input[index];
  const float4 pixel       = MaxRgb(input_pixel);
  const float3 weight      = (float3)(SoftClipWeight(pixel.x, params.clips[0]),
                                      SoftClipWeight(pixel.y, params.clips[1]),
                                      SoftClipWeight(pixel.z, params.clips[2]));

  float chrominance[3];
  chrominance[0] = (cnts[cnts_off + 0] > 30.0f) ? (sums[sums_off + 0] / cnts[cnts_off + 0]) : 0.0f;
  chrominance[1] = (cnts[cnts_off + 1] > 30.0f) ? (sums[sums_off + 1] / cnts[cnts_off + 1]) : 0.0f;
  chrominance[2] = (cnts[cnts_off + 2] > 30.0f) ? (sums[sums_off + 2] / cnts[cnts_off + 2]) : 0.0f;

  float4 result = pixel;
  if (weight.x > 0.0f || weight.y > 0.0f || weight.z > 0.0f) {
    const float4 ref = CalcRefavg(input + in_off, (int)y, (int)x, params);
    result.x = ReconstructChannel(pixel.x, ref.x, chrominance[0], weight.x);
    result.y = ReconstructChannel(pixel.y, ref.y, chrominance[1], weight.y);
    result.z = ReconstructChannel(pixel.z, ref.z, chrominance[2], weight.z);
  }

  output[out_index] = (float4)(result.x, result.y, result.z, input_pixel.w);
}

typedef struct {
  uint  src_x;
  uint  src_y;
  uint  src_width;
  uint  src_height;
  uint  dst_width;
  uint  dst_height;
  uint  flip;
  float scale_r;
  float scale_g;
  float scale_b;
  uint  plane_stride;
  uint  src_stride;
} PackOrientParams;

static inline int2 OrientedCoord(uint x, uint y, PackOrientParams params) {
  if (params.flip == 3u) {
    return (int2)((int)(params.src_width - 1u - x), (int)(params.src_height - 1u - y));
  }
  if (params.flip == 5u) {
    return (int2)((int)y, (int)(params.src_width - 1u - x));
  }
  if (params.flip == 6u) {
    return (int2)((int)(params.src_height - 1u - y), (int)x);
  }
  return (int2)((int)x, (int)y);
}

static inline float4 LoadPlanarRgb(global const float* r, global const float* g,
                                   global const float* b, uint r_off, uint g_off, uint b_off,
                                   int row, int col, uint plane_stride) {
  const uint src = (uint)row * plane_stride + (uint)col;
  return (float4)(r[r_off + src], g[g_off + src], b[b_off + src], 1.0f);
}

static inline float4 CalcRefavgPlanar(global const float* r, global const float* g,
                                       global const float* b, uint r_off, uint g_off, uint b_off,
                                       int local_row, int local_col, HighlightParams params,
                                       uint crop_x, uint crop_y) {
  float valid_sum[3] = {0.0f, 0.0f, 0.0f};
  float valid_cnt[3] = {0.0f, 0.0f, 0.0f};
  float all_sum[3]   = {0.0f, 0.0f, 0.0f};
  float all_cnt[3]   = {0.0f, 0.0f, 0.0f};

  const int dymin = max(0, local_row - 1);
  const int dxmin = max(0, local_col - 1);
  const int dymax = min((int)params.height - 1, local_row + 1);
  const int dxmax = min((int)params.width - 1, local_col + 1);

  for (int dy = dymin; dy <= dymax; ++dy) {
    for (int dx = dxmin; dx <= dxmax; ++dx) {
      RefavgAccumulate(valid_sum, valid_cnt, all_sum, all_cnt,
                       LoadPlanarRgb(r, g, b, r_off, g_off, b_off, (int)crop_y + dy,
                                     (int)crop_x + dx, params.stride),
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

  return (float4)(Cube(0.5f * (mean[1] + mean[2])), Cube(0.5f * (mean[0] + mean[2])),
                  Cube(0.5f * (mean[0] + mean[1])), 0.0f);
}

__kernel void hlr_build_mask_planar(global const float* r, global const float* g,
                                      global const float* b, global uchar* mask_buf,
                                      global int* anyclipped, HighlightParams params, uint r_off,
                                      uint g_off, uint b_off, uint mask_off, uint anyclipped_off,
                                      uint crop_x, uint crop_y) {
  uint x = get_global_id(0);
  uint y = get_global_id(1);
  if (x >= params.width || y >= params.height) {
    return;
  }
  const uint   size  = params.width * params.height;
  const uint   idx   = y * params.width + x;
  const float4 pixel = MaxRgb(LoadPlanarRgb(r, g, b, r_off, g_off, b_off, (int)crop_y + (int)y,
                                             (int)crop_x + (int)x, params.stride));

  mask_buf[mask_off + kPlaneBaseR * size + idx] = pixel.x >= params.clips[0] ? 1 : 0;
  mask_buf[mask_off + kPlaneBaseG * size + idx] = pixel.y >= params.clips[1] ? 1 : 0;
  mask_buf[mask_off + kPlaneBaseB * size + idx] = pixel.z >= params.clips[2] ? 1 : 0;

  if (pixel.x >= params.clips[0] || pixel.y >= params.clips[1] || pixel.z >= params.clips[2]) {
    atomic_add(anyclipped + anyclipped_off, 1);
  }
}

__kernel void hlr_chrominance_contrib_planar(global const float* r, global const float* g,
                                              global const float* b, global const uchar* mask_buf,
                                              global float* global_sums, global float* global_cnts,
                                              HighlightParams params, uint r_off, uint g_off,
                                              uint b_off, uint mask_off, uint sums_off,
                                              uint cnts_off, uint crop_x, uint crop_y) {
  uint x         = get_global_id(0);
  uint y         = get_global_id(1);
  bool in_bounds = x < params.width && y < params.height;

  float4 contrib_value = (float4)(0.0f, 0.0f, 0.0f, 0.0f);
  float4 count_value   = (float4)(0.0f, 0.0f, 0.0f, 0.0f);

  if (in_bounds) {
    const uint   size  = params.width * params.height;
    const uint   idx   = y * params.width + x;
    const float4 pixel = MaxRgb(LoadPlanarRgb(r, g, b, r_off, g_off, b_off, (int)crop_y + (int)y,
                                               (int)crop_x + (int)x, params.stride));

    const bool use_r = mask_buf[mask_off + kPlaneDilatedR * size + idx] &&
                       pixel.x > params.clipdark[0] && pixel.x < params.clips[0];
    const bool use_g = mask_buf[mask_off + kPlaneDilatedG * size + idx] &&
                       pixel.y > params.clipdark[1] && pixel.y < params.clips[1];
    const bool use_b = mask_buf[mask_off + kPlaneDilatedB * size + idx] &&
                       pixel.z > params.clipdark[2] && pixel.z < params.clips[2];

    if (use_r || use_g || use_b) {
      const float4 ref =
          CalcRefavgPlanar(r, g, b, r_off, g_off, b_off, (int)y, (int)x, params, crop_x, crop_y);
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
  }

  const uint lid = get_local_id(1) * get_local_size(0) + get_local_id(0);
  const uint lsz = get_local_size(0) * get_local_size(1);

  local float l_contrib_r[256];
  local float l_contrib_g[256];
  local float l_contrib_b[256];
  local float l_cnt_r[256];
  local float l_cnt_g[256];
  local float l_cnt_b[256];

  l_contrib_r[lid] = contrib_value.x;
  l_contrib_g[lid] = contrib_value.y;
  l_contrib_b[lid] = contrib_value.z;
  l_cnt_r[lid]     = count_value.x;
  l_cnt_g[lid]     = count_value.y;
  l_cnt_b[lid]     = count_value.z;

  barrier(CLK_LOCAL_MEM_FENCE);

  for (uint stride = lsz / 2; stride > 0; stride >>= 1) {
    if (lid < stride) {
      l_contrib_r[lid] += l_contrib_r[lid + stride];
      l_contrib_g[lid] += l_contrib_g[lid + stride];
      l_contrib_b[lid] += l_contrib_b[lid + stride];
      l_cnt_r[lid] += l_cnt_r[lid + stride];
      l_cnt_g[lid] += l_cnt_g[lid + stride];
      l_cnt_b[lid] += l_cnt_b[lid + stride];
    }
    barrier(CLK_LOCAL_MEM_FENCE);
  }

  if (lid == 0) {
    AtomicAddFloat(global_sums + sums_off + 0, l_contrib_r[0]);
    AtomicAddFloat(global_sums + sums_off + 1, l_contrib_g[0]);
    AtomicAddFloat(global_sums + sums_off + 2, l_contrib_b[0]);
    AtomicAddFloat(global_cnts + cnts_off + 0, l_cnt_r[0]);
    AtomicAddFloat(global_cnts + cnts_off + 1, l_cnt_g[0]);
    AtomicAddFloat(global_cnts + cnts_off + 2, l_cnt_b[0]);
  }
}

__kernel void hlr_reconstruct_from_stats_planar_pack(global const float* r, global const float* g,
                                                      global const float* b,
                                                      __write_only image2d_t dst,
                                                      global const float* sums,
                                                      global const float* cnts,
                                                      HighlightParams params, PackOrientParams pack,
                                                      uint r_off, uint g_off, uint b_off,
                                                      uint sums_off, uint cnts_off) {
  uint x = get_global_id(0);
  uint y = get_global_id(1);
  if (x >= pack.src_width || y >= pack.src_height) {
    return;
  }

  const float4 input_pixel = LoadPlanarRgb(r, g, b, r_off, g_off, b_off, (int)pack.src_y + (int)y,
                                            (int)pack.src_x + (int)x, pack.plane_stride);
  const float4 pixel        = MaxRgb(input_pixel);
  const float3 weight        = (float3)(SoftClipWeight(pixel.x, params.clips[0]),
                                          SoftClipWeight(pixel.y, params.clips[1]),
                                          SoftClipWeight(pixel.z, params.clips[2]));

  float chrominance[3];
  chrominance[0] = (cnts[cnts_off + 0] > 30.0f) ? (sums[sums_off + 0] / cnts[cnts_off + 0]) : 0.0f;
  chrominance[1] = (cnts[cnts_off + 1] > 30.0f) ? (sums[sums_off + 1] / cnts[cnts_off + 1]) : 0.0f;
  chrominance[2] = (cnts[cnts_off + 2] > 30.0f) ? (sums[sums_off + 2] / cnts[cnts_off + 2]) : 0.0f;

  float4 result = pixel;
  if (weight.x > 0.0f || weight.y > 0.0f || weight.z > 0.0f) {
    const float4 ref = CalcRefavgPlanar(r, g, b, r_off, g_off, b_off, (int)y, (int)x, params,
                                        pack.src_x, pack.src_y);
    result.x        = ReconstructChannel(pixel.x, ref.x, chrominance[0], weight.x);
    result.y        = ReconstructChannel(pixel.y, ref.y, chrominance[1], weight.y);
    result.z        = ReconstructChannel(pixel.z, ref.z, chrominance[2], weight.z);
  }

  write_imagef(dst, OrientedCoord(x, y, pack),
               (float4)(result.x * pack.scale_r, result.y * pack.scale_g, result.z * pack.scale_b,
                        1.0f));
}
