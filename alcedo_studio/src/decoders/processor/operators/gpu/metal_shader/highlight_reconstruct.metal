//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <metal_stdlib>

using namespace metal;

struct HighlightParams {
  float clips[4];
  float clipdark[4];
  uint  width;
  uint  height;
};

constant uint  kDilateRadius     = 3u;
constant uint  kBlockX           = 16u;
constant uint  kBlockY           = 16u;
constant uint  kTileW            = kBlockX + 2u * kDilateRadius;
constant uint  kTileH            = kBlockY + 2u * kDilateRadius;
constant float kSoftClipLo       = 0.95f;
constant float kMinChromaSamples = 30.0f;

static inline float Cube(float value) { return value * value * value; }

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

static inline float3 RefavgFinalize(thread const float* valid_sum, thread const float* valid_cnt,
                                    thread const float* all_sum, thread const float* all_cnt) {
  float mean[3];
  for (uint c = 0u; c < 3u; ++c) {
    const float m = (valid_cnt[c] > 0.0f) ? valid_sum[c] / valid_cnt[c]
                    : (all_cnt[c] > 0.0f)   ? all_sum[c] / all_cnt[c]
                                            : 0.0f;
    mean[c] = pow(m, 1.0f / 3.0f);
  }
  return float3(Cube(0.5f * (mean[1] + mean[2])), Cube(0.5f * (mean[0] + mean[2])),
                Cube(0.5f * (mean[0] + mean[1])));
}

static inline float3 CalcRefavgFromTile(threadgroup float3 tile_img[kTileH][kTileW], int row,
                                        int col, int local_y, int local_x, int height, int width,
                                        constant HighlightParams& params) {
  float valid_sum[3] = {0.0f, 0.0f, 0.0f};
  float valid_cnt[3] = {0.0f, 0.0f, 0.0f};
  float all_sum[3]   = {0.0f, 0.0f, 0.0f};
  float all_cnt[3]   = {0.0f, 0.0f, 0.0f};

  const int dy0 = max(-1, -row);
  const int dx0 = max(-1, -col);
  const int dy1 = min(1, height - 1 - row);
  const int dx1 = min(1, width - 1 - col);
  for (int dy = dy0; dy <= dy1; ++dy) {
    for (int dx = dx0; dx <= dx1; ++dx) {
      RefavgAccumulate(valid_sum, valid_cnt, all_sum, all_cnt,
                       tile_img[local_y + dy][local_x + dx], params);
    }
  }
  return RefavgFinalize(valid_sum, valid_cnt, all_sum, all_cnt);
}

static inline float3 CalcRefavgTex(texture2d<float, access::read> src, int row, int col,
                                   constant HighlightParams& params) {
  float valid_sum[3] = {0.0f, 0.0f, 0.0f};
  float valid_cnt[3] = {0.0f, 0.0f, 0.0f};
  float all_sum[3]   = {0.0f, 0.0f, 0.0f};
  float all_cnt[3]   = {0.0f, 0.0f, 0.0f};

  const int height = static_cast<int>(params.height);
  const int width  = static_cast<int>(params.width);
  const int dymin  = max(0, row - 1);
  const int dxmin  = max(0, col - 1);
  const int dymax  = min(height - 1, row + 1);
  const int dxmax  = min(width - 1, col + 1);
  for (int dy = dymin; dy <= dymax; ++dy) {
    for (int dx = dxmin; dx <= dxmax; ++dx) {
      RefavgAccumulate(valid_sum, valid_cnt, all_sum, all_cnt,
                       MaxRgb(src.read(uint2(static_cast<uint>(dx), static_cast<uint>(dy)))),
                       params);
    }
  }
  return RefavgFinalize(valid_sum, valid_cnt, all_sum, all_cnt);
}

