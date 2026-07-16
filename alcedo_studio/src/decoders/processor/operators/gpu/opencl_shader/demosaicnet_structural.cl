//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
//
// Fixed DemosaicNet structural / post / boundary kernels (OpenCL).
// Pack, residual unpack+concat, and product RGB extraction for hard-coded modules.

#ifndef DEMOSAICNET_SIGNED_GAMMA_ENCODE_EXP
#define DEMOSAICNET_SIGNED_GAMMA_ENCODE_EXP (1.0f / 2.2f)
#endif
#ifndef DEMOSAICNET_SIGNED_GAMMA_DECODE_EXP
#define DEMOSAICNET_SIGNED_GAMMA_DECODE_EXP 2.2f
#endif

inline int demosaicnet_nhwc4_index(const int n, const int y, const int x, const int cb,
                                   const int height, const int width, const int channel_blocks) {
  return (((n * height + y) * width + x) * channel_blocks + cb);
}

// Contiguous NCHW index: [n, c, h, w]
inline int demosaicnet_nchw_index(const int n, const int c, const int y, const int x,
                                  const int channels, const int height, const int width) {
  return (((n * channels + c) * height + y) * width + x);
}

inline int demosaicnet_reflect101(int coordinate, const int limit) {
  if (limit <= 1) {
    return 0;
  }
  while (coordinate < 0 || coordinate >= limit) {
    coordinate = coordinate < 0 ? -coordinate : 2 * limit - coordinate - 2;
  }
  return coordinate;
}

inline float demosaicnet_signed_gamma_encode(const float v) {
  const float a = fabs(v);
  const float g = pow(a, DEMOSAICNET_SIGNED_GAMMA_ENCODE_EXP);
  return copysign(g, v);
}

inline float demosaicnet_signed_gamma_decode(const float v) {
  const float a = fabs(v);
  const float g = pow(a, DEMOSAICNET_SIGNED_GAMMA_DECODE_EXP);
  return copysign(g, v);
}

inline float demosaicnet_float4_lane(const float4 v, const int lane) {
  if (lane == 0) {
    return v.s0;
  }
  if (lane == 1) {
    return v.s1;
  }
  if (lane == 2) {
    return v.s2;
  }
  return v.s3;
}

// Pack HWC linear float channels into NHWC4 with signed gamma encoding.
// Used at the product Neural boundary (Phase 5/6); module goldens use NCHW pack below.
__kernel void demosaicnet_pack_gamma_nhwc4(__global const float* restrict input,
                                           __global float4* restrict output, const int batch,
                                           const int height, const int width, const int in_channels,
                                           const int out_channel_blocks) {
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int n = get_global_id(2);
  if (x >= width || y >= height || n >= batch) {
    return;
  }

  for (int cb = 0; cb < out_channel_blocks; ++cb) {
    float vals[4];
    for (int lane = 0; lane < 4; ++lane) {
      const int c = cb * 4 + lane;
      if (c < in_channels) {
        const float linear = input[((n * height + y) * width + x) * in_channels + c];
        vals[lane] = demosaicnet_signed_gamma_encode(linear);
      } else {
        vals[lane] = 0.0f;
      }
    }
    output[demosaicnet_nhwc4_index(n, y, x, cb, height, width, out_channel_blocks)] =
        (float4)(vals[0], vals[1], vals[2], vals[3]);
  }
}

// Product tile boundary: phase-aligned HWC3 linear CFA -> NCHW3 student input.
// The host only submits origins divisible by cfa_period. Reflect-101 therefore
// extends each phase without changing the phase of any in-bounds source sample.
__kernel void demosaicnet_pack_reflect_nchw(__global const float* restrict input,
                                            __global float* restrict tile,
                                            const int frame_h, const int frame_w,
                                            const int origin_y, const int origin_x,
                                            const int tile_h, const int tile_w) {
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int c = get_global_id(2);
  if (x >= tile_w || y >= tile_h || c >= 3) {
    return;
  }
  const int sx = demosaicnet_reflect101(origin_x + x, frame_w);
  const int sy = demosaicnet_reflect101(origin_y + y, frame_h);
  const float linear = input[(sy * frame_w + sx) * 3 + c];
  tile[(c * tile_h + y) * tile_w + x] = demosaicnet_signed_gamma_encode(linear);
}

