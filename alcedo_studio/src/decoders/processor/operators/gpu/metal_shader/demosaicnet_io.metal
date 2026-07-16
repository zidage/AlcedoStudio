//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
//
// Metal DemosaicNet tile boundary + fused post/output tail kernels.
// - Input:  R32F CFA texture → fixed NHWC FP32 tile buffer (phase + reflect-101 + sparse RGB + gamma).
// - Ordinary output: NHWC RGB tile → crop-sized RGBA texture (ownership ∩ crop + gamma decode).
// - Fused tail: NHWC cat6 → post 3×3 + bias + ReLU + output 1×1 + bias + optional gamma
//   → either crop-sized RGBA (product) or NHWC RGB buffer (reference), without materializing
//   the 24/32-channel post activation. Matches CUDA fused_post_output semantics.

#include <metal_stdlib>

using namespace metal;

// Must match host-side DemosaicNetTileInputParams / DemosaicNetTileOutputParams /
// DemosaicNetFusedTailParams.
struct DemosaicNetTileInputParams {
  int batch_index;
  int origin_x;
  int origin_y;
  int tile_w;
  int tile_h;
  int aligned_w;
  int aligned_h;
  int shift_sx;
  int shift_sy;
  int full_w;
  int full_h;
  int period;
  int rgb_fc[36];
};

struct DemosaicNetTileOutputParams {
  int batch_index;
  int tile_w;
  int tile_h;
  int src_x0;
  int src_y0;
  int owned_w;
  int owned_h;
  int dst_x;
  int dst_y;
  int crop_x;
  int crop_y;
  int crop_w;
  int crop_h;
};

// Fused student tail: cat [N,H,W,6] → export RGB.
// post_weight OIHW [Cout,6,3,3] as flat [Cout][6][9]; out_weight_cio [Cout,3].
struct DemosaicNetFusedTailParams {
  int batch_index;
  int cat_h;
  int cat_w;
  int export_h;
  int export_w;
  int final_crop;  // center crop of natural (cat-2) into export 1024
  // Ownership / product crop (RGBA path). Ignored by the NHWC buffer path.
  int src_x0;
  int src_y0;
  int owned_w;
  int owned_h;
  int dst_x;
  int dst_y;
  int crop_x;
  int crop_y;
  int crop_w;
  int crop_h;
  int apply_gamma;
};

constant float kGammaEncode = 1.0f / 2.2f;
constant float kGammaDecode = 2.2f;

static inline int Reflect101(int coordinate, int limit) {
  if (limit <= 1) {
    return 0;
  }
  int reflected = coordinate;
  while (reflected < 0 || reflected >= limit) {
    reflected = reflected < 0 ? -reflected : 2 * limit - reflected - 2;
  }
  return reflected;
}

static inline float PowSigned(float v, float gamma) {
  if (v == 0.0f) {
    return 0.0f;
  }
  return copysign(pow(fabs(v), gamma), v);
}

static inline int WrapPeriod(int coordinate, int period) {
  if (period <= 0) {
    return 0;
  }
  int wrapped = coordinate % period;
  return wrapped < 0 ? wrapped + period : wrapped;
}

static inline int RgbColorAt(constant DemosaicNetTileInputParams& params, int y, int x) {
  const int period = params.period;
  const int wy     = WrapPeriod(y, period);
  const int wx     = WrapPeriod(x, period);
  return params.rgb_fc[wy * period + wx];
}

// Pack one model tile from the original R32F CFA texture.
// Coordinates: job origin is signed in the phase-aligned lattice. Reflect-101 is applied in
// aligned space, then the phase shift maps into the full-frame texture. Sparse RGB uses the
// training-origin pattern at the reflected aligned sample.
kernel void demosaicnet_tile_input_nhwc(
    texture2d<float, access::read> cfa [[texture(0)]],
    device float*                  tile_nhwc [[buffer(0)]],
    constant DemosaicNetTileInputParams& params [[buffer(1)]],
    uint2                          gid [[thread_position_in_grid]]) {
  const int x = static_cast<int>(gid.x);
  const int y = static_cast<int>(gid.y);
  if (x >= params.tile_w || y >= params.tile_h) {
    return;
  }

  const int aligned_x = Reflect101(params.origin_x + x, params.aligned_w);
  const int aligned_y = Reflect101(params.origin_y + y, params.aligned_h);
  const int tex_x     = aligned_x + params.shift_sx;
  const int tex_y     = aligned_y + params.shift_sy;

  float linear = 0.0f;
  if (tex_x >= 0 && tex_y >= 0 && tex_x < params.full_w && tex_y < params.full_h) {
    linear = cfa.read(uint2(static_cast<uint>(tex_x), static_cast<uint>(tex_y))).r;
  }

  const int   color  = RgbColorAt(params, aligned_y, aligned_x);
  const float encoded = PowSigned(linear, kGammaEncode);

  const int tile_elements = params.tile_w * params.tile_h * 3;
  const int base = params.batch_index * tile_elements + (y * params.tile_w + x) * 3;
  tile_nhwc[base + 0] = (color == 0) ? encoded : 0.0f;
  tile_nhwc[base + 1] = (color == 1) ? encoded : 0.0f;
  tile_nhwc[base + 2] = (color == 2) ? encoded : 0.0f;
}

