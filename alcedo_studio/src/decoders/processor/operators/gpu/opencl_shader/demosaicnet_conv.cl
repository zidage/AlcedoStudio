//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
//
// Project-owned fixed-shape direct convolution for OpenCL DemosaicNet.
// The mapping follows the CUDA persistent-NHWC kernel: one work-group owns a
// spatial tile and all output channel blocks, while input channels are reduced
// in small strips through local memory.

#ifndef IN_CHANNEL_BLOCKS
#define IN_CHANNEL_BLOCKS 8
#endif
#ifndef OUT_CHANNEL_BLOCKS
#define OUT_CHANNEL_BLOCKS 8
#endif

#define DEMOSAICNET_CONV_TILE_W 16
#define DEMOSAICNET_CONV_TILE_H 8
#define DEMOSAICNET_CONV_INPUT_W (DEMOSAICNET_CONV_TILE_W + 2)
#define DEMOSAICNET_CONV_INPUT_H (DEMOSAICNET_CONV_TILE_H + 2)
#define DEMOSAICNET_CONV_CIN_TILE_BLOCKS 2

inline int demosaicnet_conv_nhwc4_index(const int n, const int y, const int x, const int cb,
                                        const int height, const int width,
                                        const int channel_blocks) {
  return (((n * height + y) * width + x) * channel_blocks + cb);
}

inline int demosaicnet_conv_weight_index(const int out_block, const int ky, const int kx,
                                         const int in_block, const int out_lane,
                                         const int in_lane, const int in_channel_blocks) {
  return (((((out_block * 3 + ky) * 3 + kx) * in_channel_blocks + in_block) * 4 + out_lane) *
              4 +
          in_lane);
}

inline float4 demosaicnet_conv_mask_input(float4 value, const int in_block,
                                          const int logical_channels) {
  const int base = in_block * 4;
  if (base + 0 >= logical_channels) value.s0 = 0.0f;
  if (base + 1 >= logical_channels) value.s1 = 0.0f;
  if (base + 2 >= logical_channels) value.s2 = 0.0f;
  if (base + 3 >= logical_channels) value.s3 = 0.0f;
  return value;
}

