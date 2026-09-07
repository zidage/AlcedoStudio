//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "common.metal"

constant int kMetalOdtMethodAces20   = 0;
constant int kMetalOdtMethodOpenDrt  = 1;
constant int kMetalEotfLinear        = 0;
constant int kMetalEotfSt2084        = 1;
constant int kMetalEotfHlg           = 2;
constant int kMetalEotfGamma26       = 3;
constant int kMetalEotfBt1886        = 4;
constant int kMetalEotfGamma22       = 5;
constant int kMetalEotfGamma18       = 6;
constant int kMetalOdtTableSize      = 360;
constant int kMetalOdtTotalTableSize = 362;
constant int kMetalOdtBaseIndex      = 1;
constant float kMetalHueLimit        = 360.0f;

constant float kAp0ToAp1Mat[9] = {
    1.4514393161f,  -0.2365107469f, -0.2149285693f,
   -0.0765537734f,   1.1762296998f, -0.0996759264f,
    0.0083161484f,  -0.0060324498f,  0.9977163014f};

constant float kAcesccLog2Min      = -15.0f;
constant float kAcesccLog2Denorm   = -16.0f;
constant float kAcesccDenormTrans  = 0.00003051757812f;
constant float kAcesccDenormOffset = 0.00001525878906f;
constant float kAcesccA            = 9.72f;
constant float kAcesccB            = 17.52f;

constant float kRgcLimCyan    = 1.147f;
constant float kRgcLimMagenta = 1.264f;
constant float kRgcLimYellow  = 1.312f;
constant float kRgcThrCyan    = 0.815f;
constant float kRgcThrMagenta = 0.803f;
constant float kRgcThrYellow  = 0.880f;
constant float kRgcPwr        = 1.2f;

constant float kPqM1 = 0.1593017578125f;
constant float kPqM2 = 78.84375f;
constant float kPqC1 = 0.8359375f;
constant float kPqC2 = 18.8515625f;
constant float kPqC3 = 18.6875f;
constant float kPqC  = 10000.0f;

constant float kRefLuminance        = 100.0f;
constant float kJScale              = 100.0f;
constant float kCamNlOffset         = 27.13f;
constant float kModelGamma          = 1.13705599f;
constant float kSmoothCusps         = 0.12f;
constant float kCuspMidBlend        = 1.3f;
constant float kFocusGainBlend      = 0.3f;
constant float kCompressionThreshold = 0.75f;

constant float kAp1ToAp0[9] = {
    0.695452213f, 0.0447945632f, -0.00552588236f,
    0.140678704f, 0.859671116f,   0.00402521016f,
    0.163869068f, 0.0955343172f,  1.00150073f};

constant float kOpenDrtSqrt3 = 1.7320508075688772f;
constant float kOpenDrtPi    = 3.1415926535897932f;

constant float kOpenDrtAp1ToXyz[9] = {
    0.6524187177f, 0.1271799255f, 0.1708572838f,
    0.2680640592f, 0.6724644790f, 0.0594714618f,
   -0.0054699285f, 0.0051828000f, 1.0893448793f};
constant float kOpenDrtP3D65ToXyz[9] = {
    0.4865709486f, 0.2656676932f, 0.1982172852f,
    0.2289745641f, 0.6917385218f, 0.0792869141f,
    0.0f,          0.0451133819f, 1.0439443689f};
constant float kOpenDrtXyzToP3D65[9] = {
    2.4934969119f, -0.9313836179f, -0.4027107845f,
   -0.8294889696f,  1.7626640603f,  0.0236246858f,
    0.0358458302f, -0.0761723893f,  0.9568845240f};
constant float kOpenDrtXyzToRec709[9] = {
    3.2409699419f, -1.5373831776f, -0.4986107603f,
   -0.9692436363f,  1.8759675015f,  0.0415550574f,
    0.0556300797f, -0.2039769589f,  1.0569715142f};
constant float kOpenDrtP3ToRec2020[9] = {
    0.7538330344f, 0.1985973691f, 0.0475695966f,
    0.0457438490f, 0.9417772198f, 0.0124789312f,
   -0.0012103404f, 0.0176017173f, 0.9836086231f};

constant float kOpenDrtCatDciToD93[9] = {
    0.9656850100f, 0.0018374524f, 0.0912967324f,
    0.0005145721f, 0.9651667476f, 0.0360146537f,
    0.0015425049f, 0.0070265178f, 1.4728747606f};
constant float kOpenDrtCatDciToD75[9] = {
    0.9901207685f, 0.0151389474f, 0.0511047691f,
    0.0102197211f, 0.9717181325f, 0.0200536624f,
    0.0007430727f, 0.0042176349f, 1.2795965672f};
constant float kOpenDrtCatDciToD65[9] = {
    1.0095160007f, 0.0269675441f, 0.0213620812f,
    0.0187991038f, 0.9753303528f, 0.0082273334f,
    0.0001345433f, 0.0021790350f, 1.1386636496f};
constant float kOpenDrtCatDciToD60[9] = {
    1.0215952396f, 0.0348486789f, 0.0037125200f,
    0.0244968776f, 0.9769372344f, 0.0012030154f,
   -0.0002339159f, 0.0009866878f, 1.0559426546f};
constant float kOpenDrtCatDciToD55[9] = {
    1.0359457731f, 0.0450937562f, -0.0157573819f,
    0.0318740681f, 0.9777445197f, -0.0065574497f,
   -0.0006536094f, -0.0002973722f, 0.9663277864f};
constant float kOpenDrtCatDciToD50[9] = {
    1.0530687571f, 0.0581297316f, -0.0376100838f,
    0.0412359424f, 0.9776936769f, -0.0152792223f,
   -0.0011377768f, -0.0017075930f, 0.8673683405f};
constant float kOpenDrtCatD65ToD93[9] = {
    0.9570342302f, -0.0247171503f, 0.0624028593f,
   -0.0179296955f, 0.9900198579f,  0.0248119533f,
    0.0012758914f, 0.0042791907f,  1.2934571505f};
constant float kOpenDrtCatD65ToD75[9] = {
    0.9810010791f, -0.0116619254f, 0.0265614092f,
   -0.0084348805f, 0.9965060949f,  0.0105696544f,
    0.0005528096f, 0.0017984081f,  1.1237472296f};
constant float kOpenDrtCatD65ToD60[9] = {
    1.0118224621f, 0.0077887932f, -0.0157783031f,
    0.0056168283f, 1.0015064478f, -0.0062851757f,
   -0.0003357357f, -0.0010509500f, 0.9273666739f};
constant float kOpenDrtCatD65ToD55[9] = {
    1.0258508921f, 0.0179439820f, -0.0332137793f,
    0.0129133854f, 1.0021477938f, -0.0132421032f,
   -0.0007199403f, -0.0021810681f, 0.8486801386f};
constant float kOpenDrtCatD65ToD50[9] = {
    1.0425740480f, 0.0308911763f, -0.0528126210f,
    0.0221935362f, 1.0018566847f, -0.0210737623f,
   -0.0011648831f, -0.0034205271f, 0.7617890835f};
constant float kOpenDrtCatD65ToDci[9] = {
    0.9910855889f, -0.0273622870f, -0.0183956623f,
   -0.0191021916f,  1.0258377790f, -0.0070537254f,
   -0.0000805503f, -0.0019598883f,  0.8782384396f};
constant float kOpenDrtCatD60ToD93[9] = {
    0.9460569024f, -0.0319503024f, 0.0831701458f,
   -0.0231979694f, 0.9887458086f,  0.0330617502f,
    0.0016920343f, 0.0057232874f,  1.3948310614f};
constant float kOpenDrtCatD60ToD75[9] = {
    0.9696599841f, -0.0191383120f, 0.0450099558f,
   -0.0138545772f, 0.9951338172f,  0.0179062262f,
    0.0009314523f, 0.0030600820f,  1.2117980719f};
constant float kOpenDrtCatD60ToD65[9] = {
    0.9883639216f, -0.0076691005f, 0.0167641640f,
   -0.0055409619f, 0.9985461235f,  0.0066733211f,
    0.0003515370f, 0.0011288375f,  1.0783357620f};
constant float kOpenDrtCatD60ToD55[9] = {
    1.0138028860f, 0.0100131510f, -0.0184983462f,
    0.0072056516f, 1.0005768538f, -0.0073752999f,
   -0.0004011337f, -0.0012143496f, 0.9151356816f};
