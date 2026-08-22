//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/graph/analytic_mask_node_model.hpp"

#include "edit/operators/models/json_read.hpp"

namespace alcedo {

AnalyticMaskNodeModel::AnalyticMaskNodeModel(NodeId id, AnalyticMaskKind kind)
    : id_(std::move(id)), kind_(kind) {
  outputs_[0] = PortDescriptor{PortId{"mask"}, PortDataType::Mask, true};
}

auto AnalyticMaskNodeModel::InputPorts() const -> std::span<const PortDescriptor> { return {}; }

auto AnalyticMaskNodeModel::OutputPorts() const -> std::span<const PortDescriptor> { return outputs_; }

auto AnalyticMaskNodeModel::ToJson() const -> nlohmann::json {
  nlohmann::json params;
  if (kind_ == AnalyticMaskKind::Radial) {
    params = {{"kind", "radial"},
              {"center_x", radial_.center_x},
              {"center_y", radial_.center_y},
              {"major_radius", radial_.major_radius},
              {"minor_radius", radial_.minor_radius},
              {"rotation", radial_.rotation},
              {"inner_feather", radial_.inner_feather},
              {"outer_feather", radial_.outer_feather},
              {"invert", radial_.invert}};
  } else {
    params = {{"kind", "graduated_nd"},
              {"origin_x", graduated_.origin_x},
              {"origin_y", graduated_.origin_y},
              {"normal_x", graduated_.normal_x},
              {"normal_y", graduated_.normal_y},
              {"transition_distance", graduated_.transition_distance},
              {"start_value", graduated_.start_value},
              {"end_value", graduated_.end_value},
              {"invert", graduated_.invert}};
  }
  return {{"id", std::string{id_.Value()}},
          {"type", std::string{Type().Text()}},
          {"params", std::move(params)}};
}

auto AnalyticMaskNodeModel::FromJson(const nlohmann::json& json)
    -> std::unique_ptr<AnalyticMaskNodeModel> {
  const auto kind_text =
      json.contains("params") ? json_util::ReadString(json["params"], "kind", "radial") : "radial";
  const auto kind =
      kind_text == "graduated_nd" ? AnalyticMaskKind::GraduatedNd : AnalyticMaskKind::Radial;
  auto node = std::make_unique<AnalyticMaskNodeModel>(NodeId{json.at("id").get<std::string>()}, kind);
  if (!json.contains("params") || !json["params"].is_object()) {
    return node;
  }
  const auto& params = json["params"];
  if (kind == AnalyticMaskKind::Radial) {
    RadialMaskParams radial;
    radial.center_x      = json_util::ReadFloat(params, "center_x", radial.center_x);
    radial.center_y      = json_util::ReadFloat(params, "center_y", radial.center_y);
    radial.major_radius  = json_util::ReadFloat(params, "major_radius", radial.major_radius);
    radial.minor_radius  = json_util::ReadFloat(params, "minor_radius", radial.minor_radius);
    radial.rotation      = json_util::ReadFloat(params, "rotation", radial.rotation);
    radial.inner_feather = json_util::ReadFloat(params, "inner_feather", radial.inner_feather);
    radial.outer_feather = json_util::ReadFloat(params, "outer_feather", radial.outer_feather);
    radial.invert        = json_util::ReadBool(params, "invert", radial.invert);
    node->SetRadial(radial);
  } else {
    GraduatedNdMaskParams graduated;
    graduated.origin_x            = json_util::ReadFloat(params, "origin_x", graduated.origin_x);
    graduated.origin_y            = json_util::ReadFloat(params, "origin_y", graduated.origin_y);
    graduated.normal_x            = json_util::ReadFloat(params, "normal_x", graduated.normal_x);
    graduated.normal_y            = json_util::ReadFloat(params, "normal_y", graduated.normal_y);
    graduated.transition_distance =
        json_util::ReadFloat(params, "transition_distance", graduated.transition_distance);
    graduated.start_value = json_util::ReadFloat(params, "start_value", graduated.start_value);
    graduated.end_value   = json_util::ReadFloat(params, "end_value", graduated.end_value);
    graduated.invert      = json_util::ReadBool(params, "invert", graduated.invert);
    node->SetGraduatedNd(graduated);
  }
  return node;
}

}  // namespace alcedo
