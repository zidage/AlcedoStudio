//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "edit/geometry/types.hpp"
#include "edit/graph/adjustment_ownership.hpp"
#include "edit/graph/i_node_model.hpp"
#ifdef ALCEDO_ENABLE_BRUSH_MASK
#include "edit/mask/brush_mask_commands.hpp"
#endif
#include "edit/mask/mask_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/i_operator_model.hpp"
#include "edit/operators/models/parameter_revision.hpp"

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
  /// Shares adjustment Models and the Mask list with this node (@ref INodeModel::Clone).
  [[nodiscard]] auto Clone() const -> std::shared_ptr<INodeModel> override;

  /**
   * @brief Replace the UI label. Does not change @ref Id.
   * @param name New label. Empty names are rejected by graph commands, not by this setter.
   */
  void SetDisplayName(std::string name);

  /**
   * @brief Catalog Color Grade: CAT02 through LMT in the documented order.
   *
   * Adjustment values are catalog identity (exposure 0 EV, contrast 0, saturation 1.0).
   * Product Default look (+1.5 EV, contrast +15, saturation 1.3) is applied by
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

  /// Persistent deletion-only protection. Caller holds the document owner's access lock.
  [[nodiscard]] auto DeletionProtected() const -> bool { return deletion_protected_; }
  /// Update metadata without invalidating pixels, execution plans, or Mask coverage.
  void SetDeletionProtected(bool value) { deletion_protected_ = value; }
  /// Update one Mask's metadata; throws for a missing ID, without changing content revision.
  void SetMaskDeletionProtected(const MaskId& mask_id, bool value);
  /**
   * @brief Replace one Mask's UI label. Metadata: does not change the content revision.
   * @throws std::runtime_error when @p mask_id is missing. The Mask list is unchanged.
   */
  void SetMaskDisplayName(const MaskId& mask_id, std::string name);

  /**
   * @brief Stamp of the last change to enabled or mix (@ref NextParameterRevision).
   *
   * Display-name edits do not change it. Runtime invalidation compares it with the stamp it
   * saw last; it never clears it.
   */
  [[nodiscard]] auto MixRevision() const -> ParameterRevision { return mix_revision_; }

  /**
   * @brief Copy the mix, adjustment, and Mask content stamps of @p source, a node with equal
   *        values.
   *
   * @pre @p source was serialized into this node (document clone). Adjustments are matched
   *      by instance ID and Masks by @ref MaskId; an ID missing from @p source keeps its own
   *      stamps.
   */
  void               CopyRevisionsFrom(const ColorGradeNodeModel& source);

  /**
   * @brief Stamp of the last content write to the Mask @p mask_id (@ref NextParameterRevision).
   *
   * Every write that can change coverage (source, enabled, opacity, invert, insertion) takes a
   * new process-wide stamp, so two Masks that report the same stamp hold the same content, even
   * across documents. Display-order moves and metadata (display name, deletion protection) do
   * not change it. Not serialized.
   *
   * @return @ref kNoParameterRevision when @p mask_id is absent.
   */
  [[nodiscard]] auto MaskContentRevision(const MaskId& mask_id) const -> ParameterRevision;

  [[nodiscard]] auto Enabled() const -> bool { return enabled_; }
  [[nodiscard]] auto Mix() const -> float { return mix_; }

  [[nodiscard]] auto AdjustmentCount() const -> std::size_t { return adjustments_.size(); }
  [[nodiscard]] auto AdjustmentIdAt(std::size_t index) const -> const AdjustmentInstanceId&;
  /// Non-const Model lookups return a Model that only this node holds; a Model shared with a
  /// frozen document is copied first (@ref MutableAdjustmentModel). Use the const overloads to
  /// read.
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

