//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only

#ifndef ALCEDO_OPENCL_OKLAB_CL
#define ALCEDO_OPENCL_OKLAB_CL

// OkLab of scene-linear AP1 for the OpenCL Color Grade and Mask programs. Contrast
// (primary_grade.cl) and the Mask Luminance Range and Hue Range (mask.cl) call these functions, so
// both read the same lightness and hue. Same math as cuda_oklab_math.cuh (documented there). Each
// program lists color_encoding_math.h and this file before the sources that use them
// (opencl_gpu_dag_programs.cpp).

// ACEScc-encoded AP1 (the image between Grades) to scene-linear AP1.
static inline float3 AcesccToLinearAp1(float3 acescc) {
  return (float3)(CeAcesccDecode(acescc.x), CeAcesccDecode(acescc.y), CeAcesccDecode(acescc.z));
}

// Scene-linear AP1 to ACEScc-encoded AP1.
static inline float3 LinearAp1ToAcescc(float3 linear_ap1) {
  return (float3)(CeAcesccEncode(linear_ap1.x), CeAcesccEncode(linear_ap1.y),
                  CeAcesccEncode(linear_ap1.z));
}

static inline float3 LinearAp1ToOkLab(float3 c) {
  const float l  = 0.6341104672f * c.x + 0.3489495087f * c.y + 0.0169400240f * c.z;
  const float m  = 0.2754060131f * c.x + 0.6327713632f * c.y + 0.0918226237f * c.z;
  const float s  = 0.1056775254f * c.x + 0.1971481306f * c.y + 0.6971743440f * c.z;
  const float l_ = cbrt(l);
  const float m_ = cbrt(m);
  const float s_ = cbrt(s);
  return (float3)(0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_,
                  1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_,
                  0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_);
}

static inline float3 OkLabToLinearAp1(float3 lab) {
  const float l_ = lab.x + 0.3963377774f * lab.y + 0.2158037573f * lab.z;
  const float m_ = lab.x - 0.1055613458f * lab.y - 0.0638541728f * lab.z;
  const float s_ = lab.x - 0.0894841775f * lab.y - 1.2914855480f * lab.z;
  const float l  = l_ * l_ * l_;
  const float m  = m_ * m_ * m_;
  const float s  = s_ * s_ * s_;
  return (float3)(2.0693822907f * l - 1.1736821545f * m + 0.1042998638f * s,
                  -0.8917480677f * l + 2.1537429245f * m - 0.2619948569f * s,
                  -0.0615064732f * l - 0.4311325686f * m + 1.4926390418f * s);
}

#endif  // ALCEDO_OPENCL_OKLAB_CL