// Fixed 3x3 stride-1 direct convolution. One work-group owns a 16x8 spatial
// tile and all compile-time output blocks. Input channels are staged in two-C4
// strips, matching the CUDA kernel's Cin-tile strategy.
__kernel __attribute__((reqd_work_group_size(16, 8, 1))) void demosaicnet_conv3x3_nhwc4(
    __global const float4* restrict input, __global const float* restrict weights,
    __global const float* restrict bias, __global float4* restrict output, const int batch,
    const int in_h, const int in_w, const int out_h, const int out_w, const int pad_h,
    const int pad_w, const int in_channel_blocks, const int out_channel_blocks,
    const int in_logical_channels, const int out_logical_channels) {
  __local float4 input_tile[DEMOSAICNET_CONV_INPUT_H * DEMOSAICNET_CONV_INPUT_W *
                            DEMOSAICNET_CONV_CIN_TILE_BLOCKS];
  __local float4 weight_tile[DEMOSAICNET_CONV_CIN_TILE_BLOCKS * 9 * OUT_CHANNEL_BLOCKS * 4];

  const int local_x = get_local_id(0);
  const int local_y = get_local_id(1);
  const int local_linear = local_y * DEMOSAICNET_CONV_TILE_W + local_x;
  const int group_x = get_group_id(0);
  const int group_y = get_group_id(1);
  const int ox = get_global_id(0);
  const int oy = get_global_id(1);
  const int n = get_group_id(2);

  float4 acc[OUT_CHANNEL_BLOCKS];
#pragma unroll
  for (int out_cb = 0; out_cb < OUT_CHANNEL_BLOCKS; ++out_cb) {
    acc[out_cb] = (float4)(0.0f);
  }

  const int input_tile_elements =
      DEMOSAICNET_CONV_INPUT_H * DEMOSAICNET_CONV_INPUT_W * DEMOSAICNET_CONV_CIN_TILE_BLOCKS;
  for (int ci0 = 0; ci0 < in_channel_blocks; ci0 += DEMOSAICNET_CONV_CIN_TILE_BLOCKS) {
    for (int i = local_linear; i < input_tile_elements;
         i += DEMOSAICNET_CONV_TILE_W * DEMOSAICNET_CONV_TILE_H) {
      const int local_in_cb = i % DEMOSAICNET_CONV_CIN_TILE_BLOCKS;
      const int spatial = i / DEMOSAICNET_CONV_CIN_TILE_BLOCKS;
      const int tx = spatial % DEMOSAICNET_CONV_INPUT_W;
      const int ty = spatial / DEMOSAICNET_CONV_INPUT_W;
      const int global_in_cb = ci0 + local_in_cb;
      const int ix = group_x * DEMOSAICNET_CONV_TILE_W + tx - pad_w;
      const int iy = group_y * DEMOSAICNET_CONV_TILE_H + ty - pad_h;
      float4 value = (float4)(0.0f);
      if (n < batch && global_in_cb < in_channel_blocks && ix >= 0 && ix < in_w && iy >= 0 &&
          iy < in_h) {
        value = input[demosaicnet_conv_nhwc4_index(n, iy, ix, global_in_cb, in_h, in_w,
                                                   in_channel_blocks)];
      }
      input_tile[i] = value;
    }

    const int weight_tile_elements =
        DEMOSAICNET_CONV_CIN_TILE_BLOCKS * 9 * OUT_CHANNEL_BLOCKS * 4;
    for (int i = local_linear; i < weight_tile_elements;
         i += DEMOSAICNET_CONV_TILE_W * DEMOSAICNET_CONV_TILE_H) {
      const int out_lane = i % 4;
      int       t = i / 4;
      const int out_cb = t % OUT_CHANNEL_BLOCKS;
      t /= OUT_CHANNEL_BLOCKS;
      const int kernel_cell = t % 9;
      const int local_in_cb = t / 9;
      const int global_in_cb = ci0 + local_in_cb;
      const int ky = kernel_cell / 3;
      const int kx = kernel_cell % 3;
      float4    value = (float4)(0.0f);
      if (n < batch && global_in_cb < in_channel_blocks && out_cb < out_channel_blocks) {
        value = vload4(0, weights + demosaicnet_conv_weight_index(
                                   out_cb, ky, kx, global_in_cb, out_lane, 0,
                                   in_channel_blocks));
      }
      const int local_weight_index =
          ((local_in_cb * 9 + kernel_cell) * OUT_CHANNEL_BLOCKS + out_cb) * 4 + out_lane;
      weight_tile[local_weight_index] = value;
    }

    barrier(CLK_LOCAL_MEM_FENCE);

    if (ox < out_w && oy < out_h && n < batch) {
#pragma unroll
      for (int ky = 0; ky < 3; ++ky) {
#pragma unroll
        for (int kx = 0; kx < 3; ++kx) {
#pragma unroll
          for (int local_in_cb = 0; local_in_cb < DEMOSAICNET_CONV_CIN_TILE_BLOCKS;
               ++local_in_cb) {
            const int global_in_cb = ci0 + local_in_cb;
            if (global_in_cb >= in_channel_blocks) continue;
            const int input_index =
                (((local_y + ky) * DEMOSAICNET_CONV_INPUT_W + local_x + kx) *
                     DEMOSAICNET_CONV_CIN_TILE_BLOCKS) +
                local_in_cb;
            const float4 in_value = demosaicnet_conv_mask_input(
                input_tile[input_index], global_in_cb, in_logical_channels);

#pragma unroll
            for (int out_cb = 0; out_cb < OUT_CHANNEL_BLOCKS; ++out_cb) {
              if (out_cb >= out_channel_blocks) continue;
              const int weight_base =
                  ((local_in_cb * 9 + ky * 3 + kx) * OUT_CHANNEL_BLOCKS + out_cb) * 4;
              acc[out_cb].s0 += dot(in_value, weight_tile[weight_base + 0]);
              acc[out_cb].s1 += dot(in_value, weight_tile[weight_base + 1]);
              acc[out_cb].s2 += dot(in_value, weight_tile[weight_base + 2]);
              acc[out_cb].s3 += dot(in_value, weight_tile[weight_base + 3]);
            }
          }
        }
      }
    }

    barrier(CLK_LOCAL_MEM_FENCE);
  }

  if (ox >= out_w || oy >= out_h || n >= batch) return;

#pragma unroll
  for (int out_cb = 0; out_cb < OUT_CHANNEL_BLOCKS; ++out_cb) {
    if (out_cb >= out_channel_blocks) continue;
    const int output_base = out_cb * 4;
    float4    value = acc[out_cb];
    if (bias != 0) {
      if (output_base + 0 < out_logical_channels) value.s0 += bias[output_base + 0];
      if (output_base + 1 < out_logical_channels) value.s1 += bias[output_base + 1];
      if (output_base + 2 < out_logical_channels) value.s2 += bias[output_base + 2];
      if (output_base + 3 < out_logical_channels) value.s3 += bias[output_base + 3];
    }
    value = fmax(value, (float4)(0.0f));
    if (output_base + 0 >= out_logical_channels) value.s0 = 0.0f;
    if (output_base + 1 >= out_logical_channels) value.s1 = 0.0f;
    if (output_base + 2 >= out_logical_channels) value.s2 = 0.0f;
    if (output_base + 3 >= out_logical_channels) value.s3 = 0.0f;
    output[demosaicnet_conv_nhwc4_index(n, oy, ox, out_cb, out_h, out_w,
                                        out_channel_blocks)] = value;
  }
}

