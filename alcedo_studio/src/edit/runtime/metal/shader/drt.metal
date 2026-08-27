//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <metal_stdlib>

using namespace metal;

constant int kMetalAcesOdtTableSize = 362;

struct MetalJMhParams {
  float MATRIX_RGB_to_CAM16_c_[9];
  float MATRIX_CAM16_c_to_RGB_[9];
  float MATRIX_cone_response_to_Aab_[9];
  float MATRIX_Aab_to_cone_response_[9];
  float F_L_n_;
  float cz_;
  float inv_cz_;
  float A_w_J_;
  float inv_A_w_J_;
};

struct MetalTSParams {
  float n_;
  float n_r_;
  float g_;
  float t_1_;
  float c_t_;
  float s_2_;
  float u_2_;
  float m_2_;
  float forward_limit_;
  float inverse_limit_;
  float log_peak_;
};

struct MetalODTParams {
  float          peak_luminance_;
  MetalJMhParams input_params_;
  MetalJMhParams reach_params_;
  MetalJMhParams limit_params_;
  MetalTSParams  ts_;
  float          limit_J_max;
  float          model_gamma_inv;
  float          mid_J;
  float          focus_dist;
  float          lower_hull_gamma_inv;
  int            hue_linearity_search_range[2];
  float          sat;
  float          sat_thr;
  float          compr;
  float          chroma_compress_scale;
  float          table_reach_M_[kMetalAcesOdtTableSize];
  float          table_hues_[kMetalAcesOdtTableSize];
  float          table_upper_hull_gamma_[kMetalAcesOdtTableSize];
  float          table_gamut_cusps_[kMetalAcesOdtTableSize][4];
};

struct MetalOpenDRTParams {
  int   tn_hcon_enable_;
  int   tn_lcon_enable_;
  int   pt_enable_;
  int   ptl_enable_;
  int   ptm_enable_;
  int   brl_enable_;
  int   brlp_enable_;
  int   hc_enable_;
  int   hs_rgb_enable_;
  int   hs_cmy_enable_;
  int   creative_white_;
  int   surround_;
  int   clamp_;
  int   display_gamut_;
  int   display_eotf_;
  float tn_con_;
  float tn_sh_;
  float tn_toe_;
  float tn_off_;
  float tn_hcon_;
  float tn_hcon_pv_;
  float tn_hcon_st_;
  float tn_lcon_;
  float tn_lcon_w_;
  float cwp_lm_;
  float rs_sa_;
  float rs_rw_;
  float rs_bw_;
  float pt_lml_;
  float pt_lml_r_;
  float pt_lml_g_;
  float pt_lml_b_;
  float pt_lmh_;
  float pt_lmh_r_;
  float pt_lmh_b_;
  float ptl_c_;
  float ptl_m_;
  float ptl_y_;
  float ptm_low_;
  float ptm_low_rng_;
  float ptm_low_st_;
  float ptm_high_;
  float ptm_high_rng_;
  float ptm_high_st_;
  float brl_;
  float brl_r_;
  float brl_g_;
  float brl_b_;
  float brl_rng_;
  float brl_st_;
  float brlp_;
  float brlp_r_;
  float brlp_g_;
  float brlp_b_;
  float hc_r_;
  float hc_r_rng_;
  float hs_r_;
  float hs_r_rng_;
  float hs_g_;
  float hs_g_rng_;
  float hs_b_;
  float hs_b_rng_;
  float hs_c_;
  float hs_c_rng_;
  float hs_m_;
  float hs_m_rng_;
  float hs_y_;
  float hs_y_rng_;
  float ts_x1_;
  float ts_y1_;
  float ts_x0_;
  float ts_y0_;
  float ts_s0_;
  float ts_p_;
  float ts_s10_;
  float ts_m1_;
  float ts_m2_;
  float ts_s_;
  float ts_dsc_;
  float pt_cmp_Lf_;
  float s_Lp100_;
  float ts_s1_;
};

struct MetalToOutputParams {
  int                method_;
  int                eotf_;
  MetalODTParams     aces_params_;
  MetalOpenDRTParams open_drt_params_;
  float              limit_to_display_matx[9];
  float              display_linear_scale_;
};

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
constant int kMetalOdtBaseIndex      = 1;
constant float kMetalHueLimit        = 360.0f;

constant float kAcesccLog2Min      = -15.0f;
constant float kAcesccLog2Denorm   = -16.0f;
constant float kAcesccDenormTrans  = 0.00003051757812f;
constant float kAcesccDenormOffset = 0.00001525878906f;
constant float kAcesccA            = 9.72f;
constant float kAcesccB            = 17.52f;

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

static inline float Tonescale_fwd(float x, const constant MetalTSParams& params) {
  const float denom = x + params.s_2_;
  const float ratio = (denom > 1e-7f) ? (fmax(0.0f, x) / denom) : 0.0f;
  const float f     = params.m_2_ * pow(ratio, params.g_);
  const float h     = fmax(0.0f, f * f / (f + params.t_1_));
  return h * params.n_r_;
}

static inline float moncurve_inv(float y, float gamma, float offs) {
  const float yb = pow(offs * gamma / ((gamma - 1.0f) * (1.0f + offs)), gamma);
  const float rs =
      pow((gamma - 1.0f) / offs, gamma - 1.0f) * pow((1.0f + offs) / gamma, gamma);
  return (y >= yb) ? (1.0f + offs) * pow(y, 1.0f / gamma) - offs : y * rs;
}

static inline float3 moncurve_inv_f3(float3 v, float gamma, float offs) {
  return float3(moncurve_inv(v.x, gamma, offs), moncurve_inv(v.y, gamma, offs),
                moncurve_inv(v.z, gamma, offs));
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

static inline float Y_to_ST2084(float c) {
  const float l  = c / kPqC;
  const float lm = pow(l, kPqM1);
  const float n  = (kPqC1 + kPqC2 * lm) / (1.0f + kPqC3 * lm);
  return pow(n, kPqM2);
}

static inline float3 Y_to_ST2084_f3(float3 c) {
  return float3(Y_to_ST2084(c.x), Y_to_ST2084(c.y), Y_to_ST2084(c.z));
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

static inline float3 DisplayEncoding(float3 rgb, constant float* mat_limit_to_display, int eotf_num,
                                     float linear_scale = 1.0f) {
  const float3 rgb_disp_linear   = mult_f3_f33(rgb, mat_limit_to_display);
  const float3 rgb_display_scale = mult_f_f3(rgb_disp_linear, linear_scale);
  return eotf_inv(rgb_display_scale, eotf_num);
}

#include "drt_aces.metal"
#include "drt_opendrt.metal"

kernel void drt_display(texture2d<float, access::read> input [[texture(0)]],
                        texture2d<float, access::write> output [[texture(1)]],
                        constant MetalToOutputParams& params [[buffer(0)]],
                        uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= input.get_width() || gid.y >= input.get_height()) {
    return;
  }
  const float4 source = input.read(gid);
  const float3 scene =
      float3(acescc_decode(source.x), acescc_decode(source.y), acescc_decode(source.z));
  float3 display_linear;
  if (params.method_ == kMetalOdtMethodAces20) {
    display_linear = OutputTransform_fwd(scene, params.aces_params_);
  } else {
    display_linear = OpenDRTTransform_fwd(scene, params.open_drt_params_);
  }
  const float3 encoded = DisplayEncoding(display_linear, params.limit_to_display_matx, params.eotf_,
                                         params.display_linear_scale_);
  output.write(float4(encoded, source.w), gid);
}
