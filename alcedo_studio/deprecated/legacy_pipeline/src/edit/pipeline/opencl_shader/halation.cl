//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifndef ALCEDO_OPENCL_EDIT_PIPELINE_HALATION_CL
#define ALCEDO_OPENCL_EDIT_PIPELINE_HALATION_CL

// === Halation helpers ==========================================================

static inline float opencl_detail_moncurve_fwd(float x, float gamma, float offs) {
  const float fs = ((gamma - 1.0f) / offs) *
                   pow(offs * gamma / ((gamma - 1.0f) * (1.0f + offs)), gamma);
  const float xb = offs / (gamma - 1.0f);
  return (x >= xb) ? pow((x + offs) / (1.0f + offs), gamma) : x * fs;
}

static inline float opencl_detail_moncurve_inv(float y, float gamma, float offs) {
  const float yb = pow(offs * gamma / ((gamma - 1.0f) * (1.0f + offs)), gamma);
  const float rs = pow((gamma - 1.0f) / offs, gamma - 1.0f) *
                   pow((1.0f + offs) / gamma, gamma);
  return (y >= yb) ? (1.0f + offs) * pow(y, 1.0f / gamma) - offs : y * rs;
}

static inline float opencl_detail_st2084_to_y(float n) {
  const float pq_m1 = 0.1593017578125f;
  const float pq_m2 = 78.84375f;
  const float pq_c1 = 0.8359375f;
  const float pq_c2 = 18.8515625f;
  const float pq_c3 = 18.6875f;
  const float pq_c = 10000.0f;
  const float np = pow(n, 1.0f / pq_m2);
  float l = fmax(np - pq_c1, 0.0f);
  l = l / (pq_c2 - pq_c3 * np);
  return pow(l, 1.0f / pq_m1) * pq_c;
}

static inline float opencl_detail_y_to_st2084(float c) {
  const float pq_m1 = 0.1593017578125f;
  const float pq_m2 = 78.84375f;
  const float pq_c1 = 0.8359375f;
  const float pq_c2 = 18.8515625f;
  const float pq_c3 = 18.6875f;
  const float pq_c = 10000.0f;
  const float l = c / pq_c;
  const float lm = pow(l, pq_m1);
  const float n = (pq_c1 + pq_c2 * lm) / (1.0f + pq_c3 * lm);
  return pow(n, pq_m2);
}

static inline float3 opencl_detail_hlg_to_display_linear_1000nits(float3 hlg_signal) {
  const float a = 0.17883277f;
  const float b = 0.28466892f;
  const float c = 0.55991073f;
  float3 rgb;
  rgb.x = (hlg_signal.x <= 0.5f) ? (hlg_signal.x * hlg_signal.x) / 3.0f
                                 : (exp((hlg_signal.x - c) / a) + b) / 12.0f;
  rgb.y = (hlg_signal.y <= 0.5f) ? (hlg_signal.y * hlg_signal.y) / 3.0f
                                 : (exp((hlg_signal.y - c) / a) + b) / 12.0f;
  rgb.z = (hlg_signal.z <= 0.5f) ? (hlg_signal.z * hlg_signal.z) / 3.0f
                                 : (exp((hlg_signal.z - c) / a) + b) / 12.0f;

  const float ys = 0.2627f * rgb.x + 0.6780f * rgb.y + 0.0593f * rgb.z;
  if (ys > 0.0f) {
    rgb *= pow(ys, 1.2f - 1.0f);
  }
  return rgb;
}