// C6-padded post layer specialization. The post input is always exactly two
// NHWC4 channel blocks (logical C6), so the fixed loop removes the generic
// runtime input-channel branch while preserving the same accumulation order.
__kernel __attribute__((reqd_work_group_size(16, 8, 1))) void demosaicnet_conv3x3_c6_nhwc4(
    __global const float4* restrict input, __global const float* restrict weights,
    __global const float* restrict bias, __global float4* restrict output, const int batch,
    const int in_h, const int in_w, const int out_h, const int out_w, const int pad_h,
    const int pad_w, const int in_channel_blocks, const int out_channel_blocks,
    const int in_logical_channels, const int out_logical_channels) {
  (void)in_channel_blocks;
  __local float4 input_tile[DEMOSAICNET_CONV_INPUT_H * DEMOSAICNET_CONV_INPUT_W * 2];
  __local float4 weight_tile[2 * 9 * OUT_CHANNEL_BLOCKS * 4];

  const int local_x      = get_local_id(0);
  const int local_y      = get_local_id(1);
  const int local_linear = local_y * DEMOSAICNET_CONV_TILE_W + local_x;
  const int group_x      = get_group_id(0);
  const int group_y      = get_group_id(1);
  const int ox           = get_global_id(0);
  const int oy           = get_global_id(1);
  const int n            = get_group_id(2);

  float4 acc[OUT_CHANNEL_BLOCKS];
#pragma unroll
  for (int out_cb = 0; out_cb < OUT_CHANNEL_BLOCKS; ++out_cb) {
    acc[out_cb] = (float4)(0.0f);
  }

  const int input_tile_elements = DEMOSAICNET_CONV_INPUT_H * DEMOSAICNET_CONV_INPUT_W * 2;
  for (int i = local_linear; i < input_tile_elements;
       i += DEMOSAICNET_CONV_TILE_W * DEMOSAICNET_CONV_TILE_H) {
    const int local_in_cb = i % 2;
    const int spatial      = i / 2;
    const int tx           = spatial % DEMOSAICNET_CONV_INPUT_W;
    const int ty           = spatial / DEMOSAICNET_CONV_INPUT_W;
    const int ix           = group_x * DEMOSAICNET_CONV_TILE_W + tx - pad_w;
    const int iy           = group_y * DEMOSAICNET_CONV_TILE_H + ty - pad_h;
    float4    value        = (float4)(0.0f);
    if (n < batch && ix >= 0 && ix < in_w && iy >= 0 && iy < in_h) {
      value = input[demosaicnet_conv_nhwc4_index(n, iy, ix, local_in_cb, in_h, in_w, 2)];
    }
    input_tile[i] = value;
  }

  const int weight_tile_elements = 2 * 9 * OUT_CHANNEL_BLOCKS * 4;
  for (int i = local_linear; i < weight_tile_elements;
       i += DEMOSAICNET_CONV_TILE_W * DEMOSAICNET_CONV_TILE_H) {
    const int out_lane = i % 4;
    int       t        = i / 4;
    const int out_cb   = t % OUT_CHANNEL_BLOCKS;
    t /= OUT_CHANNEL_BLOCKS;
    const int kernel_cell = t % 9;
    const int local_in_cb = t / 9;
    const int ky           = kernel_cell / 3;
    const int kx           = kernel_cell % 3;
    const float4 value = vload4(
        0, weights + demosaicnet_conv_weight_index(out_cb, ky, kx, local_in_cb, out_lane, 0, 2));
    const int local_weight_index =
        ((local_in_cb * 9 + kernel_cell) * OUT_CHANNEL_BLOCKS + out_cb) * 4 + out_lane;
    weight_tile[local_weight_index] = value;
  }

  barrier(CLK_LOCAL_MEM_FENCE);

  if (ox < out_w && oy < out_h && n < batch) {
#pragma unroll
    for (int ky = 0; ky < 3; ++ky) {
#pragma unroll
      for (int kx = 0; kx < 3; ++kx) {
#pragma unroll
        for (int local_in_cb = 0; local_in_cb < 2; ++local_in_cb) {
          const int input_index =
              (((local_y + ky) * DEMOSAICNET_CONV_INPUT_W + local_x + kx) * 2) + local_in_cb;
          const float4 in_value =
              demosaicnet_conv_mask_input(input_tile[input_index], local_in_cb, 6);
#pragma unroll
          for (int out_cb = 0; out_cb < OUT_CHANNEL_BLOCKS; ++out_cb) {
            if (out_cb >= out_channel_blocks) continue;
            const int weight_base =
                ((local_in_cb * 9 + ky * 3 + kx) * OUT_CHANNEL_BLOCKS + out_cb) * 4;
            acc[out_cb].s0 += dot(in_value, weight_tile[weight_base + 0]);
            acc[out_cb].s1 += dot(in_value, weight_tile[weight_base + 1]);
            acc[out_cb].s2 += dot(in_value, weight_tile[weight_base + 2]);
            acc[out_cb].s3 += dot(in_value, weight_tile[weight_base + 3]);
          }
        }
      }
    }
  }

  barrier(CLK_LOCAL_MEM_FENCE);
  if (ox >= out_w || oy >= out_h || n >= batch) return;

#pragma unroll
  for (int out_cb = 0; out_cb < OUT_CHANNEL_BLOCKS; ++out_cb) {
    if (out_cb >= out_channel_blocks) continue;
    const int output_base = out_cb * 4;
    float4    value       = acc[out_cb];
    if (bias != 0) {
      if (output_base + 0 < out_logical_channels) value.s0 += bias[output_base + 0];
      if (output_base + 1 < out_logical_channels) value.s1 += bias[output_base + 1];
      if (output_base + 2 < out_logical_channels) value.s2 += bias[output_base + 2];
      if (output_base + 3 < out_logical_channels) value.s3 += bias[output_base + 3];
    }
    value = fmax(value, (float4)(0.0f));
    if (output_base + 0 >= out_logical_channels) value.s0 = 0.0f;
    if (output_base + 1 >= out_logical_channels) value.s1 = 0.0f;
    if (output_base + 2 >= out_logical_channels) value.s2 = 0.0f;
    if (output_base + 3 >= out_logical_channels) value.s3 = 0.0f;
    output[demosaicnet_conv_nhwc4_index(n, oy, ox, out_cb, out_h, out_w,
                                        out_channel_blocks)] = value;
  }
}

