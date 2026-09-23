//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/drt/aces_odt_runtime.hpp"

#include <bit>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace alcedo::odt_cpu {
namespace {

struct ACESODTCacheKey {
  ColorUtils::ColorSpace limiting_space_      = ColorUtils::ColorSpace::REC709;
  std::uint32_t          peak_luminance_bits_ = 0;

  auto operator==(const ACESODTCacheKey& other) const -> bool {
    return limiting_space_ == other.limiting_space_ &&
           peak_luminance_bits_ == other.peak_luminance_bits_;
  }
};

struct ACESODTCacheKeyHash {
  auto operator()(const ACESODTCacheKey& key) const noexcept -> std::size_t {
    std::size_t h = std::hash<int>{}(static_cast<int>(key.limiting_space_));
    h ^= std::hash<std::uint32_t>{}(key.peak_luminance_bits_) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
  }
};

auto RuntimeCache() -> std::unordered_map<ACESODTCacheKey, ColorUtils::ODTParams, ACESODTCacheKeyHash>& {
  static std::unordered_map<ACESODTCacheKey, ColorUtils::ODTParams, ACESODTCacheKeyHash> cache;
  return cache;
}

auto RuntimeCacheMutex() -> std::mutex& {
  static std::mutex cache_mutex;
  return cache_mutex;
}

auto BuildCacheKey(ColorUtils::ColorSpace limiting_space, float peak_luminance) -> ACESODTCacheKey {
  return ACESODTCacheKey{limiting_space, std::bit_cast<std::uint32_t>(peak_luminance)};
}

auto InitJMhParams(const ColorUtils::ColorSpacePrimaries& prims) -> ColorUtils::JMhParams {
  using namespace ColorUtils;

  const cv::Matx33f RGB_to_XYZ = RGB_TO_XYZ_f33(prims, 1.f);
  const cv::Matx13f XYZ_w      = cv::Matx13f(ref_lum, ref_lum, ref_lum) * RGB_to_XYZ;

  const float       Y_w        = XYZ_w(1);
  const cv::Matx13f RGW_w      = XYZ_w * MATRIX_16;

  const float       k          = 1.f / (5.f * L_A + 1.f);
  const float       k4         = k * k * k * k;
  const float       F_L =
      0.2f * k4 * (5.f * L_A) + 0.1f * powf(1.f - k4, 2.f) * powf(5.f * L_A, 1.f / 3.f);

  const float       F_L_n  = F_L / ref_lum;
  const float       cz     = model_gamma;

  const cv::Matx13f D_RGB  = {F_L_n * Y_w / RGW_w(0), F_L_n * Y_w / RGW_w(1),
                              F_L_n * Y_w / RGW_w(2)};

  const cv::Matx13f RGB_wc = {D_RGB(0) * RGW_w(0), D_RGB(1) * RGW_w(1), D_RGB(2) * RGW_w(2)};
  const cv::Matx13f RGB_Aw = {pacrc_fwd(RGB_wc(0)), pacrc_fwd(RGB_wc(1)), pacrc_fwd(RGB_wc(2))};

  cv::Matx33f       cone_response_to_Aab =
      cv::Matx33f::diag({cam_nl_scale, cam_nl_scale, cam_nl_scale}) * base_cone_repponse_to_Aab;
  const float A_w = cone_response_to_Aab(0, 0) * RGB_Aw(0) +
                    cone_response_to_Aab(1, 0) * RGB_Aw(1) +
                    cone_response_to_Aab(2, 0) * RGB_Aw(2);
  const float A_w_J = _pacrc_fwd_(F_L);

  cv::Matx33f M1                  = RGB_to_XYZ * MATRIX_16;
  cv::Matx33f M2                  = cv::Matx33f::diag({ref_lum, ref_lum, ref_lum});
  cv::Matx33f MATRIX_RGB_to_CAM16 = M1 * M2;
  cv::Matx33f MATRIX_RGB_to_CAM16_c =
      MATRIX_RGB_to_CAM16 * cv::Matx33f::diag({D_RGB(0), D_RGB(1), D_RGB(2)});

  cv::Matx33f MATRIX_cone_response_to_Aab = {cone_response_to_Aab(0, 0) / A_w,
                                             cone_response_to_Aab(0, 1) * 43.f * surround[2],
                                             cone_response_to_Aab(0, 2) * 43.f * surround[2],
                                             cone_response_to_Aab(1, 0) / A_w,
                                             cone_response_to_Aab(1, 1) * 43.f * surround[2],
                                             cone_response_to_Aab(1, 2) * 43.f * surround[2],
                                             cone_response_to_Aab(2, 0) / A_w,
                                             cone_response_to_Aab(2, 1) * 43.f * surround[2],
                                             cone_response_to_Aab(2, 2) * 43.f * surround[2]};

  JMhParams p;
  p.MATRIX_RGB_to_CAM16_c_       = MATRIX_RGB_to_CAM16_c;
  p.MATRIX_CAM16_c_to_RGB_       = MATRIX_RGB_to_CAM16_c.inv();
  p.MATRIX_cone_response_to_Aab_ = MATRIX_cone_response_to_Aab;
  p.MATRIX_Aab_to_cone_response_ = MATRIX_cone_response_to_Aab.inv();
  p.F_L_n_                       = F_L_n;
  p.cz_                          = cz;
  p.inv_cz_                      = 1.f / cz;
  p.A_w_z_                       = A_w_J;
  p.inv_A_w_J_                   = 1.f / A_w_J;
  return p;
}

void InitTSParams(float peak_luminance, ColorUtils::ODTParams* odt) {
  const float n         = peak_luminance;
  const float n_r       = 100.0f;
  const float g         = 1.15f;
  const float c         = 0.18f;
  const float c_d       = 10.013f;
  const float w_g       = 0.14f;
  const float t_1       = 0.04f;
  const float r_hit_min = 128.f;
  const float r_hit_max = 896.f;

  const float r_hit  = r_hit_min + (r_hit_max - r_hit_min) * (log(n / n_r) / log(10000.f / 100.f));
  const float m_0    = (n / n_r);
  const float m_1    = 0.5f * (m_0 + sqrt(m_0 * (m_0 + 4.f * t_1)));
  const float u      = pow((r_hit / m_1) / ((r_hit / m_1) + 1), g);
  const float m      = m_1 / u;
  const float w_i    = log(n / 100.f) / log(2.f);
  const float c_t    = c_d / n_r * (1.f + w_i * w_g);
  const float g_ip   = 0.5f * (c_t + sqrt(c_t * (c_t + 4.f * t_1)));
  const float g_ipp2 = -(m_1 * pow((g_ip / m), (1.f / g))) / (pow(g_ip / m, 1.f / g) - 1.f);
  const float w_2    = c / g_ipp2;
  const float s_2    = w_2 * m_1;
  const float u_2    = pow((r_hit / m_1) / ((r_hit / m_1) + w_2), g);
  const float m_2    = m_1 / u_2;

  odt->ts_params_ = {n,
                     n_r,
                     g,
                     t_1,
                     c_t,
                     s_2,
                     u_2,
                     m_2,
                     8.f * r_hit,
                     n / (u_2 * n_r),
                     log10(n / n_r)};
}

}  // namespace

