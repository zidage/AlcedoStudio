//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only

// SM8.3: OpenCL Feather and Expand of AI Mask coverage (SM8 section 5.1), the passes of
// AiMaskEdgeDispatch. R8 images are CL_UNORM_INT8: a read gives u / 255, so each read converts
// back to the integer u, and each R8 write stores v / 255 for an integer v. Every pass clamps
// coordinates to the output (replicated border).

__constant sampler_t kAiMaskNearestClamp =
    CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST;

/// Layout of AiMaskEdgeGrid in opencl_ai_mask_coverage.cpp. The input is placed at
/// (source_x, source_y) with source_width x source_height texels; texels outside read 0.
typedef struct {
  int width;
  int height;
  int source_x;
  int source_y;
  int source_width;
  int source_height;
} AiMaskEdgeGrid;

/// Layout of AiMaskFeatherWeights in opencl_ai_mask_coverage.cpp (kAiMaskFeatherMaximumRadius).
typedef struct {
  int   radius;
  float values[201];
} AiMaskFeatherWeights;

inline uint ai_mask_read_placed(__read_only image2d_t input, AiMaskEdgeGrid grid, int x, int y) {
  const int source_x = clamp(x, 0, grid.width - 1) - grid.source_x;
  const int source_y = clamp(y, 0, grid.height - 1) - grid.source_y;
  if (source_x < 0 || source_y < 0 || source_x >= grid.source_width ||
      source_y >= grid.source_height) {
    return 0u;
  }
  const float value = read_imagef(input, kAiMaskNearestClamp, (int2)(source_x, source_y)).x;
  return convert_uint_sat_rte(value * 255.0f);
}

inline void ai_mask_write_r8(__write_only image2d_t output, int x, int y, uint value) {
  write_imagef(output, (int2)(x, y), (float4)((float)value / 255.0f, 0.0f, 0.0f, 1.0f));
}

/// One 3 x 3 dilate (dilate != 0) or erode step with the cross (square == 0) or square kernel.
__kernel void ai_mask_morphology_r8(__read_only image2d_t input, __write_only image2d_t output,
                                    AiMaskEdgeGrid grid, int dilate, int square) {
  const int x = (int)get_global_id(0);
  const int y = (int)get_global_id(1);
  if (x >= grid.width || y >= grid.height) {
    return;
  }
  uint value = ai_mask_read_placed(input, grid, x, y);
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      if ((dx == 0 && dy == 0) || (square == 0 && dx != 0 && dy != 0)) {
        continue;
      }
      const uint neighbor = ai_mask_read_placed(input, grid, x + dx, y + dy);
      value               = dilate != 0 ? max(value, neighbor) : min(value, neighbor);
    }
  }
  ai_mask_write_r8(output, x, y, value);
}

/// Horizontal Feather pass: float sum of the R8 values / 255 into an R32F image.
__kernel void ai_mask_feather_rows_r32f(__read_only image2d_t input, __write_only image2d_t rows,
                                        AiMaskEdgeGrid grid, AiMaskFeatherWeights weights) {
  const int x = (int)get_global_id(0);
  const int y = (int)get_global_id(1);
  if (x >= grid.width || y >= grid.height) {
    return;
  }
  // The host converts with `u * float(1 / 255.0)`; 1.0f / 255.0f is the same float.
  const float scale = 1.0f / 255.0f;
  float       sum   = weights.values[0] * ((float)ai_mask_read_placed(input, grid, x, y) * scale);
  for (int k = 1; k <= weights.radius; ++k) {
    const float left  = (float)ai_mask_read_placed(input, grid, x - k, y) * scale;
    const float right = (float)ai_mask_read_placed(input, grid, x + k, y) * scale;
    sum += weights.values[k] * (left + right);
  }
  write_imagef(rows, (int2)(x, y), (float4)(sum, 0.0f, 0.0f, 1.0f));
}

/// Vertical Feather pass over the float rows; quantizes like cv::saturate_cast<uchar>.
__kernel void ai_mask_feather_columns_r8(__read_only image2d_t rows, __write_only image2d_t output,
                                         AiMaskEdgeGrid grid, AiMaskFeatherWeights weights) {
  const int x = (int)get_global_id(0);
  const int y = (int)get_global_id(1);
  if (x >= grid.width || y >= grid.height) {
    return;
  }
  const int last = grid.height - 1;
  float     sum  = weights.values[0] * read_imagef(rows, kAiMaskNearestClamp, (int2)(x, y)).x;
  for (int k = 1; k <= weights.radius; ++k) {
    const float above = read_imagef(rows, kAiMaskNearestClamp, (int2)(x, clamp(y - k, 0, last))).x;
    const float below = read_imagef(rows, kAiMaskNearestClamp, (int2)(x, clamp(y + k, 0, last))).x;
    sum += weights.values[k] * (above + below);
  }
  const int quantized = clamp(convert_int_rte(sum * 255.0f), 0, 255);
  ai_mask_write_r8(output, x, y, (uint)quantized);
}