#ifdef ALCEDO_ENABLE_BRUSH_MASK
  /**
   * @brief Append one canonical stroke to an existing Brush Mask.
   *
   * @param command Exact NodeId, MaskId, StrokeId, sample body, and observed
   *        Mask content revision. The sample body is shared, not copied.
   * @throws std::runtime_error when the target is missing or not a Brush, the
   *         revision does not match, or the stroke is invalid. The Mask list
   *         is left unchanged. Runs on the document-owning thread.
   */
  void AppendBrushStroke(AppendBrushStrokeCommand command);
  /**
   * @brief Remove one stroke by StrokeId. Remaining sample bodies are not copied.
   *
   * @throws std::runtime_error when the target, revision, or StrokeId fail.
   *         The Mask list is left unchanged.
   */
  void RemoveBrushStroke(const RemoveBrushStrokeCommand& command);
  /**
   * @brief Insert a stroke at @p command.index. Values past the end append.
   *
   * @throws std::runtime_error when the target, revision, or stroke fail.
   *         The Mask list is left unchanged.
   */
  void InsertBrushStroke(InsertBrushStrokeCommand command);
  /**
   * @brief Set Brush placement_translation to @p command.after.
   *
   * @p command.before must equal the current translation. Canonical samples are
   * not copied or rewritten. Equal before/after is a no-op and does not bump
   * the Mask content revision.
   *
   * @throws std::runtime_error when the target, revision, or before-value fail.
   *         The Mask list is left unchanged.
   */
  void SetBrushTranslation(const SetBrushTranslationCommand& command);

  /**
   * @brief Ordered strokes of a Brush Mask. Load-only; does not copy sample bodies.
   *
   * @throws std::runtime_error when @p mask_id is missing or the Mask is not a Brush.
   */
  [[nodiscard]] auto BrushStrokes(const MaskId& mask_id) const -> std::span<const BrushStroke>;
  /**
   * @brief Current placement_translation of a Brush Mask. Load-only.
   *
   * @throws std::runtime_error when @p mask_id is missing or the Mask is not a Brush.
   */
  [[nodiscard]] auto BrushPlacementTranslation(const MaskId& mask_id) const -> Vector2;
#endif

  [[nodiscard]] auto MaskCount() const -> std::size_t { return mask_entries_->size(); }
  /// Masks in display order. Valid until the next Mask write on this node (@ref MaskListView).
  [[nodiscard]] auto Masks() const -> MaskListView;
  /**
   * @name Mask reads
   * Masks are read-only outside this node: every write goes through a setter above, which
   * validates the new value and stamps @ref MaskContentRevision. Returned references are valid
   * until the next Mask write on this node.
   * @{
   */
  [[nodiscard]] auto MaskAt(std::size_t index) const -> const MaskModel&;
  [[nodiscard]] auto FindMask(const MaskId& mask_id) const -> const MaskModel*;
  /** @} */

 private:
  /**
   * @brief One Mask and the stamp of its last content write.
   *
   * The Mask value is immutable once stored: a write builds a new value and replaces
   * @ref mask (@ref StoreMask), so frozen documents that share the old value keep it.
   */
  struct MaskEntry {
    std::shared_ptr<const MaskModel> mask;
    ParameterRevision                content_revision = kNoParameterRevision;
  };
  using MaskEntryList = std::vector<MaskEntry>;

  /// Whether a Mask write changes coverage (new content stamp) or only metadata.
  enum class MaskWriteKind : std::uint8_t { Content, Metadata };

  /// Used only by @ref Clone. Copy assignment is deleted.
  ColorGradeNodeModel(const ColorGradeNodeModel& other)            = default;
  ColorGradeNodeModel& operator=(const ColorGradeNodeModel& other) = delete;

  static auto MaskAtEntry(const void* entries, std::size_t index) -> const MaskModel&;
  /// The entry list that only this node holds (@ref UnshareForWrite). Copies pointers and stamps
  /// only; Mask values stay shared.
  auto MutableMaskEntries() -> MaskEntryList&;
  [[nodiscard]] auto FindMaskIndex(const MaskId& mask_id) const -> std::optional<std::size_t>;
  /// @throws std::runtime_error when @p mask_id is missing.
  [[nodiscard]] auto RequireMaskIndex(const MaskId& mask_id) const -> std::size_t;
  /**
   * @brief Validate @p mask and store it as the value of entry @p index.
   *
   * The single write path for an existing Mask. @p mask is a changed copy of the current value
   * with the same @ref MaskId. Validation runs before any change, so a rejected value leaves the
   * list unchanged. @ref MaskWriteKind::Content takes a new content stamp.
   *
   * @throws std::runtime_error when @p mask fails @ref ValidateMaskModel.
   */
  void StoreMask(std::size_t index, MaskModel mask, MaskWriteKind kind);
#ifdef ALCEDO_ENABLE_BRUSH_MASK
  /// Index of the Brush Mask a command targets, after its NodeId, Mask, and revision checks.
  [[nodiscard]] auto RequireBrushMaskIndex(const NodeId& node_id, const MaskId& mask_id,
                                           std::uint64_t expected_revision) const -> std::size_t;
#endif

  NodeId id_;
  std::string display_name_ = "Color Grade";
  std::vector<AdjustmentModelEntry> adjustments_;
  /// Shared with frozen documents; freezing and cloning this node do not copy it.
  std::shared_ptr<const MaskEntryList> mask_entries_;
  bool  enabled_ = true;
  float mix_     = 1.0f;
  ParameterRevision                 mix_revision_       = NextParameterRevision();
  bool  deletion_protected_ = false;
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