// One work-item owns one spatial position and all output C4 blocks. This is
// the OpenCL equivalent of CUDA's exact-small-Cout 1x1 kernels and avoids
// reloading the same input vector for every output block.
__kernel void demosaicnet_conv1x1_nhwc4(
    __global const float4* restrict input, __global const float* restrict weights,
    __global const float* restrict bias, __global float4* restrict output, const int batch,
    const int height, const int width, const int in_channel_blocks, const int out_channel_blocks,
    const int in_logical_channels, const int out_logical_channels, const int apply_relu) {
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int n = get_global_id(2);
  if (x >= width || y >= height || n >= batch) return;

  float4 acc[OUT_CHANNEL_BLOCKS];
#pragma unroll
  for (int out_cb = 0; out_cb < OUT_CHANNEL_BLOCKS; ++out_cb) {
    acc[out_cb] = (float4)(0.0f);
  }

  for (int in_cb = 0; in_cb < in_channel_blocks; ++in_cb) {
    float4 in_value = input[demosaicnet_conv_nhwc4_index(n, y, x, in_cb, height, width,
                                                         in_channel_blocks)];
    in_value = demosaicnet_conv_mask_input(in_value, in_cb, in_logical_channels);
#pragma unroll
    for (int out_cb = 0; out_cb < OUT_CHANNEL_BLOCKS; ++out_cb) {
      if (out_cb >= out_channel_blocks) continue;
      const int weight_base = (out_cb * in_channel_blocks + in_cb) * 16;
      acc[out_cb].s0 += dot(in_value, vload4(0, weights + weight_base + 0));
      acc[out_cb].s1 += dot(in_value, vload4(0, weights + weight_base + 4));
      acc[out_cb].s2 += dot(in_value, vload4(0, weights + weight_base + 8));
      acc[out_cb].s3 += dot(in_value, vload4(0, weights + weight_base + 12));
    }
  }

#pragma unroll
  for (int out_cb = 0; out_cb < OUT_CHANNEL_BLOCKS; ++out_cb) {
    if (out_cb >= out_channel_blocks) continue;
    const int output_base = out_cb * 4;
    float4    value = acc[out_cb];
    if (bias != 0) {
      if (output_base + 0 < out_logical_channels) value.s0 += bias[output_base + 0];
      if (output_base + 1 < out_logical_channels) value.s1 += bias[output_base + 1];
      if (output_base + 2 < out_logical_channels) value.s2 += bias[output_base + 2];
      if (output_base + 3 < out_logical_channels) value.s3 += bias[output_base + 3];
    }
    if (apply_relu != 0) value = fmax(value, (float4)(0.0f));
    if (output_base + 0 >= out_logical_channels) value.s0 = 0.0f;
    if (output_base + 1 >= out_logical_channels) value.s1 = 0.0f;
    if (output_base + 2 >= out_logical_channels) value.s2 = 0.0f;
    if (output_base + 3 >= out_logical_channels) value.s3 = 0.0f;
    output[demosaicnet_conv_nhwc4_index(n, y, x, out_cb, height, width,
                                        out_channel_blocks)] = value;
  }
}

