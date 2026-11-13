#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace alcedo::detail {

constexpr int kSharedToneCurvePointCount  = 4;
constexpr int kSharedToneCurveStorageCount = 5;
constexpr float kSharedToneShadowPivot    = 0.25f;
constexpr float kSharedToneHighlightPivot = 0.75f;
constexpr std::array<float, kSharedToneCurvePointCount> kSharedToneCurveX = {
    0.0f, kSharedToneShadowPivot, kSharedToneHighlightPivot, 1.0f};

struct SharedToneCurveDescriptor {
  bool                                             enabled    = false;
  int                                              point_count = kSharedToneCurvePointCount;
  std::array<float, kSharedToneCurveStorageCount>  x          = {
      0.0f, kSharedToneShadowPivot, kSharedToneHighlightPivot, 1.0f, 0.0f};
  std::array<float, kSharedToneCurveStorageCount>  y          = {
      0.0f, kSharedToneShadowPivot, kSharedToneHighlightPivot, 1.0f, 0.0f};
  std::array<float, kSharedToneCurveStorageCount - 1> h      = {
      kSharedToneShadowPivot, kSharedToneHighlightPivot - kSharedToneShadowPivot,
      1.0f - kSharedToneHighlightPivot, 0.0f};
  std::array<float, kSharedToneCurveStorageCount>  m          = {1.0f, 1.0f, 1.0f, 1.0f, 0.0f};
};

inline auto ClampSharedToneSlope(float slope) -> float {
  return std::clamp(slope, 0.05f, 2.85f);
}

inline auto NormalizeSharedToneSlider(float slider_value) -> float {
  return std::clamp(slider_value / 100.0f, -1.0f, 1.0f);
}

inline void ComputeSharedToneTangents(SharedToneCurveDescriptor& curve) {
  if (curve.point_count <= 1) {
    return;
  }

  std::array<float, kSharedToneCurveStorageCount - 1> d = {};
  for (int i = 0; i < curve.point_count - 1; ++i) {
    const float dx = curve.h[i];
    d[i]           = (std::abs(dx) > 1.0e-8f) ? (curve.y[i + 1] - curve.y[i]) / dx : 0.0f;
  }

  curve.m[0]                      = d[0];
  curve.m[curve.point_count - 1]  = d[curve.point_count - 2];
  for (int i = 1; i < curve.point_count - 1; ++i) {
    if (d[i - 1] * d[i] <= 0.0f) {
      curve.m[i] = 0.0f;
    } else {
      curve.m[i] = 0.5f * (d[i - 1] + d[i]);
    }
  }

  for (int i = 0; i < curve.point_count - 1; ++i) {
    if (std::abs(d[i]) <= 1.0e-8f) {
      curve.m[i]     = 0.0f;
      curve.m[i + 1] = 0.0f;
      continue;
    }
    const float a = curve.m[i] / d[i];
    const float b = curve.m[i + 1] / d[i];
    const float s = a * a + b * b;
    if (s > 9.0f) {
      const float tau = 3.0f / std::sqrt(s);
      curve.m[i]      = tau * a * d[i];
      curve.m[i + 1]  = tau * b * d[i];
    }
  }

  for (int i = 0; i < curve.point_count; ++i) {
    curve.m[i] = ClampSharedToneSlope(curve.m[i]);
  }
}

inline auto BuildSharedToneCurve(bool shadows_enabled, float shadows_slider_value,
                                 bool highlights_enabled,
                                 float highlights_slider_value) -> SharedToneCurveDescriptor {
  SharedToneCurveDescriptor curve;

  const float shadow_control =
      shadows_enabled ? NormalizeSharedToneSlider(shadows_slider_value) : 0.0f;
  const float highlight_control =
      highlights_enabled ? NormalizeSharedToneSlider(highlights_slider_value) : 0.0f;

  curve.enabled = shadows_enabled || highlights_enabled;

  constexpr float kShadowPointRange    = 0.1f;
  constexpr float kHighlightPointRange = 0.4f;

  curve.y[1] = std::clamp(kSharedToneShadowPivot + shadow_control * kShadowPointRange, 0.02f, 0.73f);
  float highlight_pull = highlight_control * kHighlightPointRange;
  
  curve.y[3] = std::clamp(1.0f - highlight_pull, 0.85f, 1.3f); 
  const float y2_offset = -highlight_pull * 0.2f;
  curve.y[2] = std::clamp(kSharedToneHighlightPivot - y2_offset, 0.65f, 1.0f);
  ComputeSharedToneTangents(curve);

  return curve;
}

