// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#ifndef ALCEDO_DISPLAY_TO_AP1_MATH_H
#define ALCEDO_DISPLAY_TO_AP1_MATH_H

// Raster input color conversion to the ACEScc AP1 working space, shared by the host reference,
// CUDA, OpenCL C and Metal (docs/roadmap/alcedo_studio/edit/raster_image_input_plan.md, 5.3 and
// 5.5).
//
// Display-referred branch: the OpenColorIO 2.5.1 ACES 2.0 output transform inverse of
// aces2_reference_math.h. Scene-linear branch: one 3x3 matrix and the ACES 1.3 reference gamut
// compression. Both write ACEScc-encoded AP1.
//
// All parameters and tables live in one float buffer with the layout below. The host fills it
// (aces2_reference_runtime.cpp, raster_develop_params.cpp) and each backend uploads it as one
// device buffer.

// ACEScc comes from color/color_encoding_math.h and the ACES 2.0 stages from
// aces2_reference_math.h. OpenCL programs list both before this file; Metal shaders include them
// first.
#if !defined(__OPENCL_VERSION__) && !defined(__OPENCL_C_VERSION__) && !defined(__METAL_VERSION__)
#include "color/color_encoding_math.h"
#include "edit/runtime/aces2_reference_math.h"
#endif

#if defined(__OPENCL_VERSION__) || defined(__OPENCL_C_VERSION__)
#define D2A_GLOBAL __global
#define D2A_INLINE static inline
#define D2A_POW    pow
#elif defined(__METAL_VERSION__)
#define D2A_GLOBAL device
#define D2A_INLINE static inline
#define D2A_POW    pow
#else
#include <cmath>
#define D2A_GLOBAL
#ifdef __CUDACC__
#define D2A_INLINE static __host__ __device__ inline
#else
#define D2A_INLINE static inline
#endif
#define D2A_POW powf
#endif

// ---------------------------------------------------------------------------------------------
// Packed parameter layout (float indices)
// ---------------------------------------------------------------------------------------------

// The display-referred block is an ACES 2.0 reference block (aces2_reference_math.h). Its
// reserved slot 0 is 0 and is read here as the branch flag; its display-to-limiting matrix sits
// at ALCEDO_D2A_SOURCE_TO_TARGET.

/// 0: display-referred ACES 2.0 inverse. 1: scene-linear matrix.
#define ALCEDO_D2A_BRANCH            ALCEDO_A2R_RESERVED
/// Display branch: source RGB to limiting RGB. Scene branch: source RGB to AP1.
#define ALCEDO_D2A_SOURCE_TO_TARGET  ALCEDO_A2R_DISPLAY_TO_LIMIT
/// Floats that the scene-linear branch reads.
#define ALCEDO_D2A_SCENE_PACKED_SIZE 10
/// Floats of a display-referred block.
#define ALCEDO_D2A_PACKED_SIZE       ALCEDO_A2R_PACKED_SIZE

// ---------------------------------------------------------------------------------------------
// Scene-linear branch
// ---------------------------------------------------------------------------------------------

// ACES 1.3 reference gamut compression, identical to aces_reference_gamut_compression.h.
D2A_INLINE float D2aRgcCompressDistance(float distance, float limit, float threshold) {
  const float power = 1.2f;
  if (distance < threshold) return distance;
  const float threshold_difference = limit - threshold;
  const float one_minus_threshold  = 1.0f - threshold;
  const float inner                = D2A_POW(one_minus_threshold / threshold_difference, -power);
  const float denominator          = D2A_POW(A2rMax(inner - 1.0f, 0.0f), 1.0f / power);
  if (denominator <= 1.0e-6f) return threshold;
  const float scale      = threshold_difference / denominator;
  const float normalized = (distance - threshold) / scale;
  const float shaped     = D2A_POW(A2rMax(normalized, 0.0f), power);
  return threshold + scale * normalized / D2A_POW(1.0f + shaped, 1.0f / power);
}

D2A_INLINE A2rFloat3 D2aReferenceGamutCompress(A2rFloat3 c) {
  const float achromatic = A2rMax(c.x, A2rMax(c.y, c.z));
  const float magnitude  = achromatic < 0.0f ? -achromatic : achromatic;
  if (magnitude <= 1.0e-6f) return c;
  const float cyan    = (achromatic - c.x) / magnitude;
  const float magenta = (achromatic - c.y) / magnitude;
  const float yellow  = (achromatic - c.z) / magnitude;
  return A2rMake3(achromatic - D2aRgcCompressDistance(cyan, 1.147f, 0.815f) * magnitude,
                  achromatic - D2aRgcCompressDistance(magenta, 1.264f, 0.803f) * magnitude,
                  achromatic - D2aRgcCompressDistance(yellow, 1.312f, 0.880f) * magnitude);
}

// ---------------------------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------------------------

/**
 * Convert one source pixel to ACEScc-encoded AP1 (the develop_output encoding).
 *
 * Display-referred input goes through the ACES 2.0 inverse, AP0 to AP1 and the clamp to
 * [0, forward limit]. Scene-linear input goes through the source-to-AP1 matrix and the ACES 1.3
 * reference gamut compression.
 */
D2A_INLINE A2rFloat3 D2aSourceToAcesccAp1(A2rFloat3 source_rgb, D2A_GLOBAL const float* p) {
  A2rFloat3 ap1;
  if (p[ALCEDO_D2A_BRANCH] != 0.0f) {
    ap1 = D2aReferenceGamutCompress(A2rMul(p + ALCEDO_D2A_SOURCE_TO_TARGET, source_rgb));
  } else {
    const A2rFloat3 ap0     = A2rDisplayToAp0(source_rgb, p);
    const A2rFloat3 linear  = A2rMul(p + ALCEDO_A2R_AP0_TO_AP1, ap0);
    const float     ap1_max = p[ALCEDO_A2R_AP1_MAX];
    ap1 = A2rMake3(A2rClamp(linear.x, 0.0f, ap1_max), A2rClamp(linear.y, 0.0f, ap1_max),
                   A2rClamp(linear.z, 0.0f, ap1_max));
  }
  return A2rMake3(CeAcesccEncode(ap1.x), CeAcesccEncode(ap1.y), CeAcesccEncode(ap1.z));
}

#undef D2A_GLOBAL
#undef D2A_INLINE
#undef D2A_POW
#endif
