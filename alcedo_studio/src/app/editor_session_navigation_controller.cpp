//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_session_navigation_controller.hpp"

#include <utility>

#include "app/editor_session_edit_controller.hpp"
#include "app/editor_session_lifecycle.hpp"
#include "app/editor_session_render_controller.hpp"

namespace alcedo {

EditorSessionNavigationController::EditorSessionNavigationController(
    EditorSessionLifecycle& lifecycle, EditorSaveCheckpointService& save_service,
    EditorSessionRenderController& render, EditorSessionEditController& edit,
    IEditorJournalPort* journal, IEditorCheckpointStore* checkpoint_store,
    IEditorHistoryPort* history)
    : lifecycle_(lifecycle),
      save_service_(save_service),
      render_(render),
      edit_(edit),
      journal_(journal),
      checkpoint_store_(checkpoint_store),
      history_(history) {}

auto EditorSessionNavigationController::RequestOpenOrSwitch(sl_element_id_t element_id,
                                                            image_id_t image_id, bool is_switch)
    -> NavigationOutcome {
  std::scoped_lock  lock(mutex_);
  NavigationOutcome outcome;

  if (lifecycle_.state() == EditorSessionState::ShuttingDown) {
    outcome.rejected = true;
    outcome.message  = "Cannot open while shutting down";
    return outcome;
  }

  if (pending_action_.has_value()) {
    outcome.rejected = true;
    outcome.message  = "Editor save checkpoint is in progress";
    return outcome;
  }

  const auto current_identity = lifecycle_.identity();
  const auto current_state    = lifecycle_.state();

  // Same image already open: no-op.
  if (current_identity.element_id == element_id && current_identity.image_id == image_id &&
      (current_state == EditorSessionState::Loading ||
       current_state == EditorSessionState::Interactive ||
       current_state == EditorSessionState::Acquiring ||
       current_state == EditorSessionState::Switching ||
       current_state == EditorSessionState::Saving)) {
    outcome.completed_synchronously = true;
    outcome.same_image_noop         = true;
    outcome.message                 = "Image already open";
    return outcome;
  }

  // Seal the prior image before acquiring the next one.
  if (current_identity.session_generation != 0 &&
      (current_identity.element_id != 0 || current_identity.image_id != 0)) {
    pending_action_   = PendingEditorAction{PendingEditorActionKind::SwitchImage,
                                          element_id,
                                          image_id,
                                          is_switch,
                                          true,
                                          CheckpointTicket{}};
    const auto ticket = SealAndStartSave(true, true);
    if (!ticket.valid()) {
      pending_action_.reset();
      outcome.failed  = true;
      outcome.message = "Failed to save current image";
      return outcome;
    }
    // If pending_action_ was cleared during SealAndStartSave, the
    // OnCheckpointFinished callback ran synchronously inside
    // SaveCheckpointService::Start. The save completed immediately.
    if (!pending_action_.has_value()) {
      outcome.ticket                    = ticket;
      outcome.sealed_session_generation = current_identity.session_generation;
    } else {
      pending_action_->ticket           = ticket;
      outcome.ticket                    = ticket;
      outcome.sealed_session_generation = current_identity.session_generation;
      outcome.waiting_for_checkpoint    = true;
      outcome.message = "Waiting for save checkpoint before loading the next image";
      return outcome;
    }
    // Synchronous save: OnCheckpointFinished already released guards.
    outcome.completed_synchronously = true;
    outcome.message                 = "Switched to next image";
    return outcome;
  }

  // No prior image: open directly.
  ContinueToTarget(element_id, image_id, is_switch);
  outcome.completed_synchronously = true;
  outcome.message                 = "Opened image";
  return outcome;
}

auto EditorSessionNavigationController::RequestClose(bool persist_changes) -> NavigationOutcome {
  std::scoped_lock  lock(mutex_);
  NavigationOutcome outcome;

  if (lifecycle_.state() == EditorSessionState::ShuttingDown) {
    outcome.rejected = true;
    outcome.message  = "Cannot close while shutting down";
    return outcome;
  }

  if (pending_action_.has_value()) {
    outcome.rejected = true;
    outcome.message  = "Editor save checkpoint is in progress";
    return outcome;
  }

  const auto current_identity = lifecycle_.identity();
  if (persist_changes && current_identity.element_id != 0 && current_identity.image_id != 0) {
    pending_action_ = PendingEditorAction{
        PendingEditorActionKind::CloseEditor, 0, 0, false, true, CheckpointTicket{}};
    const auto ticket = SealAndStartSave(true, true);
    if (!ticket.valid()) {
      pending_action_.reset();
      outcome.failed  = true;
      outcome.message = "Failed to close editor session";
      return outcome;
    }
    if (!pending_action_.has_value()) {
      outcome.ticket                    = ticket;
      outcome.sealed_session_generation = current_identity.session_generation;
    } else {
      pending_action_->ticket           = ticket;
      outcome.ticket                    = ticket;
      outcome.sealed_session_generation = current_identity.session_generation;
      outcome.waiting_for_checkpoint    = true;
      outcome.message                   = "Waiting for save checkpoint before closing";
      return outcome;
    }
    // Synchronous save: OnCheckpointFinished already released guards.
    outcome.completed_synchronously = true;
    outcome.message                 = "Editor session closed";
    return outcome;
  }

  // Discard or no-image close: finalize discard and close immediately.
  if (!persist_changes && journal_ != nullptr) {
    std::string error;
    journal_->DiscardUnflushed(current_identity.element_id, &error);
  }
  render_.CancelSessionAndWait(current_identity.session_generation);
  lifecycle_.ReleaseGuards();
  lifecycle_.CompleteClose();
  edit_.ClearSnapshot();
  render_.ResetForNewImage();
  outcome.completed_synchronously = true;
  outcome.message = persist_changes ? "Editor session closed" : "Editor changes discarded";
  return outcome;
}

void EditorSessionNavigationController::OnCheckpointFinished(const SaveCheckpointResult& result) {
  std::scoped_lock lock(mutex_);
  if (!pending_action_.has_value()) {
    return;
  }
  const auto pending = *pending_action_;
  // When the ticket has not been set yet (request_id == 0), the completion
  // callback fired synchronously inside SaveCheckpointService::Start before
  // the caller obtained the ticket. Accept the result unconditionally.
  // Otherwise, correlate by request_id and session_generation to reject
  // stale completions from earlier saves.
  if (pending.ticket.request_id != 0 &&
      (pending.ticket.request_id != result.request_id ||
       pending.ticket.session_generation != result.session_generation)) {
    return;
  }
  pending_action_.reset();

  if (!result.checkpoint_completed) {
    lifecycle_.KeepCurrentAfterCheckpointFailure(result.error.empty() ? "Save checkpoint failed"
                                                                      : result.error);
    return;
  }

  lifecycle_.ReleaseAfterCheckpoint();

  if (pending.kind == PendingEditorActionKind::CloseEditor) {
    ContinueToClose(pending.persist);
    return;
  }

  ContinueToTarget(pending.element_id, pending.image_id, pending.is_switch);
}

auto EditorSessionNavigationController::has_pending_action() const -> bool {
  std::scoped_lock lock(mutex_);
  return pending_action_.has_value();
}

void EditorSessionNavigationController::ClearPendingAction() {
  std::scoped_lock lock(mutex_);
  pending_action_.reset();
}

auto EditorSessionNavigationController::SealAndStartSave(bool persist_changes,
                                                         bool start_background_save)
    -> CheckpointTicket {
  const auto identity = lifecycle_.identity();
  render_.CancelSessionAndWait(identity.session_generation);

  if (persist_changes) {
    if (journal_ != nullptr) {
      std::string error;
      if (!journal_->FinalizeEdit(identity.element_id, identity.session_generation, &error)) {
        return CheckpointTicket{};
      }
    }
    if (history_ != nullptr && lifecycle_.has_history_guard()) {
      std::string error;
      auto        capture = history_->CaptureSaveCheckpoint(lifecycle_.history_guard(), &error);
      if (!error.empty()) {
        return CheckpointTicket{};
      }
      SaveCheckpointRequest req;
      req.element_id         = identity.element_id;
      req.session_generation = identity.session_generation;
      req.capture            = std::move(capture);
      lifecycle_.BeginCheckpoint();
      if (!start_background_save) {
        return CheckpointTicket{};
      }
      return save_service_.Start(
          std::move(req), [this](const SaveCheckpointResult& r) { OnCheckpointFinished(r); });
    }
    if (start_background_save) {
      SaveCheckpointRequest req;
      req.element_id         = identity.element_id;
      req.session_generation = identity.session_generation;
      lifecycle_.BeginCheckpoint();
      return save_service_.Start(
          req, [this](const SaveCheckpointResult& r) { OnCheckpointFinished(r); });
    }
  } else if (journal_ != nullptr) {
    std::string error;
    if (!journal_->DiscardUnflushed(identity.element_id, &error)) {
      return CheckpointTicket{};
    }
  }
  return CheckpointTicket{};
}

void EditorSessionNavigationController::ContinueToTarget(sl_element_id_t element_id,
                                                         image_id_t image_id, bool is_switch) {
  std::string error;
  if (!lifecycle_.BeginAcquire(element_id, image_id, is_switch, checkpoint_store_, &error)) {
    lifecycle_.Fail(error);
    return;
  }
  if (!lifecycle_.AcquireGuards(&error)) {
    lifecycle_.Fail(error);
    return;
  }

  EditorRenderAdjustmentSnapshot history_snapshot;
  if (history_ != nullptr &&
      !history_->ReadAdjustmentSnapshot(lifecycle_.history_guard(), &history_snapshot, &error)) {
    lifecycle_.ReleaseGuards();
    lifecycle_.Fail(error);
    return;
  }
  if (!history_snapshot.params_json.empty() || !history_snapshot.patches.empty() ||
      !history_snapshot.fingerprint.empty()) {
    edit_.set_adjustment_snapshot(std::move(history_snapshot));
  } else {
    edit_.ClearSnapshot();
  }

  lifecycle_.MarkImageReady();
  render_.ResetForNewImage();
  render_.MarkImageAcquired();

  EditorRenderCommand command;
  command.reason = is_switch ? EditorRenderReason::ImageSwitch : EditorRenderReason::InitialFrame;
  command.adjustment = edit_.adjustment_snapshot();
  render_.RouteInitialRender(command, lifecycle_.identity());
}

void EditorSessionNavigationController::ContinueToClose(bool persist_changes) {
  lifecycle_.CompleteClose();
  edit_.ClearSnapshot();
  render_.ResetForNewImage();
  (void)persist_changes;
}

}  // namespace alcedo
