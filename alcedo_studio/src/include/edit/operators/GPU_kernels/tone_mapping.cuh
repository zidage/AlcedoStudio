//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Highlight/shadow local tone mapping kernels and CUDA stage.
// This file is included from color.cuh after the shared ACEScc/AP1 helpers it uses.

GPU_HD_FUNC float hs_lerp(float a, float b, float t) { return a + (b - a) * t; }

constexpr float kHsAcesccMiddleGray = ::alcedo::local_tone_mapping::kAcesccMiddleGray;
constexpr float kHsAcesccCodePerEv  = ::alcedo::local_tone_mapping::kAcesccCodePerEv;

GPU_FUNC float hs_ap1_intensity(float3 ap1) {
  return 0.27222872f * ap1.x + 0.67408177f * ap1.y + 0.05368952f * ap1.z;
}

GPU_FUNC float hs_log_intensity_from_acescc(float4 px) {
  const float3 ap1 = hls_oklch_acescc_to_ap1(make_float3(px.x, px.y, px.z));
  return hls_oklch_acescc_encode(fmaxf(hs_ap1_intensity(ap1), 1.0e-6f));
}

template <size_t N>
GPU_HD_FUNC float hs_piecewise_linear(const float (&xs)[N], const float (&ys)[N], float x) {
  if (x <= xs[0]) {
    return ys[0];
  }
  if (x >= xs[N - 1]) {
    return ys[N - 1];
  }

  for (size_t i = 0; i + 1 < N; ++i) {
    if (x <= xs[i + 1]) {
      const float span = fmaxf(xs[i + 1] - xs[i], 1.0e-6f);
      const float t    = fminf(fmaxf((x - xs[i]) / span, 0.0f), 1.0f);
      return hs_lerp(ys[i], ys[i + 1], t);
    }
  }
  return ys[N - 1];
}

GPU_HD_FUNC float hs_relative_ev_from_log_intensity(float log_intensity) {
  return (log_intensity - kHsAcesccMiddleGray) / kHsAcesccCodePerEv;
}

GPU_HD_FUNC float hs_shadow_profile_ev(float relative_ev) {
  constexpr float kXs[] = {-9.0f, -7.0f, -5.4f, -4.3f, -3.1f, -2.0f, -0.5f, 1.0f};
  constexpr float kYs[] = {0.02f, 0.35f, 0.82f, 0.98f, 0.72f, 0.42f, 0.08f, 0.0f};
  return hs_piecewise_linear(kXs, kYs, relative_ev);
}

GPU_HD_FUNC float hs_highlight_profile_ev(float relative_ev) {
  constexpr float kXs[] = {-1.0f, 0.0f, 1.2f, 2.8f, 4.5f, 6.5f, 8.0f};
  constexpr float kYs[] = {0.0f, 0.03f, 0.22f, 0.60f, 0.95f, 1.08f, 0.92f};
  return hs_piecewise_linear(kXs, kYs, relative_ev);
}

constexpr float kHsHighlightStrengthScale = ::alcedo::local_tone_mapping::kHighlightStrengthScale;
constexpr float kHsBackendAmountLimit = ::alcedo::local_tone_mapping::kBackendAmountLimit;

GPU_FUNC float hs_read_l_clamped(const float* __restrict src, int x, int y, int width,
                                 int height, size_t pitch_elems) {
  const int clamped_x = min(max(x, 0), width - 1);
  const int clamped_y = min(max(y, 0), height - 1);
  return src[static_cast<size_t>(clamped_y) * pitch_elems + static_cast<size_t>(clamped_x)];
}

GPU_FUNC float hs_bilateral_range_weight(float center_l, float sample_l) {
  constexpr float kDeadbandL = 0.018f;
  constexpr float kSigmaL = 0.075f;
  const float     delta = fmaxf(fabsf(sample_l - center_l) - kDeadbandL, 0.0f);
  const float     normalized = delta / kSigmaL;
  return exp2f(-(normalized * normalized));
}

GPU_HD_FUNC float hs_shadow_region(float reference_l) {
  const float enters_above_black = hls_oklch_smoothstep(0.020f, 0.115f, reference_l);
  const float exits_midtones = 1.0f - hls_oklch_smoothstep(0.405f, 0.670f, reference_l);
  return fminf(fmaxf(enters_above_black * exits_midtones, 0.0f), 1.0f);
}

GPU_HD_FUNC float hs_highlight_region(float reference_l) {
  const float enters_upper_mid = hls_oklch_smoothstep(0.470f, 0.800f, reference_l);
  const float soft_white_tail = 1.0f - 0.08f * hls_oklch_smoothstep(1.100f, 1.720f, reference_l);
  return fminf(fmaxf(enters_upper_mid * soft_white_tail, 0.0f), 1.0f);
}

GPU_HD_FUNC void hs_regions(float reference_l, float shadow_amount, float highlight_amount,
                            float* shadow_region, float* highlight_region) {
  const float raw_shadow = hs_shadow_region(reference_l);
  const float raw_highlight = hs_highlight_region(reference_l);
  const bool  both_active =
      fabsf(shadow_amount) > 1.0e-6f && fabsf(highlight_amount) > 1.0e-6f;
  *shadow_region = both_active ? raw_shadow * (1.0f - 0.65f * raw_highlight) : raw_shadow;
  *highlight_region = both_active ? raw_highlight * (1.0f - 0.30f * raw_shadow) : raw_highlight;
}

GPU_HD_FUNC float hs_shadow_l_transform(float source_l, float amount, float region) {
  const float lift = fmaxf(amount, 0.0f);
  const float darken = fmaxf(-amount, 0.0f);
  const float nonnegative_l = fmaxf(source_l, 0.0f);
  const float lift_shape = nonnegative_l * expf(-nonnegative_l / 0.330f);
  const float lift_toe = hls_oklch_smoothstep(0.018f, 0.105f, nonnegative_l);
  const float lift_headroom = 1.0f - hls_oklch_smoothstep(0.570f, 0.760f, nonnegative_l);
  const float lift_delta = lift * region * lift_toe * lift_headroom * 0.90f * lift_shape;
  const float darken_delta =
      darken * region * 0.24f * nonnegative_l * (1.0f - expf(-nonnegative_l / 0.280f));
  return source_l + lift_delta - darken_delta;
}

