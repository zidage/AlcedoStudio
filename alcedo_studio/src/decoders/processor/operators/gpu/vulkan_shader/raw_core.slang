//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group raw_core: the Slang form of linearize_rgb, to_linear_ref_u16_to_f32, and
// cfa_clamp01_f32 of opencl_shader/to_linear_ref.cl (plan phase VK4). Buffers are device
// addresses (plan section 4.2). The names and the push constant layouts are in
// include/vulkan/vulkan_raw_core_kernels.hpp.

#include "raw_utils.slangh"

// RawRgbLinearizationParams: 28 bytes, the struct of raw_rgb_linearization_params.hpp.
struct RawRgbLinearizationParams {
  float black[3];
  float scale[3];
  uint  integer_codes;
};

// LinearizeRgbPushConstants: 48 bytes.
struct LinearizeRgbPushConstants {
  float*                    rgba;  // width * height RGBA values, row-major
  uint                      width;
  uint                      height;
  RawRgbLinearizationParams params;
  uint                      reserved;  // pads the block to the 8-byte struct alignment
};

// ToLinearRefPushConstants: 24 bytes.
struct ToLinearRefPushConstants {
  uint64_t source;      // device address of width * height U16 samples (2-byte aligned)
  float*   destination; // width * height values
  uint64_t parameters;  // WBParams, then PatternParams at kPatternParamsOffset
};

// CfaClampPushConstants: 16 bytes.
struct CfaClampPushConstants {
  float* image;
  uint   width;
  uint   height;
};

// The U16 sample at index `index` of the samples at `base`, read from its 32-bit word, so the
// kernel needs no 16-bit storage feature.
static uint LoadU16(uint64_t base, uint index) {
  const uint64_t address = base + uint64_t(index) * 2;
  const uint     word    = *((uint*)(address & ~uint64_t(3)));
  return (word >> ((uint(address) & 2u) * 8u)) & 0xFFFFu;
}

static float LinearizeChannel(float value, float black, float scale, uint integer_codes) {
  value -= black;
  if (integer_codes != 0) {
    value = max(value, 0.0f);
  }
  return value * scale;
}

// One call for each channel, with constant indices into the parameter arrays: with a loop that
// indexed the push constant arrays with the loop counter, the Intel driver (Iris Xe) left every
// RGB value unchanged (VK4 record).
[shader("compute")]
[numthreads(16, 16, 1)]
void linearize_rgb(uint3 id: SV_DispatchThreadID, uniform LinearizeRgbPushConstants push) {
  if (id.x >= push.width || id.y >= push.height) {
    return;
  }
  const RawRgbLinearizationParams params = push.params;
  const uint                      base   = (id.y * push.width + id.x) * 4;
  push.rgba[base] =
      LinearizeChannel(push.rgba[base], params.black[0], params.scale[0], params.integer_codes);
  push.rgba[base + 1] = LinearizeChannel(push.rgba[base + 1], params.black[1], params.scale[1],
                                         params.integer_codes);
  push.rgba[base + 2] = LinearizeChannel(push.rgba[base + 2], params.black[2], params.scale[2],
                                         params.integer_codes);
  push.rgba[base + 3] = 1.0f;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void to_linear_ref_u16_to_f32(uint3 id: SV_DispatchThreadID,
                              uniform ToLinearRefPushConstants push) {
  WBParams*      wb      = (WBParams*)(push.parameters);
  PatternParams* pattern = (PatternParams*)(push.parameters + kPatternParamsOffset);
  const int      x       = int(id.x);
  const int      y       = int(id.y);
  if (x >= pattern->width || y >= pattern->height) {
    return;
  }
  const int   color_idx = RawColorAt(pattern, y, x);
  const uint  idx       = uint(y * pattern->width + x);
  const float sample    = float(LoadU16(push.source, idx));
  const float black     = wb->black_level[color_idx] + PatternBlackAt(wb, y, x);
  float       pixel     = NormalizeSample(sample, black, wb->white_level[color_idx]);
  pixel *= RelativeWBMultiplier(wb, color_idx);
  push.destination[idx] = pixel;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void cfa_clamp01_f32(uint3 id: SV_DispatchThreadID, uniform CfaClampPushConstants push) {
  if (id.x >= push.width || id.y >= push.height) {
    return;
  }
  const uint idx  = id.y * push.width + id.x;
  push.image[idx] = Clamp01(push.image[idx]);
}