// Assemble the tile-owned ROI into the crop-sized RGBA result texture.
// Destination ROIs are first-writer exclusive in the aligned lattice; only the intersection
// with product_crop is written. Gamma decode is applied here so the graph stays gamma-encoded.
kernel void demosaicnet_tile_output_rgba(
    device const float*                  tile_nhwc [[buffer(0)]],
    texture2d<float, access::write>      out_rgba [[texture(0)]],
    constant DemosaicNetTileOutputParams& params [[buffer(1)]],
    uint2                                gid [[thread_position_in_grid]]) {
  const int ox = static_cast<int>(gid.x);
  const int oy = static_cast<int>(gid.y);
  if (ox >= params.owned_w || oy >= params.owned_h) {
    return;
  }

  const int aligned_x = params.dst_x + ox;
  const int aligned_y = params.dst_y + oy;
  if (aligned_x < params.crop_x || aligned_y < params.crop_y ||
      aligned_x >= params.crop_x + params.crop_w || aligned_y >= params.crop_y + params.crop_h) {
    return;
  }

  const int src_x = params.src_x0 + ox;
  const int src_y = params.src_y0 + oy;
  if (src_x < 0 || src_y < 0 || src_x >= params.tile_w || src_y >= params.tile_h) {
    return;
  }

  const int tile_elements = params.tile_w * params.tile_h * 3;
  const int base = params.batch_index * tile_elements + (src_y * params.tile_w + src_x) * 3;
  const float r    = PowSigned(tile_nhwc[base + 0], kGammaDecode);
  const float g    = PowSigned(tile_nhwc[base + 1], kGammaDecode);
  const float b    = PowSigned(tile_nhwc[base + 2], kGammaDecode);

  const int out_x = aligned_x - params.crop_x;
  const int out_y = aligned_y - params.crop_y;
  out_rgba.write(float4(r, g, b, 1.0f),
                 uint2(static_cast<uint>(out_x), static_cast<uint>(out_y)));
}

// ---------------------------------------------------------------------------
// Fused post 3×3 + ReLU + output 1×1 (+ optional gamma). Cout specialized as
// w24 / w32 entry points (Bayer / X-Trans).
// post_w: [Cout, 6, 9] OIHW spatial-flattened; out_w_cio: [Cout, 3].
// ---------------------------------------------------------------------------

constant int kPostCin = 6;
constant int kRgbCout = 3;

static inline float3 ApplyGammaDecode(float3 rgb, int apply_gamma) {
  if (apply_gamma != 0) {
    rgb.x = PowSigned(rgb.x, kGammaDecode);
    rgb.y = PowSigned(rgb.y, kGammaDecode);
    rgb.z = PowSigned(rgb.z, kGammaDecode);
  }
  return rgb;
}