// Product tile boundary: combine reflect-101, signed gamma, and Bayer 2x2
// collapse directly into the first persistent NHWC4 activation. This keeps
// the operation order of demosaicnet_pack_reflect_nchw followed by the Bayer
// one-hot pack while removing the intermediate NCHW tile.
__kernel void demosaicnet_pack_reflect_bayer_nhwc4(
    __global const float* restrict input, __global float4* restrict output,
    const int frame_h, const int frame_w, const int origin_y, const int origin_x,
    const int tile_h, const int tile_w) {
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if (x >= tile_w / 2 || y >= tile_h / 2) {
    return;
  }

  float vals[4];
  for (int py = 0; py < 2; ++py) {
    for (int px = 0; px < 2; ++px) {
      const int sx = demosaicnet_reflect101(origin_x + x * 2 + px, frame_w);
      const int sy = demosaicnet_reflect101(origin_y + y * 2 + py, frame_h);
      float     sum = 0.0f;
      for (int c = 0; c < 3; ++c) {
        const float linear = input[(sy * frame_w + sx) * 3 + c];
        sum += demosaicnet_signed_gamma_encode(linear);
      }
      vals[py * 2 + px] = sum;
    }
  }
  output[demosaicnet_nhwc4_index(0, y, x, 0, tile_h / 2, tile_w / 2, 1)] =
      (float4)(vals[0], vals[1], vals[2], vals[3]);
}

// Product tile boundary: combine reflect-101, signed gamma, and X-Trans
// 2x2 space-to-depth directly into the first persistent NHWC4 activation.
__kernel void demosaicnet_pack_reflect_xtrans_nhwc4(
    __global const float* restrict input, __global float4* restrict output,
    const int frame_h, const int frame_w, const int origin_y, const int origin_x,
    const int tile_h, const int tile_w) {
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if (x >= tile_w / 2 || y >= tile_h / 2) {
    return;
  }

  for (int cb = 0; cb < 3; ++cb) {
    float vals[4];
    for (int lane = 0; lane < 4; ++lane) {
      const int py = lane / 2;
      const int px = lane % 2;
      const int sx = demosaicnet_reflect101(origin_x + x * 2 + px, frame_w);
      const int sy = demosaicnet_reflect101(origin_y + y * 2 + py, frame_h);
      const int c  = cb;
      vals[lane] = demosaicnet_signed_gamma_encode(input[(sy * frame_w + sx) * 3 + c]);
    }
    output[demosaicnet_nhwc4_index(0, y, x, cb, tile_h / 2, tile_w / 2, 3)] =
        (float4)(vals[0], vals[1], vals[2], vals[3]);
  }
}

