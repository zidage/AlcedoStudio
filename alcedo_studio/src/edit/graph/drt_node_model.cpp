//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/graph/drt_node_model.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "edit/graph/adjustment_ownership.hpp"
#include "edit/operators/models/adjustment_catalog.hpp"
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

auto DetailedEqual(const OpenDrtDetailedParams& lhs, const OpenDrtDetailedParams& rhs) -> bool {
  return std::tie(lhs.tn_con, lhs.tn_sh, lhs.tn_toe, lhs.tn_off, lhs.tn_hcon, lhs.tn_hcon_pv,
                  lhs.tn_hcon_st, lhs.tn_lcon, lhs.tn_lcon_w, lhs.cwp_lm, lhs.rs_sa, lhs.rs_rw,
                  lhs.rs_bw, lhs.pt_lml, lhs.pt_lml_r, lhs.pt_lml_g, lhs.pt_lml_b, lhs.pt_lmh,
                  lhs.pt_lmh_r, lhs.pt_lmh_b, lhs.ptl_c, lhs.ptl_m, lhs.ptl_y, lhs.ptm_low,
                  lhs.ptm_low_rng, lhs.ptm_low_st, lhs.ptm_high, lhs.ptm_high_rng, lhs.ptm_high_st,
                  lhs.brl, lhs.brl_r, lhs.brl_g, lhs.brl_b, lhs.brl_rng, lhs.brl_st, lhs.brlp,
                  lhs.brlp_r, lhs.brlp_g, lhs.brlp_b, lhs.hc_r, lhs.hc_r_rng, lhs.hs_r,
                  lhs.hs_r_rng, lhs.hs_g, lhs.hs_g_rng, lhs.hs_b, lhs.hs_b_rng, lhs.hs_c,
                  lhs.hs_c_rng, lhs.hs_m, lhs.hs_m_rng, lhs.hs_y, lhs.hs_y_rng) ==
         std::tie(rhs.tn_con, rhs.tn_sh, rhs.tn_toe, rhs.tn_off, rhs.tn_hcon, rhs.tn_hcon_pv,
                  rhs.tn_hcon_st, rhs.tn_lcon, rhs.tn_lcon_w, rhs.cwp_lm, rhs.rs_sa, rhs.rs_rw,
                  rhs.rs_bw, rhs.pt_lml, rhs.pt_lml_r, rhs.pt_lml_g, rhs.pt_lml_b, rhs.pt_lmh,
                  rhs.pt_lmh_r, rhs.pt_lmh_b, rhs.ptl_c, rhs.ptl_m, rhs.ptl_y, rhs.ptm_low,
                  rhs.ptm_low_rng, rhs.ptm_low_st, rhs.ptm_high, rhs.ptm_high_rng, rhs.ptm_high_st,
                  rhs.brl, rhs.brl_r, rhs.brl_g, rhs.brl_b, rhs.brl_rng, rhs.brl_st, rhs.brlp,
                  rhs.brlp_r, rhs.brlp_g, rhs.brlp_b, rhs.hc_r, rhs.hc_r_rng, rhs.hs_r,
                  rhs.hs_r_rng, rhs.hs_g, rhs.hs_g_rng, rhs.hs_b, rhs.hs_b_rng, rhs.hs_c,
                  rhs.hs_c_rng, rhs.hs_m, rhs.hs_m_rng, rhs.hs_y, rhs.hs_y_rng);
}