// One pass: load a halo tile, dilate, and atomically fold chrominance. Matches the CUDA
// AccumulateHighlightStats kernel so we never materialize mask/contrib planes.
kernel void hlr_accumulate_stats(texture2d<float, access::read> src [[texture(0)]],
                                 device atomic<float>*          stats [[buffer(0)]],
                                 constant HighlightParams&      params [[buffer(1)]],
                                 uint2 gid [[thread_position_in_grid]],
                                 uint2 lid [[thread_position_in_threadgroup]],
                                 uint2 groupid [[threadgroup_position_in_grid]]) {
  threadgroup float3 tile_img[kTileH][kTileW];
  threadgroup uchar  tile_r[kTileH][kTileW];
  threadgroup uchar  tile_g[kTileH][kTileW];
  threadgroup uchar  tile_b[kTileH][kTileW];
  threadgroup float  tg_sum[3][256];
  threadgroup float  tg_cnt[3][256];

  const uint lane = lid.y * kBlockX + lid.x;

  const int tile_origin_x = static_cast<int>(groupid.x * kBlockX) - static_cast<int>(kDilateRadius);
  const int tile_origin_y = static_cast<int>(groupid.y * kBlockY) - static_cast<int>(kDilateRadius);
  const int width         = static_cast<int>(params.width);
  const int height        = static_cast<int>(params.height);

  for (uint sy = lid.y; sy < kTileH; sy += kBlockY) {
    const int gy = clamp(tile_origin_y + static_cast<int>(sy), 0, max(height - 1, 0));
    for (uint sx = lid.x; sx < kTileW; sx += kBlockX) {
      const int    gx    = clamp(tile_origin_x + static_cast<int>(sx), 0, max(width - 1, 0));
      const float3 pixel = MaxRgb(src.read(uint2(static_cast<uint>(gx), static_cast<uint>(gy))));
      tile_img[sy][sx]   = pixel;
      tile_r[sy][sx]     = pixel.x >= params.clips[0] ? 1 : 0;
      tile_g[sy][sx]     = pixel.y >= params.clips[1] ? 1 : 0;
      tile_b[sy][sx]     = pixel.z >= params.clips[2] ? 1 : 0;
    }
  }
  threadgroup_barrier(mem_flags::mem_threadgroup);

  float local_sum[3] = {0.0f, 0.0f, 0.0f};
  float local_cnt[3] = {0.0f, 0.0f, 0.0f};

  const bool in_bounds = gid.x < params.width && gid.y < params.height;
  if (in_bounds) {
    const int row     = static_cast<int>(gid.y);
    const int col     = static_cast<int>(gid.x);
    const int local_x = static_cast<int>(lid.x) + static_cast<int>(kDilateRadius);
    const int local_y = static_cast<int>(lid.y) + static_cast<int>(kDilateRadius);

    const int dy0 = max(-static_cast<int>(kDilateRadius), -row);
    const int dx0 = max(-static_cast<int>(kDilateRadius), -col);
    const int dy1 = min(static_cast<int>(kDilateRadius), height - 1 - row);
    const int dx1 = min(static_cast<int>(kDilateRadius), width - 1 - col);

    uchar dil_r = 0;
    uchar dil_g = 0;
    uchar dil_b = 0;
    for (int dy = dy0; dy <= dy1; ++dy) {
      for (int dx = dx0; dx <= dx1; ++dx) {
        dil_r |= tile_r[local_y + dy][local_x + dx];
        dil_g |= tile_g[local_y + dy][local_x + dx];
        dil_b |= tile_b[local_y + dy][local_x + dx];
      }
    }

    const float3 pixel = tile_img[local_y][local_x];
    const bool   use_r =
        dil_r != 0 && pixel.x > params.clipdark[0] && pixel.x < params.clips[0];
    const bool use_g =
        dil_g != 0 && pixel.y > params.clipdark[1] && pixel.y < params.clips[1];
    const bool use_b =
        dil_b != 0 && pixel.z > params.clipdark[2] && pixel.z < params.clips[2];

    if (use_r || use_g || use_b) {
      const float3 ref =
          CalcRefavgFromTile(tile_img, row, col, local_y, local_x, height, width, params);
      if (use_r) {
        local_sum[0] = pixel.x - ref.x;
        local_cnt[0] = 1.0f;
      }
      if (use_g) {
        local_sum[1] = pixel.y - ref.y;
        local_cnt[1] = 1.0f;
      }
      if (use_b) {
        local_sum[2] = pixel.z - ref.z;
        local_cnt[2] = 1.0f;
      }
    }
  }

  tg_sum[0][lane] = local_sum[0];
  tg_sum[1][lane] = local_sum[1];
  tg_sum[2][lane] = local_sum[2];
  tg_cnt[0][lane] = local_cnt[0];
  tg_cnt[1][lane] = local_cnt[1];
  tg_cnt[2][lane] = local_cnt[2];
  threadgroup_barrier(mem_flags::mem_threadgroup);

  for (uint stride = 128u; stride > 0u; stride >>= 1u) {
    if (lane < stride) {
      tg_sum[0][lane] += tg_sum[0][lane + stride];
      tg_sum[1][lane] += tg_sum[1][lane + stride];
      tg_sum[2][lane] += tg_sum[2][lane + stride];
      tg_cnt[0][lane] += tg_cnt[0][lane + stride];
      tg_cnt[1][lane] += tg_cnt[1][lane + stride];
      tg_cnt[2][lane] += tg_cnt[2][lane + stride];
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
  }

  if (lane == 0u) {
    for (uint c = 0u; c < 3u; ++c) {
      if (tg_cnt[c][0] > 0.0f || tg_sum[c][0] != 0.0f) {
        atomic_fetch_add_explicit(stats + c, tg_sum[c][0], memory_order_relaxed);
        atomic_fetch_add_explicit(stats + 3u + c, tg_cnt[c][0], memory_order_relaxed);
      }
    }
  }
}

kernel void hlr_reconstruct_tex(texture2d<float, access::read>  src [[texture(0)]],
                                texture2d<float, access::write> dst [[texture(1)]],
                                device const float*             stats [[buffer(0)]],
                                constant HighlightParams&       params [[buffer(1)]],
                                uint2                           gid [[thread_position_in_grid]]) {
  if (gid.x >= params.width || gid.y >= params.height) {
    return;
  }

  const float4 input_pixel = src.read(gid);
  const float3 pixel       = MaxRgb(input_pixel);
  const float3 weight      = float3(SoftClipWeight(pixel.x, params.clips[0]),
                                    SoftClipWeight(pixel.y, params.clips[1]),
                                    SoftClipWeight(pixel.z, params.clips[2]));

  float3 chrominance = float3(0.0f);
  for (uint c = 0u; c < 3u; ++c) {
    const float cnt = stats[3u + c];
    chrominance[c]  = (cnt > kMinChromaSamples) ? (stats[c] / cnt) : 0.0f;
  }

  float3 result = pixel;
  if (weight.x > 0.0f || weight.y > 0.0f || weight.z > 0.0f) {
    const float3 ref =
        CalcRefavgTex(src, static_cast<int>(gid.y), static_cast<int>(gid.x), params);
    result.x = ReconstructChannel(pixel.x, ref.x, chrominance.x, weight.x);
    result.y = ReconstructChannel(pixel.y, ref.y, chrominance.y, weight.y);
    result.z = ReconstructChannel(pixel.z, ref.z, chrominance.z, weight.z);
  }

  dst.write(float4(result, input_pixel.w), gid);
}