// Load the 3×3×6 patch once, then accumulate all post channels.
#define DEMOASICNET_FUSED_POST_OUTPUT(kPostCout)                                                  \
  float acc[kPostCout];                                                                           \
  for (int co = 0; co < (kPostCout); ++co) {                                                      \
    acc[co] = 0.0f;                                                                               \
  }                                                                                               \
  for (int ci = 0; ci < kPostCin; ++ci) {                                                         \
    float patch[9];                                                                               \
    for (int ky = 0; ky < 3; ++ky) {                                                              \
      for (int kx = 0; kx < 3; ++kx) {                                                            \
        const int gy  = natural_y + ky;                                                           \
        const int gx  = natural_x + kx;                                                           \
        patch[ky * 3 + kx] = cat_n[((gy * cat_w + gx) * kPostCin) + ci];                          \
      }                                                                                           \
    }                                                                                             \
    for (int co = 0; co < (kPostCout); ++co) {                                                    \
      const int wbase = (co * kPostCin + ci) * 9;                                                 \
      float     a     = acc[co];                                                                  \
      a               = fma(patch[0], post_w[wbase + 0], a);                                      \
      a               = fma(patch[1], post_w[wbase + 1], a);                                      \
      a               = fma(patch[2], post_w[wbase + 2], a);                                      \
      a               = fma(patch[3], post_w[wbase + 3], a);                                      \
      a               = fma(patch[4], post_w[wbase + 4], a);                                      \
      a               = fma(patch[5], post_w[wbase + 5], a);                                      \
      a               = fma(patch[6], post_w[wbase + 6], a);                                      \
      a               = fma(patch[7], post_w[wbase + 7], a);                                      \
      a               = fma(patch[8], post_w[wbase + 8], a);                                      \
      acc[co]         = a;                                                                        \
    }                                                                                             \
  }                                                                                               \
  for (int co = 0; co < (kPostCout); ++co) {                                                      \
    acc[co] = max(acc[co] + post_b[co], 0.0f);                                                    \
  }                                                                                               \
  float3 rgb = float3(out_b[0], out_b[1], out_b[2]);                                              \
  for (int co = 0; co < (kPostCout); ++co) {                                                      \
    const int w3 = co * kRgbCout;                                                                 \
    const float p = acc[co];                                                                      \
    rgb.x         = fma(p, out_w_cio[w3 + 0], rgb.x);                                             \
    rgb.y         = fma(p, out_w_cio[w3 + 1], rgb.y);                                             \
    rgb.z         = fma(p, out_w_cio[w3 + 2], rgb.z);                                             \
  }

kernel void demosaicnet_fused_tail_rgba_w24(
    device const float* cat_nhwc [[buffer(0)]],
    device const float* post_w [[buffer(1)]],
    device const float* post_b [[buffer(2)]],
    device const float* out_w_cio [[buffer(3)]],
    device const float* out_b [[buffer(4)]],
    texture2d<float, access::write> out_rgba [[texture(0)]],
    constant DemosaicNetFusedTailParams& params [[buffer(5)]],
    uint2 gid [[thread_position_in_grid]]) {
  const int ox = static_cast<int>(gid.x);
  const int oy = static_cast<int>(gid.y);
  if (ox >= params.owned_w || oy >= params.owned_h) {
    return;
  }
  const int aligned_x = params.dst_x + ox;
  const int aligned_y = params.dst_y + oy;
  if (aligned_x < params.crop_x || aligned_y < params.crop_y ||
      aligned_x >= params.crop_x + params.crop_w || aligned_y >= params.crop_y + params.crop_h) {
    return;
  }
  const int export_x = params.src_x0 + ox;
  const int export_y = params.src_y0 + oy;
  if (export_x < 0 || export_y < 0 || export_x >= params.export_w || export_y >= params.export_h) {
    return;
  }
  const int natural_y = export_y + params.final_crop;
  const int natural_x = export_x + params.final_crop;
  if (natural_y < 0 || natural_x < 0 || natural_y + 2 >= params.cat_h ||
      natural_x + 2 >= params.cat_w) {
    return;
  }
  const int cat_plane           = params.cat_h * params.cat_w * kPostCin;
  device const float* cat_n     = cat_nhwc + params.batch_index * cat_plane;
  const int           cat_w     = params.cat_w;
  DEMOASICNET_FUSED_POST_OUTPUT(24)
  rgb = ApplyGammaDecode(rgb, params.apply_gamma);
  out_rgba.write(float4(rgb, 1.0f),
                 uint2(static_cast<uint>(aligned_x - params.crop_x),
                       static_cast<uint>(aligned_y - params.crop_y)));
}

