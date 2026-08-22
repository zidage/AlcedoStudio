//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/graph/raster_mask_node_model.hpp"

#include "edit/operators/models/json_read.hpp"

namespace alcedo {

RasterMaskNodeModel::RasterMaskNodeModel(NodeId id) : id_(std::move(id)) {
  outputs_[0] = PortDescriptor{PortId{"mask"}, PortDataType::Mask, true};
}

auto RasterMaskNodeModel::InputPorts() const -> std::span<const PortDescriptor> { return {}; }

auto RasterMaskNodeModel::OutputPorts() const -> std::span<const PortDescriptor> { return outputs_; }

auto RasterMaskNodeModel::ToJson() const -> nlohmann::json {
  return {{"id", std::string{id_.Value()}},
          {"type", std::string{Type().Text()}},
          {"params",
           {{"asset_key", asset_key_},
            {"reference_bounds",
             nlohmann::json::array({reference_bounds_.x, reference_bounds_.y, reference_bounds_.w,
                                    reference_bounds_.h})},
            {"feather_radius", feather_radius_},
            {"invert", invert_}}}};
}

auto RasterMaskNodeModel::FromJson(const nlohmann::json& json) -> std::unique_ptr<RasterMaskNodeModel> {
  auto node = std::make_unique<RasterMaskNodeModel>(NodeId{json.at("id").get<std::string>()});
  if (!json.contains("params") || !json["params"].is_object()) {
    return node;
  }
  const auto& params = json["params"];
  node->SetAssetKey(json_util::ReadString(params, "asset_key", {}));
  node->SetFeatherRadius(json_util::ReadFloat(params, "feather_radius", 0.0f));
  node->SetInvert(json_util::ReadBool(params, "invert", false));
  if (params.contains("reference_bounds") && params["reference_bounds"].is_array() &&
      params["reference_bounds"].size() >= 4) {
    NormalizedRect bounds;
    bounds.x = params["reference_bounds"][0].get<float>();
    bounds.y = params["reference_bounds"][1].get<float>();
    bounds.w = params["reference_bounds"][2].get<float>();
    bounds.h = params["reference_bounds"][3].get<float>();
    node->SetReferenceBounds(bounds);
  }
  return node;
}

}  // namespace alcedo
