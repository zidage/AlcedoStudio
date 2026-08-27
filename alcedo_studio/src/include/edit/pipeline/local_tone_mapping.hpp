//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace alcedo::local_tone_mapping {

constexpr int   kMaxLevels                = 12;
constexpr int   kMaxSamples               = 32;
constexpr int   kReferenceMaskMaxLongEdge = 2048;
constexpr float kPyramidRadius            = 18.0f;
constexpr float kGammaMinL                = -0.15f;
constexpr float kGammaMaxL                = 1.18f;
constexpr float kBaseSigmaR               = 0.07545252f;
constexpr float kGammaStepScale           = 1.35f;
constexpr float kMinSampleStep            = 0.045f;
constexpr float kHighlightStrengthScale   = 1.5f;
constexpr float kBackendAmountLimit       = 1.5f;
constexpr float kAcesccMiddleGray         = 0.41358840f;
constexpr float kAcesccCodePerEv          = 1.0f / 17.52f;
constexpr float kToneBetaEps              = 0.035f;
constexpr float kToneBetaMin              = 0.08f;
constexpr float kToneBetaMax              = 1.70f;

struct LlfSample {
  union {
    float gamma_;
    float gamma;
  };
  union {
    float target_;
    float target;
  };
  union {
    float beta_;
    float beta;
  };
  union {
    float alpha_;
    float alpha;
  };

  constexpr LlfSample(float gamma_value = 0.0f, float target_value = 0.0f, float beta_value = 1.0f,
                      float alpha_value = 1.0f)
      : gamma_(gamma_value), target_(target_value), beta_(beta_value), alpha_(alpha_value) {}
};

struct MaskDimensions {
  union {
    int width_;
    int width;
  };
  union {
    int height_;
    int height;
  };

  constexpr MaskDimensions(int width_value = 1, int height_value = 1)
      : width_(width_value), height_(height_value) {}
};

