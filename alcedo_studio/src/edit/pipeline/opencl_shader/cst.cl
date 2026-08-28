//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifndef ALCEDO_OPENCL_EDIT_PIPELINE_CST_CL
#define ALCEDO_OPENCL_EDIT_PIPELINE_CST_CL

// === 3D LUT helper ============================================================

static inline uint opencl_lut3d_index(uint edge, uint x, uint y, uint z) {
  return (z * edge + y) * edge + x;
}

// Sample a 3D LUT with trilinear interpolation.
// The LUT is stored as a flat buffer of packed float4 (R, G, B, A) values,
// with x (red) varying fastest, then y (green), then z (blue).
// Coordinate mapping matches CUDA tex3D with normalized coordinates and
// cudaFilterModeLinear.
static inline float4 opencl_sample_lut3d_linear(__global const float* lut, uint edge,
                                                 float u, float v, float w) {
  // tex3D normalized-coordinate mapping: texel_pos = coord * size - 0.5
  float3 tex_pos = (float3)(u * (float)edge - 0.5f,
                             v * (float)edge - 0.5f,
                             w * (float)edge - 0.5f);
  float3 pos     = clamp(tex_pos, 0.0f, (float)(edge - 1));
  uint3  lo      = convert_uint3(pos);
  uint3  hi      = min(lo + (uint3)(1), (uint3)(edge - 1));
  float3 t       = pos - convert_float3(lo);

  uint e = edge;
  // vload4(offset, ptr) reads from ptr + offset*4, so the offset is the
  // voxel index directly (each voxel is 4 packed floats).
  float4 c000 = vload4(opencl_lut3d_index(e, lo.x, lo.y, lo.z), lut);
  float4 c100 = vload4(opencl_lut3d_index(e, hi.x, lo.y, lo.z), lut);
  float4 c010 = vload4(opencl_lut3d_index(e, lo.x, hi.y, lo.z), lut);
  float4 c110 = vload4(opencl_lut3d_index(e, hi.x, hi.y, lo.z), lut);
  float4 c001 = vload4(opencl_lut3d_index(e, lo.x, lo.y, hi.z), lut);
  float4 c101 = vload4(opencl_lut3d_index(e, hi.x, lo.y, hi.z), lut);
  float4 c011 = vload4(opencl_lut3d_index(e, lo.x, hi.y, hi.z), lut);
  float4 c111 = vload4(opencl_lut3d_index(e, hi.x, hi.y, hi.z), lut);

  // Manual trilinear interpolation (avoids potential mix() scalar broadcast
  // differences across OpenCL implementations).
  float4 c00 = c000 + (c100 - c000) * t.x;
  float4 c10 = c010 + (c110 - c010) * t.x;
  float4 c01 = c001 + (c101 - c001) * t.x;
  float4 c11 = c011 + (c111 - c011) * t.x;
  float4 c0  = c00  + (c10  - c00)  * t.y;
  float4 c1  = c01  + (c11  - c01)  * t.y;
  return c0 + (c1 - c0) * t.z;
}

// === LMT (Look Modification Transform) operator ===============================

static inline float4 opencl_lmt_op(float4 px, __global const OpenClFusedParams* params,
                                    __global const float* lmt_lut) {
  if (params->lmt_enabled_ == 0u || params->lmt_lut_enabled_ == 0u ||
      params->lmt_lut_edge_size_ <= 1u) {
    return px;
  }

  uint  edge   = params->lmt_lut_edge_size_;
  float scale  = (float)(edge - 1u) / (float)edge;
  float offset = 1.0f / (2.0f * (float)edge);

  float u = px.x * scale + offset;
  float v = px.y * scale + offset;
  float w = px.z * scale + offset;

  float4 lut_v = opencl_sample_lut3d_linear(lmt_lut, edge, u, v, w);
  return (float4)(lut_v.x, lut_v.y, lut_v.z, px.w);
}

// === Shared CST math ==========================================================

#define ALCEDO_OPENCL_ODT_TABLE_SIZE 360
#define ALCEDO_OPENCL_ODT_BASE_INDEX 1
#define ALCEDO_OPENCL_ODT_HUE_LIMIT 360.0f
#define ALCEDO_OPENCL_REF_LUMINANCE 100.0f
#define ALCEDO_OPENCL_J_SCALE 100.0f
#define ALCEDO_OPENCL_CAM_NL_OFFSET 27.13f
#define ALCEDO_OPENCL_SMOOTH_CUSPS 0.12f
#define ALCEDO_OPENCL_CUSP_MID_BLEND 1.3f
#define ALCEDO_OPENCL_FOCUS_GAIN_BLEND 0.3f
#define ALCEDO_OPENCL_COMPRESSION_THRESHOLD 0.75f
#define ALCEDO_OPENCL_PI 3.1415926535897932f
#define ALCEDO_OPENCL_OPEN_DRT_SQRT3 1.7320508075688772f

static inline float3 opencl_mat3_mul_private(const float mat[9], float3 v) {
  return (float3)(v.x * mat[0] + v.y * mat[3] + v.z * mat[6],
                  v.x * mat[1] + v.y * mat[4] + v.z * mat[7],
                  v.x * mat[2] + v.y * mat[5] + v.z * mat[8]);
}

static inline float3 opencl_mat3_mul_global(float3 v, __global const float* mat) {
  return (float3)(v.x * mat[0] + v.y * mat[3] + v.z * mat[6],
                  v.x * mat[1] + v.y * mat[4] + v.z * mat[7],
                  v.x * mat[2] + v.y * mat[5] + v.z * mat[8]);
}

static inline float opencl_clamp_f(float x, float lo, float hi) {
  return fmin(fmax(x, lo), hi);
}

static inline int opencl_clamp_i(int x, int lo, int hi) {
  return x < lo ? lo : (x > hi ? hi : x);
}

static inline float opencl_safe_sqrt(float x) {
  return sqrt(fmax(x, 0.0f));
}

static inline float opencl_safe_div(float a, float b, float eps) {
  const float ab = fabs(b);
  const float bb = (ab < eps) ? copysign(eps, b) : b;
  return a / bb;
}

static inline float opencl_safe_log10_ratio(float num, float den, float eps) {
  return log10(fmax(num, eps) / fmax(den, eps));
}

static inline float opencl_safe_pow_pos(float base, float expv) {
  return pow(fmax(base, 0.0f), expv);
}