GPU_HD_FUNC float hs_highlight_l_transform(float source_l, float amount, float region) {
  const float reduce = fmaxf(amount, 0.0f);
  const float boost = fmaxf(-amount, 0.0f);
  const float nonnegative_l = fmaxf(source_l, 0.0f);
  const float distance = fmaxf(nonnegative_l - 0.555f, 0.0f);
  const float onset = hls_oklch_smoothstep(0.555f, 0.760f, nonnegative_l);
  const float reduce_delta = reduce * region * onset * (0.190f * kHsHighlightStrengthScale) *
                             (1.0f - expf(-distance / 0.310f));
  const float boost_delta = boost * region * onset * 0.155f * (1.0f - expf(-distance / 0.360f));
  return source_l + boost_delta - reduce_delta;
}

GPU_HD_FUNC float hs_apply_reference_curve(float reference_l, float shadow_amount,
                                           float highlight_amount,
                                           float* shadow_region = nullptr,
                                           float* highlight_region = nullptr) {
  const float relative_ev = hs_relative_ev_from_log_intensity(reference_l);
  const float shadow_lift = fmaxf(shadow_amount, 0.0f) * hs_shadow_profile_ev(relative_ev);
  const float shadow_darken =
      fmaxf(-shadow_amount, 0.0f) * 0.55f * hs_shadow_profile_ev(relative_ev);
  const float highlight_reduce =
      fmaxf(highlight_amount, 0.0f) * kHsHighlightStrengthScale *
      hs_highlight_profile_ev(relative_ev);
  const float highlight_boost =
      fmaxf(-highlight_amount, 0.0f) * 0.65f * hs_highlight_profile_ev(relative_ev);
  const float practical_dark =
      hls_oklch_smoothstep(-5.85f, -3.95f, relative_ev) *
      (1.0f - hls_oklch_smoothstep(-3.20f, -1.65f, relative_ev));
  const float fill_plateau =
      hls_oklch_smoothstep(-5.55f, -3.30f, relative_ev) *
      (1.0f - 0.45f * hls_oklch_smoothstep(-2.65f, -0.20f, relative_ev));
  const float deep_toe_fill =
      shadow_lift * (1.0f - hls_oklch_smoothstep(-7.35f, -4.95f, relative_ev)) * 0.28f;
  const float shadow_fill_lift =
      shadow_lift * (0.62f * practical_dark + 0.14f * fill_plateau) + deep_toe_fill;
  const float lifted_relative_ev = relative_ev + 0.24f * (shadow_lift + 0.84f * shadow_fill_lift);
  const float combo_shadow_rollback =
      ((shadow_lift > 1.0e-6f && highlight_reduce > 1.0e-6f) ? 1.0f : 0.0f) * shadow_fill_lift *
      hls_oklch_smoothstep(-2.00f, -0.60f, lifted_relative_ev) *
      (1.0f - hls_oklch_smoothstep(0.10f, 1.30f, lifted_relative_ev)) * 1.08f;
  const float combo_low_mid_darken =
      fminf(shadow_lift + shadow_fill_lift, highlight_reduce) *
      hls_oklch_smoothstep(-2.45f, -0.90f, lifted_relative_ev) *
      (1.0f - hls_oklch_smoothstep(0.50f, 1.95f, lifted_relative_ev)) * 1.30f;

  if (shadow_region != nullptr) {
    *shadow_region =
        fminf(fmaxf((shadow_lift + 0.20f * shadow_fill_lift) / 0.86f, 0.0f), 1.0f);
  }
  if (highlight_region != nullptr) {
    *highlight_region = fminf(fmaxf(highlight_reduce / 1.08f, 0.0f), 1.0f);
  }

  const float delta_ev = shadow_lift + shadow_fill_lift - combo_shadow_rollback -
                         shadow_darken - highlight_reduce - combo_low_mid_darken +
                         highlight_boost;
  return reference_l + delta_ev * kHsAcesccCodePerEv;
}

GPU_HD_FUNC float hs_llf_detail_alpha(float reference_l, float shadow_amount,
                                      float highlight_amount) {
  (void)highlight_amount;
  const float relative_ev = hs_relative_ev_from_log_intensity(reference_l);
  const float deep_shadow =
      1.0f - hls_oklch_smoothstep(-5.7f, -4.1f, relative_ev);
  const float mid_shadow =
      hls_oklch_smoothstep(-5.0f, -3.6f, relative_ev) *
      (1.0f - hls_oklch_smoothstep(-2.4f, -1.0f, relative_ev));
  const float lift_amount = fmaxf(shadow_amount, 0.0f);
  return 1.0f + 0.40f * lift_amount * deep_shadow - 0.14f * lift_amount * mid_shadow;
}

GPU_HD_FUNC float hs_llf_tone_beta(float reference_l, float shadow_amount,
                                    float highlight_amount) {
  constexpr float kEps = ::alcedo::local_tone_mapping::kToneBetaEps;
  const float lo = hs_apply_reference_curve(reference_l - kEps, shadow_amount, highlight_amount);
  const float hi = hs_apply_reference_curve(reference_l + kEps, shadow_amount, highlight_amount);
  return fminf(fmaxf((hi - lo) / (2.0f * kEps),
                     ::alcedo::local_tone_mapping::kToneBetaMin),
               ::alcedo::local_tone_mapping::kToneBetaMax);
}

GPU_FUNC float hs_llf_gamma_interp_t(float gamma_lo, float gamma_hi, float g) {
  const float span = fmaxf(gamma_hi - gamma_lo, 1.0e-6f);
  return fminf(fmaxf((g - gamma_lo) / span, 0.0f), 1.0f);
}

GPU_FUNC float hs_llf_remap_delta(float delta_l, float sigma_r, float alpha, float beta) {
  const float abs_delta = fabsf(delta_l);
  if (abs_delta <= 1.0e-6f) {
    return 0.0f;
  }

  const float sign = copysignf(1.0f, delta_l);
  if (abs_delta <= sigma_r) {
    const float normalized = fminf(fmaxf(abs_delta / fmaxf(sigma_r, 1.0e-6f), 0.0f), 1.0f);
    return sign * sigma_r * powf(normalized, alpha);
  }
  return sign * (sigma_r + beta * (abs_delta - sigma_r));
}

GPU_FUNC float hs_read_plane_clamped(const float* __restrict src, int x, int y, int width,
                                     int height) {
  const int clamped_x = min(max(x, 0), width - 1);
  const int clamped_y = min(max(y, 0), height - 1);
  return src[static_cast<size_t>(clamped_y) * static_cast<size_t>(width) +
             static_cast<size_t>(clamped_x)];
}