constant float kOpenDrtCatD60ToD50[9] = {
    1.0302526951f, 0.0227910466f, -0.0392656922f,
    0.0163766481f, 1.0002059937f, -0.0156668238f,
   -0.0008645768f, -0.0025466848f, 0.8214220405f};

static inline float3 mult_f3_f33(float3 v, constant float* m) {
  return float3(v.x * m[0] + v.y * m[3] + v.z * m[6], v.x * m[1] + v.y * m[4] + v.z * m[7],
                v.x * m[2] + v.y * m[5] + v.z * m[8]);
}

static inline float3 apply_matrix3x3(constant float* mat, float3 v) {
  return float3(mat[0] * v.x + mat[1] * v.y + mat[2] * v.z,
                mat[3] * v.x + mat[4] * v.y + mat[5] * v.z,
                mat[6] * v.x + mat[7] * v.y + mat[8] * v.z);
}

static inline float3 mult_f_f3(float3 v, float s) { return v * s; }

static inline float3 clamp_f3(float3 v, float min_val, float max_val) {
  return clamp(v, float3(min_val), float3(max_val));
}

static inline float3 pow_f3(float3 v, float expv) {
  return float3(pow(v.x, expv), pow(v.y, expv), pow(v.z, expv));
}

static inline float rgc_compress_curve(float dist, float lim, float thr, float pwr) {
  if (dist < thr) {
    return dist;
  }
  float t_diff      = lim - thr;
  float one_minus_t = 1.0f - thr;
  float inner_pow   = pow(one_minus_t / t_diff, -pwr);
  float denom       = pow(fmax(0.0f, inner_pow - 1.0f), 1.0f / pwr);
  float scl         = (denom > 1e-6f) ? (t_diff / denom) : 0.0f;
  float nd          = (dist - thr) / scl;
  float p           = pow(fmax(0.0f, nd), pwr);
  return thr + scl * nd / pow(1.0f + p, 1.0f / pwr);
}

static inline float acescc_encode(float x) {
  const float encode_floor = (kAcesccLog2Denorm + kAcesccA) / kAcesccB;
  if (x <= 0.0f) {
    return encode_floor + x;
  }
  if (x < kAcesccDenormTrans) {
    return (log2(kAcesccDenormOffset + x * 0.5f) + kAcesccA) / kAcesccB;
  }
  return (log2(x) + kAcesccA) / kAcesccB;
}

static inline float acescc_decode(float acescc) {
  const float encode_floor     = (kAcesccLog2Denorm + kAcesccA) / kAcesccB;
  const float denorm_threshold = (kAcesccLog2Min + kAcesccA) / kAcesccB;
  if (acescc < encode_floor) {
    return acescc - encode_floor;
  }
  if (acescc <= denorm_threshold) {
    return (exp2(acescc * kAcesccB - kAcesccA) - kAcesccDenormOffset) * 2.0f;
  }
  return exp2(acescc * kAcesccB - kAcesccA);
}

static inline float gamma22_encode(float linear) { return pow(fmax(linear, 0.0f), 1.0f / 2.2f); }

static inline float Tonescale_fwd(float x, const constant MetalTSParams& params) {
  if (!isfinite(x)) {
    if (x > 0.0f) {
      const float f_inf = params.m_2_;
      const float h_inf = fmax(0.0f, f_inf * f_inf / (f_inf + params.t_1_));
      return h_inf * params.n_r_;
    }
    return 0.0f;
  }
  const float denom = x + params.s_2_;
  const float ratio = (denom > 1e-7f) ? (fmax(0.0f, x) / denom) : 0.0f;
  const float f     = params.m_2_ * pow(ratio, params.g_);
  const float h     = fmax(0.0f, f * f / (f + params.t_1_));
  return h * params.n_r_;
}

static inline float moncurve_fwd(float x, float gamma, float offs) {
  const float fs =
      ((gamma - 1.0f) / offs) * pow(offs * gamma / ((gamma - 1.0f) * (1.0f + offs)), gamma);
  const float xb = offs / (gamma - 1.0f);
  return (x >= xb) ? pow((x + offs) / (1.0f + offs), gamma) : x * fs;
}

static inline float moncurve_inv(float y, float gamma, float offs) {
  const float yb = pow(offs * gamma / ((gamma - 1.0f) * (1.0f + offs)), gamma);
  const float rs =
      pow((gamma - 1.0f) / offs, gamma - 1.0f) * pow((1.0f + offs) / gamma, gamma);
  return (y >= yb) ? (1.0f + offs) * pow(y, 1.0f / gamma) - offs : y * rs;
}

static inline float3 moncurve_fwd_f3(float3 v, float gamma, float offs) {
  return float3(moncurve_fwd(v.x, gamma, offs), moncurve_fwd(v.y, gamma, offs),
                moncurve_fwd(v.z, gamma, offs));
}

static inline float3 moncurve_inv_f3(float3 v, float gamma, float offs) {
  return float3(moncurve_inv(v.x, gamma, offs), moncurve_inv(v.y, gamma, offs),
                moncurve_inv(v.z, gamma, offs));
}

static inline float bt1886_fwd(float v, float gamma, float lw = 1.0f, float lb = 0.0f) {
  const float a = pow(pow(lw, 1.0f / gamma) - pow(lb, 1.0f / gamma), gamma);
  const float b = pow(lb, 1.0f / gamma) / (pow(lw, 1.0f / gamma) - pow(lb, 1.0f / gamma));
  return a * pow(fmax(v + b, 0.0f), gamma);
}

static inline float bt1886_inv(float l, float gamma, float lw = 1.0f, float lb = 0.0f) {
  const float a = pow(pow(lw, 1.0f / gamma) - pow(lb, 1.0f / gamma), gamma);
  const float b = pow(lb, 1.0f / gamma) / (pow(lw, 1.0f / gamma) - pow(lb, 1.0f / gamma));
  return pow(fmax(l / a, 0.0f), 1.0f / gamma) - b;
}

static inline float3 bt1886_inv_f3(float3 v, float gamma, float lw = 1.0f, float lb = 0.0f) {
  return float3(bt1886_inv(v.x, gamma, lw, lb), bt1886_inv(v.y, gamma, lw, lb),
                bt1886_inv(v.z, gamma, lw, lb));
}

static inline float ST2084_to_Y(float n) {
  float np = pow(n, 1.0f / kPqM2);
  float l  = np - kPqC1;
  if (l < 0.0f) l = 0.0f;
  l = l / (kPqC2 - kPqC3 * np);
  l = pow(l, 1.0f / kPqM1);
  return l * kPqC;
}

static inline float Y_to_ST2084(float c) {
  const float l  = c / kPqC;
  const float lm = pow(l, kPqM1);
  const float n  = (kPqC1 + kPqC2 * lm) / (1.0f + kPqC3 * lm);
  return pow(n, kPqM2);
}

static inline float3 Y_to_ST2084_f3(float3 c) {
  return float3(Y_to_ST2084(c.x), Y_to_ST2084(c.y), Y_to_ST2084(c.z));
}

static inline float3 ST2084_to_Y_f3(float3 n) {
  return float3(ST2084_to_Y(n.x), ST2084_to_Y(n.y), ST2084_to_Y(n.z));
}

static inline float3 HLG_from_display_linear_1000nits_f3(float3 display_linear) {
  const float yd = 0.2627f * display_linear.x + 0.6780f * display_linear.y + 0.0593f * display_linear.z;
  float3 rgb     = display_linear;
  if (yd > 0.0f) {
    rgb = mult_f_f3(rgb, pow(yd, (1.0f - 1.2f) / 1.2f));
  }
  const float a = 0.17883277f;
  const float b = 0.28466892f;
  const float c = 0.55991073f;
  rgb.x         = (rgb.x <= (1.0f / 12.0f)) ? sqrt(3.0f * rgb.x) : a * log(12.0f * rgb.x - b) + c;
  rgb.y         = (rgb.y <= (1.0f / 12.0f)) ? sqrt(3.0f * rgb.y) : a * log(12.0f * rgb.y - b) + c;
  rgb.z         = (rgb.z <= (1.0f / 12.0f)) ? sqrt(3.0f * rgb.z) : a * log(12.0f * rgb.z - b) + c;
  return rgb;
}

