//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// OpenCL kernels for RAW to linear reference conversion.
// Depends on definitions from raw_utils_opencl.cl (concatenated at build time).

typedef struct {
  float black[3];
  float scale[3];
  uint integer_codes;
} RawRgbLinearizationParams;

__kernel void linearize_rgb(global float4* rgba, uint width, uint height,
                            RawRgbLinearizationParams params, uint offset) {
  const uint x = get_global_id(0);
  const uint y = get_global_id(1);
  if (x >= width || y >= height) return;
  float4 value = rgba[offset + y * width + x];
  for (int c = 0; c < 3; ++c) {
    value[c] -= params.black[c];
    if (params.integer_codes) value[c] = fmax(value[c], 0.0f);
    value[c] *= params.scale[c];
  }
  value.w = 1.0f;
  rgba[offset + y * width + x] = value;
}

// Converts uint16 raw input to linearized float output in one pass.
__kernel void to_linear_ref_u16_to_f32(global const ushort* image_in,
                                       global float*          image_out,
                                       WBParams               wb,
                                       PatternParams          pattern,
                                       uint                   in_offset,
                                       uint                   out_offset) {
  int x = get_global_id(0);
  int y = get_global_id(1);

  if (x >= pattern.width || y >= pattern.height) {
    return;
  }

  int color_idx = RawColorAt(pattern, y, x);
  int idx       = y * pattern.width + x;

  float sample = (float)image_in[in_offset + idx];
  float black  = wb.black_level[color_idx] + PatternBlackAt(wb, y, x);
  float pixel  = NormalizeSample(sample, black, wb.white_level[color_idx]);
  pixel *= RelativeWBMultiplier(wb, color_idx);

  image_out[out_offset + idx] = pixel;
}

__kernel void cfa_clamp01_f32(global float* image, uint width, uint height, uint offset) {
  uint x = get_global_id(0);
  uint y = get_global_id(1);
  if (x >= width || y >= height) {
    return;
  }
  const uint idx   = offset + y * width + x;
  const float sample = image[idx];
  image[idx]         = Clamp01(sample);
}

// In-place float variant for already-converted buffers.
__kernel void to_linear_ref_f32(global float* image,
                                WBParams      wb,
                                PatternParams pattern) {
  int x = get_global_id(0);
  int y = get_global_id(1);

  if (x >= pattern.width || y >= pattern.height) {
    return;
  }

  int color_idx = RawColorAt(pattern, y, x);
  int idx       = y * pattern.width + x;

  float sample = image[idx];
  float black  = wb.black_level[color_idx] + PatternBlackAt(wb, y, x);
  float pixel  = NormalizeSample(sample, black, wb.white_level[color_idx]);
  pixel *= RelativeWBMultiplier(wb, color_idx);

  image[idx] = pixel;
}