GPU_FUNC float hs_pyr_weight_1d(int tap) {
  switch (tap) {
    case -2:
    case 2:
      return 1.0f / 16.0f;
    case -1:
    case 1:
      return 4.0f / 16.0f;
    default:
      return 6.0f / 16.0f;
  }
}

GPU_FUNC float hs_expand_from_coarse(const float* __restrict coarse, int coarse_width,
                                     int coarse_height, int x, int y) {
  float sum = 0.0f;
  for (int ky = -2; ky <= 2; ++ky) {
    const int sample_y = y - ky;
    if ((sample_y & 1) != 0) continue;
    const int cy = min(max(sample_y / 2, 0), coarse_height - 1);
    const float wy = hs_pyr_weight_1d(ky);
    for (int kx = -2; kx <= 2; ++kx) {
      const int sample_x = x - kx;
      if ((sample_x & 1) != 0) continue;
      const int cx = min(max(sample_x / 2, 0), coarse_width - 1);
      const float wx = hs_pyr_weight_1d(kx);
      sum += 4.0f * wx * wy *
             coarse[static_cast<size_t>(cy) * static_cast<size_t>(coarse_width) +
                    static_cast<size_t>(cx)];
    }
  }
  return sum;
}

__global__ void HsCopyThroughKernel(const float4* __restrict src, float4* __restrict dst,
                                    int width, int height, size_t pitch_elems) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y >= height) return;
  const size_t offset = static_cast<size_t>(y) * pitch_elems + static_cast<size_t>(x);
  dst[offset] = src[offset];
}

__global__ void HsExtractLogIntensityKernel(const float4* __restrict src, float* __restrict dst,
                                            int width, int height, size_t src_pitch_elems) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y >= height) return;

  const size_t src_offset = static_cast<size_t>(y) * src_pitch_elems + static_cast<size_t>(x);
  const size_t dst_offset = static_cast<size_t>(y) * static_cast<size_t>(width) +
                            static_cast<size_t>(x);
  dst[dst_offset] = hs_log_intensity_from_acescc(src[src_offset]);
}

GPU_FUNC float4 hs_read_rgba_bilinear(const float4* __restrict src, int width, int height,
                                      size_t pitch_elems, float x, float y) {
  const float clamped_x = fminf(fmaxf(x, 0.0f), static_cast<float>(width - 1));
  const float clamped_y = fminf(fmaxf(y, 0.0f), static_cast<float>(height - 1));
  const int x0 = min(max(static_cast<int>(floorf(clamped_x)), 0), width - 1);
  const int y0 = min(max(static_cast<int>(floorf(clamped_y)), 0), height - 1);
  const int x1 = min(x0 + 1, width - 1);
  const int y1 = min(y0 + 1, height - 1);
  const float tx = clamped_x - static_cast<float>(x0);
  const float ty = clamped_y - static_cast<float>(y0);

  const float4 v00 = src[static_cast<size_t>(y0) * pitch_elems + static_cast<size_t>(x0)];
  const float4 v10 = src[static_cast<size_t>(y0) * pitch_elems + static_cast<size_t>(x1)];
  const float4 v01 = src[static_cast<size_t>(y1) * pitch_elems + static_cast<size_t>(x0)];
  const float4 v11 = src[static_cast<size_t>(y1) * pitch_elems + static_cast<size_t>(x1)];
  const float4 vx0 = make_float4(hs_lerp(v00.x, v10.x, tx), hs_lerp(v00.y, v10.y, tx),
                                 hs_lerp(v00.z, v10.z, tx), hs_lerp(v00.w, v10.w, tx));
  const float4 vx1 = make_float4(hs_lerp(v01.x, v11.x, tx), hs_lerp(v01.y, v11.y, tx),
                                 hs_lerp(v01.z, v11.z, tx), hs_lerp(v01.w, v11.w, tx));
  return make_float4(hs_lerp(vx0.x, vx1.x, ty), hs_lerp(vx0.y, vx1.y, ty),
                     hs_lerp(vx0.z, vx1.z, ty), hs_lerp(vx0.w, vx1.w, ty));
}

__global__ void HsExtractLogIntensityResampledKernel(const float4* __restrict src,
                                                     float* __restrict dst, int src_width,
                                                     int src_height, size_t src_pitch_elems,
                                                     int dst_width, int dst_height) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= dst_width || y >= dst_height) return;

  const float src_x = ((static_cast<float>(x) + 0.5f) * static_cast<float>(src_width) /
                       fmaxf(static_cast<float>(dst_width), 1.0f)) -
                      0.5f;
  const float src_y = ((static_cast<float>(y) + 0.5f) * static_cast<float>(src_height) /
                       fmaxf(static_cast<float>(dst_height), 1.0f)) -
                      0.5f;
  const size_t dst_offset = static_cast<size_t>(y) * static_cast<size_t>(dst_width) +
                            static_cast<size_t>(x);
  dst[dst_offset] =
      hs_log_intensity_from_acescc(hs_read_rgba_bilinear(src, src_width, src_height,
                                                         src_pitch_elems, src_x, src_y));
}

__global__ void HsBuildRemappedSampleKernel(const float* __restrict source_l,
                                            float* __restrict remapped_l, int width, int height,
                                            float gamma, float target, float beta, float alpha,
                                            float sigma_r) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y >= height) return;

  const size_t offset = static_cast<size_t>(y) * static_cast<size_t>(width) +
                        static_cast<size_t>(x);
  const float source_value = source_l[offset];
  remapped_l[offset] =
      target + hs_llf_remap_delta(source_value - gamma, sigma_r, alpha, beta);
}

__global__ void HsPyrDownKernel(const float* __restrict src, int src_width, int src_height,
                                float* __restrict dst, int dst_width, int dst_height) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= dst_width || y >= dst_height) return;

  const int center_x = x * 2;
  const int center_y = y * 2;
  float     sum = 0.0f;
  for (int ky = -2; ky <= 2; ++ky) {
    const float wy = hs_pyr_weight_1d(ky);
    for (int kx = -2; kx <= 2; ++kx) {
      const float wx = hs_pyr_weight_1d(kx);
      sum += wx * wy *
             hs_read_plane_clamped(src, center_x + kx, center_y + ky, src_width, src_height);
    }
  }

  dst[static_cast<size_t>(y) * static_cast<size_t>(dst_width) + static_cast<size_t>(x)] = sum;
}