static inline float opencl_wrap_to_360(float hue) {
  if (!isfinite(hue)) return 0.0f;
  float y = fmod(hue, 360.0f);
  return y + ((y < 0.0f) ? 360.0f : 0.0f);
}

static inline float opencl_degrees_to_radians(float deg) {
  return deg * (ALCEDO_OPENCL_PI / 180.0f);
}

static inline float opencl_radians_to_degrees(float rad) {
  return rad * (180.0f / ALCEDO_OPENCL_PI);
}

// === Display encoding, matching CUDA disp_enc_funcs.cuh ======================

static inline float opencl_moncurve_inv(float y, float gamma, float offs) {
  const float yb = pow(offs * gamma / ((gamma - 1.0f) * (1.0f + offs)), gamma);
  const float rs = pow((gamma - 1.0f) / offs, gamma - 1.0f) *
                   pow((1.0f + offs) / gamma, gamma);
  return (y >= yb) ? (1.0f + offs) * pow(y, 1.0f / gamma) - offs : y * rs;
}

static inline float opencl_bt1886_inv(float L, float gamma) {
  const float Lw = 1.0f;
  const float Lb = 0.0f;
  const float a = pow(pow(Lw, 1.0f / gamma) - pow(Lb, 1.0f / gamma), gamma);
  const float b = pow(Lb, 1.0f / gamma) / (pow(Lw, 1.0f / gamma) - pow(Lb, 1.0f / gamma));
  return pow(fmax(L / a, 0.0f), 1.0f / gamma) - b;
}

static inline float opencl_y_to_st2084(float C) {
  const float pq_m1 = 0.1593017578125f;
  const float pq_m2 = 78.84375f;
  const float pq_c1 = 0.8359375f;
  const float pq_c2 = 18.8515625f;
  const float pq_c3 = 18.6875f;
  const float pq_C = 10000.0f;
  float L = C / pq_C;
  float Lm = pow(L, pq_m1);
  float N = (pq_c1 + pq_c2 * Lm) / (1.0f + pq_c3 * Lm);
  return pow(N, pq_m2);
}

static inline float3 opencl_hlg_from_display_linear_1000nits(float3 display_linear) {
  float Yd = 0.2627f * display_linear.x + 0.6780f * display_linear.y + 0.0593f * display_linear.z;
  float3 rgb = display_linear;
  if (Yd > 0.0f) {
    rgb *= pow(Yd, (1.0f - 1.2f) / 1.2f);
  }
  const float a = 0.17883277f;
  const float b = 0.28466892f;
  const float c = 0.55991073f;
  rgb.x = (rgb.x <= (1.0f / 12.0f)) ? sqrt(3.0f * rgb.x) : a * log(12.0f * rgb.x - b) + c;
  rgb.y = (rgb.y <= (1.0f / 12.0f)) ? sqrt(3.0f * rgb.y) : a * log(12.0f * rgb.y - b) + c;
  rgb.z = (rgb.z <= (1.0f / 12.0f)) ? sqrt(3.0f * rgb.z) : a * log(12.0f * rgb.z - b) + c;
  return rgb;
}

static inline float3 opencl_eotf_inv(float3 rgb_linear_in, int eotf_type) {
  float3 rgb = fmax(rgb_linear_in, (float3)(0.0f));
  if (eotf_type == 0) {
    return rgb;
  } else if (eotf_type == 1) {
    return (float3)(opencl_y_to_st2084(rgb.x), opencl_y_to_st2084(rgb.y),
                    opencl_y_to_st2084(rgb.z));
  } else if (eotf_type == 2) {
    return opencl_hlg_from_display_linear_1000nits(rgb);
  } else if (eotf_type == 4) {
    return (float3)(opencl_bt1886_inv(rgb.x, 2.4f), opencl_bt1886_inv(rgb.y, 2.4f),
                    opencl_bt1886_inv(rgb.z, 2.4f));
  } else if (eotf_type == 3) {
    return pow(rgb, (float3)(1.0f / 2.6f));
  } else if (eotf_type == 5) {
    return pow(rgb, (float3)(1.0f / 2.2f));
  } else if (eotf_type == 6) {
    return pow(rgb, (float3)(1.0f / 1.8f));
  }
  return (float3)(opencl_moncurve_inv(rgb.x, 2.4f, 0.055f),
                  opencl_moncurve_inv(rgb.y, 2.4f, 0.055f),
                  opencl_moncurve_inv(rgb.z, 2.4f, 0.055f));
}

static inline float3 opencl_display_encoding(float3 rgb, __global const float* limit_to_display,
                                             int eotf, float linear_scale) {
  float3 rgb_disp_linear = opencl_mat3_mul_global(rgb, limit_to_display);
  return opencl_eotf_inv(rgb_disp_linear * linear_scale, eotf);
}

// === ACES 2.0 output transform ===============================================

typedef struct {
  float2 JMcusp;
  float gamma_bottom_inv;
  float gamma_top_inv;
  float focus_J;
  float analytical_threshold;
} OpenClHueDependentGamutParams;

static inline float opencl_tonescale_fwd(float x, __global const OpenClTSParams* ts) {
  const float denom = x + ts->s_2_;
  const float ratio = (denom > 1e-7f) ? (fmax(0.0f, x) / denom) : 0.0f;
  float f = ts->m_2_ * pow(ratio, ts->g_);
  float h = fmax(0.0f, f * f / (f + ts->t_1_));
  return h * ts->n_r_;
}

static inline float opencl_pacrc_fwd(float v) {
  const float abs_v = fabs(v);
  const float F_L_Y = pow(abs_v, 0.42f);
  const float Rc = F_L_Y / (ALCEDO_OPENCL_CAM_NL_OFFSET + F_L_Y);
  return copysign(Rc, v);
}

static inline float opencl_pacrc_inv(float v) {
  const float abs_v = fabs(v);
  const float Ra_lim = fmin(abs_v, 0.99f);
  const float F_L_Y = (ALCEDO_OPENCL_CAM_NL_OFFSET * Ra_lim) / (1.0f - Ra_lim);
  return copysign(pow(F_L_Y, 1.0f / 0.42f), v);
}

static inline float opencl_achromatic_n_to_J(float A, float cz) {
  return ALCEDO_OPENCL_J_SCALE * pow(A, cz);
}

static inline float opencl_J_to_achromatic_n(float J, float inv_cz) {
  return pow(J * (1.0f / ALCEDO_OPENCL_J_SCALE), inv_cz);
}