// Product residual boundary: read the original HWC3 frame through the same
// phase-aware reflect-101 and signed-gamma mapping used by the old NCHW tile,
// while combining it with the residual into the C6 post input. This removes
// the second consumer of the former intermediate NCHW tile.
__kernel void demosaicnet_unpack_reflect_concat_nhwc4(
    __global const float* restrict input, __global const float4* restrict residual,
    __global float4* restrict cat, const int frame_h, const int frame_w,
    const int origin_y, const int origin_x, const int tile_h, const int tile_w,
    const int residual_h, const int residual_w, const int residual_channel_blocks) {
  const int up_w = residual_w * 2;
  const int up_h = residual_h * 2;
  const int x   = get_global_id(0);
  const int y   = get_global_id(1);
  if (x >= up_w || y >= up_h) {
    return;
  }

  const int crop_x = (tile_w - up_w) / 2;
  const int crop_y = (tile_h - up_h) / 2;
  const int tx     = x + crop_x;
  const int ty     = y + crop_y;
  const int sx     = demosaicnet_reflect101(origin_x + tx, frame_w);
  const int sy     = demosaicnet_reflect101(origin_y + ty, frame_h);

  float vals[8];
  const int mosaic_base = (sy * frame_w + sx) * 3;
  vals[0]               = demosaicnet_signed_gamma_encode(input[mosaic_base + 0]);
  vals[1]               = demosaicnet_signed_gamma_encode(input[mosaic_base + 1]);
  vals[2]               = demosaicnet_signed_gamma_encode(input[mosaic_base + 2]);

  const int sub_y = y & 1;
  const int sub_x = x & 1;
  const int ry    = y / 2;
  const int rx    = x / 2;
  for (int g = 0; g < 3; ++g) {
    const int rc     = g * 4 + sub_y * 2 + sub_x;
    const int src_cb = rc / 4;
    const int lane   = rc % 4;
    vals[3 + g] = demosaicnet_float4_lane(
        residual[demosaicnet_nhwc4_index(0, ry, rx, src_cb, residual_h, residual_w,
                                         residual_channel_blocks)],
        lane);
  }
  vals[6] = 0.0f;
  vals[7] = 0.0f;

  cat[demosaicnet_nhwc4_index(0, y, x, 0, up_h, up_w, 2)] =
      (float4)(vals[0], vals[1], vals[2], vals[3]);
  cat[demosaicnet_nhwc4_index(0, y, x, 1, up_h, up_w, 2)] =
      (float4)(vals[4], vals[5], vals[6], vals[7]);
}

// Fixed Bayer collapse-colors pack: NCHW [N,3,H,W] → NHWC4 [N,H/2,W/2,C4].
// out_c = py*2+px sums all three mosaic color planes at that sub-pixel (stride 2).
__kernel void demosaicnet_pack_bayer_nchw_to_nhwc4(__global const float* restrict mosaic,
                                                   __global float4* restrict packed,
                                                   const int batch, const int in_h, const int in_w,
                                                   const int out_h, const int out_w) {
  const int ox = get_global_id(0);
  const int oy = get_global_id(1);
  const int n  = get_global_id(2);
  if (ox >= out_w || oy >= out_h || n >= batch) {
    return;
  }

  float vals[4];
  for (int py = 0; py < 2; ++py) {
    for (int px = 0; px < 2; ++px) {
      const int iy = oy * 2 + py;
      const int ix = ox * 2 + px;
      float sum = 0.0f;
      for (int c = 0; c < 3; ++c) {
        sum += mosaic[demosaicnet_nchw_index(n, c, iy, ix, 3, in_h, in_w)];
      }
      vals[py * 2 + px] = sum;
    }
  }
  packed[demosaicnet_nhwc4_index(n, oy, ox, 0, out_h, out_w, 1)] =
      (float4)(vals[0], vals[1], vals[2], vals[3]);
}

// Fixed X-Trans space-to-depth pack: NCHW [N,3,H,W] → NHWC4 [N,H/2,W/2,C12] (3 blocks).
// out_c = c*4 + py*2 + px.
__kernel void demosaicnet_pack_xtrans_nchw_to_nhwc4(__global const float* restrict mosaic,
                                                    __global float4* restrict packed,
                                                    const int batch, const int in_h, const int in_w,
                                                    const int out_h, const int out_w) {
  const int ox = get_global_id(0);
  const int oy = get_global_id(1);
  const int n  = get_global_id(2);
  if (ox >= out_w || oy >= out_h || n >= batch) {
    return;
  }

  const int out_channel_blocks = 3;
  for (int cb = 0; cb < out_channel_blocks; ++cb) {
    float vals[4];
    for (int lane = 0; lane < 4; ++lane) {
      const int out_c = cb * 4 + lane;
      const int c     = out_c / 4;
      const int sub   = out_c % 4;
      const int py    = sub / 2;
      const int px    = sub % 2;
      const int iy    = oy * 2 + py;
      const int ix    = ox * 2 + px;
      vals[lane] = mosaic[demosaicnet_nchw_index(n, c, iy, ix, 3, in_h, in_w)];
    }
    packed[demosaicnet_nhwc4_index(n, oy, ox, cb, out_h, out_w, out_channel_blocks)] =
        (float4)(vals[0], vals[1], vals[2], vals[3]);
  }
}