auto DetailedToJson(const OpenDrtDetailedParams& params) -> nlohmann::json {
  return {{"tn_con", params.tn_con},
          {"tn_sh", params.tn_sh},
          {"tn_toe", params.tn_toe},
          {"tn_off", params.tn_off},
          {"tn_hcon", params.tn_hcon},
          {"tn_hcon_pv", params.tn_hcon_pv},
          {"tn_hcon_st", params.tn_hcon_st},
          {"tn_lcon", params.tn_lcon},
          {"tn_lcon_w", params.tn_lcon_w},
          {"cwp_lm", params.cwp_lm},
          {"rs_sa", params.rs_sa},
          {"rs_rw", params.rs_rw},
          {"rs_bw", params.rs_bw},
          {"pt_lml", params.pt_lml},
          {"pt_lml_r", params.pt_lml_r},
          {"pt_lml_g", params.pt_lml_g},
          {"pt_lml_b", params.pt_lml_b},
          {"pt_lmh", params.pt_lmh},
          {"pt_lmh_r", params.pt_lmh_r},
          {"pt_lmh_b", params.pt_lmh_b},
          {"ptl_c", params.ptl_c},
          {"ptl_m", params.ptl_m},
          {"ptl_y", params.ptl_y},
          {"ptm_low", params.ptm_low},
          {"ptm_low_rng", params.ptm_low_rng},
          {"ptm_low_st", params.ptm_low_st},
          {"ptm_high", params.ptm_high},
          {"ptm_high_rng", params.ptm_high_rng},
          {"ptm_high_st", params.ptm_high_st},
          {"brl", params.brl},
          {"brl_r", params.brl_r},
          {"brl_g", params.brl_g},
          {"brl_b", params.brl_b},
          {"brl_rng", params.brl_rng},
          {"brl_st", params.brl_st},
          {"brlp", params.brlp},
          {"brlp_r", params.brlp_r},
          {"brlp_g", params.brlp_g},
          {"brlp_b", params.brlp_b},
          {"hc_r", params.hc_r},
          {"hc_r_rng", params.hc_r_rng},
          {"hs_r", params.hs_r},
          {"hs_r_rng", params.hs_r_rng},
          {"hs_g", params.hs_g},
          {"hs_g_rng", params.hs_g_rng},
          {"hs_b", params.hs_b},
          {"hs_b_rng", params.hs_b_rng},
          {"hs_c", params.hs_c},
          {"hs_c_rng", params.hs_c_rng},
          {"hs_m", params.hs_m},
          {"hs_m_rng", params.hs_m_rng},
          {"hs_y", params.hs_y},
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
    return payload.method == DrtMethod::OpenDrt &&
           payload.encoding_space == DrtColorSpace::Rec709 &&
           payload.encoding_eotf == DrtEotf::Gamma22 && payload.peak_luminance == 100.0f;
  });
}

auto DrtParamsModel::Method() const -> DrtMethod {
  return Read([](const DrtPayload& payload) { return payload.method; });
}

auto DrtParamsModel::EncodingSpace() const -> DrtColorSpace {
  return Read([](const DrtPayload& payload) { return payload.encoding_space; });
}

auto DrtParamsModel::EncodingEotf() const -> DrtEotf {
  return Read([](const DrtPayload& payload) { return payload.encoding_eotf; });
}

auto DrtParamsModel::LimitingSpace() const -> DrtColorSpace {
  return Read([](const DrtPayload& payload) { return payload.limiting_space; });
}

auto DrtParamsModel::PeakLuminance() const -> float {
  return Read([](const DrtPayload& payload) { return payload.peak_luminance; });
}

auto DrtParamsModel::LookPreset() const -> std::string {
  return Read([](const DrtPayload& payload) { return payload.look_preset; });
}

auto DrtParamsModel::TonescalePreset() const -> std::string {
  return Read([](const DrtPayload& payload) { return payload.tonescale_preset; });
}

auto DrtParamsModel::CreativeWhite() const -> std::string {
  return Read([](const DrtPayload& payload) { return payload.creative_white; });
}

auto DrtParamsModel::CreativeWhiteLimit() const -> float {
  return Read([](const DrtPayload& payload) { return payload.creative_white_limit; });
}

auto DrtParamsModel::DisplayGreyLuminance() const -> float {
  return Read([](const DrtPayload& payload) { return payload.display_grey_luminance; });
}

auto DrtParamsModel::HdrGreyBoost() const -> float {
  return Read([](const DrtPayload& payload) { return payload.hdr_grey_boost; });
}

auto DrtParamsModel::HdrPurity() const -> float {
  return Read([](const DrtPayload& payload) { return payload.hdr_purity; });
}

