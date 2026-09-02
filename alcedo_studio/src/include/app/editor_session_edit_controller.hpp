//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "app/editor_adjustment_types.hpp"
#include "app/editor_render_intent.hpp"
#include "app/editor_session_ports.hpp"
#include "app/editor_session_types.hpp"
#include "type/type.hpp"

namespace alcedo {

/// Outcome of an edit operation. The facade maps this to an EditorSessionResult
/// and passes the render command to the render controller.
struct EditorEditOutcome {
  enum class Kind : std::uint8_t {
    Accepted,
    RenderRouted,
    Failed,
    Rejected,
  };
  Kind                    kind            = Kind::Accepted;
  EditorSessionIdentity   identity{};
  EditorRenderReason      reason          = EditorRenderReason::InteractiveAdjustment;
  bool                    schedule_render = true;
  EditorRenderCommand     render_command;
  std::string             message;
};

/// Owns provisional/settled edit routing. All methods run during queue
/// reduction; the type deliberately has no mutation mutex and never calls a live
/// pipeline executor. Committed adjustment state lives in history and the live
/// pipeline — not in this controller.
class EditorSessionEditController final {
 public:
  struct Dependencies {
    std::shared_ptr<IEditorHistoryPort> history;
    std::shared_ptr<IEditorJournalPort> journal;
  };

  explicit EditorSessionEditController(Dependencies dependencies);

  /// Apply an interactive (settled=false) or settled (settled=true) adjustment
  /// patch. Unspecified current-panel targets are completed from the live
  /// document in history. Explicit incomplete targets are rejected. Captures the
  /// before-preview state, commits settled patches to history, and returns a
  /// render command carrying only the current field patch.
  auto HandlePatch(EditorAdjustmentPatch patch, bool settled,
                   const EditorHistoryGuardHandle& guard,
                   const EditorSessionIdentity& identity) -> EditorEditOutcome;

  /// Undo or redo the last history operation. History rebuilds the live pipeline;
  /// the render command carries no adjustment replay.
  auto HandleUndoRedo(bool undo, const EditorHistoryGuardHandle& guard,
                      const EditorSessionIdentity& identity) -> EditorEditOutcome;

  /// Move the working head to an explicit commit in one operation (Phase 7A
  /// P1). The history port applies the traversed before/after deltas; the render
  /// command carries no adjustment replay.
  auto HandleMoveHeadToCommit(const commit_hash_t& target, const EditorHistoryGuardHandle& guard,
                              const EditorSessionIdentity& identity) -> EditorEditOutcome;

  /// Discard unflushed edits. Returns a render command with Retry (from Failed)
  /// or SettledAdjustment (from Interactive); adjustment replay is empty.
  auto HandleDiscard(const EditorHistoryGuardHandle& guard,
                     const EditorSessionIdentity& identity,
                     EditorSessionState current_state) -> EditorEditOutcome;

 private:
  struct Dependencies deps_;
};

}  // namespace alcedo