inline auto FloatBits(float value) -> std::uint32_t {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

inline void HashCombine(std::uint64_t& seed, std::uint64_t value) {
  seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
}

template <typename Params>
inline auto BuildAdjustedResultCacheKey(const Params& params, float shadow_amount,
                                        float highlight_amount) -> std::uint64_t {
  std::uint64_t key = params.hs_mask_base_cache_key_;
  HashCombine(key, static_cast<std::uint64_t>(params.shadows_enabled_));
  HashCombine(key, static_cast<std::uint64_t>(params.highlights_enabled_));
  HashCombine(key, static_cast<std::uint64_t>(FloatBits(shadow_amount)));
  HashCombine(key, static_cast<std::uint64_t>(FloatBits(highlight_amount)));
  return key;
}

template <typename Params>
inline auto BuildRoiAdjustedResultCacheKey(const Params& params, std::uint64_t base_key)
    -> std::uint64_t {
  std::uint64_t key = base_key;
  HashCombine(key, static_cast<std::uint64_t>(params.render_roi_enabled_));
  HashCombine(key, static_cast<std::uint64_t>(params.render_roi_x_));
  HashCombine(key, static_cast<std::uint64_t>(params.render_roi_y_));
  HashCombine(key, static_cast<std::uint64_t>(FloatBits(params.render_roi_scale_x_)));
  HashCombine(key, static_cast<std::uint64_t>(FloatBits(params.render_roi_scale_y_)));
  HashCombine(key, static_cast<std::uint64_t>(params.render_roi_reference_width_));
  HashCombine(key, static_cast<std::uint64_t>(params.render_roi_reference_height_));
  return key;
}

inline auto CanReuseReferenceForRoi(bool roi_frame_with_source_reference,
                                    bool reference_source_cache_valid, int roi_reference_width,
                                    int roi_reference_height) -> bool {
  return roi_frame_with_source_reference && reference_source_cache_valid &&
         roi_reference_width > 0 && roi_reference_height > 0;
}

inline auto ComputeMaskDimensions(int width, int height, int max_long_edge) -> MaskDimensions {
  const float scale = std::min(1.0f, static_cast<float>(std::max(1, max_long_edge)) /
                                         static_cast<float>(std::max(width, height)));
  return {std::max(1, static_cast<int>(std::ceil(static_cast<float>(width) * scale))),
          std::max(1, static_cast<int>(std::ceil(static_cast<float>(height) * scale)))};
}

inline auto ComputeLevelCount(int width, int height, float radius) -> int {
  const int radius_levels = std::max(
      3, std::min(kMaxLevels, static_cast<int>(std::ceil(std::log2(std::max(radius, 1.0f)))) + 2));
  int count = 1;
  int w     = width;
  int h     = height;
  while (count < radius_levels && (w > 1 || h > 1)) {
    w = std::max(1, (w + 1) / 2);
    h = std::max(1, (h + 1) / 2);
    ++count;
  }
  return count;
}

inline auto AlignUpBytes(std::size_t value, std::size_t alignment) -> std::size_t {
  return (value + alignment - 1) & ~(alignment - 1);
}

/** @brief Byte size of one Gaussian pyramid at the canonical LLF mask resolution. */
inline auto EstimatePyramidBytes(int width, int height, float radius, std::size_t alignment)
    -> std::size_t {
  const auto  dims  = ComputeMaskDimensions(width, height, kReferenceMaskMaxLongEdge);
  const int   count = ComputeLevelCount(dims.width, dims.height, radius);
  std::size_t total = 0;
  int         w     = dims.width;
  int         h     = dims.height;
  for (int level = 0; level < count; ++level) {
    total += AlignUpBytes(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * sizeof(float),
                          alignment);
    w = std::max(1, (w + 1) / 2);
    h = std::max(1, (h + 1) / 2);
  }
  return total;
}

/** @brief Peak transient bytes for source, remap A/B, and result pyramids. */
inline auto EstimateLlfTransientBytes(int width, int height, std::size_t alignment) -> std::size_t {
  return 4U * EstimatePyramidBytes(width, height, kPyramidRadius, alignment);
}

inline auto SigmaR(float shadow_amount, float highlight_amount) -> float {
  (void)shadow_amount;
  (void)highlight_amount;
  return kBaseSigmaR;
}

inline auto Lerp(float a, float b, float t) -> float { return a + (b - a) * t; }

inline auto Segment(float x, float x0, float y0, float x1, float y1) -> float {
  const float t = std::clamp((x - x0) / std::max(x1 - x0, 1.0e-6f), 0.0f, 1.0f);
  return Lerp(y0, y1, t);
}

inline auto ShadowProfileEv(float relative_ev) -> float {
  if (relative_ev <= -9.0f) return 0.02f;
  if (relative_ev <= -7.0f) return Segment(relative_ev, -9.0f, 0.02f, -7.0f, 0.35f);
  if (relative_ev <= -5.4f) return Segment(relative_ev, -7.0f, 0.35f, -5.4f, 0.82f);
  if (relative_ev <= -4.3f) return Segment(relative_ev, -5.4f, 0.82f, -4.3f, 0.98f);
  if (relative_ev <= -3.1f) return Segment(relative_ev, -4.3f, 0.98f, -3.1f, 0.72f);
  if (relative_ev <= -2.0f) return Segment(relative_ev, -3.1f, 0.72f, -2.0f, 0.42f);
  if (relative_ev <= -0.5f) return Segment(relative_ev, -2.0f, 0.42f, -0.5f, 0.08f);
  if (relative_ev <= 1.0f) return Segment(relative_ev, -0.5f, 0.08f, 1.0f, 0.0f);
  return 0.0f;
}

inline auto HighlightProfileEv(float relative_ev) -> float {
  if (relative_ev <= -1.0f) return 0.0f;
  if (relative_ev <= 0.0f) return Segment(relative_ev, -1.0f, 0.0f, 0.0f, 0.03f);
  if (relative_ev <= 1.2f) return Segment(relative_ev, 0.0f, 0.03f, 1.2f, 0.22f);
  if (relative_ev <= 2.8f) return Segment(relative_ev, 1.2f, 0.22f, 2.8f, 0.60f);
  if (relative_ev <= 4.5f) return Segment(relative_ev, 2.8f, 0.60f, 4.5f, 0.95f);
  if (relative_ev <= 6.5f) return Segment(relative_ev, 4.5f, 0.95f, 6.5f, 1.08f);
  if (relative_ev <= 8.0f) return Segment(relative_ev, 6.5f, 1.08f, 8.0f, 0.92f);
  return 0.92f;
}

inline auto Smoothstep(float edge0, float edge1, float x) -> float {
  const float t = std::clamp((x - edge0) / std::max(edge1 - edge0, 1.0e-6f), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

inline auto RelativeEv(float log_intensity) -> float {
  return (log_intensity - kAcesccMiddleGray) / kAcesccCodePerEv;
}

inline auto ApplyReferenceCurve(float reference_l, float shadow_amount, float highlight_amount)
    -> float {
  const float relative_ev   = RelativeEv(reference_l);
  const float shadow_lift   = std::max(shadow_amount, 0.0f) * ShadowProfileEv(relative_ev);
  const float shadow_darken = std::max(-shadow_amount, 0.0f) * 0.55f * ShadowProfileEv(relative_ev);
  const float highlight_reduce =
      std::max(highlight_amount, 0.0f) * kHighlightStrengthScale * HighlightProfileEv(relative_ev);
  const float highlight_boost =
      std::max(-highlight_amount, 0.0f) * 0.65f * HighlightProfileEv(relative_ev);
  const float practical_dark =
      Smoothstep(-5.85f, -3.95f, relative_ev) * (1.0f - Smoothstep(-3.20f, -1.65f, relative_ev));
  const float fill_plateau = Smoothstep(-5.55f, -3.30f, relative_ev) *
                             (1.0f - 0.45f * Smoothstep(-2.65f, -0.20f, relative_ev));
  const float deep_toe_fill =
      shadow_lift * (1.0f - Smoothstep(-7.35f, -4.95f, relative_ev)) * 0.28f;
  const float shadow_fill_lift =
      shadow_lift * (0.62f * practical_dark + 0.14f * fill_plateau) + deep_toe_fill;
  const float lifted_relative_ev = relative_ev + 0.24f * (shadow_lift + 0.84f * shadow_fill_lift);
  const float combo_shadow_rollback =
      ((shadow_lift > 1.0e-6f && highlight_reduce > 1.0e-6f) ? 1.0f : 0.0f) * shadow_fill_lift *
      Smoothstep(-2.00f, -0.60f, lifted_relative_ev) *
      (1.0f - Smoothstep(0.10f, 1.30f, lifted_relative_ev)) * 1.08f;
  const float combo_low_mid_darken = std::min(shadow_lift + shadow_fill_lift, highlight_reduce) *
                                     Smoothstep(-2.45f, -0.90f, lifted_relative_ev) *
                                     (1.0f - Smoothstep(0.50f, 1.95f, lifted_relative_ev)) * 1.30f;
  const float delta_ev = shadow_lift + shadow_fill_lift - combo_shadow_rollback - shadow_darken -
                         highlight_reduce - combo_low_mid_darken + highlight_boost;
  return reference_l + delta_ev * kAcesccCodePerEv;
}

inline auto DetailAlpha(float reference_l, float shadow_amount, float highlight_amount) -> float {
  (void)highlight_amount;
  const float relative_ev = RelativeEv(reference_l);
  const float deep_shadow = 1.0f - Smoothstep(-5.7f, -4.1f, relative_ev);
  const float mid_shadow =
      Smoothstep(-5.0f, -3.6f, relative_ev) * (1.0f - Smoothstep(-2.4f, -1.0f, relative_ev));
  const float lift_amount = std::max(shadow_amount, 0.0f);
  return 1.0f + 0.40f * lift_amount * deep_shadow - 0.14f * lift_amount * mid_shadow;
}

inline auto ToneBeta(float reference_l, float shadow_amount, float highlight_amount) -> float {
  const float lo = ApplyReferenceCurve(reference_l - kToneBetaEps, shadow_amount, highlight_amount);
  const float hi = ApplyReferenceCurve(reference_l + kToneBetaEps, shadow_amount, highlight_amount);
  return std::clamp((hi - lo) / (2.0f * kToneBetaEps), kToneBetaMin, kToneBetaMax);
}

inline auto BuildSamples(float shadow_amount, float highlight_amount) -> std::vector<LlfSample> {
  const float sigma_r     = SigmaR(shadow_amount, highlight_amount);
  const float sample_step = std::max(sigma_r * kGammaStepScale, kMinSampleStep);
  const int   sample_count =
      std::max(2, static_cast<int>(std::ceil((kGammaMaxL - kGammaMinL) / sample_step)) + 1);
  std::vector<LlfSample> samples;
  samples.reserve(static_cast<size_t>(sample_count));
  for (int i = 0; i < sample_count; ++i) {
    const float t =
        sample_count == 1 ? 0.0f : static_cast<float>(i) / static_cast<float>(sample_count - 1);
    const float gamma = Lerp(kGammaMinL, kGammaMaxL, t);
    samples.push_back({gamma, ApplyReferenceCurve(gamma, shadow_amount, highlight_amount),
                       ToneBeta(gamma, shadow_amount, highlight_amount),
                       DetailAlpha(gamma, shadow_amount, highlight_amount)});
  }
  return samples;
}

inline auto ShouldRun(float shadow_amount, float highlight_amount) -> bool {
  return std::abs(shadow_amount) > 1.0e-6f || std::abs(highlight_amount) > 1.0e-6f;
}

}  // namespace alcedo::local_tone_mapping
