//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
//
//  Portions of this file are a port of OpenColorIO 2.5.1
//  (src/OpenColorIO/ops/fixedfunction/ACES2/Transform.cpp and
//  src/OpenColorIO/transforms/builtins/ColorMatrixHelpers.cpp):
//  Copyright Contributors to the OpenColorIO Project. SPDX-License-Identifier: BSD-3-Clause.
//  See THIRD_PARTY_NOTICE.txt.

#include "edit/runtime/drt/aces2_inverse_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>

// The tables are compared with OpenColorIO within the tolerance of raster_image_input_plan.md,
// section 5.7. Apple clang fuses a * b + c into one FMA by default; with that rounding the table
// build moves the inverse outside the tolerance near a display channel of 0 (seen on Apple
// silicon, on the host and on Metal, which uses these tables). The build keeps separate
// multiplies and adds on every compiler.
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace alcedo {
namespace {

// The port keeps OCIO's single-precision arithmetic so that its tables match OCIO's.

using F2                                 = std::array<float, 2>;
using F3                                 = std::array<float, 3>;
using M33f                               = std::array<float, 9>;  // Row major; Mul(v, m) is m * v.
using M33d                               = std::array<double, 9>;

constexpr float    kPi                   = 3.14159265358979f;
constexpr float    kHueLimit             = 360.0f;
constexpr float    kReferenceLuminance   = 100.0f;
constexpr float    kLA                   = 100.0f;
constexpr float    kYb                   = 20.0f;
constexpr F3       kSurround             = {0.9f, 0.59f, 0.9f};
constexpr float    kJScale               = 100.0f;
constexpr float    kCamNlOffset          = 0.2713f * 100.0f;
constexpr float    kCamNlScale           = 4.0f * 100.0f;
constexpr float    kChromaCompress       = 2.4f;
constexpr float    kChromaCompressFact   = 3.3f;
constexpr float    kChromaExpand         = 1.3f;
constexpr float    kChromaExpandFact     = 0.69f;
constexpr float    kChromaExpandThr      = 0.5f;
constexpr float    kSmoothCusps          = 0.12f;
constexpr float    kSmoothM              = 0.27f;
constexpr float    kCuspMidBlend         = 1.3f;
constexpr float    kFocusGainBlend       = 0.3f;
constexpr float    kFocusDistance        = 1.35f;
constexpr float    kFocusDistanceScaling = 1.75f;
constexpr float    kGammaMinimum         = 0.0f;
constexpr float    kGammaMaximum         = 5.0f;
constexpr float    kGammaSearchStep      = 0.4f;
constexpr float    kGammaAccuracy        = 1e-5f;
constexpr int      kCuspCornerCount      = 6;
constexpr int      kTotalCornerCount     = kCuspCornerCount + 2;
constexpr int      kMaxSortedCorners     = 2 * kCuspCornerCount;
constexpr float    kReachCuspTolerance   = 1e-3f;
constexpr float    kDisplayCuspTolerance = 1e-7f;

constexpr unsigned kTableSize            = ALCEDO_D2A_TABLE_SIZE;
constexpr unsigned kNominalSize          = 360;
constexpr unsigned kBaseIndex            = ALCEDO_D2A_TABLE_BASE_INDEX;
constexpr unsigned kLowerWrapIndex       = ALCEDO_D2A_TABLE_LOWER_WRAP_INDEX;
constexpr unsigned kUpperWrapIndex       = ALCEDO_D2A_TABLE_UPPER_WRAP_INDEX;
constexpr unsigned kFirstNominalIndex    = kBaseIndex;
constexpr unsigned kLastNominalIndex     = kUpperWrapIndex - 1;

using Table1D                            = std::array<float, kTableSize>;
using Table3D                            = std::array<F3, kTableSize>;

constexpr std::array<float, 8> kCam16Primaries = {0.8336f, 0.1735f, 2.3854f, -1.4659f,
                                                  0.087f,  -0.125f, 0.333f,  0.333f};
constexpr std::array<float, 8> kAp0Primaries   = {0.7347f, 0.2653f,  0.0f,     1.0f,
                                                  0.0001f, -0.0770f, 0.32168f, 0.33767f};
constexpr std::array<float, 8> kAp1Primaries   = {0.713f, 0.293f, 0.165f,   0.830f,
                                                  0.128f, 0.044f, 0.32168f, 0.33767f};

// -------------------------------------------------------------------------------------------
// Matrices (OCIO ColorMatrixHelpers: double precision, then single)
// -------------------------------------------------------------------------------------------

auto                           MulD(const M33d& a, const M33d& b) -> M33d {
  M33d out{};
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      out[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
    }
  }
  return out;
}

/// Gauss-Jordan inverse with partial pivoting, in the operation order of OCIO
/// MatrixOpData::MatrixArray::inverse (from Imath gjInverse), so that the single-precision
/// matrices match OCIO's bit for bit.
auto InverseD(const M33d& m) -> M33d {
  M33d t = m;
  M33d s = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
  for (int i = 0; i < 3; ++i) {
    int    pivot     = i;
    double pivotsize = std::abs(t[i * 3 + i]);
    for (int j = i + 1; j < 3; ++j) {
      const double tmp = std::abs(t[j * 3 + i]);
      if (tmp > pivotsize) {
        pivot     = j;
        pivotsize = tmp;
      }
    }
    if (pivotsize == 0.0) {
      throw std::runtime_error("ACES 2.0 inverse: singular matrix");
    }
    if (pivot != i) {
      for (int j = 0; j < 3; ++j) {
        std::swap(t[i * 3 + j], t[pivot * 3 + j]);
        std::swap(s[i * 3 + j], s[pivot * 3 + j]);
      }
    }
    for (int j = i + 1; j < 3; ++j) {
      const double f = t[j * 3 + i] / t[i * 3 + i];
      for (int k = 0; k < 3; ++k) {
        t[j * 3 + k] -= f * t[i * 3 + k];
        s[j * 3 + k] -= f * s[i * 3 + k];
      }
    }
  }
  for (int i = 2; i >= 0; --i) {
    const double f = t[i * 3 + i];
    if (f == 0.0) {
      throw std::runtime_error("ACES 2.0 inverse: singular matrix");
    }
    for (int j = 0; j < 3; ++j) {
      t[i * 3 + j] /= f;
      s[i * 3 + j] /= f;
    }
    for (int j = 0; j < i; ++j) {
      const double g = t[j * 3 + i];
      for (int k = 0; k < 3; ++k) {
        t[j * 3 + k] -= g * t[i * 3 + k];
        s[j * 3 + k] -= g * s[i * 3 + k];
      }
    }
  }
  return s;
}

auto ToFloat(const M33d& m) -> M33f {
  M33f out{};
  for (std::size_t i = 0; i < 9; ++i) {
    out[i] = static_cast<float>(m[i]);
  }
  return out;
}

/// RGB to XYZ with Y(white) = 1 (OCIO rgb2xyz_from_xy).
auto RgbToXyzD(const std::array<float, 8>& p) -> M33d {
  const double rx = p[0], ry = p[1], gx = p[2], gy = p[3], bx = p[4], by = p[5];
  const double wx = p[6], wy = p[7];
  const M33d   xyz      = {rx, gx, bx, ry, gy, by, 1.0 - rx - ry, 1.0 - gx - gy, 1.0 - bx - by};
  const M33d   inv      = InverseD(xyz);
  const double white[3] = {wx / wy, 1.0, (1.0 - wx - wy) / wy};
  M33d         out{};
  for (int i = 0; i < 3; ++i) {
    const double gain =
        white[0] * inv[i * 3] + white[1] * inv[i * 3 + 1] + white[2] * inv[i * 3 + 2];
    for (int j = 0; j < 3; ++j) {
      out[j * 3 + i] = gain * xyz[j * 3 + i];
    }
  }
  return out;
}

/// CAT02 adaptation from @p source_white to @p target_white (xy).
auto Cat02D(double sx, double sy, double tx, double ty) -> M33d {
  constexpr M33d kCat02 = {0.7328, 0.4296, -0.1624, -0.7036, 1.6975,
                           0.0061, 0.0030, 0.0136,  0.9834};
  const double   src[3] = {sx / sy, 1.0, (1.0 - sx - sy) / sy};
  const double   dst[3] = {tx / ty, 1.0, (1.0 - tx - ty) / ty};
  double         src_lms[3]{}, dst_lms[3]{};
  for (int r = 0; r < 3; ++r) {
    src_lms[r] = kCat02[r * 3] * src[0] + kCat02[r * 3 + 1] * src[1] + kCat02[r * 3 + 2] * src[2];
    dst_lms[r] = kCat02[r * 3] * dst[0] + kCat02[r * 3 + 1] * dst[1] + kCat02[r * 3 + 2] * dst[2];
  }
  const M33d scale = {dst_lms[0] / src_lms[0], 0, 0, 0, dst_lms[1] / src_lms[1], 0, 0, 0,
                      dst_lms[2] / src_lms[2]};
  return MulD(InverseD(kCat02), MulD(scale, kCat02));
}

/// Source RGB to target RGB through XYZ with a CAT02 adaptation between the two whites.
auto RgbToRgbAdaptedD(const std::array<float, 8>& source, const std::array<float, 8>& target)
    -> M33d {
  const M33d to_xyz   = RgbToXyzD(source);
  const M33d from_xyz = InverseD(RgbToXyzD(target));
  if (source[6] == target[6] && source[7] == target[7]) {
    return MulD(from_xyz, to_xyz);
  }
  return MulD(from_xyz, MulD(Cat02D(source[6], source[7], target[6], target[7]), to_xyz));
}

auto MulF(const M33f& a, const M33f& b) -> M33f {
  return {a[0] * b[0] + a[1] * b[3] + a[2] * b[6], a[0] * b[1] + a[1] * b[4] + a[2] * b[7],
          a[0] * b[2] + a[1] * b[5] + a[2] * b[8], a[3] * b[0] + a[4] * b[3] + a[5] * b[6],
          a[3] * b[1] + a[4] * b[4] + a[5] * b[7], a[3] * b[2] + a[4] * b[5] + a[5] * b[8],
          a[6] * b[0] + a[7] * b[3] + a[8] * b[6], a[6] * b[1] + a[7] * b[4] + a[8] * b[7],
          a[6] * b[2] + a[7] * b[5] + a[8] * b[8]};
}

auto Mul(const F3& v, const M33f& m) -> F3 {
  return {v[0] * m[0] + v[1] * m[1] + v[2] * m[2], v[0] * m[3] + v[1] * m[4] + v[2] * m[5],
          v[0] * m[6] + v[1] * m[7] + v[2] * m[8]};
}

auto Diag(const F3& d) -> M33f { return {d[0], 0.f, 0.f, 0.f, d[1], 0.f, 0.f, 0.f, d[2]}; }

auto Scale(float s, const F3& v) -> F3 { return {s * v[0], s * v[1], s * v[2]}; }

auto LerpF(float a, float b, float t) -> float { return (b - a) * t + a; }

auto LerpF3(const F3& a, const F3& b, float t) -> F3 {
  return {LerpF(a[0], b[0], t), LerpF(a[1], b[1], t), LerpF(a[2], b[2], t)};
}

// -------------------------------------------------------------------------------------------
// CAM (OCIO ACES2)
// -------------------------------------------------------------------------------------------

struct JmhParams {
  M33f  rgb_to_cam16_c_{};
  M33f  cam16_c_to_rgb_{};
  M33f  cone_to_aab_{};
  M33f  aab_to_cone_{};
  float f_l_n_     = 0.0f;
  float cz_        = 0.0f;
  float inv_cz_    = 0.0f;
  float a_w_j_     = 0.0f;
  float inv_a_w_j_ = 0.0f;
};

auto ConeCompressFwdAbs(float rc) -> float {
  const float f_l_y = std::pow(rc, 0.42f);
  return f_l_y / (kCamNlOffset + f_l_y);
}

auto ConeCompressInvAbs(float ra) -> float {
  const float ra_lim = std::min(ra, 0.99f);
  const float f_l_y  = (kCamNlOffset * ra_lim) / (1.0f - ra_lim);
  return std::pow(f_l_y, 1.f / 0.42f);
}

auto ConeCompressFwd(float v) -> float { return std::copysign(ConeCompressFwdAbs(std::abs(v)), v); }
auto ConeCompressInv(float v) -> float { return std::copysign(ConeCompressInvAbs(std::abs(v)), v); }

auto ModelGamma() -> float { return kSurround[1] * (1.48f + std::sqrt(kYb / kReferenceLuminance)); }

auto InitJmhParams(const std::array<float, 8>& primaries) -> JmhParams {
  const M33f base_cone_to_aab = {2.0f,        1.0f,           1.0f / 20.0f,
                                 1.0f,        -12.0f / 11.0f, 1.0f / 11.0f,
                                 1.0f / 9.0f, 1.0f / 9.0f,    -2.0f / 9.0f};
  const M33f matrix_16        = ToFloat(InverseD(RgbToXyzD(kCam16Primaries)));
  const M33f rgb_to_xyz       = ToFloat(RgbToXyzD(primaries));
  const F3 xyz_w = Mul({kReferenceLuminance, kReferenceLuminance, kReferenceLuminance}, rgb_to_xyz);
  const float     y_w   = xyz_w[1];
  const F3        rgb_w = Mul(xyz_w, matrix_16);

  constexpr float k     = 1.f / (5.f * kLA + 1.f);
  constexpr float k4    = k * k * k * k;
  const float     f_l =
      0.2f * k4 * (5.f * kLA) + 0.1f * std::pow((1.f - k4), 2.f) * std::pow(5.f * kLA, 1.f / 3.f);
  const float f_l_n  = f_l / kReferenceLuminance;
  const float cz     = ModelGamma();

  const F3    d_rgb  = {f_l_n * y_w / rgb_w[0], f_l_n * y_w / rgb_w[1], f_l_n * y_w / rgb_w[2]};
  const F3    rgb_wc = {d_rgb[0] * rgb_w[0], d_rgb[1] * rgb_w[1], d_rgb[2] * rgb_w[2]};
  const F3    rgb_aw = {ConeCompressFwd(rgb_wc[0]), ConeCompressFwd(rgb_wc[1]),
                        ConeCompressFwd(rgb_wc[2])};

  const M33f  cone_to_aab = MulF(Diag({kCamNlScale, kCamNlScale, kCamNlScale}), base_cone_to_aab);
  const float a_w =
      cone_to_aab[0] * rgb_aw[0] + cone_to_aab[1] * rgb_aw[1] + cone_to_aab[2] * rgb_aw[2];
  const float a_w_j = ConeCompressFwdAbs(f_l);

  // RGBtoRGB_f33(prims, CAM16) = XYZtoRGB(CAM16) * RGBtoXYZ(prims), in single precision.
  const M33f  rgb_to_cam16 =
      MulF(MulF(matrix_16, rgb_to_xyz),
           Diag({kReferenceLuminance, kReferenceLuminance, kReferenceLuminance}));
  const M33f rgb_to_cam16_c = MulF(Diag(d_rgb), rgb_to_cam16);

  JmhParams  p;
  p.rgb_to_cam16_c_ = rgb_to_cam16_c;
  p.cam16_c_to_rgb_ = ToFloat(InverseD({rgb_to_cam16_c[0], rgb_to_cam16_c[1], rgb_to_cam16_c[2],
                                        rgb_to_cam16_c[3], rgb_to_cam16_c[4], rgb_to_cam16_c[5],
                                        rgb_to_cam16_c[6], rgb_to_cam16_c[7], rgb_to_cam16_c[8]}));
  const float s     = 43.f * kSurround[2];
  p.cone_to_aab_    = {cone_to_aab[0] / a_w, cone_to_aab[1] / a_w, cone_to_aab[2] / a_w,
                       cone_to_aab[3] * s,   cone_to_aab[4] * s,   cone_to_aab[5] * s,
                       cone_to_aab[6] * s,   cone_to_aab[7] * s,   cone_to_aab[8] * s};
  p.aab_to_cone_    = ToFloat(InverseD({p.cone_to_aab_[0], p.cone_to_aab_[1], p.cone_to_aab_[2],
                                        p.cone_to_aab_[3], p.cone_to_aab_[4], p.cone_to_aab_[5],
                                        p.cone_to_aab_[6], p.cone_to_aab_[7], p.cone_to_aab_[8]}));
  p.f_l_n_          = f_l_n;
  p.cz_             = cz;
  p.inv_cz_         = 1.0f / cz;
  p.a_w_j_          = a_w_j;
  p.inv_a_w_j_      = 1.0f / a_w_j;
  return p;
}

auto RgbToAab(const F3& rgb, const JmhParams& p) -> F3 {
  const F3 rgb_m = Mul(rgb, p.rgb_to_cam16_c_);
  const F3 rgb_a = {ConeCompressFwd(rgb_m[0]), ConeCompressFwd(rgb_m[1]),
                    ConeCompressFwd(rgb_m[2])};
  return Mul(rgb_a, p.cone_to_aab_);
}

auto WrapHue(float y) -> float { return y < 0.f ? y + kHueLimit : y; }

auto AabToJmh(const F3& aab, const JmhParams& p) -> F3 {
  if (aab[0] <= 0.f) {
    return {0.f, 0.f, 0.f};
  }
  const float J = kJScale * std::pow(aab[0], p.cz_);
  const float M = std::sqrt(aab[1] * aab[1] + aab[2] * aab[2]);
  const float h = WrapHue(180.0f * std::atan2(aab[2], aab[1]) / kPi);
  return {J, M, h};
}

auto RgbToJmh(const F3& rgb, const JmhParams& p) -> F3 { return AabToJmh(RgbToAab(rgb, p), p); }

auto JmhToRgb(const F3& jmh, const JmhParams& p) -> F3 {
  const float h_rad = kPi * jmh[2] / 180.0f;
  const F3    aab   = {std::pow(jmh[0] * (1.0f / kJScale), p.inv_cz_), jmh[1] * std::cos(h_rad),
                       jmh[1] * std::sin(h_rad)};
  const F3    rgb_a = Mul(aab, p.aab_to_cone_);
  const F3    rgb_m = {ConeCompressInv(rgb_a[0]), ConeCompressInv(rgb_a[1]),
                       ConeCompressInv(rgb_a[2])};
  return Mul(rgb_m, p.cam16_c_to_rgb_);
}

auto YToJ(float y, const JmhParams& p) -> float {
  const float ra = ConeCompressFwdAbs(std::abs(y) * p.f_l_n_);
  return std::copysign(kJScale * std::pow(ra * p.inv_a_w_j_, p.cz_), y);
}

auto JToAchromatic(float J, float inv_cz) -> float {
  return std::pow(J * (1.0f / kJScale), inv_cz);
}

// -------------------------------------------------------------------------------------------
// Tonescale, chroma and gamut parameters
// -------------------------------------------------------------------------------------------

struct ToneScaleParams {
  float n_ = 0, n_r_ = 0, g_ = 0, t_1_ = 0, c_t_ = 0, s_2_ = 0, u_2_ = 0, m_2_ = 0;
  float forward_limit_ = 0, inverse_limit_ = 0, log_peak_ = 0;
};

auto InitToneScaleParams(float peak) -> ToneScaleParams {
  const float n         = peak;
  const float n_r       = 100.0f;
  const float g         = 1.15f;
  const float c         = 0.18f;
  const float c_d       = 10.013f;
  const float w_g       = 0.14f;
  const float t_1       = 0.04f;
  const float r_hit_min = 128.f;
  const float r_hit_max = 896.f;
  const float r_hit =
      r_hit_min + (r_hit_max - r_hit_min) * (std::log(n / n_r) / std::log(10000.f / 100.f));
  const float m_0  = (n / n_r);
  const float m_1  = 0.5f * (m_0 + std::sqrt(m_0 * (m_0 + 4.f * t_1)));
  const float u    = std::pow((r_hit / m_1) / ((r_hit / m_1) + 1.f), g);
  const float m    = m_1 / u;
  const float w_i  = std::log(n / 100.f) / std::log(2.f);
  const float c_t  = c_d / n_r * (1.f + w_i * w_g);
  const float g_ip = 0.5f * (c_t + std::sqrt(c_t * (c_t + 4.f * t_1)));
  const float g_ipp2 =
      -(m_1 * std::pow((g_ip / m), (1.f / g))) / (std::pow(g_ip / m, 1.f / g) - 1.f);
  const float     w_2 = c / g_ipp2;
  const float     s_2 = w_2 * m_1 * kReferenceLuminance;
  const float     u_2 = std::pow((r_hit / m_1) / ((r_hit / m_1) + w_2), g);
  const float     m_2 = m_1 / u_2;
  ToneScaleParams p;
  p.n_             = n;
  p.n_r_           = n_r;
  p.g_             = g;
  p.t_1_           = t_1;
  p.c_t_           = c_t;
  p.s_2_           = s_2;
  p.u_2_           = u_2;
  p.m_2_           = m_2;
  p.forward_limit_ = 8.0f * r_hit;
  p.inverse_limit_ = n / (u_2 * n_r);
  p.log_peak_      = std::log10(n / n_r);
  return p;
}

auto MakeReachMTable(const JmhParams& params, float limit_j_max) -> Table1D {
  Table1D table{};
  for (unsigned i = 0; i < kNominalSize; ++i) {
    const auto      hue            = static_cast<float>(i);
    constexpr float kSearchRange   = 50.f;
    constexpr float kSearchMaximum = 1300.f;
    float           low            = 0.f;
    float           high           = low + kSearchRange;
    bool            outside        = false;
    while (!outside && high < kSearchMaximum) {
      const F3 rgb = JmhToRgb({limit_j_max, high, hue}, params);
      outside      = rgb[0] < 0.f || rgb[1] < 0.f || rgb[2] < 0.f;
      if (!outside) {
        low  = high;
        high = high + kSearchRange;
      }
    }
    while (high - low > 1e-2) {
      const float sample_m = (high + low) / 2.f;
      const F3    rgb      = JmhToRgb({limit_j_max, sample_m, hue}, params);
      outside              = rgb[0] < 0.f || rgb[1] < 0.f || rgb[2] < 0.f;
      if (outside) {
        high = sample_m;
      } else {
        low = sample_m;
      }
    }
    table[i + kBaseIndex] = high;
  }
  table[kLowerWrapIndex]     = table[kLastNominalIndex];
  table[kUpperWrapIndex]     = table[kFirstNominalIndex];
  table[kUpperWrapIndex + 1] = table[kFirstNominalIndex + 1];
  return table;
}

auto UnitCubeCuspCorner(unsigned corner) -> F3 {
  // Order R, Y, G, C, B, M so that hues rotate in order.
  return {static_cast<float>(((corner + 1) % kCuspCornerCount) < 3),
          static_cast<float>(((corner + 5) % kCuspCornerCount) < 3),
          static_cast<float>(((corner + 3) % kCuspCornerCount) < 3)};
}

using CornerTable = std::array<F3, kTotalCornerCount>;

void BuildLimitingCuspCorners(CornerTable& rgb_corners, CornerTable& jmh_corners,
                              const JmhParams& params, float peak) {
  std::array<F3, kCuspCornerCount> temp_rgb{};
  std::array<F3, kCuspCornerCount> temp_jmh{};
  unsigned                         min_index = 0;
  for (unsigned i = 0; i != kCuspCornerCount; ++i) {
    temp_rgb[i] = Scale(peak / kReferenceLuminance, UnitCubeCuspCorner(i));
    temp_jmh[i] = RgbToJmh(temp_rgb[i], params);
    if (temp_jmh[i][2] < temp_jmh[min_index][2]) min_index = i;
  }
  for (unsigned i = 0; i != kCuspCornerCount; ++i) {
    rgb_corners[i + 1] = temp_rgb[(i + min_index) % kCuspCornerCount];
    jmh_corners[i + 1] = temp_jmh[(i + min_index) % kCuspCornerCount];
  }
  rgb_corners[0]                    = rgb_corners[kCuspCornerCount];
  rgb_corners[kCuspCornerCount + 1] = rgb_corners[1];
  jmh_corners[0]                    = jmh_corners[kCuspCornerCount];
  jmh_corners[kCuspCornerCount + 1] = jmh_corners[1];
  jmh_corners[0][2] -= kHueLimit;
  jmh_corners[kCuspCornerCount + 1][2] += kHueLimit;
}

void FindReachCorners(CornerTable& jmh_corners, const JmhParams& params, float limit_j,
                      float maximum_source) {
  std::array<F3, kCuspCornerCount> temp_jmh{};
  const float                      limit_a   = JToAchromatic(limit_j, params.inv_cz_);
  unsigned                         min_index = 0;
  for (unsigned i = 0; i != kCuspCornerCount; ++i) {
    const F3 rgb_vector = UnitCubeCuspCorner(i);
    float    lower      = 0.0f;
    float    upper      = maximum_source;
    while ((upper - lower) > kReachCuspTolerance) {
      const float test = (lower + upper) / 2.f;
      const float A    = RgbToAab(Scale(test, rgb_vector), params)[0];
      if (A < limit_a) {
        lower = test;
      } else {
        upper = test;
      }
      if (A == limit_a) break;
    }
    temp_jmh[i] = RgbToJmh(Scale(upper, rgb_vector), params);
    if (temp_jmh[i][2] < temp_jmh[min_index][2]) min_index = i;
  }
  for (unsigned i = 0; i != kCuspCornerCount; ++i) {
    jmh_corners[i + 1] = temp_jmh[(i + min_index) % kCuspCornerCount];
  }
  jmh_corners[0]                    = jmh_corners[kCuspCornerCount];
  jmh_corners[kCuspCornerCount + 1] = jmh_corners[1];
  jmh_corners[0][2] -= kHueLimit;
  jmh_corners[kCuspCornerCount + 1][2] += kHueLimit;
}

auto ExtractSortedCubeHues(std::array<float, kMaxSortedCorners>& sorted, const CornerTable& reach,
                           const CornerTable& display) -> unsigned {
  unsigned idx = 0, reach_idx = 1, display_idx = 1;
  while (reach_idx < kCuspCornerCount + 1 || display_idx < kCuspCornerCount + 1) {
    const float reach_hue   = reach[reach_idx][2];
    const float display_hue = display[display_idx][2];
    if (reach_hue == display_hue) {
      sorted[idx] = reach_hue;
      ++reach_idx;
      ++display_idx;
    } else if (reach_hue < display_hue) {
      sorted[idx] = reach_hue;
      ++reach_idx;
    } else {
      sorted[idx] = display_hue;
      ++display_idx;
    }
    ++idx;
  }
  return idx;
}

void BuildHueSampleInterval(unsigned samples, float lower, float upper, Table1D& hue_table,
                            unsigned base) {
  const float delta = (upper - lower) / static_cast<float>(samples);
  for (unsigned i = 0; i != samples; ++i) {
    hue_table[base + i] = lower + static_cast<float>(i) * delta;
  }
}

void BuildHueTable(Table1D& hue_table, const std::array<float, kMaxSortedCorners>& sorted,
                   unsigned unique_hues) {
  const float ideal_spacing = static_cast<float>(kNominalSize) / kHueLimit;
  std::array<unsigned, 2 * kCuspCornerCount + 2> samples_count{};
  unsigned                                       last_idx  = std::numeric_limits<unsigned>::max();
  unsigned                                       min_index = sorted[0] == 0.0f ? 0 : 1;
  for (unsigned hue_idx = 0; hue_idx != unique_hues; ++hue_idx) {
    unsigned nominal_idx = std::min(
        std::max(static_cast<unsigned>(std::round(sorted[hue_idx] * ideal_spacing)), min_index),
        kNominalSize - 1);
    if (last_idx == nominal_idx) {
      if (hue_idx > 1 && samples_count[hue_idx - 2] != (samples_count[hue_idx - 1] - 1)) {
        samples_count[hue_idx - 1] = samples_count[hue_idx - 1] - 1;
      } else {
        nominal_idx = nominal_idx + 1;
      }
    }
    samples_count[hue_idx] = std::min(nominal_idx, kNominalSize - 1U);
    last_idx = min_index = nominal_idx;
  }
  unsigned total_samples = 0;
  unsigned i             = 0;
  BuildHueSampleInterval(samples_count[i], 0.0f, sorted[i], hue_table, total_samples + 1);
  total_samples += samples_count[i];
  for (++i; i != unique_hues; ++i) {
    const unsigned samples = samples_count[i] - samples_count[i - 1];
    BuildHueSampleInterval(samples, sorted[i - 1], sorted[i], hue_table, total_samples + 1);
    total_samples += samples;
  }
  BuildHueSampleInterval(kNominalSize - total_samples, sorted[i - 1], kHueLimit, hue_table,
                         total_samples + 1);
  hue_table[kLowerWrapIndex]     = hue_table[kLastNominalIndex] - kHueLimit;
  hue_table[kUpperWrapIndex]     = hue_table[kFirstNominalIndex] + kHueLimit;
  hue_table[kUpperWrapIndex + 1] = hue_table[kFirstNominalIndex + 1] + kHueLimit;
}

auto FindDisplayCuspForHue(float hue, const CornerTable& rgb_corners,
                           const CornerTable& jmh_corners, const JmhParams& params, F2& previous)
    -> F2 {
  unsigned upper_corner = 1;
  for (unsigned i = upper_corner; i != kTotalCornerCount; ++i) {
    if (jmh_corners[i][2] > hue) {
      upper_corner = i;
      break;
    }
  }
  const unsigned lower_corner = upper_corner - 1;
  if (jmh_corners[lower_corner][2] == hue) {
    return {jmh_corners[lower_corner][0], jmh_corners[lower_corner][1]};
  }
  const F3 cusp_lower = rgb_corners[lower_corner];
  const F3 cusp_upper = rgb_corners[upper_corner];
  float    sample_t   = 0.0f;
  float    lower_t    = (static_cast<float>(upper_corner) == previous[0]) ? previous[1] : 0.0f;
  float    upper_t    = 1.0f;
  F3       jmh{};
  while ((upper_t - lower_t) > kDisplayCuspTolerance) {
    sample_t = (lower_t + upper_t) / 2.f;
    jmh      = RgbToJmh(LerpF3(cusp_lower, cusp_upper, sample_t), params);
    if (jmh[2] < jmh_corners[lower_corner][2]) {
      upper_t = sample_t;
    } else if (jmh[2] >= jmh_corners[upper_corner][2]) {
      lower_t = sample_t;
    } else if (jmh[2] > hue) {
      upper_t = sample_t;
    } else {
      lower_t = sample_t;
    }
  }
  sample_t    = (lower_t + upper_t) / 2.f;
  jmh         = RgbToJmh(LerpF3(cusp_lower, cusp_upper, sample_t), params);
  previous[0] = static_cast<float>(upper_corner);
  previous[1] = sample_t;
  return {jmh[0], jmh[1]};
}

auto BuildCuspTable(const Table1D& hue_table, const CornerTable& rgb_corners,
                    const CornerTable& jmh_corners, const JmhParams& params) -> Table3D {
  F2      previous = {0.0f, 0.0f};
  Table3D table{};
  for (unsigned i = kFirstNominalIndex; i != kUpperWrapIndex; ++i) {
    const float hue = hue_table[i];
    const F2    jm  = FindDisplayCuspForHue(hue, rgb_corners, jmh_corners, params, previous);
    table[i]        = {jm[0], jm[1] * (1.f + kSmoothM * kSmoothCusps), hue};
  }
  table[kLowerWrapIndex]     = {table[kLastNominalIndex][0], table[kLastNominalIndex][1],
                                hue_table[kLowerWrapIndex]};
  table[kUpperWrapIndex]     = {table[kFirstNominalIndex][0], table[kFirstNominalIndex][1],
                                hue_table[kUpperWrapIndex]};
  table[kUpperWrapIndex + 1] = {table[kFirstNominalIndex + 1][0], table[kFirstNominalIndex + 1][1],
                                hue_table[kUpperWrapIndex + 1]};
  return table;
}

// ---- Upper hull gamma (CPU forms of the boundary functions, as OCIO's table builder uses) ---

auto GetFocusGain(float J, float analytical_threshold, float limit_j_max, float focus_dist)
    -> float {
  float gain = limit_j_max * focus_dist;
  if (J > analytical_threshold) {
    float adjustment =
        std::log10((limit_j_max - analytical_threshold) / std::max(0.0001f, limit_j_max - J));
    adjustment = adjustment * adjustment + 1.f;
    gain       = gain * adjustment;
  }
  return gain;
}

auto SolveJIntersect(float J, float M, float focus_j, float max_j, float slope_gain) -> float {
  const float m_scaled = M / slope_gain;
  const float a        = m_scaled / focus_j;
  if (J < focus_j) {
    const float b = 1.f - m_scaled;
    const float c = -J;
    return -2.f * c / (b + std::sqrt(b * b - 4.f * a * c));
  }
  const float b = -(1.f + m_scaled + max_j * a);
  const float c = max_j * m_scaled + J;
  return -2.f * c / (b - std::sqrt(b * b - 4.f * a * c));
}

auto SminScaled(float a, float b, float scale_reference) -> float {
  const float s_scaled = kSmoothCusps * scale_reference;
  const float h        = std::max(s_scaled - std::abs(a - b), 0.0f) / s_scaled;
  return std::min(a, b) - h * h * h * s_scaled * (1.f / 6.f);
}

auto CompressionVectorSlope(float intersect_j, float focus_j, float limit_j_max, float slope_gain)
    -> float {
  const float direction = (intersect_j < focus_j) ? intersect_j : (limit_j_max - intersect_j);
  return direction * (intersect_j - focus_j) / (focus_j * slope_gain);
}

auto LineBoundaryIntersectionM(float j_axis_intersect, float slope, float inv_gamma, float j_max,
                               float m_max, float j_reference) -> float {
  const float normalised = j_axis_intersect / j_reference;
  const float shifted    = j_reference * std::pow(normalised, inv_gamma);
  return shifted * m_max / (j_max - slope * m_max);
}

auto FindGamutBoundaryIntersection(const F2& jm_cusp, float j_max, float gamma_top_inv,
                                   float gamma_bottom_inv, float j_intersect_source, float slope,
                                   float j_intersect_cusp) -> float {
  const float lower = LineBoundaryIntersectionM(j_intersect_source, slope, gamma_bottom_inv,
                                                jm_cusp[0], jm_cusp[1], j_intersect_cusp);
  const float upper =
      LineBoundaryIntersectionM(j_max - j_intersect_source, -slope, gamma_top_inv,
                                j_max - jm_cusp[0], jm_cusp[1], j_max - j_intersect_cusp);
  return SminScaled(lower, upper, jm_cusp[1]);
}

auto ComputeFocusJ(float cusp_j, float mid_j, float limit_j_max) -> float {
  return LerpF(cusp_j, mid_j, std::min(1.f, kCuspMidBlend - (cusp_j / limit_j_max)));
}

struct GammaTestData {
  F3    test_jmh_{};
  float j_intersect_source_ = 0.0f;
  float slope_              = 0.0f;
  float j_intersect_cusp_   = 0.0f;
};

auto GenerateGammaTestData(const F2& jm_cusp, float hue, float limit_j_max, float mid_j,
                           float focus_dist) -> std::array<GammaTestData, 5> {
  constexpr std::array<float, 5> kPositions = {0.01f, 0.1f, 0.5f, 0.8f, 0.99f};
  const float analytical_threshold          = LerpF(jm_cusp[0], limit_j_max, kFocusGainBlend);
  const float focus_j                       = ComputeFocusJ(jm_cusp[0], mid_j, limit_j_max);
  std::array<GammaTestData, 5> data{};
  for (std::size_t i = 0; i < data.size(); ++i) {
    const float test_j     = LerpF(jm_cusp[0], limit_j_max, kPositions[i]);
    const float slope_gain = GetFocusGain(test_j, analytical_threshold, limit_j_max, focus_dist);
    const float j_source   = SolveJIntersect(test_j, jm_cusp[1], focus_j, limit_j_max, slope_gain);
    data[i]                = {{test_j, jm_cusp[1], hue},
                              j_source,
                              CompressionVectorSlope(j_source, focus_j, limit_j_max, slope_gain),
                              SolveJIntersect(jm_cusp[0], jm_cusp[1], focus_j, limit_j_max, slope_gain)};
  }
  return data;
}

auto EvaluateGammaFit(const F2& jm_cusp, const std::array<GammaTestData, 5>& data,
                      float top_gamma_inv, float peak, float limit_j_max,
                      float lower_hull_gamma_inv, const JmhParams& limit_params) -> bool {
  const float luminance_limit = peak / kReferenceLuminance;
  for (const auto& test : data) {
    const float m   = FindGamutBoundaryIntersection(jm_cusp, limit_j_max, top_gamma_inv,
                                                    lower_hull_gamma_inv, test.j_intersect_source_,
                                                    test.slope_, test.j_intersect_cusp_);
    const float j   = test.j_intersect_source_ + test.slope_ * m;
    const F3    rgb = JmhToRgb({j, m, test.test_jmh_[2]}, limit_params);
    if (!(rgb[0] > luminance_limit || rgb[1] > luminance_limit || rgb[2] > luminance_limit)) {
      return false;
    }
  }
  return true;
}

/**
 * OCIO make_upper_hull_gamma with a warm start: each hue's bracket starts around the previous
 * hue's gamma instead of stepping up from 0 (the TODO in OCIO's implementation). The fit
 * predicate is monotonic in gamma, so the result stays within the bisection accuracy of OCIO's.
 */
void MakeUpperHullGamma(const Table1D& hue_table, Table3D& cusp_table, float peak,
                        float limit_j_max, float mid_j, float focus_dist,
                        float lower_hull_gamma_inv, const JmhParams& limit_params) {
  float previous_gamma = -1.0f;
  for (unsigned i = kFirstNominalIndex; i != kUpperWrapIndex; ++i) {
    const float hue     = hue_table[i];
    const F2    jm_cusp = {cusp_table[i][0], cusp_table[i][1]};
    const auto  data    = GenerateGammaTestData(jm_cusp, hue, limit_j_max, mid_j, focus_dist);
    const auto  fits    = [&](float gamma) {
      return EvaluateGammaFit(jm_cusp, data, 1.0f / gamma, peak, limit_j_max, lower_hull_gamma_inv,
                                  limit_params);
    };

    float low   = kGammaMinimum;
    float high  = low + kGammaSearchStep;
    bool  found = false;
    if (previous_gamma > 0.0f) {
      constexpr float kWarmWindow = 0.02f;
      low                         = std::max(kGammaMinimum, previous_gamma - kWarmWindow);
      high                        = std::min(kGammaMaximum, previous_gamma + kWarmWindow);
      // Move the bracket until fits(low) is false and fits(high) is true.
      while (low > kGammaMinimum && fits(low)) {
        high = low;
        low  = std::max(kGammaMinimum, low - kGammaSearchStep);
      }
      while (high < kGammaMaximum && !fits(high)) {
        low  = high;
        high = high + kGammaSearchStep;
      }
      found = high < kGammaMaximum || fits(high);
    }
    if (!found) {
      low          = kGammaMinimum;
      high         = low + kGammaSearchStep;
      bool outside = false;
      while (!outside && high < kGammaMaximum) {
        if (!fits(high)) {
          low  = high;
          high = high + kGammaSearchStep;
        } else {
          outside = true;
        }
      }
    }
    while ((high - low) > kGammaAccuracy) {
      const float test = (high + low) / 2.f;
      if (fits(test)) {
        high = test;
      } else {
        low = test;
      }
    }
    previous_gamma   = high;
    cusp_table[i][2] = 1.0f / high;
  }
  cusp_table[kLowerWrapIndex][2]     = cusp_table[kLastNominalIndex][2];
  cusp_table[kUpperWrapIndex][2]     = cusp_table[kFirstNominalIndex][2];
  cusp_table[kUpperWrapIndex + 1][2] = cusp_table[kFirstNominalIndex + 1][2];
}

auto DetermineHueLinearitySearchRange(const Table3D& cusp_table) -> std::array<int, 2> {
  std::array<int, 2> range = {0, 1};
  for (unsigned i = kFirstNominalIndex; i != kUpperWrapIndex; ++i) {
    const unsigned pos   = kFirstNominalIndex + static_cast<unsigned>(cusp_table[i][2]);
    const int      delta = static_cast<int>(i) - static_cast<int>(pos);
    range[0]             = std::min(range[0], delta);
    range[1]             = std::max(range[1], delta + 1);
  }
  return range;
}

// -------------------------------------------------------------------------------------------
// Packing
// -------------------------------------------------------------------------------------------

void PutMatrix(std::vector<float>& packed, int offset, const M33f& m) {
  std::copy(m.begin(), m.end(), packed.begin() + offset);
}

auto PrimaryInside(float x, float y, const std::array<float, 8>& tri) -> bool {
  const double xr = tri[0], yr = tri[1], xg = tri[2], yg = tri[3], xb = tri[4], yb = tri[5];
  const double d  = (yg - yb) * (xr - xb) + (xb - xg) * (yr - yb);
  const double l1 = ((yg - yb) * (x - xb) + (xb - xg) * (y - yb)) / d;
  const double l2 = ((yb - yr) * (x - xb) + (xr - xb) * (y - yb)) / d;
  const double l3 = 1.0 - l1 - l2;
  return l1 >= -1e-4 && l2 >= -1e-4 && l3 >= -1e-4;
}

struct CacheKey {
  std::array<uint32_t, 9> bits_{};
  auto operator<(const CacheKey& other) const -> bool { return bits_ < other.bits_; }
};

auto CacheMutex() -> std::mutex& {
  static std::mutex mutex;
  return mutex;
}

auto Cache() -> std::map<CacheKey, std::shared_ptr<const Aces2InverseRuntime>>& {
  static std::map<CacheKey, std::shared_ptr<const Aces2InverseRuntime>> cache;
  return cache;
}

std::atomic<std::uint64_t> g_build_count{0};

}  // namespace

