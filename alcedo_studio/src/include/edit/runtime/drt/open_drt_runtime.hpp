//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//
//  This file also contains material subject to the upstream notices below.

//  This file contains GPLv3-derived logic based on OpenDRT v1.1.0
//  by Jed Smith: https://github.com/jedypod/open-display-transform

#pragma once

#include <string>
#include <string_view>

#include "edit/operators/utils/color_utils.hpp"

namespace alcedo::odt_cpu {

enum class OpenDRTLookPreset : int {
  CUSTOM   = -1,
  STANDARD = 0,
  ARRIBA,
  SYLVAN,
  COLORFUL,
  AERY,
  DYSTOPIC,
  UMBRA,
};

enum class OpenDRTTonescalePreset : int {
  CUSTOM          = -1,
  USE_LOOK_PRESET = 0,
  LOW_CONTRAST,
  MEDIUM_CONTRAST,
  HIGH_CONTRAST,
  ARRIBA_TONESCALE,
  SYLVAN_TONESCALE,
  COLORFUL_TONESCALE,
  AERY_TONESCALE,
  DYSTOPIC_TONESCALE,
  UMBRA_TONESCALE,
  ACES_1_X,
  ACES_2_0,
  MARVELOUS_TONESCAPE,
  DAGRINCHI_TONEGROAN,
};

enum class OpenDRTCreativeWhitePreset : int {
  USE_LOOK_PRESET = 0,
  D93,
  D75,
  D65,
  D60,
  D55,
  D50,
};

struct OpenDRTDetailedSettings {
  int   tn_hcon_enable_ = 0;
  int   tn_lcon_enable_ = 0;
  int   pt_enable_      = 1;
  int   ptl_enable_     = 1;
  int   ptm_enable_     = 1;
  int   brl_enable_     = 1;
  int   brlp_enable_    = 1;
  int   hc_enable_      = 1;
  int   hs_rgb_enable_  = 1;
  int   hs_cmy_enable_  = 1;

  float tn_con_         = 1.66f;
  float tn_sh_          = 0.5f;
  float tn_toe_         = 0.003f;
  float tn_off_         = 0.005f;
  float tn_hcon_        = 0.0f;
  float tn_hcon_pv_     = 1.0f;
  float tn_hcon_st_     = 4.0f;
  float tn_lcon_        = 0.0f;
  float tn_lcon_w_      = 0.5f;
  float cwp_lm_         = 0.25f;
  float rs_sa_          = 0.35f;
  float rs_rw_          = 0.25f;
  float rs_bw_          = 0.55f;
  float pt_lml_         = 0.25f;
  float pt_lml_r_       = 0.5f;
  float pt_lml_g_       = 0.0f;
  float pt_lml_b_       = 0.1f;
  float pt_lmh_         = 0.25f;
  float pt_lmh_r_       = 0.5f;
  float pt_lmh_b_       = 0.0f;
  float ptl_c_          = 0.06f;
  float ptl_m_          = 0.08f;
  float ptl_y_          = 0.06f;
  float ptm_low_        = 0.4f;
  float ptm_low_rng_    = 0.25f;
  float ptm_low_st_     = 0.5f;
  float ptm_high_       = -0.8f;
  float ptm_high_rng_   = 0.35f;
  float ptm_high_st_    = 0.4f;
  float brl_            = 0.0f;
  float brl_r_          = -2.5f;
  float brl_g_          = -1.5f;
  float brl_b_          = -1.5f;
  float brl_rng_        = 0.5f;
  float brl_st_         = 0.35f;
  float brlp_           = -0.5f;
  float brlp_r_         = -1.25f;
  float brlp_g_         = -1.25f;
  float brlp_b_         = -0.25f;
  float hc_r_           = 1.0f;
  float hc_r_rng_       = 0.3f;
  float hs_r_           = 0.6f;
  float hs_r_rng_       = 0.6f;
  float hs_g_           = 0.35f;
  float hs_g_rng_       = 1.0f;
  float hs_b_           = 0.66f;
  float hs_b_rng_       = 1.0f;
  float hs_c_           = 0.25f;
  float hs_c_rng_       = 1.0f;
  float hs_m_           = 0.0f;
  float hs_m_rng_       = 1.0f;
  float hs_y_           = 0.0f;
  float hs_y_rng_       = 1.0f;
};

struct OpenDRTSettings {
  OpenDRTLookPreset          look_preset_            = OpenDRTLookPreset::STANDARD;
  OpenDRTTonescalePreset     tonescale_preset_       = OpenDRTTonescalePreset::USE_LOOK_PRESET;
  OpenDRTCreativeWhitePreset creative_white_         = OpenDRTCreativeWhitePreset::USE_LOOK_PRESET;
  float                      creative_white_limit_   = 0.25f;
  float                      display_grey_luminance_ = 10.0f;
  float                      hdr_grey_boost_         = 0.13f;
  float                      hdr_purity_             = 0.5f;
  OpenDRTDetailedSettings    detailed_               = {};
};

auto OpenDRTLookPresetFromString(std::string_view value) -> OpenDRTLookPreset;
auto OpenDRTTonescalePresetFromString(std::string_view value) -> OpenDRTTonescalePreset;
auto OpenDRTCreativeWhitePresetFromString(std::string_view value) -> OpenDRTCreativeWhitePreset;

auto OpenDRTLookPresetToString(OpenDRTLookPreset value) -> std::string;
auto OpenDRTTonescalePresetToString(OpenDRTTonescalePreset value) -> std::string;
auto OpenDRTCreativeWhitePresetToString(OpenDRTCreativeWhitePreset value) -> std::string;

void ApplyOpenDRTLookPresetToSettings(OpenDRTLookPreset look, OpenDRTSettings* settings);
void ApplyOpenDRTTonescalePresetToSettings(OpenDRTTonescalePreset tonescale,
                                           OpenDRTSettings*       settings);
void CopyOpenDRTDetailToRuntime(const OpenDRTDetailedSettings& detailed,
                                ColorUtils::OpenDRTParams*     runtime);
void CopyOpenDRTRuntimeToDetail(const ColorUtils::OpenDRTParams& runtime,
                                OpenDRTDetailedSettings*         detailed);
void CopyOpenDRTTonescaleToDetail(const ColorUtils::OpenDRTParams& runtime,
                                  OpenDRTDetailedSettings*         detailed);

auto ResolveOpenDRTRuntime(ColorUtils::ColorSpace encoding_space, ColorUtils::EOTF encoding_eotf,
                           float peak_luminance, const OpenDRTSettings& settings)
    -> ColorUtils::OpenDRTParams;

auto ResolveOpenDRTDisplayLinearScale(const ColorUtils::OpenDRTParams& params) -> float;

}  // namespace alcedo::odt_cpu
