//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/color/conversion/Oklab_cvt.hpp"

#include <opencv2/core.hpp>

namespace OklabCvt {

// --- fast LUT for ACEScc -> LinearAP1 ---
namespace {
constexpr int                       ACESCC_LUT_N = 4096;
std::array<float, ACESCC_LUT_N + 1> g_acescc2lin{};
std::once_flag                      g_acescc2lin_once;

static inline float                 ACEScc_to_linear_AP1_exact(float acescc) {
  const float a      = 17.52f;
  const float b      = 9.72f;
  const float thresh = (b - 15.0f) / a;  // (9.72 - 15) / 17.52
  float       lin;
  if (acescc < thresh) {
    lin = (std::pow(2.0f, acescc * a - b) - std::pow(2.0f, -16.0f)) * 2.0f;
  } else {
    lin = std::pow(2.0f, acescc * a - b);
  }
  if (lin > 65504.0f) lin = 65504.0f;
  return lin;
}

static inline void build_acescc2lin_lut() {
  for (int i = 0; i <= ACESCC_LUT_N; ++i) {
    float x         = static_cast<float>(i) / ACESCC_LUT_N;
    g_acescc2lin[i] = ACEScc_to_linear_AP1_exact(x);
  }
}

static inline float ACEScc_to_linear_AP1_fast(float acescc) {
  std::call_once(g_acescc2lin_once, build_acescc2lin_lut);
  if (acescc <= 0.0f) return 0.0f;
  if (acescc >= 1.0f) return g_acescc2lin[ACESCC_LUT_N];
  float f = acescc * ACESCC_LUT_N;
  int   i = static_cast<int>(f);
  float t = f - i;
  return g_acescc2lin[i] * (1.0f - t) + g_acescc2lin[i + 1] * t;
}
}  // namespace

static inline cv::Vec3f ACEScc_to_linearAP1_vec(const cv::Vec3f& acescc_rgb) {
  return {ACEScc_to_linear_AP1_fast(acescc_rgb[0]), ACEScc_to_linear_AP1_fast(acescc_rgb[1]),
          ACEScc_to_linear_AP1_fast(acescc_rgb[2])};
}

static inline float LinearAP1_to_ACEScc(float lin) {
  const float a = 17.52f;
  const float b = 9.72f;

  float       acescc;
  if (lin <= 0.0f) {
    acescc = 0.0f;
  } else if (lin < powf(2.0f, -15.0f)) {
    acescc = (log2f((lin * 0.5f) + powf(2.0f, -16.0f)) + b) / a;
  } else {
    acescc = (log2f(lin) + b) / a;
  }

  if (acescc < 0.0f) acescc = 0.0f;
  if (acescc > 1.0f) acescc = 1.0f;

  return acescc;
}

static inline cv::Vec3f LinearAP1_to_ACEScc_vec(const cv::Vec3f& lin_rgb) {
  return {LinearAP1_to_ACEScc(lin_rgb[0]), LinearAP1_to_ACEScc(lin_rgb[1]),
          LinearAP1_to_ACEScc(lin_rgb[2])};
}
/**
 * @brief Convert a RGB value to Oklab value.
 * Adapted from https://bottosson.github.io/posts/oklab/
 *
 * @param rgb
 * @return Oklab
 */
Oklab LinearRGB2Oklab(const cv::Vec3f& rgb) {
  // RGB to LMS (via linear transformation)
  float r = rgb[0], g = rgb[1], b = rgb[2];

  float l   = 0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b;
  float m   = 0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b;
  float s   = 0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b;

  // Non-linear transformation (cube root)
  float l_  = std::cbrtf(l);
  float m_  = std::cbrtf(m);
  float s_  = std::cbrtf(s);

  // LMS to Oklab
  float L   = 0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_;
  float a   = 1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_;
  float l_b = 0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_;

  return {L, a, l_b};
}

/**
 * @brief Convert a Oklab value back to Oklab value.
 * Adapted from https://bottosson.github.io/posts/oklab/
 *
 * @param rgb
 * @return Oklab
 */
cv::Vec3f Oklab2LinearRGB(const Oklab& lab) {
  float     L = lab.l_, a = lab.a_, b = lab.b_;

  // Oklab to LMS (non-linear)
  float     l_    = L + 0.3963377774f * a + 0.2158037573f * b;
  float     m_    = L - 0.1055613458f * a - 0.0638541728f * b;
  float     s_    = L - 0.0894841775f * a - 1.2914855480f * b;

  // LMS to linear (cube)
  float     l     = l_ * l_ * l_;
  float     m     = m_ * m_ * m_;
  float     s     = s_ * s_ * s_;

  // LMS to RGB
  float     r     = +4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
  float     g     = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
  float     rgb_b = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;

  cv::Vec3f result_linear{r, g, rgb_b};

  return result_linear;
}

Oklab ACESRGB2Oklab(const cv::Vec3f& rgb) {
  // RGB to LMS (via linear transformation)
  cv::Vec3f linear_rgb = ACEScc_to_linearAP1_vec(rgb);
  float     r = linear_rgb[0], g = linear_rgb[1], b = linear_rgb[2];

  float     l   = 0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b;
  float     m   = 0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b;
  float     s   = 0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b;

  // Non-linear transformation (cube root)
  float     l_  = std::cbrtf(l);
  float     m_  = std::cbrtf(m);
  float     s_  = std::cbrtf(s);

  // LMS to Oklab
  float     L   = 0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_;
  float     a   = 1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_;
  float     l_b = 0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_;

  return {L, a, l_b};
}

/**
 * @brief Convert a Oklab value back to Oklab value.
 * Adapted from https://bottosson.github.io/posts/oklab/
 *
 * @param rgb
 * @return Oklab
 */
cv::Vec3f Oklab2ACESRGB(const Oklab& lab) {
  float     L = lab.l_, a = lab.a_, b = lab.b_;

  // Oklab to LMS (non-linear)
  float     l_    = L + 0.3963377774f * a + 0.2158037573f * b;
  float     m_    = L - 0.1055613458f * a - 0.0638541728f * b;
  float     s_    = L - 0.0894841775f * a - 1.2914855480f * b;

  // LMS to linear (cube)
  float     l     = l_ * l_ * l_;
  float     m     = m_ * m_ * m_;
  float     s     = s_ * s_ * s_;

  // LMS to RGB
  float     r     = +4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
  float     g     = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
  float     rgb_b = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;

  cv::Vec3f result_linear{r, g, rgb_b};

  return LinearAP1_to_ACEScc_vec(result_linear);
}

Oklab ACESRGB2Oklab(const alcedo::Pixel& pixel) {
  cv::Vec3f acescc_rgb{pixel.r_, pixel.g_, pixel.b_};
  return ACESRGB2Oklab(acescc_rgb);
}

void Oklab2ACESRGB(const Oklab& lab, alcedo::Pixel& pixel) {
  cv::Vec3f acescc_rgb = Oklab2ACESRGB(lab);
  pixel.r_              = acescc_rgb[0];
  pixel.g_              = acescc_rgb[1];
  pixel.b_              = acescc_rgb[2];
}
};  // namespace OklabCvt