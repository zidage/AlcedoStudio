//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "app/editor_adjustment_types.hpp"
#include "app/editor_render_intent.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/mask/mask_id.hpp"
#include "json.hpp"

namespace alcedo {

class MaskStore;

/**
 * @brief Convert a complete editor parameter target into a stored typed target.
 *
 * @throws std::runtime_error when @p target is unspecified or incomplete.
 */
[[nodiscard]] auto ToPipelineParameterTarget(const EditorParameterTarget& target)
    -> PipelineParameterTarget;

/**
 * @brief Convert a stored typed target into an editor parameter target.
 */
[[nodiscard]] auto ToEditorParameterTarget(const PipelineParameterTarget& target)
    -> EditorParameterTarget;

/**
 * @brief Convert a stored scene edge into a graph edge.
 */
[[nodiscard]] auto ToGraphEdge(const PipelineSceneEdge& edge) -> GraphEdge;

/**
 * @brief Convert a graph edge into a stored scene edge.
 */
[[nodiscard]] auto ToPipelineSceneEdge(const GraphEdge& edge) -> PipelineSceneEdge;

/**
 * @brief Localization key for @p kind. Stable; not user-visible text.
 */
[[nodiscard]] auto PresentationKeyForOperation(PipelineEditOperationKind kind) -> std::string;

/**
 * @brief Render reason for a successful commit of @p batch.
 *
 * Rename-only batches return nullopt (no render). Graph topology and Mask
 * edits return their dedicated Quality reasons. Parameter, enabled, and mix
 * edits return @ref EditorRenderReason::SettledAdjustment. Paste returns
 * @ref EditorRenderReason::PastedPipelineDocument.
 */
[[nodiscard]] auto RenderReasonForBatch(const PipelineEditBatch& batch)
    -> std::optional<EditorRenderReason>;

/**
 * @brief Render reason after Undo/Redo across @p commits.
 *
 * Returns nullopt when every traversed typed commit is rename-only. Ordinary
 * or pixel-changing typed commits return @ref EditorRenderReason::UndoRedo.
 */
[[nodiscard]] auto RenderReasonForHeadMove(const std::vector<EditCommit>& commits)
    -> std::optional<EditorRenderReason>;

/**
 * @brief Build a validated SetParameter batch from captured model JSON.
 */
[[nodiscard]] auto MakeSetParameterBatch(const EditorParameterTarget& target,
                                         nlohmann::json before_value, nlohmann::json after_value,
                                         bool before_enabled, bool after_enabled,
                                         std::string node_display_name) -> PipelineEditBatch;

/**
 * @brief Capture a clean Color Grade insertion at @p before_node_id as a typed change.
 *
 * Creates the node JSON with @p new_id before any graph mutation. Apply inserts
 * that exact node.
 */
[[nodiscard]] auto CaptureAddColorGradeChange(const PipelineDocument& document,
                                              const NodeId& before_node_id, const NodeId& new_id)
    -> AddColorGradeChange;

[[nodiscard]] auto MakeAddColorGradeBatch(AddColorGradeChange change) -> PipelineEditBatch;

[[nodiscard]] auto CaptureRemoveColorGradeChange(const PipelineDocument& document,
                                                 const NodeId& node_id) -> RemoveColorGradeChange;

[[nodiscard]] auto MakeRemoveColorGradeBatch(RemoveColorGradeChange change) -> PipelineEditBatch;

[[nodiscard]] auto CaptureReconnectColorGradeChange(const PipelineDocument& document,
                                                    const NodeId& node_id,
                                                    const NodeId& new_predecessor_id,
                                                    const NodeId& new_successor_id)
    -> ReconnectColorGradeChange;

[[nodiscard]] auto MakeReconnectColorGradeBatch(ReconnectColorGradeChange change)
    -> PipelineEditBatch;

[[nodiscard]] auto MakeRenameColorGradeBatch(const NodeId& node_id, std::string before_name,
                                             std::string after_name) -> PipelineEditBatch;

[[nodiscard]] auto MakeSetNodeEnabledBatch(const NodeId& node_id, PipelineEditNodeKind node_kind,
                                           bool before_enabled, bool after_enabled)
    -> PipelineEditBatch;

[[nodiscard]] auto MakeSetNodeMixBatch(const NodeId& node_id, float before_mix, float after_mix)
    -> PipelineEditBatch;

[[nodiscard]] auto MakeAddMaskBatch(const NodeId& node_id, MaskId mask_id, nlohmann::json mask,
                                    std::uint32_t display_index) -> PipelineEditBatch;

[[nodiscard]] auto MakeRemoveMaskBatch(const NodeId& node_id, MaskId mask_id, nlohmann::json mask,
                                       std::uint32_t display_index) -> PipelineEditBatch;

[[nodiscard]] auto MakeReplaceMaskSourceBatch(const NodeId& node_id, const MaskId& mask_id,
                                              nlohmann::json before_source,
                                              nlohmann::json after_source) -> PipelineEditBatch;

[[nodiscard]] auto MakeReplaceMaskAssetBatch(const NodeId& node_id, const MaskId& mask_id,
                                             nlohmann::json before_source,
                                             nlohmann::json after_source) -> PipelineEditBatch;

[[nodiscard]] auto MakeSetMaskFieldBatch(const NodeId& node_id, const MaskId& mask_id,
                                         std::string field_key, nlohmann::json before_value,
                                         nlohmann::json after_value) -> PipelineEditBatch;

/**
 * @brief Build one typed Paste batch from ordered document changes.
 *
 * @p changes may mix Grade, Mask, and parameter variants. Validate still requires
 * a non-empty list. Presentation uses @ref PresentationKeyForOperation for Paste.
 */
[[nodiscard]] auto MakePasteBatch(std::vector<PipelineEditChange> changes) -> PipelineEditBatch;

/**
 * @brief Append one typed-batch journal record and publish the commit and head.
 *
 * Does not mutate the document. Callers apply or inverse-apply around this.
 */
auto PublishTypedPipelineEdit(MiniGitWorkingHistory& history, const PipelineEditBatch& batch)
    -> MiniGitEditAppendResult;

}  // namespace alcedo
