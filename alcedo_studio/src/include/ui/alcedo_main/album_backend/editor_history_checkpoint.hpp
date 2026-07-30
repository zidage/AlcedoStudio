//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "app/editor_session_ports.hpp"

namespace alcedo {
struct EditorMiniGitSaveCapture;
}  // namespace alcedo

namespace alcedo::ui {

struct HistoryWorkingState;
class EditorHistoryState;

/// Extracted checkpoint unit. Handles save-capture and journal truncation.
class EditorHistoryCheckpoint {
 public:
  explicit EditorHistoryCheckpoint(EditorHistoryState& state);

  /// Return an immutable save capture with all records needed by the store.
  auto CaptureSaveCheckpoint(const alcedo::EditorHistoryGuardHandle& guard, std::string* error)
      -> std::shared_ptr<const alcedo::EditorMiniGitSaveCapture>;

  /// Truncate the live Mini-Git journal through last_sequence after a successful
  /// materialize so same-session captures no longer include that prefix.
  auto DiscardMaterializedJournalThrough(const alcedo::EditorHistoryGuardHandle& guard,
                                         std::uint64_t last_sequence, std::string* error) -> bool;

  /// Reconcile the in-memory ImageEditState.materialized_* with the durable tuple
  /// a successful checkpoint just wrote to DuckDB. Advances the in-memory
  /// materialized head/chain to the active Version's working head so a subsequent
  /// PersistEditorHistoryState guard accepts the durable state.
  auto SyncMaterializedStateAfterCheckpoint(const alcedo::EditorHistoryGuardHandle& guard,
                                             std::string* error) -> bool;

 private:
  EditorHistoryState& state_;
};

}  // namespace alcedo::ui
