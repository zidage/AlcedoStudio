//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "edit/graph/adjustment_ownership.hpp"
#include "edit/graph/i_node_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/i_operator_model.hpp"

namespace alcedo {

/**
 * @brief Color grade node with an ordered adjustment list, mix, and optional mask input.
 *
 * Adjustment list order is user data. GraphCompiler must not reorder Models.
 * Clarity, Sharpen, Halation, and Film Grain are DRT/Post-owned and are rejected here.
 */
class ColorGradeNodeModel final : public INodeModel {
 public:
  explicit ColorGradeNodeModel(NodeId id);

  [[nodiscard]] auto Id() const -> const NodeId& override { return id_; }
  [[nodiscard]] auto Type() const -> const OperatorTypeId& override {
    return type_ids::ColorGradeNode();
  }
  [[nodiscard]] auto DisplayName() const -> std::string_view override { return display_name_; }
  [[nodiscard]] auto InputPorts() const -> std::span<const PortDescriptor> override;
  [[nodiscard]] auto OutputPorts() const -> std::span<const PortDescriptor> override;
  [[nodiscard]] auto ToJson() const -> nlohmann::json override;

  /**
   * @brief Replace the UI label. Does not change @ref Id.
   * @param name New label. Empty names are rejected by graph commands, not by this setter.
   */
  void SetDisplayName(std::string name);

  /**
   * @brief Catalog Color Grade: CAT02 through LMT in the documented order.
   *
   * Adjustment values are catalog identity (exposure 0 EV, saturation 1.0). Product
   * Default look (+1.5 EV, saturation 1.3) is applied by
   * @ref CreateDefaultPipelineDocument, not by this factory. Clarity, Sharpen,
   * Halation, and Film Grain belong to DRT/Post and are omitted.
   *
   * @param id Node id, typically "grade.primary" for the Default document.
   */
  static auto MakeDefault(NodeId id) -> std::unique_ptr<ColorGradeNodeModel>;

  /**
   * @brief Identity Color Grade: the same 13 catalog types as @ref MakeDefault.
   *
   * Exposure is 0 EV, saturation is 1.0, mix is 1, enabled is true. Does not copy
   * or patch @ref MakeDefault. Product look stays on @ref CreateDefaultPipelineDocument.
   *
   * @param id Stable NodeId for the new node.
   * @return Owned node. Caller inserts it into a graph.
   */
  static auto MakeClean(NodeId id) -> std::unique_ptr<ColorGradeNodeModel>;

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
  [[nodiscard]] auto FindAdjustment(const AdjustmentInstanceId& id) const -> const IOperatorModel*;
  [[nodiscard]] auto FindAdjustmentByType(const OperatorTypeId& type) -> IOperatorModel*;
  [[nodiscard]] auto FindAdjustmentByType(const OperatorTypeId& type) const -> const IOperatorModel*;
  [[nodiscard]] auto FindAdjustmentIdByType(const OperatorTypeId& type) const
      -> const AdjustmentInstanceId*;

  /**
   * @brief Insert a Color Grade-owned adjustment. Caller must mark topology_dirty.
   *
   * @throws std::runtime_error when @p model is DRT/Post-owned or unsupported.
   */
  void InsertAdjustment(std::size_t index, AdjustmentInstanceId id,
                        std::unique_ptr<IOperatorModel> model);
  void RemoveAdjustment(const AdjustmentInstanceId& id);
  void MoveAdjustment(const AdjustmentInstanceId& id, std::size_t index);

 private:
  NodeId id_;
  std::string display_name_ = "Color Grade";
  std::vector<AdjustmentModelEntry> adjustments_;
  bool  enabled_ = true;
  float mix_     = 1.0f;
  std::array<PortDescriptor, 2> inputs_;
  std::array<PortDescriptor, 1> outputs_;
};

/**
 * @brief Identity Color Grade node for @ref AddCleanColorGrade.
 *
 * Equivalent to @ref ColorGradeNodeModel::MakeClean. Distinct from the product
 * Default look on @ref CreateDefaultPipelineDocument.
 *
 * @param id Stable NodeId.
 * @return Owned node with identity params and the 13 Color Grade catalog types.
 *         The caller owns insertion into a graph.
 */
[[nodiscard]] auto CreateCleanColorGradeNode(NodeId id) -> std::unique_ptr<ColorGradeNodeModel>;

}  // namespace alcedo