static inline float3 opencl_rgb_to_aab(float3 RGB, __global OpenClJMhParams* p) {
  float3 RGB_m = opencl_mat3_mul_global(RGB, p->MATRIX_RGB_to_CAM16_c_);
  float3 RGB_a = (float3)(opencl_pacrc_fwd(RGB_m.x), opencl_pacrc_fwd(RGB_m.y),
                          opencl_pacrc_fwd(RGB_m.z));
  return opencl_mat3_mul_global(RGB_a, p->MATRIX_cone_response_to_Aab_);
}

static inline float3 opencl_aab_to_jmh(float3 Aab, __global OpenClJMhParams* p) {
  const float mask = Aab.x > 0.0f ? 1.0f : 0.0f;
  const float J = opencl_achromatic_n_to_J(Aab.x, p->cz_) * mask;
  const float M = opencl_safe_sqrt(Aab.y * Aab.y + Aab.z * Aab.z) * mask;
  const float h = opencl_wrap_to_360(opencl_radians_to_degrees(atan2(Aab.z, Aab.y))) * mask;
  return (float3)(J, M, h);
}

static inline float3 opencl_jmh_to_aab(float3 JMh, __global OpenClJMhParams* p) {
  const float h_rad = opencl_degrees_to_radians(JMh.z);
  const float A = opencl_J_to_achromatic_n(JMh.x, p->inv_cz_);
  return (float3)(A, JMh.y * cos(h_rad), JMh.y * sin(h_rad));
}

static inline float3 opencl_aab_to_rgb(float3 Aab, __global OpenClJMhParams* p) {
  float3 RGB_a = opencl_mat3_mul_global(Aab, p->MATRIX_Aab_to_cone_response_);
  float3 RGB_m = (float3)(opencl_pacrc_inv(RGB_a.x), opencl_pacrc_inv(RGB_a.y),
                          opencl_pacrc_inv(RGB_a.z));
  return opencl_mat3_mul_global(RGB_m, p->MATRIX_CAM16_c_to_RGB_);
}

static inline float3 opencl_rgb_to_jmh(float3 color, __global OpenClJMhParams* p) {
  return opencl_aab_to_jmh(opencl_rgb_to_aab(color, p), p);
}

static inline float3 opencl_jmh_to_rgb(float3 JMh, __global OpenClJMhParams* p) {
  return opencl_aab_to_rgb(opencl_jmh_to_aab(JMh, p), p);
}

static inline float opencl_A_to_Y(float A, __global OpenClJMhParams* p) {
  float Ra = p->A_w_J_ * A;
  return opencl_pacrc_inv(Ra) / p->F_L_n_;
}

static inline float opencl_J_to_Y(float J, __global OpenClJMhParams* p) {
  return opencl_A_to_Y(opencl_J_to_achromatic_n(fabs(J), p->inv_cz_), p);
}

static inline float opencl_Y_to_J(float Y, __global OpenClJMhParams* p) {
  float Ra = opencl_pacrc_fwd(fabs(Y) * p->F_L_n_);
  float J = opencl_achromatic_n_to_J(Ra * p->inv_A_w_J_, p->cz_);
  return copysign(J, Y);
}

static inline float opencl_reach_M_from_table(float h, __global const OpenClODTParams* p) {
  const float hw = opencl_wrap_to_360(h);
  const float pos = hw * ((float)ALCEDO_OPENCL_ODT_TABLE_SIZE / ALCEDO_OPENCL_ODT_HUE_LIMIT);
  const int base = opencl_clamp_i((int)pos, 0, ALCEDO_OPENCL_ODT_TABLE_SIZE - 1);
  const float t = opencl_clamp_f(pos - (float)base, 0.0f, 1.0f);
  const int i_lo = base + ALCEDO_OPENCL_ODT_BASE_INDEX;
  const int i_hi = i_lo + 1;
  return p->table_reach_M_[i_lo] + (p->table_reach_M_[i_hi] - p->table_reach_M_[i_lo]) * t;
}

static inline float opencl_chroma_compress_norm(float h, float chroma_compress_scale) {
  float hr = opencl_degrees_to_radians(h);
  float a = cos(hr);
  float b = sin(hr);
  float cos_hr2 = a * a - b * b;
  float sin_hr2 = 2.0f * a * b;
  float cos_hr3 = 4.0f * a * a * a - 3.0f * a;
  float sin_hr3 = 3.0f * b - 4.0f * b * b * b;
  return (11.34072f * a + 16.46899f * cos_hr2 + 7.88380f * cos_hr3 +
          14.66441f * b - 6.37224f * sin_hr2 + 9.19364f * sin_hr3 + 77.12896f) *
         chroma_compress_scale;
}

static inline float opencl_toe(float x, float limit, float k1_in, float k2_in) {
  if (x > limit) return x;
  float k2 = fmax(k2_in, 0.001f);
  float k1 = sqrt(k1_in * k1_in + k2 * k2);
  float k3 = (limit + k1) / (limit + k2);
  float minus_b = k3 * x - k1;
  float minus_c = k2 * k3 * x;
  return 0.5f * (minus_b + opencl_safe_sqrt(minus_b * minus_b + 4.0f * minus_c));
}

static inline float3 opencl_chroma_compress_fwd(float3 JMh, float tonemapped_J,
                                                __global const OpenClODTParams* p) {
  float J = JMh.x;
  float M = JMh.y;
  float h = JMh.z;
  float M_compr = M;
  if (M != 0.0f) {
    const float limitJ = fmax(p->limit_J_max, 1e-6f);
    const float Jts = fmax(tonemapped_J, 0.0f);
    const float nJ = opencl_clamp_f(Jts / limitJ, 0.0f, 1.0f);
    const float snJ = fmax(0.0f, 1.0f - nJ);
    float Mnorm = opencl_chroma_compress_norm(h, p->chroma_compress_scale);
    if (!isfinite(Mnorm) || fabs(Mnorm) < 1e-6f) {
      return (float3)(Jts, 0.0f, h);
    }
    float limit = opencl_safe_pow_pos(nJ, p->model_gamma_inv) * opencl_reach_M_from_table(h, p) / Mnorm;
    limit = fmax(limit, 0.0f);
    const float ratio = (fabs(J) < 1e-6f) ? 1.0f : (Jts / fabs(J));
    M_compr = M * opencl_safe_pow_pos(ratio, p->model_gamma_inv);
    M_compr = M_compr / Mnorm;
    M_compr = limit - opencl_toe(limit - M_compr, limit - 0.001f,
                                 snJ * p->sat, sqrt(nJ * nJ + p->sat_thr));
    M_compr = opencl_toe(M_compr, limit, nJ * p->compr, snJ);
    M_compr *= Mnorm;
  }
  return (float3)(tonemapped_J, M_compr, h);
}

