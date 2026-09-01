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
#include "edit/mask/mask_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/i_operator_model.hpp"

namespace alcedo {

/**
 * @brief Color grade node with an ordered adjustment list, mix, and Grade-owned Masks.
 *
 * Adjustment list order is user data. GraphCompiler must not reorder Models.
 * Mask list order is display order only. Clarity, Sharpen, Halation, and Film Grain
 * are DRT/Post-owned and are rejected here. This node has a scene-image input only.
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

  /**
   * @brief Insert a validated Mask at @p index. Display order only; not pixel order.
   *
   * @param mask Owned Mask value. @ref MaskId must be unique in this Grade.
   * @param index Insertion index. Values past the end append.
   * @throws std::runtime_error when identity, values, or duplicate @ref MaskId fail.
   *         The Mask list is left unchanged.
   */
  void AddMask(MaskModel mask, std::size_t index);
  /**
   * @brief Remove the Mask with @p mask_id.
   * @throws std::runtime_error when @p mask_id is missing. The Mask list is unchanged.
   */
  void RemoveMask(const MaskId& mask_id);
  /**
   * @brief Replace the source variant of an existing Mask.
   * @throws std::runtime_error when @p mask_id is missing or @p source is invalid.
   *         The Mask list is unchanged.
   */
  void ReplaceMaskSource(const MaskId& mask_id, MaskSource source);
  /**
   * @brief Set enabled. Does not change @ref MaskId or display order.
   * @throws std::runtime_error when @p mask_id is missing. The Mask list is unchanged.
   */
  void SetMaskEnabled(const MaskId& mask_id, bool enabled);
  /**
   * @brief Set opacity in `[0, 1]`.
   * @throws std::runtime_error when @p mask_id is missing or @p opacity is invalid.
   *         The Mask list is unchanged.
   */
  void SetMaskOpacity(const MaskId& mask_id, float opacity);
  /**
   * @brief Set invert. Applied after source feather and before range fields.
   * @throws std::runtime_error when @p mask_id is missing. The Mask list is unchanged.
   */
  void SetMaskInvert(const MaskId& mask_id, bool invert);
  /**
   * @brief Move a Mask in display order. Does not change pixel identity.
   * @throws std::runtime_error when @p mask_id is missing. The Mask list is unchanged.
   */
  void MoveMaskForDisplay(const MaskId& mask_id, std::size_t index);

  [[nodiscard]] auto MaskCount() const -> std::size_t { return masks_.size(); }
  [[nodiscard]] auto Masks() const -> std::span<const MaskModel> { return masks_; }
  [[nodiscard]] auto MaskAt(std::size_t index) -> MaskModel&;
  [[nodiscard]] auto MaskAt(std::size_t index) const -> const MaskModel&;
  [[nodiscard]] auto FindMask(const MaskId& mask_id) -> MaskModel*;
  [[nodiscard]] auto FindMask(const MaskId& mask_id) const -> const MaskModel*;

 private:
  NodeId id_;
  std::string display_name_ = "Color Grade";
  std::vector<AdjustmentModelEntry> adjustments_;
  std::vector<MaskModel>            masks_;
  bool  enabled_ = true;
  float mix_     = 1.0f;
  std::array<PortDescriptor, 1> inputs_;
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