void DrtParamsModel::ApplyUpdate(DrtParameterUpdate update) {
  MutateWithDirtyFields([update = std::move(update)](DrtPayload& payload) mutable {
    DirtyFieldMask changed;
    if (update.method.has_value() && payload.method != *update.method) {
      payload.method = *update.method;
      changed |= DirtyFieldMask{DrtDirty::Method};
    }
    if (update.encoding_space.has_value() && payload.encoding_space != *update.encoding_space) {
      payload.encoding_space = *update.encoding_space;
      changed |= DirtyFieldMask{DrtDirty::Encoding};
    }
    if (update.encoding_eotf.has_value() && payload.encoding_eotf != *update.encoding_eotf) {
      payload.encoding_eotf = *update.encoding_eotf;
      changed |= DirtyFieldMask{DrtDirty::Encoding};
    }
    if (update.limiting_space.has_value() && payload.limiting_space != *update.limiting_space) {
      payload.limiting_space = *update.limiting_space;
      changed |= DirtyFieldMask{DrtDirty::Encoding};
    }
    if (update.peak_luminance.has_value() && payload.peak_luminance != *update.peak_luminance) {
      payload.peak_luminance = *update.peak_luminance;
      changed |= DirtyFieldMask{DrtDirty::Encoding};
    }
    if (update.look_preset.has_value() && payload.look_preset != *update.look_preset) {
      payload.look_preset = std::move(*update.look_preset);
      changed |= DirtyFieldMask{DrtDirty::OpenDrt};
    }
    if (update.tonescale_preset.has_value() &&
        payload.tonescale_preset != *update.tonescale_preset) {
      payload.tonescale_preset = std::move(*update.tonescale_preset);
      changed |= DirtyFieldMask{DrtDirty::OpenDrt};
    }
    if (update.creative_white.has_value() && payload.creative_white != *update.creative_white) {
      payload.creative_white = std::move(*update.creative_white);
      changed |= DirtyFieldMask{DrtDirty::OpenDrt};
    }
    if (update.creative_white_limit.has_value() &&
        payload.creative_white_limit != *update.creative_white_limit) {
      payload.creative_white_limit = *update.creative_white_limit;
      changed |= DirtyFieldMask{DrtDirty::OpenDrt};
    }
    if (update.display_grey_luminance.has_value() &&
        payload.display_grey_luminance != *update.display_grey_luminance) {
      payload.display_grey_luminance = *update.display_grey_luminance;
      changed |= DirtyFieldMask{DrtDirty::OpenDrt};
    }
    if (update.hdr_grey_boost.has_value() && payload.hdr_grey_boost != *update.hdr_grey_boost) {
      payload.hdr_grey_boost = *update.hdr_grey_boost;
      changed |= DirtyFieldMask{DrtDirty::OpenDrt};
    }
    if (update.hdr_purity.has_value() && payload.hdr_purity != *update.hdr_purity) {
      payload.hdr_purity = *update.hdr_purity;
      changed |= DirtyFieldMask{DrtDirty::OpenDrt};
    }
    if (update.parameters.has_value() && !DetailedEqual(payload.parameters, *update.parameters)) {
      payload.parameters = *update.parameters;
      changed |= DirtyFieldMask{DrtDirty::OpenDrt};
    }
    return changed;
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
    payload.method = MethodFromString(json_util::ReadString(json, "method", "open_drt"));
    payload.encoding_space =
        SpaceFromString(json_util::ReadString(json, "encoding_space", "rec709"));
    payload.encoding_eotf =
        EotfFromString(json_util::ReadString(json, "encoding_eotf", "gamma_2_2"));
    payload.limiting_space =
        SpaceFromString(json_util::ReadString(json, "limiting_space", "rec709"));
    payload.peak_luminance = json_util::ReadFloat(json, "peak_luminance", payload.peak_luminance);
    if (json.contains("open_drt") && json["open_drt"].is_object()) {
      const auto& open_drt = json["open_drt"];
      payload.look_preset  = json_util::ReadString(open_drt, "look_preset", payload.look_preset);
      payload.tonescale_preset =
          json_util::ReadString(open_drt, "tonescale_preset", payload.tonescale_preset);
      payload.creative_white =
          json_util::ReadString(open_drt, "creative_white", payload.creative_white);
      payload.creative_white_limit =
          json_util::ReadFloat(open_drt, "creative_white_limit", payload.creative_white_limit);
      payload.display_grey_luminance =
          json_util::ReadFloat(open_drt, "display_grey_luminance", payload.display_grey_luminance);
      payload.hdr_grey_boost =
          json_util::ReadFloat(open_drt, "hdr_grey_boost", payload.hdr_grey_boost);
      payload.hdr_purity = json_util::ReadFloat(open_drt, "hdr_purity", payload.hdr_purity);
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
  std::vector<OperatorTypeId> types;
  types.reserve(adjustments_.size());
  for (const auto& entry : adjustments_) {
    types.push_back(entry.model->Type());
  }
  RequireCompleteDrtPostTypes(types, "DrtNode ToJson");
  nlohmann::json adjustments = nlohmann::json::array();
  for (const auto& entry : adjustments_) {
    adjustments.push_back({{"id", std::string{entry.instance_id.Value()}},
                           {"type", std::string{entry.model->Type().Text()}},
                           {"params", entry.model->ToJson()}});
  }
  return {{"id", std::string{id_.Value()}},
          {"type", std::string{Type().Text()}},
          {"params", params_.ToJson()},
          {"adjustments", std::move(adjustments)}};
}

auto DrtNodeModel::MakeDefault(NodeId id) -> std::unique_ptr<DrtNodeModel> {
  auto        node    = std::make_unique<DrtNodeModel>(std::move(id));
  const auto& catalog = BuiltinAdjustmentCatalog::Instance();
  const auto  types   = DrtPostAdjustmentTypes();
  for (const auto& type : types) {
    auto model = catalog.CreateDefault(type);
    if (model == nullptr) {
      throw std::logic_error("Missing default DRT/Post adjustment factory for " +
                             std::string{type.Text()});
    }
    node->InsertAdjustment(node->AdjustmentCount(), MakeAdjustmentInstanceId(node->Id(), type),
                           std::move(model));
  }
  return node;
}

auto DrtNodeModel::FromJson(const nlohmann::json& json) -> std::unique_ptr<DrtNodeModel> {
  auto        node    = std::make_unique<DrtNodeModel>(NodeId{json.at("id").get<std::string>()});
  const auto& catalog = BuiltinAdjustmentCatalog::Instance();
  if (!json.contains("params") || !json["params"].is_object()) {
    throw std::runtime_error("DrtNode FromJson: missing params object");
  }
  node->params_.LoadJson(json["params"]);
  if (!json.contains("adjustments") || !json["adjustments"].is_array()) {
    throw std::runtime_error("DrtNode FromJson: missing adjustments array");
  }
  for (const auto& item : json["adjustments"]) {
    const auto type_text = item.at("type").get<std::string>();
    auto       model     = catalog.CreateDefault(OperatorTypeId{type_text});
    if (model == nullptr) {
      throw std::runtime_error("Unknown DRT/Post adjustment type: " + type_text);
    }
    if (item.contains("params") && item["params"].is_object()) {
      model->LoadJson(item["params"]);
    }
    node->InsertAdjustment(node->AdjustmentCount(),
                           AdjustmentInstanceId{item.at("id").get<std::string>()},
                           std::move(model));
  }
  std::vector<OperatorTypeId> types;
  types.reserve(node->AdjustmentCount());
  for (std::size_t index = 0; index < node->AdjustmentCount(); ++index) {
    types.push_back(node->AdjustmentAt(index).Type());
  }
  RequireCompleteDrtPostTypes(types, "DrtNode FromJson");
  return node;
}

auto DrtNodeModel::AdjustmentIdAt(std::size_t index) const -> const AdjustmentInstanceId& {
  return adjustments_.at(index).instance_id;
}

auto DrtNodeModel::AdjustmentAt(std::size_t index) -> IOperatorModel& {
  return *adjustments_.at(index).model;
}

auto DrtNodeModel::AdjustmentAt(std::size_t index) const -> const IOperatorModel& {
  return *adjustments_.at(index).model;
}

auto DrtNodeModel::FindAdjustment(const AdjustmentInstanceId& id) -> IOperatorModel* {
  for (auto& entry : adjustments_) {
    if (entry.instance_id == id) {
      return entry.model.get();
    }
  }
  return nullptr;
}

auto DrtNodeModel::FindAdjustment(const AdjustmentInstanceId& id) const -> const IOperatorModel* {
  for (const auto& entry : adjustments_) {
    if (entry.instance_id == id) {
      return entry.model.get();
    }
  }
  return nullptr;
}

auto DrtNodeModel::FindAdjustmentByType(const OperatorTypeId& type) -> IOperatorModel* {
  for (auto& entry : adjustments_) {
    if (entry.model->Type() == type) {
      return entry.model.get();
    }
  }
  return nullptr;
}

auto DrtNodeModel::FindAdjustmentByType(const OperatorTypeId& type) const -> const IOperatorModel* {
  for (const auto& entry : adjustments_) {
    if (entry.model->Type() == type) {
      return entry.model.get();
    }
  }
  return nullptr;
}

auto DrtNodeModel::FindAdjustmentIdByType(const OperatorTypeId& type) const
    -> const AdjustmentInstanceId* {
  for (const auto& entry : adjustments_) {
    if (entry.model->Type() == type) {
      return &entry.instance_id;
    }
  }
  return nullptr;
}

void DrtNodeModel::InsertAdjustment(std::size_t index, AdjustmentInstanceId id,
                                    std::unique_ptr<IOperatorModel> model) {
  if (model == nullptr) {
    throw std::invalid_argument("DrtNode InsertAdjustment requires a Model");
  }
  RequireAdjustmentOwner(model->Type(), AdjustmentParameterOwner::DrtPost,
                         "DrtNode InsertAdjustment");
  if (index > adjustments_.size()) {
    index = adjustments_.size();
  }
  AdjustmentModelEntry entry;
  entry.instance_id = std::move(id);
  entry.model       = std::move(model);
  adjustments_.insert(adjustments_.begin() + static_cast<std::ptrdiff_t>(index), std::move(entry));
}

}  // namespace alcedo
