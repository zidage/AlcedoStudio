//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
//
// Fixed DemosaicNet structural / post / boundary kernels (OpenCL).
// Phase 2 registers and compiles these entry points. Full residual/post/output
// semantics are completed with the fixed modules in later phases; kernels here
// must stay compilable and use the stable names consumed by the program library.

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

// Final RGB extraction: gamma decode, optional clamp, write HWC RGB.
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