auto Aces2InverseRuntime::ReachMTable() const -> std::span<const float> {
  return {packed_.data() + ALCEDO_D2A_REACH_TABLE, ALCEDO_D2A_TABLE_SIZE};
}

auto Aces2InverseRuntime::HueTable() const -> std::span<const float> {
  return {packed_.data() + ALCEDO_D2A_HUE_TABLE, ALCEDO_D2A_TABLE_SIZE};
}

auto Aces2InverseRuntime::CuspTable() const -> std::span<const float> {
  return {packed_.data() + ALCEDO_D2A_CUSP_TABLE, 3 * ALCEDO_D2A_TABLE_SIZE};
}

auto SourcePrimariesInsideAp1(const std::array<float, 8>& source_primaries_xy) -> bool {
  for (int i = 0; i < 3; ++i) {
    if (!PrimaryInside(source_primaries_xy[i * 2], source_primaries_xy[i * 2 + 1], kAp1Primaries)) {
      return false;
    }
  }
  return true;
}

auto BuildAces2InverseRuntime(const std::array<float, 8>& source_primaries_xy,
                              float peak_luminance_nits) -> Aces2InverseRuntime {
  if (!std::isfinite(peak_luminance_nits) || peak_luminance_nits <= 0.0f) {
    throw std::invalid_argument("ACES 2.0 inverse: peak luminance must be positive");
  }
  Aces2InverseRuntime runtime;
  runtime.source_primaries_xy_    = source_primaries_xy;
  runtime.peak_luminance_nits_    = peak_luminance_nits;
  runtime.limiting_is_ap1_        = !SourcePrimariesInsideAp1(source_primaries_xy);
  runtime.limiting_primaries_xy_  = runtime.limiting_is_ap1_ ? kAp1Primaries : source_primaries_xy;
  const float     peak            = peak_luminance_nits;

  // OCIO Renderer_ACES_OutputTransform20: input AP0, reach AP1, limiting = output primaries.
  const JmhParams input_params    = InitJmhParams(kAp0Primaries);
  const JmhParams limit_params    = InitJmhParams(runtime.limiting_primaries_xy_);
  const JmhParams reach_params    = InitJmhParams(kAp1Primaries);
  const auto      tonescale       = InitToneScaleParams(peak);
  const float     limit_j_max     = YToJ(peak, input_params);
  const float     model_gamma_inv = 1.f / ModelGamma();
  const Table1D   reach_m         = MakeReachMTable(reach_params, limit_j_max);

  const float     compr =
      kChromaCompress + (kChromaCompress * kChromaCompressFact) * tonescale.log_peak_;
  const float sat =
      std::max(0.2f, kChromaExpand - (kChromaExpand * kChromaExpandFact) * tonescale.log_peak_);
  const float sat_thr               = kChromaExpandThr / tonescale.n_;
  const float chroma_compress_scale = std::pow(0.03379f * peak, 0.30596f) - 0.45135f;

  const float mid_j                 = YToJ(tonescale.c_t_ * kReferenceLuminance, input_params);
  const float focus_dist =
      kFocusDistance + kFocusDistance * kFocusDistanceScaling * tonescale.log_peak_;
  const float lower_hull_gamma_inv = 1.0f / (1.14f + 0.07f * tonescale.log_peak_);

  CornerTable reach_corners{}, limiting_rgb_corners{}, limiting_jmh_corners{};
  std::array<float, kMaxSortedCorners> sorted_hues{};
  FindReachCorners(reach_corners, reach_params, limit_j_max, tonescale.forward_limit_);
  BuildLimitingCuspCorners(limiting_rgb_corners, limiting_jmh_corners, limit_params, peak);
  const unsigned unique_hues =
      ExtractSortedCubeHues(sorted_hues, reach_corners, limiting_jmh_corners);
  Table1D hue_table{};
  BuildHueTable(hue_table, sorted_hues, unique_hues);
  Table3D cusp_table =
      BuildCuspTable(hue_table, limiting_rgb_corners, limiting_jmh_corners, limit_params);
  runtime.hue_linearity_search_range_ = DetermineHueLinearitySearchRange(cusp_table);
  MakeUpperHullGamma(hue_table, cusp_table, peak, limit_j_max, mid_j, focus_dist,
                     lower_hull_gamma_inv, limit_params);

  auto& packed = runtime.packed_;
  packed.assign(ALCEDO_D2A_PACKED_SIZE, 0.0f);
  packed[ALCEDO_D2A_BRANCH] = 0.0f;
  PutMatrix(packed, ALCEDO_D2A_SOURCE_TO_TARGET,
            runtime.limiting_is_ap1_ ? ToFloat(RgbToRgbAdaptedD(source_primaries_xy, kAp1Primaries))
                                     : M33f{1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f});
  PutMatrix(packed, ALCEDO_D2A_LIMIT_RGB_TO_CAM16, limit_params.rgb_to_cam16_c_);
  PutMatrix(packed, ALCEDO_D2A_LIMIT_CONE_TO_AAB, limit_params.cone_to_aab_);
  PutMatrix(packed, ALCEDO_D2A_AP0_AAB_TO_CONE, input_params.aab_to_cone_);
  PutMatrix(packed, ALCEDO_D2A_AP0_CAM16_TO_RGB, input_params.cam16_c_to_rgb_);
  PutMatrix(packed, ALCEDO_D2A_AP0_TO_AP1,
            ToFloat(MulD(InverseD(RgbToXyzD(kAp1Primaries)), RgbToXyzD(kAp0Primaries))));
  packed[ALCEDO_D2A_INPUT_MAX]             = peak / kReferenceLuminance;
  packed[ALCEDO_D2A_AP1_MAX]               = tonescale.forward_limit_;
  packed[ALCEDO_D2A_CZ]                    = input_params.cz_;
  packed[ALCEDO_D2A_INV_CZ]                = input_params.inv_cz_;
  packed[ALCEDO_D2A_AP0_A_W_J]             = input_params.a_w_j_;
  packed[ALCEDO_D2A_AP0_INV_A_W_J]         = input_params.inv_a_w_j_;
  packed[ALCEDO_D2A_AP0_F_L_N]             = input_params.f_l_n_;
  packed[ALCEDO_D2A_TS_INVERSE_LIMIT]      = tonescale.inverse_limit_;
  packed[ALCEDO_D2A_TS_T_1]                = tonescale.t_1_;
  packed[ALCEDO_D2A_TS_S_2]                = tonescale.s_2_;
  packed[ALCEDO_D2A_TS_M_2]                = tonescale.m_2_;
  packed[ALCEDO_D2A_TS_G]                  = tonescale.g_;
  packed[ALCEDO_D2A_LIMIT_J_MAX]           = limit_j_max;
  packed[ALCEDO_D2A_MODEL_GAMMA_INV]       = model_gamma_inv;
  packed[ALCEDO_D2A_SAT]                   = sat;
  packed[ALCEDO_D2A_SAT_THR]               = sat_thr;
  packed[ALCEDO_D2A_COMPR]                 = compr;
  packed[ALCEDO_D2A_CHROMA_COMPRESS_SCALE] = chroma_compress_scale;
  packed[ALCEDO_D2A_MID_J]                 = mid_j;
  packed[ALCEDO_D2A_FOCUS_DIST]            = focus_dist;
  packed[ALCEDO_D2A_LOWER_HULL_GAMMA_INV]  = lower_hull_gamma_inv;
  packed[ALCEDO_D2A_HUE_SEARCH_LO] = static_cast<float>(runtime.hue_linearity_search_range_[0]);
  packed[ALCEDO_D2A_HUE_SEARCH_HI] = static_cast<float>(runtime.hue_linearity_search_range_[1]);
  for (unsigned i = 0; i < kTableSize; ++i) {
    packed[ALCEDO_D2A_REACH_TABLE + i]        = reach_m[i];
    packed[ALCEDO_D2A_HUE_TABLE + i]          = hue_table[i];
    packed[ALCEDO_D2A_CUSP_TABLE + 3 * i]     = cusp_table[i][0];
    packed[ALCEDO_D2A_CUSP_TABLE + 3 * i + 1] = cusp_table[i][1];
    packed[ALCEDO_D2A_CUSP_TABLE + 3 * i + 2] = cusp_table[i][2];
  }
  return runtime;
}

