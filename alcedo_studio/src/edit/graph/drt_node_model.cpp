//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/graph/drt_node_model.hpp"

#include <string_view>

#include "edit/operators/models/json_read.hpp"

namespace alcedo {

namespace {

auto MethodToString(DrtMethod method) -> const char* {
  return method == DrtMethod::Aces20 ? "aces_2_0" : "open_drt";
}

auto MethodFromString(std::string_view text) -> DrtMethod {
  return text == "aces_2_0" ? DrtMethod::Aces20 : DrtMethod::OpenDrt;
}

auto SpaceToString(DrtColorSpace space) -> const char* {
  switch (space) {
    case DrtColorSpace::Rec2020:
      return "rec2020";
    case DrtColorSpace::P3D65:
      return "p3_d65";
    case DrtColorSpace::Rec709:
    default:
      return "rec709";
  }
}

auto SpaceFromString(std::string_view text) -> DrtColorSpace {
  if (text == "rec2020") {
    return DrtColorSpace::Rec2020;
  }
  if (text == "p3_d65") {
    return DrtColorSpace::P3D65;
  }
  return DrtColorSpace::Rec709;
}

auto EotfToString(DrtEotf eotf) -> const char* {
  switch (eotf) {
    case DrtEotf::Linear:
      return "linear";
    case DrtEotf::St2084:
      return "st2084";
    case DrtEotf::Hlg:
      return "hlg";
    case DrtEotf::Gamma26:
      return "gamma_2_6";
    case DrtEotf::Bt1886:
      return "bt1886";
    case DrtEotf::Gamma18:
      return "gamma_1_8";
    case DrtEotf::Gamma22:
    default:
      return "gamma_2_2";
  }
}

auto EotfFromString(std::string_view text) -> DrtEotf {
  if (text == "linear") {
    return DrtEotf::Linear;
  }
  if (text == "st2084") {
    return DrtEotf::St2084;
  }
  if (text == "hlg") {
    return DrtEotf::Hlg;
  }
  if (text == "gamma_2_6") {
    return DrtEotf::Gamma26;
  }
  if (text == "bt1886") {
    return DrtEotf::Bt1886;
  }
  if (text == "gamma_1_8") {
    return DrtEotf::Gamma18;
  }
  return DrtEotf::Gamma22;
}

auto DetailedToJson(const OpenDrtDetailedParams& params) -> nlohmann::json {
  return {{"tn_con", params.tn_con},       {"tn_sh", params.tn_sh},
          {"tn_toe", params.tn_toe},       {"tn_off", params.tn_off},
          {"tn_hcon", params.tn_hcon},     {"tn_hcon_pv", params.tn_hcon_pv},
          {"tn_hcon_st", params.tn_hcon_st}, {"tn_lcon", params.tn_lcon},
          {"tn_lcon_w", params.tn_lcon_w}, {"cwp_lm", params.cwp_lm},
          {"rs_sa", params.rs_sa},         {"rs_rw", params.rs_rw},
          {"rs_bw", params.rs_bw},         {"pt_lml", params.pt_lml},
          {"pt_lml_r", params.pt_lml_r},   {"pt_lml_g", params.pt_lml_g},
          {"pt_lml_b", params.pt_lml_b},   {"pt_lmh", params.pt_lmh},
          {"pt_lmh_r", params.pt_lmh_r},   {"pt_lmh_b", params.pt_lmh_b},
          {"ptl_c", params.ptl_c},         {"ptl_m", params.ptl_m},
          {"ptl_y", params.ptl_y},         {"ptm_low", params.ptm_low},
          {"ptm_low_rng", params.ptm_low_rng}, {"ptm_low_st", params.ptm_low_st},
          {"ptm_high", params.ptm_high},   {"ptm_high_rng", params.ptm_high_rng},
          {"ptm_high_st", params.ptm_high_st}, {"brl", params.brl},
          {"brl_r", params.brl_r},         {"brl_g", params.brl_g},
          {"brl_b", params.brl_b},         {"brl_rng", params.brl_rng},
          {"brl_st", params.brl_st},       {"brlp", params.brlp},
          {"brlp_r", params.brlp_r},       {"brlp_g", params.brlp_g},
          {"brlp_b", params.brlp_b},       {"hc_r", params.hc_r},
          {"hc_r_rng", params.hc_r_rng},   {"hs_r", params.hs_r},
          {"hs_r_rng", params.hs_r_rng},   {"hs_g", params.hs_g},
          {"hs_g_rng", params.hs_g_rng},   {"hs_b", params.hs_b},
          {"hs_b_rng", params.hs_b_rng},   {"hs_c", params.hs_c},
          {"hs_c_rng", params.hs_c_rng},   {"hs_m", params.hs_m},
          {"hs_m_rng", params.hs_m_rng},   {"hs_y", params.hs_y},
          {"hs_y_rng", params.hs_y_rng}};
}

void DetailedFromJson(const nlohmann::json& json, OpenDrtDetailedParams& params) {
  if (!json.is_object()) {
    return;
  }
#define ALCEDO_READ_ODRT(field) params.field = json_util::ReadFloat(json, #field, params.field)
  ALCEDO_READ_ODRT(tn_con);
  ALCEDO_READ_ODRT(tn_sh);
  ALCEDO_READ_ODRT(tn_toe);
  ALCEDO_READ_ODRT(tn_off);
  ALCEDO_READ_ODRT(tn_hcon);
  ALCEDO_READ_ODRT(tn_hcon_pv);
  ALCEDO_READ_ODRT(tn_hcon_st);
  ALCEDO_READ_ODRT(tn_lcon);
  ALCEDO_READ_ODRT(tn_lcon_w);
  ALCEDO_READ_ODRT(cwp_lm);
  ALCEDO_READ_ODRT(rs_sa);
  ALCEDO_READ_ODRT(rs_rw);
  ALCEDO_READ_ODRT(rs_bw);
  ALCEDO_READ_ODRT(pt_lml);
  ALCEDO_READ_ODRT(pt_lml_r);
  ALCEDO_READ_ODRT(pt_lml_g);
  ALCEDO_READ_ODRT(pt_lml_b);
  ALCEDO_READ_ODRT(pt_lmh);
  ALCEDO_READ_ODRT(pt_lmh_r);
  ALCEDO_READ_ODRT(pt_lmh_b);
  ALCEDO_READ_ODRT(ptl_c);
  ALCEDO_READ_ODRT(ptl_m);
  ALCEDO_READ_ODRT(ptl_y);
  ALCEDO_READ_ODRT(ptm_low);
  ALCEDO_READ_ODRT(ptm_low_rng);
  ALCEDO_READ_ODRT(ptm_low_st);
  ALCEDO_READ_ODRT(ptm_high);
  ALCEDO_READ_ODRT(ptm_high_rng);
  ALCEDO_READ_ODRT(ptm_high_st);
  ALCEDO_READ_ODRT(brl);
  ALCEDO_READ_ODRT(brl_r);
  ALCEDO_READ_ODRT(brl_g);
  ALCEDO_READ_ODRT(brl_b);
  ALCEDO_READ_ODRT(brl_rng);
  ALCEDO_READ_ODRT(brl_st);
  ALCEDO_READ_ODRT(brlp);
  ALCEDO_READ_ODRT(brlp_r);
  ALCEDO_READ_ODRT(brlp_g);
  ALCEDO_READ_ODRT(brlp_b);
  ALCEDO_READ_ODRT(hc_r);
  ALCEDO_READ_ODRT(hc_r_rng);
  ALCEDO_READ_ODRT(hs_r);
  ALCEDO_READ_ODRT(hs_r_rng);
  ALCEDO_READ_ODRT(hs_g);
  ALCEDO_READ_ODRT(hs_g_rng);
  ALCEDO_READ_ODRT(hs_b);
  ALCEDO_READ_ODRT(hs_b_rng);
  ALCEDO_READ_ODRT(hs_c);
  ALCEDO_READ_ODRT(hs_c_rng);
  ALCEDO_READ_ODRT(hs_m);
  ALCEDO_READ_ODRT(hs_m_rng);
  ALCEDO_READ_ODRT(hs_y);
  ALCEDO_READ_ODRT(hs_y_rng);
#undef ALCEDO_READ_ODRT
}

}  // namespace

auto DrtParamsModel::IsDefault() const -> bool {
  return Read([](const DrtPayload& payload) {
    return payload.method == DrtMethod::OpenDrt && payload.encoding_space == DrtColorSpace::Rec709 &&
           payload.encoding_eotf == DrtEotf::Gamma22 && payload.peak_luminance == 100.0f;
  });
}

auto DrtParamsModel::ToJson() const -> nlohmann::json {
  const auto payload = PayloadCopy();
  return {{"method", MethodToString(payload.method)},
          {"encoding_space", SpaceToString(payload.encoding_space)},
          {"encoding_eotf", EotfToString(payload.encoding_eotf)},
          {"limiting_space", SpaceToString(payload.limiting_space)},
          {"peak_luminance", payload.peak_luminance},
          {"open_drt",
           {{"look_preset", payload.look_preset},
            {"tonescale_preset", payload.tonescale_preset},
            {"creative_white", payload.creative_white},
            {"creative_white_limit", payload.creative_white_limit},
            {"display_grey_luminance", payload.display_grey_luminance},
            {"hdr_grey_boost", payload.hdr_grey_boost},
            {"hdr_purity", payload.hdr_purity},
            {"parameters", DetailedToJson(payload.parameters)}}}};
}

void DrtParamsModel::LoadJson(const nlohmann::json& json) {
  Mutate(DrtDirty::All, [&json](DrtPayload& payload) {
    payload.method         = MethodFromString(json_util::ReadString(json, "method", "open_drt"));
    payload.encoding_space = SpaceFromString(json_util::ReadString(json, "encoding_space", "rec709"));
    payload.encoding_eotf  = EotfFromString(json_util::ReadString(json, "encoding_eotf", "gamma_2_2"));
    payload.limiting_space = SpaceFromString(json_util::ReadString(json, "limiting_space", "rec709"));
    payload.peak_luminance = json_util::ReadFloat(json, "peak_luminance", payload.peak_luminance);
    if (json.contains("open_drt") && json["open_drt"].is_object()) {
      const auto& open_drt       = json["open_drt"];
      payload.look_preset        = json_util::ReadString(open_drt, "look_preset", payload.look_preset);
      payload.tonescale_preset   = json_util::ReadString(open_drt, "tonescale_preset", payload.tonescale_preset);
      payload.creative_white     = json_util::ReadString(open_drt, "creative_white", payload.creative_white);
      payload.creative_white_limit =
          json_util::ReadFloat(open_drt, "creative_white_limit", payload.creative_white_limit);
      payload.display_grey_luminance =
          json_util::ReadFloat(open_drt, "display_grey_luminance", payload.display_grey_luminance);
      payload.hdr_grey_boost = json_util::ReadFloat(open_drt, "hdr_grey_boost", payload.hdr_grey_boost);
      payload.hdr_purity     = json_util::ReadFloat(open_drt, "hdr_purity", payload.hdr_purity);
      if (open_drt.contains("parameters")) {
        DetailedFromJson(open_drt["parameters"], payload.parameters);
      }
    }
  });
}

void DrtParamsModel::ReplaceParams(DrtPayload payload) {
  Mutate(DrtDirty::All,
         [payload = std::move(payload)](DrtPayload& dest) mutable { dest = std::move(payload); });
}

DrtNodeModel::DrtNodeModel(NodeId id) : id_(std::move(id)) {
  inputs_[0]  = PortDescriptor{PortId{"image"}, PortDataType::SceneImage, true};
  outputs_[0] = PortDescriptor{PortId{"display"}, PortDataType::DisplayImage, true};
}

auto DrtNodeModel::InputPorts() const -> std::span<const PortDescriptor> { return inputs_; }

auto DrtNodeModel::OutputPorts() const -> std::span<const PortDescriptor> { return outputs_; }

auto DrtNodeModel::ToJson() const -> nlohmann::json {
  return {{"id", std::string{id_.Value()}},
          {"type", std::string{Type().Text()}},
          {"params", params_.ToJson()}};
}

auto DrtNodeModel::FromJson(const nlohmann::json& json) -> std::unique_ptr<DrtNodeModel> {
  auto node = std::make_unique<DrtNodeModel>(NodeId{json.at("id").get<std::string>()});
  if (json.contains("params") && json["params"].is_object()) {
    node->params_.LoadJson(json["params"]);
  }
  return node;
}

}  // namespace alcedo