static inline float3 opencl_detail_hlg_from_display_linear_1000nits(float3 display_linear) {
  float y_d = 0.2627f * display_linear.x + 0.6780f * display_linear.y +
              0.0593f * display_linear.z;
  float3 rgb = display_linear;
  if (y_d > 0.0f) {
    rgb *= pow(y_d, (1.0f - 1.2f) / 1.2f);
  }

  const float a = 0.17883277f;
  const float b = 0.28466892f;
  const float c = 0.55991073f;
  rgb.x = (rgb.x <= (1.0f / 12.0f)) ? sqrt(3.0f * rgb.x) : a * log(12.0f * rgb.x - b) + c;
  rgb.y = (rgb.y <= (1.0f / 12.0f)) ? sqrt(3.0f * rgb.y) : a * log(12.0f * rgb.y - b) + c;
  rgb.z = (rgb.z <= (1.0f / 12.0f)) ? sqrt(3.0f * rgb.z) : a * log(12.0f * rgb.z - b) + c;
  return rgb;
}

static inline float3 opencl_detail_eotf(float3 rgb_cv, int eotf_type) {
  if (eotf_type == 0) return rgb_cv;
  if (eotf_type == 1) {
    return (float3)(opencl_detail_st2084_to_y(rgb_cv.x), opencl_detail_st2084_to_y(rgb_cv.y),
                    opencl_detail_st2084_to_y(rgb_cv.z)) *
           (1.0f / 100.0f);
  }
  if (eotf_type == 2) return opencl_detail_hlg_to_display_linear_1000nits(rgb_cv);
  if (eotf_type == 4) return pow(rgb_cv, (float3)(2.6f));
  if (eotf_type == 3) return pow(rgb_cv, (float3)(2.6f));
  if (eotf_type == 5) return pow(rgb_cv, (float3)(2.2f));
  if (eotf_type == 6) return pow(rgb_cv, (float3)(1.8f));
  return (float3)(opencl_detail_moncurve_fwd(rgb_cv.x, 2.4f, 0.055f),
                  opencl_detail_moncurve_fwd(rgb_cv.y, 2.4f, 0.055f),
                  opencl_detail_moncurve_fwd(rgb_cv.z, 2.4f, 0.055f));
}

static inline float3 opencl_detail_eotf_inv(float3 rgb_linear_in, int eotf_type) {
  float3 rgb = fmax(rgb_linear_in, (float3)(0.0f));
  if (eotf_type == 0) return rgb;
  if (eotf_type == 1) {
    return (float3)(opencl_detail_y_to_st2084(rgb.x), opencl_detail_y_to_st2084(rgb.y),
                    opencl_detail_y_to_st2084(rgb.z));
  }
  if (eotf_type == 2) return opencl_detail_hlg_from_display_linear_1000nits(rgb);
  if (eotf_type == 4) {
    return (float3)(opencl_detail_moncurve_inv(rgb.x, 2.4f, 0.055f),
                    opencl_detail_moncurve_inv(rgb.y, 2.4f, 0.055f),
                    opencl_detail_moncurve_inv(rgb.z, 2.4f, 0.055f));
  }
  if (eotf_type == 3) return pow(rgb, (float3)(1.0f / 2.6f));
  if (eotf_type == 5) return pow(rgb, (float3)(1.0f / 2.2f));
  if (eotf_type == 6) return pow(rgb, (float3)(1.0f / 1.8f));
  return (float3)(opencl_detail_moncurve_inv(rgb.x, 2.4f, 0.055f),
                  opencl_detail_moncurve_inv(rgb.y, 2.4f, 0.055f),
                  opencl_detail_moncurve_inv(rgb.z, 2.4f, 0.055f));
}

static inline float3 opencl_halation_decode_display_linear(
    float4 pixel, __global const OpenClNeighborStageParams* params) {
  return opencl_detail_eotf(
      (float3)(fmax(pixel.x, 0.0f), fmax(pixel.y, 0.0f), fmax(pixel.z, 0.0f)),
      params->eotf_);
}

static inline float opencl_halation_blur_radius(float sigma) {
  if (!(sigma > 0.0f)) return 0.0f;
  return (float)clamp((int)ceil(sigma * 3.0f), 1, ALCEDO_OPENCL_NEIGHBOR_MAX_TAP_COUNT - 1);
}

