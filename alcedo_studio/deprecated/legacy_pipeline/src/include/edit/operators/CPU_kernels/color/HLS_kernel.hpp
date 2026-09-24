//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <cmath>

#include "edit/operators/op_base.hpp"

namespace alcedo {
namespace hls_kernel {

inline auto WrapHue(float hue) -> float {
  hue = std::fmod(hue, 360.0f);
  if (hue < 0.0f) {
    hue += 360.0f;
  }
  return hue;
}

inline auto HueDistance(float a, float b) -> float {
  const float diff = std::abs(WrapHue(a) - WrapHue(b));
  return std::min(diff, 360.0f - diff);
}

inline auto Smoothstep(float edge0, float edge1, float x) -> float {
  const float denom = std::max(edge1 - edge0, 1e-6f);
  const float t     = std::clamp((x - edge0) / denom, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

inline auto SoftFloor(float x, float floor, float softness) -> float {
  const float t = (x - floor) / std::max(softness, 1e-6f);
  if (t > 20.0f) {
    return x;
  }
  if (t < -20.0f) {
    return floor;
  }
  return floor + softness * std::log1p(std::exp(t));
}

inline auto AcesccDecode(float acescc) -> float {
  constexpr float kLog2Min      = -15.0f;
  constexpr float kLog2Denorm   = -16.0f;
  constexpr float kDenormOffset = 0.00001525878906f;
  constexpr float kA            = 9.72f;
  constexpr float kB            = 17.52f;

  const float encode_floor     = (kLog2Denorm + kA) / kB;
  const float denorm_threshold = (kLog2Min + kA) / kB;
  if (acescc < encode_floor) {
    return acescc - encode_floor;
  }
  if (acescc <= denorm_threshold) {
    return (std::exp2(acescc * kB - kA) - kDenormOffset) * 2.0f;
  }
  return std::exp2(acescc * kB - kA);
}

inline auto AcesccEncode(float linear_ap1) -> float {
  constexpr float kLog2Denorm   = -16.0f;
  constexpr float kDenormTrans  = 0.00003051757812f;
  constexpr float kDenormOffset = 0.00001525878906f;
  constexpr float kA            = 9.72f;
  constexpr float kB            = 17.52f;

  const float encode_floor = (kLog2Denorm + kA) / kB;
  if (linear_ap1 <= 0.0f) {
    return encode_floor + linear_ap1;
  }
  if (linear_ap1 < kDenormTrans) {
    return (std::log2(kDenormOffset + linear_ap1 * 0.5f) + kA) / kB;
  }
  return (std::log2(linear_ap1) + kA) / kB;
}

inline auto Ap1ToOklab(const Pixel& p) -> Pixel {
  const float r = AcesccDecode(p.r_);
  const float g = AcesccDecode(p.g_);
  const float b = AcesccDecode(p.b_);

  const float l = 0.62217537f * r + 0.34268438f * g + 0.02339492f * b;
  const float m = 0.26593478f * r + 0.62930460f * g + 0.10828100f * b;
  const float s = 0.09725037f * r + 0.18525749f * g + 0.77254586f * b;

  const float l_ = std::cbrt(l);
  const float m_ = std::cbrt(m);
  const float s_ = std::cbrt(s);

  return Pixel{0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_,
               1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_,
               0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_, p.a_};
}

inline auto OklabToAp1(float l_value, float a_value, float b_value) -> Pixel {
  const float l_ = l_value + 0.3963377774f * a_value + 0.2158037573f * b_value;
  const float m_ = l_value - 0.1055613458f * a_value - 0.0638541728f * b_value;
  const float s_ = l_value - 0.0894841775f * a_value - 1.2914855480f * b_value;

  const float l = l_ * l_ * l_;
  const float m = m_ * m_ * m_;
  const float s = s_ * s_ * s_;

  return Pixel{2.09085732f * l - 1.16812363f * m + 0.10040848f * s,
               -0.87435428f * l + 2.14592958f * m - 0.27429822f * s,
               -0.05353206f * l - 0.36754978f * m + 1.34755888f * s, 1.0f};
}

inline auto FitAp1LowerGamut(const Pixel& adjusted_ap1, const Pixel& neutral_ap1) -> Pixel {
  constexpr float kLower = -1e-5f;
  float           scale  = 1.0f;

  if (adjusted_ap1.r_ < kLower && neutral_ap1.r_ > adjusted_ap1.r_) {
    scale = std::min(scale, (neutral_ap1.r_ - kLower) / (neutral_ap1.r_ - adjusted_ap1.r_));
  }
  if (adjusted_ap1.g_ < kLower && neutral_ap1.g_ > adjusted_ap1.g_) {
    scale = std::min(scale, (neutral_ap1.g_ - kLower) / (neutral_ap1.g_ - adjusted_ap1.g_));
  }
  if (adjusted_ap1.b_ < kLower && neutral_ap1.b_ > adjusted_ap1.b_) {
    scale = std::min(scale, (neutral_ap1.b_ - kLower) / (neutral_ap1.b_ - adjusted_ap1.b_));
  }

  scale = std::clamp(scale, 0.0f, 1.0f);
  return Pixel{neutral_ap1.r_ + (adjusted_ap1.r_ - neutral_ap1.r_) * scale,
               neutral_ap1.g_ + (adjusted_ap1.g_ - neutral_ap1.g_) * scale,
               neutral_ap1.b_ + (adjusted_ap1.b_ - neutral_ap1.b_) * scale, adjusted_ap1.a_};
}

}  // namespace hls_kernel

struct HLSOpKernel : PointOpTag {
  inline void operator()(Pixel& p, OperatorParams& params) const {
    if (!params.hls_enabled_) return;

    const int profile_count = std::clamp(params.hls_profile_count_, 1, OperatorParams::kHlsProfileCount);
    constexpr float kEps = 1e-6f;
    constexpr float kPi  = 3.14159265358979323846f;

    const Pixel lab    = hls_kernel::Ap1ToOklab(p);
    const float chroma = std::hypot(lab.g_, lab.b_);
    if (chroma <= kEps) {
      return;
    }

    const float hue = hls_kernel::WrapHue(std::atan2(lab.b_, lab.g_) * 180.0f / kPi);
    int         nearest = 0;
    float       nearest_dist =
        hls_kernel::HueDistance(hue, hls_kernel::WrapHue(params.hls_profile_hues_[0]));
    float accum_h = 0.0f;
    float accum_l = 0.0f;
    float accum_c = 0.0f;
    float accum_w = 0.0f;

    for (int i = 0; i < profile_count; ++i) {
      const float target_h = hls_kernel::WrapHue(params.hls_profile_hues_[i]);
      const float hue_dist = hls_kernel::HueDistance(hue, target_h);
      if (hue_dist < nearest_dist) {
        nearest_dist = hue_dist;
        nearest      = i;
      }

      const float width  = std::max(params.hls_profile_hue_ranges_[i], 1.0f);
      const float t      = hue_dist / width;
      const float weight = std::exp2(-(t * t));
      accum_h += params.hls_profile_adjustments_[i][0] * weight;
      accum_l += params.hls_profile_adjustments_[i][1] * weight;
      accum_c += params.hls_profile_adjustments_[i][2] * weight;
      accum_w += weight;
    }

    float adj_h = params.hls_profile_adjustments_[nearest][0];
    float adj_l = params.hls_profile_adjustments_[nearest][1];
    float adj_c = params.hls_profile_adjustments_[nearest][2];
    if (accum_w > kEps) {
      const float inv_w = 1.0f / accum_w;
      adj_h             = accum_h * inv_w;
      adj_l             = accum_l * inv_w;
      adj_c             = accum_c * inv_w;
    }

    if (std::abs(adj_h) <= kEps && std::abs(adj_l) <= kEps && std::abs(adj_c) <= kEps) {
      return;
    }

    const float chroma_confidence    = hls_kernel::Smoothstep(0.005f, 0.030f, chroma);
    const float shadow_confidence    = hls_kernel::Smoothstep(0.005f, 0.050f, lab.r_);
    const float highlight_confidence = 1.0f - hls_kernel::Smoothstep(1.35f, 2.25f, lab.r_);
    const float protection =
        std::clamp(chroma_confidence * shadow_confidence * highlight_confidence, 0.0f, 1.0f);
    if (protection <= kEps) {
      return;
    }

    constexpr float kCurveGain = 2.25f;
    const float adjusted_hue_rad =
        hls_kernel::WrapHue(hue + adj_h * kCurveGain * protection) * (kPi / 180.0f);
    const float adjusted_lightness =
        hls_kernel::SoftFloor(lab.r_ + adj_l * kCurveGain * 0.5f * protection, 0.0f, 0.02f);
    const float chroma_strength = (adj_c >= 0.0f) ? 4.5f : 3.25f;
    const float adjusted_chroma = chroma * std::exp2(adj_c * kCurveGain * chroma_strength * protection);

    const Pixel adjusted_ap1 =
        hls_kernel::OklabToAp1(adjusted_lightness, adjusted_chroma * std::cos(adjusted_hue_rad),
                               adjusted_chroma * std::sin(adjusted_hue_rad));
    const Pixel neutral_ap1 = hls_kernel::OklabToAp1(adjusted_lightness, 0.0f, 0.0f);
    const Pixel fitted_ap1  = hls_kernel::FitAp1LowerGamut(adjusted_ap1, neutral_ap1);

    p.r_ = hls_kernel::AcesccEncode(fitted_ap1.r_);
    p.g_ = hls_kernel::AcesccEncode(fitted_ap1.g_);
    p.b_ = hls_kernel::AcesccEncode(fitted_ap1.b_);
  }
};
};  // namespace alcedo
