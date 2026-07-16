//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
//
// Metal DemosaicNet tile boundary kernels.
// - Input:  R32F CFA texture → fixed NHWC FP32 tile buffer (phase + reflect-101 + sparse RGB + gamma).
// - Output: NHWC FP32 tile buffer → crop-sized RGBA texture (ownership intersect + gamma decode).

#include <metal_stdlib>

using namespace metal;

// Must match host-side DemosaicNetTileInputParams / DemosaicNetTileOutputParams.
struct DemosaicNetTileInputParams {
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

  const int base = (y * params.tile_w + x) * 3;
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

  const int   base = (src_y * params.tile_w + src_x) * 3;
  const float r    = PowSigned(tile_nhwc[base + 0], kGammaDecode);
  const float g    = PowSigned(tile_nhwc[base + 1], kGammaDecode);
  const float b    = PowSigned(tile_nhwc[base + 2], kGammaDecode);

  const int out_x = aligned_x - params.crop_x;
  const int out_y = aligned_y - params.crop_y;
  out_rgba.write(float4(r, g, b, 1.0f),
                 uint2(static_cast<uint>(out_x), static_cast<uint>(out_y)));
}
