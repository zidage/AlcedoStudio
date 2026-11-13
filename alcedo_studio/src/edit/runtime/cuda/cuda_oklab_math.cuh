//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cuda_runtime.h>

#include "color/color_encoding_math.h"

namespace alcedo {

// OkLab of scene-linear AP1 for the CUDA Color Grade and Mask kernels. Contrast
// (cuda_primary_grade_pass.cu) and the Mask Luminance Range and Hue Range (cuda_mask_pass.cu)
// call these functions, so both read the same lightness and hue.
//   - AP1 -> LMS uses a white-normalized matrix (each row sums to 1, a von Kries adaptation in
//     OkLab LMS), so AP1 neutrals have a = b = 0 and L = cbrt(Y).
// The OpenCL (shader/oklab.cl) and Metal (shader/oklab_math.h) copies implement the same math;
// GPU tests compare all three against tests/edit/runtime/oklab_contrast_reference.hpp and
// tests/edit/runtime/mask_range_reference.hpp.

/// ACEScc-encoded AP1 (the image between Grades) to scene-linear AP1.
__device__ inline auto AcesccToLinearAp1(const float3& acescc) -> float3 {
  return make_float3(CeAcesccDecode(acescc.x), CeAcesccDecode(acescc.y), CeAcesccDecode(acescc.z));
}

/// Scene-linear AP1 to ACEScc-encoded AP1.
__device__ inline auto LinearAp1ToAcescc(const float3& linear_ap1) -> float3 {
  return make_float3(CeAcesccEncode(linear_ap1.x), CeAcesccEncode(linear_ap1.y),
                     CeAcesccEncode(linear_ap1.z));
}

__device__ inline auto LinearAp1ToOkLab(const float3& c) -> float3 {
  const float l  = 0.6341104672f * c.x + 0.3489495087f * c.y + 0.0169400240f * c.z;
  const float m  = 0.2754060131f * c.x + 0.6327713632f * c.y + 0.0918226237f * c.z;
  const float s  = 0.1056775254f * c.x + 0.1971481306f * c.y + 0.6971743440f * c.z;
  const float l_ = cbrtf(l);
  const float m_ = cbrtf(m);
  const float s_ = cbrtf(s);
  return make_float3(0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_,
                     1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_,
                     0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_);
}

__device__ inline auto OkLabToLinearAp1(const float3& lab) -> float3 {
  const float l_ = lab.x + 0.3963377774f * lab.y + 0.2158037573f * lab.z;
  const float m_ = lab.x - 0.1055613458f * lab.y - 0.0638541728f * lab.z;
  const float s_ = lab.x - 0.0894841775f * lab.y - 1.2914855480f * lab.z;
  const float l  = l_ * l_ * l_;
  const float m  = m_ * m_ * m_;
  const float s  = s_ * s_ * s_;
  return make_float3(2.0693822907f * l - 1.1736821545f * m + 0.1042998638f * s,
                     -0.8917480677f * l + 2.1537429245f * m - 0.2619948569f * s,
                     -0.0615064732f * l - 0.4311325686f * m + 1.4926390418f * s);
}

}  // namespace alcedo
