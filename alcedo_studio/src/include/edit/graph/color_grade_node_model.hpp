//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

#include "edit/graph/i_node_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/i_operator_model.hpp"

namespace alcedo {

struct AdjustmentModelEntry {
  AdjustmentInstanceId           instance_id;
  std::unique_ptr<IOperatorModel> model;
};

/**
 * @brief Color grade node with an ordered adjustment list, mix, and optional mask input.
 *
 * Adjustment list order is user data. GraphCompiler must not reorder Models.
 */
class ColorGradeNodeModel final : public INodeModel {
 public:
  explicit ColorGradeNodeModel(NodeId id);

  [[nodiscard]] auto Id() const -> const NodeId& override { return id_; }
  [[nodiscard]] auto Type() const -> const OperatorTypeId& override {
    return type_ids::ColorGradeNode();
  }
  [[nodiscard]] auto InputPorts() const -> std::span<const PortDescriptor> override;
  [[nodiscard]] auto OutputPorts() const -> std::span<const PortDescriptor> override;
  [[nodiscard]] auto ToJson() const -> nlohmann::json override;

  /**
   * @brief Default primary grade: CAT02 through film grain in the documented order.
   * @param id Node id, typically "grade.primary".
   */
  static auto MakeDefault(NodeId id) -> std::unique_ptr<ColorGradeNodeModel>;
  static auto FromJson(const nlohmann::json& json) -> std::unique_ptr<ColorGradeNodeModel>;

  void SetEnabled(bool enabled);
  void SetMix(float mix);

  [[nodiscard]] auto Enabled() const -> bool { return enabled_; }
  [[nodiscard]] auto Mix() const -> float { return mix_; }

  [[nodiscard]] auto AdjustmentCount() const -> std::size_t { return adjustments_.size(); }
  [[nodiscard]] auto AdjustmentIdAt(std::size_t index) const -> const AdjustmentInstanceId&;
  [[nodiscard]] auto AdjustmentAt(std::size_t index) -> IOperatorModel&;
  [[nodiscard]] auto AdjustmentAt(std::size_t index) const -> const IOperatorModel&;
  [[nodiscard]] auto FindAdjustment(const AdjustmentInstanceId& id) -> IOperatorModel*;
  [[nodiscard]] auto FindAdjustmentByType(const OperatorTypeId& type) -> IOperatorModel*;
  [[nodiscard]] auto FindAdjustmentByType(const OperatorTypeId& type) const -> const IOperatorModel*;

  /**
   * @brief Insert an adjustment instance. Caller must mark topology_dirty on the document.
   */
  void InsertAdjustment(std::size_t index, AdjustmentInstanceId id,
                        std::unique_ptr<IOperatorModel> model);
  void RemoveAdjustment(const AdjustmentInstanceId& id);
  void MoveAdjustment(const AdjustmentInstanceId& id, std::size_t index);

 private:
  NodeId id_;
  std::vector<AdjustmentModelEntry> adjustments_;
  bool  enabled_ = true;
  float mix_     = 1.0f;
  std::array<PortDescriptor, 2> inputs_;
  std::array<PortDescriptor, 1> outputs_;
};

}  // namespace alcedo