auto ResolveACESODTRuntime(ColorUtils::ColorSpace limiting_space,
                           float peak_luminance) -> ColorUtils::ODTParams {
  const ACESODTCacheKey      cache_key = BuildCacheKey(limiting_space, peak_luminance);
  std::lock_guard<std::mutex> lock(RuntimeCacheMutex());
  auto&                       cache = RuntimeCache();
  const auto                  cached_it = cache.find(cache_key);
  if (cached_it != cache.end()) {
    return cached_it->second;
  }

  using namespace ColorUtils;

  ODTParams runtime;
  runtime.peak_luminance_ = peak_luminance;
  runtime.input_params_   = InitJMhParams(AP0_PRIMARY);
  runtime.reach_params_   = InitJMhParams(REACH_PRIMARY);
  runtime.limit_params_   = InitJMhParams(SpaceEnumToPrimary(limiting_space));
  InitTSParams(peak_luminance, &runtime);

  TSParams& ts            = runtime.ts_params_;

  runtime.limit_J_max_    = Y_to_J(peak_luminance, runtime.input_params_);
  runtime.model_gamma_inv_ = 1.f / model_gamma;
  runtime.table_reach_M_   = MakeReachMTable(runtime.reach_params_, runtime.limit_J_max_);

  runtime.sat_                   =
      fmaxf(0.2f, chroma_expand - (chroma_expand * chroma_expand_fact) * ts.log_peak_);
  runtime.sat_thr_               = chroma_expand_thr / peak_luminance;
  runtime.compr_                 =
      chroma_compress + (chroma_compress * chroma_compress_fact) * ts.log_peak_;
  runtime.chroma_compress_scale_ = powf(0.03379f * peak_luminance, 0.30596f) - 0.45135f;

  runtime.mid_J_                 = Y_to_J(ts.c_t_ * ref_lum, runtime.input_params_);
  runtime.focus_dist_            =
      focus_distance + focus_distance * focus_distance_scaling * ts.log_peak_;
  const float lower_hull_gamma   = 1.14f + 0.07f * ts.log_peak_;
  runtime.lower_hull_gamma_      = lower_hull_gamma;
  runtime.lower_hull_gamma_inv_  = 1.f / lower_hull_gamma;
  runtime.table_gamut_cusps_     =
      MakeUniformHueGamutTable(runtime.reach_params_, runtime.limit_params_, runtime);

  runtime.table_hues_ = std::make_shared<std::array<float, TOTAL_TABLE_SIZE>>();
  for (int i = 0; i < TOTAL_TABLE_SIZE; ++i) {
    (*runtime.table_hues_)[i] = (*runtime.table_gamut_cusps_)[i](2);
  }
  runtime.table_upper_hull_gammas_ = std::make_shared<std::array<float, TOTAL_TABLE_SIZE>>(
      MakeUpperHullGammaTable(*runtime.table_gamut_cusps_, runtime));
  runtime.hue_linearity_search_range_ = DetermineHueLinearitySearchRange(*runtime.table_hues_);

  cache.emplace(cache_key, runtime);
  return runtime;
}

}  // namespace alcedo::odt_cpu
