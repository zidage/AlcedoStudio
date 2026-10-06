//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <metal_stdlib>

using namespace metal;
#include "../../../../include/color/color_encoding_math.h"

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
constant int kMetalOdtTableSize      = 360;
constant int kMetalOdtTotalTableSize = 362;
constant int kMetalOdtBaseIndex      = 1;
constant float kMetalHueLimit        = 360.0f;

constant float kRefLuminance        = 100.0f;
constant float kJScale              = 100.0f;
constant float kCamNlOffset         = 27.13f;
constant float kModelGamma          = 1.13705599f;
constant float kSmoothCusps         = 0.12f;
constant float kCuspMidBlend        = 1.3f;
constant float kFocusGainBlend      = 0.3f;
constant float kCompressionThreshold = 0.75f;
constant float kHuntNJ = 0.012f;
constant float kChromaJFloor = 0.25f;
constant float kRgbMappingFailureRatio = 8.0f;

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

static inline float3 DisplayEncoding(float3 rgb, constant float* mat_limit_to_display, int eotf_num,
                                     float linear_scale = 1.0f) {
  const float3 scaled   = mult_f_f3(mult_f3_f33(rgb, mat_limit_to_display), linear_scale);
  const float  hlg_gain = eotf_num == CE_TF_HLG ? CeHlgDisplayGain(scaled.x, scaled.y, scaled.z) : 1.0f;
  return float3(CeDisplayEncodeChannel(eotf_num, scaled.x, hlg_gain),
                CeDisplayEncodeChannel(eotf_num, scaled.y, hlg_gain),
                CeDisplayEncodeChannel(eotf_num, scaled.z, hlg_gain));
}

#include "drt_aces.metal"
#include "drt_opendrt.metal"
#include "../../../../include/edit/runtime/aces_reference_gamut_compression.h"

kernel void drt_display(texture2d<float, access::read> input [[texture(0)]],
                        texture2d<float, access::write> output [[texture(1)]],
                        constant MetalToOutputParams& params [[buffer(0)]],
                        uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= input.get_width() || gid.y >= input.get_height()) {
    return;
  }
  const float4 source = input.read(gid);
  // Input is linear AP1; the DiffusionFilter pass already decoded ACEScc.
  const AcesRgcRgb compressed = AcesReferenceGamutCompress(source.x, source.y, source.z);
  const float3 scene = float3(compressed.r, compressed.g, compressed.b);
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

/// DiffusionFilter with strength 0: decode the ACEScc AP1 scene to linear AP1 for the DRT.
kernel void diffusion_filter_decode(texture2d<float, access::read> input [[texture(0)]],
                                    texture2d<float, access::write> output [[texture(1)]],
                                    uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= input.get_width() || gid.y >= input.get_height()) {
    return;
  }
  const float4 source = input.read(gid);
  output.write(float4(CeAcesccDecode(source.x), CeAcesccDecode(source.y), CeAcesccDecode(source.z),
                      source.w),
               gid);
}

// === DiffusionFilter scatter ==================================================
// Mirrors cuda_diffusion_filter_pass.cu. Texel `i` covers [i, i + 1); every read clamps to edge.

/// Mirrors `ReduceParams` in metal_diffusion_filter_pass.mm.
struct DiffusionReduceParams {
  int   src_width;
  int   src_height;
  int   dst_width;
  int   dst_height;
  int   samples;
  float gain;
  float low;
  float high;
  float base_to_render[12];
};

/// Mirrors `MixParams` in metal_diffusion_filter_pass.mm.
struct DiffusionMixParams {
  int   width;
  int   height;
  float scatter_fraction;
  float transmission;
  float render_to_base[12];
};

static inline float2 DiffusionTransform(constant float* m, float x, float y) {
  return float2(m[0] * x + m[1] * y + m[2], m[3] * x + m[4] * y + m[5]);
}

static inline float4 DiffusionDecode(float4 value) {
  return float4(CeAcesccDecode(value.x), CeAcesccDecode(value.y), CeAcesccDecode(value.z), value.w);
}

static inline float4 DiffusionFetch(texture2d<float, access::read> image, int x, int y) {
  const int width  = int(image.get_width());
  const int height = int(image.get_height());
  return image.read(uint2(uint(clamp(x, 0, width - 1)), uint(clamp(y, 0, height - 1))));
}

