//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Check kernels of the shared color math (vulkan_compute_backend_plan.md, phase VK3). They evaluate
// the functions of color/color_encoding_math.h and oklab.slangh for a list of inputs, so a test can
// compare each value with the OpenCL program of the same functions. The names and the push
// constant layouts are in include/vulkan/vulkan_color_math_check_kernels.hpp.

#include "color/color_encoding_math.h"
#include "edit/runtime/vulkan/shader/oklab.slangh"

// TransferPushConstants: 24 bytes, scalar layout.
struct TransferPushConstants {
  float* input;   // device address of `count` values
  float* output;  // device address of `count` * CE_TF_COUNT values
  uint   count;
  uint   encode;  // 0: CeDecode, else CeEncode
};

// ColorPushConstants: 24 bytes, scalar layout.
struct ColorPushConstants {
  float* input;     // device address of `count` RGBA values (alpha is not read)
  float* output;    // device address of `count` RGBA values (alpha is 0)
  uint   count;
  uint   function;  // kColorFunction* of the header
};

// output[tf * count + i] = CeDecode(tf, input[i]) or CeEncode(tf, input[i]), the kernel of
// color_encoding_opencl_test.cpp.
[shader("compute")]
[numthreads(64, 1, 1)]
void color_math_check_transfer(uint3 id: SV_DispatchThreadID,
                               uniform TransferPushConstants push) {
  if (id.x >= push.count * uint(CE_TF_COUNT)) {
    return;
  }
  const int   tf    = int(id.x / push.count);
  const float value = push.input[id.x % push.count];
  push.output[id.x] = push.encode != 0 ? CeEncode(tf, value) : CeDecode(tf, value);
}

// output[i].rgb = function(input[i].rgb).
[shader("compute")]
[numthreads(64, 1, 1)]
void color_math_check_color(uint3 id: SV_DispatchThreadID, uniform ColorPushConstants push) {
  if (id.x >= push.count) {
    return;
  }
  const uint   base  = id.x * 4;
  const float3 value = float3(push.input[base], push.input[base + 1], push.input[base + 2]);
  float3       result;
  switch (push.function) {
    case 0:
      result = AcesccToLinearAp1(value);
      break;
    case 1:
      result = LinearAp1ToAcescc(value);
      break;
    case 2:
      result = LinearAp1ToOkLab(value);
      break;
    default:
      result = OkLabToLinearAp1(value);
      break;
  }
  push.output[base]     = result.x;
  push.output[base + 1] = result.y;
  push.output[base + 2] = result.z;
  push.output[base + 3] = 0.0f;
}
