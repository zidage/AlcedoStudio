//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <metal_stdlib>

using namespace metal;

struct WBParams {
  float black_level[4];
  float white_level[4];
  float wb_multipliers[4];
  uint  apply_white_balance;
  uint  padding[3];
};

struct ToLinearRefParams {
  uint width;
  uint height;
  uint tile_width;
  uint tile_height;
  uint black_tile_width;
  uint black_tile_height;
  uint raw_fc[36];
};

static inline uint RawColorAt(constant ToLinearRefParams& params, uint y, uint x) {
  const uint tile_y = params.tile_height == 0u ? 0u : (y % params.tile_height);
  const uint tile_x = params.tile_width == 0u ? 0u : (x % params.tile_width);
  return params.raw_fc[tile_y * params.tile_width + tile_x];
}

static inline float PatternBlackAt(constant ToLinearRefParams& params, constant float* pattern_black,
                                   uint y, uint x) {
  if (params.black_tile_width == 0u || params.black_tile_height == 0u) {
    return 0.0f;
  }
  const uint tile_y = y % params.black_tile_height;
  const uint tile_x = x % params.black_tile_width;
  return pattern_black[tile_y * params.black_tile_width + tile_x];
}

static inline float LinearizeSample(float sample, uint color_idx, constant ToLinearRefParams& params,
                                    constant WBParams& wb_params, constant float* pattern_black,
                                    uint y, uint x) {
  const float black =
      wb_params.black_level[color_idx] + PatternBlackAt(params, pattern_black, y, x);
  const float denom     = wb_params.white_level[color_idx] - black;
  float       pixel_val = denom > 0.0f ? clamp((sample - black) / denom, 0.0f, 1.0f) : 0.0f;

  if (wb_params.apply_white_balance != 0u && wb_params.wb_multipliers[1] > 0.0f &&
      (color_idx == 0u || color_idx == 2u)) {
    pixel_val *= wb_params.wb_multipliers[color_idx] / wb_params.wb_multipliers[1];
  }
  return pixel_val;
}

kernel void to_linear_ref_r16u(texture2d<ushort, access::read> src [[texture(0)]],
                               texture2d<float, access::write> dst [[texture(1)]],
                               constant ToLinearRefParams&     params [[buffer(0)]],
                               constant WBParams&              wb_params [[buffer(1)]],
                               constant float*                 pattern_black [[buffer(2)]],
                               uint2                           gid [[thread_position_in_grid]]) {
  if (gid.x >= params.width || gid.y >= params.height) {
    return;
  }

  const uint  color_idx = RawColorAt(params, gid.y, gid.x);
  const float sample    = float(src.read(gid).r);
  dst.write(LinearizeSample(sample, color_idx, params, wb_params, pattern_black, gid.y, gid.x),
            gid);
}

struct ClampParams {
  uint width;
  uint height;
};

kernel void cfa_clamp01_r32f(texture2d<float, access::read_write> image [[texture(0)]],
                             constant ClampParams&                params [[buffer(0)]],
                             uint2                                gid [[thread_position_in_grid]]) {
  if (gid.x >= params.width || gid.y >= params.height) {
    return;
  }
  const float sample = image.read(gid).r;
  image.write(clamp(sample, 0.0f, 1.0f), gid);
}
