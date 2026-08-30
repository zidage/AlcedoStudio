// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#ifndef ALCEDO_DNG_PROFILE_GPU_MATH_H
#define ALCEDO_DNG_PROFILE_GPU_MATH_H

// One implementation for the host reference, CUDA, OpenCL C and Metal.
#if defined(__OPENCL_VERSION__) || defined(__OPENCL_C_VERSION__)
#define DNG_GLOBAL __global
#define DNG_INLINE static inline
#define DNG_POW    pow
#elif defined(__METAL_VERSION__)
#define DNG_GLOBAL device
#define DNG_INLINE static inline
#define DNG_POW    pow
#else
#include <cmath>
#define DNG_GLOBAL
#ifdef __CUDACC__
#define DNG_INLINE static __host__ __device__ inline
#else
#define DNG_INLINE static inline
#endif
#define DNG_POW powf
#endif

typedef struct {
  float r, g, b;
} DngRgb;
DNG_INLINE DngRgb DngMakeRgb(float r, float g, float b) {
  DngRgb c;
  c.r = r;
  c.g = g;
  c.b = b;
  return c;
}
DNG_INLINE float DngMin(float a, float b) { return a < b ? a : b; }
DNG_INLINE float DngMax(float a, float b) { return a > b ? a : b; }
DNG_INLINE float DngClamp(float x, float lo, float hi) { return DngMin(hi, DngMax(lo, x)); }
DNG_INLINE float DngSrgbEncode(float x) {
  return x <= 0.0031308f ? 12.92f * x : 1.055f * DNG_POW(x, 1.0f / 2.4f) - 0.055f;
}
DNG_INLINE float DngSrgbDecode(float x) {
  return x <= 0.04045f ? x / 12.92f : DNG_POW((x + 0.055f) / 1.055f, 2.4f);
}
DNG_INLINE DngRgb DngMatrixRgb(DngRgb c, DNG_GLOBAL const float* m) {
  return DngMakeRgb(m[0] * c.r + m[1] * c.g + m[2] * c.b, m[3] * c.r + m[4] * c.g + m[5] * c.b,
                    m[6] * c.r + m[7] * c.g + m[8] * c.b);
}

/** Trilinear DNG HSV interpolation, with hue wrapping and an optional sRGB value axis.
 * Lookup coordinates stop at the table boundary; positive scene headroom is retained.
 * Table layout: [value][hue][saturation][hue_degrees, saturation_scale, value_scale].
 */
DNG_INLINE DngRgb DngApplyHueSatMap(DngRgb rgb, DNG_GLOBAL const float* data, unsigned descriptor) {
  const unsigned offset = (unsigned)data[descriptor];
  if (offset == 0) return rgb;
  const unsigned hd = (unsigned)data[descriptor + 1], sd = (unsigned)data[descriptor + 2];
  const unsigned vd = (unsigned)data[descriptor + 3], encoding = (unsigned)data[descriptor + 4];
  rgb.r             = DngMax(0.0f, rgb.r);
  rgb.g             = DngMax(0.0f, rgb.g);
  rgb.b             = DngMax(0.0f, rgb.b);
  float       v     = DngMax(rgb.r, DngMax(rgb.g, rgb.b));
  const float delta = v - DngMin(rgb.r, DngMin(rgb.g, rgb.b));
  float       s     = v > 0.0f ? delta / v : 0.0f;
  float       h     = 0.0f;
  if (delta > 0.0f) {
    if (v == rgb.r)
      h = (rgb.g - rgb.b) / delta;
    else if (v == rgb.g)
      h = 2.0f + (rgb.b - rgb.r) / delta;
    else
      h = 4.0f + (rgb.r - rgb.g) / delta;
    if (h < 0.0f) h += 6.0f;
  }
  const float    encoded_v = encoding ? DngSrgbEncode(v) : v;
  const float    hx = h * ((float)hd / 6.0f), sx = DngClamp(s, 0.0f, 1.0f) * (float)(sd - 1);
  const float    vx = DngClamp(encoded_v, 0.0f, 1.0f) * (float)(vd - 1);
  const unsigned h0 = (unsigned)hx % hd, h1 = (h0 + 1) % hd;
  const unsigned s0 = (unsigned)DngMin((float)(sd - 2), sx), s1 = s0 + 1;
  const unsigned v0 = vd > 1 ? (unsigned)DngMin((float)(vd - 2), vx) : 0;
  const unsigned v1 = vd > 1 ? v0 + 1 : 0;
  const float    hw = hx - (float)(unsigned)hx, sw = sx - (float)s0, vw = vx - (float)v0;
  float          correction[3] = {0.0f, 0.0f, 0.0f};
  for (unsigned vi = 0; vi < 2; ++vi)
    for (unsigned hi = 0; hi < 2; ++hi)
      for (unsigned si = 0; si < 2; ++si) {
        const float weight = (vi ? vw : 1.0f - vw) * (hi ? hw : 1.0f - hw) * (si ? sw : 1.0f - sw);
        const unsigned at =
            offset + (((vi ? v1 : v0) * hd + (hi ? h1 : h0)) * sd + (si ? s1 : s0)) * 3;
        for (unsigned channel = 0; channel < 3; ++channel)
          correction[channel] += weight * data[at + channel];
      }
  h += correction[0] / 60.0f;
  h -= 6.0f * (float)floor(h / 6.0f);
  s                     = DngClamp(s * correction[1], 0.0f, 1.0f);
  v                     = encoding ? DngSrgbDecode(encoded_v * correction[2]) : v * correction[2];
  const unsigned sector = (unsigned)h;
  const float    f = h - (float)sector, p = v * (1.0f - s), q = v * (1.0f - s * f),
              t = v * (1.0f - s * (1.0f - f));
  switch (sector) {
    case 0:
      return DngMakeRgb(v, t, p);
    case 1:
      return DngMakeRgb(q, v, p);
    case 2:
      return DngMakeRgb(p, v, t);
    case 3:
      return DngMakeRgb(p, q, v);
    case 4:
      return DngMakeRgb(t, p, v);
    default:
      return DngMakeRgb(v, p, q);
  }
}

/// AP1 -> linear ProPhoto D50 -> HueSatMap -> baseline exposure -> LookTable -> AP1.
DNG_INLINE DngRgb DngApplyColorProfile(DngRgb ap1, DNG_GLOBAL const float* data) {
  if (data[0] == 0.0f) return ap1;
  DngRgb c = DngMatrixRgb(ap1, data + 12);
  c        = DngApplyHueSatMap(c, data, 2);
  c.r *= data[1];
  c.g *= data[1];
  c.b *= data[1];
  c = DngApplyHueSatMap(c, data, 7);
  return DngMatrixRgb(c, data + 21);
}
#undef DNG_GLOBAL
#undef DNG_INLINE
#undef DNG_POW
#endif
