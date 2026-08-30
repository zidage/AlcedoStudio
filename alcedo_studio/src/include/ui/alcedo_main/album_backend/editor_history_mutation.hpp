//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <memory>
#include <string>

#include "app/editor_session_ports.hpp"
#include "app/editor_session_types.hpp"

namespace alcedo::ui {

struct HistoryWorkingState;
class EditorHistoryState;

/// Extracted mutation/navigation unit. Handles adjustment capture, settled
/// commit, Undo, Redo, explicit head movement, and Version checkout. Preserves
/// the existing render and revision publication call chain.
class EditorHistoryMutation {
 public:
  explicit EditorHistoryMutation(EditorHistoryState& state);

  /// Capture the committed operator value before interactive preview begins.
  /// @pre @p patch.target is a complete production target. Incomplete targets are
  ///      rejected; this path does not fill owner_kind or node_id.
  auto CaptureAdjustmentBeforePreview(const alcedo::EditorHistoryGuardHandle& guard,
                                      const alcedo::EditorAdjustmentPatch& patch,
                                      std::string* error) -> bool;

  /// Append one settled adjustment and advance the live working head.
  /// Live effect writes PipelineGuard::document_ using the locked target.
  auto CommitAdjustment(const alcedo::EditorHistoryGuardHandle& guard,
                        const alcedo::EditorAdjustmentPatch& patch, std::string* error) -> bool;

  /// Move the working head to its first parent and apply the before value.
  auto Undo(const alcedo::EditorHistoryGuardHandle& guard, std::string* error) -> bool;

  /// Move the working head to the redo child and apply the after value.
  auto Redo(const alcedo::EditorHistoryGuardHandle& guard, std::string* error) -> bool;

  /// Move the working head to an explicit commit in one operation.
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