// Output-tail specialization. The final layer always produces logical C3 in
// one physical block; compile-time input/output sizes avoid accumulating and
// branching over the unused output blocks of the trunk-oriented kernel.
__kernel void demosaicnet_conv1x1_out3_nhwc4(
    __global const float4* restrict input, __global const float* restrict weights,
    __global const float* restrict bias, __global float4* restrict output, const int batch,
    const int height, const int width, const int in_channel_blocks, const int out_channel_blocks,
    const int in_logical_channels, const int out_logical_channels, const int apply_relu) {
  (void)in_channel_blocks;
  (void)out_channel_blocks;
  (void)in_logical_channels;
  (void)out_logical_channels;
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int n = get_global_id(2);
  if (x >= width || y >= height || n >= batch) return;

  float4 acc = (float4)(0.0f);
  for (int in_cb = 0; in_cb < IN_CHANNEL_BLOCKS; ++in_cb) {
    float4 in_value = input[demosaicnet_conv_nhwc4_index(n, y, x, in_cb, height, width,
                                                         IN_CHANNEL_BLOCKS)];
    in_value = demosaicnet_conv_mask_input(in_value, in_cb, IN_LOGICAL_CHANNELS);
    const int weight_base = in_cb * 16;
    acc.s0 += dot(in_value, vload4(0, weights + weight_base + 0));
    acc.s1 += dot(in_value, vload4(0, weights + weight_base + 4));
    acc.s2 += dot(in_value, vload4(0, weights + weight_base + 8));
    acc.s3 += dot(in_value, vload4(0, weights + weight_base + 12));
  }

  if (bias != 0) {
    acc.s0 += bias[0];
    acc.s1 += bias[1];
    acc.s2 += bias[2];
  }
  if (apply_relu != 0) acc = fmax(acc, (float4)(0.0f));
  acc.s3 = 0.0f;
  output[demosaicnet_conv_nhwc4_index(n, y, x, 0, height, width, 1)] = acc;
}