static inline float3 opencl_tonemap_and_compress_fwd(float3 JMh,
                                                     __global const OpenClODTParams* p) {
  float linear = opencl_J_to_Y(JMh.x, &p->input_params_) / ALCEDO_OPENCL_REF_LUMINANCE;
  float tonemapped_Y = opencl_tonescale_fwd(linear, &p->ts_);
  float J_ts = opencl_Y_to_J(tonemapped_Y, &p->input_params_);
  return opencl_chroma_compress_fwd(JMh, J_ts, p);
}

static inline int opencl_hue_position_in_uniform_table(float hue) {
  const float wrapped = opencl_wrap_to_360(hue);
  const float pos = wrapped * ((float)ALCEDO_OPENCL_ODT_TABLE_SIZE / ALCEDO_OPENCL_ODT_HUE_LIMIT);
  return opencl_clamp_i((int)pos, 0, ALCEDO_OPENCL_ODT_TABLE_SIZE - 1);
}

static inline int opencl_look_hue_interval(float h, __global const OpenClODTParams* p) {
  const float hw = opencl_wrap_to_360(h);
  int i = ALCEDO_OPENCL_ODT_BASE_INDEX + opencl_hue_position_in_uniform_table(hw);
  int i_lo = i + p->hue_linearity_search_range[0];
  int i_hi = i + p->hue_linearity_search_range[1];
  i_lo = i_lo < ALCEDO_OPENCL_ODT_BASE_INDEX ? ALCEDO_OPENCL_ODT_BASE_INDEX : i_lo;
  i_hi = i_hi > (ALCEDO_OPENCL_ODT_BASE_INDEX + ALCEDO_OPENCL_ODT_TABLE_SIZE)
             ? (ALCEDO_OPENCL_ODT_BASE_INDEX + ALCEDO_OPENCL_ODT_TABLE_SIZE)
             : i_hi;
  i = (i_lo + i_hi) >> 1;
  for (int k = 0; k < 6 && (i_lo + 1 < i_hi); ++k) {
    const float v = p->table_hues_[i];
    const int gt = hw > v;
    i_lo = gt ? i : i_lo;
    i_hi = gt ? i_hi : i;
    i = (i_lo + i_hi) >> 1;
  }
  return (i_hi < 1) ? 1 : i_hi;
}

static inline float2 opencl_cusp_from_table(float h, __global const OpenClODTParams* p) {
  const float hw = opencl_wrap_to_360(h);
  int low_i = 0;
  int high_i = ALCEDO_OPENCL_ODT_BASE_INDEX + ALCEDO_OPENCL_ODT_TABLE_SIZE;
  int i = ALCEDO_OPENCL_ODT_BASE_INDEX + opencl_hue_position_in_uniform_table(hw);
  for (int k = 0; k < 10 && (low_i + 1 < high_i); ++k) {
    const float h_i = p->table_gamut_cusps_[i][2];
    const int gt = hw > h_i;
    low_i = gt ? i : low_i;
    high_i = gt ? high_i : i;
    i = (low_i + high_i) >> 1;
  }
  const int lo_i = high_i - 1;
  const int hi_i = high_i;
  const float denom = p->table_gamut_cusps_[hi_i][2] - p->table_gamut_cusps_[lo_i][2];
  const float t = (denom != 0.0f) ? (hw - p->table_gamut_cusps_[lo_i][2]) / denom : 0.0f;
  return (float2)(p->table_gamut_cusps_[lo_i][0] +
                      (p->table_gamut_cusps_[hi_i][0] - p->table_gamut_cusps_[lo_i][0]) * t,
                  p->table_gamut_cusps_[lo_i][1] +
                      (p->table_gamut_cusps_[hi_i][1] - p->table_gamut_cusps_[lo_i][1]) * t);
}

static inline OpenClHueDependentGamutParams opencl_init_hue_dependent_gamut_params(
    float h, __global const OpenClODTParams* p) {
  OpenClHueDependentGamutParams hdp;
  hdp.gamma_bottom_inv = p->lower_hull_gamma_inv;
  const int i_hi = opencl_look_hue_interval(h, p);
  const float hw = opencl_wrap_to_360(h);
  const float t = hw - p->table_hues_[i_hi - 1];
  hdp.JMcusp = opencl_cusp_from_table(h, p);
  hdp.gamma_top_inv = p->table_upper_hull_gamma_[i_hi - 1] +
                      (p->table_upper_hull_gamma_[i_hi] -
                       p->table_upper_hull_gamma_[i_hi - 1]) * t;
  hdp.focus_J = hdp.JMcusp.x + (p->mid_J - hdp.JMcusp.x) *
                                fmin(1.0f, ALCEDO_OPENCL_CUSP_MID_BLEND -
                                               (hdp.JMcusp.x / p->limit_J_max));
  hdp.analytical_threshold = hdp.JMcusp.x + (p->limit_J_max - hdp.JMcusp.x) *
                                             ALCEDO_OPENCL_FOCUS_GAIN_BLEND;
  return hdp;
}

static inline float opencl_get_focus_gain(float J, float analytical_threshold,
                                          float limit_J_max, float focus_dist) {
  float gain = limit_J_max * focus_dist;
  if (J > analytical_threshold) {
    float adj = opencl_safe_log10_ratio(limit_J_max - analytical_threshold, limit_J_max - J, 1e-4f);
    gain *= adj * adj + 1.0f;
  }
  return gain;
}

static inline float opencl_solve_J_intersect(float J, float M, float focusJ, float maxJ,
                                             float slope_gain) {
  const float sg = fmax(fabs(slope_gain), 1e-6f);
  const float fj = fmax(fabs(focusJ), 1e-6f);
  const float M_scaled = M / sg;
  const float a = M_scaled / fj;
  const float b1 = 1.0f - M_scaled;
  const float c1 = -J;
  const float r1 = (b1 * b1 - 4.0f * a * c1 > 0.0f) ? opencl_safe_sqrt(b1 * b1 - 4.0f * a * c1) : 0.0f;
  const float den1 = copysign(fmax(fabs(b1 + r1), 1e-6f), b1 + r1);
  const float res1 = (-2.0f * c1) / den1;
  const float b2 = -(1.0f + M_scaled + maxJ * a);
  const float c2 = maxJ * M_scaled + J;
  const float r2 = (b2 * b2 - 4.0f * a * c2 > 0.0f) ? opencl_safe_sqrt(b2 * b2 - 4.0f * a * c2) : 0.0f;
  const float den2 = copysign(fmax(fabs(b2 - r2), 1e-6f), b2 - r2);
  const float res2 = (-2.0f * c2) / den2;
  return (J < focusJ) ? res1 : res2;
}

