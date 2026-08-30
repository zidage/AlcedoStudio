//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "edit/geometry/types.hpp"
#include "edit/graph/i_node_model.hpp"
#include "edit/mask/mask_asset.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"

namespace alcedo {

/**
 * @brief Raster mask node referencing a MaskAssetKey. GPU textures are not stored here.
 */
class RasterMaskNodeModel final : public INodeModel {
 public:
  explicit RasterMaskNodeModel(NodeId id);

  [[nodiscard]] auto Id() const -> const NodeId& override { return id_; }
  [[nodiscard]] auto Type() const -> const OperatorTypeId& override {
    return type_ids::RasterMaskNode();
  }
  [[nodiscard]] auto DisplayName() const -> std::string_view override { return "Mask"; }
  [[nodiscard]] auto InputPorts() const -> std::span<const PortDescriptor> override;
  [[nodiscard]] auto OutputPorts() const -> std::span<const PortDescriptor> override;
  [[nodiscard]] auto ToJson() const -> nlohmann::json override;

  [[nodiscard]] auto AssetKey() const -> const MaskAssetKey& { return asset_key_; }
  [[nodiscard]] auto ReferenceBounds() const -> NormalizedRect { return reference_bounds_; }
  [[nodiscard]] auto FeatherRadius() const -> float { return feather_radius_; }
  [[nodiscard]] auto Invert() const -> bool { return invert_; }

  void               SetAssetKey(MaskAssetKey key) { asset_key_ = std::move(key); }
  void               SetAssetKey(std::string key) { asset_key_ = MaskAssetKey{std::move(key)}; }
  void               SetReferenceBounds(NormalizedRect bounds) { reference_bounds_ = bounds; }
  void               SetFeatherRadius(float radius) { feather_radius_ = radius; }
  void               SetInvert(bool invert) { invert_ = invert; }

  static auto        FromJson(const nlohmann::json& json) -> std::unique_ptr<RasterMaskNodeModel>;

 private:
  NodeId                        id_;
  MaskAssetKey                  asset_key_;
  NormalizedRect                reference_bounds_{};
  float                         feather_radius_ = 0.0f;
  bool                          invert_         = false;
  std::array<PortDescriptor, 1> outputs_;
};

}  // namespace alcedo
