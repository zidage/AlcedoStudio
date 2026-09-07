// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#ifndef ALCEDO_ACES_REFERENCE_GAMUT_COMPRESSION_H
#define ALCEDO_ACES_REFERENCE_GAMUT_COMPRESSION_H

// ACES 1.3 Reference Gamut Compression shared by the host, CUDA, OpenCL C, and Metal.
#if defined(__OPENCL_VERSION__) || defined(__OPENCL_C_VERSION__)
#define ACES_RGC_INLINE static inline
#define ACES_RGC_POW    pow
#elif defined(__METAL_VERSION__)
#define ACES_RGC_INLINE static inline
#define ACES_RGC_POW    pow
#else
#include <cmath>
#ifdef __CUDACC__
#define ACES_RGC_INLINE static __host__ __device__ inline
#else
#define ACES_RGC_INLINE static inline
#endif
#define ACES_RGC_POW powf
#endif

typedef struct {
  float r, g, b;
} AcesRgcRgb;

ACES_RGC_INLINE AcesRgcRgb AcesRgcMakeRgb(float r, float g, float b) {
  AcesRgcRgb result;
  result.r = r;
  result.g = g;
  result.b = b;
  return result;
}

ACES_RGC_INLINE float AcesRgcMax(float a, float b) { return a > b ? a : b; }

ACES_RGC_INLINE float AcesRgcCompressDistance(float distance, float limit, float threshold) {
  const float power = 1.2f;
  if (distance < threshold) return distance;
  const float threshold_difference = limit - threshold;
  const float one_minus_threshold   = 1.0f - threshold;
  const float inner = ACES_RGC_POW(one_minus_threshold / threshold_difference, -power);
  const float denominator =
      ACES_RGC_POW(AcesRgcMax(inner - 1.0f, 0.0f), 1.0f / power);
  if (denominator <= 1.0e-6f) return threshold;
  const float scale      = threshold_difference / denominator;
  const float normalized = (distance - threshold) / scale;
  const float shaped     = ACES_RGC_POW(AcesRgcMax(normalized, 0.0f), power);
  return threshold + scale * normalized / ACES_RGC_POW(1.0f + shaped, 1.0f / power);
}

/** Compress AP1 colors toward their achromatic axis before ACEScc encoding. */
ACES_RGC_INLINE AcesRgcRgb AcesReferenceGamutCompress(float r, float g, float b) {
  const float achromatic = AcesRgcMax(r, AcesRgcMax(g, b));
  const float magnitude  = achromatic < 0.0f ? -achromatic : achromatic;
  if (magnitude <= 1.0e-6f) return AcesRgcMakeRgb(r, g, b);

  const float cyan_distance    = (achromatic - r) / magnitude;
  const float magenta_distance = (achromatic - g) / magnitude;
  const float yellow_distance  = (achromatic - b) / magnitude;
  return AcesRgcMakeRgb(
      achromatic - AcesRgcCompressDistance(cyan_distance, 1.147f, 0.815f) * magnitude,
      achromatic - AcesRgcCompressDistance(magenta_distance, 1.264f, 0.803f) * magnitude,
      achromatic - AcesRgcCompressDistance(yellow_distance, 1.312f, 0.880f) * magnitude);
}

#undef ACES_RGC_INLINE
#undef ACES_RGC_POW
#endif