static inline float opencl_estimate_line_boundary_M(float J_axis_intersect, float slope,
                                                     float inv_gamma, float J_max, float M_max,
                                                     float J_intersection_reference) {
  const float refJ = fmax(J_intersection_reference, 1e-6f);
  const float shifted = refJ * opencl_safe_pow_pos(fmax(J_axis_intersect / refJ, 0.0f), inv_gamma);
  const float denom = copysign(fmax(fabs(J_max - slope * M_max), 1e-6f), J_max - slope * M_max);
  return shifted * M_max / denom;
}

static inline float opencl_smin_scaled(float a, float b, float scale_reference) {
  const float s_scaled = ALCEDO_OPENCL_SMOOTH_CUSPS * scale_reference;
  if (s_scaled <= 1e-6f) return fmin(a, b);
  const float h = fmax(s_scaled - fabs(a - b), 0.0f) / s_scaled;
  return fmin(a, b) - h * h * h * s_scaled * (1.0f / 6.0f);
}

static inline float opencl_find_gamut_boundary_intersection(float2 JM_cusp, float J_max,
                                                            float gamma_top_inv,
                                                            float gamma_bottom_inv,
                                                            float J_intersect_source,
                                                            float slope,
                                                            float J_intersect_cusp) {
  const float lower = opencl_estimate_line_boundary_M(
      J_intersect_source, slope, gamma_bottom_inv, JM_cusp.x, JM_cusp.y, J_intersect_cusp);
  const float upper = opencl_estimate_line_boundary_M(
      J_max - J_intersect_source, -slope, gamma_top_inv, J_max - JM_cusp.x, JM_cusp.y,
      J_max - J_intersect_cusp);
  return opencl_smin_scaled(lower, upper, JM_cusp.y);
}

static inline float opencl_remap_M(float M, float gamut_boundary_M, float reach_boundary_M) {
  const float boundary_ratio = opencl_safe_div(gamut_boundary_M, reach_boundary_M, 1e-6f);
  const float proportion = fmax(boundary_ratio, ALCEDO_OPENCL_COMPRESSION_THRESHOLD);
  const float threshold = proportion * gamut_boundary_M;
  if (M <= threshold || proportion >= 1.0f) return M;
  const float m_offset = M - threshold;
  const float gamut_off = gamut_boundary_M - threshold;
  const float reach_off = reach_boundary_M - threshold;
  const float ratio_rg = opencl_safe_div(reach_off, gamut_off, 1e-6f);
  const float scale = reach_off / fmax(ratio_rg - 1.0f, 1e-7f);
  const float nd = m_offset / scale;
  return threshold + scale * nd / (1.0f + nd);
}

static inline float3 opencl_compress_gamut(float3 JMh, float Jx, __global const OpenClODTParams* p,
                                           OpenClHueDependentGamutParams hdp) {
  const float slope_gain = fmax(opencl_get_focus_gain(Jx, hdp.analytical_threshold,
                                                      p->limit_J_max, p->focus_dist), 1e-6f);
  const float J_intersect_source =
      opencl_solve_J_intersect(JMh.x, JMh.y, hdp.focus_J, p->limit_J_max, slope_gain);
  const float direction_scalar =
      (J_intersect_source < hdp.focus_J) ? J_intersect_source : p->limit_J_max - J_intersect_source;
  const float gamut_slope = direction_scalar * (J_intersect_source - hdp.focus_J) /
                            fmax(fabs(hdp.focus_J * slope_gain), 1e-6f);
  const float J_intersect_cusp =
      opencl_solve_J_intersect(hdp.JMcusp.x, hdp.JMcusp.y, hdp.focus_J, p->limit_J_max, slope_gain);
  const float gamut_boundary_M = opencl_find_gamut_boundary_intersection(
      hdp.JMcusp, p->limit_J_max, hdp.gamma_top_inv, hdp.gamma_bottom_inv,
      J_intersect_source, gamut_slope, J_intersect_cusp);
  if (gamut_boundary_M <= 0.0f) {
    return (float3)(Jx, 0.0f, JMh.z);
  }
  const float reach_max_M = fmax(opencl_reach_M_from_table(JMh.z, p), 0.0f);
  const float reach_boundary_M = opencl_estimate_line_boundary_M(
      J_intersect_source, gamut_slope, p->model_gamma_inv, p->limit_J_max, reach_max_M,
      p->limit_J_max);
  const float remapped_M = opencl_remap_M(JMh.y, gamut_boundary_M, reach_boundary_M);
  return (float3)(J_intersect_source + remapped_M * gamut_slope, remapped_M, JMh.z);
}

static inline float3 opencl_gamut_compress_fwd(float3 JMh, __global const OpenClODTParams* p) {
  if (JMh.x <= 0.0f) return (float3)(0.0f, 0.0f, JMh.z);
  if (JMh.y <= 0.0f || JMh.x > p->limit_J_max) return (float3)(JMh.x, 0.0f, JMh.z);
  OpenClHueDependentGamutParams hdp = opencl_init_hue_dependent_gamut_params(JMh.z, p);
  return opencl_compress_gamut(JMh, JMh.x, p, hdp);
}

static inline float3 opencl_ap1_to_ap0(float3 ap1) {
    const float AP1_TO_AP0[9] = {
        0.695452213f, 0.0447945632f, -0.00552588236f,
        0.140678704f, 0.859671116f, 0.00402521016f,
        0.163869068f, 0.0955343172f, 1.00150073f};
  return opencl_mat3_mul_private(AP1_TO_AP0, ap1);
}

static inline float3 opencl_limit_rgb_preserve_chroma(float3 rgb, float lower, float upper) {
  if (!isfinite(rgb.x) || !isfinite(rgb.y) || !isfinite(rgb.z)) return (float3)(0.0f);
  rgb = fmax(rgb, (float3)(lower));
  const float m = fmax(rgb.x, fmax(rgb.y, rgb.z));
  if (m > upper && m > 0.0f) rgb *= upper / m;
  return rgb;
}

