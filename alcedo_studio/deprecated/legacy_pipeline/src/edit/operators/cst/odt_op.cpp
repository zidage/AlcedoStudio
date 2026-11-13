//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/cst/odt_op.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>

#include "edit/runtime/drt/aces_odt_runtime.hpp"

namespace alcedo {
namespace {

auto OpenDRTDetailedParamsToJson(const odt_cpu::OpenDRTDetailedSettings& p) -> nlohmann::json {
  return {{"tn_con", p.tn_con_},
          {"tn_sh", p.tn_sh_},
          {"tn_toe", p.tn_toe_},
          {"tn_off", p.tn_off_},
          {"tn_hcon", p.tn_hcon_},
          {"tn_hcon_pv", p.tn_hcon_pv_},
          {"tn_hcon_st", p.tn_hcon_st_},
          {"tn_lcon", p.tn_lcon_},
          {"tn_lcon_w", p.tn_lcon_w_},
          {"cwp_lm", p.cwp_lm_},
          {"rs_sa", p.rs_sa_},
          {"rs_rw", p.rs_rw_},
          {"rs_bw", p.rs_bw_},
          {"pt_lml", p.pt_lml_},
          {"pt_lml_r", p.pt_lml_r_},
          {"pt_lml_g", p.pt_lml_g_},
          {"pt_lml_b", p.pt_lml_b_},
          {"pt_lmh", p.pt_lmh_},
          {"pt_lmh_r", p.pt_lmh_r_},
          {"pt_lmh_b", p.pt_lmh_b_},
          {"ptl_c", p.ptl_c_},
          {"ptl_m", p.ptl_m_},
          {"ptl_y", p.ptl_y_},
          {"ptm_low", p.ptm_low_},
          {"ptm_low_rng", p.ptm_low_rng_},
          {"ptm_low_st", p.ptm_low_st_},
          {"ptm_high", p.ptm_high_},
          {"ptm_high_rng", p.ptm_high_rng_},
          {"ptm_high_st", p.ptm_high_st_},
          {"brl", p.brl_},
          {"brl_r", p.brl_r_},
          {"brl_g", p.brl_g_},
          {"brl_b", p.brl_b_},
          {"brl_rng", p.brl_rng_},
          {"brl_st", p.brl_st_},
          {"brlp", p.brlp_},
          {"brlp_r", p.brlp_r_},
          {"brlp_g", p.brlp_g_},
          {"brlp_b", p.brlp_b_},
          {"hc_r", p.hc_r_},
          {"hc_r_rng", p.hc_r_rng_},
          {"hs_r", p.hs_r_},
          {"hs_r_rng", p.hs_r_rng_},
          {"hs_g", p.hs_g_},
          {"hs_g_rng", p.hs_g_rng_},
          {"hs_b", p.hs_b_},
          {"hs_b_rng", p.hs_b_rng_},
          {"hs_c", p.hs_c_},
          {"hs_c_rng", p.hs_c_rng_},
          {"hs_m", p.hs_m_},
          {"hs_m_rng", p.hs_m_rng_},
          {"hs_y", p.hs_y_},
          {"hs_y_rng", p.hs_y_rng_}};
}

void LoadOpenDRTDetailedParams(const nlohmann::json& j, odt_cpu::OpenDRTDetailedSettings* p) {
  if (!p || !j.is_object()) {
    return;
  }
  auto read = [&j](const char* key, float* out) {
    if (j.contains(key) && j.at(key).is_number()) {
      *out = j.at(key).get<float>();
    }
  };
  read("tn_con", &p->tn_con_);
  read("tn_sh", &p->tn_sh_);
  read("tn_toe", &p->tn_toe_);
  read("tn_off", &p->tn_off_);
  read("tn_hcon", &p->tn_hcon_);
  read("tn_hcon_pv", &p->tn_hcon_pv_);
  read("tn_hcon_st", &p->tn_hcon_st_);
  read("tn_lcon", &p->tn_lcon_);
  read("tn_lcon_w", &p->tn_lcon_w_);
  read("cwp_lm", &p->cwp_lm_);
  read("rs_sa", &p->rs_sa_);
  read("rs_rw", &p->rs_rw_);
  read("rs_bw", &p->rs_bw_);
  read("pt_lml", &p->pt_lml_);
  read("pt_lml_r", &p->pt_lml_r_);
  read("pt_lml_g", &p->pt_lml_g_);
  read("pt_lml_b", &p->pt_lml_b_);
  read("pt_lmh", &p->pt_lmh_);
  read("pt_lmh_r", &p->pt_lmh_r_);
  read("pt_lmh_b", &p->pt_lmh_b_);
  read("ptl_c", &p->ptl_c_);
  read("ptl_m", &p->ptl_m_);
  read("ptl_y", &p->ptl_y_);
  read("ptm_low", &p->ptm_low_);
  read("ptm_low_rng", &p->ptm_low_rng_);
  read("ptm_low_st", &p->ptm_low_st_);
  read("ptm_high", &p->ptm_high_);
  read("ptm_high_rng", &p->ptm_high_rng_);
  read("ptm_high_st", &p->ptm_high_st_);
  read("brl", &p->brl_);
  read("brl_r", &p->brl_r_);
  read("brl_g", &p->brl_g_);
  read("brl_b", &p->brl_b_);
  read("brl_rng", &p->brl_rng_);
  read("brl_st", &p->brl_st_);
  read("brlp", &p->brlp_);
  read("brlp_r", &p->brlp_r_);
  read("brlp_g", &p->brlp_g_);
  read("brlp_b", &p->brlp_b_);
  read("hc_r", &p->hc_r_);
  read("hc_r_rng", &p->hc_r_rng_);
  read("hs_r", &p->hs_r_);
  read("hs_r_rng", &p->hs_r_rng_);
  read("hs_g", &p->hs_g_);
  read("hs_g_rng", &p->hs_g_rng_);
  read("hs_b", &p->hs_b_);
  read("hs_b_rng", &p->hs_b_rng_);
  read("hs_c", &p->hs_c_);
  read("hs_c_rng", &p->hs_c_rng_);
  read("hs_m", &p->hs_m_);
  read("hs_m_rng", &p->hs_m_rng_);
  read("hs_y", &p->hs_y_);
  read("hs_y_rng", &p->hs_y_rng_);
}

}  // namespace

ODT_Op::ODT_Op(const nlohmann::json& params) { SetParams(params); }

void ODT_Op::Apply(std::shared_ptr<ImageBuffer>) {
  throw std::runtime_error(
      "ODT_Op: Use the pipeline output transform stage. This operator is a descriptor for the "
      "GPU/CPU output transform runtime.");
}

void ODT_Op::ApplyGPU(std::shared_ptr<ImageBuffer>) {
  throw std::runtime_error("ODT_Op: GPU Apply is handled by the pipeline output stage.");
}

auto ODT_Op::GetParams() const -> nlohmann::json {
  nlohmann::json odt;
  odt["method"]         = ColorUtils::ODTMethodToString(method_);
  odt["encoding_space"] = ColorUtils::ColorSpaceToString(encoding_space_);
  odt["encoding_eotf"]  = ColorUtils::EOTFToString(encoding_eotf_);
  odt["limiting_space"] = ColorUtils::ColorSpaceToString(limiting_space_);
  odt["peak_luminance"] = peak_luminance_;

  odt["open_drt"]       = {
      {"look_preset", odt_cpu::OpenDRTLookPresetToString(open_drt_settings_.look_preset_)},
      {"tonescale_preset",
             odt_cpu::OpenDRTTonescalePresetToString(open_drt_settings_.tonescale_preset_)},
      {"creative_white",
             odt_cpu::OpenDRTCreativeWhitePresetToString(open_drt_settings_.creative_white_)},
      {"creative_white_limit", open_drt_settings_.creative_white_limit_},
      {"display_grey_luminance", open_drt_settings_.display_grey_luminance_},
      {"hdr_grey_boost", open_drt_settings_.hdr_grey_boost_},
      {"hdr_purity", open_drt_settings_.hdr_purity_},
      {"parameters", OpenDRTDetailedParamsToJson(open_drt_settings_.detailed_)},
  };

  return {{std::string(script_name_), odt}};
}

void ODT_Op::SetParams(const nlohmann::json& in_j) {
  if (!in_j.contains(script_name_) || !in_j.at(script_name_).is_object()) {
    return;
  }

  const auto& j = in_j.at(script_name_);

  if (j.contains("method") && j.at("method").is_string()) {
    const std::string method_str = j.at("method").get<std::string>();
    if (method_str != "open_drt" && method_str != "aces_2_0") {
      throw std::runtime_error("ODT_Op: unsupported odt.method \"" + method_str + "\".");
    }
    method_ = ColorUtils::ODTMethodFromString(method_str);
  }
  if (j.contains("encoding_space") && j.at("encoding_space").is_string()) {
    encoding_space_ = ColorUtils::ColorSpaceFromString(j.at("encoding_space").get<std::string>());
  }
  if (j.contains("encoding_eotf") && j.at("encoding_eotf").is_string()) {
    encoding_eotf_ = ColorUtils::EOTFFromString(j.at("encoding_eotf").get<std::string>());
  }
  if (j.contains("limiting_space") && j.at("limiting_space").is_string()) {
    limiting_space_ = ColorUtils::ColorSpaceFromString(j.at("limiting_space").get<std::string>());
  }
  if (j.contains("peak_luminance") && j.at("peak_luminance").is_number()) {
    peak_luminance_ = j.at("peak_luminance").get<float>();
  }

  if (j.contains("open_drt") && j.at("open_drt").is_object()) {
    const auto& open_drt = j.at("open_drt");
    if (open_drt.contains("look_preset") && open_drt.at("look_preset").is_string()) {
      open_drt_settings_.look_preset_ =
          odt_cpu::OpenDRTLookPresetFromString(open_drt.at("look_preset").get<std::string>());
    }
    if (open_drt.contains("tonescale_preset") && open_drt.at("tonescale_preset").is_string()) {
      open_drt_settings_.tonescale_preset_ = odt_cpu::OpenDRTTonescalePresetFromString(
          open_drt.at("tonescale_preset").get<std::string>());
    }
    if (open_drt.contains("creative_white") && open_drt.at("creative_white").is_string()) {
      open_drt_settings_.creative_white_ = odt_cpu::OpenDRTCreativeWhitePresetFromString(
          open_drt.at("creative_white").get<std::string>());
    }
    if (open_drt.contains("creative_white_limit") &&
        open_drt.at("creative_white_limit").is_number()) {
      open_drt_settings_.creative_white_limit_ = open_drt.at("creative_white_limit").get<float>();
    }
    if (open_drt.contains("display_grey_luminance") &&
        open_drt.at("display_grey_luminance").is_number()) {
      open_drt_settings_.display_grey_luminance_ =
          open_drt.at("display_grey_luminance").get<float>();
    }
    if (open_drt.contains("hdr_grey_boost") && open_drt.at("hdr_grey_boost").is_number()) {
      open_drt_settings_.hdr_grey_boost_ = open_drt.at("hdr_grey_boost").get<float>();
    }
    if (open_drt.contains("hdr_purity") && open_drt.at("hdr_purity").is_number()) {
      open_drt_settings_.hdr_purity_ = open_drt.at("hdr_purity").get<float>();
    }
    if (open_drt.contains("parameters") && open_drt.at("parameters").is_object()) {
      LoadOpenDRTDetailedParams(open_drt.at("parameters"), &open_drt_settings_.detailed_);
    }
  }

  ValidateParams();
  RebuildRuntime();
}

void ODT_Op::ValidateParams() const {
  if (peak_luminance_ <= 0.0f) {
    throw std::runtime_error("ODT_Op: peak_luminance must be positive.");
  }

  if (method_ == ColorUtils::ODTMethod::OPEN_DRT) {
    if (encoding_space_ == ColorUtils::ColorSpace::PROPHOTO ||
        encoding_space_ == ColorUtils::ColorSpace::ADOBE_RGB) {
      throw std::runtime_error("ODT_Op: OpenDRT does not support encoding_space \"" +
                               ColorUtils::ColorSpaceToString(encoding_space_) + "\".");
    }
  } else {
    if (limiting_space_ == ColorUtils::ColorSpace::XYZ) {
      throw std::runtime_error("ODT_Op: ACES 2.0 limiting_space does not support \"xyz\".");
    }
  }
}

void ODT_Op::RebuildRuntime() {
  to_output_params_                 = {};
  to_output_params_.method_         = method_;
  to_output_params_.encoding_space_ = encoding_space_;
  to_output_params_.eotf_           = encoding_eotf_;
  to_output_params_.peak_luminance_ = peak_luminance_;

  if (method_ == ColorUtils::ODTMethod::ACES_2_0) {
    to_output_params_.aces_params_ =
        odt_cpu::ResolveACESODTRuntime(limiting_space_, peak_luminance_);
    to_output_params_.limit_to_display_matx_ =
        ColorUtils::RGB_TO_XYZ_f33(limiting_space_) * ColorUtils::XYZ_TO_RGB_f33(encoding_space_);
    to_output_params_.display_linear_scale_ =
        (encoding_eotf_ == ColorUtils::EOTF::ST2084) ? ColorUtils::ref_lum : 1.0f;
    return;
  }

  to_output_params_.open_drt_params_ = odt_cpu::ResolveOpenDRTRuntime(
      encoding_space_, encoding_eotf_, peak_luminance_, open_drt_settings_);
  to_output_params_.limit_to_display_matx_ = cv::Matx33f::eye();
  to_output_params_.display_linear_scale_ =
      odt_cpu::ResolveOpenDRTDisplayLinearScale(to_output_params_.open_drt_params_);
}

void ODT_Op::SetGlobalParams(OperatorParams& global_params) const {
  global_params.to_output_params_ = to_output_params_;
  global_params.to_output_dirty_  = true;
}

void ODT_Op::EnableGlobalParams(OperatorParams& params, bool enable) {
  params.to_output_enabled_ = enable;
  params.to_output_dirty_   = enable;
}

}  // namespace alcedo