__global__ void HsSelectInterpolatedLevelKernel(
    const float* __restrict source_level, const float* __restrict sample_lo_level,
    const float* __restrict sample_lo_coarse, const float* __restrict sample_hi_level,
    const float* __restrict sample_hi_coarse, float* __restrict output_level, int width,
    int height, int coarse_width, int coarse_height, float gamma_lo, float gamma_hi,
    bool first_pair, bool last_pair, bool top_level) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y >= height) return;

  const size_t offset =
      static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
  const float  g = source_level[offset];
  const bool   in_interval =
      (first_pair && g <= gamma_hi) || (last_pair && g >= gamma_lo) ||
      (g >= gamma_lo && g < gamma_hi);
  if (!in_interval) {
    return;
  }

  const float t = hs_llf_gamma_interp_t(gamma_lo, gamma_hi, g);
  if (top_level) {
    output_level[offset] = hs_lerp(sample_lo_level[offset], sample_hi_level[offset], t);
    return;
  }

  const float lap_lo = sample_lo_level[offset] -
                       hs_expand_from_coarse(sample_lo_coarse, coarse_width, coarse_height, x, y);
  const float lap_hi = sample_hi_level[offset] -
                       hs_expand_from_coarse(sample_hi_coarse, coarse_width, coarse_height, x, y);
  output_level[offset] = hs_lerp(lap_lo, lap_hi, t);
}

__global__ void HsCollapseLevelKernel(const float* __restrict lap_level,
                                      const float* __restrict coarse_level,
                                      float* __restrict dst_level, int width, int height,
                                      int coarse_width, int coarse_height) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y >= height) return;

  const size_t offset =
      static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
  dst_level[offset] = lap_level[offset] +
                      hs_expand_from_coarse(coarse_level, coarse_width, coarse_height, x, y);
}

GPU_FUNC float4 hs_apply_adjusted_l_pixel(float4 px, float adjusted_l) {
  const float3 source_ap1 = hls_oklch_acescc_to_ap1(make_float3(px.x, px.y, px.z));
  const float  source_intensity = fmaxf(hs_ap1_intensity(source_ap1), 1.0e-5f);
  const float  adjusted_intensity = hls_oklch_acescc_decode(adjusted_l);
  const float  ratio = fminf(fmaxf(adjusted_intensity / source_intensity, 0.0f), 32.0f);
  const float3 ratio_ap1 = make_float3(source_ap1.x * ratio, source_ap1.y * ratio,
                                       source_ap1.z * ratio);
  const float3 neutral_ap1 =
      make_float3(adjusted_intensity, adjusted_intensity, adjusted_intensity);
  const float3 output_ap1 = hls_oklch_fit_ap1_lower_gamut(ratio_ap1, neutral_ap1);
  const float3 output_acescc = hls_oklch_ap1_to_acescc(output_ap1);
  return make_float4(output_acescc.x, output_acescc.y, output_acescc.z, px.w);
}

GPU_FUNC float4 hs_apply_adjusted_l_delta_pixel(float4 px, float reference_l, float adjusted_l) {
  const float source_l = hs_log_intensity_from_acescc(px);
  return hs_apply_adjusted_l_pixel(px, source_l + (adjusted_l - reference_l));
}

GPU_FUNC float hs_read_plane_bilinear(const float* __restrict plane, int width, int height,
                                      size_t pitch_elems, float x, float y) {
  const float clamped_x = fminf(fmaxf(x, 0.0f), static_cast<float>(width - 1));
  const float clamped_y = fminf(fmaxf(y, 0.0f), static_cast<float>(height - 1));
  const int x0 = min(max(static_cast<int>(floorf(clamped_x)), 0), width - 1);
  const int y0 = min(max(static_cast<int>(floorf(clamped_y)), 0), height - 1);
  const int x1 = min(x0 + 1, width - 1);
  const int y1 = min(y0 + 1, height - 1);
  const float tx = clamped_x - static_cast<float>(x0);
  const float ty = clamped_y - static_cast<float>(y0);

  const float v00 = plane[static_cast<size_t>(y0) * pitch_elems + static_cast<size_t>(x0)];
  const float v10 = plane[static_cast<size_t>(y0) * pitch_elems + static_cast<size_t>(x1)];
  const float v01 = plane[static_cast<size_t>(y1) * pitch_elems + static_cast<size_t>(x0)];
  const float v11 = plane[static_cast<size_t>(y1) * pitch_elems + static_cast<size_t>(x1)];
  const float vx0 = hs_lerp(v00, v10, tx);
  const float vx1 = hs_lerp(v01, v11, tx);
  return hs_lerp(vx0, vx1, ty);
}

__global__ void HsApplyAdjustedLKernel(const float4* __restrict src,
                                        const float* __restrict adjusted_l,
                                        float4* __restrict dst, int width, int height,
                                        size_t src_pitch_elems) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y >= height) return;

  const size_t src_offset =
      static_cast<size_t>(y) * src_pitch_elems + static_cast<size_t>(x);
  const size_t l_offset =
      static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
  dst[src_offset] = hs_apply_adjusted_l_pixel(src[src_offset], adjusted_l[l_offset]);
}

__global__ void HsApplyAdjustedLFromFrameKernel(
    const float4* __restrict src, const float* __restrict adjusted_l, float4* __restrict dst,
    int width, int height, size_t src_pitch_elems, int adjusted_width, int adjusted_height,
    size_t adjusted_pitch_elems) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y >= height) return;

  const float adjusted_x =
      ((static_cast<float>(x) + 0.5f) * static_cast<float>(adjusted_width) /
       fmaxf(static_cast<float>(width), 1.0f)) -
      0.5f;
  const float adjusted_y =
      ((static_cast<float>(y) + 0.5f) * static_cast<float>(adjusted_height) /
       fmaxf(static_cast<float>(height), 1.0f)) -
      0.5f;
  const size_t src_offset =
      static_cast<size_t>(y) * src_pitch_elems + static_cast<size_t>(x);
  const float sampled_l = hs_read_plane_bilinear(adjusted_l, adjusted_width, adjusted_height,
                                                  adjusted_pitch_elems, adjusted_x, adjusted_y);
  dst[src_offset] = hs_apply_adjusted_l_pixel(src[src_offset], sampled_l);
}

__global__ void HsApplyAdjustedDeltaLKernel(
    const float4* __restrict src, const float* __restrict reference_l,
    const float* __restrict adjusted_l, float4* __restrict dst, int width, int height,
    size_t src_pitch_elems) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y >= height) return;

  const size_t src_offset =
      static_cast<size_t>(y) * src_pitch_elems + static_cast<size_t>(x);
  const size_t l_offset =
      static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
  dst[src_offset] =
      hs_apply_adjusted_l_delta_pixel(src[src_offset], reference_l[l_offset], adjusted_l[l_offset]);
}