// Residual path: center-crop residual features and add to skip connection (NHWC4).
__kernel void demosaicnet_residual_add_crop(__global const float4* restrict residual,
                                            __global const float4* restrict skip,
                                            __global float4* restrict output, const int batch,
                                            const int residual_h, const int residual_w,
                                            const int skip_h, const int skip_w, const int out_h,
                                            const int out_w, const int channel_blocks,
                                            const int residual_top, const int residual_left,
                                            const int skip_top, const int skip_left) {
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int plane = get_global_id(2);
  if (x >= out_w || y >= out_h) {
    return;
  }
  const int cb = plane % channel_blocks;
  const int n = plane / channel_blocks;
  if (n >= batch) {
    return;
  }

  const int rx = x + residual_left;
  const int ry = y + residual_top;
  const int sx = x + skip_left;
  const int sy = y + skip_top;
  const float4 r =
      residual[demosaicnet_nhwc4_index(n, ry, rx, cb, residual_h, residual_w, channel_blocks)];
  const float4 s = skip[demosaicnet_nhwc4_index(n, sy, sx, cb, skip_h, skip_w, channel_blocks)];
  output[demosaicnet_nhwc4_index(n, y, x, cb, out_h, out_w, channel_blocks)] = r + s;
}

// Grouped unpack residual C12 → RGB sub-pixels, center-crop mosaic, concat → C6 NHWC4
// (2 physical float4 blocks, logical channels = 6).
// residual: NHWC4 [N, residual_h, residual_w, C12]
// mosaic:   NCHW  [N, 3, input_h, input_w]
// cat:      NHWC4 [N, residual_h*2, residual_w*2, C6 logical / 2 blocks]
__kernel void demosaicnet_unpack_crop_concat_nhwc4(__global const float* restrict mosaic,
                                                   __global const float4* restrict residual,
                                                   __global float4* restrict cat, const int batch,
                                                   const int input_h, const int input_w,
                                                   const int residual_h, const int residual_w,
                                                   const int residual_channel_blocks) {
  const int up_w = residual_w * 2;
  const int up_h = residual_h * 2;
  const int x    = get_global_id(0);
  const int y    = get_global_id(1);
  const int n    = get_global_id(2);
  if (x >= up_w || y >= up_h || n >= batch) {
    return;
  }

  const int crop_x = (input_w - up_w) / 2;
  const int crop_y = (input_h - up_h) / 2;
  const int sx     = x + crop_x;
  const int sy     = y + crop_y;

  float vals[8];
  // Mosaic RGB (channels 0..2)
  for (int c = 0; c < 3; ++c) {
    vals[c] = mosaic[demosaicnet_nchw_index(n, c, sy, sx, 3, input_h, input_w)];
  }
  // Unpacked residual RGB (channels 3..5): residual channel g*4 + (y&1)*2 + (x&1)
  const int sub_y = y & 1;
  const int sub_x = x & 1;
  const int ry    = y / 2;
  const int rx    = x / 2;
  for (int g = 0; g < 3; ++g) {
    const int rc       = g * 4 + sub_y * 2 + sub_x;
    const int src_cb   = rc / 4;
    const int src_lane = rc % 4;
    const float4 rv =
        residual[demosaicnet_nhwc4_index(n, ry, rx, src_cb, residual_h, residual_w,
                                         residual_channel_blocks)];
    vals[3 + g] = demosaicnet_float4_lane(rv, src_lane);
  }
  vals[6] = 0.0f;
  vals[7] = 0.0f;

  const int out_channel_blocks = 2;
  cat[demosaicnet_nhwc4_index(n, y, x, 0, up_h, up_w, out_channel_blocks)] =
      (float4)(vals[0], vals[1], vals[2], vals[3]);
  cat[demosaicnet_nhwc4_index(n, y, x, 1, up_h, up_w, out_channel_blocks)] =
      (float4)(vals[4], vals[5], vals[6], vals[7]);
}