auto ResolveAces2InverseRuntime(const std::array<float, 8>& source_primaries_xy,
                                float                       peak_luminance_nits)
    -> std::shared_ptr<const Aces2InverseRuntime> {
  CacheKey key;
  for (std::size_t i = 0; i < 8; ++i) {
    key.bits_[i] = std::bit_cast<uint32_t>(source_primaries_xy[i]);
  }
  key.bits_[8] = std::bit_cast<uint32_t>(peak_luminance_nits);
  std::lock_guard<std::mutex> lock(CacheMutex());
  auto&                       cache = Cache();
  if (const auto it = cache.find(key); it != cache.end()) {
    return it->second;
  }
  auto runtime = std::make_shared<const Aces2InverseRuntime>(
      BuildAces2InverseRuntime(source_primaries_xy, peak_luminance_nits));
  g_build_count.fetch_add(1, std::memory_order_relaxed);
  cache.emplace(key, runtime);
  return runtime;
}

auto Aces2InverseRuntimeBuildCount() -> std::uint64_t {
  return g_build_count.load(std::memory_order_relaxed);
}

auto PackSceneLinearToAp1(const std::array<float, 8>& source_primaries_xy)
    -> std::array<float, ALCEDO_D2A_SCENE_PACKED_SIZE> {
  std::array<float, ALCEDO_D2A_SCENE_PACKED_SIZE> packed{};
  packed[ALCEDO_D2A_BRANCH] = 1.0f;
  const M33f m              = ToFloat(RgbToRgbAdaptedD(source_primaries_xy, kAp1Primaries));
  std::copy(m.begin(), m.end(), packed.begin() + ALCEDO_D2A_SOURCE_TO_TARGET);
  return packed;
}

}  // namespace alcedo