__global__ void HsApplyAdjustedDeltaLFromFrameKernel(
    const float4* __restrict src, const float* __restrict reference_l,
    const float* __restrict adjusted_l, float4* __restrict dst, int width, int height,
    size_t src_pitch_elems, int adjusted_width, int adjusted_height, size_t adjusted_pitch_elems) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y >= height) return;

  const float adjusted_x =
      ((static_cast<float>(x) + 0.5f) * static_cast<float>(adjusted_width) /
       fmaxf(static_cast<float>(width), 1.0f)) -
      0.5f;
  const float adjusted_y =
      ((static_cast<float>(y) + 0.5f) * static_cast<float>(adjusted_height) /
       fmaxf(static_cast<float>(height), 1.0f)) -
      0.5f;
  const size_t src_offset =
      static_cast<size_t>(y) * src_pitch_elems + static_cast<size_t>(x);
  const float sampled_reference = hs_read_plane_bilinear(
      reference_l, adjusted_width, adjusted_height, adjusted_pitch_elems, adjusted_x, adjusted_y);
  const float sampled_adjusted = hs_read_plane_bilinear(
      adjusted_l, adjusted_width, adjusted_height, adjusted_pitch_elems, adjusted_x, adjusted_y);
  dst[src_offset] =
      hs_apply_adjusted_l_delta_pixel(src[src_offset], sampled_reference, sampled_adjusted);
}

__global__ void HsApplyAdjustedDeltaLFromReferenceKernel(
    const float4* __restrict src, const float* __restrict reference_l,
    const float* __restrict adjusted_l, float4* __restrict dst, int width, int height,
    size_t src_pitch_elems, int adjusted_width, int adjusted_height, size_t adjusted_pitch_elems,
    GPUOperatorParams params) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y >= height) return;

  const float reference_width =
      static_cast<float>(max(params.render_roi_reference_width_, 1));
  const float reference_height =
      static_cast<float>(max(params.render_roi_reference_height_, 1));
  const float roi_origin_x =
      params.render_roi_enabled_ ? static_cast<float>(params.render_roi_x_) : 0.0f;
  const float roi_origin_y =
      params.render_roi_enabled_ ? static_cast<float>(params.render_roi_y_) : 0.0f;
  const float roi_width = params.render_roi_enabled_
                              ? fmaxf(params.render_roi_scale_x_ * reference_width, 1.0f)
                              : reference_width;
  const float roi_height = params.render_roi_enabled_
                               ? fmaxf(params.render_roi_scale_y_ * reference_height, 1.0f)
                               : reference_height;
  const float reference_x = roi_origin_x +
                            ((static_cast<float>(x) + 0.5f) * roi_width /
                             fmaxf(static_cast<float>(width), 1.0f)) -
                            0.5f;
  const float reference_y = roi_origin_y +
                            ((static_cast<float>(y) + 0.5f) * roi_height /
                             fmaxf(static_cast<float>(height), 1.0f)) -
                            0.5f;
  const float adjusted_x =
      ((reference_x + 0.5f) * static_cast<float>(adjusted_width) /
       fmaxf(reference_width, 1.0f)) -
      0.5f;
  const float adjusted_y =
      ((reference_y + 0.5f) * static_cast<float>(adjusted_height) /
       fmaxf(reference_height, 1.0f)) -
      0.5f;

  const size_t src_offset =
      static_cast<size_t>(y) * src_pitch_elems + static_cast<size_t>(x);
  const float sampled_reference = hs_read_plane_bilinear(
      reference_l, adjusted_width, adjusted_height, adjusted_pitch_elems, adjusted_x, adjusted_y);
  const float sampled_adjusted = hs_read_plane_bilinear(
      adjusted_l, adjusted_width, adjusted_height, adjusted_pitch_elems, adjusted_x, adjusted_y);
  dst[src_offset] =
      hs_apply_adjusted_l_delta_pixel(src[src_offset], sampled_reference, sampled_adjusted);
}

struct GPU_HighlightShadowLocalToneStage {
  static constexpr int   kMaxLevels   = ::alcedo::local_tone_mapping::kMaxLevels;
  static constexpr float kGammaMinL   = ::alcedo::local_tone_mapping::kGammaMinL;
  static constexpr float kGammaMaxL   = ::alcedo::local_tone_mapping::kGammaMaxL;
  static constexpr float kBaseSigmaR  = ::alcedo::local_tone_mapping::kBaseSigmaR;
  static constexpr float kGammaStepScale = ::alcedo::local_tone_mapping::kGammaStepScale;
  static constexpr float kMinSampleStep = ::alcedo::local_tone_mapping::kMinSampleStep;
  static constexpr int   kReferenceMaskMaxLongEdge =
      ::alcedo::local_tone_mapping::kReferenceMaskMaxLongEdge;

  struct HsLlfSample {
    float gamma  = 0.0f;
    float target = 0.0f;
    float beta   = 1.0f;
    float alpha  = 1.0f;
  };

  std::array<float*, kMaxLevels> source_levels_ = {};
  std::array<float*, kMaxLevels> remap_a_levels_ = {};
  std::array<float*, kMaxLevels> remap_b_levels_ = {};
  std::array<float*, kMaxLevels> output_levels_ = {};
  std::array<int, kMaxLevels>    level_widths_ = {};
  std::array<int, kMaxLevels>    level_heights_ = {};
  int                            level_count_ = 0;
  int                            cached_width_ = 0;
  int                            cached_height_ = 0;
  int                            cached_frame_width_ = 0;
  int                            cached_frame_height_ = 0;
  size_t                         cached_pitch_ = 0;
  std::uint64_t                  cached_source_key_ = 0;
  std::uint64_t                  cached_key_ = 0;
  bool                           cached_reference_base_ = false;

  GPU_HighlightShadowLocalToneStage() = default;

  GPU_HighlightShadowLocalToneStage(const GPU_HighlightShadowLocalToneStage&) {}

  GPU_HighlightShadowLocalToneStage& operator=(const GPU_HighlightShadowLocalToneStage&) {
    ReleaseResources();
    return *this;
  }

