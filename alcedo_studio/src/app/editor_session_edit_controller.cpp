//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_session_edit_controller.hpp"

#include <algorithm>
#include <utility>

#include "edit/history/edit_transaction.hpp"

namespace alcedo {

EditorSessionEditController::EditorSessionEditController(Dependencies dependencies)
    : deps_(std::move(dependencies)) {}

auto EditorSessionEditController::HandlePatch(EditorAdjustmentPatch patch, bool settled,
                                              const EditorHistoryGuardHandle& guard,
                                              const EditorSessionIdentity&    identity)
    -> EditorEditOutcome {
  EditorEditOutcome outcome;
  outcome.identity = identity;

  patch.settled = settled;
  if (patch.field_key.empty()) {
    outcome.kind    = EditorEditOutcome::Kind::Rejected;
    outcome.message = "Adjustment patch requires a field key";
    return outcome;
  }
  if (!deps_.history || !guard.valid) {
    outcome.kind    = EditorEditOutcome::Kind::Rejected;
    outcome.message = "Adjustment history is unavailable";
    return outcome;
  }
  std::string history_error;
  if (!deps_.history->CaptureAdjustmentBeforePreview(guard, patch, &history_error)) {
    outcome.kind    = EditorEditOutcome::Kind::Rejected;
    outcome.message = history_error.empty() ? "Failed to capture committed adjustment state"
                                             : history_error;
    return outcome;
  }
  if (settled && !deps_.history->CommitAdjustment(guard, patch, &history_error)) {
    outcome.kind    = EditorEditOutcome::Kind::Rejected;
    outcome.message = history_error.empty() ? "Failed to commit settled adjustment" : history_error;
    return outcome;
  }

  ++adjustment_snapshot_.snapshot_generation;
  const auto existing = std::find_if(
      adjustment_snapshot_.patches.begin(), adjustment_snapshot_.patches.end(),
      [&](const EditorAdjustmentPatch& current) { return current.field_key == patch.field_key; });
  if (existing == adjustment_snapshot_.patches.end()) {
    adjustment_snapshot_.patches.push_back(patch);
  } else {
    *existing = patch;
  }
  if (!patch.params_json.empty()) {
    adjustment_snapshot_.params_json = patch.params_json;
  }
  adjustment_snapshot_.fingerprint.clear();
  for (const auto& current : adjustment_snapshot_.patches) {
    if (!adjustment_snapshot_.fingerprint.empty()) {
      adjustment_snapshot_.fingerprint += "|";
    }
    adjustment_snapshot_.fingerprint += current.field_key;
  }

  outcome.kind          = EditorEditOutcome::Kind::RenderRouted;
  outcome.reason        = settled ? EditorRenderReason::SettledAdjustment
                                  : EditorRenderReason::InteractiveAdjustment;
  outcome.render_command.reason     = outcome.reason;
  outcome.render_command.adjustment = adjustment_snapshot_;
  outcome.render_command.policy     = EditorRenderSupersessionPolicy::PreserveInflightFullFrame;
  return outcome;
}

auto EditorSessionEditController::HandleUndoRedo(bool undo,
                                                 const EditorHistoryGuardHandle& guard,
                                                 const EditorSessionIdentity&    identity)
    -> EditorEditOutcome {
  EditorEditOutcome outcome;
  outcome.identity = identity;

  if (!deps_.history || !guard.valid) {
    outcome.kind    = EditorEditOutcome::Kind::Rejected;
    outcome.message = "History port unavailable";
    return outcome;
  }
  std::string error;
  const bool  ok = undo ? deps_.history->Undo(guard, &error)
                        : deps_.history->Redo(guard, &error);
  if (!ok) {
    outcome.kind    = EditorEditOutcome::Kind::Failed;
    outcome.message = error.empty() ? (undo ? "Undo failed" : "Redo failed") : error;
    return outcome;
  }
  EditorRenderAdjustmentSnapshot snapshot;
  if (!deps_.history->ReadAdjustmentSnapshot(guard, &snapshot, &error)) {
    outcome.kind    = EditorEditOutcome::Kind::Failed;
    outcome.message = error.empty() ? "Failed to read history adjustment state" : error;
    return outcome;
  }
  adjustment_snapshot_ = std::move(snapshot);

  outcome.kind          = EditorEditOutcome::Kind::Accepted;
  outcome.reason        = EditorRenderReason::UndoRedo;
  outcome.message       = undo ? "Undo applied" : "Redo applied";
  outcome.render_command.reason     = EditorRenderReason::UndoRedo;
  outcome.render_command.adjustment = adjustment_snapshot_;
  return outcome;
}

auto EditorSessionEditController::HandleMoveHeadToCommit(const commit_hash_t& target,
                                                         const EditorHistoryGuardHandle& guard,
                                                         const EditorSessionIdentity& identity)
    -> EditorEditOutcome {
  EditorEditOutcome outcome;
  outcome.identity = identity;

  if (!deps_.history || !guard.valid) {
    outcome.kind    = EditorEditOutcome::Kind::Rejected;
    outcome.message = "History port unavailable";
    return outcome;
  }
  std::string error;
  if (!deps_.history->MoveHeadToCommit(guard, target, &error)) {
    outcome.kind    = EditorEditOutcome::Kind::Failed;
    outcome.message = error.empty() ? "Editor head move failed" : error;
    return outcome;
  }
  EditorRenderAdjustmentSnapshot snapshot;
  if (!deps_.history->ReadAdjustmentSnapshot(guard, &snapshot, &error)) {
    outcome.kind    = EditorEditOutcome::Kind::Failed;
    outcome.message = error.empty() ? "Failed to read history adjustment state" : error;
    return outcome;
  }
  adjustment_snapshot_ = std::move(snapshot);

  outcome.kind                      = EditorEditOutcome::Kind::Accepted;
  outcome.reason                    = EditorRenderReason::UndoRedo;
  outcome.message                   = "Editor head moved";
  outcome.render_command.reason     = EditorRenderReason::UndoRedo;
  outcome.render_command.adjustment = adjustment_snapshot_;
  return outcome;
}

auto EditorSessionEditController::HandleDiscard(const EditorHistoryGuardHandle& guard,
                                                const EditorSessionIdentity&    identity,
                                                EditorSessionState              current_state)
    -> EditorEditOutcome {
  EditorEditOutcome outcome;
  outcome.identity = identity;

  if (!guard.valid) {
    outcome.kind    = EditorEditOutcome::Kind::Rejected;
    outcome.message = "Discard requires an image with an active history session";
    return outcome;
  }
  if (deps_.journal) {
    std::string error;
    if (!deps_.journal->DiscardUnflushed(identity.element_id, &error)) {
      outcome.kind    = EditorEditOutcome::Kind::Failed;
      outcome.message = error.empty() ? "Discard failed" : error;
      return outcome;
    }
  }
  std::string                    error;
  if (deps_.history && !deps_.history->DiscardUnmaterializedChanges(guard, &error)) {
    outcome.kind    = EditorEditOutcome::Kind::Failed;
    outcome.message = error.empty() ? "Discard failed" : error;
    return outcome;
  }
  EditorRenderAdjustmentSnapshot snapshot;
  if (!deps_.history ||
      !deps_.history->ReadAdjustmentSnapshot(guard, &snapshot, &error)) {
    outcome.kind    = EditorEditOutcome::Kind::Failed;
    outcome.message = error.empty() ? "Failed to restore discarded adjustment state" : error;
    return outcome;
  }
  adjustment_snapshot_ = std::move(snapshot);

  if (current_state == EditorSessionState::Failed) {
    outcome.kind          = EditorEditOutcome::Kind::RenderRouted;
    outcome.reason        = EditorRenderReason::Retry;
    outcome.message       = "Retrying after discard";
    outcome.render_command.reason     = EditorRenderReason::Retry;
  } else {
    outcome.kind          = EditorEditOutcome::Kind::Accepted;
    outcome.reason        = EditorRenderReason::SettledAdjustment;
    outcome.message       = "Discarded unflushed transaction";
    outcome.render_command.reason     = EditorRenderReason::SettledAdjustment;
  }
  outcome.render_command.adjustment = adjustment_snapshot_;
  return outcome;
}

auto EditorSessionEditController::adjustment_snapshot() const
    -> EditorRenderAdjustmentSnapshot {
  return adjustment_snapshot_;
}

void EditorSessionEditController::set_adjustment_snapshot(
    EditorRenderAdjustmentSnapshot snapshot) {
  adjustment_snapshot_ = std::move(snapshot);
}

void EditorSessionEditController::ClearSnapshot() {
  adjustment_snapshot_ = {};
}

}  // namespace alcedo