static inline float3 HLG_to_display_linear_1000nits_f3(float3 hlg_signal) {
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
    rgb = mult_f_f3(rgb, pow(ys, 0.2f));
  }
  return rgb;
}

static inline float3 eotf_inv(float3 rgb_linear_in, int otf_type) {
  const float3 rgb_linear = float3(fmax(0.0f, rgb_linear_in.x), fmax(0.0f, rgb_linear_in.y),
                                   fmax(0.0f, rgb_linear_in.z));
  if (otf_type == kMetalEotfLinear) return rgb_linear;
  if (otf_type == kMetalEotfSt2084) return Y_to_ST2084_f3(rgb_linear);
  if (otf_type == kMetalEotfHlg) return HLG_from_display_linear_1000nits_f3(rgb_linear);
  if (otf_type == kMetalEotfBt1886) return bt1886_inv_f3(rgb_linear, 2.4f, 1.0f, 0.0f);
  if (otf_type == kMetalEotfGamma26) return pow_f3(rgb_linear, 1.0f / 2.6f);
  if (otf_type == kMetalEotfGamma22) return pow_f3(rgb_linear, 1.0f / 2.2f);
  if (otf_type == kMetalEotfGamma18) return pow_f3(rgb_linear, 1.0f / 1.8f);
  return moncurve_inv_f3(rgb_linear, 2.4f, 0.055f);
}

static inline float3 eotf(float3 rgb_cv, int eotf_enum) {
  switch (eotf_enum) {
    case kMetalEotfLinear:
      return rgb_cv;
    case kMetalEotfSt2084:
      return mult_f_f3(ST2084_to_Y_f3(rgb_cv), 1.0f / kRefLuminance);
    case kMetalEotfHlg:
      return HLG_to_display_linear_1000nits_f3(rgb_cv);
    case kMetalEotfBt1886:
      return pow_f3(rgb_cv, 2.6f);
    case kMetalEotfGamma26:
      return pow_f3(rgb_cv, 2.6f);
    case kMetalEotfGamma22:
      return pow_f3(rgb_cv, 2.2f);
    case kMetalEotfGamma18:
      return pow_f3(rgb_cv, 1.8f);
    default:
      return moncurve_fwd_f3(rgb_cv, 2.4f, 0.055f);
  }
}

static inline float3 ApplyWhiteScale(float3 rgb, constant float* mat_limit_to_display) {
  const float3 rgb_w_f = mult_f3_f33(float3(1.0f), mat_limit_to_display);
  const float scale    = 1.0f / fmax(fmax(rgb_w_f.x, rgb_w_f.y), rgb_w_f.z);
  return mult_f_f3(rgb, scale);
}

static inline float3 DisplayEncoding(float3 rgb, constant float* mat_limit_to_display, int eotf_num,
                                     float linear_scale = 1.0f) {
  const float3 rgb_disp_linear   = mult_f3_f33(rgb, mat_limit_to_display);
  const float3 rgb_display_scale = mult_f_f3(rgb_disp_linear, linear_scale);
  return eotf_inv(rgb_display_scale, eotf_num);
}

static inline float3 DisplayDecoding(float3 rgb_cv, constant float* mat_display_to_limit, int eotf_num,
                                     float linear_scale = 1.0f) {
  const float3 rgb_display_linear = eotf(rgb_cv, eotf_num);
  const float3 rgb_limit_scaled   = mult_f_f3(rgb_display_linear, 1.0f / linear_scale);
  return mult_f3_f33(rgb_limit_scaled, mat_display_to_limit);
}