  GPU_HighlightShadowLocalToneStage(GPU_HighlightShadowLocalToneStage&& other) noexcept
      : source_levels_(other.source_levels_),
        remap_a_levels_(other.remap_a_levels_),
        remap_b_levels_(other.remap_b_levels_),
        output_levels_(other.output_levels_),
        level_widths_(other.level_widths_),
        level_heights_(other.level_heights_),
        level_count_(other.level_count_),
        cached_width_(other.cached_width_),
        cached_height_(other.cached_height_),
        cached_frame_width_(other.cached_frame_width_),
        cached_frame_height_(other.cached_frame_height_),
        cached_pitch_(other.cached_pitch_),
        cached_source_key_(other.cached_source_key_),
        cached_key_(other.cached_key_),
        cached_reference_base_(other.cached_reference_base_) {
    other.source_levels_.fill(nullptr);
    other.remap_a_levels_.fill(nullptr);
    other.remap_b_levels_.fill(nullptr);
    other.output_levels_.fill(nullptr);
    other.level_widths_.fill(0);
    other.level_heights_.fill(0);
    other.level_count_ = 0;
    other.cached_width_ = 0;
    other.cached_height_ = 0;
    other.cached_frame_width_ = 0;
    other.cached_frame_height_ = 0;
    other.cached_pitch_ = 0;
    other.cached_source_key_ = 0;
    other.cached_key_ = 0;
    other.cached_reference_base_ = false;
  }

  GPU_HighlightShadowLocalToneStage& operator=(GPU_HighlightShadowLocalToneStage&& other) noexcept {
    if (this != &other) {
      ReleaseResources();
      source_levels_ = other.source_levels_;
      remap_a_levels_ = other.remap_a_levels_;
      remap_b_levels_ = other.remap_b_levels_;
      output_levels_ = other.output_levels_;
      level_widths_ = other.level_widths_;
      level_heights_ = other.level_heights_;
      level_count_ = other.level_count_;
      cached_width_ = other.cached_width_;
      cached_height_ = other.cached_height_;
      cached_frame_width_ = other.cached_frame_width_;
      cached_frame_height_ = other.cached_frame_height_;
      cached_pitch_ = other.cached_pitch_;
      cached_source_key_ = other.cached_source_key_;
      cached_key_ = other.cached_key_;
      cached_reference_base_ = other.cached_reference_base_;
      other.source_levels_.fill(nullptr);
      other.remap_a_levels_.fill(nullptr);
      other.remap_b_levels_.fill(nullptr);
      other.output_levels_.fill(nullptr);
      other.level_widths_.fill(0);
      other.level_heights_.fill(0);
      other.level_count_ = 0;
      other.cached_width_ = 0;
      other.cached_height_ = 0;
      other.cached_frame_width_ = 0;
      other.cached_frame_height_ = 0;
      other.cached_pitch_ = 0;
      other.cached_source_key_ = 0;
      other.cached_key_ = 0;
      other.cached_reference_base_ = false;
    }
    return *this;
  }

  ~GPU_HighlightShadowLocalToneStage() { ReleaseResources(); }

  static auto FloatBits(float value) -> std::uint32_t {
    return ::alcedo::local_tone_mapping::FloatBits(value);
  }

  static void HashCombine(std::uint64_t& seed, std::uint64_t value) {
    ::alcedo::local_tone_mapping::HashCombine(seed, value);
  }

  static auto BuildAdjustedResultCacheKey(const GPUOperatorParams& params, float shadow_amount,
                                          float highlight_amount) -> std::uint64_t {
    return ::alcedo::local_tone_mapping::BuildAdjustedResultCacheKey(params, shadow_amount,
                                                                     highlight_amount);
  }

  static auto BuildRoiAdjustedResultCacheKey(const GPUOperatorParams& params,
                                             std::uint64_t base_key) -> std::uint64_t {
    return ::alcedo::local_tone_mapping::BuildRoiAdjustedResultCacheKey(params, base_key);
  }

  static auto CanReuseReferenceForRoi(const GPUOperatorParams& params,
                                      bool reference_source_cache_valid) -> bool {
    return ::alcedo::local_tone_mapping::CanReuseReferenceForRoi(
        params.render_roi_enabled_ && params.render_roi_reference_width_ > 0 &&
            params.render_roi_reference_height_ > 0,
        reference_source_cache_valid, params.render_roi_reference_width_,
        params.render_roi_reference_height_);
  }

  struct MaskDimensions {
    int width = 1;
    int height = 1;
  };

  static auto ComputeMaskDimensions(int width, int height, int max_long_edge)
      -> MaskDimensions {
    const auto dims =
        ::alcedo::local_tone_mapping::ComputeMaskDimensions(width, height, max_long_edge);
    return {dims.width, dims.height};
  }

  static auto GridFor(int width, int height, dim3 block) -> dim3 {
    return dim3((static_cast<unsigned int>(width) + block.x - 1) / block.x,
                (static_cast<unsigned int>(height) + block.y - 1) / block.y);
  }

  static auto ComputeLevelCount(int width, int height, float radius) -> int {
    return ::alcedo::local_tone_mapping::ComputeLevelCount(width, height, radius);
  }

  static auto SigmaR(float shadow_amount, float highlight_amount) -> float {
    return ::alcedo::local_tone_mapping::SigmaR(shadow_amount, highlight_amount);
  }

  static auto BuildSamples(float shadow_amount, float highlight_amount, float sigma_r)
      -> std::vector<HsLlfSample> {
    const float sample_step = std::max(sigma_r * kGammaStepScale, kMinSampleStep);
    const int sample_count =
        std::max(2, static_cast<int>(std::ceil((kGammaMaxL - kGammaMinL) / sample_step)) + 1);
    std::vector<HsLlfSample> samples;
    samples.reserve(static_cast<size_t>(sample_count));
    for (int i = 0; i < sample_count; ++i) {
      const float t =
          (sample_count == 1) ? 0.0f : static_cast<float>(i) / static_cast<float>(sample_count - 1);
      const float gamma = hs_lerp(kGammaMinL, kGammaMaxL, t);
      samples.push_back(
          {gamma,
           ::alcedo::local_tone_mapping::ApplyReferenceCurve(gamma, shadow_amount,
                                                             highlight_amount),
           ::alcedo::local_tone_mapping::ToneBeta(gamma, shadow_amount, highlight_amount),
           ::alcedo::local_tone_mapping::DetailAlpha(gamma, shadow_amount, highlight_amount)});
    }
    return samples;
  }