// Form post-network C6 logical input in an 8-lane (2 block) NHWC4 allocation.
// Copies first `logical_channels` lanes and zero-fills the rest.
__kernel void demosaicnet_form_post_input_c6(__global const float4* restrict input,
                                             __global float4* restrict output, const int batch,
                                             const int height, const int width,
                                             const int in_channel_blocks,
                                             const int out_channel_blocks,
                                             const int logical_channels) {
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int n = get_global_id(2);
  if (x >= width || y >= height || n >= batch) {
    return;
  }

  for (int cb = 0; cb < out_channel_blocks; ++cb) {
    float vals[4];
    for (int lane = 0; lane < 4; ++lane) {
      const int c = cb * 4 + lane;
      if (c < logical_channels) {
        const int src_cb = c / 4;
        const int src_lane = c % 4;
        const float4 v =
            input[demosaicnet_nhwc4_index(n, y, x, src_cb, height, width, in_channel_blocks)];
        vals[lane] = demosaicnet_float4_lane(v, src_lane);
      } else {
        vals[lane] = 0.0f;
      }
    }
    output[demosaicnet_nhwc4_index(n, y, x, cb, height, width, out_channel_blocks)] =
        (float4)(vals[0], vals[1], vals[2], vals[3]);
  }
}

// Final RGB extraction from NHWC4 C3 (1 block): optional gamma decode, optional clamp,
// optional center-crop into contiguous HWC RGB.
__kernel void demosaicnet_output_rgb_hwc(__global const float4* restrict input,
                                         __global float* restrict output, const int batch,
                                         const int in_h, const int in_w, const int out_h,
                                         const int out_w, const int crop_top, const int crop_left,
                                         const int in_channel_blocks, const int apply_gamma,
                                         const int clamp_enabled) {
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int n = get_global_id(2);
  if (x >= out_w || y >= out_h || n >= batch) {
    return;
  }

  const int sx = x + crop_left;
  const int sy = y + crop_top;
  const float4 v0 = input[demosaicnet_nhwc4_index(n, sy, sx, 0, in_h, in_w, in_channel_blocks)];
  float r = v0.s0;
  float g = v0.s1;
  float b = v0.s2;
  if (apply_gamma != 0) {
    r = demosaicnet_signed_gamma_decode(r);
    g = demosaicnet_signed_gamma_decode(g);
    b = demosaicnet_signed_gamma_decode(b);
  }
  if (clamp_enabled != 0) {
    r = clamp(r, 0.0f, 1.0f);
    g = clamp(g, 0.0f, 1.0f);
    b = clamp(b, 0.0f, 1.0f);
  }
  const int out_index = ((n * out_h + y) * out_w + x) * 3;
  output[out_index + 0] = r;
  output[out_index + 1] = g;
  output[out_index + 2] = b;
}

// Compatibility alias: gamma-decode full field without center crop (legacy name).
__kernel void demosaicnet_output_gamma_hwc(__global const float4* restrict input,
                                           __global float* restrict output, const int batch,
                                           const int height, const int width,
                                           const int in_channel_blocks, const int clamp_enabled) {
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int n = get_global_id(2);
  if (x >= width || y >= height || n >= batch) {
    return;
  }

  const float4 v0 = input[demosaicnet_nhwc4_index(n, y, x, 0, height, width, in_channel_blocks)];
  float r = demosaicnet_signed_gamma_decode(v0.s0);
  float g = demosaicnet_signed_gamma_decode(v0.s1);
  float b = demosaicnet_signed_gamma_decode(v0.s2);
  if (clamp_enabled != 0) {
    r = clamp(r, 0.0f, 1.0f);
    g = clamp(g, 0.0f, 1.0f);
    b = clamp(b, 0.0f, 1.0f);
  }
  const int out_index = ((n * height + y) * width + x) * 3;
  output[out_index + 0] = r;
  output[out_index + 1] = g;
  output[out_index + 2] = b;
}

