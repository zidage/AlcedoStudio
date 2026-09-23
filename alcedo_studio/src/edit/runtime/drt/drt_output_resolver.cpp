//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/drt/drt_output_resolver.hpp"

#include <stdexcept>
#include <string>

#include "edit/runtime/drt/aces_odt_runtime.hpp"
#include "edit/runtime/drt/open_drt_runtime.hpp"

namespace alcedo {
namespace {

void SetError(std::string* error, std::string message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

auto ToMethod(DrtMethod method, ColorUtils::ODTMethod* out) -> bool {
  switch (method) {
    case DrtMethod::Aces20:
      *out = ColorUtils::ODTMethod::ACES_2_0;
      return true;
    case DrtMethod::OpenDrt:
      *out = ColorUtils::ODTMethod::OPEN_DRT;
      return true;
  }
  return false;
}

auto ToColorSpace(DrtColorSpace space, ColorUtils::ColorSpace* out) -> bool {
  switch (space) {
    case DrtColorSpace::Rec709:
      *out = ColorUtils::ColorSpace::REC709;
      return true;
    case DrtColorSpace::Rec2020:
      *out = ColorUtils::ColorSpace::REC2020;
      return true;
    case DrtColorSpace::P3D65:
      *out = ColorUtils::ColorSpace::P3_D65;
      return true;
  }
  return false;
}

auto ToEotf(DrtEotf eotf, ColorUtils::EOTF* out) -> bool {
  switch (eotf) {
    case DrtEotf::Linear:
      *out = ColorUtils::EOTF::LINEAR;
      return true;
    case DrtEotf::St2084:
      *out = ColorUtils::EOTF::ST2084;
      return true;
    case DrtEotf::Hlg:
      *out = ColorUtils::EOTF::HLG;
      return true;
    case DrtEotf::Gamma26:
      *out = ColorUtils::EOTF::GAMMA_2_6;
      return true;
    case DrtEotf::Bt1886:
      *out = ColorUtils::EOTF::BT1886;
      return true;
    case DrtEotf::Gamma22:
      *out = ColorUtils::EOTF::GAMMA_2_2;
      return true;
    case DrtEotf::Gamma18:
      *out = ColorUtils::EOTF::GAMMA_1_8;
      return true;
  }
  return false;
}

// Export encoding keeps the DRT encoding domain: Rec.2020 and P3-D65 pass through and every other
// export space encodes as Rec.709. This is the mapping the export path has always applied.
auto ExportSpaceToDrt(ColorUtils::ColorSpace space) -> ColorUtils::ColorSpace {
  switch (space) {
    case ColorUtils::ColorSpace::REC2020:
    case ColorUtils::ColorSpace::P3_D65:
      return space;
    default:
      return ColorUtils::ColorSpace::REC709;
  }
}

auto ToOpenDrtDetail(const OpenDrtDetailedParams& p) -> odt_cpu::OpenDRTDetailedSettings {
  odt_cpu::OpenDRTDetailedSettings d;
  d.tn_con_       = p.tn_con;
  d.tn_sh_        = p.tn_sh;
  d.tn_toe_       = p.tn_toe;
  d.tn_off_       = p.tn_off;
  d.tn_hcon_      = p.tn_hcon;
  d.tn_hcon_pv_   = p.tn_hcon_pv;
  d.tn_hcon_st_   = p.tn_hcon_st;
  d.tn_lcon_      = p.tn_lcon;
  d.tn_lcon_w_    = p.tn_lcon_w;
  d.cwp_lm_       = p.cwp_lm;
  d.rs_sa_        = p.rs_sa;
  d.rs_rw_        = p.rs_rw;
  d.rs_bw_        = p.rs_bw;
  d.pt_lml_       = p.pt_lml;
  d.pt_lml_r_     = p.pt_lml_r;
  d.pt_lml_g_     = p.pt_lml_g;
  d.pt_lml_b_     = p.pt_lml_b;
  d.pt_lmh_       = p.pt_lmh;
  d.pt_lmh_r_     = p.pt_lmh_r;
  d.pt_lmh_b_     = p.pt_lmh_b;
  d.ptl_c_        = p.ptl_c;
  d.ptl_m_        = p.ptl_m;
  d.ptl_y_        = p.ptl_y;
  d.ptm_low_      = p.ptm_low;
  d.ptm_low_rng_  = p.ptm_low_rng;
  d.ptm_low_st_   = p.ptm_low_st;
  d.ptm_high_     = p.ptm_high;
  d.ptm_high_rng_ = p.ptm_high_rng;
  d.ptm_high_st_  = p.ptm_high_st;
  d.brl_          = p.brl;
  d.brl_r_        = p.brl_r;
  d.brl_g_        = p.brl_g;
  d.brl_b_        = p.brl_b;
  d.brl_rng_      = p.brl_rng;
  d.brl_st_       = p.brl_st;
  d.brlp_         = p.brlp;
  d.brlp_r_       = p.brlp_r;
  d.brlp_g_       = p.brlp_g;
  d.brlp_b_       = p.brlp_b;
  d.hc_r_         = p.hc_r;
  d.hc_r_rng_     = p.hc_r_rng;
  d.hs_r_         = p.hs_r;
  d.hs_r_rng_     = p.hs_r_rng;
  d.hs_g_         = p.hs_g;
  d.hs_g_rng_     = p.hs_g_rng;
  d.hs_b_         = p.hs_b;
  d.hs_b_rng_     = p.hs_b_rng;
  d.hs_c_         = p.hs_c;
  d.hs_c_rng_     = p.hs_c_rng;
  d.hs_m_         = p.hs_m;
  d.hs_m_rng_     = p.hs_m_rng;
  d.hs_y_         = p.hs_y;
  d.hs_y_rng_     = p.hs_y_rng;
  return d;
}

auto ToOpenDrtSettings(const DrtPayload& payload) -> odt_cpu::OpenDRTSettings {
  odt_cpu::OpenDRTSettings settings;
  settings.look_preset_      = odt_cpu::OpenDRTLookPresetFromString(payload.look_preset);
  settings.tonescale_preset_ = odt_cpu::OpenDRTTonescalePresetFromString(payload.tonescale_preset);
  settings.creative_white_ = odt_cpu::OpenDRTCreativeWhitePresetFromString(payload.creative_white);
  settings.creative_white_limit_   = payload.creative_white_limit;
  settings.display_grey_luminance_ = payload.display_grey_luminance;
  settings.hdr_grey_boost_         = payload.hdr_grey_boost;
  settings.hdr_purity_             = payload.hdr_purity;
  settings.detailed_               = ToOpenDrtDetail(payload.parameters);
  return settings;
}

}  // namespace

auto DrtOutputResolver::Resolve(const DrtPayload&               payload,
                                const ExportColorProfileConfig* export_encoding,
                                ColorUtils::TO_OUTPUT_Params* output, std::string* error) -> bool {
  ColorUtils::ODTMethod  method{};
  ColorUtils::ColorSpace encoding_space{};
  ColorUtils::ColorSpace limiting_space{};
  ColorUtils::EOTF       encoding_eotf{};
  if (!ToMethod(payload.method, &method)) {
    SetError(error, "DrtOutputResolver: unsupported DRT method " +
                        std::to_string(static_cast<int>(payload.method)) + ".");
    return false;
  }
  if (!ToColorSpace(payload.limiting_space, &limiting_space)) {
    SetError(error, "DrtOutputResolver: unsupported DRT limiting space " +
                        std::to_string(static_cast<int>(payload.limiting_space)) + ".");
    return false;
  }
  float peak_luminance = payload.peak_luminance;
  if (export_encoding != nullptr) {
    encoding_space = ExportSpaceToDrt(export_encoding->encoding_space);
    encoding_eotf  = export_encoding->encoding_eotf;
    peak_luminance = export_encoding->peak_luminance;
  } else {
    if (!ToColorSpace(payload.encoding_space, &encoding_space)) {
      SetError(error, "DrtOutputResolver: unsupported DRT encoding space " +
                          std::to_string(static_cast<int>(payload.encoding_space)) + ".");
      return false;
    }
    if (!ToEotf(payload.encoding_eotf, &encoding_eotf)) {
      SetError(error, "DrtOutputResolver: unsupported DRT encoding EOTF " +
                          std::to_string(static_cast<int>(payload.encoding_eotf)) + ".");
      return false;
    }
  }
  if (peak_luminance <= 0.0f) {
    SetError(error, "DrtOutputResolver: peak_luminance must be positive.");
    return false;
  }

  ColorUtils::TO_OUTPUT_Params resolved{};
  resolved.method_         = method;
  resolved.encoding_space_ = encoding_space;
  resolved.eotf_           = encoding_eotf;
  resolved.peak_luminance_ = peak_luminance;
  // The ACES and OpenDRT precompute reject inputs they cannot resolve, for example an OpenDRT
  // encoding space and EOTF pair that has no display encoding.
  try {
    if (method == ColorUtils::ODTMethod::ACES_2_0) {
      resolved.aces_params_ = odt_cpu::ResolveACESODTRuntime(limiting_space, peak_luminance);
      resolved.limit_to_display_matx_ =
          ColorUtils::RGB_TO_XYZ_f33(limiting_space) * ColorUtils::XYZ_TO_RGB_f33(encoding_space);
      resolved.display_linear_scale_ =
          (encoding_eotf == ColorUtils::EOTF::ST2084) ? ColorUtils::ref_lum : 1.0f;
    } else {
      resolved.open_drt_params_ = odt_cpu::ResolveOpenDRTRuntime(
          encoding_space, encoding_eotf, peak_luminance, ToOpenDrtSettings(payload));
      resolved.limit_to_display_matx_ = cv::Matx33f::eye();
      resolved.display_linear_scale_ =
          odt_cpu::ResolveOpenDRTDisplayLinearScale(resolved.open_drt_params_);
    }
  } catch (const std::runtime_error& failure) {
    SetError(error, std::string("DrtOutputResolver: ") + failure.what());
    return false;
  }
  *output = std::move(resolved);
  return true;
}

auto DrtOutputResolver::ResolveNode(const DrtNodeModel&                            drt,
                                    const std::optional<ExportColorProfileConfig>& export_encoding,
                                    std::string_view caller) -> ColorUtils::TO_OUTPUT_Params {
  const DrtPayload             payload = drt.Params().Params();
  ColorUtils::TO_OUTPUT_Params resolved;
  std::string                  error;
  if (!Resolve(payload, export_encoding.has_value() ? &*export_encoding : nullptr, &resolved,
               &error)) {
    throw std::runtime_error(std::string(caller) + ": " + error);
  }
  return resolved;
}

}  // namespace alcedo
