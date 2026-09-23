//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifndef ALCEDO_OPENCL_COMMON_CL
#define ALCEDO_OPENCL_COMMON_CL

// === ACEScc encoding constants ================================================

#define ALCEDO_OPENCL_ACESCC_LOG2_MIN       (-15.0f)
#define ALCEDO_OPENCL_ACESCC_LOG2_DENORM    (-16.0f)
#define ALCEDO_OPENCL_ACESCC_DENORM_TRANS   0.00003051757812f
#define ALCEDO_OPENCL_ACESCC_DENORM_OFFSET  0.00001525878906f
#define ALCEDO_OPENCL_ACESCC_A              9.72f
#define ALCEDO_OPENCL_ACESCC_B              17.52f

// === Reference Gamut Compression constants ====================================

#define ALCEDO_OPENCL_RGC_LIM_CYAN    1.147f
#define ALCEDO_OPENCL_RGC_LIM_MAGENTA 1.264f
#define ALCEDO_OPENCL_RGC_LIM_YELLOW  1.312f
#define ALCEDO_OPENCL_RGC_THR_CYAN    0.815f
#define ALCEDO_OPENCL_RGC_THR_MAGENTA 0.803f
#define ALCEDO_OPENCL_RGC_THR_YELLOW  0.880f
#define ALCEDO_OPENCL_RGC_PWR         1.2f

// === AP0 → AP1 matrix (ACES2065-1 → ACEScg) ==================================

// Values inlined in opencl_tows_op to avoid file-scope __constant limitations on OpenCL 1.2.

// === Matrix helper ============================================================

static inline float3 opencl_apply_matrix3x3(__global const float* mat, float3 v) {
  return (float3)(mat[0] * v.x + mat[1] * v.y + mat[2] * v.z,
                  mat[3] * v.x + mat[4] * v.y + mat[5] * v.z,
                  mat[6] * v.x + mat[7] * v.y + mat[8] * v.z);
}

// === RGC compression curve ====================================================

static inline float opencl_rgc_compress_curve(float dist, float lim, float thr, float pwr) {
  if (dist < thr) {
    return dist;
  }
  const float t_diff      = lim - thr;
  const float one_minus_t = 1.0f - thr;
  const float inner_pow   = pow(one_minus_t / t_diff, -pwr);
  const float denom       = pow(fmax(0.0f, inner_pow - 1.0f), 1.0f / pwr);
  const float scl         = (denom > 1e-6f) ? (t_diff / denom) : 0.0f;
  const float nd          = (dist - thr) / scl;
  const float p           = pow(fmax(0.0f, nd), pwr);
  return thr + scl * nd / pow(1.0f + p, 1.0f / pwr);
}

// === ACEScc encode / decode ===================================================

static inline float opencl_acescc_encode(float x) {
  const float encode_floor = (ALCEDO_OPENCL_ACESCC_LOG2_DENORM + ALCEDO_OPENCL_ACESCC_A) / ALCEDO_OPENCL_ACESCC_B;
  if (x <= 0.0f) {
    return encode_floor + x;
  }
  if (x < ALCEDO_OPENCL_ACESCC_DENORM_TRANS) {
    return (log2(ALCEDO_OPENCL_ACESCC_DENORM_OFFSET + x * 0.5f) + ALCEDO_OPENCL_ACESCC_A) / ALCEDO_OPENCL_ACESCC_B;
  }
  return (log2(x) + ALCEDO_OPENCL_ACESCC_A) / ALCEDO_OPENCL_ACESCC_B;
}

static inline float opencl_acescc_decode(float acescc) {
  const float encode_floor     = (ALCEDO_OPENCL_ACESCC_LOG2_DENORM + ALCEDO_OPENCL_ACESCC_A) / ALCEDO_OPENCL_ACESCC_B;
  const float denorm_threshold = (ALCEDO_OPENCL_ACESCC_LOG2_MIN + ALCEDO_OPENCL_ACESCC_A) / ALCEDO_OPENCL_ACESCC_B;
  if (acescc < encode_floor) {
    return acescc - encode_floor;
  }
  if (acescc <= denorm_threshold) {
    return (exp2(acescc * ALCEDO_OPENCL_ACESCC_B - ALCEDO_OPENCL_ACESCC_A) - ALCEDO_OPENCL_ACESCC_DENORM_OFFSET) * 2.0f;
  }
  return exp2(acescc * ALCEDO_OPENCL_ACESCC_B - ALCEDO_OPENCL_ACESCC_A);
}

// === Sigmoid / contrast helpers ===============================================

static inline float opencl_sigmoid(float t) {
  return 1.0f / (1.0f + exp(-t));
}

static inline float opencl_contrast_sigmoid_01(float x, float k) {
  const float a = opencl_sigmoid(-0.5f * k);
  const float b = opencl_sigmoid(0.5f * k);
  const float y = opencl_sigmoid(k * (x - 0.5f));
  return (y - a) / (b - a);
}