static inline bool isfinite_f(float x) { return isfinite(x); }
static inline float clamp_f(float x, float lo, float hi) { return fmin(hi, fmax(lo, x)); }
static inline int clamp_i(int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline float safe_sqrt(float x) { return sqrt(fmax(x, 0.0f)); }

static inline float safe_div(float a, float b, float eps = 1e-7f) {
  const float ab = fabs(b);
  const float bb = (ab < eps) ? copysign(eps, b) : b;
  return a / bb;
}

static inline float safe_log10_ratio(float num, float den, float eps = 1e-7f) {
  return log10(fmax(num, eps) / fmax(den, eps));
}

static inline float safe_pow_pos(float base, float expv) {
  const float b = fmax(base, 0.0f);
  if (b == 0.0f) {
    return (expv > 0.0f) ? 0.0f : 1.0f;
  }
  return pow(b, expv);
}

struct HueDependentGamutParams {
  float2 JMcusp;
  float  gamma_bottom_inv;
  float  gamma_top_inv;
  float  focus_J;
  float  analytical_threshold;
};

static inline float3 clamp_AP1(float3 ap1_color, float clamp_lower_limit, float clamp_upper_limit) {
  const float3 ap1_clamped = clamp_f3(ap1_color, clamp_lower_limit, clamp_upper_limit);
  return mult_f3_f33(ap1_clamped, kAp1ToAp0);
}

static inline float wrap_to_360(float hue) {
  if (!isfinite_f(hue)) return 0.0f;
  float y = fmod(hue, 360.0f);
  if (y < 0.0f) y += 360.0f;
  return y;
}

static inline float radians_to_degrees(float rad) { return rad * (180.0f / kOpenDrtPi); }
static inline float degrees_to_radians(float deg) { return deg * (kOpenDrtPi / 180.0f); }

static inline int hue_position_in_uniform_table(float hue, int table_size) {
  const float wrapped = wrap_to_360(hue);
  const float pos     = wrapped * (float(table_size) / kMetalHueLimit);
  return clamp_i(int(pos), 0, table_size - 1);
}

static inline float lerp_f(float a, float b, float t) { return a + (b - a) * t; }

static inline float reach_M_from_table(float h, const constant MetalODTParams& p) {
  const float hw   = wrap_to_360(h);
  const float pos  = hw * (float(kMetalOdtTableSize) / kMetalHueLimit);
  const int   base = clamp_i(int(pos), 0, kMetalOdtTableSize - 1);
  const float t    = clamp_f(pos - float(base), 0.0f, 1.0f);
  const int   i_lo = base + kMetalOdtBaseIndex;
  const int   i_hi = i_lo + 1;
  return lerp_f(p.table_reach_M_[i_lo], p.table_reach_M_[i_hi], t);
}

static inline float pacrc_fwd_base(float rc) {
  const float fl_y = pow(rc, 0.42f);
  return fl_y / (kCamNlOffset + fl_y);
}

static inline float pacrc_fwd(float v) {
  return copysign(pacrc_fwd_base(fabs(v)), v);
}

static inline float pacrc_inv_base(float ra) {
  const float ra_lim = fmin(ra, 0.99f);
  const float fl_y   = (kCamNlOffset * ra_lim) / (1.0f - ra_lim);
  return pow(fl_y, 1.0f / 0.42f);
}

static inline float pacrc_inv(float v) {
  return copysign(pacrc_inv_base(fabs(v)), v);
}

static inline float Achromatic_n_to_J(float a, float cz) { return kJScale * pow(a, cz); }
static inline float J_to_Achromatic_n(float j, float inv_cz) { return pow(j * (1.0f / kJScale), inv_cz); }

static inline float3 RGB_to_Aab(float3 rgb, const constant MetalJMhParams& p) {
  const float3 rgb_m = mult_f3_f33(rgb, p.MATRIX_RGB_to_CAM16_c_);
  const float3 rgb_a = float3(pacrc_fwd(rgb_m.x), pacrc_fwd(rgb_m.y), pacrc_fwd(rgb_m.z));
  return mult_f3_f33(rgb_a, p.MATRIX_cone_response_to_Aab_);
}

static inline float3 Aab_to_JMh(float3 aab, const constant MetalJMhParams& p) {
  const float mask  = aab.x > 0.0f ? 1.0f : 0.0f;
  const float j     = Achromatic_n_to_J(aab.x, p.cz_) * mask;
  const float m     = safe_sqrt(aab.y * aab.y + aab.z * aab.z) * mask;
  const float h_rad = atan2(aab.z, aab.y);
  const float h     = wrap_to_360(radians_to_degrees(h_rad)) * mask;
  return float3(j, m, h);
}

static inline float3 JMh_to_Aab(float3 jmh, const constant MetalJMhParams& p) {
  const float h_rad = degrees_to_radians(jmh.z);
  return float3(J_to_Achromatic_n(jmh.x, p.inv_cz_), jmh.y * cos(h_rad), jmh.y * sin(h_rad));
}

static inline float3 Aab_to_RGB(float3 aab, const constant MetalJMhParams& p) {
  const float3 rgb_a = mult_f3_f33(aab, p.MATRIX_Aab_to_cone_response_);
  const float3 rgb_m = float3(pacrc_inv(rgb_a.x), pacrc_inv(rgb_a.y), pacrc_inv(rgb_a.z));
  return mult_f3_f33(rgb_m, p.MATRIX_CAM16_c_to_RGB_);
}

static inline float3 RGB_to_JMh(float3 color, const constant MetalJMhParams& p) {
  return Aab_to_JMh(RGB_to_Aab(color, p), p);
}

static inline float3 JMh_to_RGB(float3 jmh, const constant MetalJMhParams& p) {
  return Aab_to_RGB(JMh_to_Aab(jmh, p), p);
}

static inline float A_to_Y(float a, const constant MetalJMhParams& p) {
  return pacrc_inv_base(p.A_w_J_ * a) / p.F_L_n_;
}

static inline float J_to_Y(float j, const constant MetalJMhParams& p) {
  return A_to_Y(J_to_Achromatic_n(fabs(j), p.inv_cz_), p);
}

static inline float Y_to_J(float y, const constant MetalJMhParams& p) {
  const float ra = pacrc_fwd_base(fabs(y) * p.F_L_n_);
  const float j  = Achromatic_n_to_J(ra * p.inv_A_w_J_, p.cz_);
  return copysign(j, y);
}

static inline float chroma_compress_norm(float h, float chroma_compress_scale) {
  const float hr      = degrees_to_radians(h);
  const float a       = cos(hr);
  const float b       = sin(hr);
  const float cos_hr2 = a * a - b * b;
  const float sin_hr2 = 2.0f * a * b;
  const float cos_hr3 = 4.0f * a * a * a - 3.0f * a;
  const float sin_hr3 = 3.0f * b - 4.0f * b * b * b;
  const float m       = 11.34072f * a + 16.46899f * cos_hr2 + 7.88380f * cos_hr3 +
                  14.66441f * b - 6.37224f * sin_hr2 + 9.19364f * sin_hr3 + 77.12896f;
  return m * chroma_compress_scale;
}

static inline float reinhard_remap(float scale, float nd, bool invert = false) {
  if (invert) {
    if (nd >= 1.0f) return scale;
    return scale * -(nd / (nd - 1.0f));
  }
  return scale * nd / (1.0f + nd);
}

static inline float toe(float x, float limit, float k1_in, float k2_in, bool invert = false) {
  if (x > limit) return x;
  const float k2 = fmax(k2_in, 0.001f);
  const float k1 = sqrt(k1_in * k1_in + k2 * k2);
  const float k3 = (limit + k1) / (limit + k2);
  if (invert) {
    return (x * x + k1 * x) / (k3 * (x + k2));
  }
  const float minus_b = k3 * x - k1;
  const float minus_c = k2 * k3 * x;
  return 0.5f * (minus_b + safe_sqrt(minus_b * minus_b + 4.0f * minus_c));
}

static inline float3 chroma_compress_fwd(float3 jmh, float tonemapped_j, const constant MetalODTParams& p,
                                         bool invert = false) {
  float m_compr = jmh.y;
  if (jmh.y != 0.0f) {
    const float limit_j          = fmax(p.limit_J_max, 1e-6f);
    const float jts              = fmax(tonemapped_j, 0.0f);
    const float nj               = clamp_f(jts / limit_j, 0.0f, 1.0f);
    const float snj              = fmax(0.0f, 1.0f - nj);
    const float mnorm            = chroma_compress_norm(jmh.z, p.chroma_compress_scale);
    if (!isfinite_f(mnorm) || fabs(mnorm) < 1e-6f) {
      return float3(jts, 0.0f, jmh.z);
    }
    const float limit            = fmax(safe_pow_pos(nj, p.model_gamma_inv) * reach_M_from_table(jmh.z, p) / mnorm,
                               0.0f);
    const float toe_limit        = limit - 0.001f;
    const float toe_snj_sat      = snj * p.sat;
    const float toe_sqrt_nj_thr  = sqrt(nj * nj + p.sat_thr);
    const float toe_nj_compr     = nj * p.compr;
    const float ratio            = (fabs(jmh.x) < 1e-6f) ? 1.0f : (jts / fabs(jmh.x));
    m_compr                      = jmh.y * safe_pow_pos(ratio, p.model_gamma_inv);
    m_compr                      = m_compr / mnorm;
    m_compr                      = limit - toe(limit - m_compr, toe_limit, toe_snj_sat, toe_sqrt_nj_thr, false);
    m_compr                      = toe(m_compr, limit, toe_nj_compr, snj, false);
    m_compr                      = m_compr * mnorm;
  }
  (void)invert;
  return float3(tonemapped_j, m_compr, jmh.z);
}

static inline float3 tonemap_and_compress_fwd(float3 jmh, const constant MetalODTParams& p) {
  const float linear       = J_to_Y(jmh.x, p.input_params_) / kRefLuminance;
  const float tonemapped_y = Tonescale_fwd(linear, p.ts_);
  const float j_ts         = Y_to_J(tonemapped_y, p.input_params_);
  return chroma_compress_fwd(jmh, j_ts, p);
}

static inline float gamut_cusp_hue(constant MetalODTParams& p, int index) {
  return p.table_gamut_cusps_[index][2];
}

static inline float2 cusp_from_table(float h, const constant MetalODTParams& p) {
  const float hw = wrap_to_360(h);
  int low_i      = 0;
  int high_i     = kMetalOdtBaseIndex + kMetalOdtTableSize;
  int i          = kMetalOdtBaseIndex + hue_position_in_uniform_table(hw, kMetalOdtTableSize);
  for (int k = 0; k < 10 && (low_i + 1 < high_i); ++k) {
    const float h_i = gamut_cusp_hue(p, i);
    if (hw > h_i) {
      low_i = i;
    } else {
      high_i = i;
    }
    i = (low_i + high_i) >> 1;
  }
  const int lo_idx = high_i - 1;
  const int hi_idx = high_i;
  const float lo_h = p.table_gamut_cusps_[lo_idx][2];
  const float hi_h = p.table_gamut_cusps_[hi_idx][2];
  const float denom = hi_h - lo_h;
  const float t     = clamp_f((fabs(denom) > 1e-6f) ? (hw - lo_h) / denom : 0.0f, 0.0f, 1.0f);
  return float2(lerp_f(p.table_gamut_cusps_[lo_idx][0], p.table_gamut_cusps_[hi_idx][0], t),
                lerp_f(p.table_gamut_cusps_[lo_idx][1], p.table_gamut_cusps_[hi_idx][1], t));
}

static inline float compute_focus_J(float cusp_j, float mid_j, float limit_j_max) {
  return lerp_f(cusp_j, mid_j, fmin(1.0f, kCuspMidBlend - (cusp_j / limit_j_max)));
}

static inline int look_hue_interval(float h, const constant MetalODTParams& p) {
  const float hw = wrap_to_360(h);
  int i          = kMetalOdtBaseIndex + hue_position_in_uniform_table(hw, kMetalOdtTotalTableSize);
  int i_lo       = i + p.hue_linearity_search_range[0];
  int i_hi       = i + p.hue_linearity_search_range[1];
  i_lo           = i_lo < kMetalOdtBaseIndex ? kMetalOdtBaseIndex : i_lo;
  i_hi           = i_hi > (kMetalOdtBaseIndex + kMetalOdtTableSize)
                       ? (kMetalOdtBaseIndex + kMetalOdtTableSize)
                       : i_hi;
  if (i_lo > i_hi) {
    i_lo = kMetalOdtBaseIndex;
    i_hi = kMetalOdtBaseIndex + kMetalOdtTableSize;
  }
  i              = (i_lo + i_hi) >> 1;
  for (int k = 0; k < 16 && (i_lo + 1 < i_hi); ++k) {
    const float v = p.table_hues_[i];
    if (hw > v) {
      i_lo = i;
    } else {
      i_hi = i;
    }
    i = (i_lo + i_hi) >> 1;
  }
  return (i_hi < 1) ? 1 : i_hi;
}

static inline float interpolation_weight(float h, float h_lo, float h_hi) {
  const float denom = h_hi - h_lo;
  if (fabs(denom) < 1e-6f) return 0.0f;
  return clamp_f((h - h_lo) / denom, 0.0f, 1.0f);
}

static inline HueDependentGamutParams init_HueDependentGamutParams(float h, const constant MetalODTParams& p) {
  HueDependentGamutParams hdp;
  hdp.gamma_bottom_inv = p.lower_hull_gamma_inv;
  const int i_hi       = look_hue_interval(h, p);
  const float hw       = wrap_to_360(h);
  const float t        = interpolation_weight(hw, p.table_hues_[i_hi - 1], p.table_hues_[i_hi]);
  hdp.JMcusp           = cusp_from_table(h, p);
  hdp.gamma_top_inv    = clamp_f(
      lerp_f(p.table_upper_hull_gamma_[i_hi - 1], p.table_upper_hull_gamma_[i_hi], t), 0.05f, 8.0f);
  hdp.focus_J          = compute_focus_J(hdp.JMcusp.x, p.mid_J, p.limit_J_max);
  hdp.analytical_threshold = lerp_f(hdp.JMcusp.x, p.limit_J_max, kFocusGainBlend);
  return hdp;
}

static inline float get_focus_gain(float j, float analytical_threshold, float limit_j_max, float focus_dist) {
  float gain = limit_j_max * focus_dist;
  if (j > analytical_threshold) {
    float gain_adjustment = safe_log10_ratio(limit_j_max - analytical_threshold, limit_j_max - j, 1e-4f);
    gain_adjustment       = gain_adjustment * gain_adjustment + 1.0f;
    gain                  = gain * gain_adjustment;
  }
  return gain;
}

static inline float solve_J_intersect(float j, float m, float focus_j, float max_j, float slope_gain) {
  const float sg       = fmax(fabs(slope_gain), 1e-6f);
  const float fj       = fmax(fabs(focus_j), 1e-6f);
  const float m_scaled = m / sg;
  const float a        = m_scaled / fj;
  const float b1       = 1.0f - m_scaled;
  const float c1       = -j;
  const float det1     = b1 * b1 - 4.0f * a * c1;
  const float r1       = (det1 > 0.0f) ? safe_sqrt(det1) : 0.0f;
  const float den1     = copysign(fmax(fabs(b1 + r1), 1e-6f), b1 + r1);
  const float res1     = (-2.0f * c1) / den1;
  const float b2       = -(1.0f + m_scaled + max_j * a);
  const float c2       = max_j * m_scaled + j;
  const float det2     = b2 * b2 - 4.0f * a * c2;
  const float r2       = (det2 > 0.0f) ? safe_sqrt(det2) : 0.0f;
  const float den2     = copysign(fmax(fabs(b2 - r2), 1e-6f), b2 - r2);
  const float res2     = (-2.0f * c2) / den2;
  return (j < focus_j) ? res1 : res2;
}

static inline float compute_compression_vector_slope(float intersect_j, float focus_j, float limit_j_max,
                                                     float slope_gain) {
  const float direction_scalar = (intersect_j < focus_j) ? intersect_j : (limit_j_max - intersect_j);
  const float denom            = fmax(fabs(focus_j * slope_gain), 1e-6f);
  return direction_scalar * (intersect_j - focus_j) / denom;
}

static inline float estimate_line_and_boundary_intersection_M(float j_axis_intersect, float slope,
                                                              float inv_gamma, float j_max, float m_max,
                                                              float j_intersection_reference) {
  const float refj                 = fmax(j_intersection_reference, 1e-6f);
  const float normalized_j         = fmax(j_axis_intersect / refj, 0.0f);
  const float shifted_intersection = refj * safe_pow_pos(normalized_j, inv_gamma);
  const float denom                = copysign(fmax(fabs(j_max - slope * m_max), 1e-6f), (j_max - slope * m_max));
  return shifted_intersection * m_max / denom;
}

static inline float smin_scaled(float a, float b, float scale_reference) {
  const float s_scaled = kSmoothCusps * scale_reference;
  if (s_scaled <= 1e-6f) return fmin(a, b);
  const float h = fmax(s_scaled - fabs(a - b), 0.0f) / s_scaled;
  return fmin(a, b) - h * h * h * s_scaled * (1.0f / 6.0f);
}

static inline float find_gamut_boundary_intersection(float2 jm_cusp, float j_max, float gamma_top_inv,
                                                     float gamma_bottom_inv, float j_intersect_source,
                                                     float slope, float j_intersect_cusp) {
  const float m_boundary_lower = estimate_line_and_boundary_intersection_M(
      j_intersect_source, slope, gamma_bottom_inv, jm_cusp.x, jm_cusp.y, j_intersect_cusp);
  const float f_j_intersect_cusp   = j_max - j_intersect_cusp;
  const float f_j_intersect_source = j_max - j_intersect_source;
  const float f_jm_cusp_j          = j_max - jm_cusp.x;
  const float m_boundary_upper     = estimate_line_and_boundary_intersection_M(
      f_j_intersect_source, -slope, gamma_top_inv, f_jm_cusp_j, jm_cusp.y, f_j_intersect_cusp);
  return smin_scaled(m_boundary_lower, m_boundary_upper, jm_cusp.y);
}

static inline float remap_M(float m, float gamut_boundary_m, float reach_boundary_m, bool invert = false) {
  const float boundary_ratio = safe_div(gamut_boundary_m, reach_boundary_m, 1e-6f);
  const float proportion     = fmax(boundary_ratio, kCompressionThreshold);
  const float threshold      = proportion * gamut_boundary_m;
  if (m <= threshold || proportion >= 1.0f) return m;
  const float m_offset       = m - threshold;
  const float gamut_off      = gamut_boundary_m - threshold;
  const float reach_off      = reach_boundary_m - threshold;
  const float ratio_rg       = safe_div(reach_off, gamut_off, 1e-6f);
  const float denom          = fmax(ratio_rg - 1.0f, 1e-7f);
  const float scale          = reach_off / denom;
  const float nd             = m_offset / scale;
  return threshold + reinhard_remap(scale, nd, invert);
}

static inline float3 compress_gamut(float3 jmh, float jx, const constant MetalODTParams& p,
                                    HueDependentGamutParams hdp, bool invert = false) {
  const float slope_gain = fmax(get_focus_gain(jx, hdp.analytical_threshold, p.limit_J_max, p.focus_dist), 1e-6f);
  const float j_intersect_source = solve_J_intersect(jmh.x, jmh.y, hdp.focus_J, p.limit_J_max, slope_gain);
  const float gamut_slope =
      compute_compression_vector_slope(j_intersect_source, hdp.focus_J, p.limit_J_max, slope_gain);
  const float j_intersect_cusp =
      solve_J_intersect(hdp.JMcusp.x, hdp.JMcusp.y, hdp.focus_J, p.limit_J_max, slope_gain);
  const float gamut_boundary_m = find_gamut_boundary_intersection(
      hdp.JMcusp, p.limit_J_max, hdp.gamma_top_inv, hdp.gamma_bottom_inv, j_intersect_source,
      gamut_slope, j_intersect_cusp);
  if (gamut_boundary_m <= 0.0f) {
    return float3(jx, 0.0f, jmh.z);
  }
  const float reach_max_m      = fmax(reach_M_from_table(jmh.z, p), 0.0f);
  const float reach_boundary_m = estimate_line_and_boundary_intersection_M(
      j_intersect_source, gamut_slope, p.model_gamma_inv, p.limit_J_max, reach_max_m, p.limit_J_max);
  const float remapped_m       = remap_M(jmh.y, gamut_boundary_m, reach_boundary_m, invert);
  const float j_out            = j_intersect_source + remapped_m * gamut_slope;
  if (!isfinite_f(j_out) || !isfinite_f(remapped_m)) {
    return float3(fmax(jx, 0.0f), 0.0f, jmh.z);
  }
  return float3(j_out, remapped_m, jmh.z);
}

static inline float3 gamut_compress_fwd(float3 jmh, const constant MetalODTParams& p) {
  if (jmh.x <= 0.0f) {
    return float3(0.0f, 0.0f, jmh.z);
  }
  if (jmh.y <= 0.0f || jmh.x > p.limit_J_max) {
    return float3(jmh.x, 0.0f, jmh.z);
  }
  return compress_gamut(jmh, jmh.x, p, init_HueDependentGamutParams(jmh.z, p), false);
}

static inline float3 limit_rgb_preserve_chroma(float3 rgb, float lower, float upper) {
  if (!all(isfinite(rgb))) {
    return float3(0.0f);
  }
  rgb = max(rgb, float3(lower));
  const float m = fmax(rgb.x, fmax(rgb.y, rgb.z));
  if (m > upper && m > 0.0f) {
    rgb *= upper / m;
  }
  return rgb;
}

static inline float3 OutputTransform_fwd(float3 in_color, const constant MetalODTParams& p) {
  if (!all(isfinite(in_color))) {
    return float3(0.0f);
  }
  const float3 ap0_clamped    = clamp_AP1(in_color, 0.0f, p.ts_.forward_limit_);
  const float3 jmh            = RGB_to_JMh(ap0_clamped, p.input_params_);
  const float3 tonemapped_jmh = tonemap_and_compress_fwd(jmh, p);
  const float3 compressed_jmh = gamut_compress_fwd(tonemapped_jmh, p);
  const float3 out_rgb        = JMh_to_RGB(compressed_jmh, p.limit_params_);
  return limit_rgb_preserve_chroma(out_rgb, 0.0f, p.ts_.forward_limit_);
}

static inline float3 odrt_apply_matrix(constant float* mat, float3 v) {
  return apply_matrix3x3(mat, v);
}

static inline float3 odrt_add_scalar(float3 v, float s) { return v + s; }
static inline float3 odrt_add(float3 a, float3 b) { return a + b; }
static inline float3 odrt_sub(float3 a, float3 b) { return a - b; }
static inline float3 odrt_mul_scalar(float3 v, float s) { return v * s; }
static inline float3 odrt_mul(float3 a, float3 b) { return a * b; }

static inline float3 odrt_div_scalar(float3 v, float s) {
  if (fabs(s) < 1e-8f) return float3(0.0f);
  return v / s;
}

static inline float odrt_spowf(float a, float b) { return (a <= 0.0f) ? a : pow(a, b); }
static inline float3 odrt_spowf3(float3 a, float b) {
  return float3(odrt_spowf(a.x, b), odrt_spowf(a.y, b), odrt_spowf(a.z, b));
}
static inline float odrt_hypot2(float2 v) { return sqrt(fmax(0.0f, dot(v, v))); }
static inline float odrt_hypot3(float3 v) { return sqrt(fmax(0.0f, dot(v, v))); }
static inline float3 odrt_clampf3(float3 v, float mn, float mx) { return clamp(v, float3(mn), float3(mx)); }
static inline float3 odrt_clampminf3(float3 v, float mn) { return max(v, float3(mn)); }
static inline float odrt_compress_hyperbolic_power(float x, float s, float p) { return odrt_spowf(x / (x + s), p); }

static inline float odrt_compress_toe_quadratic(float x, float toe, bool inv = false) {
  if (toe == 0.0f) return x;
  if (!inv) return odrt_spowf(x, 2.0f) / (x + toe);
  return (x + sqrt(x * (4.0f * toe + x))) / 2.0f;
}

static inline float odrt_compress_toe_cubic(float x, float m, float w, bool inv = false) {
  if (m == 1.0f) return x;
  const float x2 = x * x;
  if (!inv) return x * (x2 + m * w) / (x2 + w);
  const float p0 = x2 - 3.0f * m * w;
  const float p1 = 2.0f * x2 + 27.0f * w - 9.0f * m * w;
  const float p2 =
      pow(sqrt(x2 * p1 * p1 - 4.0f * p0 * p0 * p0) / 2.0f + x * p1 / 2.0f, 1.0f / 3.0f);
  return p0 / (3.0f * p2) + p2 / 3.0f + x / 3.0f;
}

static inline float odrt_contrast_high(float x, float p, float pv, float pv_lx, bool inv = false) {
  const float x0 = 0.18f * pow(2.0f, pv);
  if (x < x0 || p == 1.0f) return x;
  const float o  = x0 - x0 / p;
  const float s0 = pow(x0, 1.0f - p) / p;
  const float x1 = x0 * pow(2.0f, pv_lx);
  const float k1 = p * s0 * pow(x1, p) / x1;
  const float y1 = s0 * pow(x1, p) + o;
  if (inv) return (x > y1) ? (x - y1) / k1 + x1 : pow((x - o) / s0, 1.0f / p);
  return (x > x1) ? k1 * (x - x1) + y1 : s0 * pow(x, p) + o;
}

static inline float odrt_softplus(float x, float s) {
  if (x > 10.0f * s || s < 1e-4f) return x;
  return s * log(fmax(0.0f, 1.0f + exp(x / s)));
}

static inline float odrt_gauss_window(float x, float w) { return exp(-x * x / w); }
static inline float2 odrt_opponent(float3 rgb) { return float2(rgb.x - rgb.z, rgb.y - (rgb.x + rgb.z) / 2.0f); }
static inline float odrt_hue_offset(float h, float o) { return fmod(h - o + kOpenDrtPi, 2.0f * kOpenDrtPi) - kOpenDrtPi; }

static inline float3 odrt_display_gamut_whitepoint(float3 rgb, float tsn, float cwp_lm, int display_gamut, int cwp) {
  rgb                 = odrt_apply_matrix(kOpenDrtP3D65ToXyz, rgb);
  float3 cwp_neutral  = rgb;
  const float cwp_f   = pow(tsn, 2.0f * cwp_lm);
  if (display_gamut < 3) {
    if (cwp == 0) rgb = odrt_apply_matrix(kOpenDrtCatD65ToD93, rgb);
    else if (cwp == 1) rgb = odrt_apply_matrix(kOpenDrtCatD65ToD75, rgb);
    else if (cwp == 3) rgb = odrt_apply_matrix(kOpenDrtCatD65ToD60, rgb);
    else if (cwp == 4) rgb = odrt_apply_matrix(kOpenDrtCatD65ToD55, rgb);
    else if (cwp == 5) rgb = odrt_apply_matrix(kOpenDrtCatD65ToD50, rgb);
  } else if (display_gamut == 3) {
    if (cwp == 0) rgb = odrt_apply_matrix(kOpenDrtCatD60ToD93, rgb);
    else if (cwp == 1) rgb = odrt_apply_matrix(kOpenDrtCatD60ToD75, rgb);
    else if (cwp == 2) rgb = odrt_apply_matrix(kOpenDrtCatD60ToD65, rgb);
    else if (cwp == 4) rgb = odrt_apply_matrix(kOpenDrtCatD60ToD55, rgb);
    else if (cwp == 5) rgb = odrt_apply_matrix(kOpenDrtCatD60ToD50, rgb);
  } else {
    cwp_neutral = odrt_apply_matrix(kOpenDrtCatDciToD65, rgb);
    if (cwp == 0) rgb = odrt_apply_matrix(kOpenDrtCatDciToD93, rgb);
    else if (cwp == 1) rgb = odrt_apply_matrix(kOpenDrtCatDciToD75, rgb);
    else if (cwp == 2) rgb = cwp_neutral;
    else if (cwp == 3) rgb = odrt_apply_matrix(kOpenDrtCatDciToD60, rgb);
    else if (cwp == 4) rgb = odrt_apply_matrix(kOpenDrtCatDciToD55, rgb);
    else if (cwp == 5) rgb = odrt_apply_matrix(kOpenDrtCatDciToD50, rgb);
  }
  rgb = odrt_add(odrt_mul_scalar(rgb, cwp_f), odrt_mul_scalar(cwp_neutral, 1.0f - cwp_f));
  if (display_gamut == 0) {
    rgb = odrt_apply_matrix(kOpenDrtXyzToRec709, rgb);
  } else if (display_gamut == 5) {
    rgb = odrt_apply_matrix(kOpenDrtCatD65ToDci, rgb);
  } else {
    rgb = odrt_apply_matrix(kOpenDrtXyzToP3D65, rgb);
  }
  float cwp_norm = 1.0f;
  if (display_gamut == 0) {
    if (cwp == 0) cwp_norm = 0.7441926991f;
    else if (cwp == 1) cwp_norm = 0.8734708321f;
    else if (cwp == 3) cwp_norm = 0.9559369922f;
    else if (cwp == 4) cwp_norm = 0.9056713328f;
    else if (cwp == 5) cwp_norm = 0.8500043850f;
  } else if (display_gamut == 1 || display_gamut == 2) {
    if (cwp == 0) cwp_norm = 0.7626870573f;
    else if (cwp == 1) cwp_norm = 0.8840540833f;
    else if (cwp == 3) cwp_norm = 0.9643201867f;
    else if (cwp == 4) cwp_norm = 0.9230765189f;
    else if (cwp == 5) cwp_norm = 0.8765728378f;
  } else if (display_gamut == 3) {
    if (cwp == 0) cwp_norm = 0.7049563210f;
    else if (cwp == 1) cwp_norm = 0.8167157098f;
    else if (cwp == 2) cwp_norm = 0.9233821937f;
    else if (cwp == 4) cwp_norm = 0.9561385003f;
    else if (cwp == 5) cwp_norm = 0.9068014530f;
  } else if (display_gamut == 4) {
    if (cwp == 0) cwp_norm = 0.6653361412f;
    else if (cwp == 1) cwp_norm = 0.7703971314f;
    else if (cwp == 2) cwp_norm = 0.8705723433f;
    else if (cwp == 3) cwp_norm = 0.8913545475f;
    else if (cwp == 4) cwp_norm = 0.8553278252f;
    else if (cwp == 5) cwp_norm = 0.8145664361f;
  } else if (display_gamut == 5) {
    if (cwp == 0) cwp_norm = 0.7071427840f;
    else if (cwp == 1) cwp_norm = 0.8155610826f;
    else if (cwp >= 2) cwp_norm = 0.9165552797f;
  }
  return odrt_mul_scalar(rgb, cwp_norm * cwp_f + 1.0f - cwp_f);
}

static inline float3 OpenDRTTransform_fwd(float3 input_color, const constant MetalOpenDRTParams& p) {
  float3 rgb           = odrt_apply_matrix(kOpenDrtAp1ToXyz, input_color);
  rgb                  = odrt_apply_matrix(kOpenDrtXyzToP3D65, rgb);
  const float3 rs_w    = float3(p.rs_rw_, 1.0f - p.rs_rw_ - p.rs_bw_, p.rs_bw_);
  float sat_l          = dot(rgb, rs_w);
  rgb                  = odrt_add(odrt_mul_scalar(float3(sat_l), p.rs_sa_), odrt_mul_scalar(rgb, 1.0f - p.rs_sa_));
  rgb                  = odrt_add_scalar(rgb, p.tn_off_);
  float tsn            = odrt_hypot3(rgb) / kOpenDrtSqrt3;
  rgb                  = odrt_div_scalar(rgb, tsn);
  const float2 opp     = odrt_opponent(rgb);
  float ach_d          = odrt_hypot2(opp) / 2.0f;
  ach_d                = 1.25f * odrt_compress_toe_quadratic(ach_d, 0.25f, false);
  const float hue      = fmod(atan2(opp.x, opp.y) + kOpenDrtPi + 1.10714931f, 2.0f * kOpenDrtPi);
  const float3 ha_rgb  = float3(odrt_gauss_window(odrt_hue_offset(hue, 0.1f), 0.66f),
                                odrt_gauss_window(odrt_hue_offset(hue, 4.3f), 0.66f),
                                odrt_gauss_window(odrt_hue_offset(hue, 2.3f), 0.66f));
  const float3 ha_rgb_hs = float3(odrt_gauss_window(odrt_hue_offset(hue, -0.4f), 0.66f), ha_rgb.y,
                                  odrt_gauss_window(odrt_hue_offset(hue, 2.5f), 0.66f));
  const float3 ha_cmy    = float3(odrt_gauss_window(odrt_hue_offset(hue, 3.3f), 0.5f),
                               odrt_gauss_window(odrt_hue_offset(hue, 1.3f), 0.5f),
                               odrt_gauss_window(odrt_hue_offset(hue, -1.15f), 0.5f));
  if (p.brl_enable_) {
    const float brl_tsf = pow(tsn / (tsn + 1.0f), 1.0f - p.brl_rng_);
    const float brl_exf =
        (p.brl_ + p.brl_r_ * ha_rgb.x + p.brl_g_ * ha_rgb.y + p.brl_b_ * ha_rgb.z) *
        pow(ach_d, 1.0f / p.brl_st_);
    const float brl_ex = pow(2.0f, brl_exf * ((brl_exf < 0.0f) ? brl_tsf : 1.0f - brl_tsf));
    tsn *= brl_ex;
  }
  if (p.tn_lcon_enable_) {
    const float lcon_m       = pow(2.0f, -p.tn_lcon_);
    float lcon_w             = p.tn_lcon_w_ / 4.0f;
    lcon_w                  *= lcon_w;
    const float lcon_cnst_sc = odrt_compress_toe_cubic(p.ts_x0_, lcon_m, lcon_w, true) / p.ts_x0_;
    tsn *= lcon_cnst_sc;
    tsn  = odrt_compress_toe_cubic(tsn, lcon_m, lcon_w, false);
  }
  if (p.tn_hcon_enable_) {
    tsn = odrt_contrast_high(tsn, pow(2.0f, p.tn_hcon_), p.tn_hcon_pv_, p.tn_hcon_st_, false);
  }
  const float tsn_pt    = odrt_compress_hyperbolic_power(tsn, p.ts_s1_, p.ts_p_);
  const float tsn_const = odrt_compress_hyperbolic_power(tsn, p.s_Lp100_, p.ts_p_);
  tsn                   = odrt_compress_hyperbolic_power(tsn, p.ts_s_, p.ts_p_);
  if (p.hc_enable_) {
    float hc_ts = 1.0f - tsn_const;
    float hc_c  = hc_ts * (1.0f - ach_d) + ach_d * (1.0f - hc_ts);
    hc_c       *= ach_d * ha_rgb.x;
    hc_ts       = pow(hc_ts, 1.0f / p.hc_r_rng_);
    const float hc_f = p.hc_r_ * (hc_c - 2.0f * hc_c * hc_ts) + 1.0f;
    rgb              = float3(rgb.x, rgb.y * hc_f, rgb.z * hc_f);
  }
  if (p.hs_rgb_enable_) {
    const float3 hs_rgb = float3(ha_rgb_hs.x * ach_d * pow(tsn_pt, 1.0f / p.hs_r_rng_),
                                 ha_rgb_hs.y * ach_d * pow(tsn_pt, 1.0f / p.hs_g_rng_),
                                 ha_rgb_hs.z * ach_d * pow(tsn_pt, 1.0f / p.hs_b_rng_));
    const float3 hsf    = float3((hs_rgb.z * -p.hs_b_) - (hs_rgb.y * -p.hs_g_),
                              (hs_rgb.x * p.hs_r_) - (hs_rgb.z * -p.hs_b_),
                              (hs_rgb.y * -p.hs_g_) - (hs_rgb.x * p.hs_r_));
    rgb                 = odrt_add(rgb, hsf);
  }
  if (p.hs_cmy_enable_) {
    const float tsn_pt_compl = 1.0f - tsn_pt;
    const float3 hs_cmy = float3(ha_cmy.x * ach_d * pow(tsn_pt_compl, 1.0f / p.hs_c_rng_),
                                 ha_cmy.y * ach_d * pow(tsn_pt_compl, 1.0f / p.hs_m_rng_),
                                 ha_cmy.z * ach_d * pow(tsn_pt_compl, 1.0f / p.hs_y_rng_));
    const float3 hsf    = float3((hs_cmy.z * p.hs_y_) - (hs_cmy.y * p.hs_m_),
                              (hs_cmy.x * -p.hs_c_) - (hs_cmy.z * p.hs_y_),
                              (hs_cmy.y * p.hs_m_) - (hs_cmy.x * -p.hs_c_));
    rgb                 = odrt_add(rgb, hsf);
  }
  const float pt_lml_p = 1.0f + 4.0f * (1.0f - tsn_pt) *
                                    (p.pt_lml_ + p.pt_lml_r_ * ha_rgb_hs.x + p.pt_lml_g_ * ha_rgb_hs.y +
                                     p.pt_lml_b_ * ha_rgb_hs.z);
  float ptf            = 1.0f - pow(tsn_pt, pt_lml_p);
  const float pt_lmh_p = (1.0f - ach_d * (p.pt_lmh_r_ * ha_rgb_hs.x + p.pt_lmh_b_ * ha_rgb_hs.z)) *
                         (1.0f - p.pt_lmh_ * ach_d);
  ptf                  = pow(ptf, pt_lmh_p);
  if (p.ptm_enable_) {
    float ptm_low_f = 1.0f;
    if (p.ptm_low_st_ != 0.0f && p.ptm_low_rng_ != 0.0f) {
      ptm_low_f = 1.0f + p.ptm_low_ * exp(-2.0f * ach_d * ach_d / p.ptm_low_st_) *
                             pow(1.0f - tsn_const, 1.0f / p.ptm_low_rng_);
    }
    float ptm_high_f = 1.0f;
    if (p.ptm_high_st_ != 0.0f && p.ptm_high_rng_ != 0.0f) {
      ptm_high_f = 1.0f + p.ptm_high_ * exp(-2.0f * ach_d * ach_d / p.ptm_high_st_) *
                              pow(tsn_pt, 1.0f / (4.0f * p.ptm_high_rng_));
    }
    ptf *= ptm_low_f * ptm_high_f;
  }
  rgb   = odrt_add(odrt_mul_scalar(rgb, ptf), float3(1.0f - ptf));
  sat_l = dot(rgb, rs_w);
  rgb   = odrt_div_scalar(odrt_sub(odrt_mul_scalar(float3(sat_l), p.rs_sa_), rgb), p.rs_sa_ - 1.0f);
  rgb   = odrt_display_gamut_whitepoint(rgb, tsn_const, p.cwp_lm_, p.display_gamut_, p.creative_white_);
  if (p.brlp_enable_) {
    const float2 brlp_opp  = odrt_opponent(rgb);
    float brlp_ach_d       = odrt_hypot2(brlp_opp) / 4.0f;
    brlp_ach_d             = 1.1f * (brlp_ach_d * brlp_ach_d / (brlp_ach_d + 0.1f));
    const float3 brlp_ha   = odrt_mul_scalar(ha_rgb, ach_d);
    const float brlp_m     = p.brlp_ + p.brlp_r_ * brlp_ha.x + p.brlp_g_ * brlp_ha.y + p.brlp_b_ * brlp_ha.z;
    rgb                    = odrt_mul_scalar(rgb, pow(2.0f, brlp_m * brlp_ach_d * tsn));
  }
  if (p.ptl_enable_) {
    rgb = float3(odrt_softplus(rgb.x, p.ptl_c_), odrt_softplus(rgb.y, p.ptl_m_), odrt_softplus(rgb.z, p.ptl_y_));
  }
  tsn *= p.ts_m2_;
  tsn  = odrt_compress_toe_quadratic(tsn, p.tn_toe_, false);
  tsn *= p.ts_dsc_;
  rgb *= tsn;
  if (p.display_gamut_ == 2) {
    rgb = odrt_clampminf3(rgb, 0.0f);
    rgb = odrt_apply_matrix(kOpenDrtP3ToRec2020, rgb);
  }
  if (p.clamp_) {
    rgb = odrt_clampf3(rgb, 0.0f, 1.0f);
  }
  return rgb;
}

static inline uint lut3d_index(uint edge, uint x, uint y, uint z) {
  return (z * edge + y) * edge + x;
}

static inline float4 sample_lut3d_linear(device const float4* lut, uint edge, float u, float v, float w) {
  if (lut == nullptr || edge <= 1) return float4(u, v, w, 1.0f);
  const float3 coord   = clamp(float3(u, v, w), 0.0f, 1.0f);
  const float3 tex_pos = coord * float(edge) - 0.5f;
  const float3 pos     = clamp(tex_pos, 0.0f, float(edge - 1));
  const uint3 lo       = uint3(pos);
  const uint3 hi       = min(lo + uint3(1), uint3(edge - 1));
  const float3 t       = pos - float3(lo);
  const float4 c000    = lut[lut3d_index(edge, lo.x, lo.y, lo.z)];
  const float4 c100    = lut[lut3d_index(edge, hi.x, lo.y, lo.z)];
  const float4 c010    = lut[lut3d_index(edge, lo.x, hi.y, lo.z)];
  const float4 c110    = lut[lut3d_index(edge, hi.x, hi.y, lo.z)];
  const float4 c001    = lut[lut3d_index(edge, lo.x, lo.y, hi.z)];
  const float4 c101    = lut[lut3d_index(edge, hi.x, lo.y, hi.z)];
  const float4 c011    = lut[lut3d_index(edge, lo.x, hi.y, hi.z)];
  const float4 c111    = lut[lut3d_index(edge, hi.x, hi.y, hi.z)];
  const float4 c00     = mix(c000, c100, t.x);
  const float4 c10     = mix(c010, c110, t.x);
  const float4 c01     = mix(c001, c101, t.x);
  const float4 c11     = mix(c011, c111, t.x);
  const float4 c0      = mix(c00, c10, t.y);
  const float4 c1      = mix(c01, c11, t.y);
  return mix(c0, c1, t.z);
}

static inline float4 GPU_TOWS_Kernel(float4 px, constant MetalFusedParams& params) {
  if (!params.to_ws_enabled_) return px;
  float3 ap1;
  const bool use_camera_to_ap1 =
      (params.raw_decode_input_space_ == 1) && (params.color_temp_matrices_valid_ != 0u);
  if (use_camera_to_ap1) {
    ap1 = apply_matrix3x3(params.color_temp_cam_to_ap1_, px.xyz);
  } else {
    ap1 = apply_matrix3x3(kAp0ToAp1Mat, px.xyz);
  }
  const float ach     = fmax(ap1.x, fmax(ap1.y, ap1.z));
  const float abs_ach = fabs(ach);
  if (abs_ach > 1e-6f) {
    const float dist_cyan    = (ach - ap1.x) / abs_ach;
    const float dist_magenta = (ach - ap1.y) / abs_ach;
    const float dist_yellow  = (ach - ap1.z) / abs_ach;
    ap1.x                    = ach - rgc_compress_curve(dist_cyan, kRgcLimCyan, kRgcThrCyan, kRgcPwr) * abs_ach;
    ap1.y                    = ach - rgc_compress_curve(dist_magenta, kRgcLimMagenta, kRgcThrMagenta, kRgcPwr) * abs_ach;
    ap1.z                    = ach - rgc_compress_curve(dist_yellow, kRgcLimYellow, kRgcThrYellow, kRgcPwr) * abs_ach;
  }
  return float4(acescc_encode(ap1.x), acescc_encode(ap1.y), acescc_encode(ap1.z), px.w);
}

static inline float4 GPU_LMT_Kernel(float4 px, constant MetalFusedParams& params,
                                    device const float4* lmt_lut) {
  if (!params.lmt_enabled_ || !params.lmt_lut_enabled_ || params.lmt_lut_edge_size_ <= 1u) return px;
  const float scale  = float(params.lmt_lut_edge_size_ - 1u) / float(params.lmt_lut_edge_size_);
  const float offset = 1.0f / (2.0f * float(params.lmt_lut_edge_size_));
  const float u      = px.x * scale + offset;
  const float v      = px.y * scale + offset;
  const float w      = px.z * scale + offset;
  const float4 lut_v = sample_lut3d_linear(lmt_lut, params.lmt_lut_edge_size_, u, v, w);
  return float4(lut_v.x, lut_v.y, lut_v.z, px.w);
}

static inline float4 GPU_OUTPUT_Kernel(float4 px, constant MetalFusedParams& params) {
  if (!params.to_output_enabled_) return px;
  const float3 aces_linear = float3(acescc_decode(px.x), acescc_decode(px.y), acescc_decode(px.z));
  float3 odt_color;
  if (params.to_output_params_.method_ == kMetalOdtMethodAces20) {
    odt_color = OutputTransform_fwd(aces_linear, params.to_output_params_.aces_params_);
  } else {
    odt_color = OpenDRTTransform_fwd(aces_linear, params.to_output_params_.open_drt_params_);
  }
  const float3 cv = DisplayEncoding(
      odt_color, params.to_output_params_.limit_to_display_matx, params.to_output_params_.eotf_,
      params.to_output_params_.display_linear_scale_);
  return float4(cv, px.w);
}