/// Bilinear sample with clamp-to-edge addressing.
static inline float4 DiffusionBilinear(texture2d<float, access::read> image, float px, float py) {
  const float  fx     = px - 0.5f;
  const float  fy     = py - 0.5f;
  const float  x0     = floor(fx);
  const float  y0     = floor(fy);
  const float  ax     = fx - x0;
  const float  ay     = fy - y0;
  const int    ix     = int(x0);
  const int    iy     = int(y0);
  const float4 a      = DiffusionFetch(image, ix, iy);
  const float4 b      = DiffusionFetch(image, ix + 1, iy);
  const float4 c      = DiffusionFetch(image, ix, iy + 1);
  const float4 d      = DiffusionFetch(image, ix + 1, iy + 1);
  const float4 top    = a * (1.0f - ax) + b * ax;
  const float4 bottom = c * (1.0f - ax) + d * ax;
  return top * (1.0f - ay) + bottom * ay;
}

/// Bilinear sample of the ACEScc scene, decoded to linear AP1 per tap before interpolation.
static inline float4 DiffusionBilinearDecoded(texture2d<float, access::read> image, float px,
                                              float py) {
  const float  fx     = px - 0.5f;
  const float  fy     = py - 0.5f;
  const float  x0     = floor(fx);
  const float  y0     = floor(fy);
  const float  ax     = fx - x0;
  const float  ay     = fy - y0;
  const int    ix     = int(x0);
  const int    iy     = int(y0);
  const float4 a      = DiffusionDecode(DiffusionFetch(image, ix, iy));
  const float4 b      = DiffusionDecode(DiffusionFetch(image, ix + 1, iy));
  const float4 c      = DiffusionDecode(DiffusionFetch(image, ix, iy + 1));
  const float4 d      = DiffusionDecode(DiffusionFetch(image, ix + 1, iy + 1));
  const float4 top    = a * (1.0f - ax) + b * ax;
  const float4 bottom = c * (1.0f - ax) + d * ax;
  return top * (1.0f - ay) + bottom * ay;
}

static inline float4 DiffusionBSplineWeights(float t) {
  const float t2 = t * t;
  const float t3 = t2 * t;
  const float u  = 1.0f - t;
  return float4(u * u * u / 6.0f, (3.0f * t3 - 6.0f * t2 + 4.0f) / 6.0f,
                (-3.0f * t3 + 3.0f * t2 + 3.0f * t + 1.0f) / 6.0f, t3 / 6.0f);
}

/// Cubic B-spline sample; smooth magnification of the coarse scatter image.
static inline float4 DiffusionSampleBSpline(texture2d<float, access::read> image, float px,
                                            float py) {
  const float  fx  = px - 0.5f;
  const float  fy  = py - 0.5f;
  const float  x0  = floor(fx);
  const float  y0  = floor(fy);
  const float4 wx  = DiffusionBSplineWeights(fx - x0);
  const float4 wy  = DiffusionBSplineWeights(fy - y0);
  const int    ix  = int(x0) - 1;
  const int    iy  = int(y0) - 1;
  float4       sum = float4(0.0f);
  for (int j = 0; j < 4; ++j) {
    float4 row = float4(0.0f);
    for (int i = 0; i < 4; ++i) {
      row += DiffusionFetch(image, ix + i, iy + j) * wx[i];
    }
    sum += row * wy[j];
  }
  return sum;
}

/// Linear AP1 with the highlight boost, a smoothstep of the peak channel in log2 exposure from
/// low to high stops. Negative (out-of-gamut) light does not scatter.
static inline float3 DiffusionBoostHighlights(float4 linear, float gain, float low, float high) {
  const float r    = fmax(linear.x, 0.0f);
  const float g    = fmax(linear.y, 0.0f);
  const float b    = fmax(linear.z, 0.0f);
  const float peak = fmax(r, fmax(g, b));
  const float t    = fmin(fmax((log2(fmax(peak, 1.0e-6f)) - low) / (high - low), 0.0f), 1.0f);
  const float lift = 1.0f + gain * t * t * (3.0f - 2.0f * t);
  return float3(r * lift, g * lift, b * lift);
}

/// Base level: average of samples x samples decoded, boosted render samples inside the footprint
/// of one base texel. base_to_render maps base texel coordinates to the render.
kernel void diffusion_filter_reduce_boost(texture2d<float, access::read> src [[texture(0)]],
                                          texture2d<float, access::write> dst [[texture(1)]],
                                          constant DiffusionReduceParams& params [[buffer(0)]],
                                          uint2 gid [[thread_position_in_grid]]) {
  const int x = int(gid.x);
  const int y = int(gid.y);
  if (x >= params.dst_width || y >= params.dst_height) {
    return;
  }
  const float step = 1.0f / float(params.samples);
  float3      sum  = float3(0.0f);
  for (int j = 0; j < params.samples; ++j) {
    const float by = float(y) + (float(j) + 0.5f) * step;
    for (int i = 0; i < params.samples; ++i) {
      const float  bx     = float(x) + (float(i) + 0.5f) * step;
      const float2 render = DiffusionTransform(params.base_to_render, bx, by);
      const float4 linear = DiffusionBilinearDecoded(src, render.x, render.y);
      sum += DiffusionBoostHighlights(linear, params.gain, params.low, params.high);
    }
  }
  const float inv = step * step;
  dst.write(float4(sum * inv, 1.0f), gid);
}

