//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "edit/graph/i_node_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/operator_model_base.hpp"

namespace alcedo {

struct DevelopPayload {
  std::string demosaic_method          = "default";
  bool        highlights_reconstruct   = true;
  bool        use_camera_wb            = true;
  float       user_wb                  = 7600.0f;
  std::string wb_mode                  = "as_shot";
  float       custom_cct               = 6500.0f;
  float       custom_tint              = 0.0f;
  float       as_shot_cct              = 6500.0f;
  float       as_shot_tint             = 0.0f;
  bool        lens_enabled             = false;
  bool        apply_vignetting         = true;
  bool        apply_distortion         = true;
  bool        apply_tca                = true;
  bool        apply_crop               = true;
  bool        auto_scale               = true;
  bool        use_user_scale           = false;
  float       user_scale               = 1.0f;
  bool        projection_enabled       = false;
  std::string target_projection        = "unknown";
  std::string lens_profile_db_path     = "src/config/lens_calib";
};

enum class DevelopDirty : std::uint32_t {
  None        = 0,
  Demosaic    = 1U << 0,
  Highlights  = 1U << 1,
  WhiteBalance = 1U << 2,
  Lens        = 1U << 3,
  All         = Demosaic | Highlights | WhiteBalance | Lens,
};

/**
 * @brief Develop endpoint parameters. LibRaw unpack stays outside the graph.
 */
class DevelopParamsModel final
    : public OperatorModelBase<DevelopParamsModel, DevelopPayload, DevelopDirty> {
 public:
  static auto TypeId() -> const OperatorTypeId& { return type_ids::DevelopNode(); }

  [[nodiscard]] auto IsDefault() const -> bool override;
  [[nodiscard]] auto ToJson() const -> nlohmann::json override;
  void               LoadJson(const nlohmann::json& json) override;

  [[nodiscard]] auto Params() const -> DevelopPayload { return PayloadCopy(); }
  void               ReplaceParams(DevelopPayload payload);
};

/**
 * @brief Develop root node. No image input; one scene-image output.
 */
class DevelopNodeModel final : public INodeModel {
 public:
  explicit DevelopNodeModel(NodeId id);

  [[nodiscard]] auto Id() const -> const NodeId& override { return id_; }
  [[nodiscard]] auto Type() const -> const OperatorTypeId& override { return type_ids::DevelopNode(); }
  [[nodiscard]] auto InputPorts() const -> std::span<const PortDescriptor> override;
  [[nodiscard]] auto OutputPorts() const -> std::span<const PortDescriptor> override;
  [[nodiscard]] auto ToJson() const -> nlohmann::json override;

  [[nodiscard]] auto Params() -> DevelopParamsModel& { return params_; }
  [[nodiscard]] auto Params() const -> const DevelopParamsModel& { return params_; }

  static auto FromJson(const nlohmann::json& json) -> std::unique_ptr<DevelopNodeModel>;

 private:
  NodeId             id_;
  DevelopParamsModel params_;
  std::array<PortDescriptor, 1> outputs_;
};

}  // namespace alcedo