static inline float3 opencl_contrast_on_luma_acescc(float3 rgb_acescc, float k, float pivot, float range) {
  const float Y = 0.2126f * rgb_acescc.x + 0.7152f * rgb_acescc.y + 0.0722f * rgb_acescc.z;
  const float lo = pivot - range;
  const float hi = pivot + range;
  const float t  = (Y - lo) / (hi - lo);
  if (t <= 0.0f || t >= 1.0f) {
    return rgb_acescc;
  }
  const float t2    = opencl_contrast_sigmoid_01(t, k);
  const float Y2    = lo + (hi - lo) * t2;
  const float scale = Y2 / fmax(Y, 1e-6f);
  return rgb_acescc * scale;
}

// === Luma helpers =============================================================

static inline float opencl_luma(float3 rgb) {
  return 0.2126f * rgb.x + 0.7152f * rgb.y + 0.0722f * rgb.z;
}

static inline float opencl_shared_tone_luma(float3 rgb) {
  return 0.2126f * rgb.x + 0.7152f * rgb.y + 0.0722f * rgb.z;
}

// === Shared tone curve evaluation =============================================

static inline float opencl_evaluate_shared_tone_curve(float x, __global const OpenClFusedParams* params) {
  const int curve_count = params->shared_tone_curve_ctrl_pts_size_;
  if (curve_count <= 0) return x;
  if (curve_count == 1) {
    return params->shared_tone_curve_ctrl_pts_y_[0];
  }
  if (x <= params->shared_tone_curve_ctrl_pts_x_[0]) {
    return params->shared_tone_curve_ctrl_pts_y_[0];
  }
  if (x >= params->shared_tone_curve_ctrl_pts_x_[curve_count - 1]) {
    return params->shared_tone_curve_ctrl_pts_y_[curve_count - 1] +
           (x - params->shared_tone_curve_ctrl_pts_x_[curve_count - 1]) *
               params->shared_tone_curve_m_[curve_count - 1];
  }

  int idx = curve_count - 2;
  for (int i = 0; i < curve_count - 1; ++i) {
    if (x < params->shared_tone_curve_ctrl_pts_x_[i + 1]) {
      idx = i;
      break;
    }
  }

  const float dx = params->shared_tone_curve_h_[idx];
  if (fabs(dx) <= 1e-8f) {
    return params->shared_tone_curve_ctrl_pts_y_[idx];
  }

  const float t   = (x - params->shared_tone_curve_ctrl_pts_x_[idx]) / dx;
  const float h00 = 2.0f * t * t * t - 3.0f * t * t + 1.0f;
  const float h10 = t * t * t - 2.0f * t * t + t;
  const float h01 = -2.0f * t * t * t + 3.0f * t * t;
  const float h11 = t * t * t - t * t;

  return h00 * params->shared_tone_curve_ctrl_pts_y_[idx] +
         h10 * dx * params->shared_tone_curve_m_[idx] +
         h01 * params->shared_tone_curve_ctrl_pts_y_[idx + 1] +
         h11 * dx * params->shared_tone_curve_m_[idx + 1];
}

static inline float3 opencl_reconstruct_shared_tone_rgb(float3 rgb, float source_luma, float mapped_luma) {
  const float3 delta = rgb - (float3)(source_luma);

  float scale = 1.0f;
  if (delta.x < 0.0f) scale = fmin(scale, mapped_luma / -delta.x);
  if (delta.y < 0.0f) scale = fmin(scale, mapped_luma / -delta.y);
  if (delta.z < 0.0f) scale = fmin(scale, mapped_luma / -delta.z);
  scale = clamp(scale, 0.0f, 1.0f);

  return (float3)(mapped_luma) + delta * scale;
}

static inline float4 opencl_apply_shared_tone_mapping(float4 px, __global const OpenClFusedParams* params) {
  if (params->shared_tone_curve_enabled_ == 0u) {
    return px;
  }

  const float  source_l = opencl_shared_tone_luma(px.xyz);
  float        mapped_l = opencl_evaluate_shared_tone_curve(source_l, params);
  if (!isfinite(mapped_l)) {
    mapped_l = source_l;
  }

  px.xyz = opencl_reconstruct_shared_tone_rgb(px.xyz, source_l, mapped_l);
  return px;
}

// === Curve hermite evaluation =================================================