// Tile assembly: copy an owned RGB HWC tile into a destination canvas at (dst_x, dst_y).
__kernel void demosaicnet_assemble_rgb_tile(__global const float* restrict tile,
                                            __global float* restrict canvas, const int tile_w,
                                            const int tile_h, const int canvas_w,
                                            const int canvas_h, const int dst_x, const int dst_y,
                                            const int owned_w, const int owned_h, const int src_x0,
                                            const int src_y0) {
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if (x >= owned_w || y >= owned_h) {
    return;
  }
  const int dst_xx = dst_x + x;
  const int dst_yy = dst_y + y;
  if (dst_xx < 0 || dst_yy < 0 || dst_xx >= canvas_w || dst_yy >= canvas_h) {
    return;
  }
  const int src_x = src_x0 + x;
  const int src_y = src_y0 + y;
  if (src_x < 0 || src_y < 0 || src_x >= tile_w || src_y >= tile_h) {
    return;
  }
  const int src_index = (src_y * tile_w + src_x) * 3;
  const int dst_index = (dst_yy * canvas_w + dst_xx) * 3;
  canvas[dst_index + 0] = tile[src_index + 0];
  canvas[dst_index + 1] = tile[src_index + 1];
  canvas[dst_index + 2] = tile[src_index + 2];
}

// Product boundary: clamp linear CFA samples to [0,1] when highlight reconstruction is off.
__kernel void demosaicnet_clamp01(__global float* restrict data, const int count) {
  const int i = get_global_id(0);
  if (i >= count) {
    return;
  }
  const float v = data[i];
  data[i]       = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

// Product boundary: monochrome linear CFA ROI -> sparse HWC3 mosaic at the training origin.
// Reads a crop (crop_x, crop_y, width, height) from a full-frame mono plane of size
// (src_width, src_height) so phase crop does not require a materialised intermediate.
// rgb_fc is a row-major period×period table of RGB color indices (0/1/2) in the
// aligned (training-origin) lattice.
__kernel void demosaicnet_pack_cfa_mono_to_hwc3(__global const float* restrict mono,
                                                __global float* restrict hwc3,
                                                const int src_width, const int src_height,
                                                const int crop_x, const int crop_y,
                                                const int width, const int height,
                                                const int period,
                                                __global const int* restrict rgb_fc) {
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if (x >= width || y >= height || period <= 0) {
    return;
  }
  const int sx = crop_x + x;
  const int sy = crop_y + y;
  if (sx < 0 || sy < 0 || sx >= src_width || sy >= src_height) {
    return;
  }
  const int color = rgb_fc[(y % period) * period + (x % period)];
  const float v   = mono[sy * src_width + sx];
  const int base  = (y * width + x) * 3;
  hwc3[base + 0]  = (color == 0) ? v : 0.0f;
  hwc3[base + 1]  = (color == 1) ? v : 0.0f;
  hwc3[base + 2]  = (color == 2) ? v : 0.0f;
}

// Product continuation: HWC3 camera RGB -> HWC4 RGBA with alpha = 1.
__kernel void demosaicnet_rgb3_to_rgba4(__global const float* restrict rgb,
                                        __global float* restrict rgba, const int width,
                                        const int height) {
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if (x >= width || y >= height) {
    return;
  }
  const int src = (y * width + x) * 3;
  const int dst = (y * width + x) * 4;
  rgba[dst + 0] = rgb[src + 0];
  rgba[dst + 1] = rgb[src + 1];
  rgba[dst + 2] = rgb[src + 2];
  rgba[dst + 3] = 1.0f;
}
