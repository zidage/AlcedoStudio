//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "app/editor_session_ports.hpp"
#include "app/editor_session_types.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/mask/mask_id.hpp"
#include "edit/mask/mask_model.hpp"
#include "json.hpp"

namespace alcedo {
class MaskStore;
}

namespace alcedo::ui {

struct HistoryWorkingState;
class EditorHistoryState;

/// Extracted mutation/navigation unit. Handles adjustment capture, settled
/// commit, graph and Mask commands, Undo, Redo, explicit head movement, and
/// Version checkout. Parameter operations run on the history queue and hold the
/// executor render lock across document access and WAL publication. They never
/// copy the document or update stage params.
class EditorHistoryMutation {
 public:
  explicit EditorHistoryMutation(EditorHistoryState& state);

  /// Capture the target Model value once and apply preview under the render lock.
  /// Unspecified current-panel targets are completed from the live document.
  /// Explicit incomplete targets are rejected.
  auto CaptureAdjustmentBeforePreview(const alcedo::EditorHistoryGuardHandle& guard,
                                      const alcedo::EditorAdjustmentPatch& patch,
                                      std::string* error) -> bool;

  /// Append one settled typed-batch adjustment and advance the live working head.
  /// The live document already holds after values from preview. WAL failure restores
  /// the locked target. Unchanged normalized values produce neither a commit nor a
  /// WAL record.
  auto CommitAdjustment(const alcedo::EditorHistoryGuardHandle& guard,
                        const alcedo::EditorAdjustmentPatch& patch, std::string* error) -> bool;

  /// Apply @p batch to the live document, then append one typed commit.
  /// WAL failure inverse-applies the batch. Used by graph/Mask commands and tests.
  auto CommitPipelineEditBatch(const alcedo::EditorHistoryGuardHandle& guard,
                               alcedo::PipelineEditBatch batch, std::string* error) -> bool;

  auto AddColorGrade(const alcedo::EditorHistoryGuardHandle& guard,
                     const alcedo::NodeId& before_node_id, const alcedo::NodeId& new_id,
                     std::string* error) -> bool;
  auto RemoveColorGrade(const alcedo::EditorHistoryGuardHandle& guard, const alcedo::NodeId& node_id,
                        std::string* error) -> bool;
  auto ReconnectColorGrade(const alcedo::EditorHistoryGuardHandle& guard, const alcedo::NodeId& node_id,
                           const alcedo::NodeId& new_predecessor_id,
                           const alcedo::NodeId& new_successor_id, std::string* error) -> bool;
  auto RenameColorGrade(const alcedo::EditorHistoryGuardHandle& guard, const alcedo::NodeId& node_id,
                        std::string display_name, std::string* error) -> bool;
  auto SetColorGradeEnabled(const alcedo::EditorHistoryGuardHandle& guard, const alcedo::NodeId& node_id,
                            bool enabled, std::string* error) -> bool;
  auto SetColorGradeMix(const alcedo::EditorHistoryGuardHandle& guard, const alcedo::NodeId& node_id,
                        float mix, std::string* error) -> bool;
  auto AddMask(const alcedo::EditorHistoryGuardHandle& guard, const alcedo::NodeId& node_id,
               alcedo::MaskModel mask, std::uint32_t display_index, std::string* error) -> bool;
  auto RemoveMask(const alcedo::EditorHistoryGuardHandle& guard, const alcedo::NodeId& node_id,
                  const alcedo::MaskId& mask_id, std::string* error) -> bool;
  auto ReplaceMaskSource(const alcedo::EditorHistoryGuardHandle& guard, const alcedo::NodeId& node_id,
                         const alcedo::MaskId& mask_id, nlohmann::json after_source,
                         std::string* error) -> bool;
  auto ReplaceMaskAsset(const alcedo::EditorHistoryGuardHandle& guard, const alcedo::NodeId& node_id,
                        const alcedo::MaskId& mask_id, nlohmann::json after_source,
                        alcedo::MaskStore& mask_store, std::string* error) -> bool;
  auto SetMaskField(const alcedo::EditorHistoryGuardHandle& guard, const alcedo::NodeId& node_id,
                    const alcedo::MaskId& mask_id, std::string field_key, nlohmann::json after_value,
                    std::string* error) -> bool;

  /// Move the working head to its first parent and inverse-apply stored batches.
  auto Undo(const alcedo::EditorHistoryGuardHandle& guard, std::string* error) -> bool;

  /// Move the working head to the redo child and forward-apply stored batches.
  auto Redo(const alcedo::EditorHistoryGuardHandle& guard, std::string* error) -> bool;

  /// Move the working head to an explicit commit in one operation.
  /// Typed batches apply from their stored payload. Ordinary commits still require
  /// a same-session document target.
  auto MoveHeadToCommit(const alcedo::EditorHistoryGuardHandle& guard,
                        const alcedo::commit_hash_t& commit_id, std::string* error) -> bool;

  /// Restore the active working state to the last materialized head and remove
  /// its Mini-Git journal records.
  auto DiscardUnmaterializedChanges(const alcedo::EditorHistoryGuardHandle& guard,
                                    std::string* error) -> bool;

  /// Switch the checked-out Version, rebuild the live pipeline, refresh the snapshot.
  auto CheckoutVersion(const alcedo::EditorHistoryGuardHandle& guard,
                       const alcedo::Hash128& version_id, std::string* error) -> bool;

 private:
  EditorHistoryState& state_;
};

}  // namespace alcedo::ui