inline auto EvaluateSharedToneCurve(float x, const SharedToneCurveDescriptor& curve) -> float {
  if (curve.point_count <= 0) {
    return x;
  }
  if (curve.point_count == 1) {
    return curve.y[0];
  }
  if (x <= curve.x[0]) {
    return curve.y[0];
  }
  if (x >= curve.x[curve.point_count - 1]) {
    return curve.y[curve.point_count - 1] +
           (x - curve.x[curve.point_count - 1]) * curve.m[curve.point_count - 1];
  }

  int idx = curve.point_count - 2;
  for (int i = 0; i < curve.point_count - 1; ++i) {
    if (x < curve.x[i + 1]) {
      idx = i;
      break;
    }
  }

  const float dx = curve.h[idx];
  if (std::abs(dx) <= 1e-8f) {
    return curve.y[idx];
  }

  const float t   = (x - curve.x[idx]) / dx;
  const float h00 = 2.0f * t * t * t - 3.0f * t * t + 1.0f;
  const float h10 = t * t * t - 2.0f * t * t + t;
  const float h01 = -2.0f * t * t * t + 3.0f * t * t;
  const float h11 = t * t * t - t * t;
  return h00 * curve.y[idx] + h10 * dx * curve.m[idx] + h01 * curve.y[idx + 1] +
         h11 * dx * curve.m[idx + 1];
}

template <typename Params>
inline void StoreSharedToneCurve(const SharedToneCurveDescriptor& curve, Params& params) {
  params.shared_tone_curve_enabled_       = curve.enabled;
  params.shared_tone_curve_ctrl_pts_size_ = curve.point_count;
  for (int i = 0; i < kSharedToneCurveStorageCount; ++i) {
    params.shared_tone_curve_ctrl_pts_x_[i] = 0.0f;
    params.shared_tone_curve_ctrl_pts_y_[i] = 0.0f;
    params.shared_tone_curve_m_[i]          = 0.0f;
    if (i < kSharedToneCurveStorageCount - 1) {
      params.shared_tone_curve_h_[i] = 0.0f;
    }
  }
  for (int i = 0; i < curve.point_count; ++i) {
    params.shared_tone_curve_ctrl_pts_x_[i] = curve.x[i];
    params.shared_tone_curve_ctrl_pts_y_[i] = curve.y[i];
    params.shared_tone_curve_m_[i]          = curve.m[i];
    if (i < curve.point_count - 1) {
      params.shared_tone_curve_h_[i] = curve.h[i];
    }
  }
}

template <typename RGBLike>
inline auto ReconstructFromSharedToneLuma(float mapped_luma, float source_luma,
                                          const RGBLike& source_rgb) -> RGBLike {
  const float delta_r      = source_rgb.x - source_luma;
  const float delta_g      = source_rgb.y - source_luma;
  const float delta_b      = source_rgb.z - source_luma;

  const float max_scale = 1.0f;
  auto        scale     = max_scale;

  if (delta_r < 0.0f) scale = std::min(scale, mapped_luma / -delta_r);
  if (delta_g < 0.0f) scale = std::min(scale, mapped_luma / -delta_g);
  if (delta_b < 0.0f) scale = std::min(scale, mapped_luma / -delta_b);

  scale = std::clamp(scale, 0.0f, max_scale);

  RGBLike out{};
  out.x = mapped_luma + delta_r * scale;
  out.y = mapped_luma + delta_g * scale;
  out.z = mapped_luma + delta_b * scale;
  return out;
}

}  // namespace alcedo::detail