  void ReleaseResources() {
    for (float*& ptr : source_levels_) {
      if (ptr != nullptr) {
        cudaFree(ptr);
        ptr = nullptr;
      }
    }
    for (float*& ptr : remap_a_levels_) {
      if (ptr != nullptr) {
        cudaFree(ptr);
        ptr = nullptr;
      }
    }
    for (float*& ptr : remap_b_levels_) {
      if (ptr != nullptr) {
        cudaFree(ptr);
        ptr = nullptr;
      }
    }
    for (float*& ptr : output_levels_) {
      if (ptr != nullptr) {
        cudaFree(ptr);
        ptr = nullptr;
      }
    }
    level_widths_.fill(0);
    level_heights_.fill(0);
    level_count_ = 0;
    cached_width_ = 0;
    cached_height_ = 0;
    cached_frame_width_ = 0;
    cached_frame_height_ = 0;
    cached_pitch_ = 0;
    cached_source_key_ = 0;
    cached_key_ = 0;
    cached_reference_base_ = false;
  }

  void EnsurePyramidBuffers(int width, int height, float radius) {
    const int new_level_count = ComputeLevelCount(width, height, radius);
    std::array<int, kMaxLevels> new_widths = {};
    std::array<int, kMaxLevels> new_heights = {};
    new_widths[0] = width;
    new_heights[0] = height;
    for (int level = 1; level < new_level_count; ++level) {
      new_widths[level] = max(1, (new_widths[level - 1] + 1) / 2);
      new_heights[level] = max(1, (new_heights[level - 1] + 1) / 2);
    }

    bool layout_matches = level_count_ == new_level_count;
    for (int level = 0; layout_matches && level < new_level_count; ++level) {
      layout_matches = level_widths_[level] == new_widths[level] &&
                       level_heights_[level] == new_heights[level] &&
                       source_levels_[level] != nullptr && remap_a_levels_[level] != nullptr &&
                       remap_b_levels_[level] != nullptr && output_levels_[level] != nullptr;
    }
    if (layout_matches) {
      return;
    }

    ReleaseResources();
    level_count_ = new_level_count;
    level_widths_ = new_widths;
    level_heights_ = new_heights;
    for (int level = 0; level < level_count_; ++level) {
      const size_t elems =
          static_cast<size_t>(level_widths_[level]) * static_cast<size_t>(level_heights_[level]);
      cudaMalloc(reinterpret_cast<void**>(&source_levels_[level]), elems * sizeof(float));
      cudaMalloc(reinterpret_cast<void**>(&remap_a_levels_[level]), elems * sizeof(float));
      cudaMalloc(reinterpret_cast<void**>(&remap_b_levels_[level]), elems * sizeof(float));
      cudaMalloc(reinterpret_cast<void**>(&output_levels_[level]), elems * sizeof(float));
    }
    cached_width_ = 0;
    cached_height_ = 0;
    cached_frame_width_ = 0;
    cached_frame_height_ = 0;
    cached_pitch_ = 0;
    cached_source_key_ = 0;
    cached_key_ = 0;
    cached_reference_base_ = false;
  }

  void BuildSourcePyramid(const float4* src, int width, int height, size_t src_pitch_elems,
                          dim3 block, cudaStream_t stream) {
    if (level_widths_[0] == width && level_heights_[0] == height) {
      HsExtractLogIntensityKernel<<<GridFor(width, height, block), block, 0, stream>>>(
          src, source_levels_[0], width, height, src_pitch_elems);
    } else {
      HsExtractLogIntensityResampledKernel<<<GridFor(level_widths_[0], level_heights_[0], block),
                                             block, 0, stream>>>(
          src, source_levels_[0], width, height, src_pitch_elems, level_widths_[0],
          level_heights_[0]);
    }
    for (int level = 1; level < level_count_; ++level) {
      HsPyrDownKernel<<<GridFor(level_widths_[level], level_heights_[level], block), block, 0,
                        stream>>>(source_levels_[level - 1], level_widths_[level - 1],
                                  level_heights_[level - 1], source_levels_[level],
                                  level_widths_[level], level_heights_[level]);
    }
  }

  void BuildRemapPyramid(const HsLlfSample& sample, float sigma_r,
                         std::array<float*, kMaxLevels>& remap_levels, dim3 block,
                         cudaStream_t stream) {
    HsBuildRemappedSampleKernel<<<GridFor(level_widths_[0], level_heights_[0], block), block, 0,
                                  stream>>>(source_levels_[0], remap_levels[0], level_widths_[0],
                                            level_heights_[0], sample.gamma, sample.target,
                                            sample.beta, sample.alpha, sigma_r);
    for (int level = 1; level < level_count_; ++level) {
      HsPyrDownKernel<<<GridFor(level_widths_[level], level_heights_[level], block), block, 0,
                        stream>>>(remap_levels[level - 1], level_widths_[level - 1],
                                  level_heights_[level - 1], remap_levels[level],
                                  level_widths_[level], level_heights_[level]);
    }
  }

  void BuildOutputPyramid(const std::vector<HsLlfSample>& samples, float sigma_r, dim3 block,
                          cudaStream_t stream) {
    for (int level = 0; level < level_count_; ++level) {
      cudaMemsetAsync(output_levels_[level], 0,
                      static_cast<size_t>(level_widths_[level]) *
                          static_cast<size_t>(level_heights_[level]) * sizeof(float),
                      stream);
    }

    BuildRemapPyramid(samples.front(), sigma_r, remap_a_levels_, block, stream);
    BuildRemapPyramid(samples[1], sigma_r, remap_b_levels_, block, stream);

    for (size_t pair_index = 0; pair_index + 1 < samples.size(); ++pair_index) {

      for (int level = 0; level < level_count_; ++level) {
        const bool top_level = level == (level_count_ - 1);
        const int coarse_width = top_level ? 1 : level_widths_[level + 1];
        const int coarse_height = top_level ? 1 : level_heights_[level + 1];
        HsSelectInterpolatedLevelKernel<<<GridFor(level_widths_[level], level_heights_[level], block),
                                          block, 0, stream>>>(
            source_levels_[level], remap_a_levels_[level],
            top_level ? nullptr : remap_a_levels_[level + 1], remap_b_levels_[level],
            top_level ? nullptr : remap_b_levels_[level + 1], output_levels_[level],
            level_widths_[level], level_heights_[level], coarse_width, coarse_height,
            samples[pair_index].gamma, samples[pair_index + 1].gamma, pair_index == 0,
            pair_index + 2 == samples.size(), top_level);
      }

      if (pair_index + 2 < samples.size()) {
        std::swap(remap_a_levels_, remap_b_levels_);
        BuildRemapPyramid(samples[pair_index + 2], sigma_r, remap_b_levels_, block, stream);
      }
    }

    for (int level = level_count_ - 2; level >= 0; --level) {
      HsCollapseLevelKernel<<<GridFor(level_widths_[level], level_heights_[level], block), block,
                              0, stream>>>(output_levels_[level], output_levels_[level + 1],
                                           remap_a_levels_[level], level_widths_[level],
                                           level_heights_[level], level_widths_[level + 1],
                                           level_heights_[level + 1]);
      std::swap(output_levels_[level], remap_a_levels_[level]);
    }
  }