static inline float opencl_evaluate_curve_hermite(float x, __global const OpenClFusedParams* params) {
  const int curve_count = params->curve_ctrl_pts_size_;
  if (curve_count <= 0) return x;
  if (curve_count == 1) {
    return params->curve_ctrl_pts_y_[0];
  }

  if (x <= params->curve_ctrl_pts_x_[0]) return params->curve_ctrl_pts_y_[0];
  if (x >= params->curve_ctrl_pts_x_[curve_count - 1]) return params->curve_ctrl_pts_y_[curve_count - 1];

  int idx = curve_count - 2;
  for (int i = 0; i < curve_count - 1; ++i) {
    if (x < params->curve_ctrl_pts_x_[i + 1]) {
      idx = i;
      break;
    }
  }

  const float dx = params->curve_h_[idx];
  if (fabs(dx) <= 1e-8f) {
    return params->curve_ctrl_pts_y_[idx];
  }

  const float t   = (x - params->curve_ctrl_pts_x_[idx]) / dx;
  const float h00 = 2.0f * t * t * t - 3.0f * t * t + 1.0f;
  const float h10 = t * t * t - 2.0f * t * t + t;
  const float h01 = -2.0f * t * t * t + 3.0f * t * t;
  const float h11 = t * t * t - t * t;

  return h00 * params->curve_ctrl_pts_y_[idx] +
         h10 * dx * params->curve_m_[idx] +
         h01 * params->curve_ctrl_pts_y_[idx + 1] +
         h11 * dx * params->curve_m_[idx + 1];
}

// === HLS helpers ==============================================================

static inline float opencl_wrap_hue(float h) {
  h = fmod(h, 360.0f);
  if (h < 0.0f) {
    h += 360.0f;
  }
  return h;
}

static inline float opencl_hue_distance(float a, float b) {
  const float diff = fabs(opencl_wrap_hue(a) - opencl_wrap_hue(b));
  return fmin(diff, 360.0f - diff);
}

static inline float opencl_smoothstep_range(float edge0, float edge1, float x) {
  const float denom = fmax(edge1 - edge0, 1e-6f);
  const float t     = clamp((x - edge0) / denom, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

static inline float opencl_soft_floor(float x, float floor, float softness) {
  const float t = (x - floor) / fmax(softness, 1e-6f);
  if (t > 20.0f) return x;
  if (t < -20.0f) return floor;
  return floor + softness * log(1.0f + exp(t));
}

static inline float3 opencl_acescc_to_ap1(float3 acescc) {
  return (float3)(opencl_acescc_decode(acescc.x), opencl_acescc_decode(acescc.y),
                  opencl_acescc_decode(acescc.z));
}

static inline float3 opencl_ap1_to_acescc(float3 ap1) {
  return (float3)(opencl_acescc_encode(ap1.x), opencl_acescc_encode(ap1.y),
                  opencl_acescc_encode(ap1.z));
}

static inline float3 opencl_ap1_to_oklab(float3 ap1) {
  const float l = 0.62217537f * ap1.x + 0.34268438f * ap1.y + 0.02339492f * ap1.z;
  const float m = 0.26593478f * ap1.x + 0.62930460f * ap1.y + 0.10828100f * ap1.z;
  const float s = 0.09725037f * ap1.x + 0.18525749f * ap1.y + 0.77254586f * ap1.z;

  const float l_ = cbrt(l);
  const float m_ = cbrt(m);
  const float s_ = cbrt(s);

  return (float3)(0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_,
                  1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_,
                  0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_);
}

static inline float3 opencl_oklab_to_ap1(float3 lab) {
  const float l_ = lab.x + 0.3963377774f * lab.y + 0.2158037573f * lab.z;
  const float m_ = lab.x - 0.1055613458f * lab.y - 0.0638541728f * lab.z;
  const float s_ = lab.x - 0.0894841775f * lab.y - 1.2914855480f * lab.z;

  const float l = l_ * l_ * l_;
  const float m = m_ * m_ * m_;
  const float s = s_ * s_ * s_;

  return (float3)(2.09085732f * l - 1.16812363f * m + 0.10040848f * s,
                  -0.87435428f * l + 2.14592958f * m - 0.27429822f * s,
                  -0.05353206f * l - 0.36754978f * m + 1.34755888f * s);
}

static inline float3 opencl_fit_ap1_lower_gamut(float3 adjusted_ap1, float3 neutral_ap1) {
  const float lower = -1e-5f;
  float scale = 1.0f;

  if (adjusted_ap1.x < lower && neutral_ap1.x > adjusted_ap1.x) {
    scale = fmin(scale, (neutral_ap1.x - lower) / (neutral_ap1.x - adjusted_ap1.x));
  }
  if (adjusted_ap1.y < lower && neutral_ap1.y > adjusted_ap1.y) {
    scale = fmin(scale, (neutral_ap1.y - lower) / (neutral_ap1.y - adjusted_ap1.y));
  }
  if (adjusted_ap1.z < lower && neutral_ap1.z > adjusted_ap1.z) {
    scale = fmin(scale, (neutral_ap1.z - lower) / (neutral_ap1.z - adjusted_ap1.z));
  }

  scale = clamp(scale, 0.0f, 1.0f);
  return neutral_ap1 + (adjusted_ap1 - neutral_ap1) * scale;
}

static inline float opencl_hue2rgb(float p, float q, float t) {
  if (t < 0.0f) t += 1.0f;
  if (t > 1.0f) t -= 1.0f;
  if (t < (1.0f / 6.0f)) return p + (q - p) * 6.0f * t;
  if (t < 0.5f)         return q;
  if (t < (2.0f / 3.0f)) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
  return p;
}

#endif  // ALCEDO_OPENCL_COMMON_CL
