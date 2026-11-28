//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group ai_mask_coverage (plan #1036, phase VK11): the Feather and Expand of AI Mask
// coverage (SM8 section 5.1), the passes of AiMaskEdgeDispatch, as in
// opencl/shader/ai_mask_coverage.cl. The names, the bindings, and the push constant layout are in
// include/vulkan/vulkan_ai_mask_coverage_kernels.hpp.
//
// R8 images are UNORM: a read gives u / 255, so each read converts back to the integer u, and
// each R8 write stores v / 255 for an integer v. Every pass clamps coordinates to the output
// (replicated border). The Feather weights are a float buffer of radius + 1 values (the half
// kernel), read by its device address: 201 values do not fit in the push constants.

[[vk::binding(0, 0)]] [vk::image_format("r8")] RWTexture2D<float> input_r8;
[[vk::binding(1, 0)]] [vk::image_format("r8")] RWTexture2D<float> output_r8;
[[vk::binding(2, 0)]] [vk::image_format("r32f")] RWTexture2D<float> feather_rows;

// EdgePushConstants: 48 bytes. The input is placed at (source_x, source_y) with source_width x
// source_height texels in the width x height output; texels outside read 0 (AiMaskEdgeGrid).
struct EdgePushConstants {
  uint64_t weights;  // float[radius + 1]; 0 for the morphology
  int      width;
  int      height;
  int      source_x;
  int      source_y;
  int      source_width;
  int      source_height;
  int      dilate;  // 1: dilate, 0: erode
  int      square;  // 1: the square 3 x 3 kernel, 0: the cross
  int      radius;
  int      pad;
};

static float* FloatAt(uint64_t address) { return (float*)address; }

// The integer code of an R8 value read as u / 255: convert_uint_sat_rte(value * 255).
static uint   CodeOf(float value) { return uint(clamp(floor(value * 255.0f + 0.5f), 0.0f, 255.0f)); }

// round-half-even of @p x (convert_int_rte), exact for every float.
static int    RoundHalfEven(float x) {
  const float floor_x  = floor(x);
  const float fraction = x - floor_x;
  int         result   = int(floor_x);
  if (fraction > 0.5f || (fraction == 0.5f && (result & 1) != 0)) {
    result += 1;
  }
  return result;
}

static uint ReadPlaced(EdgePushConstants push, int x, int y) {
  const int source_x = clamp(x, 0, push.width - 1) - push.source_x;
  const int source_y = clamp(y, 0, push.height - 1) - push.source_y;
  if (source_x < 0 || source_y < 0 || source_x >= push.source_width ||
      source_y >= push.source_height) {
    return 0u;
  }
  return CodeOf(input_r8[uint2(uint(source_x), uint(source_y))]);
}

static void WriteR8(int x, int y, uint value) {
  output_r8[uint2(uint(x), uint(y))] = float(value) / 255.0f;
}

// One 3 x 3 dilate (dilate != 0) or erode step with the cross (square == 0) or square kernel.
[shader("compute")]
[numthreads(16, 16, 1)]
void ai_mask_morphology_r8(uint3 id: SV_DispatchThreadID, uniform EdgePushConstants push) {
  const int x = int(id.x);
  const int y = int(id.y);
  if (x >= push.width || y >= push.height) {
    return;
  }
  uint value = ReadPlaced(push, x, y);
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      if ((dx == 0 && dy == 0) || (push.square == 0 && dx != 0 && dy != 0)) {
        continue;
      }
      const uint neighbor = ReadPlaced(push, x + dx, y + dy);
      value               = push.dilate != 0 ? max(value, neighbor) : min(value, neighbor);
    }
  }
  WriteR8(x, y, value);
}

// Horizontal Feather pass: float sum of the R8 values / 255 into an R32F image.
[shader("compute")]
[numthreads(16, 16, 1)]
void ai_mask_feather_rows_r32f(uint3 id: SV_DispatchThreadID, uniform EdgePushConstants push) {
  const int x = int(id.x);
  const int y = int(id.y);
  if (x >= push.width || y >= push.height) {
    return;
  }
  // The host converts with `u * float(1 / 255.0)`; 1.0f / 255.0f is the same float.
  const float  scale   = 1.0f / 255.0f;
  float*       weights = FloatAt(push.weights);
  float        sum     = weights[0] * (float(ReadPlaced(push, x, y)) * scale);
  for (int k = 1; k <= push.radius; ++k) {
    const float left  = float(ReadPlaced(push, x - k, y)) * scale;
    const float right = float(ReadPlaced(push, x + k, y)) * scale;
    sum += weights[k] * (left + right);
  }
  feather_rows[uint2(uint(x), uint(y))] = sum;
}

// Vertical Feather pass over the float rows; quantizes like cv::saturate_cast<uchar>.
[shader("compute")]
[numthreads(16, 16, 1)]
void ai_mask_feather_columns_r8(uint3 id: SV_DispatchThreadID, uniform EdgePushConstants push) {
  const int x = int(id.x);
  const int y = int(id.y);
  if (x >= push.width || y >= push.height) {
    return;
  }
  const int    last    = push.height - 1;
  float*       weights = FloatAt(push.weights);
  float        sum     = weights[0] * feather_rows[uint2(uint(x), uint(y))];
  for (int k = 1; k <= push.radius; ++k) {
    const float above = feather_rows[uint2(uint(x), uint(clamp(y - k, 0, last)))];
    const float below = feather_rows[uint2(uint(x), uint(clamp(y + k, 0, last)))];
    sum += weights[k] * (above + below);
  }
  WriteR8(x, y, uint(clamp(RoundHalfEven(sum * 255.0f), 0, 255)));
}