static inline float3 opencl_aces_output_transform_fwd(float3 in_color,
                                                      __global const OpenClODTParams* p) {
  if (!isfinite(in_color.x) || !isfinite(in_color.y) || !isfinite(in_color.z)) {
    return (float3)(0.0f);
  }
  float3 ap0 = opencl_ap1_to_ap0(in_color);
  float3 JMh = opencl_rgb_to_jmh(ap0, &p->input_params_);
  float3 tonemapped_JMh = opencl_tonemap_and_compress_fwd(JMh, p);
  float3 compressed_JMh = opencl_gamut_compress_fwd(tonemapped_JMh, p);
  float3 out_rgb = opencl_jmh_to_rgb(compressed_JMh, &p->limit_params_);
  return opencl_limit_rgb_preserve_chroma(out_rgb, 0.0f, p->ts_.forward_limit_);
}

// === OpenDRT output transform =================================================

static inline float3 opencl_odrt_apply_matrix(const float mat[9], float3 v) {
  return (float3)(mat[0] * v.x + mat[1] * v.y + mat[2] * v.z,
                  mat[3] * v.x + mat[4] * v.y + mat[5] * v.z,
                  mat[6] * v.x + mat[7] * v.y + mat[8] * v.z);
}

static inline float opencl_odrt_spowf(float a, float b) {
  return (a <= 0.0f) ? a : pow(a, b);
}

static inline float opencl_odrt_hypot2(float2 v) {
  return sqrt(fmax(0.0f, v.x * v.x + v.y * v.y));
}

static inline float opencl_odrt_hypot3(float3 v) {
  return sqrt(fmax(0.0f, v.x * v.x + v.y * v.y + v.z * v.z));
}

static inline float opencl_odrt_compress_hyperbolic_power(float x, float s, float p) {
  return opencl_odrt_spowf(x / (x + s), p);
}

static inline float opencl_odrt_compress_toe_quadratic(float x, float toe) {
  return (toe == 0.0f) ? x : opencl_odrt_spowf(x, 2.0f) / (x + toe);
}

static inline float opencl_odrt_compress_toe_cubic(float x, float m, float w, int inv) {
  if (m == 1.0f) return x;
  const float x2 = x * x;
  if (inv == 0) {
    return x * (x2 + m * w) / (x2 + w);
  }
  const float p0 = x2 - 3.0f * m * w;
  const float p1 = 2.0f * x2 + 27.0f * w - 9.0f * m * w;
  const float p2 = pow(sqrt(x2 * p1 * p1 - 4.0f * p0 * p0 * p0) / 2.0f + x * p1 / 2.0f,
                       1.0f / 3.0f);
  return p0 / (3.0f * p2) + p2 / 3.0f + x / 3.0f;
}

static inline float opencl_odrt_contrast_high(float x, float p, float pv, float pv_lx) {
  const float x0 = 0.18f * pow(2.0f, pv);
  if (x < x0 || p == 1.0f) return x;
  const float o = x0 - x0 / p;
  const float s0 = pow(x0, 1.0f - p) / p;
  const float x1 = x0 * pow(2.0f, pv_lx);
  const float k1 = p * s0 * pow(x1, p) / x1;
  const float y1 = s0 * pow(x1, p) + o;
  return (x > x1) ? k1 * (x - x1) + y1 : s0 * pow(x, p) + o;
}

static inline float opencl_odrt_softplus(float x, float s) {
  if (x > 10.0f * s || s < 1e-4f) return x;
  return s * log(fmax(0.0f, 1.0f + exp(x / s)));
}

static inline float opencl_odrt_gauss_window(float x, float w) {
  return exp(-x * x / w);
}

static inline float2 opencl_odrt_opponent(float3 rgb) {
  return (float2)(rgb.x - rgb.z, rgb.y - (rgb.x + rgb.z) / 2.0f);
}

static inline float opencl_odrt_hue_offset(float h, float o) {
  return fmod(h - o + ALCEDO_OPENCL_PI, 2.0f * ALCEDO_OPENCL_PI) - ALCEDO_OPENCL_PI;
}

static inline float3 opencl_odrt_display_gamut_whitepoint(float3 rgb, float tsn, float cwp_lm,
                                                          int display_gamut, int cwp) {
  const float P3D65_TO_XYZ[9] = {0.4865709486f, 0.2656676932f, 0.1982172852f,
                                 0.2289745641f, 0.6917385218f, 0.0792869141f,
                                 0.0f, 0.0451133819f, 1.0439443689f};
  const float XYZ_TO_P3D65[9] = {2.4934969119f, -0.9313836179f, -0.4027107845f,
                                 -0.8294889696f, 1.7626640603f, 0.0236246858f,
                                 0.0358458302f, -0.0761723893f, 0.9568845240f};
  const float XYZ_TO_REC709[9] = {3.2409699419f, -1.5373831776f, -0.4986107603f,
                                  -0.9692436363f, 1.8759675015f, 0.0415550574f,
                                  0.0556300797f, -0.2039769589f, 1.0569715142f};
  const float P3_TO_REC2020[9] = {0.7538330344f, 0.1985973691f, 0.0475695966f,
                                  0.0457438490f, 0.9417772198f, 0.0124789312f,
                                  -0.0012103404f, 0.0176017173f, 0.9836086231f};
  const float CAT_D65_TO_D60[9] = {1.0118224621f, 0.0077887932f, -0.0157783031f,
                                   0.0056168283f, 1.0015064478f, -0.0062851757f,
                                   -0.0003357357f, -0.0010509500f, 0.9273666739f};
  const float CAT_D65_TO_DCI[9] = {0.9910855889f, -0.0273622870f, -0.0183956623f,
                                   -0.0191021916f, 1.0258377790f, -0.0070537254f,
                                   -0.0000805503f, -0.0019598883f, 0.8782384396f};

  rgb = opencl_odrt_apply_matrix(P3D65_TO_XYZ, rgb);
  float3 cwp_neutral = rgb;
  const float cwp_f = pow(tsn, 2.0f * cwp_lm);
  if (display_gamut == 3 && cwp == 2) {
    const float CAT_D60_TO_D65[9] = {0.9883639216f, -0.0076691005f, 0.0167641640f,
                                     -0.0055409619f, 0.9985461235f, 0.0066733211f,
                                     0.0003515370f, 0.0011288375f, 1.0783357620f};
    rgb = opencl_odrt_apply_matrix(CAT_D60_TO_D65, rgb);
  } else if (display_gamut < 3 && cwp == 3) {
    rgb = opencl_odrt_apply_matrix(CAT_D65_TO_D60, rgb);
  }
  rgb = rgb * cwp_f + cwp_neutral * (1.0f - cwp_f);
  if (display_gamut == 0) {
    rgb = opencl_odrt_apply_matrix(XYZ_TO_REC709, rgb);
  } else if (display_gamut == 5) {
    rgb = opencl_odrt_apply_matrix(CAT_D65_TO_DCI, rgb);
  } else {
    rgb = opencl_odrt_apply_matrix(XYZ_TO_P3D65, rgb);
  }
  float cwp_norm = 1.0f;
  if (display_gamut == 0 && cwp == 3) cwp_norm = 0.9559369922f;
  else if ((display_gamut == 1 || display_gamut == 2) && cwp == 3) cwp_norm = 0.9643201867f;
  else if (display_gamut == 3 && cwp == 2) cwp_norm = 0.9233821937f;
  return rgb * (cwp_norm * cwp_f + 1.0f - cwp_f);
}