kernel void demosaicnet_fused_tail_rgba_w32(
    device const float* cat_nhwc [[buffer(0)]],
    device const float* post_w [[buffer(1)]],
    device const float* post_b [[buffer(2)]],
    device const float* out_w_cio [[buffer(3)]],
    device const float* out_b [[buffer(4)]],
    texture2d<float, access::write> out_rgba [[texture(0)]],
    constant DemosaicNetFusedTailParams& params [[buffer(5)]],
    uint2 gid [[thread_position_in_grid]]) {
  const int ox = static_cast<int>(gid.x);
  const int oy = static_cast<int>(gid.y);
  if (ox >= params.owned_w || oy >= params.owned_h) {
    return;
  }
  const int aligned_x = params.dst_x + ox;
  const int aligned_y = params.dst_y + oy;
  if (aligned_x < params.crop_x || aligned_y < params.crop_y ||
      aligned_x >= params.crop_x + params.crop_w || aligned_y >= params.crop_y + params.crop_h) {
    return;
  }
  const int export_x = params.src_x0 + ox;
  const int export_y = params.src_y0 + oy;
  if (export_x < 0 || export_y < 0 || export_x >= params.export_w || export_y >= params.export_h) {
    return;
  }
  const int natural_y = export_y + params.final_crop;
  const int natural_x = export_x + params.final_crop;
  if (natural_y < 0 || natural_x < 0 || natural_y + 2 >= params.cat_h ||
      natural_x + 2 >= params.cat_w) {
    return;
  }
  const int cat_plane           = params.cat_h * params.cat_w * kPostCin;
  device const float* cat_n     = cat_nhwc + params.batch_index * cat_plane;
  const int           cat_w     = params.cat_w;
  DEMOASICNET_FUSED_POST_OUTPUT(32)
  rgb = ApplyGammaDecode(rgb, params.apply_gamma);
  out_rgba.write(float4(rgb, 1.0f),
                 uint2(static_cast<uint>(aligned_x - params.crop_x),
                       static_cast<uint>(aligned_y - params.crop_y)));
}

kernel void demosaicnet_fused_tail_nhwc_w24(
    device const float* cat_nhwc [[buffer(0)]],
    device const float* post_w [[buffer(1)]],
    device const float* post_b [[buffer(2)]],
    device const float* out_w_cio [[buffer(3)]],
    device const float* out_b [[buffer(4)]],
    device float* rgb_nhwc [[buffer(5)]],
    constant DemosaicNetFusedTailParams& params [[buffer(6)]],
    uint2 gid [[thread_position_in_grid]]) {
  const int export_x = static_cast<int>(gid.x);
  const int export_y = static_cast<int>(gid.y);
  if (export_x >= params.export_w || export_y >= params.export_h) {
    return;
  }
  const int natural_y = export_y + params.final_crop;
  const int natural_x = export_x + params.final_crop;
  if (natural_y < 0 || natural_x < 0 || natural_y + 2 >= params.cat_h ||
      natural_x + 2 >= params.cat_w) {
    return;
  }
  const int cat_plane           = params.cat_h * params.cat_w * kPostCin;
  device const float* cat_n     = cat_nhwc + params.batch_index * cat_plane;
  const int           cat_w     = params.cat_w;
  DEMOASICNET_FUSED_POST_OUTPUT(24)
  rgb = ApplyGammaDecode(rgb, params.apply_gamma);
  const int rgb_plane = params.export_h * params.export_w * kRgbCout;
  const int base =
      params.batch_index * rgb_plane + (export_y * params.export_w + export_x) * kRgbCout;
  rgb_nhwc[base + 0] = rgb.x;
  rgb_nhwc[base + 1] = rgb.y;
  rgb_nhwc[base + 2] = rgb.z;
}

kernel void demosaicnet_fused_tail_nhwc_w32(
    device const float* cat_nhwc [[buffer(0)]],
    device const float* post_w [[buffer(1)]],
    device const float* post_b [[buffer(2)]],
    device const float* out_w_cio [[buffer(3)]],
    device const float* out_b [[buffer(4)]],
    device float* rgb_nhwc [[buffer(5)]],
    constant DemosaicNetFusedTailParams& params [[buffer(6)]],
    uint2 gid [[thread_position_in_grid]]) {
  const int export_x = static_cast<int>(gid.x);
  const int export_y = static_cast<int>(gid.y);
  if (export_x >= params.export_w || export_y >= params.export_h) {
    return;
  }
  const int natural_y = export_y + params.final_crop;
  const int natural_x = export_x + params.final_crop;
  if (natural_y < 0 || natural_x < 0 || natural_y + 2 >= params.cat_h ||
      natural_x + 2 >= params.cat_w) {
    return;
  }
  const int cat_plane           = params.cat_h * params.cat_w * kPostCin;
  device const float* cat_n     = cat_nhwc + params.batch_index * cat_plane;
  const int           cat_w     = params.cat_w;
  DEMOASICNET_FUSED_POST_OUTPUT(32)
  rgb = ApplyGammaDecode(rgb, params.apply_gamma);
  const int rgb_plane = params.export_h * params.export_w * kRgbCout;
  const int base =
      params.batch_index * rgb_plane + (export_y * params.export_w + export_x) * kRgbCout;
  rgb_nhwc[base + 0] = rgb.x;
  rgb_nhwc[base + 1] = rgb.y;
  rgb_nhwc[base + 2] = rgb.z;
}