/// 13-tap downsample (Jimenez, SIGGRAPH 2014). Destination texel `x` spans source [2x, 2x + 2).
kernel void diffusion_filter_downsample(texture2d<float, access::read> src [[texture(0)]],
                                        texture2d<float, access::write> dst [[texture(1)]],
                                        uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) {
    return;
  }
  const float  cx      = 2.0f * (float(gid.x) + 0.5f);
  const float  cy      = 2.0f * (float(gid.y) + 0.5f);
  const float4 center  = DiffusionBilinear(src, cx, cy);
  const float4 corners = DiffusionBilinear(src, cx - 2.0f, cy - 2.0f) +
                         DiffusionBilinear(src, cx + 2.0f, cy - 2.0f) +
                         DiffusionBilinear(src, cx - 2.0f, cy + 2.0f) +
                         DiffusionBilinear(src, cx + 2.0f, cy + 2.0f);
  const float4 edges   = DiffusionBilinear(src, cx, cy - 2.0f) +
                         DiffusionBilinear(src, cx - 2.0f, cy) +
                         DiffusionBilinear(src, cx + 2.0f, cy) +
                         DiffusionBilinear(src, cx, cy + 2.0f);
  const float4 inner   = DiffusionBilinear(src, cx - 1.0f, cy - 1.0f) +
                         DiffusionBilinear(src, cx + 1.0f, cy - 1.0f) +
                         DiffusionBilinear(src, cx - 1.0f, cy + 1.0f) +
                         DiffusionBilinear(src, cx + 1.0f, cy + 1.0f);
  dst.write(center * 0.125f + corners * 0.03125f + edges * 0.0625f + inner * 0.125f, gid);
}

/// dst = level_weight * level + coarse_weight * tent_upsample(coarse) at the level extent.
kernel void diffusion_filter_upsample_accumulate(
    texture2d<float, access::read> coarse [[texture(0)]],
    texture2d<float, access::read> level [[texture(1)]],
    texture2d<float, access::write> dst [[texture(2)]], constant float& coarse_weight [[buffer(0)]],
    constant float& level_weight [[buffer(1)]], uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) {
    return;
  }
  const float  cx      = (float(gid.x) + 0.5f) * 0.5f;
  const float  cy      = (float(gid.y) + 0.5f) * 0.5f;
  const float4 center  = DiffusionBilinear(coarse, cx, cy);
  const float4 edges   = DiffusionBilinear(coarse, cx - 1.0f, cy) +
                         DiffusionBilinear(coarse, cx + 1.0f, cy) +
                         DiffusionBilinear(coarse, cx, cy - 1.0f) +
                         DiffusionBilinear(coarse, cx, cy + 1.0f);
  const float4 corners = DiffusionBilinear(coarse, cx - 1.0f, cy - 1.0f) +
                         DiffusionBilinear(coarse, cx + 1.0f, cy - 1.0f) +
                         DiffusionBilinear(coarse, cx - 1.0f, cy + 1.0f) +
                         DiffusionBilinear(coarse, cx + 1.0f, cy + 1.0f);
  const float4 tent    = (center * 4.0f + edges * 2.0f + corners) * (1.0f / 16.0f);
  dst.write(level.read(gid) * level_weight + tent * coarse_weight, gid);
}

/// out = T * ((1 - s) * I + s * B); B is the scatter image at the reference position of the
/// render pixel, so every render of the same frame reads the same glow.
kernel void diffusion_filter_mix(texture2d<float, access::read> input [[texture(0)]],
                                 texture2d<float, access::write> output [[texture(1)]],
                                 texture2d<float, access::read> scatter [[texture(2)]],
                                 constant DiffusionMixParams& params [[buffer(0)]],
                                 uint2 gid [[thread_position_in_grid]]) {
  if (int(gid.x) >= params.width || int(gid.y) >= params.height) {
    return;
  }
  const float4 linear = DiffusionDecode(input.read(gid));
  const float2 base =
      DiffusionTransform(params.render_to_base, float(gid.x) + 0.5f, float(gid.y) + 0.5f);
  const float4 glow   = DiffusionSampleBSpline(scatter, base.x, base.y);
  const float  direct = 1.0f - params.scatter_fraction;
  const float  t      = params.transmission;
  const float  s      = params.scatter_fraction;
  output.write(float4(t * (direct * linear.xyz + s * glow.xyz), linear.w), gid);
}

#include "drt_neighbor.metal"