  void Dispatch(float4* src, float4* dst, int width, int height, size_t pitch_elems,
                GPUOperatorParams& params, dim3 grid, dim3 block, cudaStream_t stream) {
    const bool active =
        params.hs_local_tone_enabled_ &&
        ((params.shadows_enabled_ && fabsf(params.shadows_offset_) > 1.0e-6f) ||
         (params.highlights_enabled_ && fabsf(params.highlights_offset_) > 1.0e-6f));
    if (!active) {
      HsCopyThroughKernel<<<grid, block, 0, stream>>>(src, dst, width, height, pitch_elems);
      return;
    }

    const float shadow_amount =
        params.shadows_enabled_ ? params.shadows_offset_ : 0.0f;
    const float highlight_amount = params.highlights_enabled_
                                       ? fminf(fmaxf(-params.highlights_offset_,
                                                    -kHsBackendAmountLimit),
                                               kHsBackendAmountLimit)
                                       : 0.0f;
    const std::uint64_t adjusted_cache_key =
        BuildAdjustedResultCacheKey(params, shadow_amount, highlight_amount);
    if (fabsf(shadow_amount) <= 1.0e-6f && fabsf(highlight_amount) <= 1.0e-6f) {
      HsCopyThroughKernel<<<grid, block, 0, stream>>>(src, dst, width, height, pitch_elems);
      return;
    }

    const bool roi_frame_with_source_reference =
        params.render_roi_enabled_ && params.render_roi_reference_width_ > 0 &&
        params.render_roi_reference_height_ > 0;
    const bool preserve_source_detail = params.render_hs_preserve_source_detail_;
    const int reference_max_long_edge =
        max(1, min(params.render_hs_reference_max_long_edge_, kReferenceMaskMaxLongEdge));
    const MaskDimensions current_reference_dims =
        ComputeMaskDimensions(width, height, reference_max_long_edge);
    const std::uint64_t reference_source_cache_key = params.hs_mask_base_cache_key_;
    std::uint64_t reference_cache_key = adjusted_cache_key;
    HashCombine(reference_cache_key, static_cast<std::uint64_t>(preserve_source_detail));
    const bool reference_source_cache_valid =
        cached_reference_base_ && source_levels_[0] != nullptr && cached_source_key_ ==
        reference_source_cache_key && cached_width_ > 0 &&
        cached_height_ > 0 && cached_frame_width_ > 0 && cached_frame_height_ > 0 &&
        cached_pitch_ > 0;
    const bool reference_result_cache_valid =
        reference_source_cache_valid && output_levels_[0] != nullptr &&
        cached_key_ == reference_cache_key;
    const int current_reference_long_edge =
        max(current_reference_dims.width, current_reference_dims.height);
    const int cached_reference_long_edge = max(cached_width_, cached_height_);
    const bool current_can_improve_reference =
        params.render_hs_can_seed_reference_ &&
        current_reference_long_edge > cached_reference_long_edge;
    const auto ensure_reference_output = [&]() {
      if (!reference_result_cache_valid) {
        const float sigma_r = SigmaR(shadow_amount, highlight_amount);
        const auto  samples = BuildSamples(shadow_amount, highlight_amount, sigma_r);
        BuildOutputPyramid(samples, sigma_r, block, stream);
        cached_key_ = reference_cache_key;
      }
    };
    if (CanReuseReferenceForRoi(params, reference_source_cache_valid)) {
      ensure_reference_output();
      HsApplyAdjustedDeltaLFromReferenceKernel<<<grid, block, 0, stream>>>(
          src, source_levels_[0], output_levels_[0], dst, width, height, pitch_elems,
          cached_width_, cached_height_, cached_pitch_, params);
      return;
    }
    if (!roi_frame_with_source_reference && reference_source_cache_valid &&
        !current_can_improve_reference &&
        (cached_frame_width_ != width || cached_frame_height_ != height)) {
      ensure_reference_output();
      HsApplyAdjustedDeltaLFromFrameKernel<<<grid, block, 0, stream>>>(
          src, source_levels_[0], output_levels_[0], dst, width, height, pitch_elems,
          cached_width_, cached_height_, cached_pitch_);
      return;
    }

    const bool build_roi_local_reference = roi_frame_with_source_reference;
    const bool seed_canonical_reference =
        params.render_hs_can_seed_reference_ && !build_roi_local_reference;
    std::uint64_t render_cache_key =
        build_roi_local_reference ? BuildRoiAdjustedResultCacheKey(params, reference_cache_key)
                                  : reference_cache_key;
    const MaskDimensions mask_dims = current_reference_dims;
    EnsurePyramidBuffers(mask_dims.width, mask_dims.height, params.hs_base_radius_);
    const bool cache_valid =
        output_levels_[0] != nullptr && cached_key_ == render_cache_key &&
        cached_frame_width_ == width && cached_frame_height_ == height &&
        cached_width_ == mask_dims.width && cached_height_ == mask_dims.height &&
        cached_pitch_ == static_cast<size_t>(level_widths_[0]);
    if (!cache_valid) {
      const float sigma_r = SigmaR(shadow_amount, highlight_amount);
      const auto  samples = BuildSamples(shadow_amount, highlight_amount, sigma_r);
      BuildSourcePyramid(src, width, height, pitch_elems, block, stream);
      BuildOutputPyramid(samples, sigma_r, block, stream);
      cached_key_ = render_cache_key;
      cached_source_key_ = seed_canonical_reference ? reference_source_cache_key : 0;
      cached_width_ = mask_dims.width;
      cached_height_ = mask_dims.height;
      cached_frame_width_ = width;
      cached_frame_height_ = height;
      cached_pitch_ = static_cast<size_t>(level_widths_[0]);
      cached_reference_base_ = seed_canonical_reference;
    }

    if (cached_width_ == width && cached_height_ == height) {
      HsApplyAdjustedLKernel<<<grid, block, 0, stream>>>(src, output_levels_[0], dst, width, height,
                                                         pitch_elems);
    } else {
      HsApplyAdjustedDeltaLFromFrameKernel<<<grid, block, 0, stream>>>(
          src, source_levels_[0], output_levels_[0], dst, width, height, pitch_elems,
          cached_width_, cached_height_,
          cached_pitch_);
    }
  }
};