static inline float opencl_halation_weight(int tap, float sigma) {
  return (tap == 0) ? 1.0f : exp(-((float)tap) / fmax(sigma, 1.0e-6f));
}

static inline float opencl_halation_weight_norm(int radius, float sigma) {
  float sum = 1.0f;
  for (int tap = 1; tap <= radius; ++tap) {
    sum += 2.0f * opencl_halation_weight(tap, sigma);
  }
  return 1.0f / fmax(sum, 1.0e-6f);
}

static inline float4 opencl_halation_blur_horizontal(
    __global const float4* src, int x, int y, int width, int height,
    __global const OpenClNeighborStageParams* params) {
  const float sigma = params->sigma_x_;
  const int radius = (int)opencl_halation_blur_radius(sigma);
  const float norm = opencl_halation_weight_norm(radius, sigma);

  const float4 center = opencl_detail_read_clamped(src, x, y, width, height);
  const float3 center_linear = opencl_halation_decode_display_linear(center, params);
  float4 blur = (float4)(center_linear.x * norm, center_linear.y * norm,
                         center_linear.z * norm, center.w);

  for (int tap = 1; tap <= radius; ++tap) {
    const float weight = opencl_halation_weight(tap, sigma) * norm;
    const float4 left = opencl_detail_read_clamped(src, x - tap, y, width, height);
    const float4 right = opencl_detail_read_clamped(src, x + tap, y, width, height);
    const float3 left_linear = opencl_halation_decode_display_linear(left, params);
    const float3 right_linear = opencl_halation_decode_display_linear(right, params);
    blur.x += (left_linear.x + right_linear.x) * weight;
    blur.y += (left_linear.y + right_linear.y) * weight;
    blur.z += (left_linear.z + right_linear.z) * weight;
  }

  return blur;
}

static inline float4 opencl_halation_blur_vertical(
    __global const float4* src, int x, int y, int width, int height,
    __global const OpenClNeighborStageParams* params) {
  const float sigma = params->sigma_y_;
  const int radius = (int)opencl_halation_blur_radius(sigma);
  const float norm = opencl_halation_weight_norm(radius, sigma);
  float4 blur = opencl_detail_read_clamped(src, x, y, width, height);
  blur.x *= norm;
  blur.y *= norm;
  blur.z *= norm;

  for (int tap = 1; tap <= radius; ++tap) {
    const float weight = opencl_halation_weight(tap, sigma) * norm;
    const float4 top = opencl_detail_read_clamped(src, x, y - tap, width, height);
    const float4 bottom = opencl_detail_read_clamped(src, x, y + tap, width, height);
    blur.x += (top.x + bottom.x) * weight;
    blur.y += (top.y + bottom.y) * weight;
    blur.z += (top.z + bottom.z) * weight;
  }
  return blur;
}

static inline float4 opencl_apply_halation(float4 px, float4 blur,
                                           __global const OpenClNeighborStageParams* params) {
  if (params->enabled_ == 0u || !(params->amount_ > 0.0f)) {
    return px;
  }

  const float3 original_linear = opencl_halation_decode_display_linear(px, params);
  const float3 spill_linear = (float3)(fmax(blur.x - original_linear.x, 0.0f),
                                       fmax(blur.y - original_linear.y, 0.0f),
                                       fmax(blur.z - original_linear.z, 0.0f));
  const float3 result_linear =
      original_linear + spill_linear *
                            (float3)(params->amount_ * params->redshift_[0],
                                     params->amount_ * params->redshift_[1],
                                     params->amount_ * params->redshift_[2]);
  const float3 encoded = opencl_detail_eotf_inv(result_linear, params->eotf_);
  return (float4)(encoded.x, encoded.y, encoded.z, px.w);
}

#endif  // ALCEDO_OPENCL_EDIT_PIPELINE_HALATION_CL