static inline float3 opencl_open_drt_transform_fwd(float3 input_color,
                                                   __global const OpenClOpenDRTParams* p) {
  const float AP1_TO_XYZ[9] = {0.6524187177f, 0.1271799255f, 0.1708572838f,
                               0.2680640592f, 0.6724644790f, 0.0594714618f,
                               -0.0054699285f, 0.0051828000f, 1.0893448793f};
  const float XYZ_TO_P3D65[9] = {2.4934969119f, -0.9313836179f, -0.4027107845f,
                                 -0.8294889696f, 1.7626640603f, 0.0236246858f,
                                 0.0358458302f, -0.0761723893f, 0.9568845240f};
  const float P3_TO_REC2020[9] = {0.7538330344f, 0.1985973691f, 0.0475695966f,
                                  0.0457438490f, 0.9417772198f, 0.0124789312f,
                                  -0.0012103404f, 0.0176017173f, 0.9836086231f};
  float3 rgb = opencl_odrt_apply_matrix(AP1_TO_XYZ, input_color);
  rgb = opencl_odrt_apply_matrix(XYZ_TO_P3D65, rgb);

  const float3 rs_w = (float3)(p->rs_rw_, 1.0f - p->rs_rw_ - p->rs_bw_, p->rs_bw_);
  float sat_L = rgb.x * rs_w.x + rgb.y * rs_w.y + rgb.z * rs_w.z;
  rgb = (float3)(sat_L) * p->rs_sa_ + rgb * (1.0f - p->rs_sa_);
  rgb += (float3)(p->tn_off_);
  float tsn = opencl_odrt_hypot3(rgb) / ALCEDO_OPENCL_OPEN_DRT_SQRT3;
  rgb = (fabs(tsn) < 1e-8f) ? (float3)(0.0f) : rgb / tsn;

  const float2 opp = opencl_odrt_opponent(rgb);
  float ach_d = 1.25f * opencl_odrt_compress_toe_quadratic(opencl_odrt_hypot2(opp) / 2.0f, 0.25f);
  const float hue = fmod(atan2(opp.x, opp.y) + ALCEDO_OPENCL_PI + 1.10714931f,
                         2.0f * ALCEDO_OPENCL_PI);
  const float3 ha_rgb = (float3)(opencl_odrt_gauss_window(opencl_odrt_hue_offset(hue, 0.1f), 0.66f),
                                 opencl_odrt_gauss_window(opencl_odrt_hue_offset(hue, 4.3f), 0.66f),
                                 opencl_odrt_gauss_window(opencl_odrt_hue_offset(hue, 2.3f), 0.66f));
  const float3 ha_rgb_hs =
      (float3)(opencl_odrt_gauss_window(opencl_odrt_hue_offset(hue, -0.4f), 0.66f), ha_rgb.y,
               opencl_odrt_gauss_window(opencl_odrt_hue_offset(hue, 2.5f), 0.66f));
  const float3 ha_cmy =
      (float3)(opencl_odrt_gauss_window(opencl_odrt_hue_offset(hue, 3.3f), 0.5f),
               opencl_odrt_gauss_window(opencl_odrt_hue_offset(hue, 1.3f), 0.5f),
               opencl_odrt_gauss_window(opencl_odrt_hue_offset(hue, -1.15f), 0.5f));

  if (p->brl_enable_) {
    const float brl_tsf = pow(tsn / (tsn + 1.0f), 1.0f - p->brl_rng_);
    const float brl_exf =
        (p->brl_ + p->brl_r_ * ha_rgb.x + p->brl_g_ * ha_rgb.y + p->brl_b_ * ha_rgb.z) *
        pow(ach_d, 1.0f / p->brl_st_);
    tsn *= pow(2.0f, brl_exf * ((brl_exf < 0.0f) ? brl_tsf : 1.0f - brl_tsf));
  }
  if (p->tn_lcon_enable_) {
    const float lcon_m = pow(2.0f, -p->tn_lcon_);
    float lcon_w = p->tn_lcon_w_ / 4.0f;
    lcon_w *= lcon_w;
    tsn *= opencl_odrt_compress_toe_cubic(p->ts_x0_, lcon_m, lcon_w, 1) / p->ts_x0_;
    tsn = opencl_odrt_compress_toe_cubic(tsn, lcon_m, lcon_w, 0);
  }
  if (p->tn_hcon_enable_) {
    tsn = opencl_odrt_contrast_high(tsn, pow(2.0f, p->tn_hcon_), p->tn_hcon_pv_, p->tn_hcon_st_);
  }

  const float tsn_pt = opencl_odrt_compress_hyperbolic_power(tsn, p->ts_s1_, p->ts_p_);
  const float tsn_const = opencl_odrt_compress_hyperbolic_power(tsn, p->s_Lp100_, p->ts_p_);
  tsn = opencl_odrt_compress_hyperbolic_power(tsn, p->ts_s_, p->ts_p_);

  if (p->hc_enable_) {
    float hc_ts = 1.0f - tsn_const;
    float hc_c = (hc_ts * (1.0f - ach_d) + ach_d * (1.0f - hc_ts)) * ach_d * ha_rgb.x;
    hc_ts = pow(hc_ts, 1.0f / p->hc_r_rng_);
    const float hc_f = p->hc_r_ * (hc_c - 2.0f * hc_c * hc_ts) + 1.0f;
    rgb.y *= hc_f;
    rgb.z *= hc_f;
  }
  if (p->hs_rgb_enable_) {
    const float3 hs_rgb =
        (float3)(ha_rgb_hs.x * ach_d * pow(tsn_pt, 1.0f / p->hs_r_rng_),
                 ha_rgb_hs.y * ach_d * pow(tsn_pt, 1.0f / p->hs_g_rng_),
                 ha_rgb_hs.z * ach_d * pow(tsn_pt, 1.0f / p->hs_b_rng_));
    rgb += (float3)((hs_rgb.z * -p->hs_b_) - (hs_rgb.y * -p->hs_g_),
                    (hs_rgb.x * p->hs_r_) - (hs_rgb.z * -p->hs_b_),
                    (hs_rgb.y * -p->hs_g_) - (hs_rgb.x * p->hs_r_));
  }
  if (p->hs_cmy_enable_) {
    const float tsn_pt_compl = 1.0f - tsn_pt;
    const float3 hs_cmy =
        (float3)(ha_cmy.x * ach_d * pow(tsn_pt_compl, 1.0f / p->hs_c_rng_),
                 ha_cmy.y * ach_d * pow(tsn_pt_compl, 1.0f / p->hs_m_rng_),
                 ha_cmy.z * ach_d * pow(tsn_pt_compl, 1.0f / p->hs_y_rng_));
    rgb += (float3)((hs_cmy.z * p->hs_y_) - (hs_cmy.y * p->hs_m_),
                    (hs_cmy.x * -p->hs_c_) - (hs_cmy.z * p->hs_y_),
                    (hs_cmy.y * p->hs_m_) - (hs_cmy.x * -p->hs_c_));
  }

  const float pt_lml_p = 1.0f + 4.0f * (1.0f - tsn_pt) *
                                    (p->pt_lml_ + p->pt_lml_r_ * ha_rgb_hs.x +
                                     p->pt_lml_g_ * ha_rgb_hs.y + p->pt_lml_b_ * ha_rgb_hs.z);
  float ptf = pow(1.0f - pow(tsn_pt, pt_lml_p),
                  (1.0f - ach_d * (p->pt_lmh_r_ * ha_rgb_hs.x + p->pt_lmh_b_ * ha_rgb_hs.z)) *
                      (1.0f - p->pt_lmh_ * ach_d));
  if (p->ptm_enable_) {
    float ptm_low_f = 1.0f;
    if (p->ptm_low_st_ != 0.0f && p->ptm_low_rng_ != 0.0f) {
      ptm_low_f = 1.0f + p->ptm_low_ * exp(-2.0f * ach_d * ach_d / p->ptm_low_st_) *
                             pow(1.0f - tsn_const, 1.0f / p->ptm_low_rng_);
    }
    float ptm_high_f = 1.0f;
    if (p->ptm_high_st_ != 0.0f && p->ptm_high_rng_ != 0.0f) {
      ptm_high_f = 1.0f + p->ptm_high_ * exp(-2.0f * ach_d * ach_d / p->ptm_high_st_) *
                              pow(tsn_pt, 1.0f / (4.0f * p->ptm_high_rng_));
    }
    ptf *= ptm_low_f * ptm_high_f;
  }
  rgb = rgb * ptf + (float3)(1.0f - ptf);

  sat_L = rgb.x * rs_w.x + rgb.y * rs_w.y + rgb.z * rs_w.z;
  rgb = ((float3)(sat_L) * p->rs_sa_ - rgb) / (p->rs_sa_ - 1.0f);
  rgb = opencl_odrt_display_gamut_whitepoint(rgb, tsn_const, p->cwp_lm_, p->display_gamut_,
                                             p->creative_white_);
  if (p->brlp_enable_) {
    float brlp_ach_d = opencl_odrt_hypot2(opencl_odrt_opponent(rgb)) / 4.0f;
    brlp_ach_d = 1.1f * (brlp_ach_d * brlp_ach_d / (brlp_ach_d + 0.1f));
    const float3 brlp_ha_rgb = ha_rgb * ach_d;
    const float brlp_m = p->brlp_ + p->brlp_r_ * brlp_ha_rgb.x +
                         p->brlp_g_ * brlp_ha_rgb.y + p->brlp_b_ * brlp_ha_rgb.z;
    rgb *= pow(2.0f, brlp_m * brlp_ach_d * tsn);
  }
  if (p->ptl_enable_) {
    rgb = (float3)(opencl_odrt_softplus(rgb.x, p->ptl_c_),
                   opencl_odrt_softplus(rgb.y, p->ptl_m_),
                   opencl_odrt_softplus(rgb.z, p->ptl_y_));
  }
  tsn *= p->ts_m2_;
  tsn = opencl_odrt_compress_toe_quadratic(tsn, p->tn_toe_);
  tsn *= p->ts_dsc_;
  rgb *= tsn;
  if (p->display_gamut_ == 2) {
    rgb = fmax(rgb, (float3)(0.0f));
    rgb = opencl_odrt_apply_matrix(P3_TO_REC2020, rgb);
  }
  if (p->clamp_) {
    rgb = clamp(rgb, (float3)(0.0f), (float3)(1.0f));
  }
  return rgb;
}

// === To Output operator =======================================================

static inline float4 opencl_output_op(float4 px, __global OpenClFusedParams* params) {
  if (params->to_output_enabled_ == 0u) {
    return px;
  }

  const float3 aces_linear =
      (float3)(opencl_acescc_decode(px.x), opencl_acescc_decode(px.y),
               opencl_acescc_decode(px.z));
  float3 odt_color;
  if (params->to_output_params_.method_ == 0) {
    odt_color = opencl_aces_output_transform_fwd(aces_linear,
                                                 &params->to_output_params_.aces_params_);
  } else {
    odt_color = opencl_open_drt_transform_fwd(aces_linear,
                                              &params->to_output_params_.open_drt_params_);
  }

  float3 cv = opencl_display_encoding(odt_color, params->to_output_params_.limit_to_display_matx,
                                      params->to_output_params_.eotf_,
                                      params->to_output_params_.display_linear_scale_);
  return (float4)(cv.x, cv.y, cv.z, px.w);
}

#endif  // ALCEDO_OPENCL_EDIT_PIPELINE_CST_CL
