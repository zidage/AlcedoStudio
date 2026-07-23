//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "app/editor_adjustment_types.hpp"
#include "app/editor_render_intent.hpp"
#include "app/editor_session_ports.hpp"
#include "app/editor_session_types.hpp"
#include "type/type.hpp"

namespace alcedo {

class EditTransaction;
class Hash128;

/// Outcome of an edit operation. The facade maps this to an EditorSessionResult
/// and passes the render command to the render controller.
struct EditorEditOutcome {
  enum class Kind : std::uint8_t {
    Accepted,
    RenderRouted,
    Failed,
    Rejected,
  };
  Kind                    kind      = Kind::Accepted;
  EditorSessionIdentity   identity{};
  EditorRenderReason       reason    = EditorRenderReason::InteractiveAdjustment;
  EditorRenderCommand      render_command;
  std::string              message;
};

/// Owns the adjustment snapshot and provisional/settled edit state. Receives
/// the active history guard as a value handle from the facade; does not own
/// session identity or guards.
class EditorSessionEditController final {
 public:
  struct Dependencies {
    std::shared_ptr<IEditorHistoryPort> history;
    std::shared_ptr<IEditorJournalPort> journal;
  };

  explicit EditorSessionEditController(Dependencies dependencies);

  /// Apply an interactive (settled=false) or settled (settled=true) adjustment
  /// patch. Captures the before-preview state, commits settled patches to
  /// history, updates the adjustment snapshot, and returns a render command
  /// for the facade to submit.
  auto HandlePatch(EditorAdjustmentPatch patch, bool settled,
                   const EditorHistoryGuardHandle& guard,
                   const EditorSessionIdentity& identity) -> EditorEditOutcome;

  /// Undo or redo the last history operation. Reads the snapshot from history
  /// and returns a render command.
  auto HandleUndoRedo(bool undo, const EditorHistoryGuardHandle& guard,
                      const EditorSessionIdentity& identity) -> EditorEditOutcome;

  /// Discard unflushed edits and restore the history snapshot. Returns a render
  /// command with Retry (from Failed) or SettledAdjustment (from Interactive).
  auto HandleDiscard(const EditorHistoryGuardHandle& guard,
                     const EditorSessionIdentity& identity,
                     EditorSessionState current_state) -> EditorEditOutcome;

  /// Record a finalized edit transaction to the journal.
  auto RecordFinalizedEdit(const EditTransaction& transaction,
                           const EditorSessionIdentity& identity, std::string* error) -> bool;

  /// Record a history cursor move to the journal.
  auto RecordHistoryCursorMove(std::uint64_t from_cursor, std::uint64_t to_cursor,
                               const EditorSessionIdentity& identity, std::string* error) -> bool;

  /// Record a timeline rewrite to the journal.
  auto RecordTimelineRewrite(const Hash128& expected_timeline_hash,
                             const Hash128& discarded_tail_hash,
                             std::uint64_t  retained_cursor,
                             const EditTransaction& replacement,
                             const EditorSessionIdentity& identity, std::string* error) -> bool;

  /// Read-only snapshot of the current adjustment state.
  [[nodiscard]] auto adjustment_snapshot() const -> EditorRenderAdjustmentSnapshot;

  /// Set the adjustment snapshot from an external source (e.g. history read
  /// during open/switch). Used by the navigation controller when loading a new
  /// image.
  void set_adjustment_snapshot(EditorRenderAdjustmentSnapshot snapshot);

  /// Clear the adjustment snapshot. Used when switching images or closing.
  void ClearSnapshot();

 private:
  struct Dependencies deps_;
  mutable std::mutex  mutex_;
  EditorRenderAdjustmentSnapshot adjustment_snapshot_{};
};

}  // namespace alcedo